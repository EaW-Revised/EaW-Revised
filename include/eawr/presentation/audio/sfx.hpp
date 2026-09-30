#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// P2-21 (#84, docs/behaviour/battle-audio.md): FoC's sound events, as the viewer plays them from
// the live battle's presentation events. Engine-free: the registry, the voice rules, the 3D
// falloff, the listener and the music choice live here; the viewer only owns the players. Audio is
// presentation only and never reaches the simulation.
namespace eawr::presentation::audio {

// One <SFXEvent> of the SFXEvents*.xml files after its Use_Preset (BA-01 to BA-03).
struct SfxEvent final {
    std::string name;
    bool preset{};
    bool is_3d{true};
    bool gui{};
    bool hud_vo{};
    bool unit_response_vo{};
    bool ambient_vo{};
    bool localized{};
    bool play_sequentially{};
    std::vector<std::string> samples;
    std::vector<std::string> pre_samples;
    std::vector<std::string> post_samples;
    int priority{3};       // 1 (most important) .. 5
    int probability{100};  // percent
    int play_count{1};     // -1 loops
    int max_instances{1};
    int min_volume{100};
    int max_volume{100};
    int min_pitch{100};
    int max_pitch{100};
    int min_predelay_ms{};
    int max_predelay_ms{};
    double saturation_distance{300.0};  // Volume_Saturation_Distance
    double loop_fade_in_seconds{};
    double loop_fade_out_seconds{};
    bool kills_previous_object_sfx{};
    std::string overlap_test;
    std::string chained;
};

// One child element of an <SFXEvent>: its tag and trimmed text, in document order.
using Field = std::pair<std::string, std::string>;

// The SFXEvent registry (BA-01): events in SFXEventFiles order; a Use_Preset names an earlier
// preset. Names compare case-insensitively.
class SfxRegistry final {
public:
    // Adds one event. Problems (an unknown preset, a malformed value) go to `problems`.
    void add(std::string_view name, std::span<const Field> fields, std::vector<std::string>& problems);
    [[nodiscard]] const SfxEvent* find(std::string_view name) const;
    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }

private:
    std::map<std::string, SfxEvent, std::less<>> events_;  // key: upper-case name
};

[[nodiscard]] std::string upper(std::string_view text);
// FoC's integer fields read the leading integer ("80, 20" is 80), BA-02.
[[nodiscard]] std::optional<int> leading_integer(std::string_view text);
// A sample list: names separated by blanks or commas.
[[nodiscard]] std::vector<std::string> split_list(std::string_view text);

// --- Samples ------------------------------------------------------------------------------------

// A RIFF WAVE file's 16-bit PCM payload (every FoC SFX sample; BA-30).
struct WavPcm final {
    std::uint32_t sample_rate{};
    std::uint16_t channels{};
    std::span<const std::byte> data;  // little-endian 16-bit frames, borrowed from the file bytes
};
// Parses `bytes`; nullopt with `error` set when it is not 16-bit PCM WAVE.
[[nodiscard]] std::optional<WavPcm> parse_wav(std::span<const std::byte> bytes, std::string& error);

// --- 3D -----------------------------------------------------------------------------------------

using Vec3 = std::array<double, 3>;

// audio.xml's space factors and the engine's fixed maximum distance (BA-10, BA-11).
struct Space3D final {
    double saturation_factor{1.5};  // Audio_Space_3D_Saturation_Distance_Mod
    double rolloff_factor{2.0};     // Audio_Space_3D_Rolloff_Distance_Mod
    double listener_z{60.0};        // Audio_Space_3D_Listener_Z_Pullback_Dist
    double max_distance{15000.0};
};

// FoC's volume sliders (master, music, speech, SFX) start at 0.75, and a channel plays at its
// slider times the master's (BA-45).
inline constexpr double default_volume_slider = 0.75;

// The gain of a 3D sample `distance` from the listener (BA-11): full inside the minimum distance
// (Volume_Saturation_Distance times the mode factor, at least 1), then Miles' inverse-distance
// rolloff min / (min + rolloff * (d - min)), silent past the maximum distance.
[[nodiscard]] double falloff_gain(double distance, double saturation_distance, const Space3D& space);

// Where the listener stands for a camera at `camera` looking along `forward` (BA-12): where the
// view ray meets the plane z = listener_z, pulled back along the ray to |camera.z - listener_z|
// from the camera when that point is farther. `ahead` is the listener's facing: the camera's
// forward flattened onto the plane (the camera's own forward when it looks straight down).
struct Listener final {
    Vec3 position{};
    Vec3 ahead{1.0, 0.0, 0.0};
};
[[nodiscard]] Listener listener_for_camera(const Vec3& camera, const Vec3& forward, const Space3D& space);

// --- Voices -------------------------------------------------------------------------------------

// A small deterministic generator for FoC's free (unsynchronised) random draws.
class Random final {
public:
    explicit Random(std::uint64_t seed = 0x9e3779b97f4a7c15ULL) : state_(seed ? seed : 1U) {}
    // Uniform in [low, high] (inclusive); low when high < low.
    [[nodiscard]] int between(int low, int high);

private:
    std::uint64_t state_;
};

