#include "galactic_ai_lane_inbox.hpp"
#include "eawr/sim/galactic/events.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace eawr::campaign {
namespace {
namespace ga = script::foc::galactic_ai;
namespace gc = sim::galactic;
using Change = gc::GalacticSession::NativeActorChange;

// A block written after the saved outbox; the tag tells it from trailing garbage.
constexpr std::string_view block_tag = "eawr-gcai-lane-inbox";
constexpr std::uint64_t block_version = 2; // v2 (#6378): actor changes carry the AI-usage writes
constexpr std::uint64_t max_entries = std::uint64_t{1} << 20;
enum class Kind : std::uint64_t { production_outcome = 1, movement_event = 2, actor_changes = 3, battle = 4, credit = 5, bill = 6 };

template<class T> core::Result<T> fail(std::string message) {
    core::Diagnostic error; error.code = "EAWR-GCAI-ASYNC-0002";
    error.message = "galactic AI inbox state: " + std::move(message);
    return core::Result<T>::failure(std::move(error));
}

void put(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
}
void put_optional(std::vector<std::uint8_t>& bytes, const std::optional<bool>& value) { put(bytes, value ? (*value ? 2 : 1) : 0); }

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    bool bad{};
    std::uint64_t word() {
        if (bad || bytes.size() - at < 8) { bad = true; return 0; }
        std::uint64_t value{};
        for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(bytes[at + i]) << (i * 8U);
        at += 8; return value;
    }
    bool flag() { const auto value = word(); if (value > 1) bad = true; return value == 1; }
    std::optional<bool> optional_flag() {
        const auto value = word(); if (value > 2) bad = true;
        return value == 0 ? std::nullopt : std::optional<bool>(value == 2);
    }
    // A count whose rows take at least `row_bytes` each: bounded by what is left.
    std::uint64_t count(std::size_t row_bytes) {
        const auto value = word();
        if (bad || value > (bytes.size() - at) / row_bytes) { bad = true; return 0; }
        return value;
    }
    template<class T> T narrow(std::uint64_t value) {
        if (value > std::numeric_limits<T>::max()) bad = true;
        return static_cast<T>(value);
    }
};

void encode_battle(std::vector<std::uint8_t>& bytes, const GalacticAiBattleFacts& facts) {
    put(bytes, static_cast<std::uint64_t>(facts.kind)); put(bytes, facts.synchronization_tick); put(bytes, facts.auto_resolve);
    put(bytes, facts.conflict.id); put(bytes, static_cast<std::uint64_t>(facts.conflict.layer)); put(bytes, facts.conflict.planet);
    put(bytes, facts.conflict.forces.size());
    for (const auto& force : facts.conflict.forces) { put(bytes, force.id); put(bytes, force.owner); put(bytes, force.parent); put(bytes, force.carrier); }
    put(bytes, facts.result ? 1 : 0);
    if (facts.result) {
        const auto& result = *facts.result;
        put(bytes, result.conflict); put(bytes, result.winner);
        put(bytes, result.retreat_player ? 1 : 0); put(bytes, result.retreat_player.value_or(0));
        put(bytes, result.forces.size());
        for (const auto& force : result.forces) { put(bytes, force.id); put(bytes, static_cast<std::uint64_t>(force.disposition)); }
    }
    put(bytes, facts.fleets.size());
    for (const auto fleet : facts.fleets) put(bytes, fleet);
    put(bytes, facts.orbiting.size());
    for (const auto fleet : facts.orbiting) put(bytes, fleet);
}

std::optional<GalacticAiBattleFacts> decode_battle(Reader& in) {
    GalacticAiBattleFacts facts;
    const auto kind = in.word();
    if (kind > static_cast<std::uint64_t>(GalacticAiBattleKind::returned)) in.bad = true;
    facts.kind = static_cast<GalacticAiBattleKind>(kind);
    facts.synchronization_tick = in.word(); facts.auto_resolve = in.flag();
    facts.conflict.id = in.word();
    const auto layer = in.word();
    if (layer > static_cast<std::uint64_t>(BattleLayer::land)) in.bad = true;
    facts.conflict.layer = static_cast<BattleLayer>(layer);
    facts.conflict.planet = in.word();
    const auto forces = in.count(32);
    for (std::uint64_t i = 0; i < forces && !in.bad; ++i) {
        ConflictForce force; force.id = in.word(); force.owner = in.narrow<gc::PlayerId>(in.word());
        force.parent = in.word(); force.carrier = in.word();
        facts.conflict.forces.push_back(force);
    }
    if (in.flag()) {
        BattleResult result; result.conflict = in.word(); result.winner = in.narrow<gc::PlayerId>(in.word());
        const bool retreat = in.flag(); const auto player = in.narrow<gc::PlayerId>(in.word());
        if (retreat) result.retreat_player = player; else if (player) in.bad = true;
        const auto rows = in.count(16);
        for (std::uint64_t i = 0; i < rows && !in.bad; ++i) {
            ForceResult row; row.id = in.word();
            const auto disposition = in.word();
            if (disposition > static_cast<std::uint64_t>(ForceDisposition::destroy_after_tactical)) in.bad = true;
            row.disposition = static_cast<ForceDisposition>(disposition);
            result.forces.push_back(row);
        }
        facts.result = std::move(result);
    }
    const auto fleets = in.count(8);
    for (std::uint64_t i = 0; i < fleets && !in.bad; ++i) {
        const auto fleet = in.word();
        if (!facts.fleets.empty() && fleet <= facts.fleets.back()) in.bad = true;
        facts.fleets.push_back(fleet);
    }
    const auto orbiting = in.count(8);
    for (std::uint64_t i = 0; i < orbiting && !in.bad; ++i) {
        const auto fleet = in.word();
        if (!facts.orbiting.empty() && fleet <= facts.orbiting.back()) in.bad = true;
        facts.orbiting.push_back(fleet);
    }
    if (in.bad) return std::nullopt;
    return facts;
}

