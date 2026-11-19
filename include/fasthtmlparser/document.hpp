#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "attr.hpp"
#include "cow_string.hpp"
#include "tag.hpp"

namespace fasthtmlparser {

class Document;
class Node;

namespace detail {
struct DocumentData;
struct DocumentDeleter {
    void operator()(DocumentData* d) const noexcept;
};
} // namespace detail

// ----- Query -----

/// Selects tags by name and, optionally, class, id, attribute value, visible text, inner HTML and the source of the
/// opening tag. It holds views of its strings: build it inline, searching allocates nothing.
///
///     doc.find(by(Tag::SPAN).class_name("y"));
///     doc.find(by_name("my-tag"));
///     doc.find(any().id("a"));
class Query {
public:
    constexpr Query() noexcept = default;

    /// Adds a class criterion: one of the space-separated classes, case-insensitive.
    constexpr Query class_name(std::string_view c) const noexcept { return with(&Query::class_, c, has_class_); }
    /// Adds an id criterion.
    constexpr Query id(std::string_view v) const noexcept { return with(&Query::id_, v, has_id_); }
    /// Adds an attribute criterion: the decoded value equals value.
    constexpr Query attr(std::string_view name, std::string_view value) const noexcept {
        Query q = with(&Query::attr_value_, value, has_attr_);
        q.attr_ = name;
        return q;
    }
    /// Adds a visible-text criterion (see Node::text).
    constexpr Query text(std::string_view v) const noexcept { return with(&Query::text_, v, has_text_); }
    /// Adds a criterion: the decoded id starts with prefix.
    constexpr Query id_starts_with(std::string_view v) const noexcept {
        return with(&Query::id_starts_with_, v, has_id_starts_with_);
    }
    /// Adds a criterion: the visible text starts with prefix.
    constexpr Query text_starts_with(std::string_view v) const noexcept {
        return with(&Query::text_starts_with_, v, has_text_starts_with_);
    }
    /// Adds a criterion: the inner HTML (trimmed) equals body.
    constexpr Query body(std::string_view v) const noexcept { return with(&Query::body_, v, has_body_); }
    /// Adds a criterion: the inner HTML (trimmed) starts with prefix.
    constexpr Query body_starts_with(std::string_view v) const noexcept {
        return with(&Query::body_starts_with_, v, has_body_starts_with_);
    }
    /// Adds a criterion: the opening tag without '<' and '>' (see Node::source) equals source.
    constexpr Query source(std::string_view v) const noexcept { return with(&Query::source_, v, has_source_); }

    /// Whether the tag matches every criterion.
    bool matches(const Node& n) const noexcept;

    friend constexpr Query by(Tag tag) noexcept;
    friend Query by_name(std::string_view name) noexcept;
    friend constexpr Query any() noexcept;

private:
    static constexpr std::uint16_t has_class_ = 1, has_id_ = 1 << 1, has_attr_ = 1 << 2, has_text_ = 1 << 3,
                                   has_id_starts_with_ = 1 << 4, has_text_starts_with_ = 1 << 5,
                                   has_body_ = 1 << 6, has_body_starts_with_ = 1 << 7, has_source_ = 1 << 8;

    constexpr Query with(std::string_view Query::*field, std::string_view v, std::uint16_t bit) const noexcept {
        Query q = *this;
        q.*field = v;
        q.has_ |= bit;
        return q;
    }

