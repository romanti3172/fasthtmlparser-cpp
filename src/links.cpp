// Resolution follows RFC 3986 the way the Go port does it (net/url: Parse + ResolveReference), byte for byte.

#include "links.hpp"

#include "chars.hpp"
#include "document_data.hpp"

namespace fasthtmlparser {

namespace detail {
namespace {

enum class Mode { path, host, user_password, fragment };

bool should_escape(unsigned char c, Mode mode) {
    if (is_alnum(c)) return false;
    if (mode == Mode::host) {
        switch (c) {
        case '!': case '$': case '&': case '\'': case '(': case ')': case '*': case '+': case ',': case ';':
        case '=': case ':': case '[': case ']': case '<': case '>': case '"':
            return false;
        default:
            break;
        }
    }
    switch (c) {
    case '-': case '_': case '.': case '~':
        return false;
    case '$': case '&': case '+': case ',': case '/': case ':': case ';': case '=': case '?': case '@':
        switch (mode) {
        case Mode::path: return c == '?';
        case Mode::user_password: return c == '@' || c == '/' || c == '?' || c == ':';
        case Mode::fragment: return false;
        case Mode::host: break;
        }
        break;
    default:
        break;
    }
    return !(mode == Mode::fragment && (c == '!' || c == '(' || c == ')' || c == '*'));
}

unsigned char unhex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

bool unescape(std::string_view s, Mode mode, std::string& out) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '%') {
            ++n;
            if (i + 2 >= s.size() || !is_hex(s[i + 1]) || !is_hex(s[i + 2])) return false;
            // in the host, %-encoding is only for non-ASCII bytes (and %25)
            if (mode == Mode::host && unhex(s[i + 1]) < 8 && s.substr(i, 3) != "%25") return false;
            i += 3;
        } else {
            if (mode == Mode::host && c < 0x80 && should_escape(c, mode)) return false;
            ++i;
        }
    }
    if (n == 0) {
        out.assign(s);
        return true;
    }
    std::string r;
    r.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '%') {
            r.push_back(static_cast<char>(unhex(s[i + 1]) << 4 | unhex(s[i + 2])));
            i += 3;
        } else {
            r.push_back(s[i++]);
        }
    }
    out = std::move(r);
    return true;
}

constexpr char upper_hex[] = "0123456789ABCDEF";

