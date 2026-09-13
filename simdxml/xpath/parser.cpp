// Implementation of the XPath 1.0 parser.
//
// Ported from Rust `xpath/parser.rs`. The Rust parser uses the `nom`
// combinator library; the C++ port is a hand-rolled recursive descent /
// Pratt-style precedence parser. Grammar coverage matches the Rust parser.
//
// Operator precedence (lowest to highest):
//   or < and < equality (= !=) < relational (< > <= >=)
//         < additive (+ -) < multiplicative (* div mod) < unary (-)
//
// The parser is organised as a `Parser` struct that holds the input string
// and a current position. Each `parse_*` method returns `Result<T>` and
// advances `pos_` on success. On failure, `pos_` is left at the offending
// position (no backtracking beyond what is needed for `alt`-style choices).
#include "parser.hpp"

#include "../error.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rai::xml {

namespace {

inline bool is_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
inline bool is_name_start(char c) noexcept {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
inline bool is_name_char(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
}
inline bool is_axis_char(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
}

struct Parser {
    std::string_view src;
    std::size_t pos = 0;

    explicit Parser(std::string_view s) : src(s) {}

    /// Skip whitespace.
    void skip_ws() noexcept {
        while (pos < src.size() && is_ws(src[pos])) ++pos;
    }
    /// Peek the next non-whitespace character (does NOT skip).
    char peek() const noexcept {
        return (pos < src.size()) ? src[pos] : '\0';
    }
    /// Peek the character `offset` positions ahead (no skip).
    char peek_at(std::size_t offset) const noexcept {
        return (pos + offset < src.size()) ? src[pos + offset] : '\0';
    }
    /// Check if the input starts with `literal` at the current position.
    bool starts_with(std::string_view literal) const noexcept {
        if (pos + literal.size() > src.size()) return false;
        return src.substr(pos, literal.size()) == literal;
    }
    /// Consume `literal` and advance pos. Caller must `starts_with` first.
    void consume(std::size_t n) noexcept { pos += n; }

    /// Build an XPathParseError at the current position.
    SimdXmlError error(std::string msg) const {
        return SimdXmlError::xpath_parse_error(std::move(msg));
    }

    /// Skip whitespace then expect `expected` char.
    Result<void> expect(char expected) {
        skip_ws();
        if (pos >= src.size() || src[pos] != expected) {
            return std::unexpected(error(std::string("expected '") + expected + "'"));
        }
        ++pos;
        return {};
    }

    /// Parse a name (NCName or QName).
    std::optional<std::string> parse_name() {
        skip_ws();
        std::size_t start = pos;
        // Allow leading letter or underscore
        if (pos >= src.size() || !is_name_start(src[pos])) {
            // Allow leading digit for names like `h2`? XPath NCName disallows this,
            // but be lenient — caller decides.
            return std::nullopt;
        }
        while (pos < src.size() && (is_name_char(src[pos]) || src[pos] == ':')) {
            ++pos;
        }
        if (pos == start) return std::nullopt;
        return std::string(src.substr(start, pos - start));
    }

    /// Parse a name consisting of alphanumeric/hyphen chars (used for axis
    /// names and function names like `processing-instruction`).
    std::optional<std::string> parse_axis_name() {
        skip_ws();
        std::size_t start = pos;
        while (pos < src.size() && is_axis_char(src[pos])) ++pos;
        if (pos == start) return std::nullopt;
        return std::string(src.substr(start, pos - start));
    }

    /// Parse a quoted string literal — `'...'` or `"..."`.
    std::optional<std::string> parse_string_literal() {
        skip_ws();
        if (pos >= src.size()) return std::nullopt;
        char quote = src[pos];
        if (quote != '\'' && quote != '"') return std::nullopt;
        ++pos;
        std::size_t start = pos;
        while (pos < src.size() && src[pos] != quote) ++pos;
        if (pos >= src.size()) return std::nullopt;  // unterminated
        std::string s(src.substr(start, pos - start));
        ++pos;  // consume closing quote
        return s;
    }
};

/// Forward declarations of recursive descent entry points.
Result<XPathExpr> parse_xpath_expr(Parser& p);
Result<XPathExpr> parse_predicate_expr(Parser& p);
Result<XPathExpr> parse_or_expr(Parser& p);
Result<XPathExpr> parse_and_expr(Parser& p);
Result<XPathExpr> parse_equality_expr(Parser& p);
Result<XPathExpr> parse_relational_expr(Parser& p);
Result<XPathExpr> parse_additive_expr(Parser& p);
Result<XPathExpr> parse_multiplicative_expr(Parser& p);
Result<XPathExpr> parse_unary_expr(Parser& p);
Result<XPathExpr> parse_primary_expr_inner(Parser& p);
Result<XPathExpr> parse_union_path_expr(Parser& p);
Result<XPathExpr> parse_primary_expr(Parser& p);
Result<XPathExpr> parse_function_call_expr(Parser& p);
Result<XPathExpr> parse_location_path(Parser& p);
Result<Step> parse_step(Parser& p);
Result<NodeTest> parse_node_test(Parser& p);
Result<std::vector<std::unique_ptr<XPathExpr>>> parse_predicates(Parser& p);
Result<std::vector<Step>> parse_continuation_steps(Parser& p);

/// `axis::nodetest[preds]` or abbreviated `@nodetest[preds]` or `..` or `.`.
Result<Step> parse_step(Parser& p) {
    p.skip_ws();
    // `..` — parent
    if (p.starts_with("..")) {
        p.consume(2);
        Step s;
        s.axis = Axis::Parent;
        s.node_test = NodeTest::node();
        return s;
    }
    // `.` — self
    if (p.peek() == '.' && p.peek_at(1) != '.') {
        p.consume(1);
        Step s;
        s.axis = Axis::SelfAxis;
        s.node_test = NodeTest::node();
        return s;
    }
    // `@attr` — abbreviated attribute axis
    if (p.peek() == '@') {
        p.consume(1);
        auto test_r = parse_node_test(p);
        if (!test_r) return std::unexpected(test_r.error());
        auto preds_r = parse_predicates(p);
        if (!preds_r) return std::unexpected(preds_r.error());
        Step s;
        s.axis = Axis::Attribute;
        s.node_test = std::move(*test_r);
        s.predicates = std::move(*preds_r);
        return s;
    }

    // Explicit `axis::`
    std::size_t save = p.pos;
    p.skip_ws();
    auto axis_name = p.parse_axis_name();
    if (axis_name && p.starts_with("::")) {
        Axis axis;
        bool ok = true;
        auto const& n = *axis_name;
        if (n == "child") axis = Axis::Child;
        else if (n == "descendant") axis = Axis::Descendant;
        else if (n == "parent") axis = Axis::Parent;
        else if (n == "ancestor") axis = Axis::Ancestor;
        else if (n == "following-sibling") axis = Axis::FollowingSibling;
        else if (n == "preceding-sibling") axis = Axis::PrecedingSibling;
        else if (n == "following") axis = Axis::Following;
        else if (n == "preceding") axis = Axis::Preceding;
        else if (n == "self") axis = Axis::SelfAxis;
        else if (n == "descendant-or-self") axis = Axis::DescendantOrSelf;
        else if (n == "ancestor-or-self") axis = Axis::AncestorOrSelf;
        else if (n == "attribute") axis = Axis::Attribute;
        else if (n == "namespace") axis = Axis::Namespace;
        else ok = false;

        if (ok) {
            p.consume(2);  // `::`
            p.skip_ws();
            auto test_r = parse_node_test(p);
            if (!test_r) return std::unexpected(test_r.error());
            auto preds_r = parse_predicates(p);
            if (!preds_r) return std::unexpected(preds_r.error());
            Step s;
            s.axis = axis;
            s.node_test = std::move(*test_r);
            s.predicates = std::move(*preds_r);
            return s;
        }
    }
    // Restore position — axis specifier did not match.
    p.pos = save;

    // Default: child axis
    auto test_r = parse_node_test(p);
    if (!test_r) return std::unexpected(test_r.error());
    auto preds_r = parse_predicates(p);
    if (!preds_r) return std::unexpected(preds_r.error());
    Step s;
    s.axis = Axis::Child;
    s.node_test = std::move(*test_r);
    s.predicates = std::move(*preds_r);
    return s;
}

/// Parse a node test: name, wildcard, node-type test.
Result<NodeTest> parse_node_test(Parser& p) {
    p.skip_ws();
    if (p.pos >= p.src.size()) {
        return std::unexpected(p.error("expected node test"));
    }

    // Wildcard `*` or `prefix:*`
    if (p.peek() == '*') {
        p.consume(1);
        return NodeTest::wildcard();
    }

    // Try `prefix:*` — name `:` `*`
    {
        std::size_t save = p.pos;
        if (auto nm = p.parse_name()) {
            if (p.peek() == ':' && p.peek_at(1) == '*') {
                // name ended with `:` is impossible — parse_name consumes `:`
                // only as part of QName. So this check never triggers here.
            }
            // detect `prefix:*` form: name without `:` followed by `:*`
            if (!nm->empty() && nm->find(':') == std::string::npos
                && p.peek() == ':' && p.peek_at(1) == '*') {
                p.consume(2);
                return NodeTest::namespaced(*nm, "*");
            }
            p.pos = save;
        } else {
            p.pos = save;
        }
    }

    // Try node-type test: `text()`, `node()`, `comment()`, `processing-instruction(['name'])`
    {
        std::size_t save = p.pos;
        p.skip_ws();
        if (auto nm = p.parse_axis_name()) {
            // std::size_t after_name = p.pos;  // unused — silenced
            p.skip_ws();
            if (p.peek() == '(') {
                p.consume(1);
                p.skip_ws();
                auto const& n = *nm;
                if (n == "text") {
                    if (p.peek() != ')') { p.pos = save; goto not_type_test; }
                    p.consume(1);
                    return NodeTest::text();
                }
                if (n == "node") {
                    if (p.peek() != ')') { p.pos = save; goto not_type_test; }
                    p.consume(1);
                    return NodeTest::node();
                }
                if (n == "comment") {
                    if (p.peek() != ')') { p.pos = save; goto not_type_test; }
                    p.consume(1);
                    return NodeTest::comment();
                }
                if (n == "processing-instruction") {
                    if (p.peek() == ')') {
                        p.consume(1);
                        return NodeTest::pi();
                    }
                    // Optional string argument
                    auto sl = p.parse_string_literal();
                    if (!sl) { p.pos = save; goto not_type_test; }
                    p.skip_ws();
                    if (p.peek() != ')') { p.pos = save; goto not_type_test; }
                    p.consume(1);
                    return NodeTest::pi_name(std::move(*sl));
                }
            }
            p.pos = save;
        } else {
            p.pos = save;
        }
    }
not_type_test:;

    // Name test — Name or NamespacedName (prefix:local)
    auto nm = p.parse_name();
    if (!nm) {
        return std::unexpected(p.error("expected node test"));
    }
    // Reject names containing `::` (axis-like — should have been caught earlier)
    if (nm->find("::") != std::string::npos) {
        return std::unexpected(p.error("unexpected '::' in name test"));
    }
    auto colon = nm->find(':');
    if (colon != std::string::npos && colon + 1 < nm->size()) {
        std::string prefix = nm->substr(0, colon);
        std::string local = nm->substr(colon + 1);
        return NodeTest::namespaced(std::move(prefix), std::move(local));
    }
    return NodeTest::set_name(std::move(*nm));
}

/// Parse zero or more `[pred]` predicates.
Result<std::vector<std::unique_ptr<XPathExpr>>> parse_predicates(Parser& p) {
    std::vector<std::unique_ptr<XPathExpr>> preds;
    while (true) {
        p.skip_ws();
        if (p.peek() != '[') break;
        p.consume(1);
        auto e = parse_predicate_expr(p);
        if (!e) return std::unexpected(e.error());
        p.skip_ws();
        if (p.peek() != ']') {
            return std::unexpected(p.error("expected ']'"));
        }
        p.consume(1);
        preds.push_back(std::make_unique<XPathExpr>(std::move(*e)));
    }
    return preds;
}

/// Parse continuation steps after the first step: `/step`, `//step`.
Result<std::vector<Step>> parse_continuation_steps(Parser& p) {
    std::vector<Step> steps;
    while (true) {
        if (p.starts_with("//")) {
            p.consume(2);
            Step desc;
            desc.axis = Axis::DescendantOrSelf;
            desc.node_test = NodeTest::node();
            steps.push_back(std::move(desc));
            auto s = parse_step(p);
            if (!s) return std::unexpected(s.error());
            steps.push_back(std::move(*s));
        } else if (p.peek() == '/') {
            p.consume(1);
            if (p.pos >= p.src.size() || p.peek() == '|' || p.peek() == ')'
                || p.peek() == ']') {
                break;
            }
            auto s = parse_step(p);
            if (!s) return std::unexpected(s.error());
            steps.push_back(std::move(*s));
        } else {
            break;
        }
    }
    return steps;
}

/// Parse a location path (absolute or relative).
Result<XPathExpr> parse_location_path(Parser& p) {
    p.skip_ws();
    if (p.peek() == '/') {
        p.consume(1);
        // `//...`
        if (p.peek() == '/') {
            p.consume(1);
            auto first = parse_step(p);
            if (!first) return std::unexpected(first.error());
            std::vector<Step> steps;
            Step desc;
            desc.axis = Axis::DescendantOrSelf;
            desc.node_test = NodeTest::node();
            steps.push_back(std::move(desc));
            steps.push_back(std::move(*first));
            auto more = parse_continuation_steps(p);
            if (!more) return std::unexpected(more.error());
            for (auto& s : *more) steps.push_back(std::move(s));
            LocationPath path;
            path.absolute = true;
            path.steps = std::move(steps);
            return XPathExpr(std::move(path));
        }
        // Bare `/` — select root
        if (p.pos >= p.src.size() || p.peek() == '|' || p.peek() == ')'
            || p.peek() == ']') {
            LocationPath path;
            path.absolute = true;
            return XPathExpr(std::move(path));
        }
        auto first = parse_step(p);
        if (!first) return std::unexpected(first.error());
        std::vector<Step> steps;
        steps.push_back(std::move(*first));
        auto more = parse_continuation_steps(p);
        if (!more) return std::unexpected(more.error());
        for (auto& s : *more) steps.push_back(std::move(s));
        LocationPath path;
        path.absolute = true;
        path.steps = std::move(steps);
        return XPathExpr(std::move(path));
    }
    if (p.starts_with("//")) {
        p.consume(2);
        auto first = parse_step(p);
        if (!first) return std::unexpected(first.error());
        std::vector<Step> steps;
        Step desc;
        desc.axis = Axis::DescendantOrSelf;
        desc.node_test = NodeTest::node();
        steps.push_back(std::move(desc));
        steps.push_back(std::move(*first));
        auto more = parse_continuation_steps(p);
        if (!more) return std::unexpected(more.error());
        for (auto& s : *more) steps.push_back(std::move(s));
        LocationPath path;
        path.absolute = true;
        path.steps = std::move(steps);
        return XPathExpr(std::move(path));
    }
    // Relative path
    auto first = parse_step(p);
    if (!first) return std::unexpected(first.error());
    std::vector<Step> steps;
    steps.push_back(std::move(*first));
    auto more = parse_continuation_steps(p);
    if (!more) return std::unexpected(more.error());
    for (auto& s : *more) steps.push_back(std::move(s));
    LocationPath path;
    path.absolute = false;
    path.steps = std::move(steps);
    return XPathExpr(std::move(path));
}

/// `function_name ( args )` — function call expression.
Result<XPathExpr> parse_function_call_expr(Parser& p) {
    p.skip_ws();
    if (p.pos >= p.src.size() || !is_name_start(p.peek())) {
        return std::unexpected(p.error("expected function name"));
    }
    auto nm = p.parse_axis_name();
    if (!nm) return std::unexpected(p.error("expected function name"));
    // Reject node-type tests — those are node tests, not functions.
    if (*nm == "text" || *nm == "node" || *nm == "comment"
        || *nm == "processing-instruction") {
        return std::unexpected(p.error("node-type test in expression context"));
    }
    p.skip_ws();
    if (p.peek() != '(') {
        return std::unexpected(p.error("expected '(' after function name"));
    }
    p.consume(1);
    p.skip_ws();

    std::vector<std::unique_ptr<XPathExpr>> args;
    if (p.peek() != ')') {
        auto first = parse_predicate_expr(p);
        if (!first) return std::unexpected(first.error());
        args.push_back(std::make_unique<XPathExpr>(std::move(*first)));
        while (true) {
            p.skip_ws();
            if (p.peek() != ',') break;
            p.consume(1);
            auto a = parse_predicate_expr(p);
            if (!a) return std::unexpected(a.error());
            args.push_back(std::make_unique<XPathExpr>(std::move(*a)));
        }
    }
    p.skip_ws();
    if (p.peek() != ')') return std::unexpected(p.error("expected ')'"));
    p.consume(1);

    XPathExpr e;
    e.kind = XPathExpr::Kind::FunctionCall;
    e.function_name = std::move(*nm);
    e.function_args = std::move(args);
    return e;
}

/// Primary expression — function call, parenthesized, string, number, or path.
Result<XPathExpr> parse_primary_expr(Parser& p) {
    p.skip_ws();
    if (p.pos >= p.src.size()) {
        return std::unexpected(p.error("expected primary expression"));
    }
    char c = p.peek();

    // String literal
    if (c == '\'' || c == '"') {
        auto sl = p.parse_string_literal();
        if (!sl) return std::unexpected(p.error("unterminated string literal"));
        XPathExpr e;
        e.kind = XPathExpr::Kind::StringLiteral;
        e.string_literal = std::move(*sl);
        return e;
    }

    // Number literal
    if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && p.pos + 1 < p.src.size()
        && std::isdigit(static_cast<unsigned char>(p.src[p.pos + 1])))) {
        std::size_t start = p.pos;
        bool any_digit = false;
        while (p.pos < p.src.size()
               && (std::isdigit(static_cast<unsigned char>(p.src[p.pos]))
                   || p.src[p.pos] == '.')) {
            if (std::isdigit(static_cast<unsigned char>(p.src[p.pos]))) any_digit = true;
            ++p.pos;
        }
        // scientific notation
        if (p.pos < p.src.size() && (p.src[p.pos] == 'e' || p.src[p.pos] == 'E')) {
            ++p.pos;
            if (p.pos < p.src.size() && (p.src[p.pos] == '+' || p.src[p.pos] == '-')) ++p.pos;
            while (p.pos < p.src.size()
                   && std::isdigit(static_cast<unsigned char>(p.src[p.pos]))) ++p.pos;
        }
        if (!any_digit) {
            p.pos = start;
            return std::unexpected(p.error("invalid number literal"));
        }
        std::string num_str(p.src.substr(start, p.pos - start));
        double num = std::nan("");
        try {
            num = std::stod(num_str);
        } catch (...) {
            num = std::nan("");
        }
        XPathExpr e;
        e.kind = XPathExpr::Kind::NumberLiteral;
        e.number_literal = num;
        return e;
    }

