#include <array>

#include "chars.hpp"
#include "document_data.hpp"

namespace fasthtmlparser {

namespace detail {
namespace {

/// For the inside of an opening tag, the end tag ("</script") when the element's content is never visible text
/// (script, style, noscript, template, noframes, noembed).
std::string_view invisible_content_end(std::string_view tag) {
    if (tag.empty()) return {};
    char first = static_cast<char>(tag[0] | 0x20);
    if (first != 's' && first != 't' && first != 'n') return {};
    std::size_t n = 1;
    while (n < tag.size() && n <= 8 && is_ascii_alpha(static_cast<unsigned char>(tag[n]))) ++n;
    if (n < tag.size() && tag[n] != '/' && !is_space(static_cast<unsigned char>(tag[n]))) return {};
    std::string_view name = tag.substr(0, n);
    std::string_view end;
    if (eq_ignore_ascii_case(name, "style"))
        end = "</style";
    else if (eq_ignore_ascii_case(name, "script"))
        end = "</script";
    else if (eq_ignore_ascii_case(name, "noembed"))
        end = "</noembed";
    else if (eq_ignore_ascii_case(name, "noscript"))
        end = "</noscript";
    else if (eq_ignore_ascii_case(name, "template"))
        end = "</template";
    else if (eq_ignore_ascii_case(name, "noframes"))
        end = "</noframes";
    else
        return {};
    // <script src="x" />: no content
    std::string_view t = trim_ascii_end(tag);
    if (!t.empty() && t.back() == '/') return {};
    return end;
}

/// s[i] is '<'. Returns the index of the last byte of what is not text there: a comment, a tag, or a tag together
/// with invisible content (script, style, ...) up to its end tag. npos: an unterminated tag. The result may be
/// "one before i" (the wrapped -1 of an empty raw-text run at the very start); callers add 1 to it.
std::size_t skip_markup(std::string_view s, std::size_t i) {
    std::string_view rest = s.substr(i + 1);
    if (rest.substr(0, 3) == "!--") return find_comment_end(s, i + 4);
    std::size_t close = find_byte(rest, '>');
    if (close == npos) return npos;
    std::size_t tag_end = i + 1 + close;
    std::string_view end_tag = invisible_content_end(rest.substr(0, close));
    if (!end_tag.empty()) return find_raw_text_end(s, tag_end + 1, end_tag) - 1;
    return tag_end;
}

/// Whether the markup at s[i] ('<') is a tag that separates words in the one-line text, as in jsoup's text(): <br>,
/// a block element or a table cell, opening or closing. Comments, declarations, inline and unknown tags do not.
bool separates_words(std::string_view s, std::size_t i) {
    std::size_t start = i + 1;
    if (start < s.size() && s[start] == '/') ++start;
    std::size_t end = start;
    while (end < s.size() && (is_ascii_alpha(static_cast<unsigned char>(s[end])) || (s[end] >= '0' && s[end] <= '9'))) ++end;
    if (end == start) return false;
    int key = lookup_tag(s.substr(start, end - start));
    return key >= 0 && separates_words_tag(key);
}

bool is_pre_key(int key) noexcept {
    return key == Tag::PRE.index() || key == Tag::LISTING.index() || key == Tag::PLAINTEXT.index();
}

/// +1 for an opening pre, listing or plaintext at s[i], -1 for a closing one, 0 otherwise: their text keeps its
/// whitespace, as in jsoup.
int pre_tag(std::string_view s, std::size_t i) {
    std::size_t start = i + 1;
    int sign = 1;
    if (start < s.size() && s[start] == '/') {
        ++start;
        sign = -1;
    }
    if (start >= s.size() || ((s[start] | 0x20) != 'p' && (s[start] | 0x20) != 'l')) return 0; // the usual tag
    std::size_t end = start;
    while (end < s.size() && is_ascii_alpha(static_cast<unsigned char>(s[end]))) ++end;
    return is_pre_key(lookup_tag(s.substr(start, end - start))) ? sign : 0;
}

/// Removes tags, comments and invisible content, collapses whitespace runs into one ' ' and trims: the visible text
/// in one line, as jsoup's text(). <br>, block elements and table cells separate words ("a<br>b", a row of cells:
/// "a b"), inline tags do not ("a<b>b</b>": "ab"). Inside <pre> (in_pre: the text itself is in one) whitespace is
/// kept as written.
void strip_tags(std::string_view s, std::string& dst, bool in_pre) {
    bool prev_space = true; // left trim
    int pre_depth = in_pre ? 1 : 0;
    std::size_t i = 0;
    std::size_t run = 0; // the start of the text not yet copied
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '<' && is_markup_start(s, i)) {
            dst.append(s.substr(run, i - run));
            std::size_t last = skip_markup(s, i);
            if (last == npos) {
                run = s.size();
                break;
            }
            if (!in_pre) pre_depth = std::max(0, pre_depth + pre_tag(s, i));
            // only after a word: next to whitespace the tag changes nothing (and costs no lookup)
            if (!prev_space && separates_words(s, i)) {
                dst.push_back(' ');
                prev_space = true;
            }
            i = last + 1;
            run = i;
            continue;
        }
        if (pre_depth > 0) {
            bool space;
            Utf8Char ch = space_at(s, i, space);
            if (space) {
                // as written (left in the run), but not at the start
                if (dst.empty() && run == i) run = i + ch.size;
                prev_space = true;
            } else {
                prev_space = false;
            }
            i += ch.size;
            continue;
        }
        if (c < 0x80) {
            if (is_space(c)) {
                dst.append(s.substr(run, i - run));
                if (!prev_space) {
                    dst.push_back(' ');
                    prev_space = true;
                }
                run = i + 1;
            } else {
                prev_space = false;
            }
            ++i;
            continue;
        }
        bool space;
        Utf8Char ch = space_at(s, i, space);
        if (space) {
            dst.append(s.substr(run, i - run));
            if (!prev_space) {
                dst.push_back(' ');
                prev_space = true;
            }
            run = i + ch.size;
        } else {
            prev_space = false;
        }
        i += ch.size;
    }
    if (run < s.size()) dst.append(s.substr(run));
    // Right trim (inside <pre> there may be more than one trailing whitespace char).
    while (!dst.empty()) {
        std::size_t k = dst.size() - 1;
        while (k > 0 && (static_cast<unsigned char>(dst[k]) & 0xC0) == 0x80) --k; // the start of the last char
        bool space;
        space_at(dst, k, space);
        if (!space) break;
        dst.resize(k);
    }
}

} // namespace