std::string escape(std::string_view s, Mode mode) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (should_escape(c, mode)) {
            out.push_back('%');
            out.push_back(upper_hex[c >> 4]);
            out.push_back(upper_hex[c & 15]);
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

bool valid_encoded(std::string_view s, Mode mode) {
    for (unsigned char c : s) {
        switch (c) {
        case '!': case '$': case '&': case '\'': case '(': case ')': case '*': case '+': case ',': case ';':
        case '=': case ':': case '@': case '[': case ']': case '%':
            continue;
        default:
            if (should_escape(c, mode)) return false;
        }
    }
    return true;
}

/// Splits the scheme off; false: missing protocol scheme.
bool get_scheme(std::string_view raw, std::string_view& scheme, std::string_view& rest) {
    for (std::size_t i = 0; i < raw.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        if (is_ascii_alpha(c)) continue;
        if (is_digit(c) || c == '+' || c == '-' || c == '.') {
            if (i == 0) {
                scheme = {};
                rest = raw;
                return true;
            }
            continue;
        }
        if (c == ':') {
            if (i == 0) return false;
            scheme = raw.substr(0, i);
            rest = raw.substr(i + 1);
            return true;
        }
        scheme = {};
        rest = raw;
        return true;
    }
    scheme = {};
    rest = raw;
    return true;
}

bool set_path(Url& u, std::string_view p) {
    std::string path;
    if (!unescape(p, Mode::path, path)) return false;
    u.path = std::move(path);
    if (escape(u.path, Mode::path) == p)
        u.raw_path.clear();
    else
        u.raw_path.assign(p);
    return true;
}

std::string escaped_path(const Url& u) {
    if (!u.raw_path.empty() && valid_encoded(u.raw_path, Mode::path)) {
        std::string p;
        if (unescape(u.raw_path, Mode::path, p) && p == u.path) return u.raw_path;
    }
    if (u.path == "*") return "*";
    return escape(u.path, Mode::path);
}

std::string escaped_fragment(const Url& u) {
    if (!u.raw_fragment.empty() && valid_encoded(u.raw_fragment, Mode::fragment)) {
        std::string f;
        if (unescape(u.raw_fragment, Mode::fragment, f) && f == u.fragment) return u.raw_fragment;
    }
    return escape(u.fragment, Mode::fragment);
}

bool valid_optional_port(std::string_view port) {
    if (port.empty()) return true;
    if (port[0] != ':') return false;
    for (unsigned char c : port.substr(1))
        if (!is_digit(c)) return false;
    return true;
}

bool parse_host(std::string_view host, std::string& out) {
    if (!host.empty() && host[0] == '[') {
        std::size_t i = host.rfind(']');
        if (i == npos) return false;
        if (!valid_optional_port(host.substr(i + 1))) return false;
    } else {
        std::size_t i = host.rfind(':');
        if (i != npos && !valid_optional_port(host.substr(i))) return false;
    }
    return unescape(host, Mode::host, out);
}

bool valid_userinfo(std::string_view s) {
    static constexpr std::string_view allowed = "-._:~!$&'()*+,;=%@";
    for (unsigned char c : s)
        if (!is_alnum(c) && allowed.find(static_cast<char>(c)) == npos) return false;
    return true;
}

bool parse_authority(Url& u, std::string_view authority) {
    std::size_t at = authority.rfind('@');
    if (!parse_host(at == npos ? authority : authority.substr(at + 1), u.host)) return false;
    if (at == npos) return true;
    std::string_view userinfo = authority.substr(0, at);
    if (!valid_userinfo(userinfo)) return false;
    std::size_t colon = userinfo.find(':');
    if (colon == npos) {
        std::string name;
        if (!unescape(userinfo, Mode::user_password, name)) return false;
        u.user = escape(name, Mode::user_password);
    } else {
        std::string name, password;
        if (!unescape(userinfo.substr(0, colon), Mode::user_password, name)) return false;
        if (!unescape(userinfo.substr(colon + 1), Mode::user_password, password)) return false;
        u.user = escape(name, Mode::user_password) + ":" + escape(password, Mode::user_password);
    }
    return true;
}

bool parse_no_fragment(std::string_view raw, Url& u) {
    for (unsigned char c : raw)
        if (c < 0x20 || c == 0x7f) return false; // invalid control character in URL
    if (raw == "*") {
        u.path = "*";
        return true;
    }
    std::string_view scheme, rest;
    if (!get_scheme(raw, scheme, rest)) return false;
    u.scheme.assign(scheme);
    for (char& c : u.scheme) c = to_lower_ascii(c);

    std::size_t q = rest.find('?');
    if (!rest.empty() && rest.back() == '?' && q == rest.size() - 1) {
        u.force_query = true;
        rest.remove_suffix(1);
    } else if (q != npos) {
        u.raw_query.assign(rest.substr(q + 1));
        rest = rest.substr(0, q);
    }

    if (rest.empty() || rest[0] != '/') {
        if (!u.scheme.empty()) {
            u.opaque.assign(rest); // rootless paths are opaque (RFC 3986)
            return true;
        }
        // a relative-path reference: the first segment cannot contain ':'
        std::string_view segment = rest.substr(0, rest.find('/'));
        if (segment.find(':') != npos) return false;
    }

    if ((!u.scheme.empty() || rest.substr(0, 3) != "///") && rest.substr(0, 2) == "//") {
        std::string_view authority = rest.substr(2);
        rest = {};
        std::size_t slash = authority.find('/');
        if (slash != npos) {
            rest = authority.substr(slash);
            authority = authority.substr(0, slash);
        }
        if (!parse_authority(u, authority)) return false;
    }
    return set_path(u, rest);
}

/// net/url Parse on bytes.
bool parse_url(std::string_view raw, Url& u) {
    std::size_t hash = raw.find('#');
    if (!parse_no_fragment(hash == npos ? raw : raw.substr(0, hash), u)) return false;
    if (hash != npos) {
        std::string_view frag = raw.substr(hash + 1);
        if (!unescape(frag, Mode::fragment, u.fragment)) return false;
        if (escape(u.fragment, Mode::fragment) == frag)
            u.raw_fragment.clear();
        else
            u.raw_fragment.assign(frag);
    }
    return true;
}

std::string resolve_path(std::string_view base, std::string_view r) {
    std::string full;
    if (r.empty()) {
        full.assign(base);
    } else if (r[0] != '/') {
        std::size_t i = base.rfind('/');
        full.assign(base.substr(0, i == npos ? 0 : i + 1));
        full.append(r);
    } else {
        full.assign(r);
    }
    if (full.empty()) return {};

    std::string dst = "/";
    bool first = true;
    std::string_view remaining = full;
    bool found = true;
    std::string_view elem;
    while (found) {
        std::size_t slash = remaining.find('/');
        found = slash != npos;
        if (found) {
            elem = remaining.substr(0, slash);
            remaining = remaining.substr(slash + 1);
        } else {
            elem = remaining;
            remaining = {};
        }
        if (elem == ".") {
            first = false;
            continue;
        }
        if (elem == "..") {
            std::string_view s = std::string_view(dst).substr(1);
            std::size_t index = s.rfind('/');
            if (index == npos) {
                dst = "/";
                first = true;
            } else {
                dst = "/" + std::string(s.substr(0, index));
            }
        } else {
            if (!first) dst.push_back('/');
            dst.append(elem);
            first = false;
        }
    }
    if (elem == "." || elem == "..") dst.push_back('/');
    if (dst.size() > 1 && dst[1] == '/') dst.erase(0, 1);
    return dst;
}

/// net/url ResolveReference.
Url resolve_reference(const Url& base, const Url& r) {
    Url url = r;
    if (r.scheme.empty()) url.scheme = base.scheme;
    if (!r.scheme.empty() || !r.host.empty() || r.user) {
        std::string p = resolve_path(escaped_path(r), "");
        set_path(url, p); // as net/url: on an error the path stays as it was
        return url;
    }
    if (!r.opaque.empty()) {
        url.user.reset();
        url.host.clear();
        url.path.clear();
        return url;
    }
    if (r.path.empty() && !r.force_query && r.raw_query.empty()) {
        url.raw_query = base.raw_query;
        if (r.fragment.empty()) {
            url.fragment = base.fragment;
            url.raw_fragment = base.raw_fragment;
        }
    }
    if (r.path.empty() && !base.opaque.empty()) {
        url.opaque = base.opaque;
        url.user.reset();
        url.host.clear();
        url.path.clear();
        return url;
    }
    url.host = base.host;
    url.user = base.user;
    std::string p = resolve_path(escaped_path(base), escaped_path(r));
    set_path(url, p);
    return url;
}

std::string_view default_port(std::string_view scheme) {
    if (scheme == "http" || scheme == "ws") return "80";
    if (scheme == "https" || scheme == "wss") return "443";
    if (scheme == "ftp") return "21";
    return {};
}

void split_host_port(std::string_view host_port, std::string_view& host, std::string_view& port) {
    host = host_port;
    port = {};
    std::size_t colon = host.rfind(':');
    if (colon != npos && valid_optional_port(host.substr(colon))) {
        port = host.substr(colon + 1);
        host = host.substr(0, colon);
    }
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
}

constexpr std::string_view reserved_for_display = "%/?#[]@:!$&'()*+,;=";

bool valid_utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        Utf8Char ch = decode_utf8(s, i);
        if (ch.c == 0xFFFD && !(ch.size == 3 && s.substr(i, 3) == "\xEF\xBF\xBD")) return false;
        i += ch.size;
    }
    return true;
}

