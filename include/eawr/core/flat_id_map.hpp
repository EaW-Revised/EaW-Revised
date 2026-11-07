#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace eawr::core {

// An open-addressing map from 64-bit ids to records, for per-entity presentation state that is looked up and
// assigned for every unit every frame and never iterated: linear probing, backward-shift erase (no tombstones),
// power-of-two capacity at a load of at most one half, one contiguous allocation. The surface is the part of
// std::map those callers use: find() returns a pointer to a slot (`first` the id, `second` the record) or
// nullptr, which is also end(); contains, operator[], erase by id (erase_slot by slot), erase_if, size, clear. A pointer to a slot
// is valid until the next insert (operator[]) or erase; nothing keeps one across such a call.
template <class Value>
class FlatIdMap final {
public:
    struct Slot final {
        std::uint64_t first{};
        Value second{};
        bool used{};
    };

    [[nodiscard]] Slot* find(const std::uint64_t id) noexcept {
        const std::size_t at = locate(id);
        return at == npos ? nullptr : &slots_[at];
    }
    [[nodiscard]] const Slot* find(const std::uint64_t id) const noexcept {
        const std::size_t at = locate(id);
        return at == npos ? nullptr : &slots_[at];
    }
    [[nodiscard]] static constexpr Slot* end() noexcept { return nullptr; }
    [[nodiscard]] bool contains(const std::uint64_t id) const noexcept { return locate(id) != npos; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    // The record for `id`, default-constructed when it is new.
    Value& operator[](const std::uint64_t id) {
        if (const std::size_t at = locate(id); at != npos) return slots_[at].second;
        if ((size_ + 1) * 2 > slots_.size()) grow();
        std::size_t at = home(id);
        while (slots_[at].used) at = (at + 1) & mask();
        slots_[at].first = id;
        slots_[at].second = Value{};
        slots_[at].used = true;
        ++size_;
        return slots_[at].second;
    }

    std::size_t erase(const std::uint64_t id) {
        const std::size_t at = locate(id);
        if (at == npos) return 0;
        remove_at(at);
        return 1;
    }
    // By slot (the pointer find() returned); named apart from erase(id) so a literal 0 stays unambiguous.
    void erase_slot(const Slot* slot) { remove_at(static_cast<std::size_t>(slot - slots_.data())); }

    // Erases every record `drop(id, record)` is true for and returns how many; `drop` must be a pure function of
    // its arguments (a record the backward shift moves is asked again). One sweep over the table, in slot order.
    template <class Drop>
    std::size_t erase_if(Drop&& drop) {
        std::size_t erased = 0;
        for (std::size_t at = 0; at < slots_.size();) {
            if (slots_[at].used && drop(slots_[at].first, slots_[at].second)) {
                remove_at(at);
                ++erased;
                continue; // the backward shift may have moved a later record into this slot
            }
            ++at;
        }
        return erased;
    }

    void clear() {
        for (Slot& slot : slots_) {
            slot.used = false;
            slot.second = Value{};
        }
        size_ = 0;
    }

private:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    [[nodiscard]] std::size_t mask() const noexcept { return slots_.size() - 1; }
    [[nodiscard]] std::size_t home(const std::uint64_t id) const noexcept {
        // Fibonacci hashing: the high bits of the product spread sequential ids.
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ULL) >> shift_);
    }
    [[nodiscard]] std::size_t locate(const std::uint64_t id) const noexcept {
        if (slots_.empty()) return npos;
        for (std::size_t at = home(id); slots_[at].used; at = (at + 1) & mask()) {
            if (slots_[at].first == id) return at;
        }
        return npos;
    }

    void grow() {
        std::vector<Slot> old;
        old.swap(slots_);
        const std::size_t capacity = old.empty() ? 16 : old.size() * 2;
        slots_.assign(capacity, Slot{});
        unsigned bits = 0;
        while ((std::size_t{1} << bits) < capacity) ++bits;
        shift_ = 64U - bits;
        for (Slot& slot : old) {
            if (!slot.used) continue;
            std::size_t at = home(slot.first);
            while (slots_[at].used) at = (at + 1) & mask();
            slots_[at] = std::move(slot);
        }
    }

    // Backward-shift deletion: later members of the probe run move up into the hole unless their home lies
    // cyclically after it, so lookups never need tombstones.
    void remove_at(std::size_t hole) {
        std::size_t scan = hole;
        for (;;) {
            scan = (scan + 1) & mask();
            if (!slots_[scan].used) break;
            const std::size_t wanted = home(slots_[scan].first);
            const bool stays = hole <= scan ? (hole < wanted && wanted <= scan) : (hole < wanted || wanted <= scan);
            if (stays) continue;
            slots_[hole] = std::move(slots_[scan]);
            hole = scan;
        }
        slots_[hole].used = false;
        slots_[hole].second = Value{};
        --size_;
    }

    std::vector<Slot> slots_;
    std::size_t size_{};
    unsigned shift_{63};
};

} // namespace eawr::core
