#include "galactic_ai_lifecycle_state.hpp"
#include "galactic_ai_internal.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"
#include "eawr/core/sha256.hpp"

#include <bit>
#include <limits>

namespace eawr::script::foc::galactic_ai {
namespace {
constexpr std::string_view domain = "eawr-galactic-ai-lifecycle-v2";
constexpr std::string_view component_domain = "eawr-galactic-ai-lifecycle-native-v2";
void word(std::string& bytes, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) bytes.push_back(static_cast<char>((value >> (8 * index)) & 255U));
}
void text(std::string& bytes, std::string_view value) { word(bytes, value.size()); bytes.append(value); }
void optional(std::string& bytes, const std::optional<gc::PlayerId>& value) { word(bytes, value.has_value()); if (value) word(bytes, *value); }
template<class T> void settings(std::string& bytes, const std::map<std::string, Setting<T>, std::less<>>& entries) {
    word(bytes, entries.size());
    for (const auto& [name, entry] : entries) {
        text(bytes, name); word(bytes, std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(entry.priority)));
        if constexpr (std::is_same_v<T, std::string>) text(bytes, entry.value);
        else if constexpr (std::is_same_v<T, Number>) word(bytes, entry.value.repr);
        else word(bytes, entry.value);
    }
}
std::string encoded(const LifecycleState& state) {
    std::string bytes;
    text(bytes, domain);
    word(bytes, state.version); word(bytes, state.clocks.synchronization); word(bytes, state.clocks.mode);
    word(bytes, static_cast<std::uint64_t>(state.admission.active_mode)); word(bytes, state.admission.processing); word(bytes, state.admission.auto_resolve);
    for (const auto frames : state.service_periods) word(bytes, frames);
    word(bytes, state.players.size());
    for (const auto& player : state.players) {
        word(bytes, player.selected.player); text(bytes, player.selected.player_type); text(bytes, player.selected.difficulty);
        optional(bytes, player.selected.enemy); optional(bytes, player.selected.human); word(bytes, player.initialized);
        for (const auto deadline : player.deadlines) word(bytes, deadline);
        word(bytes, player.active_templates.size()); for (const bool active : player.active_templates) word(bytes, active);
        word(bytes, player.activation_order.size()); for (const auto index : player.activation_order) word(bytes, index);
        word(bytes, player.template_cursor);
        settings(bytes, player.policy.equations); settings(bytes, player.policy.allowances);
        settings(bytes, player.policy.goal_categories); settings(bytes, player.policy.goal_types);
        settings(bytes, player.policy.plan_categories); settings(bytes, player.policy.plan_types); settings(bytes, player.policy.plan_names);
    }
    return bytes;
}
std::string digest(std::string_view bytes) {
    return core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}
struct Reader {
    std::span<const std::uint8_t> bytes;
    bool bad{};
    std::uint64_t word() {
        if (bytes.size() < 8) { bad = true; return 0; }
        std::uint64_t value{};
        for (unsigned index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(bytes[index]) << (8 * index);
        bytes = bytes.subspan(8); return value;
    }
    bool flag() { const auto value = word(); if (value > 1) bad = true; return value != 0; }
    std::string text() {
        const auto size = word();
        if (size > bytes.size()) { bad = true; return {}; }
        const auto value = bytes.first(static_cast<std::size_t>(size)); bytes = bytes.subspan(value.size());
        return {value.begin(), value.end()};
    }
    std::uint32_t identity() {
        const auto value = word(); if (value > std::numeric_limits<std::uint32_t>::max()) bad = true;
        return static_cast<std::uint32_t>(value);
    }
    std::optional<gc::PlayerId> optional() { if (!flag()) return {}; return identity(); }
    std::size_t count(std::size_t minimum) {
        const auto value = word();
        if (value > gc::galactic_max_objects || value > bytes.size() / minimum) { bad = true; return 0; }
        return static_cast<std::size_t>(value);
    }
    int priority() {
        const auto value = std::bit_cast<std::int64_t>(word());
        if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) bad = true;
        return static_cast<int>(value);
    }
    template<class T> std::map<std::string, Setting<T>, std::less<>> settings() {
        std::map<std::string, Setting<T>, std::less<>> entries;
        const auto size = count(24);
        std::string previous;
        for (std::size_t index = 0; index < size && !bad; ++index) {
            auto name = text();
            if (index && !(previous < name)) bad = true;
            previous = name;
            Setting<T> entry; entry.priority = priority();
            if constexpr (std::is_same_v<T, std::string>) entry.value = text();
            else if constexpr (std::is_same_v<T, Number>) entry.value = Number::from_repr(word());
            else entry.value = flag();
            entries.emplace(std::move(name), std::move(entry));
        }
        return entries;
    }
};
core::Result<LifecycleState> decoded(Reader& in) {
    using Result = core::Result<LifecycleState>;
    LifecycleState state;
    if (in.text() != domain || in.word() != 2) return Result::failure(lifecycle_error("unsupported lifecycle component schema"));
    state.clocks = {in.word(), in.word()};
    const auto mode = in.word();
    if (mode > static_cast<std::uint64_t>(Mode::land)) in.bad = true;
    state.admission = {static_cast<Mode>(mode), in.flag(), in.flag()};
    for (auto& frames : state.service_periods) { frames = in.word(); if (frames == 0) in.bad = true; }
    const auto count = in.count(176);
    for (std::size_t index = 0; index < count && !in.bad; ++index) {
        PlayerLifecycle player;
        player.selected.player = in.identity(); player.selected.player_type = in.text(); player.selected.difficulty = in.text();
        player.selected.enemy = in.optional(); player.selected.human = in.optional(); player.initialized = in.flag();
        for (auto& deadline : player.deadlines) deadline = in.word();
        const auto active = in.count(8);
        for (std::size_t n = 0; n < active && !in.bad; ++n) player.active_templates.push_back(in.flag());
        const auto order = in.count(8);
        for (std::size_t n = 0; n < order && !in.bad; ++n) player.activation_order.push_back(static_cast<std::size_t>(in.word()));
        player.template_cursor = static_cast<std::size_t>(in.word());
        player.policy.equations = in.settings<std::string>(); player.policy.allowances = in.settings<Number>();
        player.policy.goal_categories = in.settings<bool>(); player.policy.goal_types = in.settings<bool>();
        player.policy.plan_categories = in.settings<bool>(); player.policy.plan_types = in.settings<bool>(); player.policy.plan_names = in.settings<bool>();
        state.players.push_back(std::move(player));
    }
    if (in.bad || !in.bytes.empty()) return Result::failure(lifecycle_error("malformed or trailing lifecycle component bytes"));
    return Result::success(std::move(state));
}
} // namespace

