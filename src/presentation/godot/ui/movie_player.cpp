#include "gpu_debug_labels.hpp"
#include "eawr/vfs/vfs.hpp"
#include "movie_player.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/video_stream_theora.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/core/object.hpp>
#include "eawr/core/json_text.hpp"
#include "eawr/core/text.hpp"
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <cstring>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace codes = data::ui::diagnostic_codes;

// Colour from the left half, opacity from the luma of the right half: luma
// is what the grey was encoded as, so chroma noise at the seam cannot leak
// into it. UVs stay half a texel inside each half so filtering never mixes
// the halves.
constexpr const char* packed_alpha_shader = R"(shader_type canvas_item;
varying vec4 tint;
void vertex() {
    tint = COLOR;
}
void fragment() {
    float inset = 0.5 * TEXTURE_PIXEL_SIZE.x;
    vec2 uv = vec2(clamp(UV.x * 0.5, inset, 0.5 - inset), UV.y);
    vec3 colour = texture(TEXTURE, uv).rgb;
    float opacity = dot(texture(TEXTURE, uv + vec2(0.5, 0.0)).rgb, vec3(0.299, 0.587, 0.114));
    COLOR = vec4(colour, clamp(opacity, 0.0, 1.0)) * tint;
}
)";

core::Diagnostic diagnostic(const std::string_view code, std::string message, const data::ui::HudMovie& movie) {
    return core::Diagnostic{std::string(code), core::Severity::error, std::move(message),
        movie.source.canonical_path, {}, {}, movie.source.source_id};
}

String godot_string(const std::string& text) {
    return String::utf8(text.data(), static_cast<int64_t>(text.size()));
}

bool is_ogg(const String& path) {
    const Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
    if (file.is_null() || file->get_length() < 4) return false;
    const PackedByteArray magic = file->get_buffer(4);
    return magic.size() == 4 && magic[0] == 'O' && magic[1] == 'g' && magic[2] == 'g' && magic[3] == 'S';
}

} // namespace

namespace {

core::Result<VideoStreamPlayer*> prepare_hud_movie(const data::ui::HudMovie& movie,
    const String& cache_directory, const Rect2& rect, const bool loop) {
    const String path = cache_directory.path_join(godot_string(data::ui::movie_cache_file(movie)));
    if (!FileAccess::file_exists(path)) {
        return core::Result<VideoStreamPlayer*>::failure(diagnostic(codes::movie_unconverted,
            "movie \"" + movie.name + "\" is not converted; run tools/ui/convert_hud_movie.py with this cache", movie));
    }
    if (!is_ogg(path)) {
        return core::Result<VideoStreamPlayer*>::failure(diagnostic(codes::movie_undecodable,
            "the cache entry of movie \"" + movie.name + "\" is not an Ogg stream", movie));
    }

    Ref<VideoStreamTheora> stream;
    stream.instantiate();
    stream->set_file(path);
    auto* player = memnew(VideoStreamPlayer);
    player->set_name(godot_string(movie.name));
    player->set_expand(true);
    player->set_position(rect.position);
    player->set_size(rect.size);
    player->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    player->set_loop(loop);
    player->set_volume(0.0F);
    player->set_stream(stream);
    if (movie.alpha) {
        Ref<Shader> shader;
        shader.instantiate();
        name_canvas_shader(*shader.ptr(), "EAWR/UI/HudMoviePackedAlpha");
        shader->set_code(packed_alpha_shader);
        Ref<ShaderMaterial> material;
        material.instantiate();
        material->set_shader(shader);
        player->set_material(material);
    }
    return core::Result<VideoStreamPlayer*>::success(player);
}

VideoStreamPlayer* movie_player(const std::int64_t id) {
    return id && UtilityFunctions::is_instance_id_valid(id)
        ? Object::cast_to<VideoStreamPlayer>(ObjectDB::get_instance(static_cast<uint64_t>(id))) : nullptr;
}

TextureRect* bink_node(const std::int64_t id) {
    return id && UtilityFunctions::is_instance_id_valid(id)
        ? Object::cast_to<TextureRect>(ObjectDB::get_instance(static_cast<uint64_t>(id))) : nullptr;
}

void prepare_bink_frame(BinkFrame& frame) {
    auto bytes = std::make_shared<PackedByteArray>();
    bytes->resize(static_cast<int64_t>(frame.rgba.size()));
    std::memcpy(bytes->ptrw(), frame.rgba.data(), frame.rgba.size());
    frame.presentation_data = std::move(bytes);
    frame.rgba.clear();
}

Ref<Image> bink_image(const BinkFrame& frame) {
    const auto bytes = std::static_pointer_cast<PackedByteArray>(frame.presentation_data);
    if (!bytes || !frame.width || !frame.height || static_cast<std::uint64_t>(bytes->size()) !=
        static_cast<std::uint64_t>(frame.width) * frame.height * 4) return {};
    return Image::create_from_data(static_cast<int32_t>(frame.width), static_cast<int32_t>(frame.height), false, Image::FORMAT_RGBA8, *bytes);
}

} // namespace

