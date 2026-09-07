// Implementation of `.sxi` serialization / deserialization.
//
// Ported from Rust `persist/mod.rs`. The Rust crate uses `xxhash_rust::xxh3`
// for content hashing; the C++ port uses FNV-1a 64-bit, which is sufficient
// for staleness detection within the port. `.sxi` files written by Rust are
// NOT byte-compatible with this port (hash function differs).
#include "persist.hpp"

#include "../bloom.hpp"
#include "../index/structural.hpp"
#include "../index/types.hpp"
#include "../index/xml_index.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace simdxml {

namespace {

constexpr std::array<std::byte, 4> MAGIC = {std::byte{'S'}, std::byte{'X'},
                                             std::byte{'I'}, std::byte{1}};
constexpr std::uint32_t VERSION = 2;
constexpr std::size_t HEADER_SIZE = 64;
constexpr std::size_t NUM_SECTIONS = 14;
constexpr std::size_t OFFSET_TABLE_SIZE = NUM_SECTIONS * 8;

constexpr std::uint16_t FLAG_HAS_NAME_INDEX = 1;
constexpr std::uint16_t FLAG_HAS_BLOOM = 2;

[[nodiscard]] std::uint64_t
content_hash(std::span<std::byte const> data) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (auto b : data) {
        h ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b));
        h *= 0x100000001b3ULL;
    }
    return h;
}

void write_u32(std::ostream& out, std::uint32_t v) {
    std::array<std::byte, 4> buf{};
    for (int i = 0; i < 4; ++i) {
        buf[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
    out.write(reinterpret_cast<char const*>(buf.data()), 4);
}
void write_u64(std::ostream& out, std::uint64_t v) {
    std::array<std::byte, 8> buf{};
    for (int i = 0; i < 8; ++i) {
        buf[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
    out.write(reinterpret_cast<char const*>(buf.data()), 8);
}
void write_u16(std::ostream& out, std::uint16_t v) {
    std::array<std::byte, 2> buf{};
    for (int i = 0; i < 2; ++i) {
        buf[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    }
    out.write(reinterpret_cast<char const*>(buf.data()), 2);
}

[[nodiscard]] std::uint32_t
read_u32(std::span<std::byte const> data, std::size_t& pos) noexcept {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(data[pos + i])) << (8 * i);
    }
    pos += 4;
    return v;
}
[[nodiscard]] std::uint64_t
read_u64(std::span<std::byte const> data, std::size_t& pos) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(data[pos + i])) << (8 * i);
    }
    pos += 8;
    return v;
}
[[nodiscard]] std::uint16_t
read_u16(std::span<std::byte const> data, std::size_t& pos) noexcept {
    std::uint16_t v = 0;
    for (int i = 0; i < 2; ++i) {
        v |= static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(data[pos + i])) << (8 * i);
    }
    pos += 2;
    return v;
}

void write_u32_slice(std::ostream& out, std::span<std::uint32_t const> data) {
    for (auto v : data) write_u32(out, v);
}
void write_u64_slice(std::ostream& out, std::span<std::uint64_t const> data) {
    for (auto v : data) write_u64(out, v);
}
void write_u16_slice(std::ostream& out, std::span<std::uint16_t const> data) {
    for (auto v : data) write_u16(out, v);
}

[[nodiscard]] std::vector<std::uint32_t>
read_u32_vec(std::span<std::byte const> data, std::size_t count) {
    std::vector<std::uint32_t> v;
    v.reserve(count);
    std::size_t pos = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (pos + 4 > data.size()) break;
        v.push_back(read_u32(data, pos));
    }
    return v;
}
[[nodiscard]] std::vector<std::uint64_t>
read_u64_vec(std::span<std::byte const> data, std::size_t count) {
    std::vector<std::uint64_t> v;
    v.reserve(count);
    std::size_t pos = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (pos + 8 > data.size()) break;
        v.push_back(read_u64(data, pos));
    }
    return v;
}
[[nodiscard]] std::vector<std::uint16_t>
read_u16_vec(std::span<std::byte const> data, std::size_t count) {
    std::vector<std::uint16_t> v;
    v.reserve(count);
    std::size_t pos = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (pos + 2 > data.size()) break;
        v.push_back(read_u16(data, pos));
    }
    return v;
}
[[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint16_t>>
read_tag_names(std::span<std::byte const> data, std::size_t count) {
    std::vector<std::pair<std::uint64_t, std::uint16_t>> v;
    v.reserve(count);
    std::size_t pos = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (pos + 10 > data.size()) break;
        std::uint64_t off = read_u64(data, pos);
        std::uint16_t len = read_u16(data, pos);
        v.emplace_back(off, len);
    }
    return v;
}
[[nodiscard]] std::vector<TextRange>
read_text_ranges(std::span<std::byte const> data, std::size_t count) {
    std::vector<TextRange> v;
    v.reserve(count);
    std::size_t pos = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (pos + 20 > data.size()) break;
        std::uint64_t s = read_u64(data, pos);
        std::uint64_t e = read_u64(data, pos);
        std::uint32_t p = read_u32(data, pos);
        v.push_back(TextRange{s, e, p});
    }
    return v;
}

