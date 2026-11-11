#include "eawr/net/galactic_setup.hpp"
#include "eawr/net/lan_directory.hpp"

#include <algorithm>
#include <chrono>
#include <set>

namespace eawr::net {
namespace {
using Bytes = std::vector<std::uint8_t>;
namespace gc = sim::galactic;
constexpr std::string_view magic = "GCSU";

template<class T> core::Result<T> refuse(SetupReason reason, std::string message) {
    return core::Result<T>::failure(SetupRefusal{reason, {}, std::move(message), {}, {}}.diagnostic());
}
void word(Bytes& bytes, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
}
void blob(Bytes& bytes, std::span<const std::uint8_t> value) {
    word(bytes, value.size(), 4);
    bytes.insert(bytes.end(), value.begin(), value.end());
}
void text(Bytes& bytes, std::string_view value) {
    blob(bytes, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}
struct Reader {
    std::span<const std::uint8_t> bytes;
    bool word(std::uint64_t& value, unsigned width) {
        if (bytes.size() < width) return false;
        value = 0;
        for (unsigned i = 0; i < width; ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8U);
        bytes = bytes.subspan(width);
        return true;
    }
    bool flag(bool& value) {
        std::uint64_t raw{};
        if (!word(raw, 1) || raw > 1) return false;
        value = raw == 1;
        return true;
    }
    bool blob(Bytes& value, std::size_t limit) {
        std::uint64_t size{};
        if (!word(size, 4) || size > limit || size > bytes.size()) return false;
        value.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
        bytes = bytes.subspan(static_cast<std::size_t>(size));
        return true;
    }
    // Hashed text stays byte-exact: anything the display sanitiser would repair is refused.
    bool text(std::string& value, std::size_t limit) {
        Bytes data;
        if (!blob(data, limit)) return false;
        const std::string_view raw(reinterpret_cast<const char*>(data.data()), data.size());
        if (sanitize_lan_text(raw, limit) != raw) return false;
        value = std::string(raw);
        return true;
    }
};
}

core::Result<Bytes> encode_galactic_setup(const GalacticSetupPayload& payload) {
    const auto& roster = payload.roster;
    if (auto valid = gc::validate_mp_roster(roster); !valid) return refuse<Bytes>(SetupReason::options, valid.error().message);
    if (payload.command_quota != max_batch_commands || payload.byte_quota != max_batch_bytes)
        return refuse<Bytes>(SetupReason::options, "batch quotas differ from the session limits");
    const auto nonzero = [](const auto& digest) { return std::any_of(digest.begin(), digest.end(), [](auto byte) { return byte != 0; }); };
    const bool checkpoint = payload.start == GalacticStart::checkpoint;
    if (payload.start != GalacticStart::new_campaign && !checkpoint)
        return refuse<Bytes>(SetupReason::options, "unknown campaign start discriminator");
    if (checkpoint ? (payload.initial_tick == 0 || payload.initial_tick >= gc::galactic_max_ticks
            || !nonzero(payload.checkpoint_digest) || !nonzero(payload.state_digest)
            || !nonzero(payload.frontier_digest) || !nonzero(payload.terms_digest))
        : (payload.initial_tick != 0 || nonzero(payload.checkpoint_digest) || nonzero(payload.state_digest)
            || nonzero(payload.frontier_digest) || nonzero(payload.terms_digest)))
        return refuse<Bytes>(SetupReason::options, "inconsistent campaign resume agreement");
    Bytes bytes(magic.begin(), magic.end());
    word(bytes, 3, 4);
    text(bytes, roster.campaign); text(bytes, roster.difficulty);
    word(bytes, roster.custom ? 1U : 0U, 1);
    word(bytes, static_cast<std::uint64_t>(roster.credits), 8);
    word(bytes, static_cast<std::uint64_t>(roster.starting_tech), 8);
    word(bytes, static_cast<std::uint64_t>(roster.maximum_tech), 8);
    word(bytes, roster.allow_heroes ? 1U : 0U, 1);
    word(bytes, roster.vote_timeout_seconds, 4);
    word(bytes, payload.command_quota, 4); word(bytes, payload.byte_quota, 4);
    word(bytes, roster.players.size(), 4);
    for (const auto& player : roster.players) {
        word(bytes, player.player, 4); word(bytes, player.faction_id, 4); word(bytes, player.flags, 4);
        text(bytes, player.faction);
        word(bytes, player.active ? 1U : 0U, 1); word(bytes, static_cast<std::uint8_t>(player.dropout), 1);
        blob(bytes, player.ai_configuration);
    }
    word(bytes, roster.seats.size(), 4);
    for (const auto& seat : roster.seats) {
        word(bytes, seat.seat_id.value, 4); word(bytes, seat.player, 4);
        word(bytes, static_cast<std::uint8_t>(seat.kind), 1); word(bytes, seat.lobby_slot, 4);
        word(bytes, seat.active ? 1U : 0U, 1); word(bytes, seat.human_side_id, 4);
    }
    word(bytes, roster.teams.size(), 4);
    for (const auto& team : roster.teams) {
        word(bytes, team.side, 4); word(bytes, team.lead.value, 4); word(bytes, team.observer_ai ? 1U : 0U, 1);
    }
    bytes.insert(bytes.end(), roster.tick_zero.begin(), roster.tick_zero.end());
    word(bytes, static_cast<std::uint8_t>(payload.start), 1); word(bytes, payload.initial_tick, 8);
    for (const auto* digest : {&payload.checkpoint_digest, &payload.state_digest, &payload.frontier_digest, &payload.terms_digest})
        bytes.insert(bytes.end(), digest->begin(), digest->end());
    if (bytes.size() > max_setup_bytes - 16384)
        return refuse<Bytes>(SetupReason::options, "campaign setup exceeds offer budget");
    return core::Result<Bytes>::success(std::move(bytes));
}

core::Result<GalacticSetupPayload> decode_galactic_setup(std::span<const std::uint8_t> input) {
    using Result = core::Result<GalacticSetupPayload>;
    const auto malformed = [] { return refuse<GalacticSetupPayload>(SetupReason::malformed, "malformed campaign setup payload"); };
    if (input.size() < magic.size() + 4 || input.size() > max_setup_bytes
        || !std::equal(magic.begin(), magic.end(), input.begin())) return malformed();
    Reader reader{input.subspan(magic.size())};
    GalacticSetupPayload payload;
    auto& roster = payload.roster;
    std::uint64_t version{}, credits{}, start{}, maximum{}, quota_commands{}, quota_bytes{}, count{}, vote_timeout{};
    if (!reader.word(version, 4) || version != 3 || !reader.text(roster.campaign, gc::mp_roster_max_text)
        || !reader.text(roster.difficulty, gc::mp_roster_max_text) || !reader.flag(roster.custom)
        || !reader.word(credits, 8) || !reader.word(start, 8) || !reader.word(maximum, 8)
        || !reader.flag(roster.allow_heroes) || !reader.word(vote_timeout, 4) || !reader.word(quota_commands, 4) || !reader.word(quota_bytes, 4)
        || !reader.word(count, 4) || count > gc::galactic_max_players) return malformed();
    roster.credits = static_cast<std::int64_t>(credits);
    roster.starting_tech = static_cast<std::int64_t>(start);
    roster.maximum_tech = static_cast<std::int64_t>(maximum);
    roster.vote_timeout_seconds = static_cast<std::uint32_t>(vote_timeout);
    payload.command_quota = static_cast<std::uint32_t>(quota_commands);
    payload.byte_quota = static_cast<std::uint32_t>(quota_bytes);
    for (std::uint64_t i = 0; i < count; ++i) {
        gc::MpRosterPlayer player;
        std::uint64_t id{}, faction_id{}, flags{}, dropout{};
        if (!reader.word(id, 4) || !reader.word(faction_id, 4) || !reader.word(flags, 4)
            || !reader.text(player.faction, gc::mp_roster_max_text) || !reader.flag(player.active)
            || !reader.word(dropout, 1) || !reader.blob(player.ai_configuration, gc::mp_roster_max_ai_configuration))
            return malformed();
        player.player = static_cast<gc::PlayerId>(id); player.faction_id = static_cast<gc::FactionId>(faction_id);
        player.flags = static_cast<std::uint32_t>(flags);
        player.dropout = static_cast<gc::FactionControl>(dropout);
        roster.players.push_back(std::move(player));
    }
    if (!reader.word(count, 4) || count > gc::galactic_max_seats) return malformed();
    for (std::uint64_t i = 0; i < count; ++i) {
        gc::Seat seat;
        std::uint64_t id{}, player{}, kind{}, slot{}, side{};
        if (!reader.word(id, 4) || !reader.word(player, 4) || !reader.word(kind, 1) || !reader.word(slot, 4)
            || !reader.flag(seat.active) || !reader.word(side, 4)) return malformed();
        seat.seat_id = sim::SeatId{static_cast<std::uint32_t>(id)}; seat.player = static_cast<gc::PlayerId>(player);
        seat.kind = static_cast<sim::SeatKind>(kind); seat.lobby_slot = static_cast<std::uint32_t>(slot);
        seat.human_side_id = static_cast<gc::HumanSideId>(side);
        roster.seats.push_back(seat);
    }
    if (!reader.word(count, 4) || count > 2) return malformed();
    for (std::uint64_t i = 0; i < count; ++i) {
        gc::TeamLead team;
        std::uint64_t side{}, lead{};
        if (!reader.word(side, 4) || !reader.word(lead, 4) || !reader.flag(team.observer_ai)) return malformed();
        team.side = static_cast<gc::HumanSideId>(side); team.lead = sim::SeatId{static_cast<std::uint32_t>(lead)};
        roster.teams.push_back(team);
    }
    if (reader.bytes.size() != roster.tick_zero.size() + 1 + 8 + 128) return malformed();
    std::copy_n(reader.bytes.begin(), roster.tick_zero.size(), roster.tick_zero.begin());
    reader.bytes = reader.bytes.subspan(roster.tick_zero.size());
    std::uint64_t start_kind{};
    if (!reader.word(start_kind, 1) || !reader.word(payload.initial_tick, 8)) return malformed();
    payload.start = static_cast<GalacticStart>(start_kind);
    for (auto* digest : {&payload.checkpoint_digest, &payload.state_digest, &payload.frontier_digest, &payload.terms_digest}) {
        std::copy_n(reader.bytes.begin(), digest->size(), digest->begin()); reader.bytes = reader.bytes.subspan(digest->size());
    }
    // Encoding validates roster enum values; comparison then enforces canonical bytes.
    auto encoded = encode_galactic_setup(payload);
    if (!encoded) return Result::failure(encoded.error());
    if (!std::equal(input.begin(), input.end(), encoded.value().begin(), encoded.value().end())) return malformed();
    return Result::success(std::move(payload));
}

std::string_view galactic_setup_difference(std::span<const std::uint8_t> host, std::span<const std::uint8_t> local) {
    const auto left = decode_galactic_setup(host);
    const auto right = decode_galactic_setup(local);
    if (!left || !right) return {};
    const auto& a = left.value().roster;
    const auto& b = right.value().roster;
    // AI claims first: the tick-zero claim is derived from them, so a differing claim also moves the tick-zero claim.
    if (a.players.size() == b.players.size()) {
        for (std::size_t i = 0; i < a.players.size(); ++i)
            if (a.players[i].ai_configuration != b.players[i].ai_configuration) return setup_difference_ai_configuration;
    }
    if (a.tick_zero != b.tick_zero) return setup_difference_start_state;
    return {};
}

core::Result<void> validate_galactic_offer(const SetupOffer& offer) {
    if (offer.seats.size() > gc::galactic_max_human_seats)
        return refuse<void>(SetupReason::options, "a campaign setup has at most eight human seats");
    auto payload = decode_galactic_setup(offer.setup);
    if (!payload) return core::Result<void>::failure(payload.error());
    if (payload.value().initial_tick >= offer.final_tick_count)
        return refuse<void>(SetupReason::options, "checkpoint horizon has no remaining tick");
    const auto& roster = payload.value().roster;
    // Peers and seats: exactly the human seats of the roster, each once, on the faction the roster names.
    std::set<sim::SeatId> offered;
    for (std::size_t index = 0; index < offer.seats.size(); ++index) {
        const auto& seat = offer.seats[index];
        if (seat.peer != index) return refuse<void>(SetupReason::options, "peer indices are not canonical");
        const auto found = std::find_if(roster.seats.begin(), roster.seats.end(), [&](const auto& row) {
            return row.seat_id == seat.seat; });
        if (found == roster.seats.end() || found->kind != sim::SeatKind::human || !found->active
            || found->player != seat.player || !offered.insert(seat.seat).second)
            return refuse<void>(SetupReason::options, "peer seat is not an active human seat of the campaign roster");
    }
    const auto humans = static_cast<std::size_t>(std::count_if(roster.seats.begin(), roster.seats.end(),
        [](const auto& seat) { return seat.kind == sim::SeatKind::human && seat.active; }));
    if (payload.value().start == GalacticStart::new_campaign && humans != offer.seats.size())
        return refuse<void>(SetupReason::options, "every human seat of the campaign roster needs one peer");
    const auto zero = std::find_if(offer.identity.content.domains.begin(), offer.identity.content.domains.end(),
        [](const auto& domain) { return domain.name == galactic_tick_zero_domain; });
    if (zero == offer.identity.content.domains.end() || zero->digest != roster.tick_zero)
        return refuse<void>(SetupReason::options, "offer identity and campaign setup disagree on the tick-zero digest");
    return core::Result<void>::success();
}

} // namespace eawr::net
// Pre-setup distribution shares the canonical GC setup codec, independently of
// lockstep's input queues and recovery log. No simulation or file I/O runs here.
#include "eawr/net/galactic_checkpoint_transfer.hpp"
#include "eawr/core/sha256.hpp"

namespace eawr::net {
namespace {
core::Diagnostic transfer_error(std::string reason) {
    core::Diagnostic error; error.code = "EAWR-NET-SAVE-REFUSED";
    error.message = "checkpoint transfer: " + std::move(reason); return error;
}
Bytes transfer_frame(std::uint8_t kind, std::uint64_t epoch) {
    Bytes frame{kind}; word(frame, epoch, 8); return frame;
}
Bytes restore_proof(const SaveManifest& manifest) {
    auto payload = decode_galactic_setup(manifest.offer.setup);
    Bytes frame = transfer_frame(save_restore_kind, manifest.epoch);
    if (manifest.recovery) for (const auto& digest : manifest.recovery->proof)
        frame.insert(frame.end(), digest.begin(), digest.end());
    else if (payload) for (const auto* digest : {&payload.value().checkpoint_digest, &payload.value().state_digest,
        &payload.value().frontier_digest, &payload.value().terms_digest})
        frame.insert(frame.end(), digest->begin(), digest->end());
    return frame;
}
auto file_digest(const SaveManifest& manifest) {
    return manifest.recovery ? manifest.recovery->proof[0] : decode_galactic_setup(manifest.offer.setup).value().checkpoint_digest;
}
core::Result<void> valid_manifest(const SaveManifest& manifest, const MessageTransport& transport, bool recovery_only) {
    if (!manifest.epoch || !manifest.byte_length || manifest.byte_length > save_transfer_limit
        || manifest.peers.size() < 2 || (recovery_only ? manifest.peers.size() != 2 : manifest.peers.size() != manifest.offer.seats.size())
        || manifest.peers.front() != 0)
        return core::Result<void>::failure(transfer_error("invalid_manifest"));
    std::set<PeerIndex> unique;
    for (const auto peer : manifest.peers)
        if (peer >= transport.peer_count() || !unique.insert(peer).second)
            return core::Result<void>::failure(transfer_error("duplicate_or_extra_peer"));
    if (!unique.contains(transport.self()))
        return core::Result<void>::failure(transfer_error("unassigned_peer"));
    if (recovery_only) {
        if (!manifest.recovery || manifest.recovery->initial_tick >= gc::galactic_max_ticks
            || manifest.recovery->source == manifest.recovery->target)
            return core::Result<void>::failure(transfer_error("invalid_recovery_manifest"));
        const auto& identity = *manifest.recovery;
        for (const auto seat : {identity.source, identity.target})
            if (std::none_of(manifest.offer.seats.begin(), manifest.offer.seats.end(),
                [&](const auto& row) { return row.seat == seat; }))
                return core::Result<void>::failure(transfer_error("recovery_seat_binding"));
        for (const auto& digest : identity.proof)
            if (std::all_of(digest.begin(), digest.end(), [](auto byte) { return byte == 0; }))
                return core::Result<void>::failure(transfer_error("recovery_identity_missing"));
        const auto encoded = encode_setup_offer(manifest.offer);
        if (!encoded || core::sha256(encoded.value()) != identity.proof[3])
            return core::Result<void>::failure(transfer_error("recovery_offer_mismatch"));
        return validate_galactic_offer(manifest.offer);
    }
    if (manifest.recovery) return core::Result<void>::failure(transfer_error("recovery_manifest_in_lobby"));
    auto payload = decode_galactic_setup(manifest.offer.setup);
    if (!payload || payload.value().start != GalacticStart::checkpoint)
        return core::Result<void>::failure(transfer_error("checkpoint_selection_required"));
    return validate_galactic_offer(manifest.offer);
}
}
class GalacticCheckpointTransfer::Impl {
public:
    SaveTransferView view;
    std::optional<SaveManifest> selected;
    std::shared_ptr<const Bytes> outgoing;
    Bytes incoming;
    std::vector<std::uint64_t> sent, acked, progress;
    std::vector<bool> restored;
    std::uint64_t started{}, last_now{}, local_progress{}, high_epoch{};
    bool published{}, recovery_only{}, awaiting_restore{};
    AsyncRestore async_restore;
    std::shared_ptr<const Bytes> staged;
    std::shared_ptr<Trace> trace;
    std::uint64_t acks_seen{};

