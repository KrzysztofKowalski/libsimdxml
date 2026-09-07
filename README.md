# libsimdxml

SIMD-accelerated XML parser with full XPath 1.0 for **C++23** — single-pass
indexing, zero-copy by design, runtime SIMD dispatch.

## Why

- **One binary, every CPU.** AVX2, SSE4.2, NEON and scalar backends are all
  compiled into the library and picked at run time — no `-mavx2` build flags,
  no fat binaries, no recompiles per deployment target. Structural character
  classification runs 32 bytes per instruction (PSHUFB nibble classification,
  the simdjson technique).
- **Zero copy.** The index is a structure-of-arrays of `(offset, length)`
  ranges over the input buffer. Tag names, text and attribute values are
  `string_view`s into the original bytes — nothing is allocated per node.
- **Exact allocation.** Structural counts come from SIMD popcounts, not
  `size/128` guesses — result vectors are reserved once, so large documents
  don't pay reallocation storms.
- **Vectorized tree construction.** Parent/child CSR indices are built with an
  AVX2 prefix-sum; ancestor queries are a single comparison via a post-order
  array.
- **Full XPath 1.0** — compile once, evaluate many times, over the index.

## Benchmarks

### Test data

All inputs come from the same deterministic generator (fixed seed), so runs
are reproducible:

| File              | Size   | What's inside                                                                                                                                                            |
|-------------------|--------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `sample_1mb.xml`  | ~1 MB  | synthetic XML: 16,824 elements, nesting depth up to 22, attributes on most elements, XML entities, CDATA sections, comments, processing instructions                     |
| `sample_10mb.xml` | ~10 MB | the same profile scaled 10× (~168k elements)                                                                                                                             |
| `sample_1mb.html` | ~1 MB  | synthetic HTML with a realistic tag mix and ~9% unclosed `<li>` elements — robustness checks only (libsimdxml does not target HTML)                                      |

### Parse speed (MB/s)

Measured on an Intel Core i7-4850HQ (Haswell, 2013 laptop, 2.3 GHz), Clang 22
`-O3`, C++23. Medians over 100+ alternating A/B runs; cold runs discarded.

| Workload                                          | sample_1mb.xml         | sample_10mb.xml        |
|---------------------------------------------------|------------------------|------------------------|
| libsimdxml — parse (structural index)             | **351 MB/s** (2.99 ms) | **374 MB/s** (28.1 ms) |
| libsimdxml — full index (parse + CSR tree + name index) | 285 MB/s (3.69 ms) | 300 MB/s (34.9 ms)   |
| libsimdxml — structural classification only       | 4,753 MB/s             | 4,272 MB/s             |
| pugixml 1.16, in-situ (reference)                 | ~280 MB/s (3.77 ms)    | ~275 MB/s (38.2 ms)    |

Speedup of libsimdxml over pugixml 1.16: **parse 1.26× (1 MB) / 1.36×
(10 MB)**; **full index 1.02× / 1.10×**; classification-only 16–17×;
attribute queries 1.05× / 1.14×.

### Cross-language context

Same `sample_1mb.xml` file, same machine, medians of the same benchmark
harness (JVM rows via a Docker JVM):

| Parser         | Language | Parse time  | Throughput    |
|----------------|----------|-------------|---------------|
| **libsimdxml** | C++      | **2.99 ms** | **~350 MB/s** |
| pugixml 1.16   | C++      | 3.4 ms      | ~310 MB/s     |
| lxml           | Python   | 22.6 ms     | ~46 MB/s      |
| cheerio        | Node.js  | 71 ms       | ~15 MB/s      |
| sax            | Node.js  | 93 ms       | ~11 MB/s      |
| Xerces (DOM)   | Java     | 124 ms      | ~8.5 MB/s     |
| jsoup          | Java     | 179 ms      | ~5.9 MB/s     |
| dom4j          | Java     | 189 ms      | ~5.6 MB/s     |
| html.parser    | Python   | 1050 ms     | ~1.0 MB/s     |

**Runtime dispatch, measured in-binary** — structural classification on the
same 10 MB file: scalar 0.8 GB/s → AVX2 3.1 GB/s (3.8×) when dispatch was
wired up, 4.3 GB/s after tuning. One binary for every x86-64 host.

Numbers come from a 2013 laptop; the ratios, not the absolute values, are the
point.

## Usage

```cmake
add_subdirectory(libsimdxml)
target_link_libraries(my_app PRIVATE simdxml)
```

```cpp
#include <simdxml/simdxml.hpp>
#include <simdxml/index/xml_index.hpp>
#include <iostream>
#include <span>
#include <string>

int main() {
    std::string xml = R"(<?xml version="1.0"?>
<catalog>
  <book id="1"><title>XML in Practice</title></book>
  <book id="2"><title>SIMD for Everyone</title></book>
</catalog>)";

    auto bytes = std::as_bytes(std::span{xml.data(), xml.size()});

    // Parse: single pass, zero copy, runtime-dispatched SIMD.
    auto idx = simdxml::parse(bytes);
    if (!idx) {
        std::cerr << "parse error\n";
        return 1;
    }

    // Precompute tree indices (CSR children) and the name index.
    idx->ensure_indices();
    idx->build_name_index();

    std::cout << idx->tag_count() << " tags, max depth "
              << idx->max_depth() << "\n";

    // All <title> elements, in document order.
    for (auto t : idx->tags_by_name("title"))
        std::cout << idx->tag_name(t) << "\n";
}
```

XPath 1.0:

```cpp
#include <simdxml/xpath/xpath.hpp>

auto xp  = simdxml::CompiledXPath::compile("//book/title");
auto res = xp->eval(*idx);
if (res) {
    for (auto const& node : *res) { /* ... */ }
}
```

## Scope & status

- Lenient parser: does not validate XML well-formedness (same behavior as the
  original crate's fast path). Garbage in does not crash it, but the index of
  malformed documents is best-effort.
- Documents nesting deeper than 32 levels are indexed with degraded parent
  links (fixed-size parent stack in `index/structural.cpp`).
- The `.sxi` persistence format validates a hash and re-parses on mismatch,
  but is intended for trusted cache files, not hostile input.
- HTML is not a supported input.

## Credits & License

- Code: MIT License — © 2026 KrzysztofKowalski (see `LICENSE`).
- Derived from the Rust crate
  [simdxml](https://github.com/simdxml/simdxml) by Christopher Grainger
  (MIT OR Apache-2.0). The original copyright notice is retained in `NOTICE`
  as required by its license.