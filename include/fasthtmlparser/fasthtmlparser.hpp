// fasthtmlparser: a fast HTML / XML parser for scraping, with XPath 1.0 and CSS selectors. A C++20 port of
// z3l.FastHtmlParser (https://github.com/romanti3172/z3l.FastHtmlParser).
//
// A Document is a flat list of tags in document order; each tag holds only positions in the source. Text,
// attributes and entities are read when they are asked for, and a value that needs no decoding is a view of the
// page.
#pragma once

#include "attr.hpp"
#include "cow_string.hpp"
#include "document.hpp"
#include "query.hpp"
#include "tag.hpp"

#ifndef FASTHTMLPARSER_NO_ALIAS
/// The short name (define FASTHTMLPARSER_NO_ALIAS to leave it out).
namespace fhp = fasthtmlparser;
#endif
