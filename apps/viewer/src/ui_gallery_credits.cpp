#include "ui_gallery_internal.hpp"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

// `--eawr-credits <script|script-click|live>` (MNU-07): the credits screen on the gallery's data.
// `script` lets the roll and the movie run for a few seconds, checks that both advanced, then skips with
// Esc through Godot's real input dispatch and checks that `closed` fired; `script-click` skips with a
// click. `live` leaves it open until the roll ends or is skipped. The movie comes from
// `--eawr-movie-cache`; without it the roll plays over the plain backdrop and the report says so.
namespace eawr::presentation::godot_backend {
namespace {

// Long enough for the text to climb into view and the movie to show several frames.
constexpr std::chrono::milliseconds run_time{5000};

void send_escape() {
    for (const bool pressed : {true, false}) {
        Ref<InputEventKey> event;
        event.instantiate();
        event->set_keycode(KEY_ESCAPE);
        event->set_physical_keycode(KEY_ESCAPE);
        event->set_pressed(pressed);
        Input::get_singleton()->parse_input_event(event);
    }
}

void send_click(Viewport& viewport, const Vector2 local) {
    const Vector2 window = viewport.get_final_transform().xform(local);
    Ref<InputEventMouseMotion> motion;
    motion.instantiate();
    motion->set_position(window);
    motion->set_global_position(window);
    Input::get_singleton()->parse_input_event(motion);
    for (const bool pressed : {true, false}) {
        Ref<InputEventMouseButton> event;
        event.instantiate();
        event->set_button_index(MOUSE_BUTTON_LEFT);
        event->set_pressed(pressed);
        event->set_factor(1.0F);
        event->set_position(window);
        event->set_global_position(window);
        Input::get_singleton()->parse_input_event(event);
    }
}

} // namespace

UiGalleryMode::State* UiGalleryMode::State::credits_active = nullptr;

void UiGalleryMode::State::on_credits_closed() {
    if (credits_active != nullptr) credits_active->credits_closed = true;
}

bool UiGalleryMode::State::credits_alive() {
    if (credits_screen != nullptr && !UtilityFunctions::is_instance_id_valid(credits_id)) credits_screen = nullptr;
    return credits_screen != nullptr;
}

void UiGalleryMode::State::sample_credits() {
    if (!credits_alive()) return;
    credits.lines = credits_screen->line_count();
    credits.logos = credits_screen->logos_loaded();
    credits.moved = credits_screen->scroll_moved();
    credits.elapsed = credits_screen->elapsed();
    VideoStreamPlayer* player = credits_screen->movie_player();
    if (player == nullptr || credits_ticks++ % 5 != 0) return;
    const Ref<Texture2D> texture = player->get_video_texture();
    if (texture.is_null()) return;
    credits.texture = {texture->get_width(), texture->get_height()};
    const Ref<Image> frame = texture->get_image();
    if (frame.is_null() || frame->is_empty()) return;
    const PackedByteArray bytes = frame->get_data();
    credits_frame_hashes.insert(core::sha256_hex({bytes.ptr(), static_cast<std::size_t>(bytes.size())}));
}

void UiGalleryMode::State::build_credits_page(Control& root) {
    static_cast<void>(root);
    const std::string& mode = options.credits;
    if (mode != "script" && mode != "script-click" && mode != "live") {
        failure = "--eawr-credits expects script, script-click or live";
        return;
    }
    credits_active = this;
    // The first frame is not the capture: the text has not climbed into view yet (the script captures
    // after the run).
    box_captured = true;

    CreditsContext context;
    context.filesystem = &*filesystem;
    context.fonts = fonts.get();
    const std::u8string cache = options.movie_cache.generic_u8string();
    context.movie_cache = String::utf8(reinterpret_cast<const char*>(cache.data()), static_cast<int64_t>(cache.size()));
    context.viewport_size = Vector2(static_cast<float>(width), static_cast<float>(height));
    credits_screen = memnew(EawrCreditsScreen);
    if (!credits_screen->show(*layer, context)) {
        failure = credits_screen->failure();
        memdelete(credits_screen);
        credits_screen = nullptr;
        return;
    }
    credits_id = static_cast<int64_t>(credits_screen->get_instance_id());
    credits_screen->connect("closed", callable_mp_static(&State::on_credits_closed));
    credits.opened = true;
    credits.notes = credits_screen->notes();
    if (credits_screen->movie()) {
        credits.movie = true;
        credits.movie_name = credits_screen->movie()->name;
        credits.movie_source = credits_screen->movie()->source.canonical_path;
    }
    if (mode == "live") return;

    Viewport* viewport = layer->get_viewport();
    const bool click = mode == "script-click";
    const Vector2 centre(static_cast<float>(width) * 0.5F, static_cast<float>(height) * 0.5F);
    BoxStep running;
    running.name = "the roll and the movie advance";
    running.act = [] {};
    running.wait_time = run_time;
    running.check = [this]() -> std::string {
        if (!credits_alive()) return "the screen closed on its own";
        sample_credits();
        if (!credits_screen->rolling()) return "no credits text was rolled: " + credits_screen->failure();
        if (credits_screen->scroll_moved() <= 0.0F) return "the text did not scroll";
        // A movie the cache lacks is a noted fallback, not a failure: the roll plays over the backdrop.
        if (credits.movie && credits_screen->movie_player() == nullptr && credits.notes.empty()) {
            return "the movie was not attached and nothing says why";
        }
        if (credits_screen->movie_player() != nullptr && credits_frame_hashes.size() < 2) {
            return "the movie showed " + std::to_string(credits_frame_hashes.size()) + " distinct frame(s)";
        }
        // The capture is the screen as the player sees it, mid-roll.
        if (!options.capture_path.empty() && !capture()) return "the capture failed";
        return {};
    };
    box_steps.push_back(std::move(running));
    box_steps.push_back({click ? "a click skips and closed fires" : "Esc skips and closed fires",
                         [this, viewport, click, centre] {
                             if (click) send_click(*viewport, centre);
                             else send_escape();
                         },
                         [this]() -> std::string {
                             if (!credits_closed) return "closed did not fire";
                             return {};
                         }});
}

} // namespace eawr::presentation::godot_backend
