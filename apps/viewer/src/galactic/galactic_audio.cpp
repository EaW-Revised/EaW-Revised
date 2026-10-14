#include "galactic_audio.hpp"
#include "audio_output.hpp"
#include "galactic_mode_internal.hpp"

#include "eawr/data/tag_trace.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/presentation/space/space.hpp"
#include <godot_cpp/classes/audio_stream_mp3.hpp>
#include <godot_cpp/classes/audio_stream_wav.hpp>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/core/memory.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <charconv>
#include <cstring>

namespace eawr::presentation::godot_backend {
namespace {
std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    return first == value.npos ? std::string{} : std::string(value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1));
}
std::vector<audio::Field> fields(const data::XmlNode& node) {
    std::vector<audio::Field> result;
    for (const auto& child : node.children) result.emplace_back(child.name, trim(child.raw_text));
    return result;
}
std::string value(std::span<const audio::Field> table, std::string_view tag) {
    for (const auto& [name, text] : table) if (audio::upper(name) == audio::upper(tag)) return text;
    return {};
}
float db(double level) { return level <= 0.0001 ? -80.0F : static_cast<float>(20.0 * std::log10(level)); }
std::optional<double> number(std::string_view text) {
    double result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
}
godot::Vector3 render(const audio::Vec3& source) {
    return {static_cast<float>(source[0]), static_cast<float>(source[2]), static_cast<float>(-source[1])};
}
}

