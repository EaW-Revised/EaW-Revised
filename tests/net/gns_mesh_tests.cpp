#include "eawr/net/gns_transport.hpp"
#include "eawr/net/tactical_adapter.hpp"
#include "eawr/net/desync_monitor.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "../../src/net/platform_socket.hpp"
#include "../../src/net/gns_deferred_signals.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

// N-peer GNS mesh (slice N2 of docs/net-online-npeer.md). One broker process owns an InProcessGnsRoom and relays the
// room's opaque signals over loopback UDP; every member is its own process with its own GNS identity and a real ICE
// data path. Test-only IPC carries room operations and signals, never game frames.
namespace {
using namespace eawr::net;
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;

void expect(bool ok, const std::string& reason) { if (!ok) throw std::runtime_error(reason); }
void deferred_signal_limits() {
    std::deque<GnsRoomSignal> pending;
    GnsRoomSignal noisy; noisy.from_slot = 1; noisy.from_identity = "noisy"; noisy.bytes = {1};
    for (std::size_t index = 0; index < max_deferred_signals_per_peer; ++index) {
        noisy.bytes = {static_cast<std::uint8_t>(index)};
        expect(retain_deferred_signal(pending, noisy), "A sender can use its own deferred request allowance");
    }
    GnsRoomSignal honest; honest.from_slot = 2; honest.from_identity = "honest"; honest.bytes = {2};
    expect(retain_deferred_signal(pending, honest), "A full sender quota cannot reject another sender's request");
    expect(!retain_deferred_signal(pending, noisy), "Only the overflowing sender is refused its next deferred request");
    expect(pending.back().from_slot == 2, "A refused sender cannot evict an honest pending request");
}
template<class T> T require(eawr::core::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().code + ": " + value.error().message);
    return std::move(value).value();
}

struct Udp {
    socket::Open binding{eawr::net::socket::udp_open("127.0.0.1", 0, false)};
    Udp() { expect(binding.handle != eawr::net::socket::invalid && binding.port != 0, "IPC UDP open"); }
    ~Udp() { eawr::net::socket::close(binding.handle); }
    bool send(std::uint16_t port, std::span<const std::uint8_t> bytes) const {
        return eawr::net::socket::udp_send(binding.handle, eawr::net::socket::ipv4_loopback, port, bytes);
    }
    std::optional<Bytes> poll() const {
        std::array<std::uint8_t, 64000> bytes{};
        const auto read = eawr::net::socket::udp_receive(binding.handle, bytes);
        if (read.status == eawr::net::socket::Status::pending) return {};
        expect(read.status == eawr::net::socket::Status::ready && read.from == eawr::net::socket::ipv4_loopback, "IPC receive");
        return Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(read.count));
    }
};
void put_text(Bytes& bytes, const std::string& value) {
    expect(value.size() < 128, "IPC string capacity"); bytes.push_back(static_cast<std::uint8_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}
std::string take_text(std::span<const std::uint8_t>& bytes) {
    expect(!bytes.empty() && bytes.front() < 128, "IPC string prefix");
    const auto size = bytes.front(); bytes = bytes.subspan(1); expect(bytes.size() >= size, "IPC string length");
    std::string value(reinterpret_cast<const char*>(bytes.data()), size); bytes = bytes.subspan(size); return value;
}
void put_u16(Bytes& bytes, std::uint16_t value) { bytes.push_back(static_cast<std::uint8_t>(value)); bytes.push_back(static_cast<std::uint8_t>(value >> 8)); }
std::uint16_t take_u16(std::span<const std::uint8_t>& bytes) {
    expect(bytes.size() >= 2, "IPC u16"); const auto value = static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8)); bytes = bytes.subspan(2); return value;
}
void put_u64(Bytes& bytes, std::uint64_t value) { for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8))); }
std::uint64_t take_u64(std::span<const std::uint8_t>& bytes) {
    expect(bytes.size() >= 8, "IPC u64"); std::uint64_t value{};
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
    bytes = bytes.subspan(8); return value;
}

// IPC tags. child -> broker: attach, signal, room operation, barrier arrival, done. broker -> child: attach result,
// roster, forwarded signal, barrier release.
enum Tag : std::uint8_t { tag_attach = 0, tag_signal = 1, tag_operation = 2, tag_arrive = 3, tag_done = 4,
    tag_attached = 10, tag_roster = 11, tag_forwarded = 12, tag_released = 13, tag_repaired = 14 };
enum Operation : std::uint8_t { op_leave = 0, op_release = 1, op_kick = 2, op_lock = 3, op_repair = 4 };

