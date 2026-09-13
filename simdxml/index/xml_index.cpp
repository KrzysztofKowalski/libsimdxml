// Implementation of `XmlIndex` methods + free-function CSR builders.
//
// Ported from Rust `index/mod.rs` (`impl XmlIndex`, `build_csr_children`,
// `build_csr_text_children`, `build_close_map_and_post_order`,
// `c14n_element`, `c14n_escape_text`, `c14n_escape_attr`,
// `decode_entities`).
#include "xml_index.hpp"

#include "../simd/dispatch.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <iterator>
#include <numeric>
#include <optional>
#include <ranges>
#include <thread>
#include <utility>

namespace rai::xml {

namespace {

constexpr std::uint32_t U32_SENTINEL = UINT32_MAX;

// Unused helpers kept for reference (ported from Rust index/mod.rs).
/* unused — silenced for -Wunused-function */
#if 0
[[nodiscard]] std::string_view
span_to_view(std::span<std::byte const> s) noexcept {
    return std::string_view(reinterpret_cast<char const*>(s.data()), s.size());
}

[[nodiscard]] bool
is_ws(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
#endif

/// C14N-escape text content. Ampersand, less-than, greater-than, CR.
void c14n_escape_text(std::string_view text, std::string& out) {
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '\r': out += "&#xD;"; break;
            default: out.push_back(c); break;
        }
    }
}

/// C14N-escape attribute value. Ampersand, less-than, double-quote, tab,
/// newline, CR.
void c14n_escape_attr(std::string_view text, std::string& out) {
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '"': out += "&quot;"; break;
            case '\t': out += "&#x9;"; break;
            case '\n': out += "&#xA;"; break;
            case '\r': out += "&#xD;"; break;
            default: out.push_back(c); break;
        }
    }
}

}  // namespace

std::uint16_t XmlIndex::max_depth() const noexcept {
    std::uint16_t m = 0;
    for (auto d : depths) m = std::max(m, d);
    return m;
}

std::string_view
XmlIndex::slice_str(std::size_t start, std::size_t end) const noexcept {
    if (start >= input_.size() || end > input_.size() || start > end) {
        return {};
    }
    return std::string_view(reinterpret_cast<char const*>(input_.data() + start),
                            end - start);
}

std::string_view XmlIndex::tag_name(std::size_t idx) const noexcept {
    if (idx >= tag_names.size()) return {};
    auto const [off, len] = tag_names[idx];
    return slice_str(static_cast<std::size_t>(off),
                     static_cast<std::size_t>(off) + len);
}

bool XmlIndex::tag_name_eq(std::size_t idx, std::string_view name) const noexcept {
    if (idx >= tag_names.size()) return false;
    auto const [off, len] = tag_names[idx];
    if (name.size() != len) return false;
    auto const s = slice_str(static_cast<std::size_t>(off),
                              static_cast<std::size_t>(off) + len);
    return s == name;
}

void XmlIndex::ensure_indices() {
    if (!has_indices() && tag_count() >= 1) {
        build_indices();
    }
}

