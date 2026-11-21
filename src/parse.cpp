// A page is scanned once in fast mode: tags are matched to their end tags and nothing else. Well-formed markup needs
// nothing more. Where the HTML5 optional-end-tag rules matter (<li>one<li>two</ul>), tags are left open inside an
// element: when its end tag (or the end of the document) finds them on the stack and one of them is a rule target
// (li, p, td, tr, option, ...), only that part is scanned again in rules mode, from the outermost such tag up to
// that end tag. Everything before it was parsed right and is kept.
//
// Port of ParsingCore.cs from z3l.FastHtmlParser; positions are byte offsets into the UTF-8 source.

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "chars.hpp"
#include "document_data.hpp"


namespace fasthtmlparser {

namespace detail {
namespace {

/// Nesting deeper than this is flattened, as browsers do (Blink: 512).
constexpr std::size_t max_depth = 512;
/// The parts scanned again with the optional-end-tag rules may add up to this many times the source length.
constexpr std::size_t repair_budget_factor = 8;

using TagSet = std::array<bool, tag_count>;

/// The slot of a lower-case name in Tables::lookup.
std::size_t name_hash(std::string_view lower) noexcept {
    std::uint32_t h = 0;
    for (unsigned char c : lower) h = h * 31 + c;
    return ((h + static_cast<std::uint32_t>(lower.size())) * 2654435769u) >> 22; // 10 bits: lookup_size
}


struct Rule {
    TagSet targets{};
    TagSet stops{};
    std::vector<int> target_keys;

    Rule(std::initializer_list<Tag> t, std::initializer_list<std::initializer_list<Tag>> s) {
        for (Tag x : t) {
            targets[x.index()] = true;
            target_keys.push_back(x.index());
        }
        for (auto& list : s)
            for (Tag x : list) stops[x.index()] = true;
    }

    /// Whether a target is open at all: in well-formed markup it is not.
    bool any_open(const std::vector<int>& open_by_key) const noexcept {
        for (int k : target_keys)
            if (open_by_key[k] > 0) return true;
        return false;
    }
};

TagSet tag_set(std::initializer_list<Tag> tags) {
    TagSet set{};
    for (Tag t : tags) set[t.index()] = true;
    return set;
}

struct Tables {
    std::vector<Rule> rules;
    std::size_t p_rule = 0;
    std::array<int, tag_count> implied_end_rules{};
    TagSet is_rule_target{};
    TagSet closes_p{};
    TagSet void_tags{};
    std::array<std::string, tag_count> raw_text_ends{};
    // tag name → Tag: open addressing over the lower-case names
    static constexpr std::size_t lookup_size = 1024;
    std::array<std::int16_t, lookup_size> lookup{};