// What a start request resolved to.
struct Start final {
    enum class Result : std::uint8_t {
        playing,          // took a voice (maybe a stolen one)
        preset,           // a preset or Play_Count 0 never plays
        no_instances,     // Max_Instances 0
        instance_limit,   // 2D at Max_Instances, or 3D at Max_Instances and farther than the farthest
        overlap,          // an event with the same Overlap_Test is playing
        probability,      // the Probability roll failed
        hidden,           // the object is fogged for the local player
        no_samples,       // the event names no sample
        no_voice,         // every voice busy with more important or nearer sounds
    };
    Result result{Result::playing};
    std::size_t voice{};                         // valid when playing
    std::optional<std::size_t> stopped;          // a voice stopped to make room
    std::string sample;                          // the sample name chosen
    double volume{1.0};                          // Min_Volume..Max_Volume / 100
    double pitch{1.0};                           // Min_Pitch..Max_Pitch / 100
};
[[nodiscard]] std::string_view to_string(Start::Result result) noexcept;

// FoC's SFX voice rules (BA-04 to BA-09): 32 3D voices and a 2D pool. The viewer asks start(),
// plays the sample on the voice it names (stopping the one `stopped` names first) and reports a
// voice whose sample ended with finished().
class Voices final {
public:
    static constexpr std::size_t voices_3d = 32;
    static constexpr std::size_t voices_2d = 16;  // BA-09: the 2D pool size is not modelled

    struct Request final {
        const SfxEvent* event{};
        std::optional<Vec3> position;  // 3D events: where it plays
        bool hidden{};                 // the object it belongs to is fogged for the local player
    };
    Start start(const Request& request, const Vec3& listener, Random& random);
    void finished(std::size_t voice);
    // The voice's event, while it plays.
    [[nodiscard]] const SfxEvent* playing(std::size_t voice) const;
    [[nodiscard]] std::size_t playing_count() const;
    // 3D voices are 0..31, 2D voices 32..47.
    [[nodiscard]] static constexpr bool is_3d_voice(std::size_t voice) noexcept { return voice < voices_3d; }

private:
    struct Voice final {
        const SfxEvent* event{};
        Vec3 position{};
    };
    std::array<Voice, voices_3d + voices_2d> voices_{};
    std::map<const SfxEvent*, std::size_t> sequential_;
};

// --- Unit responses -----------------------------------------------------------------------------

// A selected unit as the response rules rank it (BA-20).
struct RankedUnit final {
    std::size_t index{};                 // the caller's index
    std::vector<std::string> categories; // CategoryMask names
    std::optional<int> ranking;          // Ranking_In_Category (25 when absent)
};
// FoC's speaker for a selection: the unit whose first category in `rankings`
// (GameConstants Unit_Command_Rankings_By_Category) comes first, with FoC's tie rule that a lower
// Ranking_In_Category also takes over (BA-20). nullopt for an empty list or no ranked category.
[[nodiscard]] std::optional<std::size_t> speaker(std::span<const RankedUnit> units,
                                                 std::span<const std::string> rankings);

// --- Music --------------------------------------------------------------------------------------

// One <MusicEvent> (BA-40).
struct MusicEvent final {
    std::string name;
    std::vector<std::string> files;
    double volume{1.0};
    double fade_in_seconds{};
    double fade_out_previous_seconds{};
    bool loop{};
};
[[nodiscard]] MusicEvent parse_music_event(std::string_view name, std::span<const Field> fields);

// The battle's music mode (BA-41 to BA-43): ambient until a weapon fires with a local player's unit
// as firer or target, battle from then until `peace_ticks` pass without one, then ambient again.
// Each event plays its files in order, from where it last stopped, looping.
class MusicDirector final {
public:
    enum class Mode : std::uint8_t { none, ambient, battle };
    struct Cue final {
        const MusicEvent* event{};
        std::string file;
        Mode mode{Mode::none};
    };
    MusicDirector(std::vector<const MusicEvent*> ambient, std::vector<const MusicEvent*> battle,
                  std::uint64_t peace_ticks);
    // The battle starts: ambient music (a cue).
    std::optional<Cue> begin(Random& random);
    // A tick passed; `attack` when a weapon fired that involves the local player that tick.
    std::optional<Cue> tick(bool attack, Random& random);
    // The playing track ended: the event's next file, when it loops.
    std::optional<Cue> track_ended();
    [[nodiscard]] Mode mode() const noexcept { return mode_; }

private:
    std::optional<Cue> start(Mode mode, Random& random);
    std::vector<const MusicEvent*> ambient_;
    std::vector<const MusicEvent*> battle_;
    std::uint64_t peace_ticks_{};
    std::uint64_t quiet_{};
    Mode mode_{Mode::none};
    const MusicEvent* current_{};
    std::map<const MusicEvent*, std::size_t> next_file_;
};

} // namespace eawr::presentation::audio
