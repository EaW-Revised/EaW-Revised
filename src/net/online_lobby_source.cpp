#include "eawr/net/online_lobby_source.hpp"
#include "relay_directory_codec.hpp"
#include <algorithm>
#include <stdexcept>
#include <stop_token>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace eawr::net {
namespace {
using namespace presentation::ui;
using Clock = std::chrono::steady_clock;
LobbyGame game_row(const OnlineRoomRow& row, const OnlineRoomMetadata* tags = nullptr) {
    LobbyGame game;
    game.name = row.metadata.name; game.map = row.metadata.map; game.game_type = row.metadata.mode;
    game.address = row.room; game.players = row.metadata.occupancy; game.max_players = row.metadata.capacity;
    game.passworded = row.passworded;
    game.version_matches = !tags || (row.metadata.build == tags->build && row.metadata.protocol == tags->protocol
        && row.metadata.content == tags->content && row.metadata.mod == tags->mod);
    game.player_names.push_back(row.metadata.host_name);
    return game;
}
}
struct OnlineLobbyGameSource::Impl {
    std::unique_ptr<OnlineLobbyService> service;
    OnlineRoomMetadata tags;
    mutable std::mutex mutex;
    std::condition_variable condition;
    LobbyPresence presence;
    std::string password;
    bool presence_changed{}, changed{};
    struct Join { LobbyAttemptId attempt; std::string room, password, name; std::uint64_t generation{}; };
    std::optional<Join> pending_join;
    std::atomic<std::uint64_t> generation{};
    std::optional<LobbyAttemptId> current_attempt;
    bool leave_requested{}, refresh_requested{};
    std::deque<std::string> sends;
    std::vector<LobbyGame> games;
    std::vector<LobbyPlayer> players;
    std::vector<LobbyChatMessage> incoming;
    std::vector<OnlineLobbyJoinCompletion> results;
    std::optional<core::Diagnostic> failure;
    std::vector<core::Diagnostic> notices;
    OnlineHostingState hosting{OnlineHostingState::idle};
    std::string active_name;
    std::jthread worker;
    Impl(std::unique_ptr<OnlineLobbyService> s, OnlineRoomMetadata t) : service(std::move(s)), tags(std::move(t)) {}

