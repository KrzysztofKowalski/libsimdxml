// XPath evaluation coordinator — implements the public API declared in
// `eval.hpp` by delegating to the `eval/` submodules.
//
// Ported from Rust `xpath/eval.rs`:
//   - `evaluate`              (lines 211-250)
//   - `eval_text`             (lines 252-292)
//   - `extract_text`          (lines 294-326)
//   - `eval_xpath`            (lines 186-209)
//   - `eval_expr_with_doc`    (lines 107-110)
//   - `eval_expr_with_context`(lines 112-121)
//   - `eval_standalone_expr`  (lines 123-140)
//   - `evaluate_from_context` (lines 1522-1580)
//   - `eval_predicate_value`  (lines 911-1037) — the predicate engine that
//     dispatches across literals, function calls, location paths, unary minus,
//     binary ops, and unions. Node-set comparison semantics (XPath §3.4) live
//     here for the BinaryOp arm with LocationPath operands.
//
// NOTE: the standalone-expression evaluator constructs a minimal dummy XML
// document ("<r/>") via `parse_scalar` (mirroring Rust). For the C++ port we
// depend on the structural parser's public API; if that API isn't available
// in this build, standalone expressions return an `XPathEvalError` instead
// of crashing — matching the Rust behavior of requiring a parser.
#include "eval.hpp"
#include "eval/internal.hpp"
#include "eval/axis_step.hpp"
#include "eval/binary_op.hpp"
#include "eval/filter.hpp"
#include "eval/function_call.hpp"
#include "eval/literal.hpp"
#include "eval/path_expr.hpp"
#include "parser.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace simdxml {

