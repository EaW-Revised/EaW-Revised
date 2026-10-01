#include "font_mode.hpp"

#include "capture_viewport.hpp"
#include "viewer_path.hpp"

#include "ui/font_provider.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/presentation/renderer.hpp"
#include "eawr/presentation/ui/fonts.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/font_file.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;

constexpr int warmup_frames = 6;
constexpr std::int32_t face_point_size = 24;
constexpr std::int32_t small_point_size = 7;
constexpr std::int32_t caption_pixels = 12;
constexpr std::string_view face_text = "EMPIRE AT WAR - Forces of Corruption 0123";
constexpr std::string_view upper_text = "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789";
constexpr std::string_view lower_text = "the quick brown fox jumps over the lazy dog";
const Color background(0.05F, 0.06F, 0.08F);
const Color ink(0.95F, 0.93F, 0.85F);
const Color caption_ink(0.55F, 0.62F, 0.70F);

// UI-F3 requests beyond the four faces: a system face, a face no system has,
// game data's lower-case spelling, and the two languages that always take the
// Unicode face.
struct FallbackRequest final {
    std::string_view face;
    std::int32_t point_size;
    std::string_view language;
};
constexpr std::array<FallbackRequest, 6> fallback_requests{{
    {"Arial Bold", 10, "ENGLISH"},
    {"Arial Medium", 10, "ENGLISH"},
    {"EmpireAtWar-light", 10, "ENGLISH"},
    {"EmpireAtWar-Bold", 5, "RUSSIAN"},
    {"EmpireAtWar-Stencil", 10, "JAPANESE"},
    {"Missing Face", 10, "ENGLISH"},
}};

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), static_cast<std::size_t>(converted.length()));
}

[[nodiscard]] String text(const std::string_view value) {
    return String::utf8(value.data(), static_cast<int64_t>(value.size()));
}

[[nodiscard]] std::string json(const std::string_view value) {
    std::ostringstream output;
    output << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) output << "\\u00" << hex[character >> 4] << hex[character & 0x0f];
            else output << static_cast<char>(character);
        }
    }
    output << '"';
    return output.str();
}

[[nodiscard]] bool write_bytes(const std::filesystem::path& path, const PackedByteArray& bytes) {
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.ptr()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

// One drawn line: its request, what UI-F3 made of it and what the engine measured.
struct Sample final {
    std::string kind; // "face" or "fallback"
    model::FontRequest request;
    std::string language;
    model::ResolvedFont resolved;
    std::string text;
    std::int32_t pixels{};
    std::string engine_font_name;
    std::string engine_style_name;
    float text_width{};
    float font_height{};
    Vector2 position;
    std::uint64_t ink_pixels{};
};

} // namespace

struct FontMode::State final {
    explicit State(Options value) : options(std::move(value)) {}

    Options options;
    std::string cache_source;
    std::unique_ptr<FontProvider> provider;
    std::vector<Sample> samples;
    Node3D* host{};
    std::uint32_t width{};
    std::uint32_t height{};
    int frames{warmup_frames};
    bool completed{};
    std::string status{"failed"};
    std::string failure;
    std::optional<std::array<std::int32_t, 2>> png;
    std::string capture_sha256;
    BackendInfo backend;

    [[nodiscard]] std::filesystem::path cache_directory();
    Label* add_label(CanvasLayer& layer, std::string_view value, const Ref<Font>& font,
                                   std::int32_t pixels, const Color& colour, Vector2 position);
    void build(CanvasLayer& layer);
    [[nodiscard]] bool capture();
    [[nodiscard]] bool write_report() const;
};

std::filesystem::path FontMode::State::cache_directory() {
    if (!options.font_cache.empty()) {
        cache_source = "flag";
        return options.font_cache;
    }
    const String environment = OS::get_singleton()->get_environment("EAWR_FONT_CACHE");
    if (!environment.is_empty()) {
        cache_source = "environment";
        return ViewerPath{utf8(environment)}.native();
    }
    // The extraction tool's default in the checkout that holds apps/viewer/project.
    cache_source = "checkout";
    const std::filesystem::path project = ViewerPath{utf8(ProjectSettings::get_singleton()->globalize_path("res://"))}
                                              .native();
    return (project / ".." / ".." / ".." / "out" / "fonts").lexically_normal();
}

Label* FontMode::State::add_label(CanvasLayer& layer, const std::string_view value, const Ref<Font>& font,
                                  const std::int32_t pixels, const Color& colour, const Vector2 position) {
    Label* label = memnew(Label);
    label->set_text(text(value));
    label->add_theme_font_override("font", font);
    label->add_theme_font_size_override("font_size", pixels);
    label->add_theme_color_override("font_color", colour);
    label->set_position(position);
    layer.add_child(label);
    return label;
}

