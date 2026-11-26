"""The LAN multiplayer screens (MNU-26 to MNU-29, docs/ui/menu-screens.md, #2940).

`--eawr-lan-lobby` builds the LAN lobby, the host options, the staging and the direct-connect page
from the dialog resources, drives them with scripted steps (`--eawr-lan-do`) and writes a report of
what each screen showed. The engine-free half (filter, sort, join checks, staging rows, start and
accept checks, the game source seam) is the root CTest `ui_lan_lobby_contracts`; the presence, chat and
game-list datagrams are `net_lan_directory_contracts`.

The structural half runs everywhere. The graphical half is opt-in: set
EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT. It extracts the four
faces from that install into a temporary cache, which is deleted after the run. Set
EAWR_LAN_LOBBY_CAPTURE_DIR to keep the captures for an eye check.

The fixed-list case is the lobby's contract with its game source: the games a fixed source lists are
sorted, filtered and joined, and the player's presence and chat go back to the source.

The two-viewer case is the discovery contract: one viewer hosts a game, the other sees it in its game
list over UDP, joins it over TCP, and both stagings show both players and the chat line each sent.
"""

import json
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import source_text  # noqa: E402
from dialog_rectangles import assert_dialog_rectangles  # noqa: E402
from test_load_game import checkpoint_fixture  # noqa: E402

SRC = ROOT / "apps/viewer/src"
TOOL = ROOT / "tools/fonts/extract_eaw_fonts.py"

RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")