core::Result<VideoStreamPlayer*> attach_hud_movie(Control& parent, const data::ui::HudMovie& movie,
    const String& cache_directory, const Rect2& rect, const bool loop) {
    auto prepared = prepare_hud_movie(movie, cache_directory, rect, loop);
    if (!prepared) return prepared;
    parent.add_child(prepared.value());
    prepared.value()->play();
    return prepared;
}

core::Result<void> validate_hud_movie(const VideoStreamPlayer& player, const data::ui::HudMovie& movie) {
    const Ref<Texture2D> texture = player.get_video_texture();
    if (texture.is_null() || texture->get_width() <= 0 || texture->get_height() <= 0) {
        return core::Result<void>::failure(diagnostic(codes::movie_undecodable,
            "movie \"" + movie.name + "\" decoded no frame from its cache entry", movie));
    }
    return core::Result<void>::success();
}

CommandBarMovie::~CommandBarMovie() { stop(); release_prepared(); }

VideoStreamPlayer* CommandBarMovie::player() const {
    return movie_player(player_id_);
}

void CommandBarMovie::release_prepared() {
    // Prepared players are not in a scene tree; this owner releases them directly.
    if (auto* prepared = movie_player(prepared_player_id_)) memdelete(prepared);
    prepared_player_id_ = 0;
    if (auto* node = bink_node(bink_node_id_); node && !bink_active_) {
        if (node->get_parent()) node->get_parent()->remove_child(node);
        memdelete(node);
    }
    if (!bink_active_) { bink_node_id_ = 0; bink_.reset(); bink_texture_.unref(); bink_ready_ = false; }
    warming_ = false;
}

void CommandBarMovie::stop() {
    if (auto* current = player()) { current->stop(); current->hide(); current->queue_free(); }
    player_id_ = 0;
    if (bink_active_) {
        if (auto* node = bink_node(bink_node_id_)) { node->hide(); node->queue_free(); }
        bink_node_id_ = 0; bink_active_ = bink_ready_ = false;
        bink_.reset(); bink_texture_.unref();
    }
    speech_.clear();
}

