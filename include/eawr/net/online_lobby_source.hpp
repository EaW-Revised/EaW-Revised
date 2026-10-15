#pragma once
#include "eawr/net/online_lobby.hpp"
#include "eawr/presentation/ui/lan_lobby.hpp"
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace eawr::net {
enum class OnlineHostingState : std::uint8_t { idle, publishing, hosted, retrying };
struct OnlineLobbyJoinCompletion {
    presentation::ui::LobbyAttemptId attempt;
    std::optional<OnlineRoomAdmission> admission;
    std::optional<core::Diagnostic> refusal;
};
// What the shared lobby screen needs from an online room source beyond the list, members and chat:
// the host password, the password join, refresh and the hosting state. Nonblocking.
class OnlineRoomSource : public presentation::ui::LobbyGameSource {
public:
    // The live source publishes only after its host's bridge invitation exists.
    // Fixed presentation sources do not need a signalling capability.
    virtual void set_bridge_invitation(std::string, std::string = {}) {}
    // False when the source refuses the change (for example while the room is listed).
    virtual bool set_host_password(std::string password) = 0;
    virtual void begin_join(presentation::ui::LobbyAttemptId, std::string room, std::string password, std::string name) = 0;
    virtual void cancel(presentation::ui::LobbyAttemptId) = 0;
    // Asks for a fresh room list now instead of at the next poll.
    virtual void refresh() = 0;
    virtual std::vector<OnlineLobbyJoinCompletion> take_join_results() = 0;
    virtual std::optional<core::Diagnostic> last_error() const = 0;
    virtual OnlineHostingState hosting_state() const = 0;
    virtual std::vector<core::Diagnostic> take_notices() = 0;
};
// Nonblocking frame seam. All service calls, including lease renewal, run on
// the owned I/O worker. Directory advertisements never authorize game setup.
class OnlineLobbyGameSource final : public OnlineRoomSource {
public:
    OnlineLobbyGameSource(std::unique_ptr<OnlineLobbyService>, OnlineRoomMetadata local_tags, bool require_bridge = false);
    void set_bridge_invitation(std::string, std::string ownership = {}) override;
    ~OnlineLobbyGameSource() override;
    bool set_host_password(std::string password) override;
    void begin_join(presentation::ui::LobbyAttemptId, std::string room, std::string password, std::string name) override;
    void cancel(presentation::ui::LobbyAttemptId) override;
    void refresh() override;
    std::vector<OnlineLobbyJoinCompletion> take_join_results() override;
    std::optional<core::Diagnostic> last_error() const override;
    OnlineHostingState hosting_state() const override;
    std::vector<core::Diagnostic> take_notices() override;
    void announce(const presentation::ui::LobbyPresence&) override;
    void send_chat(presentation::ui::LobbyChatScope, const std::string&, const std::string&, const std::string&) override;
    bool pump() override;
    std::vector<presentation::ui::LobbyGame> games() const override;
    std::vector<presentation::ui::LobbyPlayer> players() const override;
    std::vector<presentation::ui::LobbyPlayer> joining(const std::string&) const override;
    std::vector<presentation::ui::LobbyChatMessage> take_chat() override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Scriptable fixed source for list/password/chat UI fixtures. Uses only public
// display types; no credentials or network I/O are needed in a fixture. A join checks the room's
// scripted password like the service does: a wrong one is refused with 1546, and the third wrong
// one in a row starts the cooldown (1547) until `clear_cooldown`.
class FixedOnlineLobbySource final : public OnlineRoomSource {
public:
    void set_rooms(std::vector<OnlineRoomRow>);
    void set_members(std::string game, std::vector<OnlineRoomMember>);
    void queue_chat(presentation::ui::LobbyChatMessage);
    void set_room_password(const std::string& room, std::string password);
    void clear_cooldown() { wrong_attempts_ = 0; }
    void set_hosting_state(OnlineHostingState state) { hosting_ = state; }
    void fail_listing(bool failed) { listing_failed_ = failed; changed_ = true; }
    std::size_t refreshes() const { return refreshes_; }
    const std::string& host_password() const { return host_password_; }
    bool set_host_password(std::string password) override { host_password_ = std::move(password); return true; }
    void begin_join(presentation::ui::LobbyAttemptId, std::string room, std::string password, std::string name) override;
    void cancel(presentation::ui::LobbyAttemptId) override;
    void refresh() override { ++refreshes_; changed_ = true; }
    std::vector<OnlineLobbyJoinCompletion> take_join_results() override;
    std::optional<core::Diagnostic> last_error() const override;
    OnlineHostingState hosting_state() const override { return hosting_; }
    std::vector<core::Diagnostic> take_notices() override { return std::exchange(notices_, {}); }
    const presentation::ui::LobbyPresence& announced() const { return presence_; }
    const std::vector<presentation::ui::LobbyChatMessage>& sent() const { return sent_; }
    void announce(const presentation::ui::LobbyPresence& p) override;
    void send_chat(presentation::ui::LobbyChatScope, const std::string&, const std::string&, const std::string&) override;
    bool pump() override { const bool result = changed_; changed_ = false; return result; }
    std::vector<presentation::ui::LobbyGame> games() const override { return listing_failed_ ? std::vector<presentation::ui::LobbyGame>{} : games_; }
    std::vector<presentation::ui::LobbyPlayer> players() const override { return players_; }
    std::vector<presentation::ui::LobbyPlayer> joining(const std::string&) const override;
    std::vector<presentation::ui::LobbyChatMessage> take_chat() override;
private:
    presentation::ui::LobbyPresence presence_;
    std::vector<presentation::ui::LobbyGame> games_;
    std::vector<presentation::ui::LobbyPlayer> players_;
    std::vector<presentation::ui::LobbyChatMessage> incoming_, sent_;
    std::vector<OnlineRoomRow> rows_;
    std::vector<std::pair<std::string, std::string>> passwords_; // room -> password
    std::vector<OnlineLobbyJoinCompletion> results_;
    std::vector<core::Diagnostic> notices_;
    std::string host_password_;
    OnlineHostingState hosting_{OnlineHostingState::idle};
    std::size_t refreshes_{}, wrong_attempts_{};
    bool changed_{true}, listing_failed_{};
};
} // namespace eawr::net
