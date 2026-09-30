// P1-07 fog-stub-v1 CPU contract tests. Expected bytes and digests come from the
// independent oracle in fixtures/generate_fog_fixture.py, never from production.

#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/fog.hpp"
#include "eawr/sim/fog_sidecar.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/snapshot.hpp"
#include "eawr/sim/world.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace fog = eawr::sim::fog;
using Bytes = std::vector<std::uint8_t>;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename T>
void expect_code(const eawr::core::Result<T>& result, const std::string_view code, const std::string_view message) {
    if (result) {
        expect(false, std::string(message) + " (unexpectedly succeeded)");
        return;
    }
    if (result.error().code != code) {
        expect(false, std::string(message) + " (got " + result.error().code + ": " + result.error().message
            + ", expected " + std::string(code) + ")");
    }
}

[[nodiscard]] Bytes read_bytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    expect(static_cast<bool>(input), "open " + path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Text goldens may be checked out with CRLF; the contract is LF.
[[nodiscard]] std::string read_text(const std::string& path) {
    const auto bytes = read_bytes(path);
    std::string text;
    for (const auto byte : bytes) {
        if (byte != '\r') {
            text.push_back(static_cast<char>(byte));
        }
    }
    return text;
}

[[nodiscard]] std::vector<std::vector<std::string>> read_tsv(const std::string& path) {
    std::vector<std::vector<std::string>> rows;
    std::istringstream input(read_text(path));
    std::string line;
    bool header = true;
    while (std::getline(input, line)) {
        if (header) {
            header = false;
            continue;
        }
        std::vector<std::string> fields;
        std::size_t start = 0;
        while (true) {
            const auto tab = line.find('\t', start);
            fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
            if (tab == std::string::npos) {
                break;
            }
            start = tab + 1;
        }
        rows.push_back(std::move(fields));
    }
    return rows;
}

[[nodiscard]] std::uint64_t to_u64(const std::string& text) {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    expect(error == std::errc{} && end == text.data() + text.size(), "numeric field " + text);
    return value;
}

[[nodiscard]] std::string digest_hex(const std::array<std::uint8_t, 32>& digest) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text;
    for (const auto byte : digest) {
        text += digits[byte >> 4U];
        text += digits[byte & 0x0FU];
    }
    return text;
}

struct Paths {
    std::string fog;
    std::string replay;
};

[[nodiscard]] fog::FogGridDesc base_desc() {
    return fog::FogGridDesc{
        .team_id = 0,
        .width = 3,
        .height = 2,
        .origin_x_raw = -7 * (std::int64_t{1} << 24) / 2,
        .origin_y_raw = 41 * (std::int64_t{1} << 24) / 4,
        .cell_x_raw = 2 * (std::int64_t{1} << 24),
        .cell_y_raw = 3 * (std::int64_t{1} << 24) / 4,
        .encoding = fog::encoding_linear_u8_attenuation,
        .revision = 1,
    };
}

constexpr std::array<std::uint8_t, 6> base_cells{0, 64, 128, 192, 255, 17};

[[nodiscard]] fog::FogGrid make_grid(const fog::FogGridDesc& desc, const std::span<const std::uint8_t> cells) {
    auto grid = fog::FogGrid::create(desc, cells);
    if (!grid) {
        std::cerr << "FATAL: " << grid.error().message << '\n';
        std::exit(2);
    }
    return std::move(grid).value();
}