namespace {
std::vector<std::uint8_t> lifecycle_record(const LifecycleState& state, std::string_view configuration, gc::PlayerId player) {
    LifecycleState record;
    std::string bytes;
    if (!player) {
        record.version = state.version; record.clocks = state.clocks; record.admission = state.admission;
        record.service_periods = state.service_periods; text(bytes, configuration);
    } else {
        for (const auto& row : state.players) if (row.selected.player == player) { record.players.push_back(row); break; }
    }
    bytes += encoded(record);
    return {bytes.begin(), bytes.end()};
}
}
void Manager::Impl::update_record(gc::PlayerId player) {
    auto& slice = player ? player_state.at(player).records : records;
    slice.set(player, 0, 0, lifecycle_record(state, configuration_digest, player));
}
void Manager::Impl::reset_records() {
    records = {}; update_record();
    for (auto& [player, slice] : player_state) { slice.records = {}; slice.traces.clear(); update_record(player); }
}
core::RecordDigest Manager::Impl::rebuild_records() const {
    core::RecordDigest rebuilt; rebuilt.set(0, 0, 0, lifecycle_record(state, configuration_digest, 0));
    for (const auto& row : state.players) rebuilt.set(row.selected.player, 0, 0, lifecycle_record(state, configuration_digest, row.selected.player));
    return rebuilt;
}
std::size_t Manager::record_byte_upper_bound() const noexcept {
    auto bytes = impl_->records.byte_size() + 1024;
    for (const auto& [player, slice] : impl_->player_state) { static_cast<void>(player); bytes += slice.records.byte_size(); }
    return bytes;
}
core::Result<std::string> Manager::record_sync_hash(bool cold) const {
    if (impl_->failure) return core::Result<std::string>::failure(*impl_->failure);
    if (cold) return core::Result<std::string>::success(impl_->rebuild_records().digest(component_domain));
    auto sums = impl_->records.shards();
    for (const auto& [player, slice] : impl_->player_state) {
        static_cast<void>(player); core::RecordDigest::merge_shards(sums, slice.records.shards());
    }
    return core::Result<std::string>::success(core::RecordDigest::digest_of(component_domain, sums));
}
std::string lifecycle_configuration_digest(const LifecycleSetup& setup, const ai::AiData& data, std::span<const PlanInput> plans) {
    std::string bytes; text(bytes, "eawr-galactic-ai-lifecycle-configuration-v1");
    const auto paths = setup.xml_paths.empty() ? required_xml() : setup.xml_paths;
    word(bytes, paths.size()); for (const auto& path : paths) text(bytes, path);
    word(bytes, setup.xml.size()); for (const auto& [path, contents] : setup.xml) { text(bytes, path); text(bytes, contents); }
    word(bytes, data.goal_categories.size()); for (const auto& [name, category] : data.goal_categories) { text(bytes, name); word(bytes, category); }
    word(bytes, plans.size());
    for (const auto& plan : plans) { text(bytes, plan.name); word(bytes, plan.goals.size()); for (const auto& goal : plan.goals) text(bytes, goal); }
    return digest(bytes);
}
core::Result<std::string> Manager::lifecycle_hash() const { return core::Result<std::string>::success(digest(encoded(impl_->state))); }
core::Result<NativeState> Manager::capture() const {
    if (auto verified = verify_record_sync_hash(); !verified) return core::Result<NativeState>::failure(verified.error());
    if (impl_->failure) return core::Result<NativeState>::failure(*impl_->failure);
    std::string payload; text(payload, component_domain); text(payload, impl_->configuration_digest); payload += encoded(impl_->state);
    NativeState state;
    state.components.emplace(lifecycle_component, std::vector<std::uint8_t>(payload.begin(), payload.end()));
    if (auto valid = encode_native_state(state); !valid) return core::Result<NativeState>::failure(valid.error());
    return core::Result<NativeState>::success(std::move(state));
}
core::Result<void> Manager::restore(const NativeState& state) {
    if (auto valid = encode_native_state(state); !valid) return core::Result<void>::failure(valid.error());
    const auto found = state.components.find(lifecycle_component);
    if (found == state.components.end()) return core::Result<void>::failure(lifecycle_error("lifecycle native component is missing"));
    Reader in{found->second};
    if (in.text() != component_domain || in.text() != impl_->configuration_digest || in.bad)
        return core::Result<void>::failure(lifecycle_error("lifecycle native component configuration differs"));
    auto saved = decoded(in);
    if (!saved) return core::Result<void>::failure(saved.error());
    return restore_lifecycle(saved.value());
}
core::Result<std::string> Manager::state_hash() const {
    auto state = capture(); if (!state) return core::Result<std::string>::failure(state.error());
    return native_state_hash(state.value());
}
} // namespace eawr::script::foc::galactic_ai
