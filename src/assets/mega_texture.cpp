#include "asset_internal.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <sstream>

namespace eawr::assets {
namespace {
constexpr std::size_t name_bytes = 64U;
constexpr std::size_t record_bytes = name_bytes + 4U * sizeof(std::uint32_t) + sizeof(std::uint8_t);

template <typename T>
core::Result<T> fail(const Source& source, const std::string_view code,
                     std::string message,
                     const std::optional<std::uint64_t> offset = std::nullopt) {
    return core::Result<T>::failure(detail::error(source, code, std::move(message), offset));
}

bool printable_ascii(const std::byte value) {
    const auto character = std::to_integer<unsigned char>(value);
    return character >= 0x20U && character <= 0x7eU;
}

bool ascii_equal(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        const auto fold = [](const unsigned char value) {
            return value >= 'A' && value <= 'Z'
                ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
        };
        if (fold(static_cast<unsigned char>(left[index])) !=
            fold(static_cast<unsigned char>(right[index]))) return false;
    }
    return true;
}

core::Result<std::string> page_stem(const Source& source) {
    const auto dot = source.logical_path.find_last_of('.');
    if (dot == std::string::npos || dot == 0U) {
        return fail<std::string>(source, diagnostic_codes::mega_texture_header,
                                 "MTD source path has no replaceable extension", 0U);
    }
    std::string extension = source.logical_path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](const unsigned char value) {
                       return static_cast<char>((value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value));
                   });
    if (extension != ".mtd") {
        return fail<std::string>(source, diagnostic_codes::mega_texture_header,
                                 "mega texture source path must end in .mtd", 0U);
    }
    return core::Result<std::string>::success(source.logical_path.substr(0U, dot));
}

core::Result<std::optional<vfs::AssetRecord>> probe_page(
    const vfs::Vfs& filesystem, const std::string_view logical_path) {
    auto record = filesystem.stat(logical_path);
    if (record) {
        return core::Result<std::optional<vfs::AssetRecord>>::success(
            std::move(record.value()));
    }
    if (record.error().code == vfs::diagnostic_codes::not_found) {
        return core::Result<std::optional<vfs::AssetRecord>>::success(std::nullopt);
    }
    return core::Result<std::optional<vfs::AssetRecord>>::failure(record.error());
}

core::Result<MegaTextureAtlas> load_page_and_bind(
    const vfs::Vfs& filesystem, MegaTexture directory,
    const std::string_view page_path) {
    auto page = load_texture(filesystem, page_path);
    if (!page) return core::Result<MegaTextureAtlas>::failure(page.error());
    return bind_mega_texture(std::move(directory), std::move(page.value()));
}
} // namespace

const MegaTextureEntry* MegaTexture::find(const std::string_view name) const noexcept {
    const auto found = std::find_if(entries.rbegin(), entries.rend(),
                                    [name](const MegaTextureEntry& entry) {
                                        return ascii_equal(entry.name, name);
                                    });
    return found == entries.rend() ? nullptr : &*found;
}