    std::int16_t key_ = 0; // Tag + 1; 0: by name or any
    std::uint16_t has_ = 0;
    std::string_view name_, class_, id_, attr_, attr_value_, text_, id_starts_with_, text_starts_with_, body_,
        body_starts_with_, source_;
};

/// Selects tags with a known name.
constexpr Query by(Tag tag) noexcept {
    Query q;
    q.key_ = static_cast<std::int16_t>(tag.index() + 1);
    return q;
}

/// Selects tags by name, case-insensitively; it works for names that are not in Tag.
Query by_name(std::string_view name) noexcept;

/// Selects every tag.
constexpr Query any() noexcept { return Query(); }

/// A tag name for the shortcuts: a Tag or any name as a string.
struct Name {
    Query query;
    constexpr Name(Tag tag) noexcept : query(by(tag)) {}
    Name(std::string_view name) noexcept : query(by_name(name)) {}
    Name(const char* name) noexcept : query(by_name(name)) {}
    Name(const std::string& name) noexcept : query(by_name(name)) {}
};

/// An attribute of an opening tag: the name and the raw value as written ("" without a value).
struct Attribute {
    std::string_view name;
    std::string_view value;
};

// ----- the search shortcuts of z3l.FastHtmlParser, on a Document (the page) and a Node (the tags inside it) -----

/// The return types are deduced: Node is not complete where Node derives from this class.
template <class Self>
class SearchShortcuts {
public:
    /// The first tag with the id; none for an empty id.
    auto find_by_id(std::string_view id) const {
        if (id.empty()) return decltype(self().find(any()))();
        return self().find(any().id(id));
    }
    /// The first tag with the name.
    auto find_by_name(Name name) const { return self().find(name.query); }
    /// The first tag with the name and the inner HTML.
    auto find_by_name_body(Name name, std::string_view body) const {
        return self().find(name.query.body(body));
    }
    /// The first tag with the name and the decoded attribute value.
    auto find_by_name_attribute(Name name, std::string_view attribute, std::string_view value) const {
        return self().find(name.query.attr(attribute, value));
    }
    /// The first tag with the name and the class.
    auto find_by_name_class(Name name, std::string_view class_name) const {
        return self().find(name.query.class_name(class_name));
    }
    /// The first tag with the name, the class and the inner HTML.
    auto find_by_name_class_body(Name name, std::string_view class_name, std::string_view body) const {
        return self().find(name.query.class_name(class_name).body(body));
    }
    /// The first tag with the name, and the class and visible text when they are not empty.
    auto find_by_name_class_text(Name name, std::string_view class_name, std::string_view text) const {
        return self().find(opt_text(opt_class(name.query, class_name), text));
    }
    /// The first tag with the name, and the id and visible text when they are not empty.
    auto find_by_name_id_text(Name name, std::string_view id, std::string_view text) const {
        return self().find(opt_text(opt_id(name.query, id), text));
    }
    /// The first tag with the name and the visible text.
    auto find_by_name_text(Name name, std::string_view text) const {
        return self().find(name.query.text(text));
    }
    /// The first tag with the name and the id.
    auto find_by_name_id(Name name, std::string_view id) const { return self().find(name.query.id(id)); }
    /// The first tag with the name whose id starts with the prefix.
    auto find_by_name_id_starts_with(Name name, std::string_view prefix) const {
        return self().find(name.query.id_starts_with(prefix));
    }
    /// The first tag with the name whose inner HTML starts with the prefix.
    auto find_by_name_body_starts_with(Name name, std::string_view prefix) const {
        return self().find(name.query.body_starts_with(prefix));
    }
    /// The first tag with the name whose visible text starts with the prefix; none for an empty prefix.
    auto find_by_name_text_starts_with(Name name, std::string_view prefix) const {
        if (prefix.empty()) return decltype(self().find(any()))();
        return self().find(name.query.text_starts_with(prefix));
    }
    /// The first tag with the name and the class whose visible text starts with the prefix; none for an empty prefix.
    auto find_by_name_class_text_starts_with(Name name, std::string_view class_name,
                                                            std::string_view prefix) const {
        if (prefix.empty()) return decltype(self().find(any()))();
        return self().find(name.query.class_name(class_name).text_starts_with(prefix));
    }
    /// The first tag with the name and the itemprop.
    auto find_by_name_itemprop(Name name, std::string_view itemprop) const {
        return self().find(name.query.attr(attr::ITEMPROP, itemprop));
    }
    /// All tags with the name.
    auto find_all_by_name(Name name) const { return self().find_all(name.query); }
    /// All tags with the name and the decoded attribute value.
    auto find_all_by_name_attribute(Name name, std::string_view attribute, std::string_view value) const {
        return self().find_all(name.query.attr(attribute, value));
    }
    /// All tags with the name and the class.
    auto find_all_by_name_class(Name name, std::string_view class_name) const {
        return self().find_all(name.query.class_name(class_name));
    }
    /// All tags with the name and the id.
    auto find_all_by_name_id(Name name, std::string_view id) const {
        return self().find_all(name.query.id(id));
    }
    /// All tags with the name whose id starts with the prefix.
    auto find_all_by_name_id_starts_with(Name name, std::string_view prefix) const {
        return self().find_all(name.query.id_starts_with(prefix));
    }
    /// All tags with the name and the itemprop.
    auto find_all_by_name_itemprop(Name name, std::string_view itemprop) const {
        return self().find_all(name.query.attr(attr::ITEMPROP, itemprop));
    }
    /// All tags with the name whose inner HTML starts with the prefix.
    auto find_all_by_name_body_starts_with(Name name, std::string_view prefix) const {
        return self().find_all(name.query.body_starts_with(prefix));
    }

protected:
    // The older "optional" shortcuts skip an empty criterion.
    static Query opt_class(Query q, std::string_view v) { return v.empty() ? q : q.class_name(v); }
    static Query opt_id(Query q, std::string_view v) { return v.empty() ? q : q.id(v); }
    static Query opt_text(Query q, std::string_view v) { return v.empty() ? q : q.text(v); }
    static Query opt_body(Query q, std::string_view v) { return v.empty() ? q : q.body(v); }

private:
    const Self& self() const noexcept { return static_cast<const Self&>(*this); }
};

// ----- Node -----

/// One tag of a document: a small handle (the document and an index), cheap to copy. Positions are byte offsets
/// into the document source. A Node, and every view it returns, is valid as long as its Document.
class Node : public SearchShortcuts<Node> {
public:
    Node() noexcept = default;

