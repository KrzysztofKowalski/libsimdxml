// XPath evaluation — shared internal declarations.
//
// This header is internal to the `xpath/eval/` modules and is NOT part of the
// public API. It declares the polymorphic `XpathValue` (string/number/boolean)
// used during predicate evaluation, plus shared helpers used by multiple
// modules: number formatting (libxml2-compatible), node string value, node
// dedup/sort, and the `EvalContext` bundle carried through evaluation.
//
// Ported from Rust `xpath/eval.rs` (internal `XPathValue` enum, `xpath_format_number`,
// `node_string_value`, `dedup_nodes`, `sort_doc_order`, `node_doc_pos`).
#pragma once

#include "xpath/ast.hpp"
#include "error.hpp"
#include "index/xml_index.hpp"
#include "xpath/eval.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rai::xml {

/// Polymorphic XPath value during evaluation.
///
/// Mirrors Rust internal `XPathValue` enum. The C++ port uses a tagged class
/// rather than `std::variant` because the conversion helpers (`as_string`,
/// `as_number`, `is_truthy`) are non-trivial and a tagged class keeps the
/// dispatch centralized.
class XpathValue {
public:
    enum class Kind { String, Number, Boolean };

    XpathValue() = default;
    static XpathValue string(std::string s) {
        XpathValue v; v.kind_ = Kind::String; v.str_ = std::move(s); return v;
    }
    static XpathValue number(double n) {
        XpathValue v; v.kind_ = Kind::Number; v.num_ = n; return v;
    }
    static XpathValue boolean(bool b) {
        XpathValue v; v.kind_ = Kind::Boolean; v.bool_ = b; return v;
    }

    Kind kind() const noexcept { return kind_; }
    std::string const& as_string_owned() const noexcept { return str_; }
    double as_number() const noexcept { return num_; }
    bool as_boolean() const noexcept { return bool_; }

    /// XPath truthiness rules (sec 2.4.3):
    /// - boolean → its value
    /// - string  → false iff empty
    /// - number  → false iff NaN or 0
    [[nodiscard]] bool is_truthy() const noexcept;

    /// Convert to string per XPath 1.0 coercion rules:
    /// - string  → itself
    /// - number  → libxml2-format (%0.15g equivalent)
    /// - boolean → "true" / "false"
    [[nodiscard]] std::string to_string() const;

    /// Convert to number per XPath 1.0 coercion rules:
    /// - number  → itself
    /// - string  → trimmed parse, NaN on failure
    /// - boolean → 1.0 / 0.0
    [[nodiscard]] double to_number() const noexcept;

private:
    Kind kind_ = Kind::Boolean;
    std::string str_;
    double num_ = 0.0;
    bool bool_ = false;
};

/// Format a number matching libxml2's `xmlXPathFormatNumber` (%0.15g).
/// Mirrors Rust `xpath_format_number`.
[[nodiscard]] std::string
xpath_format_number(double n);

/// Tag types that count as "node" elements for axis traversal
/// (Open, SelfClose, Comment, PI). CData content lives in text ranges.
[[nodiscard]] inline bool is_node_tag(TagType tt) noexcept {
    return tt == TagType::Open || tt == TagType::SelfClose ||
           tt == TagType::Comment || tt == TagType::PI;
}

/// String value of a node (XPath §3.2 spec).
/// Mirrors Rust `node_string_value`.
[[nodiscard]] std::string
node_string_value(XmlIndex const& index, XPathNode const& node);

/// Resolve a namespace URI by walking ancestors looking for `xmlns:prefix`
/// (or `xmlns` for the default namespace). Returns std::nullopt if not declared.
[[nodiscard]] std::optional<std::string>
resolve_namespace_uri(XmlIndex const& index, std::size_t start_idx,
                      std::optional<std::string_view> prefix);

/// Remove duplicate nodes from a vector (preserving first occurrence).
/// Mirrors Rust `dedup_nodes`.
void dedup_nodes(std::vector<XPathNode>& nodes);

/// Sort nodes in document order. Mirrors Rust `sort_doc_order`.
void sort_doc_order(XmlIndex const& index, std::vector<XPathNode>& nodes);

/// Document-order position key for a node (mirrors Rust `node_doc_pos`).
[[nodiscard]] std::uint64_t
node_doc_pos(XmlIndex const& index, XPathNode const& node) noexcept;

/// Evaluation context bundle: index + context node + position/size.
struct EvalContext {
    XmlIndex const& index;
    XPathNode context_node;
    std::size_t position;
    std::size_t size;
};

/// Evaluate a predicate sub-expression to a value (forward decl — implemented
/// in `eval.cpp` coordinator since it dispatches across all modules).
[[nodiscard]] Result<XpathValue>
eval_predicate_value(XmlIndex const& index, XPathNode const& node,
                     XPathExpr const& expr, std::size_t position,
                     std::size_t size);

/// Evaluate an expression in the context of a specific node, returning
/// a node set (forward decl — implemented in `path_expr.cpp`).
[[nodiscard]] Result<std::vector<XPathNode>>
evaluate_in_context(XmlIndex const& index, XPathNode const& context_node,
                    XPathExpr const& expr);

/// Evaluate an expression from a context node — public-API variant used by
/// `evaluate_from_context` (forward decl — implemented in `path_expr.cpp`).
[[nodiscard]] Result<std::vector<XPathNode>>
evaluate_from_context_impl(XmlIndex const& index, XPathExpr const& expr,
                           XPathNode const& context_node);

}  // namespace rai::xml