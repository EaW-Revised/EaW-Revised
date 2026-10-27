#pragma once

#include "eawr/campaign/game_session.hpp"
#include "../sim/galactic/checkpoint_commands_internal.hpp"
#include "checkpoint_fields_internal.hpp"

#include <algorithm>

namespace eawr::sim::galactic::checkpoint_detail {
template <> struct Fields<tactical::TacticalReplay> {
    template <class A, class T> static void apply(A& a, T& v) {
        if constexpr (A::reading) {
            std::vector<std::uint8_t> bytes; a(bytes);
            auto parsed = tactical::parse_replay(bytes);
            if (!parsed) throw Invalid(parsed.error().message);
            v = std::move(parsed).value();
        } else {
            auto bytes = tactical::write_replay(v);
            if (!bytes) throw Invalid(bytes.error().message);
            a(bytes.value());
        }
    }
};
template <> struct Fields<tactical::TacticalSetup> {
    template <class A, class T> static void apply(A& a, T& v) {
        tactical::TacticalReplay replay;
        if constexpr (!A::reading) replay.setup = v;
        a(replay);
        if constexpr (A::reading) v = std::move(replay.setup);
    }
};
template <> struct Fields<std::monostate> {
    template <class A, class T> static void apply(A&, T&) {}
};
template <> struct Fields<script::numeric::LuaNumber> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.repr); }
};
template <> struct Fields<script::authoritative::Handle> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.kind, v.id); }
};
template <> struct Fields<script::authoritative::Value> {
    static bool within_list_depth(const script::authoritative::Value& value, int depth = 0) {
        const auto* list = std::get_if<script::authoritative::ValueList>(&value.data);
        if (!list) return true;
        if (depth >= script::authoritative::max_value_list_depth) return false;
        return std::all_of(list->begin(), list->end(), [depth](const auto& item) {
            return within_list_depth(item, depth + 1);
        });
    }
    template <class A, class T> static void apply(A& a, T& v) {
        if constexpr (!A::reading)
            if (!within_list_depth(v)) throw Invalid("host value exceeds list depth budget");
        a(v.data);
        if constexpr (A::reading)
            if (!within_list_depth(v)) throw Invalid("host value exceeds list depth budget");
    }
};
template <> struct Fields<script::authoritative::ScriptCommand> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.tick, v.issuer, v.sequence, v.verb, v.arguments); }
};
template <> struct Fields<script::authoritative::TacticalScriptSave> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.completed_tick, v.scheduler, v.pending, v.last_keys, v.native); }
};
template <> struct Fields<campaign::ModeId> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.mode, v.conflict); }
};
template <> struct Fields<campaign::ModeTimelineSnapshot> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.mode, v.completed_tick, v.value); }
};
struct ChildOwner {
    campaign::ConflictRecord conflict{};
    std::vector<std::uint8_t> state{};
    script::authoritative::TacticalScriptSave scripts{};
    tactical::TacticalReplay log{};
    std::map<tactical::PlayerId, std::uint64_t> sequences{};
    std::map<EntityId, tactical::PlayerId> killers{};
    bool owns_pause{};
    std::vector<campaign::PendingBattleInput> controller_inputs;
};
template <> struct Fields<campaign::PendingBattleInput> {
    template <class A, class T> static void apply(A& a, T& v) { a(v.outer, v.body, v.producer_sequence); }
};
template <> struct Fields<ChildOwner> {
    template <class A, class T> static void apply(A& a, T& v) {
        auto& conflict = v.conflict;
        a(conflict, v.state, v.scripts, v.log, v.sequences, v.killers, v.owns_pause);
        if (conflict.tactical.controllers) {
            std::uint32_t tag{0x4c525443U}, version{4};
            a(tag, version);
            if (tag != 0x4c525443U || version != 4) throw Invalid("unsupported child controller continuation");
            a(v.controller_inputs);
        }
    }
};
} // namespace eawr::sim::galactic::checkpoint_detail