    Tables() {
        implied_end_rules.fill(-1);
        // Elements that limit the scope of the rules (HTML5 "has an element in scope").
        std::initializer_list<Tag> scope_stops = {Tag::HTML,     Tag::BODY,   Tag::TABLE,  Tag::CAPTION,
                                                  Tag::TD,       Tag::TH,     Tag::TEMPLATE, Tag::OBJECT,
                                                  Tag::APPLET,   Tag::MARQUEE, Tag::BUTTON};
        auto add = [&](Rule rule, std::initializer_list<Tag> on) {
            rules.push_back(std::move(rule));
            for (Tag t : on) implied_end_rules[t.index()] = static_cast<int>(rules.size() - 1);
        };
        add(Rule({Tag::LI}, {scope_stops, {Tag::UL, Tag::OL, Tag::MENU}}), {Tag::LI});
        add(Rule({Tag::DT, Tag::DD}, {scope_stops, {Tag::DL}}), {Tag::DT, Tag::DD});
        std::initializer_list<Tag> table_stops = {Tag::HTML, Tag::BODY, Tag::TEMPLATE, Tag::TABLE};
        add(Rule({Tag::TD, Tag::TH}, {table_stops, {Tag::TR, Tag::TBODY, Tag::THEAD, Tag::TFOOT}}), {Tag::TD, Tag::TH});
        add(Rule({Tag::TR, Tag::TD, Tag::TH}, {table_stops, {Tag::TBODY, Tag::THEAD, Tag::TFOOT}}), {Tag::TR});
        add(Rule({Tag::TBODY, Tag::THEAD, Tag::TFOOT, Tag::TR, Tag::TD, Tag::TH, Tag::CAPTION}, {table_stops}),
            {Tag::TBODY, Tag::THEAD, Tag::TFOOT});
        std::initializer_list<Tag> select_stops = {Tag::HTML, Tag::BODY, Tag::TEMPLATE, Tag::SELECT, Tag::DATALIST};
        add(Rule({Tag::OPTION}, {select_stops, {Tag::OPTGROUP}}), {Tag::OPTION});
        add(Rule({Tag::OPTION, Tag::OPTGROUP}, {select_stops}), {Tag::OPTGROUP});

        // <p> is ended by the start of a block element (HTML5 "close a p element").
        rules.push_back(Rule({Tag::P}, {scope_stops}));
        p_rule = rules.size() - 1;

        for (int r : implied_end_rules)
            if (r >= 0)
                for (int k : rules[r].target_keys) is_rule_target[k] = true;

        closes_p = tag_set({Tag::ADDRESS, Tag::ARTICLE, Tag::ASIDE,  Tag::BLOCKQUOTE, Tag::CENTER, Tag::DETAILS,
                            Tag::DIALOG,  Tag::DIR,     Tag::DIV,    Tag::DL,         Tag::FIELDSET, Tag::FIGCAPTION,
                            Tag::FIGURE,  Tag::FOOTER,  Tag::FORM,   Tag::H1,         Tag::H2,     Tag::H3,
                            Tag::H4,      Tag::H5,      Tag::H6,     Tag::HEADER,     Tag::HGROUP, Tag::HR,
                            Tag::LISTING, Tag::MAIN,    Tag::MENU,   Tag::NAV,        Tag::OL,     Tag::P,
                            Tag::PRE,     Tag::SEARCH,  Tag::SECTION, Tag::SUMMARY,   Tag::TABLE,  Tag::UL,
                            Tag::LI,      Tag::DD,      Tag::DT,     Tag::PLAINTEXT,  Tag::XMP});

        // HTML5 void elements (incl. obsolete ones the parsing rules also treat as void) and declarations.
        void_tags = tag_set({Tag::AREA,  Tag::BASE,   Tag::BR,      Tag::COL,     Tag::EMBED,    Tag::HR,
                             Tag::IMG,   Tag::INPUT,  Tag::LINK,    Tag::META,    Tag::SOURCE,   Tag::TRACK,
                             Tag::WBR,   Tag::BASEFONT, Tag::BGSOUND, Tag::FRAME, Tag::KEYGEN,   Tag::PARAM,
                             Tag::ISINDEX, Tag::DOCTYPE, Tag::XML_DECLARATION});

        // "</script", "</style", ... for the elements whose content is raw text (HTML5 RAWTEXT / RCDATA).
        for (Tag t : {Tag::SCRIPT, Tag::STYLE, Tag::TEXTAREA, Tag::TITLE, Tag::XMP, Tag::IFRAME, Tag::NOEMBED,
                      Tag::NOFRAMES})
            raw_text_ends[t.index()] = "</" + std::string(t.name());

        lookup.fill(-1);
        for (std::size_t i = 0; i < tag_count; ++i) {
            std::size_t slot = name_hash(tag_names[i]);
            while (lookup[slot] >= 0) slot = (slot + 1) & (lookup_size - 1);
            lookup[slot] = static_cast<std::int16_t>(i);
        }
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

/// Per-tag data needed while parsing: matching an end tag scans these without touching the nodes.
struct TagState {
    std::int32_t key;       // Tag, or -1 for a custom name
    std::int32_t depth;
    std::int32_t prev_open; // the open tags as a linked list in document order
    bool self_closed;       // <x/>: stays in the open list (a later </x> ends it), but does not nest anything
};

/// Scratch buffers reused between parses on a thread: only the result is allocated per document.
struct Scratch {
    std::vector<NodeData> nodes;
    std::vector<TagState> states;
    std::vector<std::int32_t> stack;
    std::vector<int> open_by_key;
    std::vector<int> open_in_list;
};

/// An XML document (RSS, Atom, sitemap, XHTML) is parsed by XML rules: elements are closed only by an end tag or "/>".
bool is_xml(std::string_view src) {
    std::size_t i = 0;
    for (;;) {
        if (i < src.size() && is_space(static_cast<unsigned char>(src[i]))) {
            ++i;
        } else if (src.substr(i, 3) == "\xEF\xBB\xBF") {
            i += 3;
        } else {
            break;
        }
    }
    return starts_with_ignore_ascii_case(src.substr(i), "<?xml");
}

std::string_view extract_tag_name(std::string_view tag_source) {
    std::string_view t = trim(tag_source);
    if (t.substr(0, 3) == "!--") return t.substr(0, 3);
    // The name ends at whitespace or '/': <br/> is "br", as <br /> is.
    std::size_t i = 0;
    while (i < t.size() && !is_space(static_cast<unsigned char>(t[i])) && t[i] != '/') ++i;
    return t.substr(0, i);
}

/// The first of '<', '=', '>' in s[from, to), or npos.
std::size_t find_tag_char(std::string_view s, std::size_t from, std::size_t to) {
    const char* p = s.data();
    std::size_t i = from;
#ifdef FHP_SSE2
    const __m128i lt = _mm_set1_epi8('<'), eq = _mm_set1_epi8('='), gt = _mm_set1_epi8('>');
    while (i + 16 <= to) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
        __m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, lt), _mm_cmpeq_epi8(v, eq)), _mm_cmpeq_epi8(v, gt));
        unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(m));
        if (mask) return i + static_cast<std::size_t>(std::countr_zero(mask));
        i += 16;
    }
#endif
    for (; i < to; ++i) {
        char c = p[i];
        if (c == '<' || c == '=' || c == '>') return i;
    }
    return npos;
}

