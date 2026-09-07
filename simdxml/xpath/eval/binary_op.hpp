// XPath binary operator evaluation — =, !=, <, <=, >, >=, +, -, *, div, mod,
// and, or, |.
//
// Ported from Rust `xpath/eval.rs` `compare_values` and the `BinaryOp` arm of
// `eval_predicate_value`. XPath 1.0 §3.4 type coercion rules:
//   - If either operand is a boolean → convert other to boolean
//   - Else if either is a number     → convert other to number
//   - Else compare as strings
// Arithmetic operators coerce both operands to numbers.
//
// Node-set comparison semantics (§3.4) are handled in `eval_predicate_value`
// (in `eval.cpp`); this module provides the scalar comparison primitive plus
// the arithmetic dispatch used by both the predicate engine and the function
// call module.
#pragma once

#include "internal.hpp"

namespace simdxml {

/// Compare two `XpathValue`s under an XPath 1.0 binary operator.
/// Returns the boolean result. Mirrors Rust `compare_values`.
[[nodiscard]] bool
compare_values(XpathValue const& left, BinaryOp op,
               XpathValue const& right) noexcept;

/// Evaluate a binary operator whose operands have already been reduced to
/// scalar `XpathValue`s. Arithmetic ops return Number; comparison/logical
/// ops return Boolean. Mirrors the arithmetic arm of Rust `eval_predicate_value`.
[[nodiscard]] Result<XpathValue>
eval_binary_op_scalar(BinaryOp op, XpathValue const& left,
                      XpathValue const& right);

}  // namespace simdxml