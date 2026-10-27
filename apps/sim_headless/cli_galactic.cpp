#include "cli_galactic.hpp"

#include "cli_internal.hpp"
#include "cli_galactic_install.hpp"
#include "json.hpp"

#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/synthetic.hpp"
#include "eawr/sim/galactic/replay.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace sim_headless::cli {

namespace {

namespace galactic = eawr::sim::galactic;

constexpr std::string_view usage =
    "expected --galactic-replay <file> --hash-out <file.csv> [--workers <n|hardware>], or "
    "--galactic <campaign.json> [--game-root <install>] [--ticks <n>] [--workers <n|hardware>] "
    "with at least one of --hash-out <file.csv>, --replay-out <file>, --census-out <file.json>, or "
    "--galactic <installed campaign name> --game-root <install> [--mod-root <leaf;parent;...>] [--ticks <n>] [--play <n>] "
    "[--live <n> --live-ai on [--live-goals <n>] [--live-perception <equation>]] "
    "[--workers <n|hardware>] [--census-out <report.json>] (staged start from installed data)";

struct GalacticOptions {
    std::string replay_path;
    std::string campaign_path;
    std::string game_root;
    std::string mod_root;
    std::string hash_path;
    std::string replay_out_path;
    std::string census_path;
    std::uint64_t ticks{};
    std::uint64_t play_ticks{};
    std::uint64_t live_ticks{};
    bool live_ai{};
    bool live_ai_given{};
    std::string perception_trace;
    std::uint64_t goals_every{}; // --live-goals: print the native AI's goals every n ticks (0 = off)
    std::size_t workers{1};
};

[[nodiscard]] std::optional<GalacticOptions> parse(const int argc, const char* const argv[]) {
    GalacticOptions options;
    std::vector<std::string_view> seen;
    bool ticks = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (std::find(seen.begin(), seen.end(), argument) != seen.end() || index + 1 >= argc) return std::nullopt;
        seen.push_back(argument);
        const std::string value(argv[++index]);
        if (value.empty()) return std::nullopt;
        if (argument == "--galactic-replay") {
            options.replay_path = value;
        } else if (argument == "--galactic") {
            options.campaign_path = value;
        } else if (argument == "--game-root") {
            options.game_root = value;
        } else if (argument == "--mod-root") {
            options.mod_root = value;
        } else if (argument == "--hash-out") {
            options.hash_path = value;
        } else if (argument == "--replay-out") {
            options.replay_out_path = value;
        } else if (argument == "--census-out") {
            options.census_path = value;
            if (std::filesystem::path(value).extension() != ".json") return std::nullopt;
        } else if (argument == "--ticks") {
            ticks = true;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), options.ticks);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || options.ticks > galactic::galactic_max_ticks) {
                return std::nullopt;
            }
        } else if (argument == "--play") {
            const auto result = std::from_chars(value.data(), value.data() + value.size(), options.play_ticks);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || options.play_ticks > galactic::galactic_max_ticks) {
                return std::nullopt;
            }
        } else if (argument == "--live-ai") {
            if (value != "on" && value != "off") return std::nullopt;
            options.live_ai = value == "on";
            options.live_ai_given = true;
        } else if (argument == "--live-perception") {
            options.perception_trace = value;
        } else if (argument == "--live-goals") {
            const auto result = std::from_chars(value.data(), value.data() + value.size(), options.goals_every);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || options.goals_every == 0
                || options.goals_every > galactic::galactic_max_ticks) {
                return std::nullopt;
            }
        } else if (argument == "--live") {
            const auto result = std::from_chars(value.data(), value.data() + value.size(), options.live_ticks);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || options.live_ticks > galactic::galactic_max_ticks) {
                return std::nullopt;
            }
        } else if (argument == "--workers") {
            if (value == "hardware") {
                options.workers = eawr::platform::ThreadWorkerAdapter::hardware_worker_count();
            } else {
                const auto result = std::from_chars(value.data(), value.data() + value.size(), options.workers);
                if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || options.workers == 0
                    || options.workers > eawr::platform::ThreadWorkerAdapter::max_worker_count) {
                    return std::nullopt;
                }
            }
        } else {
            return std::nullopt;
        }
    }
    if (options.replay_path.empty() == options.campaign_path.empty()) return std::nullopt;
    // --play, --live and --live-ai belong to an installed campaign by name; elsewhere they would be accepted and ignored.
    const bool installed_only = options.play_ticks || options.live_ticks || options.live_ai_given || options.goals_every || !options.perception_trace.empty();
    if (!options.replay_path.empty()) {
        if (installed_only) return std::nullopt;
        if (options.hash_path.empty() || ticks || !options.game_root.empty() || !options.replay_out_path.empty()
            || !options.census_path.empty()) {
            return std::nullopt;
        }
        return options;
    }
    if (std::filesystem::path(options.campaign_path).extension() != ".json") {
        // An installed campaign by name: the staged report is its only output.
        if (options.game_root.empty() || !options.hash_path.empty() || !options.replay_out_path.empty()) return std::nullopt;
        // --live returns before the gameplay stage, and --live-ai only configures --live.
        if (options.live_ticks && options.play_ticks) return std::nullopt;
        if (options.live_ai_given && !options.live_ticks) return std::nullopt;
        // The goal lines read the native AI, so they need the live run with the AI on.
        if ((options.goals_every || !options.perception_trace.empty()) && !(options.live_ticks && options.live_ai)) return std::nullopt;
        return options;
    }
    if (!options.mod_root.empty() || installed_only) return std::nullopt;
    if (options.hash_path.empty() && options.replay_out_path.empty() && options.census_path.empty()) return std::nullopt;
    return options;
}

