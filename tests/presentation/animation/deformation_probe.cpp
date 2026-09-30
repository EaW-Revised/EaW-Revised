// CPU vertex-deformation probe for one explicitly named sample.  Test and
// evidence tooling only: it mounts the effective VFS read-only, checks the
// caller's pinned hashes, builds a bind-only Player and (unless --bind-only) a
// separate animated Player, and writes a receipt of the deformed indexed
// vertices of one exact mesh/submesh.  See deformation_probe.hpp for the rule.
//
//   animation_deformation_probe --game-root <dir> --mod-root <dir>
//       --model <logical.alo> --model-sha256 <hex>
//       ( --animation <logical.ala> --animation-sha256 <hex>
//         --time <seconds> --mode loop|clamp | --bind-only )
//       --mesh <exact name> --submesh <index> --out <receipt.json>
//
// No input is discovered or associated: the model/animation pairing is the
// caller's explicit request and is never promoted.  Exit codes: 0 receipt
// written; 1 mount or I/O failure; 2 usage; 4 a pinned hash does not match,
// an input changed during the run, or the evaluation refused the data.  No
// receipt is written unless the exit code is 0.

#include "deformation_probe.hpp"

#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"

#include "deformation_tool_identity.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace probe = eawr::tests::animation_deformation;
namespace playback = eawr::presentation::animation;