class LanLobbyStructure(unittest.TestCase):
    def test_empty_faction_list_has_no_selectable_placeholder(self):
        refresh = (SRC / "lan_lobby_screen_refresh.cpp").read_text(encoding="utf-8")
        combo = refresh.split("if (faction != nullptr) {", 1)[1].split("if (colour != nullptr)", 1)[0]
        self.assertEqual(combo.count("faction->add_item("), 1)
        self.assertIn("for (const std::string& value : factions)", combo)
        self.assertIn("faction->set_disabled(!choices_editable || factions.empty())", combo)

    def test_host_setup_faction_captions_come_from_the_active_text_chain(self):
        # #5452: the online host setup showed identities (Rebel/Empire) instead of the mod's own faction text.
        refresh = (SRC / "lan_lobby_screen_refresh.cpp").read_text(encoding="utf-8")
        combo = refresh.split("if (faction != nullptr) {", 1)[1].split("if (colour != nullptr)", 1)[0]
        self.assertIn("faction_display_name(", combo)
        self.assertNotIn("faction->add_item(text(value))", combo)
        selected = (SRC / "lan_lobby_screen.cpp").read_text(encoding="utf-8")
        # A picked entry maps back to the identity by position, never by reading the caption.
        self.assertNotIn("choice.faction = utf8(combo->get_item_text", selected)
        self.assertNotIn("record->faction = utf8(combo->get_item_text", selected)

    def test_staging_faction_choices_always_use_playable_catalog(self):
        source = (SRC / "lan_lobby_screen_refresh.cpp").read_text(encoding="utf-8")
        roster = source.split("void EawrLanLobby::refresh_staging_roster(", 1)[1].split(
            "void EawrLanLobby::", 1)[0]
        faction_list = roster.split("std::vector<std::string> factions;", 1)[1].split("impl.row_states.clear();", 1)[0]
        # #7842: the combo list is the shared per-map helper over the playable catalog (the local setup uses it too).
        self.assertIn("lobby_factions(impl, resources)", roster)
        self.assertIn("for (const auto& entry : listed_factions) factions.push_back(entry.name);", faction_list)
        self.assertNotIn("if (map", faction_list)
        helper = (SRC / "lan_lobby_screen_internal.hpp").read_text(encoding="utf-8")
        helper = helper.split("lobby_factions(const ImplT& impl", 1)[1].split("} // namespace lan_lobby_detail", 1)[0]
        self.assertIn("skirmish::setup_factions(*resources.content->catalog)", helper)
        self.assertIn("skirmish::setup_factions_for_map(all, map)", helper)
        faction_control = roster.split("if (faction != nullptr)", 1)[1].split("if (colour != nullptr)", 1)[0]
        self.assertNotIn("add_item(text(row.faction))", faction_control)

    def test_galactic_options_use_the_gc_publication_path_and_refresh_on_difficulty(self):
        options = (SRC / "lan_lobby_screen_options.cpp").read_text(encoding="utf-8")
        close = options.split("void EawrLanLobby::close_galactic_options(", 1)[1].split("void EawrLanLobby::open_options()", 1)[0]
        self.assertIn("match_->lobby_galactic_game(edited)", close)
        self.assertNotIn("publish_staging_selection", close)
        self.assertNotIn("accept_setup_options", close)
        galactic = (SRC / "lan_lobby_screen_galactic.cpp").read_text(encoding="utf-8")
        signature = galactic.split("if (options) {\n        signature +=", 1)[1].split("impl.rows = rows", 1)[0]
        self.assertIn("options->difficulty", signature)
        self.assertIn("button->set_visible(impl.host)", galactic)

    def test_production_multiplayer_lan_discovery_is_lazy_and_released(self):
        production = (SRC / "skirmish_setup_mode.cpp").read_text(encoding="utf-8")
        online = production.split("void SkirmishSetupMode::enter_online(", 1)[1].split("void SkirmishSetupMode::enter(", 1)[0]
        self.assertIn("options.lan_source_factory =", online)
        self.assertIn("discovery.menu_chat_limits = true", online)
        self.assertIn("BroadcastLobbySource::open", online)
        screen = source_text("apps/viewer/src/lan_lobby_screen.cpp")
        self.assertIn("options_.lan_source_factory(failure)", screen)
        self.assertIn("initial_source != model::LobbySourceKind::online ? lan_source()", screen)
        self.assertIn("if (source == model::LobbySourceKind::online) owned_lan_source_.reset()", screen)
        physical = (SRC / "lan_lobby_screen.cpp").read_text(encoding="utf-8")
        closing = physical.split("void EawrLanLobby::close_screen(", 1)[1].split("void EawrLanLobby::dismiss()", 1)[0]
        self.assertIn("owned_lan_source_.reset()", closing)
        chat = (SRC / "lan_lobby_screen_chat.cpp").read_text(encoding="utf-8")
        self.assertIn("impl_->source->try_send_chat", chat)
        self.assertIn("edit->set_text(text(typed))", chat)

    def test_every_production_match_constructor_configures_loaded_factions(self):
        import re
        constructors = 0
        production_roots = (ROOT / "apps/viewer/src", ROOT / "src/platform/net")
        for path in (path for folder in production_roots for path in folder.rglob("*.cpp")):
            if path.name == "lan_match.cpp":  # Factory definition, not a construction site.
                continue
            source = path.read_text(encoding="utf-8")
            calls = list(re.finditer(r"\bmake_(?:lan|online)_match\s*\(", source))
            assignments = list(re.finditer(r"(?P<receiver>\w+)\s*=\s*make_(?:lan|online)_match\([^;]*\);", source))
            self.assertEqual(len(calls), len(assignments), str(path))
            for call in assignments:
                receiver = call.group("receiver")
                following = source[call.end():]
                logger = re.match(rf"^\s*{receiver}->set_diagnostic_logger\([\s\S]*?\n\s*\}}\);", following)
                if logger:
                    following = following[logger.end():]
                self.assertRegex(following[:180],
                                 rf"^\s*{receiver}->configure_factions\(scene::faction_order\(\*",
                                 str(path))
                constructors += 1
        self.assertGreater(constructors, 0, "production match sites must be discovered")
        screen = source_text("apps/viewer/src/lan_lobby_screen.cpp")
        self.assertRegex(screen, r"match_ = options_\.match;\s*if \(match_\) match_->configure_factions\(scene::faction_order\(\*")

    def test_the_mode_and_the_screen_are_wired_into_the_host_and_the_build(self):
        ready = (SRC / "viewer_host_ready.cpp").read_text(encoding="utf-8")
        self.assertIn("LanLobbyMode::requested()", ready)
        self.assertIn("lan_lobby_mode_->process()", (SRC / "viewer_host_process.cpp").read_text(encoding="utf-8"))
        self.assertIn("register_lan_lobby_classes()", (SRC / "register_types.cpp").read_text(encoding="utf-8"))
        build = source_text("apps/viewer/CMakeLists.txt")
        for source in ("src/lan_lobby_mode.cpp", "src/lan_lobby_screen.cpp"):
            self.assertIn(source, build)
        self.assertIn("lan_lobby.cpp", (ROOT / "src/presentation/ui/sources.cmake").read_text(encoding="utf-8"))
        self.assertIn("src/lan_broadcast_source.cpp", build)
        net = (ROOT / "src/net/sources.cmake").read_text(encoding="utf-8")
        self.assertIn("lan_directory.cpp", net)
        self.assertIn("lan_broadcast.cpp", net)

    def test_the_screen_reuses_the_dialog_builder_and_signals_its_close_and_start(self):
        screen = source_text("apps/viewer/src/lan_lobby_screen.cpp")
        self.assertIn("build_dialog(", screen)
        self.assertIn('ADD_SIGNAL(MethodInfo("closed"))', screen)
        self.assertIn('ADD_SIGNAL(MethodInfo("started"))', screen)
        self.assertIn("EawrUiModalLayer", screen)
        for dialog in ("IDD_LAN_LOBBY_DIALOG", "IDD_INTERNET_HOST_OPTIONS_DIALOG", "IDD_LAN_STAGING_DIALOG",
                       "IDD_LOBBY_GAME_LIST_FILTER_DIALOG", "IDD_IP_DIRECT_CONNECT_DIALOG"):
            self.assertIn(dialog, screen)
        # The untraced 620 by 520 host and join dialogs have no caller in the debug build.
        self.assertNotIn("IDD_LAN_HOST_DIALOG", screen)
        self.assertNotIn("IDD_LAN_JOIN_DIALOG", screen)

    def test_the_staging_row_count_comes_from_the_dialog(self):
        screen = source_text("apps/viewer/src/lan_lobby_screen.cpp")
        self.assertIn("staging_row_count(ids)", screen)
        self.assertNotRegex(screen, r"dialog_rows\s*=\s*\d")

    def test_a_gc_slot_with_several_humans_stacks_one_cell_per_player(self):
        # #8020: the humans of one slot are listed one per line, in the cell and in the dropdown, never on one clipped line.
        galactic = source_text("apps/viewer/src/lan_lobby_screen_galactic.cpp")
        self.assertIn("slot_players[slot].size() >= 2U", galactic)
        self.assertIn("TextServer::OVERRUN_TRIM_ELLIPSIS", galactic)
        self.assertIn("EAWR_SLOT_LINE_", galactic)
        # The cell's mods state is the one the room list already draws, not a second wording.
        self.assertEqual(galactic.count("model::mod_check_tag("), 1)
        self.assertEqual(galactic.count("model::mod_check_tag_tiers("), 1)
        # The row only grows inside the dialog's own last row; the skirmish lobby never shares a slot between humans.
        self.assertIn("local_rect(*state.dialog, *last).position.y - local_rect(*state.dialog, *drawn_last).position.y", galactic)
        self.assertNotIn("EAWR_SLOT_LINE_", source_text("apps/viewer/src/lan_lobby_screen_refresh.cpp"))

    def test_skirmish_humans_have_distinct_map_bounded_rows(self):
        screen = source_text("apps/viewer/src/lan_lobby_screen.cpp")
        self.assertIn("network_row_count(*map)", screen)
        self.assertIn("std::iota(impl_->human_owners.begin(), impl_->human_owners.end(), 1U)", screen)
        self.assertIn("if (impl.host && row.kind != model::RowKind::human)", screen)
        self.assertNotIn("name->set_item_disabled(1, true)", screen)
        self.assertNotIn("Human side", screen)
        self.assertNotIn('" human seats)"', screen)
        self.assertNotIn("human_team_splits(", screen)

    def test_host_count_preserves_the_password_and_teams_belong_to_staging(self):
        screen = source_text("apps/viewer/src/lan_lobby_screen.cpp")
        self.assertNotIn('label->set_text("Human players")', screen)
        self.assertNotIn('gadget == "IDC_INTERNET_HOST_CLAN_COMBO"', screen)
        self.assertNotIn("EAWR_LAN_HOST_TEAMS", screen)
        self.assertIn('impl.host_options.password = online && password != nullptr', screen)
        self.assertIn('argument.find_first_of(":=")', screen)

    def test_the_models_stay_engine_free(self):
        for name in ("include/eawr/presentation/ui/lan_lobby.hpp", "include/eawr/net/lan_directory.hpp",
                     "include/eawr/net/lan_broadcast.hpp"):
            header = (ROOT / name).read_text(encoding="utf-8")
            self.assertNotIn("godot", header.lower(), name)

    def test_the_screen_reads_its_game_list_from_the_source_seam_only(self):
        screen = source_text("apps/viewer/src/lan_lobby_screen.cpp")
        self.assertIn("LobbyGameSource", (SRC / "lan_lobby_screen.hpp").read_text(encoding="utf-8"))
        self.assertIn("impl.source->games()", screen)
        self.assertNotIn("lan_broadcast", screen)
        self.assertNotIn("lan_directory", screen)

    def test_the_lobby_reads_and_writes_the_player_profile_and_keeps_it_out_of_the_match(self):
        mode = (SRC / "lan_lobby_mode.cpp").read_text(encoding="utf-8")
        for call in ("ProfileStore", "effective_network_name(", "set_network_name(", "set_lobby_colour(",
                     "profile_store->save("):
            self.assertIn(call, mode)
        # UI-C2: the screen and the transport never see the profile record.
        # lan_match.cpp is a bundle key: a split extends its tuple, so this reads every LAN match member.
        for name in ("lan_lobby_screen.cpp", "lan_match.cpp"):
            self.assertNotIn("PlayerProfile", source_text(f"apps/viewer/src/{name}"), name)

    def test_only_the_changed_fields_are_written_back_to_the_profile(self):
        mode = (SRC / "lan_lobby_mode.cpp").read_text(encoding="utf-8")
        self.assertIn("if (name_changed) {", mode)
        self.assertIn("if (colour_changed) {", mode)

    def test_every_graphical_launch_names_its_own_profile_and_settings_file(self):
        # No test may fall back to the player's user:// profile or settings.
        source = pathlib.Path(__file__).read_text(encoding="utf-8")
        self.assertIn('"--eawr-profile-file"', source)
        self.assertIn('"--eawr-settings-file"', source)
        # No launch spells out its own argument list: they all come from command().
        self.assertNotRegex(source, r"subprocess\.(run|Popen)\(\s*\[(?!sys\.executable)")

    def test_network_text_is_sanitised_by_the_decoder(self):
        decoder = (ROOT / "src/net/lan_directory.cpp").read_text(encoding="utf-8")
        self.assertIn("sanitize_lan_text(", decoder)
        # Every field read goes through the one sanitising reader.
        self.assertNotIn("value.assign(reinterpret_cast", decoder)

    def test_the_lockstep_transport_is_reused_not_rewritten(self):
        screen = source_text("apps/viewer/src/lan_lobby_screen.cpp")
        for call in ("match_->host(", "match_->join(", "match_->pump_setup(", "match_->accept()", "match_->start(",
                     "match_->take_start()"):
            self.assertIn(call, screen)

    def test_room_chat_is_authoritative_even_before_the_first_snapshot(self):
        screen = source_text("apps/viewer/src/lan_lobby_screen.cpp")
        send = (SRC / "lan_lobby_screen_chat.cpp").read_text(encoding="utf-8").split("void EawrLanLobby::send_chat(", 1)[1].split("void EawrLanLobby::announce", 1)[0]
        self.assertIn("match_->lobby_enabled()", send)
        self.assertNotIn("page_ == Page::staging && match_->lobby_state()", send)
        self.assertIn('note_chat(model::ChatKind::notice, "TEXT_WAITING")', send)
        self.assertIn("(online || !match_->lobby_enabled())", screen)
        self.assertIn("if (!room->identity_pending && local !=", screen)

    def test_staging_uses_the_authoritative_core_and_shared_live_builder(self):
        screen = source_text("apps/viewer/src/lan_lobby_screen.cpp")
        bridge = source_text("apps/viewer/src/lan_match.cpp")
        for call in ("LobbySession::host", "LobbySession::join", "configure_game(", "accept_setup()", "take_result()"):
            self.assertIn(call, bridge)
        self.assertIn("room->snapshot.seats", screen)
        self.assertIn("room->chat", screen)
        for name in ("lan_lobby_screen.cpp", "battle_source_local.cpp"):
            source = source_text(f"apps/viewer/src/{name}")
            self.assertIn("prepare_skirmish(", source)
            self.assertIn("lan_recording_setup(", source)
        self.assertIn("src/platform/net/skirmish_prepare.cpp", source_text("apps/viewer/CMakeLists.txt"))
        self.assertIn("eawr::platform_net", (ROOT / "tests/net/CMakeLists.txt").read_text(encoding="utf-8"))

    def test_online_service_uses_saved_settings_and_live_adapters(self):
        screen = source_text("apps/viewer/src/lan_lobby_screen.cpp")
        service = (ROOT / "src/platform/net/online_service_settings.hpp").read_text(encoding="utf-8")
        self.assertIn("make_live_online_source(configured.value())", screen)
        self.assertIn("make_online_match(std::move(configured).value()", screen)
        self.assertIn("make_unique<net::NetworkOnlineLobby>", service)
        self.assertIn("options.turn_only = settings.online_relay_only", service)
        self.assertIn("match_ = lan_match_", screen)

    def test_the_online_half_binds_the_room_source_into_the_same_screen(self):
        screen = source_text("apps/viewer/src/lan_lobby_screen.cpp")
        header = (SRC / "lan_lobby_screen.hpp").read_text(encoding="utf-8")
        mode = (SRC / "lan_lobby_mode.cpp").read_text(encoding="utf-8")
        self.assertIn("std::shared_ptr<net::OnlineRoomSource> online_rooms;", header)
        # The retired invitation-only seam is gone from the screen.
        for gone in ("OnlineLobbySource", "set_online_source", "private_online_empty_notice"):
            self.assertNotIn(gone, screen)
            self.assertNotIn(gone, header)
        # A private room shows and publishes no invitation code: the screen never reads one.
        self.assertNotIn("set_bridge_invitation", screen)
        self.assertIn("bind_online_room_source(", screen)
        self.assertIn("impl.host_password.empty()", screen)
        self.assertNotIn("invitation_expires", screen)
        # Rows, paging, refresh, the password prompt, the hosting state and the chat window.
        for call in ("online_row_text(", "online_page(", "online_rooms->refresh()", "take_join_results()",
                     "begin_join(*attempt, room, password, impl.name)", "set_host_password(impl.host_password)",
                     "hosting_state()", "take_notices()", "online_menu_chat_notice(", "check_join(selected, impl.name, true)",
                     "drawn.locked = online && game.passworded"):
            self.assertIn(call, screen)
        for control in ("EAWR_LOBBY_REFRESH", "EAWR_LOBBY_PREV_PAGE", "EAWR_LOBBY_NEXT_PAGE", "EAWR_LOBBY_PROMPT_PASSWORD",
                        "EAWR_LOBBY_JOIN_CODE", "EAWR_LOBBY_CANCEL_JOIN"):
            self.assertIn(control, screen)
        self.assertIn("edit->set_secret(true);", (SRC / "lan_lobby_screen_list.cpp").read_text(encoding="utf-8").split("void EawrLanLobby::open_password_prompt", 1)[1])
        self.assertIn("--eawr-online-room", mode)
        self.assertIn("FixedOnlineLobbySource", mode)

    def test_a_campaign_room_opens_for_the_playable_rows_and_lists_the_campaign_title(self):
        # #8130: no "Human players" choice. A campaign room's capacity is the campaign's playable rows (2..8, co-op the seat ceiling), the
        # room opens with staged capacity like a skirmish room, and it starts with the humans present.
        staging = (SRC / "lan_lobby_screen_staging.cpp").read_text(encoding="utf-8")
        internal = (SRC / "lan_lobby_screen_internal.hpp").read_text(encoding="utf-8")
        self.assertIn("net::galactic_lobby_max_humans(*hostable, seat_limit, impl_->staging_dialog_rows)", staging)
        self.assertIn("match_->configure_staging_capacity(true);", staging)
        self.assertNotIn("configure_staging_capacity(false)", staging)
        self.assertNotIn("configure_staging_capacity(impl.host_options.game_type != galactic_game_type)", staging)
        for gone in ("galactic_humans", "galactic_humans_chosen", "galactic_humans_defaulted", "requested_count", "The room waits for this many players"):
            self.assertNotIn(gone, staging + internal)
        self.assertNotIn("galactic_lobby_default_humans", staging)
        chat = (SRC / "lan_lobby_screen_chat.cpp").read_text(encoding="utf-8")
        self.assertIn("campaign_display_name(impl_->galactic_campaign)", chat)

    def test_a_reopened_campaign_room_keeps_the_hosts_mode_and_names_its_size_once(self):
        # #8497: a room opened again (also after leaving) opens on the host's last picked campaign, so Co-op stays Co-op; its co-op
        # capacity follows the staging dialog's rows; the "has room for N" line goes through the once-per-room check.
        staging = (SRC / "lan_lobby_screen_staging.cpp").read_text(encoding="utf-8")
        galactic = (SRC / "lan_lobby_screen_galactic.cpp").read_text(encoding="utf-8")
        self.assertIn("net::galactic_lobby_host_campaign(impl_->galactic_backend->campaigns(), impl_->galactic_host_campaign)", staging)
        self.assertIn("net::galactic_lobby_host_campaign(campaigns, impl.galactic_host_campaign)", galactic)
        self.assertNotIn("first_hostable_lobby_campaign", staging + galactic)
        self.assertIn("impl.galactic_host_campaign = campaign.name;", galactic)
        self.assertIn("impl.staging_dialog_rows = state.dialog_rows;", staging)
        self.assertIn("impl.capacity_noted = 0;", staging)
        self.assertIn("viewer::room_capacity_note_due(held, wanted, impl.capacity_noted)", galactic)

    def test_the_host_tells_the_room_how_many_rows_its_dialog_draws(self):
        # The room plans no human row the dialog cannot draw: the host hands its dialog's row count to the backend before it pumps.
        galactic = (SRC / "lan_lobby_screen_galactic.cpp").read_text(encoding="utf-8")
        self.assertIn("impl.galactic_backend->set_dialog_rows(dialog_rows)", galactic)
        self.assertLess(galactic.index("set_dialog_rows(dialog_rows)"), galactic.index("match_->pump_setup(skirmish::FixtureOptions{})"))
        self.assertIn("*campaign, room->snapshot, match_->view().seat, dialog_rows)", galactic)

    def test_the_online_join_attaches_the_room_scope_with_the_directory_admission(self):
        screen = source_text("apps/viewer/src/lan_lobby_screen.cpp")
        pump = (SRC / "lan_lobby_screen_list.cpp").read_text(encoding="utf-8").split("void EawrLanLobby::pump_online()", 1)[1].split("// ---- Host options", 1)[0]
        self.assertIn("if (result.admission)", pump)
        self.assertIn("match_->join_admitted(*result.admission);", pump)
        self.assertIn("result.admission->member_capability.empty()", pump)
        self.assertIn("open_staging(false, game, {});", pump)
        code = (SRC / "lan_lobby_screen_list.cpp").read_text(encoding="utf-8").split("void EawrLanLobby::join_code(", 1)[1].split("void EawrLanLobby::join_online_selected", 1)[0]
        self.assertIn("match_->join(*canonical);", code)
        self.assertIn("normalize_short_code(code)", code)
        self.assertIn("skirmish::setup_row_count(map, false)", screen)
        self.assertNotIn("direct_seats = 2", pump)
        self.assertNotIn("direct_seats = 2", code)

    def test_online_failures_show_curated_text_and_no_secret_reaches_logs(self):
        screen = source_text("apps/viewer/src/lan_lobby_screen.cpp")
        mode = (SRC / "lan_lobby_mode.cpp").read_text(encoding="utf-8")
        failure = (SRC / "lan_lobby_screen_list.cpp").read_text(encoding="utf-8").split("void EawrLanLobby::online_failure(", 1)[1].split("// One frame of the online half", 1)[0]
        # Every code shows its curated text; only the typed build refusal (1503) keeps its bounded, cleaned detail (#7461).
        self.assertIn("model::online_failure_message(diagnostic.code, diagnostic.message)", failure)
        curated = (ROOT / "src/presentation/ui/lan_lobby.cpp").read_text(encoding="utf-8").split("std::string online_failure_message(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn('if (code != "EAWR-NET-1503") return std::string(online_failure_text(classify_online_failure(code)));', curated)
        self.assertIn("detail.substr(0, 512)", curated)
        pump = (SRC / "lan_lobby_screen_list.cpp").read_text(encoding="utf-8").split("void EawrLanLobby::pump_online()", 1)[1].split("// ---- Host options", 1)[0]
        # A refusal's own message reaches the player only through the curated online_failure_message.
        self.assertNotIn("refusal->message", pump.replace("model::online_failure_message(result.refusal->code, result.refusal->message)", ""))
        self.assertNotIn("format_diagnostic", pump)
        submit = (SRC / "lan_lobby_screen_list.cpp").read_text(encoding="utf-8").split("void EawrLanLobby::submit_password(", 1)[1].split("// A refused service call", 1)[0]
        self.assertNotIn("note_chat(model::ChatKind::normal, password", submit)
        self.assertNotIn("password +", submit)
        self.assertIn("The password goes to the source and nowhere else", submit)
        # The scripted report redacts typed passwords and carries only whether one was handed over.
        self.assertIn('return "password:***";', mode)
        self.assertIn("host_password_set", mode)
        self.assertNotIn("host_password()", mode.replace("fixed_online->host_password().empty()", ""))

    def test_the_online_model_stays_engine_free_and_lan_checks_are_unchanged(self):
        header = (ROOT / "include/eawr/presentation/ui/lan_lobby.hpp").read_text(encoding="utf-8")
        self.assertNotIn("godot", header.lower())
        self.assertIn("bool password_supported = false", header)
        net = (ROOT / "include/eawr/net/online_lobby_source.hpp").read_text(encoding="utf-8")
        self.assertIn("class OnlineRoomSource : public presentation::ui::LobbyGameSource", net)
        self.assertNotIn("godot", net.lower())
        screen = source_text("apps/viewer/src/lan_lobby_screen.cpp")
        # LAN joins still go through the unchanged check without password support.
        self.assertIn("model::check_join(selected, impl.name);", screen)


def free_udp_ports(count):
    sockets = []
    ports = []
    try:
        for _ in range(count):
            probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            probe.bind(("127.0.0.1", 0))
            sockets.append(probe)
            ports.append(probe.getsockname()[1])
    finally:
        for probe in sockets:
            probe.close()
    return ports


def command(cache, name, directory, picture, *extra, resolution="1280x720", engine=()):
    executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
    assert executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary"
    # Every launch gets its own profile and settings file, so no run reads or writes the player's
    # real user:// files (the lobby writes the profile back when the name or colour changes). A test
    # that wants a seeded file names its own, which wins.
    isolated = []
    for flag, suffix in (("--eawr-profile-file", "profile"), ("--eawr-settings-file", "settings")):
        if flag not in extra:
            own = directory / f"{name}.{suffix}.cfg"
            own.unlink(missing_ok=True)
            isolated += [flag, str(own)]
    return [executable, "--resolution", resolution, "--max-fps", "30", *engine, "--path", str(ROOT / "apps/viewer/project"),
            "--", "--eawr-lan-lobby", "--eawr-game-root", GAME_ROOT, "--eawr-font-cache", str(cache),
            "--eawr-report", str(directory / f"{name}.json"), "--eawr-capture", str(picture), *isolated, *extra]


def load(test, directory, name, completed, failed_actions=()):
    report = directory / f"{name}.json"
    test.assertTrue(report.is_file(), completed)
    result = json.loads(report.read_text(encoding="utf-8"))
    test.assertEqual(result["failure"], "action failed: " + failed_actions[-1] if failed_actions else "", result["actions"])
    test.assertEqual(result["status"], "failed" if failed_actions else "ok", result["failure"])
    test.assertEqual([action for action in result["actions"] if not action.endswith(" ok")],
                     [action + " FAILED" for action in failed_actions], result["actions"])
    return result


def wide_campaign_mod(directory, playable, ai_only, small=0):
    """A made-up overlay mod with one multiplayer campaign of `playable` player factions and `ai_only` AI factions (D8, #8071).

    The names are invented here; nothing comes from the game's own files. The campaign is listed first, so a campaign room opens
    on it. Home planets are stock planet names of the installed data.
    """
    planets = ["Yavin", "Coruscant", "Corellia", "Kuat", "Dantooine", "Hypori", "Korriban", "Kashyyyk", "Felucia", "Bothawui",
               "Muunilinst", "Myrkr", "Dathomir", "Ryloth"]
    names = [f"Wide_Player_{index + 1}" for index in range(playable)] + [f"Wide_Ai_{index + 1}" for index in range(ai_only)]
    root = pathlib.Path(directory) / "wide_mod"
    xml = root / "Data" / "XML"
    xml.mkdir(parents=True, exist_ok=True)
    (xml / "FactionFiles.xml").write_text(
        '<?xml version="1.0" ?>\n<Faction_Files>\n<File>Factions.xml</File>\n<File>Expansion_Factions.xml</File>\n'
        '<File>Factions_Wide.xml</File>\n</Faction_Files>\n', encoding="utf-8")
    (xml / "CampaignFiles.xml").write_text(
        '<?xml version="1.0" ?>\n<Campaign_Files>\n<File>Campaigns_Wide.xml</File>\n<File>Campaigns_Underworld_GC.xml</File>\n'
        '<File>Campaigns_Underworld_Story.xml</File>\n<File>Campaigns_Multiplayer.xml</File>\n<File>Campaigns_Tutorial.xml</File>\n'
        '<File>Campaigns_Underworld_Tutorial.xml</File>\n</Campaign_Files>\n', encoding="utf-8")
    factions = ['<?xml version="1.0"?>\n<Factions>']
    for index, name in enumerate(names):
        player = index < playable
        factions.append(
            f'<Faction Name="{name}">\n<Is_Playable>{"true" if player else "false"}</Is_Playable>\n<Is_Neutral>No</Is_Neutral>\n'
            f'<Basic_AI>BasicRebel</Basic_AI>\n<Color>{40 + index * 20}, {200 - index * 15}, {90 + index * 10}, 255</Color>\n</Faction>')
    factions.append('</Factions>')
    (xml / "Factions_Wide.xml").write_text("\n".join(factions) + "\n", encoding="utf-8")
    campaign = ['<?xml version="1.0" ?>\n<Campaigns>\n<Campaign Name="Wide_Room_Test">',
                '<Campaign_Set> Multiplayer_Campaign_Set </Campaign_Set>',
                '<Locations>\n' + ",\n".join(planets[:len(names)] if len(names) <= len(planets) else planets) + '\n</Locations>']
    for index, name in enumerate(names):
        campaign.append(f'<Home_Location> {name}, {planets[index % len(planets)]} </Home_Location>')
    campaign.append('<Is_Multiplayer> Yes </Is_Multiplayer>')
    campaign.append('<Human_Victory_Conditions> Galactic_All_Planets_Controlled </Human_Victory_Conditions>')
    campaign.append('<AI_Victory_Conditions> Galactic_All_Planets_Controlled </AI_Victory_Conditions>')
    for name in names[playable:]:
        campaign.append(f'<AI_Player_Control> {name}, BasicRebel </AI_Player_Control>')
    campaign.append('</Campaign>')
    if small:
        # A second, smaller multiplayer campaign over the first factions: the host switches the room to it.
        campaign += ['<Campaign Name="Small_Room_Test">', '<Campaign_Set> Multiplayer_Campaign_Set </Campaign_Set>',
                     '<Locations>\n' + ",\n".join(planets[:small]) + '\n</Locations>']
        for index, name in enumerate(names[:small]):
            campaign.append(f'<Home_Location> {name}, {planets[index % len(planets)]} </Home_Location>')
        campaign += ['<Is_Multiplayer> Yes </Is_Multiplayer>',
                     '<Human_Victory_Conditions> Galactic_All_Planets_Controlled </Human_Victory_Conditions>',
                     '<AI_Victory_Conditions> Galactic_All_Planets_Controlled </AI_Victory_Conditions>', '</Campaign>']
    campaign.append('</Campaigns>')
    (xml / "Campaigns_Wide.xml").write_text("\n".join(campaign) + "\n", encoding="utf-8")
    return root


def text_overlay_mod(directory, name, pairs):
    """A made-up overlay mod that carries only a text database (Data/Text/MasterTextFile_English.dat) of `pairs` (#8071, D11 follow-up).

    The text database is one file read from the highest mounted layer, so a mod's file stands for the whole text stack of the room:
    one that words the AI captions is a stack with strings, one that words none is a stack without. Layout: count, (crc, length,
    key length) records, UTF-16 values, ASCII keys.
    """
    import struct
    import zlib
    root = pathlib.Path(directory) / name
    path = root / "Data" / "Text" / "MasterTextFile_English.dat"
    path.parent.mkdir(parents=True, exist_ok=True)
    keys = [key.encode("ascii") for key, _ in pairs]
    values = [value.encode("utf-16-le") for _, value in pairs]
    blob = struct.pack("<I", len(pairs))
    for key, value in zip(keys, values):
        blob += struct.pack("<III", zlib.crc32(key) & 0xFFFFFFFF, len(value) // 2, len(key))
    path.write_bytes(blob + b"".join(values) + b"".join(keys))
    return root


def four_row_staging_dialog(mod, rows=4):
    """Give the mod a LAN staging dialog that draws only `rows` player rows (a mod may author fewer than retail's nine).

    The dialog script is the installed game's own (read at test time, nothing is copied into the repository); every control of a
    row past `rows` is dropped from the staging dialog, so the viewer counts `rows` IDC_PLAYER_NAME_n gadgets.
    """
    import re
    sys.path.insert(0, str(ROOT))
    from tools.inventory.corpus import Corpus
    source = Corpus(GAME_ROOT).read_effective("foc", "Data/Resources/GUIDialog/Guidialogs.rc")
    if source is None:
        raise unittest.SkipTest("the installed game has no dialog script")
    codec = "utf-16" if source.data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    try:
        script = source.data.decode(codec)
    except UnicodeDecodeError:
        script = source.data.decode("latin-1")
    dialog = re.search(r"\bIDD_LAN_STAGING_DIALOG\s+DIALOGEX\b.*?\nEND\b", script, re.S)
    if dialog is None:
        raise unittest.SkipTest("the dialog script has no LAN staging dialog")
    # A control statement continues on the next line after a trailing comma or a trailing "|" (a style list): it is kept or
    # dropped as a whole, or a dropped row would leave its style words behind as a statement of their own.
    newline = "\r\n" if "\r\n" in dialog[0] else "\n"
    statements, continued = [], False
    for line in dialog[0].split(newline):
        if continued and statements:
            statements[-1].append(line)
        else:
            statements.append([line])
        continued = line.rstrip().endswith((",", "|"))
    row_of = re.compile(r"\bIDC_[A-Z_]*?_(\d+)\b")  # the per-row gadgets: name, faction, colour, team, accept
    kept = [line for statement in statements
            if not any(int(index) >= rows for line in statement for index in row_of.findall(line))
            for line in statement]
    if len(re.findall(r"\bIDC_PLAYER_NAME_\d+\b", newline.join(kept))) != rows:
        raise unittest.SkipTest("the staging dialog does not author one IDC_PLAYER_NAME_n per row as expected")
    resource = pathlib.Path(mod) / "Data/Resources/GUIDialog/guidialogs.rc"
    resource.parent.mkdir(parents=True, exist_ok=True)
    # Bytes, not write_text: text mode would turn the script's own CRLF into CR CR LF on Windows.
    resource.write_bytes(script.replace(dialog[0], newline.join(kept)).encode("utf-8"))


@unittest.skipUnless(RUNTIME and GAME_ROOT,
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT")
class LanLobbyGraphical(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory_owner = tempfile.TemporaryDirectory(prefix="eawr-lan-lobby-")
        cls.directory = pathlib.Path(cls.directory_owner.name)
        cls.cache = pathlib.Path(tempfile.mkdtemp(prefix="eawr-font-cache-"))
        keep = os.environ.get("EAWR_LAN_LOBBY_CAPTURE_DIR")
        cls.pictures = pathlib.Path(keep) if keep else cls.directory
        cls.pictures.mkdir(parents=True, exist_ok=True)
        extracted = subprocess.run([sys.executable, str(TOOL), "--game-root", GAME_ROOT, "--out", str(cls.cache),
                                    "--allow-unknown-build", "--json"],
                                   capture_output=True, text=True, timeout=120, check=False)
        if extracted.returncode != 0:
            raise RuntimeError(extracted.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.cache, ignore_errors=True)
        cls.directory_owner.cleanup()  # The rig runner preserves TemporaryDirectory evidence.

    def test_dialog_rectangles_lobby_host_and_joiner(self):
        for resolution in ("1920x1080", "1280x720", "1600x1200"):
            with self.subTest(resolution=resolution):
                name = "dialog-rectangles-" + resolution
                # Windows may clamp the startup window to its usable desktop area.
                # Resize after launch so the viewport exercises the exact requested dimensions.
                steps = ("resize:" + resolution, "sleep:20", "source:online", "snapshot", "shot:" + name + "-online",
                         "source:lan", "snapshot", "host_options", "snapshot",
                         "shot:" + name + "-host-options", "cancel", "join:Geometry night",
                         "wait:staging@30", "snapshot", "shot:" + name + "-joiner")
                actions = [arg for step in steps for arg in ("--eawr-lan-do", step)]
                completed = subprocess.run(command(
                    self.cache, name, self.pictures, self.pictures / (name + ".png"),
                    "--eawr-lan-name", "Geometry player", "--eawr-lan-fake", "joined",
                    "--eawr-lan-game", "Geometry night|Synthetic map|TYPE_A|1/2|room-host:47624|Host",
                    *actions, resolution=resolution), cwd=ROOT, text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, check=False, timeout=600)
                self.assertEqual(completed.returncode, 0, completed.stdout[-5000:])
                report = load(self, self.pictures, name, completed.stdout)
                self.assertEqual(struct.unpack(">II", (self.pictures / (name + "-online.png")).read_bytes()[16:24]),
                                 tuple(int(v) for v in resolution.split("x")))
                self.assertEqual([shot["page"] for shot in report["snapshots"]],
                                 ["lobby", "lobby", "host_options", "staging"])
                for shot in report["snapshots"]:
                    with self.subTest(page=shot["page"], source=shot["source"]):
                        assert_dialog_rectangles(self, shot["rectangles"], [int(v) for v in resolution.split("x")])

    def test_online_staging_rectangles_and_live_resize(self):
        for host in (True, False):
            reference = None
            for resolution in ("1920x1080", "1280x720", "1600x1200"):
                with self.subTest(host=host, resolution=resolution):
                    name = "online-geometry-" + ("host-" if host else "joiner-") + resolution
                    steps = ["resize:" + resolution, "sleep:20", "source:online",
                             *(["host_options"] if host else []),
                             "host:Geometry night" if host else "join:Geometry night",
                             "wait:staging@30", "sleep:10", "snapshot", "shot:" + name]
                    if resolution == "1920x1080":
                        steps += ["resize:1280x720", "sleep:20", "snapshot", "shot:" + name + "-resized",
                                  "connections", "sleep:5", "snapshot", "shot:" + name + "-connections",
                                  "resize:1600x1200", "sleep:20", "snapshot", "shot:" + name + "-connections-resized"]
                    else:
                        steps += ["connections", "sleep:5", "snapshot", "shot:" + name + "-connections"]
                    actions = [arg for step in steps for arg in ("--eawr-lan-do", step)]
                    completed = subprocess.run(command(
                        self.cache, name, self.pictures, self.pictures / (name + "-final.png"),
                        "--eawr-lan-name", "Geometry player", "--eawr-lan-fake", "online-nine",
                        "--eawr-lan-game-port", "0", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                        "--eawr-online-room", "Geometry night|Host|Synthetic map|TEXT_GAME_TYPE_TEAM_SPACE|1/9",
                        *actions, resolution=resolution), cwd=ROOT, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, check=False, timeout=600)
                    self.assertEqual(completed.returncode, 0, completed.stdout[-5000:])
                    report = load(self, self.pictures, name, completed.stdout)
                    self.assertEqual(struct.unpack(">II", (self.pictures / (name + ".png")).read_bytes()[16:24]),
                                     tuple(int(v) for v in resolution.split("x")))
                    opened = report["snapshots"][0]
                    if reference is None:
                        reference = opened["rectangles"]
                    width, height = [int(v) for v in resolution.split("x")]
                    scale = min(width / 1920, height / 1080)
                    offset = [round((width - 1920 * scale) / 2), round((height - 1080 * scale) / 2)]
                    for first, second in zip(reference["controls"], opened["rectangles"]["controls"]):
                        self.assertEqual(first["id"], second["id"])
                        for axis, (value, actual) in enumerate(zip(first["rect"], second["rect"])):
                            self.assertAlmostEqual(value * scale + (offset[axis] if axis < 2 else 0), actual, delta=0.6)
                    for index, shot in enumerate(report["snapshots"]):
                        self.assertEqual((shot["page"], shot["host"], shot["source"]), ("staging", host, "online"))
                        viewport = [width, height]
                        if resolution == "1920x1080" and index:
                            viewport = [1600, 1200] if index == 3 else [1280, 720]
                        assert_dialog_rectangles(self, shot["rectangles"], viewport)
                        if shot["connections_open"]:
                            self.assertEqual(len(shot["connections"]), 9)
                        control_ids = {item["id"] for item in shot["rectangles"]["controls"]}
                        self.assertIn("EAWR_ONLINE_CONNECTIONS", control_ids)
                        if host and not shot["connections_open"]:
                            authored_ids = {"IDC_SELECT_OPTIONS", "IDC_LOAD_GAME", "IDC_SELECT_OFFICIAL_MAPS",
                                            "IDC_SELECT_CUSTOM_MAPS", "IDC_STAGING_MAP_LIST"}
                            self.assertTrue(authored_ids <= control_ids, control_ids)
                            for control in shot["rectangles"]["controls"]:
                                if control["id"] in authored_ids:
                                    for actual, authored in zip(control["rect"], control["authored"]):
                                        self.assertAlmostEqual(actual, authored, delta=0.5, msg=control["id"])
                    if resolution == "1920x1080":
                        before, after, connections, resized_connections = report["snapshots"]
                        self.assertTrue(connections["connections_open"])
                        self.assertTrue(resized_connections["connections_open"])
                        for first, second in zip(connections["rectangles"]["controls"], resized_connections["rectangles"]["controls"]):
                            self.assertEqual(first["id"], second["id"])
                            for axis, (value, actual) in enumerate(zip(first["rect"], second["rect"])):
                                self.assertAlmostEqual(value * 1.25 + (150 if axis == 1 else 0), actual, delta=0.6)
                        self.assertEqual(before["rows"], after["rows"], "resize preserves live staging state")
                        for first, second in zip(before["rectangles"]["controls"], after["rectangles"]["controls"]):
                            self.assertEqual(first["id"], second["id"])
                            for value, resized in zip(first["rect"], second["rect"]):
                                self.assertAlmostEqual(value * 2 / 3, resized, delta=0.6)

    def test_private_online_source_switch_keeps_lan_rows_and_chat_isolated(self):
        name = "private-online"
        arguments = ["--eawr-lan-name", "Solo", "--eawr-lan-fake", "joined",
                     "--eawr-lan-game", "Fleet night|Kessel|TYPE_A|1/2|10.0.0.5:47624|Rex",
                     "--eawr-lan-player", "Echo|3"]
        for step in ("wait:game:Fleet night@30", "chat:LAN only", "snapshot",
                     "source:online", "snapshot", "shot:private-online-empty",
                     "source:lan", "snapshot", "shot:private-online-return-lan", "join:Fleet night", "snapshot"):
            arguments += ["--eawr-lan-do", step]
        completed = subprocess.run(command(self.cache, name, self.directory, self.pictures / f"{name}.png",
                                           *arguments), capture_output=True, text=True, timeout=120, check=False)
        report = load(self, self.directory, name, completed)
        lan, online, returned, staging = report["snapshots"][:4]
        self.assertEqual(lan["players"], ["Solo", "Echo"])
        self.assertIn("Solo: LAN only", lan["chat"])
        self.assertEqual(online["games"], [])
        self.assertEqual(online["players"], ["Solo"])
        self.assertEqual(online["chat"], [])
        self.assertEqual(online["status"], "Set the online service in Network Options first.")
        self.assertEqual(returned["players"], ["Solo", "Echo"])
        self.assertEqual(returned["chat"], [])
        self.assertTrue(returned["games"])
        self.assertEqual(staging["page"], "staging")
        self.assertEqual(report["sent_chat"], ["lobby:Solo: LAN only"])

    def test_online_service_unconfigured_and_unreachable_fail_cleanly(self):
        for configured in (False, True):
            name = "online-service-live-" + str(configured)
            settings = self.directory / (name + ".cfg")
            if configured:
                with socket.socket() as closed:
                    closed.bind(("127.0.0.1", 0))
                    port = closed.getsockname()[1]
                settings.write_text("eawr-settings 1\nnetwork.online_host=127.0.0.1\n"
                                    f"network.online_port={port}\nnetwork.online_trust=pin\nnetwork.online_pin=" + "a" * 64 + "\n", encoding="utf-8")
            else:
                settings.write_text("eawr-settings 1\nnetwork.online_host=\nnetwork.online_unconfigured=1\n", encoding="utf-8")
            picture = self.pictures / (name + ".png")
            args = command(self.cache, name, self.directory, picture,
                           "--eawr-settings-file", str(settings), "--eawr-lan-do", "source:online",
                           "--eawr-lan-do", "sleep:360" if configured else "sleep:5",
                           "--eawr-lan-do", "snapshot")
            completed = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=300)
            self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
            result = load(self, self.directory, name, completed.stdout)
            self.assertEqual(result["final"]["source"], "online")
            notice = result["final"]["room_notice"]
            self.assertIn("service" if configured else "Network Options", notice)
            self.assertEqual(result["final"]["rooms_total"], 0)
            self.assertNotIn("127.0.0.1", json.dumps(result))
            self.assertNotIn("a" * 64, completed.stdout + completed.stderr + json.dumps(result))
            self.assertNotIn("ERROR:", completed.stdout + completed.stderr)

    def test_online_rooms_page_prompt_for_a_password_and_join_into_the_staging(self):
        name = "online-rooms"
        arguments = ["--eawr-lan-name", "Solo", "--eawr-lan-fake", "joined",
                     "--eawr-lan-game", "Fleet night|Kessel|TYPE_A|1/2|fleet-host:47624|Rex",
                     "--eawr-online-room", "Open night|Rex|Kessel|TEXT_GAME_TYPE_TEAM_SPACE|2/9",
                     "--eawr-online-room", "Locked night|Echo|Endor|TEXT_GAME_TYPE_TEAM_SPACE|3/9|password",
                     "--eawr-online-room", "Second locked|Fox|Corellia|TEXT_GAME_TYPE_TEAM_SPACE|1/2|password",
                     "--eawr-online-room", "Full night|Wedge|Hoth|TEXT_GAME_TYPE_TEAM_SPACE|9/9|password",
                     "--eawr-online-password", "Locked night=hunter2",
                     "--eawr-online-password", "Second locked=open sesame"]
        for index in range(1, 15):
            arguments += ["--eawr-online-room", f"Room {index:02d}|Host{index}|Yavin|TEXT_GAME_TYPE_TEAM_SPACE|1/4"]
        steps = ["source:online", "wait:rooms:18@30", "snapshot", "shot:online-rooms-page-1",
                 "rooms:next", "snapshot", "shot:online-rooms-page-2", "rooms:prev",
                 "sort:host", "snapshot", "sort:name",
                 "join:Locked night", "wait:prompt@30", "snapshot", "shot:online-rooms-prompt",
                 "password:nope", "wait:prompt_ready@30", "wait:prompt_error:The password was not accepted.@30",
                 "snapshot", "shot:online-rooms-wrong",
                 "password:nope", "wait:prompt_ready@30", "password:nope",
                 "wait:prompt_error:Too many password attempts@30", "snapshot", "shot:online-rooms-cooldown",
                 "prompt_cancel", "snapshot",
                 "chat:anyone there", "snapshot",
                 "refresh", "code:zz", "snapshot",
                 "cooldown:clear", "rooms:next", "join:Second locked", "wait:prompt@30", "password:open sesame", "wait:staging@30", "snapshot",
                 "shot:online-rooms-joined"]
        for step in steps:
            arguments += ["--eawr-lan-do", step]
        completed = subprocess.run(command(self.cache, name, self.directory, self.pictures / f"{name}.png",
                                           *arguments), capture_output=True, text=True, timeout=180, check=False)
        report = load(self, self.directory, name, completed)
        raw = (self.directory / f"{name}.json").read_text(encoding="utf-8")
        for secret in ("hunter2", "open sesame", "nope", "code:zz"):
            self.assertNotIn(secret, raw, "no typed password or code reaches the report")
        self.assertIn("code:*** ok", raw)
        first, second, by_host, prompt, wrong, cooldown, cancelled, no_chat, refreshed, staging = report["snapshots"][:10]
        self.assertEqual(first["source"], "online")
        self.assertEqual((first["room_page"], first["room_pages"], first["rooms_total"]), (0, 2, 18))
        self.assertEqual(len(first["games"]), 10)
        self.assertIn("Locked night", first["locked"])
        self.assertTrue(any("Locked night | TEXT_GAME_TYPE_TEAM_SPACE | 3/9 | Endor | ?" in game and "password" in game for game in first["games"]),
                        first["games"])
        self.assertEqual((second["room_page"], len(second["games"])), (1, 8))
        self.assertTrue(by_host["games"][0].startswith("Locked night | "), by_host["games"])
        self.assertTrue(prompt["prompt_open"])
        self.assertEqual(prompt["prompt_error"], "")
        self.assertEqual(wrong["prompt_error"], "The password was not accepted.")
        self.assertTrue(wrong["prompt_open"])
        self.assertIn("The password was not accepted.", wrong["chat"])
        self.assertIn("Too many password attempts", cooldown["prompt_error"])
        self.assertFalse(cancelled["prompt_open"])
        self.assertIn("Join cancelled.", cancelled["chat"])
        self.assertIn("Join or host a room to chat.", no_chat["chat"])
        self.assertFalse(no_chat["chat_open"])
        self.assertFalse(refreshed["prompt_open"])
        self.assertIn("Enter four letters or digits.", refreshed["chat"])
        self.assertEqual(report["online"]["refreshes"], 1)
        self.assertEqual(staging["page"], "staging")
        self.assertFalse(staging["host"])
        self.assertEqual(report["online"]["sent_chat"], [])

    def test_online_host_staging_colour_and_faction_dropdowns(self):
        name = "online-host-staging-dropdowns"
        arguments = ["--eawr-lan-name", "Solo", "--eawr-lan-fake", "connected",
                     "--eawr-online-room", "Open night|Rex|Kessel|TEXT_GAME_TYPE_TEAM_SPACE|2/9",
                     "--eawr-lan-game-port", "0", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                     "--eawr-lan-broadcast-port", str(free_udp_ports(1)[0])]
        for step in ("source:online", "host:Dropdown night", "wait:staging@30",
                     "wait:hosting:hosted@30", "snapshot", "staging_dropdown:FACTION:0",
                     "shot:online-host-faction-open", "staging_dropdown:COLOR:0",
                     "shot:online-host-colour-open"):
            arguments += ["--eawr-lan-do", step]
        completed = subprocess.run(command(self.cache, name, self.directory,
                                           self.pictures / f"{name}.png", *arguments),
                                   capture_output=True, text=True, timeout=180, check=False)
        self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
        report = load(self, self.directory, name, completed)
        staging = report["snapshots"][0]
        self.assertEqual(staging["page"], "staging")
        self.assertEqual(staging["source"], "online")
        self.assertTrue(staging["host"])
        self.assertEqual(staging["hosting"], "hosted")
        for field in ("faction", "colour"):
            self.assertTrue((self.pictures / f"online-host-{field}-open.png").is_file())

    def test_online_host_lists_a_public_or_a_password_room_and_shows_its_state(self):
        name = "online-host"
        arguments = ["--eawr-lan-name", "Solo", "--eawr-lan-fake", "connected",
                     "--eawr-lan-game", "Fleet night|Kessel|TYPE_A|1/2|fleet-host:47624|Rex",
                     "--eawr-online-room", "Open night|Rex|Kessel|TEXT_GAME_TYPE_TEAM_SPACE|2/9"]
        for step in ("source:online", "host:Private night|hunter2", "wait:staging@30", "wait:hosting:hosted@30", "snapshot",
                     "shot:online-host-private", "leave", "snapshot", "host:Public night", "wait:staging@30",
                     "wait:hosting:hosted@30", "snapshot"):
            arguments += ["--eawr-lan-do", step]
        completed = subprocess.run(command(self.cache, name, self.directory, self.pictures / f"{name}.png",
                                           *arguments), capture_output=True, text=True, timeout=180, check=False)
        report = load(self, self.directory, name, completed)
        raw = (self.directory / f"{name}.json").read_text(encoding="utf-8")
        self.assertNotIn("hunter2", raw, "the room password stays out of the report")
        private, lobby, public = report["snapshots"][:3]
        self.assertEqual(private["page"], "staging")
        self.assertTrue(private["host"])
        self.assertTrue(private["room_private"])
        self.assertEqual(private["hosting"], "hosted")
        self.assertIn("Your room is listed.", private["chat"])
        self.assertEqual(lobby["page"], "lobby")
        self.assertEqual(lobby["source"], "online")
        self.assertEqual(public["page"], "staging")
        self.assertFalse(public["room_private"])
        self.assertEqual(report["online"]["announced_game"], "Public night")
        self.assertFalse(report["online"]["announced_private"])
        self.assertFalse(report["online"]["host_password_set"])

    def test_chat_enter_keeps_focus_in_lan_online_and_room(self):
        name = "chat-enter-focus"
        steps = ("chat_enter:first LAN line", "sleep:2", "snapshot",
                 "chat_enter:second LAN line", "sleep:2", "snapshot",
                 "source:online", "online_menu:active", "sleep:2", "chat_enter:first online line", "sleep:2", "snapshot",
                 "chat_enter:second online line", "sleep:2", "snapshot",
                 "join:Open night", "wait:staging@30", "chat_enter:first room line", "sleep:2", "snapshot",
                 "chat_enter:second room line", "sleep:2", "snapshot")
        arguments = ["--eawr-lan-name", "Chat player", "--eawr-lan-fake", "joined",
                     "--eawr-online-room", "Open night|Rex|Kessel|TEXT_GAME_TYPE_TEAM_SPACE|2/9"]
        for step in steps:
            arguments += ["--eawr-lan-do", step]
        completed = subprocess.run(command(self.cache, name, self.directory, self.pictures / f"{name}.png",
                                           *arguments), capture_output=True, text=True, timeout=120, check=False)
        report = load(self, self.directory, name, completed)
        snapshots = report["snapshots"]
        self.assertEqual(len(snapshots), 6)
        for shot in snapshots:
            self.assertTrue(shot["chat_focused"], shot)
        self.assertEqual([shot["page"] for shot in snapshots], ["lobby"] * 4 + ["staging"] * 2)
        self.assertIn("Chat player: first LAN line", snapshots[1]["chat"])
        self.assertIn("Chat player: second LAN line", snapshots[1]["chat"])
        self.assertTrue(snapshots[3]["chat_open"])
        self.assertIn("Chat player: first online line", report["online"]["sent_chat"])
        self.assertIn("Chat player: second online line", report["online"]["sent_chat"])

    def test_online_list_failure_and_chat_window_use_the_same_screen(self):
        name = "online-unavailable"
        arguments = ["--eawr-lan-name", "Solo", "--eawr-lan-fake", "joined", "--eawr-online-list-failed", "true",
                     "--eawr-lan-game", "Fleet night|Kessel|TYPE_A|1/2|fleet-host:47624|Rex",
                     "--eawr-online-room", "Open night|Rex|Kessel|TEXT_GAME_TYPE_TEAM_SPACE|2/9"]
        for step in ("source:online", "online_chat:Rex|hello there", "wait:chat:hello there@30", "snapshot",
                     "shot:online-unavailable", "code:ab12", "wait:staging@30", "snapshot", "leave", "source:lan", "snapshot"):
            arguments += ["--eawr-lan-do", step]
        completed = subprocess.run(command(self.cache, name, self.directory, self.pictures / f"{name}.png",
                                           *arguments), capture_output=True, text=True, timeout=120, check=False)
        report = load(self, self.directory, name, completed)
        online, coded, lan = report["snapshots"][:3]
        self.assertEqual(coded["page"], "staging", "the room code field joins through the online room")
        self.assertEqual(online["games"], [])
        self.assertIn("unavailable", online["room_notice"])
        self.assertIn("Rex: hello there", online["chat"])
        self.assertEqual(lan["source"], "lan")
        self.assertEqual(lan["chat"], [], "no online chat leaks into the LAN window")

    def test_host_credits_dialog_accept_cancel_defaults(self):
        sys.path.insert(0, str(ROOT))
        from tools.inventory.corpus import Corpus
        source = Corpus(GAME_ROOT).read_effective("foc", "data/xml/gameconstants.xml").data
        for modded in (False, True):
            with self.subTest(modded=modded):
                constants = ET.fromstring(source)
                low = int(constants.find(".//Min_Skirmish_Credits").text)
                high = int(constants.find(".//Max_Skirmish_Credits").text)
                default = int(constants.find(".//MP_Default_Credits").text)
                extra = []
                name = "lobby-credit-mod" if modded else "lobby-credit-stock"
                if modded:
                    low, high = low + 250, high + 750
                    default = low + 501
                    constants.find(".//Min_Skirmish_Credits").text = str(low)
                    constants.find(".//Max_Skirmish_Credits").text = str(high)
                    constants.find(".//MP_Default_Credits").text = str(default)
                    data = self.directory / name / "Data"
                    (data / "XML").mkdir(parents=True)
                    (data / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
                    ET.ElementTree(constants).write(data / "XML/GameConstants.xml", encoding="utf-8")
                    extra = ["--eawr-mod-root", str(data.parent)]
                chosen = low + 1000
                steps = ("host_options", "host:Credit options", "snapshot", "options:open", "snapshot",
                         f"options:credits={low + 1}", "sleep:3", "snapshot", f"shot:{name}-edit",
                         "options:cancel", "snapshot", "options:open", f"options:credits={low + 1}",
                         "options:accept", "sleep:3", "snapshot", "options:open",
                         f"options:credits={high - 1}", "options:defaults", "snapshot",
                         "options:cancel", "snapshot", "options:open", "options:defaults", "options:accept",
                         "sleep:3", "snapshot", "leave")
                actions = [arg for step in steps for arg in ("--eawr-lan-do", step)]
                completed = subprocess.run(command(
                    self.cache, name, self.pictures, self.pictures / f"{name}.png",
                    "--eawr-lan-name", "Credits host", "--eawr-lan-game-port", "0",
                    "--eawr-lan-broadcast-port", str(free_udp_ports(1)[0]), *extra, *actions),
                    cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=600)
                self.assertEqual(completed.returncode, 0, completed.stdout[-6000:])
                before, opened, edited, cancelled, accepted, reset, reset_cancelled, reset_accepted = load(
                    self, self.pictures, name, completed.stdout)["snapshots"]
                self.assertEqual((opened["credits_min"], opened["credits_max"]), (low, high))
                self.assertTrue(opened["options_open"] and opened["credits_enabled"])
                for shown in (opened, reset):
                    self.assertEqual(shown["credits"], default)
                    self.assertEqual(shown["credits_caption"], str(default))
                    self.assertFalse(shown["credits_custom"])
                self.assertEqual(edited["credits"], chosen)
                self.assertEqual(edited["credits_caption"], str(chosen))
                self.assertTrue(edited["credits_custom"])
                self.assertEqual(cancelled["credits"], before["credits"])
                self.assertFalse(cancelled["options_open"] or cancelled["credits_custom"])
                for shown in (accepted, reset_cancelled):
                    self.assertEqual(shown["credits"], chosen)
                    self.assertTrue(shown["credits_custom"])
                    self.assertFalse(shown["options_open"])
                self.assertGreater(accepted["room_revision"], before["room_revision"])
                self.assertEqual(reset_accepted["credits"], default)
                self.assertFalse(reset_accepted["credits_custom"] or reset_accepted["options_open"])
                self.assertGreater(reset_accepted["room_revision"], accepted["room_revision"])

    def test_vacant_seat_controls_change_count_inside_lobby(self):
        name = "lobby-seat-controls"
        steps = ("host_options", "shot:host-options-without-count", "host:Seat controls",
                 "snapshot", "shot:seats-before",
                 "seat:1:hard", "snapshot", "shot:seats-hard-ai", "seat:1:closed", "snapshot",
                 "shot:seats-closed", "seat:1:open", "snapshot", "shot:seats-open",
                 "staging_dropdown:NAME:1", "shot:open-seat-menu", "leave")
        actions = [arg for step in steps for arg in ("--eawr-lan-do", step)]
        completed = subprocess.run(command(
            self.cache, name, self.pictures, self.pictures / f"{name}.png", "--eawr-lan-name", "Row host",
            "--eawr-lan-game-port", "0", "--eawr-lighting", "sh", "--eawr-shadows", "on",
            "--eawr-lan-broadcast-port", str(free_udp_ports(1)[0]), *actions),
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=600)
        self.assertEqual(completed.returncode, 0, completed.stdout[-5000:])
        before, ai, closed, opened = load(self, self.pictures, name, completed.stdout)["snapshots"]
        self.assertEqual(before["rows"][0]["kind"], "human")
        self.assertTrue(all(row["kind"] == "open" for row in before["rows"][1:] if row["visible"]))
        self.assertEqual(ai["rows"][1]["kind"], "ai")
        self.assertEqual(ai["rows"][1]["name"], "TEXT_HARD_AI_PLAYER")
        self.assertEqual(sum(row["kind"] in ("human", "ai") for row in ai["rows"]), 2)
        self.assertEqual(closed["rows"][1]["kind"], "none")
        self.assertEqual(opened["rows"][1]["kind"], "open")
        self.assertEqual(sum(row["kind"] in ("human", "ai") for row in opened["rows"]), 1)
        self.assertTrue(opened["rows"][1]["enabled"])
        self.assertEqual(opened["rows"][1]["faction"], "")
        self.assertEqual(opened["rows"][1]["team"], "")
        for capture in ("host-options-without-count", "seats-before", "open-seat-menu", "seats-hard-ai", "seats-closed", "seats-open"):
            self.assertTrue((self.pictures / f"{capture}.png").is_file())

    def test_nine_seat_online_staging_connections(self):
        name = "online-nine-staging"
        arguments = ["--eawr-lan-name", "Nine host", "--eawr-lan-fake", "online-nine",
                     "--eawr-online-room", "Nine night|Host|Kessel|TEXT_GAME_TYPE_TEAM_SPACE|1/9",
                     "--eawr-lighting", "sh", "--eawr-shadows", "on",
                     "--eawr-lan-broadcast-port", str(free_udp_ports(1)[0])]
        for step in ("source:online", "host:Nine night", "wait:staging@30", "wait:hosting:hosted@30",
                     "snapshot", "shot:online-nine-staging", "connections", "snapshot",
                     "shot:online-nine-connections", "connections", "snapshot", "leave"):
            arguments += ["--eawr-lan-do", step]
        completed = subprocess.run(command(self.cache, name, self.pictures, self.pictures / f"{name}.png", *arguments),
                                   cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   check=False, timeout=600)
        self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
        staging, opened, closed = load(self, self.pictures, name, completed.stdout)["snapshots"]
        self.assertEqual(staging["source"], "online")
        self.assertEqual(staging["page"], "staging")
        # Retain #4446's nine-row font and geometry regression coverage after
        # moving skirmish player count from the host form into staging rows.
        combos = {item["id"]: item for item in staging["rectangles"]["combos"]}
        for field in ("NAME", "FACTION", "COLOR", "TEAM"):
            template = combos[f"IDC_PLAYER_{field}_0"]
            for row in range(9):
                control = combos[f"IDC_PLAYER_{field}_{row}"]
                for key in ("kind", "font", "font_size", "font_color", "outline_size"):
                    self.assertEqual(control[key], template[key], (field, row, key))
                self.assertEqual(control["rect"][2:], template["rect"][2:], (field, row, "size"))
        self.assertFalse(staging["start"]["enabled"], "connecting/failed pairs keep Start unavailable")
        self.assertEqual(staging["start"]["reason"], "Waiting for all players to connect")
        self.assertTrue(staging["map"], "the shared map list remains available with nine humans")
        self.assertEqual(len(opened["connections"]), 9)
        self.assertNotIn("human seats)", str(staging["rows"]))
        self.assertTrue(opened["connections_open"])
        self.assertFalse(closed["connections_open"])
        self.assertIn("Connecting", opened["connections"][7])
        self.assertIn("relay was offered", opened["connections"][8])
        self.assertEqual(opened["connections"], closed["connections"])
        self.assertEqual(staging["chat"], closed["chat"], "opening connection details adds no repeated notices")
        for capture in ("online-nine-staging", "online-nine-connections"):
            self.assertTrue((self.pictures / f"{capture}.png").is_file())

        # The same GPU case also exercises the joiner's map/setting review at nine seats and the single refusal surface.
        for fixture, suffix in (("online-nine", "joiner"), ("online-refused", "refused")):
            joined_name = name + "-" + suffix
            joined_arguments = ["--eawr-lan-name", "Nine joiner", "--eawr-lan-fake", fixture,
                                "--eawr-online-room", "Nine night|Host|Kessel|TEXT_GAME_TYPE_TEAM_SPACE|1/9",
                                "--eawr-lighting", "sh", "--eawr-shadows", "on",
                                "--eawr-lan-broadcast-port", str(free_udp_ports(1)[0])]
            steps = ["source:online", "join:Nine night", "wait:staging@30", "snapshot", "shot:online-nine-" + suffix]
            if fixture == "online-nine":
                steps += ["connections", "snapshot", "connections", "snapshot", "shot:online-nine-joiner-restored", "leave"]
            for step in steps:
                joined_arguments += ["--eawr-lan-do", step]
            completed = subprocess.run(command(self.cache, joined_name, self.pictures,
                                               self.pictures / f"{joined_name}.png", *joined_arguments),
                                       cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       check=False, timeout=600)
            self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
            shots = load(self, self.pictures, joined_name, completed.stdout)["snapshots"]
            if fixture == "online-nine":
                before, details, restored = shots
                self.assertEqual(len(details["connections"]), 9)
                self.assertTrue(details["connections_open"])
                self.assertFalse(restored["connections_open"])
                for shot in (before, restored):
                    self.assertTrue(shot["drawn_map"])
                    self.assertTrue(shot["settings_read_only"])
                    self.assertTrue(shot["preview_visible"])
                self.assertEqual(before["settings"], restored["settings"])
            else:
                self.assertTrue(shots[0]["box_open"])
                self.assertFalse(any("Echo" in line or "EAWR-NET-1762" in line for line in shots[0]["chat"]),
                                 "the terminal refusal has one dialog, without a duplicate status/chat line")

    def test_two_and_four_humans_have_named_individual_rows(self):
        # Two humans exercise the typed-address path; four exercise discovery admission.
        for count in (2, 4):
            names = ["Alice", "Bob", "Cara", "Dan"][:count]
            game = f"Separate humans {count}"
            ports = free_udp_ports(count)
            with socket.socket() as available:
                available.bind(("127.0.0.1", 0))
                game_port = available.getsockname()[1]
            prefix = "four-human" if count == 4 else "two-human"
            scripts = [
                ["host:" + game,
                 *["wait:room_player:" + player + "@180" for player in names[1:]],
                 # Admissions invalidate edits. One host barrier starts a guest completion
                 # chain, keeping host chat below the four-message burst limit.
                 "chat:all players admitted",
                 *["wait:chat:faction picked " + player + "@180" for player in names[1:]],
                 "faction:Rebel", *[f"team:{row}:{row % 2 + 1}" for row in range(count)], "chat:teams set",
                 *["wait:chat:captured " + player + "@180" for player in names[1:]],
                 "snapshot", "shot:" + prefix + "-host-staging", "chat:captures complete", "sleep:60", "leave"]
            ]
            for previous, player in zip(names, names[1:]):
                join = f"direct:127.0.0.1:{game_port}" if count == 2 else "join:" + game
                permission = "all players admitted" if previous == names[0] else "faction picked " + previous
                scripts.append(["wait:game:" + game + "@180", join,
                                "wait:room_player:" + player + "@180", "wait:chat:" + permission + "@180",
                                "faction:Rebel", "chat:faction picked " + player,
                                "wait:chat:teams set@180", "snapshot", "shot:" + prefix + "-joiner-" + player,
                                "chat:captured " + player, "wait:chat:captures complete@180", "sleep:10", "leave"])
            processes, logs = [], []
            try:
                for index, (player, steps) in enumerate(zip(names, scripts)):
                    actions = [arg for step in steps for arg in ("--eawr-lan-do", step)]
                    report_name = f"separate-{count}-{player}"
                    argv = command(self.cache, report_name, self.pictures, self.pictures / f"{report_name}.png",
                                   "--eawr-lan-name", player, "--eawr-lan-colour", "0",
                                   "--eawr-lan-broadcast-port", str(ports[index]),
                                   *[arg for other in ports if other != ports[index]
                                     for arg in ("--eawr-lan-broadcast-extra", str(other))],
                                   "--eawr-lan-game-port", str(game_port if index == 0 and count == 2 else 0),
                                   "--eawr-lighting", "sh", "--eawr-shadows", "on", *actions)
                    log = (self.pictures / f"{report_name}.log").open("w", encoding="utf-8")
                    logs.append(log)
                    processes.append(subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, text=True))
                for process in processes:
                    process.wait(timeout=900)
            finally:
                for process in processes:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=30)
                for log in logs:
                    log.close()
            for player, process in zip(names, processes):
                report_name = f"separate-{count}-{player}"
                log = (self.pictures / f"{report_name}.log").read_text(encoding="utf-8")
                self.assertEqual(process.returncode, 0, log[-6000:])
                staging = load(self, self.pictures, report_name, log)["snapshots"][0]
                rows = staging["rows"][:count]
                self.assertCountEqual([row["name"] for row in rows], names)
                self.assertEqual([row["kind"] for row in rows], ["human"] * count)
                self.assertEqual([row["faction"] for row in rows], ["Rebel"] * count)
                self.assertEqual([row["team"] for row in rows], [str(row % 2 + 1) for row in range(count)])
                self.assertEqual(len({row["colour"] for row in rows}), count)
                assert_dialog_rectangles(self, staging["rectangles"])

    def test_a_host_alone_checks_its_start_and_shows_the_dialogs_rows(self):
        completed = subprocess.run(
            command(self.cache, "alone", self.pictures, self.pictures / "alone.png",
                    "--eawr-lan-name", "Solo", "--eawr-lan-colour", "1",
                    "--eawr-lan-broadcast-port", str(free_udp_ports(1)[0]), "--eawr-lan-game-port", "0",
                    "--eawr-lan-do", "snapshot", "--eawr-lan-do", "shot:alone-lobby",
                    "--eawr-lan-do", "host:Lonely night", "--eawr-lan-do", "map:data/art/maps/_mp_space_coruscant.ted",
                    "--eawr-lan-do", "ai:2:easy", "--eawr-lan-do", "snapshot",
                    "--eawr-lan-do", "ai:2:hard", "--eawr-lan-do", "snapshot",
                    "--eawr-lan-do", "ai:2:medium", "--eawr-lan-do", "snapshot",
                    "--eawr-lan-do", "shot:alone-staging", "--eawr-lan-do", "start", "--eawr-lan-do", "snapshot",
                    "--eawr-lan-do", "ai:8", "--eawr-lan-do", "snapshot",
                    "--eawr-lan-do", "chat:anyone there", "--eawr-lan-do", "snapshot",
                    "--eawr-lan-do", "cancel", "--eawr-lan-do", "cancel"),
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=600)
        # `start` fails with a red line: the run reports it as a failed step, by design, and exits 2
        # (a crash at shutdown would show as another code).
        self.assertEqual(completed.returncode, 2, completed.stdout[-3000:])
        report = json.loads((self.pictures / "alone.json").read_text(encoding="utf-8"))
        self.assertTrue(any(action.startswith("start") and action.endswith("FAILED") for action in report["actions"]),
                        report["actions"])
        lobby, easy, hard, staging, after_start, refused_edit, after_chat = report["snapshots"]
        for snapshot, label in ((easy, "TEXT_EASY_AI_PLAYER"), (hard, "TEXT_HARD_AI_PLAYER"),
                                (staging, "TEXT_MEDIUM_AI_PLAYER")):
            self.assertEqual(snapshot["rows"][2]["name"], label)
            self.assertTrue(snapshot["rows"][2]["enabled"])
            self.assertEqual(sum(row["kind"] == "ai" for row in snapshot["rows"]), 1)
        self.assertEqual(lobby["page"], "lobby")
        self.assertEqual(lobby["players"], ["Solo"])
        self.assertEqual(staging["page"], "staging")
        # The nine row gadgets of the retail dialog, whatever the map holds.
        self.assertEqual(staging["dialog_rows"], 9)
        self.assertEqual(staging["rows"][0]["name"], "Solo")
        self.assertEqual(staging["rows"][0]["kind"], "human")
        self.assertEqual(staging["rows"][1]["kind"], "open")
        self.assertFalse(staging["rows"][1]["accepted"], "a vacant row has no human to accept its choices")
        self.assertEqual(staging["rows"][2]["kind"], "ai")
        self.assertIn("TEXT_LAN_HOST_MIN_PLAYERS", after_start["chat"])
        self.assertFalse(after_start["start"]["enabled"])
        self.assertEqual(refused_edit["room_revision"], after_start["room_revision"])
        self.assertEqual(refused_edit["rows"][8]["kind"], "none")
        self.assertIn("EAWR-NET-1603", refused_edit["chat_last_shown"], "a refused host edit has a visible typed notice")
        self.assertIn("Solo: anyone there", after_chat["chat_last_shown"])
        # Cancel on the staging leaves it for the lobby, which the second Cancel closes: the run reports closed.
        self.assertTrue(report["closed"], report["actions"])
        self.assertNotIn("ERROR:", completed.stdout)

    def run_galactic_rows(self, name, resolution, *extra, settle=0):
        steps = ("resize:" + resolution, "sleep:10", "host_options", "host_dropdown:game_type=TEXT_GAME_TYPE_CAMPAIGN",
                 "host:Campaign rows", "wait:staging@60", "wait:room@60", *(("sleep:" + str(settle),) if settle else ("sleep:3",)),
                 "snapshot", "shot:" + name + "-staging", "cancel", "cancel")
        completed = subprocess.run(
            command(self.cache, name, self.pictures, self.pictures / (name + ".png"), "--eawr-lan-name", "Solo",
                    "--eawr-lan-game-port", "0", "--eawr-lighting", "sh", "--eawr-shadows", "on", *extra,
                    *[arg for step in steps for arg in ("--eawr-lan-do", step)], resolution=resolution),
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=600)
        self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
        self.assertNotIn("ERROR:", completed.stdout)
        return load(self, self.pictures, name, completed.stdout)["snapshots"][0]

    def test_galactic_room_shows_every_playable_faction(self):
        # D8 (#8071): a new campaign room plans the host's row only and shows every other playable faction as an AI faction.
        for resolution in ("1280x720", "2560x1080"):
            snapshot = self.run_galactic_rows("gc-rows-stock-" + resolution, resolution)
            shown = [row for row in snapshot["rows"] if row["visible"]]
            self.assertEqual(shown[0]["kind"], "human")
            self.assertTrue(shown[0]["name"].startswith("Solo"), shown[0]["name"])
            self.assertGreaterEqual(len(shown), 3, "stock multiplayer entries have three playable factions")
            self.assertTrue(all(row["kind"] == "ai" for row in shown[1:3]), [row["kind"] for row in shown])
            self.assertFalse(any(row["kind"] == "more" for row in snapshot["rows"]), "the stock factions fit the dialog")
            geometry = snapshot["rectangles"]
            controls = {item["id"]: item for item in geometry["controls"]}
            combos = {item["id"]: item for item in geometry["combos"]}
            for row, roster_row in enumerate(snapshot["rows"]):
                if not roster_row["visible"]:
                    continue
                name = controls[f"IDC_PLAYER_NAME_{row}"]
                faction = controls[f"IDC_PLAYER_FACTION_{row}"]
                colour = combos[f"IDC_PLAYER_COLOR_{row}"]
                extra = colour["rect"][2] / 2
                self.assertAlmostEqual(name["rect"][0], name["authored"][0], delta=0.5)
                self.assertAlmostEqual(name["rect"][2], name["authored"][2] + extra, delta=0.5)
                self.assertAlmostEqual(faction["rect"][0], faction["authored"][0] + extra, delta=0.5)
                self.assertAlmostEqual(faction["rect"][2], faction["authored"][2] + extra, delta=0.5)
                if row == 0:
                    team = controls[f"IDC_PLAYER_TEAM_{row}"]
                    for actual, authored in zip(team["rect"], team["authored"]):
                        self.assertAlmostEqual(actual, authored, delta=0.5)
            assert_dialog_rectangles(self, geometry)

    def test_galactic_waiting_banner(self):
        for resolution in ("1280x720", "2560x1080"):
            name = "gc-waiting-" + resolution
            steps = ("host_options", "host_dropdown:game_type=TEXT_GAME_TYPE_CAMPAIGN",
                     "host:Waiting room", "wait:staging@60", "wait:room@60", "sleep:30", "snapshot",
                     "shot:" + name + "-staging", "sleep:30", "snapshot", "cancel", "cancel")
            args = command(self.cache, name, self.pictures, self.pictures / (name + ".png"),
                           "--eawr-lan-fault", "waiting-banner",
                           "--eawr-lan-name", "Solo", "--eawr-lan-game-port", "0", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                           *[arg for step in steps for arg in ("--eawr-lan-do", step)], resolution=resolution)
            completed = subprocess.run(args, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600, check=False)
            self.assertEqual(completed.returncode, 0, completed.stdout[-5000:])
            snapshots = load(self, self.pictures, name, completed.stdout)["snapshots"]
            for snapshot in snapshots:
                self.assertEqual(snapshot["start"]["reason"], "Waiting for all players to connect")
                self.assertIn("Waiting for all players to connect", snapshot["pointer"]["header"])
            self.assertEqual(completed.stdout.count("Every pair must connect before Start; My connections shows only your own links."), 1)
            self.assertTrue((self.pictures / (name + "-staging.png")).is_file())

    def test_galactic_room_wide_campaign_names_the_rows_it_cannot_draw(self):
        # Eight player factions and four AI factions on the dialog's nine rows: the host's row, seven claimable rows, one line.
        mod = wide_campaign_mod(self.directory, 8, 4)
        for resolution in ("1280x720", "2560x1080"):
            snapshot = self.run_galactic_rows("gc-rows-wide-" + resolution, resolution, "--eawr-mod-root", str(mod))
            rows = snapshot["rows"]
            self.assertEqual(snapshot["dialog_rows"], 9)
            self.assertEqual(rows[0]["kind"], "human")
            self.assertEqual([row["kind"] for row in rows[1:8]], ["ai"] * 7, [row["kind"] for row in rows])
            self.assertEqual(rows[8]["kind"], "more")
            self.assertEqual(rows[8]["name"], "+4 AI factions")
            self.assertTrue(rows[8]["visible"])

    def test_galactic_room_opens_for_all_rows_and_two_humans_with_ai_rows_can_start(self):
        # #8130: the room opens for every playable row of the campaign (the stock one has three, a mod stack more), the guest claims a
        # row, every other faction stays an AI row, and Start unlocks once both present humans accepted: a room never waits for seats
        # nobody took.
        names = ["Solo", "Guest B"]
        tag = "gc-rows-default-two"
        with socket.socket() as available:
            available.bind(("127.0.0.1", 0))
            game_port = available.getsockname()[1]
        ports = free_udp_ports(len(names))
        host_script = ["host_options", "host_dropdown:game_type=TEXT_GAME_TYPE_CAMPAIGN", "sleep:5",
                       "host:Default rows", "wait:staging@60",
                       *["wait:room_player:" + player + "@180" for player in names[1:]], "ready", "sleep:20", "snapshot",
                       "shot:result-" + tag, "chat:default rows shots done", "sleep:20", "leave"]
        scripts = [host_script] + [["sleep:30", "direct:127.0.0.1:" + str(game_port), "wait:room_player:" + player + "@180",
                                    "ready", "wait:chat:default rows shots done@240", "leave"] for player in names[1:]]
        processes, logs = [], []
        try:
            for index, (player, steps) in enumerate(zip(names, scripts)):
                report_name = f"{tag}-{index}"
                actions = [arg for step in steps for arg in ("--eawr-lan-do", step)]
                argv = command(self.cache, report_name, self.pictures, self.pictures / f"{report_name}.png",
                               "--eawr-lan-name", player, "--eawr-lan-colour", "0",
                               "--eawr-lan-broadcast-port", str(ports[index]),
                               *[arg for other in ports if other != ports[index]
                                 for arg in ("--eawr-lan-broadcast-extra", str(other))],
                               "--eawr-lan-game-port", str(game_port if index == 0 else 0), *actions)
                log = (self.pictures / f"{report_name}.log").open("w", encoding="utf-8")
                logs.append(log)
                processes.append(subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, text=True))
            for process in processes:
                process.wait(timeout=900)
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=30)
            for log in logs:
                log.close()
        for index, process in enumerate(processes):
            log = (self.pictures / f"{tag}-{index}.log").read_text(encoding="utf-8")
            self.assertEqual(process.returncode, 0, log[-6000:])
        host_log = (self.pictures / f"{tag}-0.log").read_text(encoding="utf-8")
        report = load(self, self.pictures, f"{tag}-0", host_log)
        full = report["snapshots"][-1]
        humans = [row["name"] for row in full["rows"] if row["kind"] == "human" and row["visible"]]
        self.assertEqual(len(humans), 2, [(row["kind"], row["name"]) for row in full["rows"]])
        for player in names:
            self.assertTrue(any(name.startswith(player) for name in humans), (player, humans))
        self.assertGreaterEqual(sum(row["kind"] == "ai" for row in full["rows"]), 1, "the other factions are AI rows")
        self.assertTrue(full["start"]["enabled"], "two humans and AI rows: Start unlocks, never 'waiting for connections': "
                        + str(full["start"].get("reason")))
        self.assertTrue((self.pictures / f"result-{tag}.png").is_file())

    def test_galactic_room_seats_follow_the_campaign_while_only_the_host_is_in(self):
        # D8 (#8071), review fix: the room's seats follow the campaign the host picks (a room nobody else is in is opened again with the
        # new number); the seats of a room with guests are proven by the room contracts (net_galactic_lobby_contracts).
        mod = wide_campaign_mod(self.directory, 8, 4, small=3)
        name = "gc-rows-switch"
        # The wait is on the room's own seats: the room is opened again for each campaign, and its capacity is what changes.
        steps = ("host_options", "host_dropdown:game_type=TEXT_GAME_TYPE_CAMPAIGN", "host:Switch rows", "wait:capacity:8@60", "sleep:10",
                 "snapshot", "campaign:Small_Room_Test", "wait:capacity:3@60", "sleep:10", "snapshot",
                 "campaign:Wide_Room_Test", "wait:capacity:8@60", "sleep:10", "snapshot", "cancel", "cancel")
        completed = subprocess.run(
            command(self.cache, name, self.pictures, self.pictures / (name + ".png"), "--eawr-lan-name", "Solo",
                    "--eawr-lan-game-port", "0", "--eawr-mod-root", str(mod),
                    *[arg for step in steps for arg in ("--eawr-lan-do", step)]),
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=600)
        self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
        wide, small, wide_again = load(self, self.pictures, name, completed.stdout)["snapshots"]
        self.assertEqual(wide["room_capacity"], 8, "eight playable factions: a room for eight")
        self.assertEqual(small["room_capacity"], 3, "the three faction campaign: a room for three")
        self.assertEqual(wide_again["room_capacity"], 8)
        small_shown = [row["kind"] for row in small["rows"] if row["visible"]]
        self.assertEqual(small_shown[:3], ["human", "ai", "ai"], small_shown)
        self.assertEqual(sum(row["kind"] == "ai" for row in wide_again["rows"][:8]), 7)

    def test_galactic_room_with_a_four_row_dialog_keeps_its_last_row_for_the_hidden_rows_line(self):
        # D8 (#8071), review fix: a mod's dialog draws four player rows for a twelve faction campaign. The room keeps the fourth row
        # for the "+N AI factions" line, so a player is only ever seated on the three rows drawn above it (the seating itself is
        # proven by the room contracts: net_galactic_lobby_contracts, ui_lan_lobby_contracts). A room with a mod and several
        # guests does not form in this harness (also on main), so this case looks at the host's room alone.
        mod = wide_campaign_mod(self.directory, 8, 4)
        four_row_staging_dialog(mod, 4)
        snapshot = self.run_galactic_rows("gc-rows-four-row-dialog", "1280x720", "--eawr-mod-root", str(mod), settle=20)
        rows = snapshot["rows"]
        self.assertEqual(snapshot["dialog_rows"], 4)
        self.assertEqual([row["kind"] for row in rows], ["human", "ai", "ai", "more"], [row["kind"] for row in rows])
        self.assertTrue(rows[0]["name"].startswith("Solo"), rows[0]["name"])
        self.assertEqual(rows[3]["name"], "+9 AI factions", "the line has the fourth row to itself")
        self.assertTrue(rows[3]["visible"])
        self.assertTrue((self.pictures / "gc-rows-four-row-dialog-staging.png").is_file())

    def test_campaign_host_options_validate_cancel_and_restore_defaults(self):
        name = "campaign-host-options"
        steps = ("host_options", "host_dropdown:game_type=TEXT_GAME_TYPE_CAMPAIGN", "host:Campaign options", "wait:staging@60",
                 "snapshot", "options:open", "snapshot", "options:custom=on", "options:credits=6500",
                 "options:start_tech=2", "options:max_tech=4", "options:heroes=off",
                 "options:difficulty=Hard_Default", "options:auto_resolve=3", "options:cancel", "snapshot",
                 "options:open", "options:custom=on", "options:credits=6500", "options:start_tech=2",
                 "options:max_tech=1", "options:accept", "snapshot", "options:max_tech=4",
                 "options:credits=9223372036854775808", "options:accept", "snapshot",
                 "options:credits=6500", "options:heroes=off", "options:difficulty=Hard_Default",
                 "options:auto_resolve=3", "options:accept", "sleep:3", "snapshot",
                 "options:open", "options:defaults", "options:cancel", "snapshot",
                 "options:open", "options:defaults", "options:accept", "sleep:3", "snapshot",
                 "cancel", "cancel")
        completed = subprocess.run(
            command(self.cache, name, self.pictures, self.pictures / (name + ".png"),
                    "--eawr-lan-name", "Solo", "--eawr-lan-game-port", "0",
                    *[arg for step in steps for arg in ("--eawr-lan-do", step)]),
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=600)
        self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
        report = load(self, self.pictures, name, completed.stdout)
        initial, opened, cancelled, invalid_range, overflow, accepted, reset_cancelled, reset = report["snapshots"]
        self.assertNotIn("IDC_SELECT_OPTIONS", initial["hidden"])
        self.assertTrue(opened["options_open"])
        self.assertFalse(cancelled["options_open"] or cancelled["credits_custom"])
        self.assertTrue(invalid_range["options_open"])
        self.assertTrue(overflow["options_open"])
        self.assertTrue(invalid_range["options_error"])
        self.assertTrue(overflow["options_error"])
        self.assertEqual(initial["galactic_options"], cancelled["galactic_options"])
        self.assertEqual(initial["galactic_options"], invalid_range["galactic_options"])
        self.assertEqual(initial["galactic_options"], overflow["galactic_options"])
        for snapshot in (accepted, reset_cancelled):
            self.assertFalse(snapshot["options_open"])
            self.assertTrue(snapshot["credits_custom"])
            self.assertEqual(snapshot["credits"], 6500)
            self.assertEqual(snapshot["galactic_options"], {"custom": True, "credits": 6500,
                             "starting_tech": 2, "maximum_tech": 4, "difficulty": "Hard_Default",
                             "heroes": False, "auto_resolve": 3,
                             # owner decision D11: a room opens at 30 s whatever vote timeout the stock data authors (15)
                             "vote_timeout": 30})
        self.assertEqual(initial["galactic_options"], reset["galactic_options"])
        self.assertFalse(reset["options_open"] or reset["credits_custom"])
        # #8071: the host's one AI difficulty is what every AI row of the room reads (Hard after the accepted edit, Normal at
        # the defaults), by the level the campaign's entry stands for.
        for snapshot, caption in ((accepted, "TEXT_HARD_AI_PLAYER"), (reset_cancelled, "TEXT_HARD_AI_PLAYER"),
                                  (initial, "TEXT_MEDIUM_AI_PLAYER"), (reset, "TEXT_MEDIUM_AI_PLAYER")):
            ai_rows = [row for row in snapshot["rows"] if row["kind"] == "ai"]
            self.assertTrue(ai_rows, "the campaign room seats an AI faction")
            self.assertEqual({row["name"] for row in ai_rows}, {caption})
        # The words the room's difficulty shows as (the Difficulty line, the dropdown and the AI rows) on the stock text stack.
        self.assertEqual({snapshot["difficulty_shown"] for snapshot in (initial, reset)}, {"Medium AI"})
        self.assertEqual({snapshot["difficulty_shown"] for snapshot in (accepted, reset_cancelled)}, {"Hard AI"})
        self.assertNotIn("ERROR:", completed.stdout)

    def test_the_rooms_difficulty_shows_the_text_stacks_own_words_or_plain_level_names(self):
        # #8184: a stack that words the AI captions shows its own words for the agreed level ("AI on Spice"); a stack that words none
        # shows Easy / Normal / Hard. Never the text key and never a raw entry name (Normal_Default, Medium_Skirmish).
        captions = [("TEXT_EASY_AI_PLAYER", "AI on Kolto"), ("TEXT_MEDIUM_AI_PLAYER", "AI on Spice"),
                    ("TEXT_HARD_AI_PLAYER", "AI on Death Sticks")]
        for label, pairs, normal, hard in (
                ("with-strings", captions, "AI on Spice", "AI on Death Sticks"),
                ("without-strings", [("TEXT_UNRELATED_KEY", "Unrelated")], "Normal", "Hard")):
            with self.subTest(stack=label):
                mod = text_overlay_mod(self.directory, "text_" + label, pairs)
                name = "gc-difficulty-" + label
                steps = ("host_options", "host_dropdown:game_type=TEXT_GAME_TYPE_CAMPAIGN", "host:Difficulty words", "wait:staging@60",
                         "sleep:3", "snapshot", "options:open", "options:difficulty=Hard_Default", "options:accept", "sleep:3",
                         "snapshot", "shot:" + name, "cancel", "cancel")
                completed = subprocess.run(
                    command(self.cache, name, self.pictures, self.pictures / (name + ".png"), "--eawr-lan-name", "Solo",
                            "--eawr-lan-game-port", "0", "--eawr-mod-root", str(mod),
                            *[arg for step in steps for arg in ("--eawr-lan-do", step)]),
                    cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=600)
                self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
                initial, accepted = load(self, self.pictures, name, completed.stdout)["snapshots"]
                self.assertEqual(initial["galactic_options"]["difficulty"], "Normal_Default")
                self.assertEqual(initial["difficulty_shown"], normal)
                self.assertEqual(accepted["galactic_options"]["difficulty"], "Hard_Default")
                self.assertEqual(accepted["difficulty_shown"], hard)
                for snapshot in (initial, accepted):
                    self.assertNotRegex(snapshot["difficulty_shown"], r"_|TEXT_")
                self.assertNotIn("ERROR:", completed.stdout)

    def test_campaign_host_and_load_game_never_block_the_ui_thread(self):
        # #5512: Host (Campaign) and the staging Load Game press used to run the campaign content load on the UI thread,
        # and the owner's host ended with a lobby whose buttons did nothing. The script waits on frames, so a blocked
        # thread shows as one long gap between two frames and a frozen one ends in the subprocess deadline below.
        # Both render-thread models run.
        for render_thread in ("safe", "separate"):
            with self.subTest(render_thread=render_thread):
                name = "campaign-load-game-frames-" + render_thread
                saves = self.pictures / (name + "-saves")
                shutil.rmtree(saves, ignore_errors=True)
                saves.mkdir(parents=True)
                steps = ("host_options", "host_dropdown:game_type=TEXT_GAME_TYPE_CAMPAIGN", "host:Campaign load",
                         "wait:staging@60", "snapshot", "load_game", "wait:load_list@60", "snapshot", "sleep:30",
                         "load_cancel", "wait:load_list_closed@30", "snapshot", "load_game", "wait:load_list@60", "snapshot",
                         "load_cancel", "wait:load_list_closed@30", "snapshot")
                completed = subprocess.run(
                    command(self.cache, name, self.pictures, self.pictures / (name + ".png"),
                            "--eawr-lan-name", "Solo", "--eawr-lan-colour", "1", "--eawr-save-dir", str(saves),
                            "--eawr-lan-broadcast-port", str(free_udp_ports(1)[0]), "--eawr-lan-game-port", "0",
                            *[arg for step in steps for arg in ("--eawr-lan-do", step)],
                            engine=("--render-thread", render_thread)),
                    cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=240)
                self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
                report = load(self, self.pictures, name, completed.stdout)
                shots = report["snapshots"]
                self.assertEqual(len(shots), 5, report["actions"])
                before, opened, closed, reopened, closed_again = shots
                self.assertEqual(before["page"], "staging")
                # While the list is open it is a distinct canvas layer above the lobby (equal layers draw and hit-test in
                # an arbitrary order) and under the message boxes (100); once closed the lobby is back on its own layer
                # and Load Game works again, twice. A fresh host with no saves gets the empty list, not a dead lobby.
                self.assertFalse(before["load_game_disabled"])
                self.assertEqual(before["list_layer"], -1)
                for shot in (opened, reopened):
                    self.assertTrue(shot["list_visible"], shot)
                    self.assertGreater(shot["list_layer"], shot["lobby_layer"], shot)
                    self.assertLess(shot["list_layer"], 100, shot)
                for shot in (closed, closed_again):
                    self.assertEqual(shot["list_layer"], -1, shot)
                    self.assertEqual(shot["lobby_layer"], before["lobby_layer"], shot)
                    self.assertFalse(shot["load_game_disabled"], shot)
                    self.assertEqual(shot["list_note"], "", shot)
                self.assertLess(report["max_frame_gap_ms"], 1000, report["actions"])
                self.assert_no_engine_errors(completed.stdout)

    def assert_no_engine_errors(self, output):
        # The Separate render thread logs a harmless empty-image texture update while it runs and a render-thread finalize
        # complaint at exit (engine shutdown, after the viewer host is freed); neither is a lobby fault.
        benign = ("p_image.is_null() || p_image->is_empty()", "This function (finalize) can only be called from the render thread")
        errors = [line for line in output.splitlines() if "ERROR:" in line and not any(text in line for text in benign)]
        self.assertEqual(errors, [], output[-3000:])

    # #5512: the real-click cases. The scripted steps above call a button's handler; hover, click and click_row push
    # real mouse events through the viewport, so the GUI's hit test decides what is reached, as for the owner's click.
    HOST_CAMPAIGN = ("host_options", "host_dropdown:game_type=TEXT_GAME_TYPE_CAMPAIGN", "host:Campaign click", "wait:staging@60",
                     "wait:content_ready@120")
    LOBBY_BUTTONS = ("EAWR_GC_LOAD_GAME", "EAWR_GC_NEW_CAMPAIGN", "IDC_LAN_START_GAME_BUTTON", "IDC_LAN_CANCEL_BUTTON")

    def populated_saves(self, name):
        # Readable checkpoint headers (no campaign recorded, so the rows list and cannot load) under reserved and plain names.
        saves = self.pictures / (name + "-saves")
        shutil.rmtree(saves, ignore_errors=True)
        saves.mkdir(parents=True)
        for index, save in enumerate(("Auto Save", "Alpha", "Bravo", "Charlie")):
            (saves / f"{save}.eawrsave").write_bytes(checkpoint_fixture(30 * (index + 1)))
        (saves / "Damaged.eawrsave").write_bytes(b"not a checkpoint at all")
        return saves

    def run_clicks(self, name, steps, saves, render_thread, *extra):
        completed = subprocess.run(
            command(self.cache, name, self.pictures, self.pictures / (name + ".png"),
                    "--eawr-lan-name", "Solo", "--eawr-lan-colour", "1", "--eawr-save-dir", str(saves),
                    "--eawr-lan-broadcast-port", str(free_udp_ports(1)[0]), "--eawr-lan-game-port", "0", *extra,
                    *[arg for step in steps for arg in ("--eawr-lan-do", step)],
                    engine=("--render-thread", render_thread)),
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=300)
        self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
        report = load(self, self.pictures, name, completed.stdout)
        self.assert_no_engine_errors(completed.stdout)
        return report

    def assert_one_top_modal(self, shot):
        # Two input-stopping modal layers on the topmost canvas layer tie in an order nobody chose: one of them can sit
        # over the other with its dialog invisible while it eats every click. The top layer holds exactly one, and it
        # holds a visible dialog.
        modals = [modal for modal in shot["pointer"]["modals"] if modal["visible"]]
        top = max(modal["layer"] for modal in modals)
        on_top = [modal for modal in modals if modal["layer"] == top]
        self.assertEqual(len(on_top), 1, shot["pointer"]["modals"])
        self.assertGreater(on_top[0]["content"], 0, shot["pointer"]["modals"])

    def test_load_game_dialog_fits_the_window_at_every_size(self):
        # #5512 (B's rig run): the Load Game list was built for the window size but with the lobby's 1920x1080 theme and
        # without the lobby's letterbox transform, so at 1280x720 it ran off the right/bottom edge and its Cancel button
        # could not be clicked. The Cancel button's centre, in window pixels, must lie inside the window.
        for resolution in ("1280x720", "1920x1080"):
            with self.subTest(resolution=resolution):
                name = "load-game-fits-" + resolution
                saves = self.pictures / (name + "-saves")
                shutil.rmtree(saves, ignore_errors=True)
                saves.mkdir(parents=True)
                steps = ("resize:" + resolution, "sleep:10", "host_options",
                         "host_dropdown:game_type=TEXT_GAME_TYPE_CAMPAIGN", "host:Campaign fit", "wait:staging@60",
                         "load_game", "wait:load_list@60", "sleep:10", "snapshot")
                completed = subprocess.run(
                    command(self.cache, name, self.pictures, self.pictures / (name + ".png"),
                            "--eawr-lan-name", "Solo", "--eawr-lan-colour", "1", "--eawr-save-dir", str(saves),
                            "--eawr-lan-broadcast-port", str(free_udp_ports(1)[0]), "--eawr-lan-game-port", "0",
                            *[arg for step in steps for arg in ("--eawr-lan-do", step)], resolution=resolution),
                    cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=240)
                self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
                shot = load(self, self.pictures, name, completed.stdout)["snapshots"][-1]
                width, height = shot["window"]
                self.assertEqual([width, height], [int(v) for v in resolution.split("x")], shot)
                x, y = shot["list_cancel"]
                self.assertTrue(0 <= x <= width and 0 <= y <= height, f"Cancel centre {x},{y} outside {width}x{height}")

        # The list's own message box (here: the notice for a save folder that cannot be listed) takes the list's transform
        # (#5575 review: a child canvas layer does not inherit it) so it is not drawn off-screen at other sizes.
        for resolution in ("1280x720", "1024x768"):
            with self.subTest(resolution=resolution, box="notice"):
                name = "load-game-box-fits-" + resolution
                unreadable = self.pictures / (name + "-not-a-folder")
                unreadable.write_text("not a directory", encoding="utf-8")
                steps = ("resize:" + resolution, "sleep:10", "host_options",
                         "host_dropdown:game_type=TEXT_GAME_TYPE_CAMPAIGN", "host:Campaign box", "wait:staging@60",
                         "load_game", "wait:load_list@60", "sleep:10", "snapshot")
                completed = subprocess.run(
                    command(self.cache, name, self.pictures, self.pictures / (name + ".png"),
                            "--eawr-lan-name", "Solo", "--eawr-lan-colour", "1", "--eawr-save-dir", str(unreadable),
                            "--eawr-lan-broadcast-port", str(free_udp_ports(1)[0]), "--eawr-lan-game-port", "0",
                            *[arg for step in steps for arg in ("--eawr-lan-do", step)], resolution=resolution),
                    cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=240)
                self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
                shot = load(self, self.pictures, name, completed.stdout)["snapshots"][-1]
                self.assertGreater(shot["list_box_scale"], 0.0, "the unreadable folder opens a notice box over the list")
                self.assertAlmostEqual(shot["list_box_scale"], shot["list_scale"], places=4)

    def test_a_hung_saved_game_listing_is_abandoned_with_a_visible_note(self):
        # #5575: --eawr-lan-fault listing-hang blocks the listing worker for good; --eawr-lan-listing-timeout-ms shortens the
        # 30 s limit. The lobby must give up, say so in the header note, keep Start usable and let Load Game run again.
        name = "load-game-listing-hang"
        saves = self.pictures / (name + "-saves")
        shutil.rmtree(saves, ignore_errors=True)
        saves.mkdir(parents=True)
        steps = ("host_options", "host_dropdown:game_type=TEXT_GAME_TYPE_CAMPAIGN", "host:Campaign hang", "wait:staging@60",
                 "load_game", "wait:listing_pending@60", "snapshot", "wait:load_outcome@30", "snapshot", "load_game", "snapshot")
        completed = subprocess.run(
            command(self.cache, name, self.pictures, self.pictures / (name + ".png"),
                    "--eawr-lan-name", "Solo", "--eawr-lan-colour", "1", "--eawr-save-dir", str(saves),
                    "--eawr-lan-fault", "listing-hang", "--eawr-lan-listing-timeout-ms", "1500",
                    "--eawr-lan-broadcast-port", str(free_udp_ports(1)[0]), "--eawr-lan-game-port", "0",
                    *[arg for step in steps for arg in ("--eawr-lan-do", step)]),
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=240)
        self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
        pending, abandoned, retried = load(self, self.pictures, name, completed.stdout)["snapshots"]
        self.assertTrue(pending["load_game_disabled"], "the listing is pending: Load Game waits")
        self.assertEqual(abandoned["list_layer"], -1)
        self.assertIn("took too long", abandoned["list_note"])
        self.assertFalse(abandoned["load_game_disabled"], "Load Game is usable again after the abandon")
        self.assertTrue(retried["load_game_disabled"], "a second press starts a new listing")

    def test_a_slow_restore_is_a_busy_state_with_a_cancel_after_the_slow_mark(self):
        # #7858: while the host restores a saved campaign the lobby says it is restoring (with the elapsed time), never "Refused".
        # --eawr-lan-do restore_fixture:N starts a restore that has run N seconds and never ends. Past 5 minutes the line says it is
        # still waiting and the bottom button becomes Cancel restore; pressing it returns to a lobby that can apply the seats again.
        name = "restore-progress"
        saves = self.pictures / (name + "-saves")
        shutil.rmtree(saves, ignore_errors=True)
        saves.mkdir(parents=True)
        steps = (*self.HOST_CAMPAIGN, "restore_fixture:125", "sleep:40", "snapshot", "shot:" + name + "-restoring")
        report = self.run_clicks(name, steps, saves, "separate")
        restoring = report["snapshots"][-1]["pointer"]
        self.assertTrue(restoring["restoring"], restoring)
        self.assertRegex(restoring["summary_first"], r"^Restoring saved game\.\.\. please wait \(2:\d\d\)$")
        self.assertEqual(restoring["problem"], "", "a restore in progress is not a refusal")
        self.assertEqual(restoring["panel_button"], "Apply saved seats / Retry")
        self.assertTrue(restoring["panel_button_disabled"], "Apply waits while the restore runs")
        name = "restore-progress-slow"
        steps = (*self.HOST_CAMPAIGN, "restore_fixture:299", "sleep:90", "snapshot", "shot:" + name + "-slow",
                 "restore_cancel", "wait:restore_stopped@30", "sleep:2", "snapshot", "shot:" + name + "-cancelled")
        report = self.run_clicks(name, steps, saves, "separate")
        slow, cancelled = (shot["pointer"] for shot in report["snapshots"][-2:])
        self.assertRegex(slow["summary_first"], r"^Still waiting for the saved game to restore \(5:0\d so far\)")
        self.assertNotIn("Refused", slow["summary_first"])
        self.assertEqual(slow["panel_button"], "Cancel restore")
        self.assertFalse(slow["panel_button_disabled"])
        self.assertFalse(cancelled["restoring"], "Cancel restore ends the wait")
        self.assertEqual(cancelled["summary_first"], "Restore cancelled. Apply the saved seats to try again.")
        self.assertEqual(cancelled["panel_button"], "Apply saved seats / Retry")
        self.assertFalse(cancelled["panel_button_disabled"], "the lobby can apply the seats again without leaving")

    def test_a_refusal_for_old_battle_records_offers_the_recovery_button_and_says_what_was_dropped(self):
        # #7881 layout check without the player's campaign: recover_fixture:N is a host whose restore was refused because N finished-
        # battle records came from an older build. The panel then shows the refusal and a second button, Load without old battle
        # history; pressing it runs a canned conversion, the offer goes, and the chat says how many battles were dropped.
        name = "old-battle-history-fixture"
        saves = self.pictures / (name + "-saves")
        shutil.rmtree(saves, ignore_errors=True)
        saves.mkdir(parents=True)
        steps = (*self.HOST_CAMPAIGN, "recover_fixture:3", "sleep:5", "snapshot", "shot:" + name + "-offered",
                 "recover_history", "sleep:1", "snapshot", "shot:" + name + "-converting",
                 "wait:history_recovered@30", "snapshot", "shot:" + name + "-converted")
        report = self.run_clicks(name, steps, saves, "separate")
        offered, converting, converted = (shot["pointer"] for shot in report["snapshots"][-3:])
        self.assertTrue(offered["recoverable"], offered)
        self.assertIn("written by another build", offered["problem"])
        self.assertEqual(offered["recover_button"], "Load without old battle history")
        self.assertFalse(offered["recover_button_disabled"])
        self.assertEqual(offered["panel_button"], "Apply saved seats / Retry", "Apply stays the bottom button")
        self.assertTrue(converting["recovering"], converting)
        self.assertFalse(converting["recoverable"], "the offer goes while the conversion runs")
        self.assertTrue(converting["panel_button_disabled"], "Apply is off while the conversion runs: a click would drop it")
        self.assertFalse(converted["recovering"] or converted["recoverable"], converted)
        self.assertNotIn("another build", converted["problem"], "the old refusal is gone after the conversion")

    @unittest.skipUnless(os.environ.get("EAWR_JOB_OLD_BATTLE_SAVE"), "set EAWR_JOB_OLD_BATTLE_SAVE to a copy of a save written by an older build")
    def test_a_save_with_battle_records_of_an_older_build_offers_the_recovery(self):
        # #7881: the host's restore is refused because finished-battle records came from an older build. The panel then offers
        # Load without old battle history; pressing it converts the save off the UI thread, says in the chat what was dropped,
        # and applies the saved seats with the converted file. EAWR_JOB_OLD_BATTLE_SAVE is a COPY of such a save (never the
        # player's own folder); the case needs the campaign content the save was written with.
        source = pathlib.Path(os.environ["EAWR_JOB_OLD_BATTLE_SAVE"])
        name = "old-battle-history"
        saves = self.pictures / (name + "-saves")
        shutil.rmtree(saves, ignore_errors=True)
        saves.mkdir(parents=True)
        shutil.copyfile(source, saves / source.name)
        steps = (*self.HOST_CAMPAIGN, "load_game", "wait:load_list@60", "click_row:0", "wait:load_list_closed@30",
                 "sleep:120", "snapshot", "shot:" + name + "-refused",
                 "recover_history", "sleep:5", "snapshot", "shot:" + name + "-converting",
                 "sleep:240", "snapshot", "shot:" + name + "-recovered")
        report = self.run_clicks(name, steps, saves, "separate")
        refused, converting, recovered = (shot["pointer"] for shot in report["snapshots"][-3:])
        self.assertTrue(refused["recoverable"], refused)
        self.assertIn("another build", refused["problem"], refused)
        self.assertEqual(refused["recover_button"], "Load without old battle history")
        self.assertFalse(refused["recover_button_disabled"])
        self.assertEqual(refused["panel_button"], "Apply saved seats / Retry")
        self.assertTrue(converting["recovering"] or converting["restoring"], converting)
        self.assertFalse(converting["recoverable"], "the offer is gone while the conversion runs")
        self.assertFalse(recovered["recoverable"], recovered)
        self.assertNotIn("another build", recovered["problem"], recovered)

    def test_load_game_list_and_lobby_buttons_take_real_clicks(self):
        # The owner's host pressed Load Game and nothing answered again: the list's modal layer sat over the lobby and the
        # chat edit kept focus. (a) the list is up, its row and Cancel take a click, (b) after Cancel the lobby buttons
        # (Load Game, New Campaign, Start, Leave) are reached again, and Load Game opens the list a second time.
        for render_thread in ("safe", "separate"):
            with self.subTest(render_thread=render_thread):
                name = "campaign-real-clicks-" + render_thread
                saves = self.populated_saves(name)
                steps = (*self.HOST_CAMPAIGN, "sleep:10", "snapshot",
                         "click:EAWR_GC_LOAD_GAME", "wait:load_list@60", "sleep:10", "snapshot",
                         "hover:load:IDC_LOAD_CANCEL_BUTTON", "click_row:0", "sleep:5", "snapshot",
                         "click:load:IDC_LOAD_CANCEL_BUTTON", "wait:load_list_closed@30", "sleep:10", "snapshot",
                         *("hover:" + button for button in self.LOBBY_BUTTONS), "snapshot",
                         "click:EAWR_GC_LOAD_GAME", "wait:load_list@60", "sleep:10", "snapshot",
                         "click:load:IDC_LOAD_CANCEL_BUTTON", "wait:load_list_closed@30",
                         "click:IDC_LAN_CANCEL_BUTTON", "wait:page:lobby@30")
                report = self.run_clicks(name, steps, saves, render_thread)
                staging, opened, row, closed, lobby, reopened = report["snapshots"]
                for shot in report["snapshots"]:
                    self.assert_one_top_modal(shot)
                self.assertFalse(staging["pointer"]["load_list"]["open"])
                self.assertFalse(staging["pointer"]["load_game_disabled"], staging["pointer"])
                # (a) the list is on screen and the click reached it: the hover on Cancel ended on Cancel, the row click
                # selected row 0. A list that is up needs no visible error.
                self.assertTrue(opened["pointer"]["load_list"]["open"], opened["pointer"])
                self.assertTrue(opened["pointer"]["load_list"]["visible"], opened["pointer"])
                self.assertGreaterEqual(opened["pointer"]["load_list"]["rows"], 3, opened["pointer"])
                (cancel_hover,) = [entry for entry in row["pointer"]["log"] if entry["verb"] == "hover"]
                self.assertTrue(cancel_hover["found"] and cancel_hover["visible"] and cancel_hover["reaches"], cancel_hover)
                (row_click,) = [entry for entry in row["pointer"]["log"] if entry["verb"] == "click_row"]
                self.assertTrue(row_click["reaches"], row_click)
                self.assertEqual(row["pointer"]["load_list"]["selected"], 0, row["pointer"])
                # (b) Cancel closed the list, and the lobby's buttons are reached again.
                self.assertFalse(closed["pointer"]["load_list"]["open"], closed["pointer"])
                self.assertFalse(closed["pointer"]["load_game_disabled"], closed["pointer"])
                hovers = [entry for entry in lobby["pointer"]["log"] if entry["verb"] == "hover" and not entry["target"].startswith("load:")]
                self.assertEqual([entry["target"] for entry in hovers], list(self.LOBBY_BUTTONS), hovers)
                for entry in hovers:
                    self.assertTrue(entry["found"] and entry["visible"] and entry["reaches"], entry)
                self.assertTrue(reopened["pointer"]["load_list"]["open"], reopened["pointer"])
                self.assertGreaterEqual(reopened["pointer"]["load_list"]["rows"], 3, reopened["pointer"])

    def test_a_campaign_room_lists_readable_names_and_describes_the_selected_campaign(self):
        # #4553: the host's campaign list shows the Text_ID names, not the raw IDs, and the selected campaign's
        # Description_Text fills the panel where the map preview sits.
        name = "campaign-names"
        steps = ("host_options", "host_dropdown:game_type=TEXT_GAME_TYPE_CAMPAIGN", "host:Campaign night", "wait:staging@60", "snapshot",
                 "shot:" + name, "cancel", "cancel")
        completed = subprocess.run(
            command(self.cache, name, self.pictures, self.pictures / (name + ".png"),
                    "--eawr-lan-name", "Solo", "--eawr-lan-colour", "1",
                    "--eawr-lan-broadcast-port", str(free_udp_ports(1)[0]), "--eawr-lan-game-port", "0",
                    *[arg for step in steps for arg in ("--eawr-lan-do", step)]),
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=600)
        self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
        report = load(self, self.pictures, name, completed.stdout)
        (staging,) = report["snapshots"]
        self.assertEqual(staging["page"], "staging")
        self.assertTrue(staging["campaigns"], "the host lists the installed multiplayer campaigns")
        for title in staging["campaigns"]:
            self.assertFalse(title.upper().startswith(("MULTIPLAYER_", "TEXT_")), title)
        self.assertTrue(staging["campaign_description"].strip(), "the selected campaign has a description")
        self.assertFalse(staging["campaign_description"].startswith("TEXT_"), staging["campaign_description"])
        self.assertNotIn("ERROR:", completed.stdout)

    def test_the_chat_list_keeps_showing_new_lines_once_the_log_is_full(self):
        arguments = ["--eawr-lan-name", "Solo", "--eawr-lan-colour", "1", "--eawr-lan-game-port", "0"]
        for index in range(210):
            arguments += ["--eawr-lan-do", f"chat:line {index}"]
        arguments += ["--eawr-lan-do", "sleep:20", "--eawr-lan-do", "snapshot"]
        completed = subprocess.run(
            command(self.cache, "chatcap", self.pictures, self.pictures / "chatcap.png", *arguments),
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=900)
        self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
        report = load(self, self.pictures, "chatcap", completed.stdout)
        shot = report["snapshots"][0]
        self.assertEqual(len(shot["chat"]), 200)
        self.assertEqual(shot["chat_shown"], 200)
        self.assertTrue(shot["chat_last_shown"].endswith("line 209"), shot["chat_last_shown"])
        self.assertTrue(shot["chat"][0].endswith("line 10"), shot["chat"][0])

    def test_a_fixed_game_list_sorts_filters_and_a_join_opens_the_joiner_staging(self):
        games = ("Bravo|Kessel|TYPE_B|1/4|10.0.0.5:47624|Rex", "Alpha|Endor|TYPE_A|2/2|10.0.0.6:47624|Cody,Fives",
                 "Charlie|Corellia|TYPE_A|1/8|10.0.0.7:47624|Wolffe|password")
        arguments = ["--eawr-lan-name", "Solo", "--eawr-lan-colour", "1", "--eawr-lan-game-port", "0",
                     "--eawr-lan-fake", "joined", "--eawr-lan-player", "Echo|3"]
        for game in games:
            arguments += ["--eawr-lan-game", game]
        for step in ("wait:game:Alpha@30", "snapshot", "sort:map", "snapshot", "filter:type=TYPE_A", "snapshot",
                     "filter:clear", "shot:fixed-lobby", "join:Bravo", "snapshot", "chat:hello there"):
            arguments += ["--eawr-lan-do", step]
        completed = subprocess.run(
            command(self.cache, "fixed", self.pictures, self.pictures / "fixed.png", *arguments),
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=600)
        self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
        report = load(self, self.pictures, "fixed", completed.stdout)
        by_name, by_map, only_a, staging = report["snapshots"]

        def listed(snapshot):
            return [row.split(" | ")[0] for row in snapshot["games"]]

        self.assertEqual(by_name["page"], "lobby")
        self.assertEqual(listed(by_name), ["Alpha", "Bravo", "Charlie"])
        self.assertEqual(by_name["players"], ["Solo", "Echo"])
        self.assertTrue(by_name["games"][2].endswith(" | password"), by_name["games"])
        self.assertIn(" | 2/2 | ", by_name["games"][0])
        self.assertEqual(listed(by_map), ["Charlie", "Alpha", "Bravo"])
        self.assertEqual(listed(only_a), ["Charlie", "Alpha"])
        self.assertEqual(staging["page"], "staging")
        self.assertFalse(staging["host"])
        # What the lobby told its source: it is joining Bravo, and its chat line went to that game.
        self.assertEqual(report["announced"]["place"], "joining")
        self.assertEqual(report["announced"]["game"], "Bravo")
        self.assertEqual(report["sent_chat"], ["game:Solo: hello there"])
        # Map selection and editing remain host-only; the joiner has a read-only summary.
        self.assertEqual(sorted(staging["hidden"]),
                         ["IDC_SELECT_CUSTOM_MAPS", "IDC_SELECT_OFFICIAL_MAPS", "IDC_SELECT_OPTIONS"])

    def test_the_saved_profile_name_opens_the_lobby_and_an_edit_persists_across_a_restart(self):
        profile = self.pictures / "lan-profile.cfg"
        profile.write_text("eawr-profile 1\nplayer.name=Fallback\nplayer.lobby_colour=2\n"
                           "player.network_name=Z\u00f6e Bridge\n", encoding="utf-8")
        port = str(free_udp_ports(1)[0])

        def run(name, *steps):
            arguments = ["--eawr-profile-file", str(profile), "--eawr-lan-game-port", "0",
                         "--eawr-settings-file", str(self.pictures / "lan-profile-settings.cfg"),
                         "--eawr-lan-broadcast-port", port]
            for step in steps:
                arguments += ["--eawr-lan-do", step]
            completed = subprocess.run(
                command(self.cache, name, self.pictures, self.pictures / f"{name}.png", *arguments),
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=600)
            self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
            return load(self, self.pictures, name, completed.stdout)

        # The profile's network name and colour are the lobby's, with no command-line name.
        first = run("profile-first", "snapshot", "name:Rex Edited", "colour:3", "snapshot")
        opened, edited = first["snapshots"]
        self.assertEqual(opened["name"], "Zöe Bridge")
        self.assertEqual(opened["players"], ["Zöe Bridge"])
        self.assertEqual(opened["colour"], 2)
        self.assertEqual(edited["name"], "Rex Edited")
        self.assertEqual(first["profile_note"], "")
        saved = profile.read_text(encoding="utf-8")
        self.assertIn("player.network_name=Rex Edited", saved)
        self.assertIn("player.lobby_colour=3", saved)
        # The rest of the record is kept, not rebuilt from the screen.
        self.assertIn("player.name=Fallback", saved)

        # A restart starts from the edit.
        second = run("profile-second", "snapshot")
        restarted = second["snapshots"][0]
        self.assertEqual(restarted["name"], "Rex Edited")
        self.assertEqual(restarted["colour"], 3)

    def test_a_colour_only_edit_leaves_the_network_name_unset(self):
        profile = self.pictures / "lan-colour-profile.cfg"
        profile.write_text("eawr-profile 1\nplayer.name=Fallback\n", encoding="utf-8")
        arguments = ["--eawr-profile-file", str(profile), "--eawr-lan-game-port", "0",
                     "--eawr-lan-broadcast-port", str(free_udp_ports(1)[0])]
        for step in ("snapshot", "colour:3", "snapshot"):
            arguments += ["--eawr-lan-do", step]
        completed = subprocess.run(
            command(self.cache, "profile-colour", self.pictures, self.pictures / "profile-colour.png", *arguments),
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=600)
        self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
        report = load(self, self.pictures, "profile-colour", completed.stdout)
        opened, edited = report["snapshots"]
        # The lobby opens with the player name, a colour edit changes only the colour.
        self.assertEqual(opened["name"], "Fallback")
        self.assertEqual(edited["colour"], 3)
        saved = profile.read_text(encoding="utf-8")
        self.assertIn("player.lobby_colour=3", saved)
        # An unset network name is saved empty (it follows the player name), never the platform name.
        self.assertRegex(saved, r"(?m)^player\.network_name=\s*$")
        self.assertIn("player.name=Fallback", saved)

    def test_a_refused_connection_shows_its_box_and_the_screen_closes_cleanly(self):
        arguments = ["--eawr-lan-name", "Solo", "--eawr-lan-colour", "1", "--eawr-lan-game-port", "0",
                     "--eawr-lan-fake", "refused", "--eawr-lan-game", "Bravo|Kessel|TYPE_B|1/4|10.0.0.5:47624|Rex"]
        for step in ("wait:game:Bravo@30", "join:Bravo", "wait:box@30", "snapshot", "shot:refused-box"):
            arguments += ["--eawr-lan-do", step]
        completed = subprocess.run(
            command(self.cache, "refused", self.pictures, self.pictures / "refused.png", *arguments),
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=600)
        # The box is still up at exit: freeing the screen with its box open must not crash.
        self.assertEqual(completed.returncode, 0, completed.stdout[-3000:])
        report = load(self, self.pictures, "refused", completed.stdout)
        self.assertTrue(report["snapshots"][0]["box_open"])
        self.assertNotIn("ERROR:", completed.stdout)

    def test_joiner_reviews_map_and_options_before_accept(self):
        host_port, join_port = free_udp_ports(2)
        scripts = {
            "host": ["host:Review night", "wait:room_player:Reviewer@120", "wait:chat:moved to seven@120",
                     "snapshot", "map:data/art/maps/_mp_space_hypori.ted", "snapshot", "shot:sparse-map-refusal",
                     "chat:map checked", "wait:chat:reviewed@120"],
            "join": ["wait:game:Review night@120", "join:Review night", "wait:room_player:Reviewer@120",
                     "slot:7", "sleep:20", "chat:moved to seven", "wait:chat:map checked@120",
                     "snapshot", "shot:joiner-map-options", "chat:reviewed"],
        }
        processes, logs = [], []
        try:
            for side, local_port, remote_port in (("host", host_port, join_port), ("join", join_port, host_port)):
                args = ["--eawr-lan-name", "Reviewer" if side == "join" else "Host",
                        "--eawr-lan-broadcast-port", str(local_port), "--eawr-lan-broadcast-extra", str(remote_port),
                        "--eawr-lan-game-port", "0", "--eawr-lighting", "sh", "--eawr-shadows", "on"]
                for step in scripts[side]:
                    args += ["--eawr-lan-do", step]
                log = (self.pictures / f"review-{side}.log").open("w", encoding="utf-8")
                logs.append(log)
                processes.append(subprocess.Popen(command(self.cache, f"review-{side}", self.pictures,
                                                          self.pictures / f"review-{side}.png", *args),
                                                  cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, text=True))
            for process in processes:
                process.wait(timeout=300)
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=30)
            for log in logs:
                log.close()
        reports = {}
        for side, process in zip(scripts, processes):
            output = (self.pictures / f"review-{side}.log").read_text(encoding="utf-8")
            self.assertEqual(process.returncode, 2 if side == "host" else 0, output[-6000:])
            report = load(self, self.pictures, f"review-{side}", output,
                          ("map:data/art/maps/_mp_space_hypori.ted",) if side == "host" else ())
            reports[side] = report["snapshots"][0]
            if side == "host":
                before, after = report["snapshots"]
                self.assertIn("data/art/maps/_mp_space_hypori.ted", before["disabled_maps"])
                self.assertEqual(before["map"], after["map"])
                self.assertEqual(before["rows"], after["rows"], "a refused four-row map cannot renumber rows one and seven")
                self.assertEqual([i + 1 for i, row in enumerate(after["rows"]) if row["kind"] == "human"], [1, 7])
        host, join = reports["host"], reports["join"]
        self.assertTrue(join["map"], "selected logical map id arrives before Accept")
        self.assertEqual(join["drawn_map"], host["map"], "draw the locally resolved name, not the logical path")
        self.assertTrue(join["preview_visible"], "the joiner actually has a loaded preview texture")
        self.assertTrue(join["settings_read_only"])
        self.assertEqual(len(join["settings"]), 6, join["settings"])
        self.assertTrue(all(": " in line for line in join["settings"]), join["settings"])
        # #4542: the row the guest left (row 2) is Open again at its full authored width, not the narrow
        # combo the guest's own row used; the guest's new row 7 is human and readable.
        vacated, moved = join["rows"][1], join["rows"][6]
        self.assertEqual(vacated["kind"], "open")
        self.assertGreater(vacated["name_authored_width"], 0)
        self.assertAlmostEqual(vacated["name_width"], vacated["name_authored_width"], delta=1.0,
                               msg="the vacated row keeps its full Open caption width")
        self.assertEqual(moved["kind"], "human")
        self.assertGreater(moved["name_authored_width"], 0)
        self.assertLess(moved["name_width"], moved["name_authored_width"] - 1.0,
                        "the guest's own row keeps the narrow seat combo")
        self.assertFalse(join["local_ready"], "no Accept was needed to populate the screen")
        self.assertFalse(host["start"]["enabled"])
        self.assertNotIn("IDC_IMAGE", join["hidden"])
        self.assertNotIn("IDC_STAGING_MAP_LIST", join["hidden"])
        self.assertTrue((self.pictures / "joiner-map-options.png").is_file())

    def test_a_gc_slot_with_several_humans_lists_one_player_per_line(self):
        # #8020: two and three humans share the host's slot of a co-op campaign room. Every human has its own cell (name and
        # mods state) below the previous one, nothing is cut at 1080p or on an ultrawide window, the host keeps one Kick per
        # other human, and the same room before the guests join (they wait 30 s) is where the dialog authors it.
        for count, resolution in ((2, "1920x1080"), (3, "1920x1080"), (2, "3440x1440"), (3, "3440x1440")):
            names = ["Fordo", "Fordo #2", "Fordo #3"][:count]
            tag = f"gc-stack-{count}-{resolution}"
            with socket.socket() as available:
                available.bind(("127.0.0.1", 0))
                game_port = available.getsockname()[1]
            ports = free_udp_ports(count)
            host_script = ["host_options", "host_dropdown:game_type=TEXT_GAME_TYPE_CAMPAIGN", f"humans:{count}",
                           "host:Stacked slot", "wait:staging@60", "click:EAWR_GC_TAB_COOP", "campaign:coop", "wait:capacity:" + str(count) + "@60",
                           "sleep:10", "snapshot",
                           *["wait:room_player:" + player + "@180" for player in names[1:]], "sleep:40", "snapshot",
                           "shot:result-" + tag, "chat:stack shots done", "sleep:20", "leave"]
            # The host picks the first co-op campaign (one side): every guest is seated on the host's row by the room, so no guest moves
            # (a multiplayer room would have each guest claim a row of its own, D8).
            scripts = [host_script] + [["sleep:30", "direct:127.0.0.1:" + str(game_port),
                                        "wait:room_player:" + player + "@180", "sleep:30", "snapshot",
                                        "wait:chat:stack shots done@300", "leave"] for player in names[1:]]
            processes, logs = [], []
            try:
                for index, (player, steps) in enumerate(zip(names, scripts)):
                    report_name = f"{tag}-{index}"
                    actions = [arg for step in steps for arg in ("--eawr-lan-do", step)]
                    argv = command(self.cache, report_name, self.pictures, self.pictures / f"{report_name}.png",
                                   "--eawr-lan-name", player, "--eawr-lan-colour", "0",
                                   "--eawr-lan-broadcast-port", str(ports[index]),
                                   *[arg for other in ports if other != ports[index]
                                     for arg in ("--eawr-lan-broadcast-extra", str(other))],
                                   "--eawr-lan-game-port", str(game_port if index == 0 else 0),
                                   *actions, resolution=resolution)
                    log = (self.pictures / f"{report_name}.log").open("w", encoding="utf-8")
                    logs.append(log)
                    processes.append(subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, text=True))
                for process in processes:
                    process.wait(timeout=900)
            finally:
                for process in processes:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=30)
                for log in logs:
                    log.close()
            for index, process in enumerate(processes):
                log = (self.pictures / f"{tag}-{index}.log").read_text(encoding="utf-8")
                self.assertEqual(process.returncode, 0, log[-6000:])
            host_log = (self.pictures / f"{tag}-0.log").read_text(encoding="utf-8")
            alone, full = load(self, self.pictures, f"{tag}-0", host_log)["snapshots"]
            shared = next(row for row in full["rows"] if row["cells"] > 1)
            # One cell and one line per human, each name once and whole (the room seats guests in the order they connect, which no
            # test orders); the stack ends on the mods state of each.
            self.assertEqual(shared["cells"], count)
            self.assertEqual(len(shared["lines"]), count)
            self.assertCountEqual([line.rsplit(" - ", 1)[0] for line in shared["lines"]], names)
            # Each cell shows its own player's whole name and a mods state that is not cut (a shorter wording when the full one is wider).
            self.assertEqual(len(shared["cell_texts"]), count)
            self.assertCountEqual([cell.rsplit(" - ", 1)[0] for cell in shared["cell_texts"]], names)
            for cell in shared["cell_texts"]:
                self.assertNotIn("\u2026", cell, shared["cell_texts"])
                self.assertRegex(cell, r" - (mods ready|ready|ok)$")
            self.assertEqual(shared["line_fits"], [True] * count, "a name is cut at " + resolution)
            # The host's list: every human on its own line, then a Kick per other human and no more.
            self.assertEqual(shared["items"][:count], shared["lines"])
            self.assertCountEqual(shared["items"][count:], ["Kick " + player for player in names[1:]], shared["items"])
            # A single-human slot of the same room is unchanged: one cell, the dialog's own place.
            before = {index: row for index, row in enumerate(alone["rows"]) if row["visible"]}
            after = {index: row for index, row in enumerate(full["rows"]) if row["visible"]}
            held = next(index for index, row in after.items() if row["cells"] > 1)  # the row the humans share, wherever the campaign draws it
            self.assertEqual(shared["top"], before[held]["top"])
            self.assertTrue(all(row["cells"] == 1 and not row["lines"] for row in alone["rows"]))
            ordered = sorted(before)
            pitch = before[ordered[1]]["top"] - before[ordered[0]]["top"] if len(ordered) > 1 else 0
            for index in after:
                if index == held or after[index]["cells"] != 1 or index not in before:
                    continue
                # Rows above the shared one stay, rows below move down by the lines the stack adds.
                self.assertAlmostEqual(after[index]["top"] - before[index]["top"], (count - 1) * pitch if index > held else 0, delta=1.0)
            self.assertTrue((self.pictures / f"result-{tag}.png").is_file())

    def test_two_viewers_host_find_join_and_both_stagings_show_both_players(self):
        host_port, join_port = free_udp_ports(2)
        host_name, join_name, game = "Alice", "Bob", "Fleet night"
        common_host = ["--eawr-lan-broadcast-port", str(host_port), "--eawr-lan-broadcast-extra", str(join_port),
                       "--eawr-lan-game-port", "0"]
        common_join = ["--eawr-lan-broadcast-port", str(join_port), "--eawr-lan-broadcast-extra", str(host_port)]
        host_script = ["name:" + host_name, "colour:2", "host:" + game, "ai:2:hard", "wait:room_player:" + join_name + "@120", "snapshot",
                       "shot:two-host-staging", "ready", "wait:ready:all@180", "snapshot", "shot:two-host-ready",
                       "chat:ready from host", "wait:chat:ready from joiner@120", "name:Alice Prime", "sleep:10",
                       "snapshot", "chat:name-only", "wait:notall@120", "snapshot", "shot:two-host-reset",
                       "chat:choices reset", "ready", "wait:ready:all@180", "snapshot", "shot:two-host-reready",
                       "kick:1", "sleep:30", "snapshot"]
        join_script = ["name:" + join_name, "colour:4", "wait:game:" + game + "@120", "snapshot", "shot:two-join-lobby",
                       "join:" + game, "wait:room_player:" + join_name + "@120", "snapshot", "shot:two-join-staging",
                       "accept", "wait:ready:all@180", "wait:chat:ready from host@120", "chat:ready from joiner",
                       "wait:chat:name-only@120", "colour:5", "wait:notall@120", "snapshot", "shot:two-join-reset",
                       "wait:chat:choices reset@120", "ready", "wait:ready:all@180", "snapshot", "shot:two-join-ready",
                       "wait:box@120", "snapshot", "shot:two-join-kicked"]
        arguments = []
        for side, script, extra in (("host", host_script, common_host), ("join", join_script, common_join)):
            line = []
            for step in script:
                line += ["--eawr-lan-do", step]
            arguments.append((side, command(self.cache, "two-" + side, self.pictures,
                                            self.pictures / f"two-{side}.png", *extra, *line)))
        processes = []
        logs = []
        try:
            for side, argv in arguments:
                log = (self.pictures / f"two-{side}.log").open("w", encoding="utf-8")
                logs.append(log)
                processes.append(subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, text=True))
            for process in processes:
                process.wait(timeout=900)
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=30)
            for log in logs:
                log.close()
        reports = {}
        for (side, _), process in zip(arguments, processes):
            log = (self.pictures / f"two-{side}.log").read_text(encoding="utf-8")
            self.assertEqual(process.returncode, 0, log[-6000:])
            reports[side] = load(self, self.pictures, f"two-{side}", log)
        host, join = reports["host"], reports["join"]

        lobby = join["snapshots"][0]
        self.assertEqual(lobby["page"], "lobby")
        self.assertEqual(len(lobby["games"]), 1, lobby["games"])
        self.assertTrue(lobby["games"][0].startswith(game + " | "), lobby["games"])
        self.assertIn(host_name, lobby["players"] + [host_name])

        host_staging = host["snapshots"][0]
        join_staging = join["snapshots"][1]
        capacity = sum(row["visible"] and row["kind"] in ("human", "open") for row in host_staging["rows"])
        self.assertRegex(lobby["games"][0], r" \| 1/\d+ \| ", "discovery sees the host before the guest joins")
        # The first broadcast can precede the host's AI edit. Check the updated
        # capacity after the exchange, once that edit has propagated over UDP.
        self.assertIn(f" | 1/{capacity} | ", join["snapshots"][-1]["games"][0],
                      "discovery updates to humans plus Open rows after the host's AI edit")
        for staging in (host_staging, join_staging):
            self.assertEqual(staging["page"], "staging")
            names = [row["name"] for row in staging["rows"][:2]]
            self.assertEqual(names, [host_name, join_name], staging["rows"][:2])
            self.assertTrue(all(row["kind"] == "human" for row in staging["rows"][:2]))
        self.assertTrue(host_staging["host"] and not join_staging["host"])
        for staging in (host_staging, join_staging):
            self.assertEqual(staging["rows"][2]["kind"], "ai")
            self.assertEqual(staging["rows"][2]["name"], "TEXT_HARD_AI_PLAYER")
        self.assertTrue(host_staging["rows"][2]["enabled"])
        self.assertFalse(join_staging["rows"][2]["enabled"], "the guest's AI row is read-only")
        self.assertLessEqual(host["final"]["chat"].count(host_staging["status"]), 4,
                             "preparation status changes do not flood the bounded chat every frame")
        self.assertFalse(host_staging["start"]["enabled"], "connection alone never enables Start")
        self.assertFalse(any(row["accepted"] for row in host_staging["rows"][:2]))
        ready, renamed, reset, reready, kicked = host["snapshots"][1:]
        self.assertTrue(ready["start"]["enabled"])
        self.assertEqual(ready["ready_seats"], 2)
        self.assertEqual(renamed["rows"][0]["name"], "Alice Prime")
        self.assertEqual(renamed["ready_seats"], 2, "a name-only edit keeps readiness")
        self.assertFalse(reset["start"]["enabled"])
        self.assertEqual(reset["ready_seats"], 1, "only the joiner's own colour change unreadies the joiner; the host stays ready")
        self.assertEqual(reset["rows"][1]["colour"], join["snapshots"][2]["rows"][1]["colour"])
        self.assertTrue(reready["start"]["enabled"])
        self.assertEqual(kicked["ready_seats"], 1, "kicking the joiner keeps the host ready")
        self.assertTrue(join["snapshots"][-1]["box_open"])
        # Each viewer received the other's staging chat line, once.
        self.assertTrue(any(line.endswith("ready from joiner") for line in host["final"]["chat"]))
        self.assertTrue(any(line.endswith("ready from host") for line in join["final"]["chat"]))
        # The chat sound names resolve against the effective sound data, not a fixed spelling.
        self.assertTrue(any("chat_re" in sound.lower() for sound in join["sounds"]), join["sounds"])



