// SIMD-accelerated XML parser with XPath 1.0 evaluation.
//
// Ported from Rust `lib.rs`. This is the public API entry point.
//
// Port status: error, bloom, simd (runtime dispatch), index, batch, persist,
// parallel and xpath are ported. `parse()` is wired to `index::parse_scalar`
// and `load_or_parse()` to `persist`; `parse_for_xpath()` (lazy query-driven
// indexing) is still a stub.
#pragma once

#include "bloom.hpp"
#include "error.hpp"

#include <span>
#include <string>
#include <string_view>

namespace rai::xml {

// Forward declarations of types ported in later phases. They are referenced
// by the public API below so that this header compiles standalone.
class XmlIndex;
class OwnedXmlIndex;
class CompiledXPath;
class XPathResult;

/// Parse XML bytes and build a structural index.
///
/// This is the main entry point. Returns an `XmlIndex` that can be queried
/// with XPath expressions. Uses the fastest available parser for the input.
///
/// Implemented on top of `index::parse_scalar` (memchr scanner with SIMD
/// structural classification). The quote-ratio heuristic from the Rust crate
/// is not ported yet — see `index/structural.cpp`.
[[nodiscard]] Result<XmlIndex> parse(std::span<std::byte const> input);

/// Parse XML with query-driven optimization: only index tags relevant to the
/// given XPath expression. Falls back to full parse if the query uses wildcards.
///
/// Not yet implemented — depends on `index::lazy` (Phase 3) + `xpath` (Phase 5).
[[nodiscard]] Result<XmlIndex>
parse_for_xpath(std::span<std::byte const> input, std::string_view xpath_str);

/// Load a pre-built `.sxi` index if it exists and is fresh, otherwise parse
/// and save the index for next time. Returns an `OwnedXmlIndex` that owns
/// its backing XML bytes.
///
/// Implemented in `persist/persist.cpp`.
[[nodiscard]] Result<OwnedXmlIndex>
load_or_parse(std::string_view xml_path);

}  // namespace rai::xml