struct ParseState {
    std::string_view src;
    bool xml;
    NodeData* nodes;
    TagState* states;
    std::vector<std::int32_t>& stack; // indexes of the open tags that contain the following ones
    std::vector<int>& open_by_key;    // per Tag, how many are on the stack: kept only in rules mode
    std::vector<int>& open_in_list;   // per Tag, how many are in the open list: an end tag with none is skipped
    std::pmr::vector<std::pmr::string>& custom_names;
    std::unordered_map<std::string, std::uint32_t, StringHash, std::equal_to<>> custom; // lower-case custom name → index into custom_names
    std::vector<int> custom_open; // per custom name, how many tags with it are in the open list
    std::size_t count = 0;
    std::size_t stack_top = 0;
    std::int32_t last_open = -1;
    int open_p = 0;          // <p> tags on the stack, kept in both modes
    std::size_t budget = 0;  // bytes that may still be scanned again by repair
    std::string lower_buffer;

    const Tables& t = tables();

    /// Parses src[from, to) (from is outside a tag), appending tags at count. Tags still open at to stay on the
    /// stack. Returns whether it stopped inside a tag, and where that tag starts.
    std::pair<bool, std::size_t> scan(bool rules, std::size_t from, std::size_t to) {
        const char* s = src.data();
        bool tag_started = false;
        std::size_t tag_start = 0;
        std::string_view window = src.substr(0, to);

        std::size_t i = from;
        while (i < to) {
            if (!tag_started) {
                // Outside a tag only '<' matters: jump to it.
                std::size_t lt = find_byte(window, '<', i);
                if (lt == npos) break;
                i = lt;

                if (window.substr(i + 1, 3) == "!--") {
                    // <!-- ... -->: one "!--" tag; its content is not markup, it does not nest anything.
                    std::size_t comment_end = find_comment_end(window, i + 4);
                    auto [parent, depth] = parent_and_depth();
                    nodes[count] = NodeData{no_custom,
                                            parent,
                                            static_cast<std::int32_t>(i),
                                            static_cast<std::int32_t>(comment_end),
                                            static_cast<std::int32_t>(comment_end + 1),
                                            0,
                                            depth,
                                            static_cast<std::int16_t>(Tag::COMMENT.index())};
                    states[count] = TagState{Tag::COMMENT.index(), depth, -1, false};
                    ++count;
                    i = comment_end + 1;
                    continue;
                }

                // As in HTML5, '<' starts markup only before a letter, '/', '!' or '?': "a < b" is text.
                if (!is_markup_start(window, i)) {
                    ++i;
                    continue;
                }
                tag_started = true;
                tag_start = i + 1;
                ++i;
                continue;
            }

            // Inside a tag only '<', '=' and '>' matter.
            std::size_t next = find_tag_char(window, i, to);
            if (next == npos) break;
            i = next;

            if (s[i] == '<') {
                // An unfinished tag: start over at this '<' (it may also start a comment).
                tag_started = false;
                continue;
            }
            if (s[i] == '=') {
                // A quoted value may contain '<' and '>' (data-x="<p>...</p>"): jump over it.
                std::size_t q = i + 1;
                while (q < to && is_space(static_cast<unsigned char>(s[q]))) ++q;
                if (q < to && (s[q] == '"' || s[q] == '\'')) {
                    std::size_t close = find_byte(window, s[q], q + 1);
                    if (close != npos) i = close;
                }
            } else { // '>'
                if (s[tag_start] == '/')
                    end_tag(rules, tag_start, i);
                else
                    i = start_tag(rules, to, tag_start, i);
                tag_started = false;
            }
            ++i;
        }
        return {tag_started, tag_start};
    }

