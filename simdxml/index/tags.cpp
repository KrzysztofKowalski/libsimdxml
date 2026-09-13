// Tag-level attribute helpers — attribute parsing, namespace resolution.
//
// Ported from Rust `index/tags.rs` (`impl XmlIndex`). All methods scan raw
// bytes directly with `std::memchr` for zero-allocation hot paths.
#include "xml_index.hpp"

#include "../simd/dispatch.hpp"

#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rai::xml {

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

[[nodiscard]] std::string_view
slice(std::span<std::byte const> input, std::size_t start, std::size_t end) noexcept {
    if (start >= input.size() || end > input.size() || start > end) return {};
    return std::string_view(reinterpret_cast<char const*>(input.data() + start),
                             end - start);
}

#if SIMDXML_HAS_AVX2
#include "../simd/avx2.hpp"  // simd::detail::movemask_64_avx2

/// Class masks for one 64-byte block of tag bytes: '=', '"', '\''. Reads
/// exactly 64 bytes — the caller guarantees readability.
///
/// NOTE: the movemask pairs come from simd::detail::movemask_64_avx2 — a
/// target-attributed function. A lambda here would NOT inherit this
/// function's avx2 target and would fail the AVX-ABI check at codegen.
SIMDXML_TARGET_ISA("avx2") inline void
classify_tag_block_avx2(unsigned char const* p, std::uint64_t& eq,
                        std::uint64_t& dq, std::uint64_t& sq) noexcept {
    __m256i const v0 = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(p));
    __m256i const v1 = _mm256_loadu_si256(
        reinterpret_cast<__m256i const*>(p + 32));
    __m256i const c_eq = _mm256_set1_epi8(static_cast<char>('='));
    __m256i const c_dq = _mm256_set1_epi8(static_cast<char>('"'));
    __m256i const c_sq = _mm256_set1_epi8(static_cast<char>('\''));
    eq = simd::detail::movemask_64_avx2(_mm256_cmpeq_epi8(v0, c_eq),
                                        _mm256_cmpeq_epi8(v1, c_eq));
    dq = simd::detail::movemask_64_avx2(_mm256_cmpeq_epi8(v0, c_dq),
                                        _mm256_cmpeq_epi8(v1, c_dq));
    sq = simd::detail::movemask_64_avx2(_mm256_cmpeq_epi8(v0, c_sq),
                                        _mm256_cmpeq_epi8(v1, c_sq));
}

/// get_attribute over class masks: walks 64-byte blocks of the tag computing
/// '=', '"' and '\'' masks on the fly, iterates the '=' positions (each
/// attribute value separator) and jumps over values with the quote masks.
/// Byte-exact with the scalar loop it replaces: both visit full matches in
/// document order, and the scalar break conditions map 1:1 (a candidate '='
/// within one byte of the tag end ends the scan — later candidates could not
/// hold a quoted value either).
[[nodiscard]] std::optional<std::string_view>
get_attribute_avx2(std::span<std::byte const> input, std::size_t start,
                   std::size_t end, std::span<std::byte const> name) {
    std::size_t const tag_len = end - start;
    std::byte const* const tag = input.data() + start;
    std::size_t const readable = input.size() - start;

    // eq / dq / sq masks for the 64-byte block at tag-relative `bs`
    // (valid length `blen`; bits past it are cleared).
    auto const block_masks = [&](std::size_t bs, std::size_t blen)
        -> std::array<std::uint64_t, 3> {
        std::uint64_t eq = 0;
        std::uint64_t dq = 0;
        std::uint64_t sq = 0;
        if (readable - bs >= 64) {
            classify_tag_block_avx2(
                reinterpret_cast<unsigned char const*>(tag + bs), eq, dq, sq);
            if (blen < 64) {
                std::uint64_t const keep = (std::uint64_t{1} << blen) - 1;
                eq &= keep;
                dq &= keep;
                sq &= keep;
            }
        } else {
            // Final block at the end of the input buffer: byte loop.
            for (std::size_t i = 0; i < blen; ++i) {
                unsigned char const c =
                    std::to_integer<unsigned char>(tag[bs + i]);
                std::uint64_t const bit = (std::uint64_t{1} << i);
                if (c == '=') eq |= bit;
                else if (c == '"') dq |= bit;
                else if (c == '\'') sq |= bit;
            }
        }
        return {eq, dq, sq};
    };

    // First `quote` byte at or after tag-relative `from`, or npos.
    auto const find_quote = [&](unsigned char quote,
                                std::size_t from) -> std::size_t {
        for (std::size_t b = from / 64; b * 64 < tag_len; ++b) {
            std::size_t const bs = b * 64;
            std::size_t const blen =
                (tag_len - bs < 64) ? tag_len - bs : 64;
            auto const masks = block_masks(bs, blen);
            std::uint64_t const m0 = (quote == '"') ? masks[1] : masks[2];
            std::uint64_t m = m0;
            std::size_t const first_bit = from - bs;
            if (b == from / 64 && first_bit != 0) {
                m &= ~((std::uint64_t{1} << first_bit) - 1);
            }
            if (m != 0) {
                return bs + static_cast<std::size_t>(std::countr_zero(m));
            }
        }
        return std::size_t(-1);
    };

    for (std::size_t b = 0; b * 64 < tag_len; ++b) {
        std::size_t const bs = b * 64;
        std::size_t const blen = (tag_len - bs < 64) ? tag_len - bs : 64;
        auto const masks = block_masks(bs, blen);
        std::uint64_t m = masks[0];
        // The name must fit entirely before the '=': p >= name.size().
        if (name.size() >= bs + 64) continue;
        if (name.size() > bs) {
            m &= ~((std::uint64_t{1} << (name.size() - bs)) - 1);
        }
        while (m != 0) {
            std::size_t const bit =
                static_cast<std::size_t>(std::countr_zero(m));
            m &= m - 1;
            std::size_t const p = bs + bit;  // '=' position (tag-relative)
            if (p + 1 >= tag_len) return std::nullopt;
            std::size_t const pos = p - name.size();
            bool const boundary =
                (pos == 0) ||
                is_ws(std::to_integer<unsigned char>(tag[pos - 1]));
            bool match = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                if (tag[pos + i] != name[i]) {
                    match = false;
                    break;
                }
            }
            if (!match || !boundary) continue;
            std::size_t const val_start = p + 1;
            unsigned char const quote =
                std::to_integer<unsigned char>(tag[val_start]);
            if (quote != '"' && quote != '\'') continue;
            std::size_t const q = find_quote(quote, val_start + 1);
            if (q == std::size_t(-1)) continue;
            return slice(input, start + val_start + 1, start + q);
        }
    }
    return std::nullopt;
}
#endif

}  // namespace

