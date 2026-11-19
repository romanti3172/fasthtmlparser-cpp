// CSS selectors over the flat tag list.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "chars.hpp"
#include "css_tree.hpp"
#include "document_data.hpp"
#include "fasthtmlparser/query.hpp"
#include "query_cache.hpp"

namespace fasthtmlparser {

namespace detail {

// ----- the tree -----

CssTree::CssTree(const DocumentData& doc) {
    const auto& nodes = doc.nodes;
    std::size_t count = nodes.size();
    previous.assign(count, -1);
    next.assign(count, -1);
    position.assign(count, 0);
    from_end.assign(count, 0);
    // per parent (slot = parent index + 1, slot 0: the top level): its last element child so far and their number
    std::vector<std::int32_t> last_child(count + 1, -1);
    std::vector<std::int32_t> children(count + 1, 0);
    for (std::size_t i = 0; i < count; ++i) {
        if (!is_element(doc, i)) continue;
        std::size_t slot = static_cast<std::size_t>(nodes[i].parent + 1);
        std::int32_t prev = last_child[slot];
        previous[i] = prev;
        if (prev >= 0) next[prev] = static_cast<std::int32_t>(i);
        last_child[slot] = static_cast<std::int32_t>(i);
        position[i] = ++children[slot];
    }
    for (std::size_t i = 0; i < count; ++i)
        if (position[i] > 0) from_end[i] = children[nodes[i].parent + 1] - position[i] + 1;
}

namespace {

struct TypeKey {
    std::int32_t parent;
    std::int16_t key;
    std::uint32_t custom;
    bool operator==(const TypeKey&) const = default;
};

struct TypeKeyHash {
    std::size_t operator()(const TypeKey& k) const noexcept {
        std::uint64_t h = static_cast<std::uint32_t>(k.parent);
        h = h * 0x9E3779B97F4A7C15ull ^ static_cast<std::uint16_t>(k.key);
        h = h * 0x9E3779B97F4A7C15ull ^ k.custom;
        return static_cast<std::size_t>(h ^ (h >> 29));
    }
};

} // namespace

const TypePositions& CssTree::type_positions(const DocumentData& doc) const {
    if (TypePositions* t = types.load(std::memory_order_acquire)) return *t;
    const auto& nodes = doc.nodes;
    std::size_t count = nodes.size();
    auto tp = std::make_unique<TypePositions>();
    tp->position.assign(count, 0);
    tp->from_end.assign(count, 0);
    std::unordered_map<TypeKey, std::int32_t, TypeKeyHash> seen;
    auto key = [&](std::size_t i) {
        const NodeData& d = nodes[i];
        return TypeKey{d.parent, d.key, d.key < 0 ? d.custom : 0u};
    };
    for (std::size_t i = 0; i < count; ++i) {
        if (position[i] == 0) continue; // not an element
        tp->position[i] = ++seen[key(i)];
    }
    for (std::size_t i = 0; i < count; ++i)
        if (tp->position[i] > 0) tp->from_end[i] = seen[key(i)] - tp->position[i] + 1;
    TypePositions* expected = nullptr;
    TypePositions* raw = tp.get();
    if (types.compare_exchange_strong(expected, raw, std::memory_order_acq_rel)) {
        tp.release();
        return *raw;
    }
    return *expected;
}

const CssTree& css_tree(const DocumentData& doc) {
    if (CssTree* t = doc.css_tree.load(std::memory_order_acquire)) return *t;
    auto tree = std::make_unique<CssTree>(doc);
    CssTree* expected = nullptr;
    CssTree* raw = tree.get();
    if (doc.css_tree.compare_exchange_strong(expected, raw, std::memory_order_acq_rel)) {
        tree.release();
        return *raw;
    }
    return *expected;
}

// ----- the compiled selector -----

enum class Combinator { none, descendant, child, adjacent, sibling };
enum class AttrOp { exists, equals, includes, dash, prefix, suffix, substring };
enum class Pseudo { root, scope, empty, link, checked, disabled, enabled, required, optional };

struct CssList;

/// :nth-child(An+B [of S]) and its relatives; :first-child is :nth-child(1), and so on.
struct Nth {
    std::int64_t a = 0;
    std::int64_t b = 0;
    bool from_end = false;
    bool of_type = false;
    std::unique_ptr<CssList> of;
};

struct Simple {
    enum class Kind { id, class_name, attribute, nth, is_not, is, has, contains, pseudo };
    explicit Simple(Kind k) : kind(k) {}

