#include "lan_lobby_screen_internal.hpp"
#include "viewer_mounted_mods.hpp"

#include "galactic_lobby_backend.hpp"
#include "coop_campaign_tab.hpp"
#include "galactic/faction_choices.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <random>

// GCMP-07 (#4363): the Galactic Conquest mode of the LAN/online staging. The host picks it with the retail "Campaign"
// game type before the room opens; the mode, the campaign and the options then travel in the lobby's game blob, so
// every peer draws the same room. The staging dialog is the skirmish one: its rows are the campaign's factions (a human
// row or an AI row), each seat sits on a human row, and a row's team is the human side. Starting the match is the
// galactic lockstep driver's (GCMP-03): Start stays off, with a reason, until a build installs its adapter.
namespace eawr::presentation::godot_backend {
namespace {
// SK-42/WGEP-16 default difficulty; the editor accepts an authored XML difficulty name.
constexpr const char* galactic_difficulty = "Normal_Default";

// Item ids of the host's Open / AI choice on a playable, unheld row (kick entries use 10000 + peer).
constexpr int galactic_row_open_id = 20000;
constexpr int galactic_row_ai_id = 20001;

std::string row_label(const model::StagingRow& row) {
    if (!row.name.empty()) return row.name;
    return row.kind == model::RowKind::open ? "TEXT_OPEN_PLAYER" : "TEXT_MEDIUM_AI_PLAYER";
}

// The campaign's localized name and description (#4553), through the same text table the campaign menu uses. A campaign
// whose Text_ID or Description_Text the table lacks (a mod) shows its ID and names the missing keys once in the log.
eawr::campaign::MpLobbyCampaignLabels campaign_labels(std::optional<data::TextLookup>& lookup, const sim::galactic::MpLobbyCampaign& campaign,
                                                std::set<std::string>& logged) {
    auto labels = eawr::campaign::label_mp_lobby_campaign(campaign, [&](const std::string_view key) -> std::optional<std::string> {
        if (!lookup || !lookup->contains(key)) return std::nullopt;
        return data::to_utf8(lookup->text(key));
    });
    if (!labels.missing.empty() && logged.insert(campaign.name).second) {
        std::string line = "lan-lobby: campaign " + campaign.name + " has no text for";
        for (const auto& key : labels.missing) line += " " + key;
        UtilityFunctions::print(String::utf8(line.data(), static_cast<std::int64_t>(line.size())));
    }
    return labels;
}
} // namespace

// The room has ONE AI difficulty (the options dialog's choice, the value every AI seat is played and agreed at), so an AI
// seat names that level: Easy/Normal/Hard entries use the stock AI captions; a mod's own difficulty shows its authored name.
std::string ai_seat_caption(std::string_view difficulty) {
    using Caption = net::GalacticAiCaption;
    const auto level = net::galactic_ai_caption(difficulty);
    if (!level) return std::string(difficulty);
    return *level == Caption::easy ? "TEXT_EASY_AI_PLAYER" : *level == Caption::hard ? "TEXT_HARD_AI_PLAYER" : "TEXT_MEDIUM_AI_PLAYER";
}

std::string EawrLanLobby::campaign_display_name(const std::string& campaign) {
    Impl& impl = *impl_;
    if (!impl.galactic_backend || !impl.galactic_backend->content_ready() || !resources_) return campaign;
    for (const auto& candidate : impl.galactic_backend->campaigns()) {
        if (candidate.name != campaign) continue;
        auto title = campaign_labels(resources_->text, candidate, impl.campaign_text_logged).title;
        return title.empty() ? campaign : title;
    }
    return campaign;
}

void EawrLanLobby::configure_galactic_content() {
    Impl& impl = *impl_;
    prime_mounted_mods(options_.game_root);
    if (!impl.galactic_backend) {
        const auto content = resources_->content;
        // Loaded off the UI thread, and only when a Campaign room is in sight (host options, a listed or joined campaign).
        impl.galactic_backend = std::make_shared<GalacticLobbyBackend>(GalacticLobbyLoader([content] {
            return load_galactic_lobby_content(*content->filesystem, *content->catalog, content);
        }));
    }
    match_->configure_galactic(impl.galactic_backend);
}

bool EawrLanLobby::prefetch_galactic_content() {
    Impl& impl = *impl_;
    prime_mounted_mods(options_.game_root);
    if (!impl.galactic_backend && !(resources_ && resources_->content)) return false;
    if (!impl.galactic_backend) {
        const auto content = resources_->content;
        impl.galactic_backend = std::make_shared<GalacticLobbyBackend>(GalacticLobbyLoader([content] {
            return load_galactic_lobby_content(*content->filesystem, *content->catalog, content);
        }));
    }
    GalacticLobbyBackend::prefetch_content(impl.galactic_backend);
    return impl.galactic_backend->content_ready() && mounted_mods_ready();
}

// Host and Load Game both need the campaign list. While the load runs they show it and return; this finishes them.
void EawrLanLobby::finish_pending_galactic() {
    Impl& impl = *impl_;
    if (!impl.host_pending && !impl.load_pending) return;
    // A prefetch thread that could not start is retried here; the buttons never wait on a load nobody runs.
    if (!prefetch_galactic_content()) return;
    const bool host = std::exchange(impl.host_pending, false);
    const bool load = std::exchange(impl.load_pending, false);
    if (load) { impl.checkpoint_problem.clear(); impl.checkpoint_signature.clear(); }
    if (host && page_ == Page::host_options) {
        if (auto* button = EAWR_CONTROL(EawrUiButton, "IDC_INTERNET_CREATE_GAME_BUTTON")) {
            button->set_disabled(false);
            button->set_text(text(impl.host_button_caption));
        }
        begin_hosting();
    }
    if (load && page_ == Page::staging) open_checkpoint_list();
}

std::optional<std::string> EawrLanLobby::host_galactic_game() {
    Impl& impl = *impl_;
    if (!impl.galactic_backend) return std::string("No campaign content is loaded.");
    const auto description = mounted_mod_description();
    const auto encoded = description ? mods::encode_description(*description) : std::nullopt;
    // The mod list is display only. When it cannot be encoded or the room codec would refuse it, the room still opens and the guest
    // is told the host's list could not be checked (empty bytes mean "not known").
    if (encoded && encoded->size() <= net::max_lobby_mod_description_bytes && net::valid_lobby_mod_description(*encoded))
        impl.galactic_backend->set_mounted_mods(*encoded);
    else impl.galactic_backend->set_mounted_mods({});
    if (!impl.galactic_backend->content_ready()) return std::string("Campaign content is still loading.");
    if (const auto error = impl.galactic_backend->load_error()) return core::format_diagnostic(*error);
    const auto& campaigns = impl.galactic_backend->campaigns();
    if (campaigns.empty()) return std::string("No campaign is installed.");
    // The room opens on the first row a player can hold; a mod may have no multiplayer campaign at all.
    const auto* hostable = eawr::campaign::first_hostable_lobby_campaign(campaigns);
    if (hostable == nullptr) return std::string("No installed campaign has a playable faction, so no campaign room can be hosted.");
    // The seed is the host's choice and travels with the options, so every peer derives the same galaxy from it.
    const std::uint64_t seed = net::draw_galactic_session_seed();
    auto options = net::default_galactic_lobby_options(*hostable, galactic_difficulty, seed, options_.input_delay);
    if (!options) return options.error().message;
    if (!match_->set_galactic_game(std::move(options).value())) return std::string("The campaign room could not be opened.");
    return std::nullopt;
}

void EawrLanLobby::refresh_galactic_staging() {
    Impl& impl = *impl_;
    PageState& state = *state_;
    Resources& resources = *resources_;
    const std::size_t dialog_rows = state.dialog_rows;
    // The galactic room is not built from a skirmish selection: the host's options opened it, edits go through the list.
    match_->pump_setup(skirmish::FixtureOptions{});
    review_host_mods();
    const LanView view = match_->view();
    const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
    refresh_staging_connections(impl, view, online);
    const auto room = match_->lobby_state();
    refresh_staging_status(impl, state, resources, view, online, room);
    std::vector<model::StagingRow> rows(dialog_rows);
    const skirmish::SetupMap* map = nullptr;
    std::size_t visible = 0;
    const bool connected = view.connected && view.phase != LanPhase::ended;
    // Chat, notices, the profile name and the Accept button are mode-agnostic; its skirmish rows stay empty for a GC room.
    build_staging_rows(impl, resources, view, room, dialog_rows, rows, map, visible, connected);
    refresh_galactic_roster(impl, state, resources, view, room, rows, dialog_rows);
    refresh_staging_start(impl, resources, view, room, rows, online);
    finish_staging_refresh(impl, view, room);
    refresh_checkpoint_staging();
    if (!impl.galactic_seam_noted && match_->galactic_start()) {
        impl.galactic_seam_noted = true;
        note_chat(model::ChatKind::notice, "The campaign room is frozen for Start; the galactic game driver finishes the launch.");
    }
}

void EawrLanLobby::review_host_mods() {
    const auto view = match_->view();
    if (!view.mod_review_pending || impl_->box || !mounted_mods_ready()) return;
    const auto host = mods::decode_description(view.host_mod_description);
    const auto local = mounted_mod_description();
    auto message = host && local ? viewer::mod_mismatch_message(*host, *local)
        : std::string("The host's mod list could not be checked. Both players need the same mods.");
    if (message.empty() && host && local && viewer::mod_comparison_incomplete(*host, *local))
        message = "The mod lists could not be fully checked. Both players need the same mods.";
    if (message.empty()) { match_->accept_mod_review(); return; }
    message += "\nContinue joining?";
    model::MessageBoxRequest request;
    request.caption = "Mods differ";
    request.lines[0] = message; request.literal_lines[0] = true;
    request.buttons.push_back({"TEXT_BUTTON_YES", 1, false});
    request.buttons.push_back({"TEXT_BUTTON_NO", 0, true});
    Resources& resources = *resources_;
    const MessageBoxContext context{&*resources.dialogs, &*resources.theme_model,
        resources.text ? &*resources.text : nullptr, resources.theme, resources.theme_size};
    auto* box = memnew(EawrMessageBox);
    if (!box->open(*this, context, request)) { memdelete(box); return; }
    box->set_scrollable_text(message);
    impl_->box = box; impl_->mod_review_box = true;
    layout_page();
    box->connect("result_chosen", callable_mp(this, &EawrLanLobby::on_box_result));
}

// Retail's campaign staging draws the galaxy backdrop with a dot at each planet of the selected campaign where a
// skirmish room draws its map preview (captured on the FoC LAN host setup, #4553).
void EawrLanLobby::refresh_galactic_preview(Impl& impl, PageState& state, Resources& resources,
                                            const sim::galactic::MpLobbyCampaign* campaign) {
    if (state.preview == nullptr) return;
    const std::string wanted = campaign != nullptr ? campaign->name : std::string{};
    const std::string key = "campaign:" + wanted;
    state.preview->set_visible(true);
    if (impl.preview_path == key) return;
    impl.preview_path = key;
    Ref<Image> image;
    std::string failure;
    for (const char* extension : {".tga", ".dds"}) {
        auto texture = assets::load_texture(*resources.content->filesystem, std::string("data/art/textures/galactic_back") + extension);
        if (!texture) continue;
        image = texture_image(texture.value(), failure);
        if (image.is_valid()) break;
    }
    state.preview->set_texture(image.is_valid() ? ImageTexture::create_from_image(image) : Ref<ImageTexture>());
    constexpr const char* blips_key = "EAWR_CAMPAIGN_BLIPS";
    Control* blips = state.controls.count(blips_key) != 0 ? state.controls.at(blips_key) : nullptr;
    if (blips == nullptr) {
        blips = memnew(Control);
        blips->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        state.preview->add_child(blips);
        state.controls[blips_key] = blips;
    }
    blips->set_position(Vector2(0, 0));
    blips->set_size(state.preview->get_size());
    while (blips->get_child_count() > 0) {
        Node* child = blips->get_child(0);
        blips->remove_child(child);
        memdelete(child);
    }
    if (campaign == nullptr) return;
    // The radar's world window the points are normalised against (x by 440, y by 520), as the campaign menu's preview.
    constexpr double x_origin = 220.0, x_span = 440.0, y_origin = 260.0, y_span = 520.0;
    constexpr float blip_size = 4.0F;
    const Vector2 size = blips->get_size();
    const auto number = [](const sim::math::Fixed value) {
        return static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale);
    };
    for (const auto& [fixed_x, fixed_y] : campaign->planet_points) {
        const double x = number(fixed_x), y = number(fixed_y);
        const double nx = std::clamp((x + x_origin) / x_span, 0.0, 1.0);
        const double ny = std::clamp((y + y_origin) / y_span, 0.0, 1.0);
        auto* blip = memnew(ColorRect);
        blip->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        blip->set_color(Color(0.7F, 0.7F, 0.7F, 1.0F));
        blip->set_size(Vector2(blip_size, blip_size));
        blip->set_position(Vector2(static_cast<float>(nx) * size.x - blip_size * 0.5F,
                                   static_cast<float>(1.0 - ny) * size.y - blip_size * 0.5F));
        blips->add_child(blip);
    }
}

