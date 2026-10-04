#include "eawr/presentation/particles/particles.hpp"
#include "eawr/presentation/particles/prewarmed_capacity.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

// CPU behavior is ported from the MIT-licensed alo-viewer revision
// 9bb0053919cc5df8377610d4f91b11d956d6c2f4. DirectX matrices and renderer
// ownership are intentionally replaced by portable presentation values.

namespace eawr::presentation::particles {
namespace {

constexpr float pi=3.14159265358979323846F;

Vec3 operator+(const Vec3 a,const Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
Vec3 operator-(const Vec3 a,const Vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
Vec3 operator*(const Vec3 a,const float b){return {a.x*b,a.y*b,a.z*b};}
Vec3& operator+=(Vec3& a,const Vec3 b){a=a+b;return a;}
float length(const Vec3 value){return std::sqrt(value.x*value.x+value.y*value.y+value.z*value.z);}
Vec3 normalized(const Vec3 value){const auto magnitude=length(value);return magnitude>1.0e-12F?value*(1.0F/magnitude):Vec3{};}
Vec3 transformed(const Basis3& basis,const Vec3 value){return basis.x*value.x+basis.y*value.y+basis.z*value.z;}
float dot(const Vec3 a,const Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(const Vec3 a,const Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
bool same(const Vec3 a,const Vec3 b){return a.x==b.x&&a.y==b.y&&a.z==b.z;}
bool same(const Basis3& a,const Basis3& b){return same(a.x,b.x)&&same(a.y,b.y)&&same(a.z,b.z);}
bool finite(const Vec3 value){return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);}
bool finite(const Vec4 value){return finite(Vec3{value.x,value.y,value.z})&&std::isfinite(value.w);}
bool finite(const Basis3& basis){return finite(basis.x)&&finite(basis.y)&&finite(basis.z);}
bool finite(const MeshFrame& frame){return finite(frame.origin)&&finite(frame.basis);}
bool finite(const PropertyGroup& group){return finite(group.point)&&finite(group.direction)&&
    std::isfinite(group.magnitude_min)&&std::isfinite(group.magnitude_max)&&
    std::isfinite(group.radius_min)&&std::isfinite(group.radius_max)&&finite(group.range_min)&&
    finite(group.range_max)&&std::isfinite(group.angle_min)&&std::isfinite(group.angle_max)&&
    std::isfinite(group.spherical_radius_min)&&std::isfinite(group.spherical_radius_max)&&
    std::isfinite(group.cylinder_radius)&&std::isfinite(group.cylinder_height_min)&&
    std::isfinite(group.cylinder_height_max)&&std::isfinite(group.torus_radius)&&std::isfinite(group.tube_radius);}
bool finite(const ScalarTrack& track){return std::all_of(track.keys.begin(),track.keys.end(),
    [](const ScalarKey& key){return std::isfinite(key.time)&&std::isfinite(key.value);});}
bool finite(const EmitterDefinition& emitter){return std::isfinite(emitter.particles_per_interval)&&
    std::isfinite(emitter.mesh_surface_offset)&&
    std::isfinite(emitter.spawn_interval)&&std::isfinite(emitter.start_delay)&&std::isfinite(emitter.stop_time)&&
    std::isfinite(emitter.skip_time)&&std::isfinite(emitter.freeze_time)&&
    std::isfinite(emitter.inherited_velocity_scale)&&std::isfinite(emitter.max_inherited_velocity)&&
    finite(emitter.position)&&finite(emitter.velocity)&&std::isfinite(emitter.lifetime)&&
    std::isfinite(emitter.lifetime_variation)&&(!emitter.lifetime_range||
        (std::isfinite(emitter.lifetime_range->minimum)&&std::isfinite(emitter.lifetime_range->maximum)))&&
    std::isfinite(emitter.inward_speed)&&finite(emitter.acceleration)&&
    std::isfinite(emitter.inward_acceleration)&&std::isfinite(emitter.wind_response)&&
    std::isfinite(emitter.terrain_elasticity)&&finite(emitter.red)&&finite(emitter.green)&&finite(emitter.blue)&&
    finite(emitter.alpha)&&finite(emitter.size)&&std::isfinite(emitter.size_variation)&&finite(emitter.uv_index)&&
    finite(emitter.rotation_rate)&&std::isfinite(emitter.random_rotation_average)&&
    std::isfinite(emitter.random_rotation_variation)&&finite(emitter.color_variance);}
float saturated(const float value){return std::clamp(value,0.0F,1.0F);}
float mix(const float first,const float last,float amount,const Interpolation interpolation){
    amount=saturated(amount);if(interpolation==Interpolation::step)return first;
    if(interpolation==Interpolation::smooth)amount=amount*amount*(3.0F-2.0F*amount);
    return first+(last-first)*amount;
}
float sample_track(const ScalarTrack& track,const float time,const float fallback=0.0F){
    if(track.keys.empty()){return fallback;}
    if(track.keys.size()==1||time<=track.keys.front().time){return track.keys.front().value;}
    const auto next=std::find_if(track.keys.begin()+1,track.keys.end(),[&](const ScalarKey& key){return time<=key.time;});
    if(next==track.keys.end()){return track.keys.back().value;}
    const auto& previous=*(next-1);
    const auto width=next->time-previous.time;return width>0?mix(previous.value,next->value,(time-previous.time)/width,track.interpolation):next->value;
}
bool has(const EmitterDefinition& emitter,const std::uint32_t id){return std::find(emitter.modifier_ids.begin(),emitter.modifier_ids.end(),id)!=emitter.modifier_ids.end();}
void saturating_add(std::size_t& target,const std::size_t value){
    target=value>std::numeric_limits<std::size_t>::max()-target?
        std::numeric_limits<std::size_t>::max():target+value;
}
// FoC takes the atlas cell as the integer part of the sampled UV track value
// (docs/rendering.md, "V1 particle size and bump lighting"), not the nearest cell.
int safe_truncated_index(const float value){
    if(!std::isfinite(value))return 0;
    if(value>=static_cast<float>(std::numeric_limits<int>::max()))return std::numeric_limits<int>::max();
    if(value<=static_cast<float>(std::numeric_limits<int>::min()))return std::numeric_limits<int>::min();
    return static_cast<int>(value);
}

} // namespace

core::Result<void> validate_mesh_binding(const MeshBinding& binding,const MeshSpawnMode required_mode){
    const auto fail=[](const std::string& message){return core::Result<void>::failure({
        std::string(diagnostic_codes::invalid_value),core::Severity::error,message,{},{},{},{}});};
    if(!finite(binding.frame))return fail("mesh frame contains a non-finite value");
    if(binding.geometry.submeshes.empty()||
       binding.geometry.submeshes.size()>std::numeric_limits<std::uint32_t>::max())
        return fail("mesh has no submeshes or too many submeshes");
    std::size_t total{};
    for(const auto& submesh:binding.geometry.submeshes){
        if(submesh.vertices.empty()||submesh.vertices.size()>std::numeric_limits<std::uint32_t>::max())
            return fail("mesh submesh has no vertices or too many vertices");
        if(submesh.vertices.size()>std::numeric_limits<std::size_t>::max()-total)
            return fail("mesh vertex count overflows");
        total+=submesh.vertices.size();
        if(total>std::numeric_limits<std::uint32_t>::max())
            return fail("mesh total vertex count exceeds the bounded source count");
        for(const auto& vertex:submesh.vertices)
            if(!finite(vertex.position)||!finite(vertex.normal))
                return fail("mesh vertex contains a non-finite value");
        if(submesh.triangle_indices.size()%3!=0)
            return fail("mesh submesh has an incomplete triangle");
        if(required_mode==MeshSpawnMode::random_surface&&submesh.triangle_indices.empty())
            return fail("surface emission requires triangles in every submesh");
        if(submesh.triangle_indices.size()/3>std::numeric_limits<std::uint32_t>::max())
            return fail("mesh has too many triangles");
        for(const auto index:submesh.triangle_indices)
            if(index>=submesh.vertices.size())return fail("mesh triangle index is out of range");
    }
    return core::Result<void>::success();
}

CpuSystem::CpuSystem(SystemDefinition definition,const std::uint32_t seed,const std::size_t max_particles,
                     std::optional<MeshBinding> mesh_binding)
    :definition_(std::move(definition)),random_state_(seed?seed:1U),max_particles_(max_particles),
     mesh_binding_(std::move(mesh_binding)){
    // PL-01, PS-47: keep raw decode separate from the legacy post-load runtime.
    if(definition_.version==AloParticleVersion::legacy_v1){
        for(auto& emitter:definition_.emitters)
            if(finite(emitter))emitter.lifetime_range=postload_lifetime_range(emitter);
    }
    if(mesh_binding_){
        bool surface{};
        for(const auto& emitter:definition_.emitters)
            surface|=emitter.creator_id==35&&emitter.mesh_mode==MeshSpawnMode::random_surface;
        if(!validate_mesh_binding(*mesh_binding_,surface?MeshSpawnMode::random_surface:MeshSpawnMode::random_vertex))
            mesh_binding_.reset();
        else for(const auto& submesh:mesh_binding_->geometry.submeshes)
            mesh_vertex_count_+=submesh.vertices.size();
    }
    emitters_.reserve(definition_.emitters.size());
    for(const auto& emitter:definition_.emitters){
        const bool mesh=definition_.version==AloParticleVersion::legacy_v1&&
            emitter.creator_id==35&&mesh_binding_.has_value()&&
            (emitter.mesh_mode==MeshSpawnMode::random_vertex||
             emitter.mesh_mode==MeshSpawnMode::random_surface||
             emitter.mesh_mode==MeshSpawnMode::every_vertex);
        const long double mesh_base_count=emitter.bursting?
            std::trunc(static_cast<long double>(emitter.particles_per_interval)):1.0L;
        const long double size_limit=std::ldexp(1.0L,std::numeric_limits<std::size_t>::digits);
        const bool mesh_count_fits=!mesh||(std::isfinite(emitter.particles_per_interval)&&
            mesh_base_count>=0&&
            mesh_base_count<size_limit&&
            (emitter.mesh_mode!=MeshSpawnMode::every_vertex||
             (mesh_vertex_count_!=0&&mesh_base_count*mesh_vertex_count_<size_limit)));
        const bool valid=emitter.cpu_ready&&(emitter.creator_id==34||mesh||emitter.creator_id==39||emitter.creator_id==40)&&
            finite(emitter)&&emitter.particles_per_interval>0.0F&&emitter.spawn_interval>0.0F&&mesh_count_fits;
        emitters_.push_back({emitter.start_delay,valid&&(emitter.creator_id==34||mesh)&&
            (mesh||emitter.parent_emitter==EmitterDefinition::no_parent)});
    }
    for(std::size_t index=0;index<definition_.emitters.size();++index){
        if(definition_.emitters[index].creator_id!=35)continue;
        std::size_t node=index;
        for(std::size_t hops=0;hops<=definition_.emitters.size();++hops){
            const auto parent=definition_.emitters[node].parent_emitter;
            if(parent==EmitterDefinition::no_parent)break;
            if(parent>=definition_.emitters.size()||hops==definition_.emitters.size()){
                emitters_[index].active=false;break;
            }
            node=parent;
        }
    }
    // Programmatic definitions receive the same cycle and parent validation as ALO files.
    for(std::size_t child=0;child<definition_.emitters.size();++child){
        const auto& emitter=definition_.emitters[child];
        if(emitter.creator_id!=39&&emitter.creator_id!=40)continue;
        bool valid=emitter.cpu_ready&&finite(emitter)&&emitter.particles_per_interval>0.0F&&
            emitter.spawn_interval>0.0F&&emitter.parent_emitter!=EmitterDefinition::no_parent;
        std::size_t node=child;
        for(std::size_t hops=0;valid&&hops<=definition_.emitters.size();++hops){
            const auto parent=definition_.emitters[node].parent_emitter;
            if(parent==EmitterDefinition::no_parent)break;
            if(parent>=definition_.emitters.size()||hops==definition_.emitters.size()){valid=false;break;}
            node=parent;
        }
        emitters_[child].active=valid;
    }
    children_.resize(definition_.emitters.size());
    for(std::size_t child=0;child<definition_.emitters.size();++child){
        const auto parent=definition_.emitters[child].parent_emitter;
        if(parent<children_.size()&&emitters_[child].active&&
            (definition_.emitters[child].creator_id==39||definition_.emitters[child].creator_id==40))
            children_[parent].push_back(child);
    }
    capacities_=emitter_capacity_plan(definition_,mesh_vertex_count_,max_particles_);
    std::size_t reserved{};
    for(std::size_t index=0;index<capacities_.size();++index){
        if(!emitters_[index].active)capacities_[index].reserved=0;
        emitters_[index].free_slots.reserve(capacities_[index].reserved);
        reserved+=capacities_[index].reserved;
    }
    // PS-19: packed storage is shared, admission and reusable slots are not.
    particles_.reserve(reserved);
    std::size_t parent_slots{},links{};
    const auto link_budget=std::min<std::size_t>(reserved,65536U);
    for(std::size_t index=0;index<children_.size();++index){
        if(!children_[index].empty())parent_slots+=capacities_[index].reserved;
        for(const auto child:children_[index])if(definition_.emitters[child].creator_id==39){
            const auto extra=std::min(link_budget-links,capacities_[index].reserved);
            links+=extra;
        }
    }
    events_.reserve(parent_slots*2U);
    max_child_instances_=links;
    child_instances_.reserve(links);
    emitter_start_.assign(definition_.emitters.size(),0.0F);
    enabled_.assign(definition_.emitters.size(),1U);
}

core::Result<void> CpuSystem::set_detail(ParticleDetail detail){
    if(!std::isfinite(detail.global)||!std::isfinite(detail.local))
        return core::Result<void>::failure({std::string(diagnostic_codes::invalid_value),
            core::Severity::error,"particle detail inputs must be finite",{},{},{},{}});
    detail.global=std::clamp(detail.global,0.0F,1.0F);
    detail.local=std::clamp(detail.local,0.0F,1.0F);
    if(detail==detail_)return core::Result<void>::success();
    const bool local_changed=detail.local!=detail_.local;
    detail_=detail;
    if(local_changed){
        // PS-36: band thresholds are inclusive; emitter count is independent of draw detail.
        const float band=detail.local<=0.5F?0.5F:detail.local<=0.7F?0.7F:1.0F;
        const auto count=static_cast<std::size_t>(std::ceil(static_cast<float>(emitters_.size())*band));
        for(std::size_t index=0;index<enabled_.size();++index){
            enabled_[index]=index<count?1U:0U;
            std::size_t node=index;
            for(std::size_t hops=0;enabled_[index]&&hops<=enabled_.size();++hops){
                const auto parent=definition_.emitters[node].parent_emitter;
                if(parent==EmitterDefinition::no_parent)break;
                if(parent>=count||parent>=enabled_.size()||hops==enabled_.size())enabled_[index]=0;
                else node=parent;
            }
        }
    }
    for(auto& particle:particles_)particle.draw_eligible=draw_eligible(particle);
    return core::Result<void>::success();
}

std::size_t CpuSystem::reserved_memory_bytes() const {
    std::size_t bytes=particles_.capacity()*sizeof(Particle)+events_.capacity()*sizeof(ParentEvent)
        +child_instances_.capacity()*sizeof(ChildInstance);
    for(const auto& emitter:emitters_)bytes+=emitter.free_slots.capacity()*sizeof(std::size_t);
    return bytes;
}

bool CpuSystem::emitter_enabled(const std::size_t index) const noexcept{
    return index<enabled_.size()&&enabled_[index]!=0;
}

bool CpuSystem::draw_eligible(const Particle& particle) const noexcept{
    const auto& emitter=definition_.emitters[particle.emitter_index];
    // PS-35: nonweather every-vertex draws all slots, including at zero detail.
    if(emitter.mesh_mode==MeshSpawnMode::every_vertex&&!emitter.weather)return true;
    const auto capacity=capacities_[particle.emitter_index].reserved;
    if(capacity==0)return false;
    const float k=std::ceil(static_cast<float>(capacity)*detail_.local*detail_.global);
    if(k>=static_cast<float>(capacity))return true;
    if(k<=0)return false;
    // PS-35: masks use the independent emitter's admitted allocation denominator.
    const auto rank=(static_cast<std::uint64_t>(particle.draw_slot)*60659ULL+60913ULL)%capacity;
    return rank<static_cast<std::size_t>(k);
}

void CpuSystem::set_origin(const Vec3 origin){
    if(!finite(origin))return;
    if(!origin_set_){sampled_origin_=origin;origin_set_=true;}
    origin_=origin;
}
void CpuSystem::set_basis(const Basis3 basis){if(finite(basis))basis_=basis;}
void CpuSystem::set_wind(const Vec3 acceleration){if(finite(acceleration))wind_=acceleration;}
core::Result<void> CpuSystem::set_mesh_frame(const MeshFrame& frame){
    if(!mesh_binding_||!finite(frame))return core::Result<void>::failure({
        std::string(diagnostic_codes::invalid_value),core::Severity::error,
        "mesh binding is absent or frame is non-finite",{},{},{},{}});
    mesh_binding_->frame=frame;
    return core::Result<void>::success();
}

float CpuSystem::random(const float minimum,const float maximum){
    random_state_^=random_state_<<13U;random_state_^=random_state_>>17U;random_state_^=random_state_<<5U;
    const float unit=static_cast<float>(random_state_>>8U)*(1.0F/16777216.0F);
    return minimum+(maximum-minimum)*unit;
}

std::uint32_t CpuSystem::random_index(const std::uint32_t upper){
    // One xorshift32 draw, scaled by multiply-high to the half-open [0, upper).
    // All 32 random bits participate; no floating upper endpoint can be selected.
    random_state_^=random_state_<<13U;random_state_^=random_state_>>17U;random_state_^=random_state_<<5U;
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(random_state_)*upper)>>32U);
}

Vec3 CpuSystem::sample(const PropertyGroup& group,const bool hollow){
    switch(group.shape){
    case Shape::point:return group.point;
    case Shape::direction:{const float magnitude=hollow?(random(0,1)<0.5F?group.magnitude_min:group.magnitude_max):random(group.magnitude_min,group.magnitude_max);return normalized(group.direction)*magnitude;}
    case Shape::sphere:{const float radius=hollow?group.radius_max:random(group.radius_min,group.radius_max);const float azimuth=random(0,2*pi),tilt=random(-pi,pi);const float planar=std::cos(tilt);return {std::cos(azimuth)*planar*radius,std::sin(azimuth)*planar*radius,std::sin(tilt)*radius};}
    case Shape::range:return {random(group.range_min.x,group.range_max.x),random(group.range_min.y,group.range_max.y),random(group.range_min.z,group.range_max.z)};
    case Shape::spherical_range:{const float angle=random(group.angle_min,group.angle_max),radius=random(group.spherical_radius_min,group.spherical_radius_max),azimuth=random(0,2*pi);return {std::cos(azimuth)*std::cos(angle)*radius,std::sin(azimuth)*std::cos(angle)*radius,std::sin(angle)*radius};}
    case Shape::cylinder:{const float radius=hollow?group.cylinder_radius:random(0,group.cylinder_radius),angle=random(0,2*pi);return {std::cos(angle)*radius,std::sin(angle)*radius,random(group.cylinder_height_min,group.cylinder_height_max)};}
    case Shape::torus:{const float radius=hollow?group.tube_radius:random(0,group.tube_radius),around=random(0,2*pi),tube=random(0,2*pi);const float ring=group.torus_radius+std::cos(tube)*radius;return {std::cos(around)*ring,std::sin(around)*ring,std::sin(tube)*radius};}
    }
    return {};
}

void CpuSystem::initialize_particle(Particle& particle,const EmitterDefinition& emitter,
                                    const std::size_t emitter_index,const float spawn_time,const Particle* parent){
    particle={};particle.id=next_particle_id_++;particle.emitter_index=emitter_index;particle.spawn_time=spawn_time;
    Vec3 mesh_offset_normal{};
    if(emitter.creator_id==35){
        // EnhancedMesh overrides Shape position sampling and parent inheritance.
        particle.velocity=sample(emitter.velocity,emitter.hollow_velocity);
        if(emitter.local_velocity)particle.velocity=transformed(basis_,particle.velocity);
        const auto& geometry=mesh_binding_->geometry;
        auto& state=emitters_[emitter_index];
        std::size_t submesh_index{},vertex_index{};
        if(emitter.mesh_mode==MeshSpawnMode::every_vertex){
            submesh_index=state.submesh;vertex_index=state.vertex;
            if(++state.vertex==geometry.submeshes[state.submesh].vertices.size()){
                state.vertex=0;state.submesh=(state.submesh+1)%geometry.submeshes.size();
            }
        }else{
            submesh_index=random_index(static_cast<std::uint32_t>(geometry.submeshes.size()));
            if(emitter.mesh_mode==MeshSpawnMode::random_vertex)
                vertex_index=random_index(static_cast<std::uint32_t>(geometry.submeshes[submesh_index].vertices.size()));
        }
        const auto& submesh=geometry.submeshes[submesh_index];
        MeshVertex vertex;
        if(emitter.mesh_mode==MeshSpawnMode::random_surface){
            const std::size_t face=static_cast<std::size_t>(
                random_index(static_cast<std::uint32_t>(submesh.triangle_indices.size()/3)))*3U;
            const float w1=random(0,1),w2=random(0,1-w1),w3=1-w1-w2;
            const auto& a=submesh.vertices[submesh.triangle_indices[face]];
            const auto& b=submesh.vertices[submesh.triangle_indices[face+1]];
            const auto& c=submesh.vertices[submesh.triangle_indices[face+2]];
            vertex.position=a.position*w1+b.position*w2+c.position*w3;
            vertex.normal=a.normal*w1+b.normal*w2+c.normal*w3;
        }else vertex=submesh.vertices[vertex_index];
        const auto& mesh_frame=mesh_binding_->frame;
        const Vec3 normal=transformed(mesh_frame.basis,vertex.normal);
        particle.position=mesh_frame.origin+transformed(mesh_frame.basis,vertex.position);
        if(emitter.mesh_mode!=MeshSpawnMode::every_vertex)mesh_offset_normal=normalized(normal);
        const float tilt=std::atan2(normal.z,std::sqrt(normal.x*normal.x+normal.y*normal.y));
        const float azimuth=std::atan2(normal.y,normal.x);
        const float first=particle.velocity.x,second=particle.velocity.y;
        particle.velocity={-second,first,particle.velocity.z}; // source Z quarter-turn
        const float y_angle=pi/2-tilt,cy=std::cos(y_angle),sy=std::sin(y_angle);
        particle.velocity={particle.velocity.x*cy+particle.velocity.z*sy,
            particle.velocity.y,-particle.velocity.x*sy+particle.velocity.z*cy};
        const float cz=std::cos(azimuth),sz=std::sin(azimuth);
        particle.velocity={particle.velocity.x*cz-particle.velocity.y*sz,
            particle.velocity.x*sz+particle.velocity.y*cz,particle.velocity.z};
    }else{
        particle.position=transformed(basis_,sample(emitter.position,emitter.hollow_position))+(parent?parent->position:origin_);
        particle.velocity=sample(emitter.velocity,emitter.hollow_velocity);
        if(emitter.local_velocity)particle.velocity=transformed(basis_,particle.velocity);
    }
    if(emitter.creator_id!=35&&parent&&emitter.inherit_parent_velocity){
        const float speed=length(parent->velocity);
        particle.velocity+=normalized(parent->velocity)*std::min(speed*emitter.inherited_velocity_scale,
            emitter.max_inherited_velocity);
    }
    particle.texcoords={0,0,1,1};particle.color={0.1F,1.0F,0.5F,1.0F};particle.size=1.0F;
    particle.death_time=spawn_time+emitter.lifetime;
    // PL-01: legacy births use the post-load sampler; constants keep their saved default.
    if(emitter.lifetime_range){
        const auto range=*emitter.lifetime_range;
        particle.death_time=spawn_time+(range.minimum==range.maximum?range.minimum:random(range.minimum,range.maximum));
    }else if(emitter.lifetime_variation>0)particle.death_time+=random(0,emitter.lifetime_variation)*emitter.lifetime;
    particle.size_scale=1.0F;
    if(emitter.size_variation>0)particle.size_scale+=random(-emitter.size_variation,emitter.size_variation);
    particle.rotation_direction=emitter.random_rotation_direction&&random(0,1)<0.5F?-1.0F:1.0F;
    if(emitter.random_rotation){particle.rotation=emitter.random_rotation_average;if(emitter.random_rotation_variation>0)particle.rotation+=random(-emitter.random_rotation_variation,emitter.random_rotation_variation)*particle.rotation;particle.rotation*=particle.rotation_direction;}
    if(has(emitter,53)){
        particle.color_offset.x=random(0,emitter.color_variance.x);
        particle.color_offset.y=emitter.grayscale_variance?particle.color_offset.x:random(0,emitter.color_variance.y);
        particle.color_offset.z=emitter.grayscale_variance?particle.color_offset.x:random(0,emitter.color_variance.z);
        particle.color_offset.w=emitter.grayscale_variance?particle.color_offset.x:random(0,emitter.color_variance.w);
    }
    if(emitter.creator_id==35&&emitter.mesh_mode!=MeshSpawnMode::every_vertex){
        // Mesh emission: the debug build offsets random samples by half the
        // sampled billboard half-extent, along the normalized world normal.
        const float lifetime=particle.death_time-particle.spawn_time;
        const float relative=lifetime>0?saturated((time_-particle.spawn_time)/lifetime):1.0F;
        const float size=std::max(0.0F,particle.size_scale*sample_track(emitter.size,relative,1));
        particle.position+=mesh_offset_normal*(emitter.mesh_surface_offset*size*0.5F);
    }
    if(emitter.inward_speed!=0.0F)particle.velocity=normalized(particle.position-origin_)*emitter.inward_speed;
    particle.motion_velocity=particle.velocity;
    particle.inherited_speed_limit=inherited_speed_limit_;
    update_particle(particle,emitter,0.0F);
}

void CpuSystem::update_particle(Particle& particle,const EmitterDefinition& emitter,const float delta){
    const float lifetime=particle.death_time-particle.spawn_time;
    const float relative=lifetime>0?saturated((time_-particle.spawn_time)/lifetime):1.0F;
    particle.acceleration={};
    if(has(emitter,10))particle.acceleration=emitter.acceleration_local?transformed(basis_,emitter.acceleration):emitter.acceleration;
    if(has(emitter,11))particle.acceleration+=normalized(particle.position-origin_)*emitter.inward_acceleration;
    if(has(emitter,54))particle.acceleration+=wind_*emitter.wind_response;
    particle.color={sample_track(emitter.red,relative,1),sample_track(emitter.green,relative,1),sample_track(emitter.blue,relative,1),sample_track(emitter.alpha,relative,1)};
    if(has(emitter,53)){particle.color.x=saturated(particle.color.x+particle.color_offset.x);particle.color.y=saturated(particle.color.y+particle.color_offset.y);particle.color.z=saturated(particle.color.z+particle.color_offset.z);particle.color.w=saturated(particle.color.w+particle.color_offset.w);}
    particle.size=std::max(0.0F,particle.size_scale*sample_track(emitter.size,relative,1));
    // PS-30: integer square root, including nonsquare authored frame counts.
    const int grid=std::max(1,static_cast<int>(std::sqrt(static_cast<double>(emitter.texture_size))));
    const int uv=safe_truncated_index(sample_track(emitter.uv_index,relative,0));
    particle.texcoords={static_cast<float>(uv%grid)/grid,static_cast<float>(uv/grid)/grid,1.0F/grid,1.0F/grid};
    if(!emitter.random_rotation)particle.rotation+=delta*particle.rotation_direction*sample_track(emitter.rotation_rate,relative,0);
}

std::size_t CpuSystem::live_child_instances() const{
    return static_cast<std::size_t>(std::count_if(child_instances_.begin(),child_instances_.end(),
        [](const ChildInstance& instance){return instance.active;}));
}

void CpuSystem::spawn_batch(const std::size_t index,const float spawn_time,const Particle* parent,
                            AdvanceStats& stats,std::vector<ParentEvent>& events){
    const auto& emitter=definition_.emitters[index];
    // PS-36/PS-37: reject births only; already live particles still update and retire.
    if(!emitter_enabled(index))return;
    if(emitter.creator_id==39&&(detail_.local<=0.7F||
        (!emitter.bursting&&parent&&!draw_eligible(*parent))))return;
    const float requested=emitter.bursting?emitter.particles_per_interval:1.0F;
    const std::size_t base_count=requested>=static_cast<float>(std::numeric_limits<std::size_t>::max())?
        std::numeric_limits<std::size_t>::max():static_cast<std::size_t>(requested);
    const std::size_t count=emitter.creator_id==35&&emitter.mesh_mode==MeshSpawnMode::every_vertex?
        base_count*mesh_vertex_count_:base_count;
    auto& state=emitters_[index];
    const std::size_t available=capacities_[index].reserved-state.live;
    const std::size_t spawn_count=std::min(count,available);
    saturating_add(stats.requested,count);
    for(std::size_t particle_index=0;particle_index<spawn_count;++particle_index){
        Particle particle;initialize_particle(particle,emitter,index,spawn_time,parent);
        if(state.free_slots.empty())particle.draw_slot=state.next_slot++;
        else{particle.draw_slot=state.free_slots.back();state.free_slots.pop_back();}
        particle.draw_eligible=draw_eligible(particle);
        particles_.push_back(particle);
        if(!children_[index].empty())events.push_back({particle,false});
        ++stats.spawned;++state.live;
    }
    const std::size_t dropped=count-spawn_count;
    saturating_add(stats.dropped_at_capacity,dropped);saturating_add(total_dropped_,dropped);
}

void CpuSystem::process_events(std::vector<ParentEvent>& events,AdvanceStats& stats){
    std::size_t cursor{};
    std::size_t active_instances=live_child_instances();
    while(cursor<events.size()){
        const ParentEvent event=events[cursor++];
        for(const std::size_t index:children_[event.parent.emitter_index]){
            if(frozen(index))continue;
            const auto& emitter=definition_.emitters[index];
            if(event.death){
                // PS-22/PS-36: draw masks do not stand in for death-spawn permission.
                if(emitter.creator_id!=40||detail_.local<=0.5F||!emitter_enabled(index))continue;
                ++stats.death_bursts;
                spawn_batch(index,time_,&event.parent,stats,events);
            }else{
                if(emitter.creator_id!=39)continue;
                if(active_instances>=max_child_instances_){
                    ++stats.instances_dropped_at_capacity;continue;
                }
                // Retain the per-parent link while its birth gate is closed: a later
                // detail increase can resume trails without recreating the parent.
                ChildInstance instance;
                instance.id=next_instance_id_++;
                instance.emitter_index=index;
                instance.parent_id=event.parent.id;
                instance.parent_snapshot=event.parent;
                instance.start_time=event.parent.spawn_time;
                instance.next_spawn=instance.start_time+emitter.start_delay;
                if(!std::isfinite(instance.next_spawn))continue;
                child_instances_.push_back(instance);++stats.child_instances_started;++active_instances;
                if(time_>=instance.next_spawn){
                    spawn_batch(index,instance.next_spawn,&event.parent,stats,events);
                    auto& added=child_instances_.back();
                    if(emitter.stop_time>0&&added.next_spawn-added.start_time>=emitter.stop_time)
                        added.active=false;
                    else{
                        const float delay=emitter.bursting?emitter.spawn_interval:
                            emitter.spawn_interval/emitter.particles_per_interval;
                        added.next_spawn+=delay;
                        if(!(added.next_spawn>instance.next_spawn)||!std::isfinite(added.next_spawn))added.active=false;
                    }
                }
                if(!child_instances_.back().active){--active_instances;child_instances_.pop_back();}
            }
        }
    }
    events.clear();
}

bool CpuSystem::frozen(const std::size_t emitter_index) const{
    return emitters_[emitter_index].frozen;
}

void CpuSystem::preroll(AdvanceStats& stats){
    std::vector<float> durations(definition_.emitters.size());
    std::vector<std::uint32_t> counts(definition_.emitters.size());
    std::uint32_t steps{};
    for(std::size_t index=0;index<definition_.emitters.size();++index){
        const float target=definition_.emitters[index].skip_time;
        if(frozen(index)||!std::isfinite(target)||target<=0.0F)continue;
        const float bounded=std::min(target,max_skip_seconds);
        std::uint32_t count{};
        // PS-07: inclusive comparison against accumulated single-precision time.
        while(durations[index]<=bounded){durations[index]+=preroll_step;++count;}
        counts[index]=count;
        steps=std::max(steps,count);
    }
    if(steps==0)return;
    std::vector<float> times(static_cast<std::size_t>(steps)+1U);
    for(std::uint32_t index=0;index<steps;++index)times[index+1U]=times[index]+preroll_step;
    // Align by complete step count on the same float clock sequence, avoiding
    // cancellation that could drop a short emitter's first prewarm step.
    // Zero-target emitters begin only on the ordinary caller update.
    for(std::size_t index=0;index<definition_.emitters.size();++index){
        const float start=times[steps-counts[index]];
        emitter_start_[index]=start;
        emitters_[index].next_spawn+=start;
    }
    for(std::uint32_t index=0;index<steps;++index)
        step_segment(preroll_step,stats,true);
    for(std::size_t index=0;index<emitters_.size();++index)
        emitters_[index].elapsed=durations[index];
    // Rebase: presentation time restarts at zero with the pre-rolled state.
    const float shift=time_;
    time_=0.0F;
    for(auto& particle:particles_){particle.spawn_time-=shift;particle.death_time-=shift;}
    for(auto& state:emitters_)state.next_spawn-=shift;
    for(auto& start:emitter_start_)start-=shift;
    for(auto& instance:child_instances_){
        instance.next_spawn-=shift;instance.start_time-=shift;
        instance.parent_snapshot.spawn_time-=shift;instance.parent_snapshot.death_time-=shift;
    }
}

AdvanceStats CpuSystem::advance(const float delta_seconds){
    AdvanceStats stats;if(!std::isfinite(delta_seconds)||delta_seconds<0||
        delta_seconds>std::numeric_limits<float>::max()-time_)return stats;
    if(!prerolled_){
        prerolled_=true;
        // PS-08: the first outer update admits freeze before prewarming.
        freeze_crossing(delta_seconds);
        preroll(stats);
        step_segment(delta_seconds,stats);
    }else step(delta_seconds,stats);
    return stats;
}

void CpuSystem::step(const float delta_seconds,AdvanceStats& stats){
    freeze_crossing(delta_seconds);
    step_segment(delta_seconds,stats);
}

void CpuSystem::freeze_crossing(const float delta_seconds){
    // PS-08: equality advances; strict crossing freezes permanently and skips
    // the complete update rather than splitting it at the boundary.
    for(std::size_t index=0;index<definition_.emitters.size();++index){
        const float freeze=definition_.emitters[index].freeze_time;
        if(!std::isfinite(freeze)||freeze<=0.0F)continue;
        if(emitters_[index].elapsed+delta_seconds>freeze)emitters_[index].frozen=true;
    }
}

CpuSystem::EmitterMotion CpuSystem::emitter_motion() const{
    EmitterMotion motion{origin_-previous_origin_,std::nullopt};
    if(same(basis_,previous_basis_))return motion;
    // The rotation that carries the previous emitter frame onto the current one: basis_ times
    // the inverse of previous_basis_ (its columns are the images of the unit axes).
    const Basis3& b=previous_basis_;
    const float determinant=dot(b.x,cross(b.y,b.z));
    if(!std::isfinite(determinant)||std::abs(determinant)<1.0e-20F)return motion;
    const float inverse=1.0F/determinant;
    // Rows of inverse(previous_basis_).
    const Vec3 row_x=cross(b.y,b.z)*inverse,row_y=cross(b.z,b.x)*inverse,row_z=cross(b.x,b.y)*inverse;
    const Basis3 rotation{transformed(basis_,{row_x.x,row_y.x,row_z.x}),
                          transformed(basis_,{row_x.y,row_y.y,row_z.y}),
                          transformed(basis_,{row_x.z,row_y.z,row_z.z})};
    if(finite(rotation))motion.rotation=rotation;
    return motion;
}

// BP-40: only the position of a linked (Emitter translater) particle
// is kept in the emitter's frame. Its velocity stays a world vector (set at birth by the emitter's
// transform, then accelerated in world space) and is rotated into the emitter's frame only to move
// the position, so the emitter's turn carries the position alone.
void CpuSystem::follow(Particle& particle,const EmitterMotion& motion) const{
    if(!motion.rotation){particle.position+=motion.translation;return;}
    particle.position=origin_+transformed(*motion.rotation,particle.position-previous_origin_);
}

void CpuSystem::follow_emitter(){
    const EmitterMotion motion=emitter_motion();
    for(Particle& particle:particles_){
        if(definition_.emitters[particle.emitter_index].translater_id!=26||frozen(particle.emitter_index))continue;
        follow(particle,motion);
    }
    previous_origin_=origin_;
    previous_basis_=basis_;
}

void CpuSystem::step_segment(const float delta_seconds,AdvanceStats& stats,const bool prewarming){
    const float before=time_;
    time_+=delta_seconds;const EmitterMotion motion=emitter_motion();
    // MD-07: sample the attached frame independently of render-only following.
    if(delta_seconds>0.0F){
        emitter_velocity_=(origin_-sampled_origin_)*(1.0F/delta_seconds);
        if(!finite(emitter_velocity_))emitter_velocity_={};
        const float sampled_speed=std::hypot(emitter_velocity_.x,emitter_velocity_.y,emitter_velocity_.z);
        if(std::isfinite(sampled_speed))inherited_speed_limit_=std::max(inherited_speed_limit_,sampled_speed);
        sampled_origin_=origin_;
    }
    auto& events=events_;
    events.clear();
    std::size_t write{};
    for(std::size_t read=0;read<particles_.size();++read){auto particle=particles_[read];const auto& emitter=definition_.emitters[particle.emitter_index];
        if(frozen(particle.emitter_index)){particles_[write++]=particle;continue;}
        if(time_>=particle.death_time||(detail_.local>0.7F&&emitter.killer_id==21&&particle.position.z<0)){
            emitters_[particle.emitter_index].free_slots.push_back(particle.draw_slot);
            --emitters_[particle.emitter_index].live;
            if(!children_[particle.emitter_index].empty())events.push_back({particle,true});
            ++stats.killed;continue;
        }
        // A linked particle first moves with its emitter to the current frame; the world velocity,
        // with an object-space acceleration rotated once by the current basis, moves it from there.
        update_particle(particle,emitter,delta_seconds);if(emitter.translater_id==26)follow(particle,motion);
        particle.motion_velocity=particle.velocity;
        if(emitter.inherit_emitter_motion&&emitter.parent_emitter==EmitterDefinition::no_parent){
            particle.motion_velocity+=emitter_velocity_*emitter.inherited_velocity_scale;
        }
        particle.inherited_speed_limit=inherited_speed_limit_;
        particle.position+=particle.motion_velocity*delta_seconds;particle.velocity+=particle.acceleration*delta_seconds;
        particles_[write++]=particle;
    }
    particles_.resize(write);
    // Stable IDs remain ascending through compaction and append. Binary lookup
    // avoids rebuilding allocating hash nodes for every particle each update.
    const auto find_parent=[&](const std::uint64_t id){
        return std::lower_bound(particles_.begin(),particles_.end(),id,
            [](const Particle& particle,const std::uint64_t wanted){return particle.id<wanted;});
    };
    for(auto& instance:child_instances_)if(instance.active){
        const auto parent=find_parent(instance.parent_id);
        if(parent==particles_.end()||parent->id!=instance.parent_id){
            instance.active=false;++stats.child_instances_detached;
        }
    }
    child_instances_.erase(std::remove_if(child_instances_.begin(),child_instances_.end(),
        [](const ChildInstance& instance){return !instance.active;}),child_instances_.end());
    process_events(events,stats);
    constexpr std::size_t max_spawn_events_per_advance=100000U;std::size_t events_count{};
    for(auto& instance:child_instances_){
        const auto parent=find_parent(instance.parent_id);
        if(parent==particles_.end()||parent->id!=instance.parent_id){
            instance.active=false;++stats.child_instances_detached;continue;
        }
        instance.parent_snapshot=*parent;
        const auto& emitter=definition_.emitters[instance.emitter_index];
        if(frozen(instance.emitter_index))continue;
        while(instance.active&&time_>=instance.next_spawn&&events_count++<max_spawn_events_per_advance){
            spawn_batch(instance.emitter_index,instance.next_spawn,&instance.parent_snapshot,stats,events);
            const float elapsed=instance.next_spawn-instance.start_time;
            if(emitter.stop_time>0&&elapsed>=emitter.stop_time){instance.active=false;break;}
            const float delay=emitter.bursting?emitter.spawn_interval:emitter.spawn_interval/emitter.particles_per_interval;
            const float previous=instance.next_spawn;instance.next_spawn+=delay;
            if(!(instance.next_spawn>previous)||!std::isfinite(instance.next_spawn)){instance.active=false;break;}
        }
        if(events_count>=max_spawn_events_per_advance)instance.active=false;
    }
    child_instances_.erase(std::remove_if(child_instances_.begin(),child_instances_.end(),
        [](const ChildInstance& instance){return !instance.active;}),child_instances_.end());
    process_events(events,stats);
    // A detached system schedules no further root emission; the emitter state
    // is kept as it was so no random draw or ID is consumed on its behalf.
    std::size_t root_events{};
    for(std::size_t index=0;!detached_&&index<emitters_.size();++index){auto& state=emitters_[index];const auto& emitter=definition_.emitters[index];
        if(emitter.creator_id!=34&&emitter.creator_id!=35)continue;
        if(frozen(index)||(prewarming&&before<emitter_start_[index]))continue;
        while(state.active&&time_>=state.next_spawn&&root_events++<max_spawn_events_per_advance){
            spawn_batch(index,state.next_spawn,nullptr,stats,events);
            const float elapsed=state.next_spawn-emitter_start_[index]-emitter.start_delay;if(emitter.stop_time>0&&elapsed>=emitter.stop_time){state.active=false;break;}
            const float delay=emitter.bursting?emitter.spawn_interval:emitter.spawn_interval/emitter.particles_per_interval;
            const float previous=state.next_spawn;state.next_spawn+=delay;
            if(!(delay>0)||!std::isfinite(delay)||!(state.next_spawn>previous)){state.active=false;break;}
        }
        if(root_events>=max_spawn_events_per_advance)state.active=false;
    }
    process_events(events,stats);
    if(!prewarming)for(auto& state:emitters_)if(!state.frozen)state.elapsed+=delta_seconds;
    previous_origin_=origin_;
    previous_basis_=basis_;
}

} // namespace eawr::presentation::particles
