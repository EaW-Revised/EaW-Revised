#include "eawr/mods/chain.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/mods/steam/library_folders.hpp"
#include "eawr/mods/workshop.hpp"

#include <algorithm>
#include <set>
#include <system_error>

namespace eawr::mods {
namespace fs = steam::fs;
namespace {
using J = steam::JsonValue;
void put(J& object, std::string key, J value) {
    object.kind = J::Kind::object;
    for (std::size_t i = 0; i < object.keys.size(); ++i) {
        if (object.keys[i] == key) { object.values[i] = std::move(value); return; }
    }
    object.keys.push_back(std::move(key)); object.values.push_back(std::move(value));
}
J string_value(std::string value) { J j; j.kind = J::Kind::string; j.string = std::move(value); return j; }
// JSON comments/trailing commas belong to modinfo only, never to our own strict formats.
std::optional<J> mod_json(std::string_view text) {
    if (text.size() > kMaxModText) return std::nullopt;
    std::string clean(text);
    bool quoted = false;
    for (std::size_t i = 0; i < clean.size(); ++i) {
        if (quoted) {
            if (clean[i] == '\\') { ++i; continue; }
            if (clean[i] == '"') quoted = false;
        } else if (clean[i] == '"') quoted = true;
        else if (clean[i] == '/' && i + 1 < clean.size()) {
            if (clean[i + 1] == '/') {
                clean[i++] = ' '; clean[i] = ' ';
                while (i + 1 < clean.size() && clean[i + 1] != '\n') clean[++i] = ' ';
            } else if (clean[i + 1] == '*') {
                clean[i++] = ' '; clean[i] = ' ';
                bool ended = false;
                while (i + 1 < clean.size()) {
                    ++i;
                    if (clean[i] == '*' && i + 1 < clean.size() && clean[i + 1] == '/') {
                        clean[i++] = ' '; clean[i] = ' '; ended = true; break;
                    }
                    clean[i] = ' ';
                }
                if (!ended) return std::nullopt;
            }
        }
    }
    quoted = false;
    for (std::size_t i = 0; i < clean.size(); ++i) {
        if (quoted) { if (clean[i] == '\\') ++i; else if (clean[i] == '"') quoted = false; }
        else if (clean[i] == '"') quoted = true;
        else if (clean[i] == ',') {
            const auto next = clean.find_first_not_of(" \t\r\n", i + 1);
            if (next != std::string::npos && (clean[next] == ']' || clean[next] == '}')) clean[i] = ' ';
        }
    }
    return steam::parse_json(clean);
}
std::string mods_key(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    path = steam::path_to_utf8(steam::path_from_utf8(path).lexically_normal());
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.starts_with("./")) path.erase(0, 2);
    if (steam::lower_ascii(path).starts_with("mods/")) path.erase(0, 5);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return "mods:" + path;
}
const std::string* usable_name(const J& json) {
    if (const auto* name = json.string_member("name"); name && !name->empty()) return name;
    const auto* steamdata = json.member("steamdata");
    const auto* title = steamdata && steamdata->kind == J::Kind::object ? steamdata->string_member("title") : nullptr;
    return title && !title->empty() ? title : nullptr;
}
std::optional<ModInfo> info_from_value(const J& j) {
    if (j.kind != J::Kind::object) return std::nullopt;
    const auto* name = usable_name(j);
    if (!name) return std::nullopt;
    ModInfo info; info.metadata = j; info.name = *name;
    if (const auto* version = j.member("version")) {
        if (version->kind != J::Kind::string) return std::nullopt;
        info.version = version->string;
    }
    if (const auto* role = j.member("role")) {
        if (role->kind != J::Kind::string) return std::nullopt;
        const auto parsed = parse_mod_role(role->string);
        if (!parsed) return std::nullopt;
        info.role = *parsed;
    }
    if (const auto* deps = j.member("dependencies")) {
        if (deps->kind != J::Kind::array) return std::nullopt;
        info.dependencies.emplace();
        for (std::size_t i = 0; i < deps->array.size(); ++i) {
            const J& entry = deps->array[i];
            if (i == 0 && entry.kind == J::Kind::string) {
                if (entry.string == "FullResolved") info.dependencies->layout = ResolveLayout::full;
                else if (entry.string == "ResolveLastItem") info.dependencies->layout = ResolveLayout::last_item;
                else if (entry.string != "ResolveRecursive") return std::nullopt;
                continue;
            }
            const auto* type = entry.member("modtype");
            const auto* id = entry.string_member("identifier");
            if (!type || type->kind != J::Kind::number || !id || id->empty()) return std::nullopt;
            const auto separator = id->find(':');
            const auto workshop_id = id->substr(0, separator);
            if (type->number == 1 && is_workshop_id(workshop_id)
                && (separator == std::string::npos || separator + 1 < id->size())) info.dependencies->parents.push_back("workshop:" + *id);
            else if (type->number == 0) info.dependencies->parents.push_back(mods_key(*id));
            else return std::nullopt;
        }
    }
    return info;
}
std::vector<fs::path> top_files(const fs::path& root, bool& incomplete) {
    std::vector<fs::path> files;
    std::error_code error;
    std::size_t count = 0;
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        if (++count > kMaxModTopEntries) { incomplete = true; break; }
        std::error_code status_error;
        if (it->symlink_status(status_error).type() == fs::file_type::regular) files.push_back(it->path());
        if (status_error) incomplete = true;
    }
    if (error) incomplete = true;
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
        return steam::path_to_utf8(a.filename()) < steam::path_to_utf8(b.filename());
    });
    return files;
}
void apply_info(ModRecord& record, const ModInfo& info) {
    record.name = info.name; record.role = info.role;
    put(record.metadata, "modinfo", info.metadata);
}
const ModRecord* find_record(std::span<const ModRecord> records, const std::string& key) {
    for (const auto& r : records) if (r.key == key) return &r;
    // Mods-folder paths are case insensitive; Workshop/manual/variant keys remain exact.
    if (key.starts_with("mods:")) for (const auto& r : records) {
        if (steam::lower_ascii(r.key) == steam::lower_ascii(key)) return &r;
    }
    for (const auto& r : records) {
        const bool variant = r.metadata.member("variant_file") != nullptr;
        const std::string suffix = variant ? ":" + r.name : "";
        const auto base_key = r.key.substr(0, r.key.find("#variant:"));
        if (variant && steam::lower_ascii(base_key + suffix) == steam::lower_ascii(key)) return &r;
        if (key.starts_with("mods:") && !r.root.empty()
            && steam::lower_ascii(mods_key(r.root) + suffix) == steam::lower_ascii(key)) return &r;
    }
    return nullptr;
}
const KnownChain* find_known(const KnownChains& known, std::string_view key) {
    for (const auto& c : known.chains) if (key == "workshop:" + c.id) return &c;
    return nullptr;
}
void field(std::string& bytes, std::string_view value) {
    bytes += std::to_string(value.size()) + ":"; bytes += value;
}
} // namespace

