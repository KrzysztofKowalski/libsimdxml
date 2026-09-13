// Persistent structural index — serialize to `.sxi`, load back.
//
// Ported from Rust `persist/mod.rs`. The `.sxi` (SIMD XML Index) format
// stores the complete `XmlIndex` as flat arrays in a single file. On
// subsequent loads, the XML bytes and `.sxi` arrays are read into memory,
// avoiding the entire parse pipeline.
//
// File format (little-endian):
//   [Header: 64 bytes]
//     magic: [u8; 4]    = b"SXI\x01"
//     version: u32       = 2
//     xml_hash: u64      = content hash of XML bytes
//     tag_count: u32
//     text_count: u32
//     name_count: u16
//     flags: u16         = bit 0: has_name_index, bit 1: has_bloom
//     bloom: [u8; 16]
//     padding: [u8; 16]
//   [Offset table: 14 x u64]  byte offsets of each section
//   [Sections 0..13]
//
// The C++ port uses FNV-1a 64-bit for the content hash (Rust uses xxh3-64).
// The hash is only used for staleness detection within the C++ port; .sxi
// files written by Rust are NOT compatible (different hash function).
#pragma once

#include "../bloom.hpp"
#include "../error.hpp"
#include "../index/xml_index.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rai::xml {

/// A self-contained index that owns both the XML bytes and the structural index.
///
/// Mirrors Rust `OwnedXmlIndex`. The XML bytes are stored in a `std::vector`
/// declared before the `XmlIndex` so the vector is destroyed after the index
/// (C++ destroys members in reverse declaration order). The `XmlIndex` holds
/// a `std::span<std::byte const>` referencing the vector's buffer.
class OwnedXmlIndex {
public:
    OwnedXmlIndex(std::vector<std::byte> xml_data, XmlIndex index)
        : xml_data_(std::move(xml_data)), index_(std::move(index)) {}

    XmlIndex const& as_index() const noexcept { return index_; }
    XmlIndex const& operator*() const noexcept { return index_; }
    XmlIndex const* operator->() const noexcept { return &index_; }

private:
    std::vector<std::byte> xml_data_;
    XmlIndex index_;
};

/// Read just the bloom filter from an `.sxi` file header.
/// Very fast (reads only 64 bytes).
[[nodiscard]] Result<TagBloom>
read_bloom(std::filesystem::path const& sxi_path);

/// Serialize an `XmlIndex` to a `.sxi` file.
[[nodiscard]] Result<void>
serialize_index(XmlIndex const& index,
                std::span<std::byte const> xml_bytes,
                std::filesystem::path const& sxi_path);

/// Load a `.sxi` index file and the corresponding XML file (read into memory).
[[nodiscard]] Result<OwnedXmlIndex>
load_index(std::filesystem::path const& sxi_path,
           std::filesystem::path const& xml_path);

/// Load a `.sxi` index using XML bytes already in memory.
[[nodiscard]] Result<OwnedXmlIndex>
load_index_with_bytes(std::filesystem::path const& sxi_path,
                      std::vector<std::byte> xml_bytes);

/// Load a pre-built `.sxi` index if it exists and is fresh, otherwise parse
/// and save the index for next time. Returns an `OwnedXmlIndex`.
[[nodiscard]] Result<OwnedXmlIndex>
load_or_parse(std::filesystem::path const& xml_path);

}  // namespace rai::xml