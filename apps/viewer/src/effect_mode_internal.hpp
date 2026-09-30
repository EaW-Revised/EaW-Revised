#pragma once

// Private to the effect mode translation units (effect_mode*.cpp): the mode's
// State, its frame and capture records, and the helpers the units share.
#include "effect_mode.hpp"

#include "capture_viewport.hpp"
#include "particle_adapter.hpp"
#include "viewer_path.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/particles/attachment_lifecycle.hpp"
#include "eawr/presentation/particles/particles.hpp"
#include "eawr/presentation/particles/proxy_binding.hpp"
#include "eawr/presentation/particles/render.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace effect_mode_detail {

[[nodiscard]] std::string hash_bytes(std::span<const std::byte> bytes);
[[nodiscard]] std::string canonical(std::string_view name);
[[nodiscard]] std::string_view file_name(std::string_view authored);
[[nodiscard]] bool write_png(const std::filesystem::path& path, const CaptureResult& capture);

struct ResolvedTexture final {
    std::string logical_path;
    std::string sha256;
    std::optional<assets::Texture> texture;
};

struct FrameRecord final {
    std::uint32_t frame{};
    float time{};
    particles::EffectFrameStats stats;
    // False once the instance is gone: an immediate release or a completed
    // drain skips every later advance, so the frame carries no stream.
    bool advanced{true};
    bool released{};
    std::size_t resources{};
};

// Decoded-pixel corroboration of one capture against one frame's streams.
struct CaptureEvidence final {
    std::string label;
    std::uint32_t frame{};
    std::string check{"projected_emitter_bounds"};
    std::string sha256;
    // The read-back PNG's size; zero until a capture was read.
    std::uint32_t width{};
    std::uint32_t height{};
    std::array<float, 4> projected{}; // min x, min y, max x, max y in pixels
    float inside_coverage{};
    float outside_coverage{};
    std::uint64_t inside_drawn{};
    std::uint64_t drawn_samples{};
    bool verified{};
    bool written{};
    std::string failure;
};

// Where one registry instance stands in the detach schedule. The dry run, the
// replay validation and the graphical run each own one and walk it identically.
struct Lifecycle final {
    bool live{true};
    std::optional<particles::EffectDetachState> result;
    std::optional<std::uint32_t> release_index;
    std::string released_by;
};

// One sample of the host-visibility lifecycle (--eawr-effect-visibility). The
// replay validation and the graphical run must produce equal sequences.
struct VisibilityFrame final {
    bool visible{};
    bool spawned{};
    std::string detached;
    std::size_t drains_released{};
    std::size_t drains_cut_short{};
    std::size_t live_instances{};
    bool active{};
    [[nodiscard]] bool operator==(const VisibilityFrame&) const = default;
};

} // namespace effect_mode_detail

using namespace effect_mode_detail;

struct EffectMode::State final {
    explicit State(Options value) : options(std::move(value)) {}

    Options options;
    std::optional<vfs::Vfs> filesystem;
    std::unique_ptr<GodotRenderer> renderer;
    std::unique_ptr<GodotParticleBackend> backend;
    std::unique_ptr<particles::EffectRegistry> registry;
    std::map<std::string, ResolvedTexture> textures;

    std::string profile{"synthetic"};
    std::vector<std::string> layers;
    std::string effect_hash;
    particles::SystemDefinition system;
    std::vector<particles::EmitterRenderPlan> plans;
    particles::EffectHandle handle{};

    // Attachment host.
    std::string attach_model;
    std::string attach_bone;
    std::string attach_model_hash;
    std::string animation_hash;
    std::optional<animation::Player> player;
    std::optional<particles::ProxyMeshBinding> proxy_mesh;
    std::vector<particles::Vec3> attachment_origins;
    std::vector<particles::Vec3> proxy_origins;
    std::vector<particles::Vec3> mesh_origins;

    FixedCamera camera;
    particles::CameraFrame camera_frame;
    std::vector<std::uint64_t> probe_hashes;
    std::vector<FrameRecord> records;
    std::uint32_t next_index{};
    std::uint32_t hold{};
    std::string pending_capture;
    std::chrono::steady_clock::time_point first_frame{};
    std::chrono::steady_clock::time_point last_frame{};

    // Optional detach schedule (--eawr-effect-detach-frame): the zero-based
    // advance index before which EffectRegistry::detach is called.
    std::optional<std::uint32_t> detach_frame;
    Lifecycle lifecycle;
    std::optional<particles::EffectDetachState> replay_result;
    // Intermediate captures planned from the replay: label and advance index.
    std::vector<std::pair<std::string, std::uint32_t>> capture_plan;
    std::vector<CaptureEvidence> detach_captures;

    // Optional host-visibility lifecycle (--eawr-effect-visibility): the host
    // bone's sampled visibility detaches the effect once when it hides and,
    // under `respawn`, starts a new generation when it reappears.
    std::optional<particles::ReappearancePolicy> visibility;
    std::optional<std::size_t> visibility_bone;
    std::string visibility_bone_name;
    std::optional<particles::AttachmentLifecycle> visibility_life;
    std::vector<VisibilityFrame> visibility_frames;
    std::vector<VisibilityFrame> replay_visibility;
    std::size_t visibility_released_at_end{};
    std::size_t peak_live_rids{};

    CaptureEvidence evidence;
    std::size_t live_rids_before_release{};
    std::size_t live_rids_after_release{};
    std::size_t registry_resources_after_release{};
    std::string released_by;
    bool completed{};
    std::string status{"failed"};
    std::string failure;

    [[nodiscard]] const assets::Texture* resolve(std::string_view name);
    struct HostFrames final {
        particles::EmitterFrame emitter;
        particles::MeshFrame mesh;
        // The visibility bone's sampled state; true without a visibility bone.
        bool visible{true};
    };
    [[nodiscard]] core::Result<HostFrames> host_frames(float time) const;
    void record_origins(const HostFrames& frames);
    [[nodiscard]] core::Result<void> apply_host_frames(
        particles::EffectRegistry& target, particles::EffectHandle effect, float time,
        bool record = false);
    [[nodiscard]] core::Result<particles::EffectHandle> spawn(
        particles::EffectRegistry& target) const;
    // One scheduled frame: host frames, the detach when it is due, then the
    // advance while the instance lives, releasing it once a drain finishes.
    [[nodiscard]] core::Result<FrameRecord> step(
        particles::EffectRegistry& target, particles::EffectHandle effect, Lifecycle& life,
        std::uint32_t index, const particles::CameraFrame& view, bool record);
    [[nodiscard]] particles::AttachmentLifecycle make_visibility_life(particles::EffectRegistry& target) const;
    // One visibility-driven frame: host frames and visibility, then the
    // lifecycle's one detach or spawn, advances and drain releases.
    [[nodiscard]] core::Result<FrameRecord> visibility_step(
        particles::EffectRegistry& target, particles::AttachmentLifecycle& life, std::uint32_t index,
        const particles::CameraFrame& view, bool record, VisibilityFrame& event);
    void plan_visibility_captures(const std::vector<bool>& drawn);
    [[nodiscard]] bool empty_capture_allowed(std::string_view label) const;
    [[nodiscard]] bool fit_camera();
    [[nodiscard]] bool verify_capture(const CaptureResult& capture, const FrameRecord& frame,
                                      CaptureEvidence& result, bool allow_empty) const;
    [[nodiscard]] std::filesystem::path capture_path_for(std::string_view label) const;
    void capture_intermediate();
    [[nodiscard]] int finish();
    [[nodiscard]] bool write_report() const;
};

} // namespace eawr::presentation::godot_backend
