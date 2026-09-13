// Shared scalar bit-twiddling helpers for the structural classifiers.
//
// These helpers used to be duplicated verbatim in sse42.hpp, avx2.hpp and
// neon.hpp. That broke twice:
//   1. With the runtime-dispatch model (SIMDXML_HAS_SSE42/AVX2 = 1 on every
//      x86_64 TU) the same `detail::` functions were defined twice per TU —
//      redefinition errors.
//   2. sse42.hpp:91 (and avx2.hpp:81, neon.hpp:115) called
//      `apply_quote_mask_slow` *before* its definition with no forward
//      declaration — hard error even with -mavx2.
//
// One copy lives here. It is plain scalar u64 code with **no** ISA target
// attribute, so it may be freely inlined into backend functions carrying
// [[gnu::target("avx2")]] / [[gnu::target("sse4.2")]] / [[gnu::target("+neon")]]
// attributes (the restriction is only on inlining ISA code into a function
// without that target — the reverse direction is always legal).
//
// Keeping a single copy also guarantees all backends agree bit-for-bit on the
// quote masking.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>

#ifndef SIMDXML_ALWAYS_INLINE
#define SIMDXML_ALWAYS_INLINE [[gnu::always_inline]]
#endif

namespace rai::xml::simd::detail {

/// Compute prefix-XOR: bit i of result = XOR of bits 0..=i in mask.
/// Scalar shift-and-XOR chain (6 ops on a u64).
SIMDXML_ALWAYS_INLINE inline std::uint64_t
prefix_xor(std::uint64_t mask) noexcept {
    std::uint64_t x = mask;
    x ^= x << 1;
    x ^= x << 2;
    x ^= x << 4;
    x ^= x << 8;
    x ^= x << 16;
    x ^= x << 32;
    return x;
}

/// Bitmask with bits [0, pos] set (all bits up to and including `pos`).
SIMDXML_ALWAYS_INLINE inline std::uint64_t
mask_up_to(std::uint32_t pos) noexcept {
    if (pos >= 63) return ~std::uint64_t{0};
    return (std::uint64_t{1} << (pos + 1)) - 1;
}

/// Bitmask with bits [pos, 63] set.
SIMDXML_ALWAYS_INLINE inline std::uint64_t
mask_from(std::uint32_t pos) noexcept {
    if (pos >= 64) return 0;
    return ~((std::uint64_t{1} << pos) - 1);
}

/// Sequential bit-walk fallback for chunks that mix `"` and `'`.
///
/// Scalar code — defined before `apply_quote_mask` so the call below resolves
/// without a forward declaration.
inline std::pair<std::uint64_t, std::uint64_t>
apply_quote_mask_slow(std::uint64_t lt_mask, std::uint64_t gt_mask,
                      std::uint64_t dq_mask, std::uint64_t sq_mask,
                      bool& in_dquote, bool& in_squote) noexcept {
    std::uint64_t quoted_mask = 0;
    std::uint64_t remaining = dq_mask | sq_mask;

    if (in_dquote) {
        if (dq_mask != 0) {
            std::uint32_t const close_pos = std::countr_zero(dq_mask);
            quoted_mask |= mask_up_to(close_pos);
            in_dquote = false;
            remaining &= ~mask_up_to(close_pos);
        } else {
            return {0, 0};
        }
    } else if (in_squote) {
        if (sq_mask != 0) {
            std::uint32_t const close_pos = std::countr_zero(sq_mask);
            quoted_mask |= mask_up_to(close_pos);
            in_squote = false;
            remaining &= ~mask_up_to(close_pos);
        } else {
            return {0, 0};
        }
    }

    while (remaining != 0) {
        std::uint32_t const pos = std::countr_zero(remaining);
        remaining &= remaining - 1;
        bool const byte_is_dquote = ((dq_mask >> pos) & 1) == 1;

        std::uint64_t const after_mask =
            (pos < 63) ? ~((std::uint64_t{1} << (pos + 1)) - 1) : 0;
        std::uint64_t const close_mask =
            byte_is_dquote ? (dq_mask & after_mask) : (sq_mask & after_mask);

        if (close_mask != 0) {
            std::uint32_t const close_pos = std::countr_zero(close_mask);
            std::uint64_t const range = mask_up_to(close_pos) & mask_from(pos);
            quoted_mask |= range;
            remaining &= ~range;
        } else {
            quoted_mask |= mask_from(pos);
            if (byte_is_dquote) in_dquote = true; else in_squote = true;
            break;
        }
    }
    return {lt_mask & ~quoted_mask, gt_mask & ~quoted_mask};
}

/// Apply quote masking to structural character bitmasks.
///
/// Fast path: only one quote type present -> prefix-XOR.
/// Slow path: both quote types present -> sequential bit-walk.
SIMDXML_ALWAYS_INLINE inline std::pair<std::uint64_t, std::uint64_t>
apply_quote_mask(std::uint64_t lt_mask, std::uint64_t gt_mask,
                 std::uint64_t dq_mask, std::uint64_t sq_mask,
                 bool& in_dquote, bool& in_squote) noexcept {
    if (dq_mask == 0 && sq_mask == 0 && !in_dquote && !in_squote) {
        return {lt_mask, gt_mask};
    }
    if (sq_mask == 0 && !in_squote) {
        std::uint64_t quoted = prefix_xor(dq_mask);
        if (in_dquote) quoted = ~quoted;
        in_dquote = ((std::popcount(dq_mask) & 1) == 1) ^ in_dquote;
        return {lt_mask & ~quoted, gt_mask & ~quoted};
    }
    if (dq_mask == 0 && !in_dquote) {
        std::uint64_t quoted = prefix_xor(sq_mask);
        if (in_squote) quoted = ~quoted;
        in_squote = ((std::popcount(sq_mask) & 1) == 1) ^ in_squote;
        return {lt_mask & ~quoted, gt_mask & ~quoted};
    }
    return apply_quote_mask_slow(lt_mask, gt_mask, dq_mask, sq_mask,
                                 in_dquote, in_squote);
}

}  // namespace rai::xml::simd::detail