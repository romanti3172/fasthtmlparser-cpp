#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <memory_resource>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "fasthtmlparser/fasthtmlparser.hpp"
#include "test_util.hpp"

using namespace fasthtmlparser;

namespace {

std::string testdata(const std::string& name) {
    return test::read_file(std::string(FASTHTMLPARSER_TESTDATA) + "/" + name);
}

std::string repeat(std::string_view s, std::size_t n) {
    std::string out;
    out.reserve(s.size() * n);
    for (std::size_t i = 0; i < n; ++i) out += s;
    return out;
}

} // namespace

// The example of the README.
TEST_CASE("readme") {
    std::string page = R"(<html><body>
  <article class="product_pod"><h3><a href="b/1" title="A Light in the Attic">A Light...</a></h3>
    <p class="price_color">£51.77</p><p class="star-rating Three"></p></article>
  <dl><dt>Price</dt><dd>£51.77</dd></dl>
</body></html>)";
    auto doc = fhp::Document::parse(page, "https://example.com/catalog/");

    // CSS selectors, as querySelectorAll / querySelector / matches
    for (fhp::Node pod : doc.css("article.product_pod")) {
        CHECK(pod.css_first("h3 a")->attr("title") == "A Light in the Attic");
        CHECK(pod.css_first("p.price_color")->text() == "£51.77");
    }
    fhp::Node pod = *doc.css_first("article");
    CHECK(pod.css("> h3").size() == 1); // the children of pod

    // XPath 1.0
    CHECK(doc.xpath_value("//dt[.='Price']/following-sibling::dd[1]") == "£51.77");
    CHECK(doc.xpath_values("//a[contains(@href, 'b/')]/@href") == std::vector<std::string>{"b/1"});
    CHECK(doc.xpath_value("count(//p)") == "2");

    // compile once, reuse on any number of pages and threads
    auto titles = fhp::XPathQuery::compile("//article//h3/a/@title");
    REQUIRE(titles);
    CHECK(titles->values(doc) == std::vector<std::string>{"A Light in the Attic"});
    auto prices = fhp::CssQuery::compile("p.price_color");
    REQUIRE(prices);
    CHECK(prices->all(doc).size() == 1);

    // search by name, class, id, attribute, text
    auto found = doc.find(fhp::by(fhp::Tag::ARTICLE).class_name("product_pod"));
    REQUIRE(found);
    CHECK(found->find(fhp::by(fhp::Tag::P).class_name("price_color"))->text() == "£51.77");
    CHECK(!doc.find(fhp::by_name("my-widget").class_name("x"))); // any name works as a string

    // one-call shortcuts
    auto dt = doc.find_by_name_text(fhp::Tag::DT, "Price");
    REQUIRE(dt);
    CHECK(dt->next_sibling_by_name(fhp::Tag::DD)->text() == "£51.77");

    // reading a tag
    fhp::Node a = *doc.css_first("h3 a");
    CHECK(a.text() == "A Light...");                                  // visible text, entities decoded
    CHECK(a.attr(fhp::attr::TITLE) == "A Light in the Attic");       // decoded value; attr_raw(): as written
    CHECK(a.href() == "https://example.com/catalog/b/1");            // absolute URL; src() for src
    CHECK(a.source() == R"(a href="b/1" title="A Light in the Attic")"); // the opening tag without '<' and '>'
    CHECK(a.name() == "a");
    CHECK(a.depth() == 4);
    std::string text = doc.find(fhp::by(fhp::Tag::BODY))->text_line_breaks(); // block elements → lines
    CHECK(text.rfind("A Light...\n\n£51.77", 0) == 0);

    // navigation
    CHECK(a.parent()->name() == "h3");
    CHECK(a.parent_by_name(fhp::Tag::ARTICLE) == pod);
    CHECK(pod.children().size() == 3);
    CHECK(dt->next_sibling()->name() == "dd");
}

TEST_CASE("shortcuts on the books page") {
    auto doc = Document::parse(testdata("books/index.html"), "https://books.toscrape.com/index.html");
    auto pods = doc.find_all_by_name_class(Tag::ARTICLE, "product_pod");
    REQUIRE(pods.size() == 20);
    Node first = pods[0];
    CHECK(first.find_by_name(Tag::H3)->find_by_name(Tag::A)->attribute_title() == "A Light in the Attic");
    CHECK(first.find_by_name_class(Tag::P, "price_color")->text() == "£51.77");
    CHECK(first.find_by_name_class_text_starts_with(Tag::P, "price_color", "£5")->attr(attr::CLASS) == "price_color");
    CHECK(first.find_by_name(Tag::A)->href() ==
          "https://books.toscrape.com/catalogue/a-light-in-the-attic_1000/index.html");
    CHECK(first.find_by_name(Tag::IMG)->src() ==
          "https://books.toscrape.com/media/cache/2c/da/2cdad67c44b002e7ead0cc35693c0e8b.jpg");
    CHECK(first.find_by_name(Tag::IMG)->parent_by_name(Tag::ARTICLE)->attr(attr::CLASS) == "product_pod");
    CHECK(first.find_by_name(Tag::H3)->next_sibling_by_name(Tag::DIV)->attr(attr::CLASS) == "product_price");
    CHECK(first_level_by_name(doc.find_all(any()), Tag::HTML).size() == 1);
    CHECK(first.is_name_class(Tag::ARTICLE, "product_pod"));
    CHECK(!first.is_name_class(Tag::DIV, "product_pod"));
    CHECK(!doc.find_by_name_text_starts_with(Tag::A, ""));
    CHECK(!doc.find_by_id(""));
    CHECK(first.find_by_name("ARTICLE") == std::nullopt); // inside the tag only
    CHECK(doc.find_by_name("Article") == first);           // names as strings, case-insensitive
}

