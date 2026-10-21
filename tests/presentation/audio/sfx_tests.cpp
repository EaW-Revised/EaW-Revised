// P2-21 (#84): FoC's sound-event rules (presentation::audio, docs/behaviour/battle-audio.md).
// Synthetic inputs only; no game data.

#include "../../support/check.hpp"
#include "eawr/presentation/audio/sfx.hpp"
#include "eawr/presentation/audio/announcements.hpp"
#include "eawr/presentation/audio/command_cues.hpp"
#include "eawr/presentation/audio/hardpoint_loss.hpp"
#include "eawr/presentation/space/unit_fade.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace audio = eawr::presentation::audio;
namespace space = eawr::presentation::space;
namespace sim = eawr::sim;
using audio::Field;
using Result = audio::Start::Result;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string& message) {
    if (!condition) local_checker.check_before_print(condition, message, std::cerr, "FAIL: ");
}

[[nodiscard]] bool near(const double a, const double b, const double tolerance = 1.0e-9) {
    return std::abs(a - b) <= tolerance;
}

void hardpoint_loss() {
    audio::SfxRegistry registry;
    std::vector<std::string> problems;
    registry.add("Unit_Lost_Laser_Calamari", std::vector<Field>{{"Is_2D", "Yes"}, {"Is_3D", "No"},
        {"Is_Unit_Response_VO", "Yes"}, {"Samples", "synthetic.wav"}, {"Overlap_Test", "Unit_Calamari"}}, problems);
    registry.add("All_Weapons", std::vector<Field>{{"Samples", "synthetic.wav"}}, problems);
    const auto* laser = registry.find("Unit_Lost_Laser_Calamari");
    const auto* all = registry.find("All_Weapons");
    std::map<std::string, const audio::SfxEvent*> cues;
    const auto resolve = [&registry](const std::string& name) { return registry.find(name); };
    audio::read_hardpoint_loss("HARD_POINT_WEAPON_LASER, Unit_Lost_Laser_Calamari", cues, resolve);
    audio::read_hardpoint_loss("HARD_POINT_WEAPON_MISSILE, Unit_Lost_Laser_Calamari", cues, resolve);
    audio::read_hardpoint_loss("hard_point_weapon_missile, ", cues, resolve);
    const std::vector<std::string> types{"hard_point_weapon_laser", "hard_point_weapon_laser", "hard_point_weapon_missile"};
    const std::array<std::uint32_t, 2> first{1, 2};
    const std::array<std::uint32_t, 1> remaining{2};
    const std::span<const std::uint32_t> none;
    const auto choose = [&](bool local, std::size_t lost, std::span<const std::uint32_t> standing,
                            bool weapons, const audio::SfxEvent* fallback) {
        return audio::hardpoint_loss_cue(local, true, weapons, lost, standing, types, cues, fallback);
    };
    expect(!choose(true, 0, first, true, all), "BA-64: first laser loss stays silent while another laser stands");
    const auto* chosen = choose(true, 1, remaining, true, all);
    expect(chosen == laser, "BA-64: local Calamari's last laser selects its authored warning");
    expect(!choose(false, 1, remaining, true, all), "BA-64: enemy/allied owner cannot warn the local player");
    expect(!choose(true, 2, none, false, nullptr), "BA-64: blank derived class clears inherited warning");
    expect(choose(true, 1, none, false, all) == all, "BA-64: authored all-weapons warning takes precedence");
    expect(choose(true, 1, none, false, nullptr) == laser, "BA-64: absent all-weapons event falls back to class");
    audio::Random random(7);
    audio::EventQueue queue;
    const auto admitted = queue.admit({{chosen, {}}}, random);
    expect(admitted.handle.has_value(), "BA-64: local hardpoint warning enters ordinary SFX queue once");
    expect(queue.admit({{chosen, {}}}, random).result == Result::instance_limit,
        "BA-64: pending warning respects ordinary instance throttling");
    auto competing = *laser;
    competing.name = "Other_Calamari_Response";
    expect(queue.admit({{&competing, {}}}, random).result == Result::overlap,
        "BA-64: authored shared overlap group throttles different Calamari responses");
    expect(queue.size() == 1, "BA-64: refused warnings do not queue retries");
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

// A WAVE file from a raw fmt body and data payload; `fact` adds a fact chunk with that frame count.
std::vector<std::byte> riff(const std::vector<std::uint8_t>& fmt, const std::vector<std::uint8_t>& data,
                            const std::optional<std::uint32_t> fact = std::nullopt) {
    std::vector<std::byte> bytes;
    const auto put = [&bytes](const char* text) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::byte>(text[i]));
    };
    const auto u32 = [&bytes](const std::uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xffU));
    };
    const auto raw = [&bytes](const std::vector<std::uint8_t>& values) {
        for (const auto value : values) bytes.push_back(static_cast<std::byte>(value));
    };
    put("RIFF");
    u32(0);  // not read by the decoder
    put("WAVE");
    put("fmt ");
    u32(static_cast<std::uint32_t>(fmt.size()));
    raw(fmt);
    if (fact) {
        put("fact");
        u32(4);
        u32(*fact);
    }
    put("data");
    u32(static_cast<std::uint32_t>(data.size()));
    raw(data);
    if ((data.size() & 1U) != 0U) bytes.push_back(std::byte{0});
    return bytes;
}