GalacticAudio::GalacticAudio(godot::Node3D& host, const vfs::Vfs& filesystem,
    const data::Catalog& catalog, const std::string_view local_faction, const bool muted)
    : filesystem_(filesystem), catalog_(catalog), muted_(muted || audio_output_muted()) {
    // BA-01: presets and overrides in effective registry/file order.
    std::vector<const data::Definition*> definitions;
    for (const auto& definition : catalog.definitions())
        if (definition.active && definition.category == data::Category::sfx) definitions.push_back(&definition);
    std::stable_sort(definitions.begin(), definitions.end(), [](const auto* a, const auto* b) {
        return std::pair(a->registry_order, a->definition_order) < std::pair(b->registry_order, b->definition_order);
    });
    for (const auto* definition : definitions) registry_.add(definition->id, fields(definition->root), problems_);
    if (const auto document = data::load_document(filesystem, "data/xml/musicevents.xml")) {
        for (const auto& node : document.value().root.children) {
            if (audio::upper(node.name) != "MUSICEVENT") continue;
            std::string name;
            for (const auto& attribute : node.attributes) if (audio::upper(attribute.name) == "NAME") name = trim(attribute.value);
            music_events_.push_back(audio::parse_music_event(name, fields(node)));
        }
    } else problems_.push_back("music event table could not be loaded");
    if (const auto document = data::load_document(filesystem, "data/xml/audio.xml")) {
        audio_fields_ = fields(document.value().root);
        const auto saturation = number(value(audio_fields_, "Audio_Galactic_3D_Saturation_Distance_Mod"));
        const auto rolloff = number(value(audio_fields_, "Audio_Galactic_3D_Rolloff_Distance_Mod"));
        const auto height = number(value(audio_fields_, "Audio_Galactic_3D_Listener_Z_Pullback_Dist"));
        if (saturation && rolloff && height) {
            space_ = audio::Space3D{};
            space_->saturation_factor = *saturation; space_->rolloff_factor = *rolloff; space_->listener_z = *height;
        }
        for (const auto& node : document.value().root.children)
            if (node.name.starts_with("Audio_Galactic_3D_") || node.name == "Music_Event_Battle_Load_Screen"
                || node.name == "Music_Event_Battle_End_Summary_Screen_Win"
                || node.name == "Music_Event_Battle_End_Summary_Screen_Lose"
                || node.name == "SFXEvent_Planet_Revealed" || node.name == "SFXEvent_Planet_Capture_Bonus_Reward") data::tag_trace::used(node);
    } else problems_.push_back("audio table could not be loaded");
    // BA-20: the command rankings choose the speaking fleet member.
    if (const auto document = data::load_document(filesystem, "data/xml/gameconstants.xml"))
        rankings_ = audio::split_list(value(fields(document.value().root), "Unit_Command_Rankings_By_Category"));
    else problems_.push_back("game constants could not be loaded");
    // SND-44: speech is a distinct registry, not a WAV event or a music playlist.
    if (const auto document = data::load_document(filesystem, "data/xml/speechevents.xml")) {
        for (const auto& node : document.value().root.children) {
            if (audio::upper(node.name) != "SPEECHEVENT") continue;
            std::string name;
            for (const auto& attribute : node.attributes) if (audio::upper(attribute.name) == "NAME") name = trim(attribute.value);
            const auto parsed = audio::parse_music_event(name, fields(node));
            speech_events_.try_emplace(audio::upper(name), SpeechEvent{parsed.name, parsed.files, parsed.volume});
        }
    } else problems_.push_back("speech table could not be loaded");
    if (!local_faction.empty()) {
        if (const auto faction = catalog.resolve(local_faction, data::Category::faction)) {
            // Scalar fields resolve inheritance; opposing-faction lists retain
            // their authored order in the effective local faction definition.
            if (!faction.value().chain.empty()) {
                const auto& layer = faction.value().chain.front();
                if (const auto* definition = catalog.find(layer, data::Category::faction)) {
                    for (const auto& child : definition->root.children) {
                        if (child.name == "Music_Event_Strategic_Win_Vs_Faction" || child.name == "Music_Event_Strategic_Lose_Vs_Faction") {
                            data::tag_trace::used(child);
                            faction_fields_.emplace_back(child.name, trim(child.raw_text));
                        }
                    }
                }
            }
            std::vector<std::string> keys{"Strategic_Map_Music_Event", "Music_Event_Strategic_Win", "Music_Event_Strategic_Lose",
                "Music_Event_Tactical_Space_Battle_Pending", "Music_Event_Tactical_Land_Battle_Pending"};
            for (int index = 0; index <= static_cast<int>(audio::GalacticSound::land_battle_pending); ++index) {
                const auto cue = static_cast<audio::GalacticSound>(index);
                if (!audio::galactic_sound_uses_audio_table(cue)) keys.emplace_back(audio::galactic_sound_tag(cue));
            }
            for (const auto& key : keys) if (const auto* field = faction.value().value(key)) {
                data::tag_trace::used(field->value);
                faction_fields_.emplace_back(key, trim(field->value.raw_text));
            }
        } else problems_.push_back("local faction did not resolve");
    }
    for (std::size_t index = 0; index < players_.size(); ++index) {
        auto& voice = players_[index];
        voice.player = memnew(godot::AudioStreamPlayer);
        auto* server = godot::AudioServer::get_singleton();
        const auto bus_name = godot::String("EAWR_GC_Pan_") + godot::String::num_int64(static_cast<std::int64_t>(index));
        auto bus = server->get_bus_index(bus_name);
        if (bus < 0) { bus = server->get_bus_count(); server->add_bus(); server->set_bus_name(bus, bus_name); }
        server->set_bus_send(bus, "Master");
        while (server->get_bus_effect_count(bus) > 0) server->remove_bus_effect(bus, 0);
        panners_[index].instantiate(); server->add_bus_effect(bus, panners_[index]);
        voice.player->set_bus(bus_name);
        host.add_child(voice.player);
    }
    for (auto& track : tracks_) {
        track.player = memnew(godot::AudioStreamPlayer);
        host.add_child(track.player);
    }
    speech_player_ = memnew(godot::AudioStreamPlayer);
    host.add_child(speech_player_);
    // The renderer owns a bare camera; use the established tactical listener
    // viewport adapter to give Godot a listener in the galaxy's world.
    listener_viewport_ = memnew(godot::SubViewport);
    listener_viewport_->set_size(godot::Vector2i(2, 2));
    listener_viewport_->set_update_mode(godot::SubViewport::UPDATE_DISABLED);
    listener_viewport_->set_as_audio_listener_3d(true);
    listener_camera_ = memnew(godot::Camera3D);
    listener_ = memnew(godot::AudioListener3D);
    listener_viewport_->add_child(listener_camera_); listener_viewport_->add_child(listener_);
    host.add_child(listener_viewport_);
    for (auto& voice : spatial_players_) {
        voice.player = memnew(godot::AudioStreamPlayer3D);
        voice.player->set_attenuation_model(godot::AudioStreamPlayer3D::ATTENUATION_DISABLED);
        voice.player->set_max_distance(0.0F);
        voice.player->set_attenuation_filter_cutoff_hz(20500.0F);
        voice.player->set_attenuation_filter_db(0.0F);
        voice.player->set_doppler_tracking(godot::AudioStreamPlayer3D::DOPPLER_TRACKING_DISABLED);
        host.add_child(voice.player);
    }
}