[[nodiscard]] int report(const eawr::core::Diagnostic& diagnostic) {
    std::cerr << eawr::core::format_diagnostic(diagnostic) << '\n';
    return 3;
}

[[nodiscard]] std::optional<std::string> read_file(const std::string& path, std::string& error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "could not open the input";
        return std::nullopt;
    }
    const auto size = static_cast<std::streamoff>(input.tellg());
    if (size < 0 || static_cast<std::uint64_t>(size) > galactic::galactic_replay_max_bytes) {
        error = "the input exceeds the 256 MiB limit";
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!bytes.empty() && !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        error = "could not read the complete input";
        return std::nullopt;
    }
    return bytes;
}

template <typename Integer>
[[nodiscard]] bool read_integer(const Json& campaign, const std::string_view key, Integer& out) {
    const auto* value = campaign.get(key);
    if (value == nullptr) return true;
    const auto* digits = value->digits();
    if (digits == nullptr) return false;
    const auto result = std::from_chars(digits->data(), digits->data() + digits->size(), out);
    return result.ec == std::errc{} && result.ptr == digits->data() + digits->size();
}

[[nodiscard]] bool read_flag(const Json& campaign, const std::string_view key, bool& out) {
    const auto* value = campaign.get(key);
    if (value == nullptr) return true;
    if (value->kind != Json::Kind::boolean) return false;
    out = value->flag;
    return true;
}

