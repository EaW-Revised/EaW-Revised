#include "eawr/presentation/audio/sfx.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

namespace eawr::presentation::audio {
namespace {

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.remove_suffix(1);
    return text;
}

[[nodiscard]] bool equals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const char x, const char y) {
        return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
    });
}

[[nodiscard]] std::optional<bool> boolean(std::string_view text) {
    text = trim(text);
    if (equals(text, "yes") || equals(text, "true") || text == "1") return true;
    if (equals(text, "no") || equals(text, "false") || text == "0") return false;
    return std::nullopt;
}

[[nodiscard]] std::optional<double> number(std::string_view text) {
    text = trim(text);
    // Leading number only, as the integer fields (std::from_chars stops at the first non-digit).
    double value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end == text.data()) return std::nullopt;
    return value;
}

[[nodiscard]] double squared(const Vec3& a, const Vec3& b) {
    const double x = a[0] - b[0];
    const double y = a[1] - b[1];
    const double z = a[2] - b[2];
    return x * x + y * y + z * z;
}

// Applies one child of an <SFXEvent> (BA-02). False for a tag the event does not use.
bool apply(SfxEvent& event, const std::string& tag, const std::string_view value, std::vector<std::string>& problems) {
    const auto integer = [&](int& field, const int low, const int high) {
        if (const auto parsed = leading_integer(value)) {
            field = std::clamp(*parsed, low, high);
        } else {
            problems.push_back(event.name + ": " + tag + " '" + std::string(value) + "' is not an integer");
        }
    };
    const auto flag = [&](bool& field) {
        if (const auto parsed = boolean(value)) {
            field = *parsed;
        } else {
            problems.push_back(event.name + ": " + tag + " '" + std::string(value) + "' is not Yes or No");
        }
    };
    const auto real = [&](double& field) {
        if (const auto parsed = number(value)) {
            field = *parsed;
        } else {
            problems.push_back(event.name + ": " + tag + " '" + std::string(value) + "' is not a number");
        }
    };
    const std::string key = upper(tag);
    if (key == "IS_PRESET") flag(event.preset);
    else if (key == "IS_3D") flag(event.is_3d);
    else if (key == "IS_2D") {
        bool two{};
        flag(two);
        event.is_3d = !two;
    } else if (key == "IS_GUI") flag(event.gui);
    else if (key == "IS_HUD_VO") flag(event.hud_vo);
    else if (key == "IS_UNIT_RESPONSE_VO") flag(event.unit_response_vo);
    else if (key == "IS_AMBIENT_VO") flag(event.ambient_vo);
    else if (key == "LOCALIZE") flag(event.localized);
    else if (key == "PLAY_SEQUENTIALLY") flag(event.play_sequentially);
    else if (key == "SAMPLES") event.samples = split_list(value);
    else if (key == "PRE_SAMPLES") event.pre_samples = split_list(value);
    else if (key == "POST_SAMPLES") event.post_samples = split_list(value);
    else if (key == "PRIORITY") integer(event.priority, 1, 5);
    else if (key == "PROBABILITY") integer(event.probability, 0, 100);
    else if (key == "PLAY_COUNT") integer(event.play_count, -1, std::numeric_limits<int>::max());
    else if (key == "MAX_INSTANCES") integer(event.max_instances, 0, std::numeric_limits<int>::max());
    else if (key == "MIN_VOLUME") integer(event.min_volume, 0, 100);
    else if (key == "MAX_VOLUME") integer(event.max_volume, 0, 100);
    else if (key == "MIN_PITCH") integer(event.min_pitch, 50, 200);
    else if (key == "MAX_PITCH") integer(event.max_pitch, 50, 200);
    else if (key == "MIN_PREDELAY") integer(event.min_predelay_ms, 0, std::numeric_limits<int>::max());
    else if (key == "MAX_PREDELAY") integer(event.max_predelay_ms, 0, std::numeric_limits<int>::max());
    else if (key == "VOLUME_SATURATION_DISTANCE") real(event.saturation_distance);
    else if (key == "LOOP_FADE_IN_SECONDS") real(event.loop_fade_in_seconds);
    else if (key == "LOOP_FADE_OUT_SECONDS") real(event.loop_fade_out_seconds);
    else if (key == "KILLS_PREVIOUS_OBJECT_SFX") flag(event.kills_previous_object_sfx);
    else if (key == "OVERLAP_TEST") event.overlap_test = upper(trim(value));
    else if (key == "CHAINED_SFXEVENT") event.chained = std::string(trim(value));
    else if (key == "TEXT_ID") {
        // Subtitle text; not audio.
    } else {
        return false;
    }
    return true;
}

} // namespace