std::optional<ModInfo> parse_modinfo(std::string_view text) {
    const auto j = mod_json(text);
    return j ? info_from_value(*j) : std::nullopt;
}

KnownChains parse_known_chains(std::string_view text) {
    KnownChains result;
    const auto bad = [&]() {
        result.chains.clear();
        result.warnings.push_back({std::string(codes::malformed_known_chains), "known-chains input has an unsupported or malformed schema"});
        return result;
    };
    if (text.size() > kMaxModText) return bad();
    const auto j = steam::parse_json(text);
    if (!j || j->kind != J::Kind::object) return bad();
    const auto* format = j->string_member("format");
    const auto* version = j->member("version");
    const auto* chains = j->member("chains");
    if (!format || *format != "eawr-known-chains" || !version || version->kind != J::Kind::number || version->number != 1
        || !chains || chains->kind != J::Kind::array) return bad();
    std::set<std::string> ids;
    for (const auto& c : chains->array) {
        const auto* id = c.string_member("id"); const auto* name = c.string_member("name");
        const auto* parents = c.member("parents");
        if (!id || !is_workshop_id(*id) || !ids.insert(*id).second || !name || name->empty()
            || !parents || parents->kind != J::Kind::array) return bad();
        KnownChain row; row.id = *id; row.name = *name;
        for (const auto& p : parents->array) {
            if (p.kind != J::Kind::string || !is_workshop_id(p.string)) return bad();
            row.parents.push_back(p.string);
        }
        if (const auto* role = c.member("role")) {
            if (role->kind != J::Kind::string) return bad();
            const auto parsed = parse_mod_role(role->string);
            if (!parsed) return bad();
            row.role = *parsed;
        }
        result.chains.push_back(std::move(row));
    }
    return result;
}

