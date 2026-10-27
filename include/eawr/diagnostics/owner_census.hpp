#pragma once

#include "eawr/assets/assets.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <ostream>
#include <string>
#include <type_traits>
#include <vector>

namespace eawr::diagnostics {

// Construct only at an opt-in snapshot, before acquiring any owner's lock.
// All inserts and walkers use this preallocated storage; no owner callback,
// decode, collection, or allocation is performed by an insert.
class OwnerCensus final {
public:
    enum Group : unsigned { collision, picking, xml_definitions, xml_resolved, animation,
        model_vertices, model_indices, emitter_metadata_copies, emitter_binding_copies, group_count };
    // Wire bits 8/16 are reserved for real model aliases and are never set by
    // production. Emitter metadata/bindings are copies; their actual model pin
    // is the registered unit_emitters raw wrapper. Production derived classes
    // are disjoint: that raw edge is registered VFS storage, never fallback.
    enum Owner : std::uint32_t { renderer = 1, strong_lru = 2, raw_cache = 4,
        emitter_metadata = 8, emitter_binding = 16, fallback = 32 };
    struct Range final {
        const void* address{};
        std::uint64_t bytes{}, elements{};
        std::uint32_t groups{};
    };
    struct Model final {
        const assets::Model* address{};
        std::uint64_t vertices{}, indices{}, payload{}, raw_wrappers{};
        std::uint32_t owners{};
        bool registered_vfs{};
    };
    struct Total final { std::uint64_t bytes{}, elements{}, buffers{}, strings{}, inline_strings{}; };
    // pf-mem-9: split of the animation group. references counts every Player a walker reached;
    // players the unique ones.
    struct PlayerParts final {
        std::uint64_t references{}, players{}, bones{}, tracks{}, bone_bytes{}, track_bytes{}, sample_bytes{};
        // pf-anim-elide: per-channel constant tracks (one stored value) and what the per-frame layout would hold.
        std::uint64_t constant_translation{}, constant_scale{}, constant_rotation{}, constant_visible{}, frames{},
            unelided_sample_bytes{};
    };
    void player_reference() noexcept { ++player_parts_.references; }
    [[nodiscard]] PlayerParts& player_parts() noexcept { return player_parts_; }

    explicit OwnerCensus(std::size_t range_slots = 1U << 20, std::size_t model_slots = 1U << 15,
        std::uint64_t work_limit = 8U << 20)
        : ranges_(std::max<std::size_t>(range_slots, 1)), models_(std::max<std::size_t>(model_slots, 1)),
          work_limit_(work_limit) {}

    bool step(Group group) noexcept {
        if (incomplete_ & (1U << group)) return false;
        if (work_[group]++ < work_limit_) return true;
        incomplete(group); return false;
    }
    void incomplete(Group group) noexcept { incomplete_ |= 1U << group; }
    [[nodiscard]] bool complete() const noexcept { return incomplete_ == 0 && !models_truncated_; }
    [[nodiscard]] const Total& total(Group group) const noexcept { return totals_[group]; }