std::uint64_t raw(sim::math::Fixed value) { return static_cast<std::uint64_t>(value.raw()); }
sim::math::Fixed fixed(std::uint64_t value) { return sim::math::Fixed::from_raw(static_cast<std::int64_t>(value)); }
void encode_credit(std::vector<std::uint8_t>& bytes, const gc::GalacticSession::CreditNotification& credit) {
    put(bytes, static_cast<std::uint64_t>(credit.kind)); put(bytes, credit.player);
    put(bytes, raw(credit.credits)); put(bytes, raw(credit.amount)); put(bytes, raw(credit.seconds));
}
gc::GalacticSession::CreditNotification decode_credit(Reader& in) {
    gc::GalacticSession::CreditNotification credit;
    const auto kind = in.word();
    if (kind > static_cast<std::uint64_t>(gc::GalacticSession::CreditNotification::Kind::windfall)) in.bad = true;
    credit.kind = static_cast<gc::GalacticSession::CreditNotification::Kind>(kind);
    credit.player = in.narrow<gc::PlayerId>(in.word());
    credit.credits = fixed(in.word()); credit.amount = fixed(in.word()); credit.seconds = fixed(in.word());
    return credit;
}
void encode_bill(std::vector<std::uint8_t>& bytes, const GalacticAiRecordedBill& bill) {
    put(bytes, bill.build); put(bytes, static_cast<std::uint64_t>(bill.effect.kind)); put(bytes, bill.effect.payer);
    put(bytes, bill.effect.goal); put(bytes, raw(bill.effect.amount)); put(bytes, raw(bill.effect.remaining));
}
GalacticAiRecordedBill decode_bill(Reader& in) {
    GalacticAiRecordedBill bill;
    bill.build = in.word();
    const auto kind = in.word();
    if (kind > static_cast<std::uint64_t>(ga::BillEffect::Kind::spend)) in.bad = true;
    bill.effect.kind = static_cast<ga::BillEffect::Kind>(kind);
    bill.effect.payer = in.narrow<gc::PlayerId>(in.word()); bill.effect.goal = in.word();
    bill.effect.amount = fixed(in.word()); bill.effect.remaining = fixed(in.word());
    return bill;
}
} // namespace

void GalacticAiLaneInbox::encode(std::vector<std::uint8_t>& bytes) const {
    if (entries_.empty()) return;
    put(bytes, block_tag.size()); bytes.insert(bytes.end(), block_tag.begin(), block_tag.end());
    put(bytes, block_version); put(bytes, entries_.size());
    for (const auto& entry : entries_) {
        if (const auto* battle = std::get_if<GalacticAiBattleFacts>(&entry)) {
            put(bytes, static_cast<std::uint64_t>(Kind::battle)); encode_battle(bytes, *battle); continue;
        }
        if (const auto* credit = std::get_if<gc::GalacticSession::CreditNotification>(&entry)) {
            put(bytes, static_cast<std::uint64_t>(Kind::credit)); encode_credit(bytes, *credit); continue;
        }
        if (const auto* bill = std::get_if<GalacticAiRecordedBill>(&entry)) {
            put(bytes, static_cast<std::uint64_t>(Kind::bill)); encode_bill(bytes, *bill); continue;
        }
        const auto& delivery = std::get<ga::NativeActions::Delivery>(entry);
        if (const auto* outcome = std::get_if<gc::GalacticSession::NativeProductionOutcome>(&delivery)) {
            put(bytes, static_cast<std::uint64_t>(Kind::production_outcome));
            put(bytes, outcome->player); put(bytes, outcome->build); put(bytes, outcome->object); put(bytes, outcome->completed);
        } else if (const auto* event = std::get_if<gc::GalacticEvent>(&delivery)) {
            put(bytes, static_cast<std::uint64_t>(Kind::movement_event));
            put(bytes, event->tick); put(bytes, static_cast<std::uint64_t>(event->kind)); put(bytes, event->producer.value);
            put(bytes, event->subject); put(bytes, event->sequence); put(bytes, event->value); put(bytes, static_cast<std::uint64_t>(event->reason));
        } else {
            const auto& changes = std::get<ga::NativeActions::ActorChangesDelivery>(delivery);
            put(bytes, static_cast<std::uint64_t>(Kind::actor_changes));
            put(bytes, changes.changes.size());
            for (const auto& change : changes.changes) {
                put(bytes, static_cast<std::uint64_t>(change.kind)); put(bytes, change.actor); put(bytes, change.source);
                put_optional(bytes, change.dead); put_optional(bytes, change.deleting); put_optional(bytes, change.death_clone);
                put_optional(bytes, change.prevent_ai_usage); put_optional(bytes, change.prefer_ground);
            }
            put(bytes, changes.facts.size());
            for (const auto& [actor, facts] : changes.facts) ga::encode_freestore_facts(bytes, actor, facts);
        }
    }
}

