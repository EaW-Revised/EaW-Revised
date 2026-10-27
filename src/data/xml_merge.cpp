#include "xml_internal.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/data/retail_values.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace eawr::data {

core::Result<TypeMemberFlags> Catalog::member_flags(const std::string_view object_id) const {
    using Result = core::Result<TypeMemberFlags>;
    const auto* definition = find(object_id, Category::game_object);
    if (definition == nullptr) return Result::failure(diagnostic(
        diagnostic_codes::object_not_found, "member flags: unknown game object " + std::string(object_id)));
    const auto& issue = overlay_ ? overlay_->member_flags_issue : impl_->member_flags_issue;
    if (!issue.empty()) return Result::failure(diagnostic(
        diagnostic_codes::member_timeline_unavailable, "member-parse-timeline: " + issue, &definition->root.source));
    const auto& flags = overlay_ ? overlay_->member_flags : impl_->member_flags;
    const auto& indexes = impl_->by_id.at(ascii_lower(object_id));
    for (const auto index : indexes) {
        if (definition_at(index).category == Category::game_object) return Result::success(flags[index]);
    }
    return Result::failure(diagnostic(diagnostic_codes::member_timeline_unavailable,
        "member-parse-timeline: game object has no timeline slot"));
}

const std::vector<MemberRosterNote>& Catalog::member_roster_notes() const noexcept {
    return overlay_ ? overlay_->member_roster_notes : impl_->member_roster_notes;
}

