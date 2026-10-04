#include "eawr/core/load_profile.hpp"
#include "renderer_upload_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace {
[[nodiscard]] Vector3 axis_convert(const assets::Vec3f& value) {
    return {value.x, value.z, -value.y};
}

[[nodiscard]] Image::Format image_format(const assets::PixelFormat format) {
    switch (format) {
    case assets::PixelFormat::rgba8: return Image::FORMAT_RGBA8;
    case assets::PixelFormat::bgra8: return Image::FORMAT_RGBA8;
    case assets::PixelFormat::bgr8: return Image::FORMAT_RGB8;
    case assets::PixelFormat::l8: return Image::FORMAT_L8;
    case assets::PixelFormat::a8: return Image::FORMAT_LA8;
    case assets::PixelFormat::bc1: return Image::FORMAT_DXT1;
    case assets::PixelFormat::bc2: return Image::FORMAT_DXT3;
    case assets::PixelFormat::bc3: return Image::FORMAT_DXT5;
    case assets::PixelFormat::bc4: return Image::FORMAT_RGTC_R;
    case assets::PixelFormat::bc5: return Image::FORMAT_RGTC_RG;
    case assets::PixelFormat::bc7: return Image::FORMAT_BPTC_RGBA;
    }
    return Image::FORMAT_MAX;
}
} // namespace

[[nodiscard]] core::Result<void> GodotRenderer::Impl::register_shared_texture(
    const std::string_view name, const assets::Texture& texture) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) {
        return failure(diagnostic_codes::backend_unavailable, "Godot RenderingServer is unavailable");
    }
    const std::string key(name);
    if (key.empty() || shared_textures_.contains(key)) {
        return failure(diagnostic_codes::upload_failed,
            "shared texture '" + key + "' is empty or already registered");
    }
    RID target;
    if (!upload_texture(*rendering, texture, target)) {
        return failure(diagnostic_codes::upload_failed,
            "Godot rejected shared texture '" + key + "'");
    }
    shared_textures_.emplace(key, target);
    return core::Result<void>::success();
}

[[nodiscard]] core::Result<void> GodotRenderer::Impl::register_shared_texture_array(
    const std::string_view name, const std::span<const assets::Texture> layers, const std::uint32_t edge) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) {
        return failure(diagnostic_codes::backend_unavailable, "Godot RenderingServer is unavailable");
    }
    const std::string key(name);
    if (key.empty() || shared_textures_.contains(key) || layers.empty() || edge == 0 || edge > 4096) {
        return failure(diagnostic_codes::upload_failed,
            "shared texture array '" + key + "' is empty, already registered or has an invalid edge");
    }
    TypedArray<Image> images;
    for (const assets::Texture& layer : layers) {
        const Image::Format format = image_format(layer.format);
        if (format == Image::FORMAT_MAX || layer.mips.empty()) {
            return failure(diagnostic_codes::upload_failed,
                "shared texture array '" + key + "' has a layer in an unsupported format");
        }
        // The top mip alone: the chain is regenerated after the resize.
        const assets::MipLevel& top = layer.mips.front();
        PackedByteArray bytes;
        bytes.resize(static_cast<int64_t>(top.bytes.size()));
        if (!top.bytes.empty()) std::memcpy(bytes.ptrw(), top.bytes.data(), top.bytes.size());
        Ref<Image> image = Image::create_from_data(static_cast<int32_t>(layer.width),
            static_cast<int32_t>(layer.height), false, format, bytes);
        if (image.is_null() || image->is_empty()) {
            return failure(diagnostic_codes::upload_failed,
                "Godot rejected a layer of shared texture array '" + key + "'");
        }
        if (image->is_compressed() && image->decompress() != OK) {
            return failure(diagnostic_codes::upload_failed,
                "a layer of shared texture array '" + key + "' could not be decompressed");
        }
        image->convert(Image::FORMAT_RGBA8);
        if (image->get_width() != static_cast<int32_t>(edge) || image->get_height() != static_cast<int32_t>(edge)) {
            image->resize(static_cast<int32_t>(edge), static_cast<int32_t>(edge), Image::INTERPOLATE_CUBIC);
        }
        image->generate_mipmaps();
        images.push_back(image);
    }
    const RID texture = rendering->texture_2d_layered_create(images, RenderingServer::TEXTURE_LAYERED_2D_ARRAY);
    if (!texture.is_valid()) {
        return failure(diagnostic_codes::upload_failed,
            "Godot rejected shared texture array '" + key + "'");
    }
    shared_textures_.emplace(key, texture);
    return core::Result<void>::success();
}

