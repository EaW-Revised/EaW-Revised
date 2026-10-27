#pragma once

#include "../support/result.hpp"
#include "../support/check.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/campaign/setup.hpp"
#include "../../src/sim/galactic/checkpoint_fields_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace galactic_history_diff {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
using eawr::testing::result_code_space::take;
using Bytes = std::vector<std::uint8_t>;

// Compare the complete saved owner after removing precisely its History field.
// This never changes either live world or any synchronization hash.
inline Bytes without_history(const cp::GameSession& game) {
    auto owner = take(game.save_checkpoint_state());
    gc::checkpoint_detail::Reader in(owner);
    std::uint32_t version{}; Bytes world; in(version, world);
    gc::checkpoint_detail::Writer history; history(game.galaxy().history_archive());
    const auto at = std::search(world.begin(), world.end(), history.bytes.begin(), history.bytes.end());
    eawr::testing::require(at != world.end(), "saved owner contains its exact History field");
    const auto end = at + static_cast<std::ptrdiff_t>(history.bytes.size());
    eawr::testing::require(std::search(end, world.end(), history.bytes.begin(), history.bytes.end()) == world.end(),
        "History field has an unambiguous saved-owner location");
    world.erase(at, end);
    gc::checkpoint_detail::Writer normalized; normalized(version, world);
    normalized.bytes.insert(normalized.bytes.end(), owner.end() - static_cast<std::ptrdiff_t>(in.remaining()), owner.end());
    return normalized.bytes;
}
inline Bytes scripts_without_world_digest(const cp::GameSession& game) {
    const auto saved = take(game.save_galactic_scripts());
    eawr::testing::require(saved.has_value(), "History fixture has hosted scripts");
    auto checkpoint = take(eawr::checkpoint::parse_checkpoint(take(eawr::checkpoint::save_game_checkpoint(
        game, eawr::checkpoint::game_identities(game.galaxy().setup())))));
    const auto section = std::find_if(checkpoint.sections.begin(), checkpoint.sections.end(),
        [](const auto& row) { return row.id == static_cast<std::uint16_t>(eawr::checkpoint::SectionId::scripts); });
    eawr::testing::require(section != checkpoint.sections.end(), "History fixture saves its script section");
    auto bytes = std::move(section->bytes);
    const auto& digest = saved->world_sha256;
    const auto at = std::search(bytes.begin(), bytes.end(), digest.begin(), digest.end());
    eawr::testing::require(!digest.empty() && at != bytes.end(), "script section contains its world digest");
    const auto end = at + static_cast<std::ptrdiff_t>(digest.size());
    eawr::testing::require(std::search(end, bytes.end(), digest.begin(), digest.end()) == bytes.end(),
        "script world digest has an unambiguous saved location");
    bytes.erase(at, end);
    return bytes;
}
inline void equal_except_history(cp::GameSession& left, cp::GameSession& right) {
    const auto a = without_history(left), b = without_history(right);
    if (a != b) {
        const auto first = std::mismatch(a.begin(), a.end(), b.begin(), b.end());
        std::cerr << "History owner mismatch normalized_bytes=" << a.size() << '/' << b.size()
            << " first_offset=" << std::distance(a.begin(), first.first) << '\n';
    }
    eawr::testing::require(a == b,
        "complete saved owners differ only in History rows");
    eawr::testing::require(take(left.save_galactic_ai()).state->components == take(right.save_galactic_ai()).state->components,
        "History service ownership does not change any native component");
    eawr::testing::require(scripts_without_world_digest(left) == scripts_without_world_digest(right),
        "script payloads differ only in the digest of their History-bearing world");
}
template<class Plan, class Services, class Configuration, class Executor>
inline void verify(const Plan& plan, const Services& services, const Configuration& configuration,
    const Executor& executor, const std::filesystem::path& directory) {
    auto baseline = take(cp::CampaignRoot::create(plan, services));
    auto corrected = take(cp::CampaignRoot::create(plan, services));
    for (auto* game : {baseline.game(), corrected.game()}) {
        take(configuration.configure(*game));
        take(configuration.configure_native_ai(*game));
    }
    // Reproduce the old fresh-install ownership exactly, after adopting the world.
    baseline.game()->galactic_scripts()->parent_world().defer_history_service(false);
    const auto identities = eawr::checkpoint::game_identities(baseline.game()->galaxy().setup());
    take(baseline.game()->step(executor)); take(corrected.game()->step(executor));
    const auto boundary = take(baseline.game()->history_frontier());
    eawr::testing::require(boundary == take(corrected.game()->history_frontier()),
        "History sampling does not change the sealed input frontier");
    auto restored = take(eawr::checkpoint::load_game_checkpoint(
        take(eawr::checkpoint::save_game_checkpoint(*baseline.game(), identities)), identities, configuration.restore, executor));
    // Match the loader's ordinary input-journal compaction on both controls.
    // This leaves chart History and every gameplay/hash field untouched.
    baseline.game()->schedule_history_rebase(boundary);
    corrected.game()->schedule_history_rebase(boundary);
    take(baseline.game()->step(executor)); take(corrected.game()->step(executor)); take(restored.session.step(executor));
    equal_except_history(*baseline.game(), *corrected.game());
    equal_except_history(*baseline.game(), restored.session);
    std::ofstream output(directory / "history-owner-diff.tsv");
    output << "owner\ttick\tplayer\tindex\tframe\teconomy_bits\tplanets\ttechnology\tmilitary_bits\n";
    std::size_t fresh_difference{}, restored_difference{};
    for (const auto& [player, rows] : baseline.game()->galaxy().history_archive()) {
        const auto& fixed = corrected.game()->galaxy().history_archive().at(player);
        const auto& loaded = restored.session.galaxy().history_archive().at(player);
        eawr::testing::require(rows.size() == fixed.size() + 2 && rows.size() == loaded.size() + 1,
            "two fresh turns and one resumed turn remove exactly their duplicate History rows");
        fresh_difference += rows.size() - fixed.size(); restored_difference += rows.size() - loaded.size();
    }
    for (const auto& [name, game] : {std::pair{"base-fresh", baseline.game()},
            std::pair{"head-fresh", corrected.game()}, std::pair{"base-restored", &restored.session}})
        for (const auto& [player, rows] : game->galaxy().history_archive())
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const auto& row = rows[i];
                output << name << '\t' << game->completed_tick() << '\t' << player << '\t' << i << '\t'
                    << row.frame << '\t' << row.economy.bits << '\t' << row.planets << '\t'
                    << row.technology << '\t' << row.military.bits << '\n';
            }
    output.close(); eawr::testing::require(static_cast<bool>(output), "History owner diff receipt is durable");
    std::cout << "History owner diff tick=2 fresh_duplicate_rows=" << fresh_difference
        << " restore_duplicate_rows=" << restored_difference
        << " other_owner_fields_equal=true native_components_equal=true script_payloads_equal=true\n";
}
} // namespace galactic_history_diff
