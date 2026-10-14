// MAT-01 to MAT-10 (docs/behaviour/main-menu-attract.md): the main-menu demo attract's settings,
// map choice and cycle, clocks and keys, and the saved camera it reads. Engine-free and needs
// no game data: the XML nodes and the map chunks are built here.

#include "eawr/skirmish/attract.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace skirmish = eawr::skirmish;
using skirmish::AttractAction;
using skirmish::AttractFrame;
using skirmish::AttractKey;
using skirmish::AttractManager;
using skirmish::AttractSettings;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

eawr::data::XmlNode node(std::string name, std::string text) {
    eawr::data::XmlNode result;
    result.name = std::move(name);
    result.raw_text = std::move(text);
    return result;
}

AttractFrame frame(const std::uint64_t now, const bool dialog = false, const bool menu = false,
                   const bool foreground = true) {
    AttractFrame result;
    result.now_ms = now;
    result.dialog_active = dialog;
    result.main_menu_active = menu;
    result.foreground = foreground;
    return result;
}

void settings_read() {
    eawr::data::XmlNode root = node("GameConstants", "");
    root.children.push_back(node("Main_Menu_Demo_Attract_Mode", " Yes "));
    root.children.push_back(node("Demo_Attract_Maps", "Demo_Attract_Kamino.ted,\n Demo_Attract_Korriban.ted"));
    root.children.push_back(node("demo_attract_maps", "Third.ted"));
    root.children.push_back(node("Demo_Attract_Map_Cycle_Delay_Seconds", "1200, 1000"));
    root.children.push_back(node("Demo_Attract_Start_Timeout_Seconds", "3600"));
    root.children.push_back(node("Unrelated", "x"));
    const AttractSettings settings = skirmish::attract_settings(root);
    expect(settings.menu_attract, "MAT-01: Main_Menu_Demo_Attract_Mode Yes turns the attract on");
    expect((settings.maps == std::vector<std::string>{"Demo_Attract_Kamino.ted", "Demo_Attract_Korriban.ted", "Third.ted"}),
        "MAT-03: the map list is read in order and a repeated element appends");
    expect((settings.cycle_delays == std::vector<std::int64_t>{1200, 1000}), "MAT-04: one delay per map by position");
    expect(settings.start_timeout_seconds == 3600, "MAT-05: the start timeout is whole seconds");

    eawr::data::XmlNode empty = node("GameConstants", "");
    const AttractSettings defaults = skirmish::attract_settings(empty);
    expect(!defaults.menu_attract && defaults.maps.empty() && defaults.cycle_delays.empty()
        && defaults.start_timeout_seconds == 0, "MAT-01: absent tags leave the attract off and empty");

    eawr::data::XmlNode off = node("GameConstants", "");
    off.children.push_back(node("Main_Menu_Demo_Attract_Mode", "No"));
    expect(!skirmish::attract_settings(off).menu_attract, "MAT-01: Main_Menu_Demo_Attract_Mode No keeps it off");
}

void map_paths() {
    expect(skirmish::attract_map_path("Demo_Attract_Kamino.ted") == "data/art/maps/demo_attract_kamino.ted",
        "MAT-03: the map file is under data/art/maps, case folded");
    expect(skirmish::attract_map_path("  Sub\\Map.ted ") == "data/art/maps/sub/map.ted",
        "MAT-03: separators fold and the name is trimmed");
    expect(skirmish::attract_map_path("").empty(), "MAT-03: an empty name is no map");
    expect(skirmish::attract_map_path("../x.ted").empty(), "MAT-03: a name leaving the maps directory is no map");
}

void cycle_and_clocks() {
    AttractSettings settings;
    settings.menu_attract = true;
    settings.maps = {"A.ted", "B.ted"};
    settings.cycle_delays = {10}; // the second map has none: 120 s
    settings.start_timeout_seconds = 1000;
    AttractManager manager(settings);
    expect(!manager.initialized() && !manager.active(), "MAT-02: a new manager is neither initialised nor running");
    expect(!manager.advance(0), "MAT-03: nothing starts before init");
    manager.init(1000);
    expect(manager.initialized() && !manager.active() && !manager.map_index(),
        "MAT-02: init sets the clocks and starts no map");
    expect(manager.idle_since_ms() == 1000 && manager.map_since_ms() == 1000, "MAT-02: both clocks start at now");
    expect(manager.service(frame(1500)).action == AttractAction::none, "MAT-04: not running, nothing to advance");

    expect(manager.advance(2000) == std::optional<std::size_t>(0), "MAT-03: the first start picks entry 0");
    expect(manager.active() && manager.map_since_ms() == 2000, "MAT-03: running, the map clock restarts");
    expect(manager.service(frame(12000)).action == AttractAction::none, "MAT-04: a delay is passed only past its end");
    expect(manager.service(frame(12001)).action == AttractAction::advance, "MAT-04: the map advances past its delay");
    expect(manager.advance(12001) == std::optional<std::size_t>(1), "MAT-03: the next start picks entry 1");
    // Entry 1 has no delay entry: 120 s.
    expect(manager.service(frame(12001 + 120000)).action == AttractAction::none, "MAT-04: the default delay is 120 s");
    expect(manager.service(frame(12001 + 120001)).action == AttractAction::advance,
        "MAT-04: past 120 s the map without a delay entry advances");
    expect(manager.advance(200000) == std::optional<std::size_t>(0), "MAT-03: the index wraps to entry 0");

    // MAT-06: input restarts the idle clock and resumes the battle.
    const auto result = manager.key(AttractKey::other, frame(250000));
    expect(!result.consumed && manager.idle_since_ms() == 250000, "MAT-06: any key refreshes the idle clock");
    expect(!manager.service(frame(250001)).paused, "MAT-05: input clears the pause");
    manager.pointer_moved(260000, false);
    expect(manager.idle_since_ms() == 260000, "MAT-06: a pointer move refreshes it while running");
    manager.window_activated(270000);
    expect(manager.idle_since_ms() == 270000, "MAT-06: a window activation refreshes it");
}

