// XPath 1.0 over the flat tag list: a tokenizer, a recursive-descent compiler and an evaluator.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

#include "chars.hpp"
#include "document_data.hpp"
#include "fasthtmlparser/query.hpp"
#include "query_cache.hpp"

namespace fasthtmlparser {

namespace detail {
namespace {

// ----- the data model over the flat tag list -----

enum class XKind : unsigned char { root, element, comment, text, attribute };

/// A node of the XPath data model: the document root, a tag (element or comment), a text node (a run of source text
/// between tags) or an attribute (positions in the opening tag).
struct XNode {
    static constexpr std::uint32_t no_value = 0xFFFFFFFFu;

    XKind kind = XKind::root;
    std::int32_t tag = -1;           // element / comment: the tag; text / attribute: the parent element (-1: the root)
    std::uint32_t start = 0;         // text: first byte; attribute: name start (positions fit in 32 bits: NodeData)
    std::uint32_t length = 0;        // text: length; attribute: name length
    std::uint32_t value_start = no_value; // attribute: value start (no_value without a value)
    std::uint32_t value_length = 0;

    static XNode root() { return XNode{}; }

    static XNode of_tag(const DocumentData& doc, std::size_t i) {
        XNode x;
        x.kind = doc.nodes[i].key == Tag::COMMENT.index() ? XKind::comment : XKind::element;
        x.tag = static_cast<std::int32_t>(i);
        return x;
    }

    /// Document order and identity: tags and text nodes by their first byte; attributes right after their element,
    /// before its children.
    std::int64_t key(const DocumentData& doc) const {
        switch (kind) {
        case XKind::root: return -1;
        case XKind::element:
        case XKind::comment: return static_cast<std::int64_t>(doc.nodes[tag].start) << 32;
        case XKind::attribute:
            return (static_cast<std::int64_t>(doc.nodes[tag].start) << 32) |
                   static_cast<std::int64_t>(static_cast<std::uint32_t>(start));
        case XKind::text: return static_cast<std::int64_t>(start) << 32;
        }
        return -1;
    }

    /// The source positions the node covers (for following / preceding).
    std::int64_t range_start(const DocumentData& doc) const {
        switch (kind) {
        case XKind::root: return -1;
        case XKind::text: return static_cast<std::int64_t>(start);
        default: return doc.nodes[tag].start;
        }
    }

    std::int64_t range_end(const DocumentData& doc) const {
        switch (kind) {
        case XKind::root: return std::numeric_limits<std::int32_t>::max();
        case XKind::element:
        case XKind::comment: return doc.nodes[tag].end;
        case XKind::text: return static_cast<std::int64_t>(start) + length - 1;
        case XKind::attribute: return doc.nodes[tag].start; // the children of its element follow it
        }
        return 0;
    }

    std::string_view attr_name(const DocumentData& doc) const { return doc.source.substr(start, length); }
};

/// The node-sets of one evaluation take their memory from an arena that lives as long as the evaluation.
using NodeSet = std::pmr::vector<XNode>;

// ----- node tests -----

enum class TestKind : unsigned char {
    name,
    prefix,    // dc:*
    wildcard,  // *
    node,      // node()
    text,      // text()
    comment,   // comment()
    pi,        // processing-instruction(): never matches
    container, // internal: the root and elements (// before a child or attribute step)
};

struct NodeTest {
    TestKind kind = TestKind::node;
    std::string name;
    int key = -1; // the name as a known tag

    NodeTest() = default;
    NodeTest(TestKind k, std::string_view n) : kind(k), name(n) {
        if (k == TestKind::name) key = lookup_tag(n);
    }

    static NodeTest any_node() { return NodeTest(TestKind::node, ""); }

    bool matches_text() const { return kind == TestKind::node || kind == TestKind::text; }
    bool matches_tags() const { return kind != TestKind::text && kind != TestKind::pi; }

    bool prefix_matches(std::string_view n) const {
        return n.size() >= name.size() && is_char_boundary(n, name.size()) &&
               equal_fold(n.substr(0, name.size()), name);
    }

    bool matches_tag(const DocumentData& doc, std::size_t i) const {
        const NodeData& d = doc.nodes[i];
        switch (kind) {
        case TestKind::name:
            // as by_name(name).matches(n)
            if (key >= 0) return d.key == key || (d.key < 0 && equal_fold(doc.name(i), name));
            return equal_fold(doc.name(i), name);
        case TestKind::prefix: return is_element(doc, i) && prefix_matches(doc.name(i));
        case TestKind::wildcard:
        case TestKind::container: return is_element(doc, i);
        case TestKind::node: return is_element(doc, i) || d.key == Tag::COMMENT.index();
        case TestKind::comment: return d.key == Tag::COMMENT.index();
        default: return false;
        }
    }

    bool matches_attribute(std::string_view n) const {
        switch (kind) {
        case TestKind::name: return equal_fold(n, name);
        case TestKind::prefix: return prefix_matches(n);
        case TestKind::wildcard:
        case TestKind::node: return true;
        default: return false;
        }
    }

