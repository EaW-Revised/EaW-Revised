#include "galactic_ai_outbox_state.hpp"

#include "eawr/sim/galactic/commands.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace eawr::script::foc::galactic_ai {
namespace {
namespace gc = sim::galactic;
using Shadow = gc::GalacticSession::NativeActionShadow;
using sim::math::Fixed;

constexpr std::size_t max_state_bytes = std::size_t{256} << 20;
constexpr std::uint32_t max_slices = 4096;

template<class T> core::Result<T> fail(std::string message) {
    core::Diagnostic error; error.code = "EAWR-GCAI-ASYNC-0001";
    error.message = "galactic async state: " + std::move(message);
    return core::Result<T>::failure(std::move(error));
}

struct Writer {
    std::vector<std::uint8_t> bytes;
    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u16(std::uint16_t value) { for (unsigned i = 0; i < 2; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8U))); }
    void u32(std::uint32_t value) { for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8U))); }
    void u64(std::uint64_t value) { for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8U))); }
    void flag(bool value) { u8(value ? 1 : 0); }
    void fixed(Fixed value) { u64(static_cast<std::uint64_t>(value.raw())); }
    void blob(std::span<const std::uint8_t> data) { u32(static_cast<std::uint32_t>(data.size())); bytes.insert(bytes.end(), data.begin(), data.end()); }
    void text(const std::string& value) { blob(std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size())); }
    void ids(const std::vector<sim::EntityId>& values) { u32(static_cast<std::uint32_t>(values.size())); for (const auto value : values) u64(value); }
};

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    bool bad{};
    [[nodiscard]] bool failed() const noexcept { return bad; }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes.size() - at; }
    std::uint64_t take(unsigned count) {
        if (bad || remaining() < count) { bad = true; return 0; }
        std::uint64_t value{};
        for (unsigned i = 0; i < count; ++i) value |= static_cast<std::uint64_t>(bytes[at + i]) << (i * 8U);
        at += count; return value;
    }
    std::uint8_t u8() { return static_cast<std::uint8_t>(take(1)); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(take(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(take(4)); }
    std::uint64_t u64() { return take(8); }
    bool flag() { const auto value = u8(); if (value > 1) bad = true; return value == 1; }
    Fixed fixed() { return Fixed::from_raw(static_cast<std::int64_t>(u64())); }
    std::vector<std::uint8_t> blob() {
        const auto size = u32();
        if (bad || remaining() < size) { bad = true; return {}; }
        std::vector<std::uint8_t> out(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size; return out;
    }
    std::string text() { const auto data = blob(); return std::string(data.begin(), data.end()); }
    std::vector<sim::EntityId> ids() {
        const auto count = u32();
        if (bad || remaining() / 8 < count) { bad = true; return {}; }
        std::vector<sim::EntityId> out; out.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) out.push_back(u64());
        return out;
    }
};

void put_value(Writer& out, const authoritative::Value& value, int depth) {
    if (depth > authoritative::max_value_list_depth) throw std::length_error("script value nesting exceeds limit");
    out.u8(static_cast<std::uint8_t>(value.data.index()));
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, bool>) out.flag(item);
        else if constexpr (std::is_same_v<T, numeric::LuaNumber>) out.u64(item.repr);
        else if constexpr (std::is_same_v<T, std::string>) out.text(item);
        else if constexpr (std::is_same_v<T, authoritative::Handle>) { out.u32(item.kind); out.u64(item.id); }
        else if constexpr (std::is_same_v<T, authoritative::ValueList>) {
            out.u32(static_cast<std::uint32_t>(item.size())); for (const auto& child : item) put_value(out, child, depth + 1);
        }
    }, value.data);
}

authoritative::Value get_value(Reader& in, int depth) {
    if (depth > authoritative::max_value_list_depth || in.failed()) { in.bad = true; return {}; }
    switch (in.u8()) {
    case 0: return {};
    case 1: return authoritative::Value{in.flag()};
    case 2: return authoritative::Value::number(numeric::LuaNumber::from_repr(in.u64()));
    case 3: return authoritative::Value::text(in.text());
    case 4: { const auto kind = in.u32(); return authoritative::Value{authoritative::Handle{kind, in.u64()}}; }
    case 5: {
        const auto count = in.u32();
        if (in.failed() || in.remaining() < count) { in.bad = true; return {}; }
        authoritative::ValueList list;
        for (std::uint32_t i = 0; i < count && !in.failed(); ++i) list.push_back(get_value(in, depth + 1));
        return authoritative::Value{std::move(list)};
    }
    default: in.bad = true; return {};
    }
}