void CommandBarMovie::prepare(const vfs::Vfs& filesystem, const std::string_view movie,
    const presentation::ui::ReferencePoint bone, Control& parent, BinkSourceFactory decoder_factory,
    std::function<void(std::string, std::string, std::string)> failure_sink) {
    stop(); release_prepared(); movie_.reset(); problem_.clear(); decoded_ = false; bone_ = bone; requested_movie_ = movie; requested_speech_.clear();
    failure_sink_ = std::move(failure_sink); failure_noted_ = false;
    if (movie.empty()) return;
    // Prepared behind the loading screen: no XML/source hashing on the battle frame.
    auto resolved = data::ui::resolve_speech_movie(filesystem, {{}, std::string(movie)});
    if (!resolved) { problem_ = core::format_diagnostic(resolved.error()); note_failure(); return; }
    movie_ = std::move(resolved.value().movie);
    style_ = data::ui::command_bar_movie_style(filesystem);
    bink_parent_id_ = static_cast<std::int64_t>(parent.get_instance_id());
    auto source = filesystem.open(movie_->source);
    if (source && decoder_factory) {
        try {
            bink_ = std::make_unique<BinkWorker>([factory = std::move(decoder_factory), bytes = std::move(source).value()](std::shared_ptr<std::atomic_bool> cancelled) mutable {
                return factory(std::move(bytes), std::move(cancelled));
            }, prepare_bink_frame);
        } catch (...) {
            // Thread creation is optional: continue with the cached player.
            bink_.reset();
        }
    }
    auto* os = OS::get_singleton();
    bink_trace_ = os->get_environment("EAWR_BINK_TRACE") == "1";
    cache_ = os->get_environment("EAWR_MOVIE_CACHE");
    if (cache_.is_empty()) cache_ = "user://hud-movies";
    const auto arguments = os->get_cmdline_user_args();
    for (int64_t i = 0; i + 1 < arguments.size(); ++i)
        if (arguments[i] == "--eawr-movie-cache") cache_ = arguments[++i];
    // Opening the stream and creating its shader/material belong to HUD loading.
    auto prepared = prepare_hud_movie(*movie_, cache_, Rect2(0, 0, 1, 1), true);
    if (!prepared) { problem_ = core::format_diagnostic(prepared.error()); serviced_at_ = {}; return; }
    prepared_player_id_ = static_cast<int64_t>(prepared.value()->get_instance_id());
    // A visible, in-viewport draw warms the real material/texture pipeline.
    // Hidden/offscreen nodes are culled and would defer that work to battle.
    prepared.value()->set_name("EAWR_HUD_MOVIE_WARMUP");
    // Godot culls canvas items below 0.007 alpha; remain faint but above it.
    prepared.value()->set_modulate(Color(style_.colour[0], style_.colour[1], style_.colour[2], 1.0F / 64.0F));
    prepared.value()->set_size(Vector2(2, 2));
    parent.add_child(prepared.value());
    prepared.value()->play();
    warming_ = true;
    warmup_first_draw_ = 0;
    serviced_at_ = {}; // Remaining synchronous scene loading is not decode time.
}

