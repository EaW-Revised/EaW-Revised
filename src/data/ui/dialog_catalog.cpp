#include "eawr/data/ui/dialog_catalog.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace eawr::data::ui {
namespace {
using assets::Source;

constexpr std::array<std::string_view, 9> role_names{
    "Global_Default", "Push_Button", "List_Box", "Combo_Box", "Edit_Box",
    "IME_Edit_Box", "L_Text", "R_Text", "Overlay_Caption_Text",
};
constexpr std::array<std::string_view, 8> font_fields{
    "Name", "Size", "Character_Padding", "Stretch_Factor",
    "Top_Color", "Bottom_Color", "Emboss", "Outline",
};

char fold(const char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}
bool iequals(const std::string_view left, const std::string_view right) {
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(),
                   [](const char a, const char b) { return fold(a) == fold(b); });
}
std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), fold);
    return result;
}
std::string_view trim(std::string_view text) {
    const auto space = [](const char value) {
        return value == ' ' || value == '\t' || value == '\r' || value == '\n';
    };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return text;
}

core::Diagnostic make_diagnostic(const Source& source, const std::string_view code,
                                 const core::Severity severity, std::string message,
                                 const std::uint32_t line = 0U) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = severity;
    diagnostic.message = std::move(message);
    diagnostic.logical_path = source.logical_path;
    if (line != 0U) diagnostic.line = line;
    diagnostic.source_id = source.source_id;
    return diagnostic;
}

class SkinReader final {
public:
    SkinReader(const std::span<const std::byte> bytes, DialogSkin& skin) : skin_(skin) {
        line_starts_.push_back(0U);
        for (std::size_t index = 0U; index < bytes.size(); ++index) {
            if (bytes[index] == std::byte{'\n'}) line_starts_.push_back(index + 1U);
        }
    }

    std::uint32_t line(const pugi::xml_node node) const {
        const auto offset = node.offset_debug();
        if (offset < 0) return 0U;
        const auto found = std::upper_bound(line_starts_.begin(), line_starts_.end(),
                                            static_cast<std::size_t>(offset));
        return static_cast<std::uint32_t>(found - line_starts_.begin());
    }

    void warn(const pugi::xml_node node, std::string message) {
        skin_.diagnostics.push_back(make_diagnostic(skin_.source, diagnostic_codes::skin_value,
                                                    core::Severity::warning, std::move(message),
                                                    line(node)));
    }

    static std::string text(const pugi::xml_node node) {
        return std::string(trim(node.text().get()));
    }

    TextureSet textures(const pugi::xml_node node) {
        TextureSet set{node.name(), {}, line(node)};
        for (const auto slot : node.children()) {
            if (slot.type() != pugi::node_element) continue;
            if (slot.first_child().type() == pugi::node_element) {
                warn(slot, std::string("texture slot '") + slot.name() + "' has child elements");
                continue;
            }
            set.slots.push_back({slot.name(), text(slot), line(slot)});
        }
        return set;
    }

    std::optional<double> number(const pugi::xml_node node) {
        const auto value = text(node);
        double parsed{};
        const auto* begin = value.data();
        const auto* end = begin + value.size();
        const auto result = std::from_chars(begin, end, parsed);
        if (value.empty() || result.ec != std::errc{} || result.ptr != end || !std::isfinite(parsed)) {
            warn(node, std::string(node.name()) + " '" + value + "' is not a number");
            return std::nullopt;
        }
        return parsed;
    }

    std::optional<Rgba> color(const pugi::xml_node node) {
        const auto value = text(node);
        std::array<int, 4> parts{};
        std::size_t count = 0U;
        bool valid = true;
        std::string_view rest = value;
        while (valid) {
            const auto comma = rest.find(',');
            const auto part = trim(rest.substr(0U, comma));
            int parsed{};
            const auto result = std::from_chars(part.data(), part.data() + part.size(), parsed);
            valid = count < parts.size() && !part.empty() && result.ec == std::errc{} &&
                result.ptr == part.data() + part.size() && parsed >= 0 && parsed <= 255;
            if (valid) parts[count++] = parsed;
            if (comma == std::string_view::npos) break;
            rest = rest.substr(comma + 1U);
        }
        if (!valid || count != parts.size()) {
            warn(node, std::string(node.name()) + " '" + value + "' is not four 0-255 components");
            return std::nullopt;
        }
        return Rgba{static_cast<std::uint8_t>(parts[0]), static_cast<std::uint8_t>(parts[1]),
                    static_cast<std::uint8_t>(parts[2]), static_cast<std::uint8_t>(parts[3])};
    }

