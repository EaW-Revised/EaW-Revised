#include "unit_internal.hpp"

#include "eawr/data/tag_trace.hpp"
#include "eawr/scene/scene.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <functional>
#include <iterator>
#include <system_error>

namespace eawr::units::detail {
namespace {

[[nodiscard]] char fold(const char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] bool space(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' || value == '\v';
}

[[nodiscard]] std::optional<std::string> attribute(const data::XmlNode& node, const std::string_view name) {
    for (const auto& item : node.attributes) {
        if (!iequals(item.name, name)) continue;
        data::tag_trace::used_attribute(node, item.name);
        return item.value;
    }
    return std::nullopt;
}

[[nodiscard]] const data::XmlNode* last_child(const data::XmlNode& node, const std::string_view name) {
    const data::XmlNode* result = nullptr;
    for (const auto& child : node.children) {
        if (iequals(child.name, name)) result = &child;
    }
    data::tag_trace::used(result);
    return result;
}

[[nodiscard]] std::vector<std::string> child_tokens(const data::XmlNode& node, const std::string_view name) {
    std::vector<std::string> result;
    for (const auto& child : node.children) {
        if (!iequals(child.name, name)) continue;
        data::tag_trace::used(child);
        for (auto& token : tokens(child.raw_text)) result.push_back(std::move(token));
    }
    return result;
}

// child_tokens for a mask tag, which also splits on `|`.
[[nodiscard]] std::vector<std::string> mask_tokens(const data::XmlNode& node, const std::string_view name) {
    std::vector<std::string> result;
    for (const auto& child : node.children) {
        if (!iequals(child.name, name)) continue;
        data::tag_trace::used(child);
        std::string spaced = child.raw_text;
        std::replace(spaced.begin(), spaced.end(), '|', ' ');
        for (auto& token : tokens(spaced)) result.push_back(std::move(token));
    }
    return result;
}

void record(Report& report, const data::XmlDocument& document) {
    report.input({document.source.logical_path, document.source.source_id, document.source.layer_id,
                  document.input_sha256});
}

// gameconstants.xml scalars the M2 combat tickets read. Booleans load as 0/1; a list
// (Diminishing_Firepower) keeps its text.
constexpr std::string_view combat_scalars[] = {
    "Diminishing_Firepower",              // #74
    "ShieldRechargeIntervalInSecs",
    "EnergyRechargeIntervalInSecs",
    "EnergyToShieldExchangeRate",
    "Depleted_Shield_Damage_Increment",
    "Depleted_Shield_Disable_Time",
    "Depleted_Shield_Regen_Cap",
    "Hull_Vs_Hard_Points_Health_Constraint",
    "Hardpoint_Recharge_Cutoff_For_Opportunity_Fire",
    "Engines_Disabled_Speed_Modifier",
    "Space_Elevated_Vulnerability_Duration",
    "Space_Elevated_Vulnerability_Factor",
    "Space_Reinforcement_Collision_Check_Distance", // WR-25
    "Object_Max_Speed_Multiplier_Space",
    "Auto_Rotate_For_Space_Targeting",
    "Bombing_Run_Reduction_Per_Squadron_Percent",
    "Object_Max_Health_Multiplier_Space", // #72
    "Health_Low_Percent_Threshold",       // #72
    "MaxRotationsSpace",                  // #70
    "XYExpansionDistanceSpace",           // #70
    "TurnInPlaceSlowdownCorvette",        // #70
    "TurnInPlaceSlowdownFrigate",         // #70
    "TurnInPlaceSlowdownCapital",         // #70
    "WaitOperatorSpeedCoefficient",       // #71
    "WaitOperatorBaseFrameTime",          // #71
    "WaitOperatorCostCoefficient",        // #71
    "MinObstacleCostSpace",               // #71
    "CurrentPathCostCoefficientSpace",    // #71
    "OccupationRadiusCoefficientSpace",   // #71
    "SpacePathFailureDistanceCutoffCoefficient",  // #71
    "SpacePathFailureMaxExpansionsCoefficient",   // #71
    "SpacePathFailureRotationExpansionIncrement", // #71
    "SpacePathFailureForwardExpansionIncrement",  // #71
    "SpacePathfindMaxExpansions",         // #71
    "SpacePathingTries",                  // #71
    "SpaceObjectTrackingInterval",        // #71
    "SpaceObjectTrackingTreeCount",       // #71
    "DestinationSearchRadiusIncrementSpace", // #266
    "MP_Default_Credits",                 // #530 PU-01
    "Tactical_Build_Time_Multiplier",     // #530 PU-13
    "Allow_Reinforcement_Percentage_Normalized", // #530 PU-21
    "FormationMinimumSideError",          // #599
    "FormationMaximumSideError",          // #599
};

constexpr std::string_view constants_path = "data/xml/gameconstants.xml";
constexpr std::string_view priority_registry = "data/xml/targetingprioritysetfiles.xml";
constexpr std::string_view category_enum_path = "data/xml/enum/gameobjectcategorytype.xml";
constexpr std::string_view property_enum_path = "data/xml/enum/gameobjectpropertiestype.xml";

// The names Hard_Point_Priorities and Hard_Point_Exclusions accept (FoC's hardpoint-type
// converter for priority sets), matched without case.
constexpr std::string_view hard_point_names[] = {"Engine", "Shield_Generator", "Gravity_Well", "Fighter_Bay",
    "Tractor_Beam", "Weapon_Laser", "Weapon_Missile", "Weapon_Torpedo", "Weapon_Ion_Cannon", "Weapon_Mass_Driver",
    "Weapon_Special", "Dummy_Art"};

// An enum value: `0x` hexadecimal or decimal, 64 bits.
[[nodiscard]] std::optional<std::uint64_t> enum_number(const std::string_view text) {
    const auto value = trim(text);
    const bool hex = value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X');
    const auto* begin = value.data() + (hex ? 2 : 0);
    const auto* end = value.data() + value.size();
    std::uint64_t result{};
    const auto [next, error] = std::from_chars(begin, end, result, hex ? 16 : 10);
    if (error != std::errc{} || next != end || begin == end) return std::nullopt;
    return result;
}

void load_enum(const vfs::Vfs& filesystem, const std::string_view path, std::vector<EnumValue>& values,
               Report& report) {
    auto document = data::load_document(filesystem, path);
    if (!document) {
        report.missing("EnumDefinition", "file", std::string(path), document.error().message);
        return;
    }
    record(report, document.value());
    data::tag_trace::document(document.value().root);
    for (const auto& child : document.value().root.children) {
        data::tag_trace::used(child);
        const auto value = enum_number(child.raw_text);
        if (!value) {
            report.missing(std::string(path), child.name, trim(child.raw_text), "enum value is not a 64-bit number");
            continue;
        }
        if (enum_value(values, child.name)) {
            report.note(std::string(path), child.name, trim(child.raw_text), "enum name defined again; the first is used");
            continue;
        }
        values.push_back({child.name, *value});
    }
}

// Hardpoint names in a priority set, unknown names reported and dropped.
[[nodiscard]] std::vector<std::string> hard_points(const data::XmlNode& node, const std::string_view tag,
                                                   const std::string& owner, Report& report) {
    std::vector<std::string> result;
    for (auto& name : child_tokens(node, tag)) {
        if (std::none_of(std::begin(hard_point_names), std::end(hard_point_names),
                         [&](const std::string_view known) { return iequals(known, name); })) {
            report.missing(owner, std::string(tag), name, "not a hardpoint type name");
            continue;
        }
        result.push_back(std::move(name));
    }
    return result;
}

} // namespace

void load_enums(const vfs::Vfs& filesystem, UnitTables& tables, Report& report) {
    load_enum(filesystem, category_enum_path, tables.categories, report);
    load_enum(filesystem, property_enum_path, tables.properties, report);
}

std::optional<std::uint64_t> enum_value(const std::vector<EnumValue>& values, const std::string_view name) {
    for (const auto& value : values) {
        if (iequals(value.name, name)) return value.value;
    }
    return std::nullopt;
}

std::uint64_t enum_bits(const std::vector<EnumValue>& values, const std::vector<std::string>& names,
                        const std::string& owner, const std::string_view field, Report& report) {
    std::uint64_t bits = 0;
    for (const auto& name : names) {
        const auto value = enum_value(values, name);
        if (value) {
            bits |= *value;
        } else {
            report.missing(owner, std::string(field), name, "not an enum name");
        }
    }
    return bits;
}

std::string trim(const std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && space(value[begin])) ++begin;
    while (end > begin && space(value[end - 1])) --end;
    return std::string(value.substr(begin, end - begin));
}