std::vector<std::string> launch_script_hints(std::string_view text) {
    std::vector<std::string> hints;
    if (text.size() > kMaxModText) return hints;
    // Token scanning, not a shell parser: never expands variables, quotes, redirects or commands.
    for (std::size_t i = 0; i < text.size();) {
        const bool boundary = i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t' || text[i - 1] == '\n'
                              || text[i - 1] == '\r' || text[i - 1] == '"';
        const auto tail = text.substr(i);
        const bool workshop = tail.starts_with("STEAMMOD=");
        const bool modpath = tail.starts_with("MODPATH=");
        if (!boundary || (!workshop && !modpath)) { ++i; continue; }
        i += workshop ? 9 : 8;
        const bool quoted = i < text.size() && text[i] == '"';
        if (quoted) ++i;
        const auto start = i;
        while (i < text.size() && text[i] != '"' && text[i] != '\r' && text[i] != '\n'
               && text[i] != '&' && text[i] != '|' && text[i] != '<' && text[i] != '>'
               && (quoted || (text[i] != ' ' && text[i] != '\t'))) ++i;
        const std::string value(text.substr(start, i - start));
        if (value.empty() || value.find_first_of("%!^") != std::string::npos) continue;
        if (workshop && is_workshop_id(value)) hints.push_back("workshop:" + value);
        else if (modpath) hints.push_back(mods_key(value));
    }
    return hints;
}

void read_mod_metadata(Catalog& catalog) {
    std::vector<ModRecord> enriched;
    for (auto record : catalog.records) {
        if (record.metadata.member("variant_file")) { enriched.push_back(std::move(record)); continue; }
        if (record.state == ModState::missing || record.state == ModState::downloading || record.root.empty()) {
            enriched.push_back(std::move(record)); continue;
        }
        bool incomplete = false;
        const auto files = top_files(steam::path_from_utf8(record.root), incomplete);
        std::optional<ModInfo> main;
        std::vector<std::pair<std::string, J>> variants;
        J hints; hints.kind = J::Kind::array;
        bool has_main = false;
        for (const auto& file : files) {
            const auto name = steam::lower_ascii(steam::path_to_utf8(file.filename()));
            const bool is_main = name == "modinfo.json";
            const bool variant = name.size() > 13 && name.ends_with("-modinfo.json");
            const auto ext = steam::lower_ascii(steam::path_to_utf8(file.extension()));
            if (!is_main && !variant && ext != ".bat" && ext != ".cmd") continue;
            const auto text = steam::read_text_file(file, kMaxModText);
            if (!is_main && !variant) {
                if (!text) { incomplete = true; continue; }
                for (const auto& key : launch_script_hints(*text)) hints.array.push_back(string_value(key));
                continue;
            }
            if (is_main) has_main = true;
            const auto json = text ? mod_json(*text) : std::nullopt;
            if (!json || json->kind != J::Kind::object || !usable_name(*json)) {
                record.warnings.push_back({std::string(codes::malformed_modinfo), "a modinfo file could not be read"}); continue;
            }
            if (is_main) {
                main = info_from_value(*json);
                if (!main) record.warnings.push_back({std::string(codes::malformed_modinfo), "main modinfo has malformed fields"});
            } else variants.emplace_back(steam::path_to_utf8(file.filename()), *json);
        }
        if (incomplete) record.warnings.push_back({std::string(codes::scan_incomplete), "top-level metadata scan was incomplete"});
        put(record.metadata, "launch_script_hints", std::move(hints));
        const ModRecord base = record;
        if (main) apply_info(record, *main);
        std::vector<ModRecord> entries;
        for (auto& [filename, json] : variants) {
            J merged = main ? main->metadata : J{};
            for (std::size_t i = 0; i < json.keys.size(); ++i) put(merged, json.keys[i], json.values[i]);
            auto info = info_from_value(merged);
            if (!info) {
                record.warnings.push_back({std::string(codes::malformed_modinfo), "variant modinfo has malformed fields"}); continue;
            }
            // A variant supplies its own identity/name while other fields inherit.
            info->name = *usable_name(json);
            ModRecord entry = base;
            entry.key += "#variant:" + filename;
            put(entry.metadata, "variant_file", string_value(filename));
            apply_info(entry, *info);
            entries.push_back(std::move(entry));
        }
        const auto warnings = record.warnings;
        if (has_main || entries.empty()) enriched.push_back(std::move(record));
        for (auto& entry : entries) {
            // A bad sibling must still be visible when only variants are instantiated.
            entry.warnings = warnings;
            enriched.push_back(std::move(entry));
        }
    }
    catalog.records.clear();
    std::set<std::string> keys;
    for (auto& record : enriched) if (keys.insert(record.key).second) catalog.records.push_back(std::move(record));
}