[[nodiscard]] std::size_t
compute_name_section_size(XmlIndex const& index) noexcept {
    if (index.name_ids.empty()) return 0;
    std::size_t const n = index.tag_count();
    std::size_t const name_count = index.name_table.size();
    std::size_t total_posting = 0;
    for (auto const& p : index.name_posting) total_posting += p.size();
    return n * 2 + name_count * 10 + (name_count + 1) * 4 + total_posting * 4;
}

void write_name_section(std::ostream& out, XmlIndex const& index) {
    if (index.name_ids.empty()) return;
    write_u16_slice(out, index.name_ids);
    for (auto const& [off, len] : index.name_table) {
        write_u64(out, off);
        write_u16(out, len);
    }
    std::size_t const name_count = index.name_table.size();
    std::vector<std::uint32_t> offsets;
    offsets.reserve(name_count + 1);
    std::uint32_t pos = 0;
    for (auto const& posting : index.name_posting) {
        offsets.push_back(pos);
        pos += static_cast<std::uint32_t>(posting.size());
    }
    offsets.push_back(pos);
    write_u32_slice(out, offsets);
    for (auto const& posting : index.name_posting) {
        write_u32_slice(out, posting);
    }
}

[[nodiscard]] Result<std::tuple<std::vector<std::uint16_t>,
                                std::vector<std::pair<std::uint64_t, std::uint16_t>>,
                                std::vector<std::vector<std::uint32_t>>>>
read_name_section(std::span<std::byte const> data,
                  std::size_t tag_count, std::size_t name_count) {
    std::size_t pos = 0;
    if (tag_count * 2 > data.size()) {
        return std::unexpected(SimdXmlError::invalid_sxi("name_ids truncated"));
    }
    auto name_ids = read_u16_vec(data, tag_count);
    pos += tag_count * 2;

    if (pos + name_count * 10 > data.size()) {
        return std::unexpected(SimdXmlError::invalid_sxi("name_table truncated"));
    }
    auto name_table = read_tag_names(data.subspan(pos), name_count);
    pos += name_count * 10;

    if (pos + (name_count + 1) * 4 > data.size()) {
        return std::unexpected(SimdXmlError::invalid_sxi("posting_offsets truncated"));
    }
    auto posting_offsets = read_u32_vec(data.subspan(pos), name_count + 1);
    pos += (name_count + 1) * 4;

    std::uint32_t const total_posting = posting_offsets.empty()
                                            ? 0
                                            : posting_offsets.back();
    if (pos + total_posting * 4 > data.size()) {
        return std::unexpected(SimdXmlError::invalid_sxi("posting_data truncated"));
    }
    auto posting_data = read_u32_vec(data.subspan(pos), total_posting);

    std::vector<std::vector<std::uint32_t>> name_posting;
    name_posting.reserve(name_count);
    for (std::size_t i = 0; i < name_count; ++i) {
        std::size_t const s = posting_offsets[i];
        std::size_t const e = posting_offsets[i + 1];
        if (s > e || e > posting_data.size()) {
            name_posting.emplace_back();
            continue;
        }
        name_posting.emplace_back(posting_data.begin() + s,
                                  posting_data.begin() + e);
    }
    // Result<T> = std::expected<T, SimdXmlError> is NOT an aggregate, so a
    // bare `{a, b, c}` brace-init is parsed as `Result::Result(a, b, c)`
    // (which doesn't exist) instead of `Result{tuple{a, b, c}}`. Wrap
    // explicitly in a std::tuple so the expected ctor (T const&) is used.
    return std::tuple{std::move(name_ids), std::move(name_table),
                      std::move(name_posting)};
}

