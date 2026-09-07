// Implementation of the XPath 1.0 core function library.
//
// Ported from Rust `xpath/eval.rs` lines 1039-1421. Each function arm mirrors
// the Rust implementation; numeric coercion goes through `XpathValue::to_number`
// and string coercion through `XpathValue::to_string` (defined in `internal.cpp`).
// The `not()` and `boolean()` functions treat LocationPath arguments as
// node-set existence checks (XPath §4.3) rather than string truthiness.
#include "function_call.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace simdxml {

namespace {

[[nodiscard]] std::string
to_ascii_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

// Unused helper kept for reference (ported from Rust xpath/eval.rs).
/* unused — silenced for -Wunused-function */
#if 0
[[nodiscard]] std::vector<char>
to_char_vec(std::string_view s) {
    return std::vector<char>(s.begin(), s.end());
}
#endif

}  // namespace

std::optional<std::string>
resolve_namespace_uri(XmlIndex const& index, std::size_t start_idx,
                      std::optional<std::string_view> prefix) {
    std::string attr_name;
    if (prefix) {
        attr_name = "xmlns:" + std::string(*prefix);
    } else {
        attr_name = "xmlns";
    }
    std::size_t idx = start_idx;
    for (;;) {
        if (auto uri = index.get_attribute(idx, attr_name)) {
            return std::string(*uri);
        }
        std::uint32_t const parent = index.parents[idx];
        if (parent == UINT32_MAX) return std::nullopt;
        idx = parent;
    }
}

Result<std::vector<XPathNode>>
eval_id_function(XmlIndex const& index,
                 std::vector<std::unique_ptr<XPathExpr>> const& args) {
    if (args.size() != 1) {
        return std::unexpected(SimdXmlError::xpath_eval_error(
            "id() requires exactly 1 argument"));
    }
    if (!args[0] || args[0]->kind != XPathExpr::Kind::StringLiteral) {
        return std::vector<XPathNode>{};
    }
    std::string const id_value = args[0]->string_literal;

    for (std::size_t i = 0; i < index.tag_count(); ++i) {
        if (index.tag_types[i] == TagType::Open ||
            index.tag_types[i] == TagType::SelfClose) {
            if (auto val = index.get_attribute(i, "id")) {
                if (*val == id_value) {
                    return std::vector<XPathNode>{XPathNode::element(i)};
                }
            }
        }
    }
    return std::vector<XPathNode>{};
}

