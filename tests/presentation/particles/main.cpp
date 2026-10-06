#include "eawr/presentation/particles/particles.hpp"
#include "eawr/presentation/particles/prewarmed_capacity.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace particles = eawr::presentation::particles;

namespace {

void expect(const bool condition,const char* message){if(!condition){std::cerr<<"FAIL: "<<message<<'\n';std::exit(1);}}
bool close(const float a,const float b,const float epsilon=0.0001F){return std::fabs(a-b)<=epsilon;}

using Bytes=std::vector<std::byte>;
void u32(Bytes& out,const std::uint32_t value){for(unsigned shift=0;shift<32;shift+=8)out.push_back(static_cast<std::byte>((value>>shift)&0xffU));}
void f32(Bytes& out,const float value){u32(out,std::bit_cast<std::uint32_t>(value));}
void text(Bytes& out,const std::string& value){for(const char ch:value)out.push_back(static_cast<std::byte>(ch));out.push_back(std::byte{});}
Bytes chunk(const std::uint32_t type,Bytes payload,const bool group=false){Bytes out;u32(out,type);u32(out,static_cast<std::uint32_t>(payload.size())|(group?0x80000000U:0));out.insert(out.end(),payload.begin(),payload.end());return out;}
void append(Bytes& out,const Bytes& value){out.insert(out.end(),value.begin(),value.end());}
Bytes mini(const std::uint8_t type,Bytes payload){Bytes out{static_cast<std::byte>(type),static_cast<std::byte>(payload.size())};append(out,payload);return out;}
Bytes integer(const std::uint32_t value){Bytes out;u32(out,value);return out;}
Bytes scalar(const float value){Bytes out;f32(out,value);return out;}
Bytes vector3(const float x,const float y,const float z){Bytes out;f32(out,x);f32(out,y);f32(out,z);return out;}

Bytes old_group(const std::uint32_t type,const float x,const float y,const float z,
                const float minimum=0,const float maximum=0){
    Bytes data;u32(data,type);for(int i=0;i<12;++i)f32(data,i==0?minimum:i==3?maximum:0); // through cylinder height
    f32(data,x);f32(data,y);f32(data,z);
    return chunk(0x1100,chunk(0x1101,std::move(data)),true);
}
Bytes track_header(const float first,const float last,const bool color=false){
    Bytes data;if(color){append(data,mini(2,Bytes{static_cast<std::byte>(first*255)}));append(data,mini(3,Bytes{static_cast<std::byte>(last*255)}));}
    else{append(data,mini(2,scalar(first)));append(data,mini(3,scalar(last)));}
    append(data,mini(4,integer(0)));return chunk(0,std::move(data));
}
Bytes scalar_track_header(Bytes first,Bytes last){
    Bytes data;append(data,mini(2,std::move(first)));append(data,mini(3,std::move(last)));
    append(data,mini(4,integer(0)));return chunk(0,std::move(data));
}

Bytes legacy_fixture(Bytes lifetime=scalar(2.0F),Bytes size_first=scalar(4.0F),Bytes acceleration=vector3(1,0,0),Bytes mesh_props={},Bytes age_group={},Bytes velocity_group={}){
    Bytes properties;append(properties,mini(0x0f,std::move(lifetime)));append(properties,mini(0x13,scalar(0.25F)));append(properties,mini(0x12,scalar(0.2F)));append(properties,mini(0x2a,integer(2)));append(properties,mini(0x10,integer(16)));
    append(properties,mini(0x0a,std::move(acceleration)));append(properties,mini(0x31,Bytes{std::byte{1}}));
    append(properties,mesh_props);
    Bytes groups;append(groups,velocity_group.empty()?old_group(0,0,0,0):velocity_group);append(groups,age_group.empty()?old_group(0,2,0,0):age_group);append(groups,old_group(0,2,0,0));
    Bytes tracks;for(int i=0;i<4;++i){append(tracks,track_header(1,1,true));append(tracks,chunk(1,{}));}
    append(tracks,scalar_track_header(std::move(size_first),scalar(8)));append(tracks,chunk(1,{}));append(tracks,track_header(0,3));append(tracks,chunk(1,{}));append(tracks,track_header(1,1));append(tracks,chunk(1,{}));
    Bytes emitter;append(emitter,chunk(2,std::move(properties)));Bytes name;text(name,"fixture");append(emitter,chunk(0x16,std::move(name)));append(emitter,chunk(0x29,std::move(groups),true));append(emitter,chunk(1,std::move(tracks),true));
    Bytes emitters;append(emitters,chunk(0x700,std::move(emitter),true));Bytes root;Bytes system_name;text(system_name,"test");append(root,chunk(0,std::move(system_name)));append(root,chunk(1,integer(1)));append(root,chunk(0x800,std::move(emitters),true));append(root,chunk(2,Bytes{std::byte{1}}));return chunk(0x900,std::move(root),true);
}

particles::ScalarTrack constant(const float value){return {particles::Interpolation::linear,{{0,value},{1,value}}};}
particles::EmitterDefinition base_emitter(){
    particles::EmitterDefinition emitter;emitter.name="synthetic";emitter.particles_per_interval=1;emitter.spawn_interval=100;emitter.stop_time=0.001F;emitter.lifetime=10;
    emitter.position.shape=particles::Shape::point;emitter.velocity.shape=particles::Shape::point;
    emitter.red=emitter.green=emitter.blue=emitter.alpha=constant(1);emitter.size=constant(1);emitter.uv_index=constant(0);emitter.rotation_rate=constant(0);return emitter;
}
particles::CpuSystem system_with(particles::EmitterDefinition emitter,const std::uint32_t seed=7,const std::size_t capacity=64){particles::SystemDefinition definition;definition.emitters.push_back(std::move(emitter));return particles::CpuSystem(std::move(definition),seed,capacity);}

void test_catalog_and_parser(){
    expect(particles::plugin_catalog().size()==63,"all stable plugin IDs are catalogued");
    expect(particles::find_plugin(34)->support==particles::Support::cpu,"shape creator is CPU supported");
    expect(particles::find_plugin(22)->support==particles::Support::metadata_only,"renderers are explicit metadata-only families");
    expect(particles::find_plugin(51)->support==particles::Support::unsupported,"reference-unimplemented terrain bounce is explicit");
    const auto fixture=legacy_fixture();const auto loaded=particles::load_alo(fixture,"synthetic.alo");expect(bool(loaded),"synthetic legacy ALO parses");
    expect(loaded.value().emitters.size()==1,"one emitter parsed");const auto& emitter=loaded.value().emitters[0];
    expect(emitter.name=="fixture"&&emitter.creator_id==34&&emitter.killer_id==19,"legacy emitter converts to stable plugin IDs");
    expect(close(emitter.position.point.x,2),"legacy position group decodes");expect(close(emitter.lifetime,2),"legacy age killer data decodes");
    expect(close(emitter.size.keys.front().value,4.0F/2.0F)&&close(emitter.size.keys.back().value,8.0F/2.0F)&&close(emitter.size_variation,0.2F),"legacy size keys are full widths halved and the variation is the raw retail spread");expect(close(emitter.lifetime_variation,0.25F),"legacy lifetime variation remains fractional");
    expect(std::find(emitter.modifier_ids.begin(),emitter.modifier_ids.end(),10U)!=emitter.modifier_ids.end(),"acceleration family inferred");
    auto truncated=fixture;truncated.pop_back();const auto rejected=particles::load_alo(truncated,"truncated.alo");expect(!rejected&&rejected.error().code==particles::diagnostic_codes::structure,"enclosing bounds reject truncation");
}

void test_malformed_numeric_inputs(){
    const auto short_scalar=particles::load_alo(legacy_fixture(Bytes{std::byte{0},std::byte{0},std::byte{0}}),"short-scalar.alo");
    expect(!short_scalar&&short_scalar.error().code==particles::diagnostic_codes::structure,"wrong-sized float property is rejected");
    const auto short_vector=particles::load_alo(legacy_fixture(scalar(2),scalar(4),scalar(1)),"short-vector.alo");
    expect(!short_vector&&short_vector.error().code==particles::diagnostic_codes::structure,"wrong-sized float vector property is rejected");
    const auto short_track=particles::load_alo(legacy_fixture(scalar(2),Bytes{std::byte{0},std::byte{0},std::byte{0}}),"short-track.alo");
    expect(!short_track&&short_track.error().code==particles::diagnostic_codes::structure,"wrong-sized float track endpoint is rejected");
    const auto nan_property=particles::load_alo(legacy_fixture(scalar(std::numeric_limits<float>::quiet_NaN())),"nan-property.alo");
    expect(!nan_property&&nan_property.error().code==particles::diagnostic_codes::invalid_value,"NaN float property is rejected");
    const auto infinite_track=particles::load_alo(legacy_fixture(scalar(2),scalar(std::numeric_limits<float>::infinity())),"infinite-track.alo");
    expect(!infinite_track&&infinite_track.error().code==particles::diagnostic_codes::invalid_value,"infinite float track endpoint is rejected");
}

void test_mesh_parser(){
    expect(particles::find_plugin(35)->support==particles::Support::cpu,
        "EnhancedMesh is classified as V1 CPU-capable with a mesh binding");
    for(const std::uint32_t raw:{0U,1U,2U,3U,99U}){
        Bytes props;append(props,mini(0x34,integer(raw)));
        const auto loaded=particles::load_alo(legacy_fixture(scalar(2),scalar(4),vector3(0,0,0),props));
        expect(bool(loaded),"mesh mode fixture parses");
        if(!loaded)continue;
        const auto& emitter=loaded.value().emitters.front();
        const auto expected=raw==0?particles::MeshSpawnMode::disabled:
            raw==2?particles::MeshSpawnMode::random_surface:
            raw==3?particles::MeshSpawnMode::every_vertex:particles::MeshSpawnMode::random_vertex;
        expect(emitter.mesh_mode_raw==raw&&emitter.mesh_mode==expected&&
            emitter.creator_id==(raw==0?34U:35U)&&emitter.cpu_ready,
            "mesh mode conversion keeps raw value and source fallback");
        expect(close(emitter.mesh_surface_offset,0.5F),"missing surface offset defaults to one half");
    }
    Bytes offset;append(offset,mini(0x34,integer(1)));append(offset,mini(0x3c,scalar(-2.25F)));
    const auto loaded=particles::load_alo(legacy_fixture(scalar(2),scalar(4),vector3(0,0,0),offset));
    expect(loaded&&close(loaded.value().emitters.front().mesh_surface_offset,-2.25F),
        "signed surface offset is preserved");
    for(const auto& malformed:{mini(0x34,Bytes{std::byte{1}}),mini(0x3c,Bytes{std::byte{1}}),
        mini(0x3c,scalar(std::numeric_limits<float>::infinity()))}){
        const auto rejected=particles::load_alo(legacy_fixture(scalar(2),scalar(4),vector3(0,0,0),malformed));
        expect(!rejected,"malformed or non-finite mesh mini fails parsing");
    }
}

particles::MeshBinding asymmetric_mesh(){
    particles::MeshBinding binding;
    binding.geometry.submeshes={
        {{{{1,0,0},{1,0,0}},{{2,0,0},{0,1,0}}},{}},
        {{{{10,0,0},{0,0,1}},{{20,0,0},{0,1,0}},{{30,0,0},{1,0,0}}},
         {0,1,2}}};
    return binding;
}
std::uint32_t draw(std::uint32_t& state){
    state^=state<<13U;state^=state>>17U;state^=state<<5U;return state;
}
std::uint32_t bounded(std::uint32_t& state,const std::uint32_t bound){
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(draw(state))*bound)>>32U);
}
float unit(std::uint32_t& state){return static_cast<float>(draw(state)>>8U)/16777216.0F;}
particles::EmitterDefinition mesh_emitter(const particles::MeshSpawnMode mode){
    auto emitter=base_emitter();emitter.creator_id=35;emitter.mesh_mode=mode;
    emitter.mesh_mode_raw=static_cast<std::uint32_t>(mode);emitter.mesh_surface_offset=0;
    return emitter;
}