core::Result<std::size_t> GalacticAiLaneInbox::restore_prefix(std::span<const std::uint8_t> bytes) {
    std::vector<Entry> staged;
    std::size_t used{};
    {
        Reader probe{bytes};
        const bool ours = probe.word() == block_tag.size() && !probe.bad && bytes.size() - probe.at >= block_tag.size()
            && std::equal(block_tag.begin(), block_tag.end(), bytes.begin() + static_cast<std::ptrdiff_t>(probe.at));
        if (!ours) { entries_.clear(); ++generation_; return core::Result<std::size_t>::success(0); }
    }
    {
        Reader in{bytes};
        in.word();
        in.at += block_tag.size();
        if (in.word() != block_version) return fail<std::size_t>("unsupported version");
        const auto count = in.word();
        if (in.bad || !count || count > max_entries) return fail<std::size_t>("entry count is invalid");
        for (std::uint64_t i = 0; i < count && !in.bad; ++i) {
            const auto kind = in.word();
            if (kind == static_cast<std::uint64_t>(Kind::battle)) {
                auto battle = decode_battle(in);
                if (!battle) return fail<std::size_t>("battle entry is invalid");
                staged.emplace_back(std::move(*battle));
            } else if (kind == static_cast<std::uint64_t>(Kind::credit)) {
                staged.emplace_back(decode_credit(in));
            } else if (kind == static_cast<std::uint64_t>(Kind::bill)) {
                staged.emplace_back(decode_bill(in));
            } else if (kind == static_cast<std::uint64_t>(Kind::production_outcome)) {
                gc::GalacticSession::NativeProductionOutcome outcome;
                outcome.player = in.narrow<gc::PlayerId>(in.word()); outcome.build = in.word(); outcome.object = in.word();
                outcome.completed = in.flag();
                staged.emplace_back(ga::NativeActions::Delivery{outcome});
            } else if (kind == static_cast<std::uint64_t>(Kind::movement_event)) {
                gc::GalacticEvent event;
                event.tick = in.word();
                const auto event_kind = in.word();
                event.kind = static_cast<gc::GalacticEventKind>(in.narrow<std::uint8_t>(event_kind));
                event.producer = gc::SeatId{in.narrow<decltype(gc::SeatId{}.value)>(in.word())};
                event.subject = in.word(); event.sequence = in.word(); event.value = in.word();
                event.reason = static_cast<gc::GalacticRejectReason>(in.narrow<std::uint8_t>(in.word()));
                // Only values the enums name (they have gaps): the sim names every member and nothing else.
                if (gc::to_string(event.kind) == "unknown" || gc::to_string(event.reason) == "unknown") in.bad = true;
                staged.emplace_back(ga::NativeActions::Delivery{event});
            } else if (kind == static_cast<std::uint64_t>(Kind::actor_changes)) {
                ga::NativeActions::ActorChangesDelivery changes;
                const auto rows = in.count(64);
                for (std::uint64_t row = 0; row < rows && !in.bad; ++row) {
                    Change change;
                    const auto change_kind = in.word();
                    if (change_kind > static_cast<std::uint64_t>(Change::Kind::lost)) in.bad = true;
                    change.kind = static_cast<Change::Kind>(change_kind);
                    change.actor = in.word(); change.source = in.word();
                    change.dead = in.optional_flag(); change.deleting = in.optional_flag(); change.death_clone = in.optional_flag();
                    change.prevent_ai_usage = in.optional_flag(); change.prefer_ground = in.optional_flag();
                    changes.changes.push_back(change);
                }
                const auto facts = in.count(177);
                for (std::uint64_t row = 0; row < facts && !in.bad; ++row) {
                    ga::FreestoreFacts value;
                    if (!ga::decode_freestore_facts(bytes, in.at, value)) { in.bad = true; break; }
                    if (!changes.facts.empty() && value.object.id <= changes.facts.back().first) in.bad = true;
                    changes.facts.emplace_back(value.object.id, value);
                }
                staged.emplace_back(ga::NativeActions::Delivery{std::move(changes)});
            } else return fail<std::size_t>("unknown entry kind");
        }
        if (in.bad) return fail<std::size_t>("entry framing is invalid");
        used = in.at;
    }
    entries_ = std::move(staged); ++generation_;
    return core::Result<std::size_t>::success(used);
}
core::Result<void> GalacticAiLaneInbox::restore(std::span<const std::uint8_t> bytes) {
    // A whole-span restore refuses a foreign block before touching the queue.
    if (!bytes.empty()) {
        Reader probe{bytes};
        if (probe.word() != block_tag.size() || probe.bad || bytes.size() - probe.at < block_tag.size()
            || !std::equal(block_tag.begin(), block_tag.end(), bytes.begin() + static_cast<std::ptrdiff_t>(probe.at)))
            return fail<void>("unknown trailing block");
    }
    auto used = restore_prefix(bytes);
    if (!used) return core::Result<void>::failure(used.error());
    if (used.value() != bytes.size()) return fail<void>("unknown trailing block");
    return core::Result<void>::success();
}

