#include "galactic_ai_execution_state.hpp"
#include "eawr/core/sha256.hpp"

#include <limits>
#include <set>
#include <tuple>

namespace eawr::script::foc::galactic_ai {
namespace {
core::Diagnostic invalid(std::string message) {
    core::Diagnostic out;
    out.code = std::string(authoritative::codes::invalid_request);
    out.message = "galactic-execution state: " + std::move(message);
    return out;
}
core::Result<void> validate(const ExecutionRetainedState& state) {
    for (const auto& [player, row] : state) {
        if (!player || (row.finder_present && !row.initialized))
            return core::Result<void>::failure(invalid("invalid player or finder initialization"));
        if (!numeric::binary64::is_finite(row.maximum_distance.repr) || row.maximum_distance < Number(0))
            return core::Result<void>::failure(invalid("invalid maximum distance"));
        std::set<gc::PlanetId> nodes;
        if (row.node_order.size() > 1000)
            return core::Result<void>::failure(invalid("node order exceeds qualified reader domain"));
        for (const auto node : row.node_order) if (!node || !nodes.insert(node).second)
            return core::Result<void>::failure(invalid("node order requires unique nonzero identities"));
        for (const auto& [object, plan] : row.reservations) if (!object || !plan)
            return core::Result<void>::failure(invalid("reservation requires object and potential plan identities"));
    }
    return core::Result<void>::success();
}
void word(std::vector<std::uint8_t>& bytes, std::uint64_t value, unsigned size = 8) {
    for (unsigned i = 0; i < size; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
std::vector<std::uint8_t> execution_record(gc::PlayerId player, const ExecutionRetainedPlayer& row) {
    std::vector<std::uint8_t> bytes;
    word(bytes, player); word(bytes, row.initialized, 1); word(bytes, row.finder_present, 1);
    word(bytes, row.next_refresh); word(bytes, row.maximum_distance.repr); word(bytes, row.node_order.size());
    for (const auto node : row.node_order) word(bytes, node);
    word(bytes, row.reservations.size());
    for (const auto& [object, plan] : row.reservations) { word(bytes, object); word(bytes, plan); }
    return bytes;
}
void update_records(core::RecordDigest& records, const ExecutionRetainedState& before, const ExecutionRetainedState& after) {
    for (const auto& [player, row] : before) { static_cast<void>(row); if (!after.contains(player)) records.erase(player, 0, 0); }
    for (const auto& [player, row] : after) {
        const auto old = before.find(player); if (old != before.end() && old->second == row && old->second.maximum_distance.repr == row.maximum_distance.repr) continue;
        records.set(player, 0, 0, execution_record(player, row));
    }
}
core::RecordDigest execution_records(const ExecutionRetainedState& state) {
    core::RecordDigest records; update_records(records, {}, state); return records;
}
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    bool valid{true};
    std::uint64_t word(unsigned size = 8) {
        if (size > bytes.size() - at) { valid = false; return 0; }
        std::uint64_t out{};
        for (unsigned i = 0; i < size; ++i) out |= static_cast<std::uint64_t>(bytes[at++]) << (i * 8);
        return out;
    }
    bool flag() {
        const auto value = word(1);
        if (value > 1) valid = false;
        return value == 1;
    }
    std::uint64_t count(std::size_t minimum_row) {
        const auto value = word();
        if (!valid || value > (bytes.size() - at) / minimum_row) { valid = false; return 0; }
        return value;
    }
};
}
core::Result<void> ExecutionStateContributor::replace(ExecutionRetainedState state) {
    if (auto checked = validate(state); !checked) return checked;
    if (validator_) if (auto checked = validator_(state); !checked) return checked;
    mark_state_changed();
    update_records(records_, state_, state); state_ = std::move(state);
    return core::Result<void>::success();
}
core::Result<NativeState> ExecutionStateContributor::capture() const {
    if (auto verified = verify_record_sync_hash(); !verified) return core::Result<NativeState>::failure(verified.error());
    if (auto checked = validate(state_); !checked) return core::Result<NativeState>::failure(checked.error());
    NativeState out;
    auto& bytes = out.components[std::string(component)];
    word(bytes, component.size(), 4);
    bytes.insert(bytes.end(), component.begin(), component.end());
    word(bytes, version, 4);
    word(bytes, state_.size());
    for (const auto& [player, row] : state_) {
        word(bytes, player);
        word(bytes, row.initialized, 1); word(bytes, row.finder_present, 1);
        word(bytes, row.next_refresh); word(bytes, row.maximum_distance.repr);
        word(bytes, row.node_order.size());
        for (const auto node : row.node_order) word(bytes, node);
        word(bytes, row.reservations.size());
        for (const auto& [object, plan] : row.reservations) { word(bytes, object); word(bytes, plan); }
    }
    return core::Result<NativeState>::success(std::move(out));
}
core::Result<void> ExecutionStateContributor::restore(const NativeState& input) {
    if (!validator_) {
        auto error = invalid("restore requires the reconstructed native reference validator");
        error.code = std::string(authoritative::codes::missing_api);
        return core::Result<void>::failure(std::move(error));
    }
    const auto found = input.components.find(component);
    if (input.version != 1 || found == input.components.end())
        return core::Result<void>::failure(invalid("missing component or unsupported envelope version"));
    Reader reader{found->second};
    const auto length = reader.word(4);
    if (!reader.valid || length != component.size() || length > reader.bytes.size() - reader.at)
        return core::Result<void>::failure(invalid("invalid component domain"));
    for (const auto character : component) if (reader.word(1) != static_cast<std::uint8_t>(character)) reader.valid = false;
    if (reader.word(4) != version) reader.valid = false;
    const auto players = reader.count(42);
    ExecutionRetainedState staged;
    gc::PlayerId last_player{};
    for (std::uint64_t i = 0; reader.valid && i < players; ++i) {
        const auto player = reader.word();
        if (!player || player > std::numeric_limits<gc::PlayerId>::max() || player <= last_player) { reader.valid = false; break; }
        last_player = static_cast<gc::PlayerId>(player);
        ExecutionRetainedPlayer row;
        row.initialized = reader.flag(); row.finder_present = reader.flag();
        row.next_refresh = reader.word(); row.maximum_distance = Number::from_repr(reader.word());
        const auto nodes = reader.count(8);
        if (nodes > 1000) reader.valid = false;
        for (std::uint64_t n = 0; reader.valid && n < nodes; ++n) {
            const auto node = reader.word();
            if (node > std::numeric_limits<gc::PlanetId>::max()) { reader.valid = false; break; }
            row.node_order.push_back(static_cast<gc::PlanetId>(node));
        }
        const auto reservations = reader.count(16);
        sim::EntityId last_object{};
        for (std::uint64_t n = 0; reader.valid && n < reservations; ++n) {
            const auto object = reader.word(), plan = reader.word();
            if (!object || object <= last_object || !plan) { reader.valid = false; break; }
            last_object = object; row.reservations.emplace(object, plan);
        }
        staged.emplace(last_player, std::move(row));
    }
    if (!reader.valid || reader.at != reader.bytes.size())
        return core::Result<void>::failure(invalid("malformed, unordered or trailing component bytes"));
    return replace(std::move(staged));
}
core::Result<std::string> ExecutionStateContributor::state_hash() const {
    auto captured = capture();
    if (!captured) return core::Result<std::string>::failure(captured.error());
    return core::Result<std::string>::success(core::sha256_hex(captured.value().components.at(std::string(component))));
}
core::Result<NativeState> LiveExecutionStateContributor::capture() const {
    if (auto verified = verify_record_sync_hash(); !verified) return core::Result<NativeState>::failure(verified.error());
    ExecutionStateContributor codec(validator_);
    if (auto copied = codec.replace(retained_); !copied) return core::Result<NativeState>::failure(copied.error());
    return codec.capture();
}
core::Result<void> LiveExecutionStateContributor::replace_retained(ExecutionRetainedState state) {
    ExecutionStateContributor codec(validator_);
    if (auto valid = codec.replace(std::move(state)); !valid) return valid;
    mark_state_changed(); update_records(records_, retained_, codec.state()); retained_ = codec.state();
    return core::Result<void>::success();
}
core::Result<std::string> LiveExecutionStateContributor::state_hash() const {
    auto captured = capture();
    if (!captured) return core::Result<std::string>::failure(captured.error());
    return core::Result<std::string>::success(core::sha256_hex(captured.value().components.at(std::string(ExecutionStateContributor::component))));
}
core::Result<void> LiveExecutionStateContributor::restore(const NativeState& input) {
    if (!pool_ || !pool_->pool || !inputs_) return core::Result<void>::failure(invalid("live restore requires rebuilt pool and route inputs"));
    ExecutionStateContributor codec(validator_);
    if (auto restored = codec.restore(input); !restored) return restored;
    auto state = codec.state();
    std::map<gc::PlayerId, ExecutionRoutes> staged;
    auto pool = std::make_shared<FreestorePool>(*pool_->pool);
    pool->reservations.clear();
    for (const auto& [player, row] : state) {
        for (const auto& [object, plan] : row.reservations) if (!pool->reservations.emplace(object, plan).second)
            return core::Result<void>::failure(invalid("duplicate reservation across native players"));
        auto& routes = staged[player]; routes.next_refresh = row.next_refresh;
        if (!row.initialized) continue;
        const ai::DerivedExecutionRestore reconstruction;
        auto source = inputs_(player, &reconstruction);
        if (!source) return core::Result<void>::failure(source.error());
        auto ordered = std::move(source).value();
        if (ordered.player != player || ordered.planets.size() != row.node_order.size())
            return core::Result<void>::failure(invalid("reconstructed route node domain differs"));
        std::vector<ExecutionPlanet> planets;
        for (const auto node : row.node_order) {
            const auto found = std::find_if(ordered.planets.begin(), ordered.planets.end(), [&](const auto& p) { return p.planet == node; });
            if (found == ordered.planets.end()) return core::Result<void>::failure(invalid("unresolved restored route node"));
            planets.push_back(*found);
        }
        ordered.planets = std::move(planets); ordered.maximum_distance = row.maximum_distance;
        auto built = build_execution_routes(ordered, executor_);
        if (!built) return core::Result<void>::failure(built.error());
        routes.available = true; routes.paths = built.value().paths; routes.rows = std::move(built).value().rows;
        std::sort(routes.rows.begin(), routes.rows.end(), [](const auto& a, const auto& b) {
            return std::tie(a.player, a.source, a.candidate) < std::tie(b.player, b.source, b.candidate);
        });
    }
    // WGA-139: new native owners have no finder. Rebuild from copied inputs,
    // preserve the restored deadline, then publish before their first service.
    mark_state_changed();
    update_records(records_, retained_, state); retained_ = std::move(state); routes_ = std::move(staged); pool_->pool = std::move(pool);
    return core::Result<void>::success();
}
core::Result<std::string> ExecutionStateContributor::record_sync_hash(bool cold) const {
    return core::Result<std::string>::success(cold ? execution_records(state_).digest(component) : records_.digest(component));
}
void LiveExecutionStateContributor::enable_revision_tracking() {
    mark_state_changed(); records_ = execution_records(retained_); tracked_ = true;
}
core::Result<std::string> LiveExecutionStateContributor::record_sync_hash(bool cold) const {
    return core::Result<std::string>::success(cold || !tracked_ ? execution_records(retained_).digest(ExecutionStateContributor::component)
        : records_.digest(ExecutionStateContributor::component));
}
} // namespace eawr::script::foc::galactic_ai
