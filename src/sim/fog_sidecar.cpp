#include "eawr/sim/fog_sidecar.hpp"

#include "fog_internal.hpp"
#include "replay_internal.hpp"

#include "eawr/sim/replay.hpp"
#include "eawr/sim/world.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace eawr::sim::fog {
namespace {

constexpr std::array<std::uint8_t, 8> sidecar_magic{'E', 'A', 'W', 'R', 'F', 'G', 'F', 0};
constexpr std::array<std::uint8_t, 8> evidence_magic{'E', 'A', 'W', 'R', 'F', 'G', 'E', 0};
// Tick, length and a grid of at least one cell.
constexpr std::size_t min_event_size = 16 + grid_header_size + 1;

using detail::fog_diagnostic;

template <typename T>
[[nodiscard]] core::Result<T> fail(
    const std::string_view code,
    std::string message,
    const std::string_view logical_path) {
    return core::Result<T>::failure(fog_diagnostic(code, std::move(message), logical_path));
}

[[nodiscard]] std::string event_label(const std::size_t index, const FogSidecarEvent& event) {
    return "fog event " + std::to_string(index) + " (tick " + std::to_string(event.completed_tick)
        + ", team " + std::to_string(event.grid.team_id()) + "): ";
}

[[nodiscard]] std::uint64_t saturating_add(const std::uint64_t left, const std::uint64_t right) noexcept {
    return left > std::numeric_limits<std::uint64_t>::max() - right
        ? std::numeric_limits<std::uint64_t>::max()
        : left + right;
}

[[nodiscard]] std::uint64_t saturating_mul(const std::uint64_t left, const std::uint64_t right) noexcept {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left * right;
}

[[nodiscard]] std::optional<std::array<std::uint8_t, 32>> decode_sha256_hex(const std::string_view text) {
    if (text.size() != 64) {
        return std::nullopt;
    }
    const auto nibble = [](const char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        return -1;
    };
    std::array<std::uint8_t, 32> digest{};
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto high = nibble(text[index * 2]);
        const auto low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        digest[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return digest;
}

} // namespace

core::Result<void> validate_fog_sidecar(const FogSidecar& sidecar, const std::string_view logical_path) {
    if (sidecar.final_completed_tick > sidecar_max_final_tick) {
        return fail<void>(diagnostic_codes::resource_limit,
            "fog sidecar final completed tick exceeds 1,000,000", logical_path);
    }
    if (sidecar.events.empty()) {
        return fail<void>(diagnostic_codes::invalid_value, "fog sidecar has no events", logical_path);
    }
    if (sidecar.events.size() > sidecar_max_events) {
        return fail<void>(diagnostic_codes::resource_limit,
            "fog sidecar event count exceeds 1,000,000", logical_path);
    }
    struct TeamState {
        std::uint32_t team_id{};
        std::size_t last_event{};
    };
    std::vector<TeamState> teams;
    std::uint64_t active_cells = 0;
    for (std::size_t index = 0; index < sidecar.events.size(); ++index) {
        const auto& event = sidecar.events[index];
        const auto& grid = event.grid;
        const auto team = grid.team_id();
        const auto cells = static_cast<std::uint64_t>(grid.cells().size());
        const auto valid_grid = detail::validate_grid_desc(grid.desc(), grid_schema_version, logical_path);
        if (!valid_grid) {
            return valid_grid;
        }
        if (cells != static_cast<std::uint64_t>(grid.desc().width) * grid.desc().height) {
            return fail<void>(diagnostic_codes::malformed,
                event_label(index, event) + "cell count does not equal width*height", logical_path);
        }
        if (event.completed_tick > sidecar.final_completed_tick) {
            return fail<void>(diagnostic_codes::order,
                event_label(index, event) + "tick is beyond final completed tick "
                    + std::to_string(sidecar.final_completed_tick), logical_path);
        }
        if (index > 0) {
            const auto& previous = sidecar.events[index - 1];
            const auto previous_key = std::pair{previous.completed_tick, previous.grid.team_id()};
            if (std::pair{event.completed_tick, team} <= previous_key) {
                return fail<void>(diagnostic_codes::order,
                    event_label(index, event) + "(tick, team) is not strictly increasing", logical_path);
            }
        }
        if (event.completed_tick == 0) {
            // Strict (tick, team) ordering makes each tick-0 team unique and sorted.
            if (grid.revision() != 1) {
                return fail<void>(diagnostic_codes::revision,
                    event_label(index, event) + "first revision must be 1", logical_path);
            }
            if (teams.size() == max_grids) {
                return fail<void>(diagnostic_codes::resource_limit,
                    event_label(index, event) + "more than 64 teams", logical_path);
            }
            teams.push_back(TeamState{team, index});
            active_cells += cells;
        } else {
            const auto found = std::lower_bound(teams.begin(), teams.end(), team,
                [](const TeamState& state, const std::uint32_t value) { return state.team_id < value; });
            if (found == teams.end() || found->team_id != team) {
                return fail<void>(diagnostic_codes::order,
                    event_label(index, event) + "team has no tick-0 grid", logical_path);
            }
            const auto& previous = sidecar.events[found->last_event].grid;
            if (grid.revision() < previous.revision()) {
                return fail<void>(diagnostic_codes::revision,
                    event_label(index, event) + "revision " + std::to_string(grid.revision())
                        + " decreases from " + std::to_string(previous.revision()), logical_path);
            }
            if (grid.revision() == previous.revision() && !(grid == previous)) {
                return fail<void>(diagnostic_codes::revision,
                    event_label(index, event) + "revision " + std::to_string(grid.revision())
                        + " reused with different grid data", logical_path);
            }
            active_cells = active_cells - previous.cells().size() + cells;
            found->last_event = index;
        }
        if (active_cells > max_collection_cells) {
            return fail<void>(diagnostic_codes::resource_limit,
                event_label(index, event) + "active grids exceed 16 MiB aggregate cells", logical_path);
        }
    }
    return core::Result<void>::success();
}

core::Result<FogSidecar> parse_fog_sidecar(const std::span<const std::uint8_t> bytes, const std::string_view logical_path) {
    if (bytes.size() > sidecar_max_bytes) {
        return fail<FogSidecar>(diagnostic_codes::resource_limit, "fog sidecar exceeds 256 MiB", logical_path);
    }
    if (bytes.size() < sidecar_header_size) {
        return fail<FogSidecar>(diagnostic_codes::malformed, "truncated fog sidecar header", logical_path);
    }
    detail::ByteReader reader(bytes);
    std::array<std::uint8_t, 8> magic{};
    std::uint32_t version{};
    FogSidecar sidecar;
    std::uint64_t event_count{};
    // The header length was checked above, so these reads cannot fail.
    static_cast<void>(reader.read_bytes(magic));
    static_cast<void>(reader.read_u32(version));
    static_cast<void>(reader.read_bytes(sidecar.replay_sha256));
    static_cast<void>(reader.read_u64(sidecar.final_completed_tick));
    static_cast<void>(reader.read_u64(event_count));
    if (magic != sidecar_magic) {
        return fail<FogSidecar>(diagnostic_codes::malformed, "fog sidecar magic is not EAWRFGF", logical_path);
    }
    if (version != sidecar_version) {
        return fail<FogSidecar>(diagnostic_codes::version,
            "unsupported fog sidecar version " + std::to_string(version), logical_path);
    }
    if (sidecar.final_completed_tick > sidecar_max_final_tick) {
        return fail<FogSidecar>(diagnostic_codes::resource_limit,
            "fog sidecar final completed tick exceeds 1,000,000", logical_path);
    }
    if (event_count > sidecar_max_events) {
        return fail<FogSidecar>(diagnostic_codes::resource_limit,
            "fog sidecar event count exceeds 1,000,000", logical_path);
    }
    if (event_count == 0) {
        return fail<FogSidecar>(diagnostic_codes::invalid_value, "fog sidecar has no events", logical_path);
    }
    if (event_count > reader.remaining() / min_event_size) {
        return fail<FogSidecar>(diagnostic_codes::malformed,
            "fog sidecar event count " + std::to_string(event_count) + " exceeds the remaining bytes",
            logical_path);
    }
    sidecar.events.reserve(static_cast<std::size_t>(event_count));
    for (std::uint64_t index = 0; index < event_count; ++index) {
        std::uint64_t tick{};
        std::uint64_t length{};
        if (!reader.read_u64(tick) || !reader.read_u64(length)) {
            return fail<FogSidecar>(diagnostic_codes::malformed,
                "truncated fog event " + std::to_string(index) + " header", logical_path);
        }
        if (length < grid_header_size || length > reader.remaining()) {
            return fail<FogSidecar>(diagnostic_codes::malformed,
                "fog event " + std::to_string(index) + " grid length " + std::to_string(length)
                    + " is invalid for the remaining bytes", logical_path);
        }
        auto grid = parse_fog_grid(reader.take(static_cast<std::size_t>(length)), logical_path);
        if (!grid) {
            auto error = std::move(grid.error());
            error.message = "fog event " + std::to_string(index) + ": " + error.message;
            return core::Result<FogSidecar>::failure(std::move(error));
        }
        sidecar.events.push_back(FogSidecarEvent{tick, std::move(grid).value()});
    }
    if (reader.remaining() != 0) {
        return fail<FogSidecar>(diagnostic_codes::malformed,
            "fog sidecar has " + std::to_string(reader.remaining()) + " trailing bytes", logical_path);
    }
    const auto valid = validate_fog_sidecar(sidecar, logical_path);
    if (!valid) {
        return core::Result<FogSidecar>::failure(valid.error());
    }
    return core::Result<FogSidecar>::success(std::move(sidecar));
}

core::Result<std::vector<std::uint8_t>> write_fog_sidecar(const FogSidecar& sidecar) {
    const auto valid = validate_fog_sidecar(sidecar, {});
    if (!valid) {
        return core::Result<std::vector<std::uint8_t>>::failure(valid.error());
    }
    std::size_t size = sidecar_header_size;
    for (const auto& event : sidecar.events) {
        const auto event_size = 16 + event.grid.canonical_size();
        if (size > sidecar_max_bytes - event_size) {
            return fail<std::vector<std::uint8_t>>(diagnostic_codes::resource_limit,
                "encoded fog sidecar exceeds the 256 MiB output limit", {});
        }
        size += event_size;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(size);
    bytes.insert(bytes.end(), sidecar_magic.begin(), sidecar_magic.end());
    sim::detail::append_u32(bytes, sidecar_version);
    bytes.insert(bytes.end(), sidecar.replay_sha256.begin(), sidecar.replay_sha256.end());
    sim::detail::append_u64(bytes, sidecar.final_completed_tick);
    sim::detail::append_u64(bytes, sidecar.events.size());
    for (const auto& event : sidecar.events) {
        sim::detail::append_u64(bytes, event.completed_tick);
        sim::detail::append_u64(bytes, event.grid.canonical_size());
        detail::append_grid(bytes, event.grid);
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

struct FogTimeline::State {
    FogSidecar sidecar;
    // Event indices per team, ascending team order and ascending tick.
    std::vector<std::vector<std::size_t>> team_events;
    std::uint64_t cumulative_grid_bytes{};
};

FogTimeline::FogTimeline(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

core::Result<FogTimeline> FogTimeline::bind(
    FogSidecar sidecar,
    const std::array<std::uint8_t, 32>& replay_file_sha256,
    const std::uint64_t replay_final_tick_count,
    const std::string_view logical_path) {
    const auto valid = validate_fog_sidecar(sidecar, logical_path);
    if (!valid) {
        return core::Result<FogTimeline>::failure(valid.error());
    }
    if (sidecar.replay_sha256 != replay_file_sha256) {
        return fail<FogTimeline>(diagnostic_codes::identity_mismatch,
            "fog sidecar is bound to a different replay file SHA-256", logical_path);
    }
    if (sidecar.final_completed_tick != replay_final_tick_count) {
        return fail<FogTimeline>(diagnostic_codes::identity_mismatch,
            "fog sidecar final completed tick " + std::to_string(sidecar.final_completed_tick)
                + " does not equal replay final_tick_count " + std::to_string(replay_final_tick_count),
            logical_path);
    }
    auto state = std::make_shared<State>();
    std::vector<std::uint32_t> team_ids;
    for (std::size_t index = 0; index < sidecar.events.size(); ++index) {
        const auto& event = sidecar.events[index];
        if (event.completed_tick == 0) {
            team_ids.push_back(event.grid.team_id());
            state->team_events.push_back({index});
            continue;
        }
        const auto found = std::lower_bound(team_ids.begin(), team_ids.end(), event.grid.team_id());
        state->team_events[static_cast<std::size_t>(found - team_ids.begin())].push_back(index);
    }
    std::uint64_t total = 0;
    for (const auto& indices : state->team_events) {
        for (std::size_t position = 0; position < indices.size(); ++position) {
            const auto start = sidecar.events[indices[position]].completed_tick;
            const auto end = position + 1 < indices.size()
                ? sidecar.events[indices[position + 1]].completed_tick
                : sidecar.final_completed_tick + 1;
            const auto size = static_cast<std::uint64_t>(sidecar.events[indices[position]].grid.canonical_size());
            total = saturating_add(total, saturating_mul(end - start, size));
        }
    }
    state->cumulative_grid_bytes = total;
    state->sidecar = std::move(sidecar);
    return core::Result<FogTimeline>::success(FogTimeline(std::move(state)));
}

std::uint64_t FogTimeline::final_completed_tick() const noexcept {
    return state_->sidecar.final_completed_tick;
}

const std::array<std::uint8_t, 32>& FogTimeline::replay_sha256() const noexcept {
    return state_->sidecar.replay_sha256;
}

std::uint64_t FogTimeline::cumulative_evidence_grid_bytes() const noexcept {
    return state_->cumulative_grid_bytes;
}

core::Result<FogGridSet> FogTimeline::active_at(const std::uint64_t completed_tick) const {
    if (completed_tick > state_->sidecar.final_completed_tick) {
        return fail<FogGridSet>(diagnostic_codes::order,
            "completed tick " + std::to_string(completed_tick) + " is beyond the fog timeline", {});
    }
    const auto& events = state_->sidecar.events;
    std::vector<FogGrid> active;
    active.reserve(state_->team_events.size());
    for (const auto& indices : state_->team_events) {
        // indices[0] is the team's tick-0 event, so a match always exists.
        const auto after = std::upper_bound(indices.begin(), indices.end(), completed_tick,
            [&events](const std::uint64_t tick, const std::size_t index) { return tick < events[index].completed_tick; });
        active.push_back(events[*(after - 1)].grid);
    }
    return FogGridSet::create(std::move(active));
}

core::Result<std::shared_ptr<const RenderSnapshot>> attach_fog(
    const RenderSnapshot& completed,
    const FogTimeline& timeline) {
    auto active = timeline.active_at(completed.completed_tick());
    if (!active) {
        return core::Result<std::shared_ptr<const RenderSnapshot>>::failure(active.error());
    }
    const auto instances = completed.instances();
    return core::Result<std::shared_ptr<const RenderSnapshot>>::success(std::make_shared<const RenderSnapshot>(
        completed.completed_tick(),
        std::vector<RenderInstance>(instances.begin(), instances.end()),
        std::move(active).value()));
}

std::vector<std::uint8_t> fog_evidence_preimage(
    const std::array<std::uint8_t, 32>& replay_file_sha256,
    const std::array<std::uint8_t, 32>& sidecar_file_sha256,
    const std::uint64_t completed_tick,
    const std::array<std::uint8_t, 32>& world_state_sha256,
    const FogGridSet& active) {
    std::size_t size = 8 + 4 + 32 + 32 + 8 + 32 + 4;
    for (const auto& grid : active.grids()) {
        size += 8 + grid.canonical_size();
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(size);
    bytes.insert(bytes.end(), evidence_magic.begin(), evidence_magic.end());
    sim::detail::append_u32(bytes, evidence_version);
    bytes.insert(bytes.end(), replay_file_sha256.begin(), replay_file_sha256.end());
    bytes.insert(bytes.end(), sidecar_file_sha256.begin(), sidecar_file_sha256.end());
    sim::detail::append_u64(bytes, completed_tick);
    bytes.insert(bytes.end(), world_state_sha256.begin(), world_state_sha256.end());
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(active.size()));
    for (const auto& grid : active.grids()) {
        sim::detail::append_u64(bytes, grid.canonical_size());
        detail::append_grid(bytes, grid);
    }
    return bytes;
}

core::Result<std::vector<FogEvidenceRow>> compute_fog_evidence(
    const std::span<const std::uint8_t> replay_file_bytes,
    const std::span<const std::uint8_t> sidecar_file_bytes,
    const PartitionExecutor& executor,
    const std::string_view sidecar_logical_path) {
    using Rows = std::vector<FogEvidenceRow>;
    auto replay = parse_replay(replay_file_bytes);
    if (!replay) {
        return core::Result<Rows>::failure(replay.error());
    }
    auto sidecar = parse_fog_sidecar(sidecar_file_bytes, sidecar_logical_path);
    if (!sidecar) {
        return core::Result<Rows>::failure(sidecar.error());
    }
    const auto replay_sha = sim::sha256(replay_file_bytes);
    const auto sidecar_sha = sim::sha256(sidecar_file_bytes);
    const auto timeline = FogTimeline::bind(
        std::move(sidecar).value(), replay_sha, replay.value().final_tick_count, sidecar_logical_path);
    if (!timeline) {
        return core::Result<Rows>::failure(timeline.error());
    }
    if (timeline.value().cumulative_evidence_grid_bytes() > evidence_max_grid_bytes) {
        return fail<Rows>(diagnostic_codes::evidence_limit,
            "fog evidence would hash more than 1 GiB of canonical grid bytes", sidecar_logical_path);
    }
    auto world = World::create(replay.value());
    if (!world) {
        return core::Result<Rows>::failure(world.error());
    }
    Rows rows;
    rows.reserve(static_cast<std::size_t>(replay.value().final_tick_count) + 1U);
    const auto emit = [&](const std::uint64_t tick, const std::string& state_hex) -> core::Result<void> {
        const auto state = decode_sha256_hex(state_hex);
        if (!state) {
            return fail<void>(diagnostic_codes::invalid_value, "world state hash is not 64 lowercase hex", {});
        }
        const auto active = timeline.value().active_at(tick);
        if (!active) {
            return core::Result<void>::failure(active.error());
        }
        rows.push_back(FogEvidenceRow{
            tick,
            sim::sha256(fog_evidence_preimage(replay_sha, sidecar_sha, tick, *state, active.value())),
        });
        return core::Result<void>::success();
    };
    auto emitted = emit(0, world.value().state_sha256());
    if (!emitted) {
        return core::Result<Rows>::failure(emitted.error());
    }
    for (std::uint64_t tick = 1; tick <= replay.value().final_tick_count; ++tick) {
        const auto step = world.value().step(executor);
        if (!step) {
            return core::Result<Rows>::failure(step.error());
        }
        if (step.value().completed_tick != tick) {
            return fail<Rows>(diagnostic_codes::order, "world completed an unexpected tick", {});
        }
        emitted = emit(tick, step.value().state_sha256);
        if (!emitted) {
            return core::Result<Rows>::failure(emitted.error());
        }
    }
    return core::Result<Rows>::success(std::move(rows));
}

std::string format_fog_evidence_csv(const std::span<const FogEvidenceRow> rows) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string output = "tick,sha256\n";
    for (const auto& row : rows) {
        output += std::to_string(row.completed_tick);
        output += ',';
        for (const auto byte : row.sha256) {
            output += digits[byte >> 4U];
            output += digits[byte & 0x0FU];
        }
        output += '\n';
    }
    return output;
}

} // namespace eawr::sim::fog
