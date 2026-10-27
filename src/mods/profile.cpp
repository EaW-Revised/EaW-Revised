#include "eawr/mods/profile.hpp"
#include "profile_io.hpp"
#include "eawr/mods/steam/library_folders.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <cstdlib>
#include <set>

namespace eawr::mods {
namespace {
using steam::JsonValue;
using steam::json_quote;
namespace fs = steam::fs;
Warning failure(std::string_view code, std::string message) { return {std::string(code), std::move(message)}; }
ProfileResult failed(std::string_view code, std::string message) { return {{}, failure(code, std::move(message))}; }
bool safe_id(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    for (char c : id) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    const auto lower = steam::lower_ascii(id);
    if (lower == "con" || lower == "prn" || lower == "aux" || lower == "nul") return false;
    return !(lower.size() == 4 && (lower.starts_with("com") || lower.starts_with("lpt"))
        && lower[3] >= '0' && lower[3] <= '9');
}
bool directory(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::is_directory(steam::path_from_utf8(path), ec);
}
bool text_field(const JsonValue& object, std::string_view key, std::string& out, bool required = false) {
    const auto* value = object.member(key);
    if (!value) return !required;
    if (value->kind != JsonValue::Kind::string) return false;
    out = value->string;
    return true;
}
bool integer(const JsonValue& value, double minimum, double maximum) {
    return value.kind == JsonValue::Kind::number && std::isfinite(value.number)
        && value.number >= minimum && value.number <= maximum && std::floor(value.number) == value.number;
}
}

#ifndef _WIN32
bool detail::replace_profile_file(const steam::fs::path& from, const steam::fs::path& to) {
    std::error_code ec;
    fs::rename(from, to, ec);
    return !ec;
}
const ProfileSteamReader& native_profile_steam_reader() {
    class HomeSteamReader final : public ProfileSteamReader {
    public:
        std::vector<fs::path> roots() const override {
            if (const char* home = std::getenv("HOME")) return {
                fs::path(home) / ".steam/steam", fs::path(home) / ".local/share/Steam",
                fs::path(home) / ".var/app/com.valvesoftware.Steam/data/Steam"};
            return {};
        }
    };
    static const HomeSteamReader reader;
    return reader;
}
#endif

std::optional<Warning> validate_profile(const Profile& p) {
    if (!safe_id(p.id)) return failure(codes::profile_id, "Profile id must be a portable file name using letters, digits, '-' or '_'");
    if (p.name.empty() || (p.order_source != "chain" && p.order_source != "player"))
        return failure(codes::profile_fields, "Profile requires a name and order_source chain or player");
    std::set<std::string> keys;
    for (const auto& layer : p.layers) {
        const auto path = steam::path_from_utf8(layer.root);
        if (layer.key.empty() || layer.key.find('\0') != std::string::npos || !keys.insert(layer.key).second
            || path.empty() || !path.is_absolute() || layer.root.find_first_of(";\n\r") != std::string::npos
            || layer.root.find('\0') != std::string::npos)
            return failure(codes::profile_layer, "Invalid or duplicate layer: " + layer.key);
    }
    for (const auto& separator : p.separators) if (separator.before > p.layers.size())
        return failure(codes::profile_fields, "Separator index exceeds layer count");
    if (p.launch.mode != "front-end" || (p.launch.render_thread != "safe" && p.launch.render_thread != "separate")
        || (p.launch.frame_rate_limit && *p.launch.frame_rate_limit < -1))
        return failure(codes::profile_launch, "Invalid launch mode, render_thread or frame_rate_limit (-1..INT_MAX)");
    return {};
}

std::string profile_to_json(const Profile& p) {
    std::string out = "{\"format\":\"eawr-mod-profile\",\"version\":1,\"id\":" + json_quote(p.id)
        + ",\"name\":" + json_quote(p.name) + ",\"layers\":[";
    for (std::size_t i = 0; i < p.layers.size(); ++i) {
        if (i) out += ',';
        out += "{\"key\":" + json_quote(p.layers[i].key) + ",\"root\":" + json_quote(p.layers[i].root) + '}';
    }
    out += "],\"order_source\":" + json_quote(p.order_source) + ",\"separators\":[";
    for (std::size_t i = 0; i < p.separators.size(); ++i) {
        if (i) out += ',';
        out += "{\"before\":" + std::to_string(p.separators[i].before) + ",\"label\":" + json_quote(p.separators[i].label) + '}';
    }
    return out + "],\"launch\":{\"mode\":" + json_quote(p.launch.mode) + ",\"render_thread\":"
        + json_quote(p.launch.render_thread) + ",\"frame_rate_limit\":"
        + (p.launch.frame_rate_limit ? std::to_string(*p.launch.frame_rate_limit) : "null")
        + "},\"accepted\":{\"chain_fingerprint\":" + json_quote(p.accepted.chain_fingerprint)
        + ",\"load_check\":" + json_quote(p.accepted.load_check) + "}}";
}

ProfileResult profile_from_json(std::string_view text) {
    const auto value = steam::parse_json(text);
    if (!value || value->kind != JsonValue::Kind::object) return failed(codes::profile_json, "Expected a profile JSON object");
    const auto& v = *value;
    const auto* format = v.member("format");
    const auto* version = v.member("version");
    if ((format && (format->kind != JsonValue::Kind::string || format->string != "eawr-mod-profile"))
        || (version && (!integer(*version, 1, 1)))) return failed(codes::profile_version, "Expected eawr-mod-profile version 1");
    Profile p;
    if (!text_field(v, "id", p.id, true)) return failed(codes::profile_id, "Missing or invalid profile id");
    if (!text_field(v, "name", p.name, true) || !text_field(v, "order_source", p.order_source))
        return failed(codes::profile_fields, "Invalid name or order_source");
    const auto* layers = v.member("layers");
    if (!layers || layers->kind != JsonValue::Kind::array) return failed(codes::profile_layer, "Expected layers array");
    for (const auto& item : layers->array) {
        ProfileLayer layer;
        if (item.kind != JsonValue::Kind::object || !text_field(item, "key", layer.key, true)
            || !text_field(item, "root", layer.root, true)) return failed(codes::profile_layer, "Invalid layer key/root");
        p.layers.push_back(std::move(layer));
    }
    if (const auto* items = v.member("separators")) {
        if (items->kind != JsonValue::Kind::array) return failed(codes::profile_fields, "Expected separators array");
        for (const auto& item : items->array) {
            const auto* before = item.member("before");
            ProfileSeparator s;
            if (item.kind != JsonValue::Kind::object || !before || !integer(*before, 0, static_cast<double>(p.layers.size()))
                || !text_field(item, "label", s.label, true)) return failed(codes::profile_fields, "Invalid separator");
            s.before = static_cast<std::size_t>(before->number);
            p.separators.push_back(std::move(s));
        }
    }
    if (const auto* launch = v.member("launch")) {
        if (launch->kind != JsonValue::Kind::object || !text_field(*launch, "mode", p.launch.mode)
            || !text_field(*launch, "render_thread", p.launch.render_thread)) return failed(codes::profile_launch, "Invalid launch settings");
        if (const auto* cap = launch->member("frame_rate_limit")) {
            if (cap->kind != JsonValue::Kind::null) {
                if (!integer(*cap, -1, std::numeric_limits<int>::max())) return failed(codes::profile_launch, "Invalid frame_rate_limit (-1..INT_MAX)");
                p.launch.frame_rate_limit = static_cast<int>(cap->number);
            }
        }
    }
    if (const auto* a = v.member("accepted")) {
        if (a->kind != JsonValue::Kind::object || !text_field(*a, "chain_fingerprint", p.accepted.chain_fingerprint)
            || !text_field(*a, "load_check", p.accepted.load_check)) return failed(codes::profile_fields, "Invalid accepted state");
    }
    if (const auto error = validate_profile(p)) return {{}, error};
    return {std::move(p), {}};
}

ProfileResult load_profile(const steam::fs::path& file) {
    const auto text = steam::read_text_file(file);
    if (!text) return failed(codes::profile_io, "Cannot read profile: " + steam::path_to_utf8(file));
    const auto json = steam::parse_json(*text);
    if (json && (!json->member("format") || !json->member("version")))
        return failed(codes::profile_version, "Profile file requires format and version");
    return profile_from_json(*text);
}

std::optional<Warning> save_profile(const steam::fs::path& user_data, const Profile& p) {
    if (const auto error = validate_profile(p)) return error;
    if (user_data.empty()) return failure(codes::profile_io, "User-data folder is required");
    std::error_code ec;
    fs::create_directories(user_data, ec);
    if (ec) return failure(codes::profile_io, "Cannot create user-data folder");
    auto folder = fs::canonical(user_data, ec);
    if (ec) return failure(codes::profile_io, "Cannot resolve user-data folder");
    for (const auto* part : {"mods", "profiles"}) {
        folder /= part;
        const auto status = fs::symlink_status(folder, ec);
        if (!ec && fs::is_symlink(status)) return failure(codes::profile_io, "Profile folder must not be a symbolic link");
        ec.clear();
        fs::create_directories(folder, ec);
        if (ec) return failure(codes::profile_io, "Cannot create profile folder");
        const auto canonical = fs::canonical(folder, ec);
        if (ec || canonical != folder) return failure(codes::profile_io, "Profile folder escapes user data");
    }
    const auto target = folder / (p.id + ".json");
    // A unique sibling directory reserves the temporary name across threads and processes.
    fs::path staging;
    for (unsigned attempt = 0; attempt != 100; ++attempt) {
        staging = folder / (".profile-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(attempt));
        if (fs::create_directory(staging, ec)) break;
        staging.clear();
        ec.clear();
    }
    if (staging.empty()) return failure(codes::profile_io, "Cannot reserve profile temporary file");
    const auto temp = staging / "profile.json";
    const auto bytes = profile_to_json(p) + '\n';
    bool written = false;
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        output << bytes;
        output.close();
        written = static_cast<bool>(output);
    }
    bool renamed = false;
    if (written) renamed = detail::replace_profile_file(temp, target);
    fs::remove(temp, ec);
    fs::remove(staging, ec);
    if (!renamed) return failure(codes::profile_io, "Cannot atomically replace profile " + p.id);
    return {};
}

ProfileResult resolve_profile(Profile p, const DiscoveryInput& input) {
    if (const auto error = validate_profile(p)) return {{}, error};
    std::optional<Catalog> catalog;
    for (auto& layer : p.layers) {
        if (directory(layer.root)) continue;
        if (!catalog) { auto light = input; light.walk_trees = false; catalog = discover(light); }
        const auto found = std::find_if(catalog->records.begin(), catalog->records.end(), [&](const auto& record) {
            return record.key == layer.key && directory(record.root);
        });
        if (found == catalog->records.end()) return failed(codes::profile_missing_key, "Missing profile layer key: " + layer.key);
        layer.root = found->root;
    }
    if (const auto error = validate_profile(p)) return {{}, error};
    return {std::move(p), {}};
}

ProfileArguments expand_profile_arguments(const std::vector<std::string>& args, DiscoveryInput input, std::string_view root_flag, const ProfileSteamReader* steam_reader) {
    ProfileArguments out;
    std::string file;
    bool explicit_root = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--eawr-mod-profile") {
            if (!file.empty() || i + 1 == args.size() || args[i + 1].starts_with("--")) {
                out.error = failure(codes::profile_arguments, "Expected one --eawr-mod-profile <file>"); return out;
            }
            file = args[++i];
            if (file.empty()) { out.error = failure(codes::profile_arguments, "Profile path is empty"); return out; }
        } else {
            if (arg == root_flag || arg == "--eawr-mod-root") explicit_root = true;
            out.arguments.push_back(arg == "--eawr-mod-root" ? std::string(root_flag) : arg);
            if ((arg == "--game-root" || arg == "--eawr-game-root") && i + 1 < args.size()) {
                auto game = steam::path_from_utf8(args[i + 1]);
                input.mods_folder = game / "corruption" / "Mods";
                for (auto parent = game; !parent.empty() && parent != parent.parent_path(); parent = parent.parent_path()) {
                    if (steam::lower_ascii(steam::path_to_utf8(parent.filename())) == "steamapps") {
                        input.steam_roots.push_back(parent.parent_path()); break;
                    }
                }
            }
        }
    }
    if (file.empty()) return out;
    if (explicit_root) {
        out.report.push_back("EAWR mods: explicit mod root overrides --eawr-mod-profile"); return out;
    }
    auto loaded = load_profile(steam::path_from_utf8(file));
    if (loaded.error) { out.error = loaded.error; return out; }
    const auto& reader = steam_reader ? *steam_reader : native_profile_steam_reader();
    for (const auto& root : reader.roots()) steam::add_library(input.steam_roots, root);
    auto resolved = resolve_profile(std::move(*loaded.profile), input);
    if (resolved.error) { out.error = resolved.error; return out; }
    out.launch = resolved.profile->launch;
    std::string roots;
    for (const auto& layer : resolved.profile->layers) { if (!roots.empty()) roots += ';'; roots += layer.root; }
    if (!roots.empty()) { out.arguments.emplace_back(root_flag); out.arguments.push_back(roots); }
    out.report.push_back("EAWR mods: profile " + resolved.profile->id);
    out.report.push_back("EAWR mods: mount roots=" + roots);
    out.profile = std::move(resolved.profile);
    return out;
}
} // namespace eawr::mods
