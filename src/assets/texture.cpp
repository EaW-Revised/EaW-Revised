#include "asset_internal.hpp"

#include <algorithm>

namespace eawr::assets {
namespace {
using detail::Reader;
// Texture pages have a separate budget from mesh/chunk/directory elements.
constexpr std::size_t max_texture_pixels = 8192U * 8192U;

template <typename T>
core::Result<T> fail(const Source& source, std::string message, const std::uint64_t offset,
                     const std::string_view code = diagnostic_codes::texture_header) {
    return core::Result<T>::failure(detail::error(source, code, std::move(message), offset));
}

std::string extension(std::string_view path) {
    const auto dot=path.find_last_of('.');std::string result(dot==std::string_view::npos?std::string_view{}:path.substr(dot));
    std::transform(result.begin(),result.end(),result.begin(),[](unsigned char c){return static_cast<char>((c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c));});return result;
}

core::Result<Texture> load_bmp(const std::span<const std::byte> bytes, Source source) {
    if (bytes.size() < 54U)
        return fail<Texture>(source, "BMP header is truncated", 0, diagnostic_codes::truncated);
    Reader reader(bytes);
    std::uint16_t magic{}, reserved1{}, reserved2{}, planes{}, depth{};
    std::uint32_t file_size{}, pixels_offset{}, header_size{}, compression{}, image_size{};
    std::int32_t width{}, signed_height{};
    if (!reader.u16(magic) || !reader.u32(file_size) || !reader.u16(reserved1) ||
        !reader.u16(reserved2) || !reader.u32(pixels_offset) || !reader.u32(header_size) ||
        !reader.i32(width) || !reader.i32(signed_height) || !reader.u16(planes) ||
        !reader.u16(depth) || !reader.u32(compression) || !reader.u32(image_size))
        return fail<Texture>(source, "BMP header is truncated", 0, diagnostic_codes::truncated);
    if (magic != 0x4d42U || reserved1 != 0U || reserved2 != 0U || planes != 1U)
        return fail<Texture>(source, "invalid BMP file header or plane count", 0);
    // The mod pages use BITMAPINFOHEADER, BI_RGB and BGRA bytes (including alpha).
    // Other DIB versions, palettes, bitfields and compression fail closed.
    if (header_size != 40U || depth != 32U || compression != 0U)
        return fail<Texture>(source, "unsupported BMP DIB header, depth or compression", 14,
                             diagnostic_codes::texture_format);
    std::uint32_t x_resolution{}, y_resolution{}, colors_used{}, colors_important{};
    if (!reader.u32(x_resolution) || !reader.u32(y_resolution) ||
        !reader.u32(colors_used) || !reader.u32(colors_important))
        return fail<Texture>(source, "BMP info header is truncated", 38, diagnostic_codes::truncated);
    if (colors_used != 0U || colors_important != 0U)
        return fail<Texture>(source, "BMP palette metadata is unsupported", 46, diagnostic_codes::texture_format);
    if (width <= 0 || signed_height == 0 || signed_height == std::numeric_limits<std::int32_t>::min())
        return fail<Texture>(source, "invalid BMP dimensions", 18, diagnostic_codes::limit);
    const auto height = static_cast<std::uint32_t>(signed_height < 0 ? -signed_height : signed_height);
    std::size_t pixel_count{};
    if (!detail::checked_multiply<std::size_t>(static_cast<std::size_t>(width), height, pixel_count) ||
        pixel_count > max_texture_pixels)
        return fail<Texture>(source, "BMP pixel count exceeds safety limit", 18, diagnostic_codes::limit);
    const auto payload_size = pixel_count * 4U;
    if (pixels_offset < 54U || pixels_offset > bytes.size())
        return fail<Texture>(source, "BMP pixel offset crosses header/file bounds", 10, diagnostic_codes::bounds);
    if (payload_size > bytes.size() - pixels_offset)
        return fail<Texture>(source, "BMP pixel data is truncated", pixels_offset, diagnostic_codes::truncated);
    if (file_size != bytes.size() || payload_size != bytes.size() - pixels_offset ||
        (image_size != 0U && image_size != payload_size))
        return fail<Texture>(source, "BMP file/image size disagrees with pixel payload", 2, diagnostic_codes::bounds);
    Texture result;
    result.source = std::move(source);
    result.width = static_cast<std::uint32_t>(width);
    result.height = height;
    result.format = PixelFormat::bgra8;
    result.source_origin = signed_height < 0 ? ImageOrigin::top_left : ImageOrigin::bottom_left;
    result.has_alpha = true;
    MipLevel mip{result.width, height, result.width * 4U, {}};
    mip.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pixels_offset), bytes.end());
    result.mips.push_back(std::move(mip));
    return core::Result<Texture>::success(std::move(result));
}

