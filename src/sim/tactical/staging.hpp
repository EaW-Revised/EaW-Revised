#pragma once

#include "eawr/sim/tactical/types.hpp"

#include <iterator>
#include <map>
#include <optional>
#include <utility>

namespace eawr::sim::tactical::detail {

// A tick's sparse undo journal. Reads share the committed map; only an edited or removed
// entry is copied. Until commit(), destruction restores the map on every failure path.
// Mutation is confined to the ordered commit portions of a tick; partitioned readers use
// const access and never touch the journal.
template <typename T>
class MapStage final {
public:
    explicit MapStage(std::map<EntityId, T>& values) : values_(values) {}
    MapStage(const MapStage&) = delete;
    MapStage& operator=(const MapStage&) = delete;
    ~MapStage() {
        if (committed_) return;
        for (auto& [id, before] : undo_) {
            if (before) values_.insert_or_assign(id, std::move(*before));
            else values_.erase(id);
        }
    }

    using iterator = typename std::map<EntityId, T>::const_iterator;
    [[nodiscard]] iterator begin() const { return values_.cbegin(); }
    [[nodiscard]] iterator end() const { return values_.cend(); }
    [[nodiscard]] iterator find(EntityId id) const { return values_.find(id); }
    [[nodiscard]] bool empty() const { return values_.empty(); }
    [[nodiscard]] std::size_t size() const { return values_.size(); }
    [[nodiscard]] const T& read(EntityId id) const { return values_.at(id); }
    [[nodiscard]] T& at(EntityId id) { save(id); return values_.at(id); }
    [[nodiscard]] T& operator[](EntityId id) { save(id); return values_[id]; }
    void assign(EntityId id, T value) {
        const auto found = values_.find(id);
        if (found != values_.end() && found->second == value) return;
        save(id);
        values_.insert_or_assign(id, std::move(value));
    }
    template <typename U>
    void emplace(EntityId id, U&& value) {
        if (values_.contains(id)) return;
        save(id);
        values_.emplace(id, std::forward<U>(value));
    }
    iterator erase(iterator position) {
        save(position->first);
        return values_.erase(position);
    }
    template <typename Predicate>
    void erase_if(Predicate predicate) {
        for (auto it = begin(); it != end();) {
            it = predicate(*it) ? erase(it) : std::next(it);
        }
    }
    void commit() noexcept { committed_ = true; }
    [[nodiscard]] std::size_t copied_elements() const noexcept { return copied_; }

private:
    void save(EntityId id) {
        if (undo_.contains(id)) return;
        const auto found = values_.find(id);
        if (found == values_.end()) undo_.emplace(id, std::nullopt);
        else { undo_.emplace(id, found->second); ++copied_; }
    }
    std::map<EntityId, T>& values_;
    std::map<EntityId, std::optional<T>> undo_;
    std::size_t copied_{};
    bool committed_{};
};

} // namespace eawr::sim::tactical::detail
