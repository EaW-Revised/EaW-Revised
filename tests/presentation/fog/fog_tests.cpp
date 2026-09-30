#include "eawr/presentation/fog/fog.hpp"
#include "eawr/sim/snapshot.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/visibility.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Engine-free contracts for the fog-stub-v1 texture cache. Every grid is
// invented here; the recording backend counts real calls and bytes so the
// cache's own counters are cross-checked against what was actually requested.
namespace {

namespace fog = eawr::presentation::fog;
namespace sim_fog = eawr::sim::fog;

int failures{};

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::int64_t one = std::int64_t{1} << 24;

// Asymmetric 3x2 grid: negative X origin, rectangular 2.0 x 0.75 cells.
sim_fog::FogGridDesc base_desc(const std::uint64_t revision = 1) {
    return sim_fog::FogGridDesc{
        .team_id = 0,
        .width = 3,
        .height = 2,
        .origin_x_raw = -5 * one / 2,
        .origin_y_raw = one / 4,
        .cell_x_raw = 2 * one,
        .cell_y_raw = 3 * one / 4,
        .encoding = sim_fog::encoding_linear_u8_attenuation,
        .revision = revision,
    };
}

// Row-major y*width+x; every cell distinct so orientation errors are visible.
const std::vector<std::uint8_t> base_cells{10, 60, 110, 160, 210, 255};

sim_fog::FogGrid grid(const sim_fog::FogGridDesc& desc, const std::vector<std::uint8_t>& cells) {
    auto created = sim_fog::FogGrid::create(desc, cells);
    if (!created) {
        std::cerr << "grid fixture rejected: " << created.error().message << '\n';
        std::exit(2);
    }
    return std::move(created).value();
}

sim_fog::FogGridSet set_of(std::vector<sim_fog::FogGrid> grids) {
    auto created = sim_fog::FogGridSet::create(std::move(grids));
    if (!created) {
        std::cerr << "set fixture rejected: " << created.error().message << '\n';
        std::exit(2);
    }
    return std::move(created).value();
}

sim_fog::FogGridSet single(const sim_fog::FogGridDesc& desc, const std::vector<std::uint8_t>& cells) {
    return set_of({grid(desc, cells)});
}

struct Texture {
    fog::TextureSpec spec;
    std::vector<std::uint8_t> pixels;
};

class RecordingBackend final : public fog::TextureBackend {
public:
    fog::TextureHandle create_texture(
        const fog::TextureSpec& spec, const std::span<const std::uint8_t> pixels) override {
        if (fail_create) {
            log.push_back("create-failed");
            return {};
        }
        const fog::TextureHandle handle = next_handle++;
        textures[handle] = Texture{spec, {pixels.begin(), pixels.end()}};
        bytes += pixels.size();
        log.push_back("create " + std::to_string(handle));
        return handle;
    }

    bool update_texture(const fog::TextureHandle texture, const std::span<const std::uint8_t> pixels) override {
        const auto found = textures.find(texture);
        if (fail_update || found == textures.end()
            || pixels.size() != std::size_t{found->second.spec.width} * found->second.spec.height) {
            log.push_back("update-failed");
            return false;
        }
        found->second.pixels.assign(pixels.begin(), pixels.end());
        bytes += pixels.size();
        log.push_back("update " + std::to_string(texture));
        return true;
    }

    void destroy_texture(const fog::TextureHandle texture) override {
        if (textures.erase(texture) != 1) log.push_back("destroy-unknown");
        log.push_back("destroy " + std::to_string(texture));
    }

    void bind(const fog::ConsumerId consumer, const fog::TextureHandle texture,
              const fog::FogMapping& mapping) override {
        if (!textures.contains(texture)) log.push_back("bind-dead-texture");
        bound[consumer] = {texture, mapping};
        log.push_back("bind " + std::to_string(consumer) + "->" + std::to_string(texture));
    }

