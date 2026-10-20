#include "galactic_ai_world.hpp"
#include "eawr/core/gc_profile.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

namespace eawr::script::foc::ai {
namespace {
// Rows the simulation keeps in ascending id order: one binary search instead of a scan per target evaluation.
template<class Rows, class Id, class Key>
const auto* sorted_find(const Rows& rows, Id id, Key key) {
    const auto found = std::lower_bound(rows.begin(), rows.end(), id, [&](const auto& row, Id value) { return key(row) < value; });
    return found != rows.end() && key(*found) == id ? &*found : nullptr;
}
std::optional<Real> parameter_number(const std::vector<Binding>& bindings, std::string_view name) {
    for (const auto& b : bindings) if (b.parameter == name) if (const auto* value = std::get_if<Real>(&b.value)) return *value;
    return {};
}
std::optional<std::string> parameter_text(const std::vector<Binding>& bindings, std::string_view name) {
    for (const auto& b : bindings) if (b.parameter == name) if (const auto* value = std::get_if<std::string>(&b.value)) return upper_case(*value);
    return {};
}
bool parameter_equals_nocase(const std::vector<Binding>& bindings, std::string_view name, std::string_view value) {
    for (const auto& b : bindings) if (b.parameter == name)
        if (const auto* text = std::get_if<std::string>(&b.value))
            return text->size() == value.size() && std::equal(text->begin(), text->end(), value.begin(),
                [](char a, char c) { // ASCII-only fold: no ambient locale in the simulation
                    const auto fold = [](char x) { return x >= 'a' && x <= 'z' ? static_cast<char>(x - 'a' + 'A') : x; };
                    return fold(a) == fold(c);
                });
    return false;
}
bool has_text_parameter(const std::vector<Binding>& bindings, std::string_view name) {
    for (const auto& b : bindings) if (b.parameter == name && std::holds_alternative<std::string>(b.value)) return true;
    return false;
}
Real fraction(Real part, Real whole) { return whole == Real{} ? Real{} : to_single(part / whole); }
Real boolean_value(bool value) { return real(value ? 1 : 0); }
bool parameter_matches(const std::vector<Binding>& bindings, std::string_view name, std::string_view value) {
    for (const auto& binding : bindings) if (binding.parameter == name)
        if (const auto* text = std::get_if<std::string>(&binding.value); text && upper_case(*text) == value) return true;
    return false;
}
bool key_less(const GalacticCacheKey& a, const GalacticCacheKey& b) {
    const auto left = std::tie(a.player, a.enemy, a.human, a.planet, a.tokens);
    const auto right = std::tie(b.player, b.enemy, b.human, b.planet, b.tokens);
    if (left != right) return left < right;
    for (std::size_t i = 0; i < std::min(a.bindings.size(), b.bindings.size()); ++i) {
        const auto& x = a.bindings[i]; const auto& y = b.bindings[i];
        if (x.parameter != y.parameter) return x.parameter < y.parameter;
        if (x.value.index() != y.value.index()) return x.value.index() < y.value.index();
        if (const auto* xn = std::get_if<Real>(&x.value)) {
            const auto yn = std::get<Real>(y.value);
            if (*xn != yn) return xn->repr < yn.repr;
        } else if (std::get<std::string>(x.value) != std::get<std::string>(y.value)) return std::get<std::string>(x.value) < std::get<std::string>(y.value);
    }
    return a.bindings.size() < b.bindings.size();
}
}

bool operator==(const GalacticCacheKey& a, const GalacticCacheKey& b) {
    if (std::tie(a.player, a.enemy, a.human, a.planet, a.tokens) != std::tie(b.player, b.enemy, b.human, b.planet, b.tokens)
        || a.bindings.size() != b.bindings.size()) return false;
    for (std::size_t i = 0; i < a.bindings.size(); ++i)
        if (a.bindings[i].parameter != b.bindings[i].parameter || a.bindings[i].value != b.bindings[i].value) return false;
    return true;
}
bool operator<(const GalacticCacheKey& a, const GalacticCacheKey& b) { return key_less(a, b); }

struct GalacticWorld::Evaluator final : LookupResolver {
    const GalacticWorld& world;
    const Engine& engine;
    const Context& context;
    unsigned depth{};
    ReadMode mode{ReadMode::ordinary};
    std::optional<int> timeline;
    Evaluator(const GalacticWorld& w, const Engine& e, const Context& c, unsigned nesting = 0, ReadMode selected = ReadMode::ordinary, std::optional<int> era = {})
        : world(w), engine(e), context(c), depth(nesting), mode(selected), timeline(era) {}
    std::optional<Real> resolve(const Lookup& lookup, const std::vector<Binding>& bindings) override {
        if (lookup.tokens.empty()) return {};
        const auto& head = lookup.tokens.front();
        if (lookup.tokens.size() == 2 && lookup.tokens[1] == "ACTIVEGOALS"
            && (head == "VARIABLE_SELF" || head == "VARIABLE_ENEMY" || head == "VARIABLE_HUMAN")) {
            const auto category = parameter_number(bindings, "PARAMETER_CATEGORY");
            const auto owner = head == "VARIABLE_SELF" ? std::optional{context.player}
                : head == "VARIABLE_ENEMY" ? context.enemy : context.human;
            if (!category || !owner || std::count_if(bindings.begin(), bindings.end(),
                [](const auto& binding) { return binding.parameter == "PARAMETER_CATEGORY"; }) != 1) return {};
            const auto count = engine.active_goal_count(*owner, static_cast<std::uint64_t>(*category));
            return count ? std::optional{Real(*count)} : std::nullopt;
        }
        if (lookup.tokens.size() == 2 && head == "GAME" && lookup.tokens[1] == "TIMELINE") {
            if (timeline) return real(*timeline);
            if (world.timeline_reader_) {
                auto value = world.timeline_reader_();
                return value && value.value() >= 0 && value.value() <= 5 ? std::optional{real(value.value())} : std::nullopt;
            }
        }
        if (head.starts_with("FUNCTION_")) {
            if (lookup.tokens.size() != 2 || lookup.tokens[1] != "EVALUATE" || depth >= 64) return {};
            const auto* nested = engine.data().equations.find(head.substr(9)); if (!nested) return {};
            AiRandom random;
            random.set_seed(world.setup_.seed + (context.player + 1U) * 0x83U + 0x12345678U
                + crc32(nested->name) + crc32(context.target ? context.target->name : "")); // PE-12
            Evaluator evaluator(world, engine, context, depth + 1, mode, timeline);
            return run(*nested, evaluator, random);
        }
        return world.lookup(context, lookup, bindings, mode);
    }
};

std::optional<Real> GalacticWorld::evaluate_with_timeline(const Engine& engine, std::string_view name,
    tactical::PlayerId owner, const Target* target, int value) const {
    gc_profile::Scope profile_scope(gc_profile::Phase::perception);
    const auto reads = sequential_reads();
    if (value < 0 || value > 5 || !snapshot_ || !player(owner) || !refresh_economy_runtime()) return {};
    const auto* equation = engine.data().equations.find(name); if (!equation) return {};
    PlayerAi selected; selected.player = owner;
    const auto context = goal_context(selected, target);
    AiRandom random; random.set_seed(setup_.seed + (owner + 1U) * 0x83U + 0x12345678U
        + crc32(equation->name) + crc32(target ? target->name : "")); // PE-12
    Evaluator evaluator(*this, engine, context, 0, ReadMode::read_only, value);
    return run(*equation, evaluator, random);
}

std::optional<Real> GalacticWorld::perception(const Engine& engine, const Equation& equation,
    const Context& context, AiRandom& random) const {
    gc_profile::Scope profile_scope(gc_profile::Phase::perception);
    const auto reads = sequential_reads();
    if (!snapshot_ || !player(context.player) || !refresh_economy_runtime()) return {};
    Evaluator evaluator(*this, engine, context); return run(equation, evaluator, random);
}

std::optional<Real> GalacticWorld::perception_read_only(const Engine& engine, const Equation& equation,
    const Context& context, AiRandom& random) const {
    gc_profile::Scope profile_scope(gc_profile::Phase::perception);
    const auto reads = sequential_reads();
    if (!snapshot_ || !player(context.player) || !refresh_economy_runtime()) return {};
    Evaluator evaluator(*this, engine, context, 0, ReadMode::read_only); return run(equation, evaluator, random);
}

std::optional<Real> GalacticWorld::reconstruction_enemy_space(const DerivedExecutionRestore&, const Context& context) const {
    const auto reads = sequential_reads();
    if (!snapshot_ || !player(context.player) || !context.target || !planet(context.target->object)) return {};
    const GalacticCacheKey key{context.player, context.enemy, context.human, context.target->object,
        {"VARIABLE_TARGET", "ENEMYFORCE", "SPACETOTALUNNORMALIZED"}, {}};
    const auto lock = query_lock();
    // B-U05: a derived load is not an admitted force-cache refresh. Preserve
    // archived observations, including expired/frame-zero rows. A missing row
    // uses the qualified raw getter without publishing cache or visibility.
    return read(key, ReadMode::archived);
}

std::optional<Real> GalacticWorld::target_enemy_force(const Context& context,
    galactic::ProductionDomain channel, std::uint64_t category_mask) const {
    const auto reads = sequential_reads();
    if (!snapshot_ || !player(context.player) || !context.target) return {};
    const auto* at = planet(context.target->object);
    if (!at) return {};
    GalacticCacheKey key{context.player, context.enemy, context.human, at->planet.id,
        {"VARIABLE_TARGET", "ENEMYFORCE", channel == galactic::ProductionDomain::ground
            ? "GROUNDTOTALUNNORMALIZED" : "SPACETOTALUNNORMALIZED"}, {}};
    if (category_mask) key.bindings.push_back({"PARAMETER_CATEGORY", Real(category_mask)});
    const auto lock = query_lock();
    const auto cached = read(key, ReadMode::ordinary);
    if (!cached || allied(context.player, at->planet.owner)) return cached;
    // The original contrast reader adds these actual defensive facts after
    // the cached force read. They are not another fogged perception query.
    if (!at->production) return {};
    const auto mask = category_mask ? category_mask : ~std::uint64_t{};
    const auto power = [&](tactical::TypeId id) -> std::optional<Real> {
        const auto found = std::lower_bound(setup_.force_types.begin(), setup_.force_types.end(), id,
            [](const auto& row, auto type) { return row.type < type; });
        if (found == setup_.force_types.end() || found->type != id) return {};
        return (found->categories & mask) ? to_single(found->power) : Real{};
    };
    Real result = *cached;
    if (channel == galactic::ProductionDomain::space) {
        if (const auto type = at->production->starbase_type) {
            const auto value = power(type); if (!value) return {};
            result += *value;
        }
        return result;
    }
    for (const auto& object : at->production->objects)
        if (object.domain == galactic::ProductionDomain::ground && object.kind == galactic::ProductionKind::structure) {
            const auto value = power(object.type); if (!value) return {};
            result += *value;
        }
    const auto* control = snapshot_->control();
    if (!control) return {};
    const auto surface = std::find_if(control->planets.begin(), control->planets.end(),
        [&](const auto& row) { return row.planet == at->planet.id; });
    if (surface == control->planets.end()) return {};
    Real built{};
    for (const auto& record : surface->built_structures) {
        const auto value = power(record.type); if (!value) return {};
        built = to_single(built + *value); // WGA-441: this subtotal is binary32.
    }
    return result + built;
}

std::optional<Real> GalacticWorld::reconstruction_perception(const DerivedExecutionRestore&,
    const Engine& engine, const Equation& equation, const Context& context, AiRandom& random) const {
    const auto reads = sequential_reads();
    if (!snapshot_ || !player(context.player)) return {};
    Evaluator evaluator(*this, engine, context, 0, ReadMode::archived); return run(equation, evaluator, random);
}

std::uint32_t galactic_visibility_mask(std::span<const std::string> tokens) {
    if (tokens.size() < 2 || tokens[0] != "VARIABLE_TARGET") return 0;
    if (tokens[1] == "TARGETPLAYERID") return galactic::bit(galactic::Intelligence::ownership);
    if (tokens.size() < 3 || (tokens[1] != "FRIENDLYFORCE" && tokens[1] != "ENEMYFORCE")) return 0;
    // WGA-258: strength visibility is fleet/ground count, not content visibility.
    if (tokens[2] == "HASSPACEFORCE") return galactic::bit(galactic::Intelligence::fleet_presence);
    if (tokens[2].starts_with("SPACETOTAL")) return galactic::bit(galactic::Intelligence::fleet_count);
    if (tokens[2].starts_with("GROUNDTOTAL") || tokens[2] == "HASGROUNDFORCE")
        return galactic::bit(galactic::Intelligence::ground_count);
    return 0;
}
namespace {
std::uint64_t mix(std::uint64_t value) {
    value ^= value >> 30U; value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U; value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}
std::uint64_t key_hash(const GalacticInternedKey& key) {
    auto value = mix(key.player);
    value = mix(value ^ (key.enemy ? static_cast<std::uint64_t>(*key.enemy) + 1 : 0));
    value = mix(value ^ (key.human ? static_cast<std::uint64_t>(*key.human) + 1 : 0));
    return mix(mix(mix(value ^ key.planet) ^ key.token) ^ key.binding.identity);
}
bool bindings_equal(const std::vector<Binding>& a, const std::vector<Binding>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].parameter != b[i].parameter || a[i].value.index() != b[i].value.index()) return false;
        if (const auto* number = std::get_if<Real>(&a[i].value)) {
            const auto other = std::get<Real>(b[i].value);
            if (number->repr != other.repr && *number != other) return false;
        } else if (std::get<std::string>(a[i].value) != std::get<std::string>(b[i].value)) return false;
    }
    return true;
}
std::uint64_t binding_hash(const std::vector<Binding>& bindings) {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto text = [&](std::string_view value) {
        hash = mix(hash ^ value.size());
        for (const auto c : value) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ULL; }
    };
    for (const auto& binding : bindings) {
        text(binding.parameter); hash = mix(hash ^ binding.value.index());
        if (const auto* number = std::get_if<Real>(&binding.value)) hash = mix(hash ^ (*number == Real{} ? 0 : number->repr));
        else text(std::get<std::string>(binding.value));
    }
    return hash;
}
}
thread_local const GalacticWorld* GalacticWorld::active_world_{};
GalacticWorld::Shard& GalacticWorld::shard(tactical::PlayerId owner) const {
    // One shard per setup player exists from construction, so a read finds its shard without inserting.
    const auto found = shards_.find(owner);
    if (found != shards_.end()) return found->second;
    // Only a player outside the setup misses, and inserting into the map is never safe inside a partitioned phase.
    assert(partitioned_reads_.load(std::memory_order_acquire) == 0 && owner_reads_.load(std::memory_order_acquire) == 0);
    return shards_[owner];
}
const PerceptionToken& GalacticWorld::token_of(const Shard& owner, std::uint64_t id) const {
    if (const auto found = tokens_.find(id); found != tokens_.end()) return *found->second;
    return *owner.late_tokens.at(id);
}
namespace {
// Interns into one id-to-token table and its spelling index.
std::uint64_t intern_into(std::map<std::vector<std::string>, std::uint64_t>& ids,
    std::map<std::uint64_t, std::shared_ptr<const PerceptionToken>>& tokens, const std::vector<std::string>& spelling,
    std::shared_ptr<const PerceptionToken> compiled) {
    if (!compiled || compiled->tokens != spelling) compiled = compile_perception_token(spelling);
    tokens.emplace(compiled->id, compiled); ids.emplace(spelling, compiled->id);
    return compiled->id;
}
}
// Load-time interning (equation tokens, restored rows): the one writer of the shared tables.
std::uint64_t GalacticWorld::intern_shared_token(const std::vector<std::string>& tokens,
    std::shared_ptr<const PerceptionToken> compiled) const {
    if (compiled && compiled->tokens == tokens && tokens_.contains(compiled->id)) return compiled->id;
    if (const auto found = token_ids_.find(tokens); found != token_ids_.end()) return found->second;
    return intern_into(token_ids_, tokens_, tokens, std::move(compiled));
}
void GalacticWorld::freeze_perception_tokens(const std::vector<const Equation*>& equations) const {
    const auto lock = query_lock();
    for (const auto* equation : equations)
        if (equation) for (const auto& lookup : equation->lookups)
            if (lookup.tokens.size() >= 2) static_cast<void>(intern_shared_token(lookup.tokens, lookup.compiled));
}
// A read during a pass: the shared tables are read-only, so an unknown token stays in the reader's shard.
std::uint64_t GalacticWorld::intern_token(Shard& owner, const std::vector<std::string>& tokens,
    std::shared_ptr<const PerceptionToken> compiled) const {
    if (compiled && compiled->tokens == tokens && tokens_.contains(compiled->id)) return compiled->id;
    if (const auto found = token_ids_.find(tokens); found != token_ids_.end()) return found->second;
    if (const auto found = owner.late_token_ids.find(tokens); found != owner.late_token_ids.end()) return found->second;
    return intern_into(owner.late_token_ids, owner.late_tokens, tokens, std::move(compiled));
}
std::optional<GalacticBindingId> GalacticWorld::find_bindings(tactical::PlayerId player, const std::vector<Binding>& bindings) const {
    if (bindings.empty()) return GalacticBindingId{};
    auto& owner = shard(player);
    const auto bucket = owner.binding_ids.find(binding_hash(bindings));
    if (bucket == owner.binding_ids.end()) return {};
    for (const auto id : bucket->second)
        if (bindings_equal(owner.bindings[static_cast<std::size_t>(id)], bindings)) return GalacticBindingId{id, id};
    return {};
}
GalacticBindingId GalacticWorld::intern_bindings(tactical::PlayerId player, const std::vector<Binding>& bindings, bool memo) const {
    if (bindings.empty()) return {};
    auto& owner = shard(player);
    auto& values = memo ? owner.memo_bindings : owner.bindings;
    auto& ids = memo ? owner.memo_binding_ids : owner.binding_ids;
    auto& bucket = ids[binding_hash(bindings)];
    std::optional<std::uint64_t> identity;
    for (const auto id : bucket) if (bindings_equal(values[static_cast<std::size_t>(id)], bindings)) {
        if (!identity) identity = id;
        bool exact = true;
        for (std::size_t i = 0; i < bindings.size(); ++i)
            if (const auto* number = std::get_if<Real>(&bindings[i].value);
                number && number->repr != std::get<Real>(values[static_cast<std::size_t>(id)][i].value).repr) exact = false;
        if (exact) return {*identity, id};
    }
    const auto id = static_cast<std::uint64_t>(values.size());
    values.push_back(bindings); bucket.push_back(id); return {identity.value_or(id), id};
}
GalacticInternedKey GalacticWorld::intern_restored_key(const GalacticCacheKey& key) const {
    return {key.player, key.enemy, key.human, key.planet, intern_shared_token(key.tokens), intern_bindings(key.player, key.bindings)};
}
GalacticCacheKey GalacticWorld::expand_key(const GalacticInternedKey& key) const {
    const auto& owner = shard(key.player);
    return {key.player, key.enemy, key.human, key.planet, token_of(owner, key.token).tokens,
        owner.bindings[static_cast<std::size_t>(key.binding.spelling)]};
}
GalacticQueryKey GalacticWorld::query_key(const GalacticInternedKey& key) const {
    const auto& owner = shard(key.player);
    return {key.player, key.enemy, key.human, key.planet, token_of(owner, key.token).tokens,
        owner.bindings[static_cast<std::size_t>(key.binding.spelling)], key.token};
}
void GalacticWorld::index_cache(Shard& owner) const {
    std::size_t capacity = 16;
    while (capacity < owner.cache.size() * 2) capacity *= 2;
    owner.cache_index.assign(capacity, 0);
    for (std::size_t i = 0; i < owner.cache.size(); ++i) {
        auto slot = static_cast<std::size_t>(key_hash(owner.cache[i].key)) & (capacity - 1);
        while (owner.cache_index[slot]) slot = (slot + 1) & (capacity - 1);
        owner.cache_index[slot] = i + 1;
    }
}
GalacticInternedValue* GalacticWorld::cached_value(const GalacticInternedKey& key) const {
    auto& owner = shard(key.player);
    if (owner.cache_index.empty()) index_cache(owner);
    auto slot = static_cast<std::size_t>(key_hash(key)) & (owner.cache_index.size() - 1);
    while (const auto position = owner.cache_index[slot]) {
        if (owner.cache[position - 1].key == key) return &owner.cache[position - 1];
        slot = (slot + 1) & (owner.cache_index.size() - 1);
    }
    return nullptr;
}
GalacticVisibility* GalacticWorld::visibility(tactical::PlayerId owner_id, galactic::PlanetId id, std::uint32_t mask, bool publish) const {
    auto& owner = shard(owner_id);
    if (owner.visibility_index.size() != owner.visibility.size()) {
        owner.visibility_index.clear();
        for (std::size_t index = 0; index < owner.visibility.size(); ++index) {
            const auto& row = owner.visibility[index];
            owner.visibility_index.emplace(std::tuple{row.player, row.planet, row.mask}, index);
        }
    }
    const auto key = std::tuple{owner_id, id, mask};
    if (const auto found = owner.visibility_index.find(key); found != owner.visibility_index.end()) return &owner.visibility[found->second];
    if (!publish) return nullptr;
    ++owner.revision;
    owner.visibility.push_back({owner_id, id, !setup_.uses_fog, 0, mask});
    update_record(owner_id, 2, owner.visibility.size() - 1);
    owner.visibility_index.emplace(key, owner.visibility.size() - 1);
    return &owner.visibility.back();
}

