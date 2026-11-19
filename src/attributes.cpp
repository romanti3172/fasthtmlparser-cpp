#include "chars.hpp"
#include "document_data.hpp"

namespace fasthtmlparser {

namespace detail {
namespace {

bool starts_name(unsigned char c) { return is_space(c) || c == '"' || c == '\'' || c == '/' || c == '='; }
bool ends_name(unsigned char c) { return is_space(c) || c == '=' || c == '/' || c == '>'; }

/// The next place at or after from where the attribute name stands as a word of its own.
std::size_t next_word(std::string_view open, std::string_view attribute, std::size_t from) {
    while (from < open.size()) {
        std::size_t found = index_fold(open, attribute, from);
        if (found == npos) return npos;
        from = found;
        std::size_t after = from + attribute.size();
        if (starts_name(static_cast<unsigned char>(open[from - 1])) &&
            (after == open.size() || ends_name(static_cast<unsigned char>(open[after]))))
            return from;
        ++from;
    }
    return npos;
}

/// Whether the quote at quote stands right after an '='.
bool opens_value(std::string_view before, std::size_t quote) {
    if (quote == npos || quote == 0) return false;
    std::size_t k = quote - 1; // the tag starts with '<'
    while (k > 0 && is_space(static_cast<unsigned char>(before[k]))) --k;
    return before[k] == '=';
}

/// Whether the word at pos of an opening tag is an attribute name, judged by the bytes before it alone.
/// False means "not sure".
bool is_name_for_sure(std::string_view open, std::size_t pos) {
    // After whitespace, and not as the unquoted value in "x= word".
    std::size_t last = pos - 1;
    if (!is_space(static_cast<unsigned char>(open[last]))) return false;
    while (last > 0 && is_space(static_cast<unsigned char>(open[last]))) --last;
    if (open[last] == '=') return false;
    // Not inside a quoted value: it is enough that the last quote of each kind before the word does not open a value.
    std::string_view before = open.substr(0, last + 1);
    return !opens_value(before, before.rfind('"')) && !opens_value(before, before.rfind('\''));
}

/// The position of the '>' that ends the opening tag.
std::size_t open_tag_end(const Node& n) {
    const NodeData& d = data(n);
    std::int32_t end = d.inner_start > 0 && d.inner_end > 0 ? d.inner_start - 1 : d.end;
    return std::min(static_cast<std::size_t>(end), doc(n).source.size());
}

/// Reads the value that follows an attribute name at pos; value_start is npos when there is no '='. Returns the
/// position after it.
std::size_t read_value_at(std::string_view s, std::size_t pos, std::size_t end, std::size_t& value_start,
                          std::size_t& value_len) {
    std::size_t p = pos;
    while (p < end && is_space(static_cast<unsigned char>(s[p]))) ++p;
    if (p >= end || s[p] != '=') {
        value_start = npos;
        value_len = 0;
        return pos;
    }
    ++p;
    while (p < end && is_space(static_cast<unsigned char>(s[p]))) ++p;
    if (p < end && (s[p] == '"' || s[p] == '\'')) {
        std::size_t vs = p + 1;
        std::size_t close = find_byte(s.substr(0, end), s[p], vs);
        value_start = vs;
        if (close == npos) {
            value_len = end - vs;
            return end;
        }
        value_len = close - vs;
        return close + 1;
    }
    std::size_t vs = p;
    while (p < end && !is_space(static_cast<unsigned char>(s[p]))) ++p;
    value_start = vs;
    value_len = p - vs;
    return p;
}

} // namespace

AttrScanner::AttrScanner(const Node& n) noexcept : src_(doc(n).source), end_(open_tag_end(n)) {
    std::size_t i = static_cast<std::size_t>(data(n).start) + 1; // skip '<' and the name
    while (i < end_) {
        unsigned char c = static_cast<unsigned char>(src_[i]);
        if (is_space(c) || c == '/' || c == '>') break;
        ++i;
    }
    pos_ = i;
}

bool AttrScanner::next(ScannedAttr& a) noexcept {
    for (;;) {
        while (pos_ < end_ && (is_space(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '/')) ++pos_;
        if (pos_ >= end_) return false;
        std::size_t name_start = pos_;
        while (pos_ < end_) {
            unsigned char c = static_cast<unsigned char>(src_[pos_]);
            if (is_space(c) || c == '=' || c == '/' || c == '>') break;
            ++pos_;
        }
        std::size_t name_len = pos_ - name_start;
        if (name_len == 0) {
            ++pos_; // a stray '=': skip it
            continue;
        }
        a.name_start = name_start;
        a.name_len = name_len;
        pos_ = read_value_at(src_, pos_, end_, a.value_start, a.value_len);
        return true;
    }
}

bool attr_equals(std::string_view raw, std::string_view value) {
    if (needs_decoding(raw)) return decode(raw, true) == value;
    return raw == value;
}

} // namespace detail

using detail::data;
using detail::doc;
using detail::npos;

std::string_view Node::attr_raw(std::string_view attribute) const noexcept {
    const detail::NodeData& d = data(*this);
    // a comment has no attributes, whatever its text looks like
    if (attribute.empty() || d.key == Tag::COMMENT.index()) return {};
    std::string_view src = doc(*this).source;
    std::size_t start = static_cast<std::size_t>(d.start);
    std::size_t end = detail::open_tag_end(*this);
    if (end <= start) return {};
    std::string_view open = src.substr(start, end - start); // "<name attributes", without the closing '>'

    // The first place where the name stands as a word of its own. Most tags have none.
    std::size_t pos = detail::next_word(open, attribute, 1);
    if (pos == npos) return {};

    // That word is the attribute unless it is inside the value of another one (title="see id=5", x= id).
    // Nearly always that is ruled out at once, and the value is read in place.
    bool first_word = d.key >= 0 && pos == detail::tag_names[d.key].size() + 2;
    if (first_word || detail::is_name_for_sure(open, pos)) {
        std::size_t vs, vl;
        detail::read_value_at(src, start + pos + attribute.size(), end, vs, vl);
        return vs == npos ? std::string_view() : src.substr(vs, vl);
    }

    // Otherwise the attributes are read one by one, up to the one that starts at that word.
    detail::AttrScanner s(*this);
    detail::ScannedAttr a;
    while (s.next(a)) {
        std::size_t name = a.name_start - start;
        // a word the scanner has passed was inside a value: the next one takes its place
        while (pos < name) {
            pos = detail::next_word(open, attribute, pos + 1);
            if (pos == npos) return {};
        }
        if (pos == name && a.name_len == attribute.size()) return s.value(a);
    }
    return {};
}

CowString Node::attr(std::string_view name) const { return detail::decode(attr_raw(name), true); }

bool Node::has_class(std::string_view class_name) const {
    if (class_name.empty()) return false;
    std::string_view raw = attr_raw(attr::CLASS);
    CowString decoded = detail::needs_decoding(raw) ? detail::decode(raw, true) : CowString(raw);
    std::string_view v = decoded;
    if (v.size() == class_name.size()) return detail::equal_fold(v, class_name);
    if (class_name.size() > v.size()) return false;
    std::size_t from = 0;
    while (from + class_name.size() <= v.size()) {
        std::size_t i = detail::index_fold(v, class_name, from);
        if (i == npos) return false;
        std::size_t after = i + class_name.size();
        if ((i == 0 || detail::is_space(static_cast<unsigned char>(v[i - 1]))) &&
            (after == v.size() || detail::is_space(static_cast<unsigned char>(v[after]))))
            return true;
        from = i + 1;
    }
    return false;
}

std::vector<Attribute> Node::attrs() const {
    std::vector<Attribute> out;
    if (data(*this).key == Tag::COMMENT.index()) return out;
    detail::AttrScanner s(*this);
    detail::ScannedAttr a;
    while (s.next(a)) out.push_back({s.name(a), s.value(a)});
    return out;
}

} // namespace fasthtmlparser
