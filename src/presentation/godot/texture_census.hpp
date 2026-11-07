#pragma once

#include "eawr/assets/assets.hpp"
#include "gpu_memory_ledger.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <map>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend::detail {

// Opt-in texture owner census (--eawr-texture-census). One snapshot lists every live GPU texture with
// its owner, shape, bytes and CPU snapshot, then aggregates it. Presentation diagnostics only: nothing
// here feeds the simulation, and with the switch off no record is kept and no snapshot is built.
enum class TextureOwnerKind : std::uint8_t { model, particle, shared_named, shared_array, unowned, count };

[[nodiscard]] constexpr std::string_view owner_kind_name(const TextureOwnerKind value) noexcept {
    switch (value) {
    case TextureOwnerKind::model: return "model";
    case TextureOwnerKind::particle: return "particle";
    case TextureOwnerKind::shared_named: return "shared_named";
    case TextureOwnerKind::shared_array: return "shared_array";
    default: return "unowned";
    }
}

struct TextureCensusRow final {
    TextureOwnerKind owner_kind{TextureOwnerKind::unowned};
    // The first model path (lowest renderer asset id) holding the texture, the shared name, or the
    // particle texture key.
    std::string owner;
    // Role of the texture for its first model owner: "base" or the material binding name.
    std::string role;
    // Canonical path and layer of the decoded source; empty when no recipe still knows it.
    std::string path, source_id, layer_id;
    // Model resources (or backend references) that hold this one GPU texture.
    std::uint64_t shared_by{1};
    std::uint64_t key{}, check{};
    assets::PixelFormat format{};
    std::uint32_t width{}, height{}, mips{};
    std::uint64_t device_bytes{};
    bool compressed{};
    // The recipe's decoded CPU copy of the texture, when it is still held.
    bool cpu_snapshot_held{};
    std::uint64_t cpu_bytes{};
    // False while a demand-only resource has no GPU copy: the row then counts only its CPU snapshot.
    bool resident{true};
};

// Aggregation limits: the report lists the largest textures and duplicate groups only.
inline constexpr std::size_t texture_census_top = 50;
inline constexpr std::size_t texture_census_duplicate_groups = 20;
inline constexpr std::uint64_t texture_census_large_uncompressed = 1U << 20U;

class TextureCensus final {
public:
    void add(TextureCensusRow row) { rows_.push_back(std::move(row)); }
    [[nodiscard]] const std::vector<TextureCensusRow>& rows() const noexcept { return rows_; }

    struct Group final {
        std::string label;
        std::uint64_t copies{}, bytes_each{}, wasted{};
    };
    struct Summary final {
        std::uint64_t textures{}, device_bytes{}, cpu_bytes{}, cpu_held{};
        std::uint64_t uncompressed_large{}, uncompressed_large_bytes{};
        std::array<std::uint64_t, static_cast<std::size_t>(TextureOwnerKind::count)> kind_count{}, kind_bytes{},
            kind_cpu{}, kind_compressed{};
        std::map<std::string, std::array<std::uint64_t, 3>> by_format; // count, device bytes, cpu bytes
        std::vector<Group> content_groups, path_groups;
        std::uint64_t content_wasted{}, path_wasted{};
    };