void test_mesh_cpu_sampling(){
    auto binding=asymmetric_mesh();
    auto emitter=mesh_emitter(particles::MeshSpawnMode::random_vertex);
    emitter.position.shape=particles::Shape::range;
    emitter.position.range_min={100,100,100};emitter.position.range_max={200,200,200};
    emitter.bursting=true;emitter.particles_per_interval=8;
    particles::SystemDefinition definition;definition.emitters.push_back(emitter);
    particles::CpuSystem cpu(definition,123,64,binding);
    cpu.set_origin({500,0,0});
    expect(cpu.advance(std::numeric_limits<float>::min()).spawned==8,"random vertex burst emits requested count");
    std::uint32_t state=123;
    for(const auto& particle:cpu.particles()){
        const auto submesh=bounded(state,2);
        const auto vertex=bounded(state,static_cast<std::uint32_t>(binding.geometry.submeshes[submesh].vertices.size()));
        const auto expected=binding.geometry.submeshes[submesh].vertices[vertex].position;
        expect(close(particle.position.x,expected.x)&&close(particle.position.y,expected.y),
            "independent xorshift bounded draws select submesh then vertex without position RNG");
    }

    emitter=mesh_emitter(particles::MeshSpawnMode::random_surface);
    emitter.bursting=true;emitter.particles_per_interval=5;
    definition.emitters[0]=emitter;
    binding.geometry.submeshes[0].triangle_indices={0,1,0};
    binding.geometry.submeshes[1].triangle_indices={0,1,2,0,2,1};
    cpu=particles::CpuSystem(definition,0,64,binding);
    expect(cpu.advance(std::numeric_limits<float>::min()).spawned==5,"seed zero maps to one for surface sampling");
    state=1;
    for(const auto& particle:cpu.particles()){
        const auto submesh=bounded(state,2);
        const auto& sub=binding.geometry.submeshes[submesh];
        const auto face=bounded(state,static_cast<std::uint32_t>(sub.triangle_indices.size()/3))*3U;
        const float w1=unit(state),w2=unit(state)*(1-w1),w3=1-w1-w2;
        const float x=sub.vertices[sub.triangle_indices[face]].position.x*w1+
            sub.vertices[sub.triangle_indices[face+1]].position.x*w2+
            sub.vertices[sub.triangle_indices[face+2]].position.x*w3;
        expect(close(particle.position.x,x),"independent seeded barycentrics match source draw order");
    }

    emitter=mesh_emitter(particles::MeshSpawnMode::random_vertex);
    emitter.velocity.shape=particles::Shape::range;
    emitter.velocity.range_min={1,2,3};emitter.velocity.range_max={2,3,4};
    definition.emitters[0]=emitter;
    cpu=particles::CpuSystem(definition,123,4,binding);
    expect(cpu.advance(std::numeric_limits<float>::min()).spawned==1,"mesh emission with random velocity spawns");
    state=123;
    static_cast<void>(unit(state));static_cast<void>(unit(state));static_cast<void>(unit(state));
    const auto selected_submesh=bounded(state,2);
    const auto selected_vertex=bounded(state,static_cast<std::uint32_t>(
        binding.geometry.submeshes[selected_submesh].vertices.size()));
    expect(close(cpu.particles().front().position.x,
        binding.geometry.submeshes[selected_submesh].vertices[selected_vertex].position.x),
        "velocity group consumes RNG before mesh selection");

    emitter=mesh_emitter(particles::MeshSpawnMode::every_vertex);
    emitter.bursting=true;emitter.particles_per_interval=2;
    definition.emitters[0]=emitter;
    cpu=particles::CpuSystem(definition,7,8,binding);
    const auto first=cpu.advance(std::numeric_limits<float>::min());
    expect(first.spawned==8&&first.dropped_at_capacity==2,
        "every-vertex burst multiplies total vertices by truncated Shape count and drops at capacity");
    const float expected_x[]{20,2,10,30,1,20,2,10};
    for(std::size_t i=0;i<cpu.particles().size();++i)
        expect(close(cpu.particles()[i].position.x,expected_x[i]),"shuffled every-vertex batch spans submeshes and cycles");
    emitter.bursting=false;emitter.particles_per_interval=4;emitter.spawn_interval=1;
    emitter.stop_time=0;definition.emitters[0]=emitter;
    cpu=particles::CpuSystem(definition,7,10,binding);
    expect(cpu.advance(std::numeric_limits<float>::min()).spawned==5,"continuous every-vertex event emits one mesh traversal");
    const auto second=cpu.advance(std::nextafter(0.25F,1.0F));
    expect(second.spawned==5&&cpu.particles().size()==10,
        "continuous interval divides by particles_per_interval and cursor restarts at first vertex");
}

void test_mesh_frames_and_capacity(){
    particles::MeshBinding binding;
    binding.geometry.submeshes={{{{{2,3,4},{1,2,3}}},{}}};
    binding.frame.origin={7,11,13};
    binding.frame.basis={{2,0,0},{0,3,0},{0,0,4}};
    auto emitter=mesh_emitter(particles::MeshSpawnMode::every_vertex);
    emitter.mesh_surface_offset=0.5F;emitter.velocity.point={1,2,3};
    particles::SystemDefinition definition;definition.emitters.push_back(emitter);
    particles::CpuSystem cpu(definition,7,4,binding);
    cpu.set_origin({100,100,100});
    cpu.set_basis({{1,0,0},{0,2,0},{0,0,1}});
    binding.geometry.submeshes[0].vertices[0].position={999,999,999};
    expect(cpu.advance(std::numeric_limits<float>::min()).spawned==1,"mesh emission owns caller geometry after mutation");
    const auto particle=cpu.particles().front();
    expect(close(particle.position.x,11)&&close(particle.position.y,20)&&close(particle.position.z,29),
        "every-vertex emission uses the affine vertex without a surface offset");
    const float tilt=std::atan2(12.0F,std::sqrt(40.0F));
    const float pitch=3.14159265358979323846F/2-tilt;
    const float azimuth=std::atan2(6.0F,2.0F);
    const float after_y_x=-4*std::cos(pitch)+3*std::sin(pitch);
    const float expected_x=after_y_x*std::cos(azimuth)-std::sin(azimuth);
    const float expected_y=after_y_x*std::sin(azimuth)+std::cos(azimuth);
    const float expected_z=4*std::sin(pitch)+3*std::cos(pitch);
    expect(close(particle.velocity.x,expected_x)&&close(particle.velocity.y,expected_y)&&
        close(particle.velocity.z,expected_z),
        "nonaxial alignment keeps the source Z-Y-Z rotation roll after emitter-local velocity");

    emitter=mesh_emitter(particles::MeshSpawnMode::every_vertex);
    emitter.spawn_interval=1;emitter.stop_time=0;emitter.lifetime=0.5F;
    definition.emitters[0]=emitter;
    binding.geometry.submeshes[0].vertices[0].position={2,3,4};
    cpu=particles::CpuSystem(definition,7,4,binding);
    cpu.set_origin({900,0,0});
    expect(cpu.advance(std::numeric_limits<float>::min()).spawned==1&&close(cpu.particles().front().position.x,11),
        "mesh frame position is separate from emitter origin");
    expect(bool(cpu.set_mesh_frame({{17,11,13},binding.frame.basis})),"finite mesh frame update succeeds");
    const auto changed=cpu.advance(std::nextafter(1.0F,2.0F));
    expect(changed.killed==1&&changed.spawned==1&&close(cpu.particles().front().position.x,21),
        "mesh frame update affects future emission");
    const auto invalid=cpu.set_mesh_frame({{std::numeric_limits<float>::quiet_NaN(),0,0},{}});
    expect(!invalid,"non-finite mesh frame update is rejected");

    binding=asymmetric_mesh();
    emitter=mesh_emitter(particles::MeshSpawnMode::every_vertex);
    emitter.spawn_interval=1;emitter.stop_time=0;emitter.lifetime=0.5F;
    definition.emitters[0]=emitter;
    cpu=particles::CpuSystem(definition,7,2,binding);
    expect(cpu.advance(std::numeric_limits<float>::min()).spawned==2,"first every-vertex event fills capacity");
    expect(cpu.advance(std::nextafter(1.0F,2.0F)).spawned==2&&close(cpu.particles()[0].position.x,30)&&
        close(cpu.particles()[1].position.x,10),
        "new capacity-limited batch reshuffles the complete vertex set");

    definition.emitters.push_back(emitter);
    cpu=particles::CpuSystem(definition,7,16,binding);
    expect(cpu.advance(std::numeric_limits<float>::min()).spawned==10&&close(cpu.particles()[0].position.x,20)&&
        close(cpu.particles()[5].position.x,30),
        "two mesh emitters in one effect shuffle independent batches");

    definition.emitters.resize(1);
    emitter=mesh_emitter(particles::MeshSpawnMode::random_vertex);
    emitter.spawn_interval=1;emitter.stop_time=0;emitter.lifetime=1.5F;
    definition.emitters[0]=emitter;
    cpu=particles::CpuSystem(definition,123,1,binding);
    expect(cpu.advance(std::numeric_limits<float>::min()).spawned==1&&cpu.advance(std::nextafter(1.0F,2.0F)).dropped_at_capacity==1,
        "capacity drop occurs without creating a random mesh particle");
    std::uint32_t state=123;
    for(int i=0;i<1;++i){
        const auto submesh=bounded(state,2);
        static_cast<void>(bounded(state,static_cast<std::uint32_t>(
            binding.geometry.submeshes[submesh].vertices.size())));
    }
    const auto next_submesh=bounded(state,2);
    const auto next_vertex=bounded(state,static_cast<std::uint32_t>(
        binding.geometry.submeshes[next_submesh].vertices.size()));
    const auto resumed=cpu.advance(std::nextafter(1.0F,2.0F));
    expect(resumed.spawned==1&&close(cpu.particles().front().position.x,
        binding.geometry.submeshes[next_submesh].vertices[next_vertex].position.x),
        "dropped event consumes no mesh RNG before later emission");
}

void test_mesh_surface_size_offset() {
    particles::MeshBinding binding;
    binding.geometry.submeshes={{{{{0,0,0},{0,0,3}},{{2,0,0},{0,0,3}},{{0,3,0},{0,0,3}}},{0,1,2}}};
    binding.frame.origin={7,11,13};
    binding.frame.basis={{2,0,0},{0,3,0},{0,0,4}};
    for(const auto mode:{particles::MeshSpawnMode::random_vertex,particles::MeshSpawnMode::random_surface}){
        auto emitter=mesh_emitter(mode);
        emitter.bursting=true;emitter.particles_per_interval=8;
        emitter.mesh_surface_offset=1.5F;emitter.size_variation=0.4F;
        emitter.size={particles::Interpolation::linear,{{0,4},{1,1}}};
        particles::SystemDefinition definition;definition.emitters.push_back(emitter);
        particles::CpuSystem shifted(definition,123,16,binding);
        definition.emitters[0].mesh_surface_offset=0;
        particles::CpuSystem control(definition,123,16,binding);
        expect(shifted.advance(std::numeric_limits<float>::min()).spawned==8&&control.advance(std::numeric_limits<float>::min()).spawned==8,
            "random mesh controls emit the same authored burst");
        const std::vector<particles::Particle> before(shifted.particles().begin(),shifted.particles().end());
        for(std::size_t i=0;i<before.size();++i){
            const auto& a=before[i];const auto& b=control.particles()[i];
            expect(close(a.position.x,b.position.x)&&close(a.position.y,b.position.y)&&
                close(a.position.z-b.position.z,0.75F*a.size),
                "random mesh offset uses normalized transformed normal and sampled half-extent");
            expect(a.id==b.id&&a.size==b.size&&a.death_time==b.death_time,
                "surface offset adds no RNG draws to size variation or lifetime");
        }
        static_cast<void>(shifted.advance(1));
        for(std::size_t i=0;i<before.size();++i)
            expect(shifted.particles()[i].position.z==before[i].position.z&&
                shifted.particles()[i].size<before[i].size,
                "birth surface offset stays fixed as the size gradient shrinks");

        definition.emitters[0].mesh_surface_offset=-2;
        particles::CpuSystem inward(definition,123,16,binding);
        static_cast<void>(inward.advance(std::numeric_limits<float>::min()));
        for(const auto& a:inward.particles())
            expect(close(a.position.z,13-a.size),"signed mesh offset remains authored");
        definition.emitters[0].size=constant(0);
        particles::CpuSystem zero_size(definition,123,16,binding);
        static_cast<void>(zero_size.advance(std::numeric_limits<float>::min()));
        for(const auto& a:zero_size.particles())
            expect(a.position.z==13,"zero half-extent has zero surface displacement");
    }
    auto emitter=mesh_emitter(particles::MeshSpawnMode::random_vertex);
    emitter.mesh_surface_offset=1.5F;emitter.size=constant(4);
    binding.geometry.submeshes[0].vertices={{{0,0,0},{1,2,3}}};
    binding.geometry.submeshes[0].triangle_indices.clear();
    particles::SystemDefinition definition;definition.emitters.push_back(emitter);
    particles::CpuSystem skewed(definition,123,4,binding);
    static_cast<void>(skewed.advance(std::numeric_limits<float>::min()));
    const auto point=skewed.particles().front().position;
    const float magnitude=std::sqrt(184.0F);
    expect(close(point.x,7+6/magnitude)&&close(point.y,11+18/magnitude)&&close(point.z,13+36/magnitude),
        "world-normal normalization follows the nonuniform mesh frame");
    binding.geometry.submeshes[0].vertices[0].normal={};
    particles::CpuSystem degenerate(definition,123,4,binding);
    static_cast<void>(degenerate.advance(std::numeric_limits<float>::min()));
    const auto zero=degenerate.particles().front().position;
    expect(zero.x==7&&zero.y==11&&zero.z==13,"zero normal yields a finite zero surface offset");
}

void test_lifetime_variation() {
    // PL-01: raw saved bounds remain inspectable; legacy post-load rebuilds the runtime range.
    const auto loaded = particles::load_alo(legacy_fixture(scalar(8),scalar(4),vector3(0,0,0),{},
        old_group(1,8,8,8,1.44F,14.56F)), "age-range.alo");
    expect(bool(loaded), "serialized lifetime range parses");
    if (!loaded) return;
    auto emitter = loaded.value().emitters.front();
    expect(emitter.lifetime_range && close(emitter.lifetime_range->minimum,1.44F)
        && close(emitter.lifetime_range->maximum,14.56F) && !emitter.lifetime_range->constant_mode,
        "loader retains both saved scalar age bounds and range mode");
    emitter.bursting = true; emitter.particles_per_interval = 512;
    emitter.spawn_interval = 100; emitter.lifetime = 8; emitter.lifetime_variation = 0.82F;
    auto cpu = system_with(emitter, 123, 512);
    expect(cpu.advance(std::numeric_limits<float>::min()).spawned == 512, "lifetime probe fills one seeded birth batch");
    float sum = 0, minimum = 8, maximum = 0;
    for (const auto& particle : cpu.particles()) {
        const float age = particle.death_time - particle.spawn_time;
        expect(age >= 1.44F - 0.00001F && age <= 8, "births sample the post-load age range");
        sum += age; minimum = std::min(minimum, age); maximum = std::max(maximum, age);
    }
    expect(minimum < 2 && maximum > 7.8F && std::abs(sum / 512 - 4.72F) < 0.25F,
        "post-load smoke covers the shorter range and expected mean");
    expect(close(loaded.value().emitters.front().lifetime_range->maximum,14.56F),
        "runtime construction leaves the raw decoded definition unchanged");
    expect(cpu.advance(8).killed == 512 && cpu.particles().empty(),
        "all varied smoke particles retire by the post-load maximum");

    emitter.lifetime_range = particles::EmitterDefinition::LifetimeRange{3,3};
    emitter.lifetime_variation = 0;
    cpu = system_with(emitter, 123, 512); static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    expect(std::all_of(cpu.particles().begin(), cpu.particles().end(),
        [](const auto& particle) { return particle.death_time - particle.spawn_time == 8; }),
        "equal range-mode bounds are rebuilt, and zero variation yields the authored maximum");
    emitter.lifetime_variation = 1.5F;
    cpu = system_with(emitter, 123, 512); static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    expect(std::all_of(cpu.particles().begin(), cpu.particles().end(),
        [](const auto& particle) { return particle.death_time>=0 && particle.death_time<=8; }),
        "wide variation clamps the post-load lower age bound to zero");
    emitter.lifetime_variation = -0.5F;
    cpu = system_with(emitter, 123, 512); static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    expect(std::all_of(cpu.particles().begin(), cpu.particles().end(),
        [](const auto& particle) { return particle.death_time>=8 && particle.death_time<=12; }),
        "negative variation sorts the rebuilt age bounds");
    emitter.lifetime_variation = -std::numeric_limits<float>::max();
    cpu = system_with(emitter, 123, 512);
    expect(cpu.advance(std::numeric_limits<float>::min()).spawned==0, "overflowing rebuilt age bounds fail closed");
    const auto point = particles::load_alo(legacy_fixture(scalar(8)), "age-point.alo");
    expect(point && point.value().emitters.front().lifetime_range
        && point.value().emitters.front().lifetime_range->minimum==2
        && point.value().emitters.front().lifetime_range->maximum==2
        && point.value().emitters.front().lifetime_range->constant_mode,
        "constant-mode age loading uses its saved default");
    if(point){
        particles::CpuSystem constant(point.value(),123,512);static_cast<void>(constant.advance(std::numeric_limits<float>::min()));
        expect(!constant.particles().empty() && std::all_of(constant.particles().begin(),constant.particles().end(),
            [](const auto& particle){return particle.death_time-particle.spawn_time==2;}),
            "post-load preserves a true constant sampler's mode and saved default");
    }
}