void test_fixture_round_trip(const Paths& paths) {
    const auto grids = read_tsv(paths.fog + "/fog-stub-v1.grids.tsv");
    expect(grids.size() == 14, "grid digest table has 14 rows");
    for (const std::string name : {"fog-stub-v1", "fog-stub-v1-one-cell", "fog-stub-v1-zero-tick"}) {
        const auto path = paths.fog + "/" + name + ".eawr-fog";
        const auto bytes = read_bytes(path);
        const auto parsed = fog::parse_fog_sidecar(bytes, path);
        expect(static_cast<bool>(parsed), "fixture parses: " + name);
        if (!parsed) {
            std::cerr << "  " << parsed.error().message << '\n';
            continue;
        }
        std::size_t matched = 0;
        for (const auto& row : grids) {
            if (row[0] != name) {
                continue;
            }
            const auto index = to_u64(row[1]);
            expect(index < parsed.value().events.size(), "grid row index in range");
            if (index >= parsed.value().events.size()) {
                continue;
            }
            const auto& event = parsed.value().events[index];
            expect(event.completed_tick == to_u64(row[2]), name + " event tick");
            expect(event.grid.team_id() == to_u64(row[3]), name + " event team");
            expect(event.grid.revision() == to_u64(row[4]), name + " event revision");
            const auto offset = to_u64(row[5]);
            const auto length = to_u64(row[6]);
            const auto canonical = event.grid.canonical_bytes();
            expect(canonical.size() == length, name + " canonical grid length");
            expect(offset + length <= bytes.size()
                    && std::equal(canonical.begin(), canonical.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset)),
                name + " canonical grid bytes equal oracle bytes at audited offset");
            expect(digest_hex(event.grid.sha256()) == row[7], name + " grid sha256 equals oracle");
            const auto reparsed = fog::parse_fog_grid(canonical);
            expect(reparsed && reparsed.value() == event.grid, name + " grid parses back");
            ++matched;
        }
        expect(matched == parsed.value().events.size(), name + " every event has an oracle row");
        const auto written = fog::write_fog_sidecar(parsed.value());
        expect(written && written.value() == bytes, name + " serializer reproduces oracle bytes");
    }
}

void test_asymmetric_grid_layout(const Paths& paths) {
    const auto parsed = fog::parse_fog_sidecar(read_bytes(paths.fog + "/fog-stub-v1.eawr-fog"));
    if (!parsed) {
        expect(false, "base sidecar parses for layout test");
        return;
    }
    const auto& team0 = parsed.value().events[0].grid;
    const auto& team7 = parsed.value().events[1].grid;
    expect(team0.desc().width == 3 && team0.desc().height == 2, "team 0 is 3x2");
    expect(team7.desc().width == 2 && team7.desc().height == 3, "team 7 is 2x3");
    // Row-major y*width+x with +x along source X and +y along source Y.
    expect(team0.cell(0, 0) == 0 && team0.cell(2, 0) == 128 && team0.cell(0, 1) == 192 && team0.cell(2, 1) == 17,
        "team 0 row-major cell addressing");
    expect(team7.cell(1, 0) == 0 && team7.cell(0, 2) == 33 && team7.cell(1, 2) == 1,
        "team 7 row-major cell addressing");
    expect(!team0.cell(3, 0).has_value() && !team0.cell(0, 2).has_value(), "cell outside grid is empty");
    expect(team0.desc().cell_x_raw != team0.desc().cell_y_raw, "rectangular cells preserved");
    expect(team0.desc().origin_x_raw < 0, "negative Q24 origin preserved");
}

void test_mutations(const Paths& paths) {
    const auto replay_bytes = read_bytes(paths.replay + "/original-v1.eawr-replay");
    const auto replay_sha = eawr::sim::sha256(replay_bytes);
    const auto rows = read_tsv(paths.fog + "/fog-stub-v1.mutations.tsv");
    expect(rows.size() >= 30, "mutation table is populated");
    const eawr::sim::InlineExecutor executor;
    for (const auto& row : rows) {
        const auto path = paths.fog + "/" + row[0];
        const auto bytes = read_bytes(path);
        const auto parsed = fog::parse_fog_sidecar(bytes, path);
        if (row[1] == "parse") {
            expect_code(parsed, row[2], "parse rejects " + row[0]);
        } else {
            expect(static_cast<bool>(parsed), "bind-stage mutation parses: " + row[0]);
            if (parsed) {
                const auto bound = fog::FogTimeline::bind(parsed.value(), replay_sha, 5, path);
                expect_code(bound, row[2], "bind rejects " + row[0]);
            }
        }
        // Evidence generation fails closed on every mutation, with the same code.
        const auto evidence = fog::compute_fog_evidence(replay_bytes, bytes, executor, path);
        expect_code(evidence, row[2], "evidence rejects " + row[0]);
    }
}