/// Decodes %XX escapes except those of reserved and control characters; escaped bytes of non-ASCII text are
/// decoded when they form valid UTF-8.
void unescape_for_display(std::string_view s, std::string& out) {
    if (s.find('%') == npos) {
        out.append(s);
        return;
    }
    auto escaped = [&](std::size_t j) { return j + 2 < s.size() && s[j] == '%' && is_hex(s[j + 1]) && is_hex(s[j + 2]); };
    for (std::size_t i = 0; i < s.size();) {
        if (!escaped(i)) {
            out.push_back(s[i++]);
            continue;
        }
        unsigned char c = static_cast<unsigned char>(unhex(s[i + 1]) << 4 | unhex(s[i + 2]));
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f || reserved_for_display.find(static_cast<char>(c)) != npos)
                out.append(s.substr(i, 3));
            else
                out.push_back(static_cast<char>(c));
            i += 3;
            continue;
        }
        // a run of escaped bytes >= 0x80: decoded together when it is valid UTF-8
        std::size_t j = i;
        std::string run;
        while (escaped(j)) {
            unsigned char v = static_cast<unsigned char>(unhex(s[j + 1]) << 4 | unhex(s[j + 2]));
            if (v < 0x80) break;
            run.push_back(static_cast<char>(v));
            j += 3;
        }
        if (valid_utf8(run))
            out += run;
        else
            out.append(s.substr(i, j - i));
        i = j;
    }
}