void FontMode::State::build(CanvasLayer& layer) {
    ColorRect* fill = memnew(ColorRect);
    fill->set_color(background);
    fill->set_position(Vector2(0.0F, 0.0F));
    fill->set_size(Vector2(static_cast<float>(width), static_cast<float>(height)));
    layer.add_child(fill);

    const Ref<Font> plain = provider->font(model::ResolvedFont{});
    const float left = 16.0F;
    float y = 8.0F;
    const std::string header = "EAWR fonts (UI-05): " + std::to_string(provider->cache().faces.size())
        + " of 4 faces from " + provider->cache().directory + " (" + cache_source + ")";
    add_label(layer, header, plain, 14, caption_ink, Vector2(left, y));
    y += 24.0F;

    const auto add_sample = [&](Sample sample, const bool captioned, const float line_gap) {
        const Ref<Font> font = provider->font(sample.resolved);
        sample.pixels = model::font_pixel_height(sample.resolved.point_size, height);
        const std::string caption = sample.request.face + " " + std::to_string(sample.request.point_size) + " pt "
            + sample.language + " -> "
            + (sample.resolved.face.empty() ? std::string("engine default") : sample.resolved.face) + " ("
            + std::string(model::to_string(sample.resolved.source)) + "), "
            + std::to_string(sample.resolved.point_size) + " pt = " + std::to_string(sample.pixels) + " px";
        if (captioned) {
            add_label(layer, caption, plain, caption_pixels, caption_ink, Vector2(left, y));
            y += static_cast<float>(caption_pixels) + 5.0F;
        }
        sample.position = Vector2(left, y);
        add_label(layer, sample.text, font, sample.pixels, ink, sample.position);
        sample.engine_font_name = utf8(font->get_font_name());
        sample.engine_style_name = utf8(font->get_font_style_name());
        sample.text_width = font->get_string_size(text(sample.text), HORIZONTAL_ALIGNMENT_LEFT, -1.0F,
                                                  sample.pixels).x;
        sample.font_height = font->get_height(sample.pixels);
        y += std::max(sample.font_height, static_cast<float>(sample.pixels)) + line_gap;
        samples.push_back(std::move(sample));
    };

    for (const std::string_view face : model::embedded_faces) {
        for (const std::int32_t points : {face_point_size, small_point_size}) {
            Sample sample;
            sample.kind = "face";
            sample.request = {std::string(face), points};
            sample.language = "ENGLISH";
            sample.resolved = provider->resolve(sample.request, sample.language);
            const bool large = points == face_point_size;
            sample.text = large ? std::string(face_text) : std::string(lower_text) + " " + std::string(upper_text);
            add_sample(std::move(sample), large, large ? 2.0F : 8.0F);
        }
    }
    y += 6.0F;
    for (const FallbackRequest& request : fallback_requests) {
        Sample sample;
        sample.kind = "fallback";
        sample.request = {std::string(request.face), request.point_size};
        sample.language = std::string(request.language);
        sample.resolved = provider->resolve(sample.request, sample.language);
        sample.text = std::string(lower_text) + " " + std::string(upper_text);
        add_sample(std::move(sample), true, 4.0F);
    }
}