namespace {
constexpr std::string_view view_tag = "eawr-gcai-billing-view";
constexpr std::uint64_t view_version = 4; // v2 (W1 S4): AI digests; v3 (W1 S6): story-plot commands, diagnostics, load notes; v4 (W1 S5): AI global writes
constexpr std::string_view globals_tag = "eawr-gcai-story-globals";
constexpr std::uint64_t globals_version = 1;
void put_scalar(std::vector<std::uint8_t>& bytes, const GalacticStoryGlobals::Scalar& scalar) {
    const auto text = [&](std::string_view value) { put(bytes, value.size()); bytes.insert(bytes.end(), value.begin(), value.end()); };
    if (const auto* number = std::get_if<script::numeric::LuaNumber>(&scalar)) { put(bytes, 0); put(bytes, number->repr); }
    else if (const auto* value = std::get_if<std::string>(&scalar)) { put(bytes, 1); text(*value); }
    else { put(bytes, 2); put(bytes, std::get<bool>(scalar) ? 1U : 0U); }
}
std::string read_text(Reader& in, std::size_t limit) {
    const auto size = in.count(1);
    if (in.bad || size > limit) { in.bad = true; return {}; }
    std::string value(reinterpret_cast<const char*>(in.bytes.data()) + in.at, static_cast<std::size_t>(size));
    in.at += static_cast<std::size_t>(size);
    return value;
}
GalacticStoryGlobals::Scalar read_scalar(Reader& in) {
    const auto kind = in.word();
    if (kind == 0) { script::numeric::LuaNumber number; number.repr = in.word(); return number; }
    if (kind == 1) return read_text(in, ga::native_state_max_text_bytes);
    if (kind != 2) in.bad = true;
    const auto flag = in.word();
    if (flag > 1) in.bad = true;
    return flag == 1;
}
void put_global_writes(std::vector<std::uint8_t>& bytes, const std::vector<GalacticStoryGlobals::Write>& writes) {
    put(bytes, writes.size());
    for (const auto& [key, value] : writes) {
        put(bytes, key.size()); bytes.insert(bytes.end(), key.begin(), key.end());
        put(bytes, value ? 1U : 0U);
        if (value) put_scalar(bytes, *value);
    }
}
bool read_global_writes(Reader& in, std::vector<GalacticStoryGlobals::Write>& writes) {
    const auto count = in.count(24);
    for (std::uint64_t i = 0; i < count && !in.bad; ++i) {
        auto key = read_text(in, ga::native_state_max_text_bytes);
        const auto present = in.word();
        if (present > 1 || key.empty()) in.bad = true;
        std::optional<GalacticStoryGlobals::Scalar> value;
        if (present == 1) value = read_scalar(in);
        writes.emplace_back(std::move(key), std::move(value));
    }
    return !in.bad;
}
template<class T> core::Result<T> view_fail(std::string message) {
    core::Diagnostic error; error.code = "EAWR-GCAI-ASYNC-0003";
    error.message = "galactic AI billing view: " + std::move(message);
    return core::Result<T>::failure(std::move(error));
}
core::Result<void> encode_publication(std::vector<std::uint8_t>& bytes, const GalacticAiBillingPublication& publication) {
    put(bytes, publication.visible_at); put(bytes, publication.log_cut);
    ga::FundingState state;
    for (const auto& [player, ledger] : publication.ledgers) state.emplace(player, ledger);
    auto encoded = ga::encode_funding_state(state);
    if (!encoded) return core::Result<void>::failure(encoded.error());
    put(bytes, encoded.value().size()); bytes.insert(bytes.end(), encoded.value().begin(), encoded.value().end());
    put(bytes, publication.rows.size());
    for (const auto& [build, row] : publication.rows) {
        put(bytes, build); put(bytes, row.player); put(bytes, row.goal); put(bytes, raw(row.price)); put(bytes, raw(row.billed));
        put(bytes, raw(row.marked)); put(bytes, raw(row.remaining)); put(bytes, row.orphan);
    }
    const auto text = [&](std::string_view value) { put(bytes, value.size()); bytes.insert(bytes.end(), value.begin(), value.end()); };
    put(bytes, publication.ai_digests.size());
    for (const auto& [name, digest] : publication.ai_digests) { text(name); text(digest); }
    text(publication.scheduler_digest);
    put(bytes, publication.plot_commands.size());
    for (const auto& plot : publication.plot_commands) {
        put(bytes, static_cast<std::uint64_t>(plot.op)); text(plot.subplot); text(plot.event); put(bytes, plot.index);
        put(bytes, plot.values.size());
        for (const auto& value : plot.values) text(value);
    }
    put(bytes, publication.diagnostics.size());
    for (const auto& line : publication.diagnostics) text(line);
    put(bytes, publication.load_notes.size());
    for (const auto& note : publication.load_notes) {
        text(note.code); put(bytes, static_cast<std::uint64_t>(note.classification));
        text(note.layer.mod_id); put(bytes, note.layer.chain_position ? 1 : 0); put(bytes, note.layer.chain_position.value_or(0));
        text(note.file); put(bytes, note.line ? 1 : 0); put(bytes, note.line.value_or(0));
        text(note.object); text(note.tag); text(note.authored_value); put(bytes, static_cast<std::uint64_t>(note.action));
        text(note.effective_value); text(note.retail); put(bytes, static_cast<std::uint64_t>(note.phase));
        text(note.diagnostic_code); text(note.message);
    }
    put_global_writes(bytes, publication.global_writes);
    return core::Result<void>::success();
}
std::optional<GalacticAiBillingPublication> decode_publication(Reader& in) {
    GalacticAiBillingPublication publication;
    publication.visible_at = in.word(); publication.log_cut = in.word();
    const auto size = in.count(1);
    if (in.bad) return std::nullopt;
    auto state = ga::decode_funding_state(in.bytes.subspan(in.at, static_cast<std::size_t>(size)));
    if (!state) return std::nullopt;
    in.at += static_cast<std::size_t>(size);
    for (const auto& [player, ledger] : state.value()) publication.ledgers.emplace(static_cast<gc::PlayerId>(player), ledger);
    const auto rows = in.count(64);
    for (std::uint64_t i = 0; i < rows && !in.bad; ++i) {
        const auto build = in.word();
        ga::BillRow row; row.player = in.narrow<gc::PlayerId>(in.word()); row.goal = in.word(); row.price = fixed(in.word());
        row.billed = fixed(in.word()); row.marked = fixed(in.word()); row.remaining = fixed(in.word()); row.orphan = in.flag();
        if (!publication.rows.empty() && build <= publication.rows.rbegin()->first) in.bad = true;
        publication.rows.emplace_hint(publication.rows.end(), build, row);
    }
    // Fixed-format texts (component names, hex digests) keep a bound; the texts a script supplies (plot names and values, notes)
    // are bounded only by the block, as on write, where a refusal would turn a long script string into a failed save or lane hash.
    constexpr auto any = std::numeric_limits<std::size_t>::max();
    const auto text = [&](std::size_t limit) {
        const auto size = in.count(1);
        if (in.bad || size > limit) { in.bad = true; return std::string{}; }
        std::string value(reinterpret_cast<const char*>(in.bytes.data()) + in.at, static_cast<std::size_t>(size));
        in.at += static_cast<std::size_t>(size);
        return value;
    };
    const auto digests = in.count(16);
    for (std::uint64_t i = 0; i < digests && !in.bad; ++i) {
        auto name = text(256);
        auto digest = text(64);
        if (name.empty() || digest.size() != 64 || publication.ai_digests.contains(name)) in.bad = true;
        if (!publication.ai_digests.empty() && name <= publication.ai_digests.rbegin()->first) in.bad = true;
        publication.ai_digests.emplace(std::move(name), std::move(digest));
    }
    publication.scheduler_digest = text(64);
    const auto plots = in.count(32);
    for (std::uint64_t i = 0; i < plots && !in.bad; ++i) {
        sim::galactic::story::PlotCommand plot;
        const auto op = in.word();
        if (op > static_cast<std::uint64_t>(sim::galactic::story::PlotCommand::Op::clear_dialog_text)) in.bad = true;
        plot.op = static_cast<sim::galactic::story::PlotCommand::Op>(op);
        plot.subplot = text(any); plot.event = text(any);
        const auto index = in.word();
        if (index > std::numeric_limits<std::uint32_t>::max()) in.bad = true;
        plot.index = static_cast<std::uint32_t>(index);
        const auto values = in.count(8);
        for (std::uint64_t v = 0; v < values && !in.bad; ++v) plot.values.push_back(text(any));
        publication.plot_commands.push_back(std::move(plot));
    }
    const auto lines = in.count(8);
    for (std::uint64_t i = 0; i < lines && !in.bad; ++i) publication.diagnostics.push_back(text(any));
    const auto notes = in.count(8 * 15);
    for (std::uint64_t i = 0; i < notes && !in.bad; ++i) {
        core::LoadNote note;
        note.code = text(any);
        const auto classification = in.word();
        if (classification > static_cast<std::uint64_t>(core::LoadCategory::unknown_degrade)) in.bad = true;
        note.classification = static_cast<core::LoadCategory>(classification);
        note.layer.mod_id = text(any);
        const bool chained = in.flag();
        const auto position = in.word();
        if (!chained && position != 0) in.bad = true;
        note.layer.chain_position = chained ? std::optional<std::uint64_t>(position) : std::nullopt;
        note.file = text(any);
        const bool lined = in.flag();
        const auto line = in.word();
        if (!lined && line != 0) in.bad = true;
        note.line = lined ? std::optional<std::uint64_t>(line) : std::nullopt;
        note.object = text(any); note.tag = text(any); note.authored_value = text(any);
        const auto action = in.word();
        if (action > static_cast<std::uint64_t>(core::LoadAction::runtime_degraded)) in.bad = true;
        note.action = static_cast<core::LoadAction>(action);
        note.effective_value = text(any); note.retail = text(any);
        const auto phase = in.word();
        if (phase > static_cast<std::uint64_t>(core::LoadPhase::runtime)) in.bad = true;
        note.phase = static_cast<core::LoadPhase>(phase);
        note.diagnostic_code = text(any); note.message = text(any);
        publication.load_notes.push_back(std::move(note));
    }
    if (!read_global_writes(in, publication.global_writes)) return std::nullopt;
    if (in.bad) return std::nullopt;
    return publication;
}
} // namespace