std::string lower(const std::string_view value) {
    std::string result(value);
    for (char& item : result) item = fold(item);
    return result;
}

std::string upper(const std::string_view value) {
    std::string result(value);
    for (char& item : result) {
        if (item >= 'a' && item <= 'z') item = static_cast<char>(item - 'a' + 'A');
    }
    return result;
}

bool iequals(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (fold(left[index]) != fold(right[index])) return false;
    }
    return true;
}

std::vector<std::string> tokens(const std::string_view text) {
    std::vector<std::string> result;
    std::string current;
    for (const char item : text) {
        if (item == ',' || space(item)) {
            if (!current.empty()) result.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(item);
        }
    }
    if (!current.empty()) result.push_back(std::move(current));
    return result;
}

std::optional<Fixed> number(const std::string_view text) {
    std::string value = trim(text);
    if (value.size() > 1 && (value.back() == 'f' || value.back() == 'F')) value.pop_back();
    auto parsed = Fixed::from_decimal(value);
    if (!parsed) return std::nullopt;
    return parsed.value();
}

std::optional<bool> boolean(const std::string_view text) {
    const std::string value = lower(trim(text));
    if (value == "yes" || value == "true" || value == "1") return true;
    if (value == "no" || value == "false" || value == "0") return false;
    return std::nullopt;
}

