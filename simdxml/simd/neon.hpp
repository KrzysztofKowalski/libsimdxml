// NEON (AArch64) structural character classifier.
//
// Ported from Rust `simd/neon.rs`. Processes 64 bytes at a time (4x 16-byte
// NEON registers) to produce bitmasks for '<' and '>' positions, with quote
// masking to ignore structural characters inside attribute values.
//
// Key insight from simdjson: instead of branching per-byte, classify ALL
// bytes in one vectorized pass, then walk the bitmasks with bit manipulation.
//
// Quote masking uses PMULL (carryless multiply) for the fast path on AArch64.
// The scalar prefix-XOR chain is used elsewhere. Shared scalar helpers
// (prefix_xor / mask_up_to / mask_from / apply_quote_mask_slow) live in
// simd/detail.hpp — one copy for all backends.
//
// `prefix_xor_pmull` is deliberately OUTLINED ([[gnu::noinline]]): it needs
// the optional +crypto target and must never be inlined into a function
// compiled with plain `+neon` (feature mismatch). The dispatcher checks
// has_crypto() at runtime before entering classify_neon.
#pragma once

#include "simd/dispatch.hpp"
#include "simd/detail.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace simdxml::simd {

#if SIMDXML_HAS_NEON

namespace detail {

/// Extract a 16-bit bitmask from a NEON comparison result (0xFF/0x00 per
/// byte). Equivalent to x86 `_mm_movemask_epi8`.
SIMDXML_ALWAYS_INLINE SIMDXML_TARGET_ISA("+neon") inline std::uint16_t
neon_movemask(uint8x16_t v) noexcept {
    alignas(16) static constexpr std::uint8_t MASK[16] = {
        1, 2, 4, 8, 16, 32, 64, 128,
        1, 2, 4, 8, 16, 32, 64, 128,
    };
    uint8x16_t const mask = vld1q_u8(MASK);
    uint8x16_t const masked = vandq_u8(v, mask);
    std::uint8_t const lo_sum = vaddv_u8(vget_low_u8(masked));
    std::uint8_t const hi_sum = vaddv_u8(vget_high_u8(masked));
    return static_cast<std::uint16_t>(lo_sum) |
           (static_cast<std::uint16_t>(hi_sum) << 8);
}

/// Convert four 16-byte NEON comparison results into a single u64 bitmask.
SIMDXML_ALWAYS_INLINE SIMDXML_TARGET_ISA("+neon") inline std::uint64_t
neon_to_bitmask_64(uint8x16_t v0, uint8x16_t v1,
                   uint8x16_t v2, uint8x16_t v3) noexcept {
    std::uint64_t const m0 = neon_movemask(v0);
    std::uint64_t const m1 = neon_movemask(v1);
    std::uint64_t const m2 = neon_movemask(v2);
    std::uint64_t const m3 = neon_movemask(v3);
    return m0 | (m1 << 16) | (m2 << 32) | (m3 << 48);
}

/// Compute prefix-XOR using ARM PMULL (polynomial multiply).
/// prefix_xor(x) at bit i = XOR of all bits 0..=i in x.
///
/// Outlined on purpose: PMULL requires the optional `crypto` feature, so the
/// function carries its own `+crypto` target and must not be inlined into
/// plain `+neon` callers.
[[gnu::noinline]] SIMDXML_TARGET_ISA("+crypto,+neon") inline std::uint64_t
prefix_xor_pmull(std::uint64_t mask) noexcept {
    // PMULL: polynomial multiply long (carryless multiply).
    // clmul(mask, 0xFFFF_FFFF_FFFF_FFFF) produces the prefix XOR.
    // We only need the low 64 bits of the 128-bit result.
    poly64_t const a = static_cast<poly64_t>(mask);
    poly64_t const b = static_cast<poly64_t>(~std::uint64_t{0});
    poly128_t const r = vmull_p64(a, b);
    return vgetq_lane_u64(vreinterpretq_u64_p128(r), 0);
}

/// Apply quote masking to structural character bitmasks (PMULL variant).
///
/// Same split as the shared detail::apply_quote_mask, but the prefix-XOR of
/// the fast paths comes from PMULL. The slow path is shared scalar code.
SIMDXML_ALWAYS_INLINE inline std::pair<std::uint64_t, std::uint64_t>
apply_quote_mask_pmull(std::uint64_t lt_mask, std::uint64_t gt_mask,
                       std::uint64_t dq_mask, std::uint64_t sq_mask,
                       bool& in_dquote, bool& in_squote) noexcept {
    if (dq_mask == 0 && sq_mask == 0 && !in_dquote && !in_squote) {
        return {lt_mask, gt_mask};
    }
    if (sq_mask == 0 && !in_squote) {
        std::uint64_t quoted = prefix_xor_pmull(dq_mask);
        if (in_dquote) quoted = ~quoted;
        in_dquote = ((std::popcount(dq_mask) & 1) == 1) ^ in_dquote;
        return {lt_mask & ~quoted, gt_mask & ~quoted};
    }
    if (dq_mask == 0 && !in_dquote) {
        std::uint64_t quoted = prefix_xor_pmull(sq_mask);
        if (in_squote) quoted = ~quoted;
        in_squote = ((std::popcount(sq_mask) & 1) == 1) ^ in_squote;
        return {lt_mask & ~quoted, gt_mask & ~quoted};
    }
    return apply_quote_mask_slow(lt_mask, gt_mask, dq_mask, sq_mask,
                                 in_dquote, in_squote);
}

}  // namespace detail