std::string upper(const std::string_view text) {
    std::string result(text);
    for (char& c : result) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return result;
}

std::optional<int> leading_integer(std::string_view text) {
    text = trim(text);
    int value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end == text.data()) return std::nullopt;
    return value;
}

std::vector<std::string> split_list(const std::string_view text) {
    std::vector<std::string> items;
    std::string current;
    for (const char c : text) {
        if (c == ',' || std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (!current.empty()) items.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) items.push_back(std::move(current));
    return items;
}

void SfxRegistry::add(const std::string_view name, const std::span<const Field> fields,
                      std::vector<std::string>& problems) {
    SfxEvent event;
    event.name = std::string(name);
    for (const auto& [tag, raw] : fields) {
        const std::string_view value = trim(raw);
        // BA-02: empty and TBD values are skipped.
        if (value.empty() || value == "TBD") continue;
        if (equals(tag, "Use_Preset")) {
            const SfxEvent* preset = find(value);
            if (preset == nullptr) {
                problems.push_back(event.name + ": Use_Preset '" + std::string(value) + "' is not a known preset");
                continue;
            }
            // The preset's every field, keeping the event's own name; the copy is no preset.
            const std::string own = event.name;
            event = *preset;
            event.name = own;
            event.preset = false;
            continue;
        }
        if (!apply(event, tag, value, problems)) {
            // Unknown tags are reported by the loader's schema pass, not here.
        }
    }
    if (event.max_volume < event.min_volume) event.max_volume = event.min_volume;
    if (event.max_pitch < event.min_pitch) event.max_pitch = event.min_pitch;
    if (event.max_predelay_ms < event.min_predelay_ms) event.max_predelay_ms = event.min_predelay_ms;
    events_.insert_or_assign(upper(name), std::move(event));
}

const SfxEvent* SfxRegistry::find(const std::string_view name) const {
    const auto found = events_.find(upper(trim(name)));
    return found == events_.end() ? nullptr : &found->second;
}

std::optional<WavPcm> parse_wav(const std::span<const std::byte> bytes, std::string& error) {
    const auto u32 = [&](const std::size_t at) {
        std::uint32_t value{};
        for (std::size_t i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[at + i]) << (8U * i);
        return value;
    };
    const auto u16 = [&](const std::size_t at) {
        return static_cast<std::uint16_t>(std::to_integer<std::uint32_t>(bytes[at])
                                          | (std::to_integer<std::uint32_t>(bytes[at + 1]) << 8U));
    };
    const auto tag = [&](const std::size_t at, const char* name) {
        return std::memcmp(bytes.data() + at, name, 4) == 0;
    };
    if (bytes.size() < 12 || !tag(0, "RIFF") || !tag(8, "WAVE")) {
        error = "not a RIFF WAVE file";
        return std::nullopt;
    }
    std::optional<WavPcm> result;
    bool format_seen = false;
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const std::uint32_t size = u32(at + 4);
        const std::size_t body = at + 8;
        if (size > bytes.size() - body) {
            error = "a chunk runs past the end of the file";
            return std::nullopt;
        }
        if (tag(at, "fmt ")) {
            if (size < 16) {
                error = "fmt chunk too short";
                return std::nullopt;
            }
            const std::uint16_t format = u16(body);
            const std::uint16_t bits = u16(body + 14);
            if (format != 1 || bits != 16) {
                error = "format " + std::to_string(format) + " with " + std::to_string(bits) + " bits (16-bit PCM expected)";
                return std::nullopt;
            }
            result = WavPcm{u32(body + 4), u16(body + 2), {}};
            format_seen = true;
            if (result->channels == 0 || result->channels > 2 || result->sample_rate == 0) {
                error = "unsupported channel count or rate";
                return std::nullopt;
            }
        } else if (tag(at, "data")) {
            if (!format_seen) {
                error = "data before fmt";
                return std::nullopt;
            }
            const std::size_t frame = 2U * result->channels;
            result->data = bytes.subspan(body, size - size % frame);
            return result;
        }
        at = body + size + (size & 1U);
    }
    error = format_seen ? "no data chunk" : "no fmt chunk";
    return std::nullopt;
}

