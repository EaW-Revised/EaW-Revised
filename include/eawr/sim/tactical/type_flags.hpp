#pragma once

#include "eawr/sim/tactical/types.hpp"
#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace eawr::sim::tactical {
// RG-04: sparse flags indexed by the eight bytes of a type ID. Queries take at
// most eight array reads, independent of roster size, and never allocate.
// Immutable shared storage keeps copied session rules cheap.
class TypeFlags {
    using Node = std::array<std::size_t, 256>;
    struct Index { std::vector<Node> nodes{1}; };
    std::vector<TypeId> ids_;
    std::shared_ptr<const Index> index_;
public:
    TypeFlags() = default;
    TypeFlags(std::vector<TypeId> ids) { *this = std::move(ids); }
    TypeFlags& operator=(std::vector<TypeId> ids) {
        auto index = std::make_shared<Index>();
        for (const auto id : ids) {
            std::size_t node = 0;
            for (int shift = 56; shift > 0; shift -= 8) {
                const auto byte = (id >> shift) & 255U;
                auto next = index->nodes[node][byte];
                if (next == 0) {
                    next = index->nodes.size();
                    index->nodes[node][byte] = next;
                    index->nodes.emplace_back();
                }
                node = next;
            }
            index->nodes[node][id & 255U] = 1;
        }
        ids_ = std::move(ids);
        index_ = std::move(index);
        return *this;
    }
    [[nodiscard]] bool contains(const TypeId id, std::size_t* query_work = nullptr) const noexcept {
        if (!index_) return false;
        std::size_t node = 0;
        for (int shift = 56; shift > 0; shift -= 8) {
            if (query_work) ++*query_work;
            node = index_->nodes[node][(id >> shift) & 255U];
            if (node == 0) return false;
        }
        if (query_work) ++*query_work;
        return index_->nodes[node][id & 255U] != 0;
    }
    void clear() noexcept { ids_.clear(); index_.reset(); }
    auto begin() const noexcept { return ids_.begin(); }
    auto end() const noexcept { return ids_.end(); }
    friend bool operator==(const TypeFlags& a, const TypeFlags& b) { return a.ids_ == b.ids_; }
};
} // namespace eawr::sim::tactical
