// Implementation of XPath path-expression evaluation.
//
// Ported from Rust `xpath/eval.rs` lines 358-653 and 1522-1629. The fused
// descendant-or-self/child scan handles `//name` patterns in one pass over
// the tag array, optionally with per-parent predicate grouping for `//p[1]`.
// `evaluate_in_context` and `evaluate_from_context` dispatch on expression
// kind: relative location paths evaluate from the context node, absolute
// paths from the document root, unions concatenate and dedup.
#include "path_expr.hpp"
#include "axis_step.hpp"
#include "filter.hpp"
#include "function_call.hpp"

#include <algorithm>
#include <cstdint>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rai::xml {

namespace {

/// Compute the [scan_start, scan_end) range for a fused descendant scan
/// rooted at `ctx_node`. Returns std::nullopt if the context node can't
/// contribute (e.g., text/attribute context).
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
fused_scan_range(XmlIndex const& index, XPathNode const& ctx_node) {
    if (ctx_node.is_element()) {
        std::size_t const idx = ctx_node.index();
        if (idx == DOC_ROOT) return std::make_pair(std::size_t{0}, index.tag_count());
        if (idx < index.tag_count()) {
            auto close = index.matching_close(idx);
            std::size_t const close_idx = close.value_or(index.tag_count());
            // Start from idx+1 to skip the context element itself for
            // fused descendant-or-self/child — the context's children ARE
            // included (self is in desc-or-self), but the context itself
            // is NOT a child of any node in the descendant-or-self set.
            return std::make_pair(idx + 1, close_idx);
        }
    }
    return std::nullopt;
}

}  // namespace

Result<std::vector<XPathNode>>
eval_fused_descendant_child(XmlIndex const& index,
                            std::vector<XPathNode> const& context,
                            Step const& child_step) {
    std::vector<XPathNode> result;

    for (auto const& ctx_node : context) {
        auto range = fused_scan_range(index, ctx_node);
        if (!range) continue;
        auto [scan_start, scan_end] = *range;

        switch (child_step.node_test.kind) {
            case NodeTest::Kind::Name: {
                std::string const& name = child_step.node_test.name;
                auto posting = index.tags_by_name(name);
                if (!posting.empty() && scan_start == 0 &&
                    scan_end == index.tag_count()) {
                    for (auto j : posting) {
                        result.push_back(XPathNode::element(j));
                    }
                } else if (!posting.empty()) {
                    auto lo = std::lower_bound(posting.begin(), posting.end(),
                                                static_cast<std::uint32_t>(scan_start));
                    auto hi = std::lower_bound(posting.begin(), posting.end(),
                                                static_cast<std::uint32_t>(scan_end));
                    for (auto it = lo; it != hi; ++it) {
                        result.push_back(XPathNode::element(*it));
                    }
                } else {
                    for (std::size_t j = scan_start; j < scan_end; ++j) {
                        TagType const tt = index.tag_types[j];
                        if ((tt == TagType::Open || tt == TagType::SelfClose) &&
                            index.tag_name_eq(j, name)) {
                            result.push_back(XPathNode::element(j));
                        }
                    }
                }
                break;
            }
            case NodeTest::Kind::Text: {
                for (std::size_t ti = 0; ti < index.text_ranges.size(); ++ti) {
                    std::size_t const p = index.text_ranges[ti].parent_tag;
                    if (p >= scan_start && p < scan_end) {
                        result.push_back(XPathNode::text(ti));
                    }
                }
                break;
            }
            case NodeTest::Kind::Wildcard: {
                for (std::size_t j = scan_start; j < scan_end; ++j) {
                    TagType const tt = index.tag_types[j];
                    if (tt == TagType::Open || tt == TagType::SelfClose) {
                        result.push_back(XPathNode::element(j));
                    }
                }
                break;
            }
            case NodeTest::Kind::Node: {
                for (std::size_t j = scan_start; j < scan_end; ++j) {
                    if (is_node_tag(index.tag_types[j])) {
                        result.push_back(XPathNode::element(j));
                    }
                }
                for (std::size_t ti = 0; ti < index.text_ranges.size(); ++ti) {
                    std::size_t const p = index.text_ranges[ti].parent_tag;
                    if (p >= scan_start && p < scan_end) {
                        result.push_back(XPathNode::text(ti));
                    }
                }
                break;
            }
            case NodeTest::Kind::Comment: {
                for (std::size_t j = scan_start; j < scan_end; ++j) {
                    if (index.tag_types[j] == TagType::Comment) {
                        result.push_back(XPathNode::element(j));
                    }
                }
                break;
            }
            case NodeTest::Kind::NamespacedName: {
                auto const& prefix = child_step.node_test.prefix;
                auto const& local = child_step.node_test.local;
                if (local == "*") {
                    std::string const prefix_colon = prefix + ":";
                    for (std::size_t j = scan_start; j < scan_end; ++j) {
                        TagType const tt = index.tag_types[j];
                        if ((tt == TagType::Open || tt == TagType::SelfClose) &&
                            index.tag_name(j).starts_with(prefix_colon)) {
                            result.push_back(XPathNode::element(j));
                        }
                    }
                } else {
                    std::string const full_name = prefix + ":" + local;
                    auto posting = index.tags_by_name(full_name);
                    if (!posting.empty() && scan_start == 0 &&
                        scan_end == index.tag_count()) {
                        for (auto j : posting) result.push_back(XPathNode::element(j));
                    } else if (!posting.empty()) {
                        auto lo = std::lower_bound(posting.begin(), posting.end(),
                                                    static_cast<std::uint32_t>(scan_start));
                        auto hi = std::lower_bound(posting.begin(), posting.end(),
                                                    static_cast<std::uint32_t>(scan_end));
                        for (auto it = lo; it != hi; ++it) {
                            result.push_back(XPathNode::element(*it));
                        }
                    } else {
                        for (std::size_t j = scan_start; j < scan_end; ++j) {
                            TagType const tt = index.tag_types[j];
                            if ((tt == TagType::Open || tt == TagType::SelfClose) &&
                                index.tag_name_eq(j, full_name)) {
                                result.push_back(XPathNode::element(j));
                            }
                        }
                    }
                }
                break;
            }
            default: {
                // Fallback to general descendant + child step.
                auto desc = eval_axis(index, ctx_node, Axis::DescendantOrSelf,
                                       child_step.node_test);
                for (auto const& dn : desc) {
                    auto children = eval_axis(index, dn, Axis::Child,
                                               child_step.node_test);
                    for (auto const& c : children) {
                        if (matches_node_test(index, c, child_step.node_test)) {
                            result.push_back(c);
                        }
                    }
                }
                break;
            }
        }
    }

    if (context.size() > 1) {
        dedup_nodes(result);
        sort_doc_order(index, result);
    }
    return result;
}

