// Implementation of XPath axis evaluation.
//
// Ported from Rust `xpath/eval.rs` lines 1631-2164. Each axis is implemented
// as array operations over `XmlIndex` (CSR slices, parents, depths, pre/post
// ordering, posting lists). The C++ port mirrors the Rust logic closely:
//
//   - child: CSR `child_tag_slice` / `child_text_slice`, merged in doc order
//   - descendant: range scan open..close, plus text ranges whose parent is in
//     that range
//   - parent/ancestor: `parents` chain with `u32::MAX` sentinel
//   - following-sibling / preceding-sibling: scan parent's CSR children on
//     either side of the context node
//   - following / preceding: full-doc scan with ancestor exclusion via
//     `is_ancestor` (pre/post order)
//   - attribute / namespace: iterate attribute names / namespace decls
//
// `eval_step` applies predicates per-context-node so `position()` is correct
// for `//p[1]` patterns. Multi-context results are deduped + sorted.
#include "axis_step.hpp"
#include "filter.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rai::xml {

namespace {

/// Children of a node — used by `eval_axis` for `Axis::Child`.
[[nodiscard]] std::vector<XPathNode>
eval_child_axis(XmlIndex const& index, XPathNode const& node) {
    if (!node.is_element()) return {};
    std::size_t const parent_idx = node.index();

    if (parent_idx == DOC_ROOT) {
        // Document root's children are depth-0 elements/comments/PIs.
        std::vector<std::pair<std::uint64_t, XPathNode>> pairs;
        for (std::size_t i = 0; i < index.tag_count(); ++i) {
            if (index.depths[i] == 0 && is_node_tag(index.tag_types[i])) {
                pairs.emplace_back(index.tag_starts[i],
                                   XPathNode::element(i));
            }
        }
        std::sort(pairs.begin(), pairs.end(),
                  [](auto const& a, auto const& b) { return a.first < b.first; });
        std::vector<XPathNode> out;
        out.reserve(pairs.size());
        for (auto& p : pairs) out.push_back(p.second);
        return out;
    }

    if (!index.has_indices()) {
        // Linear scan fallback for small documents.
        std::vector<std::pair<std::uint64_t, XPathNode>> pairs;
        for (std::size_t i = 0; i < index.tag_count(); ++i) {
            if (index.parents[i] == static_cast<std::uint32_t>(parent_idx) &&
                is_node_tag(index.tag_types[i])) {
                pairs.emplace_back(index.tag_starts[i],
                                   XPathNode::element(i));
            }
        }
        for (std::size_t i = 0; i < index.text_ranges.size(); ++i) {
            if (index.text_ranges[i].parent_tag == parent_idx) {
                pairs.emplace_back(index.text_ranges[i].start,
                                   XPathNode::text(i));
            }
        }
        std::sort(pairs.begin(), pairs.end(),
                  [](auto const& a, auto const& b) { return a.first < b.first; });
        std::vector<XPathNode> out;
        out.reserve(pairs.size());
        for (auto& p : pairs) out.push_back(p.second);
        return out;
    }

    auto tags = index.child_tag_slice(parent_idx);
    auto texts = index.child_text_slice(parent_idx);

    if (texts.empty()) {
        std::vector<XPathNode> out;
        out.reserve(tags.size());
        for (auto t : tags) out.push_back(XPathNode::element(t));
        return out;
    }

    // Merge tag children and text children in document order.
    std::vector<XPathNode> out;
    out.reserve(tags.size() + texts.size());
    std::size_t ti = 0, xi = 0;
    while (ti < tags.size() && xi < texts.size()) {
        std::uint64_t const tag_pos = index.tag_starts[tags[ti]];
        std::uint64_t const txt_pos = index.text_ranges[texts[xi]].start;
        if (tag_pos < txt_pos) {
            out.push_back(XPathNode::element(tags[ti]));
            ++ti;
        } else {
            out.push_back(XPathNode::text(texts[xi]));
            ++xi;
        }
    }
    while (ti < tags.size()) {
        out.push_back(XPathNode::element(tags[ti]));
        ++ti;
    }
    while (xi < texts.size()) {
        out.push_back(XPathNode::text(texts[xi]));
        ++xi;
    }
    return out;
}

[[nodiscard]] std::vector<XPathNode>
eval_descendant_axis(XmlIndex const& index, XPathNode const& node,
                     bool include_self) {
    if (!node.is_element()) {
        return include_self ? std::vector<XPathNode>{node} : std::vector<XPathNode>{};
    }
    std::size_t const start_idx = node.index();

    if (start_idx == DOC_ROOT) {
        // Descendants of document root = all node types, in document order.
        std::vector<std::pair<std::uint64_t, XPathNode>> items;
        if (include_self) items.emplace_back(0, XPathNode::element(DOC_ROOT));
        for (std::size_t i = 0; i < index.tag_count(); ++i) {
            if (is_node_tag(index.tag_types[i])) {
                items.emplace_back(index.tag_starts[i], XPathNode::element(i));
            }
        }
        for (std::size_t i = 0; i < index.text_ranges.size(); ++i) {
            // Skip root-level text (whitespace between PI and root element).
            if (index.text_ranges[i].parent_tag == UINT32_MAX) continue;
            items.emplace_back(index.text_ranges[i].start, XPathNode::text(i));
        }
        std::sort(items.begin(), items.end(),
                  [](auto const& a, auto const& b) { return a.first < b.first; });
        std::vector<XPathNode> out;
        out.reserve(items.size());
        for (auto& it : items) out.push_back(it.second);
        return out;
    }

    std::vector<std::pair<std::uint64_t, XPathNode>> items;
    if (include_self) {
        items.emplace_back(index.tag_starts[start_idx], node);
    }
    auto close = index.matching_close(start_idx);
    std::size_t const close_idx = close.value_or(index.tag_count());
    for (std::size_t i = start_idx + 1; i < close_idx; ++i) {
        if (is_node_tag(index.tag_types[i])) {
            items.emplace_back(index.tag_starts[i], XPathNode::element(i));
        }
    }
    for (std::size_t i = 0; i < index.text_ranges.size(); ++i) {
        std::size_t const parent = index.text_ranges[i].parent_tag;
        if (parent >= start_idx && parent < close_idx) {
            items.emplace_back(index.text_ranges[i].start, XPathNode::text(i));
        }
    }
    std::sort(items.begin(), items.end(),
              [](auto const& a, auto const& b) { return a.first < b.first; });
    std::vector<XPathNode> out;
    out.reserve(items.size());
    for (auto& it : items) out.push_back(it.second);
    return out;
}

[[nodiscard]] std::vector<XPathNode>
eval_parent_axis(XmlIndex const& index, XPathNode const& node) {
    if (node.is_element()) {
        std::size_t const idx = node.index();
        if (idx == DOC_ROOT) return {};
        std::uint32_t const parent = index.parents[idx];
        if (parent != UINT32_MAX) return {XPathNode::element(parent)};
        return {XPathNode::element(DOC_ROOT)};
    }
    if (node.is_text()) {
        std::uint32_t const parent = index.text_ranges[node.index()].parent_tag;
        if (parent != UINT32_MAX) return {XPathNode::element(parent)};
        return {XPathNode::element(DOC_ROOT)};
    }
    if (node.is_attribute() || node.is_namespace()) {
        return {XPathNode::element(node.index())};
    }
    return {};
}

[[nodiscard]] std::vector<XPathNode>
eval_ancestor_axis(XmlIndex const& index, XPathNode const& node,
                   bool include_self) {
    std::vector<XPathNode> result;
    if (include_self) result.push_back(node);

    std::uint32_t current = UINT32_MAX;
    if (node.is_element()) {
        if (node.index() == DOC_ROOT) current = UINT32_MAX;
        else if (node.index() < index.tag_count())
            current = index.parents[node.index()];
    } else if (node.is_text()) {
        current = index.text_ranges[node.index()].parent_tag;
    } else if (node.is_attribute() || node.is_namespace()) {
        if (node.index() < index.tag_count())
            current = static_cast<std::uint32_t>(node.index());
    }

    while (current != UINT32_MAX &&
           static_cast<std::size_t>(current) < index.tag_count()) {
        result.push_back(XPathNode::element(current));
        current = index.parents[current];
    }

    if (!(node.is_element() && node.index() == DOC_ROOT)) {
        result.push_back(XPathNode::element(DOC_ROOT));
    }
    return result;
}

[[nodiscard]] std::vector<XPathNode>
eval_following_sibling_axis(XmlIndex const& index, XPathNode const& node) {
    if (!node.is_element()) return {};
    std::size_t const idx = node.index();
    if (idx == DOC_ROOT || idx >= index.tag_count()) return {};
    std::uint32_t const parent_tag = index.parents[idx];

    std::vector<XPathNode> result;
    if (index.has_indices() && parent_tag != UINT32_MAX) {
        std::size_t const parent_idx = parent_tag;
        for (auto child : index.child_tag_slice(parent_idx)) {
            if (static_cast<std::size_t>(child) > idx) {
                result.push_back(XPathNode::element(child));
            }
        }
        std::uint64_t const my_pos = index.tag_starts[idx];
        for (auto ti : index.child_text_slice(parent_idx)) {
            if (index.text_ranges[ti].start > my_pos) {
                result.push_back(XPathNode::text(ti));
            }
        }
        sort_doc_order(index, result);
    } else {
        std::uint16_t const depth = index.depths[idx];
        for (std::size_t i = idx + 1; i < index.tag_count(); ++i) {
            if (index.parents[i] == parent_tag && index.depths[i] == depth &&
                is_node_tag(index.tag_types[i])) {
                result.push_back(XPathNode::element(i));
            }
        }
    }
    return result;
}

[[nodiscard]] std::vector<XPathNode>
eval_preceding_sibling_axis(XmlIndex const& index, XPathNode const& node) {
    if (!node.is_element()) return {};
    std::size_t const idx = node.index();
    if (idx == DOC_ROOT || idx >= index.tag_count()) return {};
    std::uint32_t const parent_tag = index.parents[idx];

    std::vector<XPathNode> result;
    if (index.has_indices() && parent_tag != UINT32_MAX) {
        std::size_t const parent_idx = parent_tag;
        for (auto child : index.child_tag_slice(parent_idx)) {
            if (static_cast<std::size_t>(child) < idx) {
                result.push_back(XPathNode::element(child));
            }
        }
        std::uint64_t const my_pos = index.tag_starts[idx];
        for (auto ti : index.child_text_slice(parent_idx)) {
            if (index.text_ranges[ti].start < my_pos) {
                result.push_back(XPathNode::text(ti));
            }
        }
        sort_doc_order(index, result);
    } else {
        std::uint16_t const depth = index.depths[idx];
        for (std::size_t i = idx; i > 0; --i) {
            std::size_t const j = i - 1;
            if (index.parents[j] == parent_tag && index.depths[j] == depth &&
                is_node_tag(index.tag_types[j])) {
                result.push_back(XPathNode::element(j));
            }
        }
    }
    return result;
}

[[nodiscard]] std::vector<XPathNode>
eval_following_axis(XmlIndex const& index, XPathNode const& node) {
    bool const is_attr = node.is_attribute() || node.is_namespace();
    std::size_t idx;
    if (node.is_element()) idx = node.index();
    else if (node.is_attribute() || node.is_namespace()) idx = node.index();
    else return {};
    if (idx == DOC_ROOT || idx >= index.tag_count()) return {};

    auto close = index.matching_close(idx);
    std::size_t const close_idx = close.value_or(idx);
    std::vector<XPathNode> result;

    if (is_attr) {
        for (std::size_t i = idx + 1; i < index.tag_count(); ++i) {
            if (is_node_tag(index.tag_types[i]) &&
                index.tag_types[i] != TagType::Close) {
                result.push_back(XPathNode::element(i));
            }
        }
        for (std::size_t ti = 0; ti < index.text_ranges.size(); ++ti) {
            if (index.text_ranges[ti].start > index.tag_starts[idx]) {
                result.push_back(XPathNode::text(ti));
            }
        }
        sort_doc_order(index, result);
    } else {
        for (std::size_t i = close_idx + 1; i < index.tag_count(); ++i) {
            if (index.tag_types[i] == TagType::Open ||
                index.tag_types[i] == TagType::SelfClose) {
                result.push_back(XPathNode::element(i));
            }
        }
    }
    return result;
}

[[nodiscard]] std::vector<XPathNode>
eval_preceding_axis(XmlIndex const& index, XPathNode const& node) {
    std::size_t idx;
    if (node.is_element()) idx = node.index();
    else if (node.is_attribute() || node.is_namespace()) idx = node.index();
    else return {};
    if (idx == DOC_ROOT || idx >= index.tag_count()) return {};

    std::vector<XPathNode> result;
    if (!index.post_order.empty()) {
        for (std::size_t i = 0; i < idx; ++i) {
            if ((index.tag_types[i] == TagType::Open ||
                 index.tag_types[i] == TagType::SelfClose) &&
                !index.is_ancestor(i, idx)) {
                result.push_back(XPathNode::element(i));
            }
        }
    } else {
        // Fallback: build ancestor set via parent chain.
        std::unordered_set<std::uint32_t> ancestors;
        std::uint32_t current = index.parents[idx];
        while (current != UINT32_MAX) {
            ancestors.insert(current);
            current = index.parents[current];
        }
        for (std::size_t i = 0; i < idx; ++i) {
            if ((index.tag_types[i] == TagType::Open ||
                 index.tag_types[i] == TagType::SelfClose) &&
                !ancestors.contains(static_cast<std::uint32_t>(i))) {
                result.push_back(XPathNode::element(i));
            }
        }
    }
    return result;
}

[[nodiscard]] std::vector<XPathNode>
eval_attribute_axis(XmlIndex const& index, XPathNode const& node,
                    NodeTest const& test) {
    if (!node.is_element()) return {};
    std::size_t const idx = node.index();
    if (idx == DOC_ROOT || idx >= index.tag_count()) return {};

    switch (test.kind) {
        case NodeTest::Kind::Name: {
            // xmlns is not a regular attribute.
            if (test.name == "xmlns" || test.name.starts_with("xmlns:")) return {};
            if (index.get_attribute(idx, test.name)) {
                return {XPathNode::attribute(idx, attr_name_hash(test.name))};
            }
            return {};
        }
        case NodeTest::Kind::NamespacedName: {
            if (test.prefix == "xmlns") return {};
            if (test.local == "*") {
                std::string const prefix_colon = test.prefix + ":";
                std::vector<XPathNode> out;
                for (auto name : index.get_all_attribute_names(idx)) {
                    if (name.starts_with(prefix_colon)) {
                        out.push_back(XPathNode::attribute(idx, attr_name_hash(name)));
                    }
                }
                return out;
            }
            std::string const full = test.prefix + ":" + test.local;
            if (index.get_attribute(idx, full)) {
                return {XPathNode::attribute(idx, attr_name_hash(full))};
            }
            return {};
        }
        case NodeTest::Kind::Wildcard:
        case NodeTest::Kind::Node: {
            std::vector<XPathNode> out;
            for (auto name : index.get_all_attribute_names(idx)) {
                if (name == "xmlns" || name.starts_with("xmlns:")) continue;
                out.push_back(XPathNode::attribute(idx, attr_name_hash(name)));
            }
            return out;
        }
        default:
            return {};
    }
}

[[nodiscard]] std::vector<XPathNode>
eval_namespace_axis(XmlIndex const& index, XPathNode const& node) {
    if (!node.is_element()) return {};
    std::size_t const idx = node.index();
    if (idx == DOC_ROOT || idx >= index.tag_count()) return {};

    // Collect in-scope namespaces walking up; closer ancestor wins on duplicate.
    std::vector<std::pair<std::string, std::uint64_t>> ns_map;
    std::unordered_set<std::string> seen_prefixes;

    std::optional<std::size_t> current = idx;
    while (current) {
        std::size_t const cur = *current;
        if (cur < index.tag_count() &&
            (index.tag_types[cur] == TagType::Open ||
             index.tag_types[cur] == TagType::SelfClose)) {
            for (auto const& [prefix, _uri] : index.get_namespace_decls(cur)) {
                std::string p{prefix};
                if (seen_prefixes.insert(p).second) {
                    ns_map.emplace_back(p, attr_name_hash(p));
                }
            }
        }
        std::uint32_t const parent = index.parents[cur];
        current = (parent != UINT32_MAX) ? std::optional<std::size_t>{parent}
                                          : std::nullopt;
    }

    std::vector<XPathNode> out;
    out.reserve(ns_map.size());
    for (auto& [_, hash] : ns_map) {
        out.push_back(XPathNode::ns(idx, hash));
    }
    return out;
}

}  // namespace

