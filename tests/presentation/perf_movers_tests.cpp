// --eawr-perf-movers selection (apps/viewer/src/perf_movers.hpp): engine-free contracts.
#include "perf_movers.hpp"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace pm = eawr::presentation::godot_backend::perf_movers;
using eawr::presentation::godot_backend::perf_movers::clamp_count;
using eawr::presentation::godot_backend::perf_movers::max_movers;
using eawr::presentation::godot_backend::perf_movers::select;
using eawr::presentation::godot_backend::perf_movers::should_refill;
using Ids = std::vector<std::uint64_t>;

int failures = 0;

std::size_t fields(const std::string_view csv) { return static_cast<std::size_t>(std::count(csv.begin(), csv.end(), ',')) + 1U; }
void check(const bool ok, const char* what) {
    if (ok) return;
    ++failures;
    std::cerr << "FAIL: " << what << '\n';
}

} // namespace

int main() {
    // The restored heavy save: the first squadron members are docked (not drawn), later units fly.
    const std::set<std::uint64_t> drawn_set{30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41};
    const auto drawn = [&](std::uint64_t id) { return drawn_set.contains(id); };
    const Ids docked{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    Ids preferred = docked;
    preferred.insert(preferred.end(), {30, 31, 32, 33});
    const Ids others{34, 35, 36, 37, 38, 39, 40, 41, 99};

    const Ids first = select<std::uint64_t>({}, preferred, others, 6U, drawn);
    check(first == Ids({30, 31, 32, 33, 34, 35}), "docked members are skipped, preferred drawn first, then the others");
    check(select<std::uint64_t>({}, docked, {}, 6U, drawn).empty(), "nothing drawn gives an empty choice, not stale ids");

    // Kept while still drawn: survivors first; a full choice stays as it is.
    const Ids keep = select<std::uint64_t>(first, preferred, others, 6U, drawn);
    check(keep == first, "an unchanged choice is kept as it is");

    // Entities that stop being drawn are dropped; the choice is topped up to `want` in order without duplicates.
    const std::set<std::uint64_t> later{33, 34, 40, 41, 50};
    const auto drawn_later = [&](std::uint64_t id) { return later.contains(id); };
    const Ids refill = select<std::uint64_t>(first, {50, 33}, {40, 41, 34}, 6U, drawn_later);
    check(refill == Ids({33, 34, 50, 40, 41}), "tops up from the lists, survivors first, no duplicates");
    check(select<std::uint64_t>(first, {50}, {}, 0U, drawn_later).empty(), "want 0 follows nothing");
    // Half or more drawn but fewer than want: the top-up still fills the choice (no sitting at 10-19 of 20).
    const Ids topped = select<std::uint64_t>({33, 34, 40}, {50, 41}, {}, 5U, drawn_later);
    check(topped == Ids({33, 34, 40, 50, 41}), "a partial choice is topped up to want");
    check(select<std::uint64_t>({33, 34, 40}, {}, {}, 6U, drawn_later).size() == 3U, "without lists only the survivors remain");

    // The call-site policy and the bound.
    check(should_refill(0U, 20U, 1U), "an empty choice refills at once");
    check(should_refill(9U, 20U, 1U), "below half refills at once");
    check(!should_refill(15U, 20U, 1U), "below want but above half waits for the 15th frame");
    check(should_refill(15U, 20U, 30U), "below want refills on the 15th frame");
    check(!should_refill(20U, 20U, 30U), "a full choice never refills");
    check(clamp_count(-5) == 0U && clamp_count(0) == 0U && clamp_count(20) == 20U, "N is passed through in range");
    check(clamp_count(5000) == max_movers && clamp_count(65) == max_movers, "N is clamped to max_movers");

    // The movers file's columns: the first 33 are unchanged and the new ones are appended; header and row writer share
    // one definition, so a row has exactly as many fields as the header.
    check(fields(pm::base_columns) == 33U, "the first columns are still the 33 of the first movers file");
    check(pm::base_columns.ends_with("fov_deg"), "the new columns are appended after fov_deg");
    check(pm::row_columns.starts_with("type_name,") && fields(pm::row_columns) == 7U, "per-row columns: type name and two velocities");
    pm::FrameDiagnostics empty;
    std::ostringstream blank;
    empty.write(blank);
    check(fields(blank.str()) == fields(pm::frame_columns), "an empty frame writes as many fields as the header names");
    check(blank.str().find("free") != std::string::npos && blank.str().find("nan") != std::string::npos,
        "no camera subject writes mode free and nan positions");

    pm::FrameDiagnostics shot;
    shot.viewport_w = 2560;
    shot.viewport_h = 1421;
    shot.mode = pm::CameraMode::c_key_preview;
    shot.shot_id = 3U;
    shot.shot_type = "preview";
    shot.shot_elapsed_s = 1.5;
    shot.shot_cut = true;
    shot.paused = true;
    shot.tick_step = 2;
    shot.subject_entity = 323U;
    shot.subject_type_name = "Corellian_Corvette";
    shot.camera_input_src = pm::Vec3d{1.0, 2.0, 3.0};
    shot.pivot_src = pm::Vec3d{10.0, 20.0, 30.0};
    shot.centre_src = pm::Vec3d{11.0, 20.0, 28.0};
    std::ostringstream row;
    row << std::fixed << std::setprecision(6); // as the trace writes
    shot.write(row);
    check(fields(row.str()) == fields(pm::frame_columns), "a full frame writes as many fields as the header names");
    check(row.str() == "2560,1421,c_key_preview,3,preview,1.500000,1,1,2,323,Corellian_Corvette,"
                       "1.000000,2.000000,3.000000,10.000000,20.000000,30.000000,11.000000,20.000000,28.000000,"
                       "10.000000,30.000000,-20.000000,11.000000,28.000000,-20.000000",
        "the frame columns are written in header order, the pivot and box centre also in RENDER space (x, z, -y)");
    check(pm::camera_mode_name(pm::CameraMode::battlecam) == "battlecam" && pm::camera_mode_name(pm::CameraMode::follow) == "follow"
            && pm::camera_mode_name(pm::CameraMode::free) == "free",
        "camera modes have stable names");

    // The visual centre is the middle of the model box, rotated, from the pivot.
    const pm::Mat3d identity{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    const auto flat = pm::bounds_centre_source({10, 20, 30}, identity, {-2, -1, -4}, {4, 1, 0});
    check(flat == pm::Vec3d({11, 20, 28}), "the box centre is the pivot plus the middle of the box");
    const pm::Mat3d quarter_turn{{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}};
    const auto turned = pm::bounds_centre_source({10, 20, 30}, quarter_turn, {-2, -1, -4}, {4, 1, 0});
    check(turned == pm::Vec3d({10, 21, 28}), "the box middle is rotated with the unit");
    check(pm::render_from_source({1, 2, 3}) == pm::Vec3d({1, 3, -2}), "source (x, y, z) is render (x, z, -y)");

    // --eawr-perf-screenshot-frames: a list of trace frames, refused whole when bad.
    std::string error;
    const auto frames = pm::parse_screenshot_frames("40,7,40,900", error);
    check(frames && *frames == Ids({40, 7, 900}), "screenshot frames keep the order and drop duplicates");
    check(!pm::parse_screenshot_frames("", error) && !error.empty(), "an empty list is refused");
    check(!pm::parse_screenshot_frames("5,,6", error), "an empty item is refused");
    check(!pm::parse_screenshot_frames("5,x", error), "a non-number is refused");
    check(!pm::parse_screenshot_frames("5,-1", error), "a negative frame is refused");
    std::string many;
    for (int i = 0; i < 17; ++i) many += (i == 0 ? "" : ",") + std::to_string(i + 1);
    check(!pm::parse_screenshot_frames(many, error), "more than max_screenshot_frames frames are refused");
    check(pm::parse_screenshot_frames(many.substr(0, many.rfind(',')), error).has_value(), "exactly max_screenshot_frames frames are accepted");

    if (failures == 0) std::cout << "perf_movers_tests: ok\n";
    return failures == 0 ? 0 : 1;
}