std::optional<core::Result<ga::BillDecision>> GalacticAiBillingView::bill(const Notice& notice,
    std::span<const gc::ProductionPlayer> players) {
    const auto row = rows_.find(notice.build);
    if (!base_ || row == rows_.end() || row->second.player != notice.player) return std::nullopt;
    const ga::BillLedgers lookup = [this](gc::PlayerId player) -> const gc::funding::Ledger* {
        const auto found = ledgers_.find(player);
        return found == ledgers_.end() ? nullptr : &found->second;
    };
    auto decided = ga::decide_bill(row->second, notice, lookup, players);
    if (!decided) return decided;
    if (decided.value().effect.kind != ga::BillEffect::Kind::none) {
        LogEntry entry{next_sequence_, GalacticAiRecordedBill{notice.build, decided.value().effect}};
        if (auto applied = replay(entry, ledgers_, rows_); !applied) return core::Result<ga::BillDecision>::failure(applied.error());
        log_.push_back(std::move(entry)); ++next_sequence_; ++generation_;
    }
    return decided;
}
core::Result<void> GalacticAiBillingView::credit(const Credit& notice) {
    LogEntry entry{next_sequence_, notice};
    if (auto applied = replay(entry, ledgers_, rows_); !applied) return applied;
    log_.push_back(std::move(entry)); ++next_sequence_; ++generation_;
    return core::Result<void>::success();
}
core::Result<void> GalacticAiBillingView::replay(const LogEntry& entry, Ledgers& budgets, Rows& billing_rows) {
    if (const auto* income = std::get_if<Credit>(&entry.entry)) {
        const auto found = budgets.find(income->player);
        if (found == budgets.end()) return core::Result<void>::success(); // not initialized yet: the AI ignores it too
        auto next = income->kind == Credit::Kind::windfall ? gc::funding::windfall(found->second, income->amount)
            : found->second.tactical.switched ? view_fail<gc::funding::Ledger>("parent fiscal service during a tactical credit switch")
            : gc::funding::fiscal(found->second, income->credits, income->amount, income->seconds);
        if (!next) return core::Result<void>::failure(next.error());
        found->second = std::move(next).value();
        return core::Result<void>::success();
    }
    const auto& recorded = std::get<GalacticAiRecordedBill>(entry.entry);
    const auto row = billing_rows.find(recorded.build);
    if (row == billing_rows.end()) return view_fail<void>("a logged bill has no purchase row");
    const auto owner = recorded.effect.kind == ga::BillEffect::Kind::mark ? row->second.player : recorded.effect.payer;
    const auto budget = budgets.find(owner);
    if (budget == budgets.end()) return view_fail<void>("a logged bill has no ledger");
    if (recorded.effect.kind == ga::BillEffect::Kind::mark && !ga::bill_mark_applies(budget->second, recorded.effect))
        return core::Result<void>::success();
    auto next = ga::apply_bill_ledger(budget->second, recorded.effect);
    if (!next) return core::Result<void>::failure(next.error());
    budget->second = std::move(next).value();
    ga::apply_bill_row(row->second, recorded.effect);
    return core::Result<void>::success();
}
void GalacticAiBillingView::capture(GalacticAiBillingPublication publication) {
    pending_.push_back(std::move(publication)); ++generation_;
}
core::Result<void> GalacticAiBillingView::publish_due(std::uint64_t ai_tick, GalacticAiDueOutput* output) {
    std::size_t due{};
    while (due < pending_.size() && pending_[due].visible_at <= ai_tick) ++due;
    if (!due) return core::Result<void>::success();
    for (std::size_t i = 0; i < due; ++i) {
        auto& publication = pending_[i];
        if (output) {
            output->plot_commands.insert(output->plot_commands.end(), std::make_move_iterator(publication.plot_commands.begin()),
                std::make_move_iterator(publication.plot_commands.end()));
            output->diagnostics.insert(output->diagnostics.end(), std::make_move_iterator(publication.diagnostics.begin()),
                std::make_move_iterator(publication.diagnostics.end()));
            output->load_notes.insert(output->load_notes.end(), std::make_move_iterator(publication.load_notes.begin()),
                std::make_move_iterator(publication.load_notes.end()));
            output->global_writes.insert(output->global_writes.end(), std::make_move_iterator(publication.global_writes.begin()),
                std::make_move_iterator(publication.global_writes.end()));
        }
        // Handed to the world once.
        publication.plot_commands.clear(); publication.diagnostics.clear(); publication.load_notes.clear();
        publication.global_writes.clear();
    }
    base_ = std::move(pending_[due - 1]);
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(due));
    std::erase_if(log_, [cut = base_->log_cut](const LogEntry& entry) { return entry.sequence <= cut; });
    ++generation_;
    return rebuild();
}
core::Result<void> GalacticAiBillingView::rebuild() {
    ledgers_ = base_ ? base_->ledgers : Ledgers{}; rows_ = base_ ? base_->rows : Rows{};
    for (const auto& entry : log_) if (auto applied = replay(entry, ledgers_, rows_); !applied) return applied;
    return core::Result<void>::success();
}
core::Result<std::pair<GalacticAiBillingView::Ledgers, GalacticAiBillingView::Rows>> GalacticAiBillingView::latest_replayed() const {
    using Result = core::Result<std::pair<Ledgers, Rows>>;
    const auto* latest = !pending_.empty() ? &pending_.back() : base_ ? &*base_ : nullptr;
    if (!latest) return view_fail<std::pair<Ledgers, Rows>>("no publication");
    auto budgets = latest->ledgers; auto billing_rows = latest->rows;
    for (const auto& entry : log_) {
        if (entry.sequence <= latest->log_cut) continue;
        if (auto applied = replay(entry, budgets, billing_rows); !applied) return Result::failure(applied.error());
    }
    return Result::success({std::move(budgets), std::move(billing_rows)});
}
core::Result<void> GalacticAiBillingView::encode(std::vector<std::uint8_t>& bytes) const {
    if (!base_) return core::Result<void>::success();
    put(bytes, view_tag.size()); bytes.insert(bytes.end(), view_tag.begin(), view_tag.end());
    put(bytes, view_version); put(bytes, next_sequence_);
    if (auto saved = encode_publication(bytes, *base_); !saved) return saved;
    put(bytes, pending_.size());
    for (const auto& publication : pending_) if (auto saved = encode_publication(bytes, publication); !saved) return saved;
    put(bytes, log_.size());
    for (const auto& entry : log_) {
        put(bytes, entry.sequence);
        if (const auto* income = std::get_if<Credit>(&entry.entry)) { put(bytes, static_cast<std::uint64_t>(Kind::credit)); encode_credit(bytes, *income); }
        else { put(bytes, static_cast<std::uint64_t>(Kind::bill)); encode_bill(bytes, std::get<GalacticAiRecordedBill>(entry.entry)); }
    }
    return core::Result<void>::success();
}
std::vector<GalacticStoryGlobals::Write> GalacticAiBillingView::pending_global_writes() const {
    std::vector<GalacticStoryGlobals::Write> writes;
    for (const auto& publication : pending_) writes.insert(writes.end(), publication.global_writes.begin(), publication.global_writes.end());
    return writes;
}
core::Result<void> GalacticAiBillingView::restore(std::span<const std::uint8_t> bytes) {
    auto used = restore_prefix(bytes);
    if (!used) return core::Result<void>::failure(used.error());
    if (used.value() != bytes.size()) return view_fail<void>("unknown trailing block");
    return core::Result<void>::success();
}
core::Result<std::size_t> GalacticAiBillingView::restore_prefix(std::span<const std::uint8_t> bytes) {
    GalacticAiBillingView staged;
    std::size_t used{};
    bool ours{};
    if (!bytes.empty()) {
        Reader probe{bytes};
        ours = probe.word() == view_tag.size() && !probe.bad && bytes.size() - probe.at >= view_tag.size()
            && std::equal(view_tag.begin(), view_tag.end(), bytes.begin() + static_cast<std::ptrdiff_t>(probe.at));
    }
    if (ours) {
        Reader in{bytes};
        in.word();
        in.at += view_tag.size();
        if (in.word() != view_version) return view_fail<std::size_t>("unsupported version");
        staged.next_sequence_ = in.word();
        auto base = decode_publication(in);
        if (!base || !staged.next_sequence_) return view_fail<std::size_t>("published budget is invalid");
        staged.base_ = std::move(*base);
        const auto pending = in.count(32);
        for (std::uint64_t i = 0; i < pending && !in.bad; ++i) {
            auto publication = decode_publication(in);
            if (!publication) return view_fail<std::size_t>("pending budget is invalid");
            staged.pending_.push_back(std::move(*publication));
        }
        const auto entries = in.count(16);
        for (std::uint64_t i = 0; i < entries && !in.bad; ++i) {
            LogEntry entry; entry.sequence = in.word();
            const auto kind = in.word();
            if (kind == static_cast<std::uint64_t>(Kind::credit)) entry.entry = decode_credit(in);
            else if (kind == static_cast<std::uint64_t>(Kind::bill)) entry.entry = decode_bill(in);
            else in.bad = true;
            if (entry.sequence <= staged.base_->log_cut || entry.sequence >= staged.next_sequence_
                || (!staged.log_.empty() && entry.sequence <= staged.log_.back().sequence)) in.bad = true;
            staged.log_.push_back(std::move(entry));
        }
        if (in.bad) return view_fail<std::size_t>("framing is invalid");
        if (auto rebuilt = staged.rebuild(); !rebuilt) return core::Result<std::size_t>::failure(rebuilt.error());
        used = in.at;
    }
    staged.generation_ = generation_ + 1;
    *this = std::move(staged);
    return core::Result<std::size_t>::success(used);
}

