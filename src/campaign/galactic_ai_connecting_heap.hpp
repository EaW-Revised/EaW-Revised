#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace eawr::campaign {
// WGA-327: entries expose total priority; payload never breaks a tie.
template<class Entry> class GalacticAiConnectingHeap {
public:
    bool empty() const { return entries_.empty(); }
    void push(Entry entry) {
        entries_.push_back(std::move(entry));
        auto child = entries_.size() - 1;
        while (child != 0) {
            const auto parent = (child - 1) / 2;
            if (!(entries_[child].priority < entries_[parent].priority)) break;
            std::swap(entries_[child], entries_[parent]);
            child = parent;
        }
    }
    // Requires a non-empty heap; callers must check empty() before extraction.
    Entry pop() {
        auto result = std::move(entries_.front());
        if (entries_.size() == 1) {
            entries_.pop_back();
            return result;
        }
        entries_.front() = std::move(entries_.back());
        entries_.pop_back();
        std::size_t parent = 0;
        for (;;) {
            auto best = parent;
            const auto left = parent * 2 + 1;
            const auto right = left + 1;
            // Strict comparisons retain the parent on equality and the left
            // child when both children strictly improve it equally (WGA-327).
            if (left < entries_.size() && entries_[left].priority < entries_[best].priority) best = left;
            if (right < entries_.size() && entries_[right].priority < entries_[best].priority) best = right;
            if (best == parent) break;
            std::swap(entries_[parent], entries_[best]);
            parent = best;
        }
        return result;
    }
private:
    std::vector<Entry> entries_;
};
} // namespace eawr::campaign
