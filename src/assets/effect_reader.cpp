#include "eawr/assets/effect.hpp"
#include "asset_internal.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace eawr::assets {
namespace {
struct Stop final {
    std::string_view code;
    const char* message;
    std::size_t offset;
};
[[noreturn]] void stop(const std::string_view code, const char* message, const std::size_t offset) {
    throw Stop{code, message, offset};
}
bool sampler(const EffectValueKind kind) {
    return kind >= EffectValueKind::sampler && kind <= EffectValueKind::sampler_cube;
}
bool texture(const EffectValueKind kind) {
    return kind >= EffectValueKind::texture && kind <= EffectValueKind::texture_cube;
}
bool compatible(const EffectValueKind a, const EffectValueKind b) {
    return a == b || (texture(a) && texture(b) &&
                     (a == EffectValueKind::texture || b == EffectValueKind::texture));
}

// EFX-01..07: all offsets are checked before use, and all graph walks use heap
// frames/indices. No effect state or program is executed by this reader.
class EffectReader final {
    struct CachedType { std::size_t index{}, end{}; bool finished{}; };
    struct TypeFrame { std::size_t offset{}, cursor{}, index{}; std::uint32_t remaining{}; };
    struct Completed { std::size_t index{}, end{}; };
    struct ValueFrame {
        std::size_t type{}, start{}, cursor{}, index{}, element{}, member{}, state{};
        bool prepared{};
        std::optional<std::uint32_t> state_count;
        std::optional<Completed> child;
        std::array<std::uint32_t, 4> pending_state{};
    };
    std::span<const std::byte> input_;
    std::span<const std::byte> pool_;
    EffectView result_;
    std::size_t metadata_left_;
    std::size_t work_left_;
    std::map<std::size_t, CachedType> types_;
    std::map<std::size_t, std::string_view> strings_;
    std::map<std::size_t, std::vector<std::size_t>> names_;