    bool matches(const DocumentData& doc, const XNode& x, bool attribute_axis) const {
        switch (x.kind) {
        case XKind::root: return kind == TestKind::node || kind == TestKind::container;
        case XKind::element:
        case XKind::comment: return matches_tag(doc, static_cast<std::size_t>(x.tag));
        case XKind::text: return matches_text();
        case XKind::attribute: return attribute_axis ? matches_attribute(x.attr_name(doc)) : kind == TestKind::node;
        }
        return false;
    }
};

// ----- navigation -----

struct Content {
    std::size_t first;
    std::int32_t end;
    std::int32_t child_depth;
    std::size_t text_start;
    std::size_t text_end;
};

/// The content of a container: the tags (nodes[first..], up to end, at child_depth) and the text range.
bool content(const DocumentData& doc, const XNode& x, Content& c) {
    if (x.kind == XKind::root) {
        c = {0, std::numeric_limits<std::int32_t>::max(), 0, 0, doc.source.size()};
        return true;
    }
    if (x.kind == XKind::element) {
        const NodeData& t = doc.nodes[x.tag];
        if (t.inner_start > 0 && t.inner_end > 0) {
            c = {static_cast<std::size_t>(x.tag) + 1, t.end, t.depth + 1, static_cast<std::size_t>(t.inner_start),
                 static_cast<std::size_t>(t.inner_end) + 1};
            return true;
        }
    }
    return false;
}

void add_text(std::int32_t parent, std::size_t start, std::size_t end, NodeSet& out) {
    if (end > start) {
        XNode x;
        x.kind = XKind::text;
        x.tag = parent;
        x.start = static_cast<std::uint32_t>(start);
        x.length = static_cast<std::uint32_t>(end - start);
        out.push_back(x);
    }
}

/// Appends the children of the root or an element that match test, text nodes included, in document order.
void add_children(const DocumentData& doc, const XNode& x, const NodeTest& test, NodeSet& out) {
    bool want_text = test.matches_text();
    bool want_tags = test.matches_tags();
    if (!want_text && !want_tags) return;
    Content c;
    if (!content(doc, x, c)) return;
    std::int32_t parent = x.kind == XKind::element ? x.tag : -1;
    std::size_t p = c.text_start;
    const auto& nodes = doc.nodes;
    for (std::size_t i = c.first; i < nodes.size(); ++i) {
        const NodeData& t = nodes[i];
        if (t.end > c.end) break;
        if (t.depth != c.child_depth) continue;
        if (want_text && static_cast<std::size_t>(t.start) > p)
            add_text(parent, p, std::min(static_cast<std::size_t>(t.start), c.text_end), out);
        if (want_tags && test.matches_tag(doc, i)) out.push_back(XNode::of_tag(doc, i));
        p = static_cast<std::size_t>(t.end) + 1;
    }
    if (want_text && c.text_end > p) add_text(parent, p, c.text_end, out);
}

void add_descendants_with_text(const DocumentData& doc, const XNode& x, const NodeTest& test, NodeSet& out) {
    NodeSet children(out.get_allocator());
    add_children(doc, x, NodeTest::any_node(), children);
    for (const XNode& child : children) {
        if (test.matches(doc, child, false)) out.push_back(child);
        if (child.kind == XKind::element) add_descendants_with_text(doc, child, test, out);
    }
}

/// Appends the descendants of the root or an element that match test, in document order.
void add_descendants(const DocumentData& doc, const XNode& x, const NodeTest& test, NodeSet& out) {
    if (test.matches_text()) {
        add_descendants_with_text(doc, x, test, out);
        return;
    }
    if (!test.matches_tags()) return;
    Content c;
    if (!content(doc, x, c)) return;
    // tags only: one linear scan of the flat list
    for (std::size_t i = c.first; i < doc.nodes.size(); ++i) {
        if (doc.nodes[i].end > c.end) break;
        if (test.matches_tag(doc, i)) out.push_back(XNode::of_tag(doc, i));
    }
}

std::optional<XNode> parent_node(const DocumentData& doc, const XNode& x) {
    switch (x.kind) {
    case XKind::root: return std::nullopt;
    case XKind::text:
    case XKind::attribute:
        if (x.tag >= 0) return XNode::of_tag(doc, static_cast<std::size_t>(x.tag));
        return XNode::root();
    default: {
        const NodeData& t = doc.nodes[x.tag];
        if (t.depth > 0 && t.parent >= 0) return XNode::of_tag(doc, static_cast<std::size_t>(t.parent));
        return XNode::root();
    }
    }
}

void add_attributes(const DocumentData& doc, const XNode& x, const NodeTest& test, NodeSet& out) {
    if (x.kind != XKind::element) return;
    AttrScanner s(doc.at(static_cast<std::size_t>(x.tag)));
    ScannedAttr a;
    while (s.next(a)) {
        if (test.matches_attribute(s.name(a))) {
            XNode n;
            n.kind = XKind::attribute;
            n.tag = x.tag;
            n.start = static_cast<std::uint32_t>(a.name_start);
            n.length = static_cast<std::uint32_t>(a.name_len);
            n.value_start = a.value_start == npos ? XNode::no_value : static_cast<std::uint32_t>(a.value_start);
            n.value_length = static_cast<std::uint32_t>(a.value_len);
            out.push_back(n);
        }
    }
}

/// The raw value of the first attribute with the name; false when the element has none.
bool attribute_of(const DocumentData& doc, const XNode& x, std::string_view name, std::string_view& value) {
    if (x.kind != XKind::element) return false;
    AttrScanner s(doc.at(static_cast<std::size_t>(x.tag)));
    ScannedAttr a;
    while (s.next(a)) {
        if (equal_fold(s.name(a), name)) {
            value = s.value(a);
            return true;
        }
    }
    return false;
}

/// The root and elements give their visible text (as text()); text nodes and attributes their decoded value;
/// comments their content.
CowString string_value(const DocumentData& doc, const XNode& x) {
    switch (x.kind) {
    case XKind::root: return clear_tags(doc.source);
    case XKind::element: return doc.at(static_cast<std::size_t>(x.tag)).text();
    case XKind::attribute:
        if (x.value_start == XNode::no_value) return CowString();
        return decode(doc.source.substr(x.value_start, x.value_length), true);
    case XKind::text: {
        std::string_view text = doc.source.substr(x.start, x.length);
        if (x.tag >= 0 && !raw_text_end(doc.nodes[x.tag].key).empty()) return normalize_newlines(text);
        return decode(text, false);
    }
    case XKind::comment: {
        const NodeData& t = doc.nodes[x.tag];
        std::string_view source =
            doc.source.substr(static_cast<std::size_t>(t.start), static_cast<std::size_t>(t.end + 1 - t.start));
        if (source.substr(0, 4) == "<!--") source.remove_prefix(4);
        if (source.size() >= 3 && source.substr(source.size() - 3) == "-->") source.remove_suffix(3);
        return CowString(source);
    }
    }
    return CowString();
}

/// For name() / local-name(): the lower-case tag or attribute name; "" for other nodes.
std::string node_name(const DocumentData& doc, const XNode& x) {
    switch (x.kind) {
    case XKind::element: return std::string(doc.name(static_cast<std::size_t>(x.tag)));
    case XKind::attribute: return to_lower(x.attr_name(doc));
    default: return {};
    }
}

// ----- values -----

enum class XType : unsigned char { node_set, number, string, boolean };

const char* type_name(XType t) {
    switch (t) {
    case XType::node_set: return "node-set";
    case XType::number: return "number";
    case XType::string: return "string";
    case XType::boolean: return "boolean";
    }
    return "";
}

using Value = std::variant<NodeSet, double, std::string, bool>;

bool is_nodes(const Value& v) { return v.index() == 0; }
bool is_number(const Value& v) { return v.index() == 1; }
bool is_bool(const Value& v) { return v.index() == 3; }

/// An optional '-', digits with an optional decimal point; anything else is NaN.
double string_to_number(std::string_view value) {
    std::string_view s = trim(value);
    std::string_view body = !s.empty() && s[0] == '-' ? s.substr(1) : s;
    int digits = 0;
    int point = 0;
    for (char c : body) {
        if (is_digit(static_cast<unsigned char>(c)))
            ++digits;
        else if (c == '.')
            ++point;
        else
            return std::numeric_limits<double>::quiet_NaN();
    }
    if (digits == 0 || point > 1) return std::numeric_limits<double>::quiet_NaN();
    // from_chars needs a digit before the point
    std::string text;
    if (body[0] == '.') {
        text = s[0] == '-' ? "-0" : "0";
        text.append(body);
        s = text;
    }
    double d = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), d);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return std::numeric_limits<double>::quiet_NaN();
    return d;
}

/// The shortest round-trip digits, written without an exponent (XPath has none).
std::string number_to_string(double d) {
    if (std::isnan(d)) return "NaN";
    if (d == std::numeric_limits<double>::infinity()) return "Infinity";
    if (d == -std::numeric_limits<double>::infinity()) return "-Infinity";
    if (d == 0) return "0";
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::scientific);
    std::string_view sci(buf, static_cast<std::size_t>(r.ptr - buf));
    bool negative = sci[0] == '-';
    if (negative) sci.remove_prefix(1);
    std::size_t e = sci.find('e');
    std::string digits;
    for (char c : sci.substr(0, e))
        if (c != '.') digits.push_back(c);
    int exponent = 0;
    std::string_view exp_text = sci.substr(e + 1);
    if (!exp_text.empty() && exp_text[0] == '+') exp_text.remove_prefix(1);
    std::from_chars(exp_text.data(), exp_text.data() + exp_text.size(), exponent);
    // digits d1 d2 d3... with the point after d1, times 10^exponent
    std::string out = negative ? "-" : "";
    int point = exponent + 1; // the number of digits before the point
    if (point <= 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-point), '0');
        out += digits;
    } else if (static_cast<std::size_t>(point) >= digits.size()) {
        out += digits;
        out.append(static_cast<std::size_t>(point) - digits.size(), '0');
    } else {
        out.append(digits, 0, static_cast<std::size_t>(point));
        out.push_back('.');
        out.append(digits, static_cast<std::size_t>(point));
    }
    return out;
}

bool to_boolean(const Value& v) {
    switch (v.index()) {
    case 0: return !std::get<0>(v).empty();
    case 1: {
        double d = std::get<1>(v);
        return d != 0 && !std::isnan(d);
    }
    case 2: return !std::get<2>(v).empty();
    default: return std::get<3>(v);
    }
}

double to_number(const DocumentData& doc, const Value& v) {
    switch (v.index()) {
    case 0: {
        const NodeSet& n = std::get<0>(v);
        if (n.empty()) return std::numeric_limits<double>::quiet_NaN();
        return string_to_number(string_value(doc, n.front()));
    }
    case 1: return std::get<1>(v);
    case 2: return string_to_number(std::get<2>(v));
    default: return std::get<3>(v) ? 1.0 : 0.0;
    }
}

CowString to_string(const DocumentData& doc, const Value& v) {
    switch (v.index()) {
    case 0: {
        const NodeSet& n = std::get<0>(v);
        if (n.empty()) return CowString();
        return string_value(doc, n.front());
    }
    case 1: return CowString(number_to_string(std::get<1>(v)));
    case 2: return CowString(std::string_view(std::get<2>(v)));
    default: return CowString(std::get<3>(v) ? "true" : "false");
    }
}

std::string into_string(const DocumentData& doc, Value&& v) {
    if (v.index() == 2) return std::move(std::get<2>(v));
    return to_string(doc, v).str();
}

bool in_document_order(const DocumentData& doc, const NodeSet& nodes) {
    for (std::size_t i = 1; i < nodes.size(); ++i)
        if (!(nodes[i - 1].key(doc) < nodes[i].key(doc))) return false;
    return true;
}

/// Puts the nodes in document order without duplicates.
NodeSet sort_unique(const DocumentData& doc, NodeSet nodes) {
    if (nodes.size() < 2 || in_document_order(doc, nodes)) return nodes;
    std::stable_sort(nodes.begin(), nodes.end(),
                     [&](const XNode& a, const XNode& b) { return a.key(doc) < b.key(doc); });
    nodes.erase(std::unique(nodes.begin(), nodes.end(),
                            [&](const XNode& a, const XNode& b) { return a.key(doc) == b.key(doc); }),
                nodes.end());
    return nodes;
}

// ----- the compiled expression -----

