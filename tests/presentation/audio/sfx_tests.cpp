// P2-21 (#84): FoC's sound-event rules (presentation::audio, docs/behaviour/battle-audio.md).
// Synthetic inputs only; no game data.
#include "eawr/presentation/audio/sfx.hpp"
#include "eawr/presentation/audio/announcements.hpp"
#include "eawr/presentation/audio/command_cues.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace audio = eawr::presentation::audio;
using audio::Field;
using Result = audio::Start::Result;

int failures{};

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] bool near(const double a, const double b, const double tolerance = 1.0e-9) {
    return std::abs(a - b) <= tolerance;
}

void registry_and_presets() {
    audio::SfxRegistry registry;
    std::vector<std::string> problems;
    const std::vector<Field> preset{{"Is_Preset", " Yes "}, {"Is_3D", "Yes"}, {"Is_2D", "No"}, {"Priority", "4"},
                                    {"Max_Instances", "4"}, {"Volume_Saturation_Distance", "300.0"},
                                    {"Min_Volume", "55"}, {"Max_Volume", "55"}, {"Min_Pitch", "95"}, {"Max_Pitch", "105"}};
    registry.add("Preset_GUNV", preset, problems);
    // BA-02: the preset first, then the event's own fields; "80, 20" reads as 80.
    const std::vector<Field> fire{{"Use_Preset", "Preset_GUNV"}, {"Samples", "GUN_A.wav GUN_B.wav"},
                                  {"Probability", "80, 20"}};
    registry.add("Unit_Fire", fire, problems);
    // A field before Use_Preset is overwritten by the preset (document order).
    const std::vector<Field> early{{"Max_Instances", "9"}, {"Use_Preset", "preset_gunv"}, {"Samples", "X.wav, Y.wav"}};
    registry.add("Early", early, problems);
    const std::vector<Field> bad{{"Use_Preset", "Nope"}, {"Priority", "9"}, {"Min_Pitch", "10"}, {"Samples", "TBD"}};
    registry.add("Bad", bad, problems);
    expect(problems.size() == 1 && problems[0].find("Nope") != std::string::npos, "unknown preset reported");
    const audio::SfxEvent* event = registry.find("unit_fire");
    expect(event != nullptr, "case-insensitive find");
    if (event != nullptr) {
        expect(!event->preset && event->is_3d && event->priority == 4 && event->max_instances == 4, "preset copied");
        expect(event->name == "Unit_Fire", "own name kept");
        expect(event->probability == 80, "leading integer probability");
        expect(event->samples == std::vector<std::string>{"GUN_A.wav", "GUN_B.wav"}, "samples split");
    }
    const audio::SfxEvent* first = registry.find("Early");
    expect(first != nullptr && first->max_instances == 4 && first->samples.size() == 2, "Use_Preset overwrites earlier fields");
    const audio::SfxEvent* clamped = registry.find("Bad");
    expect(clamped != nullptr && clamped->priority == 5 && clamped->min_pitch == 50 && clamped->samples.empty(),
           "priority and pitch clamped, TBD skipped");
    // Defaults (BA-01): a bare event is 3D, priority 3, one instance, saturation 300.
    registry.add("Bare", std::vector<Field>{}, problems);
    const audio::SfxEvent* bare = registry.find("Bare");
    expect(bare != nullptr && bare->is_3d && bare->priority == 3 && bare->max_instances == 1
               && near(bare->saturation_distance, 300.0) && bare->probability == 100,
           "defaults");
    expect(audio::leading_integer(" 12abc") == 12 && !audio::leading_integer("x"), "leading integer");
}

std::vector<std::byte> wav(const std::uint16_t format, const std::uint16_t bits, const std::uint16_t channels,
                           const std::size_t frames) {
    std::vector<std::byte> bytes;
    const auto put = [&bytes](const char* text) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::byte>(text[i]));
    };
    const auto u32 = [&bytes](const std::uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xffU));
    };
    const auto u16 = [&bytes](const std::uint16_t value) {
        bytes.push_back(static_cast<std::byte>(value & 0xffU));
        bytes.push_back(static_cast<std::byte>(value >> 8U));
    };
    const std::uint32_t data = static_cast<std::uint32_t>(frames * channels * 2U);
    put("RIFF");
    u32(4 + 26 + 8 + 4 + 8 + data);
    put("WAVE");
    put("fmt ");
    u32(18);
    u16(format);
    u16(channels);
    u32(22050);
    u32(22050U * channels * 2U);
    u16(static_cast<std::uint16_t>(channels * 2U));
    u16(bits);
    u16(0);  // cbSize
    put("LIST");  // a chunk to skip
    u32(4);
    put("INFO");
    put("data");
    u32(data);
    for (std::size_t i = 0; i < data; ++i) bytes.push_back(static_cast<std::byte>(i & 0xffU));
    return bytes;
}

audio::SfxEvent event(const std::string& name, bool three_d, int instances, int priority = 3);

struct LifecycleRig final {
    audio::Random random{73};
    audio::SfxRegistry registry;
    audio::Voices voices;
    audio::EventQueue queue;
    std::array<bool, audio::Voices::voices_3d + audio::Voices::voices_2d> playing{};
    std::vector<audio::EventQueue::Sample> starts;
    std::vector<audio::EventQueue::Chain> chains;
    bool missing{};
    std::uint64_t stops{};
    audio::EventQueue::Backend backend() {
        return {
            [this](const audio::EventQueue::Sample& cue) {
                starts.push_back(cue);
                auto start = voices.allocate(cue.request.voice, {}, cue.values);
                if (start.result == Result::playing) {
                    playing[start.voice] = !missing;
                    if (missing) {
                        voices.finished(start.voice);
                        start.result = Result::no_samples;
                    }
                }
                return start;
            },
            [this](const std::size_t voice) { return playing[voice]; },
            [this](const std::size_t voice) { playing[voice] = false; voices.finished(voice); ++stops; },
            [this](const std::size_t voice) { voices.set_fading(voice); }
        };
    }
    void step(const double ms = 0.0, const bool paused = false) {
        const auto released = queue.service(ms, paused, {}, random, registry, backend());
        chains.insert(chains.end(), released.begin(), released.end());
    }
    void pump(const int count) { for (int i = 0; i < count; ++i) step(); }
    void finish() { playing.fill(false); }
};

