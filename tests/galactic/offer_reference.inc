// Test-only pre-memo offer implementation, retained as an independent reference.
core::Result<gc::ProductionOffer> offer(const Prepared& p, const CampaignSetupPlan& plan,
    const gc::GalacticSetup& setup, const gc::PlanetProduction& location,
    const FleetTypeBinding& binding, const units::GalacticType& type, OfferMemo& memo, Fixed* unrounded_price = nullptr) {
    (void)memo;
    gc::ProductionOffer admission; admission.type = binding.id;
    const bool company = behaves(type, "DUMMY_GROUND_COMPANY");
    const bool squadron = behaves(type, "DUMMY_SPACE_FIGHTER_SQUADRON");
    const bool station = behaves(type, "DUMMY_STAR_BASE");
    const bool ground_structure = behaves(type, "DUMMY_GROUND_STRUCTURE");
    const bool orbital_structure = behaves(type, "DUMMY_ORBITAL_STRUCTURE");
    const bool ground_base = behaves(type, "DUMMY_GROUND_BASE");
    const bool ordinary_ship = behaves(type, "DUMMY_STARSHIP");
    admission.domain = units::built_on_ground(type) ? gc::ProductionDomain::ground : gc::ProductionDomain::space;
    admission.kind = type.build_advances_tech_level.value_or(0) > 0 ? gc::ProductionKind::technology : station ? gc::ProductionKind::starbase : ground_structure || orbital_structure
        ? gc::ProductionKind::structure : gc::ProductionKind::unit;
    admission.direct_build_allowed = station || ground_structure || orbital_structure || ordinary_ship || company || squadron;
    if (ground_base) admission.direct_build_allowed = false;
    if (admission.kind == gc::ProductionKind::technology) {
        admission.direct_build_allowed = true; admission.target_tech = *type.build_advances_tech_level;
    }
    const auto* owner = player_source(plan, location.target_owner);
    if (!owner) return refuse<gc::ProductionOffer>("production owner is unbound");
    admission.affiliated = contains(type.affiliation, owner->faction);
    const auto original = layers(p.catalog, type.name, data::Category::game_object);
    if (const auto* node = data::retail::single(original, "Cinematic_Object_Only")) {
        data::tag_trace::used(node); const auto flag = data::retail::boolean(node->raw_text);
        if (!flag) return refuse<gc::ProductionOffer>("invalid cinematic flag");
        admission.cinematic = *flag;
    } // WGS-90: fresh false.
    if (const auto* node = data::retail::single(original, "Build_Can_Be_Unlocked_By_Slicer")) {
        data::tag_trace::used(node); const auto flag = data::retail::boolean(node->raw_text);
        if (!flag) return refuse<gc::ProductionOffer>("invalid slicer flag");
        admission.slicer_unlockable = *flag;
    }
    const auto* limit = data::retail::single(original, "Build_Max_Instances_Per_Planet");
    admission.planet_limit = limit ? data::retail::integer_constant(limit->raw_text) : -1; // LSA-06
    for (const auto& name : type.required_orbiting_units) {
        const auto id = p.identity(name);
        if (!id) {
            // CGS-07: a name that resolves to no object is a null reference; the eligibility check skips it.
            p.note("campaign_unresolved_prerequisite", type.name + ": Required_Orbiting_Units names " + p.unresolved(name)
                + "; the entry is ignored (CGS-07)");
            continue;
        }
        admission.required_orbiting_units.push_back(id);
    }
    auto structures = resolve_structure_requirements(type.required_special_structures,
        [&](const std::string_view name) { return p.identity(name); });
    if (!structures) return core::Result<gc::ProductionOffer>::failure(structures.error());
    for (const auto& name : structures.value().ignored)
        p.note("campaign_unresolved_prerequisite", type.name + ": Required_Special_Structures names " + p.unresolved(name)
            + "; the null entry follows the positional eligibility rule (CGS-07, RGC-09)");
    admission.required_structures = std::move(structures.value().groups);
    const auto* source = planet_source(p, plan, location.planet);
    if (!source) return refuse<gc::ProductionOffer>("unbound production planet");
    if (!type.required_planets.empty() && !contains(type.required_planets, source->name)) admission.direct_build_allowed = false;
    // WGEP-158..160: an authored positive cost never admits alone. Ground construction also needs a nonzero
    // required ground-base level or the ground-structure role; space construction a nonzero required
    // starbase level or the level-one starbase role. Heroes and variants follow their own resolved tags,
    // so a mod that authors a level makes them buildable. Every reader of the offer table (human list,
    // queue command, Lua production, AI producer options) sees this through the shared admission.
    const bool positive_cost = type.build_cost_credits.value_or(Fixed{}).raw() >= Fixed::scale;
    const bool built_on_ground = units::built_on_ground(type);
    const bool built_in_space = positive_cost
        && (type.required_star_base_level.value_or(0) != 0 || (station && type.base_level.value_or(0) == 1));
    if (!built_on_ground && !built_in_space) admission.direct_build_allowed = false;
    ProductionPreparation quantities;
    quantities.price_multiplier = quantities.time_multiplier = quantities.tech_purchase_multiplier = one;
    quantities.corruption_price_multiplier = quantities.corruption_time_multiplier = one;
    const auto faction = layers(p.catalog, owner->faction, data::Category::faction);
    const auto corruption_factor = [&](std::string_view tag) -> core::Result<Fixed> {
        const auto* node = data::retail::single(faction, tag);
        if (!node) return core::Result<Fixed>::success(one); // WGS-94: empty fresh vector getter fallback.
        data::tag_trace::used(node); const auto entries = words(node->raw_text);
        if (entries.empty()) return core::Result<Fixed>::success(one);
        const auto value = data::retail::float_value(entries.front());
        if (!value) return refuse<Fixed>("invalid corruption factor");
        return core::Result<Fixed>::success(value->value); // Fresh uncorrupted planet, WGEP-29/32.
    };
    const auto* faction_data = p.data.faction(owner->faction);
    if (!faction_data || !faction_data->benefits_from_corruption) return refuse<gc::ProductionOffer>("unbound corruption eligibility");
    if (!*faction_data->benefits_from_corruption && admission.affiliated) {
        auto price = corruption_factor("Corruption_Galactic_Production_Price_Multipliers");
        auto time = corruption_factor("Corruption_Galactic_Production_Time_Multipliers");
        if (!price) return core::Result<gc::ProductionOffer>::failure(price.error());
        if (!time) return core::Result<gc::ProductionOffer>::failure(time.error());
        quantities.corruption_price_multiplier = price.value(); quantities.corruption_time_multiplier = time.value();
    }
    quantities.ai_time_multiplier = human(setup, location.target_owner) ? one : p.ai_time;
    quantities.base_tech_multiplier = one;
    const auto difference = location.space_level - type.tech_level.value_or(0) - 1;
    if (admission.domain == gc::ProductionDomain::space && location.space_level > 0 && difference >= 0)
        quantities.base_tech_multiplier = p.base_time[static_cast<std::size_t>(std::min<std::int64_t>(difference, 4))];
    quantities.factory_count = 1;
    if (type.build_time_reduced_by_multiple_factories.value_or(false)) {
        std::int64_t count{};
        for (const auto& group : admission.required_structures) for (const auto id : group)
            count += static_cast<std::int64_t>(std::count_if(location.objects.begin(), location.objects.end(),
                [&](const auto& object) { return object.type == id && object.kind == gc::ProductionKind::structure; }));
        quantities.factory_count = std::max<std::int64_t>(1, count);
    }
    const auto* campaign = p.data.campaign(plan.campaign_name());
    quantities.tutorial = campaign && campaign->tutorial.value_or(false);
    auto adjusted = type;
    Fixed time;
    // WGEP-92: consume constructor defaults without mutating original facts.
    time = gc::production_product(adjusted.build_time_seconds.value_or(Fixed{}), p.production_speed);
    adjusted.build_time_seconds = time;
    if (unrounded_price) {
        *unrounded_price = gc::production_product(adjusted.build_cost_credits.value_or(Fixed{}),
            *quantities.corruption_price_multiplier);
    }
    return prepare_production_offer(adjusted, std::move(admission), quantities, &p.load_buffer);
}