enum class CompareOp : unsigned char { eq, neq, lt, le, gt, ge };
enum class ArithOp : unsigned char { add, sub, mul, div, mod };
enum class Axis : unsigned char {
    child,
    descendant,
    descendant_or_self,
    self,
    parent,
    ancestor,
    ancestor_or_self,
    following_sibling,
    preceding_sibling,
    following,
    preceding,
    attribute,
};
enum class Func : unsigned char {
    last,
    position,
    count,
    name,
    local_name,
    string,
    concat,
    starts_with,
    ends_with,
    contains,
    substring_before,
    substring_after,
    substring,
    string_length,
    normalize_space,
    translate,
    lower_case,
    upper_case,
    not_,
    true_,
    false_,
    boolean,
    number,
    sum,
    floor,
    ceiling,
    round,
};

struct FunctionInfo {
    std::string_view name;
    Func func;
    std::size_t min;
    std::size_t max;
    XType type;
};

constexpr std::size_t unbounded = std::numeric_limits<std::size_t>::max();

constexpr FunctionInfo functions[] = {
    {"last", Func::last, 0, 0, XType::number},
    {"position", Func::position, 0, 0, XType::number},
    {"count", Func::count, 1, 1, XType::number},
    {"name", Func::name, 0, 1, XType::string},
    {"local-name", Func::local_name, 0, 1, XType::string},
    {"string", Func::string, 0, 1, XType::string},
    {"concat", Func::concat, 2, unbounded, XType::string},
    {"starts-with", Func::starts_with, 2, 2, XType::boolean},
    {"ends-with", Func::ends_with, 2, 2, XType::boolean},
    {"contains", Func::contains, 2, 2, XType::boolean},
    {"substring-before", Func::substring_before, 2, 2, XType::string},
    {"substring-after", Func::substring_after, 2, 2, XType::string},
    {"substring", Func::substring, 2, 3, XType::string},
    {"string-length", Func::string_length, 0, 1, XType::number},
    {"normalize-space", Func::normalize_space, 0, 1, XType::string},
    {"translate", Func::translate, 3, 3, XType::string},
    {"lower-case", Func::lower_case, 1, 1, XType::string},
    {"upper-case", Func::upper_case, 1, 1, XType::string},
    {"not", Func::not_, 1, 1, XType::boolean},
    {"true", Func::true_, 0, 0, XType::boolean},
    {"false", Func::false_, 0, 0, XType::boolean},
    {"boolean", Func::boolean, 1, 1, XType::boolean},
    {"number", Func::number, 0, 1, XType::number},
    {"sum", Func::sum, 1, 1, XType::number},
    {"floor", Func::floor, 1, 1, XType::number},
    {"ceiling", Func::ceiling, 1, 1, XType::number},
    {"round", Func::round, 1, 1, XType::number},
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Step {
    Axis axis = Axis::child;
    NodeTest test;
    std::vector<ExprPtr> predicates;

    bool is_reverse() const {
        return axis == Axis::ancestor || axis == Axis::ancestor_or_self || axis == Axis::preceding ||
               axis == Axis::preceding_sibling;
    }
    bool has_positional_predicates() const;
};

enum class ExprKind : unsigned char {
    literal,
    number,
    logic,
    compare,
    arith,
    negate,
    union_,
    attribute_equals, // @name = 'literal', compared in place without building a node-set
    attribute_exists, // [@name]
    path,             // a location path, absolute or relative, optionally starting from a filter expression
    filter,           // a primary expression with predicates: (//li)[last()]
    function,
};

struct Expr {
    ExprKind kind;
    std::string text;   // literal; attribute name
    std::string value;  // attribute_equals value
    double number = 0;
    bool flag = false;  // logic: and; attribute_equals: negate; path: absolute
    CompareOp compare_op = CompareOp::eq;
    ArithOp arith_op = ArithOp::add;
    Func func = Func::last;
    XType func_type = XType::number;
    ExprPtr left, right; // operands; negate: left; path: start (may be null); filter: primary
    std::vector<ExprPtr> args;       // function arguments; filter predicates
    std::vector<Step> steps;         // path

    explicit Expr(ExprKind k) : kind(k) {}

    XType type() const {
        switch (kind) {
        case ExprKind::literal: return XType::string;
        case ExprKind::number:
        case ExprKind::arith:
        case ExprKind::negate: return XType::number;
        case ExprKind::logic:
        case ExprKind::compare:
        case ExprKind::attribute_equals:
        case ExprKind::attribute_exists: return XType::boolean;
        case ExprKind::union_:
        case ExprKind::path:
        case ExprKind::filter: return XType::node_set;
        case ExprKind::function: return func_type;
        }
        return XType::number;
    }

    /// Whether position() or last() is used in the current context (not inside nested predicates).
    bool uses_position() const {
        switch (kind) {
        case ExprKind::logic:
        case ExprKind::compare:
        case ExprKind::arith:
        case ExprKind::union_: return left->uses_position() || right->uses_position();
        case ExprKind::negate: return left->uses_position();
        case ExprKind::path: return left && left->uses_position();
        case ExprKind::filter: return left->uses_position();
        case ExprKind::function:
            if (func == Func::position || func == Func::last) return true;
            for (const ExprPtr& a : args)
                if (a->uses_position()) return true;
            return false;
        default: return false;
        }
    }

    /// Whether the predicate depends on the position of the node (a number, position(), last()).
    bool is_positional() const { return type() == XType::number || uses_position(); }
};

bool Step::has_positional_predicates() const {
    for (const ExprPtr& p : predicates)
        if (p->is_positional()) return true;
    return false;
}

} // namespace

/// The compiled expression (declared in the public header).
struct XPathExpr {
    ExprPtr root;
};

namespace {

// ----- evaluation -----

struct EvalState {
    const DocumentData& doc;
    std::pmr::memory_resource* mr;
    std::optional<NodeSet> all; // every element, comment and text node in document order