    std::pair<std::int32_t, std::int32_t> parent_and_depth() const {
        if (stack_top > 0) {
            std::int32_t parent = stack[stack_top - 1];
            return {parent, states[parent].depth + 1};
        }
        return {-1, 0};
    }

    /// Matches the end tag src[tag_start-1 ..= gt] to the innermost open tag with that name.
    void end_tag(bool rules, std::size_t tag_start, std::size_t gt) {
        std::string_view name = trim(src.substr(tag_start + 1, gt - tag_start - 1));
        if (name.empty()) return;
        // Resolve the name once, then compare ints instead of a lookup per candidate.
        int key = lookup_tag(name);
        // The rules may have ended the matched tag itself (<li><div><li>x</div>: the second <li> ends the first one
        // together with its <div>, as in a browser): the end tag is matched again, to what is open now.
        if (match_end(rules, key, name, tag_start, gt, true)) match_end(rules, key, name, tag_start, gt, false);
    }

    /// Ends the innermost open tag with the end tag's name. Returns true when it scanned a part again with the
    /// optional-end-tag rules instead, and the end tag is to be matched once more.
    bool match_end(bool rules, int key, std::string_view name, std::size_t tag_start, std::size_t gt,
                   bool can_repair) {
        // A name that no open tag has is a stray end tag: it is skipped without scanning the open tags.
        std::uint32_t closing = no_custom;
        if (key >= 0) {
            if (open_in_list[key] <= 0) return false;
        } else {
            if (custom.empty()) return false;
            auto it = custom.find(lower_key(name, lower_buffer));
            if (it == custom.end() || custom_open[it->second] <= 0) return false;
            closing = it->second;
        }

        std::int32_t j = last_open;
        while (j >= 0) {
            std::int32_t next = states[j].prev_open;
            bool matched = key >= 0 ? states[j].key == key : states[j].key < 0 && nodes[j].custom == closing;
            if (!matched) {
                j = next;
                continue;
            }

            std::int64_t inner_end = static_cast<std::int64_t>(tag_start) - 2;

            // Tags left open inside it: if the optional-end-tag rules apply to one of them, scan its part again
            // with the rules (fast mode only; rules mode has applied them).
            if (can_repair && !rules && !xml && stack_top > 0 && stack[stack_top - 1] > j) {
                std::size_t above = stack_top - 1;
                while (above > 0 && stack[above - 1] > j) --above;
                std::size_t target;
                if (first_misnested(above, stack_top, target)) {
                    repair(target, tag_start - 1);
                    return true;
                }
            }

            nodes[j].inner_end = static_cast<std::int32_t>(inner_end);
            nodes[j].end = static_cast<std::int32_t>(gt);

            while (stack_top > 0 && stack[stack_top - 1] >= j) {
                --stack_top;
                int popped = states[stack[stack_top]].key;
                if (popped == Tag::P.index()) --open_p;
                if (rules && popped >= 0) --open_by_key[popped];
            }

            // It leaves the open list with everything opened after it; the tags opened inside it and never closed
            // end right before this end tag, as in a browser.
            truncate_open_list(j, inner_end);
            return false;
        }
        return false;
    }