core::Result<MegaTexture> load_mega_texture(const std::span<const std::byte> bytes,
                                            Source source) {
    if (bytes.size() > detail::max_file_size) {
        return fail<MegaTexture>(source, diagnostic_codes::limit,
                                 "MTD exceeds 512 MiB safety limit", 0U);
    }
    if (source.stored_size != 0U && source.stored_size != bytes.size()) {
        return fail<MegaTexture>(source, diagnostic_codes::source_mismatch,
                                 "provenance size does not match supplied MTD bytes", 0U);
    }
    auto stem = page_stem(source);
    if (!stem) return core::Result<MegaTexture>::failure(stem.error());
    if (bytes.size() < sizeof(std::uint32_t)) {
        return fail<MegaTexture>(source, diagnostic_codes::truncated,
                                 "MTD entry-count header is truncated", 0U);
    }

    detail::Reader reader(bytes);
    std::uint32_t count{};
    if (!reader.u32(count)) {
        return fail<MegaTexture>(source, diagnostic_codes::truncated,
                                 "MTD entry-count header is truncated", 0U);
    }
    if (count > detail::max_elements) {
        return fail<MegaTexture>(source, diagnostic_codes::limit,
                                 "MTD entry count exceeds safety limit", 0U);
    }
    std::size_t table_bytes{};
    if (!detail::checked_multiply<std::size_t>(count, record_bytes, table_bytes)) {
        return fail<MegaTexture>(source, diagnostic_codes::limit,
                                 "MTD table size overflows the host size type", 0U);
    }
    if (table_bytes > reader.remaining()) {
        return fail<MegaTexture>(source, diagnostic_codes::truncated,
                                 "MTD entry table is truncated", reader.absolute());
    }
    if (table_bytes != reader.remaining()) {
        return fail<MegaTexture>(source, diagnostic_codes::bounds,
                                 "MTD has unexplained trailing bytes",
                                 sizeof(std::uint32_t) + table_bytes);
    }

    MegaTexture result;
    result.source = std::move(source);
    result.backing_page_stem = std::move(stem.value());
    result.entries.reserve(count);
    for (std::uint32_t index = 0U; index < count; ++index) {
        const auto record_offset = reader.absolute();
        std::span<const std::byte> raw_name;
        if (!reader.bytes(name_bytes, raw_name)) {
            return fail<MegaTexture>(result.source, diagnostic_codes::truncated,
                                     "MTD entry name is truncated", record_offset);
        }
        const auto terminator = std::find(raw_name.begin(), raw_name.end(), std::byte{0});
        if (terminator == raw_name.end() || terminator == raw_name.begin()) {
            return fail<MegaTexture>(result.source, diagnostic_codes::mega_texture_entry,
                                     "MTD entry name must be non-empty and NUL-terminated",
                                     record_offset);
        }
        if (!std::all_of(raw_name.begin(), terminator, printable_ascii)) {
            return fail<MegaTexture>(result.source, diagnostic_codes::mega_texture_entry,
                                     "MTD entry name contains non-ASCII bytes", record_offset);
        }
        if (!std::all_of(terminator, raw_name.end(),
                         [](const std::byte value) { return value == std::byte{0}; })) {
            return fail<MegaTexture>(result.source, diagnostic_codes::mega_texture_entry,
                                     "MTD entry name padding is not zero-filled", record_offset);
        }

        MegaTextureEntry entry;
        entry.name.assign(reinterpret_cast<const char*>(raw_name.data()),
                          static_cast<std::size_t>(terminator - raw_name.begin()));
        std::uint8_t alpha{};
        if (!reader.u32(entry.rectangle.x) || !reader.u32(entry.rectangle.y) ||
            !reader.u32(entry.rectangle.width) || !reader.u32(entry.rectangle.height) ||
            !reader.u8(alpha)) {
            return fail<MegaTexture>(result.source, diagnostic_codes::truncated,
                                     "MTD entry rectangle is truncated", record_offset + name_bytes);
        }
        if (entry.rectangle.width == 0U || entry.rectangle.height == 0U) {
            return fail<MegaTexture>(result.source, diagnostic_codes::mega_texture_entry,
                                     "MTD entry rectangle must not be empty",
                                     record_offset + name_bytes + 8U);
        }
        if (entry.rectangle.x > std::numeric_limits<std::uint32_t>::max() - entry.rectangle.width ||
            entry.rectangle.y > std::numeric_limits<std::uint32_t>::max() - entry.rectangle.height) {
            return fail<MegaTexture>(result.source, diagnostic_codes::bounds,
                                     "MTD entry rectangle overflows its coordinate domain",
                                     record_offset + name_bytes);
        }
        if (alpha > 1U) {
            return fail<MegaTexture>(result.source, diagnostic_codes::mega_texture_entry,
                                     "MTD alpha field must be zero or one",
                                     record_offset + record_bytes - 1U);
        }
        entry.has_alpha = alpha != 0U;
        entry.flip_x = false;
        entry.flip_y = false;
        result.entries.push_back(std::move(entry));
    }
    return core::Result<MegaTexture>::success(std::move(result));
}

