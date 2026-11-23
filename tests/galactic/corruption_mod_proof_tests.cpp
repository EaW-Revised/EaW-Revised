#include <algorithm>
#include <chrono>
#include <cstddef>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <typeinfo>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif
#include "../support/env.hpp"
#include "../support/result.hpp"
#include "../support/temp_dir.hpp"
#include "../presentation/galactic/stock_hero_place.hpp"
#include "galactic_support.hpp"
#include "galactic/map_input.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/setup.hpp"
#include "eawr/campaign/tactical_return.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/assets/assets.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/agreed_campaign.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

// #7753 (corruption PR 3): the corruption system proven end to end on installed mod data, headless, through the live
// session and the viewer's own drop preview and order path (as the stock corruption tests do).
//   raw-chain   Republic at War: the diplomacy hero in her transport corrupts planets: price charged, intimidation use
//               counters, a named hero consumed to the respawn timer, the 5 s transition, the one-time bonus and the
//               racketeering upgrade of the corrupting faction.
//   raw-own     a hero of the corrupting faction on its own planet takes nothing off the default trade agreement.
//   raw-info    the information menu: authored slots on a slotted planet, empty and valid on a slotless planet; no debit.
//   remake      corruption stays dormant: no beneficiary, no slots, no icons, no tiles, nothing admitted.
//   bfc         the information menu opens on a slotted planet; the corrupted-planet bribe modifier is read.
// No planet or price is named for the assertions: they come from the authored data the session loaded. The corruption heroes
// (the diplomacy hero, the named senator) and their companies are named, since the data gives no other way to pick them.
namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
namespace view = eawr::viewer::galactic;
namespace fixture = eawr::sim::galactic::corruption_fixture;
using Fixed = eawr::sim::math::Fixed;
using galactic_test::expect;
using eawr::testing::result_diagnostic::take;
using eawr::testing::galactic::find_hero_place;

struct Mod {
    std::string id, campaign_file, campaign;
    bool overlay{};
    bool remake_chain{}; // the Remake content as the owner plays it: the whole chain, leaf first (EAWR_REMAKE_MOD_CHAIN)
};

struct MissingMod : std::runtime_error { using std::runtime_error::runtime_error; };

// The workshop folders searched the way the AI day test does it.
std::vector<eawr::vfs::MountSpec> mount_mod(const Mod& mod) {
    const auto& mod_id = mod.id;
    const auto root = eawr::testing::game_root();
    if (!root) throw MissingMod("set EAWR_EAW_GAME_ROOT and EAWR_REMAKE_MOD_ROOT");
    const std::filesystem::path game(*root);
    std::vector<eawr::vfs::LayerRoot> layers;
    std::vector<std::filesystem::path> roots;
    if (mod.remake_chain) {
        // Mounting only the base item would prove dormancy on content the leaf layers override.
        const auto chain = eawr::testing::remake_mount();
        if (!chain || !chain->chain) throw MissingMod("Remake chain not configured (EAWR_REMAKE_MOD_CHAIN)");
        layers = eawr::vfs::mod_chain_roots(std::filesystem::path(chain->mod_root));
    }
    if (const auto workshop = eawr::testing::environment("EAWR_WORKSHOP_ROOT")) roots.emplace_back(*workshop);
    if (const auto mod = eawr::testing::environment("EAWR_REMAKE_MOD_ROOT")) roots.push_back(std::filesystem::path(*mod).parent_path());
    roots.push_back(game.parent_path().parent_path() / "workshop" / "content" / "32470");
    roots.push_back(game.parent_path() / "workshop" / "content" / "32470");
    roots.push_back(game.parent_path().parent_path() / "mods" / "workshop" / "content" / "32470");
    for (const auto& workshop : roots) {
        if (!layers.empty()) break;
        std::error_code unreadable;
        if (std::filesystem::is_directory(workshop / mod_id, unreadable) && !unreadable) {
            layers = eawr::vfs::mod_chain_roots(workshop / mod_id);
            break;
        }
    }
    if (layers.empty()) throw MissingMod("workshop mod " + mod_id + " is not installed on this host");
    layers.emplace_back("expansion", game / "corruption" / "Data");
    layers.emplace_back("base", game / "GameData" / "Data");
    std::vector<eawr::vfs::MountSpec> mounts;
    for (auto& layer : take(eawr::vfs::resolve_manifest_chain(layers))) mounts.push_back(std::move(layer.mount));
    return mounts;
}

std::string reject_text(const std::vector<gc::GalacticEvent>& rejected) {
    std::string text;
    for (const auto& event : rejected)
        text += " [tick " + std::to_string(event.tick) + " " + std::string(gc::to_string(event.reason))
            + " subject=" + std::to_string(event.subject) + "]";
    return text.empty() ? " none" : text;
}

