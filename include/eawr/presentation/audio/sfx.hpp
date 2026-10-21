#pragma once

#include "eawr/core/result.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
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
    int min_pan{50};
    int max_pan{50};
    int min_predelay_ms{};
    int max_predelay_ms{};
    int min_postdelay_ms{};
    int max_postdelay_ms{};
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

// A RIFF WAVE file as little-endian 16-bit frames. The decoder behind the original game's audio
// layer chooses by file content, not by extension (#1708), so a mod's sample may be 16-bit PCM,
// 8-bit PCM or IMA ADPCM; every one is handed on as 16-bit frames.
struct WavPcm final {
    std::uint32_t sample_rate{};
    std::uint16_t channels{};
    // Borrowed from the file bytes for 16-bit PCM, else it views `converted`.
    std::span<const std::byte> data;
    std::vector<std::byte> converted;  // the frames of an 8-bit or ADPCM file

    WavPcm() = default;
    WavPcm(const std::uint32_t rate, const std::uint16_t channel_count) noexcept
        : sample_rate(rate), channels(channel_count) {}
    // Moving a vector keeps its buffer, so `data` stays valid; a copy would not.
    WavPcm(WavPcm&&) noexcept = default;
    WavPcm& operator=(WavPcm&&) noexcept = default;
    WavPcm(const WavPcm&) = delete;
    WavPcm& operator=(const WavPcm&) = delete;
};
// Parses `bytes`; nullopt with `error` set when the file is not a WAVE this decoder reads: 8-bit
// or 16-bit PCM (also inside WAVE_FORMAT_EXTENSIBLE) or 4-bit IMA ADPCM, mono or stereo.
[[nodiscard]] std::optional<WavPcm> parse_wav(std::span<const std::byte> bytes, std::string& error);

// --- Sample names -------------------------------------------------------------------------------

// The original opens the authored SFX name (BA-30, ML-04/05/10). Data-rooted
// names may find loose files or full-path archive entries; bare names do not
// search loose Data/Audio/SFX files. Candidates are logical VFS index paths:
// rooted path alone, otherwise the conventional SFX archive prefix then the
// authored name. open_sfx_sample qualifies each candidate's original name and
// origin so an archive mount prefix never changes the requested name.
[[nodiscard]] std::vector<std::string> sfx_sample_candidates(std::string_view name);
[[nodiscard]] eawr::core::Result<std::vector<std::byte>> open_sfx_sample(const eawr::vfs::Vfs& filesystem, std::string_view name);
// Music and speech streams are different: the original adds a fixed directory to the event's
// file name (`Data\Audio\Music\` and `Data\Audio\Speech\<language>\`) without looking at the
// name. `language_directory` is the language's folder name (`English`).
[[nodiscard]] std::string music_sample_path(std::string_view file);
[[nodiscard]] std::string speech_sample_path(std::string_view language_directory, std::string_view file);

// Shared by battle and galactic streams. A malformed RIFF stays a WAVE failure;
// it is never retried as MP3. The backend owns MP3 decoding and stream validity.
template<class WaveDecoder, class Mp3Decoder>
[[nodiscard]] auto decode_stream(std::span<const std::byte> bytes, std::string& error,
                                 WaveDecoder wave, Mp3Decoder mp3) -> decltype(mp3(bytes)) {
    if (bytes.size() >= 4 && bytes[0] == std::byte{'R'} && bytes[1] == std::byte{'I'}
        && bytes[2] == std::byte{'F'} && bytes[3] == std::byte{'F'}) {
        if (const auto pcm = parse_wav(bytes, error)) return wave(*pcm);
        return {};
    }
    return mp3(bytes);
}

// SFX failures must not poison a speech/music stream for the same authored path.
enum class GalacticStreamKind { speech, music, sfx };
[[nodiscard]] inline std::string galactic_stream_key(GalacticStreamKind kind, std::string_view path) {
    return kind == GalacticStreamKind::sfx ? "sfx:" + std::string(path) : std::string(path);
}

struct SampleDiagnostics final {
    std::size_t sfx_requested{}, sfx_resolved{}, files_missing_total{}, files_undecodable_total{};
    std::map<std::string, std::size_t> files_missing, files_undecodable;
};

