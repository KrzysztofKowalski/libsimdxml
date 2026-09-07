// Per-document bloom filter of tag names.
//
// Ported from Rust `bloom.rs`. 128-bit bloom filter with 2 hash functions
// (~2% FPR for 20 tag names). Used to skip files that cannot match a query
// without parsing them.
//
// The C++ port keeps the on-disk layout identical: 16 little-endian bytes.
// `from_index` is declared but not defined here because `XmlIndex` is
// forward-declared only (defined in a later phase).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace simdxml {

class XmlIndex;  // forward declaration — defined in index module

/// 128-bit bloom filter for tag names.
///
/// Stores 2 FNV-1a hashes per name, mod 128. False positives are possible;
/// false negatives are not.
class TagBloom {
public:
    /// Empty bloom filter.
    static constexpr TagBloom EMPTY() noexcept { return TagBloom{0, 0}; }

    TagBloom() = default;
    explicit constexpr TagBloom(std::uint64_t lo, std::uint64_t hi) noexcept
        : lo_(lo), hi_(hi) {}

    /// Insert a tag name into the bloom filter.
    void insert(std::span<std::byte const> name) noexcept {
        auto [h1, h2] = hash_pair(name);
        set_bit(h1 % 128);
        set_bit(h2 % 128);
    }

    /// Convenience overload for string literals / `std::string_view`.
    void insert(std::string_view name) noexcept {
        insert(std::span<std::byte const>(
            reinterpret_cast<std::byte const*>(name.data()), name.size()));
    }

    /// Check if a tag name might be in the bloom filter.
    /// False positives are possible; false negatives are not.
    [[nodiscard]] bool may_contain(std::span<std::byte const> name) const noexcept {
        auto [h1, h2] = hash_pair(name);
        std::uint64_t mask_lo = 0, mask_hi = 0;
        set_bit_into(h1 % 128, mask_lo, mask_hi);
        set_bit_into(h2 % 128, mask_lo, mask_hi);
        return (lo_ & mask_lo) == mask_lo && (hi_ & mask_hi) == mask_hi;
    }

    [[nodiscard]] bool may_contain(std::string_view name) const noexcept {
        return may_contain(std::span<std::byte const>(
            reinterpret_cast<std::byte const*>(name.data()), name.size()));
    }

    /// Check if the bloom filter may contain any of the given names.
    [[nodiscard]] bool may_contain_any(std::span<std::string_view const> names) const noexcept {
        for (auto const& name : names) {
            if (may_contain(name)) return true;
        }
        return false;
    }

    /// Build a bloom filter by fast-scanning XML bytes without full parsing.
    ///
    /// Scans for `<` positions, reads tag names, inserts into bloom.
    /// Runs at near-memchr speed with minimal per-tag work.
    [[nodiscard]] static TagBloom from_prescan(std::span<std::byte const> input) noexcept;

    /// Build a bloom filter from an `XmlIndex` by scanning its name table.
    /// Defined in `bloom.cpp` once `XmlIndex` is available.
    [[nodiscard]] static TagBloom from_index(XmlIndex const& index);

    /// Serialize to 16 bytes (little-endian).
    [[nodiscard]] std::array<std::byte, 16> to_bytes() const noexcept {
        std::array<std::byte, 16> out{};
        for (int i = 0; i < 8; ++i) {
            out[i] = static_cast<std::byte>((lo_ >> (8 * i)) & 0xFF);
            out[8 + i] = static_cast<std::byte>((hi_ >> (8 * i)) & 0xFF);
        }
        return out;
    }

    /// Deserialize from 16 bytes (little-endian).
    [[nodiscard]] static TagBloom from_bytes(std::array<std::byte, 16> const& bytes) noexcept {
        std::uint64_t lo = 0, hi = 0;
        for (int i = 0; i < 8; ++i) {
            lo |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[i])) << (8 * i);
            hi |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[8 + i])) << (8 * i);
        }
        return TagBloom{lo, hi};
    }

    bool operator==(TagBloom const&) const noexcept = default;

    std::uint64_t low_word() const noexcept { return lo_; }
    std::uint64_t high_word() const noexcept { return hi_; }

private:
    std::uint64_t lo_ = 0;  ///< bits 0..63
    std::uint64_t hi_ = 0;  ///< bits 64..127

    void set_bit(unsigned bit) noexcept {
        if (bit < 64) {
            lo_ |= (std::uint64_t{1} << bit);
        } else {
            hi_ |= (std::uint64_t{1} << (bit - 64));
        }
    }

    static void set_bit_into(unsigned bit, std::uint64_t& lo, std::uint64_t& hi) noexcept {
        if (bit < 64) {
            lo |= (std::uint64_t{1} << bit);
        } else {
            hi |= (std::uint64_t{1} << (bit - 64));
        }
    }

    /// Two independent FNV-1a hash functions with different seeds.
    [[nodiscard]] static std::pair<std::uint32_t, std::uint32_t>
    hash_pair(std::span<std::byte const> name) noexcept {
        std::uint32_t h1 = 0x811c9dc5u;
        std::uint32_t h2 = 0x050c5d1fu;
        constexpr std::uint32_t prime = 0x01000193u;
        for (std::byte b : name) {
            std::uint32_t const byte = std::to_integer<std::uint32_t>(b);
            h1 ^= byte;
            h1 = h1 * prime;
            h2 ^= byte;
            h2 = h2 * prime;
        }
        return {h1, h2};
    }
};

}  // namespace simdxml