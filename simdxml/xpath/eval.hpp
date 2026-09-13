// XPath 1.0 evaluation engine — public declarations.
//
// Ported from Rust `xpath/eval.rs`. The C++ port mirrors the Rust module's
// public surface: `evaluate`, `eval_text`, `extract_text`, `eval_xpath`,
// `eval_expr_with_doc`, `eval_expr_with_context`, `eval_standalone_expr`,
// `evaluate_from_context`. The evaluator works against `XmlIndex` using
// array-based axis traversal — no DOM pointers.
#pragma once

#include "ast.hpp"
#include "../error.hpp"
#include "../index/xml_index.hpp"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace rai::xml {

/// A node in the XPath result set.
///
/// Lightweight reference into the `XmlIndex` arrays — carries only an index
/// (and optionally a hash). Mirrors Rust `XPathNode`.
class XPathNode {
public:
    enum class Kind {
        Element,
        Text,
        Attribute,
        Namespace,
    };

    XPathNode() = default;
    static XPathNode element(std::size_t idx) {
        XPathNode n; n.kind_ = Kind::Element; n.idx_ = idx; return n;
    }
    static XPathNode text(std::size_t idx) {
        XPathNode n; n.kind_ = Kind::Text; n.idx_ = idx; return n;
    }
    static XPathNode attribute(std::size_t tag_idx, std::uint64_t hash) {
        XPathNode n; n.kind_ = Kind::Attribute; n.idx_ = tag_idx; n.hash_ = hash; return n;
    }
    static XPathNode ns(std::size_t tag_idx, std::uint64_t hash) {
        XPathNode n; n.kind_ = Kind::Namespace; n.idx_ = tag_idx; n.hash_ = hash; return n;
    }

    Kind kind() const noexcept { return kind_; }
    std::size_t index() const noexcept { return idx_; }
    std::uint64_t hash() const noexcept { return hash_; }
    bool is_element() const noexcept { return kind_ == Kind::Element; }
    bool is_text() const noexcept { return kind_ == Kind::Text; }
    bool is_attribute() const noexcept { return kind_ == Kind::Attribute; }
    bool is_namespace() const noexcept { return kind_ == Kind::Namespace; }

    bool operator==(XPathNode const&) const noexcept = default;

private:
    Kind kind_ = Kind::Element;
    std::size_t idx_ = 0;
    std::uint64_t hash_ = 0;
};

/// Sentinel index for the virtual document root.
constexpr std::size_t DOC_ROOT = static_cast<std::size_t>(-1);

/// Hash an attribute name for storage in `XPathNode` (FNV-1a 64-bit).
[[nodiscard]] std::uint64_t
attr_name_hash(std::string_view name) noexcept;

/// Result of standalone expression evaluation (no document context).
struct StandaloneResult {
    enum class Kind { Number, String, Boolean };
    Kind kind = Kind::String;
    double number = 0.0;
    std::string str;
    bool boolean = false;
};

/// Result of a top-level XPath evaluation — node set or scalar value.
class XPathResult {
public:
    enum class Kind { NodeSet, String, Number, Boolean };

    XPathResult() = default;
    static XPathResult node_set(std::vector<XPathNode> ns) {
        XPathResult r; r.kind_ = Kind::NodeSet; r.nodes_ = std::move(ns); return r;
    }
    static XPathResult string(std::string s) {
        XPathResult r; r.kind_ = Kind::String; r.str_ = std::move(s); return r;
    }
    static XPathResult number(double n) {
        XPathResult r; r.kind_ = Kind::Number; r.number_ = n; return r;
    }
    static XPathResult boolean(bool b) {
        XPathResult r; r.kind_ = Kind::Boolean; r.boolean_ = b; return r;
    }

    Kind kind() const noexcept { return kind_; }
    std::vector<XPathNode> const& nodes() const noexcept { return nodes_; }
    std::string const& as_string() const noexcept { return str_; }
    double as_number() const noexcept { return number_; }
    bool as_boolean() const noexcept { return boolean_; }

    /// Format as a display string, matching xmllint behavior.
    [[nodiscard]] std::string
    to_display_string(XmlIndex const& index) const;

private:
    Kind kind_ = Kind::NodeSet;
    std::vector<XPathNode> nodes_;
    std::string str_;
    double number_ = 0.0;
    bool boolean_ = false;
};

/// Evaluate an XPath expression against an `XmlIndex`, returning a node set.
[[nodiscard]] Result<std::vector<XPathNode>>
evaluate(XmlIndex const& index, XPathExpr const& expr);

/// Evaluate and return text content of matching nodes.
[[nodiscard]] Result<std::vector<std::string_view>>
eval_text(XmlIndex const& index, XPathExpr const& expr);

/// Extract text from pre-evaluated nodes.
[[nodiscard]] Result<std::vector<std::string_view>>
extract_text(XmlIndex const& index, std::vector<XPathNode> const& nodes);

/// Evaluate a top-level XPath expression, returning a node set or scalar.
[[nodiscard]] Result<XPathResult>
eval_xpath(XmlIndex const& index, XPathExpr const& expr);

/// Evaluate a predicate expression against the document root.
[[nodiscard]] Result<StandaloneResult>
eval_expr_with_doc(XmlIndex const& index, std::string_view expr_str);

/// Evaluate a predicate expression from a specific context node.
[[nodiscard]] Result<StandaloneResult>
eval_expr_with_context(XmlIndex const& index, std::string_view expr_str,
                       XPathNode context);

/// Evaluate a standalone expression (no document context needed).
[[nodiscard]] Result<StandaloneResult>
eval_standalone_expr(std::string_view expr_str);

/// Evaluate an expression from a specific context node (public API).
[[nodiscard]] Result<std::vector<XPathNode>>
evaluate_from_context(XmlIndex const& index, XPathExpr const& expr,
                      XPathNode context_node);

}  // namespace rai::xml