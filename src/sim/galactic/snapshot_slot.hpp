#pragma once

#include "eawr/sim/galactic/snapshot.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace eawr::sim::galactic {

// The published snapshot of one galactic session, with an optional pending rebuild (#6933).
//
// A write that republishes the snapshot may, inside a batch, only mark the slot stale (`defer`): the rebuild runs once
// on the next read of the slot (any `->`, `*`, `get`, `current` or conversion to the shared pointer) or when the batch
// ends. Every read therefore sees exactly the snapshot an immediate rebuild at the last write would have produced, and
// the slot is never observed stale from outside.
//
// Threading. Writes (`operator=`, `defer`) belong to the session's owning thread, exactly as before this type existed:
// no reader may run while a write is in progress. Reads may come from any thread between writes, as they could before
// (the partitioned phases of a tick and the script queries read the published snapshot concurrently). A read that finds
// the slot stale settles it under a lock, so concurrent first readers rebuild once and all of them see the rebuilt
// snapshot; a read of a settled slot takes no lock. The rebuild itself runs on the reading thread.
class SnapshotSlot final {
public:
    using Pointer = std::shared_ptr<const GalacticSnapshot>;

    SnapshotSlot() = default;
    SnapshotSlot(const SnapshotSlot&) = delete;
    SnapshotSlot& operator=(const SnapshotSlot&) = delete;

    // The owner and its rebuild callback; the callback installs the rebuilt snapshot with `operator=`.
    void bind(void* owner, void (*rebuild)(void*)) noexcept { owner_ = owner; rebuild_ = rebuild; }
    // An immediate publication replaces the value and drops any pending rebuild.
    SnapshotSlot& operator=(Pointer value) noexcept {
        value_ = std::move(value);
        pending_.store(false, std::memory_order_release);
        return *this;
    }
    // A deferred publication keeps the previous value until the next read.
    void defer() noexcept { pending_.store(true, std::memory_order_release); }
    [[nodiscard]] bool pending() const noexcept { return pending_.load(std::memory_order_acquire); }

    [[nodiscard]] const GalacticSnapshot* operator->() const { settle(); return value_.get(); }
    [[nodiscard]] const GalacticSnapshot& operator*() const { settle(); return *value_; }
    [[nodiscard]] const GalacticSnapshot* get() const { settle(); return value_.get(); }
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(value_); }
    operator const Pointer&() const { settle(); return value_; }
    // The settled shared pointer, named: for comparisons and conditional expressions where the implicit conversion is not
    // tried, and for the end of a batch.
    [[nodiscard]] const Pointer& current() const { settle(); return value_; }

private:
    void settle() const {
        if (!pending_.load(std::memory_order_acquire) || !rebuild_) return;
        const std::lock_guard lock(mutex_);
        // The rebuild reads the slot (its previous value) on this thread: that nested read must not rebuild again.
        if (rebuilding_ || !pending_.load(std::memory_order_acquire)) return;
        rebuilding_ = true;
        struct Done { bool& flag; ~Done() { flag = false; } } done{rebuilding_};
        rebuild_(owner_);
    }

    Pointer value_;
    std::atomic<bool> pending_{};
    mutable std::recursive_mutex mutex_;
    mutable bool rebuilding_{};
    void* owner_{};
    void (*rebuild_)(void*){};
};

} // namespace eawr::sim::galactic
