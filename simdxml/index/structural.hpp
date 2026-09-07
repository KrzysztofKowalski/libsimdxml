// Structural XML parsers that produce an `XmlIndex`.
//
// Ported from Rust `index/structural.rs`. Two strategies:
//   - `parse_scalar`    — memchr-based scanner (text-heavy XML)
//   - `parse_two_stage` — SIMD two-stage classifier (attribute-heavy XML)
//
// The `simdxml::parse` entry point (defined in `simdxml.hpp` in a later phase)
// selects between them using a `quote_ratio` heuristic. Both produce the same
// `XmlIndex` arrays.
#pragma once

#include "../error.hpp"
#include "xml_index.hpp"

#include <span>

namespace simdxml {

/// Build an `XmlIndex` from XML bytes using memchr-based scanning.
/// Mirrors Rust `parse_scalar`. Jumps directly between `<` characters using
/// SIMD-accelerated `std::memchr`, skipping text regions entirely.
[[nodiscard]] Result<XmlIndex> parse_scalar(std::span<std::byte const> input);

/// Build an `XmlIndex` from XML bytes using NEON/SSE/AVX two-stage
/// classification. Mirrors Rust `parse_two_stage`. Stage 1 classifies every
/// byte with SIMD vector ops; Stage 2 walks the bitmasks.
[[nodiscard]] Result<XmlIndex> parse_two_stage(std::span<std::byte const> input);

}  // namespace simdxml