    /// Adds the tag src[tag_start-1 ..= gt]; returns the position to continue from (gt, or before a raw-text end
    /// tag).
    std::size_t start_tag(bool rules, std::size_t to, std::size_t tag_start, std::size_t gt) {
        std::string_view tag_source = src.substr(tag_start, gt - tag_start);
        std::string_view name = extract_tag_name(tag_source);

        int key = lookup_tag(name);
        std::uint32_t custom_index = no_custom;
        // Custom names (svg elements, web components, ...): one string per distinct name per document.
        if (key < 0) custom_index = intern_name(name);

        if (!xml && key >= 0) {
            // Optional end tags (HTML5): <li> ends an open <li>, <td> an open <td>, ...
            int rule_index = t.implied_end_rules[key];
            if (rule_index >= 0) {
                const Rule& rule = t.rules[rule_index];
                if (rules) {
                    if (rule.any_open(open_by_key))
                        close_in_scope(rule, static_cast<std::int64_t>(tag_start) - 2, true);
                } else if (stack_top > 0) {
                    // The common case of a left-out end tag is caught right here: the innermost open tag is a target.
                    int top = states[stack[stack_top - 1]].key;
                    if (top >= 0 && rule.targets[top]) apply_rule_in_fast_mode(rule, tag_start);
                }
            }

            // A block element ends an open <p>. Usually no <p> is open and this is one compare.
            if (open_p > 0 && t.closes_p[key]) {
                const Rule& p_rule = t.rules[t.p_rule];
                if (rules)
                    close_in_scope(p_rule, static_cast<std::int64_t>(tag_start) - 2, true);
                else
                    apply_rule_in_fast_mode(p_rule, tag_start);
            }
        }

        // the innermost open tag is the parent
        auto [parent, depth] = parent_and_depth();

        std::size_t index = count;
        nodes[index] = NodeData{custom_index,
                                parent,
                                static_cast<std::int32_t>(tag_start - 1),
                                static_cast<std::int32_t>(gt),
                                static_cast<std::int32_t>(gt + 1),
                                0,
                                depth,
                                static_cast<std::int16_t>(key)};

        // Void elements (<br>, <img>, <meta>, ...) and declarations never contain anything.
        // Neither does markup named "!..." or "?..." (<!doctype>, <?xml ...?>, <?php ...?>, <![if IE]>), in HTML and in
        // XML alike: a bogus comment in a browser, a declaration or processing instruction in XML. The name starts with
        // an ASCII letter, '!' or '?' (is_markup_start), and '!' and '?' sort before the letters: one compare.
        bool is_void = static_cast<unsigned char>(src[tag_start]) <= '?' || (!xml && key >= 0 && t.void_tags[key]);
        std::string_view trimmed = trim_ascii_end(tag_source);
        bool self_closing = is_void || (!trimmed.empty() && trimmed.back() == '/');

        if (is_void) {
            states[index] = TagState{key, depth, -1, false};
        } else {
            states[index] = TagState{key, depth, last_open, self_closing};
            last_open = static_cast<std::int32_t>(index);
            if (key >= 0)
                ++open_in_list[key];
            else
                adjust_custom(custom_index, 1);
        }
        ++count;

        if (!self_closing) {
            // Deeper than max_depth the tag is not nested: it stays in the open list for its end tag.
            if (stack_top < max_depth) {
                if (stack_top == stack.size()) stack.resize(stack.size() * 2);
                stack[stack_top++] = static_cast<std::int32_t>(index);
                if (key == Tag::P.index()) ++open_p;
                if (rules && key >= 0) ++open_by_key[key];
            }

            // <script>, <style>, <textarea>, <title>, ...: the content is text, not markup;
            // continue right before the matching end tag.
            if (!xml && key >= 0) {
                std::string_view end = t.raw_text_ends[key];
                if (!end.empty()) return find_raw_text_end(src.substr(0, to), gt + 1, end) - 1;
            }
        }
        return gt;
    }

    // ----- implied end tags -----

