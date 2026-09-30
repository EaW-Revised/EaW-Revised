#include "asset_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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

struct PackedVector { std::uint16_t x{}, y{}, z{}; };
struct PackedQuat { std::int16_t x{}, y{}, z{}, w{}; };
struct TrackInfo {
    AnimationTrack track;
    Vec3f trans_offset{}, trans_scale{}, scale_offset{}, scale_scale{};
    std::uint16_t trans_index{0xffffU}, scale_index{0xffffU}, rot_index{0xffffU};
    Vec4f default_rotation{};
    std::vector<PackedVector> v1_trans, v1_scale;
    std::vector<PackedQuat> v1_rot;
    std::vector<bool> visibility;
};

bool packed_vector(Reader& reader, PackedVector& value) { return reader.u16(value.x)&&reader.u16(value.y)&&reader.u16(value.z); }
bool packed_quat(Reader& reader, PackedQuat& value) { return reader.i16(value.x)&&reader.i16(value.y)&&reader.i16(value.z)&&reader.i16(value.w); }
Vec3f unpack(const PackedVector& p, const Vec3f& offset, const Vec3f& scale) {
    return {offset.x + float(p.x)*scale.x, offset.y + float(p.y)*scale.y, offset.z + float(p.z)*scale.z};
}
Vec4f unpack(const PackedQuat& p) {
    constexpr float divisor=32767.0F;
    return {float(p.x)/divisor,float(p.y)/divisor,float(p.z)/divisor,float(p.w)/divisor};
}
bool read_string(const Mini& mini, std::string& value) { Reader r(mini.payload); return r.string(value)&&r.remaining()==0; }

core::Result<TrackInfo> parse_track(const Chunk& group, const Source& source, const AnimationVersion version, const std::uint32_t frames) {
    if(!group.group||group.children.empty()||group.children[0].type!=0x1003) return fail<TrackInfo>(source,"invalid animation track group",group.header_offset);
    auto minis=detail::parse_minis(group.children[0],source);if(!minis)return core::Result<TrackInfo>::failure(minis.error());
    TrackInfo info; std::size_t pos=0;
    auto require=[&](std::uint8_t type)->const Mini*{if(pos>=minis.value().size()||minis.value()[pos].type!=type)return nullptr;return &minis.value()[pos++];};
    const Mini* name=require(4);const Mini* index=require(5);if(!name||!index||!read_string(*name,info.track.bone_name))return fail<TrackInfo>(source,"animation track lacks valid name/index",group.children[0].header_offset);
    {Reader r(index->payload);if(!r.u32(info.track.bone_index)||r.remaining()!=0)return fail<TrackInfo>(source,"invalid animation bone index",index->offset);}
    if(pos<minis.value().size()&&minis.value()[pos].type==10)++pos;
    const Mini* to=require(6);const Mini* ts=require(7);const Mini* so=require(8);const Mini* ss=require(9);
    auto read_vec=[](const Mini* m,Vec3f& value){if(!m)return false;Reader r(m->payload);return r.vec3(value)&&r.remaining()==0;};
    if(!read_vec(to,info.trans_offset)||!read_vec(ts,info.trans_scale)||!read_vec(so,info.scale_offset)||!read_vec(ss,info.scale_scale))return fail<TrackInfo>(source,"invalid animation vector metadata",group.children[0].header_offset);
    if(version==AnimationVersion::v2){
        const Mini* ti=require(14);const Mini* si=require(15);const Mini* ri=require(16);const Mini* dr=require(17);
        auto read_idx=[](const Mini* m,std::uint16_t& value){if(!m)return false;Reader r(m->payload);return r.u16(value)&&r.remaining()==0;};
        PackedQuat pq;Reader qr(dr?dr->payload:std::span<const std::byte>{});
        if(!read_idx(ti,info.trans_index)||!read_idx(si,info.scale_index)||!read_idx(ri,info.rot_index)||!packed_quat(qr,pq)||qr.remaining()!=0)return fail<TrackInfo>(source,"invalid v2 animation indices/default rotation",group.children[0].header_offset);
        if(info.trans_index!=0xffffU){if(info.trans_index%3U!=0)return fail<TrackInfo>(source,"unaligned translation block index",ti->offset,diagnostic_codes::index);info.trans_index/=3U;}
        if(info.scale_index!=0xffffU){if(info.scale_index%3U!=0)return fail<TrackInfo>(source,"unaligned scale block index",si->offset,diagnostic_codes::index);info.scale_index/=3U;}
        if(info.rot_index!=0xffffU){if(info.rot_index%4U!=0)return fail<TrackInfo>(source,"unaligned rotation block index",ri->offset,diagnostic_codes::index);info.rot_index/=4U;}
        info.default_rotation=unpack(pq);
    }
    if(pos!=minis.value().size())return fail<TrackInfo>(source,"unsupported essential animation metadata mini-chunk",minis.value()[pos].offset,diagnostic_codes::unsupported);
    std::size_t child=1;
    auto vectors=[&](std::uint32_t type,std::vector<PackedVector>& out)->core::Result<void>{
        if(child>=group.children.size()||group.children[child].type!=type)return core::Result<void>::success();
        Reader r(group.children[child].payload,static_cast<std::size_t>(group.children[child].payload_offset));if(r.remaining()!=static_cast<std::size_t>(frames)*6U)return core::Result<void>::failure(detail::error(source,diagnostic_codes::bounds,"animation vector track byte count mismatch",group.children[child].header_offset));
        out.resize(frames);for(auto& v:out)if(!packed_vector(r,v))return core::Result<void>::failure(detail::error(source,diagnostic_codes::truncated,"truncated animation vector track",r.absolute()));++child;return core::Result<void>::success();};
    if(version==AnimationVersion::v1){
        auto a=vectors(0x1004,info.v1_trans);if(!a)return core::Result<TrackInfo>::failure(a.error());auto b=vectors(0x1005,info.v1_scale);if(!b)return core::Result<TrackInfo>::failure(b.error());
        if(child<group.children.size()&&group.children[child].type==0x1006){Reader r(group.children[child].payload,static_cast<std::size_t>(group.children[child].payload_offset));const auto count=r.remaining()==8U?1U:frames;if(r.remaining()!=static_cast<std::size_t>(count)*8U)return fail<TrackInfo>(source,"rotation track byte count mismatch",group.children[child].header_offset,diagnostic_codes::bounds);info.v1_rot.resize(count);for(auto& q:info.v1_rot)packed_quat(r,q);++child;}
    }
    info.visibility.assign(frames,true);
    if(child<group.children.size()&&group.children[child].type==0x1007){const auto needed=(static_cast<std::size_t>(frames)+7U)/8U;if(group.children[child].payload.size()!=needed)return fail<TrackInfo>(source,"visibility bitset byte count mismatch",group.children[child].header_offset,diagnostic_codes::bounds);for(std::size_t i=0;i<frames;++i)info.visibility[i]=(std::to_integer<unsigned>(group.children[child].payload[i/8U])&(1U<<(i%8U)))!=0;++child;}
    if(child<group.children.size()&&group.children[child].type==0x1008)++child;
    if(child!=group.children.size())return fail<TrackInfo>(source,"unsupported essential animation track chunk",group.children[child].header_offset,diagnostic_codes::unsupported);
    return core::Result<TrackInfo>::success(std::move(info));
}
} // namespace