    /// The tag as a Tag, or none for a name that is not one of the known tags.
    std::optional<Tag> tag() const noexcept;
    /// The lower-case tag name ("div", "!--" for comments, "custom-tag", ...).
    std::string_view name() const noexcept;
    /// The nesting depth: 0 for top-level tags.
    std::size_t depth() const noexcept;
    /// The position of the tag in the document.
    std::size_t index() const noexcept { return index_; }
    /// The byte offsets of the opening '<' and of the last '>'.
    std::pair<std::size_t, std::size_t> pos() const noexcept;

    /// The inner HTML (trimmed); "" for tags that are not closed.
    std::string_view body() const noexcept;
    /// The source of the whole element; "" for tags that are not closed.
    std::string_view outer_html() const noexcept;
    /// The visible text with entities decoded. With child tags: tags, comments and the content of script, style,
    /// noscript, template, noframes, noembed removed, whitespace collapsed; without: the inner text as is (trimmed).
    CowString text() const;
    /// The text with the visible line structure kept: <br> → new line, p and h1–h6 → blank line, other block
    /// elements → new line, td / th → space; whitespace collapsed, entities decoded. Lines are joined with "\n".
    std::string text_line_breaks() const;
    /// Whether another tag starts inside this one.
    bool has_children() const noexcept;
    /// The opening tag without '<' and '>': the name and the attributes.
    std::string_view source() const noexcept;

    // ----- attributes -----

    /// The decoded value of the first attribute with the name (case-insensitive); "" when the attribute is missing
    /// or has no value.
    CowString attr(std::string_view name) const;
    /// The value of the first attribute with the name as written: quoted or unquoted, "" for an attribute without a
    /// value or a missing one.
    std::string_view attr_raw(std::string_view name) const noexcept;
    /// Whether one of the space-separated classes equals class_name (case-insensitive).
    bool has_class(std::string_view class_name) const;
    /// The attributes of the opening tag in source order.
    std::vector<Attribute> attrs() const;

    CowString attribute_id() const { return attr(attr::ID); }
    CowString attribute_name() const { return attr(attr::NAME); }
    CowString attribute_title() const { return attr(attr::TITLE); }
    CowString attribute_value() const { return attr(attr::VALUE); }
    CowString attribute_style() const { return attr(attr::STYLE); }
    CowString attribute_item_prop() const { return attr(attr::ITEMPROP); }

    // ----- links -----

    /// The link a browser follows: the href attribute decoded and made absolute against the page URL. An empty or
    /// missing href gives the page URL, as in a browser.
    std::string href() const;
    /// href() resolved against base_url instead of the page URL.
    std::string href_from(std::string_view base_url) const;
    /// The src attribute decoded and made absolute against the page URL.
    std::string src() const;
    /// src() resolved against base_url instead of the page URL.
    std::string src_from(std::string_view base_url) const;
    /// The address of a mailto: link (everything after the first ':'), or "" for other links.
    std::string href_mail_to() const;
    /// The number of a tel: link (everything after the first ':'), or "" for other links.
    std::string href_telephone() const;