void test_creator_killers_and_translation(){
    auto emitter=base_emitter();emitter.position.shape=particles::Shape::range;emitter.position.range_min={1,2,3};emitter.position.range_max={2,3,4};
    emitter.velocity.shape=particles::Shape::sphere;emitter.velocity.radius_min=2;emitter.velocity.radius_max=2;auto cpu=system_with(emitter,123);expect(cpu.advance(std::numeric_limits<float>::min()).spawned==1,"shape creator spawns");
    const auto particle=cpu.particles().front();expect(particle.position.x>=1&&particle.position.x<=2&&close(std::sqrt(particle.velocity.x*particle.velocity.x+particle.velocity.y*particle.velocity.y+particle.velocity.z*particle.velocity.z),2),"range/sphere sampling stays bounded");

    emitter=base_emitter();emitter.lifetime=0.5F;cpu=system_with(emitter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));expect(cpu.advance(0.5F).killed==1&&cpu.particles().empty(),"age killer uses presentation time");
    emitter=base_emitter();emitter.killer_id=21;emitter.position.point.z=-1;cpu=system_with(emitter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));expect(cpu.advance(0.01F).killed==1,"terrain killer uses legacy z=0 presentation plane");
    emitter=base_emitter();emitter.translater_id=26;cpu=system_with(emitter);cpu.set_origin({1,0,0});static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));cpu.set_origin({4,0,0});static_cast<void>(cpu.advance(0.01F));expect(close(cpu.particles().front().position.x,4),"emitter translater follows origin delta");
    emitter=base_emitter();emitter.position.point={2,0,0};emitter.inward_speed=-2;cpu=system_with(emitter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));expect(close(cpu.particles().front().motion_velocity.x,-2)&&cpu.particles().front().velocity.x==0,"radial motion remains separate from sampled velocity");
    emitter=base_emitter();emitter.position.point={1,0,0};emitter.velocity.point={1,0,0};cpu=system_with(emitter);cpu.set_basis({{0,1,0},{-1,0,0},{0,0,1}});static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));expect(close(cpu.particles().front().position.y,1)&&close(cpu.particles().front().velocity.y,1),"portable basis replaces DirectX local transform");
}

// #433: a particle of the Emitter translater (FoC's linked particles, BP-40) keeps its position in its
// emitter's frame: it follows the emitter's rotation as well as its translation, on an advance and
// on a follow_emitter between advances, and a follow is never applied twice. Its velocity stays a
// world vector (BP-40).
void test_emitter_translater_follows_rotation(){
    const particles::Basis3 quarter{{0,1,0},{-1,0,0},{0,0,1}};
    const particles::Basis3 half{{-1,0,0},{0,-1,0},{0,0,1}};
    auto emitter=base_emitter();emitter.translater_id=26;emitter.position.point={2,0,0};emitter.velocity.point={1,0,0};
    auto cpu=system_with(emitter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    expect(close(cpu.particles().front().position.x,2),"emitter-linked particle starts at its local offset");
    cpu.set_origin({5,0,0});cpu.set_basis(quarter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    auto particle=cpu.particles().front();
    expect(close(particle.position.x,5)&&close(particle.position.y,2),"emitter-linked particle turns with its emitter");
    expect(close(particle.velocity.x,1)&&close(particle.velocity.y,0),"emitter-linked particle velocity stays a world vector");
    const float time=cpu.presentation_time();
    cpu.set_origin({0,0,0});cpu.set_basis(half);cpu.follow_emitter();
    particle=cpu.particles().front();
    expect(close(particle.position.x,-2)&&close(particle.position.y,0),"a follow between advances places it at its emitter now");
    expect(cpu.presentation_time()==time,"a follow does not advance the clock");
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    expect(close(cpu.particles().front().position.x,-2)&&close(cpu.particles().front().position.y,0),"the next advance does not apply the follow again");
    emitter.translater_id=27;cpu=system_with(emitter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    cpu.set_origin({5,0,0});cpu.set_basis(quarter);cpu.follow_emitter();static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    expect(close(cpu.particles().front().position.x,2)&&close(cpu.particles().front().position.y,0),"a world particle stays where it was born");
}

// #439: on a sample where the emitter turns, a linked particle's object-space acceleration is
// rotated once, by the emitter's current basis, into its world velocity
// (BP-40: FoC rotates the particle's acceleration by the emitter's current transform).
void test_emitter_translater_local_acceleration(){
    const particles::Basis3 quarter{{0,1,0},{-1,0,0},{0,0,1}};
    const particles::Basis3 half{{-1,0,0},{0,-1,0},{0,0,1}};
    auto emitter=base_emitter();emitter.translater_id=26;emitter.modifier_ids={10};emitter.acceleration={1,0,0};emitter.acceleration_local=true;
    auto cpu=system_with(emitter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    cpu.set_basis(quarter);static_cast<void>(cpu.advance(1));
    auto particle=cpu.particles().front();
    expect(close(particle.acceleration.x,0)&&close(particle.acceleration.y,1),"a turning sample takes the local acceleration in the emitter's current frame");
    expect(close(particle.velocity.x,0)&&close(particle.velocity.y,1),"a turning sample rotates the local acceleration once");
    cpu.set_basis(half);cpu.follow_emitter();
    particle=cpu.particles().front();
    expect(close(particle.velocity.x,0)&&close(particle.velocity.y,1),"a follow between samples leaves the world velocity alone");
    static_cast<void>(cpu.advance(1));
    particle=cpu.particles().front();
    expect(close(particle.acceleration.x,-1)&&close(particle.acceleration.y,0),"the next sample takes the acceleration in the frame it is drawn in");
    expect(close(particle.velocity.x,-1)&&close(particle.velocity.y,1),"the next sample adds it once to the world velocity");
}

void test_walk_trails(){
    // PS-16: simultaneous parents visit the same shared schedule along separate segments.
    auto root=base_emitter();root.bursting=true;root.particles_per_interval=2;root.velocity.point={4,0,0};
    auto trail=base_emitter();trail.creator_id=39;trail.parent_emitter=0;trail.spawn_interval=0.25F;
    trail.stop_time=0;trail.lifetime=5;auto death=base_emitter();death.creator_id=40;death.parent_emitter=0;
    death.bursting=true;death.particles_per_interval=1;
    particles::SystemDefinition definition;definition.emitters={root,trail,death};
    particles::CpuSystem cpu(definition,7,64);
    expect(cpu.advance(0.125F).spawned==4,"each simultaneous parent supplies an initial dependent segment");
    const auto initial_children=std::vector<particles::Particle>(cpu.particles().begin()+2,cpu.particles().end());
    expect(cpu.advance(0.5F).spawned==4,"shared dependent phase repeats both scheduled events for each parent");
    const float positions[]{1,2,1,2};const float birth_times[]{0.25F,0.5F,0.25F,0.5F};
    for(std::size_t i=0;i<4;++i){const auto& p=cpu.particles()[i+4];
        expect(close(p.position.x,positions[i])&&p.spawn_time==birth_times[i],"trail births lie at their event fraction along the parent segment");
    }
    for(const auto& old:initial_children){const auto found=std::find_if(cpu.particles().begin(),cpu.particles().end(),
        [&](const auto& p){return p.id==old.id;});expect(found!=cpu.particles().end()&&found->spawn_time==old.spawn_time,
        "dependent start phase ages existing children once before new emission");}
    expect(bool(cpu.set_detail({0,1})),"mask simultaneous parents");
    expect(cpu.advance(0.5F).spawned==0,"masked parents supply no trail segment");
    root.lifetime=0.5F;definition.emitters[0]=root;cpu=particles::CpuSystem(definition,7,64);
    static_cast<void>(cpu.advance(0.125F));const auto retired=cpu.advance(0.5F);
    expect(retired.killed==2&&retired.death_bursts==2&&retired.spawned==2&&cpu.live_child_instances()==0,
        "parents retiring in an accumulated update emit death bursts once and no postmortem trail");
    expect(close(cpu.particles()[cpu.particles().size()-1].position.x,0.5F),"death uses the final retained parent position before retirement motion");
}

void test_walk_births(){
    // PS-10/PS-42: equality and residuals are intentional observables.
    auto emitter=base_emitter();emitter.start_delay=0.5F;emitter.spawn_interval=0.25F;
    emitter.stop_time=0;emitter.velocity.point={2,0,0};auto cpu=system_with(emitter);
    expect(cpu.advance(0).spawned==0&&cpu.presentation_time()==0&&cpu.particles().empty(),"zero delta is a complete no-op");
    expect(cpu.advance(0.75F).spawned==1&&close(cpu.particles()[0].position.x,0.5F)&&
        cpu.particles()[0].spawn_time==0.5F,"first delayed birth uses residual age and one initial event");
    expect(cpu.advance(0).spawned==0&&cpu.presentation_time()==0.75F,"zero delta leaves an existing generation untouched");
    expect(cpu.advance(0.01F).spawned==1&&close(cpu.particles().back().position.x,0.02F),
        "strict later interval entry catches the inclusive delayed event with residual motion");
    emitter.start_delay=0;emitter.spawn_interval=0.5F;emitter.velocity.point={};cpu=system_with(emitter);
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    expect(cpu.advance(0.5F).spawned==0,"later exact interval equality does not enter scheduling");
    expect(cpu.advance(0.001F).spawned==1&&close(cpu.particles().back().spawn_time,0.5F),
        "later scheduling visits the equality event once after strict entry");
    emitter.lifetime=1;emitter.spawn_interval=0.25F;cpu=system_with(emitter,7,64);
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));const auto catchup=cpu.advance(10);
    expect(catchup.killed==1&&catchup.spawned==4&&cpu.particles().size()==4&&cpu.particles()[0].spawn_time==9.25F,
        "long catch-up skips events already at maximum age without consuming capacity");
    emitter.lifetime=10;emitter.spawn_interval=0.25F;cpu=system_with(emitter);cpu.set_origin({});
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));cpu.set_origin({10,0,0});
    const auto moved=cpu.advance(1);expect(moved.spawned==4,"moving-emitter update schedules four retained events");
    for(std::size_t i=1;i<cpu.particles().size();++i)
        expect(close(cpu.particles()[i].position.x,2.5F*static_cast<float>(i)),"births interpolate the host translation within an update");
    cpu=system_with(emitter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    cpu.set_basis({{0,1,0},{-1,0,0},{0,0,1}});emitter.position.point={1,0,0};
    auto rotating=system_with(emitter);static_cast<void>(rotating.advance(std::numeric_limits<float>::min()));
    rotating.set_basis({{0,1,0},{-1,0,0},{0,0,1}});static_cast<void>(rotating.advance(1));
    const auto midpoint=rotating.particles()[2].position;
    expect(close(midpoint.x,std::sqrt(0.5F))&&close(midpoint.y,std::sqrt(0.5F)),
        "birth frame uses normalized quaternion interpolation at the half-turn fraction");

    // BP-40/PS-15: births during a moving update keep their local host offset.
    emitter.translater_id=26;emitter.velocity.point={};cpu=system_with(emitter);
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));cpu.set_origin({10,0,0});
    expect(cpu.advance(1).spawned==4,"linked moving update admits four historical birth events");
    for(const auto& particle:cpu.particles())
        expect(close(particle.position.x,11)&&close(particle.position.y,0),
            "new and existing linked particles render at the current translated host offset");
    cpu=system_with(emitter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    cpu.set_origin({10,0,0});cpu.set_basis({{0,1,0},{-1,0,0},{0,0,1}});static_cast<void>(cpu.advance(1));
    for(const auto& particle:cpu.particles())
        expect(close(particle.position.x,10)&&close(particle.position.y,1),
            "births within a turning update carry their local offset into the current rigid frame");
    emitter.velocity.point={1,0,0};emitter.local_velocity=false;cpu=system_with(emitter);
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    cpu.set_basis({{0,1,0},{-1,0,0},{0,0,1}});static_cast<void>(cpu.advance(1));
    const auto& linked_midpoint=cpu.particles()[2];
    const float residual_component=std::sqrt(0.5F)*0.5F;
    expect(close(linked_midpoint.position.x,residual_component)&&close(linked_midpoint.position.y,1+residual_component),
        "linked residual movement uses the inverse sampled frame then the current host frame");
    expect(close(linked_midpoint.velocity.x,1)&&close(linked_midpoint.velocity.y,0)&&
        close(linked_midpoint.previous_position.x,0)&&close(linked_midpoint.previous_position.y,1),
        "linked birth velocity remains world-oriented and retained segment starts use the current host");
}