std::optional<std::string_view>
XmlIndex::get_attribute(std::size_t tag_idx, std::string_view attr_name) const {
    if (tag_idx >= tag_count() || attr_name.empty()) return std::nullopt;
    auto const start = static_cast<std::size_t>(tag_starts[tag_idx]);
    auto const end = static_cast<std::size_t>(tag_ends[tag_idx]) + 1;
    if (end > input_.size()) return std::nullopt;
    auto const name = std::as_bytes(std::span{attr_name.data(), attr_name.size()});
    if (name.empty()) return std::nullopt;
#if SIMDXML_HAS_AVX2
    if (simd::has_avx2()) {
        return get_attribute_avx2(input_, start, end, name);
    }
#endif
    auto const tag_bytes = input_.subspan(start, end - start);
    unsigned char const first = static_cast<unsigned char>(
        std::to_integer<std::uint8_t>(name[0]));

    std::size_t pos = 0;
    while (true) {
        std::size_t const off = find_byte(first, tag_bytes, pos);
        if (off == std::size_t(-1)) break;
        pos = off;
        if (pos + name.size() + 1 >= tag_bytes.size()) break;

        bool match = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
            if (tag_bytes[pos + i] != name[i]) { match = false; break; }
        }
        unsigned char const sep = b_at(tag_bytes, pos + name.size());
        bool const boundary = (pos == 0) || is_ws(b_at(tag_bytes, pos - 1));
        if (match && sep == '=' && boundary) {
            std::size_t const val_start = pos + name.size() + 1;
            if (val_start >= tag_bytes.size()) break;
            unsigned char const quote = b_at(tag_bytes, val_start);
            if (quote == '"' || quote == '\'') {
                std::size_t const content_start = val_start + 1;
                std::size_t const q = find_byte(quote, tag_bytes, content_start);
                if (q != std::size_t(-1)) {
                    return slice(input_, start + content_start, start + q);
                }
            }
        }
        ++pos;
    }
    return std::nullopt;
}

std::vector<std::string_view>
XmlIndex::get_all_attribute_names(std::size_t tag_idx) const {
    std::vector<std::string_view> result;
    if (tag_idx >= tag_count()) return result;
    auto const start = static_cast<std::size_t>(tag_starts[tag_idx]);
    auto const end = static_cast<std::size_t>(tag_ends[tag_idx]) + 1;
    if (end > input_.size()) return result;
    auto const tag_bytes = input_.subspan(start, end - start);

    std::size_t pos = 1;  // skip '<'
    while (pos < tag_bytes.size() && b_at(tag_bytes, pos) != '>' &&
           b_at(tag_bytes, pos) != '/' && !is_ws(b_at(tag_bytes, pos))) {
        ++pos;
    }

    while (pos < tag_bytes.size() && b_at(tag_bytes, pos) != '>') {
        if (b_at(tag_bytes, pos) == '/') break;
        if (is_ws(b_at(tag_bytes, pos))) { ++pos; continue; }

        std::size_t const attr_name_start = pos;
        while (pos < tag_bytes.size() && b_at(tag_bytes, pos) != '=' &&
               b_at(tag_bytes, pos) != '>' && !is_ws(b_at(tag_bytes, pos))) {
            ++pos;
        }
        std::size_t const attr_name_end = pos;
        if (pos < tag_bytes.size() && b_at(tag_bytes, pos) == '=') {
            ++pos;
            if (pos < tag_bytes.size() &&
                (b_at(tag_bytes, pos) == '"' || b_at(tag_bytes, pos) == '\'')) {
                unsigned char const quote = b_at(tag_bytes, pos);
                ++pos;
                std::size_t const q = find_byte(quote, tag_bytes, pos);
                if (q != std::size_t(-1)) pos = q + 1;
                if (attr_name_end > attr_name_start) {
                    auto const name = slice(input_, start + attr_name_start,
                                            start + attr_name_end);
                    if (!name.starts_with("xmlns")) result.push_back(name);
                }
            } else {
                ++pos;
            }
        } else {
            ++pos;
        }
    }
    return result;
}

