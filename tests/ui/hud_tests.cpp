#include "eawr/presentation/ui/hud.hpp"
#include "ui_test_support.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace {
using namespace eawr;
namespace ui = presentation::ui;
using test::ui::expect;

std::string numbered(std::string_view stem, unsigned i) {
    return std::string(stem)+(i < 10 ? "0" : "")+std::to_string(i);
}
assets::Mesh quad(std::string name, float width, float height, std::string texture = {}) {
    assets::Mesh mesh;
    mesh.name = std::move(name); mesh.bone = 0;
    assets::Submesh sub;
    for (const auto p : std::array<assets::Vec2f,4>{{{0,0},{width,0},{width,height},{0,height}}}) {
        assets::Vertex vertex;
        vertex.position = {p.x,p.y,0};
        vertex.texcoord[0] = {p.x/width,-p.y/height};
        sub.vertices.push_back(vertex);
    }
    sub.indices = {0,1,2,0,2,3};
    sub.parameters.push_back({"BaseTexture",assets::ParameterKind::texture,std::move(texture)});
    mesh.submeshes.push_back(std::move(sub));
    return mesh;
}
assets::Model model() {
    assets::Model out;
    assets::Bone root;
    root.name = "root";
    root.relative_transform = {1,0,0,0, 0,1,0,0, 0,0,1,0};
    out.bones.push_back(root);
    return out;
}
data::ui::CommandBarCatalog commandbar_mod_edits() {
    std::string xml = "<CommandBarComponents>";
    xml += "<CommandBarComponent Name='i_main_skirmish'><Type>Shell</Type><Model_Name>swapped.alo</Model_Name></CommandBarComponent>";
    for (unsigned i = 0; i < 36; ++i)
        xml += "<CommandBarComponent Name='"+numbered("s_select_",i)+"'><Type>Button</Type></CommandBarComponent>";
    xml += "<CommandBarComponent Name='hero'><Type>Button</Type><Group>NewGroup</Group></CommandBarComponent>";
    xml += "<CommandBarComponent Name='hero'><Type>Button</Type><Group>ReplacementGroup</Group></CommandBarComponent>";
    xml += "</CommandBarComponents>";
    data::ui::CommandBarCatalog catalog;
    std::vector<core::Diagnostic> diagnostics;
    vfs::AssetRecord record;
    record.canonical_path = "data/xml/synthetic.xml";
    auto result = data::ui::parse_command_bar_components(std::as_bytes(std::span(xml.data(),xml.size())),record,catalog,diagnostics);
    expect(result.has_value(),"commandbar_mod_edits parses");
    expect(diagnostics.size() == 1 && diagnostics[0].code == data::ui::diagnostic_codes::component_duplicate,
        "only the deliberate duplicate is diagnosed");
    expect(catalog.find("hero") && catalog.find("hero")->in_group("ReplacementGroup"),"last duplicate and new group survive");
    expect(!catalog.find("text_fleet1"),"removed component stays absent");
    expect(catalog.shell_for_model("swapped.alo") != nullptr,"shell Model_Name swap is data");
    return catalog;
}
void shell_widescreen_card() {
    auto source = model();
    source.meshes.push_back(quad("Empire_Faceplate_ALT0",1352,1081,"card"));
    source.meshes.push_back(quad("Rebel_Faceplate_ALT1",2000,1081,"card"));
    for (unsigned i = 0; i < 36; ++i) source.meshes.push_back(quad(numbered("s_select_",i),10,10));
    source.meshes.push_back(quad("hero",10,20));
    source.meshes.back().bone = 1;
    auto hero = source.bones.front();
    hero.name = "hero"; hero.parent = 0;
    hero.relative_transform = {0,-1,0,100, 1,0,0,50, 0,0,1,0};
    source.bones.push_back(hero);
    const auto catalog = commandbar_mod_edits();
    auto mask = std::make_shared<ui::ShellAlphaMask>();
    mask->width = 4; mask->height = 4; mask->alpha.resize(16);
    std::fill(mask->alpha.begin()+12,mask->alpha.end(),std::uint8_t{255});
    mask->alpha[13] = 0; // a transparent hole within the visible bounding box
    const ui::ShellMaskLookup masks = [&](std::string_view) { return mask; };
    const auto anchors = data::ui::shell_anchors(source);
    expect(anchors.diagnostics.empty(),"rotated hero is supported without EAWR-UI-0312");
    auto hud = ui::hud_view_model(anchors.shell,catalog,0,masks);
    expect(hud.family_size("S_SELECT_") == 36,"36 slots from shell/catalogue intersection, case insensitive");
    expect(hud.family_size("special_border_") == 0,"absent family is empty");
    expect(hud.visible_extent && hud.visible_extent->right() == 1352 &&
        std::abs(hud.visible_extent->top()-270.25F) < 0.001F,"opaque bottom band, not 1081-unit card, sets extent");
    expect(hud.hit_test({1200,100}) && !hud.hit_test({1200,600}) && !hud.hit_test({500,100}),"opaque face blocks input, transparent card passes it");
    expect(mask->opaque(1.1,-0.1) && !mask->opaque(-0.1,-0.6),"both UV axes repeat across negative and positive tiles");
    const auto narrow = ui::reference_space({1024,768});
    expect(hud.placement(narrow).left == 0 && hud.placement(narrow).scale == 1,"overwide shell left-anchors and clips");
    expect(!hud.hit_test_screen({1200,700},narrow),"hit test clips to viewport");
    expect(hud.hit_test_screen({1000,668},narrow),"screen hit test uses shell placement");
    const auto wide = ui::reference_space({2560,1080});
    expect(hud.placement(wide) == ui::place_shell(wide,1352),"existing safe-area placement uses visible right extent");
    // Bound components can exceed the faceplate.
    auto extra = anchors.shell;
    data::ui::ShellAnchor far;
    far.name = "s_select_35"; far.alt = data::ui::split_alt(far.name); far.rect = {1400,0,10,10};
    data::ui::ShellAnchors replaced;
    for (const auto& a : extra.anchors()) if (a.name != far.name) replaced.add(a);
    replaced.add(far);
    auto extended = ui::hud_view_model(replaced,catalog,0,masks);
    expect(extended.visible_extent && extended.visible_extent->right() == 1410,"bound component extends visible union");
    expect(extended.hit_test({1405,5}),"component outside faceplate receives input");
    data::ui::CommandBarCatalog catalog_gap;
    for (const auto& c : catalog.components()) if (c.name != "s_select_07") catalog_gap.add(c);
    expect(ui::hud_view_model(anchors.shell,catalog_gap,0,masks).family_size("s_select_") == 7,
        "family stops at first catalogue gap");
    data::ui::ShellAnchors gap;
    for (const auto& a : anchors.shell.anchors()) if (a.name != "s_select_12") gap.add(a);
    expect(ui::hud_view_model(gap,catalog,0,masks).family_size("s_select_") == 12,"family stops at first shell gap");
    auto missing_catalog = catalog;
    // Catalogue-only slot 36 must not count.
    data::ui::CommandBarComponent slot;
    slot.name = "s_select_36"; slot.alt = data::ui::split_alt(slot.name);
    missing_catalog.add(slot);
    expect(ui::hud_view_model(anchors.shell,missing_catalog,0,masks).family_size("s_select_") == 36,
        "catalogue-only slots do not count");
    auto missing = ui::hud_view_model(anchors.shell,catalog,0,{});
    expect(missing.faceplates.empty() && missing.diagnostics.size() == 1,"missing alpha is diagnosed once, no mesh-bound fallback");

    assets::Animation animation;
    animation.stored_frame_count = 2; animation.playable_frame_count = 1;
    animation.frames_per_second = 1; animation.duration_seconds = 1;
    assets::AnimationTrack track;
    track.bone_name = "hero"; track.bone_index = 1;
    track.samples = {{{100,50,0},{1,1,1},{0,0,0,1},true},
                     {{200,50,0},{1,1,1},{0,0,1,0},true}};
    animation.tracks.push_back(track);
    auto player = presentation::animation::Player::create(source,&animation);
    expect(player.has_value(),"synthetic ALA player binds");
    if (player) {
        const auto sampled = ui::animated_shell_anchors(source,player.value(),{0.5F,presentation::animation::PlaybackMode::clamp,0});
        expect(sampled.has_value(),"ALA midpoint samples");
        if (sampled) {
            const auto* anchor = sampled.value().shell.find("hero");
            expect(anchor && std::abs(anchor->rect.x-130) < 0.001F && std::abs(anchor->rect.y-50) < 0.001F &&
                std::abs(anchor->rect.width-20) < 0.001F && std::abs(anchor->rect.height-10) < 0.001F,
                "midpoint combines interpolated translation and 90-degree rotation");
            const auto animated = ui::hud_view_model(sampled.value().shell,catalog,0,masks);
            expect(animated.family_size("s_select_") == 36,"animation leaves family capacity stable");
        }
    }
}
void alpha_formats() {
    assets::Texture texture;
    texture.format = assets::PixelFormat::rgba8;
    texture.has_alpha = true;
    texture.source_origin = assets::ImageOrigin::bottom_left;
    texture.mips.push_back({1,2,4,{std::byte{0},std::byte{0},std::byte{0},std::byte{255},
                                  std::byte{0},std::byte{0},std::byte{0},std::byte{0}}});
    auto mask = ui::shell_alpha_mask(texture);
    expect(mask && !mask.value().opaque(0,0.1) && mask.value().opaque(0,0.9),"bottom-origin texture alpha is normalized");
    texture.format = assets::PixelFormat::bc1; texture.source_origin = assets::ImageOrigin::top_left;
    texture.mips = {{4,4,8,std::vector<std::byte>(8,std::byte{0xff})}};
    mask = ui::shell_alpha_mask(texture);
    expect(mask && !mask.value().opaque(0.1,0.1),"BC1 transparent selector decodes");
    texture.format = assets::PixelFormat::bc2;
    texture.mips = {{4,4,16,std::vector<std::byte>(16)}};
    texture.mips[0].bytes[0] = std::byte{15};
    mask = ui::shell_alpha_mask(texture);
    expect(mask && mask.value().opaque(0.1,0.1) && !mask.value().opaque(0.3,0.1),"BC2 alpha nibbles decode");
    texture.format = assets::PixelFormat::bc3;
    texture.mips[0].bytes[0] = std::byte{255};
    texture.mips[0].bytes[2] = std::byte{8};
    mask = ui::shell_alpha_mask(texture);
    expect(mask && mask.value().opaque(0.1,0.1) && !mask.value().opaque(0.3,0.1),"BC3 alpha selectors decode");
}
void corpus(const std::string& name, const vfs::Vfs& filesystem) {
    auto loaded = data::ui::load_command_bar(filesystem);
    expect(loaded.has_value(),"corpus command bar loads");
    if (!loaded) return;
    const auto& catalog = loaded.value().catalog;
    std::size_t new_diagnostics = 0;
    std::size_t animations_sampled = 0;
    std::size_t inherited_clip_mismatches = 0;
    auto clips = filesystem.enumerate("data/art/models",".ala");
    expect(clips.has_value(),"shell idle enumeration succeeds");
    for (const auto& d : loaded.value().diagnostics)
        if (d.code == data::ui::diagnostic_codes::component_invalid || d.code == data::ui::diagnostic_codes::field_unknown)
            ++new_diagnostics;
    std::map<std::string,std::shared_ptr<const ui::ShellAlphaMask>> cache;
    const ui::ShellMaskLookup lookup = [&](std::string_view key) -> std::shared_ptr<const ui::ShellAlphaMask> {
        auto [it, inserted] = cache.try_emplace(std::string(key));
        if (!inserted) return it->second;
        std::string texture_path = "data/art/textures/"+std::string(key);
        if (!filesystem.stat(texture_path)) texture_path.replace(texture_path.find_last_of('.'),std::string::npos,".dds");
        auto texture = assets::load_texture(filesystem,texture_path);
        if (!texture) { std::cerr << core::format_diagnostic(texture.error()) << '\n'; return {}; }
        auto mask = ui::shell_alpha_mask(texture.value());
        if (!mask) { std::cerr << core::format_diagnostic(mask.error()) << '\n'; return {}; }
        it->second = std::make_shared<ui::ShellAlphaMask>(std::move(mask.value()));
        return it->second;
    };
    for (const auto& component : catalog.components()) {
        if (component.type != data::ui::ComponentType::shell) continue;
        auto path = component.text(data::ui::Field::model_name);
        if (path.empty()) continue;
        auto source = assets::load_model(filesystem,"data/art/models/"+std::string(path));
        expect(source.has_value(),"corpus shell loads");
        if (!source) continue;
        auto anchors = data::ui::shell_anchors(source.value());
        new_diagnostics += anchors.diagnostics.size();
        std::string stem(path.substr(0,path.find_last_of('.')));
        std::transform(stem.begin(),stem.end(),stem.begin(),[](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c+('a'-'A')) : c;
        });

        if (clips) for (const auto& record : clips.value()) {
            if (!record.canonical_path.starts_with("data/art/models/"+stem+"_idle")) continue;
            auto clip = assets::load_animation(filesystem,record.canonical_path);
            expect(clip.has_value(),"corpus shell ALA loads");
            if (!clip) continue;
            auto player = presentation::animation::Player::create(source.value(),&clip.value());
            if (!player) {
                // FotR replaces the galactic skeleton but inherits this FoC clip.
                // This is the existing Player association diagnostic, not a new
                // shell-layout diagnostic; keep the mismatch explicit.
                const bool inherited_fotr = catalog.components().size() == 865U &&
                    record.canonical_path == "data/art/models/i_galactic_controls_idle_00.ala" &&
                    player.error().code == presentation::animation::diagnostic_codes::invalid_animation;
                expect(inherited_fotr,"only the known inherited FotR galactic clip may fail association");
                std::cout << "HUD corpus " << name << " existing clip association: "
                          << core::format_diagnostic(player.error()) << '\n';
                ++inherited_clip_mismatches;
                continue;
            }
            for (const float fraction : {0.0F,0.5F,1.0F}) {
                auto sampled = ui::animated_shell_anchors(source.value(),player.value(),
                    {clip.value().duration_seconds*fraction,presentation::animation::PlaybackMode::clamp,0});
                expect(sampled.has_value(),"corpus shell ALA samples endpoints and midpoint");
                if (sampled) new_diagnostics += sampled.value().diagnostics.size();
            }
            ++animations_sampled;
        }
        if (component.name != "i_main_skirmish" && !anchors.shell.find("b_create_00")) continue;
        auto hud = ui::hud_view_model(anchors.shell,catalog,0,lookup);
        new_diagnostics += hud.diagnostics.size();
        std::cout << "HUD corpus " << name << " " << component.name << ": select=" << hud.family_size("s_select_")
                  << " health=" << hud.family_size("s_health_") << " shield=" << hud.family_size("s_shield_")
                  << " special=" << hud.family_size("special_button_") << " border=" << hud.family_size("special_border_")
                  << " build=" << hud.family_size("b_create_");
        if (anchors.shell.find("b_create_00")) {
            const auto total = catalog.components().size();
            const std::size_t expected_build = total == 846U || total == 845U ? 26U : (total == 865U ? 34U : 40U);
            expect(hud.family_size("b_create_") == expected_build,"corpus build capacity counts shell/catalogue intersection");
        }
        if (hud.family_size("b_create_") == 40)
            std::cout << " (slot40 catalogue=" << (catalog.find("b_create_40") != nullptr)
                      << ",shell=" << (anchors.shell.find("b_create_40") != nullptr) << ")";
        if (hud.visible_extent) std::cout << " extent=" << hud.visible_extent->x << "," << hud.visible_extent->y
                                       << ".." << hud.visible_extent->right() << "," << hud.visible_extent->top();
        std::cout << '\n';
        if (name == "FoC" && component.name == "i_main_skirmish") {
            for (const auto viewport : {ui::Viewport{1024,768},ui::Viewport{1280,720},
                                       ui::Viewport{1920,1061},ui::Viewport{2560,1080}}) {
                for (const auto rules : {ui::LayoutRules::retail,ui::LayoutRules::aspect_correct}) {
                    const auto space = ui::reference_space(viewport,rules);
                    expect(hud.placement(space) == ui::place_shell(space,1077),"FoC-only layout placement unchanged");
                }
            }
        }
        if (component.name == "i_main_skirmish") {
            const auto expected = catalog.components().size() == 920U ? 36U : 24U;
            expect(hud.family_size("s_select_") == expected,"survey tactical selection capacity");
            expect(hud.family_size("special_border_") == (expected == 36U ? 18U : 12U),"survey border capacity");
        }
    }
    std::cout << "HUD corpus " << name << ": animations=" << animations_sampled << " inherited clip mismatches=" << inherited_clip_mismatches << " new diagnostics=" << new_diagnostics << '\n';
    expect(new_diagnostics == 0,"corpus has no new shell/schema/mask diagnostics");
}
} // namespace
void hud_contracts() {
    shell_widescreen_card();
    alpha_formats();
    if (auto filesystem = test::ui::foc_corpus("HUD")) corpus("FoC",*filesystem);
    for (const auto& mod : test::ui::mod_corpora("HUD")) corpus(mod.name,mod.filesystem);
}