    // Returns false for an already registered range in this group (also useful
    // for deduplicating immutable Player/catalog trees before descending).
    bool range(Group group, const void* address, std::uint64_t bytes, std::uint64_t elements = 0) noexcept {
        if (ranges_saturated_) { incomplete(group); return false; }
        if (!address || !step(group)) return false;
        const auto start = hash(address) % ranges_.size();
        for (std::size_t probe = 0; probe < std::min<std::size_t>(ranges_.size(), 256); ++probe) {
            auto& slot = ranges_[(start + probe) % ranges_.size()];
            if (slot.address && slot.address != address) continue;
            const auto bit = 1U << group;
            if (slot.groups & bit) return false;
            if (!slot.address) { slot.address = address; slot.bytes = bytes; slot.elements = elements; }
            else if (slot.bytes != bytes) incomplete(group);
            slot.groups |= bit;
            totals_[group].bytes += bytes;
            totals_[group].elements += elements;
            ++totals_[group].buffers;
            return true;
        }
        ranges_saturated_ = true;
        incomplete(group); return false;
    }
    template<class T> bool vector(Group group, const std::vector<T>& values) noexcept {
        return values.capacity() && range(group, values.data(),
            static_cast<std::uint64_t>(values.capacity()) * sizeof(T), values.size());
    }
    void string(Group group, const std::string& value) noexcept {
        // SSO storage lies inside the string object and is already in the
        // containing structure. Never report it as another allocation.
        const auto data = reinterpret_cast<std::uintptr_t>(value.data());
        const auto object = reinterpret_cast<std::uintptr_t>(&value);
        if (data >= object && data - object < sizeof(value)) { ++totals_[group].inline_strings; return; }
        if (range(group, value.data(), value.capacity() + 1, value.size())) ++totals_[group].strings;
    }
    void models_incomplete() noexcept { models_truncated_ = true; }
    // Labels an already registered owner, not an inferred reference-count edge.
    void emitter_raw_wrapper(const void* wrapper) noexcept { emitter_wrappers_ |= wrapper_bit(wrapper); }
    void model(const assets::Model* value, std::uint32_t owner, const void* wrapper = nullptr,
        bool registered_vfs = false) noexcept {
        if (!value) return;
        if (model_slots_saturated_) return;
        const auto start = hash(value) % models_.size();
        for (std::size_t probe = 0; probe < std::min<std::size_t>(models_.size(), 256); ++probe) {
            auto& slot = models_[(start + probe) % models_.size()];
            if (slot.address && slot.address != value) continue;
            if (!slot.address) {
                slot.address = value;
                const auto bytes = [](const auto& values) noexcept {
                    return static_cast<std::uint64_t>(values.capacity()) * sizeof(typename std::remove_cvref_t<decltype(values)>::value_type);
                };
                slot.payload = bytes(value->bones) + bytes(value->meshes) + bytes(value->lights)
                    + bytes(value->proxies) + bytes(value->dazzles) + bytes(value->notices);
                for (const auto& mesh : value->meshes) {
                    if (!step(model_vertices)) { models_truncated_ = true; break; }
                    slot.payload += bytes(mesh.submeshes);
                    for (const auto& surface : mesh.submeshes) {
                        if (!step(model_vertices)) { models_truncated_ = true; break; }
                        slot.vertices += static_cast<std::uint64_t>(surface.vertices.capacity()) * sizeof(assets::Vertex);
                        slot.indices += static_cast<std::uint64_t>(surface.indices.capacity()) * sizeof(decltype(surface.indices)::value_type);
                        slot.payload += bytes(surface.vertices) + bytes(surface.indices) + bytes(surface.parameters) + bytes(surface.skin_bones);
                        vector(model_vertices, surface.vertices);
                        vector(model_indices, surface.indices);
                    }
                }
            }
            slot.owners |= owner;
            slot.registered_vfs = slot.registered_vfs || registered_vfs;
            if (wrapper) {
                slot.raw_wrappers |= wrapper_bit(wrapper);
            }
            return;
        }
        models_truncated_ = true;
        model_slots_saturated_ = true;
    }
    void write(std::ostream& output) const {
        static constexpr const char* names[] = {"collision", "picking", "xml_definitions", "xml_resolved",
            "animation_players", "model_vertices", "model_indices", "emitter_metadata_copies", "emitter_binding_copies"};
        output << "{\"diagnostic\":true,\"listed_owners_only\":true,\"complete\":" << (complete() ? "true" : "false")
            << ",\"scope\":\"requested_capacity_excluding_allocator_map_links_buckets_gpu\",\"preallocated_bytes\":"
            << ranges_.size() * sizeof(Range) + models_.size() * sizeof(Model) << ",\"groups\":[";
        for (unsigned group = 0; group < group_count; ++group) {
            const auto& row = totals_[group];
            output << (group ? "," : "") << "{\"name\":\"" << names[group] << "\",\"bytes\":" << row.bytes
                << ",\"elements\":" << row.elements << ",\"buffers\":" << row.buffers << ",\"heap_strings\":" << row.strings
                << ",\"inline_strings\":" << row.inline_strings << ",\"complete\":"
                << ((incomplete_ & (1U << group)) ? "false" : "true") << '}';
        }
        output << "],\"animation_parts\":{\"player_references\":" << player_parts_.references << ",\"players\":"
            << player_parts_.players << ",\"bones\":" << player_parts_.bones << ",\"tracks\":" << player_parts_.tracks
            << ",\"bone_bytes\":" << player_parts_.bone_bytes << ",\"track_bytes\":" << player_parts_.track_bytes
            << ",\"sample_bytes\":" << player_parts_.sample_bytes
            << ",\"constant_translation\":" << player_parts_.constant_translation
            << ",\"constant_scale\":" << player_parts_.constant_scale
            << ",\"constant_rotation\":" << player_parts_.constant_rotation
            << ",\"constant_visible\":" << player_parts_.constant_visible
            << ",\"track_frames\":" << player_parts_.frames
            << ",\"unelided_sample_bytes\":" << player_parts_.unelided_sample_bytes
            << "},\"owner_bits\":{\"renderer_recipe\":1,\"strong_lru\":2,\"raw_cache\":4,"
            "\"emitter_metadata\":8,\"emitter_binding\":16,\"fallback\":32},\"models_complete\":"
            << (models_truncated_ ? "false" : "true") << ",\"raw_wrapper_count\":" << wrapper_count_ << ",\"raw_wrappers\":[";
        for (std::size_t index = 0; index < wrapper_count_; ++index) {
            output << (index ? "," : "") << "{\"bit_index\":" << index << ",\"identity\":"
                << reinterpret_cast<std::uintptr_t>(wrappers_[index]) << ",\"role\":\""
                << ((emitter_wrappers_ & (std::uint64_t{1} << index)) ? "unit_emitters" : "unlabelled_raw_cache") << "\"}";
        }
        output << "],\"models\":[";
        bool first = true;
        std::uint64_t cache_only = 0, cache_payload = 0, renderer_only = 0, renderer_emitter = 0, fallback_bytes = 0;
        for (const auto& row : models_) {
            if (!row.address) continue;
            const auto owners = row.owners | ((row.owners & renderer) && !row.registered_vfs ? fallback : 0U);
            const auto geometry = row.vertices + row.indices;
            if (owners == strong_lru) { cache_only += geometry; cache_payload += row.payload; }
            if (owners == renderer) renderer_only += geometry;
            if ((owners & renderer) && ((owners & (emitter_metadata | emitter_binding)) || (row.raw_wrappers & emitter_wrappers_)))
                renderer_emitter += geometry;
            if (owners & fallback) fallback_bytes += geometry;
            output << (first ? "" : ",") << "{\"identity\":" << reinterpret_cast<std::uintptr_t>(row.address)
                << ",\"vertex_capacity\":" << row.vertices << ",\"index_capacity\":" << row.indices
                << ",\"listed_payload_capacity\":" << row.payload
                << ",\"owner_mask\":" << owners << ",\"raw_wrapper_mask\":" << row.raw_wrappers << '}';
            first = false;
        }
        output << "],\"derived_geometry_bytes\":{\"cache_only\":" << cache_only << ",\"renderer_only\":" << renderer_only
            << ",\"renderer_emitter\":" << renderer_emitter << ",\"fallback\":" << fallback_bytes
            << "},\"cache_only_listed_payload_bytes\":" << cache_payload
            << ",\"fallback_means_renderer_model_without_registered_vfs_identity\":true"
            << ",\"renderer_emitter_includes_registered_emitter_raw_wrapper\":true"
            << ",\"derived_are_listed_owner_classifications_not_proven_releasable_bytes\":true}";
    }
private:
    std::uint64_t wrapper_bit(const void* wrapper) noexcept {
        if (!wrapper) return 0;
        std::size_t index = 0;
        while (index < wrapper_count_ && wrappers_[index] != wrapper) ++index;
        if (index == wrappers_.size()) { models_truncated_ = true; return 0; }
        if (index == wrapper_count_) wrappers_[wrapper_count_++] = wrapper;
        return std::uint64_t{1} << index;
    }
    static std::size_t hash(const void* address) noexcept {
        auto bits = reinterpret_cast<std::uintptr_t>(address);
        bits ^= bits >> 17; bits *= static_cast<std::uintptr_t>(0xed5ad4bbU); bits ^= bits >> 11;
        return static_cast<std::size_t>(bits);
    }
    std::vector<Range> ranges_;
    std::vector<Model> models_;
    std::array<Total, group_count> totals_{};
    PlayerParts player_parts_{};
    std::array<std::uint64_t, group_count> work_{};
    std::array<const void*, 64> wrappers_{};
    std::size_t wrapper_count_{};
    std::uint64_t work_limit_{};
    std::uint64_t emitter_wrappers_{};
    std::uint32_t incomplete_{};
    bool models_truncated_{};
    bool ranges_saturated_{}, model_slots_saturated_{};
};
} // namespace eawr::diagnostics
