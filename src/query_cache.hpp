// The cache of the compiled selectors and expressions behind the string methods (doc.css("..."), ...).
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace fasthtmlparser::detail {

struct TransparentHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

template <class Q>
class QueryCache {
public:
    /// The compiled query for key; compile(key) makes it (and throws on an invalid one).
    template <class F>
    std::shared_ptr<const Q> get(std::string_view key, F&& compile) {
        {
            std::shared_lock lock(mutex_);
            auto it = map_.find(key);
            if (it != map_.end()) return it->second;
        }
        auto q = std::make_shared<const Q>(compile(key));
        std::unique_lock lock(mutex_);
        if (map_.size() >= 1024) map_.clear(); // dynamically built queries must not grow the cache forever
        map_.emplace(std::string(key), q);
        return q;
    }

private:
    std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const Q>, TransparentHash, std::equal_to<>> map_;
};

} // namespace fasthtmlparser::detail