    std::optional<bool> flag(const pugi::xml_node node) {
        const auto value = text(node);
        if (iequals(value, "yes") || iequals(value, "true")) return true;
        if (iequals(value, "no") || iequals(value, "false")) return false;
        warn(node, std::string(node.name()) + " '" + value + "' is not Yes or No");
        return std::nullopt;
    }

    FontSpec font(const pugi::xml_node node) {
        FontSpec spec;
        spec.line = line(node);
        for (const auto field : node.children()) {
            if (field.type() != pugi::node_element) continue;
            const std::string_view name = field.name();
            if (name == "Name") {
                spec.face = text(field);
            } else if (name == "Size") {
                spec.size = number(field);
            } else if (name == "Character_Padding") {
                // Range-checked first: converting a double outside int32 is undefined.
                const auto value = number(field);
                if (value && *value != std::floor(*value)) {
                    warn(field, "Character_Padding is not an integer");
                } else if (value && (*value < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
                                     *value > static_cast<double>(std::numeric_limits<std::int32_t>::max()))) {
                    warn(field, "Character_Padding '" + text(field) + "' is out of the 32-bit range");
                } else if (value) {
                    spec.character_padding = static_cast<std::int32_t>(*value);
                }
            } else if (name == "Stretch_Factor") {
                spec.stretch_factor = number(field);
            } else if (name == "Top_Color") {
                spec.top_color = color(field);
            } else if (name == "Bottom_Color") {
                spec.bottom_color = color(field);
            } else if (name == "Emboss") {
                spec.emboss = flag(field);
            } else if (name == "Outline") {
                spec.outline = flag(field);
            } else {
                warn(field, "unknown font field '" + std::string(name) + "'");
            }
        }
        return spec;
    }

    FontSet fonts(const pugi::xml_node node, const bool default_set) {
        FontSet set{node.name(), std::nullopt, {}, line(node)};
        bool direct = false;
        for (const auto child : node.children()) {
            if (child.type() != pugi::node_element) continue;
            direct = direct || std::find(font_fields.begin(), font_fields.end(),
                                         std::string_view(child.name())) != font_fields.end();
        }
        if (direct && !default_set) {
            set.spec = font(node);
            return set;
        }
        for (const auto child : node.children()) {
            if (child.type() != pugi::node_element) continue;
            const auto role = font_role_from_name(child.name());
            if (!role) {
                warn(child, std::string("unknown font role '") + child.name() + "' in " + set.name);
                continue;
            }
            set.roles.push_back({*role, font(child)});
        }
        return set;
    }

private:
    DialogSkin& skin_;
    std::vector<std::size_t> line_starts_;
};

template <typename Entry>
void report_duplicates(DialogSkin& skin, const std::string_view section,
                       const std::vector<Entry>& entries) {
    std::map<std::string, std::vector<std::uint32_t>> lines;
    for (const auto& entry : entries) lines[entry.name].push_back(entry.line);
    for (const auto& [name, where] : lines) {
        if (where.size() < 2U) continue;
        std::ostringstream message;
        message << section << " entry '" << name << "' appears " << where.size()
                << " times (lines";
        for (const auto line : where) message << ' ' << line;
        message << "); the first entry wins";
        skin.diagnostics.push_back(make_diagnostic(skin.source, diagnostic_codes::override_duplicate,
                                                   core::Severity::warning, message.str(),
                                                   where.front()));
    }
}

template <typename Entry>
const Entry* first_named(const std::vector<Entry>& entries, const std::string_view name) {
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [name](const Entry& entry) { return entry.name == name; });
    return found == entries.end() ? nullptr : &*found;
}

