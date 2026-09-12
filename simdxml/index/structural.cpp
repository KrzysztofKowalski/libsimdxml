// Implementation of structural parsers.
//
// Ported from Rust `index/structural.rs` (`parse_scalar`, `parse_two_stage`).
// Both build an `XmlIndex` from XML bytes via different scan strategies.
#include "structural.hpp"

#include "../simdxml.hpp"
#include "../simd/dispatch.hpp"
#include "error.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace simdxml {

namespace {

[[nodiscard]] inline unsigned char b_at(std::span<std::byte const> input,
                                         std::size_t i) noexcept {
    return static_cast<unsigned char>(
        std::to_integer<std::uint8_t>(input[i]));
}

[[nodiscard]] inline bool
is_ws(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/// memchr helper — find first occurrence of `needle` in `haystack`.
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

XmlIndex make_empty_index(std::span<std::byte const> input) {
    std::size_t const est_tags = input.size() / 128;
    std::size_t const est_text = est_tags / 2;
    return XmlIndex(input, est_tags, est_text);
}

/// Forward cursor over a class-mask bitset (one u64 per 64-byte chunk).
/// `next(from)` returns the first set bit at or after `from`, walking the
/// current 64-bit word with countr_zero and advancing to the next word when
/// exhausted. Callers pass monotonically advancing `from` values (a parser
/// property), so the cursor never needs to rewind.
struct MaskCursor {
    std::vector<std::uint64_t> const* bits = nullptr;
    std::size_t chunk = 0;
    std::uint64_t word = 0;

    void init(std::vector<std::uint64_t> const& b) noexcept {
        bits = &b;
        chunk = 0;
        word = b.empty() ? 0 : b[0];
    }

    [[nodiscard]] std::size_t next(std::size_t from) noexcept {
        std::size_t const nchunks = bits->size();
        while (chunk < nchunks && (chunk + 1) * 64 <= from) {
            ++chunk;
            word = (*bits)[chunk];
        }
        if (chunk >= nchunks) return std::size_t(-1);
        std::size_t const bit_from = from - chunk * 64;  // < 64 by the loop above
        if (bit_from != 0) word &= ~((std::uint64_t{1} << bit_from) - 1);
        while (word == 0) {
            ++chunk;
            if (chunk >= nchunks) return std::size_t(-1);
            word = (*bits)[chunk];
        }
        std::size_t const bit = static_cast<std::size_t>(std::countr_zero(word));
        word &= word - 1;  // consume the returned bit
        return chunk * 64 + bit;
    }
};

/// First position p >= from where a two-byte run of the `single_bits`
/// character is followed by '>' — i.e. the "-->" comment terminator or the
/// "]]>" CDATA terminator — or std::size_t(-1). `last_single` receives the
/// position of the LAST `single_bits` byte at or after `from`, or
/// std::size_t(-1) (the CDATA no-terminator fallback leaves `pos` just past
/// that byte).
///
/// Bit-exact with the memchr loops it replaces: mask bits only exist for
/// real input bytes, so candidates never point past the end of input and
/// the first candidate equals the first memchr-verified match (including
/// runs straddling 64-byte chunk seams, handled explicitly below).
[[nodiscard]] std::size_t
find_pair_gt(std::vector<std::uint64_t> const& single_bits,
             std::vector<std::uint64_t> const& gt_bits, std::size_t from,
             std::size_t& last_single) noexcept {
    last_single = std::size_t(-1);
    std::size_t const nchunks = single_bits.size();
    if (nchunks == 0 || from / 64 >= nchunks) return std::size_t(-1);
    std::size_t const first_chunk = from / 64;
    std::size_t const first_bit = from % 64;
    std::uint64_t const skip =
        (first_bit != 0) ? (std::uint64_t{1} << first_bit) - 1 : 0;

    for (std::size_t chunk = first_chunk; chunk < nchunks; ++chunk) {
        std::uint64_t const single = (chunk == first_chunk)
                                         ? single_bits[chunk] & ~skip
                                         : single_bits[chunk];
        std::uint64_t const gt = gt_bits[chunk];

        // In-chunk candidates: p, p+1, p+2 all inside this 64-bit word.
        std::uint64_t cand = single & (single >> 1) & (gt >> 2);
        if (chunk + 1 < nchunks) {
            std::uint64_t const single_next = single_bits[chunk + 1];
            std::uint64_t const gt_next = gt_bits[chunk + 1];
            // p = 62: both run bytes here, '>' in the next word.
            if (((single >> 62) & (single >> 63) & 1) != 0 &&
                (gt_next & 1) != 0) {
                cand |= (std::uint64_t{1} << 62);
            }
            // p = 63: second run byte and '>' live in the next word.
            if (((single >> 63) & 1) != 0 && (single_next & 1) != 0 &&
                ((gt_next >> 1) & 1) != 0) {
                cand |= (std::uint64_t{1} << 63);
            }
        }
        if (chunk == first_chunk && first_bit != 0) cand &= ~skip;
        if (single != 0) {
            last_single = chunk * 64 + static_cast<std::size_t>(
                                           63 - std::countl_zero(single));
        }
        if (cand != 0) {
            return chunk * 64 +
                   static_cast<std::size_t>(std::countr_zero(cand));
        }
    }
    return std::size_t(-1);
}

/// First position p >= from with input[p] == '?' and input[p+1] == '>' (the
/// `?>` PI terminator), or std::size_t(-1). Byte-exact with the memchr walk
/// it replaces, including runs straddling a 64-byte chunk seam.
[[nodiscard]] std::size_t
find_qmark_gt(std::vector<std::uint64_t> const& qmark_bits,
              std::vector<std::uint64_t> const& gt_bits,
              std::size_t from) noexcept {
    std::size_t const nchunks = qmark_bits.size();
    if (nchunks == 0 || from / 64 >= nchunks) return std::size_t(-1);
    std::size_t const first_chunk = from / 64;
    std::size_t const first_bit = from % 64;
    std::uint64_t const skip =
        (first_bit != 0) ? (std::uint64_t{1} << first_bit) - 1 : 0;

    for (std::size_t chunk = first_chunk; chunk < nchunks; ++chunk) {
        std::uint64_t const qm = (chunk == first_chunk)
                                     ? qmark_bits[chunk] & ~skip
                                     : qmark_bits[chunk];
        std::uint64_t const gt = gt_bits[chunk];

        std::uint64_t cand = qm & (gt >> 1);
        if (chunk + 1 < nchunks && ((qm >> 63) & 1) != 0 &&
            (gt_bits[chunk + 1] & 1) != 0) {
            cand |= (std::uint64_t{1} << 63);  // '?' at 63, '>' in the next word
        }
        if (chunk == first_chunk && first_bit != 0) cand &= ~skip;
        if (cand != 0) {
            return chunk * 64 +
                   static_cast<std::size_t>(std::countr_zero(cand));
        }
    }
    return std::size_t(-1);
}

}  // namespace

Result<XmlIndex> parse_scalar(std::span<std::byte const> input) {
    // --- SIMD-dispatched tag discovery -----------------------------------
    // The outer loop used to memchr-scan the whole document for the next
    // '<' byte. classify_structural() (runtime dispatch: AVX2 > SSE4.2 >
    // NEON > scalar) computes the *raw* — quote-agnostic — position sets of
    // '<' and '>' in one vectorized pass. Those sets are byte-exact (every
    // such byte, no quote/comment/CDATA filtering), so the parse result is
    // bit-identical to the memchr scanner: the scalar state machine below
    // (quotes / comments / CDATA / PI / DOCTYPE) is untouched, only the
    // byte lookups are fed from the SIMD index.
    // On a CPU with no SIMD backend we skip the extra classify pass and
    // keep the pure memchr scanner (zero regression).
    bool const use_simd =
        simd::has_avx2() || simd::has_sse42() || simd::has_neon();

    // Classify up front: the popcounts of the raw masks size every array
    // this parser pushes to. n_lt is a true upper bound for the six tag
    // arrays (each entry corresponds to one '<' byte); n_gt is the report's
    // capacity hint for text_ranges (ranges correlate with '>' bytes and
    // only in a rare text-then-CDATA corner can they exceed it — reserve()
    // is a hint, so that just costs one growth, never correctness). The raw
    // variant skips the quote-masking pass: the state machine below owns
    // quotes, so only the raw byte positions are needed (2 mask vectors,
    // not 4).
    simd::StructuralIndex structural;
    std::size_t n_lt = 0;
    std::size_t n_gt = 0;
    if (use_simd) {
        structural = simd::classify_structural_raw(input);
        for (std::uint64_t const bits : structural.lt_raw_bits) {
            n_lt += static_cast<std::size_t>(std::popcount(bits));
        }
        for (std::uint64_t const bits : structural.gt_raw_bits) {
            n_gt += static_cast<std::size_t>(std::popcount(bits));
        }
    }

    XmlIndex index = use_simd ? XmlIndex(input, n_lt, n_gt)
                              : make_empty_index(input);
    std::size_t pos = 0;
    std::uint16_t depth = 0;
    std::size_t last_tag_end = 0;

    constexpr std::size_t MAX_DEPTH = 32;
    std::array<std::uint32_t, MAX_DEPTH> pstack{};
    std::size_t stop = 0;

    // Mask-word cursors over the raw bitsets. countr_zero walks the set
    // bits of the current 64-byte chunk; an exhausted word advances to the
    // next chunk. No intermediate position vector is materialized.
    MaskCursor lt_cur;
    MaskCursor gt_cur;
    MaskCursor dq_cur;
    MaskCursor sq_cur;
    if (use_simd) {
        lt_cur.init(structural.lt_raw_bits);
        gt_cur.init(structural.gt_raw_bits);
        dq_cur.init(structural.dq_bits);
        sq_cur.init(structural.sq_bits);
    }

    // Next '<' / '>' / quote byte at or after `from`. The `from` arguments
    // advance monotonically through the document in every branch of the
    // parser, so the cursors never need to move backwards.
    auto const next_lt = [&](std::size_t from) noexcept -> std::size_t {
        return use_simd ? lt_cur.next(from) : find_byte('<', input, from);
    };
    auto const next_gt = [&](std::size_t from) noexcept -> std::size_t {
        return use_simd ? gt_cur.next(from) : find_byte('>', input, from);
    };
    // Attribute-value terminators straight from the classify masks (the
    // same byte positions a memchr would find — the raw masks record every
    // quote byte, quote-state agnostic).
    auto const next_dq = [&](std::size_t from) noexcept -> std::size_t {
        return use_simd ? dq_cur.next(from) : find_byte('"', input, from);
    };
    auto const next_sq = [&](std::size_t from) noexcept -> std::size_t {
        return use_simd ? sq_cur.next(from) : find_byte('\'', input, from);
    };
    // First position >= from where any of the selected name-delimiter
    // classes (whitespace, '>', and optionally '/' or '?') is set —
    // countr_zero on the OR-ed class-mask words instead of a byte loop.
    // std::size_t(-1) means "no delimiter until end of input"; callers then
    // treat the name as running to the end, exactly like the byte loops did.
    auto const next_delim = [&](std::size_t from, bool use_slash,
                                bool use_qmark) noexcept -> std::size_t {
        if (!use_simd) {
            while (from < input.size()) {
                unsigned char const b = b_at(input, from);
                if (b == '>' || is_ws(b) || (use_slash && b == '/') ||
                    (use_qmark && b == '?')) {
                    break;
                }
                ++from;
            }
            return (from < input.size()) ? from : std::size_t(-1);
        }
        std::size_t const nchunks = structural.ws_bits.size();
        if (nchunks == 0 || from / 64 >= nchunks) return std::size_t(-1);
        std::size_t const first_chunk = from / 64;
        std::size_t const first_bit = from % 64;
        std::uint64_t const skip =
            (first_bit != 0) ? (std::uint64_t{1} << first_bit) - 1 : 0;
        for (std::size_t chunk = first_chunk; chunk < nchunks; ++chunk) {
            std::uint64_t m =
                structural.ws_bits[chunk] | structural.gt_raw_bits[chunk];
            if (use_slash) m |= structural.slash_bits[chunk];
            if (use_qmark) m |= structural.qmark_bits[chunk];
            if (chunk == first_chunk && first_bit != 0) m &= ~skip;
            if (m != 0) {
                return chunk * 64 +
                       static_cast<std::size_t>(std::countr_zero(m));
            }
        }
        return std::size_t(-1);
    };
    while (true) {
        std::size_t const offset = next_lt(pos);
        if (offset == std::size_t(-1)) break;
        pos = offset;

        std::uint32_t const cp = (stop == 0) ? UINT32_MAX
                                  : (stop <= MAX_DEPTH) ? pstack[stop - 1]
                                                        : UINT32_MAX;

        // Text content between previous tag end and this tag start.
        std::size_t const text_start = (last_tag_end > 0) ? last_tag_end + 1 : 0;
        if (text_start < pos) {
            index.text_ranges.push_back(TextRange{
                static_cast<std::uint64_t>(text_start),
                static_cast<std::uint64_t>(pos),
                cp});
        }

        std::size_t const tag_start = pos;
        if (pos + 1 >= input.size()) {
            return std::unexpected(SimdXmlError::unclosed_tag(pos));
        }

        unsigned char const c1 = b_at(input, pos + 1);
        if (c1 == '/') {
            // Close tag.
            pos += 2;
            std::size_t const name_start = pos;
            std::size_t const delim = next_delim(pos, false, false);
            std::size_t const name_end =
                (delim == std::size_t(-1)) ? input.size() : delim;
            pos = name_end;
            // A close tag usually runs straight into '>' — the delimiter
            // byte itself is then the terminator and no second scan is
            // needed. Otherwise search the '>' in the current chunk word
            // first, cursor fallback across a chunk seam (same byte the
            // cursor would return either way).
            std::size_t gt = std::size_t(-1);
            if (delim != std::size_t(-1) && b_at(input, delim) == '>') {
                gt = delim;
            } else {
                std::size_t const gt_chunk = pos / 64;
                if (gt_chunk < structural.gt_raw_bits.size()) {
                    std::uint64_t const w = structural.gt_raw_bits[gt_chunk] &
                        ~(((std::uint64_t{1} << (pos % 64)) - 1));
                    if (w != 0) {
                        gt = gt_chunk * 64 +
                             static_cast<std::size_t>(std::countr_zero(w));
                    }
                }
                if (gt == std::size_t(-1)) gt = next_gt(pos);
            }
            if (gt == std::size_t(-1)) {
                return std::unexpected(SimdXmlError::unclosed_tag(tag_start));
            }
            pos = gt;

            if (depth > 0) --depth;
            if (stop > 0) --stop;

            std::uint32_t const parent = (stop == 0) ? UINT32_MAX
                                         : (stop <= MAX_DEPTH) ? pstack[stop - 1]
                                                               : UINT32_MAX;

            index.tag_starts.push_back(static_cast<std::uint64_t>(tag_start));
            index.tag_ends.push_back(static_cast<std::uint64_t>(pos));
            index.tag_types.push_back(TagType::Close);
            index.tag_names.emplace_back(static_cast<std::uint64_t>(name_start),
                                         static_cast<std::uint16_t>(name_end - name_start));
            index.depths.push_back(depth);
            index.parents.push_back(parent);
            last_tag_end = pos;
            ++pos;
        } else if (c1 == '!') {
            // Comment, CDATA, or DOCTYPE.
            if (pos + 4 <= input.size() &&
                b_at(input, pos + 2) == '-' && b_at(input, pos + 3) == '-') {
                // Comment.
                std::uint32_t const parent = cp;
                index.tag_starts.push_back(static_cast<std::uint64_t>(tag_start));
                index.tag_types.push_back(TagType::Comment);
                index.tag_names.emplace_back(0, 0);
                index.depths.push_back(depth);
                index.parents.push_back(parent);

                pos += 4;
                if (use_simd) {
                    // "-->" straight from the dash/gt mask words:
                    // dash & (dash >> 1) & (gt >> 2) marks every `-->` start.
                    std::size_t last_dash = 0;
                    std::size_t const end = find_pair_gt(
                        structural.dash_bits, structural.gt_raw_bits, pos,
                        last_dash);
                    pos = (end == std::size_t(-1)) ? input.size() : end + 2;
                } else {
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
                }
                index.tag_ends.push_back(static_cast<std::uint64_t>(pos));
                last_tag_end = pos;
                ++pos;
            } else if (pos + 9 <= input.size() &&
                       b_at(input, pos + 2) == '[' && b_at(input, pos + 3) == 'C' &&
                       b_at(input, pos + 4) == 'D' && b_at(input, pos + 5) == 'A' &&
                       b_at(input, pos + 6) == 'T' && b_at(input, pos + 7) == 'A' &&
                       b_at(input, pos + 8) == '[') {
                // CDATA.
                std::uint32_t const parent = cp;
                index.tag_starts.push_back(static_cast<std::uint64_t>(tag_start));
                index.tag_types.push_back(TagType::CData);
                index.tag_names.emplace_back(0, 0);
                index.depths.push_back(depth);
                index.parents.push_back(parent);

                pos += 9;
                std::size_t const content_start = pos;
                if (use_simd) {
                    // "]]>" straight from the rbrack/gt mask words.
                    // No terminator: the memchr loop leaves `pos` just past
                    // the last ']' (or at content_start) — replicated via
                    // `last_rbrack`.
                    std::size_t last_rbrack = 0;
                    std::size_t const end = find_pair_gt(
                        structural.rbrack_bits, structural.gt_raw_bits, pos,
                        last_rbrack);
                    if (end != std::size_t(-1)) {
                        if (end > content_start) {
                            index.text_ranges.push_back(TextRange{
                                static_cast<std::uint64_t>(content_start),
                                static_cast<std::uint64_t>(end),
                                parent});
                        }
                        pos = end + 2;
                    } else if (last_rbrack != std::size_t(-1)) {
                        pos = last_rbrack + 1;
                    }
                } else {
                    while (true) {
                        std::size_t const br = find_byte(']', input, pos);
                        if (br == std::size_t(-1)) break;
                        pos = br;
                        if (pos + 3 <= input.size() &&
                            b_at(input, pos) == ']' && b_at(input, pos + 1) == ']' &&
                            b_at(input, pos + 2) == '>') {
                            if (pos > content_start) {
                                index.text_ranges.push_back(TextRange{
                                    static_cast<std::uint64_t>(content_start),
                                    static_cast<std::uint64_t>(pos),
                                    parent});
                            }
                            pos += 2;
                            break;
                        }
                        ++pos;
                    }
                }
                index.tag_ends.push_back(static_cast<std::uint64_t>(pos));
                last_tag_end = pos;
                ++pos;
            } else {
                // DOCTYPE or other — skip.
                std::size_t const gt = next_gt(pos);
                if (gt != std::size_t(-1)) pos = gt;
                last_tag_end = pos;
                ++pos;
            }
        } else if (c1 == '?') {
            // Processing instruction.
            std::uint32_t const parent = cp;
            pos += 2;
            std::size_t const name_start = pos;
            std::size_t const pi_delim = next_delim(pos, false, true);
            std::size_t const name_end = (pi_delim == std::size_t(-1))
                                             ? input.size()
                                             : pi_delim;
            pos = name_end;
            index.tag_starts.push_back(static_cast<std::uint64_t>(tag_start));
            index.tag_types.push_back(TagType::PI);
            index.tag_names.emplace_back(static_cast<std::uint64_t>(name_start),
                                         static_cast<std::uint16_t>(name_end - name_start));
            index.depths.push_back(depth);
            index.parents.push_back(parent);
            if (use_simd) {
                // "?>" straight from the qmark/gt mask words. No terminator:
                // the byte walk leaves `pos` at input.size() - 1 (or
                // unchanged when it never entered the loop).
                std::size_t const p = find_qmark_gt(structural.qmark_bits,
                                                    structural.gt_raw_bits, pos);
                if (p != std::size_t(-1)) {
                    pos = p + 1;
                } else if (pos + 1 < input.size()) {
                    pos = input.size() - 1;
                }
            } else {
                while (pos + 1 < input.size()) {
                    if (b_at(input, pos) == '?' && b_at(input, pos + 1) == '>') {
                        ++pos;
                        break;
                    }
                    ++pos;
                }
            }
            index.tag_ends.push_back(static_cast<std::uint64_t>(pos));
            last_tag_end = pos;
            ++pos;
        } else {
            // Open or self-closing tag.
            std::uint32_t const parent = cp;
            pos += 1;
            std::size_t const name_start = pos;
            std::size_t const open_delim = next_delim(pos, true, false);
            std::size_t const name_end = (open_delim == std::size_t(-1))
                                             ? input.size()
                                             : open_delim;
            pos = name_end;

            // Attribute region, word-at-a-time: instead of one cursor call
            // per event (each re-deriving the chunk and re-OR-ing the four
            // masks), walk the OR-ed event mask one 64-bit word per chunk
            // and classify every event byte in place. countr_zero over the
            // word yields events in byte order, so the event sequence — and
            // the resulting pos sequence — is exactly what the per-event
            // cursor produced. Attribute values are skipped with the quote
            // masks: the closing quote is searched in the current word
            // first, cursor fallback across a chunk seam. The nesting stack
            // above stays scalar.
            bool self_closing = false;
            if (use_simd) {
                std::size_t const nchunks = structural.gt_raw_bits.size();
                bool terminated = false;
                while (!terminated) {
                    if (pos >= input.size()) break;
                    std::size_t const chunk = pos / 64;
                    if (chunk >= nchunks) {
                        pos = input.size();
                        break;
                    }
                    std::uint64_t m = structural.gt_raw_bits[chunk] |
                                      structural.slash_bits[chunk] |
                                      structural.dq_bits[chunk] |
                                      structural.sq_bits[chunk];
                    std::size_t const bit0 = pos - chunk * 64;
                    if (bit0 != 0) {
                        m &= ~((std::uint64_t{1} << bit0) - 1);
                    }
                    bool jumped_seam = false;
                    while (m != 0) {
                        std::size_t const e_bit = std::countr_zero(m);
                        m &= m - 1;
                        pos = chunk * 64 + e_bit;
                        unsigned char const eb = b_at(input, pos);
                        if (eb == '>') {
                            terminated = true;  // normal tag end
                            break;
                        }
                        if (eb == '/') {
                            if (pos + 1 < input.size() &&
                                b_at(input, pos + 1) == '>') {
                                self_closing = true;
                                ++pos;
                                terminated = true;
                                break;
                            }
                            ++pos;  // stray '/' inside the tag body
                            continue;
                        }
                        // '"' or '\'': jump to the closing quote. Found ->
                        // the scan resumes after it; missing -> the scan
                        // skips one byte past the opening quote (quirk kept
                        // for bit-exactness).
                        std::uint64_t const* qbits = (eb == '"')
                            ? structural.dq_bits.data()
                            : structural.sq_bits.data();
                        std::size_t q = std::size_t(-1);
                        std::size_t const sbit = pos + 1 - chunk * 64;
                        if (sbit < 64) {
                            std::uint64_t const w =
                                qbits[chunk] &
                                ~((std::uint64_t{1} << sbit) - 1);
                            if (w != 0) {
                                q = chunk * 64 +
                                    static_cast<std::size_t>(std::countr_zero(w));
                            }
                        }
                        if (q == std::size_t(-1)) {
                            q = (eb == '"') ? next_dq(pos + 1) : next_sq(pos + 1);
                        }
                        pos = (q != std::size_t(-1)) ? q + 1 : pos + 2;
                        if (pos >= input.size() || pos >= (chunk + 1) * 64) {
                            jumped_seam = true;  // recompute the chunk from pos
                            break;
                        }
                        // kill event bits inside the skipped value (bits
                        // strictly below the resumed position)
                        m &= ~((std::uint64_t{1} << (pos - chunk * 64)) - 1);
                    }
                    if (terminated) break;
                    if (!jumped_seam) {
                        pos = (chunk + 1) * 64;  // no events left in this word
                    }
                }
                if (!terminated) pos = input.size();
            } else {
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
            }
            if (pos >= input.size()) {
                return std::unexpected(SimdXmlError::unclosed_tag(tag_start));
            }

            TagType const tt = self_closing ? TagType::SelfClose : TagType::Open;
            std::uint32_t const tag_idx = static_cast<std::uint32_t>(index.tag_starts.size());

            index.tag_starts.push_back(static_cast<std::uint64_t>(tag_start));
            index.tag_ends.push_back(static_cast<std::uint64_t>(pos));
            index.tag_types.push_back(tt);
            index.tag_names.emplace_back(static_cast<std::uint64_t>(name_start),
                                         static_cast<std::uint16_t>(name_end - name_start));
            index.depths.push_back(depth);
            index.parents.push_back(parent);

            if (tt == TagType::Open) {
                if (stop < MAX_DEPTH) pstack[stop] = tag_idx;
                ++stop;
                ++depth;
            }
            last_tag_end = pos;
            ++pos;
        }
    }
    return index;
}

Result<XmlIndex> parse_two_stage(std::span<std::byte const> input) {
    auto structural = simd::classify_structural(input);
    XmlIndex index = make_empty_index(input);

    std::uint16_t depth = 0;
    std::vector<std::uint32_t> parent_stack;
    parent_stack.reserve(64);
    std::size_t last_tag_end = 0;

    std::vector<std::size_t> gt_positions = structural.gt_positions();
    std::size_t gt_idx = 0;

    for (auto const lt_pos : structural.lt_positions()) {
        std::size_t const text_start = (last_tag_end > 0) ? last_tag_end + 1 : 0;
        if (text_start < lt_pos) {
            std::uint32_t const p = parent_stack.empty() ? UINT32_MAX
                                                          : parent_stack.back();
            index.text_ranges.push_back(TextRange{
                static_cast<std::uint64_t>(text_start),
                static_cast<std::uint64_t>(lt_pos), p});
        }

        std::size_t const tag_start = lt_pos;
        if (tag_start + 1 >= input.size()) break;

        // Find the matching '>' for this '<'.
        while (gt_idx < gt_positions.size() && gt_positions[gt_idx] <= lt_pos) ++gt_idx;
        if (gt_idx >= gt_positions.size()) {
            return std::unexpected(SimdXmlError::unclosed_tag(tag_start));
        }
        std::size_t const gt_pos = gt_positions[gt_idx];

        unsigned char const c1 = b_at(input, tag_start + 1);
        if (c1 == '/') {
            std::size_t const name_start = tag_start + 2;
            std::size_t name_end = name_start;
            while (name_end < gt_pos && !is_ws(b_at(input, name_end))) ++name_end;
            if (depth > 0) --depth;
            if (!parent_stack.empty()) parent_stack.pop_back();

            std::uint32_t const p = parent_stack.empty() ? UINT32_MAX
                                                         : parent_stack.back();
            index.tag_starts.push_back(static_cast<std::uint64_t>(tag_start));
            index.tag_ends.push_back(static_cast<std::uint64_t>(gt_pos));
            index.tag_types.push_back(TagType::Close);
            index.tag_names.emplace_back(static_cast<std::uint64_t>(name_start),
                                         static_cast<std::uint16_t>(name_end - name_start));
            index.depths.push_back(depth);
            index.parents.push_back(p);
            last_tag_end = gt_pos;
        } else if (c1 == '!') {
            if (tag_start + 4 <= input.size() &&
                b_at(input, tag_start + 2) == '-' &&
                b_at(input, tag_start + 3) == '-') {
                // Comment.
                std::size_t end = tag_start + 4;
                while (end + 3 <= input.size()) {
                    if (b_at(input, end) == '-' && b_at(input, end + 1) == '-' &&
                        b_at(input, end + 2) == '>') {
                        end += 2;
                        break;
                    }
                    ++end;
                }
                std::uint32_t const p = parent_stack.empty() ? UINT32_MAX
                                                              : parent_stack.back();
                index.tag_starts.push_back(static_cast<std::uint64_t>(tag_start));
                index.tag_ends.push_back(static_cast<std::uint64_t>(end));
                index.tag_types.push_back(TagType::Comment);
                index.tag_names.emplace_back(0, 0);
                index.depths.push_back(depth);
                index.parents.push_back(p);
                last_tag_end = end;
                while (gt_idx < gt_positions.size() && gt_positions[gt_idx] <= end) ++gt_idx;
            } else if (tag_start + 9 <= input.size() &&
                       b_at(input, tag_start + 2) == '[' &&
                       b_at(input, tag_start + 3) == 'C' &&
                       b_at(input, tag_start + 4) == 'D' &&
                       b_at(input, tag_start + 5) == 'A' &&
                       b_at(input, tag_start + 6) == 'T' &&
                       b_at(input, tag_start + 7) == 'A' &&
                       b_at(input, tag_start + 8) == '[') {
                // CDATA.
                std::size_t const content_start = tag_start + 9;
                std::size_t end = content_start;
                while (end + 3 <= input.size()) {
                    if (b_at(input, end) == ']' && b_at(input, end + 1) == ']' &&
                        b_at(input, end + 2) == '>') {
                        std::uint32_t const p = parent_stack.empty() ? UINT32_MAX
                                                                      : parent_stack.back();
                        if (end > content_start) {
                            index.text_ranges.push_back(TextRange{
                                static_cast<std::uint64_t>(content_start),
                                static_cast<std::uint64_t>(end), p});
                        }
                        end += 2;
                        break;
                    }
                    ++end;
                }
                std::uint32_t const p = parent_stack.empty() ? UINT32_MAX
                                                              : parent_stack.back();
                index.tag_starts.push_back(static_cast<std::uint64_t>(tag_start));
                index.tag_ends.push_back(static_cast<std::uint64_t>(end));
                index.tag_types.push_back(TagType::CData);
                index.tag_names.emplace_back(0, 0);
                index.depths.push_back(depth);
                index.parents.push_back(p);
                last_tag_end = end;
                while (gt_idx < gt_positions.size() && gt_positions[gt_idx] <= end) ++gt_idx;
            } else {
                // DOCTYPE — skip.
                last_tag_end = gt_pos;
            }
        } else if (c1 == '?') {
            std::size_t const name_start = tag_start + 2;
            std::size_t name_end = name_start;
            while (name_end < input.size() && b_at(input, name_end) != '?' &&
                   b_at(input, name_end) != '>' && !is_ws(b_at(input, name_end))) {
                ++name_end;
            }
            std::size_t end = name_end;
            while (end + 1 < input.size()) {
                if (b_at(input, end) == '?' && b_at(input, end + 1) == '>') {
                    ++end;
                    break;
                }
                ++end;
            }
            std::uint32_t const p = parent_stack.empty() ? UINT32_MAX
                                                          : parent_stack.back();
            index.tag_starts.push_back(static_cast<std::uint64_t>(tag_start));
            index.tag_ends.push_back(static_cast<std::uint64_t>(end));
            index.tag_types.push_back(TagType::PI);
            index.tag_names.emplace_back(static_cast<std::uint64_t>(name_start),
                                         static_cast<std::uint16_t>(name_end - name_start));
            index.depths.push_back(depth);
            index.parents.push_back(p);
            last_tag_end = end;
            while (gt_idx < gt_positions.size() && gt_positions[gt_idx] <= end) ++gt_idx;
        } else {
            // Open or self-closing tag.
            std::size_t const name_start = tag_start + 1;
            std::size_t name_end = name_start;
            while (name_end < gt_pos && b_at(input, name_end) != '>' &&
                   b_at(input, name_end) != '/' && !is_ws(b_at(input, name_end))) {
                ++name_end;
            }
            bool const self_closing = gt_pos > 0 && b_at(input, gt_pos - 1) == '/';
            TagType const tt = self_closing ? TagType::SelfClose : TagType::Open;
            std::uint32_t const tag_idx = static_cast<std::uint32_t>(index.tag_starts.size());
            std::uint32_t const p = parent_stack.empty() ? UINT32_MAX
                                                          : parent_stack.back();
            index.tag_starts.push_back(static_cast<std::uint64_t>(tag_start));
            index.tag_ends.push_back(static_cast<std::uint64_t>(gt_pos));
            index.tag_types.push_back(tt);
            index.tag_names.emplace_back(static_cast<std::uint64_t>(name_start),
                                         static_cast<std::uint16_t>(name_end - name_start));
            index.depths.push_back(depth);
            index.parents.push_back(p);
            if (tt == TagType::Open) {
                parent_stack.push_back(tag_idx);
                ++depth;
            }
            last_tag_end = gt_pos;
        }
    }
    return index;
}

// Public entry point (`simdxml.hpp`). The quote-ratio heuristic that selects
// between `parse_scalar` and `parse_two_stage` (Rust `lib.rs:155-176`) is not
// ported yet; `parse_scalar` is the fastest general path — memchr between '<'
// plus SIMD structural classification and exact-reserve allocation.
Result<XmlIndex> parse(std::span<std::byte const> input) {
    return parse_scalar(input);
}

}  // namespace simdxml