// The live campaign of one mod's galactic game, its first human seat, and the helpers the proofs share.
class Rig {
public:
    // `companies` join the human faction at its first starting planet (the sandbox gives no corruption heroes).
    Rig(const Mod& mod, const std::vector<std::string>& companies, const std::string& label)
        : label_(label) {
        auto mounts = mount_mod(mod);
        const auto base = take(eawr::vfs::Vfs::mount(mounts));
        std::filesystem::path directory;
        if (!companies.empty()) {
            const auto bytes = take(base.open("data/xml/" + mod.campaign_file));
            auto xml = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            const auto campaign = xml.find("<Campaign Name=\"" + mod.campaign + "\">");
            const auto active = campaign == std::string::npos ? campaign : xml.find("<Starting_Active_Player>", campaign);
            if (active == std::string::npos) throw std::runtime_error("missing campaign or active player: " + mod.campaign);
            std::string faction;
            for (auto at = active + std::string_view("<Starting_Active_Player>").size(); xml[at] != '<'; ++at)
                if (xml[at] != ' ' && xml[at] != '\t' && xml[at] != '\r' && xml[at] != '\n') faction += xml[at];
            std::size_t forces = xml.find("<Starting_Forces>", active);
            for (; forces != std::string::npos; forces = xml.find("<Starting_Forces>", forces + 1)) {
                auto at = forces + std::string_view("<Starting_Forces>").size();
                while (xml[at] == ' ' || xml[at] == '\t') ++at;
                if (xml.compare(at, faction.size() + 1, faction + ",") == 0) break;
            }
            if (forces == std::string::npos) throw std::runtime_error("the active faction has no starting forces");
            auto at = forces + std::string_view("<Starting_Forces>").size();
            while (xml[at] == ' ' || xml[at] == '\t') ++at;
            at += faction.size() + 1;
            std::string home;
            for (; xml[at] != ','; ++at) if (xml[at] != ' ' && xml[at] != '\t') home += xml[at];
            std::string added;
            for (const auto& company : companies)
                added += "<Starting_Forces> " + faction + ", " + home + ", " + company + " </Starting_Forces>\n";
            xml.insert(forces, added);
            directory_ = std::filesystem::current_path() / ("corruption-mod-proof-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(directory_ / "xml");
            std::ofstream out(directory_ / "xml" / mod.campaign_file, std::ios::binary);
            out << xml;
            if (!out) throw std::runtime_error("overlay write failed");
            mounts.insert(mounts.begin(), {"corruption-mod-proof", directory_, "data", {}});
        }
        files_ = std::make_shared<const eawr::vfs::Vfs>(take(eawr::vfs::Vfs::mount(mounts)));
        loaded_ = std::make_unique<eawr::data::LoadResult>(take(eawr::data::load_catalog(*files_, eawr::data::Profile::remake)));
        data_ = std::make_unique<eawr::data::galactic::GalacticData>(take(eawr::data::galactic::load_galactic_data(*files_, loaded_->catalog)));
        types_ = std::make_unique<eawr::units::GalacticTypeTable>(eawr::units::galactic_type_table(loaded_->catalog));
        const auto* source = data_->campaign(mod.campaign);
        if (!source) throw std::runtime_error("missing campaign " + mod.campaign);
        options_.seed = 42;
        for (const auto& row : data_->factions) {
            const auto id = static_cast<gc::PlayerId>(options_.players.size() + 1);
            ids_[row.name] = id;
            options_.players.push_back({{id, id, 0}, row.name, true});
            const bool human = row.name == source->starting_active_player;
            neutral_[id] = row.is_neutral.value_or(false);
            if (human) { seat_ = gc::SeatId{id}; player_ = id; options_.solo_reference_human = id; }
            if (!neutral_[id]) options_.seats.push_back({gc::SeatId{id}, id,
                human ? gc::SeatKind::human : gc::SeatKind::ai, id, true, human ? id : 0});
        }
        std::vector<std::string> originals;
        for (const auto& type : types_->types) originals.push_back(type.name);
        std::sort(originals.begin(), originals.end());
        for (const auto& name : originals) {
            const gc::TypeId id = options_.types.size() + 1;
            options_.types.push_back({id, name});
            type_ids_[name] = id;
        }
        auto prepared = take(eawr::platform::PreparedLiveCampaign::create(files_, loaded_->catalog, *data_, *types_,
            mod.campaign, options_, "Normal_Default", false));
        host_ = prepared->layout().ability_host;
        for (const auto& location : prepared->plan().locations())
            if (location.planet) planet_names_[location.planet->id] = location.original_type;
        eawr::platform::LiveGameSession::Options live_options;
        live_options.workers = 1; live_options.pacing = eawr::platform::LiveGameSession::Pacing::driven;
        live_ = take(eawr::platform::LiveGameSession::start(std::move(prepared), live_options));
    }
    ~Rig() {
        if (live_) live_->stop();
        live_.reset();
        if (!directory_.empty()) eawr::testing::remove_temp_directory(directory_);
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    [[nodiscard]] const std::string& label() const { return label_; }
    [[nodiscard]] gc::PlayerId player() const { return player_; }
    [[nodiscard]] const eawr::data::galactic::GalacticData& data() const { return *data_; }
    [[nodiscard]] const eawr::data::Catalog& catalog() const { return loaded_->catalog; }
    [[nodiscard]] const eawr::vfs::Vfs& files() const { return *files_; }
    [[nodiscard]] const gc::GalacticAbilitySetup& host() const { return *host_; }
    [[nodiscard]] bool has_host() const { return host_.has_value(); }
    [[nodiscard]] gc::TypeId type(const std::string& name) const {
        const auto found = type_ids_.find(name);
        return found == type_ids_.end() ? gc::TypeId{} : found->second;
    }
    [[nodiscard]] std::string planet_name(gc::PlanetId planet) const {
        const auto found = planet_names_.find(planet);
        return found == planet_names_.end() ? std::string("?") : found->second;
    }
    [[nodiscard]] gc::PlanetId planet_by_name(const std::string& name) const {
        for (const auto& [id, value] : planet_names_) if (value == name) return id;
        return {};
    }
    [[nodiscard]] bool neutral(gc::PlayerId owner) const { const auto found = neutral_.find(owner); return found != neutral_.end() && found->second; }
    [[nodiscard]] std::shared_ptr<const gc::GalacticSnapshot> snapshot() const { return live_->frame().latest; }
    [[nodiscard]] std::uint64_t tick() const { return tick_; }
    [[nodiscard]] std::size_t rejections() const { return rejected_.size(); }
    [[nodiscard]] const std::vector<gc::GalacticEvent>& rejected() const { return rejected_; }
    [[nodiscard]] std::size_t settled() const { return settled_; }
    // The hero respawn timers as the live session publishes them. The campaign's own return ledger cannot be read from here:
    // the prepared campaign's GameSession moves into the live session (the old pointer to it dangled and crashed the proof, #7753).
    [[nodiscard]] std::vector<gc::HeroReturnView> hero_returns() const {
        const auto frame = live_->frame();
        return frame.hero_returns ? *frame.hero_returns : std::vector<gc::HeroReturnView>{};
    }

    void step() {
        const auto advanced = eawr::platform::advance_unattended(*live_, ++tick_, std::chrono::seconds(120), answered_);
        if (!advanced.reached) {
            if (const auto error = live_->failure()) throw std::runtime_error(eawr::core::format_diagnostic(*error));
            throw std::runtime_error(label_ + ": the live campaign stalled at tick " + std::to_string(tick_)
                + (advanced.stalled ? " (" + advanced.stalled->describe() + ")" : std::string()));
        }
        const auto events = live_->galactic_events_after(cursor_);
        cursor_ = events.through;
        for (const auto& row : events.events) {
            if (row.event.producer != seat_) continue;
            if (row.event.kind == gc::GalacticEventKind::command_rejected) rejected_.push_back(row.event);
            if (row.event.kind == gc::GalacticEventKind::ability_settled) ++settled_;
        }
    }
    void steps(unsigned count) { for (unsigned i = 0; i < count; ++i) step(); }
    void submit(gc::CommandPayload payload) { live_->submit(eawr::platform::LiveGameOrder{seat_, std::move(payload), tick_ + 1}); }

    [[nodiscard]] std::optional<Fixed> credits() const {
        const auto now = snapshot();
        const auto& economy = now->player_economy();
        if (economy) for (const auto& row : economy->players) if (row.player == player_) return row.credits;
        return std::nullopt;
    }
    [[nodiscard]] fixture::Type corruption_at(gc::PlanetId planet) const {
        const auto now = snapshot();
        const auto* state = now->economy_corruption();
        if (state) for (const auto& row : state->planets) if (row.planet == planet) return row.corruption;
        return fixture::Type::none;
    }
    [[nodiscard]] std::optional<eawr::testing::galactic::HeroPlace> hero(gc::TypeId member) const {
        return find_hero_place(*snapshot(), player_, member);
    }
    // Wait (bounded) until the hero stands on the map.
    std::optional<eawr::testing::galactic::HeroPlace> wait_hero(gc::TypeId member, unsigned limit) {
        auto place = hero(member);
        for (unsigned wait = 0; !place && wait < limit; ++wait) { step(); place = hero(member); }
        return place;
    }
    [[nodiscard]] std::vector<view::InputPlanet> input_planets() const {
        const auto now = snapshot();
        std::vector<view::InputPlanet> result;
        for (const auto& planet : now->planets()) {
            auto allegiance = planet.owner;
            if (const auto* control = now->control()) for (const auto& row : control->state)
                if (row.planet == planet.id) allegiance = row.current_player;
            result.push_back({planet.id, {}, allegiance, true}); // route planning only: the whole map as discovered
        }
        return result;
    }
    // The player's own route to a corruption dialog: the release on the target planet's corruption tile (the move and the
    // action are one order), then the hero's arrival opens the dialog. Returns the published menu, or null.
    const gc::CorruptionMenu* open_dialog(gc::TypeId member, gc::PlanetId target, std::string& why) {
        const auto place = hero(member);
        if (!place) { why = "the hero is not on the map"; return nullptr; }
        const auto now = snapshot();
        auto planets = input_planets();
        const auto tile = view::ability_drop_candidate(*now, player_, place->dragged, *host_, {}, target);
        if (!tile || tile->kind != gc::AbilityKind::corruption) { why = "the hero's drag shows no corruption tile on the planet"; return nullptr; }
        view::MapInput input;
        view::DropIntent intent;
        if (place->planet == target && !place->transit) {
            intent.kind = view::DropKind::ability;
            intent.source = tile->actor; intent.planet = target;
            intent.ability = view::ability_drop_order(*tile);
        } else if (place->fleet) {
            const auto movement = view::fleet_destination_intent(*now, player_, place->dragged, target, planets, {});
            if (!movement) { why = "the target has no fleet route"; return nullptr; }
            intent = *movement;
            intent.destination_actor = tile->supplied;
        } else {
            const auto path = view::destination_path(*now, player_, place->planet, target, planets, true);
            if (!path) { why = "the target has no route"; return nullptr; }
            intent.kind = view::DropKind::launch;
            intent.source = tile->supplied; intent.planet = place->planet; intent.path = *path;
            intent.destination_actor = tile->supplied;
        }
        const auto command = input.command(intent, planets);
        if (!command) { why = "the drop maps to no order"; return nullptr; }
        submit(*command);
        for (unsigned wait = 0; wait < 12000; ++wait) {
            step();
            auto latest = snapshot();
            const auto* menu = latest->corruption_menu();
            if (menu && menu->planet == target) { menu_snapshot_ = std::move(latest); return menu_snapshot_->corruption_menu(); }
        }
        why = "no dialog opened (rejections:" + reject_text(rejected_) + ")";
        return nullptr;
    }

private:
    std::string label_;
    std::filesystem::path directory_;
    std::shared_ptr<const eawr::vfs::Vfs> files_;
    std::unique_ptr<eawr::data::LoadResult> loaded_;
    std::unique_ptr<eawr::data::galactic::GalacticData> data_;
    std::unique_ptr<eawr::units::GalacticTypeTable> types_;
    cp::CampaignSetupOptions options_;
    std::map<std::string, gc::PlayerId> ids_;
    std::map<gc::PlayerId, bool> neutral_;
    std::map<std::string, gc::TypeId> type_ids_;
    std::map<gc::PlanetId, std::string> planet_names_;
    gc::PlayerId player_{};
    gc::SeatId seat_{};
    std::optional<gc::GalacticAbilitySetup> host_;
    std::unique_ptr<eawr::platform::LiveGameSession> live_;
    std::uint64_t tick_{}, cursor_{};
    eawr::sim::EntityId answered_{};
    std::vector<gc::GalacticEvent> rejected_;
    std::size_t settled_{};
    std::shared_ptr<const gc::GalacticSnapshot> menu_snapshot_;
};

constexpr std::string_view kRawId = "1129810972";
constexpr std::string_view kBfcId = "2712013423";
constexpr std::string_view kRemakeBase = "1770851727"; // the base item of the Remake chain; the chain itself comes from the host

Mod raw_mod() { return {std::string(kRawId), "Campaign_Republic_at_War_Clone_Wars.xml", "Sandbox_The_Clone_Wars_Empire", true}; }

const gc::CorruptionPlanet* content_for(const Rig& rig, gc::PlanetId planet) {
    for (const auto& row : rig.host().corruption_planets) if (row.planet == planet) return &row;
    return nullptr;
}
bool has_slots(const gc::CorruptionPlanet& planet) {
    return std::any_of(planet.slots.begin(), planet.slots.end(), [](const auto& slot) { return slot.type != fixture::Type::none; });
}
std::size_t slot_of(const gc::CorruptionPlanet& planet, fixture::Type type, bool require_free = false) {
    for (std::size_t i = 0; i < planet.slots.size(); ++i)
        if (planet.slots[i].type == type && (!require_free || (planet.slots[i].unit_types.empty() && planet.slots[i].mission_count == 0))) return i;
    return planet.slots.size();
}
// A hero that stands inside a hero company is scheduled under the company's type (WGM-80), so the return ledger names the
// company; a hero with no company is named by its own type.
struct HeroRef {
    gc::TypeId hero{}, company{};
};
bool respawn_pending(const Rig& rig, const HeroRef& who) {
    const auto rows = rig.hero_returns();
    for (const auto& row : rows)
        if (row.owner == rig.player() && row.type == (who.company ? who.company : who.hero)) return row.deadline > row.start_frame;
    std::cerr << "respawn ledger has " << rows.size() << " rows for hero " << who.hero << " company " << who.company << ":";
    for (const auto& row : rows) std::cerr << " [owner " << row.owner << " type " << row.type << " " << row.start_frame << ".." << row.deadline << "]";
    std::cerr << std::endl;
    return false;
}
struct Outcome {
    bool ok{};
    Fixed charged{}, price{};
    std::uint64_t ticks_to_apply{};
};

// One corruption by the diplomacy hero: opens the planet's dialog by the player's route, executes the slot of `type`, and
// checks the charge, the consumed hero, the 5 s transition and the one-time reward.
Outcome corrupt(Rig& rig, const HeroRef& member, gc::PlanetId target, fixture::Type type, const std::vector<HeroRef>& named_prerequisites) {
    Outcome out;
    const std::string where = rig.label() + " " + rig.planet_name(target) + " " + std::to_string(static_cast<int>(type));
    const auto* content = content_for(rig, target);
    expect(content != nullptr, where + ": the planet has authored corruption content");
    if (!content) return out;
    const auto slot = slot_of(*content, type);
    expect(slot < content->slots.size(), where + ": the planet authors a slot of that type");
    if (slot >= content->slots.size()) return out;
    const auto authored = content->slots[slot];
    std::cout << where << " :: hero wait" << std::endl;
    const auto place = rig.wait_hero(member.hero, 12000);
    expect(place.has_value(), where + ": the corruption hero stands on the map (respawned if it was consumed)");
    if (!place) return out;
    std::cout << where << " :: open dialog" << std::endl;
    std::string why;
    const auto* menu = rig.open_dialog(member.hero, target, why);
    expect(menu != nullptr, where + ": the player's route opens the Corrupt Planet dialog: " + why);
    if (!menu) return out;
    const gc::ObservedCorruptionChoice* row{};
    for (const auto& choice : menu->choices) if (choice.slot == slot + 1) row = &choice;
    expect(row && row->live_prepared, where + ": the dialog offers the slot prepared for execution");
    if (!row || !row->live_prepared) return out;
    expect(row->prepared.choice.credits == authored.price, where + ": the dialog shows the authored price");
    expect(row->affordable && row->units_satisfied && row->missions_satisfied && row->planets_satisfied,
        where + ": the slot's requirements are met (affordable=" + std::to_string(row->affordable) + " units="
        + std::to_string(row->units_satisfied) + " missions=" + std::to_string(row->missions_satisfied) + ")");
    if (!(row->affordable && row->units_satisfied && row->missions_satisfied && row->planets_satisfied)) return out;
    std::cout << where << " :: execute" << std::endl;
    const auto before = rig.credits();
    const auto settled_before = rig.settled(), refused_before = rig.rejections();
    out.price = authored.price;
    std::cerr << where << " :: submit execute at tick " << rig.tick() << std::endl;
    rig.submit(gc::ExecutePreparedAbility{*row->live_prepared});
    std::cerr << where << " :: execute submitted" << std::endl;
    for (unsigned wait = 0; wait < 60 && rig.settled() == settled_before && rig.rejections() == refused_before; ++wait) {
        std::cerr << where << " :: execute step " << wait << " tick " << rig.tick() << std::endl;
        rig.step();
    }
    std::cerr << where << " :: execute loop done settled=" << rig.settled() << " rejected=" << rig.rejections() << std::endl;
    std::cerr << where << " :: reading credits" << std::endl;
    const auto after = rig.credits();
    std::cerr << where << " :: credits read" << std::endl;
    expect(before && after, where + ": the player has an economy row");
    if (!before || !after) return out;
    out.charged = Fixed::from_raw(before->raw() - after->raw());
    expect(rig.settled() > settled_before && rig.rejections() == refused_before,
        where + ": executing is accepted (rejections:" + reject_text(rig.rejected()) + ")");
    expect(out.charged.raw() >= authored.price.raw(), where + ": the authored price is charged");
    // WGCH-09/WGCH-06: the acting hero and a consumed named prerequisite are scheduled through the default-time path.
    expect(respawn_pending(rig, member), where + ": the acting hero went on the respawn timer");
    for (const auto& prerequisite : named_prerequisites)
        expect(respawn_pending(rig, prerequisite), where + ": the named prerequisite hero went on the respawn timer");
    std::cout << where << " :: transition" << std::endl;
    // WGCH-09: a direct corruption begins its transition with a 5 s duration (debug build evidence there).
    // The 5 s transition (150 logical frames), then the planet carries the type.
    const auto started = rig.tick();
    const auto cash_before_completion = rig.credits();
    bool applied = false;
    Fixed last_cash = cash_before_completion.value_or(Fixed{});
    Fixed completion_delta{};
    for (unsigned wait = 0; wait < 600 && !applied; ++wait) {
        rig.step();
        const auto now = rig.credits().value_or(Fixed{});
        applied = rig.corruption_at(target) == type;
        if (applied) completion_delta = Fixed::from_raw(now.raw() - last_cash.raw());
        last_cash = now;
    }
    // WGCH-13: the one-time credit reward arrives with the completion.
    out.ticks_to_apply = rig.tick() - started;
    expect(applied, where + ": the planet carries the corruption after the timed transition");
    expect(out.ticks_to_apply >= 140 && out.ticks_to_apply <= 175, where + ": the transition takes about five seconds (" + std::to_string(out.ticks_to_apply) + " ticks)");
    if (authored.credit_reward > Fixed{})
        expect(completion_delta.raw() >= authored.credit_reward.raw(), where + ": the one-time bonus arrives with the completion");
    std::cout << where << " charged=" << out.charged.raw() << " price=" << authored.price.raw() << " bonus_delta=" << completion_delta.raw()
        << " ticks=" << out.ticks_to_apply << " bonus_authored=" << authored.credit_reward.raw() << '\n';
    out.ok = applied;
    return out;
}

// ---- raw-chain -------------------------------------------------------------------------------------------------------
int raw_chain() {
    Rig rig(raw_mod(), {"Padme_Amidala_Team", "Senator_Organa_Team"}, "raw-chain");
    expect(rig.has_host(), "the campaign has an ability host");
    if (!rig.has_host()) return 1;
    const HeroRef padme{rig.type("Padme_Amidala"), rig.type("Padme_Amidala_Team")};
    const HeroRef organa{rig.type("Senator_Organa"), rig.type("Senator_Organa_Team")};
    expect(padme.hero && padme.company && organa.hero && organa.company,
        "the diplomacy hero, the named senator and their companies are registered types");
    if (!padme.hero || !padme.company || !organa.hero || !organa.company) return 1;
    const auto start = rig.snapshot();
    const auto* economy = start->economy_corruption();
    expect(economy != nullptr, "the campaign has corruption economy state");
    if (!economy) return 1;
    for (const auto& row : economy->players)
        if (row.player == rig.player()) expect(row.benefits, "the human faction is the corrupting faction");

    // The planets the chain uses are the ones whose authored slots fit: an intimidation slot with no prerequisite (counts
    // the first use), the intimidation slot that needs the named hero (second use and the consumed hero), and a racketeering
    // slot with no unit requirement and a one-time bonus. They are found in the authored data, not named here.
    const auto owner_of = [&](gc::PlanetId planet) {
        const auto now = rig.snapshot();
        gc::PlayerId owner{};
        for (const auto& row : now->planets()) if (row.id == planet) owner = row.owner;
        return owner;
    };
    const auto hostile = [&](gc::PlanetId planet) { const auto owner = owner_of(planet); return owner && owner != rig.player() && !rig.neutral(owner); };
    gc::PlanetId first{}, second{}, third{};
    for (const auto& planet : rig.host().corruption_planets) {
        if (!hostile(planet.planet)) continue;
        if (!first && slot_of(planet, fixture::Type::intimidation, true) < planet.slots.size()) first = planet.planet;
    }
    for (const auto& planet : rig.host().corruption_planets) {
        if (!hostile(planet.planet) || planet.planet == first) continue;
        const auto at = slot_of(planet, fixture::Type::intimidation);
        if (!second && at < planet.slots.size() && !planet.slots[at].unit_types.empty() && planet.slots[at].mission_count > 0) second = planet.planet;
    }
    for (const auto& planet : rig.host().corruption_planets) {
        if (!hostile(planet.planet) || planet.planet == first || planet.planet == second) continue;
        const auto at = slot_of(planet, fixture::Type::racketeering);
        if (!third && at < planet.slots.size() && planet.slots[at].unit_types.empty() && planet.slots[at].credit_reward > Fixed{}
            && planet.slots[at].mission_count == 2) third = planet.planet;
    }
    std::cout << "raw-chain planets: first=" << rig.planet_name(first) << " second=" << rig.planet_name(second)
        << " third=" << rig.planet_name(third) << '\n';
    expect(first && second && third, "the authored data offers the three slots of the chain");
    if (!first || !second || !third) return 1;

    const auto one = corrupt(rig, padme, first, fixture::Type::intimidation, {});
    if (!one.ok) return 1;
    const auto two = corrupt(rig, padme, second, fixture::Type::intimidation, {organa});
    if (!two.ok) return 1;
    // The use counters now satisfy the racketeering slot's two intimidation uses.
    const auto three = corrupt(rig, padme, third, fixture::Type::racketeering, {});
    if (!three.ok) return 1;

    // The corrupting faction's racketeering upgrade is held on the planet it just corrupted.
    const auto racketeering = rig.host().corruption_players;
    gc::TypeId upgrade{};
    for (const auto& row : racketeering) if (row.player == rig.player())
        upgrade = row.automatic_upgrades[static_cast<std::size_t>(fixture::Type::racketeering)];
    expect(upgrade != 0, "the faction authors a racketeering upgrade");
    return 0;
}

// ---- raw-own ---------------------------------------------------------------------------------------------------------
int raw_own() {
    Rig rig(raw_mod(), {"Yoda_Team"}, "raw-own");
    expect(rig.has_host(), "the campaign has an ability host");
    if (!rig.has_host()) return 1;
    const auto yoda = rig.type("Yoda");
    expect(yoda != 0, "the removal hero is a registered type");
    if (!yoda) return 1;
    const auto place = rig.wait_hero(yoda, 120);
    expect(place.has_value(), "the removal hero stands on the map");
    if (!place) return 1;
    // The planet the hero stands on is the corrupting faction's own and carries the default trade agreement.
    const auto planet = place->planet;
    rig.steps(30);
    const auto at_start = rig.corruption_at(planet);
    expect(at_start == fixture::Type::racketeering, "the faction's own planet carries the default trade agreement (racketeering)");
    const auto before = rig.credits();
    expect(before.has_value(), "the player has an economy row");
    if (!before) return 1;
    // The viewer's own preview: the hero's drag offers no removal tile on the faction's own planet.
    const auto now = rig.snapshot();
    const auto tile = view::ability_drop_candidate(*now, rig.player(), place->dragged, rig.host(), {}, planet);
    std::cout << "raw-own tile=" << (tile ? static_cast<int>(tile->kind) : -1) << '\n';
    expect(!tile || tile->kind != gc::AbilityKind::remove_corruption, "no removal tile on the faction's own planet");
    // The hero stays there for a while (and the order a forged client could send is the sim's to refuse).
    rig.submit(gc::PrepareGalacticAbility{place->bearer, planet, gc::AbilityKind::remove_corruption});
    rig.steps(400);
    expect(rig.corruption_at(planet) == fixture::Type::racketeering, "the default trade agreement is still there");
    const auto after = rig.credits();
    expect(after.has_value(), "the economy row is still there");
    if (after) expect(after->raw() >= before->raw(), "no credits were charged for a removal on the faction's own planet");
    return 0;
}

// ---- information menu ------------------------------------------------------------------------------------------------
void check_information(Rig& rig, bool expect_slotted, bool expect_slotless) {
    rig.steps(60);
    const auto now = rig.snapshot();
    const auto before = rig.credits();
    std::size_t slotted{}, empty{}, wrong{};
    std::string empty_names;
    for (const auto& planet : rig.host().corruption_planets) {
        const auto* menu = now->corruption_information(planet.planet);
        if (!menu) continue;
        const bool authored = has_slots(planet);
        authored ? ++slotted : ++empty;
        if (!authored && empty_names.size() < 120) empty_names += " " + rig.planet_name(planet.planet);
        // Information only: no acting hero, no preparation, nothing executable, every row without a prepared order.
        if (!menu->information_only || menu->actor || menu->preparation || menu->player != rig.player()) ++wrong;
        if (menu->choices.empty() == authored) ++wrong;
        for (const auto& row : menu->choices) if (row.live_prepared) ++wrong;
        if (authored) {
            std::size_t shown{};
            for (const auto& slot : planet.slots) if (slot.type != fixture::Type::none) ++shown;
            if (menu->choices.size() != shown) ++wrong;
            for (const auto& row : menu->choices) {
                const auto& authored_slot = planet.slots[row.slot - 1];
                if (row.prepared.choice.credits != authored_slot.price || row.prepared.choice.type != authored_slot.type) ++wrong;
            }
        }
    }
    std::cout << rig.label() << " information menus: slotted=" << slotted << " empty=" << empty << " wrong=" << wrong
        << " (empty planets:" << empty_names << ")\n";
    expect(wrong == 0, rig.label() + ": every information menu is information-only and lists exactly the authored slots");
    if (expect_slotted) expect(slotted > 0, rig.label() + ": the information menu opens on a slotted planet");
    if (expect_slotless) expect(empty > 0, rig.label() + ": the information menu opens, empty, on a planet with no slots");
    expect(now->corruption_menu() == nullptr, rig.label() + ": no actor dialog is open");
    // Opening it changes nothing: no debit, no queued order, no rejection.
    rig.steps(30);
    const auto after = rig.credits();
    expect(before && after && after->raw() >= before->raw(), rig.label() + ": reading the menu debits nothing");
    expect(rig.rejections() == 0, rig.label() + ": reading the menu queues nothing and is rejected nowhere");
}

int raw_info() {
    Rig rig(raw_mod(), {}, "raw-info");
    expect(rig.has_host(), "the campaign has an ability host");
    if (!rig.has_host()) return 1;
    check_information(rig, true, true);
    return 0;
}

// ---- remake: dormant ------------------------------------------------------------------------------------------------
int remake() {
    const Mod mod{std::string(kRemakeBase), "", "GC_Mini_Web_Way_Rebel", false, true};
    Rig rig(mod, {}, "remake");
    expect(rig.has_host(), "the campaign has an ability host (corruption is dormant by its data, not by a missing host)");
    if (!rig.has_host()) return 1;
    rig.steps(300);
    const auto now = rig.snapshot();
    const auto* economy = now->economy_corruption();
    expect(economy != nullptr, "the campaign has corruption economy state (dormant means no beneficiary, not no state)");
    if (!economy) return 1;
    std::size_t beneficiaries{}, corrupted{}, slotted{}, informations{}, tiles{};
    {
        for (const auto& row : economy->players) if (row.benefits) ++beneficiaries;
        for (const auto& row : economy->planets) if (row.corruption != fixture::Type::none) ++corrupted;
    }
    {
        for (const auto& planet : rig.host().corruption_planets) {
            if (has_slots(planet)) ++slotted;
            if (now->corruption_information(planet.planet)) ++informations;
        }
        // No hero offers a corruption tile on any planet.
        for (const auto& fleet : now->fleets()) {
            if (fleet.owner != rig.player()) continue;
            for (const auto& planet : now->planets()) {
                const auto tile = view::ability_drop_candidate(*now, rig.player(), fleet.id, rig.host(), {}, planet.id);
                if (tile && tile->kind == gc::AbilityKind::corruption) ++tiles;
            }
        }
    }
    std::cout << "remake: beneficiaries=" << beneficiaries << " corrupted=" << corrupted << " slotted=" << slotted
        << " information=" << informations << " tiles=" << tiles << '\n';
    expect(beneficiaries == 0, "no faction benefits from corruption (no information icon is drawn)");
    expect(slotted == 0, "no planet authors a corruption slot");
    expect(informations == 0, "no information menu is published");
    expect(corrupted == 0, "no planet is corrupted");
    expect(tiles == 0, "no hero's drag shows a corruption tile");
    expect(now->corruption_menu() == nullptr, "no corruption dialog is open");
    rig.steps(120);
    expect(rig.rejections() == 0, "nothing is admitted or rejected");
    return 0;
}

// ---- bfc ------------------------------------------------------------------------------------------------------------
int bfc() {
    const Mod mod{std::string(kBfcId), "campaigns_underworld_gc.xml", "Sandbox_Equal_Footing_Underworld", false};
    Rig rig(mod, {}, "bfc");
    expect(rig.has_host(), "the campaign has an ability host");
    if (!rig.has_host()) return 1;
    check_information(rig, true, false);

    // The corrupted-planet bribe modifier is read from the data: the authored briber's pair, and a price that follows it.
    eawr::units::LoadInput load;
    load.campaign_scene = true;
    load.catalog = &rig.catalog();
    load.filesystem = &rig.files();
    load.model = [](std::string_view) -> const eawr::assets::Model* { return nullptr; };
    load.types = {"Novoc_Vevut"};
    const auto tables = take(eawr::units::load_unit_tables(load));
    const eawr::units::UnitType* briber{};
    for (const auto& unit : tables.units) if (unit.id == "Novoc_Vevut") briber = &unit;
    expect(briber != nullptr, "the authored briber loads");
    if (!briber) return 1;
    expect(briber->bribe_corrupted_modifier.has_value() && briber->bribe_uncorrupted_modifier.has_value(),
        "the briber's corrupted and uncorrupted bribe modifiers are read");
    if (!briber->bribe_corrupted_modifier || !briber->bribe_uncorrupted_modifier) return 1;
    expect(*briber->bribe_corrupted_modifier < *briber->bribe_uncorrupted_modifier, "a bribe is cheaper on a corrupted planet");
    eawr::units::UnitType target;
    target.tactical_bribe_cost = Fixed::from_integer(100).value();
    const auto plain = take(eawr::units::tactical_bribe_cost(*briber, target, std::nullopt));
    const auto corrupted = take(eawr::units::tactical_bribe_cost(*briber, target, true));
    const auto uncorrupted = take(eawr::units::tactical_bribe_cost(*briber, target, false));
    expect(plain == target.tactical_bribe_cost.value(), "outside a galactic battle the price is unmodified");
    expect(corrupted.raw() < plain.raw() && corrupted.raw() == (plain.raw() / Fixed::scale) * briber->bribe_corrupted_modifier->raw()
        , "the corrupted-planet price follows the briber's authored modifier");
    expect(uncorrupted.raw() == (plain.raw() / Fixed::scale) * briber->bribe_uncorrupted_modifier->raw(),
        "the uncorrupted-planet price follows the briber's authored modifier");
    std::cout << "bfc bribe: corrupted=" << corrupted.raw() << " uncorrupted=" << uncorrupted.raw() << '\n';
    return 0;
}
} // namespace

// An abort or an unhandled exception on any thread (the live session's sim thread included) must say so before the process
// ends: without this the test exited nonzero with no message (#7753).
void on_terminate() {
    std::cerr << "FATAL: std::terminate called";
    if (const auto current = std::current_exception()) {
        try { std::rethrow_exception(current); }
        catch (const std::exception& error) { std::cerr << " (" << typeid(error).name() << ": " << error.what() << ")"; }
        catch (...) { std::cerr << " (non-std exception)"; }
    } else std::cerr << " (no current exception)";
    std::cerr << std::endl;
    std::_Exit(113);
}
void on_abort(int) { std::cerr << "FATAL: SIGABRT (abort() called)" << std::endl; std::_Exit(114); }

#ifdef _WIN32
// A native crash (access violation, stack overflow, fast fail) on any thread: the wrapper script that runs the test turns the
// child's exit code into a bare "Failed", so the code and the stack are written here.
LONG WINAPI on_native_crash(EXCEPTION_POINTERS* pointers) {
    std::cerr << "FATAL: native exception 0x" << std::hex << pointers->ExceptionRecord->ExceptionCode << " at "
        << pointers->ExceptionRecord->ExceptionAddress << std::dec << " thread " << GetCurrentThreadId() << std::endl;
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);
    STACKFRAME64 stack{};
    CONTEXT context = *pointers->ContextRecord;
    stack.AddrPC = {context.Rip, 0, AddrModeFlat};
    stack.AddrStack = {context.Rsp, 0, AddrModeFlat};
    stack.AddrFrame = {context.Rbp, 0, AddrModeFlat};
    for (unsigned index = 0; index < 40; ++index) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &stack, &context, nullptr,
                SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !stack.AddrPC.Offset) break;
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO); symbol->MaxNameLen = 255;
        DWORD64 displacement{};
        const bool named = SymFromAddr(process, stack.AddrPC.Offset, &displacement, symbol);
        std::cerr << "  #" << index << " 0x" << std::hex << stack.AddrPC.Offset << std::dec << ' '
            << (named ? symbol->Name : "?") << "+" << displacement << std::endl;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    std::cout << std::unitbuf;  // a crash must not swallow the progress lines
    std::set_terminate(on_terminate);
    std::signal(SIGABRT, on_abort);
#ifdef _WIN32
    SetUnhandledExceptionFilter(on_native_crash);
#endif
    try {
        if (mode == "raw-chain") return raw_chain() + galactic_test::checker.finish();
        if (mode == "raw-own") return raw_own() + galactic_test::checker.finish();
        if (mode == "raw-info") return raw_info() + galactic_test::checker.finish();
        if (mode == "remake") return remake() + galactic_test::checker.finish();
        if (mode == "bfc") return bfc() + galactic_test::checker.finish();
        std::cerr << "usage: galactic_corruption_mod_proof_tests raw-chain|raw-own|raw-info|remake|bfc\n";
        return 2;
    } catch (const MissingMod& missing) {
        std::cout << "SKIPPED: " << missing.what() << '\n';
        return 77;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
