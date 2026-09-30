#include "space_environment_internal.hpp"

namespace eawr::presentation::godot_backend {

std::shared_ptr<const sim::RenderSnapshot> SpaceEnvironment::State::fog_phase_snapshot(const std::uint8_t step) {
    FogMode& fog = *options.fog;
    if (step == 2) {
        // The labelled presentation override: the selected grid's centre cell
        // is painted 255 (or 0 when its source value is already 255) in
        // FogMode's own buffer; the immutable source grid and every retained
        // snapshot stay untouched.
        const auto* source = fog.source().find(fog.team());
        const std::uint32_t x = source->desc().width / 2;
        const std::uint32_t y = source->desc().height / 2;
        const auto current = source->cell(x, y);
        static_cast<void>(fog.set_painting(true));
        static_cast<void>(fog.paint_cell(x, y, current && *current == 255 ? 0 : 255));
    } else if (step == 3) {
        static_cast<void>(fog.set_painting(false));
    }
    if (fog.stream() != fog_renderer_stream) {
        renderer->reset_fog_stream(fog.stream());
        fog_renderer_stream = fog.stream();
    }
    std::vector<sim::RenderInstance> instances = sky_instances;
    instances.insert(instances.end(), fog_units->instances().begin(), fog_units->instances().end());
    return fog.snapshot(std::move(instances));
}

bool SpaceEnvironment::State::fog_bound() {
    if (!options.fog) return true;
    const auto actual = renderer->fog_status();
    if (actual.ready()) return true;
    fog_status = "failed";
    fog_failure = actual.last_rejection ? core::format_diagnostic(*actual.last_rejection)
                                        : "fog renderer did not bind the selected source grid";
    return false;
}

void SpaceEnvironment::State::evaluate_fog() {
    fog_status = "failed";
    const auto fail = [&](std::string message) { fog_failure = std::move(message); };
    const auto sky = captures.find("configured");
    const auto drawn = captures.find("fog_units");
    if (sky == captures.end() || drawn == captures.end()) return fail("the sky-only or fog unit capture is missing");
    const auto sky_rgb = rgb_of(sky->second);
    const auto unit_rgb = rgb_of(drawn->second);
    if (!sky_rgb || !unit_rgb || sky_rgb->width != unit_rgb->width || sky_rgb->height != unit_rgb->height) {
        return fail("the sky-only and fog unit captures could not be decoded at equal size");
    }
    const auto& units = fog_units->units();
    const sim::fog::FogGrid* grid = options.fog->source().find(options.fog->team());
    std::vector<space::ScreenMask> masks;
    for (const auto& unit : units) masks.push_back(space::rasterize(camera, unit.triangles));
    space::ScreenMask all = masks.front();
    for (std::size_t index = 1; index < masks.size(); ++index) all = space::unite(all, masks[index]);
    const space::ScreenMask grown = space::dilate(all, space::mask_margin);
    const std::uint32_t width = unit_rgb->width;
    const auto difference = [&](const std::size_t pixel) {
        std::uint32_t sum = 0;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const int a = sky_rgb->rgb[pixel * 3 + channel];
            const int b = unit_rgb->rgb[pixel * 3 + channel];
            sum += static_cast<std::uint32_t>(a > b ? a - b : b - a);
        }
        return sum;
    };
    // Everything outside the grown unit masks is sky or clear colour, and the
    // fog phase must leave it untouched. Both frames carry the grid, so this
    // proves the units' draw is confined to their masks, not that the sky is
    // unfogged (see the consumer check below).
    for (std::uint32_t y = 0; y < unit_rgb->height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            if (grown.at(x, y)) continue;
            const std::uint32_t changed = difference(static_cast<std::size_t>(y) * width + x);
            ++fog_outside_pixels;
            if (changed > space::changed_threshold) ++fog_changed_outside;
            if (changed != 0) ++fog_differing_outside;
        }
    }
    fog_evidence.assign(units.size(), {});
    for (std::size_t index = 0; index < units.size(); ++index) {
        FogUnitEvidence& evidence = fog_evidence[index];
        const auto& unit = units[index];
        evidence.potentially_visible = space_fog::can_reveal(grid, unit.bounds);
        evidence.mask_pixels = masks[index].count();
        const space::ScreenMask interior = space::erode(masks[index], 1);
        std::array<double, 3> sum{};
        for (std::uint32_t y = 0; y < unit_rgb->height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                if (!interior.at(x, y)) continue;
                bool shared = false;
                for (std::size_t other = 0; other < masks.size(); ++other) {
                    if (other != index && masks[other].at(x, y)) shared = true;
                }
                if (shared) continue;
                const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
                std::uint32_t level = 0;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    level += unit_rgb->rgb[pixel * 3 + channel];
                    sum[channel] += unit_rgb->rgb[pixel * 3 + channel];
                }
                ++evidence.interior_pixels;
                if (level > space::changed_threshold) ++evidence.lit_interior;
                else ++evidence.dark_interior;
                if (difference(pixel) > space::changed_threshold) ++evidence.changed_interior;
            }
        }
        if (evidence.interior_pixels != 0) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                evidence.mean_rgb[channel] = sum[channel] / static_cast<double>(evidence.interior_pixels);
            }
        }
        evidence.submitted = !unit.entities.empty();
        for (std::size_t surface = 0; surface < unit.entities.size(); ++surface) {
            const bool seen = std::any_of(fog_observed.begin(), fog_observed.end(), [&](const auto& item) {
                return item.entity_id == unit.entities[surface] && item.asset_id == unit.assets[surface];
            });
            if (!seen) evidence.submitted = false;
        }
        evidence.fog_material = !unit.assets.empty();
        for (const sim::AssetId asset : unit.assets) {
            const auto consumer = std::find_if(fog_consumers.begin(), fog_consumers.end(),
                [&](const auto& item) { return item.asset_id == asset; });
            if (consumer == fog_consumers.end() || !consumer->attached || consumer->surfaces == 0
                || consumer->surfaces_with_fog_material != consumer->surfaces) {
                evidence.fog_material = false;
            }
        }
    }
    if (fog_changed_outside != 0) {
        return fail("the fog phase changed " + std::to_string(fog_changed_outside)
                    + " pixels outside the unit masks; the sky is not a fog consumer and must stay unchanged");
    }
    for (std::size_t index = 0; index < units.size(); ++index) {
        const FogUnitEvidence& evidence = fog_evidence[index];
        const std::string identity = "unit placement " + std::to_string(units[index].scene_ordinal) + " "
            + units[index].object_id;
        if (evidence.mask_pixels == 0) return fail(identity + " does not project into the fixed camera frame");
        if (evidence.interior_pixels == 0) return fail(identity + " has no unshared interior pixel to measure");
        if (!evidence.submitted) return fail(identity + " was not in the renderer's fog phase submissions");
        if (!evidence.fog_material) return fail(identity + " does not draw with an attached fog material");
        if (evidence.potentially_visible && evidence.lit_interior == 0) {
            return fail(identity + " intersects a nonzero cell of the selected grid but drew no lit pixel");
        }
        if (!evidence.potentially_visible && evidence.lit_interior != 0) {
            return fail(identity + " is hidden by the selected grid but drew " + std::to_string(evidence.lit_interior)
                        + " lit pixels");
        }
    }
    // The renderer's fog consumers are exactly the unit surfaces: no sky or
    // control asset is declared or attached. With the no-fog baseline's
    // sky-only frame (checked by the harness), this is what shows the sky is
    // unfogged; the outside-mask comparison above cannot, because the
    // sky-only control frame carries the same grid.
    std::size_t unit_assets = 0;
    for (const auto& unit : units) unit_assets += unit.assets.size();
    for (const auto& consumer : fog_consumers) {
        const bool owned = std::any_of(units.begin(), units.end(), [&](const auto& unit) {
            return std::find(unit.assets.begin(), unit.assets.end(), consumer.asset_id) != unit.assets.end();
        });
        if (!owned) {
            return fail("renderer asset " + std::to_string(consumer.asset_id)
                        + " is a fog consumer but not an admitted unit surface");
        }
    }
    if (fog_consumers.size() != unit_assets) {
        return fail("the renderer has " + std::to_string(fog_consumers.size()) + " fog consumers for "
                    + std::to_string(unit_assets) + " admitted unit surfaces");
    }
    // Revision-only upload: the unchanged selected grid, resubmitted by the
    // warmup and timed frames (one retained snapshot) and by the sky evidence
    // phases, is uploaded once and never updated.
    if (fog_phase_uploads["source"] != 1 || fog_phase_updates["source"] != 0) {
        return fail("the unchanged source grid was uploaded " + std::to_string(fog_phase_uploads["source"])
                    + " times and updated " + std::to_string(fog_phase_updates["source"]) + " times");
    }
    if (grid == nullptr || fog_phase_revisions["source"] != grid->revision()) {
        return fail("the fog phase did not bind the selected source revision");
    }
    fog_status = "verified";
}