Result<XpathValue>
eval_predicate_value(XmlIndex const& index, XPathNode const& node,
                     XPathExpr const& expr, std::size_t position,
                     std::size_t size) {
    switch (expr.kind) {
        case XPathExpr::Kind::StringLiteral:
            return eval_string_literal(expr.string_literal);
        case XPathExpr::Kind::NumberLiteral:
            return eval_number_literal(expr.number_literal);
        case XPathExpr::Kind::FunctionCall:
            return eval_function(index, node, expr.function_name,
                                 expr.function_args, position, size);
        case XPathExpr::Kind::LocationPath: {
            // @attr shortcut.
            if (auto const& path = expr.location_path;
                !path.absolute && path.steps.size() == 1 &&
                path.steps[0].axis == Axis::Attribute &&
                path.steps[0].predicates.empty() &&
                path.steps[0].node_test.kind == NodeTest::Kind::Name) {
                auto const& attr_name = path.steps[0].node_test.name;
                if (attr_name != "xmlns" && !attr_name.starts_with("xmlns:")) {
                    if (node.is_element()) {
                        if (auto v = index.get_attribute(node.index(),
                                                          attr_name)) {
                            return XpathValue::string(std::string(*v));
                        }
                    }
                    return XpathValue::string(std::string{});
                }
            }
            // General case: evaluate path in context, return string value.
            auto nodes = evaluate_in_context(index, node, expr);
            if (!nodes) return std::unexpected(nodes.error());
            if (!nodes->empty()) {
                return XpathValue::string(node_string_value(index,
                                                             nodes->front()));
            }
            return XpathValue::string(std::string{});
        }
        case XPathExpr::Kind::UnaryMinus: {
            if (!expr.unary_inner) {
                return std::unexpected(SimdXmlError::xpath_eval_error(
                    "UnaryMinus with null inner expression"));
            }
            auto v = eval_predicate_value(index, node, *expr.unary_inner,
                                           position, size);
            if (!v) return std::unexpected(v.error());
            return XpathValue::number(-v->to_number());
        }
        case XPathExpr::Kind::BinaryOp: {
            if (!expr.binary_left || !expr.binary_right) {
                return std::unexpected(SimdXmlError::xpath_eval_error(
                    "BinaryOp with null operand"));
            }
            BinaryOp const op = expr.binary_op;
            bool const is_comparison =
                op == BinaryOp::Eq || op == BinaryOp::Neq ||
                op == BinaryOp::Lt || op == BinaryOp::Gt ||
                op == BinaryOp::Lte || op == BinaryOp::Gte;
            bool const left_is_path =
                expr.binary_left->kind == XPathExpr::Kind::LocationPath;
            bool const right_is_path =
                expr.binary_right->kind == XPathExpr::Kind::LocationPath;

            if (is_comparison && (left_is_path || right_is_path)) {
                // Node-set comparison semantics (XPath §3.4).
                std::vector<XPathNode> left_nodes;
                std::vector<XPathNode> right_nodes;
                if (left_is_path) {
                    auto r = evaluate_in_context(index, node, *expr.binary_left);
                    if (!r) return std::unexpected(r.error());
                    left_nodes = std::move(*r);
                }
                if (right_is_path) {
                    auto r = evaluate_in_context(index, node, *expr.binary_right);
                    if (!r) return std::unexpected(r.error());
                    right_nodes = std::move(*r);
                }

                // `found` flag + break out of nested loops via do-while(false)
                // (avoids fragile cross-scope goto).
                bool result = false;
                auto found = [&] { result = true; };
                if (left_is_path && right_is_path) {
                    // Both are node-sets: true if any pair of string values matches.
                    std::vector<std::string> lvs, rvs;
                    lvs.reserve(left_nodes.size());
                    for (auto const& n : left_nodes)
                        lvs.push_back(node_string_value(index, n));
                    rvs.reserve(right_nodes.size());
                    for (auto const& n : right_nodes)
                        rvs.push_back(node_string_value(index, n));
                    do {
                        for (auto const& lv : lvs) {
                            for (auto const& rv : rvs) {
                                if (compare_values(XpathValue::string(lv), op,
                                                   XpathValue::string(rv))) {
                                    found();
                                    break;
                                }
                            }
                            if (result) break;
                        }
                    } while (false);
                } else if (left_is_path) {
                    auto r = eval_predicate_value(index, node,
                                                   *expr.binary_right, position, size);
                    if (!r) return std::unexpected(r.error());
                    bool const is_eq = (op == BinaryOp::Eq || op == BinaryOp::Neq);
                    if (is_eq && r->kind() == XpathValue::Kind::Boolean) {
                        auto lv = XpathValue::boolean(!left_nodes.empty());
                        result = compare_values(lv, op, *r);
                    } else {
                        for (auto const& n : left_nodes) {
                            auto lv = XpathValue::string(node_string_value(index, n));
                            if (compare_values(lv, op, *r)) { found(); break; }
                        }
                    }
                } else {
                    auto l = eval_predicate_value(index, node,
                                                   *expr.binary_left, position, size);
                    if (!l) return std::unexpected(l.error());
                    bool const is_eq = (op == BinaryOp::Eq || op == BinaryOp::Neq);
                    if (is_eq && l->kind() == XpathValue::Kind::Boolean) {
                        auto rv = XpathValue::boolean(!right_nodes.empty());
                        return XpathValue::boolean(compare_values(*l, op, rv));
                    }
                    for (auto const& n : right_nodes) {
                        auto rv = XpathValue::string(node_string_value(index, n));
                        if (compare_values(*l, op, rv)) { found(); break; }
                    }
                }
                return XpathValue::boolean(result);
            }

            // Both sides scalar: arithmetic returns number, comparison returns boolean.
            auto l = eval_predicate_value(index, node, *expr.binary_left,
                                           position, size);
            if (!l) return std::unexpected(l.error());
            auto r = eval_predicate_value(index, node, *expr.binary_right,
                                           position, size);
            if (!r) return std::unexpected(r.error());
            return eval_binary_op_scalar(op, *l, *r);
        }
        case XPathExpr::Kind::Union:
        case XPathExpr::Kind::FilterPath:
        case XPathExpr::Kind::GlobalFilter: {
            auto nodes = evaluate_in_context(index, node, expr);
            if (!nodes) return std::unexpected(nodes.error());
            if (!nodes->empty()) {
                return XpathValue::string(node_string_value(index,
                                                             nodes->front()));
            }
            return XpathValue::string(std::string{});
        }
    }
    return std::unexpected(SimdXmlError::xpath_eval_error(
        "Unknown XPath expression kind"));
}