// W1 S5: the story-owned GlobalValue store.
std::string GalacticStoryGlobals::admit(const std::string& key, const std::optional<Scalar>& value) {
    // The AI registry's archive quotas (ai_service.cpp commit_global_values) without the AI's other native rows: the store is its own.
    const auto row_bytes = [](const std::string& name, const Scalar& scalar) {
        const auto payload = std::visit([](const auto& part) -> std::size_t {
            using T = std::decay_t<decltype(part)>;
            if constexpr (std::is_same_v<T, std::string>) return 9 + part.size();
            else if constexpr (std::is_same_v<T, bool>) return 10;
            else return 8;
        }, scalar);
        return 2 * (8 + name.size()) + payload;
    };
    if (value) {
        const auto* text = std::get_if<std::string>(&*value);
        if (key.size() > ga::native_state_max_text_bytes || (text && text->size() > ga::native_state_max_text_bytes))
            return "GlobalValue.Set refused by project archive quotas; registry unchanged (L-55)";
        std::size_t retained{};
        for (const auto& [name, scalar] : values_) if (name != key) retained += row_bytes(name, scalar);
        const auto count = values_.size() + (values_.contains(key) ? 0U : 1U);
        if (count > ga::native_state_max_rows || retained > ga::global_value_max_retained_bytes
            || row_bytes(key, *value) > ga::global_value_max_retained_bytes - retained)
            return "GlobalValue.Set refused by project archive quotas; registry unchanged (L-55)";
        values_.insert_or_assign(key, *value);
    } else values_.erase(key);
    ++generation_;
    return {};
}
void GalacticStoryGlobals::commit(script::authoritative::ServiceReport& report) {
    std::erase_if(report.commands, [&](const script::authoritative::ScriptCommand& command) {
        if (command.verb != script::authoritative::global_value_command) return false;
        const auto& args = command.arguments;
        const auto* key = args.empty() ? nullptr : std::get_if<std::string>(&args[0].data);
        if (!key) return true;
        const auto note = [&](std::string message) {
            core::LoadNote row;
            row.code = "EAWR-LOAD-1400"; row.object = std::to_string(command.issuer);
            row.action = core::LoadAction::runtime_degraded; row.phase = core::LoadPhase::runtime;
            row.diagnostic_code = std::string(script::authoritative::codes::unsupported_api);
            row.message = std::move(message); row.effective_value = "GlobalValue write refused; prior registry preserved";
            report.load_notes.push_back(std::move(row));
        };
        if (args.size() != 2) { note("GlobalValue.Set table ignored; cross-instance graph identity is unimplemented (L-55)"); return true; }
        std::optional<Scalar> value;
        if (const auto* number = std::get_if<script::numeric::LuaNumber>(&args[1].data)) value = *number;
        else if (const auto* text = std::get_if<std::string>(&args[1].data)) value = *text;
        else if (const auto* boolean = std::get_if<bool>(&args[1].data)) value = *boolean;
        else if (!std::holds_alternative<std::monostate>(args[1].data)) {
            note("GlobalValue.Set unsupported value ignored; registry unchanged (L-55)"); return true;
        }
        if (auto refused = admit(*key, value); !refused.empty()) note(std::move(refused));
        return true;
    });
}
void GalacticStoryGlobals::merge(std::span<const Write> writes, std::vector<core::LoadNote>& notes) {
    for (const auto& [key, value] : writes) {
        auto refused = admit(key, value);
        if (refused.empty()) continue;
        core::LoadNote row;
        row.code = "EAWR-LOAD-1400"; row.object = "GlobalValue:" + key;
        row.action = core::LoadAction::runtime_degraded; row.phase = core::LoadPhase::runtime;
        row.diagnostic_code = std::string(script::authoritative::codes::unsupported_api);
        row.message = "AI " + refused; row.effective_value = "GlobalValue write refused; prior registry preserved";
        notes.push_back(std::move(row));
    }
}
void GalacticStoryGlobals::encode(std::vector<std::uint8_t>& bytes) const {
    if (!active_) return;
    put(bytes, globals_tag.size()); bytes.insert(bytes.end(), globals_tag.begin(), globals_tag.end());
    put(bytes, globals_version);
    put(bytes, values_.size());
    for (const auto& [key, scalar] : values_) {
        put(bytes, key.size()); bytes.insert(bytes.end(), key.begin(), key.end());
        put_scalar(bytes, scalar);
    }
}
core::Result<std::size_t> GalacticStoryGlobals::restore_prefix(std::span<const std::uint8_t> bytes) {
    const auto fail_globals = [](std::string message) {
        core::Diagnostic error; error.code = "EAWR-GCAI-ASYNC-0004";
        error.message = "galactic story globals: " + std::move(message);
        return core::Result<std::size_t>::failure(std::move(error));
    };
    bool ours{};
    if (!bytes.empty()) {
        Reader probe{bytes};
        ours = probe.word() == globals_tag.size() && !probe.bad && bytes.size() - probe.at >= globals_tag.size()
            && std::equal(globals_tag.begin(), globals_tag.end(), bytes.begin() + static_cast<std::ptrdiff_t>(probe.at));
    }
    if (!ours) { active_ = false; values_.clear(); ++generation_; return core::Result<std::size_t>::success(0); }
    Reader in{bytes};
    in.word(); in.at += globals_tag.size();
    if (in.word() != globals_version) return fail_globals("unsupported version");
    Values staged;
    const auto count = in.count(24);
    for (std::uint64_t i = 0; i < count && !in.bad; ++i) {
        auto key = read_text(in, ga::native_state_max_text_bytes);
        auto scalar = read_scalar(in);
        if (key.empty() || (!staged.empty() && key <= staged.rbegin()->first)) in.bad = true;
        staged.emplace_hint(staged.end(), std::move(key), std::move(scalar));
    }
    if (in.bad) return fail_globals("framing is invalid");
    active_ = true; values_ = std::move(staged); ++generation_;
    return core::Result<std::size_t>::success(in.at);
}
} // namespace eawr::campaign