// Samples contains SFX only; failures may also contain speech and music.
template<class Samples, class Failures, class Resolved>
[[nodiscard]] SampleDiagnostics sample_diagnostics(const Samples& samples, const Failures& failures, Resolved resolved) {
    SampleDiagnostics result;
    result.sfx_requested = samples.size();
    for (const auto& [name, sample] : samples) {
        static_cast<void>(name);
        if (resolved(sample)) ++result.sfx_resolved;
    }
    for (const auto& [name, cause] : failures) {
        static_cast<void>(name);
        constexpr std::string_view prefix = "undecodable:";
        if (cause.starts_with(prefix)) {
            auto detail = std::string_view(cause).substr(prefix.size());
            if (detail.starts_with(' ')) detail.remove_prefix(1);
            ++result.files_undecodable[std::string(detail)];
            ++result.files_undecodable_total;
        } else {
            auto detail = std::string_view(cause);
            if (detail.starts_with("missing:")) {
                detail.remove_prefix(std::string_view("missing:").size());
                if (detail.starts_with(' ')) detail.remove_prefix(1);
            }
            // Keep the stream category and VFS code, not the per-file path,
            // source or message. Different files with the same cause aggregate.
            if (const auto code = detail.find("EAWR-"); code != std::string_view::npos) {
                const auto end = detail.find(' ', code);
                detail = detail.substr(0, end);
            }
            ++result.files_missing[std::string(detail)];
            ++result.files_missing_total;
        }
    }
    return result;
}

// --- 3D -----------------------------------------------------------------------------------------

using Vec3 = std::array<double, 3>;

// audio.xml's space factors and the engine's fixed maximum distance (BA-10, BA-11).
struct Space3D final {
    double saturation_factor{};  // Audio_Space_3D_Saturation_Distance_Mod
    double rolloff_factor{};     // Audio_Space_3D_Rolloff_Distance_Mod
    double listener_z{};        // Audio_Space_3D_Listener_Z_Pullback_Dist
    double max_distance{15000.0};
};
// BA-10: authored settings have no sourced missing-tag defaults. Invalid data
// reports EAWR-AUDIO-0301; the viewer keeps playback in its 2D path.
[[nodiscard]] std::optional<Space3D> space_3d(std::span<const Field> fields, std::vector<std::string>& problems);
// SND-46: absent tags use the sourced 5/10-second defaults silently; invalid ranges diagnose.
[[nodiscard]] std::optional<std::array<int, 2>> ambient_moving_delays(std::string_view minimum,
    std::string_view maximum, std::string_view context, std::vector<std::string>& problems);

// FoC's volume sliders (master, music, speech, SFX) start at 0.75, and a channel plays at its
// slider times the master's (BA-45).
inline constexpr double default_volume_slider = 0.75;

// SND-60: Localize chooses the gain category independently of spatial mode or VO flags.
struct MixLevels final {
    // ST-A1: quieter fresh-profile output; the retail slider-times-channel model stays BA-45.
    double master{0.37};
    double sfx{default_volume_slider};
    double speech{default_volume_slider};
    double music{default_volume_slider};
};
[[nodiscard]] double category_gain(const SfxEvent& event, const MixLevels& levels) noexcept;
// SND-13: ordinary 2D one-shots continue through game pause.
[[nodiscard]] bool pauses_with_game(const SfxEvent& event, bool spatial) noexcept;

// SND-16/17: caller-supplied mode facts; default skirmish enables every category.
struct Admission final {
    bool localized{true};
    bool unit_response{true};
    bool hud{true};
    bool ambient{true};
    bool speech_stream{};
    bool cinematic{};
    bool story_cinematic{};
    bool tutorial_tactical{};
    bool demo_dialog{};
    const SfxEvent* negative_feedback{};
};

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
        category_disabled,
        cinematic_feedback,
        hud_speech,
        tutorial_hud,
        demo_dialog,
        delete_pending,
        zero_gain,
        attached_loop,
    };
    Result result{Result::playing};
    std::size_t voice{};                         // valid when playing
    std::optional<std::size_t> stopped;          // a voice stopped to make room
    std::string sample;                          // the sample name chosen
    double volume{1.0};                          // Min_Volume..Max_Volume / 100
    double pitch{1.0};                           // Min_Pitch..Max_Pitch / 100
    double pan{0.5};                             // Min_Pan2D..Max_Pan2D / 100
    bool admitted{};                            // event gates passed, even if allocation fails
};
[[nodiscard]] std::string_view to_string(Start::Result result) noexcept;

// FoC's SFX voice rules (BA-04 to BA-09): 32 3D voices and a 2D pool. The viewer asks start(),
// plays the sample on the voice it names (stopping the one `stopped` names first) and reports a
// voice whose sample ended with finished().
class Voices final {
public:
    static constexpr std::size_t voices_3d = 32;
    static constexpr std::size_t voices_2d = 16;  // SND-07

