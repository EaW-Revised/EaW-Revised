#include "file_methods.hpp"
#include "loadcheck.hpp"
#include "eawr/core/sha256.hpp"
#include <algorithm>
#include <array>
#include <random>

namespace eawr::mods_service {
namespace {
using J = mods::steam::JsonValue;
using mods::steam::json_quote;
bool string(const J& object, std::string_view name, std::string& target) {
    const auto* value = object.member(name);
    if (!value) return true;
    if (value->kind != J::Kind::string || value->string.find('\0') != std::string::npos) return false;
    target = value->string;
    return true;
}
bool boolean(const J& object, std::string_view name, bool& target) {
    const auto* value = object.member(name);
    if (!value) return true;
    if (value->kind != J::Kind::boolean) return false;
    target = value->boolean;
    return true;
}
Reply invalid(std::string text) { return Reply::fail(kInvalidParams, std::move(text)); }
}

Reply method_files_page(Service& service, const J& params) {
    mods::FileFilter filter;
    if (const auto* value = params.member("filters")) {
        if (value->kind != J::Kind::object || !string(*value, "layer", filter.layer)
            || !string(*value, "role", filter.role) || !string(*value, "type", filter.type)
            || !string(*value, "path", filter.path) || !boolean(*value, "conflicts_only", filter.conflicts_only)
            || !boolean(*value, "hide_identical", filter.hide_identical)) return invalid("invalid file filters");
    }
    if (filter.role != "winner" && filter.role != "loser" && filter.role != "either") return invalid("role must be winner, loser or either");
    std::size_t limit = 100;
    if (const auto* value = params.member("limit")) {
        const auto count = json_count(value);
        if (!count || *count < 1 || *count > 500) return invalid("limit must be 1..500");
        limit = static_cast<std::size_t>(*count);
    }
    const auto* cursor = params.member("cursor");
    if (cursor && cursor->kind != J::Kind::null && cursor->kind != J::Kind::string) return invalid("cursor must be a string or null");
    auto cached = service.loadcheck().indexed_files(params);
    if (cached.error) return std::move(*cached.error);
    const auto* profile = params.string_member("profile"); // indexed_files validates this.
    const auto method = "files.page:" + *profile;
    if (!cached.index) {
        if (cursor && cursor->kind == J::Kind::string) return invalid("unknown or stale cursor");
        return Reply::ok("{\"ready\":false,\"requires_loadcheck\":true,\"index_version\":1,\"chain_fingerprint\":"
            + json_quote(cached.key) + ",\"rows\":[],\"layers\":[],\"total\":0,\"next_cursor\":null}");
    }
    const auto signature = cached.key + '\n' + cached.revision + '\n' + json_quote(filter.layer) + '\n'
        + json_quote(filter.role) + '\n' + json_quote(filter.type) + '\n' + json_quote(filter.path)
        + (filter.conflicts_only ? "\ntrue" : "\nfalse") + (filter.hide_identical ? "\ntrue" : "\nfalse");
    const auto digest = core::sha256(std::span(reinterpret_cast<const std::uint8_t*>(signature.data()), signature.size()));
    std::size_t offset = 0;
    if (cursor && cursor->kind == J::Kind::string) {
        const auto found = service.model.pages.find(cursor->string);
        if (found == service.model.pages.end() || found->second.method != method || found->second.digest != digest)
            return invalid("unknown or stale cursor");
        offset = found->second.offset;
    }
    std::vector<std::size_t> matches;
    for (std::size_t i = 0; i < cached.index->rows.size(); ++i)
        if (mods::matches_file_filter(cached.index->rows[i], filter)) matches.push_back(i);
    if (offset > matches.size()) return invalid("unknown or stale cursor");
    std::string extra = ",\"ready\":true,\"requires_loadcheck\":false,\"index_version\":1,\"chain_fingerprint\":"
        + json_quote(cached.key) + ",\"layers\":" + mods::file_layers_json(*cached.index)
        + ",\"total\":" + std::to_string(matches.size());
    std::string rows = "{\"rows\":[";
    auto end = offset;
    while (end < matches.size() && end - offset < limit) {
        auto row = mods::file_row_json(cached.index->rows[matches[end]]);
        if (rows.size() + row.size() + extra.size() + 2048 > kLineLimit) break;
        if (end > offset) rows += ',';
        rows += row;
        ++end;
    }
    if (end == offset && end < matches.size()) return Reply::fail(kInternalError, "one file row exceeds the page limit");
    if (rows.size() + extra.size() + 2048 > kLineLimit) return Reply::fail(kInternalError, "file layer summary exceeds the page limit");
    std::string next = "null";
    if (end < matches.size()) {
        const auto token = "files-" + std::to_string(std::random_device{}()) + '-' + std::to_string(++service.model.sequence);
        if (service.model.pages.size() >= 64) service.model.pages.erase(service.model.pages.begin());
        service.model.pages.emplace(token, Page{method, digest, end});
        next = json_quote(token);
    }
    return Reply::ok(rows + "],\"next_cursor\":" + next + extra + '}');
}
} // namespace eawr::mods_service