void put_production_request(Writer& out, const gc::GalacticSession::NativeProductionRequest& request) {
    out.u32(request.player); out.u64(request.planet); out.u64(request.type);
    out.flag(request.mark_cost.has_value()); if (request.mark_cost) out.fixed(*request.mark_cost);
}
gc::GalacticSession::NativeProductionRequest get_production_request(Reader& in) {
    gc::GalacticSession::NativeProductionRequest request;
    request.player = in.u32(); request.planet = in.u64(); request.type = in.u64();
    if (in.flag()) request.mark_cost = in.fixed();
    return request;
}
void put_fleet_receipt(Writer& out, const gc::GalacticSession::NativeFleetReceipt& receipt) {
    out.flag(receipt.admitted); out.u64(receipt.object); out.u8(static_cast<std::uint8_t>(receipt.reason));
}
gc::GalacticSession::NativeFleetReceipt get_fleet_receipt(Reader& in) {
    gc::GalacticSession::NativeFleetReceipt receipt;
    receipt.admitted = in.flag(); receipt.object = in.u64(); receipt.reason = static_cast<gc::GalacticRejectReason>(in.u8());
    return receipt;
}

enum class Tag : std::uint8_t { fleet = 1, control, cash, force, production, finish, invasion, journal, orbit_recheck };

void put_record(Writer& out, const IntentRecord& record) {
    std::visit([&](const auto& request) {
        using T = std::decay_t<decltype(request)>;
        if constexpr (std::is_same_v<T, Shadow::FleetRequest>) {
            out.u8(static_cast<std::uint8_t>(Tag::fleet)); out.u32(request.player);
            out.u8(static_cast<std::uint8_t>(gc::layer_of(request.payload))); out.u16(gc::opcode_of(request.payload));
            out.blob(gc::encode_payload(request.payload)); out.flag(request.pending); put_fleet_receipt(out, request.receipt);
        } else if constexpr (std::is_same_v<T, Shadow::ControlRequest>) {
            out.u8(static_cast<std::uint8_t>(Tag::control)); out.u8(static_cast<std::uint8_t>(request.kind)); out.u32(request.player);
            out.u64(request.object); out.u64(request.target); out.flag(request.pending);
            out.flag(request.live_landed.has_value()); if (request.live_landed) out.u32(*request.live_landed);
            put_fleet_receipt(out, request.receipt); out.flag(request.linked);
        } else if constexpr (std::is_same_v<T, Shadow::CashRequest>) {
            out.u8(static_cast<std::uint8_t>(Tag::cash)); out.u32(request.player);
            out.fixed(request.delta); out.fixed(request.multiplier); out.fixed(request.actual);
        } else if constexpr (std::is_same_v<T, Shadow::ForceRequest>) {
            out.u8(static_cast<std::uint8_t>(Tag::force));
            if (!request.force) throw std::length_error("a force intent without its request");
            put_production_request(out, request.force->request); out.ids(request.actors);
        } else if constexpr (std::is_same_v<T, Shadow::ProductionRequest>) {
            out.u8(static_cast<std::uint8_t>(Tag::production)); put_production_request(out, request.request);
            out.u8(static_cast<std::uint8_t>(request.receipt.admission)); out.u64(request.receipt.build); out.fixed(request.receipt.price);
        } else if constexpr (std::is_same_v<T, Shadow::FinishRequest>) {
            out.u8(static_cast<std::uint8_t>(Tag::finish)); out.u64(request.build); out.flag(request.salvage); out.fixed(request.multiplier);
        } else if constexpr (std::is_same_v<T, Shadow::InvasionRequest>) {
            out.u8(static_cast<std::uint8_t>(Tag::invasion)); out.flag(request.retire); out.u32(request.player); out.u64(request.fleet);
            out.ids(request.transports); out.flag(request.pending);
            out.flag(request.receipt.admitted); out.u64(request.receipt.request); out.flag(request.receipt.queued);
            out.u8(static_cast<std::uint8_t>(request.receipt.reason)); out.u64(request.number);
        } else if constexpr (std::is_same_v<T, Shadow::JournalRequest>) {
            out.u8(static_cast<std::uint8_t>(Tag::journal)); out.u16(static_cast<std::uint16_t>(request.kind));
            out.blob(request.payload); out.u8(static_cast<std::uint8_t>(request.phase));
        } else {
            static_assert(std::is_same_v<T, Shadow::OrbitRecheckRequest>);
            out.u8(static_cast<std::uint8_t>(Tag::orbit_recheck)); out.u32(request.player); out.u64(request.fleet);
        }
    }, record.request);
    out.u64(record.first_id); out.u64(record.next_id); out.u64(record.first_invasion); out.u64(record.next_invasion);
}