    // Parenthesized expression
    if (c == '(') {
        p.consume(1);
        auto first = parse_predicate_expr(p);
        if (!first) return std::unexpected(first.error());
        std::vector<std::unique_ptr<XPathExpr>> all;
        all.push_back(std::make_unique<XPathExpr>(std::move(*first)));
        // union inside parens
        while (true) {
            p.skip_ws();
            if (p.peek() != '|') break;
            p.consume(1);
            auto next = parse_predicate_expr(p);
            if (!next) return std::unexpected(next.error());
            all.push_back(std::make_unique<XPathExpr>(std::move(*next)));
        }
        p.skip_ws();
        if (p.peek() != ')') return std::unexpected(p.error("expected ')'"));
        p.consume(1);
        if (all.size() == 1) {
            return std::move(*all.front());
        }
        XPathExpr e;
        e.kind = XPathExpr::Kind::Union;
        e.union_members = std::move(all);
        return e;
    }

    // Function call — name followed by `(`. Distinguish from path by looking
    // ahead: identifier then `(`.
    {
        std::size_t save = p.pos;
        p.skip_ws();
        if (p.pos < p.src.size() && is_name_start(p.peek())) {
            auto nm = p.parse_axis_name();
            if (nm) {
                // std::size_t after = p.pos;  // unused — silenced
                p.skip_ws();
                if (p.peek() == '('
                    && *nm != "text" && *nm != "node"
                    && *nm != "comment" && *nm != "processing-instruction") {
                    p.pos = save;
                    return parse_function_call_expr(p);
                }
            }
            p.pos = save;
        } else {
            p.pos = save;
        }
    }