GalacticAudio::~GalacticAudio() {
    for (auto& voice : players_) { voice.player->stop(); voice.player->queue_free(); }
    for (auto& track : tracks_) { track.player->stop(); track.player->queue_free(); }
    for (auto& voice : spatial_players_) { voice.player->stop(); voice.player->queue_free(); }
    speech_player_->stop(); speech_player_->queue_free();
    listener_viewport_->queue_free();
}

const audio::MusicEvent* GalacticAudio::music(const std::string_view name) const {
    for (const auto& event : music_events_) if (audio::upper(event.name) == audio::upper(name)) return &event;
    return nullptr;
}
void GalacticAudio::ambient(const bool forced) {
    strategic_ = true;
    play(director_.ambient(music(value(faction_fields_, "Strategic_Map_Music_Event")), true, false, forced));
}
void GalacticAudio::battle_pending(const bool land) {
    sound(land ? audio::GalacticSound::land_battle_pending : audio::GalacticSound::space_battle_pending);
    play(director_.start(audio::GalacticMusicDirector::Mode::battle_pending,
        music(value(faction_fields_, land ? "Music_Event_Tactical_Land_Battle_Pending" : "Music_Event_Tactical_Space_Battle_Pending"))));
}
void GalacticAudio::consume(const platform::LiveGalacticEvent& published, const std::uint32_t local) {
    std::optional<audio::GalacticPendingAlert> pending;
    if (published.pending_alert) pending = audio::GalacticPendingAlert{
        published.pending_alert->invader, published.pending_alert->defender, published.pending_alert->land};
    const auto cue = audio::galactic_event_sound(published.event, local, published.previous_owner, pending,
        published.new_owner_playable, published.construction_options_player);
    if (!cue) return;
    if (*cue == audio::GalacticSound::space_battle_pending || *cue == audio::GalacticSound::land_battle_pending)
        battle_pending(*cue == audio::GalacticSound::land_battle_pending);
    else sound(*cue);
}
void GalacticAudio::battle_load() {
    strategic_ = false;
    const auto loading = director_.battle_load(music(value(audio_fields_, "Music_Event_Battle_Load_Screen")));
    if (loading.stop_music) {
        for (auto& track : tracks_) { track.player->stop(); track.active = false; }
    }
    play(loading.cue);
}
void GalacticAudio::tactical() {
    strategic_ = false;
    director_.external_mode(audio::GalacticMusicDirector::Mode::tactical);
    for (auto& track : tracks_) { track.player->stop(); track.active = false; }
}
void GalacticAudio::outcome(const bool exact_local_winner, const bool participants_present,
    const std::string_view winner_faction, const std::string_view loser_faction) {
    play(director_.outcome(exact_local_winner, participants_present,
        music(audio::strategic_result_event(faction_fields_, true, loser_faction)),
        music(audio::strategic_result_event(faction_fields_, false, winner_faction))));
}
void GalacticAudio::battle_summary(const std::uint64_t conflict, const bool visible, const bool won) {
    // WGP-185: optional result-screen cue on entry, ambient on dismissal;
    // campaign-end strategic win/lose music is a separate selector.
    if (visible) {
        if (summary_conflict_ == conflict) return;
        summary_conflict_ = conflict;
        play(director_.start(audio::GalacticMusicDirector::Mode::battle_summary,
            music(value(audio_fields_, won ? "Music_Event_Battle_End_Summary_Screen_Win"
                                          : "Music_Event_Battle_End_Summary_Screen_Lose"))));
    } else if (summary_conflict_) {
        summary_conflict_ = 0;
        ambient(); // WGP-24: unforced return to the local faction's strategic playlist.
    }
}
void GalacticAudio::sound(const audio::GalacticSound cue, const std::optional<audio::Vec3> position) {
    const auto name = audio::galactic_sound_event(cue, faction_fields_, audio_fields_);
    if (name.empty()) return;
    const auto* event = registry_.find(name);
    if (!event) { problems_.push_back("missing SFX event: " + name); return; }
    ++sounds_[name];
    if (position && event->is_3d && !space_) {
        if (problems_.size() < 256) problems_.push_back("galactic spatial audio constants did not resolve");
        return;
    }
    // WGP-27's HUD announcements and WGP-26's pending alert have no object
    // attachment. Spatial event producers remain owned by their rule services.
    const auto accepted = events_.admit({{event, position, false, false, false, clock_}, 0, false}, random_);
    if (!accepted.handle) ++playback_failures_[name + ": admission_" + std::string(audio::to_string(accepted.result))];
}

