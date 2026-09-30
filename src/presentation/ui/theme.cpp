#include "eawr/presentation/ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace eawr::presentation::ui {
namespace {

using data::ui::FontRole;
using data::ui::OverrideLevel;

constexpr std::array<FontRole, 9> font_roles{
    FontRole::global_default, FontRole::push_button, FontRole::list_box, FontRole::combo_box,
    FontRole::edit_box,       FontRole::ime_edit_box, FontRole::l_text,  FontRole::r_text,
    FontRole::overlay_caption_text,
};

// Project-authored values for fields a font entry leaves out (ui-layer.md 3.6).
constexpr std::int32_t fallback_point_size = 8;
constexpr data::ui::Rgba fallback_colour{255, 255, 255, 255};

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) noexcept {
    const auto lower = [](const char value) {
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
    };
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(),
                      [&](const char a, const char b) { return lower(a) == lower(b); });
}

[[nodiscard]] std::string variation_name(const std::string_view entry) {
    return std::string(theme_type) + "__" + std::string(entry);
}

class Builder final {
public:
    Builder(const ThemeSources& sources, ThemeModel& model) : sources_(sources), model_(model) {}

    void build() {
        const data::ui::DialogCatalog& catalog = *sources_.catalog;
        const data::ui::DialogSkin& skin = catalog.skin;
        model_.defaults.name = std::string(theme_type);
        model_.defaults.level = OverrideLevel::default_set;
        model_.defaults.entry = "Default";
        model_.defaults.textures = textures(skin.default_textures);
        for (const KitSlot& slot : kit_slots) {
            if (model_.defaults.texture(slot.name) == nullptr) {
                warn(diagnostic_codes::theme_texture, "the Default set has no " + std::string(slot.name)
                         + " slot; the kit draws nothing there", skin.default_textures.line);
            }
        }
        for (const FontRole role : font_roles) {
            const data::ui::FontSpec* spec = skin.default_fonts.find(role);
            if (spec == nullptr) spec = skin.default_fonts.find(FontRole::global_default);
            model_.defaults.fonts.push_back(font(role, spec, spec ? OverrideLevel::default_set : OverrideLevel::none,
                                                 "Default"));
        }

        std::set<std::string, std::less<>> dialogs;
        std::set<std::string, std::less<>> controls;
        for (const data::ui::Dialog& dialog : catalog.script.dialogs) {
            dialogs.insert(dialog.name);
            for (const data::ui::DialogControl& control : dialog.controls) controls.insert(control.id_name);
        }
        // Entry names in file order, textures first; a repeated name keeps its
        // first entry, as the catalogue does.
        std::vector<std::string> names;
        std::set<std::string, std::less<>> seen;
        for (const data::ui::TextureSet& set : skin.texture_overrides) {
            if (seen.insert(set.name).second) names.push_back(set.name);
        }
        for (const data::ui::FontSet& set : skin.font_overrides) {
            if (seen.insert(set.name).second) names.push_back(set.name);
        }
        for (const std::string& name : names) {
            ThemeStyle style;
            if (dialogs.contains(name)) {
                style.level = OverrideLevel::dialog;
            } else if (controls.contains(name)) {
                style.level = OverrideLevel::control;
            } else {
                model_.unmatched.push_back(name);
                continue;
            }
            style.name = variation_name(name);
            style.base = std::string(theme_type);
            style.entry = name;
            if (const data::ui::TextureSet* set = skin.texture_override(name)) style.textures = textures(*set);
            if (const data::ui::FontSet* set = skin.font_override(name)) {
                if (style.level == OverrideLevel::dialog) {
                    for (const FontRole role : font_roles) {
                        if (const data::ui::FontSpec* spec = set->find(role)) {
                            style.fonts.push_back(font(role, spec, OverrideLevel::dialog, name));
                        }
                    }
                } else if (set->spec) {
                    // A control entry is one description for whichever role the
                    // control has, so it stands in for every role.
                    for (const FontRole role : font_roles) {
                        style.fonts.push_back(font(role, &*set->spec, OverrideLevel::control, name));
                    }
                }
            }
            model_.variations.push_back(std::move(style));
        }

        // A control entry inside a dialog with an entry: chain it on the dialog's.
        std::vector<ThemeStyle> chained;
        for (const data::ui::Dialog& dialog : catalog.script.dialogs) {
            const ThemeStyle* outer = model_.find(variation_name(dialog.name));
            if (outer == nullptr || outer->level != OverrideLevel::dialog) continue;
            for (const data::ui::DialogControl& control : dialog.controls) {
                const ThemeStyle* inner = model_.find(variation_name(control.id_name));
                if (inner == nullptr || inner->level != OverrideLevel::control) continue;
                const std::string name = outer->name + "__" + control.id_name;
                const bool exists = std::any_of(chained.begin(), chained.end(),
                                                [&](const ThemeStyle& style) { return style.name == name; });
                if (exists) continue;
                ThemeStyle style = *inner;
                style.name = name;
                style.base = outer->name;
                chained.push_back(std::move(style));
            }
        }
        for (ThemeStyle& style : chained) model_.variations.push_back(std::move(style));
    }

private:
    void warn(const std::string_view code, std::string message, const std::uint32_t line = 0U) {
        core::Diagnostic diagnostic;
        diagnostic.code = std::string(code);
        diagnostic.severity = core::Severity::warning;
        diagnostic.message = std::move(message);
        const assets::Source& source = sources_.catalog->skin.source;
        if (!source.logical_path.empty()) diagnostic.logical_path = source.logical_path;
        if (line != 0U) diagnostic.line = line;
        model_.diagnostics.push_back(std::move(diagnostic));
    }

