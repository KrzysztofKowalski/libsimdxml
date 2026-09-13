// XPath 1.0 core function library.
//
// Ported from Rust `xpath/eval.rs` `eval_function` (lines 1039-1401) plus
// `resolve_namespace_uri` and `eval_id_function`. Coverage:
//
//   - String:    concat, starts-with, contains, substring-before,
//                substring-after, substring, string-length, normalize-space,
//                translate
//   - Number:    sum, floor, ceiling, round, number
//   - Boolean:   true, false, boolean, not, lang
//   - Node-set:  last, position, count, id, local-name, namespace-uri, name,
//                string
//
// Function arity is checked where the spec mandates it; extra arguments are
// ignored (matching Rust behavior). Position/size context is forwarded by
// the predicate engine.
#pragma once

#include "internal.hpp"

namespace rai::xml {

/// Evaluate an XPath 1.0 core function call. Returns the result value.
/// Mirrors Rust `eval_function`.
[[nodiscard]] Result<XpathValue>
eval_function(XmlIndex const& index, XPathNode const& node,
              std::string_view name,
              std::vector<std::unique_ptr<XPathExpr>> const& args,
              std::size_t position, std::size_t size);

/// Evaluate `id('value')` — find element with matching `id` attribute.
/// Returns a node set. Mirrors Rust `eval_id_function`.
[[nodiscard]] Result<std::vector<XPathNode>>
eval_id_function(XmlIndex const& index,
                 std::vector<std::unique_ptr<XPathExpr>> const& args);

}  // namespace rai::xml