core::Result<Animation> load_animation(const std::span<const std::byte> bytes, Source source) {
    if(bytes.size()>detail::max_file_size)return core::Result<Animation>::failure(detail::error(source,diagnostic_codes::limit,"animation exceeds 512 MiB safety limit"));
    if(source.stored_size!=0&&source.stored_size!=bytes.size())return core::Result<Animation>::failure(detail::error(source,diagnostic_codes::source_mismatch,"provenance size does not match supplied animation bytes"));
    auto roots=detail::parse_chunks(bytes,source);if(!roots)return core::Result<Animation>::failure(roots.error());
    if(roots.value().size()!=1||roots.value()[0].type!=0x1000||!roots.value()[0].group)return fail<Animation>(source,"ALA requires one 0x1000 root group",0);
    const Chunk& root=roots.value()[0];if(root.children.empty()||root.children[0].type!=0x1001)return fail<Animation>(source,"ALA is missing information chunk",root.header_offset);
    auto header=detail::parse_minis(root.children[0],source);if(!header)return core::Result<Animation>::failure(header.error());
    Animation result;result.source=std::move(source);std::size_t hp=0;auto req=[&](std::uint8_t t)->const Mini*{if(hp>=header.value().size()||header.value()[hp].type!=t)return nullptr;return &header.value()[hp++];};
    const Mini* fm=req(1);const Mini* fps=req(2);const Mini* bc=req(3);std::uint32_t track_count{};
    auto u32=[](const Mini* m,std::uint32_t& v){if(!m)return false;Reader r(m->payload);return r.u32(v)&&r.remaining()==0;};
    if(!u32(fm,result.stored_frame_count)||!u32(bc,track_count)||result.stored_frame_count==0||result.stored_frame_count>detail::max_elements||track_count>detail::max_elements)return fail<Animation>(result.source,"invalid animation frame/track counts",root.children[0].header_offset,diagnostic_codes::limit);
    {Reader r(fps?fps->payload:std::span<const std::byte>{});if(!r.f32(result.frames_per_second)||r.remaining()!=0||!std::isfinite(result.frames_per_second)||result.frames_per_second<=0.0F)return fail<Animation>(result.source,"invalid animation frame rate",fps?fps->offset:root.children[0].header_offset);}
    std::uint32_t rot_block{},trans_block{},scale_block{};
    if(hp<header.value().size()&&header.value()[hp].type==11){result.version=AnimationVersion::v2;const Mini* r=req(11);const Mini* t=req(12);const Mini* s=req(13);if(!u32(r,rot_block)||!u32(t,trans_block)||!u32(s,scale_block)||rot_block%4U||trans_block%3U||scale_block%3U)return fail<Animation>(result.source,"invalid v2 animation block sizes",root.children[0].header_offset,diagnostic_codes::index);rot_block/=4U;trans_block/=3U;scale_block/=3U;}
    if(hp!=header.value().size())return fail<Animation>(result.source,"unknown ALA information mini-chunk",header.value()[hp].offset,diagnostic_codes::unsupported);
    if(root.children.size()<static_cast<std::size_t>(track_count)+1U)return fail<Animation>(result.source,"ALA has fewer tracks than declared",root.header_offset,diagnostic_codes::truncated);
    std::vector<TrackInfo> tracks;tracks.reserve(track_count);std::unordered_set<std::uint32_t> bone_indices;
    for(std::size_t i=0;i<track_count;++i){if(root.children[i+1].type!=0x1002)return fail<Animation>(result.source,"unexpected chunk in animation track sequence",root.children[i+1].header_offset);auto parsed=parse_track(root.children[i+1],result.source,result.version,result.stored_frame_count);if(!parsed)return core::Result<Animation>::failure(parsed.error());if(!bone_indices.insert(parsed.value().track.bone_index).second)return fail<Animation>(result.source,"duplicate animation bone index",root.children[i+1].header_offset,diagnostic_codes::index);tracks.push_back(std::move(parsed.value()));}
    std::vector<PackedVector> global_trans;std::vector<PackedQuat> global_rot;std::size_t tail=static_cast<std::size_t>(track_count)+1U;
    if(result.version==AnimationVersion::v2){
        if(scale_block!=0)return fail<Animation>(result.source,"v2 scale blocks are essential but have no documented payload chunk",root.children[0].header_offset,diagnostic_codes::unsupported);
        if(trans_block){if(tail>=root.children.size()||root.children[tail].type!=0x100A)return fail<Animation>(result.source,"missing v2 translation payload",root.header_offset);const auto count=static_cast<std::size_t>(trans_block)*result.stored_frame_count;if(root.children[tail].payload.size()!=count*6U)return fail<Animation>(result.source,"v2 translation payload size mismatch",root.children[tail].header_offset,diagnostic_codes::bounds);Reader r(root.children[tail].payload);global_trans.resize(count);for(auto& v:global_trans)packed_vector(r,v);++tail;}
        if(rot_block){if(tail>=root.children.size()||root.children[tail].type!=0x1009)return fail<Animation>(result.source,"missing v2 rotation payload",root.header_offset);const auto count=static_cast<std::size_t>(rot_block)*result.stored_frame_count;if(root.children[tail].payload.size()!=count*8U)return fail<Animation>(result.source,"v2 rotation payload size mismatch",root.children[tail].header_offset,diagnostic_codes::bounds);Reader r(root.children[tail].payload);global_rot.resize(count);for(auto& q:global_rot)packed_quat(r,q);++tail;}
    }
    if(tail!=root.children.size())return fail<Animation>(result.source,"unsupported trailing ALA chunk",root.children[tail].header_offset,diagnostic_codes::unsupported);
    result.playable_frame_count=result.stored_frame_count-1U;
    result.duration_seconds=static_cast<float>(result.playable_frame_count)/result.frames_per_second;
    for(auto& info:tracks){info.track.samples.resize(result.stored_frame_count);for(std::size_t f=0;f<result.stored_frame_count;++f){auto& sample=info.track.samples[f];sample.translation=info.trans_offset;sample.scale=info.scale_offset;sample.rotation=info.default_rotation;sample.visible=info.visibility[f];if(result.version==AnimationVersion::v1){if(!info.v1_trans.empty())sample.translation=unpack(info.v1_trans[f],info.trans_offset,info.trans_scale);if(!info.v1_scale.empty())sample.scale=unpack(info.v1_scale[f],info.scale_offset,info.scale_scale);if(!info.v1_rot.empty())sample.rotation=unpack(info.v1_rot.size()==1?info.v1_rot[0]:info.v1_rot[f]);}else{if(info.trans_index!=0xffffU){if(info.trans_index>=trans_block)return fail<Animation>(result.source,"translation block index out of range",root.header_offset,diagnostic_codes::index);sample.translation=unpack(global_trans[f*trans_block+info.trans_index],info.trans_offset,info.trans_scale);}if(info.rot_index!=0xffffU){if(info.rot_index>=rot_block)return fail<Animation>(result.source,"rotation block index out of range",root.header_offset,diagnostic_codes::index);sample.rotation=unpack(global_rot[f*rot_block+info.rot_index]);}if(info.scale_index!=0xffffU)return fail<Animation>(result.source,"v2 track references unavailable scale block",root.header_offset,diagnostic_codes::unsupported);}}result.tracks.push_back(std::move(info.track));}
    return core::Result<Animation>::success(std::move(result));
}

core::Result<Animation> load_animation(const vfs::Vfs& filesystem,const std::string_view path){auto record=filesystem.stat(path);if(!record)return core::Result<Animation>::failure(record.error());auto bytes=filesystem.open(path);if(!bytes)return core::Result<Animation>::failure(bytes.error());return load_animation(bytes.value(),source_from(record.value()));}
} // namespace eawr::assets