    void unbind(const fog::ConsumerId consumer) override {
        bound.erase(consumer);
        log.push_back("unbind " + std::to_string(consumer));
    }

    std::string failure_cause() const override { return "injected synthetic failure"; }

    std::map<fog::TextureHandle, Texture> textures;
    std::map<fog::ConsumerId, std::pair<fog::TextureHandle, fog::FogMapping>> bound;
    std::vector<std::string> log;
    std::uint64_t bytes{};
    fog::TextureHandle next_handle{1};
    bool fail_create{};
    bool fail_update{};
};

void test_mapping_and_half_open_bounds() {
    const auto g = grid(base_desc(), base_cells);
    const auto mapping = fog::mapping_for(g.desc());
    expect(mapping.origin_x == -2.5 && mapping.origin_y == 0.25, "mapping origin is the Q24 lower corner");
    expect(mapping.extent_x == 6.0 && mapping.extent_y == 1.5, "mapping extent is width*cell per axis");
    expect(mapping.width == 3 && mapping.height == 2, "mapping carries grid dimensions");

    const auto& d = g.desc();
    const auto at = [&](const std::int64_t x, const std::int64_t y) { return fog::cell_at_raw(d, x, y); };
    expect(at(d.origin_x_raw, d.origin_y_raw) == fog::CellIndex{0, 0}, "lower corner is inside cell (0,0)");
    expect(!at(d.origin_x_raw - 1, d.origin_y_raw), "one raw unit left of origin is outside");
    expect(!at(d.origin_x_raw, d.origin_y_raw - 1), "one raw unit below origin is outside");
    expect(at(d.origin_x_raw + d.cell_x_raw - 1, d.origin_y_raw) == fog::CellIndex{0, 0}, "cell x end is exclusive");
    expect(at(d.origin_x_raw + d.cell_x_raw, d.origin_y_raw) == fog::CellIndex{1, 0}, "next cell starts at the boundary");
    expect(at(d.origin_x_raw, d.origin_y_raw + d.cell_y_raw) == fog::CellIndex{0, 1}, "row 1 starts one cell up in +Y");
    const std::int64_t end_x = d.origin_x_raw + 3 * d.cell_x_raw;
    const std::int64_t end_y = d.origin_y_raw + 2 * d.cell_y_raw;
    expect(at(end_x - 1, end_y - 1) == fog::CellIndex{2, 1}, "last raw unit is inside cell (2,1)");
    expect(!at(end_x, end_y - 1) && !at(end_x - 1, end_y), "upper extent is exclusive on both axes");
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    expect(!at(low, low) && !at(high, high), "extreme raw points are outside without overflow");

    // Every cell centre maps to its own byte; outside is dark.
    for (std::uint32_t y = 0; y < 2; ++y) {
        for (std::uint32_t x = 0; x < 3; ++x) {
            const std::int64_t cx = d.origin_x_raw + static_cast<std::int64_t>(x) * d.cell_x_raw + d.cell_x_raw / 2;
            const std::int64_t cy = d.origin_y_raw + static_cast<std::int64_t>(y) * d.cell_y_raw + d.cell_y_raw / 2;
            const std::uint8_t expected = base_cells[y * 3 + x];
            expect(fog::attenuation_at_raw(g, cx, cy) == expected, "raw centre of cell " + std::to_string(x) + "," + std::to_string(y));
            const fog::SourcePoint source{fog::q24_to_double(cx), fog::q24_to_double(cy)};
            expect(fog::attenuation_at(g, source) == expected, "float centre of cell " + std::to_string(x) + "," + std::to_string(y));
            // Render position (X, h, Z) with Z = -source Y recovers the same cell.
            const auto recovered = fog::source_from_render(source.x, 42.0, -source.y);
            expect(fog::attenuation_at(g, recovered) == expected, "render (X,-Z) mapping for cell " + std::to_string(x) + "," + std::to_string(y));
        }
    }
    expect(fog::attenuation_at_raw(g, d.origin_x_raw - 1, d.origin_y_raw) == 0, "raw outside samples dark");
    expect(fog::attenuation_at(g, {-2.6, 1.0}) == 0 && fog::attenuation_at(g, {3.5, 1.0}) == 0, "float x outside is dark");
    expect(fog::attenuation_at(g, {0.0, 0.2}) == 0 && fog::attenuation_at(g, {0.0, 1.75}) == 0, "float y outside is dark");
    expect(fog::attenuation_at(g, {-2.5, 0.25}) == 10, "float lower corner is inside");
    // Negative control: using +Z instead of -Z lands outside or on another row.
    const auto wrong = fog::SourcePoint{-1.5, -0.625};
    expect(fog::attenuation_at(g, fog::source_from_render(-1.5, 0.0, -0.625)) == 10
               && fog::attenuation_at(g, wrong) == 0,
           "the sign of Z matters (orientation negative control)");
}

struct Harness {
    RecordingBackend backend;
    std::optional<fog::TextureCache> cache;
    Harness() { cache.emplace(backend); }
};

void test_upload_lifecycle() {
    Harness h;
    auto& cache = *h.cache;
    auto& backend = h.backend;
    const fog::StreamTeam key{1, 0};
    expect(static_cast<bool>(cache.add_consumer(11)), "current consumer registers before any grid");
    expect(backend.log.empty(), "registering without a binding does no GPU work");

    // First selection: one create of exactly width*height bytes.
    auto result = cache.submit(single(base_desc(1), base_cells), key);
    expect(result && result.value() == fog::SubmitAction::created, "first submit creates");
    expect(cache.stats().creates == 1 && cache.stats().uploads == 1 && cache.stats().upload_bytes == 6, "one create of 6 bytes");
    expect(backend.bytes == 6 && backend.textures.size() == 1, "backend received exactly 6 bytes in one texture");
    const fog::TextureHandle first = cache.texture(key);
    expect(backend.textures[first].pixels == base_cells, "texture pixels are the row-major cells");
    expect(backend.textures[first].spec == fog::TextureSpec{3, 2}, "texture is 3x2");
    expect(backend.bound[11].first == first && cache.stats().binds == 1, "current consumer bound once");

    // Identical resubmit from a separately created grid: zero work.
    const auto log_size = backend.log.size();
    const auto before = cache.stats();
    result = cache.submit(single(base_desc(1), base_cells), key);
    expect(result && result.value() == fog::SubmitAction::unchanged, "identical grid is unchanged");
    expect(cache.stats() == before && backend.log.size() == log_size, "identical grid does no backend call");

    // Revision-only bump: identity only, no upload, no rebind (mapping unchanged).
    result = cache.submit(single(base_desc(2), base_cells), key);
    expect(result && result.value() == fog::SubmitAction::metadata_only, "revision-only bump is metadata only");
    expect(cache.stats().uploads == 1 && backend.bytes == 6 && backend.log.size() == log_size, "revision-only bump uploads nothing");
    expect(cache.accepted(key)->revision() == 2, "accepted revision advances");

    // Origin change: uniforms rebind, still no upload.
    auto moved = base_desc(3);
    moved.origin_x_raw += one;
    result = cache.submit(single(moved, base_cells), key);
    expect(result && result.value() == fog::SubmitAction::metadata_only, "origin change is metadata only");
    expect(cache.stats().uploads == 1 && backend.bytes == 6, "origin change uploads nothing");
    expect(cache.stats().binds == 2 && backend.bound[11].second.origin_x == -1.5, "origin change rebinds the new mapping once");

    // One cell change: exactly one same-size update.
    auto changed = base_cells;
    changed[4] = 0;
    moved.revision = 4;
    result = cache.submit(single(moved, changed), key);
    expect(result && result.value() == fog::SubmitAction::updated, "cell change updates");
    expect(cache.stats().updates == 1 && cache.stats().uploads == 2 && cache.stats().upload_bytes == 12, "one update of 6 bytes");
    expect(backend.textures[first].pixels == changed && cache.texture(key) == first, "same texture now holds changed cells");
    expect(cache.stats().binds == 2, "same texture and mapping need no rebind");

    // Dimension change: create replacement, rebind, then destroy the old one.
    auto wider = moved;
    wider.revision = 5;
    wider.width = 4;
    std::vector<std::uint8_t> wide_cells{1, 2, 3, 4, 5, 6, 7, 8};
    const auto mark = backend.log.size();
    result = cache.submit(single(wider, wide_cells), key);
    expect(result && result.value() == fog::SubmitAction::recreated, "dimension change recreates");
    const fog::TextureHandle second = cache.texture(key);
    expect(second != first && backend.textures.size() == 1 && !backend.textures.contains(first), "old texture released");
    expect(backend.textures[second].spec == fog::TextureSpec{4, 2} && backend.textures[second].pixels == wide_cells, "replacement holds 4x2 cells");
    expect(backend.log.size() == mark + 3 && backend.log[mark] == "create 2" && backend.log[mark + 1] == "bind 11->2"
               && backend.log[mark + 2] == "destroy 1",
           "replacement is created and bound before the old texture is destroyed");
    expect(cache.stats().recreates == 1 && cache.stats().destroys == 1 && cache.stats().upload_bytes == 20, "recreate counts 8 bytes");
    expect(cache.live_textures() == 1, "one live texture");
}

void test_fail_closed() {
    Harness h;
    auto& cache = *h.cache;
    auto& backend = h.backend;
    const fog::StreamTeam key{1, 0};
    (void)cache.add_consumer(11);
    (void)cache.submit(single(base_desc(3), base_cells), key);
    const fog::TextureHandle texture = cache.texture(key);
    const auto log_size = backend.log.size();

    // Rollback: fails, replaces nothing, keeps the same-selection binding.
    auto result = cache.submit(single(base_desc(2), base_cells), key);
    expect(!result && result.error().code == fog::diagnostic_codes::revision_rollback, "rollback fails closed");
    expect(backend.log.size() == log_size && cache.accepted(key)->revision() == 3, "rollback touches nothing");
    expect(cache.active() == key && backend.bound[11].first == texture, "rollback keeps the accepted binding");

    // Equal revision with divergent cells or metadata.
    auto divergent = base_cells;
    divergent[0] = 11;
    result = cache.submit(single(base_desc(3), divergent), key);
    expect(!result && result.error().code == fog::diagnostic_codes::revision_conflict, "equal revision divergent cells fails");
    auto shifted = base_desc(3);
    shifted.cell_y_raw += 1;
    result = cache.submit(single(shifted, base_cells), key);
    expect(!result && result.error().code == fog::diagnostic_codes::revision_conflict, "equal revision divergent metadata fails");
    expect(backend.log.size() == log_size && backend.textures[texture].pixels == base_cells, "conflicts upload nothing");
    expect(cache.stats().rejected == 3, "three rejections counted");

    // Missing team: unbinds so no other team's texture is presented.
    auto other = base_desc(1);
    other.team_id = 7;
    result = cache.submit(single(other, base_cells), key);
    expect(!result && result.error().code == fog::diagnostic_codes::missing_team, "missing team fails");
    expect(!cache.active() && !backend.bound.contains(11) && cache.stats().unbinds == 1, "missing team unbinds consumers");
    expect(cache.live_textures() == 1 && backend.textures.contains(texture), "missing team keeps cached texture");
    result = cache.submit(set_of({}), key);
    expect(!result && result.error().code == fog::diagnostic_codes::missing_team, "empty set is a missing team");

    // Reselecting the identical accepted grid rebinds without an upload.
    const auto uploads = cache.stats().uploads;
    result = cache.submit(single(base_desc(3), base_cells), key);
    expect(result && result.value() == fog::SubmitAction::reselected, "identical grid reselects");
    expect(cache.stats().uploads == uploads && backend.bound[11].first == texture, "reselect rebinds cached texture");

    // Backend update failure: atomic, binding kept, retry succeeds once.
    backend.fail_update = true;
    auto changed = base_cells;
    changed[2] = 0;
    result = cache.submit(single(base_desc(4), changed), key);
    expect(!result && result.error().code == fog::diagnostic_codes::backend_failure, "update failure reported");
    expect(result.has_value() || result.error().message.find("injected") != std::string::npos, "failure cause propagated");
    expect(backend.textures[texture].pixels == base_cells && cache.accepted(key)->revision() == 3, "update failure replaces nothing");
    expect(cache.active() == key && backend.bound[11].first == texture, "update failure keeps binding");
    backend.fail_update = false;
    result = cache.submit(single(base_desc(4), changed), key);
    expect(result && result.value() == fog::SubmitAction::updated && cache.stats().updates == 1, "retry updates once");

    // Backend create failure during recreation: old texture retained.
    backend.fail_create = true;
    auto taller = base_desc(5);
    taller.height = 3;
    const std::vector<std::uint8_t> tall(9, 99);
    result = cache.submit(single(taller, tall), key);
    expect(!result && result.error().code == fog::diagnostic_codes::backend_failure, "recreate failure reported");
    expect(cache.texture(key) == texture && backend.textures.size() == 1 && backend.textures[texture].pixels == changed,
           "recreate failure keeps the old texture and pixels");
    expect(cache.accepted(key)->revision() == 4 && cache.stats().destroys == 0, "recreate failure destroys nothing");

    // Create failure for a new selection: that selection is never shown.
    auto seven = base_desc(1);
    seven.team_id = 7;
    result = cache.submit(set_of({grid(base_desc(4), changed), grid(seven, base_cells)}), {1, 7});
    expect(!result && result.error().code == fog::diagnostic_codes::backend_failure, "new-selection create failure reported");
    expect(!cache.active() && !backend.bound.contains(11), "failed new selection unbinds the previous team");
    expect(cache.live_textures() == 1 && !cache.accepted({1, 7}), "failed create caches nothing");
    backend.fail_create = false;
}

void test_consumers_teams_streams_and_release() {
    Harness h;
    auto& backend = h.backend;
    auto& cache = *h.cache;
    auto seven = base_desc(1);
    seven.team_id = 7;
    seven.width = 2;
    seven.height = 3;
    const std::vector<std::uint8_t> seven_cells{1, 2, 3, 4, 5, 6};
    const auto both = set_of({grid(base_desc(1), base_cells), grid(seven, seven_cells)});

    auto snapshot = std::make_shared<const eawr::sim::RenderSnapshot>(
        std::uint64_t{9}, std::vector<eawr::sim::RenderInstance>{}, both);

    expect(static_cast<bool>(cache.submit(snapshot->fog_grids(), {1, 0})), "team 0 selected");
    // Late consumer is bound immediately to the current texture and mapping.
    expect(static_cast<bool>(cache.add_consumer(22)), "late consumer registers");
    expect(backend.bound[22].first == cache.texture({1, 0}) && backend.bound[22].second == fog::mapping_for(base_desc(1)),
           "late consumer gets the current binding");
    expect(!cache.add_consumer(22) && !cache.add_consumer(0), "duplicate and zero consumers are rejected");
    expect(!cache.remove_consumer(99), "unknown consumer removal is rejected");

    // Team switch creates team 7 once, and switching back reuses team 0.
    auto result = cache.submit(snapshot->fog_grids(), {1, 7});
    expect(result && result.value() == fog::SubmitAction::created && backend.bound[22].first == cache.texture({1, 7}), "team 7 created and bound");
    const auto uploads = cache.stats().uploads;
    result = cache.submit(snapshot->fog_grids(), {1, 0});
    expect(result && result.value() == fog::SubmitAction::reselected && cache.stats().uploads == uploads, "team switch reuses cache");
    expect(cache.stats().reselects == 1 && backend.bound[22].first == cache.texture({1, 0}), "reselect binds team 0 again");

    // Streams are separate cache domains; revision 1 is valid on stream 2.
    result = cache.submit(snapshot->fog_grids(), {2, 0});
    expect(result && result.value() == fog::SubmitAction::created && cache.live_textures() == 3, "stream 2 has its own texture");

    // Seek reset of stream 2 releases only its textures.
    cache.reset_stream(2);
    expect(cache.live_textures() == 2 && !cache.active() && !backend.bound.contains(22), "active stream reset unbinds");
    cache.reset_stream(1);
    expect(cache.live_textures() == 0 && backend.textures.empty(), "stream 1 reset releases both teams");
    // After a seek reset the stream may restart at revision 1.
    auto higher = base_desc(9);
    expect(static_cast<bool>(cache.submit(single(higher, base_cells), {1, 0})), "stream restarts after reset");
    cache.reset_stream(1);
    expect(static_cast<bool>(cache.submit(snapshot->fog_grids(), {1, 0})), "revision 1 accepted after reset");

    // Removing a bound consumer unbinds it.
    expect(static_cast<bool>(cache.remove_consumer(22)) && !backend.bound.contains(22), "remove unbinds");
    expect(static_cast<bool>(cache.add_consumer(22)), "consumer re-added");

    // Full reset and teardown release every texture; snapshots stay valid.
    cache.reset();
    expect(cache.live_textures() == 0 && backend.textures.empty() && backend.bound.empty(), "reset releases everything");
    expect(cache.consumers() == 1, "reset keeps consumer registrations");
    expect(static_cast<bool>(cache.submit(snapshot->fog_grids(), {1, 7})), "rebind after reset");
    const auto destroys_before = cache.stats().destroys;
    h.cache.reset();
    expect(backend.textures.empty() && backend.bound.empty(), "teardown releases the texture and unbinds");
    expect(destroys_before == 5 && backend.log.back().rfind("destroy ", 0) == 0, "teardown ends with the texture release");
    const auto* retained = snapshot->fog_grids().find(7);
    expect(retained != nullptr && std::vector<std::uint8_t>(retained->cells().begin(), retained->cells().end()) == seven_cells,
           "retained snapshot grid survives cache teardown");
    expect(snapshot->completed_tick() == 9 && snapshot->fog_grids().size() == 2, "retained snapshot unchanged");
}

void test_byte_counts_scale() {
    Harness h;
    auto desc = base_desc(1);
    desc.width = 64;
    desc.height = 32;
    std::vector<std::uint8_t> cells(64 * 32);
    for (std::size_t i = 0; i < cells.size(); ++i) cells[i] = static_cast<std::uint8_t>(i * 7);
    (void)h.cache->submit(single(desc, cells), {5, 0});
    cells[2047] ^= 1;
    desc.revision = 2;
    (void)h.cache->submit(single(desc, cells), {5, 0});
    expect(h.cache->stats().upload_bytes == 4096 && h.backend.bytes == 4096, "64x32 create plus one update is 4096 bytes");
    expect(h.cache->stats().uploads == 2 && h.cache->stats().updates == 1, "two uploads for one create and one update");
}


class SerialExecutor final : public eawr::sim::PartitionExecutor {
public:
    [[nodiscard]] std::size_t worker_count() const noexcept override { return 1; }
    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t count, const std::function<void(std::size_t)>& partition) const override {
        for (std::size_t index = 0; index < count; ++index) partition(index);
        return eawr::core::Result<void>::success();
    }
};

