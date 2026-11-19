#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "document.hpp"

namespace fasthtmlparser {

namespace detail {
struct CssList;
struct XPathExpr;
} // namespace detail

/// The result of compiling a selector or an expression: the compiled query, or the error message.
template <class T>
class Compiled {
public:
    Compiled(T value) : value_(std::move(value)) {}
    static Compiled failure(std::string message) { return Compiled(std::move(message), 0); }

    bool ok() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    /// The compiled query (ok() must be true).
    T& value() & noexcept { return *value_; }
    const T& value() const& noexcept { return *value_; }
    T&& value() && noexcept { return std::move(*value_); }
    T& operator*() & noexcept { return *value_; }
    const T& operator*() const& noexcept { return *value_; }
    T* operator->() noexcept { return &*value_; }
    const T* operator->() const noexcept { return &*value_; }

    /// The error message ("" when ok()).
    const std::string& error() const noexcept { return error_; }

private:
    Compiled(std::string message, int) : error_(std::move(message)) {}
    std::optional<T> value_;
    std::string error_;
};

/// A compiled CSS selector (or a comma-separated list of them). Compile once and reuse it on any number of
/// documents, from any number of threads; the string methods (Document::css, Node::css, ...) compile and cache
/// selectors.
///
/// Supported: type, *, #id, .class, attribute selectors ([a], =, ~=, |=, ^=, $=, *=, with the i / s flags), the
/// combinators (descendant, >, +, ~), selector lists (a, b) and the pseudo-classes :first-child, :last-child,
/// :only-child, :first-of-type, :last-of-type, :only-of-type, :nth-child(An+B [of S]), :nth-last-child(),
/// :nth-of-type(), :nth-last-of-type(), :not(), :is(), :where(), :has(), :empty, :root, :scope, :link, :any-link,
/// :checked, :disabled, :enabled, :required, :optional, and :contains("text"). Pseudo-elements and namespaces are not
/// supported.
///
/// HTML rules: tag and attribute names are case-insensitive; ids, classes and attribute values are case-sensitive,
/// except the values the HTML standard lists (type, rel, lang, method, ...); the i / s flags override that.
/// Attribute values are compared decoded. Only elements are selected, never comments.
///
/// As querySelectorAll: all_in(node) returns the tags inside the node, while the whole selector is matched against
/// the document. :scope is the node; a selector may start with '>' for its children: "> li".
class CssQuery {
public:
    /// Compiles a CSS selector; never throws on an invalid one.
    static Compiled<CssQuery> compile(std::string_view selector);

    /// The source of the selector.
    const std::string& str() const noexcept { return selector_; }

    /// The matching tags in the document, in document order.
    std::vector<Node> all(const Document& doc) const;
    /// The first matching tag in the document.
    std::optional<Node> first(const Document& doc) const;
    /// The matching tags inside n (as querySelectorAll).
    std::vector<Node> all_in(const Node& n) const;
    /// The first matching tag inside n.
    std::optional<Node> first_in(const Node& n) const;
    /// Whether the tag matches (as element.matches(): :scope is the tag itself); false for comments.
    bool matches(const Node& n) const;

private:
    CssQuery(std::string selector, std::shared_ptr<const detail::CssList> list)
        : selector_(std::move(selector)), list_(std::move(list)) {}
    std::string selector_;
    std::shared_ptr<const detail::CssList> list_;
};

/// A compiled XPath 1.0 expression. Compile once and reuse it on any number of documents, from any number of
/// threads; the string methods (Document::xpath_value, Node::xpath, ...) compile and cache expressions.
///
/// All axes (except namespace), predicates, '|', the operators and the XPath 1.0 functions are supported, plus
/// ends-with(), lower-case() and upper-case(). HTML rules apply: names are case-insensitive, the string-value of an
/// element is its visible text (as Node::text), attribute values and text are decoded. Variables are not supported.
///
/// On a tag, relative paths start at the tag; '/' and '//' search the whole document.
class XPathQuery {
public:
    /// Compiles an XPath 1.0 expression; never throws on an invalid one.
    static Compiled<XPathQuery> compile(std::string_view expression);

    /// The source of the expression.
    const std::string& str() const noexcept { return expression_; }

    /// Whether the expression gives a node-set (and so nodes() can be used); false for one that gives a number,
    /// string or boolean.
    bool is_node_set() const noexcept;

    /// The selected tags (elements and comments) in document order, evaluated on the document. Throws
    /// std::logic_error when the expression does not give a node-set (see is_node_set; use values()).
    std::vector<Node> nodes(const Document& doc) const;
    /// nodes() evaluated on a tag: relative paths start at it.
    std::vector<Node> nodes_from(const Node& n) const;
    /// The string-values of the selected nodes, or the result of an expression that gives a number, string or
    /// boolean (count(//a), concat(...)) as one value.
    std::vector<std::string> values(const Document& doc) const;
    std::vector<std::string> values_from(const Node& n) const;
    /// The string-value of the first selected node, or the result of the expression; none when it selects no node.
    std::optional<std::string> value(const Document& doc) const;
    std::optional<std::string> value_from(const Node& n) const;

private:
    XPathQuery(std::string expression, std::shared_ptr<const detail::XPathExpr> expr)
        : expression_(std::move(expression)), expr_(std::move(expr)) {}
    std::string expression_;
    std::shared_ptr<const detail::XPathExpr> expr_;
};

} // namespace fasthtmlparser