std::string model_path(const std::string_view name) {
    std::string value = lower(trim(name));
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.empty()) return {};
    if (!value.ends_with(".alo")) value += ".alo";
    return "data/art/models/" + value;
}

const data::XmlNode* Object::single(const std::string_view tag) {
    read_single.insert(lower(tag));
    const auto* found = effective.value(tag);
    return found == nullptr ? nullptr : &found->value;
}

std::string Object::text(const std::string_view tag) {
    const auto* node = single(tag);
    return node == nullptr ? std::string{} : trim(node->raw_text);
}

std::optional<Fixed> Object::fixed(const std::string_view tag, Report& report, const bool required) {
    const auto* node = single(tag);
    if (node == nullptr) {
        if (required) report.missing(effective.object_id, std::string(tag), {}, "required tag is absent");
        return std::nullopt;
    }
    auto value = number(node->raw_text);
    if (!value) report.missing(effective.object_id, std::string(tag), trim(node->raw_text), "not a decimal in Q24 range");
    return value;
}

std::vector<const data::XmlNode*> Object::list(const std::string_view tag, Report& report) const {
    std::vector<const data::XmlNode*> result;
    std::string authored_by;
    for (const auto* layer : layers) {
        std::vector<const data::XmlNode*> found;
        for (const auto& child : layer->root.children) {
            if (iequals(child.name, tag)) found.push_back(&child);
        }
        if (found.empty()) continue;
        if (result.empty()) {
            for (const auto* node : found) data::tag_trace::used(node);
            result = std::move(found);
            authored_by = layer->id;
        } else {
            report.note(effective.object_id, std::string(tag), layer->id,
                        "list tag also authored by base layer; the derived layer " + authored_by + " is used");
        }
    }
    return result;
}

void Object::note_duplicates(Report& report) const {
    for (const auto* layer : layers) {
        std::map<std::string, std::vector<std::string>> seen;
        for (const auto& child : layer->root.children) {
            const auto key = lower(child.name);
            if (read_single.contains(key)) seen[key].push_back(trim(child.raw_text));
        }
        for (const auto& [tag, values] : seen) {
            if (values.size() < 2) continue;
            std::string joined;
            for (const auto& value : values) joined += (joined.empty() ? "" : " | ") + value;
            report.note(layer->id, tag, joined, "single-value tag authored " + std::to_string(values.size()) +
                        " times in one layer; the last occurrence is used");
        }
    }
}

std::optional<Object> resolve(const data::Catalog& catalog, const std::string_view id,
                              const std::string_view owner, const std::string_view field, Report& report) {
    auto resolved = catalog.resolve(id);
    if (!resolved) {
        report.missing(std::string(owner), std::string(field), std::string(id), resolved.error().message);
        return std::nullopt;
    }
    Object result{std::move(resolved).value(), {}, {}};
    for (const auto& name : result.effective.chain) {
        const auto* definition = catalog.find(name);
        if (definition != nullptr) result.layers.push_back(definition);
    }
    return result;
}

