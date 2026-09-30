#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A small JSON reader for scenario and behaviour-case files; numbers stay decimal text so a
// caller converts them exactly (Fixed::from_decimal). Shared by sim_headless --scenario and the
// targeting case tests.
namespace sim_headless {

// A JSON value; containers hold their elements in `items` (and an object its keys in `keys`),
// so the type needs no container of an incomplete pair.
struct Json {
    enum class Kind : std::uint8_t { null, boolean, string, number, array, object };
    Kind kind{Kind::null};
    bool flag{};
    std::string text_value; // a string, or a number's decimal text
    std::vector<std::string> keys;
    std::vector<Json> items;

    [[nodiscard]] const Json* get(const std::string_view key) const {
        if (kind != Kind::object) return nullptr;
        for (std::size_t index = 0; index < keys.size(); ++index) {
            if (keys[index] == key) return &items[index];
        }
        return nullptr;
    }
    [[nodiscard]] const std::string* text() const { return kind == Kind::string ? &text_value : nullptr; }
    [[nodiscard]] const std::string* digits() const { return kind == Kind::number ? &text_value : nullptr; }
    [[nodiscard]] const std::vector<Json>* array() const { return kind == Kind::array ? &items : nullptr; }
};

class JsonReader final {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}

    [[nodiscard]] std::optional<Json> document() {
        auto value = parse(0);
        skip();
        if (!value || at_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    void skip() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\n' || text_[at_] == '\r' || text_[at_] == '\t')) {
            ++at_;
        }
    }
    bool literal(const std::string_view word) {
        if (text_.substr(at_, word.size()) != word) return false;
        at_ += word.size();
        return true;
    }
    std::optional<std::string> string() {
        if (at_ >= text_.size() || text_[at_] != '"') return std::nullopt;
        ++at_;
        std::string result;
        while (at_ < text_.size() && text_[at_] != '"') {
            char character = text_[at_++];
            if (character == '\\') {
                if (at_ >= text_.size()) return std::nullopt;
                const char escaped = text_[at_++];
                switch (escaped) {
                case '"': character = '"'; break;
                case '\\': character = '\\'; break;
                case '/': character = '/'; break;
                case 'n': character = '\n'; break;
                case 't': character = '\t'; break;
                case 'r': character = '\r'; break;
                default: return std::nullopt; // scenario files need no other escapes
                }
            }
            result.push_back(character);
        }
        if (at_ >= text_.size()) return std::nullopt;
        ++at_;
        return result;
    }
    std::optional<Json> parse(const int depth) {
        if (depth > 32) return std::nullopt;
        skip();
        if (at_ >= text_.size()) return std::nullopt;
        const char first = text_[at_];
        Json json;
        if (first == '{') {
            ++at_;
            json.kind = Json::Kind::object;
            skip();
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return json;
            }
            while (true) {
                skip();
                auto key = string();
                skip();
                if (!key || at_ >= text_.size() || text_[at_++] != ':') return std::nullopt;
                auto item = parse(depth + 1);
                if (!item) return std::nullopt;
                json.keys.push_back(std::move(*key));
                json.items.push_back(std::move(*item));
                skip();
                if (at_ < text_.size() && text_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (at_ < text_.size() && text_[at_] == '}') {
                    ++at_;
                    break;
                }
                return std::nullopt;
            }
            return json;
        }
        if (first == '[') {
            ++at_;
            json.kind = Json::Kind::array;
            skip();
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return json;
            }
            while (true) {
                auto item = parse(depth + 1);
                if (!item) return std::nullopt;
                json.items.push_back(std::move(*item));
                skip();
                if (at_ < text_.size() && text_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (at_ < text_.size() && text_[at_] == ']') {
                    ++at_;
                    break;
                }
                return std::nullopt;
            }
            return json;
        }
        if (first == '"') {
            auto text = string();
            if (!text) return std::nullopt;
            json.kind = Json::Kind::string;
            json.text_value = std::move(*text);
            return json;
        }
        if (literal("true")) {
            json.kind = Json::Kind::boolean;
            json.flag = true;
            return json;
        }
        if (literal("false")) {
            json.kind = Json::Kind::boolean;
            return json;
        }
        if (literal("null")) {
            return json;
        }
        const auto start = at_;
        while (at_ < text_.size()
               && (std::isdigit(static_cast<unsigned char>(text_[at_])) != 0 || text_[at_] == '-' || text_[at_] == '+'
                   || text_[at_] == '.' || text_[at_] == 'e' || text_[at_] == 'E')) {
            ++at_;
        }
        if (start == at_) return std::nullopt;
        json.kind = Json::Kind::number;
        json.text_value = std::string(text_.substr(start, at_ - start));
        return json;
    }

    std::string_view text_;
    std::size_t at_{};
};

} // namespace sim_headless
