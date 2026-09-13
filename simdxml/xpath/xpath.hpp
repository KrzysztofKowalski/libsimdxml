// XPath 1.0 public API — compiled expressions and convenience helpers.
//
// Ported from Rust `xpath/mod.rs`. Mirrors the `CompiledXPath` struct:
//   - `CompiledXPath::compile(string)` → parse once
//   - `eval(index)`                     → re-evaluate against any document
//   - `eval_text(index)`                → text content of matching nodes
//   - `interesting_names()`             → selectivity hint for lazy parsing
//
// Also re-exports the parser + evaluator entry points.
#pragma once

#include "analyze.hpp"
#include "ast.hpp"
#include "eval.hpp"
#include "parser.hpp"

#include "../error.hpp"
#include "../index/xml_index.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace rai::xml {

/// Compiled XPath expression — reusable across documents.
///
/// Compile once, evaluate many times. Avoids re-parsing the expression string
/// on each call. Use this for batch processing or repeated queries.
class CompiledXPath {
public:
    CompiledXPath() = default;

    /// Compile an XPath expression. Returns an error on parse failure.
    [[nodiscard]] static Result<CompiledXPath>
    compile(std::string_view expr_str) {
        auto expr = parse_xpath(expr_str);
        if (!expr) return std::unexpected(expr.error());
        CompiledXPath c;
        c.expr_ = std::move(*expr);
        return c;
    }

    /// Evaluate and return matching nodes.
    [[nodiscard]] Result<std::vector<XPathNode>>
    eval(XmlIndex const& index) const {
        return evaluate(index, expr_);
    }

    /// Evaluate and return text content of matching nodes.
    [[nodiscard]] Result<std::vector<std::string_view>>
    eval_text(XmlIndex const& index) const {
        return eval_text(index, expr_);
    }

    /// Analyze this expression for query-driven lazy parsing.
    /// Returns the set of tag names referenced, or `std::nullopt` if the
    /// query uses wildcards/`node()` and requires all tags.
    [[nodiscard]] std::optional<std::unordered_set<std::string>>
    interesting_names() const {
        auto hint = selectivity(expr_);
        if (hint.needs_all()) return std::nullopt;
        return std::move(hint.names);
    }

    /// Access the underlying parsed expression.
    [[nodiscard]] XPathExpr const& expr() const noexcept { return expr_; }

private:
    XPathExpr expr_;
};

}  // namespace rai::xml