bool
matches_node_test(XmlIndex const& index, XPathNode const& node,
                  NodeTest const& test) {
    if (test.kind == NodeTest::Kind::Node) return true;

    if (test.kind == NodeTest::Kind::Wildcard) {
        if (node.is_namespace()) return true;
        if (node.is_element()) {
            std::size_t const idx = node.index();
            return idx != DOC_ROOT && idx < index.tag_count() &&
                   (index.tag_types[idx] == TagType::Open ||
                    index.tag_types[idx] == TagType::SelfClose);
        }
        return false;
    }

    if (test.kind == NodeTest::Kind::Text) {
        return node.is_text();
    }

    if (node.is_element()) {
        std::size_t const idx = node.index();
        if (idx >= index.tag_count()) return false;
        switch (test.kind) {
            case NodeTest::Kind::Name:
                return (index.tag_types[idx] == TagType::Open ||
                        index.tag_types[idx] == TagType::SelfClose) &&
                       index.tag_name_eq(idx, test.name);
            case NodeTest::Kind::Comment:
                return index.tag_types[idx] == TagType::Comment;
            case NodeTest::Kind::PI:
                return index.tag_types[idx] == TagType::PI;
            case NodeTest::Kind::PIName:
                return index.tag_types[idx] == TagType::PI &&
                       index.tag_name_eq(idx, test.name);
            case NodeTest::Kind::NamespacedName: {
                TagType const tt = index.tag_types[idx];
                if (tt != TagType::Open && tt != TagType::SelfClose) return false;
                std::string_view const full = index.tag_name(idx);
                if (test.local == "*") {
                    return full.starts_with(test.prefix) &&
                           full.size() > test.prefix.size() &&
                           full[test.prefix.size()] == ':';
                }
                auto const colon = full.find(':');
                if (colon == std::string_view::npos) return false;
                return full.substr(0, colon) == test.prefix &&
                       full.substr(colon + 1) == test.local;
            }
            default:
                return false;
        }
    }

    if (node.is_attribute() && test.kind == NodeTest::Kind::Name) {
        return node.hash() == attr_name_hash(test.name);
    }
    if (node.is_namespace() && test.kind == NodeTest::Kind::Name) {
        return node.hash() == attr_name_hash(test.name);
    }
    return false;
}

