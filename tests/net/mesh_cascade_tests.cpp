#include "pair_network.hpp"
#include "eawr/net/mesh_incident.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

// Relay cascades at nine seats (#4021, N9-H): a pair goes silent, the 15 s watchdog closes it, the redial takes a few seconds,
// and while that repair runs more pairs of the same seat go silent. Every seat runs its own MeshIncident over a PairNetwork
// and a model of the transport's repair bookkeeping (who may call a closed pair "relay failed" and when), so a seat only
// ever learns about pairs of its own and through the reports its open pairs deliver.
namespace {
namespace net = eawr::net;
using eawr::testing::PairNetwork;
using eawr::testing::PairTransport;
using Time = std::chrono::milliseconds;
using Kind = net::MeshIncidentResult::Kind;

int failures{};
bool explore{};
void expect(bool value, const std::string& message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

// What the transport answers MeshIncidentOptions::relay_failed with. The higher seat of a pair dials, so its pair is only
// failed once the current repair stretch's deadline (30 s after its first loss) has passed; the lower seat accepts and
// reports a closed pair failed at once (GnsTransport::fail_pair).
struct SeatModel {
    std::uint64_t armed_incident{};
    std::optional<std::uint64_t> deadline;
};

struct Event {
    std::uint64_t at{};
    enum class Do { silent, cut, heal } what{};
    net::PeerIndex a{}, b{};
};

struct Cascade {
    explicit Cascade(unsigned seats, std::chrono::milliseconds grace = std::chrono::seconds(30))
        : count(seats), network(seats), models(seats), results(seats), stopped(seats), left_at(seats), errors(seats) {
        const auto all = static_cast<net::MeshSeats>((1U << seats) - 1U);
        for (net::PeerIndex seat = 0; seat < seats; ++seat) {
            transports.push_back(std::make_unique<PairTransport>(network, seat));
            net::MeshIncidentOptions options;
            options.repair_grace = grace;
            options.relay_failed = [this, seat](net::PeerIndex remote) { return pair_failed(seat, remote); };
            incidents.push_back(std::make_unique<net::MeshIncident>(*transports.back(), all, options));
        }
    }
    bool pair_failed(net::PeerIndex seat, net::PeerIndex remote) const {
        if (!network.is_cut(seat, remote)) return false;
        if (seat < remote) return true;
        return models[seat].deadline && now >= *models[seat].deadline;
    }
    void schedule(std::uint64_t at, Event::Do what, net::PeerIndex a, net::PeerIndex b) { events.push_back({at, what, a, b}); }
    // The relay goes silent at `at`; the watchdog closes the pair 15 s later and the redial opens it again `repair` later.
    void silent_pair(std::uint64_t at, net::PeerIndex a, net::PeerIndex b, std::uint64_t repair, std::uint64_t watchdog = 15'000) {
        schedule(at, Event::Do::silent, a, b);
        schedule(at + watchdog, Event::Do::cut, a, b);
        schedule(at + watchdog + repair, Event::Do::heal, a, b);
    }
    void apply_events() {
        for (const auto& event : events) {
            if (event.at > now || event.at + 100 <= now || stopped[event.a] || stopped[event.b]) continue;
            switch (event.what) {
            case Event::Do::silent: network.go_silent(event.a, event.b); break;
            case Event::Do::cut: network.cut(event.a, event.b); break;
            case Event::Do::heal: network.heal(event.a, event.b); break;
            }
        }
    }
    void step(std::uint64_t dt) {
        now += dt;
        network.round = now;
        apply_events();
        for (net::PeerIndex seat = 0; seat < count; ++seat) {
            if (stopped[seat]) continue;
            auto& model = models[seat];
            // The viewer arms the repair epoch only while the incident runs; the deadline starts at the epoch's first loss.
            if (incidents[seat]->running() && model.armed_incident != incidents[seat]->incident()) {
                model.armed_incident = incidents[seat]->incident(); model.deadline.reset();
            }
            bool needed = false;
            for (net::PeerIndex remote = 0; remote < count; ++remote)
                if (remote != seat && network.is_cut(seat, remote)) needed = true;
            if (!needed) model.deadline.reset();
            else if (!model.deadline) model.deadline = now + 30'000;
            while (auto frame = transports[seat]->poll()) {
                if (frame->frame.empty() || frame->frame.front() < net::mesh_report_kind || frame->frame.front() > net::mesh_confirm_kind) continue;
                if (auto received = incidents[seat]->receive(*frame, Time(static_cast<std::int64_t>(now))); !received) errors[seat] = received.error().code;
            }
            if (auto serviced = incidents[seat]->service(Time(static_cast<std::int64_t>(now))); !serviced) errors[seat] = serviced.error().code;
            while (auto result = incidents[seat]->take_result()) {
                results[seat].push_back(*result);
                if (explore) std::cout << "  t=" << now / 1000.0 << " s seat " << seat << ": "
                    << (result->kind == Kind::restored ? "restored" : result->kind == Kind::departure ? "departure" : "leave " + result->code)
                    << " incident " << result->incident << '\n';
                if (result->kind == Kind::leave) { stopped[seat] = true; left_at[seat] = now; network.isolate(seat); }
                else if (result->kind == Kind::departure) incidents[seat]->settled();
            }
        }
    }
    void run(std::uint64_t ms) {
        const auto end = now + ms;
        while (now < end) {
            step(100);
            if (trace && now % 1000 == 0) {
                std::cout << "t=" << now / 1000 << " ";
                for (net::PeerIndex seat = 0; seat < count; ++seat) {
                    const auto state = incidents[seat]->state();
                    std::cout << (stopped[seat] ? 'X' : incidents[seat]->running() ? "rdgRSsL"[static_cast<int>(state)] : '.');
                }
                std::cout << " id=" << incidents[0]->incident() << std::endl;
            }
        }
    }
    [[nodiscard]] unsigned leavers(std::string_view code = {}) const {
        unsigned total{};
        for (const auto& seat : results)
            for (const auto& result : seat) if (result.kind == Kind::leave && (code.empty() || result.code == code)) ++total;
        return total;
    }
    [[nodiscard]] unsigned restorations() const {
        unsigned total{};
        for (const auto& seat : results) for (const auto& result : seat) if (result.kind == Kind::restored) ++total;
        return total;
    }
    unsigned count{};
    bool trace{};
    PairNetwork network;
    std::uint64_t now{};
    std::vector<Event> events;
    std::vector<SeatModel> models;
    std::vector<std::unique_ptr<PairTransport>> transports;
    std::vector<std::unique_ptr<net::MeshIncident>> incidents;
    std::vector<std::vector<net::MeshIncidentResult>> results;
    std::vector<bool> stopped;
    std::vector<std::uint64_t> left_at;
    std::vector<std::string> errors;
};

void expect_agreement(const Cascade& rig, const std::string& context) {
    std::map<std::uint64_t, net::MeshIncidentResult> agreed;
    for (const auto& seat : rig.results) for (const auto& result : seat) {
        if (result.kind == Kind::leave) continue;
        const auto [found, inserted] = agreed.emplace(result.incident, result);
        if (!inserted) {
            expect(found->second.kind == result.kind, context + ": no incident mixes restoration and departure");
            expect(found->second.survivors == result.survivors, context + ": every peer agrees on survivors");
        }
    }
}

// N9-B t8130..8142: a repaired incident is followed immediately by more losses. One seat has pledged but misses the
// confirmation on its replaced stream; its neighbours already run the next incident. It must still learn the old result.
void consecutive_incidents_with_a_late_confirmation() {
    Cascade rig(9);
    std::optional<std::uint64_t> confirmed_at;
    rig.network.fault = [&](net::PeerIndex, net::PeerIndex to, std::span<const std::uint8_t> frame) {
        eawr::testing::FakeDelivery delivery;
        if (frame.front() == net::mesh_confirm_kind) {
            const auto decoded = net::decode_mesh_confirmation(frame);
            if (decoded && decoded.value().proposal.incident == 1) {
                if (!confirmed_at) {
                    confirmed_at = rig.now;
                    rig.schedule(rig.now + 100, Event::Do::cut, 4, 8);
                    rig.schedule(rig.now + 4'000, Event::Do::heal, 4, 8);
                    rig.schedule(rig.now + 6'000, Event::Do::cut, 0, 2);
                    rig.schedule(rig.now + 9'000, Event::Do::heal, 0, 2);
                    rig.schedule(rig.now + 10'000, Event::Do::cut, 3, 8);
                    rig.schedule(rig.now + 13'000, Event::Do::heal, 3, 8);
                }
                delivery.drop = to == 3 && rig.now < *confirmed_at + 5'000;
            }
        }
        return delivery;
    };
    rig.schedule(100, Event::Do::cut, 3, 7);
    rig.schedule(2'000, Event::Do::heal, 3, 7);
    rig.run(180'000);
    expect(confirmed_at.has_value(), "the first repaired incident was confirmed");
    expect(rig.leavers() == 0, "consecutive incidents retain the late confirmation: " + std::to_string(rig.leavers()) + " seats left");
    expect_agreement(rig, "N9-B consecutive incidents");
    for (net::PeerIndex seat = 0; seat < 9; ++seat) {
        expect(rig.incidents[seat]->incident() >= 3 && !rig.incidents[seat]->running(),
            "every seat completes the consecutive incidents: seat=" + std::to_string(seat)
            + " incident=" + std::to_string(rig.incidents[seat]->incident()));
        expect(rig.incidents[seat]->incident() == rig.incidents[0]->incident(), "every seat finishes on the same incident id");
        expect(rig.errors[seat].empty(), "no protocol error: " + rig.errors[seat]);
    }
}

// Keep an incident running beyond the first grace, then heal and lose a different pair before a new epoch is armed.
// The higher seat must have a full new grace even though its incident id has not changed.
void healed_stretch_resets_repair_grace() {
    Cascade rig(9);
    // A cut ring leaves no all-open decider, so nobody can remove a seat before the planned heal.
    for (net::PeerIndex seat = 0; seat < 9; ++seat) {
        const net::PeerIndex next = (seat + 1) % 9;
        rig.schedule(100, Event::Do::cut, seat, next);
        rig.schedule(35'000, Event::Do::heal, seat, next);
    }
    rig.run(35'000);
    const auto incident = rig.models[8].armed_incident;
    expect(incident != 0 && rig.incidents[8]->running(), "the long incident is still running when both pairs heal");
    expect(!rig.models[8].deadline, "healing every pair clears the expired repair deadline");
    rig.schedule(35'100, Event::Do::cut, 4, 8);
    rig.run(100);
    expect(rig.models[8].armed_incident == incident, "the next loss precedes arming a new incident");
    expect(rig.models[8].deadline == std::optional<std::uint64_t>{65'100}, "the next loss receives a full thirty-second grace");
    expect(!rig.pair_failed(8, 4), "a fresh loss is not immediately relay-failed");
    rig.run(29'900);
    expect(!rig.pair_failed(8, 4), "the fresh grace lasts until its own deadline");
    rig.run(100);
    expect(rig.pair_failed(8, 4), "a continuously cut pair fails at its new deadline");
}

// The two shapes the 17 Oct N9-H run showed, as explicit schedules (times in ms: silent, watchdog, redial).
//
// A: a pair is repaired while another one has already gone silent. The silent pair reads open, so the restoration is proposed
// and pledged over everything else; the seat behind the silent pair cannot take part until its watchdog fires, and that
// closing used to end the incident for every seat that had pledged (EAWR-NET-1781) and the one that had not (EAWR-NET-1780).
void silent_pair_during_pending_restoration(unsigned seats) {
    Cascade rig(seats);
    rig.silent_pair(1'800, 8 % seats, 5 % seats, 9'000, 14'100);
    rig.silent_pair(18'000, 4 % seats, 8 % seats, 8'700, 15'900);
    rig.run(120'000);
    expect(rig.leavers() == 0, "a pair going silent while a restoration is pending ends nobody's match: " + std::to_string(rig.leavers()) + " seats left");
    expect(rig.restorations() >= seats * 2, "both losses were agreed restored: " + std::to_string(rig.restorations()));
    for (const auto& error : rig.errors) expect(error.empty(), "no protocol error: " + error);
}

// B: pairs keep going silent one after another, so the mesh is never whole and stable for two seconds inside the first
// window (repair grace and decision window). That used to end every seat with EAWR-NET-1780; the next window decides.
void churn_overruns_the_first_window() {
    Cascade rig(9);
    rig.silent_pair(2'300, 0, 7, 7'600, 15'700);
    rig.silent_pair(10'450, 4, 3, 8'500, 15'000);
    rig.silent_pair(19'570, 7, 4, 8'600, 14'300);
    rig.silent_pair(42'075, 4, 0, 9'000, 14'650);
    rig.silent_pair(49'185, 6, 2, 7'100, 15'400);
    rig.silent_pair(50'533, 8, 2, 6'300, 15'300);
    rig.run(200'000);
    expect(rig.leavers() == 0, "churn that outlasts the first window ends nobody's match: " + std::to_string(rig.leavers()) + " seats left");
    expect(rig.incidents[0]->incident() >= 2, "the incident was restored in a later window");
}

// C: one pair never comes back while other pairs go silent and return. Exactly the higher seat of the dead pair leaves with
// the removal code; everybody else keeps one identical survivor set.
void dead_pair_amid_churn() {
    Cascade rig(9);
    rig.schedule(12'000, Event::Do::cut, 2, 7);
    rig.silent_pair(3'000, 0, 5, 6'000, 14'500);
    rig.silent_pair(20'000, 3, 4, 7'000, 14'800);
    rig.run(150'000);
    expect(rig.leavers() == 1 && rig.leavers("EAWR-NET-1778") == 1 && rig.stopped[7],
        "the dead pair costs exactly its higher seat: " + std::to_string(rig.leavers()) + " seats left, seat 7 stopped=" + (rig.stopped[7] ? "yes" : "no"));
    std::optional<net::MeshSeats> survivors;
    for (net::PeerIndex seat = 0; seat < 9; ++seat) {
        if (rig.stopped[seat]) continue;
        net::MeshSeats mine{};
        for (const auto& result : rig.results[seat]) if (result.kind == Kind::departure) mine = result.survivors;
        expect(mine != 0, "seat " + std::to_string(seat) + " agreed a departure");
        if (survivors) expect(*survivors == mine, "every continuing seat holds the same survivor set");
        survivors = mine;
    }
    for (const auto& error : rig.errors) expect(error.empty(), "no protocol error: " + error);
}

// A relay that keeps dropping pairs: every `gap` ms on average one more pair goes silent for the first `span` ms.
void fuzz(Cascade& rig, std::uint32_t seed, std::uint64_t gap, std::uint64_t span, unsigned burst) {
    std::mt19937 random(seed);
    for (std::uint64_t at = 1'000; at < span; at += 1 + random() % (2 * gap)) {
        const net::PeerIndex hub = random() % rig.count;
        for (unsigned i = 0; i < burst; ++i) {
            net::PeerIndex other = random() % rig.count;
            if (other == hub) continue;
            rig.silent_pair(at + random() % 2000, hub, other, 3'000 + random() % 7'000, 14'000 + random() % 2'000);
        }
    }
}

// Seeded relay churn: one more pair goes silent about every `gap` on average for a minute. However the schedule falls, a match
// is never ended for most of its seats, and the seats that do leave are few.
void seeded_cascades() {
    for (const std::uint64_t gap : {30'000ULL, 20'000ULL}) {
        for (std::uint32_t seed = 1; seed <= 30; ++seed) {
            Cascade rig(9);
            fuzz(rig, seed, gap, 60'000, 1);
            rig.run(200'000);
            expect_agreement(rig, "seed " + std::to_string(seed) + " gap " + std::to_string(gap));
            expect(rig.leavers() <= 2 && rig.leavers("EAWR-NET-1780") + rig.leavers("EAWR-NET-1781") <= 2,
                "seed " + std::to_string(seed) + " gap " + std::to_string(gap / 1000) + " s: " + std::to_string(rig.leavers()) + " seats left");
            for (const auto& error : rig.errors) expect(error.empty(), "no protocol error: " + error);
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    explore = argc > 1 && std::string(argv[1]) == "x";
    if (argc > 3 && std::string(argv[1]) == "fuzz") {
        unsigned bad{}, most{};
        const std::uint64_t gap = std::strtoull(argv[2], nullptr, 10) * 1000;
        const unsigned burst = static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10));
        for (std::uint32_t seed = 1; seed <= 40; ++seed) {
            Cascade rig(9);
            fuzz(rig, seed, gap, 60'000, burst);
            rig.run(200'000);
            const auto undecided = rig.leavers("EAWR-NET-1780") + rig.leavers("EAWR-NET-1781");
            if (undecided != 0) ++bad;
            if (undecided * 2 >= rig.count) ++most;
            if (rig.leavers()) std::cout << "seed " << seed << ": leavers=" << rig.leavers() << " 1780=" << rig.leavers("EAWR-NET-1780")
                << " 1781=" << rig.leavers("EAWR-NET-1781") << " restorations=" << rig.restorations() << std::endl;
        }
        std::cout << "runs with a 1780/1781: " << bad << " of 40, with half the seats or more: " << most << std::endl;
        return 0;
    }
    if (argc > 4 && std::string(argv[1]) == "seed") {
        explore = true;
        const std::uint32_t seed = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
        Cascade rig(9);
        fuzz(rig, seed, std::strtoull(argv[3], nullptr, 10) * 1000, 60'000, static_cast<unsigned>(std::strtoul(argv[4], nullptr, 10)));
        for (const auto& event : rig.events)
            std::cout << "event t=" << event.at / 1000.0 << " " << (event.what == Event::Do::silent ? "silent" : event.what == Event::Do::cut ? "cut" : "heal")
                << " " << event.a << "-" << event.b << std::endl;
        rig.trace = true;
        rig.run(120'000);
        return 0;
    }
    silent_pair_during_pending_restoration(9);
    consecutive_incidents_with_a_late_confirmation();
    silent_pair_during_pending_restoration(6);
    churn_overruns_the_first_window();
    dead_pair_amid_churn();
    healed_stretch_resets_repair_grace();
    seeded_cascades();
    if (failures != 0) return 1;
    std::cout << "mesh cascade contracts passed" << std::endl;
    return 0;
}