[[nodiscard]] Result<std::vector<std::byte>>
read_file(std::filesystem::path const& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::unexpected(SimdXmlError::io("cannot open file"));
    std::streamsize const size = in.tellg();
    if (size < 0) return std::unexpected(SimdXmlError::io("tellg failed"));
    in.seekg(0);
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(data.data()), size);
        if (!in) return std::unexpected(SimdXmlError::io("read failed"));
    }
    return data;
}

}  // namespace

Result<TagBloom>
read_bloom(std::filesystem::path const& sxi_path) {
    auto data_result = read_file(sxi_path);
    if (!data_result) return std::unexpected(data_result.error());
    auto const& data = *data_result;
    if (data.size() < HEADER_SIZE) {
        return std::unexpected(SimdXmlError::invalid_sxi("file too small"));
    }
    if (std::memcmp(data.data(), MAGIC.data(), 4) != 0) {
        return std::unexpected(SimdXmlError::invalid_sxi("bad magic bytes"));
    }
    std::size_t pos = 26;
    std::uint16_t const flags = read_u16(data, pos);
    if ((flags & FLAG_HAS_BLOOM) == 0) {
        return TagBloom::EMPTY();
    }
    std::array<std::byte, 16> bloom_bytes{};
    std::memcpy(bloom_bytes.data(), data.data() + 28, 16);
    return TagBloom::from_bytes(bloom_bytes);
}

Result<void>
serialize_index(XmlIndex const& index,
                std::span<std::byte const> xml_bytes,
                std::filesystem::path const& sxi_path) {
    std::ofstream out(sxi_path, std::ios::binary | std::ios::trunc);
    if (!out) return std::unexpected(SimdXmlError::io("cannot create file"));

    std::uint32_t const tag_count = static_cast<std::uint32_t>(index.tag_count());
    std::uint32_t const text_count = static_cast<std::uint32_t>(index.text_count());
    bool const has_names = !index.name_ids.empty();
    std::uint16_t const name_count = has_names
        ? static_cast<std::uint16_t>(index.name_table.size()) : 0;
    std::uint16_t const flags = (has_names ? FLAG_HAS_NAME_INDEX : 0) | FLAG_HAS_BLOOM;
    std::uint64_t const xml_hash = content_hash(xml_bytes);
    TagBloom const bloom = TagBloom::from_index(index);
    auto const bloom_bytes = bloom.to_bytes();

    // Header (64 bytes), all little-endian.
    std::array<std::byte, HEADER_SIZE> header{};
    std::memcpy(header.data(), MAGIC.data(), 4);
    for (int i = 0; i < 4; ++i) {
        header[4 + i] = static_cast<std::byte>((VERSION >> (8 * i)) & 0xFF);
    }
    for (int i = 0; i < 8; ++i) {
        header[8 + i] = static_cast<std::byte>((xml_hash >> (8 * i)) & 0xFF);
    }
    for (int i = 0; i < 4; ++i) {
        header[16 + i] = static_cast<std::byte>((tag_count >> (8 * i)) & 0xFF);
    }
    for (int i = 0; i < 4; ++i) {
        header[20 + i] = static_cast<std::byte>((text_count >> (8 * i)) & 0xFF);
    }
    for (int i = 0; i < 2; ++i) {
        header[24 + i] = static_cast<std::byte>((name_count >> (8 * i)) & 0xFF);
    }
    for (int i = 0; i < 2; ++i) {
        header[26 + i] = static_cast<std::byte>((flags >> (8 * i)) & 0xFF);
    }
    std::memcpy(header.data() + 28, bloom_bytes.data(), 16);
    // bytes 44..64: padding (already zero)
    out.write(reinterpret_cast<char const*>(header.data()), HEADER_SIZE);

    // Compute section sizes and offsets.
    std::array<std::size_t, NUM_SECTIONS> section_sizes{};
    section_sizes[0]  = index.tag_starts.size() * 8;
    section_sizes[1]  = index.tag_ends.size() * 8;
    section_sizes[2]  = index.tag_types.size();
    section_sizes[3]  = index.tag_names.size() * 10;
    section_sizes[4]  = index.depths.size() * 2;
    section_sizes[5]  = index.parents.size() * 4;
    section_sizes[6]  = index.text_ranges.size() * 20;
    section_sizes[7]  = index.child_offsets.size() * 4;
    section_sizes[8]  = index.child_data.size() * 4;
    section_sizes[9]  = index.text_child_offsets.size() * 4;
    section_sizes[10] = index.text_child_data.size() * 4;
    section_sizes[11] = index.close_map.size() * 4;
    section_sizes[12] = index.post_order.size() * 4;
    section_sizes[13] = compute_name_section_size(index);

    std::array<std::uint64_t, NUM_SECTIONS> offsets{};
    std::uint64_t p = HEADER_SIZE + OFFSET_TABLE_SIZE;
    for (std::size_t i = 0; i < NUM_SECTIONS; ++i) {
        offsets[i] = p;
        p += section_sizes[i];
    }
    for (auto off : offsets) write_u64(out, off);

    // Sections.
    write_u64_slice(out, index.tag_starts);
    write_u64_slice(out, index.tag_ends);
    for (auto tt : index.tag_types) out.put(static_cast<char>(static_cast<std::uint8_t>(tt)));
    for (auto const& [off, len] : index.tag_names) {
        write_u64(out, off);
        write_u16(out, len);
    }
    write_u16_slice(out, index.depths);
    write_u32_slice(out, index.parents);
    for (auto const& r : index.text_ranges) {
        write_u64(out, r.start);
        write_u64(out, r.end);
        write_u32(out, r.parent_tag);
    }
    write_u32_slice(out, index.child_offsets);
    write_u32_slice(out, index.child_data);
    write_u32_slice(out, index.text_child_offsets);
    write_u32_slice(out, index.text_child_data);
    write_u32_slice(out, index.close_map);
    write_u32_slice(out, index.post_order);
    write_name_section(out, index);

    out.flush();
    if (!out) return std::unexpected(SimdXmlError::io("write failed"));
    return {};
}