[[nodiscard]] bool GodotRenderer::Impl::upload_texture(
    RenderingServer& rendering, const assets::Texture& source, RID& target) {
    core::load_profile::Scope upload_scope(core::load_profile::Phase::texture_upload);
    const Image::Format format = image_format(source.format);
    if (format == Image::FORMAT_MAX || source.mips.empty()) return false;
    std::size_t byte_count{};
    for (const assets::MipLevel& mip : source.mips) byte_count += mip.bytes.size();
    PackedByteArray bytes;
    bytes.resize(static_cast<int64_t>(byte_count));
    std::size_t offset{};
    for (const assets::MipLevel& mip : source.mips) {
        if (!mip.bytes.empty()) {
            std::memcpy(bytes.ptrw() + offset, mip.bytes.data(), mip.bytes.size());
        }
        offset += mip.bytes.size();
    }
    const Ref<Image> image = Image::create_from_data(
        static_cast<int32_t>(source.width), static_cast<int32_t>(source.height),
        source.mips.size() > 1, format, bytes);
    if (image.is_null() || image->is_empty()) return false;
    target = rendering.texture_2d_create(image);
    return target.is_valid();
}


[[nodiscard]] bool GodotRenderer::Impl::upload_mesh(
    RenderingServer& rendering, const assets::Model& source, Resource& target, const bool authored_binormals) {
    core::load_profile::Scope scope(core::load_profile::Phase::mesh_upload);
    target.mesh = rendering.mesh_create();
    target.bone_count = source.bones.size();
    target.skin_used_bones.assign(source.bones.size(), 0);
    bool has_billboard = false;
    for (const assets::Bone& bone : source.bones) {
        const std::uint32_t mode = bone.billboard & 15U;
        has_billboard = has_billboard || mode == 1 || mode == 2 || mode == 3 || mode == 6 || mode == 7;
    }
    if (has_billboard) {
        billboard_pose_.prepare(source.bones.size());
        auto player = animation::Player::create(source);
        if (!player) return false;
        auto rest_pose = player.value().sample({});
        if (!rest_pose) return false;
        for (std::size_t index = 0; index < source.bones.size(); ++index) {
            target.billboard_modes.push_back(source.bones[index].billboard);
            target.bone_parents.push_back(source.bones[index].parent);
            target.bind_models.push_back(transform_from(animation::Player::asset_to_render_transform(
                rest_pose.value().bones[index].model_asset)));
            const auto& bind = source.bones[index].relative_transform;
            target.billboard_distances.push_back(std::hypot(bind[3], bind[7], bind[11]));
        }
    }
    // Rigid ALO vertices are stored in their bone's space and move to bind
    // model space here, so one animated * inverse(bind) palette places
    // both routes (Player::rigid_vertex_to_bind).
    std::optional<animation::Pose> rest;
    for (const assets::Mesh& mesh : source.meshes) {
        if (!mesh.visible) continue;
        for (const assets::Submesh& submesh : mesh.submeshes) {
            const bool palette_skinned = !submesh.skin_bones.empty();
            const bool rigid_skinned = !palette_skinned && mesh.bone >= 0;
            if ((palette_skinned || rigid_skinned) && source.bones.empty()) return false;
            if (rigid_skinned
                && static_cast<std::size_t>(mesh.bone) >= source.bones.size()) return false;
            if (rigid_skinned && !rest) {
                auto player = animation::Player::create(source);
                if (!player) return false;
                auto sampled = player.value().sample({});
                if (!sampled) return false;
                rest = std::move(sampled.value());
            }
            const animation::Matrix* rigid_bind = rigid_skinned
                ? &rest->bones[static_cast<std::size_t>(mesh.bone)].model_asset : nullptr;
            PackedVector3Array vertices;
            PackedVector3Array normals;
            PackedVector2Array uv;
            PackedColorArray colors;
            PackedFloat32Array tangents;
            PackedFloat32Array binormals;
            PackedInt32Array bones;
            PackedFloat32Array weights;
            PackedInt32Array indices;
            vertices.resize(static_cast<int64_t>(submesh.vertices.size()));
            normals.resize(static_cast<int64_t>(submesh.vertices.size()));
            uv.resize(static_cast<int64_t>(submesh.vertices.size()));
            colors.resize(static_cast<int64_t>(submesh.vertices.size()));
            tangents.resize(static_cast<int64_t>(submesh.vertices.size() * 4));
            if (authored_binormals) binormals.resize(static_cast<int64_t>(submesh.vertices.size() * 3));
            if (palette_skinned || rigid_skinned) {
                bones.resize(static_cast<int64_t>(submesh.vertices.size() * 4));
                weights.resize(static_cast<int64_t>(submesh.vertices.size() * 4));
                target.skinned = true;
            }
            for (std::size_t index = 0; index < submesh.vertices.size(); ++index) {
                const assets::Vertex vertex = rigid_bind
                    ? animation::Player::rigid_vertex_to_bind(*rigid_bind, submesh.vertices[index])
                    : submesh.vertices[index];
                const Vector3 normal = axis_convert(vertex.normal).normalized();
                const Vector3 tangent = axis_convert(vertex.tangent).normalized();
                const Vector3 binormal = axis_convert(vertex.binormal).normalized();
                vertices.set(static_cast<int64_t>(index), axis_convert(vertex.position));
                normals.set(static_cast<int64_t>(index), normal);
                uv.set(static_cast<int64_t>(index),
                    Vector2(vertex.texcoord[0].x, vertex.texcoord[0].y));
                // Authored vertex colour; only shaders that read COLOR see it.
                colors.set(static_cast<int64_t>(index),
                    Color(vertex.color.x, vertex.color.y, vertex.color.z, vertex.color.w));
                tangents.set(static_cast<int64_t>(index * 4), tangent.x);
                tangents.set(static_cast<int64_t>(index * 4 + 1), tangent.y);
                tangents.set(static_cast<int64_t>(index * 4 + 2), tangent.z);
                tangents.set(static_cast<int64_t>(index * 4 + 3),
                    normal.cross(tangent).dot(binormal) < 0.0 ? -1.0F : 1.0F);
                if (authored_binormals) {
                    const Vector3 authored = axis_convert(vertex.binormal);
                    const auto coefficients = legacy::binormal_coefficients(
                        {static_cast<float>(normal.x), static_cast<float>(normal.y), static_cast<float>(normal.z)},
                        {static_cast<float>(tangent.x), static_cast<float>(tangent.y), static_cast<float>(tangent.z)},
                        {static_cast<float>(authored.x), static_cast<float>(authored.y),
                            static_cast<float>(authored.z)});
                    for (std::size_t axis = 0; axis < 3; ++axis) {
                        binormals.set(static_cast<int64_t>(index * 3 + axis), coefficients[axis]);
                    }
                }
                if (rigid_skinned) {
                    if (!mark_skin_bone_usage(target.skin_used_bones, static_cast<std::uint32_t>(mesh.bone), 1.0F))
                        return false;
                    bones.set(static_cast<int64_t>(index * 4), mesh.bone);
                    weights.set(static_cast<int64_t>(index * 4), 1.0F);
                    for (std::size_t influence = 1; influence < 4; ++influence) {
                        bones.set(static_cast<int64_t>(index * 4 + influence), 0);
                        weights.set(static_cast<int64_t>(index * 4 + influence), 0.0F);
                    }
                } else if (palette_skinned) {
                    for (std::size_t influence = 0; influence < 4; ++influence) {
                        const std::uint32_t local = vertex.bone_indices[influence];
                        const float weight = vertex.bone_weights[influence];
                        if (!std::isfinite(weight) || weight < 0.0F
                            || (weight != 0.0F && local >= submesh.skin_bones.size())) return false;
                        const std::uint32_t global = weight == 0.0F
                            ? 0U : submesh.skin_bones[local];
                        if (global >= source.bones.size()) return false;
                        if (!mark_skin_bone_usage(target.skin_used_bones, global, weight)) return false;
                        bones.set(static_cast<int64_t>(index * 4 + influence),
                            static_cast<std::int32_t>(global));
                        weights.set(static_cast<int64_t>(index * 4 + influence), weight);
                    }
                }
            }
            indices.resize(static_cast<int64_t>(submesh.indices.size()));
            for (std::size_t index = 0; index < submesh.indices.size(); ++index) {
                indices.set(static_cast<int64_t>(index),
                    detail::uploaded_triangle_index(submesh.indices, index));
            }
            Array arrays;
            arrays.resize(RenderingServer::ARRAY_MAX);
            arrays[RenderingServer::ARRAY_VERTEX] = vertices;
            arrays[RenderingServer::ARRAY_NORMAL] = normals;
            arrays[RenderingServer::ARRAY_TANGENT] = tangents;
            arrays[RenderingServer::ARRAY_TEX_UV] = uv;
            arrays[RenderingServer::ARRAY_COLOR] = colors;
            if (authored_binormals) arrays[RenderingServer::ARRAY_CUSTOM0] = binormals;
            if (palette_skinned || rigid_skinned) {
                arrays[RenderingServer::ARRAY_BONES] = bones;
                arrays[RenderingServer::ARRAY_WEIGHTS] = weights;
            }
            arrays[RenderingServer::ARRAY_INDEX] = indices;
            rendering.mesh_add_surface_from_arrays(
                target.mesh, RenderingServer::PRIMITIVE_TRIANGLES, arrays, Array(), Dictionary(),
                authored_binormals ? static_cast<BitField<RenderingServer::ArrayFormat>>(
                    static_cast<int64_t>(RenderingServer::ARRAY_CUSTOM_RGB_FLOAT)
                    << RenderingServer::ARRAY_FORMAT_CUSTOM0_SHIFT) : BitField<RenderingServer::ArrayFormat>(0));
            rendering.mesh_surface_set_material(
                target.mesh, rendering.mesh_get_surface_count(target.mesh) - 1,
                target.material);
        }
    }
    return target.mesh.is_valid() && rendering.mesh_get_surface_count(target.mesh) > 0;
}


} // namespace eawr::presentation::godot_backend