    void step(const std::size_t amount, const std::size_t at) {
        if (amount > work_left_) stop(diagnostic_codes::limit, "compiled-effect input-relative work budget exceeded", at);
        work_left_ -= amount;
    }
    void metadata(const std::size_t bytes, const std::size_t at) {
        if (bytes > metadata_left_) stop(diagnostic_codes::limit, "compiled-effect metadata budget exceeded", at);
        metadata_left_ -= bytes;
    }
    std::size_t multiply(const std::size_t a, const std::size_t b, const std::size_t at) {
        std::size_t value{};
        if (!detail::checked_multiply(a, b, value)) stop(diagnostic_codes::bounds, "compiled-effect size multiplication overflow", at);
        return value;
    }
    template<class T> void metadata_items(const std::size_t count, const std::size_t at) {
        metadata(multiply(count, sizeof(T), at), at);
    }
    static void range(const std::span<const std::byte> bytes, const std::size_t offset,
                      const std::size_t count, const std::size_t base) {
        if (offset > bytes.size() || count > bytes.size() - offset)
            stop(diagnostic_codes::bounds, "compiled-effect range exceeds its enclosing span", base + std::min(offset, bytes.size()));
    }
    std::uint32_t word(const std::span<const std::byte> bytes, const std::size_t offset,
                       const std::size_t base) {
        step(1U, base + offset);
        if (offset % 4U != 0U) stop(diagnostic_codes::structure, "compiled-effect word offset is unaligned", base + offset);
        range(bytes, offset, 4U, base);
        std::uint32_t value{};
        for (unsigned i = 0U; i < 4U; ++i)
            value |= std::uint32_t(std::to_integer<unsigned char>(bytes[offset + i])) << (8U * i);
        return value;
    }
    std::uint32_t pool_word(const std::size_t offset) { return word(pool_, offset, 8U); }
    std::uint32_t take(std::size_t& cursor) {
        const auto value = word(input_, cursor, 0U); cursor += 4U; return value;
    }
    std::span<const std::byte> pool_bytes(const std::size_t offset, const std::size_t count) {
        range(pool_, offset, count, 8U); return pool_.subspan(offset, count);
    }
    std::span<const std::byte> payload(std::size_t& cursor, const std::size_t size) {
        if (size > std::numeric_limits<std::size_t>::max() - 3U)
            stop(diagnostic_codes::bounds, "compiled-effect payload padding overflows", cursor);
        const auto padded = (size + 3U) & ~std::size_t{3U};
        range(input_, cursor, padded, 0U);
        const auto bytes = input_.subspan(cursor, size); cursor += padded; return bytes;
    }
    void count(const std::uint32_t number, const std::size_t cursor, const std::size_t minimum) {
        range(input_, cursor, 0U, 0U);
        if (number > (input_.size() - cursor) / minimum)
            stop(diagnostic_codes::bounds, "compiled-effect count exceeds remaining records", cursor);
    }
    std::string_view terminated(const std::span<const std::byte> bytes, const std::size_t at) {
        step(bytes.size(), at);
        if (bytes.empty() || bytes.back() != std::byte{} ||
            std::find(bytes.begin(), bytes.end() - 1, std::byte{}) != bytes.end() - 1)
            stop(diagnostic_codes::structure, "compiled-effect string lacks a single terminal zero", at);
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size() - 1U};
    }
    std::string_view text(const std::size_t offset) {
        if (offset == 0U) return {};
        if (const auto found = strings_.find(offset); found != strings_.end()) return found->second;
        const auto length = pool_word(offset);
        const auto padded = (std::size_t{length} + 3U) & ~std::size_t{3U};
        range(pool_, offset + 4U, padded, 8U);
        const auto value = terminated(pool_.subspan(offset + 4U, length), offset + 12U);
        metadata(64U, offset + 8U); strings_.emplace(offset, value); return value;
    }
    std::size_t type(const std::size_t offset) {
        if (offset == 0U) stop(diagnostic_codes::index, "compiled-effect type references the null sentinel", 8U);
        if (const auto found = types_.find(offset); found != types_.end()) {
            if (!found->second.finished) stop(diagnostic_codes::hierarchy_cycle, "compiled-effect type cycle", offset + 8U);
            return found->second.index;
        }
        std::vector<TypeFrame> frames;
        const auto enter = [&](const std::size_t at) {
            EffectType node;
            const auto kind = pool_word(at), category = pool_word(at + 4U);
            if (kind > static_cast<std::uint32_t>(EffectValueKind::vertex_program) || category > 5U)
                stop(diagnostic_codes::unsupported, "compiled-effect type kind or class is unsupported", at + 8U);
            if (category == 3U)
                stop(diagnostic_codes::unsupported, "compiled-effect column-matrix class is unsupported", at + 12U);
            node.kind = static_cast<EffectValueKind>(kind);
            node.value_class = static_cast<EffectValueCategory>(category);
            node.name = text(pool_word(at + 8U)); node.semantic = text(pool_word(at + 12U));
            node.array_count = pool_word(at + 16U);
            auto cursor = at + 20U;
            std::uint32_t members{};
            if (category <= 3U) {
                if (kind < 1U || kind > 3U) stop(diagnostic_codes::unsupported, "compiled-effect numeric kind is unsupported", at + 8U);
                const auto first = pool_word(cursor), second = pool_word(cursor + 4U); cursor += 8U;
                node.rows = category == 2U ? first : second;
                node.columns = category == 2U ? second : first;
                if (node.columns == 0U || node.rows == 0U) stop(diagnostic_codes::structure, "compiled-effect numeric shape is empty", at + 8U);
            } else if (category == 5U) {
                if (kind != 0U) stop(diagnostic_codes::unsupported, "compiled-effect struct kind is unsupported", at + 8U);
                members = pool_word(cursor); cursor += 4U;
                range(pool_, cursor, 0U, 8U);
                if (members > (pool_.size() - cursor) / 20U)
                    stop(diagnostic_codes::bounds, "compiled-effect member count exceeds type bytes", cursor + 8U);
            } else if (kind < 4U) {
                stop(diagnostic_codes::unsupported, "compiled-effect object kind is unsupported", at + 8U);
            }
            metadata(sizeof(EffectType) + sizeof(TypeFrame) + 64U, at + 8U);
            const auto index = result_.types.size(); result_.types.push_back(std::move(node));
            types_.emplace(at, CachedType{index, 0U, false}); frames.push_back({at, cursor, index, members});
        };
        enter(offset);
        while (!frames.empty()) {
            step(1U, frames.back().cursor + 8U);
            auto& frame = frames.back();
            if (frame.remaining == 0U) {
                auto& cached = types_.at(frame.offset); cached.end = frame.cursor; cached.finished = true;
                frames.pop_back(); continue;
            }
            const auto found = types_.find(frame.cursor);
            if (found == types_.end()) { enter(frame.cursor); continue; }
            if (!found->second.finished) stop(diagnostic_codes::hierarchy_cycle, "compiled-effect type cycle", frame.cursor + 8U);
            metadata_items<std::size_t>(1U, frame.cursor + 8U);
            result_.types[frame.index].members.push_back(found->second.index);
            frame.cursor = found->second.end; --frame.remaining;
        }
        return types_.at(offset).index;
    }
    void object_use(const std::uint32_t slot, const EffectValueKind kind, const std::size_t at) {
        if (slot >= result_.objects.size()) stop(diagnostic_codes::index, "compiled-effect object slot is out of range", at);
        if (slot == 0U) return;
        auto& object = result_.objects[slot];
        if (object.kind && !compatible(*object.kind, kind))
            stop(diagnostic_codes::structure, "compiled-effect object has conflicting kinds", at);
        if (!object.kind || *object.kind == EffectValueKind::texture) object.kind = kind;
    }
    std::size_t value(const std::size_t type_index, const std::size_t offset) {
        std::vector<ValueFrame> frames;
        std::set<std::pair<std::size_t, std::size_t>> active;
        const auto enter = [&](const std::size_t ti, const std::size_t at) {
            if (at % 4U != 0U)
                stop(diagnostic_codes::structure, "compiled-effect default offset is unaligned", at + 8U);
            if (!active.emplace(ti, at).second)
                stop(diagnostic_codes::hierarchy_cycle, "compiled-effect default-value cycle", at + 8U);
            metadata(sizeof(EffectValue) + sizeof(ValueFrame) + 64U, at + 8U);
            EffectValue node; node.type = ti;
            const auto index = result_.values.size(); result_.values.push_back(std::move(node));
            ValueFrame frame; frame.type = ti; frame.start = at; frame.cursor = at; frame.index = index;
            frames.push_back(frame);
            return index;
        };
        const auto root = enter(type_index, offset);
        const auto finish = [&]() {
            const auto frame = frames.back();
            result_.values[frame.index].bytes = pool_bytes(frame.start, frame.cursor - frame.start);
            active.erase({frame.type, frame.start}); frames.pop_back();
            if (!frames.empty()) frames.back().child = Completed{frame.index, frame.cursor};
        };
        while (!frames.empty()) {
            auto& frame = frames.back(); step(1U, frame.cursor + 8U);
            const auto category = result_.types[frame.type].value_class;
            const auto kind = result_.types[frame.type].kind;
            const auto elements = std::max<std::uint32_t>(1U, result_.types[frame.type].array_count);
            if (category <= EffectValueCategory::column_matrix) {
                auto length = multiply(result_.types[frame.type].rows, result_.types[frame.type].columns, frame.start + 8U);
                length = multiply(multiply(length, elements, frame.start + 8U), 4U, frame.start + 8U);
                pool_bytes(frame.start, length); frame.cursor = frame.start + length; finish(); continue;
            }
            if (category == EffectValueCategory::structure) {
                if (frame.child) {
                    metadata_items<std::size_t>(1U, frame.cursor + 8U);
                    result_.values[frame.index].members.push_back(frame.child->index);
                    frame.cursor = frame.child->end; frame.child.reset(); ++frame.member;
                }
                if (frame.member == result_.types[frame.type].members.size()) { frame.member = 0U; ++frame.element; }
                if (frame.element >= elements) { finish(); continue; }
                if (result_.types[frame.type].members.empty()) continue;
                const auto child_type = result_.types[frame.type].members[frame.member];
                enter(child_type, frame.cursor); continue;
            }
            if (!sampler(kind)) {
                const auto length = multiply(elements, 4U, frame.start + 8U);
                pool_bytes(frame.start, length); metadata_items<std::uint32_t>(elements, frame.start + 8U);
                auto& slots = result_.values[frame.index].object_slots; slots.reserve(elements);
                for (std::uint32_t i = 0U; i < elements; ++i) {
                    const auto slot = pool_word(frame.cursor); object_use(slot, kind, frame.cursor + 8U);
                    slots.push_back(slot); frame.cursor += 4U;
                }
                finish(); continue;
            }
            if (!frame.prepared) {
                pool_bytes(frame.start, multiply(elements, 4U, frame.start + 8U));
                metadata_items<EffectSamplerElement>(elements, frame.start + 8U);
                result_.values[frame.index].sampler_elements.resize(elements); frame.prepared = true;
            }
            if (frame.element >= elements) { finish(); continue; }
            if (!frame.state_count) {
                const auto number = pool_word(frame.cursor); frame.cursor += 4U;
                range(pool_, frame.cursor, 0U, 8U);
                if (number > (pool_.size() - frame.cursor) / 16U)
                    stop(diagnostic_codes::bounds, "compiled-effect sampler state count exceeds pool", frame.cursor + 8U);
                frame.state_count = number; frame.state = 0U;
            }
            if (frame.child) {
                metadata(sizeof(EffectState) + sizeof(std::size_t), frame.cursor + 8U);
                const auto index = result_.states.size();
                result_.states.push_back({frame.pending_state[0], frame.pending_state[1], frame.child->index});
                result_.values[frame.index].sampler_elements[frame.element].states.push_back(index);
                frame.child.reset(); frame.cursor += 16U; ++frame.state;
            }
            if (frame.state == *frame.state_count) { frame.state_count.reset(); ++frame.element; continue; }
            for (std::size_t i = 0U; i < 4U; ++i) frame.pending_state[i] = pool_word(frame.cursor + i * 4U);
            const auto child_type = type(frame.pending_state[2]);
            enter(child_type, frame.pending_state[3]);
        }
        return root;
    }
    std::vector<std::size_t> annotations(std::size_t& cursor, const std::uint32_t number) {
        count(number, cursor, 8U); std::vector<std::size_t> indices;
        for (std::uint32_t i = 0U; i < number; ++i) {
            const auto type_offset = take(cursor), value_offset = take(cursor);
            const auto ti = type(type_offset); const auto vi = value(ti, value_offset);
            metadata_items<std::size_t>(1U, cursor); indices.push_back(vi);
        }
        return indices;
    }
    std::size_t state(std::size_t& cursor) {
        const auto id = take(cursor), selector = take(cursor), type_offset = take(cursor), value_offset = take(cursor);
        const auto ti = type(type_offset); const auto vi = value(ti, value_offset);
        metadata_items<EffectState>(1U, cursor); const auto index = result_.states.size();
        result_.states.push_back({id, selector, vi}); return index;
    }
    std::size_t target(const std::array<std::uint32_t, 4>& path, const std::size_t at) {
        constexpr auto absent = std::numeric_limits<std::uint32_t>::max();
        if (path[0] != absent) {
            if (path[0] >= result_.techniques.size()) stop(diagnostic_codes::index, "compiled-effect initializer technique is invalid", at);
            const auto& technique = result_.techniques[path[0]];
            if (path[1] >= technique.passes.size() || path[2] != absent)
                stop(diagnostic_codes::index, "compiled-effect initializer pass path is invalid", at);
            const auto& pass = technique.passes[path[1]];
            if (path[3] >= pass.states.size()) stop(diagnostic_codes::index, "compiled-effect initializer state is invalid", at);
            return result_.states[pass.states[path[3]]].value;
        }
        if (path[1] >= result_.parameters.size()) stop(diagnostic_codes::index, "compiled-effect initializer parameter is invalid", at);
        const auto& initial = result_.values[result_.parameters[path[1]].value];
        if (path[2] >= initial.sampler_elements.size()) stop(diagnostic_codes::index, "compiled-effect initializer sampler element is invalid", at);
        const auto& states = initial.sampler_elements[path[2]].states;
        if (path[3] >= states.size()) stop(diagnostic_codes::index, "compiled-effect initializer sampler state is invalid", at);
        return result_.states[states[path[3]]].value;
    }
    std::size_t name_hash(const std::string_view name, const std::size_t at) {
        step(name.size(), at); return std::hash<std::string_view>{}(name);
    }
    std::size_t parameter(const std::string_view name, const std::size_t at) {
        const auto found = names_.find(name_hash(name, at)); std::optional<std::size_t> match;
        if (found != names_.end()) {
            for (const auto index : found->second) {
                const auto& v = result_.values[result_.parameters[index].value];
                const auto other = result_.types[v.type].name;
                step(std::max(name.size(), other.size()) + 1U, at);
                if (name == other) {
                    if (match) stop(diagnostic_codes::structure, "compiled-effect parameter reference is ambiguous", at);
                    match = index;
                }
            }
        }
        if (!match) stop(diagnostic_codes::index, "compiled-effect named parameter reference is unresolved", at);
        return *match;
    }
    void initializers(std::size_t& cursor, const std::uint32_t number) {
        for (std::size_t i = 0U; i < result_.parameters.size(); ++i) {
            const auto& v = result_.values[result_.parameters[i].value];
            const auto hash = name_hash(result_.types[v.type].name, cursor);
            metadata(sizeof(std::size_t) + 64U, cursor); names_[hash].push_back(i);
        }
        count(number, cursor, 24U);
        for (std::uint32_t i = 0U; i < number; ++i) {
            const auto at = cursor; EffectInitializer item;
            for (auto& part : item.target) part = take(cursor);
            const auto kind = take(cursor), length = take(cursor);
            item.bytes = payload(cursor, length);
            if (kind > 1U) stop(diagnostic_codes::unsupported, "compiled-effect initializer kind is unsupported", at);
            item.kind = static_cast<EffectInitializerKind>(kind); item.value = target(item.target, at);
            auto& v = result_.values[item.value];
            if (v.initializer) stop(diagnostic_codes::structure, "compiled-effect value has duplicate initializers", at);
            if (item.kind == EffectInitializerKind::parameter_reference) {
                item.parameter = parameter(terminated(item.bytes, at + 24U), at);
                const auto& other = result_.values[result_.parameters[*item.parameter].value];
                if (v.object_slots.size() != 1U || other.object_slots.size() != 1U ||
                    !compatible(result_.types[v.type].kind, result_.types[other.type].kind))
                    stop(diagnostic_codes::unsupported, "compiled-effect named reference shape is unsupported", at);
            }
            const auto index = result_.initializers.size(); v.initializer = index;
            for (const auto slot : v.object_slots) {
                if (slot == 0U || result_.objects[slot].has_payload || result_.objects[slot].initializer)
                    stop(diagnostic_codes::structure, "compiled-effect object has conflicting initializers", at);
                result_.objects[slot].initializer = index;
            }
            metadata_items<EffectInitializer>(1U, at); result_.initializers.push_back(std::move(item));
        }
    }
    void validate_objects(const std::size_t at) {
        metadata_items<unsigned char>(result_.objects.size(), at);
        metadata_items<std::size_t>(result_.objects.size(), at);
        std::vector<unsigned char> colours(result_.objects.size());
        for (std::size_t start = 1U; start < result_.objects.size(); ++start) {
            if (!result_.objects[start].kind) stop(diagnostic_codes::unsupported, "compiled-effect object kind is unknown", at);
            if (!result_.objects[start].has_payload && !result_.objects[start].initializer)
                stop(diagnostic_codes::index, "compiled-effect object has no initializer or payload", at);
            if (colours[start] != 0U) continue;
            std::vector<std::size_t> chain; auto slot = start;
            while (slot != 0U && colours[slot] != 2U) {
                step(1U, at);
                if (colours[slot] == 1U) stop(diagnostic_codes::hierarchy_cycle, "compiled-effect named object reference cycle", at);
                colours[slot] = 1U; chain.push_back(slot);
                const auto initializer = result_.objects[slot].initializer;
                if (!initializer || !result_.initializers[*initializer].parameter) break;
                const auto p = *result_.initializers[*initializer].parameter;
                slot = result_.values[result_.parameters[p].value].object_slots.front();
            }
            for (const auto visited : chain) colours[visited] = 2U;
        }
    }
public:
    EffectReader(const std::span<const std::byte> bytes, Source source, const EffectReadLimits limits)
        : input_(bytes), metadata_left_(limits.metadata_bytes),
          work_left_(bytes.size() > std::numeric_limits<std::size_t>::max() / 32U
                         ? std::numeric_limits<std::size_t>::max() : bytes.size() * 32U) {
        result_.source = std::move(source);
    }
    EffectView read() {
        if (result_.source.stored_size != 0U && result_.source.stored_size != input_.size())
            stop(diagnostic_codes::source_mismatch, "compiled-effect provenance size differs from its byte span", 0U);
        if (input_.size() < 8U) stop(diagnostic_codes::truncated, "compiled-effect header is truncated", 0U);
        std::size_t cursor{};
        if (take(cursor) != 0xfeff0901U) stop(diagnostic_codes::unsupported, "compiled-effect container version is unsupported", 0U);
        const auto pool_size = take(cursor);
        if (pool_size % 4U != 0U) stop(diagnostic_codes::structure, "compiled-effect pool size is unaligned", 4U);
        range(input_, cursor, pool_size, 0U); pool_ = input_.subspan(cursor, pool_size); cursor += pool_size;
        if (pool_word(0U) != 0U) stop(diagnostic_codes::structure, "compiled-effect null sentinel is not zero", 8U);
        const auto parameters = take(cursor), techniques = take(cursor);
        result_.auxiliary_field = take(cursor); const auto objects = take(cursor);
        if (objects == 0U || objects > input_.size() / 4U)
            stop(diagnostic_codes::bounds, "compiled-effect object count cannot fit the input", cursor - 4U);
        metadata_items<EffectObject>(objects, cursor); result_.objects.resize(objects);
        count(parameters, cursor, 16U);
        for (std::uint32_t i = 0U; i < parameters; ++i) {
            const auto type_offset = take(cursor), value_offset = take(cursor), flags = take(cursor), number = take(cursor);
            EffectParameter item; item.flags = flags;
            const auto ti = type(type_offset); item.value = value(ti, value_offset);
            item.annotations = annotations(cursor, number); metadata_items<EffectParameter>(1U, cursor);
            result_.parameters.push_back(std::move(item));
        }
        count(techniques, cursor, 12U);
        for (std::uint32_t i = 0U; i < techniques; ++i) {
            EffectTechnique item; item.name = text(take(cursor)); const auto number = take(cursor), passes = take(cursor);
            item.annotations = annotations(cursor, number); count(passes, cursor, 12U);
            for (std::uint32_t j = 0U; j < passes; ++j) {
                EffectPass pass; pass.name = text(take(cursor)); const auto pass_annotations = take(cursor), states = take(cursor);
                pass.annotations = annotations(cursor, pass_annotations); count(states, cursor, 16U);
                for (std::uint32_t k = 0U; k < states; ++k) {
                    const auto index = state(cursor); metadata_items<std::size_t>(1U, cursor); pass.states.push_back(index);
                }
                metadata_items<EffectPass>(1U, cursor); item.passes.push_back(std::move(pass));
            }
            metadata_items<EffectTechnique>(1U, cursor); result_.techniques.push_back(std::move(item));
        }
        const auto blobs = take(cursor), references = take(cursor); count(blobs, cursor, 8U);
        for (std::uint32_t i = 0U; i < blobs; ++i) {
            const auto at = cursor; const auto slot = take(cursor), length = take(cursor);
            if (slot == 0U || slot >= result_.objects.size()) stop(diagnostic_codes::index, "compiled-effect payload object slot is invalid", at);
            auto& object = result_.objects[slot];
            if (object.has_payload) stop(diagnostic_codes::structure, "compiled-effect object has duplicate payloads", at);
            object.bytes = payload(cursor, length); object.has_payload = true;
        }
        initializers(cursor, references);
        if (cursor != input_.size()) stop(diagnostic_codes::structure, "compiled-effect trailing bytes are unsupported", cursor);
        validate_objects(cursor); return std::move(result_);
    }
};
} // namespace

core::Result<EffectView> read_compiled_effect(const std::span<const std::byte> bytes,
                                           Source source, const EffectReadLimits limits) {
    try {
        return core::Result<EffectView>::success(EffectReader(bytes, source, limits).read());
    } catch (const Stop& failure) {
        return core::Result<EffectView>::failure(detail::error(source, failure.code, failure.message,
                                                             static_cast<std::uint64_t>(failure.offset)));
    }
}
} // namespace eawr::assets
