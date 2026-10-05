#include "ui_gallery_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace {

const Color backdrop(0.03F, 0.05F, 0.11F);

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), static_cast<std::size_t>(converted.length()));
}









} // namespace



std::filesystem::path UiGalleryMode::State::cache_directory() {
    if (!options.font_cache.empty()) {
        font_cache_source = "flag";
        return options.font_cache;
    }
    const String environment = OS::get_singleton()->get_environment("EAWR_FONT_CACHE");
    if (!environment.is_empty()) {
        font_cache_source = "environment";
        return ViewerPath{utf8(environment)}.native();
    }
    font_cache_source = "checkout";
    const std::filesystem::path project = ViewerPath{utf8(ProjectSettings::get_singleton()->globalize_path("res://"))}
                                              .native();
    return (project / ".." / ".." / ".." / "out" / "fonts").lexically_normal();
}

bool UiGalleryMode::State::load() {
    if (options.game_root.empty()) {
        failure = "--eawr-ui-gallery requires --eawr-game-root";
        return false;
    }
    // The project targets FoC: the expansion over the base game, and a mod
    // (its folder or its Data folder) over both when one is given.
    const std::filesystem::path expansion = options.game_root / "corruption" / "Data";
    const std::filesystem::path base = options.game_root / "GameData" / "Data";
    if (!std::filesystem::is_directory(expansion) || !std::filesystem::is_directory(base)) {
        failure = "the game root needs corruption/Data and GameData/Data (FoC)";
        return false;
    }
    std::vector<std::pair<std::string, std::filesystem::path>> roots;
    if (!options.mod_root.empty()) {
        const std::filesystem::path data = options.mod_root / "Data";
        roots.emplace_back("mod", std::filesystem::is_directory(data) ? data : options.mod_root);
    }
    roots.emplace_back("expansion", expansion);
    roots.emplace_back("base", base);
    std::vector<vfs::MountSpec> specs;
    for (const auto& [id, root] : roots) {
        auto manifest = vfs::resolve_manifest_mount(id, root);
        if (!manifest) {
            failure = core::format_diagnostic(manifest.error());
            return false;
        }
        specs.push_back(std::move(manifest.value().mount));
    }
    auto mounted = vfs::Vfs::mount(specs);
    if (!mounted) {
        failure = core::format_diagnostic(mounted.error());
        return false;
    }
    filesystem.emplace(std::move(mounted.value()));

    // The HUD page loads its own shell, textures and fonts (TacticalHud).
    if (!options.hud.empty()) return true;

    // The movie page needs neither the skin nor the fonts.
    if (!options.movie.empty()) {
        if (options.movie_cache.empty()) {
            failure = "--eawr-ui-movie requires --eawr-movie-cache";
            return false;
        }
        auto resolved = data::resolve_hud_movie(*filesystem, options.movie);
        if (!resolved) {
            failure = core::format_diagnostic(resolved.error());
            return false;
        }
        movie.emplace(std::move(resolved.value()));
        frames = movie_frames;
        return true;
    }

    auto loaded_catalog = data::load_dialog_catalog(*filesystem);
    if (!loaded_catalog) {
        failure = core::format_diagnostic(loaded_catalog.error());
        return false;
    }
    catalog.emplace(std::move(loaded_catalog.value()));
    const std::string mtd = "Data/Art/Textures/" + catalog->skin.texture_file + ".mtd";
    auto loaded_atlas = assets::load_mega_texture_atlas(*filesystem, mtd);
    if (!loaded_atlas) {
        failure = core::format_diagnostic(loaded_atlas.error());
        return false;
    }
    atlas.emplace(std::move(loaded_atlas.value()));
    if (auto loaded_text = data::load_language_text_database(*filesystem, "ENGLISH")) {
        text_database.emplace(std::move(loaded_text.value()));
        text_lookup.emplace(*text_database);
    } else {
        notes.push_back("text database: " + core::format_diagnostic(loaded_text.error()) + "; captions show their keys");
    }

    // The font cache is its own loose layer at fonts/; a missing directory is an empty cache.
    const std::filesystem::path directory = cache_directory();
    const vfs::MountSpec mount{.layer_id = "font-cache", .data_root = directory,
                               .loose_logical_prefix = std::string(model::font_cache_prefix), .active_archives = {}};
    auto font_mount = vfs::Vfs::mount(std::span<const vfs::MountSpec>(&mount, 1));
    if (font_mount) font_filesystem.emplace(std::move(font_mount.value()));
    const vfs::Vfs unmounted;
    fonts = std::make_unique<FontProvider>(
        model::load_font_cache(font_filesystem ? *font_filesystem : unmounted, ViewerPath::utf8(directory)));

    const model::LayoutRules rules = options.rules == "retail" ? model::LayoutRules::retail
                                                                : model::LayoutRules::aspect_correct;
    model::ThemeSources sources;
    sources.catalog = &*catalog;
    sources.atlas = &atlas->directory;
    sources.standalone = model::vfs_standalone_textures(*filesystem);
    sources.fonts = &fonts->cache();
    const std::vector<std::string>& families = fonts->system_families();
    sources.system = [&families](const std::string_view face) {
        return model::match_system_face(face, families).has_value();
    };
    theme_model.emplace(model::build_theme_model(sources, model::reference_space({width, height}, rules)));
    textures = std::make_unique<UiTextures>(*atlas, *filesystem);
    if (!textures->page_failure().empty()) {
        failure = "atlas page: " + textures->page_failure();
        return false;
    }
    theme = build_theme(*theme_model, *textures, *fonts, &summary);
    return true;
}