void test_truncation_sweep(const Paths& paths) {
    const auto bytes = read_bytes(paths.fog + "/fog-stub-v1.eawr-fog");
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        const auto parsed = fog::parse_fog_sidecar(std::span<const std::uint8_t>(bytes.data(), size));
        if (parsed) {
            expect(false, "truncated sidecar of " + std::to_string(size) + " bytes must fail");
            break;
        }
    }
    auto extended = bytes;
    extended.push_back(0);
    expect_code(fog::parse_fog_sidecar(extended), fog::diagnostic_codes::malformed, "trailing byte fails");
}

void test_grid_validation() {
    const auto desc = base_desc();
    expect(static_cast<bool>(fog::FogGrid::create(desc, base_cells)), "valid base grid");

    auto bad = desc;
    bad.width = 0;
    expect_code(fog::FogGrid::create(bad, {}), fog::diagnostic_codes::invalid_value, "zero width");
    bad = desc;
    bad.height = 4097;
    expect_code(fog::FogGrid::create(bad, base_cells), fog::diagnostic_codes::resource_limit, "height over 4096");
    bad = desc;
    bad.cell_x_raw = 0;
    expect_code(fog::FogGrid::create(bad, base_cells), fog::diagnostic_codes::invalid_value, "zero cell");
    bad = desc;
    bad.cell_y_raw = -1;
    expect_code(fog::FogGrid::create(bad, base_cells), fog::diagnostic_codes::invalid_value, "negative cell");
    bad = desc;
    bad.encoding = 2;
    expect_code(fog::FogGrid::create(bad, base_cells), fog::diagnostic_codes::version, "unknown encoding");
    bad = desc;
    bad.revision = 0;
    expect_code(fog::FogGrid::create(bad, base_cells), fog::diagnostic_codes::invalid_value, "revision zero");
    const std::array<std::uint8_t, 5> short_cells{};
    expect_code(fog::FogGrid::create(desc, short_cells), fog::diagnostic_codes::malformed, "cell count mismatch");

    // Exact int64 boundary: origin + width*cell == INT64_MAX passes; one more fails.
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    auto edge = desc;
    edge.origin_x_raw = max - 3 * desc.cell_x_raw;
    expect(static_cast<bool>(fog::FogGrid::create(edge, base_cells)), "extent exactly INT64_MAX passes");
    edge.origin_x_raw += 1;
    expect_code(fog::FogGrid::create(edge, base_cells), fog::diagnostic_codes::overflow, "extent INT64_MAX+1 overflows");
    edge = desc;
    edge.cell_y_raw = max / 2 + 1;
    edge.origin_y_raw = std::numeric_limits<std::int64_t>::min();
    expect_code(fog::FogGrid::create(edge, base_cells), fog::diagnostic_codes::overflow,
        "product overflow fails even with minimum origin");
    edge = desc;
    edge.origin_x_raw = std::numeric_limits<std::int64_t>::min();
    edge.origin_y_raw = std::numeric_limits<std::int64_t>::min();
    expect(static_cast<bool>(fog::FogGrid::create(edge, base_cells)), "minimum origins are valid");

    std::vector<std::uint8_t> big(4096U * 4096U, 7);
    auto largest = desc;
    largest.width = 4096;
    largest.height = 4096;
    expect(static_cast<bool>(fog::FogGrid::create(largest, big)), "4096x4096 grid is within limits");
}

void test_grid_immutability() {
    static_assert(std::is_same_v<decltype(std::declval<const fog::FogGrid&>().cells()), std::span<const std::uint8_t>>);
    static_assert(!std::is_default_constructible_v<fog::FogGrid>);
    std::vector<std::uint8_t> source(base_cells.begin(), base_cells.end());
    const auto grid = make_grid(base_desc(), source);
    const auto before = grid.sha256();
    source[0] = 99;  // mutate the caller's buffer after creation
    expect(grid.cell(0, 0) == 0 && grid.sha256() == before, "grid does not alias caller cells");
    const auto copy = grid;
    expect(copy == grid && copy.cells().data() == grid.cells().data(), "copies share one immutable buffer");
    expect(copy.cells().data() != source.data(), "shared buffer is owned by the grid");
}