    [[nodiscard]] std::vector<ThemeTexture> textures(const data::ui::TextureSet& set) {
        std::vector<ThemeTexture> result;
        for (const data::ui::TextureSlot& slot : set.slots) {
            const bool repeated = std::any_of(result.begin(), result.end(),
                                              [&](const ThemeTexture& earlier) { return earlier.slot == slot.slot; });
            if (repeated) continue;
            if (find_kit_slot(slot.slot) == nullptr && unknown_slots_.insert(slot.slot).second) {
                warn(diagnostic_codes::theme_slot, "no kit part draws the " + slot.slot + " slot (" + set.name + ")",
                     slot.line);
            }
            result.push_back(texture(slot));
        }
        return result;
    }

    [[nodiscard]] ThemeTexture texture(const data::ui::TextureSlot& slot) {
        ThemeTexture result;
        result.slot = slot.slot;
        if (slot.texture.empty() || ieq(slot.texture, "none")) {
            result.origin = TextureOrigin::none;
            return result;
        }
        result = resolve_ui_texture(slot.texture, sources_.atlas, sources_.standalone);
        result.slot = slot.slot;
        if (result.origin != TextureOrigin::missing) return result;
        if (missing_textures_.insert(slot.texture).second) {
            warn(diagnostic_codes::theme_texture, slot.texture + " (" + slot.slot
                     + ") is neither in the atlas nor a texture file; the kit draws nothing there",
                 slot.line);
        }
        return result;
    }