    // Otherwise: a (possibly abbreviated) location path
    return parse_location_path(p);
}

/// `primary_expr (| primary_expr)*` — union of path-like expressions.
Result<XPathExpr> parse_union_path_expr(Parser& p) {
    auto first = parse_primary_expr_inner(p);
    if (!first) return std::unexpected(first.error());

    bool is_path_like = false;
    switch (first->kind) {
        case XPathExpr::Kind::LocationPath:
        case XPathExpr::Kind::FunctionCall:
        case XPathExpr::Kind::FilterPath:
        case XPathExpr::Kind::GlobalFilter:
        case XPathExpr::Kind::Union:
            is_path_like = true;
            break;
        default: break;
    }
    if (!is_path_like) {
        return std::move(*first);
    }

    std::vector<std::unique_ptr<XPathExpr>> all;
    all.push_back(std::make_unique<XPathExpr>(std::move(*first)));
    while (true) {
        p.skip_ws();
        if (p.peek() != '|') break;
        p.consume(1);
        auto next = parse_primary_expr_inner(p);
        if (!next) return std::unexpected(next.error());
        all.push_back(std::make_unique<XPathExpr>(std::move(*next)));
    }
    if (all.size() == 1) return std::move(*all.front());
    XPathExpr e;
    e.kind = XPathExpr::Kind::Union;
    e.union_members = std::move(all);
    return e;
}