void test_one_cell_sensitivity(const Paths& paths) {
    const auto grids = read_tsv(paths.fog + "/fog-stub-v1.grids.tsv");
    std::string base_digest;
    std::string one_cell_digest;
    for (const auto& row : grids) {
        if (row[1] == "0" && row[0] == "fog-stub-v1") {
            base_digest = row[7];
        }
        if (row[1] == "0" && row[0] == "fog-stub-v1-one-cell") {
            one_cell_digest = row[7];
        }
    }
    const auto base = make_grid(base_desc(), base_cells);
    expect(digest_hex(base.sha256()) == base_digest, "production base grid digest equals oracle");
    for (std::size_t index = 0; index < base_cells.size(); ++index) {
        auto cells = base_cells;
        cells[index] = static_cast<std::uint8_t>(cells[index] ^ 1U);
        const auto changed = make_grid(base_desc(), cells);
        expect(changed.sha256() != base.sha256(), "one-cell change at " + std::to_string(index) + " changes digest");
        if (index == 0) {
            expect(digest_hex(changed.sha256()) == one_cell_digest, "one-cell digest equals oracle");
        }
    }
    auto revised = base_desc();
    revised.revision = 2;
    expect(make_grid(revised, base_cells).sha256() != base.sha256(), "revision participates in digest");
    auto moved = base_desc();
    moved.origin_y_raw += 1;
    expect(make_grid(moved, base_cells).sha256() != base.sha256(), "one-raw origin change changes digest");
    auto other_team = base_desc();
    other_team.team_id = 1;
    expect(make_grid(other_team, base_cells).sha256() != base.sha256(), "team participates in digest");

    const auto base_rows = read_text(paths.fog + "/fog-stub-v1.evidence.csv");
    const auto cell_rows = read_text(paths.fog + "/fog-stub-v1-one-cell.evidence.csv");
    std::istringstream left(base_rows);
    std::istringstream right(cell_rows);
    std::string left_line;
    std::string right_line;
    std::getline(left, left_line);
    std::getline(right, right_line);
    std::size_t rows = 0;
    while (std::getline(left, left_line) && std::getline(right, right_line)) {
        expect(left_line != right_line, "one-cell sidecar changes evidence row " + std::to_string(rows));
        ++rows;
    }
    expect(rows == 6, "six evidence rows compared for one-cell sensitivity");
}

void test_grid_sets() {
    auto desc = base_desc();
    const auto team0 = make_grid(desc, base_cells);
    desc.team_id = 5;
    const auto team5 = make_grid(desc, base_cells);
    const auto set = fog::FogGridSet::create({team0, team5});
    expect(set && set.value().size() == 2, "sorted unique set");
    if (set) {
        expect(set.value().find(5) != nullptr && set.value().find(5)->team_id() == 5, "find team 5");
        expect(set.value().find(0) != nullptr, "team zero is valid and found");
        expect(set.value().find(3) == nullptr, "absent team has no grid");
    }
    expect_code(fog::FogGridSet::create({team5, team0}), fog::diagnostic_codes::order, "unsorted teams rejected");
    expect_code(fog::FogGridSet::create({team0, team0}), fog::diagnostic_codes::order, "duplicate teams rejected");
    const fog::FogGridSet empty;
    expect(empty.empty() && empty.size() == 0 && empty.grids().empty(), "default set is no attachment");
    const auto created_empty = fog::FogGridSet::create({});
    expect(created_empty && created_empty.value().empty(), "empty collection means no attachment");

    std::vector<fog::FogGrid> many;
    for (std::uint32_t team = 0; team < 65; ++team) {
        auto one = base_desc();
        one.team_id = team;
        one.width = 1;
        one.height = 1;
        many.push_back(make_grid(one, std::array<std::uint8_t, 1>{1}));
    }
    expect_code(fog::FogGridSet::create(many), fog::diagnostic_codes::resource_limit, "65 grids rejected");
    many.pop_back();
    expect(static_cast<bool>(fog::FogGridSet::create(many)), "64 grids accepted");

    std::vector<std::uint8_t> big(4096U * 4096U, 1);
    auto huge = base_desc();
    huge.width = 4096;
    huge.height = 4096;
    const auto huge_grid = make_grid(huge, big);
    auto tiny = base_desc();
    tiny.team_id = 1;
    tiny.width = 1;
    tiny.height = 1;
    const auto tiny_grid = make_grid(tiny, std::array<std::uint8_t, 1>{1});
    expect(static_cast<bool>(fog::FogGridSet::create({huge_grid})), "exactly 16 MiB aggregate accepted");
    expect_code(fog::FogGridSet::create({huge_grid, tiny_grid}), fog::diagnostic_codes::resource_limit,
        "16 MiB + 1 aggregate cells rejected");
}

