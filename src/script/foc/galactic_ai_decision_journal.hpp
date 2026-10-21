#pragma once

#include "eawr/script/foc/galactic_ai.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <span>
#include <stdexcept>
#include <vector>

namespace eawr::script::foc::galactic_ai {
// Transient barrier work, never a save/replay format. Each owner appends to its
// pre-created stream on one decision thread. The barrier joins those threads
// before commit; streams publish in player order and preserve cross-component
// operation order within each player. Callbacks own their DTO inputs and retain
// only live facade owners, never a Lua stack or a temporary publication callback.
class DecisionIntentJournal {
public:
    DecisionIntentJournal() = default;
    DecisionIntentJournal(const DecisionIntentJournal&) = delete;
    DecisionIntentJournal& operator=(const DecisionIntentJournal&) = delete;
    // false is a retained refusal; an error aborts the canonical publication.
    using Replay = std::function<core::Result<bool>()>;
    [[nodiscard]] core::Result<void> prepare(std::span<const gc::PlayerId> players) {
        if (phase_ != Phase::idle || !std::is_sorted(players.begin(), players.end())
            || std::adjacent_find(players.begin(), players.end()) != players.end()
            || std::any_of(players.begin(), players.end(), [](auto player) { return !player; }))
            return invalid<void>("decision intents require an idle barrier and ordered known owners");
        streams_.clear();
        for (const auto player : players) streams_.try_emplace(player);
        phase_ = Phase::recording;
        return core::Result<void>::success();
    }
    void append(gc::PlayerId player, Replay replay) {
        const auto at = streams_.find(player);
        if (phase_ != Phase::recording || at == streams_.end() || !replay)
            throw std::logic_error("decision intent requires its prepared owner stream");
        if (at->second.size() == gc::galactic_max_commands)
            throw std::length_error("decision intent owner exhausted the barrier command bound");
        at->second.push_back(std::move(replay));
    }
    [[nodiscard]] core::Result<std::size_t> commit() {
        if (phase_ != Phase::recording) return invalid<std::size_t>("decision intent commit requires its recorded barrier");
        std::size_t count{};
        for (const auto& [player, stream] : streams_) {
            static_cast<void>(player);
            if (stream.size() > gc::galactic_max_commands - count)
                return invalid<std::size_t>("decision intents exceed the whole barrier command bound");
            count += stream.size();
        }
        phase_ = Phase::committing;
        struct Finish {
            DecisionIntentJournal& journal;
            ~Finish() { journal.streams_.clear(); journal.phase_ = Phase::idle; }
        } finish{*this};
        std::size_t refused{};
        for (const auto& [player, stream] : streams_) {
            static_cast<void>(player);
            for (const auto& replay : stream) {
                auto result = replay();
                if (!result) return core::Result<std::size_t>::failure(result.error());
                if (!result.value()) ++refused;
            }
        }
        return core::Result<std::size_t>::success(refused);
    }
    // The caller has joined every writer before abandoning a failed barrier.
    void cancel() {
        if (phase_ == Phase::committing) throw std::logic_error("cannot abandon a publishing decision journal");
        streams_.clear(); phase_ = Phase::idle;
    }
private:
    template<class T> static core::Result<T> invalid(const char* message) {
        core::Diagnostic error; error.code = std::string(authoritative::codes::invalid_request); error.message = message;
        return core::Result<T>::failure(std::move(error));
    }
    enum class Phase { idle, recording, committing } phase_{};
    std::map<gc::PlayerId, std::vector<Replay>> streams_;
};
} // namespace eawr::script::foc::galactic_ai
