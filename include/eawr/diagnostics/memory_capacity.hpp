#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <map>
#include <vector>

namespace eawr::sim::tactical { template<class T> class CollisionBuffer; }

namespace eawr::diagnostics {

// Diagnostic capacity only: excludes allocator metadata, strings and unlisted
// nested allocations. Collision allocation addresses are retained only within an opt-in
// snapshot, never serialized. No world mutation or GC operation.
struct MemoryCapacity final {
    struct Row final {
        const char* group{}; // Static labels, never content-owned pointers.
        const char* name{};
        std::uint64_t bytes{}, elements{};
    };
    std::array<Row, 128> rows{};
    std::size_t count{};
    bool complete{true};

    void add(const char* group, const char* name, std::uint64_t byte_count, std::uint64_t elements = 0) noexcept {
        if (count == rows.size()) { complete = false; return; }
        rows[count++] = {group, name, byte_count, elements};
    }
    template<class T, class Allocator>
    static std::uint64_t bytes(const std::vector<T, Allocator>& values) noexcept {
        return static_cast<std::uint64_t>(values.capacity()) * sizeof(T);
    }
    template<class T>
    static std::uint64_t bytes(const sim::tactical::CollisionBuffer<T>& values) noexcept {
        return static_cast<std::uint64_t>(values.capacity()) * sizeof(T);
    }
    // Add logical rows separately. Each key identifies immutable backing storage,
    // including when independently queried owners are appended to one snapshot.
    // Indexed raw storage aggregates its vertices, indices and Storage object.
    void collision_buffer(const void* key, const std::uint64_t byte_count) {
        if (key != nullptr && byte_count != 0)
            collision_buffers_.try_emplace(key, byte_count);
    }
    template<class Buffer>
    void collision_buffer(const Buffer& values) {
        collision_buffer(values.data(), bytes(values));
    }
    [[nodiscard]] std::uint64_t physical_collision_bytes() const noexcept {
        std::uint64_t total = 0;
        for (const auto& entry : collision_buffers_) total += entry.second;
        return total;
    }
    // Reuse the physical registry after all owners have been appended. The census
    // has allocated its bounded storage before any owner's snapshot lock.
    template<class Census>
    void collision_census(Census& output) const noexcept {
        for (const auto& [address, byte_count] : collision_buffers_) {
            if (!output.step(Census::collision)) break;
            output.range(Census::collision, address, byte_count);
        }
        if (!complete) output.incomplete(Census::collision);
    }
    template<class Allocator>
    static std::uint64_t bytes(const std::vector<bool, Allocator>& values) noexcept {
        return (static_cast<std::uint64_t>(values.capacity()) + 7U) / 8U;
    }
    template<class T>
    void vector(const char* group, const char* name, const T& values) noexcept {
        add(group, name, bytes(values), values.size());
    }
    void append(const MemoryCapacity& other) {
        collision_buffers_.insert(other.collision_buffers_.begin(), other.collision_buffers_.end());
        for (std::size_t index = 0; index < other.count; ++index) {
            const auto& row = other.rows[index];
            add(row.group, row.name, row.bytes, row.elements);
        }
        complete = complete && other.complete;
    }
    void write(std::ostream& output) const {
        output << "{\"units\":\"bytes\",\"scope\":\"listed_capacity_excluding_strings_allocator_gpu\","
                  "\"rows_complete\":" << (complete ? "true" : "false") << ",\"rows\":[";
        for (std::size_t index = 0; index < count; ++index) {
            const auto& row = rows[index];
            output << (index ? "," : "") << "{\"group\":\"" << row.group << "\",\"name\":\"" << row.name
                   << "\",\"bytes\":" << row.bytes << ",\"elements\":" << row.elements << '}';
        }
        output << "],\"physical_collision_buffers\":{\"scope\":\"unique_listed_raw_indexed_storage_and_placed_triangles_bvh_excluding_allocator_control_blocks\","
                  "\"listed_owners_only\":true,\"overlaps_logical_owner_rows\":true,\"bytes\":" << physical_collision_bytes()
               << ",\"buffers\":" << collision_buffers_.size() << "}}";
    }
private:
    std::map<const void*, std::uint64_t> collision_buffers_;
};

// Works on the real CombatTable in each owner; never copies its geometry.
template<class Table>
void combat_capacity(MemoryCapacity& output, const char* group, const Table& table) {
    std::uint64_t meshes = 0, triangles = 0, nodes = 0, other = 0, mesh_count = 0, triangle_count = 0;
    const auto visit = [&](const auto& profiles) {
        for (const auto& profile : profiles) {
            meshes += MemoryCapacity::bytes(profile.meshes);
            mesh_count += profile.meshes.size();
            other += MemoryCapacity::bytes(profile.weapons) + MemoryCapacity::bytes(profile.target_bones)
                + MemoryCapacity::bytes(profile.hardpoints) + MemoryCapacity::bytes(profile.aimed_routes)
                + MemoryCapacity::bytes(profile.hardpoint_meshes) + MemoryCapacity::bytes(profile.death_projectiles);
            for (const auto& mesh : profile.meshes) {
                triangles += MemoryCapacity::bytes(mesh.triangles);
                nodes += MemoryCapacity::bytes(mesh.nodes);
                triangle_count += mesh.triangles.size();
                output.collision_buffer(mesh.triangles);
                output.collision_buffer(mesh.nodes);
            }
        }
    };
    visit(table.profiles);
    visit(table.retaliation_team_types);
    output.add(group, "profile_structures", MemoryCapacity::bytes(table.profiles)
        + MemoryCapacity::bytes(table.retaliation_team_types), table.profiles.size() + table.retaliation_team_types.size());
    output.add(group, "mesh_structures", meshes, mesh_count);
    output.add(group, "triangles", triangles, triangle_count);
    output.add(group, "bvh_nodes", nodes);
    output.add(group, "listed_other_profile_vectors", other);
}

} // namespace eawr::diagnostics
