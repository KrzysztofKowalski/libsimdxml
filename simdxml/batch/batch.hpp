// Columnar batch XPath evaluation.
//
// Ported from Rust `batch/mod.rs`. Evaluate an XPath expression against a
// batch of XML documents, returning results grouped by document. Amortizes
// XPath compilation and integrates bloom filtering and lazy parsing for
// maximum throughput.
//
// The implementations reference `CompiledXPath` (Phase 5). Function bodies
// are linked once the xpath module is available; the declarations here keep
// the public API surface stable across phases.
#pragma once

#include "../error.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace simdxml {

class CompiledXPath;  // forward declaration — Phase 5

/// Evaluate an XPath expression against a batch of documents, returning text.
[[nodiscard]] Result<std::vector<std::vector<std::string>>>
eval_batch_text(std::span<std::span<std::byte const> const> docs,
                CompiledXPath const& xpath);

/// Evaluate with lazy parsing: only index tags relevant to the XPath query.
[[nodiscard]] Result<std::vector<std::vector<std::string>>>
eval_batch_text_lazy(std::span<std::span<std::byte const> const> docs,
                     CompiledXPath const& xpath);

/// Evaluate with bloom filtering + lazy parsing: skip documents that can't match.
[[nodiscard]] Result<std::vector<std::vector<std::string>>>
eval_batch_text_bloom(std::span<std::span<std::byte const> const> docs,
                      CompiledXPath const& xpath);

/// Count matching nodes per document without extracting text.
[[nodiscard]] Result<std::vector<std::size_t>>
count_batch(std::span<std::span<std::byte const> const> docs,
            CompiledXPath const& xpath);

/// Evaluate a batch of documents with automatic inter-document parallelism.
///
/// Large docs (>=256 KB) get intra-document parallel parsing; all docs are
/// processed concurrently up to `max_threads`.
[[nodiscard]] Result<std::vector<std::vector<std::string>>>
eval_batch_parallel(std::span<std::span<std::byte const> const> docs,
                     CompiledXPath const& xpath, std::size_t max_threads);

}  // namespace simdxml