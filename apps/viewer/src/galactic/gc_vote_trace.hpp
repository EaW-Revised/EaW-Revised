#pragma once

#include "eawr/platform/live_game_session.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

// #8353: a log trace of the multiplayer battle vote, so a slow Begin can be told apart: the prompt wait (who is
// required, who voted, when, which route the session chose) versus the battle's own staging and load. Pure view of the
// published frame: nothing here touches the simulation, and a line is produced only when something it reports changes.
namespace eawr::viewer::galactic {

[[nodiscard]] inline std::string vote_trace_utc() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char text[32];
    std::snprintf(text, sizeof text, "%02d:%02d:%02d.%03d", utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<int>(millis));
    return text;
}

class VoteTrace final {
public:
    // Lines for this frame (empty most frames). `stamp` is the wall clock, `seat`/`local` the local seat and player.
    [[nodiscard]] std::vector<std::string> observe(const platform::LiveGameFrame& frame, sim::galactic::SeatId seat,
        sim::galactic::PlayerId local, const std::string& stamp) {
        std::vector<std::string> lines;
        const auto head = [&](const char* event, std::uint64_t conflict) {
            return std::string("EAWR gc-vote: ") + event + " utc=" + stamp + " tick=" + std::to_string(frame.session_tick)
                + " conflict=" + std::to_string(conflict) + " seat=" + std::to_string(seat.value)
                + " player=" + std::to_string(local);
        };
        const auto pending = frame.pending_conflict;
        if (!pending) {
            if (open_) {
                const char* next = frame.conflict ? "battle-child" : frame.auto_resolve_progress || frame.auto_resolve_outcome
                    ? "auto-resolve" : "none";
                lines.push_back(head("prompt-closed", conflict_) + " next=" + next);
                reset();
            }
            return lines;
        }
        const auto& entry = pending->conflict;
        const auto votes = pending->votes;
        const auto route = static_cast<int>(pending->arbitration.route);
        const auto tail = [&] {
            std::string text = " required=" + list(pending->required) + " votes=";
            for (std::size_t i = 0; i < votes.size(); ++i)
                text += (i ? "," : "") + std::to_string(votes[i].first) + (votes[i].second ? ":begin" : ":auto");
            if (votes.empty()) text += "-";
            return text;
        };
        if (!open_ || conflict_ != entry.id) {
            reset(); open_ = true; conflict_ = entry.id; route_ = route; ready_ = pending->choice_ready; votes_ = votes;
            std::string text = head("prompt-open", entry.id) + " invader=" + std::to_string(entry.invader) + " defender="
                + std::to_string(entry.defender) + " route=" + route_name(route) + " ready=" + (ready_ ? "1" : "0") + tail();
            if (pending->vote_started) text += " vote_started=" + std::to_string(*pending->vote_started);
            if (pending->alert) text += " timeout_ms=" + std::to_string(pending->alert->timeout_ms)
                + " elapsed_ms=" + std::to_string(pending->alert->elapsed_wall_ms) + (pending->alert->untimed ? " untimed" : "");
            lines.push_back(std::move(text));
        } else {
            if (route != route_) { route_ = route; lines.push_back(head("route", entry.id) + " route=" + route_name(route) + tail()); }
            if (pending->choice_ready != ready_) {
                ready_ = pending->choice_ready;
                lines.push_back(head("choice-ready", entry.id) + " ready=" + (ready_ ? "1" : "0") + tail());
            }
            if (votes != votes_) { votes_ = votes; lines.push_back(head("votes", entry.id) + tail()); }
        }
        for (const auto& row : frame.battle_choice_responses)
            if (row.seat == seat && row.conflict == entry.id && (row.revision != revision_ || row.refusal != refusal_)) {
                revision_ = row.revision; refusal_ = row.refusal;
                lines.push_back(head("response", entry.id) + " revision=" + std::to_string(row.revision)
                    + (row.refusal.empty() ? " accepted" : " refused=\"" + row.refusal + "\""));
            }
        return lines;
    }
    // The local click at the choice dispatch, whether the session accepted it for dispatch or not.
    [[nodiscard]] static std::string click(const platform::LiveGameFrame& frame, sim::galactic::SeatId seat,
        sim::galactic::PlayerId local, std::uint64_t conflict, bool begin, bool dispatched, const std::string& stamp) {
        return std::string("EAWR gc-vote: click utc=") + stamp + " tick=" + std::to_string(frame.session_tick)
            + " conflict=" + std::to_string(conflict) + " seat=" + std::to_string(seat.value) + " player=" + std::to_string(local)
            + " choice=" + (begin ? "begin" : "auto") + " dispatched=" + (dispatched ? "1" : "0");
    }
private:
    static std::string route_name(int route) {
        static constexpr const char* names[] = {"instant_auto", "dialog_auto", "vote", "pending_tactical", "tactical"};
        return route >= 0 && route < 5 ? names[route] : "?";
    }
    static std::string list(const std::vector<sim::galactic::PlayerId>& values) {
        std::string text;
        for (std::size_t i = 0; i < values.size(); ++i) text += (i ? "," : "") + std::to_string(values[i]);
        return text.empty() ? "-" : text;
    }
    void reset() { open_ = false; conflict_ = 0; route_ = -1; ready_ = false; votes_.clear(); revision_ = 0; refusal_.clear(); }
    bool open_{}, ready_{};
    std::uint64_t conflict_{}, revision_{};
    int route_{-1};
    std::vector<std::pair<sim::galactic::PlayerId, bool>> votes_;
    std::string refusal_;
};

} // namespace eawr::viewer::galactic