    // ----- navigation -----

    /// The enclosing tag.
    std::optional<Node> parent() const noexcept;
    /// The direct children: the tags inside this one, one level deeper.
    std::vector<Node> children() const;
    /// The tag that follows in document order (the first child, if there is one).
    std::optional<Node> next() const noexcept;
    /// The tag before this one in document order.
    std::optional<Node> previous() const noexcept;
    /// The first tag after the end of this one that matches q (tags inside it are skipped).
    std::optional<Node> next_matching(const Query& q) const noexcept;
    /// The next tag at the same level inside the same parent.
    std::optional<Node> next_sibling() const noexcept { return next_sibling_matching(any()); }
    std::optional<Node> next_sibling_matching(const Query& q) const noexcept;
    /// The previous tag at the same level inside the same parent.
    std::optional<Node> previous_sibling() const noexcept { return previous_sibling_matching(any()); }
    std::optional<Node> previous_sibling_matching(const Query& q) const noexcept;
    /// The nearest enclosing tag that matches q.
    std::optional<Node> parent_matching(const Query& q) const noexcept;

    /// The source text right after the tag up to the next '<' (or the end of the page).
    std::string_view text_after_tag_until_next_one() const noexcept;
    /// text_after_tag_until_next_one() as visible text: entities decoded, whitespace collapsed.
    CowString text_no_tags_after_tag_until_next_one() const;
    /// The source between the end of the tag and the next tag that matches q; "" when there is none.
    std::string_view text_after_until(const Query& q) const noexcept;

    // ----- search -----

    /// The first tag inside this one (a descendant) that matches q.
    std::optional<Node> find(const Query& q) const noexcept;
    /// All tags inside this one (descendants) that match q.
    std::vector<Node> find_all(const Query& q) const;
    /// Whether the tag matches q.
    bool is(const Query& q) const noexcept { return q.matches(*this); }

    /// The tags inside this one that match the CSS selector (as querySelectorAll; '>' first selects children).
    /// Throws std::invalid_argument on an invalid selector; compile it with CssQuery::compile to handle errors.
    std::vector<Node> css(std::string_view selector) const;
    std::optional<Node> css_first(std::string_view selector) const;
    /// Whether this tag matches the selector (as element.matches()).
    bool is_css(std::string_view selector) const;

    /// The tags the XPath expression selects, relative paths starting at this tag. Throws std::invalid_argument on
    /// an invalid expression and std::logic_error on one that gives no node-set.
    std::vector<Node> xpath(std::string_view expression) const;
    std::optional<Node> xpath_first(std::string_view expression) const;
    /// The values the expression selects from this tag.
    std::vector<std::string> xpath_values(std::string_view expression) const;
    /// The first value the expression selects from this tag, or "".
    std::string xpath_value(std::string_view expression) const;

    // ----- shortcuts -----

    bool is_name_attribute(Name name, std::string_view attribute, std::string_view value) const {
        return is(name.query.attr(attribute, value));
    }
    bool is_name_body(Name name, std::string_view body) const { return is(name.query.body(body)); }
    bool is_name_text(Name name, std::string_view text) const { return is(name.query.text(text)); }
    bool is_name_class(Name name, std::string_view c) const { return is(name.query.class_name(c)); }
    bool is_name_class_body(Name name, std::string_view c, std::string_view body) const {
        return is(name.query.class_name(c).body(body));
    }
    /// Whether the tag has the name, and the class, id and inner HTML when they are not empty.
    bool is_name_class_id_body(Name name, std::string_view c, std::string_view id, std::string_view body) const {
        return is(opt_body(opt_id(opt_class(name.query, c), id), body));
    }
    /// Whether the tag has the name, and the class and visible text when they are not empty.
    bool is_name_class_text(Name name, std::string_view c, std::string_view text) const {
        return is(opt_text(opt_class(name.query, c), text));
    }
    /// Whether the tag has the name, and the id and visible text when they are not empty.
    bool is_name_id_text(Name name, std::string_view id, std::string_view text) const {
        return is(opt_text(opt_id(name.query, id), text));
    }
    bool is_name_id(Name name, std::string_view id) const { return is(name.query.id(id)); }
    bool is_name_text_starts_with(Name name, std::string_view prefix) const {
        return is(name.query.text_starts_with(prefix));
    }
    bool is_name_body_starts_with(Name name, std::string_view prefix) const {
        return is(name.query.body_starts_with(prefix));
    }
    bool is_name_class_text_starts_with(Name name, std::string_view c, std::string_view prefix) const {
        return is(name.query.class_name(c).text_starts_with(prefix));
    }
    bool is_name_id_starts_with(Name name, std::string_view prefix) const {
        return is(name.query.id_starts_with(prefix));
    }
    bool is_name_itemprop(Name name, std::string_view itemprop) const {
        return is(name.query.attr(attr::ITEMPROP, itemprop));
    }
    bool is_name_source(Name name, std::string_view source) const { return is(name.query.source(source)); }

