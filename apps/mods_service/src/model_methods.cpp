#include "model_methods.hpp"
#include "loadcheck.hpp"
#include "records.hpp"
#include "profile_locks.hpp"
#include "eawr/mods/chain.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/core/text.hpp"
#include "eawr/mods/profile.hpp"
#include "install_discovery.hpp"
#include "launcher_state.hpp"
#include "eawr/presentation/ui/settings.hpp"
#include "eawr/platform/ui_files.hpp"
#ifdef _WIN32
#include "win_platform.hpp"
#endif
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <random>
#include <stdexcept>
#include <system_error>

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
// <user data>/mods/cache/content, only when every level resolves beneath the user-data root without redirects
// (the rule loadcheck's cache folder follows); otherwise digests stay in memory.
fs::path content_cache_folder(const fs::path& user_data) {
    if (user_data.empty()) return {};
    std::error_code ec;
    auto folder = fs::weakly_canonical(user_data, ec);
    if (ec) return {};
    for (const char* part : {"mods", "cache", "content"}) {
        folder /= part;
        if (fs::weakly_canonical(folder, ec) != folder || ec) return {};
    }
    return folder;
}
mods::ContentDigests& content_digests(ModelState& m) {
    if (!m.digests) {
        mods::ContentDigestOptions options;
        options.cache_folder = content_cache_folder(m.user_data);
        // The manager's digests are background work: two hashing threads (not eight), and nothing at all while a game
        // instance runs. The running game hashes the layers it mounts itself into the same cache folder; the paused job
        // takes that result when it is there (#6588).
        options.worker_count = 2;
        options.background_io = false;   // below-normal CPU only: background I/O mode is ~6x slower and ~4x the kernel CPU
        options.pause_while = [user_data = m.user_data] {
            const ProfileLocks locks = read_profile_locks(user_data);
            return !locks.locks.empty() || !locks.unreadable.empty();
        };
        m.digests = std::make_shared<mods::ContentDigests>(std::move(options));
    }
    return *m.digests;
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
namespace ui = presentation::ui;
ui::PlayerSettings revised_settings(const ModelState& m) {
    const auto text = platform::read_text_file(m.user_data / "eawr-settings.cfg");
    return text ? ui::parse_settings(*text).settings : ui::PlayerSettings{};
}
std::string revised_row(const ModelState& m) {
    const auto settings = revised_settings(m);
    const auto override_path = settings.revised_layer_path ? path_from_utf8(*settings.revised_layer_path) : fs::path{};
    const bool bad_override = !override_path.empty() && !override_path.is_absolute();
    const auto root = !override_path.empty() && !bad_override ? override_path : m.player_dir / "Revised";
    std::error_code ec;
    const bool present = fs::is_directory(root / "Data", ec);
    return "{\"key\":\"builtin:revised\",\"kind\":\"builtin\",\"name\":\"Revised\",\"enabled\":"
        + std::string(settings.revised_layer_enabled ? "true" : "false") + ",\"root\":"
        + (present ? json_quote(path_to_utf8(root)) : "null") + ",\"why\":\"builtin\",\"state\":"
        + json_quote(present ? "installed" : "missing") + ",\"note\":"
        + (bad_override ? json_quote("Relative Revised layer path ignored")
            : present ? "null" : json_quote("Revised data folder is missing")) + '}';
}
bool strip_builtin_layers(mods::Profile& profile) {
    std::vector<std::size_t> removed;
    for (std::size_t i = 0; i < profile.layers.size(); ++i)
        if (profile.layers[i].key.starts_with("builtin:")) removed.push_back(i);
    if (removed.empty()) return false;
    for (auto& separator : profile.separators)
        separator.before -= static_cast<std::size_t>(std::count_if(removed.begin(), removed.end(),
            [&](std::size_t i) { return i < separator.before; }));
    std::erase_if(profile.layers, [](const auto& layer) { return layer.key.starts_with("builtin:"); });
    profile.accepted = {};
    return true;
}
mods::ProfileResult load_service_profile(const fs::path& file, bool& stripped) {
    auto result = mods::load_profile(file);
    if (result.profile) {
        stripped = strip_builtin_layers(*result.profile);
        return result;
    }
    // Older rows may have a null/missing root. Remove built-ins before core validation.
    const auto bytes = platform::read_text_file(file);
    auto json = bytes ? mods::steam::parse_json(*bytes) : std::nullopt;
    if (!json || json->kind != J::Kind::object || !json->member("format") || !json->member("version")) return result;
    auto field = [&](std::string_view name) -> J* {
        for (std::size_t i = 0; i < json->keys.size(); ++i)
            if (json->keys[i] == name) return &json->values[i];
        return nullptr;
    };
    auto* layers = field("layers");
    if (!layers || layers->kind != J::Kind::array) return result;
    std::vector<std::size_t> removed;
    for (std::size_t i = 0; i < layers->array.size(); ++i)
        if (const auto* key = layers->array[i].string_member("key"); key && key->starts_with("builtin:")) removed.push_back(i);
    if (removed.empty()) return result;
    if (auto* separators = field("separators"); separators && separators->kind == J::Kind::array) {
        for (auto& separator : separators->array) {
            for (std::size_t i = 0; i < separator.keys.size(); ++i) {
                if (separator.keys[i] != "before" || separator.values[i].kind != J::Kind::number) continue;
                auto& before = separator.values[i].number;
                if (before < 0 || before > static_cast<double>(layers->array.size())) continue;
                before -= static_cast<double>(std::count_if(removed.begin(), removed.end(),
                    [&](std::size_t index) { return static_cast<double>(index) < before; }));
            }
        }
    }
    std::erase_if(layers->array, [](const auto& layer) {
        const auto* key = layer.string_member("key"); return key && key->starts_with("builtin:");
    });
    auto sanitized = mods::profile_from_json(encode(*json));
    if (sanitized.profile) { sanitized.profile->accepted = {}; stripped = true; return sanitized; }
    return result;
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
    if (rows.size() > 1) rows += ',';
    rows += revised_row(m);
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
Reply method_settings_set_builtin_layer(Service& service, const J& params) {
    const auto* key = params.string_member("key");
    const auto* enabled = params.member("enabled");
    if (!key || *key != "builtin:revised" || !enabled || enabled->kind != J::Kind::boolean)
        return invalid("key must be builtin:revised and enabled must be boolean");
    if (service.model.user_data.empty()) return Reply::fail(kInternalError, "settings folder unavailable");
    const auto target = service.model.user_data / "eawr-settings.cfg";
    std::error_code ec;
    fs::create_directories(service.model.user_data, ec);
    if (ec) return Reply::fail(kInternalError, "cannot create settings folder");
    fs::path staging;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = service.model.user_data / (".settings-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(attempt));
        if (fs::create_directory(candidate, ec)) { staging = candidate; break; }
        ec.clear();
    }
    if (staging.empty()) return Reply::fail(kInternalError, "cannot reserve settings temporary file");
    // Read after reserving the temporary location to minimize the read-modify-write window.
    const auto original = platform::read_text_file(target);
    const bool exists = fs::exists(target, ec);
    if (ec || (!original && exists)) { fs::remove(staging, ec); return Reply::fail(kInternalError, "cannot read settings"); }
    const auto loaded = original ? ui::parse_settings(*original) : ui::SettingsLoad{};
    if (original && (loaded.defaulted || std::any_of(loaded.notes.begin(), loaded.notes.end(), [](const auto& note) {
            return !note.starts_with("unknown setting ignored:") && note != "unknown online setting ignored";
        }))) {
        fs::remove(staging, ec);
        return Reply::fail("EAWR-MODS-SVC-0015", "settings file is invalid or unsupported; it was not changed");
    }
    std::string text = original.value_or(ui::serialize_settings(ui::PlayerSettings{}));
    std::string updated;
    for (std::size_t pos = 0; pos < text.size();) {
        const auto end = text.find('\n', pos);
        const auto length = (end == std::string::npos ? text.size() : end + 1) - pos;
        const std::string_view line(text.data() + pos, length);
        const auto equals = line.find('=');
        if (equals == std::string_view::npos || core::trim_view(line.substr(0, equals)) != "revised_layer_enabled")
            updated.append(line);
        pos += length;
    }
    if (!updated.empty() && updated.back() != '\n') updated += '\n';
    updated += std::string("revised_layer_enabled=") + (enabled->boolean ? "1\n" : "0\n");
    const auto temporary = staging / "settings.cfg";
    bool written = false;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << updated;
        output.close();
        written = static_cast<bool>(output);
    }
    if (written) fs::rename(temporary, target, ec);
    else ec = std::make_error_code(std::errc::io_error);
    const bool saved = !ec;
    std::error_code ignored;
    fs::remove(temporary, ignored);
    fs::remove(staging, ignored);
    if (!saved) return Reply::fail(kInternalError, "cannot save settings");
    return Reply::ok("{\"key\":\"builtin:revised\",\"enabled\":" + std::string(enabled->boolean ? "true" : "false") + '}');
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
    service.records().stop();
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
    // Cache hits only: asking for the discovered records never starts a hashing job (chain.resolve does).
    for (auto& r : m.catalog.records) content_digests(m).apply(r, false);
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
            + ",\"why\":" + json_quote(row.why) + ",\"state\":" + json_quote(mods::to_string(row.state));
        // A mounted layer gets its content digest: ready from the cache, else pending with a background job queued.
        if (it != catalog.records.end() && std::find(chain.layers.begin(), chain.layers.end(), row.key) != chain.layers.end()) {
            out += ",\"content\":" + mods::content_digest_to_json(content_digests(m).lookup(*it, true));
        }
        out += '}';
    }
    if (out.back() != '[') out += ',';
    out += revised_row(m);
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
std::string instance_list(const std::vector<int>& instances) {
    std::string out = "[";
    for (const int instance : instances) {
        if (out.size() > 1) out += ',';
        out += std::to_string(instance);
    }
    return out + ']';
}
// A profile a running game instance holds is read-only for everyone (UI or not): edits, renames, layer changes
// and deletes are refused until the instance exits. The lock comes from the launcher's state files, not the caller.
static std::optional<Reply> refuse_if_locked(const ModelState& m, const std::string& id) {
    const ProfileBlock block = profile_block_for(m.user_data, id);
    if (!block.blocked()) return std::nullopt;
    std::string data = "{\"profile\":" + json_quote(id) + ",\"instances\":" + instance_list(block.instances);
    if (!block.unreadable.empty()) data += ",\"unreadable\":" + instance_list(block.unreadable);
    data += '}';
    if (!block.instances.empty()) {
        std::string where;
        for (const int instance : block.instances) where += (where.empty() ? "" : ", ") + std::to_string(instance);
        return Reply::fail(kProfileLocked, "profile \"" + id + "\" is running in game instance " + where + " and cannot be changed", data);
    }
    // Fail closed: the state of a game instance exists but cannot be read, so it is unknown whether it runs this profile.
    std::string where;
    for (const int instance : block.unreadable) where += (where.empty() ? "" : ", ") + std::to_string(instance);
    return Reply::fail(kProfileLocked, "the running-game state of instance " + where + " could not be read, so profile \"" + id
                       + "\" cannot be changed until that state is fixed or removed", data);
}
Reply method_profile_locks(Service& service, const J&) {
    const ProfileLocks all = read_profile_locks(service.model.user_data);
    std::string rows = "[";
    for (const ProfileLock& lock : all.locks) {
        if (rows.size() > 1) rows += ',';
        rows += "{\"profile\":" + json_quote(lock.profile) + ",\"instances\":" + instance_list(lock.instances) + '}';
    }
    return Reply::ok("{\"locked\":" + rows + "],\"unreadable\":" + instance_list(all.unreadable) + '}');
}
Reply method_profile_save(Service& service, const J& params) {
    const auto* value = params.member("profile");
    if (!value || value->kind != J::Kind::object) return invalid("profile object is required");
    if (const auto* layers = value->member("layers"); layers && layers->kind == J::Kind::array)
        for (const auto& layer : layers->array)
            if (const auto* key = layer.string_member("key"); key && key->starts_with("builtin:"))
                return invalid("built-in layers cannot be saved in profiles");
    const auto parsed = mods::profile_from_json(encode(*value));
    if (parsed.error) return failure(*parsed.error);
    if (const auto locked = refuse_if_locked(service.model, parsed.profile->id)) return *locked;
    service.records().forget_profile(parsed.profile->id);
    if (const auto error = mods::save_profile(service.model.user_data, *parsed.profile)) return failure(*error);
    service.model.pages.clear();
    return Reply::ok("{\"profile\":" + mods::profile_to_json(*parsed.profile) + '}');
}
Reply method_profile_load(Service& service, const J& params) {
    auto& m = service.model;
    const auto* id = params.string_member("id");
    const auto file = id ? profile_path(m, *id) : std::nullopt;
    if (!file) return invalid("invalid profile id or unsafe profile folder");
    bool stripped = false;
    auto p = load_service_profile(*file, stripped);
    if (p.error) return failure(*p.error);
    if (p.profile->id != *id) return invalid("profile id differs from file name");
    if (!m.discovered) refresh(m);
    // Core re-resolution consults the session sources when a saved root has disappeared.
    auto resolved = mods::resolve_profile(*p.profile, m.discovery);
    if (resolved.profile) p = std::move(resolved);
    bool ok = !resolved.error;
    const auto rows = profile_rows(m, *p.profile, ok);
    return Reply::ok("{\"profile\":" + mods::profile_to_json(*p.profile) + ",\"rows\":" + rows
        + ",\"status\":" + json_quote(ok ? "ok" : "missing")
        + (stripped ? ",\"notes\":[\"Built-in layers were removed from the saved profile\"]" : "") + '}');
}
Reply method_profile_delete(Service& service, const J& params) {
    auto& m = service.model;
    const auto* id = params.string_member("id");
    const auto file = id ? profile_path(m, *id) : std::nullopt;
    if (!file) return invalid("invalid profile id or unsafe profile folder");
    std::error_code ec;
    if (!fs::is_regular_file(*file, ec)) return failure({std::string(mods::codes::profile_io), "Profile is not a file"});
    if (const auto locked = refuse_if_locked(m, *id)) return *locked;
    service.loadcheck().forget_profile(*id);
    service.records().forget_profile(*id);
    const bool removed = fs::remove(*file, ec);
    if (ec || !removed) return failure({std::string(mods::codes::profile_io), "Cannot delete profile"});
    m.pages.clear();
    return Reply::ok("{\"deleted\":true}");
}
std::optional<mods::Profile> records_profile(Service& s, const J& params, Reply& error) {
    const auto* id = params.string_member("profile");
    if (!id) { error = invalid("profile must be an id"); return {}; }
    J args; args.kind = J::Kind::object; args.keys = {"id"};
    J value; value.kind = J::Kind::string; value.string = *id; args.values = {value};
    const auto loaded = method_profile_load(s, args);
    if (!loaded.error_code.empty()) { error = loaded; return {}; }
    const auto doc = mods::steam::parse_json(loaded.result);
    const auto* status = doc ? doc->string_member("status") : nullptr;
    if (!status || *status != "ok" || doc->member("notes")) { error = invalid("profile is missing or unclean"); return {}; }
    const auto* json = doc->member("profile");
    auto parsed = json ? mods::profile_from_json(encode(*json)) : mods::ProfileResult{};
    if (!parsed.profile) { error = invalid("profile unavailable"); return {}; }
    // A read-only inspection must not silently follow a relocated saved layer.
    const auto file = profile_path(s.model, *id);
    const auto saved = file ? mods::load_profile(*file) : mods::ProfileResult{};
    if (!saved.profile || saved.profile->layers != parsed.profile->layers) { error = invalid("profile roots are stale"); return {}; }
    std::error_code ec;
    if (s.model.game_root.empty() || !fs::is_directory(s.model.game_root / "corruption/Data", ec)
        || !fs::is_directory(s.model.game_root / "GameData/Data", ec)) { error = invalid("game root unavailable"); return {}; }
    return std::move(parsed.profile);
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
        bool stripped = false;
        auto p = profile_path(m, id) ? load_service_profile(file, stripped) : mods::ProfileResult{};
        const bool valid = p.profile.has_value() && p.profile->id == id;
        bool ok = valid;
        if (valid) (void)profile_rows(m, *p.profile, ok);
        rows.push_back("{\"id\":" + json_quote(id) + ",\"name\":" + json_quote(p.profile ? p.profile->name : id)
            + ",\"status\":" + json_quote(!valid ? "invalid" : ok ? "ok" : "missing") + '}');
    }
    return page(m, params, "profile.list", "profiles", rows);
}
}