// A synthetic campaign definition: {"name": "...", "seed", "planets", "players",
// "fleets_per_player", "members_per_fleet": integers, "coop_seat", "debug_commands": booleans,
// "fiscal_cycle_seconds": decimal}. Unknown keys are refused.
[[nodiscard]] eawr::core::Result<eawr::campaign::SyntheticCampaign> read_campaign(const std::string& text, const std::string& path) {
    using Out = eawr::core::Result<eawr::campaign::SyntheticCampaign>;
    const auto refuse = [&](const std::string& message) {
        eawr::core::Diagnostic diagnostic;
        diagnostic.code = std::string(galactic::diagnostic_codes::invalid_setup);
        diagnostic.message = message;
        diagnostic.logical_path = path;
        return Out::failure(std::move(diagnostic));
    };
    const auto document = JsonReader(text).document();
    if (!document || document->kind != Json::Kind::object) return refuse("a synthetic campaign is one JSON object");
    constexpr std::string_view known[] = {"name", "seed", "planets", "players", "fleets_per_player", "members_per_fleet",
        "coop_seat", "debug_commands", "fiscal_cycle_seconds"};
    auto keys = document->keys;
    std::sort(keys.begin(), keys.end());
    if (std::adjacent_find(keys.begin(), keys.end()) != keys.end()) return refuse("duplicate campaign key");
    for (const auto& key : document->keys) {
        if (std::find(std::begin(known), std::end(known), key) == std::end(known)) return refuse("unknown key \"" + key + "\"");
    }
    eawr::campaign::SyntheticCampaign campaign;
    const auto* name = document->get("name");
    if (name == nullptr || name->text() == nullptr || name->text()->empty()) return refuse("\"name\" must be a non-empty string");
    campaign.name = *name->text();
    auto& galaxy = campaign.galaxy;
    galaxy.debug_commands = false;
    if (!read_integer(*document, "seed", galaxy.seed) || !read_integer(*document, "planets", galaxy.planets)
        || !read_integer(*document, "players", galaxy.players) || !read_integer(*document, "fleets_per_player", galaxy.fleets_per_player)
        || !read_integer(*document, "members_per_fleet", galaxy.members_per_fleet)
        || !read_flag(*document, "coop_seat", galaxy.coop_seat) || !read_flag(*document, "debug_commands", galaxy.debug_commands)) {
        return refuse("a count must be a non-negative integer and a flag a boolean");
    }
    if (galaxy.planets < 2 || galaxy.planets > 10'000 || galaxy.players < 1 || galaxy.players > galactic::galactic_max_players
        || galaxy.fleets_per_player > 1'000 || galaxy.members_per_fleet < 1 || galaxy.members_per_fleet > 1'000) {
        return refuse("planets 2..10000, players 1..64, fleets per player 0..1000, members per fleet 1..1000");
    }
    if (const auto* seconds = document->get("fiscal_cycle_seconds")) {
        const auto* digits = seconds->digits();
        const auto value = digits != nullptr ? eawr::sim::math::Fixed::from_decimal(*digits)
                                             : eawr::core::Result<eawr::sim::math::Fixed>::failure({});
        if (!value) return refuse("\"fiscal_cycle_seconds\" must be a decimal number");
        campaign.fiscal_cycle_seconds = value.value();
    }
    return Out::success(std::move(campaign));
}

[[nodiscard]] std::string hex(const std::array<std::uint8_t, 32>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

[[nodiscard]] std::string census(const std::string& name, const galactic::GalacticSetup& setup, const std::uint64_t ticks,
    const std::string& tick_zero_sha256, const std::string& final_sha256) {
    std::size_t members = 0;
    for (const auto& fleet : setup.fleets) members += fleet.members.size();
    std::ostringstream json;
    std::string escaped_name;
    constexpr char digits[] = "0123456789abcdef";
    for (const auto character : name) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') {
            escaped_name.push_back('\\');
            escaped_name.push_back(character);
        } else if (byte < 0x20U) {
            escaped_name += "\\u00";
            escaped_name.push_back(digits[byte >> 4U]);
            escaped_name.push_back(digits[byte & 0x0fU]);
        } else {
            escaped_name.push_back(character);
        }
    }
    json << "{\n  \"campaign\": \"" << escaped_name << "\",\n  \"campaign_identity\": \"" << hex(setup.campaign_identity)
         << "\",\n  \"content_identity\": \"" << hex(setup.content_identity) << "\",\n  \"seed\": " << setup.seed
         << ",\n  \"players\": " << setup.players.size() << ",\n  \"seats\": " << setup.seats.size()
         << ",\n  \"planets\": " << setup.planets.size() << ",\n  \"routes\": " << setup.routes.size()
         << ",\n  \"fleets\": " << setup.fleets.size() << ",\n  \"fleet_members\": " << members
         << ",\n  \"fiscal_cycle_frames\": " << setup.rules.fiscal_cycle_frames << ",\n  \"ticks\": " << ticks
         << ",\n  \"tick_zero_sha256\": \"" << tick_zero_sha256 << "\",\n  \"final_sha256\": \"" << final_sha256 << "\"\n}\n";
    return json.str();
}

