#include "lan_match.hpp"
#include "online_match.hpp"
#include "match_transport.hpp"
#include "skirmish_prepare.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/platform/publish_files.hpp"
#include "eawr/net/desync_monitor.hpp"
#include "eawr/net/setup.hpp"
#include "eawr/net/lan_credentials.hpp"
#include "eawr/net/tactical_input.hpp"
#include "eawr/net/tcp_transport.hpp"
#include "eawr/presentation/ui/time_controls.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <deque>
#include <fstream>
#include <future>
#include <iomanip>
#include <sstream>
#include <locale>
#include <cctype>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <set>
#include <utility>

namespace eawr::presentation::godot_backend {
namespace {
namespace tactical = sim::tactical;
using Bytes = std::vector<std::uint8_t>;
using DriverResult = core::Result<bool>;
std::string_view command_kind(const tactical::CommandPayload& payload) {
    if (std::holds_alternative<tactical::MovePayload>(payload)) return "move";
    if (std::holds_alternative<tactical::AttackPayload>(payload)
        || std::holds_alternative<tactical::AttackMovePayload>(payload)) return "attack";
    if (std::holds_alternative<tactical::AbilityPayload>(payload)
        || std::holds_alternative<tactical::AreaAbilityPayload>(payload)) return "ability";
    if (std::holds_alternative<tactical::QuitPayload>(payload)) return "quit";
    return "other";
}
struct InputTime { std::uint64_t tick{}, frame{}; std::int64_t epoch{}, steady{}; std::uint64_t ui_epoch{}; };
InputTime input_time(std::uint64_t tick, std::uint64_t frame = 0) {
    using namespace std::chrono;
    return {tick, frame, duration_cast<microseconds>(system_clock::now().time_since_epoch()).count(),
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()};
}
class LanTrace {
public:
    explicit LanTrace(const std::filesystem::path& path) {
        if (path.empty()) return;
        output_.open(path);
        if (!output_) { failed_ = true; return; }
        output_ << "event,seat,sequence,kind,tick,frame,epoch_us,steady_us,ui_epoch,target_tick,seal_frontier\n";
        output_.flush();
        if (!output_) failed_ = true;
    }
    bool failed() const { return failed_.load(); }
    void note(std::uint64_t tick, std::uint64_t frame, std::uint64_t ui_epoch) {
        if (!output_.is_open()) return;
        const std::lock_guard lock(mutex_); marker_ = input_time(tick, frame); marker_->ui_epoch = ui_epoch; native_entry_.reset();
    }
    void native_input(std::uint64_t tick, std::uint64_t epoch) {
        if (!output_.is_open()) return;
        const std::lock_guard lock(mutex_);
        native_entry_ = input_time(tick); native_entry_->ui_epoch = epoch;
    }
    void issue(std::uint64_t sequence, std::uint64_t epoch) {
        if (!output_.is_open()) return;
        const std::lock_guard lock(mutex_);
        if (marker_) { auto time = input_time(marker_->tick, marker_->frame); time.ui_epoch = epoch;
            issued_.emplace(sequence, Issued{*marker_, time, native_entry_}); marker_.reset(); }
    }
    void submitted(sim::SeatCommandKey key, std::string_view kind, std::uint64_t tick, std::uint64_t frontier,
        std::uint64_t sequence, std::uint64_t epoch) {
        if (!output_.is_open()) return;
        const std::lock_guard lock(mutex_);
        if (const auto found = issued_.find(sequence); found != issued_.end()) {
            write("input", key, kind, found->second.input);
            write("issue", key, kind, found->second.issue);
            if (found->second.native_entry) write("native_entry", key, kind, *found->second.native_entry);
            issued_.erase(found);
        }
        auto time = input_time(tick); time.ui_epoch = epoch;
        write("submit", key, kind, time, frontier);
    }
    void stage(std::string_view event, sim::SeatCommandKey key, std::string_view kind, std::uint64_t tick) {
        if (!output_.is_open()) return;
        const std::lock_guard lock(mutex_); write(event, key, kind, input_time(tick));
    }
    void executed(sim::SeatCommandKey key, std::string_view kind, std::uint64_t tick) {
        if (!output_.is_open()) return;
        const std::lock_guard lock(mutex_); write("execute", key, kind, input_time(tick));
    }
private:
    void write(std::string_view event, sim::SeatCommandKey key, std::string_view kind, InputTime time, std::uint64_t frontier = 0) {
        if (!output_.is_open()) return;
        output_ << event << ',' << key.seat.value << ',' << key.sequence << ',' << kind << ',' << time.tick << ','
            << time.frame << ',' << time.epoch << ',' << time.steady << ',' << time.ui_epoch << ',' << key.tick << ',' << frontier << '\n';
        output_.flush();
        if (!output_) failed_ = true;
    }
    std::atomic<bool> failed_{false};
    std::mutex mutex_;
    std::ofstream output_;
    std::optional<InputTime> marker_;
    struct Issued { InputTime input, issue; std::optional<InputTime> native_entry; };
    std::optional<InputTime> native_entry_;
    std::map<std::uint64_t, Issued> issued_;
};
core::Diagnostic diagnostic(std::string code, std::string message) {
    core::Diagnostic result; result.code = std::move(code); result.message = std::move(message); return result;
}
template<class T> core::Result<T> fail(std::string message) {
    return core::Result<T>::failure(diagnostic("EAWR-NET-0600", std::move(message)));
}
template<class T> core::Result<T> lobby_fail(net::LobbyReason reason, std::string message) {
    return core::Result<T>::failure(net::lobby_diagnostic(reason, std::move(message)));
}
core::Result<skirmish::FixtureOptions> fixture_options(const net::SetupOffer& offer, std::span<const std::string> factions) {
    auto decoded = net::decode_skirmish_setup(offer.setup);
    if (!decoded) return core::Result<skirmish::FixtureOptions>::failure(decoded.error());
    const auto& setup = decoded.value();
    skirmish::FixtureOptions result;
    result.map = setup.skirmish->map;
    result.seed = offer.seed;
    result.match = setup.skirmish->match;
    result.victory_condition = static_cast<tactical::VictoryCondition>(setup.skirmish->victory_condition);
    result.slots.emplace();
    for (const auto& slot : setup.skirmish->slots) {
        const auto player = std::find_if(setup.players.begin(), setup.players.end(),
            [&](const auto& value) { return value.player_id == slot.player; });
        if (player == setup.players.end()) return fail<skirmish::FixtureOptions>("Offered lobby player is missing.");
        const auto faction = std::find_if(factions.begin(), factions.end(), [&](const auto& name) {
            return skirmish::faction_id(name) == player->faction_id;
        });
        if (faction == factions.end()) return fail<skirmish::FixtureOptions>("Offered faction is unavailable in the loaded data.");
        skirmish::LobbySlot lobby{slot.player, *faction, player->team_id, slot.human, slot.fleet};
        lobby.colour_index = slot.colour_index;
        result.slots->push_back(std::move(lobby));
    }
    return core::Result<skirmish::FixtureOptions>::success(std::move(result));
}
// Owns ordered frames read by the setup screen. After handover it is exclusively
// polled by the simulation thread, including during loading, stalls and results.
class BufferedTransport final : public net::MessageTransport {
public:
    std::unique_ptr<MatchTransport> tcp;
    std::deque<net::Received> pending;
    std::size_t pending_bytes{};
    std::uint64_t sent{}, received{}, batches_sent{}, batches_received{};
    bool count_progress{};
    std::shared_ptr<net::LanReconnectCredentials> credentials;
    std::optional<core::Diagnostic> credential_error;
    core::Result<void> send(net::PeerIndex peer, std::span<const std::uint8_t> bytes) override {
        auto result = tcp->send(peer, bytes);
        if (result && count_progress) { ++sent; if (!bytes.empty() && bytes.front() == net::sealed_batch_kind) ++batches_sent; }
        return result;
    }
    std::optional<net::Received> poll() override {
        return poll_impl(true);
    }
    std::optional<net::Received> poll_socket() { return poll_impl(false); }
    std::optional<net::Received> poll_impl(bool queued) {
        for (unsigned attempt = 0; attempt < 256; ++attempt) {
            std::optional<net::Received> next;
            if (!queued || pending.empty()) next = tcp->poll();
            else { next = std::move(pending.front()); pending.pop_front(); pending_bytes -= std::min(pending_bytes, next->frame.size()); }
            if (next && !next->frame.empty() && (next->frame[0] == net::lan_credentials_kind || next->frame[0] == net::lan_credentials_ack_kind)) {
                if (!credentials) {
                    net::erase_reconnect_secret(next->frame);
                    credential_error = diagnostic("EAWR-NET-1025", "Reconnect credential provider is unavailable.");
                    return {};
                }
                auto consumed = credentials->receive(*next, *this);
                if (!consumed) { credential_error = consumed.error(); return {}; }
                continue;
            }
            if (queued && next && count_progress) { ++received; if (!next->frame.empty() && next->frame.front() == net::sealed_batch_kind) ++batches_received; }
            return next;
        }
        credential_error = diagnostic("EAWR-NET-1026", "Reconnect credential receive frame limit exceeded.");
        return {};
    }
    net::LinkState link(net::PeerIndex peer) const override { return tcp->link(peer); }
    net::PeerIndex self() const override { return tcp->self(); }
    std::uint32_t peer_count() const override { return tcp->peer_count(); }
    std::optional<net::LinkStats> link_stats(net::PeerIndex peer) const override { return tcp->link_stats(peer); }
};
// This stable owner keeps the monitor's transport reference valid when a stopped
// match hands its remaining I/O to a drain thread. No live-world callbacks escape.
struct TerminalIo {
    BufferedTransport transport;
    std::shared_ptr<void> room_lifetime;
    net::MessageTransport* application_stream{};
    std::unique_ptr<net::DesyncMonitor> monitor;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    bool closing{};
    core::Result<void> pump() {
        if (!transport.tcp) return core::Result<void>::success();
        const auto now = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 256; ++i) {
            auto frame = application_stream ? application_stream->poll() : transport.poll();
            if (!frame) break;
            if (monitor && !frame->frame.empty() &&
                (frame->frame.front() == net::desync_combined_kind || frame->frame.front() == net::desync_rows_kind)) {
                auto received = monitor->receive(*frame);
                if (!received) return received;
            }
        }
        // The final summary can trigger a row exchange. Queue close only after
        // that exchange finishes, so every response precedes the close frame.
        const bool expired = deadline && now >= *deadline;
        if (!closing && (!monitor || monitor->waiting_for_peers().empty() || expired)) {
            transport.tcp->close(); closing = true;
        }
        // close() only queues the ordered marker. Flush it even on the final
        // deadline pump before releasing the stream.
        if (expired) (void)transport.tcp->poll();
        if (transport.tcp->closed() || expired) {
            transport.tcp.reset(); transport.pending.clear();
        }
        return core::Result<void>::success();
    }
};
class DrainWorkers {
public:
    ~DrainWorkers() { shutdown(); }
    void start(std::unique_ptr<net::PortMapping> mapping) {
        const std::lock_guard lock(mutex_);
        // A match can release its mapping after the explicit shutdown drain.
        // Keep late work owned here too; final registry destruction joins it
        // before extension unload, while this caller returns without router waits.
        std::erase_if(workers_, [](const Worker& worker) { return worker.finished->load(); });
        auto finished = std::make_shared<std::atomic_bool>(false);
        workers_.push_back({finished, std::jthread([mapping = std::move(mapping), finished]() mutable {
            // Mapping destruction requests stop and waits for bounded router cleanup.
            // Keep that wait off the UI thread, just like the terminal stream drain.
            mapping.reset();
            finished->store(true);
        })});
    }
    void start(std::function<void()> work) {
        const std::lock_guard lock(mutex_);
        std::erase_if(workers_, [](const Worker& worker) { return worker.finished->load(); });
        auto finished = std::make_shared<std::atomic_bool>(false);
        workers_.push_back({finished, std::jthread([work = std::move(work), finished] { work(); finished->store(true); })});
    }
    void start(std::unique_ptr<TerminalIo> io) {
        const std::lock_guard lock(mutex_);
        if (shutting_down_) return;
        // Only completed threads are joined during normal UI cleanup. Their I/O
        // has finished; the join synchronizes the worker's final return.
        std::erase_if(workers_, [](const Worker& worker) { return worker.finished->load(); });
        auto finished = std::make_shared<std::atomic_bool>(false);
        workers_.push_back({finished, std::jthread([io = std::move(io), finished] {
            while (io->transport.tcp) {
                if (!io->pump()) break;
                if (io->transport.tcp) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            finished->store(true);
        })});
    }
    void shutdown() {
        std::vector<Worker> draining;
        {
            const std::lock_guard lock(mutex_);
            shutting_down_ = true;
            draining.swap(workers_);
        }
        // jthread joins outside the registry lock. This runs only after live
        // drivers stop, before the extension unloads, never on a HUD exit path.
    }
private:
    struct Worker {
        std::shared_ptr<std::atomic_bool> finished;
        std::jthread thread;
    };
    std::mutex mutex_;
    bool shutting_down_{};
    std::vector<Worker> workers_;
};
DrainWorkers& drain_workers() { static DrainWorkers workers; return workers; }
void finish_io(std::unique_ptr<TerminalIo> io) {
    if (io->transport.tcp) {
        // Only teardown has a bounded drain. An ended match still owns its I/O
        // and must await the peer's final exchange without an elapsed-time cut-off.
        // Online GNS callbacks share a process I/O owner; teardown stays on it.
        if (io->transport.tcp->online()) { io->transport.tcp->close(); (void)io->transport.tcp->poll(); return; }
        io->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        drain_workers().start(std::move(io));
    }
}
// Gate advancement without bypassing the core receive path while presentation holds.
class GatedMonitor final : public net::SessionMonitor {
public:
    net::DesyncMonitor* monitor{};
    bool advance_allowed{};
    bool needs_encoding(std::uint64_t tick, bool end = false) const override { return monitor->needs_encoding(tick, end); }
    core::Result<void> record(std::uint64_t tick, std::string_view hash, std::span<const std::uint8_t> state,
        std::span<const std::uint8_t> input, bool end = false) override { return monitor->record(tick, hash, state, input, end); }
    core::Result<void> receive(const net::Received& frame) override { return monitor->receive(frame); }
    core::Result<void> on_departure(net::PeerIndex peer, std::uint64_t tick) override { return monitor->on_departure(peer, tick); }
    bool ready_for_resync() const override { return monitor->ready_for_resync(); }
    core::Result<void> on_resync(std::uint64_t tick, std::string_view hash) override { return monitor->on_resync(tick, hash); }
    core::Result<void> on_reconnect(std::uint64_t tick, std::string_view hash, std::span<const net::PeerIndex> inactive) override {
        return monitor->on_reconnect(tick, hash, inactive);
    }
    bool may_step() const override { return advance_allowed && monitor->may_step(); }
    bool frozen() const noexcept override { return monitor->frozen(); }
    std::vector<net::PeerIndex> waiting_for_peers() const override { return monitor->waiting_for_peers(); }
};
class LiveAdapter final : public net::SessionAdapter {
public:
    explicit LiveAdapter(tactical::TacticalSetup setup, std::vector<net::SetupSeat> seats, std::shared_ptr<LanTrace> trace)
        : setup_(std::move(setup)), trace_(std::move(trace)) {
        for (const auto& seat : seats) bindings_.push_back({seat.seat, seat.player, true, false});
    }
    platform::LiveStepContext* context{};
    std::shared_ptr<platform::LiveStepContext> staging;
    std::function<core::Result<void>(LiveAdapter&)> activate_if_current;
    std::function<core::Result<void>()> service_io;
    bool advanced{};
    std::function<void(net::MatchOptions, std::uint64_t)> options_applied;
    void bind_live_context(platform::LiveStepContext& live, const std::shared_ptr<LanTrace>& trace) {
        context = &live; trace_ = trace;
    }
    std::uint64_t completed_tick() const override { return context ? context->completed_tick() : 0; }
    std::string state_hash() const override { return context->state_hash(); }
    Bytes canonical_state() const override { return context->canonical_state(); }
    const tactical::SeatBinding* binding(sim::SeatId seat) const {
        const auto found = std::find_if(bindings_.begin(), bindings_.end(), [&](const auto& bound) { return bound.seat == seat; });
        return found == bindings_.end() ? nullptr : &*found;
    }
    core::Result<void> validate(const net::SeatInput& input) const override {
        const auto* bound = binding(input.key.seat);
        if (!bound) return fail<void>("Unknown player seat.");
        if (context->departures) for (const auto& control : context->departures())
            if (control.player == bound->player) return fail<void>("This human seat has departed.");
        if (net::is_match_options(input.bytes)) {
            auto decoded = net::decode_match_options(input.bytes);
            return decoded ? core::Result<void>::success() : core::Result<void>::failure(decoded.error());
        }
        auto decoded = net::decode_tactical_input(input, *bound);
        return decoded ? core::Result<void>::success() : core::Result<void>::failure(decoded.error());
    }
    core::Result<void> install(std::span<const net::SeatInput> canonical) override {
        if (staging && activate_if_current) {
            auto activated = activate_if_current(*this); if (!activated) return activated;
        }
        std::vector<tactical::PlayerCommand> commands;
        trace_commands_.clear();
        options_input_.clear();
        pending_options_.reset();
        for (const auto& input : canonical) {
            const auto* bound = binding(input.key.seat);
            if (!bound) return fail<void>("Unknown player seat.");
            if (net::is_match_options(input.bytes)) {
                auto decoded = net::decode_match_options(input.bytes);
                if (!decoded) return core::Result<void>::failure(decoded.error());
                pending_options_ = decoded.value();
                options_input_.insert(options_input_.end(), input.bytes.begin(), input.bytes.end());
                continue;
            }
            auto decoded = net::decode_tactical_input(input, *bound);
            if (!decoded) return core::Result<void>::failure(decoded.error());
            auto command = tactical::admit_human_command(*bound, decoded.value());
            if (!command) return core::Result<void>::failure(command.error());
            if (trace_) {
                trace_commands_.emplace_back(input.key, command_kind(command.value().command.payload));
                trace_->stage("admit", input.key, command_kind(command.value().command.payload), context->completed_tick());
            }
            if (std::holds_alternative<tactical::QuitPayload>(command.value().command.payload)) {
                departed_.insert(bound->seat);
                if (std::any_of(bindings_.begin(), bindings_.end(), [&](const auto& seat) {
                    return seat.player == bound->player && seat.active && !seat.observer && !departed_.contains(seat.seat);
                })) continue;
            }
            command.value().command.key.sequence = sequences_[bound->player]++;
            commands.push_back(std::move(command).value().command);
        }
        std::sort(commands.begin(), commands.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
        inputs_ = commands;
        return context->install(commands);
    }
    core::Result<Bytes> canonical_input() const override {
        if (!options_input_.empty()) {
            auto tactical_input = context->canonical_input ? context->canonical_input()
                : inputs_.empty() ? core::Result<Bytes>::success({}) : tactical::write_replay({setup_, completed_tick() + 1, inputs_});
            if (!tactical_input) return tactical_input;
            auto bytes = options_input_;
            bytes.insert(bytes.end(), tactical_input.value().begin(), tactical_input.value().end());
            return core::Result<Bytes>::success(std::move(bytes));
        }
        if (context->canonical_input) return context->canonical_input();
        if (inputs_.empty()) return core::Result<Bytes>::success({});
        return tactical::write_replay({setup_, completed_tick() + 1, inputs_});
    }
    core::Result<net::CompletedTick> step() override {
        if (staging && activate_if_current) { auto activated = activate_if_current(*this); if (!activated) return core::Result<net::CompletedTick>::failure(activated.error()); }
        if (staging && service_io) { auto serviced = service_io(); if (!serviced) return core::Result<net::CompletedTick>::failure(serviced.error()); }
        if (!context->advance_allowed) return fail<net::CompletedTick>("Loading barrier attempted to advance.");
        auto result = context->step();
        if (!result) return core::Result<net::CompletedTick>::failure(result.error());
        if (pending_options_) {
            if (context->set_target_rate) context->set_target_rate(presentation::ui::speed_step_rates[pending_options_->tactical_speed]);
            if (!staging && options_applied) options_applied(*pending_options_, context->completed_tick() - 1);
            pending_options_.reset();
        }
        for (const auto& [key, kind] : trace_commands_) trace_->executed(key, kind, context->completed_tick());
        advanced = !staging;
        return core::Result<net::CompletedTick>::success({context->completed_tick(), context->state_hash()});
    }
    bool is_departure(const net::SeatInput& input) const override {
        const auto* bound = binding(input.key.seat);
        if (!bound) return false;
        auto decoded = net::decode_tactical_input(input, *bound);
        return decoded && std::holds_alternative<tactical::QuitPayload>(decoded.value().payload);
    }
private:
    tactical::TacticalSetup setup_;
    std::vector<tactical::SeatBinding> bindings_;
    std::set<sim::SeatId> departed_;
    std::map<tactical::PlayerId, std::uint64_t> sequences_;
    std::vector<tactical::PlayerCommand> inputs_;
    Bytes options_input_;
    std::optional<net::MatchOptions> pending_options_;
    std::shared_ptr<LanTrace> trace_;
    std::vector<std::pair<sim::SeatCommandKey, std::string_view>> trace_commands_;
};
class TcpLanMatch final : public LanMatch, public std::enable_shared_from_this<TcpLanMatch> {
public:
    void set_recovery_policy(net::SetupRecoveryPolicy policy, LanCredentialProvider credentials = {}) override {
        if (handed_over_) return;
        recovery_policy_ = policy; credentials_ = std::move(credentials);
        default_credentials_ = false;
        if (!credentials_) recovery_policy_.reconnect = false;
        { const std::lock_guard lock(mutex_);
          view_.credential_provider_available = static_cast<bool>(credentials_);
          view_.reconnect_enabled = recovery_policy_.reconnect;
          view_.reconnect_unavailable = recovery_policy_.reconnect ? "" : "Reconnect is disabled for this match.";
        }
    }
    bool rejoin() override {
        const std::lock_guard lock(mutex_);
        if (!view_.rejoin_enabled || rejoin_requested_) return false;
        rejoin_requested_ = true; return true;
    }
    bool request_speed(std::uint32_t step) override {
        const std::lock_guard lock(mutex_);
        if (!lan_gameplay_enabled(view_) || step >= presentation::ui::speed_step_rates.size()) return false;
        const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if (!speed_window_.accept(now)) return false;
        auto options = view_.options;
        options.tactical_speed = step;
        speed_requests_.push_back(options); view_.input_feedback.clear();
        return true;
    }
    explicit TcpLanMatch(std::filesystem::path root, std::uint64_t delay, bool smoke_progress, std::filesystem::path trace)
        : diagnostic_root_(std::move(root)), input_delay_(delay), smoke_progress_(smoke_progress),
          trace_(trace.empty() ? nullptr : std::make_shared<LanTrace>(trace)) {
        io_->transport.count_progress = smoke_progress;
        if (diagnostic_root_.empty()) diagnostic_root_ = std::filesystem::path("out") / "lan-desync";
        net::SetupRecoveryPolicy policy; policy.reconnect = policy.resync = true;
        set_recovery_policy(policy, [this](const net::SetupOffer& offer) {
            return credential_state_->provide(offer, io_->transport);
        });
        default_credentials_ = true;
    }
    bool lobby_enabled() const override { return static_cast<bool>(lobby_builder_); }
    void configure_lobby(std::string name, std::uint32_t colour, std::vector<skirmish::SetupMap> maps, LanLobbyBuilder builder) override {
        // This staging slice has no recovery provisioning; preserve explicit caller policies for typed refusal.
        if (default_credentials_) set_recovery_policy({});
        lobby_name_ = net::sanitize_lobby_text(name, name.size()); lobby_colour_ = colour; lobby_maps_ = std::move(maps); lobby_builder_ = std::move(builder);
    }
    bool lobby_select(net::LobbySelection selection) override {
        if (!lobby_mutable()) return false;
        selection.name = net::sanitize_lobby_text(selection.name, selection.name.size());
        const auto local = std::find_if(lobby_->snapshot().seats.begin(), lobby_->snapshot().seats.end(), [&](const auto& seat) { return seat.peer == io_->transport.self(); });
        if (local != lobby_->snapshot().seats.end() && selection.slot != local->selection.slot) {
            auto options = decode_lan_lobby_game(lobby_->snapshot()); if (!options) return false;
            const auto row = std::find_if(options.value().slots->begin(), options.value().slots->end(), [&](const auto& slot) { return slot.slot == selection.slot; });
            if (row == options.value().slots->end()) return false;
            selection.faction = row->faction; selection.team = row->team; selection.colour = row->colour_index.value_or(row->slot - 1);
        }
        if (local != lobby_->snapshot().seats.end() && selection.name == local->selection.name && selection.name != lobby_name_) selection.name = lobby_name_;
        const auto requested_name = selection.name;
        auto applied = lobby_->request(net::LobbyAction::select, lobby_->snapshot().revision, std::move(selection));
        if (applied) lobby_name_ = requested_name;
        return lobby_feedback(std::move(applied));
    }
    bool lobby_assign(net::PeerIndex peer, net::LobbySelection selection) override {
        return lobby_mutable() && lobby_feedback(lobby_->assign(peer, selection, selection.slot, lobby_->snapshot().revision));
    }
    bool lobby_game(const skirmish::FixtureOptions& options) override {
        if (!lobby_mutable() || !view().host) return false;
        auto configured = options;
        std::set<sim::tactical::PlayerId> planned;
        for (const auto& seat : lobby_->snapshot().seats) planned.insert(seat.selection.slot);
        for (net::PeerIndex peer = 0; peer < owners_.size(); ++peer)
            if (std::none_of(lobby_->snapshot().seats.begin(), lobby_->snapshot().seats.end(), [&](const auto& seat) { return seat.peer == peer; })) planned.insert(owners_[peer]);
        if (configured.slots) for (auto& row : *configured.slots) row.human = planned.contains(row.slot);
        auto encoded = encode_lan_lobby_game(configured, input_delay_);
        if (!encoded) return lobby_feedback(core::Result<void>::failure(encoded.error()));
        auto assignments = lobby_->snapshot().seats;
        for (auto& seat : assignments) {
            const auto row = std::find_if(options.slots->begin(), options.slots->end(), [&](const auto& slot) { return slot.slot == seat.selection.slot; });
            if (row == options.slots->end()) return false;
            seat.selection.faction = row->faction; seat.selection.team = row->team;
            seat.selection.colour = row->colour_index.value_or(row->slot - 1); seat.player = row->slot;
        }
        return lobby_feedback(lobby_->configure_game(std::move(encoded).value(), std::move(assignments), lobby_->snapshot().revision));
    }
    bool lobby_ready(bool ready) override {
        if (!lobby_mutable()) return false;
        if (ready && lobby_state()->identity_pending) return lobby_feedback(lobby_fail<void>(net::LobbyReason::not_ready, "TEXT_WAITING"));
        if (!ready) {
            ready_pending_ = false;
            return lobby_feedback(lobby_->request(net::LobbyAction::unready, lobby_->snapshot().revision));
        }
        auto options = decode_lan_lobby_game(lobby_->snapshot());
        if (!options) return lobby_feedback(core::Result<void>::failure(options.error()));
        if (auto valid = validate_lobby(lobby_->snapshot(), true); !valid) return lobby_feedback(std::move(valid));
        if (preparing_.valid()) return false;
        auto canonical = encode_lan_lobby_game(options.value()); if (!canonical) return false;
        preparing_game_ = std::move(canonical).value();
        ready_pending_ = true; preparing_revision_ = lobby_->snapshot().revision;
        auto work = std::make_shared<std::packaged_task<core::Result<tactical::TacticalSetup>()>>(
            [builder = lobby_builder_, selected = std::move(options).value()] { return builder(selected); });
        preparing_ = work->get_future();
        drain_workers().start(std::function<void()>([work] { (*work)(); }));
        return true;
    }
    bool lobby_chat(std::string text) override {
        return lobby_mutable() && lobby_feedback(lobby_->chat(std::move(text)));
    }
    bool lobby_kick(net::PeerIndex peer) override {
        return lobby_mutable() && lobby_feedback(lobby_->kick(peer, lobby_->snapshot().revision));
    }
    void lobby_leave() override {
        if (!lobby_mutable()) return;
        (void)(view().host ? lobby_->close() : lobby_->request(net::LobbyAction::leave, lobby_->snapshot().revision));
        // One nonblocking pump flushes ordered notice/leave before cancellation.
        (void)io_->transport.tcp->poll();
    }
    std::optional<LanLobbyState> lobby_state() const override {
        if (handed_over_) return last_lobby_state_;
        if (!lobby_) return {};
        LanLobbyState state; state.snapshot = lobby_->snapshot(); state.chat = lobby_->chat_history(); state.notice = lobby_->notice() ? lobby_->notice() : lobby_notice_;
        state.preparing = preparing_.valid();
        const auto local = std::find_if(state.snapshot.seats.begin(), state.snapshot.seats.end(), [&](const auto& seat) { return seat.peer == lobby_self_; });
        state.local_ready = local != state.snapshot.seats.end() && local->ready;
        state.identity_pending = local != state.snapshot.seats.end() && local->selection.name != lobby_name_;
        state.ready_enabled = !state.preparing && !state.identity_pending && local != state.snapshot.seats.end() && state.snapshot.phase == net::LobbyPhase::staging;
        state.start_enabled = view().host && state.snapshot.seats.size() >= 2 && state.snapshot.phase == net::LobbyPhase::staging
            && std::all_of(state.snapshot.seats.begin(), state.snapshot.seats.end(), [](const auto& seat) { return seat.ready; });
        return state;
    }
private:
    bool lobby_mutable() const { return !handed_over_ && view().phase != LanPhase::ended && lobby_ && io_->transport.tcp; }
    void flush_lobby_closes() {
        if (io_->transport.tcp) for (const auto peer : closing_peers_) (void)io_->transport.tcp->close_peer(peer);
        closing_peers_.clear();
    }
    bool lobby_feedback(core::Result<void> result) {
        if (!result && lobby_) {
            const auto reason = lan_lobby_reason(result.error());
            lobby_notice_ = net::LobbyNotice{lobby_->snapshot().room, lobby_->snapshot().revision, reason, result.error().message};
        }
        return static_cast<bool>(result);
    }
    core::Result<void> validate_lobby(const net::LobbySnapshot& state, bool starting) const {
        auto options = decode_lan_lobby_game(state);
        if (!options) return core::Result<void>::failure(options.error());
        const auto map = std::find_if(lobby_maps_.begin(), lobby_maps_.end(), [&](const auto& value) { return value.path == state.game.map; });
        if (map == lobby_maps_.end() || !map->unavailable.empty()) return lobby_fail<void>(net::LobbyReason::invalid_choice, "TEXT_MAP_NOT_AVAILABLE_TO_PLAY");
        if (auto slots = skirmish::validate_setup_slots(*options.value().slots, map->metadata, false); !slots) return lobby_fail<void>(net::LobbyReason::invalid_choice, slots.error().message);
        auto planned_state = state; planned_state.seats.clear();
        auto planned = decode_lan_lobby_game(planned_state);
        if (!planned) return core::Result<void>::failure(planned.error());
        if (view().host) for (const auto owner : owners_) {
            if (std::none_of(planned.value().slots->begin(), planned.value().slots->end(), [&](const auto& row) { return row.slot == owner; }))
                return lobby_fail<void>(net::LobbyReason::invalid_choice, "A planned human owner has no loaded map row.");
        }
        for (const auto& row : *planned.value().slots) if (row.slot > skirmish::setup_row_count(*map))
            return lobby_fail<void>(net::LobbyReason::invalid_choice, "An owner row is outside the loaded map capacity.");
        std::set<std::uint32_t> colours;
        for (const auto& slot : *options.value().slots) {
            const auto colour = slot.colour_index.value_or(slot.slot - 1);
            if (colour >= skirmish::multiplayer_colour_count || !colours.insert(colour).second)
                return lobby_fail<void>(net::LobbyReason::invalid_choice, "Choose distinct colours from the authored multiplayer palette.");
        }
        std::set<std::string> names;
        std::set<sim::tactical::PlayerId> human_rows;
        for (const auto& seat : state.seats) {
            std::string name = seat.selection.name;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name.empty() || name.size() > presentation::ui::lan_name_max || !names.insert(name).second
                || seat.selection.colour >= skirmish::multiplayer_colour_count || seat.selection.slot == 0 || std::none_of(options.value().slots->begin(), options.value().slots->end(), [&](const auto& row) { return row.slot == seat.selection.slot; }))
                return lobby_fail<void>(net::LobbyReason::invalid_choice, "Invalid or duplicate player choice.");
            if (std::none_of(planned.value().slots->begin(), planned.value().slots->end(), [&](const auto& row) { return row.slot == seat.selection.slot && row.human; }))
                return lobby_fail<void>(net::LobbyReason::invalid_choice, "This owner row is reserved for AI by the host.");
            // Defense in depth: the host roster already limits its planned human rows.
            human_rows.insert(seat.selection.slot);
            if (human_rows.size() > sim::max_match_human_sides)
                return lobby_fail<void>(net::LobbyReason::invalid_choice, "Too many human owner rows.");
            for (const auto& other : state.seats) if (seat.peer != other.peer && seat.selection.slot == other.selection.slot
                && (seat.selection.faction != other.selection.faction || seat.selection.team != other.selection.team || seat.selection.colour != other.selection.colour))
                return lobby_fail<void>(net::LobbyReason::invalid_choice, "Shared human seats must agree on their owner row.");
            const auto team = std::find_if(map->teams.begin(), map->teams.end(), [&](const auto& value) { return value.team == seat.selection.team; });
            if (team == map->teams.end() || std::find(team->factions.begin(), team->factions.end(), seat.selection.faction) == team->factions.end())
                return lobby_fail<void>(net::LobbyReason::invalid_choice, "Faction is unavailable on this team.");
            if (starting && seat.player != seat.selection.slot) return lobby_fail<void>(net::LobbyReason::invalid_choice, "Owner assignment is not committed.");
        }
        if (starting) {
            skirmish::SetupSelection selected; selected.map = *options.value().map; selected.slots = *options.value().slots; selected.match = options.value().match;
            auto built = validated_lan_options(selected, lobby_maps_);
            if (!built) return lobby_fail<void>(net::LobbyReason::invalid_choice, built.error().message);
        }
        return core::Result<void>::success();
    }
    net::LobbyHooks lobby_hooks() {
        net::LobbyHooks hooks;
        hooks.validate = [this](const net::LobbySnapshot& state, bool starting) { return validate_lobby(state, starting); };
        hooks.offer = [this](const net::LobbySnapshot& state, const std::vector<net::SetupSeat>& seats) -> core::Result<net::SetupOffer> {
            std::uint64_t delay{};
            auto selected = decode_lan_lobby_game(state, &delay);
            auto canonical = selected ? encode_lan_lobby_game(selected.value()) : core::Result<net::LobbyGame>::failure(selected.error());
            if (!prepared_setup_ || !prepared_setup_->match_identity || !canonical || canonical.value() != prepared_game_) return lobby_fail<net::SetupOffer>(net::LobbyReason::setup_failed, "Current staging choices have not been prepared.");
            auto bytes = net::encode_skirmish_setup(*prepared_setup_); if (!bytes) return core::Result<net::SetupOffer>::failure(bytes.error());
            net::SetupOffer offer; offer.setup = std::move(bytes).value(); offer.seed = prepared_setup_->seed;
            offer.identity = {std::string(core::build_identity()), *prepared_setup_->match_identity}; offer.seats = seats; offer.input_delay = delay;
            // The default LAN lobby does not provision confidential recovery. Existing legacy setup retains that path.
            if (recovery_policy_.reconnect || recovery_policy_.resync) return lobby_fail<net::SetupOffer>(net::LobbyReason::recovery_unavailable, "Recovery provisioning for staging is unavailable.");
            return core::Result<net::SetupOffer>::success(std::move(offer));
        };
        hooks.setup = [this](net::MessageTransport& stream, const net::LobbySnapshot&) -> core::Result<net::LobbySetupContext> {
            lobby_stream_ = &stream;
            net::LobbySetupContext context; context.options.auto_accept = false; context.options.auto_start = false;
            context.options.timeout_ms = 180'000; context.options.session.keep_admission_tick_open = true;
            context.options.session.pending_input_epoch = [this] { return pending_input_epoch(); };
            context.providers.identify = [this](const net::SetupOffer&) {
                if (!prepared_setup_ || !prepared_setup_->match_identity) return lobby_fail<sim::MatchAgreement>(net::LobbyReason::setup_failed, "Local staging match identity is unavailable.");
                return core::Result<sim::MatchAgreement>::success({std::string(core::build_identity()), *prepared_setup_->match_identity});
            };
            context.providers.build = [this](const net::SetupOffer& offer) -> core::Result<std::unique_ptr<net::SessionAdapter>> {
                if (!view().host) return deferred_providers_.build(offer);
                auto adapter = std::make_unique<LiveAdapter>(*prepared_setup_, offer.seats, trace_);
                adapter_ = adapter.get();
                return core::Result<std::unique_ptr<net::SessionAdapter>>::success(std::move(adapter));
            };
            context.providers.monitor = &gate_;
            context.providers.tick_zero_agreed = [this] { return io_->monitor && io_->monitor->setup_agreed(); };
            context.providers.tick_zero_mismatch = [this]() -> std::optional<net::TickZeroMismatch> {
                if (!io_->monitor || !io_->monitor->report()) return {};
                const auto& report = *io_->monitor->report();
                if (report.first_divergent_tick != 0) return {};
                return net::TickZeroMismatch{report.local_hash, report.remote_hash, report.channel};
            };
            return core::Result<net::LobbySetupContext>::success(std::move(context));
        };
        hooks.remove_peer = [this](net::PeerIndex peer, net::LobbyReason) { closing_peers_.insert(peer); removed_peers_.insert(peer); };
        return hooks;
    }
    void reset_lobby() {
        lobby_.reset(); lobby_result_.reset(); lobby_stream_ = nullptr; prepared_setup_.reset(); ready_pending_ = false; lobby_setup_accepted_ = false;
        removed_peers_.clear(); known_edges_.clear(); closing_peers_.clear(); lobby_notice_.reset(); last_lobby_state_.reset(); identity_retry_at_ms_ = 0;
    }
    void start_lobby() {
        const auto state = lobby_state();
        if (!state || !state->start_enabled || !prepared_setup_ || handed_over_) return;
        if (!lobby_feedback(lobby_->start(state->snapshot.revision))) return;
        auto raw = io_->transport.tcp->poll();
        if (raw) { io_->transport.pending_bytes += raw->frame.size(); io_->transport.pending.push_back(std::move(*raw)); }
        last_lobby_state_ = lobby_state();
        auto options = decode_lan_lobby_game(lobby_->snapshot());
        if (options) pending_ = std::move(options).value();
    }
    void pump_lobby(const skirmish::FixtureOptions& selected) {
        if (handed_over_ || !io_->transport.tcp || view().phase == LanPhase::ended) return;
        lobby_clock_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        if (lobby_ && lobby_->snapshot().phase == net::LobbyPhase::starting && view().host) {
            auto raw = io_->transport.tcp->poll();
            if (raw) {
                constexpr auto max_pending_bytes = 8U * 1024U * 1024U;
                if (io_->transport.pending.size() >= 256 || io_->transport.pending_bytes > max_pending_bytes
                    || raw->frame.size() > max_pending_bytes - io_->transport.pending_bytes) {
                    set_failure(net::lobby_diagnostic(net::LobbyReason::resource_limit, "Ordered setup input exceeds the staging handover budget."));
                    return;
                }
                io_->transport.pending_bytes += raw->frame.size(); io_->transport.pending.push_back(std::move(*raw));
            }
            return; // No core pump or tick-zero publication before live bind.
        }
        if (!lobby_) {
            if (view().host) {
                auto game = encode_lan_lobby_game(selected, input_delay_);
                if (!game || !selected.slots || selected.slots->empty()) {
                    if (online_options_) set_failure(diagnostic("EAWR-NET-1535", "Online staging requires valid game options and player slots."));
                    return;
                }
                auto configured = selected;
                for (auto& slot : *configured.slots) slot.human = std::find(owners_.begin(), owners_.end(), slot.slot) != owners_.end();
                configured.slots->front().colour_index = lobby_colour_;
                std::set<std::uint32_t> used_colours{lobby_colour_};
                for (auto& slot : *configured.slots) {
                    if (slot.slot == configured.slots->front().slot) continue;
                    auto colour = slot.colour_index.value_or(slot.slot - 1);
                    if (used_colours.contains(colour)) {
                        colour = 0;
                        while (colour < skirmish::multiplayer_colour_count && used_colours.contains(colour)) ++colour;
                    }
                    slot.colour_index = colour; used_colours.insert(colour);
                }
                game = encode_lan_lobby_game(configured, input_delay_);
                if (!game) {
                    if (online_options_) set_failure(diagnostic("EAWR-NET-1535", "Online staging options could not be encoded."));
                    return;
                }
                const auto& row = configured.slots->front();
                const auto room_seed = lobby_name_ + std::to_string(lobby_clock_);
                const Bytes room_bytes(room_seed.begin(), room_seed.end());
                auto created = net::LobbySession::host(io_->transport, "lan-" + core::sha256_hex(room_bytes).substr(0, 60),
                    static_cast<std::uint32_t>(owners_.size()), {lobby_name_, row.faction, lobby_colour_, row.team, row.slot}, row.slot, std::move(game).value(), lobby_hooks());
                if (!created) { set_failure(created.error()); return; }
                lobby_ = std::make_shared<net::LobbySession>(std::move(created).value()); io_->room_lifetime = lobby_; lobby_self_ = io_->transport.self();
            } else {
                auto raw = io_->transport.tcp->poll();
                if (!raw) {
                    if (online_snapshot_deadline_ && std::chrono::steady_clock::now() >= *online_snapshot_deadline_)
                        set_failure(diagnostic("EAWR-NET-1536", "Online staging snapshot timed out."));
                    return;
                }
                auto decoded = net::decode_lobby_frame(raw->frame);
                const auto* snapshot = decoded ? std::get_if<net::LobbySnapshot>(&decoded.value()) : nullptr;
                if (raw->from == 0 && decoded) if (const auto* refused = std::get_if<net::LobbyRejected>(&decoded.value())) {
                    set_failure(net::lobby_diagnostic(refused->reason, refused->message)); return;
                }
                if (!snapshot || raw->from != 0) { set_failure(net::lobby_diagnostic(net::LobbyReason::malformed, "Missing authoritative admission snapshot.")); return; }
                auto created = net::LobbySession::join(io_->transport, snapshot->room, 0, lobby_hooks());
                if (!created) { set_failure(created.error()); return; }
                lobby_ = std::make_shared<net::LobbySession>(std::move(created).value()); io_->room_lifetime = lobby_; lobby_self_ = io_->transport.self();
                online_snapshot_deadline_.reset();
                io_->transport.pending_bytes += raw->frame.size(); io_->transport.pending.push_back(std::move(*raw));
            }
        }
        // Closing is deferred until the core has queued its typed notice.
        flush_lobby_closes();
        if (view().host) {
            if (io_->transport.peer_count() == 2 && lobby_->snapshot().phase == net::LobbyPhase::staging && lobby_->snapshot().seats.size() == 1)
                (void)lobby_feedback(io_->transport.tcp->reopen_staging_listener());
            // Polling advances admission even when there is no application frame.
            auto frame = io_->transport.tcp->poll();
            if (frame) { io_->transport.pending_bytes += frame->frame.size(); io_->transport.pending.push_back(std::move(*frame)); }
            const auto& snapshot = lobby_->snapshot();
            auto options = decode_lan_lobby_game(snapshot);
            if (!options) { set_failure(options.error()); return; }
            for (net::PeerIndex peer = 1; peer < io_->transport.peer_count(); ++peer) {
                if (io_->transport.link(peer) != net::LinkState::open
                    || std::any_of(lobby_->snapshot().seats.begin(), lobby_->snapshot().seats.end(), [&](const auto& seat) { return seat.peer == peer; })) continue;
                const auto edge = io_->transport.tcp->edge_identity(peer);
                if (known_edges_[peer] != edge) { known_edges_[peer] = edge; removed_peers_.erase(peer); }
                if (removed_peers_.contains(peer)) {
                    auto rejected = net::encode_lobby_frame(net::LobbyRejected{{lobby_->snapshot().room, lobby_->snapshot().revision, net::LobbyReason::denied, "This connection was removed from the room."}});
                    if (rejected) (void)io_->transport.send(peer, rejected.value());
                    closing_peers_.insert(peer); continue;
                }
                const auto row = std::find_if(options.value().slots->begin(), options.value().slots->end(), [&](const auto& value) { return value.slot == owners_[peer]; });
                if (row == options.value().slots->end()) {
                    if (online_options_) { set_failure(diagnostic("EAWR-NET-1535", "Online staging has no vacant player row for this seat.")); return; }
                    continue;
                }
                auto admitted = lobby_->admit(peer, "lan-edge-" + std::to_string(edge),
                    {lan_admission_name(lobby_->snapshot(), peer), row->faction, row->colour_index.value_or(row->slot - 1), row->team, row->slot}, row->slot, lobby_->snapshot().revision);
                if (!admitted) {
                    const auto reason = lan_lobby_reason(admitted.error());
                    auto rejected = net::encode_lobby_frame(net::LobbyRejected{{lobby_->snapshot().room, lobby_->snapshot().revision, reason, admitted.error().message}});
                    if (rejected) (void)io_->transport.send(peer, rejected.value());
                    closing_peers_.insert(peer); removed_peers_.insert(peer);
                    (void)lobby_feedback(std::move(admitted));
                }
            }
        }
        auto pumped = lobby_->pump(lobby_clock_); flush_lobby_closes(); if (!pumped) { set_failure(pumped.error()); return; }
        if (!view().host && lobby_->snapshot().phase == net::LobbyPhase::staging && lobby_clock_ >= identity_retry_at_ms_) {
            const auto local = std::find_if(lobby_->snapshot().seats.begin(), lobby_->snapshot().seats.end(), [&](const auto& seat) { return seat.peer == io_->transport.self(); });
            if (local != lobby_->snapshot().seats.end() && local->selection.name != lobby_name_) {
                auto choice = local->selection; choice.name = lobby_name_;
                const bool shared = std::any_of(lobby_->snapshot().seats.begin(), lobby_->snapshot().seats.end(), [&](const auto& other) { return other.peer != local->peer && other.selection.slot == local->selection.slot; });
                if (!shared && lobby_colour_ < skirmish::multiplayer_colour_count) {
                    const auto choices = decode_lan_lobby_game(lobby_->snapshot());
                    if (choices && std::none_of(choices.value().slots->begin(), choices.value().slots->end(), [&](const auto& row) {
                        return row.slot != choice.slot && row.colour_index.value_or(row.slot - 1) == lobby_colour_;
                    })) choice.colour = lobby_colour_;
                }
                // A queued name request is not an acknowledgement: simultaneous guests can lose a revision race.
                // Retry against fresh authoritative choices at most once per core request-budget window.
                identity_retry_at_ms_ = lobby_clock_ + net::LobbyLimits{}.chat_window_ms;
                (void)lobby_select(std::move(choice));
            }
        }
        if (view().host && lobby_->snapshot().phase == net::LobbyPhase::staging) {
            // Commit requested owner mapping after the core validates the player's own slot request.
            const auto seats = lobby_->snapshot().seats;
            for (const auto& seat : seats) if (seat.player != seat.selection.slot)
                (void)lobby_feedback(lobby_->assign(seat.peer, seat.selection, seat.selection.slot, lobby_->snapshot().revision));
        }
        if (preparing_.valid() && preparing_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto prepared = preparing_.get();
            if (prepared && preparing_revision_ == lobby_->snapshot().revision && ready_pending_) {
                prepared_setup_ = std::move(prepared).value(); prepared_revision_ = preparing_revision_; prepared_game_ = preparing_game_;
                (void)lobby_feedback(lobby_->request(net::LobbyAction::ready, prepared_revision_));
            } else if (!prepared) (void)lobby_feedback(lobby_fail<void>(net::LobbyReason::setup_failed, prepared.error().message));
            ready_pending_ = false;
        }
        std::uint64_t delay{};
        auto options = decode_lan_lobby_game(lobby_->snapshot(), &delay);
        if (options) {
            offered_delay_ = delay;
            auto canonical = encode_lan_lobby_game(options.value());
            if (prepared_setup_ && canonical && canonical.value() == prepared_game_) prepared_revision_ = lobby_->snapshot().revision;
            else if (prepared_setup_) prepared_setup_.reset();
            offered_ = std::move(options).value();
        }
        const auto state = lobby_state();
        if (!state) return;
        last_lobby_state_ = state;
        const auto local = std::find_if(state->snapshot.seats.begin(), state->snapshot.seats.end(), [&](const auto& seat) { return seat.peer == io_->transport.self(); });
        { const std::lock_guard lock(mutex_);
            view_.seat = io_->transport.self(); if (local != state->snapshot.seats.end()) view_.player = local->player;
            view_.connected_seats = static_cast<std::uint32_t>(state->snapshot.seats.size()); view_.connected = state->snapshot.seats.size() > 1;
            if (online_options_ && view_.connected && view_.message == "Player connecting...") view_.message.clear();
            view_.human_owners.clear(); for (const auto& seat : state->snapshot.seats) view_.human_owners.push_back(seat.player);
            view_.phase = LanPhase::setup; view_.start_enabled = state->start_enabled; view_.accept_enabled = state->ready_enabled;
            if (state->snapshot.phase == net::LobbyPhase::closed) { view_.phase = LanPhase::ended; view_.reason = LanReason::peer_left; }
        }
        if (state->snapshot.phase == net::LobbyPhase::closed) { io_->transport.tcp->close(); (void)io_->transport.tcp->poll(); }
        if (state->snapshot.phase == net::LobbyPhase::starting && !view().host && offered_) { accepted_ = true; pending_ = offered_; }
        if (io_->transport.tcp->diagnostic() && !view().connected) set_failure(*io_->transport.tcp->diagnostic());
    }
    net::LobbyGame prepared_game_, preparing_game_;
    std::string lobby_name_;
    std::uint32_t lobby_colour_{};
    net::PeerIndex lobby_self_{};
    std::vector<skirmish::SetupMap> lobby_maps_;
    LanLobbyBuilder lobby_builder_;
    std::vector<std::string> factions_;
    std::shared_ptr<net::LobbySession> lobby_;
    std::shared_ptr<net::LobbyResult> lobby_result_;
    net::MessageTransport* lobby_stream_{};
    std::optional<tactical::TacticalSetup> prepared_setup_;
    std::future<core::Result<tactical::TacticalSetup>> preparing_;
    std::uint64_t preparing_revision_{}, prepared_revision_{}, lobby_clock_{};
    bool ready_pending_{}, lobby_setup_accepted_{};
    std::uint64_t identity_retry_at_ms_{};
    std::set<net::PeerIndex> removed_peers_, closing_peers_;
    std::map<net::PeerIndex, std::uint64_t> known_edges_;
    std::optional<net::LobbyNotice> lobby_notice_;
    std::optional<LanLobbyState> last_lobby_state_;
public:

    void set_public_invitation(bool value) override { if (online_options_) online_options_->service.public_alias = value; }
    void configure_online(OnlineMatchOptions options) {
        online_options_ = std::move(options);
        recovery_policy_ = {}; // Repair/lifetime qualification belongs to the online repair slice.
    }
private:
    std::optional<OnlineMatchOptions> online_options_;
    std::shared_ptr<OnlineAdmission> online_admission_, online_retiring_;
    // Observed after pairing only to read the service lease and credential age for notices (ticket E). Weak on
    // purpose: the transport owns signalling, and closing it must release the service session for the other peer.
    std::weak_ptr<net::GnsSignallingEndpoint> online_endpoint_;
    std::chrono::steady_clock::time_point online_paired_at_{};
    std::optional<std::chrono::steady_clock::time_point> online_snapshot_deadline_;
    void begin_online(bool hosting, std::string code) {
        cancel();
        if (online_retiring_ && !online_retiring_->finished.load()) {
            set_failure(diagnostic("EAWR-NET-1534", "The previous online request is still closing.")); return;
        }
        if (online_retiring_ && !online_retiring_->close_confirmed.load()) {
            const std::lock_guard lock(online_retiring_->mutex);
            if (std::chrono::steady_clock::now() < online_retiring_->expires) {
                set_failure(diagnostic("EAWR-NET-1534", "Wait for the previous invitation to expire before publishing again.")); return;
            }
        }
        online_retiring_.reset();
        if (owners_.size() != 2 || roster_error_) {
            set_failure(diagnostic("EAWR-NET-1530", "Online play currently requires two seats.")); return;
        }
        if (!net::GnsTransport::available() || !net::NetworkGnsIntroduction::available()) {
            set_failure(diagnostic("EAWR-NET-1530", "Online play is disabled in this build.")); return;
        }
        { const std::lock_guard lock(mutex_);
          view_ = {}; view_.host = hosting; view_.read_only = !hosting; view_.phase = LanPhase::connecting;
          view_.human_owners = owners_; view_.player = hosting ? owners_.front() : owners_.back();
          view_.message = hosting ? "Creating invitation..." : "Finding game...";
        }
        online_admission_ = std::make_shared<OnlineAdmission>();
        drain_workers().start(std::function<void()>([state = online_admission_, options = *online_options_, hosting, code = std::move(code)]() mutable {
            state->run(std::move(options), hosting, std::move(code));
            state->finished = true;
        }));
    }
    bool pump_online() {
        std::shared_ptr<net::GnsSignallingEndpoint> endpoint;
        std::optional<core::Diagnostic> error;
        { const std::lock_guard lock(online_admission_->mutex);
          error = online_admission_->failure; endpoint = std::move(online_admission_->paired);
          const std::lock_guard view_lock(mutex_);
          view_.invitation_code = online_admission_->code; view_.invitation_expires = online_admission_->expires;
          if (!online_admission_->bridge_code.empty()) view_.message = "Waiting for a player to join...";
        }
        if (error) { online_admission_->cancelled = true; online_retiring_ = std::move(online_admission_); set_failure(*error); return false; }
        if (!endpoint) return false;
        if (online_options_->turn_credentials_observer)
            if (const auto credentials = endpoint->turn_credentials()) online_options_->turn_credentials_observer(*credentials);
        net::GnsOptions options; options.build_identity = online_options_->build_identity;
        options.timeout = std::chrono::milliseconds(180000); options.turn_only = online_options_->turn_only;
        if (lobby_builder_) options.max_frame_size = static_cast<std::uint32_t>(net::max_lobby_frame_bytes);
        auto opened = view().host ? net::GnsTransport::host_introduced(endpoint, options) : net::GnsTransport::connect_introduced(endpoint, options);
        online_admission_.reset();
        online_endpoint_ = endpoint; online_paired_at_ = std::chrono::steady_clock::now();
        { const std::lock_guard lock(mutex_); view_.invitation_code.clear(); view_.invitation_consumed = true; view_.message = "Player connecting..."; }
        if (!opened) { set_failure(opened.error()); return false; }
        io_->transport.tcp = std::make_unique<MatchTransport>(std::move(opened).value());
        if (!view().host) online_snapshot_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        return true;
    }
public:
    void configure_factions(std::vector<std::string> factions) override {
        if (!handed_over_) factions_ = std::move(factions);
    }
    void configure_seats(std::vector<tactical::PlayerId> owners) override {
        if (handed_over_) return;
        const std::set<tactical::PlayerId> sides(owners.begin(), owners.end());
        roster_error_.reset();
        if (owners.size() < 2 || owners.size() > net::max_match_seats || sides.contains(0) || sides.size() > net::max_match_human_sides) {
            roster_error_ = diagnostic("EAWR-NET-1300", "Choose two to nine humans sharing up to three owners.");
            return;
        }
        owners_ = std::move(owners);
    }
    ~TcpLanMatch() override {
        if (online_admission_) online_admission_->cancelled = true;
        erase_credentials();
        if (mapping_) drain_workers().start(std::move(mapping_));
    }
    void set_port_mapping(bool enabled) override { map_port_ = enabled; }
    void host(std::uint16_t port) override {
        if (online_options_) { begin_online(true, {}); return; }
        cancel();
        io_->transport.credentials = credentials_ ? credential_state_ : nullptr;
        if (roster_error_) { set_failure(*roster_error_); return; }
        net::TcpOptions options; options.build_identity = core::build_identity();
        options.peers = static_cast<std::uint32_t>(owners_.size());
        if (lobby_builder_) options.max_frame_size = static_cast<std::uint32_t>(net::max_lobby_frame_bytes);
        // Loading is synchronous in Godot. Preserve the stream through a bounded load.
        options.timeout = std::chrono::milliseconds(180'000);
        auto opened = net::TcpTransport::host("0.0.0.0", port, options);
        if (!opened) { set_failure(opened.error()); return; }
        io_->transport.tcp = std::make_unique<MatchTransport>(std::move(opened).value());
        if (map_port_) mapping_ = std::make_unique<net::PortMapping>(io_->transport.tcp->port());
        const std::lock_guard lock(mutex_);
        view_ = {}; view_.host = true; view_.port = io_->transport.tcp->port(); view_.phase = LanPhase::connecting;
        view_.human_owners = owners_; view_.player = owners_.front();
        view_.reconnect_enabled = recovery_policy_.reconnect && static_cast<bool>(credentials_);
        view_.credential_provider_available = static_cast<bool>(credentials_);
        if (view_.reconnect_enabled) view_.reconnect_unavailable.clear();
    }
    void join(std::string endpoint) override {
        if (online_options_) { begin_online(false, std::move(endpoint)); return; }
        cancel();
        io_->transport.credentials = credentials_ ? credential_state_ : nullptr;
        if (roster_error_) { set_failure(*roster_error_); return; }
        auto parsed = net::parse_join_endpoint(endpoint);
        if (!parsed) { set_failure(parsed.error()); return; }
        const auto port = parsed.value().port;
        const auto remote_endpoint = parsed.value().host + ":" + std::to_string(port);
        endpoint = std::move(parsed).value().host;
        net::TcpOptions options; options.build_identity = core::build_identity(); options.timeout = std::chrono::milliseconds(180'000);
        options.peers = static_cast<std::uint32_t>(owners_.size());
        if (lobby_builder_) options.max_frame_size = static_cast<std::uint32_t>(net::max_lobby_frame_bytes);
        auto opened = net::TcpTransport::connect(std::move(endpoint), port, options);
        if (!opened) { set_failure(opened.error()); return; }
        io_->transport.tcp = std::make_unique<MatchTransport>(std::move(opened).value());
        const std::lock_guard lock(mutex_);
        view_ = {}; view_.read_only = true; view_.port = port; view_.phase = LanPhase::connecting;
        view_.human_owners = owners_; view_.player = 2;
        view_.remote_endpoint = remote_endpoint;
        view_.reconnect_enabled = recovery_policy_.reconnect && static_cast<bool>(credentials_);
        view_.credential_provider_available = static_cast<bool>(credentials_);
        if (view_.reconnect_enabled) view_.reconnect_unavailable.clear();
    }
    void cancel() override {
        online_snapshot_deadline_.reset();
        online_endpoint_.reset();
        if (online_admission_) {
            online_admission_->cancelled = true;
            { const std::lock_guard lock(online_admission_->mutex);
              if (auto endpoint = std::move(online_admission_->paired)) {
                  online_admission_->close_confirmed = true;
                  drain_workers().start(std::function<void()>([endpoint = std::move(endpoint)]() mutable { endpoint.reset(); }));
              }
            }
            online_retiring_ = std::move(online_admission_);
        }
        erase_credentials();
        if (mapping_) drain_workers().start(std::move(mapping_));
        // The viewer stops and joins LiveSession before returning to this screen.
        reset_lobby();
        rejoin_session_.reset(); returning_adapter_.reset(); setup_session_.reset(); runtime_.reset(); io_->monitor.reset(); io_->application_stream = nullptr; io_->room_lifetime.reset(); adapter_ = nullptr;
        for (auto& frame : io_->transport.pending) net::erase_reconnect_secret(frame.frame);
        for (auto& frame : early_) net::erase_reconnect_secret(frame.frame);
        for (auto& frame : retained_rejoin_frames_) net::erase_reconnect_secret(frame.frame);
        io_->transport.pending.clear(); io_->transport.pending_bytes = 0; io_->transport.tcp.reset(); io_->deadline.reset(); io_->closing = false; handed_over_ = accepted_ = false; deferred_providers_ = {};
        io_->transport.credential_error.reset();
        offered_.reset(); pending_.reset(); offered_delay_.reset(); waiting_since_.reset(); queued_.clear();
        early_.clear(); early_bytes_ = 0;
        retained_options_ = {}; retained_offer_ = {}; recovery_factory_ = {};
        diagnostic_bundle_.reset(); diagnostic_sink_ = {}; diagnostic_saved_ = false;
        last_repair_ms_.reset(); repair_notice_.reset(); retained_rejoin_frames_.clear();
        const std::lock_guard lock(mutex_); rejoin_requested_ = false; speed_requests_.clear(); speed_window_ = {}; abort_requested_ = false; view_ = {}; view_.reason = LanReason::cancelled;
    }
    bool request_abort() override {
        const std::lock_guard lock(mutex_);
        if (!lan_can_abort(view_)) return false;
        abort_requested_ = true;
        return true;
    }
    void complete_abort() override {
        auto terminal = view();
        if (terminal.phase == LanPhase::ended) {
            pump_close();
            terminal = view();
            finish_io(std::exchange(io_, std::make_unique<TerminalIo>()));
        }
        cancel();
        if (terminal.phase == LanPhase::ended) {
            const std::lock_guard lock(mutex_);
            view_ = terminal; view_.connected = false;
        }
    }
    bool save_diagnostic_details() override {
        net::DiagnosticBundle bundle;
        net::DiagnosticSink sink;
        {
            const std::lock_guard lock(mutex_);
            if (!diagnostic_bundle_ || !diagnostic_sink_) return false;
            if (!view_.bundle_path.empty() && diagnostic_saved_) {
                view_.diagnostic_save_notice = (view_.diagnostic_saved_automatically ? "Diagnostic details were already saved automatically: " : "Diagnostic details are already saved locally: ") + view_.bundle_path;
                return true;
            }
            bundle = *diagnostic_bundle_; sink = diagnostic_sink_;
        }
        auto saved = sink(bundle);
        const std::lock_guard lock(mutex_);
        if (!diagnostic_bundle_ || diagnostic_bundle_->id != bundle.id) return static_cast<bool>(saved);
        diagnostic_saved_ = static_cast<bool>(saved); view_.diagnostic_saved = diagnostic_saved_; view_.diagnostic_saved_automatically = false;
        if (saved) {
            view_.bundle_path = saved.value();
            view_.diagnostic_save_notice = "Diagnostic details saved locally: " + saved.value();
        } else view_.diagnostic_save_notice = "Diagnostic details could not be saved locally.";
        return static_cast<bool>(saved);
    }
    void acknowledge_recovery() override {
        const std::lock_guard lock(mutex_);
        if (view_.recovery == LanRecovery::recovered) {
            view_.recovery = LanRecovery::none; view_.diagnostic_save_notice.clear(); view_.diagnostic_code.clear(); view_.input_feedback.clear();
        }
    }
    void acknowledge_input_feedback() override { const std::lock_guard lock(mutex_); view_.input_feedback.clear(); }
    void set_discard_observer(std::function<void(std::uint64_t)> observer) override {
        const std::lock_guard lock(input_epoch_mutex_); discard_observer_ = std::move(observer);
    }
    void discard_command(const tactical::PlayerCommand& command) {
        std::function<void(std::uint64_t)> observer;
        { const std::lock_guard lock(input_epoch_mutex_); observer = discard_observer_; }
        if (observer) observer(command.key.sequence);
        const std::lock_guard lock(mutex_);
        view_.input_feedback = "Pending gameplay input was cancelled because synchronization changed. Issue it again after play resumes.";
    }
    void discard_recovery_commands() {
        for (const auto& command : queued_) discard_command(command);
        queued_.clear();
    }
    void capture_diagnostic(const net::DesyncReport& report) {
        if (diagnostic_bundle_ && diagnostic_bundle_->id == report.bundle.id) return;
        view_.desync_tick = report.first_divergent_tick;
        view_.bundle_path = report.bundle_location;
        diagnostic_bundle_ = report.bundle;
        diagnostic_saved_ = !report.bundle_location.empty() && !report.bundle_error;
        view_.diagnostic_available = !report.bundle.files.empty(); view_.diagnostic_saved = diagnostic_saved_; view_.diagnostic_saved_automatically = diagnostic_saved_;
        if (report.bundle_error) view_.diagnostic_save_notice = "Diagnostic details could not be saved locally.";
    }
    void pump_setup(const skirmish::FixtureOptions& selected) override {
        if (online_admission_ && !pump_online()) return;
        if (online_options_ && !handed_over_ && view().phase == LanPhase::ended) {
            // Finish the bounded in-band GNS refusal so both peers see its reason.
            if (io_->transport.tcp) (void)io_->transport.tcp->poll();
            return;
        }
        if (lobby_builder_) {
            pump_lobby(selected);
            // The first-snapshot wait has no LobbySession yet. Its transport
            // refusal still must leave the online page's pending phase.
            if (online_options_ && !handed_over_ && io_->transport.tcp && !view().connected
                && io_->transport.tcp->diagnostic()) set_failure(*io_->transport.tcp->diagnostic());
            return;
        }
        if (handed_over_) return;
        if (view().phase == LanPhase::ended) { pump_close(); return; }
        if (!io_->transport.tcp) return;
        for (unsigned i = 0; !offered_ && i < 256; ++i) {
            auto received = io_->transport.poll();
            if (!received) break;
            if (!received->frame.empty() && received->frame[0] == net::setup_refuse_kind) {
                auto refusal = net::decode_setup_refusal(received->frame);
                set_failure(refusal ? refusal.value().diagnostic() : refusal.error()); return;
            }
            if (!view().host && !received->frame.empty() && received->frame.front() != net::setup_offer_kind
                && (received->frame.front() == net::sealed_batch_kind
                    || (received->frame.front() >= 32 && received->frame.front() <= 39))
                && received->frame.size() <= net::max_batch_bytes && early_.size() < 8192
                && received->frame.size() <= 8U * 1024U * 1024U - early_bytes_) {
                early_bytes_ += received->frame.size(); early_.push_back(std::move(*received)); continue;
            }
            if (received->frame.empty() || received->from != 0 || received->frame.front() != net::setup_offer_kind || view().host) {
                set_failure(diagnostic("EAWR-NET-0602", "Unexpected message while waiting for match setup.")); return;
            }
            auto offer = net::decode_setup_offer(received->frame);
            if (!offer) { set_failure(offer.error()); return; }
            if (io_->transport.self() >= offer.value().seats.size()) {
                set_failure(diagnostic("EAWR-NET-1300", "Assigned seat is missing from the host offer.")); return;
            }
            {
                const std::lock_guard lock(mutex_);
                view_.seat = io_->transport.self(); view_.player = offer.value().seats[view_.seat].player;
                view_.human_owners.clear();
                for (const auto& seat : offer.value().seats) view_.human_owners.push_back(seat.player);
            }
            auto fixture = fixture_options(offer.value(), factions_);
            if (!fixture) { set_failure(fixture.error()); return; }
            offered_delay_ = offer.value().input_delay;
            offered_ = std::move(fixture).value();
            net::SetupProviders providers;
            providers.identify = [this](const net::SetupOffer& value) { return deferred_providers_.identify(value); };
            providers.build = [this](const net::SetupOffer& value) { return deferred_providers_.build(value); };
            providers.configure_session = [this](net::SessionOptions& policy, const net::SetupOffer& value) {
                if (!deferred_providers_.configure_session) return fail<void>("Local recovery provider is unavailable.");
                return deferred_providers_.configure_session(policy, value);
            };
            providers.monitor = &gate_;
            providers.tick_zero_agreed = [this] { return io_->monitor && io_->monitor->setup_agreed(); };
            providers.tick_zero_mismatch = [this] { return deferred_providers_.tick_zero_mismatch(); };
            net::SetupOptions policy; policy.auto_accept = false; policy.timeout_ms = 180'000;
            policy.session.keep_admission_tick_open = true;
            policy.session.pending_input_epoch = [this] { return pending_input_epoch(); };
            auto reviewer = net::SetupSession::join(io_->transport, std::move(providers), policy);
            if (!reviewer) { set_failure(reviewer.error()); return; }
            setup_session_.emplace(std::move(reviewer).value());
            auto reviewed = setup_session_->receive_offer(std::move(*received));
            if (!reviewed) { set_failure(reviewed.error()); return; }
            while (!early_.empty()) {
                io_->transport.pending.push_back(std::move(early_.front())); early_.pop_front();
            }
            early_bytes_ = 0;
            const auto& canonical = offer.value();
            std::string summary = "Host setup: " + *offered_->map + "\nSeed " + std::to_string(canonical.seed)
                + " | Victory " + (*offered_->victory_condition == tactical::VictoryCondition::enemy_starbase_destroyed ? std::string("Destroy enemy starbase") : *offered_->victory_condition == tactical::VictoryCondition::all_enemy_units_destroyed ? std::string("Destroy all enemy units") : std::string("None"));
            if (offered_->match) {
                const auto& match = *offered_->match;
                summary += " | " + match.space_win_condition + " | Credits " + std::to_string(match.credits.trunc_to_integer())
                    + " | Tech " + std::to_string(match.start_tech) + "/" + std::to_string(match.max_tech)
                    + " | Timer " + std::to_string(match.game_timer) + " | Win limit " + std::to_string(match.win_integer)
                    + " | Win value " + std::to_string(match.win_float.trunc_to_integer()) + " | Auto resolve " + std::to_string(match.auto_resolve)
                    + "\nHeroes " + (match.allow_heroes ? "on" : "off") + " | Superweapons " + (match.allow_superweapons ? "on" : "off")
                    + " | Starting units " + (match.free_starting_units ? "on" : "off")
                    + " | Base " + (match.pre_built_base ? "on" : "off") + " | Events " + (match.allow_random_events ? "on" : "off");
            }
            for (const auto& slot : *offered_->slots) {
                summary += "\nSlot " + std::to_string(slot.slot) + " " + slot.faction + (slot.human ? " Human" : " AI")
                    + " | Team " + std::to_string(slot.team + 1) + " | Colour " + std::to_string(slot.colour_index.value_or(slot.slot - 1) + 1)
                    + " | Fleet:";
                for (const auto& unit : slot.fleet) summary += " " + unit;
            }
            summary += "\nBuild: " + canonical.identity.build + "\nContent: " + core::sha256_hex(sim::canonical_match_identity(canonical.identity.content));
            { const std::lock_guard lock(mutex_); view_.accept_enabled = true; view_.setup_summary = std::move(summary); }
            // Local scene preparation computes identity; the offer is never trusted as local data.
            break;
        }
        if (io_->transport.credential_error) { refuse_credentials(*io_->transport.credential_error); return; }
        if (offered_ && setup_session_) {
            // Human review has no setup deadline. Keep its caller clock at zero until
            // the live driver takes ownership, and let LAN-3 bound and validate held frames.
            auto reviewed = setup_session_->pump(0);
            if (!reviewed) { set_failure(reviewed.error()); return; }
        }
        const auto remote = io_->transport.self() == 0 ? 1U : 0U;
        if (io_->transport.tcp->diagnostic()) {
            const bool retry = !online_options_ && view().host && io_->transport.peer_count() == 2 && io_->transport.link(remote) == net::LinkState::connecting;
            set_failure(*io_->transport.tcp->diagnostic(), !retry);
            if (retry) {
                const auto refused = lan_message(view());
                const std::lock_guard lock(mutex_);
                view_.phase = LanPhase::connecting; view_.connected = false;
                view_.message = refused + " Waiting for a player to join...";
            } else return;
        }
        {
            const std::lock_guard lock(mutex_);
            view_.connected_seats = 1;
            bool closed{};
            view_.link_stats.reset();
            for (net::PeerIndex peer = 0; peer < io_->transport.peer_count(); ++peer) {
                if (peer == io_->transport.self()) continue;
                const auto state = io_->transport.link(peer);
                if (state == net::LinkState::open) ++view_.connected_seats;
                closed = closed || state == net::LinkState::closed;
                if (const auto stats = io_->transport.link_stats(peer);
                    stats && (!view_.link_stats || stats->smoothed_rtt > view_.link_stats->smoothed_rtt)) view_.link_stats = stats;
            }
            if (view_.connected_seats == io_->transport.peer_count()) {
                view_.connected = true; view_.start_enabled = view_.host;
                view_.message.clear(); view_.reason = LanReason::none;
                view_.phase = LanPhase::setup;
            } else if (closed) {
                view_.phase = LanPhase::ended; view_.reason = LanReason::peer_left; view_.start_enabled = false;
            }
        }
        if (view().phase == LanPhase::ended) (void)pump_close();
    }
    void accept() override {
        if (lobby_builder_) { (void)lobby_ready(true); return; }
        if (!view().accept_enabled || !offered_ || handed_over_) return;
        accepted_ = true; pending_ = offered_;
        const std::lock_guard lock(mutex_); view_.accept_enabled = false;
    }
    void start(const skirmish::FixtureOptions& selected) override {
        if (lobby_builder_) { start_lobby(); return; }
        const auto current = view();
        if (!current.host || !current.start_enabled || handed_over_) return;
        auto selected_lan = selected;
        if (!selected_lan.slots || selected_lan.slots->size() < 2) {
            set_failure(diagnostic("EAWR-NET-1300", "A match needs at least two occupied sides.")); return;
        }
        for (auto& slot : *selected_lan.slots)
            slot.human = std::find(owners_.begin(), owners_.end(), slot.slot) != owners_.end();
        pending_ = std::move(selected_lan);
    }
    std::optional<skirmish::FixtureOptions> take_start() override {
        if (pending_ && lobby_ && !handed_over_ && lobby_->snapshot().phase != net::LobbyPhase::starting) { pending_.reset(); lobby_stream_ = nullptr; adapter_ = nullptr; }
        if (!pending_ || view().phase == LanPhase::ended) return {};
        handed_over_ = true; // Between main-thread polls, before the simulation thread exists.
        { const std::lock_guard lock(mutex_); view_.phase = LanPhase::barrier; view_.start_enabled = false; }
        return std::exchange(pending_, {});
    }
    std::optional<skirmish::FixtureOptions> offered_setup() const override { return offered_; }
    std::string bridge_ownership() const override {
        if (!online_admission_) return {};
        const std::lock_guard lock(online_admission_->mutex);
        return online_admission_->bridge_owner;
    }
    std::string bridge_invitation() const override {
        if (!online_admission_) return {};
        const std::lock_guard lock(online_admission_->mutex);
        return online_admission_->bridge_code;
    }
    LanView view() const override {
        const std::lock_guard lock(mutex_);
        auto result = view_;
        if (mapping_) result.mapping = mapping_->view();
        return result;
    }
    tactical::PlayerId local_player() const override { return view().player; }
    void note_input(std::uint64_t tick, std::uint64_t frame, std::uint64_t ui_epoch) override { if (trace_) trace_->note(tick, frame, ui_epoch); }
    void note_native_input(std::uint64_t tick, std::uint64_t epoch) override { if (trace_) trace_->native_input(tick, epoch); }
    void note_issue(std::uint64_t sequence, std::uint64_t epoch) override { if (trace_) trace_->issue(sequence, epoch); }
    void set_submission_observer(std::function<void(std::uint64_t, std::uint64_t)> observer) override {
        const std::lock_guard lock(input_epoch_mutex_);
        submission_observer_ = std::move(observer);
    }
    void set_input_epoch_provider(std::function<std::optional<std::uint64_t>()> provider) override {
        const std::lock_guard lock(input_epoch_mutex_);
        input_epoch_provider_ = std::move(provider);
    }
    platform::LiveStepDriver driver(const tactical::TacticalSetup& setup) override {
        return [self = shared_from_this(), setup](platform::LiveStepContext& context,
            std::span<const tactical::PlayerCommand> commands, std::uint64_t now) {
                self->progress(context, now);
                if (self->trace_ && self->trace_->failed()) {
                    auto result = fail<bool>("Cannot open or write LAN acceptance trace.");
                    self->set_failure(result.error());
                    return result;
                }
                auto result = self->drive(setup, context, commands, now);
                if (!result && self->view().phase != LanPhase::ended) self->set_failure(result.error());
                // Trace observers only record I/O failure; report it at the driver boundary.
                if (self->trace_ && self->trace_->failed()) {
                    result = fail<bool>("Cannot open or write LAN acceptance trace.");
                    self->set_failure(result.error());
                }
                if (!result && self->smoke_progress_) std::cerr << "EAWR LAN drive failed: " << result.error().message << std::endl;
                return result;
            };
    }
private:
    void erase_credentials() noexcept {
        credential_state_->clear();
        io_->transport.credentials.reset(); // Drains cannot provision a finished match again.
        const auto erase_delivery = [](auto& queue) {
            for (auto& frame : queue) if (!frame.frame.empty() && frame.frame[0] == net::lan_credentials_kind)
                net::erase_reconnect_secret(frame.frame);
        };
        erase_delivery(io_->transport.pending); erase_delivery(early_); erase_delivery(retained_rejoin_frames_);
        if (setup_session_) setup_session_->erase_reconnect_credentials();
        if (retained_options_.reconnect) retained_options_.reconnect->seat_tokens.clear();
        if (rejoin_session_) rejoin_session_->erase_reconnect_credentials();
        if (runtime_) runtime_->session().erase_reconnect_credentials();
        if (io_->transport.tcp) io_->transport.tcp->erase_reconnect_credentials();
    }
    void refuse_credentials(const core::Diagnostic& error) {
        const auto refusal = net::encode_setup_refusal({net::SetupReason::options, {}, error.code + ": " + error.message, {}, {}});
        if (refusal) for (net::PeerIndex peer = 0; peer < io_->transport.peer_count(); ++peer)
            if (peer != io_->transport.self() && io_->transport.link(peer) == net::LinkState::open)
                static_cast<void>(io_->transport.send(peer, refusal.value()));
        set_failure(error);
    }
    core::Result<void> service_recovery_io() {
        { const std::lock_guard lock(mutex_);
          if (abort_requested_) return core::Result<void>::failure(diagnostic("EAWR-NET-1801", "Local recovery cancelled.")); }
        // Replay remains on the simulation owner. Drain the socket into the bounded
        // stable wrapper to keep keepalive/chunk traffic moving between replay ticks.
        for (unsigned index = 0; index < 256 && io_->transport.pending.size() + retained_rejoin_frames_.size() < 4096; ++index) {
            auto frame = io_->transport.poll_socket(); if (!frame) break;
            if (frame->frame.size() > 8U * 1024U * 1024U - io_->transport.pending_bytes)
                return core::Result<void>::failure(diagnostic("EAWR-NET-1215", "Recovery receive buffer exceeded its limit."));
            io_->transport.pending_bytes += frame->frame.size();
            io_->transport.pending.push_back(std::move(*frame));
        }
        if (io_->transport.credential_error) return core::Result<void>::failure(*io_->transport.credential_error);
        return core::Result<void>::success();
    }
    net::TcpRejoinAuth tcp_rejoin_auth(std::uint64_t epoch) const {
        net::TcpRejoinAuth auth; auth.match_identity = retained_options_.reconnect->match_identity;
        auth.seat_tokens = retained_options_.reconnect->seat_tokens; auth.epoch = epoch; return auth;
    }
    net::SetupResult& setup_result() { return lobby_result_ ? lobby_result_->setup() : *runtime_; }
    net::LockstepSession& core_session() { return rejoin_session_ ? *rejoin_session_ : setup_result().session(); }
    void attach_stage_observer() {
        if (trace_) core_session().set_stage_observer([this](std::string_view event, const net::SeatInput& input, std::uint64_t tick) {
            const auto* bound = static_cast<const LiveAdapter&>(core_session().active_adapter()).binding(input.key.seat);
            if (!bound) return;
            auto decoded = net::decode_tactical_input(input, *bound);
            if (decoded) trace_->stage(event, input.key, command_kind(decoded.value().payload), tick);
        });
    }
    core::Diagnostic repair_diagnostic(core::Diagnostic error) const {
        if (io_->transport.self() == 0 && io_->transport.peer_count() == 2 && error.code == "EAWR-NET-0206") {
            error.code = "EAWR-NET-1017";
            error.message = "Reconnect listener failed: " + error.message;
        }
        return error;
    }
    core::Result<void> activate_adapter(LiveAdapter& adapter) {
        if ((!runtime_ && !lobby_result_) || &core_session().active_adapter() != &adapter || !adapter.staging
            || core_session().reconnect_status().phase != net::ReconnectPhase::playing)
            return core::Result<void>::success();
        if (!adapter.staging->activate) return core::Result<void>::failure(diagnostic("EAWR-NET-1213", "Recovery publication is unavailable."));
        auto activated = adapter.staging->activate(); if (!activated) return activated;
        adapter.staging.reset(); adapter.bind_live_context(*current_context_, trace_); adapter_ = &adapter;
        queued_.clear();
        { const std::lock_guard lock(mutex_); ++view_.active_adapter_publications; }
        return core::Result<void>::success();
    }
    void update_desync() {
        // Protocol failures freeze without a divergence report. Keep their typed
        // diagnostic when cleanup polls again instead of relabelling them desync.
        if (!io_->monitor || !io_->monitor->frozen() || !io_->monitor->report()) return;
        const std::lock_guard lock(mutex_);
        view_.phase = LanPhase::ended; view_.reason = LanReason::desync; view_.recovery = LanRecovery::failed;
        if (const auto& report = io_->monitor->report()) {
            capture_diagnostic(*report);
        }
    }
    core::Result<void> pump_close() {
        auto pumped = io_->pump();
        if (!pumped) {
            set_failure(pumped.error(), false);
            io_->transport.tcp.reset();
            const std::lock_guard lock(mutex_);
            view_.message = pumped.error().code + ": " + pumped.error().message;
        } else update_desync();
        if (!io_->transport.tcp) {
            const std::lock_guard lock(mutex_); view_.connected = false;
        }
        return pumped;
    }
    void progress(const platform::LiveStepContext& context, std::uint64_t now) {
        if (!smoke_progress_ || !io_->transport.tcp || (last_progress_ && now - *last_progress_ < 5000)) return;
        last_progress_ = now;
        std::cerr << "EAWR LAN progress peer=" << io_->transport.self() << " tick=" << context.completed_tick()
            << " advance=" << context.advance_allowed << " setup=" << (setup_session_ ? static_cast<int>(setup_session_->phase()) : -1)
            << " runtime=" << runtime_.has_value() << " may_step=" << (io_->monitor && io_->monitor->may_step())
            << " frozen=" << (io_->monitor && io_->monitor->frozen()) << " zero_agreed=" << (io_->monitor && io_->monitor->setup_agreed())
            << " waiting=" << (io_->monitor ? io_->monitor->waiting_for_peers().size() : 0)
            << " link=" << static_cast<int>(io_->transport.link(io_->transport.self() == 0 ? 1U : 0U))
            << " sent=" << io_->transport.sent << " received=" << io_->transport.received
            << " batches_sent=" << io_->transport.batches_sent << " batches_received=" << io_->transport.batches_received << std::endl;
    }
    void set_failure(const core::Diagnostic& diagnostic, bool terminal = true) {
        LanReason reason = LanReason::refused;
        if (diagnostic.code == net::lobby_diagnostic(net::LobbyReason::not_ready, "").code) reason = LanReason::cancelled;
        else if (diagnostic.code == "EAWR-NET-0202" || diagnostic.message.find("build") != std::string::npos) reason = LanReason::build;
        else if (diagnostic.code == "EAWR-NET-0201" || diagnostic.message.find("version") != std::string::npos) reason = LanReason::version;
        else if (diagnostic.code == "EAWR-NET-0302" || diagnostic.message.find("identity") != std::string::npos) reason = LanReason::identity;
        else if (diagnostic.code == "EAWR-NET-0207" || (diagnostic.code == "EAWR-NET-0209" && diagnostic.message.find("deadline") != std::string::npos) || diagnostic.code == "EAWR-NET-0306" || diagnostic.message.find("timeout") != std::string::npos || diagnostic.message.find("expired") != std::string::npos) reason = LanReason::timeout;
        else if ((diagnostic.code == "EAWR-NET-0206" && view().connected) || diagnostic.code == "EAWR-NET-0305" || diagnostic.message.find("closed") != std::string::npos || diagnostic.message.find("disconnect") != std::string::npos) reason = LanReason::peer_left;
        {
            const std::lock_guard lock(mutex_);
            view_.phase = LanPhase::ended; view_.reason = reason; view_.message.clear();
            view_.diagnostic_code = diagnostic.code.starts_with("EAWR-NET-1") ? diagnostic.code : std::string{}; view_.start_enabled = view_.accept_enabled = false;
            if (io_->monitor && io_->monitor->frozen() && io_->monitor->report()) {
                const auto& report = *io_->monitor->report();
                view_.reason = LanReason::desync; capture_diagnostic(report);
                view_.bundle_path = report.bundle_location;
            }
            if (view_.reason == LanReason::desync || view_.recovery == LanRecovery::resyncing)
                view_.recovery = LanRecovery::failed;
            else if (view_.recovery == LanRecovery::held || view_.recovery == LanRecovery::catching_up
                || view_.recovery == LanRecovery::proving || diagnostic.code.starts_with("EAWR-NET-10"))
                view_.recovery = LanRecovery::rejoin_failed;
            else view_.recovery = LanRecovery::none;
            view_.rejoin_enabled = false;
            if (online_options_) {
                view_.diagnostic_code = diagnostic.code;
                if (reason == LanReason::build && diagnostic.code != "EAWR-NET-1530") view_.message = "Both players need the same game build.";
                else if (diagnostic.code == "EAWR-NET-1520") view_.message = "The online service did not accept your access credentials.";
                else if (diagnostic.code == "EAWR-NET-1522" || diagnostic.code == "EAWR-NET-1523" || diagnostic.code == "EAWR-NET-1524") view_.message = "This invitation is no longer available. Ask the host for a new one.";
                else if (diagnostic.code == "EAWR-NET-1532" || diagnostic.code == "EAWR-NET-1525" || diagnostic.code == "EAWR-NET-1527") view_.message = "The online service is busy. Try again shortly.";
                else if (diagnostic.code == "EAWR-NET-1534") view_.message = "The previous invitation is still closing. Wait for it to expire before trying again.";
                else if (diagnostic.code == "EAWR-NET-1530") { view_.reason = LanReason::refused; view_.message = "Online play is unavailable in this build or configuration."; }
                else if (diagnostic.code == "EAWR-NET-1535" || diagnostic.code == "EAWR-NET-1536") view_.message = diagnostic.message;
                else if (diagnostic.code == "EAWR-NET-1730") view_.message = "The online service session reached its maximum length.";
                else if (diagnostic.code == "EAWR-NET-1731") view_.message = "The online service session was not renewed in time.";
                else if (const auto endpoint = online_endpoint_.lock(); endpoint && online_relay_allocation_ended(reason, endpoint->turn_credentials(), online_paired_at_, std::chrono::steady_clock::now())) {
                    view_.message = "The connection ended after the online service credentials expired.";
                    view_.diagnostic_code = "EAWR-NET-1737";
                }
                else view_.message = "Could not connect to this game. Check the online service settings and try again.";
                view_.message += "\n" + view_.diagnostic_code;
            }
            else if (diagnostic.code.starts_with("EAWR-NET-06") || diagnostic.code.starts_with("EAWR-NET-16") || diagnostic.code.starts_with("EAWR-NET-102")) view_.message = diagnostic.message;
            else if (diagnostic.code >= "EAWR-NET-0208" && diagnostic.code <= "EAWR-NET-0212" && diagnostic.code != "EAWR-NET-0209")
                view_.message = diagnostic.code + ": " + diagnostic.message;
            else if (diagnostic.code >= "EAWR-NET-0200" && diagnostic.code <= "EAWR-NET-0209")
                view_.message.clear(); // Let lan_message retain the curated legacy reason, including host retries.
        }
        // A failed driver will no longer be polled by LiveSession.
        if (terminal) {
            if (lobby_ && !lobby_result_) { io_->monitor.reset(); gate_.monitor = nullptr; lobby_stream_ = nullptr; }
            erase_credentials();
            pump_close();
            finish_io(std::exchange(io_, std::make_unique<TerminalIo>()));
            lobby_.reset(); lobby_result_.reset();
            if (!handed_over_) last_lobby_state_.reset(); // Frozen staging report is immutable while the driver owns I/O.
            const std::lock_guard lock(mutex_); view_.connected = false;
        }
    }
    DriverResult drive(const tactical::TacticalSetup& setup, platform::LiveStepContext& context,
        std::span<const tactical::PlayerCommand> commands, std::uint64_t now) {
        {
            const std::lock_guard lock(mutex_);
            if (abort_requested_) return DriverResult::success(false);
        }
        if (view().phase == LanPhase::ended) {
            auto pumped = pump_close();
            if (!pumped) return DriverResult::failure(pumped.error());
            return DriverResult::success(false);
        }
        if (!io_->transport.tcp) return fail<bool>("LAN transport is unavailable.");
        current_context_ = &context;
        gate_.advance_allowed = context.advance_allowed;
        queued_.insert(queued_.end(), commands.begin(), commands.end());
        if (adapter_ && !adapter_->staging) adapter_->context = &context;
        if (!io_->monitor) {
            if (!setup.match_identity) return fail<bool>("Local match identity is missing.");
            auto bytes = net::encode_skirmish_setup(setup);
            if (!bytes) { set_failure(bytes.error()); return DriverResult::failure(bytes.error()); }
            net::SetupOffer offer; offer.seed = setup.seed; offer.setup = bytes.value();
            offer.input_delay = view().host ? input_delay_ : offered_delay_.value_or(input_delay_);
            monitor_delay_ = offer.input_delay;
            offer.identity = {std::string(core::build_identity()), *setup.match_identity};
            offer.recovery = recovery_policy_;
            // Recovery requires the live source to recreate its own authoritative hooks.
            offer.recovery.resync = offer.recovery.resync && static_cast<bool>(context.rebuild);
            offer.recovery.reconnect = offer.recovery.reconnect && static_cast<bool>(context.rebuild);
            { const std::lock_guard lock(mutex_);
              view_.reconnect_enabled = offer.recovery.reconnect;
              view_.reconnect_unavailable = offer.recovery.reconnect ? "" : "Reconnect is disabled for this match.";
            }
            if (lobby_) for (const auto& seat : lobby_->snapshot().seats)
                offer.seats.push_back({static_cast<net::PeerIndex>(offer.seats.size()), seat.seat, seat.player});
            else for (net::PeerIndex peer = 0; peer < owners_.size(); ++peer)
                offer.seats.push_back({peer, sim::SeatId{peer + 1}, owners_[peer]});
            net::DesyncOptions options;
            if (online_options_) options.exchange_every = online_options_->hash_exchange_every;
            options.diagnostic_sink = [root = diagnostic_root_](const net::DiagnosticBundle& bundle) -> core::Result<std::string> {
                std::error_code error;
                std::filesystem::create_directories(root, error);
                if (error) return fail<std::string>("Cannot create the desync diagnostic folder: " + error.message());
                std::filesystem::path location;
                for (unsigned index = 0; index < 10000; ++index) {
                    location = root / (bundle.id + "-" + std::to_string(index));
                    if (std::filesystem::create_directory(location, error)) break;
                    if (error || index == 9999) return fail<std::string>("Cannot create a unique desync diagnostic folder: " + error.message());
                }
                std::vector<platform::PublishedFile> files;
                for (const auto& file : bundle.files) files.push_back({file.name, (location / file.name).string(), {file.bytes.begin(), file.bytes.end()}});
                const auto published = platform::publish_files(files);
                if (published.failure) {
                    std::string message = published.failure->message;
                    for (const auto& leftover : published.leftovers) message += "; " + leftover.message;
                    return fail<std::string>(std::move(message));
                }
                return core::Result<std::string>::success(std::filesystem::absolute(location).string());
            };
            diagnostic_sink_ = options.diagnostic_sink;
            options.input_delay = offer.input_delay;
            options.build_identity = offer.identity.build;
            options.match_setup = offer.setup;
            options.match_identity = core::sha256_hex(sim::canonical_match_identity(offer.identity.content));
            io_->monitor = std::make_unique<net::DesyncMonitor>(lobby_stream_ ? *lobby_stream_ : static_cast<net::MessageTransport&>(io_->transport), std::move(options));
            net::SetupProviders providers;
            providers.identify = [identity = offer.identity](const net::SetupOffer&) { return core::Result<sim::MatchAgreement>::success(identity); };
            providers.build = [this, setup, bytes = bytes.value()](const net::SetupOffer& agreed) -> core::Result<std::unique_ptr<net::SessionAdapter>> {
                if (agreed.input_delay != monitor_delay_) return fail<std::unique_ptr<net::SessionAdapter>>("Monitor delay differs from the agreed input delay.");
                if (agreed.setup != bytes) return fail<std::unique_ptr<net::SessionAdapter>>("The locally built match differs from the host setup.");
                auto adapter = std::make_unique<LiveAdapter>(setup, agreed.seats, trace_);
                adapter->options_applied = [this](net::MatchOptions options, std::uint64_t tick) {
                    const std::lock_guard lock(mutex_); view_.options = options; view_.options_tick = tick;
                };
                adapter_ = adapter.get(); adapter_->context = current_context_;
                return core::Result<std::unique_ptr<net::SessionAdapter>>::success(std::move(adapter));
            };
            providers.configure_session = [this, setup](net::SessionOptions& policy, const net::SetupOffer& agreed) -> core::Result<void> {
                if (agreed.recovery.reconnect != (recovery_policy_.reconnect && static_cast<bool>(current_context_->rebuild)))
                    return core::Result<void>::failure(diagnostic("EAWR-NET-1025", "Local reconnect policy differs from the host offer."));
                if (!agreed.recovery.reconnect && !agreed.recovery.resync) return core::Result<void>::success();
                net::ReconnectOptions retention;
                retention.allow_rejoin = agreed.recovery.reconnect;
                retention.setup = agreed.setup;
                auto envelope = net::encode_setup_offer(agreed);
                if (!envelope) return core::Result<void>::failure(envelope.error());
                retention.match_identity = core::sha256(envelope.value());
                if (retention.allow_rejoin) {
                    if (!credentials_) return core::Result<void>::failure(diagnostic("EAWR-NET-1025", "Reconnect credential provider is unavailable."));
                    auto tokens = credentials_(agreed); if (!tokens) return core::Result<void>::failure(tokens.error());
                    retention.seat_tokens = std::move(tokens).value();
                }
                retention.grace_ms = agreed.recovery.grace_ms; retention.progress_timeout_ms = agreed.recovery.progress_timeout_ms;
                retention.hard_cap_ms = agreed.recovery.hard_cap_ms; retention.max_log_bytes = static_cast<std::size_t>(agreed.recovery.max_log_bytes);
                policy.reconnect = std::move(retention);
                if (agreed.recovery.resync || agreed.recovery.reconnect) {
                    net::ResyncOptions recovery; recovery.timeout_ms = agreed.recovery.resync_timeout_ms;
                    recovery.rebuild = [this, setup, seats = agreed.seats, expected = agreed.setup](std::span<const std::uint8_t> bytes)
                        -> core::Result<std::unique_ptr<net::SessionAdapter>> {
                        using Result = core::Result<std::unique_ptr<net::SessionAdapter>>;
                        if (!std::equal(bytes.begin(), bytes.end(), expected.begin(), expected.end()) || !current_context_->rebuild)
                            return Result::failure(diagnostic("EAWR-NET-1214", "Verified recovery setup or local factory differs."));
                        auto rebuilt = current_context_->rebuild(); if (!rebuilt) return Result::failure(rebuilt.error());
                        auto fresh = std::make_unique<LiveAdapter>(setup, seats, nullptr);
                        fresh->staging = std::move(rebuilt).value(); fresh->context = fresh->staging.get();
                        fresh->activate_if_current = [this](LiveAdapter& value) { return activate_adapter(value); };
                        fresh->service_io = [this, staged = fresh.get()] {
                            { const std::lock_guard lock(mutex_);
                              view_.phase = core_session().resyncing() ? LanPhase::resyncing : LanPhase::reconnecting;
                              view_.recovery = core_session().resyncing() ? LanRecovery::resyncing
                                  : core_session().reconnect_status().phase == net::ReconnectPhase::proving ? LanRecovery::proving : LanRecovery::catching_up;
                              view_.catch_up_tick = staged->completed_tick(); }
                            return service_recovery_io();
                        };
                        fresh->options_applied = [this](net::MatchOptions options, std::uint64_t tick) {
                            const std::lock_guard lock(mutex_); view_.options = options; view_.options_tick = tick;
                        };
                        return Result::success(std::move(fresh));
                    };
                    recovery_factory_ = recovery.rebuild;
                    if (agreed.recovery.resync) policy.resync = std::move(recovery);
                }
                retained_options_ = policy; retained_offer_ = agreed;
                return core::Result<void>::success();
            };
            gate_.monitor = io_->monitor.get();
            providers.monitor = &gate_;
            providers.credentials_delivered = [this, enabled = offer.recovery.reconnect] {
                return !enabled || !default_credentials_ || credential_state_->delivered();
            };
            providers.tick_zero_agreed = [this] { return io_->monitor->setup_agreed(); };
            providers.tick_zero_mismatch = [this]() -> std::optional<net::TickZeroMismatch> {
                const auto& report = io_->monitor->report();
                return report && report->first_divergent_tick == 0 ? std::optional<net::TickZeroMismatch>{{report->local_hash, report->remote_hash, report->channel}} : std::nullopt;
            };
            net::SetupOptions policy; policy.timeout_ms = 180'000; policy.auto_accept = false;
            policy.session.keep_admission_tick_open = true;
            policy.session.pending_input_epoch = [this] { return pending_input_epoch(); };
            if (lobby_) {
                auto frozen_bytes = prepared_setup_ && prepared_setup_->match_identity ? net::encode_skirmish_setup(*prepared_setup_)
                    : lobby_fail<Bytes>(net::LobbyReason::setup_failed, "Local staging match identity is unavailable.");
                if (!frozen_bytes || frozen_bytes.value() != bytes.value())
                    return lobby_fail<bool>(net::LobbyReason::setup_failed, "Live setup differs from frozen staging setup.");
                if (adapter_) adapter_->bind_live_context(context, trace_);
                if (!view().host) {
                    deferred_providers_ = std::move(providers);
                }
            }
            else if (setup_session_) deferred_providers_ = std::move(providers);
            else {
                auto created = view().host ? net::SetupSession::host(io_->transport, std::move(offer), std::move(providers), policy)
                                          : net::SetupSession::join(io_->transport, std::move(providers), policy);
                if (!created) { set_failure(created.error()); return DriverResult::failure(created.error()); }
                setup_session_.emplace(std::move(created).value());
            }
        }
        if (lobby_ && !lobby_result_) {
            lobby_clock_ = std::max(lobby_clock_, now);
            auto pumped = lobby_->pump(lobby_clock_); // Steady-clock milliseconds, independent of sim ticks.
            flush_lobby_closes();
            if (lobby_->snapshot().phase == net::LobbyPhase::staging || lobby_->snapshot().phase == net::LobbyPhase::closed) {
                io_->monitor.reset(); gate_.monitor = nullptr; lobby_stream_ = nullptr;
                const auto reason = lobby_->notice() ? lobby_->notice()->reason : net::LobbyReason::setup_failed;
                const auto message = reason == net::LobbyReason::kicked ? "TEXT_INTERNET_PLAYER_KICKED"
                    : reason == net::LobbyReason::host_closed ? "TEXT_HOST_LEFT_GAME" : "The room ended before Start was committed.";
                return lobby_fail<bool>(reason, message);
            }
            if (!pumped) return DriverResult::failure(pumped.error());
            if (!view().host && !lobby_setup_accepted_ && lobby_->setup_phase() == net::SetupPhase::reviewing_offer) {
                auto accepted = lobby_->accept_setup(); if (!accepted) return DriverResult::failure(accepted.error());
                lobby_setup_accepted_ = true;
            }
            if (lobby_->snapshot().phase != net::LobbyPhase::committed || !context.advance_allowed)
                return DriverResult::success(false);
            auto ready = lobby_->take_result(); if (!ready) return DriverResult::failure(ready.error());
            lobby_result_ = std::make_shared<net::LobbyResult>(std::move(ready).value());
            io_->room_lifetime = lobby_result_; io_->application_stream = lobby_stream_;
            { const std::lock_guard lock(mutex_); view_.phase = LanPhase::running; view_.input_delay = monitor_delay_; }
            attach_stage_observer();
            if (setup.departure_policy) core_session().enable_departures([this](sim::SeatCommandKey key) {
                const auto* bound = static_cast<const LiveAdapter&>(core_session().active_adapter()).binding(key.seat);
                if (!bound) return fail<Bytes>("Unbound departure seat.");
                return net::encode_tactical_input(key, *bound, {{}, tactical::QuitPayload{}});
            });
        }
        if (!runtime_ && !lobby_result_) {
            // Adapter creation can occur in this pump, so refresh its context in the provider.
            if (accepted_ && setup_session_->phase() == net::SetupPhase::reviewing_offer) {
                auto accepted = setup_session_->accept();
                if (!accepted) { set_failure(accepted.error()); return DriverResult::failure(accepted.error()); }
            }
            auto pumped = setup_session_->pump(now);
            if (!pumped) { set_failure(pumped.error()); return DriverResult::failure(pumped.error()); }
            if (io_->transport.credential_error) {
                const auto error = *io_->transport.credential_error; refuse_credentials(error);
                return DriverResult::failure(error);
            }
            if (setup_session_->phase() != net::SetupPhase::ready || !context.advance_allowed) return DriverResult::success({});
            {
                const std::lock_guard lock(mutex_);
                if (abort_requested_) return DriverResult::success(false);
                auto ready = setup_session_->take_result();
                if (!ready) return DriverResult::failure(ready.error());
                runtime_.emplace(std::move(ready).value());
                if (setup_result().offer().input_delay != monitor_delay_)
                    return fail<bool>("Monitor delay differs from the ready session.");
                view_.phase = LanPhase::running; view_.input_delay = monitor_delay_;
            }
            attach_stage_observer();
            if (setup.departure_policy) core_session().enable_departures([this](sim::SeatCommandKey key) {
                const auto* bound = static_cast<const LiveAdapter&>(core_session().active_adapter()).binding(key.seat);
                if (!bound) return fail<Bytes>("Unbound departure seat.");
                return net::encode_tactical_input(key, *bound, {{}, tactical::QuitPayload{}});
            });
        }
        bool rejoining{};
        { const std::lock_guard lock(mutex_); rejoining = std::exchange(rejoin_requested_, false); }
        const auto requested_status = core_session().reconnect_status();
        if (rejoining && requested_status.phase == net::ReconnectPhase::held && core_session().reconnect_available()) {
            bool retry = last_repair_ms_ && now - *last_repair_ms_ < 1000;
            if (!retry) {
                last_repair_ms_ = now;
                auto restarted = io_->transport.tcp->restart_links(tcp_rejoin_auth(requested_status.epoch));
                if (!restarted) { repair_notice_ = repair_diagnostic(restarted.error()); retry = true; }
                else {
                    repair_notice_.reset();
                    if (!current_context_->rebuild) return DriverResult::failure(diagnostic("EAWR-NET-1015", "Authenticated rejoin factory is unavailable."));
                    auto fresh = recovery_factory_(retained_offer_.setup);
                    if (!fresh) return DriverResult::failure(fresh.error());
                    auto* returning = dynamic_cast<LiveAdapter*>(fresh.value().get());
                    if (!returning) return fail<bool>("Returning adapter has the wrong mode.");
                    auto policy = retained_options_;
                    policy.reconnect->rejoin_source = requested_status.peer;
                    policy.reconnect->rejoin_epoch = requested_status.epoch;
                    std::vector<net::PeerSeat> seats;
                    for (const auto& seat : retained_offer_.seats) seats.push_back({seat.seat, true});
                    // The package restores the executed prefix and source's ready_
                    // frontier. Another survivor may have sent a valid future batch
                    // that the source has not read yet: keep it until restoration,
                    // when ordinary receive validates it (or its retained digest).
                    // New catch-up traffic stays in pending; nothing clears it later.
                    while (!io_->transport.pending.empty()) {
                        auto frame = std::move(io_->transport.pending.front()); io_->transport.pending.pop_front();
                        bool keep{};
                        if (!frame.frame.empty() && frame.frame.front() == net::sealed_batch_kind) {
                            auto batch = net::decode_batch(frame.frame);
                            if (!batch) return DriverResult::failure(batch.error());
                            keep = batch.value().sender == frame.from && frame.from != io_->transport.self()
                                && frame.from < retained_offer_.seats.size() && batch.value().tick >= requested_status.barrier_tick
                                && batch.value().tick - requested_status.barrier_tick < core_session().input_horizon();
                        }
                        if (keep) retained_rejoin_frames_.push_back(std::move(frame));
                        else io_->transport.pending_bytes -= std::min(io_->transport.pending_bytes, frame.frame.size());
                    }
                    auto created = net::LockstepSession::create(*returning, io_->transport, std::move(seats), policy, &gate_);
                    if (!created) return DriverResult::failure(created.error());
                    rejoin_session_.emplace(std::move(created).value()); returning_adapter_ = std::move(fresh).value(); adapter_ = returning;
                    rejoin_session_->set_message_handler([](net::Received frame) {
                        if (!frame.frame.empty() && frame.frame[0] == net::setup_refuse_kind) {
                            auto refusal = net::decode_setup_refusal(frame.frame);
                            return core::Result<void>::failure(refusal ? refusal.value().diagnostic() : refusal.error());
                        }
                        return fail<void>("Unexpected recovery application frame.");
                    });
                    attach_stage_observer();
                    if (setup.departure_policy) core_session().enable_departures([this](sim::SeatCommandKey key) {
                        const auto* bound = static_cast<const LiveAdapter&>(core_session().active_adapter()).binding(key.seat);
                        if (!bound) return fail<Bytes>("Unbound departure seat.");
                        return net::encode_tactical_input(key, *bound, {{}, tactical::QuitPayload{}});
                    });
                    discard_recovery_commands();
                }
            }
            if (retry) { const std::lock_guard lock(mutex_); rejoin_requested_ = true; }
        }
        if (!adapter_->staging) adapter_->context = &context;
        auto& session = core_session();
        const auto status_before = session.reconnect_status();
        if (session.reconnect_available() && status_before.phase != net::ReconnectPhase::playing
            && (!last_repair_ms_ || now - *last_repair_ms_ >= 1000)) {
            auto repaired = io_->transport.tcp->repair_closed_links(tcp_rejoin_auth(status_before.epoch));
            if (!repaired) repair_notice_ = repair_diagnostic(repaired.error());
            else repair_notice_.reset();
            last_repair_ms_ = now;
        }
        if (session.resyncing() || io_->monitor->frozen() || session.reconnect_status().phase != net::ReconnectPhase::playing) discard_recovery_commands();
        for (const auto& command : queued_) {
            const auto& seat = setup_result().offer().seats[lobby_stream_ ? lobby_stream_->self() : io_->transport.self()];
            if (command.key.player_id != seat.player) return fail<bool>("Command belongs to another player's seat.");
            const tactical::SeatBinding binding{seat.seat, seat.player, true, false};
            const auto frontier = session.next_seal();
            auto submitted = session.submit([&](sim::SeatCommandKey key) {
                return net::encode_tactical_input(key, binding, {command.units, command.payload});
            }, command.key.tick);
            if (!submitted) {
                if (submitted.error().code == net::session_errors::admission) { discard_command(command); continue; }
                set_failure(submitted.error()); return DriverResult::failure(submitted.error());
            }
            std::function<void(std::uint64_t, std::uint64_t)> observer;
            { const std::lock_guard lock(input_epoch_mutex_); observer = submission_observer_; }
            if (observer) observer(command.key.sequence, submitted.value().tick);
            if (trace_) trace_->submitted(submitted.value(), command_kind(command.payload), context.completed_tick(), frontier,
                command.key.sequence, command.key.tick);
        }
        queued_.clear();
        std::vector<net::MatchOptions> speed_requests;
        { const std::lock_guard lock(mutex_); speed_requests.swap(speed_requests_); }
        for (const auto options : speed_requests) {
            auto submitted = session.submit([options](sim::SeatCommandKey) { return net::encode_match_options(options); });
            if (!submitted) {
                // A departure agreement can close admission between the UI request
                // and this drain. An optional speed change must not terminate it.
                if (submitted.error().code == net::session_errors::admission) {
                    const std::lock_guard lock(mutex_); view_.input_feedback = "The speed change was cancelled because synchronization changed."; continue;
                }
                set_failure(submitted.error()); return DriverResult::failure(submitted.error());
            }
        }
        // Close presentation admission before synchronous replay can begin.
        { const std::lock_guard lock(mutex_);
          if (session.resyncing()) { view_.phase = LanPhase::resyncing; view_.recovery = LanRecovery::resyncing; }
          else if (session.reconnect_status().phase != net::ReconnectPhase::playing)
              { view_.phase = LanPhase::reconnecting; view_.recovery = LanRecovery::held; }
          else if (io_->monitor->frozen()) view_.recovery = LanRecovery::desync; }
        auto pumped = session.pump(now);
        if (!pumped) {
            if (pumped.error().code.starts_with("EAWR-NET-102")) refuse_credentials(pumped.error());
            else set_failure(pumped.error());
            return DriverResult::failure(pumped.error());
        }
        if (io_->transport.credential_error) {
            const auto error = *io_->transport.credential_error; refuse_credentials(error);
            return DriverResult::failure(error);
        }
        if (const auto status = session.reconnect_status(); status.phase == net::ReconnectPhase::proving
            || status.phase == net::ReconnectPhase::playing) {
            // Prefix restoration made these immutable future batches admissible.
            // Their bytes stayed charged against the same recovery receive budget.
            while (!retained_rejoin_frames_.empty()) {
                io_->transport.pending.push_front(std::move(retained_rejoin_frames_.back()));
                retained_rejoin_frames_.pop_back();
            }
        }
        if (&session.active_adapter() != adapter_ || adapter_->staging) {
            auto* published = const_cast<LiveAdapter*>(dynamic_cast<const LiveAdapter*>(&session.active_adapter()));
            if (!published) return fail<bool>("Published recovery adapter has the wrong mode.");
            auto activated = activate_adapter(*published); if (!activated) return DriverResult::failure(activated.error());
        }
        const auto& completeness = session.waiting_for();
        const bool waiting = (completeness && !completeness->peers.empty()) || !io_->monitor->waiting_for_peers().empty();
        if (!waiting) waiting_since_.reset();
        else {
            if (!waiting_since_) waiting_since_ = now;
            if (completeness && !completeness->peers.empty()) waiting_since_ = std::min(*waiting_since_, completeness->since_ms);
        }
        {
            const std::lock_guard lock(mutex_);
            view_.peer_confirmed_tick = io_->monitor->peer_confirmed_tick();
            view_.link_stats.reset();
            for (net::PeerIndex peer = 0; peer < io_->transport.peer_count(); ++peer)
                if (peer != io_->transport.self()) {
                    if (const auto stats = io_->transport.link_stats(peer);
                        stats && (!view_.link_stats || stats->smoothed_rtt > view_.link_stats->smoothed_rtt)) view_.link_stats = stats;
                }
            if (view_.phase != LanPhase::ended) {
                view_.message.clear(); view_.diagnostic_code.clear();
                const auto reconnect = session.reconnect_status();
                view_.reconnect_enabled = session.reconnect_available();
                view_.rejoin_enabled = session.reconnect_available() && reconnect.phase == net::ReconnectPhase::held;
                if (reconnect.epoch) { view_.recovery_epoch = reconnect.epoch; view_.reconnect_seat = reconnect.peer; }
                view_.phase = session.resyncing() ? LanPhase::resyncing
                    : reconnect.phase != net::ReconnectPhase::playing ? LanPhase::reconnecting : LanPhase::running;
                view_.grace_ms = retained_offer_.recovery.grace_ms;
                view_.progress_timeout_ms = retained_offer_.recovery.progress_timeout_ms;
                view_.hard_cap_ms = retained_offer_.recovery.hard_cap_ms;
                view_.resync_timeout_ms = retained_offer_.recovery.resync_timeout_ms;
                view_.grace_remaining_ms = session.reconnect_grace_remaining_ms();
                const auto previous_recovery = view_.recovery;
                if (session.resyncing()) view_.recovery = LanRecovery::resyncing;
                else if (io_->monitor->frozen()) view_.recovery = LanRecovery::desync;
                else switch (reconnect.phase) {
                case net::ReconnectPhase::agreeing:
                case net::ReconnectPhase::held: view_.recovery = LanRecovery::held; break;
                case net::ReconnectPhase::catching_up: view_.recovery = LanRecovery::catching_up; break;
                case net::ReconnectPhase::proving: view_.recovery = LanRecovery::proving; break;
                case net::ReconnectPhase::playing:
                    view_.recovery = previous_recovery == LanRecovery::held || previous_recovery == LanRecovery::catching_up
                        || previous_recovery == LanRecovery::proving || previous_recovery == LanRecovery::resyncing
                        || previous_recovery == LanRecovery::desync || previous_recovery == LanRecovery::recovered
                        ? LanRecovery::recovered : LanRecovery::none;
                    break;
                }
                if (io_->monitor->report()) capture_diagnostic(*io_->monitor->report());
                else if (io_->monitor->recovered_report()) capture_diagnostic(*io_->monitor->recovered_report());
                if (reconnect.phase == net::ReconnectPhase::playing) repair_notice_.reset();
                if (repair_notice_) {
                    view_.diagnostic_code = repair_notice_->code;
                    view_.message.clear(); // Curated recovery text; raw transport details stay local.
                }
                else if (session.reconnect_notice()) {
                    if (session.reconnect_notice()->code != net::reconnect_errors::unavailable) view_.diagnostic_code = session.reconnect_notice()->code;
                    if (session.reconnect_notice()->code == net::reconnect_errors::unavailable)
                        view_.reconnect_warning = "Reconnect is unavailable for this match. Play can continue.";
                }
            }
            view_.service_notice.clear();
            if (view_.phase != LanPhase::ended) {
                if (const auto endpoint = online_endpoint_.lock()) {
                    const auto notice_now = std::chrono::steady_clock::now();
                    const auto status = endpoint->status();
                    view_.service_notice = online_lease_notice(endpoint->lease(), notice_now, status ? "" : status.error().code);
                    const auto credentials = online_credential_notice(endpoint->turn_credentials(), online_paired_at_, notice_now);
                    if (!credentials.empty()) view_.service_notice += (view_.service_notice.empty() ? "" : "\n") + credentials;
                }
                if (online_options_ && adapter_) {
                    const auto length = match_length_notice(adapter_->completed_tick(), retained_offer_.final_tick_count);
                    if (!length.empty()) view_.service_notice += (view_.service_notice.empty() ? "" : "\n") + length;
                }
            }
            if (view_.phase == LanPhase::running && waiting_since_ && now > *waiting_since_ && now - *waiting_since_ > 3000) {
                // MP-33: completeness wait is a UI message, never a pause or elapsed sim time.
                view_.recovery = LanRecovery::waiting;
                view_.message = view_.human_owners.size() > 2 || view_.human_owners[0] == view_.human_owners[1]
                    ? "Waiting for human seats..." : "Waiting for " + view_.peer_name + "...";
            }
            if (io_->monitor->frozen() && !session.resyncing() && !retained_options_.resync) {
                view_.phase = LanPhase::ended; view_.reason = LanReason::desync; view_.recovery = LanRecovery::failed;
                if (const auto& report = io_->monitor->report()) { capture_diagnostic(*report); }
            }
        }
        for (const auto& event : pumped.value().departure_events) {
            const auto* bound = adapter_->binding(event.key.seat);
            if (!bound) continue;
            net::DepartureOutcome departure{event, bound->player, bound->player, net::DepartureState::continues};
            if (context.departures) for (const auto& control : context.departures())
                if (control.player == bound->player && control.tick == event.key.tick) {
                    departure.controller = control.controller;
                    departure.state = control.basic_ai ? net::DepartureState::continues_with_ai : net::DepartureState::continues;
                }
            if (const auto& outcome = context.snapshot()->outcome(); outcome && outcome->condition == tactical::VictoryCondition::intentional_quit) {
                const auto local = std::find_if(setup.players.begin(), setup.players.end(),
                    [&](const auto& player) { return player.player_id == local_player(); });
                departure.state = local != setup.players.end() && local->team_id == outcome->winner_team
                    ? net::DepartureState::won_by_departure : net::DepartureState::match_ended;
            }
            const std::lock_guard lock(mutex_); view_.departure = departure;
            if (event.key.seat == setup_result().offer().seats[lobby_stream_ ? lobby_stream_->self() : io_->transport.self()].seat) {
                view_.phase = LanPhase::ended; view_.reason = LanReason::peer_left;
                view_.message = "You left the match.";
            }
        }
        if (adapter_->advanced && context.snapshot()->outcome()) {
            const auto& outcome = *context.snapshot()->outcome();
            { const std::lock_guard lock(mutex_);
              if (context.completed_tick() >= outcome.end_tick) view_.phase = LanPhase::ended;
              const auto local = std::find_if(setup.players.begin(), setup.players.end(),
                  [&](const auto& player) { return player.player_id == local_player_unlocked(); });
              view_.victory = local != setup.players.end() && local->team_id == outcome.winner_team;
              if (outcome.condition == tactical::VictoryCondition::intentional_quit) {
                  view_.reason = LanReason::peer_left;
                  view_.message = *view_.victory ? "Victory - opponent left." : "Defeat - you left the match.";
              }
            }
            if (context.completed_tick() >= outcome.end_tick) {
                auto ended = io_->monitor->finish(context.canonical_state());
                if (!ended) { set_failure(ended.error()); return DriverResult::failure(ended.error()); }
            }
        }
        if (view().phase == LanPhase::ended) {
            erase_credentials();
            auto closed = pump_close();
            if (!closed) return DriverResult::failure(closed.error());
        }
        return DriverResult::success(std::exchange(adapter_->advanced, false));
    }
    tactical::PlayerId local_player_unlocked() const { return view_.player; }
    std::optional<std::uint64_t> pending_input_epoch() {
        std::function<std::optional<std::uint64_t>()> provider;
        { const std::lock_guard lock(input_epoch_mutex_); provider = input_epoch_provider_; }
        return provider ? provider() : std::nullopt;
    }
    std::function<void(std::uint64_t, std::uint64_t)> submission_observer_;
    std::function<void(std::uint64_t)> discard_observer_;
    std::mutex input_epoch_mutex_;
    std::function<std::optional<std::uint64_t>()> input_epoch_provider_;
    std::filesystem::path diagnostic_root_;
    net::DiagnosticSink diagnostic_sink_;
    std::optional<net::DiagnosticBundle> diagnostic_bundle_;
    bool diagnostic_saved_{};
    std::uint64_t input_delay_{3}, monitor_delay_{3};
    net::SetupRecoveryPolicy recovery_policy_{false, true};
    std::shared_ptr<net::LanReconnectCredentials> credential_state_{std::make_shared<net::LanReconnectCredentials>()};
    LanCredentialProvider credentials_;
    bool default_credentials_{};
    net::SessionOptions retained_options_;
    net::SetupOffer retained_offer_;
    decltype(net::ResyncOptions::rebuild) recovery_factory_;
    std::optional<net::LockstepSession> rejoin_session_;
    std::unique_ptr<net::SessionAdapter> returning_adapter_;
    bool rejoin_requested_{};
    std::optional<std::uint64_t> last_repair_ms_;
    std::optional<core::Diagnostic> repair_notice_;
    std::deque<net::Received> retained_rejoin_frames_;
    bool smoke_progress_{};
    std::optional<std::uint64_t> last_progress_;
    std::optional<std::uint64_t> offered_delay_, waiting_since_;
    mutable std::mutex mutex_;
    LanView view_;
    bool map_port_{};
    std::unique_ptr<net::PortMapping> mapping_;
    std::unique_ptr<TerminalIo> io_{std::make_unique<TerminalIo>()};
    bool handed_over_{}, accepted_{}, abort_requested_{};
    net::SetupProviders deferred_providers_;
    std::optional<skirmish::FixtureOptions> pending_, offered_;
    GatedMonitor gate_;
    LiveAdapter* adapter_{};
    platform::LiveStepContext* current_context_{};
    std::optional<net::SetupSession> setup_session_;
    std::optional<net::SetupResult> runtime_;
    std::vector<tactical::PlayerCommand> queued_;
    std::vector<tactical::PlayerId> owners_{1, 2};
    std::optional<core::Diagnostic> roster_error_;
    std::deque<net::Received> early_;
    std::size_t early_bytes_{};
    std::vector<net::MatchOptions> speed_requests_;
    net::MatchSpeedWindow speed_window_;
    std::shared_ptr<LanTrace> trace_;
};
}
net::LobbyReason lan_lobby_reason(const core::Diagnostic& error) {
    for (unsigned value = 0; value <= static_cast<unsigned>(net::LobbyReason::recovery_unavailable); ++value)
        if (error.code == net::lobby_diagnostic(static_cast<net::LobbyReason>(value), "").code) return static_cast<net::LobbyReason>(value);
    return net::LobbyReason::invalid_choice;
}
std::string lan_admission_name(const net::LobbySnapshot& room, net::PeerIndex peer) {
    const auto base = std::to_string(peer + 1);
    auto candidate = base;
    for (std::size_t suffix = 1; std::any_of(room.seats.begin(), room.seats.end(), [&](const auto& seat) { return seat.selection.name == candidate; }); ++suffix)
        candidate = base + "-" + std::to_string(suffix);
    return candidate;
}
std::shared_ptr<LanMatch> make_online_match(OnlineMatchOptions options, std::filesystem::path root, std::uint64_t delay) {
    auto result = std::make_shared<TcpLanMatch>(std::move(root), delay, false, std::filesystem::path{});
    result->configure_online(std::move(options));
    return result;
}
void shutdown_lan_drains() { drain_workers().shutdown(); }
std::shared_ptr<LanMatch> make_lan_match(std::filesystem::path root, std::uint64_t delay, bool smoke_progress, std::filesystem::path trace) {
    return std::make_shared<TcpLanMatch>(std::move(root), delay, smoke_progress, std::move(trace));
}
} // namespace eawr::presentation::godot_backend
