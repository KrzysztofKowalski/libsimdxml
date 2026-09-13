# libsimdxml

![libsimdxml](logo.webp)

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

Measured on an Intel Core i7-4850HQ (Haswell, 2013 laptop) at the CPU's
deterministic base clock (2.3 GHz, no turbo). Sequential benchmark
orchestrator: fresh process per run, cold runs discarded, medians of 9 reps
(2 cold discarded) — ratios within one run are the point.

| Workload                                          | sample_1mb.xml          | sample_5mb.xml         | sample_10mb.xml        |
|---------------------------------------------------|-------------------------|------------------------|------------------------|
| libsimdxml — parse (structural index)             | **246.8 MB/s** (4.06 ms)| **263.6 MB/s** (19.0 ms)| **286.3 MB/s** (34.9 ms)|
| pugixml 1.16, in-situ (reference)                 | 205.5 MB/s (4.87 ms)    | 227.0 MB/s (22.0 ms)   | 229.8 MB/s (43.5 ms)   |

Speedup of libsimdxml over pugixml 1.16: **1.20× / 1.16× / 1.25×**
(1 MB / 5 MB / 10 MB), from the final 13.09 full-native matrix
(`rerun_crosslang_20260913_165853`: 18 parsers, 7 languages —
C++/Python/Node/Java/Rust/Ruby/Go — in one run, Java natively via javac). The
numbers reproduce across independent runs within ~3% noise. libsimdxml
nodes/attributes counts are bit-identical to pugixml and expat at every size.

Earlier in-process A/B harness (100+ alternating runs, pre-fusion code):
parse 351/374 MB/s (1.26×/1.36× vs pugixml), full index 285/300 MB/s
(1.02×/1.10×), structural classification only ~4.3–4.8 GB/s; runtime dispatch
measured in-binary: scalar 0.8 GB/s → AVX2 3.1–4.3 GB/s.

### Cross-language context

Same `sample_1mb.xml` file, same base-clock run 165853 (13.09) — all rows
measured in one native orchestrator run, Java and Go included:

| Parser             | Language | Parse time  | Throughput    |
|--------------------|----------|-------------|---------------|
| **libsimdxml**     | C++      | **4.06 ms** | **246.8 MB/s**|
| pugixml 1.16       | C++      | 4.87 ms     | 205.5 MB/s    |
| quick-xml 0.36     | Rust     | 5.11 ms     | 195.9 MB/s    |
| expat (SAX)        | C++      | 15.51 ms    | 64.5 MB/s     |
| roxmltree 0.20     | Rust     | 17.93 ms    | 55.8 MB/s     |
| lxml               | Python   | 33.45 ms    | 29.9 MB/s     |
| libxml2 (DOM)      | C++      | 36.29 ms    | 27.6 MB/s     |
| nokogiri           | Ruby     | 44.63 ms    | 22.4 MB/s     |
| lxml.html          | Python   | 48.73 ms    | 20.5 MB/s     |
| gosax              | Go       | 49.43 ms    | 20.2 MB/s     |
| cheerio            | Node.js  | 99.37 ms    | 10.1 MB/s     |
| sax (Node)         | Node.js  | 131.26 ms   | 7.6 MB/s      |
| Xerces (DOM)       | Java     | 153.85 ms   | 6.5 MB/s      |
| xml.etree          | Python   | 159.31 ms   | 6.3 MB/s      |
| jsoup              | Java     | 260.73 ms   | 3.8 MB/s      |
| dom4j              | Java     | 305.58 ms   | 3.3 MB/s      |
| html.parser        | Python   | 1548.49 ms  | 0.7 MB/s      |
| rexml              | Ruby     | 1684.12 ms  | 0.6 MB/s      |

Rust's quick-xml is the fastest non-C++ parser (~195 MB/s, flat across sizes;
5–7% behind pugixml @1 MB, ~15% @5–10 MB) — libsimdxml beats it by
1.26×/1.36×/1.47× (1 MB/5 MB/10 MB). Go's `encoding/xml` (gosax, event-based)
sits mid-table at ~20 MB/s flat — over 10× behind quick-xml, with
bit-identical node/attribute counts. Versions: Java natively on this host
(javac, JDK 27); Go 1.27; quick-xml 0.36.2, roxmltree 0.20.0; nokogiri 1.19.4,
rexml 3.4.4. tinyxml2 and fast-xml-parser error out on the `<?bench-pi…?>`
corpus (a pre-existing entity limit / PI-node limitation, not a bug) and are
excluded.

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