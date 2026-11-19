// Conformance with z3l.FastHtmlParser (.NET): the expected output in testdata/conformance comes from the .NET version
// for the same pages (the tools/conformance dumper of the Rust port). Tag names, parents, depths, positions (UTF-8
// byte offsets), texts, attributes, navigation and links must be exactly the same, and so must the results of every
// XPath and CSS query of testdata/conformance/queries.json.

#include <doctest/doctest.h>

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "document_data.hpp" // the inner positions of the tags
#include "fasthtmlparser/fasthtmlparser.hpp"
#include "test_util.hpp"

namespace fs = std::filesystem;
using namespace fasthtmlparser;
using test::Json;

namespace {

/// The page URL the .NET dump uses.
constexpr std::string_view page_url = "https://Example.com:443/catalog/page-1.html?sort=asc#top";

fs::path testdata() { return fs::path(FASTHTMLPARSER_TESTDATA) / "conformance"; }

std::vector<fs::path> files(const fs::path& dir, const std::string& ext) {
    std::vector<fs::path> v;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ext) v.push_back(e.path());
    std::sort(v.begin(), v.end());
    return v;
}

long long index_of(const std::optional<Node>& n) { return n ? static_cast<long long>(n->index()) : -1; }

std::string short_text(const std::string& s) { return s.size() > 300 ? s.substr(0, 300) + "..." : s; }

/// [count, hash] of the values (sha256 of the values joined with \x01, the first 16 hex digits), or "throws" when
/// the evaluation throws (as .NET does).
Json result(const std::function<std::vector<std::string>()>& values) {
    std::vector<std::string> v;
    try {
        v = values();
    } catch (const std::exception&) {
        return Json::str("throws");
    }
    std::string joined;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) joined.push_back('\x01');
        joined += v[i];
    }
    return Json::arr({Json::str(std::to_string(v.size())), Json::str(test::sha256_hex(joined).substr(0, 16))});
}

std::vector<std::string> indexes(const std::vector<Node>& nodes) {
    std::vector<std::string> v;
    for (const Node& n : nodes) v.push_back(std::to_string(n.index()));
    return v;
}

} // namespace

TEST_CASE("tags as .NET") {
    auto inputs = files(testdata() / "input", ".html");
    REQUIRE(!inputs.empty());
    std::vector<std::string> failed;
    std::size_t tags = 0;
    for (const auto& input : inputs) {
        std::string name = input.stem().string();
        std::string source = test::read_file(input.string());
        Json expected = test::parse_json(test::read_file((testdata() / "expected" / (name + ".json")).string()));
        Document doc = Document::parse(source, page_url);

        std::vector<std::string> problems;
        if (doc.size() != expected.items.size())
            problems.push_back("tags: " + std::to_string(doc.size()) + ", .NET: " + std::to_string(expected.items.size()));
        for (std::size_t i = 0; i < std::min(doc.size(), expected.items.size()) && problems.size() < 10; ++i) {
            const Json& e = expected[i];
            Node n = doc.node(i);
            ++tags;
            auto pos = n.pos();
            auto parent = n.parent();
            const detail::NodeData& d = detail::data(n);
            std::string got = Json::str(std::string(n.name())).dump() + " parent " + std::to_string(index_of(parent)) +
                              " depth " + std::to_string(n.depth()) + " " + std::to_string(pos.first) + ".." +
                              std::to_string(pos.second) + " inner " + std::to_string(d.inner_start) + ".." +
                              std::to_string(d.inner_end);
            std::string want = e["name"].dump() + " parent " + e["parent"].text + " depth " + e["depth"].text + " " +
                               e["start"].text + ".." + e["end"].text + " inner " + e["innerStart"].text + ".." +
                               e["innerEnd"].text;
            if (got != want) {
                problems.push_back("[" + std::to_string(i) + "]\n  c++:  " + got + "\n  .net: " + want);
                continue;
            }
            auto check = [&](const char* key, const Json& value) {
                if (e[key].dump() != value.dump())
                    problems.push_back("[" + std::to_string(i) + "] <" + std::string(n.name()) + "> " + key +
                                       "\n  c++:  " + short_text(value.dump()) + "\n  .net: " + short_text(e[key].dump()));
            };
            check("text", Json::str(n.text().str()));
            for (const auto& [attr, value] : e["attrs"].members) {
                std::string v = n.attr(attr).str();
                if (v != value.text)
                    problems.push_back("[" + std::to_string(i) + "] <" + std::string(n.name()) + "> @" + attr + ": " +
                                       Json::str(v).dump() + ", .NET: " + value.dump());
            }
            std::string children;
            for (const Node& c : n.children()) children += (children.empty() ? "" : ",") + std::to_string(c.index());
            check("textLineBreaks", Json::str(n.text_line_breaks()));
            check("source", Json::str(std::string(n.source())));
            check("next", Json::num(index_of(n.next())));
            check("previous", Json::num(index_of(n.previous())));
            check("nextSibling", Json::num(index_of(n.next_sibling())));
            check("previousSibling", Json::num(index_of(n.previous_sibling())));
            check("children", Json::str(children));
            check("href", Json::str(n.href()));
            check("src", Json::str(n.src()));
            check("mailto", Json::str(n.href_mail_to()));
            check("tel", Json::str(n.href_telephone()));
            check("textAfter", Json::str(std::string(n.text_after_tag_until_next_one())));
            check("textNoTagsAfter", Json::str(n.text_no_tags_after_tag_until_next_one().str()));
        }
        if (!problems.empty()) {
            std::string all;
            for (const auto& p : problems) all += p + "\n";
            failed.push_back(name + ":\n" + all);
        }
    }
    for (const auto& f : failed) MESSAGE(f);
    MESSAGE(tags << " tags compared on " << inputs.size() << " pages");
    CHECK(failed.empty());
}