void test_mesh_birth_frames(){
    // PS-10/PS-15: every nozzle retains its mesh-bone offset at historical births.
    for(const float scale:{0.25F,0.5F,1.0F}){
        const particles::Basis3 initial{{scale,0,0},{0,scale,0},{0,0,scale}};
        const particles::Basis3 quarter{{0,scale,0},{-scale,0,0},{0,0,scale}};
        particles::MeshBinding binding;
        binding.geometry.submeshes={{{{{-30,0,0},{0,0,1}},{{0,0,0},{0,0,1}},
                                       {{30,0,0},{0,0,1}}},{}}};
        binding.frame={{3,0,0},initial};
        auto emitter=mesh_emitter(particles::MeshSpawnMode::every_vertex);
        emitter.translater_id=26;emitter.stop_time=0;emitter.spawn_interval=0.25F;
        particles::SystemDefinition definition;definition.emitters={emitter};
        particles::CpuSystem cpu(definition,123,64,binding);
        cpu.set_origin({});cpu.set_basis(initial);
        expect(cpu.advance(std::numeric_limits<float>::min()).spawned==3,"initial nozzle cloud emits");
        cpu.set_origin({10,0,0});cpu.set_basis(quarter);
        expect(bool(cpu.set_mesh_frame({{10,3,0},quarter})),"nozzle bone turns with scaled host");
        expect(cpu.advance(1).spawned==12,"turn admits four complete nozzle clouds");
        for(std::size_t batch=0;batch<5;++batch){
            float minimum=100,maximum=-100;
            for(std::size_t vertex=0;vertex<3;++vertex){
                const auto& particle=cpu.particles()[batch*3+vertex];
                expect(close(particle.position.x,10)&&close(particle.position.z,0),
                    "each linked mesh birth reaches the current bone plane exactly once");
                minimum=std::min(minimum,particle.position.y);maximum=std::max(maximum,particle.position.y);
            }
            expect(close(minimum,3-30*scale)&&close(maximum,3+30*scale),
                "every birth cloud spans the full scaled nozzle wall during a turn");
        }
        cpu.follow_emitter();
        expect(cpu.advance(0.251F).spawned==3,"sampled mesh frame is restored for subsequent births");
        for(const auto& particle:cpu.particles())
            expect(close(particle.position.x,10),"unchanged host does not carry mesh births twice");
    }
    // Shape and world-space mesh births also preserve scale in the sampled frame.
    auto emitter=base_emitter();emitter.stop_time=0;emitter.spawn_interval=0.25F;
    emitter.position.point={2,0,0};
    auto shape=system_with(emitter);
    shape.set_basis({{0.5F,0,0},{0,0.5F,0},{0,0,0.5F}});
    static_cast<void>(shape.advance(std::numeric_limits<float>::min()));
    shape.set_basis({{0,0.5F,0},{-0.5F,0,0},{0,0,0.5F}});
    static_cast<void>(shape.advance(1));
    expect(close(shape.particles()[2].position.x,std::sqrt(0.5F))&&
           close(shape.particles()[2].position.y,std::sqrt(0.5F)),
        "interpolated rotation retains authored half scale");
    particles::MeshBinding binding;binding.geometry.submeshes={{{{{2,0,0},{0,0,1}}},{}}};
    binding.frame={{3,0,0},{{0.5F,0,0},{0,0.5F,0},{0,0,0.5F}}};
    emitter.creator_id=35;emitter.mesh_mode=particles::MeshSpawnMode::random_vertex;
    particles::SystemDefinition definition;definition.emitters={emitter};
    particles::CpuSystem world(definition,123,64,binding);
    world.set_origin({});world.set_basis(binding.frame.basis);
    static_cast<void>(world.advance(std::numeric_limits<float>::min()));
    world.set_origin({10,0,0});world.set_basis({{0,0.5F,0},{-0.5F,0,0},{0,0,0.5F}});
    expect(bool(world.set_mesh_frame({{10,3,0},{{0,0.5F,0},{-0.5F,0,0},{0,0,0.5F}}})),"world mesh frame updates");
    static_cast<void>(world.advance(1));
    expect(close(world.particles()[2].position.x,5+4*std::sqrt(0.5F))&&
           close(world.particles()[2].position.y,4*std::sqrt(0.5F)),
        "world mesh birth samples the earlier host while retaining independent bone offset");
}

void test_walk_rotation(){
    constexpr float turn=6.283185307179586F;
    auto emitter=base_emitter();emitter.initial_rotation_only=true;emitter.rotation_rate=constant(0.25F);
    auto cpu=system_with(emitter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    expect(close(cpu.particles()[0].rotation,turn/4),"initial-only rotation converts turns to bin 64 at birth");
    static_cast<void>(cpu.advance(1));expect(close(cpu.particles()[0].rotation,turn/4),"initial-only orientation remains held");
    emitter.initial_rotation_only=false;emitter.rotation_rate=constant(1);cpu=system_with(emitter);
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(1));
    expect(cpu.particles()[0].rotation==0,"one full turn wraps to bin zero");
    for(const bool initial_only:{false,true})for(const float turns:{3.0F,6.0F,9.0F}){
        auto whole=base_emitter();whole.initial_rotation_only=initial_only;whole.rotation_rate=constant(turns);
        whole.spawn_interval=100;whole.lifetime=100;auto wrapped=system_with(whole);
        static_cast<void>(wrapped.advance(std::numeric_limits<float>::min()));
        if(initial_only)expect(wrapped.particles()[0].rotation_angle==0&&wrapped.particles()[0].rotation==0,
            "initial 3/6/9 turns subtract the sourced float turn count to exact zero");
        for(int step=0;step<3;++step){
            static_cast<void>(wrapped.advance(1));
            expect(wrapped.particles()[0].rotation_angle==0&&wrapped.particles()[0].rotation==0,
                "initial-only and accumulated 3/6/9-turn updates retain bin zero");
        }
    }
    emitter.rotation_rate=constant(0.25F);emitter.random_rotation_direction=true;cpu=system_with(emitter,7);
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(1));
    expect(cpu.particles()[0].rotation_direction<0&&close(cpu.particles()[0].rotation,191*turn/256),
        "reverse rotation mirrors bin 64 to 191");
    emitter.random_rotation_direction=false;emitter.rotation_rate=constant(0.5F);emitter.rotation_variation=0.4F;
    cpu=system_with(emitter,41);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));std::uint32_t state=41;static_cast<void>(unit(state));float angle{};
    for(int step=0;step<3;++step){
        const float variation=-0.4F+0.8F*unit(state);const float increment=turn*0.5F*0.25F;
        angle+=increment+increment*variation;
        static_cast<void>(cpu.advance(0.25F));const int bin=static_cast<int>((angle/turn)*256);
        expect(close(cpu.particles()[0].rotation,bin*turn/256),"each rotating update samples a fresh signed variation");
    }
    emitter.rotation_variation=0;emitter.initial_rotation_only=true;
    for(const float turns:{0.2499F,0.25F,0.2501F}){
        emitter.rotation_rate=constant(turns);cpu=system_with(emitter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
        expect(close(cpu.particles()[0].rotation,(turns<0.25F?63:64)*turn/256),"orientation truncates at a bin boundary");
    }
    // PS-29: divide by one turn before scaling; a reciprocal product rounds this down to bin 14.
    emitter.rotation_rate=constant(15.0F/256);cpu=system_with(emitter);
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    expect(close(cpu.particles()[0].rotation,15*turn/256),"source arithmetic retains bin 15 at its exact turn fraction");
    Bytes props;append(props,mini(0x17,scalar(0.3F)));append(props,mini(0x48,Bytes{std::byte{1}}));
    const auto loaded=particles::load_alo(legacy_fixture(scalar(2),scalar(4),vector3(0,0,0),props));
    expect(loaded&&loaded.value().emitters[0].initial_rotation_only&&loaded.value().emitters[0].rotation_variation==0.3F,
        "rotation loader retains the independent initial-only flag and raw variation");
}

void test_walk_color(){
    // PS-23: derive inclusive integer offsets independently from the local seed.
    auto emitter=base_emitter();emitter.bursting=true;emitter.particles_per_interval=256;
    emitter.modifier_ids={53};emitter.color_variance={0.2F,0.3F,0.4F,0.1F};
    emitter.red=emitter.green=emitter.blue=emitter.alpha=constant(128.0F/255);
    for(const bool locked:{false,true}){
        emitter.grayscale_variance=locked;auto cpu=system_with(emitter,29,256);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
        std::uint32_t state=29;bool negative{},positive{};
        for(const auto& particle:cpu.particles()){
            const int red=static_cast<int>(bounded(state,21));
            const int green=locked?red:static_cast<int>(bounded(state,31));
            const int blue=locked?red:static_cast<int>(bounded(state,41));
            const int alpha=static_cast<int>(bounded(state,21))-10;
            expect(particle.color_offset.x==red&&particle.color_offset.y==green&&
                particle.color_offset.z==blue&&particle.color_offset.w==alpha,"variation is byte-scaled with independent signed alpha");
            expect(close(particle.color.x,(128.0F+red)/255)&&close(particle.color.w,(128.0F+alpha)/255),
                "byte tracks receive integer variation before conversion to upload floats");
            negative|=alpha<0;positive|=alpha>0;
        }
        expect(negative&&positive,"signed alpha covers both sides even with locked RGB");
    }
    emitter.red=constant(1);emitter.alpha=constant(0);auto cpu=system_with(emitter,29,256);
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    expect(std::all_of(cpu.particles().begin(),cpu.particles().end(),[](const auto& p){
        return p.color.x==1&&p.color.w>=0&&p.color.w<=10.0F/255;
    }),"colour clamp endpoints stay within the byte range");
}

void test_walk_shapes(){
    constexpr float turn=6.283185307179586F;
    auto emitter=base_emitter();emitter.position.shape=particles::Shape::sphere;
    emitter.position.radius_max=3;emitter.bursting=true;emitter.particles_per_interval=4096;
    for(const bool hollow:{false,true}){
        emitter.hollow_position=hollow;auto cpu=system_with(emitter,117,4096);
        expect(cpu.advance(std::numeric_limits<float>::min()).spawned==4096,"sphere contract admits a complete batch");
        std::uint32_t state=117;double cosine_sum{},cosine_square{},radius_sum{};
        for(const auto& particle:cpu.particles()){
            const float radius=hollow?3:3*unit(state);
            const float angle=turn*unit(state),cosine=2*unit(state)-1;
            const float planar=std::sqrt(1-cosine*cosine);
            expect(close(particle.position.x,std::cos(angle)*planar*radius)&&
                close(particle.position.y,std::sin(angle)*planar*radius)&&close(particle.position.z,cosine*radius),
                "sphere uses uniform cosine and azimuth with authored radial sampling");
            cosine_sum+=cosine;cosine_square+=cosine*cosine;radius_sum+=radius;
        }
        expect(std::abs(cosine_sum/4096)<0.03&&std::abs(cosine_square/4096-1.0/3)<0.025&&
            std::abs(radius_sum/4096-(hollow?3:1.5))<0.06,"sphere moments follow a uniform polar cosine and radius");
    }
    emitter.position.shape=particles::Shape::pinched_cylinder;emitter.position.cylinder_radius=4;
    emitter.position.cylinder_height_max=10;emitter.position.pinch_fraction=0.6F;emitter.particles_per_interval=128;
    for(const bool hollow:{false,true}){
        emitter.hollow_position=hollow;auto cpu=system_with(emitter,19,128);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
        std::uint32_t state=19;bool upper{},lower{};
        for(const auto& particle:cpu.particles()){
            const float angle=turn*unit(state);float height=0.3F*unit(state);
            if(unit(state)<0.5F)height=1-height;
            float radius=4*(4*height*height-4*height+1);if(!hollow)radius*=unit(state);
            expect(close(particle.position.x,-std::sin(angle)*radius)&&close(particle.position.y,std::cos(angle)*radius)&&
                close(particle.position.z,10*height),"pinched cylinder mirrors height and uses the radius polynomial");
            upper|=height>=0.7F;lower|=height<=0.3F;
        }
        expect(upper&&lower,"pinched cylinder samples both ends");
    }
    Bytes group;u32(group,5);for(int index=0;index<15;++index)
        f32(group,index==9?4:index==10?std::bit_cast<float>(1U):index==11?10:0);
    const auto loaded=particles::load_alo(legacy_fixture(scalar(2),scalar(4),vector3(0,0,0),{},{},
        chunk(0x1100,chunk(0x1101,group),true)));
    expect(loaded&&loaded.value().emitters[0].velocity.shape==particles::Shape::pinched_cylinder&&
        loaded.value().emitters[0].velocity.pinch_fraction==1&&loaded.value().emitters[0].hollow_velocity&&
        loaded.value().emitters[0].velocity.cylinder_radius==4&&loaded.value().emitters[0].velocity.cylinder_height_max==10,
        "known V1 selector loads cylinder geometry and retains native default pinch fraction");
}

void test_walk_motion(){
    // PS-24..PS-26: expected values follow semi-implicit motion, not snapshots.
    auto emitter=base_emitter();emitter.modifier_ids={10};emitter.acceleration={2,0,0};
    auto cpu=system_with(emitter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(0.5F));
    expect(close(cpu.particles()[0].velocity.x,1)&&close(cpu.particles()[0].position.x,0.5F),
        "acceleration from rest precedes position motion");
    emitter.acceleration={0,0,2};emitter.acceleration_local=true;emitter.gravity=3;
    cpu=system_with(emitter);cpu.set_basis({{0,0,-1},{0,1,0},{1,0,0}});
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(0.5F));
    expect(close(cpu.particles()[0].position.x,0.5F)&&close(cpu.particles()[0].position.z,-0.75F),
        "gravity remains world negative Z after authored acceleration rotates");
    for(const float value:{0.009F,0.01F,0.011F}){
        emitter=base_emitter();emitter.gravity=value;cpu=system_with(emitter);
        static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(1));
        expect(close(cpu.particles()[0].velocity.z,value>0.01F?-value:0),"simple gravity threshold is inclusive");
    }
    for(const float value:{0.099F,0.101F}){
        emitter=base_emitter();emitter.acceleration={value,0,0};emitter.modifier_ids={10,11};
        emitter.position.point={2,0,0};emitter.inward_acceleration=-2;cpu=system_with(emitter);
        static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(1));
        expect(close(cpu.particles()[0].velocity.x,value>0.1F?value-2:0),
            "simple radial acceleration shares the squared-length activation gate");
    }
    emitter=base_emitter();emitter.modifier_ids={11,54};emitter.position.point={2,0,0};
    emitter.inward_acceleration=-2;emitter.wind_response=0.5F;cpu=system_with(emitter);cpu.set_wind({4,0,0});
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(1));
    expect(close(cpu.particles()[0].velocity.x,0),"ordinary radial path and half-scaled wind cancel");
    emitter.inward_acceleration=0;emitter.wind_disturbances=true;cpu=system_with(emitter);
    const particles::WindDisturbance disturbance{{},{4,0,0},4,2,1};
    expect(bool(cpu.set_wind_disturbances(std::span(&disturbance,1))),"finite scene disturbance accepted");
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(0.25F));
    expect(close(cpu.particles()[0].position.x,4)&&close(cpu.particles()[0].velocity.x,0),
        "mid-radius disturbance advances position without changing stored velocity");
    Bytes props;append(props,mini(0x0c,scalar(3)));append(props,mini(0x35,Bytes{std::byte{1}}));
    const auto loaded=particles::load_alo(legacy_fixture(scalar(2),scalar(4),vector3(0,0,2),props));
    expect(loaded&&loaded.value().emitters[0].gravity==3&&loaded.value().emitters[0].acceleration.z==2&&
        loaded.value().emitters[0].wind_response==0.5F,"loader retains independent gravity and native wind scaling");
}