void XmlIndex::build_indices() {
    std::size_t const n = tag_count();
    bool const need_close_map = close_map.empty();

    // Threshold: thread spawn costs ~25us. Below 10K tags, sequential is faster.
    // Measured on the 1MB corpus (33.8k tags): jthreads 0.25ms vs sequential
    // 0.33ms — two parallel CSR builds win even at this size.
    if (n < 10'000) {
        auto [co, cd] = build_csr_children(tag_types, parents, n);
        auto [tco, tcd] = build_csr_text_children(text_ranges, n);
        child_offsets = std::move(co);
        child_data = std::move(cd);
        text_child_offsets = std::move(tco);
        text_child_data = std::move(tcd);
        if (need_close_map) {
            auto [cm, po] = build_close_map_and_post_order(tag_types, n);
            close_map = std::move(cm);
            post_order = std::move(po);
        }
        return;
    }

    // Large documents: run CSR builds concurrently via std::jthread.
    // Capture copies of spans — they remain valid for the duration of the scope.
    auto const tag_types_span = std::span<TagType const>(tag_types);
    auto const parents_span = std::span<std::uint32_t const>(parents);
    auto const text_ranges_span = std::span<TextRange const>(text_ranges);

    std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>> csr_c;
    std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>> csr_t;
    std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>> cm_po;

    std::jthread t1([&] {
        csr_c = build_csr_children(tag_types_span, parents_span, n);
    });
    std::jthread t2([&] {
        csr_t = build_csr_text_children(text_ranges_span, n);
    });
    // close_map stays on the main thread: this build targets 2 physical cores
    // (Haswell 2C/4T) and a third concurrent worker measured SLOWER than
    // running it here (ensure 2.5ms -> 3.0-3.6ms per 10MB — memory contention,
    // csr_text is bandwidth-bound).
    if (need_close_map) {
        cm_po = build_close_map_and_post_order(tag_types_span, n);
    }
    t1.join();
    t2.join();

    child_offsets = std::move(csr_c.first);
    child_data = std::move(csr_c.second);
    text_child_offsets = std::move(csr_t.first);
    text_child_data = std::move(csr_t.second);
    if (need_close_map) {
        close_map = std::move(cm_po.first);
        post_order = std::move(cm_po.second);
    }
}

std::span<std::uint32_t const>
XmlIndex::child_tag_slice(std::size_t parent_idx) const noexcept {
    if (child_offsets.size() < 2 || parent_idx + 1 >= child_offsets.size()) {
        return {};
    }
    std::size_t const start = child_offsets[parent_idx];
    std::size_t const end = child_offsets[parent_idx + 1];
    if (start > child_data.size() || end > child_data.size() || start > end) return {};
    return std::span<std::uint32_t const>(child_data.data() + start, end - start);
}

std::span<std::uint32_t const>
XmlIndex::child_text_slice(std::size_t parent_idx) const noexcept {
    if (text_child_offsets.size() < 2 || parent_idx + 1 >= text_child_offsets.size()) {
        return {};
    }
    std::size_t const start = text_child_offsets[parent_idx];
    std::size_t const end = text_child_offsets[parent_idx + 1];
    if (start > text_child_data.size() || end > text_child_data.size() || start > end) {
        return {};
    }
    return std::span<std::uint32_t const>(text_child_data.data() + start, end - start);
}

bool XmlIndex::is_ancestor(std::size_t a, std::size_t d) const noexcept {
    if (post_order.empty()) return false;
    return a < d && post_order[a] > post_order[d];
}

void XmlIndex::build_name_index() {
    if (!name_posting.empty()) return;
    std::size_t const n = tag_count();
    NameInterner interner(input_);
    name_ids.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        auto const [off, len] = tag_names[i];
        if (len > 0) {
            std::size_t const o = static_cast<std::size_t>(off);
            std::size_t const l = static_cast<std::size_t>(len);
            if (o + l > input_.size()) {
                name_ids.push_back(UINT16_MAX);
                continue;
            }
            auto const slice = input_.subspan(o, l);
            name_ids.push_back(interner.intern(slice, off, len));
        } else {
            name_ids.push_back(UINT16_MAX);
        }
    }
    name_table = std::move(interner).into_table();
    std::size_t const num_names = name_table.size();
    std::vector<std::vector<std::uint32_t>> posting(num_names);
    for (std::size_t i = 0; i < n; ++i) {
        auto const nid = name_ids[i];
        if (nid != UINT16_MAX && static_cast<std::size_t>(nid) < num_names) {
            auto const tt = tag_types[i];
            if (tt == TagType::Open || tt == TagType::SelfClose) {
                posting[nid].push_back(static_cast<std::uint32_t>(i));
            }
        }
    }
    name_posting = std::move(posting);
}

