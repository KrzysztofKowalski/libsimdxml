// Flat-array structural index for XML documents.
//
// Ported from Rust `index/mod.rs` `XmlIndex<'a>`. The C++ port drops the
// lifetime parameter — `XmlIndex` holds a non-owning `std::span<std::byte const>`
// referencing the XML bytes. The caller is responsible for keeping the bytes
// alive for the duration of index use (mirroring the Rust `'a` borrow).
//
// Memory layout matches the Rust struct exactly (struct-of-arrays), so the
// `.sxi` persistence format (Phase 4) is byte-compatible.
#pragma once

#include "types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rai::xml {

/// Flat-array structural index — no DOM tree.
///
/// Built from XML bytes in one pass (scalar or SIMD). Enables random-access
/// evaluation of all 13 XPath 1.0 axes via array operations instead of
/// pointer-chasing through a DOM.
///
/// Memory: ~16 bytes per tag vs ~35 bytes per node in a typical DOM.
class XmlIndex {
public:
    XmlIndex() = default;
    explicit XmlIndex(std::span<std::byte const> input) : input_(input) {}

    /// Construct with reserved capacities (used by parsers).
    XmlIndex(std::span<std::byte const> input,
             std::size_t est_tags, std::size_t est_text) : input_(input) {
        tag_starts.reserve(est_tags);
        tag_ends.reserve(est_tags);
        tag_types.reserve(est_tags);
        tag_names.reserve(est_tags);
        depths.reserve(est_tags);
        parents.reserve(est_tags);
        text_ranges.reserve(est_text);
    }

    // === Accessors ===

    std::span<std::byte const> input() const noexcept { return input_; }
    std::size_t tag_count() const noexcept { return tag_starts.size(); }
    std::size_t text_count() const noexcept { return text_ranges.size(); }

    TagType tag_type(std::size_t idx) const { return tag_types.at(idx); }
    std::uint16_t depth(std::size_t idx) const { return depths.at(idx); }
    std::uint16_t max_depth() const noexcept;

    /// Byte offset of the i-th '<'.
    std::uint64_t tag_start(std::size_t idx) const { return tag_starts.at(idx); }
    /// Byte offset of the i-th '>'.
    std::uint64_t tag_end(std::size_t idx) const { return tag_ends.at(idx); }

    /// Tag name as a string_view into the XML input. "" if out of range.
    [[nodiscard]] std::string_view tag_name(std::size_t idx) const noexcept;

    /// Fast tag-name equality check (avoids constructing string_view on hot path).
    [[nodiscard]] bool tag_name_eq(std::size_t idx, std::string_view name) const noexcept;

    // === Index build / query ===

    /// Ensure precomputed CSR indices are built. No-op if already built.
    void ensure_indices();

    /// Build precomputed indices (CSR children, text children, close_map,
    /// post_order). O(n) time, flat memory layout. Sequential for <10K tags,
    /// threaded CSR builds above that threshold.
    void build_indices();

    /// Build inverted name index for repeated query workloads.
    void build_name_index();

    [[nodiscard]] std::optional<std::uint16_t>
    name_id(std::string_view name) const noexcept;

    /// Posting list (sorted tag indices) for a name. O(1) lookup.
    [[nodiscard]] std::span<std::uint32_t const>
    tags_by_name(std::string_view name) const noexcept;

    [[nodiscard]] bool has_indices() const noexcept { return !child_offsets.empty(); }

    /// O(1) ancestor check using pre/post numbering.
    [[nodiscard]] bool
    is_ancestor(std::size_t ancestor_idx, std::size_t descendant_idx) const noexcept;

    /// Find the matching close tag for an open tag. std::nullopt if none.
    [[nodiscard]] std::optional<std::size_t>
    matching_close(std::size_t open_idx) const;

    /// Get child tag indices for a parent (allocates).
    [[nodiscard]] std::vector<std::size_t>
    children(std::size_t parent_idx) const;

    /// Borrowed child slice (zero allocation). Requires `ensure_indices()`.
    [[nodiscard]] std::span<std::uint32_t const>
    child_tag_slice(std::size_t parent_idx) const noexcept;

    /// Borrowed text-child slice (zero allocation). Requires `ensure_indices()`.
    [[nodiscard]] std::span<std::uint32_t const>
    child_text_slice(std::size_t parent_idx) const noexcept;

    /// Number of direct child elements.
    [[nodiscard]] std::size_t child_count(std::size_t parent_idx) const;