TEST_CASE("compile as .NET") {
    Json expected = test::parse_json(test::read_file((testdata() / "expected" / "queries" / "compile.json").string()));
    std::vector<std::string> problems;
    for (const auto& [x, want] : expected["xpath"].members) {
        auto got = XPathQuery::compile(x);
        std::string g = got ? "ok" : "error";
        if (g != want.text) problems.push_back("XPath " + Json::str(x).dump() + ": " + g + ", .NET: " + want.text + " (" + got.error() + ")");
    }
    for (const auto& [c, want] : expected["css"].members) {
        auto got = CssQuery::compile(c);
        std::string g = got ? "ok" : "error";
        if (g != want.text) problems.push_back("CSS " + Json::str(c).dump() + ": " + g + ", .NET: " + want.text + " (" + got.error() + ")");
    }
    for (const auto& p : problems) MESSAGE(p);
    MESSAGE((expected["xpath"].members.size() + expected["css"].members.size()) << " queries compiled");
    CHECK(problems.empty());
}

TEST_CASE("queries as .NET") {
    // Page and query where the .NET result depends on the order of an unstable sort (see the Go port).
    const std::vector<std::string> known_differences = {"snip039 //div/node()"};
    auto query_files = files(testdata() / "expected" / "queries", ".json");
    REQUIRE(query_files.size() > 100);
    std::vector<std::string> failed;
    std::size_t evaluated = 0;
    for (const auto& file : query_files) {
        std::string name = file.stem().string();
        if (name == "compile") continue;
        Json expected = test::parse_json(test::read_file(file.string()));
        std::string source = test::read_file((testdata() / "input" / (name + ".html")).string());
        Document doc = Document::parse(source);
        std::vector<std::size_t> scopes;
        for (const Json& s : expected["scopes"].items) scopes.push_back(static_cast<std::size_t>(s.number()));
        std::vector<std::string> problems;
        auto check = [&](const std::string& what, const Json& want, const Json& got) {
            ++evaluated;
            if (problems.size() < 10 && got.dump() != want.dump())
                problems.push_back(what + "\n  c++:  " + short_text(got.dump()) + "\n  .net: " + short_text(want.dump()));
        };

        for (const auto& [x, want] : expected["xpath"].members) {
            if (std::find(known_differences.begin(), known_differences.end(), name + " " + x) != known_differences.end())
                continue;
            auto compiled = XPathQuery::compile(x);
            REQUIRE_MESSAGE(compiled.ok(), compiled.error());
            const XPathQuery& q = *compiled;
            check("XPath tags " + x, want["tags"], result([&] { return indexes(q.nodes(doc)); }));
            check("XPath values " + x, want["values"], result([&] { return q.values(doc); }));
            check("XPath value " + x, want["value"],
                  result([&] { return std::vector<std::string>{q.value(doc).value_or("<null>")}; }));
            std::vector<Json> scoped;
            for (std::size_t s : scopes) {
                Node n = doc.node(s);
                scoped.push_back(result([&] {
                    std::vector<std::string> v = indexes(q.nodes_from(n));
                    v.push_back("|");
                    for (auto& value : q.values_from(n)) v.push_back(std::move(value));
                    return v;
                }));
            }
            check("XPath scoped " + x, want["scoped"], Json::arr(std::move(scoped)));
        }

        for (const auto& [c, want] : expected["css"].members) {
            auto compiled = CssQuery::compile(c);
            REQUIRE_MESSAGE(compiled.ok(), compiled.error());
            const CssQuery& q = *compiled;
            check("CSS all " + c, want["all"], result([&] { return indexes(q.all(doc)); }));
            check("CSS first " + c, want["first"], result([&] {
                      auto f = q.first(doc);
                      return std::vector<std::string>{f ? std::to_string(f->index()) : "<null>"};
                  }));
            check("CSS matches " + c, want["matches"], result([&] {
                      std::vector<Node> m;
                      for (Node n : doc.nodes())
                          if (q.matches(n)) m.push_back(n);
                      return indexes(m);
                  }));
            std::vector<Json> scoped;
            for (std::size_t s : scopes) scoped.push_back(result([&] { return indexes(q.all_in(doc.node(s))); }));
            check("CSS scoped " + c, want["scoped"], Json::arr(std::move(scoped)));
        }
        if (!problems.empty()) {
            std::string all;
            for (const auto& p : problems) all += p + "\n";
            failed.push_back(name + ":\n" + all);
        }
    }
    for (const auto& f : failed) MESSAGE(f);
    MESSAGE(evaluated << " query results compared");
    CHECK(failed.empty());
}

