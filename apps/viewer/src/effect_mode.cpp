#include "effect_mode.hpp"
#include "effect_mode_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace {

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), converted.length());
}

} // namespace

namespace effect_mode_detail {

[[nodiscard]] std::string hash_bytes(const std::span<const std::byte> bytes) {
    return sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

} // namespace effect_mode_detail

namespace {

// `--eawr-effect-detach-frame <uint32>`: the zero-based advance index before
// which the effect is detached. It is read here, beside the mode, so the
// shared option struct and host stay untouched; absent means no schedule. A
// trailing flag with no value yields an empty string, which the parser rejects
// as an option error instead of silently running without a detach.
[[nodiscard]] std::optional<std::string> detach_frame_argument() {
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    std::optional<std::string> value;
    for (int64_t index = 0; index < arguments.size(); ++index) {
        if (utf8(arguments[index]) != "--eawr-effect-detach-frame") continue;
        value = index + 1 < arguments.size() ? utf8(arguments[index + 1]) : std::string{};
    }
    return value;
}

// `--eawr-effect-visibility <respawn|stay-detached>`: drive the effect's
// lifecycle from the host bone's sampled visibility. Absent means the host's
// visibility is ignored, exactly as before. A trailing flag yields an empty
// value, which is an option error.
[[nodiscard]] std::optional<std::string> visibility_argument() {
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    std::optional<std::string> value;
    for (int64_t index = 0; index < arguments.size(); ++index) {
        if (utf8(arguments[index]) != "--eawr-effect-visibility") continue;
        value = index + 1 < arguments.size() ? utf8(arguments[index + 1]) : std::string{};
    }
    return value;
}

[[nodiscard]] std::optional<std::uint32_t> parse_frame_index(const std::string_view text) {
    if (text.empty() || text.size() > 10) return std::nullopt;
    std::uint64_t value{};
    for (const char digit : text) {
        if (digit < '0' || digit > '9') return std::nullopt;
        value = value * 10U + static_cast<std::uint64_t>(digit - '0');
    }
    if (value > 0xffffffffULL) return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

} // namespace

bool EffectMode::requested() {
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index < arguments.size(); ++index) {
        if (utf8(arguments[index]) == "--eawr-effect") return true;
    }
    return false;
}

EffectMode::Options EffectMode::from_command_line() {
    Options options;
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index < arguments.size(); ++index) {
        const std::string argument = utf8(arguments[index]);
        if (argument == "--eawr-profile" && index + 1 >= arguments.size()) {
            options.profile_error = "missing value for --eawr-profile (expected eaw, foc or remake)";
            break;
        }
        if (index + 1 >= arguments.size()) continue;
        const std::string value = utf8(arguments[index + 1]);
        const auto number = [&](auto& target) {
            try {
                if constexpr (std::is_same_v<std::decay_t<decltype(target)>, float>) target = std::stof(value);
                else target = static_cast<std::uint32_t>(std::stoul(value));
            } catch (const std::exception&) {
                target = {};
            }
        };
        bool consumed = true;
        if (argument == "--eawr-game-root") options.game_root = ViewerPath{value}.native();
        else if (argument == "--eawr-mod-root") options.mod_root = ViewerPath{value}.native();
        else if (argument == "--eawr-profile") options.profile = value;
        else if (argument == "--eawr-effect") options.effect_path = value;
        else if (argument == "--eawr-effect-attach") options.attach = value;
        else if (argument == "--eawr-effect-proxy-host") options.proxy_host = value;
        else if (argument == "--eawr-effect-proxy") options.proxy_name = value;
        else if (argument == "--eawr-animation") options.animation_path = value;
        else if (argument == "--eawr-effect-seed") number(options.seed);
        else if (argument == "--eawr-effect-frames") number(options.frames);
        else if (argument == "--eawr-effect-dt") number(options.delta_seconds);
        else if (argument == "--eawr-effect-capacity") number(options.capacity);
        else if (argument == "--eawr-report") options.report_path = ViewerPath{value}.native();
        else if (argument == "--eawr-capture") options.capture_path = ViewerPath{value}.native();
        else consumed = false;
        if (consumed) ++index;
    }
    return options;
}