void start_timeout_pause() {
    AttractSettings settings;
    settings.menu_attract = true;
    settings.maps = {"A.ted"};
    settings.cycle_delays = {10};
    settings.start_timeout_seconds = 100;
    AttractManager manager(settings);
    manager.init(0);
    static_cast<void>(manager.advance(0));
    expect(!manager.service(frame(100000)).paused, "MAT-05: the pause needs the idle clock older than the timeout");
    const auto frozen = manager.service(frame(100001));
    expect(frozen.paused && frozen.action == AttractAction::none,
        "MAT-05: past the start timeout the battle is paused and a paused frame never advances");
    static_cast<void>(manager.key(AttractKey::other, frame(100002)));
    const auto resumed = manager.service(frame(100003));
    expect(!resumed.paused && resumed.action == AttractAction::advance,
        "MAT-05: input clears the pause, and the expired map then advances");
}

void dialogs_and_keys() {
    AttractSettings settings;
    settings.menu_attract = true;
    settings.maps = {"A.ted", "B.ted", "C.ted"};
    settings.cycle_delays = {5, 5, 5};
    settings.start_timeout_seconds = 3600;
    AttractManager manager(settings);
    manager.init(0);
    static_cast<void>(manager.advance(0));

    // MAT-04: with the main menu up the cycle also waits for 8 s without input and the foreground.
    expect(manager.service(frame(6000, true, true)).action == AttractAction::none,
        "MAT-04: the main menu defers the advance while input is recent");
    expect(manager.service(frame(8001, true, true, false)).action == AttractAction::none,
        "MAT-04: a background window defers the advance");
    expect(manager.service(frame(8001, true, true)).action == AttractAction::advance,
        "MAT-04: past 8 s idle, in front, the main menu lets the map advance");
    expect(manager.service(frame(8001, true, false)).action == AttractAction::none,
        "MAT-04: any other dialog waits 20 s");
    expect(manager.service(frame(20001, true, false)).action == AttractAction::advance,
        "MAT-04: past 20 s idle another dialog lets the map advance");

    // MAT-07: keys with no dialog up.
    expect(!manager.key(AttractKey::enter, frame(30000, true, true)).consumed,
        "MAT-07: with a dialog up the keys are not the attract's");
    auto enter = manager.key(AttractKey::enter, frame(30001));
    expect(enter.consumed && enter.action == AttractAction::advance, "MAT-07: Enter advances");
    static_cast<void>(manager.advance(30001));
    expect(manager.map_index() == std::optional<std::size_t>(1), "MAT-03: the advance after Enter picks the next map");
    auto back = manager.key(AttractKey::backspace, frame(30002));
    expect(back.consumed && back.action == AttractAction::advance, "MAT-07: Backspace advances");
    expect(manager.advance(30002) == std::optional<std::size_t>(1),
        "MAT-07: Backspace steps back then advances: the current map starts again");
    const auto before = manager.map_since_ms();
    auto minus = manager.key(AttractKey::minus, frame(30003));
    expect(minus.consumed && minus.action == AttractAction::none && manager.map_since_ms() == before - 5000,
        "MAT-07: minus moves the map clock back by the map's delay");
    expect(manager.service(frame(30004)).action == AttractAction::advance, "MAT-07: the skipped time ends the map");
    auto escape = manager.key(AttractKey::escape, frame(30005));
    expect(escape.consumed && escape.action == AttractAction::stop, "MAT-07: Escape stops the attract");
    expect(!manager.initialized() && !manager.active(), "MAT-13: a stop leaves the manager down");
    expect(manager.service(frame(31000)).action == AttractAction::none, "MAT-13: a stopped manager does nothing");
    manager.init(40000);
    expect(manager.advance(40000) == std::optional<std::size_t>(2), "MAT-13: the map index survives a stop");
}

