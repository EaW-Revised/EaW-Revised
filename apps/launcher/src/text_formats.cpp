#include "text_formats.hpp"

#include "paths.hpp"

#include <cstdint>
#include <cstdlib>

namespace eawr::launcher {

namespace {

constexpr int kMaxDepth = 64;

void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

std::string_view strip_bom(std::string_view text) {
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    return text;
}

class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}

    std::optional<JsonValue> parse() {
        JsonValue value;
        if (!read_value(value, 0)) return std::nullopt;
        skip_space();
        if (position_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    void skip_space() {
        while (position_ < text_.size()
               && (text_[position_] == ' ' || text_[position_] == '\t' || text_[position_] == '\n' || text_[position_] == '\r')) {
            ++position_;
        }
    }

    bool consume(char expected) {
        skip_space();
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    bool literal(std::string_view word) {
        if (text_.substr(position_, word.size()) != word) return false;
        position_ += word.size();
        return true;
    }

    bool read_hex4(std::uint32_t& out) {
        if (position_ + 4 > text_.size()) return false;
        out = 0;
        for (int digit = 0; digit < 4; ++digit) {
            const char c = text_[position_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool read_string(std::string& out) {
        if (!consume('"')) return false;
        out.clear();
        while (position_ < text_.size()) {
            const char c = text_[position_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (position_ >= text_.size()) return false;
            const char escape = text_[position_++];
            switch (escape) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                std::uint32_t code = 0;
                if (!read_hex4(code)) return false;
                if (code >= 0xD800 && code <= 0xDBFF) {
                    std::uint32_t low = 0;
                    if (text_.substr(position_, 2) != "\\u") return false;
                    position_ += 2;
                    if (!read_hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                } else if (code >= 0xDC00 && code <= 0xDFFF) {
                    return false;
                }
                append_utf8(out, code);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool read_value(JsonValue& value, int depth) {
        if (depth > kMaxDepth) return false;
        skip_space();
        if (position_ >= text_.size()) return false;
        const char c = text_[position_];
        if (c == '{') {
            ++position_;
            value.kind = JsonValue::Kind::object;
            if (consume('}')) return true;
            for (;;) {
                std::string key;
                if (!read_string(key) || !consume(':')) return false;
                JsonValue member;
                if (!read_value(member, depth + 1)) return false;
                value.keys.push_back(std::move(key));
                value.values.push_back(std::move(member));
                if (consume(',')) continue;
                return consume('}');
            }
        }
        if (c == '[') {
            ++position_;
            value.kind = JsonValue::Kind::array;
            if (consume(']')) return true;
            for (;;) {
                JsonValue item;
                if (!read_value(item, depth + 1)) return false;
                value.array.push_back(std::move(item));
                if (consume(',')) continue;
                return consume(']');
            }
        }
        if (c == '"') {
            value.kind = JsonValue::Kind::string;
            return read_string(value.string);
        }
        if (literal("true")) {
            value.kind = JsonValue::Kind::boolean;
            value.boolean = true;
            return true;
        }
        if (literal("false")) {
            value.kind = JsonValue::Kind::boolean;
            return true;
        }
        if (literal("null")) return true;
        const std::size_t begin = position_;
        while (position_ < text_.size()
               && (text_[position_] == '-' || text_[position_] == '+' || text_[position_] == '.' || text_[position_] == 'e'
                   || text_[position_] == 'E' || (text_[position_] >= '0' && text_[position_] <= '9'))) {
            ++position_;
        }
        if (position_ == begin) return false;
        const std::string number(text_.substr(begin, position_ - begin));
        char* end = nullptr;
        value.number = std::strtod(number.c_str(), &end);
        value.kind = JsonValue::Kind::number;
        return end == number.c_str() + number.size();
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

class VdfReader {
public:
    explicit VdfReader(std::string_view text) : text_(text) {}

    std::optional<VdfNode> parse() {
        VdfNode root;
        root.is_object = true;
        if (!read_members(root, 0, false)) return std::nullopt;
        return root;
    }

private:
    enum class Token { end, text, open, close, error };

    void skip_space_and_comments() {
        for (;;) {
            while (position_ < text_.size()
                   && (text_[position_] == ' ' || text_[position_] == '\t' || text_[position_] == '\n' || text_[position_] == '\r')) {
                ++position_;
            }
            if (text_.substr(position_, 2) == "//") {
                while (position_ < text_.size() && text_[position_] != '\n') ++position_;
                continue;
            }
            return;
        }
    }

    Token next(std::string& text) {
        skip_space_and_comments();
        if (position_ >= text_.size()) return Token::end;
        const char c = text_[position_];
        if (c == '{') {
            ++position_;
            return Token::open;
        }
        if (c == '}') {
            ++position_;
            return Token::close;
        }
        text.clear();
        if (c == '"') {
            ++position_;
            while (position_ < text_.size()) {
                const char d = text_[position_++];
                if (d == '"') return Token::text;
                if (d == '\\' && position_ < text_.size()) {
                    const char e = text_[position_];
                    if (e == '\\' || e == '"') { text.push_back(e); ++position_; continue; }
                    if (e == 'n') { text.push_back('\n'); ++position_; continue; }
                    if (e == 't') { text.push_back('\t'); ++position_; continue; }
                    // Unknown escape (a path like C:\Users): keep the backslash.
                }
                text.push_back(d);
            }
            return Token::error;
        }
        while (position_ < text_.size()) {
            const char d = text_[position_];
            if (d == ' ' || d == '\t' || d == '\n' || d == '\r' || d == '{' || d == '}' || d == '"') break;
            text.push_back(d);
            ++position_;
        }
        return Token::text;
    }

    bool read_members(VdfNode& parent, int depth, bool nested) {
        if (depth > kMaxDepth) return false;
        for (;;) {
            std::string key;
            const Token first = next(key);
            if (first == Token::end) return !nested;
            if (first == Token::close) return nested;
            if (first != Token::text) return false;
            std::string value;
            const Token second = next(value);
            VdfNode node;
            node.key = std::move(key);
            if (second == Token::open) {
                node.is_object = true;
                if (!read_members(node, depth + 1, true)) return false;
            } else if (second == Token::text) {
                node.value = std::move(value);
            } else {
                return false;
            }
            parent.children.push_back(std::move(node));
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

} // namespace

const JsonValue* JsonValue::member(std::string_view key) const {
    if (kind != Kind::object) return nullptr;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (keys[index] == key) return &values[index];
    }
    return nullptr;
}

const std::string* JsonValue::string_member(std::string_view key) const {
    const JsonValue* found = member(key);
    return found != nullptr && found->kind == Kind::string ? &found->string : nullptr;
}

std::optional<JsonValue> parse_json(std::string_view text) {
    return JsonReader(strip_bom(text)).parse();
}

std::string json_quote(std::string_view text) {
    static const char* const hex = "0123456789abcdef";
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += "\\u00";
                out.push_back(hex[(c >> 4) & 0xF]);
                out.push_back(hex[c & 0xF]);
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
    return out;
}

const VdfNode* VdfNode::child(std::string_view name) const {
    const std::string wanted = lower_ascii(name);
    for (const VdfNode& node : children) {
        if (lower_ascii(node.key) == wanted) return &node;
    }
    return nullptr;
}

std::optional<VdfNode> parse_vdf(std::string_view text) {
    return VdfReader(strip_bom(text)).parse();
}

} // namespace eawr::launcher