core::Result<Texture> load_dds(const std::span<const std::byte> bytes, Source source) {
    if(bytes.size()<128U)return fail<Texture>(source,"DDS header is truncated",0,diagnostic_codes::truncated);
    Reader r(bytes);std::uint32_t magic{},header_size{},flags{},height{},width{},pitch{},depth{},mip_count{};
    if(!r.u32(magic)||magic!=0x20534444U||!r.u32(header_size)||header_size!=124U||!r.u32(flags)||!r.u32(height)||!r.u32(width)||!r.u32(pitch)||!r.u32(depth)||!r.u32(mip_count))return fail<Texture>(source,"invalid DDS magic or base header",0);
    if(width==0||height==0||width>65536U||height>65536U)return fail<Texture>(source,"unsupported or excessive DDS width or height",12,diagnostic_codes::limit);
    if(depth>1U)return fail<Texture>(source,"DDS volume texture is not representable by the 2D Texture API",24,diagnostic_codes::limit);
    if(mip_count>32U)return fail<Texture>(source,"DDS mip count exceeds the supported limit",28,diagnostic_codes::limit);
    std::span<const std::byte> reserved;if(!r.bytes(44,reserved))return fail<Texture>(source,"truncated DDS reserved fields",32,diagnostic_codes::truncated);
    std::uint32_t pf_size{},pf_flags{},fourcc{},rgb_bits{},rmask{},gmask{},bmask{},amask{},caps{},caps2{},caps3{},caps4{},reserved2{};
    if(!r.u32(pf_size)||!r.u32(pf_flags)||!r.u32(fourcc)||!r.u32(rgb_bits)||!r.u32(rmask)||!r.u32(gmask)||!r.u32(bmask)||!r.u32(amask)||!r.u32(caps)||!r.u32(caps2)||!r.u32(caps3)||!r.u32(caps4)||!r.u32(reserved2)||pf_size!=32U)return fail<Texture>(source,"invalid DDS pixel format/header",76);
    if((caps2&0x0000fe00U)!=0U)return fail<Texture>(source,"cubemap and volume DDS resources are not representable by the texture API",112,diagnostic_codes::unsupported);
    Texture result;result.source=std::move(source);result.width=width;result.height=height;result.source_origin=ImageOrigin::top_left;
    std::uint32_t block_bytes{},bytes_per_pixel{};std::size_t data_offset=128;
    if((pf_flags&0x4U)!=0U){
        if(fourcc==0x31545844U){result.format=PixelFormat::bc1;block_bytes=8;result.has_alpha=(pf_flags&0x1U)!=0;}
        else if(fourcc==0x33545844U){result.format=PixelFormat::bc2;block_bytes=16;result.has_alpha=true;}
        else if(fourcc==0x35545844U){result.format=PixelFormat::bc3;block_bytes=16;result.has_alpha=true;}
        else if(fourcc==0x30315844U){std::uint32_t dxgi{},dimension{},misc{},array_size{},misc2{};if(!r.u32(dxgi)||!r.u32(dimension)||!r.u32(misc)||!r.u32(array_size)||!r.u32(misc2)||dimension!=3U||array_size!=1U||(misc&0x4U)!=0U)return fail<Texture>(result.source,"unsupported DDS DX10 resource shape",128,diagnostic_codes::unsupported);data_offset=148;switch(dxgi){case 71:case 72:result.format=PixelFormat::bc1;block_bytes=8;break;case 74:case 75:result.format=PixelFormat::bc2;block_bytes=16;result.has_alpha=true;break;case 77:case 78:result.format=PixelFormat::bc3;block_bytes=16;result.has_alpha=true;break;case 80:case 81:result.format=PixelFormat::bc4;block_bytes=8;break;case 83:case 84:result.format=PixelFormat::bc5;block_bytes=16;break;case 98:case 99:result.format=PixelFormat::bc7;block_bytes=16;result.has_alpha=true;break;default:return fail<Texture>(result.source,"unsupported DDS DX10 pixel format "+std::to_string(dxgi),128,diagnostic_codes::texture_format);}}
        else return fail<Texture>(result.source,"unsupported DDS FourCC "+std::to_string(fourcc),84,diagnostic_codes::texture_format);
    }else if((pf_flags&0x40U)!=0U&&rgb_bits==32U){bytes_per_pixel=4;result.has_alpha=amask!=0;if(rmask==0x00ff0000U&&gmask==0x0000ff00U&&bmask==0x000000ffU){result.format=PixelFormat::bgra8;}else if(rmask==0x000000ffU&&gmask==0x0000ff00U&&bmask==0x00ff0000U){result.format=PixelFormat::rgba8;}else return fail<Texture>(result.source,"unsupported 32-bit DDS channel masks",88,diagnostic_codes::texture_format);
    }else if((pf_flags&0x40U)!=0U&&rgb_bits==24U&&rmask==0x00ff0000U&&gmask==0x0000ff00U&&bmask==0x000000ffU){result.format=PixelFormat::bgr8;bytes_per_pixel=3;
    }else if((pf_flags&0x20000U)!=0U&&rgb_bits==8U){result.format=PixelFormat::l8;bytes_per_pixel=1;
    }else if((pf_flags&0x2U)!=0U&&rgb_bits==8U){result.format=PixelFormat::a8;bytes_per_pixel=1;result.has_alpha=true;
    }else return fail<Texture>(result.source,"unsupported DDS pixel layout flags="+std::to_string(pf_flags)+" bits="+std::to_string(rgb_bits),80,diagnostic_codes::texture_format);
    const std::uint32_t levels=mip_count==0?1:mip_count;std::size_t offset=data_offset;std::uint32_t w=width,h=height;
    for(std::uint32_t level=0;level<levels;++level){std::size_t size{};std::uint32_t row{};if(block_bytes){const auto bw=(w+3U)/4U,bh=(h+3U)/4U;row=bw*block_bytes;if(!detail::checked_multiply<std::size_t>(row,bh,size))return fail<Texture>(result.source,"DDS mip size overflow",offset,diagnostic_codes::limit);}else{row=w*bytes_per_pixel;if(!detail::checked_multiply<std::size_t>(row,h,size))return fail<Texture>(result.source,"DDS mip size overflow",offset,diagnostic_codes::limit);}if(size>bytes.size()-std::min(offset,bytes.size()))return fail<Texture>(result.source,"DDS mip payload is truncated",offset,diagnostic_codes::truncated);MipLevel mip{w,h,row,{}};mip.bytes.assign(bytes.begin()+static_cast<std::ptrdiff_t>(offset),bytes.begin()+static_cast<std::ptrdiff_t>(offset+size));result.mips.push_back(std::move(mip));offset+=size;w=std::max(1U,w/2U);h=std::max(1U,h/2U);}
    while(offset<bytes.size()&&(w>1U||h>1U)){std::size_t size{};std::uint32_t row{};if(block_bytes){const auto bw=(w+3U)/4U,bh=(h+3U)/4U;row=bw*block_bytes;if(!detail::checked_multiply<std::size_t>(row,bh,size))return fail<Texture>(result.source,"DDS mip size overflow",offset,diagnostic_codes::limit);}else{row=w*bytes_per_pixel;if(!detail::checked_multiply<std::size_t>(row,h,size))return fail<Texture>(result.source,"DDS mip size overflow",offset,diagnostic_codes::limit);}if(size>bytes.size()-offset)break;MipLevel mip{w,h,row,{}};mip.bytes.assign(bytes.begin()+static_cast<std::ptrdiff_t>(offset),bytes.begin()+static_cast<std::ptrdiff_t>(offset+size));result.mips.push_back(std::move(mip));offset+=size;w=std::max(1U,w/2U);h=std::max(1U,h/2U);}
    if(offset<bytes.size()&&w==1U&&h==1U){const std::uint32_t row=block_bytes?block_bytes:bytes_per_pixel;const std::size_t size=row;if(bytes.size()-offset==size){MipLevel mip{1,1,row,{}};mip.bytes.assign(bytes.begin()+static_cast<std::ptrdiff_t>(offset),bytes.end());result.mips.push_back(std::move(mip));offset=bytes.size();}}
    if(offset!=bytes.size())return fail<Texture>(result.source,"DDS has unexplained trailing bytes",offset,diagnostic_codes::bounds);
    return core::Result<Texture>::success(std::move(result));
}

