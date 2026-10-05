#include "eawr/data/ui/cursors.hpp"
#include "eawr/presentation/ui/cursors.hpp"
#include "ui_test_support.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <string_view>
#include <tuple>

namespace {
std::atomic<std::size_t> allocations{};
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(const std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* const memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* const memory) noexcept { std::free(memory); }
void operator delete(void* const memory, std::size_t) noexcept { std::free(memory); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {
using namespace eawr;
namespace ui = presentation::ui;
using test::ui::expect;
void states() {
    ui::CursorInput state;
    const auto check = [&](const std::string_view id) { expect(ui::battle_cursor(state) == id, id.data()); };
    check("POINTER_NORMAL");
    state.hostile = true;
    check("POINTER_NORMAL"); // a hostile contact does not give a select action
    state.hostile = false;
    state.selectable = true;
    check("POINTER_SELECT");
    state.hostile = true;
    state.own_selection = true;
    check("POINTER_ATTACK"); // ship, hardpoint and squadron icon share the hover facts
    state.hostile = state.selectable = false;
    state.movable_selection = true;
    check("POINTER_MOVE");
    state.ctrl = true;
    check("POINTER_ATTACK_MOVE");
    state.alt = true;
    check("POINTER_GUARD");
    state.hostile = true;
    check("POINTER_ATTACK"); // guard modifiers do not turn enemies into escort targets
    state.ctrl = state.alt = state.hostile = false;
    state.mode = ui::OrderMode::attack;
    check("POINTER_ATTACK_ONLY_MODE_NO_UNIT_TARGETED");
    state.hostile = true;
    check("POINTER_ATTACK_ONLY_MODE_UNIT_TARGETED");
    state.passable = false;
    check("POINTER_ATTACK_ONLY_MODE_NO_UNIT_TARGETED");
    state.passable = true;
    state.mode = ui::OrderMode::move;
    check("POINTER_MOVE_ONLY_MODE_PASSABLE_TARGETED");
    state.passable = false;
    check("POINTER_MOVE_ONLY_MODE_NO_PASSABLE_TARGETED");
    state.mode = ui::OrderMode::none;
    state.hostile = false;
    check("POINTER_CANT_MOVE");
    state.own_selection = state.movable_selection = false;
    check("POINTER_CANT_MOVE"); // outside the map is invalid even with nothing selected
    state.own_selection = state.movable_selection = true;
    state.passable = true;
    state.mode = ui::OrderMode::attack_move;
    check("POINTER_ATTACK_MOVE");
    state.mode = ui::OrderMode::guard;
    check("POINTER_GUARD");
    state.mode = ui::OrderMode::none;
    state.alt = true;
    check("POINTER_WAYPOINT_PLACEMENT");
    state = {};
    state.own_selection = true; // a station cannot imply movement
    check("POINTER_NORMAL");
    state = {}; // enemy-only selection has no controllable units
    state.selectable = true;
    check("POINTER_SELECT");
    state.selectable = false;
    check("POINTER_NORMAL");
    state.placing = true;
    check("POINTER_CANT_MOVE");
    state.placement_valid = true;
    check("POINTER_REINFORCEMENTS_LANDING_POINT");
    state.ability = ui::CursorTarget::enemy;
    check("POINTER_TARGET_SPECIAL_ABILITY_TO_ENEMY_OBJECT_INVALID");
    state.ability_valid = true;
    check("POINTER_TARGET_SPECIAL_ABILITY_TO_ENEMY_OBJECT");
    for (const auto& [target, valid, invalid] : std::array{
        std::tuple{ui::CursorTarget::friendly, "POINTER_TARGET_SPECIAL_ABILITY_TO_FRIENDLY_OBJECT", "POINTER_TARGET_SPECIAL_ABILITY_TO_FRIENDLY_OBJECT_INVALID"},
        std::tuple{ui::CursorTarget::terrain, "POINTER_TARGET_SPECIAL_ABILITY_TO_PASSABLE_TERRAIN", "POINTER_TARGET_SPECIAL_ABILITY_TO_PASSABLE_TERRAIN_INVALID"},
        std::tuple{ui::CursorTarget::space_position, "POINTER_TARGET_SPECIAL_ABILITY_TO_SPACE_POSITION", "POINTER_TARGET_SPECIAL_ABILITY_TO_SPACE_POSITION_INVALID"}}) {
        state.ability = target;
        state.ability_valid = true;
        check(valid);
        state.ability_valid = false;
        check(invalid);
    }
    state.hud = true;
    check("POINTER_NORMAL");
    state.camera_rotate = true;
    check("POINTER_MAP_ROTATING_MODE");
    state.camera_pan = true;
    check("POINTER_MAP_SCROLLING_MODE");
    state.dragging = true;
    check("POINTER_DRAG_SELECT_MODE");
    state = {};
    state.repair = true;
    check("POINTER_REPAIR_HARDPOINT");
}
void animation() {
    expect(ui::cursor_frame(0.175, 10, 4) == 0, "delay includes the zero countdown service");
    expect(ui::cursor_frame(0.177, 10, 4) == 1, "animation advances after eleven services");
    expect(ui::cursor_frame(0.705, 10, 4) == 0, "animation wraps");
    expect(ui::cursor_frame(1000.0, 0, 1) == 0, "single frame remains static");
    expect(ui::cursor_frame(std::numeric_limits<double>::infinity(), 5, 4) == 0, "nonfinite clock is safe");
    expect(ui::cursor_frame(-1.0, 5, 4) == 0, "negative clock is safe");
}
void steady_history() {
    ui::CursorInput state;
    state.ability = ui::CursorTarget::enemy;
    const std::string_view long_id = ui::battle_cursor(state);
    ui::CursorHistory history;
    const auto initial = allocations.load(std::memory_order_relaxed);
    history.record(0, long_id, "unit"); // Warm-up retains the long ID and the first sample.
    expect(allocations.load(std::memory_order_relaxed) > initial, "allocation counter sees warm-up allocations");
    const auto steady = [&] {
        const auto before = allocations.load(std::memory_order_relaxed);
        for (std::uint64_t frame = 1; frame <= 10000; ++frame) {
            const std::string_view id = ui::battle_cursor(state);
            history.record(frame, id, "unit");
        }
        expect(allocations.load(std::memory_order_relaxed) == before,
            "unchanged long cursor resolution and history allocate nothing after warm-up");
    };
    steady();
    expect(history.samples().size() == 1, "steady frames record no additional samples");
    for (std::uint64_t frame = 1; frame <= 300; ++frame) {
        state.ability_valid = !state.ability_valid;
        history.record(frame, ui::battle_cursor(state), "unit");
    }
    expect(history.samples().size() == 256, "history remains bounded");
    steady(); // The full diagnostic history must not bring the frame allocation back.
    expect(history.samples().front().id == long_id, "samples own their retained IDs");
}
void loader() {
    test::ui::TempTree tree("cursors");
    test::ui::write_text(tree.root / "xml/mousepointerfiles.xml", "<MousePointerFiles><File>MousePointers.xml</File></MousePointerFiles>");
    test::ui::write_text(tree.root / "xml/mousepointers.xml",
        "<MousePointers><MousePointer Name='POINTER_ATTACK'><Base_Texture>attack_00.tga</Base_Texture>"
        "<Hot_X>16</Hot_X><Hot_Y>15</Hot_Y><Anim_Frame_Delay>10</Anim_Frame_Delay></MousePointer>"
        "<MousePointer Name='POINTER_NORMAL'><Base_Texture>static.tga</Base_Texture></MousePointer></MousePointers>");
    for (const auto name : {"attack_00.dds", "attack_01.dds", "attack_03.dds", "static.dds"})
        test::ui::write_text(tree.root / "art/textures" / name, "synthetic frame; metadata lookup only");
    const std::array mounts{vfs::MountSpec{"synthetic", tree.root, "data", {}}};
    auto filesystem = vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(filesystem), "synthetic cursor VFS");
    if (!filesystem) return;
    const auto loaded = data::ui::load_cursors(filesystem.value());
    expect(static_cast<bool>(loaded), "cursor catalog parses");
    if (!loaded) return;
    const auto& attack = loaded.value().at("POINTER_ATTACK");
    expect(attack.hot_x == 16 && attack.hot_y == 15 && attack.frame_delay == 10, "authored hotspot and timing");
    expect(attack.frames.size() == 2, "numbered sequence stops at first gap, TGA resolves DDS");
    expect(loaded.value().at("POINTER_NORMAL").frames.size() == 1, "unnumbered art loads once");
    test::ui::write_text(tree.root / "xml/mousepointers.xml",
        "<MousePointers><MousePointer Name='broken'><Base_Texture>static.tga</Base_Texture><Hot_X>-1</Hot_X></MousePointer></MousePointers>");
    expect(!data::ui::load_cursors(filesystem.value()), "negative hotspot is rejected");
    test::ui::write_text(tree.root / "xml/mousepointers.xml", "<!DOCTYPE x><MousePointers/>");
    expect(!data::ui::load_cursors(filesystem.value()), "hardened XML rejects doctype");
    if (auto corpus = test::ui::foc_corpus("cursors")) {
        const auto actual = data::ui::load_cursors(*corpus);
        expect(static_cast<bool>(actual), "installed FoC pointers load");
        if (actual) {
            expect(actual.value().size() == 53, "complete FoC pointer catalog");
            expect(actual.value().at("POINTER_ATTACK").frames.size() == 4, "attack animation has four consecutive frames");
            expect(actual.value().at("POINTER_ATTACK").hot_x == 16, "attack click offset is authored");
        }
    }
}
}
int main() {
    states(); animation(); steady_history(); loader();
    return eawr::test::ui::failures() ? 1 : 0;
}