bool CommandBarMovie::warmup() {
    if (bink_) {
        if (serviced_at_ == std::chrono::steady_clock::time_point{}) serviced_at_ = std::chrono::steady_clock::now();
        if (!bink_node_id_) {
            BinkFrame frame;
            if (bink_->take(frame)) {
                const Ref<Image> image = bink_image(frame);
                auto* parent = Object::cast_to<Control>(ObjectDB::get_instance(static_cast<uint64_t>(bink_parent_id_)));
                if (image.is_valid() && parent && frame.rate_numerator && frame.rate_denominator) {
                    bink_frame_seconds_ = static_cast<double>(frame.rate_denominator) / frame.rate_numerator;
                    bink_texture_ = ImageTexture::create_from_image(image);
                    auto* node = memnew(TextureRect);
                    node->set_name("EAWR_HUD_MOVIE_WARMUP");
                    node->set_texture(bink_texture_);
                    if (bink_trace_) node->set_meta("eawr_movie_selected_frame", static_cast<int64_t>(frame.selected_frame));
                    if (bink_trace_) node->set_meta("eawr_movie_total_frames", static_cast<int64_t>(frame.total_frames));
                    node->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
                    node->set_stretch_mode(TextureRect::STRETCH_SCALE);
                    node->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                    node->set_modulate(Color(style_.colour[0], style_.colour[1], style_.colour[2], 1.0F / 64.0F));
                    node->set_size(Vector2(2, 2));
                    parent->add_child(node);
                    bink_node_id_ = static_cast<std::int64_t>(node->get_instance_id());
                    warmup_first_draw_ = 0;
                } else bink_.reset();
            }
        }
        if (auto* node = bink_node(bink_node_id_)) {
            const auto drawn = static_cast<std::uint64_t>(Engine::get_singleton()->get_frames_drawn());
            if (!warmup_first_draw_) warmup_first_draw_ = drawn;
            if (drawn >= warmup_first_draw_ + 2) {
                node->set_meta("eawr_movie_warmup_frames", static_cast<int64_t>(drawn - warmup_first_draw_));
                node->hide(); node->get_parent()->remove_child(node);
                node->set_name(godot_string(movie_->name));
                node->set_modulate(Color(style_.colour[0], style_.colour[1], style_.colour[2], style_.colour[3]));
                if (auto* cached = movie_player(prepared_player_id_)) {
                    cached->stop();
                    if (cached->get_parent()) cached->get_parent()->remove_child(cached);
                    cached->hide();
                }
                warming_ = false; bink_ready_ = true; problem_.clear();
                return true;
            }
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - serviced_at_).count() < 4.0) return false;
            if (node->get_parent()) node->get_parent()->remove_child(node);
            memdelete(node);
            bink_node_id_ = 0;
        }
        if (bink_ && !bink_->failed()
            && std::chrono::duration<double>(std::chrono::steady_clock::now() - serviced_at_).count() < 4.0) return false;
        bink_.reset(); serviced_at_ = {}; warmup_first_draw_ = 0;
    }
    auto* prepared = movie_player(prepared_player_id_);
    if (!warming_ || !prepared) { if (!problem_.empty()) note_failure(); return true; }
    if (serviced_at_ == std::chrono::steady_clock::time_point{}) serviced_at_ = std::chrono::steady_clock::now();
    const Ref<Texture2D> texture = prepared->get_video_texture();
    if (texture.is_valid() && texture->get_width() > 0 && texture->get_height() > 0) {
        const auto drawn = static_cast<std::uint64_t>(Engine::get_singleton()->get_frames_drawn());
        if (warmup_first_draw_ == 0) warmup_first_draw_ = drawn;
        if (drawn >= warmup_first_draw_ + 2) {
            prepared->set_meta("eawr_movie_warmup_frames", static_cast<int64_t>(drawn - warmup_first_draw_));
            prepared->stop();
            prepared->hide();
            prepared->get_parent()->remove_child(prepared);
            prepared->set_name(godot_string(movie_->name));
            prepared->set_modulate(Color(style_.colour[0], style_.colour[1], style_.colour[2], style_.colour[3]));
            warming_ = false;
            return true;
        }
    }
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - serviced_at_).count() < 1.0) return false;
    const auto valid = validate_hud_movie(*prepared, *movie_);
    problem_ = core::format_diagnostic(!valid ? valid.error() : diagnostic(codes::movie_undecodable,
        "movie did not complete its loading draw warm-up", *movie_));
    note_failure();
    release_prepared();
    return true;
}

bool CommandBarMovie::start(Control& parent, const std::string_view speech) {
    stop(); decode_seconds_ = 0; decoded_ = false; requested_speech_ = speech;
    serviced_at_ = std::chrono::steady_clock::now();
    if (bink_ready_ && bink_) {
        if (auto* node = bink_node(bink_node_id_)) {
            speech_ = core::to_upper(speech); bink_active_ = true; bink_elapsed_ = 0;
            parent.add_child(node); node->show(); decoded_ = true;
            return true;
        }
    }
    auto* prepared = movie_player(prepared_player_id_);
    // G12/SND-69: failed movie start leaves speech queued and the slot empty.
    if (!prepared || warming_) return false;
    speech_ = core::to_upper(speech);
    player_id_ = prepared_player_id_;
    prepared_player_id_ = 0;
    parent.add_child(prepared);
    prepared->show();
    prepared->play();
    return true;
}