std::optional<std::size_t> bone_index(const assets::Model& model, const std::string_view bone) {
    for (std::size_t index = 0; index < model.bones.size(); ++index) {
        if (iequals(model.bones[index].name, bone)) return index;
    }
    return std::nullopt;
}

std::optional<CollisionBounds> collision_bounds(
    const assets::Model& model, const std::vector<sim::math::Mat3x4>& frames) {
    std::optional<CollisionBounds> result;
    for (const auto& mesh : model.meshes) {
        if (!mesh.collidable) continue;
        const auto low = scene::fixed_from_binary32(mesh.bounds_min.x);
        const auto low_y = scene::fixed_from_binary32(mesh.bounds_min.y);
        const auto low_z = scene::fixed_from_binary32(mesh.bounds_min.z);
        const auto high = scene::fixed_from_binary32(mesh.bounds_max.x);
        const auto high_y = scene::fixed_from_binary32(mesh.bounds_max.y);
        const auto high_z = scene::fixed_from_binary32(mesh.bounds_max.z);
        if (!low || !low_y || !low_z || !high || !high_y || !high_z) continue;
        const auto frame = mesh.bone >= 0 && static_cast<std::size_t>(mesh.bone) < frames.size()
            ? frames[static_cast<std::size_t>(mesh.bone)]
            : sim::math::identity_matrix();
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 local{(corner & 1) != 0 ? high.value() : low.value(), (corner & 2) != 0 ? high_y.value() : low_y.value(),
                (corner & 4) != 0 ? high_z.value() : low_z.value()};
            const auto placed = sim::math::transform_point(frame, local);
            if (!placed) continue;
            const auto& point = placed.value();
            if (!result) {
                result = CollisionBounds{point, point};
                continue;
            }
            result->min = {std::min(result->min.x, point.x), std::min(result->min.y, point.y), std::min(result->min.z, point.z)};
            result->max = {std::max(result->max.x, point.x), std::max(result->max.y, point.y), std::max(result->max.z, point.z)};
        }
    }
    return result;
}

void append_collision_meshes(const assets::Model& model, const std::vector<sim::math::Mat3x4>& frames,
    const sim::math::Mat3x4& base, const std::uint32_t hardpoint, std::vector<CollisionMesh>& out) {
    for (const auto& mesh : model.meshes) {
        if (!mesh.collidable) continue;
        CollisionMesh entry;
        entry.name = mesh.name;
        entry.hardpoint = hardpoint;
        const auto frame_of = [&](const std::int64_t bone) {
            auto local = bone >= 0 && static_cast<std::size_t>(bone) < frames.size()
                ? frames[static_cast<std::size_t>(bone)]
                : sim::math::identity_matrix();
            return sim::math::compose(base, local);
        };
        const auto mesh_frame = frame_of(mesh.bone);
        if (!mesh_frame) continue;
        for (const auto& submesh : mesh.submeshes) {
            // A skinned vertex follows its first bone; a rigid one its mesh's bone.
            std::vector<Vec3> placed;
            placed.reserve(submesh.vertices.size());
            bool usable = true;
            for (const auto& vertex : submesh.vertices) {
                const auto x = scene::fixed_from_binary32(vertex.position.x);
                const auto y = scene::fixed_from_binary32(vertex.position.y);
                const auto z = scene::fixed_from_binary32(vertex.position.z);
                if (!x || !y || !z) {
                    usable = false;
                    break;
                }
                auto frame = mesh_frame;
                if (!submesh.skin_bones.empty() && vertex.bone_indices[0] < submesh.skin_bones.size()) {
                    frame = frame_of(static_cast<std::int64_t>(submesh.skin_bones[vertex.bone_indices[0]]));
                }
                auto point = frame ? sim::math::transform_point(frame.value(), Vec3{x.value(), y.value(), z.value()})
                                   : core::Result<Vec3>::failure(frame.error());
                if (!point) {
                    usable = false;
                    break;
                }
                placed.push_back(point.value());
            }
            if (!usable) continue;
            for (std::size_t index = 0; index + 2 < submesh.indices.size(); index += 3) {
                const auto a = submesh.indices[index];
                const auto b = submesh.indices[index + 1];
                const auto c = submesh.indices[index + 2];
                if (a >= placed.size() || b >= placed.size() || c >= placed.size()) continue;
                entry.triangles.push_back({placed[a], placed[b], placed[c]});
            }
        }
        if (!entry.triangles.empty()) out.push_back(std::move(entry));
    }
}