    Kind kind;
    std::string name;  // id, class, attribute name, :contains text
    std::string value; // attribute value
    AttrOp op = AttrOp::exists;
    bool fold = false;
    Pseudo pseudo = Pseudo::root;
    Nth nth;
    std::unique_ptr<CssList> list; // :not, :is, :has
};

struct Compound {
    std::int16_t key = -1; // Tag, or -1
    std::string name;      // lower-case name not in Tag (has_name); neither: any element
    bool has_name = false;
    std::vector<Simple> simples;
};

struct Complex {
    Combinator leading = Combinator::none;
    std::vector<Compound> compounds;
    std::vector<Combinator> combinators; // combinators[i] stands between compounds[i] and [i+1]
};

struct CssList {
    std::vector<Complex> items;
};

namespace {

/// The document, the tag the query was called on (:scope, -1 for the document) and the tag a relative selector
/// starts from (:has(> a), or node.css("> li"); -1: the document).
struct Ctx {
    const DocumentData& doc;
    std::int64_t scope;
    std::int64_t anchor;

    const CssTree& tree() const { return css_tree(doc); }
    std::int64_t parent(std::size_t i) const { return doc.nodes[i].parent; }
};

bool list_matches(const CssList& list, std::size_t n, const Ctx& ctx);
bool complex_matches(const Complex& cx, std::size_t i, std::size_t n, const Ctx& ctx);

bool is_css_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == 0x0c || c == '\r'; }

bool equal(std::string_view a, std::string_view b, bool fold) { return fold ? equal_fold(a, b) : a == b; }

/// Whether a whitespace-separated list contains word.
bool contains_word(std::string_view list, std::string_view word, bool fold) {
    if (word.empty() || list.size() < word.size()) return false;
    std::size_t i = 0;
    while (i <= list.size()) {
        std::size_t j = i;
        while (j < list.size() && !is_css_space(static_cast<unsigned char>(list[j]))) ++j;
        if (j > i && equal(list.substr(i, j - i), word, fold)) return true;
        i = j + 1;
    }
    return false;
}

/// Whether the element has the attribute, with or without a value.
bool has_attribute(const Node& n, std::string_view name) {
    if (!n.attr_raw(name).empty()) return true;
    if (data(n).key == Tag::COMMENT.index()) return false;
    AttrScanner s(n);
    ScannedAttr a;
    while (s.next(a))
        if (equal_fold(s.name(a), name)) return true;
    return false;
}

bool attribute_matches(const Node& n, const Simple& s) {
    std::string_view raw = n.attr_raw(s.name);
    const std::string& value = s.value;
    if (raw.empty()) {
        // missing or empty: only these can match an empty value
        switch (s.op) {
        case AttrOp::exists: return has_attribute(n, s.name);
        case AttrOp::equals:
        case AttrOp::dash: return value.empty() && has_attribute(n, s.name);
        default: return false;
        }
    }
    if (s.op == AttrOp::exists) return true;
    CowString actual = decode(raw, true);
    std::string_view a = actual;
    bool fold = s.fold;
    auto slice_eq = [&](std::size_t from, std::size_t to) {
        return is_char_boundary(a, from) && is_char_boundary(a, to) && equal(a.substr(from, to - from), value, fold);
    };
    switch (s.op) {
    case AttrOp::equals: return equal(a, value, fold);
    case AttrOp::includes:
        for (unsigned char c : value)
            if (is_css_space(c)) return false;
        return contains_word(a, value, fold);
    case AttrOp::dash:
        return equal(a, value, fold) || (a.size() > value.size() && a[value.size()] == '-' && slice_eq(0, value.size()));
    case AttrOp::prefix: return !value.empty() && a.size() >= value.size() && slice_eq(0, value.size());
    case AttrOp::suffix:
        return !value.empty() && a.size() >= value.size() && slice_eq(a.size() - value.size(), a.size());
    case AttrOp::substring:
        if (value.empty()) return false;
        if (!fold) return a.find(value) != npos;
        for (std::size_t i = 0; i + value.size() <= a.size(); ++i)
            if (slice_eq(i, i + value.size())) return true;
        return false;
    case AttrOp::exists: return true;
    }
    return false;
}

bool nth_matches(const Nth& nth, std::size_t i, const Ctx& ctx) {
    const CssTree& t = ctx.tree();
    std::int64_t position;
    if (nth.of) {
        if (!list_matches(*nth.of, i, ctx)) return false;
        position = 1;
        auto step = [&](std::size_t k) { return nth.from_end ? t.next[k] : t.previous[k]; };
        for (std::int32_t sib = step(i); sib >= 0; sib = step(static_cast<std::size_t>(sib)))
            if (list_matches(*nth.of, static_cast<std::size_t>(sib), ctx)) ++position;
    } else if (nth.of_type) {
        const TypePositions& tp = t.type_positions(ctx.doc);
        position = nth.from_end ? tp.from_end[i] : tp.position[i];
    } else {
        position = nth.from_end ? t.from_end[i] : t.position[i];
    }
    if (position <= 0) return false; // not an element
    // position = a*n + b for some n >= 0
    if (nth.a == 0) return position == nth.b;
    std::int64_t diff = position - nth.b;
    return diff % nth.a == 0 && diff / nth.a >= 0;
}

/// The first top-level element: <html> on a page.
bool is_root(std::size_t i, const Ctx& ctx) { return ctx.doc.nodes[i].parent < 0 && ctx.tree().previous[i] < 0; }

bool is_form_control(std::string_view name) {
    return name == "button" || name == "input" || name == "select" || name == "textarea" || name == "optgroup" ||
           name == "option" || name == "fieldset";
}

bool is_input(std::string_view name) { return name == "input" || name == "select" || name == "textarea"; }

/// No child elements and no text; comments are allowed.
bool is_empty(std::size_t i, const DocumentData& doc) {
    const NodeData& n = doc.nodes[i];
    if (n.inner_start <= 0 || n.inner_end <= 0) return true; // a void or unclosed tag has no content
    std::int32_t position = n.inner_start;
    for (std::size_t k = i + 1; k < doc.nodes.size() && doc.nodes[k].end <= n.end; ++k) {
        const NodeData& child = doc.nodes[k];
        if (is_element(doc, k) || child.start > position) return false; // an element, or text before a comment
        position = std::max(position, child.end + 1);
    }
    return position > n.inner_end;
}

bool pseudo_matches(Pseudo kind, std::size_t i, const Ctx& ctx) {
    Node n = ctx.doc.at(i);
    std::string_view name = n.name();
    switch (kind) {
    case Pseudo::root: return is_root(i, ctx);
    case Pseudo::scope: return ctx.scope >= 0 ? static_cast<std::int64_t>(i) == ctx.scope : is_root(i, ctx);
    case Pseudo::empty: return is_empty(i, ctx.doc);
    case Pseudo::link: return (name == "a" || name == "area") && has_attribute(n, "href");
    case Pseudo::checked:
        return (name == "input" && has_attribute(n, "checked")) || (name == "option" && has_attribute(n, "selected"));
    case Pseudo::disabled: return is_form_control(name) && has_attribute(n, "disabled");
    case Pseudo::enabled: return is_form_control(name) && !has_attribute(n, "disabled");
    case Pseudo::required: return is_input(name) && has_attribute(n, "required");
    case Pseudo::optional: return is_input(name) && !has_attribute(n, "required");
    }
    return false;
}

/// Whether n stands in the combinator relation to anchor (-1: the document).
bool is_related(Combinator c, std::int64_t anchor, std::size_t n, const Ctx& ctx) {
    const auto& nodes = ctx.doc.nodes;
    switch (c) {
    case Combinator::child: return anchor < 0 ? nodes[n].parent < 0 : nodes[n].parent == anchor;
    case Combinator::descendant: {
        if (anchor < 0) return true;
        for (std::int64_t p = ctx.parent(n); p >= 0; p = ctx.parent(static_cast<std::size_t>(p)))
            if (p == anchor) return true;
        return false;
    }
    case Combinator::adjacent: return anchor >= 0 && ctx.tree().previous[n] == anchor;
    case Combinator::sibling:
        return anchor >= 0 && anchor < static_cast<std::int64_t>(n) && nodes[anchor].parent == nodes[n].parent &&
               is_element(ctx.doc, static_cast<std::size_t>(anchor));
    case Combinator::none: return false;
    }
    return false;
}

/// :has(): whether a tag in the anchor's scope matches the relative selector.
bool has_match(const CssList& relative, std::size_t anchor, const Ctx& ctx) {
    const DocumentData& doc = ctx.doc;
    const auto& nodes = doc.nodes;
    Ctx inner{doc, ctx.scope, static_cast<std::int64_t>(anchor)};
    std::int32_t anchor_end = nodes[anchor].end;
    for (const Complex& item : relative.items) {
        std::size_t last = item.compounds.size() - 1;
        std::size_t i = anchor + 1;
        if (item.leading == Combinator::descendant || item.leading == Combinator::child) {
            // the tags inside the anchor
            for (; i < nodes.size() && nodes[i].end <= anchor_end; ++i)
                if (is_element(doc, i) && complex_matches(item, last, i, inner)) return true;
        } else {
            // the following siblings and the tags inside them, up to the end of the parent
            while (i < nodes.size() && nodes[i].end <= anchor_end) ++i;
            std::int64_t parent = ctx.parent(anchor);
            std::int64_t end = parent >= 0 ? nodes[parent].end : std::numeric_limits<std::int32_t>::max();
            for (; i < nodes.size() && nodes[i].end <= end; ++i)
                if (is_element(doc, i) && complex_matches(item, last, i, inner)) return true;
        }
    }
    return false;
}

bool simple_matches(const Simple& s, std::size_t i, const Ctx& ctx) {
    Node n = ctx.doc.at(i);
    switch (s.kind) {
    case Simple::Kind::id: return attr_equals(n.attr_raw(attr::ID), s.name);
    case Simple::Kind::class_name: {
        std::string_view raw = n.attr_raw(attr::CLASS);
        return !raw.empty() && contains_word(decode(raw, true), s.name, false);
    }
    case Simple::Kind::attribute: return attribute_matches(n, s);
    case Simple::Kind::nth: return nth_matches(s.nth, i, ctx);
    case Simple::Kind::is_not: return !list_matches(*s.list, i, ctx);
    case Simple::Kind::is: return list_matches(*s.list, i, ctx);
    case Simple::Kind::has: return has_match(*s.list, i, ctx);
    case Simple::Kind::contains: return n.text().view().find(s.name) != npos;
    case Simple::Kind::pseudo: return pseudo_matches(s.pseudo, i, ctx);
    }
    return false;
}

bool compound_matches(const Compound& c, std::size_t n, const Ctx& ctx) {
    const NodeData& d = ctx.doc.nodes[n];
    if (c.key >= 0) {
        if (d.key != c.key) return false;
    } else if (c.has_name) {
        if (d.key >= 0 || ctx.doc.name(n) != c.name) return false;
    } else if (!is_element(ctx.doc, n)) {
        return false;
    }
    for (const Simple& s : c.simples)
        if (!simple_matches(s, n, ctx)) return false;
    return true;
}

/// n matches compound i and the part to the left of it matches the tags the combinators lead to, right to left as
/// browsers do.
bool complex_matches(const Complex& cx, std::size_t i, std::size_t n, const Ctx& ctx) {
    if (!compound_matches(cx.compounds[i], n, ctx)) return false;
    if (i == 0) return cx.leading == Combinator::none || is_related(cx.leading, ctx.anchor, n, ctx);
    switch (cx.combinators[i - 1]) {
    case Combinator::child: {
        std::int64_t p = ctx.parent(n);
        return p >= 0 && complex_matches(cx, i - 1, static_cast<std::size_t>(p), ctx);
    }
    case Combinator::descendant:
        for (std::int64_t p = ctx.parent(n); p >= 0; p = ctx.parent(static_cast<std::size_t>(p)))
            if (complex_matches(cx, i - 1, static_cast<std::size_t>(p), ctx)) return true;
        return false;
    case Combinator::adjacent: {
        std::int32_t p = ctx.tree().previous[n];
        return p >= 0 && complex_matches(cx, i - 1, static_cast<std::size_t>(p), ctx);
    }
    case Combinator::sibling: {
        const CssTree& t = ctx.tree();
        for (std::int32_t p = t.previous[n]; p >= 0; p = t.previous[p])
            if (complex_matches(cx, i - 1, static_cast<std::size_t>(p), ctx)) return true;
        return false;
    }
    case Combinator::none: return false;
    }
    return false;
}

bool list_matches(const CssList& list, std::size_t n, const Ctx& ctx) {
    for (const Complex& item : list.items)
        if (complex_matches(item, item.compounds.size() - 1, n, ctx)) return true;
    return false;
}

// ----- compiler -----

/// HTML: the values of these attributes are compared ignoring ASCII case unless the selector has the 's' flag.
constexpr std::string_view case_insensitive_values[] = {
    "accept",   "accept-charset", "align",    "alink",    "axis",     "bgcolor",   "charset", "checked",
    "clear",    "codetype",       "color",    "compact",  "declare",  "defer",     "dir",     "direction",
    "disabled", "enctype",        "face",     "frame",    "hreflang", "http-equiv", "lang",   "language",
    "link",     "media",          "method",   "multiple", "nohref",   "noresize",  "noshade", "nowrap",
    "readonly", "rel",            "rev",      "rules",    "scope",    "scrolling", "selected", "shape",
    "target",   "text",           "type",     "valign",   "valuetype", "vlink",
};

struct CompileError {
    std::string message;
};

bool is_name_start_byte(unsigned char c) { return is_ascii_alpha(c) || c == '_' || c >= 0x80; }

bool is_nth_char(unsigned char c) {
    return c == '+' || c == '-' || c == 'n' || c == 'N' || is_digit(c) || is_css_space(c);
}

std::optional<std::int64_t> parse_int(std::string_view s) {
    if (s.empty()) return std::nullopt;
    std::size_t i = 0;
    bool negative = false;
    if (s[0] == '+' || s[0] == '-') {
        negative = s[0] == '-';
        i = 1;
    }
    if (i == s.size()) return std::nullopt;
    std::int64_t v = 0;
    for (; i < s.size(); ++i) {
        if (!is_digit(static_cast<unsigned char>(s[i]))) return std::nullopt;
        v = v * 10 + (s[i] - '0');
        if (v > (std::int64_t(1) << 40)) return std::nullopt; // far out of the i32 range
    }
    if (negative) v = -v;
    if (v < std::numeric_limits<std::int32_t>::min() || v > std::numeric_limits<std::int32_t>::max())
        return std::nullopt;
    return v;
}

std::string quoted(std::string_view s) {
    // as Rust's {:?} for the strings of a message: quotes and backslashes escaped
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

/// Parses a selector list (Selectors Level 3 and the common Level 4 pseudo-classes). Identifiers and strings follow
/// the CSS syntax, escapes included (#a\:b, .\31 0, [title="a\"b"]).
class Compiler {
public:
    explicit Compiler(std::string_view s) : s_(s) {}

    std::unique_ptr<CssList> compile() {
        auto list = parse_list(false, true);
        if (p_ < s_.size()) fail("unexpected '" + char_at(p_) + "'");
        return list;
    }

private:
    std::string_view s_;
    std::size_t p_ = 0;

    [[noreturn]] void fail(const std::string& msg) const {
        throw CompileError{"invalid CSS selector " + quoted(s_) + " at position " + std::to_string(p_) + ": " + msg};
    }

    /// The character at byte position p (for messages).
    std::string char_at(std::size_t p) const {
        while (p > 0 && !is_char_boundary(s_, p)) --p;
        if (p >= s_.size()) return std::string(1, '\0');
        return std::string(s_.substr(p, decode_utf8(s_, p).size));
    }

    unsigned char peek(std::size_t offset) const {
        return p_ + offset < s_.size() ? static_cast<unsigned char>(s_[p_ + offset]) : 0;
    }

    bool skip_space() {
        std::size_t start = p_;
        while (p_ < s_.size() && is_css_space(static_cast<unsigned char>(s_[p_]))) ++p_;
        return p_ > start;
    }

    void expect(char ch) {
        if (peek(0) != static_cast<unsigned char>(ch)) {
            if (p_ >= s_.size()) fail(std::string("'") + ch + "' expected");
            fail(std::string("'") + ch + "' expected instead of '" + char_at(p_) + "'");
        }
        ++p_;
    }

    /// selector-list: complex (',' complex)*
    std::unique_ptr<CssList> parse_list(bool relative, bool top_level) {
        auto list = std::make_unique<CssList>();
        for (;;) {
            list->items.push_back(parse_complex(relative, top_level));
            skip_space();
            if (peek(0) != ',') return list;
            ++p_;
        }
    }

    /// complex: [combinator] compound (combinator compound)*
    Complex parse_complex(bool relative, bool top_level) {
        skip_space();
        Complex cx;
        if (relative || top_level) {
            Combinator comb = read_combinator();
            if (comb != Combinator::none) {
                if (top_level && comb != Combinator::child)
                    fail("a selector can start only with '>' (the children of the tag it is called on)");
                cx.leading = comb;
            } else if (relative) {
                cx.leading = Combinator::descendant;
            }
        }
        cx.compounds.push_back(parse_compound());
        for (;;) {
            bool ws = skip_space();
            unsigned char ch = peek(0);
            if (ch == 0 || ch == ',' || ch == ')') return cx;
            Combinator comb = read_combinator();
            if (comb == Combinator::none) {
                if (!ws) fail("unexpected '" + char_at(p_) + "'");
                comb = Combinator::descendant;
            }
            cx.combinators.push_back(comb);
            cx.compounds.push_back(parse_compound());
        }
    }

    Combinator read_combinator() {
        Combinator comb;
        switch (peek(0)) {
        case '>': comb = Combinator::child; break;
        case '+': comb = Combinator::adjacent; break;
        case '~': comb = Combinator::sibling; break;
        default: return Combinator::none;
        }
        ++p_;
        skip_space();
        return comb;
    }

    /// compound: [type | '*'] (#id | .class | [attribute] | :pseudo)*
    Compound parse_compound() {
        Compound cp;
        bool has_type = false;
        if (peek(0) == '*') {
            ++p_;
            has_type = true;
        } else if (is_ident_start(p_)) {
            std::string name = to_lower(read_name());
            has_type = true;
            int key = lookup_tag(name);
            if (key >= 0) {
                cp.key = static_cast<std::int16_t>(key);
            } else {
                cp.name = std::move(name);
                cp.has_name = true;
            }
        }
        if (peek(0) == '|') fail("namespaces are not supported; escape the colon of a prefixed name: dc\\:creator");

        for (;;) {
            switch (peek(0)) {
            case '#': {
                ++p_;
                if (!is_name_char(p_)) fail("an id expected after '#'");
                Simple s{Simple::Kind::id};
                s.name = read_name();
                cp.simples.push_back(std::move(s));
                break;
            }
            case '.': {
                ++p_;
                if (!is_ident_start(p_)) fail("a class name expected after '.'");
                Simple s{Simple::Kind::class_name};
                s.name = read_name();
                cp.simples.push_back(std::move(s));
                break;
            }
            case '[': cp.simples.push_back(parse_attribute()); break;
            case ':': parse_pseudo(cp); break;
            default:
                if (!has_type && cp.simples.empty()) {
                    if (p_ >= s_.size()) fail("a selector expected");
                    fail("unexpected '" + char_at(p_) + "'");
                }
                return cp;
            }
        }
    }

    /// '[' name [op value [i|s]] ']'
    Simple parse_attribute() {
        ++p_; // '['
        skip_space();
        if (!is_ident_start(p_)) fail("an attribute name expected");
        Simple s{Simple::Kind::attribute};
        s.name = read_name();
        skip_space();
        if (peek(0) == ']') {
            ++p_;
            s.op = AttrOp::exists;
            return s;
        }
        unsigned char ch = peek(0);
        switch (ch) {
        case '=':
            ++p_;
            s.op = AttrOp::equals;
            break;
        case '~': case '|': case '^': case '$': case '*':
            s.op = ch == '~' ? AttrOp::includes
                 : ch == '|' ? AttrOp::dash
                 : ch == '^' ? AttrOp::prefix
                 : ch == '$' ? AttrOp::suffix
                             : AttrOp::substring;
            if (peek(1) != '=') fail(std::string("'=' expected after '") + static_cast<char>(ch) + "'");
            p_ += 2;
            break;
        default: fail("'=', '~=', '|=', '^=', '$=', '*=' or ']' expected");
        }

        skip_space();
        if (peek(0) == '"' || peek(0) == '\'')
            s.value = read_string();
        else if (is_ident_start(p_))
            s.value = read_name();
        else
            fail("an attribute value expected (quote it if it is not a plain word)");

        skip_space();
        std::string lower_name = to_lower(s.name);
        s.fold = std::find(std::begin(case_insensitive_values), std::end(case_insensitive_values), lower_name) !=
                 std::end(case_insensitive_values);
        if (is_ident_start(p_)) {
            std::string flag = read_name();
            if (equal_fold(flag, "i"))
                s.fold = true;
            else if (equal_fold(flag, "s"))
                s.fold = false;
            else
                fail("unknown attribute flag '" + flag + "'");
            skip_space();
        }
        expect(']');
        return s;
    }

    static Simple make_nth(std::int64_t b, bool from_end, bool of_type) {
        Simple s{Simple::Kind::nth};
        s.nth.b = b;
        s.nth.from_end = from_end;
        s.nth.of_type = of_type;
        return s;
    }

    void parse_pseudo(Compound& cp) {
        ++p_; // ':'
        if (peek(0) == ':') fail("pseudo-elements (::before, ::text) select no tags");
        if (!is_ident_start(p_)) fail("a pseudo-class name expected after ':'");
        std::size_t start = p_;
        std::string name = to_lower(read_name());

        if (peek(0) == '(') {
            ++p_;
            skip_space();
            if (name == "not") {
                Simple s{Simple::Kind::is_not};
                s.list = parse_list(false, false);
                cp.simples.push_back(std::move(s));
            } else if (name == "is" || name == "where" || name == "matches" || name == "any") {
                Simple s{Simple::Kind::is};
                s.list = parse_list(false, false);
                cp.simples.push_back(std::move(s));
            } else if (name == "has") {
                Simple s{Simple::Kind::has};
                s.list = parse_list(true, false);
                cp.simples.push_back(std::move(s));
            } else if (name == "nth-child" || name == "nth-last-child" || name == "nth-of-type" ||
                       name == "nth-last-of-type") {
                auto [a, b] = parse_nth();
                bool of_type = name.size() >= 7 && name.compare(name.size() - 7, 7, "of-type") == 0;
                Simple s{Simple::Kind::nth};
                skip_space();
                if (!of_type && is_word("of")) {
                    p_ += 2;
                    s.nth.of = parse_list(false, false);
                }
                s.nth.a = a;
                s.nth.b = b;
                s.nth.from_end = name.find("last") != std::string::npos;
                s.nth.of_type = of_type;
                cp.simples.push_back(std::move(s));
            } else if (name == "contains") {
                Simple s{Simple::Kind::contains};
                if (peek(0) == '"' || peek(0) == '\'')
                    s.name = read_string();
                else if (is_ident_start(p_))
                    s.name = read_name();
                else
                    fail("a string expected in :contains()");
                cp.simples.push_back(std::move(s));
            } else {
                p_ = start;
                fail("unknown pseudo-class ':" + name + "()'");
            }
            skip_space();
            expect(')');
            return;
        }

        auto pseudo = [&](Pseudo k) {
            Simple s{Simple::Kind::pseudo};
            s.pseudo = k;
            cp.simples.push_back(std::move(s));
        };
        if (name == "first-child") {
            cp.simples.push_back(make_nth(1, false, false));
        } else if (name == "last-child") {
            cp.simples.push_back(make_nth(1, true, false));
        } else if (name == "only-child") {
            cp.simples.push_back(make_nth(1, false, false));
            cp.simples.push_back(make_nth(1, true, false));
        } else if (name == "first-of-type") {
            cp.simples.push_back(make_nth(1, false, true));
        } else if (name == "last-of-type") {
            cp.simples.push_back(make_nth(1, true, true));
        } else if (name == "only-of-type") {
            cp.simples.push_back(make_nth(1, false, true));
            cp.simples.push_back(make_nth(1, true, true));
        } else if (name == "root") {
            pseudo(Pseudo::root);
        } else if (name == "scope") {
            pseudo(Pseudo::scope);
        } else if (name == "empty") {
            pseudo(Pseudo::empty);
        } else if (name == "link" || name == "any-link") {
            pseudo(Pseudo::link);
        } else if (name == "checked") {
            pseudo(Pseudo::checked);
        } else if (name == "disabled") {
            pseudo(Pseudo::disabled);
        } else if (name == "enabled") {
            pseudo(Pseudo::enabled);
        } else if (name == "required") {
            pseudo(Pseudo::required);
        } else if (name == "optional") {
            pseudo(Pseudo::optional);
        } else {
            p_ = start;
            fail("unknown pseudo-class ':" + name + "'");
        }
    }

    /// An+B: odd, even, 3, n, -n+3, 2n+1, +2n - 1
    std::pair<std::int64_t, std::int64_t> parse_nth() {
        if (is_word("odd")) {
            p_ += 3;
            return {2, 1};
        }
        if (is_word("even")) {
            p_ += 4;
            return {2, 0};
        }
        std::size_t start = p_;
        std::string text;
        while (p_ < s_.size()) {
            unsigned char ch = static_cast<unsigned char>(s_[p_]);
            if (!is_nth_char(ch)) break;
            if (ch == 'N') ch = 'n';
            if (!is_css_space(ch)) text.push_back(static_cast<char>(ch));
            ++p_;
        }
        std::size_t n = text.find('n');
        if (n == std::string::npos) {
            if (auto v = parse_int(text)) return {0, *v};
        } else if (text.find('n', n + 1) == std::string::npos) {
            std::string_view a_text = std::string_view(text).substr(0, n);
            std::string_view b_text = std::string_view(text).substr(n + 1);
            std::optional<std::int64_t> a;
            if (a_text.empty() || a_text == "+")
                a = 1;
            else if (a_text == "-")
                a = -1;
            else
                a = parse_int(a_text);
            std::optional<std::int64_t> b;
            if (b_text.empty())
                b = 0;
            else if ((b_text[0] == '+' || b_text[0] == '-') && b_text.size() > 1 &&
                     is_digit(static_cast<unsigned char>(b_text[1])))
                b = parse_int(b_text);
            if (a && b) return {*a, *b};
        }
        p_ = start;
        fail("An+B expected (odd, even, 3, 2n+1, -n+3)");
    }

    /// Whether the identifier at the position is word (case-insensitive) and ends there.
    bool is_word(std::string_view word) const {
        return p_ + word.size() <= s_.size() && eq_ignore_ascii_case(s_.substr(p_, word.size()), word) &&
               !is_name_char(p_ + word.size());
    }

    bool is_escape(std::size_t p) const {
        return p < s_.size() && s_[p] == '\\' &&
               (p + 1 >= s_.size() || (s_[p + 1] != '\n' && s_[p + 1] != '\r' && s_[p + 1] != 0x0c));
    }

    bool is_name_char(std::size_t p) const {
        if (p >= s_.size()) return false;
        unsigned char ch = static_cast<unsigned char>(s_[p]);
        return is_name_start_byte(ch) || is_digit(ch) || ch == '-' || is_escape(p);
    }

    bool is_ident_start(std::size_t p) const {
        if (p >= s_.size()) return false;
        if (s_[p] == '-') {
            ++p;
            return p < s_.size() &&
                   (is_name_start_byte(static_cast<unsigned char>(s_[p])) || s_[p] == '-' || is_escape(p));
        }
        return is_name_start_byte(static_cast<unsigned char>(s_[p])) || is_escape(p);
    }

    std::string read_name() {
        std::string b;
        while (is_name_char(p_)) {
            if (s_[p_] == '\\')
                read_escape(b);
            else
                b.push_back(s_[p_++]);
        }
        return b;
    }

    std::string read_string() {
        char quote = s_[p_];
        std::size_t start = p_;
        ++p_;
        std::string b;
        for (;;) {
            if (p_ >= s_.size()) {
                p_ = start;
                fail("unterminated string");
            }
            char ch = s_[p_];
            if (ch == quote) {
                ++p_;
                return b;
            }
            if (ch == '\\') {
                if (p_ + 1 < s_.size() && (s_[p_ + 1] == '\n' || s_[p_ + 1] == 0x0c)) {
                    p_ += 2; // an escaped line break continues the string
                } else if (p_ + 2 < s_.size() && s_[p_ + 1] == '\r') {
                    p_ += s_[p_ + 2] == '\n' ? 3 : 2;
                } else {
                    read_escape(b);
                }
            } else {
                b.push_back(ch);
                ++p_;
            }
        }
    }

    /// '\' then up to 6 hex digits and one optional whitespace, or any other char as itself.
    void read_escape(std::string& b) {
        ++p_; // '\'
        if (p_ >= s_.size()) {
            append_utf8(b, 0xFFFD);
            return;
        }
        if (!is_hex(static_cast<unsigned char>(s_[p_]))) {
            std::size_t size = is_char_boundary(s_, p_) ? decode_utf8(s_, p_).size : 1;
            b.append(s_.substr(p_, size));
            p_ += size;
            return;
        }
        std::uint32_t code = 0;
        int digits = 0;
        while (digits < 6 && p_ < s_.size() && is_hex(static_cast<unsigned char>(s_[p_]))) {
            unsigned char c = static_cast<unsigned char>(s_[p_]);
            code = code * 16 + (is_digit(c) ? c - '0' : (c | 0x20) - 'a' + 10);
            ++p_;
            ++digits;
        }
        if (p_ < s_.size() && is_css_space(static_cast<unsigned char>(s_[p_]))) {
            if (s_[p_] == '\r' && peek(1) == '\n')
                p_ += 2;
            else
                ++p_;
        }
        if (code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) code = 0xFFFD;
        append_utf8(b, code);
    }
};

std::vector<Node> scan(const CssList& list, const DocumentData& doc, std::int64_t scope, std::size_t from,
                       std::int32_t end, bool first_only) {
    Ctx ctx{doc, scope, scope};
    std::vector<Node> result;
    for (std::size_t i = from; i < doc.nodes.size(); ++i) {
        if (doc.nodes[i].end > end) break;
        if (list_matches(list, i, ctx)) {
            result.push_back(doc.at(i));
            if (first_only) break;
        }
    }
    return result;
}

std::optional<Node> scan_first(const CssList& list, const DocumentData& doc, std::int64_t scope, std::size_t from,
                               std::int32_t end) {
    Ctx ctx{doc, scope, scope};
    for (std::size_t i = from; i < doc.nodes.size(); ++i) {
        if (doc.nodes[i].end > end) break;
        if (list_matches(list, i, ctx)) return doc.at(i);
    }
    return std::nullopt;
}

QueryCache<CssQuery>& css_cache() {
    static QueryCache<CssQuery> cache;
    return cache;
}

const CssQuery& cached_css(std::string_view selector, std::shared_ptr<const CssQuery>& holder) {
    holder = css_cache().get(selector, [](std::string_view s) {
        Compiled<CssQuery> c = CssQuery::compile(s);
        if (!c) throw std::invalid_argument(c.error());
        return std::move(c).value();
    });
    return *holder;
}

} // namespace
} // namespace detail

Compiled<CssQuery> CssQuery::compile(std::string_view selector) {
    try {
        detail::Compiler c(selector);
        std::shared_ptr<const detail::CssList> list = c.compile();
        return CssQuery(std::string(selector), std::move(list));
    } catch (const detail::CompileError& e) {
        return Compiled<CssQuery>::failure(e.message);
    }
}

std::vector<Node> CssQuery::all(const Document& doc) const {
    return detail::scan(*list_, *doc.data(), -1, 0, INT32_MAX, false);
}

std::optional<Node> CssQuery::first(const Document& doc) const {
    return detail::scan_first(*list_, *doc.data(), -1, 0, INT32_MAX);
}

std::vector<Node> CssQuery::all_in(const Node& n) const {
    return detail::scan(*list_, detail::doc(n), static_cast<std::int64_t>(n.index()), n.index() + 1,
                        detail::data(n).end, false);
}

std::optional<Node> CssQuery::first_in(const Node& n) const {
    return detail::scan_first(*list_, detail::doc(n), static_cast<std::int64_t>(n.index()), n.index() + 1,
                              detail::data(n).end);
}

bool CssQuery::matches(const Node& n) const {
    std::size_t i = n.index();
    const detail::DocumentData& doc = detail::doc(n);
    return detail::is_element(doc, i) &&
           detail::list_matches(*list_, i, detail::Ctx{doc, static_cast<std::int64_t>(i), static_cast<std::int64_t>(i)});
}

std::vector<Node> Document::css(std::string_view selector) const {
    std::shared_ptr<const CssQuery> q;
    return detail::cached_css(selector, q).all(*this);
}

std::optional<Node> Document::css_first(std::string_view selector) const {
    std::shared_ptr<const CssQuery> q;
    return detail::cached_css(selector, q).first(*this);
}

std::vector<Node> Node::css(std::string_view selector) const {
    std::shared_ptr<const CssQuery> q;
    return detail::cached_css(selector, q).all_in(*this);
}

std::optional<Node> Node::css_first(std::string_view selector) const {
    std::shared_ptr<const CssQuery> q;
    return detail::cached_css(selector, q).first_in(*this);
}

bool Node::is_css(std::string_view selector) const {
    std::shared_ptr<const CssQuery> q;
    return detail::cached_css(selector, q).matches(*this);
}

} // namespace fasthtmlparser
