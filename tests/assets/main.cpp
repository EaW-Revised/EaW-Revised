#include "eawr/assets/assets.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {
int failures{};
void expect(bool condition,const char* message){if(!condition){std::cerr<<"FAIL: "<<message<<'\n';++failures;}}
void u8(std::vector<std::byte>& out,std::uint8_t v){out.push_back(static_cast<std::byte>(v));}
void u16(std::vector<std::byte>& out,std::uint16_t v){u8(out,v&255U);u8(out,(v>>8U)&255U);}
void u32(std::vector<std::byte>& out,std::uint32_t v){for(unsigned s=0;s<32;s+=8)u8(out,(v>>s)&255U);}
void f32(std::vector<std::byte>& out,float v){u32(out,std::bit_cast<std::uint32_t>(v));}
void str(std::vector<std::byte>& out,const char* v){while(*v)u8(out,static_cast<std::uint8_t>(*v++));u8(out,0);}
std::vector<std::byte> chunk(std::uint32_t type,const std::vector<std::byte>& payload,bool group=false){std::vector<std::byte> out;u32(out,type);u32(out,static_cast<std::uint32_t>(payload.size())|(group?0x80000000U:0));out.insert(out.end(),payload.begin(),payload.end());return out;}
void add(std::vector<std::byte>& out,const std::vector<std::byte>& value){out.insert(out.end(),value.begin(),value.end());}
void mini(std::vector<std::byte>& out,std::uint8_t type,const std::vector<std::byte>& payload){u8(out,type);u8(out,static_cast<std::uint8_t>(payload.size()));add(out,payload);}
std::vector<std::byte> integer(std::uint32_t v){std::vector<std::byte> out;u32(out,v);return out;}

