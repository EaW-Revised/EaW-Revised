// The table of docs/behaviour/asset-name-resolution.md (cases ANR-C1..C11) and the model rule ANR-02 against the shared
// resolvers over a synthetic set of existing files, plus the once-per-name miss report and the VFS wrappers.
#include "eawr/assets/asset_name_resolver.hpp"
#include "eawr/vfs/vfs.hpp"

#include "../ui/ui_test_support.hpp"

#include <array>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {
using eawr::test::ui::expect;
namespace assets = eawr::assets;

constexpr std::string_view texture_root = "data/art/textures/";
constexpr std::string_view model_root = "data/art/models/";

struct Files {
    std::set<std::string> present;
    std::vector<std::string> asked;
    Files(const std::string_view root, std::initializer_list<std::string_view> names) {
        for (const auto name : names) present.insert(std::string(root) + std::string(name));
    }
    [[nodiscard]] auto exists() {
        return [this](const std::string_view path) {
            asked.emplace_back(path);
            return present.contains(std::string(path));
        };
    }
};

std::string texture(const std::string_view name, std::initializer_list<std::string_view> files) {
    Files set(texture_root, files);
    return assets::resolve_texture_name(texture_root, name, set.exists());
}

std::string model(const std::string_view name, std::initializer_list<std::string_view> files) {
    Files set(model_root, files);
    return assets::resolve_model_name(model_root, name, set.exists());
}

std::string under(const std::string_view root, const std::string_view leaf) { return std::string(root) + std::string(leaf); }

void texture_cases() {
    expect(texture("sky.jpg", {"sky.dds"}) == under(texture_root, "sky.dds"), "ANR-C1: .jpg authored, .dds shipped");
    expect(texture("sky.tga", {"sky.dds"}) == under(texture_root, "sky.dds"), "ANR-C2: .tga authored, .dds shipped");
    expect(texture("sky.dds", {"sky.tga"}) == under(texture_root, "sky.tga"), "ANR-C3: .dds authored, .tga shipped (tolerance)");
    expect(texture("sky", {"sky.dds", "sky.tga"}) == under(texture_root, "sky.tga"),
           "ANR-C4: extensionless, both shipped, the retained order picks .tga");
    expect(texture("sky.jpg", {"sky.jpg", "sky.dds"}) == under(texture_root, "sky.jpg"), "ANR-C5: the authored name wins");
    expect(texture("Panel.Detail.Source", {"panel.dds", "panel.detail.dds"}) == under(texture_root, "panel.dds"),
           "ANR-C6: first-dot-token retry, not a last-extension replacement");
    expect(texture("panel.detail.tga", {"panel.detail.dds", "panel.dds"}) == under(texture_root, "panel.detail.dds"),
           "ANR-C11: dotted .tga keeps the full stem first");
    expect(texture("panel.detail.tga", {"panel.dds"}).empty(),
           "DEV-10: a dotted .tga never takes the first-token .dds retry (the original would find panel.dds)");
    expect(texture("W_Skydome_Sunset.jpg", {"w_skydome_sunset.dds"}) == under(texture_root, "w_skydome_sunset.dds"),
           "sky dome case folds the authored name");
    expect(texture("art\\Sky.JPG", {"art/sky.dds"}) == under(texture_root, "art/sky.dds"), "backslashes and case fold");
    expect(texture("  sky.jpg \t", {"sky.dds"}) == under(texture_root, "sky.dds"), "surrounding whitespace is dropped");
    expect(texture("sky.jpg", {}).empty() && texture("", {"sky.dds"}).empty() && texture("  ", {"sky.dds"}).empty(),
           "ANR-C10 (texture): nothing present, empty and blank names resolve to nothing");
    expect(texture("sky.bmp", {"sky.tga"}).empty(), "other source extensions do not invent a .tga retry");
    expect(texture("sky.png", {"sky.png"}) == under(texture_root, "sky.png"), "an existing literal other-extension file is used");
}

void model_cases() {
    expect(model("ship", {"ship.alo"}) == under(model_root, "ship.alo"), "ANR-C7: extensionless model gains .alo");
    expect(model("Ship.ALO", {"ship.alo"}) == under(model_root, "ship.alo"), "ANR-C8: mixed-case .alo");
    expect(model("ship.x", {"ship.alo"}) == under(model_root, "ship.alo"), "ANR-C9: other extension retries the stem");
    expect(model("ship.dds", {"ship.alo"}) == under(model_root, "ship.alo"), "a texture-like extension on a model retries the stem");
    expect(model("Dir\\Ship.Detail.Alo", {"dir/ship.alo"}) == under(model_root, "dir/ship.alo"),
           "ANR-02: the stem is the leaf cut at its first dot");
    expect(model("a.b/ship", {"a.b/ship.alo"}) == under(model_root, "a.b/ship.alo"), "ANR-02: a dot in a directory is not a leaf dot");
    expect(model("ship.alo", {"ship.alo", "ship.x.alo"}) == under(model_root, "ship.alo"), "ANR-02: the authored name first");
    expect(model("ship.x", {"ship.x", "ship.alo"}) == under(model_root, "ship.x"), "ANR-02: an existing literal file wins");
    expect(model(".alo", {".alo", "x.alo"}) == under(model_root, ".alo") && model(".x", {"x.alo"}).empty(),
           "ANR-02: a leaf that starts with a dot has no stem");
    expect(model("ship", {}).empty() && model(" ", {"ship.alo"}).empty(), "ANR-C10 (model): nothing present and blank resolve to nothing");
    expect(model("ship.alo", {"ship.dds"}).empty(), "ANR-02: no texture-like fallback for models");
    // A model name that already carries its own rooted path (galactic scene content) is not rooted twice.
    Files set("", {"data/art/models/ship.alo"});
    expect(assets::resolve_model_name(assets::effective_root(model_root, "Data\\Art\\Models\\Ship"), "Data\\Art\\Models\\Ship",
                                      set.exists()) == "data/art/models/ship.alo",
           "a data/-prefixed model name resolves without the root");
}

