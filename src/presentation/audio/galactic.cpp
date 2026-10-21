#include "eawr/presentation/audio/galactic.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <set>

namespace eawr::presentation::audio {
bool galactic_population_warning(std::uint32_t serviced, std::uint32_t local, bool strategic,
    sim::math::Fixed previous, sim::math::Fixed current, sim::math::Fixed capacity) noexcept {
    return local && serviced == local && strategic && previous.raw() > 0
        && current > previous && current >= capacity;
}

namespace {
std::string field(std::span<const Field> fields, std::string_view name) {
    for (const auto& [tag, value] : fields) if (upper(tag) == upper(name)) return value;
    return {};
}
}

std::string_view galactic_sound_tag(const GalacticSound cue) noexcept {
    switch (cue) {
    case GalacticSound::planet_gained: return "SFXEvent_Planet_Gained_Control";
    case GalacticSound::planet_lost: return "SFXEvent_Planet_Lost_Control";
    case GalacticSound::planet_corrupted: return "SFXEvent_Planet_Corrupted";
    case GalacticSound::corruption_removed: return "SFXEvent_Corruption_Removed";
    case GalacticSound::enemy_approaching: return "SFXEvent_Enemy_Fleet_Approaching_Planet";
    case GalacticSound::planet_revealed: return "SFXEvent_Planet_Revealed";
    case GalacticSound::capture_bonus: return "SFXEvent_Planet_Capture_Bonus_Reward";
    case GalacticSound::population_cap: return "SFXEvent_Strategic_Pop_Cap_Reached";
    case GalacticSound::credit_cap: return "SFXEvent_Max_Credits_Limit_Reached";
    case GalacticSound::mission_added: return "SFXEvent_Mission_Added";
    case GalacticSound::construction_options: return "SFXEvent_New_Construction_Options_Available";
    case GalacticSound::starbase_upgraded: return "SFXEvent_Starbase_Upgraded";
    case GalacticSound::enemy_starbase_upgraded: return "SFXEvent_Starbase_Enemy_Upgraded";
    case GalacticSound::ally_starbase_upgraded: return "SFXEvent_Starbase_Ally_Upgraded";
    case GalacticSound::blockade: return "SFXEvent_Build_Impossible_Location_Blockaded";
    case GalacticSound::space_battle_pending: return "SFX_Event_Tactical_Space_Battle_Pending";
    case GalacticSound::land_battle_pending: return "SFX_Event_Tactical_Land_Battle_Pending";
    }
    return {};
}

bool galactic_sound_uses_audio_table(const GalacticSound cue) noexcept {
    return cue == GalacticSound::planet_revealed || cue == GalacticSound::capture_bonus;
}

std::optional<GalacticSound> galactic_owner_sound(const std::uint32_t local_player,
    const std::uint32_t previous_owner, const std::uint32_t new_owner, const bool new_owner_playable) noexcept {
    if (local_player == 0 || previous_owner == new_owner) return std::nullopt;
    if (new_owner == local_player) return GalacticSound::planet_gained;
    // WGM-194: exact old-local identity and an existing playable new owner.
    if (previous_owner == local_player && new_owner && new_owner_playable) return GalacticSound::planet_lost;
    return std::nullopt;
}

std::string galactic_sound_event(const GalacticSound cue, std::span<const Field> faction,
    std::span<const Field> audio) {
    return field(galactic_sound_uses_audio_table(cue) ? audio : faction, galactic_sound_tag(cue));
}

std::optional<GalacticSound> galactic_event_sound(const sim::galactic::GalacticEvent& event,
    const std::uint32_t local, const std::optional<std::uint32_t> previous_owner,
    const std::optional<GalacticPendingAlert> pending, const bool new_owner_playable,
    const std::optional<std::uint32_t> construction_options_player,
    const std::optional<sim::galactic::GalacticFeedbackNotice> feedback) noexcept {
    if (!local) return std::nullopt;
    if (feedback) {
        if (feedback->player != local) return std::nullopt;
        using Kind = sim::galactic::GalacticFeedbackNotice::Kind;
        switch (feedback->kind) {
        case Kind::capture_reward: return GalacticSound::capture_bonus;
        case Kind::enemy_approaching: return GalacticSound::enemy_approaching;
        case Kind::planet_corrupted: return GalacticSound::planet_corrupted;
        case Kind::corruption_removed: return GalacticSound::corruption_removed;
        }
        return std::nullopt;
    }
    // SND-68: one ordinary faction SFX per qualified assignment, local only.
    if (construction_options_player)
        return *construction_options_player == local ? std::optional{GalacticSound::construction_options} : std::nullopt;
    using Kind = sim::galactic::GalacticEventKind;
    if (event.kind == Kind::planet_owner_changed && previous_owner)
        return galactic_owner_sound(local, *previous_owner, static_cast<std::uint32_t>(event.value), new_owner_playable);
    // WGUI-05: one feedback event per player/service, rather than per planet.
    if (event.kind == Kind::discovery_feedback && event.value == local)
        return GalacticSound::planet_revealed;
    if (event.kind == Kind::conflict_pending && pending
        && (pending->invader == local || pending->defender == local))
        return pending->land ? GalacticSound::land_battle_pending : GalacticSound::space_battle_pending;
    return std::nullopt;
}

std::string_view galactic_build_speech_tag(const GalacticBuildCue cue) noexcept {
    switch (cue) {
    case GalacticBuildCue::underway: return "Build_Speech_Underway";
    case GalacticBuildCue::completed: return "Build_Speech_Completed";
    case GalacticBuildCue::stopped: return "Build_Speech_Stopped";
    }
    return {};
}

std::string_view galactic_build_sfx_tag(const GalacticBuildCue cue) noexcept {
    switch (cue) {
    case GalacticBuildCue::underway: return "SFXEvent_Build_Started";
    case GalacticBuildCue::completed: return "SFXEvent_Build_Complete";
    case GalacticBuildCue::stopped: return "SFXEvent_Build_Cancelled";
    }
    return {};
}

GalacticBuildFeedback galactic_build_feedback(const GalacticBuildCue cue, std::span<const Field> type,
    const bool local_builder) {
    if (!local_builder) return {};
    // WGEP-96/97: authored speech suppresses the SFX fallback.
    if (auto speech = field(type, galactic_build_speech_tag(cue)); !speech.empty()) return {std::move(speech), {}};
    return {{}, field(type, galactic_build_sfx_tag(cue))};
}

std::vector<sim::galactic::FleetMember> galactic_fleet_move_members(
    std::span<const sim::galactic::FleetMember> members,
    const sim::galactic::MovementSetup* movement) {
    std::vector<sim::galactic::FleetMember> result;
    std::vector<sim::galactic::FleetMember> pending(members.rbegin(), members.rend());
    std::set<sim::EntityId> visited;
    std::map<sim::EntityId, const sim::galactic::MovementContents*> contents;
    if (movement) for (const auto& row : movement->contents) contents.emplace(row.container, &row);
    while (!pending.empty()) {
        const auto member = pending.back(); pending.pop_back();
        if (!visited.insert(member.id).second) continue;
        result.push_back(member);
        if (!movement) continue;
        const auto type = std::find_if(movement->types.begin(), movement->types.end(),
            [&](const auto& row) { return row.type == member.type; });
        if (type != movement->types.end() && type->squadron) continue;
        const auto children = contents.find(member.id);
        if (children != contents.end())
            pending.insert(pending.end(), children->second->children.rbegin(), children->second->children.rend());
    }
    return result;
}

std::string galactic_fleet_move_event(std::span<const std::vector<Field>> members,
    std::span<const std::string> rankings, std::span<const Field> category_masks) {
    // BA-20: ranking categories are masks, including authored composite aliases.
    std::map<std::string, std::uint64_t> masks;
    for (const auto& [name, raw] : category_masks) {
        auto text = std::string_view(raw);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
        const bool hex = text.starts_with("0x") || text.starts_with("0X");
        if (hex) text.remove_prefix(2);
        std::uint64_t bits{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), bits, hex ? 16 : 10);
        if (!text.empty() && error == std::errc{} && end == text.data() + text.size())
            masks.try_emplace(upper(name), bits);
    }
    std::vector<RankedUnit> units;
    for (std::size_t index = 0; index < members.size(); ++index) {
        const auto& traits = members[index];
        RankedUnit unit{index, {}, leading_integer(field(traits, "Ranking_In_Category"))};
        std::uint64_t bits{};
        std::string token;
        for (const char character : field(traits, "CategoryMask") + "|") {
            if (character == '|' || character == ',' || std::isspace(static_cast<unsigned char>(character))) {
                if (const auto mask = masks.find(upper(token)); mask != masks.end()) bits |= mask->second;
                token.clear();
            } else token += character;
        }
        for (const auto& category : rankings)
            if (const auto mask = masks.find(upper(category)); mask != masks.end() && (mask->second & bits))
                unit.categories.push_back(category);
        units.push_back(std::move(unit));
    }
    auto chosen = speaker(units, rankings);
    if (!chosen) return {};
    auto event = field(members[*chosen], "SFXEvent_Fleet_Move");
    if (event.empty()) event = field(members[*chosen], "SFXEvent_Move");
    if (!event.empty()) return event;
    // WGP-181: a silent top-ranked member falls back to an event-bearing
    // member; that fallback prefers the ordinary move response.
    std::erase_if(units, [&](const RankedUnit& unit) {
        return field(members[unit.index], "SFXEvent_Move").empty()
            && field(members[unit.index], "SFXEvent_Fleet_Move").empty();
    });
    chosen = speaker(units, rankings);
    if (!chosen) return {};
    event = field(members[*chosen], "SFXEvent_Move");
    if (event.empty()) event = field(members[*chosen], "SFXEvent_Fleet_Move");
    return event;
}

