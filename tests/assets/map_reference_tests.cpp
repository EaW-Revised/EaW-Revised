#include "map_test_support.hpp"

namespace eawr::tests::map_contracts {
// --- Placement identity, shape and orientation (T5, T8, T9, D3, D4) ---------
void placements() {
    using namespace eawr::assets;
    expect(object_type_crc("Skirmish_Build_Pad") == 0x09964BDAU, "public-name CRC vector 1");
    expect(object_type_crc("Base Shield Structure Position") == 0x5AA37424U, "public-name CRC vector 2");
    expect(object_type_crc("Land_Speeder") == 0x8BE3542CU, "public-name CRC vector 3");

    ObjectTypeCatalog catalog{{type_ref("Fixture_Prop", "fixture_prop.alo"), type_ref("Fixture_Marker")}};
    Fixture fixture;
    fixture.records = {Record{}, Record{"Fixture_Marker", std::array<float, 3>{0.0F, 0.0F, 45.0F}, true},
                       Record{"Fixture_Prop", std::array<float, 3>{10.0F, 20.0F, 30.0F}},
                       Record{"Fixture_Prop", std::nullopt},
                       Record{"Fixture_Prop", std::array<float, 3>{std::numeric_limits<float>::quiet_NaN(), 0.0F, 30.0F}}};
    auto result = load(fixture, catalog);
    expect(bool(result), "placement fixture loads");
    if (!result || result.value().placements.size() != 5) { expect(false, "five placements decode"); return; }
    const auto& p = result.value().placements;
    expect(p[0].type_resolution == TypeResolution::unique && p[0].type_crc == object_type_crc("Fixture_Prop"), "CRC placement resolves uniquely");
    expect(p[0].position && p[0].position->x == 10.0F && p[0].position->y == 20.0F && p[0].position->z == 30.0F,
           "mini 4 is the authored position, not cached mini 21");
    expect(p[0].orientation_degrees && p[0].orientation_degrees->x == 0.0F && p[0].orientation_degrees->z == 90.0F,
           "mini 5 is the authored orientation, not cached mini 18");
    expect(p[0].orientation_status == OrientationStatus::yaw_only, "yaw-only orientation is accepted");
    expect(std::count_if(p[0].fields.begin(), p[0].fields.end(), [](const RawField& f) { return f.id == 18 || f.id == 21; }) == 2,
           "cached minis 18 and 21 are kept only as raw fields");
    expect(p[1].type_resolution == TypeResolution::unique && p[1].type_candidates.size() == 1
               && p[1].type_candidates[0].logical_name == "Fixture_Marker" && p[1].serialized_object_id == 78U,
           "shuffled minis decode by id, and a second type resolves");
    expect(p[1].position && p[1].position->y == 20.0F && p[1].orientation_degrees && p[1].orientation_degrees->z == 45.0F,
           "shuffled minis keep the authored transform");
    expect(p[2].orientation_status == OrientationStatus::three_axis && p[2].orientation_degrees
               && p[2].orientation_degrees->x == 10.0F && p[2].orientation_degrees->y == 20.0F && p[2].orientation_degrees->z == 30.0F,
           "three-axis Euler is accepted with exact raw degrees (R-ROT-01..03)");
    expect(p[3].orientation_status == OrientationStatus::absent && !p[3].orientation_degrees, "an absent mini 5 is labelled absent");
    expect(p[4].orientation_status == OrientationStatus::nonfinite, "a NaN roll is labelled nonfinite, not three-axis");
    const auto& map = result.value();
    const auto* absent = find_issue(map, MapIssue::orientation_absent);
    expect(absent && absent->record_ordinal == 3U && absent->field_id == 5, "the absent orientation notice names ordinal and mini");
    expect(count_issue(map, MapIssue::orientation_nonfinite) == 1 && count_issue(map, MapIssue::orientation_three_axis) == 0,
           "orientation notices are distinct");
    for (std::uint32_t index = 0; index < 5; ++index) expect(p[index].key.record_ordinal == index, "ordinals follow 1100 order");

    ObjectTypeCatalog collision{{type_ref("Fixture_Prop"), type_ref("fixture_prop")}};
    auto collision_result = load(Fixture{}, collision);
    expect(collision_result && collision_result.value().placements[0].type_resolution == TypeResolution::collision
               && count_issue(collision_result.value(), MapIssue::placement_type_collision) == 1,
           "CRC collisions are never first-wins");
    auto absent_result = load(Fixture{}, ObjectTypeCatalog{});
    expect(absent_result && absent_result.value().placements[0].type_resolution == TypeResolution::missing
               && absent_result.value().placements[0].type_crc == object_type_crc("Fixture_Prop"),
           "missing catalog type remains explicit with its raw CRC");

    // D4: shapes other than exactly one 1113/1200 per 1100 stay unresolved rows.
    Fixture shapes;
    std::vector<std::byte> no_body = chunk(1100, chunk(1108, vector3(0, 0, 0)), true);
    std::vector<std::byte> two_payloads;
    {
        std::vector<std::byte> data; add(data, chunk(1200, placement_body(Record{}, 90))); add(data, chunk(1200, placement_body(Record{}, 91)));
        std::vector<std::byte> object; add(object, chunk(1113, data, true)); two_payloads = chunk(1100, object, true);
    }
    std::vector<std::byte> optional_group;
    {
        // A 1200 under an optional 1134 group is not a second placement.
        std::vector<std::byte> data; add(data, chunk(1200, placement_body(Record{}, 92)));
        std::vector<std::byte> object; add(object, chunk(1113, data, true));
        std::vector<std::byte> extra; add(extra, chunk(1200, placement_body(Record{}, 93)));
        add(object, chunk(1134, extra, true));
        optional_group = chunk(1100, object, true);
    }
    shapes.raw_records = {no_body, two_payloads, optional_group};
    auto shaped = load(shapes);
    expect(bool(shaped), "odd object-graph shapes load");
    if (shaped) {
        const auto& q = shaped.value().placements;
        expect(q.size() == 4, "every 1100 is one placement row, and only 1100 rows count");
        if (q.size() == 4) {
            expect(!q[1].type_crc && q[1].fields.empty() && q[1].key.record_ordinal == 1, "a 1100 without a payload is an unresolved row");
            expect(!q[2].type_crc && q[2].key.record_ordinal == 2, "a 1100 with two payloads chooses neither");
            expect(q[3].serialized_object_id == 92U && q[3].key.record_ordinal == 3, "a 1200 under an optional group is not counted");
        }
        expect(count_issue(shaped.value(), MapIssue::placement_record_shape) == 2, "each unresolved record shape is named");
        const auto resolved = resolve_map(shaped.value(), fixture_catalog(), probe_over({}));
        expect(resolved.placements == 4 && resolved.crc_absent == 2, "unresolved shapes stay in the placement denominator");
    }
}

// --- Reference resolution and per-map resolution accounting -----------------
void resolution() {
    using namespace eawr::assets;
    const auto& catalog = fixture_catalog();
    const auto present = probe_over({
        "data/art/models/fixture_prop.alo",
        "data/art/textures/grass.tga",
        "data/art/textures/fixture_water.dds",
        "data/art/models/dawn_sky.alo",
    });
    expect(resolve_reference(std::nullopt, ReferenceKind::model, present) == ReferenceStatus::absent,
           "an undeclared reference is absent, not unresolved");
    expect(resolve_reference(std::string(), ReferenceKind::model, present) == ReferenceStatus::absent,
           "an empty declared reference is absent");
    expect(resolve_reference(std::string("Fixture_Prop.ALO"), ReferenceKind::model, present) == ReferenceStatus::resolved,
           "a suffixed reference resolves case-insensitively");
    expect(resolve_reference(std::string("fixture_prop"), ReferenceKind::model, present) == ReferenceStatus::resolved,
           "a suffixless model reference resolves through the .alo suffix");
    expect(resolve_reference(std::string("grass"), ReferenceKind::texture, present) == ReferenceStatus::resolved,
           "a suffixless texture reference resolves through the .tga suffix");
    expect(resolve_reference(std::string("grass"), ReferenceKind::model, present) == ReferenceStatus::unresolved,
           "a texture never satisfies a model reference");
    expect(resolve_reference(std::string("W_TempGrnd02.tga"), ReferenceKind::texture,
                             probe_over({"data/art/textures/w_tempgrnd02.dds"})) == ReferenceStatus::resolved,
           "an authored .tga name resolves to the shipped .dds through its stem");
    expect(resolve_reference(std::string("my.odd.name"), ReferenceKind::model,
                             probe_over({"data/art/models/my.odd.name.alo"})) == ReferenceStatus::resolved,
           "an unknown suffix is never truncated away");
    expect(resolve_reference(std::string("absent_asset"), ReferenceKind::model, present) == ReferenceStatus::unresolved,
           "a declared-but-missing reference stays unresolved");
    expect(resolve_reference(std::string("fixture_prop"), ReferenceKind::model, {}) == ReferenceStatus::unresolved,
           "an empty probe never reports a resolved reference");

    expect(find_object_type(catalog, "fixture_prop") != nullptr, "a catalog object id is found case-insensitively");
    expect(find_object_type(catalog, "Fixture_Prop_Missing") == nullptr, "an absent catalog object id is not silently matched");
    expect(find_object_type(catalog, "") == nullptr, "an empty id never matches an object");
    expect(find_object_type({}, "fixture_prop") == nullptr, "an empty catalog matches nothing");

    auto result = load(Fixture{});
    if (result) {
        const auto resolved = resolve_map(result.value(), catalog, present);
        expect(resolved.placements == 1 && resolved.crc_unique == 1, "unique CRC is counted once");
        expect(resolved.crc_absent == 0 && resolved.crc_missing == 0 && resolved.crc_collision == 0,
               "a resolved map reports no other CRC bucket");
        expect(resolved.crc_absent + resolved.crc_missing + resolved.crc_collision + resolved.crc_unique == resolved.placements,
               "CRC buckets sum to the placement denominator");
        expect(resolved.model_chain_renderable + resolved.model_chain_unresolved
                   + resolved.model_chain_undeclared + resolved.model_chain_unknown == resolved.placements,
               "model-chain buckets sum to the placement denominator");
        expect(resolved.model_chain_renderable == 1, "a resolvable model chain is renderable");
        expect(resolved.texture_references == 8 && resolved.textures_resolved == 7,
               "terrain material and water textures share one texture denominator");
        expect(resolved.unresolved_texture_names == std::vector<std::string>{"Fixture_Water_Bump"},
               "an unresolved water texture is named");
        expect(resolved.sky_references == 1 && resolved.skies_resolved == 0, "an unknown sky id stays unresolved");

        const auto missing_model = probe_over({"data/art/textures/grass.tga"});
        const auto degraded = resolve_map(result.value(), catalog, missing_model);
        expect(degraded.crc_unique == 1 && degraded.model_chain_unresolved == 1,
               "a missing model leaves the placement uniquely typed but unrenderable");
        expect(degraded.textures_resolved == 6, "texture resolution is independent of model resolution");
        const auto empty_probe = resolve_map(result.value(), catalog, probe_over({}));
        expect(empty_probe.texture_references == 8 && empty_probe.textures_resolved == 0,
               "unresolved textures stay rows in the denominator");
    }
    Fixture failed_terrain; failed_terrain.plane_delta = -1;
    if (auto degraded = load(failed_terrain)) {
        const auto resolved = resolve_map(degraded.value(), catalog, present);
        expect(resolved.texture_references == 2, "water textures stay declared when the terrain view fails");
    }

    ObjectTypeCatalog untextured{{type_ref("Fixture_Prop")}};
    if (auto undeclared = load(Fixture{}, untextured)) {
        const auto resolved = resolve_map(undeclared.value(), untextured, present);
        expect(resolved.crc_unique == 1 && resolved.model_chain_undeclared == 1,
               "an undeclared model chain is never counted as unrenderable");
    } else expect(false, "a catalog without a model chain still loads the map");
    ObjectTypeCatalog collision{{type_ref("Fixture_Prop"), type_ref("fixture_prop")}};
    if (auto colliding = load(Fixture{}, collision)) {
        const auto resolved = resolve_map(colliding.value(), collision, present);
        expect(resolved.crc_collision == 1 && resolved.model_chain_unknown == 1,
               "a colliding type has unknown, not unrenderable, model renderability");
    }
    if (auto missing = load(Fixture{}, ObjectTypeCatalog{})) {
        const auto resolved = resolve_map(missing.value(), ObjectTypeCatalog{}, present);
        expect(resolved.crc_missing == 1 && resolved.model_chain_unknown == 1,
               "a missing catalog type has unknown model renderability");
    }
}

void environment_reference_ledger() {
    using namespace eawr::assets;
    Fixture fixture;
    std::vector<std::byte> first;
    mini(first, 0x19, narrow("MissingSky"));
    mini(first, 0x1a, narrow("NoModel"));
    mini(first, 0x2f, narrow("MissingCloud"));
    std::vector<std::byte> second;
    mini(second, 0x19, narrow("BadModel"));
    mini(second, 0x1a, narrow("GoodSky"));
    mini(second, 0x2f, narrow("GoodCloud"));
    std::vector<std::byte> third;
    mini(third, 0x19, narrow("GoodSky"));
    mini(third, 0x19, narrow("MissingSky")); // last valid field wins, retaining both raw minis
    mini(third, 0x2f, narrow("")); // declared empty differs from a missing mini
    fixture.environment_records = {first, {std::byte{0x19}}, second, third};

    auto loaded = load(fixture);
    expect(bool(loaded), "multiple synthetic environments load despite an invalid sibling");
    if (!loaded) return;
    const auto& map = loaded.value();
    expect(map.environments.size() == 3 && map.environments[0].record_ordinal == 0
               && map.environments[1].record_ordinal == 2 && map.environments[2].record_ordinal == 3,
           "environment ordinals retain the invalid record's slot");
    if (map.environments.size() != 3) return;
    expect(map.environments[2].fields.size() == 3 && map.environments[2].primary_sky == "MissingSky",
           "duplicate field declarations remain raw while the final valid value is effective");

    ObjectTypeCatalog catalog{{type_ref("NoModel"), type_ref("BadModel", "missing_sky.alo"),
                               type_ref("GoodSky", "good_sky.alo")}};
    const auto probe = probe_over({"data/art/models/fixture_prop.alo", "data/art/models/good_sky.alo",
                                   "data/art/textures/goodcloud.dds"});
    const auto resolved = resolve_map(map, catalog, probe);
    const auto& rows = resolved.environment_references;
    expect(rows.size() == 9, "three rows are emitted per decoded environment, including absent fields");
    if (rows.size() != 9) return;
    const std::array<EnvironmentReferenceStatus, 9> expected{
        EnvironmentReferenceStatus::missing_catalog_object, EnvironmentReferenceStatus::undeclared_model,
        EnvironmentReferenceStatus::unresolved_texture, EnvironmentReferenceStatus::unresolved_model,
        EnvironmentReferenceStatus::resolved, EnvironmentReferenceStatus::resolved,
        EnvironmentReferenceStatus::missing_catalog_object, EnvironmentReferenceStatus::undeclared,
        EnvironmentReferenceStatus::undeclared,
    };
    bool ordered = true;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        ordered = ordered && rows[index].status == expected[index]
            && rows[index].environment_ordinal == (index < 3 ? 0U : index < 6 ? 2U : 3U)
            && rows[index].field == (index % 3 == 0 ? EnvironmentReferenceField::primary_sky
                                     : index % 3 == 1 ? EnvironmentReferenceField::secondary_sky
                                                      : EnvironmentReferenceField::cloud_texture)
            && rows[index].field_id == (index % 3 == 0 ? 0x19 : index % 3 == 1 ? 0x1a : 0x2f);
    }
    expect(ordered, "ledger order and field identity remain stable across environments and duplicate names");
    expect(rows[0].reference_name == "MissingSky" && rows[6].reference_name == "MissingSky"
               && rows[0].declaration_byte_offset != rows[6].declaration_byte_offset,
           "duplicate missing sky names remain distinct source records");
    expect(rows[6].declaration_byte_offset == map.environments[2].fields[1].byte_offset,
           "a duplicate field row points to its effective TED declaration");
    expect(!rows[7].reference_name && !rows[7].declaration_byte_offset
               && rows[8].reference_name == "" && rows[8].declaration_byte_offset,
           "undeclared rows distinguish an absent mini from an empty declaration");
    expect(rows[0].catalog_source == std::nullopt && rows[1].catalog_source
               && rows[1].catalog_source->logical_path == "NoModel.xml"
               && rows[3].model_name == "missing_sky.alo" && rows[4].model_name == "good_sky.alo",
           "sky rows retain effective catalog provenance and selected model names");
    expect(resolved.sky_references == 5 && resolved.skies_resolved == 3 && resolved.skies_model_renderable == 1,
           "sky aggregate counts retain their original denominator and meaning");
    expect(resolved.unresolved_sky_ids == std::vector<std::string>{"MissingSky"}
               && resolved.unresolved_model_names == std::vector<std::string>{"missing_sky.alo"},
           "distinct unresolved summary names coexist with duplicate ledger rows");
    expect(resolved.texture_references == 10 && resolved.textures_resolved == 1
               && std::find(resolved.unresolved_texture_names.begin(), resolved.unresolved_texture_names.end(),
                            "MissingCloud") != resolved.unresolved_texture_names.end(),
           "cloud texture rows join the existing texture totals");
    const auto again = resolve_map(map, catalog, probe);
    bool stable = again.environment_references.size() == rows.size();
    for (std::size_t index = 0; stable && index < rows.size(); ++index) {
        stable = again.environment_references[index].environment_ordinal == rows[index].environment_ordinal
            && again.environment_references[index].field == rows[index].field
            && again.environment_references[index].status == rows[index].status
            && again.environment_references[index].declaration_byte_offset == rows[index].declaration_byte_offset;
    }
    expect(stable, "repeated resolution produces the same ordered ledger");

    Fixture empty_model_fixture;
    std::vector<std::byte> empty_model_environment;
    mini(empty_model_environment, 0x19, narrow("EmptyModel"));
    empty_model_fixture.environment_records = {empty_model_environment};
    if (auto empty_model_map = load(empty_model_fixture)) {
        const ObjectTypeCatalog empty_model_catalog{{type_ref("EmptyModel", " ")}};
        const auto empty_model_resolution = resolve_map(empty_model_map.value(), empty_model_catalog, probe);
        expect(empty_model_resolution.environment_references[0].status
                   == EnvironmentReferenceStatus::unresolved_model
                   && empty_model_resolution.unresolved_model_names == std::vector<std::string>{" "},
               "a declared blank sky model remains in the unresolved aggregate summary");
    } else expect(false, "a sky with a blank declared model loads for resolution");
}
} // namespace eawr::tests::map_contracts