// CR LF and a lone CR are LF in values, as a browser reads the page; a CR written as a reference stays.
TEST_CASE("line breaks are read as in a browser") {
    auto doc = Document::parse(
        "<meta name=\"d\" content=\"one\r\ntwo\rthree&#13;four\">"
        "<textarea>a\r\nb</textarea><pre>c\r\nd</pre><div class=\"x\r\ny\">t</div>");
    Node div = *doc.find(by(Tag::DIV));
    CHECK(doc.find(by(Tag::META))->attr("content") == "one\ntwo\nthree\rfour");
    CHECK(doc.find(by(Tag::TEXTAREA))->text() == "a\nb");
    CHECK(doc.find(by(Tag::PRE))->text() == "c\nd");
    CHECK(doc.xpath_value("//textarea/text()") == "a\nb");
    CHECK(doc.xpath_value("//meta/@content") == "one\ntwo\nthree\rfour");
    CHECK(div.attr_raw("class") == "x\r\ny"); // the source as written
    CHECK(div.has_class("y"));
    CHECK(div.is(any().attr("class", "x\ny")));
    CHECK(doc.css(R"(div[class="x\a y"])").size() == 1);
}

// Descendant and ancestor steps on nodes inside one another stay linear.
TEST_CASE("XPath on nested nodes is linear") {
    const std::size_t many = 100000;
    std::vector<std::pair<std::string, std::string>> pages = {
        {"unclosed nesting", repeat("<div><span>", many / 2) + R"(<a href="/x">link</a>)"},
        {"many links in deep nesting", repeat("<span>", 1000) + repeat(R"(<a href="/x">x</a>)", many)},
        {"nested lists", repeat(R"(<div><ul><li><a href="/x">x</a>)", many / 4)},
    };
    for (const auto& [name, page] : pages) {
        CAPTURE(name);
        auto doc = Document::parse(page);
        auto links = doc.find_all(by(Tag::A));
        auto start = std::chrono::steady_clock::now();
        std::size_t descendants = doc.xpath("//*//*//a").size();
        std::size_t in_div = doc.xpath("//div//a[@href]").size();
        std::string ancestors = doc.xpath_value("count(//a/ancestor::*)");
        std::size_t self = doc.xpath("//a/ancestor-or-self::*[@href]").size();
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));

        std::size_t want_descendants = 0, want_in_div = 0;
        std::set<std::size_t> seen;
        for (const Node& a : links) {
            if (a.depth() >= 2) ++want_descendants;
            if (a.parent_matching(by(Tag::DIV))) ++want_in_div;
            for (auto p = a.parent(); p && !seen.count(p->index()); p = p->parent()) seen.insert(p->index());
        }
        CHECK(descendants == want_descendants);
        CHECK(in_div == want_in_div);
        CHECK(ancestors == std::to_string(seen.size()));
        CHECK(self == links.size());
    }
}

TEST_CASE("Tag::from_name") {
    for (std::size_t i = 0; i < detail::tag_count; ++i) {
        std::string name(detail::tag_names[i]);
        std::string upper = name;
        for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        CHECK(Tag::from_name(name) == Tag(static_cast<std::uint16_t>(i)));
        CHECK(Tag::from_name(upper) == Tag(static_cast<std::uint16_t>(i)));
    }
    for (std::string_view s : {"", "x", "divv", "di", "custom-tag", "d\xC4\xB0v", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})
        CHECK(!Tag::from_name(s));
    CHECK(Tag::DIV.name() == "div");
    CHECK(Tag::COMMENT.name() == "!--");
}

TEST_CASE("compile errors are values, the string methods throw") {
    auto bad_css = CssQuery::compile("div >> p");
    CHECK(!bad_css);
    CHECK(bad_css.error().find("invalid CSS selector") != std::string::npos);
    auto bad_xpath = XPathQuery::compile("//div[");
    CHECK(!bad_xpath);
    CHECK(bad_xpath.error().find("invalid XPath") != std::string::npos);

    auto doc = Document::parse("<div><p>1</p><p>2</p></div>");
    CHECK_THROWS_AS(doc.css("div >> p"), std::invalid_argument);
    CHECK_THROWS_AS(doc.xpath("//div["), std::invalid_argument);
    try {
        doc.xpath("//div[");
        FAIL("no error");
    } catch (const std::invalid_argument& e) {
        CHECK(std::string(e.what()).find("unexpected end of the expression") != std::string::npos);
    }
    CHECK_THROWS_AS(doc.xpath("count(//p)"), std::logic_error); // a number, not tags
    CHECK(doc.xpath_value("count(//p)") == "2");
    auto count = XPathQuery::compile("count(//p)");
    CHECK(!count->is_node_set());
}

