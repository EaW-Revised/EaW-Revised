#include "lan_lobby_screen_internal.hpp"
#include "viewer_mounted_mods.hpp"

#include "galactic_lobby_backend.hpp"
#include "coop_campaign_tab.hpp"
#include "galactic/faction_choices.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <random>

#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/text_server.hpp>

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
// The informational (disabled) lines of a slot held by several humans (#8020).
constexpr int stack_line_id = 30000;

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
    if (!impl.galactic_backend) {
        const auto content = resources_->content;
        // Loaded off the UI thread, and only when a Campaign room is in sight (host options, a listed or joined campaign).
        impl.galactic_backend = std::make_shared<GalacticLobbyBackend>(GalacticLobbyLoader([content] {
            return load_galactic_lobby_content(*content->filesystem, *content->catalog, content);
        }));
    }
    match_->configure_galactic(impl.galactic_backend);
}

bool EawrLanLobby::prefetch_galactic_content(const bool needs_mod_digests) {
    Impl& impl = *impl_;
    if (!impl.galactic_backend && !(resources_ && resources_->content)) return false;
    if (!impl.galactic_backend) {
        const auto content = resources_->content;
        impl.galactic_backend = std::make_shared<GalacticLobbyBackend>(GalacticLobbyLoader([content] {
            return load_galactic_lobby_content(*content->filesystem, *content->catalog, content);
        }));
    }
    GalacticLobbyBackend::prefetch_content(impl.galactic_backend);
    // Startup requests the digests; this gate only checks their progress.
    const bool content_ready = impl.galactic_backend->content_ready();
    const auto gate = model::galactic_press_gate(content_ready,
        needs_mod_digests ? model::GalacticPress::load_game : model::GalacticPress::host, [] { return mounted_mods_ready(); });
    const bool digests_settled = gate != model::GalacticContentGate::checking_mod_files;
    const auto now = std::chrono::steady_clock::now();
    if (gate == model::GalacticContentGate::open) {
        if (impl.content_wait_since) {
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(now - *impl.content_wait_since).count();
            const auto line = "EAWR lobby: campaign content gate open after " + std::to_string(waited) + " ms (mod digests "
                + (needs_mod_digests ? "waited for" : "not needed") + ")";
            UtilityFunctions::print(String::utf8(line.data(), static_cast<std::int64_t>(line.size())));
            impl.content_wait_since.reset();
        }
        return true;
    }
    if (!impl.content_wait_since) {
        impl.content_wait_since = now;
        const auto progress = mounted_mods_progress();
        const auto line = std::string("EAWR lobby: campaign content gate closed: ")
            + (gate == model::GalacticContentGate::loading_content ? "campaign list still loading" : "mod content digests not settled")
            + " (content_ready=" + (content_ready ? "yes" : "no") + " digests_settled=" + (digests_settled ? "yes" : "no")
            + " needs_mod_digests=" + (needs_mod_digests ? "yes" : "no") + " mod files hashed " + std::to_string(progress.files_done)
            + " of " + std::to_string(progress.files_total) + ")";
        UtilityFunctions::print(String::utf8(line.data(), static_cast<std::int64_t>(line.size())));
    }
    return false;
}

