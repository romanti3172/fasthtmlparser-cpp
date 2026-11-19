// Sibling links and positions for the CSS selectors, built once per document by the first selector that needs them.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace fasthtmlparser::detail {

struct DocumentData;

struct TypePositions {
    std::vector<std::int32_t> position;
    std::vector<std::int32_t> from_end;
};

struct CssTree {
    std::vector<std::int32_t> previous; // previous element sibling, -1 for none
    std::vector<std::int32_t> next;
    std::vector<std::int32_t> position; // 1-based position among the element siblings, 0: not an element
    std::vector<std::int32_t> from_end;
    mutable std::atomic<TypePositions*> types{nullptr};

    explicit CssTree(const DocumentData& doc);
    ~CssTree() { delete types.load(std::memory_order_acquire); }
    CssTree(const CssTree&) = delete;
    CssTree& operator=(const CssTree&) = delete;

    const TypePositions& type_positions(const DocumentData& doc) const;
};

/// The tree of the document, built on first use (thread-safe).
const CssTree& css_tree(const DocumentData& doc);

} // namespace fasthtmlparser::detail
