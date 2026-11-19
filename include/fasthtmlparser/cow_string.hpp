#pragma once

#include <compare>
#include <cstddef>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace fasthtmlparser {

/// A decoded value: a view of the page when there was nothing to decode, an owned string otherwise (as Rust's
/// Cow<str>). It converts to std::string_view and compares with strings; str() gives a std::string.
///
/// A view stays valid as long as the Document it was read from.
class CowString {
public:
    CowString() noexcept = default;
    CowString(std::string_view view) noexcept : view_(view) {}
    CowString(const char* s) noexcept : view_(s) {}
    CowString(std::string&& owned) noexcept : owned_(std::move(owned)), is_owned_(true) {}

    CowString(const CowString&) = default;
    CowString(CowString&&) noexcept = default;
    CowString& operator=(const CowString&) = default;
    CowString& operator=(CowString&&) noexcept = default;

    std::string_view view() const noexcept { return is_owned_ ? std::string_view(owned_) : view_; }
    operator std::string_view() const noexcept { return view(); }

    /// Whether the value was decoded into a string of its own (false: it is a view of the page).
    bool is_owned() const noexcept { return is_owned_; }

    std::string str() const& { return std::string(view()); }
    std::string str() && { return is_owned_ ? std::move(owned_) : std::string(view_); }

    const char* data() const noexcept { return view().data(); }
    std::size_t size() const noexcept { return view().size(); }
    bool empty() const noexcept { return view().empty(); }
    auto begin() const noexcept { return view().begin(); }
    auto end() const noexcept { return view().end(); }
    char operator[](std::size_t i) const noexcept { return view()[i]; }

    friend bool operator==(const CowString& a, const CowString& b) noexcept { return a.view() == b.view(); }
    friend bool operator==(const CowString& a, std::string_view b) noexcept { return a.view() == b; }
    friend bool operator==(const CowString& a, const char* b) noexcept { return a.view() == std::string_view(b); }
    friend bool operator==(const CowString& a, const std::string& b) noexcept { return a.view() == b; }
    friend std::strong_ordering operator<=>(const CowString& a, const CowString& b) noexcept {
        return a.view().compare(b.view()) <=> 0;
    }
    friend std::strong_ordering operator<=>(const CowString& a, std::string_view b) noexcept {
        return a.view().compare(b) <=> 0;
    }

    friend std::ostream& operator<<(std::ostream& os, const CowString& s) { return os << s.view(); }

private:
    std::string owned_;
    std::string_view view_;
    bool is_owned_ = false;
};

} // namespace fasthtmlparser