void test_modifier_families(){
    auto emitter=base_emitter();emitter.modifier_ids={10};emitter.acceleration={2,0,0};auto cpu=system_with(emitter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(1));expect(close(cpu.particles().front().velocity.x,2),"acceleration modifier");
    emitter=base_emitter();emitter.modifier_ids={10};emitter.acceleration={2,0,0};emitter.acceleration_local=true;cpu=system_with(emitter);cpu.set_basis({{0,1,0},{-1,0,0},{0,0,1}});static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(1));expect(close(cpu.particles().front().velocity.y,2),"local acceleration uses portable basis");
    emitter=base_emitter();emitter.position.point={2,0,0};emitter.modifier_ids={11};emitter.inward_acceleration=-3;emitter.wind_response=0.5F;cpu=system_with(emitter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(1));expect(close(cpu.particles().front().velocity.x,-3),"ordinary attract acceleration modifier");
    emitter=base_emitter();emitter.modifier_ids={54};emitter.wind_response=0.5F;cpu=system_with(emitter);cpu.set_wind({4,0,0});static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(1));expect(close(cpu.particles().front().velocity.x,2),"wind acceleration modifier");

    emitter=base_emitter();emitter.lifetime=2;emitter.modifier_ids={7,18,49};emitter.size={particles::Interpolation::linear,{{0,2},{1,6}}};emitter.uv_index={particles::Interpolation::step,{{0,0},{0.4F,3},{1,3}}};emitter.texture_size=4;emitter.rotation_rate=constant(2);emitter.red={particles::Interpolation::linear,{{0,0},{1,1}}};cpu=system_with(emitter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(1));const auto& p=cpu.particles().front();expect(close(p.size,4),"linear/keyed size modifier");expect(close(p.texcoords.x,0.5F)&&close(p.texcoords.y,0.5F),"constant/keyed UV modifier");expect(close(p.rotation,0),"two-turn rotation wraps to bin zero");expect(close(p.color.x,127.0F/255),"legacy keyed colour truncates to an integer byte");

    {   // Retail spread: a factor 1 + U(-v, v) around the halved key, so sizes
        // exceed the key; the former (1 + v/2) normalisation never did.
        emitter=base_emitter();emitter.modifier_ids={57};emitter.size=constant(5);emitter.size_variation=0.92F;
        emitter.bursting=true;emitter.particles_per_interval=64;auto spread=system_with(emitter,5,64);static_cast<void>(spread.advance(std::numeric_limits<float>::min()));
        float smallest=100.0F,largest=0.0F;for(const auto& particle:spread.particles()){smallest=std::min(smallest,particle.size);largest=std::max(largest,particle.size);}
        expect(spread.particles().size()==64&&smallest>=5.0F*0.08F-1e-4F&&largest<=5.0F*1.92F+1e-4F&&largest>5.0F*1.5F&&smallest<5.0F*0.5F,
            "V1 size variation spreads the half-extent by the raw retail fraction");
    }
    emitter=base_emitter();emitter.modifier_ids={57,53,58};emitter.size=constant(3);emitter.size_variation=0.2F;emitter.color_variance={0.2F,0.3F,0.4F,0.1F};emitter.random_rotation=true;emitter.random_rotation_average=1;emitter.random_rotation_variation=0.5F;auto first=system_with(emitter,99);auto second=system_with(emitter,99);static_cast<void>(first.advance(std::numeric_limits<float>::min()));static_cast<void>(second.advance(std::numeric_limits<float>::min()));const auto& a=first.particles().front();const auto& b=second.particles().front();expect(close(a.size,b.size)&&close(a.rotation,b.rotation)&&close(a.color.x,b.color.x),"fixed seed reproduces constant size/rotation and color variance");expect(a.size>=2.4F&&a.size<=3.6F&&a.color.x>=1.0F,"variance modifiers stay bounded and saturate color");
}

void test_uv_cell_is_integer_part(){
    // FoC's explosion debris (`debre`: UV 7 -> 2 as a step track) keeps atlas cell 7 all its life.
    auto emitter=base_emitter();emitter.lifetime=2;emitter.modifier_ids={18};emitter.texture_size=64;
    emitter.uv_index={particles::Interpolation::step,{{0,7},{1,2}}};auto cpu=system_with(emitter);
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(1.9F));auto uv=cpu.particles().front().texcoords;
    expect(close(uv.x,0.875F)&&close(uv.y,0)&&close(uv.z,0.125F),"a step UV track holds its start cell to the end");
    // An interpolated value takes its integer part: 1.6 draws cell 1, not the nearest cell 2.
    emitter=base_emitter();emitter.lifetime=1;emitter.modifier_ids={18};emitter.texture_size=4;
    emitter.uv_index={particles::Interpolation::linear,{{0,0},{1,4}}};cpu=system_with(emitter);
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(0.4F));uv=cpu.particles().front().texcoords;
    expect(close(uv.x,0.5F)&&close(uv.y,0),"a fractional UV value draws its integer cell");
}

void test_resource_bound(){auto emitter=base_emitter();emitter.bursting=true;emitter.particles_per_interval=10;auto cpu=system_with(emitter,1,3);const auto stats=cpu.advance(std::numeric_limits<float>::min());expect(stats.spawned==3&&stats.dropped_at_capacity==7&&cpu.particles().size()==3&&cpu.total_dropped()==7,"particle storage is hard bounded");emitter.particles_per_interval=std::numeric_limits<float>::max();cpu=system_with(emitter,1,2);const auto huge=cpu.advance(std::numeric_limits<float>::min());expect(huge.spawned==2&&cpu.particles().size()==2&&huge.dropped_at_capacity>1000000,"pathological burst is counted without an unbounded loop");}

particles::SystemDefinition parent_system(){
    particles::SystemDefinition definition;
    auto root=base_emitter();root.name="parent";root.lifetime=0.5F;
    root.velocity.point={4,0,0};
    auto birth=base_emitter();birth.name="birth";birth.creator_id=39;birth.parent_emitter=0;
    birth.spawn_interval=0.25F;birth.stop_time=0;birth.lifetime=3;
    birth.position.point={1,0,0};birth.velocity.point={1,0,0};
    birth.inherit_parent_velocity=true;birth.inherited_velocity_scale=0.5F;
    birth.max_inherited_velocity=1.5F;
    auto death=base_emitter();death.name="death";death.creator_id=40;death.parent_emitter=0;
    death.spawn_on_parent_death=true;death.bursting=true;death.particles_per_interval=2;
    death.lifetime=3;death.position.point={2,0,0};
    death.inherit_parent_velocity=true;death.inherited_velocity_scale=0.25F;
    definition.emitters={root,birth,death};return definition;
}

void test_parent_lifecycle(){
    auto first=particles::CpuSystem(parent_system(),21,32);
    auto second=particles::CpuSystem(parent_system(),21,32);
    const auto initial=first.advance(std::numeric_limits<float>::min());
    expect(initial.spawned==2&&initial.child_instances_started==1&&first.live_child_instances()==1,
        "parent birth starts a separate link to the shared dependent emitter");
    expect(first.particles()[0].id!=first.particles()[1].id&&
        close(first.particles()[1].position.x,1)&&close(first.particles()[1].velocity.x,2.5F),
        "child position and capped inherited velocity use parent particle");
    const auto parent_id=first.particles()[0].id;
    static_cast<void>(first.advance(std::nextafter(0.25F,1.0F)));
    expect(first.particles().size()==3&&close(first.particles().back().position.x,2),
        "one child emitter follows its moving parent");
    const auto death=first.advance(0.25F);
    expect(death.killed==1&&death.child_instances_detached==1&&death.death_bursts==1&&
        death.spawned==2&&first.live_child_instances()==0,"death detaches once and emits one burst");
    expect(std::none_of(first.particles().begin(),first.particles().end(),[&](const particles::Particle& p){return p.id==parent_id;})&&
        close(first.particles().back().position.x,3)&&close(first.particles().back().velocity.x,1),
        "stable parent handle survives packing; death burst uses last position and inherited velocity");
    const auto after=first.advance(0.25F);
    expect(after.spawned==0&&after.death_bursts==0&&first.live_child_instances()==0,
        "detached child emitter stops and death burst does not repeat");
    // PS-10/PS-16: repeat the same positive initialization and shared-clock boundaries.
    for(const float step:{std::numeric_limits<float>::min(),std::nextafter(0.25F,1.0F),0.25F,0.25F})static_cast<void>(second.advance(step));
    expect(first.particles().size()==second.particles().size(),"fixed step reproduces lifecycle count");
    for(std::size_t i=0;i<first.particles().size();++i)
        expect(first.particles()[i].id==second.particles()[i].id&&
            close(first.particles()[i].position.x,second.particles()[i].position.x),
            "fixed seed reproduces stable IDs and lifecycle positions");

    auto paired=parent_system();paired.emitters[0].bursting=true;
    paired.emitters[0].particles_per_interval=2;
    particles::CpuSystem two_parents(std::move(paired),21,32);
    const auto births=two_parents.advance(std::numeric_limits<float>::min());
    expect(births.child_instances_started==2&&two_parents.live_child_instances()==2&&
        births.spawned==4,"two parent particles own links to one shared dependent clock");
    const auto deaths=two_parents.advance(0.5F);
    expect(deaths.child_instances_detached==2&&deaths.death_bursts==2&&
        two_parents.live_child_instances()==0,"each parent detaches and bursts independently");

    particles::CpuSystem bounded(parent_system(),21,2);
    static_cast<void>(bounded.advance(std::numeric_limits<float>::min()));
    const auto limited=bounded.advance(0.5F);
    expect(bounded.emitter_capacities()[0].reserved==1&&
        bounded.emitter_capacities()[1].reserved==1&&bounded.emitter_capacities()[2].reserved==0&&
        bounded.particles().size()==1&&bounded.particles()[0].emitter_index==1&&
        limited.requested==2&&limited.dropped_at_capacity==2&&limited.death_bursts==1,
        "PS-19 death bursts cannot borrow retired root slots under the explicit host budget");
    auto cycle=parent_system();cycle.emitters[0].creator_id=39;cycle.emitters[0].parent_emitter=1;
    particles::CpuSystem rejected(std::move(cycle),21,32);
    expect(rejected.advance(std::numeric_limits<float>::min()).spawned==0&&rejected.live_child_instances()==0,
        "programmatic parent cycles fail closed");
}

void test_parent_inward_speed_uses_system_origin(){
    auto definition=parent_system();
    definition.emitters[0].position.point={10,0,0};
    definition.emitters[1].position.point={0,0,0};
    definition.emitters[1].inward_speed=3;
    particles::CpuSystem cpu(std::move(definition),21,32);
    cpu.set_origin({5,0,0});
    const auto stats=cpu.advance(std::numeric_limits<float>::min());
    expect(stats.child_instances_started==1&&cpu.particles().size()==2,
        "displaced parent starts one attached child at zero local offset");
    expect(close(cpu.particles()[1].position.x,15)&&close(cpu.particles()[1].radial_velocity.x,3),
        "inward speed points from system translation even at zero child offset");
}

void test_parent_id_survives_live_compaction(){
    auto definition=parent_system();
    auto& root=definition.emitters[0];
    root.lifetime=0.75F;root.spawn_interval=0.5F;root.stop_time=0.5F;
    auto& birth=definition.emitters[1];
    birth.position.point={1,0,0};birth.spawn_interval=0.25F;birth.stop_time=0;
    particles::CpuSystem cpu(std::move(definition),21,64);
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    // PS-10: admit the second parent just beyond the shared root interval.
    static_cast<void>(cpu.advance(std::nextafter(0.5F,1.0F)));
    std::uint64_t surviving_parent_id{};
    std::size_t index_before{};
    for(std::size_t index=0;index<cpu.particles().size();++index){
        const auto& particle=cpu.particles()[index];
        if(particle.emitter_index==0&&particle.spawn_time==0.5F){
            surviving_parent_id=particle.id;index_before=index;break;
        }
    }
    expect(surviving_parent_id!=0&&index_before>0,"second parent begins behind particles in packed storage");
    const auto previous_max_id=cpu.particles().back().id;
    const auto step=cpu.advance(0.25F);
    expect(step.killed==1&&cpu.live_child_instances()==1,
        "first parent dies while second attached parent survives packing");
    std::size_t index_after=cpu.particles().size();
    for(std::size_t index=0;index<cpu.particles().size();++index)
        if(cpu.particles()[index].id==surviving_parent_id){index_after=index;break;}
    expect(index_after<index_before&&index_after<cpu.particles().size()&&
        close(cpu.particles()[index_after].position.x,1),
        "surviving moving parent keeps its stable ID after storage compaction");
    const auto child=std::find_if(cpu.particles().begin(),cpu.particles().end(),
        [&](const particles::Particle& particle){return particle.id>previous_max_id&&particle.emitter_index==1;});
    expect(child!=cpu.particles().end()&&close(child->position.x,2),
        "new child emission follows the surviving parent's position after packing");
}