void event_lifecycle() {
    for (const bool spatial : {false, true}) {
        for (const bool pre_sample : {false, true}) {
            for (const int delay : {0, 100}) {
                LifecycleRig rig;
                auto cue = event("StartTiming", spatial, 1);
                if (pre_sample) cue.pre_samples = {"intro"};
                cue.min_predelay_ms = cue.max_predelay_ms = delay;
                const auto position = spatial ? std::optional<audio::Vec3>{audio::Vec3{}} : std::nullopt;
                const auto accepted = rig.queue.admit({{&cue, position}}, rig.random);
                rig.step();
                expect(accepted.handle && rig.starts.empty(), "loop initialization does not allocate a sample");
                if (delay) {
                    rig.step(99);
                    expect(rig.starts.empty(), "authored predelay remains pending before expiry");
                    rig.step(1);
                    expect(rig.starts.empty(), "predelay expiry selects the next sample for the following service");
                }
                rig.step();
                expect(rig.starts.size() == 1
                       && rig.starts.front().stage == (pre_sample ? audio::EventQueue::Stage::pre : audio::EventQueue::Stage::main),
                       "first sample starts on the next service without an empty pre-stage wait");
            }
        }
    }
    {
        LifecycleRig rig;
        auto response = event("Delayed", false, 1);
        response.min_predelay_ms = response.max_predelay_ms = 100;
        response.overlap_test = "BUSY";
        const auto accepted = rig.queue.admit({{&response, std::nullopt, false}, 7}, rig.random);
        expect(accepted.handle && rig.starts.empty() && rig.voices.playing_count() == 0, "admission precedes decoding/allocation");
        expect(rig.queue.admit({{&response, std::nullopt, false}}, rig.random).result == Result::instance_limit, "queued event counts at instance limit");
        auto overlapping = response;
        expect(rig.queue.admit({{&overlapping, std::nullopt, false}}, rig.random).result == Result::overlap, "delayed event counts for overlap");
        rig.step();
        rig.step(99);
        expect(rig.starts.empty(), "predelay uses elapsed milliseconds");
        rig.step(1);
        rig.step();
        expect(rig.starts.size() == 1, "sample starts only after predelay");
        rig.queue.detach(7, rig.backend());
        expect(!rig.queue.active(*accepted.handle) && rig.voices.playing_count() == 0, "detachment stops the owned sample");
        const auto queued = rig.queue.admit({{&response, std::nullopt, false}, 8}, rig.random);
        rig.queue.detach(8, rig.backend());
        rig.pump(8);
        expect(queued.handle && rig.starts.size() == 1 && rig.queue.size() == 0, "detachment removes a queued event without later playback");
    }
    {
        LifecycleRig rig;
        auto staged = event("Stages", false, 1);
        staged.play_sequentially = true;
        staged.play_count = 2;
        staged.samples = {"main0", "main1"};
        staged.pre_samples = {"pre0", "pre1"};
        staged.post_samples = {"post0", "post1"};
        staged.min_volume = staged.max_volume = 61;
        staged.min_pitch = staged.max_pitch = 123;
        staged.min_pan = staged.max_pan = 17;
        staged.min_postdelay_ms = staged.max_postdelay_ms = 50;
        const auto handle = rig.queue.admit({{&staged, std::nullopt, false}}, rig.random).handle;
        for (int loop = 0; loop < 2; ++loop) {
            rig.pump(2); // initialize, pre
            expect(rig.starts.back().values.sample == "pre" + std::to_string(loop), "pre cursor in turn");
            rig.finish(); rig.pump(2); // completion, main
            expect(rig.starts.back().values.sample == "main" + std::to_string(loop)
                   && !rig.starts.back().continuous, "finite main plays once per event loop");
            rig.finish(); rig.pump(2); // completion, post
            expect(rig.starts.back().values.sample == "post" + std::to_string(loop), "post cursor in turn");
            rig.finish(); rig.step();
            rig.step(49);
            expect(rig.queue.completed_loops() == static_cast<std::uint64_t>(loop), "postdelay precedes completed-loop count");
            rig.step(1); rig.step();
        }
        expect(handle && !rig.queue.active(*handle) && rig.queue.completed_loops() == 2 && rig.starts.size() == 6,
               "positive play count counts whole completed loops");
        for (const auto& start : rig.starts) {
            expect(near(start.values.volume, .61) && near(start.values.pitch, 1.23) && near(start.values.pan, .17),
                   "pre/main/post use the same loop gain pitch and pan");
        }
        // Unequal pre/main sizes keep separate cursors; the next admission resynchronizes equals.
        staged.pre_samples = {"short"};
        const auto next = rig.queue.admit({{&staged, std::nullopt, false}}, rig.random).handle;
        rig.pump(2); rig.finish(); rig.pump(2);
        expect(next && rig.starts.back().values.sample == "main0", "main cursor retained across admissions");
        rig.queue.stop(*next, rig.backend());
        staged.pre_samples = {"pre0", "pre1"};
        rig.queue.admit({{&staged, std::nullopt, false}}, rig.random);
        rig.pump(2);
        expect(rig.starts.back().values.sample == "pre1", "equal pre/main cursors synchronize at admission");
    }
    {
        LifecycleRig rig;
        auto loop = event("Loop", true, 8);
        loop.play_count = -1;
        loop.pre_samples = {"intro"};
        const auto accepted = rig.queue.admit({{&loop, audio::Vec3{}}, 5}, rig.random);
        expect(rig.queue.admit({{&loop, audio::Vec3{}}, 5}, rig.random).result == Result::attached_loop,
               "duplicate loop refused before sample allocation");
        rig.pump(2);
        expect(!rig.starts.back().continuous, "loop pre sample is finite");
        rig.finish(); rig.pump(2);
        expect(rig.starts.back().continuous && rig.queue.completed_loops() == 0, "infinite main is continuous, not repeated one-shot restarts");
        rig.queue.stop(*accepted.handle, rig.backend(), .2);
        expect(rig.queue.instances(&loop) == 0, "fading-to-silence excluded from event admission count");
        const auto replacement = rig.queue.admit({{&loop, audio::Vec3{}}, 5}, rig.random);
        expect(replacement.handle.has_value(), "fading loop no longer blocks same attachment");
        rig.step(100, true);
        expect(near(rig.queue.fade(*accepted.handle), 1.0), "spatial pause freezes event fade");
        rig.step(100);
        expect(near(rig.queue.fade(*accepted.handle), .5), "fade advances in elapsed time");
        rig.queue.detach(5, rig.backend()); rig.step();
        expect(rig.queue.size() == 0 && rig.voices.playing_count() == 0, "destruction stops fading and queued replacement");
    }
    {
        LifecycleRig rig;
        std::vector<std::string> problems;
        rig.registry.add("Authored", std::vector<audio::Field>{{"Is_3D", "No"}, {"Samples", "authored"}}, problems);
        const auto* authored = rig.registry.find("Authored");
        auto primary = event("Primary", false, 1);
        auto assist = event("Assist", false, 1);
        primary.chained = "Authored";
        primary.min_postdelay_ms = primary.max_postdelay_ms = 100;
        const auto handle = rig.queue.admit({{&primary, std::nullopt, false}}, rig.random).handle;
        rig.queue.chain(*handle, &assist, true, 99);
        rig.pump(3); rig.finish(); rig.pump(3);
        expect(rig.chains.empty(), "runtime chain waits for whole event, including postdelay");
        rig.step(100); rig.step();
        expect(rig.chains.size() == 1 && rig.chains[0].event == &assist && rig.chains[0].runtime && rig.chains[0].attack,
               "runtime 2D chain takes precedence over authored chain");
        assist.probability = 0;
        expect(rig.queue.admit({{rig.chains[0].event, std::nullopt, false}}, rig.random).result == Result::probability, "chains re-enter ordinary admission");
        rig.chains.clear();
        const auto again = rig.queue.admit({{&primary, std::nullopt, false}}, rig.random).handle;
        rig.queue.chain(*again, &assist, false, 99);
        rig.queue.cancel_missing_chains([](std::uint64_t) { return false; });
        rig.pump(3); rig.finish(); rig.pump(3); rig.step(100); rig.step();
        expect(rig.chains.size() == 1 && rig.chains[0].event == authored && !rig.chains[0].runtime,
               "removed runtime chain source leaves authored fallback");
        rig.chains.clear();
        const auto cancelled = rig.queue.admit({{&primary, std::nullopt, false}}, rig.random).handle;
        rig.queue.chain(*cancelled, &assist, false, 99);
        rig.queue.stop(*cancelled, rig.backend()); rig.pump(20);
        expect(rig.chains.empty(), "cancelled event releases no runtime or authored chain");
    }
    {
        LifecycleRig rig;
        auto source = event("Missing", false, 1);
        rig.missing = true;
        const auto handle = rig.queue.admit({{&source, std::nullopt, false}}, rig.random).handle;
        rig.pump(2);
        expect(handle && rig.queue.active(*handle) && rig.voices.playing_count() == 0, "missing sample is an allocation failure after accepted admission");
        rig.step();
        expect(!rig.queue.active(*handle), "finite failed allocation retires cleanly");
    }
    for (const auto failure : {Result::no_samples, Result::no_voice}) {
        for (const bool runtime : {false, true}) {
            LifecycleRig rig;
            std::vector<std::string> problems;
            rig.registry.add("Authored", std::vector<Field>{{"Is_3D", "No"}, {"Samples", "authored"}}, problems);
            auto primary = event("FailedPrimary", false, 1, 5);
            primary.play_count = 2;
            primary.chained = "Authored";
            auto assist = event("Runtime", false, 1);
            auto occupied = event("Occupied", false, static_cast<int>(audio::Voices::voices_2d), 1);
            rig.missing = failure == Result::no_samples;
            if (failure == Result::no_voice) {
                for (std::size_t slot = 0; slot < audio::Voices::voices_2d; ++slot) {
                    expect(rig.voices.start({&occupied, {}}, {}, rig.random).result == Result::playing,
                           "fill higher-priority voices before admitted allocation failure");
                }
            }
            const auto handle = rig.queue.admit({{&primary, {}}}, rig.random).handle;
            if (runtime) rig.queue.chain(*handle, &assist, true, 99);
            rig.pump(8);
            expect(handle && !rig.queue.active(*handle) && rig.queue.completed_loops() == 2
                   && rig.starts.size() == 2, "admitted sample failure counts and repeats completed loops");
            expect(rig.chains.size() == 1 && rig.chains[0].runtime == runtime
                   && rig.chains[0].event == (runtime ? &assist : rig.registry.find("Authored")),
                   "missing/capacity failure preserves runtime-before-authored completion chain");
        }
    }
    expect(audio::attached_gain(false, false, true, true) == 1.0, "story cinematic bypasses attached fog");
    expect(audio::attached_gain(false, false, true, false) == 0.0, "ordinary attached fog mutes");
    expect(audio::attached_gain(true, false, false, true) == 0.0
           && audio::attached_gain(false, true, false, true) == 0.0, "silence/model-hidden gates survive cinematic");
    expect(audio::attached_gain(false, false, false, false) == 1.0, "reveal restores attached gain");
}

