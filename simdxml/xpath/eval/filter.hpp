// XPath predicate filtering — `[...]` expressions.
//
// Ported from Rust `xpath/eval.rs` `apply_predicate` (lines 736-875) plus
// the SIMD batch fast paths `try_batch_attr_predicate` and
// `try_batch_string_predicate` and the `extract_simple_attr_eq` helper.
//
// XPath 1.0 §3.4 predicate semantics:
//   - If the predicate evaluates to a number, it is a positional predicate
//     (`[1]` means `position() = 1`).
//   - Otherwise the predicate is boolean — keep nodes where it is truthy.
//   - Predicates are applied per-context-node so `position()` resets per
//     parent (handled in `axis_step.cpp`'s `eval_step`).
//
// Fast paths:
//   - `[@attr='literal']`  — inline attribute scan (Rust keeps the memmem path
//     disabled; the fused scan in `path_expr.cpp` handles the common case).
//   - `contains(., 'lit')` / `starts-with(., 'lit')` — SIMD batched eval
//     via `simd_pred` module when candidate count exceeds the threshold.
#pragma once

#include "internal.hpp"

namespace rai::xml {

/// Apply a predicate to filter a node set.
/// Mirrors Rust `apply_predicate`.
[[nodiscard]] Result<std::vector<XPathNode>>
apply_predicate(XmlIndex const& index,
                 std::vector<XPathNode> const& nodes,
                 XPathExpr const& pred);

/// Detect a simple `@attr='literal'` (or `'literal' = @attr`) predicate.
/// Returns `(attr_name, value, predicate_index)` if found. Mirrors Rust
/// `extract_simple_attr_eq`.
[[nodiscard]] std::optional<
    std::tuple<std::string, std::string, std::size_t>>
extract_simple_attr_eq(std::vector<std::unique_ptr<XPathExpr>> const& preds);

}  // namespace rai::xml