bool needs_collapse(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size()) {
        bool space;
        if (s[i] == ' ') {
            if (i + 1 < s.size()) {
                space_at(s, i + 1, space);
                if (space) return true;
            }
            ++i;
            continue;
        }
        Utf8Char ch = space_at(s, i, space);
        if (space) return true;
        i += ch.size;
    }
    return false;
}

bool document_has_pre(const DocumentData& d) noexcept {
    int v = d.has_pre.load(std::memory_order_relaxed);
    if (v == 0) {
        v = 1;
        for (const NodeData& n : d.nodes) {
            if (is_pre_key(n.key)) {
                v = 2;
                break;
            }
        }
        d.has_pre.store(v, std::memory_order_relaxed); // a race only computes the same answer twice
    }
    return v == 2;
}

bool in_pre(const Node& n) noexcept {
    if (!document_has_pre(doc(n))) return false;
    for (std::optional<Node> t = n; t; t = t->parent()) {
        if (is_pre_key(data(*t).key)) return true;
    }
    return false;
}

CowString clear_tags(std::string_view s, bool in_pre) {
    if (s.empty()) return CowString();
    std::string buf;
    buf.reserve(s.size());
    strip_tags(s, buf, in_pre);
    if (find_byte(buf, '&') != npos) return CowString(std::move(decode(buf, false)).str());
    return CowString(std::move(buf));
}