// WGA-363/364, WAR-84: load-only state machine. Preserve copy-time flags and
// rosters rather than closing the final variant ancestry over the final roster.
void derive_member_flags(const std::span<const Definition* const> definitions,
    std::vector<TypeMemberFlags>& flags, std::string& issue, std::vector<MemberRosterNote>& notes) {
    struct State {
        bool complete{};
        std::string base;
        std::array<std::vector<std::string>, 2> rosters;
    };
    flags.assign(definitions.size(), {});
    issue.clear();
    notes.clear();
    std::vector<State> states(definitions.size());
    std::vector<std::size_t> order;
    std::unordered_map<std::string, std::size_t> available;
    constexpr std::array<std::string_view, 2> tags{"Company_Units", "Squadron_Units"};
    const auto authored = [&](const std::size_t index, const bool variant) {
        for (std::size_t roster = 0; roster < tags.size(); ++roster) {
            bool first = true;
            for (const auto& node : definitions[index]->root.children) {
                if (!ascii_iequals(node.name, tags[roster]) || retail::is_placeholder(node.raw_text)) continue;
                tag_trace::used(node);
                std::vector<std::string> names;
                std::string name;
                for (const char ch : node.raw_text) {
                    if (xml_space(static_cast<unsigned char>(ch)) || ch == ',' || ch == '\f' || ch == '\v') {
                        if (!name.empty()) names.push_back(std::exchange(name, {}));
                    } else name += ch;
                }
                if (!name.empty()) names.push_back(std::move(name));
                if (names.empty()) continue;
                if (variant && first) states[index].rosters[roster].clear();
                first = false;
                auto& destination = states[index].rosters[roster];
                destination.insert(destination.end(), names.begin(), names.end());
            }
        }
    };
    // The entry a variant overlays: the catalog's winner for the base name (the strongest layer, then the later registration,
    // as Catalog lookups answer), except that an entry naming ITSELF as its base overlays the nearest earlier entry of that
    // name, and none when there is no earlier one. The catalog still answers duplicates with the last registered entry; the
    // debug build's first-registered rule (RGC-180..187) is the follow-up #3741.
    std::unordered_map<std::string, std::vector<std::size_t>> entries;
    const auto base_of = [&](const std::size_t index) -> std::optional<std::size_t> {
        const auto name = ascii_lower(states[index].base);
        if (name == ascii_lower(definitions[index]->id)) {
            std::optional<std::size_t> earlier;
            if (const auto list = entries.find(name); list != entries.end())
                for (const auto candidate : list->second) if (candidate < index) earlier = candidate;
            return earlier;
        }
        const auto found = available.find(name);
        if (found == available.end()) return std::nullopt;
        return found->second;
    };
    // An entry a stronger layer redefines is shadowed: it still serves as the base of a same-named variant, but its roster no
    // longer marks anything. Duplicates inside one layer are not shadowed; each keeps its own roster marks.
    const auto shadowed = [&](const std::size_t index) {
        const auto winner = available.find(ascii_lower(definitions[index]->id));
        return winner != available.end()
            && definitions[winner->second]->root.source.layer_id != definitions[index]->root.source.layer_id;
    };
    const auto overlay = [&](const std::size_t index) {
        const auto base = base_of(index);
        if (!base || !states[*base].complete) return;
        flags[index] = flags[*base];
        states[index].rosters = states[*base].rosters;
        authored(index, true);
        states[index].complete = true;
    };
    // A variant whose base chain ends at a name no game object carries can never finish: its overlay returns false and the
    // object keeps its own authored tags (CGS-13). Such a variant is not a reason to refuse the whole timeline.
    const auto blocked = [&](std::size_t index) {
        for (std::size_t steps = 0; steps <= definitions.size(); ++steps) {
            if (states[index].base.empty()) return false;
            const auto base = base_of(index);
            if (!base) return true;
            index = *base;
        }
        return false; // a cycle stays incomplete
    };
    // Roster references and base names resolve to the catalog winner of each name: a cross-layer override (the base layer
    // defines X, a stronger layer redefines it in another file) answers with the stronger layer's entry.
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const auto& definition = *definitions[index];
        if (!definition.active || definition.category != Category::game_object) continue;
        if (definition.id.empty()) {
            issue = "empty game-object identity";
            return;
        }
        const auto name = ascii_lower(definition.id);
        entries[name].push_back(index);
        if (definition.namespace_winner) available.insert_or_assign(name, index);
        else available.emplace(name, index);
    }
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const auto& definition = *definitions[index];
        if (!definition.active || definition.category != Category::game_object) continue;
        // A later definition of the same name is a separate entry with its own state, flags and roster marks.
        order.push_back(index);
        if (const auto* base = child_named(definition.root, "Variant_Of_Existing_Type");
            base && !retail::is_placeholder(base->raw_text)) {
            tag_trace::used(*base);
            states[index].base = trim(base->raw_text);
        }
        authored(index, false);
        states[index].complete = states[index].base.empty();
        if (!states[index].complete) overlay(index);
    }
    // Pass zero created types in registry/document order. Later passes replay
    // an entire file for each still-incomplete type from that file, including
    // already-complete variants in it. Copying may overwrite earlier marks.
    for (std::size_t pass = 0; pass < 10; ++pass) {
        if (pass != 0) {
            for (std::size_t start = 0; start < order.size();) {
                std::size_t end = start + 1;
                const auto file = definitions[order[start]]->registry_order;
                while (end < order.size() && definitions[order[end]]->registry_order == file) ++end;
                for (std::size_t candidate = start; candidate < end; ++candidate) {
                    if (states[order[candidate]].complete) continue;
                    for (std::size_t entry = start; entry < end; ++entry)
                        if (!states[order[entry]].base.empty()) overlay(order[entry]);
                }
                start = end;
            }
        }
        // All current rosters participate, including an incomplete variant's
        // authored roster. Writes only set true; the fix-up never clears bits.
        for (const auto index : order) {
            if (shadowed(index)) continue;
            for (std::size_t roster = 0; roster < tags.size(); ++roster) {
                std::size_t slot = 0;
                for (const auto& member : states[index].rosters[roster]) {
                    const auto position = slot++;
                    const auto found = available.find(ascii_lower(member));
                    if (found == available.end()) {
                        // Recovery policy (RGC-154): skip the slot, keep marking the others, report it once.
                        MemberRosterNote note{definitions[index]->id, std::string(tags[roster]), position, member,
                            definitions[index]->root.source.logical_path, definitions[index]->root.source.line};
                        if (std::find(notes.begin(), notes.end(), note) == notes.end()) notes.push_back(std::move(note));
                        continue;
                    }
                    auto& target = flags[found->second];
                    (roster == 0 ? target.ground_company_member : target.squadron_member) = true;
                }
            }
        }
        if (std::all_of(order.begin(), order.end(), [&](const auto index) { return states[index].complete; })) return;
    }
    if (std::all_of(order.begin(), order.end(), [&](const auto index) { return states[index].complete || blocked(index); })) return;
    issue = "variant loading incomplete after ten passes:";
    std::size_t listed = 0;
    for (const auto index : order) {
        if (states[index].complete || blocked(index) || listed++ == 5) continue;
        issue += " " + definitions[index]->id + " (base " + states[index].base + ")";
    }
}