void broker(std::size_t participants, std::size_t children, const std::string& drop_from, const std::string& drop_to) {
    Udp udp;
    InProcessGnsRoom room(static_cast<std::uint32_t>(participants));
    std::array<std::shared_ptr<GnsRoomEndpoint>, max_match_seats> endpoints;
    std::array<std::uint16_t, max_match_seats> ports{};
    std::array<std::string, max_match_seats> identities;
    std::map<unsigned, std::set<PeerIndex>> arrivals;
    std::set<unsigned> released_stages;
    std::size_t finished = 0, forwarded_blobs = 0, dropped_blobs = 0;
    std::uint64_t last_revision = ~0ULL;
    auto next_push = Clock::now();
    std::cout << udp.binding.port << '\n' << std::flush;
    const auto deadline = Clock::now() + 150s;
    const auto filtered = [&](PeerIndex a, PeerIndex b) {
        return !drop_from.empty() && ((identities[a] == drop_from && identities[b] == drop_to)
            || (identities[a] == drop_to && identities[b] == drop_from));
    };
    const auto push_roster = [&] {
        // The room is the source of truth; any attached endpoint can read it.
        for (PeerIndex slot = 0; slot < max_match_seats; ++slot) {
            if (!endpoints[slot]) continue;
            const auto view = endpoints[slot]->roster();
            Bytes wire{tag_roster}; put_u64(wire, view.revision); wire.push_back(static_cast<std::uint8_t>(view.capacity));
            wire.push_back(static_cast<std::uint8_t>(view.state));
            for (PeerIndex p = 0; p < view.capacity; ++p) {
                wire.push_back(static_cast<std::uint8_t>(view.slots[p].state)); put_text(wire, view.slots[p].identity);
            }
            for (PeerIndex p = 0; p < max_match_seats; ++p) if (ports[p]) udp.send(ports[p], wire);
            last_revision = view.revision;
            return;
        }
    };
    while (finished < children) {
        expect(Clock::now() < deadline, "Mesh broker deadline");
        while (auto packet = udp.poll()) {
            expect(packet->size() >= 2, "IPC envelope");
            const auto tag = (*packet)[0];
            const PeerIndex slot = (*packet)[1];
            expect(slot < max_match_seats, "IPC slot");
            std::span<const std::uint8_t> rest = std::span<const std::uint8_t>(*packet).subspan(2);
            if (tag == tag_attach) {
                const auto identity = take_text(rest); const auto port = take_u16(rest);
                auto result = room.attach(slot, identity);
                Bytes reply{tag_attached, static_cast<std::uint8_t>(result ? 1 : 0)};
                if (result) { endpoints[slot] = std::move(result).value(); ports[slot] = port; identities[slot] = identity; }
                udp.send(port, reply);
                push_roster();
            } else if (tag == tag_signal) {
                expect(rest.size() >= 2, "IPC signal");
                const PeerIndex to = rest[0]; rest = rest.subspan(1);
                if (!endpoints[slot] || to >= max_match_seats) continue;
                if (filtered(slot, to)) { ++dropped_blobs; continue; }
                static_cast<void>(endpoints[slot]->send(to, rest));
            } else if (tag == tag_operation) {
                expect(rest.size() == 2 && endpoints[slot], "IPC operation");
                const auto op = rest[0]; const PeerIndex arg = rest[1];
                if (op == op_release) static_cast<void>(endpoints[slot]->release(arg));
                else if (op == op_kick) static_cast<void>(endpoints[slot]->kick(arg));
                else if (op == op_lock) static_cast<void>(endpoints[slot]->lock());
                else if (op == op_leave) static_cast<void>(endpoints[slot]->leave());
                else if (op == op_repair) {
                    const auto result = endpoints[slot]->repair_attachment(arg);
                    udp.send(ports[slot], Bytes{tag_repaired, static_cast<std::uint8_t>(arg), static_cast<std::uint8_t>(result && result.value())});
                }
                push_roster();
            } else if (tag == tag_arrive) {
                expect(rest.size() == 2, "IPC barrier");
                const unsigned stage = rest[0], need = rest[1];
                arrivals[stage].insert(slot);
                if (arrivals[stage].size() >= need && !released_stages.contains(stage)) {
                    released_stages.insert(stage);
                    for (const auto arrived : arrivals[stage]) udp.send(ports[arrived], Bytes{tag_released, static_cast<std::uint8_t>(stage)});
                }
            } else if (tag == tag_done) ++finished;
            else expect(false, "Unknown mesh IPC tag");
        }
        for (PeerIndex slot = 0; slot < max_match_seats; ++slot) {
            if (!endpoints[slot]) continue;
            for (unsigned i = 0; i < 64; ++i) {
                auto next = endpoints[slot]->poll();
                if (!next || !next.value()) break;
                const auto& item = *next.value();
                Bytes wire{tag_forwarded, static_cast<std::uint8_t>(item.from_slot)}; put_text(wire, item.from_identity);
                wire.insert(wire.end(), item.bytes.begin(), item.bytes.end());
                udp.send(ports[slot], wire); ++forwarded_blobs;
            }
        }
        if (Clock::now() >= next_push) { push_roster(); next_push = Clock::now() + 40ms; }
        std::this_thread::sleep_for(1ms);
    }
    static_cast<void>(last_revision);
    std::cout << "Mesh broker forwarded " << forwarded_blobs << " signals, dropped " << dropped_blobs << '\n';
}