void GalacticAudio::play_event(const std::string_view name) {
    if (name.empty()) return;
    const auto* found = registry_.find(std::string(name));
    if (!found) { if (problems_.size() < 256) problems_.push_back("missing SFX event: " + std::string(name)); return; }
    ++sounds_[std::string(name)];
    // Ordinary unforced, unattached HUD feedback with zero fade (WGP-57).
    const auto accepted = events_.admit({{found, std::nullopt, false, false, false, clock_}, 0, false}, random_);
    if (!accepted.handle) ++playback_failures_[std::string(name) + ": admission_" + std::string(audio::to_string(accepted.result))];
}

const std::vector<audio::Field>& GalacticAudio::type_fields(const std::string_view type) {
    const auto key = audio::upper(type);
    if (const auto found = type_fields_.find(key); found != type_fields_.end()) return found->second;
    auto& result = type_fields_[key];
    const auto object = catalog_.resolve(type, data::Category::game_object);
    if (!object) { if (problems_.size() < 256) problems_.push_back("type did not resolve: " + std::string(type)); return result; }
    std::vector<std::string_view> tags{"CategoryMask", "Ranking_In_Category", "SFXEvent_Fleet_Move", "SFXEvent_Move"};
    for (const auto cue : {audio::GalacticBuildCue::underway, audio::GalacticBuildCue::completed, audio::GalacticBuildCue::stopped}) {
        tags.push_back(audio::galactic_build_speech_tag(cue)); tags.push_back(audio::galactic_build_sfx_tag(cue));
    }
    for (const auto tag : tags)
        if (const auto* field = object.value().value(tag)) {
            data::tag_trace::used(field->value);
            result.emplace_back(std::string(tag), trim(field->value.raw_text));
        }
    // ML-26: preserve roster occurrence order, replacing inherited members
    // as a whole when a variant supplies a nonempty roster.
    std::vector<const data::Definition*> layers;
    for (const auto& layer : object.value().chain)
        if (const auto* definition = catalog_.find(layer, data::Category::game_object)) layers.push_back(definition);
    for (const auto* field : data::retail::list(layers, "Squadron_Units", data::retail::ListRule::replace_by_variant_all)) {
        const auto craft = audio::split_list(field->raw_text);
        if (craft.empty()) continue;
        data::tag_trace::used(*field);
        result.emplace_back("Squadron_Units", craft.front());
        break;
    }
    return result;
}

