#pragma once

#include "eawr/assets/map.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/world.hpp"
#include "eawr/vfs/vfs.hpp"
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace unit_tables_test_support {

using eawr::units::Fixed;
using eawr::units::UnitKind;
using eawr::units::Vec3;
extern int failures;


struct TempTree;
struct Loaded;

extern const std::string_view hardpoints_xml;
void expect(const bool condition, const std::string_view message);
std::optional<std::string> environment(const char* name);
std::int64_t raw(const std::int64_t whole);
Vec3 point(const std::int64_t x, const std::int64_t y, const std::int64_t z);
void write(const std::filesystem::path& path, const std::string_view text);
std::string units_xml(const std::string_view frigate_health, const std::string_view select_sfx);
void write_fixture(const std::filesystem::path& root, const std::string& units);
eawr::assets::Bone bone(const std::string& name, const std::int32_t parent, const std::array<float, 3> translation,
                        const bool rotate_quarter_turn = false);
eawr::assets::Model model(const std::string& path, std::vector<eawr::assets::Bone> bones);
std::map<std::string, eawr::assets::Model> models(const float engine_x);
Loaded load(const std::filesystem::path& root, const std::map<std::string, eawr::assets::Model>& assets);
bool has_row(const std::vector<eawr::units::Unresolved>& rows, const std::string_view owner,
             const std::string_view field, const std::string_view value = {});
void object_weapon_defaults();
void squadron_container_health();
void living_collision_admission();
void presentation_admission();
void print_rows(const std::string_view label, const std::vector<eawr::units::Unresolved>& rows);
void mass_driver_type();
void synthetic_tables();
void priority_rules();
void content_identity();
void bind_frame_errors();
void foc_fleet();

struct TempTree final {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("eawr-unit-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempTree();
    ~TempTree();
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
};

struct Loaded final {
    std::optional<eawr::units::UnitTables> tables;
};


} // namespace unit_tables_test_support
