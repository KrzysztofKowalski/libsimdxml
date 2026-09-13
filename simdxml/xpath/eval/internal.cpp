// Implementation of shared internal helpers for the XPath evaluator.
//
// Ported from Rust `xpath/eval.rs`:
//   - `attr_name_hash` (lines 98-105)
//   - `xpath_format_number` (lines 24-78)
//   - `XpathValue` methods (lines 877-909)
//   - `node_string_value` (lines 1456-1518)
//   - `dedup_nodes`, `node_doc_pos`, `sort_doc_order` (lines 709-734)
//
// `xpath_format_number` mirrors libxml2's `xmlXPathFormatNumber` (the %0.15g
// equivalent): 15 significant digits, fixed notation for exponents in [-4, 15),
// scientific notation otherwise, with trailing zeros stripped and a leading
// '+' inserted in positive exponents.
#include "internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace rai::xml {

std::uint64_t
attr_name_hash(std::string_view name) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a offset basis
    for (unsigned char b : name) {
        h ^= static_cast<std::uint64_t>(b);
        h *= 0x100000001b3ULL;
    }
    return h;
}

bool XpathValue::is_truthy() const noexcept {
    switch (kind_) {
        case Kind::Boolean: return bool_;
        case Kind::String:  return !str_.empty();
        case Kind::Number:
            return num_ != 0.0 && !std::isnan(num_);
    }
    return false;
}

std::string XpathValue::to_string() const {
    switch (kind_) {
        case Kind::String:  return str_;
        case Kind::Number:   return xpath_format_number(num_);
        case Kind::Boolean:  return bool_ ? "true" : "false";
    }
    return {};
}