void test_runtime_numeric_inputs(){
    const float nan=std::numeric_limits<float>::quiet_NaN();const float infinity=std::numeric_limits<float>::infinity();
    auto emitter=base_emitter();emitter.position.point={1,0,0};auto cpu=system_with(emitter);cpu.set_origin({2,0,0});cpu.set_origin({nan,0,0});static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));expect(close(cpu.particles().front().position.x,3),"non-finite origin is ignored");
    emitter=base_emitter();emitter.position.point={1,0,0};cpu=system_with(emitter);cpu.set_basis({{0,1,0},{-1,0,0},{0,0,1}});cpu.set_basis({{infinity,0,0},{0,1,0},{0,0,1}});static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));expect(close(cpu.particles().front().position.y,1),"non-finite basis is ignored");
    emitter=base_emitter();emitter.modifier_ids={54};emitter.wind_response=0.5F;cpu=system_with(emitter);cpu.set_wind({4,0,0});cpu.set_wind({infinity,0,0});static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));static_cast<void>(cpu.advance(1));expect(close(cpu.particles().front().velocity.x,2),"non-finite wind is ignored");
    emitter=base_emitter();emitter.texture_size=4;emitter.uv_index=constant(std::numeric_limits<float>::max());cpu=system_with(emitter);static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));const auto uv=cpu.particles().front().texcoords;expect(close(uv.x,0.5F)&&uv.y>0&&std::isfinite(uv.y),"out-of-int-range UV index converts safely");
    emitter=base_emitter();emitter.modifier_ids={10};emitter.acceleration.x=nan;cpu=system_with(emitter);expect(cpu.advance(std::numeric_limits<float>::min()).spawned==0,"non-finite programmatic emitter input is inactive");
    particles::CpuSystem clock_only(particles::SystemDefinition{});static_cast<void>(clock_only.advance(std::numeric_limits<float>::max()));static_cast<void>(clock_only.advance(std::numeric_limits<float>::max()));expect(clock_only.presentation_time()==std::numeric_limits<float>::max()&&std::isfinite(clock_only.presentation_time()),"finite deltas cannot overflow the accumulated clock");
}

bool same_particles(const particles::CpuSystem& a,const particles::CpuSystem& b){
    if(a.particles().size()!=b.particles().size())return false;
    for(std::size_t i=0;i<a.particles().size();++i){
        const auto& x=a.particles()[i];const auto& y=b.particles()[i];
        if(x.id!=y.id||x.emitter_index!=y.emitter_index||
           std::bit_cast<std::uint32_t>(x.position.x)!=std::bit_cast<std::uint32_t>(y.position.x)||
           std::bit_cast<std::uint32_t>(x.position.z)!=std::bit_cast<std::uint32_t>(y.position.z)||
           std::bit_cast<std::uint32_t>(x.size)!=std::bit_cast<std::uint32_t>(y.size)||
           std::bit_cast<std::uint32_t>(x.death_time)!=std::bit_cast<std::uint32_t>(y.death_time))return false;
    }
    return true;
}
particles::EmitterDefinition continuous_emitter(){
    auto emitter=base_emitter();emitter.spawn_interval=0.5F;emitter.stop_time=0;emitter.lifetime=1.2F;
    emitter.velocity.point={2,0,0};return emitter;
}

void test_detach_before_first_spawn_and_schedule_boundary(){
    auto cpu=system_with(continuous_emitter());
    expect(!cpu.detached()&&!cpu.finished(),"a fresh system is neither detached nor finished");
    cpu.detach();
    expect(cpu.detached()&&cpu.finished(),"detach before the first spawn finishes with nothing to drain");
    const auto first=cpu.advance(std::numeric_limits<float>::min());
    expect(first.spawned==0&&cpu.particles().empty()&&cpu.finished(),"detached root never emits its first batch");

    auto delayed=continuous_emitter();delayed.start_delay=1.0F;
    auto waiting=system_with(delayed);
    expect(waiting.advance(std::numeric_limits<float>::min()).spawned==0&&waiting.particles().empty()&&!waiting.finished(),
        "empty but scheduled system is not finished while attached");
    static_cast<void>(waiting.advance(0.5F));
    expect(!waiting.finished(),"still empty and scheduled, still not finished");

    auto attached=system_with(continuous_emitter());
    auto boundary=system_with(continuous_emitter());
    for(auto* system:{&attached,&boundary}){
        expect(system->advance(std::numeric_limits<float>::min()).spawned==1,"first root batch at time zero");
        expect(system->advance(0.25F).spawned==0,"no batch before the 0.5 s schedule");
    }
    boundary.detach();
    expect(same_particles(attached,boundary),"detach itself changes no particle");
    const auto kept=attached.advance(0.2501F);
    const auto stopped=boundary.advance(0.25F);
    expect(kept.spawned==1&&stopped.spawned==0&&stopped.killed==0,
        "detach just before a scheduled boundary cancels exactly that batch and kills nothing");
    expect(boundary.particles().size()==1&&!boundary.finished(),"the live particle keeps the system draining");
}

void test_detach_after_births_drains(){
    auto cpu=system_with(continuous_emitter(),7,64);
    static_cast<void>(cpu.advance(std::numeric_limits<float>::min()));
    for(int i=0;i<3;++i)static_cast<void>(cpu.advance(0.25F));
    const auto live=cpu.particles().size();
    expect(live==2,"two root batches are live before detach");
    const std::vector<particles::Particle> before(cpu.particles().begin(),cpu.particles().end());
    cpu.detach();
    std::size_t killed{},spawned{},frames{};
    bool moved=true,monotonic=true;
    std::size_t previous=live;
    while(!cpu.finished()&&frames<40){
        const auto stats=cpu.advance(0.25F);++frames;
        killed+=stats.killed;spawned+=stats.spawned;
        monotonic=monotonic&&cpu.particles().size()<=previous;previous=cpu.particles().size();
        for(const auto& particle:cpu.particles()){
            const auto original=std::find_if(before.begin(),before.end(),[&](const particles::Particle& p){return p.id==particle.id;});
            moved=moved&&original!=before.end()&&particle.position.x>original->position.x;
        }
        expect(cpu.finished()==cpu.particles().empty(),"finished exactly when the last particle is gone");
    }
    expect(cpu.finished()&&frames==4,"residual particles age out on their own lifetime");
    expect(spawned==0&&killed==live&&monotonic,"continuous root stops; every residual dies once, none invented");
    expect(moved,"residual particles keep moving and aging after detach");
    expect(cpu.advance(1).killed==0&&cpu.finished(),"a finished system stays finished");
}

void test_repeated_detach_is_stable(){
    auto definition=parent_system();
    definition.emitters[0].spawn_interval=0.2F;definition.emitters[0].stop_time=0;
    definition.emitters[0].lifetime_variation=0.5F;definition.emitters[1].size_variation=0.3F;
    particles::CpuSystem once(definition,33,64),again(definition,33,64);
    for(auto* system:{&once,&again}){static_cast<void>(system->advance(std::numeric_limits<float>::min()));static_cast<void>(system->advance(0.25F));}
    const auto highest=std::max_element(once.particles().begin(),once.particles().end(),
        [](const auto& a,const auto& b){return a.id<b.id;})->id;
    once.detach();again.detach();again.detach();
    for(int frame=0;frame<24;++frame){
        const auto a=once.advance(0.125F);
        again.detach();
        const auto b=again.advance(0.125F);
        expect(a.spawned==b.spawned&&a.killed==b.killed&&a.death_bursts==b.death_bursts&&
            a.child_instances_detached==b.child_instances_detached,"repeated detach does not change lifecycle counts");
        expect(same_particles(once,again),"repeated detach keeps IDs and random draws identical");
    }
    bool fresh_ids=true;
    for(const auto& particle:once.particles())
        if(particle.spawn_time>0.25F)fresh_ids=fresh_ids&&particle.id>highest;
    expect(fresh_ids,"particles born while draining take the next IDs; nothing is renumbered");
}

void test_detach_keeps_parent_chains(){
    auto definition=parent_system();
    definition.emitters[0].spawn_interval=0.2F;definition.emitters[0].stop_time=0;
    particles::CpuSystem cpu(definition,21,64);
    const auto initial=cpu.advance(std::numeric_limits<float>::min());
    expect(initial.spawned==2&&cpu.live_child_instances()==1,"parent and its first child exist before detach");
    const auto parent_id=cpu.particles()[0].id;
    cpu.detach();
    const auto trail=cpu.advance(std::nextafter(0.25F,1.0F));
    const auto parent=std::find_if(cpu.particles().begin(),cpu.particles().end(),
        [&](const particles::Particle& p){return p.id==parent_id;});
    expect(trail.spawned==1&&trail.child_instances_started==0&&cpu.live_child_instances()==1&&
        parent!=cpu.particles().end()&&close(parent->position.x,1),
        "detached root starts no parent, while the live parent moves and its birth child keeps spawning");
    const auto death=cpu.advance(0.25F);
    expect(death.killed==1&&death.child_instances_detached==1&&death.death_bursts==1&&death.spawned==2&&
        cpu.live_child_instances()==0,"parent death still detaches its child once and bursts once");
    std::size_t bursts=death.death_bursts,frames{};
    while(!cpu.finished()&&frames<40){
        const auto stats=cpu.advance(0.5F);++frames;
        bursts+=stats.death_bursts;
        expect(stats.spawned==0,"nothing new is spawned once the only parent is gone");
    }
    expect(cpu.finished()&&bursts==1,"children drain to finished without a second burst");

    particles::CpuSystem bounded(definition,21,2);
    static_cast<void>(bounded.advance(std::numeric_limits<float>::min()));
    bounded.detach();
    const auto limited=bounded.advance(0.5F);
    expect(bounded.particles().size()<=2&&limited.death_bursts==1&&limited.dropped_at_capacity>=1,
        "draining child and burst emission keep the hard particle bound");
    auto capped=definition;capped.emitters[0].bursting=true;capped.emitters[0].particles_per_interval=4;
    particles::CpuSystem instances(capped,21,2);
    const auto started=instances.advance(std::numeric_limits<float>::min());
    instances.detach();
    const auto drained=instances.advance(0.1F);
    expect(started.dropped_at_capacity==4&&started.child_instances_started==2&&
        drained.child_instances_started==0&&instances.live_child_instances()==2&&instances.particles().size()<=2,
        "capacity-bounded parents keep their instances and detach adds none");
}

void test_detach_fixed_seed_schedule(){
    const auto run=[](const int detach_frame){
        auto definition=parent_system();
        definition.emitters[0].spawn_interval=0.2F;definition.emitters[0].stop_time=0;
        definition.emitters[0].lifetime_variation=0.5F;definition.emitters[0].velocity.shape=particles::Shape::sphere;
        definition.emitters[0].velocity.radius_min=1;definition.emitters[0].velocity.radius_max=3;
        particles::CpuSystem cpu(definition,99,256);
        std::vector<std::uint64_t> hashes;
        for(int frame=0;frame<60;++frame){
            if(frame==detach_frame)cpu.detach();
            static_cast<void>(cpu.advance(frame==0?0.0F:1.0F/30.0F));
            std::uint64_t hash=0xcbf29ce484222325ULL;
            for(const auto& particle:cpu.particles()){
                for(const std::uint32_t word:{static_cast<std::uint32_t>(particle.id),
                    std::bit_cast<std::uint32_t>(particle.position.x),std::bit_cast<std::uint32_t>(particle.position.y),
                    std::bit_cast<std::uint32_t>(particle.position.z)}){hash^=word;hash*=0x100000001b3ULL;}
            }
            hashes.push_back(hash^(cpu.finished()?1U:0U));
        }
        return hashes;
    };
    const auto attached=run(-1),first=run(12),second=run(12);
    expect(first==second,"a fixed seed and detach frame repeat the same particle stream");
    expect(std::equal(first.begin(),first.begin()+12,attached.begin()),"frames before the detach frame match the attached run");
    expect(first[12]!=attached[12]||first[13]!=attached[13],"the detach frame changes the stream");
}

