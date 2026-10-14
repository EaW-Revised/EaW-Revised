#include "render_internal.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <chrono>
#include <limits>
#include <utility>

// Quad construction follows the MIT-licensed alo-viewer revision
// 9bb0053919cc5df8377610d4f91b11d956d6c2f4 (DirectX9/ParticleRenderers.cpp):
// corner order, the 0-1-2 / 2-1-3 index pattern, the texture-coordinate
// assignment. Legacy kite geometry follows MD-07. Blend, depth-write and phase policy
// are taken from the render state of the public Engine/Prim* effects that the
// legacy selector names; see docs/reports/P1-08-rendering.md.

namespace eawr::presentation::particles {
using namespace render_detail;

namespace {
constexpr float pi = 3.14159265358979323846F;
// Corners in the quad's local plane, before rotation: the alo-viewer order
// (-s,+s), (-s,-s), (+s,+s), (+s,-s) with indices 0-1-2 and 2-1-3.
void append_quad(VertexStream& stream, const Particle& particle, const Vec3 axis_x,
                 const Vec3 axis_y, const std::array<std::array<float, 2>, 4>& corners,
                 const float angle, const bool triangle = false) {
    if (!std::isfinite(angle) || !finite(axis_x) || !finite(axis_y) ||
        !finite(particle.color) || !finite(particle.texcoords) ||
        stream.vertices.size() > std::numeric_limits<std::uint32_t>::max() - 3U) return;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const Vec4 uv = particle.texcoords;
    std::array<std::array<float, 2>, 4> texcoords{{
        {uv.x, uv.y}, {uv.x, uv.y + uv.w}, {uv.x + uv.z, uv.y}, {uv.x + uv.z, uv.y + uv.w},
    }};
    auto positions = corners;
    const std::size_t count = triangle ? 3U : 4U;
    if (triangle) {
        // PS-31: native triangle corners and UVs (debug build PSE-CAP-01/02).
        const float size = particle.size;
        positions = {{{0, -size}, {-size, size}, {size, size}, {0, 0}}};
        texcoords = {{{uv.x + uv.z * 0.5F, uv.y}, {uv.x + uv.z, uv.y + uv.w},
            {uv.x, uv.y + uv.w}, {0, 0}}};
    }
    std::array<ParticleVertex, 4> vertices;
    Vec3 bounds_min = stream.bounds_min;
    Vec3 bounds_max = stream.bounds_max;
    for (std::size_t corner = 0; corner < count; ++corner) {
        const float x = positions[corner][0] * c - positions[corner][1] * s;
        const float y = positions[corner][0] * s + positions[corner][1] * c;
        const Vec3 position = particle.position + axis_x * x + axis_y * y;
        if (!finite(position) || !std::isfinite(texcoords[corner][0]) ||
            !std::isfinite(texcoords[corner][1])) return;
        if (stream.vertices.empty() && corner == 0) {
            bounds_min = bounds_max = position;
        } else {
            bounds_min = {std::min(bounds_min.x, position.x),
                          std::min(bounds_min.y, position.y),
                          std::min(bounds_min.z, position.z)};
            bounds_max = {std::max(bounds_max.x, position.x),
                          std::max(bounds_max.y, position.y),
                          std::max(bounds_max.z, position.z)};
        }
        vertices[corner] = {position, particle.color, texcoords[corner][0], texcoords[corner][1]};
    }
    if (!finite(bounds_min) || !finite(bounds_max)) return;
    const auto base = static_cast<std::uint32_t>(stream.vertices.size());
    stream.vertices.insert(stream.vertices.end(), vertices.begin(), vertices.begin() + static_cast<std::ptrdiff_t>(count));
    if (triangle) {
        for (const std::uint32_t offset : {0U, 1U, 2U}) stream.indices.push_back(base + offset);
        ++stream.triangles;
    } else {
        for (const std::uint32_t offset : {0U, 1U, 2U, 2U, 1U, 3U}) stream.indices.push_back(base + offset);
        ++stream.quads;
    }
    stream.bounds_min = bounds_min;
    stream.bounds_max = bounds_max;
}

void mix_hash(std::uint64_t& hash, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= (value >> shift) & 0xffU;
        hash *= 0x100000001b3ULL;
    }
}
void mix_hash(std::uint64_t& hash, const float value) { mix_hash(hash, std::bit_cast<std::uint32_t>(value)); }

} // namespace