void GalacticAudio::fleet_move(const std::span<const std::string> member_types) {
    // WGP-181: the local fleet's best-ranked member speaks (BA-20 ranking), its type's
    // fleet-move event first, then its ordinary move event.
    std::vector<audio::RankedUnit> units;
    std::vector<const std::vector<audio::Field>*> response_fields;
    for (std::size_t index = 0; index < member_types.size(); ++index) {
        // BA-24: a team responds through its leading craft. On the galaxy
        // map the authored first craft supplies both ranking and response.
        const auto& container = type_fields(member_types[index]);
        const auto craft = audio::split_list(value(container, "Squadron_Units"));
        const auto& traits = craft.empty() ? container : type_fields(craft.front());
        response_fields.push_back(&traits);
        audio::RankedUnit unit{index, {}, audio::leading_integer(value(traits, "Ranking_In_Category"))};
        std::string token;
        for (const char character : value(traits, "CategoryMask") + "|") {
            if (character == '|' || character == ',' || std::isspace(static_cast<unsigned char>(character))) {
                if (!token.empty()) unit.categories.push_back(std::move(token));
                token.clear();
            } else token += character;
        }
        units.push_back(std::move(unit));
    }
    const auto speaker = audio::speaker(units, rankings_);
    if (!speaker) return;
    const auto& traits = *response_fields[*speaker];
    auto name = value(traits, "SFXEvent_Fleet_Move");
    if (name.empty()) name = value(traits, "SFXEvent_Move");
    play_event(name);
}

void GalacticAudio::build(const audio::GalacticBuildCue cue, const std::string_view type, const bool local_builder) {
    if (type.empty() || !local_builder) return;
    const auto feedback = audio::galactic_build_feedback(cue, type_fields(type), local_builder);
    if (feedback.speech.empty()) { play_event(feedback.sfx); return; }
    // WGEP-96/97: authored speech wins even when its stream fails to open (SND-35).
    const auto found = speech_events_.find(audio::upper(feedback.speech));
    if (found == speech_events_.end()) {
        if (problems_.size() < 256) problems_.push_back("missing speech event: " + feedback.speech);
        return;
    }
    if (!speech_queue_.push(&found->second, audio::galactic_build_speech_tag(cue)) && problems_.size() < 256)
        problems_.push_back("speech queue full: " + feedback.speech);
}

void GalacticAudio::update_speech(const bool paused) {
    speech_player_->set_stream_paused(paused);
    if (paused) return;
    // SND-48: a started entry leaves the FIFO when its stream ends.
    if (speech_stream_.active() && !speech_player_->is_playing() && clock_ - speech_started_at_ > 0.1)
        speech_stream_.finished(false);
    const auto* front = speech_queue_.front();
    if (!front) return;
    if (speech_started_) {
        if (speech_stream_.contains(speech_handle_)) return;
        speech_queue_.pop(); speech_started_ = false;
        front = speech_queue_.front();
        if (!front) return;
    }
    const auto& speech = *front->event;
    if (!speech_stream_.admit(3) || speech.files.empty()) { speech_queue_.pop(); return; }
    const auto& file = speech.files[static_cast<std::size_t>(random_.between(0, static_cast<int>(speech.files.size()) - 1))];
    std::string path = "data/audio/speech/english/";
    for (const unsigned char character : file) path += character == '\\' ? '/' : static_cast<char>(std::tolower(character));
    godot::Ref<godot::AudioStream> loaded;
    if (const auto cached = streams_.find(path); cached != streams_.end()) loaded = cached->second;
    else {
        if (const auto bytes = filesystem_.open(path)) {
            godot::PackedByteArray data;
            if (path.ends_with(".wav")) {
                std::string error;
                if (const auto pcm = audio::parse_wav(bytes.value(), error)) {
                    data.resize(static_cast<std::int64_t>(pcm->data.size()));
                    std::memcpy(data.ptrw(), pcm->data.data(), pcm->data.size());
                    godot::Ref<godot::AudioStreamWAV> wav;
                    wav.instantiate(); wav->set_format(godot::AudioStreamWAV::FORMAT_16_BITS);
                    wav->set_mix_rate(static_cast<std::int32_t>(pcm->sample_rate));
                    wav->set_stereo(pcm->channels == 2); wav->set_data(data); loaded = wav;
                }
            } else {
                data.resize(static_cast<std::int64_t>(bytes.value().size()));
                std::memcpy(data.ptrw(), bytes.value().data(), bytes.value().size());
                godot::Ref<godot::AudioStreamMP3> mp3;
                mp3.instantiate(); mp3->set_data(data); mp3->set_loop(false);
                if (mp3->get_length() > 0) loaded = mp3;
            }
        }
        if (loaded.is_null() && problems_.size() < 256) problems_.push_back("missing or undecodable speech: " + path);
        streams_.emplace(path, loaded);
    }
    if (loaded.is_null()) { speech_queue_.pop(); return; }
    speech_player_->stop(); speech_player_->set_stream(loaded);
    speech_player_->set_volume_db(db(muted_ ? 0 : speech.volume)); speech_player_->play();
    speech_stream_.opened();
    speech_handle_ = speech_stream_.handle(); speech_started_ = true; speech_started_at_ = clock_;
    if (speech_log_.size() < 256) speech_log_.push_back(speech.name + " " + file);
}