bool is_text(const Node& n, std::string_view expected) {
    std::string_view value = n.body();
    if (value.empty() || expected.empty()) return false;
    // <pre> keeps its whitespace in text(), raw text is as written: compare with text() itself (rare, the slow way)
    if ((!n.has_children() && !raw_text_end(data(n).key).empty()) || document_has_pre(doc(n))) return n.text() == expected;
    std::size_t matched = 0;
    bool prev_space = true;     // left trim
    bool pending_space = false; // a collapsed space counts only if more text follows (right trim)
    std::size_t i = 0;
    while (i < value.size()) {
        unsigned char c = static_cast<unsigned char>(value[i]);
        if (c == '<' && is_markup_start(value, i)) {
            std::size_t last = skip_markup(value, i);
            if (last == npos) break;
            // <br>, blocks and cells separate words, as in text()
            if (!prev_space && separates_words(value, i)) {
                pending_space = true;
                prev_space = true;
            }
            i = last + 1;
            continue;
        }
        bool space;
        Utf8Char ch = space_at(value, i, space);
        if (space) {
            if (!prev_space) {
                pending_space = true;
                prev_space = true;
            }
            i += ch.size;
            continue;
        }
        // Text with entities must be decoded before comparing: take the full path.
        if (c == '&') return n.text() == expected;
        if (pending_space) {
            if (matched >= expected.size() || expected[matched] != ' ') return false;
            ++matched;
            pending_space = false;
        }
        if (matched + ch.size > expected.size() || expected.substr(matched, ch.size) != value.substr(i, ch.size))
            return false;
        matched += ch.size;
        prev_space = false;
        i += ch.size;
    }
    return matched == expected.size();
}

// ----- text with line breaks -----

namespace {

enum class LineBreak : unsigned char {
    none,         // inline tag (b, span, a, ...): removed, words are not split
    space,        // table cell: word separator
    line,         // <br> and block elements (div, li, tr, ...): new line
    paragraph,    // p, h1-h6: new paragraph (blank line)
    skip_content, // script, style, noscript: contents are not text
};

const std::array<LineBreak, tag_count>& line_break_kinds() {
    static const std::array<LineBreak, tag_count> kinds = [] {
        std::array<LineBreak, tag_count> k{};
        k.fill(LineBreak::none);
        for (Tag t : {Tag::P, Tag::H1, Tag::H2, Tag::H3, Tag::H4, Tag::H5, Tag::H6}) k[t.index()] = LineBreak::paragraph;
        for (Tag t : {Tag::BR,       Tag::DIV,     Tag::LI,      Tag::UL,       Tag::OL,      Tag::DL,
                      Tag::DT,       Tag::DD,      Tag::TABLE,   Tag::CAPTION,  Tag::TR,      Tag::THEAD,
                      Tag::TBODY,    Tag::TFOOT,   Tag::SECTION, Tag::ARTICLE,  Tag::ASIDE,   Tag::NAV,
                      Tag::HEADER,   Tag::FOOTER,  Tag::MAIN,    Tag::ADDRESS,  Tag::BLOCKQUOTE, Tag::PRE,
                      Tag::HR,       Tag::FIGURE,  Tag::FIGCAPTION, Tag::FORM,  Tag::FIELDSET, Tag::LEGEND,
                      Tag::DETAILS,  Tag::SUMMARY, Tag::DIALOG,  Tag::OPTION,   Tag::HTML,    Tag::HEAD,
                      Tag::BODY,     Tag::TITLE,   Tag::HGROUP,  Tag::SEARCH,   Tag::MENU,    Tag::CENTER,
                      Tag::DIR,      Tag::LISTING, Tag::XMP,     Tag::PLAINTEXT, Tag::MULTICOL, Tag::FRAMESET,
                      Tag::MARQUEE})
            k[t.index()] = LineBreak::line;
        k[Tag::TD.index()] = LineBreak::space;
        k[Tag::TH.index()] = LineBreak::space;
        for (Tag t : {Tag::SCRIPT, Tag::STYLE, Tag::NOSCRIPT, Tag::TEMPLATE, Tag::NOFRAMES, Tag::NOEMBED})
            k[t.index()] = LineBreak::skip_content;
        return k;
    }();
    return kinds;
}

} // namespace

bool separates_words_tag(int key) noexcept {
    LineBreak k = line_break_kinds()[static_cast<std::size_t>(key)];
    return k == LineBreak::line || k == LineBreak::paragraph || k == LineBreak::space;
}

namespace {

/// Writes text, collapsing whitespace and turning requested breaks into trimmed lines.
struct LineWriter {
    std::string out;
    std::size_t pending_breaks = 0; // new lines requested since the last text char
    bool pending_space = false;