void candidate_order() {
    Files set(model_root, {});
    expect(assets::resolve_model_name(model_root, "ship.x", set.exists()).empty()
               && set.asked == std::vector<std::string>{"data/art/models/ship.x", "data/art/models/ship.alo"},
           "model candidate order is the authored name, then the stem with .alo");
    Files none(model_root, {});
    expect(assets::resolve_model_name(model_root, "ship.alo", none.exists()).empty()
               && none.asked == std::vector<std::string>{"data/art/models/ship.alo"},
           "an .alo name asks for its file once");
}

std::vector<std::string> emitted;
void capture(const std::string_view line) { emitted.emplace_back(line); }

void miss_reporting() {
    assets::reset_name_misses();
    emitted.clear();
    assets::set_name_miss_emitter(&capture);
    Files set(texture_root, {"sky.dds"});
    expect(assets::resolve_texture_reporting(texture_root, "sky.jpg", "test-site", set.exists()) == under(texture_root, "sky.dds")
               && emitted.empty(), "a hit reports nothing");
    expect(assets::resolve_texture_reporting(texture_root, "Gone.TGA", "site-a", set.exists()).empty()
               && emitted.size() == 1
               && emitted.front() == "EAWR-ASSET-NAME-0001 texture 'Gone.TGA' not found (site site-a)",
           "ANR-C10: a miss logs one clear line");
    expect(assets::resolve_texture_reporting(texture_root, "gone.tga", "site-b", set.exists()).empty() && emitted.size() == 1,
           "the same name, folded, is not reported twice even from another site");
    Files models(model_root, {});
    expect(assets::resolve_model_reporting(model_root, "Gone.TGA", "site-c", models.exists()).empty() && emitted.size() == 2
               && emitted.back().starts_with("EAWR-ASSET-NAME-0001 model 'Gone.TGA'"),
           "the same name as another kind is its own report");
    expect(assets::resolve_texture_reporting(texture_root, " ", "site-d", set.exists()).empty() && emitted.size() == 2,
           "a blank name is not a miss");
    expect(assets::resolve_texture_reporting(texture_root, "bad\nname\x01.tga", "line\nsite", set.exists()).empty()
               && emitted.size() == 3 && emitted.back().find('\n') == std::string::npos
               && emitted.back() == "EAWR-ASSET-NAME-0001 texture 'bad?name?.tga' not found (site line?site)",
           "authored text cannot break the one-line format");
    expect(assets::name_miss_lines() == emitted, "the recorded lines match what was emitted");

    // The cap: later names are counted, one notice is emitted.
    assets::reset_name_misses();
    emitted.clear();
    for (std::size_t i = 0; i < assets::name_miss_cap + 5; ++i)
        assets::report_name_miss(assets::NameKind::texture, "n" + std::to_string(i) + ".tga", "cap");
    expect(emitted.size() == assets::name_miss_cap + 1
               && emitted.back().find("further missing asset names are not reported") != std::string::npos,
           "after the cap one notice follows and later names stay quiet");

    assets::reset_name_misses();
    assets::set_name_miss_emitter(nullptr);
}

void vfs_wrappers() {
    eawr::test::ui::TempTree tree("name-resolution");
    eawr::test::ui::write_text(tree.root / "art/textures/w_skydome_sunset.dds", "x");
    eawr::test::ui::write_text(tree.root / "art/models/ship.alo", "x");
    const std::array mounts{eawr::vfs::MountSpec{"mod", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(bool(mounted), "wrapper fixture VFS mounts");
    if (!mounted) return;
    emitted.clear();
    assets::reset_name_misses();
    assets::set_name_miss_emitter(&capture);
    const auto& vfs = mounted.value();
    expect(assets::resolve_texture(vfs, "W_Skydome_Sunset.jpg", "vfs-test") == "data/art/textures/w_skydome_sunset.dds",
           "the VFS wrapper finds the .dds of a .jpg name");
    expect(assets::resolve_texture(vfs, "Data\\Art\\Textures\\W_Skydome_Sunset.JPG", "vfs-test")
               == "data/art/textures/w_skydome_sunset.dds", "a data/-prefixed texture name is not rooted twice");
    expect(assets::resolve_model(vfs, "Ship", "vfs-test") == "data/art/models/ship.alo", "the VFS wrapper adds .alo");
    expect(assets::resolve_model(vfs, "Ship.X", "vfs-test") == "data/art/models/ship.alo", "the VFS wrapper retries the model stem");
    expect(assets::resolve_model(vfs, "data/art/models/Ship", "vfs-test") == "data/art/models/ship.alo",
           "a data/-prefixed model name is not rooted twice");
    expect(emitted.empty(), "hits through the wrappers log nothing");
    expect(assets::resolve_texture(vfs, "missing.jpg", "vfs-test").empty() && assets::resolve_model(vfs, "missing", "vfs-test").empty()
               && emitted.size() == 2, "misses through the wrappers log once per kind and name");
    static_cast<void>(assets::resolve_texture(vfs, "missing.jpg", "vfs-test"));
    expect(emitted.size() == 2, "repeating a miss logs nothing more");
    assets::set_name_miss_emitter(nullptr);
    assets::reset_name_misses();
}
} // namespace

int main() {
    texture_cases();
    model_cases();
    candidate_order();
    miss_reporting();
    vfs_wrappers();
    return eawr::test::ui::failures() == 0 ? 0 : 1;
}