[[nodiscard]] fog::FogSidecar stream(std::vector<fog::FogSidecarEvent> events, const std::uint64_t final_tick = 5) {
    return fog::FogSidecar{.replay_sha256 = {}, .final_completed_tick = final_tick, .events = std::move(events)};
}

void test_stream_rules() {
    const auto r1 = make_grid(base_desc(), base_cells);
    auto changed_cells = base_cells;
    changed_cells[5] = 18;
    auto desc2 = base_desc();
    desc2.revision = 2;
    const auto r2 = make_grid(desc2, changed_cells);
    const auto r2_same_content = make_grid(desc2, base_cells);
    const auto r1_changed = make_grid(base_desc(), changed_cells);

    expect(static_cast<bool>(fog::write_fog_sidecar(stream({{0, r1}, {2, r2}}))), "changed grid with advanced revision");
    expect(static_cast<bool>(fog::write_fog_sidecar(stream({{0, r1}, {2, r1}}))), "identical grid retains revision");
    expect(static_cast<bool>(fog::write_fog_sidecar(stream({{0, r1}, {2, r2_same_content}}))),
        "revision-only bump allowed");
    expect_code(fog::write_fog_sidecar(stream({{0, r1}, {2, r1_changed}})), fog::diagnostic_codes::revision,
        "serializer rejects revision reuse with different data");
    expect_code(fog::write_fog_sidecar(stream({{0, r1}, {2, r2}, {3, r1}})), fog::diagnostic_codes::revision,
        "serializer rejects revision decrease");
    expect_code(fog::write_fog_sidecar(stream({{0, r2}})), fog::diagnostic_codes::revision,
        "first revision must be 1");
    expect_code(fog::write_fog_sidecar(stream({{0, r1}, {6, r2}})), fog::diagnostic_codes::order,
        "tick beyond final rejected");
    expect_code(fog::write_fog_sidecar(stream({})), fog::diagnostic_codes::invalid_value, "empty stream rejected");
    expect_code(fog::write_fog_sidecar(stream({{0, r1}}, 1'000'001)), fog::diagnostic_codes::resource_limit,
        "final tick limit");
}