const EffectiveValue* EffectiveObject::value(const std::string_view tag_name) const noexcept {
    const EffectiveValue* result = nullptr;
    for (const auto& item : values) if (ascii_iequals(item.value.name, tag_name)) result = &item;
    if (result != nullptr) tag_trace::used(result->value);
    return result;
}

std::optional<EffectiveValue> EffectiveObject::single(const std::string_view tag_name) const {
    const EffectiveValue* last = nullptr;
    for (const auto& item : values) if (ascii_iequals(item.value.name, tag_name)) last = &item;
    if (last == nullptr) return std::nullopt;
    if (!last->value.children.empty() || !retail::is_placeholder(last->value.raw_text)) {
        tag_trace::used(last->value);
        return *last;
    }
    // The loader read this occurrence to learn it is a placeholder; value() would mark it too.
    tag_trace::used(last->value);
    if (!last->inherited_real) return std::nullopt;
    EffectiveValue result{last->inherited_real->value, ValueProvenance::inherited, last->inherited_real->source_object_id, std::nullopt, std::nullopt};
    tag_trace::used(result.value);
    return result;
}

std::string EffectiveObject::single_text(const std::string_view tag_name) const {
    const auto result = single(tag_name);
    return result ? trim(result->value.raw_text) : std::string{};
}

namespace {

// #628: every definition of a resolved object's variant chain is part of what the loaders touch,
// and its variant link is read, on every resolve (a cached one too).
void trace_chain(const Catalog& catalog, const EffectiveObject& object, const std::optional<Category> category) {
    if (!tag_trace::enabled()) return;
    for (const auto& name : object.chain) {
        const auto* definition = category ? catalog.find(name, *category) : catalog.find(name);
        if (definition == nullptr) continue;
        tag_trace::object(definition->root, object.object_id);
        tag_trace::used(child_named(definition->root, "Variant_Of_Existing_Type"));
    }
}

} // namespace