/// Classify structural characters using NEON vector instructions.
SIMDXML_TARGET_ISA("+neon") [[nodiscard]] inline StructuralIndex
classify_neon(std::span<std::byte const> input) noexcept {
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

    uint8x16_t const v_lt = vdupq_n_u8(static_cast<std::uint8_t>('<'));
    uint8x16_t const v_gt = vdupq_n_u8(static_cast<std::uint8_t>('>'));
    uint8x16_t const v_dquote = vdupq_n_u8(static_cast<std::uint8_t>('"'));
    uint8x16_t const v_squote = vdupq_n_u8(static_cast<std::uint8_t>('\''));

    for (std::size_t chunk = 0; chunk < full_chunks; ++chunk) {
        std::size_t const base = chunk * 64;
        std::uint8_t const* ptr =
            reinterpret_cast<std::uint8_t const*>(input.data() + base);

        uint8x16_t const v0 = vld1q_u8(ptr);
        uint8x16_t const v1 = vld1q_u8(ptr + 16);
        uint8x16_t const v2 = vld1q_u8(ptr + 32);
        uint8x16_t const v3 = vld1q_u8(ptr + 48);

        uint8x16_t const lt0 = vceqq_u8(v0, v_lt);
        uint8x16_t const lt1 = vceqq_u8(v1, v_lt);
        uint8x16_t const lt2 = vceqq_u8(v2, v_lt);
        uint8x16_t const lt3 = vceqq_u8(v3, v_lt);

        uint8x16_t const gt0 = vceqq_u8(v0, v_gt);
        uint8x16_t const gt1 = vceqq_u8(v1, v_gt);
        uint8x16_t const gt2 = vceqq_u8(v2, v_gt);
        uint8x16_t const gt3 = vceqq_u8(v3, v_gt);

        uint8x16_t const dq0 = vceqq_u8(v0, v_dquote);
        uint8x16_t const dq1 = vceqq_u8(v1, v_dquote);
        uint8x16_t const dq2 = vceqq_u8(v2, v_dquote);
        uint8x16_t const dq3 = vceqq_u8(v3, v_dquote);

        uint8x16_t const sq0 = vceqq_u8(v0, v_squote);
        uint8x16_t const sq1 = vceqq_u8(v1, v_squote);
        uint8x16_t const sq2 = vceqq_u8(v2, v_squote);
        uint8x16_t const sq3 = vceqq_u8(v3, v_squote);

        std::uint64_t const lt_mask = detail::neon_to_bitmask_64(lt0, lt1, lt2, lt3);
        std::uint64_t const gt_mask = detail::neon_to_bitmask_64(gt0, gt1, gt2, gt3);
        std::uint64_t const dq_mask = detail::neon_to_bitmask_64(dq0, dq1, dq2, dq3);
        std::uint64_t const sq_mask = detail::neon_to_bitmask_64(sq0, sq1, sq2, sq3);

        // Raw masks: every '<'/'>' byte, quote-agnostic.
        idx.lt_raw_bits[chunk] = lt_mask;
        idx.gt_raw_bits[chunk] = gt_mask;

        auto [masked_lt, masked_gt] = detail::apply_quote_mask_pmull(
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

/// Raw NEON variant: quote-agnostic '<' / '>' masks only. Implemented as
/// the plain byte loop (same as the scalar backend) — the raw path is not
/// the hot path on AArch64 and a vectorized version can follow the x86
/// raw classifiers if measurements justify it. `lt_bits` / `gt_bits` are
/// left empty — see `classify_structural_raw`.
[[nodiscard]] inline StructuralIndex
classify_neon_raw(std::span<std::byte const> input) noexcept {
    return classify_scalar_raw(input);
}

#endif  // SIMDXML_HAS_NEON

}  // namespace simdxml::simd