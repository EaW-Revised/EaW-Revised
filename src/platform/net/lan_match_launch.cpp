#include "lan_match_internal.hpp"
#include <iostream>

namespace eawr::presentation::godot_backend::lan_detail {

void TcpLanMatch::pump_setup(const skirmish::FixtureOptions& selected)  {
        pump_setup_impl(selected);
        if (!handed_over_) diagnostics_->view(view());
    }

void TcpLanMatch::pump_setup_impl(const skirmish::FixtureOptions& selected) {
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

void TcpLanMatch::accept()  {
        if (lobby_builder_) { (void)lobby_ready(true); return; }
        if (!view().accept_enabled || !offered_ || handed_over_) return;
        accepted_ = true; pending_ = offered_;
        const std::lock_guard lock(mutex_); view_.accept_enabled = false;
    }

void TcpLanMatch::start(const skirmish::FixtureOptions& selected)  {
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

std::optional<skirmish::FixtureOptions> TcpLanMatch::take_start()  {
        if (pending_ && lobby_ && !handed_over_ && lobby_->snapshot().phase != net::LobbyPhase::starting) { pending_.reset(); lobby_stream_ = nullptr; adapter_ = nullptr; }
        if (!pending_ || view().phase == LanPhase::ended) return {};
        handed_over_ = true; // Between main-thread polls, before the simulation thread exists.
        { const std::lock_guard lock(mutex_); view_.phase = LanPhase::barrier; view_.start_enabled = false; }
        return std::exchange(pending_, {});
    }

std::optional<skirmish::FixtureOptions> TcpLanMatch::offered_setup() const  { return offered_; }

LanView TcpLanMatch::view() const  {
        const std::lock_guard lock(mutex_);
        auto result = view_;
        if (mapping_) result.mapping = mapping_->view();
        return result;
    }

tactical::PlayerId TcpLanMatch::local_player() const  { return view().player; }

void TcpLanMatch::note_input(std::uint64_t tick, std::uint64_t frame, std::uint64_t ui_epoch)  { if (trace_) trace_->note(tick, frame, ui_epoch); }

void TcpLanMatch::note_native_input(std::uint64_t tick, std::uint64_t epoch)  { if (trace_) trace_->native_input(tick, epoch); }

void TcpLanMatch::note_issue(std::uint64_t sequence, std::uint64_t epoch)  { if (trace_) trace_->issue(sequence, epoch); }

void TcpLanMatch::set_submission_observer(std::function<void(std::uint64_t, std::uint64_t)> observer)  {
        const std::lock_guard lock(input_epoch_mutex_);
        submission_observer_ = std::move(observer);
    }

void TcpLanMatch::set_input_epoch_provider(std::function<std::optional<std::uint64_t>()> provider)  {
        const std::lock_guard lock(input_epoch_mutex_);
        input_epoch_provider_ = std::move(provider);
    }

platform::LiveStepDriver TcpLanMatch::driver(const tactical::TacticalSetup& setup)  {
        return [self = shared_from_this(), setup](platform::LiveStepContext& context,
            std::span<const tactical::PlayerCommand> commands, std::uint64_t now) {
                self->progress(context, now);
                self->diagnostics_->tick = context.completed_tick();
                if (self->trace_ && self->trace_->failed()) {
                    auto result = fail<bool>("Cannot open or write LAN acceptance trace.");
                    self->set_failure(result.error());
                    return result;
                }
                auto result = self->drive(setup, context, commands, now);
                if (!result && self->view().phase != LanPhase::ended) self->set_failure(result.error());
                self->diagnostics_->tick = context.completed_tick();
                self->diagnostics_->view(self->view());
                if (self->view().phase == LanPhase::ended) { self->diagnostics_->stall(false, now); self->diagnostics_->ledger.observe(now, false, 0); }
                // Trace observers only record I/O failure; report it at the driver boundary.
                if (self->trace_ && self->trace_->failed()) {
                    result = fail<bool>("Cannot open or write LAN acceptance trace.");
                    self->set_failure(result.error());
                }
                if (!result && self->smoke_progress_) std::cerr << "EAWR LAN drive failed: " << result.error().message << std::endl;
                return result;
            };
    }

} // namespace eawr::presentation::godot_backend::lan_detail