namespace {

// SG-CLS-01: the element names FoC's game-object files use (lower case, sorted). The retail loader reads no class
// from an element name, so a name outside this list is a mod's own label, not a class.
constexpr std::array<std::string_view, 165> foc_object_classes{
    "absorb_blaster_ability", "ambient_map_sound", "ambientmapsound", "arc_sweep_attack_ability", "attack_action",
    "base_power_ability", "battlefield_modifier_ability", "berserker_ability", "black_market_ability",
    "blast_ability", "bounty_on_faction_ability", "buzz_droids_ability", "cable_ability", "campaign",
    "cin_groundinfantry", "cin_groundprop", "cin_groundstructure", "cin_groundvehicle", "cin_projectile",
    "cin_spaceprop", "cin_spaceunit", "cin_transportunit", "cluster_bomb_ability", "combat_bonus_ability",
    "commandbarcomponent", "concentrate_fire_attack_ability", "container", "corrupt_systems_ability",
    "corruption_ability", "decal", "difficulty_adjustment", "drain_life_ability", "earthquake_attack_ability",
    "eat_attack_ability", "enable_ability", "enable_radar_ability", "energy_weapon_attack_ability",
    "enhance_defense_ability", "event", "faction", "find_weakness_ability", "force_cloak_ability",
    "force_confuse_ability", "force_healing_ability", "force_lightning_ability", "force_sight_ability",
    "force_telekinesis_ability", "force_whirlwind_ability", "galactic_sabotage_ability", "galactic_stealth_ability",
    "galaxy_wide_upgrade_ability", "garrison_upgrade_ability", "generic_attack_ability", "genericherounit",
    "grenade_attack_ability", "groundbase", "groundbuildable", "groundcompany", "groundinfantry", "groundstructure",
    "groundvehicle", "hack_ability", "hack_super_weapon_ability", "hardpoint", "hero_clash",
    "hero_protection_ability", "herocompany", "herounit", "income_stream_ability", "income_stream_mod_ability",
    "indigenous_unit", "infection_ability", "ion_cannon_shot_attack_ability", "item", "landbombingunit",
    "landprimaryskydome", "landsecondaryskydome", "laser_defense_ability", "leech_shields_ability", "lensflare",
    "lightsource", "lucky_shot_attack_ability", "marker", "maximum_firepower_attack_ability", "miscobject",
    "mobile_defense_unit", "mousepointer", "mov_cinematic", "move_action", "movie", "multiplayerstructuremarker",
    "musicevent", "neutralize_hero_ability", "particle", "permanent_weapon_swap_ability",
    "personal_flame_thrower_ability", "planet", "planet_destruction_ability", "planet_income_bonus_ability",
    "planet_income_gambling_ability", "political_control_protection_ability", "political_transition_bonus_ability",
    "priority_set", "projectile", "prop_desert", "prop_felucia", "prop_forest", "props_generic", "props_snow",
    "props_story", "props_swamp", "props_temperate", "props_urban", "props_volcanic", "proximity_mines_ability",
    "radioactive_contaminate_ability", "redirect_blaster_ability", "reduce_production_price_ability",
    "reduce_production_time_ability", "reduce_technology_price_ability", "remote_bomb_ability",
    "remove_corruption_ability", "repair_ability", "retreat_prevention_ability", "saber_throw_ability",
    "scriptmarker", "secondarystructure", "sensor_jamming_ability", "settings_for_faction", "sfxevent",
    "shield_flare_ability", "siphon_credits_ability", "slave_unit", "slavecompany", "slicer_ability",
    "spacebuildable", "spaceprimaryskydome", "spaceprop", "spacesecondaryskydome", "spacestructure", "spaceunit",
    "spawn_ability", "special_ability_action", "specialeffect", "specialstructure", "speechevent", "squadron",
    "starbase", "starbase_upgrade_ability", "stealth_ability", "stun_ability", "summon_ability",
    "super_laser_ability", "system_spy_ability", "tactical_bribe_ability", "tacticalcamera", "techbuilding",
    "tractor_beam_attack_ability", "traderoute", "traderouteline", "transportunit", "uniqueunit", "upgradeobject",
    "vehicle_thief_ability", "weatherproof_ability",
};

// SG-CLS-01: tokens of a flag list tag (`A | B`, `A, B`, `A B`), lower case.
std::vector<std::string> class_tokens(const std::string_view text) {
    std::vector<std::string> result;
    std::string current;
    for (const char c : text) {
        if (c == '|' || c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!current.empty()) result.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) result.push_back(std::move(current));
    for (auto& token : result) token = ascii_lower(token);
    return result;
}

bool has_class_token(const EffectiveObject& object, const std::string_view tag, const std::string_view token) {
    const auto tokens = class_tokens(object.single_text(tag));
    return std::find(tokens.begin(), tokens.end(), std::string(token)) != tokens.end();
}

bool class_flag(const EffectiveObject& object, const std::string_view tag) {
    const auto text = ascii_lower(object.single_text(tag));
    return text == "yes" || text == "true" || text == "1";
}

// The class the effective tags imply for an object with no class named along its chain: the same tags the FoC
// objects of that class carry (a squad lists units, a company lists units, a dummy starbase behavior, an upgrade
// category, a hero flag). Empty when no tag implies one.
std::string class_from_tags(const EffectiveObject& object) {
    const bool hero = class_flag(object, "Is_Named_Hero") || class_flag(object, "Is_Generic_Hero");
    if (object.single("Company_Units")) return hero ? "HeroCompany" : "GroundCompany";
    if (object.single("Squadron_Units")) return "Squadron";
    if (has_class_token(object, "Behavior", "dummy_star_base")) return "StarBase";
    if (has_class_token(object, "CategoryMask", "upgrade")) return "UpgradeObject";
    if (hero) return "HeroUnit";
    return {};
}

} // namespace

