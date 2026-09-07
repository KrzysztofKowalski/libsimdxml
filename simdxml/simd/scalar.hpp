// Scalar fallback structural character classifier.
//
// Ported from Rust `simd/scalar.rs`. Byte-at-a-time classification used as
// the universal fallback on platforms without a dedicated SIMD backend and
// as the trailing-bytes handler for the SIMD backends.
#pragma once

#include "simd/dispatch.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace simdxml::simd {

/// Scalar fallback: byte-at-a-time classification.
///
/// Produces a `StructuralIndex` with one u64 bitmask per 64-byte chunk for
/// each of `<` and `>`. Quote regions (`"..."` and `'...'`) are skipped so
/// that structural characters inside attribute values are not indexed.
/// The raw (`*_raw_bits`) masks record every `<`/`>` byte regardless of quote
/// state — byte-exact, matching what a memchr scan would find.
[[nodiscard]] inline StructuralIndex
classify_scalar(std::span<std::byte const> input) {
    std::size_t const len = input.size();
    std::size_t const num_chunks = (len + 63) / 64;
    StructuralIndex idx;
    idx.lt_bits.assign(num_chunks, 0);
    idx.gt_bits.assign(num_chunks, 0);
    idx.lt_raw_bits.assign(num_chunks, 0);
    idx.gt_raw_bits.assign(num_chunks, 0);
    idx.len = len;

    unsigned char in_quote = 0;  // 0 = not in quote; '"' or '\'' = in that quote

    for (std::size_t i = 0; i < len; ++i) {
        std::size_t const chunk = i / 64;
        std::size_t const bit = i % 64;
        unsigned char const byte = static_cast<unsigned char>(
            std::to_integer<std::uint8_t>(input[i]));
        std::uint64_t const bit_val = (std::uint64_t{1} << bit);

        switch (byte) {
            case '<':
                idx.lt_raw_bits[chunk] |= bit_val;
                if (in_quote == 0) idx.lt_bits[chunk] |= bit_val;
                break;
            case '>':
                idx.gt_raw_bits[chunk] |= bit_val;
                if (in_quote == 0) idx.gt_bits[chunk] |= bit_val;
                break;
            case '"':
                if (in_quote == 0) in_quote = '"';
                else if (in_quote == '"') in_quote = 0;
                break;
            case '\'':
                if (in_quote == 0) in_quote = '\'';
                else if (in_quote == '\'') in_quote = 0;
                break;
            default:
                break;
        }
    }
    return idx;
}

/// Raw scalar fallback: byte-at-a-time classification of the quote-agnostic
/// '<' / '>' masks plus the class masks the parse state machine consumes
/// (quotes, dash, bracket, whitespace, name delimiters, '=', '&').
/// `lt_bits` / `gt_bits` are left empty — see `classify_structural_raw`.
[[nodiscard]] inline StructuralIndex
classify_scalar_raw(std::span<std::byte const> input) {
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

    for (std::size_t i = 0; i < len; ++i) {
        std::size_t const chunk = i / 64;
        std::size_t const bit = i % 64;
        unsigned char const byte = static_cast<unsigned char>(
            std::to_integer<std::uint8_t>(input[i]));
        std::uint64_t const bit_val = (std::uint64_t{1} << bit);

        switch (byte) {
            case '<':
                idx.lt_raw_bits[chunk] |= bit_val;
                break;
            case '>':
                idx.gt_raw_bits[chunk] |= bit_val;
                break;
            case '"':
                idx.dq_bits[chunk] |= bit_val;
                break;
            case '\'':
                idx.sq_bits[chunk] |= bit_val;
                break;
            case '-':
                idx.dash_bits[chunk] |= bit_val;
                break;
            case ']':
                idx.rbrack_bits[chunk] |= bit_val;
                break;
            case ' ':
            case '\t':
            case '\n':
            case '\r':
                idx.ws_bits[chunk] |= bit_val;
                break;
            case '/':
                idx.slash_bits[chunk] |= bit_val;
                break;
            case '?':
                idx.qmark_bits[chunk] |= bit_val;
                break;
            case '=':
                idx.eq_bits[chunk] |= bit_val;
                break;
            case '&':
                idx.amp_bits[chunk] |= bit_val;
                break;
            default:
                break;
        }
    }
    return idx;
}

}  // namespace simdxml::simd