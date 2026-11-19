#pragma once

#include "eawr/net/galactic_checkpoint_transfer.hpp"
#include "eawr/net/lobby_session.hpp"
#include "eawr/mods/save_description.hpp"
#include "eawr/sim/galactic/mp_roster.hpp"

#include <type_traits>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <set>
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
// #6468: why the host's settled mod list cannot go to the guests, or an empty string when it can. Each reason is its own
// text: a missing encoding, a list over the room limit and text the room codec refuses are different problems for the host.
inline std::string host_mod_list_unsendable_reason(const std::optional<std::vector<std::uint8_t>>& encoded) {
    if (!encoded) return "The mod list could not be prepared for the other players, so they cannot check it against theirs.";
    if (encoded->size() > net::max_lobby_mod_description_bytes)
        return "The mod list is too large to send to the other players (" + std::to_string(encoded->size()) + " bytes, the limit is "
            + std::to_string(net::max_lobby_mod_description_bytes) + "), so they cannot check it against theirs.";
    if (!net::valid_lobby_mod_description(*encoded))
        return "The mod list has text the lobby cannot carry (for example hidden direction marks in a mod name), so the other players "
               "cannot check it against theirs.";
    return {};
}
// #6468: a refused refresh is retried with a growing pause and given up after a few tries, so it never loops per frame.
struct HostModRefreshRetry {
    static constexpr unsigned max_attempts = 8;
    unsigned attempts{};
    std::chrono::steady_clock::time_point next{};
    [[nodiscard]] bool ready(const std::chrono::steady_clock::time_point now) const { return now >= next; }
    // Records one refusal. True once the cap is reached and the caller must stop retrying and say so.
    bool refused(const std::chrono::steady_clock::time_point now) {
        ++attempts;
        next = now + std::chrono::milliseconds(100U << std::min(attempts, 5U));
        return attempts >= max_attempts;
    }
};
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
enum class CheckpointRestoreState { idle, pending, ready, stale, room_changed, failed };
template<class Value, class Room> class CheckpointRestoreJob {
public:
    using Clock = std::chrono::steady_clock;
    struct Poll { CheckpointRestoreState state; std::optional<Value> value; };
    [[nodiscard]] bool active() const { return work_.valid(); }
    // `same` decides whether the room at the end is still the room the work was built for (#7878). The default is full
    // equality; the lobby passes the restore's own notion (players, seats, game settings), so a revision, a readiness flag or
    // the host's display-only mod description moving during a long restore is not a room change.
    using Same = bool (*)(const Room&, const Room&);
    // The restore of a saved campaign is a black box without progress ticks, so the job never gives up on it: a heavy-mod chain takes
    // minutes, and a result abandoned at a deadline left the lobby refusing a restore that then finished (#7858). It only remembers
    // when it started, so the lobby can show the elapsed time.
    template<class Work> bool submit(std::uint64_t request, Room room, Clock::time_point started, Work work, Same same = nullptr) {
        if (active() || finishing()) return false;
        request_ = request; room_ = std::move(room); started_ = started; same_ = std::move(same);
        // The worker gets a cancel flag when it takes one (a work that takes `const std::atomic<bool>&` checks it between its phases),
        // and `running` tells the lobby a cancelled worker is still finishing (#7858).
        auto flag = std::make_shared<std::atomic<bool>>(false);
        auto running = std::make_shared<std::atomic<bool>>(true);
        cancel_flag_ = flag; running_ = running; finishing_.reset();
        work_ = checkpoint_work([work = std::move(work), flag, running]() mutable {
            struct Ends { std::shared_ptr<std::atomic<bool>> running; ~Ends() { running->store(false, std::memory_order_release); } } ends{running};
            if constexpr (std::is_invocable_v<Work&, const std::atomic<bool>&>) return work(*flag);
            else return work();
        });
        return true;
    }
    // The room the last submitted work was built for; what a room_changed result is compared against.
    [[nodiscard]] const Room* submitted_room() const { return room_ ? &*room_ : nullptr; }
    Poll poll(std::uint64_t request, const Room* room, Clock::time_point /*now: nothing expires any more*/) {
        if (!active()) return {CheckpointRestoreState::idle, {}};
        const bool current = request == request_;
        if (work_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return {CheckpointRestoreState::pending, {}};
        try {
            auto value = work_.get();
            if (!current || !room || !matches(*room)) {
                (void)checkpoint_work([discard = std::move(value)] { static_cast<void>(discard); });
                return {current ? CheckpointRestoreState::room_changed : CheckpointRestoreState::stale, {}};
            }
            return {CheckpointRestoreState::ready, std::move(value)};
        } catch (...) {
            return {current && room && matches(*room) ? CheckpointRestoreState::failed : CheckpointRestoreState::stale, {}};
        }
    }
    [[nodiscard]] Clock::time_point started() const { return started_; }
    // The host's Cancel restore: the lobby stops waiting and the worker is told to stop at its next phase boundary. Its result is dropped
    // unread (a future from checkpoint_work never joins). No new restore starts until the cancelled worker has really ended
    // (`finishing()`), so two heavy restores never run side by side (#7858).
    void cancel() {
        if (active() && cancel_flag_) { cancel_flag_->store(true, std::memory_order_release); finishing_ = running_; }
        work_ = {}; room_.reset();
    }
    [[nodiscard]] bool finishing() const { return finishing_ && finishing_->load(std::memory_order_acquire); }
private:
    [[nodiscard]] bool matches(const Room& room) const { return same_ ? same_(*room_, room) : *room_ == room; }
    std::future<Value> work_;
    std::optional<Room> room_;
    Same same_{};
    Clock::time_point started_{};
    std::shared_ptr<std::atomic<bool>> cancel_flag_, running_, finishing_;
    std::uint64_t request_{};
};
// A restore that has run this long is called slow, never failed (#7858): the lobby keeps waiting for the result.
inline constexpr std::chrono::minutes checkpoint_restore_slow_after{5};
// The lobby's line while a restore runs, on the host (preparing the save, then waiting for the guests) and on a guest: a busy
// state with the elapsed time, never a refusal.
// The line while a cancelled restore is still ending: no new restore starts beside it (#7858).
inline std::string checkpoint_finishing_text() { return "Finishing the cancelled restore..."; }
inline std::string checkpoint_restoring_text(const std::chrono::steady_clock::duration elapsed, const bool others = false) {
    const auto seconds = std::max<long long>(0, std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
    const auto clock = std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
    const std::string subject = others ? "the other players to restore the saved game" : "the saved game to restore";
    return elapsed >= checkpoint_restore_slow_after
        ? "Still waiting for " + subject + " (" + clock + " so far). A large modded campaign can take a while."
        : "Restoring saved game... please wait (" + clock + (others ? ", waiting for the other players)" : ")");
}
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
// #7876: the departed human seats of the save (a player dropped before it was saved). They are not required, but a joining player may
// take one back: it is re-seated when the game loads.
inline std::vector<sim::galactic::Seat> checkpoint_departed(const sim::galactic::MpRoster& roster) {
    std::vector<sim::galactic::Seat> result;
    for (const auto& seat : roster.seats)
        if (seat.kind == sim::SeatKind::human && !seat.active) result.push_back(seat);
    return result;
}
// #7869: a faction a human can join in a loaded save, instead of holding a saved seat. `shared` is a faction that has humans in
// the save (the newcomer plays beside them, as in a new campaign); the others are AI factions a human takes over. `playable` is the
// campaign's own rule for which factions a player may take; the caller (the screen) reads it from the loaded campaign. A faction
// with no human in the save is offered only while the saved humans hold fewer than two sides (the lobby never has a third).
template<class Playable>
inline std::vector<sim::galactic::MpRosterPlayer> checkpoint_join_choices(const sim::galactic::MpRoster& roster, const Playable& playable) {
    std::set<sim::galactic::HumanSideId> sides;
    std::set<sim::galactic::PlayerId> held;
    for (const auto& seat : roster.seats)
        if (seat.kind == sim::SeatKind::human) { sides.insert(seat.human_side_id); held.insert(seat.player); }
    std::vector<sim::galactic::MpRosterPlayer> result;
    for (const auto& player : roster.players) {
        if (!player.active) continue;
        if (held.contains(player.player)) { result.push_back(player); continue; }
        if (sides.size() < 2 && playable(player)) result.push_back(player);
    }
    return result;
}
// #6121: the host's saved-seat assignment, one entry per seat now in the room (peer index = position in the room's roster).
// It is rebuilt whenever the room's seat count changes: the first version was built once when the save was read, so a guest who
// joined afterwards had no entry (its combo showed the first saved seat, selecting a seat did nothing, Apply always refused).
// An existing assignment is kept while its saved seat is still a human seat of the save and no earlier peer holds it; every other
// peer gets the first saved human seat nobody holds, in saved order, so each human has a distinct saved seat, whatever the
// factions (three or more humans on one faction included). A peer beyond the save's humans joins the save (#7869): saved seat 0
// and the faction it joins, by default the faction of the first saved human (a shared seat); the host picks another in its combo.
// A join the host already chose stays with its peer, unless a saved seat has become free (a saved player left the room): a peer
// takes a free saved seat before it joins, so a leaving player never makes Apply say a saved human is missing.
// #6178: `current` is indexed by place in the room as it was (`previous_peers`, the lobby peer at each place), and the result by place
// in the room as it is (`room_peers`). A kept assignment follows its PEER, so when a middle guest leaves, the guest behind keeps its
// own saved seat instead of inheriting the one at the place it moved into.
inline std::vector<net::SetupSeat> reconcile_checkpoint_bindings(const sim::galactic::MpRoster& roster,
    const std::vector<net::SetupSeat>& current, const std::vector<net::PeerIndex>& previous_peers,
    const std::vector<net::PeerIndex>& room_peers) {
    const auto humans = checkpoint_humans(roster);
    // The active saved humans first, then the departed ones (#7876): a peer takes a free active seat before a departed one.
    auto mappable = humans;
    for (const auto& seat : checkpoint_departed(roster)) mappable.push_back(seat);
    const std::size_t room_seats = room_peers.size();
    std::vector<net::SetupSeat> result(room_seats);
    std::vector<bool> taken(mappable.size(), false);
    const auto hold = [&](const net::SetupSeat& binding) {
        for (std::size_t j = 0; j < mappable.size(); ++j)
            if (mappable[j].seat_id == binding.seat && mappable[j].player == binding.player && !taken[j]) { taken[j] = true; return true; }
        return false;
    };
    const auto listed = [&](sim::galactic::PlayerId player) {
        return std::any_of(roster.seats.begin(), roster.seats.end(), [&](const auto& seat) { return seat.player == player; });
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
    const sim::galactic::PlayerId join_default = humans.empty() ? sim::galactic::PlayerId{} : humans.front().player;
    for (std::size_t peer = 0; peer < room_seats; ++peer) {
        if (kept[peer]) continue;
        bool seated = false;
        for (std::size_t j = 0; j < mappable.size() && !seated; ++j)
            if (!taken[j]) { taken[j] = true; result[peer].seat = mappable[j].seat_id; result[peer].player = mappable[j].player; seated = true; }
        if (seated) continue;
        // No saved seat is free: the peer joins. A faction it joined before stays; otherwise the default.
        const auto before = std::find(previous_peers.begin(), previous_peers.end(), room_peers[peer]);
        const auto place = static_cast<std::size_t>(before - previous_peers.begin());
        const bool was_join = before != previous_peers.end() && place < current.size() && current[place].seat.value == 0
            && current[place].player != 0 && listed(current[place].player);
        result[peer].player = was_join ? current[place].player : join_default;
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
    const FactionName& faction_name = {}, const std::string& status = {}) {
    // The model has no catalog: the screen passes the display-name lookup in (#6086). Without one the identity stays.
    CheckpointLobbyModel result;
    result.can_assign = host && staging;
    result.can_accept = staging && (host ? transfer.agreed : transfer.restored) && problem.empty() && status.empty() && !transfer.refusal;
    // #7858: never "Transfer: 0 / 0 bytes". The host shows only the restore state (its own restore is the status line); a guest shows
    // real byte progress while the file arrives and nothing once it is all here (its restore is then the status line).
    result.progress = transfer.agreed ? "Save restored by every player"
        : transfer.restored ? (host ? "Waiting for every player to restore the save" : "Save restored; accept when ready")
        : !host && transfer.total > 0 && transfer.received < transfer.total
            ? "Transfer: " + std::to_string(transfer.received) + " / " + std::to_string(transfer.total) + " bytes"
            : std::string{};
    result.refusal = !problem.empty() ? problem : transfer.refusal ? transfer.refusal->message : std::string{};
    // #7858: a restore in progress is a state of its own, the first row so a long seat list never scrolls it away; only a real
    // refusal reads "Refused".
    if (!status.empty()) result.rows.push_back(status);
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
    // #7869: the players joining the save are the peers whose binding has no saved seat; each names the faction it plays.
    for (std::size_t i = 0; i < bindings.size() && i < peers.size(); ++i) {
        const auto departed_seat = std::find_if(roster.seats.begin(), roster.seats.end(), [&](const auto& seat) {
            return seat.seat_id == bindings[i].seat && seat.kind == sim::SeatKind::human && !seat.active; });
        if (departed_seat != roster.seats.end()) {
            const auto owner = std::find_if(roster.players.begin(), roster.players.end(),
                [&](const auto& row) { return row.player == departed_seat->player; });
            result.rows.push_back(std::string(peers[i] == local ? "You take back seat " : "A new player takes back seat ")
                + std::to_string(departed_seat->seat_id.value) + (owner == roster.players.end() ? std::string{} : " - " + faction_name(owner->faction))
                + " (its player left before the save)");
            continue;
        }
        if (bindings[i].seat.value != 0 || bindings[i].player == 0) continue;
        const auto player = std::find_if(roster.players.begin(), roster.players.end(),
            [&](const auto& row) { return row.player == bindings[i].player; });
        const auto held = std::any_of(roster.seats.begin(), roster.seats.end(), [&](const auto& seat) {
            return seat.kind == sim::SeatKind::human && seat.player == bindings[i].player; });
        result.rows.push_back(std::string(peers[i] == local ? "You join " : "A new player joins ")
            + (player == roster.players.end() ? std::string("a faction") : faction_name(player->faction))
            + (held ? " (shared with the saved players)" : " (the AI hands over this faction)"));
    }
    result.rows.push_back("Difficulty: " + roster.difficulty);
    if (roster.vote_timeout_seconds) result.rows.push_back("Battle vote timeout: " + std::to_string(roster.vote_timeout_seconds) + " s");
    if (roster.custom) {
        result.rows.push_back("Credits: " + std::to_string(roster.credits));
        result.rows.push_back("Technology: " + std::to_string(roster.starting_tech) + " / " + std::to_string(roster.maximum_tech));
        result.rows.push_back(roster.allow_heroes ? "Heroes: On" : "Heroes: Off");
    }
    if (!result.progress.empty()) result.rows.push_back(result.progress);
    // A cancelled restore is the host's choice (the Cancel restore button), not a refusal of anything the guest did.
    if (result.refusal == "checkpoint transfer: selection_cleared") result.rows.push_back("The host cancelled the restore or changed a seat. Wait for it to apply the saved seats again.");
    else if (!result.refusal.empty()) result.rows.push_back("Refused: " + result.refusal);
    return result;
}
} // namespace eawr::viewer
