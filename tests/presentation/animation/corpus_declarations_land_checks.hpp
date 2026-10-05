#pragma once

// Evaluates every pinned pair against a loaded catalog.  `catalog` is null
// when load_catalog failed; every pair is then catalog_unresolved.
[[nodiscard]] inline Audit evaluate(const PinnedPairs& pinned, const data::Catalog* catalog,
    const std::vector<core::Diagnostic>& diagnostics, const std::string& load_error, const SourceHash& source_hash) {
    Audit audit;
    audit.catalog.loaded = catalog != nullptr;
    audit.catalog.load_error = load_error;
    for (const auto& diagnostic : diagnostics) ++audit.catalog.diagnostics_by_code[diagnostic.code];
    for (const PinnedPair& pair : pinned.pairs) {
        PairResult result;
        result.pair = pair;
        result.animation_set = pair.selected_model;
        audit.pairs.push_back(std::move(result));
    }
    std::sort(audit.pairs.begin(), audit.pairs.end(),
        [](const PairResult& left, const PairResult& right) { return left.pair.animation.path < right.pair.animation.path; });

    // Indexes from canonical model paths and mention tokens to pairs.
    std::map<std::string, std::vector<std::size_t>> by_candidate;
    std::map<std::string, std::vector<std::size_t>> by_set;
    std::map<std::string, std::vector<std::size_t>> by_name;
    for (std::size_t index = 0; index < audit.pairs.size(); ++index) {
        const PinnedPair& pair = audit.pairs[index].pair;
        by_candidate[pair.candidate.path].push_back(index);
        by_set[pair.selected_model].push_back(index);
        const std::string clip = path_stem(pair.animation.path);
        for (const std::string& name : {clip, clip + ".ala", path_stem(pair.selected_model)}) {
            auto& list = by_name[name];
            if (list.empty() || list.back() != index) list.push_back(index);
        }
    }
    const auto pairs_naming = [&](const std::string& canonical) {
        std::set<std::size_t> result;
        if (const auto found = by_candidate.find(canonical); found != by_candidate.end())
            result.insert(found->second.begin(), found->second.end());
        if (const auto found = by_set.find(canonical); found != by_set.end())
            result.insert(found->second.begin(), found->second.end());
        return result;
    };

    if (catalog != nullptr) {
        audit.catalog.profile = std::string(data::to_string(catalog->profile()));
        audit.catalog.definitions = catalog->definitions().size();
        audit.catalog.registry_files = catalog->registry_files().size();
        for (const auto& file : catalog->registry_files()) audit.catalog.registry_files_loaded += file.loaded ? 1U : 0U;

        // One pass over object IDs, in a stable order.
        std::map<std::string, std::vector<const data::Definition*>> by_id;
        for (const auto& definition : catalog->definitions())
            if (!definition.id.empty()) by_id[fold(definition.id)].push_back(&definition);
        audit.catalog.objects = by_id.size();

        for (const auto& [key, definitions] : by_id) {
            const data::Definition* winner = catalog->find(key);
            if (winner == nullptr) continue;

            // Shadowed definitions: recorded, never evidence.
            for (const data::Definition* definition : definitions) {
                if (definition == winner) continue;
                for (const auto& [tag, nodes] : detail::own_link_values(*definition)) {
                    for (const data::XmlNode* node : nodes) {
                        for (const std::size_t index : pairs_naming(canonical_model_path(node->raw_text))) {
                            audit.pairs[index].observations.push_back(Observation{
                                std::string(observation::shadowed_definition), winner->id, false,
                                detail::site_of(*node, "shadowed", definition->id, source_hash)});
                        }
                    }
                }
            }

            auto resolved = catalog->resolve(winner->id);
            if (!resolved) {
                const auto& error = resolved.error();
                ++audit.catalog.unresolved_by_code[error.code];
                // Unresolved objects that name a pinned model or set anywhere
                // on the part of their chain that exists.
                std::map<std::size_t, std::vector<ValueSite>> touched;
                for (const data::Definition* definition : detail::partial_chain(*catalog, *winner)) {
                    for (const auto& [tag, nodes] : detail::own_link_values(*definition)) {
                        for (const data::XmlNode* node : nodes) {
                            for (const std::size_t index : pairs_naming(canonical_model_path(node->raw_text)))
                                touched[index].push_back(detail::site_of(*node, "unresolved", definition->id, source_hash));
                        }
                    }
                }
                for (auto& [index, sites] : touched)
                    audit.pairs[index].unresolved.push_back(Unresolved{winner->id, error.code, error.message, std::move(sites)});
                continue;
            }
            ++audit.catalog.resolved;
            const data::EffectiveObject& object = resolved.value();

            // The link itself, from effective (inherited, winning) values.
            const data::EffectiveValue* model = detail::land_model(object);
            const data::EffectiveValue* set = object.value(override_tag);
            const std::string model_path = model != nullptr ? canonical_model_path(model->value.raw_text) : std::string{};
            const std::string set_path = set != nullptr ? canonical_model_path(set->value.raw_text) : std::string{};
            const auto link_for = [&](const std::string& candidate_path, const std::string& set_candidate) {
                std::set<std::size_t> result;
                if (candidate_path.empty() || set_candidate.empty()) return result;
                if (const auto found = by_candidate.find(candidate_path); found != by_candidate.end())
                    for (const std::size_t index : found->second)
                        if (audit.pairs[index].pair.selected_model == set_candidate) result.insert(index);
                return result;
            };
            const std::set<std::size_t> linked = link_for(model_path, set_path);
            for (const std::size_t index : linked) {
                audit.pairs[index].evidence.push_back(Evidence{object.object_id, object.type_name,
                    std::string(data::to_string(object.category)), object.chain,
                    detail::site_of(model->value, to_string(model->provenance), model->source_object_id, source_hash),
                    detail::site_of(set->value, to_string(set->provenance), set->source_object_id, source_hash)});
            }

            // Equally authoritative alternatives that would change the link:
            // (a) one definition of the chain declaring a land-model or
            // override tag more than once with different values (the resolver
            // keeps the last), and (b) a same-layer, same-file duplicate of the
            // winner or of a variant base that, resolved with its own
            // inheritance, links where the winner does not, or the reverse.
            // Either makes the pair conflicting, never evidence.
            std::map<std::size_t, std::vector<Conflict>> conflicts;
            for (const std::string& link : object.chain) {
                const data::Definition* definition = catalog->find(link);
                if (definition == nullptr) continue;
                std::set<std::string> models{model_path};
                std::set<std::string> sets{set_path};
                std::vector<const data::XmlNode*> ambiguous;
                for (const auto& [tag, nodes] : detail::own_link_values(*definition)) {
                    const bool land = iequals(tag, "Land_Model_Name") || iequals(tag, "Model_Name");
                    if (!land && !iequals(tag, override_tag)) continue;
                    std::set<std::string> values;
                    for (const data::XmlNode* node : nodes) values.insert(canonical_model_path(node->raw_text));
                    if (values.size() < 2) continue;
                    (land ? models : sets).insert(values.begin(), values.end());
                    ambiguous.insert(ambiguous.end(), nodes.begin(), nodes.end());
                }
                if (ambiguous.empty()) continue;
                std::set<std::size_t> possible;
                for (const std::string& candidate_path : models)
                    for (const std::string& set_candidate : sets)
                        for (const std::size_t index : link_for(candidate_path, set_candidate)) possible.insert(index);
                for (const std::size_t index : possible) {
                    Conflict conflict{object.object_id, "definition " + definition->id
                        + " declares a link tag more than once with different values", {}};
                    for (const data::XmlNode* node : ambiguous)
                        conflict.sites.push_back(detail::site_of(*node, "own", definition->id, source_hash));
                    conflicts[index].push_back(std::move(conflict));
                }
            }
            // (b) Same-layer, same-file duplicates, of the object or of any
            // variant base on its way, are each resolved with their own
            // values over their own inheritance.  A way of resolving that
            // links where the winner does not, or the reverse, is a conflict;
            // one that cannot be followed conflicts with every pair its chain
            // could link, and with the winner's own link.
            detail::LinkOutcomes ways;
            {
                std::vector<std::string> visiting;
                const auto tops = detail::equally_authoritative(*catalog, winner->id);
                for (const data::Definition* top : tops) {
                    for (detail::LinkOutcome outcome : detail::link_outcomes(*catalog, *top, visiting, ways)) {
                        outcome.alternative = outcome.alternative || top != winner;
                        ways.outcomes.push_back(std::move(outcome));
                    }
                }
            }
            // The winners-only walk must reproduce the resolver, or none of
            // the walks is trusted.
            const auto winner_way = std::find_if(ways.outcomes.begin(), ways.outcomes.end(),
                [](const detail::LinkOutcome& outcome) { return !outcome.alternative; });
            if (winner_way == ways.outcomes.end()
                || detail::outcome_link(*winner_way) != std::pair{model_path, set_path})
                ways.undetermined.insert("the winners-only walk of " + object.object_id + " disagrees with the resolver");
            const auto effective_sites = [&](std::vector<ValueSite>& sites) {
                if (model != nullptr)
                    sites.push_back(detail::site_of(
                        model->value, to_string(model->provenance), model->source_object_id, source_hash));
                if (set != nullptr)
                    sites.push_back(
                        detail::site_of(set->value, to_string(set->provenance), set->source_object_id, source_hash));
            };
            std::map<std::size_t, std::vector<ValueSite>> disagreeing;
            for (const detail::LinkOutcome& outcome : ways.outcomes) {
                if (!outcome.alternative) continue;
                const auto [other_model, other_set] = detail::outcome_link(outcome);
                const auto other = link_for(other_model, other_set);
                std::set<std::size_t> differing;
                std::set_symmetric_difference(linked.begin(), linked.end(), other.begin(), other.end(),
                    std::inserter(differing, differing.end()));
                for (const std::size_t index : differing) {
                    auto& sites = disagreeing[index];
                    for (const detail::LinkSource* source : {&outcome.land, &outcome.model, &outcome.set})
                        if (source->node != nullptr)
                            sites.push_back(detail::site_of(*source->node, "alternative", source->definition_id, source_hash));
                }
            }
            for (auto& [index, sites] : disagreeing) {
                effective_sites(sites);
                conflicts[index].push_back(Conflict{object.object_id, "equally authoritative same-layer, same-file "
                    "definitions in the variant chain of " + object.object_id + " disagree on the link", std::move(sites)});
            }
            if (!ways.undetermined.empty()) {
                std::set<std::size_t> candidates;
                std::set<std::size_t> sets;
                for (const detail::LinkSource& source : ways.reachable) {
                    const std::string canonical = canonical_model_path(source.node->raw_text);
                    const bool is_set = iequals(source.node->name, override_tag);
                    auto& target = is_set ? sets : candidates;
                    const auto& named = is_set ? by_set : by_candidate;
                    if (const auto found = named.find(canonical); found != named.end())
                        target.insert(found->second.begin(), found->second.end());
                }
                std::set<std::size_t> possible = linked;
                std::set_intersection(candidates.begin(), candidates.end(), sets.begin(), sets.end(),
                    std::inserter(possible, possible.end()));
                std::string reason = "the link of " + object.object_id + " cannot be determined for every equally "
                    "authoritative definition:";
                for (const std::string& item : ways.undetermined) reason += " " + item + ";";
                for (const std::size_t index : possible) {
                    Conflict conflict{object.object_id, reason, {}};
                    const PinnedPair& pinned_pair = audit.pairs[index].pair;
                    for (const detail::LinkSource& source : ways.reachable) {
                        const std::string canonical = canonical_model_path(source.node->raw_text);
                        if (canonical == pinned_pair.candidate.path || canonical == pinned_pair.selected_model)
                            conflict.sites.push_back(
                                detail::site_of(*source.node, "undetermined", source.definition_id, source_hash));
                    }
                    effective_sites(conflict.sites);
                    conflicts[index].push_back(std::move(conflict));
                }
            }
            for (auto& [index, list] : conflicts)
                for (auto& conflict : list) audit.pairs[index].conflicts.push_back(std::move(conflict));

            // Non-promoting observations from the effective values.
            for (const data::EffectiveValue& value : object.values) {
                if (!is_link_field(value.value.name)) continue;
                const std::string canonical = canonical_model_path(value.value.raw_text);
                const bool is_override = iequals(value.value.name, override_tag);
                if (!is_override) {
                    if (const auto found = by_candidate.find(canonical); found != by_candidate.end()) {
                        for (const std::size_t index : found->second) {
                            if (linked.contains(index)) continue;
                            const bool other_set = &value == model && set != nullptr;
                            audit.pairs[index].observations.push_back(Observation{
                                std::string(other_set ? observation::override_other_set : observation::model_only),
                                object.object_id, true,
                                detail::site_of(value.value, to_string(value.provenance), value.source_object_id, source_hash)});
                        }
                    }
                } else if (const auto found = by_set.find(canonical); found != by_set.end()) {
                    for (const std::size_t index : found->second) {
                        if (linked.contains(index)) continue;
                        audit.pairs[index].observations.push_back(Observation{std::string(observation::set_on_other_model),
                            object.object_id, true,
                            detail::site_of(value.value, to_string(value.provenance), value.source_object_id, source_hash)});
                    }
                }
            }
            for (const data::EffectiveValue& value : object.values) {
                detail::scan_mentions(value.value, object.object_id, std::string(to_string(value.provenance)),
                    value.source_object_id, by_name, audit.pairs, source_hash, true);
            }
        }
    }

    for (PairResult& result : audit.pairs) {
        std::sort(result.evidence.begin(), result.evidence.end(), [](const Evidence& left, const Evidence& right) {
            return std::tie(left.object_id, left.model.source.logical_path, left.model.source.line)
                < std::tie(right.object_id, right.model.source.logical_path, right.model.source.line);
        });
        std::sort(result.conflicts.begin(), result.conflicts.end(), [](const Conflict& left, const Conflict& right) {
            return std::tie(left.object_id, left.reason) < std::tie(right.object_id, right.reason);
        });
        for (Conflict& conflict : result.conflicts) {
            std::sort(conflict.sites.begin(), conflict.sites.end(), detail::site_less);
            conflict.sites.erase(std::unique(conflict.sites.begin(), conflict.sites.end(),
                                     [](const ValueSite& left, const ValueSite& right) {
                                         return detail::site_key(left) == detail::site_key(right)
                                             && left.provenance == right.provenance
                                             && left.source_object_id == right.source_object_id;
                                     }),
                conflict.sites.end());
        }
        std::sort(result.unresolved.begin(), result.unresolved.end(),
            [](const Unresolved& left, const Unresolved& right) { return left.object_id < right.object_id; });
        for (Unresolved& unresolved : result.unresolved)
            std::sort(unresolved.sites.begin(), unresolved.sites.end(), detail::site_less);
        std::sort(result.observations.begin(), result.observations.end(), [](const Observation& left, const Observation& right) {
            return std::tuple_cat(std::tie(left.kind, left.object_id), detail::site_key(left.site))
                < std::tuple_cat(std::tie(right.kind, right.object_id), detail::site_key(right.site));
        });
        // Precedence: a conflict or a gap in the catalog is never resolved in
        // favour of evidence.
        if (catalog == nullptr) result.disposition = disposition::catalog_unresolved;
        else if (!result.conflicts.empty()) result.disposition = disposition::conflicting_evidence;
        else if (!result.unresolved.empty()) result.disposition = disposition::catalog_unresolved;
        else if (!result.evidence.empty()) result.disposition = disposition::evidence_bearing;
        else result.disposition = disposition::no_explicit_evidence;
        ++audit.dispositions[result.disposition];
        audit.evidence_records += result.evidence.size();
        audit.observation_records += result.observations.size();
    }
    return audit;
}