bool is_foc_object_class(const std::string_view name) {
    const auto key = ascii_lower(name);
    return std::binary_search(foc_object_classes.begin(), foc_object_classes.end(), std::string_view(key));
}

core::Result<EffectiveObject> Catalog::resolve(const std::string_view object_id) const {
    return resolve_in(object_id, std::nullopt);
}

core::Result<EffectiveObject> Catalog::resolve(const std::string_view object_id, const Category category) const {
    return resolve_in(object_id, category);
}

core::Result<EffectiveObject> Catalog::resolve_in(const std::string_view object_id,
    const std::optional<Category> category) const {
    if (!impl_) return core::Result<EffectiveObject>::failure(diagnostic(
        diagnostic_codes::object_not_found, "catalog is empty"));
    const auto key = (category ? std::string(to_string(*category)) : std::string("untyped")) + ":" + ascii_lower(object_id);
    // An overlay (with_overrides) caches apart from the load it shares.
    auto& cache_mutex = overlay_ ? overlay_->cache_mutex : impl_->cache_mutex;
    auto& resolve_cache = overlay_ ? overlay_->resolve_cache : impl_->resolve_cache;
    std::optional<EffectiveObject> cached;
    {
        const std::scoped_lock lock(cache_mutex);
        const auto found = resolve_cache.find(key);
        if (found != resolve_cache.end()) cached = found->second;
    }
    if (cached) {
        trace_chain(*this, *cached, category);
        return core::Result<EffectiveObject>::success(std::move(*cached));
    }
    const Definition* current = category ? find(object_id, *category) : find(object_id);
    if (current == nullptr) return core::Result<EffectiveObject>::failure(diagnostic(
        diagnostic_codes::object_not_found, "object not found: " + std::string(object_id)));

    std::vector<const Definition*> chain;
    std::vector<std::string> chain_names;
    std::unordered_set<std::string> visited;
    while (current != nullptr) {
        const auto current_key = ascii_lower(current->id);
        if (!visited.insert(current_key).second) {
            chain_names.push_back(current->id);
            return core::Result<EffectiveObject>::failure(diagnostic(
                diagnostic_codes::variant_cycle,
                "variant cycle: " + join_chain(chain_names),
                &current->root.source));
        }
        chain.push_back(current);
        chain_names.push_back(current->id);
        const auto* parent = child_named(current->root, "Variant_Of_Existing_Type");
        if (parent == nullptr || trim(parent->raw_text).empty()) break;
        const auto parent_id = trim(parent->raw_text);
        current = category ? find(parent_id, *category) : find(parent_id);
        if (current == nullptr) {
            chain_names.push_back(parent_id);
            return core::Result<EffectiveObject>::failure(diagnostic(
                diagnostic_codes::variant_missing_base,
                "missing variant base: " + join_chain(chain_names),
                &parent->source));
        }
    }

    struct Group {
        std::string key;
        std::vector<EffectiveValue> occurrences;
    };
    std::vector<Group> groups;
    std::unordered_map<std::string, std::size_t> group_index;
    const auto& top = *chain.front();
    for (auto layer = chain.rbegin(); layer != chain.rend(); ++layer) {
        const auto& definition = **layer;
        for (const auto& tag : definition.root.children) {
            if (ascii_iequals(tag.name, "Variant_Of_Existing_Type")) continue;
            const auto tag_key = ascii_lower(tag.name);
            auto found = group_index.find(tag_key);
            const bool existed = found != group_index.end() && !groups[found->second].occurrences.empty();
            if (tag.merge_mode == MergeMode::ignored && &definition != chain.back()) continue;
            if (found == group_index.end()) {
                found = group_index.emplace(tag_key, groups.size()).first;
                groups.push_back(Group{tag_key, {}});
            }
            auto& occurrences = groups[found->second].occurrences;
            if (tag.merge_mode == MergeMode::merge && tag.multiple_allowed) {
                EffectiveValue value{tag, ValueProvenance::inherited, definition.id, std::nullopt, std::nullopt};
                if (chain.size() == 1) {
                    value.provenance = ValueProvenance::own;
                } else if (&definition == &top) {
                    value.provenance = existed ? ValueProvenance::merged : ValueProvenance::added;
                    if (existed) value.displaced_value = occurrences.front().value;
                }
                occurrences.push_back(std::move(value));
                continue;
            }
            XmlNode effective = tag;
            std::optional<XmlNode> displaced;
            std::optional<InheritedValue> inherited_real;
            if (existed) displaced = occurrences.back().value;
            if (existed && tag.merge_mode != MergeMode::merge && tag.children.empty() && retail::is_placeholder(tag.raw_text)) {
                const auto& previous = occurrences.back();
                if (!previous.value.children.empty() || !retail::is_placeholder(previous.value.raw_text)) {
                    inherited_real = InheritedValue{previous.value, previous.source_object_id};
                } else {
                    inherited_real = previous.inherited_real;
                }
            }
            if (tag.merge_mode == MergeMode::merge && existed) {
                effective.raw_text = token_merge(occurrences.back().value.raw_text, tag.raw_text);
            }
            ValueProvenance provenance = ValueProvenance::inherited;
            if (chain.size() == 1) provenance = ValueProvenance::own;
            else if (&definition == &top) {
                provenance = !existed ? ValueProvenance::added
                    : tag.merge_mode == MergeMode::merge ? ValueProvenance::merged
                    : ValueProvenance::overridden;
            }
            occurrences.assign(1, EffectiveValue{
                std::move(effective), provenance, definition.id,
                provenance == ValueProvenance::overridden || provenance == ValueProvenance::merged
                    ? std::move(displaced) : std::nullopt, std::move(inherited_real)});
        }
    }

    EffectiveObject result{
        .object_id = top.id,
        .type_name = top.type_name,
        .class_origin = {},
        .category = top.category,
        .chain = std::move(chain_names),
        .values = {},
        .catalog_generation = generation(),
    };
    for (auto& group : groups) {
        for (auto& occurrence : group.occurrences) result.values.push_back(std::move(occurrence));
    }
    if (top.category == Category::game_object && !is_foc_object_class(result.type_name)) {
        for (const auto* ancestor : chain) {
            if (!is_foc_object_class(ancestor->type_name)) continue;
            result.type_name = ancestor->type_name;
            result.class_origin = "ancestor:" + ancestor->type_name;
            break;
        }
        if (result.class_origin.empty()) {
            if (auto implied = class_from_tags(result); !implied.empty()) {
                result.class_origin = "tags:" + implied;
                result.type_name = std::move(implied);
            } else {
                result.class_origin = "unclassed";
            }
        }
    }
    {
        const std::scoped_lock lock(cache_mutex);
        resolve_cache.insert_or_assign(key, result);
    }
    trace_chain(*this, result, category);
    return core::Result<EffectiveObject>::success(std::move(result));
}


} // namespace eawr::data