std::optional<IntentRecord> get_record(Reader& in) {
    std::optional<Shadow::Intent> request;
    switch (static_cast<Tag>(in.u8())) {
    case Tag::fleet: {
        const auto player = in.u32();
        const auto layer = static_cast<gc::CommandLayer>(in.u8()); const auto opcode = in.u16();
        auto payload = gc::decode_payload(layer, opcode, in.blob());
        if (in.failed() || !payload) return std::nullopt;
        const auto pending = in.flag();
        const auto receipt = get_fleet_receipt(in);
        request.emplace(Shadow::FleetRequest{player, std::move(*payload), pending, receipt});
        break;
    }
    case Tag::control: {
        Shadow::ControlRequest row;
        const auto kind = in.u8();
        if (kind > static_cast<std::uint8_t>(Shadow::ControlRequest::Kind::actor_land)) return std::nullopt;
        row.kind = static_cast<Shadow::ControlRequest::Kind>(kind); row.player = in.u32();
        row.object = in.u64(); row.target = in.u64(); row.pending = in.flag();
        if (in.flag()) row.live_landed = in.u32();
        row.receipt = get_fleet_receipt(in); row.linked = in.flag();
        request.emplace(std::move(row)); break;
    }
    case Tag::cash: {
        Shadow::CashRequest row;
        row.player = in.u32(); row.delta = in.fixed(); row.multiplier = in.fixed(); row.actual = in.fixed();
        request.emplace(std::move(row)); break;
    }
    case Tag::force: {
        auto production = get_production_request(in);
        auto actors = in.ids();
        // The lane's projected placement is not read by the readmission; it rebuilds it against the world.
        request.emplace(Shadow::ForceRequest{std::make_shared<const Shadow::Force>(Shadow::Force{production, {}}), std::move(actors)});
        break;
    }
    case Tag::production: {
        Shadow::ProductionRequest row;
        row.request = get_production_request(in);
        row.receipt.admission = static_cast<gc::ProductionAdmission>(in.u8()); row.receipt.build = in.u64(); row.receipt.price = in.fixed();
        request.emplace(std::move(row)); break;
    }
    case Tag::finish: {
        Shadow::FinishRequest row;
        row.build = in.u64(); row.salvage = in.flag(); row.multiplier = in.fixed();
        request.emplace(std::move(row)); break;
    }
    case Tag::invasion: {
        Shadow::InvasionRequest row;
        row.retire = in.flag(); row.player = in.u32(); row.fleet = in.u64(); row.transports = in.ids(); row.pending = in.flag();
        row.receipt.admitted = in.flag(); row.receipt.request = in.u64(); row.receipt.queued = in.flag();
        row.receipt.reason = static_cast<gc::GalacticRejectReason>(in.u8()); row.number = in.u64();
        request.emplace(std::move(row)); break;
    }
    case Tag::journal: {
        Shadow::JournalRequest row;
        row.kind = static_cast<gc::NativeEffectKind>(in.u16()); row.payload = in.blob();
        const auto phase = in.u8();
        if (phase > static_cast<std::uint8_t>(gc::NativeEffectPhase::world_service)) return std::nullopt;
        row.phase = static_cast<gc::NativeEffectPhase>(phase);
        request.emplace(std::move(row)); break;
    }
    case Tag::orbit_recheck: {
        Shadow::OrbitRecheckRequest row;
        row.player = in.u32(); row.fleet = in.u64();
        request.emplace(std::move(row)); break;
    }
    default: return std::nullopt;
    }
    const auto first = in.u64(), next = in.u64(), first_invasion = in.u64(), next_invasion = in.u64();
    if (in.failed() || !request) return std::nullopt;
    return IntentRecord{std::move(*request), first, next, first_invasion, next_invasion};
}
} // namespace

