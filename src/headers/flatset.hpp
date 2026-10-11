// SPDX-License-Identifier: GPL-3.0-only
/**
 * @file flatset.hpp
 *
 * @copyright Copyright (C) 2018-2024 srcML, LLC. (www.srcML.org)
 *
 * This file is part of the srcSlice application.
 */

#ifndef SRCSLICE_FLATSET
#define SRCSLICE_FLATSET

#include <algorithm>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <utility>
#include <vector>

// A sorted, duplicate-free vector with the std::set operations srcslice uses. It keeps the
// same element order as std::set<T, Compare> and the same rule for equivalent elements (the
// first inserted stays), but holds the elements contiguously: no tree node per
// element (32 bytes plus the allocator's overhead on top of a 32-byte position, millions of
// times) and a merge of two sets is one linear pass instead of an insertion per element
// (perf plan: the std::set inserts of ResolveCall and the position sets' node overhead).
// Elements are immutable through the iterators, as in std::set.
template<class T, class Compare = std::less<T>>
class FlatSet {
public:
    using value_type = T;
    using const_iterator = typename std::vector<T>::const_iterator;
    using iterator = const_iterator;
    using size_type = std::size_t;

    FlatSet() = default;
    FlatSet(std::initializer_list<T> init) { insert(init.begin(), init.end()); }

    iterator begin() const { return items.begin(); }
    iterator end() const { return items.end(); }
    size_type size() const { return items.size(); }
    bool empty() const { return items.empty(); }
    void clear() { items.clear(); }

    // the sorted elements as a plain vector
    const std::vector<T>& values() const { return items; }

    std::pair<iterator, bool> insert(const T& value) {
        auto pos = std::lower_bound(items.begin(), items.end(), value, Less);
        if (pos != items.end() && !Less(value, *pos)) return {pos, false};
        return {items.insert(pos, value), true};
    }
    std::pair<iterator, bool> insert(T&& value) {
        auto pos = std::lower_bound(items.begin(), items.end(), value, Less);
        if (pos != items.end() && !Less(value, *pos)) return {pos, false};
        return {items.insert(pos, std::move(value)), true};
    }
    template<class... Args>
    std::pair<iterator, bool> emplace(Args&&... args) { return insert(T(std::forward<Args>(args)...)); }

    // Inserts [first, last) in order. A range that is already sorted and free of equivalent
    // neighbours (another FlatSet, or any set with the same order) is merged in one pass;
    // anything else is inserted element by element. Both give the result std::set gives.
    template<class InputIt>
    void insert(InputIt first, InputIt last) {
        if (first == last) return;
        if (IsSortedUnique(first, last)) {
            Merge(first, last);
            return;
        }
        for (; first != last; ++first) insert(*first);
    }

    // Inserts several sorted, duplicate-free ranges as if one by one in the given order (the
    // first of equivalent elements wins), merging adjacent pairs level by level: log(k)
    // linear passes instead of k merges into a set that grows with each. The ranges are
    // copied out first, so they may point into this set's own storage.
    using Range = std::pair<const T*, const T*>;
    void InsertRanges(const std::vector<Range>& ranges) {
        if (ranges.empty()) return;
        std::size_t total = 0;
        for (const Range& range : ranges) total += static_cast<std::size_t>(range.second - range.first);
        if (total == 0) return;

        std::vector<T> a, b;
        std::vector<std::size_t> bounds, next;   // offsets where each pending range starts, then the end
        a.reserve(total);
        bounds.reserve(ranges.size() + 1);
        for (const Range& range : ranges) {
            if (range.first == range.second) continue;
            bounds.push_back(a.size());
            a.insert(a.end(), range.first, range.second);
        }
        bounds.push_back(a.size());

        while (bounds.size() > 2) {
            b.clear();
            b.reserve(a.size());
            next.clear();
            for (std::size_t i = 0; i + 1 < bounds.size(); i += 2) {
                next.push_back(b.size());
                if (i + 2 < bounds.size()) {
                    std::set_union(a.begin() + static_cast<std::ptrdiff_t>(bounds[i]), a.begin() + static_cast<std::ptrdiff_t>(bounds[i + 1]),
                                   a.begin() + static_cast<std::ptrdiff_t>(bounds[i + 1]), a.begin() + static_cast<std::ptrdiff_t>(bounds[i + 2]),
                                   std::back_inserter(b), Less);
                } else {
                    b.insert(b.end(), a.begin() + static_cast<std::ptrdiff_t>(bounds[i]), a.begin() + static_cast<std::ptrdiff_t>(bounds[i + 1]));
                }
            }
            next.push_back(b.size());
            a.swap(b);
            bounds.swap(next);
        }
        Merge(a.begin(), a.end());
    }

    size_type erase(const T& value) {
        auto pos = std::lower_bound(items.begin(), items.end(), value, Less);
        if (pos == items.end() || Less(value, *pos)) return 0;
        items.erase(pos);
        return 1;
    }
    iterator erase(iterator pos) { return items.erase(pos); }

    iterator find(const T& value) const {
        auto pos = std::lower_bound(items.begin(), items.end(), value, Less);
        if (pos == items.end() || Less(value, *pos)) return items.end();
        return pos;
    }
    size_type count(const T& value) const { return find(value) == end() ? 0 : 1; }
    bool contains(const T& value) const { return find(value) != end(); }

    void reserve(size_type n) { items.reserve(n); }
    void shrink_to_fit() { items.shrink_to_fit(); }
    void swap(FlatSet& other) noexcept { items.swap(other.items); }

    bool operator==(const FlatSet& rhs) const { return items == rhs.items; }
    bool operator!=(const FlatSet& rhs) const { return !(items == rhs.items); }

private:
    static bool Less(const T& a, const T& b) { return Compare()(a, b); }

    template<class InputIt>
    static bool IsSortedUnique(InputIt first, InputIt last) {
        InputIt prev = first;
        for (InputIt it = std::next(first); it != last; prev = it, ++it) {
            if (!Less(*prev, *it)) return false;
        }
        return true;
    }

    // set union keeping our element when both ranges hold equivalent ones; the result's
    // capacity is exactly its size plus nothing beyond the two inputs' lengths
    template<class InputIt>
    void Merge(InputIt first, InputIt last) {
        if (items.empty()) {
            items.assign(first, last);
            return;
        }
        // common case in Finalize: everything new sorts after what is there
        if (Less(items.back(), *first)) {
            items.insert(items.end(), first, last);
            return;
        }
        std::vector<T> merged;
        merged.reserve(items.size() + static_cast<size_type>(std::distance(first, last)));
        std::set_union(items.begin(), items.end(), first, last, std::back_inserter(merged), Less);
        items = std::move(merged);
    }

    std::vector<T> items;
};

#endif
