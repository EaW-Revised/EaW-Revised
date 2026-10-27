#pragma once

#include "unit_tables_support.hpp"

namespace unit_tables_test_support {

void foc_space_healing(eawr::units::LoadInput input);
void foc_active_projectile_defence(eawr::units::LoadInput input);
void foc_stealth(eawr::units::LoadInput input);
void foc_laser_defence(eawr::units::LoadInput input);
void foc_redirect_blaster(eawr::units::LoadInput input);
void foc_passive_projectile_defence(eawr::units::LoadInput input);
void foc_bomber_torpedo_volley(eawr::units::LoadInput input);
void foc_diamond_boron_cluster(eawr::units::LoadInput input);
void foc_barrage_buttons(eawr::units::LoadInput input);
void foc_super_capitals(eawr::units::LoadInput input);
void foc_resource_container(eawr::units::LoadInput input);
void foc_container_not_opportunity(eawr::units::LoadInput input);
void foc_living_collision(eawr::units::LoadInput input);
void foc_gunboat_pole(eawr::units::LoadInput input);
void foc_fighter_muzzles(eawr::units::LoadInput input);
void foc_object_weapon_defaults(eawr::units::LoadInput input);
void foc_mass_drivers(eawr::units::LoadInput input);
void foc_hazard_maps(eawr::units::LoadInput input);
void foc_leech_shields(eawr::units::LoadInput input);
void foc_lure(eawr::units::LoadInput input);
void foc_full_salvo(eawr::units::LoadInput input);
void foc_self_destruct(eawr::units::LoadInput input);
void foc_peacebringer_blast(eawr::units::LoadInput input);
void foc_asymmetric_release_geometry(eawr::units::LoadInput input);
void foc_special_without_blast(eawr::units::LoadInput input);
void foc_corrupt_systems(eawr::units::LoadInput input);
void foc_planetary_sources(const eawr::units::UnitTables& tables, eawr::units::LoadInput input);
void foc_hardpoint_ranks(eawr::units::LoadInput input);
void foc_land_companies(eawr::units::LoadInput input);

} // namespace unit_tables_test_support
