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
| `sample_5–30mb.xml` | 5–30 MB | the same profile scaled to the full benchmark matrix (5, 10, 15, 20, 30 MB)                                                                                              |
| `sample_1mb.html` | ~1 MB  | synthetic HTML with a realistic tag mix and ~9% unclosed `<li>` elements — robustness checks only (libsimdxml does not target HTML)                                      |

### Parse speed (MB/s)

Measured on an Intel Core i7-4850HQ (Haswell, 2013 laptop, 2.3 GHz), C++23.
Sequential benchmark orchestrator: fresh process per run, cold runs discarded,
one process at a time, thermal abort at 100 °C (no cooldown gating).

| Workload                                          | sample_1mb.xml          | sample_5mb.xml         | sample_10mb.xml        |
|---------------------------------------------------|-------------------------|------------------------|------------------------|
| libsimdxml — parse (structural index)             | **351 MB/s** (2.85 ms)  | **376 MB/s** (13.3 ms) | **405 MB/s** (24.7 ms) |
| pugixml 1.16, in-situ (reference)                 | 298 MB/s (3.36 ms)      | 322 MB/s (15.5 ms)     | 322 MB/s (31.1 ms)     |

Speedup of libsimdxml over pugixml 1.16: **1.18× (1 MB) → 1.26× (10 MB)**,
from the 13.09 rerun of the full cpp/python/node matrix. The earlier 12.09
run (cooldown-gated policy) measured 1.16×/1.12×/1.26× on the same sizes and
reached **1.40× at 30 MB** (414 MB/s) — the gap grows with input size as
fixed per-parse costs amortize. libsimdxml nodes/attributes counts are
bit-identical to pugixml and expat at every size.

Earlier in-process A/B harness (100+ alternating runs, pre-fusion code):
parse 351/374 MB/s (1.26×/1.36× vs pugixml), full index 285/300 MB/s
(1.02×/1.10×), structural classification only ~4.3–4.8 GB/s; runtime dispatch
measured in-binary: scalar 0.8 GB/s → AVX2 3.1–4.3 GB/s.

### Cross-language context

Same `sample_1mb.xml` file, same machine, medians of the same 13.09
orchestrator run (JVM rows from an earlier Docker-JVM run of the same harness):

| Parser             | Language | Parse time  | Throughput    |
|--------------------|----------|-------------|---------------|
| **libsimdxml**     | C++      | **2.85 ms** | **351 MB/s**  |
| pugixml 1.16       | C++      | 3.36 ms     | 298 MB/s      |
| expat (SAX)        | C++      | 10.7 ms     | 94 MB/s       |
| lxml               | Python   | 22.4 ms     | 45 MB/s       |
| libxml2 (DOM)      | C++      | 24.2 ms     | 41 MB/s       |
| lxml.html          | Python   | 34.1 ms     | 29 MB/s       |
| cheerio            | Node.js  | 68.9 ms     | 15 MB/s       |
| sax (Node)         | Node.js  | 89.9 ms     | 11 MB/s       |
| xml.etree          | Python   | 112.7 ms    | 9 MB/s        |
| Xerces (DOM)       | Java     | 124 ms      | ~8.5 MB/s     |
| jsoup              | Java     | 179 ms      | ~5.9 MB/s     |
| dom4j              | Java     | 189 ms      | ~5.6 MB/s     |
| html.parser        | Python   | 1015 ms     | ~1.0 MB/s     |

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