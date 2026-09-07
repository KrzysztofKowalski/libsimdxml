// SIMD dispatch + shared structural-index types.
//
// Ported from Rust `simd/mod.rs`. This header:
//   1. Defines `StructuralIndex` shared by all SIMD backends.
//   2. Includes the backend headers (scalar / sse42 / avx2 / neon) at the
//      bottom so that the `classify_structural` dispatcher can call them.
//   3. Provides runtime CPU detection helpers (`has_avx2`, `has_sse42`,
//      `has_neon`, `has_crypto`) and the `classify_structural` entry point
//      that picks the best backend at RUNTIME.
//
// Dispatch model — runtime, not compile-time:
//   The backend bodies are compiled into EVERY x86_64 TU. The ISA selection
//   happens per function via [[gnu::target("avx2")]] / [[gnu::target("sse4.2")]]
//   (GCC + Clang), so no `-mavx2`/`-msse4.2` is needed on the command line and
//   the same binary runs on pre-AVX2 machines (falling back to SSE4.2 or the
//   scalar backend). `SIMDXML_HAS_*` below therefore mean "the backend code is
//   compiled in", not "the CPU supports it" — availability is checked at
//   runtime by classify_structural(). On MSVC there is no per-function target
//   attribute, so there the backends still require /arch flags (old model).
//
// Backends:
//   - scalar.hpp  — universal fallback (always available)
//   - sse42.hpp   — x86_64 SSE4.2 (64-byte chunks via 4x __m128i)
//   - avx2.hpp    — x86_64 AVX2 (64-byte chunks via 2x __m256i)
//   - neon.hpp    — AArch64 NEON (64-byte chunks via 4x uint8x16_t)
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#endif

// Per-function ISA targeting (GCC / Clang). MSVC has no equivalent attribute;
// there the marker expands to nothing and the backend bodies only compile when
// the command line already enables the ISA (see the SIMDXML_HAS_* logic).
#if defined(__GNUC__) || defined(__clang__)
#define SIMDXML_TARGET_ISA(isa) [[gnu::target(isa)]]
#else
#define SIMDXML_TARGET_ISA(isa)
#endif

#ifndef SIMDXML_ALWAYS_INLINE
#define SIMDXML_ALWAYS_INLINE [[gnu::always_inline]]
#endif

// --- Which backends are compiled in -----------------------------------------

#if defined(__aarch64__) || defined(_M_ARM64)
#define SIMDXML_HAS_NEON 1
#else
#define SIMDXML_HAS_NEON 0
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define SIMDXML_X86_64 1
#else
#define SIMDXML_X86_64 0
#endif

#if SIMDXML_X86_64 && (defined(__GNUC__) || defined(__clang__))
// GCC/Clang: per-function target attributes -> both x86 backends always
// compiled, ISA checked at runtime.
#define SIMDXML_HAS_AVX2 1
#define SIMDXML_HAS_SSE42 1
#elif SIMDXML_X86_64
// MSVC: needs /arch:AVX2 (implies /arch:SSE4.2) on the command line.
#if defined(__AVX2__)
#define SIMDXML_HAS_AVX2 1
#define SIMDXML_HAS_SSE42 1
#else
#define SIMDXML_HAS_AVX2 0
#if defined(__SSE4_2__)
#define SIMDXML_HAS_SSE42 1
#else
#define SIMDXML_HAS_SSE42 0
#endif
#endif
#else
#define SIMDXML_HAS_AVX2 0
#define SIMDXML_HAS_SSE42 0
#endif

