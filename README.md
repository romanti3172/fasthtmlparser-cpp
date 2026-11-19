# fasthtmlparser (C++)

C++20 port of [z3l.FastHtmlParser](https://github.com/romanti3172/z3l.FastHtmlParser): a fast HTML / XML parser for
scraping, with **XPath 1.0** and **CSS selectors**. It is built for extracting data, not for building a DOM: a page
becomes a flat list of tags that hold only positions in the source. Text, attributes and entities are read when you
ask for them, and a value that needs no decoding is a `std::string_view` of the page.

The whole API is ported: the parser, attributes, HTML5 entities, visible text and text with line breaks,
`find` / `find_all`, navigation, links, XPath 1.0, CSS selectors and the one-call shortcuts. It is a port of its own,
not a wrapper: there are also ports to [Go](https://github.com/romanti3172/fasthtmlparser),
[Rust](https://github.com/romanti3172/fasthtmlparser-rs), Java, TypeScript, Python, PHP and Ruby.

C++20, no dependencies; tested with MSVC 19.52 and gcc 16 (std::pmr, floating-point `std::to_chars`, `<bit>`). CMake:

```cmake
include(FetchContent)
FetchContent_Declare(fasthtmlparser GIT_REPOSITORY https://github.com/romanti3172/fasthtmlparser-cpp.git GIT_TAG main)
FetchContent_MakeAvailable(fasthtmlparser)
target_link_libraries(app PRIVATE fasthtmlparser::fasthtmlparser)
```

or `cmake --install` it and `find_package(fasthtmlparser CONFIG REQUIRED)` (the install exports the
`fasthtmlparser::fasthtmlparser` target and a package config, as vcpkg and Conan recipes expect).

```cpp
#include <fasthtmlparser/fasthtmlparser.hpp>

auto doc = fhp::Document::parse(page, "https://example.com/catalog/"); // or Document::parse(page)

// CSS selectors, as querySelectorAll / querySelector / matches
for (fhp::Node pod : doc.css("article.product_pod")) {
    auto title = pod.css_first("h3 a")->attr("title");   // "A Light in the Attic"
    auto price = pod.css_first("p.price_color")->text(); // "£51.77"
}
doc.css_first("article")->css("> h3"); // the children of the article

// XPath 1.0
doc.xpath_value("//dt[.='Price']/following-sibling::dd[1]"); // "£51.77"
doc.xpath_values("//a[contains(@href, 'b/')]/@href");        // {"b/1"}
doc.xpath_value("count(//p)");                                // "2"

// compile once, reuse on any number of pages and threads; compiling never throws
auto titles = fhp::XPathQuery::compile("//article//h3/a/@title");
if (!titles) std::cerr << titles.error();
titles->values(doc);
auto prices = fhp::CssQuery::compile("p.price_color");
prices->all(doc);

// search by name, class, id, attribute, text: a Query holds views, searching allocates nothing
auto pod = doc.find(fhp::by(fhp::Tag::ARTICLE).class_name("product_pod"));
pod->find(fhp::by(fhp::Tag::P).class_name("price_color"))->text();
doc.find(fhp::by_name("my-widget").class_name("x")); // any name works as a string

// one-call shortcuts (find_by_name_*, find_all_by_name_*, is_name_*, parent_by_name, next_sibling_by_name, ...)
auto dt = doc.find_by_name_text(fhp::Tag::DT, "Price");
dt->next_sibling_by_name(fhp::Tag::DD)->text(); // "£51.77"

// reading a tag
fhp::Node a = *doc.css_first("h3 a");
a.text();                      // visible text, entities decoded
a.attr(fhp::attr::TITLE);      // decoded value; attr_raw(): as written
a.href();                      // "https://example.com/catalog/b/1"; src() for src
a.source();                    // R"(a href="b/1" title="A Light in the Attic")": the opening tag without '<' and '>'
a.name(); a.depth(); a.pos();  // "a", 4, byte offsets of '<' and the last '>'
doc.find(fhp::by(fhp::Tag::BODY))->text_line_breaks(); // block elements → lines

// navigation
a.parent(); a.parent_by_name(fhp::Tag::ARTICLE); pod->children(); dt->next_sibling(); a.next(); a.previous();
```

A `Node` is a small handle (the document and an index), cheap to copy. A missing tag is an empty
`std::optional<Node>`, a missing value is `""`. Decoded values are `CowString`: a view of the page when there is
nothing to decode, an owned string otherwise (`.view()`, `.str()`, it compares with strings). Every view is valid as
long as its `Document`; moving a `Document` keeps them valid (its data lives on the heap).

The string methods (`css`, `xpath`, ...) compile and cache the query and throw `std::invalid_argument` on an invalid
one; `CssQuery::compile` / `XPathQuery::compile` return a `Compiled<T>` with the query or the error message instead.
`XPathQuery::nodes` throws `std::logic_error` on an expression that gives a number or string (check `is_node_set()`,
or use `values` / `value`). Nothing throws while parsing or reading values (except `std::bad_alloc`).

Memory: `Document::parse(page, url, mr)` takes all the memory of the document (the copy of the page, the tag list,
custom names) from a `std::pmr::memory_resource`, so a `std::pmr::monotonic_buffer_resource` over a reused buffer
makes it an arena. `Document::parse_view` reads the page in place without copying it (the page must outlive the
document). The parser keeps its scratch buffers per thread; each XPath evaluation keeps its node-sets in an arena on
the stack.

Queries: `by(Tag::DIV)`, `by_name("custom-tag")` or `any()`, then `.class_name`, `.id`, `.attr`, `.text`,
`.id_starts_with`, `.text_starts_with`, `.body`, `.body_starts_with`, `.source`. The shortcuts take a `Tag` or any
name as a string. `Tag::*` and `attr::*` cover about 250 tag and 230 attribute names (HTML, SVG, MathML, RSS, Atom,
sitemap).

CSS: type, `*`, `#id`, `.class`, attribute selectors with all operators and the `i` / `s` flags, the four combinators,
selector lists, `:first-child`, `:last-child`, `:only-child`, `:*-of-type`, `:nth-child(An+B [of S])` and its relatives,
`:not()`, `:is()`, `:where()`, `:has()`, `:empty`, `:root`, `:scope`, `:link`, `:checked`, `:disabled`, `:enabled`,
`:required`, `:optional`, `:contains("text")`. XPath: all axes except namespace, predicates, `|`, the operators and all
XPath 1.0 functions, plus `ends-with()`, `lower-case()`, `upper-case()`. HTML rules apply: names are case-insensitive,
the string-value of an element is its visible text, attribute values and text are decoded.

A `Document` is read-only and can be used from any number of threads at once.

## Same results as the .NET version

`ctest` (or `build/tests/fasthtmlparser_tests`) runs the conformance suite of the Rust port against
z3l.FastHtmlParser 1.4.1 (.NET) on 117 pages: the test fixtures, broken-markup snippets, edge cases, pages with end
tags left out or dropped at random, names upper-cased, pages cut off, saved with CR LF and with a lone CR.

- `tags as .NET`: every tag (16,643): names, parents, depths, positions (UTF-8 byte offsets), inner positions,
  visible text, text with line breaks, 10 attribute values, the opening tag source, next / previous tag and sibling,
  children, the resolved href and src, mailto: and tel: values, and the text after the tag must be identical.
- `compile as .NET` and `queries as .NET`: 569 queries. Whether each one compiles must be the same (119 are invalid);
  the XPath and CSS queries that compile are run on every page, on the document and from three tags of the page, and
  every CSS selector is also matched against every tag: 210,596 results, all identical.
- `no crash on mangled pages`: pages cut, spliced and sprinkled with markup, multi-byte text and invalid UTF-8; every
  method on every tag and a set of queries must run (`FASTHTML_FUZZ_ROUNDS=200` for a longer run).

All pass on MSVC 19.52 and gcc 16, with the one documented exception of the other ports: on the page nested 600
levels deep the .NET result depends on the order of an unstable sort.

Intended differences, as in the other ports: `text_line_breaks` joins lines with `"\n"` on every OS, and a missing
value is `""` (or an empty optional for a tag) where the .NET version returns null. Links resolved against the page are
written as .NET's `System.Uri` writes them; absolute links are returned as written. A page is read as UTF-8; invalid
sequences are kept as bytes and read as U+FFFD where text is decoded.

## Benchmarks

BENCHMARKS

## Building

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build        # fetches doctest
cmake --install build --prefix <dir>
```

`-DFASTHTMLPARSER_BUILD_TESTS=OFF` builds the library alone (the default when it is a subproject).

## Layout

- `src/parse.cpp`: the scanner and the HTML5 optional-end-tag rules (port of `ParsingCore.cs`)
- `src/attributes.cpp`, `src/decode.cpp`, `src/text.cpp`: attributes, character references, visible text and text with
  line breaks
- `src/document.cpp`: `Document`, `Node`, `Query`, search and navigation; `src/links.cpp`: href / src, mailto:, tel:
- `src/css.cpp`: CSS selectors; `src/xpath.cpp`: XPath 1.0
- `include/fasthtmlparser/tag.hpp`, `attr.hpp`, `src/entities.inc`, `src/unicode_tables.inc`: generated by
  `python tools/gen.py ../fasthtmlparser` from the tables of the Go port and the Unicode database
- `tests/`: doctest; `testdata/`: the pages and the .NET output (shared with the Rust port)
- `bench/`: the benchmark, its own CMake project, so the library has no dependencies; `bench/Dockerfile`: the Linux
  image `fhp-bench-cpp`

## License

Same terms as z3l.FastHtmlParser (see LICENSE.md): free for companies under USD 1M annual revenue and under 100
people, a commercial license above that. This is source-available, not an OSI open-source license.