// Steps the session to `final_tick`, one hash row per completed tick.
[[nodiscard]] std::optional<std::string> run_ticks(
    eawr::campaign::GameSession& session, const std::uint64_t final_tick, const std::size_t workers) {
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    std::ostringstream csv;
    csv << "tick,sha256\n";
    while (session.completed_tick() < final_tick) {
        const auto tick = session.step(executor);
        if (!tick) {
            static_cast<void>(report(tick.error()));
            return std::nullopt;
        }
        csv << tick.value().completed_tick << ',' << tick.value().state_hash.get() << '\n';
    }
    return csv.str();
}

[[nodiscard]] bool distinct(const GalacticOptions& galactic_options) {
    Options options;
    options.replay_path = galactic_options.replay_path.empty() ? galactic_options.campaign_path : galactic_options.replay_path;
    options.hash_path = galactic_options.hash_path;
    options.replay_out_path = galactic_options.replay_out_path;
    options.census_path = galactic_options.census_path;
    return paths_are_distinct(options);
}

} // namespace

bool is_galactic_invocation(const int argc, const char* const argv[]) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--galactic" || argument == "--galactic-replay") return true;
    }
    return false;
}

void print_galactic_help(std::ostream& output) { output << "galactic runs: " << usage << '\n'; }

int run_galactic(const int argc, const char* const argv[]) {
    const auto options = parse(argc, argv);
    if (!options) return report_argument_error(usage);
    if (!distinct(*options)) return report_argument_error("the input and every output must name different files");

    std::vector<eawr::platform::PublishedFile> outputs;
    if (!options->replay_path.empty()) {
        std::string error;
        const auto bytes = read_file(options->replay_path, error);
        if (!bytes) return report_io_error(options->replay_path, error, 3);
        const std::vector<std::uint8_t> data(bytes->begin(), bytes->end());
        const auto replay = galactic::parse_replay(data, options->replay_path);
        if (!replay) return report(replay.error());
        auto session = eawr::campaign::GameSession::from_replay(replay.value());
        if (!session) return report(session.error());
        const auto csv = run_ticks(session.value(), replay.value().final_tick_count, options->workers);
        if (!csv) return 3;
        outputs.push_back({"hash output", options->hash_path, *csv});
        return publish(outputs);
    }

    std::string error;
    if (std::filesystem::path(options->campaign_path).extension() != ".json") {
        InstalledCampaignRequest request;
        request.campaign = options->campaign_path;
        request.game_root = options->game_root;
        request.mod_root = options->mod_root;
        request.ticks = options->ticks;
        request.play_ticks = options->play_ticks;
        request.live_ticks = options->live_ticks;
        request.live_ai = options->live_ai;
        request.goals_every = options->goals_every;
        request.perception_trace = options->perception_trace;
        request.workers = options->workers;
        request.report_path = options->census_path;
        return run_installed_campaign(request);
    }
    if (!options->game_root.empty() && !std::filesystem::is_directory(options->game_root)) {
        return report_io_error(options->game_root, "--game-root is not a directory", 3);
    }
    const auto text = read_file(options->campaign_path, error);
    if (!text) return report_io_error(options->campaign_path, error, 3);
    const auto campaign = read_campaign(*text, options->campaign_path);
    if (!campaign) return report(campaign.error());
    const auto setup = eawr::campaign::build_synthetic_campaign(campaign.value());
    if (!setup) return report(setup.error());
    auto session = eawr::campaign::GameSession::create(setup.value());
    if (!session) return report(session.error());
    const auto tick_zero = session.value().state_sha256();
    const auto csv = run_ticks(session.value(), options->ticks, options->workers);
    if (!csv) return 3;
    if (!options->hash_path.empty()) outputs.push_back({"hash output", options->hash_path, *csv});
    if (!options->replay_out_path.empty()) {
        const auto written = galactic::write_replay(session.value().record());
        if (!written) return report(written.error());
        outputs.push_back({"replay output", options->replay_out_path, std::string(written.value().begin(), written.value().end())});
    }
    if (!options->census_path.empty()) {
        outputs.push_back({"census output", options->census_path,
            census(campaign.value().name, setup.value(), options->ticks, tick_zero, session.value().state_sha256())});
    }
    return publish(outputs);
}

} // namespace sim_headless::cli
