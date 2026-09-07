// XPath axis evaluation — all 13 axes as array operations on `XmlIndex`.
//
// Ported from Rust `xpath/eval.rs` axis implementations (lines 1686-2164):
// child, descendant, descendant-or-self, parent, ancestor, ancestor-or-self,
// following-sibling, preceding-sibling, following, preceding, self,
// attribute, namespace. Also `matches_node_test` and `eval_step`.
//
// Array-based traversal: each axis is a scan or lookup over the flat
// `XmlIndex` arrays (CSR offsets, parents, depths, pre/post ordering) —
// no DOM pointers. Predicates are applied per-context-node within `eval_step`
// so that `position()` is correct for `//p[1]` patterns.
#pragma once

#include "internal.hpp"

namespace simdxml {

/// Evaluate a single XPath axis for a single context node, returning the
/// candidate nodes BEFORE predicate filtering (no node-test filtering either,
/// except for Attribute/Namespace which pre-filter by test).
[[nodiscard]] std::vector<XPathNode>
eval_axis(XmlIndex const& index, XPathNode const& node, Axis axis,
          NodeTest const& test);

/// Evaluate a single `Step` against a context-set. Each context node's axis
/// candidates are filtered by the node test, then predicates are applied
/// PER CONTEXT NODE (so `position()` resets per parent). Multi-context results
/// are deduped and sorted in document order.
[[nodiscard]] Result<std::vector<XPathNode>>
eval_step(XmlIndex const& index,
          std::vector<XPathNode> const& context, Step const& step);

/// Returns true if `node` matches `test` under XPath 1.0 node-test rules.
/// Mirrors Rust `matches_node_test`. For attribute/namespace axes, name
/// matching is handled by `eval_attribute_axis` directly; this function
/// returns false for attribute-vs-element-name combinations.
[[nodiscard]] bool
matches_node_test(XmlIndex const& index, XPathNode const& node,
                  NodeTest const& test);

}  // namespace simdxml