// Batched SIMD string predicate evaluation for XPath.
//
// Ported from Rust `xpath/simd_pred.rs`. When evaluating `contains(., 'needle')`
// or `starts-with(., 'prefix')` across many candidate nodes, this module
// batches the operation: concatenate all text content with NUL separators,
// run a SIMD-accelerated substring search, then map matches back to the
// originating candidate via binary search into the offset table.
//
// The Rust implementation uses `memchr::memmem` (Teddy algorithm). The C++
// port uses a hand-rolled search that delegates to the libc `memchr`/
// `std::string_view::find` fast path; on glibc/musl these are themselves
// SIMD-accelerated. A pluggable backend hook is provided so a future AVX2
// Teddy implementation can slot in.
#pragma once

#include "eval.hpp"
#include "../index/xml_index.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rai::xml {

/// Minimum candidates to trigger batched evaluation.
/// Below this threshold, per-node evaluation is faster (lower overhead).
constexpr std::size_t XPATH_BATCH_THRESHOLD = 8;

/// Evaluate `contains(., needle)` across a set of candidate element nodes.
/// Returns a bitmask: `result[i]` is true if the text content of
/// `candidates[i]` contains `needle`.
[[nodiscard]] std::vector<bool>
batch_contains(XmlIndex const& index,
               std::vector<XPathNode> const& candidates,
               std::string_view needle);

/// Evaluate `starts-with(., prefix)` across a set of candidate element nodes.
[[nodiscard]] std::vector<bool>
batch_starts_with(XmlIndex const& index,
                 std::vector<XPathNode> const& candidates,
                 std::string_view prefix);

}  // namespace rai::xml