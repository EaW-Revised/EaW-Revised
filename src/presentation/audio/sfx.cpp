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
    else if (key == "MIN_PAN2D") integer(event.min_pan, 0, 100);
    else if (key == "MAX_PAN2D") integer(event.max_pan, 0, 100);
    else if (key == "MIN_PREDELAY") integer(event.min_predelay_ms, 0, std::numeric_limits<int>::max());
    else if (key == "MAX_PREDELAY") integer(event.max_predelay_ms, 0, std::numeric_limits<int>::max());
    else if (key == "MIN_POSTDELAY") integer(event.min_postdelay_ms, 0, std::numeric_limits<int>::max());
    else if (key == "MAX_POSTDELAY") integer(event.max_postdelay_ms, 0, std::numeric_limits<int>::max());
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
    case Start::Result::category_disabled: return "category_disabled";
    case Start::Result::cinematic_feedback: return "cinematic_feedback";
    case Start::Result::hud_speech: return "hud_speech";
    case Start::Result::tutorial_hud: return "tutorial_hud";
    case Start::Result::demo_dialog: return "demo_dialog";
    case Start::Result::delete_pending: return "delete_pending";
    case Start::Result::zero_gain: return "zero_gain";
    case Start::Result::attached_loop: return "attached_loop";
    }
    return "playing";
}

double category_gain(const SfxEvent& event, const MixLevels& levels) noexcept {
    return std::clamp(levels.master, 0.0, 1.0)
        * std::clamp(event.localized ? levels.speech : levels.sfx, 0.0, 1.0);
}

bool pauses_with_game(const SfxEvent& event, const bool spatial) noexcept {
    return spatial || event.play_count == -1;
}

Start Voices::start(const Request& request, const Vec3& listener, Random& random, const Admission& admission) {
    Start start;
    const SfxEvent* event = request.event;
    const auto refuse = [&start](const Start::Result result) {
        start.result = result;
        return start;
    };
    if (event == nullptr || event->preset || event->play_count == 0) return refuse(Start::Result::preset);
    // SND-16/17: forcing bypasses ordinary admission, never disabled categories.
    if (event == admission.negative_feedback && admission.cinematic) return refuse(Start::Result::cinematic_feedback);
    if ((!admission.localized && event->localized) || (!admission.unit_response && event->unit_response_vo)
        || (!admission.hud && event->hud_vo) || (!admission.ambient && event->ambient_vo)) {
        return refuse(Start::Result::category_disabled);
    }
    if (!request.forced) {
        if (admission.demo_dialog && (!event->gui || event == admission.negative_feedback)) return refuse(Start::Result::demo_dialog);
        if (event->hud_vo && admission.tutorial_tactical) return refuse(Start::Result::tutorial_hud);
        if (event->hud_vo && admission.speech_stream) return refuse(Start::Result::hud_speech);
    }
    // BA-04: Max_Instances 0 never plays; a 2D event at its limit is refused, a 3D one goes on to
    // the distance cull below.
    if (!request.forced && event->max_instances == 0) return refuse(Start::Result::no_instances);
    const bool three_d = event->is_3d && request.position.has_value();
    std::size_t instances = 0;
    for (const Voice& voice : voices_) {
        if (voice.event == event && !voice.fading) ++instances;
    }
    if (!request.forced && !three_d && instances >= static_cast<std::size_t>(event->max_instances)) {
        return refuse(Start::Result::instance_limit);
    }
    // BA-05: one event with the same Overlap_Test playing keeps this one silent.
    if (!request.forced && !event->overlap_test.empty()) {
        for (const Voice& voice : voices_) {
            if (voice.event != nullptr && !voice.fading && voice.event->overlap_test == event->overlap_test) {
                return refuse(Start::Result::overlap);
            }
        }
    }
    // BA-06: a Probability under 100 is a roll of 1..100.
    if (!request.forced && event->probability < 100 && random.between(1, 100) > event->probability) return refuse(Start::Result::probability);
    // BA-07: the sound of a unit the local player does not see does not play.
    if (request.delete_pending) return refuse(Start::Result::delete_pending);
    if (request.hidden && !admission.story_cinematic) return refuse(Start::Result::hidden);
    start.admitted = true;
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
    if (!three_d && start.volume <= 0.0) return refuse(Start::Result::zero_gain);
    return allocate(request, listener, std::move(start));
}

