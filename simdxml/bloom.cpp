// Implementation of `TagBloom` non-header-dependent routines.
//
// `from_prescan` scans XML bytes for `<` positions and reads tag names
// without doing a full structural parse. `from_index` builds a bloom from
// an `XmlIndex` name table (Phase 3 dependency resolved here).
#include "bloom.hpp"
#include "index/xml_index.hpp"

#include <cstddef>
#include <cstring>

namespace rai::xml {

TagBloom TagBloom::from_prescan(std::span<std::byte const> input) noexcept {
    TagBloom bloom = TagBloom::EMPTY();
    std::size_t pos = 0;
    std::size_t const len = input.size();

    auto byte_at = [&](std::size_t i) -> unsigned char {
        return static_cast<unsigned char>(std::to_integer<std::uint8_t>(input[i]));
    };

    while (pos < len) {
        // Find next '<'.
        std::size_t remaining = len - pos;
        void const* base = static_cast<void const*>(input.data() + pos);
        auto const* p = static_cast<unsigned char const*>(
            std::memchr(base, '<', remaining));
        if (p == nullptr) break;
        pos += static_cast<std::size_t>(p - static_cast<unsigned char const*>(base)) + 1;
        if (pos >= len) break;

        unsigned char const c = byte_at(pos);
        // Skip non-element starts: </, <!, <?
        if (c == '/' || c == '!' || c == '?') continue;

        // Read tag name: bytes until whitespace, >, /.
        std::size_t name_start = pos;
        while (pos < len) {
            unsigned char const b = byte_at(pos);
            if (b == ' ' || b == '\t' || b == '\n' || b == '\r' ||
                b == '>' || b == '/') {
                break;
            }
            ++pos;
        }
        if (pos > name_start) {
            bloom.insert(input.subspan(name_start, pos - name_start));
        }
    }
    return bloom;
}

// `from_index` builds a bloom from an `XmlIndex` by iterating its name table
// (unique interned tag names). Each name is inserted into the bloom. This
// runs in O(unique_names) and is used by the persist module to embed a bloom
// in `.sxi` headers.
TagBloom TagBloom::from_index(XmlIndex const& index) {
    TagBloom bloom = TagBloom::EMPTY();
    for (auto const& [off, len] : index.name_table) {
        if (len == 0) continue;
        std::size_t const o = static_cast<std::size_t>(off);
        std::size_t const l = static_cast<std::size_t>(len);
        auto const input = index.input();
        if (o + l > input.size()) continue;
        bloom.insert(input.subspan(o, l));
    }
    return bloom;
}

}  // namespace rai::xml