@unittest.skipUnless(RUNTIME and GAME_ROOT, 'requires the viewer GPU lane and installed game data')
class SocketLifecycleProbe(unittest.TestCase):
    def setUp(self):
        self.directory_owner = tempfile.TemporaryDirectory(prefix='eawr-lan-lobby-')
        self.directory = pathlib.Path(self.directory_owner.name)
        self.cache = pathlib.Path(tempfile.mkdtemp(prefix='eawr-font-cache-'))
        self.pictures = self.directory
        self.addCleanup(self.directory_owner.cleanup)
        self.addCleanup(shutil.rmtree, self.cache, ignore_errors=True)
        extracted = subprocess.run([sys.executable, str(TOOL), '--game-root', GAME_ROOT, '--out', str(self.cache),
                                    '--allow-unknown-build', '--json'], capture_output=True, text=True, timeout=120)
        self.assertEqual(extracted.returncode, 0, extracted.stderr)

    def test_real_lan_lifecycle(self):
        import time
        ports = free_udp_ports(2)
        loopback = socket.gethostbyname('localhost')
        with socket.socket() as probe:
            probe.bind((loopback, 0))
            game_port = probe.getsockname()[1]
        scripts = [
            ['host:Socket probe', 'wait:room_player:Bob@180', 'shot:host_active', 'wait:chat:ready@600', 'sleep:300',
             'chat:leaving', 'sleep:90', 'leave', 'source:online', 'shot:host_after', 'sleep:300',
             'source:lan', 'shot:host_reopen', 'sleep:300', 'cancel'],
            ['wait:game:Socket probe@180', f'direct:{loopback}:{game_port}', 'wait:room_player:Bob@180',
             'shot:join_active', 'chat:ready', 'wait:chat:leaving@600', 'leave', 'source:online', 'shot:join_after', 'sleep:300',
             'source:lan', 'shot:join_reopen', 'sleep:300', 'cancel'],
        ]
        processes, streams = [], []
        samples = {}
        try:
            for index, (player, steps) in enumerate(zip(('Alice', 'Bob'), scripts)):
                name = ('hostprobe', 'joinprobe')[index]
                actions = [arg for step in steps for arg in ('--eawr-lan-do', step)]
                args = command(self.cache, name, self.pictures, self.pictures / (name + '.png'),
                               '--eawr-lan-name', player, '--eawr-lan-broadcast-port', str(ports[index]),
                               '--eawr-lan-broadcast-extra', str(ports[1-index]),
                               '--eawr-lan-game-port', str(game_port if index == 0 else 0), *actions)
                stream = (self.pictures / (name + '.log')).open('w', encoding='utf-8')
                streams.append(stream)
                processes.append(subprocess.Popen(args, stdout=stream, stderr=subprocess.STDOUT, text=True))
            deadline = time.monotonic() + 720
            wanted = ('host_active', 'join_active', 'host_after', 'join_after', 'host_reopen', 'join_reopen')
            while len(samples) < len(wanted) and time.monotonic() < deadline:
                for name in wanted:
                    if name in samples or not (self.pictures / (name + '.png')).is_file():
                        continue
                    who = 'hostprobe.json' if name.startswith('host') else 'joinprobe.json'
                    ps = r'''$p = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'Godot*.exe' -and $_.CommandLine -and $_.CommandLine.Contains($args[0]) });
$ids = @($p | ForEach-Object { [int]$_.ProcessId });
[ordered]@{ processes = @($p | Select-Object ProcessId,ExecutablePath,CommandLine); udp = @(Get-NetUDPEndpoint | Where-Object { $_.OwningProcess -in $ids } | Select-Object OwningProcess,LocalAddress,LocalPort); tcp = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -in $ids } | Select-Object OwningProcess,LocalAddress,LocalPort) } | ConvertTo-Json -Depth 6 -Compress'''
                    ps = ps.replace('$args[0]', "'" + who + "'")
                    query = ['powershell.exe', '-NoProfile', '-Command', ps]
                    result = subprocess.run(query, capture_output=True, text=True, timeout=30, check=True)
                    data = json.loads(result.stdout)
                    self.assertTrue(data['processes'], (name, data))
                    samples[name] = data
                    (self.pictures / 'socket-lifecycle.json').write_text(json.dumps({'udp_ports': ports, 'game_port': game_port, 'samples': samples}, indent=2), encoding='utf-8')
                if any(p.poll() is not None and any(name.startswith(prefix) and name not in samples for name in wanted)
                       for p, prefix in zip(processes, ('host', 'join'))):
                    break
                time.sleep(.2)
            self.assertEqual(set(samples), set(wanted), samples)
            for p in processes:
                p.wait(timeout=600)
            for stream in streams:
                stream.flush()
            for name, p in zip(('hostprobe', 'joinprobe'), processes):
                self.assertEqual(p.returncode, 0, (self.pictures / (name + '.log')).read_text(encoding='utf-8')[-5000:])
                load(self, self.pictures, name, 'probe')
            self.assertTrue(any(int(x['LocalPort']) == game_port for x in samples['host_active']['tcp']), samples)
            for i, name in enumerate(('host_active', 'join_active')):
                self.assertTrue(any(int(x['LocalPort']) == ports[i] for x in samples[name]['udp']), samples)
            for i, name in enumerate(('host_after', 'join_after')):
                self.assertFalse(any(int(x['LocalPort']) == ports[i] for x in samples[name]['udp']), samples)
                self.assertFalse(samples[name]['tcp'], samples)
            for i, name in enumerate(('host_reopen', 'join_reopen')):
                self.assertTrue(any(int(x['LocalPort']) == ports[i] for x in samples[name]['udp']), samples)
        finally:
            for p in processes:
                if p.poll() is None:
                    p.kill()
                    p.wait(timeout=30)
            for stream in streams:
                stream.close()


    @staticmethod
    def mod_room_overlay(directory, playable=3):
        """A made-up overlay mod with one multiplayer campaign of `playable` player factions and one AI faction (#8195).

        The names are invented here; nothing comes from the game's own files. The campaign is listed first, so a campaign room
        opens on it, and its home planets are stock planet names of the installed data. The harness mounts it with
        --eawr-mod-root, which has no workshop or Mods key: the guests cannot identify it and ask before they continue.
        """
        planets = ["Yavin", "Coruscant", "Corellia", "Kuat", "Dantooine", "Hypori"]
        names = [f"Room_Player_{index + 1}" for index in range(playable)] + ["Room_Ai_1"]
        root = pathlib.Path(directory) / "mod_room_overlay"
        xml = root / "Data" / "XML"
        xml.mkdir(parents=True, exist_ok=True)
        (xml / "FactionFiles.xml").write_text(
            '<?xml version="1.0" ?>\n<Faction_Files>\n<File>Factions.xml</File>\n<File>Expansion_Factions.xml</File>\n'
            '<File>Factions_Room.xml</File>\n</Faction_Files>\n', encoding="utf-8")
        (xml / "CampaignFiles.xml").write_text(
            '<?xml version="1.0" ?>\n<Campaign_Files>\n<File>Campaigns_Room.xml</File>\n<File>Campaigns_Underworld_GC.xml</File>\n'
            '<File>Campaigns_Underworld_Story.xml</File>\n<File>Campaigns_Multiplayer.xml</File>\n<File>Campaigns_Tutorial.xml</File>\n'
            '<File>Campaigns_Underworld_Tutorial.xml</File>\n</Campaign_Files>\n', encoding="utf-8")
        factions = ['<?xml version="1.0"?>\n<Factions>']
        for index, name in enumerate(names):
            factions.append(
                f'<Faction Name="{name}">\n<Is_Playable>{"true" if index < playable else "false"}</Is_Playable>\n'
                f'<Is_Neutral>No</Is_Neutral>\n<Basic_AI>BasicRebel</Basic_AI>\n'
                f'<Color>{40 + index * 20}, {200 - index * 15}, {90 + index * 10}, 255</Color>\n</Faction>')
        factions.append('</Factions>')
        (xml / "Factions_Room.xml").write_text("\n".join(factions) + "\n", encoding="utf-8")
        campaign = ['<?xml version="1.0" ?>\n<Campaigns>\n<Campaign Name="Mod_Room_Test">',
                    '<Campaign_Set> Multiplayer_Campaign_Set </Campaign_Set>',
                    '<Locations>\n' + ",\n".join(planets[:len(names)]) + '\n</Locations>']
        for index, name in enumerate(names):
            campaign.append(f'<Home_Location> {name}, {planets[index]} </Home_Location>')
        campaign += ['<Is_Multiplayer> Yes </Is_Multiplayer>',
                     '<Human_Victory_Conditions> Galactic_All_Planets_Controlled </Human_Victory_Conditions>',
                     '<AI_Victory_Conditions> Galactic_All_Planets_Controlled </AI_Victory_Conditions>',
                     f'<AI_Player_Control> {names[-1]}, BasicRebel </AI_Player_Control>', '</Campaign>', '</Campaigns>']
        (xml / "Campaigns_Room.xml").write_text("\n".join(campaign) + "\n", encoding="utf-8")
        return root

    def test_a_modded_galactic_lobby_with_two_guests_forms_once_each_answers_the_mod_review(self):
        # #8195: a campaign room on a mod the guests cannot identify (a mod folder mounted by path) holds every joiner in the "Mods
        # differ ... Continue joining?" box, on "Connecting...", until it answers. The harness had no way to answer it, so a room
        # with a mod and guests never formed; nothing in the room or the transport was wrong. With the answer scripted
        # (mod_review:yes), a host and two guests on one mod all reach the staging and list each other.
        mod = self.mod_room_overlay(self.directory)
        names = ["Host", "Guest B", "Guest C"]
        tag = "gc-mod-room"
        with socket.socket() as available:
            available.bind(("127.0.0.1", 0))
            game_port = available.getsockname()[1]
        ports = free_udp_ports(len(names))
        host_script = ["host_options", "host_dropdown:game_type=TEXT_GAME_TYPE_CAMPAIGN", "humans:3", "host:Mod room",
                       "wait:staging@60", *["wait:room_player:" + player + "@240" for player in names[1:]], "sleep:5",
                       "snapshot", "shot:result-" + tag, "chat:mod room shots done", "sleep:20", "leave"]
        guest_script = ["sleep:30", "direct:127.0.0.1:" + str(game_port), "wait:mod_review@180", "snapshot", "mod_review:yes",
                        *["wait:room_player:" + player + "@240" for player in names], "sleep:5", "snapshot",
                        "wait:chat:mod room shots done@240", "leave"]
        processes, logs = [], []
        try:
            for index, player in enumerate(names):
                report_name = f"{tag}-{index}"
                actions = [arg for step in (host_script if index == 0 else guest_script) for arg in ("--eawr-lan-do", step)]
                argv = command(self.cache, report_name, self.pictures, self.pictures / f"{report_name}.png",
                               "--eawr-lan-name", player, "--eawr-lan-colour", "0", "--eawr-mod-root", str(mod),
                               "--eawr-lan-broadcast-port", str(ports[index]),
                               *[arg for other in ports if other != ports[index]
                                 for arg in ("--eawr-lan-broadcast-extra", str(other))],
                               "--eawr-lan-game-port", str(game_port if index == 0 else 0), *actions)
                log = (self.pictures / f"{report_name}.log").open("w", encoding="utf-8")
                logs.append(log)
                processes.append(subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, text=True))
            for process in processes:
                process.wait(timeout=900)
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
            for log in logs:
                log.close()
        for index, process in enumerate(processes):
            output = (self.pictures / f"{tag}-{index}.log").read_text(encoding="utf-8", errors="replace")
            self.assertEqual(process.returncode, 0, output[-3000:])
            report = load(self, self.pictures, f"{tag}-{index}", output)
            if index == 0:
                continue  # the host's own steps wait for both guests to appear in its room
            held, joined = report["snapshots"]
            self.assertTrue(held["box_open"], "the joiner waits in the mod review box before it answers")
            self.assertEqual(held["status"], "Connecting...", held["status"])
            self.assertFalse(joined["box_open"], "answering the review closes the box")
            self.assertEqual(joined["page"], "staging")
            self.assertNotEqual(joined["status"], "Connecting...", joined["status"])


if __name__ == "__main__":
    unittest.main()
