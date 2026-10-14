#include "relay_cloudflare.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace eawr::net::relay {
namespace {
// Small bounded JSON reader for the one response shape we accept. Depth and size
// are capped, strings decode the standard escapes, numbers and literals are kept as text.
struct Json {
    enum class Type { null, boolean, number, string, array, object } type{Type::null};
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;
    const Json* find(std::string_view name) const {
        for (const auto& [key, value] : members) if (key == name) return &value;
        return nullptr;
    }
};
class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}
    std::optional<Json> parse() {
        auto value = parse_value(0);
        skip();
        if (!value || at_ != text_.size()) return {};
        return value;
    }
private:
    std::string_view text_;
    std::size_t at_{}, nodes_{};
    void skip() { while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r')) ++at_; }
    bool take(char ch) { skip(); if (at_ < text_.size() && text_[at_] == ch) { ++at_; return true; } return false; }
    std::optional<Json> parse_value(unsigned depth) {
        skip();
        if (depth > 8 || at_ >= text_.size() || ++nodes_ > 512) return {};
        Json value;
        const char ch = text_[at_];
        if (ch == '"') {
            auto text = parse_string();
            if (!text) return {};
            value.type = Json::Type::string; value.text = std::move(*text);
        } else if (ch == '[') {
            ++at_; value.type = Json::Type::array;
            if (!take(']')) {
                do {
                    auto item = parse_value(depth + 1);
                    if (!item) return {};
                    value.items.push_back(std::move(*item));
                } while (take(','));
                if (!take(']')) return {};
            }
        } else if (ch == '{') {
            ++at_; value.type = Json::Type::object;
            if (!take('}')) {
                do {
                    skip();
                    if (at_ >= text_.size() || text_[at_] != '"') return {};
                    auto key = parse_string();
                    if (!key || !take(':')) return {};
                    auto member = parse_value(depth + 1);
                    if (!member) return {};
                    value.members.emplace_back(std::move(*key), std::move(*member));
                } while (take(','));
                if (!take('}')) return {};
            }
        } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
            const auto start = at_++;
            while (at_ < text_.size() && std::string_view("0123456789+-.eE").find(text_[at_]) != std::string_view::npos) ++at_;
            value.type = Json::Type::number; value.text = std::string(text_.substr(start, at_ - start));
        } else {
            for (const std::string_view literal : {"true", "false", "null"}) {
                if (text_.substr(at_).starts_with(literal)) {
                    at_ += literal.size();
                    value.type = literal == "null" ? Json::Type::null : Json::Type::boolean;
                    return value;
                }
            }
            return {};
        }
        return value;
    }
    static void utf8(std::string& out, unsigned code) {
        if (code < 0x80) out += static_cast<char>(code);
        else if (code < 0x800) { out += static_cast<char>(0xC0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 0x3F)); }
        else if (code < 0x10000) { out += static_cast<char>(0xE0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); out += static_cast<char>(0x80 | (code & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (code >> 18)); out += static_cast<char>(0x80 | ((code >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); out += static_cast<char>(0x80 | (code & 0x3F)); }
    }
    std::optional<unsigned> hex4() {
        if (at_ + 4 > text_.size()) return {};
        unsigned code{};
        for (int i = 0; i < 4; ++i) {
            const char ch = text_[at_++];
            code <<= 4;
            if (ch >= '0' && ch <= '9') code |= static_cast<unsigned>(ch - '0');
            else if (ch >= 'a' && ch <= 'f') code |= static_cast<unsigned>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F') code |= static_cast<unsigned>(ch - 'A' + 10);
            else return {};
        }
        return code;
    }
    std::optional<std::string> parse_string() {
        const auto start = ++at_;
        std::string out;
        while (at_ < text_.size()) {
            // Decoded bytes and raw source (escapes included) are both bounded.
            if (out.size() > 4096 || at_ - start > 12288) return {};
            const char ch = text_[at_++];
            if (ch == '"') return out;
            if (static_cast<unsigned char>(ch) < 0x20) return {};
            if (ch != '\\') { out += ch; continue; }
            if (at_ >= text_.size()) return {};
            const char escape = text_[at_++];
            switch (escape) {
            case '"': case '\\': case '/': out += escape; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                auto code = hex4();
                if (!code) return {};
                if (*code >= 0xD800 && *code < 0xDC00) {
                    if (!text_.substr(at_).starts_with("\\u")) return {};
                    at_ += 2;
                    const auto low = hex4();
                    if (!low || *low < 0xDC00 || *low > 0xDFFF) return {};
                    *code = 0x10000 + ((*code - 0xD800) << 10) + (*low - 0xDC00);
                } else if (*code >= 0xDC00 && *code <= 0xDFFF) return {};
                utf8(out, *code);
                break;
            }
            default: return {};
            }
        }
        return {};
    }
};
// `turn:host:port` or `turn:host:port?transport=udp`; anything else is not plain UDP TURN.
std::optional<std::string> udp_turn_uri(const std::string& url) {
    if (!url.starts_with("turn:")) return {};
    const auto query = url.find('?');
    if (query != std::string::npos && url.substr(query) != "?transport=udp") return {};
    auto uri = url.substr(0, query);
    if (!turn_uri_valid(uri) || uri.find(',') != std::string::npos) return {};
    return uri;
}
bool credential_text_valid(const std::string& text) {
    return !text.empty() && text.size() <= 256
        && std::none_of(text.begin(), text.end(), [](unsigned char ch) { return ch < 33 || ch > 126 || ch == ','; });
}
std::string encode_path_segment(std::string_view text) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char ch : text) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == '~') result += static_cast<char>(ch);
        else { result += '%'; result += hex[ch >> 4]; result += hex[ch & 15]; }
    }
    return result;
}
HttpsRequest base_request(const CloudflareTurnConfig& config, std::string path) {
    HttpsRequest request;
    request.host = config.host; request.connect_address = config.connect_address; request.port = config.port;
    request.path = std::move(path); request.bearer = config.api_token; request.ca_file = config.ca_file;
    request.timeout = config.timeout;
    return request;
}
}
bool cloudflare_key_id_valid(std::string_view key_id) {
    return !key_id.empty() && key_id.size() <= 128
        && std::all_of(key_id.begin(), key_id.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'; });
}
bool cloudflare_api_token_valid(std::string_view token) {
    return !token.empty() && token.size() <= 512
        && std::none_of(token.begin(), token.end(), [](unsigned char ch) { return ch < 33 || ch > 126; });
}
core::Result<GnsTurnCredentials> cloudflare_credentials(const HttpsResponse& response, std::chrono::seconds ttl) {
    using Out = core::Result<GnsTurnCredentials>;
    if (response.status < 200 || response.status > 299)
        return Out::failure(error(1557, "Cloudflare TURN API refused the credential request (HTTP " + std::to_string(response.status) + ")"));
    const auto malformed = [](const char* why) { return Out::failure(error(1558, why)); };
    const auto json = JsonReader(response.body).parse();
    if (!json || json->type != Json::Type::object) return malformed("Cloudflare TURN response is not valid JSON");
    const auto* servers = json->find("iceServers");
    if (!servers) return malformed("Cloudflare TURN response has no iceServers");
    std::vector<const Json*> candidates;
    if (servers->type == Json::Type::array) for (const auto& item : servers->items) candidates.push_back(&item);
    else if (servers->type == Json::Type::object) candidates.push_back(servers);
    for (const auto* server : candidates) {
        if (server->type != Json::Type::object) continue;
        const auto* username = server->find("username");
        const auto* credential = server->find("credential");
        const auto* urls = server->find("urls");
        if (!username || !credential || !urls || username->type != Json::Type::string || credential->type != Json::Type::string
            || !credential_text_valid(username->text) || !credential_text_valid(credential->text)) continue;
        std::vector<const Json*> entries;
        if (urls->type == Json::Type::string) entries.push_back(urls);
        else if (urls->type == Json::Type::array) for (const auto& item : urls->items) entries.push_back(&item);
        std::vector<std::string> uris;
        for (const auto* entry : entries) {
            if (entry->type != Json::Type::string || uris.size() >= 2) continue;
            auto uri = udp_turn_uri(entry->text);
            if (uri && std::find(uris.begin(), uris.end(), *uri) == uris.end()) uris.push_back(std::move(*uri));
        }
        if (uris.empty()) continue;
        GnsTurnCredentials result{{}, username->text, credential->text, ttl};
        for (const auto& uri : uris) result.uri += (result.uri.empty() ? "" : ",") + uri;
        if (!turn_uri_list_valid(result.uri)) continue;
        return Out::success(std::move(result));
    }
    return malformed("Cloudflare TURN response has no UDP TURN URL with credentials");
}
core::Result<GnsTurnCredentials> cloudflare_generate(const CloudflareTurnConfig& config, std::chrono::seconds ttl, std::stop_token stop) {
    using Out = core::Result<GnsTurnCredentials>;
    auto request = base_request(config, "/v1/turn/keys/" + config.key_id + "/credentials/generate-ice-servers");
    request.body = "{\"ttl\":" + std::to_string(ttl.count()) + "}";
    auto response = https_request(request, stop);
    if (!response) return Out::failure(response.error());
    return cloudflare_credentials(response.value(), ttl);
}
core::Result<void> cloudflare_revoke(const CloudflareTurnConfig& config, const std::string& username, std::stop_token stop) {
    auto request = base_request(config, "/v1/turn/keys/" + config.key_id + "/credentials/" + encode_path_segment(username) + "/revoke");
    auto response = https_request(request, stop);
    if (!response) return core::Result<void>::failure(response.error());
    if (response.value().status < 200 || response.value().status > 299)
        return core::Result<void>::failure(error(1557, "Cloudflare TURN API refused the credential revoke (HTTP " + std::to_string(response.value().status) + ")"));
    return core::Result<void>::success();
}

CloudflareTurnWorker::CloudflareTurnWorker(CloudflareTurnConfig config)
    : config_(std::move(config)), worker_([this](std::stop_token stop) { run(stop); }) {}
CloudflareTurnWorker::~CloudflareTurnWorker() {
    { std::lock_guard lock(mutex_); worker_.request_stop(); }
    condition_.notify_all(); worker_.join();
}
bool CloudflareTurnWorker::submit(Job job) {
    std::lock_guard lock(mutex_);
    if (outstanding_ >= 16 || (job.kind == Kind::generate && (job.session.empty() || job.ttl.count() <= 0))
        || (job.kind == Kind::revoke && job.username.empty())) return false;
    ++outstanding_; jobs_.push_back(std::move(job)); condition_.notify_all(); return true;
}
std::optional<CloudflareTurnWorker::Completed> CloudflareTurnWorker::take() {
    std::lock_guard lock(mutex_);
    if (completed_.empty()) return {};
    auto result = std::move(completed_.front()); completed_.pop_front(); --outstanding_; return result;
}
void CloudflareTurnWorker::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::optional<Job> job;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return stop.stop_requested() || !jobs_.empty(); });
            if (stop.stop_requested()) return;
            job = std::move(jobs_.front()); jobs_.pop_front();
        }
        Completed done{*job, {}, {}};
        if (job->kind == Kind::generate) {
            auto generated = cloudflare_generate(config_, job->ttl, stop);
            if (generated) done.credentials = std::move(generated).value(); else done.failure = generated.error();
        } else {
            auto revoked = cloudflare_revoke(config_, job->username, stop);
            if (!revoked) done.failure = revoked.error();
        }
        std::lock_guard lock(mutex_);
        completed_.push_back(std::move(done));
    }
}
} // namespace eawr::net::relay
