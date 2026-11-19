// HTML5 character reference decoder (WHATWG "character reference state"): all 2125 named references, the legacy
// names without ';', numeric references with or without ';', and the spec replacements (0, surrogates,
// > U+10FFFF → U+FFFD; 0x80–0x9F → Windows-1252).

#include <algorithm>

#include "chars.hpp"
#include "document_data.hpp"

namespace fasthtmlparser {

namespace detail {
namespace {

struct Entity {
    std::string_view name;
    std::string_view value;
    bool legacy;
};

#include "entities.inc"

/// Replacements for numeric references 0x80..0x9F; 0 = keep the code point as is.
constexpr char32_t windows_1252[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

const Entity* lookup_entity(std::string_view name) noexcept {
    const Entity* it = std::lower_bound(std::begin(entities), std::end(entities), name,
                                        [](const Entity& e, std::string_view n) { return e.name < n; });
    if (it == std::end(entities) || it->name != name) return nullptr;
    return it;
}

} // namespace

bool needs_decoding(std::string_view s) noexcept {
    for (char c : s)
        if (c == '&' || c == '\r') return true;
    return false;
}

CowString normalize_newlines(std::string_view s) {
    std::size_t cr = find_byte(s, '\r');
    if (cr == npos) return CowString(s);
    std::string out;
    out.reserve(s.size());
    out.append(s.substr(0, cr));
    for (std::size_t i = cr; i < s.size(); ++i) {
        if (s[i] != '\r') {
            out.push_back(s[i]);
        } else {
            out.push_back('\n');
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        }
    }
    return CowString(std::move(out));
}

std::size_t match_numeric(std::string_view s, char32_t& c) noexcept {
    std::size_t i = 1;
    bool hex = i < s.size() && (s[i] | 0x20) == 'x';
    if (hex) ++i;
    std::size_t digits = i;
    std::uint32_t value = 0;
    while (i < s.size()) {
        unsigned char ch = static_cast<unsigned char>(s[i]);
        std::uint32_t d;
        if (is_digit(ch))
            d = ch - '0';
        else if (hex && static_cast<unsigned char>((ch | 0x20) - 'a') < 6)
            d = static_cast<std::uint32_t>((ch | 0x20) - 'a') + 10;
        else
            break;
        value = hex ? value * 16 + d : value * 10 + d;
        if (value > 0x10FFFF) value = 0x110000; // saturate: no overflow on long digit runs
        ++i;
    }
    if (i == digits) return 0; // "&#" / "&#x" without digits is plain text
    if (i < s.size() && s[i] == ';') ++i;
    if (value == 0 || value > 0x10FFFF || value - 0xD800u <= 0x7FFu) {
        value = 0xFFFD;
    } else if (value - 0x80u <= 0x1Fu) {
        char32_t mapped = windows_1252[value - 0x80];
        if (mapped != 0) value = mapped;
    }
    c = value;
    return i;
}

std::size_t match_named(std::string_view s, bool in_attribute, std::string_view& value) noexcept {
    std::size_t limit = std::min(s.size(), entity_max_name + 1);
    std::size_t n = 0;
    while (n < limit && is_alnum(static_cast<unsigned char>(s[n]))) ++n;
    if (n < 2) return 0;
    // Full match: "&name;".
    if (n < s.size() && s[n] == ';') {
        if (const Entity* e = lookup_entity(s.substr(0, n))) {
            value = e->value;
            return n + 1;
        }
    }
    // Longest legacy prefix without ';': "&copy2024" → "©2024", "&notit;" → "¬it;".
    for (std::size_t k = std::min(n, entity_max_legacy_name); k >= 2; --k) {
        const Entity* e = lookup_entity(s.substr(0, k));
        if (e && e->legacy) {
            if (in_attribute && k < s.size() && (s[k] == '=' || is_alnum(static_cast<unsigned char>(s[k])))) break;
            value = e->value;
            return k;
        }
    }
    return 0;
}

CowString decode(std::string_view input, bool in_attribute) {
    // Line breaks first, as a browser reads the page; a CR written as "&#13;" is decoded after that and stays.
    CowString normalized = normalize_newlines(input);
    std::string_view s = normalized;
    std::size_t amp = find_byte(s, '&');
    if (amp == npos) return normalized;
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    for (;;) {
        out.append(s.substr(pos, amp - pos));
        std::string_view rest = s.substr(amp + 1);
        std::size_t consumed = 0;
        if (!rest.empty() && rest[0] == '#') {
            char32_t c;
            consumed = match_numeric(rest, c);
            if (consumed) append_utf8(out, c);
        } else {
            std::string_view value;
            consumed = match_named(rest, in_attribute, value);
            if (consumed) out.append(value);
        }
        if (consumed == 0) out.push_back('&'); // not a reference: keep '&'
        pos = amp + 1 + consumed;
        amp = find_byte(s, '&', pos);
        if (amp == npos) break;
    }
    out.append(s.substr(pos));
    return CowString(std::move(out));
}

} // namespace detail

CowString decode_html(std::string_view s) { return detail::decode(s, false); }

} // namespace fasthtmlparser