Start Voices::allocate(const Request& request, const Vec3& listener, Start start) {
    const auto* event = request.event;
    const bool three_d = event->is_3d && request.position.has_value();
    const auto refuse = [&start](const Start::Result result) {
        start.result = result;
        return start;
    };
    if (!three_d && start.volume <= 0.0) return refuse(Start::Result::zero_gain);
    std::size_t instances = 0;
    std::optional<std::size_t> farthest;
    double farthest_distance = -1.0;
    for (std::size_t index = 0; index < voices_.size(); ++index) {
        if (voices_[index].event != event || voices_[index].fading) continue;
        ++instances;
        if (is_3d_voice(index)) {
            const double distance = squared(voices_[index].position, listener);
            if (!farthest || farthest_distance < distance) {
                farthest = index;
                farthest_distance = distance;
            }
        }
    }
    const double started_at = request.started_at.value_or(static_cast<double>(starts_++));
    if (!three_d) {
        for (std::size_t index = voices_3d; index < voices_.size(); ++index) {
            if (voices_[index].event == nullptr) {
                voices_[index] = {event, {}, started_at};
                start.voice = index;
                return start;
            }
        }
        // SND-07: least important, then oldest; equal clocks retain the first slot.
        std::size_t candidate = voices_3d;
        for (std::size_t index = voices_3d + 1; index < voices_.size(); ++index) {
            const Voice& current = voices_[candidate];
            const Voice& next = voices_[index];
            if (next.event->priority > current.event->priority
                || (next.event->priority == current.event->priority && next.started_at < current.started_at)) {
                candidate = index;
            }
        }
        if (voices_[candidate].event->priority < event->priority) return refuse(Start::Result::no_voice);
        voices_[candidate] = {event, {}, started_at};
        start.voice = candidate;
        start.stopped = candidate;
        return start;
    }
    const Vec3 position = *request.position;
    const double distance = squared(position, listener);
    // BA-05 (3D): at Max_Instances the new instance plays only when it is not farther than the
    // farthest playing one, which it stops.
    if (!request.forced && instances >= static_cast<std::size_t>(event->max_instances)) {
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

void Voices::set_position(const std::size_t voice, const Vec3& position) {
    if (is_3d_voice(voice) && voices_[voice].event != nullptr) voices_[voice].position = position;
}

void Voices::set_fading(const std::size_t voice) {
    if (voice < voices_.size()) voices_[voice].fading = true;
}

double attached_gain(const bool silent, const bool model_hidden, const bool fogged, const bool story_cinematic) noexcept {
    return silent || model_hidden || (fogged && !story_cinematic) ? 0.0 : 1.0;
}

EventQueue::Entry* EventQueue::find(const Handle handle) {
    const auto found = handles_.find(handle);
    return found == handles_.end() ? nullptr : found->second;
}

std::size_t EventQueue::instances(const SfxEvent* event) const {
    return static_cast<std::size_t>(std::count_if(entries_.begin(), entries_.end(), [event](const Entry& entry) {
        return entry.request.voice.event == event && !entry.fading && entry.stage != Stage::done;
    }));
}

EventQueue::Accepted EventQueue::admit(const Request& request, Random& random, const Admission& admission) {
    const auto* event = request.voice.event;
    const auto refuse = [](const Start::Result result) { return Accepted{result, std::nullopt}; };
    if (!event || event->preset || event->play_count == 0) return refuse(Start::Result::preset);
    if (event == admission.negative_feedback && admission.cinematic) return refuse(Start::Result::cinematic_feedback);
    if ((!admission.localized && event->localized) || (!admission.unit_response && event->unit_response_vo)
        || (!admission.hud && event->hud_vo) || (!admission.ambient && event->ambient_vo)) {
        return refuse(Start::Result::category_disabled);
    }
    if (!request.voice.forced) {
        if (admission.demo_dialog && (!event->gui || event == admission.negative_feedback)) return refuse(Start::Result::demo_dialog);
        if (event->hud_vo && admission.tutorial_tactical) return refuse(Start::Result::tutorial_hud);
        if (event->hud_vo && admission.speech_stream) return refuse(Start::Result::hud_speech);
        if (event->max_instances == 0) return refuse(Start::Result::no_instances);
        for (const auto& entry : entries_) {
            if (entry.fading || entry.stage == Stage::done) continue;
            if (entry.request.voice.event == event && request.attachment != 0
                && entry.request.attachment == request.attachment
                && (event->play_count == -1 || request.continuous_main)) {
                return refuse(Start::Result::attached_loop);
            }
        }
        if (!(event->is_3d && request.voice.position) && instances(event) >= static_cast<std::size_t>(event->max_instances)) {
            return refuse(Start::Result::instance_limit);
        }
        if (!event->overlap_test.empty()) {
            for (const auto& entry : entries_) {
                if (!entry.fading && entry.stage != Stage::done
                    && entry.request.voice.event->overlap_test == event->overlap_test) return refuse(Start::Result::overlap);
            }
        }
        if (event->probability < 100 && random.between(1, 100) > event->probability) return refuse(Start::Result::probability);
    }
    if (request.voice.delete_pending) return refuse(Start::Result::delete_pending);
    if (request.voice.hidden && !admission.story_cinematic) return refuse(Start::Result::hidden);
    // SND-05: synchronization occurs on admission; stage selection advances independent cursors later.
    if (event->play_sequentially) {
        auto& cursor = cursors_[event];
        if (event->pre_samples.size() == event->samples.size()) cursor.pre = cursor.main;
        if (event->post_samples.size() == event->samples.size()) cursor.post = cursor.main;
    }
    Entry entry;
    entry.handle = next_++;
    entry.request = request;
    const Handle handle = entry.handle;
    if (event->is_3d && request.voice.position) {
        entries_.push_back(std::move(entry));
        handles_[handle] = &entries_.back();
    } else {
        entries_.push_front(std::move(entry));
        handles_[handle] = &entries_.front();
    }
    return {Start::Result::playing, handle};
}

std::vector<EventQueue::Chain> EventQueue::service(const double elapsed_ms, const bool paused,
    const Vec3& listener, Random& random, const SfxRegistry& registry, const Backend& backend) {
    std::vector<Entry*> order;
    order.reserve(entries_.size());
    for (auto& entry : entries_) order.push_back(&entry);
    std::stable_sort(order.begin(), order.end(), [&listener](const Entry* a, const Entry* b) {
        const auto distance = [&listener](const Entry* entry) {
            return entry->request.voice.event->is_3d && entry->request.voice.position
                ? squared(*entry->request.voice.position, listener) : -1.0;
        };
        return distance(a) < distance(b);
    });
    std::vector<Chain> chains;
    for (auto* pointer : order) {
        auto& entry = *pointer;
        const auto* event = entry.request.voice.event;
        const bool spatial = event->is_3d && entry.request.voice.position;
        if (entry.stage == Stage::done || (paused && pauses_with_game(*event, spatial))) continue;
        const double delta = std::max(0.0, elapsed_ms);
        if (entry.fading) {
            entry.fade_remaining_ms = std::max(0.0, entry.fade_remaining_ms - delta);
            if (entry.fade_remaining_ms == 0.0 || (entry.voice && !backend.playing(*entry.voice))) {
                if (entry.voice) backend.stop(*entry.voice);
                entry.voice.reset();
                entry.stage = Stage::done;
            }
            continue;
        }
        if (entry.voice) {
            if (backend.playing(*entry.voice)) continue;
            backend.stop(*entry.voice);
            entry.voice.reset();
            entry.stage = entry.stage == Stage::pre ? Stage::main
                : entry.stage == Stage::main ? Stage::post : Stage::postdelay;
            if (entry.stage == Stage::postdelay) entry.delay_ms = entry.postdelay_ms;
            continue;
        }
        switch (entry.stage) {
        case Stage::initialize:
            entry.values.volume = random.between(event->min_volume, event->max_volume) / 100.0;
            entry.values.pan = random.between(event->min_pan, event->max_pan) / 100.0;
            entry.values.pitch = random.between(event->min_pitch, event->max_pitch) / 100.0;
            entry.delay_ms = random.between(event->min_predelay_ms, event->max_predelay_ms);
            entry.postdelay_ms = random.between(event->min_postdelay_ms, event->max_postdelay_ms);
            // SND-08: no authored pre sample means the next service starts main directly.
            entry.stage = entry.delay_ms > 0.0 ? Stage::predelay
                : event->pre_samples.empty() ? Stage::main : Stage::pre;
            break;
        case Stage::predelay:
            entry.delay_ms = std::max(0.0, entry.delay_ms - delta);
            if (entry.delay_ms == 0.0) entry.stage = event->pre_samples.empty() ? Stage::main : Stage::pre;
            break;
        case Stage::pre:
        case Stage::main:
        case Stage::post: {
            const auto& samples = entry.stage == Stage::pre ? event->pre_samples
                : entry.stage == Stage::main ? event->samples : event->post_samples;
            if (samples.empty()) {
                entry.stage = entry.stage == Stage::pre ? Stage::main
                    : entry.stage == Stage::main ? Stage::post : Stage::postdelay;
                if (entry.stage == Stage::postdelay) entry.delay_ms = entry.postdelay_ms;
                break;
            }
            auto& cursors = cursors_[event];
            auto& cursor = entry.stage == Stage::pre ? cursors.pre : entry.stage == Stage::main ? cursors.main : cursors.post;
            const auto pick = event->play_sequentially ? cursor % samples.size()
                : static_cast<std::size_t>(random.between(0, static_cast<int>(samples.size()) - 1));
            if (event->play_sequentially) cursor = (pick + 1) % samples.size();
            entry.values.sample = samples[pick];
            entry.values.admitted = true;
            const bool continuous = entry.stage == Stage::main && (event->play_count == -1 || entry.request.continuous_main);
            const auto start = backend.start({entry.handle, entry.request, entry.values, entry.stage, continuous});
            if (start.stopped) {
                // SND-08/18: release the stolen slot, then complete its former event loop.
                for (auto& previous : entries_) {
                    if (previous.handle != entry.handle && previous.voice == start.stopped) {
                        previous.voice.reset();
                        previous.stage = previous.fading ? Stage::done : Stage::complete;
                    }
                }
            }
            if (start.result == Start::Result::playing) entry.voice = start.voice;
            else {
                // SND-08/09/18: admitted allocation failures retain completion and chain precedence.
                entry.stage = Stage::complete;
            }
            break;
        }
        case Stage::postdelay:
            entry.delay_ms = std::max(0.0, entry.delay_ms - delta);
            if (entry.delay_ms == 0.0) entry.stage = Stage::complete;
            break;
        case Stage::complete:
            ++entry.completed;
            ++completed_loops_;
            if (event->play_count == -1 || entry.completed < event->play_count) entry.stage = Stage::initialize;
            else {
                const auto* authored = registry.find(event->chained);
                const auto* chained = entry.chained && !entry.chained->is_3d ? entry.chained : authored;
                if (!event->is_3d && chained && !chained->is_3d) {
                    chains.push_back({chained, entry.request.attachment, chained == entry.chained, entry.attack});
                }
                entry.stage = Stage::done;
            }
            break;
        case Stage::done: break;
        }
    }
    std::erase_if(entries_, [this](const Entry& entry) {
        if (entry.stage != Stage::done) return false;
        handles_.erase(entry.handle);
        return true;
    });
    return chains;
}

void EventQueue::stop(const Handle handle, const Backend& backend, const double fade_seconds) {
    auto* entry = find(handle);
    if (!entry || entry->stage == Stage::done || entry->fading) return;
    entry->chained = nullptr;
    if (entry->voice && fade_seconds > 0.0) {
        entry->fading = true;
        entry->fade_ms = fade_seconds * 1000.0;
        entry->fade_remaining_ms = entry->fade_ms;
        if (backend.fading) backend.fading(*entry->voice);
    } else {
        if (entry->voice) backend.stop(*entry->voice);
        entry->voice.reset();
        entry->stage = Stage::done;
    }
}

void EventQueue::detach(const std::uint64_t source, const Backend& backend) {
    if (source == 0) return;
    for (auto& entry : entries_) {
        if (entry.chain_source == source) entry.chained = nullptr;
        if (entry.request.attachment != source) continue;
        if (entry.voice) backend.stop(*entry.voice);
        entry.voice.reset();
        entry.chained = nullptr;
        entry.stage = Stage::done;
    }
}

void EventQueue::set_position(const Handle handle, const Vec3& position) {
    if (auto* entry = find(handle)) entry->request.voice.position = position;
}

void EventQueue::chain(const Handle handle, const SfxEvent* event, const bool attack, const std::uint64_t source) {
    if (auto* entry = find(handle); entry && !entry->fading && entry->stage != Stage::done) {
        entry->chained = event;
        entry->attack = attack;
        entry->chain_source = source;
    }
}

void EventQueue::cancel_missing_chains(const std::function<bool(std::uint64_t)>& alive) {
    for (auto& entry : entries_) {
        if (entry.chained && entry.chain_source != 0 && !alive(entry.chain_source)) entry.chained = nullptr;
    }
}

bool EventQueue::active(const Handle handle) const {
    const auto found = handles_.find(handle);
    return found != handles_.end() && found->second->stage != Stage::done;
}

double EventQueue::fade(const Handle handle) const {
    const auto found = handles_.find(handle);
    if (found == handles_.end()) return 0.0;
    const auto& entry = *found->second;
    return entry.fading ? entry.fade_remaining_ms / entry.fade_ms : 1.0;
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

std::string tactical_music_event(const std::span<const Field> fields, const bool won,
                                 const std::string_view opposing_faction) {
    const std::string_view key = won ? "Music_Event_Tactical_Win" : "Music_Event_Tactical_Lose";
    const std::string override_key = std::string(key) + "_Vs_Faction";
    std::string fallback;
    for (const auto& [name, value] : fields) {
        if (equals(name, key)) fallback = trim(value);
        if (!equals(name, override_key)) continue;
        const auto comma = value.find(',');
        if (comma != std::string::npos && equals(trim(std::string_view(value).substr(0, comma)), opposing_faction)) {
            return std::string(trim(std::string_view(value).substr(comma + 1)));
        }
    }
    return fallback;
}

void MusicFade::begin(const double seconds) {
    ending = false;
    level = seconds > 0.0 ? 0.0 : 1.0;
    slope = seconds > 0.0 ? 1.0 / seconds : 0.0;
}

void MusicFade::retire(const double seconds) {
    ending = true;
    slope = seconds > 0.0 ? -level / seconds : 0.0;
    if (seconds <= 0.0) level = 0.0; // finite immediate policy; SND-U02 acoustic edge unverified
}

void MusicFade::advance(const double seconds) {
    level = std::clamp(level + slope * std::max(0.0, seconds), 0.0, 1.0);
    if (level == (ending ? 0.0 : 1.0)) slope = 0.0;
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

std::optional<MusicDirector::Cue> MusicDirector::result(const bool exact_local_winner, const MusicEvent* event) {
    const Mode mode = exact_local_winner ? Mode::victory : Mode::defeat;
    if (mode_ == mode || event == nullptr || event->files.empty()) return std::nullopt;
    mode_ = mode;
    if (current_ == event) return std::nullopt;
    current_ = event;
    auto& next = next_file_[event];
    const auto index = next % event->files.size();
    next = (index + 1) % event->files.size();
    return Cue{event, event->files[index], mode};
}

} // namespace eawr::presentation::audio
