// Links: href / src made absolute against the page URL, written as .NET's System.Uri writes them.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fasthtmlparser::detail {

/// A parsed URL; every field is a byte string (the UTF-8 bytes), as Go's net/url keeps them.
struct Url {
    std::string scheme;
    std::string opaque;
    std::optional<std::string> user; // the userinfo as written back (escaped)
    std::string host;
    std::string path;     // decoded
    std::string raw_path; // the path as written, when it differs from the default escaping of path
    bool force_query = false;
    std::string raw_query;
    std::string fragment; // decoded
    std::string raw_fragment;
};

/// Parses a page URL with its own scheme and host; none for anything else.
std::optional<Url> absolute_url(std::string_view s);

/// The raw attribute value with its character references decoded and made absolute against the page.
std::string correct_link(std::string_view raw, std::string_view page_url, const Url* base);

} // namespace fasthtmlparser::detail