struct Unmatched final {
    std::string_view section;
    std::string name;
    std::uint32_t line{};
};

std::vector<Unmatched> unmatched_overrides(const DialogScript& script, const DialogSkin& skin) {
    std::set<std::string, std::less<>> names;
    for (const auto& dialog : script.dialogs) {
        names.insert(dialog.name);
        for (const auto& control : dialog.controls) names.insert(control.id_name);
    }
    std::vector<Unmatched> result;
    std::set<std::pair<std::string_view, std::string>> seen;
    const auto check = [&](const std::string_view section, const std::string& name,
                           const std::uint32_t line) {
        if (names.contains(name) || !seen.emplace(section, name).second) return;
        result.push_back({section, name, line});
    };
    for (const auto& set : skin.texture_overrides) check("Textures", set.name, set.line);
    for (const auto& set : skin.font_overrides) check("Fonts", set.name, set.line);
    for (const auto& tip : skin.tooltips) check("Tooltips", tip.name, tip.line);
    return result;
}

bool key_shaped(const std::string_view text) {
    if (text.empty() || !(text.front() >= 'A' && text.front() <= 'Z')) return false;
    bool underscore = false;
    for (const char character : text) {
        if (character == '_') {
            underscore = true;
        } else if (!((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9'))) {
            return false;
        }
    }
    return underscore;
}

core::Result<std::string> read_text(const vfs::Vfs& filesystem, const std::string_view path,
                                    Source& source) {
    auto record = filesystem.stat(path);
    if (!record) return core::Result<std::string>::failure(record.error());
    auto bytes = filesystem.open(path);
    if (!bytes) return core::Result<std::string>::failure(bytes.error());
    source = assets::source_from(record.value());
    return core::Result<std::string>::success(
        std::string(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
}
} // namespace

std::string_view to_string(const FontRole role) noexcept {
    return role_names[static_cast<std::size_t>(role)];
}

std::optional<FontRole> font_role_from_name(const std::string_view name) noexcept {
    for (std::size_t index = 0U; index < role_names.size(); ++index) {
        if (role_names[index] == name) return static_cast<FontRole>(index);
    }
    return std::nullopt;
}

std::string_view to_string(const OverrideLevel level) noexcept {
    switch (level) {
    case OverrideLevel::none: return "none";
    case OverrideLevel::default_set: return "default";
    case OverrideLevel::dialog: return "dialog";
    case OverrideLevel::control: return "control";
    }
    return "none";
}

std::string_view to_string(const CaptionKind kind) noexcept {
    switch (kind) {
    case CaptionKind::empty: return "empty";
    case CaptionKind::text_key: return "text_key";
    case CaptionKind::missing_key: return "missing_key";
    case CaptionKind::literal: return "literal";
    }
    return "literal";
}

const TextureSlot* TextureSet::find(const std::string_view slot) const noexcept {
    const auto found = std::find_if(slots.begin(), slots.end(),
                                    [slot](const TextureSlot& entry) { return entry.slot == slot; });
    return found == slots.end() ? nullptr : &*found;
}

const FontSpec* FontSet::find(const FontRole role) const noexcept {
    const auto found = std::find_if(roles.begin(), roles.end(),
                                    [role](const RoleFont& entry) { return entry.role == role; });
    return found == roles.end() ? nullptr : &found->spec;
}

const TextureSet* DialogSkin::texture_override(const std::string_view name) const noexcept {
    return first_named(texture_overrides, name);
}
const FontSet* DialogSkin::font_override(const std::string_view name) const noexcept {
    return first_named(font_overrides, name);
}
const TooltipEntry* DialogSkin::tooltip(const std::string_view name) const noexcept {
    return first_named(tooltips, name);
}

core::Result<DialogSkin> parse_dialog_skin(const std::span<const std::byte> bytes, Source source) {
    DialogSkin skin;
    skin.source = std::move(source);
    pugi::xml_document document;
    const auto parsed = document.load_buffer(bytes.data(), bytes.size(), pugi::parse_default,
                                             pugi::encoding_auto);
    if (!parsed) {
        return core::Result<DialogSkin>::failure(make_diagnostic(
            skin.source, diagnostic_codes::skin_xml, core::Severity::error,
            std::string("malformed XML: ") + parsed.description()));
    }
    const auto root = document.document_element();
    if (std::string_view(root.name()) != "GUIDialogs") {
        return core::Result<DialogSkin>::failure(make_diagnostic(
            skin.source, diagnostic_codes::skin_xml, core::Severity::error,
            std::string("root element is '") + root.name() + "', expected GUIDialogs"));
    }
    SkinReader reader(bytes, skin);
    bool default_textures = false;
    bool default_fonts = false;
    for (const auto section : root.children()) {
        if (section.type() != pugi::node_element) continue;
        const std::string_view name = section.name();
        if (name == "Textures") {
            skin.texture_file = section.attribute("File").as_string();
            skin.compressed_texture_file = section.attribute("Compressed_File").as_string();
            for (const auto set : section.children()) {
                if (set.type() != pugi::node_element) continue;
                if (std::string_view(set.name()) == "Default" && !default_textures) {
                    skin.default_textures = reader.textures(set);
                    default_textures = true;
                } else {
                    skin.texture_overrides.push_back(reader.textures(set));
                }
            }
        } else if (name == "Fonts") {
            for (const auto set : section.children()) {
                if (set.type() != pugi::node_element) continue;
                if (std::string_view(set.name()) == "Default" && !default_fonts) {
                    skin.default_fonts = reader.fonts(set, true);
                    default_fonts = true;
                } else {
                    skin.font_overrides.push_back(reader.fonts(set, false));
                }
            }
        } else if (name == "Tooltips") {
            for (const auto tip : section.children()) {
                if (tip.type() != pugi::node_element) continue;
                const auto text_id = tip.child("TextID");
                if (!text_id) reader.warn(tip, std::string("tooltip '") + tip.name() + "' has no TextID");
                skin.tooltips.push_back({tip.name(), SkinReader::text(text_id), reader.line(tip)});
            }
        } else {
            reader.warn(section, "unknown GUIDialogs section '" + std::string(name) + "'");
        }
    }
    report_duplicates(skin, "Textures", skin.texture_overrides);
    report_duplicates(skin, "Fonts", skin.font_overrides);
    report_duplicates(skin, "Tooltips", skin.tooltips);
    return core::Result<DialogSkin>::success(std::move(skin));
}

FontRole font_role(const DialogControl& control) noexcept {
    const std::string_view statement = control.statement;
    if (statement == "PUSHBUTTON" || statement == "DEFPUSHBUTTON" || statement == "PUSHBOX") {
        return FontRole::push_button;
    }
    if (statement == "LISTBOX") return FontRole::list_box;
    if (statement == "COMBOBOX") return FontRole::combo_box;
    if (statement == "EDITTEXT") return FontRole::edit_box;
    if (statement == "LTEXT") return FontRole::l_text;
    if (statement == "RTEXT") return FontRole::r_text;
    if (statement == "CONTROL" && iequals(control.class_name, "IMEEditBox")) return FontRole::ime_edit_box;
    return FontRole::global_default;
}

TextureResolution DialogCatalog::texture(const Dialog& dialog, const DialogControl* control,
                                         const std::string_view slot) const {
    const auto resolved = [](const OverrideLevel level, const TextureSlot& entry) {
        TextureResolution result{level, std::nullopt};
        if (!iequals(entry.texture, "none") && !entry.texture.empty()) result.texture = entry.texture;
        return result;
    };
    if (control != nullptr) {
        if (const auto* set = skin.texture_override(control->id_name)) {
            if (const auto* entry = set->find(slot)) return resolved(OverrideLevel::control, *entry);
        }
    }
    if (const auto* set = skin.texture_override(dialog.name)) {
        if (const auto* entry = set->find(slot)) return resolved(OverrideLevel::dialog, *entry);
    }
    if (const auto* entry = skin.default_textures.find(slot)) {
        return resolved(OverrideLevel::default_set, *entry);
    }
    return {};
}

FontResolution DialogCatalog::font(const Dialog& dialog, const DialogControl& control) const {
    const auto role = font_role(control);
    if (const auto* set = skin.font_override(control.id_name); set != nullptr && set->spec) {
        return {role, OverrideLevel::control, &*set->spec};
    }
    if (const auto* set = skin.font_override(dialog.name)) {
        if (const auto* spec = set->find(role)) return {role, OverrideLevel::dialog, spec};
    }
    if (const auto* spec = skin.default_fonts.find(role)) return {role, OverrideLevel::default_set, spec};
    if (const auto* spec = skin.default_fonts.find(FontRole::global_default)) {
        return {role, OverrideLevel::default_set, spec};
    }
    return {role, OverrideLevel::none, nullptr};
}

const TooltipEntry* DialogCatalog::tooltip(const DialogControl& control) const noexcept {
    return skin.tooltip(control.id_name);
}

DialogCatalog build_dialog_catalog(ResourceSymbols symbols, DialogScript script, DialogSkin skin) {
    resolve_ids(script, symbols);
    DialogCatalog catalog{std::move(symbols), std::move(script), std::move(skin), {}};
    for (const auto& entry : unmatched_overrides(catalog.script, catalog.skin)) {
        catalog.diagnostics.push_back(make_diagnostic(
            catalog.skin.source, diagnostic_codes::override_unmatched, core::Severity::warning,
            std::string(entry.section) + " entry '" + entry.name +
                "' names no dialog or control in " + catalog.script.source.logical_path,
            entry.line));
    }
    return catalog;
}

core::Result<DialogCatalog> load_dialog_catalog(const vfs::Vfs& filesystem) {
    Source script_source;
    auto script_text = read_text(filesystem, dialog_script_path, script_source);
    if (!script_text) return core::Result<DialogCatalog>::failure(script_text.error());
    auto script = parse_dialog_script(script_text.value(), script_source);
    if (!script) return core::Result<DialogCatalog>::failure(script.error());

    const auto folder = dialog_script_path.substr(0U, dialog_script_path.find_last_of('/') + 1U);
    std::optional<ResourceSymbols> symbols;
    for (const auto& include : script.value().includes) {
        const auto path = std::string(folder) + include;
        if (!filesystem.stat(path)) continue;
        Source header_source;
        auto header_text = read_text(filesystem, path, header_source);
        if (!header_text) return core::Result<DialogCatalog>::failure(header_text.error());
        auto parsed = parse_resource_header(header_text.value(), header_source);
        if (!parsed) return core::Result<DialogCatalog>::failure(parsed.error());
        symbols = std::move(parsed.value());
        break;
    }
    if (!symbols) {
        return core::Result<DialogCatalog>::failure(make_diagnostic(
            script.value().source, diagnostic_codes::rc_symbol, core::Severity::error,
            "the dialog script includes no resource header that exists next to it"));
    }

    auto skin_record = filesystem.stat(dialog_skin_path);
    if (!skin_record) return core::Result<DialogCatalog>::failure(skin_record.error());
    auto skin_bytes = filesystem.open(dialog_skin_path);
    if (!skin_bytes) return core::Result<DialogCatalog>::failure(skin_bytes.error());
    auto skin = parse_dialog_skin(skin_bytes.value(), assets::source_from(skin_record.value()));
    if (!skin) return core::Result<DialogCatalog>::failure(skin.error());
    return core::Result<DialogCatalog>::success(build_dialog_catalog(
        std::move(*symbols), std::move(script.value()), std::move(skin.value())));
}

CaptionKind classify_caption(const std::string_view text, const TextDatabase& database) {
    if (text.empty()) return CaptionKind::empty;
    if (database.find(text) != nullptr) return CaptionKind::text_key;
    return key_shaped(text) ? CaptionKind::missing_key : CaptionKind::literal;
}

TextureProbe vfs_texture_probe(const vfs::Vfs& filesystem) {
    return [&filesystem](const std::string_view texture) {
        std::string_view stem = texture;
        for (const std::string_view suffix : {".tga", ".dds"}) {
            if (stem.size() > suffix.size() && iequals(stem.substr(stem.size() - suffix.size()), suffix)) {
                stem.remove_suffix(suffix.size());
                break;
            }
        }
        for (const std::string_view suffix : {".tga", ".dds"}) {
            if (filesystem.stat("Data/Art/Textures/" + std::string(stem) + std::string(suffix))) return true;
        }
        return false;
    };
}

CatalogAudit audit_dialog_catalog(const DialogCatalog& catalog, const assets::MegaTexture& atlas,
                                  const TextDatabase& text, const TextureProbe& standalone) {
    CatalogAudit audit;
    std::set<std::string> missing_keys;
    std::set<std::string> literals;
    std::set<std::string> unresolved_ids;
    for (const auto& dialog : catalog.script.dialogs) {
        ++audit.dialogs;
        ++audit.dialog_captions[classify_caption(dialog.caption.value_or(""), text)];
        if (!dialog.id) unresolved_ids.insert(dialog.name);
        for (const auto& control : dialog.controls) {
            ++audit.controls;
            ++audit.statements[control.statement];
            if (control.statement == "CONTROL") ++audit.control_classes[control.class_name];
            if (!control.id) unresolved_ids.insert(control.id_name);
            if (control.text) {
                const auto kind = classify_caption(*control.text, text);
                ++audit.control_captions[kind];
                if (kind == CaptionKind::missing_key) missing_keys.insert(*control.text);
                if (kind == CaptionKind::literal) literals.insert(*control.text);
            }
            const auto font = catalog.font(dialog, control);
            const std::string where = dialog.name + "/" + control.id_name;
            if (font.spec == nullptr) {
                audit.unresolved_fonts.push_back(where + ": no font for role " +
                                                 std::string(to_string(font.role)));
            } else if (!font.spec->face || font.spec->face->empty() || !font.spec->size) {
                audit.unresolved_fonts.push_back(where + ": " + std::string(to_string(font.level)) +
                                                 " font has no face or size");
            } else {
                ++audit.font_faces[*font.spec->face];
                ++audit.font_levels[font.level];
            }
        }
    }
    audit.missing_caption_keys.assign(missing_keys.begin(), missing_keys.end());
    audit.literal_captions.assign(literals.begin(), literals.end());
    audit.unresolved_ids.assign(unresolved_ids.begin(), unresolved_ids.end());

    std::map<std::string, TextureAudit> textures;
    const auto count = [&](const TextureSet& set) {
        for (const auto& slot : set.slots) {
            if (slot.texture.empty() || iequals(slot.texture, "none")) {
                ++audit.none_slots;
                continue;
            }
            auto& entry = textures[lower(slot.texture)];
            if (entry.references++ == 0U) entry.texture = slot.texture;
        }
    };
    count(catalog.skin.default_textures);
    for (const auto& set : catalog.skin.texture_overrides) count(set);
    for (auto& [key, entry] : textures) {
        entry.in_atlas = atlas.find(entry.texture) != nullptr;
        entry.standalone = !entry.in_atlas && standalone && standalone(entry.texture);
        if (!entry.in_atlas && !entry.standalone) audit.unresolved_textures.push_back(entry.texture);
        audit.textures.push_back(entry);
    }

    for (const auto& tip : catalog.skin.tooltips) {
        if (text.find(tip.text_id) == nullptr) audit.unresolved_tooltip_keys.push_back(tip.text_id);
    }
    for (const auto& entry : unmatched_overrides(catalog.script, catalog.skin)) {
        audit.unmatched_overrides.push_back(std::string(entry.section) + ":" + entry.name);
    }
    const auto duplicates = [&](const std::string_view section, const auto& entries) {
        std::map<std::string, std::size_t> seen;
        for (const auto& entry : entries) {
            if (++seen[entry.name] == 2U) {
                audit.duplicate_overrides.push_back(std::string(section) + ":" + entry.name);
            }
        }
    };
    duplicates("Textures", catalog.skin.texture_overrides);
    duplicates("Fonts", catalog.skin.font_overrides);
    duplicates("Tooltips", catalog.skin.tooltips);
    return audit;
}

} // namespace eawr::data::ui
