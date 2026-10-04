#include "foc_tag_perturb_support.hpp"

namespace foc_tag_perturb_test_support {


[[nodiscard]] double to_double(const eawr::sim::math::Fixed value) {
    return static_cast<double>(value.raw()) / static_cast<double>(eawr::sim::math::Fixed::scale);
}

[[nodiscard]] double distance(const eawr::sim::math::Vec3& left, const eawr::sim::math::Vec3& right) {
    const double x = to_double(left.x) - to_double(right.x);
    const double y = to_double(left.y) - to_double(right.y);
    const double z = to_double(left.z) - to_double(right.z);
    return std::sqrt(x * x + y * y + z * z);
}


void check_row(Installation& installation, const Baseline& baseline, const Options& options, const bool compare,
    RowResult& result) {
    const auto& row = result.row;
    // The AI takes these constants through VFS bytes, outside DocumentOverrides.
    if (row.type == "constants" && lower(row.tag).starts_with("ai_")) {
        result.verdict = "not checkable";
        result.reason = "read as raw bytes: the AI XML bypasses document overrides";
        return;
    }
    const auto leaf = lower(split(row.tag, '/').back());
    const bool membership = leaf == "behavior" || leaf == "spacebehavior" || leaf == "attributes"
        || leaf == "categorymask" || leaf == "property_flags" || leaf.ends_with("_categories")
        || leaf.ends_with("_properties") || leaf.ends_with("_restrictions") || leaf.ends_with("_exclusions");
    if (membership && (row.change == "auto" || row.change == "drop")) {
        result.verdict = "not checkable";
        result.reason = "membership list: use drop:<member> or set:<value> to name the tested member";
        return;
    }
    const auto& base_catalog = installation.catalog->catalog;
    std::vector<data::ValueOverride> overrides;
    std::vector<data::DocumentOverride> documents;
    // One object's (or the document's) values: the change it gets, or false when the row is
    // not checkable.
    const auto take = [&](const std::string& id, const Found& found, auto&& add) {
        if (found.values.empty() && row.change.rfind("set:", 0) != 0) {
            result.skipped.push_back(id);
            return true;
        }
        auto changed = change_values(row.change, found.values);
        if (const auto* reason = std::get_if<std::string>(&changed)) {
            result.verdict = "not checkable";
            result.reason = id + ": " + *reason;
            return false;
        }
        auto& made = std::get<Changed>(changed);
        const auto trimmed = [](const std::vector<std::string>& values) {
            std::vector<std::string> result;
            for (const auto& value : values) result.push_back(trim(value));
            return result;
        };
        if (trimmed(made.values) == trimmed(found.values)) {
            result.skipped.push_back(id); // a set: value it already has
            return true;
        }
        ObjectChange object{id, found.values, made.values, false};
        object.traced = found.node != nullptr; // an unauthored tag has no baseline read evidence
        if (found.node != nullptr) {
            const auto& source = found.node->source;
            object.read = installation.read.contains({source.logical_path, source.line, source.column});
            object.traced = !installation.whole.contains(source.logical_path);
        }
        result.objects.push_back(std::move(object));
        add(std::move(made.values));
        return true;
    };
    if (row.type == "constants") {
        // GameConstants: the one document the start, the tables and the AI read it from.
        auto document = data::load_document(*installation.filesystem, constants_path);
        if (!document) {
            result.verdict = "not checkable";
            result.reason = "gameconstants.xml does not load: " + document.error().message;
            return;
        }
        if (row.element.empty() || iequals(document.value().root.name, row.element)) {
            const auto found = find_values(document.value().root, row.tag);
            if (!take(std::string(constants_path), found, [&](std::vector<std::string> values) {
                    documents.push_back({std::string(constants_path), row.tag, std::move(values)});
                })) {
                return;
            }
        }
    } else {
        const auto ids = objects_of(*installation.tables, installation.factions, row.type);
        if (!ids) {
            result.verdict = "not checkable";
            result.reason = "type " + row.type + " has no objects the headless battle builds";
            return;
        }
        for (const auto& id : *ids) {
            auto resolved = base_catalog.resolve(id);
            if (!resolved) continue;
            if (!row.element.empty() && !iequals(resolved.value().type_name, row.element)) continue;
            if (!take(id, find_values(resolved.value(), row.tag), [&](std::vector<std::string> values) {
                    overrides.push_back({id, row.tag, std::move(values)});
                })) {
                return;
            }
        }
    }
    if (overrides.empty() && documents.empty()) {
        result.verdict = "no change";
        result.reason = result.skipped.empty()
            ? "not exercised: the scene has no " + row.type + " of class " + (row.element.empty() ? "*" : row.element)
            : "not exercised: no " + row.type + " in the scene authors or inherits it, or the change is the value it has";
        return;
    }
    // The document change holds for this thread's loads until the row is done.
    const data::DocumentOverrides scope(documents);
    auto catalog = data::with_overrides(base_catalog, overrides);
    if (!catalog) {
        result.verdict = "not checkable";
        result.reason = "the override failed: " + catalog.error().message;
        return;
    }
    auto tables = units::load_unit_tables(installation.input(catalog.value()));
    if (!tables) {
        result.verdict = "not checkable";
        result.reason = "the unit tables do not load: " + tables.error().message;
        return;
    }
    result.tables_changed = tables.value() != *installation.tables;
    const soak::Loaded loaded{*installation.filesystem, catalog.value(), tables.value()};

    const auto& frames = baseline.run.frames;
    auto observe = [&](const std::uint64_t tick, const Frame& units) {
        if (tick >= frames.size()) return;
        const auto& before = frames[tick];
        std::size_t at = 0;
        const auto differ = [&](const tactical::UnitState& unit, const double moved) {
            const auto kind = baseline.kinds.contains(unit.type_id) ? baseline.kinds.at(unit.type_id) : std::string("other");
            auto& delta = result.kinds[kind];
            if (!delta.first_tick) delta.first_tick = tick + 1;
            ++delta.units;
            delta.position = std::max(delta.position, moved);
        };
        for (const auto& unit : units) {
            while (at < before.size() && before[at].entity_id < unit.entity_id) differ(before[at++], 0.0);
            if (at < before.size() && before[at].entity_id == unit.entity_id) {
                const auto& old = before[at++];
                if (!(old == unit)) {
                    differ(unit, distance(old.position, unit.position));
                    const auto kind = baseline.kinds.contains(unit.type_id) ? baseline.kinds.at(unit.type_id) : std::string("other");
                    auto& delta = result.kinds[kind];
                    delta.height = std::max(delta.height, std::abs(to_double(old.position.z) - to_double(unit.position.z)));
                }
            } else {
                differ(unit, 0.0); // a unit the baseline does not have
            }
        }
        while (at < before.size()) differ(before[at++], 0.0);
    };
    auto run = run_battle(loaded, options.seed, 1, options.ticks, false, &baseline.run.hashes, options.after, observe);
    result.ticks_run = run.hashes.size();
    for (std::size_t tick = 0; tick < run.hashes.size() && tick < baseline.run.hashes.size(); ++tick) {
        if (run.hashes[tick] != baseline.run.hashes[tick]) {
            result.first_tick = tick + 1;
            break;
        }
    }
    if (!result.first_tick && run.hashes.size() != baseline.run.hashes.size()) {
        result.first_tick = std::min(run.hashes.size(), baseline.run.hashes.size()) + 1; // one battle ended earlier
    }
    if (run.hashes.empty() && !run.error.empty()) {
        // The change made the data invalid (the battle does not build): that shows the value is
        // validated, not that it is applied. A doubled number gets one gentler try, halved.
        if (row.change == "auto" && !result.retried) {
            RowResult gentler;
            gentler.row = row;
            gentler.row.change = "scale:0.5";
            gentler.retried = true;
            check_row(installation, baseline, options, compare, gentler);
            gentler.row.change = row.change;
            gentler.reason = "doubled, the battle does not build (" + run.error + "); halved: " + gentler.reason;
            result = std::move(gentler);
            return;
        }
        result.verdict = "not checkable";
        result.reason = "the change makes the data invalid, the battle does not build: " + run.error
            + " (give the row a set: hint)";
    } else if (!run.error.empty()) {
        result.verdict = "changes";
        result.reason = "the changed battle fails at a later tick: " + run.error;
    } else if (result.first_tick) {
        result.verdict = "changes";
        result.reason = "the battle differs from completed tick " + std::to_string(*result.first_tick);
    } else {
        result.verdict = "no change";
        const bool read = std::any_of(result.objects.begin(), result.objects.end(),
            [](const auto& object) { return object.read || !object.traced; });
        const bool unauthored = std::any_of(result.objects.begin(), result.objects.end(),
            [](const auto& object) { return object.from.empty(); });
        if (unauthored) {
            result.reason = "not authored; read unknown: no baseline node for one or more changed objects; "
                "the battle never differs in " + std::to_string(result.ticks_run) + " ticks";
        } else if (!read) {
            result.reason = "not read: no loader of the headless battle reads it for these objects";
        } else if (!result.tables_changed) {
            result.reason = std::all_of(result.objects.begin(), result.objects.end(), [](const auto& object) { return object.traced; })
                ? "read and dropped: the full unit tables are identical with the change; other scenario consumers remain unproven"
                : "the unit tables are identical with the change and the battle never differs (its file is read whole: per-tag reads unknown)";
        } else {
            result.reason = "in the unit tables, the battle never differs in " + std::to_string(result.ticks_run)
                + " ticks: not applied, or not exercised by the scenario";
        }
    }
    if (compare && run.error.empty()) {
        auto again = run_battle(loaded, options.seed, options.check_workers, run.hashes.size(), false, nullptr, 0,
            [](std::uint64_t, const Frame&) {});
        if (!again.error.empty() || again.hashes != run.hashes) {
            result.diverged = true;
            std::size_t tick = 0;
            while (tick < again.hashes.size() && tick < run.hashes.size() && again.hashes[tick] == run.hashes[tick]) ++tick;
            result.workers_check = "diverged at " + std::to_string(options.check_workers) + " workers, tick "
                + std::to_string(tick + 1) + (again.error.empty() ? std::string() : ": " + again.error);
        } else {
            result.workers_check = "same at " + std::to_string(options.check_workers) + " workers";
        }
    }
}


} // namespace foc_tag_perturb_test_support