std::vector<XPathNode>
eval_axis(XmlIndex const& index, XPathNode const& node, Axis axis,
          NodeTest const& test) {
    switch (axis) {
        case Axis::Child:
            return eval_child_axis(index, node);
        case Axis::Descendant:
            return eval_descendant_axis(index, node, false);
        case Axis::DescendantOrSelf:
            return eval_descendant_axis(index, node, true);
        case Axis::Parent:
            return eval_parent_axis(index, node);
        case Axis::Ancestor:
            return eval_ancestor_axis(index, node, false);
        case Axis::AncestorOrSelf:
            return eval_ancestor_axis(index, node, true);
        case Axis::FollowingSibling:
            return eval_following_sibling_axis(index, node);
        case Axis::PrecedingSibling:
            return eval_preceding_sibling_axis(index, node);
        case Axis::Following:
            return eval_following_axis(index, node);
        case Axis::Preceding:
            return eval_preceding_axis(index, node);
        case Axis::SelfAxis:
            return {node};
        case Axis::Attribute:
            return eval_attribute_axis(index, node, test);
        case Axis::Namespace:
            return eval_namespace_axis(index, node);
    }
    return {};
}

Result<std::vector<XPathNode>>
eval_step(XmlIndex const& index,
          std::vector<XPathNode> const& context, Step const& step) {
    std::vector<XPathNode> result;
    for (auto const& node : context) {
        auto candidates = eval_axis(index, node, step.axis, step.node_test);

        // Filter by node test (skip for attribute/namespace axes which pre-filter).
        std::vector<XPathNode> matched;
        if (step.axis == Axis::Attribute || step.axis == Axis::Namespace) {
            matched = std::move(candidates);
        } else {
            matched.reserve(candidates.size());
            for (auto const& c : candidates) {
                if (matches_node_test(index, c, step.node_test)) {
                    matched.push_back(c);
                }
            }
        }

        // Apply predicates per-context-node.
        for (auto const& pred : step.predicates) {
            if (!pred) continue;
            auto filtered = apply_predicate(index, matched, *pred);
            if (!filtered) return std::unexpected(filtered.error());
            matched = std::move(*filtered);
        }
        for (auto& m : matched) result.push_back(m);
    }

    if (context.size() > 1) {
        dedup_nodes(result);
        sort_doc_order(index, result);
    }
    return result;
}

}  // namespace rai::xml