std::optional<Real> GalacticWorld::lookup(const Context& context, const Lookup& lookup,
    const std::vector<Binding>& bindings, ReadMode mode) const {
    if (lookup.tokens.size() < 2) return {};
    auto& owner = shard(context.player);
    const auto token = intern_token(owner, lookup.tokens, lookup.compiled);
    const GalacticQueryKey key{context.player, context.enemy, context.human,
        context.target ? context.target->object : 0, token_of(owner, token).tokens, bindings, token};
    return read(key, mode);
}

std::optional<Real> GalacticWorld::read(const GalacticCacheKey& key, ReadMode mode) const {
    auto& owner = shard(key.player);
    const auto token = intern_token(owner, key.tokens);
    return read(GalacticQueryKey{key.player, key.enemy, key.human, key.planet, token_of(owner, token).tokens, key.bindings, token}, mode);
}
std::optional<Real> GalacticWorld::read(const GalacticQueryKey& key, ReadMode mode) const {
    auto& owner = shard(key.player);
    const auto& compiled = token_of(owner, key.token);
    const auto& token = compiled.chain;
    const auto uncached_value = [&] { return uncached(key, mode); };
    const auto remembered = [&]() -> GalacticInternedValue* {
        const auto binding = find_bindings(key.player, key.bindings);
        return binding ? cached_value({key.player, key.enemy, key.human, key.planet, key.token, *binding}) : nullptr;
    };
    if (compiled.tokens.size() == 2 && token == "MARKUP") return uncached_value(); // WGA-461: retained literal, no remembered cache.
    if (mode == ReadMode::archived) {
        if (const auto* cached = remembered()) return cached->value;
        return uncached_value();
    }
    const auto configured = setup_.cache_intervals.find(token);
    const auto* at = key.planet ? planet(key.planet) : nullptr;
    if (!at || compiled.root != PerceptionToken::Root::target || configured == setup_.cache_intervals.end()) return uncached_value();
    const auto mask = snapshot_->observer() ? galactic_visibility_mask(compiled.tokens) : 0;
    auto* observed = visibility(key.player, key.planet, mask, mode == ReadMode::ordinary);
    GalacticVisibility unseen{key.player, key.planet, false, 0, mask};
    if (!observed) observed = &unseen;
    const bool fogged = token != "ISCONNECTEDTO" && token != "CONNECTSISOLATEDPLANETSBYFORCE"
        && token != "CONNECTSLARGESTISLANDS" && token.find(".NEARBY") == std::string::npos;
    const bool visible = !setup_.uses_fog || !fogged || observed->visible;
    const Real interval = configured->second;
    const auto retain_expiry = [&](std::uint64_t frame) {
        if (mode != ReadMode::ordinary || !visible || !(Real{} < interval)) return;
        const auto expiry = real(static_cast<std::int64_t>(frame)) + interval * real(setup_.logical_fps);
        auto& memo = owner.economy;
        if (!memo.expiry || expiry < *memo.expiry) memo.expiry = expiry;
    };
    if (const auto* cached = remembered(); !owner.refreshing_visibility && cached && cached->frame != 0) {
        // WGA-65: equality at expiry passes; hidden reads use indefinite lookup.
        if (!visible || interval == real(-1) || (interval != Real{} &&
            !(real(static_cast<std::int64_t>(cached->frame)) + interval * real(setup_.logical_fps)
                < real(static_cast<std::int64_t>(synchronized_frame_))))) { retain_expiry(cached->frame); return cached->value; }
    }
    const auto value = uncached_value(); if (!value) return {};
    if (mode != ReadMode::ordinary) return value;
    retain_expiry(synchronized_frame_);
    const GalacticInternedKey retained{key.player, key.enemy, key.human, key.planet, key.token, intern_bindings(key.player, key.bindings)};
    auto* found = cached_value(retained);
    const GalacticInternedValue cached{retained, *value, interval, synchronized_frame_};
    // Numeric binding lookup coalesces signed zeros; canonical bytes retain
    // their spelling and the raw value/interval bits.
    const bool record_changed = !found || found->key != cached.key
        || found->key.binding.spelling != cached.key.binding.spelling
        || found->value.repr != cached.value.repr || found->interval.repr != cached.interval.repr
        || found->frame != cached.frame;
    if (token == "TARGETPLAYERID" && (!found || found->value != *value)) owner.economy.queries.clear();
    if ((token == "TARGETPLAYERID" || token == "ENEMYFORCE.HASSPACEFORCE"
        || token == "ENEMYFORCE.SPACETOTALUNNORMALIZED" || token == "ENEMYFORCE.GROUNDTOTALUNNORMALIZED")
        && (!found || found->value != *value)) ++owner.movement_revision;
    ++owner.revision;
    if (!found) {
        owner.cache.push_back(cached);
        if (owner.cache.size() * 2 > owner.cache_index.size()) index_cache(owner);
        else {
            auto slot = static_cast<std::size_t>(key_hash(retained)) & (owner.cache_index.size() - 1);
            while (owner.cache_index[slot]) slot = (slot + 1) & (owner.cache_index.size() - 1);
            owner.cache_index[slot] = owner.cache.size();
        }
    } else *found = cached;
    if (record_changed) update_record(key.player, 1, found ? static_cast<std::size_t>(found - owner.cache.data()) : owner.cache.size() - 1);
    if (visible && fogged) {
        auto* row = visibility(key.player, key.planet, mask);
        if (row->last_observed != synchronized_frame_) {
            row->last_observed = synchronized_frame_;
            update_record(key.player, 2, static_cast<std::size_t>(row - owner.visibility.data()));
        }
    }
    return value;
}