namespace {

[[nodiscard]] Result<OwnedXmlIndex>
load_index_with_xml(std::span<std::byte const> sxi_bytes,
                    std::vector<std::byte> xml_data) {
    if (sxi_bytes.size() < HEADER_SIZE + OFFSET_TABLE_SIZE) {
        return std::unexpected(SimdXmlError::invalid_sxi("file too small"));
    }
    if (std::memcmp(sxi_bytes.data(), MAGIC.data(), 4) != 0) {
        return std::unexpected(SimdXmlError::invalid_sxi("bad magic bytes"));
    }
    std::size_t pos = 4;
    std::uint32_t const version = read_u32(sxi_bytes, pos);
    if (version != VERSION) {
        return std::unexpected(SimdXmlError::invalid_sxi(
            "unsupported version " + std::to_string(version)));
    }
    std::uint64_t const stored_hash = read_u64(sxi_bytes, pos);
    std::span<std::byte const> const xml_span(xml_data);
    std::uint64_t const actual_hash = content_hash(xml_span);
    if (stored_hash != actual_hash) {
        return std::unexpected(SimdXmlError::stale_sxi());
    }

    std::uint32_t const tag_count = read_u32(sxi_bytes, pos);
    std::uint32_t const text_count = read_u32(sxi_bytes, pos);
    std::uint16_t const name_count = read_u16(sxi_bytes, pos);
    std::uint16_t const flags = read_u16(sxi_bytes, pos);
    bool const has_names = (flags & FLAG_HAS_NAME_INDEX) != 0;

    // Offset table.
    std::array<std::uint64_t, NUM_SECTIONS> offsets{};
    for (std::size_t i = 0; i < NUM_SECTIONS; ++i) {
        offsets[i] = read_u64(sxi_bytes, pos);
    }

    auto section = [&](std::size_t i) -> std::span<std::byte const> {
        std::size_t const s = static_cast<std::size_t>(offsets[i]);
        std::size_t const e = (i + 1 < NUM_SECTIONS)
            ? static_cast<std::size_t>(offsets[i + 1])
            : sxi_bytes.size();
        if (s > sxi_bytes.size() || e > sxi_bytes.size() || s > e) return {};
        return sxi_bytes.subspan(s, e - s);
    };
    auto section_len = [&](std::size_t i) -> std::size_t {
        std::size_t const s = static_cast<std::size_t>(offsets[i]);
        std::size_t const e = (i + 1 < NUM_SECTIONS)
            ? static_cast<std::size_t>(offsets[i + 1])
            : sxi_bytes.size();
        if (s > sxi_bytes.size() || e > sxi_bytes.size() || s > e) return 0;
        return e - s;
    };

    XmlIndex index(xml_span);
    index.tag_starts = read_u64_vec(section(0), tag_count);
    index.tag_ends = read_u64_vec(section(1), tag_count);

    auto const tt_bytes = section(2);
    index.tag_types.reserve(tag_count);
    for (std::size_t i = 0; i < tag_count && i < tt_bytes.size(); ++i) {
        index.tag_types.push_back(
            tag_type_from_u8(std::to_integer<std::uint8_t>(tt_bytes[i]))
                .value_or(TagType::Open));
    }

    index.tag_names = read_tag_names(section(3), tag_count);
    index.depths = read_u16_vec(section(4), tag_count);
    index.parents = read_u32_vec(section(5), tag_count);
    index.text_ranges = read_text_ranges(section(6), text_count);

    index.child_offsets = read_u32_vec(section(7), section_len(7) / 4);
    index.child_data = read_u32_vec(section(8), section_len(8) / 4);
    index.text_child_offsets = read_u32_vec(section(9), section_len(9) / 4);
    index.text_child_data = read_u32_vec(section(10), section_len(10) / 4);
    index.close_map = read_u32_vec(section(11), tag_count);
    index.post_order = read_u32_vec(section(12), tag_count);

    if (has_names && name_count > 0) {
        auto name_result = read_name_section(section(13), tag_count, name_count);
        if (!name_result) return std::unexpected(name_result.error());
        auto& [ids, table, posting] = *name_result;
        index.name_ids = std::move(ids);
        index.name_table = std::move(table);
        index.name_posting = std::move(posting);
    }

    return OwnedXmlIndex(std::move(xml_data), std::move(index));
}

}  // namespace