    std::optional<Node> parent_by_name(Name name) const noexcept { return parent_matching(name.query); }
    std::optional<Node> parent_by_name_class(Name name, std::string_view c) const noexcept {
        return parent_matching(name.query.class_name(c));
    }
    std::optional<Node> next_by_name(Name name) const noexcept { return next_matching(name.query); }
    std::optional<Node> next_by_name_class(Name name, std::string_view c) const noexcept {
        return next_matching(name.query.class_name(c));
    }
    std::optional<Node> next_sibling_by_name(Name name) const noexcept { return next_sibling_matching(name.query); }
    std::optional<Node> next_sibling_by_name_class(Name name, std::string_view c) const noexcept {
        return next_sibling_matching(name.query.class_name(c));
    }
    std::optional<Node> previous_sibling_by_name(Name name) const noexcept {
        return previous_sibling_matching(name.query);
    }
    std::string_view text_after_until_tag_by_name(Name name) const noexcept { return text_after_until(name.query); }
    std::string_view text_after_until_tag_by_name_class(Name name, std::string_view c) const noexcept {
        return text_after_until(name.query.class_name(c));
    }

    friend bool operator==(const Node& a, const Node& b) noexcept { return a.doc_ == b.doc_ && a.index_ == b.index_; }

    /// The document data the node belongs to (for the library's own use).
    const detail::DocumentData* data_of_document() const noexcept { return doc_; }

private:
    friend class Document;
    friend struct detail::DocumentData;
    Node(const detail::DocumentData* doc, std::uint32_t index) noexcept : doc_(doc), index_(index) {}

    const detail::DocumentData* doc_ = nullptr;
    std::uint32_t index_ = 0;
};

/// The tags of a document in document order.
class NodeRange {
public:
    class iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = Node;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = Node;

        iterator() noexcept = default;
        Node operator*() const noexcept;
        iterator& operator++() noexcept { ++i_; return *this; }
        iterator operator++(int) noexcept { iterator t = *this; ++i_; return t; }
        iterator& operator--() noexcept { --i_; return *this; }
        iterator operator--(int) noexcept { iterator t = *this; --i_; return t; }
        iterator& operator+=(difference_type n) noexcept { i_ = static_cast<std::uint32_t>(i_ + n); return *this; }
        iterator& operator-=(difference_type n) noexcept { i_ = static_cast<std::uint32_t>(i_ - n); return *this; }
        friend iterator operator+(iterator it, difference_type n) noexcept { return it += n; }
        friend iterator operator+(difference_type n, iterator it) noexcept { return it += n; }
        friend iterator operator-(iterator it, difference_type n) noexcept { return it -= n; }
        friend difference_type operator-(iterator a, iterator b) noexcept {
            return static_cast<difference_type>(a.i_) - static_cast<difference_type>(b.i_);
        }
        Node operator[](difference_type n) const noexcept { return *(*this + n); }
        friend bool operator==(iterator a, iterator b) noexcept { return a.i_ == b.i_; }
        friend auto operator<=>(iterator a, iterator b) noexcept { return a.i_ <=> b.i_; }

    private:
        friend class NodeRange;
        iterator(const detail::DocumentData* d, std::uint32_t i) noexcept : d_(d), i_(i) {}
        const detail::DocumentData* d_ = nullptr;
        std::uint32_t i_ = 0;
    };