Result<std::vector<XPathNode>>
eval_fused_descendant_child_with_preds(XmlIndex const& index,
                                       std::vector<XPathNode> const& context,
                                       Step const& child_step) {
    std::vector<XPathNode> result;
    auto inline_attr = extract_simple_attr_eq(child_step.predicates);

    for (auto const& ctx_node : context) {
        auto range = fused_scan_range(index, ctx_node);
        if (!range) continue;
        auto [scan_start, scan_end] = *range;

        std::vector<XPathNode> all_matches;
        switch (child_step.node_test.kind) {
            case NodeTest::Kind::Name: {
                std::string const& name = child_step.node_test.name;
                auto posting = index.tags_by_name(name);
                std::vector<std::size_t> js;
                if (!posting.empty() && scan_start == 0 &&
                    scan_end == index.tag_count()) {
                    for (auto j : posting) js.push_back(j);
                } else if (!posting.empty()) {
                    auto lo = std::lower_bound(posting.begin(), posting.end(),
                                                static_cast<std::uint32_t>(scan_start));
                    auto hi = std::lower_bound(posting.begin(), posting.end(),
                                                static_cast<std::uint32_t>(scan_end));
                    for (auto it = lo; it != hi; ++it) js.push_back(*it);
                } else {
                    for (std::size_t j = scan_start; j < scan_end; ++j) {
                        TagType const tt = index.tag_types[j];
                        if ((tt == TagType::Open || tt == TagType::SelfClose) &&
                            index.tag_name_eq(j, name)) {
                            js.push_back(j);
                        }
                    }
                }
                for (auto j : js) {
                    if (inline_attr) {
                        auto const& [attr, val, _] = *inline_attr;
                        if (auto v = index.get_attribute(j, attr); v != val) continue;
                    }
                    all_matches.push_back(XPathNode::element(j));
                }
                break;
            }
            case NodeTest::Kind::NamespacedName: {
                auto const& prefix = child_step.node_test.prefix;
                auto const& local = child_step.node_test.local;
                if (local == "*") {
                    std::string const prefix_colon = prefix + ":";
                    for (std::size_t j = scan_start; j < scan_end; ++j) {
                        TagType const tt = index.tag_types[j];
                        if ((tt == TagType::Open || tt == TagType::SelfClose) &&
                            index.tag_name(j).starts_with(prefix_colon)) {
                            all_matches.push_back(XPathNode::element(j));
                        }
                    }
                } else {
                    std::string const full_name = prefix + ":" + local;
                    for (std::size_t j = scan_start; j < scan_end; ++j) {
                        TagType const tt = index.tag_types[j];
                        if ((tt == TagType::Open || tt == TagType::SelfClose) &&
                            index.tag_name_eq(j, full_name)) {
                            all_matches.push_back(XPathNode::element(j));
                        }
                    }
                }
                break;
            }
            default: {
                for (std::size_t j = scan_start; j < scan_end; ++j) {
                    if (matches_node_test(index, XPathNode::element(j),
                                          child_step.node_test)) {
                        all_matches.push_back(XPathNode::element(j));
                    }
                }
                break;
            }
        }

        // Group by parent, apply predicates per group (XPath //p[1] = first
        // p child of EACH parent).
        std::unordered_map<std::uint32_t, std::vector<XPathNode>> parent_groups;
        for (auto const& m : all_matches) {
            std::uint32_t parent;
            if (m.is_element() && m.index() < index.tag_count()) {
                parent = index.parents[m.index()];
            } else {
                parent = UINT32_MAX;
            }
            parent_groups[parent].push_back(m);
        }
        for (auto& [_, group] : parent_groups) {
            for (std::size_t pi = 0; pi < child_step.predicates.size(); ++pi) {
                // Skip predicate already applied inline.
                if (inline_attr && std::get<2>(*inline_attr) == pi) continue;
                if (!child_step.predicates[pi]) continue;
                auto filtered = apply_predicate(index, group,
                                                 *child_step.predicates[pi]);
                if (!filtered) return std::unexpected(filtered.error());
                group = std::move(*filtered);
            }
            for (auto& g : group) result.push_back(g);
        }
    }

    dedup_nodes(result);
    sort_doc_order(index, result);
    return result;
}

