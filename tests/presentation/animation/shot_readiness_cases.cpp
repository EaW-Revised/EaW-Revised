#include "shot_readiness_support.hpp"

namespace shot_readiness_test_support {

namespace playback = eawr::presentation::animation;
constexpr float moved_epsilon = 1.0e-4F;

[[nodiscard]] std::string hash(const std::span<const std::byte> bytes) {
    return eawr::sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}
[[nodiscard]] bool is_sha256(const std::string_view value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}
[[nodiscard]] std::string compiler_identity() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_FULL_VER);
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#else
    return "unknown";
#endif
}
[[nodiscard]] bool inside(const std::filesystem::path& path, const std::filesystem::path& root) {
    std::error_code error;
    const auto candidate = std::filesystem::weakly_canonical(path, error);
    if (error) return true;
    const auto base = std::filesystem::weakly_canonical(root, error);
    if (error) return true;
    const auto mismatch = std::mismatch(base.begin(), base.end(), candidate.begin(), candidate.end());
    return mismatch.first == base.end();
}

// Receipt formatting: fixed key order, %.9g floats, no clock or host path.
[[nodiscard]] std::string json_string(const std::string_view value) {
    std::string out = "\"";
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (byte < 0x20U || byte == 0x7FU) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(byte));
            out += buffer;
        } else out += c;
    }
    return out + "\"";
}
[[nodiscard]] std::string json_number(const double value) {
    if (!std::isfinite(value)) return "null";
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.9g", value);
    return buffer;
}
[[nodiscard]] std::string json_bool(const bool value) { return value ? "true" : "false"; }
[[nodiscard]] std::string json_names(const std::vector<std::string>& names) {
    std::string out = "[";
    for (std::size_t index = 0; index < names.size(); ++index) out += (index ? "," : "") + json_string(names[index]);
    return out + "]";
}
[[nodiscard]] std::string json_origin(const playback::Matrix& matrix) {
    return "[" + json_number(matrix[12]) + "," + json_number(matrix[13]) + "," + json_number(matrix[14]) + "]";
}

struct Loaded final {
    std::string logical;
    std::string sha256;
    std::uint64_t size{};
    std::string layer;
    std::string origin;
    std::string source_id;
    std::vector<std::byte> bytes;
    eawr::assets::Source source;
};

// 1 = I/O or missing, 4 = hash mismatch.
[[nodiscard]] int load(const eawr::vfs::Vfs& vfs, const std::string& logical, const std::string& expected, Loaded& out) {
    out = Loaded{};
    out.logical = logical;
    auto record = vfs.stat(logical);
    if (!record) { std::cerr << "shot readiness probe: " << logical << ": " << record.error().message << '\n'; return 1; }
    auto bytes = vfs.open(logical);
    if (!bytes) { std::cerr << "shot readiness probe: " << logical << ": " << bytes.error().message << '\n'; return 1; }
    out.bytes = std::move(bytes.value());
    out.layer = record.value().layer_id;
    out.origin = std::string(eawr::vfs::to_string(record.value().origin));
    out.source_id = record.value().source_id;
    out.source = eawr::assets::source_from(record.value());
    out.size = out.bytes.size();
    out.sha256 = hash(out.bytes);
    if (out.sha256 != expected) {
        std::cerr << "shot readiness probe refused: " << logical << ": sha256 " << out.sha256
                  << " does not match pinned " << expected << '\n';
        return 4;
    }
    return 0;
}

[[nodiscard]] std::string identity_json(const Loaded& input) {
    return "{\"logical_path\":" + json_string(input.logical) + ",\"sha256\":" + json_string(input.sha256)
        + ",\"size\":" + std::to_string(input.size) + ",\"layer\":" + json_string(input.layer)
        + ",\"origin\":" + json_string(input.origin) + ",\"source_id\":" + json_string(input.source_id) + "}";
}

