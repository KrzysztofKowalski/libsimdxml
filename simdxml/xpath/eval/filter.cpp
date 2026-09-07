// Implementation of XPath predicate filtering.
//
// Ported from Rust `xpath/eval.rs` lines 736-875 plus the SIMD batch fast
// paths. Positional vs boolean predicate dispatch follows XPath §3.4:
// a numeric result triggers position comparison, otherwise the value's
// truthiness decides. SIMD batch paths delegate to `simd_pred.hpp` when
// the candidate count exceeds `XPATH_BATCH_THRESHOLD`.
#include "filter.hpp"
#include "function_call.hpp"
#include "literal.hpp"
#include "binary_op.hpp"
#include "../simd_pred.hpp"

#include <cmath>
#include <tuple>
#include <utility>
#include <vector>

namespace simdxml {

namespace {

/// Try to use SIMD batched evaluation for `contains(., 'literal')` or
/// `starts-with(., 'prefix')` predicates. Returns std::nullopt to fall back
/// to per-node evaluation. Mirrors Rust `try_batch_string_predicate`.
[[nodiscard]] std::optional<std::vector<XPathNode>>
try_batch_string_predicate(XmlIndex const& index,
                           std::vector<XPathNode> const& nodes,
                           std::string_view func_name,
                           std::vector<std::unique_ptr<XPathExpr>> const& args) {
    if (args.size() != 2) return std::nullopt;
    if (!args[0] || !args[1]) return std::nullopt;

    // First arg must be self (.) — a relative location path with single
    // self::node() step.
    if (args[0]->kind != XPathExpr::Kind::LocationPath) return std::nullopt;
    auto const& path = args[0]->location_path;
    if (path.absolute || path.steps.size() != 1) return std::nullopt;
    auto const& step = path.steps[0];
    if (step.axis != Axis::SelfAxis) return std::nullopt;
    if (step.node_test.kind != NodeTest::Kind::Node) return std::nullopt;
    if (!step.predicates.empty()) return std::nullopt;

    // Second arg must be a string literal.
    if (args[1]->kind != XPathExpr::Kind::StringLiteral) return std::nullopt;
    std::string_view const needle = args[1]->string_literal;

    std::vector<bool> mask;
    if (func_name == "contains") {
        mask = batch_contains(index, nodes, needle);
    } else if (func_name == "starts-with") {
        mask = batch_starts_with(index, nodes, needle);
    } else {
        return std::nullopt;
    }

    std::vector<XPathNode> out;
    out.reserve(nodes.size());
    for (std::size_t i = 0; i < nodes.size() && i < mask.size(); ++i) {
        if (mask[i]) out.push_back(nodes[i]);
    }
    return out;
}

}  // namespace

std::optional<std::tuple<std::string, std::string, std::size_t>>
extract_simple_attr_eq(std::vector<std::unique_ptr<XPathExpr>> const& preds) {
    for (std::size_t i = 0; i < preds.size(); ++i) {
        auto const& pred = preds[i];
        if (!pred) continue;
        if (pred->kind != XPathExpr::Kind::BinaryOp) continue;
        if (pred->binary_op != BinaryOp::Eq) continue;

        // left is @attr, right is 'literal'
        if (pred->binary_left &&
            pred->binary_left->kind == XPathExpr::Kind::LocationPath) {
            auto const& path = pred->binary_left->location_path;
            if (!path.absolute && path.steps.size() == 1 &&
                path.steps[0].axis == Axis::Attribute &&
                path.steps[0].predicates.empty() &&
                path.steps[0].node_test.kind == NodeTest::Kind::Name) {
                if (pred->binary_right &&
                    pred->binary_right->kind == XPathExpr::Kind::StringLiteral) {
                    return std::make_tuple(path.steps[0].node_test.name,
                                           pred->binary_right->string_literal, i);
                }
            }
        }
        // reversed: 'literal' = @attr
        if (pred->binary_right &&
            pred->binary_right->kind == XPathExpr::Kind::LocationPath) {
            auto const& path = pred->binary_right->location_path;
            if (!path.absolute && path.steps.size() == 1 &&
                path.steps[0].axis == Axis::Attribute &&
                path.steps[0].predicates.empty() &&
                path.steps[0].node_test.kind == NodeTest::Kind::Name) {
                if (pred->binary_left &&
                    pred->binary_left->kind == XPathExpr::Kind::StringLiteral) {
                    return std::make_tuple(path.steps[0].node_test.name,
                                           pred->binary_left->string_literal, i);
                }
            }
        }
    }
    return std::nullopt;
}

Result<std::vector<XPathNode>>
apply_predicate(XmlIndex const& index,
                 std::vector<XPathNode> const& nodes,
                 XPathExpr const& pred) {
    // Fast path: contains(., 'literal') / starts-with(., 'literal') SIMD batch.
    if (pred.kind == XPathExpr::Kind::FunctionCall) {
        if (auto out = try_batch_string_predicate(index, nodes,
                                                  pred.function_name,
                                                  pred.function_args)) {
            return *out;
        }
    }

    switch (pred.kind) {
        case XPathExpr::Kind::NumberLiteral: {
            double const n = pred.number_literal;
            if (std::isnan(n) || std::isinf(n) || n < 1.0 ||
                n > static_cast<double>(nodes.size()) ||
                n != std::trunc(n)) {
                return std::vector<XPathNode>{};
            }
            std::size_t const pos = static_cast<std::size_t>(n);
            if (pos >= 1 && pos <= nodes.size()) {
                return std::vector<XPathNode>{nodes[pos - 1]};
            }
            return std::vector<XPathNode>{};
        }

        case XPathExpr::Kind::UnaryMinus: {
            if (nodes.empty()) return std::vector<XPathNode>{};
            if (!pred.unary_inner) return std::vector<XPathNode>{};
            auto val = eval_predicate_value(index, nodes[0], *pred.unary_inner,
                                            1, nodes.size());
            if (!val) return std::unexpected(val.error());
            double const n = -val->to_number();
            if (std::isnan(n) || std::isinf(n) || n < 1.0 ||
                n > static_cast<double>(nodes.size())) {
                return std::vector<XPathNode>{};
            }
            std::size_t const pos = static_cast<std::size_t>(std::round(n));
            if (pos >= 1 && pos <= nodes.size()) {
                return std::vector<XPathNode>{nodes[pos - 1]};
            }
            return std::vector<XPathNode>{};
        }

        case XPathExpr::Kind::BinaryOp: {
            if (!pred.binary_left || !pred.binary_right) {
                return std::vector<XPathNode>{};
            }
            BinaryOp const op = pred.binary_op;
            bool const is_arithmetic =
                op == BinaryOp::Add || op == BinaryOp::Sub ||
                op == BinaryOp::Mul || op == BinaryOp::Div ||
                op == BinaryOp::Mod;

            if (is_arithmetic) {
                // Arithmetic expression as predicate: evaluate to number,
                // compare against position().
                std::vector<XPathNode> result;
                for (std::size_t i = 0; i < nodes.size(); ++i) {
                    auto val = eval_predicate_value(index, nodes[i], pred,
                                                    i + 1, nodes.size());
                    if (!val) return std::unexpected(val.error());
                    if (val->kind() == XpathValue::Kind::Number) {
                        double const pos = static_cast<double>(i + 1);
                        if (pos == val->to_number()) result.push_back(nodes[i]);
                    } else if (val->is_truthy()) {
                        result.push_back(nodes[i]);
                    }
                }
                return result;
            }

            // Comparison / logical op.
            std::vector<XPathNode> result;
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                auto left_val =
                    eval_predicate_value(index, nodes[i], *pred.binary_left,
                                         i + 1, nodes.size());
                if (!left_val) return std::unexpected(left_val.error());
                auto right_val =
                    eval_predicate_value(index, nodes[i], *pred.binary_right,
                                         i + 1, nodes.size());
                if (!right_val) return std::unexpected(right_val.error());
                if (compare_values(*left_val, op, *right_val)) {
                    result.push_back(nodes[i]);
                }
            }
            return result;
        }

        case XPathExpr::Kind::FunctionCall: {
            std::vector<XPathNode> result;
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                auto val = eval_function(index, nodes[i], pred.function_name,
                                         pred.function_args, i + 1,
                                         nodes.size());
                if (!val) return std::unexpected(val.error());
                bool keep = false;
                if (val->kind() == XpathValue::Kind::Number) {
                    double const pos = static_cast<double>(i + 1);
                    keep = pos == val->to_number();
                } else {
                    keep = val->is_truthy();
                }
                if (keep) result.push_back(nodes[i]);
            }
            return result;
        }

        case XPathExpr::Kind::LocationPath: {
            // Location path as boolean: keep nodes where the path yields any.
            std::vector<XPathNode> result;
            for (auto const& node : nodes) {
                auto sub = evaluate_in_context(index, node, pred);
                if (!sub) return std::unexpected(sub.error());
                if (!sub->empty()) result.push_back(node);
            }
            return result;
        }

        default:
            // StringLiteral, Union, FilterPath, GlobalFilter — keep all.
            return nodes;
    }
}

}  // namespace simdxml