Result<std::vector<XPathNode>>
eval_location_path(XmlIndex const& index, LocationPath const& path) {
    std::vector<XPathNode> context{XPathNode::element(DOC_ROOT)};

    auto const& steps = path.steps;
    std::size_t i = 0;
    while (i < steps.size()) {
        // Pattern: DescendantOrSelf::node() + child::Name(x) with no
        // predicates on the desc step → fused descendant scan.
        if (i + 1 < steps.size() &&
            steps[i].axis == Axis::DescendantOrSelf &&
            steps[i].node_test.kind == NodeTest::Kind::Node &&
            steps[i].predicates.empty() &&
            steps[i + 1].axis == Axis::Child) {
            if (steps[i + 1].predicates.empty()) {
                auto r = eval_fused_descendant_child(index, context, steps[i + 1]);
                if (!r) return std::unexpected(r.error());
                context = std::move(*r);
            } else {
                auto r = eval_fused_descendant_child_with_preds(index, context,
                                                                  steps[i + 1]);
                if (!r) return std::unexpected(r.error());
                context = std::move(*r);
            }
            i += 2;
        } else {
            auto r = eval_step(index, context, steps[i]);
            if (!r) return std::unexpected(r.error());
            context = std::move(*r);
            i += 1;
        }
    }
    return context;
}