struct Endpoint final {
    std::string id;
    std::string route;
    std::uint64_t tick{};
    std::uint32_t ticks_per_second{};
    float seconds{};
    playback::Pose pose;
};
int run_shot_readiness_cases(int argc, char** argv) {
    std::map<std::string, std::string> options;
    std::vector<std::string> attach;
    std::vector<std::string> expect_moving;
    bool attach_proxies = false;
    static const std::array<std::string_view, 13> valued{"--profile", "--game-root", "--mod-root",
        "--model", "--model-sha256", "--animation", "--animation-sha256", "--tick", "--ticks-per-second", "--seconds",
        "--mesh", "--submesh", "--out"};
    for (int index = 1; index < argc; ++index) {
        const std::string_view flag = argv[index];
        if (flag == "--attach-proxies") {
            if (attach_proxies) return usage("--attach-proxies repeated");
            attach_proxies = true;
            continue;
        }
        if (flag == "--attach" || flag == "--expect-moving") {
            if (index + 1 >= argc) return usage(std::string(flag) + " needs a value");
            auto& list = flag == "--attach" ? attach : expect_moving;
            list.emplace_back(argv[++index]);
            if (list.back().empty()) return usage(std::string(flag) + " must be non-empty");
            continue;
        }
        if (std::find(valued.begin(), valued.end(), flag) == valued.end()) return usage("unknown argument " + std::string(flag));
        if (index + 1 >= argc) return usage(std::string(flag) + " needs a value");
        if (!options.emplace(std::string(flag), argv[++index]).second) return usage(std::string(flag) + " repeated");
    }
    for (const char* required : {"--profile", "--game-root", "--model", "--model-sha256", "--out"})
        if (!options.contains(required)) return usage(std::string("missing ") + required);
    const std::string profile = options["--profile"];
    if (profile != "remake" && profile != "foc") return usage("--profile must be remake or foc");
    if (profile == "remake" && !options.contains("--mod-root")) return usage("--profile remake needs --mod-root");
    if (profile == "foc" && options.contains("--mod-root")) return usage("--profile foc takes no --mod-root");
    if (!is_sha256(options["--model-sha256"])) return usage("--model-sha256 must be 64 lowercase hex digits");
    const bool clip = options.contains("--animation");
    for (const char* flag : {"--animation-sha256", "--tick", "--ticks-per-second", "--seconds"})
        if (options.contains(flag) != clip) return usage("--animation, --animation-sha256, --tick, --ticks-per-second and --seconds go together");
    std::optional<std::uint64_t> tick;
    std::optional<std::uint32_t> ticks_per_second;
    std::optional<float> seconds;
    if (clip) {
        if (!is_sha256(options["--animation-sha256"])) return usage("--animation-sha256 must be 64 lowercase hex digits");
        tick = parse_unsigned<std::uint64_t>(options["--tick"]);
        if (!tick) return usage("--tick must be a non-negative integer");
        ticks_per_second = parse_unsigned<std::uint32_t>(options["--ticks-per-second"]);
        if (!ticks_per_second || *ticks_per_second == 0) return usage("--ticks-per-second must be a positive integer");
        seconds = parse_seconds(options["--seconds"]);
        if (!seconds) return usage("--seconds must be a finite non-negative number");
    }
    if (!clip && !expect_moving.empty()) return usage("--expect-moving needs a clip");
    for (const auto& bone : expect_moving) if (std::find(attach.begin(), attach.end(), bone) == attach.end()) attach.push_back(bone);
    if (options.contains("--mesh") != options.contains("--submesh")) return usage("--mesh and --submesh go together");
    std::optional<std::size_t> submesh;
    if (options.contains("--submesh")) {
        submesh = parse_unsigned<std::size_t>(options["--submesh"]);
        if (!submesh) return usage("--submesh must be a non-negative integer");
        if (options["--mesh"].empty()) return usage("--mesh must be non-empty");
    }
    const std::filesystem::path output_path = options["--out"];
    for (const char* root : {"--game-root", "--mod-root"})
        if (options.contains(root) && inside(output_path, options[root])) return usage("--out must not be inside an input root");
    if (std::filesystem::exists(output_path)) return usage("--out already exists; the probe never overwrites");

    std::vector<std::pair<std::string, std::string>> mounts;
    const std::filesystem::path game = options["--game-root"];
    std::vector<std::pair<std::string, std::filesystem::path>> roots;
    if (profile == "remake") {
        const std::filesystem::path mod = options["--mod-root"];
        roots.emplace_back("mod", mod.filename().string() == "Data" ? mod : mod / "Data");
    }
    roots.emplace_back("expansion", game / "corruption" / "Data");
    roots.emplace_back("base", game / "GameData" / "Data");
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, root] : roots) {
        auto resolved = eawr::vfs::resolve_manifest_mount(id, root);
        if (!resolved) { std::cerr << resolved.error().message << '\n'; return 1; }
        mounts.emplace_back(resolved.value().mount.layer_id, resolved.value().manifest_source_id);
        specs.push_back(std::move(resolved.value().mount));
    }
    auto mounted = eawr::vfs::Vfs::mount(specs);
    if (!mounted) { std::cerr << mounted.error().message << '\n'; return 1; }
    const eawr::vfs::Vfs& vfs = mounted.value();

    Loaded model_input;
    if (const int code = load(vfs, options["--model"], options["--model-sha256"], model_input)) return code;
    Loaded animation_input;
    if (clip) {
        if (const int code = load(vfs, options["--animation"], options["--animation-sha256"], animation_input)) return code;
    }
    const auto model = eawr::assets::load_model(model_input.bytes, model_input.source);
    if (!model) { std::cerr << "shot readiness probe refused: model: " << model.error().message << '\n'; return 4; }
    std::optional<eawr::assets::Animation> animation;
    if (clip) {
        auto parsed = eawr::assets::load_animation(animation_input.bytes, animation_input.source);
        if (!parsed) { std::cerr << "shot readiness probe refused: animation: " << parsed.error().message << '\n'; return 4; }
        animation = std::move(parsed.value());
    }
    const eawr::assets::Animation* clip_data = animation ? &*animation : nullptr;
    const auto census = playback::tracked_bind_census(model.value(), clip_data);
    if (!census) { std::cerr << "shot readiness probe refused: census: " << census.error().message << '\n'; return 4; }
    const auto player = playback::Player::create(model.value(), clip_data);
    if (!player) { std::cerr << "shot readiness probe refused: player: " << player.error().message << '\n'; return 4; }
    const auto& p = player.value();

    std::vector<Endpoint> endpoints;
    const auto add = [&](Endpoint endpoint, eawr::core::Result<playback::Pose> pose) {
        if (!pose) { std::cerr << "shot readiness probe refused: " << endpoint.id << ": " << pose.error().message << '\n'; return false; }
        endpoint.pose = std::move(pose.value());
        endpoints.push_back(std::move(endpoint));
        return true;
    };
    // The bind reference comes from this player's own bind output (blend 1),
    // so attachment() accepts it; its matrices are the bind pose exactly.
    if (!add({"bind", "bind", 0, 0, 0.0F, {}}, p.sample({0.0F, playback::PlaybackMode::clamp, 1.0F}))) return 4;
    if (clip) {
        if (!add({"clip_start", "tick", 0, *ticks_per_second, 0.0F, {}}, p.sample_tick(0, *ticks_per_second))) return 4;
        if (!add({"fixed", "tick", *tick, *ticks_per_second, 0.0F, {}}, p.sample_tick(*tick, *ticks_per_second))) return 4;
        if (!add({"fixed_seconds", "seconds", 0, 0, *seconds, {}}, p.sample({*seconds, playback::PlaybackMode::clamp, 0.0F}))) return 4;
    }
    const playback::Pose& bind = endpoints.front().pose;
    const playback::Pose& fixed = clip ? endpoints[2].pose : bind;

    std::optional<playback::SubmeshPalette> palette;
    std::vector<std::string> palette_digests;
    double max_identity_deviation = 0.0;
    if (submesh) {
        for (const auto& endpoint : endpoints) {
            auto sampled = playback::submesh_palette(p, endpoint.pose, model.value(), census.value(), options["--mesh"], *submesh);
            if (!sampled) { std::cerr << "shot readiness probe refused: palette: " << sampled.error().message << '\n'; return 4; }
            palette_digests.push_back(playback::palette_digest(sampled.value()));
            if (&endpoint.pose == &fixed) palette = std::move(sampled.value());
        }
        const auto identity = playback::Player::identity_matrix();
        for (const auto& slot : palette->slots) for (std::size_t element = 0; element < 16; ++element)
            max_identity_deviation = std::max(max_identity_deviation,
                std::abs(static_cast<double>(slot.skin_asset[element]) - static_cast<double>(identity[element])));
    }

    if (attach_proxies) for (const auto& proxy : model.value().proxies) {
        const std::string& name = model.value().bones[proxy.bone].name;
        if (std::find(attach.begin(), attach.end(), name) == attach.end()) attach.push_back(name);
    }
    std::vector<playback::AttachmentProbe> start_probes;
    std::vector<playback::AttachmentProbe> fixed_probes;
    for (const auto& bone : attach) {
        auto at_fixed = playback::attachment_probe(p, bind, fixed, census.value(), bone);
        if (!at_fixed) { std::cerr << "shot readiness probe refused: attachment " << bone << ": " << at_fixed.error().message << '\n'; return 4; }
        fixed_probes.push_back(std::move(at_fixed.value()));
        if (clip) {
            auto at_start = playback::attachment_probe(p, bind, endpoints[1].pose, census.value(), bone);
            if (!at_start) { std::cerr << "shot readiness probe refused: attachment " << bone << ": " << at_start.error().message << '\n'; return 4; }
            start_probes.push_back(std::move(at_start.value()));
        }
    }

    for (const auto& bone : expect_moving) {
        const auto found = std::find_if(fixed_probes.begin(), fixed_probes.end(), [&](const auto& probe) { return probe.bone == bone; });
        if (found->translation_delta <= moved_epsilon && found->basis_delta <= moved_epsilon) {
            std::cerr << "shot readiness probe refused: attachment " << bone << " does not move between bind and the fixed endpoint\n";
            return 4;
        }
    }

    // The pinned inputs must still be the same bytes at the end of the run.
    Loaded recheck;
    if (load(vfs, options["--model"], options["--model-sha256"], recheck) != 0 || recheck.source_id != model_input.source_id
        || (clip && (load(vfs, options["--animation"], options["--animation-sha256"], recheck) != 0
            || recheck.source_id != animation_input.source_id))) {
        std::cerr << "shot readiness probe refused: a pinned input changed during the run\n";
        return 4;
    }

    const auto& c = census.value();
    const auto names_where = [&](const auto predicate) {
        std::vector<std::string> names;
        for (const auto& bone : c.bones) if (predicate(bone)) names.push_back(bone.name);
        return names;
    };
    std::ostringstream rows;
    for (std::size_t index = 0; index < c.bones.size(); ++index) {
        const auto& bone = c.bones[index];
        rows << index << '\t' << bone.name << '\t' << bone.parent << '\t' << bone.tracked << bone.inherits_tracked << '\t'
             << bone.palette_listed << '\t' << bone.palette_weighted << '\t' << bone.rigid_meshes << '\t'
             << bone.hidden_bindings << '\t' << bone.proxies << '\t' << bone.lights << '\t' << bone.dazzles << '\n';
    }
    const std::string row_text = rows.str();

    std::ostringstream out;
    out << "{\n\"schema\":\"eawr.animation-shot-readiness.v1\",\n"
        << "\"claims\":{\"cpu_prediction_only\":true,\"original_parity\":false,\"association_approved\":false,\"gpu_draw\":false},\n"
        << "\"tool\":{\"compiler\":" << json_string(compiler_identity()) << ",\"moved_epsilon\":" << json_number(moved_epsilon) << "},\n"
        << "\"profile\":" << json_string(profile) << ",\n"
        << "\"mounts\":[";
    for (std::size_t index = 0; index < mounts.size(); ++index)
        out << (index ? "," : "") << "{\"layer\":" << json_string(mounts[index].first) << ",\"manifest\":" << json_string(mounts[index].second) << "}";
    out << "],\n\"inputs\":{\"model\":" << identity_json(model_input) << ",\"animation\":"
        << (clip ? identity_json(animation_input) : std::string("null")) << "},\n";
    out << "\"model\":{\"bones\":" << model.value().bones.size() << ",\"meshes\":" << model.value().meshes.size()
        << ",\"proxies\":[";
    for (std::size_t index = 0; index < model.value().proxies.size(); ++index) {
        const auto& proxy = model.value().proxies[index];
        out << (index ? "," : "") << "{\"name\":" << json_string(proxy.name) << ",\"bone\":" << proxy.bone
            << ",\"bone_name\":" << json_string(model.value().bones[proxy.bone].name) << ",\"visible\":" << json_bool(proxy.visible) << "}";
    }
    out << "]},\n\"animation\":";
    if (animation) {
        out << "{\"version\":" << static_cast<int>(animation->version) << ",\"frames_per_second\":" << json_number(animation->frames_per_second)
            << ",\"stored_frames\":" << animation->stored_frame_count << ",\"playable_frames\":" << animation->playable_frame_count
            << ",\"duration_seconds\":" << json_number(animation->duration_seconds) << ",\"tracks\":" << animation->tracks.size() << "}";
    } else out << "null";
    out << ",\n\"census\":{\"bone_count\":" << c.bone_count << ",\"track_count\":" << c.track_count
        << ",\"draw_bound\":" << c.draw_bound << ",\"draw_bound_tracked\":" << c.draw_bound_tracked
        << ",\"draw_bound_inherited\":" << c.draw_bound_inherited << ",\"draw_bound_static\":" << c.draw_bound_static
        << ",\"tracked_not_draw_bound\":" << c.tracked_not_draw_bound << ",\"palette_listed_unweighted\":" << c.palette_listed_unweighted
        << ",\n \"rows_sha256\":" << json_string(hash(std::as_bytes(std::span<const char>(row_text.data(), row_text.size()))))
        << ",\n \"tracked\":" << json_names(names_where([](const auto& b) { return b.tracked; }))
        << ",\n \"draw_bound_tracked_names\":" << json_names(names_where([](const auto& b) { return b.draw_bound() && b.tracked; }))
        << ",\n \"draw_bound_inherited_names\":" << json_names(names_where([](const auto& b) { return b.draw_bound() && !b.tracked && b.inherits_tracked; }))
        << ",\n \"draw_bound_static_names\":" << json_names(names_where([](const auto& b) { return b.draw_bound() && !b.tracked && !b.inherits_tracked; }))
        << ",\n \"tracked_not_draw_bound_names\":" << json_names(names_where([](const auto& b) { return !b.draw_bound() && b.tracked; }))
        << "},\n\"endpoints\":[";
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
        const auto& endpoint = endpoints[index];
        const auto moved = playback::moved_bones(p, bind, endpoint.pose, moved_epsilon);
        if (!moved) { std::cerr << "shot readiness probe refused: moved bones: " << moved.error().message << '\n'; return 4; }
        std::vector<std::string> moved_names;
        for (const std::size_t bone : moved.value()) moved_names.push_back(model.value().bones[bone].name);
        out << (index ? ",\n " : "\n ") << "{\"id\":" << json_string(endpoint.id) << ",\"route\":" << json_string(endpoint.route);
        if (endpoint.route == "tick") out << ",\"tick\":" << endpoint.tick << ",\"ticks_per_second\":" << endpoint.ticks_per_second;
        if (endpoint.route == "seconds") out << ",\"seconds\":" << json_number(endpoint.seconds) << ",\"mode\":\"clamp\"";
        out << ",\"sampled_time_seconds\":" << json_number(endpoint.pose.sampled_time_seconds)
            << ",\"pose_sha256\":" << json_string(playback::pose_digest(endpoint.pose))
            << ",\"moved_vs_bind\":" << moved_names.size() << ",\"moved_names\":" << json_names(moved_names);
        if (endpoint.route == "seconds") {
            double largest = 0.0;
            for (std::size_t bone = 0; bone < fixed.bones.size(); ++bone) for (std::size_t element = 0; element < 16; ++element)
                largest = std::max(largest, std::abs(static_cast<double>(endpoint.pose.bones[bone].skin_asset[element])
                    - static_cast<double>(fixed.bones[bone].skin_asset[element])));
            out << ",\"max_skin_difference_vs_fixed_tick\":" << json_number(largest);
        }
        out << "}";
    }
    out << "\n],\n\"palette\":";
    if (palette) {
        out << "{\"mesh\":" << json_string(palette->mesh) << ",\"submesh\":" << palette->submesh << ",\"route\":"
            << json_string(playback::to_string(palette->route)) << ",\"shader\":" << json_string(palette->shader)
            << ",\"vertex_count\":" << palette->vertex_count << ",\"fixed_max_identity_deviation\":" << json_number(max_identity_deviation)
            << ",\"digests\":{";
        for (std::size_t index = 0; index < endpoints.size(); ++index)
            out << (index ? "," : "") << json_string(endpoints[index].id) << ":" << json_string(palette_digests[index]);
        out << "},\"slots\":[";
        for (std::size_t index = 0; index < palette->slots.size(); ++index) {
            const auto& slot = palette->slots[index];
            out << (index ? "," : "") << "\n  {\"local\":" << slot.local << ",\"bone\":" << slot.bone << ",\"name\":" << json_string(slot.name)
                << ",\"tracked\":" << json_bool(slot.tracked) << ",\"inherits_tracked\":" << json_bool(slot.inherits_tracked)
                << ",\"active_influences\":" << slot.active_influences << "}";
        }
        out << "]}";
    } else out << "null";
    out << ",\n\"attachments\":[";
    for (std::size_t index = 0; index < fixed_probes.size(); ++index) {
        const auto& probe = fixed_probes[index];
        out << (index ? ",\n " : "\n ") << "{\"bone\":" << json_string(probe.bone) << ",\"index\":" << probe.index
            << ",\"tracked\":" << json_bool(probe.tracked) << ",\"inherits_tracked\":" << json_bool(probe.inherits_tracked)
            << ",\"bind_origin_render\":" << json_origin(probe.reference.column_major)
            << ",\"fixed_origin_render\":" << json_origin(probe.sampled.column_major)
            << ",\"fixed_translation_delta\":" << json_number(probe.translation_delta)
            << ",\"fixed_basis_delta\":" << json_number(probe.basis_delta);
        if (clip) out << ",\"clip_start_translation_delta\":" << json_number(start_probes[index].translation_delta)
                      << ",\"clip_start_basis_delta\":" << json_number(start_probes[index].basis_delta);
        out << "}";
    }
    out << "\n]\n}\n";

    const std::string bytes = out.str();
    std::ofstream output(output_path, std::ios::binary);
    output << bytes;
    output.close();
    if (!output) { std::cerr << "cannot write " << output_path.string() << '\n'; return 1; }
    std::cout << "shot_readiness model=" << model_input.logical << " draw_bound=" << c.draw_bound
              << " tracked=" << c.draw_bound_tracked << " inherited=" << c.draw_bound_inherited
              << " fixed_pose_sha256=" << playback::pose_digest(fixed) << '\n';
    return 0;
}

} // namespace shot_readiness_test_support