std::vector<std::uint8_t> fmt_body(const std::uint16_t format, const std::uint16_t channels, const std::uint16_t block_align,
                                   const std::uint16_t bits, const std::vector<std::uint8_t>& extra = {}) {
    std::vector<std::uint8_t> body;
    const auto u16 = [&body](const std::uint16_t value) {
        body.push_back(static_cast<std::uint8_t>(value & 0xffU));
        body.push_back(static_cast<std::uint8_t>(value >> 8U));
    };
    const auto u32 = [&u16](const std::uint32_t value) {
        u16(static_cast<std::uint16_t>(value & 0xffffU));
        u16(static_cast<std::uint16_t>(value >> 16U));
    };
    u16(format);
    u16(channels);
    u32(22050);
    u32(22050U * block_align);
    u16(block_align);
    u16(bits);
    body.insert(body.end(), extra.begin(), extra.end());
    return body;
}

std::vector<std::int16_t> frames_of(const audio::WavPcm& pcm) {
    std::vector<std::int16_t> values;
    for (std::size_t at = 0; at + 1 < pcm.data.size(); at += 2) {
        values.push_back(static_cast<std::int16_t>(std::to_integer<std::uint16_t>(pcm.data[at])
                                                   | (std::to_integer<std::uint16_t>(pcm.data[at + 1]) << 8U)));
    }
    return values;
}