double falloff_gain(const double distance, const double saturation_distance, const Space3D& space) {
    const double minimum = std::max(1.0, saturation_distance * space.saturation_factor);
    if (distance <= minimum) return 1.0;
    if (distance > space.max_distance) return 0.0;
    return minimum / (minimum + space.rolloff_factor * (distance - minimum));
}

Listener listener_for_camera(const Vec3& camera, const Vec3& forward, const Space3D& space) {
    Listener listener;
    const double height = std::abs(camera[2] - space.listener_z);
    const double length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    const Vec3 unit = length > 0.0 ? Vec3{forward[0] / length, forward[1] / length, forward[2] / length} : Vec3{0.0, 0.0, -1.0};
    // BA-12: the view line's point at z = listener_z (a level view keeps the
    // camera's x and y there, then the pull-back below places it).
    Vec3 point{camera[0], camera[1], space.listener_z};
    if (std::abs(unit[2]) > 1.0e-9) {
        const double t = (space.listener_z - camera[2]) / unit[2];
        point = {camera[0] + unit[0] * t, camera[1] + unit[1] * t, space.listener_z};
    }
    if (std::sqrt(squared(point, camera)) > height) {
        point = {camera[0] + unit[0] * height, camera[1] + unit[1] * height, camera[2] + unit[2] * height};
    }
    listener.position = point;
    const double flat = std::sqrt(unit[0] * unit[0] + unit[1] * unit[1]);
    if (flat > 1.0e-9) listener.ahead = {unit[0] / flat, unit[1] / flat, 0.0};
    return listener;
}

int Random::between(const int low, const int high) {
    if (high <= low) return low;
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 7U;
    state_ ^= state_ << 17U;
    const auto span = static_cast<std::uint64_t>(static_cast<std::int64_t>(high) - low + 1);
    return low + static_cast<int>(state_ % span);
}

std::string_view to_string(const Start::Result result) noexcept {
    switch (result) {
    case Start::Result::playing: return "playing";
    case Start::Result::preset: return "preset";
    case Start::Result::no_instances: return "no_instances";
    case Start::Result::instance_limit: return "instance_limit";
    case Start::Result::overlap: return "overlap";
    case Start::Result::probability: return "probability";
    case Start::Result::hidden: return "hidden";
    case Start::Result::no_samples: return "no_samples";
    case Start::Result::no_voice: return "no_voice";
    }
    return "playing";
}