bool FontMode::State::capture() {
    Ref<Image> image = host->get_viewport()->get_texture()->get_image();
    if (image.is_null() || image->is_empty()) {
        failure = "the viewport could not be read back";
        return false;
    }
    FixedCamera requested;
    requested.width = width;
    requested.height = height;
    CaptureResult read_back;
    read_back.width = static_cast<std::uint32_t>(image->get_width());
    read_back.height = static_cast<std::uint32_t>(image->get_height());
    if (const std::string problem = capture_size_problem(read_back, requested); !problem.empty()) {
        failure = problem;
        return false;
    }
    image->convert(Image::FORMAT_RGBA8);
    const PackedByteArray pixels = image->get_data();
    const auto differs = [&](const std::int64_t x, const std::int64_t y) {
        const std::int64_t at = (y * image->get_width() + x) * 4;
        const auto channel = [&](const std::int64_t offset, const float reference) {
            return std::abs(static_cast<int>(pixels[at + offset]) - static_cast<int>(reference * 255.0F + 0.5F)) > 48;
        };
        return channel(0, background.r) || channel(1, background.g) || channel(2, background.b);
    };
    // Ink inside each sample's line: proof that its face drew glyphs.
    for (Sample& sample : samples) {
        const auto x0 = static_cast<std::int64_t>(sample.position.x);
        const auto y0 = static_cast<std::int64_t>(sample.position.y);
        const std::int64_t x1 =
            std::min<std::int64_t>(image->get_width(), x0 + static_cast<std::int64_t>(sample.text_width) + 1);
        const std::int64_t y1 =
            std::min<std::int64_t>(image->get_height(), y0 + static_cast<std::int64_t>(sample.font_height) + 1);
        for (std::int64_t y = y0; y < y1; ++y) {
            for (std::int64_t x = x0; x < x1; ++x) sample.ink_pixels += differs(x, y) ? 1U : 0U;
        }
    }
    const PackedByteArray png_bytes = image->save_png_to_buffer();
    capture_sha256 = core::sha256_hex(
        std::span<const std::uint8_t>(png_bytes.ptr(), static_cast<std::size_t>(png_bytes.size())));
    if (!options.capture_path.empty()) {
        if (!write_bytes(options.capture_path, png_bytes)) {
            failure = "the capture PNG could not be written";
            return false;
        }
        png = std::array<std::int32_t, 2>{image->get_width(), image->get_height()};
    }
    return true;
}

bool FontMode::State::write_report() const {
    if (options.report_path.empty()) return false;
    std::ostringstream output;
    output << "{\n  \"mode\": \"fonts\",\n  \"status\": " << json(status) << ",\n  \"failure\": " << json(failure)
           << ",\n  \"backend\": {\"engine\": " << json(backend.engine) << ", \"rendering_method\": "
           << json(backend.rendering_method) << ", \"adapter_vendor\": " << json(backend.adapter_vendor)
           << ", \"adapter_name\": " << json(backend.adapter_name) << ", \"driver_api\": " << json(backend.driver_api)
           << "},\n  \"viewport\": {\"width\": " << width << ", \"height\": " << height << "}";
    if (provider) {
        const model::FontCache& cache = provider->cache();
        output << ",\n  \"cache\": {\"directory\": " << json(cache.directory) << ", \"source\": "
               << json(cache_source) << ",\n    \"faces\": [";
        for (std::size_t index = 0; index < model::embedded_faces.size(); ++index) {
            const std::string_view face = model::embedded_faces[index];
            const model::CachedFace* cached = cache.find(face);
            const Ref<FontFile> file = provider->cached_font(face);
            output << (index ? ",\n      " : "\n      ") << "{\"face\": " << json(face) << ", \"loaded\": "
                   << (cached != nullptr && file.is_valid() ? "true" : "false");
            if (cached != nullptr) {
                output << ", \"file\": " << json(cached->logical_path) << ", \"bytes\": "
                       << cached->bytes.size() << ", \"sha256\": " << json(cached->sha256) << ", \"full_name\": "
                       << json(cached->names.full_name) << ", \"family\": " << json(cached->names.family);
            }
            if (file.is_valid()) {
                output << ", \"engine_font_name\": " << json(utf8(file->get_font_name()))
                       << ", \"engine_style_name\": " << json(utf8(file->get_font_style_name()));
            }
            output << "}";
        }
        output << "],\n    \"diagnostics\": [";
        for (std::size_t index = 0; index < cache.diagnostics.size(); ++index) {
            const core::Diagnostic& diagnostic = cache.diagnostics[index];
            output << (index ? ",\n      " : "\n      ") << "{\"code\": " << json(diagnostic.code)
                   << ", \"severity\": " << json(core::to_string(diagnostic.severity)) << ", \"message\": "
                   << json(diagnostic.message) << "}";
        }
        const std::vector<std::string>& families = provider->system_families();
        output << "]},\n  \"system\": {\"families\": " << families.size() << ", \"unicode_face\": "
               << (model::match_system_face(model::unicode_face, families) ? "true" : "false") << "}";
    }
    output << ",\n  \"samples\": [";
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const Sample& sample = samples[index];
        output << (index ? ",\n    " : "\n    ") << "{\"kind\": " << json(sample.kind) << ", \"requested\": "
               << json(sample.request.face) << ", \"point_size\": " << sample.request.point_size << ", \"language\": "
               << json(sample.language) << ", \"resolved\": " << json(sample.resolved.face) << ", \"source\": "
               << json(model::to_string(sample.resolved.source)) << ", \"resolved_point_size\": "
               << sample.resolved.point_size << ", \"pixels\": " << sample.pixels << ", \"substituted\": "
               << (sample.resolved.substituted ? "true" : "false") << ", \"unavailable\": [";
        for (std::size_t tried = 0; tried < sample.resolved.unavailable.size(); ++tried) {
            output << (tried ? ", " : "") << json(sample.resolved.unavailable[tried]);
        }
        output << "], \"engine_font_name\": " << json(sample.engine_font_name) << ", \"engine_style_name\": "
               << json(sample.engine_style_name) << ", \"text_width\": " << sample.text_width
               << ", \"font_height\": " << sample.font_height << ", \"ink_pixels\": " << sample.ink_pixels << "}";
    }
    output << "],\n  \"capture\": {\"png\": "
           << (png ? "{\"width\": " + std::to_string((*png)[0]) + ", \"height\": " + std::to_string((*png)[1]) + "}"
                   : std::string("null"))
           << ", \"sha256\": " << json(capture_sha256) << "}\n}\n";
    std::error_code error;
    if (!options.report_path.parent_path().empty()) {
        std::filesystem::create_directories(options.report_path.parent_path(), error);
    }
    std::ofstream file(options.report_path, std::ios::binary | std::ios::trunc);
    file << output.str();
    return static_cast<bool>(file);
}