Result<OwnedXmlIndex>
load_index_with_bytes(std::filesystem::path const& sxi_path,
                      std::vector<std::byte> xml_bytes) {
    auto sxi_result = read_file(sxi_path);
    if (!sxi_result) return std::unexpected(sxi_result.error());
    return load_index_with_xml(*sxi_result, std::move(xml_bytes));
}

Result<OwnedXmlIndex>
load_index(std::filesystem::path const& sxi_path,
           std::filesystem::path const& xml_path) {
    auto xml_result = read_file(xml_path);
    if (!xml_result) return std::unexpected(xml_result.error());
    return load_index_with_bytes(sxi_path, std::move(*xml_result));
}

Result<OwnedXmlIndex>
load_or_parse(std::filesystem::path const& xml_path) {
    auto sxi_path = xml_path;
    sxi_path.replace_extension(".sxi");

    auto xml_result = read_file(xml_path);
    if (!xml_result) return std::unexpected(xml_result.error());
    auto xml_bytes = std::move(*xml_result);
    auto xml_span = std::span<std::byte const>(xml_bytes);

    if (std::filesystem::exists(sxi_path)) {
        // Copy bytes into the loader (it needs to own them for the lifetime
        // of the returned OwnedXmlIndex). On failure, our local xml_bytes
        // remains intact and we fall through to the parse path.
        auto load = load_index_with_bytes(sxi_path, std::vector<std::byte>(xml_bytes));
        if (load) return load;
        // Stale / corrupt — fall through to re-parse.
    }

    auto parse_result = parse_scalar(xml_span);
    if (!parse_result) return std::unexpected(parse_result.error());
    auto index = std::move(*parse_result);

    auto save = serialize_index(index, xml_span, sxi_path);
    if (!save) {
        // Failed to save — return the parsed index anyway; next call will
        // try to serialize again.
    }
    return OwnedXmlIndex(std::move(xml_bytes), std::move(index));
}

}  // namespace simdxml