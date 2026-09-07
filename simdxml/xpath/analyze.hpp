// Selectivity analysis of XPath expressions for query-driven lazy parsing.
//
// Ported from Rust `xpath/analyze.rs`. Walks the `XPathExpr` AST to collect
// the set of tag names referenced by the query. When the query only
// references specific names (e.g. `//claim/text()`), the lazy parser can
// skip index construction for irrelevant tags.
#pragma once

#include "ast.hpp"

#include <string>
#include <unordered_set>

namespace simdxml {

/// Result of analyzing an XPath expression for selective parsing.
struct SelectivityHint {
    enum class Kind {
        /// The query references only these specific tag names.
        Selective,
        /// The query uses wildcards, `node()`, or patterns requiring all tags.
        NeedsAll,
    };

    Kind kind = Kind::NeedsAll;
    std::unordered_set<std::string> names;

    static SelectivityHint selective(std::unordered_set<std::string> names) {
        SelectivityHint h;
        h.kind = Kind::Selective;
        h.names = std::move(names);
        return h;
    }
    static SelectivityHint for_needs_all() {
        SelectivityHint h;
        h.kind = Kind::NeedsAll;
        return h;
    }

    bool needs_all() const noexcept { return kind == Kind::NeedsAll; }
};

/// Analyze an XPath expression and return the set of tag names it could match.
[[nodiscard]] SelectivityHint
selectivity(XPathExpr const& expr);

}  // namespace simdxml