    void emit(std::string_view event, const std::string& detail) const {
        if (trace && *trace) (*trace)(event, detail);
    }
    std::string at(std::uint64_t now) const {
        return "epoch=" + std::to_string(view.epoch) + " now=" + std::to_string(now);
    }

    // Real time spent restoring, from just before the restore callback to now (the callback itself, or its worker's finish).
    std::chrono::steady_clock::time_point restore_began{};
    void note_restore_wall() {
        view.restore_wall_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - restore_began).count());
    }
    void note_chunk(std::uint64_t now) {
        if (!view.timed_chunks) { view.timed_chunks = true; view.first_chunk_ms = now; }
        view.last_chunk_ms = now;
    }
    // Fills one peer's window. Called from pump and, on the host, when an acknowledgement frees space,
    // so the next chunks leave in the same frame the acknowledgement arrived in.
    core::Result<void> send_window(MessageTransport& transport, PeerIndex peer, std::uint64_t now) {
        while (sent[peer] < view.total && sent[peer] - acked[peer] < save_window_chunks * save_chunk_bytes) {
            const auto offset = sent[peer];
            const auto length = std::min<std::uint64_t>(save_chunk_bytes, view.total - offset);
            const bool final = offset + length == view.total;
            auto frame = transfer_frame(save_chunk_kind, view.epoch);
            word(frame, offset / save_chunk_bytes, 4); word(frame, final ? 1 : 0, 1);
            frame.insert(frame.end(), outgoing->begin() + static_cast<std::ptrdiff_t>(offset),
                outgoing->begin() + static_cast<std::ptrdiff_t>(offset + length));
            if (final) {
                const auto digest = file_digest(*selected);
                frame.insert(frame.end(), digest.begin(), digest.end());
            }
            if (auto result = transport.send(peer, frame); !result) return refuse(transport, "chunk_send_failed");
            if (!view.timed_chunks) {
                view.timed_chunks = true; view.first_chunk_ms = now;
                emit("first-chunk", at(now) + " peer=" + std::to_string(peer) + " window_chunks=" + std::to_string(save_window_chunks));
            }
            sent[peer] += length;
        }
        return core::Result<void>::success();
    }