bool FontMode::requested() {
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index < arguments.size(); ++index) {
        if (utf8(arguments[index]) == "--eawr-fonts") return true;
    }
    return false;
}

FontMode::Options FontMode::from_command_line() {
    Options options;
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index + 1 < arguments.size(); ++index) {
        const std::string argument = utf8(arguments[index]);
        const std::string value = utf8(arguments[index + 1]);
        bool consumed = true;
        if (argument == "--eawr-font-cache") options.font_cache = ViewerPath{value}.native();
        else if (argument == "--eawr-report") options.report_path = ViewerPath{value}.native();
        else if (argument == "--eawr-capture") options.capture_path = ViewerPath{value}.native();
        else consumed = false;
        if (consumed) ++index;
    }
    return options;
}

FontMode::FontMode(Options options) : state_(std::make_unique<State>(std::move(options))) {}
FontMode::~FontMode() = default;
FontMode::FontMode(FontMode&&) noexcept = default;
FontMode& FontMode::operator=(FontMode&&) noexcept = default;

bool FontMode::ready(Node3D& host) {
    State& state = *state_;
    state.host = &host;
    RenderingServer* rendering = RenderingServer::get_singleton();
    state.backend = {"Godot 4.7.2-stable", utf8(rendering->get_current_rendering_method()),
                     utf8(rendering->get_video_adapter_vendor()), utf8(rendering->get_video_adapter_name()),
                     utf8(rendering->get_video_adapter_api_version())};
    const Vector2 size = host.get_viewport()->get_visible_rect().size;
    if (size.x < 1.0F || size.y < 1.0F) {
        state.failure = "the viewport has no size";
        state.completed = true;
        static_cast<void>(state.write_report());
        return false;
    }
    state.width = static_cast<std::uint32_t>(size.x);
    state.height = static_cast<std::uint32_t>(size.y);
    pin_capture_viewport(*host.get_window(), state.width, state.height);
    // The cache is a loose VFS layer at fonts/; a missing directory is an empty cache.
    const std::filesystem::path directory = state.cache_directory();
    const vfs::MountSpec mount{.layer_id = "font-cache", .data_root = directory,
                               .loose_logical_prefix = std::string(model::font_cache_prefix),
                               .active_archives = {}};
    const auto mounted = vfs::Vfs::mount(std::span<const vfs::MountSpec>(&mount, 1));
    const vfs::Vfs unmounted;
    model::FontCache cache = model::load_font_cache(mounted ? mounted.value() : unmounted,
                                                    ViewerPath::utf8(directory));
    std::error_code error;
    if (!mounted && std::filesystem::is_directory(directory, error)) {
        core::Diagnostic diagnostic = mounted.error();
        diagnostic.severity = core::Severity::warning;
        cache.diagnostics.insert(cache.diagnostics.begin(), std::move(diagnostic));
    }
    state.provider = std::make_unique<FontProvider>(std::move(cache));
    CanvasLayer* layer = memnew(CanvasLayer);
    host.add_child(layer);
    state.build(*layer);
    return true;
}

std::optional<int> FontMode::process() {
    State& state = *state_;
    if (state.completed) return std::nullopt;
    if (state.frames > 0) {
        --state.frames;
        return std::nullopt;
    }
    state.completed = true;
    state.status = state.capture() ? "captured" : "failed";
    const bool written = state.write_report();
    return state.failure.empty() && written ? 0 : 2;
}

} // namespace eawr::presentation::godot_backend
