// XPath path-expression evaluation — `/a/b`, `//x`, `a | b`, filter paths.
//
// Ported from Rust `xpath/eval.rs`:
//   - `eval_location_path` (lines 358-397)
//   - `eval_fused_descendant_child` (lines 399-537)
//   - `eval_fused_descendant_child_with_preds` (lines 539-653)
//   - `evaluate_in_context` (lines 1582-1629)
//   - `evaluate_from_context` (lines 1522-1580)
//
// The fused descendant-or-self::node()/child::name pattern is the most common
// XPath idiom (`//name`); fusing it into a single scan avoids materializing
// the intermediate descendant-or-self mega-nodeset. When the next step has
// predicates, the fused scan groups matches by parent and applies predicates
// per group so `//p[1]` correctly means "first p child of each parent".
#pragma once

#include "internal.hpp"

namespace rai::xml {

/// Evaluate a relative or absolute location path from the document root
/// context. Mirrors Rust `eval_location_path`.
[[nodiscard]] Result<std::vector<XPathNode>>
eval_location_path(XmlIndex const& index, LocationPath const& path);

/// Fused descendant-or-self::node()/child::test — single scan over all tags.
/// Used by `//name` patterns. Mirrors Rust `eval_fused_descendant_child`.
[[nodiscard]] Result<std::vector<XPathNode>>
eval_fused_descendant_child(XmlIndex const& index,
                            std::vector<XPathNode> const& context,
                            Step const& child_step);

/// Fused descendant scan WITH per-parent predicate application.
/// Mirrors Rust `eval_fused_descendant_child_with_preds`.
[[nodiscard]] Result<std::vector<XPathNode>>
eval_fused_descendant_child_with_preds(XmlIndex const& index,
                                       std::vector<XPathNode> const& context,
                                       Step const& child_step);

}  // namespace rai::xml