/// Primary expression inner — used by union_path_expr (skips leading ws).
Result<XPathExpr> parse_primary_expr_inner(Parser& p) {
    return parse_primary_expr(p);
}

/// Unary minus or union path.
Result<XPathExpr> parse_unary_expr(Parser& p) {
    p.skip_ws();
    if (p.peek() == '-') {
        p.consume(1);
        auto inner = parse_unary_expr(p);
        if (!inner) return std::unexpected(inner.error());
        XPathExpr e;
        e.kind = XPathExpr::Kind::UnaryMinus;
        e.unary_inner = std::make_unique<XPathExpr>(std::move(*inner));
        return e;
    }
    return parse_union_path_expr(p);
}

/// `* | div | mod` — multiplicative level.
Result<XPathExpr> parse_multiplicative_expr(Parser& p) {
    auto left = parse_unary_expr(p);
    if (!left) return std::unexpected(left.error());
    while (true) {
        p.skip_ws();
        char c = p.peek();
        if (c == '*') {
            p.consume(1);
            auto right = parse_unary_expr(p);
            if (!right) return std::unexpected(right.error());
            XPathExpr e;
            e.kind = XPathExpr::Kind::BinaryOp;
            e.binary_left = std::make_unique<XPathExpr>(std::move(*left));
            e.binary_op = BinaryOp::Mul;
            e.binary_right = std::make_unique<XPathExpr>(std::move(*right));
            left = std::move(e);
            continue;
        }
        if (p.starts_with("div")
            && (p.pos + 3 >= p.src.size()
                || (!is_axis_char(p.src[p.pos + 3])))) {
            p.consume(3);
            auto right = parse_unary_expr(p);
            if (!right) return std::unexpected(right.error());
            XPathExpr e;
            e.kind = XPathExpr::Kind::BinaryOp;
            e.binary_left = std::make_unique<XPathExpr>(std::move(*left));
            e.binary_op = BinaryOp::Div;
            e.binary_right = std::make_unique<XPathExpr>(std::move(*right));
            left = std::move(e);
            continue;
        }
        if (p.starts_with("mod")
            && (p.pos + 3 >= p.src.size()
                || (!is_axis_char(p.src[p.pos + 3])))) {
            p.consume(3);
            auto right = parse_unary_expr(p);
            if (!right) return std::unexpected(right.error());
            XPathExpr e;
            e.kind = XPathExpr::Kind::BinaryOp;
            e.binary_left = std::make_unique<XPathExpr>(std::move(*left));
            e.binary_op = BinaryOp::Mod;
            e.binary_right = std::make_unique<XPathExpr>(std::move(*right));
            left = std::move(e);
            continue;
        }
        return left;
    }
}