CameraFrame camera_frame_from_render(const std::array<float, 3>& eye,
                                     const std::array<float, 3>& target,
                                     const std::array<float, 3>& up) {
    const Vec3 position = render_to_particle(eye[0], eye[1], eye[2]);
    const Vec3 look = render_to_particle(target[0], target[1], target[2]);
    const Vec3 up_hint = render_to_particle(up[0], up[1], up[2]);
    const Vec3 forward = normalized(look - position);
    // The asset-to-render conversion is a proper rotation, so right-handed
    // look-at construction carries over unchanged.
    Vec3 right = normalized(cross(forward, up_hint));
    if (length(right) == 0.0F) right = {1.0F, 0.0F, 0.0F};
    const Vec3 camera_up = normalized(cross(right, forward));
    return {position, right, camera_up};
}

EmitterFrame emitter_frame_from_render(const std::array<float, 16>& m) {
    // C^-1 * M * C: the asset axes map through C to render (1,0,0), (0,0,-1)
    // and (0,1,0) respectively.
    const auto column = [&](const std::size_t index) {
        return render_to_particle(m[index * 4], m[index * 4 + 1], m[index * 4 + 2]);
    };
    EmitterFrame frame;
    frame.origin = column(3);
    frame.basis.x = column(0);
    frame.basis.y = column(2) * -1.0F;
    frame.basis.z = column(1);
    return frame;
}

void VertexStream::clear() {
    axis_samples = axis_reversed = 0;
    axis_max_sine_error = 0;
    vertices.clear();
    indices.clear();
    quads = 0;
    triangles = 0;
    sorted_particles.clear();
    sort_candidates = 0;
    bounds_min = bounds_max = {};
}

ParticleBounds particle_bounds(const std::span<const EmitterRenderPlan> plans,
    const std::span<const Particle> particles) {
    ParticleBounds bounds;
    for (const Particle& particle : particles) {
        if (particle.emitter_index >= plans.size()) continue;
        const auto& plan = plans[particle.emitter_index];
        if (!plan.drawable) continue;
        // PS-39: include each world-space particle, not only its current host.
        // Rotation-independent enclosing radius also includes every kite corner.
        float radius = std::abs(particle.size) * 1.414214F;
        if (plan.family == RenderFamily::kites) {
            radius = plan.legacy_kite_motion
                ? std::abs(particle.size) * (1.0F + std::abs(plan.tail_size)) * 1.414214F
                : (std::abs(particle.size) + std::abs(plan.tail_size)) * 1.414214F;
        }
        if (!finite(particle.position) || !std::isfinite(radius)) return {};
        const Vec3 low = particle.position - Vec3{radius, radius, radius};
        const Vec3 high = particle.position + Vec3{radius, radius, radius};
        if (!finite(low) || !finite(high)) return {};
        if (!bounds.valid) { bounds = {low, high, true}; continue; }
        bounds.minimum = {std::min(bounds.minimum.x, low.x), std::min(bounds.minimum.y, low.y), std::min(bounds.minimum.z, low.z)};
        bounds.maximum = {std::max(bounds.maximum.x, high.x), std::max(bounds.maximum.y, high.y), std::max(bounds.maximum.z, high.z)};
    }
    return bounds;
}

bool intersects(const ParticleBounds& bounds, const CullingFrame& frame) noexcept {
    // Unknown/empty geometry or camera fails open; it cannot prove a safe skip.
    if (!bounds.valid || !frame.valid) return true;
    for (const Vec4 plane : frame.planes) {
        if (!finite(plane) || (plane.x == 0 && plane.y == 0 && plane.z == 0)) return true;
    }
    for (const Vec4 plane : frame.planes) {
        const Vec3 nearest{plane.x >= 0 ? bounds.minimum.x : bounds.maximum.x,
            plane.y >= 0 ? bounds.minimum.y : bounds.maximum.y,
            plane.z >= 0 ? bounds.minimum.z : bounds.maximum.z};
        if (nearest.x * plane.x + nearest.y * plane.y + nearest.z * plane.z > plane.w) return false;
    }
    return true;
}

