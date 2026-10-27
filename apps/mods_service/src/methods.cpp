#include "methods.hpp"
#include "model_methods.hpp"
#include "loadcheck.hpp"
#include "file_methods.hpp"

#include "eawr/mods/steam/text_formats.hpp"

#include "eawr/core/json_text.hpp"

#include <array>

namespace eawr::mods_service {

namespace {

using mods::steam::JsonValue;

std::string printable(std::string_view text) {
    std::size_t end = text.size() < 64 ? text.size() : 64;
    while (end < text.size() && end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u) --end;
    std::string out;
    for (const char c : text.substr(0, end)) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(byte < 0x20 || byte == 0x7F ? '?' : c);
    }
    return out;
}

std::string capabilities_json() {
    std::string out = "[";
    for (const std::string_view name : capabilities()) {
        if (out.size() > 1) out += ',';
        out += core::json_quote(name);
    }
    return out + ']';
}

// The members of a hello result; also the error data of EAWR-MODS-SVC-0006.
std::string hello_members(const ServiceInfo& info) {
    return "{\"protocol\":{\"major\":" + std::to_string(kProtocolMajor) + ",\"minor\":" + std::to_string(kProtocolMinor)
         + "},\"service_version\":" + core::json_quote(info.service_version)
         + ",\"engine_build\":" + core::json_quote(info.engine_build)
         + ",\"game_release\":" + core::json_quote(info.game_release) + ",\"capabilities\":" + capabilities_json() + '}';
}

Reply method_hello(Service& service, const JsonValue& params) {
    if (service.open()) return Reply::fail(kHelloRepeated, "the session is already open");
    const std::string* client = params.string_member("client");
    const std::string* client_version = params.string_member("client_version");
    const JsonValue* protocol = params.member("protocol");
    if (client == nullptr || client_version == nullptr) {
        return Reply::fail(kInvalidParams, "hello needs string members \"client\" and \"client_version\"");
    }
    if (protocol == nullptr || protocol->kind != JsonValue::Kind::object) {
        return Reply::fail(kInvalidParams, "hello needs an object member \"protocol\"");
    }
    const auto major = json_count(protocol->member("major"));
    const auto min_minor = json_count(protocol->member("min_minor"));
    if (!major || !min_minor) {
        return Reply::fail(kInvalidParams, "\"protocol\" needs integer members \"major\" and \"min_minor\"");
    }
    const std::string asked = std::to_string(*major) + '.' + std::to_string(*min_minor);
    service.log("hello from " + printable(*client) + ' ' + printable(*client_version) + ", protocol " + asked);
    if (*major != static_cast<std::uint64_t>(kProtocolMajor) || *min_minor > static_cast<std::uint64_t>(kProtocolMinor)) {
        return Reply::fail(kProtocolNotSupported,
                           "the client needs protocol " + asked + ", this service speaks "
                               + std::to_string(kProtocolMajor) + '.' + std::to_string(kProtocolMinor),
                           hello_members(service.info()));
    }
    service.set_open();
    return Reply::ok(hello_members(service.info()));
}

Reply method_shutdown(Service& service, const JsonValue&) {
    service.set_finished();
    return Reply::ok("{}");
}

// Sorted by name: capabilities() lists them in this order.
constexpr std::array kMethods{
    MethodEntry{"chain.check", &method_chain_check, false},
    MethodEntry{"chain.resolve", &method_chain_resolve, false},
    MethodEntry{"files.page", &method_files_page, false},
    MethodEntry{"game.find", &method_game_find, false},
    MethodEntry{"hello", &method_hello, true},
    MethodEntry{"loadcheck.cancel", &method_loadcheck_cancel, false},
    MethodEntry{"loadcheck.result", &method_loadcheck_result, false},
    MethodEntry{"loadcheck.start", &method_loadcheck_start, false},
    MethodEntry{"mods.changes", &method_mods_changes, false},
    MethodEntry{"mods.discover", &method_mods_discover, false},
    MethodEntry{"profile.delete", &method_profile_delete, false},
    MethodEntry{"profile.list", &method_profile_list, false},
    MethodEntry{"profile.load", &method_profile_load, false},
    MethodEntry{"profile.save", &method_profile_save, false},
    MethodEntry{"shutdown", &method_shutdown, false},
};

} // namespace

const MethodEntry* find_method(std::string_view name) {
    for (const MethodEntry& entry : kMethods) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

std::vector<std::string_view> capabilities() {
    std::vector<std::string_view> names;
    names.reserve(kMethods.size());
    for (const MethodEntry& entry : kMethods) names.push_back(entry.name);
    return names;
}

} // namespace eawr::mods_service