[[nodiscard]] std::string hash(const std::span<const std::byte> bytes) {
    return eawr::sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

[[nodiscard]] std::filesystem::path data_root(const std::filesystem::path& root) {
    return root.filename().string() == "Data" ? root : root / "Data";
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

[[nodiscard]] bool is_sha256(const std::string_view value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

[[nodiscard]] std::optional<std::size_t> parse_index(const std::string_view text) {
    std::size_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

[[nodiscard]] std::optional<float> parse_time(const std::string_view text) {
    double value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    if (!std::isfinite(value) || value < 0.0 || value > static_cast<double>(std::numeric_limits<float>::max()))
        return std::nullopt;
    return static_cast<float>(value);
}

[[nodiscard]] bool inside(const std::filesystem::path& path, const std::filesystem::path& root) {
    std::error_code error;
    const auto candidate = std::filesystem::weakly_canonical(path, error);
    if (error) return true;  // refuse what cannot be resolved
    const auto base = std::filesystem::weakly_canonical(root, error);
    if (error) return true;
    auto mismatch = std::mismatch(base.begin(), base.end(), candidate.begin(), candidate.end());
    return mismatch.first == base.end();
}

int usage(const std::string_view reason) {
    std::cerr << "deformation probe usage error: " << reason << "\n"
              << "usage: animation_deformation_probe --game-root <dir> --mod-root <dir>"
                 " --model <logical.alo> --model-sha256 <hex>"
                 " (--animation <logical.ala> --animation-sha256 <hex> --time <s> --mode loop|clamp | --bind-only)"
                 " --mesh <name> --submesh <index> --out <receipt.json>\n";
    return 2;
}

struct Loaded final {
    probe::AssetIdentity identity;
    std::vector<std::byte> bytes;
    eawr::vfs::AssetRecord record;
};

// 1 = I/O or missing, 4 = hash mismatch.
[[nodiscard]] int load(const eawr::vfs::Vfs& vfs, const std::string& path, const std::string& expected, Loaded& out) {
    auto record = vfs.stat(path);
    if (!record) { std::cerr << "deformation probe: " << path << ": " << record.error().message << '\n'; return 1; }
    auto bytes = vfs.open(path);
    if (!bytes) { std::cerr << "deformation probe: " << path << ": " << bytes.error().message << '\n'; return 1; }
    const std::string sha = hash(bytes.value());
    if (sha != expected) {
        std::cerr << "deformation probe refused: " << path << ": sha256 " << sha << " does not match pinned " << expected << '\n';
        return 4;
    }
    out.record = record.value();
    out.identity = {path, record.value().canonical_path, sha, record.value().layer_id,
        std::string(eawr::vfs::to_string(record.value().origin)), record.value().source_id,
        record.value().original_path, record.value().size};
    out.bytes = std::move(bytes.value());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> options;
    bool bind_only = false;
    static const std::array<std::string_view, 11> valued{"--game-root", "--mod-root", "--model", "--model-sha256",
        "--animation", "--animation-sha256", "--time", "--mode", "--mesh", "--submesh", "--out"};
    for (int index = 1; index < argc; ++index) {
        const std::string_view flag = argv[index];
        if (flag == "--bind-only") {
            if (bind_only) return usage("--bind-only repeated");
            bind_only = true;
            continue;
        }
        if (std::find(valued.begin(), valued.end(), flag) == valued.end()) return usage("unknown argument " + std::string(flag));
        if (index + 1 >= argc) return usage(std::string(flag) + " needs a value");
        if (!options.emplace(std::string(flag), argv[++index]).second) return usage(std::string(flag) + " repeated");
    }
    for (const char* required : {"--game-root", "--mod-root", "--model", "--model-sha256", "--mesh", "--submesh", "--out"})
        if (!options.contains(required)) return usage(std::string("missing ") + required);
    const bool any_animation = options.contains("--animation") || options.contains("--animation-sha256")
        || options.contains("--time") || options.contains("--mode");
    if (bind_only && any_animation) return usage("--bind-only excludes --animation, --animation-sha256, --time and --mode");
    if (!bind_only) {
        for (const char* required : {"--animation", "--animation-sha256", "--time", "--mode"})
            if (!options.contains(required)) return usage(std::string("missing ") + required + " (or pass --bind-only)");
    }
    if (!is_sha256(options["--model-sha256"])) return usage("--model-sha256 must be 64 lowercase hex digits");
    if (!bind_only && !is_sha256(options["--animation-sha256"])) return usage("--animation-sha256 must be 64 lowercase hex digits");
    const auto submesh = parse_index(options["--submesh"]);
    if (!submesh) return usage("--submesh must be a non-negative integer in range");
    std::optional<float> time = 0.0F;
    playback::PlaybackMode mode = playback::PlaybackMode::clamp;
    if (!bind_only) {
        time = parse_time(options["--time"]);
        if (!time) return usage("--time must be a finite non-negative number of seconds");
        if (options["--mode"] == "loop") mode = playback::PlaybackMode::loop;
        else if (options["--mode"] != "clamp") return usage("--mode must be loop or clamp");
    }
    if (options["--mesh"].empty()) return usage("--mesh must be non-empty");

    const std::filesystem::path game_root = options["--game-root"];
    const std::filesystem::path mod_root = options["--mod-root"];
    const std::filesystem::path output_path = options["--out"];
    if (inside(output_path, game_root) || inside(output_path, mod_root))
        return usage("--out must not be inside the game or mod root");
    if (std::filesystem::exists(output_path)) return usage("--out already exists; the probe never overwrites");

    const std::array<std::pair<std::string, std::filesystem::path>, 3> roots{{
        {"mod", data_root(mod_root)}, {"expansion", game_root / "corruption" / "Data"},
        {"base", game_root / "GameData" / "Data"}}};
    std::vector<eawr::vfs::MountSpec> specs;
    probe::Header header;
    for (const auto& [id, root] : roots) {
        auto resolved = eawr::vfs::resolve_manifest_mount(id, root);
        if (!resolved) {
            std::cerr << resolved.error().message << '\n';
            return 1;
        }
        header.mounts.emplace_back(resolved.value().mount.layer_id, resolved.value().manifest_source_id);
        specs.push_back(std::move(resolved.value().mount));
    }
    auto mounted = eawr::vfs::Vfs::mount(specs);
    if (!mounted) {
        std::cerr << mounted.error().message << '\n';
        return 1;
    }
    const eawr::vfs::Vfs& vfs = mounted.value();

    Loaded model_input;
    if (const int code = load(vfs, options["--model"], options["--model-sha256"], model_input)) return code;
    Loaded animation_input;
    if (!bind_only) {
        if (const int code = load(vfs, options["--animation"], options["--animation-sha256"], animation_input)) return code;
    }

    const auto model = eawr::assets::load_model(model_input.bytes, eawr::assets::source_from(model_input.record));
    if (!model) { std::cerr << "deformation probe refused: model: " << model.error().message << '\n'; return 4; }
    std::optional<eawr::assets::Animation> animation;
    if (!bind_only) {
        auto parsed = eawr::assets::load_animation(animation_input.bytes, eawr::assets::source_from(animation_input.record));
        if (!parsed) { std::cerr << "deformation probe refused: animation: " << parsed.error().message << '\n'; return 4; }
        animation = std::move(parsed.value());
    }

    // The bind reference is always its own bind-only Player.
    const auto bind_player = playback::Player::create(model.value());
    if (!bind_player) { std::cerr << "deformation probe refused: bind player: " << bind_player.error().message << '\n'; return 4; }
    const auto bind_pose = bind_player.value().sample({0.0F, playback::PlaybackMode::clamp, 0.0F});
    if (!bind_pose) { std::cerr << "deformation probe refused: bind pose: " << bind_pose.error().message << '\n'; return 4; }
    const auto animated_player = playback::Player::create(model.value(), animation ? &*animation : nullptr);
    if (!animated_player) { std::cerr << "deformation probe refused: animated player: " << animated_player.error().message << '\n'; return 4; }
    const auto animated_pose = animated_player.value().sample({*time, mode, 0.0F});
    if (!animated_pose) { std::cerr << "deformation probe refused: animated pose: " << animated_pose.error().message << '\n'; return 4; }

    const probe::Selection selection{options["--mesh"], *submesh};
    const auto evaluation = probe::evaluate(model.value(), selection, bind_pose.value(), animated_pose.value());
    if (!evaluation) { std::cerr << "deformation probe refused: " << evaluation.error << '\n'; return 4; }

    // The pinned inputs must still be the same bytes at the end of the run.
    Loaded recheck;
    if (load(vfs, options["--model"], options["--model-sha256"], recheck) != 0
        || recheck.record != model_input.record
        || (!bind_only && (load(vfs, options["--animation"], options["--animation-sha256"], recheck) != 0
            || recheck.record != animation_input.record))) {
        std::cerr << "deformation probe refused: a pinned input changed during the run\n";
        return 4;
    }

    header.compiler = compiler_identity();
#if defined(NDEBUG)
    header.config = "release";
#else
    header.config = "debug";
#endif
    for (const auto& [source, sha] : eawr_deformation_tool::source_hashes)
        header.tool_sources.emplace_back(std::string(source), std::string(sha));
    header.model = model_input.identity;
    if (!bind_only) header.animation = animation_input.identity;
    header.mesh = selection.mesh;
    header.submesh = selection.submesh;
    header.requested_time = *time;
    header.mode = bind_only ? "bind_only" : (mode == playback::PlaybackMode::loop ? "loop" : "clamp");
    header.sampled_time = animated_pose.value().sampled_time_seconds;
    header.bone_count = model.value().bones.size();
    header.track_count = animation ? animation->tracks.size() : 0U;
    if (animation) {
        for (const auto& track : animation->tracks) header.tracked_bones.push_back(track.bone_index);
        std::sort(header.tracked_bones.begin(), header.tracked_bones.end());
    }

    const std::string vertices = probe::vertex_block(*evaluation.value);
    const std::string vertices_sha = hash(std::as_bytes(std::span<const char>(vertices.data(), vertices.size())));
    std::ostringstream receipt;
    probe::write_receipt(receipt, header, *evaluation.value, vertices, vertices_sha);
    const std::string bytes = receipt.str();
    std::ofstream output(output_path, std::ios::binary);
    output << bytes;
    output.close();
    if (!output) {
        std::cerr << "cannot write " << output_path.string() << '\n';
        return 1;
    }
    const probe::Evaluation& result = *evaluation.value;
    std::cout << "deformation_probe mode=" << header.mode << " route=" << probe::to_string(result.route)
              << " indexed=" << result.vertices.size() << " predicted=" << result.predicted
              << " changed=" << result.changed << " max_displacement=" << probe::json_number(result.max_displacement)
              << " vertices_sha256=" << vertices_sha
              << " receipt_sha256=" << hash(std::as_bytes(std::span<const char>(bytes.data(), bytes.size()))) << '\n';
    return 0;
}