/// `+ | -` — additive level.
Result<XPathExpr> parse_additive_expr(Parser& p) {
    auto left = parse_multiplicative_expr(p);
    if (!left) return std::unexpected(left.error());
    while (true) {
        p.skip_ws();
        char c = p.peek();
        if (c != '+' && c != '-') break;
        p.consume(1);
        auto right = parse_multiplicative_expr(p);
        if (!right) return std::unexpected(right.error());
        XPathExpr e;
        e.kind = XPathExpr::Kind::BinaryOp;
        e.binary_left = std::make_unique<XPathExpr>(std::move(*left));
        e.binary_op = (c == '+') ? BinaryOp::Add : BinaryOp::Sub;
        e.binary_right = std::make_unique<XPathExpr>(std::move(*right));
        left = std::move(e);
    }
    return left;
}

/// `< > <= >=` — relational level.
Result<XPathExpr> parse_relational_expr(Parser& p) {
    auto left = parse_additive_expr(p);
    if (!left) return std::unexpected(left.error());
    while (true) {
        p.skip_ws();
        BinaryOp op;
        if (p.starts_with("<=")) { op = BinaryOp::Lte; p.consume(2); }
        else if (p.starts_with(">=")) { op = BinaryOp::Gte; p.consume(2); }
        else if (p.peek() == '<') { op = BinaryOp::Lt; p.consume(1); }
        else if (p.peek() == '>') { op = BinaryOp::Gt; p.consume(1); }
        else break;
        auto right = parse_additive_expr(p);
        if (!right) return std::unexpected(right.error());
        XPathExpr e;
        e.kind = XPathExpr::Kind::BinaryOp;
        e.binary_left = std::make_unique<XPathExpr>(std::move(*left));
        e.binary_op = op;
        e.binary_right = std::make_unique<XPathExpr>(std::move(*right));
        left = std::move(e);
    }
    return left;
}

