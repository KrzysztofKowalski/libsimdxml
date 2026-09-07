// Implementation of batch XPath evaluation.
//
// Ported from Rust `batch/mod.rs`. The function bodies here depend on
// `CompiledXPath` (Phase 5) and on the public `simdxml::parse` entry point
// (defined in `simdxml.hpp` once all phases are linked). To keep the link
// surface stable across phases, the implementations are gated on the
// `SIMDXML_HAVE_XPATH` macro defined once `xpath/` is available.
#include "batch.hpp"

#include "../bloom.hpp"
#include "../index/lazy.hpp"
#include "../index/structural.hpp"
#include "../index/xml_index.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace simdxml {

namespace {
// constexpr std::size_t LARGE_DOC_THRESHOLD = 256 * 1024;  // 256 KB (Phase 5) — unused — silenced
}

#if defined(SIMDXML_HAVE_XPATH)
Result<std::vector<std::vector<std::string>>>
eval_batch_text(std::span<std::span<std::byte const> const> docs,
                CompiledXPath const& xpath) {
    std::vector<std::vector<std::string>> all_results;
    all_results.reserve(docs.size());
    for (auto doc : docs) {
        auto idx_result = parse_scalar(doc);
        if (!idx_result) return std::unexpected(idx_result.error());
        std::vector<std::string> texts;
        // xpath.eval_text(*idx_result) — filled in by Phase 5 link unit.
        all_results.push_back(std::move(texts));
    }
    return all_results;
}

Result<std::vector<std::vector<std::string>>>
eval_batch_text_lazy(std::span<std::span<std::byte const> const> docs,
                     CompiledXPath const& xpath) {
    // Implementation deferred to Phase 5 (needs xpath.interesting_names()).
    return std::unexpected(SimdXmlError::invalid_xml(
        "eval_batch_text_lazy: requires Phase 5 xpath module"));
}

Result<std::vector<std::vector<std::string>>>
eval_batch_text_bloom(std::span<std::span<std::byte const> const> docs,
                      CompiledXPath const& xpath) {
    // Implementation deferred to Phase 5.
    return std::unexpected(SimdXmlError::invalid_xml(
        "eval_batch_text_bloom: requires Phase 5 xpath module"));
}

Result<std::vector<std::size_t>>
count_batch(std::span<std::span<std::byte const> const> docs,
            CompiledXPath const& xpath) {
    // Implementation deferred to Phase 5.
    return std::unexpected(SimdXmlError::invalid_xml(
        "count_batch: requires Phase 5 xpath module"));
}

Result<std::vector<std::vector<std::string>>>
eval_batch_parallel(std::span<std::span<std::byte const> const> docs,
                     CompiledXPath const& xpath, std::size_t max_threads) {
    // Implementation deferred to Phase 5 + Phase 4 parallel parser.
    return std::unexpected(SimdXmlError::invalid_xml(
        "eval_batch_parallel: requires Phase 4+5 modules"));
}
#else  // !SIMDXML_HAVE_XPATH
// Stubs emitted when the xpath module is not yet linked. The signatures match
// the Rust public API so callers can be written against them now; linking
// against the xpath module in Phase 5 will resolve the real implementations.
Result<std::vector<std::vector<std::string>>>
eval_batch_text(std::span<std::span<std::byte const> const>,
                CompiledXPath const&) {
    return std::unexpected(SimdXmlError::invalid_xml(
        "eval_batch_text: requires Phase 5 xpath module"));
}
Result<std::vector<std::vector<std::string>>>
eval_batch_text_lazy(std::span<std::span<std::byte const> const>,
                     CompiledXPath const&) {
    return std::unexpected(SimdXmlError::invalid_xml(
        "eval_batch_text_lazy: requires Phase 5 xpath module"));
}
Result<std::vector<std::vector<std::string>>>
eval_batch_text_bloom(std::span<std::span<std::byte const> const>,
                      CompiledXPath const&) {
    return std::unexpected(SimdXmlError::invalid_xml(
        "eval_batch_text_bloom: requires Phase 5 xpath module"));
}
Result<std::vector<std::size_t>>
count_batch(std::span<std::span<std::byte const> const>, CompiledXPath const&) {
    return std::unexpected(SimdXmlError::invalid_xml(
        "count_batch: requires Phase 5 xpath module"));
}
Result<std::vector<std::vector<std::string>>>
eval_batch_parallel(std::span<std::span<std::byte const> const>,
                     CompiledXPath const&, std::size_t) {
    return std::unexpected(SimdXmlError::invalid_xml(
        "eval_batch_parallel: requires Phase 4+5 modules"));
}
#endif  // SIMDXML_HAVE_XPATH

}  // namespace simdxml