godot::Ref<godot::AudioStream> GalacticAudio::stream(const std::string_view file, const bool music_file) {
    std::string path = music_file ? "data/audio/music/" : "data/audio/sfx/";
    for (const unsigned char character : file) path += character == '\\' ? '/' : static_cast<char>(std::tolower(character));
    if (const auto found = streams_.find(path); found != streams_.end()) return found->second;
    godot::Ref<godot::AudioStream> result;
    if (const auto bytes = filesystem_.open(path)) {
        godot::PackedByteArray data;
        if (music_file) {
            data.resize(static_cast<std::int64_t>(bytes.value().size()));
            std::memcpy(data.ptrw(), bytes.value().data(), bytes.value().size());
            godot::Ref<godot::AudioStreamMP3> mp3;
            mp3.instantiate(); mp3->set_data(data); mp3->set_loop(false);
            if (mp3->get_length() > 0) result = mp3;
        } else {
            std::string error;
            if (const auto pcm = audio::parse_wav(bytes.value(), error)) {
                data.resize(static_cast<std::int64_t>(pcm->data.size()));
                std::memcpy(data.ptrw(), pcm->data.data(), pcm->data.size());
                godot::Ref<godot::AudioStreamWAV> wav;
                wav.instantiate(); wav->set_format(godot::AudioStreamWAV::FORMAT_16_BITS);
                wav->set_mix_rate(static_cast<std::int32_t>(pcm->sample_rate));
                wav->set_stereo(pcm->channels == 2); wav->set_data(data); result = wav;
            }
        }
    }
    if (result.is_null() && problems_.size() < 256) problems_.push_back("missing or undecodable audio: " + path);
    streams_.emplace(path, result);
    return result;
}

void GalacticAudio::play(const std::optional<audio::GalacticMusicDirector::Cue>& cue) {
    if (!cue) return;
    if (music_log_.size() < 256) music_log_.push_back(cue->event->name + " " + cue->file);
    auto loaded = stream(cue->file, true);
    if (loaded.is_null()) {
        if (problems_.size() < 256) problems_.push_back("music cue " + cue->event->name
            + " could not load: " + cue->file);
        return;
    }
    auto& previous = tracks_[current_];
    if (previous.active) {
        auto& older = tracks_[1 - current_];
        older.player->stop(); older.active = false;
        previous.fade.retire(cue->event->fade_out_previous_seconds);
        current_ = 1 - current_;
    }
    auto& next = tracks_[current_];
    if (cue->event->internal_loop) {
        godot::Ref<godot::AudioStreamMP3> loop = loaded->duplicate();
        if (loop.is_valid()) { loop->set_loop(true); loaded = loop; }
    }
    next.player->set_stream(loaded); next.volume = cue->event->volume;
    next.fade.begin(cue->event->fade_in_seconds); next.active = true;
    next.player->set_volume_db(db(muted_ ? 0 : next.volume * next.fade.level * audio::default_volume_slider * audio::default_volume_slider));
    next.player->play();
}