EffectMode::EffectMode(Options options) : state_(std::make_unique<State>(std::move(options))) {}
EffectMode::~EffectMode() = default;
EffectMode::EffectMode(EffectMode&&) noexcept = default;
EffectMode& EffectMode::operator=(EffectMode&&) noexcept = default;

bool EffectMode::ready(Node3D& host) {
    State& state = *state_;
    const auto give_up = [&](std::string message) {
        state.failure = std::move(message);
        state.status = "failed";
        static_cast<void>(state.write_report());
        return false;
    };
    const Options& options = state.options;
    if (!options.profile_error.empty()) {
        UtilityFunctions::printerr(String(options.profile_error.c_str()));
        return give_up(options.profile_error);
    }
    if (!options.profile.empty() && options.profile != "eaw"
        && options.profile != "foc" && options.profile != "remake") {
        constexpr std::string_view failure = "--eawr-profile must be eaw, foc or remake";
        UtilityFunctions::printerr(String(failure.data()));
        return give_up(std::string(failure));
    }
    if (options.frames < 2 || options.frames > 3600 || !(options.delta_seconds > 0.0F)
        || !std::isfinite(options.delta_seconds) || options.delta_seconds > 1.0F
        || options.capacity == 0 || options.capacity > 1'000'000U) {
        return give_up("effect mode needs 2-3600 frames, a step in (0, 1] s and a capacity in 1-1000000");
    }
    if (const auto requested_detach = detach_frame_argument()) {
        const auto parsed = parse_frame_index(*requested_detach);
        if (!parsed || *parsed >= options.frames) {
            return give_up("--eawr-effect-detach-frame needs an unsigned frame index below --eawr-effect-frames");
        }
        state.detach_frame = *parsed;
    }
    if (const auto requested_visibility = visibility_argument()) {
        if (*requested_visibility == "respawn") state.visibility = particles::ReappearancePolicy::respawn;
        else if (*requested_visibility == "stay-detached") state.visibility = particles::ReappearancePolicy::stay_detached;
        else return give_up("--eawr-effect-visibility expects respawn or stay-detached");
        if (state.detach_frame) {
            return give_up("--eawr-effect-visibility cannot be combined with --eawr-effect-detach-frame");
        }
        if (options.attach.empty() && options.proxy_name.empty()) {
            return give_up("--eawr-effect-visibility requires --eawr-effect-attach or a proxy host");
        }
    }
    if ((!options.attach.empty() && (!options.proxy_host.empty() || !options.proxy_name.empty()))
        || options.proxy_host.empty() != options.proxy_name.empty()) {
        return give_up("choose either --eawr-effect-attach or both --eawr-effect-proxy-host and --eawr-effect-proxy");
    }
    if (!options.animation_path.empty() && options.attach.empty() && options.proxy_name.empty())
        return give_up("--eawr-animation requires an attachment or proxy host");

    // Layer selection follows the installed shape, exactly as the map mode.
    const std::filesystem::path expansion = options.game_root / "corruption" / "Data";
    const std::filesystem::path base = options.game_root / "GameData" / "Data";
    if (!std::filesystem::is_directory(base)) return give_up("effect mode requires a root with GameData/Data");
    std::vector<std::pair<std::string, std::filesystem::path>> roots;
    state.profile = options.profile.empty() ? (!options.mod_root.empty() ? "remake"
        : std::filesystem::is_directory(expansion) ? "foc" : "eaw") : options.profile;
    if (state.profile != "eaw" && state.profile != "foc" && state.profile != "remake") {
        return give_up("--eawr-profile must be eaw, foc or remake");
    }
    if (state.profile == "remake") {
        if (options.mod_root.empty()) return give_up("the Remake profile requires --eawr-mod-root");
        if (!std::filesystem::is_directory(expansion)) return give_up("the Remake profile requires corruption/Data");
        for (const auto& layer : eawr::vfs::mod_chain_roots(options.mod_root)) roots.push_back(layer);
        roots.emplace_back("expansion", expansion);
        roots.emplace_back("base", base);
    } else {
        if (state.profile == "foc") {
            if (!std::filesystem::is_directory(expansion)) return give_up("the FoC profile requires corruption/Data");
            roots.emplace_back("expansion", expansion);
        }
        roots.emplace_back("base", base);
    }
    std::vector<vfs::MountSpec> specs;
    auto chain = vfs::resolve_manifest_chain(roots);
    if (!chain) return give_up(core::format_diagnostic(chain.error()));
    for (auto& manifest : chain.value()) {
        state.layers.push_back(manifest.mount.layer_id);
        specs.push_back(std::move(manifest.mount));
    }
    auto filesystem = vfs::Vfs::mount(specs);
    if (!filesystem) return give_up(core::format_diagnostic(filesystem.error()));
    state.filesystem.emplace(std::move(filesystem.value()));

    auto bytes = state.filesystem->open(options.effect_path);
    if (!bytes) return give_up(core::format_diagnostic(bytes.error()));
    state.effect_hash = hash_bytes(bytes.value());
    auto loaded = particles::load_alo(bytes.value(), options.effect_path);
    if (!loaded) return give_up(core::format_diagnostic(loaded.error()));
    state.system = std::move(loaded.value());
    if (state.system.emitters.empty()) return give_up("the effect declares no emitter");
    state.plans = particles::plan_system(state.system);

    if (!options.attach.empty() || !options.proxy_name.empty()) {
        if (!options.attach.empty()) {
            const std::size_t separator = options.attach.rfind(':');
            if (separator == std::string::npos || separator == 0 || separator + 1 == options.attach.size()) {
                return give_up("--eawr-effect-attach expects <model logical path>:<bone>");
            }
            state.attach_model = options.attach.substr(0, separator);
            state.attach_bone = options.attach.substr(separator + 1);
        } else {
            state.attach_model = options.proxy_host;
        }
        auto model_bytes = state.filesystem->open(state.attach_model);
        if (!model_bytes) return give_up(core::format_diagnostic(model_bytes.error()));
        state.attach_model_hash = hash_bytes(model_bytes.value());
        auto model = assets::load_model(*state.filesystem, state.attach_model);
        if (!model) return give_up(core::format_diagnostic(model.error()));
        if (!options.proxy_name.empty()) {
            auto selected = particles::bind_proxy_mesh(model.value(), options.proxy_name);
            if (!selected) return give_up(core::format_diagnostic(selected.error()));
            state.proxy_mesh.emplace(std::move(selected.value()));
        }
        std::optional<assets::Animation> clip;
        if (!options.animation_path.empty()) {
            auto clip_bytes = state.filesystem->open(options.animation_path);
            if (!clip_bytes) return give_up(core::format_diagnostic(clip_bytes.error()));
            state.animation_hash = hash_bytes(clip_bytes.value());
            auto loaded_clip = assets::load_animation(*state.filesystem, options.animation_path);
            if (!loaded_clip) return give_up(core::format_diagnostic(loaded_clip.error()));
            clip = std::move(loaded_clip.value());
        }
        auto player = animation::Player::create(model.value(), clip ? &*clip : nullptr);
        if (!player) return give_up(core::format_diagnostic(player.error()));
        state.player.emplace(std::move(player.value()));
        if (state.visibility) {
            // The reference reads the proxy bone's own visibility track; no
            // parent visibility is inherited (RenderObject::Update).
            if (state.proxy_mesh) {
                state.visibility_bone = state.proxy_mesh->proxy_bone;
            } else {
                const auto& bones = model.value().bones;
                const auto found = std::find_if(bones.begin(), bones.end(),
                    [&](const assets::Bone& bone) { return bone.name == state.attach_bone; });
                if (found != bones.end()) state.visibility_bone = static_cast<std::size_t>(found - bones.begin());
            }
            if (state.visibility_bone) state.visibility_bone_name = model.value().bones[*state.visibility_bone].name;
        }
        // A missing or ambiguous bone is a diagnostic here, never an origin fallback.
        if (auto first = state.host_frames(0.0F); !first) {
            return give_up(core::format_diagnostic(first.error()));
        }
    }

    if (!state.fit_camera()) return give_up(state.failure);

    // Every run reads its fixed camera back, written or not, so the root
    // viewport is drawn at the camera's size whatever the OS window.
    pin_capture_viewport(*host.get_window(), state.camera.width, state.camera.height);
    state.renderer = std::make_unique<GodotRenderer>(host);
    // The renderer reads back the last drawn frame, so the capture camera is
    // live before the first particle frame.
    state.renderer->set_camera(state.camera);
    state.backend = std::make_unique<GodotParticleBackend>(host,
        [&state](const std::string_view name) { return state.resolve(name); });
    state.registry = std::make_unique<particles::EffectRegistry>(*state.backend);
    if (state.visibility) {
        // Generations spawn when the host is visible; the plans are the dry
        // run's, resolved by the same texture resolver the backend uses.
        state.visibility_life.emplace(state.make_visibility_life(*state.registry));
        if (std::none_of(state.plans.begin(), state.plans.end(),
                [](const particles::EmitterRenderPlan& plan) { return plan.drawable; })) {
            return give_up("no emitter of this effect is drawable; every cause is in the emitter list");
        }
        return true;
    }
    auto handle = state.spawn(*state.registry);
    if (!handle) return give_up(core::format_diagnostic(handle.error()));
    state.handle = handle.value();
    state.plans = *state.registry->plans(state.handle);
    if (std::none_of(state.plans.begin(), state.plans.end(),
            [](const particles::EmitterRenderPlan& plan) { return plan.drawable; })) {
        return give_up("no emitter of this effect is drawable; every cause is in the emitter list");
    }
    return true;
}