    core::Result<void> refuse(MessageTransport& transport, std::string reason, bool notify = true) {
        if (!view.refusal || reason == "selection_cleared") {
            view.refusal = transfer_error(reason); view.agreed = false; view.restored = false;
            emit("refused", at(last_now) + " reason=" + reason.substr(0, 120) + " notify=" + (notify ? "yes" : "no")
                + " received=" + std::to_string(view.received) + " total=" + std::to_string(view.total));
            auto frame = transfer_frame(save_cancel_kind, view.epoch);
            text(frame, reason.substr(0, 256));
            if (selected && notify) for (const auto peer : selected->peers)
                if (peer != transport.self() && (transport.self() == 0 || peer == 0)
                    && transport.link(peer) == LinkState::open) static_cast<void>(transport.send(peer, frame));
        }
        Bytes{}.swap(incoming); outgoing.reset(); staged.reset(); async_restore = {}; awaiting_restore = false;
        return core::Result<void>::failure(*view.refusal);
    }
};
GalacticCheckpointTransfer::GalacticCheckpointTransfer(bool recovery_only) : impl_(std::make_unique<Impl>()) {
    impl_->recovery_only = recovery_only;
}
GalacticCheckpointTransfer::~GalacticCheckpointTransfer() = default;
GalacticCheckpointTransfer::GalacticCheckpointTransfer(GalacticCheckpointTransfer&&) noexcept = default;
GalacticCheckpointTransfer& GalacticCheckpointTransfer::operator=(GalacticCheckpointTransfer&&) noexcept = default;
SaveTransferView GalacticCheckpointTransfer::view() const { return impl_->view; }
const SaveManifest* GalacticCheckpointTransfer::manifest() const { return impl_->selected ? &*impl_->selected : nullptr; }
std::shared_ptr<const Bytes> GalacticCheckpointTransfer::restored_bytes() const noexcept {
    return impl_->view.restored && !impl_->view.refusal ? impl_->staged : nullptr;
}
void GalacticCheckpointTransfer::reset() {
    const auto epoch = impl_->high_epoch; const auto recovery_only = impl_->recovery_only;
    impl_ = std::make_unique<Impl>(); impl_->high_epoch = epoch; impl_->recovery_only = recovery_only; impl_->trace = trace_;
}
void GalacticCheckpointTransfer::set_trace(Trace trace) {
    trace_ = std::make_shared<Trace>(std::move(trace));
    impl_->trace = trace_;
}
void GalacticCheckpointTransfer::cancel(MessageTransport& transport, std::string reason) {
    static_cast<void>(impl_->refuse(transport, std::move(reason)));
}
core::Result<void> GalacticCheckpointTransfer::select(MessageTransport& transport, SaveManifest manifest,
    std::shared_ptr<const Bytes> bytes, Restore restore, std::uint64_t now) {
    if (transport.self() != 0 || manifest.epoch <= impl_->high_epoch || !bytes
        || bytes->size() != manifest.byte_length || !restore)
        return core::Result<void>::failure(transfer_error("invalid_selection_or_stale_epoch"));
    manifest.description = sanitize_lan_menu_text(manifest.description, save_description_limit);
    manifest.date = sanitize_lan_menu_text(manifest.date, save_date_limit);
    if (auto valid = valid_manifest(manifest, transport, impl_->recovery_only); !valid) return valid;
    if (core::sha256(*bytes) != file_digest(manifest))
        return core::Result<void>::failure(transfer_error("file_digest_mismatch"));
    if (auto restored = restore(manifest, *bytes); !restored) return restored;
    if (impl_->selected && !impl_->view.refusal) cancel(transport, "selection_changed");
    auto state = std::make_unique<Impl>(); state->high_epoch = manifest.epoch; state->recovery_only = impl_->recovery_only;
    state->trace = trace_;
    state->view.epoch = manifest.epoch; state->view.total = manifest.byte_length;
    state->view.chunks = (manifest.byte_length + save_chunk_bytes - 1) / save_chunk_bytes;
    state->view.received = manifest.byte_length; state->view.restored = true;
    state->started = now; state->last_now = now; state->local_progress = now;
    state->sent.resize(transport.peer_count()); state->acked.resize(transport.peer_count());
    state->progress.assign(transport.peer_count(), now); state->restored.resize(transport.peer_count());
    state->restored[0] = true; state->selected = std::move(manifest); state->outgoing = std::move(bytes);
    impl_ = std::move(state);
    impl_->emit("selected", impl_->at(now) + " bytes=" + std::to_string(impl_->view.total) + " chunks=" + std::to_string(impl_->view.chunks)
        + " peers=" + std::to_string(impl_->selected->peers.size()));
    return core::Result<void>::success();
}
core::Result<void> GalacticCheckpointTransfer::receive(MessageTransport& transport, const Received& received,
    Restore restore, std::uint64_t now) {
    if (!handles(received.frame) || received.from >= transport.peer_count() || received.from == transport.self()
        || received.frame.size() > (received.frame.front() == save_manifest_kind ? max_setup_bytes + 512 : save_chunk_bytes + 64))
        return core::Result<void>::failure(transfer_error("invalid_frame"));
    Reader in{std::span<const std::uint8_t>(received.frame).subspan(1)};
    std::uint64_t epoch{};
    if (!in.word(epoch, 8) || !epoch) return core::Result<void>::failure(transfer_error("invalid_epoch"));
    const auto kind = received.frame.front();
    if (kind == save_manifest_kind) {
        if (received.from != 0 || transport.self() == 0)
            return core::Result<void>::failure(transfer_error("unauthenticated_manifest"));
        if (epoch <= impl_->high_epoch) return core::Result<void>::failure(transfer_error("stale_selection_epoch"));
        SaveManifest manifest; manifest.epoch = epoch;
        std::uint64_t count{}; Bytes offer;
        if (!in.word(manifest.byte_length, 8) || !in.word(count, 4) || count < 2 || count > max_match_seats)
            return core::Result<void>::failure(transfer_error("invalid_manifest"));
        for (std::uint64_t i = 0; i < count; ++i) {
            std::uint64_t peer{};
            if (!in.word(peer, 4)) return core::Result<void>::failure(transfer_error("truncated_manifest"));
            manifest.peers.push_back(static_cast<PeerIndex>(peer));
        }
        if (!in.blob(offer, max_setup_bytes))
            return core::Result<void>::failure(transfer_error("truncated_manifest"));
        if (!in.bytes.empty()) {
            std::uint64_t marker{};
            if (!in.word(marker, 1)) return core::Result<void>::failure(transfer_error("invalid_manifest_extension"));
            if (impl_->recovery_only && marker == 1) {
                std::uint64_t source{}, target{};
                RecoveryTransferIdentity identity;
                if (!in.word(identity.initial_tick, 8) || !in.word(source, 4) || !in.word(target, 4)
                    || in.bytes.size() != 128)
                    return core::Result<void>::failure(transfer_error("invalid_recovery_manifest"));
                identity.source = sim::SeatId{static_cast<std::uint32_t>(source)};
                identity.target = sim::SeatId{static_cast<std::uint32_t>(target)};
                for (auto& digest : identity.proof) {
                    std::copy_n(in.bytes.begin(), digest.size(), digest.begin());
                    in.bytes = in.bytes.subspan(digest.size());
                }
                manifest.recovery = identity;
            } else if (!impl_->recovery_only && marker == save_display_metadata_version) {
                Bytes description, date;
                if (!in.blob(description, save_description_limit) || !in.blob(date, save_date_limit) || !in.bytes.empty())
                    return core::Result<void>::failure(transfer_error("invalid_display_metadata"));
                manifest.description = sanitize_lan_menu_text(std::string(description.begin(), description.end()), save_description_limit);
                manifest.date = sanitize_lan_menu_text(std::string(date.begin(), date.end()), save_date_limit);
            } else return core::Result<void>::failure(transfer_error("invalid_manifest_extension"));
        }
        auto decoded = decode_setup_offer(offer);
        if (!decoded) return core::Result<void>::failure(decoded.error());
        manifest.offer = std::move(decoded).value();
        if (auto valid = valid_manifest(manifest, transport, impl_->recovery_only); !valid) return valid;
        auto state = std::make_unique<Impl>(); state->high_epoch = epoch; state->recovery_only = impl_->recovery_only;
        state->trace = trace_;
        state->view.epoch = epoch; state->view.total = manifest.byte_length;
        state->view.chunks = (manifest.byte_length + save_chunk_bytes - 1) / save_chunk_bytes;
        state->started = now; state->last_now = now; state->local_progress = now;
        state->selected = std::move(manifest);
        state->incoming.reserve(static_cast<std::size_t>(state->view.total));
        impl_ = std::move(state);
        impl_->emit("manifest", impl_->at(now) + " bytes=" + std::to_string(impl_->view.total) + " chunks=" + std::to_string(impl_->view.chunks)
            + " peers=" + std::to_string(impl_->selected->peers.size()));
        return core::Result<void>::success();
    }
    auto& state = *impl_;
    if (!state.selected || epoch != state.view.epoch)
        return core::Result<void>::failure(transfer_error("stale_selection_epoch"));
    if (std::find(state.selected->peers.begin(), state.selected->peers.end(), received.from) == state.selected->peers.end())
        return core::Result<void>::failure(transfer_error("unassigned_peer"));
    if (kind == save_cancel_kind) {
        std::string reason;
        if (!in.text(reason, 256) || !in.bytes.empty()) reason = "malformed_cancellation";
        // Only the host can replace a refusal with an explicit New Campaign.
        // Handle it before the retained refusal and never echo it back to the host.
        if (reason == "selection_cleared") {
            if (received.from != 0) return core::Result<void>::failure(transfer_error("unauthenticated_selection_clear"));
            return state.refuse(transport, reason, false);
        }
        return state.refuse(transport, reason);
    }
    if (state.view.refusal) return core::Result<void>::failure(*state.view.refusal);
    if (kind == save_chunk_kind && received.from == 0 && transport.self() != 0 && !state.view.restored) {
        std::uint64_t index{}, final{};
        if (!in.word(index, 4) || !in.word(final, 1) || final > 1 || index * save_chunk_bytes != state.incoming.size())
            return state.refuse(transport, "chunk_index_or_truncation");
        const auto remaining = state.view.total - state.incoming.size();
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(save_chunk_bytes, remaining));
        if (!length || in.bytes.size() != length + (final ? 32U : 0U) || (final != 0) != (length == remaining))
            return state.refuse(transport, "chunk_length_or_truncation");
        state.incoming.insert(state.incoming.end(), in.bytes.begin(), in.bytes.begin() + static_cast<std::ptrdiff_t>(length));
        state.view.received = state.incoming.size(); state.local_progress = now;
        state.note_chunk(now);
        if (index % 10 == 0 || final != 0)
            state.emit("chunk", state.at(now) + " index=" + std::to_string(index) + " received=" + std::to_string(state.view.received)
                + " total=" + std::to_string(state.view.total));
        auto ack = transfer_frame(save_progress_kind, epoch); word(ack, state.view.received, 8);
        if (auto sent = transport.send(0, ack); !sent) return state.refuse(transport, "progress_send_failed");
        if (final) {
            const auto digest = core::sha256(state.incoming);
            if (digest != file_digest(*state.selected)
                || !std::equal(digest.begin(), digest.end(), in.bytes.begin() + static_cast<std::ptrdiff_t>(length)))
                return state.refuse(transport, "file_digest_mismatch");
            if (!restore) return state.refuse(transport, "restore_unavailable");
            const auto restore_began = std::chrono::steady_clock::now();
            state.emit("restore-start", state.at(now) + " bytes=" + std::to_string(state.incoming.size()));
            auto restored = restore(*state.selected, state.incoming);
            if (!restored) return state.refuse(transport, restored.error().code + ": " + restored.error().message);
            state.restore_began = restore_began;
            if (state.awaiting_restore) { state.emit("restore-async", state.at(now)); return core::Result<void>::success(); }
            state.view.restored = true; state.view.restored_ms = now; state.note_restore_wall();
            state.emit("restore-end", state.at(now) + " wire_ms=" + std::to_string(state.view.last_chunk_ms - state.view.first_chunk_ms)
                + " restore_wall_ms=" + std::to_string(state.view.restore_wall_ms) + " pumps=" + std::to_string(state.view.pumps));
            if (!state.recovery_only) { if (!state.staged) state.staged = std::make_shared<const Bytes>(std::move(state.incoming)); }
            else Bytes{}.swap(state.incoming);
            if (auto sent = transport.send(0, restore_proof(*state.selected)); !sent)
                return state.refuse(transport, "restore_ack_send_failed");
        }
        return core::Result<void>::success();
    }
    if (kind == save_progress_kind && transport.self() == 0) {
        std::uint64_t offset{};
        if (!in.word(offset, 8) || !in.bytes.empty() || offset <= state.acked[received.from]
            || offset > state.sent[received.from] || (offset != state.view.total && offset % save_chunk_bytes != 0))
            return state.refuse(transport, "invalid_progress");
        state.acked[received.from] = offset; state.progress[received.from] = now;
        if (++state.acks_seen % 10 == 1 || offset == state.view.total)
            state.emit("chunk-ack", state.at(now) + " peer=" + std::to_string(received.from) + " acked=" + std::to_string(offset)
                + " sent=" + std::to_string(state.sent[received.from]) + " total=" + std::to_string(state.view.total));
        if (offset == state.view.total) state.view.last_chunk_ms = std::max(state.view.last_chunk_ms, now);
        // The acknowledgement freed window space: refill it now instead of waiting for the next pump.
        if (!state.restored[received.from]) return state.send_window(transport, received.from, now);
        return core::Result<void>::success();
    }
    if (kind == save_restore_kind && transport.self() == 0) {
        if (received.frame != restore_proof(*state.selected) || state.acked[received.from] != state.view.total)
            return state.refuse(transport, "restore_hash_or_frontier_mismatch");
        state.restored[received.from] = true; state.progress[received.from] = now;
        state.view.restored_ms = std::max(state.view.restored_ms, now);
        state.view.agreed = std::all_of(state.selected->peers.begin(), state.selected->peers.end(),
            [&](auto peer) { return state.restored[peer]; });
        state.emit("peer-restored", state.at(now) + " peer=" + std::to_string(received.from));
        if (state.view.agreed) {
            state.emit("agreed", state.at(now) + " wire_ms=" + std::to_string(state.view.last_chunk_ms - state.view.first_chunk_ms)
                + " restore_ms=" + std::to_string(state.view.restored_ms - state.view.last_chunk_ms)
                + " pumps=" + std::to_string(state.view.pumps));
            state.outgoing.reset();
        }
        return core::Result<void>::success();
    }
    return state.refuse(transport, "unexpected_transfer_message");
}
core::Result<void> GalacticCheckpointTransfer::receive_recovery(MessageTransport& transport, const Received& received,
    AsyncRestore restore, std::uint64_t now) {
    if (!impl_->recovery_only || !restore)
        return core::Result<void>::failure(transfer_error("recovery_restore_unavailable"));
    return receive_async(transport, received, std::move(restore), now);
}
core::Result<void> GalacticCheckpointTransfer::receive_async(MessageTransport& transport, const Received& received,
    AsyncRestore restore, std::uint64_t now) {
    if (!restore) return core::Result<void>::failure(transfer_error("restore_unavailable"));
    return receive(transport, received, [this, restore = std::move(restore)](const SaveManifest& manifest, std::span<const std::uint8_t>) mutable {
        auto& state = *impl_;
        state.staged = std::make_shared<const Bytes>(std::move(state.incoming));
        auto restored = restore(manifest, state.staged);
        if (!restored) return core::Result<void>::failure(restored.error());
        if (!restored.value()) { state.awaiting_restore = true; state.async_restore = std::move(restore); }
        else if (state.recovery_only) state.staged.reset();
        return core::Result<void>::success();
    }, now);
}
core::Result<void> GalacticCheckpointTransfer::pump(MessageTransport& transport, std::uint64_t now) {
    auto& state = *impl_;
    if (!state.selected) return core::Result<void>::success();
    if (state.view.refusal) return core::Result<void>::failure(*state.view.refusal);
    if (now < state.last_now) return state.refuse(transport, "clock_went_backwards");
    state.last_now = now;
    if (state.view.agreed || (transport.self() != 0 && state.view.restored)) return core::Result<void>::success();
    ++state.view.pumps;
    // Budget aggregate host egress at 20 Mbit/s plus two minutes for setup and restore.
    // The separate idle deadline still refuses a silent peer promptly.
    const auto receivers = std::max<std::uint64_t>(1U, state.selected->peers.size() - 1U);
    const auto payload_ms = (state.selected->byte_length * receivers * 8U + 19'999U) / 20'000U;
    const auto whole_deadline = std::max<std::uint64_t>(600'000U, payload_ms + 120'000U);
    if (now - state.started >= whole_deadline) return state.refuse(transport, "whole_transfer_deadline");
    if (transport.self() != 0) {
        if (state.awaiting_restore) {
            auto restored = state.async_restore(*state.selected, state.staged);
            if (!restored) return state.refuse(transport, restored.error().code + ": " + restored.error().message);
            if (restored.value()) {
                state.view.restored = true; state.view.restored_ms = now; state.awaiting_restore = false; state.note_restore_wall();
                state.emit("restore-end", state.at(now) + " async=yes restore_wall_ms=" + std::to_string(state.view.restore_wall_ms)
                    + " pumps=" + std::to_string(state.view.pumps));
                state.async_restore = {}; if (state.recovery_only) state.staged.reset();
                if (auto sent = transport.send(0, restore_proof(*state.selected)); !sent)
                    return state.refuse(transport, "restore_ack_send_failed");
                return core::Result<void>::success();
            }
        }
        // A running restore is not idleness: a real save takes longer than the 30 s silence limit (the whole-transfer
        // deadline above still bounds it).
        if (!state.awaiting_restore && now - state.local_progress >= 30'000) return state.refuse(transport, "idle_progress_deadline");
        return core::Result<void>::success();
    }
    if (!state.published) {
        auto encoded = encode_setup_offer(state.selected->offer);
        if (!encoded || encoded.value().size() > max_setup_bytes)
            return state.refuse(transport, "manifest_budget");
        auto frame = transfer_frame(save_manifest_kind, state.view.epoch);
        word(frame, state.view.total, 8); word(frame, state.selected->peers.size(), 4);
        for (const auto peer : state.selected->peers) word(frame, peer, 4);
        blob(frame, encoded.value());
        if (state.selected->recovery) {
            const auto& identity = *state.selected->recovery;
            word(frame, 1, 1); word(frame, identity.initial_tick, 8);
            word(frame, identity.source.value, 4); word(frame, identity.target.value, 4);
            for (const auto& digest : identity.proof) frame.insert(frame.end(), digest.begin(), digest.end());
        } else {
            word(frame, save_display_metadata_version, 1);
            text(frame, state.selected->description); text(frame, state.selected->date);
        }
        for (const auto peer : state.selected->peers) if (peer != 0)
            if (auto sent = transport.send(peer, frame); !sent) return state.refuse(transport, "manifest_send_failed");
        state.published = true;
        state.emit("manifest-sent", state.at(now) + " peers=" + std::to_string(state.selected->peers.size() - 1));
    }
    for (const auto peer : state.selected->peers) {
        if (peer == 0 || state.restored[peer]) continue;
        if (transport.link(peer) == LinkState::closed) return state.refuse(transport, "peer_disconnected");
        // A peer that has every chunk and is restoring is working, not silent; the whole-transfer deadline bounds it.
        const bool restoring = state.acked[peer] == state.view.total;
        if (!restoring && now - state.progress[peer] >= 30'000) return state.refuse(transport, "idle_progress_deadline");
        // One bounded window per peer; no checkpoint copy enters send queues in full.
        if (auto window = state.send_window(transport, peer, now); !window) return window;
    }
    return core::Result<void>::success();
}
} // namespace eawr::net