    struct Request final {
        const SfxEvent* event{};
        std::optional<Vec3> position;  // 3D events: where it plays
        bool hidden{};                 // the object it belongs to is fogged for the local player
        bool forced{};
        bool delete_pending{};
        std::optional<double> started_at{}; // backend clock; omitted callers use admission sequence
    };
    Start start(const Request& request, const Vec3& listener, Random& random, const Admission& admission = {});
    // SND-18: allocation after event admission, with values already drawn for this loop.
    Start allocate(const Request& request, const Vec3& listener, Start sample);
    void finished(std::size_t voice);
    // SP-03: attached sounds move with their source; BA-05/BA-09 cull at that current position.
    void set_position(std::size_t voice, const Vec3& position);
    void set_fading(std::size_t voice);
    // The voice's event, while it plays.
    [[nodiscard]] const SfxEvent* playing(std::size_t voice) const;
    [[nodiscard]] std::size_t playing_count() const;
    // 3D voices are 0..31, 2D voices 32..47.
    [[nodiscard]] static constexpr bool is_3d_voice(std::size_t voice) noexcept { return voice < voices_3d; }

private:
    struct Voice final {
        const SfxEvent* event{};
        Vec3 position{};
        double started_at{};
        bool fading{};
    };
    std::array<Voice, voices_3d + voices_2d> voices_{};
    std::map<const SfxEvent*, std::size_t> sequential_;
    std::uint64_t starts_{};
};

// SND-03/05/08/09/18: presentation events outlive their current backend sample.
// A service call makes at most one stage transition per retained event.
class EventQueue final {
public:
    EventQueue() = default;
    EventQueue(const EventQueue&) = delete;
    EventQueue& operator=(const EventQueue&) = delete;
    using Handle = std::uint64_t;
    enum class Stage : std::uint8_t { initialize, predelay, pre, main, post, postdelay, complete, done };
    struct Request final {
        Voices::Request voice;
        std::uint64_t attachment{};
        bool continuous_main{}; // construction ownership, independent of authored finite loops
    };
    struct Accepted final {
        Start::Result result{Start::Result::playing};
        std::optional<Handle> handle;
    };
    struct Sample final {
        Handle handle{};
        Request request;
        Start values;
        Stage stage{Stage::main};
        bool continuous{};
    };
    struct Chain final {
        const SfxEvent* event{};
        std::uint64_t attachment{};
        bool runtime{};
        bool attack{};
    };
    struct Backend final {
        std::function<Start(const Sample&)> start;
        std::function<bool(std::size_t)> playing;
        std::function<void(std::size_t)> stop;
        std::function<void(std::size_t)> fading;
    };
    Accepted admit(const Request& request, Random& random, const Admission& admission = {});
    std::vector<Chain> service(double elapsed_ms, bool paused, const Vec3& listener, Random& random,
                              const SfxRegistry& registry, const Backend& backend);
    void stop(Handle handle, const Backend& backend, double fade_seconds = 0.0);
    void detach(std::uint64_t source, const Backend& backend);
    void set_position(Handle handle, const Vec3& position);
    void chain(Handle handle, const SfxEvent* event, bool attack, std::uint64_t source);
    void cancel_missing_chains(const std::function<bool(std::uint64_t)>& alive);
    [[nodiscard]] bool active(Handle handle) const;
    [[nodiscard]] double fade(Handle handle) const;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t instances(const SfxEvent* event) const;
    [[nodiscard]] std::uint64_t completed_loops() const noexcept { return completed_loops_; }
private:
    struct Entry final {
        Handle handle{};
        Request request;
        Stage stage{Stage::initialize};
        std::optional<std::size_t> voice;
        Start values;
        double delay_ms{};
        double postdelay_ms{};
        int completed{};
        bool fading{};
        double fade_ms{};
        double fade_remaining_ms{};
        const SfxEvent* chained{};
        bool attack{};
        std::uint64_t chain_source{};
    };
    struct Cursors final { std::size_t pre{}, main{}, post{}; };
    Entry* find(Handle handle);
    std::list<Entry> entries_; // 2D head, 3D tail; spatial service orders by distance
    std::map<Handle, Entry*> handles_;
    std::map<const SfxEvent*, Cursors> cursors_;
    Handle next_{1};
    std::uint64_t completed_loops_{};
};

// SND-11: ordinary 2D/fixed-position playback does not use this multiplier.
[[nodiscard]] double attached_gain(bool silent, bool model_hidden, bool fogged, bool story_cinematic) noexcept;

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
    bool internal_loop{}; // WGP-26: stream loops even while the music service is starved.
};
[[nodiscard]] MusicEvent parse_music_event(std::string_view name, std::span<const Field> fields);

// SND-54: first opposing-faction override, including an authored blank, else default.
[[nodiscard]] std::string tactical_music_event(std::span<const Field> fields, bool won,
                                              std::string_view opposing_faction);

// SND-72: normalized envelope; ending streams fade from their retained level.
struct MusicFade final {
    double level{};
    double slope{};
    bool ending{};
    void begin(double seconds);
    void retire(double seconds);
    void advance(double seconds);
};

// The battle's music mode (BA-41 to BA-43): ambient until a weapon fires with a local player's unit
// as firer or target, battle from then until `peace_ticks` pass without one, then ambient again.
// Each event plays its files in order, from where it last stopped, looping.
class MusicDirector final {
public:
    enum class Mode : std::uint8_t { none, ambient, battle, victory, defeat };
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
    std::optional<Cue> result(bool exact_local_winner, const MusicEvent* event);
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