void wave_files() {
    std::string error;
    const auto mono = wav(1, 16, 1, 100);
    const auto parsed = audio::parse_wav(mono, error);
    expect(parsed && parsed->sample_rate == 22050 && parsed->channels == 1 && parsed->data.size() == 200,
           "16-bit mono PCM: " + error);
    // 16-bit PCM stays a view of the file, not a copy.
    expect(parsed && parsed->converted.empty() && parsed->data.data() >= mono.data()
           && parsed->data.data() < mono.data() + mono.size(), "16-bit PCM is borrowed from the file");
    std::vector<std::byte> truncated(mono.begin(), mono.begin() + 30);
    expect(!audio::parse_wav(truncated, error), "truncated refused");
    expect(!audio::parse_wav(std::vector<std::byte>(8, std::byte{0}), error) && error.find("RIFF") != std::string::npos,
           "not RIFF refused");

    // 8-bit PCM is unsigned: 0x80 is silence, 0x00 the most negative and 0xFF the most positive step.
    const auto eight = riff(fmt_body(1, 1, 1, 8), {0x80, 0x00, 0xFF, 0x81, 0x7F});
    const auto widened = audio::parse_wav(eight, error);
    expect(widened && widened->channels == 1 && widened->sample_rate == 22050, "8-bit PCM parses: " + error);
    if (widened) {
        const auto values = frames_of(*widened);
        expect(values == std::vector<std::int16_t>({0, -32768, 32512, 256, -256}), "8-bit PCM is centred and widened to 16 bits");
    }
    // A stereo 8-bit file with an odd trailing byte keeps whole frames only.
    const auto eight_stereo = riff(fmt_body(1, 2, 2, 8), {0x80, 0x90, 0x70, 0x80, 0x80});
    const auto stereo = audio::parse_wav(eight_stereo, error);
    expect(stereo && frames_of(*stereo) == std::vector<std::int16_t>({0, 4096, -4096, 0}), "8-bit stereo keeps whole frames");

    // WAVE_FORMAT_EXTENSIBLE carries the real format in its sub-format; PCM and ADPCM both work.
    std::vector<std::uint8_t> extensible_extra{22, 0, 16, 0, 4, 0, 0, 0, 1, 0};  // cbSize, valid bits, mask, sub-format tag 1
    extensible_extra.resize(24, 0);  // rest of the sub-format GUID
    const auto extensible = riff(fmt_body(0xFFFE, 1, 2, 16, extensible_extra), {0x01, 0x00, 0xFF, 0x7F});
    const auto wide = audio::parse_wav(extensible, error);
    expect(wide && frames_of(*wide) == std::vector<std::int16_t>({1, 32767}), "WAVE_FORMAT_EXTENSIBLE PCM: " + error);
    auto short_extensible = fmt_body(0xFFFE, 1, 2, 16);  // 16 bytes: no sub-format
    expect(!audio::parse_wav(riff(short_extensible, {0, 0}), error) && error.find("EXTENSIBLE") != std::string::npos,
           "EXTENSIBLE without a sub-format is refused");

    // Formats this decoder does not read are refused with the format and bit depth named.
    expect(!audio::parse_wav(riff(fmt_body(1, 1, 3, 24), {0, 0, 0}), error) && error.find("24 bits") != std::string::npos,
           "24-bit PCM refused");
    expect(!audio::parse_wav(riff(fmt_body(3, 1, 4, 32), {0, 0, 0, 0}), error) && error.find("format 3") != std::string::npos,
           "float refused");
    expect(!audio::parse_wav(riff(fmt_body(1, 3, 6, 16), {0, 0, 0, 0, 0, 0}), error) && error.find("channel") != std::string::npos,
           "three channels refused");

    // IMA ADPCM, mono: a 4-byte header (first sample 1000, step index 10), then four code bytes.
    const std::vector<std::uint8_t> ima_extra{2, 0, 9, 0};  // cbSize, 9 samples per block
    const std::vector<std::uint8_t> mono_block{0xE8, 0x03, 10, 0, 0x21, 0x43, 0x65, 0x87};
    const auto ima_mono = audio::parse_wav(riff(fmt_body(0x11, 1, 8, 4, ima_extra), mono_block), error);
    expect(ima_mono && ima_mono->channels == 1, "IMA ADPCM mono parses: " + error);
    if (ima_mono) {
        expect(frames_of(*ima_mono) == std::vector<std::int16_t>({1000, 1006, 1016, 1030, 1045, 1068, 1108, 1191, 1179}),
               "IMA ADPCM mono decodes low nibble first");
    }
    // Stereo: each channel has its own header, then 4 code bytes per channel in channel order.
    const std::vector<std::uint8_t> stereo_block{0xE8, 0x03, 10, 0, 0x30, 0xF8, 40, 0,
                                                 0x21, 0x43, 0x65, 0x87, 0x9A, 0xBC, 0xDE, 0xF0};
    const auto ima_stereo = audio::parse_wav(riff(fmt_body(0x11, 2, 16, 4, ima_extra), stereo_block), error);
    expect(ima_stereo && ima_stereo->channels == 2, "IMA ADPCM stereo parses: " + error);
    if (ima_stereo) {
        const auto values = frames_of(*ima_stereo);
        const std::array<std::int16_t, 9> left{1000, 1006, 1016, 1030, 1045, 1068, 1108, 1191, 1179};
        const std::array<std::int16_t, 9> right{-2000, -2210, -2324, -2637, -2931, -3429, -4177, -4078, -5435};
        bool match = values.size() == 18;
        for (std::size_t frame = 0; match && frame < 9; ++frame) {
            match = values[frame * 2] == left[frame] && values[frame * 2 + 1] == right[frame];
        }
        expect(match, "IMA ADPCM stereo interleaves the two channels' groups");
    }
    // The predictor saturates instead of wrapping.
    const std::vector<std::uint8_t> clamp_block{0x00, 0x7D, 88, 0, 0x77, 0x77, 0x77, 0x77};
    const auto clamped = audio::parse_wav(riff(fmt_body(0x11, 1, 8, 4, ima_extra), clamp_block), error);
    expect(clamped && frames_of(*clamped) == std::vector<std::int16_t>({32000, 32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767}),
           "IMA ADPCM saturates at 16 bits");
    // Two blocks decode independently; `fact` trims the padding of the last one.
    std::vector<std::uint8_t> two_blocks = mono_block;
    two_blocks.insert(two_blocks.end(), {0x10, 0x00, 0, 0, 0x00, 0x00, 0x00, 0x00});
    const auto trimmed = audio::parse_wav(riff(fmt_body(0x11, 1, 8, 4, ima_extra), two_blocks, 12), error);
    expect(trimmed && frames_of(*trimmed).size() == 12 && frames_of(*trimmed)[9] == 16, "IMA ADPCM blocks restart and `fact` trims");
    // A damaged step index or block size is refused, not decoded.
    expect(!audio::parse_wav(riff(fmt_body(0x11, 1, 8, 4, ima_extra), {0, 0, 90, 0, 0, 0, 0, 0}), error)
           && error.find("step index") != std::string::npos, "IMA ADPCM step index out of range refused");
    expect(!audio::parse_wav(riff(fmt_body(0x11, 1, 3, 4, ima_extra), {0, 0, 0, 0}), error)
           && error.find("block size") != std::string::npos, "IMA ADPCM block size refused");
    // Moving the result keeps the converted frames readable.
    auto moved = audio::parse_wav(riff(fmt_body(0x11, 1, 8, 4, ima_extra), mono_block), error);
    std::optional<audio::WavPcm> later = std::move(moved);
    expect(later && frames_of(*later).size() == 9 && frames_of(*later)[8] == 1179, "a moved ADPCM result still views its frames");
}