void stolen_event_completion() {
    for (const bool spatial : {false, true}) {
        for (const int count : {2, -1}) {
            LifecycleRig rig;
            auto victim = event("Victim", spatial, 1, 5);
            victim.play_count = count;
            auto assist = event("Runtime", false, 1);
            const auto size = spatial ? audio::Voices::voices_3d : audio::Voices::voices_2d;
            const auto first_slot = spatial ? 0 : audio::Voices::voices_3d;
            auto occupied = event("Occupied", spatial, static_cast<int>(size) - 1, 1);
            auto incoming = event("Incoming", spatial, 1, 1);
            const auto position = spatial ? std::optional<audio::Vec3>{{10.0, 0.0, 0.0}} : std::nullopt;
            const auto handle = rig.queue.admit({{&victim, position}, 42}, rig.random).handle;
            rig.queue.chain(*handle, &assist, false, 99);
            rig.pump(3);
            expect(rig.voices.playing(first_slot) == &victim, "victim owns the first backend slot");
            for (std::size_t slot = 1; slot < size; ++slot) rig.queue.admit({{&occupied, position}}, rig.random);
            rig.pump(3);
            const auto incoming_position = spatial ? std::optional<audio::Vec3>{{5.0, 0.0, 0.0}} : std::nullopt;
            rig.queue.admit({{&incoming, incoming_position}}, rig.random);
            rig.pump(2); // initialize, main; release capacity before the victim's next loop starts
            expect(rig.voices.playing(first_slot) == &incoming && rig.playing[first_slot],
                   "higher-priority sample steals the victim's slot");
            expect(rig.queue.active(*handle) && rig.queue.completed_loops() == 1 && rig.chains.empty(),
                   "stolen sample completes its current loop and retains finite/infinite event");
            // Free a different slot while the thief keeps playing. Completion of
            // the former owner must never stop or finish the reused first slot.
            rig.playing[first_slot + 1] = false;
            rig.pump(3);
            std::size_t victim_starts = 0;
            for (const auto& cue : rig.starts) if (cue.request.voice.event == &victim) ++victim_starts;
            expect(victim_starts == 2 && rig.voices.playing(first_slot + 1) == &victim,
                   "stolen event starts its next loop on a newly available slot");
            expect(rig.voices.playing(first_slot) == &incoming && rig.playing[first_slot],
                   "former event never controls the thief's reused slot");
            if (count == 2) {
                rig.playing[first_slot + 1] = false;
                rig.pump(4);
                expect(!rig.queue.active(*handle), "finite stolen event finishes after its second loop");
                expect(spatial ? rig.chains.empty() : rig.chains.size() == 1
                       && rig.chains[0].runtime && rig.chains[0].event == &assist,
                       "finite stolen 2D event retains runtime completion chain");
            } else {
                expect(rig.starts.back().continuous, "infinite stolen main restarts continuously");
                rig.queue.stop(*handle, rig.backend());
                rig.step();
                expect(!rig.queue.active(*handle) && rig.chains.empty(), "explicit stop cancels retained infinite event");
            }
            expect(rig.voices.playing(first_slot) == &incoming && rig.playing[first_slot],
                   "retiring the former owner preserves the replacement sample");
        }
    }
}

