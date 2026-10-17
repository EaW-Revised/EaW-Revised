#pragma once

#include "eawr/data/xml.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "eawr/presentation/audio/announcements.hpp"
#include "eawr/presentation/audio/galactic.hpp"
#include "eawr/presentation/renderer.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/audio_effect_panner.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/audio_listener3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <map>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// The same engine-free audio loaders/voice queue/fades as tactical audio.
// Callers deliver session notifications; this object never inspects a session.
class GalacticAudio final {
public:
    GalacticAudio(godot::Node3D& host, const vfs::Vfs& filesystem,
        const data::Catalog& catalog, std::string_view local_faction, bool muted);
    ~GalacticAudio();
    void ambient(bool forced = false);
    void battle_pending(bool land);
    void consume(const platform::LiveGalacticEvent&, std::uint32_t local_player);
    void battle_load();
    void tactical();
    void battle_summary(std::uint64_t conflict, bool visible, bool won);
    void sound(audio::GalacticSound cue, std::optional<audio::Vec3> position = std::nullopt);
    // WGEP-96/97: one type's production feedback; `type` is its object type name.
    void build(audio::GalacticBuildCue cue, std::string_view type, bool local_builder);
    // An authored GUI SFX event by name (command-bar Click_SFX / Mouse_Over_SFX).
    void ui_sound(std::string_view name) { play_event(name); }
    void advisor_urgent();
    // WGP-182: presenter seams; the caller owns rebuild/reveal admission,
    // including GFC-40 tutorial/cinematic suppression. Live publication: #2042.
    void objectives_rebuilt(const data::ui::CommandBarComponent& icon, bool announce) {
        if (announce) ui_sound(icon.text(data::ui::Field::click_sfx));
    }
    void teletype_revealed(const data::ui::CommandBarComponent& text) {
        ui_sound(text.text(data::ui::Field::mouse_over_sfx));
    }
    // WGP-181: an accepted local fleet move; member_types are the fleet's member type names.
    void fleet_move(std::span<const std::string> member_types);
    void outcome(bool exact_local_winner, bool participants_present,
        std::string_view winner_faction, std::string_view loser_faction);
    void service(double delta, const FixedCamera& camera, bool paused);
    void write_report(std::ostream& output) const;
private:
    const audio::MusicEvent* music(std::string_view name) const;
    void play(const std::optional<audio::GalacticMusicDirector::Cue>& cue);
    godot::Ref<godot::AudioStream> stream(std::string_view file, bool music_file);
    void play_event(std::string_view name);
    const std::vector<audio::Field>& type_fields(std::string_view type);
    void update_speech(bool paused);
    const vfs::Vfs& filesystem_;
    const data::Catalog& catalog_;
    // SND-44/48: the speech registry, FIFO and its one stream, as in tactical audio.
    struct SpeechEvent {
        std::string name;
        std::vector<std::string> files;
        double volume{1.0};
    };
    std::map<std::string, SpeechEvent> speech_events_;
    audio::SpeechQueue<SpeechEvent> speech_queue_;
    audio::SpeechStream speech_stream_;
    godot::AudioStreamPlayer* speech_player_{};
    audio::SpeechStream::Handle speech_handle_{};
    bool speech_started_{};
    double speech_started_at_{};
    std::map<std::string, std::vector<audio::Field>> type_fields_;
    std::vector<std::string> speech_log_;
    audio::SfxRegistry registry_;
    std::vector<audio::MusicEvent> music_events_;
    std::vector<audio::Field> faction_fields_, audio_fields_;
    std::vector<std::string> rankings_; // GameConstants Unit_Command_Rankings_By_Category
    audio::GalacticMusicDirector director_;
    audio::EventQueue events_;
    audio::Voices voices_;
    audio::Random random_;
    struct Voice {
        godot::AudioStreamPlayer* player{};
        double started_at{};
        audio::EventQueue::Handle handle{};
        double gain{};
    };
    std::array<Voice, audio::Voices::voices_2d> players_;
    std::array<godot::Ref<godot::AudioEffectPanner>, audio::Voices::voices_2d> panners_;
    struct SpatialVoice {
        godot::AudioStreamPlayer3D* player{};
        double started_at{};
        audio::EventQueue::Handle handle{};
        double gain{}, saturation{};
        audio::Vec3 position{};
    };
    std::array<SpatialVoice, audio::Voices::voices_3d> spatial_players_;
    std::optional<audio::Space3D> space_;
    audio::Vec3 listener_position_{};
    godot::SubViewport* listener_viewport_{};
    godot::Camera3D* listener_camera_{};
    godot::AudioListener3D* listener_{};
    struct Track {
        godot::AudioStreamPlayer* player{};
        audio::MusicFade fade;
        double volume{};
        bool active{};
    };
    std::array<Track, 2> tracks_;
    std::size_t current_{};
    double clock_{};
    bool muted_{};
    bool strategic_{true};
    std::uint64_t summary_conflict_{};
    std::map<std::string, godot::Ref<godot::AudioStream>> streams_;
    std::vector<std::string> problems_, music_log_;
    std::map<std::string, std::uint64_t> sounds_;
    std::map<std::string, std::uint64_t> played_sounds_;
    std::map<std::string, std::uint64_t> playback_failures_;
};
} // namespace eawr::presentation::godot_backend
