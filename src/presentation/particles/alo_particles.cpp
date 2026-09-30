#include "eawr/presentation/particles/particles.hpp"

#include "eawr/core/diagnostic.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

// Format layout and V1-to-plugin conversion semantics are derived from the
// MIT-licensed alo-viewer revision 9bb0053919cc5df8377610d4f91b11d956d6c2f4,
// src/RenderEngine/Particles/ParticleSystem.* and the plugin sources.

namespace eawr::presentation::particles {
namespace {

using PF = PluginFamily;
using PS = Support;

constexpr std::array<PluginInfo, 63> k_plugins{{
    {0,"Point",PF::creator,PS::unsupported,"obsolete V2 creator is not present in the inventoried corpus"},
    {1,"Trail",PF::creator,PS::unsupported,"requires parent-particle attachment"},
    {2,"Sphere",PF::creator,PS::unsupported,"obsolete V2 creator is not present in the inventoried corpus"},
    {3,"Box",PF::creator,PS::unsupported,"obsolete V2 creator is not present in the inventoried corpus"},
    {4,"Torus",PF::creator,PS::unsupported,"obsolete V2 creator is not present in the inventoried corpus"},
    {5,"Mesh",PF::creator,PS::unsupported,"requires renderer mesh binding"},
    {6,"Terrain",PF::creator,PS::unsupported,"unsupported by the MIT reference and needs terrain sampling"},
    {7,"LinearSize",PF::modifier,PS::cpu,"portable presentation CPU implementation"},
    {8,"LinearColor",PF::modifier,PS::unsupported,"not present in the inventoried V1-converted corpus"},
    {9,"LinearRotation",PF::modifier,PS::unsupported,"not present in the inventoried V1-converted corpus"},
    {10,"Acceleration",PF::modifier,PS::cpu,"portable presentation CPU implementation"},
    {11,"AttractAcceleration",PF::modifier,PS::cpu,"portable presentation CPU implementation"},
    {12,"Turbulence",PF::modifier,PS::unsupported,"noise contract is not yet ported"},
    {13,"Vortex",PF::modifier,PS::unsupported,"not present in the inventoried V1-converted corpus"},
    {14,"ConstantUV",PF::modifier,PS::cpu,"portable presentation CPU implementation"},
    {15,"KeyedSize",PF::modifier,PS::cpu,"portable presentation CPU implementation"},
    {16,"KeyedRotation",PF::modifier,PS::unsupported,"not present in the inventoried V1-converted corpus"},
    {17,"KeyedColor",PF::modifier,PS::unsupported,"not present in the inventoried V1-converted corpus"},
    {18,"KeyedUV",PF::modifier,PS::cpu,"portable presentation CPU implementation"},
    {19,"Age",PF::killer,PS::cpu,"portable presentation CPU implementation"},
    {20,"Radius",PF::killer,PS::unsupported,"not present in the inventoried V1-converted corpus"},
    {21,"Terrain",PF::killer,PS::cpu,"legacy reference behavior uses presentation ground plane z=0"},
    {22,"Billboard",PF::renderer,PS::metadata_only,"renderer adapter is deferred to the P1 RenderingServer work"},
    {23,"Line",PF::renderer,PS::metadata_only,"renderer adapter is deferred to the P1 RenderingServer work"},
    {24,"Chain",PF::renderer,PS::metadata_only,"renderer adapter is deferred to the P1 RenderingServer work"},
    {25,"Volumetric",PF::renderer,PS::metadata_only,"renderer adapter is deferred to the P1 RenderingServer work"},
    {26,"Emitter",PF::translater,PS::cpu,"portable translation-delta implementation"},
    {27,"World",PF::translater,PS::cpu,"portable no-op implementation"},
    {28,"XYAligned",PF::renderer,PS::metadata_only,"renderer adapter is deferred to the P1 RenderingServer work"},
    {29,"VelocityAligned",PF::renderer,PS::metadata_only,"renderer adapter is deferred to the P1 RenderingServer work"},
    {30,"RandomUV",PF::modifier,PS::unsupported,"not present in the inventoried V1-converted corpus"},
    {31,"KeyedAcceleration",PF::modifier,PS::unsupported,"not present in the inventoried V1-converted corpus"},
    {32,"KeyedFriction",PF::modifier,PS::unsupported,"MIT reference has no behavior implementation"},
    {33,"SlottedRandomUV",PF::modifier,PS::unsupported,"not present in the inventoried V1-converted corpus"},
    {34,"Shape",PF::creator,PS::cpu,"portable point/range/sphere/cylinder/torus sampling and bounded spawning"},
    {35,"EnhancedMesh",PF::creator,PS::cpu,"V1 CPU emission requires an owned mesh binding"},
    {36,"StretchedTextureChain",PF::renderer,PS::metadata_only,"renderer adapter is deferred to the P1 RenderingServer work"},
    {37,"BumpMap",PF::renderer,PS::metadata_only,"renderer adapter is deferred to the P1 RenderingServer work"},
    {38,"HeatSaturation",PF::renderer,PS::metadata_only,"renderer adapter is deferred to the P1 RenderingServer work"},
    {39,"EnhancedTrail",PF::creator,PS::cpu,"V1 parent birth attachment"},
    {40,"Death",PF::creator,PS::cpu,"V1 parent death burst"},
    {41,"Attractor",PF::modifier,PS::unsupported,"target/bone attachment is deferred"},
    {42,"SpeedLimit",PF::modifier,PS::unsupported,"not present in the inventoried V1-converted corpus"},
    {43,"AlignedShape",PF::creator,PS::unsupported,"bone alignment is deferred"},
    {44,"Target",PF::killer,PS::unsupported,"target/bone attachment is deferred"},
    {45,"DistanceKeyTime",PF::modifier,PS::unsupported,"MIT reference has no behavior implementation"},
    {46,"LinearSpeed",PF::modifier,PS::unsupported,"not present in the inventoried V1-converted corpus"},
    {47,"XYAlignedChain",PF::renderer,PS::metadata_only,"renderer adapter is deferred to the P1 RenderingServer work"},
    {48,"LinearRotationRate",PF::modifier,PS::unsupported,"not present in the inventoried V1-converted corpus"},
    {49,"KeyedRotationRate",PF::modifier,PS::cpu,"portable presentation CPU implementation"},
    {50,"OutwardVelocityShape",PF::creator,PS::unsupported,"MIT reference only partially implements this family"},
    {51,"TerrainBounce",PF::modifier,PS::unsupported,"MIT reference has no behavior implementation"},
    {52,"Kites",PF::renderer,PS::metadata_only,"renderer adapter is deferred to the P1 RenderingServer work"},
    {53,"ColorVariance",PF::modifier,PS::cpu,"portable fixed-seed presentation CPU implementation"},
    {54,"WindAcceleration",PF::modifier,PS::cpu,"portable injected-wind presentation CPU implementation"},
    {55,"Torque",PF::modifier,PS::unsupported,"MIT reference has no behavior implementation"},
    {56,"AxisAttractor",PF::modifier,PS::unsupported,"MIT reference has no behavior implementation"},
    {57,"ConstantSize",PF::modifier,PS::cpu,"portable presentation CPU implementation"},
    {58,"ConstantRotation",PF::modifier,PS::cpu,"portable fixed-seed presentation CPU implementation"},
    {59,"ConstantColor",PF::modifier,PS::unsupported,"not present in the inventoried V1-converted corpus"},
    {60,"HardwareSpawner",PF::creator,PS::unsupported,"hardware/GPU family is outside the CPU slice"},
    {61,"HardwareBillboards",PF::renderer,PS::metadata_only,"hardware/GPU family is outside the CPU slice"},
    {62,"HardwareStomper",PF::killer,PS::unsupported,"hardware/GPU family is outside the CPU slice"},
}};

struct Chunk final {
    std::uint32_t type{};
    bool group{};
    std::size_t offset{};
    std::span<const std::byte> payload;
    std::vector<Chunk> children;
};
struct Mini final { std::uint8_t type{}; std::size_t offset{}; std::span<const std::byte> payload; };

std::uint32_t u32(const std::span<const std::byte> data, const std::size_t offset = 0) {
    std::uint32_t value{};
    std::memcpy(&value, data.data() + offset, sizeof(value));
    if constexpr (std::endian::native == std::endian::big) {
        value=((value&0x000000ffU)<<24U)|((value&0x0000ff00U)<<8U)|
              ((value&0x00ff0000U)>>8U)|((value&0xff000000U)>>24U);
    }
    return value;
}
float f32(const std::span<const std::byte> data, const std::size_t offset = 0) {
    return std::bit_cast<float>(u32(data, offset));
}

core::Diagnostic error(const std::string& path, const std::string_view code,
                       std::string message, const std::size_t offset = 0) {
    return {.code=std::string(code), .severity=core::Severity::error, .message=std::move(message),
            .logical_path=path.empty()?std::nullopt:std::optional<std::string>(path),
            .line=std::nullopt, .column=offset+1U, .source_id=std::nullopt};
}

core::Result<std::vector<Chunk>> parse_chunks(const std::span<const std::byte> bytes,
                                              const std::string& path,
                                              const std::size_t base = 0,
                                              const std::size_t depth = 0) {
    if (depth > 64) return core::Result<std::vector<Chunk>>::failure(
        error(path, diagnostic_codes::limit, "particle chunk nesting exceeds 64", base));
    std::vector<Chunk> result;
    std::size_t position{};
    while (position < bytes.size()) {
        if (bytes.size() - position < 8) return core::Result<std::vector<Chunk>>::failure(
            error(path, diagnostic_codes::truncated, "truncated particle chunk header", base+position));
        const auto type=u32(bytes,position), word=u32(bytes,position+4);
        const auto size=static_cast<std::size_t>(word & 0x7fffffffU);
        if (size > bytes.size()-position-8) return core::Result<std::vector<Chunk>>::failure(
            error(path, diagnostic_codes::structure, "particle chunk crosses enclosing bound", base+position));
        const bool group=(word&0x80000000U)!=0;
        const auto payload=bytes.subspan(position+8,size);
        Chunk chunk{type,group,base+position,payload,{}};
        if (group) {
            auto children=parse_chunks(payload,path,base+position+8,depth+1);
            if (!children) return core::Result<std::vector<Chunk>>::failure(children.error());
            chunk.children=std::move(children.value());
        }
        result.push_back(std::move(chunk));
        if (result.size()>65536) return core::Result<std::vector<Chunk>>::failure(
            error(path, diagnostic_codes::limit, "particle chunk count exceeds safety limit", base+position));
        position+=8+size;
    }
    return core::Result<std::vector<Chunk>>::success(std::move(result));
}

core::Result<std::vector<Mini>> parse_minis(const Chunk& chunk, const std::string& path) {
    std::vector<Mini> result;
    std::size_t position{};
    while (position<chunk.payload.size()) {
        if (chunk.payload.size()-position<2) return core::Result<std::vector<Mini>>::failure(
            error(path,diagnostic_codes::truncated,"truncated particle mini-chunk",chunk.offset+8+position));
        const auto type=std::to_integer<std::uint8_t>(chunk.payload[position]);
        const auto size=std::to_integer<std::uint8_t>(chunk.payload[position+1]);
        if (size>chunk.payload.size()-position-2) return core::Result<std::vector<Mini>>::failure(
            error(path,diagnostic_codes::structure,"particle mini-chunk crosses enclosing bound",chunk.offset+8+position));
        result.push_back({type,chunk.offset+8+position,chunk.payload.subspan(position+2,size)});
        position+=2+size;
    }
    return core::Result<std::vector<Mini>>::success(std::move(result));
}

const Mini* find_mini(const std::vector<Mini>& minis, const std::uint8_t type) {
    const auto it=std::find_if(minis.begin(),minis.end(),[&](const Mini& item){return item.type==type;});
    return it==minis.end()?nullptr:&*it;
}
bool read_bool(const std::vector<Mini>& minis,const std::uint8_t type,const bool fallback=false) {
    const auto* item=find_mini(minis,type);return item&&item->payload.size()==1?
        std::to_integer<std::uint8_t>(item->payload[0])!=0:fallback;
}
std::uint32_t read_u32(const std::vector<Mini>& minis,const std::uint8_t type,const std::uint32_t fallback=0) {
    const auto* item=find_mini(minis,type);return item&&item->payload.size()==4?u32(item->payload):fallback;
}
float read_float(const std::vector<Mini>& minis,const std::uint8_t type,const float fallback=0.0F) {
    const auto* item=find_mini(minis,type);return item?f32(item->payload):fallback;
}
Vec3 read_vec3(const std::vector<Mini>& minis,const std::uint8_t type,const Vec3 fallback={}) {
    const auto* item=find_mini(minis,type);return item?
        Vec3{f32(item->payload,0),f32(item->payload,4),f32(item->payload,8)}:fallback;
}
Vec4 read_vec4(const std::vector<Mini>& minis,const std::uint8_t type,const Vec4 fallback={}) {
    const auto* item=find_mini(minis,type);return item?
        Vec4{f32(item->payload,0),f32(item->payload,4),f32(item->payload,8),f32(item->payload,12)}:fallback;
}
std::string read_string(const Chunk& chunk) {
    const auto end=std::find(chunk.payload.begin(),chunk.payload.end(),std::byte{0});
    return {reinterpret_cast<const char*>(chunk.payload.data()),static_cast<std::size_t>(end-chunk.payload.begin())};
}

core::Result<void> validate_float_property(const std::vector<Mini>& minis,const std::uint8_t type,
                                           const std::size_t components,const std::string& path) {
    for(const auto& item:minis){
        if(item.type!=type)continue;
        const auto expected=components*sizeof(float);
        if(item.payload.size()!=expected)return core::Result<void>::failure(error(
            path,diagnostic_codes::structure,"legacy numeric property "+std::to_string(type)+
            " has payload length "+std::to_string(item.payload.size())+", expected "+
            std::to_string(expected),item.offset));
        for(std::size_t component=0;component<components;++component)if(!std::isfinite(f32(item.payload,component*sizeof(float))))
            return core::Result<void>::failure(error(path,diagnostic_codes::invalid_value,
                "legacy numeric property "+std::to_string(type)+" contains a non-finite float",item.offset));
    }
    return core::Result<void>::success();
}

core::Result<void> validate_v1_properties(const std::vector<Mini>& minis,const std::string& path) {
    constexpr std::array<std::uint8_t,13> scalar_types{{0x25,0x24,0x09,0x0c,0x0b,0x0f,0x13,0x30,0x12,0x17,0x42,0x28,0x3c}};
    for(const auto type:scalar_types){auto checked=validate_float_property(minis,type,1,path);if(!checked)return checked;}
    auto checked=validate_float_property(minis,0x0a,3,path);if(!checked)return checked;
    return validate_float_property(minis,0x2c,4,path);
}

core::Result<PropertyGroup> parse_old_group(const Chunk& outer,const std::string& path) {
    PropertyGroup result;
    if (!outer.group || outer.children.size()!=1 || outer.children[0].payload.size()<64)
        return core::Result<PropertyGroup>::failure(error(path,diagnostic_codes::structure,
            "legacy property group is incomplete",outer.offset));
    const auto data=outer.children[0].payload;
    constexpr std::array<std::size_t,13> float_offsets{{4,8,12,16,20,24,28,32,40,48,52,56,60}};
    for(const auto offset:float_offsets)if(!std::isfinite(f32(data,offset)))
        return core::Result<PropertyGroup>::failure(error(path,diagnostic_codes::invalid_value,
            "legacy property group contains a non-finite float",outer.children[0].offset+8+offset));
    const auto type=u32(data);
    const Vec3 minimum{f32(data,4),f32(data,8),f32(data,12)};
    const Vec3 maximum{f32(data,16),f32(data,20),f32(data,24)};
    const float side=f32(data,28), sphere=f32(data,32), cylinder=f32(data,40), height=f32(data,48);
    const Vec3 point{f32(data,52),f32(data,56),f32(data,60)};
    switch(type) {
    case 0: result.shape=Shape::point;result.point=point;break;
    case 1: result.shape=Shape::range;result.range_min=minimum;result.range_max=maximum;break;
    case 2: result.shape=Shape::range;result.range_min={-side/2,-side/2,-side/2};result.range_max={side/2,side/2,side/2};break;
    case 3: result.shape=Shape::sphere;result.radius_max=sphere;break;
    case 4: result.shape=Shape::cylinder;result.cylinder_radius=cylinder;result.cylinder_height_max=height;break;
    default: break;
    }
    return core::Result<PropertyGroup>::success(result);
}

bool old_group_hollow(const Chunk& outer) {
    if (!outer.group || outer.children.size()!=1 || outer.children[0].payload.size()<64) return false;
    const auto data=outer.children[0].payload;const auto type=u32(data);
    return (type==3&&u32(data,36)!=0)||(type==4&&u32(data,44)!=0);
}

Interpolation old_interpolation(const std::uint32_t value) {
    return value==1?Interpolation::smooth:value==2?Interpolation::step:Interpolation::linear;
}

core::Result<std::vector<ScalarTrack>> parse_old_tracks(const Chunk& tracks,const std::string& path) {
    std::vector<ScalarTrack> result;
    if (!tracks.group || tracks.children.size()!=14) return core::Result<std::vector<ScalarTrack>>::failure(
        error(path,diagnostic_codes::structure,"legacy emitter must contain seven track pairs",tracks.offset));
    for (std::size_t index=0;index<7;++index) {
        const auto& header=tracks.children[index*2];const auto& keys=tracks.children[index*2+1];
        auto hm=parse_minis(header,path);if(!hm)return core::Result<std::vector<ScalarTrack>>::failure(hm.error());
        auto km=parse_minis(keys,path);if(!km)return core::Result<std::vector<ScalarTrack>>::failure(km.error());
        const auto* first=find_mini(hm.value(),2);const auto* last=find_mini(hm.value(),3);
        const auto* interpolation=find_mini(hm.value(),4);
        const std::size_t endpoint_size=index<4?1U:sizeof(float);
        if(!first||!last||!interpolation||first->payload.size()!=endpoint_size||
           last->payload.size()!=endpoint_size||interpolation->payload.size()!=4) return core::Result<std::vector<ScalarTrack>>::failure(
            error(path,diagnostic_codes::structure,"legacy track header is incomplete",header.offset));
        auto value=[&](const Mini& item){return index<4?
            static_cast<float>(std::to_integer<std::uint8_t>(item.payload[0]))/255.0F:f32(item.payload);};
        const float first_value=value(*first),last_value=value(*last);
        if(!std::isfinite(first_value)||!std::isfinite(last_value))return core::Result<std::vector<ScalarTrack>>::failure(
            error(path,diagnostic_codes::invalid_value,"legacy track endpoint contains a non-finite float",header.offset));
        ScalarTrack track;track.interpolation=old_interpolation(u32(interpolation->payload));
        track.keys.push_back({0.0F,first_value});
        for(const auto& item:km.value()) if(item.type==5) {
            if(item.payload.size()!=8)return core::Result<std::vector<ScalarTrack>>::failure(
                error(path,diagnostic_codes::structure,"legacy track key payload must contain two 32-bit values",item.offset));
            const float key_value=index<4?static_cast<float>(u32(item.payload))/255.0F:f32(item.payload);
            const float key_time=f32(item.payload,4);
            if(!std::isfinite(key_value)||!std::isfinite(key_time))return core::Result<std::vector<ScalarTrack>>::failure(
                error(path,diagnostic_codes::invalid_value,"legacy track key contains a non-finite float",item.offset));
            track.keys.push_back({key_time,key_value});
        }
        track.keys.push_back({1.0F,last_value});
        result.push_back(std::move(track));
    }
    return core::Result<std::vector<ScalarTrack>>::success(std::move(result));
}

EmitterDefinition default_emitter() {
    EmitterDefinition emitter;
    auto constant=[](const float value){return ScalarTrack{Interpolation::linear,{{0,value},{1,value}}};};
    emitter.red=emitter.green=emitter.blue=emitter.alpha=constant(1.0F);
    emitter.size=constant(20.0F);emitter.uv_index=constant(0.0F);emitter.rotation_rate=constant(0.0F);
    return emitter;
}

bool finite(const Vec3 value){return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.z);}
bool finite(const Vec4 value){return finite(Vec3{value.x,value.y,value.z})&&std::isfinite(value.w);}
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
    std::isfinite(emitter.spawn_interval)&&std::isfinite(emitter.start_delay)&&std::isfinite(emitter.stop_time)&&
    std::isfinite(emitter.skip_time)&&std::isfinite(emitter.freeze_time)&&
    std::isfinite(emitter.inherited_velocity_scale)&&std::isfinite(emitter.max_inherited_velocity)&&
    finite(emitter.position)&&finite(emitter.velocity)&&std::isfinite(emitter.lifetime)&&
    std::isfinite(emitter.lifetime_variation)&&std::isfinite(emitter.inward_speed)&&finite(emitter.acceleration)&&
    std::isfinite(emitter.inward_acceleration)&&std::isfinite(emitter.wind_response)&&
    std::isfinite(emitter.terrain_elasticity)&&finite(emitter.red)&&finite(emitter.green)&&finite(emitter.blue)&&
    finite(emitter.alpha)&&finite(emitter.size)&&std::isfinite(emitter.size_variation)&&finite(emitter.uv_index)&&
    finite(emitter.rotation_rate)&&std::isfinite(emitter.random_rotation_average)&&
    std::isfinite(emitter.random_rotation_variation)&&finite(emitter.color_variance)&&std::isfinite(emitter.tail_size);}

