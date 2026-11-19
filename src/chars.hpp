// Byte, UTF-8 and string helpers shared by the parser and the queries.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define FHP_SSE2 1
#endif

namespace fasthtmlparser::detail {

inline constexpr std::size_t npos = std::string_view::npos;

/// ASCII whitespace as HTML reads it (and '\v').
inline bool is_space(unsigned char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0c || c == 0x0b;
}

inline bool is_ascii_alpha(unsigned char c) noexcept { return static_cast<unsigned char>((c | 0x20) - 'a') < 26; }
inline bool is_digit(unsigned char c) noexcept { return static_cast<unsigned char>(c - '0') < 10; }
inline bool is_alnum(unsigned char c) noexcept { return is_ascii_alpha(c) || is_digit(c); }
inline bool is_hex(unsigned char c) noexcept {
    return is_digit(c) || static_cast<unsigned char>((c | 0x20) - 'a') < 6;
}
inline bool is_upper_ascii(unsigned char c) noexcept { return static_cast<unsigned char>(c - 'A') < 26; }
inline char to_lower_ascii(char c) noexcept {
    return is_upper_ascii(static_cast<unsigned char>(c)) ? static_cast<char>(c | 0x20) : c;
}
inline char to_upper_ascii(char c) noexcept {
    return static_cast<unsigned char>(c - 'a') < 26 ? static_cast<char>(c & ~0x20) : c;
}

inline bool eq_ignore_ascii_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (to_lower_ascii(a[i]) != to_lower_ascii(b[i])) return false;
    return true;
}

inline bool starts_with_ignore_ascii_case(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && eq_ignore_ascii_case(s.substr(0, prefix.size()), prefix);
}

/// No upper-case ASCII letter and no non-ASCII byte.
inline bool is_lower_ascii(std::string_view s) noexcept {
    for (unsigned char c : s)
        if (is_upper_ascii(c) || c >= 0x80) return false;
    return true;
}

inline bool is_ascii(std::string_view s) noexcept {
    for (unsigned char c : s)
        if (c >= 0x80) return false;
    return true;
}

/// The position of c in s at or after from, or npos. SSE2 instead of memchr: the CRT memchr of Windows costs
/// about 20 ns a call, and the parser makes one for every tag.
inline std::size_t find_byte(std::string_view s, char c, std::size_t from = 0) noexcept {
    const char* p = s.data();
    std::size_t n = s.size();
    std::size_t i = from;
#ifdef FHP_SSE2
    const __m128i needle = _mm_set1_epi8(c);
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
        unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(v, needle)));
        if (mask) return i + static_cast<std::size_t>(std::countr_zero(mask));
        i += 16;
    }
#endif
    for (; i < n; ++i)
        if (p[i] == c) return i;
    return npos;
}

/// The number of times c occurs in s.
std::size_t count_byte(std::string_view s, char c) noexcept;

/// strings.Index with ASCII case folding; sub must be ASCII.
std::size_t index_fold(std::string_view s, std::string_view sub, std::size_t from = 0) noexcept;

/// The string with ASCII whitespace (as is_space) removed from the end.
inline std::string_view trim_ascii_end(std::string_view s) noexcept {
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// ----- UTF-8 -----

struct Utf8Char {
    char32_t c;
    std::size_t size;
};

/// The char at s[i] (i < s.size()); an invalid sequence reads as U+FFFD of one byte.
Utf8Char decode_utf8(std::string_view s, std::size_t i) noexcept;

/// The char that ends right before s[i] (i > 0).
Utf8Char decode_utf8_before(std::string_view s, std::size_t i) noexcept;

void append_utf8(std::string& out, char32_t c);

inline bool is_char_boundary(std::string_view s, std::size_t i) noexcept {
    return i == 0 || i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80;
}

/// s[from, to) with both ends moved back to a char boundary: a position from the parser may stand inside the last
/// char of a cut-off tag.
std::string_view slice(std::string_view s, std::size_t from, std::size_t to) noexcept;

/// The number of chars (code points).
std::size_t count_chars(std::string_view s) noexcept;

/// The number of UTF-16 code units.
std::size_t count_utf16(std::string_view s) noexcept;

// ----- Unicode properties -----

/// White_Space (as Rust char::is_whitespace / .NET char.IsWhiteSpace).
inline bool is_whitespace(char32_t c) noexcept {
    if (c < 0x80) return c == ' ' || (c >= 0x09 && c <= 0x0d);
    return c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 ||
           c == 0x202f || c == 0x205f || c == 0x3000;
}

bool is_alphabetic(char32_t c) noexcept;
bool is_numeric(char32_t c) noexcept;

/// Whether s[i..] starts with a whitespace char (ASCII or Unicode), and its size.
inline Utf8Char space_at(std::string_view s, std::size_t i, bool& space) noexcept {
    unsigned char b = static_cast<unsigned char>(s[i]);
    if (b < 0x80) {
        space = is_space(b);
        return {b, 1};
    }
    Utf8Char ch = decode_utf8(s, i);
    space = is_whitespace(ch.c);
    return ch;
}

/// Removes leading and trailing Unicode whitespace (as Rust str::trim).
std::string_view trim(std::string_view s) noexcept;

/// Removes trailing Unicode whitespace.
std::string_view trim_end(std::string_view s) noexcept;

/// The simple lower-case mapping of a char (one char).
char32_t simple_lower(char32_t c) noexcept;

/// Lower-cases a string (Unicode, with the final-sigma rule), as Rust str::to_lowercase.
std::string to_lower(std::string_view s);

/// Upper-cases a string (Unicode, full mappings: "ß" → "SS"), as Rust str::to_uppercase.
std::string to_upper(std::string_view s);

/// The lower-case name: s itself when it is already lower-case ASCII, otherwise written to buffer.
std::string_view lower_key(std::string_view s, std::string& buffer);

/// Case-insensitive equality as Go's strings.EqualFold: ASCII fast path, simple case folding otherwise.
bool equal_fold(std::string_view a, std::string_view b) noexcept;

} // namespace fasthtmlparser::detail
