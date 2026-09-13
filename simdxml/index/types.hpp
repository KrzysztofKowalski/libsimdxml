// Shared structural-index types — TagType, TextRange, NameInterner.
//
// Ported from Rust `index/mod.rs` (TagType, TextRange, NameInterner). Kept in
// a separate header so that `structural.cpp`, `lazy.cpp`, `xml_index.cpp` can
// all reference the same tag-classification enum without dragging in the full
// `XmlIndex` definition.
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// AVX2 fast path for short-name comparison (x86_64, GCC/Clang — same
// per-function target model as simd/dispatch.hpp; MSVC keeps the scalar
// walk).
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define SIMDXML_TYPES_HAS_AVX2_COMPARE 1
#include <immintrin.h>
#endif

namespace rai::xml {

/// Tag type classification. Mirrors Rust `TagType` enum (`#[repr(u8)]`).
enum class TagType : std::uint8_t {
    Open      = 0,  ///< `<tag>` or `<tag attr="val">`
    Close     = 1,  ///< `</tag>`
    SelfClose = 2,  ///< `<tag/>`
    Comment   = 3,  ///< `<!-- ... -->`
    CData     = 4,  ///< `<![CDATA[ ... ]]>`
    PI        = 5,  ///< `<?target ... ?>`
};

/// Convert from u8 (for deserialization). Returns std::nullopt for invalid.
[[nodiscard]] inline std::optional<TagType> tag_type_from_u8(std::uint8_t v) noexcept {
    switch (v) {
        case 0: return TagType::Open;
        case 1: return TagType::Close;
        case 2: return TagType::SelfClose;
        case 3: return TagType::Comment;
        case 4: return TagType::CData;
        case 5: return TagType::PI;
        default: return std::nullopt;
    }
}

/// A text content range between tags.
///
/// `parent_tag` is the index of the innermost enclosing open tag in the
/// `XmlIndex` arrays, or `UINT32_MAX` for text at the document root level.
struct TextRange {
    std::uint64_t start = 0;       ///< Byte offset of text start in the XML input.
    std::uint64_t end = 0;         ///< Byte offset of text end (exclusive).
    std::uint32_t parent_tag = UINT32_MAX;  ///< Index of parent open tag.

    bool operator==(TextRange const&) const noexcept = default;
};

/// Tag-name interning with a small-table / hash-map threshold.
///
/// Mirrors Rust `NameInterner`. Linear scan below 256 entries, switches to
/// an FNV-1a hash map at 256 entries. On hash collision, falls back to linear
/// scan for correctness. Tag names are stored as `(byte_offset, length)` pairs
/// into the XML input — zero copy.
class NameInterner {
public:
    explicit NameInterner(std::span<std::byte const> input)
        : input_(input)
#if SIMDXML_TYPES_HAS_AVX2_COMPARE
          ,
          avx2_(avx2_available())
#endif
    {
        table_.reserve(64);
    }

    /// Intern a name, returning its u16 ID. Clamps to u16 max on overflow.
    [[nodiscard]] std::uint16_t intern(std::span<std::byte const> name_bytes,
                                       std::uint64_t offset,
                                       std::uint16_t len) {
        // Last-name cache: XML overwhelmingly repeats the immediately
        // preceding tag name (measured: 87% of tags match the previous one,
        // and beyond that names jump far back), so a single-slot check first
        // replaces the table scan for most tags. Exact-byte match returns the
        // same ID the scan would (names in table_ are unique) — output is
        // bit-identical.
        if (len == last_len_ &&
            bytes_equal(last_off_, last_len_, name_bytes)) {
            return last_id_;
        }
        std::uint16_t const id = intern_uncached(name_bytes, offset, len);
        last_off_ = offset;
        last_len_ = len;
        last_id_ = id;
        return id;
    }

    [[nodiscard]] std::uint16_t
    intern_uncached(std::span<std::byte const> name_bytes, std::uint64_t offset,
                    std::uint16_t len) {
        if (map_) {
            std::uint64_t const hash = hash_bytes(name_bytes);
            if (auto it = map_->find(hash); it != map_->end()) {
                auto const id = it->second;
                auto const& [off, l] = table_[id];
                if (l == len && bytes_equal(off, l, name_bytes)) {
                    return id;
                }
                // Hash collision — fall back to linear scan.
                for (std::size_t i = 0; i < table_.size(); ++i) {
                    auto const& [off2, l2] = table_[i];
                    if (l2 == len && bytes_equal(off2, l2, name_bytes)) {
                        return static_cast<std::uint16_t>(i);
                    }
                }
                std::uint16_t const new_id = static_cast<std::uint16_t>(
                    std::min(table_.size(), static_cast<std::size_t>(UINT16_MAX)));
                table_.emplace_back(offset, len);
                return new_id;
            }
            // No map entry — insert.
            std::uint16_t const id = static_cast<std::uint16_t>(
                std::min(table_.size(), static_cast<std::size_t>(UINT16_MAX)));
            table_.emplace_back(offset, len);
            map_->emplace(hash, id);
            return id;
        }

        // Linear scan for small tables.
        for (std::size_t i = 0; i < table_.size(); ++i) {
            auto const& [off, l] = table_[i];
            if (l == len && bytes_equal(off, l, name_bytes)) {
                return static_cast<std::uint16_t>(i);
            }
        }
        std::uint16_t const id = static_cast<std::uint16_t>(table_.size());
        table_.emplace_back(offset, len);

        // Switch to hash map at threshold.
        if (table_.size() == 256) {
            std::unordered_map<std::uint64_t, std::uint16_t> m;
            m.reserve(512);
            for (std::size_t i = 0; i < table_.size(); ++i) {
                auto const& [off, l] = table_[i];
                m.emplace(hash_bytes(slice(off, l)), static_cast<std::uint16_t>(i));
            }
            map_ = std::move(m);
        }
        return id;
    }

