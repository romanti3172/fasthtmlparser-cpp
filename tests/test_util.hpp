// A small JSON reader and SHA-256 for the conformance tests (the tests need no other dependency than doctest).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test {

inline std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Member;

struct Json {
    enum class Type { null, boolean, number, string, array, object } type = Type::null;
    bool b = false;
    std::string text; // string value, or the number as written
    std::vector<Json> items;
    std::vector<Member> members; // in source order (a vector may hold an incomplete type; a pair may not)

    const Json& operator[](std::string_view key) const; // defined after Member
    const Json& operator[](std::size_t i) const { return items.at(i); }
    long long number() const { return std::stoll(text); }
    bool is_string() const { return type == Type::string; }

    /// Compact JSON text, for messages and comparisons.
    std::string dump() const {
        switch (type) {
        case Type::null: return "null";
        case Type::boolean: return b ? "true" : "false";
        case Type::number: return text;
        case Type::string: return quote(text);
        case Type::array: {
            std::string out = "[";
            for (std::size_t i = 0; i < items.size(); ++i) out += (i ? "," : "") + items[i].dump();
            return out + "]";
        }
        case Type::object: return dump_object();
        }
        return {};
    }

    std::string dump_object() const; // defined after Member

    static std::string quote(std::string_view s) {
        std::string out = "\"";
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') {
                out.push_back('\\');
                out.push_back(static_cast<char>(c));
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\r') {
                out += "\\r";
            } else if (c == '\t') {
                out += "\\t";
            } else if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        return out + "\"";
    }

    static Json str(std::string s) {
        Json j;
        j.type = Type::string;
        j.text = std::move(s);
        return j;
    }
    static Json num(long long v) {
        Json j;
        j.type = Type::number;
        j.text = std::to_string(v);
        return j;
    }
    static Json arr(std::vector<Json> items) {
        Json j;
        j.type = Type::array;
        j.items = std::move(items);
        return j;
    }
};

struct Member {
    std::string first;
    Json second;
};

inline const Json& Json::operator[](std::string_view key) const {
    for (const auto& m : members)
        if (m.first == key) return m.second;
    static const Json none;
    return none;
}

inline std::string Json::dump_object() const {
    std::string out = "{";
    for (std::size_t i = 0; i < members.size(); ++i)
        out += (i ? "," : "") + quote(members[i].first) + ":" + members[i].second.dump();
    return out + "}";
}

class JsonParser {
public:
    explicit JsonParser(std::string_view s) : s_(s) {}

    Json parse() {
        Json j = value();
        space();
        if (p_ != s_.size()) fail("trailing data");
        return j;
    }

private:
    std::string_view s_;
    std::size_t p_ = 0;

    [[noreturn]] void fail(const char* what) const {
        throw std::runtime_error(std::string("JSON: ") + what + " at " + std::to_string(p_));
    }

    void space() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\n' || s_[p_] == '\r' || s_[p_] == '\t')) ++p_;
    }

    Json value() {
        space();
        if (p_ >= s_.size()) fail("unexpected end");
        char c = s_[p_];
        Json j;
        if (c == '{') {
            j.type = Json::Type::object;
            ++p_;
            space();
            if (s_[p_] == '}') {
                ++p_;
                return j;
            }
            for (;;) {
                space();
                std::string key = string();
                space();
                if (s_[p_++] != ':') fail("':' expected");
                j.members.push_back(Member{std::move(key), value()});
                space();
                if (s_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (s_[p_++] != '}') fail("'}' expected");
                return j;
            }
        }
        if (c == '[') {
            j.type = Json::Type::array;
            ++p_;
            space();
            if (s_[p_] == ']') {
                ++p_;
                return j;
            }
            for (;;) {
                j.items.push_back(value());
                space();
                if (s_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (s_[p_++] != ']') fail("']' expected");
                return j;
            }
        }
        if (c == '"') {
            j.type = Json::Type::string;
            j.text = string();
            return j;
        }
        if (s_.substr(p_, 4) == "true") {
            p_ += 4;
            j.type = Json::Type::boolean;
            j.b = true;
            return j;
        }
        if (s_.substr(p_, 5) == "false") {
            p_ += 5;
            j.type = Json::Type::boolean;
            return j;
        }
        if (s_.substr(p_, 4) == "null") {
            p_ += 4;
            return j;
        }
        std::size_t start = p_;
        while (p_ < s_.size() && (std::strchr("+-0123456789.eE", s_[p_]) != nullptr)) ++p_;
        if (p_ == start) fail("value expected");
        j.type = Json::Type::number;
        j.text = std::string(s_.substr(start, p_ - start));
        return j;
    }

    static void append_utf8(std::string& out, std::uint32_t c) {
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

    std::uint32_t hex4() {
        if (p_ + 4 > s_.size()) fail("bad \\u escape");
        std::uint32_t v = static_cast<std::uint32_t>(std::stoul(std::string(s_.substr(p_, 4)), nullptr, 16));
        p_ += 4;
        return v;
    }

    std::string string() {
        if (s_[p_] != '"') fail("string expected");
        ++p_;
        std::string out;
        for (;;) {
            if (p_ >= s_.size()) fail("unterminated string");
            char c = s_[p_++];
            if (c == '"') return out;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            char e = s_[p_++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                std::uint32_t c1 = hex4();
                if (c1 >= 0xD800 && c1 <= 0xDBFF && s_.substr(p_, 2) == "\\u") {
                    p_ += 2;
                    std::uint32_t c2 = hex4();
                    c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                }
                append_utf8(out, c1);
                break;
            }
            default: fail("bad escape");
            }
        }
    }
};

inline Json parse_json(std::string_view s) { return JsonParser(s).parse(); }

/// SHA-256 as a hex string.
inline std::string sha256_hex(std::string_view data) {
    static const std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string msg(data);
    std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
    auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(msg.data() + chunk + i * 4);
            w[i] = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string out;
    char buf[9];
    for (std::uint32_t v : h) {
        std::snprintf(buf, sizeof buf, "%08x", v);
        out += buf;
    }
    return out;
}

} // namespace test