// Host and Load Game both need the campaign list. While the load runs they show it and return; this finishes them.
// A fresh host does not wait for the mod content digests, Load Game does (galactic_press_gate).
void EawrLanLobby::finish_pending_galactic() {
    Impl& impl = *impl_;
    // The host options page shows the number of humans the room opens for: once the campaign list is known, it is the default (two)
    // unless the host chose another number.
    if (page_ == Page::host_options && impl.host_options.game_type == galactic_game_type && !impl.galactic_humans_chosen
        && !impl.galactic_humans_defaulted && impl.galactic_backend && impl.galactic_backend->content_ready()) fill_host_seats();
    if (!impl.host_pending && !impl.load_pending) { impl.load_digest_wait.reset(); impl.pending_text.clear(); impl.content_wait_since.reset(); return; }
    // A prefetch thread that could not start is retried here; the buttons never wait on a load nobody runs.
    const bool host_ready = !impl.host_pending || prefetch_galactic_content(false);
    const bool load_ready = !impl.load_pending || prefetch_galactic_content(true);
    if (impl.load_pending && !load_ready && impl.galactic_backend && impl.galactic_backend->content_ready() && !mounted_mods_ready()) {
        if (!impl.load_digest_wait) impl.load_digest_wait = mods::ContentDigests::screen_wait_lease();
    } else impl.load_digest_wait.reset();
    if (impl.load_pending && !load_ready) {
        // A big mod's first check reads every file: show how far it is, and redraw only when the line changes.
        const auto progress = mounted_mods_progress();
        const auto gate = model::galactic_press_gate(impl.galactic_backend && impl.galactic_backend->content_ready(),
            model::GalacticPress::load_game, [] { return false; });
        const auto wait_line = model::galactic_content_gate_text(gate, progress.files_done, progress.files_total);
        if (wait_line != impl.pending_text) {
            impl.pending_text = wait_line;
            impl.checkpoint_status = wait_line;
            impl.checkpoint_signature.clear();
        }
    }
    if (!host_ready && !load_ready) return;
    const bool host = impl.host_pending && host_ready;
    const bool load = impl.load_pending && load_ready;
    if (!host && !load) return;
    if (host) impl.host_pending = false;
    if (load) impl.load_pending = false;
    impl.pending_text.clear();
    if (load) { impl.checkpoint_problem.clear(); impl.checkpoint_status.clear(); impl.checkpoint_signature.clear(); }
    if (host && page_ == Page::host_options) {
        if (auto* button = EAWR_CONTROL(EawrUiButton, "IDC_INTERNET_CREATE_GAME_BUTTON")) {
            button->set_disabled(false);
            button->set_text(text(impl.host_button_caption));
        }
        begin_hosting();
    }
    if (load && page_ == Page::staging) open_checkpoint_list();
}

namespace {
// #6468: the host's mod list will not be shared with the guests. Say so in the lobby and the log, and release Start.
void report_host_mod_list_not_shared(bool& refresh_due, std::string& notice, const std::string& reason) {
    refresh_due = false;
    notice = reason;
    const auto line = "EAWR lobby: host mod list not shared: " + reason;
    UtilityFunctions::print(String::utf8(line.data(), static_cast<std::int64_t>(line.size())));
}
} // namespace