/// `= !=` — equality level.
Result<XPathExpr> parse_equality_expr(Parser& p) {
    auto left = parse_relational_expr(p);
    if (!left) return std::unexpected(left.error());
    while (true) {
        p.skip_ws();
        BinaryOp op;
        if (p.starts_with("!=")) { op = BinaryOp::Neq; p.consume(2); }
        else if (p.peek() == '=') { op = BinaryOp::Eq; p.consume(1); }
        else break;
        auto right = parse_relational_expr(p);
        if (!right) return std::unexpected(right.error());
        XPathExpr e;
        e.kind = XPathExpr::Kind::BinaryOp;
        e.binary_left = std::make_unique<XPathExpr>(std::move(*left));
        e.binary_op = op;
        e.binary_right = std::make_unique<XPathExpr>(std::move(*right));
        left = std::move(e);
    }
    return left;
}

/// `and` — logical and level.
Result<XPathExpr> parse_and_expr(Parser& p) {
    auto left = parse_equality_expr(p);
    if (!left) return std::unexpected(left.error());
    while (true) {
        p.skip_ws();
        if (p.starts_with("and")
            && (p.pos + 3 >= p.src.size()
                || is_ws(p.src[p.pos + 3])
                || p.src[p.pos + 3] == '(')) {
            p.consume(3);
            auto right = parse_and_expr(p);
            if (!right) return std::unexpected(right.error());
            XPathExpr e;
            e.kind = XPathExpr::Kind::BinaryOp;
            e.binary_left = std::make_unique<XPathExpr>(std::move(*left));
            e.binary_op = BinaryOp::And;
            e.binary_right = std::make_unique<XPathExpr>(std::move(*right));
            left = std::move(e);
        } else {
            return left;
        }
    }
}