core::Result<Texture> load_tga(const std::span<const std::byte> bytes, Source source) {
    if(bytes.size()<18U)return fail<Texture>(source,"TGA header is truncated",0,diagnostic_codes::truncated);
    Reader r(bytes);
    std::uint8_t id_length{},color_map_type{},image_type{},color_depth{},descriptor{};std::uint16_t cmap_first{},cmap_length{},x_origin{},y_origin{},width{},height{};std::uint8_t cmap_depth{};
    if(!r.u8(id_length)||!r.u8(color_map_type)||!r.u8(image_type)||!r.u16(cmap_first)||!r.u16(cmap_length)||!r.u8(cmap_depth)||!r.u16(x_origin)||!r.u16(y_origin)||!r.u16(width)||!r.u16(height)||!r.u8(color_depth)||!r.u8(descriptor))return fail<Texture>(source,"TGA header is truncated",0,diagnostic_codes::truncated);
    if(color_map_type!=0||cmap_length!=0)return fail<Texture>(source,"color-mapped TGA is unsupported",1,diagnostic_codes::texture_format);
    const bool grayscale=image_type==3||image_type==11;const bool rle=image_type==10||image_type==11;if((image_type!=2&&image_type!=3&&image_type!=10&&image_type!=11)||width==0||height==0)return fail<Texture>(source,"unsupported TGA image type or dimensions",2,diagnostic_codes::texture_format);
    const std::uint8_t bytes_per_pixel=grayscale?1U:static_cast<std::uint8_t>(color_depth/8U);if((grayscale&&color_depth!=8)||(!grayscale&&color_depth!=24&&color_depth!=32))return fail<Texture>(source,"unsupported TGA pixel depth",16,diagnostic_codes::texture_format);
    if(id_length>r.remaining())return fail<Texture>(source,"TGA image identifier crosses file bound",18,diagnostic_codes::bounds);
    std::span<const std::byte> id;r.bytes(id_length,id);
    std::size_t pixel_count{};if(!detail::checked_multiply<std::size_t>(width,height,pixel_count)||pixel_count>max_texture_pixels)return fail<Texture>(source,"TGA pixel count exceeds safety limit",12,diagnostic_codes::limit);
    std::vector<std::byte> decoded;decoded.reserve(pixel_count*bytes_per_pixel);auto copy_pixel=[&](){std::span<const std::byte> p;if(!r.bytes(bytes_per_pixel,p))return false;decoded.insert(decoded.end(),p.begin(),p.end());return true;};
    if(!rle){for(std::size_t i=0;i<pixel_count;++i)if(!copy_pixel())return fail<Texture>(source,"TGA pixel data is truncated",r.absolute(),diagnostic_codes::truncated);}else{while(decoded.size()/bytes_per_pixel<pixel_count){std::uint8_t packet{};if(!r.u8(packet))return fail<Texture>(source,"TGA RLE packet header is truncated",r.absolute(),diagnostic_codes::truncated);const std::size_t count=(packet&0x7fU)+1U;if(count>pixel_count-decoded.size()/bytes_per_pixel)return fail<Texture>(source,"TGA RLE packet exceeds declared pixel count",r.absolute()-1U,diagnostic_codes::bounds);if(packet&0x80U){std::span<const std::byte> p;if(!r.bytes(bytes_per_pixel,p))return fail<Texture>(source,"TGA RLE pixel is truncated",r.absolute(),diagnostic_codes::truncated);for(std::size_t i=0;i<count;++i)decoded.insert(decoded.end(),p.begin(),p.end());}else for(std::size_t i=0;i<count;++i)if(!copy_pixel())return fail<Texture>(source,"TGA raw packet is truncated",r.absolute(),diagnostic_codes::truncated);}}
    // The optional TGA 2.0 footer is metadata, not an image plane. Accept only
    // the exact standardized 26-byte footer; other tails remain a bounds error.
    bool legacy_footer=false;if(r.remaining()!=0){constexpr char signature[]="TRUEVISION-XFILE.";std::size_t footer_size{};if(bytes.size()>=26U&&std::memcmp(bytes.data()+bytes.size()-18U,signature,17)==0&&bytes.back()==std::byte{0})footer_size=26U;else if(bytes.size()>=25U&&std::memcmp(bytes.data()+bytes.size()-17U,signature,17)==0){footer_size=25U;legacy_footer=true;}else return fail<Texture>(source,"TGA has unexplained trailing bytes",r.absolute(),diagnostic_codes::bounds);const auto footer=bytes.last(footer_size);Reader fr(footer);std::uint32_t extension_offset{},developer_offset{};fr.u32(extension_offset);fr.u32(developer_offset);const auto image_end=r.absolute();const auto footer_offset=bytes.size()-footer_size;auto valid_offset=[&](std::uint32_t value){return value==0U||(value>=image_end&&value<footer_offset);};if(!valid_offset(extension_offset)||!valid_offset(developer_offset))return fail<Texture>(source,"TGA footer metadata offset crosses image/footer bounds",footer_offset,diagnostic_codes::bounds);}
    Texture result;result.source=std::move(source);result.width=width;result.height=height;result.source_origin=(descriptor&0x20U)?ImageOrigin::top_left:ImageOrigin::bottom_left;result.has_alpha=!grayscale&&color_depth==32;
    MipLevel mip; mip.width=width;mip.height=height;
    if(grayscale){result.format=PixelFormat::l8;mip.row_pitch=width;mip.bytes=std::move(decoded);}else{result.format=PixelFormat::rgba8;mip.row_pitch=static_cast<std::uint32_t>(width)*4U;mip.bytes.resize(pixel_count*4U);for(std::size_t i=0;i<pixel_count;++i){mip.bytes[i*4]=decoded[i*bytes_per_pixel+2];mip.bytes[i*4+1]=decoded[i*bytes_per_pixel+1];mip.bytes[i*4+2]=decoded[i*bytes_per_pixel];mip.bytes[i*4+3]=bytes_per_pixel==4?decoded[i*bytes_per_pixel+3]:std::byte{0xff};}}
    result.mips.push_back(std::move(mip));if(legacy_footer)result.notices.push_back(Notice{0,bytes.size()-25U,25U,"accepted legacy TGA 2.0 footer without terminal NUL"});return core::Result<Texture>::success(std::move(result));
}
} // namespace