// The movie at twice its HUD slot size, centred, with a lighter band behind
// its right half so the capture shows what the alpha lets through.




// Counts loops (the position jumps back) and hashes every tenth frame, so the
// report shows that the stream advances.






bool UiGalleryMode::requested() {
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index < arguments.size(); ++index) {
        if (utf8(arguments[index]) == "--eawr-ui-gallery") return true;
    }
    return false;
}

UiGalleryMode::Options UiGalleryMode::from_command_line() {
    Options options;
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index + 1 < arguments.size(); ++index) {
        const std::string argument = utf8(arguments[index]);
        const std::string value = utf8(arguments[index + 1]);
        bool consumed = true;
        if (argument == "--eawr-game-root") options.game_root = ViewerPath{value}.native();
        else if (argument == "--eawr-mod-root") options.mod_root = ViewerPath{value}.native();
        else if (argument == "--eawr-font-cache") options.font_cache = ViewerPath{value}.native();
        else if (argument == "--eawr-report") options.report_path = ViewerPath{value}.native();
        else if (argument == "--eawr-capture") options.capture_path = ViewerPath{value}.native();
        else if (argument == "--eawr-ui-dialog") options.dialog = value;
        else if (argument == "--eawr-ui-movie") options.movie = value;
        else if (argument == "--eawr-movie-cache") options.movie_cache = ViewerPath{value}.native();
        else if (argument == "--eawr-ui-rules") options.rules = value;
        else if (argument == "--eawr-ui-hud") options.hud = value;
        else if (argument == "--eawr-ui-hud-map") options.hud_map = value;
        else if (argument == "--eawr-ui-hud-cards") options.hud_cards = value;
        else consumed = false;
        if (consumed) ++index;
    }
    return options;
}

UiGalleryMode::UiGalleryMode(Options options) : state_(std::make_unique<State>(std::move(options))) {}
UiGalleryMode::~UiGalleryMode() = default;
UiGalleryMode::UiGalleryMode(UiGalleryMode&&) noexcept = default;
UiGalleryMode& UiGalleryMode::operator=(UiGalleryMode&&) noexcept = default;

bool UiGalleryMode::ready(Node3D& host) {
    State& state = *state_;
    state.host = &host;
    RenderingServer* rendering = RenderingServer::get_singleton();
    state.backend = {"Godot 4.7.2-stable", utf8(rendering->get_current_rendering_method()),
                     utf8(rendering->get_video_adapter_vendor()), utf8(rendering->get_video_adapter_name()),
                     utf8(rendering->get_video_adapter_api_version())};
    const auto fail = [&]() {
        state.completed = true;
        static_cast<void>(state.write_report());
        return false;
    };
    if (state.options.rules != "aspect" && state.options.rules != "retail") {
        state.failure = "--eawr-ui-rules must be aspect or retail";
        return fail();
    }
    const Vector2 size = host.get_viewport()->get_visible_rect().size;
    if (size.x < 1.0F || size.y < 1.0F) {
        state.failure = "the viewport has no size";
        return fail();
    }
    state.width = static_cast<std::uint32_t>(size.x);
    state.height = static_cast<std::uint32_t>(size.y);
    pin_capture_viewport(*host.get_window(), state.width, state.height);
    if (!state.load()) return fail();

    CanvasLayer* layer = memnew(CanvasLayer);
    host.add_child(layer);
    auto* root = memnew(Control);
    root->set_position(Vector2());
    root->set_size(size);
    root->set_theme(state.theme);
    root->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    layer->add_child(root);
    ColorRect* fill = memnew(ColorRect);
    fill->set_color(backdrop);
    fill->set_size(size);
    fill->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    root->add_child(fill);
    if (!state.options.hud.empty()) state.build_hud_page(host);
    else if (state.movie) state.build_movie_page(*root, size);
    else if (state.options.dialog.empty()) state.build_controls(*root);
    else state.build_dialog_page(*root);
    if (!state.failure.empty()) return fail();
    return true;
}

std::optional<int> UiGalleryMode::process() {
    State& state = *state_;
    if (state.completed) return std::nullopt;
    if (state.movie_player) state.sample_movie();
    if (state.frames > 0) {
        --state.frames;
        return std::nullopt;
    }
    if (state.movie_player && std::chrono::steady_clock::now() < state.movie_deadline) return std::nullopt;
    state.completed = true;
    if (state.movie_player) {
        const auto decoded = validate_hud_movie(*state.movie_player, *state.movie);
        if (!decoded) state.failure = core::format_diagnostic(decoded.error());
    }
    state.status = state.capture() ? "captured" : "failed";
    const bool written = state.write_report();
    return state.failure.empty() && written ? 0 : 2;
}

} // namespace eawr::presentation::godot_backend
