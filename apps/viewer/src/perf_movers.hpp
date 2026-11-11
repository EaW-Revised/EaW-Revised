#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::godot_backend::perf_movers {

// --eawr-perf-movers N (report path only, no engine types). N is clamped to max_movers: every followed entity costs a
// few lookups per frame inside the frame being measured.
inline constexpr std::size_t max_movers = 64;

[[nodiscard]] constexpr std::size_t clamp_count(const long long requested) noexcept {
    return requested <= 0 ? 0U : requested > static_cast<long long>(max_movers) ? max_movers : static_cast<std::size_t>(requested);
}

// Keeps the earlier choice's entries that are still eligible (drawn this frame and present in both snapshots), then
// tops the choice up to `want` from `preferred` (fighter squadron members) and then `others`, skipping entities that
// are not eligible (docked, not launched yet, dead). Picking squadron members without that check left the trace empty on
// a restored battle whose first members were docked.
template <typename Id, typename Eligible>
[[nodiscard]] std::vector<Id> select(const std::vector<Id>& current, const std::vector<Id>& preferred,
    const std::vector<Id>& others, const std::size_t want, Eligible&& eligible) {
    std::vector<Id> kept;
    kept.reserve(want);
    for (const Id& id : current) {
        if (kept.size() < want && eligible(id)) kept.push_back(id);
    }
    for (const auto* list : {&preferred, &others}) {
        for (const Id& id : *list) {
            if (kept.size() >= want) return kept;
            if (eligible(id) && std::find(kept.begin(), kept.end(), id) == kept.end()) kept.push_back(id);
        }
    }
    return kept;
}

// When the candidate lists are worth rebuilding: below half at once, below `want` every 15th frame, so a run does not
// sit at 11 of 20 movers and the lists are not walked every frame.
[[nodiscard]] constexpr bool should_refill(const std::size_t kept, const std::size_t want, const std::uint64_t frame) noexcept {
    return kept < (want + 1U) / 2U || (kept < want && frame % 15U == 0U);
}

// --eawr-perf-screenshot-frames a,b,c (report path only): trace frame numbers (the `frame` column) whose rendered picture is
// written to <trace>.frame-<N>.png. At most max_screenshot_frames; a bad list is refused whole so a typo cannot silently
// capture nothing.
inline constexpr std::size_t max_screenshot_frames = 16;

[[nodiscard]] inline std::optional<std::vector<std::uint64_t>> parse_screenshot_frames(const std::string_view text, std::string& error) {
    std::vector<std::uint64_t> frames;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        std::size_t end = text.find(',', begin);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view item = text.substr(begin, end - begin);
        std::uint64_t value{};
        const auto parsed = std::from_chars(item.data(), item.data() + item.size(), value);
        if (item.empty() || parsed.ec != std::errc{} || parsed.ptr != item.data() + item.size()) {
            error = "--eawr-perf-screenshot-frames expects trace frame numbers a,b,c";
            return std::nullopt;
        }
        if (std::find(frames.begin(), frames.end(), value) == frames.end()) frames.push_back(value);
        if (frames.size() > max_screenshot_frames) {
            error = "--eawr-perf-screenshot-frames takes at most 16 frames";
            return std::nullopt;
        }
        begin = end + 1;
    }
    return frames;
}

// How the drawn camera is driven this frame. c_key_preview is the C-key orbit around one ship; battlecam the automatic
// cinematic shots; follow is --eawr-live-follow; free is the player's own tactical camera.
enum class CameraMode { free, follow, battlecam, c_key_preview };

[[nodiscard]] constexpr std::string_view camera_mode_name(const CameraMode mode) noexcept {
    switch (mode) {
    case CameraMode::free: return "free";
    case CameraMode::follow: return "follow";
    case CameraMode::battlecam: return "battlecam";
    case CameraMode::c_key_preview: return "c_key_preview";
    }
    return "free";
}

using Vec3d = std::array<double, 3>;
using Mat3d = std::array<Vec3d, 3>;

// The visual centre of a ship's model bounds in SOURCE coordinates: the unit's pivot position plus the model-to-world
// rotation applied to the middle of the model-space box. A camera that aims at the pivot (the position) is not aimed at
// this point; the offset is the distance between the two.
[[nodiscard]] constexpr Vec3d bounds_centre_source(const Vec3d& pivot, const Mat3d& rotation, const Vec3d& low, const Vec3d& high) noexcept {
    Vec3d result = pivot;
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t col = 0; col < 3; ++col) result[row] += rotation[row][col] * (low[col] + high[col]) * 0.5;
    return result;
}

