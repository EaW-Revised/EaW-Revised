#pragma once

#include "corpus_declarations_land_checks.hpp"

// ---------------------------------------------------------------------------
// Receipt.
// ---------------------------------------------------------------------------

struct Header final {
    std::string pairs_sha256;
    std::string receipt_sha256;
    std::string frozen_metadata_sha256;
    std::size_t animation_count{};
    std::size_t playback_passed{};
    std::size_t failure_count{};
    std::string compiler;
    std::string config;
    std::vector<std::pair<std::string, std::string>> tool_sources;
    std::vector<std::pair<std::string, std::string>> mounts;
};

namespace detail {

inline void write_identity(std::ostream& output, const AssetIdentity& identity) {
    output << "{\"path\": " << json_string(identity.path) << ", \"sha256\": " << json_string(identity.sha256)
           << ", \"layer_id\": " << json_string(identity.layer_id) << ", \"origin\": " << json_string(identity.origin)
           << ", \"source_id\": " << json_string(identity.source_id)
           << ", \"original_path\": " << json_string(identity.original_path) << ", \"size\": " << identity.size << '}';
}

inline void write_site(std::ostream& output, const ValueSite& site) {
    output << "{\"tag\": " << json_string(site.tag) << ", \"declared\": " << json_string(site.declared)
           << ", \"canonical\": " << json_string(site.canonical) << ", \"provenance\": " << json_string(site.provenance)
           << ", \"source_object_id\": " << json_string(site.source_object_id)
           << ", \"source\": {\"logical_path\": " << json_string(site.source.logical_path)
           << ", \"source_id\": " << json_string(site.source.source_id)
           << ", \"layer_id\": " << json_string(site.source.layer_id) << ", \"line\": " << site.source.line
           << ", \"column\": " << site.source.column << ", \"sha256\": " << json_string(site.source_sha256) << "}}";
}

inline void write_counts(std::ostream& output, const std::map<std::string, std::size_t>& counts) {
    output << '{';
    bool first = true;
    for (const auto& [key, count] : counts) {
        output << (first ? "" : ", ") << json_string(key) << ": " << count;
        first = false;
    }
    output << '}';
}

template <typename T, typename Write>
inline void write_list(std::ostream& output, const std::vector<T>& items, Write write) {
    output << '[';
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) output << ", ";
        write(items[index]);
    }
    output << ']';
}

} // namespace detail