    [[nodiscard]] ThemeFont font(const FontRole role, const data::ui::FontSpec* spec, const OverrideLevel level,
                                 const std::string_view entry) {
        ThemeFont result;
        result.role = role;
        result.level = level;
        const data::ui::FontSpec empty;
        const data::ui::FontSpec& source = spec != nullptr ? *spec : empty;
        const auto defaulted = [&](const std::string_view field) { result.defaulted.emplace_back(field); };
        result.face = source.face ? *source.face : std::string(last_resort_face);
        if (!source.face) defaulted("Name");
        result.point_size = source.size ? static_cast<std::int32_t>(std::lround(*source.size)) : fallback_point_size;
        if (!source.size) defaulted("Size");
        result.character_padding = source.character_padding.value_or(0);
        if (!source.character_padding) defaulted("Character_Padding");
        result.stretch_factor = source.stretch_factor.value_or(1.0);
        if (!source.stretch_factor) defaulted("Stretch_Factor");
        result.top_color = source.top_color.value_or(fallback_colour);
        if (!source.top_color) defaulted("Top_Color");
        result.bottom_color = source.bottom_color.value_or(result.top_color);
        if (!source.bottom_color) defaulted("Bottom_Color");
        result.emboss = source.emboss.value_or(false);
        if (!source.emboss) defaulted("Emboss");
        result.outline = source.outline.value_or(false);
        if (!source.outline) defaulted("Outline");
        // Padding, stretch and the flags have neutral values; a missing face,
        // size or colour changes the look, so it is reported once per entry.
        if ((!source.face || !source.size || !source.top_color) && reported_fonts_.insert(std::string(entry)).second) {
            warn(diagnostic_codes::theme_font, "font entry " + std::string(entry)
                     + " leaves out its face, size or colour; project-authored values fill them",
                 source.line);
        }
        result.resolved = resolve_font({result.face, result.point_size}, sources_.language,
                                       sources_.fonts != nullptr ? *sources_.fonts : empty_cache_, sources_.system);
        result.pixels = font_pixels({result.resolved.point_size, false, result.stretch_factor},
                                    model_.font_screen_height);
        if (sources_.fonts != nullptr) {
            const TextCell cell = gdi_text_cell(*sources_.fonts, result.resolved, result.pixels.glyph_height);
            result.cell_ascent = cell.ascent;
            result.cell_descent = cell.descent;
        }
        return result;
    }

    const ThemeSources& sources_;
    ThemeModel& model_;
    const FontCache empty_cache_;
    std::set<std::string, std::less<>> unknown_slots_;
    std::set<std::string, std::less<>> missing_textures_;
    std::set<std::string, std::less<>> reported_fonts_;
};

} // namespace

