#pragma once

#include "eawr/presentation/audio/sfx.hpp"
#include "eawr/sim/galactic/events.hpp"

namespace eawr::presentation::audio {

// WGP-27 owns the mapping. The services owning movement/economy/corruption
// decide when these semantic notifications are emitted for the local player.
enum class GalacticSound {
    planet_gained, planet_lost, planet_corrupted, enemy_approaching,
    planet_revealed, capture_bonus, population_cap, credit_cap, mission_added,
    construction_options, starbase_upgraded, enemy_starbase_upgraded,
    ally_starbase_upgraded, blockade, space_battle_pending, land_battle_pending,
};
[[nodiscard]] std::string_view galactic_sound_tag(GalacticSound cue) noexcept;
[[nodiscard]] bool galactic_sound_uses_audio_table(GalacticSound cue) noexcept;
[[nodiscard]] std::string galactic_sound_event(GalacticSound cue,
    std::span<const Field> faction, std::span<const Field> audio);
[[nodiscard]] std::optional<GalacticSound> galactic_owner_sound(std::uint32_t local_player,
    std::uint32_t previous_owner, std::uint32_t new_owner, bool new_owner_playable) noexcept;

// WGP-26: copied from the conflict owner before the publication can outlive it.
struct GalacticPendingAlert {
    std::uint32_t invader{}, defender{};
    bool land{};
};
[[nodiscard]] std::optional<GalacticSound> galactic_event_sound(
    const sim::galactic::GalacticEvent&, std::uint32_t local_player,
    std::optional<std::uint32_t> previous_owner,
    std::optional<GalacticPendingAlert> pending, bool new_owner_playable,
    std::optional<std::uint32_t> construction_options_player = {}) noexcept;

// WGEP-96/97, WGP-63: strategic production feedback of one object type. An
// authored speech event wins; otherwise the type's strategic SFX names the cue.
enum class GalacticBuildCue { underway, completed, stopped };
struct GalacticBuildFeedback {
    std::string speech; // a SpeechEvent name
    std::string sfx;    // an SFXEvent name, used only without speech
};
[[nodiscard]] std::string_view galactic_build_speech_tag(GalacticBuildCue cue) noexcept;
[[nodiscard]] std::string_view galactic_build_sfx_tag(GalacticBuildCue cue) noexcept;
// `type` holds the type's effective fields. The recipient is the local builder only (#3075 observer
// rule); an empty result is silent.
[[nodiscard]] GalacticBuildFeedback galactic_build_feedback(GalacticBuildCue cue,
    std::span<const Field> type, bool local_builder);

// WGP-25: source-order opposing-faction entries, including an authored blank.
[[nodiscard]] std::string strategic_result_event(std::span<const Field> fields,
    bool won, std::string_view opposing_faction);

// Presentation modes are not simulation/replay numbers.
class GalacticMusicDirector final {
public:
    enum class Mode { none, strategic_ambient, strategic_win, strategic_lose,
        tactical, superweapon_attack, demo_finale, scripted, battle_pending, battle_load, battle_summary };
    struct Cue {
        const MusicEvent* event{};
        std::string file;
        Mode mode{Mode::none};
    };
    // WGP-24: forced bypasses the mode/demo guard, never the strategic sub-mode.
    [[nodiscard]] std::optional<Cue> ambient(const MusicEvent* event,
        bool strategic_submode, bool demo_attract = false, bool forced = false);
    // WGP-25: caller supplies exact player-ID equality, not alliance membership.
    [[nodiscard]] std::optional<Cue> outcome(bool exact_local_winner,
        bool participants_and_local_present, const MusicEvent* win, const MusicEvent* lose);
    [[nodiscard]] std::optional<Cue> start(Mode mode, const MusicEvent* event);
    struct Loading {
        bool stop_music{};
        std::optional<Cue> cue;
    };
    // WGP-46: no authored event leaves existing music untouched. Low-memory
    // activation stops music; tutorial activation clears even pending music.
    [[nodiscard]] Loading battle_load(const MusicEvent* event,
        bool low_memory = false, bool tutorial = false);
    [[nodiscard]] std::optional<Cue> track_ended();
    void external_mode(Mode mode) noexcept { mode_ = mode; current_ = nullptr; }
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
private:
    Mode mode_{Mode::none};
    const MusicEvent* current_{};
    std::map<const MusicEvent*, std::size_t> next_file_;
};

} // namespace eawr::presentation::audio