TEST_CASE("values: views of the page or decoded strings") {
    auto doc = Document::parse(R"(<a title="plain" alt="a &amp; b">x &lt; y</a>)");
    Node a = doc.node(0);
    CowString plain = a.attr("title");
    CHECK(!plain.is_owned());
    CHECK(plain.data() >= doc.source().data());
    CHECK(plain.data() < doc.source().data() + doc.source().size());
    CowString decoded = a.attr("alt");
    CHECK(decoded.is_owned());
    CHECK(decoded == "a & b");
    CHECK(a.text() == "x < y");
    CowString moved = std::move(decoded);
    CHECK(moved == "a & b");
    std::string s = std::move(moved).str();
    CHECK(s == "a & b");
    CHECK(a.attr("missing").empty());
    CHECK(a.attr_raw("alt") == "a &amp; b");
    auto attrs = a.attrs();
    REQUIRE(attrs.size() == 2);
    CHECK(attrs[1].name == "alt");
    CHECK(attrs[1].value == "a &amp; b");
    CHECK(decode_html("&copy;2024 &notit; &#x41;&#65") == "\xC2\xA9" "2024 \xC2\xAC" "it; AA");
}

TEST_CASE("an arena for the documents, and parse_view") {
    std::string page = testdata("books/index.html");
    std::pmr::monotonic_buffer_resource arena(1 << 20);
    std::size_t total = 0;
    for (int i = 0; i < 10; ++i) {
        auto doc = Document::parse(page, "https://books.toscrape.com/", &arena);
        total += doc.css("article.product_pod").size();
    }
    CHECK(total == 200);

    auto view = Document::parse_view(page);
    CHECK(view.source().data() == page.data()); // no copy
    auto copy = Document::parse(page);
    CHECK(copy.source().data() != page.data());
    CHECK(view.size() == copy.size());

    // a moved document keeps its nodes valid (the data lives on the heap)
    Node pod = *copy.css_first("article");
    Document moved = std::move(copy);
    CHECK(pod.find_by_name(Tag::H3)->text() == "A Light in the ...");
    CHECK(moved.size() == view.size());
}

TEST_CASE("one document, many threads") {
    auto doc = Document::parse(testdata("books/index.html"));
    std::vector<std::thread> threads;
    std::vector<std::size_t> counts(8);
    for (std::size_t t = 0; t < counts.size(); ++t) {
        threads.emplace_back([&, t] {
            std::size_t c = 0;
            for (int i = 0; i < 50; ++i) {
                c += doc.css("article.product_pod:nth-of-type(odd) h3 > a").size();
                c += doc.xpath("//article[contains(@class, 'product_pod')]//a/@href").size() == 0 ? 0 : 1;
            }
            counts[t] = c;
        });
    }
    for (auto& t : threads) t.join();
    for (std::size_t c : counts) CHECK(c == counts[0]);
}

TEST_CASE("XML documents") {
    auto doc = Document::parse(R"(<?xml version="1.0"?><rss><channel><item><title>A</title><link>/a</link></item>
<item><title>B</title><dc:creator>me</dc:creator></item></channel></rss>)");
    CHECK(doc.xpath_values("//item/title") == std::vector<std::string>{"A", "B"});
    CHECK(doc.css("dc\\:creator").size() == 1);
    CHECK(doc.xpath_value("//dc:creator") == "me");
    CHECK(doc.xpath_value("count(//item)") == "2");
}

TEST_CASE("number formatting in XPath") {
    auto doc = Document::parse("<p>1</p>");
    CHECK(doc.xpath_value("1 div 3") == "0.3333333333333333");
    CHECK_THROWS_AS(doc.xpath_value("1e0"), std::invalid_argument); // no exponents in XPath 1.0
    CHECK(doc.xpath_value("1000000 * 1000000 * 1000000 * 1000") == "1000000000000000000000");
    CHECK(doc.xpath_value("0.0000001 * 1") == "0.0000001");
    CHECK(doc.xpath_value("-0.5 * 3") == "-1.5");
    CHECK(doc.xpath_value("1 div 0") == "Infinity");
    CHECK(doc.xpath_value("0 div 0") == "NaN");
    CHECK(doc.xpath_value("round(2.5)") == "3");
    CHECK(doc.xpath_value("round(-2.5)") == "-2");
    CHECK(doc.xpath_value("string-length('\xF0\x9F\x98\x80')") == "2");
    CHECK(doc.xpath_value("substring('h\xC3\xA9llo', 2, 3)") == "\xC3\xA9ll");
    CHECK(doc.xpath_value("upper-case('stra\xC3\x9F" "e')") == "STRASSE");
    CHECK(doc.xpath_value("lower-case('\xCE\x9F\xCE\xA3')") == "\xCE\xBF\xCF\x82"); // final sigma
}
