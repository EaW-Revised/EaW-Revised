#include "scene_test_support.hpp"

namespace eawr::tests::scene_tests {

void conversion_contracts() {
    const auto raw = [](const float value) -> std::optional<std::int64_t> {
        auto converted = eawr::scene::fixed_from_binary32(value);
        if (!converted) return std::nullopt;
        return converted.value().raw();
    };
    expect(raw(0.0F) == 0 && raw(-0.0F) == 0, "signed zero canonicalises to raw zero");
    expect(raw(1.0F) == Fixed::scale, "one converts exactly");
    expect(raw(-2.5F) == -5 * (Fixed::scale / 2), "negative halves convert exactly");
    expect(raw(20.0F) == 20 * Fixed::scale, "a TED cell spacing converts exactly");
    expect(raw(std::ldexp(1.0F, -24)) == 1, "one quantum converts to raw one");
    expect(raw(std::ldexp(1.0F, -25)) == 0, "a half quantum ties to even zero");
    expect(raw(std::ldexp(3.0F, -25)) == 2, "one and a half quanta tie to even two");
    expect(raw(-std::ldexp(3.0F, -25)) == -2, "rounding is symmetric for negatives");
    expect(raw(std::ldexp(5.0F, -26)) == 1, "a quantum and a quarter rounds down");
    expect(raw(std::numeric_limits<float>::denorm_min()) == 0, "a subnormal underflows to zero");
    expect(raw(std::ldexp(1.0F, 38)) == (std::int64_t{1} << 62), "2^38 fits");
    const auto code = [](const float value) {
        auto converted = eawr::scene::fixed_from_binary32(value);
        return converted ? std::string{} : converted.error().code;
    };
    expect(code(std::numeric_limits<float>::quiet_NaN()) == eawr::scene::diagnostic_codes::nonfinite, "NaN is rejected");
    expect(code(std::numeric_limits<float>::infinity()) == eawr::scene::diagnostic_codes::nonfinite, "infinity is rejected");
    expect(code(-std::numeric_limits<float>::infinity()) == eawr::scene::diagnostic_codes::nonfinite, "negative infinity is rejected");
    expect(code(std::ldexp(1.0F, 39)) == eawr::scene::diagnostic_codes::overflow, "2^39 overflows the raw range");
    expect(code(std::numeric_limits<float>::max()) == eawr::scene::diagnostic_codes::overflow, "FLT_MAX overflows");

    const Fixed one = Fixed::from_raw(Fixed::scale);
    const Fixed zero = Fixed::from_raw(0);
    // R-ROT-01: even a zero yaw carries the fixed +90 degree model turn, exactly.
    auto turned = eawr::scene::placement_transform(zero, zero, zero, zero, one);
    if (turned) {
        const auto& rows = turned.value().rows;
        expect(rows[0][0].raw() == 0 && rows[1][1].raw() == 0 && rows[0][1].raw() == -Fixed::scale
                   && rows[1][0].raw() == Fixed::scale && rows[2][2] == one,
               "zero yaw is exactly the quarter turn: model +X to +Y, model -Y to +X");
    } else {
        expect(false, "a zero-yaw placement transform builds");
    }
    auto quarter = eawr::scene::placement_transform(one, zero, zero, Fixed::from_raw(90 * Fixed::scale), one);
    if (quarter) {
        const auto& rows = quarter.value().rows;
        // Yaw 90 then the model turn: a half turn about +Z.
        expect(std::llabs(rows[0][1].raw()) < 64 && std::llabs(rows[1][0].raw()) < 64, "yaw 90 leaves no quarter terms");
        expect(std::llabs(rows[0][0].raw() + Fixed::scale) < 64, "yaw 90 maps model +X to -X");
        expect(std::llabs(rows[1][1].raw() + Fixed::scale) < 64, "yaw 90 maps model -Y to +Y, the heading");
        expect(rows[0][3] == one, "translation is carried in the fourth column");
    } else {
        expect(false, "a quarter-turn yaw transform builds");
    }
    // R-ROT-04: the simulation heading is the TED yaw itself (0 along +X), so
    // a model nose on model -Y is drawn along (cos yaw, sin yaw).
    auto heading = eawr::scene::placement_transform(zero, zero, zero, Fixed::from_raw(30 * Fixed::scale), one);
    if (heading) {
        const auto& rows = heading.value().rows;
        const double nose_x = -static_cast<double>(rows[0][1].raw()) / Fixed::scale;
        const double nose_y = -static_cast<double>(rows[1][1].raw()) / Fixed::scale;
        expect(std::abs(nose_x - std::sqrt(3.0) / 2.0) < 1e-5 && std::abs(nose_y - 0.5) < 1e-5,
               "yaw 30 draws a model -Y nose along the simulation heading of 30 degrees");
    } else {
        expect(false, "a yaw-30 placement transform builds");
    }
    auto scaled = eawr::scene::placement_transform(zero, zero, zero, zero, Fixed::from_raw(3 * Fixed::scale / 2));
    expect(scaled && scaled.value().rows[2][2].raw() == 3 * Fixed::scale / 2
               && scaled.value().rows[1][0].raw() == 3 * Fixed::scale / 2,
           "scale is uniform on every axis");
    // #351: a roll turns the model about the heading (its -Y nose): zero roll is the yaw-only
    // form exactly; a -90 roll at yaw 0 keeps the nose on +X and sends the ship's left side
    // (model +X after the quarter turn, source +Y) straight down.
    const Fixed thirty = Fixed::from_raw(30 * Fixed::scale);
    auto unrolled = eawr::scene::placement_transform(one, one, one, thirty, zero, one);
    auto plain = eawr::scene::placement_transform(one, one, one, thirty, one);
    expect(unrolled && plain && unrolled.value() == plain.value(), "a zero roll is exactly the yaw-only transform");
    auto banked = eawr::scene::placement_transform(zero, zero, zero, zero, Fixed::from_raw(-90 * Fixed::scale), one);
    if (banked) {
        const auto& rows = banked.value().rows;
        expect(std::llabs(rows[0][1].raw() + Fixed::scale) < 64 && std::llabs(rows[1][1].raw()) < 64
                   && std::llabs(rows[2][1].raw()) < 64,
               "a roll keeps the model -Y nose on the heading");
        expect(std::llabs(rows[2][0].raw() + Fixed::scale) < 64 && std::llabs(rows[1][0].raw()) < 64,
               "a -90 roll sends the left side (model +X) straight down");
        expect(std::llabs(rows[1][2].raw() - Fixed::scale) < 64, "a -90 roll lays model +Z over to the left");
    } else {
        expect(false, "a rolled placement transform builds");
    }
    // #391: the full triple. A zero pitch is exactly the roll form; a +90 pitch (right-handed
    // about +Y) at yaw 0 tips the model -Y nose from +X straight down, and the quarter-turned
    // model +X (source +Y) stays level. Any triple is a rotation: orthonormal columns.
    auto unpitched = eawr::scene::placement_transform(one, one, one, thirty, zero, thirty, one);
    auto rolled = eawr::scene::placement_transform(one, one, one, thirty, thirty, one);
    expect(unpitched && rolled && unpitched.value() == rolled.value(), "a zero pitch is exactly the roll transform");
    auto pitched = eawr::scene::placement_transform(zero, zero, zero, zero, Fixed::from_raw(90 * Fixed::scale), zero, one);
    if (pitched) {
        const auto& rows = pitched.value().rows;
        expect(std::llabs(rows[2][1].raw() - Fixed::scale) < 64 && std::llabs(rows[0][1].raw()) < 64,
               "a +90 pitch sends model -Y (the nose) to source -Z");
        expect(std::llabs(rows[1][0].raw() - Fixed::scale) < 64, "a +90 pitch keeps model +X on source +Y");
        expect(std::llabs(rows[0][2].raw() - Fixed::scale) < 64, "a +90 pitch lays model +Z onto source +X");
    } else {
        expect(false, "a pitched placement transform builds");
    }
    auto tumbling = eawr::scene::placement_transform(zero, zero, zero, Fixed::from_raw(37 * Fixed::scale),
        Fixed::from_raw(-123 * Fixed::scale), Fixed::from_raw(211 * Fixed::scale), one);
    if (tumbling) {
        const auto& rows = tumbling.value().rows;
        const auto at = [&](const std::size_t row, const std::size_t column) {
            return static_cast<double>(rows[row][column].raw()) / Fixed::scale;
        };
        bool orthonormal = true;
        for (std::size_t a = 0; a < 3; ++a) {
            for (std::size_t b = 0; b < 3; ++b) {
                const double product = at(0, a) * at(0, b) + at(1, a) * at(1, b) + at(2, a) * at(2, b);
                orthonormal = orthonormal && std::abs(product - (a == b ? 1.0 : 0.0)) < 1e-5;
            }
        }
        expect(orthonormal, "a yaw, pitch and roll triple gives orthonormal columns");
    } else {
        expect(false, "a tumbling placement transform builds");
    }
    auto huge = eawr::scene::placement_transform(zero, zero, zero, zero, Fixed::from_raw(std::numeric_limits<std::int64_t>::max()));
    expect(static_cast<bool>(huge) || huge.error().code == eawr::sim::math::diagnostic_codes::overflow,
           "an extreme scale either fits or fails with overflow");
}

void scene_contracts() {
    TempTree tree;
    write_catalog(tree.root);
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "synthetic catalog mounts");
    if (!mounted) return;
    auto loaded = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::eaw);
    expect(static_cast<bool>(loaded), "synthetic catalog loads");
    if (!loaded) return;
    const eawr::data::Catalog& catalog = loaded.value().catalog;
    const auto types = eawr::assets::object_type_catalog(catalog);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<Record> records{
        {"EAWR_SCENE_PLAIN", std::array<float, 3>{40.0F, 60.0F, 0.0F}, {0.0F, 0.0F, 90.0F}, false, 1},  // 0 resolved
        {"EAWR_SCENE_PROP", std::array<float, 3>{-20.0F, 10.5F, 1.0F}, {0.0F, 0.0F, 0.0F}, false, 0},   // 1 effect ok, scale 1.5
        {"EAWR_SCENE_DERIVED", std::array<float, 3>{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 45.0F}, false, 2},  // 2 inherits PROP
        {"EAWR_NOT_IN_CATALOG", std::array<float, 3>{0.0F, 0.0F, 0.0F}, {}},                  // 3 crc_missing
        {"", std::array<float, 3>{0.0F, 0.0F, 0.0F}, {}, false, -1},                           // 4 crc_absent
        {"EAWR_SCENE_NO_MODEL", std::array<float, 3>{0.0F, 0.0F, 0.0F}, {}, false, 9},        // 5 undeclared
        {"EAWR_SCENE_LOST_MODEL", std::array<float, 3>{0.0F, 0.0F, 0.0F}, {}},                // 6 not in vfs
        {"EAWR_SCENE_ODD_SHADER", std::array<float, 3>{5.0F, 5.0F, 0.0F}, {}, false, 3},      // 7 partial
        {"EAWR_SCENE_PLAIN", std::nullopt, {}},                                                // 8 no position
        {"EAWR_SCENE_PLAIN", std::array<float, 3>{0.0F, 0.0F, 0.0F}, {10.0F, 20.0F, 30.0F}},  // 9 three-axis
        {"EAWR_SCENE_PLAIN", std::array<float, 3>{nan, 0.0F, 0.0F}, {}},                      // 10 nonfinite
        {"EAWR_SCENE_PLAIN", std::array<float, 3>{std::ldexp(1.0F, 40), 0.0F, 0.0F}, {}},     // 11 overflow
        {"EAWR_SCENE_BAD_SCALE", std::array<float, 3>{0.0F, 0.0F, 0.0F}, {}},                 // 12 bad scale
        {"EAWR_SCENE_PLAIN", std::array<float, 3>{0.0F, 0.0F, 0.0F}, {}, true},                // 13 no orientation
        {"EAWR_SCENE_PLAIN", std::array<float, 3>{0.0F, 0.0F, 0.0F}, {nan, 0.0F, 30.0F}},     // 14 NaN roll
    };
    const auto bytes = ted(records);
    eawr::assets::Source source;
    source.logical_path = "data/art/maps/eawr_scene_synthetic.ted";
    source.stored_size = bytes.size();
    auto map = eawr::assets::load_map(bytes, source, types);
    expect(static_cast<bool>(map), "synthetic TED with placements loads");
    if (!map) return;
    expect(map.value().placements.size() == records.size(), "every record is a placement");

    const Assets assets;
    eawr::scene::BuildInput input;
    input.map = &map.value();
    input.map_sha256 = "synthetic-map-sha";
    input.catalog = &catalog;
    input.access = assets.access();
    const eawr::scene::Scene scene = eawr::scene::build(input);
    const std::string serial_bytes = eawr::scene::canonical_text(scene);
    eawr::assets::Map reversed_source = map.value();
    std::reverse(reversed_source.placements.begin(), reversed_source.placements.end());
    auto reversed_input = input;
    reversed_input.map = &reversed_source;
    for (const std::size_t workers : {1U, 2U, 4U}) {
        eawr::platform::ThreadWorkerAdapter adapter(workers);
        auto built = eawr::scene::build(input, adapter);
        expect(static_cast<bool>(built), "the real worker adapter builds the scene");
        if (!built) continue;
        expect(eawr::scene::canonical_text(built.value().scene) == serial_bytes,
               "worker lanes preserve exact serial scene bytes");
        const auto& stats = built.value().execution;
        expect(stats.workers_requested == workers && stats.partitions_completed == workers
                   && stats.placement_count == records.size() && stats.observed_worker_threads == workers,
               "all requested worker threads participate");
        std::vector<std::size_t> counts(workers);
        for (std::size_t index = 0; index < records.size(); ++index) ++counts[index % workers];
        expect(stats.partition_placement_counts == counts, "cyclic partitions finalize each placement once");
        auto reversed_source_build = eawr::scene::build(reversed_input, adapter);
        expect(reversed_source_build
                   && eawr::scene::canonical_text(reversed_source_build.value().scene) == serial_bytes,
               "reversed source order preserves every worker lane");
        ReversedExecutor reversed(workers);
        auto alternate = eawr::scene::build(input, reversed);
        expect(alternate && eawr::scene::canonical_text(alternate.value().scene) == serial_bytes,
               "reversed partition schedule preserves scene bytes");
    }
    ReversedExecutor invalid(3);
    expect(!eawr::scene::build(input, invalid), "invalid worker count fails before evidence");
    ReversedExecutor failed(2, true);
    expect(!eawr::scene::build(input, failed), "executor failure emits no build result");
    ReversedExecutor duplicate(2, false, true);
    expect(!eawr::scene::build(input, duplicate), "duplicate partition execution fails");
    bool in_parallel_phase = false;
    bool asset_callback_in_phase = false;
    auto guarded = input;
    const auto access = input.access;
    guarded.access.exists = [&](std::string_view path) {
        asset_callback_in_phase |= in_parallel_phase;
        return access.exists(path);
    };
    guarded.access.model = [&](std::string_view path) {
        asset_callback_in_phase |= in_parallel_phase;
        return access.model(path);
    };
    guarded.access.sha256 = [&](std::string_view path) {
        asset_callback_in_phase |= in_parallel_phase;
        return access.sha256(path);
    };
    guarded.access.particle_system = [&](std::string_view path) {
        asset_callback_in_phase |= in_parallel_phase;
        return access.particle_system(path);
    };
    ReversedExecutor guarded_executor(4, false, false, &in_parallel_phase);
    auto guarded_result = eawr::scene::build(guarded, guarded_executor);
    expect(guarded_result && !asset_callback_in_phase,
           "no asset callback runs during placement finalization");
    eawr::assets::Map empty = map.value();
    empty.placements.clear();
    auto empty_input = input;
    empty_input.map = &empty;
    eawr::platform::ThreadWorkerAdapter four(4);
    auto empty_result = eawr::scene::build(empty_input, four);
    expect(empty_result && empty_result.value().execution.partition_placement_counts
               == std::vector<std::size_t>(4, 0), "empty scene still completes all partitions");
    eawr::assets::Map single = map.value();
    single.placements.resize(1);
    auto single_input = input;
    single_input.map = &single;
    auto single_result = eawr::scene::build(single_input, four);
    expect(single_result && single_result.value().execution.partition_placement_counts
               == std::vector<std::size_t>({1, 0, 0, 0}), "fewer placements than workers are counted");

    scene_issue_contracts(tree, catalog, records, map, input, scene, serial_bytes);

    const auto height_bytes = ted({
        {"EAWR_SCENE_BACKGROUND", std::array<float, 3>{3.0F, 4.0F, -4500.0F}, {336.0F, 336.0F, 0.0F}},
        {"EAWR_SCENE_BAD_HEIGHT", std::array<float, 3>{0.0F, 0.0F, 0.0F}, {}},
    });
    auto height_source = source;
    height_source.stored_size = height_bytes.size();
    auto height_map = eawr::assets::load_map(height_bytes, height_source, types);
    expect(static_cast<bool>(height_map), "SpaceProp height fixture loads");
    if (height_map) {
        height_map.value().kind = eawr::assets::MapKind::space;
        auto height_input = input;
        height_input.map = &height_map.value();
        const auto elevated = eawr::scene::build(height_input);
        const auto& background = elevated.placements[0];
        expect(background.layer_z_adjust_raw == -2500 * Fixed::scale && background.transform
                   && background.transform->matrix.rows[2][3].raw() == -7000 * Fixed::scale
                   && height_map.value().placements[0].position->z == -4500.0F,
               "LZ-01 adds SpaceProp height exactly once to the shared transform and preserves TED position");
        expect(!elevated.placements[1].transform
                   && has(elevated.placements[1], Cause::transform_nonfinite, "Layer_Z_Adjust"),
               "invalid authored height fails closed");
        height_map.value().kind = eawr::assets::MapKind::land;
        const auto land = eawr::scene::build(height_input);
        expect(land.placements[0].layer_z_adjust_raw == 0 && land.placements[0].transform
                   && land.placements[0].transform->matrix.rows[2][3].raw() == -4500 * Fixed::scale,
               "the space height rule does not raise a land placement");
    }
}

} // namespace eawr::tests::scene_tests
