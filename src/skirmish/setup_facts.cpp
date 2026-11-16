#include "eawr/skirmish/setup_facts.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

#include <cstring>
#include <type_traits>

namespace eawr::skirmish {
namespace {

constexpr std::uint8_t header_format = 1;
constexpr std::uint8_t entry_format = 1;
constexpr std::string_view context_tag = "eawr-setup-facts-context/v1";

// Little-endian writer/reader for the two records. A reader never trusts a length: it stops at the end of the span.
static_assert(sizeof(assets::Vec2f) == 8 && std::is_trivially_copyable_v<assets::Vec2f>, "start positions are stored as raw point bytes");

class Writer final {
public:
    void u8(const std::uint8_t value) { out_.push_back(value); }
    void u32(const std::uint32_t value) { for (unsigned shift = 0; shift < 32; shift += 8) out_.push_back(static_cast<std::uint8_t>(value >> shift)); }
    void u64(const std::uint64_t value) { for (unsigned shift = 0; shift < 64; shift += 8) out_.push_back(static_cast<std::uint8_t>(value >> shift)); }
    // The simulation boundary forbids floating types in this directory, so a point list travels as its own bytes.
    void points(const std::vector<assets::Vec2f>& value) {
        u32(static_cast<std::uint32_t>(value.size()));
        const auto at = out_.size();
        out_.resize(at + value.size() * sizeof(assets::Vec2f));
        if (!value.empty()) std::memcpy(out_.data() + at, value.data(), value.size() * sizeof(assets::Vec2f));
    }
    void text(const std::string_view value) { u32(static_cast<std::uint32_t>(value.size())); out_.insert(out_.end(), value.begin(), value.end()); }
    void bytes(const std::span<const std::byte> value) {
        u32(static_cast<std::uint32_t>(value.size()));
        for (const auto byte : value) out_.push_back(static_cast<std::uint8_t>(byte));
    }
    template<class T, class Put> void optional(const std::optional<T>& value, Put put) {
        u8(static_cast<std::uint8_t>(value ? 1 : 0));
        if (value) put(*value);
    }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(out_); }
private:
    std::vector<std::uint8_t> out_;
};

class Reader final {
public:
    explicit Reader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool done() const noexcept { return pos_ == bytes_.size(); }
    std::uint8_t u8() { return need(1) ? bytes_[pos_++] : std::uint8_t{}; }
    std::uint32_t u32() {
        if (!need(4)) return 0;
        std::uint32_t value{};
        for (unsigned shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(bytes_[pos_++]) << shift;
        return value;
    }
    std::vector<assets::Vec2f> points() {
        const auto size = count(sizeof(assets::Vec2f));
        std::vector<assets::Vec2f> value(size);
        if (size != 0 && need(size * sizeof(assets::Vec2f))) {
            std::memcpy(value.data(), bytes_.data() + pos_, size * sizeof(assets::Vec2f));
            pos_ += size * sizeof(assets::Vec2f);
        } else value.clear();
        return value;
    }
    std::string text() {
        const auto size = u32();
        if (!need(size)) return {};
        std::string value(reinterpret_cast<const char*>(bytes_.data() + pos_), size);
        pos_ += size;
        return value;
    }
    std::vector<std::byte> bytes() {
        const auto size = u32();
        if (!need(size)) return {};
        std::vector<std::byte> value(size);
        for (std::uint32_t i = 0; i < size; ++i) value[i] = static_cast<std::byte>(bytes_[pos_++]);
        return value;
    }
    // A count that cannot be honest (more items than bytes left) fails the read instead of allocating.
    std::uint32_t count(const std::size_t minimum_item_bytes) {
        const auto value = u32();
        if (ok_ && static_cast<std::uint64_t>(value) * minimum_item_bytes > bytes_.size() - pos_) ok_ = false;
        return ok_ ? value : 0U;
    }
    template<class T, class Get> std::optional<T> optional(Get get) {
        const auto present = u8();
        if (present > 1) ok_ = false;
        if (!ok_ || present == 0) return std::nullopt;
        return get();
    }
private:
    bool need(const std::size_t count) {
        if (!ok_ || count > bytes_.size() - pos_) { ok_ = false; return false; }
        return true;
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t pos_{};
    bool ok_{true};
};

void write_lobby(Writer& out, const assets::MapLobbyMetadata& lobby) {
    const auto u32 = [&](const std::uint32_t value) { out.u32(value); };
    out.optional(lobby.capacity, u32);
    out.optional(lobby.levels, u32);
    out.optional(lobby.owner, u32);
    out.optional(lobby.terrain, u32);
    out.optional(lobby.game_types, [&](const std::string& value) { out.text(value); });
    out.optional(lobby.custom, [&](const bool value) { out.u8(static_cast<std::uint8_t>(value ? 1 : 0)); });
    out.optional(lobby.new_markers, [&](const bool value) { out.u8(static_cast<std::uint8_t>(value ? 1 : 0)); });
    out.optional(lobby.start_positions, [&](const std::vector<assets::Vec2f>& value) { out.points(value); });
}

assets::MapLobbyMetadata read_lobby(Reader& in) {
    assets::MapLobbyMetadata lobby;
    const auto u32 = [&] { return in.u32(); };
    lobby.capacity = in.optional<std::uint32_t>(u32);
    lobby.levels = in.optional<std::uint32_t>(u32);
    lobby.owner = in.optional<std::uint32_t>(u32);
    lobby.terrain = in.optional<std::uint32_t>(u32);
    lobby.game_types = in.optional<std::string>([&] { return in.text(); });
    lobby.custom = in.optional<bool>([&] { return in.u8() != 0; });
    lobby.new_markers = in.optional<bool>([&] { return in.u8() != 0; });
    lobby.start_positions = in.optional<std::vector<assets::Vec2f>>([&] { return in.points(); });
    return lobby;
}

class ContextHasher final {
public:
    void text(const std::string_view value) {
        number(value.size());
        hasher_.update(std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
    }
    void number(const std::uint64_t value) {
        hasher_.update(std::span(reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)));
    }
    [[nodiscard]] std::string hex() const { return hasher_.hex_digest(); }
private:
    core::Sha256Hasher hasher_;
};

} // namespace

std::optional<SetupFactsStamp> setup_facts_stamp(const vfs::Vfs& filesystem, const vfs::AssetRecord& record) {
    const auto stamp = filesystem.source_stamp(record);
    if (!stamp) return std::nullopt;
    return SetupFactsStamp{stamp->container_size, stamp->container_mtime, stamp->entry_offset, record.size};
}

std::string setup_facts_context(const vfs::Vfs& filesystem, const data::Catalog& catalog, const std::string_view kind) {
    const auto view = filesystem.view_identity();
    if (!view) return {};
    ContextHasher hash;
    hash.text(context_tag);
    hash.text(kind);
    hash.text(*view);
    hash.number(static_cast<std::uint64_t>(catalog.profile()));
    for (const auto& file : catalog.registry_files()) {
        hash.text(file.registry_path);
        hash.text(file.included_path);
        hash.number(file.include_order);
        hash.number(file.loaded ? 1U : 0U);
        hash.text(file.outcome);
        hash.text(file.input_sha256.value_or(std::string{}));
    }
    for (const auto& root : catalog.registry_roots()) {
        hash.text(root.registry_path);
        hash.text(root.outcome);
        hash.text(root.input_sha256.value_or(std::string{}));
    }
    // The view hash holds a loose file by path and size; the files an entry is built from also by modification time.
    for (const std::string_view prefix : {"data/xml/", "data/text/"}) {
        const auto records = filesystem.enumerate(prefix);
        if (!records) return {};
        hash.text(prefix);
        hash.number(records.value().size());
        for (const auto& record : records.value()) {
            const auto stamp = filesystem.source_stamp(record);
            if (!stamp) return {};
            hash.text(record.canonical_path);
            hash.number(stamp->container_size);
            hash.number(static_cast<std::uint64_t>(stamp->container_mtime));
            hash.number(stamp->entry_offset);
            hash.number(record.size);
        }
    }
    return hash.hex();
}

std::vector<std::uint8_t> encode_setup_header_facts(const assets::Map& header) {
    Writer out;
    out.u8(header_format);
    out.optional(header.kind, [&](const assets::MapKind kind) { out.u8(static_cast<std::uint8_t>(kind)); });
    write_lobby(out, header.lobby);
    return out.take();
}

std::optional<assets::Map> decode_setup_header_facts(const std::span<const std::uint8_t> bytes) {
    Reader in(bytes);
    if (in.u8() != header_format) return std::nullopt;
    assets::Map map;
    map.kind = in.optional<assets::MapKind>([&] { return static_cast<assets::MapKind>(in.u8()); });
    if (map.kind && *map.kind != assets::MapKind::land && *map.kind != assets::MapKind::space) return std::nullopt;
    map.lobby = read_lobby(in);
    if (!in.ok() || !in.done()) return std::nullopt;
    return map;
}

std::vector<std::uint8_t> encode_setup_entry(const SetupMap& entry) {
    Writer out;
    out.u8(entry_format);
    out.text(entry.path);
    out.text(entry.name);
    out.u32(static_cast<std::uint32_t>(entry.teams.size()));
    for (const auto& team : entry.teams) {
        out.u32(team.team);
        out.u32(static_cast<std::uint32_t>(team.factions.size()));
        for (const auto& faction : team.factions) out.text(faction);
        out.optional(team.spawn_capacity, [&](const std::uint32_t value) { out.u32(value); });
    }
    out.text(entry.unavailable);
    out.bytes(entry.embedded_preview);
    write_lobby(out, entry.metadata);
    out.text(entry.display_name);
    out.optional(entry.context_name, [&](const std::string& value) { out.text(value); });
    out.u8(static_cast<std::uint8_t>(entry.domain));
    out.u32(static_cast<std::uint32_t>(entry.legacy_factions.size()));
    for (const auto& faction : entry.legacy_factions) out.text(faction);
    return out.take();
}

std::optional<SetupMap> decode_setup_entry(const std::span<const std::uint8_t> bytes) {
    Reader in(bytes);
    if (in.u8() != entry_format) return std::nullopt;
    SetupMap entry;
    entry.path = in.text();
    entry.name = in.text();
    const auto teams = in.count(9);
    entry.teams.reserve(teams);
    for (std::uint32_t i = 0; i < teams && in.ok(); ++i) {
        SetupTeam team;
        team.team = in.u32();
        const auto factions = in.count(4);
        team.factions.reserve(factions);
        for (std::uint32_t j = 0; j < factions && in.ok(); ++j) team.factions.push_back(in.text());
        team.spawn_capacity = in.optional<std::uint32_t>([&] { return in.u32(); });
        entry.teams.push_back(std::move(team));
    }
    entry.unavailable = in.text();
    entry.embedded_preview = in.bytes();
    entry.metadata = read_lobby(in);
    entry.display_name = in.text();
    entry.context_name = in.optional<std::string>([&] { return in.text(); });
    const auto domain = in.u8();
    if (domain != static_cast<std::uint8_t>(sim::tactical::BattleDomain::space)
        && domain != static_cast<std::uint8_t>(sim::tactical::BattleDomain::land)) return std::nullopt;
    entry.domain = static_cast<sim::tactical::BattleDomain>(domain);
    const auto legacy = in.count(4);
    entry.legacy_factions.reserve(legacy);
    for (std::uint32_t i = 0; i < legacy && in.ok(); ++i) entry.legacy_factions.push_back(in.text());
    if (!in.ok() || !in.done()) return std::nullopt;
    return entry;
}

} // namespace eawr::skirmish