inline void write_audit(std::ostream& output, const Header& header, const Audit& audit) {
    output << "{\n  \"schema\": " << json_string(schema) << ",\n  \"schema_version\": " << schema_version
           << ",\n  \"profile\": \"remake-effective\",\n  \"link_rule\": " << json_string(link_rule)
           << ",\n  \"association_approved\": false,\n  \"associations_promoted\": 0,\n  \"corpus_acceptance_claimed\": false"
           << ",\n  \"baseline\": {\"animation_count\": " << header.animation_count << ", \"playback_passed\": "
           << header.playback_passed << ", \"failure_count\": " << header.failure_count << '}'
           << ",\n  \"inputs\": {\"pairs_sha256\": " << json_string(header.pairs_sha256)
           << ", \"association_receipt_sha256\": " << json_string(header.receipt_sha256)
           << ", \"frozen_metadata_sha256\": " << json_string(header.frozen_metadata_sha256) << '}'
           << ",\n  \"tool\": {\"name\": \"animation_corpus_declarations\", \"compiler\": " << json_string(header.compiler)
           << ", \"config\": " << json_string(header.config) << ", \"source_sha256\": {";
    for (std::size_t index = 0; index < header.tool_sources.size(); ++index)
        output << (index == 0 ? "" : ", ") << json_string(header.tool_sources[index].first) << ": "
               << json_string(header.tool_sources[index].second);
    output << "}},\n  \"mounts\": [";
    for (std::size_t index = 0; index < header.mounts.size(); ++index)
        output << (index == 0 ? "" : ", ") << "{\"layer_id\": " << json_string(header.mounts[index].first)
               << ", \"manifest_source_id\": " << json_string(header.mounts[index].second) << '}';
    const CatalogSummary& catalog = audit.catalog;
    output << "],\n  \"catalog\": {\"loaded\": " << (catalog.loaded ? "true" : "false")
           << ", \"load_error\": " << json_string(catalog.load_error) << ", \"profile\": " << json_string(catalog.profile)
           << ", \"definitions\": " << catalog.definitions << ", \"objects\": " << catalog.objects
           << ", \"resolved\": " << catalog.resolved << ", \"unresolved_by_code\": ";
    detail::write_counts(output, catalog.unresolved_by_code);
    output << ", \"diagnostics_by_code\": ";
    detail::write_counts(output, catalog.diagnostics_by_code);
    output << ", \"registry_files\": " << catalog.registry_files
           << ", \"registry_files_loaded\": " << catalog.registry_files_loaded << "},\n  \"counts\": {\"pairs\": "
           << audit.pairs.size() << ", \"dispositions\": ";
    std::map<std::string, std::size_t> dispositions{{std::string(disposition::evidence_bearing), 0},
        {std::string(disposition::no_explicit_evidence), 0}, {std::string(disposition::catalog_unresolved), 0},
        {std::string(disposition::conflicting_evidence), 0}};
    for (const auto& [key, count] : audit.dispositions) dispositions[key] = count;
    detail::write_counts(output, dispositions);
    std::map<std::string, std::size_t> observations;
    std::size_t priority{};
    for (const PairResult& result : audit.pairs) {
        priority += result.pair.priority ? 1U : 0U;
        for (const Observation& item : result.observations) ++observations[item.kind];
    }
    output << ", \"priority_pairs\": " << priority << ", \"evidence_records\": " << audit.evidence_records
           << ", \"observations_by_kind\": ";
    detail::write_counts(output, observations);
    output << "},\n  \"pairs\": [\n";
    for (std::size_t index = 0; index < audit.pairs.size(); ++index) {
        const PairResult& result = audit.pairs[index];
        output << "    {\"animation\": ";
        detail::write_identity(output, result.pair.animation);
        output << ", \"selected_model\": " << json_string(result.pair.selected_model)
               << ", \"selected_sha256\": " << json_string(result.pair.selected_sha256) << ", \"candidate\": ";
        detail::write_identity(output, result.pair.candidate);
        output << ", \"candidate_identity_verified\": true, \"priority\": " << (result.pair.priority ? "true" : "false")
               << ", \"animation_set\": " << json_string(result.animation_set)
               << ", \"disposition\": " << json_string(result.disposition) << ", \"approved\": false, \"evidence\": ";
        detail::write_list(output, result.evidence, [&output](const Evidence& item) {
            output << "{\"object_id\": " << json_string(item.object_id) << ", \"type_name\": " << json_string(item.type_name)
                   << ", \"category\": " << json_string(item.category) << ", \"chain\": ";
            detail::write_list(output, item.chain, [&output](const std::string& link) { output << json_string(link); });
            output << ", \"model\": ";
            detail::write_site(output, item.model);
            output << ", \"animation_set\": ";
            detail::write_site(output, item.animation_set);
            output << '}';
        });
        output << ", \"conflicts\": ";
        detail::write_list(output, result.conflicts, [&output](const Conflict& item) {
            output << "{\"object_id\": " << json_string(item.object_id) << ", \"reason\": " << json_string(item.reason)
                   << ", \"sites\": ";
            detail::write_list(output, item.sites, [&output](const ValueSite& site) { detail::write_site(output, site); });
            output << '}';
        });
        output << ", \"unresolved\": ";
        detail::write_list(output, result.unresolved, [&output](const Unresolved& item) {
            output << "{\"object_id\": " << json_string(item.object_id) << ", \"code\": " << json_string(item.code)
                   << ", \"message\": " << json_string(item.message) << ", \"sites\": ";
            detail::write_list(output, item.sites, [&output](const ValueSite& site) { detail::write_site(output, site); });
            output << '}';
        });
        output << ", \"observations\": ";
        detail::write_list(output, result.observations, [&output](const Observation& item) {
            output << "{\"kind\": " << json_string(item.kind) << ", \"object_id\": " << json_string(item.object_id)
                   << ", \"winner\": " << (item.winner ? "true" : "false") << ", \"site\": ";
            detail::write_site(output, item.site);
            output << '}';
        });
        output << '}' << (index + 1 == audit.pairs.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
}
