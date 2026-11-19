#include "chars.hpp"

#include <algorithm>


namespace fasthtmlparser::detail {

namespace {

struct CaseRow {
    std::uint32_t from;
    std::uint32_t to; // the char, or 0x80000000 | index into the pool for a mapping to several chars
};

struct Range {
    std::uint32_t first;
    std::uint32_t last;
};

#include "unicode_tables.inc"

template <std::size_t N>
bool in_ranges(const Range (&ranges)[N], char32_t c) noexcept {
    const Range* it = std::upper_bound(std::begin(ranges), std::end(ranges), static_cast<std::uint32_t>(c),
                                       [](std::uint32_t v, const Range& r) { return v < r.first; });
    if (it == std::begin(ranges)) return false;
    --it;
    return c <= it->last;
}

template <std::size_t N>
const CaseRow* find_case(const CaseRow (&rows)[N], char32_t c) noexcept {
    const CaseRow* it = std::lower_bound(std::begin(rows), std::end(rows), static_cast<std::uint32_t>(c),
                                         [](const CaseRow& r, std::uint32_t v) { return r.from < v; });
    if (it == std::end(rows) || it->from != c) return nullptr;
    return it;
}

bool is_cased(char32_t c) noexcept { return in_ranges(cased_ranges, c); }
bool is_case_ignorable(char32_t c) noexcept { return in_ranges(case_ignorable_ranges, c); }

/// Final_Sigma: a cased letter before (skipping case-ignorable chars) and none after.
bool is_final_sigma(std::string_view s, std::size_t at, std::size_t size) noexcept {
    bool before = false;
    std::size_t i = at;
    while (i > 0) {
        Utf8Char ch = decode_utf8_before(s, i);
        i -= ch.size;
        if (is_case_ignorable(ch.c)) continue;
        before = is_cased(ch.c);
        break;
    }
    if (!before) return false;
    i = at + size;
    while (i < s.size()) {
        Utf8Char ch = decode_utf8(s, i);
        i += ch.size;
        if (is_case_ignorable(ch.c)) continue;
        return !is_cased(ch.c);
    }
    return true;
}

} // namespace

std::size_t count_byte(std::string_view s, char c) noexcept {
    const char* p = s.data();
    std::size_t n = s.size();
    std::size_t count = 0;
    std::size_t i = 0;
#ifdef FHP_SSE2
    const __m128i needle = _mm_set1_epi8(c);
    while (i + 16 <= n) {
        // up to 255 blocks before the byte counters could overflow
        __m128i acc = _mm_setzero_si128();
        std::size_t blocks = std::min<std::size_t>((n - i) / 16, 255);
        for (std::size_t b = 0; b < blocks; ++b, i += 16) {
            __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
            acc = _mm_sub_epi8(acc, _mm_cmpeq_epi8(v, needle));
        }
        __m128i sums = _mm_sad_epu8(acc, _mm_setzero_si128());
        count += static_cast<std::size_t>(_mm_cvtsi128_si32(sums)) +
                 static_cast<std::size_t>(_mm_cvtsi128_si32(_mm_unpackhi_epi64(sums, sums)));
    }
#endif
    for (; i < n; ++i) count += p[i] == c;
    return count;
}

namespace {

/// The first position in [i, last] holding a or b, or npos.
std::size_t find_either(const char* p, std::size_t i, std::size_t last, char a, char b) noexcept {
#ifdef FHP_SSE2
    const __m128i va = _mm_set1_epi8(a), vb = _mm_set1_epi8(b);
    while (i + 16 <= last + 1) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
        unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(v, va), _mm_cmpeq_epi8(v, vb))));
        if (mask) return i + static_cast<std::size_t>(std::countr_zero(mask));
        i += 16;
    }
#endif
    for (; i <= last; ++i)
        if (p[i] == a || p[i] == b) return i;
    return npos;
}

} // namespace

std::size_t index_fold(std::string_view s, std::string_view sub, std::size_t from) noexcept {
    if (sub.empty()) return from <= s.size() ? from : npos;
    if (from > s.size() || sub.size() > s.size() - from) return npos;
    char lower = to_lower_ascii(sub[0]);
    char upper = to_upper_ascii(sub[0]);
    std::size_t last = s.size() - sub.size(); // the last position a match can start at
    for (std::size_t i = from; i <= last; ++i) {
        i = find_either(s.data(), i, last, lower, upper);
        if (i == npos) return npos;
        if (eq_ignore_ascii_case(s.substr(i + 1, sub.size() - 1), sub.substr(1))) return i;
    }
    return npos;
}