    iterator begin() const noexcept { return {d_, 0}; }
    iterator end() const noexcept { return {d_, size_}; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

private:
    friend class Document;
    NodeRange(const detail::DocumentData* d, std::uint32_t size) noexcept : d_(d), size_(size) {}
    const detail::DocumentData* d_;
    std::uint32_t size_;
};

// ----- Document -----

/// A parsed page: a flat list of tags in document order, each holding only positions in the source. Text,
/// attributes and entities are read when they are asked for. A Document is read-only and can be used from any
/// number of threads at once.
class Document : public SearchShortcuts<Document> {
public:
    /// An empty document.
    Document();

    /// Parses an HTML or XML page. The document keeps a copy of the source; with a page URL relative links
    /// (Node::href, Node::src) are resolved against it. All memory of the document comes from mr (the default
    /// resource when null): a std::pmr::monotonic_buffer_resource makes it an arena.
    static Document parse(std::string_view html, std::string_view url = {},
                          std::pmr::memory_resource* mr = nullptr);

    /// As parse, without the copy: the document reads html in place, and it must outlive the document.
    static Document parse_view(std::string_view html, std::string_view url = {},
                               std::pmr::memory_resource* mr = nullptr);

    Document(Document&&) noexcept = default;
    Document& operator=(Document&&) noexcept = default;
    ~Document() = default;

    /// The page URL given to parse, or "".
    std::string_view url() const noexcept;
    /// The page source.
    std::string_view source() const noexcept;
    /// The number of tags (comments included).
    std::size_t size() const noexcept;
    bool empty() const noexcept { return size() == 0; }

    /// The tag at index i in document order (i < size()).
    Node node(std::size_t i) const noexcept { return Node(d_.get(), static_cast<std::uint32_t>(i)); }
    /// The tag at index i in document order, or none.
    std::optional<Node> get(std::size_t i) const noexcept {
        if (i >= size()) return std::nullopt;
        return node(i);
    }
    /// All tags in document order.
    NodeRange nodes() const noexcept { return NodeRange(d_.get(), static_cast<std::uint32_t>(size())); }

    /// The first tag in the document that matches q.
    std::optional<Node> find(const Query& q) const noexcept;
    /// All tags in the document that match q.
    std::vector<Node> find_all(const Query& q) const;

    /// The tags that match the CSS selector. Throws std::invalid_argument on an invalid selector.
    std::vector<Node> css(std::string_view selector) const;
    std::optional<Node> css_first(std::string_view selector) const;

    /// The tags the XPath expression selects. Throws std::invalid_argument on an invalid expression and
    /// std::logic_error on one that gives no node-set.
    std::vector<Node> xpath(std::string_view expression) const;
    std::optional<Node> xpath_first(std::string_view expression) const;
    /// The values the expression selects: decoded attributes, text(), visible text of tags, or the result of
    /// count(), sum(), string(), ...
    std::vector<std::string> xpath_values(std::string_view expression) const;
    /// The first value the expression selects, or "".
    std::string xpath_value(std::string_view expression) const;

    /// The document data (for the library's own use).
    const detail::DocumentData* data() const noexcept { return d_.get(); }

private:
    explicit Document(detail::DocumentData* d) noexcept : d_(d) {}
    std::unique_ptr<detail::DocumentData, detail::DocumentDeleter> d_;
};

// ----- free functions -----

/// Decodes the character references of a text; CR LF and a lone CR become LF.
CowString decode_html(std::string_view s);

/// The visible text of an HTML fragment, as Node::text gives it for a tag's content.
std::string html_to_text(std::string_view html);

/// The text of an HTML fragment with the visible line structure kept, as Node::text_line_breaks gives it.
std::string html_to_text_line_breaks(std::string_view html);

/// The tags of the list at the level of the first one (the outermost tags of a find_all result).
std::vector<Node> first_level(const std::vector<Node>& nodes);
/// The tags of the list with the name, at the level of the first tag of the list.
std::vector<Node> first_level_by_name(const std::vector<Node>& nodes, Name name);
/// The tags of the list with the name and the class, at the level of the first tag of the list.
std::vector<Node> first_level_by_name_class(const std::vector<Node>& nodes, Name name, std::string_view class_name);

} // namespace fasthtmlparser