void GalacticAudio::service(const double delta, const FixedCamera& camera, const bool paused) {
    clock_ += std::max(0.0, delta);
    // SND-13: ordinary HUD one-shots and music continue; loops and spatial
    // samples retain their slots and offsets through a galaxy pause.
    for (std::size_t index = 0; index < players_.size(); ++index) {
        const auto* event = voices_.playing(index + audio::Voices::voices_3d);
        players_[index].player->set_stream_paused(paused && event && audio::pauses_with_game(*event, false));
    }
    for (auto& voice : spatial_players_) voice.player->set_stream_paused(paused);
    update_speech(paused);
    if (space_ && strategic_) {
        const auto eye = space::source_from_render(camera.eye);
        const auto target = space::source_from_render(camera.target);
        const auto listener = audio::listener_for_camera({eye.x, eye.y, eye.z},
            {target.x - eye.x, target.y - eye.y, target.z - eye.z}, *space_);
        listener_position_ = listener.position;
        if (listener_->is_inside_tree()) {
            if (!listener_->is_current()) listener_->make_current();
            const auto pose = godot::Transform3D().looking_at(render(listener.ahead), godot::Vector3(0, -1, 0)).translated(render(listener.position));
            listener_->set_transform(pose); listener_camera_->set_transform(pose);
        }
    }
    const audio::EventQueue::Backend backend{
        [this](const audio::EventQueue::Sample& sample) {
            auto request = sample.request.voice; request.started_at = clock_;
            auto start = voices_.allocate(request, listener_position_, sample.values);
            if (start.result != audio::Start::Result::playing) {
                ++playback_failures_[request.event->name + ": " + std::string(audio::to_string(start.result))];
                return start;
            }
            if (audio::Voices::is_3d_voice(start.voice)) spatial_players_.at(start.voice).player->stop();
            else players_.at(start.voice - audio::Voices::voices_3d).player->stop();
            auto loaded = stream(start.sample, false);
            if (loaded.is_null()) {
                ++playback_failures_[request.event->name + ": sample_missing"];
                voices_.finished(start.voice); start.result = audio::Start::Result::no_samples; return start;
            }
            ++played_sounds_[request.event->name];
            if (sample.continuous) {
                godot::Ref<godot::AudioStreamWAV> loop = loaded->duplicate();
                if (loop.is_valid()) {
                    loop->set_loop_mode(godot::AudioStreamWAV::LOOP_FORWARD);
                    loop->set_loop_end(static_cast<std::int32_t>(loop->get_data().size() / (loop->is_stereo() ? 4 : 2)));
                    loaded = loop;
                }
            }
            if (audio::Voices::is_3d_voice(start.voice)) {
                auto& voice = spatial_players_.at(start.voice);
                voice.player->stop(); voice.player->set_stream(loaded);
                voice.gain = start.volume * audio::category_gain(*request.event, {});
                voice.handle = sample.handle; voice.started_at = clock_; voice.position = *request.position;
                voice.saturation = request.event->saturation_distance;
                voice.player->set_position(render(voice.position));
                voice.player->set_pitch_scale(static_cast<float>(start.pitch));
                voice.player->set_volume_db(-80.0F); voice.player->play();
                return start;
            }
            auto& voice = players_.at(start.voice - audio::Voices::voices_3d);
            voice.player->stop(); voice.player->set_stream(loaded);
            // SND-61: an active speech stream caps ordinary SFX gain.
            voice.gain = audio::speech_sfx_gain(start.volume, speech_stream_.active(), false) * audio::category_gain(*request.event, {});
            voice.handle = sample.handle; voice.started_at = clock_;
            panners_[start.voice - audio::Voices::voices_3d]->set_pan(static_cast<float>(start.pan * 2 - 1));
            voice.player->set_pitch_scale(static_cast<float>(start.pitch));
            voice.player->set_volume_db(db(muted_ ? 0 : voice.gain)); voice.player->play();
            return start;
        },
        [this](std::size_t index) {
            if (audio::Voices::is_3d_voice(index)) { const auto& voice = spatial_players_.at(index);
                return clock_ - voice.started_at <= 0.1 || voice.player->is_playing(); }
            const auto& voice = players_.at(index - audio::Voices::voices_3d);
            return clock_ - voice.started_at <= 0.1 || voice.player->is_playing(); },
        [this](std::size_t index) {
            if (audio::Voices::is_3d_voice(index)) spatial_players_.at(index).player->stop();
            else players_.at(index - audio::Voices::voices_3d).player->stop();
            voices_.finished(index); },
        [this](std::size_t index) { voices_.set_fading(index); }
    };
    const auto chains = events_.service(std::max(0.0, delta) * 1000, paused, listener_position_, random_, registry_, backend);
    for (const auto& chain : chains)
        static_cast<void>(events_.admit({{chain.event, std::nullopt, false, false, false, clock_}, 0, false}, random_));
    for (auto& voice : players_) if (voice.handle)
        voice.player->set_volume_db(db(muted_ ? 0 : voice.gain * events_.fade(voice.handle)));
    if (space_) for (auto& voice : spatial_players_) if (voice.handle) {
        double squared = 0;
        for (std::size_t index = 0; index < 3; ++index) squared += std::pow(voice.position[index] - listener_position_[index], 2);
        const auto gain = voice.gain * events_.fade(voice.handle) * audio::falloff_gain(std::sqrt(squared), voice.saturation, *space_);
        voice.player->set_volume_db(db(muted_ || !strategic_ ? 0 : gain));
    }
    bool ended = false;
    for (std::size_t index = 0; index < tracks_.size(); ++index) {
        auto& track = tracks_[index];
        if (!track.active) continue;
        track.fade.advance(std::max(0.0, delta));
        track.player->set_volume_db(db(muted_ ? 0 : track.volume * track.fade.level * audio::default_volume_slider * audio::default_volume_slider));
        if (index == current_ && !track.fade.ending && !track.player->is_playing()) { track.active = false; ended = true; }
    }
    if (ended) play(director_.track_ended());
}

