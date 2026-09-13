// Speculative parallel chunked parsing.
//
// Ported from Rust `parallel/mod.rs`. Split a large XML document into K
// chunks at safe boundaries (between tags), parse each chunk in parallel
// with `std::jthread`, then merge and compute depth/parent relationships in
// a single sequential pass.
//
// Algorithm:
//   1. Find K-1 safe split points (positions of `>` between tags).
//   2. Spawn K threads, each parsing its chunk independently.
//   3. Each thread produces: tag_starts, tag_ends, tag_types, tag_names,
//      text_ranges (no depth/parent — those need global state).
//   4. Merge chunks (concatenate — already in document order).
//   5. Compute depths and parents in one sequential pass.
//   6. Build CSR indices lazily on first XPath eval.
#pragma once

#include "../error.hpp"
#include "../index/xml_index.hpp"

#include <cstddef>
#include <span>

namespace rai::xml {

/// Parse XML using multiple threads.
///
/// Falls back to sequential parsing for small documents or when
/// `num_threads <= 1`.
[[nodiscard]] Result<XmlIndex>
parse_parallel(std::span<std::byte const> input, std::size_t num_threads);

/// Parse parallel and immediately build indices (for callers that need them).
[[nodiscard]] Result<XmlIndex>
parse_parallel_indexed(std::span<std::byte const> input, std::size_t num_threads);

}  // namespace rai::xml