// Source (x, y, z) is render (x, z, -y): the same mapping as the focus call and the camera columns.
[[nodiscard]] constexpr Vec3d render_from_source(const Vec3d& source) noexcept { return {source[0], source[2], -source[1]}; }

// The per-frame diagnostic columns that end every movers row (repeated on each row of a frame). Header, row writer and
// the test share this one definition so the counts cannot drift apart.
// The columns that came first (PR #6507): positions are SOURCE (sim) coordinates, the camera is RENDER space (source
// (x, y) is render (x, -y)). arrival_* are the arrival frames flown (-1: not arriving); visible_* the local player's bit in
// visible_to for the two snapshots (-1: the local player is not a snapshot player); camera_valid 0 means every camera field
// is nan. The camera fields are the camera the frame is drawn with, read after the view's process step.
inline constexpr std::string_view base_columns =
    "frame,clock_frame_us,entity,type_id,owner,squadron_member,previous_tick,latest_tick,alpha,"
    "arrival_previous,arrival_latest,visible_previous,visible_latest,"
    "presented_src_x,presented_src_y,presented_src_z,previous_src_x,previous_src_y,previous_src_z,"
    "latest_src_x,latest_src_y,latest_src_z,camera_valid,eye_render_x,eye_render_y,eye_render_z,"
    "target_render_x,target_render_y,target_render_z,up_render_x,up_render_y,up_render_z,fov_deg";
// Per mover row: the unit type's content name and the previous/latest per-sim-frame craft velocity (SOURCE; nan for a unit
// without one).
inline constexpr std::string_view row_columns =
    "type_name,craft_velocity_previous_x,craft_velocity_previous_y,craft_velocity_previous_z,"
    "craft_velocity_latest_x,craft_velocity_latest_y,craft_velocity_latest_z";
inline constexpr std::string_view frame_columns =
    "viewport_w,viewport_h,camera_mode,shot_id,shot_type,shot_elapsed_s,shot_cut,paused,tick_step,"
    "camera_subject_entity,camera_subject_type_name,camera_input_src_x,camera_input_src_y,camera_input_src_z,"
    "subject_pivot_src_x,subject_pivot_src_y,subject_pivot_src_z,subject_centre_src_x,subject_centre_src_y,subject_centre_src_z,"
    "subject_pivot_render_x,subject_pivot_render_y,subject_pivot_render_z,subject_centre_render_x,subject_centre_render_y,subject_centre_render_z";

struct FrameDiagnostics final {
    int viewport_w{}, viewport_h{};
    CameraMode mode{CameraMode::free};
    std::uint64_t shot_id{};          // counts shots/previews started this session; 0 before the first
    std::string shot_type;            // chase/circle/flyby/belly/floating/frigate, tec, hardpoint, preview, follow or empty
    double shot_elapsed_s{};          // the cinematic shot's clock; 0 outside a shot
    bool shot_cut{};                  // 1 on the first row written after a shot or preview starts (a paused battle writes none)
    bool paused{};                    // the battle clock is paused
    long long tick_step{};            // latest_tick minus the latest_tick of the previously written frame (0 on the first)
    std::uint64_t subject_entity{};   // the entity the camera follows; 0 when none
    std::string subject_type_name;
    std::optional<Vec3d> camera_input_src;  // the subject position the camera pose was built from (SOURCE)
    std::optional<Vec3d> pivot_src;         // the subject's presented position this frame (SOURCE)
    std::optional<Vec3d> centre_src;        // pivot plus the rotated model-box middle (SOURCE)

    void write(std::ostream& out) const {
        out << viewport_w << ',' << viewport_h << ',' << camera_mode_name(mode) << ',' << shot_id << ',' << shot_type << ','
            << shot_elapsed_s << ',' << (shot_cut ? 1 : 0) << ',' << (paused ? 1 : 0) << ',' << tick_step << ','
            << subject_entity << ',' << subject_type_name;
        const auto three = [&out](const std::optional<Vec3d>& value) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (value) out << ',' << (*value)[axis];
                else out << ",nan";
            }
        };
        three(camera_input_src);
        three(pivot_src);
        three(centre_src);
        three(pivot_src ? std::optional<Vec3d>{render_from_source(*pivot_src)} : std::nullopt);
        three(centre_src ? std::optional<Vec3d>{render_from_source(*centre_src)} : std::nullopt);
    }
};

} // namespace eawr::presentation::godot_backend::perf_movers
