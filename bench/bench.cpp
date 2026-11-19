// Benchmarks of fasthtmlparser against lexbor (CSS selectors), Gumbo (tree walk), and libxml2 (XPath) on the
// same pages as the .NET, Go and Rust benchmarks. Each library must extract the same records first.
//
//     cmake -S bench -B bench/build -G Ninja -DCMAKE_BUILD_TYPE=Release
//     cmake --build bench/build && bench/build/fasthtmlparser_bench
//
// With --check it only checks the records; --filter text runs only the rows that contain text.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory_resource>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <fasthtmlparser/fasthtmlparser.hpp>

extern "C" {
#include <gumbo.h>
#include <lexbor/css/css.h>
#include <lexbor/html/html.h>
#include <lexbor/selectors/selectors.h>
}

#ifdef BENCH_LIBXML2
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#endif


namespace {

std::string read(const std::string& name) {
    std::ifstream f(std::string(TESTDATA) + "/" + name, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

struct Book {
    std::string title, price, rating;
    bool operator==(const Book&) const = default;
};

struct Quote {
    std::string text, author, tags;
    bool operator==(const Quote&) const = default;
};

std::string last_word(std::string_view s) {
    std::size_t i = s.rfind(' ');
    return std::string(i == std::string_view::npos ? s : s.substr(i + 1));
}

std::string trim(std::string_view s) {
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; };
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return std::string(s);
}

/// Keeps a result alive so the compiler cannot drop the work.
volatile std::size_t sink;
template <class T>
void keep(const T& v) {
    sink = sink + reinterpret_cast<std::uintptr_t>(&v) % 7;
}

// ----- fasthtmlparser -----

namespace fhp = fasthtmlparser;
using fhp::Tag;

std::vector<Book> books_fast(std::string_view page) {
    auto doc = fhp::Document::parse(page);
    std::vector<Book> out;
    for (fhp::Node pod : doc.find_all(fhp::by(Tag::ARTICLE).class_name("product_pod"))) {
        Book b;
        if (auto h = pod.find(fhp::by(Tag::H3)))
            if (auto a = h->find(fhp::by(Tag::A))) b.title = a->attr("title").str();
        if (auto p = pod.find(fhp::by(Tag::P).class_name("price_color"))) b.price = p->text().str();
        if (auto p = pod.find(fhp::by(Tag::P).class_name("star-rating"))) b.rating = last_word(p->attr("class"));
        out.push_back(std::move(b));
    }
    return out;
}

constexpr const char* pods_xpath = "//article[contains(concat(' ', @class, ' '), ' product_pod ')]";
constexpr const char* title_xpath = ".//h3/a/@title";
constexpr const char* price_xpath = ".//p[contains(concat(' ', @class, ' '), ' price_color ')]";
constexpr const char* rating_xpath = ".//p[contains(concat(' ', @class, ' '), ' star-rating ')]/@class";

struct FastQueries {
    fhp::CssQuery pods_css = css("article.product_pod");
    fhp::CssQuery title_css = css("h3 a");
    fhp::CssQuery price_css = css("p.price_color");
    fhp::CssQuery rating_css = css("p.star-rating");
    fhp::XPathQuery pods_xpath_q = xpath(pods_xpath);
    fhp::XPathQuery title_xpath_q = xpath(title_xpath);
    fhp::XPathQuery price_xpath_q = xpath(price_xpath);
    fhp::XPathQuery rating_xpath_q = xpath(rating_xpath);
    fhp::CssQuery quote_css = css("div.quote");
    fhp::CssQuery text_css = css("span.text");
    fhp::CssQuery author_css = css("small.author");
    fhp::CssQuery tag_css = css("a.tag");

    static fhp::CssQuery css(const char* s) { return std::move(fhp::CssQuery::compile(s)).value(); }
    static fhp::XPathQuery xpath(const char* s) { return std::move(fhp::XPathQuery::compile(s)).value(); }
};

std::vector<Book> books_fast_css(const FastQueries& q, std::string_view page) {
    auto doc = fhp::Document::parse(page);
    std::vector<Book> out;
    for (fhp::Node pod : q.pods_css.all(doc)) {
        Book b;
        if (auto a = q.title_css.first_in(pod)) b.title = a->attr("title").str();
        if (auto p = q.price_css.first_in(pod)) b.price = p->text().str();
        if (auto p = q.rating_css.first_in(pod)) b.rating = last_word(p->attr("class"));
        out.push_back(std::move(b));
    }
    return out;
}

std::vector<Book> books_fast_xpath(const FastQueries& q, std::string_view page) {
    auto doc = fhp::Document::parse(page);
    std::vector<Book> out;
    for (fhp::Node pod : q.pods_xpath_q.nodes(doc)) {
        Book b;
        b.title = q.title_xpath_q.value_from(pod).value_or("");
        b.price = q.price_xpath_q.value_from(pod).value_or("");
        b.rating = last_word(q.rating_xpath_q.value_from(pod).value_or(""));
        out.push_back(std::move(b));
    }
    return out;
}

std::string join_texts(const std::vector<fhp::Node>& nodes) {
    std::string out;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (i) out.push_back(',');
        out += nodes[i].text().view();
    }
    return out;
}

std::vector<Quote> quotes_fast(std::string_view page) {
    auto doc = fhp::Document::parse(page);
    std::vector<Quote> out;
    for (fhp::Node q : doc.find_all(fhp::by(Tag::DIV).class_name("quote"))) {
        Quote r;
        if (auto n = q.find(fhp::by(Tag::SPAN).class_name("text"))) r.text = n->text().str();
        if (auto n = q.find(fhp::by(Tag::SMALL).class_name("author"))) r.author = n->text().str();
        r.tags = join_texts(q.find_all(fhp::by(Tag::A).class_name("tag")));
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<Quote> quotes_fast_css(const FastQueries& fq, std::string_view page) {
    auto doc = fhp::Document::parse(page);
    std::vector<Quote> out;
    for (fhp::Node q : fq.quote_css.all(doc)) {
        Quote r;
        if (auto n = fq.text_css.first_in(q)) r.text = n->text().str();
        if (auto n = fq.author_css.first_in(q)) r.author = n->text().str();
        r.tags = join_texts(fq.tag_css.all_in(q));
        out.push_back(std::move(r));
    }
    return out;
}

// ----- lexbor (CSS selectors) -----

struct Lexbor {
    lxb_css_memory_t* memory = lxb_css_memory_create();
    lxb_css_parser_t* parser = lxb_css_parser_create();
    lxb_css_selectors_t* css_selectors = lxb_css_selectors_create();
    lxb_selectors_t* selectors = lxb_selectors_create();
    lxb_css_selector_list_t *pods, *title, *price, *rating, *quote, *text, *author, *tag;

    Lexbor() {
        lxb_css_memory_init(memory, 128);
        lxb_css_parser_init(parser, nullptr);
        lxb_css_parser_memory_set(parser, memory);
        lxb_css_selectors_init(css_selectors);
        lxb_css_parser_selectors_set(parser, css_selectors);
        lxb_selectors_init(selectors);
        pods = compile("article.product_pod");
        title = compile("h3 a");
        price = compile("p.price_color");
        rating = compile("p.star-rating");
        quote = compile("div.quote");
        text = compile("span.text");
        author = compile("small.author");
        tag = compile("a.tag");
    }
    ~Lexbor() {
        lxb_selectors_destroy(selectors, true);
        lxb_css_selectors_destroy(css_selectors, true);
        lxb_css_parser_destroy(parser, true);
        lxb_css_memory_destroy(memory, true);
    }

    lxb_css_selector_list_t* compile(const char* s) {
        return lxb_css_selectors_parse(parser, reinterpret_cast<const lxb_char_t*>(s), std::strlen(s));
    }

    static lxb_status_t collect(lxb_dom_node_t* node, lxb_css_selector_specificity_t, void* ctx) {
        static_cast<std::vector<lxb_dom_node_t*>*>(ctx)->push_back(node);
        return LXB_STATUS_OK;
    }

    std::vector<lxb_dom_node_t*> all(lxb_dom_node_t* root, lxb_css_selector_list_t* list) {
        std::vector<lxb_dom_node_t*> out;
        lxb_selectors_opt_set(selectors, LXB_SELECTORS_OPT_DEFAULT);
        lxb_selectors_find(selectors, root, list, collect, &out);
        return out;
    }

    lxb_dom_node_t* first(lxb_dom_node_t* root, lxb_css_selector_list_t* list) {
        std::vector<lxb_dom_node_t*> out;
        lxb_selectors_opt_set(selectors, LXB_SELECTORS_OPT_MATCH_FIRST);
        lxb_selectors_find(selectors, root, list, collect, &out);
        return out.empty() ? nullptr : out.front();
    }

    static std::string attr(lxb_dom_node_t* node, const char* name) {
        if (!node) return {};
        std::size_t len = 0;
        const lxb_char_t* v = lxb_dom_element_get_attribute(lxb_dom_interface_element(node),
                                                            reinterpret_cast<const lxb_char_t*>(name),
                                                            std::strlen(name), &len);
        return v ? std::string(reinterpret_cast<const char*>(v), len) : std::string();
    }

    static std::string text_of(lxb_dom_node_t* node) {
        if (!node) return {};
        std::size_t len = 0;
        lxb_char_t* t = lxb_dom_node_text_content(node, &len);
        if (!t) return {};
        std::string out = trim(std::string_view(reinterpret_cast<const char*>(t), len));
        lxb_dom_document_destroy_text(node->owner_document, t);
        return out;
    }
};

lxb_html_document_t* lexbor_parse(std::string_view page) {
    lxb_html_document_t* doc = lxb_html_document_create();
    lxb_html_document_parse(doc, reinterpret_cast<const lxb_char_t*>(page.data()), page.size());
    return doc;
}

std::vector<Book> books_lexbor(Lexbor& lx, std::string_view page) {
    lxb_html_document_t* doc = lexbor_parse(page);
    std::vector<Book> out;
    for (lxb_dom_node_t* pod : lx.all(lxb_dom_interface_node(doc), lx.pods)) {
        Book b;
        b.title = Lexbor::attr(lx.first(pod, lx.title), "title");
        b.price = Lexbor::text_of(lx.first(pod, lx.price));
        b.rating = last_word(Lexbor::attr(lx.first(pod, lx.rating), "class"));
        out.push_back(std::move(b));
    }
    lxb_html_document_destroy(doc);
    return out;
}

std::vector<Quote> quotes_lexbor(Lexbor& lx, std::string_view page) {
    lxb_html_document_t* doc = lexbor_parse(page);
    std::vector<Quote> out;
    for (lxb_dom_node_t* q : lx.all(lxb_dom_interface_node(doc), lx.quote)) {
        Quote r;
        r.text = Lexbor::text_of(lx.first(q, lx.text));
        r.author = Lexbor::text_of(lx.first(q, lx.author));
        auto tags = lx.all(q, lx.tag);
        for (std::size_t i = 0; i < tags.size(); ++i) {
            if (i) r.tags.push_back(',');
            r.tags += Lexbor::text_of(tags[i]);
        }
        out.push_back(std::move(r));
    }
    lxb_html_document_destroy(doc);
    return out;
}

// ----- Gumbo (a walk over the tree: Gumbo has no selectors) -----

bool gumbo_has_class(const GumboNode* n, std::string_view cls) {
    const GumboAttribute* a = gumbo_get_attribute(&n->v.element.attributes, "class");
    if (!a) return false;
    std::string_view v = a->value;
    std::size_t i = 0;
    while (i <= v.size()) {
        std::size_t j = v.find_first_of(" \t\n\r\f", i);
        if (j == std::string_view::npos) j = v.size();
        if (v.substr(i, j - i) == cls) return true;
        i = j + 1;
    }
    return false;
}

using GumboPredicate = std::function<bool(const GumboNode*)>;

void gumbo_find_all(const GumboNode* n, const GumboPredicate& pred, std::vector<const GumboNode*>& out, bool first) {
    if (n->type != GUMBO_NODE_ELEMENT && n->type != GUMBO_NODE_DOCUMENT) return;
    const GumboVector& children = n->type == GUMBO_NODE_ELEMENT ? n->v.element.children : n->v.document.children;
    for (unsigned i = 0; i < children.length; ++i) {
        const GumboNode* c = static_cast<const GumboNode*>(children.data[i]);
        if (c->type != GUMBO_NODE_ELEMENT) continue;
        if (pred(c)) {
            out.push_back(c);
            if (first) return;
        }
        gumbo_find_all(c, pred, out, first);
        if (first && !out.empty()) return;
    }
}

const GumboNode* gumbo_find(const GumboNode* n, const GumboPredicate& pred) {
    std::vector<const GumboNode*> out;
    gumbo_find_all(n, pred, out, true);
    return out.empty() ? nullptr : out.front();
}

GumboPredicate gumbo_by(GumboTag tag, const char* cls = nullptr) {
    return [tag, cls](const GumboNode* n) { return n->v.element.tag == tag && (!cls || gumbo_has_class(n, cls)); };
}

void gumbo_text(const GumboNode* n, std::string& out) {
    if (n->type == GUMBO_NODE_TEXT || n->type == GUMBO_NODE_WHITESPACE || n->type == GUMBO_NODE_CDATA) {
        out += n->v.text.text;
    } else if (n->type == GUMBO_NODE_ELEMENT) {
        for (unsigned i = 0; i < n->v.element.children.length; ++i)
            gumbo_text(static_cast<const GumboNode*>(n->v.element.children.data[i]), out);
    }
}

std::string gumbo_text_of(const GumboNode* n) {
    if (!n) return {};
    std::string out;
    gumbo_text(n, out);
    return trim(out);
}

std::string gumbo_attr(const GumboNode* n, const char* name) {
    if (!n) return {};
    const GumboAttribute* a = gumbo_get_attribute(&n->v.element.attributes, name);
    return a ? std::string(a->value) : std::string();
}

GumboOutput* gumbo_parse_page(std::string_view page) {
    return gumbo_parse_with_options(&kGumboDefaultOptions, page.data(), page.size());
}

std::vector<Book> books_gumbo(std::string_view page) {
    GumboOutput* g = gumbo_parse_page(page);
    std::vector<Book> out;
    std::vector<const GumboNode*> pods;
    gumbo_find_all(g->document, gumbo_by(GUMBO_TAG_ARTICLE, "product_pod"), pods, false);
    for (const GumboNode* pod : pods) {
        Book b;
        if (const GumboNode* h3 = gumbo_find(pod, gumbo_by(GUMBO_TAG_H3)))
            b.title = gumbo_attr(gumbo_find(h3, gumbo_by(GUMBO_TAG_A)), "title");
        b.price = gumbo_text_of(gumbo_find(pod, gumbo_by(GUMBO_TAG_P, "price_color")));
        b.rating = last_word(gumbo_attr(gumbo_find(pod, gumbo_by(GUMBO_TAG_P, "star-rating")), "class"));
        out.push_back(std::move(b));
    }
    gumbo_destroy_output(&kGumboDefaultOptions, g);
    return out;
}

std::vector<Quote> quotes_gumbo(std::string_view page) {
    GumboOutput* g = gumbo_parse_page(page);
    std::vector<Quote> out;
    std::vector<const GumboNode*> quotes;
    gumbo_find_all(g->document, gumbo_by(GUMBO_TAG_DIV, "quote"), quotes, false);
    for (const GumboNode* q : quotes) {
        Quote r;
        r.text = gumbo_text_of(gumbo_find(q, gumbo_by(GUMBO_TAG_SPAN, "text")));
        r.author = gumbo_text_of(gumbo_find(q, gumbo_by(GUMBO_TAG_SMALL, "author")));
        std::vector<const GumboNode*> tags;
        gumbo_find_all(q, gumbo_by(GUMBO_TAG_A, "tag"), tags, false);
        for (std::size_t i = 0; i < tags.size(); ++i) {
            if (i) r.tags.push_back(',');
            r.tags += gumbo_text_of(tags[i]);
        }
        out.push_back(std::move(r));
    }
    gumbo_destroy_output(&kGumboDefaultOptions, g);
    return out;
}

// ----- libxml2 (HTML parser and XPath) -----

#ifdef BENCH_LIBXML2
constexpr int html_options = HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_NONET;

htmlDocPtr libxml2_parse(std::string_view page) {
    return htmlReadMemory(page.data(), static_cast<int>(page.size()), nullptr, "UTF-8", html_options);
}

struct Libxml2Queries {
    xmlXPathCompExprPtr pods = compile(pods_xpath);
    xmlXPathCompExprPtr title = compile(title_xpath);
    xmlXPathCompExprPtr price = compile(price_xpath);
    xmlXPathCompExprPtr rating = compile(rating_xpath);
    xmlXPathCompExprPtr quote = compile("//div[contains(concat(' ', @class, ' '), ' quote ')]");
    xmlXPathCompExprPtr text = compile(".//span[contains(concat(' ', @class, ' '), ' text ')]");
    xmlXPathCompExprPtr author = compile(".//small[contains(concat(' ', @class, ' '), ' author ')]");
    xmlXPathCompExprPtr tag = compile(".//a[contains(concat(' ', @class, ' '), ' tag ')]");

    static xmlXPathCompExprPtr compile(const char* s) { return xmlXPathCompile(reinterpret_cast<const xmlChar*>(s)); }
    ~Libxml2Queries() {
        for (auto q : {pods, title, price, rating, quote, text, author, tag}) xmlXPathFreeCompExpr(q);
    }
};

std::vector<xmlNodePtr> libxml2_nodes(xmlXPathContextPtr ctx, xmlNodePtr from, xmlXPathCompExprPtr q) {
    ctx->node = from;
    xmlXPathObjectPtr r = xmlXPathCompiledEval(q, ctx);
    std::vector<xmlNodePtr> out;
    if (r && r->nodesetval)
        for (int i = 0; i < r->nodesetval->nodeNr; ++i) out.push_back(r->nodesetval->nodeTab[i]);
    xmlXPathFreeObject(r);
    return out;
}

std::string libxml2_value(xmlNodePtr n) {
    xmlChar* c = xmlNodeGetContent(n);
    std::string out = c ? trim(reinterpret_cast<const char*>(c)) : std::string();
    xmlFree(c);
    return out;
}

std::string libxml2_first_value(xmlXPathContextPtr ctx, xmlNodePtr from, xmlXPathCompExprPtr q) {
    ctx->node = from;
    xmlXPathObjectPtr r = xmlXPathCompiledEval(q, ctx);
    std::string out;
    if (r && r->nodesetval && r->nodesetval->nodeNr > 0) out = libxml2_value(r->nodesetval->nodeTab[0]);
    xmlXPathFreeObject(r);
    return out;
}

std::vector<Book> books_libxml2(const Libxml2Queries& q, std::string_view page) {
    htmlDocPtr doc = libxml2_parse(page);
    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
    std::vector<Book> out;
    for (xmlNodePtr pod : libxml2_nodes(ctx, reinterpret_cast<xmlNodePtr>(doc), q.pods)) {
        Book b;
        b.title = libxml2_first_value(ctx, pod, q.title);
        b.price = libxml2_first_value(ctx, pod, q.price);
        b.rating = last_word(libxml2_first_value(ctx, pod, q.rating));
        out.push_back(std::move(b));
    }
    xmlXPathFreeContext(ctx);
    xmlFreeDoc(doc);
    return out;
}

std::vector<Quote> quotes_libxml2(const Libxml2Queries& q, std::string_view page) {
    htmlDocPtr doc = libxml2_parse(page);
    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
    std::vector<Quote> out;
    for (xmlNodePtr quote : libxml2_nodes(ctx, reinterpret_cast<xmlNodePtr>(doc), q.quote)) {
        Quote r;
        r.text = libxml2_first_value(ctx, quote, q.text);
        r.author = libxml2_first_value(ctx, quote, q.author);
        auto tags = libxml2_nodes(ctx, quote, q.tag);
        for (std::size_t i = 0; i < tags.size(); ++i) {
            if (i) r.tags.push_back(',');
            r.tags += libxml2_value(tags[i]);
        }
        out.push_back(std::move(r));
    }
    xmlXPathFreeContext(ctx);
    xmlFreeDoc(doc);
    return out;
}
#endif

// ----- timing -----

using Clock = std::chrono::steady_clock;

/// The median time per call of five batches (as the Rust benchmark).
double measure(const std::function<void()>& f) {
    std::size_t iterations = 1;
    for (;;) {
        auto start = Clock::now();
        for (std::size_t i = 0; i < iterations; ++i) f();
        if (Clock::now() - start > std::chrono::milliseconds(100)) break;
        iterations *= 2;
    }
    std::vector<double> samples;
    for (int s = 0; s < 5; ++s) {
        auto start = Clock::now();
        for (std::size_t i = 0; i < iterations * 5; ++i) f();
        std::chrono::duration<double> elapsed = Clock::now() - start;
        samples.push_back(elapsed.count() / static_cast<double>(iterations * 5));
    }
    std::sort(samples.begin(), samples.end());
    return samples[2];
}

std::string filter; // --filter text: only the rows whose task or library contains it

void row(const std::string& task, const std::string& lib, std::size_t bytes, const std::function<void()>& f) {
    if (!filter.empty() && (task + " " + lib).find(filter) == std::string::npos) return;
    double t = measure(f);
    char line[256];
    if (bytes > 0)
        std::snprintf(line, sizeof line, "| %s | %s | %.1f µs | %.0f MB/s |", task.c_str(), lib.c_str(), t * 1e6,
                      static_cast<double>(bytes) / t / 1e6);
    else
        std::snprintf(line, sizeof line, "| %s | %s | %.1f µs |  |", task.c_str(), lib.c_str(), t * 1e6);
    std::cout << line << std::endl;
}

template <class T>
bool same(const char* what, const std::vector<T>& got, const std::vector<T>& want) {
    if (got == want) return true;
    std::cerr << what << ": the records differ (" << got.size() << " vs " << want.size() << ")\n";
    for (std::size_t i = 0; i < std::min(got.size(), want.size()); ++i)
        if (!(got[i] == want[i])) {
            std::cerr << "  first difference at " << i << "\n";
            break;
        }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    bool check_only = argc > 1 && std::string_view(argv[1]) == "--check";
    if (argc > 2 && std::string_view(argv[1]) == "--filter") filter = argv[2];
    std::string quotes = read("quotes/page1.html");
    std::string laptops = read("webscraper/laptops.html");
    std::string books = read("books/index.html");
    std::string books_x10;
    for (int i = 0; i < 10; ++i) books_x10 += books;
    std::string books_no_end = replace_all(replace_all(books, "</li>", ""), "</p>", "");

    FastQueries fq;
    Lexbor lx;
#ifdef BENCH_LIBXML2
    xmlInitParser();
    Libxml2Queries xq;
#endif

    // every library must extract the same records
    auto expected = books_fast(books);
    bool ok = expected.size() == 20;
    ok &= same("fasthtmlparser CSS", books_fast_css(fq, books), expected);
    ok &= same("fasthtmlparser XPath", books_fast_xpath(fq, books), expected);
    ok &= same("lexbor", books_lexbor(lx, books), expected);
    ok &= same("Gumbo", books_gumbo(books), expected);
    auto expected_quotes = quotes_fast(quotes);
    ok &= expected_quotes.size() == 10;
    ok &= same("fasthtmlparser CSS quotes", quotes_fast_css(fq, quotes), expected_quotes);
    ok &= same("lexbor quotes", quotes_lexbor(lx, quotes), expected_quotes);
    ok &= same("Gumbo quotes", quotes_gumbo(quotes), expected_quotes);
#ifdef BENCH_LIBXML2
    ok &= same("libxml2", books_libxml2(xq, books), expected);
    ok &= same("libxml2 quotes", quotes_libxml2(xq, quotes), expected_quotes);
#endif
    std::cout << "Book { title: \"" << expected[0].title << "\", price: \"" << expected[0].price << "\", rating: \""
              << expected[0].rating << "\" }\n"
              << expected_quotes[0].author << "\n\n";
    if (!ok) return 1;
    if (check_only) return 0;


    std::cout << "| task | library | time | throughput |\n|---|---|---:|---:|\n";
    std::vector<std::byte> arena_buffer(4 << 20); // the memory of one document at a time, reused
    const std::pair<const char*, const std::string*> pages[] = {
        {"parse quotes 11 KB", &quotes},
        {"parse laptops 38 KB", &laptops},
        {"parse books 51 KB", &books},
        {"parse books x10 510 KB", &books_x10},
        {"parse books, no </li> </p>", &books_no_end},
    };
    for (const auto& [name, page] : pages) {
        std::size_t n = page->size();
        row(name, "fasthtmlparser", n, [&] { keep(fhp::Document::parse(*page)); });
        row(name, "lexbor", n, [&] { lxb_html_document_destroy(lexbor_parse(*page)); });
        row(name, "Gumbo", n, [&] { gumbo_destroy_output(&kGumboDefaultOptions, gumbo_parse_page(*page)); });
#ifdef BENCH_LIBXML2
        row(name, "libxml2", n, [&] { xmlFreeDoc(libxml2_parse(*page)); });
#endif
    }
    row("parse books 51 KB, pmr arena", "fasthtmlparser", books.size(), [&] {
        std::pmr::monotonic_buffer_resource arena(arena_buffer.data(), arena_buffer.size());
        keep(fhp::Document::parse(books, {}, &arena));
    });
    row("parse books 51 KB, no copy", "fasthtmlparser", books.size(), [&] { keep(fhp::Document::parse_view(books)); });

    row("scrape 20 books", "fasthtmlparser find", 0, [&] { keep(books_fast(books)); });
    row("scrape 20 books", "fasthtmlparser CSS", 0, [&] { keep(books_fast_css(fq, books)); });
    row("scrape 20 books", "fasthtmlparser XPath", 0, [&] { keep(books_fast_xpath(fq, books)); });
    row("scrape 20 books", "lexbor CSS", 0, [&] { keep(books_lexbor(lx, books)); });
    row("scrape 20 books", "Gumbo tree walk", 0, [&] { keep(books_gumbo(books)); });
#ifdef BENCH_LIBXML2
    row("scrape 20 books", "libxml2 XPath", 0, [&] { keep(books_libxml2(xq, books)); });
#endif
    row("scrape 10 quotes", "fasthtmlparser find", 0, [&] { keep(quotes_fast(quotes)); });
    row("scrape 10 quotes", "fasthtmlparser CSS", 0, [&] { keep(quotes_fast_css(fq, quotes)); });
    row("scrape 10 quotes", "lexbor CSS", 0, [&] { keep(quotes_lexbor(lx, quotes)); });
    row("scrape 10 quotes", "Gumbo tree walk", 0, [&] { keep(quotes_gumbo(quotes)); });
#ifdef BENCH_LIBXML2
    row("scrape 10 quotes", "libxml2 XPath", 0, [&] { keep(quotes_libxml2(xq, quotes)); });
#endif

    return 0;
}