/// `or` — logical or level (lowest precedence).
Result<XPathExpr> parse_or_expr(Parser& p) {
    auto left = parse_and_expr(p);
    if (!left) return std::unexpected(left.error());
    while (true) {
        p.skip_ws();
        if (p.starts_with("or")
            && (p.pos + 2 >= p.src.size()
                || is_ws(p.src[p.pos + 2])
                || p.src[p.pos + 2] == '(')) {
            p.consume(2);
            auto right = parse_or_expr(p);
            if (!right) return std::unexpected(right.error());
            XPathExpr e;
            e.kind = XPathExpr::Kind::BinaryOp;
            e.binary_left = std::make_unique<XPathExpr>(std::move(*left));
            e.binary_op = BinaryOp::Or;
            e.binary_right = std::make_unique<XPathExpr>(std::move(*right));
            left = std::move(e);
        } else {
            return left;
        }
    }
}

/// Predicate expression — top of the precedence chain.
Result<XPathExpr> parse_predicate_expr(Parser& p) {
    return parse_or_expr(p);
}

/// Function optionally followed by `/path` or `[preds]`.
Result<XPathExpr> parse_function_path_expr(Parser& p) {
    auto func = parse_function_call_expr(p);
    if (!func) return std::unexpected(func.error());

    if (p.peek() == '/') {
        auto steps = parse_continuation_steps(p);
        if (!steps) return std::unexpected(steps.error());
        if (steps->empty()) return std::move(*func);
        XPathExpr e;
        e.kind = XPathExpr::Kind::FilterPath;
        e.filter_inner = std::make_unique<XPathExpr>(std::move(*func));
        e.filter_steps = std::move(*steps);
        return e;
    }
    auto preds = parse_predicates(p);
    if (!preds) return std::unexpected(preds.error());
    if (preds->empty()) return std::move(*func);
    XPathExpr e;
    e.kind = XPathExpr::Kind::GlobalFilter;
    e.global_filter_inner = std::make_unique<XPathExpr>(std::move(*func));
    e.global_filter_preds = std::move(*preds);
    return e;
}