Vec3 translation(const sim::math::Mat3x4& frame) noexcept {
    return Vec3{frame.rows[0][3], frame.rows[1][3], frame.rows[2][3]};
}

std::array<Vec3, 3> axes(const sim::math::Mat3x4& frame) noexcept {
    const auto column = [&](const std::size_t index) {
        return Vec3{frame.rows[0][index], frame.rows[1][index], frame.rows[2][index]};
    };
    return {column(0), column(1), column(2)};
}

void load_priority_sets(const vfs::Vfs& filesystem, const std::vector<std::string>& wanted,
                        UnitTables& tables, Report& report) {
    if (wanted.empty()) return;
    auto registry = data::load_document(filesystem, priority_registry);
    if (!registry) {
        report.missing("TargetingPrioritySet", "file", std::string(priority_registry), registry.error().message);
        return;
    }
    record(report, registry.value());
    // #628: the registry is used whole; of its files, only the sets taken below.
    data::tag_trace::document(registry.value().root);
    std::map<std::string, data::XmlNode> sets;
    for (const auto& file : registry.value().root.children) {
        if (!iequals(file.name, "File")) continue;
        data::tag_trace::used(file);
        const auto path = data::registry_include_path(priority_registry, trim(file.raw_text));
        auto document = data::load_document(filesystem, path);
        if (!document) {
            report.missing("TargetingPrioritySet", "file", path, document.error().message);
            continue;
        }
        record(report, document.value());
        for (auto& set : document.value().root.children) {
            const auto name = attribute(set, "Name");
            if (!name) continue;
            if (sets.contains(lower(*name))) {
                report.note("TargetingPrioritySet", "Name", *name, "defined again in " + path + "; the later one is used");
            }
            sets.insert_or_assign(lower(*name), std::move(set));
        }
    }
    for (const auto& id : wanted) {
        const auto found = sets.find(lower(id));
        if (found == sets.end()) {
            report.missing("TargetingPrioritySet", "Name", id, "no Priority_Set with this name");
            continue;
        }
        const auto& node = found->second;
        data::tag_trace::object(node, id);
        TargetingPrioritySet set;
        set.id = attribute(node, "Name").value_or(id);
        const auto pairs = child_tokens(node, "Attack_Priorities");
        if (pairs.size() % 2 != 0) {
            report.missing(set.id, "Attack_Priorities", {}, "odd token count; expected category, weight pairs");
        }
        for (std::size_t index = 0; index + 1 < pairs.size(); index += 2) {
            const auto weight = number(pairs[index + 1]);
            if (!weight) {
                report.missing(set.id, "Attack_Priorities", pairs[index + 1], "weight is not a decimal");
                continue;
            }
            // FoC tries the category enum, then the property enum, then takes an object type name.
            PriorityEntry entry{pairs[index], *weight, PriorityMatch::type, 0};
            if (const auto category = enum_value(tables.categories, entry.name)) {
                entry.match = PriorityMatch::category;
                entry.bits = *category;
            } else if (const auto property = enum_value(tables.properties, entry.name)) {
                entry.match = PriorityMatch::property;
                entry.bits = *property;
            }
            set.attack_priorities.push_back(std::move(entry));
        }
        set.hard_point_priorities = hard_points(node, "Hard_Point_Priorities", set.id, report);
        set.hard_point_exclusions = hard_points(node, "Hard_Point_Exclusions", set.id, report);
        set.category_exclusions = mask_tokens(node, "Category_Exclusions");
        set.property_exclusions = mask_tokens(node, "Property_Exclusions");
        set.unit_exclusions = child_tokens(node, "Unit_Exclusions");
        set.category_exclusion_bits =
            enum_bits(tables.categories, set.category_exclusions, set.id, "Category_Exclusions", report);
        set.property_exclusion_bits =
            enum_bits(tables.properties, set.property_exclusions, set.id, "Property_Exclusions", report);
        tables.priority_sets.push_back(std::move(set));
    }
}