const std::array<KitSlot, 87> kit_slots{{
    {"Frame_Top_Left", KitPart::frame},
    {"Frame_Top_Transition_Left", KitPart::frame},
    {"Frame_Top", KitPart::frame},
    {"Frame_Top_Transition_Right", KitPart::frame},
    {"Frame_Top_Right", KitPart::frame},
    {"Frame_Right_Transition_Top", KitPart::frame},
    {"Frame_Right", KitPart::frame},
    {"Frame_Right_Transition_Bottom", KitPart::frame},
    {"Frame_Bottom_Right", KitPart::frame},
    {"Frame_Bottom_Transition_Right", KitPart::frame},
    {"Frame_Bottom", KitPart::frame},
    {"Frame_Bottom_Transition_Left", KitPart::frame},
    {"Frame_Bottom_Left", KitPart::frame},
    {"Frame_Left_Transition_Bottom", KitPart::frame},
    {"Frame_Left", KitPart::frame},
    {"Frame_Left_Transition_Top", KitPart::frame},
    {"Frame_Background", KitPart::frame},
    {"Small_Frame_Bottom", KitPart::small_frame},
    {"Small_Frame_Bottom_Left", KitPart::small_frame},
    {"Small_Frame_Bottom_Right", KitPart::small_frame},
    {"Small_Frame_Left", KitPart::small_frame},
    {"Small_Frame_Right", KitPart::small_frame},
    {"Small_Frame_Top", KitPart::small_frame},
    {"Small_Frame_Top_Left", KitPart::small_frame},
    {"Small_Frame_Top_Right", KitPart::small_frame},
    {"Small_Frame_Background", KitPart::small_frame},
    {"Button_Left", KitPart::button},
    {"Button_Middle", KitPart::button},
    {"Button_Right", KitPart::button},
    {"Button_Left_Mouse_Over", KitPart::button},
    {"Button_Middle_Mouse_Over", KitPart::button},
    {"Button_Right_Mouse_Over", KitPart::button},
    {"Button_Left_Pressed", KitPart::button},
    {"Button_Middle_Pressed", KitPart::button},
    {"Button_Right_Pressed", KitPart::button},
    {"Button_Left_Disabled", KitPart::button},
    {"Button_Middle_Disabled", KitPart::button},
    {"Button_Right_Disabled", KitPart::button},
    {"Check_On", KitPart::check},
    {"Check_Off", KitPart::check},
    {"Dial_Left", KitPart::dial},
    {"Dial_Right", KitPart::dial},
    {"Dial_Middle", KitPart::dial},
    {"Dial_Tab", KitPart::dial},
    {"Dial_Plus", KitPart::dial},
    {"Dial_Minus", KitPart::dial},
    {"Dial_Plus_Mouse_Over", KitPart::dial},
    {"Dial_Plus_Pressed", KitPart::dial},
    {"Dial_Minus_Mouse_Over", KitPart::dial},
    {"Dial_Minus_Pressed", KitPart::dial},
    {"Scroll_Down_Button", KitPart::scroll},
    {"Scroll_Down_Button_Pressed", KitPart::scroll},
    {"Scroll_Down_Button_Mouse_Over", KitPart::scroll},
    {"Scroll_Middle", KitPart::scroll},
    {"Scroll_Tab", KitPart::scroll},
    {"Scroll_Up_Button", KitPart::scroll},
    {"Scroll_Up_Button_Pressed", KitPart::scroll},
    {"Scroll_Up_Button_Mouse_Over", KitPart::scroll},
    {"Scroll_Up_Button_Disabled", KitPart::scroll},
    {"Scroll_Down_Button_Disabled", KitPart::scroll},
    {"Scroll_Middle_Disabled", KitPart::scroll},
    {"Scroll_Tab_Disabled", KitPart::scroll},
    {"Trackbar_Scroll_Down_Button", KitPart::trackbar},
    {"Trackbar_Scroll_Down_Button_Pressed", KitPart::trackbar},
    {"Trackbar_Scroll_Down_Button_Mouse_Over", KitPart::trackbar},
    {"Trackbar_Scroll_Middle", KitPart::trackbar},
    {"Trackbar_Scroll_Tab", KitPart::trackbar},
    {"Trackbar_Scroll_Up_Button", KitPart::trackbar},
    {"Trackbar_Scroll_Up_Button_Pressed", KitPart::trackbar},
    {"Trackbar_Scroll_Up_Button_Mouse_Over", KitPart::trackbar},
    {"Trackbar_Scroll_Up_Button_Disabled", KitPart::trackbar},
    {"Trackbar_Scroll_Down_Button_Disabled", KitPart::trackbar},
    {"Trackbar_Scroll_Middle_Disabled", KitPart::trackbar},
    {"Trackbar_Scroll_Tab_Disabled", KitPart::trackbar},
    {"Combo_Box_Popdown_Button", KitPart::combo},
    {"Combo_Box_Popdown_Button_Mouse_Over", KitPart::combo},
    {"Combo_Box_Popdown_Button_Pressed", KitPart::combo},
    {"Combo_Box_Text_Box", KitPart::combo},
    {"Combo_Box_Left_Cap", KitPart::combo},
    {"Progress_Bar_Left", KitPart::progress},
    {"Progress_Bar_Middle_Off", KitPart::progress},
    {"Progress_Bar_Middle_On", KitPart::progress},
    {"Progress_Bar_Right", KitPart::progress},
    {"Radio_On", KitPart::radio},
    {"Radio_Off", KitPart::radio},
    {"Radio_Mouse_Over", KitPart::radio},
    {"Scanlines", KitPart::scanlines},
}};

const KitSlot* find_kit_slot(const std::string_view name) noexcept {
    const auto found = std::find_if(kit_slots.begin(), kit_slots.end(),
                                    [name](const KitSlot& slot) { return slot.name == name; });
    return found == kit_slots.end() ? nullptr : &*found;
}