namespace simdxml::simd {

/// Structural positions extracted from SIMD classification.
/// Each u64 is a bitmask over a 64-byte chunk of input.
struct StructuralIndex {
    /// Positions of '<' characters (not inside quotes).
    std::vector<std::uint64_t> lt_bits;
    /// Positions of '>' characters (not inside quotes).
    std::vector<std::uint64_t> gt_bits;
    /// Positions of ALL '<' bytes, quote-agnostic. Byte-exact superset used
    /// by `parse_scalar` (which handles quotes with its own scalar state
    /// machine) so the SIMD-dispatched scanner stays bit-identical to the
    /// old memchr scanner.
    std::vector<std::uint64_t> lt_raw_bits;
    /// Positions of ALL '>' bytes, quote-agnostic. Same rationale.
    std::vector<std::uint64_t> gt_raw_bits;
    /// Positions of every '"' byte. Filled by the `*_raw` classifiers only
    /// (the parse state machine reuses them instead of memchr-scanning for
    /// attribute-value terminators).
    std::vector<std::uint64_t> dq_bits;
    /// Positions of every '\'' byte. Filled by the `*_raw` classifiers only.
    std::vector<std::uint64_t> sq_bits;
    /// Positions of every '-' byte. Filled by the `*_raw` classifiers only
    /// (comment-end scanning: `dash & (dash >> 1)` finds a `--` pair).
    std::vector<std::uint64_t> dash_bits;
    /// Positions of every ']' byte. Filled by the `*_raw` classifiers only
    /// (CDATA-end scanning: `rbrack & (rbrack >> 1)` finds a `]]` pair).
    std::vector<std::uint64_t> rbrack_bits;
    /// Whitespace bytes (space, tab, CR, LF). Filled by the `*_raw`
    /// classifiers only (name-terminator scanning).
    std::vector<std::uint64_t> ws_bits;
    /// Positions of every '/' byte. Filled by the `*_raw` classifiers only
    /// (open-tag name terminator).
    std::vector<std::uint64_t> slash_bits;
    /// Positions of every '?' byte. Filled by the `*_raw` classifiers only
    /// (PI name terminator and `?>` scanning).
    std::vector<std::uint64_t> qmark_bits;
    /// Positions of every '=' byte. Filled by the `*_raw` classifiers only.
    std::vector<std::uint64_t> eq_bits;
    /// Positions of every '&' byte. Filled by the `*_raw` classifiers only.
    std::vector<std::uint64_t> amp_bits;
    // NOTE: the `*_raw` classifiers below fill ONLY the raw mask sets
    // and leave `lt_bits` / `gt_bits` EMPTY (no quote-masking pass) — they
    // serve scanners that track quotes in their own state machine.
    /// Total input length.
    std::size_t len = 0;

    /// Iterate all '<' positions in document order.
    [[nodiscard]] std::vector<std::size_t> lt_positions() const {
        return collect_positions(lt_bits);
    }

    /// Iterate all '>' positions in document order.
    [[nodiscard]] std::vector<std::size_t> gt_positions() const {
        return collect_positions(gt_bits);
    }

    /// Iterate all raw '<' byte positions in document order.
    [[nodiscard]] std::vector<std::size_t> raw_lt_positions() const {
        return collect_positions(lt_raw_bits);
    }

    /// Iterate all raw '>' byte positions in document order.
    [[nodiscard]] std::vector<std::size_t> raw_gt_positions() const {
        return collect_positions(gt_raw_bits);
    }

private:
    [[nodiscard]] static std::vector<std::size_t>
    collect_positions(std::vector<std::uint64_t> const& bits) {
        std::vector<std::size_t> out;
        for (std::size_t chunk = 0; chunk < bits.size(); ++chunk) {
            std::uint64_t cur = bits[chunk];
            while (cur != 0) {
                std::size_t const pos = std::countr_zero(cur);
                out.push_back(chunk * 64 + pos);
                cur &= cur - 1;  // clear lowest set bit
            }
        }
        return out;
    }
};

}  // namespace simdxml::simd

// Include backends so the dispatcher below can call them. The backends
// `#include "dispatch.hpp"` themselves, but the `#pragma once` guard above
// makes the recursive include a no-op — by the time a backend is processed,
// `StructuralIndex` and the SIMDXML_HAS_* macros are already defined.
#include "simd/scalar.hpp"
#if SIMDXML_HAS_SSE42
#include "simd/sse42.hpp"
#endif
#if SIMDXML_HAS_AVX2
#include "simd/avx2.hpp"
#endif
#if SIMDXML_HAS_NEON
#include "simd/neon.hpp"
#endif