    const NodeSet& all_nodes() {
        if (!all) {
            all.emplace(mr);
            add_descendants(doc, XNode::root(), NodeTest::any_node(), *all);
        }
        return *all;
    }
};

struct Ctx {
    XNode node;
    std::size_t position;
    std::size_t size;
    EvalState* state;
};

Value eval(const Expr& e, const Ctx& c);
bool eval_bool(const Expr& e, const Ctx& c);
NodeSet filter(NodeSet nodes, const std::vector<ExprPtr>& predicates, EvalState& state);

NodeSet eval_nodes(const Expr& e, const Ctx& c) {
    Value v = eval(e, c);
    if (!is_nodes(v)) throw std::logic_error("fasthtmlparser: the XPath expression does not give a node-set");
    return std::move(std::get<0>(v));
}

const std::string* attribute_name(const Expr& e);

std::string eval_string(const Expr& e, const Ctx& c) {
    // @name in a string context: the value read in place, without a node-set
    if (const std::string* name = attribute_name(e)) {
        std::string_view raw;
        if (!attribute_of(c.state->doc, c.node, *name, raw)) return {};
        return decode(raw, true).str();
    }
    return into_string(c.state->doc, eval(e, c));
}

double eval_number(const Expr& e, const Ctx& c) {
    if (e.kind == ExprKind::number) return e.number;
    return to_number(c.state->doc, eval(e, c));
}

bool compare_atoms(const DocumentData& doc, CompareOp op, const Value& a, const Value& b) {
    if (op == CompareOp::eq || op == CompareOp::neq) {
        bool eq;
        if (is_bool(a) || is_bool(b))
            eq = to_boolean(a) == to_boolean(b);
        else if (is_number(a) || is_number(b))
            eq = to_number(doc, a) == to_number(doc, b);
        else
            eq = to_string(doc, a) == to_string(doc, b);
        return eq == (op == CompareOp::eq);
    }
    double x = to_number(doc, a);
    double y = to_number(doc, b);
    switch (op) {
    case CompareOp::lt: return x < y;
    case CompareOp::le: return x <= y;
    case CompareOp::gt: return x > y;
    default: return x >= y;
    }
}

bool compare_with_nodes(const DocumentData& doc, CompareOp op, const NodeSet& nodes, const Value& other,
                        bool nodes_on_left) {
    if (is_bool(other)) {
        Value as_bool = !nodes.empty();
        return nodes_on_left ? compare_atoms(doc, op, as_bool, other) : compare_atoms(doc, op, other, as_bool);
    }
    bool number = is_number(other);
    for (const XNode& x : nodes) {
        CowString s = string_value(doc, x);
        Value value = number ? Value(string_to_number(s)) : Value(std::move(s).str());
        if (nodes_on_left ? compare_atoms(doc, op, value, other) : compare_atoms(doc, op, other, value)) return true;
    }
    return false;
}

bool compare(const DocumentData& doc, CompareOp op, const Value& a, const Value& b) {
    if (is_nodes(a) && is_nodes(b)) {
        const NodeSet& set_a = std::get<0>(a);
        const NodeSet& set_b = std::get<0>(b);
        if (set_a.empty() || set_b.empty()) return false;
        std::vector<std::string> values_b;
        values_b.reserve(set_b.size());
        for (const XNode& x : set_b) values_b.push_back(string_value(doc, x).str());
        for (const XNode& x : set_a) {
            Value value = string_value(doc, x).str();
            for (const std::string& other : values_b)
                if (compare_atoms(doc, op, value, Value(other))) return true;
        }
        return false;
    }
    if (is_nodes(a)) return compare_with_nodes(doc, op, std::get<0>(a), b, true);
    if (is_nodes(b)) return compare_with_nodes(doc, op, std::get<0>(b), a, false);
    return compare_atoms(doc, op, a, b);
}

/// A number compares with the position, anything else is converted to boolean.
bool eval_predicate(const Expr& e, const Ctx& c) {
    if (e.type() == XType::boolean) return eval_bool(e, c);
    Value v = eval(e, c);
    if (is_number(v)) return std::get<1>(v) == static_cast<double>(c.position);
    return to_boolean(v);
}

/// Applies the predicates one after another; positions are 1-based in the order of the list.
NodeSet filter(NodeSet nodes, const std::vector<ExprPtr>& predicates, EvalState& state) {
    for (const ExprPtr& p : predicates) {
        std::size_t size = nodes.size();
        NodeSet kept(nodes.get_allocator());
        kept.reserve(size);
        for (std::size_t i = 0; i < size; ++i)
            if (eval_predicate(*p, Ctx{nodes[i], i + 1, size, &state})) kept.push_back(nodes[i]);
        nodes = std::move(kept);
    }
    return nodes;
}

void collect_siblings(const DocumentData& doc, const XNode& x, const NodeTest& test, NodeSet& out, bool following) {
    if (x.kind == XKind::root || x.kind == XKind::attribute) return;
    if (!test.matches_text() && x.kind != XKind::text) {
        // tags only: walk the flat list; deeper tags are inside the siblings, a shallower one ends the parent
        std::size_t index = static_cast<std::size_t>(x.tag);
        std::int32_t depth = doc.nodes[index].depth;
        const auto& nodes = doc.nodes;
        auto visit = [&](std::size_t i) {
            const NodeData& item = nodes[i];
            if (item.depth < depth) return false;
            if (item.depth == depth && test.matches_tag(doc, i)) out.push_back(XNode::of_tag(doc, i));
            return true;
        };
        if (following) {
            for (std::size_t i = index + 1; i < nodes.size(); ++i)
                if (!visit(i)) break;
        } else {
            for (std::size_t i = index; i > 0;)
                if (!visit(--i)) break;
        }
        return;
    }

    std::optional<XNode> parent = parent_node(doc, x);
    if (!parent) return;
    NodeSet siblings(out.get_allocator());
    add_children(doc, *parent, NodeTest::any_node(), siblings);
    std::int64_t key = x.key(doc);
    auto it = std::find_if(siblings.begin(), siblings.end(), [&](const XNode& s) { return s.key(doc) == key; });
    if (it == siblings.end()) return;
    std::size_t index = static_cast<std::size_t>(it - siblings.begin());
    if (following) {
        for (std::size_t k = index + 1; k < siblings.size(); ++k)
            if (test.matches(doc, siblings[k], false)) out.push_back(siblings[k]);
    } else {
        for (std::size_t k = index; k > 0;) {
            --k;
            if (test.matches(doc, siblings[k], false)) out.push_back(siblings[k]);
        }
    }
}

void collect_following(const XNode& x, const NodeTest& test, NodeSet& out, EvalState& state) {
    const DocumentData& doc = state.doc;
    if (x.kind == XKind::root) return;
    std::int64_t after = x.range_end(doc);
    if (test.matches_text()) {
        for (const XNode& n : state.all_nodes())
            if (n.range_start(doc) > after && test.matches(doc, n, false)) out.push_back(n);
        return;
    }
    std::size_t first = x.kind == XKind::element || x.kind == XKind::comment ? static_cast<std::size_t>(x.tag) + 1 : 0;
    for (std::size_t i = first; i < doc.nodes.size(); ++i)
        if (doc.nodes[i].start > after && test.matches_tag(doc, i)) out.push_back(XNode::of_tag(doc, i));
}

void collect_preceding(const XNode& x, const NodeTest& test, NodeSet& out, EvalState& state) {
    const DocumentData& doc = state.doc;
    if (x.kind == XKind::root) return;
    std::int64_t before = x.range_start(doc);
    if (test.matches_text()) {
        const NodeSet& all = state.all_nodes();
        for (std::size_t k = all.size(); k > 0;) {
            const XNode& n = all[--k];
            if (n.range_end(doc) < before && test.matches(doc, n, false)) out.push_back(n);
        }
        return;
    }
    std::size_t last = x.kind == XKind::element || x.kind == XKind::comment ? static_cast<std::size_t>(x.tag)
                                                                             : doc.nodes.size();
    for (std::size_t i = last; i > 0;) {
        --i;
        if (doc.nodes[i].end < before && test.matches_tag(doc, i)) out.push_back(XNode::of_tag(doc, i));
    }
}

/// Appends the nodes of the axis that pass the node test, in axis order (reverse axes: nearest first).
void collect(const Step& step, const XNode& x, NodeSet& out, EvalState& state) {
    const DocumentData& doc = state.doc;
    const NodeTest& test = step.test;
    switch (step.axis) {
    case Axis::child: add_children(doc, x, test, out); break;
    case Axis::descendant: add_descendants(doc, x, test, out); break;
    case Axis::descendant_or_self:
        if (test.matches(doc, x, false)) out.push_back(x);
        add_descendants(doc, x, test, out);
        break;
    case Axis::self:
        if (test.matches(doc, x, false)) out.push_back(x);
        break;
    case Axis::parent:
        if (std::optional<XNode> p = parent_node(doc, x))
            if (test.matches(doc, *p, false)) out.push_back(*p);
        break;
    case Axis::ancestor:
    case Axis::ancestor_or_self: {
        XNode current = x;
        if (step.axis == Axis::ancestor_or_self && test.matches(doc, current, false)) out.push_back(current);
        while (std::optional<XNode> p = parent_node(doc, current)) {
            current = *p;
            if (test.matches(doc, current, false)) out.push_back(current);
        }
        break;
    }
    case Axis::following_sibling:
    case Axis::preceding_sibling:
        collect_siblings(doc, x, test, out, step.axis == Axis::following_sibling);
        break;
    case Axis::following: collect_following(x, test, out, state); break;
    case Axis::preceding: collect_preceding(x, test, out, state); break;
    case Axis::attribute: add_attributes(doc, x, test, out); break;
    }
}

/// Appends the nodes of the axis that pass the test and the predicates, in document order.
void select_from(const Step& step, const XNode& x, NodeSet& out, EvalState& state) {
    std::size_t start = out.size();
    collect(step, x, out, state);
    if (!step.predicates.empty() || step.is_reverse()) {
        NodeSet tail(out.begin() + static_cast<std::ptrdiff_t>(start), out.end(), out.get_allocator());
        out.resize(start);
        NodeSet selected = filter(std::move(tail), step.predicates, state);
        if (step.is_reverse()) std::reverse(selected.begin(), selected.end());
        out.insert(out.end(), selected.begin(), selected.end());
    }
}

/// The descendant axes (tags only) on input nodes in document order. A tag that lies in the part of the tag list
/// already scanned for an earlier input node is skipped: that scan has tested it and every tag its own scan would
/// test.
NodeSet select_descendants_of_nested(const Step& step, const NodeSet& input, EvalState& state) {
    const auto& nodes = state.doc.nodes;
    NodeSet out(state.mr);
    std::size_t scanned_to = 0; // the tags before this index were scanned for an earlier input node
    bool scanned_any = false;
    for (const XNode& x : input) {
        if (x.kind == XKind::element || x.kind == XKind::comment) {
            std::size_t index = static_cast<std::size_t>(x.tag);
            if (scanned_any && index < scanned_to) continue;
            select_from(step, x, out, state);
            // where add_descendants stops for this tag
            const NodeData& t = nodes[index];
            if (x.kind == XKind::element && t.inner_start > 0 && t.inner_end > 0) {
                std::size_t k = index + 1;
                while (k < nodes.size() && nodes[k].end <= t.end) ++k;
                scanned_to = scanned_any ? std::max(scanned_to, k) : k;
                scanned_any = true;
            }
        } else {
            select_from(step, x, out, state);
            if (x.kind == XKind::root) {
                scanned_to = std::numeric_limits<std::size_t>::max();
                scanned_any = true;
            }
        }
    }
    return out;
}

/// The walk up from an input node stops at an ancestor already reached from an earlier one, because that walk has
/// gone on to the root. Only the root and elements are marked: they are the only parents.
NodeSet select_shared_ancestors(const Step& step, const NodeSet& input, EvalState& state) {
    const DocumentData& doc = state.doc;
    std::unordered_set<std::int64_t> visited;
    NodeSet out(state.mr);
    for (const XNode& x : input) {
        std::size_t start = out.size();
        XNode current = x;
        if (step.axis == Axis::ancestor_or_self) {
            if ((current.kind == XKind::root || current.kind == XKind::element) &&
                !visited.insert(current.key(doc)).second)
                continue;
            if (step.test.matches(doc, current, false)) out.push_back(current);
        }
        while (std::optional<XNode> p = parent_node(doc, current)) {
            if (!visited.insert(p->key(doc)).second) break;
            current = *p;
            if (step.test.matches(doc, current, false)) out.push_back(current);
        }
        if (!step.predicates.empty() && out.size() > start) {
            NodeSet tail(out.begin() + static_cast<std::ptrdiff_t>(start), out.end(), out.get_allocator());
            out.resize(start);
            NodeSet kept = filter(std::move(tail), step.predicates, state);
            out.insert(out.end(), kept.begin(), kept.end());
        }
    }
    return out;
}

NodeSet apply(const Step& step, NodeSet input, EvalState& state) {
    const DocumentData& doc = state.doc;
    if (input.size() == 1) {
        NodeSet out(state.mr);
        select_from(step, input[0], out, state);
        return out;
    }

    // Nested input nodes (//div//a on divs inside divs, //a/ancestor::* on links in one list) give the same nodes
    // again and again: n nested nodes would collect about n² nodes before the duplicates are removed. When the
    // predicates do not depend on the position, a node passes them or not whichever input node it comes from, so the
    // input nodes whose results are already collected are skipped: the work stays linear.
    if (!step.has_positional_predicates()) {
        switch (step.axis) {
        case Axis::descendant:
        case Axis::descendant_or_self:
            if (!step.test.matches_text() && in_document_order(doc, input))
                return sort_unique(doc, select_descendants_of_nested(step, input, state));
            break;
        case Axis::ancestor:
        case Axis::ancestor_or_self: return sort_unique(doc, select_shared_ancestors(step, input, state));
        default: break;
        }
    }

    NodeSet result(state.mr);
    for (const XNode& x : input) select_from(step, x, result, state);
    return sort_unique(doc, std::move(result));
}

double round_half_up(double v) {
    if (std::isnan(v) || std::isinf(v)) return v;
    return std::floor(v + 0.5);
}

std::u32string to_chars32(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        Utf8Char ch = decode_utf8(s, i);
        out.push_back(ch.c);
        i += ch.size;
    }
    return out;
}