std::vector<std::pair<std::string_view, std::string_view>>
XmlIndex::attributes(std::size_t tag_idx) const {
    std::vector<std::pair<std::string_view, std::string_view>> result;
    if (tag_idx >= tag_count()) return result;
    auto const start = static_cast<std::size_t>(tag_starts[tag_idx]);
    auto const end = static_cast<std::size_t>(tag_ends[tag_idx]) + 1;
    if (end > input_.size()) return result;
    auto const tag_bytes = input_.subspan(start, end - start);

    std::size_t pos = 1;
    while (pos < tag_bytes.size() && b_at(tag_bytes, pos) != '>' &&
           b_at(tag_bytes, pos) != '/' && !is_ws(b_at(tag_bytes, pos))) {
        ++pos;
    }

    while (pos < tag_bytes.size() && b_at(tag_bytes, pos) != '>') {
        if (b_at(tag_bytes, pos) == '/') break;
        if (is_ws(b_at(tag_bytes, pos))) { ++pos; continue; }

        std::size_t const attr_name_start = pos;
        while (pos < tag_bytes.size() && b_at(tag_bytes, pos) != '=' &&
               b_at(tag_bytes, pos) != '>' && !is_ws(b_at(tag_bytes, pos))) {
            ++pos;
        }
        std::size_t const attr_name_end = pos;
        if (pos < tag_bytes.size() && b_at(tag_bytes, pos) == '=') {
            ++pos;
            if (pos < tag_bytes.size() &&
                (b_at(tag_bytes, pos) == '"' || b_at(tag_bytes, pos) == '\'')) {
                unsigned char const quote = b_at(tag_bytes, pos);
                ++pos;
                std::size_t const val_start = pos;
                std::size_t const q = find_byte(quote, tag_bytes, pos);
                if (q == std::size_t(-1)) break;
                std::size_t const val_end = q;
                pos = val_end + 1;
                if (attr_name_end > attr_name_start) {
                    auto const name = slice(input_, start + attr_name_start,
                                            start + attr_name_end);
                    auto const value = slice(input_, start + val_start,
                                              start + val_end);
                    result.emplace_back(name, value);
                }
            } else {
                ++pos;
            }
        } else {
            ++pos;
        }
    }
    return result;
}

std::vector<std::pair<std::string_view, std::string_view>>
XmlIndex::get_namespace_decls(std::size_t tag_idx) const {
    std::vector<std::pair<std::string_view, std::string_view>> result;
    if (tag_idx >= tag_count()) return result;
    auto const start = static_cast<std::size_t>(tag_starts[tag_idx]);
    auto const end = static_cast<std::size_t>(tag_ends[tag_idx]) + 1;
    if (end > input_.size()) return result;
    auto const tag_str = slice(input_, start, end);
    if (tag_str.empty()) return result;

    std::size_t pos = 0;
    while (pos < tag_str.size()) {
        auto const idx = tag_str.find("xmlns:", pos);
        if (idx == std::string_view::npos) break;
        std::size_t const abs_idx = pos + idx;
        auto const after = tag_str.substr(abs_idx + 6);
        auto const eq = after.find('=');
        if (eq == std::string_view::npos) { pos = abs_idx + 6; continue; }
        auto const prefix = after.substr(0, eq);
        auto rest = after.substr(eq + 1);
        char quote = 0;
        if (!rest.empty() && (rest.front() == '"' || rest.front() == '\'')) {
            quote = rest.front();
            rest = rest.substr(1);
        } else {
            pos = abs_idx + 6;
            continue;
        }
        auto const end_q = rest.find(quote);
        if (end_q == std::string_view::npos) { pos = abs_idx + 6; continue; }
        auto const uri = rest.substr(0, end_q);
        std::size_t const prefix_offset = start + abs_idx + 6;
        std::size_t const uri_offset = start + abs_idx + 6 + eq + 2;
        result.emplace_back(
            slice(input_, prefix_offset, prefix_offset + prefix.size()),
            slice(input_, uri_offset, uri_offset + uri.size()));
        pos = abs_idx + 6 + eq + 2 + end_q + 1;
    }
    return result;
}

}  // namespace rai::xml