double XpathValue::to_number() const noexcept {
    switch (kind_) {
        case Kind::Number:  return num_;
        case Kind::Boolean: return bool_ ? 1.0 : 0.0;
        case Kind::String: {
            // Trim leading/trailing whitespace then parse.
            std::size_t b = 0, e = str_.size();
            while (b < e && std::isspace(static_cast<unsigned char>(str_[b]))) ++b;
            while (e > b && std::isspace(static_cast<unsigned char>(str_[e - 1]))) --e;
            if (b >= e) return std::numeric_limits<double>::quiet_NaN();
            std::string trimmed(str_.data() + b, e - b);
            try {
                std::size_t consumed = 0;
                double const v = std::stod(trimmed, &consumed);
                // XPath: trailing garbage → NaN.
                if (consumed != trimmed.size()) {
                    return std::numeric_limits<double>::quiet_NaN();
                }
                return v;
            } catch (...) {
                return std::numeric_limits<double>::quiet_NaN();
            }
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

std::string
xpath_format_number(double n) {
    if (std::isnan(n)) return "NaN";
    if (n == std::numeric_limits<double>::infinity()) return "Infinity";
    if (n == -std::numeric_limits<double>::infinity()) return "-Infinity";

    // Integer that fits exactly → format without decimal.
    if (n == std::trunc(n) && std::fabs(n) < 1e15) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%lld",
                      static_cast<long long>(n));
        return std::string(buf);
    }

    double const abs_n = std::fabs(n);
    int exp = 0;
    if (abs_n > 0.0) {
        exp = static_cast<int>(std::floor(std::log10(abs_n)));
    }

    if (exp >= -4 && exp < 15) {
        // Fixed notation: 15 sig digits, strip trailing zeros.
        int const decimal_digits = std::max(0, 14 - exp);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%.*f", decimal_digits, n);
        std::string s(buf);
        if (auto dot = s.find('.'); dot != std::string::npos) {
            std::size_t last = s.size();
            while (last > dot + 1 && s[last - 1] == '0') --last;
            if (last > dot + 1 && s[last - 1] == '.') --last;
            s.resize(last);
        }
        return s;
    }

    // Scientific notation: mantissa with 14 fractional digits, e+NN.
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.14e", n);
    std::string s(buf);
    if (auto e_pos = s.find('e'); e_pos != std::string::npos) {
        // Insert '+' for positive exponent if missing.
        if (e_pos + 1 < s.size() && s[e_pos + 1] != '-' && s[e_pos + 1] != '+') {
            s.insert(e_pos + 1, 1, '+');
            e_pos = s.find('e');
        }
        // Strip trailing zeros in mantissa.
        auto mant_end = e_pos;
        auto dot = s.find('.');
        if (dot != std::string::npos && dot < e_pos) {
            std::size_t last = mant_end;
            while (last > dot + 1 && s[last - 1] == '0') --last;
            if (last > dot + 1 && s[last - 1] == '.') --last;
            std::string mantissa = s.substr(0, last);
            std::string exp_str = s.substr(e_pos);
            s = mantissa + exp_str;
        }
        // Remove leading zeros from exponent (e.g. e+019 → e+19).
        if (auto e2 = s.find('e'); e2 != std::string::npos) {
            std::size_t sign_start = e2 + 1;
            std::string prefix = s.substr(0, sign_start);
            std::string exp_part = s.substr(sign_start);
            std::string sign;
            std::string digits;
            if (!exp_part.empty() && (exp_part[0] == '-' || exp_part[0] == '+')) {
                sign = exp_part.substr(0, 1);
                digits = exp_part.substr(1);
            } else {
                digits = exp_part;
            }
            std::size_t k = 0;
            while (k < digits.size() && digits[k] == '0') ++k;
            if (k == digits.size()) --k;  // keep at least one digit
            digits = digits.substr(k);
            s = prefix + sign + digits;
        }
    }
    return s;
}

std::string
node_string_value(XmlIndex const& index, XPathNode const& node) {
    if (node.is_element()) {
        std::size_t const idx = node.index();
        if (idx == DOC_ROOT) {
            // Document root: concatenation of all text in the document.
            std::string result;
            for (auto const& range : index.text_ranges) {
                result += XmlIndex::decode_entities(index.text_content(range));
            }
            return result;
        }
        if (idx < index.tag_count()) {
            TagType const tt = index.tag_types[idx];
            if (tt == TagType::PI) {
                std::string_view const sv = index.raw_tag(idx);
                std::string_view inner = sv;
                if (inner.starts_with("<?")) inner.remove_prefix(2);
                if (inner.ends_with("?>")) inner.remove_suffix(2);
                else if (inner.ends_with("?")) inner.remove_suffix(1);
                else if (inner.ends_with(">")) inner.remove_suffix(1);
                // Skip target name (first word).
                if (auto sp = std::find_if(inner.begin(), inner.end(),
                        [](char c){ return std::isspace(static_cast<unsigned char>(c)); });
                    sp != inner.end()) {
                    std::string_view rest{sp, inner.end()};
                    std::string trimmed;
                    bool in_ws = false;
                    for (char c : rest) {
                        if (std::isspace(static_cast<unsigned char>(c))) {
                            if (!trimmed.empty()) in_ws = true;
                        } else {
                            if (in_ws) { trimmed.push_back(' '); in_ws = false; }
                            trimmed.push_back(c);
                        }
                    }
                    return trimmed;
                }
                return std::string{};
            }
            if (tt == TagType::Comment) {
                std::string_view const sv = index.raw_tag(idx);
                std::string_view inner = sv;
                if (inner.starts_with("<!--")) inner.remove_prefix(4);
                if (inner.ends_with("-->")) inner.remove_suffix(3);
                else if (inner.ends_with("--")) inner.remove_suffix(2);
                else if (inner.ends_with(">")) inner.remove_suffix(1);
                return std::string(inner);
            }
            return XmlIndex::decode_entities(index.all_text(idx));
        }
        return {};
    }
    if (node.is_text()) {
        std::size_t const idx = node.index();
        if (idx >= index.text_ranges.size()) return {};
        return XmlIndex::decode_entities(index.text_content(index.text_ranges[idx]));
    }
    if (node.is_attribute()) {
        std::size_t const tag_idx = node.index();
        std::uint64_t const hash = node.hash();
        for (auto name : index.get_all_attribute_names(tag_idx)) {
            if (attr_name_hash(name) == hash) {
                if (auto val = index.get_attribute(tag_idx, name)) {
                    return std::string(*val);
                }
            }
        }
        return {};
    }
    return {};
}

void dedup_nodes(std::vector<XPathNode>& nodes) {
    // XPath node-sets must be unique. Keep first occurrence in vector order.
    // Mirror Rust `dedup_nodes` which uses a HashSet + retain.
    struct Key {
        int kind;
        std::size_t idx;
        std::uint64_t aux;
        bool operator==(Key const&) const noexcept = default;
    };
    struct Hash {
        std::size_t operator()(Key const& k) const noexcept {
            std::size_t h = std::hash<int>{}(k.kind);
            h ^= std::hash<std::size_t>{}(k.idx) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<std::uint64_t>{}(k.aux) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_set<Key, Hash> seen;
    seen.reserve(nodes.size());
    std::size_t write = 0;
    for (std::size_t read = 0; read < nodes.size(); ++read) {
        XPathNode const& n = nodes[read];
        Key k{static_cast<int>(n.kind()), n.index(),
              (n.is_attribute() || n.is_namespace()) ? n.hash() : 0u};
        if (seen.insert(k).second) {
            if (write != read) nodes[write] = nodes[read];
            ++write;
        }
    }
    nodes.resize(write);
}

std::uint64_t
node_doc_pos(XmlIndex const& index, XPathNode const& node) noexcept {
    if (node.is_element()) {
        std::size_t const idx = node.index();
        if (idx < index.tag_count()) return index.tag_starts[idx];
    }
    if (node.is_text()) {
        std::size_t const idx = node.index();
        if (idx < index.text_ranges.size()) return index.text_ranges[idx].start;
    }
    if (node.is_attribute() || node.is_namespace()) {
        std::size_t const idx = node.index();
        if (idx < index.tag_count()) return index.tag_starts[idx];
    }
    return std::numeric_limits<std::uint64_t>::max();
}

void sort_doc_order(XmlIndex const& index, std::vector<XPathNode>& nodes) {
    std::stable_sort(nodes.begin(), nodes.end(),
        [&](XPathNode const& a, XPathNode const& b) {
            return node_doc_pos(index, a) < node_doc_pos(index, b);
        });
}

}  // namespace rai::xml