void test_skip_and_freeze_times(){
    // Parser: 0x33 skip and 0x32 freeze seconds; malformed or non-positive values disable them.
    {
        Bytes props;append(props,mini(0x33,scalar(1.5F)));append(props,mini(0x32,scalar(2.0F)));
        const auto loaded=particles::load_alo(legacy_fixture(scalar(2),scalar(4),vector3(0,0,0),props),"skip.alo");
        expect(loaded&&close(loaded.value().emitters[0].skip_time,1.5F)&&close(loaded.value().emitters[0].freeze_time,2.0F),
            "skip and freeze seconds decode");
        Bytes bad;append(bad,mini(0x33,scalar(-1.0F)));append(bad,mini(0x32,Bytes{std::byte{1}}));
        const auto ignored=particles::load_alo(legacy_fixture(scalar(2),scalar(4),vector3(0,0,0),bad),"bad-skip.alo");
        expect(ignored&&ignored.value().emitters[0].skip_time==0.0F&&ignored.value().emitters[0].freeze_time==0.0F,
            "negative or malformed skip and freeze values are ignored, not fatal");
    }
    // A continuous emitter (10/s, 1 s life) pre-simulated for 2 s is at steady state on its first frame.
    auto emitter=base_emitter();emitter.spawn_interval=1;emitter.particles_per_interval=10;emitter.stop_time=0;
    emitter.lifetime=1;emitter.velocity={.shape=particles::Shape::point,.point={0,0,3}};
    auto skipped=emitter;skipped.skip_time=2;
    auto plain=system_with(emitter,5,256);auto warm=system_with(skipped,5,256);
    const auto cold_first=plain.advance(std::numeric_limits<float>::min());const auto warm_first=warm.advance(std::numeric_limits<float>::min());
    expect(cold_first.spawned==1&&plain.particles().size()==1,"without a skip time the first frame holds one particle");
    expect(warm_first.spawned>=20&&warm.particles().size()>=9&&warm.particles().size()<=11,
        "a skip time pre-simulates to steady state before the first frame");
    expect(close(warm.presentation_time(),0.0F),"presentation time restarts at zero after the pre-roll");
    float highest{};bool spread{};
    for(const auto& particle:warm.particles()){
        expect(particle.spawn_time<=0.0F&&particle.death_time>0.0F,"pre-rolled particles were born before zero and are alive");
        highest=std::max(highest,particle.position.z);spread|=particle.position.z>1.0F;
    }
    expect(spread&&highest<=3.0F+0.001F,"pre-rolled particles moved for their pre-rolled age");
    auto again=system_with(skipped,5,256);static_cast<void>(again.advance(std::numeric_limits<float>::min()));
    expect(again.particles().size()==warm.particles().size()&&
        std::equal(again.particles().begin(),again.particles().end(),warm.particles().begin(),
            [](const particles::Particle& a,const particles::Particle& b){
                return a.id==b.id&&a.position.z==b.position.z&&a.death_time==b.death_time;}),
        "the pre-roll is deterministic for a fixed seed");
    // Freeze at the skip time: the field is filled, then nothing ages, moves, dies or spawns.
    auto field=skipped;field.freeze_time=2;
    auto frozen=system_with(field,5,256);static_cast<void>(frozen.advance(std::numeric_limits<float>::min()));
    const auto snapshot=std::vector<particles::Particle>(frozen.particles().begin(),frozen.particles().end());
    for(int frame=0;frame<90;++frame){
        const auto stats=frozen.advance(1.0F/30.0F);
        expect(stats.spawned==0&&stats.killed==0,"a frozen emitter neither spawns nor kills");
    }
    expect(frozen.particles().size()==snapshot.size()&&
        std::equal(snapshot.begin(),snapshot.end(),frozen.particles().begin(),
            [](const particles::Particle& a,const particles::Particle& b){
                return a.id==b.id&&a.position.z==b.position.z&&a.size==b.size&&a.color.w==b.color.w;}),
        "frozen particles keep their last state");
    // PS-08: equality advances, while a crossing rejects the complete update permanently.
    auto crossing=emitter;crossing.freeze_time=1;crossing.lifetime=10;
    auto long_step=system_with(crossing,5,256);
    auto split_step=system_with(crossing,5,256);
    static_cast<void>(long_step.advance(std::numeric_limits<float>::min()));static_cast<void>(split_step.advance(std::numeric_limits<float>::min()));
    const auto crossed=long_step.advance(2);
    const auto to_boundary=split_step.advance(1);
    const auto after_boundary=split_step.advance(1);
    expect(crossed.spawned==0&&to_boundary.spawned>0&&after_boundary.spawned==0&&
        crossed.killed==0&&after_boundary.killed==0,
        "freeze equality admits births but a crossing skips the full update");
    expect(close(long_step.presentation_time(),2)&&long_step.particles().size()==1&&
        close(long_step.particles().front().position.z,0)&&close(split_step.particles().front().position.z,3),
        "a crossed update keeps the prior state rather than splitting at the boundary");
    expect(long_step.advance(0.25F).spawned==0&&close(long_step.particles().front().position.z,0),
        "a smaller later delta cannot unfreeze the emitter");
    auto linked=crossing;linked.translater_id=26;
    auto held=system_with(linked,5,256);held.set_origin({});static_cast<void>(held.advance(std::numeric_limits<float>::min()));
    static_cast<void>(held.advance(2));held.set_origin({10,0,0});held.follow_emitter();
    expect(close(held.particles().front().position.x,0), "frozen linked particles stop following their owner");
    // An emitter without a skip time in a pre-rolled system starts at zero, exactly as before.
    particles::SystemDefinition mixed;mixed.emitters={skipped,emitter};
    particles::CpuSystem both(mixed,5,512);static_cast<void>(both.advance(std::numeric_limits<float>::min()));
    const auto late=std::count_if(both.particles().begin(),both.particles().end(),
        [](const particles::Particle& particle){return particle.emitter_index==1;});
    expect(late==1,"an emitter without a skip time is not pre-simulated with its neighbour");
}

void test_prewarm_boundaries(){
    auto emitter=base_emitter();emitter.velocity.point={1,0,0};
    const auto probe=[&](const float target,const float caller_delta){
        auto warm=emitter;warm.skip_time=target;
        auto cpu=system_with(warm,7,256);static_cast<void>(cpu.advance(caller_delta==0?std::numeric_limits<float>::min():caller_delta));return cpu;
    };
    const auto zero=probe(0,0),equal=probe(0.1F,0),between=probe(0.15F,0),next=probe(0.2F,0);
    expect(zero.particles().front().spawn_time==0&&close(zero.particles().front().position.x,0),
        "zero prewarm target runs no presimulation step");
    expect(equal.particles().front().spawn_time==-0.2F&&close(equal.particles().front().position.x,0.2F),
        "inclusive 0.1-second target runs two 0.1-second steps");
    expect(between.particles().front().spawn_time==-0.2F&&close(between.particles().front().position.x,0.2F),
        "a target between steps overshoots to the next complete step");
    expect(next.particles().front().spawn_time==-0.3F&&close(next.particles().front().position.x,0.3F),
        "exact equality at the second prewarm step admits one more step");
    const auto ordinary=probe(0.1F,0.05F);
    expect(close(ordinary.presentation_time(),0.05F)&&close(ordinary.particles().front().position.x,0.25F),
        "the admitted caller update runs after prewarm");
    auto warm=emitter;warm.skip_time=0.2F;warm.freeze_time=0.15F;
    auto admitted=system_with(warm,7,256);static_cast<void>(admitted.advance(0.05F));
    expect(close(admitted.particles().front().position.x,0.35F),
        "first-update freeze admission precedes prewarm and is not repeated inside it");
    static_cast<void>(admitted.advance(0.01F));
    expect(close(admitted.particles().front().position.x,0.35F), "the next crossing freezes the prewarmed state");
    auto rejected=system_with(warm,7,256);
    expect(rejected.advance(0.2F).spawned==0&&rejected.particles().empty(),
        "a rejected first update freezes before prewarming or births");
    expect(rejected.advance(0.01F).spawned==0, "a rejected first update remains frozen");
    auto boundary=emitter;boundary.skip_time=0.1F;boundary.freeze_time=0.2F;boundary.translater_id=26;
    auto neighbor=emitter;neighbor.skip_time=10;neighbor.lifetime=100;
    particles::SystemDefinition mixed;mixed.emitters={boundary,neighbor};
    particles::CpuSystem both(mixed,7,256);both.set_origin({});static_cast<void>(both.advance(std::numeric_limits<float>::min()));
    static_cast<void>(both.advance(std::numeric_limits<float>::min()));both.set_origin({1,0,0});both.follow_emitter();
    const auto first=std::find_if(both.particles().begin(),both.particles().end(),
        [](const auto& particle){return particle.emitter_index==0;});
    expect(first!=both.particles().end()&&first->position.x>1,
        "freeze equality uses the emitter's own elapsed time across unequal prewarm targets");
    const auto fixed=first->position.x;
    static_cast<void>(both.advance(0.05F));
    const auto after=std::find_if(both.particles().begin(),both.particles().end(),
        [](const auto& particle){return particle.emitter_index==0;});
    const auto moving=std::find_if(both.particles().begin(),both.particles().end(),
        [](const auto& particle){return particle.emitter_index==1;});
    expect(after->position.x==fixed&&moving->position.x>9.9F,
        "one frozen emitter leaves its neighbor advancing");
}

void test_prewarmed_capacity() {
    // Retail-shaped two-emitter field: 33 + 50 births/s, ten-second
    // lifetime, pre-roll and freeze. The old 256 cap discarded most births.
    auto inner = base_emitter();
    inner.bursting = false; inner.spawn_interval = 1; inner.particles_per_interval = 33;
    inner.stop_time = 0; inner.lifetime = 10; inner.skip_time = 10; inner.freeze_time = 10;
    auto outer = inner; outer.particles_per_interval = 50;
    particles::SystemDefinition field; field.emitters = {inner, outer};
    const auto capacity = particles::prewarmed_capacity(field, 256);
    expect(capacity >= 830 && capacity < 850, "authored rates size the field, with bounded scheduling slack");
    particles::CpuSystem old(field, 238, 256), full(field, 238, capacity), repeat(field, 238, capacity);
    expect(old.advance(std::numeric_limits<float>::min()).dropped_at_capacity > 500, "the old per-proxy cap reproduces missing pebbles");
    expect(full.advance(std::numeric_limits<float>::min()).dropped_at_capacity == 0 && full.particles().size() >= 825,
        "the source-derived budget retains the prewarmed field");
    static_cast<void>(repeat.advance(std::numeric_limits<float>::min()));
    expect(full.particles().size() == repeat.particles().size() &&
        std::equal(full.particles().begin(), full.particles().end(), repeat.particles().begin(),
            [](const auto& a, const auto& b) { return a.id == b.id && a.position.x == b.position.x
                && a.position.y == b.position.y && a.position.z == b.position.z; }),
        "field allocation preserves seeded placement determinism");
    for (int tick = 0; tick < 60; ++tick)
        expect(full.advance(1.0F / 30.0F).dropped_at_capacity == 0, "frozen population stays within capacity");
    expect(particles::prewarmed_capacity(field, 256, 100) == 100, "automatic allocation respects its safety cap");
    field.emitters[0].creator_id = 39;
    expect(particles::prewarmed_capacity(field, 256) == 256, "parent emitters retain caller policy");
    field.emitters = {inner}; field.emitters[0].skip_time = 0;
    expect(particles::prewarmed_capacity(field, 256) == 256, "cold effects retain caller policy");
    field.emitters[0] = inner; field.emitters[0].freeze_time = 0;
    field.emitters[0].lifetime_variation = 1;
    field.emitters[0].lifetime_range = particles::EmitterDefinition::LifetimeRange{1,14};
    const auto varied_capacity = particles::prewarmed_capacity(field, 256);
    field.emitters[0].lifetime_variation = 0;
    expect(varied_capacity == 335 && varied_capacity == particles::prewarmed_capacity(field, 256),
        "post-load maximum age bounds prewarming independently of raw bounds or positive variation");
    field.emitters[0].particles_per_interval = std::numeric_limits<float>::max();
    expect(particles::prewarmed_capacity(field, 256) == 65536, "huge finite rates saturate before integer conversion");
    auto short_freeze=inner;short_freeze.particles_per_interval=500;short_freeze.skip_time=5;short_freeze.freeze_time=0.1F;
    field.emitters={short_freeze};
    const auto budget=particles::continuous_capacity(field,256);
    particles::CpuSystem warm(field,7,budget);
    expect(budget>2500&&warm.advance(std::numeric_limits<float>::min()).dropped_at_capacity==0&&warm.particles().size()>2400,
        "capacity includes admitted prewarm even when its target exceeds freeze");
}

void test_continuous_mesh_capacity() {
    // pi_damage_elec_SD00: independent random-surface births, 3500/s,
    // saved age 0.5 s, no pre-roll. The old allocation loses most of the arcs.
    auto emitter = mesh_emitter(particles::MeshSpawnMode::random_surface);
    emitter.bursting = false; emitter.particles_per_interval = 3500; emitter.spawn_interval = 1;
    emitter.stop_time = 0; emitter.lifetime = 0.5F;
    emitter.lifetime_range = particles::EmitterDefinition::LifetimeRange{0.5F, 0.5F};
    particles::SystemDefinition system; system.emitters = {emitter};
    const auto capacity = particles::continuous_mesh_capacity(system, 256);
    expect(capacity >= 1750 && capacity < 2000, "cold mesh population is sized from authored rate and age");
    auto mesh = asymmetric_mesh(); mesh.geometry.submeshes.erase(mesh.geometry.submeshes.begin());
    particles::CpuSystem old(system, 1187, 256, mesh), full(system, 1187, capacity, mesh);
    std::size_t old_drops = 0;
    for (int tick = 0; tick < 60; ++tick) {
        old_drops += old.advance(1.0F / 30.0F).dropped_at_capacity;
        expect(full.advance(1.0F / 30.0F).dropped_at_capacity == 0,
            "derived allocation keeps the electrical stream without dropped births");
    }
    expect(old_drops > 5000 && old.particles().size() <= 256,
        "the old allocation reproduces severe electrical density loss");
    expect(full.particles().size() >= 1700 && full.particles().size() <= capacity,
        "authored steady electrical population fits the bounded pool");
    full.detach();
    for (int tick = 0; tick < 17; ++tick)
        expect(full.advance(1.0F / 30.0F).spawned == 0, "stun end stops births with the larger pool");
    expect(full.finished(), "the full electrical population drains within its authored age");
    expect(particles::continuous_mesh_capacity(system, 256, 100) == 100, "caller limit remains authoritative");
    system.emitters[0].particles_per_interval = std::numeric_limits<float>::max();
    expect(particles::continuous_mesh_capacity(system, 256) == 5003,
        "untrusted authored rates saturate at FoC's independent-emitter ceiling");
    system.emitters[0] = emitter; system.emitters[0].mesh_mode = particles::MeshSpawnMode::every_vertex;
    expect(particles::continuous_mesh_capacity(system, 256) == 256, "vertex multiplicity retains explicit policy");
    system.emitters[0] = emitter; system.emitters[0].parent_emitter = 0;
    expect(particles::continuous_mesh_capacity(system, 256) == 256, "parent multiplicity retains explicit policy");
    system.emitters[0] = emitter; system.emitters[0].bursting = true;
    expect(particles::continuous_mesh_capacity(system, 256) == 256, "bursts retain explicit policy");
    system.emitters[0] = emitter; system.emitters[0].lifetime_range->maximum = std::numeric_limits<float>::quiet_NaN();
    expect(particles::continuous_mesh_capacity(system, 256) == 256, "invalid saved age retains bounded fallback");
}

