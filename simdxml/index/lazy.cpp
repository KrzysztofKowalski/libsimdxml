// Implementation of the query-driven lazy parser.
//
// Ported from Rust `index/lazy.rs` (`parse_for_query`). Tracks full structural
// state for correctness (depth, parent stack) but only emits index entries
// for tags whose names are in the interesting set plus their ancestors.
#include "lazy.hpp"

#include "structural.hpp"

#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace simdxml {

namespace {

[[nodiscard]] inline unsigned char b_at(std::span<std::byte const> in,
                                         std::size_t i) noexcept {
    return static_cast<unsigned char>(std::to_integer<std::uint8_t>(in[i]));
}
[[nodiscard]] inline bool is_ws(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

[[nodiscard]] inline std::size_t
find_byte(unsigned char needle, std::span<std::byte const> haystack,
          std::size_t start) noexcept {
    if (start >= haystack.size()) return std::size_t(-1);
    void const* base = static_cast<void const*>(haystack.data() + start);
    std::size_t const remaining = haystack.size() - start;
    auto const* p = static_cast<unsigned char const*>(
        std::memchr(base, needle, remaining));
    if (p == nullptr) return std::size_t(-1);
    return start + static_cast<std::size_t>(p - static_cast<unsigned char const*>(base));
}

struct ParentEntry {
    bool is_interesting = false;
    std::uint32_t index_tag_idx = UINT32_MAX;
};

[[nodiscard]] bool
has_interesting_ancestor(std::vector<ParentEntry> const& stack) noexcept {
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        if (it->is_interesting) return true;
    }
    return false;
}

[[nodiscard]] std::uint32_t
find_interesting_parent(std::vector<ParentEntry> const& stack) noexcept {
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        if (it->is_interesting) return it->index_tag_idx;
    }
    return UINT32_MAX;
}

void capture_text_if_interesting(
    XmlIndex& index, std::span<std::byte const> input,
    std::size_t last_tag_end, std::size_t current_pos,
    std::vector<ParentEntry> const& parent_stack) {
    std::size_t const text_start = (last_tag_end > 0) ? last_tag_end + 1 : 0;
    if (text_start >= current_pos) return;
    if (!has_interesting_ancestor(parent_stack)) return;
    bool has_non_ws = false;
    for (std::size_t i = text_start; i < current_pos; ++i) {
        if (!is_ws(b_at(input, i))) { has_non_ws = true; break; }
    }
    if (!has_non_ws) return;
    auto const parent = find_interesting_parent(parent_stack);
    index.text_ranges.push_back(TextRange{
        static_cast<std::uint64_t>(text_start),
        static_cast<std::uint64_t>(current_pos), parent});
}

}  // namespace

