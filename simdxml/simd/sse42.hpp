// SSE4.2 (x86_64) structural character classifier.
//
// Ported from Rust `simd/sse42.rs`. Processes 64 bytes at a time (4x 16-byte
// SSE registers) to produce bitmasks for '<' and '>' positions, with quote
// masking to ignore structural characters inside attribute values.
//
// Quote masking uses a scalar prefix-XOR chain (6 ops on a u64) to compute
// the "inside quotes" bitmask. The fast path handles chunks with only one
// quote type; the slow path walks bits sequentially when both `"` and `'`
// appear in the same 64-byte chunk.
//
// Runtime-dispatch model: the whole body is compiled into every x86_64 TU
// (the `sse4.2` per-function target attribute below makes the ISA legal
// without any -msse4.2 on the command line). Availability is checked at
// runtime by the dispatcher in dispatch.hpp.
#pragma once

#include "simd/dispatch.hpp"
#include "simd/detail.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace simdxml::simd {

#if SIMDXML_HAS_SSE42

namespace detail {

/// Combine four 16-byte SSE movemask results into a single u64 bitmask.
SIMDXML_ALWAYS_INLINE SIMDXML_TARGET_ISA("sse4.2") inline std::uint64_t
movemask_64(__m128i v0, __m128i v1, __m128i v2, __m128i v3) noexcept {
    std::uint64_t const m0 = static_cast<std::uint16_t>(_mm_movemask_epi8(v0));
    std::uint64_t const m1 = static_cast<std::uint16_t>(_mm_movemask_epi8(v1));
    std::uint64_t const m2 = static_cast<std::uint16_t>(_mm_movemask_epi8(v2));
    std::uint64_t const m3 = static_cast<std::uint16_t>(_mm_movemask_epi8(v3));
    return m0 | (m1 << 16) | (m2 << 32) | (m3 << 48);
}

/// Nibble class tables (same layout as the AVX2 backend). Class bytes:
///   0x01 ws-ctrl (\t \n \r)   0x02 ' '   0x04 '"'   0x06 '\''
///   0x08 '&'   0x0A '-'   0x0C '/'   0x10 '<'   0x20 '='
///   0x30 '?'   0x40 '>'   0x80 ']'   0x00 everything else
SIMDXML_ALWAYS_INLINE SIMDXML_TARGET_ISA("sse4.2") inline __m128i
nibble_class_sse42(__m128i v, __m128i lo_table, __m128i hi_table,
                   __m128i v_0F) noexcept {
    return _mm_and_si128(
        _mm_shuffle_epi8(lo_table, _mm_and_si128(v, v_0F)),
        _mm_shuffle_epi8(hi_table,
                         _mm_and_si128(_mm_srli_epi16(v, 4), v_0F)));
}

/// One 64-byte chunk (4x 16-byte registers) -> eleven class masks (order:
/// lt, gt, dq, sq, dash, rbrack, ws, slash, qmark, eq, amp). PSHUFB nibble
/// classification with self-contained tables and class constants.
///
/// This MUST be a target-attributed free function, not a lambda inside
/// `classify_sse42_raw`: a lambda's call operator does not inherit the
/// enclosing function's target attribute in Clang and fails the ABI check
/// at codegen.
SIMDXML_ALWAYS_INLINE SIMDXML_TARGET_ISA("sse4.2") inline std::array<std::uint64_t, 11>
classify_chunk_masks_sse42(__m128i const* ptr) noexcept {
    __m128i const lo_table = _mm_setr_epi8(
        0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x06,
        0x01, 0x01, 0x01, 0x00, 0x10, 0xAB, 0x40, 0x3C);
    __m128i const hi_table = _mm_setr_epi8(
        0x01, 0x00, 0x0E, 0x70, 0x00, 0x80, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);

    __m128i const v_0F = _mm_set1_epi8(0x0F);
    __m128i const c_lt = _mm_set1_epi8(static_cast<char>(0x10));
    __m128i const c_gt = _mm_set1_epi8(static_cast<char>(0x40));
    __m128i const c_dq = _mm_set1_epi8(static_cast<char>(0x04));
    __m128i const c_sq = _mm_set1_epi8(static_cast<char>(0x06));
    __m128i const c_dash = _mm_set1_epi8(static_cast<char>(0x0A));
    __m128i const c_rbrack = _mm_set1_epi8(static_cast<char>(0x80));
    __m128i const c_ws = _mm_set1_epi8(static_cast<char>(0x01));
    __m128i const c_spc = _mm_set1_epi8(static_cast<char>(0x02));
    __m128i const c_slash = _mm_set1_epi8(static_cast<char>(0x0C));
    __m128i const c_qmark = _mm_set1_epi8(static_cast<char>(0x30));
    __m128i const c_eq = _mm_set1_epi8(static_cast<char>(0x20));
    __m128i const c_amp = _mm_set1_epi8(static_cast<char>(0x08));

    __m128i const v0 = _mm_loadu_si128(ptr);
    __m128i const v1 = _mm_loadu_si128(ptr + 1);
    __m128i const v2 = _mm_loadu_si128(ptr + 2);
    __m128i const v3 = _mm_loadu_si128(ptr + 3);

    __m128i const cls0 = nibble_class_sse42(v0, lo_table, hi_table, v_0F);
    __m128i const cls1 = nibble_class_sse42(v1, lo_table, hi_table, v_0F);
    __m128i const cls2 = nibble_class_sse42(v2, lo_table, hi_table, v_0F);
    __m128i const cls3 = nibble_class_sse42(v3, lo_table, hi_table, v_0F);

    return std::array<std::uint64_t, 11>{
        movemask_64(_mm_cmpeq_epi8(cls0, c_lt), _mm_cmpeq_epi8(cls1, c_lt),
                    _mm_cmpeq_epi8(cls2, c_lt), _mm_cmpeq_epi8(cls3, c_lt)),
        movemask_64(_mm_cmpeq_epi8(cls0, c_gt), _mm_cmpeq_epi8(cls1, c_gt),
                    _mm_cmpeq_epi8(cls2, c_gt), _mm_cmpeq_epi8(cls3, c_gt)),
        movemask_64(_mm_cmpeq_epi8(cls0, c_dq), _mm_cmpeq_epi8(cls1, c_dq),
                    _mm_cmpeq_epi8(cls2, c_dq), _mm_cmpeq_epi8(cls3, c_dq)),
        movemask_64(_mm_cmpeq_epi8(cls0, c_sq), _mm_cmpeq_epi8(cls1, c_sq),
                    _mm_cmpeq_epi8(cls2, c_sq), _mm_cmpeq_epi8(cls3, c_sq)),
        movemask_64(_mm_cmpeq_epi8(cls0, c_dash), _mm_cmpeq_epi8(cls1, c_dash),
                    _mm_cmpeq_epi8(cls2, c_dash), _mm_cmpeq_epi8(cls3, c_dash)),
        movemask_64(_mm_cmpeq_epi8(cls0, c_rbrack), _mm_cmpeq_epi8(cls1, c_rbrack),
                    _mm_cmpeq_epi8(cls2, c_rbrack), _mm_cmpeq_epi8(cls3, c_rbrack)),
        // Whitespace spans two class bytes (ctrl + space).
        movemask_64(
            _mm_or_si128(_mm_cmpeq_epi8(cls0, c_ws), _mm_cmpeq_epi8(cls0, c_spc)),
            _mm_or_si128(_mm_cmpeq_epi8(cls1, c_ws), _mm_cmpeq_epi8(cls1, c_spc)),
            _mm_or_si128(_mm_cmpeq_epi8(cls2, c_ws), _mm_cmpeq_epi8(cls2, c_spc)),
            _mm_or_si128(_mm_cmpeq_epi8(cls3, c_ws), _mm_cmpeq_epi8(cls3, c_spc))),
        movemask_64(_mm_cmpeq_epi8(cls0, c_slash), _mm_cmpeq_epi8(cls1, c_slash),
                    _mm_cmpeq_epi8(cls2, c_slash), _mm_cmpeq_epi8(cls3, c_slash)),
        movemask_64(_mm_cmpeq_epi8(cls0, c_qmark), _mm_cmpeq_epi8(cls1, c_qmark),
                    _mm_cmpeq_epi8(cls2, c_qmark), _mm_cmpeq_epi8(cls3, c_qmark)),
        movemask_64(_mm_cmpeq_epi8(cls0, c_eq), _mm_cmpeq_epi8(cls1, c_eq),
                    _mm_cmpeq_epi8(cls2, c_eq), _mm_cmpeq_epi8(cls3, c_eq)),
        movemask_64(_mm_cmpeq_epi8(cls0, c_amp), _mm_cmpeq_epi8(cls1, c_amp),
                    _mm_cmpeq_epi8(cls2, c_amp), _mm_cmpeq_epi8(cls3, c_amp)),
    };
}

}  // namespace detail