void load_constants(const vfs::Vfs& filesystem, const std::set<std::string>& damage_types,
                    const std::set<std::string>& armor_types, UnitTables& tables, Report& report) {
    auto document = data::load_document(filesystem, constants_path);
    if (!document) {
        report.missing("GameConstants", "file", std::string(constants_path), document.error().message);
        return;
    }
    record(report, document.value());
    const auto& root = document.value().root;
    data::tag_trace::document(root);
    for (const auto tag : combat_scalars) {
        NamedConstant constant{std::string(tag), std::nullopt, {}};
        const auto* node = last_child(root, tag);
        if (node == nullptr) {
            report.missing("GameConstants", constant.tag, {}, "required constant is absent");
        } else {
            constant.text = trim(node->raw_text);
            if (const auto flag = boolean(constant.text)) {
                constant.value = Fixed::from_raw(*flag ? Fixed::scale : 0);
            } else if (constant.text.find(',') != std::string::npos) {
                // A list constant keeps only its text; each value must be a decimal.
                const auto values = tokens(constant.text);
                if (values.empty() || !std::all_of(values.begin(), values.end(),
                        [](const std::string& value) { return number(value).has_value(); })) {
                    report.missing("GameConstants", constant.tag, constant.text, "not a list of decimals");
                }
            } else {
                constant.value = number(constant.text);
            }
            if (!constant.value && constant.text.find(',') == std::string::npos) {
                report.missing("GameConstants", constant.tag, constant.text, "not a decimal or boolean");
            }
        }
        tables.constants.scalars.push_back(std::move(constant));
    }
    std::set<std::pair<std::string, std::string>> present;
    for (const auto& child : root.children) {
        if (!iequals(child.name, "Damage_To_Armor_Mod")) continue;
        data::tag_trace::used(child);
        const auto row = tokens(child.raw_text);
        if (row.size() != 3) continue;
        const auto damage = lower(row[0]);
        const auto armor = lower(row[1]);
        if (!damage_types.contains(damage) || !armor_types.contains(armor)) continue;
        const auto value = number(row[2]);
        if (!value) {
            report.missing("GameConstants", "Damage_To_Armor_Mod", row[0] + ", " + row[1], "multiplier is not a decimal");
            continue;
        }
        if (!present.insert({damage, armor}).second) {
            report.note("GameConstants", "Damage_To_Armor_Mod", row[0] + ", " + row[1],
                        "row authored again; both rows are kept in file order");
        }
        tables.constants.damage_to_armor.push_back({row[0], row[1], *value});
    }
    for (const auto& damage : damage_types) {
        for (const auto& armor : armor_types) {
            if (!present.contains({damage, armor})) {
                report.note("GameConstants", "Damage_To_Armor_Mod", damage + ", " + armor,
                            "no row for this fleet pair; it multiplies by 1 (space-damage DG-12)");
            }
        }
    }
}