void wave_files() {
    std::string error;
    const auto mono = wav(1, 16, 1, 100);
    const auto parsed = audio::parse_wav(mono, error);
    expect(parsed && parsed->sample_rate == 22050 && parsed->channels == 1 && parsed->data.size() == 200,
           "16-bit mono PCM: " + error);
    const auto adpcm = wav(0x11, 4, 1, 10);
    expect(!audio::parse_wav(adpcm, error) && error.find("16-bit PCM") != std::string::npos, "ADPCM refused");
    std::vector<std::byte> truncated(mono.begin(), mono.begin() + 30);
    expect(!audio::parse_wav(truncated, error), "truncated refused");
}

void falloff_and_listener() {
    const audio::Space3D space{};
    // Minimum distance 300 * 1.5 = 450: full volume inside, then min / (min + 2 (d - min)).
    expect(near(audio::falloff_gain(100.0, 300.0, space), 1.0), "inside saturation");
    expect(near(audio::falloff_gain(450.0, 300.0, space), 1.0), "at saturation");
    expect(near(audio::falloff_gain(675.0, 300.0, space), 450.0 / (450.0 + 450.0)), "rolloff 2");
    expect(near(audio::falloff_gain(16000.0, 300.0, space), 0.0), "past max distance");
    expect(near(audio::falloff_gain(1.0, 0.0, space), 1.0), "minimum at least 1");
    // A camera 1000 above the plane looking 45 degrees down along +x: the view line meets z = 60
    // 940 ahead, farther than the 940 height, so the listener stands 940 along the ray.
    const double s = std::sqrt(0.5);
    const auto listener = audio::listener_for_camera({0.0, 0.0, 1000.0}, {s, 0.0, -s}, space);
    expect(near(listener.position[0], 940.0 * s, 1.0e-6) && near(listener.position[2], 1000.0 - 940.0 * s, 1.0e-6),
           "pulled back along the ray");
    expect(near(listener.ahead[0], 1.0) && near(listener.ahead[1], 0.0), "flattened facing");
    // Straight down: the look-at point itself.
    const auto down = audio::listener_for_camera({5.0, 7.0, 500.0}, {0.0, 0.0, -1.0}, space);
    expect(near(down.position[0], 5.0) && near(down.position[1], 7.0) && near(down.position[2], 60.0), "straight down");
}

audio::SfxEvent event(const std::string& name, const bool three_d, const int instances, const int priority) {
    audio::SfxEvent result;
    result.name = name;
    result.is_3d = three_d;
    result.max_instances = instances;
    result.priority = priority;
    result.samples = {"A.wav", "B.wav", "C.wav"};
    return result;
}

