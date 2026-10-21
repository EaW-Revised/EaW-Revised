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
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import source_text  # noqa: E402
from dialog_rectangles import assert_dialog_rectangles  # noqa: E402

SRC = ROOT / "apps/viewer/src"
TOOL = ROOT / "tools/fonts/extract_eaw_fonts.py"

RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")


class LanLobbyStructure(unittest.TestCase):
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
        self.assertIn('label->set_text("Human players")', screen)
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
        self.assertIn("online_failure_text(model::classify_online_failure(diagnostic.code))", failure)
        self.assertNotIn("diagnostic.message", failure)
        pump = (SRC / "lan_lobby_screen_list.cpp").read_text(encoding="utf-8").split("void EawrLanLobby::pump_online()", 1)[1].split("// ---- Host options", 1)[0]
        self.assertNotIn("refusal->message", pump)
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


def command(cache, name, directory, picture, *extra, resolution="1280x720"):
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
    return [executable, "--resolution", resolution, "--max-fps", "30", "--path", str(ROOT / "apps/viewer/project"),
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
                                    f"network.online_port={port}\nnetwork.online_pin=" + "a" * 64 + "\n", encoding="utf-8")
            else:
                settings.unlink(missing_ok=True)
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
        self.assertTrue(any("Locked night | Echo | 3/9 | Endor | ?" in game and "password" in game for game in first["games"]),
                        first["games"])
        self.assertEqual((second["room_page"], len(second["games"])), (1, 8))
        self.assertEqual(by_host["games"][0].split(" | ")[1], "Echo")
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
        self.assertIn("connections between players", staging["start"]["reason"])
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
        self.assertFalse(join["local_ready"], "no Accept was needed to populate the screen")
        self.assertFalse(host["start"]["enabled"])
        self.assertNotIn("IDC_IMAGE", join["hidden"])
        self.assertNotIn("IDC_STAGING_MAP_LIST", join["hidden"])
        self.assertTrue((self.pictures / "joiner-map-options.png").is_file())

    def test_two_viewers_host_find_join_and_both_stagings_show_both_players(self):
        host_port, join_port = free_udp_ports(2)
        host_name, join_name, game = "Alice", "Bob", "Fleet night"
        common_host = ["--eawr-lan-broadcast-port", str(host_port), "--eawr-lan-broadcast-extra", str(join_port),
                       "--eawr-lan-game-port", "0"]
        common_join = ["--eawr-lan-broadcast-port", str(join_port), "--eawr-lan-broadcast-extra", str(host_port)]
        host_script = ["name:" + host_name, "colour:2", "host:" + game, "ai:2:hard", "wait:room_player:" + join_name + "@120", "snapshot",
                       "shot:two-host-staging", "ready", "wait:ready:all@180", "snapshot", "shot:two-host-ready",
                       "chat:ready from host", "wait:chat:ready from joiner@120", "name:Alice Prime", "sleep:10",
                       "snapshot", "chat:name-only", "wait:unready@120", "snapshot", "shot:two-host-reset",
                       "chat:choices reset", "ready", "wait:ready:all@180", "snapshot", "shot:two-host-reready",
                       "kick:1", "sleep:30", "snapshot"]
        join_script = ["name:" + join_name, "colour:4", "wait:game:" + game + "@120", "snapshot", "shot:two-join-lobby",
                       "join:" + game, "wait:room_player:" + join_name + "@120", "snapshot", "shot:two-join-staging",
                       "accept", "wait:ready:all@180", "wait:chat:ready from host@120", "chat:ready from joiner",
                       "wait:chat:name-only@120", "colour:5", "wait:unready@120", "snapshot", "shot:two-join-reset",
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
        self.assertEqual(reset["ready_seats"], 0)
        self.assertEqual(reset["rows"][1]["colour"], join["snapshots"][2]["rows"][1]["colour"])
        self.assertTrue(reready["start"]["enabled"])
        self.assertEqual(kicked["ready_seats"], 0)
        self.assertTrue(join["snapshots"][-1]["box_open"])
        # Each viewer received the other's staging chat line, once.
        self.assertTrue(any(line.endswith("ready from joiner") for line in host["final"]["chat"]))
        self.assertTrue(any(line.endswith("ready from host") for line in join["final"]["chat"]))
        # The chat sound names resolve against the effective sound data, not a fixed spelling.
        self.assertTrue(any("chat_re" in sound.lower() for sound in join["sounds"]), join["sounds"])


if __name__ == "__main__":
    unittest.main()