namespace simdxml::simd {

/// Detect AVX2 support at runtime. Returns false on non-x86_64 platforms.
[[nodiscard]] inline bool has_avx2() noexcept {
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#elif defined(__x86_64__) && defined(__clang__)
    return __builtin_cpu_supports("avx2");
#elif defined(__x86_64__) && defined(_MSC_VER)
    // MSVC: a full runtime check uses __cpuid leaf 7 sub-leaf 0, ebx bit 5.
    // For now we conservatively return false and rely on compile-time /arch.
    return false;
#else
    return false;
#endif
}

/// Detect SSE4.2 support at runtime. Returns false on non-x86_64 platforms.
[[nodiscard]] inline bool has_sse42() noexcept {
#if defined(__x86_64__) && defined(__GNUC__)
    return __builtin_cpu_supports("sse4.2");
#elif defined(__x86_64__) && defined(_MSC_VER)
    return false;  // conservative; rely on compile-time /arch flag
#else
    return false;
#endif
}

/// Compile-time NEON availability (every AArch64 core has NEON).
[[nodiscard]] inline bool has_neon() noexcept {
#if SIMDXML_HAS_NEON
    return true;
#else
    return false;
#endif
}

/// Detect the AArch64 crypto extension (PMULL) at runtime. The NEON fast
/// path uses `vmull_p64`, which traps on cores without the optional crypto
/// feature, so the dispatcher must check before entering `classify_neon`.
[[nodiscard]] inline bool has_crypto() noexcept {
#if defined(__aarch64__) && defined(__linux__)
#ifndef HWCAP_PMULL
#define HWCAP_PMULL (1UL << 4)
#endif
    return (getauxval(AT_HWCAP) & static_cast<unsigned long>(HWCAP_PMULL)) != 0;
#elif defined(__aarch64__)
    return true;  // Apple silicon and friends: crypto always present
#else
    return false;
#endif
}

/// Build structural index using the best available SIMD on this platform.
///
/// Mirrors the Rust `classify_structural` dispatcher, but with RUNTIME
/// feature checks on every x86_64 CPU (the backends are always compiled):
/// - AArch64 with crypto:  NEON
/// - x86_64 with AVX2:     AVX2     (runtime-checked)
/// - x86_64 with SSE4.2:   SSE4.2   (runtime-checked)
/// - Otherwise:            scalar fallback
[[nodiscard]] inline StructuralIndex
classify_structural(std::span<std::byte const> input) {
#if SIMDXML_HAS_NEON
    if (has_crypto()) return classify_neon(input);
#endif
#if SIMDXML_HAS_AVX2
    if (has_avx2()) return classify_avx2(input);
#endif
#if SIMDXML_HAS_SSE42
    if (has_sse42()) return classify_sse42(input);
#endif
    return classify_scalar(input);
}

/// Raw variant of `classify_structural`: computes ONLY the quote-agnostic
/// `lt_raw_bits` / `gt_raw_bits` masks and leaves `lt_bits` / `gt_bits`
/// empty. Skips the quote-masking pass entirely — for scanners that handle
/// quotes in their own state machine and need nothing but raw byte
/// positions (e.g. `parse_scalar`). Same backend selection as
/// `classify_structural`.
[[nodiscard]] inline StructuralIndex
classify_structural_raw(std::span<std::byte const> input) {
#if SIMDXML_HAS_NEON
    if (has_crypto()) return classify_neon_raw(input);
#endif
#if SIMDXML_HAS_AVX2
    if (has_avx2()) return classify_avx2_raw(input);
#endif
#if SIMDXML_HAS_SSE42
    if (has_sse42()) return classify_sse42_raw(input);
#endif
    return classify_scalar_raw(input);
}

}  // namespace simdxml::simd