void sample_paths() {
    using Paths = std::vector<std::string>;
    // A bare name: the SFX archives (mounted under data/audio/sfx), then the name as an archive entry.
    expect(audio::sfx_sample_candidates("Blaster_Fire_1.WAV") == Paths({"data/audio/sfx/blaster_fire_1.wav", "blaster_fire_1.wav"}),
           "a bare sample name");
    expect(audio::sfx_sample_candidates("  Spaced.wav ") == Paths({"data/audio/sfx/spaced.wav", "spaced.wav"}), "a padded name");
    // Data-rooted names are looked up as given, with no second prefix (ML-04's four spellings, any case).
    const Paths rooted{"data/audio/voiceovers/rebel/lead_01.wav"};
    expect(audio::sfx_sample_candidates("Data\\Audio\\Voiceovers\\Rebel\\Lead_01.wav") == rooted, "Data\\ rooted");
    expect(audio::sfx_sample_candidates("DATA/Audio/Voiceovers/Rebel/Lead_01.wav") == rooted, "DATA/ rooted");
    expect(audio::sfx_sample_candidates(".\\Data\\Audio\\Voiceovers\\Rebel\\Lead_01.wav") == rooted, ".\\Data\\ rooted");
    expect(audio::sfx_sample_candidates("./data/audio/voiceovers/rebel/lead_01.wav") == rooted, "./data/ rooted");
    // A sub-path that is not Data-rooted is not redirected: it is a bare archive-style name.
    expect(audio::sfx_sample_candidates("Weapons\\Laser.wav") == Paths({"data/audio/sfx/weapons/laser.wav", "weapons/laser.wav"}),
           "a sub-path without Data");
    expect(audio::sfx_sample_candidates("database.wav") == Paths({"data/audio/sfx/database.wav", "database.wav"}),
           "a name that only starts with the letters data");
    expect(audio::sfx_sample_candidates(" ").empty(), "an empty name has no candidates");
    // Music and speech add their fixed directory, whatever the name looks like.
    expect(audio::music_sample_path("GC_Empire_1.MP3") == "data/audio/music/gc_empire_1.mp3", "music path");
    expect(audio::speech_sample_path("English", "UWEC_TYB0100_ENG.MP3") == "data/audio/speech/english/uwec_tyb0100_eng.mp3",
           "speech path");
}