std::optional<Real> GalacticWorld::uncached(const GalacticQueryKey& key, ReadMode mode) const {
    if (key.tokens.size() < 2 || !snapshot_) return {};
    const auto& token = token_of(shard(key.player), key.token).chain;
    const auto& root = key.tokens.front();
    if ((key.tokens.size() >= 3 && (key.tokens[1] == "FRIENDLYFORCE" || key.tokens[1] == "ENEMYFORCE")
        && !token.ends_with("TIMELASTSEEN") && !token.ends_with("TIMELASTSEENUNNORMALIZED"))
        || token == "ISCONNECTEDTO" || token == "CONNECTSISOLATEDPLANETSBYFORCE" || token == "CONNECTSLARGESTISLANDS")
        return force_token(key, mode);
    if (root == "GAME") {
        if (token == "TIMELINE") return economy_runtime_ && economy_runtime_->timeline
            && economy_runtime_->completed_tick == snapshot_->completed_tick()
            ? std::optional{real(*economy_runtime_->timeline)} : std::nullopt; // WGA-295/#2701
        if (token == "AGE") return fraction(real(static_cast<std::int64_t>(snapshot_->clock().galactic_frame)), real(setup_.logical_fps));
        if (token == "ISCAMPAIGNGAME") return real(1);
        if (token == "ISSTORYCAMPAIGN") return setup_.is_story_campaign
            ? std::optional{boolean_value(*setup_.is_story_campaign)} : std::nullopt; // WGA-423.
        if (token == "PLANETSCORRUPTED" || token == "PLANETSCORRUPTEDUNNORMALIZED") return corruption_total_token(key, token, mode); // WGA-487
        return {};
    }
    if (root == "VARIABLE_SELF" || root == "VARIABLE_ENEMY" || root == "VARIABLE_HUMAN") {
        std::optional<tactical::PlayerId> id = key.player;
        if (root == "VARIABLE_ENEMY") id = key.enemy;
        if (root == "VARIABLE_HUMAN") id = key.human;
        const auto* who = id ? player(*id) : nullptr; if (!who) return {};
        if (token == "MARKUP") return markup_->value({key.player, true, *id}); // WGA-461.
        static const std::set<std::string, std::less<>> economy_tokens{
            "MAXEDSTARBASES", "MAXEDGROUNDBASES", "HASSTARBASEOFLEVEL", "HASGROUNDBASEOFLEVEL",
            "NUMSTARBASEOFLEVEL", "NUMGROUNDBASEOFLEVEL", "HASSTRUCTURE", "STRUCTURECOUNT",
            "HASUNIT", "HASTECHTOPRODUCE", "CANADVANCETECH", "MAINTENANCE",
            "MAINTENANCEUNNORMALIZED", "GROSSINCOMEUNNORMALIZED", "NETINCOMEUNNORMALIZED",
            "BUDGETFRACTIONTOBUILD", "SPACETOTALUNNORMALIZED", "GROUNDTOTALUNNORMALIZED",
            "PLANETSCONTROLLED", "PLANETSCONTROLLEDUNNORMALIZED"};
        if (economy_tokens.contains(token)) return player_economy_token(key, *id, token, mode);
        if (token == "AVERAGEAGEOFSPACEINTELLIGENCE" || token == "AVERAGEAGEOFGROUNDINTELLIGENCE") {
            // WGA-263: selected evaluator only, all nonallied planet targets,
            // normalized enemy-force age; no new wall clock or observation store.
            if (*id != key.player) return {};
            Real sum{}; std::int64_t count{};
            for (const auto& row : planets_) {
                auto owner = row.planet.owner;
                if (snapshot_->observer()) for (const auto& facts : snapshot_->observer()->planets)
                    if (facts.planet == row.planet.id) { owner = facts.allegiance; break; }
                if (allied(*id, owner)) continue;
                auto nested = copy_key(key); nested.planet = row.planet.id;
                nested.tokens = {"VARIABLE_TARGET", "ENEMYFORCE",
                    token == "AVERAGEAGEOFSPACEINTELLIGENCE" ? "SPACETOTAL" : "GROUNDTOTAL", "TIMELASTSEEN"};
                nested.bindings.clear();
                const auto age = read(nested, mode); if (!age) return {};
                sum = sum + *age; ++count;
            }
            return count ? sum / real(count) : Real{};
        }
        if (token == "WORSTISLANDFRACTIONOFLARGEST") {
            // WGA-476: only the manager's own player reads the pair; a foreign evaluator returns zero. Second over
            // largest component size, with the -1 reset sentinel (fewer than two components) and a zero size giving zero.
            if (*id != key.player) return Real{};
            const auto sizes = std::find_if(connectivity_->island_sizes.begin(), connectivity_->island_sizes.end(),
                [&](const auto& row) { return row.player == key.player; });
            if (sizes == connectivity_->island_sizes.end()) return {};
            if (sizes->largest <= 0 || sizes->second <= 0) return Real{};
            return real(static_cast<std::int64_t>(sizes->second)) / real(static_cast<std::int64_t>(sizes->largest));
        }
        if (token == "ANYCURRENTTHREATS") {
            // WGA-474: a human evaluator is never threatened. Otherwise the AI is threatened when a human-owned planet that
            // has a human fleet attached has any neighbourhood planet (WGA-192) owned by the evaluated player.
            if (who->human) return Real{};
            if (!setup_.maximum_fleet_distance) return {};
            for (const auto& target : planets_) {
                const auto* holder = player(target.planet.owner);
                if (!holder || !holder->human) continue;
                const auto attached = std::any_of(snapshot_->fleets().begin(), snapshot_->fleets().end(), [&](const auto& fleet) {
                    const auto* fleet_owner = player(fleet.owner);
                    return fleet.orbit == target.planet.id && !fleet.motion.transit && fleet_owner && fleet_owner->human;
                });
                if (!attached) continue;
                for (const auto& near : target.nearby) {
                    const auto* neighbour = planet(near.planet);
                    if (neighbour && neighbour->planet.owner == *id) return real(1);
                }
            }
            return Real{};
        }
        if (token == "ISFACTION") return boolean_value(parameter_matches(key.bindings, "PARAMETER_FACTION", who->faction));
        if (token == "ISDIFFICULTY") {
            if (who->human || !who->ai) return Real{}; // WGA-424: no AI-controlled seat difficulty.
            const auto value = parameter_number(key.bindings, "PARAMETER_DIFFICULTY_LEVEL_TYPE");
            if (!value) return {};
            const Real selected = who->difficulty == "EASY" ? real(0) : who->difficulty == "NORMAL" ? real(1) : who->difficulty == "HARD" ? real(2) : real(-1);
            return boolean_value(*value == selected);
        }
        if (token == "CREDITSUNNORMALIZED") { const auto* row = account(*id); return row ? std::optional{numeric::from_fixed(row->credits)} : std::nullopt; }
        if (token == "TECHLEVEL") { const auto* row = account(*id); return row ? std::optional{real(row->tech_level)} : std::nullopt; }
        if (token == "HOMEPLANET.TYPE.TOKEN") { const auto* home = planet(who->home); return home && home->facts.type ? std::optional{real(static_cast<std::int64_t>(home->facts.type))} : std::nullopt; }
        return {};
    }
    if (root != "VARIABLE_TARGET") return {};
    const auto* row = planet(key.planet); if (!row) return {};
    if (token == "MARKUP") return markup_->value({key.player, false, key.planet}); // WGA-461.
    const auto& production = row->production;
    if (token == "TARGETPLAYERID") {
        // WGA-258: ownership visibility controls caching; the live getter reads
        // the allegiance's target player, independently of political owner.
        if (snapshot_->observer()) for (const auto& facts : snapshot_->observer()->planets)
            if (facts.planet == key.planet) return real(facts.target_owner);
        return {};
    }
    if (token == "TYPE.TOKEN") return row->facts.type ? std::optional{real(static_cast<std::int64_t>(row->facts.type))} : std::nullopt;
    if (token == "TYPE.ISTYPE") { const auto type = parameter_text(key.bindings, "PARAMETER_TYPE"); return type ? std::optional{boolean_value(*type == row->facts.name)} : std::nullopt; }
    if (token == "ISFACTION") { const auto* who = player(row->planet.owner); return who ? std::optional{boolean_value(parameter_matches(key.bindings, "PARAMETER_FACTION", who->faction))} : std::nullopt; }
    if (token == "ISHUMANCONTROLLED") { const auto* who = player(row->planet.owner); return who ? std::optional{boolean_value(who->human)} : std::nullopt; }
    if (token == "BASEINCOME") return row->facts.base_income;
    if (token == "BASEINCOMENBTP") {
        if (!economy_runtime_ || economy_runtime_->completed_tick != snapshot_->completed_tick()) return {};
        const auto selected = economy_runtime_->base_credits.find(key.planet);
        if (selected == economy_runtime_->base_credits.end()) return {};
        auto owner_key = copy_key(key); owner_key.tokens = {"VARIABLE_TARGET", "TARGETPLAYERID"};
        owner_key.bindings.clear();
        const auto owner = read(owner_key, mode); if (!owner) return {};
        std::int64_t maximum{};
        for (const auto& at : planets_) {
            owner_key.planet = at.planet.id;
            const auto controller = read(owner_key, mode); if (!controller) return {};
            if (static_cast<std::int32_t>(*controller) != static_cast<std::int32_t>(*owner)) continue;
            const auto credits = economy_runtime_->base_credits.find(at.planet.id);
            if (credits == economy_runtime_->base_credits.end()) return {};
            maximum = std::max(maximum, credits->second);
        }
        // WGA-449: binary64 ratio and clamp, without the BaseIncome reader's
        // binary32 normalization or any hypothetical economic multiplier.
        if (!maximum) return Real{};
        const auto ratio = real(selected->second) / real(maximum);
        return std::clamp(ratio, Real{}, real(1));
    }
    if (token == "HASCREDITSIPHON" || token == "ISCORRUPTED") {
        const auto* economy = snapshot_->economy_corruption();
        if (!economy) return {};
        const auto found = std::find_if(economy->planets.begin(), economy->planets.end(),
            [&](const auto& at) { return at.planet == key.planet; });
        if (found == economy->planets.end()) return {};
        // WGA-450/451: read committed owning facts; duration and pending
        // corruption transitions do not change either predicate.
        return boolean_value(token == "HASCREDITSIPHON" ? found->siphon.active
            : found->corruption != galactic::corruption_fixture::Type::none);
    }
    if (token == "ISCONNECTEDTOCORRUPTION") {
        // WGA-473: any planet of the evaluator's neighbourhood (WGA-192, source planet included) is corrupted.
        const auto* economy = snapshot_->economy_corruption();
        if (!economy || !setup_.maximum_fleet_distance) return {};
        const auto corrupted = [&](galactic::PlanetId id) {
            const auto found = std::find_if(economy->planets.begin(), economy->planets.end(), [&](const auto& at) { return at.planet == id; });
            return found == economy->planets.end() ? std::optional<bool>{} : std::optional{found->corruption != galactic::corruption_fixture::Type::none};
        };
        for (const auto& near : row->nearby) {
            const auto state = corrupted(near.planet); if (!state) return {};
            if (*state) return real(1);
        }
        return Real{};
    }
    if (token == "RETAINSRESIDUALINFLUENCE") return boolean_value(row->facts.retains_residual_influence);
    if (token == "ISCORRUPTIONTRANSITIONACTIVE" || token == "TIMESINCECORRUPTIONCHANGE") {
        // WGA-488: the planet's committed transition row; a planet that never changed has last change zero.
        const auto transitions = snapshot_->corruption_transitions(); // ascending by planet
        const auto* found = sorted_find(transitions, key.planet, [](const auto& at) { return at.planet; });
        if (token == "ISCORRUPTIONTRANSITIONACTIVE") return boolean_value(found && found->transition.active());
        const auto last_change = found ? found->transition.last_change : std::uint64_t{};
        const auto frame = snapshot_->clock().galactic_frame;
        // Single-precision frames-to-seconds, as the debug build converts.
        return to_single(real(static_cast<std::int64_t>(frame) - static_cast<std::int64_t>(last_change))
            * to_single(real(1) / real(setup_.logical_fps)));
    }
    if (token == "BLACKMARKETPRICEMODIFIER" || token == "BLACKMARKETABILITIESAVAILABLE" || token == "BLACKMARKETMINIMUMABILITYPRICE") {
        // WGA-488: the price modifier is the planet type's own; the other two read the authored market items of
        // the planet owner's faction that the named faction's player has not unlocked yet.
        const auto* economy = snapshot_->economy_corruption();
        if (!economy) return {};
        const auto* planet_row = sorted_find(economy->planets, key.planet, [](const auto& at) { return at.planet; }); // ascending
        if (!planet_row) return {};
        const auto modifier = numeric::from_fixed(planet_row->market_modifier);
        if (token == "BLACKMARKETPRICEMODIFIER") return to_single(modifier);
        if (!has_text_parameter(key.bindings, "PARAMETER_FACTION")) return {};
        const galactic::CorruptionEconomyPlayer* buyer = nullptr; // no per-player allocation
        for (const auto& at : economy->players) { const auto* facts = player(at.player); if (facts && parameter_equals_nocase(key.bindings, "PARAMETER_FACTION", facts->faction)) { buyer = &at; break; } }
        if (!buyer) return {};
        const auto* owner = sorted_find(economy->players, row->planet.owner, [](const auto& at) { return at.player; }); // ascending
        const bool market = planet_row->corruption == galactic::corruption_fixture::Type::black_market;
        if (token == "BLACKMARKETABILITIESAVAILABLE" && !market) return Real{};
        if (!owner) return {};
        const auto* owner_account = account(owner->player); if (!owner_account) return {};
        static const Real very_large = parse_real("1000000000000000000").value(); // the debug build's start value
        Real cheapest = very_large; bool available{};
        for (const auto& item : economy->items) {
            if (item.faction != owner->faction || buyer->unlocks.contains(item.id)) continue;
            available = true;
            auto cost = numeric::from_fixed(item.base_cost);
            if (static_cast<std::int64_t>(owner_account->tech_level) < static_cast<std::int64_t>(item.tech)) cost = cost * numeric::from_fixed(item.uncommon_multiplier);
            if (cost < cheapest) cheapest = cost;
        }
        if (token == "BLACKMARKETABILITIESAVAILABLE") return boolean_value(available);
        return cheapest + to_single(modifier);
    }
    if (token == "TARGETPOLITICALCONTROL") {
        // WGA-475: truncated magnitude of the target allegiance, signed by whether the target player is the evaluating
        // AI, divided by the authored maximum political control. The live getter reads the allegiance, not the political owner.
        if (!snapshot_->observer() || !setup_.maximum_political_control) return {};
        const auto observed = std::find_if(snapshot_->observer()->planets.begin(), snapshot_->observer()->planets.end(),
            [&](const auto& facts) { return facts.planet == key.planet; });
        if (observed == snapshot_->observer()->planets.end()) return {};
        const auto magnitude = static_cast<std::int64_t>(std::abs(observed->political_control.raw() / sim::math::Fixed::scale));
        return real(observed->target_owner == key.player ? magnitude : -magnitude) / *setup_.maximum_political_control;
    }
    if (token == "MAXSTRUCTURESLOTS") return row->facts.maximum_structure_slots;
    if (token == "ACTIVETRADEROUTES") return fraction(real(static_cast<std::int64_t>(row->active_routes)), real(static_cast<std::int64_t>(row->routes))); // WGA-62
    if (token == "TRADEROUTES") {
        std::uint64_t maximum{}; for (const auto& at : planets_) maximum = std::max(maximum, at.routes);
        return fraction(real(static_cast<std::int64_t>(row->routes)), real(static_cast<std::int64_t>(maximum)));
    }
    if (token == "INCOMEUNNORMALIZED") return row->income;
    if (token == "INCOME") {
        if (!row->income) return {};
        Real maximum{}; for (const auto& at : planets_) { if (!at.income) return {}; if (maximum < *at.income) maximum = *at.income; }
        return fraction(*row->income, maximum);
    }
    if (token.starts_with("HINTS.")) {
        for (const auto& hint : shard(key.player).hints) if (hint.player == key.player && hint.planet == row->facts.name && hint.name == token.substr(6)) return hint.value;
        return markup_->hint(key.player, row->facts.name, token.substr(6)); // WGA-428/464.
    }
    if (token == "STORYTRIGGER") {
        const auto name = parameter_text(key.bindings, "PARAMETER_TRIGGER"); if (!name) return {};
        const auto& flags = shard(key.player).flags;
        return boolean_value(std::any_of(flags.begin(), flags.end(), [&](const auto& flag) { return flag.player == key.player && flag.planet == key.planet && flag.name == *name; }));
    }
    static const std::set<std::string> ages{"ENEMYFORCE.GROUNDTOTAL.TIMELASTSEEN", "ENEMYFORCE.GROUNDTOTAL.TIMELASTSEENUNNORMALIZED",
        "ENEMYFORCE.SPACETOTAL.TIMELASTSEEN", "ENEMYFORCE.SPACETOTAL.TIMELASTSEENUNNORMALIZED",
        "FRIENDLYFORCE.GROUNDTOTAL.TIMELASTSEEN", "FRIENDLYFORCE.GROUNDTOTAL.TIMELASTSEENUNNORMALIZED",
        "FRIENDLYFORCE.SPACETOTAL.TIMELASTSEEN", "FRIENDLYFORCE.SPACETOTAL.TIMELASTSEENUNNORMALIZED"};
    if (ages.contains(token)) {
        const auto* seen = visibility(key.player, key.planet, snapshot_->observer() ? galactic_visibility_mask(key.tokens) : 0, mode == ReadMode::ordinary);
        if (!setup_.uses_fog || (seen && seen->visible)) return Real{};
        const auto last_observed = seen ? seen->last_observed : 0;
        const auto delta = synchronized_frame_ >= last_observed ? synchronized_frame_ - last_observed : 0;
        return fraction(real(static_cast<std::int64_t>(delta)), token.ends_with("UNNORMALIZED") ? real(setup_.logical_fps) : real(static_cast<std::int64_t>(synchronized_frame_))); // WGA-65
    }
    if (!production) return {};
    if (token == "GROUNDBASELEVELUNNORMALIZED") return real(production->ground_level);
    if (token == "STARBASELEVELUNNORMALIZED") return real(production->space_level);
    if (token == "GROUNDBASELEVEL") return setup_.maximum_ground_level ? std::optional{real(production->ground_level) / *setup_.maximum_ground_level} : std::nullopt;
    if (token == "STARBASELEVEL") return setup_.maximum_space_level ? std::optional{real(production->space_level) / *setup_.maximum_space_level} : std::nullopt;
    if (token == "MAXGROUNDBASELEVEL") return setup_.maximum_ground_level ? std::optional{real(std::min<std::int64_t>(production->ground_slots, 5)) / *setup_.maximum_ground_level} : std::nullopt;
    if (token == "MAXSTARBASELEVEL") return setup_.maximum_space_level ? std::optional{real(production->maximum_space_level) / *setup_.maximum_space_level} : std::nullopt;
    if (token == "ISSURFACEACCESSIBLE") return boolean_value(production->surface_accessible);
    if (token == "MAXSTRUCTURESLOTSUNNORMALIZED") return real(production->ground_slots);
    if (token == "OPENCOMPANYSLOTSUNNORMALIZED") return real(production->maximum_ground_forces - production->landed_transports); // WGA-62 raw, including negatives
    if (token == "STRUCTURECOUNT" || token == "HASSTRUCTURE"
        || token == "HASSTARBASEOFLEVEL" || token == "HASGROUNDBASEOFLEVEL") return target_economy_token(key, token);
    if (token == "OPENSTRUCTURESLOTS") {
        std::int64_t count{};
        for (const auto& object : production->objects) if (object.kind == galactic::ProductionKind::structure && object.domain == galactic::ProductionDomain::ground) {
            ++count;
        }
        return production->ground_slots == 0 ? Real{} : to_single(real(1) - fraction(real(count), real(production->ground_slots))); // WGA-62
    }
    return {}; // U-01: no fabricated value for an unqualified token.
}