void voice_rules() {
    audio::Random random(7);
    const audio::Vec3 origin{};
    {
        audio::Voices voices;
        auto response = event("UR", false, 1);
        response.overlap_test = "UNIT_TARTAN";
        auto other = event("UR2", false, 1);
        other.overlap_test = "UNIT_TARTAN";
        const auto first = voices.start({&response, std::nullopt, false}, origin, random);
        expect(first.result == Result::playing && !audio::Voices::is_3d_voice(first.voice), "2D plays on a 2D voice");
        expect(voices.start({&response, std::nullopt, false}, origin, random).result == Result::instance_limit,
               "2D at its limit refused");
        expect(voices.start({&other, std::nullopt, false}, origin, random).result == Result::overlap, "overlap test");
        voices.finished(first.voice);
        expect(voices.start({&other, std::nullopt, false}, origin, random).result == Result::playing, "after it ended");
    }
    {
        audio::Voices voices;
        auto preset = event("P", true, 2);
        preset.preset = true;
        expect(voices.start({&preset, audio::Vec3{}, false}, origin, random).result == Result::preset, "preset refused");
        auto none = event("N", true, 0);
        expect(voices.start({&none, audio::Vec3{}, false}, origin, random).result == Result::no_instances, "Max_Instances 0");
        auto never = event("Z", true, 2);
        never.probability = 0;
        expect(voices.start({&never, audio::Vec3{}, false}, origin, random).result == Result::probability, "probability 0");
        auto fire = event("F", true, 2);
        expect(voices.start({&fire, audio::Vec3{}, true}, origin, random).result == Result::hidden, "fogged object");
        // BA-05: at two instances a nearer third stops the farthest; a farther one is refused.
        const auto a = voices.start({&fire, audio::Vec3{100.0, 0.0, 0.0}, false}, origin, random);
        const auto b = voices.start({&fire, audio::Vec3{300.0, 0.0, 0.0}, false}, origin, random);
        expect(a.result == Result::playing && b.result == Result::playing, "two instances");
        expect(voices.start({&fire, audio::Vec3{400.0, 0.0, 0.0}, false}, origin, random).result == Result::instance_limit,
               "farther instance refused");
        const auto c = voices.start({&fire, audio::Vec3{200.0, 0.0, 0.0}, false}, origin, random);
        expect(c.result == Result::playing && c.stopped == b.voice, "nearer instance stops the farthest");
        expect(voices.playing_count() == 2, "still two");
        // Sequential samples cycle in order.
        auto line = event("S", false, 3);
        line.play_sequentially = true;
        std::string order;
        for (int i = 0; i < 4; ++i) {
            const auto started = voices.start({&line, std::nullopt, false}, origin, random);
            order += started.sample.substr(0, 1);
            voices.finished(started.voice);
        }
        expect(order == "ABCA", "sequential samples: " + order);
    }
    {
        // BA-09: 32 voices busy. A more important sound steals the least important, farthest one.
        audio::Voices voices;
        std::vector<audio::SfxEvent> events;
        events.reserve(33);
        for (int i = 0; i < 32; ++i) events.push_back(event("E" + std::to_string(i), true, 1, i == 5 ? 5 : 2));
        for (int i = 0; i < 32; ++i) {
            const auto started = voices.start({&events[static_cast<std::size_t>(i)], audio::Vec3{1000.0 + i, 0.0, 0.0}, false},
                                              origin, random);
            expect(started.result == Result::playing, "fill voice " + std::to_string(i));
        }
        events.push_back(event("New", true, 1, 2));
        const auto stolen = voices.start({&events.back(), audio::Vec3{10.0, 0.0, 0.0}, false}, origin, random);
        expect(stolen.result == Result::playing && stolen.stopped == stolen.voice
                   && voices.playing(stolen.voice) == &events.back(),
               "steals the priority-5 voice");
        auto important = event("Far", true, 1, 1);
        expect(voices.start({&important, audio::Vec3{50000.0, 0.0, 0.0}, false}, origin, random).result == Result::no_voice,
               "a sound farther than every candidate gets no voice");
    }
    {
        // Volume and pitch stay inside their ranges.
        audio::Voices voices;
        auto ranged = event("R", false, 64);
        ranged.min_volume = 40;
        ranged.max_volume = 60;
        ranged.min_pitch = 95;
        ranged.max_pitch = 105;
        bool inside = true;
        for (int i = 0; i < 12; ++i) {
            const auto started = voices.start({&ranged, std::nullopt, false}, origin, random);
            inside = inside && started.volume >= 0.4 && started.volume <= 0.6 && started.pitch >= 0.95 && started.pitch <= 1.05;
            voices.finished(started.voice);
        }
        expect(inside, "volume and pitch ranges");
    }
}

void two_dimensional_pool() {
    audio::Random random(17);
    audio::Voices voices;
    std::array<audio::SfxEvent, 16> pool{};
    const audio::Vec3 listener{};
    for (std::size_t index = 0; index < pool.size(); ++index) {
        pool[index] = event("Pool" + std::to_string(index), false, 1, index == 3 || index == 9 ? 5 : 2);
        const double clock = index == 9 ? 1.0 : 10.0 + static_cast<double>(index);
        expect(voices.start({&pool[index], {}, false, false, false, clock}, listener, random).result == Result::playing,
               "fill 2D slot");
    }
    auto incoming = event("Incoming", false, 32, 5);
    auto result = voices.start({&incoming, {}}, listener, random);
    expect(result.admitted && result.stopped == audio::Voices::voices_3d + 9, "equal priority steals oldest least important");
    result = voices.start({&incoming, {}}, listener, random);
    expect(result.stopped == audio::Voices::voices_3d + 3, "replacement resets age");
    auto important = event("Important", false, 32, 1);
    result = voices.start({&important, {}}, listener, random);
    expect(result.stopped == audio::Voices::voices_3d + 9, "higher priority steals remaining least important oldest");
    auto low = event("Low", false, 1, 5);
    // Retire the remaining priority-5 voice, leaving only priority 1 and 2.
    result = voices.start({&important, {}}, listener, random);
    expect(result.stopped == audio::Voices::voices_3d + 3, "higher priority clears final priority-5 voice");
    result = voices.start({&low, {}}, listener, random);
    expect(result.result == Result::no_voice && result.admitted && !result.stopped, "lower priority admitted but not allocated");
    pool[0].overlap_test = "BUSY";
    low.overlap_test = "BUSY";
    result = voices.start({&low, {}}, listener, random);
    expect(result.result == Result::overlap && !result.admitted, "overlap refuses before allocation");
    expect(voices.playing_count() == 16, "pool bounded after steals and refusals");
    audio::Voices tied;
    for (auto& sfx : pool) {
        sfx.priority = 3;
        sfx.overlap_test.clear();
        expect(tied.start({&sfx, {}, false, false, false, 7.0}, listener, random).result == Result::playing, "fill tied pool");
    }
    low.priority = 3;
    low.overlap_test.clear();
    expect(tied.start({&low, {}}, listener, random).stopped == audio::Voices::voices_3d, "equal clocks retain first slot");
    auto silent = event("Silent", false, 1, 1);
    silent.play_sequentially = true;
    silent.min_volume = silent.max_volume = 0;
    result = tied.start({&silent, {}}, listener, random);
    expect(result.result == Result::zero_gain && result.admitted && !result.stopped, "zero target gain never steals");
    expect(result.sample == "A.wav", "silent allocation still draws the event sample");
    silent.min_volume = silent.max_volume = 100;
    expect(tied.start({&silent, {}}, listener, random).sample == "B.wav", "silent sample advances sequential cursor before refusal");
}