void SpaceEnvironment::State::write_fog_report(std::ostream& output) const {
    const FogMode& fog = *options.fog;
    const auto* selected = fog.source().find(fog.team());
    const GodotRenderer::FogStatus actual = fog_final ? *fog_final
        : (renderer ? renderer->fog_status() : GodotRenderer::FogStatus{});
    const char* readiness = "disabled";
    switch (actual.readiness) {
    case GodotRenderer::FogReadiness::disabled: readiness = "disabled"; break;
    case GodotRenderer::FogReadiness::awaiting_grid: readiness = "awaiting_grid"; break;
    case GodotRenderer::FogReadiness::ready: readiness = "ready"; break;
    case GodotRenderer::FogReadiness::rejected: readiness = "rejected"; break;
    }
    const auto strings = [](const std::vector<std::string>& values) {
        std::string text = "[";
        for (std::size_t index = 0; index < values.size(); ++index) text += (index == 0 ? "" : ", ") + json(values[index]);
        return text + "]";
    };
    output << "  \"fog\": {\"slice\": \"eawr-space-fog-synthetic-v1\""
        << ", \"policy\": \"opt-in synthetic fog-stub-v1 harness: caller-declared admission and a pinned grid; not "
           "retail space fog, attenuation, soft edge or unit-hiding behaviour\""
        << ",\n    \"source\": [";
    for (std::size_t index = 0; index < fog.sources().size(); ++index) {
        const auto& source = fog.sources()[index];
        output << (index ? ", " : "") << "{\"path\": " << json(source.path.generic_string())
            << ", \"sha256\": " << json(source.sha256)
            << ", \"team\": " << source.team << ", \"revision\": " << source.revision << '}';
    }
    output << "], \"team\": " << fog.team() << ", \"source_revision\": " << (selected ? selected->revision() : 0)
        << ", \"grid\": ";
    if (selected) {
        const auto& desc = selected->desc();
        output << "{\"width\": " << desc.width << ", \"height\": " << desc.height
            << ", \"origin_raw\": [" << desc.origin_x_raw << ", " << desc.origin_y_raw
            << "], \"cell_raw\": [" << desc.cell_x_raw << ", " << desc.cell_y_raw << "]}";
    } else {
        output << "null";
    }
    output << ", \"bound_revision\": ";
    if (actual.bound_revision) output << *actual.bound_revision; else output << "null";
    output << ", \"stream\": " << fog.stream() << ", \"snapshot_tick\": " << fog.tick()
        << ", \"mapping\": \"source_xy_from_world_x_negative_z\", \"filter\": \"nearest\", \"outside_grid\": \"dark\""
        << ", \"fixed_capture\": " << (fog.fixed_capture() ? "true" : "false")
        << ", \"override\": " << (fog.override_active() ? "true" : "false")
        << ",\n    \"sky\": {\"fog\": \"excluded\", \"consumer\": false, \"cause\": \"no source contract maps a "
           "world-XY grid onto a sky seen from inside; the sky adapters are not declared fog consumers, the renderer's "
           "fog consumers must be exactly the admitted unit surfaces and the fog phase must leave every pixel "
           "outside the unit masks unchanged\"}"
        << ", \"environment\": {\"water\": \"not_applicable: a kind-2 map has no water record and no water plane is "
           "manufactured\", \"nebula_planet\": \"classified_not_rendered: the environment path draws no surface "
           "besides the sky, so it declares no fog consumer; a map placement of any catalog type the caller admits "
           "is composed as a fog-consuming unit whatever it depicts, since admission does not classify\"}"
        << ",\n    \"admission\": {\"types\": " << strings(options.fog_admit)
        << ", \"rule\": \"compose only placements whose catalog XML element type is declared; nothing is admitted by "
           "default and nothing is inferred. Harness policy for #28 evidence; #32 must approve any real space "
           "placement classification\", \"placements\": [";
    if (fog_units) {
        const auto& decisions = fog_units->decisions();
        for (std::size_t index = 0; index < decisions.size(); ++index) {
            const auto& decision = decisions[index];
            output << (index ? ", " : "") << "{\"scene_ordinal\": " << decision.scene_ordinal
                << ", \"object_id\": " << json(decision.object_id) << ", \"type\": " << json(decision.type_name)
                << ", \"decision\": " << json(space_fog::to_string(decision.decision))
                << ", \"reasons\": " << strings(decision.reasons) << '}';
        }
    }
    output << "]},\n    \"units\": [";
    if (fog_units) {
        const auto& units = fog_units->units();
        for (std::size_t index = 0; index < units.size(); ++index) {
            const auto& unit = units[index];
            output << (index ? ",\n      " : "\n      ") << "{\"scene_ordinal\": " << unit.scene_ordinal
                << ", \"object_id\": " << json(unit.object_id) << ", \"type\": " << json(unit.type_name)
                << ", \"assets\": [";
            for (std::size_t item = 0; item < unit.assets.size(); ++item) output << (item ? ", " : "") << unit.assets[item];
            output << "], \"entities\": [";
            for (std::size_t item = 0; item < unit.entities.size(); ++item) {
                output << (item ? ", " : "") << unit.entities[item];
            }
            output << "], \"source_bounds\": {\"x\": [" << number(unit.bounds.min_x) << ", " << number(unit.bounds.max_x)
                << "], \"y\": [" << number(unit.bounds.min_y) << ", " << number(unit.bounds.max_y) << "]}";
            if (index < fog_evidence.size()) {
                const FogUnitEvidence& evidence = fog_evidence[index];
                output << ", \"potentially_visible\": " << (evidence.potentially_visible ? "true" : "false")
                    << ", \"mask_pixels\": " << evidence.mask_pixels
                    << ", \"interior_pixels\": " << evidence.interior_pixels
                    << ", \"lit_interior\": " << evidence.lit_interior
                    << ", \"dark_interior\": " << evidence.dark_interior
                    << ", \"changed_from_sky\": " << evidence.changed_interior
                    << ", \"mean_rgb\": [" << number(evidence.mean_rgb[0]) << ", " << number(evidence.mean_rgb[1])
                    << ", " << number(evidence.mean_rgb[2]) << "]"
                    << ", \"submitted\": " << (evidence.submitted ? "true" : "false")
                    << ", \"fog_material_attached\": " << (evidence.fog_material ? "true" : "false");
            }
            output << '}';
        }
    }
    output << (fog_units && !fog_units->units().empty() ? "\n    " : "") << "],\n    \"unsupported\": "
        << strings(fog_units ? fog_units->unsupported() : std::vector<std::string>{})
        << ",\n    \"renderer\": {\"readiness\": " << json(readiness) << ", \"submitted_tick\": ";
    if (actual.submitted_tick) output << *actual.submitted_tick; else output << "null";
    output << ", \"attached_consumers\": " << actual.attached_consumers
        << ", \"declared_consumers\": " << actual.declared_consumers
        << ", \"external_consumers\": " << actual.external_consumers
        << ", \"live_textures\": " << actual.live_textures
        << ", \"uploads\": " << actual.cache.uploads << ", \"upload_bytes\": " << actual.cache.upload_bytes
        << ", \"creates\": " << actual.cache.creates << ", \"updates\": " << actual.cache.updates
        << ", \"binds\": " << actual.cache.binds << ", \"rejected\": " << actual.cache.rejected
        << ", \"last_action\": "
        << json(actual.last_action ? std::string(presentation::fog::to_string(*actual.last_action)) : "")
        << ", \"last_rejection\": "
        << json(actual.last_rejection ? core::format_diagnostic(*actual.last_rejection) : "") << '}'
        << ",\n    \"evidence\": {\"status\": " << json(fog_status) << ", \"failure\": " << json(fog_failure)
        << ", \"control_capture\": \"configured (the same frame with the sky only)\""
        << ", \"changed_threshold\": " << space::changed_threshold << ", \"mask_margin\": " << space::mask_margin
        << ", \"outside_pixels\": " << fog_outside_pixels << ", \"changed_outside\": " << fog_changed_outside
        << ", \"differing_outside\": " << fog_differing_outside << ", \"phases\": {";
    bool first = true;
    for (const auto& [label, hash] : fog_phase_hashes) {
        const auto count = [&](const std::map<std::string, std::uint64_t>& values) {
            const auto found = values.find(label);
            return found == values.end() ? std::uint64_t{0} : found->second;
        };
        output << (first ? "" : ", ") << json(label) << ": {\"capture_sha256\": " << json(hash)
            << ", \"uploads\": " << count(fog_phase_uploads) << ", \"updates\": " << count(fog_phase_updates)
            << ", \"bound_revision\": " << count(fog_phase_revisions) << '}';
        first = false;
    }
    output << "}, \"paint_evidence\": " << json(options.fog_paint_evidence.generic_string()) << "}"
        << ",\n    \"map_sha256\": " << json(options.map_sha256)
        << ", \"scene_sha256\": " << json(fog_units && fog_units->scene() ? fog_units->scene()->scene_sha256 : "")
        << "},\n";
}

} // namespace eawr::presentation::godot_backend