void EawrLanLobby::refresh_galactic_roster(Impl& impl, PageState& state, Resources& resources, const LanView& view,
                                           const std::optional<LanLobbyState>& room, std::vector<model::StagingRow>& rows,
                                           const std::size_t dialog_rows) {
    // Controls that only mean something for a skirmish map.
    for (const char* hidden : {"IDC_SELECT_OFFICIAL_MAPS", "IDC_SELECT_CUSTOM_MAPS"})
        if (auto* control = EAWR_CONTROL(Control, hidden)) control->set_visible(false);

    std::optional<net::GalacticLobbyOptions> options;
    if (room) {
        if (auto decoded = net::decode_galactic_lobby_game(room->snapshot.game)) options = std::move(decoded).value();
    }
    impl.galactic_options = options;
    impl.galactic_campaign = options ? options->campaign : std::string{};
    const sim::galactic::MpLobbyCampaign* campaign = nullptr;
    if (options && impl.galactic_backend && impl.galactic_backend->content_ready()) {
        for (const auto& candidate : impl.galactic_backend->campaigns())
            if (candidate.name == options->campaign) campaign = &candidate;
    }
    const bool checkpoint = impl.checkpoint_candidate.has_value() || (impl.galactic_backend && impl.galactic_backend->checkpoint_view().has_value());
    if (campaign) {
        impl.coop_campaign_tab = viewer::room_campaign_tab(campaign->name, impl.tab_room_campaign,
            campaign->single_player_rules, impl.coop_campaign_tab, checkpoint != impl.tab_room_checkpoint);
        impl.tab_room_campaign = campaign->name;
        impl.tab_room_checkpoint = checkpoint;
    }
    if (impl.host && impl.galactic_backend && impl.galactic_backend->content_ready()) {
        bool has_multiplayer = false, has_coop = false;
        for (const auto& entry : impl.galactic_backend->campaigns()) (entry.single_player_rules ? has_coop : has_multiplayer) = true;
        const auto tabs = viewer::campaign_tabs(has_multiplayer, has_coop, impl.coop_campaign_tab);
        impl.coop_campaign_tab = tabs.coop_selected;
        impl.tab_multiplayer_shown = tabs.multiplayer_shown;
        impl.tab_coop_shown = tabs.coop_shown;
    }
    refresh_galactic_preview(impl, state, resources, campaign);
    const bool staging = room && room->snapshot.phase == net::LobbyPhase::staging;
    if (auto* button = EAWR_CONTROL(EawrUiButton, "IDC_SELECT_OPTIONS")) {
        button->set_visible(impl.host);
        button->set_disabled(!impl.host || !staging || checkpoint || !options || state.options_panel == nullptr);
    }
    if (state.galactic_editing && (!staging || !options || state.galactic_editing->campaign != options->campaign))
        close_galactic_options(false);
    // The dialog may draw fewer rows than the campaign has factions: a player's row goes first, so it is never the one cut.
    std::vector<model::GalacticRowFacts> facts;
    if (options) {
        for (std::size_t index = 0; index < options->rows.size(); ++index) {
            const bool known = campaign != nullptr && index < campaign->factions.size();
            facts.push_back({options->rows[index].human, known && campaign->factions[index].playable, known && campaign->factions[index].neutral});
        }
    }
    impl.galactic_row_order = model::galactic_row_order(facts, dialog_rows);
    const auto& order = impl.galactic_row_order;
    const std::size_t shown_rows = order.size();
    for (std::size_t slot = 0; slot < shown_rows; ++slot) {
        const std::size_t index = order[slot]; // the campaign row drawn on dialog row `slot`
        const auto& source = options->rows[index];
        model::StagingRow& row = rows[slot];
        const bool neutral = campaign != nullptr && index < campaign->factions.size() && campaign->factions[index].neutral;
        row.faction = source.faction;
        if (const auto side = net::galactic_lobby_side(*options, source.player)) row.team = *side - 1U;
        row.kind = neutral ? model::RowKind::none : source.human ? model::RowKind::open : model::RowKind::ai;
        row.accepted = row.kind == model::RowKind::ai;
        if (row.kind == model::RowKind::ai) row.name = ai_seat_caption(options->difficulty);
        bool first = true;
        for (const auto& seat : room->snapshot.seats) {
            if (seat.selection.slot != source.player) continue;
            if (first) row.name.clear();
            row.kind = model::RowKind::human;
            row.name += (first ? "" : ", ") + seat.selection.name;
            row.local = row.local || seat.peer == view.seat;
            row.accepted = first ? seat.ready : row.accepted && seat.ready;
            first = false;
        }
    }

    // Refill the combos only when what they show changed: a refill closes an open drop-down.
    std::string signature = std::string(impl.host ? "GH" : "GJ") + (checkpoint ? "saved" : "new") + std::to_string(shown_rows) + (staging ? "s" : "-")
        + (options ? options->campaign : std::string{}) + (impl.coop_campaign_tab ? "|coop" : "|mp") + "|" + std::to_string(view.seat);
    for (const model::StagingRow& row : rows)
        signature += "|" + std::to_string(static_cast<int>(row.kind)) + "," + row.name + "," + row.faction + ","
            + (row.team ? std::to_string(*row.team) : std::string("-")) + "," + (row.accepted ? "a" : "p") + (row.local ? "l" : "r");
    if (options) {
        signature += "#" + std::to_string(options->custom) + "," + std::to_string(options->credits) + "," + std::to_string(options->starting_tech)
            + "," + std::to_string(options->maximum_tech) + "," + std::to_string(options->allow_heroes) + "," + std::to_string(options->auto_resolve) + "," + options->difficulty;
    }
    impl.rows = rows;
    impl.visible_rows = shown_rows;
    if (signature == impl.rows_signature) return;
    impl.rows_signature = signature;
    state.loading = true;
    impl.row_states.clear();
    for (std::size_t slot = 0; slot < dialog_rows; ++slot) {
        const model::StagingRow& row = rows[slot];
        const std::string suffix = std::to_string(slot);
        const bool shown = slot < shown_rows && row.kind != model::RowKind::none;
        const std::size_t index = slot < shown_rows ? order[slot] : options ? options->rows.size() : 0U; // the campaign row on this slot
        const bool has_row = slot < shown_rows;
        const bool mine = shown && row.local && staging && !checkpoint;
        auto* name = EAWR_CONTROL(EawrUiCombo, "IDC_PLAYER_NAME_" + suffix);
        auto* faction = EAWR_CONTROL(EawrUiCombo, "IDC_PLAYER_FACTION_" + suffix);
        auto* colour = EAWR_CONTROL(EawrUiCombo, "IDC_PLAYER_COLOR_" + suffix);
        auto* team = EAWR_CONTROL(EawrUiCombo, "IDC_PLAYER_TEAM_" + suffix);
        if (auto* rename = EAWR_CONTROL(EawrUiEdit, "EAWR_STAGING_NAME_" + suffix)) rename->set_visible(false);
        if (name != nullptr) {
            name->clear();
            name->set_visible(shown);
            if (shown) {
                const bool open_row = row.kind == model::RowKind::open;
                // A planned row nobody takes at Start becomes an AI faction, so an open row says so.
                const std::string open_label = utf8(resolved(resources.text, "TEXT_OPEN_PLAYER")) + " - AI if empty";
                name->add_item(open_row ? text(open_label) : resolved(resources.text, row_label(row)), 0);
                name->select(0);
                name->set_tooltip_text(text(row.kind == model::RowKind::ai ? std::string() : row.name));
                name->set_disabled(true);
                // The host switches a playable, unheld row between Open (a human may take it) and AI (the campaign difficulty).
                if (impl.host && staging && !checkpoint && (open_row || row.kind == model::RowKind::ai) && campaign && has_row && index < campaign->factions.size()
                    && campaign->factions[index].playable) {
                    name->clear();
                    name->add_item(text(open_label), galactic_row_open_id);
                    name->add_item(resolved(resources.text, ai_seat_caption(options->difficulty)), galactic_row_ai_id);
                    name->select(open_row ? 0 : 1);
                    name->set_disabled(false);
                }
                // The host removes a guest from a row with its kick entry, as in a skirmish room.
                if (impl.host && room && row.kind == model::RowKind::human) {
                    for (const auto& seat : room->snapshot.seats) {
                        if (!has_row || seat.selection.slot != options->rows[index].player || seat.peer == room->snapshot.host) continue;
                        name->add_item(resolved(resources.text, "TEXT_KICK") + " " + text(seat.selection.name), static_cast<int>(10000 + seat.peer));
                        name->set_disabled(!staging);
                    }
                }
            }
        }
        if (faction != nullptr) {
            faction->clear();
            faction->set_visible(shown);
            if (shown) {
                // A player's own row lists every faction a player may hold: picking one moves the seat to that row.
                if (mine) {
                    for (const auto& choice : viewer::galactic::faction_choices(*options, *resources.content->catalog,
                            resources.text ? &*resources.text : nullptr))
                        faction->add_item(text(choice.caption), static_cast<int>(choice.row));
                } else {
                    faction->add_item(text(resources.text ? model::faction_display_name(*resources.content->catalog,
                        &*resources.text, row.faction) : row.faction), static_cast<int>(index));
                }
                for (int item = 0; item < faction->get_item_count(); ++item)
                    if (faction->get_item_id(item) == static_cast<int>(index)) faction->select(item);
                faction->set_disabled(!mine);
            }
        }
        if (colour != nullptr) { colour->clear(); colour->set_visible(false); }
        if (team != nullptr) {
            team->clear();
            team->set_visible(shown && row.team.has_value());
            if (shown && row.team) {
                team->add_item(text("Team " + std::to_string(*row.team + 1U)), static_cast<int>(*row.team));
                team->select(0);
            }
            team->set_disabled(true);
        }
        if (auto* kick = EAWR_CONTROL(EawrUiButton, "IDC_BUTTON_KICK_PLAYER" + suffix)) {
            const bool remote = room && options && has_row && std::any_of(room->snapshot.seats.begin(), room->snapshot.seats.end(),
                [&](const auto& seat) { return seat.selection.slot == options->rows[index].player && seat.peer != room->snapshot.host; });
            kick->set_visible(shown && impl.host && remote);
            kick->set_disabled(!staging);
        }
        const auto icon = state.accept_icons.find("IDC_ACCEPT_PLAYER_" + suffix);
        if (icon != state.accept_icons.end()) {
            icon->second->set_visible(shown && row.occupied());
            icon->second->set_color(row.accepted ? Color(0.2F, 0.85F, 0.3F, 0.95F) : Color(0.15F, 0.15F, 0.15F, 0.9F));
        }
        RowState row_state;
        row_state.kind = row.kind == model::RowKind::human ? "human" : row.kind == model::RowKind::ai ? "ai"
                       : row.kind == model::RowKind::open ? "open" : "none";
        row_state.name = row.name;
        row_state.faction = row.faction;
        row_state.team = row.team ? std::to_string(*row.team + 1) : std::string{};
        row_state.accepted = row.accepted;
        row_state.enabled = mine;
        row_state.visible = shown;
        impl.row_states.push_back(std::move(row_state));
    }

    // The map list is the campaign list for the host; a joiner reads the host's choices as a summary.
    if (auto* list = EAWR_CONTROL(EawrUiList, "IDC_STAGING_MAP_LIST")) {
        list->clear();
        impl.campaign_list.clear();
        if (impl.host && impl.galactic_backend && impl.galactic_backend->content_ready()) {
            int selected = -1;
            bool tab_has_campaigns = false;
            for (const auto& entry : impl.galactic_backend->campaigns()) {
                const bool in_tab = viewer::campaign_in_tab(entry.single_player_rules, impl.coop_campaign_tab);
                tab_has_campaigns = tab_has_campaigns || in_tab;
                if (!in_tab && entry.name != impl.galactic_campaign) continue;
                if (entry.name == impl.galactic_campaign) selected = static_cast<int>(impl.campaign_list.size());
                impl.campaign_list.push_back(entry.name);
                const String title = text(eawr::campaign::lobby_campaign_row_title(
                    entry, campaign_labels(resources.text, entry, impl.campaign_text_logged).title));
                list->add_item(title);
                list->set_item_tooltip(list->get_item_count() - 1, title);
            }
            if (selected >= 0) list->select(selected);
            for (int item = 0; item < list->get_item_count(); ++item) list->set_item_disabled(item, !staging || checkpoint);
            if (!tab_has_campaigns) {
                list->add_item(text(impl.coop_campaign_tab ? "No co-op campaigns available." : "No multiplayer campaigns available."));
                list->set_item_disabled(list->get_item_count() - 1, true);
            }
        } else if (options) {
            std::vector<String> lines;
            lines.push_back(String("Campaign: ") + (campaign != nullptr
                ? text(eawr::campaign::lobby_campaign_row_title(*campaign, campaign_labels(resources.text, *campaign, impl.campaign_text_logged).title))
                : text(options->campaign)));
            lines.push_back(text("Difficulty: " + options->difficulty));
            if (options->custom) {
                lines.push_back(resolved(resources.text, "TEXT_SKIRMISH_CREDITS") + ": " + text(std::to_string(options->credits)));
                lines.push_back(resolved(resources.text, "TEXT_SKIRMISH_STARTING_TECH_LEVEL") + ": " + text(std::to_string(options->starting_tech)));
                lines.push_back(resolved(resources.text, "TEXT_SKIRMISH_MAX_TECH_LEVEL") + ": " + text(std::to_string(options->maximum_tech)));
            }
            lines.push_back(resolved(resources.text, "TEXT_ALLOW_HEROES") + ": " + (options->allow_heroes ? "On" : "Off"));
            lines.push_back(text("Auto-resolve: " + std::to_string(options->auto_resolve)));
            for (const auto& line : lines) {
                list->add_item(line, Ref<Texture2D>(), false);
                list->set_item_disabled(list->get_item_count() - 1, true);
                list->set_item_tooltip(list->get_item_count() - 1, line);
            }
        }
    }
    if (auto* heading = EAWR_CONTROL(EawrUiLabel, "IDC_GAME_OPTIONS_CLIENT_DISPLAY_LEFT")) heading->set_text(impl.host ? "Campaign" : "Game settings");
    const auto labels = campaign != nullptr ? campaign_labels(resources.text, *campaign, impl.campaign_text_logged)
                                            : eawr::campaign::MpLobbyCampaignLabels{impl.galactic_campaign, {}, {}};
    if (auto* name = EAWR_CONTROL(EawrUiLabel, "EAWR_JOINER_MAP_NAME")) name->set_text(text(labels.title));
    if (auto* description = EAWR_CONTROL(EawrUiLabel, "EAWR_CAMPAIGN_DESCRIPTION")) {
        description->set_text(text(labels.description));
        description->set_visible(true);
    }
    state.loading = false;
}

