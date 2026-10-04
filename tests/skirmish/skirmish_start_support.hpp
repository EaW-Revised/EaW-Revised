#pragma once

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

namespace skirmish = eawr::skirmish;
namespace tactical = eawr::sim::tactical;
using eawr::units::Fixed;
using eawr::units::UnitKind;
using eawr::units::Vec3;

extern int failures;



extern const std::string_view m2_tick_zero_state;
extern const std::string_view m2_content_identity;
void expect(const bool condition, const std::string_view message);
std::optional<std::string> environment(const char* name);
Fixed whole(const std::int64_t value);
Vec3 at(const std::int64_t x, const std::int64_t y);
bool near(const Fixed value, const std::int64_t raw, const std::int64_t quanta = 8);
skirmish::MapPlacement marker(const std::uint32_t record, std::string type, const Vec3 position, const std::int64_t yaw);
skirmish::MapPlacement object(const std::uint32_t record, std::string type, std::string element,
    const std::int32_t owner, std::string faction, const Vec3 position, const std::int64_t yaw);
eawr::units::UnitType unit_type(std::string id, const UnitKind kind, std::string affiliation,
    const std::optional<std::int64_t> power);
void synthetic_roster_gate();
void synthetic_start();
void synthetic_placement();
void synthetic_failures();
void replay_round_trip();
std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path);
void synthetic_heights();
void foc_pad_capture_build(const skirmish::SkirmishStart& start, const skirmish::StartInputs& inputs,
                           const eawr::units::UnitTables& tables);
void foc_start(const std::filesystem::path& fixtures);
void setup_contract();


} // namespace skirmish_start_test_support
