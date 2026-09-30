#include "asset_internal.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_set>

namespace eawr::assets {
namespace {
using detail::Chunk;
using detail::Mini;
using detail::Reader;

template <typename T>
core::Result<T> fail(const Source& source, std::string message, const std::uint64_t offset,
                     const std::string_view code = diagnostic_codes::structure) {
    return core::Result<T>::failure(detail::error(source, code, std::move(message), offset));
}

bool finished(const Reader& reader) { return reader.remaining() == 0; }

const Mini* mini(const std::vector<Mini>& minis, const std::uint8_t type) {
    const auto it = std::find_if(minis.begin(), minis.end(), [=](const Mini& item) { return item.type == type; });
    return it == minis.end() ? nullptr : &*it;
}

bool read_string_chunk(const Chunk& chunk, std::string& value) {
    Reader reader(chunk.payload, static_cast<std::size_t>(chunk.payload_offset));
    return reader.string(value);
}

core::Result<MaterialParameter> read_parameter(const Chunk& chunk, const Source& source) {
    auto parsed = detail::parse_minis(chunk, source);
    if (!parsed) return core::Result<MaterialParameter>::failure(parsed.error());
    if (parsed.value().size() != 2 || parsed.value()[0].type != 1 || parsed.value()[1].type != 2) {
        return fail<MaterialParameter>(source, "material parameter requires name/value mini-chunks", chunk.header_offset);
    }
    MaterialParameter result;
    Reader name(parsed.value()[0].payload, static_cast<std::size_t>(parsed.value()[0].offset + 2U));
    if (!name.string(result.name) || !finished(name)) return fail<MaterialParameter>(source, "invalid material parameter name", parsed.value()[0].offset);
    Reader value(parsed.value()[1].payload, static_cast<std::size_t>(parsed.value()[1].offset + 2U));
    switch (chunk.type) {
    case 0x10102: {
        std::int32_t v{}; if (!value.i32(v) || !finished(value)) return fail<MaterialParameter>(source, "invalid integer material parameter", chunk.header_offset);
        result.kind = ParameterKind::integer; result.value = v; break;
    }
    case 0x10103: {
        float v{}; if (!value.f32(v) || !finished(value)) return fail<MaterialParameter>(source, "invalid scalar material parameter", chunk.header_offset);
        result.kind = ParameterKind::scalar; result.value = v; break;
    }
    case 0x10104: {
        Vec3f v; if (!value.vec3(v) || !finished(value)) return fail<MaterialParameter>(source, "invalid vector3 material parameter", chunk.header_offset);
        result.kind = ParameterKind::vector3; result.value = v; break;
    }
    case 0x10105: {
        std::string v; if (!value.string(v) || !finished(value)) return fail<MaterialParameter>(source, "invalid texture material parameter", chunk.header_offset);
        result.kind = ParameterKind::texture; result.value = std::move(v); break;
    }
    case 0x10106: {
        Vec4f v; if (!value.vec4(v) || !finished(value)) return fail<MaterialParameter>(source, "invalid vector4 material parameter", chunk.header_offset);
        result.kind = ParameterKind::vector4; result.value = v; break;
    }
    default: return fail<MaterialParameter>(source, "unknown material parameter kind", chunk.header_offset, diagnostic_codes::unsupported);
    }
    return core::Result<MaterialParameter>::success(std::move(result));
}

core::Result<Submesh> read_submesh(const Chunk& chunk, const Chunk& geometry, const Source& source, const std::size_t bone_count) {
    if (!chunk.group || chunk.children.empty() || chunk.children.front().type != 0x10101) {
        return fail<Submesh>(source, "invalid submesh structure", chunk.header_offset);
    }
    Submesh result;
    if (!read_string_chunk(chunk.children.front(), result.shader)) return fail<Submesh>(source, "invalid shader name", chunk.children.front().header_offset);
    std::size_t pos = 1;
    while (pos < chunk.children.size() && chunk.children[pos].type >= 0x10102 && chunk.children[pos].type <= 0x10106) {
        auto param = read_parameter(chunk.children[pos], source);
        if (!param) return core::Result<Submesh>::failure(param.error());
        result.parameters.push_back(std::move(param.value()));
        ++pos;
    }
    if (pos != chunk.children.size() || geometry.type != 0x10000 || !geometry.group || geometry.children.size() < 4 ||
        geometry.children[0].type != 0x10001 || geometry.children[1].type != 0x10002 ||
        (geometry.children[2].type != 0x10007 && geometry.children[2].type != 0x10005) ||
        geometry.children[3].type != 0x10004) {
        return fail<Submesh>(source, "invalid vertex/index chunk sequence", geometry.header_offset);
    }
    Reader counts(geometry.children[0].payload, static_cast<std::size_t>(geometry.children[0].payload_offset));
    std::uint32_t vertex_count{}, primitive_count{};
    if (!counts.u32(vertex_count) || !counts.u32(primitive_count) ||
        vertex_count > detail::max_elements || primitive_count > detail::max_elements / 3U) {
        return fail<Submesh>(source, "invalid or excessive geometry counts", geometry.children[0].header_offset, diagnostic_codes::limit);
    }
    if (!read_string_chunk(geometry.children[1], result.vertex_format)) return fail<Submesh>(source, "invalid vertex format name", geometry.children[1].header_offset);
    const bool master = geometry.children[2].type == 0x10007;
    constexpr std::size_t master_stride = 144;
    constexpr std::size_t old_stride = 128;
    const std::size_t stride = master ? master_stride : old_stride;
    std::size_t vertex_bytes{};
    if (!detail::checked_multiply<std::size_t>(vertex_count, stride, vertex_bytes) || geometry.children[2].payload.size() != vertex_bytes) {
        return fail<Submesh>(source, "vertex payload size does not match declared count/format", geometry.children[2].header_offset, diagnostic_codes::bounds);
    }
    Reader vertices(geometry.children[2].payload, static_cast<std::size_t>(geometry.children[2].payload_offset));
    result.vertices.resize(vertex_count);
    for (auto& vertex : result.vertices) {
        if (!vertices.vec3(vertex.position) || !vertices.vec3(vertex.normal)) return fail<Submesh>(source, "truncated vertex", vertices.absolute(), diagnostic_codes::truncated);
        for (auto& uv : vertex.texcoord) if (!vertices.vec2(uv)) return fail<Submesh>(source, "truncated vertex texcoord", vertices.absolute(), diagnostic_codes::truncated);
        if (!vertices.vec3(vertex.tangent) || !vertices.vec3(vertex.binormal) || !vertices.vec4(vertex.color)) return fail<Submesh>(source, "truncated vertex attributes", vertices.absolute(), diagnostic_codes::truncated);
        if (master) { Vec4f unused; if (!vertices.vec4(unused)) return fail<Submesh>(source, "truncated master vertex", vertices.absolute(), diagnostic_codes::truncated); }
        for (auto& index : vertex.bone_indices) if (!vertices.u32(index)) return fail<Submesh>(source, "truncated bone indices", vertices.absolute(), diagnostic_codes::truncated);
        for (auto& weight : vertex.bone_weights) if (!vertices.f32(weight)) return fail<Submesh>(source, "truncated bone weights", vertices.absolute(), diagnostic_codes::truncated);
    }
    const std::size_t index_count = static_cast<std::size_t>(primitive_count) * 3U;
    if (geometry.children[3].payload.size() != index_count * 2U) return fail<Submesh>(source, "index payload size does not match triangle count", geometry.children[3].header_offset, diagnostic_codes::bounds);
    Reader indices(geometry.children[3].payload, static_cast<std::size_t>(geometry.children[3].payload_offset));
    result.indices.resize(index_count);
    for (auto& index : result.indices) {
        if (!indices.u16(index)) return fail<Submesh>(source, "truncated index payload", indices.absolute(), diagnostic_codes::truncated);
        if (index >= vertex_count) return fail<Submesh>(source, "triangle index exceeds vertex buffer", indices.absolute() - 2U, diagnostic_codes::index);
    }
    std::size_t extra = 4;
    if (extra < geometry.children.size() && geometry.children[extra].type == 0x10006) {
        Reader skin(geometry.children[extra].payload, static_cast<std::size_t>(geometry.children[extra].payload_offset));
        if (skin.remaining() % 4U != 0 || skin.remaining() / 4U > 24U) return fail<Submesh>(source, "skin palette is malformed or exceeds 24 bones", geometry.children[extra].header_offset, diagnostic_codes::limit);
        while (skin.remaining()) { std::uint32_t bone{}; skin.u32(bone); if (bone >= bone_count) return fail<Submesh>(source, "skin palette bone is out of range", skin.absolute() - 4U, diagnostic_codes::index); result.skin_bones.push_back(bone); }
        ++extra;
    }
    if (extra < geometry.children.size() && geometry.children[extra].type == 0x1200) ++extra;
    if (extra != geometry.children.size()) return fail<Submesh>(source, "unsupported essential geometry chunk", geometry.children[extra].header_offset, diagnostic_codes::unsupported);
    for (const auto& vertex : result.vertices) {
        for (std::size_t i = 0; i < 4; ++i) {
            if (!result.skin_bones.empty() && vertex.bone_weights[i] != 0.0F && vertex.bone_indices[i] >= result.skin_bones.size()) {
                return fail<Submesh>(source, "weighted vertex references outside skin palette", geometry.children[2].header_offset, diagnostic_codes::index);
            }
        }
    }
    return core::Result<Submesh>::success(std::move(result));
}

core::Result<Mesh> read_mesh(const Chunk& chunk, const Source& source, const std::size_t bone_count) {
    if (!chunk.group || chunk.children.size() < 2 || chunk.children[0].type != 0x401 || chunk.children[1].type != 0x402) return fail<Mesh>(source, "invalid mesh structure", chunk.header_offset);
    Mesh result;
    if (!read_string_chunk(chunk.children[0], result.name)) return fail<Mesh>(source, "invalid mesh name", chunk.children[0].header_offset);
    Reader info(chunk.children[1].payload, static_cast<std::size_t>(chunk.children[1].payload_offset));
    std::uint32_t submesh_count{}, ignored{}, visible{}, collidable{};
    if (!info.u32(submesh_count) || !info.vec3(result.bounds_min) || !info.vec3(result.bounds_max) ||
        !info.u32(ignored) || !info.u32(visible) || !info.u32(collidable) || submesh_count > detail::max_elements) {
        return fail<Mesh>(source, "invalid mesh metadata", chunk.children[1].header_offset);
    }
    result.visible = visible == 0; result.collidable = collidable != 0;
    if (chunk.children.size() != static_cast<std::size_t>(submesh_count) * 2U + 2U) return fail<Mesh>(source, "mesh submesh count does not match parameter/geometry chunk pairs", chunk.header_offset);
    for (std::size_t i = 0; i < submesh_count; ++i) {
        const auto parameter_index=i*2U+2U;const auto geometry_index=parameter_index+1U;
        if (chunk.children[parameter_index].type != 0x10100 || chunk.children[geometry_index].type != 0x10000) return fail<Mesh>(source, "unexpected chunk in submesh parameter/geometry sequence", chunk.children[parameter_index].header_offset);
        auto parsed = read_submesh(chunk.children[parameter_index], chunk.children[geometry_index], source, bone_count);
        if (!parsed) return core::Result<Mesh>::failure(parsed.error());
        result.submeshes.push_back(std::move(parsed.value()));
    }
    return core::Result<Mesh>::success(std::move(result));
}

core::Result<Light> read_light(const Chunk& chunk, const Source& source) {
    if (!chunk.group || chunk.children.size() != 2 || chunk.children[0].type != 0x1301 || chunk.children[1].type != 0x1302) return fail<Light>(source, "invalid light structure", chunk.header_offset);
    Light result;
    if (!read_string_chunk(chunk.children[0], result.name)) return fail<Light>(source, "invalid light name", chunk.children[0].header_offset);
    Reader data(chunk.children[1].payload, static_cast<std::size_t>(chunk.children[1].payload_offset));
    if (!data.u32(result.type) || !data.vec3(result.color) || !data.f32(result.intensity) ||
        !data.f32(result.far_attenuation_end) || !data.f32(result.far_attenuation_start) ||
        !data.f32(result.hotspot_size) || !data.f32(result.falloff_size) || result.type > 2) {
        return fail<Light>(source, "invalid light metadata", chunk.children[1].header_offset);
    }
    return core::Result<Light>::success(std::move(result));
}

struct ObjectRef { bool mesh{}; std::size_t index{}; };
core::Result<void> parse_connections(const Chunk& chunk, const Source& source, Model& model, const std::vector<ObjectRef>& objects) {
    if (!chunk.group || chunk.children.empty() || chunk.children[0].type != 0x601) return core::Result<void>::failure(detail::error(source, diagnostic_codes::structure, "invalid connections group", chunk.header_offset));
    auto header = detail::parse_minis(chunk.children[0], source);
    if (!header) return core::Result<void>::failure(header.error());
    const Mini* conns = mini(header.value(), 1); const Mini* proxies = mini(header.value(), 4); const Mini* dazzles = mini(header.value(), 9);
    std::uint32_t conn_count{}, proxy_count{}, dazzle_count{};
    auto read_count = [&](const Mini* item, std::uint32_t& out, bool required) {
        if (!item) return !required;
        Reader r(item->payload, static_cast<std::size_t>(item->offset + 2U)); return r.u32(out) && finished(r);
    };
    if (!read_count(conns, conn_count, true) || !read_count(proxies, proxy_count, true) || !read_count(dazzles, dazzle_count, false)) return core::Result<void>::failure(detail::error(source, diagnostic_codes::structure, "invalid connections counts", chunk.children[0].header_offset));
    if (conn_count > objects.size() || proxy_count > detail::max_elements || dazzle_count > detail::max_elements) return core::Result<void>::failure(detail::error(source, diagnostic_codes::limit, "connection count exceeds object/safety bounds", chunk.header_offset));
    std::size_t pos = 1;
    for (std::size_t i = 0; i < conn_count; ++i) {
        if (pos >= chunk.children.size() || chunk.children[pos].type != 0x602) return core::Result<void>::failure(detail::error(source, diagnostic_codes::structure, "missing object connection", chunk.header_offset));
        auto fields = detail::parse_minis(chunk.children[pos], source); if (!fields) return core::Result<void>::failure(fields.error());
        const Mini* object = mini(fields.value(), 2); const Mini* bone = mini(fields.value(), 3); std::uint32_t oi{}, bi{};
        if (!read_count(object, oi, true) || !read_count(bone, bi, true) || oi >= objects.size() || bi >= model.bones.size()) return core::Result<void>::failure(detail::error(source, diagnostic_codes::index, "connection object or bone index is out of range", chunk.children[pos].header_offset));
        const auto ref=objects[oi];if(ref.mesh)model.meshes[ref.index].bone=static_cast<std::int32_t>(bi);else model.lights[ref.index].bone=static_cast<std::int32_t>(bi);
        ++pos;
    }
    for (std::size_t i = 0; i < proxy_count; ++i) {
        if (pos >= chunk.children.size() || chunk.children[pos].type != 0x603) return core::Result<void>::failure(detail::error(source, diagnostic_codes::structure, "missing proxy chunk", chunk.header_offset));
        auto fields = detail::parse_minis(chunk.children[pos], source); if (!fields) return core::Result<void>::failure(fields.error());
        const Mini* name = mini(fields.value(), 5); const Mini* bone = mini(fields.value(), 6); std::uint32_t bi{};
        Proxy proxy; Reader nr(name ? name->payload : std::span<const std::byte>{});
        if (!name || !nr.string(proxy.name) || !finished(nr) || !read_count(bone, bi, true) || bi >= model.bones.size()) return core::Result<void>::failure(detail::error(source, diagnostic_codes::index, "invalid proxy name or bone", chunk.children[pos].header_offset));
        proxy.bone = bi; std::uint32_t flag{}; const Mini* visible = mini(fields.value(), 7); const Mini* alt = mini(fields.value(), 8);
        if (visible && (!read_count(visible, flag, true))) return core::Result<void>::failure(detail::error(source, diagnostic_codes::structure, "invalid proxy visibility", visible->offset));
        if (visible) proxy.visible = flag == 0;
        if (alt) { if (!read_count(alt, flag, true)) return core::Result<void>::failure(detail::error(source, diagnostic_codes::structure, "invalid proxy alternate flag", alt->offset)); proxy.alternate_decrease_stays_hidden = flag != 0; }
        model.proxies.push_back(std::move(proxy)); ++pos;
    }
    for (std::size_t i = 0; i < dazzle_count; ++i) {
        if (pos >= chunk.children.size() || chunk.children[pos].type != 0x604 || !chunk.children[pos].group || chunk.children[pos].children.size() != 1 || chunk.children[pos].children[0].type != 0) return core::Result<void>::failure(detail::error(source, diagnostic_codes::structure, "invalid dazzle group", chunk.header_offset));
        auto fields = detail::parse_minis(chunk.children[pos].children[0], source); if (!fields) return core::Result<void>::failure(fields.error());
        Dazzle d; auto f = [&](std::uint8_t t) { return mini(fields.value(), t); };
        auto read_u = [&](std::uint8_t t, std::uint32_t& v) { const auto* m=f(t); return read_count(m,v,true); };
        auto read_f = [&](std::uint8_t t, float& v) { const auto* m=f(t); if(!m)return false; Reader r(m->payload); return r.f32(v)&&finished(r); };
        auto read_v3 = [&](std::uint8_t t, Vec3f& v) { const auto* m=f(t); if(!m)return false; Reader r(m->payload); return r.vec3(v)&&finished(r); };
        auto read_s = [&](std::uint8_t t, std::string& v) { const auto* m=f(t); if(!m)return false; Reader r(m->payload); return r.string(v)&&finished(r); };
        std::uint32_t night{}, visible{};
        if (!read_v3(0,d.color)||!read_v3(1,d.position)||!read_f(2,d.radius)||!read_u(3,d.texture_x)||!read_u(4,d.texture_y)||!read_s(5,d.texture)||!read_u(6,d.texture_size)||!read_f(7,d.frequency)||!read_f(8,d.phase)||!read_u(9,night)||!read_u(10,d.bone)||!read_s(11,d.name)||!read_u(12,visible)||d.bone>=model.bones.size()) return core::Result<void>::failure(detail::error(source, diagnostic_codes::structure, "invalid dazzle fields", chunk.children[pos].header_offset));
        if (f(13) && !read_f(13,d.bias)) return core::Result<void>::failure(detail::error(source, diagnostic_codes::structure, "invalid dazzle bias", f(13)->offset));
        d.night_only=night!=0; d.visible=visible==0; model.dazzles.push_back(std::move(d)); ++pos;
    }
    if (pos != chunk.children.size()) return core::Result<void>::failure(detail::error(source, diagnostic_codes::unsupported, "unsupported essential connections chunk", chunk.children[pos].header_offset));
    return core::Result<void>::success();
}
} // namespace

Source source_from(const vfs::AssetRecord& record) {
    return Source{record.canonical_path, record.source_id, record.layer_id, record.origin, record.size};
}

core::Result<Model> load_model(const std::span<const std::byte> bytes, Source source) {
    if (bytes.size() > detail::max_file_size) return core::Result<Model>::failure(detail::error(source, diagnostic_codes::limit, "model exceeds 512 MiB safety limit"));
    if (source.stored_size != 0 && source.stored_size != bytes.size()) return core::Result<Model>::failure(detail::error(source, diagnostic_codes::source_mismatch, "provenance size does not match supplied model bytes"));
    auto roots = detail::parse_chunks(bytes, source); if (!roots) return core::Result<Model>::failure(roots.error());
    if (roots.value().size() == 1 && roots.value().front().type == 0x900) {
        return fail<Model>(source, "particle-system ALO root 0x900 is not representable by the Model API", 0, diagnostic_codes::unsupported);
    }
    if (roots.value().size() < 2 || roots.value().front().type != 0x200 || !roots.value().front().group || roots.value().back().type != 0x600) return fail<Model>(source, "ALO requires skeleton first and connections last", 0);
    Model model; model.source = std::move(source);
    const auto& skeleton = roots.value().front();
    if (skeleton.children.empty() || skeleton.children[0].type != 0x201) return fail<Model>(model.source, "skeleton is missing bone count", skeleton.header_offset);
    Reader count(skeleton.children[0].payload, static_cast<std::size_t>(skeleton.children[0].payload_offset)); std::uint32_t bone_count{};
    if (!count.u32(bone_count) || bone_count > detail::max_elements || skeleton.children.size() != static_cast<std::size_t>(bone_count)+1U) return fail<Model>(model.source, "bone count does not match skeleton chunks", skeleton.header_offset, diagnostic_codes::limit);
    model.bones.resize(bone_count);
    for (std::size_t i=0;i<bone_count;++i) {
        const auto& group=skeleton.children[i+1]; if(group.type!=0x202||!group.group||group.children.size()!=2||group.children[0].type!=0x203||(group.children[1].type!=0x205&&group.children[1].type!=0x206)) return fail<Model>(model.source,"invalid bone structure",group.header_offset);
        auto& bone=model.bones[i]; if(!read_string_chunk(group.children[0],bone.name)) return fail<Model>(model.source,"invalid bone name",group.children[0].header_offset);
        Reader data(group.children[1].payload,static_cast<std::size_t>(group.children[1].payload_offset)); std::uint32_t visible{};
        if(!data.i32(bone.parent)||!data.u32(visible)||(group.children[1].type==0x206&&!data.u32(bone.billboard))) return fail<Model>(model.source,"truncated bone metadata",group.children[1].header_offset,diagnostic_codes::truncated);
        bone.visible=visible!=0; for(auto& v:bone.relative_transform) if(!data.f32(v)) return fail<Model>(model.source,"truncated bone transform",data.absolute(),diagnostic_codes::truncated);
        if(bone.parent>=static_cast<std::int32_t>(bone_count)||bone.parent < -1) return fail<Model>(model.source,"invalid bone parent",group.children[1].header_offset,diagnostic_codes::index);
    }
    std::vector<std::uint8_t> state(bone_count); std::function<bool(std::size_t)> visit=[&](std::size_t i){ if(state[i]==1)return false;if(state[i]==2)return true;state[i]=1;const auto p=model.bones[i].parent;if(p>=0&&!visit(static_cast<std::size_t>(p)))return false;state[i]=2;return true;};
    for(std::size_t i=0;i<bone_count;++i) if(!visit(i)) return fail<Model>(model.source,"bone hierarchy contains a cycle",skeleton.header_offset,diagnostic_codes::hierarchy_cycle);
    std::vector<ObjectRef> objects;for(std::size_t i=1;i+1<roots.value().size();++i){const auto& node=roots.value()[i];if(node.type==0x400){auto mesh=read_mesh(node,model.source,bone_count);if(!mesh)return core::Result<Model>::failure(mesh.error());objects.push_back({true,model.meshes.size()});model.meshes.push_back(std::move(mesh.value()));}else if(node.type==0x1300){auto light=read_light(node,model.source);if(!light)return core::Result<Model>::failure(light.error());objects.push_back({false,model.lights.size()});model.lights.push_back(std::move(light.value()));}else return fail<Model>(model.source,"unsupported essential top-level ALO chunk",node.header_offset,diagnostic_codes::unsupported);}
    auto connections=parse_connections(roots.value().back(),model.source,model,objects);if(!connections)return core::Result<Model>::failure(connections.error());
    return core::Result<Model>::success(std::move(model));
}

core::Result<Model> load_model(const vfs::Vfs& filesystem, const std::string_view path) {
    auto record=filesystem.stat(path);if(!record)return core::Result<Model>::failure(record.error());auto bytes=filesystem.open(path);if(!bytes)return core::Result<Model>::failure(bytes.error());return load_model(bytes.value(),source_from(record.value()));
}
} // namespace eawr::assets