void apply_known_chains(Catalog& catalog, const KnownChains& known) {
    for (auto& r : catalog.records) if (const auto* c = find_known(known, r.key)) {
        if (!r.metadata.member("modinfo")) r.name = c->name;
        if (r.role == ModRole::unknown) r.role = c->role;
    }
    catalog.warnings.insert(catalog.warnings.end(), known.warnings.begin(), known.warnings.end());
}

std::string chain_fingerprint(std::span<const std::string> layers, std::span<const ModRecord> records) {
    std::string bytes = "eawr-chain-fingerprint:1;";
    for (const auto& key : layers) {
        field(bytes, key);
        const auto* r = find_record(records, key);
        field(bytes, r ? "present" : "missing");
        if (!r) continue;
        const auto& f = r->fingerprint;
        field(bytes, f.acf_manifest); field(bytes, std::to_string(f.time_updated)); field(bytes, std::to_string(f.acf_size));
        field(bytes, f.tree); field(bytes, std::to_string(f.tree_files)); field(bytes, f.tree_capped ? "capped" : "complete");
    }
    return core::sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

namespace {
Chain resolve_roots(std::span<const std::string> roots, std::span<const ModRecord> records,
                    const KnownChains& known, const PlayerEdges& player_edges,
                    std::vector<std::pair<std::string, std::string>>* constraints = nullptr) {
    Chain result;
    struct Node {
        std::string key, why;
        const ModRecord* record = nullptr;
        bool expand = false, expanded = false;
        std::set<std::size_t> parents;
        std::size_t incoming = 0;
    };
    std::vector<Node> nodes;
    std::map<std::string, std::size_t> indices;
    auto diagnostic = [&](std::string_view code, const std::string& key, std::string message, const ModRecord* record, bool fatal) {
        const bool installed = record && record->state != ModState::missing && record->state != ModState::downloading;
        std::optional<bool> listed;
        if (record && record->kind == ModKind::workshop && !record->has_warning(codes::workshop_manifest_unreadable))
            listed = !record->has_warning(codes::folder_not_listed);
        if (!listed.has_value() && key.starts_with("workshop:")
            && (code == codes::parent_not_installed || code == codes::downloading)) message += "; Steam listing is unknown";
        result.diagnostics.push_back({std::string(code), key, std::move(message), installed, listed});
        if (fatal) result.launchable = false;
    };
    auto add = [&](std::string key, std::string why, bool expand) {
        const auto* r = find_record(records, key);
        if (r) key = r->key;
        if (const auto found = indices.find(key); found != indices.end()) {
            nodes[found->second].expand = nodes[found->second].expand || expand;
            return found->second;
        }
        const auto index = nodes.size();
        indices.emplace(key, index);
        nodes.push_back({std::move(key), std::move(why), r, expand, false, {}, 0});
        return index;
    };
    auto edge = [&](std::size_t child, std::size_t parent) {
        if (nodes[child].parents.insert(parent).second) {
            ++nodes[parent].incoming;
            if (constraints) constraints->emplace_back(nodes[child].key, nodes[parent].key);
        }
    };
    for (std::size_t i = 0; i < roots.size(); ++i) add(roots[i], i == 0 ? "leaf" : "player-layer", true);
    // Build the graph iteratively; a maliciously deep dependency chain cannot exhaust the stack.
    // Revisit an unexpanded node if a later recursive reference enables expansion.
    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t n = 0; n < nodes.size(); ++n) {
            if (!nodes[n].expand || nodes[n].expanded) continue;
            nodes[n].expanded = true; changed = true;
            const std::string key = nodes[n].key;
            const auto* r = nodes[n].record;
            if (!r || r->state == ModState::missing || r->state == ModState::downloading) continue;
            const auto* kc = find_known(known, key);
            Dependencies deps;
            std::string source;
            if (const auto p = player_edges.find(key); p != player_edges.end()) { deps.parents = p->second; source = "player"; }
            else {
                const auto* value = r->metadata.member("modinfo");
                const auto info = value ? info_from_value(*value) : std::nullopt;
                if (info && info->dependencies) { deps = *info->dependencies; source = "modinfo"; }
                else if (kc) {
                    for (const auto& id : kc->parents) deps.parents.push_back("workshop:" + id);
                    deps.layout = ResolveLayout::full; source = "known-chain";
                } else if (const auto* hints = r->metadata.member("launch_script_hints")) {
                    for (const auto& h : hints->array) if (h.kind == J::Kind::string && h.string != key) deps.parents.push_back(h.string);
                    deps.layout = ResolveLayout::full; source = "launch-script";
                }
            }
            std::optional<std::size_t> previous;
            std::set<std::size_t> direct;
            for (std::size_t i = 0; i < deps.parents.size(); ++i) {
                if (source == "launch-script") diagnostic(codes::script_hint, deps.parents[i], "launch-script hint needs player confirmation", r, true);
                const auto parent = add(deps.parents[i], source, deps.layout == ResolveLayout::recursive
                      || (deps.layout == ResolveLayout::last_item && i + 1 == deps.parents.size()));
                edge(n, parent);
                if (!direct.insert(parent).second) {
                    diagnostic(codes::chain_duplicate, nodes[parent].key, "parent repeats in one dependency list; one layer retained",
                               nodes[parent].record, false);
                    continue;
                }
                result.edges.push_back({key, nodes[parent].key, source});
                // The declared sibling order is a constraint as well as the ancestry relation.
                if (previous && *previous != parent) edge(*previous, parent);
                previous = parent;
            }
        }
    }
    std::set<std::size_t> ready;
    for (std::size_t n = 0; n < nodes.size(); ++n) if (nodes[n].incoming == 0) ready.insert(n);
    std::vector<std::size_t> order;
    while (!ready.empty()) {
        const auto n = *ready.begin(); ready.erase(ready.begin()); order.push_back(n);
        for (const auto p : nodes[n].parents) if (--nodes[p].incoming == 0) ready.insert(p);
    }
    if (order.size() != nodes.size()) {
        for (std::size_t n = 0; n < nodes.size(); ++n) if (nodes[n].incoming != 0) {
            diagnostic(codes::chain_cycle, nodes[n].key, "dependency cycle or contradictory parent order", nodes[n].record, true);
            order.push_back(n);
        }
    }
    for (const auto n : order) {
        const auto& node = nodes[n]; const auto* r = node.record;
        result.layers.push_back(node.key);
        result.rows.push_back({node.key, node.why, r ? r->state : ModState::missing});
        if (!r || r->state == ModState::missing) {
            diagnostic(codes::parent_not_installed, node.key, "required layer is not installed", r, true); continue;
        }
        if (r->state == ModState::downloading) {
            diagnostic(codes::downloading, node.key, "Steam has not finished this layer", r, true); continue;
        }
        const auto* kc = find_known(known, node.key);
        const auto role = r->role == ModRole::unknown && kc ? kc->role : r->role;
        if (!r->has_data || (node.why == "leaf" && role == ModRole::parent_only))
            diagnostic(codes::not_playable, node.key, "layer has no Data folder or the selected leaf is parent-only", r, true);
    }
    result.fingerprint = chain_fingerprint(result.layers, records);
    return result;
}

} // namespace
Chain resolve_chain(std::string_view leaf, std::span<const ModRecord> records, const KnownChains& known,
                    const PlayerEdges& player_edges) {
    const std::vector<std::string> roots{std::string(leaf)};
    return resolve_roots(roots, records, known, player_edges);
}

