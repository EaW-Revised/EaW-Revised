#include <utility>
#include "../support/check.hpp"
#include "eawr/assets/assets.hpp"
#include "texture_codec_fixtures.hpp"

#include <bit>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(bool condition,const char* message){
    local_checker.check(condition, message, std::cerr);
}
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

std::vector<std::byte> image_bytes(std::string_view hex) {
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::vector<std::byte> result;
    for (std::size_t index = 0; index < hex.size(); index += 2)
        result.push_back(static_cast<std::byte>((digit(hex[index]) << 4) | digit(hex[index + 1])));
    return result;
}

void image_codec_tests() {
    using namespace eawr::assets;
    using namespace texture_codec_fixtures;
    const auto rgba = image_bytes(rgba_png);
    for (const std::string path : {"color.png", "color.tga", "color.dds", "color.bin"}) {
        auto image = load_texture(rgba, source(path, rgba.size()));
        expect(bool(image), "PNG is selected by bytes, including PNG data named TGA");
        if (!image) continue;
        expect(image.value().width == 2 && image.value().height == 2 && image.value().has_alpha,
               "PNG dimensions and authored alpha are retained");
        expect(image.value().format == PixelFormat::rgba8 && image.value().source_origin == ImageOrigin::top_left,
               "PNG produces top-left RGBA8 pixels");
        const auto& mip = image.value().mips.front();
        expect(mip.row_pitch == 8 && mip.bytes.size() == 16, "PNG has one tightly packed mip");
        const std::vector<std::byte> pixels{std::byte{220},std::byte{15},std::byte{30},std::byte{80},
            std::byte{15},std::byte{210},std::byte{40},std::byte{255},
            std::byte{10},std::byte{20},std::byte{230},std::byte{0},
            std::byte{200},std::byte{180},std::byte{15},std::byte{128}};
        expect(mip.bytes == pixels, "PNG preserves row order, RGB and partial/zero alpha exactly");
    }
    for (const auto encoded : {rgb_png, gray_png, gray_alpha_png}) {
        const auto bytes = image_bytes(encoded);
        auto image = load_texture(bytes, source("synthetic.png", bytes.size()));
        expect(bool(image), "RGB and grayscale PNG layouts decode");
        if (!image) continue;
        expect(image.value().width == 2 && image.value().height == 1, "PNG layout dimensions retained");
        expect(image.value().has_alpha == (encoded == gray_alpha_png), "PNG alpha metadata follows source channels");
        const auto& pixels = image.value().mips.front().bytes;
        if (encoded == rgb_png) {
            expect(pixels[0] == std::byte{220} && pixels[3] == std::byte{255}, "RGB PNG receives opaque alpha");
        } else {
            expect(pixels[0] == std::byte{31} && pixels[1] == std::byte{31} && pixels[2] == std::byte{31},
                   "grayscale PNG expands equally to RGB");
            expect(pixels[3] == (encoded == gray_alpha_png ? std::byte{80} : std::byte{255}),
                   "grayscale PNG retains alpha or receives opaque alpha");
        }
    }
    const auto jpeg = image_bytes(rgb_jpeg);
    auto photograph = load_texture(jpeg, source("synthetic.tga", jpeg.size()));
    expect(bool(photograph), "JPEG is decoded by bytes with a different suffix");
    if (photograph) {
        expect(photograph.value().width == 8 && photograph.value().height == 8 && !photograph.value().has_alpha,
               "JPEG dimensions and opaque source metadata retained");
        const auto& pixels = photograph.value().mips.front().bytes;
        expect(pixels.size() == 8 * 8 * 4 && photograph.value().source_origin == ImageOrigin::top_left,
               "JPEG produces tightly packed top-left RGBA8");
        for (std::size_t index = 0; index < pixels.size(); index += 4) {
            const auto r = std::to_integer<int>(pixels[index]);
            const auto g = std::to_integer<int>(pixels[index + 1]);
            const auto b = std::to_integer<int>(pixels[index + 2]);
            expect(r >= 221 && r <= 227 && g >= 29 && g <= 35 && b >= 45 && b <= 51
                && pixels[index + 3] == std::byte{255}, "JPEG decodes the invented colour within compression tolerance");
        }
    }
    const auto cmyk = image_bytes(cmyk_jpeg);
    auto print_image = load_texture(cmyk, source("synthetic-cmyk.jpg", cmyk.size()));
    expect(bool(print_image), "four-component CMYK JPEG decodes");
    if (print_image) {
        expect(!print_image.value().has_alpha, "CMYK colour components are not an alpha channel");
        const auto& pixels = print_image.value().mips.front().bytes;
        expect(pixels.size() == 8 * 8 * 4, "CMYK JPEG expands to RGBA8");
        for (std::size_t index = 3; index < pixels.size(); index += 4)
            expect(pixels[index] == std::byte{255}, "CMYK JPEG output pixels are opaque");
    }
    for (auto bytes : {rgba, jpeg}) {
        bytes.resize(12);
        auto invalid = load_texture(bytes, source("truncated.tga", bytes.size()));
        expect(!invalid && invalid.error().code == diagnostic_codes::texture_format, "truncated image headers fail closed");
    }
    auto oversized = rgba;
    // IHDR is big endian. 8193 squared exceeds the existing pixel budget;
    // the preflight must reject this before allocating or decoding a plane.
    for (const std::size_t offset : {16U, 20U}) {
        oversized[offset] = std::byte{0}; oversized[offset + 1] = std::byte{0};
        oversized[offset + 2] = std::byte{0x20}; oversized[offset + 3] = std::byte{1};
    }
    auto limited = load_texture(oversized, source("oversized.png", oversized.size()));
    expect(!limited && limited.error().code == diagnostic_codes::limit, "PNG dimensions are bounded before image allocation");
    auto corrupt = rgba;
    std::fill(corrupt.begin() + 41, corrupt.end(), std::byte{0});
    expect(!load_texture(corrupt, source("corrupt.png", corrupt.size())), "invalid PNG image data is rejected");
    expect(!load_texture(rgba, source("size.png", rgba.size() + 1)), "PNG retains the source-size provenance check");
}
}
int main(){using namespace eawr::assets;
    image_codec_tests();
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