std::optional<std::uint16_t>
XmlIndex::name_id(std::string_view name) const noexcept {
    auto const name_bytes = std::as_bytes(std::span{name.data(), name.size()});
    for (std::size_t id = 0; id < name_table.size(); ++id) {
        auto const [off, len] = name_table[id];
        if (static_cast<std::size_t>(len) != name.size()) continue;
        std::size_t const o = static_cast<std::size_t>(off);
        std::size_t const l = static_cast<std::size_t>(len);
        if (o + l > input_.size()) continue;
        auto const s = input_.subspan(o, l);
        bool match = s.size() == name_bytes.size();
        for (std::size_t i = 0; match && i < s.size(); ++i) {
            if (s[i] != name_bytes[i]) match = false;
        }
        if (match) return static_cast<std::uint16_t>(id);
    }
    return std::nullopt;
}

std::span<std::uint32_t const>
XmlIndex::tags_by_name(std::string_view name) const noexcept {
    if (auto const id = name_id(name)) {
        std::size_t const i = *id;
        if (i < name_posting.size()) {
            auto const& v = name_posting[i];
            return std::span<std::uint32_t const>(v.data(), v.size());
        }
    }
    return {};
}

std::optional<std::size_t>
XmlIndex::matching_close(std::size_t open_idx) const {
    if (open_idx >= tag_count()) return std::nullopt;
    if (!close_map.empty()) {
        auto const c = close_map[open_idx];
        return c == U32_SENTINEL ? std::nullopt : std::optional<std::size_t>{c};
    }
    if (tag_types[open_idx] == TagType::SelfClose) return open_idx;
    if (tag_types[open_idx] != TagType::Open) return std::nullopt;
    auto const d = depths[open_idx];
    auto const name = tag_name(open_idx);
    for (std::size_t i = open_idx + 1; i < tag_count(); ++i) {
        if (tag_types[i] == TagType::Close && depths[i] == d && tag_name(i) == name) {
            return i;
        }
    }
    return std::nullopt;
}

std::vector<std::size_t>
XmlIndex::children(std::size_t parent_idx) const {
    std::vector<std::size_t> out;
    if (has_indices()) {
        auto const slice = child_tag_slice(parent_idx);
        out.reserve(slice.size());
        for (auto const c : slice) out.push_back(c);
    } else {
        std::uint32_t const p = static_cast<std::uint32_t>(parent_idx);
        for (std::size_t i = 0; i < tag_count(); ++i) {
            if (parents[i] == p &&
                (tag_types[i] == TagType::Open || tag_types[i] == TagType::SelfClose)) {
                out.push_back(i);
            }
        }
    }
    return out;
}

std::size_t XmlIndex::child_count(std::size_t parent_idx) const {
    if (has_indices()) return child_tag_slice(parent_idx).size();
    return children(parent_idx).size();
}

std::optional<std::size_t>
XmlIndex::child_at(std::size_t parent_idx, std::size_t pos) const {
    if (has_indices()) {
        auto const slice = child_tag_slice(parent_idx);
        if (pos < slice.size()) return static_cast<std::size_t>(slice[pos]);
        return std::nullopt;
    }
    auto const c = children(parent_idx);
    if (pos < c.size()) return c[pos];
    return std::nullopt;
}

std::optional<std::size_t>
XmlIndex::parent(std::size_t tag_idx) const noexcept {
    if (tag_idx >= parents.size()) return std::nullopt;
    auto const p = parents[tag_idx];
    return p == U32_SENTINEL ? std::nullopt
                             : std::optional<std::size_t>{static_cast<std::size_t>(p)};
}

