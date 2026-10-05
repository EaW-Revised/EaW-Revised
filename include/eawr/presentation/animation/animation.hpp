#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::animation {

// Matrices are column-major and multiply column vectors.  Asset matrices are
// kept in the ALO right-handed, Z-up basis; conversion is deliberately a
// presentation boundary operation, never a parser or simulation operation.
using Matrix = std::array<float, 16>;

enum class PlaybackMode : std::uint8_t { clamp, loop };

struct SampleRequest final {
    float time_seconds{};
    PlaybackMode mode{PlaybackMode::loop};
    // 0 retains the sampled pose; 1 produces bind pose.  This is useful for
    // presentation-only clip fade-out and is not a simulation event input.
    // Values strictly between 0 and 1 require a proper TRS bind on each
    // tracked bone (no mirror, shear or collapsed axis) and a positive
    // animated scale; otherwise sample() fails with unsupported_bind_blend.
    float blend_to_bind{};
};

struct BonePose final {
    Matrix local_asset{};
    Matrix model_asset{};
    Matrix skin_asset{};
    bool visible{true};
};

class Player;

struct Pose final {
    // One entry per Model::bones item. The pose owns every matrix, and
    // attachment() returns a separate value with no Pose or Player lifetime.
    std::vector<BonePose> bones;
    float sampled_time_seconds{};

private:
    friend class Player;
    // Opaque identity of the Player::create call whose player sampled this
    // pose; copies of the pose keep it, and a default or hand-built pose has
    // none. It records provenance only: the public matrices stay writable, so
    // it does not make a pose tamper-proof. It does not keep the player alive.
    std::weak_ptr<const void> origin_;
};

enum class AttachmentSpace : std::uint8_t { model, world };

struct AttachmentTransform final {
    AttachmentSpace space{AttachmentSpace::model};
    Matrix column_major{};
};

namespace diagnostic_codes {
inline constexpr std::string_view invalid_model = "EAWR-ANIMATION-0001";
inline constexpr std::string_view invalid_animation = "EAWR-ANIMATION-0002";
inline constexpr std::string_view missing_attachment = "EAWR-ANIMATION-0003";
inline constexpr std::string_view invalid_request = "EAWR-ANIMATION-0004";
// sample() with 0 < blend_to_bind < 1 supports only a proper TRS bind on every
// tracked bone and a positive animated scale; anything else fails with this.
inline constexpr std::string_view unsupported_bind_blend = "EAWR-ANIMATION-0005";
} // namespace diagnostic_codes

// Validates that ALA track indices and names bind to exactly this ALO model.
// It retains no asset references, so callers may destroy the parsed inputs
// after constructing the player.
class Player final {
public:
    ~Player();
    Player(Player&&) noexcept;
    Player& operator=(Player&&) noexcept;
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    [[nodiscard]] static core::Result<Player> create(
        const assets::Model& model, const assets::Animation* animation = nullptr);

    [[nodiscard]] core::Result<Pose> sample(const SampleRequest& request) const;

    // Looped sampling at tick `tick` of an integer clock running at
    // `ticks_per_second`. The frame position tick * fps / ticks_per_second is
    // reduced modulo the playable frames in integer arithmetic, so no
    // binary32 time is wrapped by fmod or truncated at a frame edge (which
    // sample() can do for a duration that is not exact in binary32). It
    // requires an integral frame rate and fails otherwise, or if the reduced
    // position does not fit 64 bits; a clipless player yields the bind pose.
    // It is a separate entry point: sample() is unchanged for its consumers.
    [[nodiscard]] core::Result<Pose> sample_tick(std::uint64_t tick, std::uint32_t ticks_per_second) const;

    // The pose at frame position `position / subdivisions`, already reduced by
    // the caller (idle_playback.hpp): 0 <= position <= playable_frames() *
    // subdivisions, where the end is the last stored frame. It fails for a
    // zero subdivision count or a position past the end; a clipless player
    // yields the bind pose. sample_tick() is unchanged for its consumers.
    [[nodiscard]] core::Result<Pose> sample_position(std::uint64_t position, std::uint32_t subdivisions) const;
    // Reuses caller-owned bone storage. Once sized to bone_count(), valid
    // sampling performs no allocation; errors leave the previous pose intact.
    [[nodiscard]] core::Result<void> sample_position(std::uint64_t position, std::uint32_t subdivisions,
        Pose& output) const;
    [[nodiscard]] std::uint32_t playable_frames() const noexcept;
    [[nodiscard]] float frames_per_second() const noexcept;

    // Model attachment is the animated bone transform after the one documented
    // asset-to-render basis conversion.  World attachment prepends the caller's
    // already-render-basis world transform; it does not perform a second basis
    // conversion.  The returned value owns its matrix and has no Player lifetime.
    // The pose must come from this player's sample() or sample_tick(), or be a
    // copy of such a pose; a pose from any other player, including one created
    // independently from the same model, or a default/hand-built pose is an
    // invalid request even when its bone count matches.  Moving a player moves
    // its origin, so the destination accepts poses the source sampled.
    [[nodiscard]] core::Result<AttachmentTransform> attachment(
        const Pose& pose, std::string_view bone_name, AttachmentSpace space,
        const Matrix& render_world = identity_matrix()) const;

    // True when `pose` (or a copy of it) was sampled by this player: the same
    // origin rule attachment() applies. A default/hand-built pose is not.
    [[nodiscard]] bool sampled(const Pose& pose) const noexcept;
    [[nodiscard]] std::size_t bone_count() const noexcept;

    [[nodiscard]] static Matrix identity_matrix() noexcept;
    [[nodiscard]] static Matrix asset_to_render_matrix() noexcept;
    [[nodiscard]] static Matrix asset_to_render_transform(const Matrix& asset) noexcept;
    [[nodiscard]] static Matrix multiply(const Matrix& left, const Matrix& right) noexcept;

    // An ALO rigid mesh (empty skin map, connected to a bone) stores its
    // vertices in that bone's space, while a palette-skinned mesh stores them
    // in bind model space. The skin palette maps bind model space
    // (`animated_model * inverse(bind_model)`), so a draw that feeds a rigid
    // mesh through the palette first moves each vertex into bind model space
    // with its bone's bind model transform: the position by the full affine
    // transform, tangent and binormal by its linear part, and the normal by
    // its inverse transpose (orientation kept for a mirrored basis). Directions
    // are not renormalised. Asset basis in and out.
    [[nodiscard]] static assets::Vertex rigid_vertex_to_bind(
        const Matrix& bind_model, const assets::Vertex& vertex) noexcept;

private:
    Player() = default;
    struct Bone;
    struct Track;
    [[nodiscard]] core::Result<Pose> interpolate(std::size_t first, std::size_t second, float fraction, float sampled,
        float blend_to_bind) const;
    [[nodiscard]] core::Result<void> interpolate(std::size_t first, std::size_t second, float fraction, float sampled,
        float blend_to_bind, Pose& output) const;

    // Allocated once per create(); moved, never shared between players.
    std::shared_ptr<const void> origin_;
    std::vector<Bone> bones_;
    std::vector<Track> tracks_;
    float frames_per_second_{};
    float duration_seconds_{};
    std::uint32_t playable_frames_{};
};

} // namespace eawr::presentation::animation
