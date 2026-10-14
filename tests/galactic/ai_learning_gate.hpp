#pragma once

#include "ai_campaign_gate_access.hpp"
#include "ai_campaign_gate.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace galactic_learning_gate {
inline Snapshot capture(const ai::Engine& engine) { return ai::EngineContractAccess::learning_snapshot(engine); }
inline void compare(const Snapshot& control, const Snapshot& restored) {
    if (control != restored) throw std::runtime_error("installed learning histories/choices/RNG differ after disk restore");
}
struct Evidence {
    Snapshot saved, previous;
    std::int64_t first_resumed_frame{-1}, planning_frame{-1}, learning_frame{-1}, completion_frame{-1}, weighted_frame{-1}, learned_frame{-1};
    std::pair<eawr::sim::tactical::PlayerId, std::uint64_t> weighted_identity{};
    bool weighted_learned{};
    Choice weighted_choice;
    std::pair<eawr::sim::tactical::PlayerId, std::uint64_t> learned_identity{};
    explicit Evidence(Snapshot snapshot) : saved(snapshot), previous(std::move(snapshot)) {
        if (saved.frame < 0) throw std::runtime_error("post-return learning probe requires an admitted real parent");
    }
    void observe(const Snapshot& current) {
        if (current.frame < previous.frame) throw std::runtime_error("installed learning parent clock moved backwards");
        for (const auto& service : current.services) {
            const auto frame = static_cast<std::int64_t>(service.clocks.synchronization);
            if (first_resumed_frame < 0) first_resumed_frame = frame;
            if (planning_frame < 0 && service.service == ga::Service::planning) planning_frame = frame;
            if (learning_frame < 0 && service.service == ga::Service::learning) learning_frame = frame;
        }
        if (completion_frame < 0 && saved.frame < current.frame) {
            bool attempt{}, completion{};
            for (const auto& [id, player] : current.players) {
                const auto old = saved.players.find(id);
                for (const auto& [plan, counts] : player.plans) {
                    std::int64_t before{};
                    if (old != saved.players.end()) if (const auto found = old->second.plans.find(plan); found != old->second.plans.end()) before = found->second.second;
                    attempt = attempt || before < counts.second;
                }
            }
            for (const auto& row : current.completions) completion = completion || static_cast<std::uint64_t>(saved.frame) < std::get<0>(row);
            if (attempt && completion) completion_frame = current.frame;
        }
        // Require a later frame: observing a new potential and a completion in
        // one frame does not establish that the draw saw the completed outcome.
        if (completion_frame >= 0 && completion_frame < current.frame) {
            auto first = current.choices.end();
            for (auto row = current.choices.begin(); row != current.choices.end(); ++row) if (!previous.choices.contains(row->first)) {
                if (first == current.choices.end() || row->first.second < first->first.second) first = row;
                bool completed_reader{};
                const auto player = previous.players.find(row->first.first), original = saved.players.find(row->first.first);
                if (player != previous.players.end()) for (const auto& [plan, weight] : row->second.weights) {
                    static_cast<void>(weight);
                    const auto counts = player->second.plans.find(plan);
                    std::int64_t before{};
                    if (original != saved.players.end()) if (const auto found = original->second.plans.find(plan); found != original->second.plans.end()) before = found->second.second;
                    completed_reader = completed_reader || (counts != player->second.plans.end() && before < counts->second.second);
                }
                if (learned_frame < 0 && completed_reader) { learned_frame = current.frame; learned_identity = row->first; }
            }
            if (weighted_frame < 0 && first != current.choices.end()) {
                weighted_frame = current.frame; weighted_identity = first->first; weighted_learned = first->second.learned;
                weighted_choice = first->second;
            }
        }
        previous = current;
    }
    [[nodiscard]] bool complete() const noexcept {
        return first_resumed_frame >= 0 && planning_frame >= 0 && learning_frame >= 0 && completion_frame >= 0 && weighted_frame >= 0 && learned_frame >= 0;
    }
};
inline galactic_campaign_gate::LearningProbe probe(std::filesystem::path report = {}) {
    using RuntimeView = eawr::campaign::GalacticAiRuntimeView;
    using Key = std::pair<std::string, std::uint32_t>;
    auto evidence = std::make_shared<std::map<Key, Evidence>>();
    const auto snapshot = [](const RuntimeView& view, std::optional<std::uint64_t> after = {}) {
        const auto engine = view.engine.lock();
        const auto manager = view.manager.lock();
        if (!engine || !manager) throw std::runtime_error("installed learning probe lost its exact native runtime owners");
        auto result = capture(*engine); result.lifecycle = manager->lifecycle();
        // Plan timelines are process-local and reset on native restore. Only
        // newly produced completion receipts can be compared across this step.
        const auto since = after.value_or(result.lifecycle.clocks.synchronization);
        std::erase_if(result.completions, [since](const auto& row) { return std::get<0>(row) <= since; });
        if (after) for (const auto& row : manager->trace()) if (*after < row.clocks.synchronization) result.services.push_back(row);
        return result;
    };
    galactic_campaign_gate::LearningProbe result;
    result.capture = [evidence, snapshot](std::string_view boundary, std::uint32_t workers, const RuntimeView& control, const RuntimeView& restored) {
        if (boundary != "post-return") return;
        auto initial = snapshot(control); compare(initial, snapshot(restored));
        if (!evidence->emplace(Key{std::string(boundary), workers}, Evidence(std::move(initial))).second)
            throw std::runtime_error("installed learning post-return branch captured twice");
    };
    result.compare = [evidence, snapshot](std::string_view boundary, std::uint32_t workers,
        const eawr::campaign::GameTick&, const eawr::campaign::GameTick&, const RuntimeView& control, const RuntimeView& restored) {
        if (boundary != "post-return") return;
        const auto row = evidence->find(Key{std::string(boundary), workers});
        if (row == evidence->end()) throw std::runtime_error("installed learning branch has no disk capture evidence");
        if (row->second.complete()) return; // later steps still have the gate's full canonical comparison
        const auto after = row->second.previous.lifecycle.clocks.synchronization;
        const auto current = snapshot(control, after); compare(current, snapshot(restored, after));
        row->second.observe(current);
    };
    result.satisfied = [evidence](std::string_view boundary, std::uint32_t workers) {
        if (boundary != "post-return") return true;
        const auto row = evidence->find(Key{std::string(boundary), workers});
        return row != evidence->end() && row->second.complete();
    };
    result.passed = [evidence, report = std::move(report)] {
        bool passed = true;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const auto row = evidence->find(Key{"post-return", workers});
            passed = passed && row != evidence->end() && row->second.complete();
        }
        if (!report.empty()) {
            std::ofstream output(report);
            if (!output) throw std::runtime_error("installed learning evidence report unavailable");
            output << "{\"passed\":" << (passed ? "true" : "false") << ",\"branches\":[";
            bool first = true;
            for (const auto& [key, run] : *evidence) {
                if (!first) { output << ','; }
                first = false;
                output << "{\"workers\":" << key.second << ",\"saved_parent_frame\":" << run.saved.frame
                    << ",\"first_resumed_parent_frame\":" << run.first_resumed_frame << ",\"completion_frame\":" << run.completion_frame
                    << ",\"first_planning_frame\":" << run.planning_frame << ",\"first_learning_frame\":" << run.learning_frame
                    << ",\"first_observed_weighted_frame\":" << run.weighted_frame << ",\"player\":" << run.weighted_identity.first
                    << ",\"potential_plan\":" << run.weighted_identity.second << ",\"definition\":" << run.weighted_choice.definition
                    << ",\"learned_weights\":" << (run.weighted_learned ? "true" : "false") << ",\"learned_choice_frame\":" << run.learned_frame
                    << ",\"learned_player\":" << run.learned_identity.first << ",\"learned_potential\":" << run.learned_identity.second
                    << ",\"rng\":" << run.previous.random << '}';
            }
            output << "]}\n"; output.close();
            if (!output) throw std::runtime_error("installed learning evidence report write failed");
        }
        return passed;
    };
    return result;
}
}