    /// Among the tags left open at stack positions [from, to), the outermost one that a rule should have ended.
    bool first_misnested(std::size_t from, std::size_t to, std::size_t& result) const {
        for (std::size_t s = from; s + 1 < to; ++s) {
            int key = states[stack[s]].key;
            if (key < 0 || !t.is_rule_target[key]) continue;
            for (std::size_t inner = s + 1; inner < to; ++inner) {
                int inner_key = states[stack[inner]].key;
                if (inner_key < 0) continue;
                int rule = t.implied_end_rules[inner_key];
                if (rule >= 0 && t.rules[rule].targets[key]) {
                    result = s;
                    return true;
                }
            }
        }
        return false;
    }

    /// Scans again with the optional-end-tag rules from the tag at stack position stack_pos up to region_end: the
    /// tags from it on are replaced, the ones before it are kept.
    void repair(std::size_t stack_pos, std::size_t region_end) {
        std::int32_t first = stack[stack_pos];
        std::size_t from = static_cast<std::size_t>(nodes[first].start);

        // Past the budget the part is left as it is, so that parsing stays linear on any markup.
        std::size_t cost = region_end > from ? region_end - from : 0;
        if (cost > budget) return;
        budget -= cost;

        truncate_open_list(first, -1);
        stack_top = stack_pos;
        count = static_cast<std::size_t>(first);

        std::fill(open_by_key.begin(), open_by_key.end(), 0);
        open_p = 0;
        for (std::size_t s = 0; s < stack_pos; ++s) {
            int key = states[stack[s]].key;
            if (key < 0) continue;
            ++open_by_key[key];
            if (key == Tag::P.index()) ++open_p;
        }
        scan(true, from, region_end);
    }

    /// Removes the tags from index first on from the open list (they are its tail). With end >= 0 the ones that have
    /// no end tag of their own end there.
    void truncate_open_list(std::int32_t first, std::int64_t end) {
        std::int32_t k = last_open;
        while (k >= first && k >= 0) {
            const TagState state = states[k];
            if (state.key >= 0)
                --open_in_list[state.key];
            else
                adjust_custom(nodes[k].custom, -1);
            NodeData& node = nodes[k];
            if (end >= 0 && !state.self_closed && node.inner_end == 0) {
                node.inner_end = static_cast<std::int32_t>(end);
                node.end = static_cast<std::int32_t>(end);
            }
            k = state.prev_open;
        }
        last_open = k;
    }

    void adjust_custom(std::uint32_t c, int delta) {
        if (c != no_custom) custom_open[c] += delta;
    }

    /// The lower-case name as one entry per document.
    std::uint32_t intern_name(std::string_view name) {
        if (name.empty()) return no_custom;
        std::string_view key = lower_key(name, lower_buffer);
        auto it = custom.find(key);
        if (it != custom.end()) return it->second;
        std::string lower(key);
        auto index = static_cast<std::uint32_t>(custom_names.size());
        custom_names.emplace_back(lower);
        custom.emplace(std::move(lower), index);
        custom_open.push_back(0);
        return index;
    }

    /// The stack position of the outermost target of rule below the first stop tag.
    bool scope_start(const Rule& rule, std::size_t& from) const {
        bool found = false;
        for (std::size_t s = stack_top; s > 0;) {
            --s;
            int k = states[stack[s]].key;
            if (k < 0) continue;
            if (rule.targets[k]) {
                from = s;
                found = true;
            } else if (rule.stops[k]) {
                break;
            }
        }
        return found;
    }

    /// Applies rule for the tag that starts at tag_start. If a tag it ends was itself misnested, that part is
    /// scanned again with the rules first.
    void apply_rule_in_fast_mode(const Rule& rule, std::size_t tag_start) {
        std::size_t from;
        if (!scope_start(rule, from)) return;
        std::size_t m;
        if (first_misnested(from, stack_top, m)) repair(m, tag_start - 1);
        close_in_scope(rule, static_cast<std::int64_t>(tag_start) - 2, false);
    }

    /// Ends the outermost target found below the first stop tag together with everything opened inside it.
    void close_in_scope(const Rule& rule, std::int64_t end, bool by_key) {
        std::size_t from;
        if (!scope_start(rule, from)) return;
        std::int32_t first = stack[from];
        while (stack_top > from) {
            --stack_top;
            int key = states[stack[stack_top]].key;
            if (key == Tag::P.index()) --open_p;
            if (by_key && key >= 0) --open_by_key[key];
        }
        truncate_open_list(first, end);
    }
};

} // namespace

