#include "eawr/presentation/ui/project_hud.hpp"
#include "ui_test_support.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

void project_hud_contracts() {
    using namespace eawr;
    namespace ui = presentation::ui;
    const auto root = std::filesystem::temp_directory_path() /
        ("eawr-project-hud-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto base = root / "base", mod = root / "mod";
    std::filesystem::create_directories(base / "XML");
    std::filesystem::create_directories(mod / "XML");
    {
        const std::array empty_mounts{vfs::MountSpec{"empty",base,"data",{}}};
        auto empty = vfs::Vfs::mount(empty_mounts);
        test::ui::expect(empty && ui::load_project_hud(empty.value()).at("gift").stock_art,
            "missing project file retains built-in gift");
    }
    {
        std::ofstream file(base / "XML" / "EawrHud.xml");
        file << R"xml(<Eawr_Hud><Control Name="gift"><Enabled>true</Enabled><Size>40 20</Size><Recipient_Size>50 10</Recipient_Size><Normal_Texture>base.tga</Normal_Texture><Candidate><Anchor>attack</Anchor><Side>above</Side><Offset>0 -2</Offset></Candidate><Candidate><Anchor>viewport</Anchor><Side>inside_bottom_right</Side><Offset>-2 -2</Offset></Candidate></Control><Control Name="other"><Enabled>true</Enabled></Control></Eawr_Hud>)xml";
    }
    {
        std::ofstream file(mod / "XML" / "EawrHud.xml");
        file << R"xml(<Eawr_Hud><Control Name="Gift"><Enabled>true</Enabled><Size>30 10</Size><Recipient_Size>30 8</Recipient_Size><Normal_Texture>override.tga</Normal_Texture><Candidate><Anchor>viewport</Anchor><Side>inside_bottom_right</Side><Offset>-3 -4</Offset></Candidate></Control></Eawr_Hud>)xml";
    }
    const std::array mounts{vfs::MountSpec{"mod",mod,"data",{}},vfs::MountSpec{"revised",base,"data",{}}};
    auto filesystem = vfs::Vfs::mount(mounts);
    test::ui::expect(static_cast<bool>(filesystem),"project HUD layers mount");
    if (filesystem) {
        const auto defs = ui::load_project_hud(filesystem.value());
        test::ui::expect(defs.size() == 2 && defs.at("gift").normal == "override.tga" && defs.contains("other"),
            "mod replaces one named control while retaining other layer entries");
        const auto& gift = defs.at("gift");
        test::ui::expect(gift.row_gap == 0, "authored recipient rows default to no gap");
        const auto found = ui::dock_project_hud(gift,{0,0,200,100},1,{}, {},2);
        test::ui::expect(found && found->x == 167 && found->y == 86, "authored viewport offset positions gift");
        test::ui::expect(!ui::dock_project_hud(gift,{0,0,200,100},1,{},{{167,70,40,16}},2),
            "recipient list collision hides the candidate");
    }
    {
        std::ofstream file(mod / "XML" / "EawrHud.xml");
        file << R"xml(<Eawr_Hud><Control Name="gift"><Enabled>false</Enabled></Control></Eawr_Hud>)xml";
    }
    auto disabled = vfs::Vfs::mount(mounts);
    if (disabled) test::ui::expect(!ui::load_project_hud(disabled.value()).at("gift").enabled,"mod can explicitly disable gift");
    {
        std::ofstream file(mod / "XML" / "EawrHud.xml");
        file << "<Eawr_Hud><Control";
    }
    auto malformed = vfs::Vfs::mount(mounts);
    if (malformed) {
        bool invalid = false;
        const auto definitions = ui::load_project_hud(malformed.value(), &invalid);
        test::ui::expect(invalid && definitions.empty(), "malformed higher layer does not resurrect lower controls");
    }
    const std::array only_base{vfs::MountSpec{"revised",base,"data",{}}};
    auto original = vfs::Vfs::mount(only_base);
    if (original) {
        const auto gift = ui::load_project_hud(original.value()).at("gift");
        std::vector<ui::PixelRect> proxy_occupancy;
        ui::record_project_hud_occupancy(proxy_occupancy, {0,0,200,100}, true);
        ui::record_project_hud_occupancy(proxy_occupancy, {10,28,40,20}, false);
        const auto proxy_fallback = ui::dock_project_hud(gift,{0,0,200,100},1,
            {{"attack",{10,50,20,20}}},proxy_occupancy,0);
        test::ui::expect(proxy_fallback && proxy_fallback->x == 158 && proxy_fallback->y == 78,
            "full-viewport input wrapper does not block docking but its real controls do");
        const auto fallback = ui::dock_project_hud(gift,{0,0,200,100},1,{{"attack",{10,50,20,20}}},{{10,28,40,20}},0);
        test::ui::expect(fallback && fallback->x == 158 && fallback->y == 78,"occupied anchor selects next candidate");
        test::ui::expect(!ui::dock_project_hud(gift,{0,0,200,100},1,{},{{0,0,200,100}},1),"all occupied candidates hide control");
    }
    {
        // The stock Gift control with no project file: pins the pre-project geometry.
        const auto stock = ui::default_gift();
        for (const double scale : {1.0, 2.0}) {
            const ui::PixelRect viewport{0,0,1000 * scale,800 * scale};
            const auto placed = ui::dock_project_hud(stock,viewport,scale,{},{},0); // a free top band must not move the stock position
            test::ui::expect(placed && placed->x == 12 * scale && placed->y == viewport.height - 220 * scale,
                "stock fallback Gift sits 220 reference pixels above the bottom edge");
            test::ui::expect(placed && placed->width == 96 * scale && placed->height == 30 * scale,
                "stock Gift button keeps its 96x30 size");
            const auto list = ui::project_hud_list_rect(stock,*placed,scale,3);
            test::ui::expect(list.x == placed->x && list.width == 180 * scale && list.height == 3 * 32 * scale
                && list.y == placed->y - 3 * 32 * scale - 2 * scale,
                "stock recipient rows keep their 2 px pitch gap and 2 px gap above the button");
        }
        // The 2 px gap above the button belongs to the list: a control there rejects the candidate.
        const auto gap_blocked = ui::dock_project_hud(stock,{0,0,1000,800},1,{},{{12,578,10,2}},3);
        test::ui::expect(gap_blocked && gap_blocked->x == 804 && gap_blocked->y == 580,
            "stock list gap is part of the docking collision area");
        // Hide path: the button alone fits, the list above it does not, so no candidate is safe.
        const std::map<std::string,ui::PixelRect> attack{{"attack",{10,50,20,20}}};
        const auto button_only = ui::dock_project_hud(stock,{0,0,200,100},1,attack,{},0);
        test::ui::expect(button_only && button_only->x == 10 && button_only->y == 16,
            "stock Gift docks above attack when no list is shown");
        test::ui::expect(!ui::dock_project_hud(stock,{0,0,200,100},1,attack,{},3),
            "a recipient list that does not fit hides the control");
    }
    {
        // Re-docking watch: unchanged frames neither report a change nor grow the buffers.
        ui::ProjectHudLayoutWatch watch;
        const auto sample = [&](const double pane) {
            watch.begin(); watch.add(1280,720,0,0); watch.add(1.0); watch.add(10,20,30,40); watch.add(pane);
            return watch.commit();
        };
        test::ui::expect(sample(0) && watch.changes() == 1, "first sample is a change");
        for (int frame = 0; frame < 100; ++frame) if (sample(0)) test::ui::expect(false, "unchanged frame reported a change");
        test::ui::expect(watch.changes() == 1, "unchanged frames do not rebuild");
        test::ui::expect(sample(1) && watch.changes() == 2, "an opened pane is a change");
        test::ui::expect(!sample(1) && watch.changes() == 2, "the new layout is remembered");
    }
    std::filesystem::remove_all(root);
}