void category_admission_and_gain() {
    audio::Random random(19);
    const audio::Vec3 listener{};
    for (const bool spatial : {false, true}) {
        for (const bool localized : {false, true}) {
            auto sfx = event("Category", spatial, 32);
            sfx.localized = localized;
            sfx.unit_response_vo = true;
            audio::MixLevels levels{0.5, 0.4, 0.8, 0.1};
            expect(near(audio::category_gain(sfx, levels), localized ? 0.4 : 0.2), "Localize chooses gain in both spatial modes");
            levels.master = 0.25;
            expect(near(audio::category_gain(sfx, levels), localized ? 0.2 : 0.1), "master multiplies category independently");
            levels.speech = 0.2;
            expect(near(audio::category_gain(sfx, levels), localized ? 0.05 : 0.1), "speech slider affects only localized WAVs");
            levels.sfx = 0.8;
            expect(near(audio::category_gain(sfx, levels), localized ? 0.05 : 0.2), "SFX slider affects only other WAVs");
        }
    }
    auto sfx = event("Admission", false, 32);
    audio::Voices voices;
    audio::Admission policy;
    policy.speech_stream = true;
    sfx.hud_vo = true;
    expect(voices.start({&sfx, {}}, listener, random, policy).result == Result::hud_speech, "HUD refused during retained MP3");
    sfx.hud_vo = false;
    sfx.unit_response_vo = true;
    expect(voices.start({&sfx, {}}, listener, random, policy).result == Result::playing, "unit response has no MP3 exclusion");
    for (int category = 0; category < 4; ++category) {
        audio::Admission disabled;
        sfx.localized = category == 0;
        sfx.unit_response_vo = category == 1;
        sfx.hud_vo = category == 2;
        sfx.ambient_vo = category == 3;
        disabled.localized = category != 0;
        disabled.unit_response = category != 1;
        disabled.hud = category != 2;
        disabled.ambient = category != 3;
        expect(voices.start({&sfx, {}, false, true}, listener, random, disabled).result == Result::category_disabled,
               "forced request retains disabled category gate");
        expect(voices.start({&sfx, {}}, listener, random, disabled).result == Result::category_disabled,
               "ordinary request retains disabled category gate");
    }
    sfx.ambient_vo = false;
    sfx.hud_vo = true;
    sfx.max_instances = 0;
    sfx.probability = 0;
    sfx.overlap_test = "FORCED";
    policy.demo_dialog = policy.tutorial_tactical = true;
    expect(voices.start({&sfx, {}, false, true}, listener, random, policy).result == Result::playing,
           "force bypasses instances, probability, HUD speech, tutorial and demo gates");
    expect(voices.start({&sfx, {}, true, true}, listener, random, policy).result == Result::hidden, "force retains explicit fog gate");
    expect(voices.start({&sfx, {}, false, true, true}, listener, random, policy).result == Result::delete_pending, "force retains delete gate");
    policy.story_cinematic = true;
    expect(voices.start({&sfx, {}, true, true}, listener, random, policy).result == Result::playing, "story cinematic bypasses explicit fog");
    policy.negative_feedback = &sfx;
    policy.cinematic = true;
    expect(voices.start({&sfx, {}, false, true}, listener, random, policy).result == Result::cinematic_feedback, "force retains cinematic negative feedback gate");
    policy = {};
    sfx.max_instances = 32;
    sfx.probability = 100;
    sfx.overlap_test.clear();
    policy.tutorial_tactical = true;
    expect(voices.start({&sfx, {}}, listener, random, policy).result == Result::tutorial_hud, "tutorial HUD refusal");
    policy = {};
    policy.demo_dialog = true;
    expect(voices.start({&sfx, {}}, listener, random, policy).result == Result::demo_dialog, "demo dialog refuses non-GUI");
    sfx.gui = true;
    expect(voices.start({&sfx, {}}, listener, random, policy).result == Result::playing, "demo dialog admits GUI");
    expect(audio::pauses_with_game(sfx, true) && !audio::pauses_with_game(sfx, false), "pause spatial one-shots, keep 2D one-shots running");
    sfx.play_count = -1;
    expect(audio::pauses_with_game(sfx, false), "pause infinite 2D loop");
    expect(near(audio::speech_sfx_gain(0.8, false, false), 0.8), "WAV response never starts MP3 ducking");
}

void attached_voice_positions() {
    audio::Random random(7);
    audio::Voices voices;
    const audio::Vec3 listener{};
    auto spin = event("Spin", true, 2);
    const auto near_start = voices.start({&spin, audio::Vec3{100.0, 0.0, 0.0}, false}, listener, random);
    const auto far_start = voices.start({&spin, audio::Vec3{300.0, 0.0, 0.0}, false}, listener, random);
    // SP-03/BA-05: the initially nearer spinning copy has flown beyond the other voice.
    voices.set_position(near_start.voice, {500.0, 0.0, 0.0});
    const auto replacement = voices.start({&spin, audio::Vec3{400.0, 0.0, 0.0}, false}, listener, random);
    expect(replacement.result == Result::playing && replacement.stopped == near_start.voice,
           "an attached voice is culled at its current position, not its starting position");
    expect(voices.playing(far_start.voice) == &spin && voices.playing_count() == 2,
           "moving the copy keeps the other voice and the instance count");
    voices.set_position(audio::Voices::voices_3d + audio::Voices::voices_2d, {});
}

void speakers() {
    const std::vector<std::string> rankings{"Hero", "Super", "Capital", "Frigate", "Corvette", "Transport",
                                            "Bomber", "Fighter"};
    const std::vector<audio::RankedUnit> mixed{{0, {"Fighter", "AntiBomber"}, std::nullopt},
                                               {1, {"Frigate", "AntiCorvette"}, 6},
                                               {2, {"Corvette"}, 7}};
    expect(audio::speaker(mixed, rankings) == 1u, "the frigate speaks");
    // FoC's rule as written: after a frigate (ranking 6) a unit of a worse category with a lower
    // ranking takes over.
    const std::vector<audio::RankedUnit> quirk{{0, {"Frigate"}, 6}, {1, {"Fighter"}, 2}};
    expect(audio::speaker(quirk, rankings) == 1u, "lower ranking takes over");
    const std::vector<audio::RankedUnit> none{{0, {"Structure"}, 1}};
    expect(!audio::speaker(none, rankings), "unranked category");
}