    [[nodiscard]] Summary summarize() const {
        Summary out;
        std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<const TextureCensusRow*>> by_content;
        std::map<std::string, std::vector<const TextureCensusRow*>> by_path;
        for (const TextureCensusRow& row : rows_) {
            const auto cls = static_cast<std::size_t>(row.owner_kind);
            ++out.textures;
            out.device_bytes += row.device_bytes;
            out.cpu_bytes += row.cpu_bytes;
            out.cpu_held += row.cpu_snapshot_held ? 1U : 0U;
            ++out.kind_count[cls];
            out.kind_bytes[cls] += row.device_bytes;
            out.kind_cpu[cls] += row.cpu_bytes;
            if (row.compressed) out.kind_compressed[cls] += row.device_bytes;
            auto& format = out.by_format[std::string(assets::to_string(row.format))];
            ++format[0];
            format[1] += row.device_bytes;
            format[2] += row.cpu_bytes;
            if (!row.compressed && row.device_bytes > texture_census_large_uncompressed) {
                ++out.uncompressed_large;
                out.uncompressed_large_bytes += row.device_bytes;
            }
            if (row.resident) by_content[{row.key, row.check}].push_back(&row);
            // A dormant row has no GPU copy, so it can neither duplicate nor vary a resident one.
            if (row.resident && !row.path.empty()) by_path[row.path].push_back(&row);
        }
        // The same pixels held as separate GPU copies by more than one owner class (the pool shares them
        // only among models).
        for (const auto& [key, group] : by_content) {
            static_cast<void>(key);
            if (group.size() < 2) continue;
            const auto& first = *group.front();
            out.content_groups.push_back({first.path.empty() ? first.owner : first.path, group.size(),
                first.device_bytes, (group.size() - 1) * first.device_bytes});
            out.content_wasted += out.content_groups.back().wasted;
        }
        // One canonical path that reached the GPU as several distinct contents (another layer's file,
        // another decode): every copy but the largest is the redundancy.
        for (const auto& [path, group] : by_path) {
            // Identical pixels under one path are a content duplicate (counted above), not a variant.
            std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> contents;
            for (const auto* row : group) contents.emplace(std::pair{row->key, row->check}, row->device_bytes);
            if (contents.size() < 2) continue;
            std::uint64_t total = 0, largest = 0;
            for (const auto& [content, bytes] : contents) {
                static_cast<void>(content);
                total += bytes;
                largest = std::max(largest, bytes);
            }
            out.path_groups.push_back({path, contents.size(), largest, total - largest});
            out.path_wasted += total - largest;
        }
        const auto order = [](const Group& a, const Group& b) {
            return a.wasted != b.wasted ? a.wasted > b.wasted : a.label < b.label;
        };
        std::sort(out.content_groups.begin(), out.content_groups.end(), order);
        std::sort(out.path_groups.begin(), out.path_groups.end(), order);
        return out;
    }

