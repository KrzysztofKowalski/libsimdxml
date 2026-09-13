// Implementation of batched SIMD string predicate evaluation.
//
// Ported from Rust `xpath/simd_pred.rs`. The Rust implementation concatenates
// candidate text contents with NUL separators, then uses `memchr::memmem` to
// find all occurrences of `needle` in one pass, mapping hits back to the
// originating candidate via binary search into the offset table.
//
// The C++ port follows the same structure. The substring search uses
// `std::string_view::find` (which calls libc `memmem` on glibc — itself
// SIMD-accelerated on modern CPUs). A `find_all` helper collects all
// occurrences. Below `XPATH_BATCH_THRESHOLD` candidates we fall back to
// per-node evaluation (lower overhead).
#include "simd_pred.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace rai::xml {

namespace {

/// Owned text content of a node (mirrors Rust `node_text_content`).
[[nodiscard]] std::string
node_text_content(XmlIndex const& index, XPathNode const& node) {
    switch (node.kind()) {
        case XPathNode::Kind::Element: {
            std::size_t const idx = node.index();
            if (idx == DOC_ROOT) return std::string{};
            return index.all_text(idx);
        }
        case XPathNode::Kind::Text: {
            std::size_t const idx = node.index();
            if (idx >= index.text_ranges.size()) return std::string{};
            return std::string(index.text_content(index.text_ranges[idx]));
        }
        case XPathNode::Kind::Attribute: {
            std::size_t const tag_idx = node.index();
            std::uint64_t const hash = node.hash();
            auto names = index.get_all_attribute_names(tag_idx);
            for (auto name : names) {
                if (attr_name_hash(name) == hash) {
                    auto v = index.get_attribute(tag_idx, name);
                    return v ? std::string(*v) : std::string{};
                }
            }
            return std::string{};
        }
        case XPathNode::Kind::Namespace:
            return std::string{};
    }
    return std::string{};
}

/// Collect all occurrences of `needle` in `haystack` (memmem-style).
/// On glibc, `string_view::find` is implemented via memchr2 + verify,
/// which is SIMD-accelerated on x86_64/AArch64.
[[nodiscard]] std::vector<std::size_t>
find_all(std::string_view haystack, std::string_view needle) {
    std::vector<std::size_t> hits;
    if (needle.empty()) return hits;
    std::size_t pos = 0;
    while (true) {
        std::size_t const hit = haystack.find(needle, pos);
        if (hit == std::string_view::npos) break;
        hits.push_back(hit);
        pos = hit + 1;  // overlapping matches allowed (matches Rust iter behavior)
    }
    return hits;
}

/// Binary search the offset table to find which candidate owns a given byte
/// position. Mirrors the Rust `offsets.binary_search(&pos)` logic.
[[nodiscard]] std::size_t
offset_for(std::vector<std::size_t> const& offsets, std::size_t pos) {
    // offsets has N+1 entries (start of each candidate + total length).
    // Find the largest i such that offsets[i] <= pos.
    auto it = std::upper_bound(offsets.begin(), offsets.end(), pos);
    if (it == offsets.begin()) return 0;
    std::size_t const idx = static_cast<std::size_t>(it - offsets.begin()) - 1;
    return idx;
}

}  // namespace

std::vector<bool>
batch_contains(XmlIndex const& index,
               std::vector<XPathNode> const& candidates,
               std::string_view needle) {
    std::vector<bool> results(candidates.size(), false);
    if (candidates.size() < XPATH_BATCH_THRESHOLD || needle.empty()) {
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            auto text = node_text_content(index, candidates[i]);
            results[i] = text.find(needle) != std::string::npos;
        }
        return results;
    }

    // Build concatenated text buffer with NUL separators (NUL is invalid in
    // XML text content, so it acts as a safe sentinel between candidates).
    std::string buffer;
    std::vector<std::size_t> offsets;
    offsets.reserve(candidates.size() + 1);
    for (auto const& node : candidates) {
        offsets.push_back(buffer.size());
        auto text = node_text_content(index, node);
        buffer.append(text);
        buffer.push_back('\0');
    }
    offsets.push_back(buffer.size());

    auto hits = find_all(std::string_view(buffer), needle);
    for (std::size_t pos : hits) {
        std::size_t const doc_idx = offset_for(offsets, pos);
        if (doc_idx < candidates.size()) {
            // Verify the match is entirely within this candidate (not crossing
            // the NUL separator). `find` won't cross NUL since needle has no
            // NUL — but be defensive in case the needle is empty (handled above).
            results[doc_idx] = true;
        }
    }
    return results;
}

std::vector<bool>
batch_starts_with(XmlIndex const& index,
                 std::vector<XPathNode> const& candidates,
                 std::string_view prefix) {
    std::vector<bool> results(candidates.size(), false);
    if (candidates.size() < XPATH_BATCH_THRESHOLD || prefix.empty()) {
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            auto text = node_text_content(index, candidates[i]);
            results[i] = text.starts_with(prefix);
        }
        return results;
    }

    // For starts-with we only need to check the beginning of each node's text.
    // No concatenation needed — per-node prefix check is O(1) per candidate
    // after fetching the text.
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto text = node_text_content(index, candidates[i]);
        results[i] = text.size() >= prefix.size() &&
                     std::memcmp(text.data(), prefix.data(), prefix.size()) == 0;
    }
    return results;
}

}  // namespace rai::xml