void music() {
    const audio::MusicEvent ambient = audio::parse_music_event(
        "Space_Ambient", std::vector<Field>{{"Files", " Imperial_Attack_1.MP3, Beginning_The_Approach.MP3 "},
                                            {"Volume_Percent", "55"}, {"Fade_In_Seconds", "2.0"},
                                            {"Fade_Out_Previous_Seconds", "2.0"}, {"Loop", "Yes"}});
    const audio::MusicEvent battle = audio::parse_music_event(
        "Space_Battle", std::vector<Field>{{"Files", "A.MP3, B.MP3, C.MP3"}, {"Volume_Percent", "55"},
                                           {"Fade_In_Seconds", "0.5"}, {"Loop", "Yes"}});
    expect(ambient.files.size() == 2 && near(ambient.volume, 0.55) && near(ambient.fade_in_seconds, 2.0) && ambient.loop,
           "music event fields");
    audio::Random random(3);
    audio::MusicDirector director({&ambient}, {&battle}, 450);
    auto cue = director.begin(random);
    expect(cue && cue->file == "Imperial_Attack_1.MP3" && director.mode() == audio::MusicDirector::Mode::ambient, "ambient first");
    cue = director.tick(false, random);
    expect(!cue, "quiet tick");
    cue = director.tick(true, random);
    expect(cue && cue->file == "A.MP3" && director.mode() == audio::MusicDirector::Mode::battle, "a shot starts battle music");
    for (int i = 0; i < 449; ++i) {
        cue = director.tick(i == 100, random);
        expect(!cue, "battle continues");
    }
    // 348 quiet ticks since the attack at i = 100; 102 more reach the 450 of peace.
    for (int i = 0; i < 110; ++i) cue = director.tick(false, random);
    expect(director.mode() == audio::MusicDirector::Mode::ambient, "peace returns to ambient");
    cue = director.track_ended();
    expect(cue && cue->file == "Imperial_Attack_1.MP3", "ambient continues with its next file");
    cue = director.tick(true, random);
    expect(cue && cue->file == "B.MP3", "battle resumes where it stopped");

    const std::vector<Field> outcome_fields{
        {"Music_Event_Tactical_Win", " DefaultWin "},
        {"Music_Event_Tactical_Lose", " DefaultLose "},
        {"Music_Event_Tactical_Win_Vs_Faction", "Empire, FirstWin"},
        {"Music_Event_Tactical_Win_Vs_Faction", "Empire, SecondWin"},
        {"Music_Event_Tactical_Win_Vs_Faction", "Underworld, "},
        {"Music_Event_Tactical_Lose_Vs_Faction", "Empire, LoseEmpire"}};
    expect(audio::tactical_music_event(outcome_fields, true, "Empire") == "FirstWin", "first exact faction wins");
    expect(audio::tactical_music_event(outcome_fields, true, "Pirates") == "DefaultWin", "unmatched win default");
    expect(audio::tactical_music_event(outcome_fields, false, "Pirates") == "DefaultLose", "unmatched lose default");
    expect(audio::tactical_music_event(outcome_fields, true, "Underworld").empty(), "matched blank does not fall back");
    expect(audio::tactical_music_event(outcome_fields, false, "Empire") == "LoseEmpire", "defeat override");
    const auto victory = audio::parse_music_event("Win", std::vector<Field>{{"Files", "Win.MP3"}});
    const auto defeat = audio::parse_music_event("Lose", std::vector<Field>{{"Files", "Lose.MP3"}});
    cue = director.result(true, &victory);
    expect(cue && cue->file == "Win.MP3" && cue->mode == audio::MusicDirector::Mode::victory, "exact local winner");
    expect(!director.result(true, &victory), "repeated result does not restart");
    expect(!director.tick(false, random), "quiet result does not switch to ambient");
    cue = director.result(false, &defeat);
    expect(cue && cue->file == "Lose.MP3" && cue->mode == audio::MusicDirector::Mode::defeat, "other winner selects lose");
    expect(!director.result(true, nullptr) && director.mode() == audio::MusicDirector::Mode::defeat,
           "blank selection preserves prior music mode");

    audio::MusicFade fade;
    fade.begin(2.0);
    fade.advance(1.0);
    expect(near(fade.level, 0.5), "half faded in");
    fade.retire(2.0);
    fade.advance(1.0);
    expect(near(fade.level, 0.25), "partial fade keeps full duration");
    fade.advance(1.0);
    expect(near(fade.level, 0.0), "partial fade reaches zero at two seconds");
    fade.begin(0.0);
    fade.retire(2.0);
    fade.advance(1.0);
    expect(near(fade.level, 0.5), "full-level crossfade midpoint");
    fade.advance(1.0);
    expect(near(fade.level, 0.0), "full-level crossfade end");
    fade.begin(2.0);
    fade.retire(0.0);
    fade.advance(1.0);
    expect(std::isfinite(fade.level) && near(fade.level, 0.0), "zero level/duration remains finite");
}

