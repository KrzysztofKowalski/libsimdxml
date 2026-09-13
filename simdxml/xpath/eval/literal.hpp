// XPath literal evaluation — string, number, boolean literals.
//
// Ported from Rust `xpath/eval.rs` `eval_predicate_value` literal arms
// (StringLiteral, NumberLiteral). The C++ port isolates these in a separate
// module so the coordinator and other modules can delegate to them without
// pulling in the function-call / binary-op machinery.
#pragma once

#include "internal.hpp"

#include <string>

namespace rai::xml {

/// Evaluate a string literal to an `XpathValue`.
[[nodiscard]] inline XpathValue
eval_string_literal(std::string_view literal) {
    return XpathValue::string(std::string(literal));
}

/// Evaluate a numeric literal to an `XpathValue`.
[[nodiscard]] inline XpathValue
eval_number_literal(double literal) noexcept {
    return XpathValue::number(literal);
}

/// Evaluate a boolean literal to an `XpathValue`.
[[nodiscard]] inline XpathValue
eval_boolean_literal(bool literal) noexcept {
    return XpathValue::boolean(literal);
}

}  // namespace rai::xml