Start Voices::start(const Request& request, const Vec3& listener, Random& random) {
    Start start;
    const SfxEvent* event = request.event;
    const auto refuse = [&start](const Start::Result result) {
        start.result = result;
        return start;
    };
    if (event == nullptr || event->preset || event->play_count == 0) return refuse(Start::Result::preset);
    // BA-04: Max_Instances 0 never plays; a 2D event at its limit is refused, a 3D one goes on to
    // the distance cull below.
    if (event->max_instances == 0) return refuse(Start::Result::no_instances);
    const bool three_d = event->is_3d && request.position.has_value();
    std::size_t instances = 0;
    std::optional<std::size_t> farthest;
    double farthest_distance = -1.0;
    for (std::size_t index = 0; index < voices_.size(); ++index) {
        if (voices_[index].event != event) continue;
        ++instances;
        if (is_3d_voice(index)) {
            const double distance = squared(voices_[index].position, listener);
            if (!farthest || farthest_distance < distance) {
                farthest = index;
                farthest_distance = distance;
            }
        }
    }
    if (!three_d && instances >= static_cast<std::size_t>(event->max_instances)) {
        return refuse(Start::Result::instance_limit);
    }
    // BA-05: one event with the same Overlap_Test playing keeps this one silent.
    if (!event->overlap_test.empty()) {
        for (const Voice& voice : voices_) {
            if (voice.event != nullptr && voice.event->overlap_test == event->overlap_test) {
                return refuse(Start::Result::overlap);
            }
        }
    }
    // BA-06: a Probability under 100 is a roll of 1..100.
    if (event->probability < 100 && random.between(1, 100) > event->probability) return refuse(Start::Result::probability);
    // BA-07: the sound of a unit the local player does not see does not play.
    if (request.hidden) return refuse(Start::Result::hidden);
    if (event->samples.empty()) return refuse(Start::Result::no_samples);
    // BA-08: volume and pitch drawn from their ranges; the sample in turn or at random.
    start.volume = random.between(event->min_volume, event->max_volume) / 100.0;
    start.pitch = random.between(event->min_pitch, event->max_pitch) / 100.0;
    std::size_t pick = 0;
    if (event->play_sequentially) {
        std::size_t& next = sequential_[event];
        pick = next % event->samples.size();
        next = (pick + 1) % event->samples.size();
    } else {
        pick = static_cast<std::size_t>(random.between(0, static_cast<int>(event->samples.size()) - 1));
    }
    start.sample = event->samples[pick];
    if (!three_d) {
        for (std::size_t index = voices_3d; index < voices_.size(); ++index) {
            if (voices_[index].event == nullptr) {
                voices_[index] = {event, {}};
                start.voice = index;
                return start;
            }
        }
        return refuse(Start::Result::no_voice);
    }
    const Vec3 position = *request.position;
    const double distance = squared(position, listener);
    // BA-05 (3D): at Max_Instances the new instance plays only when it is not farther than the
    // farthest playing one, which it stops.
    if (instances >= static_cast<std::size_t>(event->max_instances)) {
        if (!farthest || distance > farthest_distance) return refuse(Start::Result::instance_limit);
        voices_[*farthest] = {};
        start.stopped = farthest;
    }
    for (std::size_t index = 0; index < voices_3d; ++index) {
        if (voices_[index].event == nullptr) {
            voices_[index] = {event, position};
            start.voice = index;
            return start;
        }
    }
    // BA-09: every 3D voice busy. The candidate is the least important playing sound, the farthest
    // among equals; like FoC, a switch to a less important candidate keeps the distance of the one
    // it replaced. It is stopped when it is no more important than the new sound and farther.
    std::optional<std::size_t> candidate;
    double candidate_distance = 0.0;
    for (std::size_t index = 0; index < voices_3d; ++index) {
        const double voice_distance = squared(voices_[index].position, listener);
        if (!candidate) {
            candidate = index;
            candidate_distance = voice_distance;
        } else if (voices_[*candidate].event->priority < voices_[index].event->priority) {
            candidate = index;
        } else if (voices_[index].event->priority == voices_[*candidate].event->priority
                   && candidate_distance < voice_distance) {
            candidate = index;
            candidate_distance = voice_distance;
        }
    }
    if (!candidate || voices_[*candidate].event->priority < event->priority || candidate_distance <= distance) {
        return refuse(Start::Result::no_voice);
    }
    voices_[*candidate] = {event, position};
    start.voice = *candidate;
    start.stopped = candidate;
    return start;
}

void Voices::finished(const std::size_t voice) {
    if (voice < voices_.size()) voices_[voice] = {};
}

const SfxEvent* Voices::playing(const std::size_t voice) const {
    return voice < voices_.size() ? voices_[voice].event : nullptr;
}

std::size_t Voices::playing_count() const {
    return static_cast<std::size_t>(
        std::count_if(voices_.begin(), voices_.end(), [](const Voice& voice) { return voice.event != nullptr; }));
}