void test_snapshot_attachment(const Paths& paths) {
    // The legacy constructor stays valid and has no fog attachment.
    const eawr::sim::RenderSnapshot legacy(3, {});
    expect(legacy.completed_tick() == 3 && legacy.fog_grids().empty(), "legacy snapshot has no fog");
    const eawr::sim::RenderSnapshot defaulted;
    expect(defaulted.fog_grids().empty(), "default snapshot has no fog");

    const auto replay_bytes = read_bytes(paths.replay + "/original-v1.eawr-replay");
    const auto replay = eawr::sim::parse_replay(replay_bytes);
    const auto sidecar = fog::parse_fog_sidecar(read_bytes(paths.fog + "/fog-stub-v1.eawr-fog"));
    if (!replay || !sidecar) {
        expect(false, "snapshot attachment inputs parse");
        return;
    }
    std::shared_ptr<const eawr::sim::RenderSnapshot> enriched;
    std::string hash_before;
    std::shared_ptr<const eawr::sim::RenderSnapshot> world_snapshot;
    {
        const auto timeline = fog::FogTimeline::bind(sidecar.value(), eawr::sim::sha256(replay_bytes), 5);
        expect(static_cast<bool>(timeline), "fixture binds to original-v1");
        if (!timeline) {
            return;
        }
        expect_code(timeline.value().active_at(6), fog::diagnostic_codes::order, "tick beyond timeline");
        auto world = eawr::sim::World::create(replay.value());
        const eawr::sim::InlineExecutor executor;
        static_cast<void>(world.value().step(executor));
        static_cast<void>(world.value().step(executor));
        world_snapshot = world.value().snapshot();
        hash_before = world.value().state_sha256();
        const auto attached = fog::attach_fog(*world_snapshot, timeline.value());
        expect(static_cast<bool>(attached), "attach fog at tick 2");
        if (attached) {
            enriched = attached.value();
        }
        expect(world.value().state_sha256() == hash_before, "attaching fog does not change world hash");
        expect(world.value().snapshot() == world_snapshot, "world snapshot pointer unchanged");
    }
    expect(world_snapshot->fog_grids().empty(), "world-published snapshot never carries fog");
    if (!enriched) {
        return;
    }
    // Retained after the timeline and world are gone.
    expect(enriched->completed_tick() == 2, "enriched tick copied");
    expect(std::ranges::equal(enriched->instances(), world_snapshot->instances()), "enriched instances copied");
    expect(enriched->instances().data() != world_snapshot->instances().data(), "instances are an independent copy");
    expect(enriched->fog_grids().size() == 2, "both teams attached");
    const auto* team0 = enriched->fog_grids().find(0);
    const auto* team7 = enriched->fog_grids().find(7);
    expect(team0 != nullptr && team0->revision() == 2 && team0->cell(2, 1) == 18, "team 0 at tick 2 is revision 2");
    expect(team7 != nullptr && team7->revision() == 1, "team 7 at tick 2 is revision 1");
    const auto rebuilt = eawr::sim::RenderSnapshot(
        enriched->completed_tick(),
        std::vector<eawr::sim::RenderInstance>(enriched->instances().begin(), enriched->instances().end()),
        enriched->fog_grids());
    expect(rebuilt.fog_grids().grids().data() == enriched->fog_grids().grids().data(), "fog set shares immutable data");
}

void test_active_sets(const Paths& paths) {
    const auto replay_bytes = read_bytes(paths.replay + "/original-v1.eawr-replay");
    const auto sidecar = fog::parse_fog_sidecar(read_bytes(paths.fog + "/fog-stub-v1.eawr-fog"));
    if (!sidecar) {
        expect(false, "sidecar parses for active-set test");
        return;
    }
    const auto timeline = fog::FogTimeline::bind(sidecar.value(), eawr::sim::sha256(replay_bytes), 5);
    if (!timeline) {
        expect(false, "timeline binds");
        return;
    }
    const std::array<std::array<std::uint64_t, 2>, 6> expected{{
        {1, 1}, {1, 1}, {2, 1}, {2, 1}, {3, 1}, {3, 2},
    }};
    for (std::uint64_t tick = 0; tick <= 5; ++tick) {
        const auto active = timeline.value().active_at(tick);
        expect(active && active.value().size() == 2, "two active grids at tick " + std::to_string(tick));
        if (active) {
            expect(active.value().grids()[0].revision() == expected[tick][0]
                    && active.value().grids()[1].revision() == expected[tick][1],
                "active revisions at tick " + std::to_string(tick));
        }
    }
    // (1 + 1 + 1 + 1 + 1 + 1) ticks x 2 teams x 82 bytes.
    expect(timeline.value().cumulative_evidence_grid_bytes() == 6U * 2U * 82U, "cumulative grid bytes");
}