template<class ParticleAt>
void build_stream_impl(const EmitterRenderPlan& plan, const std::size_t particle_count,
                       const ParticleAt& particle_at, const CameraFrame& camera, VertexStream& stream,
                       StreamBuildTimings* const timing) {
    if (!plan.drawable) return;
    const bool xy = plan.family == RenderFamily::xy_aligned;
    stream.sorted_particles.clear();
    stream.sort_candidates = 0;
    const bool sorting = plan.sort_particles && plan.depth_test;
    const bool slot_order = !sorting && plan.legacy_slot_order && plan.blend == Blend::alpha;
    if (sorting || slot_order) {
        if (sorting && !finite(camera.position)) return;
        for (std::size_t index = 0; index < particle_count; ++index) {
            const Particle& particle = particle_at(index);
            if (particle.emitter_index == plan.emitter_index && particle.draw_eligible && finite(particle.position))
                stream.sorted_particles.push_back(&particle);
        }
        stream.sort_candidates = sorting ? stream.sorted_particles.size() : 0;
        const auto distance = [&](const Particle* particle) {
            const double x = static_cast<double>(particle->position.x) - camera.position.x;
            const double y = static_cast<double>(particle->position.y) - camera.position.y;
            const double z = static_cast<double>(particle->position.z) - camera.position.z;
            return x*x + y*y + z*z;
        };
        // PS-32: squared distance, farthest first. Stable IDs break equal-distance
        // ties deterministically without stable_sort's temporary allocation.
        // The MIT alo-viewer reference (same revision cited above),
        // ParticleEmitterInstance::AllocateParticle / QuadParticleRenderer,
        // fills free primitive indices backwards and draws indices forwards.
        // CPU logical slots are allocated forwards, so descending logical
        // slots reproduce that order, including slots reused after retirement.
        // Birth-ID order instead puts young grey keys over older gold keys.
        const auto sort_start = timing != nullptr ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        std::sort(stream.sorted_particles.begin(), stream.sorted_particles.end(),
            [&](const auto* a, const auto* b) {
                if (slot_order) return a->draw_slot != b->draw_slot
                    ? a->draw_slot > b->draw_slot : a->id < b->id;
                const double da = distance(a), db = distance(b);
                return da != db ? da > db : a->id < b->id;
            });
        if (timing != nullptr) timing->sort_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - sort_start).count();
    }
    const auto count = sorting || slot_order ? stream.sorted_particles.size() : particle_count;
    for (std::size_t index = 0; index < count; ++index) {
        const Particle& particle = sorting || slot_order ? *stream.sorted_particles[index] : particle_at(index);
        if (particle.emitter_index != plan.emitter_index || !particle.draw_eligible) continue;
        if (!finite(particle.position) || !std::isfinite(particle.size)) continue;
        const float size = particle.size;
        if (plan.family == RenderFamily::kites) {
            if (plan.legacy_kite_motion) {
                // MD-07: project total movement into the camera plane; the
                // authored length stretches the backward corner of a diamond.
                if (!finite(particle.motion_velocity) || !finite(camera.right) || !finite(camera.up)
                    || !std::isfinite(plan.tail_size)) continue;
                const float raw_x = dot(particle.motion_velocity, camera.right);
                const float raw_y = dot(particle.motion_velocity, camera.up);
                const float speed = std::hypot(raw_x, raw_y);
                // MD-08: perspective motion is tangent to this particle's sight ray.
                // Keep MD-07's projected speed for tail length, independently of direction.
                const Vec3 forward = normalized(cross(camera.up, camera.right));
                const Vec3 ray = particle.position - camera.position;
                const float depth = dot(ray, forward);
                Vec3 tangent = particle.motion_velocity;
                if (depth > 1.0e-6F && std::isfinite(depth))
                    tangent = tangent - ray * (dot(tangent, forward) / depth);
                const float x = dot(tangent, camera.right);
                const float y = dot(tangent, camera.up);
                const float axis_speed = std::hypot(x, y);
                if (!std::isfinite(speed)) continue;
                if (!(axis_speed > 1.0e-6F) || !std::isfinite(axis_speed)) {
                    append_quad(stream, particle, camera.right, camera.up,
                        {{{-size, size}, {-size, -size}, {size, size}, {size, -size}}}, particle.rotation);
                    continue;
                }
                const float reference = plan.inherit_emitter_motion
                    ? particle.inherited_speed_limit : plan.kite_speed_limit;
                if (!std::isfinite(reference) || reference < 0.0F) continue;
                const float limit = std::max(reference, speed);
                const float stretch = limit == 0.0F ? 1.0F : 1.0F + plan.tail_size * speed / limit;
                const Vec3 backward = camera.right * (-x / axis_speed) + camera.up * (-y / axis_speed);
                const Vec3 across = camera.right * (-y / axis_speed) + camera.up * (x / axis_speed);
                // MD-07: keep the UV center on the unstretched across diagonal,
                // at the particle anchor, so the glow joins the projectile head.
                const auto first = stream.vertices.size();
                append_quad(stream, particle, backward, across,
                    {{{size * stretch, 0}, {0, size}, {0, -size}, {-size, 0}}}, 0.0F);
                if (camera.collect_axis_diagnostics && stream.vertices.size() == first + 4) {
                    // Independent point projection of generated head/tail and one frame's motion.
                    const auto project = [&](const Vec3 point) -> std::optional<Vec3> {
                        const Vec3 offset = point - camera.position;
                        const float z = dot(offset, forward);
                        if (!(z > 1.0e-6F)) return std::nullopt;
                        return Vec3{dot(offset, camera.right) / z, dot(offset, camera.up) / z, 0};
                    };
                    const auto tail = project(stream.vertices[first].position);
                    const auto head = project(stream.vertices[first + 3].position);
                    const auto start = project(particle.position);
                    const auto end = project(particle.position + particle.motion_velocity * (1.0F / 30.0F));
                    if (tail && head && start && end) {
                        const Vec3 drawn = normalized(*head - *tail);
                        const Vec3 travel = normalized(*end - *start);
                        if (length(drawn) > 0 && length(travel) > 0) {
                            ++stream.axis_samples;
                            stream.axis_max_sine_error = std::max(stream.axis_max_sine_error,
                                static_cast<double>(length(cross(drawn, travel))));
                            if (dot(drawn, travel) < 0) ++stream.axis_reversed;
                        }
                    }
                }
                continue;
            }
            // Kite: the quad plane contains the velocity and faces the camera;
            // the tail corner trails the velocity by the alo-viewer 3/4-pi turn.
            if (!finite(particle.velocity) || !std::isfinite(plan.tail_size) ||
                !std::isfinite(dot(particle.velocity, particle.velocity))) continue;
            const Vec3 along = normalized(particle.velocity);
            Vec3 across{};
            Vec3 axis_y = along;
            if (length(along) != 0.0F) {
                const Vec3 to_camera = camera.position - particle.position;
                if (!finite(camera.position) || !finite(to_camera)) continue;
                const Vec3 across_raw = cross(along, to_camera);
                if (!finite(across_raw) || !std::isfinite(dot(across_raw, across_raw))) continue;
                across = normalized(across_raw);
            }
            if (length(along) == 0.0F || length(across) == 0.0F) {
                // No defined direction of travel: face the camera instead.
                across = camera.right;
                axis_y = camera.up;
            }
            const float half = size * 0.5F;
            const float tail = (size + plan.tail_size) * 0.5F;
            append_quad(stream, particle, across, axis_y,
                {{{-tail, tail}, {-half, -half}, {half, half}, {half, -half}}}, 0.75F * pi);
            continue;
        }
        const Vec3 axis_x = xy ? Vec3{1.0F, 0.0F, 0.0F} : camera.right;
        const Vec3 axis_y = xy ? Vec3{0.0F, 1.0F, 0.0F} : camera.up;
        append_quad(stream, particle, axis_x, axis_y,
            {{{-size, size}, {-size, -size}, {size, size}, {size, -size}}},
            particle.rotation, plan.triangles); // PS-29: CPU orientation is already radians.
    }
}

