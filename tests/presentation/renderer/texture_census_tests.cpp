#include "../../support/check.hpp"
#include "texture_census.hpp"

#include <iostream>
#include <sstream>

namespace {
using namespace eawr;
using namespace eawr::presentation::godot_backend::detail;

constexpr std::uint64_t mib = 1U << 20U;

TextureCensusRow model_row(std::string owner, std::string path, std::string layer, const std::uint64_t key,
                           const assets::PixelFormat format, const std::uint64_t bytes, const std::uint64_t shared_by = 1) {
    TextureCensusRow row;
    row.owner_kind = TextureOwnerKind::model;
    row.owner = std::move(owner);
    row.role = "base";
    row.path = std::move(path);
    row.layer_id = std::move(layer);
    row.key = key;
    row.check = key ^ 0x55U;
    row.format = format;
    row.width = row.height = 512;
    row.mips = 10;
    row.device_bytes = bytes;
    row.compressed = format == assets::PixelFormat::bc1 || format == assets::PixelFormat::bc7;
    row.shared_by = shared_by;
    return row;
}

std::size_t count_of(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) ++count;
    return count;
}

std::string render(const TextureCensus& census, const GpuCategoryTotals* ledger = nullptr) {
    std::ostringstream output;
    census.write(output, "first_frame", ledger);
    return output.str();
}
} // namespace