Result<XmlIndex>
parse_for_query(std::span<std::byte const> input,
                std::unordered_set<std::string> const& interesting_names) {
    if (interesting_names.empty()) {
        return parse_scalar(input);
    }

    std::size_t const est_tags = input.size() / 256;
    std::size_t const est_text = est_tags / 2;
    XmlIndex index(input, est_tags, est_text);

    std::size_t pos = 0;
    std::uint16_t depth = 0;
    std::vector<ParentEntry> parent_stack;
    parent_stack.reserve(64);
    std::size_t last_tag_end = 0;

    while (true) {
        std::size_t const offset = find_byte('<', input, pos);
        if (offset == std::size_t(-1)) break;
        pos = offset;
        std::size_t const tag_start = pos;

        if (pos + 1 >= input.size()) {
            return std::unexpected(SimdXmlError::unclosed_tag(pos));
        }

        unsigned char const c1 = b_at(input, pos + 1);
        if (c1 == '/') {
            pos += 2;
            std::size_t const name_start = pos;
            while (pos < input.size() && b_at(input, pos) != '>' &&
                   !is_ws(b_at(input, pos))) {
                ++pos;
            }
            std::size_t const name_end = pos;
            std::size_t const gt = find_byte('>', input, pos);
            if (gt == std::size_t(-1)) {
                return std::unexpected(SimdXmlError::unclosed_tag(tag_start));
            }
            pos = gt;

            if (depth > 0) --depth;
            ParentEntry entry;
            if (!parent_stack.empty()) {
                entry = parent_stack.back();
                parent_stack.pop_back();
            }

            if (entry.is_interesting) {
                std::uint32_t const open_tag_idx = entry.index_tag_idx;
                std::size_t const text_start = (last_tag_end > 0) ? last_tag_end + 1 : 0;
                if (text_start < tag_start) {
                    bool has_non_ws = false;
                    for (std::size_t i = text_start; i < tag_start; ++i) {
                        if (!is_ws(b_at(input, i))) { has_non_ws = true; break; }
                    }
                    if (has_non_ws) {
                        index.text_ranges.push_back(TextRange{
                            static_cast<std::uint64_t>(text_start),
                            static_cast<std::uint64_t>(tag_start), open_tag_idx});
                    }
                }
                index.tag_starts.push_back(static_cast<std::uint64_t>(tag_start));
                index.tag_ends.push_back(static_cast<std::uint64_t>(pos));
                index.tag_types.push_back(TagType::Close);
                index.tag_names.emplace_back(static_cast<std::uint64_t>(name_start),
                                              static_cast<std::uint16_t>(name_end - name_start));
                index.depths.push_back(depth);
                index.parents.push_back(find_interesting_parent(parent_stack));
            }
            last_tag_end = pos;
            ++pos;
        } else if (c1 == '!') {
            if (pos + 4 <= input.size() &&
                b_at(input, pos + 2) == '-' && b_at(input, pos + 3) == '-') {
                // Comment — skip.
                pos += 4;
                while (true) {
                    std::size_t const dash = find_byte('-', input, pos);
                    if (dash == std::size_t(-1)) { pos = input.size(); break; }
                    pos = dash;
                    if (pos + 3 <= input.size() &&
                        b_at(input, pos) == '-' && b_at(input, pos + 1) == '-' &&
                        b_at(input, pos + 2) == '>') {
                        pos += 2;
                        break;
                    }
                    ++pos;
                }
                last_tag_end = pos;
                ++pos;
            } else if (pos + 9 <= input.size() &&
                       b_at(input, pos + 2) == '[' && b_at(input, pos + 3) == 'C' &&
                       b_at(input, pos + 4) == 'D' && b_at(input, pos + 5) == 'A' &&
                       b_at(input, pos + 6) == 'T' && b_at(input, pos + 7) == 'A' &&
                       b_at(input, pos + 8) == '[') {
                // CDATA — capture text if under interesting parent.
                pos += 9;
                std::size_t const content_start = pos;
                while (true) {
                    std::size_t const br = find_byte(']', input, pos);
                    if (br == std::size_t(-1)) break;
                    pos = br;
                    if (pos + 3 <= input.size() &&
                        b_at(input, pos) == ']' && b_at(input, pos + 1) == ']' &&
                        b_at(input, pos + 2) == '>') {
                        if (pos > content_start && has_interesting_ancestor(parent_stack)) {
                            auto const parent = find_interesting_parent(parent_stack);
                            index.text_ranges.push_back(TextRange{
                                static_cast<std::uint64_t>(content_start),
                                static_cast<std::uint64_t>(pos), parent});
                        }
                        pos += 2;
                        break;
                    }
                    ++pos;
                }
                last_tag_end = pos;
                ++pos;
            } else {
                std::size_t const gt = find_byte('>', input, pos);
                if (gt != std::size_t(-1)) pos = gt;
                last_tag_end = pos;
                ++pos;
            }
        } else if (c1 == '?') {
            pos += 2;
            while (pos + 1 < input.size()) {
                if (b_at(input, pos) == '?' && b_at(input, pos + 1) == '>') {
                    ++pos;
                    break;
                }
                ++pos;
            }
            last_tag_end = pos;
            ++pos;
        } else {
            // Open or self-closing tag.
            pos += 1;
            std::size_t const name_start = pos;
            while (pos < input.size() && b_at(input, pos) != '>' &&
                   b_at(input, pos) != '/' && !is_ws(b_at(input, pos))) {
                ++pos;
            }
            std::size_t const name_end = pos;

            bool self_closing = false;
            while (pos < input.size() && b_at(input, pos) != '>') {
                if (b_at(input, pos) == '/' && pos + 1 < input.size() &&
                    b_at(input, pos + 1) == '>') {
                    self_closing = true;
                    ++pos;
                    break;
                }
                if (b_at(input, pos) == '"') {
                    ++pos;
                    std::size_t const q = find_byte('"', input, pos);
                    if (q != std::size_t(-1)) pos = q;
                } else if (b_at(input, pos) == '\'') {
                    ++pos;
                    std::size_t const q = find_byte('\'', input, pos);
                    if (q != std::size_t(-1)) pos = q;
                }
                ++pos;
            }
            if (pos >= input.size()) {
                return std::unexpected(SimdXmlError::unclosed_tag(tag_start));
            }

            // Check if name is interesting.
            std::size_t const name_len = name_end - name_start;
            std::string tag_name_str;
            if (name_len > 0) {
                tag_name_str.assign(reinterpret_cast<char const*>(input.data()) + name_start,
                                    name_len);
            }
            bool const is_interesting = interesting_names.contains(tag_name_str);

            TagType const tt = self_closing ? TagType::SelfClose : TagType::Open;

            if (is_interesting) {
                capture_text_if_interesting(index, input, last_tag_end, tag_start,
                                            parent_stack);
                std::uint32_t const tag_idx = static_cast<std::uint32_t>(index.tag_starts.size());
                std::uint32_t const parent_idx = find_interesting_parent(parent_stack);
                index.tag_starts.push_back(static_cast<std::uint64_t>(tag_start));
                index.tag_ends.push_back(static_cast<std::uint64_t>(pos));
                index.tag_types.push_back(tt);
                index.tag_names.emplace_back(static_cast<std::uint64_t>(name_start),
                                              static_cast<std::uint16_t>(name_len));
                index.depths.push_back(depth);
                index.parents.push_back(parent_idx);
                if (tt == TagType::Open) {
                    parent_stack.push_back(ParentEntry{true, tag_idx});
                    ++depth;
                }
            } else {
                if (tt == TagType::Open) {
                    parent_stack.push_back(ParentEntry{false, UINT32_MAX});
                    ++depth;
                }
            }
            last_tag_end = pos;
            ++pos;
        }
    }

    if (index.tag_count() >= 64) {
        index.build_indices();
    }
    return index;
}

}  // namespace simdxml