core::Result<SystemDefinition> parse_v1(const Chunk& root,const std::string& path) {
    SystemDefinition system;system.version=AloParticleVersion::legacy_v1;
    const Chunk* emitters=nullptr;
    for(const auto& child:root.children){if(child.type==0x800)emitters=&child;if(child.type==2&&child.payload.size()==1)system.leave_particles=std::to_integer<std::uint8_t>(child.payload[0])!=0;}
    if(!emitters||!emitters->group)return core::Result<SystemDefinition>::failure(error(path,diagnostic_codes::structure,"legacy particle system lacks emitter group",root.offset));
    struct Pending final {std::uint32_t death=std::numeric_limits<std::uint32_t>::max();std::uint32_t life=std::numeric_limits<std::uint32_t>::max();};
    std::vector<Pending> dependencies;
    for(const auto& node:emitters->children){
        if(node.type!=0x700||!node.group)continue;
        if(system.emitters.size()>=65536)return core::Result<SystemDefinition>::failure(error(path,diagnostic_codes::limit,"emitter count exceeds safety limit",node.offset));
        auto emitter=default_emitter();Pending pending;
        std::vector<Mini> props;std::vector<PropertyGroup> groups;std::vector<bool> group_hollow;std::vector<ScalarTrack> tracks;
        for(const auto& child:node.children){
            if(child.type==2){auto parsed=parse_minis(child,path);if(!parsed)return core::Result<SystemDefinition>::failure(parsed.error());props=std::move(parsed.value());}
            else if(child.type==0x16)emitter.name=read_string(child);
            else if(child.type==0x03)emitter.color_texture=read_string(child);
            else if(child.type==0x45)emitter.normal_texture=read_string(child);
            else if(child.type==0x29&&child.group){for(const auto& group:child.children){auto parsed=parse_old_group(group,path);if(!parsed)return core::Result<SystemDefinition>::failure(parsed.error());groups.push_back(std::move(parsed.value()));group_hollow.push_back(old_group_hollow(group));}}
            else if(child.type==1){auto parsed=parse_old_tracks(child,path);if(!parsed)return core::Result<SystemDefinition>::failure(parsed.error());tracks=std::move(parsed.value());}
            else if(child.type==0x36){auto parsed=parse_minis(child,path);if(!parsed)return core::Result<SystemDefinition>::failure(parsed.error());pending.death=read_u32(parsed.value(),0x37,pending.death);pending.life=read_u32(parsed.value(),0x39,pending.life);}
        }
        auto valid_properties=validate_v1_properties(props,path);if(!valid_properties)return core::Result<SystemDefinition>::failure(valid_properties.error());
        if(groups.size()!=3||tracks.size()!=7)return core::Result<SystemDefinition>::failure(error(path,diagnostic_codes::structure,"legacy emitter is missing groups or tracks",node.offset));
        emitter.velocity=groups[0];emitter.position=groups[2];emitter.hollow_velocity=group_hollow[0];emitter.hollow_position=group_hollow[2];
        emitter.inherited_velocity_scale=read_bool(props,0x43)?read_float(props,0x28):0.0F;
        emitter.inherit_parent_velocity=emitter.inherited_velocity_scale!=0.0F;
        emitter.bursting=read_bool(props,0x07);emitter.particles_per_interval=static_cast<float>(emitter.bursting?read_u32(props,0x26,1):read_u32(props,0x2a,1));
        emitter.spawn_interval=emitter.bursting?read_float(props,0x25,1.0F):1.0F;
        emitter.start_delay=read_float(props,0x24);auto bursts=read_u32(props,0x27);if(bursts==std::numeric_limits<std::uint32_t>::max())bursts=0;emitter.stop_time=emitter.bursting?static_cast<float>(bursts)*emitter.spawn_interval:0.0F;
        emitter.translater_id=read_bool(props,0x08)?26U:27U;
        {   // Skip (pre-simulate) and freeze seconds; a malformed or non-positive value disables them.
            const auto seconds=[&](const std::uint8_t type){
                const auto* item=find_mini(props,type);
                if(!item||item->payload.size()!=4)return 0.0F;
                const float value=f32(item->payload);
                return std::isfinite(value)&&value>0.0F?value:0.0F;
            };
            emitter.skip_time=seconds(0x33);emitter.freeze_time=seconds(0x32);
        }
        emitter.inward_speed=read_float(props,0x09);emitter.acceleration=read_vec3(props,0x0a);emitter.acceleration.z-=read_float(props,0x0c);emitter.acceleration_local=read_bool(props,0x35);
        emitter.inward_acceleration=-read_float(props,0x0b);emitter.wind_response=read_bool(props,0x31)?0.01F:0.0F;
        emitter.lifetime=read_float(props,0x0f,1.0F);emitter.lifetime_variation=read_float(props,0x13);
        const auto ground=read_u32(props,0x2f);emitter.killer_id=ground==1?21U:19U;emitter.terrain_elasticity=ground==2?read_float(props,0x30,0.2F):ground==3?0.0F:0.0F;
        emitter.blend_mode=read_u32(props,0x04,1);emitter.disable_depth_test=read_bool(props,0x46);emitter.world_oriented=read_bool(props,0x2e);emitter.tail_size=read_float(props,0x42,50.0F);
        if(read_bool(props,0x3b))emitter.renderer_id=38;else if(read_bool(props,0x2e))emitter.renderer_id=28;else if(read_bool(props,0x41))emitter.renderer_id=52;else emitter.renderer_id=22;
        emitter.red=tracks[0];emitter.green=tracks[1];emitter.blue=tracks[2];emitter.alpha=tracks[3];emitter.size=tracks[4];emitter.uv_index=tracks[5];emitter.rotation_rate=tracks[6];
        // Retail V1 size keys are the full quad width and the size variation is
        // the raw fractional spread v: each particle draws a factor 1 + U(-v, v)
        // (docs/rendering.md#v1-particle-size). Half the key is the half-extent.
        emitter.texture_size=read_u32(props,0x10,64);emitter.size_variation=read_float(props,0x12);
        for(auto& key:emitter.size.keys)key.value=key.value/2.0F;
        emitter.color_variance=read_vec4(props,0x2c);emitter.grayscale_variance=read_bool(props,0x2d);
        emitter.random_rotation=read_bool(props,0x48);emitter.random_rotation_direction=read_bool(props,0x23);emitter.random_rotation_variation=std::fabs(read_float(props,0x17));
        if(emitter.random_rotation){emitter.random_rotation_average=emitter.rotation_rate.keys.front().value;if(emitter.random_rotation_average>0){emitter.random_rotation_variation/=emitter.random_rotation_average;emitter.random_rotation_average-=std::trunc(emitter.random_rotation_average);}else emitter.random_rotation_variation=0;}
        if(!finite(emitter))return core::Result<SystemDefinition>::failure(error(path,diagnostic_codes::invalid_value,
            "legacy emitter numeric conversion produced a non-finite value",node.offset));
        emitter.modifier_ids.clear();if(emitter.acceleration.x!=0||emitter.acceleration.y!=0||emitter.acceleration.z!=0)emitter.modifier_ids.push_back(10);if(emitter.inward_acceleration!=0)emitter.modifier_ids.push_back(11);if(emitter.wind_response!=0)emitter.modifier_ids.push_back(54);if(ground==2||ground==3){emitter.modifier_ids.push_back(51);emitter.cpu_ready=false;emitter.unsupported_reason="terrain bounce/stick has no behavior in the MIT reference";}
        emitter.modifier_ids.push_back(emitter.size.keys.size()==2&&emitter.size.keys[0].value==emitter.size.keys[1].value?57:emitter.size.keys.size()==2?7:15);
        emitter.modifier_ids.push_back(emitter.uv_index.keys.size()==2&&emitter.uv_index.keys[0].value==emitter.uv_index.keys[1].value?14:18);
        emitter.modifier_ids.push_back(emitter.random_rotation?58:49);
        if(emitter.color_variance.x!=0||emitter.color_variance.y!=0||emitter.color_variance.z!=0||emitter.color_variance.w!=0)emitter.modifier_ids.push_back(53);
        if(const auto* mesh_mode=find_mini(props,0x34)){
            if(mesh_mode->payload.size()!=4)return core::Result<SystemDefinition>::failure(error(path,
                diagnostic_codes::structure,"legacy mesh mode must contain one 32-bit value",mesh_mode->offset));
        }
        emitter.mesh_mode_raw=read_u32(props,0x34);
        emitter.mesh_surface_offset=read_float(props,0x3c,0.5F);
        if(emitter.mesh_mode_raw!=0){
            emitter.creator_id=35;
            emitter.mesh_mode=emitter.mesh_mode_raw==2?MeshSpawnMode::random_surface:
                emitter.mesh_mode_raw==3?MeshSpawnMode::every_vertex:MeshSpawnMode::random_vertex;
        }
        system.emitters.push_back(std::move(emitter));dependencies.push_back(pending);
    }
    for(std::size_t parent=0;parent<dependencies.size();++parent){
        const auto set=[&](const std::uint32_t child,const std::uint32_t id)->bool{
            if(child==EmitterDefinition::no_parent)return true;
            if(child>=system.emitters.size())return false;
            auto& target=system.emitters[child];
            if(target.parent_emitter!=EmitterDefinition::no_parent)return false;
            target.parent_emitter=static_cast<std::uint32_t>(parent);
            target.spawn_on_parent_death=id==40;
            if(target.creator_id!=35){
                target.creator_id=id;
                if(id==40){target.inherit_parent_velocity=false;target.bursting=true;target.spawn_interval=1.0F;
                    target.start_delay=0.0F;target.stop_time=0.5F;}
            }
            return true;
        };
        if(!set(dependencies[parent].life,39)||!set(dependencies[parent].death,40))
            return core::Result<SystemDefinition>::failure(error(path,diagnostic_codes::structure,"invalid or duplicate parent emitter link",root.offset));
    }
    for(std::size_t child=0;child<system.emitters.size();++child){
        std::size_t node=child;
        for(std::size_t hops=0;hops<=system.emitters.size();++hops){
            const auto parent=system.emitters[node].parent_emitter;
            if(parent==EmitterDefinition::no_parent)break;
            if(parent>=system.emitters.size()||hops==system.emitters.size())
                return core::Result<SystemDefinition>::failure(error(path,diagnostic_codes::structure,"cyclic or invalid parent emitter link",root.offset));
            node=parent;
        }
    }
    return core::Result<SystemDefinition>::success(std::move(system));
}