// P2-05 (#68): the tactical session's sensor visibility replaces the painted stub as the
// grid source. Derived grids pass through the same cache and texture policy.
void test_tactical_visibility_source() {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    const auto at = [](const std::int64_t x, const std::int64_t y) {
        return math::Vec3{math::Fixed::from_raw(x * one), math::Fixed::from_raw(y * one), math::Fixed{}};
    };
    const std::vector<tactical::SensorProfile> sensors{{1, math::Fixed::from_raw(1200 * one)}};
    tactical::TacticalSetup setup;
    setup.players = {{1, 0, 1, 1}, {2, 1, 2, 1}};
    setup.units = {
        {1, 1, 1, at(-1024, 0), math::identity_quat(), {}}, // team 0 sensor, 1200
        {2, 2, 2, at(3000, 0), math::identity_quat(), {}},  // team 1, no sensor
    };
    // 8 x 4 cells of 1024 units from (-4096, -2048): cell centres at -3584 + 1024 i.
    const tactical::FogLayout layout{
        math::Fixed::from_raw(-4096 * one), math::Fixed::from_raw(-2048 * one),
        math::Fixed::from_raw(1024 * one), math::Fixed::from_raw(1024 * one), 8, 4};

    auto session = tactical::TacticalSession::create(setup, sensors);
    expect(static_cast<bool>(session), "tactical fog source: session is created");
    if (!session) return;
    const auto initial = tactical::fog_grids(*session.value().snapshot(), layout);
    expect(initial && initial.value().size() == 2, "tactical fog source: one grid per team");
    if (!initial) return;
    const auto& own = initial.value().grids()[0];
    // Only the four cells whose centres (-1536 or -512, -512 or 512) lie within 1200 of
    // (-1024, 0) are clear: each centre is sqrt(512^2 + 512^2) = 724 away.
    std::vector<std::uint8_t> expected(32, 0);
    for (const auto index : {10, 11, 18, 19}) expected[static_cast<std::size_t>(index)] = 255;
    expect(std::vector<std::uint8_t>(own.cells().begin(), own.cells().end()) == expected,
           "tactical fog source: team 0 cells are the sensor disc");
    expect(own.revision() == 1 && initial.value().grids()[1].cells()[18] == 0,
           "tactical fog source: revision is tick + 1 and a sensorless team stays dark");

    Harness h;
    const fog::StreamTeam key{68, 0};
    expect(static_cast<bool>(h.cache->add_consumer(3)), "tactical fog source: consumer registers");
    auto result = h.cache->submit(initial.value(), key);
    expect(result && result.value() == fog::SubmitAction::created, "tactical fog source: first grid creates");
    expect(h.backend.textures[h.cache->texture(key)].pixels == expected, "tactical fog source: texture holds the disc");

    const SerialExecutor executor;
    const auto tick = session.value().step(executor);
    expect(static_cast<bool>(tick), "tactical fog source: step succeeds");
    if (!tick) return;
    const auto next = tactical::fog_grids(*tick.value().snapshot, layout);
    result = next ? h.cache->submit(next.value(), key) : eawr::core::Result<fog::SubmitAction>::failure(next.error());
    expect(result && result.value() == fog::SubmitAction::metadata_only,
           "tactical fog source: an unchanged view at the next tick uploads nothing");
    expect(h.cache->stats().uploads == 1 && h.cache->accepted(key)->revision() == 2,
           "tactical fog source: the accepted revision follows the tick");
}

} // namespace

int main() {
    test_mapping_and_half_open_bounds();
    test_upload_lifecycle();
    test_fail_closed();
    test_consumers_teams_streams_and_release();
    test_byte_counts_scale();
    test_tactical_visibility_source();
    if (failures != 0) {
        std::cerr << failures << " fog presentation contract(s) failed\n";
        return 1;
    }
    std::cout << "fog presentation contracts passed\n";
    return 0;
}