Result<std::vector<XPathNode>>
evaluate_in_context(XmlIndex const& index, XPathNode const& context_node,
                    XPathExpr const& expr) {
    switch (expr.kind) {
        case XPathExpr::Kind::LocationPath: {
            auto const& path = expr.location_path;
            if (!path.absolute) {
                std::vector<XPathNode> context{context_node};
                for (auto const& step : path.steps) {
                    auto r = eval_step(index, context, step);
                    if (!r) return std::unexpected(r.error());
                    context = std::move(*r);
                }
                return context;
            }
            // Absolute path: evaluate from document root.
            return evaluate(index, expr);
        }
        case XPathExpr::Kind::Union: {
            std::vector<XPathNode> result;
            for (auto const& e : expr.union_members) {
                if (!e) continue;
                auto sub = evaluate_in_context(index, context_node, *e);
                if (!sub) return std::unexpected(sub.error());
                for (auto& n : *sub) result.push_back(n);
            }
            dedup_nodes(result);
            sort_doc_order(index, result);
            return result;
        }
        case XPathExpr::Kind::FilterPath: {
            if (!expr.filter_inner) return std::vector<XPathNode>{};
            auto context = evaluate_in_context(index, context_node,
                                                *expr.filter_inner);
            if (!context) return std::unexpected(context.error());
            for (auto const& step : expr.filter_steps) {
                auto r = eval_step(index, *context, step);
                if (!r) return std::unexpected(r.error());
                *context = std::move(*r);
            }
            return context;
        }
        case XPathExpr::Kind::GlobalFilter: {
            if (!expr.global_filter_inner) return std::vector<XPathNode>{};
            auto nodes = evaluate_in_context(index, context_node,
                                              *expr.global_filter_inner);
            if (!nodes) return std::unexpected(nodes.error());
            for (auto const& pred : expr.global_filter_preds) {
                if (!pred) continue;
                auto filtered = apply_predicate(index, *nodes, *pred);
                if (!filtered) return std::unexpected(filtered.error());
                *nodes = std::move(*filtered);
            }
            return nodes;
        }
        case XPathExpr::Kind::FunctionCall:
            if (expr.function_name == "id") {
                return evaluate(index, expr);
            }
            return std::vector<XPathNode>{};
        default:
            return std::vector<XPathNode>{};
    }
}

Result<std::vector<XPathNode>>
evaluate_from_context_impl(XmlIndex const& index, XPathExpr const& expr,
                           XPathNode const& context_node) {
    switch (expr.kind) {
        case XPathExpr::Kind::LocationPath: {
            auto const& path = expr.location_path;
            if (!path.absolute) {
                std::vector<XPathNode> context{context_node};
                auto const& steps = path.steps;
                std::size_t i = 0;
                while (i < steps.size()) {
                    if (i + 1 < steps.size() &&
                        steps[i].axis == Axis::DescendantOrSelf &&
                        steps[i].node_test.kind == NodeTest::Kind::Node &&
                        steps[i].predicates.empty() &&
                        steps[i + 1].axis == Axis::Child) {
                        if (steps[i + 1].predicates.empty()) {
                            auto r = eval_fused_descendant_child(index, context,
                                                                  steps[i + 1]);
                            if (!r) return std::unexpected(r.error());
                            context = std::move(*r);
                        } else {
                            auto r = eval_fused_descendant_child_with_preds(
                                index, context, steps[i + 1]);
                            if (!r) return std::unexpected(r.error());
                            context = std::move(*r);
                        }
                        i += 2;
                    } else {
                        auto r = eval_step(index, context, steps[i]);
                        if (!r) return std::unexpected(r.error());
                        context = std::move(*r);
                        i += 1;
                    }
                }
                return context;
            }
            return evaluate(index, expr);
        }
        case XPathExpr::Kind::Union: {
            std::vector<XPathNode> result;
            for (auto const& e : expr.union_members) {
                if (!e) continue;
                auto sub = evaluate_from_context_impl(index, *e, context_node);
                if (!sub) return std::unexpected(sub.error());
                for (auto& n : *sub) result.push_back(n);
            }
            dedup_nodes(result);
            sort_doc_order(index, result);
            return result;
        }
        case XPathExpr::Kind::GlobalFilter: {
            if (!expr.global_filter_inner) return std::vector<XPathNode>{};
            auto nodes = evaluate_from_context_impl(index, *expr.global_filter_inner,
                                                     context_node);
            if (!nodes) return std::unexpected(nodes.error());
            for (auto const& pred : expr.global_filter_preds) {
                if (!pred) continue;
                auto filtered = apply_predicate(index, *nodes, *pred);
                if (!filtered) return std::unexpected(filtered.error());
                *nodes = std::move(*filtered);
            }
            return nodes;
        }
        case XPathExpr::Kind::FilterPath: {
            if (!expr.filter_inner) return std::vector<XPathNode>{};
            auto context = evaluate_from_context_impl(index, *expr.filter_inner,
                                                       context_node);
            if (!context) return std::unexpected(context.error());
            for (auto const& step : expr.filter_steps) {
                auto r = eval_step(index, *context, step);
                if (!r) return std::unexpected(r.error());
                *context = std::move(*r);
            }
            return context;
        }
        default:
            return evaluate(index, expr);
    }
}

}  // namespace rai::xml