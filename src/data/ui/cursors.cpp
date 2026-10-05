#include "eawr/data/ui/cursors.hpp"
#include "eawr/data/xml.hpp"
#include "ui_internal.hpp"

#include <charconv>
#include <limits>
#include <string_view>
#include <utility>

namespace eawr::data::ui {
namespace {
std::string trim(std::string_view value) {
    constexpr std::string_view spaces = " \t\r\n";
    const auto begin = value.find_first_not_of(spaces);
    if (begin == std::string_view::npos) return {};
    return std::string(value.substr(begin, value.find_last_not_of(spaces) - begin + 1));
}
core::Diagnostic invalid(std::string message) {
    return {"EAWR-UI-CURSOR-0001", core::Severity::error, std::move(message), {}, {}, {}, {}};
}
std::string texture_path(const vfs::Vfs& filesystem, std::string name) {
    if (!name.starts_with("data/")) name = "data/art/textures/" + name;
    if (filesystem.stat(name)) return name;
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos && detail::iequals(std::string_view(name).substr(dot), ".tga")) {
        name.replace(dot, std::string::npos, ".dds");
        if (filesystem.stat(name)) return name;
    }
    return {};
}
}

core::Result<CursorCatalog> load_cursors(const vfs::Vfs& filesystem) {
    constexpr std::string_view registry = "data/xml/mousepointerfiles.xml";
    auto list = data::load_document(filesystem, registry);
    if (!list) return core::Result<CursorCatalog>::failure(list.error());
    CursorCatalog result;
    for (const auto& entry : list.value().root.children) {
        if (!detail::iequals(entry.name, "File")) continue;
        auto document = data::load_document(filesystem, data::registry_include_path(registry, trim(entry.raw_text)));
        if (!document) return core::Result<CursorCatalog>::failure(document.error());
        for (const auto& node : document.value().root.children) {
            if (!detail::iequals(node.name, "MousePointer")) continue;
            CursorDefinition pointer;
            for (const auto& attribute : node.attributes) {
                if (detail::iequals(attribute.name, "Name")) pointer.name = trim(attribute.value);
            }
            for (const auto& field : node.children) {
                const auto text = trim(field.raw_text);
                if (detail::iequals(field.name, "Base_Texture")) { pointer.base_texture = text; continue; }
                std::uint32_t* output = nullptr;
                if (detail::iequals(field.name, "Hot_X")) output = &pointer.hot_x;
                if (detail::iequals(field.name, "Hot_Y")) output = &pointer.hot_y;
                if (detail::iequals(field.name, "Anim_Frame_Delay")) output = &pointer.frame_delay;
                if (output == nullptr) continue;
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *output);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
                    || *output == std::numeric_limits<std::uint32_t>::max()) {
                    return core::Result<CursorCatalog>::failure(invalid("invalid pointer field " + field.name));
                }
            }
            if (pointer.name.empty() || pointer.base_texture.empty()) {
                return core::Result<CursorCatalog>::failure(invalid("pointer needs Name and Base_Texture"));
            }
            // CU-01: numbered consecutive frames, stopping at the first gap.
            const auto slot = pointer.base_texture.find("00");
            for (unsigned frame = 0; frame < (slot == std::string::npos ? 1U : 100U); ++frame) {
                std::string name = pointer.base_texture;
                if (slot != std::string::npos) {
                    name[slot] = static_cast<char>('0' + frame / 10);
                    name[slot + 1] = static_cast<char>('0' + frame % 10);
                }
                auto path = texture_path(filesystem, std::move(name));
                if (path.empty()) break;
                pointer.frames.push_back(std::move(path));
            }
            const auto name = pointer.name;
            result.insert_or_assign(name, std::move(pointer));
        }
    }
    return core::Result<CursorCatalog>::success(std::move(result));
}

} // namespace eawr::data::ui