// A member's GnsRoomEndpoint over the broker. send runs on GNS threads; everything is under one mutex and no GNS call
// is ever made while holding it.
class ProxyRoom final : public GnsRoomEndpoint {
public:
    ProxyRoom(PeerIndex slot, std::string identity, std::uint16_t broker_port)
        : slot_(slot), identity_(std::move(identity)), broker_(broker_port) {
        const auto deadline = Clock::now() + 20s;
        while (true) {
            Bytes wire{tag_attach, static_cast<std::uint8_t>(slot_)}; put_text(wire, identity_); put_u16(wire, udp_.binding.port);
            expect(udp_.send(broker_, wire), "IPC attach");
            const auto retry = Clock::now() + 200ms;
            while (Clock::now() < retry) {
                std::lock_guard guard(mutex_);
                drain();
                if (attach_result_) {
                    if (*attach_result_) return;
                    attach_result_.reset(); break;
                }
                std::this_thread::sleep_for(1ms);
            }
            expect(Clock::now() < deadline, "Room attach was refused for " + identity_);
        }
    }
    PeerIndex local_slot() const override { return slot_; }
    GnsRoomRoster roster() const override { std::lock_guard guard(mutex_); drain(); return roster_; }
    eawr::core::Result<void> send(PeerIndex to, std::span<const std::uint8_t> blob) override {
        std::lock_guard guard(mutex_);
        Bytes wire{tag_signal, static_cast<std::uint8_t>(slot_), static_cast<std::uint8_t>(to)};
        wire.insert(wire.end(), blob.begin(), blob.end());
        if (wire.size() > 60000 || !udp_.send(broker_, wire)) return failure("EAWR-NET-1761", "IPC signal send failed");
        last_signals_[to].assign(blob.begin(), blob.end());
        return eawr::core::Result<void>::success();
    }
    // Repeat a real opaque signal for the existing native connection. This
    // proves adapter dispatch and link stability; the SDK can discard the
    // already-consumed message IDs. It does not prove fresh ICE route updates
    // or WAN loss recovery. The loopback injection sends once, without retries.
    void repeat_signal(PeerIndex to) {
        Bytes signal;
        { std::lock_guard guard(mutex_); signal = last_signals_.at(to); }
        require(send(to, signal));
    }
    eawr::core::Result<std::optional<GnsRoomSignal>> poll() override {
        std::lock_guard guard(mutex_);
        drain();
        if (inbox_.empty()) return eawr::core::Result<std::optional<GnsRoomSignal>>::success({});
        auto value = std::move(inbox_.front()); inbox_.pop_front();
        return eawr::core::Result<std::optional<GnsRoomSignal>>::success(std::move(value));
    }
    std::optional<GnsTurnCredentials> turn_credentials() const override { return {}; }
    GnsRoomLease lease() const override { return {Clock::now() + 24h}; }
    eawr::core::Result<void> status() const override { return eawr::core::Result<void>::success(); }
    eawr::core::Result<void> leave() override { return operate(op_leave, 0); }
    eawr::core::Result<void> release(PeerIndex slot) override { return operate(op_release, slot); }
    eawr::core::Result<void> kick(PeerIndex slot) override { return operate(op_kick, slot); }
    eawr::core::Result<void> lock() override { return operate(op_lock, 0); }
    bool refuse_repair{};
    std::chrono::milliseconds refresh_delay{};
    bool repair_requested() const { std::lock_guard guard(mutex_); return requested_epoch_ != 0; }
    eawr::core::Result<bool> repair_attachment(std::uint64_t epoch) override {
        std::lock_guard guard(mutex_); drain();
        if (refuse_repair) {
            eawr::core::Diagnostic diagnostic; diagnostic.code = "EAWR-NET-1791";
            diagnostic.message = "Alias Reattach refused by deployed server";
            return eawr::core::Result<bool>::failure(std::move(diagnostic));
        }
        if (repaired_epoch_ == epoch && Clock::now() >= refresh_ready_) return eawr::core::Result<bool>::success(true);
        if (requested_epoch_ != epoch) {
            requested_epoch_ = epoch; refresh_ready_ = Clock::now() + refresh_delay;
            udp_.send(broker_, Bytes{tag_operation, static_cast<std::uint8_t>(slot_), op_repair, static_cast<std::uint8_t>(epoch)});
        }
        return eawr::core::Result<bool>::success(false);
    }
    // Barrier: report arrival and keep the caller's pump running until the broker releases every participant.
    void arrive_at(unsigned stage, unsigned need) {
        std::lock_guard guard(mutex_);
        expect(udp_.send(broker_, Bytes{tag_arrive, static_cast<std::uint8_t>(slot_), static_cast<std::uint8_t>(stage), static_cast<std::uint8_t>(need)}), "IPC barrier");
    }
    bool barrier_released(unsigned stage) const { std::lock_guard guard(mutex_); drain(); return released_.contains(stage); }
    void finish() const {
        std::lock_guard guard(mutex_);
        udp_.send(broker_, Bytes{tag_done, static_cast<std::uint8_t>(slot_)});
    }
private:
    static eawr::core::Result<void> failure(const char* code, const char* message) {
        eawr::core::Diagnostic diagnostic; diagnostic.code = code; diagnostic.message = message;
        return eawr::core::Result<void>::failure(std::move(diagnostic));
    }
    eawr::core::Result<void> operate(Operation op, PeerIndex arg) {
        std::lock_guard guard(mutex_);
        if (!udp_.send(broker_, Bytes{tag_operation, static_cast<std::uint8_t>(slot_), op, static_cast<std::uint8_t>(arg)}))
            return failure("EAWR-NET-1761", "IPC operation send failed");
        return eawr::core::Result<void>::success();
    }
    void drain() const {
        while (auto packet = udp_.poll()) {
            std::span<const std::uint8_t> rest(*packet);
            expect(!rest.empty(), "IPC empty");
            const auto tag = rest.front(); rest = rest.subspan(1);
            if (tag == tag_repaired) { if (rest.size() == 2 && rest[1]) repaired_epoch_ = rest[0]; }
            else if (tag == tag_attached) { attach_result_ = rest.front() != 0; }
            else if (tag == tag_released) released_.insert(rest.front());
            else if (tag == tag_roster) {
                GnsRoomRoster value; value.revision = take_u64(rest);
                value.capacity = rest[0]; value.state = static_cast<GnsRoomState>(rest[1]); rest = rest.subspan(2);
                for (PeerIndex p = 0; p < value.capacity; ++p) {
                    value.slots[p].state = static_cast<GnsRoomSlotState>(rest[0]); rest = rest.subspan(1);
                    value.slots[p].identity = take_text(rest);
                }
                if (value.revision >= roster_.revision) roster_ = std::move(value);
            } else if (tag == tag_forwarded) {
                GnsRoomSignal item; item.from_slot = rest[0]; rest = rest.subspan(1);
                item.from_identity = take_text(rest); item.bytes.assign(rest.begin(), rest.end());
                if (inbox_.size() < 256) inbox_.push_back(std::move(item));
            } else expect(false, "Unknown mesh IPC reply");
        }
    }
    PeerIndex slot_;
    std::string identity_;
    std::uint16_t broker_;
    mutable std::mutex mutex_;
    mutable Udp udp_;
    mutable GnsRoomRoster roster_;
    mutable std::deque<GnsRoomSignal> inbox_;
    std::map<PeerIndex, Bytes> last_signals_;
    mutable std::optional<bool> attach_result_;
    mutable std::set<unsigned> released_;
    mutable std::uint64_t requested_epoch_{}, repaired_epoch_{};
    mutable Clock::time_point refresh_ready_{};
};