std::string strategic_result_event(std::span<const Field> fields, const bool won,
    const std::string_view opposing_faction) {
    const auto override_tag = won ? "Music_Event_Strategic_Win_Vs_Faction" : "Music_Event_Strategic_Lose_Vs_Faction";
    if (!opposing_faction.empty()) {
        for (const auto& [tag, value] : fields) {
            if (upper(tag) != upper(override_tag)) continue;
            const auto pair = split_list(value);
            if (!pair.empty() && upper(pair.front()) == upper(opposing_faction)) {
                return pair.size() > 1 ? pair[1] : std::string{};
            }
        }
    }
    return field(fields, won ? "Music_Event_Strategic_Win" : "Music_Event_Strategic_Lose");
}

std::optional<GalacticMusicDirector::Cue> GalacticMusicDirector::ambient(const MusicEvent* event,
    const bool strategic_submode, const bool demo_attract, const bool forced) {
    if (!strategic_submode) return std::nullopt;
    if (!forced && (demo_attract || mode_ == Mode::strategic_ambient || mode_ == Mode::superweapon_attack
        || mode_ == Mode::demo_finale || mode_ == Mode::scripted)) return std::nullopt;
    return start(Mode::strategic_ambient, event);
}

std::optional<GalacticMusicDirector::Cue> GalacticMusicDirector::outcome(const bool exact_local_winner,
    const bool participants_and_local_present, const MusicEvent* win, const MusicEvent* lose) {
    if (!participants_and_local_present) return std::nullopt;
    if (exact_local_winner) return mode_ == Mode::strategic_win ? std::nullopt : start(Mode::strategic_win, win);
    if (mode_ != Mode::strategic_lose) return start(Mode::strategic_lose, lose);
    return std::nullopt;
}