core::Result<Texture> load_texture(const std::span<const std::byte> bytes, Source source){if(bytes.size()>detail::max_file_size)return core::Result<Texture>::failure(detail::error(source,diagnostic_codes::limit,"texture exceeds 512 MiB safety limit"));if(source.stored_size!=0&&source.stored_size!=bytes.size())return core::Result<Texture>::failure(detail::error(source,diagnostic_codes::source_mismatch,"provenance size does not match supplied texture bytes"));const auto ext=extension(source.logical_path);if(bytes.size()>=2&&bytes[0]==std::byte{'B'}&&bytes[1]==std::byte{'M'})return load_bmp(bytes,std::move(source));if(bytes.size()>=4&&bytes[0]==std::byte{'D'}&&bytes[1]==std::byte{'D'}&&bytes[2]==std::byte{'S'}&&bytes[3]==std::byte{' '})return load_dds(bytes,std::move(source));if(bytes.size()>=18){const auto image_type=std::to_integer<unsigned>(bytes[2]);const auto depth=std::to_integer<unsigned>(bytes[16]);if((image_type==2U||image_type==3U||image_type==10U||image_type==11U)&&(depth==8U||depth==24U||depth==32U))return load_tga(bytes,std::move(source));}return fail<Texture>(source,"texture bytes are not supported BMP, DDS or TGA (extension "+ext+")",0,diagnostic_codes::texture_format);}
core::Result<Texture> load_texture(const vfs::Vfs& filesystem,const std::string_view path){auto record=filesystem.stat(path);if(!record)return core::Result<Texture>::failure(record.error());auto bytes=filesystem.open(path);if(!bytes)return core::Result<Texture>::failure(bytes.error());return load_texture(bytes.value(),source_from(record.value()));}
} // namespace eawr::assets
