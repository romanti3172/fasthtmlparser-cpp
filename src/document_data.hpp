// The data of a document, shared by the sources of the library.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chars.hpp"
#include "fasthtmlparser/document.hpp"
#include "links.hpp"

namespace fasthtmlparser::detail {

inline constexpr std::uint32_t no_custom = 0xFFFFFFFFu;

/// One tag of a document; positions are byte offsets into the source.
struct NodeData {
    std::uint32_t custom;     // index into custom_names for tags not in Tag; no_custom otherwise
    std::int32_t parent;      // index of the enclosing tag, -1 at the top level
    std::int32_t start;       // the opening '<'
    std::int32_t end;         // the last '>': of the end tag when there is one, otherwise of the opening tag
    std::int32_t inner_start; // right after the opening tag; 0 when the tag is not closed
    std::int32_t inner_end;   // the last byte before the end tag; 0 when the tag is not closed
    std::int32_t depth;
    std::int16_t key; // Tag, or -1
};

struct CssTree;

struct DocumentData {
    explicit DocumentData(std::pmr::memory_resource* mr)
        : mr(mr), nodes(mr), custom_names(mr), url(mr) {}
    ~DocumentData();

    DocumentData(const DocumentData&) = delete;
    DocumentData& operator=(const DocumentData&) = delete;

    std::pmr::memory_resource* mr;
    std::string_view source;
    char* owned_source = nullptr; // the copy of the source, allocated from mr; null for parse_view
    std::size_t owned_size = 0;
    std::pmr::vector<NodeData> nodes;
    std::pmr::vector<std::pmr::string> custom_names;
    std::pmr::string url;
    std::optional<Url> base; // url parsed once, for resolving relative links
    mutable std::atomic<CssTree*> css_tree{nullptr}; // sibling links and positions, built by the first selector
    mutable std::atomic<int> has_pre{0}; // 0: not looked up, 1: no pre, listing or plaintext, 2: there is one

    Node at(std::size_t i) const noexcept { return Node(this, static_cast<std::uint32_t>(i)); }
    const NodeData& node(std::size_t i) const noexcept { return nodes[i]; }
    std::size_t size() const noexcept { return nodes.size(); }

    /// The name of tag i.
    std::string_view name(std::size_t i) const noexcept {
        const NodeData& d = nodes[i];
        if (d.key >= 0) return tag_names[d.key];
        if (d.custom != no_custom) return custom_names[d.custom];
        return {};
    }
};

inline const NodeData& data(const Node& n) noexcept { return n.data_of_document()->nodes[n.index()]; }
inline const DocumentData& doc(const Node& n) noexcept { return *n.data_of_document(); }

/// Parses src into the tag list and the custom names it uses.
void parse(std::string_view src, std::pmr::vector<NodeData>& nodes, std::pmr::vector<std::pmr::string>& custom_names);

/// Tag name → Tag (ASCII case-insensitive), -1 when it is not a known one.
int lookup_tag(std::string_view name) noexcept;

/// "</script", ... for an element whose content is raw text (script, style, textarea, title, ...), "" otherwise.
std::string_view raw_text_end(int key) noexcept;

/// As in HTML5, '<' starts markup only before an ASCII letter, '/', '!' or '?'.
inline bool is_markup_start(std::string_view s, std::size_t i) noexcept {
    if (i + 1 >= s.size()) return false;
    unsigned char c = static_cast<unsigned char>(s[i + 1]);
    return is_ascii_alpha(c) || c == '/' || c == '!' || c == '?';
}

/// The position of the '<' of the end tag (e.g. "</script"), or s.size() if there is none.
std::size_t find_raw_text_end(std::string_view s, std::size_t from, std::string_view end_tag) noexcept;

/// The position of the '>' that ends a comment whose content starts at content_start.
std::size_t find_comment_end(std::string_view s, std::size_t content_start) noexcept;

// decode.cpp
bool needs_decoding(std::string_view s) noexcept;
CowString normalize_newlines(std::string_view s);
CowString decode(std::string_view s, bool in_attribute);
/// s starts with '#'. Returns the bytes consumed (0: not a reference) and the character.
std::size_t match_numeric(std::string_view s, char32_t& c) noexcept;
/// s is the text right after '&'. Returns the bytes consumed (0: not a reference) and the value.
std::size_t match_named(std::string_view s, bool in_attribute, std::string_view& value) noexcept;

// text.cpp
CowString clear_tags(std::string_view s, bool in_pre = false);
bool is_text(const Node& n, std::string_view expected);
/// Whether the tag separates words in the one-line text (as jsoup's text()): <br>, blocks, p/h1-h6, table cells.
bool separates_words_tag(int key) noexcept;
/// Whether trimmed text has whitespace to collapse: a line break, tab or other space than ' ', or two spaces in a row.
bool needs_collapse(std::string_view s) noexcept;
/// Whether the page has a pre, listing or plaintext (looked up once).
bool document_has_pre(const DocumentData& d) noexcept;
/// Whether the tag is pre, listing or plaintext, or inside one.
bool in_pre(const Node& n) noexcept;
std::string clear_tags_line_breaks(std::string_view value);

// attributes.cpp
struct ScannedAttr {
    std::size_t name_start;
    std::size_t name_len;
    std::size_t value_start; // npos without a value
    std::size_t value_len;
};

class AttrScanner {
public:
    explicit AttrScanner(const Node& n) noexcept;
    bool next(ScannedAttr& a) noexcept;
    std::string_view name(const ScannedAttr& a) const noexcept { return src_.substr(a.name_start, a.name_len); }
    std::string_view value(const ScannedAttr& a) const noexcept {
        return a.value_start == npos ? std::string_view() : src_.substr(a.value_start, a.value_len);
    }

private:
    std::string_view src_;
    std::size_t end_;
    std::size_t pos_;
};

/// Compares the decoded value; it allocates only when the raw value has '&' or CR.
bool attr_equals(std::string_view raw, std::string_view value);

/// Not a comment, the doctype or the XML declaration.
bool is_element(const DocumentData& d, std::size_t i) noexcept;

} // namespace fasthtmlparser::detail