std::string_view to_string(const KitPart part) noexcept {
    switch (part) {
    case KitPart::frame: return "frame";
    case KitPart::small_frame: return "small_frame";
    case KitPart::button: return "button";
    case KitPart::check: return "check";
    case KitPart::radio: return "radio";
    case KitPart::dial: return "dial";
    case KitPart::scroll: return "scroll";
    case KitPart::trackbar: return "trackbar";
    case KitPart::combo: return "combo";
    case KitPart::progress: return "progress";
    case KitPart::scanlines: return "scanlines";
    }
    return "frame";
}

std::string_view to_string(const TextureOrigin origin) noexcept {
    switch (origin) {
    case TextureOrigin::atlas: return "atlas";
    case TextureOrigin::standalone: return "standalone";
    case TextureOrigin::none: return "none";
    case TextureOrigin::missing: return "missing";
    }
    return "missing";
}

const ThemeTexture* ThemeStyle::texture(const std::string_view slot) const noexcept {
    const auto found = std::find_if(textures.begin(), textures.end(),
                                    [slot](const ThemeTexture& texture) { return texture.slot == slot; });
    return found == textures.end() ? nullptr : &*found;
}

const ThemeFont* ThemeStyle::font(const data::ui::FontRole role) const noexcept {
    const auto found = std::find_if(fonts.begin(), fonts.end(), [role](const ThemeFont& font) { return font.role == role; });
    return found == fonts.end() ? nullptr : &*found;
}

const ThemeStyle* ThemeModel::find(const std::string_view name) const noexcept {
    if (name == defaults.name) return &defaults;
    const auto found = std::find_if(variations.begin(), variations.end(),
                                    [name](const ThemeStyle& style) { return style.name == name; });
    return found == variations.end() ? nullptr : &*found;
}

std::string ThemeModel::variation(const data::ui::Dialog& dialog, const data::ui::DialogControl* control) const {
    const std::string outer = variation_name(dialog.name);
    const ThemeStyle* dialog_style = find(outer);
    const bool has_dialog = dialog_style != nullptr && dialog_style->level == OverrideLevel::dialog;
    if (control != nullptr) {
        if (has_dialog) {
            const std::string chained = outer + "__" + control->id_name;
            if (find(chained) != nullptr) return chained;
        }
        const std::string inner = variation_name(control->id_name);
        const ThemeStyle* control_style = find(inner);
        if (control_style != nullptr && control_style->level == OverrideLevel::control) return inner;
    }
    return has_dialog ? outer : std::string(theme_type);
}

const ThemeTexture* ThemeModel::texture(const std::string_view style, const std::string_view slot) const noexcept {
    const ThemeStyle* current = find(style);
    if (current == nullptr) current = &defaults;
    // Bases only point at earlier styles, so the walk ends; the bound guards data errors.
    for (std::size_t depth = 0U; current != nullptr && depth <= variations.size(); ++depth) {
        if (const ThemeTexture* found = current->texture(slot)) return found;
        current = current->base.empty() ? nullptr : find(current->base);
    }
    return nullptr;
}

const ThemeFont* ThemeModel::font(const std::string_view style, const data::ui::FontRole role) const noexcept {
    const ThemeStyle* current = find(style);
    if (current == nullptr) current = &defaults;
    for (std::size_t depth = 0U; current != nullptr && depth <= variations.size(); ++depth) {
        if (const ThemeFont* found = current->font(role)) return found;
        current = current->base.empty() ? nullptr : find(current->base);
    }
    return nullptr;
}