// The model's collision bounds in the bind pose (the debug build's object-space bounds
// recalculation, research E71-18): each collidable mesh's box moved by its bone's frame (the
// centre transformed, each half extent the absolute row sums), united; the X and Y half
// extents of the union. A model without a collidable mesh has the box +-1.
std::optional<std::pair<Fixed, Fixed>> collision_half_extents(
    const assets::Model& model, const std::vector<sim::math::Mat3x4>& frames) {
    namespace math = sim::math;
    const Fixed one = Fixed::from_raw(Fixed::scale);
    std::optional<std::array<Fixed, 4>> box; // low x, low y, high x, high y
    for (const auto& mesh : model.meshes) {
        if (!mesh.collidable) continue;
        // The binary32 bounds go straight into the converter (the simulation boundary rule).
        const std::array<core::Result<Fixed>, 6> read{scene::fixed_from_binary32(mesh.bounds_min.x),
            scene::fixed_from_binary32(mesh.bounds_min.y), scene::fixed_from_binary32(mesh.bounds_min.z),
            scene::fixed_from_binary32(mesh.bounds_max.x), scene::fixed_from_binary32(mesh.bounds_max.y),
            scene::fixed_from_binary32(mesh.bounds_max.z)};
        std::array<Fixed, 3> low{};
        std::array<Fixed, 3> high{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (!read[axis] || !read[axis + 3]) return std::nullopt;
            low[axis] = read[axis].value();
            high[axis] = read[axis + 3].value();
        }
        math::Mat3x4 frame = math::identity_matrix();
        if (mesh.bone >= 0 && static_cast<std::size_t>(mesh.bone) < frames.size()) {
            frame = frames[static_cast<std::size_t>(mesh.bone)];
        }
        math::Vec3 centre{};
        std::array<Fixed, 3> half{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const Fixed sum = Fixed::from_raw(low[axis].raw() / 2 + high[axis].raw() / 2);
            (axis == 0 ? centre.x : axis == 1 ? centre.y : centre.z) = sum;
            half[axis] = Fixed::from_raw(high[axis].raw() / 2 - low[axis].raw() / 2);
        }
        auto moved = math::transform_point(frame, centre);
        if (!moved) return std::nullopt;
        std::array<Fixed, 4> mesh_box{}; // low x, low y, high x, high y
        for (std::size_t row = 0; row < 2; ++row) {
            Fixed reach{};
            for (std::size_t column = 0; column < 3; ++column) {
                const Fixed m = frame.rows[row][column];
                auto term = math::multiply(m.raw() < 0 ? Fixed::from_raw(-m.raw()) : m, half[column]);
                if (!term) return std::nullopt;
                auto sum = math::add(reach, term.value());
                if (!sum) return std::nullopt;
                reach = sum.value();
            }
            const Fixed at = row == 0 ? moved.value().x : moved.value().y;
            mesh_box[row] = Fixed::from_raw(at.raw() - reach.raw());
            mesh_box[row + 2] = Fixed::from_raw(at.raw() + reach.raw());
        }
        // The first mesh's box starts the union on both axes; later meshes widen it.
        if (!box) {
            box = mesh_box;
            continue;
        }
        auto& b = *box;
        for (std::size_t row = 0; row < 2; ++row) {
            b[row] = std::min(b[row], mesh_box[row]);
            b[row + 2] = std::max(b[row + 2], mesh_box[row + 2]);
        }
    }
    if (!box) return std::pair{one, one};
    const auto& b = *box;
    return std::pair{Fixed::from_raw(b[2].raw() / 2 - b[0].raw() / 2), Fixed::from_raw(b[3].raw() / 2 - b[1].raw() / 2)};
}

} // namespace eawr::units::detail

namespace eawr::units {

core::Result<std::vector<sim::math::Mat3x4>> bind_frames(const assets::Model& model) {
    const auto failure = [&](std::string message) {
        core::Diagnostic diagnostic;
        diagnostic.code = std::string(diagnostic_codes::bone_frame);
        diagnostic.message = std::move(message) + " in " + model.source.logical_path;
        return diagnostic;
    };
    const std::size_t count = model.bones.size();
    std::vector<std::optional<sim::math::Mat3x4>> frames(count);
    std::vector<bool> visiting(count, false);
    std::function<std::optional<core::Diagnostic>(std::size_t)> build = [&](const std::size_t index)
        -> std::optional<core::Diagnostic> {
        if (frames[index]) return std::nullopt;
        if (visiting[index]) return failure("bone hierarchy cycle");
        visiting[index] = true;
        const auto& bone = model.bones[index];
        sim::math::Mat3x4 local;
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                auto value = scene::fixed_from_binary32(bone.relative_transform[row * 4 + column]);
                if (!value) return failure("bone " + bone.name + " transform is not representable in Q24");
                local.rows[row][column] = value.value();
            }
        }
        if (bone.parent < 0) {
            frames[index] = local;
        } else {
            const auto parent = static_cast<std::size_t>(bone.parent);
            if (parent >= count) return failure("bone " + bone.name + " names a missing parent");
            if (auto error = build(parent)) return error;
            auto composed = sim::math::compose(*frames[parent], local);
            if (!composed) return failure("bone " + bone.name + " frame overflows Q24");
            frames[index] = composed.value();
        }
        visiting[index] = false;
        return std::nullopt;
    };
    std::vector<sim::math::Mat3x4> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        if (auto error = build(index)) return core::Result<std::vector<sim::math::Mat3x4>>::failure(std::move(*error));
        result.push_back(*frames[index]);
    }
    return core::Result<std::vector<sim::math::Mat3x4>>::success(std::move(result));
}

} // namespace eawr::units
