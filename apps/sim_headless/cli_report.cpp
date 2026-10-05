#include "cli_internal.hpp"

namespace sim_headless::cli {

void print_help(std::ostream& output) {
    output << "Usage: sim_headless --replay <file> --hash-out <file> [--workers <n|hardware>]\n"
              "                    [--events-out <file>] [--snapshot-out <file>]\n"
              "                    [--census-out <file.json>] [--trace-out <file.csv>]\n"
              "                    [--game-root <install> [--mod-root <leaf;parent;...>]]\n"
              "       sim_headless --skirmish m2 --game-root <install> [--mod-root <leaf;parent;...>] [--census-out <file.json>]\n"
              "                    [--replay-out <file>] [--ticks <n>] [--tag-trace-out <file.json>]\n"
              "       sim_headless --scenario <S-NN.json> --game-root <install> --trace-out <file.csv>\n"
              "                    [--hash-out <file>] [--replay-out <file>] [--combat-out <file.csv>]\n"
              "                    [--workers <1|2|4>]\n"
              "\n"
              "Runs replay-v1 or replay-v2 without a renderer and writes UTF-8 tick,state-hash CSV.\n"
              "A replay-v2 run can also write its event stream, per-tick snapshot digests and the\n"
              "tick-zero census of its setup. With --game-root it runs with the sensor, durability\n"
              "and motion tables of that installation's unit tables, which must be the content its\n"
              "setup names (a viewer live session records such replays). A replay-v1 run can also\n"
              "write a docs/traces.md trace\n"
              "of entity positions, with its JSON header beside it (same name, .json).\n"
              "--workers sizes the simulation worker pool: 1 to 256 threads, or hardware for\n"
              "one per hardware thread (default 1). No output depends on it.\n"
              "--skirmish m2 builds tick zero of the pinned FoC skirmish from the installation\n"
              "(docs/skirmish-start.md), lists it, and writes its census and a replay-v2 file that\n"
              "holds the setup alone and <n> ticks (default 0). --tag-trace-out records the XML its\n"
              "loaders read (docs/tag-coverage.md).\n"
              "--scenario stages a tests/fidelity scenario on the FoC unit tables and writes the remake's\n"
              "trace of it (docs/traces.md), its tick hashes and its replay. --combat-out adds its combat log:\n"
              "every shot, projectile hit and change of hull, shield and hardpoint health (docs/traces.md).\n"
              "All outputs are published or none.\n";
}

int report_argument_error(const std::string_view message) {
    const eawr::core::Diagnostic diagnostic{
        .code = std::string(eawr::core::diagnostic_codes::invalid_argument),
        .severity = eawr::core::Severity::error,
        .message = std::string(message),
        .logical_path = std::nullopt,
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("sim_headless"),
    };
    std::cerr << eawr::core::format_diagnostic(diagnostic) << '\n';
    return 2;
}

int report_io_error(const std::string_view path, const std::string_view message, const int code) {
    const eawr::core::Diagnostic diagnostic{
        .code = "EAWR-SIM-CLI-0001",
        .severity = eawr::core::Severity::error,
        .message = std::string(message),
        .logical_path = std::string(path),
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("sim_headless"),
    };
    std::cerr << eawr::core::format_diagnostic(diagnostic) << '\n';
    return code;
}

// Files a failed publication or its clean-up left behind; the path names each one.
int report_leftover(const std::string_view path, const std::string_view message) {
    const eawr::core::Diagnostic diagnostic{
        .code = "EAWR-SIM-CLI-0002",
        .severity = eawr::core::Severity::error,
        .message = std::string(message),
        .logical_path = std::string(path),
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("sim_headless"),
    };
    std::cerr << eawr::core::format_diagnostic(diagnostic) << '\n';
    return 5;
}

[[nodiscard]] std::vector<TraceObject> trace_objects(const eawr::sim::Replay& replay) {
    std::vector<TraceObject> objects;
    const auto add = [&objects](const eawr::sim::EntityId id) {
        objects.push_back({"entity." + std::to_string(id), id});
    };
    for (const auto& entity : replay.initial_entities) {
        add(entity.entity_id);
    }
    for (const auto& command : replay.commands) {
        if (const auto* create = std::get_if<eawr::sim::CreateCommand>(&command.payload)) {
            add(create->entity.entity_id);
        }
    }
    const auto by_label = [](const TraceObject& left, const TraceObject& right) {
        return left.label < right.label;
    };
    const auto same_label = [](const TraceObject& left, const TraceObject& right) {
        return left.label == right.label;
    };
    std::sort(objects.begin(), objects.end(), by_label);
    objects.erase(std::unique(objects.begin(), objects.end(), same_label), objects.end());
    return objects;
}

// Rows for one completed tick, sorted by object label, then field name.
void append_trace_rows(
    std::ostringstream& trace,
    const std::uint64_t tick,
    const std::vector<TraceObject>& objects,
    const std::vector<eawr::sim::EntityState>& entities) {
    for (const auto& object : objects) {
        const auto found = std::lower_bound(
            entities.begin(),
            entities.end(),
            object.entity_id,
            [](const eawr::sim::EntityState& entity, const eawr::sim::EntityId id) {
                return entity.entity_id < id;
            });
        const bool alive = found != entities.end() && found->entity_id == object.entity_id;
        trace << tick << ',' << object.label << ",alive," << (alive ? 1 : 0) << '\n';
        if (alive) {
            trace << tick << ',' << object.label << ",pos.x," << found->position.x.raw() << '\n'
                  << tick << ',' << object.label << ",pos.y," << found->position.y.raw() << '\n'
                  << tick << ',' << object.label << ",pos.z," << found->position.z.raw() << '\n';
        }
    }
}

[[nodiscard]] std::string lower_hex(const std::array<std::uint8_t, 32>& bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text;
    for (const auto byte : bytes) {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 0x0fU]);
    }
    return text;
}