core::Result<std::vector<std::uint8_t>> encode_outbox_state(const IntentOutbox& outbox, std::uint64_t ai_ticks) {
    try {
        Writer out;
        out.u32(outbox_state_version); out.u32(outbox.schedule().batch_ticks); out.u32(outbox.schedule().apply_delay);
        out.u64(ai_ticks); out.u64(outbox.frontier().id); out.u64(outbox.frontier().invasion); out.u64(outbox.lane_floor());
        out.u32(static_cast<std::uint32_t>(outbox.pending_slices().size()));
        for (const auto& slice : outbox.pending_slices()) {
            out.u64(slice.snapshot_tick); out.u32(slice.virtual_tick); out.u64(slice.apply_tick);
            out.u32(static_cast<std::uint32_t>(slice.intents.size()));
            for (const auto& intent : slice.intents) { out.u32(intent.player); put_record(out, intent.record); }
            out.u32(static_cast<std::uint32_t>(slice.commands.size()));
            for (const auto& command : slice.commands) {
                out.u64(command.tick); out.u64(command.issuer); out.u64(command.sequence); out.text(command.verb);
                put_value(out, authoritative::Value{command.arguments}, 0);
            }
            if (out.bytes.size() > max_state_bytes) return fail<std::vector<std::uint8_t>>("pending work exceeds the state limit");
        }
        return core::Result<std::vector<std::uint8_t>>::success(std::move(out.bytes));
    } catch (const std::exception& error) {
        return fail<std::vector<std::uint8_t>>(error.what());
    }
}

core::Result<DecodedOutboxState> decode_outbox_state(std::span<const std::uint8_t> bytes, bool allow_tail) {
    Reader in{bytes};
    DecodedOutboxState state;
    if (in.u32() != outbox_state_version) return fail<DecodedOutboxState>("unsupported version");
    state.schedule.batch_ticks = in.u32(); state.schedule.apply_delay = in.u32();
    state.ai_ticks = in.u64(); state.frontier.id = in.u64(); state.frontier.invasion = in.u64(); state.floor = in.u64();
    const auto slices = in.u32();
    if (in.failed() || !state.schedule.batch_ticks || slices > max_slices) return fail<DecodedOutboxState>("invalid header");
    std::uint64_t previous_apply{};
    for (std::uint32_t index = 0; index < slices; ++index) {
        OutboxSlice slice;
        slice.snapshot_tick = in.u64(); slice.virtual_tick = in.u32(); slice.apply_tick = in.u64();
        if (in.failed() || slice.virtual_tick == 0 || slice.virtual_tick > state.schedule.batch_ticks
            || slice.apply_tick != apply_tick(state.schedule, slice.snapshot_tick, slice.virtual_tick)
            || (index != 0 && slice.apply_tick < previous_apply))
            return fail<DecodedOutboxState>("slice timing differs from the schedule");
        previous_apply = slice.apply_tick;
        const auto intents = in.u32();
        if (in.failed() || in.remaining() < intents) return fail<DecodedOutboxState>("truncated intents");
        for (std::uint32_t i = 0; i < intents; ++i) {
            const auto player = in.u32();
            auto record = get_record(in);
            if (!record || in.failed()) return fail<DecodedOutboxState>("invalid intent");
            slice.intents.push_back(PlayerIntent{player, std::move(*record), {}});
        }
        const auto commands = in.u32();
        if (in.failed() || in.remaining() < commands) return fail<DecodedOutboxState>("truncated commands");
        for (std::uint32_t i = 0; i < commands; ++i) {
            authoritative::ScriptCommand command;
            command.tick = in.u64(); command.issuer = in.u64(); command.sequence = in.u64(); command.verb = in.text();
            auto arguments = get_value(in, 0);
            const auto* list = std::get_if<authoritative::ValueList>(&arguments.data);
            if (in.failed() || !list) return fail<DecodedOutboxState>("invalid command");
            command.arguments = *list;
            slice.commands.push_back(std::move(command));
        }
        state.slices.push_back(std::move(slice));
    }
    if (in.failed() || (in.remaining() != 0 && !allow_tail)) return fail<DecodedOutboxState>("trailing or missing bytes");
    state.tail.assign(bytes.begin() + static_cast<std::ptrdiff_t>(in.at), bytes.end());
    return core::Result<DecodedOutboxState>::success(std::move(state));
}

} // namespace eawr::script::foc::galactic_ai
