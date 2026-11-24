#pragma once

#include <array>
#include <cstdint>
#include <type_traits>
#include <optional>
#include <vector>

// Pure model of the story cinematic camera a battle script drives (docs/behaviour/tactical-story-cinematic.md, TSC-01..TSC-10):
// Start_Cinematic_Camera, the camera and target keys (Set_ and Transition_), Transition_To_Tactical_Camera and End_Cinematic_Camera.
// A function of a clock in battle ticks (30 per second) and the geometry it is asked about. It owns no engine type and reads no
// clock; coordinates are source coordinates (X, Y on the map plane, Z up). Presentation only: nothing here reaches the world.
namespace eawr::presentation::camera {

inline constexpr double story_ticks_per_second = 30.0;

// What the host knows about an attached object at the time of the question; nullopt: the object does not exist (any more).
struct StoryAttachment final {
    std::array<double, 3> position{};
    // The object's drawn orientation, rows of a 3x3 matrix; absent for an object without a drawn model.
    std::optional<std::array<std::array<double, 3>, 3>> rotation;
    // The drawn centre (the object's fire bone, else its world bounds centre); used by a key that asks for it.
    std::optional<std::array<double, 3>> centre;
    double z_adjust{}; // the type's ranged-target height adjustment
};
// A non-owning reference to a callable `std::optional<StoryAttachment>(std::uint64_t object)`: the callable must outlive every call
// made through the reference (keep the lambda in a named variable). An empty reference finds no object.
class StoryAttachmentReader final {
public:
    StoryAttachmentReader() = default;
    template <class F, class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, StoryAttachmentReader>>>
    StoryAttachmentReader(const F& callable) noexcept  // NOLINT(google-explicit-constructor)
        : context_(&callable), call_([](const void* context, const std::uint64_t object) -> std::optional<StoryAttachment> {
              return (*static_cast<const F*>(context))(object);
          }) {}
    [[nodiscard]] explicit operator bool() const noexcept { return call_ != nullptr; }
    [[nodiscard]] std::optional<StoryAttachment> operator()(const std::uint64_t object) const {
        return call_ != nullptr ? call_(context_, object) : std::nullopt;
    }

private:
    const void* context_{};
    std::optional<StoryAttachment> (*call_)(const void*, std::uint64_t){};
};

// The values of one camera or target key call (the script's numbers, after the host's validation).
struct StoryKeyInput final {
    std::array<double, 3> position{};
    double distance{}, pitch{}, yaw{};
    bool euler{};
    std::optional<std::uint64_t> object;
    bool use_object_rotation{};
    bool centre{};
};

struct StoryPose final {
    std::array<float, 3> eye{};
    std::array<float, 3> target{};
};

class StoryCinematic final {
public:
    [[nodiscard]] bool active() const noexcept { return active_; }

    // Start_Cinematic_Camera: the camera and the target hold where the player's camera is. Starting a running cinematic starts it
    // over from the given poses.
    void start(double now, const std::array<double, 3>& eye, const std::array<double, 3>& target);
    // End_Cinematic_Camera: the keys are dropped and the player's camera is back.
    void end() noexcept;

    // The keys; `now` is the battle tick of the call. Without a running cinematic every one is ignored (as in the original).
    void set_camera_key(double now, const StoryKeyInput& input);
    void transition_camera_key(double now, const StoryKeyInput& input, double seconds);
    void set_target_key(double now, const StoryKeyInput& input);
    void transition_target_key(double now, const StoryKeyInput& input, double seconds);
    // Transition_To_Tactical_Camera: from the pose the cinematic shows now to the player's camera pose over `seconds` (at least one
    // tick). The cinematic's own clock restarts at 0.
    void transition_to_tactical(double now, double seconds, const std::array<double, 3>& eye, const std::array<double, 3>& target);

    // The pose at battle tick `now` (a tick may be fractional). Moves the clock; the clock never runs backwards. The pose of the
    // frame is cached: the next transition starts from it.
    [[nodiscard]] std::optional<StoryPose> pose(double now, const StoryAttachmentReader& reader);

    // The ease of a segment (the fraction of the way, for a time fraction t, with the ease-out of the segment's first key and the
    // ease-in of its second): TSC-07. Zero eases give t itself.
    [[nodiscard]] static double ease(double t, double ease_out, double ease_in) noexcept;

private:
    struct Key final {
        double frame{};
        std::array<double, 3> position{};
        std::array<double, 3> offset{};
        std::optional<std::uint64_t> object;
        bool ignore_attach_frame{true};
        bool centre{};
        bool cut{};
        double ease_in{}, ease_out{};
    };
    struct Track final {
        std::vector<Key> keys;
        std::array<double, 3> last{};
        // The last attachment matrix and height seen, used while the attached object is missing.
        std::optional<std::array<std::array<double, 4>, 3>> cached;
        double cached_z{};
    };

    void add_key(Track& track, const Key& key);
    [[nodiscard]] std::array<double, 3> key_position(Track& track, const Key& key, const StoryAttachmentReader& reader) const;
    [[nodiscard]] std::array<double, 3> evaluate(Track& track, const StoryAttachmentReader& reader) const;
    static std::array<double, 3> offset_of(const StoryKeyInput& input) noexcept;

    bool active_{};
    double base_{};   // the battle tick the cinematic's own clock was zero at
    double frame_{};  // the clock at the last pose or key call (battle ticks since base_)
    Track camera_, target_;
};

} // namespace eawr::presentation::camera