bool EawrLanLobby::galactic_item_selected(const int64_t index, const std::string& gadget) {
    Impl& impl = *impl_;
    if (state_->loading || !impl.galactic_options) return true;
    if (impl.checkpoint_candidate || (impl.galactic_backend && impl.galactic_backend->checkpoint_view())) return true;
    const auto room = match_->lobby_state();
    if (!room || room->snapshot.phase != net::LobbyPhase::staging) return true;
    const auto local = std::find_if(room->snapshot.seats.begin(), room->snapshot.seats.end(),
        [&](const auto& seat) { return seat.peer == match_->view().seat; });
    if (gadget == "IDC_STAGING_MAP_LIST") {
        // The host picks a campaign: a fresh set of options, the room's seed and delay kept.
        if (!impl.host || !impl.galactic_backend || !impl.galactic_backend->content_ready() || index < 0 || static_cast<std::size_t>(index) >= impl.campaign_list.size()) return true;
        const auto& name = impl.campaign_list[static_cast<std::size_t>(index)];
        if (name == impl.galactic_options->campaign) return true;
        for (const auto& campaign : impl.galactic_backend->campaigns()) {
            if (campaign.name != name) continue;
            auto fresh = net::default_galactic_lobby_options(campaign, impl.galactic_options->difficulty, impl.galactic_options->seed,
                impl.galactic_options->input_delay);
            if (!fresh) { note_chat(model::ChatKind::error, fresh.error().message); break; }
            if (!match_->lobby_galactic_game(fresh.value())) {
                if (const auto lobby = match_->lobby_state(); lobby && lobby->notice) note_chat(model::ChatKind::error, lobby->notice->message);
            }
            break;
        }
        impl.rows_signature.clear();
        return true;
    }
    const auto split = gadget.rfind('_');
    if (split == std::string::npos) return true;
    const std::string field = gadget.substr(0, split);
    auto* combo = EAWR_CONTROL(EawrUiCombo, gadget);
    if (combo == nullptr) return true;
    if (field == "IDC_PLAYER_NAME") {
        const int id_value = combo->get_item_id(static_cast<int>(index));
        if (id_value == galactic_row_open_id || id_value == galactic_row_ai_id) {
            // Host-only: re-publish the options with this row's human flag toggled; every peer sees the same row.
            const std::size_t slot = static_cast<std::size_t>(std::strtoul(gadget.c_str() + split + 1, nullptr, 10));
            // The gadget is a dialog row; the campaign row on it is the one the roster drew there.
            const std::size_t row = model::galactic_row_at(impl.galactic_row_order, slot).value_or(impl.galactic_options->rows.size());
            if (impl.host && row < impl.galactic_options->rows.size()
                && impl.galactic_options->rows[row].human != (id_value == galactic_row_open_id)) {
                auto edited = *impl.galactic_options;
                edited.rows[row].human = id_value == galactic_row_open_id;
                if (!match_->lobby_galactic_game(edited)) {
                    if (const auto lobby = match_->lobby_state(); lobby && lobby->notice) note_chat(model::ChatKind::error, lobby->notice->message);
                }
            }
            impl.rows_signature.clear();
            return true;
        }
        if (id_value >= 10000 && id_value < galactic_row_open_id && impl.host) (void)match_->lobby_kick(static_cast<net::PeerIndex>(id_value - 10000));
        return true;
    }
    if (field == "IDC_PLAYER_FACTION" && local != room->snapshot.seats.end()) {
        const auto wanted = combo->get_item_id(static_cast<int>(index));
        const auto player = wanted < 0 ? std::nullopt : viewer::galactic::faction_choice_player(
            *impl.galactic_options, static_cast<std::size_t>(wanted));
        if (player && *player != local->selection.slot) {
            auto choice = local->selection;
            choice.slot = *player; // the lobby maps the faction and the side from the row
            if (!match_->lobby_select(std::move(choice))) {
                if (const auto lobby = match_->lobby_state(); lobby && lobby->notice) note_chat(model::ChatKind::error, lobby->notice->message);
            }
        }
        impl.rows_signature.clear();
    }
    return true;
}

} // namespace eawr::presentation::godot_backend
