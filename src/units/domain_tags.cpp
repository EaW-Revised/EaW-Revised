#include "eawr/units/domain_tags.hpp"

#include <array>

namespace eawr::units {

namespace {

using sim::tactical::BattleDomain;

struct Row final {
    DomainTag stem;
    std::string_view space, land;
};

constexpr std::array<Row, 5> rows{{
    {DomainTag::reveal_range, "Space_FOW_Reveal_Range", "Land_FOW_Reveal_Range"},
    {DomainTag::layer, "Space_Layer", ""},
    {DomainTag::speed_multiplier, "Object_Max_Speed_Multiplier_Space", "Object_Max_Speed_Multiplier_Land"},
    {DomainTag::health_multiplier, "Object_Max_Health_Multiplier_Space", "Object_Max_Health_Multiplier_Land"},
    {DomainTag::model_name, "Space_Model_Name", "Land_Model_Name"},
}};

} // namespace

std::string_view domain_tag(const BattleDomain domain, const DomainTag stem) noexcept {
    for (const auto& row : rows)
        if (row.stem == stem) return domain == BattleDomain::land ? row.land : row.space;
    return {};
}

std::string_view domain_variant(const BattleDomain domain, const std::string_view space_spelling) noexcept {
    if (domain == BattleDomain::space) return space_spelling;
    for (const auto& row : rows)
        if (row.space == space_spelling && !row.land.empty()) return row.land;
    return space_spelling;
}

std::string_view automatic_activation_name(const BattleDomain domain) noexcept {
    return domain == BattleDomain::land ? "Ground_Automatic" : "Space_Automatic";
}

} // namespace eawr::units