void wrapping_clocks() {
    AttractSettings settings;
    settings.menu_attract = true;
    settings.maps = {"A.ted"};
    settings.cycle_delays = {10};
    settings.start_timeout_seconds = 100000;
    AttractManager manager(settings);
    constexpr std::uint64_t wrap = 0x100000000ULL;
    manager.init(wrap - 5000);
    static_cast<void>(manager.advance(wrap - 5000));
    // The game's clocks are 32-bit: 12 s later the elapsed time is 12000 though the value wrapped.
    expect(manager.service(frame(wrap + 7000)).action == AttractAction::advance,
        "MAT-04: the map clock is 32 bits and survives a wrap");
    expect(manager.service(frame(wrap + 4000)).action == AttractAction::none, "MAT-04: before the delay, across a wrap");

    AttractSettings never;
    never.menu_attract = true;
    never.maps = {"A.ted"};
    never.cycle_delays = {-1};
    never.start_timeout_seconds = 100000;
    AttractManager forever(never);
    forever.init(0);
    static_cast<void>(forever.advance(0));
    expect(forever.service(frame(1000000)).action == AttractAction::none,
        "MAT-04: a negative delay wraps in 32 bits to about 49 days");
}

void no_maps() {
    AttractSettings settings;
    settings.menu_attract = true;
    AttractManager manager(settings);
    manager.init(0);
    expect(!manager.advance(0) && !manager.active(), "MAT-03: an empty list starts nothing");
}

void ai_players() {
    const auto players = skirmish::attract_ai_players();
    expect(players.size() == 2 && players[0].first == 0U && players[0].second == "BasicRebel"
        && players[1].first == 6U && players[1].second == "AI_Player_Underworld",
        "MAT-09: editor index 0 runs BasicRebel and index 6 AI_Player_Underworld");
}

std::uint32_t bits_of(const float value) {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

void put_float(std::vector<std::byte>& bytes, const float value) {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof bits);
    for (std::size_t index = 0; index < 4; ++index) bytes.push_back(static_cast<std::byte>((bits >> (8U * index)) & 0xFFU));
}

void put_mini(std::vector<std::byte>& payload, const std::uint8_t id, const std::vector<float>& values) {
    payload.push_back(static_cast<std::byte>(id));
    payload.push_back(static_cast<std::byte>(values.size() * 4U));
    for (const float value : values) put_float(payload, value);
}

eawr::assets::Map map_with_camera(const std::vector<std::pair<std::uint8_t, std::vector<float>>>& minis,
                                  const std::uint32_t chunk_id = 261) {
    eawr::assets::Map map;
    eawr::assets::RawChunk camera;
    camera.id = chunk_id;
    for (const auto& [id, values] : minis) put_mini(camera.payload, id, values);
    eawr::assets::RawChunk root;
    root.id = 1;
    root.group = true;
    root.children.push_back(std::move(camera));
    map.chunks.push_back(std::move(root));
    return map;
}

void saved_camera() {
    const auto kamino = map_with_camera({{0, {47.07F}}, {1, {0.0F}}, {2, {-559.1F, -540.5F, 0.0F}}, {3, {2550.0F}},
                                         {4, {2550.0F}}, {8, {55.0F}}});
    const auto camera = skirmish::attract_camera(kamino);
    expect(camera.has_value(), "MAT-10: a map's camera chunk is read");
    if (camera) {
        expect(camera->pitch_degrees_bits == bits_of(47.07F) && camera->yaw_degrees_bits == bits_of(0.0F)
            && camera->look_at_bits[0] == bits_of(-559.1F) && camera->look_at_bits[1] == bits_of(-540.5F)
            && camera->look_at_bits[2] == bits_of(0.0F) && camera->distance_bits == bits_of(2550.0F)
            && camera->fov_degrees_bits == bits_of(55.0F), "MAT-10: pitch, yaw, look-at, distance and field of view");
    }
    expect(!skirmish::attract_camera(map_with_camera({{0, {47.0F}}, {1, {0.0F}}, {2, {1.0F, 2.0F, 3.0F}}, {3, {100.0F}}})),
        "MAT-10: a camera chunk without a field of view is no camera");
    expect(!skirmish::attract_camera(map_with_camera({{0, {47.0F}}, {1, {0.0F}}, {2, {1.0F, 2.0F}}, {3, {100.0F}}, {8, {55.0F}}})),
        "MAT-10: a short look-at is no camera");
    expect(!skirmish::attract_camera(map_with_camera({{0, {47.0F}}, {1, {0.0F}}, {2, {1.0F, 2.0F, 3.0F}}, {3, {0.0F}}, {8, {55.0F}}})),
        "MAT-10: a distance that is not positive is no camera");
    expect(!skirmish::attract_camera(map_with_camera({{0, {47.0F}}, {1, {0.0F}}, {2, {1.0F, 2.0F, 3.0F}}, {3, {100.0F}}, {8, {55.0F}}}, 260)),
        "MAT-10: only chunk 1/261 is the camera");
    expect(!skirmish::attract_camera(eawr::assets::Map{}), "MAT-10: a map with no chunks has no camera");
}

} // namespace

int main() {
    settings_read();
    map_paths();
    cycle_and_clocks();
    start_timeout_pause();
    dialogs_and_keys();
    wrapping_clocks();
    no_maps();
    ai_players();
    saved_camera();
    if (failures != 0) {
        std::cerr << failures << " attract check(s) failed\n";
        return 1;
    }
    std::cout << "attract contracts passed\n";
    return 0;
}