std::string from_chars32(const std::u32string& s, std::size_t from, std::size_t to) {
    std::string out;
    for (std::size_t i = from; i < to; ++i) append_utf8(out, s[i]);
    return out;
}

/// XPath 1.0: the chars at 1-based positions p with round(start) <= p < round(start) + round(length).
std::string substring(const std::vector<ExprPtr>& args, const Ctx& c) {
    std::u32string s = to_chars32(eval_string(*args[0], c));
    double from = round_half_up(eval_number(*args[1], c));
    double to = args.size() > 2 ? from + round_half_up(eval_number(*args[2], c))
                                : std::numeric_limits<double>::infinity();
    if (std::isnan(from) || std::isnan(to)) return {};
    double n = static_cast<double>(s.size());
    std::size_t first = from <= 1.0 ? 0 : from > n ? s.size() : static_cast<std::size_t>(from) - 1;
    std::size_t end = to >= n + 1.0 ? s.size() : to <= 1.0 ? 0 : static_cast<std::size_t>(to) - 1;
    if (end > first) return from_chars32(s, first, end);
    return {};
}

std::string normalize_space(std::string_view s) {
    std::string out;
    std::size_t i = 0;
    bool pending = false;
    while (i < s.size()) {
        bool space;
        Utf8Char ch = space_at(s, i, space);
        if (space) {
            pending = !out.empty();
        } else {
            if (pending) out.push_back(' ');
            pending = false;
            out.append(s.substr(i, ch.size));
        }
        i += ch.size;
    }
    return out;
}

Value call(const Expr& e, const Ctx& c) {
    const DocumentData& doc = c.state->doc;
    const std::vector<ExprPtr>& args = e.args;
    auto arg = [&](std::size_t i) { return eval_string(*args[i], c); };
    auto context_or_arg = [&]() { return args.empty() ? string_value(doc, c.node).str() : arg(0); };
    switch (e.func) {
    case Func::last: return static_cast<double>(c.size);
    case Func::position: return static_cast<double>(c.position);
    case Func::count: return static_cast<double>(eval_nodes(*args[0], c).size());
    case Func::name:
    case Func::local_name: {
        std::string name;
        if (args.empty()) {
            name = node_name(doc, c.node);
        } else {
            NodeSet set = eval_nodes(*args[0], c);
            if (!set.empty()) name = node_name(doc, set.front());
        }
        if (e.func == Func::local_name) {
            std::size_t colon = name.rfind(':');
            if (colon != std::string::npos) return name.substr(colon + 1);
        }
        return name;
    }
    case Func::string: return context_or_arg();
    case Func::concat: {
        std::string out;
        for (const ExprPtr& a : args) out += eval_string(*a, c);
        return out;
    }
    case Func::starts_with: {
        std::string s = arg(0), t = arg(1);
        return s.compare(0, t.size(), t) == 0 && s.size() >= t.size();
    }
    case Func::ends_with: {
        std::string s = arg(0), t = arg(1);
        return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
    }
    case Func::contains: {
        std::string s = arg(0), t = arg(1);
        return s.find(t) != std::string::npos;
    }
    case Func::substring_before: {
        std::string s = arg(0), t = arg(1);
        std::size_t i = s.find(t);
        return i == std::string::npos ? std::string() : s.substr(0, i);
    }
    case Func::substring_after: {
        std::string s = arg(0), t = arg(1);
        std::size_t i = s.find(t);
        return i == std::string::npos ? std::string() : s.substr(i + t.size());
    }
    case Func::substring: return substring(args, c);
    case Func::string_length: return static_cast<double>(count_utf16(context_or_arg()));
    case Func::normalize_space: return normalize_space(context_or_arg());
    case Func::translate: {
        std::u32string s = to_chars32(arg(0));
        std::u32string from = to_chars32(arg(1));
        std::u32string to = to_chars32(arg(2));
        std::string b;
        for (char32_t ch : s) {
            std::size_t i = from.find(ch);
            if (i == std::u32string::npos)
                append_utf8(b, ch);
            else if (i < to.size())
                append_utf8(b, to[i]);
        }
        return b;
    }
    case Func::lower_case: return to_lower(arg(0));
    case Func::upper_case: return to_upper(arg(0));
    case Func::not_: return !eval_bool(*args[0], c);
    case Func::true_: return true;
    case Func::false_: return false;
    case Func::boolean: return eval_bool(*args[0], c);
    case Func::number:
        if (args.empty()) return string_to_number(string_value(doc, c.node));
        return eval_number(*args[0], c);
    case Func::sum: {
        double sum = 0;
        for (const XNode& x : eval_nodes(*args[0], c)) sum += string_to_number(string_value(doc, x));
        return sum;
    }
    case Func::floor: return std::floor(eval_number(*args[0], c));
    case Func::ceiling: return std::ceil(eval_number(*args[0], c));
    case Func::round: return round_half_up(eval_number(*args[0], c));
    }
    return 0.0;
}

Value eval(const Expr& e, const Ctx& c) {
    const DocumentData& doc = c.state->doc;
    switch (e.kind) {
    case ExprKind::literal: return e.text;
    case ExprKind::number: return e.number;
    case ExprKind::logic:
    case ExprKind::compare:
    case ExprKind::attribute_equals:
    case ExprKind::attribute_exists: return eval_bool(e, c);
    case ExprKind::arith: {
        double x = eval_number(*e.left, c);
        double y = eval_number(*e.right, c);
        switch (e.arith_op) {
        case ArithOp::add: return x + y;
        case ArithOp::sub: return x - y;
        case ArithOp::mul: return x * y;
        case ArithOp::div: return x / y;
        case ArithOp::mod: return std::fmod(x, y);
        }
        return 0.0;
    }
    case ExprKind::negate: return -eval_number(*e.left, c);
    case ExprKind::union_: {
        NodeSet result = eval_nodes(*e.left, c);
        NodeSet right = eval_nodes(*e.right, c);
        result.insert(result.end(), right.begin(), right.end());
        return sort_unique(doc, std::move(result));
    }
    case ExprKind::path: {
        NodeSet current(c.state->mr);
        if (e.left)
            current = eval_nodes(*e.left, c);
        else if (e.flag)
            current.push_back(XNode::root());
        else
            current.push_back(c.node);
        for (const Step& s : e.steps) {
            if (current.empty()) break;
            current = apply(s, std::move(current), *c.state);
        }
        return current;
    }
    case ExprKind::filter: return filter(eval_nodes(*e.left, c), e.args, *c.state);
    case ExprKind::function: return call(e, c);
    }
    return false;
}