std::optional<std::size_t>
XmlIndex::child_position(std::size_t tag_idx) const {
    auto const p = parent(tag_idx);
    if (!p) return std::nullopt;
    if (has_indices()) {
        auto const slice = child_tag_slice(*p);
        for (std::size_t i = 0; i < slice.size(); ++i) {
            if (static_cast<std::size_t>(slice[i]) == tag_idx) return i;
        }
        return std::nullopt;
    }
    auto const c = children(*p);
    for (std::size_t i = 0; i < c.size(); ++i) {
        if (c[i] == tag_idx) return i;
    }
    return std::nullopt;
}

std::vector<std::string_view>
XmlIndex::direct_text(std::size_t tag_idx) const {
    std::vector<std::string_view> out;
    if (has_indices()) {
        auto const slice = child_text_slice(tag_idx);
        out.reserve(slice.size());
        for (auto const ti : slice) {
            out.push_back(text_by_index(static_cast<std::size_t>(ti)));
        }
    } else {
        std::uint32_t const p = static_cast<std::uint32_t>(tag_idx);
        for (auto const& r : text_ranges) {
            if (r.parent_tag == p) out.push_back(text_content(r));
        }
    }
    return out;
}

std::optional<std::string_view>
XmlIndex::direct_text_first(std::size_t tag_idx) const {
    if (has_indices()) {
        auto const slice = child_text_slice(tag_idx);
        if (!slice.empty()) {
            auto const& range = text_ranges[static_cast<std::size_t>(slice[0])];
            auto const t = text_content(range);
            if (!t.empty()) return t;
        }
        return std::nullopt;
    }
    std::uint32_t const p = static_cast<std::uint32_t>(tag_idx);
    for (auto const& r : text_ranges) {
        if (r.parent_tag == p) {
            auto const t = text_content(r);
            if (!t.empty()) return t;
        }
    }
    return std::nullopt;
}

std::optional<std::string_view>
XmlIndex::tail_text(std::size_t tag_idx) const {
    auto const p = parent(tag_idx);
    if (!p) return std::nullopt;

    auto const close_idx = matching_close(tag_idx).value_or(tag_idx);
    auto const close_end = static_cast<std::uint64_t>(tag_ends[close_idx]) + 1;

    // Linear scan — text_ranges is sorted by start offset. Use lower_bound.
    TextRange probe{close_end, 0, 0};
    auto it = std::lower_bound(
        text_ranges.begin(), text_ranges.end(), probe,
        [](TextRange const& a, TextRange const& b) { return a.start < b.start; });

    for (; it != text_ranges.end(); ++it) {
        if (it->start > close_end) break;
        if (it->start == close_end &&
            it->parent_tag == static_cast<std::uint32_t>(*p)) {
            auto const t = text_content(*it);
            if (!t.empty()) return t;
        }
    }
    return std::nullopt;
}