std::optional<JpegFrame> jpeg_frame(const std::span<const std::byte> bytes) noexcept {
    const auto at = [&](const std::size_t index) { return static_cast<std::uint8_t>(bytes[index]); };
    if (bytes.size() < 4U || at(0) != 0xFFU || at(1) != 0xD8U) return std::nullopt;
    std::size_t index = 2U;
    while (index + 4U <= bytes.size()) {
        if (at(index) != 0xFFU) return std::nullopt;
        const std::uint8_t marker = at(index + 1U);
        if (marker == 0xFFU) { ++index; continue; }  // fill byte
        if (marker == 0xD9U || marker == 0xDAU) return std::nullopt;  // end or scan before any frame
        if (marker == 0x01U || (marker >= 0xD0U && marker <= 0xD7U)) { index += 2U; continue; }  // no length
        const std::size_t length = (static_cast<std::size_t>(at(index + 2U)) << 8U) | at(index + 3U);
        if (length < 2U || index + 2U + length > bytes.size()) return std::nullopt;
        const bool frame = marker >= 0xC0U && marker <= 0xCFU && marker != 0xC4U && marker != 0xC8U && marker != 0xCCU;
        if (frame) {
            if (length < 7U) return std::nullopt;
            const std::uint32_t height = (static_cast<std::uint32_t>(at(index + 5U)) << 8U) | at(index + 6U);
            const std::uint32_t width = (static_cast<std::uint32_t>(at(index + 7U)) << 8U) | at(index + 8U);
            return JpegFrame{width, height};
        }
        index += 2U + length;
    }
    return std::nullopt;
}

std::string jpeg_refusal(const std::span<const std::byte> bytes) {
    if (bytes.size() > max_jpeg_bytes) return "JPEG exceeds " + std::to_string(max_jpeg_bytes) + " bytes";
    const auto frame = jpeg_frame(bytes);
    if (!frame) return "JPEG has no readable frame header";
    const std::uint64_t pixels = static_cast<std::uint64_t>(frame->width) * frame->height;
    if (frame->width == 0U || frame->height == 0U) return "JPEG frame has a zero dimension";
    if (pixels > max_jpeg_pixels) {
        return "JPEG frame " + std::to_string(frame->width) + "x" + std::to_string(frame->height)
            + " exceeds the 8192x8192 pixel budget";
    }
    return {};
}

ThemeTexture resolve_ui_texture(const std::string_view texture, const assets::MegaTexture* atlas,
                                const StandaloneTextures& standalone) {
    ThemeTexture result;
    result.texture = std::string(texture);
    if (atlas != nullptr) {
        if (const assets::MegaTextureEntry* entry = atlas->find(texture)) {
            result.origin = TextureOrigin::atlas;
            result.rectangle = entry->rectangle;
            result.has_alpha = entry->has_alpha;
            return result;
        }
    }
    if (standalone) {
        if (auto path = standalone(texture)) {
            result.origin = TextureOrigin::standalone;
            result.logical_path = std::move(*path);
            return result;
        }
    }
    result.origin = TextureOrigin::missing;
    return result;
}

StandaloneTextures vfs_standalone_textures(const vfs::Vfs& filesystem) {
    return [&filesystem](const std::string_view texture) -> std::optional<std::string> {
        std::string_view stem = texture;
        for (const std::string_view suffix : {".tga", ".dds"}) {
            if (stem.size() > suffix.size() && ieq(stem.substr(stem.size() - suffix.size()), suffix)) {
                stem.remove_suffix(suffix.size());
                break;
            }
        }
        for (const std::string_view suffix : {".tga", ".dds"}) {
            std::string path = "Data/Art/Textures/" + std::string(stem) + std::string(suffix);
            if (filesystem.stat(path)) return path;
        }
        // Mods also name other image files (the Remake's .jpg menu background).
        std::string written = "Data/Art/Textures/" + std::string(texture);
        if (filesystem.stat(written)) return written;
        return std::nullopt;
    };
}

ThemeModel build_theme_model(const ThemeSources& sources, const ReferenceSpace& space) {
    ThemeModel model;
    model.space = space;
    // The UI-L3 scale of one `.rc` unit, which texture pieces follow.
    const PixelRect unit = scale_rc(RcRect{0, 0, 1, 1}, space);
    model.scale_x = unit.width;
    model.scale_y = unit.height;
    model.font_screen_height = font_screen_height(space);
    model.language = sources.language;
    if (sources.atlas != nullptr) model.atlas = sources.atlas->source.logical_path;
    if (sources.catalog == nullptr) return model;
    Builder(sources, model).build();
    return model;
}

} // namespace eawr::presentation::ui
