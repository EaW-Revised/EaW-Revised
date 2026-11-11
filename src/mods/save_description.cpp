#include "eawr/mods/save_description.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace eawr::mods {
namespace {
bool text_valid(const std::string& s) {
    if (s.size() > kMaxDescriptionString) return false;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i++]);
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) continue;
        unsigned count = 0; std::uint32_t cp = 0; std::uint32_t minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; cp = c & 0x1f; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; cp = c & 0x0f; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; cp = c & 0x07; minimum = 0x10000; }
        else return false;
        if (count > s.size() - i) return false;
        while (count-- != 0) {
            const auto next = static_cast<unsigned char>(s[i++]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            (cp >= 0x80 && cp <= 0x9f)) return false;
    }
    return true;
}
bool known(const DescriptionEntry& e) { return e.kind == DescriptionKeyKind::workshop || e.kind == DescriptionKeyKind::mods_folder; }
bool valid(const ModDescription& d) {
    if (d.entries.size() > kMaxDescriptionLayers) return false;
    std::set<std::pair<DescriptionKeyKind, std::string>> keys;
    for (const auto& e : d.entries) {
        if (!text_valid(e.key) || !text_valid(e.name) || (e.version && !text_valid(*e.version))) return false;
        if (known(e)) {
            const auto prefix = e.kind == DescriptionKeyKind::workshop ? "workshop:" : "mods:";
            const auto prefix_size = std::char_traits<char>::length(prefix);
            if (!e.key.starts_with(prefix) || e.key.size() <= prefix_size) return false;
            const auto variant = e.key.find("#variant:", prefix_size);
            const auto base_end = variant == std::string::npos ? e.key.size() : variant;
            if (base_end == prefix_size || e.key.find_first_of("/\\") != std::string::npos ||
                e.key.substr(prefix_size, base_end - prefix_size).find(':') != std::string::npos ||
                (variant != std::string::npos && (variant + 9 == e.key.size() || e.key.find(':', variant + 9) != std::string::npos)) ||
                !keys.emplace(e.kind, e.key).second) return false;
        } else if ((e.kind != DescriptionKeyKind::unknown && e.kind != DescriptionKeyKind::manual) || !e.key.empty()) return false;
    }
    return true;
}
void put_u32(std::vector<std::uint8_t>& b, std::uint32_t n) {
    for (unsigned shift = 0; shift < 32; shift += 8) b.push_back(static_cast<std::uint8_t>(n >> shift));
}
void put_string(std::vector<std::uint8_t>& b, const std::string& s) {
    put_u32(b, static_cast<std::uint32_t>(s.size())); b.insert(b.end(), s.begin(), s.end());
}
struct Reader {
    std::span<const std::uint8_t> b; std::size_t pos = 0;
    bool byte(std::uint8_t& n) { if (pos == b.size()) return false; n = b[pos++]; return true; }
    bool u32(std::uint32_t& n) {
        if (b.size() - pos < 4) return false;
        n = 0; for (unsigned shift = 0; shift < 32; shift += 8) n |= static_cast<std::uint32_t>(b[pos++]) << shift;
        return true;
    }
    bool string(std::string& s) {
        std::uint32_t n = 0; if (!u32(n) || n > kMaxDescriptionString || n > b.size() - pos) return false;
        s.assign(reinterpret_cast<const char*>(b.data() + pos), n); pos += n; return true;
    }
};
} // namespace