std::optional<GalacticMusicDirector::Cue> GalacticMusicDirector::start(const Mode mode, const MusicEvent* event) {
    if (!event || event->files.empty()) return std::nullopt;
    // SND-53: even a different requested mode does not restart the same event.
    if (event == current_) return std::nullopt;
    mode_ = mode;
    current_ = event;
    auto& next = next_file_[event];
    const auto file = event->files[next++ % event->files.size()];
    return Cue{event, file, mode};
}

GalacticMusicDirector::Loading GalacticMusicDirector::battle_load(const MusicEvent* event,
    const bool low_memory, const bool tutorial) {
    if (!event) return {};
    const bool stop = low_memory || tutorial || (mode_ != Mode::battle_pending && mode_ != Mode::battle_load);
    if (stop) external_mode(Mode::none);
    return {stop, low_memory ? std::nullopt : start(Mode::battle_load, event)};
}

std::optional<GalacticMusicDirector::Cue> GalacticMusicDirector::track_ended() {
    if (!current_) return std::nullopt;
    const auto next = next_file_[current_];
    if (!current_->loop && next >= current_->files.size()) return std::nullopt;
    auto& cursor = next_file_[current_];
    return Cue{current_, current_->files[cursor++ % current_->files.size()], mode_};
}
} // namespace eawr::presentation::audio