    /// i-th child element by position, or std::nullopt.
    [[nodiscard]] std::optional<std::size_t>
    child_at(std::size_t parent_idx, std::size_t pos) const;

    /// Parent of an element, or std::nullopt for root / out-of-range.
    [[nodiscard]] std::optional<std::size_t>
    parent(std::size_t tag_idx) const noexcept;

    /// Position of a tag within its parent's children list. std::nullopt for root.
    [[nodiscard]] std::optional<std::size_t>
    child_position(std::size_t tag_idx) const;

    /// Direct text children as string_views (allocates vector).
    [[nodiscard]] std::vector<std::string_view>
    direct_text(std::size_t tag_idx) const;

    /// First direct text child (ElementTree `.text` semantics).
    [[nodiscard]] std::optional<std::string_view>
    direct_text_first(std::size_t tag_idx) const;

    /// Tail text of an element — text after its closing tag, before the next sibling.
    [[nodiscard]] std::optional<std::string_view>
    tail_text(std::size_t tag_idx) const;

    /// Collect all text within an element in stdlib itertext() order.
    [[nodiscard]] std::vector<std::string_view>
    itertext_collect(std::size_t tag_idx) const;

    /// All text within an element (including nested). Returns owned string.
    [[nodiscard]] std::string all_text(std::size_t tag_idx) const;

    /// Raw XML bytes for an element (from opening tag through closing tag).
    [[nodiscard]] std::string_view raw_xml(std::size_t tag_idx) const;

    /// Raw XML bytes for just the opening tag at a given index.
    [[nodiscard]] std::string_view raw_tag(std::size_t tag_idx) const;

    /// Text content of a text range.
    [[nodiscard]] std::string_view text_content(TextRange const& range) const;

    /// Text content of a text range by its index.
    [[nodiscard]] std::string_view text_by_index(std::size_t text_idx) const;

    /// Decode XML entities in a string. Returns owned string (always copies).
    [[nodiscard]] static std::string decode_entities(std::string_view s);

    /// C14N canonical XML serialization of an element.
    [[nodiscard]] std::string canonicalize(std::size_t tag_idx) const;

    // === Tag attribute helpers (ported from `index/tags.rs`) ===

    [[nodiscard]] std::optional<std::string_view>
    get_attribute(std::size_t tag_idx, std::string_view attr_name) const;

    [[nodiscard]] std::vector<std::string_view>
    get_all_attribute_names(std::size_t tag_idx) const;

    [[nodiscard]] std::vector<std::pair<std::string_view, std::string_view>>
    attributes(std::size_t tag_idx) const;

    [[nodiscard]] std::vector<std::pair<std::string_view, std::string_view>>
    get_namespace_decls(std::size_t tag_idx) const;

    // === Public fields (mirroring Rust `pub` fields) ===

    std::vector<std::uint64_t> tag_starts;
    std::vector<std::uint64_t> tag_ends;
    std::vector<TagType> tag_types;
    std::vector<std::pair<std::uint64_t, std::uint16_t>> tag_names;
    std::vector<std::uint16_t> depths;
    std::vector<std::uint32_t> parents;
    std::vector<TextRange> text_ranges;

    std::vector<std::uint32_t> child_offsets;
    std::vector<std::uint32_t> child_data;
    std::vector<std::uint32_t> text_child_offsets;
    std::vector<std::uint32_t> text_child_data;
    std::vector<std::uint32_t> close_map;
    std::vector<std::uint32_t> post_order;

    std::vector<std::uint16_t> name_ids;
    std::vector<std::pair<std::uint64_t, std::uint16_t>> name_table;
    std::vector<std::vector<std::uint32_t>> name_posting;

private:
    std::span<std::byte const> input_;

    /// Recursive helper for C14N serialization.
    void c14n_element(std::size_t tag_idx, std::string& out) const;

    /// Helper: slice of input as string_view.
    [[nodiscard]] std::string_view
    slice_str(std::size_t start, std::size_t end) const noexcept;
};

/// Free functions used by `build_indices` for parallel construction.
///@{
[[nodiscard]] std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>>
build_csr_children(std::span<TagType const> tag_types,
                   std::span<std::uint32_t const> parents,
                   std::size_t n);

[[nodiscard]] std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>>
build_csr_text_children(std::span<TextRange const> text_ranges, std::size_t n);

[[nodiscard]] std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>>
build_close_map_and_post_order(std::span<TagType const> tag_types, std::size_t n);
///@}

}  // namespace rai::xml