std::optional<std::vector<std::uint8_t>> encode_description(const ModDescription& d) {
    if (!valid(d)) return std::nullopt;
    std::vector<std::uint8_t> b{3, static_cast<std::uint8_t>(d.complete)};
    put_u32(b, static_cast<std::uint32_t>(d.entries.size()));
    for (const auto& e : d.entries) {
        b.push_back(static_cast<std::uint8_t>(e.kind)); put_string(b, e.key); put_string(b, e.name);
        b.push_back(static_cast<std::uint8_t>(e.version.has_value())); if (e.version) put_string(b, *e.version);
        b.push_back(static_cast<std::uint8_t>(e.checksum.has_value()));
        if (e.checksum) b.insert(b.end(), e.checksum->begin(), e.checksum->end());
        b.push_back(static_cast<std::uint8_t>(e.portable_tree.has_value()));
        if (e.portable_tree) b.insert(b.end(), e.portable_tree->begin(), e.portable_tree->end());
        if (b.size() > kMaxDescriptionBytes) return std::nullopt;
    }
    return b;
}
std::optional<ModDescription> decode_description(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > kMaxDescriptionBytes) return std::nullopt;
    Reader r{bytes}; std::uint8_t version = 0, complete = 0; std::uint32_t count = 0;
    if (!r.byte(version) || (version != 2 && version != 3) || !r.byte(complete) || complete > 1 || !r.u32(count) || count > kMaxDescriptionLayers) return std::nullopt;
    ModDescription d; d.complete = complete != 0; d.entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint8_t kind = 0, present = 0; DescriptionEntry e;
        if (!r.byte(kind) || kind > 3 || !r.string(e.key) || !r.string(e.name) || !r.byte(present) || present > 1) return std::nullopt;
        e.kind = static_cast<DescriptionKeyKind>(kind);
        if (present) { std::string v; if (!r.string(v)) return std::nullopt; e.version = std::move(v); }
        if (!r.byte(present) || present > 1) return std::nullopt;
        if (present) {
            std::array<std::uint8_t, 32> checksum{};
            for (auto& byte : checksum) if (!r.byte(byte)) return std::nullopt;
            e.checksum = checksum;
        }
        if (version == 3 && (!r.byte(present) || present > 1)) return std::nullopt;
        if (version == 3 && present) {
            std::array<std::uint8_t, 32> tree{};
            for (auto& byte : tree) if (!r.byte(byte)) return std::nullopt;
            e.portable_tree = tree;
        }
        d.entries.push_back(std::move(e));
    }
    if (r.pos != bytes.size() || !valid(d)) return std::nullopt;
    return d;
}
namespace {
// Lowercase hex of a ready content digest; anything else is not a digest and stays null.
std::optional<std::array<std::uint8_t, 32>> digest_bytes(const std::string& hex) {
    if (hex.size() != 64) return std::nullopt;
    std::array<std::uint8_t, 32> out{};
    for (std::size_t i = 0; i < 32; ++i) {
        unsigned value = 0;
        for (std::size_t n = 0; n < 2; ++n) {
            const char c = hex[i * 2 + n];
            unsigned d;
            if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
            else return std::nullopt;
            value = value * 16 + d;
        }
        out[i] = static_cast<std::uint8_t>(value);
    }
    return out;
}
} // namespace
ModDescription describe_chain(const Chain& chain, std::span<const ModRecord> records) {
    ModDescription d; d.complete = true;
    for (const auto& key : chain.layers) {
        const auto* it = find_chain_record(records, key);
        DescriptionEntry e;
        if (it == nullptr) { d.complete = false; }
        else {
            e.name = it->name;
            e.portable_tree = digest_bytes(it->fingerprint.portable_tree);
            if (it->content.state == ContentDigestState::ready) e.checksum = digest_bytes(it->content.sha256);
            if (it->kind == ModKind::manual) e.kind = DescriptionKeyKind::manual;
            else { e.kind = it->kind == ModKind::workshop ? DescriptionKeyKind::workshop : DescriptionKeyKind::mods_folder; e.key = it->key; }
            const auto* info = it->metadata.member("modinfo");
            const auto* v = info ? info->member("version") : nullptr;
            if (v && v->kind == steam::JsonValue::Kind::string) e.version = v->string;
            if (it->content.state == ContentDigestState::ready && it->content.sha256.size() == 64) {
                std::array<std::uint8_t, 32> checksum{};
                auto hex = [](char ch) -> int {
                    if (ch >= '0' && ch <= '9') return ch - '0';
                    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                    return -1;
                };
                bool valid_digest = true;
                for (std::size_t i = 0; i < checksum.size(); ++i) {
                    const auto hi = hex(it->content.sha256[i * 2]);
                    const auto lo = hex(it->content.sha256[i * 2 + 1]);
                    if (hi < 0 || lo < 0) { valid_digest = false; break; }
                    checksum[i] = static_cast<std::uint8_t>((hi << 4) | lo);
                }
                if (valid_digest) e.checksum = checksum;
            }
        }
        d.entries.push_back(std::move(e));
    }
    return d;
}
ModDescription describe_mounted_profile(const Profile& profile, std::span<const ModRecord> records) {
    Chain chain;
    for (const auto& layer : profile.layers) chain.layers.push_back(layer.key);
    return describe_chain(chain, records);
}
bool matching_portable_trees(const ModDescription& host, const ModDescription& local) {
    if (!host.complete || !local.complete || host.entries.size() != local.entries.size()) return false;
    for (std::size_t i = 0; i < host.entries.size(); ++i) {
        const auto& a = host.entries[i]; const auto& b = local.entries[i];
        if (!known(a) || a.kind != b.kind || a.key != b.key || a.version != b.version ||
            !a.portable_tree || a.portable_tree != b.portable_tree ||
            (a.checksum && b.checksum && a.checksum != b.checksum)) return false;
    }
    return true;
}
std::vector<DescriptionFinding> compare_descriptions(const ModDescription& saved, const ModDescription& current) {
    std::vector<DescriptionFinding> out;
    if (!saved.complete || !current.complete) out.push_back({FindingKind::incomplete, {}, {}, {}});
    const auto fully_identified = [](const ModDescription& d) {
        return d.complete && std::all_of(d.entries.begin(), d.entries.end(), known);
    };
    auto match = [](const auto& e, const auto& rows) {
        return std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return known(r) && r.kind == e.kind && r.key == e.key; });
    };
    for (const auto& e : saved.entries) {
        if (!known(e)) { out.push_back({FindingKind::unknown_identity, e, "saved", {}}); continue; }
        const auto it = match(e, current.entries);
        // An unidentified entry could be this mod, so absence is definite only when the other list is fully identified.
        if (it == current.entries.end()) {
            if (fully_identified(current)) out.push_back({FindingKind::missing, e, {}, {}});
        }
        else {
            if (!e.version || !it->version) out.push_back({FindingKind::unknown_version, e, e.version.value_or(""), it->version.value_or("")});
            else if (*e.version != *it->version) out.push_back({FindingKind::version_changed, e, *e.version, *it->version});
            if (!e.checksum || !it->checksum) out.push_back({FindingKind::checksum_unavailable, e, {}, {}});
            else if (*e.checksum != *it->checksum) out.push_back({FindingKind::content_changed, e, {}, {}});
        }
    }
    for (const auto& e : current.entries) {
        if (!known(e)) out.push_back({FindingKind::unknown_identity, e, {}, "current"});
        else if (match(e, saved.entries) == saved.entries.end() && fully_identified(saved)) out.push_back({FindingKind::extra, e, {}, {}});
    }
    std::vector<std::pair<DescriptionKeyKind, std::string>> a, b;
    for (const auto& e : saved.entries) if (known(e) && match(e, current.entries) != current.entries.end()) a.emplace_back(e.kind, e.key);
    for (const auto& e : current.entries) if (known(e) && match(e, saved.entries) != saved.entries.end()) b.emplace_back(e.kind, e.key);
    if (a != b) {
        auto order = [](const auto& keys) {
            std::string result;
            for (const auto& key : keys) { if (!result.empty()) result += ", "; result += key.second; }
            return result;
        };
        out.push_back({FindingKind::reordered, {}, order(a), order(b)});
    }
    return out;
}
bool description_needs_confirmation(const ModDescription& saved, const ModDescription& current) {
    const auto findings = compare_descriptions(saved, current);
    return std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == FindingKind::missing || finding.kind == FindingKind::extra ||
               finding.kind == FindingKind::reordered || finding.kind == FindingKind::version_changed ||
               finding.kind == FindingKind::content_changed;
    });
}
std::string describe_finding(const DescriptionFinding& f) {
    const auto& name = f.mod.name.empty() ? f.mod.key : f.mod.name;
    switch (f.kind) {
    case FindingKind::missing: return "Missing mod: " + name;
    case FindingKind::extra: return "Additional mod: " + name;
    case FindingKind::reordered: return "The common mods are in a different order.";
    case FindingKind::version_changed: return name + " reports version " + f.current_detail + "; the save used " + f.saved_detail + ".";
    case FindingKind::content_changed: return "The content checksum differs for mod: " + name;
    case FindingKind::checksum_unavailable: return "Could not compare the content checksum of mod: " + name;
    case FindingKind::unknown_identity: return "Could not compare the identity of mod: " + name;
    case FindingKind::unknown_version: return "Could not compare the reported version of mod: " + name;
    case FindingKind::incomplete: return "Mod details are incomplete.";
    }
    return {};
}
} // namespace eawr::mods