bool eval_bool(const Expr& e, const Ctx& c) {
    const DocumentData& doc = c.state->doc;
    switch (e.kind) {
    case ExprKind::literal: return !e.text.empty();
    case ExprKind::logic:
        return e.flag ? eval_bool(*e.left, c) && eval_bool(*e.right, c)
                      : eval_bool(*e.left, c) || eval_bool(*e.right, c);
    case ExprKind::compare: return compare(doc, e.compare_op, eval(*e.left, c), eval(*e.right, c));
    case ExprKind::attribute_equals: {
        // '!=' is true when the attribute exists and differs (a missing attribute is an empty node-set)
        std::string_view raw;
        if (!attribute_of(doc, c.node, e.text, raw)) return false;
        return attr_equals(raw, e.value) != e.flag;
    }
    case ExprKind::attribute_exists: {
        std::string_view raw;
        return attribute_of(doc, c.node, e.text, raw);
    }
    default: return to_boolean(eval(e, c));
    }
}

// ----- tokenizer and compiler -----

enum class T : unsigned char {
    end,
    slash,
    double_slash,
    lparen,
    rparen,
    lbracket,
    rbracket,
    dot,
    dot_dot,
    at,
    comma,
    colon_colon,
    pipe,
    plus,
    minus,
    eq,
    neq,
    lt,
    le,
    gt,
    ge,
    star,     // name test *
    multiply, // operator *
    and_,
    or_,
    div,
    mod,
    name,
    literal,
    number,
    dollar,
};

struct Token {
    T kind = T::end;
    std::string text;
    double number = 0;
    std::size_t position = 0; // in chars
};

struct CompileError {
    std::string message;
};

bool is_name_start(char32_t c) { return is_alphabetic(c) || c == '_'; }
bool is_name_char(char32_t c) { return is_alphabetic(c) || is_numeric(c) || c == '_' || c == '-' || c == '.'; }

bool is_node_type(std::string_view n) {
    return n == "node" || n == "text" || n == "comment" || n == "processing-instruction";
}