int main() {
    eawr::testing::Checker checks;
    const auto check = [&](const bool value, const char* message) { checks.check(value, message, std::cerr); };

    // The ledger item of a real texture carries the shape the census reports.
    assets::Texture texture;
    texture.format = assets::PixelFormat::bc1;
    texture.width = 8;
    texture.height = 4;
    texture.mips.push_back({8, 4, 16, std::vector<std::byte>(16)});
    texture.mips.push_back({4, 2, 8, std::vector<std::byte>(8)});
    const GpuItem item = texture_item(texture);
    check(item.format == assets::PixelFormat::bc1 && item.width == 8 && item.height == 4 && item.mips == 2
              && item.compressed,
        "a texture ledger item must carry its format, size and mip count");

    TextureCensus census;
    // A 4 MiB uncompressed texture shared by three models.
    census.add(model_row("data/art/models/a.alo", "data/art/textures/hull.tga", "base", 1, assets::PixelFormat::rgba8,
                         4 * mib, 3));
    // Exactly 1 MiB uncompressed is not "over 1 MiB"; 1 MiB + 1 is.
    census.add(model_row("data/art/models/b.alo", "data/art/textures/edge.tga", "base", 2, assets::PixelFormat::bgr8, mib));
    census.add(model_row("data/art/models/c.alo", "data/art/textures/edge2.tga", "base", 3, assets::PixelFormat::bgr8,
                         mib + 1));
    census.add(model_row("data/art/models/d.alo", "data/art/textures/small.dds", "base", 4, assets::PixelFormat::bc1, 64 * 1024));
    census.add(model_row("data/art/models/e.alo", "data/art/textures/big.dds", "base", 5, assets::PixelFormat::bc7, 2 * mib));
    // The same path from another layer with different pixels: a path variant.
    census.add(model_row("data/art/models/f.alo", "data/art/textures/skin.dds", "mod_layer", 6, assets::PixelFormat::bc1, 2 * mib));
    census.add(model_row("data/art/models/g.alo", "data/art/textures/skin.dds", "base", 7, assets::PixelFormat::bc1, mib));
    // The same pixels also held by a particle backend: duplicate content across owner classes.
    TextureCensusRow particle = model_row("fx_smoke", "data/art/textures/hull.tga", "base", 1, assets::PixelFormat::rgba8, 4 * mib);
    particle.owner_kind = TextureOwnerKind::particle;
    particle.check = 1U ^ 0x55U;
    census.add(particle);
    // A demand-only resource: a CPU snapshot but no GPU copy.
    TextureCensusRow dormant = model_row("data/art/models/h.alo", "data/art/textures/dormant.tga", "base", 8,
                                         assets::PixelFormat::rgba8, 0);
    dormant.resident = false;
    dormant.cpu_snapshot_held = true;
    dormant.cpu_bytes = 3 * mib;
    census.add(dormant);
    TextureCensusRow orphan = model_row("no model resource", "", "", 9, assets::PixelFormat::l8, 4096);
    orphan.owner_kind = TextureOwnerKind::unowned;
    census.add(orphan);

    const auto summary = census.summarize();
    const auto model = static_cast<std::size_t>(TextureOwnerKind::model);
    const auto particle_kind = static_cast<std::size_t>(TextureOwnerKind::particle);
    check(summary.textures == 10, "every row must be counted");
    check(summary.kind_count[model] == 8 && summary.kind_count[particle_kind] == 1, "rows must total by owner class");
    check(summary.device_bytes == 4 * mib + mib + (mib + 1) + 64 * 1024 + 2 * mib + 2 * mib + mib + 4 * mib + 4096,
        "device bytes must sum the rows");
    check(summary.kind_bytes[particle_kind] == 4 * mib, "owner class bytes must sum the class rows");
    check(summary.kind_compressed[model] == 64 * 1024 + 2 * mib + 2 * mib + mib, "compressed bytes must total by class");
    check(summary.by_format.at("rgba8")[0] == 3 && summary.by_format.at("rgba8")[1] == 8 * mib
              && summary.by_format.at("rgba8")[2] == 3 * mib,
        "format rows must hold count, device bytes and CPU bytes");
    check(summary.by_format.at("bc1")[0] == 3, "bc1 rows must be counted");
    check(summary.cpu_held == 1 && summary.cpu_bytes == 3 * mib, "held CPU snapshots must be counted");
    // rgba8 4 MiB twice (model + particle), bgr8 1 MiB + 1; the exactly-1-MiB row and the l8 row do not count.
    check(summary.uncompressed_large == 3 && summary.uncompressed_large_bytes == 4 * mib + mib + 1 + 4 * mib,
        "uncompressed textures strictly over 1 MiB must be counted");
    check(summary.content_groups.size() == 1 && summary.content_groups[0].copies == 2
              && summary.content_groups[0].wasted == 4 * mib && summary.content_wasted == 4 * mib,
        "the same content held by two owners must waste one copy");
    check(summary.content_groups[0].label == "data/art/textures/hull.tga", "a duplicate group must be labelled by its path");
    // hull.tga is the same pixels twice: a content duplicate only, never a path variant.
    check(summary.path_groups.size() == 1 && summary.path_groups[0].label == "data/art/textures/skin.dds",
        "only a path with several distinct contents is a path variant");
    // skin.dds: 2 MiB + 1 MiB, the largest is kept, 1 MiB wasted.
    check(summary.path_wasted == mib && summary.path_groups[0].wasted == mib && summary.path_groups[0].copies == 2,
        "a path variant must waste all but its largest copy");
    // A non-resident row is not a GPU copy: it never joins a content group.
    check(summary.content_groups.size() == 1, "a dormant row must not count as a GPU duplicate");

    const std::string text = render(census);
    check(text.find("\"phase\":\"first_frame\"") != std::string::npos, "the report must name its phase");
    check(text.find("\"uncompressed_over_1mib\":{\"count\":3,") != std::string::npos, "the report must carry the large uncompressed count");
    check(text.find("\"wasted_bytes\":4194304") != std::string::npos, "the report must carry the wasted bytes");
    check(text.find("\"ledger\"") == std::string::npos, "no ledger object without ledger totals");
    // Equal weight: the owner name breaks the tie, so the model row precedes the particle row.
    check(text.find("\"top\":[{\"owner_kind\":\"model\",\"owner\":\"data/art/models/a.alo\"") != std::string::npos,
        "the largest textures must lead the top list in a total order");
    check(text.find("\"same_path_distinct_content\":{\"groups\":1,\"wasted_bytes\":1048576") != std::string::npos,
        "the report must not count identical content as a path variant");
    GpuCategoryTotals ledger;
    ledger.live = 7;
    ledger.live_bytes = 99;
    check(render(census, &ledger).find("\"ledger\":{\"live\":7,\"live_bytes\":99,") != std::string::npos,
        "the report must cross-check against the ledger totals");

    // Output is independent of the order the rows were added in.
    TextureCensus reversed;
    for (auto at = census.rows().rbegin(); at != census.rows().rend(); ++at) reversed.add(*at);
    check(render(reversed) == text, "the report must not depend on row insertion order");

    // A dormant row shares a path with a resident row of other pixels but is no GPU copy: no path variant.
    TextureCensus dormant_path;
    dormant_path.add(model_row("data/art/models/r.alo", "data/art/textures/shared.dds", "base", 200,
                               assets::PixelFormat::bc1, mib));
    TextureCensusRow parked = model_row("data/art/models/s.alo", "data/art/textures/shared.dds", "mod_layer", 201,
                                        assets::PixelFormat::bc1, 0);
    parked.resident = false;
    parked.cpu_snapshot_held = true;
    parked.cpu_bytes = mib;
    dormant_path.add(parked);
    const auto parked_summary = dormant_path.summarize();
    check(parked_summary.path_groups.empty() && parked_summary.path_wasted == 0,
        "a dormant row must not form a path variant with a resident row");

    // The top list is bounded; the aggregates are not.
    TextureCensus many;
    for (std::uint64_t index = 0; index < 120; ++index)
        many.add(model_row("m" + std::to_string(index), "p" + std::to_string(index), "base", 100 + index,
                           assets::PixelFormat::bc1, 1000 + index));
    const std::string bounded = render(many);
    check(count_of(bounded, "\"resident\":") == texture_census_top, "the top list must hold at most 50 rows");
    check(bounded.find("\"textures\":120,") != std::string::npos, "totals must still cover every row");
    bool exact = true;
    for (std::uint64_t index = 0; index < 120; ++index) {
        const bool listed = bounded.find("\"owner\":\"m" + std::to_string(index) + "\"") != std::string::npos;
        exact = exact && listed == (index >= 70);
    }
    check(exact, "the top list must hold exactly the 50 largest rows");

    // Empty input is a valid report.
    const TextureCensus none;
    const auto nothing = none.summarize();
    check(nothing.textures == 0 && nothing.device_bytes == 0 && nothing.content_groups.empty()
              && nothing.path_groups.empty(), "an empty census must summarise to zero");
    const std::string blank = render(none);
    check(blank.find("\"textures\":0,") != std::string::npos && blank.find("\"top\":[]") != std::string::npos
              && blank.find("\"by_format\":[]") != std::string::npos, "an empty census must write empty lists");

    // Names are escaped.
    TextureCensus quoted;
    quoted.add(model_row("a\"b", "c\\d\n", "base", 1, assets::PixelFormat::bc1, 10));
    const std::string escaped = render(quoted);
    check(escaped.find("a\\\"b") != std::string::npos && escaped.find("c\\\\d\\n") != std::string::npos,
        "quotes, backslashes and newlines must be escaped");

    // Registry: nothing is kept while the switch is off; on, each reference is its own GPU copy.
    auto& registry = TextureCensusRegistry::instance();
    GpuItem registered;
    registered.key = 77;
    registered.check = 78;
    registered.bytes = 2 * mib;
    registered.format = assets::PixelFormat::rgba8;
    registered.width = registered.height = 512;
    assets::Source source;
    source.logical_path = "data/art/textures/spark.tga";
    source.layer_id = "base";
    TextureCensusRegistry::enabled().store(false);
    int renderer_a = 0, renderer_b = 0;
    registry.add(&renderer_a, TextureOwnerKind::shared_named, "bloom", registered);
    TextureCensus off;
    registry.append(off, &renderer_a);
    check(off.rows().empty(), "a registry with the switch off must record nothing");
    TextureCensusRegistry::enabled().store(true);
    registry.add(nullptr, TextureOwnerKind::particle, "spark", registered, &source);
    registry.add(nullptr, TextureOwnerKind::particle, "spark", registered, &source);
    registry.add(&renderer_a, TextureOwnerKind::shared_named, "bloom", registered);
    registry.add(&renderer_b, TextureOwnerKind::shared_named, "bloom", registered);
    TextureCensus on;
    registry.append(on, &renderer_a);
    check(on.rows().size() == 3, "two particle references and one shared texture of the probing renderer must list three copies");
    check(on.rows()[0].path == "data/art/textures/spark.tga" && on.rows()[0].layer_id == "base"
              && on.rows()[0].owner_kind == TextureOwnerKind::particle, "registered sources must carry their path and layer");
    TextureCensus other;
    registry.append(other, &renderer_b);
    check(other.rows().size() == 3, "each renderer reports its own shared textures plus the process-wide particles");
    registry.remove(&renderer_b, TextureOwnerKind::shared_named, "bloom", registered);
    registry.remove(nullptr, TextureOwnerKind::particle, "spark", registered);
    TextureCensus after_one;
    registry.append(after_one, &renderer_a);
    check(after_one.rows().size() == 2, "releasing one reference must drop one copy");
    registry.remove(nullptr, TextureOwnerKind::particle, "spark", registered);
    registry.remove(&renderer_a, TextureOwnerKind::shared_named, "bloom", registered);
    TextureCensus empty;
    registry.append(empty, &renderer_a);
    check(empty.rows().empty(), "releasing every reference must empty the registry");
    TextureCensusRegistry::enabled().store(false);

    // Live renderers are listed in creation order and leave when released.
    auto& renderers = TextureCensusRenderers::instance();
    check(renderers.live().empty(), "no renderer is registered before any is created");
    renderers.add(&renderer_a);
    renderers.add(&renderer_b);
    check(renderers.live().size() == 2 && renderers.live()[0] == &renderer_a && renderers.live()[1] == &renderer_b,
        "live renderers must list in creation order");
    renderers.remove(&renderer_a);
    check(renderers.live().size() == 1 && renderers.live()[0] == &renderer_b, "a released renderer must leave the list");
    renderers.remove(&renderer_b);
    renderers.remove(&renderer_b);
    check(renderers.live().empty(), "removing an absent renderer must be harmless");
    return checks.finish();
}
