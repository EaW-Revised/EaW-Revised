#include "skirmish_start_support.hpp"
// P2-04 (#67) tick-zero contracts. The synthetic placements, factions and unit
// tables are invented here. The committed m2-start replay is checked without
// the game: its header and setup alone give the pinned tick-zero hash. With
// EAWR_EAW_GAME_ROOT set, the pinned FoC fixture is also built read-only from
// the installation and must reproduce that replay byte for byte; nothing from
// the installation is written.

#include "eawr/skirmish/placement.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/skirmish/setup.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "eawr/skirmish/ai.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace skirmish_start_test_support {


// The tick-zero state hash of the committed m2-start replay (FoC data, fixture seed 67).
constexpr std::string_view m2_tick_zero_state = "9c52d81b8736b411c16cbcb68af561e5d8106cd108946a888a946c162b240b6e";
// docs/unit-data.md: the FoC fleet's unit-table identity, the replay's content identity.
constexpr std::string_view m2_content_identity = "9cc71553878e93b6dcc5a5c70662227ff2e294a8e8feeb77b6cf64eec67384d5";

int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

Fixed whole(const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }

Vec3 at(const std::int64_t x, const std::int64_t y) { return {whole(x), whole(y), Fixed{}}; }

std::string hex(const std::array<std::uint8_t, 32>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : bytes) {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 15U]);
    }
    return text;
}

bool near(const Fixed value, const std::int64_t raw, const std::int64_t quanta) {
    const auto difference = value.raw() - raw;
    return difference >= -quanta && difference <= quanta;
}

skirmish::MapPlacement marker(const std::uint32_t record, std::string type, const Vec3 position, const std::int64_t yaw) {
    skirmish::MapPlacement placement;
    placement.record = record;
    placement.type = std::move(type);
    placement.element = "Marker";
    placement.owner_index = 3;
    placement.owner_faction = "Neutral";
    placement.position = position;
    placement.orientation_degrees = Vec3{Fixed{}, Fixed{}, whole(yaw)};
    placement.marker = true;
    return placement;
}

skirmish::MapPlacement object(const std::uint32_t record, std::string type, std::string element,
    const std::int32_t owner, std::string faction, const Vec3 position, const std::int64_t yaw) {
    skirmish::MapPlacement placement;
    placement.record = record;
    placement.type = std::move(type);
    placement.element = std::move(element);
    placement.owner_index = owner;
    placement.owner_faction = std::move(faction);
    placement.position = position;
    placement.orientation_degrees = Vec3{Fixed{}, Fixed{}, whole(yaw)};
    placement.hull = whole(20);
    return placement;
}

eawr::units::UnitType unit_type(std::string id, const UnitKind kind, std::string affiliation,
    const std::optional<std::int64_t> power) {
    eawr::units::UnitType type;
    type.id = std::move(id);
    type.kind = kind;
    type.living_projectile_collision = kind != UnitKind::squadron;
    type.affiliation = std::move(affiliation);
    if (power) type.ai_combat_power = whole(*power);
    return type;
}

std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// The committed replay of the M2 start, without the game.
void committed_fixture(const std::filesystem::path& fixtures) {
    const auto bytes = read_file(fixtures / "m2-start.eawr-replay");
    expect(bytes.has_value(), "m2-start fixture is readable");
    if (!bytes) return;
    const auto replay = tactical::parse_replay(*bytes, "m2-start.eawr-replay");
    expect(static_cast<bool>(replay), "m2-start fixture parses");
    if (!replay) return;
    const auto& setup = replay.value().setup;
    expect(replay.value().commands.empty() && replay.value().final_tick_count == 30, "setup alone, 30 ticks");
    expect(hex(setup.content_identity) == m2_content_identity, "content identity is the FoC unit-table identity");
    expect(setup.seed == skirmish::m2_fixture().seed, "fixture seed");
    expect(setup.players.size() == 7 && setup.units.size() == 62 && setup.squadrons.size() == 5,
           "7 players (#272), 35 units and the 27 craft of 5 squadrons (#75)");
    const auto session = tactical::TacticalSession::from_replay(replay.value());
    expect(session && session.value().state_sha256() == m2_tick_zero_state, "pinned tick-zero state hash");
    const auto census = skirmish::census_json(setup, nullptr);
    expect(census && census.value().find(std::string(m2_tick_zero_state)) != std::string::npos,
           "the census from the replay carries the tick-zero hash");
}
} // namespace

using namespace skirmish_start_test_support;

int main(const int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: skirmish_start_tests <fixtures directory>\n";
        return 2;
    }
    const std::filesystem::path fixtures(argv[1]);
    setup_contract();
    synthetic_start();
    synthetic_roster_gate();
    synthetic_placement();
    synthetic_failures();
    replay_round_trip();
    synthetic_heights();
    committed_fixture(fixtures);
    foc_start(fixtures);
    if (failures != 0) {
        std::cerr << failures << " skirmish start check(s) failed\n";
        return 1;
    }
    std::cout << "skirmish start contracts passed\n";
    return 0;
}