/// Invalid UTF-8 sequences replaced by U+FFFD (as Rust's String::from_utf8_lossy).
std::string lossy(std::string_view s) {
    if (valid_utf8(s)) return std::string(s);
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        Utf8Char ch = decode_utf8(s, i);
        if (ch.c == 0xFFFD && ch.size == 1)
            out += "\xEF\xBF\xBD";
        else
            out.append(s.substr(i, ch.size));
        i += ch.size;
    }
    return out;
}

/// Writes a resolved URL as the .NET version does (System.Uri.ToString).
std::string display_string(const Url& u) {
    std::string scheme = u.scheme;
    for (char& c : scheme) c = to_lower_ascii(c);
    std::string b = scheme;
    b.push_back(':');
    if (!u.opaque.empty()) {
        b += u.opaque;
    } else {
        if (!u.host.empty() || u.user) {
            b += "//";
            if (u.user) {
                b += *u.user;
                b.push_back('@');
            }
            std::string_view host, port;
            split_host_port(u.host, host, port);
            b += to_lower(lossy(host));
            if (!port.empty() && port != default_port(scheme)) {
                b.push_back(':');
                b.append(port);
            }
        }
        std::string path = escaped_path(u);
        if (path.empty() && !u.host.empty()) path = "/";
        unescape_for_display(path, b);
    }
    if (!u.raw_query.empty() || u.force_query) {
        b.push_back('?');
        unescape_for_display(u.raw_query, b);
    }
    if (!u.fragment.empty()) {
        b.push_back('#');
        unescape_for_display(escaped_fragment(u), b);
    }
    return lossy(b);
}

/// A link with its own scheme ("https://…", "mailto:…", "file:…"). "/path" and "//host/path" are relative.
bool is_absolute_link(std::string_view link) {
    Url u;
    return parse_url(trim(link), u) && !u.scheme.empty();
}

} // namespace

std::optional<Url> absolute_url(std::string_view s) {
    if (s.empty()) return std::nullopt;
    Url u;
    if (!parse_url(s, u)) return std::nullopt;
    if (u.scheme.empty() || (u.host.empty() && u.opaque.empty() && !eq_ignore_ascii_case(u.scheme, "file")))
        return std::nullopt;
    return u;
}

std::string correct_link(std::string_view raw, std::string_view page_url, const Url* base) {
    if (raw.empty()) return std::string(page_url);
    CowString link = decode(raw, true);
    if (!base) return std::move(link).str();
    if (is_absolute_link(link)) return std::move(link).str();
    Url r;
    if (!parse_url(trim(link), r)) return std::move(link).str();
    return display_string(resolve_reference(*base, r));
}

} // namespace detail

using detail::correct_link;
using detail::data;
using detail::doc;

std::string Node::href() const {
    const detail::DocumentData& d = detail::doc(*this);
    return correct_link(attr_raw(attr::HREF), d.url, d.base ? &*d.base : nullptr);
}

std::string Node::href_from(std::string_view base_url) const {
    std::optional<detail::Url> base = detail::absolute_url(base_url);
    return correct_link(attr_raw(attr::HREF), base_url, base ? &*base : nullptr);
}

std::string Node::src() const {
    const detail::DocumentData& d = detail::doc(*this);
    return correct_link(attr_raw(attr::SRC), d.url, d.base ? &*d.base : nullptr);
}

std::string Node::src_from(std::string_view base_url) const {
    std::optional<detail::Url> base = detail::absolute_url(base_url);
    return correct_link(attr_raw(attr::SRC), base_url, base ? &*base : nullptr);
}

namespace {

std::string href_after_scheme(const Node& n, std::string_view scheme) {
    CowString decoded = detail::decode(n.attr_raw(attr::HREF), true);
    std::string_view href = detail::trim(decoded);
    if (!detail::starts_with_ignore_ascii_case(href, scheme)) return {};
    std::size_t i = href.find(':');
    if (i == detail::npos) return {};
    return std::string(href.substr(i + 1));
}

} // namespace

std::string Node::href_mail_to() const { return href_after_scheme(*this, "mailto"); }
std::string Node::href_telephone() const { return href_after_scheme(*this, "tel"); }

} // namespace fasthtmlparser
