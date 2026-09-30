#include "movie_player.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/video_stream_theora.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <string>
#include <string_view>
#include <utility>

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

core::Result<VideoStreamPlayer*> attach_hud_movie(Control& parent, const data::ui::HudMovie& movie,
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
        shader->set_code(packed_alpha_shader);
        Ref<ShaderMaterial> material;
        material.instantiate();
        material->set_shader(shader);
        player->set_material(material);
    }
    parent.add_child(player);
    player->play();
    return core::Result<VideoStreamPlayer*>::success(player);
}

core::Result<void> validate_hud_movie(const VideoStreamPlayer& player, const data::ui::HudMovie& movie) {
    const Ref<Texture2D> texture = player.get_video_texture();
    if (texture.is_null() || texture->get_width() <= 0 || texture->get_height() <= 0) {
        return core::Result<void>::failure(diagnostic(codes::movie_undecodable,
            "movie \"" + movie.name + "\" decoded no frame from its cache entry", movie));
    }
    return core::Result<void>::success();
}

} // namespace eawr::presentation::godot_backend