std::vector<std::string_view>
XmlIndex::itertext_collect(std::size_t tag_idx) const {
    auto const start_offset = tag_starts[tag_idx];
    auto const close_idx = matching_close(tag_idx).value_or(tag_idx);
    auto const end_offset = tag_ends[close_idx] + 1;

    TextRange probe{start_offset, 0, 0};
    auto first = std::lower_bound(
        text_ranges.begin(), text_ranges.end(), probe,
        [](TextRange const& a, TextRange const& b) { return a.start < b.start; });

    std::vector<std::string_view> out;
    for (; first != text_ranges.end(); ++first) {
        if (first->start >= end_offset) break;
        auto const t = text_content(*first);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

std::string XmlIndex::all_text(std::size_t tag_idx) const {
    auto const close_idx = matching_close(tag_idx).value_or(tag_idx);
    auto const tag_start = tag_starts[tag_idx];
    auto const tag_end = (close_idx == tag_idx) ? tag_ends[tag_idx]
                                                 : tag_starts[close_idx];

    std::string result;
    TextRange probe{tag_start, 0, 0};
    auto start_idx = std::lower_bound(
        text_ranges.begin(), text_ranges.end(), probe,
        [](TextRange const& a, TextRange const& b) { return a.start < b.start; });

    for (auto it = start_idx; it != text_ranges.end(); ++it) {
        if (it->start > tag_end) break;
        if (it->end <= tag_end) {
            result += text_content(*it);
        }
    }
    return result;
}

std::string_view XmlIndex::raw_xml(std::size_t tag_idx) const {
    auto const start = static_cast<std::size_t>(tag_starts[tag_idx]);
    if (tag_types[tag_idx] == TagType::SelfClose) {
        auto const end = static_cast<std::size_t>(tag_ends[tag_idx]) + 1;
        return slice_str(start, end);
    }
    if (auto const close_idx = matching_close(tag_idx)) {
        auto const end = static_cast<std::size_t>(tag_ends[*close_idx]) + 1;
        return slice_str(start, end);
    }
    auto const end = static_cast<std::size_t>(tag_ends[tag_idx]) + 1;
    return slice_str(start, end);
}

std::string_view XmlIndex::raw_tag(std::size_t tag_idx) const {
    auto const start = static_cast<std::size_t>(tag_starts[tag_idx]);
    auto const end = static_cast<std::size_t>(tag_ends[tag_idx]) + 1;
    return slice_str(start, end);
}

std::string_view XmlIndex::text_content(TextRange const& range) const {
    return slice_str(static_cast<std::size_t>(range.start),
                     static_cast<std::size_t>(range.end));
}

std::string_view XmlIndex::text_by_index(std::size_t text_idx) const {
    return text_content(text_ranges[text_idx]);
}

std::string XmlIndex::decode_entities(std::string_view s) {
    if (s.find('&') == std::string_view::npos) return std::string(s);
    std::string result;
    result.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        char const c = s[i];
        if (c != '&') {
            result.push_back(c);
            ++i;
            continue;
        }
        auto const semi = s.find(';', i + 1);
        if (semi == std::string_view::npos) {
            result.push_back('&');
            ++i;
            continue;
        }
        auto const entity = s.substr(i + 1, semi - i - 1);
        if (entity == "amp") result.push_back('&');
        else if (entity == "lt") result.push_back('<');
        else if (entity == "gt") result.push_back('>');
        else if (entity == "apos") result.push_back('\'');
        else if (entity == "quot") result.push_back('"');
        else if (!entity.empty() && entity[0] == '#') {
            auto const num = entity.substr(1);
            unsigned int code = 0;
            std::from_chars_result r{};
            if (!num.empty() && (num[0] == 'x' || num[0] == 'X')) {
                auto const hex = num.substr(1);
                r = std::from_chars(hex.data(), hex.data() + hex.size(), code, 16);
            } else {
                r = std::from_chars(num.data(), num.data() + num.size(), code, 10);
            }
            if (r.ec == std::errc{}) {
                // Encode as UTF-8.
                if (code < 0x80) {
                    result.push_back(static_cast<char>(code));
                } else if (code < 0x800) {
                    result.push_back(static_cast<char>(0xC0 | (code >> 6)));
                    result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else if (code < 0x10000) {
                    result.push_back(static_cast<char>(0xE0 | (code >> 12)));
                    result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                } else {
                    result.push_back(static_cast<char>(0xF0 | (code >> 18)));
                    result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                    result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                    result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                }
            } else {
                result += "&";
                result += entity;
                result += ";";
            }
        } else {
            result += "&";
            result += entity;
            result += ";";
        }
        i = semi + 1;
    }
    return result;
}

std::string XmlIndex::canonicalize(std::size_t tag_idx) const {
    std::string out;
    c14n_element(tag_idx, out);
    return out;
}

void XmlIndex::c14n_element(std::size_t tag_idx, std::string& out) const {
    auto const tag = tag_name(tag_idx);
    out.push_back('<');
    out += tag;

    // Sorted attributes (excluding xmlns).
    auto const names = get_all_attribute_names(tag_idx);
    std::vector<std::pair<std::string_view, std::string_view>> attrs;
    attrs.reserve(names.size());
    for (auto const& name : names) {
        if (auto const v = get_attribute(tag_idx, name)) {
            attrs.emplace_back(name, *v);
        }
    }
    std::sort(attrs.begin(), attrs.end(),
               [](auto const& a, auto const& b) { return a.first < b.first; });
    for (auto const& [n, v] : attrs) {
        out.push_back(' ');
        out += n;
        out += "=\"";
        c14n_escape_attr(v, out);
        out.push_back('"');
    }
    out.push_back('>');

    if (auto const t = direct_text_first(tag_idx)) {
        c14n_escape_text(*t, out);
    }
    auto const slice = child_tag_slice(tag_idx);
    for (auto const c : slice) {
        c14n_element(static_cast<std::size_t>(c), out);
        if (auto const tail = tail_text(static_cast<std::size_t>(c))) {
            c14n_escape_text(*tail, out);
        }
    }
    out += "</";
    out += tag;
    out.push_back('>');
}

// === Free-function CSR builders ===

#if SIMDXML_HAS_AVX2
/// Inclusive prefix sum of `counts` (n u32 values) shifted by one into
/// `offsets` (n+1 entries; offsets[0] must be 0): offsets[i+1] = offsets[i] +
/// counts[i]. AVX2 path: 8 lanes per register, two within-lane shift-adds,
/// then a per-lane bias for the carry and the low 128-bit half.
SIMDXML_TARGET_ISA("avx2") inline void
prefix_sum_u32_avx2(std::uint32_t const* counts, std::size_t n,
                    std::uint32_t* offsets) noexcept {
    if (n == 0) return;
    std::size_t i = 0;
    std::uint32_t carry = offsets[0];
    for (; i + 8 <= n; i += 8) {
        __m256i x = _mm256_loadu_si256(
            reinterpret_cast<__m256i const*>(counts + i));
        // Within each 128-bit lane: prefix over 4 u32 lanes.
        x = _mm256_add_epi32(x, _mm256_slli_si256(x, 4));
        x = _mm256_add_epi32(x, _mm256_slli_si256(x, 8));

        std::uint32_t const lo_total = static_cast<std::uint32_t>(_mm_extract_epi32(
            _mm256_castsi256_si128(x), 3));
        __m256i const bias = _mm256_set_epi32(
            static_cast<int>(carry + lo_total), static_cast<int>(carry + lo_total),
            static_cast<int>(carry + lo_total), static_cast<int>(carry + lo_total),
            static_cast<int>(carry), static_cast<int>(carry),
            static_cast<int>(carry), static_cast<int>(carry));
        x = _mm256_add_epi32(x, bias);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(offsets + i + 1), x);

        // hi_total is read AFTER the bias add, so it equals the stored
        // offsets[i + 8] — exactly the carry the next block needs. Adding
        // carry + lo_total on top would double-count them and corrupt every
        // offset after the first block (and overrun child_data).
        std::uint32_t const hi_total = static_cast<std::uint32_t>(_mm_extract_epi32(
            _mm256_extracti128_si256(x, 1), 3));
        carry = hi_total;
    }
    for (; i < n; ++i) {
        carry += counts[i];
        offsets[i + 1] = carry;
    }
}
#endif

/// Dispatched prefix sum (AVX2 when the CPU has it — the sums are exact
/// either way, so both paths agree bit-for-bit).
inline void prefix_sum_u32(std::uint32_t const* counts, std::size_t n,
                           std::uint32_t* offsets) {
#if SIMDXML_HAS_AVX2
    if (simd::has_avx2()) {
        prefix_sum_u32_avx2(counts, n, offsets);
        return;
    }
#endif
    std::uint32_t carry = offsets[0];
    for (std::size_t i = 0; i < n; ++i) {
        carry += counts[i];
        offsets[i + 1] = carry;
    }
}

std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>>
build_csr_children(std::span<TagType const> tag_types,
                   std::span<std::uint32_t const> parents,
                   std::size_t n) {
    std::vector<std::uint32_t> child_counts(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i) {
        auto const tt = tag_types[i];
        if (tt == TagType::Close || tt == TagType::CData) continue;
        auto const p = parents[i];
        if (p != U32_SENTINEL && static_cast<std::size_t>(p) < n) {
            child_counts[p] += 1;
        }
    }
    std::vector<std::uint32_t> child_offsets(n + 1, 0);
    prefix_sum_u32(child_counts.data(), n, child_offsets.data());
    std::size_t const total = child_offsets[n];
    std::vector<std::uint32_t> child_data(total, 0);

    // Fill from the end, decrementing offsets[p + 1] as a cursor: when every
    // child of p is written, child_offsets[p + 1] lands back on its initial
    // prefix-sum value, so the same vector ends as both the cursor and the
    // final row starts — no separate write_pos copy. Children within each row
    // keep ascending index order (bit-identical to the forward fill).
    for (std::size_t i = n; i-- > 0;) {
        auto const tt = tag_types[i];
        if (tt == TagType::Close || tt == TagType::CData) continue;
        auto const p = parents[i];
        if (p != U32_SENTINEL && static_cast<std::size_t>(p) < n) {
            child_data[--child_offsets[p + 1]] = static_cast<std::uint32_t>(i);
        }
    }
    return {std::move(child_offsets), std::move(child_data)};
}

std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>>
build_csr_text_children(std::span<TextRange const> text_ranges, std::size_t n) {
    std::vector<std::uint32_t> text_counts(n + 1, 0);
    for (auto const& r : text_ranges) {
        auto const p = r.parent_tag;
        if (p != U32_SENTINEL && static_cast<std::size_t>(p) < n) {
            text_counts[p] += 1;
        }
    }
    std::vector<std::uint32_t> offsets(n + 1, 0);
    prefix_sum_u32(text_counts.data(), n, offsets.data());
    std::size_t const total = offsets[n];
    std::vector<std::uint32_t> data(total, 0);

    // Same fill-from-the-end trick as build_csr_children — offsets double as
    // the cursor and recover their prefix-sum values.
    for (std::size_t ti = text_ranges.size(); ti-- > 0;) {
        auto const p = text_ranges[ti].parent_tag;
        if (p != U32_SENTINEL && static_cast<std::size_t>(p) < n) {
            data[--offsets[p + 1]] = static_cast<std::uint32_t>(ti);
        }
    }
    return {std::move(offsets), std::move(data)};
}

std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>>
build_close_map_and_post_order(std::span<TagType const> tag_types, std::size_t n) {
    std::vector<std::uint32_t> close_map(n, U32_SENTINEL);
    std::vector<std::uint32_t> post_order(n, 0);
    std::vector<std::size_t> stack;
    stack.reserve(64);
    std::uint32_t post_counter = 0;

    for (std::size_t i = 0; i < n; ++i) {
        switch (tag_types[i]) {
            case TagType::Open:
                stack.push_back(i);
                break;
            case TagType::Close: {
                if (!stack.empty()) {
                    auto const open_idx = stack.back();
                    stack.pop_back();
                    close_map[open_idx] = static_cast<std::uint32_t>(i);
                    post_order[open_idx] = post_counter;
                }
                post_order[i] = post_counter;
                ++post_counter;
                break;
            }
            case TagType::SelfClose:
                close_map[i] = static_cast<std::uint32_t>(i);
                post_order[i] = post_counter;
                ++post_counter;
                break;
            case TagType::Comment:
            case TagType::PI:
            case TagType::CData:
                post_order[i] = post_counter;
                ++post_counter;
                break;
        }
    }
    return {std::move(close_map), std::move(post_order)};
}

}  // namespace rai::xml