void stream_selection_cache_and_diagnostics() {
    // File spelling is deliberately absent from the shared decoder interface.
    // Both consumers use this seam for speech and music.
    const auto wave_bytes = wav(1, 16, 1, 4);
    const std::vector<std::byte> mp3_bytes{std::byte{0xff}, std::byte{0xfb}, std::byte{0x90}, std::byte{0x64}};
    std::string error;
    int waves = 0, mp3s = 0;
    const auto decode = [&](const auto& bytes) {
        return audio::decode_stream(bytes, error,
            [&](const audio::WavPcm& pcm) { ++waves; return pcm.sample_rate == 22050 && pcm.data.size() == 8; },
            [&](std::span<const std::byte> data) { ++mp3s; return data.size() == 4 && data[0] == std::byte{0xff}; });
    };
    expect(decode(wave_bytes), "WAVE bytes named line.mp3 decode as PCM");
    expect(waves == 1 && mp3s == 0 && error.empty(), "WAVE never reaches MP3 backend");
    expect(decode(mp3_bytes), "MP3 bytes named line.wav reach MP3 backend");
    expect(waves == 1 && mp3s == 1, "MP3 never reaches WAVE parser");
    const std::vector<std::byte> truncated{std::byte{'R'}, std::byte{'I'}, std::byte{'F'}, std::byte{'F'}};
    expect(!decode(truncated) && !error.empty() && mp3s == 1, "malformed RIFF does not fall through to MP3");
    const auto path = audio::speech_sample_path("english", "line.mp3");
    const auto sfx_key = audio::galactic_stream_key(audio::GalacticStreamKind::sfx, path);
    const auto speech_key = audio::galactic_stream_key(audio::GalacticStreamKind::speech, path);
    const auto music_key = audio::galactic_stream_key(audio::GalacticStreamKind::music, path);
    expect(sfx_key != speech_key && sfx_key != music_key, "production key isolates SFX from both stream decoders");
    expect(speech_key == path && music_key == path, "speech and music retain resolved-path keys");
    std::map<std::string, bool> streams;
    streams.emplace(sfx_key, false); // failed WAVE-only SFX request
    expect(streams.find(speech_key) == streams.end(), "failed SFX does not satisfy a speech cache lookup");
    streams.emplace(speech_key, true);
    expect(streams.at(speech_key) && !streams.at(sfx_key) && streams.size() == 2,
           "speech after failed SFX retains its successfully decoded stream");

    const std::map<std::string, bool> samples{{"ok-a", true}, {"ok-b", true}, {"absent", false}, {"bad", false}};
    const std::map<std::string, std::string> sample_failures{
        {"absent", "missing: EAWR-VFS-0002 [error] absent.wav: not found"}, {"bad", "undecodable: format 3"},
        {"speech", "missing: speech EAWR-VFS-0002 [error] speech-a.wav: not found"}, {"music", "missing: music EAWR-VFS-0003"},
        {"speech2", "missing: speech EAWR-VFS-0002 [error] speech-b.wav: not found"}, {"music2", "undecodable: MP3"},
        {"bad2", "undecodable: format 3"}};
    const auto counts = audio::sample_diagnostics(samples, sample_failures, [](bool valid) { return valid; });
    expect(counts.sfx_requested == 4 && counts.sfx_resolved == 2, "SFX totals exclude speech/music failures");
    expect(counts.files_missing_total == 4 && counts.files_undecodable_total == 3, "all file failure totals");
    expect(counts.files_missing == std::map<std::string, std::size_t>{{"EAWR-VFS-0002", 1},
        {"speech EAWR-VFS-0002", 2}, {"music EAWR-VFS-0003", 1}}, "every missing cause has its own total");
    expect(counts.files_undecodable == std::map<std::string, std::size_t>{{"format 3", 2}, {"MP3", 1}},
           "every undecodable cause has its own total");
    const auto empty = audio::sample_diagnostics(std::map<std::string, bool>{},
        std::map<std::string, std::string>{}, [](bool valid) { return valid; });
    expect(empty.sfx_requested == 0 && empty.sfx_resolved == 0 && empty.files_missing_total == 0
        && empty.files_undecodable_total == 0 && empty.files_missing.empty() && empty.files_undecodable.empty(),
        "empty maps have no phantom totals");
}

