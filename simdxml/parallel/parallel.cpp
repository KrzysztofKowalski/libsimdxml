// Implementation of the speculative parallel chunked parser.
//
// Ported from Rust `parallel/mod.rs` (`parse_parallel`, `find_split_points`,
// `find_safe_boundary`, `parse_chunk`, `merge_chunks`,
// `parse_parallel_indexed`). Rust `std::thread::scope` is replaced by a
// vector of `std::jthread` joined before merge.
#include "parallel.hpp"

#include "../index/structural.hpp"
#include "../index/types.hpp"
#include "../index/xml_index.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace simdxml {

namespace {

constexpr std::size_t MIN_PARALLEL_SIZE = 64 * 1024;  // 64 KB
constexpr std::uint32_t U32_SENTINEL = UINT32_MAX;

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

/// A single chunk's parse results (no depth/parent — those need global state).
struct ChunkResult {
    std::vector<std::uint64_t> tag_starts;
    std::vector<std::uint64_t> tag_ends;
    std::vector<TagType> tag_types;
    std::vector<std::pair<std::uint64_t, std::uint16_t>> tag_names;
    std::vector<TextRange> text_ranges;
};

[[nodiscard]] std::optional<std::size_t>
find_safe_boundary(std::span<std::byte const> input, std::size_t target) noexcept {
    std::size_t const t = std::min(target, input.size());
    std::size_t const search_start = (t >= 4096) ? t - 4096 : 0;

    for (std::size_t pos = t; pos > search_start; --pos) {
        if (b_at(input, pos - 1) != '>') continue;
        std::size_t const after = pos;
        if (after >= input.size()) return after;
        std::size_t check = after;
        while (check < input.size() && is_ws(b_at(input, check))) ++check;
        if (check >= input.size() || b_at(input, check) == '<') return after;
    }

    // Fallback: search forward from target.
    for (std::size_t pos = t; pos < input.size(); ++pos) {
        if (b_at(input, pos) == '>') return pos + 1;
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<std::size_t>
find_split_points(std::span<std::byte const> input, std::size_t num_chunks) {
    std::size_t const chunk_size = input.size() / num_chunks;
    std::vector<std::size_t> splits;
    splits.reserve(num_chunks - 1);
    for (std::size_t i = 1; i < num_chunks; ++i) {
        std::size_t const target = i * chunk_size;
        auto const pos = find_safe_boundary(input, target);
        if (!pos) continue;
        std::size_t const last = splits.empty() ? 0 : splits.back();
        if (*pos > last && *pos < input.size()) splits.push_back(*pos);
    }
    return splits;
}

[[nodiscard]] ChunkResult
parse_chunk(std::span<std::byte const> chunk, std::size_t chunk_start) {
    std::size_t const est_tags = chunk.size() / 128;
    std::size_t const est_text = est_tags / 2;
    ChunkResult r;
    r.tag_starts.reserve(est_tags);
    r.tag_ends.reserve(est_tags);
    r.tag_types.reserve(est_tags);
    r.tag_names.reserve(est_tags);
    r.text_ranges.reserve(est_text);

    std::size_t pos = 0;
    std::size_t last_tag_end = 0;

    while (true) {
        std::size_t const offset = find_byte('<', chunk, pos);
        if (offset == std::size_t(-1)) break;
        pos = offset;
        std::size_t const abs_pos = chunk_start + pos;
        std::size_t const tag_start = pos;

        // Text content between previous tag end and this tag start.
        std::size_t const text_start = (last_tag_end > 0) ? last_tag_end + 1 : 0;
        if (text_start < tag_start) {
            r.text_ranges.push_back(TextRange{
                static_cast<std::uint64_t>(chunk_start + text_start),
                static_cast<std::uint64_t>(abs_pos),
                U32_SENTINEL});  // placeholder — resolved during merge
        }

        if (pos + 1 >= chunk.size()) break;

        unsigned char const c1 = b_at(chunk, pos + 1);
        if (c1 == '/') {
            pos += 2;
            std::size_t const name_start = pos;
            while (pos < chunk.size() && b_at(chunk, pos) != '>' &&
                   !is_ws(b_at(chunk, pos))) {
                ++pos;
            }
            std::size_t const name_end = pos;
            std::size_t const gt = find_byte('>', chunk, pos);
            if (gt == std::size_t(-1)) break;
            pos = gt;
            r.tag_starts.push_back(static_cast<std::uint64_t>(abs_pos));
            r.tag_ends.push_back(static_cast<std::uint64_t>(chunk_start + pos));
            r.tag_types.push_back(TagType::Close);
            r.tag_names.emplace_back(static_cast<std::uint64_t>(chunk_start + name_start),
                                      static_cast<std::uint16_t>(name_end - name_start));
            last_tag_end = pos;
            ++pos;
        } else if (c1 == '!') {
            if (pos + 4 <= chunk.size() &&
                b_at(chunk, pos + 2) == '-' && b_at(chunk, pos + 3) == '-') {
                r.tag_starts.push_back(static_cast<std::uint64_t>(abs_pos));
                r.tag_types.push_back(TagType::Comment);
                r.tag_names.emplace_back(0, 0);
                pos += 4;
                while (true) {
                    std::size_t const dash = find_byte('-', chunk, pos);
                    if (dash == std::size_t(-1)) { pos = chunk.size(); break; }
                    pos = dash;
                    if (pos + 3 <= chunk.size() &&
                        b_at(chunk, pos) == '-' && b_at(chunk, pos + 1) == '-' &&
                        b_at(chunk, pos + 2) == '>') {
                        pos += 2;
                        break;
                    }
                    ++pos;
                }
                r.tag_ends.push_back(static_cast<std::uint64_t>(chunk_start + pos));
                last_tag_end = pos;
                ++pos;
            } else if (pos + 9 <= chunk.size() &&
                       b_at(chunk, pos + 2) == '[' && b_at(chunk, pos + 3) == 'C' &&
                       b_at(chunk, pos + 4) == 'D' && b_at(chunk, pos + 5) == 'A' &&
                       b_at(chunk, pos + 6) == 'T' && b_at(chunk, pos + 7) == 'A' &&
                       b_at(chunk, pos + 8) == '[') {
                r.tag_starts.push_back(static_cast<std::uint64_t>(abs_pos));
                r.tag_types.push_back(TagType::CData);
                r.tag_names.emplace_back(0, 0);
                pos += 9;
                std::size_t const content_start = pos;
                while (true) {
                    std::size_t const br = find_byte(']', chunk, pos);
                    if (br == std::size_t(-1)) break;
                    pos = br;
                    if (pos + 3 <= chunk.size() &&
                        b_at(chunk, pos) == ']' && b_at(chunk, pos + 1) == ']' &&
                        b_at(chunk, pos + 2) == '>') {
                        if (pos > content_start) {
                            r.text_ranges.push_back(TextRange{
                                static_cast<std::uint64_t>(chunk_start + content_start),
                                static_cast<std::uint64_t>(chunk_start + pos),
                                U32_SENTINEL});
                        }
                        pos += 2;
                        break;
                    }
                    ++pos;
                }
                r.tag_ends.push_back(static_cast<std::uint64_t>(chunk_start + pos));
                last_tag_end = pos;
                ++pos;
            } else {
                std::size_t const gt = find_byte('>', chunk, pos);
                if (gt != std::size_t(-1)) pos = gt;
                last_tag_end = pos;
                ++pos;
            }
        } else if (c1 == '?') {
            pos += 2;
            std::size_t const name_start = pos;
            while (pos < chunk.size() && b_at(chunk, pos) != '?' &&
                   b_at(chunk, pos) != '>' && !is_ws(b_at(chunk, pos))) {
                ++pos;
            }
            std::size_t const name_end = pos;
            r.tag_starts.push_back(static_cast<std::uint64_t>(abs_pos));
            r.tag_types.push_back(TagType::PI);
            r.tag_names.emplace_back(static_cast<std::uint64_t>(chunk_start + name_start),
                                      static_cast<std::uint16_t>(name_end - name_start));
            while (pos + 1 < chunk.size()) {
                if (b_at(chunk, pos) == '?' && b_at(chunk, pos + 1) == '>') {
                    ++pos;
                    break;
                }
                ++pos;
            }
            r.tag_ends.push_back(static_cast<std::uint64_t>(chunk_start + pos));
            last_tag_end = pos;
            ++pos;
        } else {
            pos += 1;
            std::size_t const name_start = pos;
            while (pos < chunk.size() && b_at(chunk, pos) != '>' &&
                   b_at(chunk, pos) != '/' && !is_ws(b_at(chunk, pos))) {
                ++pos;
            }
            std::size_t const name_end = pos;
            bool self_closing = false;
            while (pos < chunk.size() && b_at(chunk, pos) != '>') {
                if (b_at(chunk, pos) == '/' && pos + 1 < chunk.size() &&
                    b_at(chunk, pos + 1) == '>') {
                    self_closing = true;
                    ++pos;
                    break;
                }
                if (b_at(chunk, pos) == '"') {
                    ++pos;
                    std::size_t const q = find_byte('"', chunk, pos);
                    if (q != std::size_t(-1)) pos = q;
                } else if (b_at(chunk, pos) == '\'') {
                    ++pos;
                    std::size_t const q = find_byte('\'', chunk, pos);
                    if (q != std::size_t(-1)) pos = q;
                }
                ++pos;
            }
            if (pos >= chunk.size()) break;

            TagType const tt = self_closing ? TagType::SelfClose : TagType::Open;
            r.tag_starts.push_back(static_cast<std::uint64_t>(abs_pos));
            r.tag_ends.push_back(static_cast<std::uint64_t>(chunk_start + pos));
            r.tag_types.push_back(tt);
            r.tag_names.emplace_back(static_cast<std::uint64_t>(chunk_start + name_start),
                                      static_cast<std::uint16_t>(name_end - name_start));
            last_tag_end = pos;
            ++pos;
        }
    }
    return r;
}

[[nodiscard]] Result<XmlIndex>
merge_chunks(std::span<std::byte const> input, std::vector<ChunkResult> chunks) {
    std::size_t total_tags = 0, total_text = 0;
    for (auto const& c : chunks) {
        total_tags += c.tag_starts.size();
        total_text += c.text_ranges.size();
    }

    std::vector<std::uint64_t> tag_starts; tag_starts.reserve(total_tags);
    std::vector<std::uint64_t> tag_ends; tag_ends.reserve(total_tags);
    std::vector<TagType> tag_types; tag_types.reserve(total_tags);
    std::vector<std::pair<std::uint64_t, std::uint16_t>> tag_names;
    tag_names.reserve(total_tags);
    std::vector<TextRange> text_ranges; text_ranges.reserve(total_text);

    for (auto& c : chunks) {
        std::move(c.tag_starts.begin(), c.tag_starts.end(), std::back_inserter(tag_starts));
        std::move(c.tag_ends.begin(), c.tag_ends.end(), std::back_inserter(tag_ends));
        std::move(c.tag_types.begin(), c.tag_types.end(), std::back_inserter(tag_types));
        std::move(c.tag_names.begin(), c.tag_names.end(), std::back_inserter(tag_names));
        std::move(c.text_ranges.begin(), c.text_ranges.end(), std::back_inserter(text_ranges));
    }

    std::size_t const n = tag_types.size();
    std::vector<std::uint16_t> depths(n, 0);
    std::vector<std::uint32_t> parents(n, U32_SENTINEL);
    std::vector<std::uint32_t> close_map(n, U32_SENTINEL);
    std::vector<std::uint32_t> post_order(n, 0);

    std::uint16_t depth = 0;
    std::uint32_t post_counter = 0;
    std::size_t text_idx = 0;

    // constexpr std::size_t MAX_DEPTH = 4096;  // unused — silenced (depth limited by stack.size())
    std::vector<std::uint32_t> stack;
    stack.reserve(64);

    for (std::size_t i = 0; i < n; ++i) {
        std::uint64_t const tag_pos = tag_starts[i];
        std::uint32_t const current_parent = stack.empty() ? U32_SENTINEL
                                                            : stack.back();

        while (text_idx < text_ranges.size() &&
               text_ranges[text_idx].start < tag_pos) {
            text_ranges[text_idx].parent_tag = current_parent;
            ++text_idx;
        }

        switch (tag_types[i]) {
            case TagType::Close: {
                if (depth > 0) --depth;
                if (!stack.empty()) {
                    std::uint32_t const open_idx = stack.back();
                    stack.pop_back();
                    close_map[open_idx] = static_cast<std::uint32_t>(i);
                    post_order[open_idx] = post_counter;
                }
                post_order[i] = post_counter;
                ++post_counter;
                depths[i] = depth;
                parents[i] = stack.empty() ? U32_SENTINEL : stack.back();
                break;
            }
            case TagType::Open:
                depths[i] = depth;
                parents[i] = current_parent;
                stack.push_back(static_cast<std::uint32_t>(i));
                ++depth;
                break;
            default:
                if (tag_types[i] == TagType::SelfClose) {
                    close_map[i] = static_cast<std::uint32_t>(i);
                }
                post_order[i] = post_counter;
                ++post_counter;
                depths[i] = depth;
                parents[i] = current_parent;
                break;
        }
    }

    std::uint32_t const final_parent = stack.empty() ? U32_SENTINEL
                                                       : stack.back();
    while (text_idx < text_ranges.size()) {
        text_ranges[text_idx].parent_tag = final_parent;
        ++text_idx;
    }

    XmlIndex index(input);
    index.tag_starts = std::move(tag_starts);
    index.tag_ends = std::move(tag_ends);
    index.tag_types = std::move(tag_types);
    index.tag_names = std::move(tag_names);
    index.depths = std::move(depths);
    index.parents = std::move(parents);
    index.text_ranges = std::move(text_ranges);
    index.close_map = std::move(close_map);
    index.post_order = std::move(post_order);

    return index;
}

}  // namespace

Result<XmlIndex>
parse_parallel(std::span<std::byte const> input, std::size_t num_threads) {
    if (num_threads <= 1 || input.size() < MIN_PARALLEL_SIZE) {
        return parse_scalar(input);
    }
    std::size_t const t = std::max<std::size_t>(1, input.size() / (MIN_PARALLEL_SIZE / 2));
    num_threads = std::min(num_threads, t);

    auto const splits = find_split_points(input, num_threads);
    std::size_t const num_chunks = splits.size() + 1;

    std::vector<std::pair<std::size_t, std::size_t>> boundaries;
    boundaries.reserve(num_chunks);
    for (std::size_t i = 0; i < num_chunks; ++i) {
        std::size_t const s = (i == 0) ? 0 : splits[i - 1];
        std::size_t const e = (i < splits.size()) ? splits[i] : input.size();
        boundaries.emplace_back(s, e);
    }

    std::vector<ChunkResult> results(num_chunks);
    std::vector<std::jthread> threads;
    threads.reserve(num_chunks);
    for (std::size_t i = 0; i < num_chunks; ++i) {
        auto const [s, e] = boundaries[i];
        auto chunk = input.subspan(s, e - s);
        threads.emplace_back([&, i, chunk, s]() {
            results[i] = parse_chunk(chunk, s);
        });
    }
    threads.clear();  // joins all

    return merge_chunks(input, std::move(results));
}

Result<XmlIndex>
parse_parallel_indexed(std::span<std::byte const> input, std::size_t num_threads) {
    auto result = parse_parallel(input, num_threads);
    if (!result) return std::unexpected(result.error());
    if (result->tag_count() >= 64) {
        result->build_indices();
    }
    return result;
}

}  // namespace simdxml