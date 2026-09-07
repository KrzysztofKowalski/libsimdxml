// XPath 1.0 parser — hand-rolled recursive descent / Pratt precedence.
//
// Ported from Rust `xpath/parser.rs` (nom-based). The C++ port replaces the
// nom combinator library with a hand-rolled lexer + Pratt-style precedence
// parser. The grammar coverage matches the Rust parser exactly:
//
//   - Absolute and relative location paths (`/a/b`, `a/b`)
//   - Abbreviated syntax (`//`, `..`, `@`, `.`)
//   - All 13 axes (`child::`, `descendant-or-self::`, ...)
//   - Node tests (`name`, `*`, `text()`, `node()`, `comment()`,
//                  `processing-instruction()`, `processing-instruction('n')`)
//   - Predicates with arbitrary nesting (`[position()=1]`, `[@attr='val']`)
//   - Operators (`and`, `or`, `=`, `!=`, `<`, `>`, `<=`, `>=`, `+`, `-`,
//                 `*`, `div`, `mod`)
//   - Function calls (`contains()`, `starts-with()`, `count()`, ...)
//   - Union expressions (`a | b`)
//
// Entry points: `parse_xpath` for full expressions, `parse_xpath_predicate_expr`
// for standalone predicate/arithmetic expressions.
#pragma once

#include "ast.hpp"
#include "../error.hpp"

#include <string>
#include <string_view>

namespace simdxml {

/// Parse an XPath 1.0 expression string into an `XPathExpr` AST.
[[nodiscard]] Result<XPathExpr>
parse_xpath(std::string_view input);

/// Parse a standalone predicate expression (arithmetic, string, boolean).
[[nodiscard]] Result<XPathExpr>
parse_xpath_predicate_expr(std::string_view input);

}  // namespace simdxml