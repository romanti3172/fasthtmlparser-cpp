#include <cstring>

#include "chars.hpp"
#include "css_tree.hpp"
#include "document_data.hpp"

namespace fasthtmlparser {

namespace detail {

DocumentData::~DocumentData() {
    delete css_tree.load(std::memory_order_acquire);
    if (owned_source) mr->deallocate(owned_source, owned_size, 1);
}

void DocumentDeleter::operator()(DocumentData* d) const noexcept {
    std::pmr::polymorphic_allocator<DocumentData> alloc(d->mr);
    std::allocator_traits<decltype(alloc)>::destroy(alloc, d);
    alloc.deallocate(d, 1);
}

bool is_element(const DocumentData& d, std::size_t i) noexcept {
    const NodeData& n = d.nodes[i];
    if (n.key >= 0)
        return n.key != Tag::COMMENT.index() && n.key != Tag::DOCTYPE.index() &&
               n.key != Tag::XML_DECLARATION.index();
    std::string_view name = d.name(i);
    return !name.empty() && name[0] != '!' && name[0] != '?';
}

namespace {

DocumentData* make(std::string_view html, std::string_view url, std::pmr::memory_resource* mr, bool copy) {
    if (!mr) mr = std::pmr::get_default_resource();
    std::pmr::polymorphic_allocator<DocumentData> alloc(mr);
    DocumentData* d = alloc.allocate(1);
    try {
        std::allocator_traits<decltype(alloc)>::construct(alloc, d, mr);
    } catch (...) {
        alloc.deallocate(d, 1);
        throw;
    }
    std::unique_ptr<DocumentData, DocumentDeleter> guard(d);
    if (copy && !html.empty()) {
        d->owned_source = static_cast<char*>(mr->allocate(html.size(), 1));
        d->owned_size = html.size();
        std::memcpy(d->owned_source, html.data(), html.size());
        d->source = std::string_view(d->owned_source, html.size());
    } else {
        d->source = html;
    }
    parse(d->source, d->nodes, d->custom_names);
    if (!url.empty()) {
        d->url.assign(url);
        d->base = absolute_url(url);
    }
    return guard.release();
}

std::optional<Node> find_in(const DocumentData& d, std::size_t first, std::int32_t end, const Query& q) noexcept {
    for (std::size_t i = first; i < d.nodes.size(); ++i) {
        if (d.nodes[i].end > end) break;
        Node n = d.at(i);
        if (q.matches(n)) return n;
    }
    return std::nullopt;
}

std::vector<Node> find_all_in(const DocumentData& d, std::size_t first, std::int32_t end, const Query& q) {
    std::vector<Node> result;
    for (std::size_t i = first; i < d.nodes.size(); ++i) {
        if (d.nodes[i].end > end) break;
        Node n = d.at(i);
        if (q.matches(n)) result.push_back(n);
    }
    return result;
}

/// Whether the decoded value starts with prefix; it allocates only when the raw value has '&' or CR.
bool attr_starts_with(std::string_view raw, std::string_view prefix) {
    if (needs_decoding(raw)) return decode(raw, true).view().substr(0, prefix.size()) == prefix;
    return raw.substr(0, prefix.size()) == prefix;
}

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

} // namespace
} // namespace detail

using detail::data;
using detail::doc;

// ----- Document -----

Document::Document() : d_(detail::make({}, {}, nullptr, false)) {}

Document Document::parse(std::string_view html, std::string_view url, std::pmr::memory_resource* mr) {
    return Document(detail::make(html, url, mr, true));
}

Document Document::parse_view(std::string_view html, std::string_view url, std::pmr::memory_resource* mr) {
    return Document(detail::make(html, url, mr, false));
}

std::string_view Document::url() const noexcept { return d_->url; }
std::string_view Document::source() const noexcept { return d_->source; }
std::size_t Document::size() const noexcept { return d_->nodes.size(); }

std::optional<Node> Document::find(const Query& q) const noexcept {
    return detail::find_in(*d_, 0, INT32_MAX, q);
}

std::vector<Node> Document::find_all(const Query& q) const { return detail::find_all_in(*d_, 0, INT32_MAX, q); }

Node NodeRange::iterator::operator*() const noexcept { return d_->at(i_); }

// ----- Node -----

std::optional<Tag> Node::tag() const noexcept {
    std::int16_t key = data(*this).key;
    if (key < 0) return std::nullopt;
    return Tag(static_cast<std::uint16_t>(key));
}

std::string_view Node::name() const noexcept { return doc_->name(index_); }

std::size_t Node::depth() const noexcept { return static_cast<std::size_t>(data(*this).depth); }

std::pair<std::size_t, std::size_t> Node::pos() const noexcept {
    const detail::NodeData& d = data(*this);
    return {static_cast<std::size_t>(d.start), static_cast<std::size_t>(d.end)};
}

std::string_view Node::body() const noexcept {
    const detail::NodeData& d = data(*this);
    if (d.inner_start < 0 || d.inner_end <= 0 || d.inner_start > d.inner_end + 1) return {};
    std::string_view src = doc_->source;
    return detail::trim(src.substr(static_cast<std::size_t>(d.inner_start),
                                   static_cast<std::size_t>(d.inner_end + 1 - d.inner_start)));
}

std::string_view Node::outer_html() const noexcept {
    const detail::NodeData& d = data(*this);
    if (d.inner_start <= 0 || d.inner_end <= 0) return {};
    std::string_view src = doc_->source;
    return detail::trim(
        src.substr(static_cast<std::size_t>(d.start), static_cast<std::size_t>(d.end + 1 - d.start)));
}

// The visible text in one line, as jsoup's text(): <br>, block elements and table cells separate words, inline tags
// do not, whitespace and line breaks collapse to one space; inside <pre> whitespace is kept; script, style, textarea,
// title, ... are their content as written.
CowString Node::text() const {
    std::string_view b = body();
    int key = data(*this).key;
    bool leaf = !has_children();
    // script, style, textarea, title, ... without child tags: their content is text as written
    if (leaf && !detail::raw_text_end(key).empty()) return detail::decode(b, false);
    // no child tags, no '<' and no whitespace to collapse: the body is the text
    bool plain = leaf && detail::find_byte(b, '<') == detail::npos;
    if (plain && !detail::needs_collapse(b)) return detail::decode(b, false);
    // <pre> and what is in it keep their whitespace (looked up only when there is whitespace to keep)
    bool pre = detail::in_pre(*this);
    if (plain && pre) return detail::decode(b, false);
    return detail::clear_tags(b, pre);
}

bool Node::has_children() const noexcept {
    std::size_t i = index_;
    return i + 1 < doc_->nodes.size() && doc_->nodes[i + 1].start < data(*this).end;
}

std::string_view Node::source() const noexcept {
    const detail::NodeData& d = data(*this);
    std::string_view src = doc_->source;
    std::size_t start = static_cast<std::size_t>(d.start);
    std::size_t end = static_cast<std::size_t>(d.end);
    std::size_t length;
    if (d.inner_start > 0 && d.inner_end > 0)
        length = static_cast<std::size_t>(d.inner_start) - start - 2;
    else if (end > 0 && src[end - 1] == '/')
        length = end - start - 2;
    else
        length = end - start - 1;
    return detail::trim(detail::slice(src, start + 1, start + 1 + length));
}

std::optional<Node> Node::parent() const noexcept {
    std::int32_t p = data(*this).parent;
    if (p < 0) return std::nullopt;
    return doc_->at(static_cast<std::size_t>(p));
}

std::vector<Node> Node::children() const {
    const detail::NodeData& d = data(*this);
    const auto& nodes = doc_->nodes;
    std::vector<Node> children;
    for (std::size_t i = index_ + 1; i < nodes.size() && nodes[i].end <= d.end; ++i)
        if (nodes[i].depth == d.depth + 1) children.push_back(doc_->at(i));
    return children;
}

std::optional<Node> Node::next() const noexcept {
    std::size_t i = index_ + 1;
    if (i >= doc_->nodes.size()) return std::nullopt;
    return doc_->at(i);
}

std::optional<Node> Node::previous() const noexcept {
    if (index_ == 0) return std::nullopt;
    return doc_->at(index_ - 1);
}

std::optional<Node> Node::next_matching(const Query& q) const noexcept {
    std::int32_t end = data(*this).end;
    for (std::size_t i = index_ + 1; i < doc_->nodes.size(); ++i) {
        Node n = doc_->at(i);
        if (doc_->nodes[i].start > end && q.matches(n)) return n;
    }
    return std::nullopt;
}

std::optional<Node> Node::next_sibling_matching(const Query& q) const noexcept {
    // deeper tags are inside the following siblings; a shallower one ends the parent
    std::int32_t depth = data(*this).depth;
    const auto& nodes = doc_->nodes;
    for (std::size_t i = index_ + 1; i < nodes.size(); ++i) {
        if (nodes[i].depth < depth) break;
        if (nodes[i].depth == depth && q.matches(doc_->at(i))) return doc_->at(i);
    }
    return std::nullopt;
}

std::optional<Node> Node::previous_sibling_matching(const Query& q) const noexcept {
    // deeper tags are inside the previous siblings; a shallower one is the parent
    std::int32_t depth = data(*this).depth;
    const auto& nodes = doc_->nodes;
    for (std::size_t i = index_; i > 0;) {
        --i;
        if (nodes[i].depth < depth) break;
        if (nodes[i].depth == depth && q.matches(doc_->at(i))) return doc_->at(i);
    }
    return std::nullopt;
}

std::optional<Node> Node::parent_matching(const Query& q) const noexcept {
    for (std::optional<Node> p = parent(); p; p = p->parent())
        if (q.matches(*p)) return p;
    return std::nullopt;
}

std::string_view Node::text_after_tag_until_next_one() const noexcept {
    std::size_t from = static_cast<std::size_t>(data(*this).end) + 1;
    std::string_view src = doc_->source;
    if (from >= src.size()) return {};
    std::string_view rest = src.substr(from);
    return rest.substr(0, detail::find_byte(rest, '<'));
}

CowString Node::text_no_tags_after_tag_until_next_one() const {
    return detail::clear_tags(text_after_tag_until_next_one());
}

std::string_view Node::text_after_until(const Query& q) const noexcept {
    std::optional<Node> until = next_matching(q);
    if (!until) return {};
    std::size_t from = static_cast<std::size_t>(data(*this).end) + 1;
    std::size_t to = static_cast<std::size_t>(data(*until).start);
    return doc_->source.substr(from, to - from);
}

std::optional<Node> Node::find(const Query& q) const noexcept {
    return detail::find_in(*doc_, index_ + 1, data(*this).end, q);
}

std::vector<Node> Node::find_all(const Query& q) const {
    return detail::find_all_in(*doc_, index_ + 1, data(*this).end, q);
}

// ----- Query -----

Query by_name(std::string_view name) noexcept {
    Query q;
    int key = detail::lookup_tag(name);
    if (key >= 0) q.key_ = static_cast<std::int16_t>(key + 1);
    q.name_ = name;
    return q;
}

bool Query::matches(const Node& n) const noexcept {
    const detail::NodeData& d = data(n);
    if (key_ != 0) {
        if (d.key != key_ - 1 && (d.key >= 0 || name_.empty() || !detail::equal_fold(n.name(), name_))) return false;
    } else if (!name_.empty() && !detail::equal_fold(n.name(), name_)) {
        return false;
    }
    if (has_ == 0) return true;
    // the checks below may allocate (decoded values); an allocation failure counts as no match
    try {
        if ((has_ & has_id_) && (id_.empty() || !detail::attr_equals(n.attr_raw(attr::ID), id_))) return false;
        if ((has_ & has_attr_) && !detail::attr_equals(n.attr_raw(attr_), attr_value_)) return false;
        if ((has_ & has_class_) && !n.has_class(class_)) return false;
        if ((has_ & has_id_starts_with_) && !detail::attr_starts_with(n.attr_raw(attr::ID), id_starts_with_))
            return false;
        if ((has_ & has_source_) && n.source() != source_) return false;
        if ((has_ & has_body_) && n.body() != body_) return false;
        if ((has_ & has_body_starts_with_) && !detail::starts_with(n.body(), body_starts_with_)) return false;
        // the text last: it is the most expensive check
        if ((has_ & has_text_) && !detail::is_text(n, text_)) return false;
        if ((has_ & has_text_starts_with_) && !detail::starts_with(n.text(), text_starts_with_)) return false;
    } catch (...) {
        return false;
    }
    return true;
}

// ----- first_level -----

namespace {

std::vector<Node> first_level_matching(const std::vector<Node>& nodes, const Query& q, bool check) {
    std::vector<Node> out;
    if (nodes.empty()) return out;
    std::size_t level = nodes.front().depth();
    for (const Node& n : nodes)
        if (n.depth() == level && (!check || q.matches(n))) out.push_back(n);
    return out;
}

} // namespace

std::vector<Node> first_level(const std::vector<Node>& nodes) { return first_level_matching(nodes, any(), false); }

std::vector<Node> first_level_by_name(const std::vector<Node>& nodes, Name name) {
    return first_level_matching(nodes, name.query, true);
}

std::vector<Node> first_level_by_name_class(const std::vector<Node>& nodes, Name name, std::string_view class_name) {
    return first_level_matching(nodes, name.query.class_name(class_name), true);
}

} // namespace fasthtmlparser
