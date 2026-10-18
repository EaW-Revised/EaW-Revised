#include "galactic_ai_economic_replay.hpp"

#include <bit>
#include <limits>
#include <stdexcept>

namespace eawr::script::foc::galactic_ai {
namespace economic_replay {
namespace {
void word(Bytes& bytes, std::uint64_t value) { for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8))); }
Bytes words(std::initializer_list<std::uint64_t> values) { Bytes out; word(out, 1); for (auto value : values) word(out, value); return out; }
std::uint64_t raw(sim::math::Fixed value) { return std::bit_cast<std::uint64_t>(value.raw()); }
}
Bytes production(const gc::GalacticSession::NativeProductionRequest& request, const gc::GalacticSession::NativeProductionReceipt& receipt) {
    return words({request.player, request.planet, request.type, raw(*request.mark_cost), static_cast<std::uint64_t>(receipt.admission), receipt.build, raw(receipt.price)});
}
// A bill's inputs are rebuilt by the replayed world; only the producer callback's answer is journaled
// (build, accepted, payer, charge), which keeps the per-bill entry small (#4058).
constexpr std::size_t payment_bytes = 8 + 1 + 4 + 8;
Bytes payment(const gc::GalacticSession::NativeProductionNotice& notice, const gc::GalacticSession::NativeProductionPayment& result) {
    Bytes out; word(out, notice.build); out.push_back(result.accepted ? 1 : 0);
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(result.payer >> (i * 8)));
    word(out, raw(result.amount)); return out;
}
Bytes magic(const ActionRequest& request, std::span<const sim::EntityId> actors) {
    auto out = words({request.player, request.target, request.type, actors.size()}); for (auto actor : actors) word(out, actor); return out;
}
Bytes cash(gc::PlayerId player, sim::math::Fixed amount, sim::math::Fixed multiplier, sim::math::Fixed actual) { return words({player, raw(amount), raw(multiplier), raw(actual)}); }
Bytes finish(sim::EntityId build, bool salvage, sim::math::Fixed multiplier) { return words({build, salvage, raw(multiplier)}); }
}
namespace {
using Fixed = sim::math::Fixed;
struct Reader {
    std::span<const std::uint8_t> bytes;
    explicit Reader(std::span<const std::uint8_t> input) : bytes(input) { if (word() != 1) throw std::runtime_error("economic effect version invalid"); }
    std::uint64_t word() {
        if (bytes.size() < 8) throw std::runtime_error("economic effect truncated");
        std::uint64_t out{}; for (unsigned i = 0; i < 8; ++i) out |= std::uint64_t(bytes[i]) << (i * 8);
        bytes = bytes.subspan(8); return out;
    }
    gc::PlayerId player(bool absent_allowed = false) { const auto value = word(); if ((!value && !absent_allowed) || value > std::numeric_limits<gc::PlayerId>::max()) throw std::runtime_error("economic player invalid"); return static_cast<gc::PlayerId>(value); }
    Fixed amount() { return Fixed::from_raw(std::bit_cast<std::int64_t>(word())); }
    bool boolean() { const auto value = word(); if (value > 1) throw std::runtime_error("economic flag invalid"); return value != 0; }
    void end() const { if (!bytes.empty()) throw std::runtime_error("economic effect trailing bytes"); }
};
core::Diagnostic error(std::string message) { return detail::make_error(authoritative::codes::load_rejected, std::move(message)); }
template<class F> core::Result<void> checked(F&& body) {
    try { return body(); } catch (const std::exception& e) { return core::Result<void>::failure(error(e.what())); }
}
}
void NativeActions::bind_economic_replay_owners(gc::GalacticSession& world, Placement placement) {
    // No goal, funding, RNG, Lua or native manager exists during reconstruction.
    world.set_native_production_handler([&world](const auto& notice) -> core::Result<gc::GalacticSession::NativeProductionPayment> {
        using Result = core::Result<gc::GalacticSession::NativeProductionPayment>;
        if (!world.replaying_native_effects()) return Result::failure(error("economic replay payment owner used outside reconstruction"));
        if (notice.kind == gc::GalacticSession::NativeProductionNotice::Kind::marked) return Result::success({true, notice.player, {}});
        const auto bytes = world.consume_native_effect(gc::NativeEffectKind::production_payment); if (!bytes) return Result::failure(bytes.error());
        try {
            const auto& entry = bytes.value();
            if (entry.size() != economic_replay::payment_bytes) throw std::runtime_error("economic payment size invalid");
            const auto read = [&](std::size_t at, unsigned width) { std::uint64_t out{}; for (unsigned i = 0; i < width; ++i) out |= std::uint64_t(entry[at + i]) << (i * 8); return out; };
            gc::GalacticSession::NativeProductionPayment payment;
            const auto build = read(0, 8); const auto accepted = entry[8];
            payment.payer = static_cast<gc::PlayerId>(read(9, 4)); payment.amount = Fixed::from_raw(std::bit_cast<std::int64_t>(read(13, 8)));
            payment.accepted = accepted == 1;
            // Player, amount requested, remaining work and planet owner are rebuilt inputs;
            // their divergence (including payer versus input player) is caught by state hashes,
            // not by this compact outcome decoder.
            if (accepted > 1 || build != notice.build || payment.amount.raw() < 0 || payment.amount > notice.price
                || (payment.accepted && !payment.payer) || (!payment.accepted && payment.amount.raw()))
                return Result::failure(error("economic payment differs from recorded producer callback"));
            return Result::success(payment);
        } catch (const std::exception& e) { return Result::failure(error(e.what())); }
    });
    world.set_native_effect_replayer(gc::NativeEffectKind::production, [](auto& parent, const auto& effect, const auto& executor) {
        return checked([&]() -> core::Result<void> {
            Reader reader(effect.payload); gc::GalacticSession::NativeProductionRequest request;
            request.player = reader.player(); request.planet = reader.word(); request.type = reader.word(); request.mark_cost = reader.amount();
            const auto admission = reader.word(), build = reader.word(); const auto price = reader.amount(); reader.end();
            if (admission != static_cast<std::uint64_t>(gc::ProductionAdmission::allowed)) return core::Result<void>::failure(error("economic admission receipt invalid"));
            const auto receipt = parent.admit_native_production(request, executor); if (!receipt) return core::Result<void>::failure(receipt.error());
            if (receipt.value().admission != gc::ProductionAdmission::allowed || receipt.value().build != build || receipt.value().price != price)
                return core::Result<void>::failure(error("economic admission result diverged"));
            return core::Result<void>::success();
        });
    });
    world.set_native_effect_replayer(gc::NativeEffectKind::magic, [placement = std::move(placement)](auto& parent, const auto& effect, const auto&) {
        return checked([&]() -> core::Result<void> {
            Reader reader(effect.payload); ActionRequest request; request.kind = ActionKind::magic;
            request.player = reader.player(); request.target = reader.word(); request.type = reader.word(); const auto count = reader.word();
            if (!placement || count > gc::galactic_max_objects || count > reader.bytes.size() / 8) return core::Result<void>::failure(error("economic magic owner/actor list missing"));
            std::vector<sim::EntityId> expected; for (std::uint64_t i = 0; i < count; ++i) expected.push_back(reader.word()); reader.end();
            const auto made = parent.create_native_force({request.player, request.target, request.type, {}}, [&](const auto& snapshot, auto next) { return placement(request, snapshot, next); });
            if (!made) return core::Result<void>::failure(made.error());
            if (made.value() != expected) return core::Result<void>::failure(error("economic magic actor identities diverged"));
            return core::Result<void>::success();
        });
    });
    world.set_native_effect_replayer(gc::NativeEffectKind::cash, [](auto& parent, const auto& effect, const auto&) {
        return checked([&]() -> core::Result<void> {
            Reader reader(effect.payload); const auto player = reader.player(); const auto amount = reader.amount(), multiplier = reader.amount(), expected = reader.amount(); reader.end();
            const auto actual = parent.add_credits(player, amount, multiplier); if (!actual) return core::Result<void>::failure(actual.error());
            return actual.value() == expected ? core::Result<void>::success() : core::Result<void>::failure(error("economic cash result diverged"));
        });
    });
    world.set_native_effect_replayer(gc::NativeEffectKind::production_finish, [](auto& parent, const auto& effect, const auto&) {
        return checked([&]() -> core::Result<void> {
            Reader reader(effect.payload); const auto build = reader.word(); const auto salvage = reader.boolean(); const auto multiplier = reader.amount(); reader.end();
            return parent.finish_native_production_task(build, salvage, multiplier);
        });
    });
}
}