core::Result<MegaTexture> load_mega_texture(const vfs::Vfs& filesystem,
                                            const std::string_view logical_path) {
    auto record = filesystem.stat(logical_path);
    if (!record) return core::Result<MegaTexture>::failure(record.error());
    auto bytes = filesystem.open(logical_path);
    if (!bytes) return core::Result<MegaTexture>::failure(bytes.error());
    return load_mega_texture(bytes.value(), source_from(record.value()));
}

core::Result<MegaTextureAtlas> bind_mega_texture(MegaTexture directory,
                                                 Texture page) {
    for (std::size_t index = 0U; index < directory.entries.size(); ++index) {
        const auto& entry = directory.entries[index];
        const auto& rectangle = entry.rectangle;
        // Subtraction after the origin check avoids overflow in x + width and
        // y + height even for caller-constructed directories.
        const bool horizontal = rectangle.x <= page.width &&
            rectangle.width <= page.width - rectangle.x;
        const bool vertical = rectangle.y <= page.height &&
            rectangle.height <= page.height - rectangle.y;
        if (!horizontal || !vertical) {
            std::ostringstream message;
            message << "MTD entry " << index << " ('" << entry.name << "') rectangle "
                    << rectangle.x << ',' << rectangle.y << ' '
                    << rectangle.width << 'x' << rectangle.height
                    << " is outside decoded backing page "
                    << page.width << 'x' << page.height
                    << " from " << page.source.logical_path;
            return fail<MegaTextureAtlas>(
                directory.source, diagnostic_codes::mega_texture_rectangle,
                message.str());
        }
    }
    return core::Result<MegaTextureAtlas>::success(
        MegaTextureAtlas{std::move(directory), std::move(page)});
}

core::Result<MegaTextureAtlas> load_mega_texture_atlas(
    const vfs::Vfs& filesystem, const std::string_view mtd_path,
    const std::string_view page_path) {
    auto directory = load_mega_texture(filesystem, mtd_path);
    if (!directory) return core::Result<MegaTextureAtlas>::failure(directory.error());
    return load_page_and_bind(filesystem, std::move(directory.value()), page_path);
}

core::Result<MegaTextureAtlas> load_mega_texture_atlas(
    const vfs::Vfs& filesystem, const std::string_view mtd_path) {
    auto directory = load_mega_texture(filesystem, mtd_path);
    if (!directory) return core::Result<MegaTextureAtlas>::failure(directory.error());

    const auto dds_path = directory.value().backing_page_stem + ".dds";
    const auto tga_path = directory.value().backing_page_stem + ".tga";
    auto dds = probe_page(filesystem, dds_path);
    if (!dds) return core::Result<MegaTextureAtlas>::failure(dds.error());
    auto tga = probe_page(filesystem, tga_path);
    if (!tga) return core::Result<MegaTextureAtlas>::failure(tga.error());

    if (dds.value() && tga.value()) {
        return fail<MegaTextureAtlas>(
            directory.value().source, diagnostic_codes::mega_texture_page,
            "both sibling .dds and .tga backing pages exist; use the explicit page-path overload");
    }
    if (!dds.value() && !tga.value()) {
        return fail<MegaTextureAtlas>(
            directory.value().source, diagnostic_codes::mega_texture_page,
            "no sibling .dds or .tga backing page exists in the mounted VFS");
    }
    const std::string& page_path = dds.value() ? dds_path : tga_path;
    return load_page_and_bind(filesystem, std::move(directory.value()), page_path);
}
} // namespace eawr::assets
