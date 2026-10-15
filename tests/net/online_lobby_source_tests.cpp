#include "eawr/net/online_lobby_source.hpp"
#include "relay_directory_codec.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace eawr::net;
using namespace eawr::presentation::ui;
using namespace std::chrono_literals;
void expect(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template<class F> void until(F predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!predicate()) { if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Source worker did not reach checkpoint"); std::this_thread::sleep_for(2ms); }
}
OnlineRoomMetadata metadata() { OnlineRoomMetadata value; value.name = "Room"; value.host_name = "Host"; value.build = "local-build"; value.protocol = "local-protocol"; value.content = "content"; value.mod = "mod"; value.map = "custom-map"; value.mode = "custom-mode"; return value; }
struct BlockingList {
    std::atomic<bool> entered{}, released{}, destroyed{};
};
class FakeService final : public OnlineLobbyService {
public:
    std::atomic<unsigned> lists{}, publishes{}, heartbeats{}, leaves{}, sends{}, joins{};
    std::atomic<unsigned> fail_publish{};
    std::atomic<bool> fail_heartbeat{}, refuse_chat{}, lose_chat_membership{};
    std::mutex mutex;
    std::condition_variable condition;
    bool hold_join{}, release_join{};
    OnlineRoomMetadata published;
    std::string bridge;
    std::shared_ptr<BlockingList> list_gate;
    ~FakeService() override { if (list_gate) list_gate->destroyed = true; }
    eawr::core::Result<OnlineRoomPage> list(std::string, std::uint32_t) override {
        ++lists;
        if (list_gate) { list_gate->entered = true; while (!list_gate->released) std::this_thread::sleep_for(2ms); }
        auto value = metadata(); value.build = "foreign-build";
        return eawr::core::Result<OnlineRoomPage>::success({{{std::string(32, 'a'), value, true}}, {}});
    }
    eawr::core::Result<OnlineRoomPublication> publish(OnlineRoomMetadata value, std::string) override {
        if (!eawr::net::relay::metadata_valid(value)) return eawr::core::Result<OnlineRoomPublication>::failure(eawr::net::relay::error(1540, "Fake rejected invalid metadata"));
        { std::lock_guard lock(mutex); published = std::move(value); }
        ++publishes;
        if (fail_publish > 0) { --fail_publish; return eawr::core::Result<OnlineRoomPublication>::failure(eawr::net::relay::error(1547, "Publish temporarily refused")); }
        return eawr::core::Result<OnlineRoomPublication>::success({std::string(32, 'a'), std::string(32, 'b'), 30000});
    }
    eawr::core::Result<OnlineRoomPublication> publish_bridge(OnlineRoomMetadata value, std::string password, std::string invitation, std::string) override {
        { std::lock_guard lock(mutex); bridge = std::move(invitation); }
        return publish(std::move(value), std::move(password));
    }
    eawr::core::Result<void> heartbeat(const OnlineRoomPublication&, const OnlineRoomMetadata& value) override {
        if (!eawr::net::relay::metadata_valid(value)) return eawr::core::Result<void>::failure(eawr::net::relay::error(1540, "Fake rejected invalid metadata"));
        { std::lock_guard lock(mutex); published = value; }
        const bool expired = fail_heartbeat.exchange(false); ++heartbeats;
        if (expired) return eawr::core::Result<void>::failure(eawr::net::relay::error(1541, "Lease expired"));
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<void> remove(const OnlineRoomPublication&) override { ++leaves; return eawr::core::Result<void>::success(); }
    eawr::core::Result<OnlineRoomAdmission> join(std::string room, std::string, std::string) override {
        ++joins; std::unique_lock lock(mutex); condition.wait(lock, [&] { return !hold_join || release_join; });
        return eawr::core::Result<OnlineRoomAdmission>::success({std::move(room), 2});
    }
    eawr::core::Result<void> leave() override { ++leaves; return eawr::core::Result<void>::success(); }
    eawr::core::Result<void> send_chat(std::string) override { ++sends; return eawr::core::Result<void>::success(); }
    eawr::core::Result<OnlineChatPage> chat(std::uint32_t after) override {
        if (refuse_chat || lose_chat_membership.exchange(false)) return eawr::core::Result<OnlineChatPage>::failure(eawr::net::relay::error(1548, "Member removed"));
        OnlineChatPage page; page.through = 1; page.members = {{1, "Host"}, {2, "Guest"}};
        if (after == 0) page.lines.push_back({1, 2, "Guest", "Hello"});
        return eawr::core::Result<OnlineChatPage>::success(std::move(page));
    }
    eawr::core::Result<void> kick(const OnlineRoomPublication&, std::uint32_t) override { return eawr::core::Result<void>::success(); }
};
void fixtures() {
    FixedOnlineLobbySource source;
    auto visible = metadata(), starting = metadata(); starting.starting = true;
    source.set_rooms({{std::string(32, 'a'), visible, true}, {std::string(32, 'b'), starting, false}});
    expect(source.pump() && !source.pump() && source.games().size() == 1 && source.games()[0].passworded, "Fixed source lists protected rooms and excludes starting rooms");
    source.set_members("Room", {{1, "Host"}, {2, "Guest"}});
    expect(source.players().size() == 2 && source.joining("Room").size() == 1 && source.joining("Other").empty(), "Fixed source supplies room-scoped roster");
    source.queue_chat({LobbyChatScope::game, "Room", "Guest\n", "Hi\n"});
    auto lines = source.take_chat(); expect(lines.size() == 1 && lines[0].name == "Guest" && lines[0].text == "Hi" && source.take_chat().empty(), "Fixture chat sanitized and delivered once");
    source.send_chat(LobbyChatScope::lobby, "", "Guest", "No global channel"); expect(source.sent().empty(), "Fixture has no global chat");
}
void fixed_password_rooms() {
    FixedOnlineLobbySource source;
    auto open_data = metadata(), locked_data = metadata(), full_data = metadata();
    open_data.name = "Open"; locked_data.name = "Locked"; full_data.name = "Full";
    full_data.capacity = 2; full_data.occupancy = 2;
    const std::string open_id(32, 'a'), locked_id(32, 'b'), full_id(32, 'c');
    source.set_rooms({{open_id, open_data, false}, {locked_id, locked_data, true}, {full_id, full_data, true}});
    source.set_room_password(locked_id, "secret");
    std::uint64_t generation = 0;
    const auto join = [&](const std::string& room, const std::string& password, const std::string& name = "Guest") {
        const LobbyAttemptId attempt{LobbySourceKind::online, ++generation};
        source.begin_join(attempt, room, password, name);
        auto results = source.take_join_results();
        expect(results.size() == 1 && results[0].attempt == attempt, "A join yields exactly one completion with its own attempt");
        return std::move(results[0]);
    };
    expect(join(open_id, "").admission.has_value(), "A public room admits without a password");
    expect(join(locked_id, "wrong").refusal->code == "EAWR-NET-1546", "A wrong password is refused with 1546");
    expect(join(locked_id, "").refusal->code == "EAWR-NET-1546", "An empty password for a private room is a wrong password");
    expect(join(locked_id, "wrong").refusal->code == "EAWR-NET-1547", "The third wrong password in a row starts the cooldown (1547)");
    expect(join(locked_id, "secret").refusal->code == "EAWR-NET-1547", "The cooldown refuses even the right password");
    source.clear_cooldown();
    const auto admitted = join(locked_id, "secret");
    expect(admitted.admission && admitted.admission->room == locked_id && admitted.admission->seat == 2, "The right password admits to a vacant seat");
    expect(join(std::string(32, 'd'), "").refusal->code == "EAWR-NET-1544", "An unknown room is unavailable (1544)");
    expect(join(full_id, "x").refusal->code == "EAWR-NET-1545", "A full room is refused before the password (1545)");
    expect(join(open_id, "", "   ").refusal->code == "EAWR-NET-1540", "A blank guest name is refused locally");
    const LobbyAttemptId lan{LobbySourceKind::lan, 900};
    source.begin_join(lan, open_id, "", "Guest");
    expect(source.take_join_results().front().refusal->code == "EAWR-NET-1540", "A LAN attempt id cannot join an online room");
    for (const auto& notice : source.take_notices()) {
        expect(notice.message.find("secret") == std::string::npos && notice.message.find("wrong") == std::string::npos,
               "Join notices never carry the typed password");
    }
    const LobbyAttemptId cancelled{LobbySourceKind::online, 901};
    source.begin_join(cancelled, open_id, "", "Guest"); source.cancel(cancelled);
    expect(source.take_join_results().empty(), "A cancelled attempt has no completion");

    expect(source.hosting_state() == OnlineHostingState::idle, "A fixture starts idle");
    LobbyPresence presence; presence.place = LobbyPlace::hosting; presence.name = "Host"; presence.game = "Mine"; presence.passworded = true;
    source.set_host_password("private"); source.announce(presence);
    expect(source.hosting_state() == OnlineHostingState::hosted && source.host_password() == "private" && source.announced().passworded,
           "Hosting a private room publishes it with the password handed over");
    presence.place = LobbyPlace::lobby; source.announce(presence);
    expect(source.hosting_state() == OnlineHostingState::idle, "Leaving the room unlists it");
    source.set_hosting_state(OnlineHostingState::retrying);
    expect(source.hosting_state() == OnlineHostingState::retrying, "A fixture can script the re-publishing state");

    (void)source.pump();
    source.refresh();
    expect(source.refreshes() == 1 && source.pump() && !source.pump(), "Refresh is counted and marks the list changed once");
    source.fail_listing(true);
    expect(source.games().empty() && source.last_error() && source.last_error()->code == "EAWR-NET-1552", "A failed listing shows no rooms and a typed error");
    source.fail_listing(false);
    expect(source.games().size() == 3 && !source.last_error(), "A recovered listing shows its rooms again");
}
void live_source() {
    auto service = std::make_unique<FakeService>(); auto* fake = service.get();
    OnlineLobbyGameSource source(std::move(service), metadata());
    until([&] { source.pump(); return !source.games().empty(); });
    { const auto before = fake->lists.load(); source.refresh(); until([&] { return fake->lists > before; }); }
    const auto rows = source.games(); expect(rows[0].address == std::string(32, 'a') && rows[0].passworded && !rows[0].version_matches, "Directory rows bind to shared UI with local compatibility tags");
    LobbyPresence presence; presence.place = LobbyPlace::hosting; presence.name = "Host"; presence.game = "Room";
    presence.map = "custom-map"; presence.game_type = "custom-mode"; presence.max_players = 9; presence.passworded = true;
    source.set_host_password("private"); source.announce(presence); until([&] { return fake->publishes == 1; });
    until([&] { source.pump(); return source.players().size() == 2; });
    expect(source.joining("Room").size() == 1, "Online source supplies safe room roster");
    auto lines = source.take_chat(); expect(lines.size() == 1 && lines[0].game == "Room" && source.take_chat().empty(), "Online chat routed only to joined room");
    source.send_chat(LobbyChatScope::lobby, "", "Spoof", "No"); source.send_chat(LobbyChatScope::game, "Other", "Spoof", "No");
    source.send_chat(LobbyChatScope::game, "Room", "Spoof", "Hello"); until([&] { return fake->sends == 1; });
    presence.locked = true; source.announce(presence); until([&] { return fake->heartbeats == 1; });
    { std::lock_guard lock(fake->mutex); expect(fake->published.starting && fake->published.map == "custom-map", "Presence updates keep data-driven map and hide starting room"); }
    presence.place = LobbyPlace::lobby; source.announce(presence); until([&] { return fake->leaves == 1; });
}
void destruction_drains_off_frame() {
    auto gate = std::make_shared<BlockingList>();
    auto service = std::make_unique<FakeService>(); service->list_gate = gate;
    auto source = std::make_unique<OnlineLobbyGameSource>(std::move(service), metadata());
    until([&] { return gate->entered.load(); });
    std::atomic<bool> frame_returned{};
    std::jthread frame([owned = std::move(source), &frame_returned]() mutable { owned.reset(); frame_returned = true; });
    struct Release { std::shared_ptr<BlockingList> gate; ~Release() { gate->released = true; } } release{gate};
    until([&] { return frame_returned.load(); });
    expect(!gate->released && !gate->destroyed, "frame destruction returns while the owned service call is still gated");
    gate->released = true;
    until([&] { return gate->destroyed.load(); });
}

void bridge_publication_waits() {
    auto service = std::make_unique<FakeService>(); auto* fake = service.get();
    OnlineLobbyGameSource source(std::move(service), metadata(), true);
    LobbyPresence presence; presence.place = LobbyPlace::hosting; presence.name = "Host"; presence.game = "Room";
    source.announce(presence); until([&] { return source.hosting_state() == OnlineHostingState::publishing; });
    expect(fake->publishes == 0, "live room waits for the signalling publication before becoming joinable");
    source.set_bridge_invitation(std::string(32, 'a')); until([&] { return fake->publishes == 1; });
    { std::lock_guard lock(fake->mutex); expect(fake->bridge == std::string(32, 'a'), "invitation travels only to the service publication"); }
    for (const auto& row : source.games()) expect(row.name.find(std::string(32, 'a')) == std::string::npos, "invitation is not display metadata");
}

void cancellation() {
    auto service = std::make_unique<FakeService>(); auto* fake = service.get(); fake->hold_join = true;
    OnlineLobbyGameSource source(std::move(service), metadata());
    const LobbyAttemptId attempt{LobbySourceKind::online, 1};
    source.begin_join(attempt, std::string(32, 'a'), "private", "Guest"); until([&] { return fake->joins == 1; });
    // The service is still blocked here. Every frame API and cancellation must
    // return without waiting for that network call or taking its service lock.
    source.pump(); (void)source.games(); (void)source.players(); (void)source.take_chat(); source.cancel(attempt);
    expect(source.take_join_results().empty(), "Cancelled attempt has no completion before release");
    { std::lock_guard lock(fake->mutex); fake->release_join = true; } fake->condition.notify_all();
    until([&] { return fake->leaves == 1; });
    expect(source.take_join_results().empty(), "Late successful admission released and stale callback discarded");
    const LobbyAttemptId next{LobbySourceKind::online, 2}; source.begin_join(next, std::string(32, 'a'), "private", "Guest");
    std::vector<OnlineLobbyJoinCompletion> results;
    until([&] { results = source.take_join_results(); return !results.empty(); });
    expect(results.size() == 1 && results[0].attempt == next && results[0].admission.has_value(), "A fresh generation can join after cancellation");
    source.cancel(next); until([&] { return fake->leaves == 2; });
}
void recovery_and_validation() {
    auto service = std::make_unique<FakeService>(); auto* fake = service.get(); fake->fail_publish = 1;
    OnlineLobbyGameSource source(std::move(service), metadata());
    LobbyPresence presence; presence.place = LobbyPlace::hosting; presence.name = "Host with trailing  ";
    presence.game = "Skirmish night at the Core Worlds"; presence.map = "custom-map"; presence.game_type = "custom-mode"; presence.max_players = 9;
    source.announce(presence);
    until([&] { return source.hosting_state() == OnlineHostingState::retrying; });
    expect(fake->publishes == 1, "Failed publication enters retry state");
    until([&] { return fake->publishes == 2 && source.hosting_state() == OnlineHostingState::hosted; });
    expect(!source.last_error(), "Success clears prior error");
    { std::lock_guard lock(fake->mutex); expect(fake->published.name.size() == 24 && fake->published.host_name == "Host with trailing", "Source normalizes once before publication/renewal"); }
    source.set_host_password("change");
    auto notices = source.take_notices();
    expect(std::any_of(notices.begin(), notices.end(), [](const auto& d) { return d.code == "EAWR-NET-1554"; }), "Password changes while published produce typed notice");
    fake->lose_chat_membership = true;
    until([&] { return fake->heartbeats >= 1 && source.hosting_state() == OnlineHostingState::hosted; });
    expect(fake->publishes == 2, "A host reattaches a lost directory channel with its still-valid publication");
    fake->fail_heartbeat = true; presence.locked = true; source.announce(presence);
    until([&] { return fake->publishes == 3 && source.hosting_state() == OnlineHostingState::hosted; });
    expect(fake->heartbeats == 2, "Expired lease drops stale publication and re-publishes next tick");
    presence.place = LobbyPlace::lobby; source.announce(presence); until([&] { return source.hosting_state() == OnlineHostingState::idle; });
    const LobbyAttemptId invalid{LobbySourceKind::online, 10}; source.begin_join(invalid, "bad", "", "Guest");
    const auto refused = source.take_join_results(); expect(refused.size() == 1 && refused[0].refusal && refused[0].attempt == invalid, "Invalid begin_join has a refused completion");
    const LobbyAttemptId valid{LobbySourceKind::online, 11}; source.begin_join(valid, std::string(32, 'a'), "", "Guest");
    std::vector<OnlineLobbyJoinCompletion> results; until([&] { results = source.take_join_results(); return !results.empty(); });
    expect(results[0].admission.has_value(), "A valid attempt follows local refusal");
    until([&] { return !source.players().empty(); }); const auto previous_leaves = fake->leaves.load(); fake->refuse_chat = true;
    until([&] { source.pump(); return source.players().empty() && fake->leaves == previous_leaves + 1; });
    notices = source.take_notices(); expect(std::any_of(notices.begin(), notices.end(), [](const auto& d) { return d.code == "EAWR-NET-1548"; }), "Removed membership clears roster and queues notice");
    source.send_chat(LobbyChatScope::game, "Room", "Guest", "stale"); expect(fake->sends == 0, "Cleared membership cannot queue room chat");
}

}
int main() {
    try { fixtures(); fixed_password_rooms(); live_source(); destruction_drains_off_frame(); bridge_publication_waits(); cancellation(); recovery_and_validation(); std::cout << "Online lobby source and cancellation contracts passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