TEST_CASE("no crash on mangled pages") {
    // Pages cut, spliced and sprinkled with markup characters and multi-byte text at random: every method on every tag
    // must work (and the sanitizers must stay quiet). FASTHTML_FUZZ_ROUNDS=200 for a longer run.
    std::uint64_t seed = 0x2545F4914F6CDD1Dull;
    auto rand = [&](std::size_t n) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return static_cast<std::size_t>(seed % (n ? n : 1));
    };
    const std::vector<std::string> pieces = {
        "<", ">", "</", "<!--", "-->", "=", "\"", "'", "&", "&amp", "&#x", "<p>", "<li>", "<td>", "<script>",
        "</div>", "\xD0\xB6", "\xF0\x9F\x98\x80", "\r", " ", "<a href=", "/>", "<?xml", "<![CDATA[", "\xC2\xA0",
        "<option>", "<table>", "\xFF", "\xE2\x82"};
    std::size_t rounds = 4;
    if (const char* r = std::getenv("FASTHTML_FUZZ_ROUNDS")) rounds = static_cast<std::size_t>(std::atoi(r));
    std::size_t checked = 0;
    for (const auto& input : files(testdata() / "input", ".html")) {
        std::string page = test::read_file(input.string());
        for (std::size_t round = 0; round < rounds; ++round) {
            std::string s;
            std::size_t i = 0;
            while (i < page.size()) {
                std::size_t run = 1 + rand(200);
                s.append(page, i, std::min(run, page.size() - i));
                i += run + (rand(4) == 0 ? rand(50) : 0);
                s += pieces[rand(pieces.size())];
            }
            Document doc = Document::parse(s, "http://h.example/a/b?c#d");
            for (Node n : doc.nodes()) {
                (void)n.text();
                (void)n.text_line_breaks();
                (void)n.body();
                (void)n.outer_html();
                (void)n.source();
                (void)n.href();
                (void)n.src();
                (void)n.attr("href");
                (void)n.attr("class");
                (void)n.has_class("x");
                (void)n.attrs();
                (void)n.children();
                (void)n.text_after_tag_until_next_one();
                (void)n.text_no_tags_after_tag_until_next_one();
                (void)n.href_mail_to();
                (void)n.is(any().text("x"));
                (void)n.next_sibling();
                (void)n.previous_sibling();
                ++checked;
            }
            (void)doc.xpath_values("//text() | //@* | //comment()");
            (void)doc.xpath_values("//*[contains(., 'a')]/following::text()[1]");
            (void)doc.css("div > p:nth-child(2n+1), a[href^='/'], li:has(+ li), :empty, :root");
        }
    }
    MESSAGE(checked << " tags of mangled pages read");
    CHECK(checked > 0);
}