    // `phase` labels the snapshot; the optional ledger totals cross-check the census against the
    // renderer's own GPU texture ledger.
    void write(std::ostream& output, const std::string_view phase, const GpuCategoryTotals* ledger = nullptr) const {
        const Summary summary = summarize();
        output << "{\"diagnostic\":true,\"scope\":\"live_gpu_textures_one_shot_snapshot\",\"phase\":" << quoted(phase)
               << ",\"textures\":" << summary.textures << ",\"device_bytes\":" << summary.device_bytes
               << ",\"cpu_snapshots\":{\"held\":" << summary.cpu_held << ",\"bytes\":" << summary.cpu_bytes << '}'
               << ",\"uncompressed_over_1mib\":{\"count\":" << summary.uncompressed_large << ",\"bytes\":"
               << summary.uncompressed_large_bytes << '}';
        if (ledger) {
            output << ",\"ledger\":{\"live\":" << ledger->live << ",\"live_bytes\":" << ledger->live_bytes
                   << ",\"unique\":" << ledger->unique << ",\"unique_bytes\":" << ledger->unique_bytes
                   << ",\"compressed_bytes\":" << ledger->compressed_bytes << ",\"uncompressed_bytes\":"
                   << ledger->uncompressed_bytes << ",\"uploads\":" << ledger->uploads << ",\"reuses\":"
                   << ledger->reuses << '}';
        }
        output << ",\"by_owner_kind\":[";
        bool comma = false;
        for (std::size_t cls = 0; cls < summary.kind_count.size(); ++cls) {
            if (summary.kind_count[cls] == 0) continue;
            output << (comma ? "," : "") << "{\"owner_kind\":\""
                   << owner_kind_name(static_cast<TextureOwnerKind>(cls)) << "\",\"textures\":" << summary.kind_count[cls]
                   << ",\"device_bytes\":" << summary.kind_bytes[cls] << ",\"compressed_bytes\":"
                   << summary.kind_compressed[cls] << ",\"uncompressed_bytes\":"
                   << summary.kind_bytes[cls] - summary.kind_compressed[cls] << ",\"cpu_bytes\":" << summary.kind_cpu[cls]
                   << '}';
            comma = true;
        }
        output << "],\"by_format\":[";
        std::vector<std::pair<std::string, std::array<std::uint64_t, 3>>> formats(summary.by_format.begin(),
                                                                               summary.by_format.end());
        std::sort(formats.begin(), formats.end(), [](const auto& a, const auto& b) {
            return a.second[1] != b.second[1] ? a.second[1] > b.second[1] : a.first < b.first;
        });
        comma = false;
        for (const auto& [name, value] : formats) {
            output << (comma ? "," : "") << "{\"format\":" << quoted(name) << ",\"textures\":" << value[0]
                   << ",\"device_bytes\":" << value[1] << ",\"cpu_bytes\":" << value[2] << '}';
            comma = true;
        }
        output << "],\"top\":[";
        std::vector<const TextureCensusRow*> ranked;
        ranked.reserve(rows_.size());
        for (const TextureCensusRow& row : rows_) ranked.push_back(&row);
        const auto bigger = [](const TextureCensusRow* a, const TextureCensusRow* b) {
            const auto weight = [](const TextureCensusRow& row) { return row.device_bytes + row.cpu_bytes; };
            if (weight(*a) != weight(*b)) return weight(*a) > weight(*b);
            return std::tie(a->owner, a->path, a->role, a->key, a->check, a->owner_kind)
                < std::tie(b->owner, b->path, b->role, b->key, b->check, b->owner_kind);
        };
        const std::size_t limit = std::min(ranked.size(), texture_census_top);
        std::partial_sort(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(limit), ranked.end(), bigger);
        for (std::size_t index = 0; index < limit; ++index) {
            const TextureCensusRow& row = *ranked[index];
            output << (index ? "," : "") << "{\"owner_kind\":\"" << owner_kind_name(row.owner_kind)
                   << "\",\"owner\":" << quoted(row.owner) << ",\"role\":" << quoted(row.role)
                   << ",\"path\":" << quoted(row.path) << ",\"source\":" << quoted(row.source_id)
                   << ",\"layer\":" << quoted(row.layer_id) << ",\"format\":" << quoted(assets::to_string(row.format))
                   << ",\"width\":" << row.width << ",\"height\":" << row.height << ",\"mips\":" << row.mips
                   << ",\"device_bytes\":" << row.device_bytes << ",\"cpu_bytes\":" << row.cpu_bytes
                   << ",\"cpu_snapshot_held\":" << (row.cpu_snapshot_held ? "true" : "false")
                   << ",\"shared_by\":" << row.shared_by << ",\"resident\":" << (row.resident ? "true" : "false") << '}';
        }
        output << "],\"duplicates\":{\"same_content_across_owners\":";
        write_groups(output, summary.content_groups, summary.content_wasted);
        output << ",\"same_path_distinct_content\":";
        write_groups(output, summary.path_groups, summary.path_wasted);
        output << "}}";
    }

private:
    static void write_groups(std::ostream& output, const std::vector<Group>& groups, const std::uint64_t wasted) {
        output << "{\"groups\":" << groups.size() << ",\"wasted_bytes\":" << wasted << ",\"top\":[";
        const std::size_t limit = std::min(groups.size(), texture_census_duplicate_groups);
        for (std::size_t index = 0; index < limit; ++index) {
            output << (index ? "," : "") << "{\"label\":" << quoted(groups[index].label) << ",\"copies\":"
                   << groups[index].copies << ",\"bytes_each\":" << groups[index].bytes_each << ",\"wasted_bytes\":"
                   << groups[index].wasted << '}';
        }
        output << "]}";
    }
    [[nodiscard]] static std::string quoted(const std::string_view text) {
        std::string result = "\"";
        for (const char c : text) {
            switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20U) {
                    constexpr char hex[] = "0123456789abcdef";
                    result += "\\u00";
                    result += hex[(static_cast<unsigned char>(c) >> 4U) & 0xFU];
                    result += hex[static_cast<unsigned char>(c) & 0xFU];
                } else result += c;
            }
        }
        result += '"';
        return result;
    }

    std::vector<TextureCensusRow> rows_;
};

