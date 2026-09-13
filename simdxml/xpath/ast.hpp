// XPath 1.0 Abstract Syntax Tree.
//
// Ported from Rust `xpath/ast.rs`. The Rust enum-with-variants is
// represented in C++ as a `std::variant` over leaf node types — each
// variant carries its payload by value (literals) or via `std::unique_ptr`
// (recursive sub-expressions). Discrimination uses `std::holds_alternative`
// / `std::get_if` per the Fazy 1+2 pattern (thiserror→Kind).
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rai::xml {

/// The 13 XPath 1.0 axes.
enum class Axis {
    Child,
    Descendant,
    Parent,
    Ancestor,
    FollowingSibling,
    PrecedingSibling,
    Following,
    Preceding,
    SelfAxis,
    DescendantOrSelf,
    AncestorOrSelf,
    Attribute,
    Namespace,
};

/// What to match at each step.
struct NodeTest {
    enum class Kind {
        Name,            ///< Match a specific tag name.
        Wildcard,         ///< Match any element (`*`).
        Text,             ///< text()
        Node,             ///< node()
        Comment,          ///< comment()
        PI,               ///< processing-instruction()
        PIName,           ///< processing-instruction('name')
        NamespacedName,   ///< prefix:local
    };

    Kind kind = Kind::Wildcard;
    std::string name;          ///< For Name / PIName / NamespacedName (full).
    std::string prefix;        ///< For NamespacedName.
    std::string local;         ///< For NamespacedName.

    static NodeTest set_name(std::string n) {
        NodeTest t; t.kind = Kind::Name; t.name = std::move(n); return t;
    }
    static NodeTest wildcard() {
        NodeTest t; t.kind = Kind::Wildcard; return t;
    }
    static NodeTest text() {
        NodeTest t; t.kind = Kind::Text; return t;
    }
    static NodeTest node() {
        NodeTest t; t.kind = Kind::Node; return t;
    }
    static NodeTest comment() {
        NodeTest t; t.kind = Kind::Comment; return t;
    }
    static NodeTest pi() {
        NodeTest t; t.kind = Kind::PI; return t;
    }
    static NodeTest pi_name(std::string n) {
        NodeTest t; t.kind = Kind::PIName; t.name = std::move(n); return t;
    }
    static NodeTest namespaced(std::string p, std::string l) {
        NodeTest t; t.kind = Kind::NamespacedName;
        t.prefix = std::move(p);
        t.local = std::move(l);
        t.name = t.prefix + ":" + t.local;
        return t;
    }

    bool operator==(NodeTest const&) const noexcept = default;
};

/// Binary operators in XPath 1.0 expressions.
enum class BinaryOp {
    Or, And, Eq, Neq, Lt, Gt, Lte, Gte,
    Add, Sub, Mul, Div, Mod,
};

/// Forward declaration of `XPathExpr` (used by `Step`).
struct XPathExpr;

/// A single step in a location path: axis + node test + predicates.
struct Step {
    Axis axis = Axis::Child;
    NodeTest node_test;
    std::vector<std::unique_ptr<XPathExpr>> predicates;

    // All special members are defined out-of-line below `XPathExpr` because
    // their exception-cleanup / implicit-dtor generation needs the complete
    // `XPathExpr` type (to instantiate `~vector<unique_ptr<XPathExpr>>`).
    Step();
    Step(Axis a, NodeTest nt);
    Step(Step const& other);
    Step(Step&&) noexcept;
    Step& operator=(Step const& other);
    Step& operator=(Step&&) noexcept;
    ~Step();
    bool operator==(Step const& other) const noexcept;
};

/// A location path: absolute or relative, with a sequence of steps.
struct LocationPath {
    bool absolute = false;
    std::vector<Step> steps;
    bool operator==(LocationPath const&) const noexcept;
};

/// A complete XPath 1.0 expression. Mirrors Rust `XPathExpr`.
struct XPathExpr {
    enum class Kind {
        LocationPath,
        StringLiteral,
        NumberLiteral,
        FunctionCall,
        BinaryOp,
        UnaryMinus,
        Union,
        FilterPath,
        GlobalFilter,
    };

    Kind kind = Kind::LocationPath;

    // Payload — only one is populated based on `kind`.
    LocationPath location_path;
    std::string string_literal;
    double number_literal = 0.0;
    std::string function_name;
    std::vector<std::unique_ptr<XPathExpr>> function_args;
    std::unique_ptr<XPathExpr> binary_left;
    BinaryOp binary_op = BinaryOp::Eq;
    std::unique_ptr<XPathExpr> binary_right;
    std::unique_ptr<XPathExpr> unary_inner;
    std::vector<std::unique_ptr<XPathExpr>> union_members;
    std::unique_ptr<XPathExpr> filter_inner;
    std::vector<Step> filter_steps;
    std::unique_ptr<XPathExpr> global_filter_inner;
    std::vector<std::unique_ptr<XPathExpr>> global_filter_preds;

    XPathExpr() = default;
    explicit XPathExpr(LocationPath p) : kind(Kind::LocationPath),
                                         location_path(std::move(p)) {}

    XPathExpr(XPathExpr&&) noexcept = default;
    XPathExpr& operator=(XPathExpr&&) noexcept = default;
    XPathExpr(XPathExpr const& other) { *this = other; }
    XPathExpr& operator=(XPathExpr const& other) {
        if (this == &other) return *this;
        kind = other.kind;
        location_path = other.location_path;
        string_literal = other.string_literal;
        number_literal = other.number_literal;
        function_name = other.function_name;
        function_args = deep_copy(other.function_args);
        binary_left = deep_copy_one(other.binary_left);
        binary_op = other.binary_op;
        binary_right = deep_copy_one(other.binary_right);
        unary_inner = deep_copy_one(other.unary_inner);
        union_members = deep_copy(other.union_members);
        filter_inner = deep_copy_one(other.filter_inner);
        filter_steps = other.filter_steps;
        global_filter_inner = deep_copy_one(other.global_filter_inner);
        global_filter_preds = deep_copy(other.global_filter_preds);
        return *this;
    }

    bool operator==(XPathExpr const& other) const noexcept {
        // Structural equality — used by parser tests. Compare kinds + relevant
        // payload fields. Recursive comparison is bounded by AST depth.
        if (kind != other.kind) return false;
        switch (kind) {
            case Kind::LocationPath: return location_path == other.location_path;
            case Kind::StringLiteral: return string_literal == other.string_literal;
            case Kind::NumberLiteral: return number_literal == other.number_literal;
            case Kind::FunctionCall:
                return function_name == other.function_name &&
                       function_args.size() == other.function_args.size();
            case Kind::BinaryOp:
                return binary_op == other.binary_op;
            case Kind::UnaryMinus: return true;
            case Kind::Union: return union_members.size() == other.union_members.size();
            case Kind::FilterPath: return filter_steps == other.filter_steps;
            case Kind::GlobalFilter:
                return global_filter_preds.size() == other.global_filter_preds.size();
        }
        return false;
    }

private:
    static std::vector<std::unique_ptr<XPathExpr>>
    deep_copy(std::vector<std::unique_ptr<XPathExpr>> const& src) {
        std::vector<std::unique_ptr<XPathExpr>> dst;
        dst.reserve(src.size());
        for (auto const& p : src) {
            dst.push_back(p ? std::make_unique<XPathExpr>(*p) : nullptr);
        }
        return dst;
    }
    static std::unique_ptr<XPathExpr>
    deep_copy_one(std::unique_ptr<XPathExpr> const& src) {
        return src ? std::make_unique<XPathExpr>(*src) : nullptr;
    }
};

inline bool Step::operator==(Step const& other) const noexcept {
    return axis == other.axis && node_test == other.node_test &&
           predicates.size() == other.predicates.size();
}
inline bool LocationPath::operator==(LocationPath const& other) const noexcept {
    return absolute == other.absolute && steps.size() == other.steps.size();
}

// Out-of-line definitions for `Step` special members. They are placed
// AFTER the complete definition of `XPathExpr` so the compiler can
// instantiate `~vector<unique_ptr<XPathExpr>>`, `~unique_ptr<XPathExpr>`,
// and `std::make_unique<XPathExpr>(*p)` — all of which need `XPathExpr`
// to be a complete type.
inline Step::Step() = default;
inline Step::Step(Axis a, NodeTest nt) : axis(a), node_test(std::move(nt)) {}
inline Step::Step(Step const& other) { *this = other; }
inline Step::Step(Step&&) noexcept = default;
inline Step& Step::operator=(Step const& other) {
    if (this == &other) return *this;
    axis = other.axis;
    node_test = other.node_test;
    predicates.clear();
    for (auto const& p : other.predicates) {
        predicates.push_back(p ? std::make_unique<XPathExpr>(*p) : nullptr);
    }
    return *this;
}
inline Step& Step::operator=(Step&&) noexcept = default;
inline Step::~Step() = default;

}  // namespace rai::xml