void test_evidence(const Paths& paths) {
    const auto world_rows = read_tsv(paths.fog + "/fog-stub-v1.world.tsv");
    std::map<std::string, std::vector<std::string>> world_hashes;
    for (const auto& row : world_rows) {
        world_hashes[row[0]].push_back(row[2]);
    }
    // The v1 world hashes (including tick 0) equal the independent oracle.
    for (const auto& [name, hashes] : world_hashes) {
        const auto replay = eawr::sim::parse_replay(read_bytes(paths.replay + "/" + name));
        auto world = eawr::sim::World::create(replay.value());
        expect(world.value().state_sha256() == hashes[0], name + " tick-0 world hash equals oracle");
        const eawr::sim::InlineExecutor executor;
        for (std::size_t tick = 1; tick < hashes.size(); ++tick) {
            const auto step = world.value().step(executor);
            expect(step && step.value().state_sha256 == hashes[tick], name + " world hash at tick " + std::to_string(tick));
        }
    }

    const std::array<std::pair<std::string_view, std::string_view>, 3> fixtures{{
        {"fog-stub-v1", "original-v1.eawr-replay"},
        {"fog-stub-v1-one-cell", "original-v1.eawr-replay"},
        {"fog-stub-v1-zero-tick", "zero-tick-v1.eawr-replay"},
    }};
    for (const auto& [name, replay_name] : fixtures) {
        const auto replay = read_bytes(paths.replay + "/" + std::string(replay_name));
        const auto sidecar = read_bytes(paths.fog + "/" + std::string(name) + ".eawr-fog");
        const auto golden = read_text(paths.fog + "/" + std::string(name) + ".evidence.csv");
        for (const auto workers : eawr::platform::determinism_worker_counts()) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            const auto rows = fog::compute_fog_evidence(replay, sidecar, executor);
            expect(static_cast<bool>(rows), std::string(name) + " evidence computes");
            if (rows) {
                expect(fog::format_fog_evidence_csv(rows.value()) == golden,
                    std::string(name) + " evidence equals oracle golden with workers " + std::to_string(workers));
            }
        }
    }

    // Replay identity: the same sidecar against another replay fails closed.
    const auto zero_replay = read_bytes(paths.replay + "/zero-tick-v1.eawr-replay");
    const eawr::sim::InlineExecutor inline_executor;
    expect_code(fog::compute_fog_evidence(zero_replay, read_bytes(paths.fog + "/fog-stub-v1.eawr-fog"), inline_executor),
        fog::diagnostic_codes::identity_mismatch, "sidecar bound to another replay");
}

void test_evidence_limit() {
    eawr::sim::Replay replay;
    replay.tick_numerator = 1;
    replay.tick_denominator = 30;
    replay.final_tick_count = 1'000'000;
    const auto replay_bytes = eawr::sim::write_replay(replay);
    expect(static_cast<bool>(replay_bytes), "long empty replay encodes");
    const auto replay_sha = eawr::sim::sha256(replay_bytes.value());
    const eawr::sim::InlineExecutor executor;

    const auto sidecar_for = [&](const std::uint32_t side) {
        auto desc = base_desc();
        desc.width = side;
        desc.height = side;
        const std::vector<std::uint8_t> cells(static_cast<std::size_t>(side) * side, 3);
        auto sidecar = stream({{0, make_grid(desc, cells)}}, replay.final_tick_count);
        sidecar.replay_sha256 = replay_sha;
        return sidecar;
    };
    // 33x33: (76 + 1089) x 1,000,001 ticks exceeds 1 GiB before any hashing.
    const auto over = sidecar_for(33);
    const auto over_bytes = fog::write_fog_sidecar(over);
    expect_code(fog::compute_fog_evidence(replay_bytes.value(), over_bytes.value(), executor),
        fog::diagnostic_codes::evidence_limit, "evidence over 1 GiB rejected before hashing");
    const auto over_timeline = fog::FogTimeline::bind(over, replay_sha, replay.final_tick_count);
    expect(over_timeline && over_timeline.value().cumulative_evidence_grid_bytes() == 1'165U * 1'000'001U,
        "cumulative bytes over limit");
    // 31x31: (76 + 961) x 1,000,001 is within 1 GiB.
    const auto under_timeline = fog::FogTimeline::bind(sidecar_for(31), replay_sha, replay.final_tick_count);
    expect(under_timeline
            && under_timeline.value().cumulative_evidence_grid_bytes() == 1'037U * 1'000'001U
            && under_timeline.value().cumulative_evidence_grid_bytes() <= fog::evidence_max_grid_bytes,
        "cumulative bytes under limit");
}