/// Classify structural characters using SSE4.2 vector instructions.
///
/// Caller must ensure SSE4.2 is available (checked by dispatcher).
SIMDXML_TARGET_ISA("sse4.2") [[nodiscard]] inline StructuralIndex
classify_sse42(std::span<std::byte const> input) noexcept {
    std::size_t const len = input.size();
    std::size_t const num_chunks = (len + 63) / 64;
    StructuralIndex idx;
    idx.lt_bits.assign(num_chunks, 0);
    idx.gt_bits.assign(num_chunks, 0);
    idx.lt_raw_bits.assign(num_chunks, 0);
    idx.gt_raw_bits.assign(num_chunks, 0);
    idx.len = len;

    bool in_dquote = false;
    bool in_squote = false;
    std::size_t const full_chunks = len / 64;

    __m128i const v_lt = _mm_set1_epi8(static_cast<char>('<'));
    __m128i const v_gt = _mm_set1_epi8(static_cast<char>('>'));
    __m128i const v_dquote = _mm_set1_epi8(static_cast<char>('"'));
    __m128i const v_squote = _mm_set1_epi8(static_cast<char>('\''));

    for (std::size_t chunk = 0; chunk < full_chunks; ++chunk) {
        std::size_t const base = chunk * 64;
        __m128i const* ptr = reinterpret_cast<__m128i const*>(input.data() + base);

        __m128i const v0 = _mm_loadu_si128(ptr);
        __m128i const v1 = _mm_loadu_si128(ptr + 1);
        __m128i const v2 = _mm_loadu_si128(ptr + 2);
        __m128i const v3 = _mm_loadu_si128(ptr + 3);

        __m128i const lt0 = _mm_cmpeq_epi8(v0, v_lt);
        __m128i const lt1 = _mm_cmpeq_epi8(v1, v_lt);
        __m128i const lt2 = _mm_cmpeq_epi8(v2, v_lt);
        __m128i const lt3 = _mm_cmpeq_epi8(v3, v_lt);

        __m128i const gt0 = _mm_cmpeq_epi8(v0, v_gt);
        __m128i const gt1 = _mm_cmpeq_epi8(v1, v_gt);
        __m128i const gt2 = _mm_cmpeq_epi8(v2, v_gt);
        __m128i const gt3 = _mm_cmpeq_epi8(v3, v_gt);

        __m128i const dq0 = _mm_cmpeq_epi8(v0, v_dquote);
        __m128i const dq1 = _mm_cmpeq_epi8(v1, v_dquote);
        __m128i const dq2 = _mm_cmpeq_epi8(v2, v_dquote);
        __m128i const dq3 = _mm_cmpeq_epi8(v3, v_dquote);

        __m128i const sq0 = _mm_cmpeq_epi8(v0, v_squote);
        __m128i const sq1 = _mm_cmpeq_epi8(v1, v_squote);
        __m128i const sq2 = _mm_cmpeq_epi8(v2, v_squote);
        __m128i const sq3 = _mm_cmpeq_epi8(v3, v_squote);

        std::uint64_t const lt_mask = detail::movemask_64(lt0, lt1, lt2, lt3);
        std::uint64_t const gt_mask = detail::movemask_64(gt0, gt1, gt2, gt3);
        std::uint64_t const dq_mask = detail::movemask_64(dq0, dq1, dq2, dq3);
        std::uint64_t const sq_mask = detail::movemask_64(sq0, sq1, sq2, sq3);

        // Raw masks: every '<'/'>' byte, quote-agnostic.
        idx.lt_raw_bits[chunk] = lt_mask;
        idx.gt_raw_bits[chunk] = gt_mask;

        auto [masked_lt, masked_gt] = detail::apply_quote_mask(
            lt_mask, gt_mask, dq_mask, sq_mask, in_dquote, in_squote);

        idx.lt_bits[chunk] = masked_lt;
        idx.gt_bits[chunk] = masked_gt;
    }

    // Handle remaining bytes (< 64) with scalar.
    std::size_t const remaining_start = full_chunks * 64;
    if (remaining_start < len) {
        std::size_t const chunk_idx = full_chunks;
        std::uint64_t lt = 0;
        std::uint64_t gt = 0;
        std::uint64_t lt_raw = 0;
        std::uint64_t gt_raw = 0;
        for (std::size_t i = remaining_start; i < len; ++i) {
            unsigned char const byte = static_cast<unsigned char>(
                std::to_integer<std::uint8_t>(input[i]));
            std::uint32_t const bit = static_cast<std::uint32_t>(i - remaining_start);
            std::uint64_t const bit_val = (std::uint64_t{1} << bit);

            if (byte == '<') {
                lt_raw |= bit_val;
                if (!in_dquote && !in_squote) lt |= bit_val;
                continue;
            }
            if (byte == '>') {
                gt_raw |= bit_val;
                if (!in_dquote && !in_squote) gt |= bit_val;
                continue;
            }
            if (in_dquote) {
                if (byte == '"') in_dquote = false;
                continue;
            }
            if (in_squote) {
                if (byte == '\'') in_squote = false;
                continue;
            }
            switch (byte) {
                case '"': in_dquote = true; break;
                case '\'': in_squote = true; break;
                default: break;
            }
        }
        if (chunk_idx < idx.lt_bits.size()) {
            idx.lt_bits[chunk_idx] = lt;
            idx.gt_bits[chunk_idx] = gt;
            idx.lt_raw_bits[chunk_idx] = lt_raw;
            idx.gt_raw_bits[chunk_idx] = gt_raw;
        }
    }
    return idx;
}

