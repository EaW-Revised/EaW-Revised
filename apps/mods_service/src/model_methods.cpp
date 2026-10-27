#include "model_methods.hpp"
#include "loadcheck.hpp"
#include "eawr/mods/chain.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/mods/profile.hpp"
#include "install_discovery.hpp"
#include "launcher_state.hpp"
#ifdef _WIN32
#include "win_platform.hpp"
#endif
#include <algorithm>
#include <array>
#include <charconv>
#include <random>
#include <stdexcept>

namespace eawr::mods_service {
namespace {
using mods::steam::JsonValue;
using mods::steam::json_quote;
using mods::steam::path_from_utf8;
using mods::steam::path_to_utf8;
namespace fs = mods::steam::fs;
using J = JsonValue;
std::string encode(const J& v) {
    switch (v.kind) {
    case J::Kind::null: return "null";
    case J::Kind::boolean: return v.boolean ? "true" : "false";
    case J::Kind::number: {
        std::array<char, 64> buffer{};
        const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v.number);
        if (error != std::errc{}) throw std::runtime_error("cannot encode JSON number");
        return std::string(buffer.data(), end);
    }
    case J::Kind::string: return json_quote(v.string);
    case J::Kind::array: {
        std::string s = "[";
        for (const auto& item : v.array) { if (s.size() > 1) s += ','; s += encode(item); }
        return s + ']';
    }
    case J::Kind::object: {
        std::string s = "{";
        for (std::size_t i = 0; i < v.keys.size(); ++i) {
            if (i) s += ',';
            s += json_quote(v.keys[i]) + ':' + encode(v.values[i]);
        }
        return s + '}';
    }
    }
    return "null";
}
Reply invalid(std::string text) { return Reply::fail(kInvalidParams, std::move(text)); }
std::string warnings(const std::vector<mods::Warning>& values) {
    std::string out = "[";
    for (const auto& w : values) {
        if (out.size() > 1) out += ',';
        out += "{\"code\":" + json_quote(w.code) + ",\"message\":" + json_quote(w.message) + '}';
    }
    return out + ']';
}
Reply failure(const mods::Warning& w) {
    return Reply::fail(kInvalidParams, w.message, "{\"diagnostics\":" + warnings({w}) + '}');
}
bool paths(const J& params, std::string_view name, std::vector<fs::path>& out) {
    const auto* v = params.member(name);
    if (!v) return true;
    if (v->kind != J::Kind::array) return false;
    out.clear();
    for (const auto& item : v->array) {
        if (item.kind != J::Kind::string || item.string.find('\0') != std::string::npos) return false;
        const auto p = path_from_utf8(item.string);
        if (!p.is_absolute()) return false;
        out.push_back(p);
    }
    return true;
}
void refresh(ModelState& m) {
    m.catalog = mods::discover(m.discovery);
    mods::read_mod_metadata(m.catalog);
    m.discovered = true;
}
const mods::ModRecord* record(const ModelState& m, std::string_view key) {
    const auto it = std::find_if(m.catalog.records.begin(), m.catalog.records.end(), [&](const auto& r) {return r.key == key;});
    return it == m.catalog.records.end() ? nullptr : &*it;
}
Reply page(ModelState& m, const J& params, std::string method, std::string field,
           const std::vector<std::string>& rows, std::string extra = {}) {
    std::size_t limit = 100;
    if (const auto* v = params.member("limit")) {
        const auto count = json_count(v);
        if (!count || *count == 0 || *count > 500) return invalid("limit must be 1..500");
        limit = static_cast<std::size_t>(*count);
    }
    std::string signature = extra;
    for (const auto& row : rows) signature += row + '\n';
    const auto digest = core::sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(signature.data()), signature.size()));
    std::size_t offset = 0;
    if (const auto* v = params.member("cursor"); v && v->kind != J::Kind::null) {
        if (v->kind != J::Kind::string) return invalid("cursor must be a string or null");
        const auto it = m.pages.find(v->string);
        if (it == m.pages.end() || it->second.method != method || it->second.digest != digest)
            return invalid("unknown or stale cursor");
        offset = it->second.offset;
    }
    std::string out = "{" + json_quote(field) + ":[";
    std::size_t end = offset;
    while (end < rows.size() && end - offset < limit) {
        if (out.size() + rows[end].size() + extra.size() + 2048 > kLineLimit) break;
        if (end > offset) out += ',';
        out += rows[end++];
    }
    if (end == offset && end < rows.size()) return Reply::fail(kInternalError, "one row exceeds the page limit");
    out += "],\"next_cursor\":";
    if (end < rows.size()) {
        const auto token = std::to_string(std::random_device{}()) + "-" + std::to_string(++m.sequence);
        // Bound retained digests. Evicted tokens become stale.
        if (m.pages.size() >= 64) m.pages.erase(m.pages.begin());
        m.pages.emplace(token, Page{std::move(method), digest, end});
        out += json_quote(token);
    } else out += "null";
    return Reply::ok(out + extra + '}');
}
std::optional<fs::path> profile_path(ModelState& m, const std::string& id) {
    mods::Profile p; p.id = id; p.name = "validation";
    if (mods::validate_profile(p) || m.user_data.empty()) return {};
    std::error_code ec;
    auto base = fs::weakly_canonical(m.user_data, ec);
    if (ec) return {};
    auto folder = base;
    for (const auto* part : {"mods", "profiles"}) {
        folder /= part;
        const auto canonical = fs::weakly_canonical(folder, ec);
        if (ec || canonical != folder) return {};
    }
    const auto target = folder / (id + ".json");
    const auto status = fs::symlink_status(target, ec);
    if (!ec && fs::is_symlink(status)) return {};
    return target;
}
std::string profile_rows(const ModelState& m, const mods::Profile& p, bool& ok) {
    std::string rows = "[";
    for (const auto& layer : p.layers) {
        if (rows.size() > 1) rows += ',';
        const auto* r = record(m, layer.key);
        std::error_code ec;
        const bool saved_root_exists = fs::is_directory(path_from_utf8(layer.root), ec);
        const bool catalog_root_exists = r && fs::is_directory(path_from_utf8(r->root), ec);
        auto state = r ? r->state : mods::ModState::installed;
        if (!saved_root_exists && !catalog_root_exists && state != mods::ModState::downloading) state = mods::ModState::missing;
        const bool installed = state == mods::ModState::installed || state == mods::ModState::needs_update;
        ok = ok && installed;
        rows += "{\"key\":" + json_quote(layer.key) + ",\"name\":" + json_quote(r ? r->name : layer.key)
            + ",\"why\":" + json_quote(p.order_source) + ",\"state\":" + json_quote(mods::to_string(state)) + '}';
    }
    return rows + ']';
}
}
ModelState::ModelState() : user_data(launcher::default_user_data_dir()) {
    discovery.steam_roots = mods::native_profile_steam_reader().roots();
}
namespace {
std::string install_kind(const fs::path& root) {
    // The shared install search treats validated steamapps/common children as Steam candidates.
    const auto common = root.parent_path();
    return mods::steam::lower_ascii(path_to_utf8(common.filename())) == "common"
        && mods::steam::lower_ascii(path_to_utf8(common.parent_path().filename())) == "steamapps" ? "steam" : "other";
}
}
Reply method_game_find(Service& service, const J& params) {
    auto& m = service.model;
    fs::path root;
    std::string kind = "none";
    std::vector<mods::Warning> diagnostics;
    if (const auto* hint = params.member("hint"); hint && hint->kind != J::Kind::null) {
        if (hint->kind != J::Kind::string || hint->string.find('\0') != std::string::npos) return invalid("hint must be a folder string");
        const auto hint_path = path_from_utf8(hint->string);
        if (!hint_path.is_absolute()) return invalid("hint must be an absolute folder path");
        const auto validation = launcher::validate_install(hint_path);
        if (validation.ok) { root = validation.root; kind = install_kind(root); }
        else diagnostics.push_back({"EAWR-MODS-SVC-0012", validation.error});
    } else {
        const auto saved = m.user_data.empty() ? launcher::LauncherState{} : launcher::load_state(m.user_data);
        if (saved.game_root) {
            const auto validation = launcher::validate_install(*saved.game_root);
            if (validation.ok) { root = validation.root; kind = install_kind(root); }
        }
#ifdef _WIN32
        if (root.empty()) {
            const auto registry = launcher::make_windows_registry();
            const auto installs = launcher::discover_installs(*registry);
            if (!installs.empty()) { root = installs.front().root; kind = installs.front().source == "Steam" ? "steam" : "other"; }
        }
#endif
        if (root.empty()) diagnostics.push_back({"EAWR-MODS-SVC-0012", "No Forces of Corruption installation found"});
    }
    m.game_root = root;
    m.discovery.mods_folder.reset();
    if (!root.empty()) {
        const auto corruption = mods::steam::find_child_ci(root, "corruption");
        if (corruption) m.discovery.mods_folder = mods::steam::find_child_ci(*corruption, "Mods").value_or(*corruption / "Mods");
    }
    m.discovered = false;
    m.pages.clear();
    return Reply::ok("{\"root\":" + (root.empty() ? std::string("null") : json_quote(path_to_utf8(root)))
        + ",\"kind\":" + json_quote(kind) + ",\"diagnostics\":" + warnings(diagnostics) + '}');
}
Reply method_mods_changes(Service& service, const J& params) {
    const auto* previous = params.member("previous");
    if (previous && previous->kind != J::Kind::string)
        return invalid("previous must be a string");
    const auto snapshot = mods::workshop_snapshot(service.model.discovery);
    const bool changed = snapshot.complete && previous && previous->string != snapshot.revision;
    return Reply::ok("{\"revision\":" + json_quote(snapshot.revision)
        + ",\"changed\":" + (changed ? "true" : "false")
        + ",\"complete\":" + (snapshot.complete ? "true" : "false") + '}');
}
Reply method_mods_discover(Service& service, const J& params) {
    auto& m = service.model;
    auto input = m.discovery;
    input.walk_trees = true;
    if (!paths(params, "steam_roots", input.steam_roots) || !paths(params, "steam_libraries", input.steam_libraries)
        || !paths(params, "manual_folders", input.manual_folders)) return invalid("discovery folders must be arrays of absolute paths");
    if (const auto* v = params.member("walk_trees")) {
        if (v->kind != J::Kind::boolean) return invalid("walk_trees must be boolean");
        input.walk_trees = v->boolean;
    }
    m.discovery = std::move(input);
    refresh(m);
    std::vector<std::string> rows;
    for (const auto& r : m.catalog.records) rows.push_back(mods::record_to_json(r));
    return page(m, params, "mods.discover", "records", rows, ",\"warnings\":" + warnings(m.catalog.warnings));
}
Reply method_chain_resolve(Service& service, const J& params) {
    auto& m = service.model;
    const auto* leaf = params.string_member("leaf");
    if (!leaf || leaf->empty()) return invalid("leaf is required");
    if (!m.discovered) refresh(m);
    mods::KnownChains known;
    if (const auto* k = params.member("known_chains")) {
        if (k->kind != J::Kind::object) return invalid("known_chains must be the shipped JSON object");
        known = mods::parse_known_chains(encode(*k));
        if (!known.warnings.empty()) return failure(known.warnings.front());
    }
    mods::PlayerEdges edges;
    if (const auto* e = params.member("edges")) {
        if (e->kind != J::Kind::array) return invalid("edges must be an array");
        for (const auto& edge : e->array) {
            const auto* child = edge.string_member("child"); const auto* parent = edge.string_member("parent");
            if (!child || !parent || child->empty() || parent->empty()) return invalid("edges need child and parent keys");
            edges[*child].push_back(*parent);
        }
    }
    auto catalog = m.catalog;
    mods::apply_known_chains(catalog, known);
    const auto chain = mods::resolve_chain(*leaf, catalog.records, known, edges);
    std::string out = "{\"layers\":[";
    for (const auto& key : chain.layers) {
        if (out.back() != '[') out += ',';
        const auto* r = record(m, key);
        out += "{\"key\":" + json_quote(key) + ",\"root\":" + json_quote(r ? r->root : "") + '}';
    }
    out += "],\"rows\":[";
    for (const auto& row : chain.rows) {
        if (out.back() != '[') out += ',';
        const auto it = std::find_if(catalog.records.begin(), catalog.records.end(), [&](const auto& r){ return r.key == row.key; });
        out += "{\"key\":" + json_quote(row.key) + ",\"name\":" + json_quote(it == catalog.records.end() ? row.key : it->name)
            + ",\"why\":" + json_quote(row.why) + ",\"state\":" + json_quote(mods::to_string(row.state)) + '}';
    }
    out += "],\"edges\":[";
    for (const auto& e : chain.edges) {
        if (out.back() != '[') out += ',';
        out += "{\"child\":" + json_quote(e.child) + ",\"parent\":" + json_quote(e.parent) + ",\"why\":" + json_quote(e.why) + '}';
    }
    out += "],\"diagnostics\":[";
    for (const auto& d : chain.diagnostics) {
        if (out.back() != '[') out += ',';
        out += "{\"code\":" + json_quote(d.code) + ",\"key\":" + json_quote(d.key) + ",\"message\":" + json_quote(d.message) + '}';
    }
    return Reply::ok(out + "],\"launchable\":" + (chain.launchable ? "true" : "false") + '}');
}
Reply method_chain_check(Service& service, const J& params) {
    const auto* input = params.member("layers");
    if (!input || input->kind != J::Kind::array) return invalid("layers must be an array of key objects");
    std::vector<std::string> layers, accepted;
    for (const auto& layer : input->array) {
        const auto* key = layer.string_member("key");
        if (!key || key->empty()) return invalid("layers need nonempty keys");
        layers.push_back(*key);
    }
    if (const auto* a = params.member("accepted_warnings")) {
        if (a->kind != J::Kind::array) return invalid("accepted_warnings must be an array of codes");
        for (const auto& code : a->array) {
            if (code.kind != J::Kind::string || code.string.empty()) return invalid("accepted_warnings needs nonempty strings");
            accepted.push_back(code.string);
        }
    }
    mods::KnownChains known;
    if (const auto* k = params.member("known_chains")) {
        if (k->kind != J::Kind::object) return invalid("known_chains must be the shipped JSON object");
        known = mods::parse_known_chains(encode(*k));
        if (!known.warnings.empty()) return failure(known.warnings.front());
    }
    auto& m = service.model;
    if (!m.discovered) refresh(m);
    const auto check = mods::check_order(layers, m.catalog.records, known, accepted);
    auto rows = [](const auto& values) {
        std::string out = "[";
        for (const auto& d : values) {
            if (out.size() > 1) out += ',';
            out += "{\"code\":" + json_quote(d.code) + ",\"key\":" + json_quote(d.key)
                + ",\"message\":" + json_quote(d.message) + '}';
        }
        return out + ']';
    };
    return Reply::ok("{\"warnings\":" + rows(check.warnings) + ",\"diagnostics\":" + rows(check.diagnostics)
        + ",\"launchable\":" + (check.launchable ? "true" : "false") + '}');
}
Reply method_profile_save(Service& service, const J& params) {
    const auto* value = params.member("profile");
    if (!value || value->kind != J::Kind::object) return invalid("profile object is required");
    const auto parsed = mods::profile_from_json(encode(*value));
    if (parsed.error) return failure(*parsed.error);
    if (const auto error = mods::save_profile(service.model.user_data, *parsed.profile)) return failure(*error);
    service.model.pages.clear();
    return Reply::ok("{\"profile\":" + mods::profile_to_json(*parsed.profile) + '}');
}
Reply method_profile_load(Service& service, const J& params) {
    auto& m = service.model;
    const auto* id = params.string_member("id");
    const auto file = id ? profile_path(m, *id) : std::nullopt;
    if (!file) return invalid("invalid profile id or unsafe profile folder");
    auto p = mods::load_profile(*file);
    if (p.error) return failure(*p.error);
    if (p.profile->id != *id) return invalid("profile id differs from file name");
    if (!m.discovered) refresh(m);
    // Core re-resolution consults the session sources when a saved root has disappeared.
    auto resolved = mods::resolve_profile(*p.profile, m.discovery);
    if (resolved.profile) p = std::move(resolved);
    bool ok = !resolved.error;
    const auto rows = profile_rows(m, *p.profile, ok);
    return Reply::ok("{\"profile\":" + mods::profile_to_json(*p.profile) + ",\"rows\":" + rows
        + ",\"status\":" + json_quote(ok ? "ok" : "missing") + '}');
}
Reply method_profile_delete(Service& service, const J& params) {
    auto& m = service.model;
    const auto* id = params.string_member("id");
    const auto file = id ? profile_path(m, *id) : std::nullopt;
    if (!file) return invalid("invalid profile id or unsafe profile folder");
    std::error_code ec;
    if (!fs::is_regular_file(*file, ec)) return failure({std::string(mods::codes::profile_io), "Profile is not a file"});
    service.loadcheck().forget_profile(*id);
    const bool removed = fs::remove(*file, ec);
    if (ec || !removed) return failure({std::string(mods::codes::profile_io), "Cannot delete profile"});
    m.pages.clear();
    return Reply::ok("{\"deleted\":true}");
}
Reply method_profile_list(Service& service, const J& params) {
    auto& m = service.model;
    if (!m.discovered) refresh(m);
    std::vector<fs::path> files;
    std::error_code ec;
    const auto probe = profile_path(m, "probe");
    if (!probe) return invalid("unsafe profile folder or unavailable user data");
    for (fs::directory_iterator it(probe->parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".json") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    std::vector<std::string> rows;
    for (const auto& file : files) {
        const auto id = path_to_utf8(file.stem());
        auto p = profile_path(m, id) ? mods::load_profile(file) : mods::ProfileResult{};
        const bool valid = p.profile.has_value() && p.profile->id == id;
        bool ok = valid;
        if (valid) (void)profile_rows(m, *p.profile, ok);
        rows.push_back("{\"id\":" + json_quote(id) + ",\"name\":" + json_quote(p.profile ? p.profile->name : id)
            + ",\"status\":" + json_quote(!valid ? "invalid" : ok ? "ok" : "missing") + '}');
    }
    return page(m, params, "profile.list", "profiles", rows);
}
}