void announcements() {
    using Cue = audio::SightingAnnouncements::Cue;
    audio::SightingAnnouncements sightings;
    bool hero_seen = false;
    expect(sightings.observe(hero_seen, true, false, true, true) == Cue::type,
           "type cue wins its first service call");
    expect(hero_seen && !sightings.enemy_seen(), "type retained, enemy still eligible");
    expect(sightings.observe(hero_seen, true, false, true, true) == Cue::enemy,
           "still-visible object can trigger first enemy without a new reveal");
    expect(sightings.observe(hero_seen, true, false, true, true) == Cue::none,
           "refog/reveal never resets type or first enemy eligibility");
    bool another_type = false;
    expect(sightings.observe(another_type, true, false, true, true) == Cue::type,
           "type sighting remains independent of first enemy");
    audio::SightingAnnouncements busy;
    bool busy_type = false;
    expect(busy.observe(busy_type, true, true, true, true) == Cue::enemy,
           "busy 2D consumes type but does not block first enemy");
    expect(busy_type && busy.observe(busy_type, true, false, true, true) == Cue::none,
           "busy type is not retried after WAV completion");
    audio::SightingAnnouncements absent;
    bool absent_type = false;
    expect(absent.observe(absent_type, true, false, false, true) == Cue::enemy,
           "missing type cue still permits first enemy");
    expect(absent.observe(absent_type, true, false, true, true) == Cue::none,
           "missing enemy/type cue does not restore consumed eligibility");
    bool friendly_type = false;
    audio::SightingAnnouncements friendly;
    expect(friendly.observe(friendly_type, true, false, true, false) == Cue::type,
           "type handler has no enemy-owner gate");
    bool unannounced = false;
    expect(friendly.observe(unannounced, false, false, true, false) == Cue::none && !unannounced,
           "unflagged allied or neutral object remains silent");

    const int one = 1, two = 2;
    audio::SpeechQueue<int, 3> queue;
    expect(queue.push(&one, "first") && queue.push(&one, "duplicate") && queue.push(&two, "last"),
           "FIFO retains duplicates without priority or cooldown");
    expect(!queue.push(&two, "overflow") && queue.size() == 3, "bounded storage reports overflow");
    expect(queue.front()->event == &one && queue.front()->reason == "first", "first speech at front");
    queue.pop();
    expect(queue.front()->reason == "duplicate", "completion/failure removes exactly one front");
    expect(queue.push(&two, "wrapped"), "queue reuses released storage");
    queue.pop();
    expect(queue.front()->reason == "last", "wrapped insertion preserves FIFO");
    queue.pop(); queue.pop(); queue.pop();
    expect(!queue.front() && !queue.push(nullptr, "missing"), "empty and missing event are safe");

    audio::SpeechStream stream;
    expect(stream.admit(3), "normal speech admitted");
    stream.opened();
    const auto old = stream.handle();
    expect(!stream.admit(4) && stream.contains(old), "less important newcomer preserves playing stream");
    expect(stream.admit(3) && !stream.contains(old) && !stream.active(),
           "equal priority retires old before replacement file opens");
    stream.opened();
    stream.finished(true);
    expect(stream.active(), "paused stream remains retained for ducking");
    expect(stream.admit(1) && !stream.active(), "higher priority interrupts, failed file leaves no stream");
    stream.opened(); stream.finished(false);
    expect(!stream.active(), "unpaused completion releases stream");
    expect(near(audio::speech_sfx_gain(0.8, true, false), 0.3), "speech uses absolute ceiling, not multiplication");
    expect(near(audio::speech_sfx_gain(0.2, true, false), 0.2), "quiet SFX stays quiet");
    expect(near(audio::speech_sfx_gain(0.8, true, true), 0.8), "GUI dialog bypasses cap");
    expect(near(audio::speech_sfx_gain(0.8, false, false), 0.8), "WAV VO or queued-only speech does not duck");
}

} // namespace

int main() {
    for (const auto cue : {audio::CommandCue::attack, audio::CommandCue::attack_move,
                           audio::CommandCue::guard, audio::CommandCue::move}) {
        expect(audio::command_cue_on_press(cue, true), "mode arming requests its cue");
        expect(!audio::command_cue_on_press(cue, false), "mode disarming is silent");
    }
    expect(audio::command_cue_on_press(audio::CommandCue::stop, false), "stop cue does not depend on selection or mode");
    expect(!audio::command_cue_on_press(audio::CommandCue::none, true), "executing a world order has no button cue");
    const int asteroid = 1, nebula = 2, group = 3, ordinary = 4, assist = 5;
    expect(audio::move_response(true, true, true, &asteroid, &nebula, &group, &ordinary) == &asteroid,
           "asteroid wins overlapping hazard destinations");
    expect(audio::move_response(true, true, true, static_cast<const int*>(nullptr), &nebula, &group, &ordinary) == &group,
           "missing asteroid line skips nebula and falls through to group");
    expect(audio::move_response(false, true, false, &asteroid, &nebula, &group, &ordinary) == &nebula,
           "nebula line is used before ordinary movement");
    expect(audio::move_response(false, true, false, &asteroid, static_cast<const int*>(nullptr), &group, &ordinary) == &ordinary,
           "missing single-unit environment line falls through to ordinary");
    expect(audio::assist_eligible(false, false, false), "different-type other selected object can assist");
    expect(!audio::assist_eligible(true, false, false) && !audio::assist_eligible(false, true, false)
           && !audio::assist_eligible(false, false, true), "speaker, same type and vehicle thief cannot assist");
    audio::ResponseChains<int, 2> chains;
    expect(!chains.complete(0).cue, "refused primary creates no chain");
    chains.set(0, &assist, true, 7);
    expect(chains.complete(1).cue == nullptr, "unrelated completion cannot release chain");
    const auto completed = chains.complete(0);
    expect(completed.cue == &assist && completed.attack && completed.source == 7 && !chains.complete(0).cue,
           "natural completion releases the retained source once");
    for (const bool attack : {false, true}) {
        chains.set(0, &assist, attack, 7);
        chains.set(1, &assist, attack, 8);
        // Candidate 7 leaves between primary start and completion; candidate 8 stays live.
        chains.cancel_missing([](const std::uint64_t source) { return source == 8; });
        expect(!chains.complete(0).cue, "removed move/attack assist source cannot release at primary completion");
        const auto surviving = chains.complete(1);
        expect(surviving.cue == &assist && surviving.attack == attack && surviving.source == 8,
               "unrelated source removal preserves the other pending move/attack assist");
    }
    chains.set(0, &assist, false, 7);
    chains.cancel(0);
    expect(!chains.complete(0).cue, "explicit cancellation removes pending assist");
    registry_and_presets();
    event_lifecycle();
    stolen_event_completion();
    wave_files();
    falloff_and_listener();
    voice_rules();
    two_dimensional_pool();
    category_admission_and_gain();
    attached_voice_positions();
    speakers();
    music();
    announcements();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "audio sfx contracts passed\n";
    return 0;
}
