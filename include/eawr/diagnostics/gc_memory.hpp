#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <type_traits>
#include <tuple>
#include <vector>

namespace eawr::diagnostics {
// Read only, at a driven tick barrier. Listed storage is a lower bound: map
// node links, allocator headers, deque spare blocks and unlisted fields are
// excluded. Shared backing allocations are counted once across all rows.
class GcMemory final {
public:
    static const std::string& directory() {
        static const std::string configured = [] {
#ifdef _MSC_VER
            char* raw{};
            std::size_t length{};
            if (_dupenv_s(&raw, &length, "EAWR_GC_MEMORY_DIR") != 0 || !raw) return std::string{};
            const std::unique_ptr<char, decltype(&std::free)> owner(raw, &std::free);
            return std::string(raw);
#else
            const auto* raw = std::getenv("EAWR_GC_MEMORY_DIR");
            return raw ? std::string(raw) : std::string{};
#endif
        }();
        return configured;
    }
    static bool enabled() { return !directory().empty(); }
    std::map<std::string, std::uint64_t> rows;
    std::set<const void*> seen;
    template<class T> std::uint64_t bytes(const T& value, [[maybe_unused]] bool object_counted = false) {
        using V = std::remove_cvref_t<T>;
        if constexpr (std::is_same_v<V, std::string>) {
            const auto data = reinterpret_cast<std::uintptr_t>(value.data());
            const auto start = reinterpret_cast<std::uintptr_t>(&value);
            return data >= start && data < start + sizeof(value) ? 0 :
                (seen.insert(value.data()).second ? value.capacity() + 1 : 0);
        } else if constexpr (requires { value.get(); *value; bool(value); }) {
            if (!value || !seen.insert(value.get()).second) return 0;
            return sizeof(*value) + bytes(*value, true);
        } else if constexpr (requires { value.capacity(); value.data(); value.begin(); }) {
            if (!value.capacity() || !seen.insert(value.data()).second) return 0;
            std::uint64_t result = static_cast<std::uint64_t>(value.capacity()) * sizeof(typename V::value_type);
            for (const auto& row : value) result += bytes(row);
            return result;
        } else if constexpr (requires { std::tuple_size<V>::value; value.begin(); value.end(); }) {
            std::uint64_t result = 0;
            for (const auto& row : value) result += bytes(row);
            return result;
        } else if constexpr (requires { value.begin(); value.end(); value.size(); }) {
            // A shared pointer already deduplicated this container object.
            // Still visit its node payloads and nested vector allocations.
            if (!object_counted && !seen.insert(&value).second) return 0;
            std::uint64_t result = static_cast<std::uint64_t>(value.size()) * sizeof(typename V::value_type);
            for (const auto& row : value) result += bytes(row);
            return result;
        } else if constexpr (requires { value.first; value.second; }) {
            return bytes(value.first) + bytes(value.second);
        } else {
            std::uint64_t result = 0;
            // Nested payloads of the explicitly inventoried container rows.
#define EAWR_GC_PAYLOAD(member) if constexpr (requires { requires std::is_member_object_pointer_v<decltype(&V::member)>; }) result += bytes(value.member)
            EAWR_GC_PAYLOAD(tokens); EAWR_GC_PAYLOAD(bindings);
            EAWR_GC_PAYLOAD(name); EAWR_GC_PAYLOAD(policy); EAWR_GC_PAYLOAD(parameter); EAWR_GC_PAYLOAD(chain);
            EAWR_GC_PAYLOAD(id); EAWR_GC_PAYLOAD(raw_text); EAWR_GC_PAYLOAD(attributes); EAWR_GC_PAYLOAD(children); EAWR_GC_PAYLOAD(root);
            EAWR_GC_PAYLOAD(value); EAWR_GC_PAYLOAD(type_name); EAWR_GC_PAYLOAD(schema_type_name);
            EAWR_GC_PAYLOAD(taskforces); EAWR_GC_PAYLOAD(reserved_pads);
            EAWR_GC_PAYLOAD(units); EAWR_GC_PAYLOAD(pooled); EAWR_GC_PAYLOAD(pool_tokens); EAWR_GC_PAYLOAD(last_target);
            EAWR_GC_PAYLOAD(space); EAWR_GC_PAYLOAD(ground); EAWR_GC_PAYLOAD(starbase);
            EAWR_GC_PAYLOAD(space_units); EAWR_GC_PAYLOAD(ground_units);
            EAWR_GC_PAYLOAD(space_presence); EAWR_GC_PAYLOAD(ground_presence); EAWR_GC_PAYLOAD(nearby);
            EAWR_GC_PAYLOAD(triangles); EAWR_GC_PAYLOAD(nodes);
#undef EAWR_GC_PAYLOAD
            return result;
        }
    }
    template<class... T> void row(const std::string& name, const T&... values) {
        rows[name] += (std::uint64_t{} + ... + bytes(values));
    }
    void write(std::ostream& output) const {
        output << "{\"scope\":\"listed_storage_lower_bound_shared_allocations_deduplicated\",\"bytes\":{";
        bool first = true;
        for (const auto& [name, value] : rows) {
            output << (first ? "" : ",") << '"' << name << "\":" << value;
            first = false;
        }
        output << "}}";
    }
};
} // namespace eawr::diagnostics
