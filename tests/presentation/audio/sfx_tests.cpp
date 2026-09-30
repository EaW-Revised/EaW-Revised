// P2-21 (#84): FoC's sound-event rules (presentation::audio, docs/behaviour/battle-audio.md).
// Synthetic inputs only; no game data.
#include "eawr/presentation/audio/sfx.hpp"

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

audio::SfxEvent event(const std::string& name, const bool three_d, const int instances, const int priority = 3) {
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
}

} // namespace

int main() {
    registry_and_presets();
    wave_files();
    falloff_and_listener();
    voice_rules();
    speakers();
    music();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "audio sfx contracts passed\n";
    return 0;
}
