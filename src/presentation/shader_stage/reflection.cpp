#include "internal.hpp"
#include "reflection_association.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace eawr::presentation::shader_runtime::detail {
namespace {
constexpr auto phase = DiagnosticPhase::reflection;
void api_ok(bool ok) {
    require(ok, phase, DiagnosticReason::reflection_failure,
            "public reflection API failed");
}
Builtin builtin(std::uint32_t id) {
    BuiltinKind kind{BuiltinKind::unsupported};
    switch (id) {
    case 0:
        kind = BuiltinKind::position;
        break;
    case 1:
        kind = BuiltinKind::point_size;
        break;
    case 3:
        kind = BuiltinKind::clip_distance;
        break;
    case 4:
        kind = BuiltinKind::cull_distance;
        break;
    case 15:
        kind = BuiltinKind::fragment_coordinate;
        break;
    case 17:
        kind = BuiltinKind::front_facing;
        break;
    case 18:
        kind = BuiltinKind::sample_id;
        break;
    case 19:
        kind = BuiltinKind::sample_position;
        break;
    case 20:
        kind = BuiltinKind::sample_mask;
        break;
    case 22:
        kind = BuiltinKind::fragment_depth;
        break;
    case 42:
        kind = BuiltinKind::vertex_index;
        break;
    case 43:
        kind = BuiltinKind::instance_index;
        break;
    default:
        break;
    }
    return {kind, id};
}
struct Reflector {
    Session &s;
    abi::H compiler;
    StageReflection out;
    std::map<abi::U, TypeIndex> types;
    std::set<abi::U> active_types;
    std::size_t metadata{};
    void account(std::size_t bytes) {
        require(bytes <= s.limits.metadata_bytes -
                             std::min(metadata, s.limits.metadata_bytes),
                phase, DiagnosticReason::budget_exceeded,
                "reflection metadata budget exceeded");
        metadata += bytes;
        checkpoint(s.limits, phase);
    }
    std::string text(const char *value) {
        auto limits = s.limits;
        limits.string_bytes = std::min(
            limits.string_bytes,
            limits.metadata_bytes - std::min(metadata, limits.metadata_bytes));
        auto result = bounded_text(value, limits, phase);
        account(result.size());
        return result;
    }
    std::optional<abi::U> decoration(abi::U id, int kind) {
        if (!s.api.spvc_compiler_has_decoration(compiler, id, kind))
            return std::nullopt;
        return s.api.spvc_compiler_get_decoration(compiler, id, kind);
    }
    std::optional<abi::U> member_decoration(abi::U id, abi::U member,
                                            int kind) {
        if (!s.api.spvc_compiler_has_member_decoration(compiler, id, member,
                                                       kind))
            return std::nullopt;
        return s.api.spvc_compiler_get_member_decoration(compiler, id, member,
                                                         kind);
    }
    TypeIndex type(abi::U id, std::size_t depth = 0) {
        require(depth < s.limits.nesting, phase,
                DiagnosticReason::budget_exceeded,
                "reflected type nesting exceeded");
        require(!active_types.contains(id), phase,
                DiagnosticReason::reflection_failure,
                "recursive reflected type");
        if (const auto found = types.find(id); found != types.end())
            return found->second;
        require(out.types.size() < s.limits.table_entries, phase,
                DiagnosticReason::budget_exceeded,
                "reflection type count exceeded");
        const auto handle = s.api.spvc_compiler_get_type_handle(compiler, id);
        api_ok(handle != nullptr);
        const auto index = static_cast<TypeIndex>(out.types.size());
        types.emplace(id, index);
        active_types.insert(id);
        out.types.emplace_back();
        account(sizeof(ReflectedType));
        ReflectedType value;
        const auto base = s.api.spvc_type_get_basetype(handle);
        value.bit_width = s.api.spvc_type_get_bit_width(handle);
        value.vector_width = s.api.spvc_type_get_vector_size(handle);
        value.columns = s.api.spvc_type_get_columns(handle);
        if (base == 2)
            value.scalar = ScalarKind::boolean;
        else if (base == 3 || base == 5 || base == 7 || base == 9)
            value.scalar = ScalarKind::signed_integer;
        else if (base == 4 || base == 6 || base == 8 || base == 10)
            value.scalar = ScalarKind::unsigned_integer;
        else if (base >= 12 && base <= 14)
            value.scalar = ScalarKind::floating;
        else if (base == 15)
            value.kind = TypeKind::structure;
        else if (base == 16)
            value.kind = TypeKind::image;
        else if (base == 17)
            value.kind = TypeKind::sampled_image;
        else if (base == 18)
            value.kind = TypeKind::sampler;
        else
            refuse(phase, DiagnosticReason::unsupported_resource,
                   "unsupported reflected type kind");
        const auto dimensions =
            s.api.spvc_type_get_num_array_dimensions(handle);
        require(dimensions <= s.limits.nesting, phase,
                DiagnosticReason::budget_exceeded, "array rank exceeded");
        for (abi::U d = 0; d < dimensions; ++d) {
            const auto count = s.api.spvc_type_get_array_dimension(handle, d);
            ArrayDimension dimension;
            if (s.api.spvc_type_array_dimension_is_literal(handle, d))
                dimension.literal_count = count;
            else {
                require(bool(decoration(count, 1)), phase,
                        DiagnosticReason::unsupported_resource,
                        "unidentified specialization array extent");
                dimension.specialization_id = decoration(count, 1);
            }
            value.array_dimensions.push_back(dimension);
            account(sizeof(ArrayDimension));
        }
        value.array_stride = decoration(id, 6);
        if (base == 15) {
            const auto base_id = s.api.spvc_type_get_base_type_id(handle);
            std::size_t bytes{};
            if (s.api.spvc_compiler_get_declared_struct_size(compiler, handle,
                                                             &bytes) == 0)
                value.declared_byte_size = bytes;
            const auto count = s.api.spvc_type_get_num_member_types(handle);
            require(count <= s.limits.table_entries, phase,
                    DiagnosticReason::budget_exceeded, "member count exceeded");
            for (abi::U m = 0; m < count; ++m) {
                TypeMember member;
                member.name = text(
                    s.api.spvc_compiler_get_member_name(compiler, base_id, m));
                member.type =
                    type(s.api.spvc_type_get_member_type(handle, m), depth + 1);
                abi::U offset{}, stride{};
                if (member_decoration(base_id, m, 35)) {
                    api_ok(s.api.spvc_compiler_type_struct_member_offset(
                               compiler, handle, m, &offset) == 0);
                    member.byte_offset = offset;
                    if (s.api.spvc_compiler_get_declared_struct_member_size(
                            compiler, handle, m, &bytes) == 0)
                        member.byte_size = bytes;
                }
                if (!out.types[member.type].array_dimensions.empty() &&
                    member.byte_offset) {
                    api_ok(s.api.spvc_compiler_type_struct_member_array_stride(
                               compiler, handle, m, &stride) == 0);
                    if (out.types[member.type].array_stride)
                        require(*out.types[member.type].array_stride == stride,
                                phase, DiagnosticReason::reflection_failure,
                                "conflicting array stride");
                    out.types[member.type].array_stride = stride;
                }
                if (member_decoration(base_id, m, 7)) {
                    api_ok(s.api.spvc_compiler_type_struct_member_matrix_stride(
                               compiler, handle, m, &stride) == 0);
                    member.matrix_stride = stride;
                }
                member.row_major = bool(member_decoration(base_id, m, 4));
                value.members.push_back(std::move(member));
                account(sizeof(TypeMember));
            }
        }
        if (base == 16 || base == 17) {
            const auto dimension = s.api.spvc_type_get_image_dimension(handle);
            require(dimension >= 0 && dimension <= 6, phase,
                    DiagnosticReason::unsupported_resource,
                    "unsupported image dimension");
            value.image = ImageType{
                static_cast<ImageDimension>(dimension),
                type(s.api.spvc_type_get_image_sampled_type(handle), depth + 1),
                s.api.spvc_type_get_image_arrayed(handle) != 0,
                s.api.spvc_type_get_image_multisampled(handle) != 0,
                s.api.spvc_type_get_image_is_depth(handle) != 0,
                s.api.spvc_type_get_image_is_storage(handle) != 0,
                std::nullopt};
            const auto format =
                s.api.spvc_type_get_image_storage_format(handle);
            if (format)
                value.image->storage_format =
                    static_cast<std::uint32_t>(format);
        }
        out.types[index] = std::move(value);
        active_types.erase(id);
        return index;
    }
    void add_interface(const abi::Resource &resource,
                       InterfaceDirection direction,
                       std::optional<Builtin> built, abi::U raw_type,
                       std::vector<std::uint32_t> path = {},
                       std::optional<TypeIndex> aggregate = std::nullopt,
                       std::optional<abi::U> member_parent = std::nullopt,
                       abi::U member_index = 0) {
        require(path.size() < s.limits.nesting &&
                    out.interfaces.size() < s.limits.table_entries,
                phase, DiagnosticReason::budget_exceeded,
                "interface budget exceeded");
        const auto handle =
            s.api.spvc_compiler_get_type_handle(compiler, raw_type);
        api_ok(handle != nullptr);
        const auto t = type(raw_type);
        if (out.types[t].kind == TypeKind::structure) {
            require(out.types[t].array_dimensions.empty(), phase,
                    DiagnosticReason::unsupported_interface,
                    "array of interface blocks unsupported");
            const auto base = s.api.spvc_type_get_base_type_id(handle);
            const auto count = s.api.spvc_type_get_num_member_types(handle);
            require(count <= s.limits.table_entries, phase,
                    DiagnosticReason::budget_exceeded,
                    "interface member count exceeded");
            for (abi::U m = 0; m < count; ++m) {
                const auto member_builtin = member_decoration(base, m, 11);
                if (built && (!member_builtin ||
                              *member_builtin != built->spirv_identity))
                    continue;
                auto member_path = path;
                member_path.push_back(m);
                add_interface(
                    resource, direction,
                    member_builtin ? std::optional{builtin(*member_builtin)}
                                   : std::nullopt,
                    s.api.spvc_type_get_member_type(handle, m),
                    std::move(member_path), aggregate.value_or(t), base, m);
            }
            return;
        }
        ReflectedInterface value;
        value.direction = direction;
        value.type = t;
        value.aggregate_type = aggregate;
        value.member_path = std::move(path);
        value.builtin = built;
        value.source_name = text(resource.name);
        value.emitted_name =
            text(s.api.spvc_compiler_get_name(compiler, resource.id));
        const auto deco = [&](int d) {
            return member_parent
                       ? member_decoration(*member_parent, member_index, d)
                       : decoration(resource.id, d);
        };
        value.location = deco(30);
        value.component = deco(31);
        value.index = deco(32);
        require(built || value.location.has_value(), phase,
                DiagnosticReason::unsupported_interface,
                "interface location unavailable");
        value.interpolation = deco(14)
                                  ? Interpolation::flat
                                  : (deco(13) ? Interpolation::no_perspective
                                              : Interpolation::smooth);
        value.centroid = bool(deco(16));
        value.sample = bool(deco(17));
        const auto width = out.types[t].vector_width;
        const auto component = value.component.value_or(0);
        if (width > 0 && width <= 4 && component <= 4 - width)
            value.declared_component_mask = ((1U << width) - 1U) << component;
        // The qualified whole-register source association is applied after
        // reflection. Packed and unsupported leaves remain unassociated.
        out.interfaces.push_back(std::move(value));
        account(sizeof(ReflectedInterface));
    }
};
} // namespace
StageReflection reflect(Session &s, abi::H compiler,
                        const StageOptions &options, const Scan &scanned) {
    checkpoint(s.limits, phase);
    Reflector r{s, compiler, {}, {}, {}, 0};
    abi::H resources{};
    api_ok(s.api.spvc_compiler_create_shader_resources(compiler, &resources) ==
               0 &&
           resources);
    std::map<abi::U, ResourceIndex> ids;
    std::map<std::pair<abi::U, abi::U>, SourceRegister> binding_sources;
    std::map<std::pair<abi::U, abi::U>, const CombinedBindingAssignment *>
        combined_targets;
    for (const auto &b : options.bindings)
        binding_sources.emplace(
            std::pair{b.target.descriptor_set, b.target.binding}, b.source);
    for (const auto &b : options.combined_bindings)
        combined_targets.emplace(
            std::pair{b.target.descriptor_set, b.target.binding}, &b);
    for (int kind = 1; kind <= 14; ++kind) {
        // The pinned C API rejects RAY_QUERY even when absent. The full module
        // was checked for OpTypeRayQueryKHR before entering Cross, so skipping
        // this unsupported query cannot hide an unreflected ray-query type.
        if (kind == 13)
            continue;
        const abi::Resource *list{};
        std::size_t count{};
        api_ok(s.api.spvc_resources_get_resource_list_for_type(
                   resources, kind, &list, &count) == 0);
        require(count <= s.limits.table_entries && (!count || list), phase,
                DiagnosticReason::budget_exceeded, "resource count exceeded");
        for (std::size_t i = 0; i < count; ++i) {
            if (kind == 3 || kind == 4) {
                r.add_interface(list[i],
                                kind == 3 ? InterfaceDirection::input
                                          : InterfaceDirection::output,
                                std::nullopt, list[i].type);
                continue;
            }
            const ResourceKind kinds[]{
                ResourceKind::uniform_buffer,   ResourceKind::storage_buffer,
                ResourceKind::uniform_buffer,   ResourceKind::uniform_buffer,
                ResourceKind::subpass_input,    ResourceKind::storage_image,
                ResourceKind::combined_sampler, ResourceKind::atomic_counter,
                ResourceKind::push_constant,    ResourceKind::separate_image,
                ResourceKind::separate_sampler};
            require(kind <= 11, phase, DiagnosticReason::unsupported_resource,
                    "unsupported reflected resource category");
            require(r.out.resources.size() < s.limits.table_entries, phase,
                    DiagnosticReason::budget_exceeded,
                    "resource table exceeded");
            ReflectedResource value;
            value.kind = kinds[kind - 1];
            value.type = r.type(list[i].type);
            value.source_name = r.text(list[i].name);
            value.emitted_name =
                r.text(s.api.spvc_compiler_get_name(compiler, list[i].id));
            const auto binding = r.decoration(list[i].id, 33),
                       set = r.decoration(list[i].id, 34);
            if (binding && set) {
                value.binding = BindingPoint{*set, *binding};
                if (value.kind == ResourceKind::combined_sampler) {
                    require(
                        combined_targets.contains({*set, *binding}), phase,
                        DiagnosticReason::reflection_failure,
                        "reflected combined target absent from binding plan");
                } else {
                    const auto found = binding_sources.find({*set, *binding});
                    require(found != binding_sources.end(), phase,
                            DiagnosticReason::reflection_failure,
                            "reflected resource absent from binding plan");
                    value.source_register = found->second;
                }
            }
            value.readonly = bool(r.decoration(list[i].id, 24));
            value.writeonly = bool(r.decoration(list[i].id, 25));
            ids.emplace(list[i].id,
                        static_cast<ResourceIndex>(r.out.resources.size()));
            r.out.resources.push_back(std::move(value));
            r.account(sizeof(ReflectedResource));
        }
    }
    for (int direction = 1; direction <= 2; ++direction) {
        const abi::BuiltinResource *list{};
        std::size_t count{};
        api_ok(s.api.spvc_resources_get_builtin_resource_list_for_type(
                   resources, direction, &list, &count) == 0);
        require(count <= s.limits.table_entries && (!count || list), phase,
                DiagnosticReason::budget_exceeded, "builtin count exceeded");
        for (std::size_t i = 0; i < count; ++i)
            r.add_interface(
                list[i].resource,
                direction == 1 ? InterfaceDirection::input
                               : InterfaceDirection::output,
                builtin(static_cast<std::uint32_t>(list[i].builtin)),
                list[i].resource.type);
    }
    const abi::Combined *combined{};
    std::size_t count{};
    api_ok(s.api.spvc_compiler_get_combined_image_samplers(compiler, &combined,
                                                           &count) == 0);
    require(count <= s.limits.table_entries && (!count || combined), phase,
            DiagnosticReason::budget_exceeded,
            "combined reflection count exceeded");
    for (std::size_t i = 0; i < count; ++i) {
        const auto &c = combined[i];
        require(ids.contains(c.image) && ids.contains(c.sampler), phase,
                DiagnosticReason::reflection_failure,
                "combined source resource missing");
        ResourceIndex index{};
        if (ids.contains(c.combined))
            index = ids.at(c.combined);
        else {
            ReflectedResource resource;
            resource.kind = ResourceKind::combined_sampler;
            const auto &original_type =
                r.out.types[r.out.resources[ids.at(c.image)].type];
            r.account(sizeof(ReflectedType) +
                      original_type.array_dimensions.size() *
                          sizeof(ArrayDimension));
            auto combined_type = original_type;
            combined_type.kind = TypeKind::sampled_image;
            require(r.out.types.size() < s.limits.table_entries, phase,
                    DiagnosticReason::budget_exceeded,
                    "combined type count exceeded");
            resource.type = static_cast<TypeIndex>(r.out.types.size());
            r.out.types.push_back(std::move(combined_type));
            resource.emitted_name =
                r.text(s.api.spvc_compiler_get_name(compiler, c.combined));
            const auto b = r.decoration(c.combined, 33),
                       set = r.decoration(c.combined, 34);
            require(b && set, phase, DiagnosticReason::reflection_failure,
                    "combined target binding missing");
            resource.binding = BindingPoint{*set, *b};
            require(combined_targets.contains({*set, *b}), phase,
                    DiagnosticReason::reflection_failure,
                    "synthesized combined target absent from binding plan");
            index = static_cast<ResourceIndex>(r.out.resources.size());
            r.out.resources.push_back(std::move(resource));
            r.account(sizeof(ReflectedResource));
        }
        const auto &target = r.out.resources[index].binding;
        require(target.has_value(), phase, DiagnosticReason::reflection_failure,
                "combined target binding unavailable");
        const auto plan =
            combined_targets.find({target->descriptor_set, target->binding});
        const auto &image_source =
            r.out.resources[ids.at(c.image)].source_register;
        const auto &sampler_source =
            r.out.resources[ids.at(c.sampler)].source_register;
        const auto same_source = [](const SourceRegister &a,
                                    const SourceRegister &b) {
            return a.kind == b.kind && a.space == b.space && a.index == b.index;
        };
        require(plan != combined_targets.end() && image_source &&
                    sampler_source &&
                    same_source(*image_source, plan->second->image) &&
                    same_source(*sampler_source, plan->second->sampler),
                phase, DiagnosticReason::reflection_failure,
                "combined reflection disagrees with binding plan");
        r.out.combined_samplers.push_back(
            {index, ids.at(c.image), ids.at(c.sampler)});
        r.account(sizeof(CombinedSampler));
    }
    associate_interfaces(r.out, scanned);
    for (const auto& leaf : r.out.interfaces)
        if (leaf.source.semantic) r.account(leaf.source.semantic->name.size());
    checkpoint(s.limits, phase);
    return std::move(r.out);
}
} // namespace eawr::presentation::shader_runtime::detail