OrderCheck check_order(std::span<const std::string> layers, std::span<const ModRecord> records,
                       const KnownChains& known, std::span<const std::string> accepted_warnings) {
    OrderCheck result;
    std::map<std::string, std::size_t> positions;
    std::vector<std::string> roots;
    for (const auto& key : layers) {
        const auto* r = find_record(records, key);
        const auto canonical = r ? r->key : key;
        if (!positions.emplace(canonical, roots.size()).second) {
            result.warnings.push_back({std::string(codes::chain_duplicate), canonical, "layer repeats in player order", false, {}});
        } else roots.push_back(canonical);
    }
    std::vector<std::pair<std::string, std::string>> constraints;
    const auto chain = resolve_roots(roots, records, known, {}, &constraints);
    result.launchable = chain.launchable;
    for (const auto& d : chain.diagnostics) {
        // Repeated upstream dependencies are already deduplicated by the resolver.
        if (d.code != codes::chain_duplicate) result.diagnostics.push_back(d);
    }
    for (const auto& key : chain.layers) {
        const auto* r = find_record(records, key);
        if (r && r->has_warning(codes::scan_incomplete)) {
            result.diagnostics.push_back({std::string(codes::scan_incomplete), key, "top-level metadata scan is incomplete", false, {}});
            result.launchable = false;
        }
        const bool diagnosed = std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& d) { return d.key == key; });
        if (!positions.contains(key) && !diagnosed) {
            result.diagnostics.push_back({std::string(codes::parent_not_installed), key, "required layer is absent from player order", false, {}});
            result.launchable = false;
        }
    }
    for (const auto& [child, parent] : constraints) {
        const auto c = positions.find(child), p = positions.find(parent);
        if (c != positions.end() && p != positions.end() && p->second < c->second)
            result.warnings.push_back({std::string(codes::chain_order), child, "declared order requires " + child + " above " + parent, false, {}});
    }
    for (const auto& w : result.warnings)
        if (std::find(accepted_warnings.begin(), accepted_warnings.end(), w.code) == accepted_warnings.end()) result.launchable = false;
    return result;
}

