#include "records.hpp"
#include "eawr/vfs/vfs.hpp"
#include "records_io.hpp"
#include "../mods_service/src/json_value.hpp"
#include "eawr/data/record_index.hpp"
#include "eawr/mods/profile.hpp"
#include "eawr/core/sha256.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <optional>
#include <random>
#include <iterator>

namespace eawr::loadcheck {
namespace {
namespace fs = std::filesystem;
using J = mods::steam::JsonValue;
using mods::steam::json_quote;
using mods_service::encode_json;
using data::RecordTagHow;
constexpr std::array<std::string_view, 6> how_names{"own", "overridden", "inherited", "merged", "added", "dropped"};
std::string lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return text;
}
std::string location(const data::SourceLocation& s) {
    return "{\"logical_path\":" + json_quote(s.logical_path) + ",\"source_id\":" + json_quote(s.source_id)
        + ",\"layer_id\":" + json_quote(s.layer_id) + ",\"line\":" + std::to_string(s.line) + ",\"column\":" + std::to_string(s.column) + '}';
}
std::string node(const data::XmlNode& n) {
    std::string out = "{\"name\":" + json_quote(n.name) + ",\"raw_text\":" + json_quote(n.raw_text) + ",\"attributes\":[";
    for (const auto& a : n.attributes) {
        if (out.back() != '[') out += ',';
        out += "{\"name\":" + json_quote(a.name) + ",\"value\":" + json_quote(a.value) + ",\"source\":" + location(a.source) + '}';
    }
    out += "],\"children\":[";
    for (const auto& child : n.children) { if (out.back() != '[') out += ','; out += node(child); }
    return out + "],\"source\":" + location(n.source) + '}';
}
std::string values(const std::vector<data::EffectiveValue>& rows) {
    std::string out = "[";
    for (const auto& value : rows) {
        if (out.size() > 1) out += ',';
        std::string_view provenance = "own";
        switch (value.provenance) {
        case data::ValueProvenance::own: break;
        case data::ValueProvenance::overridden: provenance = "overridden"; break;
        case data::ValueProvenance::inherited: provenance = "inherited"; break;
        case data::ValueProvenance::merged: provenance = "merged"; break;
        case data::ValueProvenance::added: provenance = "added"; break;
        }
        out += "{\"node\":" + node(value.value) + ",\"provenance\":" + json_quote(provenance)
            + ",\"source_object_id\":" + json_quote(value.source_object_id) + '}';
    }
    return out + ']';
}
bool same_node(const data::XmlNode& left, const data::XmlNode& right) {
    if (left.name != right.name || left.raw_text != right.raw_text || left.attributes.size() != right.attributes.size()
        || left.children.size() != right.children.size()) return false;
    for (std::size_t i = 0; i < left.attributes.size(); ++i)
        if (left.attributes[i].name != right.attributes[i].name || left.attributes[i].value != right.attributes[i].value) return false;
    for (std::size_t i = 0; i < left.children.size(); ++i) if (!same_node(left.children[i], right.children[i])) return false;
    return true;
}
struct CachedRecord {
    const data::RecordEntry* entry{};
    std::vector<data::RecordTagRow> tags;
    std::vector<std::string> chain;
    std::vector<const data::Definition*> definitions;
    std::vector<std::string> layers;
    // Set when this one object cannot be resolved (variant cycle, missing base): it stays in the index, flagged.
    std::optional<core::Diagnostic> diagnostic;
    std::string lower_id;
    std::array<std::size_t, 6> counts{};
};
// One filter's matches and classification counts; paging reuses it instead of rescanning every tag row.
struct Filtered {
    std::vector<const CachedRecord*> matches;
    std::array<std::size_t, 6> counts{};
};
using RecordKey = std::pair<std::string, std::string>;
// Largest records.get answer the helper builds; the service adds an id/result envelope within the same 1 MiB line.
inline constexpr std::size_t get_budget = records_line_limit - 16 * 1024;
class Records {
    data::RecordIndex index_;
    std::map<RecordKey, CachedRecord> records_;
    std::string key_, layers_json_;
    std::set<std::string> mod_layers_;
    struct Page { std::string signature; std::size_t offset{}; };
    std::map<std::string, Page> pages_;
    std::map<std::string, std::shared_ptr<const Filtered>> filtered_;
    std::size_t sequence_{};
public:
    Records(const mods::Profile& profile, const fs::path& game, std::string key) : key_(std::move(key)) {
        const auto progress = [](std::string_view stage, std::size_t done, std::size_t total) {
            records_emit("{\"stage\":" + json_quote(stage) + ",\"done\":" + std::to_string(done) + ",\"total\":" + std::to_string(total) + '}');
        };
        progress("mount", 0, 5);
        std::vector<vfs::LayerRoot> vanilla_roots{{"expansion", game / "corruption/Data"}, {"base", game / "GameData/Data"}};
        auto roots = vanilla_roots;
        std::vector<data::RecordLayer> layers;
        for (auto it = profile.layers.rbegin(); it != profile.layers.rend(); ++it)
            roots.insert(roots.begin(), {it->key, vfs::mod_data_root(mods::steam::path_from_utf8(it->root))});
        for (const auto& layer : profile.layers) { layers.push_back({layer.key, "mod", layers.size()}); mod_layers_.insert(layer.key); }
        layers.push_back({"expansion", "expansion", layers.size()}); layers.push_back({"base", "base", layers.size()});
        const auto mount = [](const std::vector<vfs::LayerRoot>& input) {
            auto chain = vfs::resolve_manifest_chain(input);
            if (!chain) throw std::runtime_error(chain.error().message);
            std::vector<vfs::MountSpec> specs;
            for (auto& item : chain.value()) specs.push_back(std::move(item.mount));
            auto result = vfs::Vfs::mount(specs);
            if (!result) throw std::runtime_error(result.error().message);
            return std::move(result.value());
        };
        auto vanilla_vfs = mount(vanilla_roots), profile_vfs = mount(roots);
        data::LoadOptions options; options.parse_unregistered_xml = false;
        progress("vanilla", 1, 5);
        auto vanilla = data::load_catalog(vanilla_vfs, data::Profile::foc, options);
        if (!vanilla) throw std::runtime_error(vanilla.error().message);
        if (vanilla.value().health.failed()) throw std::runtime_error("vanilla catalog has registry errors");
        progress("profile", 2, 5);
        auto loaded = data::load_catalog(profile_vfs, profile.layers.empty() ? data::Profile::foc : data::Profile::remake, options);
        if (!loaded) throw std::runtime_error(loaded.error().message);
        if (loaded.value().health.failed()) throw std::runtime_error("profile catalog has registry errors");
        progress("shadowed", 3, 5);
        auto shadowed = data::load_shadowed_definitions(profile_vfs, loaded.value().catalog, options);
        if (!shadowed) throw std::runtime_error(shadowed.error().message);
        auto built = data::build_record_index(vanilla.value().catalog, loaded.value().catalog, shadowed.value(), profile_vfs, layers);
        if (!built) throw std::runtime_error(built.error().message);
        index_ = std::move(built.value());
        layers_json_ = "[";
        for (const auto& layer : layers) {
            if (layers_json_.size() > 1) layers_json_ += ',';
            layers_json_ += "{\"key\":" + json_quote(layer.name) + ",\"kind\":" + json_quote(layer.kind) + ",\"priority\":" + std::to_string(layer.priority) + '}';
        }
        layers_json_ += ']';
        progress("index", 4, 5);
        std::map<RecordKey, const data::RecordEntry*> entries;
        for (const auto& entry : index_.records)
            entries.emplace(RecordKey{std::string(data::to_string(entry.category)), lower(entry.id)}, &entry);
        // All catalog resolution, including lazy cache writes, stays on this helper's one thread.
        for (const auto& entry : index_.records) {
            CachedRecord record; record.entry = &entry;
            // One object that cannot be resolved (a variant cycle, a missing base) is flagged, never fatal: the rest
            // of the chain is still indexed and the record view is where such authoring mistakes should show.
            auto tags = index_.tags(entry);
            if (!tags) record.diagnostic = tags.error();
            else record.tags = std::move(tags.value());
            if (!record.diagnostic && entry.winner) {
                auto effective = index_.effective(entry);
                if (!effective) record.diagnostic = effective.error();
                else record.chain = std::move(effective.value().chain);
            } else record.chain = {entry.id};
            if (record.diagnostic) record.tags.clear(); // a flagged object has no tag rows and no counts
            record.lower_id = lower(entry.id);
            // Authored columns also include ancestor types, kept distinct by object_id.
            for (auto it = record.chain.rbegin(); it != record.chain.rend(); ++it) {
                const auto found = entries.find({std::string(data::to_string(entry.category)), lower(*it)});
                if (found == entries.end()) continue;
                const auto* ancestor = found->second;
                for (const auto& def : ancestor->definitions) {
                    record.definitions.push_back(def.definition);
                    if (lower(ancestor->id) == lower(entry.id)) continue;
                    std::map<std::string, std::vector<data::XmlNode>> grouped;
                    for (const auto& child : def.definition->root.children) grouped[lower(child.name)].push_back(child);
                    for (auto& [name, nodes] : grouped) {
                        auto row = std::find_if(record.tags.begin(), record.tags.end(), [&](const auto& value) { return lower(value.tag) == name; });
                        if (row == record.tags.end()) {
                            data::RecordTagRow added; added.tag = nodes.front().name; added.how = RecordTagHow::dropped;
                            record.tags.push_back(std::move(added)); row = std::prev(record.tags.end());
                        }
                        row->authored.push_back({def.definition->root.source, def.definition->registry_order,
                            def.definition->definition_order, def.definition->namespace_winner, std::move(nodes)});
                    }
                }
            }
            if (record.diagnostic) record.definitions.clear();
            for (const auto& row : record.tags) ++record.counts[static_cast<std::size_t>(row.how)];
            for (const auto& def : entry.definitions) {
                const auto& layer = def.definition->root.source.layer_id;
                if (std::find(record.layers.begin(), record.layers.end(), layer) == record.layers.end()) record.layers.push_back(layer);
            }
            records_.emplace(RecordKey{std::string(data::to_string(entry.category)), lower(entry.id)}, std::move(record));
            if (records_.size() % 100 == 0) progress("records", records_.size(), index_.records.size());
        }
        progress("complete", 5, 5);
    }
    static std::string diagnostic_json(const core::Diagnostic& d) {
        return "{\"code\":" + json_quote(d.code) + ",\"message\":" + json_quote(d.message) + '}';
    }
    std::string tag_json(const CachedRecord& r, const data::RecordTagRow& tag, bool slim) const {
        std::string out = "{\"tag\":" + json_quote(tag.tag) + ",\"vanilla\":" + (slim ? "[]" : values(tag.vanilla)) + ",\"authored\":[";
        if (!slim) for (const auto* def : r.definitions) {
            std::string authored = "[";
            for (const auto& child : def->root.children) if (lower(child.name) == lower(tag.tag)) {
                if (authored.size() > 1) authored += ',';
                authored += node(child);
            }
            if (authored.size() == 1) continue;
            if (out.back() != '[') out += ',';
            out += "{\"object_id\":" + json_quote(def->id) + ",\"source\":" + location(def->root.source)
                + ",\"registry_order\":" + std::to_string(def->registry_order) + ",\"definition_order\":" + std::to_string(def->definition_order)
                + ",\"winner\":" + (def->namespace_winner ? "true" : "false") + ",\"values\":" + authored + "]}";
        }
        out += std::string("],\"effective\":") + (slim ? "[]" : values(tag.effective)) + ",\"how\":" + json_quote(how_names[static_cast<std::size_t>(tag.how)]);
        // A row too large for any answer keeps its name and classification only; the flag says its values were left out.
        if (slim) out += ",\"too_large\":true";
        return out + '}';
    }
    // The answer stays under the 1 MiB line by construction: tags are added whole while they fit, then the answer
    // says `truncated` and the offset to ask for next (`tag_offset`). Normal objects fit and keep the default shape.
    std::string get(const J& params) const {
        const auto* id = params.string_member("id"); const auto* category = params.string_member("category");
        if (!id || !category) throw std::invalid_argument("id and category must be strings");
        const auto found = records_.find({*category, lower(*id)});
        if (found == records_.end()) throw std::invalid_argument("record not found");
        const auto& r = found->second;
        std::size_t offset{};
        if (const auto* v = params.member("tag_offset")) {
            if (v->kind != J::Kind::number || !std::isfinite(v->number) || v->number < 0 || std::floor(v->number) != v->number)
                throw std::invalid_argument("tag_offset must be a non-negative integer");
            if (v->number > static_cast<double>(r.tags.size())) throw std::invalid_argument("tag_offset is past the last tag");
            offset = static_cast<std::size_t>(v->number);
        }
        if (offset > r.tags.size()) throw std::invalid_argument("tag_offset is past the last tag");
        std::string out = "{\"chain_fingerprint\":" + json_quote(key_) + ",\"id\":" + json_quote(r.entry->id)
            + ",\"type\":" + json_quote(r.entry->type_name) + ",\"category\":" + json_quote(*category) + ",\"variant_chain\":[";
        for (const auto& ancestor : r.chain) { if (out.back() != '[') out += ','; out += json_quote(ancestor); }
        out += "],\"tags\":[";
        std::string tail;
        if (r.diagnostic) tail += ",\"diagnostic\":" + diagnostic_json(*r.diagnostic);
        const std::size_t budget = get_budget;
        std::size_t next = offset;
        for (; next < r.tags.size(); ++next) {
            auto piece = tag_json(r, r.tags[next], false);
            if (piece.size() > budget / 2) piece = tag_json(r, r.tags[next], true);
            // Leave room for the marker members and the service's result envelope.
            if (out.size() + piece.size() + 1 + tail.size() + 128 > budget && next > offset) break;
            if (out.back() != '[') out += ',';
            out += piece;
        }
        out += ']';
        if (offset > 0 || next < r.tags.size())
            out += ",\"tag_count\":" + std::to_string(r.tags.size()) + ",\"tag_offset\":" + std::to_string(offset);
        if (next < r.tags.size())
            out += ",\"truncated\":true,\"next_tag_offset\":" + std::to_string(next);
        return out + tail + '}';
    }
    // The matching records and their classification counts for one filter. Computed once per filter (the helper
    // already belongs to one profile and chain fingerprint), so paging only slices the stored list.
    std::shared_ptr<const Filtered> filter(const std::string& object, const std::string& tag, const std::string& layer,
                                           bool several, bool dropped) const {
        auto result = std::make_shared<Filtered>();
        const bool unfiltered_tags = tag.empty() && layer.empty() && !several && !dropped;
        for (const auto& [id, r] : records_) {
            (void)id;
            if (r.lower_id.find(object) == std::string::npos) continue;
            bool match = unfiltered_tags;
            if (!unfiltered_tags) for (const auto& row : r.tags) {
                if (lower(row.tag).find(tag) == std::string::npos || (dropped && row.how != RecordTagHow::dropped)) continue;
                std::set<std::string> authors;
                bool changed = layer.empty();
                for (const auto& column : row.authored) {
                    const auto& source = column.definition_source.layer_id;
                    if (mod_layers_.contains(source)) authors.insert(source);
                    if (source == layer) {
                        bool identical = column.values.size() == row.vanilla.size();
                        for (std::size_t i = 0; identical && i < column.values.size(); ++i)
                            identical = same_node(column.values[i], row.vanilla[i].value);
                        if (!identical) changed = true;
                    }
                }
                if (changed && (!several || authors.size() > 1)) { match = true; break; }
            }
            if (!match) continue;
            result->matches.push_back(&r);
            for (std::size_t i = 0; i < r.counts.size(); ++i) result->counts[i] += r.counts[i];
        }
        return result;
    }
    std::string page(const J& params) {
        const auto* filters = params.member("filters");
        std::string object, tag, layer; bool several = false, dropped = false;
        if (filters) {
            if (filters->kind != J::Kind::object) throw std::invalid_argument("invalid record filters");
            for (const auto& [name, target] : std::array<std::pair<std::string_view, std::string*>, 3>{{{"object", &object}, {"tag", &tag}, {"changed_by_layer", &layer}}}) {
                if (const auto* v = filters->member(name)) {
                    if (v->kind != J::Kind::string) throw std::invalid_argument("invalid record filters");
                    *target = v->string;
                }
            }
            for (const auto& [name, target] : std::array<std::pair<std::string_view, bool*>, 2>{{{"several_mods_same_tag", &several}, {"dropped", &dropped}}}) {
                if (const auto* v = filters->member(name)) {
                    if (v->kind != J::Kind::boolean) throw std::invalid_argument("invalid record filters");
                    *target = v->boolean;
                }
            }
        }
        object = lower(object); tag = lower(tag);
        std::size_t limit = 100;
        if (const auto* v = params.member("limit")) {
            if (v->kind != J::Kind::number || !std::isfinite(v->number) || v->number < 1 || v->number > 500 || std::floor(v->number) != v->number)
                throw std::invalid_argument("limit must be 1..500");
            limit = static_cast<std::size_t>(v->number);
        }
        const auto signature = json_quote(object) + json_quote(tag) + json_quote(layer) + (several ? "1" : "0") + (dropped ? "1" : "0");
        std::size_t offset{};
        if (const auto* v = params.member("cursor"); v && v->kind != J::Kind::null) {
            if (v->kind != J::Kind::string) throw std::invalid_argument("invalid cursor");
            const auto found = pages_.find(v->string);
            if (found == pages_.end() || found->second.signature != signature) throw std::invalid_argument("unknown or stale cursor");
            offset = found->second.offset;
        }
        std::shared_ptr<const Filtered> filtered;
        if (const auto cached = filtered_.find(signature); cached != filtered_.end()) filtered = cached->second;
        else {
            filtered = filter(object, tag, layer, several, dropped);
            if (filtered_.size() >= 16) filtered_.erase(filtered_.begin());
            filtered_.emplace(signature, filtered);
        }
        const auto& matches = filtered->matches;
        const auto& counts = filtered->counts;
        if (offset > matches.size()) throw std::invalid_argument("unknown or stale cursor");
        std::string extra = ",\"chain_fingerprint\":" + json_quote(key_) + ",\"layers\":" + layers_json_ + ",\"counts\":{";
        for (std::size_t i = 0; i < counts.size(); ++i) {
            if (i) extra += ',';
            extra += json_quote(how_names[i]) + ':' + std::to_string(counts[i]);
        }
        extra += "},\"total\":" + std::to_string(matches.size());
        std::string out = "{\"rows\":[";
        auto end = offset;
        while (end < matches.size() && end - offset < limit) {
            const auto& r = *matches[end];
            std::string row = "{\"id\":" + json_quote(r.entry->id) + ",\"type\":" + json_quote(r.entry->type_name)
                + ",\"category\":" + json_quote(data::to_string(r.entry->category)) + ",\"layers\":[";
            for (const auto& author : r.layers) { if (row.back() != '[') row += ','; row += json_quote(author); }
            row += ']';
            if (r.diagnostic) row += ",\"diagnostic\":" + diagnostic_json(*r.diagnostic);
            row += '}';
            if (out.size() + row.size() + extra.size() + 2048 > records_line_limit) break;
            if (end > offset) out += ',';
            out += row; ++end;
        }
        if (end == offset && end < matches.size()) throw std::runtime_error("one record row exceeds the page limit");
        std::string next = "null";
        if (end < matches.size()) {
            const auto token = key_ + '-' + std::to_string(std::random_device{}()) + '-' + std::to_string(++sequence_);
            if (pages_.size() >= 64) pages_.erase(pages_.begin());
            pages_.emplace(token, Page{signature, end}); next = json_quote(token);
        }
        return out + "],\"next_cursor\":" + next + extra + '}';
    }
};
}
int records(const std::vector<fs::path>& args) {
    if (args.size() != 3) return 2;
    auto profile = mods::load_profile(args[0]);
    if (!profile.profile) return 2;
    std::unique_ptr<Records> cache;
    RecordsReader input; std::string line;
    while (input.next(line)) {
        try {
            auto request = mods::steam::parse_json(line);
            const auto* method = request ? request->string_member("method") : nullptr;
            const auto* params = request ? request->member("params") : nullptr;
            if (!method || !params || params->kind != J::Kind::object) throw std::invalid_argument("invalid records request");
            if (!cache) cache = std::make_unique<Records>(*profile.profile, args[1], mods::steam::path_to_utf8(args[2]));
            auto result = *method == "records.get" ? cache->get(*params) : cache->page(*params);
            records_emit("{\"result\":" + result + '}');
        } catch (const std::invalid_argument& e) {
            records_emit("{\"error\":{\"code\":\"EAWR-MODS-SVC-0005\",\"message\":" + json_quote(e.what()) + "}}");
        } catch (const std::exception& e) {
            records_emit("{\"error\":{\"code\":\"EAWR-MODS-SVC-0011\",\"message\":" + json_quote(e.what()) + "}}");
        }
    }
    return 0;
}
}