std::size_t find_raw_text_end(std::string_view s, std::size_t from, std::string_view end_tag) noexcept {
    while (from < s.size()) {
        std::size_t pos = index_fold(s, end_tag, from);
        if (pos == npos) break;
        std::size_t after = pos + end_tag.size();
        // "</script>", "</script >", "</script/>" end it; "</scripts" does not.
        if (after >= s.size() || s[after] == '>' || s[after] == '/' || is_space(static_cast<unsigned char>(s[after])))
            return pos;
        from = after;
    }
    return s.size();
}

std::size_t find_comment_end(std::string_view s, std::size_t content_start) noexcept {
    std::string_view content = s.substr(std::min(content_start, s.size()));
    if (!content.empty() && content[0] == '>') return content_start;
    if (content.substr(0, 2) == "->") return content_start + 1;
    std::size_t from = 0;
    while (from < content.size()) {
        std::size_t dashes = content.find("--", from);
        if (dashes == npos) break;
        std::size_t pos = dashes + 2;
        if (pos < content.size() && content[pos] == '>') return content_start + pos;
        if (pos + 1 < content.size() && content[pos] == '!' && content[pos + 1] == '>') return content_start + pos + 1;
        from = dashes + 1;
    }
    return s.size() - 1;
}

int lookup_tag(std::string_view name) noexcept {
    std::size_t n = name.size();
    if (n == 0 || n > 32) return -1;
    char buf[32];
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (c >= 0x80) return -1; // known names are ASCII
        buf[i] = to_lower_ascii(static_cast<char>(c));
    }
    std::string_view lower(buf, n);
    const Tables& t = tables();
    for (std::size_t slot = name_hash(lower);; slot = (slot + 1) & (Tables::lookup_size - 1)) {
        int k = t.lookup[slot];
        if (k < 0) return -1;
        std::string_view known = tag_names[k];
        if (known.size() == n && std::memcmp(known.data(), buf, n) == 0) return k;
    }
}

std::string_view raw_text_end(int key) noexcept {
    if (key < 0) return {};
    return tables().raw_text_ends[key];
}

void parse(std::string_view src, std::pmr::vector<NodeData>& out, std::pmr::vector<std::pmr::string>& custom_names) {
    // Every tag starts with its own '<', so this is an exact upper bound.
    std::size_t max_tags = count_byte(src, '<');
    if (max_tags == 0) return;

    thread_local Scratch sc;
    if (sc.nodes.size() < max_tags) {
        sc.nodes.resize(max_tags);
        sc.states.resize(max_tags);
    }
    if (sc.stack.empty()) sc.stack.resize(64);
    sc.open_by_key.assign(tag_count, 0);
    sc.open_in_list.assign(tag_count, 0);

    ParseState st{src, is_xml(src), sc.nodes.data(), sc.states.data(), sc.stack, sc.open_by_key, sc.open_in_list,
                  custom_names, {}, {}, 0, 0, -1, 0, 0, {}, tables()};
    st.budget = repair_budget_factor * src.size();

    auto [tag_started, tag_start] = st.scan(false, 0, src.size());

    // The end of the document ends every tag still open (a truncated page keeps its text), right before an
    // unfinished tag if the source stops inside one.
    std::int64_t region_end = tag_started ? static_cast<std::int64_t>(tag_start) - 1
                                          : static_cast<std::int64_t>(src.size());
    if (!st.xml) {
        std::size_t s;
        if (st.first_misnested(0, st.stack_top, s)) st.repair(s, static_cast<std::size_t>(region_end));
    }
    st.truncate_open_list(0, region_end - 1);

    out.assign(sc.nodes.begin(), sc.nodes.begin() + static_cast<std::ptrdiff_t>(st.count));
    for (NodeData& n : out)
        if (n.inner_end == 0) n.inner_start = 0; // not closed: no body
}

} // namespace detail

std::optional<Tag> Tag::from_name(std::string_view name) noexcept {
    int key = detail::lookup_tag(name);
    if (key < 0) return std::nullopt;
    return Tag(static_cast<std::uint16_t>(key));
}

} // namespace fasthtmlparser