void test_continuous_capacity() {
    auto shape = base_emitter();
    shape.bursting = false; shape.stop_time = 0; shape.skip_time = 0;
    shape.spawn_interval = 1; shape.particles_per_interval = 600; shape.lifetime = 1;
    auto mesh = mesh_emitter(particles::MeshSpawnMode::random_surface);
    mesh.bursting = false; mesh.stop_time = 0; mesh.particles_per_interval = 100;
    mesh.spawn_interval = 1; mesh.lifetime = 5;
    particles::SystemDefinition system; system.emitters = {shape, mesh};
    const auto capacity = particles::continuous_capacity(system, 256);
    expect(capacity >= 1100 && capacity < 1150, "cold shape and mesh bounds are summed independently");
    auto binding = asymmetric_mesh(); binding.geometry.submeshes.erase(binding.geometry.submeshes.begin());
    particles::CpuSystem old(system, 119, 256, binding), full(system, 119, capacity, binding);
    std::size_t drops = 0;
    for (int tick = 0; tick < 180; ++tick) {
        drops += old.advance(1.0F / 30.0F).dropped_at_capacity;
        expect(full.advance(1.0F / 30.0F).dropped_at_capacity == 0,
            "mixed independent streams retain their authored populations without prewarming");
    }
    expect(drops > 2000 && full.particles().size() > 1050,
        "generic fallback reproduces population loss beyond the ion-only case");
    full.detach();
    for (int tick = 0; tick < 155; ++tick) static_cast<void>(full.advance(1.0F / 30.0F));
    expect(full.finished(), "larger mixed population drains without extending authored age");
    expect(particles::continuous_capacity(system, 256, 100) == 100, "explicit caller ceiling wins");
    shape.particles_per_interval = std::numeric_limits<float>::max();
    mesh.particles_per_interval = std::numeric_limits<float>::max();
    system.emitters = {shape, mesh};
    expect(particles::continuous_capacity(system, 256) == 10006,
        "5003 ceiling is applied per emitter before the system sum");
    expect(particles::continuous_capacity(system, 256, 6000) == 6000,
        "system sum saturates at the caller ceiling");
    auto excluded = shape; excluded.bursting = true; excluded.creator_id = 39;
    system.emitters = {mesh, excluded};
    expect(particles::continuous_capacity(system, 256) == 5259,
        "dependent burst retains fallback reserve beside an independent mesh bound");
    excluded.bursting = false; excluded.creator_id = 39; system.emitters[1] = excluded;
    expect(particles::continuous_capacity(system, 256) == 5259, "parent family retains fallback reserve");
    system.emitters[1] = mesh; system.emitters[1].mesh_mode = particles::MeshSpawnMode::every_vertex;
    expect(particles::continuous_capacity(system, 256) == 5259, "every-vertex multiplicity is not inferred");
    system.emitters[1] = shape; system.emitters[1].parent_emitter = 0;
    expect(particles::continuous_capacity(system, 256) == 5259, "parent reference excludes shape multiplicity");
    system.emitters = {shape}; system.emitters[0].lifetime_range =
        particles::EmitterDefinition::LifetimeRange{1, std::numeric_limits<float>::quiet_NaN()};
    expect(particles::continuous_capacity(system, 256) == 256, "invalid saved bounds retain fallback");
    system.emitters[0] = shape; system.emitters[0].cpu_ready = false;
    expect(particles::continuous_capacity(system, 256) == 256, "unsupported definitions retain fallback");
    system.emitters[0] = shape; system.emitters[0].particles_per_interval = 33;
    system.emitters[0].lifetime = 10;
    system.emitters[0].lifetime_range = particles::EmitterDefinition::LifetimeRange{1,14};
    expect(particles::continuous_capacity(system, 256) == 333, "post-load maximum age governs cold allocation");
    system.emitters[0].lifetime_range = particles::EmitterDefinition::LifetimeRange{14,14,true};
    expect(particles::continuous_capacity(system, 256) == 465, "true constant default governs effective allocation");
    system.emitters[0].lifetime_range = particles::EmitterDefinition::LifetimeRange{1,14};
    system.emitters[0].freeze_time = 2;
    expect(particles::continuous_capacity(system, 256) == 256, "freeze bounds the accumulated population");
    system.version = particles::AloParticleVersion::plugin_v2;
    expect(particles::continuous_capacity(system, 256) == 256, "unsupported version retains caller policy");
}

void test_burst_capacity() {
    // PS-18: the seven independent burst shapes of a large explosion.
    // Synthetic definitions exercise scheduling and ages without game assets.
    particles::SystemDefinition explosion;
    const float counts[] = {1, 50, 2, 150, 1, 10, 20};
    const float intervals[] = {2, 2, 0.2F, 2, 2, 0.33F, 1};
    const float stops[] = {2, 2, 0.6F, 2, 2, 1.98F, 1};
    const float ages[] = {2, 2, 2, 2, 2, 3, 4};
    const float variations[] = {0, 1, 0.5F, 0.5F, 0, 0.38F, 0.82F};
    for (std::size_t index = 0; index < 7; ++index) {
        auto emitter = base_emitter();
        emitter.bursting = true; emitter.particles_per_interval = counts[index];
        emitter.spawn_interval = intervals[index]; emitter.stop_time = stops[index];
        emitter.lifetime = ages[index]; emitter.lifetime_variation = variations[index];
        explosion.emitters.push_back(emitter);
    }
    const auto capacity = particles::continuous_capacity(explosion, 256);
    expect(capacity == 626, "overlapping whole-burst bounds sum independently of the fallback");
    for (const auto seed : {1234U, 4567U, 10001U}) {
        particles::CpuSystem old(explosion, seed, 256), full(explosion, seed, capacity);
        const auto* storage = full.particles().data();
        std::size_t peak = 0;
        for (int tick = 0; tick < 240; ++tick) {
            static_cast<void>(old.advance(1.0F / 30.0F));
            expect(full.advance(1.0F / 30.0F).dropped_at_capacity == 0,
                "all explosion births fit the bounded pool across fixed seeds");
            expect(full.particles().data() == storage, "births never grow the preallocated particle storage");
            peak = std::max(peak, full.particles().size());
        }
        expect(old.total_dropped() > 0 && peak > 256 && peak <= capacity,
            "the old fallback loses overlapping explosion births");
        full.detach();
        expect(full.finished(), "the larger pool does not prolong explosion lifetime");
    }
    expect(particles::continuous_capacity(explosion, 256, 100) == 100,
        "explicit system ceiling still bounds burst pools");

    auto burst = base_emitter();
    burst.bursting = true; burst.stop_time = 0;
    burst.particles_per_interval = 30.75F; burst.spawn_interval = 1; burst.lifetime = 0.1F;
    particles::SystemDefinition system; system.emitters = {burst};
    expect(particles::continuous_capacity(system, 1) == 30,
        "isolated bursts reserve the truncated authored event count");
    burst.particles_per_interval = 100; burst.spawn_interval = 0.6F; burst.lifetime = 1;
    system.emitters = {burst};
    const auto overlapping = particles::continuous_capacity(system, 1);
    expect(overlapping == 200, "fractional overlap retains whole bursts rather than averaging their rate");
    particles::CpuSystem repeated(system, 17, overlapping);
    for (int tick = 0; tick < 600; ++tick)
        expect(repeated.advance(1.0F / 30.0F).dropped_at_capacity == 0,
            "repeated bursts remain bounded by live overlap");
    burst.particles_per_interval = std::numeric_limits<float>::max();
    burst.spawn_interval = std::numeric_limits<float>::min();
    system.emitters = {burst, burst};
    expect(particles::continuous_capacity(system, 256) == 10006,
        "huge finite burst populations clamp before integer conversion, per emitter");
    expect(particles::continuous_capacity(system, 256, 6000) == 6000,
        "summed burst ceilings respect the caller ceiling");
    system.emitters[1].creator_id = 39;
    expect(particles::continuous_capacity(system, 256) == 5259,
        "unsupported parent bursts keep one fallback beside independent reserves");
    burst = mesh_emitter(particles::MeshSpawnMode::random_surface);
    burst.bursting = true; burst.particles_per_interval = 400; burst.spawn_interval = 2; burst.lifetime = 0.5F;
    system.emitters = {burst};
    expect(particles::continuous_capacity(system, 256) == 400,
        "independent random mesh bursts use the same authored count bound");
    system.emitters[0].mesh_mode = particles::MeshSpawnMode::every_vertex;
    expect(particles::continuous_capacity(system, 256) == 256,
        "every-vertex bursts retain explicit multiplicity policy");
    system.emitters[0] = burst; system.emitters[0].spawn_interval = 0;
    expect(particles::continuous_capacity(system, 256) == 256, "invalid burst intervals retain fallback");
    system.emitters[0] = burst; system.emitters[0].cpu_ready = false;
    expect(particles::continuous_capacity(system, 256) == 256, "unsupported burst definitions retain fallback");

    // A pool above the old initial reserve must also be allocated before births.
    system.emitters = {burst}; system.emitters[0].particles_per_interval = 4500;
    const auto large = particles::continuous_capacity(system, 256);
    auto binding = asymmetric_mesh();
    binding.geometry.submeshes.erase(binding.geometry.submeshes.begin());
    particles::CpuSystem preallocated(system, 71, large, binding);
    const auto* storage = preallocated.particles().data();
    expect(preallocated.advance(std::numeric_limits<float>::min()).spawned == 4500 && preallocated.particles().data() == storage,
        "a burst above 4096 births does not reallocate its particle pool");
}

void test_independent_emitter_capacity() {
    auto dense = base_emitter(); dense.bursting = true; dense.stop_time = 0;
    dense.particles_per_interval = 100; dense.spawn_interval = 0.1F; dense.lifetime = 10;
    auto sparse = dense; sparse.particles_per_interval = 3; sparse.spawn_interval = 20;
    particles::SystemDefinition system; system.emitters = {dense, sparse};
    particles::CpuSystem cpu(system, 42, 16);
    const auto capacities = cpu.emitter_capacities();
    expect(capacities[0].native_requested == 5003 && capacities[1].native_requested == 3,
        "PS-18 base clamp and long-interval event count are independent");
    expect(capacities[0].reserved == 13 && capacities[1].reserved == 3,
        "explicit host budget partitions before births without starvation");
    const auto memory = cpu.reserved_memory_bytes();
    expect(memory >= 16 * sizeof(particles::Particle)
        && memory <= 16 * (sizeof(particles::Particle) + sizeof(std::size_t)),
        "independent roots reserve bounded particle/slot payload without unused dependent scratch");
    const auto first = cpu.advance(std::numeric_limits<float>::min());
    expect(first.requested == 103 && first.spawned == 16 && first.dropped_at_capacity == 87,
        "requested and dropped counts include every emitter at exhaustion");
    const auto ids = std::vector<particles::Particle>(cpu.particles().begin(), cpu.particles().end());
    // PS-10: subsequent emission enters only after the interval boundary.
    const auto second = cpu.advance(std::nextafter(0.1F,1.0F));
    expect(second.spawned == 0 && second.dropped_at_capacity == 100,
        "exhausted emitter drops new births without oldest replacement");
    expect(std::equal(ids.begin(), ids.end(), cpu.particles().begin(),
        [](const auto& a, const auto& b) { return a.id == b.id && a.draw_slot == b.draw_slot; }),
        "capacity pressure preserves live IDs and stable emitter slots");
    expect(cpu.reserved_memory_bytes() == memory, "exhaustion cannot grow reserved memory");
    auto parent = sparse; parent.particles_per_interval = 2;
    auto child = dense; child.creator_id = 39; child.parent_emitter = 1;
    auto mesh = dense; mesh.creator_id = 35; mesh.mesh_mode = particles::MeshSpawnMode::every_vertex;
    mesh.parent_emitter = 0;
    system.emitters = {child, parent, mesh};
    const auto plan = particles::emitter_capacity_plan(system, 4, 65536);
    expect(plan[0].native_requested == 10006 && plan[2].native_requested == 200240072,
        "5003 base clamp precedes forward-parent and every-vertex multiplication");
    expect(plan[0].reserved + plan[1].reserved + plan[2].reserved == 65536,
        "dependent and vertex multiplicities stay inside the explicit host memory budget");
    const auto overflow = particles::emitter_capacity_plan(system, std::numeric_limits<std::size_t>::max(), 128);
    expect(overflow[2].native_requested == std::numeric_limits<std::size_t>::max()
        && overflow[0].reserved + overflow[1].reserved + overflow[2].reserved == 128,
        "multiplication saturates before host budgeting without wraparound");
    system.emitters[1].parent_emitter = 0;
    const auto cyclic = particles::emitter_capacity_plan(system, 4, 65536);
    expect(cyclic[0].reserved == 0 && cyclic[1].reserved == 0 && cyclic[2].reserved == 0,
        "cyclic capacity links fail closed");
    mesh.parent_emitter = particles::EmitterDefinition::no_parent;
    mesh.particles_per_interval = 6000; mesh.spawn_interval = 20; mesh.lifetime = 1;
    auto binding = asymmetric_mesh();
    std::size_t vertices{};
    for (const auto& submesh : binding.geometry.submeshes) vertices += submesh.vertices.size();
    system.emitters = {mesh};
    particles::CpuSystem all_vertices(system, 1, 65536, binding);
    const auto vertex_births = all_vertices.advance(std::numeric_limits<float>::min());
    expect(all_vertices.emitter_capacities()[0].native_requested == 5003 * vertices
        && vertex_births.requested == 6000 * vertices && vertex_births.spawned == 5003 * vertices
        && vertex_births.dropped_at_capacity == 997 * vertices,
        "every-vertex burst exposes clamp-before-multiply admission and dropped count");
}

} // namespace

int main(const int argc,char** argv){
    test_walk_trails();
    test_walk_births();
    test_mesh_birth_frames();
    test_walk_rotation();
    test_walk_color();
    test_walk_shapes();
    test_walk_motion();
    test_independent_emitter_capacity();
    if(argc==3&&std::string(argv[1])=="--inspect-alo"){
        std::ifstream input(argv[2],std::ios::binary);std::vector<char> raw((std::istreambuf_iterator<char>(input)),{});Bytes bytes(raw.size());
        for(std::size_t index=0;index<raw.size();++index)bytes[index]=static_cast<std::byte>(raw[index]);
        const auto loaded=particles::load_alo(bytes,argv[2]);if(!loaded){std::cerr<<loaded.error().code<<": "<<loaded.error().message<<'\n';return 1;}
        std::size_t ready{};for(const auto& emitter:loaded.value().emitters)if(emitter.cpu_ready)++ready;
        std::cout<<"emitters="<<loaded.value().emitters.size()<<" cpu_ready="<<ready<<'\n';return 0;
    }
    test_catalog_and_parser();test_malformed_numeric_inputs();test_mesh_parser();test_mesh_cpu_sampling();test_mesh_frames_and_capacity();test_mesh_surface_size_offset();test_lifetime_variation();test_creator_killers_and_translation();test_emitter_translater_follows_rotation();test_emitter_translater_local_acceleration();test_modifier_families();test_uv_cell_is_integer_part();test_resource_bound();test_parent_lifecycle();test_parent_id_survives_live_compaction();test_parent_inward_speed_uses_system_origin();test_runtime_numeric_inputs();
    test_detach_before_first_spawn_and_schedule_boundary();test_detach_after_births_drains();test_repeated_detach_is_stable();test_detach_keeps_parent_chains();test_detach_fixed_seed_schedule();test_skip_and_freeze_times();test_prewarm_boundaries();test_prewarmed_capacity();test_continuous_mesh_capacity();test_continuous_capacity();test_burst_capacity();std::cout<<"particle CPU contracts passed\n";
}