// Original fixture, manually constructed from the public chunk grammar rather
// than serialized by production code.
std::vector<std::byte> model_fixture(std::int32_t parent=-1, std::int32_t billboard=-1){
    std::vector<std::byte> count;u32(count,1);std::vector<std::byte> bone;std::vector<std::byte> name;str(name,"ROOT");add(bone,chunk(0x203,name));std::vector<std::byte> data;u32(data,std::bit_cast<std::uint32_t>(parent));u32(data,1);if(billboard>=0)u32(data,static_cast<std::uint32_t>(billboard));for(int i=0;i<12;++i)f32(data,(i==0||i==4||i==8)?1.0F:0.0F);add(bone,chunk(billboard>=0?0x206:0x205,data));std::vector<std::byte> skeleton;add(skeleton,chunk(0x201,count));add(skeleton,chunk(0x202,bone,true));
    std::vector<std::byte> connection_header;mini(connection_header,1,integer(0));mini(connection_header,4,integer(0));std::vector<std::byte> connections;add(connections,chunk(0x601,connection_header));std::vector<std::byte> file;add(file,chunk(0x200,skeleton,true));add(file,chunk(0x600,connections,true));return file;
}
std::vector<std::byte> animation_fixture(){std::vector<std::byte> info;mini(info,1,integer(1));std::vector<std::byte> rate;f32(rate,30.0F);mini(info,2,rate);mini(info,3,integer(0));std::vector<std::byte> root;add(root,chunk(0x1001,info));return chunk(0x1000,root,true);}
std::vector<std::byte> dds_fixture(){std::vector<std::byte> out;u32(out,0x20534444);u32(out,124);u32(out,0x100f);u32(out,1);u32(out,1);u32(out,4);u32(out,0);u32(out,1);for(int i=0;i<11;++i)u32(out,0);u32(out,32);u32(out,0x41);u32(out,0);u32(out,32);u32(out,0x00ff0000);u32(out,0x0000ff00);u32(out,0x000000ff);u32(out,0xff000000);u32(out,0x1000);for(int i=0;i<4;++i)u32(out,0);u8(out,3);u8(out,2);u8(out,1);u8(out,4);return out;}
std::vector<std::byte> tga_fixture(){std::vector<std::byte> out;u8(out,0);u8(out,0);u8(out,10);for(int i=0;i<5;++i)u8(out,0);for(int i=0;i<4;++i)u8(out,0);u16(out,2);u16(out,1);u8(out,24);u8(out,0x20);u8(out,0x81);u8(out,9);u8(out,8);u8(out,7);return out;}
eawr::assets::Source source(std::string path,std::size_t size){return {std::move(path),"fixture","test",eawr::vfs::AssetOrigin::loose,size};}
}
int main(){using namespace eawr::assets;
    auto mb=model_fixture();auto model=load_model(mb,source("data/art/models/test.alo",mb.size()));expect(bool(model),"valid minimal ALO loads");if(model){expect(model.value().bones.size()==1,"ALO retains bone");expect(model.value().bones[0].relative_transform[4]==1.0F,"ALO retains source float transform");}
    for(const int mode : {0,1,2,3,6,7}){auto encoded=model_fixture(-1,mode);auto decoded=load_model(encoded,source("billboard.alo",encoded.size()));expect(decoded&&decoded.value().bones[0].billboard==static_cast<std::uint32_t>(mode),"0x206 bone retains authored billboard mode");}
    auto cycle=model_fixture(0);auto bad_cycle=load_model(cycle,source("cycle.alo",cycle.size()));expect(!bad_cycle&&bad_cycle.error().code==diagnostic_codes::hierarchy_cycle,"bone self-cycle is rejected");
    auto truncated=mb;truncated.pop_back();auto bad_bounds=load_model(truncated,source("short.alo",truncated.size()));expect(!bad_bounds,"truncated chunk is rejected");
    auto ab=animation_fixture();auto animation=load_animation(ab,source("test.ala",ab.size()));expect(bool(animation),"valid minimal ALA loads");if(animation){expect(animation.value().stored_frame_count==1,"ALA preserves stored terminal frame");expect(animation.value().playable_frame_count==0,"ALA exposes duplicate-terminal convention");expect(animation.value().duration_seconds==0.0F,"ALA exposes deterministic playable duration");}
    auto db=dds_fixture();auto dds=load_texture(db,source("test.dds",db.size()));expect(bool(dds),"valid uncompressed DDS loads");if(dds){expect(dds.value().format==PixelFormat::bgra8,"DDS channel masks retained");expect(dds.value().mips[0].bytes.size()==4,"DDS mip payload retained");}
    auto volume=db;volume[24]=std::byte{2};auto volume_result=load_texture(volume,source("volume.dds",volume.size()));expect(!volume_result&&volume_result.error().code==diagnostic_codes::limit&&volume_result.error().message.find("volume")!=std::string::npos,"volume DDS has a specific representation-limit diagnostic");
    auto tb=tga_fixture();auto tga=load_texture(tb,source("test.tga",tb.size()));expect(bool(tga),"valid RLE TGA loads");if(tga){expect(tga.value().format==PixelFormat::rgba8,"TGA decodes to RGBA8");expect(tga.value().mips[0].bytes[0]==std::byte{7}&&tga.value().mips[0].bytes[3]==std::byte{0xff},"TGA BGR conversion and opaque alpha");}
    auto legacy=tb;const auto extension_offset=static_cast<std::uint32_t>(legacy.size());for(int i=0;i<10;++i)u8(legacy,0);u32(legacy,extension_offset);u32(legacy,0);for(const char ch:std::string("TRUEVISION-XFILE."))u8(legacy,static_cast<std::uint8_t>(ch));auto legacy_tga=load_texture(legacy,source("legacy.tga",legacy.size()));expect(bool(legacy_tga)&&legacy_tga.value().notices.size()==1,"bounded legacy 25-byte TGA footer loads with notice");
    auto overrun=tb;overrun[18]=std::byte{0x82};auto bad_rle=load_texture(overrun,source("bad.tga",overrun.size()));expect(!bad_rle&&bad_rle.error().code==diagnostic_codes::bounds,"TGA RLE overrun is rejected");
    if(failures==0)std::cout<<"asset tests passed\n";
    return failures==0?0:1;
}