    void put(char32_t c) {
        if (is_whitespace(c)) {
            pending_space = true;
            return;
        }
        if (!out.empty()) {
            // nothing before the first text char: leading breaks and spaces are trimmed
            if (pending_breaks > 0) {
                for (std::size_t k = 0; k < std::min<std::size_t>(pending_breaks, 2); ++k) out.push_back('\n');
            } else if (pending_space) {
                out.push_back(' ');
            }
        }
        pending_breaks = 0;
        pending_space = false;
        append_utf8(out, c);
    }

    /// A block boundary: at least count new lines before the next text.
    void line_break(std::size_t count) { pending_breaks = std::max(pending_breaks, count); }
};

} // namespace

std::string clear_tags_line_breaks(std::string_view input) {
    if (input.empty()) return {};
    CowString normalized = normalize_newlines(input); // values are read as a browser reads the page
    std::string_view value = normalized;
    LineWriter w;
    w.out.reserve(value.size());
    const auto& kinds = line_break_kinds();
    std::size_t i = 0;
    while (i < value.size()) {
        unsigned char c = static_cast<unsigned char>(value[i]);

        if (c == '&') {
            std::string_view rest = value.substr(i + 1);
            if (!rest.empty() && rest[0] == '#') {
                char32_t ch;
                if (std::size_t consumed = match_numeric(rest, ch)) {
                    w.put(ch);
                    i += 1 + consumed;
                    continue;
                }
            } else {
                std::string_view decoded;
                if (std::size_t consumed = match_named(rest, false, decoded)) {
                    // decoded chars go through the same whitespace/text logic (&nbsp; collapses, &lt; is plain text)
                    for (std::size_t k = 0; k < decoded.size();) {
                        Utf8Char ch = decode_utf8(decoded, k);
                        w.put(ch.c);
                        k += ch.size;
                    }
                    i += 1 + consumed;
                    continue;
                }
            }
            w.put('&');
            ++i;
            continue;
        }

        if (c != '<' || !is_markup_start(value, i)) {
            Utf8Char ch = decode_utf8(value, i);
            if (ch.c == 0xFFFD && ch.size == 1 && c >= 0x80) {
                w.put(0xFFFD);
            } else {
                w.put(ch.c);
            }
            i += ch.size;
            continue;
        }

        std::string_view rest = value.substr(i);
        if (rest.substr(0, 4) == "<!--") {
            std::size_t end = rest.find("-->", 4);
            if (end == npos) break;
            i += end + 3;
            continue;
        }

        std::size_t close = find_byte(rest, '>');
        if (close == npos) break; // unterminated tag: the rest is markup
        std::string_view tag = value.substr(i + 1, close - 1);
        i += close + 1;

        bool closing = !tag.empty() && tag[0] == '/';
        std::string_view name = closing ? tag.substr(1) : tag;
        std::size_t k = name.find_first_of(" \t\r\n\f/");
        if (k != npos) name = name.substr(0, k);
        int known = lookup_tag(name);
        if (known < 0) continue; // unknown tag: just removed

        switch (kinds[known]) {
        case LineBreak::line:
            if (known == Tag::BR.index())
                ++w.pending_breaks; // every <br> counts
            else
                w.line_break(1);
            break;
        case LineBreak::paragraph:
            w.line_break(2);
            break;
        case LineBreak::space:
            w.put(' ');
            break;
        case LineBreak::skip_content: {
            std::string_view t = trim_ascii_end(tag);
            if (!closing && !(!t.empty() && t.back() == '/')) {
                // jump to the closing tag; it is removed on the next iteration
                std::string end_tag = "</" + std::string(tag_names[known]);
                std::size_t end = index_fold(value, end_tag, i);
                i = end == npos ? value.size() : end;
            }
            break;
        }
        case LineBreak::none:
            break;
        }
    }
    return std::move(w.out);
}

} // namespace detail

std::string html_to_text(std::string_view html) { return std::move(detail::clear_tags(html)).str(); }

std::string html_to_text_line_breaks(std::string_view html) { return detail::clear_tags_line_breaks(html); }

std::string Node::text_line_breaks() const {
    const detail::NodeData& d = detail::data(*this);
    if (d.inner_start < 0 || d.inner_end <= 0) return {};
    return detail::clear_tags_line_breaks(body());
}

} // namespace fasthtmlparser
