// Implementation of XPath binary operator evaluation.
//
// Ported from Rust `xpath/eval.rs` `compare_values` and the `BinaryOp` arm of
// `eval_predicate_value`. The scalar-only `eval_binary_op_scalar` is used by
// both the predicate engine and the function-call module; the node-set
// comparison semantics (XPath §3.4) live in `eval_predicate_value` (eval.cpp).
#include "binary_op.hpp"

#include <cmath>
#include <string>

namespace simdxml {

namespace {

bool
eq_scalar(XpathValue const& left, XpathValue const& right) noexcept {
    // XPath equality rules (§3.4):
    // - boolean operand → coerce other to boolean
    // - number operand  → coerce other to number, NaN-aware compare
    // - otherwise       → string compare
    if (left.kind() == XpathValue::Kind::Boolean) {
        return left.as_boolean() == right.is_truthy();
    }
    if (right.kind() == XpathValue::Kind::Boolean) {
        return left.is_truthy() == right.as_boolean();
    }
    if (left.kind() == XpathValue::Kind::Number ||
        right.kind() == XpathValue::Kind::Number) {
        double const ln = left.to_number();
        double const rn = right.to_number();
        if (std::isnan(ln) || std::isnan(rn)) return false;  // NaN != NaN
        return ln == rn;
    }
    return left.to_string() == right.to_string();
}

}  // namespace

bool
compare_values(XpathValue const& left, BinaryOp op,
               XpathValue const& right) noexcept {
    switch (op) {
        case BinaryOp::Eq:
            return eq_scalar(left, right);
        case BinaryOp::Neq:
            return !eq_scalar(left, right);
        case BinaryOp::Lt:
            return left.to_number() < right.to_number();
        case BinaryOp::Gt:
            return left.to_number() > right.to_number();
        case BinaryOp::Lte:
            return left.to_number() <= right.to_number();
        case BinaryOp::Gte:
            return left.to_number() >= right.to_number();
        case BinaryOp::Or:
            return left.is_truthy() || right.is_truthy();
        case BinaryOp::And:
            return left.is_truthy() && right.is_truthy();
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod:
            // Arithmetic ops don't produce booleans — callers should use
            // `eval_binary_op_scalar` instead. Defensively return false.
            return false;
    }
    return false;
}

Result<XpathValue>
eval_binary_op_scalar(BinaryOp op, XpathValue const& left,
                      XpathValue const& right) {
    switch (op) {
        case BinaryOp::Add:
            return XpathValue::number(left.to_number() + right.to_number());
        case BinaryOp::Sub:
            return XpathValue::number(left.to_number() - right.to_number());
        case BinaryOp::Mul:
            return XpathValue::number(left.to_number() * right.to_number());
        case BinaryOp::Div: {
            double const rn = right.to_number();
            return XpathValue::number(left.to_number() / rn);
        }
        case BinaryOp::Mod: {
            double const rn = right.to_number();
            return XpathValue::number(std::fmod(left.to_number(), rn));
        }
        case BinaryOp::Eq:
        case BinaryOp::Neq:
        case BinaryOp::Lt:
        case BinaryOp::Gt:
        case BinaryOp::Lte:
        case BinaryOp::Gte:
        case BinaryOp::Or:
        case BinaryOp::And:
            return XpathValue::boolean(compare_values(left, op, right));
    }
    return std::unexpected(SimdXmlError::xpath_eval_error(
        "Unknown binary operator"));
}

}  // namespace simdxml