void GalacticWorld::finish_perception_queries() const {
    const auto lock = query_lock();
    // This is a reclamation threshold, not a limit on authored bindings. The
    // native record set defines the retained storage; spare growth amortizes GC.
    for (auto& [who, owner] : shards_) {
        if (owner.bindings.size() <= owner.cache.size() * 2 + 256) continue;
        auto previous = std::move(owner.bindings);
        owner.bindings.clear(); owner.bindings.emplace_back(); owner.binding_ids.clear();
        for (auto& row : owner.cache) row.key.binding = intern_bindings(who, previous[static_cast<std::size_t>(row.key.binding.spelling)]);
        owner.cache_index.clear();
    }
}

GalacticPerceptionState GalacticWorld::state() const {
    const auto lock = query_lock();
    GalacticPerceptionState out;
    for (const auto& [who, owner] : shards_) {
        static_cast<void>(who);
        out.visibility.insert(out.visibility.end(), owner.visibility.begin(), owner.visibility.end());
        out.flags.insert(out.flags.end(), owner.flags.begin(), owner.flags.end());
        out.hints.insert(out.hints.end(), owner.hints.begin(), owner.hints.end());
        out.cache.reserve(out.cache.size() + owner.cache.size());
        for (const auto& row : owner.cache) out.cache.push_back({expand_key(row.key), row.value, row.interval, row.frame});
    }
    // Parallel reads may insert in any order; native state publishes canonical keys.
    std::sort(out.cache.begin(), out.cache.end(), [](const auto& a, const auto& b) { return key_less(a.key, b.key); });
    std::sort(out.visibility.begin(), out.visibility.end(), [](const auto& a, const auto& b) { return std::tie(a.player, a.planet, a.mask) < std::tie(b.player, b.planet, b.mask); });
    std::sort(out.flags.begin(), out.flags.end(), [](const auto& a, const auto& b) { return std::tie(a.player, a.planet, a.name) < std::tie(b.player, b.planet, b.name); });
    std::sort(out.hints.begin(), out.hints.end(), [](const auto& a, const auto& b) { return std::tie(a.player, a.planet, a.name) < std::tie(b.player, b.planet, b.name); });
    return out;
}
Real GalacticWorld::force_cache_deadline(tactical::PlayerId owner, std::uint32_t logical_fps, bool fog) const {
    const auto lock = query_lock();
    auto through = Real(std::numeric_limits<std::int64_t>::max());
    const auto& mine = shard(owner);
    for (const auto& cached : mine.cache) {
        const auto& tokens = token_of(mine, cached.key.token).tokens;
        if (cached.key.player != owner || tokens.size() != 3
            || tokens[1] != "ENEMYFORCE"
            || (tokens[2] != "HASSPACEFORCE" && tokens[2] != "SPACETOTALUNNORMALIZED"
                && tokens[2] != "GROUNDTOTALUNNORMALIZED")) continue;
        if (fog) {
            const auto* seen = visibility(owner, cached.key.planet, galactic_visibility_mask(tokens), false);
            if (!seen) {
                // state() orders visibility by mask. Its first legacy fallback
                // is the smallest mask, regardless of native insertion order.
                const auto first = mine.visibility_index.lower_bound(std::tuple{owner, cached.key.planet, std::uint32_t{}});
                if (first != mine.visibility_index.end()
                    && std::get<0>(first->first) == owner && std::get<1>(first->first) == cached.key.planet)
                    seen = &mine.visibility[first->second];
            }
            if (!seen || !seen->visible) continue;
        }
        if (cached.interval == Real(-1) || cached.interval == Real{}) continue;
        through = std::min(through, cached.frame == 0 ? Real{} : Real(cached.frame) + cached.interval * Real(logical_fps));
    }
    return through;
}
core::Result<void> GalacticWorld::restore(GalacticPerceptionState state) {
    // F validates the whole container; reject duplicate component identities here.
    std::set<GalacticCacheKey> cache_keys;
    std::set<std::tuple<tactical::PlayerId, galactic::PlanetId, std::uint32_t>> visibility_keys;
    std::set<std::tuple<tactical::PlayerId, galactic::PlanetId, std::string>> flag_keys;
    std::set<std::tuple<tactical::PlayerId, std::string, std::string>> hint_keys;
    for (std::size_t i = 0; i < state.cache.size(); ++i) {
        const auto& row = state.cache[i];
        const auto interval = setup_.cache_intervals.find(compile_perception_token(row.key.tokens)->chain);
        if (!player(row.key.player) || !planet(row.key.planet) || (row.key.enemy && !player(*row.key.enemy))
            || (row.key.human && !player(*row.key.human)) || row.interval < real(-1)
            || row.frame > synchronized_frame_ || row.key.tokens.size() < 2 || row.key.tokens.front() != "VARIABLE_TARGET"
            || interval == setup_.cache_intervals.end() || interval->second != row.interval
            || (row.value.repr & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL
            || (row.interval.repr & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL)
            return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "invalid galactic perception cache state"));
        for (const auto& token : row.key.tokens) if (token.empty() || upper_case(token) != token)
            return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "invalid galactic cache token identity"));
        for (const auto& binding : row.key.bindings) {
            if (binding.parameter.empty() || binding.parameter != upper_case(binding.parameter))
                return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "invalid galactic cache parameter identity"));
            if (const auto* number = std::get_if<Real>(&binding.value); number && (number->repr & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL)
                return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "invalid galactic cache parameter value"));
        }
        if (!cache_keys.insert(row.key).second)
            return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "duplicate galactic perception cache binding"));
    }
    for (std::size_t i = 0; i < state.visibility.size(); ++i) {
        const auto& row = state.visibility[i];
        if (!player(row.player) || !planet(row.planet) || row.last_observed > synchronized_frame_) return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "invalid galactic visibility identity"));
        if (row.mask != 0 && row.mask != galactic::bit(galactic::Intelligence::ownership)
            && row.mask != galactic::bit(galactic::Intelligence::fleet_presence)
            && row.mask != galactic::bit(galactic::Intelligence::fleet_count)
            && row.mask != galactic::bit(galactic::Intelligence::ground_count))
            return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "unqualified visibility mask"));
        if (!visibility_keys.emplace(row.player, row.planet, row.mask).second)
            return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "duplicate galactic visibility identity"));
    }
    for (std::size_t i = 0; i < state.flags.size(); ++i) {
        auto& row = state.flags[i]; row.name = upper_case(row.name);
        if (!player(row.player) || (row.planet && !planet(row.planet))) return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "invalid galactic story identity"));
        if (!flag_keys.emplace(row.player, row.planet, row.name).second)
            return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "duplicate galactic story identity"));
    }
    for (std::size_t i = 0; i < state.hints.size(); ++i) {
        auto& row = state.hints[i]; row.planet = upper_case(row.planet); row.name = upper_case(row.name);
        if (!player(row.player)) return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "invalid galactic hint player"));
        if (!hint_keys.emplace(row.player, row.planet, row.name).second)
            return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "duplicate galactic hint identity"));
    }
    const auto lock = query_lock(); mark_state_changed(); ++movement_input_revision_;
    for (auto& [who, owner] : shards_) {
        static_cast<void>(who);
        // Restoring replaces every shard; the late-token overlay stays (its tokens are derived metadata).
        owner.cache.clear(); owner.cache_index.clear();
        owner.bindings.clear(); owner.bindings.emplace_back(); owner.binding_ids.clear();
        owner.memo_bindings.clear(); owner.memo_bindings.emplace_back(); owner.memo_binding_ids.clear();
        owner.memo_bindings_frame.reset();
        owner.visibility.clear(); owner.flags.clear(); owner.hints.clear(); owner.visibility_index.clear();
        owner.economy = {}; owner.records = {};
    }
    for (const auto& row : state.cache) {
        auto& owner = shard(row.key.player);
        owner.cache.push_back({intern_restored_key(row.key), row.value, row.interval, row.frame});
    }
    for (auto& row : state.visibility) shard(row.player).visibility.push_back(row);
    for (auto& row : state.flags) shard(row.player).flags.push_back(std::move(row));
    for (auto& row : state.hints) shard(row.player).hints.push_back(std::move(row));
    rebuild_records(); records_ready_ = true;
    return core::Result<void>::success();
}
bool GalacticWorld::insert_hint(GalacticHint hint) {
    hint.planet = upper_case(hint.planet); hint.name = upper_case(hint.name);
    const auto lock = query_lock();
    auto& owner = shard(hint.player);
    for (const auto& row : owner.hints) if (row.player == hint.player && row.planet == hint.planet && row.name == hint.name) return false;
    ++owner.revision;
    const auto holder = hint.player;
    owner.hints.push_back(std::move(hint)); update_record(holder, 4, owner.hints.size() - 1); return true;
}
void GalacticWorld::set_story_flag(tactical::PlayerId owner, galactic::PlanetId id, std::string name) {
    // The shared equation parser canonicalizes text parameters. Retail trigger
    // case identity remains a qualification seam; all component paths agree.
    name = upper_case(name);
    const auto lock = query_lock();
    auto& mine = shard(owner);
    for (const auto& row : mine.flags) if (row.player == owner && row.planet == id && row.name == name) return;
    ++mine.revision;
    mine.flags.push_back({owner, id, std::move(name)}); update_record(owner, 3, mine.flags.size() - 1);
}
bool GalacticWorld::check_story_flag(tactical::PlayerId owner, galactic::PlanetId id, std::string_view name, bool clear) {
    const auto canonical = upper_case(name);
    const auto lock = query_lock();
    auto& mine = shard(owner);
    const auto found = std::find_if(mine.flags.begin(), mine.flags.end(), [&](const auto& flag) { return flag.player == owner && flag.planet == id && flag.name == canonical; });
    if (found == mine.flags.end()) return false;
    if (clear) {
        ++mine.revision;
        const auto index = static_cast<std::size_t>(found - mine.flags.begin());
        if (records_ready_) {
            mine.records.erase(found->player, 3, index);
            if (index + 1 != mine.flags.size()) mine.records.erase(mine.flags.back().player, 3, mine.flags.size() - 1);
        }
        if (index + 1 != mine.flags.size()) { *found = std::move(mine.flags.back()); update_record(owner, 3, index); }
        mine.flags.pop_back();
    }
    return true;
}
void GalacticWorld::visibility_changed(tactical::PlayerId owner, galactic::PlanetId id, bool visible, std::uint32_t mask) {
    const auto lock = query_lock();
    clear_economy_memos();
    ++movement_input_revision_;
    mark_state_changed();
    auto& mine = shard(owner);
    auto* observed = visibility(owner, id, mask);
    if (visible || observed->visible) {
        mine.refreshing_visibility = true;
        std::vector<GalacticInternedKey> keys;
        for (const auto& cached : mine.cache) if (cached.key.player == owner && cached.key.planet == id
            && (mask == 0 || galactic_visibility_mask(token_of(mine, cached.key.token).tokens) == mask)) keys.push_back(cached.key);
        for (const auto& key : keys) if (const auto value = uncached(query_key(key))) {
            if (auto* cached = cached_value(key)) { cached->value = *value; cached->frame = synchronized_frame_; update_record(owner, 1, static_cast<std::size_t>(cached - mine.cache.data())); }
        }
        visibility(owner, id, mask)->last_observed = synchronized_frame_;
        mine.refreshing_visibility = false;
    }
    auto* retained = visibility(owner, id, mask); retained->visible = visible;
    update_record(owner, 2, static_cast<std::size_t>(retained - mine.visibility.data()));
}
} // namespace eawr::script::foc::ai