/// Raw SSE4.2 variant: classifies ONLY the quote-agnostic masks the parse
/// state machine consumes (lt/gt raw, quotes, dash, bracket, whitespace,
/// name delimiters, '=', '&'). Classification is PSHUFB nibble-style
/// (`_mm_shuffle_epi8` — SSSE3 is part of the sse4.2 target): two 16-byte
/// tables map the low / high nibble of every byte to a class byte whose AND
/// is the byte's class (0 = uninteresting). One classification pass feeds
/// all eleven property masks. `lt_bits` / `gt_bits` are left empty — see
/// `classify_structural_raw`.
SIMDXML_TARGET_ISA("sse4.2") [[nodiscard]] inline StructuralIndex
classify_sse42_raw(std::span<std::byte const> input) noexcept {
    std::size_t const len = input.size();
    std::size_t const num_chunks = (len + 63) / 64;
    StructuralIndex idx;
    idx.lt_raw_bits.assign(num_chunks, 0);
    idx.gt_raw_bits.assign(num_chunks, 0);
    idx.dq_bits.assign(num_chunks, 0);
    idx.sq_bits.assign(num_chunks, 0);
    idx.dash_bits.assign(num_chunks, 0);
    idx.rbrack_bits.assign(num_chunks, 0);
    idx.ws_bits.assign(num_chunks, 0);
    idx.slash_bits.assign(num_chunks, 0);
    idx.qmark_bits.assign(num_chunks, 0);
    idx.eq_bits.assign(num_chunks, 0);
    idx.amp_bits.assign(num_chunks, 0);
    idx.len = len;

    std::size_t const full_chunks = len / 64;

    // One 64-byte chunk (4x 16-byte registers) -> eleven class masks
    // (order: lt, gt, dq, sq, dash, rbrack, ws, slash, qmark, eq, amp).
    // The per-chunk work lives in detail::classify_chunk_masks_sse42 — a
    // TARGET-ATTRIBUTED free function, not a lambda (a lambda's call
    // operator does not inherit the enclosing function's target attribute
    // and fails the SSE-ABI check at codegen).
    auto const store_chunk = [&](std::size_t c,
                                 std::array<std::uint64_t, 11> const& m) {
        idx.lt_raw_bits[c] = m[0];
        idx.gt_raw_bits[c] = m[1];
        idx.dq_bits[c] = m[2];
        idx.sq_bits[c] = m[3];
        idx.dash_bits[c] = m[4];
        idx.rbrack_bits[c] = m[5];
        idx.ws_bits[c] = m[6];
        idx.slash_bits[c] = m[7];
        idx.qmark_bits[c] = m[8];
        idx.eq_bits[c] = m[9];
        idx.amp_bits[c] = m[10];
    };

    // Unrolled two chunks per iteration: independent chains.
    std::size_t chunk = 0;
    for (; chunk + 1 < full_chunks; chunk += 2) {
        auto const a = detail::classify_chunk_masks_sse42(
            reinterpret_cast<__m128i const*>(input.data() + chunk * 64));
        auto const b = detail::classify_chunk_masks_sse42(
            reinterpret_cast<__m128i const*>(input.data() + (chunk + 1) * 64));
        store_chunk(chunk, a);
        store_chunk(chunk + 1, b);
    }
    if (chunk < full_chunks) {
        store_chunk(chunk, detail::classify_chunk_masks_sse42(
                               reinterpret_cast<__m128i const*>(input.data() +
                                                                chunk * 64)));
    }

    // Handle remaining bytes (< 64) with scalar. Raw masks need no quote
    // state — every interesting byte is recorded.
    std::size_t const remaining_start = full_chunks * 64;
    if (remaining_start < len) {
        std::size_t const chunk_idx = full_chunks;
        std::uint64_t lt_raw = 0;
        std::uint64_t gt_raw = 0;
        std::uint64_t dq = 0;
        std::uint64_t sq = 0;
        std::uint64_t dash = 0;
        std::uint64_t rbrack = 0;
        std::uint64_t ws = 0;
        std::uint64_t slash = 0;
        std::uint64_t qmark = 0;
        std::uint64_t eq = 0;
        std::uint64_t amp = 0;
        for (std::size_t i = remaining_start; i < len; ++i) {
            unsigned char const byte = static_cast<unsigned char>(
                std::to_integer<std::uint8_t>(input[i]));
            std::uint64_t const bit_val =
                (std::uint64_t{1} << static_cast<std::uint32_t>(i - remaining_start));
            switch (byte) {
                case '<': lt_raw |= bit_val; break;
                case '>': gt_raw |= bit_val; break;
                case '"': dq |= bit_val; break;
                case '\'': sq |= bit_val; break;
                case '-': dash |= bit_val; break;
                case ']': rbrack |= bit_val; break;
                case ' ': case '\t': case '\n': case '\r':
                    ws |= bit_val; break;
                case '/': slash |= bit_val; break;
                case '?': qmark |= bit_val; break;
                case '=': eq |= bit_val; break;
                case '&': amp |= bit_val; break;
                default: break;
            }
        }
        if (chunk_idx < idx.lt_raw_bits.size()) {
            idx.lt_raw_bits[chunk_idx] = lt_raw;
            idx.gt_raw_bits[chunk_idx] = gt_raw;
            idx.dq_bits[chunk_idx] = dq;
            idx.sq_bits[chunk_idx] = sq;
            idx.dash_bits[chunk_idx] = dash;
            idx.rbrack_bits[chunk_idx] = rbrack;
            idx.ws_bits[chunk_idx] = ws;
            idx.slash_bits[chunk_idx] = slash;
            idx.qmark_bits[chunk_idx] = qmark;
            idx.eq_bits[chunk_idx] = eq;
            idx.amp_bits[chunk_idx] = amp;
        }
    }
    return idx;
}

#endif  // SIMDXML_HAS_SSE42

}  // namespace simdxml::simd