std::vector<Warning> detect_data_overwrite(const fs::path& corruption, std::span<const std::string> retail_files, std::size_t entry_cap) {
    std::vector<Warning> warnings;
    std::set<std::string> retail, extra;
    for (const auto& file : retail_files) retail.insert(steam::lower_ascii(file));
    const auto data = steam::find_child_ci(corruption, "Data");
    if (!data) return warnings;
    std::vector<std::pair<fs::path, std::string>> pending{{*data, ""}};
    std::size_t entries = 0;
    bool incomplete = false;
    while (!pending.empty() && !incomplete) {
        auto [folder, prefix] = pending.back(); pending.pop_back();
        std::error_code error;
        for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
            if (++entries > entry_cap) { incomplete = true; break; }
            std::error_code status_error;
            const auto type = it->symlink_status(status_error).type();
            if (status_error) { incomplete = true; break; }
            const auto name = prefix + steam::lower_ascii(steam::path_to_utf8(it->path().filename()));
            if (type == fs::file_type::directory) pending.emplace_back(it->path(), name + "/");
            else if (type == fs::file_type::regular && (name.ends_with(".xml") || name.ends_with(".meg")) && !retail.contains(name)) extra.insert(name);
        }
        if (error) incomplete = true;
    }
    if (!extra.empty()) warnings.push_back({std::string(codes::data_overwrite), "MM-OVERWRITE-1: non-retail XML/MEG files in corruption/Data"});
    if (incomplete) warnings.push_back({std::string(codes::scan_incomplete), "MM-OVERWRITE-1: Data inventory scan was incomplete"});
    return warnings;
}
} // namespace eawr::mods