[[nodiscard]] std::string trace_header(
    const eawr::sim::Replay& replay,
    const std::string_view replay_sha256,
    const std::string_view build_sha256) {
    std::ostringstream json;
    json << "{\n"
         << "  \"format\": \"eawr-trace\",\n"
         << "  \"format_version\": 1,\n"
         << "  \"source\": \"remake\",\n"
         << "  \"content_identity\": \"" << lower_hex(replay.content_identity) << "\",\n"
         << "  \"tick_seconds\": {\"numerator\": " << replay.tick_numerator
         << ", \"denominator\": " << replay.tick_denominator << "},\n"
         << "  \"build_identity\": {\"kind\": \"executable-sha256\", \"value\": \"" << build_sha256
         << "\"},\n"
         << "  \"scenario_sha256\": null,\n"
         << "  \"replay_sha256\": \"" << replay_sha256 << "\"\n"
         << "}\n";
    return json.str();
}

// Exit 0 when every output was published, 4 when none was, 5 when files are left behind.
int publish(const std::vector<eawr::platform::PublishedFile>& outputs) {
    const auto report = eawr::platform::publish_files(outputs);
    int code = 0;
    if (report.failure) {
        code = report_io_error(report.failure->path, report.failure->message, 4);
    }
    for (const auto& leftover : report.leftovers) {
        code = report_leftover(leftover.path, leftover.message);
    }
    return code;
}

// The unit tables of an installation, mounted read-only as --skirmish m2 mounts it: the mod
// chain, then FoC over base EaW.
void list_start(std::ostream& output, const eawr::skirmish::SkirmishStart& start, const std::string& state_sha256) {
    const auto whole = [](const eawr::sim::math::Fixed value) {
        return std::to_string(value.nearest_even_to_integer());
    };
    output << "map " << start.map << " sha256 " << start.map_sha256 << '\n';
    for (const auto& player : start.players) {
        output << "player " << player.player.player_id << ' ' << player.faction << " team " << player.player.team_id;
        if (player.lobby) {
            output << (player.human ? " human" : " ai") << " start " << player.start_side;
            if (player.colour) {
                output << " colour " << player.colour->constant << " (" << static_cast<int>(player.colour->rgb[0])
                       << ", " << static_cast<int>(player.colour->rgb[1]) << ", "
                       << static_cast<int>(player.colour->rgb[2]) << ')';
            }
            // SK-30, SK-31 (#530): the starting credits and whether the station earns and builds.
            output << " credits " << player.credits << " income " << (player.income ? "station" : "none")
                   << " production " << (player.production_queue ? "station" : "none") << " power "
                   << whole(player.combat_power_tick_zero);
        } else if (player.owner_index) {
            output << " non-playable (TED index " << *player.owner_index << ')';
        }
        output << '\n';
    }
    for (const auto& unit : start.units) {
        output << "unit " << unit.state.entity_id << ' ' << unit.type << " owner " << unit.state.owner << ' '
               << eawr::skirmish::to_string(unit.role) << " record " << unit.record << " at ("
               << whole(unit.state.position.x) << ", " << whole(unit.state.position.y) << ", "
               << whole(unit.state.position.z) << ") yaw " << whole(unit.yaw_degrees);
        if (unit.craft != 0) output << " craft " << unit.craft << " x " << unit.craft_type;
        output << '\n';
    }
    for (const auto& removed : start.removed) {
        output << "removed " << removed.type << " record " << removed.record << " (" << removed.faction << ", "
               << eawr::skirmish::to_string(removed.reason) << ")\n";
    }
    for (const auto& launch : start.launches) {
        output << "launch (not simulated) " << launch.count << " x " << launch.squadron << " from unit "
               << launch.spawner << ' ' << launch.spawner_type << '\n';
    }
    output << "tick 0 state " << state_sha256 << '\n';
}

// --scenario: the remake's trace of a tests/fidelity scenario (#70).

} // namespace sim_headless::cli
