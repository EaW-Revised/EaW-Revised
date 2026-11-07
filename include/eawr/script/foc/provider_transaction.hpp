#pragma once

#include "eawr/core/result.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace eawr::script::foc::galactic_ai {

class ProviderTransaction;

// A provider-owned slice, not an archive of all native owners. Readers retain
// immutable publications; the ordered owner thread alone publishes replacements.
// Keep this owner alive and at a stable address until its transactions settle.
template<class T> class ProviderState final {
public:
    explicit ProviderState(T initial) : published_(std::make_shared<const T>(std::move(initial))) {}
    ProviderState(const ProviderState&) = delete;
    ProviderState& operator=(const ProviderState&) = delete;
    ProviderState(ProviderState&&) = delete;
    ProviderState& operator=(ProviderState&&) = delete;

    [[nodiscard]] std::shared_ptr<const T> snapshot() const noexcept { return published_; }

private:
    friend class ProviderTransaction;
    std::shared_ptr<const T> published_;
};

// Transaction scaffolding for one provider chain. This does not make callbacks
// transactional by itself: every mutable owner and outbound journal they touch
// must be represented by a slice. Infrastructure errors are never classified or
// swallowed here. Only the owning barrier decides whether a failure retires a
// provider or refuses the session.
//
// Enlist/read does not copy T. The first write copies only that enlisted slice;
// subsequent writes share its private copy. Commit validates the complete read
// set before any no-throw publication swap. Each partition worker owns a
// separate transaction over disjoint owners; never share a transaction across
// workers. Join them before publishing at the ordered owner barrier.
// Destruction implicitly discards all private copies that were not committed;
// reject() poisons the chain but retains those copies until destruction.
class ProviderTransaction final {
public:
    enum class Commit { committed, rejected, conflict, closed, busy };
    struct Work {
        std::size_t enlisted{}, detached{}, published{};
    };

    ProviderTransaction() = default;
    ProviderTransaction(const ProviderTransaction&) = delete;
    ProviderTransaction& operator=(const ProviderTransaction&) = delete;
    ProviderTransaction(ProviderTransaction&&) = delete;
    ProviderTransaction& operator=(ProviderTransaction&&) = delete;

    template<class T> [[nodiscard]] const T& read(ProviderState<T>& owner) {
        try {
            auto& entry = enlist(owner);
            return entry.staged ? *entry.staged : *entry.base;
        } catch (...) {
            rejected_ = true;
            throw;
        }
    }

    // References passed to a writer may not escape the callback. A failed
    // Result or exception poisons the whole chain, including prior writes to
    // other owners. The exception propagates unchanged to its real boundary.
    template<class T, class Writer>
    [[nodiscard]] core::Result<void> stage(ProviderState<T>& owner, Writer&& writer) {
        return run([&]() -> core::Result<void> {
            auto& entry = enlist(owner);
            if (!entry.staged) {
                entry.staged = std::make_shared<T>(*entry.base);
                ++work_.detached;
            }
            return std::invoke(std::forward<Writer>(writer), *entry.staged);
        });
    }

    // Also covers failures before the first mutation, or between staged writes.
    template<class Callback> [[nodiscard]] core::Result<void> run(Callback&& callback) {
        require_open();
        struct Active {
            std::size_t& count;
            explicit Active(std::size_t& value) noexcept : count(value) { ++count; }
            ~Active() { --count; }
        } active(active_calls_);
        try {
            auto result = std::invoke(std::forward<Callback>(callback));
            if (!result) rejected_ = true;
            return result;
        } catch (...) {
            rejected_ = true;
            throw;
        }
    }

    void reject() noexcept { rejected_ = true; }
    [[nodiscard]] Work work() const noexcept { return work_; }

    [[nodiscard]] Commit commit() noexcept {
        if (active_calls_ != 0) return Commit::busy;
        if (closed_) return Commit::closed;
        closed_ = true;
        if (rejected_) return Commit::rejected;
        // Bounded owner slices commit serially to make publication indivisible
        // to the next provider. These loops never visit the world's entities.
        for (const auto& entry : entries_) if (!entry->current()) return Commit::conflict;
        // No allocations, validation callbacks or externally visible effects
        // belong in this loop. All readers are joined at the ordered barrier.
        for (auto& entry : entries_) if (entry->publish()) ++work_.published;
        return Commit::committed;
    }

private:
    struct EntryBase {
        explicit EntryBase(const void* address) noexcept : identity(address) {}
        virtual ~EntryBase() = default;
        const void* identity;
        [[nodiscard]] virtual bool current() const noexcept = 0;
        [[nodiscard]] virtual bool publish() noexcept = 0;
    };
    template<class T> struct Entry final : EntryBase {
        explicit Entry(ProviderState<T>& state)
            : EntryBase(&state), owner(state), base(state.published_) {}
        ProviderState<T>& owner;
        std::shared_ptr<const T> base;
        std::shared_ptr<T> staged;
        [[nodiscard]] bool current() const noexcept override { return owner.published_ == base; }
        [[nodiscard]] bool publish() noexcept override {
            if (!staged) return false;
            // Keep base alive through every publication: destruction of the old
            // value cannot reenter the owner in the middle of the commit loop.
            owner.published_ = std::move(staged);
            return true;
        }
    };

    void require_open() const {
        if (closed_ || rejected_) throw std::logic_error("provider transaction is no longer writable");
    }
    template<class T> Entry<T>& enlist(ProviderState<T>& owner) {
        require_open();
        for (auto& entry : entries_)
            if (entry->identity == &owner) return static_cast<Entry<T>&>(*entry);
        auto entry = std::make_unique<Entry<T>>(owner);
        auto& result = *entry;
        entries_.push_back(std::move(entry));
        ++work_.enlisted;
        return result;
    }

    std::vector<std::unique_ptr<EntryBase>> entries_;
    Work work_;
    std::size_t active_calls_{};
    bool rejected_{}, closed_{};
};

} // namespace eawr::script::foc::galactic_ai