    void error(const core::Diagnostic& value) {
        std::lock_guard lock(mutex); failure = value;
        if (notices.size() == 64) notices.erase(notices.begin());
        notices.push_back(value);
    }
    void success() { std::lock_guard lock(mutex); failure.reset(); }
    void state(OnlineHostingState value) { std::lock_guard lock(mutex); hosting = value; }
    void run(std::stop_token stop) {
        std::optional<OnlineRoomPublication> publication;
        OnlineRoomMetadata hosted;
        bool admitted{}, wants_host{}, passworded{};
        std::string host_password;
        std::uint32_t chat_sequence{};
        auto next_list = Clock::now(), next_heartbeat = next_list, next_chat = next_list, next_publish = next_list;
        auto last_list = Clock::time_point{};
        std::string cursor;
        std::vector<LobbyGame> page_games;
        auto clear_admission = [&] {
            admitted = false; chat_sequence = 0;
            std::lock_guard lock(mutex); active_name.clear(); players.clear(); incoming.clear(); sends.clear(); changed = true;
        };
        auto heartbeat = [&] {
            auto result = service->heartbeat(*publication, hosted);
            if (result) {
                admitted = true; success(); state(OnlineHostingState::hosted);
                std::lock_guard lock(mutex); active_name = hosted.name;
            } else {
                error(result.error());
                if (result.error().code == "EAWR-NET-1540" || result.error().code == "EAWR-NET-1541") {
                    publication.reset(); clear_admission(); state(OnlineHostingState::retrying); next_publish = Clock::now() + std::chrono::milliseconds(50);
                }
            }
            next_heartbeat = Clock::now() + std::chrono::seconds(10);
        };
        auto chat_error = [&](const core::Diagnostic& diagnostic) {
            error(diagnostic);
            if (diagnostic.code == "EAWR-NET-1541" || diagnostic.code == "EAWR-NET-1548") {
                const auto previous_sequence = chat_sequence;
                clear_admission();
                // Moderation may retain the seat until leave/disconnect. Release
                // that service membership before exposing an idle local source.
                auto released = service->leave(); if (!released) error(released.error()); else success();
                { std::lock_guard lock(mutex); current_attempt.reset(); }
                if (wants_host) {
                    if (publication) {
                        // The listing is still owned/published while its channel
                        // reattaches, so password edits remain locked.
                        state(OnlineHostingState::hosted); chat_sequence = previous_sequence;
                        next_heartbeat = Clock::now() + std::chrono::milliseconds(50);
                    } else { state(OnlineHostingState::retrying); next_publish = Clock::now() + std::chrono::milliseconds(50); }
                }
            }
        };
        while (!stop.stop_requested()) {
            std::optional<Join> join;
            std::optional<LobbyPresence> announced;
            std::string configured_password, text;
            bool leaving{}, refreshing{};
            {
                std::unique_lock lock(mutex);
                condition.wait_for(lock, std::chrono::milliseconds(50), [&] {
                    return stop.stop_requested() || presence_changed || pending_join || leave_requested || refresh_requested || !sends.empty();
                });
                if (stop.stop_requested()) break;
                if (presence_changed) { announced = presence; presence_changed = false; configured_password = password; }
                if (pending_join) { join = std::move(pending_join); pending_join.reset(); }
                leaving = leave_requested; leave_requested = false;
                refreshing = refresh_requested; refresh_requested = false;
                if (!sends.empty()) { text = std::move(sends.front()); sends.pop_front(); }
            }
            if (leaving || (announced && announced->place == LobbyPlace::lobby)) {
                if (publication) { auto result = service->remove(*publication); if (!result) error(result.error()); else success(); publication.reset(); }
                else if (admitted) { auto result = service->leave(); if (!result) error(result.error()); else success(); }
                wants_host = false; clear_admission(); state(OnlineHostingState::idle);
            }
            if (announced && announced->place == LobbyPlace::hosting) {
                wants_host = true; passworded = announced->passworded; host_password = std::move(configured_password);
                hosted = tags; hosted.name = relay::clean_text(announced->game, 24); hosted.host_name = relay::clean_text(announced->name, 20);
                hosted.mode = announced->game_type; hosted.map = announced->map;
                hosted.capacity = announced->max_players; hosted.starting = announced->locked;
                if (publication) heartbeat(); else next_publish = Clock::now();
            }
            if (wants_host && !publication && Clock::now() >= next_publish) {
                state(OnlineHostingState::publishing);
                auto result = passworded && host_password.empty()
                    ? core::Result<OnlineRoomPublication>::failure(relay::error(1540, "A private room needs a password"))
                    : service->publish(hosted, passworded ? host_password : std::string{});
                if (!result) {
                    error(result.error()); state(OnlineHostingState::retrying); next_publish = Clock::now() + std::chrono::seconds(5);
                } else {
                    publication = std::move(result).value(); admitted = true; chat_sequence = 0;
                    next_heartbeat = Clock::now() + std::chrono::seconds(10); success(); state(OnlineHostingState::hosted);
                    std::lock_guard lock(mutex); active_name = hosted.name;
                }
            }
            if (join && join->generation == generation.load()) {
                auto result = service->join(join->room, join->password, join->name);
                if (join->generation != generation.load()) {
                    if (result) { auto released = service->leave(); if (!released) error(released.error()); else success(); }
                } else {
                    if (result) success(); else error(result.error());
                    std::lock_guard lock(mutex);
                    if (join->generation == generation.load()) {
                        OnlineLobbyJoinCompletion completion; completion.attempt = join->attempt;
                        if (result) {
                            completion.admission = std::move(result).value(); admitted = true; chat_sequence = 0;
                            const auto row = std::find_if(games.begin(), games.end(), [&](const auto& game) { return game.address == join->room; });
                            active_name = row == games.end() ? join->room : row->name;
                        } else { completion.refusal = result.error(); current_attempt.reset(); }
                        if (results.size() == 64) results.erase(results.begin());
                        results.push_back(std::move(completion));
                    } else if (result) { admitted = true; leave_requested = true; }
                }
            }
            if (!text.empty() && admitted) {
                auto result = service->send_chat(std::move(text)); if (!result) chat_error(result.error()); else success();
            }
            // A refresh never lists inside the directory's one-second spacing; it keeps the current rooms meanwhile.
            if (refreshing) { next_list = std::max(Clock::now(), last_list + std::chrono::seconds(1)); cursor.clear(); page_games.clear(); }
            const auto now = Clock::now();
            if (publication && now >= next_heartbeat) heartbeat();
            if (now >= next_list) {
                last_list = Clock::now();
                auto result = service->list(cursor);
                if (!result && result.error().code == "EAWR-NET-1542") {
                    // Too soon after the last list: keep the rooms on show and ask again after the spacing.
                    cursor.clear(); page_games.clear();
                } else if (!result) {
                    error(result.error()); cursor.clear(); page_games.clear();
                    std::lock_guard lock(mutex); games.clear(); changed = true;
                } else {
                    success();
                    for (const auto& row : result.value().rooms) if (page_games.size() < 2048) page_games.push_back(game_row(row, &tags));
                    cursor = result.value().next;
                    if (cursor.empty() || page_games.size() >= 2048) {
                        std::lock_guard lock(mutex); games = std::move(page_games); page_games.clear(); cursor.clear(); changed = true;
                    }
                }
                next_list = Clock::now() + std::chrono::seconds(1);
            }
            if (admitted && now >= next_chat) {
                auto result = service->chat(chat_sequence);
                if (!result) chat_error(result.error());
                else {
                    success(); chat_sequence = result.value().through;
                    std::lock_guard lock(mutex); players.clear();
                    for (const auto& member : result.value().members)
                        players.push_back({member.seat, member.name, 0, member.seat == 1 ? LobbyPlace::hosting : LobbyPlace::joining, active_name});
                    for (const auto& line : result.value().lines) {
                        if (incoming.size() == 200) incoming.erase(incoming.begin());
                        incoming.push_back({LobbyChatScope::game, active_name, line.name, line.text});
                    }
                    changed = true;
                }
                next_chat = Clock::now() + std::chrono::seconds(2);
            }
        }
        if (publication) (void)service->remove(*publication);
        else if (admitted) (void)service->leave();
    }

};
OnlineLobbyGameSource::OnlineLobbyGameSource(std::unique_ptr<OnlineLobbyService> service, OnlineRoomMetadata tags)
    : impl_(std::make_unique<Impl>(std::move(service), std::move(tags))) {
    if (!impl_->service) throw std::invalid_argument("Online lobby source needs a service");
    impl_->worker = std::jthread([this](std::stop_token stop) { impl_->run(stop); });
}
OnlineLobbyGameSource::~OnlineLobbyGameSource() { impl_->worker.request_stop(); impl_->condition.notify_all(); impl_->worker.join(); }
bool OnlineLobbyGameSource::set_host_password(std::string password) {
    std::lock_guard lock(impl_->mutex);
    const bool locked = impl_->hosting == OnlineHostingState::hosted || impl_->hosting == OnlineHostingState::publishing;
    if (locked || password.size() > 128) {
        const auto diagnostic = relay::error(locked ? 1554 : 1540, locked ? "Room password cannot change while published or publishing" : "Room password exceeds the service limit");
        impl_->failure = diagnostic;
        if (impl_->notices.size() == 64) impl_->notices.erase(impl_->notices.begin());
        impl_->notices.push_back(diagnostic); return false;
    }
    impl_->password = std::move(password); impl_->failure.reset();
    if (impl_->presence.place == LobbyPlace::hosting) { impl_->presence_changed = true; impl_->condition.notify_all(); }
    return true;
}
void OnlineLobbyGameSource::refresh() {
    std::lock_guard lock(impl_->mutex); impl_->refresh_requested = true; impl_->condition.notify_all();
}
void OnlineLobbyGameSource::announce(const LobbyPresence& presence) {
    std::lock_guard lock(impl_->mutex);
    if (presence.place == LobbyPlace::lobby && impl_->current_attempt) {
        ++impl_->generation; impl_->current_attempt.reset(); impl_->pending_join.reset(); impl_->results.clear();
        impl_->leave_requested = true; impl_->condition.notify_all();
    }
    if (presence == impl_->presence) return;
    impl_->presence = presence; impl_->presence_changed = true; impl_->condition.notify_all();
}
void OnlineLobbyGameSource::begin_join(LobbyAttemptId attempt, std::string room, std::string password, std::string name) {
    std::lock_guard lock(impl_->mutex);
    if (attempt.source != LobbySourceKind::online || password.size() > 128 || !relay::code_valid(room) || name.size() > 256 || relay::clean_text(name, 20).empty()) {
        const auto diagnostic = relay::error(1540, "Invalid online join request"); impl_->failure = diagnostic;
        if (impl_->results.size() == 64) impl_->results.erase(impl_->results.begin());
        impl_->results.push_back({attempt, {}, diagnostic}); return;
    }
    if (impl_->current_attempt) {
        if (impl_->results.size() == 64) impl_->results.erase(impl_->results.begin());
        impl_->results.push_back({attempt, {}, relay::error(1543, "An online join is already active")}); return;
    }
    impl_->current_attempt = attempt;
    impl_->pending_join = Impl::Join{attempt, std::move(room), std::move(password), std::move(name), ++impl_->generation};
    impl_->condition.notify_all();
}
void OnlineLobbyGameSource::cancel(LobbyAttemptId attempt) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->current_attempt != attempt) return;
    ++impl_->generation; impl_->current_attempt.reset(); impl_->pending_join.reset(); impl_->leave_requested = true;
    std::erase_if(impl_->results, [&](const auto& result) { return result.attempt == attempt; });
    impl_->condition.notify_all();
}
std::vector<OnlineLobbyJoinCompletion> OnlineLobbyGameSource::take_join_results() {
    std::lock_guard lock(impl_->mutex); auto results = std::move(impl_->results); impl_->results.clear();
    return results;
}
std::optional<core::Diagnostic> OnlineLobbyGameSource::last_error() const { std::lock_guard lock(impl_->mutex); return impl_->failure; }
OnlineHostingState OnlineLobbyGameSource::hosting_state() const { std::lock_guard lock(impl_->mutex); return impl_->hosting; }
std::vector<core::Diagnostic> OnlineLobbyGameSource::take_notices() { std::lock_guard lock(impl_->mutex); auto result = std::move(impl_->notices); impl_->notices.clear(); return result; }
void OnlineLobbyGameSource::send_chat(LobbyChatScope scope, const std::string& game, const std::string&, const std::string& text) {
    std::lock_guard lock(impl_->mutex);
    if (scope != LobbyChatScope::game || game != impl_->active_name || text.empty()) return;
    if (text.size() > 128 || impl_->sends.size() == 64) {
        // A refused line is a notice for the chat window, not a failure of the room list.
        if (impl_->notices.size() == 64) impl_->notices.erase(impl_->notices.begin());
        impl_->notices.push_back(relay::error(1549, "Room chat queue is full or text exceeds the limit"));
        return;
    }
    impl_->sends.push_back(text); impl_->condition.notify_all();
}
bool OnlineLobbyGameSource::pump() { std::lock_guard lock(impl_->mutex); const bool changed = impl_->changed; impl_->changed = false; return changed; }
std::vector<LobbyGame> OnlineLobbyGameSource::games() const { std::lock_guard lock(impl_->mutex); return impl_->games; }
std::vector<LobbyPlayer> OnlineLobbyGameSource::players() const { std::lock_guard lock(impl_->mutex); return impl_->players; }
std::vector<LobbyPlayer> OnlineLobbyGameSource::joining(const std::string& game) const {
    auto players = this->players(); std::erase_if(players, [&](const auto& p) { return p.place != LobbyPlace::joining || p.game != game; }); return players;
}
std::vector<LobbyChatMessage> OnlineLobbyGameSource::take_chat() { std::lock_guard lock(impl_->mutex); auto lines = std::move(impl_->incoming); impl_->incoming.clear(); return lines; }
void FixedOnlineLobbySource::set_rooms(std::vector<OnlineRoomRow> rows) {
    rows_ = rows;
    games_.clear(); for (const auto& row : rows) if (games_.size() < 2048 && !row.metadata.starting && !row.metadata.closed) games_.push_back(game_row(row)); changed_ = true;
}
void FixedOnlineLobbySource::set_members(std::string game, std::vector<OnlineRoomMember> members) {
    players_.clear(); for (const auto& member : members) if (players_.size() < 64)
        players_.push_back({member.seat, relay::clean_text(member.name, 20), 0, member.seat == 1 ? LobbyPlace::hosting : LobbyPlace::joining, game});
    changed_ = true;
}
void FixedOnlineLobbySource::queue_chat(LobbyChatMessage message) {
    if (incoming_.size() == 200) incoming_.erase(incoming_.begin());
    message.name = relay::clean_text(message.name, 20); message.text = relay::clean_text(message.text, 128); incoming_.push_back(std::move(message));
}
void FixedOnlineLobbySource::set_room_password(const std::string& room, std::string password) {
    std::erase_if(passwords_, [&](const auto& entry) { return entry.first == room; });
    passwords_.emplace_back(room, std::move(password));
}
void FixedOnlineLobbySource::announce(const LobbyPresence& p) {
    presence_ = p;
    hosting_ = p.place == LobbyPlace::hosting ? OnlineHostingState::hosted : OnlineHostingState::idle;
}
void FixedOnlineLobbySource::begin_join(LobbyAttemptId attempt, std::string room, std::string password, std::string name) {
    OnlineLobbyJoinCompletion completion; completion.attempt = attempt;
    const auto row = std::find_if(rows_.begin(), rows_.end(), [&](const auto& r) { return r.room == room; });
    const auto expected = std::find_if(passwords_.begin(), passwords_.end(), [&](const auto& e) { return e.first == room; });
    if (attempt.source != LobbySourceKind::online || relay::clean_text(name, 20).empty()) completion.refusal = relay::error(1540, "Invalid online join request");
    else if (row == rows_.end() || row->metadata.starting || row->metadata.closed) completion.refusal = relay::error(1544, "Room is not available");
    else if (row->metadata.occupancy >= row->metadata.capacity) completion.refusal = relay::error(1545, "Room is full");
    else if (row->passworded && wrong_attempts_ >= 3) completion.refusal = relay::error(1547, "Password attempts are cooling down");
    else if (row->passworded && (expected == passwords_.end() || expected->second != password)) {
        ++wrong_attempts_;
        completion.refusal = relay::error(wrong_attempts_ >= 3 ? 1547 : 1546, "Password was not accepted");
    } else { wrong_attempts_ = 0; completion.admission = OnlineRoomAdmission{room, 2}; }
    if (completion.refusal) {
        if (notices_.size() == 64) notices_.erase(notices_.begin());
        notices_.push_back(*completion.refusal);
    }
    results_.push_back(std::move(completion)); changed_ = true;
}
void FixedOnlineLobbySource::cancel(LobbyAttemptId attempt) {
    std::erase_if(results_, [&](const auto& result) { return result.attempt == attempt; });
}
std::vector<OnlineLobbyJoinCompletion> FixedOnlineLobbySource::take_join_results() { return std::exchange(results_, {}); }
std::optional<core::Diagnostic> FixedOnlineLobbySource::last_error() const {
    if (listing_failed_) return relay::error(1552, "Room list is unavailable");
    return std::nullopt;
}
void FixedOnlineLobbySource::send_chat(LobbyChatScope scope, const std::string& game, const std::string& name, const std::string& text) {
    if (scope != LobbyChatScope::game) return;
    if (sent_.size() == 200) sent_.erase(sent_.begin());
    sent_.push_back({scope, game, relay::clean_text(name, 20), relay::clean_text(text, 128)});
}
std::vector<LobbyPlayer> FixedOnlineLobbySource::joining(const std::string& game) const {
    auto result = players_; std::erase_if(result, [&](const auto& p) { return p.place != LobbyPlace::joining || p.game != game; }); return result;
}
std::vector<LobbyChatMessage> FixedOnlineLobbySource::take_chat() { auto result = std::move(incoming_); incoming_.clear(); return result; }
} // namespace eawr::net
