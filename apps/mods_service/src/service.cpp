#include "service.hpp"

#include "methods.hpp"
#include "loadcheck.hpp"
#include "eawr/mods/steam/text_formats.hpp"

#include "eawr/core/json_text.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

namespace eawr::mods_service {

namespace {

using mods::steam::JsonValue;

constexpr double kMaxId = 9007199254740991.0; // 2^53 - 1
constexpr std::size_t kQuotedNameLimit = 64;

bool valid_utf8(std::string_view text) {
    std::size_t at = 0;
    while (at < text.size()) {
        const auto lead = static_cast<unsigned char>(text[at]);
        std::size_t length = 0;
        std::uint32_t code = 0;
        if (lead < 0x80) {
            ++at;
            continue;
        }
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            code = lead & 0x1Fu;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            code = lead & 0x0Fu;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            code = lead & 0x07u;
        } else {
            return false;
        }
        if (at + length > text.size()) return false;
        for (std::size_t index = 1; index < length; ++index) {
            const auto next = static_cast<unsigned char>(text[at + index]);
            if ((next & 0xC0u) != 0x80u) return false;
            code = (code << 6) | (next & 0x3Fu);
        }
        if ((length == 3 && code < 0x800) || (length == 4 && (code < 0x10000 || code > 0x10FFFF))
            || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        at += length;
    }
    return true;
}

bool blank(std::string_view line) {
    for (const char c : line) {
        if (c != ' ' && c != '\t') return false;
    }
    return true;
}

// A request line that is a JSON object, or nullopt (EAWR-MODS-SVC-0001).
std::optional<JsonValue> parse_object(std::string_view line) {
    if (line.substr(0, 3) == "\xEF\xBB\xBF" || !valid_utf8(line)) return std::nullopt;
    auto value = mods::steam::parse_json(line);
    if (!value || value->kind != JsonValue::Kind::object) return std::nullopt;
    return value;
}

// At most kQuotedNameLimit bytes of a client string for an error message, cut on a UTF-8 boundary.
std::string short_text(std::string_view text) {
    if (text.size() <= kQuotedNameLimit) return std::string(text);
    std::size_t end = kQuotedNameLimit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0u) == 0x80u) --end;
    return std::string(text.substr(0, end)) + "...";
}

std::string response_line(std::string_view id, const Reply& reply) {
    std::string line = "{\"id\":";
    line += id;
    if (reply.error_code.empty()) {
        line += ",\"result\":";
        line += reply.result;
    } else {
        line += ",\"error\":{\"code\":";
        line += core::json_quote(reply.error_code);
        line += ",\"message\":";
        line += core::json_quote(reply.message);
        if (!reply.error_data.empty()) {
            line += ",\"data\":";
            line += reply.error_data;
        }
        line += '}';
    }
    line += '}';
    return line;
}

} // namespace

std::optional<std::uint64_t> json_count(const JsonValue* value) {
    if (value == nullptr || value->kind != JsonValue::Kind::number) return std::nullopt;
    const double number = value->number;
    if (!std::isfinite(number) || number < 0 || number > kMaxId || std::floor(number) != number) return std::nullopt;
    return static_cast<std::uint64_t>(number);
}

Reply Reply::ok(std::string result) {
    Reply reply;
    reply.result = std::move(result);
    return reply;
}

Reply Reply::fail(std::string_view code, std::string message, std::string data) {
    Reply reply;
    reply.error_code = code;
    reply.message = std::move(message);
    reply.error_data = std::move(data);
    return reply;
}

Service::Service(ServiceInfo info, Log log) : info_(std::move(info)), log_(std::move(log)), loadcheck_(std::make_unique<LoadCheck>(*this)) {}
Service::~Service() = default;
LoadCheck& Service::loadcheck() { return *loadcheck_; }

void Service::log(std::string_view text) const {
    if (log_) log_(text);
}

void Service::handle_oversize(std::size_t bytes, std::vector<std::string>& out) {
    log("rejected a request line of " + std::to_string(bytes) + " bytes (limit " + std::to_string(kLineLimit) + ")");
    out.push_back(response_line("null", Reply::fail(kLineTooLong, "request line longer than 1 MiB")));
}

void Service::handle_line(std::string_view line, std::vector<std::string>& out) {
    if (blank(line)) return;
    const auto request = parse_object(line);
    if (!request) {
        log("rejected a malformed line of " + std::to_string(line.size()) + " bytes");
        out.push_back(response_line("null", Reply::fail(kMalformedLine, "the line is not a UTF-8 JSON object")));
        return;
    }
    const auto id = json_count(request->member("id"));
    if (!id) {
        out.push_back(response_line("null", Reply::fail(kInvalidRequest, "\"id\" must be an integer from 0 to 2^53-1")));
        return;
    }
    const std::string id_text = std::to_string(*id);
    const std::string* method = request->string_member("method");
    if (method == nullptr || method->empty()) {
        out.push_back(response_line(id_text, Reply::fail(kInvalidRequest, "\"method\" must be a non-empty string")));
        return;
    }
    static const JsonValue kNoParams = [] {
        JsonValue value;
        value.kind = JsonValue::Kind::object;
        return value;
    }();
    const JsonValue* params = request->member("params");
    if (params == nullptr) {
        params = &kNoParams;
    } else if (params->kind != JsonValue::Kind::object) {
        out.push_back(response_line(id_text, Reply::fail(kInvalidRequest, "\"params\" must be an object")));
        return;
    }
    const MethodEntry* entry = find_method(*method);
    Reply reply;
    if (entry == nullptr) {
        reply = Reply::fail(kUnknownMethod, "unknown method \"" + short_text(*method) + "\"");
    } else if (!open_ && !entry->before_hello) {
        reply = Reply::fail(kHelloRequired, "the first request must be \"hello\"");
    } else {
        try {
            if (*method == "loadcheck.start") {
                const auto immediate = loadcheck_->start(*id, *params);
                if (!immediate) return;
                reply = *immediate;
            } else {
                if (*method == "shutdown") loadcheck_->stop();
                reply = entry->handler(*this, *params);
            }
        } catch (const std::exception& error) {
            reply = Reply::fail(kInternalError, error.what());
        }
    }
    std::string response = response_line(id_text, reply);
    if (response.size() > kLineLimit) {
        log("answer to request " + id_text + " was " + std::to_string(response.size()) + " bytes, over the line limit");
        response = response_line(id_text, Reply::fail(kInternalError, "the answer exceeded the line limit"));
    }
    out.push_back(std::move(response));
}

std::string transcript_request(std::string_view line) {
    if (parse_object(line)) return "{\"dir\":\"c2s\",\"msg\":" + std::string(line) + "}";
    // Not an object: kept as text. Bytes that are not UTF-8 become \u00XX of the byte.
    const std::string raw = valid_utf8(line) ? core::json_quote(line) : core::json_quote_ascii(line);
    return "{\"dir\":\"c2s\",\"raw\":" + raw + "}";
}

std::string transcript_oversize(std::size_t bytes) {
    return "{\"dir\":\"c2s\",\"oversize_bytes\":" + std::to_string(bytes) + "}";
}

std::string transcript_response(std::string_view line) {
    return "{\"dir\":\"s2c\",\"msg\":" + std::string(line) + "}";
}

std::string transcript_meta() {
    return "{\"dir\":\"meta\",\"msg\":{\"protocol\":\"" + std::to_string(kProtocolMajor) + "."
         + std::to_string(kProtocolMinor) + "\",\"recorded_by\":\"eawr_mods_service\"}}";
}

} // namespace eawr::mods_service