// GPU textures that no model resource holds (shared named textures and arrays, particle textures)
// are noted here as they are created, but only while the census switch is on. A model texture is
// found at snapshot time from the renderer's own pool instead, so it needs no record.
class TextureCensusRegistry final {
public:
    [[nodiscard]] static std::atomic<bool>& enabled() noexcept {
        static std::atomic<bool> value{false};
        return value;
    }
    [[nodiscard]] static TextureCensusRegistry& instance() {
        static TextureCensusRegistry registry;
        return registry;
    }

    // `owner` is the renderer that holds a shared texture; null for particle textures, which belong to
    // process-wide particle backends and appear in every snapshot.
    void add(const void* owner, const TextureOwnerKind owner_kind, const std::string& label, const GpuItem& item,
             const assets::Source* source = nullptr) {
        if (!enabled().load(std::memory_order_relaxed)) return;
        std::lock_guard lock(mutex_);
        auto& entry = entries_[{owner, owner_kind, label, item.key, item.check}];
        if (entry.references++ != 0) return;
        entry.row.owner_kind = owner_kind;
        entry.row.owner = label;
        entry.row.key = item.key;
        entry.row.check = item.check;
        entry.row.format = item.format;
        entry.row.width = item.width;
        entry.row.height = item.height;
        entry.row.mips = item.mips;
        entry.row.device_bytes = item.bytes;
        entry.row.compressed = item.compressed;
        entry.row.role = owner_kind == TextureOwnerKind::particle ? "particle" : "shared";
        if (source) {
            entry.row.path = source->logical_path;
            entry.row.source_id = source->source_id;
            entry.row.layer_id = source->layer_id;
        }
    }

    void remove(const void* owner, const TextureOwnerKind owner_kind, const std::string& label, const GpuItem& item) {
        if (!enabled().load(std::memory_order_relaxed)) return;
        std::lock_guard lock(mutex_);
        const auto found = entries_.find({owner, owner_kind, label, item.key, item.check});
        if (found != entries_.end() && --found->second.references == 0) entries_.erase(found);
    }

    // Adds the entries of `owner` and the process-wide ones.
    void append(TextureCensus& census, const void* owner) const {
        std::lock_guard lock(mutex_);
        for (const auto& [key, entry] : entries_) {
            if (std::get<0>(key) != owner && std::get<0>(key) != nullptr) continue;
            // Each reference is its own GPU copy: separate backends do not share a texture.
            for (std::uint64_t copy = 0; copy < entry.references; ++copy) census.add(entry.row);
        }
    }

private:
    struct Entry final {
        TextureCensusRow row;
        std::uint64_t references{};
    };
    mutable std::mutex mutex_;
    std::map<std::tuple<const void*, TextureOwnerKind, std::string, std::uint64_t, std::uint64_t>, Entry> entries_;
};

// Renderers alive while the census switch is on, so a load-phase snapshot (galaxy ready, battle load
// start, stage boundaries) can walk every renderer without a probe handed out per creation site.
// Registration happens in the renderer's constructor only with the switch on; walks run on the main thread.
class TextureCensusRenderers final {
public:
    [[nodiscard]] static TextureCensusRenderers& instance() {
        static TextureCensusRenderers renderers;
        return renderers;
    }
    void add(const void* renderer) {
        std::lock_guard lock(mutex_);
        live_.push_back(renderer);
    }
    void remove(const void* renderer) {
        std::lock_guard lock(mutex_);
        live_.erase(std::remove(live_.begin(), live_.end(), renderer), live_.end());
    }
    [[nodiscard]] std::vector<const void*> live() const {
        std::lock_guard lock(mutex_);
        return live_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<const void*> live_; // creation order
};

} // namespace eawr::presentation::godot_backend::detail