std::optional<int> EffectMode::process() {
    State& state = *state_;
    if (state.completed || !state.registry) return std::nullopt;
    const Options& options = state.options;
    // A capture holds the particle frame for two presented frames so the
    // renderer reads back a frame drawn from exactly that frame's streams.
    if (state.hold > 0) {
        if (--state.hold > 0) return std::nullopt;
        if (state.pending_capture == "final") return state.finish();
        state.capture_intermediate();
        return std::nullopt;
    }
    const std::uint32_t index = state.next_index++;
    VisibilityFrame event;
    auto record = state.visibility_life
        ? state.visibility_step(*state.registry, *state.visibility_life, index, state.camera_frame, true, event)
        : state.step(*state.registry, state.handle, state.lifecycle, index, state.camera_frame, true);
    if (!record) {
        state.completed = true;
        state.failure = core::format_diagnostic(record.error());
        static_cast<void>(state.write_report());
        return 2;
    }
    const auto now = std::chrono::steady_clock::now();
    if (index == 0) state.first_frame = now;
    state.last_frame = now;
    state.records.push_back(std::move(record.value()));
    if (state.visibility_life) {
        state.visibility_frames.push_back(std::move(event));
        state.peak_live_rids = std::max(state.peak_live_rids, state.backend->live_rids());
    }
    if (index + 1 == options.frames) {
        state.hold = 3;
        state.pending_capture = "final";
        return std::nullopt;
    }
    for (const auto& [label, planned] : state.capture_plan) {
        if (planned != index) continue;
        state.hold = 3;
        state.pending_capture = label;
    }
    return std::nullopt;
}

} // namespace eawr::presentation::godot_backend
