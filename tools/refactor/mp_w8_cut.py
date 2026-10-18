#!/usr/bin/env python3
"""Repeatable W8 move cuts. Run t21, t22a, then t22b on a clean main checkout.

Cuts locate symbols, never line numbers. Existing destinations make a cut a no-op;
unexpected source shapes fail before writing. Move maps contain only old/new;
separate identity sidecars record normalized body hashes and inline-helper glue.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[2]
VIEW = "apps/viewer/src/"
NET = "src/platform/net/"
NS = "namespace eawr::presentation::godot_backend {\n"
END = "} // namespace eawr::presentation::godot_backend\n"


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def write(path, value):
    target = ROOT / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(value, encoding="utf-8", newline="\n")


def edit(path, old, new):
    source = read(path)
    if old not in source:
        raise ValueError(f"{path}: missing {old!r}")
    write(path, source.replace(old, new))


def manifest(ticket, moves):
    # The registry relocator accepts exactly old/new. Evidence is kept separately.
    mapping = [{key: move[key] for key in ("old", "new")} for move in moves]
    write(f"tools/refactor/mp_w8_{ticket}_moves.json", json.dumps(mapping, indent=2) + "\n")
    write(f"tools/refactor/mp_w8_{ticket}_identity.json", json.dumps(moves, indent=2) + "\n")


def entry(old, new, symbol, body):
    suffix = "" if symbol == "*" else "#" + symbol
    return {"old": old + suffix, "new": new + suffix,
            "sha256": hashlib.sha256(body.encode()).hexdigest()}


def viewer_manifest():
    fragment = VIEW + "sources.cmake"
    return fragment if (ROOT / fragment).exists() else "apps/viewer/CMakeLists.txt"


def t21():
    names = ["lan_match.cpp", "lan_match.hpp", "lan_match_fake.cpp", "match_transport.hpp",
             "online_match.cpp", "online_match.hpp", "skirmish_prepare.cpp", "skirmish_prepare.hpp",
             "lan_diagnostics.hpp", "online_service_settings.hpp"]
    if (ROOT / (NET + "lan_match.cpp")).exists():
        return
    contents = {name: read(VIEW + name) for name in names}
    assert all("godot_cpp/" not in body for body in contents.values())
    for name, body in contents.items():
        write(NET + name, body)
        (ROOT / (VIEW + name)).unlink()
    sources = [name for name in names if name.endswith(".cpp")]
    write(NET + "sources.cmake", "set(EAWR_PLATFORM_NET_SOURCES\n" + "".join(
        f'    "${{CMAKE_CURRENT_LIST_DIR}}/{name}"\n' for name in sources) + ")\n")
    write(NET + "CMakeLists.txt", '''include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")
add_library(eawr_platform_net STATIC ${EAWR_PLATFORM_NET_SOURCES})
add_library(eawr::platform_net ALIAS eawr_platform_net)
target_include_directories(eawr_platform_net PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(eawr_platform_net PUBLIC eawr::net eawr::platform eawr::skirmish eawr::ui_model eawr::scene)
target_compile_features(eawr_platform_net PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_platform_net)
''')
    edit("CMakeLists.txt", "add_subdirectory(src/skirmish)", "add_subdirectory(src/skirmish)\nadd_subdirectory(src/platform/net)")
    for path in ("tests/net/CMakeLists.txt", "apps/online_test/CMakeLists.txt"):
        source = read(path)
        for name in sources:
            source = re.sub(r'\n\s*"\$\{PROJECT_SOURCE_DIR\}/apps/viewer/src/' + re.escape(name) + r'"', "", source)
        for target in ("net_viewer_lan_tests", "net_viewer_online_tests", "eawr_online_test"):
            source = source.replace(f"target_link_libraries({target} PRIVATE ", f"target_link_libraries({target} PRIVATE eawr::platform_net ")
        write(path, source)
    path = viewer_manifest()
    source = read(path)
    for name in sources:
        source = source.replace("    src/" + name + "\n", "")
        source = source.replace('    "${CMAKE_CURRENT_LIST_DIR}/' + name + '"\n', "")
    write(path, source)
    path = "apps/viewer/CMakeLists.txt"
    source = read(path)
    source = source.replace('include("${EAWR_ROOT}/src/platform/sources.cmake")',
        'include("${EAWR_ROOT}/src/platform/sources.cmake")\ninclude("${EAWR_ROOT}/src/platform/net/sources.cmake")')
    source = source.replace("    ${EAWR_PLATFORM_SOURCES}", "    ${EAWR_PLATFORM_NET_SOURCES}\n    ${EAWR_PLATFORM_SOURCES}")
    source = source.replace('    "${EAWR_ROOT}/include"', '    "${EAWR_ROOT}/src/platform/net"\n    "${EAWR_ROOT}/include"')
    write(path, source)
    path = "tests/presentation/renderer/viewer_mode_sources.py"
    source = read(path)
    for name in names:
        # Preserve logical keys; relocate only physical manifest members.
        source = re.sub(r'(?m)^(    "[^"]+": )(.*)$',
                        lambda m: m[1] + m[2].replace(VIEW + name, NET + name), source)
    source = source.replace('        "apps/viewer/src/skirmish_prepare.cpp",', '        "src/platform/net/skirmish_prepare.cpp",')
    write(path, source)
    # Direct readers retain the same single-file scope after the physical move.
    for path in ("tests/presentation/renderer/test_lan_lobby.py",):
        source = read(path)
        source = source.replace('(SRC / "online_service_settings.hpp").read_text(encoding="utf-8")',
                                '(ROOT / "src/platform/net/online_service_settings.hpp").read_text(encoding="utf-8")')
        source = source.replace('(SRC / name).read_text(encoding="utf-8"), name)',
                                'source_text("apps/viewer/src/" + name), name)')
        source = source.replace('self.assertIn("skirmish_prepare.cpp", (ROOT / "tests/net/CMakeLists.txt").read_text(encoding="utf-8"))',
                                'self.assertIn("eawr::platform_net", (ROOT / "tests/net/CMakeLists.txt").read_text(encoding="utf-8"))')
        source = source.replace('self.assertIn("src/skirmish_prepare.cpp",', 'self.assertIn("src/platform/net/skirmish_prepare.cpp",')
        write(path, source)
    manifest("t21", [entry(VIEW + name, NET + name, "*", body) for name, body in contents.items()])


GROUPS = {
    "list": "open_lobby refresh_lobby join_selected join_address cancel_join refresh_online_rooms select_source configure_online_match join_code join_online_selected begin_online_join open_password_prompt close_password_prompt submit_password online_failure pump_online on_rows_chosen",
    "chat": "draw_chat commit_name_from_edit note_chat play_sound commit_name commit_colour send_chat announce",
    "staging": "fill_host_seats prepare_host_rows begin_hosting publish_staging_selection configure_staging open_staging fill_map_list leave_staging",
    "options": "open_host_options build_options_overlay open_options show_options_values close_options process_options open_filter open_direct_connect",
    "refresh": "refresh_staging run_action condition",
}


def t22(ticket):
    path = VIEW + "lan_lobby_screen.cpp"
    source = read(path)
    if (ROOT / (VIEW + f"lan_lobby_screen_{'list' if ticket == 't22a' else 'options'}.cpp")).exists():
        return
    internal = VIEW + "lan_lobby_screen_internal.hpp"
    moves = []
    if ticket == "t22a" and not (ROOT / internal).exists():
        # Includes, helper functions, local widget type and private state definitions.
        prefix, source = source.split("EawrLanLobby::EawrLanLobby()", 1)
        for declaration, symbol in (("struct GameTypeEntry", "GameTypeEntry"),
                                    ("class EawrLobbyGameList", "EawrLobbyGameList"),
                                    ("struct EawrLanLobby::Resources", "EawrLanLobby::Resources"),
                                    ("struct EawrLanLobby::PageState", "EawrLanLobby::PageState"),
                                    ("struct EawrLanLobby::Impl", "EawrLanLobby::Impl")):
            start = prefix.index(declaration)
            end = prefix.index("\n};", start) + len("\n};")
            moved = entry(path, internal, symbol, prefix[start:end])
            if symbol == "GameTypeEntry":
                moved["new"] = internal + "#lan_lobby_detail::GameTypeEntry"
            moves.append(moved)
        for symbol in ("screen_layer", "lobby_width", "lobby_height", "game_types", "lobby_dialog", "host_dialog",
                       "staging_dialog", "filter_dialog", "direct_dialog", "direct_button", "lan_source_button",
                       "online_source_button", "invitation_edit", "cancel_join_button", "refresh_button", "prev_button",
                       "next_button", "page_label", "code_button", "online_notice_label", "prompt_panel", "prompt_edit",
                       "prompt_title_label", "prompt_error_label", "prompt_ok_button", "prompt_cancel_button", "text",
                       "utf8", "same_ci", "within_one_edit", "default_font_cache", "colour_name", "same_slots",
                       "network_row_count", "staging_options", "now_ms", "cast", "local_rect", "find_gadget"):
            moves.append({"old": path + "#" + symbol, "new": internal + "#lan_lobby_detail::" + symbol,
                          "glue": "named inline helper/constant; body and value unchanged"})
        moves.append({"old": path + "#resolved", "new": internal + "#resolved",
                      "glue": "static helper becomes inline; body unchanged"})
        source = "EawrLanLobby::EawrLanLobby()" + source
        caption_start = source.index("static void fit_control_caption(")
        caption_end = source.index("\n}\n", caption_start) + len("\n}\n")
        caption_helper = source[caption_start:caption_end].replace("static void ", "inline void ", 1)
        source = source[:caption_start] + source[caption_end:]
        moves.append({"old": path + "#fit_control_caption", "new": internal + "#lan_lobby_detail::fit_control_caption",
                      "glue": "static helper becomes named inline; body unchanged"})
        begin = source.index("namespace {\n")
        finish = source.index("void EawrLanLobby::on_pressed")
        helpers = source[begin:finish]
        source = source[:begin] + source[finish:]
        header = prefix + helpers
        header = header.replace("namespace {", "namespace lan_lobby_detail {")
        header = header.replace("} // namespace\n", "} // namespace lan_lobby_detail\nusing namespace lan_lobby_detail;\n")
        header = re.sub(r"(?m)^constexpr ", "inline constexpr ", header)
        # Shared helpers have external inline linkage so inline widget methods obey ODR.
        header = re.sub(r"(?m)^(\[\[nodiscard\]\] )?(static )?((?:String|std::\w+(?:::\w+)*(?:<[^\n]*>)?|bool|Rect2|const BuiltGadget\*|T\*|skirmish::FixtureOptions) \w+\()",
                        lambda m: (m[1] or "") + "inline " + m[3], header)
        header += "namespace lan_lobby_detail {\n" + caption_helper + "} // namespace lan_lobby_detail\n" + END
        write(internal, "#pragma once\n" + header)
        source = '#include "lan_lobby_screen_internal.hpp"\n\n' + NS + source
    groups = ("list", "chat", "staging") if ticket == "t22a" else ("options", "refresh")
    # A member begins at column zero; the next top-level member/end bounds its body.
    pattern = re.compile(r"(?m)^(?:[\w:<>,*& ]+ )?EawrLanLobby::(?:~?\w+)\([^\n]*")
    for group in groups:
        destination = VIEW + f"lan_lobby_screen_{group}.cpp"
        if (ROOT / destination).exists():
            continue
        blocks = []
        for name in GROUPS[group].split():
            matches = list(pattern.finditer(source))
            found = [i for i, m in enumerate(matches) if re.search(r"::" + name + r"\(", m[0])]
            if len(found) != 1:
                raise ValueError(f"expected one member {name}, got {len(found)}")
            index = found[0]
            start = matches[index].start()
            end = matches[index + 1].start() if index + 1 < len(matches) else source.rindex(END)
            block = source[start:end]
            blocks.append(block)
            moves.append(entry(path, destination, "EawrLanLobby::" + name, block))
            source = source[:start] + source[end:]
        write(destination, '#include "lan_lobby_screen_internal.hpp"\n\n' + NS + "".join(blocks) + END)
        manifest_path = viewer_manifest()
        if manifest_path.endswith("sources.cmake"):
            anchor = '    "${CMAKE_CURRENT_LIST_DIR}/lan_lobby_screen.cpp"\n'
            addition = '    "${CMAKE_CURRENT_LIST_DIR}/' + pathlib.Path(destination).name + '"\n'
        else:
            anchor = "    src/lan_lobby_screen.cpp\n"
            addition = "    src/" + pathlib.Path(destination).name + "\n"
        edit(manifest_path, anchor, anchor + addition)
    write(path, source)
    manifest(ticket, moves)
    reader = "tests/presentation/renderer/viewer_mode_sources.py"
    text = read(reader)
    members = [path, internal] + [VIEW + f"lan_lobby_screen_{g}.cpp" for g in GROUPS if (ROOT / (VIEW + f"lan_lobby_screen_{g}.cpp")).exists()]
    row = '    "' + path + '": (' + ", ".join(repr(p) for p in members) + "),"
    if re.search(r'^    "' + path + r'":.*$', text, re.M):
        text = re.sub(r'^    "' + path + r'":.*$', lambda _: row, text, flags=re.M)
    else:
        text = text.replace("SOURCE_BUNDLES = {", "SOURCE_BUNDLES = {\n" + row)
    write(reader, text)
    test = "tests/presentation/renderer/test_lan_lobby.py"
    text = read(test).replace('(SRC / "lan_lobby_screen.cpp").read_text(encoding="utf-8")', 'source_text("apps/viewer/src/lan_lobby_screen.cpp")')
    for group, names in GROUPS.items():
        if not (ROOT / (VIEW + f"lan_lobby_screen_{group}.cpp")).exists():
            continue
        for name in names.split():
            text = re.sub(r'screen\.split\(("(?:void|bool) EawrLanLobby::' + name + r'[^\n]*?)',
                          lambda m: f'(SRC / "lan_lobby_screen_{group}.cpp").read_text(encoding="utf-8").split(' + m[1], text)
    write(test, text)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ticket", choices=("t21", "t22a", "t22b"))
    args = parser.parse_args()
    t21() if args.ticket == "t21" else t22(args.ticket)