Result<XpathValue>
eval_function(XmlIndex const& index, XPathNode const& node,
              std::string_view name,
              std::vector<std::unique_ptr<XPathExpr>> const& args,
              std::size_t position, std::size_t size) {
    auto arg_at = [&](std::size_t i) -> XPathExpr const* {
        return i < args.size() ? args[i].get() : nullptr;
    };

    if (name == "position") {
        return XpathValue::number(static_cast<double>(position));
    }
    if (name == "last") {
        return XpathValue::number(static_cast<double>(size));
    }
    if (name == "count") {
        if (auto arg = arg_at(0)) {
            auto nodes = evaluate_in_context(index, node, *arg);
            if (!nodes) return std::unexpected(nodes.error());
            return XpathValue::number(static_cast<double>(nodes->size()));
        }
        return XpathValue::number(0.0);
    }
    if (name == "contains") {
        if (args.size() >= 2 && args[0] && args[1]) {
            auto lhs = eval_predicate_value(index, node, *args[0], position, size);
            if (!lhs) return std::unexpected(lhs.error());
            auto rhs = eval_predicate_value(index, node, *args[1], position, size);
            if (!rhs) return std::unexpected(rhs.error());
            std::string const haystack = lhs->to_string();
            std::string const needle = rhs->to_string();
            return XpathValue::boolean(haystack.find(needle) != std::string::npos);
        }
        return XpathValue::boolean(false);
    }
    if (name == "starts-with") {
        if (args.size() >= 2 && args[0] && args[1]) {
            auto lhs = eval_predicate_value(index, node, *args[0], position, size);
            if (!lhs) return std::unexpected(lhs.error());
            auto rhs = eval_predicate_value(index, node, *args[1], position, size);
            if (!rhs) return std::unexpected(rhs.error());
            std::string const haystack = lhs->to_string();
            std::string const prefix = rhs->to_string();
            return XpathValue::boolean(haystack.starts_with(prefix));
        }
        return XpathValue::boolean(false);
    }
    if (name == "string-length") {
        std::string s;
        if (auto arg = arg_at(0)) {
            auto v = eval_predicate_value(index, node, *arg, position, size);
            if (!v) return std::unexpected(v.error());
            s = v->to_string();
        } else {
            s = node_string_value(index, node);
        }
        // XPath string-length counts Unicode characters, not bytes.
        std::size_t count = 0;
        for (auto it = s.begin(); it != s.end(); ) {
            unsigned char c = static_cast<unsigned char>(*it);
            std::size_t sz = 1;
            if ((c & 0x80) == 0) sz = 1;
            else if ((c & 0xE0) == 0xC0) sz = 2;
            else if ((c & 0xF0) == 0xE0) sz = 3;
            else if ((c & 0xF8) == 0xF0) sz = 4;
            if (static_cast<std::size_t>(std::distance(it, s.end())) < sz) break;
            std::advance(it, sz);
            ++count;
        }
        return XpathValue::number(static_cast<double>(count));
    }
    if (name == "normalize-space") {
        std::string s;
        if (auto arg = arg_at(0)) {
            auto v = eval_predicate_value(index, node, *arg, position, size);
            if (!v) return std::unexpected(v.error());
            s = v->to_string();
        } else {
            s = node_string_value(index, node);
        }
        std::string out;
        bool in_ws = false;
        for (char c : s) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!out.empty()) in_ws = true;
            } else {
                if (in_ws) { out.push_back(' '); in_ws = false; }
                out.push_back(c);
            }
        }
        return XpathValue::string(std::move(out));
    }
    if (name == "not") {
        if (auto arg = arg_at(0)) {
            // LocationPath args → node-set existence (XPath §4.3).
            if (arg->kind == XPathExpr::Kind::LocationPath) {
                auto nodes = evaluate_in_context(index, node, *arg);
                if (!nodes) return std::unexpected(nodes.error());
                return XpathValue::boolean(nodes->empty());
            }
            auto v = eval_predicate_value(index, node, *arg, position, size);
            if (!v) return std::unexpected(v.error());
            return XpathValue::boolean(!v->is_truthy());
        }
        return XpathValue::boolean(true);
    }
    if (name == "true") return XpathValue::boolean(true);
    if (name == "false") return XpathValue::boolean(false);
    if (name == "name") {
        std::optional<XPathNode> target;
        if (auto arg = arg_at(0)) {
            auto nodes = evaluate_in_context(index, node, *arg);
            if (!nodes) return std::unexpected(nodes.error());
            sort_doc_order(index, *nodes);
            if (!nodes->empty()) target = nodes->front();
        } else {
            target = node;
        }
        if (target) {
            if (target->is_element()) {
                std::size_t const idx = target->index();
                if (idx != DOC_ROOT && idx < index.tag_count()) {
                    return XpathValue::string(std::string(index.tag_name(idx)));
                }
            }
            if (target->is_attribute()) {
                std::size_t const tag_idx = target->index();
                std::uint64_t const hash = target->hash();
                for (auto n : index.get_all_attribute_names(tag_idx)) {
                    if (attr_name_hash(n) == hash) {
                        return XpathValue::string(std::string(n));
                    }
                }
                return XpathValue::string(std::string{});
            }
            if (target->is_element()) {
                std::size_t const idx = target->index();
                if (idx < index.tag_count() && index.tag_types[idx] == TagType::PI) {
                    return XpathValue::string(std::string(index.tag_name(idx)));
                }
            }
        }
        return XpathValue::string(std::string{});
    }
    if (name == "local-name") {
        std::optional<XPathNode> target;
        if (auto arg = arg_at(0)) {
            auto nodes = evaluate_in_context(index, node, *arg);
            if (!nodes) return std::unexpected(nodes.error());
            sort_doc_order(index, *nodes);
            if (!nodes->empty()) target = nodes->front();
        } else {
            target = node;
        }
        if (target) {
            if (target->is_element()) {
                std::size_t const idx = target->index();
                if (idx != DOC_ROOT && idx < index.tag_count()) {
                    std::string_view const full = index.tag_name(idx);
                    if (auto colon = full.find(':'); colon != std::string_view::npos) {
                        return XpathValue::string(std::string(full.substr(colon + 1)));
                    }
                    return XpathValue::string(std::string(full));
                }
            }
            if (target->is_attribute()) {
                std::size_t const tag_idx = target->index();
                std::uint64_t const hash = target->hash();
                for (auto n : index.get_all_attribute_names(tag_idx)) {
                    if (attr_name_hash(n) == hash) {
                        std::string_view sv{n};
                        if (auto colon = sv.find(':'); colon != std::string_view::npos) {
                            return XpathValue::string(std::string(sv.substr(colon + 1)));
                        }
                        return XpathValue::string(std::string(sv));
                    }
                }
                return XpathValue::string(std::string{});
            }
            if (target->is_element()) {
                std::size_t const idx = target->index();
                if (idx < index.tag_count() && index.tag_types[idx] == TagType::PI) {
                    return XpathValue::string(std::string(index.tag_name(idx)));
                }
            }
        }
        return XpathValue::string(std::string{});
    }
    if (name == "string") {
        if (auto arg = arg_at(0)) {
            auto v = eval_predicate_value(index, node, *arg, position, size);
            if (!v) return std::unexpected(v.error());
            return XpathValue::string(v->to_string());
        }
        return XpathValue::string(node_string_value(index, node));
    }
    if (name == "concat") {
        std::string result;
        for (auto const& arg : args) {
            if (!arg) continue;
            auto v = eval_predicate_value(index, node, *arg, position, size);
            if (!v) return std::unexpected(v.error());
            result += v->to_string();
        }
        return XpathValue::string(std::move(result));
    }
    if (name == "substring") {
        if (args.size() >= 2 && args[0] && args[1]) {
            auto sv = eval_predicate_value(index, node, *args[0], position, size);
            if (!sv) return std::unexpected(sv.error());
            auto pv = eval_predicate_value(index, node, *args[1], position, size);
            if (!pv) return std::unexpected(pv.error());
            std::string const s = sv->to_string();
            double const start_raw = pv->to_number();

            if (std::isnan(start_raw)) return XpathValue::string(std::string{});

            // Build UTF-8 char-indexed view.
            std::vector<std::pair<std::size_t, std::size_t>> spans;  // (byte_off, byte_len)
            std::size_t pos = 0;
            while (pos < s.size()) {
                unsigned char c = static_cast<unsigned char>(s[pos]);
                std::size_t sz = 1;
                if ((c & 0x80) == 0) sz = 1;
                else if ((c & 0xE0) == 0xC0) sz = 2;
                else if ((c & 0xF0) == 0xE0) sz = 3;
                else if ((c & 0xF8) == 0xF0) sz = 4;
                if (pos + sz > s.size()) break;
                spans.emplace_back(pos, sz);
                pos += sz;
            }

            if (args.size() >= 3 && args[2]) {
                auto lv = eval_predicate_value(index, node, *args[2], position, size);
                if (!lv) return std::unexpected(lv.error());
                double const len_raw = lv->to_number();
                if (std::isnan(len_raw) || len_raw == -std::numeric_limits<double>::infinity()) {
                    return XpathValue::string(std::string{});
                }
                double const p = std::round(start_raw);
                double const n = std::round(len_raw);
                double const end = p + n;
                if (end == -std::numeric_limits<double>::infinity() ||
                    p == std::numeric_limits<double>::infinity()) {
                    return XpathValue::string(std::string{});
                }
                std::int64_t si = static_cast<std::int64_t>(std::max(p, 1.0)) - 1;
                if (si < 0) si = 0;
                std::size_t start_idx = static_cast<std::size_t>(si);
                std::size_t end_idx;
                if (std::isinf(end)) {
                    end_idx = spans.size();
                } else {
                    std::int64_t ei = static_cast<std::int64_t>(end) - 1;
                    if (ei < 0) ei = 0;
                    end_idx = std::min(static_cast<std::size_t>(ei), spans.size());
                }
                if (start_idx >= end_idx || start_idx >= spans.size()) {
                    return XpathValue::string(std::string{});
                }
                std::size_t byte_off = spans[start_idx].first;
                std::size_t byte_end = (end_idx < spans.size())
                    ? spans[end_idx].first
                    : s.size();
                return XpathValue::string(s.substr(byte_off, byte_end - byte_off));
            } else {
                std::int64_t si = static_cast<std::int64_t>(
                    std::round(std::max(start_raw, 1.0))) - 1;
                if (si < 0) si = 0;
                std::size_t start_idx = static_cast<std::size_t>(si);
                if (start_idx >= spans.size()) return XpathValue::string(std::string{});
                std::size_t byte_off = spans[start_idx].first;
                return XpathValue::string(s.substr(byte_off));
            }
        }
        return XpathValue::string(std::string{});
    }
    if (name == "floor") {
        double n = 0.0;
        if (auto arg = arg_at(0)) {
            auto v = eval_predicate_value(index, node, *arg, position, size);
            if (!v) return std::unexpected(v.error());
            n = v->to_number();
        }
        return XpathValue::number(std::floor(n));
    }
    if (name == "ceiling") {
        double n = 0.0;
        if (auto arg = arg_at(0)) {
            auto v = eval_predicate_value(index, node, *arg, position, size);
            if (!v) return std::unexpected(v.error());
            n = v->to_number();
        }
        return XpathValue::number(std::ceil(n));
    }
    if (name == "round") {
        double n = 0.0;
        if (auto arg = arg_at(0)) {
            auto v = eval_predicate_value(index, node, *arg, position, size);
            if (!v) return std::unexpected(v.error());
            n = v->to_number();
        }
        // XPath round: round half to positive infinity.
        if (std::isnan(n)) return XpathValue::number(n);
        if (n == -0.5) return XpathValue::number(0.0);
        return XpathValue::number(std::round(n));
    }
    if (name == "number") {
        XpathValue v;
        if (auto arg = arg_at(0)) {
            auto r = eval_predicate_value(index, node, *arg, position, size);
            if (!r) return std::unexpected(r.error());
            v = *r;
        } else {
            v = XpathValue::string(node_string_value(index, node));
        }
        return XpathValue::number(v.to_number());
    }
    if (name == "sum") {
        if (auto arg = arg_at(0)) {
            auto nodes = evaluate_in_context(index, node, *arg);
            if (!nodes) return std::unexpected(nodes.error());
            double total = 0.0;
            for (auto const& n : *nodes) {
                std::string const sv = node_string_value(index, n);
                try {
                    std::size_t consumed = 0;
                    total += std::stod(sv, &consumed);
                } catch (...) {
                    // Non-numeric → contributes 0 (per XPath §4.4 NaN coercion).
                }
            }
            return XpathValue::number(total);
        }
        return XpathValue::number(0.0);
    }
    if (name == "translate") {
        if (args.size() >= 3 && args[0] && args[1] && args[2]) {
            auto sv = eval_predicate_value(index, node, *args[0], position, size);
            if (!sv) return std::unexpected(sv.error());
            auto fv = eval_predicate_value(index, node, *args[1], position, size);
            if (!fv) return std::unexpected(fv.error());
            auto tv = eval_predicate_value(index, node, *args[2], position, size);
            if (!tv) return std::unexpected(tv.error());
            std::string const s = sv->to_string();
            std::string const from = fv->to_string();
            std::string const to = tv->to_string();
            // Per-character translation using UTF-8 spans (chars may be multibyte).
            std::vector<std::pair<std::size_t, std::size_t>> s_spans, from_spans, to_spans;
            auto build_spans = [](std::string const& str) {
                std::vector<std::pair<std::size_t, std::size_t>> out;
                std::size_t pos = 0;
                while (pos < str.size()) {
                    unsigned char c = static_cast<unsigned char>(str[pos]);
                    std::size_t sz = 1;
                    if ((c & 0x80) == 0) sz = 1;
                    else if ((c & 0xE0) == 0xC0) sz = 2;
                    else if ((c & 0xF0) == 0xE0) sz = 3;
                    else if ((c & 0xF8) == 0xF0) sz = 4;
                    if (pos + sz > str.size()) break;
                    out.emplace_back(pos, sz);
                    pos += sz;
                }
                return out;
            };
            s_spans = build_spans(s);
            from_spans = build_spans(from);
            to_spans = build_spans(to);
            std::string out;
            for (auto const& [off, len] : s_spans) {
                std::string_view const ch{s.data() + off, len};
                bool found = false;
                for (std::size_t i = 0; i < from_spans.size(); ++i) {
                    std::string_view fc{from.data() + from_spans[i].first,
                                        from_spans[i].second};
                    if (ch == fc) {
                        if (i < to_spans.size()) {
                            out.append(to.data() + to_spans[i].first,
                                       to_spans[i].second);
                        }
                        // else: drop the character.
                        found = true;
                        break;
                    }
                }
                if (!found) out.append(ch);
            }
            return XpathValue::string(std::move(out));
        }
        return XpathValue::string(std::string{});
    }
    if (name == "substring-before") {
        if (args.size() >= 2 && args[0] && args[1]) {
            auto sv = eval_predicate_value(index, node, *args[0], position, size);
            if (!sv) return std::unexpected(sv.error());
            auto nv = eval_predicate_value(index, node, *args[1], position, size);
            if (!nv) return std::unexpected(nv.error());
            std::string const s = sv->to_string();
            std::string const needle = nv->to_string();
            if (auto pos = s.find(needle); pos != std::string::npos) {
                return XpathValue::string(s.substr(0, pos));
            }
        }
        return XpathValue::string(std::string{});
    }
    if (name == "substring-after") {
        if (args.size() >= 2 && args[0] && args[1]) {
            auto sv = eval_predicate_value(index, node, *args[0], position, size);
            if (!sv) return std::unexpected(sv.error());
            auto nv = eval_predicate_value(index, node, *args[1], position, size);
            if (!nv) return std::unexpected(nv.error());
            std::string const s = sv->to_string();
            std::string const needle = nv->to_string();
            if (auto pos = s.find(needle); pos != std::string::npos) {
                return XpathValue::string(s.substr(pos + needle.size()));
            }
        }
        return XpathValue::string(std::string{});
    }
    if (name == "boolean") {
        if (auto arg = arg_at(0)) {
            if (arg->kind == XPathExpr::Kind::LocationPath) {
                auto nodes = evaluate_in_context(index, node, *arg);
                if (!nodes) return std::unexpected(nodes.error());
                return XpathValue::boolean(!nodes->empty());
            }
            auto v = eval_predicate_value(index, node, *arg, position, size);
            if (!v) return std::unexpected(v.error());
            return XpathValue::boolean(v->is_truthy());
        }
        return XpathValue::boolean(false);
    }
    if (name == "lang") {
        if (auto arg = arg_at(0)) {
            auto tv = eval_predicate_value(index, node, *arg, position, size);
            if (!tv) return std::unexpected(tv.error());
            std::string const target = tv->to_string();
            if (target.empty()) return XpathValue::boolean(false);
            std::string const target_lower = to_ascii_lower(target);
            // Walk node and ancestors looking for xml:lang.
            std::optional<std::size_t> current;
            if (node.is_element() && node.index() != DOC_ROOT) {
                current = node.index();
            }
            while (current) {
                std::size_t const idx = *current;
                if (auto lang_val = index.get_attribute(idx, "xml:lang")) {
                    std::string const lang_lower = to_ascii_lower(*lang_val);
                    bool const matches = lang_lower == target_lower ||
                        (lang_lower.starts_with(target_lower) &&
                         lang_lower.size() > target_lower.size() &&
                         lang_lower[target_lower.size()] == '-');
                    return XpathValue::boolean(matches);
                }
                std::uint32_t const parent = index.parents[idx];
                current = (parent != UINT32_MAX)
                    ? std::optional<std::size_t>{parent} : std::nullopt;
            }
            return XpathValue::boolean(false);
        }
        return XpathValue::boolean(false);
    }
    if (name == "namespace-uri") {
        std::optional<XPathNode> target;
        if (auto arg = arg_at(0)) {
            auto nodes = evaluate_in_context(index, node, *arg);
            if (!nodes) return std::unexpected(nodes.error());
            sort_doc_order(index, *nodes);
            if (!nodes->empty()) target = nodes->front();
        } else {
            target = node;
        }
        if (target) {
            if (target->is_element()) {
                std::size_t const idx = target->index();
                if (idx != DOC_ROOT && idx < index.tag_count()) {
                    std::string_view const full = index.tag_name(idx);
                    std::optional<std::string_view> prefix;
                    if (auto colon = full.find(':'); colon != std::string_view::npos) {
                        prefix = full.substr(0, colon);
                    }
                    auto uri = resolve_namespace_uri(index, idx, prefix);
                    return XpathValue::string(uri ? *uri : std::string{});
                }
            }
            if (target->is_attribute()) {
                std::size_t const tag_idx = target->index();
                std::uint64_t const hash = target->hash();
                for (auto n : index.get_all_attribute_names(tag_idx)) {
                    if (attr_name_hash(n) == hash) {
                        std::string_view sv{n};
                        if (auto colon = sv.find(':'); colon != std::string_view::npos) {
                            auto uri = resolve_namespace_uri(index, tag_idx,
                                                              sv.substr(0, colon));
                            return XpathValue::string(uri ? *uri : std::string{});
                        }
                        return XpathValue::string(std::string{});
                    }
                }
            }
        }
        return XpathValue::string(std::string{});
    }
    // Unknown function — return empty string (matches Rust behavior).
    return XpathValue::string(std::string{});
}

}  // namespace simdxml