std::string quoted(std::string_view s) {
    std::string out = "\"";
    for (char ch : s) {
        if (ch == '"' || ch == '\\') out.push_back('\\');
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

bool can_start_step(const Token& t) {
    return t.kind == T::dot || t.kind == T::dot_dot || t.kind == T::at || t.kind == T::star || t.kind == T::name;
}

/// Whether the expression is a bare @name (one attribute step, no predicates); its name.
const std::string* attribute_name(const Expr& e) {
    if (e.kind != ExprKind::path || e.left || e.flag || e.steps.size() != 1) return nullptr;
    const Step& s = e.steps[0];
    if (s.axis != Axis::attribute || !s.predicates.empty() || s.test.kind != TestKind::name) return nullptr;
    return &s.test.name;
}

/// //x is descendant-or-self::node()/child::x. Without positional predicates on x it is the same as descendant::x:
/// one linear scan of the tag list instead of the children of every node.
std::vector<Step> optimize(std::vector<Step> steps) {
    for (std::size_t i = 0; i + 1 < steps.size(); ++i) {
        const Step& s = steps[i];
        if (s.axis != Axis::descendant_or_self || s.test.kind != TestKind::node || !s.predicates.empty()) continue;
        Step& next = steps[i + 1];
        if (next.axis == Axis::child && !next.has_positional_predicates()) {
            Step merged;
            merged.axis = Axis::descendant;
            merged.test = std::move(next.test);
            merged.predicates = std::move(next.predicates);
            steps[i] = std::move(merged);
            steps.erase(steps.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        } else if (next.axis == Axis::child || next.axis == Axis::attribute) {
            // only the root and elements have children or attributes: skip the text nodes
            Step container;
            container.axis = Axis::descendant_or_self;
            container.test = NodeTest(TestKind::container, "");
            steps[i] = std::move(container);
        }
    }
    return steps;
}

class Compiler {
public:
    explicit Compiler(std::string_view s) : text_(s), source_(to_chars32(s)) { tokenize(); }

    ExprPtr compile() {
        ExprPtr e = parse_expr();
        if (peek(0).kind != T::end) fail("unexpected " + describe(peek(0)));
        return e;
    }

private:
    std::string_view text_;
    std::u32string source_;
    std::vector<Token> tokens_;
    std::size_t index_ = 0;

    [[noreturn]] void fail_at(const std::string& msg, std::size_t position) const {
        throw CompileError{"invalid XPath " + quoted(text_) + " at position " + std::to_string(position) + ": " + msg};
    }
    [[noreturn]] void fail(const std::string& msg) const { fail_at(msg, peek(0).position); }

    void tokenize() {
        const std::u32string& s = source_;
        auto at = [&](std::size_t k) -> char32_t { return k < s.size() ? s[k] : 0; };
        std::size_t i = 0;
        for (;;) {
            while (i < s.size() && is_whitespace(s[i])) ++i;
            if (i >= s.size()) {
                Token end;
                end.position = i;
                tokens_.push_back(std::move(end));
                return;
            }
            std::size_t start = i;
            char32_t ch = s[i];
            char32_t next = at(i + 1);

            // XPath 1.0 lexical rule: after a token that can end an operand, '*' and and/or/div/mod are operators
            bool operator_expected = false;
            if (!tokens_.empty()) {
                T k = tokens_.back().kind;
                operator_expected = k == T::rparen || k == T::rbracket || k == T::dot || k == T::dot_dot ||
                                    k == T::star || k == T::name || k == T::literal || k == T::number;
            }

            Token tok;
            tok.position = start;
            auto one = [&](T kind, std::size_t len) {
                tok.kind = kind;
                i += len;
            };
            switch (ch) {
            case '/': next == '/' ? one(T::double_slash, 2) : one(T::slash, 1); break;
            case '(': one(T::lparen, 1); break;
            case ')': one(T::rparen, 1); break;
            case '[': one(T::lbracket, 1); break;
            case ']': one(T::rbracket, 1); break;
            case '@': one(T::at, 1); break;
            case ',': one(T::comma, 1); break;
            case '|': one(T::pipe, 1); break;
            case '+': one(T::plus, 1); break;
            case '-': one(T::minus, 1); break;
            case '=': one(T::eq, 1); break;
            case '$': one(T::dollar, 1); break;
            case '*': one(operator_expected ? T::multiply : T::star, 1); break;
            case '!':
                if (next != '=') fail_at("'!' must be followed by '='", i);
                one(T::neq, 2);
                break;
            case '<': next == '=' ? one(T::le, 2) : one(T::lt, 1); break;
            case '>': next == '=' ? one(T::ge, 2) : one(T::gt, 1); break;
            case ':':
                if (next != ':') fail_at("unexpected ':'", i);
                one(T::colon_colon, 2);
                break;
            case '"':
            case '\'': {
                std::size_t end = i + 1;
                while (end < s.size() && s[end] != ch) ++end;
                if (end >= s.size()) fail_at("unterminated string literal", i);
                tok.kind = T::literal;
                tok.text = from_chars32(s, i + 1, end);
                i = end + 1;
                break;
            }
            default:
                if (ch == '.' && next == '.') {
                    one(T::dot_dot, 2);
                } else if (ch == '.' && !(next < 0x80 && is_digit(static_cast<unsigned char>(next)))) {
                    one(T::dot, 1);
                } else if ((ch < 0x80 && is_digit(static_cast<unsigned char>(ch))) || ch == '.') {
                    auto digit = [&](std::size_t k) { return k < s.size() && s[k] < 0x80 && is_digit(static_cast<unsigned char>(s[k])); };
                    while (digit(i)) ++i;
                    if (i < s.size() && s[i] == '.') {
                        ++i;
                        while (digit(i)) ++i;
                    }
                    tok.kind = T::number;
                    std::string text = from_chars32(s, start, i);
                    if (text[0] == '.') text.insert(text.begin(), '0');
                    double v = 0;
                    std::from_chars(text.data(), text.data() + text.size(), v);
                    tok.number = v;
                } else if (is_name_start(ch)) {
                    ++i;
                    while (i < s.size()) {
                        if (is_name_char(s[i])) {
                            ++i;
                        } else if (s[i] == ':' && i + 1 < s.size() && s[i + 1] != ':' &&
                                   (is_name_start(s[i + 1]) || s[i + 1] == '*')) {
                            // a prefixed name (dc:creator) or prefix:*
                            i += 2;
                            if (s[i - 1] == '*') break;
                        } else {
                            break;
                        }
                    }
                    tok.kind = T::name;
                    tok.text = from_chars32(s, start, i);
                    if (operator_expected) {
                        if (tok.text == "and")
                            tok.kind = T::and_;
                        else if (tok.text == "or")
                            tok.kind = T::or_;
                        else if (tok.text == "div")
                            tok.kind = T::div;
                        else if (tok.text == "mod")
                            tok.kind = T::mod;
                        else
                            fail_at("expected an operator, found '" + tok.text + "'", start);
                    }
                } else {
                    std::string c;
                    append_utf8(c, ch);
                    fail_at("unexpected character '" + c + "'", i);
                }
            }
            tokens_.push_back(std::move(tok));
        }
    }

    // ----- parser helpers -----

    const Token& peek(std::size_t offset) const { return tokens_[std::min(index_ + offset, tokens_.size() - 1)]; }

    Token advance() { return tokens_[index_++]; }

    bool accept(T kind) {
        if (peek(0).kind != kind) return false;
        ++index_;
        return true;
    }

    void expect(T kind, const char* what) {
        if (!accept(kind)) fail(std::string("expected ") + what + ", found " + describe(peek(0)));
    }

    std::string describe(const Token& t) const {
        switch (t.kind) {
        case T::end: return "the end of the expression";
        case T::name:
        case T::literal: return "'" + t.text + "'";
        case T::number: return number_to_string(t.number);
        default: {
            std::size_t end = std::min(t.position + 2, source_.size());
            std::string text = from_chars32(source_, t.position, end);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                                     text.back() == '\n'))
                text.pop_back();
            return "'" + text + "'";
        }
        }
    }

    static ExprPtr binary(ExprKind kind, ExprPtr left, ExprPtr right) {
        auto e = std::make_unique<Expr>(kind);
        e->left = std::move(left);
        e->right = std::move(right);
        return e;
    }

    // ----- grammar (XPath 1.0, section 3) -----

    ExprPtr parse_expr() { return parse_or(); }

    ExprPtr parse_or() {
        ExprPtr left = parse_and();
        while (accept(T::or_)) {
            ExprPtr right = parse_and();
            left = binary(ExprKind::logic, std::move(left), std::move(right));
            left->flag = false;
        }
        return left;
    }

    ExprPtr parse_and() {
        ExprPtr left = parse_equality();
        while (accept(T::and_)) {
            ExprPtr right = parse_equality();
            left = binary(ExprKind::logic, std::move(left), std::move(right));
            left->flag = true;
        }
        return left;
    }

    ExprPtr parse_equality() {
        ExprPtr left = parse_relational();
        for (;;) {
            CompareOp op;
            if (accept(T::eq))
                op = CompareOp::eq;
            else if (accept(T::neq))
                op = CompareOp::neq;
            else
                return left;
            ExprPtr right = parse_relational();
            left = make_equality(op, std::move(left), std::move(right));
        }
    }

    ExprPtr parse_relational() {
        ExprPtr left = parse_additive();
        for (;;) {
            CompareOp op;
            if (accept(T::lt))
                op = CompareOp::lt;
            else if (accept(T::le))
                op = CompareOp::le;
            else if (accept(T::gt))
                op = CompareOp::gt;
            else if (accept(T::ge))
                op = CompareOp::ge;
            else
                return left;
            ExprPtr right = parse_additive();
            left = binary(ExprKind::compare, std::move(left), std::move(right));
            left->compare_op = op;
        }
    }

    ExprPtr parse_additive() {
        ExprPtr left = parse_multiplicative();
        for (;;) {
            ArithOp op;
            if (accept(T::plus))
                op = ArithOp::add;
            else if (accept(T::minus))
                op = ArithOp::sub;
            else
                return left;
            ExprPtr right = parse_multiplicative();
            left = binary(ExprKind::arith, std::move(left), std::move(right));
            left->arith_op = op;
        }
    }

    ExprPtr parse_multiplicative() {
        ExprPtr left = parse_unary();
        for (;;) {
            ArithOp op;
            if (accept(T::multiply))
                op = ArithOp::mul;
            else if (accept(T::div))
                op = ArithOp::div;
            else if (accept(T::mod))
                op = ArithOp::mod;
            else
                return left;
            ExprPtr right = parse_unary();
            left = binary(ExprKind::arith, std::move(left), std::move(right));
            left->arith_op = op;
        }
    }

    ExprPtr parse_unary() {
        if (accept(T::minus)) {
            auto e = std::make_unique<Expr>(ExprKind::negate);
            e->left = parse_unary();
            return e;
        }
        return parse_union();
    }

    ExprPtr parse_union() {
        ExprPtr left = parse_path();
        while (peek(0).kind == T::pipe) {
            std::size_t position = advance().position;
            ExprPtr right = parse_path();
            if (left->type() != XType::node_set || right->type() != XType::node_set)
                fail_at("'|' needs node-sets on both sides", position);
            left = binary(ExprKind::union_, std::move(left), std::move(right));
        }
        return left;
    }

    ExprPtr parse_path() {
        const Token& t = peek(0);
        bool is_filter = t.kind == T::literal || t.kind == T::number || t.kind == T::lparen || t.kind == T::dollar ||
                         (t.kind == T::name && peek(1).kind == T::lparen && !is_node_type(t.text));
        if (!is_filter) return parse_location_path();

        std::size_t position = t.position;
        ExprPtr primary = parse_primary();
        std::vector<ExprPtr> predicates = parse_predicates();
        if (!predicates.empty()) {
            if (primary->type() != XType::node_set) fail_at("a predicate needs a node-set", position);
            auto f = std::make_unique<Expr>(ExprKind::filter);
            f->left = std::move(primary);
            f->args = std::move(predicates);
            primary = std::move(f);
        }
        T k = peek(0).kind;
        if (k != T::slash && k != T::double_slash) return primary;
        if (primary->type() != XType::node_set) fail_at("'/' needs a node-set on the left", position);
        auto path = std::make_unique<Expr>(ExprKind::path);
        path->left = std::move(primary);
        path->flag = false;
        path->steps = optimize(parse_relative_steps({}, true));
        return path;
    }

    ExprPtr parse_location_path() {
        std::vector<Step> steps;
        bool absolute = false;
        if (accept(T::slash)) {
            absolute = true;
            if (can_start_step(peek(0))) steps = parse_relative_steps(std::move(steps), false);
        } else if (peek(0).kind == T::double_slash) {
            absolute = true;
            steps = parse_relative_steps(std::move(steps), true);
        } else {
            if (!can_start_step(peek(0))) fail("unexpected " + describe(peek(0)));
            steps = parse_relative_steps(std::move(steps), false);
        }
        auto path = std::make_unique<Expr>(ExprKind::path);
        path->flag = absolute;
        path->steps = optimize(std::move(steps));
        return path;
    }

    /// Steps separated by '/' or '//'; after_slash: the next token is a '/' or '//' to read first.
    std::vector<Step> parse_relative_steps(std::vector<Step> steps, bool after_slash) {
        if (!after_slash) steps.push_back(parse_step());
        for (;;) {
            if (accept(T::slash)) {
                steps.push_back(parse_step());
            } else if (accept(T::double_slash)) {
                Step any;
                any.axis = Axis::descendant_or_self;
                any.test = NodeTest::any_node();
                steps.push_back(std::move(any));
                steps.push_back(parse_step());
            } else {
                return steps;
            }
        }
    }

    Step parse_step() {
        Step step;
        if (accept(T::dot)) {
            step.axis = Axis::self;
            step.test = NodeTest::any_node();
            return step;
        }
        if (accept(T::dot_dot)) {
            step.axis = Axis::parent;
            step.test = NodeTest::any_node();
            return step;
        }
        step.axis = Axis::child;
        if (accept(T::at)) {
            step.axis = Axis::attribute;
        } else if (peek(0).kind == T::name && peek(1).kind == T::colon_colon) {
            Token name = advance();
            const std::string& n = name.text;
            if (n == "child")
                step.axis = Axis::child;
            else if (n == "descendant")
                step.axis = Axis::descendant;
            else if (n == "descendant-or-self")
                step.axis = Axis::descendant_or_self;
            else if (n == "self")
                step.axis = Axis::self;
            else if (n == "parent")
                step.axis = Axis::parent;
            else if (n == "ancestor")
                step.axis = Axis::ancestor;
            else if (n == "ancestor-or-self")
                step.axis = Axis::ancestor_or_self;
            else if (n == "following-sibling")
                step.axis = Axis::following_sibling;
            else if (n == "preceding-sibling")
                step.axis = Axis::preceding_sibling;
            else if (n == "following")
                step.axis = Axis::following;
            else if (n == "preceding")
                step.axis = Axis::preceding;
            else if (n == "attribute")
                step.axis = Axis::attribute;
            else if (n == "namespace")
                fail_at("the namespace axis is not supported", name.position);
            else
                fail_at("unknown axis '" + n + "'", name.position);
            advance();
        }
        step.test = parse_node_test();
        step.predicates = parse_predicates();
        return step;
    }

    NodeTest parse_node_test() {
        if (accept(T::star)) return NodeTest(TestKind::wildcard, "");
        const Token& t = peek(0);
        if (t.kind != T::name) fail("expected a node test, found " + describe(t));
        std::string name = advance().text;
        if (peek(0).kind == T::lparen && is_node_type(name)) {
            advance();
            if (name == "processing-instruction") accept(T::literal);
            expect(T::rparen, "')'");
            if (name == "node") return NodeTest::any_node();
            if (name == "text") return NodeTest(TestKind::text, "");
            if (name == "comment") return NodeTest(TestKind::comment, "");
            return NodeTest(TestKind::pi, "");
        }
        if (name.size() >= 2 && name.back() == '*' && name[name.size() - 2] == ':')
            return NodeTest(TestKind::prefix, std::string_view(name).substr(0, name.size() - 1));
        return NodeTest(TestKind::name, name);
    }

    std::vector<ExprPtr> parse_predicates() {
        std::vector<ExprPtr> predicates;
        while (accept(T::lbracket)) {
            ExprPtr p = parse_expr();
            expect(T::rbracket, "']'");
            // [@name]: an existence check instead of a node-set
            if (const std::string* name = attribute_name(*p)) {
                auto exists = std::make_unique<Expr>(ExprKind::attribute_exists);
                exists->text = *name;
                p = std::move(exists);
            }
            predicates.push_back(std::move(p));
        }
        return predicates;
    }

    ExprPtr parse_primary() {
        Token t = advance();
        switch (t.kind) {
        case T::literal: {
            auto e = std::make_unique<Expr>(ExprKind::literal);
            e->text = std::move(t.text);
            return e;
        }
        case T::number: {
            auto e = std::make_unique<Expr>(ExprKind::number);
            e->number = t.number;
            return e;
        }
        case T::lparen: {
            ExprPtr e = parse_expr();
            expect(T::rparen, "')'");
            return e;
        }
        case T::dollar: fail_at("variables are not supported", t.position);
        default: break;
        }

        // function call
        const FunctionInfo* info = nullptr;
        for (const FunctionInfo& f : functions)
            if (f.name == t.text) info = &f;
        if (!info) fail_at("unknown function '" + t.text + "()'", t.position);
        expect(T::lparen, "'('");
        auto e = std::make_unique<Expr>(ExprKind::function);
        if (!accept(T::rparen)) {
            for (;;) {
                e->args.push_back(parse_expr());
                if (!accept(T::comma)) break;
            }
            expect(T::rparen, "')'");
        }
        if (e->args.size() < info->min || e->args.size() > info->max)
            fail_at("wrong number of arguments for " + t.text + "()", t.position);
        Func f = info->func;
        if ((f == Func::count || f == Func::sum || f == Func::name || f == Func::local_name) && !e->args.empty() &&
            e->args[0]->type() != XType::node_set)
            fail_at(t.text + "() needs a node-set argument", t.position);
        e->func = f;
        e->func_type = info->type;
        return e;
    }

    /// @name = 'literal' is compared in place; everything else by the general rules.
    static ExprPtr make_equality(CompareOp op, ExprPtr left, ExprPtr right) {
        const std::string* name = attribute_name(*left);
        const Expr* literal = right.get();
        if (!(name && literal->kind == ExprKind::literal)) {
            name = attribute_name(*right);
            literal = left.get();
        }
        if (name && literal->kind == ExprKind::literal) {
            auto e = std::make_unique<Expr>(ExprKind::attribute_equals);
            e->text = *name;
            e->value = literal->text;
            e->flag = op == CompareOp::neq;
            return e;
        }
        ExprPtr e = binary(ExprKind::compare, std::move(left), std::move(right));
        e->compare_op = op;
        return e;
    }
};

// ----- running -----

/// The arena of one evaluation: a buffer on the stack, then the heap.
struct Arena {
    alignas(std::max_align_t) std::byte buffer[8192];
    std::pmr::monotonic_buffer_resource resource{buffer, sizeof buffer};
};

Value run(const XPathExpr& x, const DocumentData& doc, const XNode& context, Arena& arena) {
    EvalState state{doc, &arena.resource, std::nullopt};
    return eval(*x.root, Ctx{context, 1, 1, &state});
}

XNode tag_node(const Node& n) { return XNode::of_tag(doc(n), n.index()); }

std::vector<Node> nodes_at(const XPathQuery& q, const XPathExpr& x, const DocumentData& doc, const XNode& context) {
    if (x.root->type() != XType::node_set)
        throw std::logic_error("fasthtmlparser: the XPath " + quoted(q.str()) + " gives a " +
                               type_name(x.root->type()) + ", not tags; use xpath_value");
    Arena arena;
    Value v = run(x, doc, context, arena);
    std::vector<Node> out;
    for (const XNode& n : std::get<0>(v))
        if (n.kind == XKind::element || n.kind == XKind::comment) out.push_back(doc.at(static_cast<std::size_t>(n.tag)));
    return out;
}

std::vector<std::string> values_at(const XPathExpr& x, const DocumentData& doc, const XNode& context) {
    Arena arena;
    Value v = run(x, doc, context, arena);
    std::vector<std::string> out;
    if (is_nodes(v)) {
        const NodeSet& set = std::get<0>(v);
        out.reserve(set.size());
        for (const XNode& n : set) out.push_back(string_value(doc, n).str());
    } else {
        out.push_back(to_string(doc, v).str());
    }
    return out;
}

std::optional<std::string> value_at(const XPathExpr& x, const DocumentData& doc, const XNode& context) {
    Arena arena;
    Value v = run(x, doc, context, arena);
    if (is_nodes(v)) {
        const NodeSet& set = std::get<0>(v);
        if (set.empty()) return std::nullopt;
        return string_value(doc, set.front()).str();
    }
    return to_string(doc, v).str();
}

QueryCache<XPathQuery>& xpath_cache() {
    static QueryCache<XPathQuery> cache;
    return cache;
}

const XPathQuery& cached_xpath(std::string_view expression, std::shared_ptr<const XPathQuery>& holder) {
    holder = xpath_cache().get(expression, [](std::string_view s) {
        Compiled<XPathQuery> c = XPathQuery::compile(s);
        if (!c) throw std::invalid_argument(c.error());
        return std::move(c).value();
    });
    return *holder;
}

} // namespace
} // namespace detail