namespace {

/// Convert an `XpathValue` to a `StandaloneResult` (mirrors Rust match).
[[nodiscard]] StandaloneResult
to_standalone(XpathValue const& v) {
    StandaloneResult r;
    switch (v.kind()) {
        case XpathValue::Kind::Number:
            r.kind = StandaloneResult::Kind::Number;
            r.number = v.as_number();
            return r;
        case XpathValue::Kind::String:
            r.kind = StandaloneResult::Kind::String;
            r.str = v.to_string();
            return r;
        case XpathValue::Kind::Boolean:
            r.kind = StandaloneResult::Kind::Boolean;
            r.boolean = v.as_boolean();
            return r;
    }
    return r;
}

[[nodiscard]] Result<std::vector<XPathNode>>
evaluate_dispatch(XmlIndex const& index, XPathExpr const& expr) {
    switch (expr.kind) {
        case XPathExpr::Kind::LocationPath:
            return eval_location_path(index, expr.location_path);
        case XPathExpr::Kind::Union: {
            std::vector<XPathNode> result;
            for (auto const& e : expr.union_members) {
                if (!e) continue;
                auto sub = evaluate_dispatch(index, *e);
                if (!sub) return std::unexpected(sub.error());
                for (auto& n : *sub) result.push_back(n);
            }
            dedup_nodes(result);
            sort_doc_order(index, result);
            return result;
        }
        case XPathExpr::Kind::FunctionCall:
            if (expr.function_name == "id") {
                return eval_id_function(index, expr.function_args);
            }
            return std::unexpected(SimdXmlError::xpath_eval_error(
                "Only location paths, unions, and id() are supported"));
        case XPathExpr::Kind::FilterPath: {
            if (!expr.filter_inner) return std::vector<XPathNode>{};
            auto context = evaluate_dispatch(index, *expr.filter_inner);
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
            auto result = evaluate_dispatch(index, *expr.global_filter_inner);
            if (!result) return std::unexpected(result.error());
            for (auto const& pred : expr.global_filter_preds) {
                if (!pred) continue;
                auto filtered = apply_predicate(index, *result, *pred);
                if (!filtered) return std::unexpected(filtered.error());
                *result = std::move(*filtered);
            }
            return result;
        }
        default:
            return std::unexpected(SimdXmlError::xpath_eval_error(
                "Only location paths, unions, and id() are supported"));
    }
}

}  // namespace

Result<std::vector<XPathNode>>
evaluate(XmlIndex const& index, XPathExpr const& expr) {
    return evaluate_dispatch(index, expr);
}

Result<std::vector<std::string_view>>
eval_text(XmlIndex const& index, XPathExpr const& expr) {
    auto nodes = evaluate(index, expr);
    if (!nodes) return std::unexpected(nodes.error());
    return extract_text(index, *nodes);
}

Result<std::vector<std::string_view>>
extract_text(XmlIndex const& index, std::vector<XPathNode> const& nodes) {
    std::vector<std::string_view> results;
    results.reserve(nodes.size());
    for (auto const& node : nodes) {
        if (node.is_element()) {
            std::size_t const idx = node.index();
            if (idx == DOC_ROOT) continue;
            auto text_slice = index.child_text_slice(idx);
            if (!text_slice.empty()) {
                for (auto ti : text_slice) {
                    if (ti >= index.text_ranges.size()) continue;
                    auto text = index.text_content(index.text_ranges[ti]);
                    if (!text.empty()) results.push_back(text);
                }
            } else {
                for (auto const& range : index.text_ranges) {
                    if (range.parent_tag == idx) {
                        auto text = index.text_content(range);
                        if (!text.empty()) results.push_back(text);
                    }
                }
            }
        } else if (node.is_text()) {
            std::size_t const idx = node.index();
            if (idx >= index.text_ranges.size()) continue;
            results.push_back(index.text_content(index.text_ranges[idx]));
        }
    }
    return results;
}