/// `( expr )` then optional `/path` or `[preds]`.
Result<XPathExpr> parse_parenthesized_filter(Parser& p) {
    if (auto r = p.expect('('); !r) return std::unexpected(r.error());
    auto inner = parse_xpath_expr(p);
    if (!inner) return std::unexpected(inner.error());
    if (auto r = p.expect(')'); !r) return std::unexpected(r.error());

    if (p.peek() == '/') {
        auto steps = parse_continuation_steps(p);
        if (!steps) return std::unexpected(steps.error());
        if (steps->empty()) return std::move(*inner);
        XPathExpr filter;
        filter.kind = XPathExpr::Kind::FilterPath;
        filter.filter_inner = std::make_unique<XPathExpr>(std::move(*inner));
        filter.filter_steps = std::move(*steps);
        // optional predicates after
        auto preds = parse_predicates(p);
        if (!preds) return std::unexpected(preds.error());
        if (preds->empty()) return filter;
        XPathExpr e;
        e.kind = XPathExpr::Kind::GlobalFilter;
        e.global_filter_inner = std::make_unique<XPathExpr>(std::move(filter));
        e.global_filter_preds = std::move(*preds);
        return e;
    }
    auto preds = parse_predicates(p);
    if (!preds) return std::unexpected(preds.error());
    if (preds->empty()) return std::move(*inner);
    XPathExpr e;
    e.kind = XPathExpr::Kind::GlobalFilter;
    e.global_filter_inner = std::make_unique<XPathExpr>(std::move(*inner));
    e.global_filter_preds = std::move(*preds);
    return e;
}

/// Union expression — `path | path | ...`.
Result<XPathExpr> parse_union_expr(Parser& p) {
    auto first = parse_location_path(p);
    if (!first) return std::unexpected(first.error());
    std::vector<std::unique_ptr<XPathExpr>> all;
    all.push_back(std::make_unique<XPathExpr>(std::move(*first)));
    while (true) {
        p.skip_ws();
        if (p.peek() != '|') break;
        p.consume(1);
        auto next = parse_location_path(p);
        if (!next) return std::unexpected(next.error());
        all.push_back(std::make_unique<XPathExpr>(std::move(*next)));
    }
    if (all.size() == 1) return std::move(*all.front());
    XPathExpr e;
    e.kind = XPathExpr::Kind::Union;
    e.union_members = std::move(all);
    return e;
}

/// Top-level XPath expression — tries parenthesized filter, function path,
/// union, then bare location path.
Result<XPathExpr> parse_xpath_expr(Parser& p) {
    p.skip_ws();
    // Peek to dispatch
    if (p.peek() == '(') return parse_parenthesized_filter(p);

    // function name + `(` → function_path_expr
    {
        std::size_t save = p.pos;
        if (p.pos < p.src.size() && is_name_start(p.peek())) {
            auto nm = p.parse_axis_name();
            if (nm) {
                // std::size_t after = p.pos;  // unused — silenced
                p.skip_ws();
                if (p.peek() == '('
                    && *nm != "text" && *nm != "node"
                    && *nm != "comment" && *nm != "processing-instruction") {
                    p.pos = save;
                    return parse_function_path_expr(p);
                }
            }
            p.pos = save;
        }
    }
    // Otherwise: union expr (which falls back to location path)
    return parse_union_expr(p);
}

}  // namespace

Result<XPathExpr>
parse_xpath(std::string_view input) {
    // Trim leading/trailing whitespace
    while (!input.empty() && is_ws(input.front())) input.remove_prefix(1);
    while (!input.empty() && is_ws(input.back())) input.remove_suffix(1);

    Parser p(input);
    auto r = parse_xpath_expr(p);
    if (!r) return std::unexpected(r.error());
    p.skip_ws();
    if (p.pos != p.src.size()) {
        return std::unexpected(SimdXmlError::xpath_parse_error(
            "Unexpected trailing input: '" + std::string(p.src.substr(p.pos)) + "'"));
    }
    return std::move(*r);
}

Result<XPathExpr>
parse_xpath_predicate_expr(std::string_view input) {
    while (!input.empty() && is_ws(input.front())) input.remove_prefix(1);
    while (!input.empty() && is_ws(input.back())) input.remove_suffix(1);

    Parser p(input);
    auto r = parse_predicate_expr(p);
    if (!r) return std::unexpected(r.error());
    p.skip_ws();
    if (p.pos != p.src.size()) {
        return std::unexpected(SimdXmlError::xpath_parse_error(
            "Unexpected trailing input: '" + std::string(p.src.substr(p.pos)) + "'"));
    }
    return std::move(*r);
}

}  // namespace rai::xml