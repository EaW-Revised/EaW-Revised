#pragma once

#include "eawr/net/galactic_checkpoint_transfer.hpp"
#include "eawr/mods/save_description.hpp"
#include "eawr/sim/galactic/mp_roster.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace eawr::viewer {
inline std::string mod_mismatch_message(const mods::ModDescription& host, const mods::ModDescription& local) {
    if (!mods::description_needs_confirmation(host, local)) return {};
    std::string result = "Your mods differ from the host's. Both players need the same mods and load order.";
    for (const auto& finding : mods::compare_descriptions(host, local)) {
        using Kind = mods::FindingKind;
        if (finding.kind == Kind::missing || finding.kind == Kind::extra || finding.kind == Kind::content_changed)
            result += "\n" + mods::describe_finding(finding);
        if (finding.kind == Kind::version_changed) {
            const auto& name = finding.mod.name.empty() ? finding.mod.key : finding.mod.name;
            result += "\n" + name + ": host version " + finding.saved_detail + ", your version " + finding.current_detail + ".";
        }
        if (finding.kind == Kind::reordered) {
            const auto names = [](const mods::ModDescription& description) {
                std::string line;
                for (const auto& entry : description.entries) {
                    if (!line.empty()) line += ", ";
                    line += entry.name.empty() ? (entry.key.empty() ? "Unknown mod" : entry.key) : entry.name;
                }
                return line;
            };
            result += "\nHost order: " + names(host) + "\nYour order: " + names(local);
        }
    }
    return result;
}
inline bool mod_comparison_incomplete(const mods::ModDescription& host, const mods::ModDescription& local) {
    const auto findings = mods::compare_descriptions(host, local);
    return std::any_of(findings.begin(), findings.end(), [](const auto& finding) {
        return finding.kind == mods::FindingKind::incomplete || finding.kind == mods::FindingKind::unknown_identity
            || finding.kind == mods::FindingKind::checksum_unavailable;
    });
}
// Work owns its inputs and never calls the viewer; abandoning the future does not join it.
template<class Work> auto checkpoint_work(Work work) {
    using Value = decltype(work());
    std::packaged_task<Value()> task(std::move(work));
    auto result = task.get_future();
    std::thread(std::move(task)).detach();
    return result;
}
enum class CheckpointRestoreState { idle, pending, ready, timed_out, stale, room_changed, failed };
template<class Value, class Room> class CheckpointRestoreJob {
public:
    using Clock = std::chrono::steady_clock;
    struct Poll { CheckpointRestoreState state; std::optional<Value> value; };
    [[nodiscard]] bool active() const { return work_.valid(); }
    template<class Work> bool submit(std::uint64_t request, Room room, Clock::time_point deadline, Work work) {
        if (active()) return false;
        request_ = request; room_ = std::move(room); deadline_ = deadline; timed_out_ = false;
        work_ = checkpoint_work(std::move(work));
        return true;
    }
    Poll poll(std::uint64_t request, const Room* room, Clock::time_point now) {
        if (!active()) return {CheckpointRestoreState::idle, {}};
        const bool current = request == request_ && !timed_out_;
        if (work_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            if (current && now >= deadline_) { timed_out_ = true; return {CheckpointRestoreState::timed_out, {}}; }
            return {CheckpointRestoreState::pending, {}};
        }
        try {
            auto value = work_.get();
            if (!current || !room || *room != *room_) {
                (void)checkpoint_work([discard = std::move(value)] { static_cast<void>(discard); });
                return {current ? CheckpointRestoreState::room_changed : CheckpointRestoreState::stale, {}};
            }
            return {CheckpointRestoreState::ready, std::move(value)};
        } catch (...) {
            return {current && room && *room == *room_ ? CheckpointRestoreState::failed : CheckpointRestoreState::stale, {}};
        }
    }
private:
    std::future<Value> work_;
    std::optional<Room> room_;
    Clock::time_point deadline_{};
    std::uint64_t request_{};
    bool timed_out_{};
};
// The saved-game listing reads and checks every save on a worker; the lobby gives up on it after this long (#5512).
inline constexpr std::chrono::seconds checkpoint_listing_limit{30};
inline bool checkpoint_listing_expired(std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point now,
                                       std::chrono::milliseconds limit = checkpoint_listing_limit) {
    return now - started >= limit;
}
inline std::string checkpoint_staging_title(const std::string& title, const std::string& status) {
    return status.empty() ? title : status;
}
struct CheckpointLobbyModel {
    bool can_assign{}, can_accept{};
    std::string progress, refusal;
    std::vector<std::string> rows;
};
inline std::vector<sim::galactic::Seat> checkpoint_humans(const sim::galactic::MpRoster& roster) {
    std::vector<sim::galactic::Seat> result;
    for (const auto& seat : roster.seats)
        if (seat.kind == sim::SeatKind::human && seat.active) result.push_back(seat);
    return result;
}
inline CheckpointLobbyModel checkpoint_lobby_model(bool host, bool staging,
    const sim::galactic::MpRoster& roster, std::uint64_t tick,
    const std::vector<net::SetupSeat>& bindings, const std::vector<net::PeerIndex>& peers,
    net::PeerIndex local, const net::SaveTransferView& transfer,
    const std::string& description, const std::string& date, const std::string& problem) {
    CheckpointLobbyModel result;
    result.can_assign = host && staging;
    result.can_accept = staging && transfer.agreed && problem.empty() && !transfer.refusal;
    result.progress = transfer.agreed ? "Save restored by every player" : transfer.restored ? "Waiting for restore agreement"
        : "Transfer: " + std::to_string(transfer.received) + " / " + std::to_string(transfer.total) + " bytes";
    result.refusal = !problem.empty() ? problem : transfer.refusal ? transfer.refusal->message : std::string{};
    result.rows.push_back("Description: " + (description.empty() ? std::string("unavailable") : description));
    result.rows.push_back("Date: " + (date.empty() ? std::string("unavailable") : date));
    result.rows.push_back("Campaign: " + roster.campaign);
    result.rows.push_back("Saved tick: " + std::to_string(tick));
    result.rows.push_back("Required humans: " + std::to_string(checkpoint_humans(roster).size()));
    for (const auto& seat : roster.seats) {
        const auto player = std::find_if(roster.players.begin(), roster.players.end(),
            [&](const auto& row) { return row.player == seat.player; });
        std::string line = "Seat " + std::to_string(seat.seat_id.value) + " - "
            + (player == roster.players.end() ? std::string("unknown faction") : player->faction)
            + (seat.kind == sim::SeatKind::human ? " (human" : " (AI/script")
            + (seat.active ? ")" : ", inactive)");
        for (std::size_t i = 0; i < bindings.size() && i < peers.size(); ++i)
            if (bindings[i].seat == seat.seat_id && peers[i] == local) line += " - your assigned seat";
        result.rows.push_back(std::move(line));
    }
    result.rows.push_back("Difficulty: " + roster.difficulty);
    if (roster.custom) {
        result.rows.push_back("Credits: " + std::to_string(roster.credits));
        result.rows.push_back("Technology: " + std::to_string(roster.starting_tech) + " / " + std::to_string(roster.maximum_tech));
        result.rows.push_back(roster.allow_heroes ? "Heroes: On" : "Heroes: Off");
    }
    result.rows.push_back(result.progress);
    if (!result.refusal.empty()) result.rows.push_back("Refused: " + result.refusal);
    return result;
}
} // namespace eawr::viewer
