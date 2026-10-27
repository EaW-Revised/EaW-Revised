#pragma once
// The method table of eawr_mods_service. A new method is one handler in methods.cpp plus one row in its table
// (and its section in docs/mods-service-protocol.md).
#include "service.hpp"
#include "eawr/mods/steam/text_formats.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace eawr::mods_service {

// docs/mods-service-protocol.md, section 4.
inline constexpr std::string_view kMalformedLine = "EAWR-MODS-SVC-0001";
inline constexpr std::string_view kInvalidRequest = "EAWR-MODS-SVC-0002";
inline constexpr std::string_view kLineTooLong = "EAWR-MODS-SVC-0003";
inline constexpr std::string_view kUnknownMethod = "EAWR-MODS-SVC-0004";
inline constexpr std::string_view kInvalidParams = "EAWR-MODS-SVC-0005";
inline constexpr std::string_view kProtocolNotSupported = "EAWR-MODS-SVC-0006";
inline constexpr std::string_view kHelloRequired = "EAWR-MODS-SVC-0007";
inline constexpr std::string_view kHelloRepeated = "EAWR-MODS-SVC-0008";
inline constexpr std::string_view kCancelled = "EAWR-MODS-SVC-0009";
inline constexpr std::string_view kBusy = "EAWR-MODS-SVC-0010";
inline constexpr std::string_view kInternalError = "EAWR-MODS-SVC-0011";

struct Reply {
    std::string result;          // a JSON object's text, when error_code is empty
    std::string_view error_code; // one of the codes above
    std::string message;
    std::string error_data;      // a JSON object's text, or empty

    static Reply ok(std::string result);
    static Reply fail(std::string_view code, std::string message, std::string data = {});
};

using Handler = Reply (*)(Service& service, const mods::steam::JsonValue& params);

struct MethodEntry {
    std::string_view name;
    Handler handler;
    bool before_hello; // may run before the session is open (only hello)
};

[[nodiscard]] const MethodEntry* find_method(std::string_view name);

// A JSON integer from 0 to 2^53 - 1, or nullopt.
[[nodiscard]] std::optional<std::uint64_t> json_count(const mods::steam::JsonValue* value);

} // namespace eawr::mods_service