void GalacticAudio::write_report(std::ostream& output) const {
    using Mode = audio::GalacticMusicDirector::Mode;
    const auto mode = director_.mode();
    const auto mode_name = mode == Mode::strategic_ambient ? "strategic_ambient"
        : mode == Mode::battle_pending ? "battle_pending" : mode == Mode::battle_load ? "battle_load"
        : mode == Mode::battle_summary ? "battle_summary" : mode == Mode::tactical ? "tactical"
        : mode == Mode::strategic_win ? "strategic_win" : mode == Mode::strategic_lose ? "strategic_lose"
        : mode == Mode::superweapon_attack ? "superweapon_attack" : mode == Mode::demo_finale ? "demo_finale"
        : mode == Mode::scripted ? "scripted" : "none";
    output << "{\"mode\":" << galactic_detail::json(mode_name)
        << ",\"muted\":" << (muted_ ? "true" : "false") << ",\"music\":[";
    for (std::size_t index = 0; index < music_log_.size(); ++index)
        output << (index ? "," : "") << galactic_detail::json(music_log_[index]);
    output << "],\"speech\":[";
    for (std::size_t index = 0; index < speech_log_.size(); ++index)
        output << (index ? "," : "") << galactic_detail::json(speech_log_[index]);
    output << "],\"sounds\":{";
    bool first = true;
    for (const auto& [name, count] : sounds_) { output << (first ? "" : ",") << galactic_detail::json(name) << ':' << count; first = false; }
    output << "},\"played_sounds\":{";
    first = true;
    for (const auto& [name, count] : played_sounds_) { output << (first ? "" : ",") << galactic_detail::json(name) << ':' << count; first = false; }
    output << "},\"playback_failures\":{";
    first = true;
    for (const auto& [name, count] : playback_failures_) { output << (first ? "" : ",") << galactic_detail::json(name) << ':' << count; first = false; }
    output << "},\"resolved_type_audio\":{";
    first = true;
    for (const auto& [name, traits] : type_fields_) {
        output << (first ? "" : ",") << galactic_detail::json(name) << ":{\"category\":"
            << galactic_detail::json(value(traits, "CategoryMask")) << ",\"fleet_move\":"
            << galactic_detail::json(value(traits, "SFXEvent_Fleet_Move")) << ",\"move\":"
            << galactic_detail::json(value(traits, "SFXEvent_Move")) << '}';
        first = false;
    }
    output << "},\"problems\":[";
    for (std::size_t index = 0; index < problems_.size(); ++index) output << (index ? "," : "") << galactic_detail::json(problems_[index]);
    output << "]}";
}
} // namespace eawr::presentation::godot_backend