std::optional<std::uint32_t> nested_id(const Chunk& wrapper) {
    if(!wrapper.group||wrapper.children.empty())return std::nullopt;
    const auto& id=wrapper.children.front();if(id.type!=0||id.group||id.payload.size()!=4)return std::nullopt;
    return u32(id.payload);
}

core::Result<SystemDefinition> parse_v2(const Chunk& root,const std::string& path) {
    SystemDefinition system;system.version=AloParticleVersion::plugin_v2;
    const Chunk* group=nullptr;for(const auto& child:root.children)if(child.type==0x1520)group=&child;
    if(!group||!group->group)return core::Result<SystemDefinition>::failure(error(path,diagnostic_codes::structure,"V2 particle system lacks emitter group",root.offset));
    for(const auto& node:group->children){if(node.type!=0x1540||!node.group)continue;EmitterDefinition emitter=default_emitter();emitter.cpu_ready=false;emitter.unsupported_reason="V2 parameter decoding is not in the corpus-backed CPU slice";
        for(const auto& child:node.children){if(child.type==0)emitter.name=read_string(child);else if(const auto id=nested_id(child)){if(child.type==1)emitter.creator_id=*id;else if(child.type==2)emitter.translater_id=*id;else if(child.type==3)emitter.killer_id=*id;else if(child.type==4)emitter.renderer_id=*id;else if(child.type==5)emitter.modifier_ids.push_back(*id);}}
        system.emitters.push_back(std::move(emitter));}
    return core::Result<SystemDefinition>::success(std::move(system));
}

} // namespace

std::span<const PluginInfo> plugin_catalog(){return k_plugins;}
const PluginInfo* find_plugin(const std::uint32_t id){return id<k_plugins.size()?&k_plugins[id]:nullptr;}

core::Result<SystemDefinition> load_alo(const std::span<const std::byte> bytes,std::string logical_path) {
    if(bytes.size()>512U*1024U*1024U)return core::Result<SystemDefinition>::failure(error(logical_path,diagnostic_codes::limit,"particle ALO exceeds 512 MiB safety limit"));
    auto roots=parse_chunks(bytes,logical_path);if(!roots)return core::Result<SystemDefinition>::failure(roots.error());
    if(roots.value().size()!=1||!roots.value()[0].group)return core::Result<SystemDefinition>::failure(error(logical_path,diagnostic_codes::structure,"particle ALO must contain one grouped root"));
    if(roots.value()[0].type==0x900)return parse_v1(roots.value()[0],logical_path);
    if(roots.value()[0].type==0x1500)return parse_v2(roots.value()[0],logical_path);
    return core::Result<SystemDefinition>::failure(error(logical_path,diagnostic_codes::unsupported,"ALO root is not a particle system"));
}

} // namespace eawr::presentation::particles