Result<XPathResult>
eval_xpath(XmlIndex const& index, XPathExpr const& expr) {
    // First try as a node-set expression (the common case).
    bool is_nodeset = false;
    switch (expr.kind) {
        case XPathExpr::Kind::LocationPath:
        case XPathExpr::Kind::Union:
        case XPathExpr::Kind::FilterPath:
        case XPathExpr::Kind::GlobalFilter:
            is_nodeset = true;
            break;
        case XPathExpr::Kind::FunctionCall:
            if (expr.function_name == "id") is_nodeset = true;
            break;
        default: break;
    }
    if (is_nodeset) {
        auto nodes = evaluate(index, expr);
        if (!nodes) return std::unexpected(nodes.error());
        return XPathResult::node_set(std::move(*nodes));
    }

    // Scalar expression — evaluate via predicate engine with doc-root context.
    auto doc_root = XPathNode::element(DOC_ROOT);
    auto v = eval_predicate_value(index, doc_root, expr, 1, 1);
    if (!v) return std::unexpected(v.error());
    switch (v->kind()) {
        case XpathValue::Kind::Number:  return XPathResult::number(v->as_number());
        case XpathValue::Kind::String:  return XPathResult::string(v->to_string());
        case XpathValue::Kind::Boolean: return XPathResult::boolean(v->as_boolean());
    }
    return std::unexpected(SimdXmlError::xpath_eval_error(
        "Unknown scalar value kind"));
}

Result<StandaloneResult>
eval_expr_with_doc(XmlIndex const& index, std::string_view expr_str) {
    return eval_expr_with_context(index, expr_str, XPathNode::element(DOC_ROOT));
}

Result<StandaloneResult>
eval_expr_with_context(XmlIndex const& index, std::string_view expr_str,
                       XPathNode context) {
    auto parsed = parse_xpath_predicate_expr(expr_str);
    if (!parsed) return std::unexpected(parsed.error());
    auto v = eval_predicate_value(index, context, *parsed, 1, 1);
    if (!v) return std::unexpected(v.error());
    return to_standalone(*v);
}

Result<StandaloneResult>
eval_standalone_expr(std::string_view expr_str) {
    // Parse as a predicate expression (supports arithmetic, comparisons, functions).
    auto parsed = parse_xpath_predicate_expr(expr_str);
    if (!parsed) return std::unexpected(parsed.error());

    // Create a minimal dummy context "<r/>".
    static std::string const dummy_xml = "<r/>";
    XmlIndex index{std::span<std::byte const>(
        reinterpret_cast<std::byte const*>(dummy_xml.data()),
        dummy_xml.size())};
    // We cannot easily call parse_scalar from here without dragging in the
    // structural parser dependency. Build a minimal index by hand: one
    // self-closing tag "<r/>".
    index.tag_starts.push_back(0);
    index.tag_ends.push_back(dummy_xml.size());
    index.tag_types.push_back(TagType::SelfClose);
    index.depths.push_back(0);
    index.parents.push_back(UINT32_MAX);
    // Tag name "r" at offset 1, length 1.
    index.tag_names.emplace_back(1ull, 1u);

    auto node = XPathNode::element(DOC_ROOT);
    auto v = eval_predicate_value(index, node, *parsed, 1, 1);
    if (!v) return std::unexpected(v.error());
    return to_standalone(*v);
}

Result<std::vector<XPathNode>>
evaluate_from_context(XmlIndex const& index, XPathExpr const& expr,
                      XPathNode context_node) {
    return evaluate_from_context_impl(index, expr, context_node);
}

std::string
XPathResult::to_display_string(XmlIndex const& index) const {
    switch (kind_) {
        case Kind::NodeSet: {
            std::string out;
            for (std::size_t i = 0; i < nodes_.size(); ++i) {
                if (i) out.push_back('\n');
                out += node_string_value(index, nodes_[i]);
            }
            return out;
        }
        case Kind::String:  return str_;
        case Kind::Number:  return xpath_format_number(number_);
        case Kind::Boolean: return boolean_ ? "true" : "false";
    }
    return {};
}

}  // namespace simdxml