Compiled<XPathQuery> XPathQuery::compile(std::string_view expression) {
    try {
        detail::Compiler c(expression);
        auto x = std::make_shared<detail::XPathExpr>();
        x->root = c.compile();
        return XPathQuery(std::string(expression), std::move(x));
    } catch (const detail::CompileError& e) {
        return Compiled<XPathQuery>::failure(e.message);
    }
}

bool XPathQuery::is_node_set() const noexcept { return expr_->root->type() == detail::XType::node_set; }

std::vector<Node> XPathQuery::nodes(const Document& doc) const {
    return detail::nodes_at(*this, *expr_, *doc.data(), detail::XNode::root());
}

std::vector<Node> XPathQuery::nodes_from(const Node& n) const {
    return detail::nodes_at(*this, *expr_, detail::doc(n), detail::tag_node(n));
}

std::vector<std::string> XPathQuery::values(const Document& doc) const {
    return detail::values_at(*expr_, *doc.data(), detail::XNode::root());
}

std::vector<std::string> XPathQuery::values_from(const Node& n) const {
    return detail::values_at(*expr_, detail::doc(n), detail::tag_node(n));
}

std::optional<std::string> XPathQuery::value(const Document& doc) const {
    return detail::value_at(*expr_, *doc.data(), detail::XNode::root());
}

std::optional<std::string> XPathQuery::value_from(const Node& n) const {
    return detail::value_at(*expr_, detail::doc(n), detail::tag_node(n));
}

std::vector<Node> Document::xpath(std::string_view expression) const {
    std::shared_ptr<const XPathQuery> q;
    return detail::cached_xpath(expression, q).nodes(*this);
}

std::optional<Node> Document::xpath_first(std::string_view expression) const {
    std::vector<Node> r = xpath(expression);
    if (r.empty()) return std::nullopt;
    return r.front();
}

std::vector<std::string> Document::xpath_values(std::string_view expression) const {
    std::shared_ptr<const XPathQuery> q;
    return detail::cached_xpath(expression, q).values(*this);
}

std::string Document::xpath_value(std::string_view expression) const {
    std::shared_ptr<const XPathQuery> q;
    return detail::cached_xpath(expression, q).value(*this).value_or(std::string());
}

std::vector<Node> Node::xpath(std::string_view expression) const {
    std::shared_ptr<const XPathQuery> q;
    return detail::cached_xpath(expression, q).nodes_from(*this);
}

std::optional<Node> Node::xpath_first(std::string_view expression) const {
    std::vector<Node> r = xpath(expression);
    if (r.empty()) return std::nullopt;
    return r.front();
}

std::vector<std::string> Node::xpath_values(std::string_view expression) const {
    std::shared_ptr<const XPathQuery> q;
    return detail::cached_xpath(expression, q).values_from(*this);
}

std::string Node::xpath_value(std::string_view expression) const {
    std::shared_ptr<const XPathQuery> q;
    return detail::cached_xpath(expression, q).value_from(*this).value_or(std::string());
}

} // namespace fasthtmlparser