std::optional<std::size_t> speaker(const std::span<const RankedUnit> units, const std::span<const std::string> rankings) {
    std::optional<std::size_t> best;
    int best_category = -1;
    int best_ranking = -1;
    for (const RankedUnit& unit : units) {
        int category = -1;
        for (std::size_t index = 0; index < rankings.size() && category < 0; ++index) {
            for (const std::string& name : unit.categories) {
                if (equals(name, rankings[index])) {
                    category = static_cast<int>(index);
                    break;
                }
            }
        }
        if (category < 0) continue;
        const int ranking = unit.ranking.value_or(25);
        // BA-20: a better category wins; otherwise a lower
        // Ranking_In_Category wins whatever its category (FoC's rule as written).
        if (best_category == -1 || category < best_category) {
            best_category = category;
            best_ranking = ranking;
            best = unit.index;
        } else if (best_ranking == -1 || ranking < best_ranking) {
            best_ranking = ranking;
            best = unit.index;
        }
    }
    return best;
}

MusicEvent parse_music_event(const std::string_view name, const std::span<const Field> fields) {
    MusicEvent event;
    event.name = std::string(name);
    for (const auto& [tag, raw] : fields) {
        const std::string_view value = trim(raw);
        const std::string key = upper(tag);
        if (key == "FILES") event.files = split_list(value);
        else if (key == "VOLUME_PERCENT") {
            if (const auto parsed = leading_integer(value)) event.volume = std::clamp(*parsed, 0, 100) / 100.0;
        } else if (key == "FADE_IN_SECONDS") {
            if (const auto parsed = number(value)) event.fade_in_seconds = std::max(0.0, *parsed);
        } else if (key == "FADE_OUT_PREVIOUS_SECONDS") {
            if (const auto parsed = number(value)) event.fade_out_previous_seconds = std::max(0.0, *parsed);
        } else if (key == "LOOP") {
            event.loop = boolean(value).value_or(false);
        }
    }
    return event;
}

MusicDirector::MusicDirector(std::vector<const MusicEvent*> ambient, std::vector<const MusicEvent*> battle,
                             const std::uint64_t peace_ticks)
    : ambient_(std::move(ambient)), battle_(std::move(battle)), peace_ticks_(peace_ticks) {}

std::optional<MusicDirector::Cue> MusicDirector::start(const Mode mode, Random& random) {
    const std::vector<const MusicEvent*>& list = mode == Mode::battle ? battle_ : ambient_;
    mode_ = mode;
    if (list.empty()) {
        current_ = nullptr;
        return std::nullopt;
    }
    const MusicEvent* chosen = list[static_cast<std::size_t>(random.between(0, static_cast<int>(list.size()) - 1))];
    // BA-40: the same event already playing goes on.
    if (chosen == current_) return std::nullopt;
    current_ = chosen;
    if (chosen->files.empty()) return std::nullopt;
    std::size_t& next = next_file_[chosen];
    const std::size_t index = next % chosen->files.size();
    next = (index + 1) % chosen->files.size();
    return Cue{chosen, chosen->files[index], mode};
}

std::optional<MusicDirector::Cue> MusicDirector::begin(Random& random) {
    quiet_ = 0;
    return start(Mode::ambient, random);
}

std::optional<MusicDirector::Cue> MusicDirector::tick(const bool attack, Random& random) {
    std::optional<Cue> cue;
    // BA-43: the quiet count grows each frame; in battle it ends the battle
    // music once it reaches the peace time.
    ++quiet_;
    if (mode_ == Mode::battle && quiet_ >= peace_ticks_) cue = start(Mode::ambient, random);
    // BA-42: battle music unless it plays; the quiet count restarts.
    if (attack) {
        if (mode_ != Mode::battle) cue = start(Mode::battle, random);
        quiet_ = 0;
    }
    return cue;
}

std::optional<MusicDirector::Cue> MusicDirector::track_ended() {
    if (current_ == nullptr || !current_->loop || current_->files.empty()) {
        current_ = nullptr;
        return std::nullopt;
    }
    std::size_t& next = next_file_[current_];
    const std::size_t index = next % current_->files.size();
    next = (index + 1) % current_->files.size();
    return Cue{current_, current_->files[index], mode_};
}

} // namespace eawr::presentation::audio