void test_original_v1_frozen(const Paths& paths) {
    const auto bytes = read_bytes(paths.replay + "/original-v1.eawr-replay");
    expect(bytes.size() == 1480, "original-v1 is 1,480 bytes");
    expect(eawr::sim::sha256_hex(bytes) == "fcf7f050a4ae8be540def3e4609fc7ff68a21e4ed4fce59e46c1fc62e59a4f90",
        "original-v1 SHA-256 unchanged");
    expect(eawr::sim::replay_format_version == 1 && eawr::sim::simulation_rules_version == 1
            && eawr::sim::state_encoding_version == 1,
        "replay, rules and state encoding stay version 1");
}

int write_evidence(const std::string& replay_path, const std::string& sidecar_path, const std::string& out_path,
                   const std::size_t workers) {
    const auto replay_bytes = read_bytes(replay_path);
    const auto sidecar_bytes = read_bytes(sidecar_path);
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    const auto rows = fog::compute_fog_evidence(replay_bytes, sidecar_bytes, executor, sidecar_path);
    if (!rows) {
        std::cerr << rows.error().code << ": " << rows.error().message << '\n';
        return 1;
    }
    std::ofstream output(out_path, std::ios::binary);
    output << fog::format_fog_evidence_csv(rows.value());
    if (!output) {
        return 1;
    }

    // These identities travel with the rows into the CI artifact. The separate
    // comparator checks them against the raw fixture bytes, so a copied CSV
    // cannot be relabelled as evidence for another input sidecar or replay.
    const auto directory = std::filesystem::path(out_path).parent_path();
    const auto write_identity = [&](const std::string_view name, const std::string& digest) {
        std::ofstream identity(directory / std::string(name), std::ios::binary);
        identity << digest << '\n';
        return static_cast<bool>(identity);
    };
    return write_identity("replay.sha256", eawr::sim::sha256_hex(replay_bytes))
            && write_identity("sidecar.sha256", eawr::sim::sha256_hex(sidecar_bytes))
        ? 0
        : 1;
}

} // namespace

int main(const int argc, char** argv) {
    if (argc == 6 && std::string_view(argv[1]) == "--evidence") {
        // fog_contract_tests --evidence <replay> <sidecar> <out.csv> <workers>
        const auto workers = std::string_view(argv[5]);
        if (workers != "1" && workers != "2" && workers != "4") {
            std::cerr << "workers must be 1, 2 or 4\n";
            return 2;
        }
        return write_evidence(argv[2], argv[3], argv[4], static_cast<std::size_t>(workers[0] - '0'));
    }
    if (argc != 3) {
        std::cerr << "usage: fog_contract_tests <fog-fixture-dir> <replay-fixture-dir>\n"
                     "       fog_contract_tests --evidence <replay> <sidecar> <out.csv> <1|2|4>\n";
        return 2;
    }
    const Paths paths{argv[1], argv[2]};
    test_original_v1_frozen(paths);
    test_fixture_round_trip(paths);
    test_asymmetric_grid_layout(paths);
    test_mutations(paths);
    test_truncation_sweep(paths);
    test_grid_validation();
    test_grid_immutability();
    test_one_cell_sensitivity(paths);
    test_grid_sets();
    test_stream_rules();
    test_snapshot_attachment(paths);
    test_active_sets(paths);
    test_evidence(paths);
    test_evidence_limit();
    if (failures != 0) {
        std::cerr << failures << " fog contract failure(s)\n";
        return 1;
    }
    std::cout << "fog contract tests passed\n";
    return 0;
}
