#pragma once

#include "../support/check.hpp"
#include "../support/result_require.hpp"
#include "eawr/net/session_monitor.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::testing { struct WanProfile; }
namespace eawr::sim::math { class Fixed; }
namespace eawr::sim::tactical {
struct TacticalSetup;
struct TacticalContent;
struct SeatBinding;
}

namespace lockstep_session_test {
namespace net = eawr::net;
namespace sim = eawr::sim;
namespace tactical = sim::tactical;
using sim::math::Fixed;
extern eawr::testing::Checker local_checker;
extern int& failures;
void expect(bool value, const std::string& message);
using eawr::testing::result_code_colon::require;
Fixed units(std::int64_t value);
void object_turn_only_command_log();
tactical::TacticalSetup setup();
tactical::TacticalContent content();
std::vector<tactical::SeatBinding> bindings(std::uint32_t peers);
std::uint64_t batch_tick(std::span<const std::uint8_t> frame);
std::string adaptive_agreement(std::size_t workers, std::uint32_t peers, bool skewed);
void adaptive_policy_and_refusals();
std::string agreement(std::uint32_t peers, std::uint64_t delay, std::size_t workers, const std::string& fault,
    std::optional<eawr::testing::WanProfile> wan);
void missing_and_faults();
void strict_codec_and_admission();
std::string lan_click_latency(std::uint64_t delay, std::size_t workers, unsigned handoff);
void monitor_quit_and_options();
void chained_departures();
void target_timing_controls();
void final_tick_departure_ack();
void adaptive_settled_departure(bool delayed_host_ack = false, bool delayed_other_ack = false);
void adaptive_transition_proposal();
void adaptive_wire_and_gate();
void mode_neutral_eight_peer_observer();
void allied_last_human_is_named_winner();
void ai_slot_does_not_hold_one_human_match();
void partially_delivered_quit();
void departure_wire_refusals();
void monitored_maximum_lead();
void monitor_send_close_race();
void monitored_silent_peer();
void monitored_departures();
std::string chained_tactical_departures(unsigned workers, bool settling = false);
std::string retained_timing_departure(unsigned workers, bool host_leaves, bool late_ready = false,
    bool double_loss = false, bool expire = false);
std::string triple_tactical_departures(unsigned workers);
void deferred_departure_refusals();
std::string maximum_roster_departures(unsigned workers, bool double_loss);
void agreed_departures();
void script_hash_only_on_exchange_ticks();
class Monitor : public net::SessionMonitor {
public:
    bool allowed{}, hold_end{}, is_frozen{};
    std::uint64_t final_tick{3};
    std::vector<net::PeerIndex> missing_voters;
    std::vector<std::uint64_t> ticks;
    std::vector<std::vector<std::uint8_t>> inputs;
    eawr::core::Result<void> record(std::uint64_t tick, std::string_view hash,
        std::span<const std::uint8_t> state, std::span<const std::uint8_t> input, bool end) override;
    eawr::core::Result<void> receive(const net::Received& received) override;
    bool needs_encoding(std::uint64_t tick, bool end) const override;
    bool frozen() const override;
    bool may_step() const override;
    std::vector<net::PeerIndex> waiting_for_peers() const override;
};
class DepartureGate : public Monitor {
public:
    std::vector<std::pair<net::PeerIndex, std::uint64_t>> settled;
    eawr::core::Result<void> on_departure(net::PeerIndex peer, std::uint64_t cutoff) override;
};
}
