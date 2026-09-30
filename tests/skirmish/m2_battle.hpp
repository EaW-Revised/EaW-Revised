#pragma once

// The M2 battle with the AI on both sides, as the soak (#627) and the tag perturbation check
// (docs/tag-applied-check.md) build it from one loaded installation.

#include "eawr/data/xml.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace eawr::soak {

struct Loaded {
    const vfs::Vfs& filesystem;
    const data::Catalog& catalog;
    const units::UnitTables& tables;
};

// The pieces of one battle, in the order their lifetimes need: the content and AI setup the
// world and the scripts were built from outlive the session.
struct Battle {
    skirmish::SessionContent content;
    script::foc::AiSetup ai;
    std::map<std::string, std::string> modules;
    std::optional<script::authoritative::ScriptedTacticalSession> session;
};

struct BattleOptions {
    std::uint64_t seed{1};
    // The setup's content identity is left zero, so two battles on different unit tables hash
    // alike until their states differ (the perturbation check compares behaviour, not tables).
    bool anonymous_content{};
};

// The battle, or why it could not be built.
[[nodiscard]] std::variant<std::unique_ptr<Battle>, std::string> build_battle(
    const Loaded& loaded, const BattleOptions& options);

} // namespace eawr::soak