void falloff_and_listener() {
    const audio::Space3D space{1.5, 2.0, 60.0};
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
    // ST-A1: no stored settings uses the same quieter master as a fresh settings record.
    const audio::MixLevels defaults;
    expect(near(defaults.master, 0.37) && near(defaults.sfx, 0.75)
               && near(defaults.speech, 0.75) && near(defaults.music, 0.75),
           "fresh mix lowers only master, preserving the retail channel balance");
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

void sighting_service_timing() {
    // SND-42/64, SND-E54, WSU-03: per-object countdown, bypassed while the fade differs from target.
    audio::SightingService service;
    expect(service.tick(1, true, 1.0), "a new object is serviced on its first frame");
    for (int frame = 1; frame < 30; ++frame) expect(!service.tick(1, true, 1.0), "settled object waits out its countdown");
    expect(service.tick(1, true, 1.0), "settled object is serviced every 30 frames");
    // A second object first tracked 7 frames later keeps its own phase.
    for (int frame = 0; frame < 7; ++frame) (void)service.tick(1, true, 1.0);
    expect(service.tick(2, true, 1.0), "second object has its own first service");
    int serviced_one = 0, serviced_two = 0;
    for (int frame = 0; frame < 60; ++frame) {
        serviced_one += service.tick(1, true, 1.0) ? 1 : 0;
        serviced_two += service.tick(2, true, 1.0) ? 1 : 0;
    }
    expect(serviced_one == 2 && serviced_two == 2, "each object is serviced twice in 60 frames");
    // While the fade differs from its target, the object is serviced every frame and the countdown restarts.
    audio::SightingService fading;
    expect(fading.tick(3, true, 1.0), "fading: first service");
    expect(!fading.tick(3, true, 1.0), "settled: waits");
    int every = 0;
    for (int step = 2; step <= 8; ++step) every += fading.tick(3, true, step / 10.0) ? 1 : 0;
    expect(every == 7, "fading object is serviced every frame");
    bool again = false;
    for (int frame = 0; frame < 29; ++frame) again = again || fading.tick(3, true, 1.0);
    expect(!again, "countdown restarted at the last fading service");
    expect(fading.tick(3, true, 1.0), "then serviced 30 frames after it");
    fading.retain([](std::uint64_t) { return false; });
    expect(fading.tracked() == 0, "retain drops vanished objects");

    // Driven by the real fade: revealed from fog mid-countdown, the object is serviced every frame
    // while it eases in, so the cue request time follows the fade rather than a shared phase.
    space::UnitFade fade;
    audio::SightingService revealed;
    const std::vector<sim::EntityId> alive{9};
    int first_visible_service = -1, services_while_fading = 0;
    for (int frame = 0; frame < 120; ++frame) {
        const bool visible = frame >= 10;
        fade.advance(visible ? std::vector<sim::EntityId>{9} : std::vector<sim::EntityId>{}, alive, 1.0);
        const bool due = revealed.tick(9, visible, fade.opacity(9).value_or(0.0F));
        if (visible && due && first_visible_service < 0) first_visible_service = frame;
        if (visible && due && frame < 40) ++services_while_fading;
    }
    expect(first_visible_service >= 10 && first_visible_service <= 11, "reveal is serviced at once, not at the next shared phase");
    expect(services_while_fading > 10, "serviced every frame while the fade moves");
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
    {
        std::vector<std::string> problems;
        const std::array stock{Field{"Audio_Space_3D_Saturation_Distance_Mod", "1.5"},
            Field{"Audio_Space_3D_Rolloff_Distance_Mod", "2"}, Field{"Audio_Space_3D_Listener_Z_Pullback_Dist", "60"}};
        const std::array mod{Field{"Audio_Space_3D_Saturation_Distance_Mod", "3"},
            Field{"Audio_Space_3D_Rolloff_Distance_Mod", "4"}, Field{"Audio_Space_3D_Listener_Z_Pullback_Dist", "120"}};
        const auto original = audio::space_3d(stock, problems), changed = audio::space_3d(mod, problems);
        expect(original && changed && problems.empty(), "BA-10: authored space values validate");
        if (original && changed) {
            expect(audio::falloff_gain(1000, 300, *original) != audio::falloff_gain(1000, 300, *changed),
                "BA-11: perturbed XML factors change attenuation");
            expect(audio::listener_for_camera({0, 0, 500}, {0, 0, -1}, *original).position
                != audio::listener_for_camera({0, 0, 500}, {0, 0, -1}, *changed).position,
                "BA-12: perturbed XML pullback changes listener");
        }
        const auto missing = audio::space_3d({}, problems);
        expect(!missing && problems.size() == 3 && problems.front().starts_with("EAWR-AUDIO-0301"),
            "missing spatial tags diagnose without stock defaults");
        audio::SfxEvent sound;
        sound.name = "degraded-spatial";
        sound.samples = {"sample.wav"};
        audio::Voices voices;
        audio::Random random(1);
        const auto started = voices.start({&sound, missing ? std::optional<audio::Vec3>(audio::Vec3{1, 2, 3}) : std::nullopt}, {}, random);
        expect(started.result == Result::playing && !audio::Voices::is_3d_voice(started.voice),
            "missing spatial settings keep playback in the 2D pool");
        problems.clear();
        const auto defaults = audio::ambient_moving_delays("", "", "unit", problems);
        expect(defaults && *defaults == std::array{150, 300} && problems.empty(),
            "SND-46: missing ambient delays use sourced defaults without diagnostics");
        problems.clear();
        const auto delays = audio::ambient_moving_delays("2", "4", "unit", problems);
        expect(delays && *delays == std::array{60, 120} && problems.empty(), "SND-46: XML delays change reschedule bounds");
        expect(!audio::ambient_moving_delays("0", "4", "unit", problems), "SND-46: invalid delays do not silently default");
    }
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
    hardpoint_loss();
    event_lifecycle();
    stolen_event_completion();
    wave_files();
    sample_paths();
    stream_selection_cache_and_diagnostics();
    falloff_and_listener();
    voice_rules();
    two_dimensional_pool();
    category_admission_and_gain();
    attached_voice_positions();
    speakers();
    music();
    announcements();
    sighting_service_timing();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "audio sfx contracts passed\n";
    return 0;
}
