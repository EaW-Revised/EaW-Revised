#pragma once

#include "lan_match.hpp"
#include "lan_diagnostics.hpp"
#include "online_match.hpp"
#include "match_transport.hpp"
#include "skirmish_prepare.hpp"
#include "eawr/net/desync_monitor.hpp"
#include "eawr/net/match_pause.hpp"
#include "eawr/net/match_pause_replay.hpp"
#include "eawr/net/setup.hpp"
#include "eawr/net/lan_credentials.hpp"
#include "eawr/net/tactical_input.hpp"
#include "eawr/net/tcp_transport.hpp"
#include "eawr/presentation/ui/time_controls.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <set>
#include <utility>

namespace eawr::presentation::godot_backend::lan_detail {

namespace tactical = sim::tactical;
using Bytes = std::vector<std::uint8_t>;
using DriverResult = core::Result<bool>;
std::string_view command_kind(const tactical::CommandPayload& payload);
struct InputTime { std::uint64_t tick{}, frame{}; std::int64_t epoch{}, steady{}; std::uint64_t ui_epoch{}; };
InputTime input_time(std::uint64_t tick, std::uint64_t frame = 0);
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
core::Diagnostic diagnostic(std::string code, std::string message);
template<class T> core::Result<T> fail(std::string message) {
    return core::Result<T>::failure(diagnostic("EAWR-NET-0600", std::move(message)));
}
template<class T> core::Result<T> lobby_fail(net::LobbyReason reason, std::string message) {
    return core::Result<T>::failure(net::lobby_diagnostic(reason, std::move(message)));
}
core::Result<skirmish::FixtureOptions> fixture_options(const net::SetupOffer& offer, std::span<const std::string> factions);
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
    std::function<bool(const net::Received&)> credential_filter;
    std::optional<core::Diagnostic> credential_error;
    std::shared_ptr<LanDiagnostics> diagnostics;
    void observe_links() {
        if (diagnostics && tcp) diagnostics->links(*tcp, [this](net::PeerIndex peer) {
            const auto pair = tcp->mesh() ? tcp->pair(peer) : net::GnsPairDiagnostic{};
            if (pair.code) return "EAWR-NET-" + std::to_string(pair.code);
            return tcp->diagnostic() ? tcp->diagnostic()->code : std::string("none");
        });
    }
    void observe_departure(std::string_view event, net::PeerIndex peer, std::span<const std::uint8_t> bytes) {
        if (!diagnostics || bytes.size() < 6 || bytes.front() != net::departure_proposal_kind) return;
        const auto reason = bytes[5] == 1 ? "disconnect" : bytes[5] == 2 ? "timeout" : bytes[5] == 3 ? "pair-loss" : "invalid";
        diagnostics->emit(event, reason, peer);
    }
    core::Result<void> send(net::PeerIndex peer, std::span<const std::uint8_t> bytes) override {
        auto result = tcp->send(peer, bytes);
        observe_links();
        if (result) observe_departure("departure-sent", peer, bytes);
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
            observe_links();
            if (next) observe_departure("departure-received", next->from, next->frame);
            if (next && !next->frame.empty() && (next->frame[0] == net::lan_credentials_kind || next->frame[0] == net::lan_credentials_ack_kind)) {
                if (credential_filter && !credential_filter(*next)) { net::erase_reconnect_secret(next->frame); continue; }
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
    std::uint64_t edge_identity(net::PeerIndex peer) const override { return tcp ? tcp->edge_identity(peer) : 0; }
    void fail_repair(net::PeerIndex peer, unsigned code) override { if (tcp) tcp->fail_repair(peer, code); }
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
DrainWorkers& drain_workers();
void finish_io(std::unique_ptr<TerminalIo> io);
// Gate advancement without bypassing the core receive path while presentation holds.
class GatedMonitor final : public net::SessionMonitor {
public:
    net::DesyncMonitor* monitor{};
    bool advance_allowed{};
    core::Result<void> bind(net::DesyncMonitor& value) {
        monitor = &value;
        if (!pending_peer_) return core::Result<void>::success();
        const auto peer = *pending_peer_; pending_peer_.reset();
        return monitor->on_link_repair(peer, std::exchange(pending_ownership_, {}));
    }
    void reset() { monitor = nullptr; pending_peer_.reset(); pending_ownership_ = {}; }
    core::Result<void> on_link_repair(net::PeerIndex peer, std::function<bool(net::PeerIndex)> ownership = {}) override {
        if (monitor) return monitor->on_link_repair(peer, std::move(ownership));
        // The host creates setup before loading binds the concrete monitor.
        // Preserve the creation self-call, including send ownership, until bind.
        if (pending_peer_ && *pending_peer_ != peer)
            return core::Result<void>::failure(diagnostic("EAWR-NET-1790", "Unbound monitor received a peer repair."));
        pending_peer_ = peer; pending_ownership_ = std::move(ownership);
        return core::Result<void>::success();
    }
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
private:
    std::optional<net::PeerIndex> pending_peer_;
    std::function<bool(net::PeerIndex)> pending_ownership_;
};
class LiveAdapter final : public net::SessionAdapter {
public:
    explicit LiveAdapter(tactical::TacticalSetup, std::vector<net::SetupSeat> seats, std::shared_ptr<LanTrace> trace)
        : trace_(std::move(trace)) {
        for (const auto& seat : seats) bindings_.push_back({seat.seat, seat.player, true, false});
    }
    platform::LiveStepContext* context{};
    std::shared_ptr<platform::LiveStepContext> staging;
    std::function<core::Result<void>(LiveAdapter&)> activate_if_current;
    std::function<core::Result<void>()> service_io;
    bool advanced{};
    bool record_journal{true};
    std::function<void(net::MatchOptions, std::uint64_t)> options_applied;
    void bind_live_context(platform::LiveStepContext& live, const std::shared_ptr<LanTrace>& trace) {
        context = &live; trace_ = trace;
    }
    std::uint64_t completed_tick() const override { return clock.completed; }
    std::uint64_t session_epoch(std::uint64_t world_epoch) const {
        // A drained input keeps its producer's completed frame as the anchor: a later drain steps back by the world frames since.
        const auto world = context->completed_tick();
        if (clock.paused || world_epoch >= world) return clock.completed + (world_epoch > world && !clock.paused ? world_epoch - world : 0);
        const auto behind = world - world_epoch;
        return clock.completed > behind ? clock.completed - behind : 0;
    }
    std::uint64_t world_boundary(std::uint64_t session_boundary) const {
        if (session_boundary == 0) return 0;
        if (session_boundary <= world_boundaries_.size()) return world_boundaries_[static_cast<std::size_t>(session_boundary - 1)];
        return context->completed_tick() + (clock.paused || session_boundary <= clock.completed ? 0 : session_boundary - clock.completed);
    }
    net::MatchPauseClock clock;
    net::MatchPauseReplay recording;
    std::optional<sim::SeatId> clock_refused;
    // The last installed tick answered a pause request of this seat: it carried one of its pause inputs, or the hold flipped.
    bool pause_answered(sim::SeatId own) const {
        return clock.paused != pause_before_ || std::any_of(tick_inputs_.begin(), tick_inputs_.end(), [&](const auto& input) {
            return input.key.seat == own && net::is_match_pause(input.bytes);
        });
    }
    std::uint64_t next_control_ms{};
    bool control_due(std::uint64_t now) const { return !clock.paused || now >= next_control_ms; }
    void completed_at(std::uint64_t now) { if (clock.paused) next_control_ms = now + 33; }
    std::string combined_hash(std::string_view world) const {
        auto bytes = control_state();
        bytes.insert(bytes.end(), world.begin(), world.end());
        return core::sha256_hex(bytes);
    }
    std::string state_hash() const override { return combined_hash(context->state_hash()); }
    Bytes canonical_state() const override {
        auto bytes = context->canonical_state();
        const auto control = control_state(); bytes.insert(bytes.end(), control.begin(), control.end());
        return bytes;
    }
    const tactical::SeatBinding* binding(sim::SeatId seat) const {
        const auto found = std::find_if(bindings_.begin(), bindings_.end(), [&](const auto& bound) { return bound.seat == seat; });
        return found == bindings_.end() ? nullptr : &*found;
    }
    core::Result<void> validate(const net::SeatInput& input) const override {
        const auto* bound = binding(input.key.seat);
        if (!bound || !bound->active || bound->observer || departed_.contains(bound->seat)) return fail<void>("Inactive human seat.");
        if (net::is_match_pause(input.bytes)) {
            const auto pause = net::decode_match_pause(input.bytes);
            return pause ? core::Result<void>::success() : core::Result<void>::failure(pause.error());
        }
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
        pause_before_ = clock.paused;
        trace_commands_.clear(); clock_refused.reset();
        tick_inputs_.assign(canonical.begin(), canonical.end());
        clock.limiter.expire(clock.completed);
        for (const auto& input : canonical) {
            if (input.key.tick != clock.completed) return fail<void>("Input is not stamped for this session tick.");
            const auto* bound = binding(input.key.seat);
            if (!bound || !bound->active || bound->observer) return fail<void>("Inactive human seat.");
            if (departed_.contains(bound->seat)) continue;
            if (net::is_match_pause(input.bytes)) {
                auto pause = net::decode_match_pause(input.bytes);
                if (!pause) return core::Result<void>::failure(pause.error());
                const auto changed = clock.apply(pause.value(), input.key);
                if (changed == net::MatchPauseClock::Change::throttled) clock_refused = input.key.seat;
                continue;
            }
            if (net::is_match_options(input.bytes)) {
                auto decoded = net::decode_match_options(input.bytes);
                if (!decoded) return core::Result<void>::failure(decoded.error());
                pending_options_ = decoded.value();
                continue;
            }
            auto decoded = net::decode_tactical_input(input, *bound);
            if (!decoded) return core::Result<void>::failure(decoded.error());
            // The network retires the voter immediately. Its world departure waits
            // with other world inputs until an actual tactical frame is admitted.
            if (std::holds_alternative<tactical::QuitPayload>(decoded.value().payload)) departed_.insert(bound->seat);
            if (pending_bytes_ > net::max_batch_bytes - input.bytes.size()) return fail<void>("Paused order queue exceeded its byte budget.");
            pending_bytes_ += input.bytes.size(); pending_.push_back(input);
        }
        // Once no humans remain the obsolete human hold cannot block AI fallback.
        if (clock.paused && std::none_of(bindings_.begin(), bindings_.end(), [&](const auto& seat) {
            return seat.active && !seat.observer && !departed_.contains(seat.seat);
        })) clock.paused = false;
        return core::Result<void>::success();
    }
    core::Result<Bytes> canonical_input() const override {
        Bytes bytes(net::match_pause_domain.begin(), net::match_pause_domain.end());
        const auto put = [&](std::uint64_t value, unsigned count) {
            for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
        };
        put(tick_inputs_.size(), 4);
        for (const auto& input : tick_inputs_) {
            put(input.key.tick, 8); put(input.key.seat.value, 4); put(input.key.sequence, 8); put(input.bytes.size(), 4);
            bytes.insert(bytes.end(), input.bytes.begin(), input.bytes.end());
        }
        if (advanced && context->canonical_input) {
            auto world = context->canonical_input(); if (!world) return world;
            bytes.insert(bytes.end(), world.value().begin(), world.value().end());
        }
        return core::Result<Bytes>::success(std::move(bytes));
    }
    core::Result<net::CompletedTick> step() override {
        if (staging && activate_if_current) { auto activated = activate_if_current(*this); if (!activated) return core::Result<net::CompletedTick>::failure(activated.error()); }
        if (staging && service_io) { auto serviced = service_io(); if (!serviced) return core::Result<net::CompletedTick>::failure(serviced.error()); }
        if (!context->advance_allowed) return fail<net::CompletedTick>("Loading barrier attempted to advance.");
        advanced = false;
        if (pause_before_ != clock.paused && context->rebase_pacing) context->rebase_pacing();
        if (!clock.paused) {
            std::vector<tactical::PlayerCommand> commands;
            for (const auto& input : pending_) {
                const auto* bound = binding(input.key.seat);
                auto decoded = net::decode_tactical_input(input, *bound);
                if (!decoded) return core::Result<net::CompletedTick>::failure(decoded.error());
                if (std::holds_alternative<tactical::QuitPayload>(decoded.value().payload)
                    && std::any_of(bindings_.begin(), bindings_.end(), [&](const auto& seat) {
                        return seat.player == bound->player && seat.active && !seat.observer && !departed_.contains(seat.seat);
                    })) continue;
                auto admitted = tactical::admit_human_command(*bound, decoded.value());
                if (!admitted) return core::Result<net::CompletedTick>::failure(admitted.error());
                auto command = std::move(admitted).value().command;
                command.key.tick = context->completed_tick();
                command.key.sequence = sequences_[bound->player]++;
                if (trace_) trace_commands_.emplace_back(input.key, command_kind(command.payload));
                commands.push_back(std::move(command));
            }
            std::sort(commands.begin(), commands.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
            auto installed = context->install(commands);
            if (!installed) return core::Result<net::CompletedTick>::failure(installed.error());
            pending_.clear(); pending_bytes_ = 0;
            auto result = context->step();
            if (!result) return core::Result<net::CompletedTick>::failure(result.error());
            advanced = !staging;
        }
        if (pending_options_) {
            if (context->set_target_rate) context->set_target_rate(presentation::ui::speed_step_rates[pending_options_->tactical_speed]);
            if (!staging && options_applied) options_applied(*pending_options_, clock.completed);
            pending_options_.reset();
        }
        for (const auto& [key, kind] : trace_commands_) trace_->executed(key, kind, context->completed_tick());
        ++clock.completed;
        world_boundaries_.push_back(context->completed_tick());
        const auto hash = combined_hash(context->world_hash ? context->world_hash() : context->state_hash());
        if (record_journal && recording_bytes_ <= net::match_pause_replay_limit) {
            std::size_t size = 76;
            for (const auto& input : tick_inputs_) size += 16 + input.bytes.size();
            recording_bytes_ += size;
            if (recording_bytes_ <= net::match_pause_replay_limit) recording.rows.push_back({context->completed_tick(), tick_inputs_, hash});
            else recording.rows.clear();
        }
        return core::Result<net::CompletedTick>::success({clock.completed, hash, static_cast<bool>(context->world_hash)});
    }
    core::Result<Bytes> record_controls() const {
        if (recording_bytes_ > net::match_pause_replay_limit) return fail<Bytes>("Session recording exceeded its 64 MiB budget.");
        return net::write_match_pause_replay(recording);
    }
    bool is_departure(const net::SeatInput& input) const override {
        const auto* bound = binding(input.key.seat);
        if (!bound) return false;
        auto decoded = net::decode_tactical_input(input, *bound);
        return decoded && std::holds_alternative<tactical::QuitPayload>(decoded.value().payload);
    }
private:
    Bytes control_state() const {
        auto bytes = clock.canonical();
        const auto put = [&](std::uint64_t value, unsigned count) {
            for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
        };
        put(sequences_.size(), 4);
        for (const auto& [player, sequence] : sequences_) { put(player, 4); put(sequence, 8); }
        put(departed_.size(), 4); for (const auto seat : departed_) put(seat.value, 4);
        put(pending_.size(), 4);
        for (const auto& input : pending_) {
            put(input.key.tick, 8); put(input.key.seat.value, 4); put(input.key.sequence, 8); put(input.bytes.size(), 4);
            bytes.insert(bytes.end(), input.bytes.begin(), input.bytes.end());
        }
        return bytes;
    }
    std::vector<std::uint64_t> world_boundaries_;
    bool pause_before_{};
    std::vector<net::SeatInput> pending_, tick_inputs_;
    std::size_t pending_bytes_{}, recording_bytes_{};
    std::vector<tactical::SeatBinding> bindings_;
    std::set<sim::SeatId> departed_;
    std::map<tactical::PlayerId, std::uint64_t> sequences_;
    std::optional<net::MatchOptions> pending_options_;
    std::shared_ptr<LanTrace> trace_;
    std::vector<std::pair<sim::SeatCommandKey, std::string_view>> trace_commands_;
};
// Playback consumes control-only boundaries without wall waits. The world
// replay remains the sole source of actual player/engine commands; the same
// adapter reconstructs and checks the network clock, queue and limiter state.
class LivePauseReplay final {
public:
    LivePauseReplay(tactical::TacticalSetup setup, std::vector<net::SetupSeat> seats, net::MatchPauseReplay journal)
        : adapter_(std::move(setup), std::move(seats), nullptr), journal_(std::move(journal)) { adapter_.record_journal = false; }
    DriverResult step(platform::LiveStepContext& context, std::span<const tactical::PlayerCommand> recorded) {
        if (!context.advance_allowed) return DriverResult::success(false);
        recorded_.insert(recorded_.end(), recorded.begin(), recorded.end());
        const auto before = context.completed_tick();
        auto playback = context;
        playback.install = [](std::span<const tactical::PlayerCommand>) { return core::Result<void>::success(); };
        playback.step = [&] {
            auto installed = context.install(recorded_); if (!installed) return installed;
            recorded_.clear(); return context.step();
        };
        adapter_.context = &playback;
        struct Unbind { LiveAdapter& adapter; ~Unbind() { adapter.context = nullptr; } } unbind{adapter_};
        while (cursor_ < journal_.rows.size()) {
            const auto& row = journal_.rows[cursor_];
            // One actual world step per live call; trailing paused boundaries
            // are still checked before a caller may stop at the last world tick.
            if (context.completed_tick() != before && row.world_tick > context.completed_tick()) break;
            for (const auto& input : row.inputs) { auto valid = adapter_.validate(input); if (!valid) return DriverResult::failure(valid.error()); }
            auto installed = adapter_.install(row.inputs); if (!installed) return DriverResult::failure(installed.error());
            auto stepped = adapter_.step(); if (!stepped) return DriverResult::failure(stepped.error());
            if (context.completed_tick() != row.world_tick || stepped.value().state_sha256 != row.state_sha256)
                return fail<bool>("Multiplayer replay session/world boundary or digest differs from its journal.");
            ++cursor_;
        }
        return DriverResult::success(context.completed_tick() != before);
    }
    [[nodiscard]] bool complete() const { return cursor_ == journal_.rows.size(); }
private:
    LiveAdapter adapter_;
    net::MatchPauseReplay journal_;
    std::size_t cursor_{};
    std::vector<tactical::PlayerCommand> recorded_;
};

class TcpLanMatch final : public LanMatch, public std::enable_shared_from_this<TcpLanMatch> {
public:
    void set_diagnostic_logger(std::function<void(std::string_view)> logger) override;
    void set_recovery_policy(net::SetupRecoveryPolicy policy, LanCredentialProvider credentials = {}) override;
    bool rejoin() override;
    bool request_speed(std::uint32_t step) override;
    bool request_pause(bool paused) override;
    core::Result<Bytes> record_pause_replay() const override { return adapter_ ? adapter_->record_controls() : fail<Bytes>("No session recording."); }
    explicit TcpLanMatch(std::filesystem::path root, std::uint64_t delay, bool smoke_progress, std::filesystem::path trace);
    bool lobby_enabled() const override;
    void configure_lobby(std::string name, std::uint32_t colour, std::vector<skirmish::SetupMap> maps, LanLobbyBuilder builder) override;
    bool lobby_select(net::LobbySelection selection) override;
    bool lobby_assign(net::PeerIndex peer, net::LobbySelection selection) override;
    bool lobby_game(const skirmish::FixtureOptions& options) override;
    bool lobby_ready(bool ready) override;
    bool lobby_chat(std::string text) override;
    bool lobby_kick(net::PeerIndex peer) override;
    void lobby_leave() override;
    std::optional<LanLobbyState> lobby_state() const override;
    void configure_galactic(std::shared_ptr<GalacticLobbyBackend> backend) override;
    bool set_galactic_game(net::GalacticLobbyOptions options) override;
    bool lobby_galactic_game(const net::GalacticLobbyOptions& options) override;
    bool galactic_room() const override;
    std::optional<GalacticLobbyStart> galactic_start() const override;
    std::shared_ptr<GalacticLaunch> take_galactic_launch() override;
private:
    bool lobby_mutable() const;
    void flush_lobby_closes();
    bool lobby_feedback(core::Result<void> result);
    core::Result<void> validate_lobby(const net::LobbySnapshot& state, bool starting) const;
    void clear_mesh_repair_material();
    bool current_mesh_credentials(const net::Received& frame);
    core::Result<void> provision_mesh_repair(const net::SetupOffer& offer, std::uint64_t revision);
    net::LobbyHooks lobby_hooks();
    void reset_lobby();
    void start_lobby();
    void pump_lobby(const skirmish::FixtureOptions& selected);
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
    bool ready_pending_{}, lobby_setup_accepted_{}, lobby_room_committed_{};
    std::uint64_t identity_retry_at_ms_{};
    std::set<net::PeerIndex> removed_peers_, closing_peers_;
    std::map<net::PeerIndex, std::uint64_t> known_edges_;
    std::optional<net::LobbyNotice> lobby_notice_;
    std::optional<LanLobbyState> last_lobby_state_;
    // GCMP-07: the Galactic Conquest mode of the room (the host's first options; the Start seam once the room froze).
    std::shared_ptr<GalacticLobbyBackend> galactic_;
    std::optional<net::GalacticLobbyOptions> galactic_host_game_;
    std::optional<GalacticLobbyStart> galactic_start_;
    std::optional<std::uint64_t> galactic_flush_started_;
    core::Result<void> validate_galactic_lobby(const net::LobbySnapshot& state, bool starting) const;
    void start_galactic_lobby();
    void pump_galactic_start();
public:

    void set_public_invitation(bool value) override;
    void configure_online(OnlineMatchOptions options);
private:
    std::optional<OnlineMatchOptions> online_options_;
    std::shared_ptr<OnlineAdmission> online_admission_, online_retiring_;
    std::optional<net::TcpRejoinAuth> mesh_repair_auth_; // Native room-slot keys, I/O owner only; never published.
    std::optional<std::uint64_t> mesh_material_revision_;
    net::MeshSeats mesh_retired_{};
    // Observed after the room opens only to read the service lease and credential age for notices (ticket E) and to release
    // a seat before Start. Weak on purpose: the transport owns signalling, and closing it must release the service session.
    std::weak_ptr<net::NetworkGnsRoom> online_endpoint_;
    std::chrono::steady_clock::time_point online_paired_at_{};
    std::chrono::steady_clock::time_point online_sample_at_{};
    std::optional<std::chrono::steady_clock::time_point> online_snapshot_deadline_;
    std::string online_alias_;
    // The attached room member while its roster has not arrived yet (pump_online).
    std::shared_ptr<net::NetworkGnsRoom> online_pending_;
    std::chrono::steady_clock::time_point online_pending_deadline_{};
    // Seats the single-decider agreement removed, as curated lines for LanView::mesh_notice (oldest first, bounded).
    std::size_t mesh_seen_{};
    LanDepartureNotices mesh_notices_;
    std::string mesh_left_text_;
    // The session's seats are positions in the staged roster (host first, admission order), not transport slots.
    std::string seat_name(net::PeerIndex position) const;
    core::Result<void> arm_mesh_repair(const net::LockstepSession& session);
    core::Result<void> retire_mesh_seat(net::PeerIndex dense);
    std::string mesh_repair_message() const;
    // "Connection to <name> could not be restored. You have left the match." for the seat the agreement removed.
    std::string removed_seat_text(const net::LockstepSession& session) const;
    void append_mesh_notice(const net::MeshIncidentResult& result, std::uint64_t now_ms);
    // The room scope a failed or cancelled request opened is closed on a drain worker: its last reference joins a worker
    // that may wait out a pending connect.
    static void retire_room(std::shared_ptr<net::NetworkGnsRoom> room);
    std::uint64_t room_edge(net::PeerIndex peer) const;
    void sample_online_route();
    std::string admission_identity(net::PeerIndex peer, std::uint64_t edge) const;
    // Pair events before Start (docs/net-online-npeer.md, section 3). False once the match has failed.
    bool drain_mesh_events();
    void begin_online(bool hosting, std::string code, std::optional<net::OnlineRoomAdmission> admission = {});
    bool pump_online();
public:
    void configure_factions(std::vector<std::string> factions) override;
    void configure_seats(std::vector<tactical::PlayerId> owners) override;
    void configure_staging_capacity(bool enabled) override { if (!handed_over_) capacity_staging_ = enabled; }
    ~TcpLanMatch() override;
    void set_port_mapping(bool enabled) override;
    void host(std::uint16_t port) override;
    void join_admitted(net::OnlineRoomAdmission admission) override;
    void join(std::string endpoint) override;
    // No GNS line reaches a logger after the match that installed it is cancelled or gone.
    void log_gns_sink(std::string_view action, std::uint64_t generation);
    void install_gns_log();
    void release_gns_log();
    void cancel() override;
    bool request_abort() override;
    void complete_abort() override;
    bool save_diagnostic_details() override;
    void acknowledge_recovery() override;
    void acknowledge_input_feedback() override;
    void set_discard_observer(std::function<void(std::uint64_t)> observer) override;
    void discard_command(const tactical::PlayerCommand& command);
    void discard_recovery_commands();
    void capture_diagnostic(const net::DesyncReport& report);
    void pump_setup(const skirmish::FixtureOptions& selected) override;
    void pump_setup_impl(const skirmish::FixtureOptions& selected);
    void accept() override;
    void start(const skirmish::FixtureOptions& selected) override;
    std::optional<skirmish::FixtureOptions> take_start() override;
    std::optional<skirmish::FixtureOptions> offered_setup() const override;
    LanView view() const override;
    tactical::PlayerId local_player() const override;
    void note_input(std::uint64_t tick, std::uint64_t frame, std::uint64_t ui_epoch) override;
    void note_native_input(std::uint64_t tick, std::uint64_t epoch) override;
    void note_issue(std::uint64_t sequence, std::uint64_t epoch) override;
    void set_submission_observer(std::function<void(std::uint64_t, std::uint64_t)> observer) override;
    void set_input_epoch_provider(std::function<std::optional<std::uint64_t>()> provider) override;
    platform::LiveStepDriver driver(const tactical::TacticalSetup& setup) override;
private:
    void erase_credentials() noexcept;
    void refuse_credentials(const core::Diagnostic& error);
    core::Result<void> service_recovery_io();
    net::TcpRejoinAuth tcp_rejoin_auth(std::uint64_t epoch) const;
    net::SetupResult& setup_result();
    net::LockstepSession& core_session();
    net::PeerIndex diagnostic_peer(sim::SeatId seat);
    void attach_stage_observer();
    core::Diagnostic repair_diagnostic(core::Diagnostic error) const;
    core::Result<void> activate_adapter(LiveAdapter& adapter);
    void update_desync();
    core::Result<void> pump_close();
    void progress(const platform::LiveStepContext& context, std::uint64_t now);
    void input_progress(std::string_view reason);
    void set_failure(const core::Diagnostic& diagnostic, bool terminal = true);
    DriverResult drive(const tactical::TacticalSetup& setup, platform::LiveStepContext& context,
        std::span<const tactical::PlayerCommand> commands, std::uint64_t now);
    std::optional<DriverResult> drive_prepare(const tactical::TacticalSetup& setup, platform::LiveStepContext& context,
        std::span<const tactical::PlayerCommand> commands);
    std::optional<DriverResult> drive_commit(const tactical::TacticalSetup& setup, platform::LiveStepContext& context, std::uint64_t now);
    std::optional<DriverResult> drive_rejoin(const tactical::TacticalSetup& setup, platform::LiveStepContext& context, std::uint64_t now);
    std::optional<DriverResult> drive_submit(platform::LiveStepContext& context, std::uint64_t now);
    DriverResult drive_session(const tactical::TacticalSetup& setup, platform::LiveStepContext& context, std::uint64_t now);
    tactical::PlayerId local_player_unlocked() const;
    std::optional<std::uint64_t> pending_input_epoch();
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
    std::uint64_t gns_log_generation_{};
    std::optional<std::uint64_t> last_repair_ms_;
    std::optional<core::Diagnostic> repair_notice_;
    std::deque<net::Received> retained_rejoin_frames_;
    bool smoke_progress_{};
    std::optional<std::uint64_t> last_progress_;
    std::optional<std::uint64_t> offered_delay_, waiting_since_;
    std::optional<std::uint64_t> net_stall_at_;
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
    bool capacity_staging_{};
    std::vector<tactical::PlayerId> owners_{1, 2};
    std::optional<core::Diagnostic> roster_error_;
    std::deque<net::Received> early_;
    std::size_t early_bytes_{};
    std::vector<net::MatchOptions> speed_requests_;
    std::vector<bool> pause_requests_;
    net::MatchSpeedWindow speed_window_;
    std::shared_ptr<LanTrace> trace_;
    std::shared_ptr<LanDiagnostics> diagnostics_{std::make_shared<LanDiagnostics>()};
    std::string last_logged_failure_, last_lease_error_;
    std::uint64_t last_renewal_failure_{};
};
} // namespace eawr::presentation::godot_backend::lan_detail