std::optional<std::string> EawrLanLobby::host_galactic_game(const net::GalacticLobbyOptions* preset) {
    Impl& impl = *impl_;
    impl.host_mod_notice.clear(); // a notice belongs to one room: this is where a new room starts, and it may raise one below
    if (!impl.galactic_backend) return std::string("No campaign content is loaded.");
    const auto description = mounted_mod_description();
    const auto encoded = description ? mods::encode_description(*description) : std::nullopt;
    // The room opens now with whatever is known. Digests still being computed are sent as a refresh once they are done.
    impl.host_mod_refresh_due = !mounted_mods_ready();
    impl.host_mod_refresh_retry = {};
    match_->expect_host_mod_refresh(impl.host_mod_refresh_due); // Start waits for the refresh (#6468)
    // The mod list is display only. When it cannot be encoded or the room codec would refuse it, the room still opens and the guest
    // is told the host's list could not be checked (empty bytes mean "not known").
    const auto unsendable = impl.host_mod_refresh_due ? std::string{} : viewer::host_mod_list_unsendable_reason(encoded);
    if (impl.host_mod_refresh_due || unsendable.empty()) {
        // Digests still pending: the room opens with what is known; an unsendable list there is judged when the refresh is due.
        if (encoded && encoded->size() <= net::max_lobby_mod_description_bytes && net::valid_lobby_mod_description(*encoded))
            impl.galactic_backend->set_mounted_mods(*encoded);
        else impl.galactic_backend->set_mounted_mods({});
    } else {
        impl.galactic_backend->set_mounted_mods({});
        report_host_mod_list_not_shared(impl.host_mod_refresh_due, impl.host_mod_notice, unsendable);
    }
    if (!impl.galactic_backend->content_ready()) return std::string("Campaign content is still loading.");
    if (const auto error = impl.galactic_backend->load_error()) return core::format_diagnostic(*error);
    if (preset != nullptr) {
        if (!match_->set_galactic_game(*preset)) return std::string("The campaign room could not be opened.");
        return std::nullopt;
    }
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
    // The host's room plans no human row this dialog cannot draw (net::galactic_lobby_human_row_limit), so a claim that would land on
    // a hidden row is refused with a message instead of seating a player nobody sees. Set before the room is pumped.
    if (impl.host && impl.galactic_backend) impl.galactic_backend->set_dialog_rows(dialog_rows);
    // The galactic room is not built from a skirmish selection: the host's options opened it, edits go through the list.
    match_->pump_setup(skirmish::FixtureOptions{});
    review_host_mods();
    if (impl.host_mod_refresh_due && mounted_mods_ready()) {
        // A nonblocking refresh of the host description once its digests are ready. Joins never wait for it; Start does (the match
        // keeps it off until the refresh went out). A refusal is retried with a growing pause and given up after a few tries, with
        // a visible message, so this never encodes the list every frame.
        const auto now = std::chrono::steady_clock::now();
        if (impl.host_mod_refresh_retry.ready(now)) {
            const auto settled = mounted_mod_description();
            const auto encoded = settled ? mods::encode_description(*settled) : std::nullopt;
            const auto unsendable = viewer::host_mod_list_unsendable_reason(encoded);
            if (!unsendable.empty()) {
                // The list cannot travel: say why instead of skipping it silently, and let Start go ahead, as when a room opens with a
                // list that cannot be sent.
                report_host_mod_list_not_shared(impl.host_mod_refresh_due, impl.host_mod_notice, unsendable);
                match_->expect_host_mod_refresh(false);
            } else if (match_->refresh_host_mod_description(*encoded)) {
                impl.host_mod_refresh_due = false;
            } else if (impl.host_mod_refresh_retry.refused(now)) {
                report_host_mod_list_not_shared(impl.host_mod_refresh_due, impl.host_mod_notice,
                    "The room refused the mod list " + std::to_string(impl.host_mod_refresh_retry.attempts)
                    + " times, so the other players cannot check it against theirs.");
                match_->expect_host_mod_refresh(false);
            }
        }
    }
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
    if (!view.mod_review_pending && view.host_mod_refreshes != impl_->seen_host_mod_refreshes) {
        // The host's digests finished after this joiner was admitted: compare again once the local digests are ready too.
        impl_->seen_host_mod_refreshes = view.host_mod_refreshes;
        if (auto refreshed = mods::decode_description(view.host_mod_refresh)) impl_->deferred_mod_review = std::move(refreshed);
    }
    if (impl_->box) { impl_->review_digest_wait.reset(); return; }
    if (!view.mod_review_pending) {
        impl_->review_digest_wait.reset();
        std::string message;
        std::string caption = "Mods differ";
        if (!impl_->host_mod_notice.empty()) {
            // Host: the settled mod list could not be sent (see refresh_galactic_staging).
            message = std::move(impl_->host_mod_notice); impl_->host_mod_notice.clear(); caption = "Mod list not shared";
        } else {
            if (!impl_->deferred_mod_review || !mounted_mods_ready()) return;
            const auto local = mounted_mod_description();
            message = local ? viewer::mod_mismatch_message(*impl_->deferred_mod_review, *local) : std::string{};
            impl_->deferred_mod_review.reset();
        }
        if (message.empty()) return;
        model::MessageBoxRequest request;
        request.caption = caption;
        request.lines[0] = message; request.literal_lines[0] = true;
        request.buttons.push_back({"TEXT_BUTTON_OK", 1, false});
        Resources& resources = *resources_;
        const MessageBoxContext context{&*resources.dialogs, &*resources.theme_model,
            resources.text ? &*resources.text : nullptr, resources.theme, resources.theme_size};
        auto* box = memnew(EawrMessageBox);
        if (!box->open(*this, context, request)) { memdelete(box); return; }
        box->set_scrollable_text(message);
        impl_->box = box;
        layout_page();
        box->connect("result_chosen", callable_mp(this, &EawrLanLobby::on_box_result));
        return;
    }
    const auto host = mods::decode_description(view.host_mod_description);
    const auto local = mounted_mod_description();
    const bool cheap_match = host && local && mods::matching_portable_trees(*host, *local);
    if (!cheap_match && !mounted_mods_ready()) {
        if (!impl_->review_digest_wait) impl_->review_digest_wait = mods::ContentDigests::screen_wait_lease();
        return;
    }
    impl_->review_digest_wait.reset();
    const auto settled = mounted_mod_description();
    auto message = host && settled ? viewer::mod_mismatch_message(*host, *settled)
        : std::string("The host's mod list could not be checked. Both players need the same mods.");
    if (message.empty() && !cheap_match && host && settled && viewer::mod_comparison_incomplete(*host, *settled))
        message = "The mod lists could not be fully checked. Both players need the same mods.";
    if (message.empty()) {
        if (cheap_match && !mounted_mods_ready()) impl_->deferred_mod_review = host;
        match_->accept_mod_review(); return;
    }
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
    // A row no player can take and whose AI seat runs no controller (Neutral, or an ambient faction the entry assigns
    // nothing, GCMP-AI-03) is not drawn: it has neither a player nor an AI to name.
    const auto unseated = [&](std::size_t index) {
        if (campaign == nullptr || index >= campaign->factions.size()) return false;
        const auto& faction = campaign->factions[index];
        return !net::galactic_lobby_row_drawn(faction);
    };
    std::vector<model::GalacticRowFacts> facts;
    if (options) {
        for (std::size_t index = 0; index < options->rows.size(); ++index) {
            const bool known = campaign != nullptr && index < campaign->factions.size();
            facts.push_back({options->rows[index].human, known && campaign->factions[index].playable, unseated(index)});
        }
    }
    impl.galactic_row_order = model::galactic_row_order(facts, dialog_rows);
    const auto& order = impl.galactic_row_order;
    const std::size_t shown_rows = order.size();
    // D8 (#8071): the factions the dialog has no row for are named by one line on the row after the last drawn one.
    const std::string hidden_line = model::galactic_hidden_rows_line(facts, order);
    // #8020: the humans of a slot, one entry each (name and mods state), in seat order.
    struct SlotPlayer final { net::PeerIndex peer{}; std::string name; std::string line; std::array<std::string, 3> tags; };
    std::vector<std::vector<SlotPlayer>> slot_players(dialog_rows);
    for (std::size_t slot = 0; slot < shown_rows; ++slot) {
        const std::size_t index = order[slot]; // the campaign row drawn on dialog row `slot`
        const auto& source = options->rows[index];
        model::StagingRow& row = rows[slot];
        row.faction = source.faction;
        if (const auto side = net::galactic_lobby_side(*options, source.player)) row.team = *side - 1U;
        row.kind = unseated(index) ? model::RowKind::none : source.human ? model::RowKind::open : model::RowKind::ai;
        row.accepted = row.kind == model::RowKind::ai;
        if (row.kind == model::RowKind::ai) row.name = ai_seat_caption(options->difficulty);
        bool first = true;
        for (const auto& seat : room->snapshot.seats) {
            if (seat.selection.slot != source.player) continue;
            if (first) row.name.clear();
            row.kind = model::RowKind::human;
            // Owner 2026-11-19: every player's mod check is visible next to the name, in the host's and in every guest's room.
            const auto mod_check = std::find_if(room->mod_checks.begin(), room->mod_checks.end(), [&](const auto& entry) { return entry.peer == seat.peer; });
            const bool tag_known = room->mod_checks_known && mod_check != room->mod_checks.end();
            const bool tag_done = mod_check != room->mod_checks.end() && mod_check->done;
            const std::string line = seat.selection.name + model::mod_check_tag(tag_known, tag_done);
            row.name += (first ? "" : ", ") + line;
            if (slot < slot_players.size()) slot_players[slot].push_back({seat.peer, seat.selection.name, line, model::mod_check_tag_tiers(tag_known, tag_done)});
            row.local = row.local || seat.peer == view.seat;
            row.accepted = first ? seat.ready : row.accepted && seat.ready;
            first = false;
        }
    }

    // Refill the combos only when what they show changed: a refill closes an open drop-down.
    std::string signature = std::string(impl.host ? "GH" : "GJ") + (checkpoint ? "saved" : "new") + std::to_string(shown_rows) + hidden_line + (staging ? "s" : "-")
        + (options ? options->campaign : std::string{}) + (impl.coop_campaign_tab ? "|coop" : "|mp") + "|" + std::to_string(view.seat);
    for (const model::StagingRow& row : rows)
        signature += "|" + std::to_string(static_cast<int>(row.kind)) + "," + row.name + "," + row.faction + ","
            + (row.team ? std::to_string(*row.team) : std::string("-")) + "," + (row.accepted ? "a" : "p") + (row.local ? "l" : "r");
    if (options) {
        signature += "#" + std::to_string(options->custom) + "," + std::to_string(options->credits) + "," + std::to_string(options->starting_tech)
            + "," + std::to_string(options->maximum_tech) + "," + std::to_string(options->allow_heroes) + "," + std::to_string(options->auto_resolve) + "," + std::to_string(options->vote_timeout) + "," + options->difficulty;
    }
    impl.rows = rows;
    impl.visible_rows = shown_rows;
    if (signature == impl.rows_signature) return;
    impl.rows_signature = signature;
    state.loading = true;
    impl.row_states.clear();
    // #8020: a slot held by 2+ humans draws one name cell per human, one below the other. The rows under it move down by
    // the added cells, as far as the dialog's own last row lets them (the band under the rows is never entered); the
    // single-human slots and every slot without a stack stay on their authored rectangles.
    std::vector<std::size_t> cells(dialog_rows, 1U);
    std::vector<float> shift(dialog_rows, 0.0F);
    float pitch = 0.0F;
    if (dialog_rows >= 2U && shown_rows > 0U) {
        const auto* first = find_gadget(*state.dialog, "IDC_PLAYER_NAME_0");
        const auto* second = find_gadget(*state.dialog, "IDC_PLAYER_NAME_1");
        const auto* last = find_gadget(*state.dialog, "IDC_PLAYER_NAME_" + std::to_string(dialog_rows - 1U));
        const auto* drawn_last = find_gadget(*state.dialog, "IDC_PLAYER_NAME_" + std::to_string(shown_rows - 1U));
        if (first != nullptr && second != nullptr && last != nullptr && drawn_last != nullptr) {
            pitch = local_rect(*state.dialog, *second).position.y - local_rect(*state.dialog, *first).position.y;
            const float slack = local_rect(*state.dialog, *last).position.y - local_rect(*state.dialog, *drawn_last).position.y;
            std::size_t budget = pitch > 0.0F ? static_cast<std::size_t>(std::max(0.0F, slack / pitch + 0.01F)) : 0U;
            float moved = 0.0F;
            for (std::size_t slot = 0; slot < shown_rows; ++slot) {
                shift[slot] = moved;
                const std::size_t want = slot_players[slot].size() >= 2U ? slot_players[slot].size() - 1U : 0U;
                const std::size_t give = std::min(want, budget);
                budget -= give;
                cells[slot] = 1U + give;
                moved += static_cast<float>(give) * pitch;
            }
        }
    }
    // What a stacked cell shows for one player: the name with the longest mods note that fits the box whole, the shorter notes
    // next, and only then the name cut with an ellipsis in front of the shortest note (the note is never the part that is cut).
    // The pop-down button takes the box height. `whole` says the name was not cut.
    const auto cell_text = [](const EawrUiCombo& combo, const SlotPlayer& player) {
        const Ref<Font> font = combo.get_theme_font("font");
        const int font_size = combo.get_theme_font_size("font_size");
        constexpr float pop_down_button_ratio = 1.3F; // the pop-down button is 1.3 box heights wide
        const float box_width = combo.get_size().x - combo.get_size().y * pop_down_button_ratio;
        const auto wide = [&](const String& value) {
            return font.is_valid() ? font->get_string_size(value, HORIZONTAL_ALIGNMENT_LEFT, -1.0F, font_size).x : 0.0F;
        };
        for (const std::string& tag : player.tags) {
            const String whole = text(player.name + tag);
            if (wide(whole) <= box_width) return std::make_pair(whole, true);
        }
        const String tag = text(player.tags.back());
        const String ellipsis = String::utf8("\xE2\x80\xA6");
        String head = text(player.name);
        while (head.length() > 1 && wide(head + ellipsis + tag) > box_width) head = head.substr(0, head.length() - 1);
        return std::make_pair(head + ellipsis + tag, false);
    };
    for (std::size_t slot = 0; slot < dialog_rows; ++slot) {
        const model::StagingRow& row = rows[slot];
        const std::string suffix = std::to_string(slot);
        const bool shown = slot < shown_rows && row.kind != model::RowKind::none;
        const bool line_slot = slot == shown_rows && !hidden_line.empty();
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
            name->set_visible(shown || line_slot);
            if (line_slot) {
                name->add_item(text(hidden_line), 0);
                name->select(0);
                name->set_tooltip_text(text(hidden_line));
                name->set_disabled(true);
            }
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
        // #8020: the stacked cells of a slot held by 2+ humans. Every cell lists all the humans (one per line, not selectable)
        // and a Kick entry per non-host player, and shows its own player; the first cell is the row's own name combo.
        const auto& players = slot_players[slot];
        const bool stacked = shown && has_row && players.size() >= 2U;
        const auto place = [&](Control* control, const std::string& gadget_id) {
            if (control == nullptr) return;
            if (const auto* authored = find_gadget(*state.dialog, gadget_id))
                control->set_position(local_rect(*state.dialog, *authored).position + Vector2(0.0F, shift[slot]));
        };
        RowState row_state;
        const auto fill_stack = [&](EawrUiCombo& combo, const std::size_t selected) {
            combo.clear();
            std::string everyone;
            for (std::size_t j = 0; j < players.size(); ++j) {
                combo.add_item(text(players[j].line), static_cast<int>(stack_line_id + j));
                combo.set_item_disabled(static_cast<int>(j), true);
                everyone += (j ? "\n" : "") + players[j].line;
            }
            bool kickable = false;
            if (impl.host && room)
                for (const auto& player : players) {
                    if (player.peer == room->snapshot.host) continue;
                    combo.add_item(resolved(resources.text, "TEXT_KICK") + " " + text(player.name), static_cast<int>(10000 + player.peer));
                    kickable = true;
                }
            combo.select(static_cast<int>(selected));
            combo.set_tooltip_text(text(everyone));
            combo.set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
            combo.set_disabled(!(kickable && staging));
        };
        if (name != nullptr) {
            name->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
            place(name, "IDC_PLAYER_NAME_" + suffix);
            if (const auto* authored_name = find_gadget(*state.dialog, "IDC_PLAYER_NAME_" + suffix))
                if (const auto* authored_colour = find_gadget(*state.dialog, "IDC_PLAYER_COLOR_" + suffix)) {
                    auto size = local_rect(*state.dialog, *authored_name).size;
                    size.x += local_rect(*state.dialog, *authored_colour).size.x / 2.0F;
                    name->set_size(size);
                }
            if (stacked) {
                fill_stack(*name, 0U);
                const auto shown_text = cell_text(*name, players[0]);
                name->set_text(shown_text.first);
                row_state.cell_texts.push_back(utf8(shown_text.first));
                row_state.line_fits.push_back(shown_text.second ? 1 : 0);
            }
        }
        for (std::size_t j = 1; j < dialog_rows; ++j) {
            const std::string line_id = "EAWR_SLOT_LINE_" + suffix + "_" + std::to_string(j);
            auto* cell = EAWR_CONTROL(EawrUiCombo, line_id);
            const bool used = stacked && j < cells[slot];
            if (cell == nullptr) {
                if (!used || name == nullptr || name->get_parent() == nullptr) continue;
                cell = memnew(EawrUiCombo);
                cell->set_name(text(line_id));
                cell->set_theme_type_variation(name->get_theme_type_variation());
                cell->set_fit_to_longest_item(false);
                cell->set_clip_text(true);
                cell->set_retail_list(true);
                cell->set_list_height(name->get_list_height());
                cell->set_size(name->get_size());
                // A pick on a stacked cell is handled as a pick on the row's name combo (the lists are identical).
                cell->connect("item_selected", callable_mp(this, &EawrLanLobby::on_item_selected).bind(text("IDC_PLAYER_NAME_" + suffix)));
                name->get_parent()->add_child(cell);
                state.controls[line_id] = cell;
            }
            cell->set_visible(used);
            if (!used || name == nullptr) continue;
            cell->set_size(name->get_size());
            place(cell, "IDC_PLAYER_NAME_" + suffix);
            cell->set_position(cell->get_position() + Vector2(0.0F, static_cast<float>(j) * pitch));
            fill_stack(*cell, j);
            const auto shown_text = cell_text(*cell, players[j]);
            cell->set_text(shown_text.first);
            row_state.cell_texts.push_back(utf8(shown_text.first));
            row_state.line_fits.push_back(shown_text.second ? 1 : 0);
        }
        if (stacked)
            for (const auto& player : players) row_state.lines.push_back(player.line);
        row_state.cells = stacked ? cells[slot] : 1U;
        if (stacked && name != nullptr)
            for (int item = 0; item < name->get_item_count(); ++item) row_state.items.push_back(utf8(name->get_item_text(item)));
        if (name != nullptr) row_state.top = name->get_position().y;
        if (faction != nullptr) {
            faction->clear();
            faction->set_visible(shown);
            if (shown) {
                // A player's own row lists every faction a player may hold: picking one moves the seat to that row.
                if (mine) {
                    // A free playable faction is offered too (#6134); the host moves the rows when the seat takes it.
                    const auto choices = campaign && room
                        ? viewer::galactic::faction_choices(*options, *resources.content->catalog, resources.text ? &*resources.text : nullptr,
                            *campaign, room->snapshot, match_->view().seat, dialog_rows)
                        : viewer::galactic::faction_choices(*options, *resources.content->catalog, resources.text ? &*resources.text : nullptr);
                    for (const auto& choice : choices)
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
            // A stacked slot kicks per player (each cell's list), so its single Kick button stays out.
            kick->set_visible(shown && impl.host && remote && !stacked);
            kick->set_disabled(!staging);
            place(kick, "IDC_BUTTON_KICK_PLAYER" + suffix);
        }
        place(faction, "IDC_PLAYER_FACTION_" + suffix);
        if (faction != nullptr)
            if (const auto* authored_faction = find_gadget(*state.dialog, "IDC_PLAYER_FACTION_" + suffix))
                if (const auto* authored_colour = find_gadget(*state.dialog, "IDC_PLAYER_COLOR_" + suffix)) {
                    const float extra = local_rect(*state.dialog, *authored_colour).size.x / 2.0F;
                    auto size = local_rect(*state.dialog, *authored_faction).size;
                    size.x += extra;
                    faction->set_size(size);
                    faction->set_position(faction->get_position() + Vector2(extra, 0.0F));
                }
        place(colour, "IDC_PLAYER_COLOR_" + suffix);
        place(team, "IDC_PLAYER_TEAM_" + suffix);
        const auto icon = state.accept_icons.find("IDC_ACCEPT_PLAYER_" + suffix);
        if (icon != state.accept_icons.end()) {
            if (const auto* authored = find_gadget(*state.dialog, "IDC_ACCEPT_PLAYER_" + suffix))
                icon->second->set_position(local_rect(*state.dialog, *authored).position + Vector2(2.0F, 2.0F + shift[slot]));
            icon->second->set_visible(shown && row.occupied());
            icon->second->set_color(row.accepted ? Color(0.2F, 0.85F, 0.3F, 0.95F) : Color(0.15F, 0.15F, 0.15F, 0.9F));
        }
        row_state.kind = row.kind == model::RowKind::human ? "human" : row.kind == model::RowKind::ai ? "ai"
                       : row.kind == model::RowKind::open ? "open" : line_slot ? "more" : "none";
        row_state.name = line_slot ? hidden_line : row.name;
        row_state.faction = row.faction;
        row_state.team = row.team ? std::to_string(*row.team + 1) : std::string{};
        row_state.accepted = row.accepted;
        row_state.enabled = mine;
        row_state.visible = shown || line_slot;
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
            lines.push_back(text("Battle vote timeout: " + std::to_string(options->vote_timeout) + " s"));
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
            // The room's seats follow the campaign (a host pick stays while the campaign can start with it). Nobody else is in the room:
            // it is opened again with the new number (its connections are fixed when it opens). With guests seated the number stays, and
            // the host is told when the campaign has more player factions than the room has seats.
            const auto wanted = galactic_wanted_humans(campaign);
            const bool online = impl.join_selection.source() == model::LobbySourceKind::online;
            if (wanted != impl.human_owners.size() && room->snapshot.seats.size() == 1 && !online) {
                reopen_galactic_room(std::move(fresh).value(), wanted);
                return true;
            }
            if (wanted > impl.human_owners.size())
                note_chat(model::ChatKind::normal, "This room holds " + std::to_string(impl.human_owners.size()) + " players; the campaign has room for "
                    + std::to_string(wanted) + ". Open the room again to hold more.");
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
        const sim::galactic::MpLobbyCampaign* offered = nullptr;
        if (impl.galactic_backend && impl.galactic_backend->content_ready())
            for (const auto& candidate : impl.galactic_backend->campaigns())
                if (candidate.name == impl.galactic_options->campaign) offered = &candidate;
        const auto player = wanted < 0 ? std::nullopt
            : offered ? viewer::galactic::faction_choice_player(*impl.galactic_options, static_cast<std::size_t>(wanted), *offered)
                      : viewer::galactic::faction_choice_player(*impl.galactic_options, static_cast<std::size_t>(wanted));
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