void build_stream(const EmitterRenderPlan& plan, const std::span<const Particle> particles,
                  const CameraFrame& camera, VertexStream& stream, StreamBuildTimings* const timing) {
    build_stream_impl(plan, particles.size(), [&](const std::size_t index) -> const Particle& {
        return particles[index];
    }, camera, stream, timing);
}

void build_stream_subset(const EmitterRenderPlan& plan, const std::span<const Particle* const> particles,
                         const CameraFrame& camera, VertexStream& stream, StreamBuildTimings* const timing) {
    build_stream_impl(plan, particles.size(), [&](const std::size_t index) -> const Particle& {
        return *particles[index];
    }, camera, stream, timing);
}

std::uint64_t stream_hash(const VertexStream& stream, std::uint64_t seed) {
    std::uint64_t hash = seed;
    mix_hash(hash, static_cast<std::uint32_t>(stream.vertices.size()));
    for (const ParticleVertex& vertex : stream.vertices) {
        mix_hash(hash, vertex.position.x); mix_hash(hash, vertex.position.y);
        mix_hash(hash, vertex.position.z);
        mix_hash(hash, vertex.color.x); mix_hash(hash, vertex.color.y);
        mix_hash(hash, vertex.color.z); mix_hash(hash, vertex.color.w);
        mix_hash(hash, vertex.u); mix_hash(hash, vertex.v);
    }
    for (const std::uint32_t index : stream.indices) mix_hash(hash, index);
    return hash;
}


} // namespace eawr::presentation::particles
