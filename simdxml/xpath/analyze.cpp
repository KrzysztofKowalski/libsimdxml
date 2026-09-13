// Implementation of XPath selectivity analysis.
//
// Ported from Rust `xpath/analyze.rs`. Walks the AST and collects tag names
// referenced via `NodeTest::Name` / `NodeTest::NamespacedName`. Returns
// `SelectivityHint::NeedsAll` if the query uses `*` (Wildcard) or `node()`
// on a non-descendant-or-self axis — those patterns require the full
// structural index.
#include "analyze.hpp"

#include <string>
#include <unordered_set>

namespace rai::xml {

namespace {

void collect_names(XPathExpr const& expr,
                    std::unordered_set<std::string>& names,
                    bool& needs_all);

void collect_from_steps(std::vector<Step> const& steps,
                         std::unordered_set<std::string>& names,
                         bool& needs_all) {
    for (Step const& step : steps) {
        switch (step.node_test.kind) {
            case NodeTest::Kind::Name:
                names.insert(step.node_test.name);
                break;
            case NodeTest::Kind::NamespacedName:
                names.insert(step.node_test.local);
                break;
            case NodeTest::Kind::Wildcard:
                needs_all = true;
                break;
            case NodeTest::Kind::Node:
                // `//` (desc-or-self::node()) is structural — doesn't force
                // NeedsAll. `self::node()` (abbreviated `.`) likewise.
                if (step.axis != Axis::DescendantOrSelf
                    && step.axis != Axis::SelfAxis) {
                    needs_all = true;
                }
                break;
            case NodeTest::Kind::Text:
            case NodeTest::Kind::Comment:
            case NodeTest::Kind::PI:
            case NodeTest::Kind::PIName:
                // text()/comment()/PI don't filter by tag name and don't
                // force NeedsAll — they match non-element nodes.
                break;
        }
        for (auto const& pred : step.predicates) {
            collect_names(*pred, names, needs_all);
        }
    }
}

void collect_names(XPathExpr const& expr,
                   std::unordered_set<std::string>& names,
                   bool& needs_all) {
    switch (expr.kind) {
        case XPathExpr::Kind::LocationPath:
            collect_from_steps(expr.location_path.steps, names, needs_all);
            break;
        case XPathExpr::Kind::Union:
            for (auto const& m : expr.union_members) collect_names(*m, names, needs_all);
            break;
        case XPathExpr::Kind::FilterPath:
            collect_names(*expr.filter_inner, names, needs_all);
            collect_from_steps(expr.filter_steps, names, needs_all);
            break;
        case XPathExpr::Kind::GlobalFilter:
            collect_names(*expr.global_filter_inner, names, needs_all);
            for (auto const& p : expr.global_filter_preds) collect_names(*p, names, needs_all);
            break;
        case XPathExpr::Kind::BinaryOp:
            collect_names(*expr.binary_left, names, needs_all);
            collect_names(*expr.binary_right, names, needs_all);
            break;
        case XPathExpr::Kind::FunctionCall:
            for (auto const& a : expr.function_args) collect_names(*a, names, needs_all);
            break;
        case XPathExpr::Kind::UnaryMinus:
            collect_names(*expr.unary_inner, names, needs_all);
            break;
        case XPathExpr::Kind::StringLiteral:
        case XPathExpr::Kind::NumberLiteral:
            // Literals don't reference tags.
            break;
    }
}

}  // namespace

SelectivityHint selectivity(XPathExpr const& expr) {
    std::unordered_set<std::string> names;
    bool needs_all = false;
    collect_names(expr, names, needs_all);
    if (needs_all) return SelectivityHint::for_needs_all();
    return SelectivityHint::selective(std::move(names));
}

}  // namespace rai::xml