void CommandBarMovie::service(const bool speech_present, const bool paused,
    const presentation::ui::ShellPlacement& placement) {
    if (bink_active_) {
        auto* node = bink_node(bink_node_id_);
        if (!node || !speech_present) { stop(); return; }
        if (!bink_ || bink_->failed()) {
            auto* parent = Object::cast_to<Control>(node->get_parent());
            const auto speech = speech_;
            stop();
            if (parent && prepared_player_id_ && start(*parent, speech)) return;
            problem_ = "EAWR-LOAD-1237: direct movie playback failed and no usable cache is available";
            note_failure();
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!paused) {
            bink_elapsed_ += std::chrono::duration<double>(now - serviced_at_).count();
            BinkFrame frame;
            if (bink_->take(frame)) {
                const auto image = bink_image(frame);
                if (image.is_valid()) {
                    bink_texture_->update(image);
                    if (bink_trace_) {
                        node->set_meta("eawr_movie_selected_frame", static_cast<int64_t>(frame.selected_frame));
                        for (int i = 0; i < 3; ++i)
                            node->set_meta(String("eawr_movie_first_") + String::num_int64(i + 1), static_cast<int64_t>(frame.trace_first[i]));
                        node->set_meta("eawr_movie_last", static_cast<int64_t>(frame.trace_last));
                    }
                }
            }
            if (bink_elapsed_ >= bink_frame_seconds_) { bink_elapsed_ = 0; bink_->request(); }
        }
        serviced_at_ = now;
        node->set_position(Vector2(static_cast<float>(placement.left + (bone_.x + style_.offset_x + movie_->offset_x) * placement.scale),
            static_cast<float>(placement.bottom - (bone_.y + style_.offset_y + movie_->offset_y) * placement.scale)));
        node->set_size(Vector2(static_cast<float>(bink_texture_->get_width() * placement.scale),
            static_cast<float>(bink_texture_->get_height() * placement.scale)));
        return;
    }
    auto* current = player();
    if (!current) return;
    if (!speech_present) { stop(); return; }
    const auto now = std::chrono::steady_clock::now();
    if (!paused) decode_seconds_ += std::chrono::duration<double>(now - serviced_at_).count();
    serviced_at_ = now;
    current->set_paused(paused); // WBF-53: independent movie pause hook.
    const Ref<Texture2D> texture = current->get_video_texture();
    if (texture.is_valid() && texture->get_width() > 0 && texture->get_height() > 0) {
        decoded_ = true;
        const double width = texture->get_width() / (movie_->alpha ? 2.0 : 1.0);
        // G12: top-left at bone + global + per-movie offset; native decoded size.
        current->set_position(Vector2(static_cast<float>(placement.left
            + (bone_.x + style_.offset_x + movie_->offset_x) * placement.scale),
            static_cast<float>(placement.bottom - (bone_.y + style_.offset_y + movie_->offset_y) * placement.scale)));
        current->set_size(Vector2(static_cast<float>(width * placement.scale),
            static_cast<float>(texture->get_height() * placement.scale)));
    } else if (!paused && decode_seconds_ >= 1.0) {
        // A bounded decode probe is viewer policy; it never aborts the mode/audio.
        const auto valid = validate_hud_movie(*current, *movie_);
        if (!valid) problem_ = core::format_diagnostic(valid.error());
        note_failure();
        stop();
    }
}

bool CommandBarMovie::playing() const { const auto* current = player(); return bink_active_ || (decoded_ && current && current->is_playing()); }

void CommandBarMovie::note_failure() {
    if (failure_noted_) return;
    failure_noted_ = true;
    try {
        if (failure_sink_) failure_sink_(movie_ ? movie_->source.canonical_path : std::string{},
            movie_ ? movie_->source.layer_id : std::string{}, requested_movie_);
    } catch (...) { /* A note must never interrupt the battle. */ }
}

void CommandBarMovie::write_report(std::ostream& output) const {
    const auto* current = player();
    output << "{\"speech\":" << core::json_quote(requested_speech_)
           << ",\"tracked_speech\":" << core::json_quote(speech_)
           << ",\"movie\":" << core::json_quote(requested_movie_)
           << ",\"decoded\":" << (decoded_ ? "true" : "false")
           << ",\"playing\":" << (playing() ? "true" : "false")
           << ",\"position\":" << (current ? current->get_stream_position() : 0.0)
           << ",\"problem\":" << core::json_quote(problem_) << '}';
}

} // namespace eawr::presentation::godot_backend