Utf8Char decode_utf8(std::string_view s, std::size_t i) noexcept {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    std::size_t n = s.size();
    unsigned char b = p[i];
    if (b < 0x80) return {b, 1};
    auto cont = [&](std::size_t k) { return i + k < n && (p[i + k] & 0xC0) == 0x80; };
    if (b >= 0xC2 && b <= 0xDF) {
        if (cont(1)) return {static_cast<char32_t>(((b & 0x1F) << 6) | (p[i + 1] & 0x3F)), 2};
    } else if (b >= 0xE0 && b <= 0xEF) {
        if (cont(1) && cont(2)) {
            char32_t c = ((b & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
            if (c >= 0x800 && (c < 0xD800 || c > 0xDFFF)) return {c, 3};
        }
    } else if (b >= 0xF0 && b <= 0xF4) {
        if (cont(1) && cont(2) && cont(3)) {
            char32_t c = ((b & 0x07) << 18) | ((p[i + 1] & 0x3F) << 12) | ((p[i + 2] & 0x3F) << 6) | (p[i + 3] & 0x3F);
            if (c >= 0x10000 && c <= 0x10FFFF) return {c, 4};
        }
    }
    return {0xFFFD, 1};
}

Utf8Char decode_utf8_before(std::string_view s, std::size_t i) noexcept {
    std::size_t start = i - 1;
    std::size_t limit = i >= 4 ? i - 4 : 0;
    while (start > limit && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) --start;
    Utf8Char ch = decode_utf8(s, start);
    if (start + ch.size != i) return {0xFFFD, 1};
    return ch;
}

void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

std::string_view slice(std::string_view s, std::size_t from, std::size_t to) noexcept {
    to = std::min(to, s.size());
    while (!is_char_boundary(s, to)) --to;
    from = std::min(from, to);
    while (!is_char_boundary(s, from)) --from;
    return s.substr(from, to - from);
}

std::size_t count_chars(std::string_view s) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); i += decode_utf8(s, i).size) ++n;
    return n;
}

std::size_t count_utf16(std::string_view s) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) {
        Utf8Char ch = decode_utf8(s, i);
        n += ch.c >= 0x10000 ? 2 : 1;
        i += ch.size;
    }
    return n;
}

bool is_alphabetic(char32_t c) noexcept {
    if (c < 0x80) return is_ascii_alpha(static_cast<unsigned char>(c));
    return in_ranges(alphabetic_ranges, c);
}

bool is_numeric(char32_t c) noexcept {
    if (c < 0x80) return is_digit(static_cast<unsigned char>(c));
    return in_ranges(numeric_ranges, c);
}

std::string_view trim(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size()) {
        bool space;
        Utf8Char ch = space_at(s, i, space);
        if (!space) break;
        i += ch.size;
    }
    return trim_end(s.substr(i));
}

std::string_view trim_end(std::string_view s) noexcept {
    std::size_t end = s.size();
    while (end > 0) {
        unsigned char b = static_cast<unsigned char>(s[end - 1]);
        if (b < 0x80) {
            if (!is_space(b)) break; // the ASCII White_Space chars are exactly these
            --end;
            continue;
        }
        Utf8Char ch = decode_utf8_before(s, end);
        if (!is_whitespace(ch.c)) break;
        end -= ch.size;
    }
    return s.substr(0, end);
}

char32_t simple_lower(char32_t c) noexcept {
    if (c < 0x80) return static_cast<char32_t>(to_lower_ascii(static_cast<char>(c)));
    const CaseRow* r = find_case(lower_map, c);
    if (!r || (r->to & 0x80000000u)) return c;
    return r->to;
}

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            out.push_back(to_lower_ascii(static_cast<char>(b)));
            ++i;
            continue;
        }
        Utf8Char ch = decode_utf8(s, i);
        if (ch.c == 0x3A3 && is_final_sigma(s, i, ch.size)) {
            append_utf8(out, 0x3C2);
        } else if (const CaseRow* r = find_case(lower_map, ch.c)) {
            if (r->to & 0x80000000u)
                out += lower_pool[r->to & 0x7FFFFFFFu];
            else
                append_utf8(out, r->to);
        } else {
            out.append(s.substr(i, ch.size));
        }
        i += ch.size;
    }
    return out;
}

std::string to_upper(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            out.push_back(to_upper_ascii(static_cast<char>(b)));
            ++i;
            continue;
        }
        Utf8Char ch = decode_utf8(s, i);
        if (const CaseRow* r = find_case(upper_map, ch.c)) {
            if (r->to & 0x80000000u)
                out += upper_pool[r->to & 0x7FFFFFFFu];
            else
                append_utf8(out, r->to);
        } else {
            out.append(s.substr(i, ch.size));
        }
        i += ch.size;
    }
    return out;
}

std::string_view lower_key(std::string_view s, std::string& buffer) {
    if (is_lower_ascii(s)) return s;
    buffer = to_lower(s);
    return buffer;
}

bool equal_fold(std::string_view a, std::string_view b) noexcept {
    if (is_ascii(a) && is_ascii(b)) return eq_ignore_ascii_case(a, b);
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        Utf8Char x = decode_utf8(a, i);
        Utf8Char y = decode_utf8(b, j);
        if (x.c != y.c && simple_lower(x.c) != simple_lower(y.c)) return false;
        i += x.size;
        j += y.size;
    }
    return i == a.size() && j == b.size();
}

} // namespace fasthtmlparser::detail