// Deterministic content so a frame proves its source, destination, position and bytes.
Bytes pattern(PeerIndex from, PeerIndex to, unsigned index, std::size_t size) {
    Bytes bytes(size);
    for (std::size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<std::uint8_t>(from * 31U + to * 17U + index * 7U + i * 13U + (i >> 8));
    return bytes;
}
constexpr std::size_t big_frame = 1024 * 1024;
std::size_t small_size(unsigned index) { return index == 0 ? 0 : index * 23U; }

struct Options {
    PeerIndex slot{};
    unsigned seats{};
    std::string identity, build{"mesh-test-build"};
    std::uint16_t broker{};
    std::string scenario{"mesh"};
    unsigned participants{};
    // Assertions on what this member must observe.
    int want_left_code{-1};
    int want_refused_seat{-1}, want_refused_other{-1};
    int want_vacated{-1};
    bool pair_timeout_short{};
};

class Member {
public:
    explicit Member(Options options) : options_(std::move(options)) {
        room_ = std::make_shared<ProxyRoom>(options_.slot, options_.identity, options_.broker);
        GnsOptions config;
        config.build_identity = options_.build; config.peers = options_.seats;
        config.keepalive = 100ms; config.timeout = 8s;
        config.pair_timeout = options_.pair_timeout_short ? 5s : options_.scenario == "repair_lower" ? 20s : 25s;
        transport_ = require(options_.slot == 0 ? GnsTransport::host_room(room_, config) : GnsTransport::join_room(room_, config));
        expect(transport_->mesh() && transport_->self() == options_.slot && transport_->peer_count() == options_.seats, "Mesh shape");
    }
    // Pump the mesh once; returns whether anything useful happened. Frames are filed by sender.
    void pump() {
        while (auto frame = transport_->poll()) received_[frame->from].push_back(std::move(frame->frame));
        while (auto event = transport_->take_event()) {
            events_.push_back(*event);
            std::cout << "EVENT slot=" << options_.slot << " kind=" << static_cast<unsigned>(event->kind) << " seat=" << event->seat
                << " other=" << event->other << " code=" << event->code << std::endl;
        }
        flush_sends();
        std::this_thread::sleep_for(1ms);
    }
    bool seen(GnsMeshEventKind kind, int seat = -1, int other = -1, int code = -1) const {
        return std::any_of(events_.begin(), events_.end(), [&](const GnsMeshEvent& e) {
            return e.kind == kind && (seat < 0 || static_cast<int>(e.seat) == seat)
                && (other < 0 || static_cast<int>(e.other) == other) && (code < 0 || static_cast<int>(e.code) == code);
        });
    }
    template<class F> void until(F predicate, std::chrono::seconds timeout, const char* what) {
        const auto deadline = Clock::now() + timeout;
        while (!predicate()) {
            expect(Clock::now() < deadline, std::string("Mesh deadline: ") + what + " (slot " + std::to_string(options_.slot) + ")");
            pump();
        }
    }
    void barrier(unsigned stage, unsigned need) {
        room_->arrive_at(stage, need);
        until([&] { return room_->barrier_released(stage); }, 60s, "barrier");
    }
    void queue_to(PeerIndex to, Bytes frame) { pending_[to].push_back(std::move(frame)); }
    void flush_sends() {
        for (auto& [to, frames] : pending_) {
            while (!frames.empty()) {
                const auto sent = transport_->send(to, frames.front());
                if (!sent) {
                    // Backpressure is retryable; anything else is a failed pair.
                    expect(sent.error().code == "EAWR-NET-1505", "Mesh send failed: " + sent.error().code + " " + sent.error().message);
                    break;
                }
                frames.pop_front();
            }
        }
    }
    bool sends_flushed() const {
        return std::all_of(pending_.begin(), pending_.end(), [](const auto& entry) { return entry.second.empty(); });
    }

    // Wait for the whole mesh. The host Locks the room once the formation reports it complete.
    void form(std::chrono::seconds timeout) {
        until([&] {
            if (options_.slot == 0 && transport_->mesh_complete() && !locked_) {
                require(transport_->lock_room()); locked_ = true;
            }
            if (options_.slot == 0) return locked_;
            return transport_->mesh_complete();
        }, timeout, "complete mesh");
        for (PeerIndex p = 0; p < options_.seats; ++p) if (p != options_.slot)
            expect(transport_->link(p) == LinkState::open, "Every pair is open once the mesh is complete");
    }
    // Every seat in `peers` sends small frames to every other one, plus a 1 MiB frame around the ring.
    void exchange(const std::vector<PeerIndex>& peers, unsigned round) {
        for (const auto to : peers) {
            if (to == options_.slot) continue;
            for (unsigned i = 0; i < 10; ++i) queue_to(to, pattern(options_.slot, to, round * 100 + i, small_size(i)));
            if (round == 0 && ring_next(peers) == to) queue_to(to, pattern(options_.slot, to, 99, big_frame));
            queue_to(to, Bytes{0xEE, static_cast<std::uint8_t>(round)});
        }
        until([&] {
            if (!sends_flushed()) return false;
            for (const auto from : peers) {
                if (from == options_.slot) continue;
                const auto& got = received_[from];
                if (got.empty() || got.back() != Bytes{0xEE, static_cast<std::uint8_t>(round)}) return false;
            }
            return true;
        }, 90s, "frame exchange");
        for (const auto from : peers) {
            if (from == options_.slot) continue;
            auto& got = received_[from];
            const bool big = round == 0 && ring_next(peers, from) == options_.slot;
            expect(got.size() == 10U + (big ? 1U : 0U) + 1U, "Frame count from seat " + std::to_string(from));
            std::size_t at = 0;
            for (unsigned i = 0; i < 10; ++i, ++at)
                expect(got[at] == pattern(from, options_.slot, round * 100 + i, small_size(i)), "Ordered whole frame " + std::to_string(i) + " from seat " + std::to_string(from));
            if (big) expect(got[at++] == pattern(from, options_.slot, 99, big_frame), "1 MiB ordered tail from seat " + std::to_string(from));
            expect(got[at] == Bytes{0xEE, static_cast<std::uint8_t>(round)}, "Round marker");
            got.clear();
        }
    }
    PeerIndex ring_next(const std::vector<PeerIndex>& peers, PeerIndex of = ~0U) const {
        if (of == ~0U) of = options_.slot;
        const auto it = std::find(peers.begin(), peers.end(), of);
        return it == peers.end() || std::next(it) == peers.end() ? peers.front() : *std::next(it);
    }
    void report() const {
        for (PeerIndex p = 0; p < options_.seats; ++p) {
            if (p == options_.slot) continue;
            const auto counts = transport_->signal_counts(p);
            std::cout << "SIGNALS slot=" << options_.slot << " remote=" << p << " sent_blobs=" << counts.sent_blobs
                << " sent_bytes=" << counts.sent_bytes << " received_blobs=" << counts.received_blobs
                << " received_bytes=" << counts.received_bytes << '\n';
        }
    }
    void leave_gracefully() {
        transport_->close();
        until([&] { return transport_->closed(); }, 20s, "graceful close");
    }
    GnsTransport& transport() { return *transport_; }
    ProxyRoom& room() { return *room_; }
    const Options& options() const { return options_; }
    const std::vector<GnsMeshEvent>& events() const { return events_; }
private:
    Options options_;
    std::shared_ptr<ProxyRoom> room_;
    std::unique_ptr<GnsTransport> transport_;
    std::map<PeerIndex, std::vector<Bytes>> received_;
    std::map<PeerIndex, std::deque<Bytes>> pending_;
    std::vector<GnsMeshEvent> events_;
    bool locked_{};
};

// Real simulation outside the GNS I/O code. Four child processes use different
// worker counts; the launcher compares final hashes as well as each result.
void repair_battle(Member& member, bool refuse_refresh, bool wrong_key = false, bool returning = false) {
    const auto& config = member.options();
    const bool maintenance = config.scenario == "maintenance";
    namespace tactical = eawr::sim::tactical;
    const std::size_t workers = std::size_t{1} << (config.slot % 4);
    eawr::platform::ThreadWorkerAdapter executor(workers);
    tactical::TacticalSetup initial;
    initial.seed = 1790; initial.departure_policy.emplace();
    std::vector<tactical::SeatBinding> roster;
    for (unsigned p = 0; p < config.seats; ++p) {
        const auto id = p + 1;
        initial.players.push_back({id, id, id, tactical::player_flag_commandable});
        initial.departure_policy->humans.push_back(id);
        roster.push_back({eawr::sim::SeatId{id}, id, true, false});
        tactical::UnitState unit; unit.entity_id = id; unit.type_id = 1; unit.owner = id;
        unit.rotation = eawr::sim::math::identity_quat(); initial.units.push_back(unit);
    }
    TcpRejoinAuth auth; auth.epoch = 1; auth.match_identity.fill(17);
    for (PeerIndex p = 0; p < config.seats; ++p) {
        RejoinNonce key{};
        if (p != config.slot) key.fill(static_cast<std::uint8_t>(1 + std::min(p, config.slot) * 9 + std::max(p, config.slot)));
        auth.seat_tokens.push_back(key);
    }
    if (wrong_key && config.slot == 1) auth.seat_tokens[0][0] ^= 1;
    const bool late_arm = config.scenario == "repair_late_arm";
    const bool lower_first = config.scenario == "repair_lower";
    // #4021: the pair (4,5) goes silent, is repaired, and 36 s later the pair (1,5) goes silent. Every seat arms its
    // repair at each incident's own epoch, as the viewer does. The refused variant makes seat 5 unable to take a fresh
    // attachment the second time: only that seat may leave, with the typed departure, and the match goes on.
    // repair_twice_late: seat 1, the acceptor of the second pair, arms the second incident 3 s after seat 5 has dialled
    // with the new epoch's material, so its first request meets the previous epoch's (the order is not up to the arms).
    // repair_gave_up: seat 1 gives up on the pair (1,2) before its dial arrives; the late request must not revive it.
    const bool twice_refused = config.scenario == "repair_twice_refused";
    const bool twice_late = config.scenario == "repair_twice_late";
    const bool gave_up = config.scenario == "repair_gave_up";
    const bool twice = config.scenario == "repair_twice" || twice_refused || twice_late;
    const bool incident_arming = late_arm || lower_first || twice || gave_up;
    const PeerIndex first = twice ? 4U : (late_arm || gave_up) ? 1U : 0U, second = twice ? 5U : (late_arm || gave_up) ? 2U : 1U;
    const std::uint64_t final_ticks = twice ? 1500 : 90;
    constexpr std::uint64_t second_cut_tick = 20 + 36 * 30;
    bool armed = false, cut_again = false;
    std::uint64_t armed_epoch = 0;
    if (!incident_arming && !maintenance) { require(member.transport().repair_closed_links(auth)); armed = true; }
    if (lower_first && config.slot == 1) member.room().refresh_delay = 8s;
    member.room().refuse_repair = refuse_refresh;
    SessionOptions options; options.final_tick_count = final_ticks; options.completeness_timeout_ms = 60'000;
    MeshIncidentOptions mesh; mesh.repair_grace = 30s;
    mesh.relay_failed = [&](PeerIndex p) { return member.transport().pair(p).relay_failed; };
    options.mesh = mesh;
    DesyncOptions monitor_options; monitor_options.exchange_every = 10;
    DesyncMonitor monitor(member.transport(), monitor_options);
    auto session = require(TacticalLockstepSession::create(initial, {}, member.transport(), executor, roster, options, &monitor));
    const auto deadline = Clock::now() + (twice ? 240s : 100s);
    // The repair trace and the incident transitions go to the output, so a failure shows which incident went wrong.
    struct SinkGuard { ~SinkGuard() { GnsTransport::set_log_sink({}); } } sink_guard;
    const auto trace_slot = config.slot;
    GnsTransport::set_log_sink([trace_slot](std::string_view line) { std::cout << "TRACE slot=" << trace_slot << ' ' << line << std::endl; });
    bool trace_running = false;
    const auto trace_started = Clock::now();
    bool cut = false, guest_command_accepted = false; std::string failure;
    std::optional<Clock::time_point> incident_seen, second_incident_seen;
    bool gave_up_done = false;
    const auto arm_delay = late_arm && config.slot == first ? 3s : lower_first && config.slot == second ? 12s : 0s;
    while (session.simulation().completed_tick() < final_ticks || !monitor.agreed_end()) {
        expect(Clock::now() < deadline, "Repair battle deadline");
        const auto now = Clock::now();
        const auto tick = session.simulation().completed_tick();
        if (twice && session.mesh_running() != trace_running) {
            trace_running = session.mesh_running();
            std::cout << "MESH slot=" << config.slot << " t=" << std::chrono::duration_cast<std::chrono::milliseconds>(now - trace_started).count()
                << "ms tick=" << tick << " running=" << trace_running << " incident=" << session.mesh_incident_id()
                << " departures=" << session.departure_outcomes().size() << std::endl;
        }
        if (!cut && tick >= 20) {
            cut = true;
            if (maintenance) {
                for (PeerIndex peer = 0; peer < config.seats; ++peer)
                    if (peer != config.slot) member.room().repeat_signal(peer);
                if (config.slot == 1) {
                    const auto key = require(session.submit({{2}, tactical::StopPayload{}}));
                    expect(key.tick == 23, "The legal guest command keeps its agreed future input tick");
                }
            } else {
                // Exercise both closure directions; the other endpoint may still read open.
                const PeerIndex cutting = lower_first ? first : second;
                if (config.slot == cutting) {
                    if (returning) require(member.transport().restart_links(auth));
                    else require(member.transport().interrupt_peer(lower_first ? second : first));
                }
            }
        }
        if (twice && cut && !cut_again && tick >= second_cut_tick) {
            cut_again = true;
            if (config.slot == 1) require(member.transport().interrupt_peer(5));
            if (twice_refused && config.slot == 5) member.room().refuse_repair = true;
        }
        if (twice && session.mesh_running() && armed_epoch != session.mesh_incident_id()) {
            const bool delayed = twice_late && config.slot == 1 && armed_epoch != 0;
            if (delayed && !second_incident_seen) second_incident_seen = now;
            if (!delayed || now - *second_incident_seen >= 3s) {
                armed_epoch = session.mesh_incident_id(); auth.epoch = armed_epoch;
                require(member.transport().repair_closed_links(auth));
            }
        }
        if (maintenance && !armed && tick >= 40) {
            require(member.transport().repair_closed_links(auth)); armed = true;
            for (PeerIndex peer = 0; peer < config.seats; ++peer)
                if (peer != config.slot) member.room().repeat_signal(peer);
        }
        const auto stamp = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        const auto pumped = session.pump(stamp);
        if (!pumped) {
            failure = pumped.error().code;
            std::cerr << "PUMP slot=" << config.slot << " code=" << failure << " detail=" << pumped.error().message << std::endl;
            break;
        }
        if (maintenance) {
            const auto snapshot = session.simulation().snapshot();
            guest_command_accepted = guest_command_accepted || std::any_of(snapshot->events().begin(), snapshot->events().end(),
                [](const auto& event) { return event.kind == tactical::EventKind::order_accepted && event.player == 2 && event.tick == 23; });
        }
        if (incident_arming && session.mesh_running() && !armed) {
            if (!incident_seen) incident_seen = Clock::now();
            if (Clock::now() - *incident_seen >= arm_delay) {
                auth.epoch = session.mesh_incident_id();
                require(member.transport().repair_closed_links(auth)); armed = true;
            }
        }
        if (gave_up && config.slot == 1 && armed) {
            // After the arm (which starts the epoch with no pair given up): the session gives up on the pair, then the
            // dialer's request arrives.
            if (!gave_up_done) {
                gave_up_done = true;
                member.transport().fail_repair(2, 1794);
            }
            expect(member.transport().pair(2).phase != GnsPairPhase::open, "A pair given up on is not revived by a late request");
        }
        std::this_thread::sleep_for(1ms);
    }
    if (gave_up) {
        // Seat 1 gave up on the pair: either end of it may be the one the agreed departure removes, nobody else may fail.
        if (config.slot == 1 || config.slot == 2) {
            if (!failure.empty()) expect(failure == mesh_errors::left, "A seat of the abandoned pair leaves with the typed departure");
        } else {
            expect(failure.empty(), "Repair survivor did not fail: " + failure);
            expect(!session.departure_outcomes().empty(), "The abandoned pair ends with an agreed departure");
        }
        if (failure.empty()) {
            std::cout << "BATTLE refresh_refused=0 slot=" << config.slot << " workers=" << workers
                << " hash=" << session.simulation().state_sha256() << std::endl;
        }
        member.barrier(9, config.participants);
        return;
    }
    const bool failure_expected = refuse_refresh || wrong_key || twice_refused;
    if (failure_expected && config.slot == (twice_refused ? 5U : 1U)) {
        expect(failure == mesh_errors::left, "Unrepairable alias gets the agreed typed departure");
    } else {
        expect(failure.empty(), "Repair survivor did not fail: " + failure);
        if (failure_expected) expect(session.departure_outcomes().size() == 1, "Failed fresh pairing selects exactly one agreed departure");
        else {
            expect(session.departure_outcomes().empty(), "Successful repair removes nobody");
            for (PeerIndex peer = 0; peer < config.seats; ++peer) {
                if (peer == config.slot) continue;
                const bool repaired = !maintenance && (returning ? (peer == 1 || config.slot == 1)
                    : ((config.slot == first && peer == second) || (config.slot == second && peer == first)
                        || (twice && ((config.slot == 1 && peer == 5) || (config.slot == 5 && peer == 1)))));
                expect(member.transport().edge_identity(peer) == (repaired ? 2U : 1U), "Only the proved replacement increments its pair generation");
            }
            for (const auto& event : member.events()) expect(event.code != 1793, "Asymmetric repair never exhausts its grace");
            if (maintenance) {
                expect(guest_command_accepted, "Both peers execute the guest's legal command at its agreed tick");
                expect(!member.room().repair_requested(), "Live signalling never refreshes a repair attachment");
            }
        }
        std::cout << "BATTLE refresh_refused=" << refuse_refresh << " slot=" << config.slot << " workers=" << workers
            << " hash=" << session.simulation().state_sha256() << std::endl;
    }
    member.barrier(9, config.participants);
}

int run_member(Options options) {
    Member member(options);
    const auto everyone = [&] {
        std::vector<PeerIndex> all;
        for (PeerIndex p = 0; p < options.seats; ++p) all.push_back(p);
        return all;
    }();
    if (options.scenario == "refused") {
        // The first occupant of a slot the host refuses: join ends terminally, with the reason.
        // Signal counts are per pair and never go down, also across the host's regrant that replaces the pair's link.
        std::array<GnsSignalCounts, max_match_seats> high{};
        member.until([&] {
            for (PeerIndex p = 0; p < options.seats; ++p) {
                if (p == options.slot) continue;
                const auto now = member.transport().signal_counts(p);
                expect(now.sent_blobs >= high[p].sent_blobs && now.sent_bytes >= high[p].sent_bytes
                    && now.received_blobs >= high[p].received_blobs && now.received_bytes >= high[p].received_bytes,
                    "Signal counts never decrease across a regrant");
                high[p] = now;
            }
            return member.seen(GnsMeshEventKind::left_room);
        }, 60s, "refusal");
        expect(member.seen(GnsMeshEventKind::left_room, -1, options.want_refused_other, options.want_left_code), "Refusal carries its typed reason");
        expect(member.transport().diagnostic() && member.transport().diagnostic()->code == "EAWR-NET-" + std::to_string(options.want_left_code),
            "Refused join keeps its diagnostic");
        for (PeerIndex p = 0; p < options.seats; ++p) expect(p == options.slot || member.transport().link(p) != LinkState::open, "A refused member has no open pair");
        expect(!member.transport().mesh_complete(), "A refused member is not part of a complete mesh");
        expect(!member.transport().send(0, {}), "A refused member cannot send application frames");
        member.report();
        member.room().finish();
        std::cout << "slot " << options.slot << " refused as expected\n";
        return 0;
    }
    member.form(90s);
    if (options.want_refused_seat >= 0)
        expect(member.seen(GnsMeshEventKind::seat_refused, options.want_refused_seat, options.want_refused_other, 1762), "Host reports the refused newcomer");
    if (options.want_vacated >= 0)
        expect(member.seen(GnsMeshEventKind::seat_vacated, options.want_vacated), "Members hear about the vacated seat");
    expect(member.transport().connected_seats() == (1U << options.seats) - 1U, "Every seat is connected");
    if (options.scenario == "maintenance") {
        require(member.transport().commit_room());
        member.barrier(20, options.participants);
        std::vector<std::uint64_t> received(options.seats);
        for (const auto peer : everyone) if (peer != options.slot)
            received[peer] = member.transport().signal_counts(peer).received_blobs;
        member.barrier(21, options.participants);
        for (const auto peer : everyone) if (peer != options.slot) member.room().repeat_signal(peer);
        member.until([&] {
            for (const auto peer : everyone) if (peer != options.slot) {
                if (member.transport().signal_counts(peer).received_blobs <= received[peer]) return false;
                expect(member.transport().queue_progress(peer).deferred_signals == 0,
                    "A live connection's signal never waits for a repair epoch");
                expect(member.transport().edge_identity(peer) == 1 && member.transport().link(peer) == LinkState::open,
                    "Live signalling preserves the authenticated native connection");
            }
            return true;
        }, 5s, "post-commit live signalling");
        member.barrier(22, options.participants);
        member.exchange(everyone, 0);
        member.barrier(23, options.participants);
        repair_battle(member, false);
        member.report(); member.room().finish();
        std::cout << "slot " << options.slot << " post-commit live signalling passed\n";
        return 0;
    }
    // Pair link stats and diagnostics are observable per pair.
    for (PeerIndex p = 0; p < options.seats; ++p) if (p != options.slot)
        expect(member.transport().pair(p).phase == GnsPairPhase::open, "Pair diagnostic reports open");
    member.exchange(everyone, 0);
    member.report();
    member.barrier(1, options.participants);
    if (options.scenario == "repair" || options.scenario == "repair_refused" || options.scenario == "repair_bad_key" || options.scenario == "repair_return" || options.scenario == "repair_late_arm" || options.scenario == "repair_lower"
        || options.scenario == "repair_twice" || options.scenario == "repair_twice_refused" || options.scenario == "repair_twice_late"
        || options.scenario == "repair_gave_up") {
        repair_battle(member, options.scenario == "repair_refused", options.scenario == "repair_bad_key", options.scenario == "repair_return");
        member.room().finish(); return 0;
    }
    if (options.slot == 0) {
        // The host leaves after Lock; the pairs among the survivors keep delivering.
        member.leave_gracefully();
        member.room().finish();
        std::cout << "slot 0 host left after Lock\n";
        return 0;
    }
    member.until([&] { return member.transport().link(0) == LinkState::closed; }, 30s, "host pair closes");
    for (PeerIndex p = 1; p < options.seats; ++p) if (p != options.slot)
        expect(member.transport().link(p) == LinkState::open, "Survivor pairs stay open after the host leaves");
    std::vector<PeerIndex> survivors(everyone.begin() + 1, everyone.end());
    member.exchange(survivors, 1);
    member.barrier(2, options.participants - 1);
    member.room().finish();
    std::cout << "slot " << options.slot << " ok\n";
    return 0;
}

Options parse(int argc, char** argv) {
    // member <slot> <seats> <identity> <broker port> <scenario> <participants> [key=value ...]
    expect(argc >= 8, "member arguments");
    Options options;
    options.slot = static_cast<PeerIndex>(std::stoul(argv[2])); options.seats = static_cast<unsigned>(std::stoul(argv[3]));
    options.identity = argv[4]; options.broker = static_cast<std::uint16_t>(std::stoul(argv[5]));
    options.scenario = argv[6]; options.participants = static_cast<unsigned>(std::stoul(argv[7]));
    for (int i = 8; i < argc; ++i) {
        const std::string item = argv[i];
        const auto eq = item.find('=');
        expect(eq != std::string::npos, "member option " + item);
        const auto key = item.substr(0, eq), value = item.substr(eq + 1);
        if (key == "build") options.build = value;
        else if (key == "left") options.want_left_code = std::stoi(value);
        else if (key == "refused_seat") options.want_refused_seat = std::stoi(value);
        else if (key == "refused_other") options.want_refused_other = std::stoi(value);
        else if (key == "vacated") options.want_vacated = std::stoi(value);
        else if (key == "short") options.pair_timeout_short = value == "1";
        else expect(false, "member option " + key);
    }
    return options;
}
}

int main(int argc, char** argv) {
    try {
        deferred_signal_limits();
        if (!GnsTransport::available()) {
            std::cout << "GNS disabled: mesh contracts skipped\n"; return 77;
        }
        expect(argc >= 2, "mode");
        const std::string mode = argv[1];
        if (mode == "broker") {
            // broker <participants> <children> [drop identity A] [drop identity B]
            expect(argc >= 4, "broker arguments");
            broker(std::stoul(argv[2]), std::stoul(argv[3]), argc > 5 ? argv[4] : "", argc > 5 ? argv[5] : "");
            return 0;
        }
        if (mode == "member") return run_member(parse(argc, argv));
        expect(false, "Unknown mesh test mode");
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
    return 1;
}