    /// Take ownership of the built table — name_id -> (byte_offset, length).
    [[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint16_t>>
    into_table() && {
        return std::move(table_);
    }

private:
    std::span<std::byte const> input_;
    std::vector<std::pair<std::uint64_t, std::uint16_t>> table_;
    std::optional<std::unordered_map<std::uint64_t, std::uint16_t>> map_;
    // 4 most-recent interned table indices (SIZE_MAX = empty slot); recent_pos_
    // indexes the next slot to overwrite.
    // Last interned name (single-slot cache, see intern()): intern is only
    // called with len > 0, so last_len_ = 0 never matches.
    std::uint64_t last_off_ = 0;
    std::uint16_t last_len_ = 0;
    std::uint16_t last_id_ = 0;
#if SIMDXML_TYPES_HAS_AVX2_COMPARE
    // Cached once per interner: a static-local guard check per bytes_equal
    // call (atomic load + branch) cost ~2 ms per 10MB when interning 337k
    // short names.
    bool avx2_ = false;
#endif

    /// Name hash: names up to 8 bytes (the overwhelming majority) are packed
    /// into one word and mixed with the length; longer names keep the FNV-1a
    /// walk. The hash only buckets the lookup table — interned IDs depend on
    /// first-seen order and the exact byte comparison, so the choice of hash
    /// never changes the interner's output.
    [[nodiscard]] static std::uint64_t
    hash_bytes(std::span<std::byte const> bytes) noexcept {
        if (bytes.size() <= 8) {
            std::uint64_t v = 0;
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                v |= static_cast<std::uint64_t>(
                         std::to_integer<std::uint8_t>(bytes[i]))
                     << (8 * i);
            }
            v ^= static_cast<std::uint64_t>(bytes.size()) * 0x9E3779B97F4A7C15ULL;
            v *= 0xC2B2AE3D27D4EB4FULL;
            v ^= v >> 29;
            v *= 0x165667B19E3779F9ULL;
            v ^= v >> 32;
            return v;
        }
        return fnv1a(bytes);
    }

    [[nodiscard]] static std::uint64_t
    fnv1a(std::span<std::byte const> bytes) noexcept {
        std::uint64_t h = 0xcbf29ce484222325ULL;
        for (auto b : bytes) {
            h ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b));
            h *= 0x100000001b3ULL;
        }
        return h;
    }

#if SIMDXML_TYPES_HAS_AVX2_COMPARE
    [[nodiscard]] static bool avx2_available() noexcept {
        static bool const cached = []() {
#if defined(__GNUC__) && !defined(__clang__)
            __builtin_cpu_init();
#endif
            return __builtin_cpu_supports("avx2") != 0;
        }();
        return cached;
    }

    /// Exact equality of the first n bytes (n <= 32): one 32-byte compare,
    /// movemask, then a check that every position below n matches (bits at
    /// or above n are out-of-name garbage and masked off).
    [[gnu::target("avx2")]] static bool
    bytes_equal_avx2(std::byte const* a, std::byte const* b,
                     std::size_t n) noexcept {
        __m256i const va =
            _mm256_loadu_si256(reinterpret_cast<__m256i const*>(a));
        __m256i const vb =
            _mm256_loadu_si256(reinterpret_cast<__m256i const*>(b));
        std::uint32_t const mm = static_cast<std::uint32_t>(
            _mm256_movemask_epi8(_mm256_cmpeq_epi8(va, vb)));
        std::uint32_t const need =
            (n == 32) ? 0xFFFFFFFFu : ((1u << n) - 1u);
        return (mm & need) == need;
    }
#endif

    /// Scalar walk; AVX2 single-compare fast path for short names with at
    /// least 32 readable bytes on both sides.
    [[nodiscard]] bool
    bytes_equal_fast(std::byte const* a, std::byte const* b, std::size_t n,
                     std::byte const* input_end) const noexcept {
        if (n == 0) return true;
#if SIMDXML_TYPES_HAS_AVX2_COMPARE
        // AVX2 only pays off at >= 16 bytes: below that the two 32B loads plus
        // movemask cost more than the 1-3 scalar iterations an early-exit walk
        // needs for typical short tag names (measured: 8 vs 6 ms per 10MB of
        // interning with 9 unique names).
        if (avx2_ && n >= 16 && n <= 32 && (input_end - a) >= 32 &&
            (input_end - b) >= 32) {
            return bytes_equal_avx2(a, b, n);
        }
#else
        (void)input_end;
#endif
        for (std::size_t i = 0; i < n; ++i) {
            if (a[i] != b[i]) return false;
        }
        return true;
    }

    [[nodiscard]] std::span<std::byte const>
    slice(std::uint64_t off, std::uint16_t len) const noexcept {
        std::size_t const o = static_cast<std::size_t>(off);
        std::size_t const l = static_cast<std::size_t>(len);
        if (o + l > input_.size()) return {};
        return input_.subspan(o, l);
    }

    [[nodiscard]] bool
    bytes_equal(std::uint64_t off, std::uint16_t len,
                std::span<std::byte const> name_bytes) const noexcept {
        auto const s = slice(off, len);
        if (s.size() != name_bytes.size()) return false;
        return bytes_equal_fast(s.data(), name_bytes.data(), s.size(),
                                input_.data() + input_.size());
    }
};

}  // namespace rai::xml