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
// #6121: the host's saved-seat assignment, one entry per seat now in the room (peer index = position in the room's roster).
// It is rebuilt whenever the room's seat count changes: the first version was built once when the save was read, so a guest who
// joined afterwards had no entry (its combo showed the first saved seat, selecting a seat did nothing, Apply always refused).
// An existing assignment is kept while its saved seat is still a human seat of the save and no earlier peer holds it; every other
// peer gets the first saved human seat nobody holds, in saved order, so each human has a distinct saved seat, whatever the
// factions (three or more humans on one faction included). A peer beyond the save's humans stays unassigned (seat 0): Apply
// then says the room has an extra human.
// #6178: `current` is indexed by place in the room as it was (`previous_peers`, the lobby peer at each place), and the result by place
// in the room as it is (`room_peers`). A kept assignment follows its PEER, so when a middle guest leaves, the guest behind keeps its
// own saved seat instead of inheriting the one at the place it moved into.
inline std::vector<net::SetupSeat> reconcile_checkpoint_bindings(const sim::galactic::MpRoster& roster,
    const std::vector<net::SetupSeat>& current, const std::vector<net::PeerIndex>& previous_peers,
    const std::vector<net::PeerIndex>& room_peers) {
    const auto humans = checkpoint_humans(roster);
    const std::size_t room_seats = room_peers.size();
    std::vector<net::SetupSeat> result(room_seats);
    std::vector<bool> taken(humans.size(), false);
    const auto hold = [&](const net::SetupSeat& binding) {
        for (std::size_t j = 0; j < humans.size(); ++j)
            if (humans[j].seat_id == binding.seat && humans[j].player == binding.player && !taken[j]) { taken[j] = true; return true; }
        return false;
    };
    std::vector<bool> kept(room_seats, false);
    for (std::size_t peer = 0; peer < room_seats; ++peer) {
        result[peer].peer = static_cast<net::PeerIndex>(peer);
        const auto before = std::find(previous_peers.begin(), previous_peers.end(), room_peers[peer]);
        const auto place = static_cast<std::size_t>(before - previous_peers.begin());
        if (before != previous_peers.end() && place < current.size() && hold(current[place])) {
            result[peer].seat = current[place].seat; result[peer].player = current[place].player; kept[peer] = true;
        }
    }
    for (std::size_t peer = 0; peer < room_seats; ++peer) {
        if (kept[peer]) continue;
        for (std::size_t j = 0; j < humans.size(); ++j)
            if (!taken[j]) { taken[j] = true; result[peer].seat = humans[j].seat_id; result[peer].player = humans[j].player; break; }
    }
    return result;
}
// The room as places 0..n-1 (what the first version, and a room whose peers never change, assumed).
inline std::vector<net::SetupSeat> reconcile_checkpoint_bindings(const sim::galactic::MpRoster& roster,
    const std::vector<net::SetupSeat>& current, const std::size_t room_seats) {
    std::vector<net::PeerIndex> before(current.size()), now(room_seats);
    for (std::size_t i = 0; i < before.size(); ++i) before[i] = static_cast<net::PeerIndex>(i);
    for (std::size_t i = 0; i < now.size(); ++i) now[i] = static_cast<net::PeerIndex>(i);
    return reconcile_checkpoint_bindings(roster, current, before, now);
}
// #6128: when a restored save keeps the lobby's Start (host) and Accept (guest) buttons off. Only the host ever sees `agreed`: it is
// set when the host has collected every guest's restore acknowledgement, and no message carries it back. A guest is done when its own
// restore is `restored`; waiting for `agreed` on a guest kept Accept off for ever ("Waiting for restore agreement"), so no saved
// co-op game could start.
inline bool checkpoint_start_blocked(const bool host, const bool pending, const bool refused, const bool has_checkpoint,
    const net::SaveTransferView& transfer) {
    return pending || refused || (has_checkpoint && !(host ? transfer.agreed : transfer.restored));
}
struct IdentityFactionName {
    std::string operator()(const std::string& faction) const { return faction; }
};
template<class FactionName = IdentityFactionName>
inline CheckpointLobbyModel checkpoint_lobby_model(bool host, bool staging,
    const sim::galactic::MpRoster& roster, std::uint64_t tick,
    const std::vector<net::SetupSeat>& bindings, const std::vector<net::PeerIndex>& peers,
    net::PeerIndex local, const net::SaveTransferView& transfer,
    const std::string& description, const std::string& date, const std::string& problem,
    const FactionName& faction_name = {}) {
    // The model has no catalog: the screen passes the display-name lookup in (#6086). Without one the identity stays.
    CheckpointLobbyModel result;
    result.can_assign = host && staging;
    result.can_accept = staging && (host ? transfer.agreed : transfer.restored) && problem.empty() && !transfer.refusal;
    result.progress = transfer.agreed ? "Save restored by every player"
        : transfer.restored ? (host ? "Waiting for every player to restore the save" : "Save restored; accept when ready")
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
            + (player == roster.players.end() ? std::string("unknown faction")
                : faction_name(player->faction))
            + (seat.kind == sim::SeatKind::human ? " (human" : " (AI/script")
            + (seat.active ? ")" : ", inactive)");
        for (std::size_t i = 0; i < bindings.size() && i < peers.size(); ++i)
            if (bindings[i].seat == seat.seat_id && peers[i] == local) line += " - your assigned seat";
        result.rows.push_back(std::move(line));
    }
    result.rows.push_back("Difficulty: " + roster.difficulty);
    if (roster.vote_timeout_seconds) result.rows.push_back("Battle vote timeout: " + std::to_string(roster.vote_timeout_seconds) + " s");
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
