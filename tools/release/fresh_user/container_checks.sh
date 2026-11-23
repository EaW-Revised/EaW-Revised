#!/bin/bash
# Runs on the CLEAN Ubuntu 24.04 box as the plain user (started by fresh_user_linux.py), so a missing library shows up as a
# failed case instead of being installed away. Reports `FU-CASE <name> <PASS|FAIL|SKIP> <detail>`.
# Env: FU_WORK (folder holding the extracted package), FU_PACKAGE (top folder name), FU_OUT (evidence folder),
#      FU_GAME (REQUIRED: the FoC folder, only read), FU_PLAY_ARGS / FU_SKIRMISH_ARGS (optional overrides).
# The box needs xvfb and mesa-vulkan-drivers as the GATE's own tooling (a virtual display and a software Vulkan driver); they
# are not player dependencies. The gate_tooling case names whichever is missing.
set -u
GATE_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG="${FU_WORK:?}/${FU_PACKAGE:?}"
OUT="${FU_OUT:?}"
GAME="${FU_GAME:?set FU_GAME to the FoC folder (the gate needs a game root)}"
HOMEDIR="$OUT/home"
EMPTYHOME="$OUT/home-empty"
case_() { echo "FU-CASE $1 $2 $3"; }
last_lines() { grep -v 'ALSA\|mesh_get\|libpulse' "$1" 2>/dev/null | tail -2 | tr '\n' ' ' | cut -c1-300; }
cd "$PKG" || exit 3
# 1. SHA256SUMS lists what is there
if sha256sum -c --quiet SHA256SUMS >"$OUT/sums.log" 2>&1; then
  extra=$(find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | comm -13 <(awk '{print $2}' SHA256SUMS | sort) - | head -3 | tr '\n' ' ')
  if [ -z "$extra" ]; then case_ sums PASS "all files match SHA256SUMS"; else case_ sums FAIL "files not in SHA256SUMS: $extra"; fi
else case_ sums FAIL "$(head -3 "$OUT/sums.log" | tr '\n' ' ')"; fi
# 2. exec bits as extracted
if [ -x play.sh ] && [ -x godot/godot.linuxbsd.editor.x86_64 ]; then case_ exec_bits PASS "play.sh and the engine are executable after extraction"
else case_ exec_bits FAIL "play.sh or the engine is not executable"; fi
# 3. runtime.json names the engine that is there
actual=$(sha256sum godot/godot.linuxbsd.editor.x86_64 2>/dev/null | cut -d' ' -f1)
if [ -n "$actual" ] && grep -q "$actual" runtime.json 2>/dev/null; then case_ runtime_engine_hash PASS "$actual"
else case_ runtime_engine_hash FAIL "engine sha256 ${actual:-missing} not in runtime.json"; fi
# 4. no missing shared libraries for the engine and the viewer extension
missing=$( { ldd godot/godot.linuxbsd.editor.x86_64; ldd project/bin/libeawr_viewer.linux.template_release.x86_64.so; } 2>&1 | grep -E 'not found|No such file' | sort -u | tr '\n' ' ')
if [ -z "$missing" ]; then case_ ldd_complete PASS "no missing libraries"; else case_ ldd_complete FAIL "$missing"; fi
# 5. glibc floor of the binaries (2.38)
need=$( { objdump -T godot/godot.linuxbsd.editor.x86_64 project/bin/*.so 2>/dev/null; } | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
have=$(ldd --version | head -1 | grep -o '[0-9]\+\.[0-9]\+$')
if [ -z "$need" ]; then
  # objdump is not installed in the minimal image: fall back to the loader's own verdict
  if godot/godot.linuxbsd.editor.x86_64 --version >/dev/null 2>&1; then case_ glibc_floor PASS "loader accepts the binary on glibc $have"; else case_ glibc_floor FAIL "loader refused the engine on glibc $have"; fi
elif [ "$(printf '%s\n%s\n' "${need#GLIBC_}" "$have" | sort -V | tail -1)" = "$have" ]; then case_ glibc_floor PASS "needs $need, host has $have"
else case_ glibc_floor FAIL "needs $need, host has $have"; fi
# From here on: an unprivileged user, a read-only package folder, an empty HOME
rm -rf "$HOMEDIR" "$EMPTYHOME"; mkdir -p "$HOMEDIR" "$EMPTYHOME" "$OUT/run"
chmod -R a-w "$PKG"
before=$(find . -type f -printf '%p %s %m\n' | sort | sha256sum | cut -d' ' -f1)
uid=$(id -u)
tmp_list() { find /tmp -mindepth 1 -maxdepth 3 -user "$uid" ! -path '/tmp/.X11-unix*' ! -name '.X*-lock' 2>/dev/null | sort; }
tmp_before=$(tmp_list)
# No VK_ICD_FILENAMES override: the loader must find the software driver the way a user's machine would.
USERENV=(env -i HOME="$HOMEDIR" PATH=/usr/bin:/bin LIBGL_ALWAYS_SOFTWARE=1)
as_user() { "${USERENV[@]}" "$@"; }
# 6. the engine reports the patched identity
ver=$(as_user ./godot/godot.linuxbsd.editor.x86_64 --headless --version 2>&1 | tail -1)
if echo "$ver" | grep -q '\.eawr\.'; then case_ engine_version PASS "$ver"; else case_ engine_version FAIL "version: $ver"; fi
# 7. the gate's own tooling (not a player dependency): a virtual display and a software Vulkan driver
tooling_missing=
command -v xvfb-run >/dev/null 2>&1 || tooling_missing="$tooling_missing xvfb (xvfb-run)"
ls /usr/share/vulkan/icd.d/lvp_icd*.json >/dev/null 2>&1 || tooling_missing="$tooling_missing mesa-vulkan-drivers (lavapipe)"
if [ -z "$tooling_missing" ]; then case_ gate_tooling PASS "xvfb-run and the lavapipe Vulkan driver are installed on the box"
else case_ gate_tooling FAIL "install on the box (gate tooling, not player dependencies):$tooling_missing"; fi
have_display=1; [ -z "$tooling_missing" ] || have_display=0
need_tooling() { case_ "$1" FAIL "needs the gate tooling, see gate_tooling:$tooling_missing"; }
# 8. play.sh --check: with the game root it reports success and does not start the game
check_out=$(as_user ./play.sh --game-root "$GAME" --check 2>&1); rc=$?
if [ $rc -eq 0 ] && echo "$check_out" | grep -q 'checks passed'; then case_ play_check PASS "play.sh --check reported 'checks passed' with the game root given"
else case_ play_check FAIL "exit $rc, game root given: $(echo "$check_out" | tail -2 | tr '\n' ' ' | sed "s|$GAME|<game root>|g" | cut -c1-300)"; fi
# 9. no Forces of Corruption folder: a clear message and exit 3, with --game-root on an empty folder and with no argument
mkdir -p "$OUT/no-foc"
miss1=$(as_user ./play.sh --game-root "$OUT/no-foc" 2>&1); rc1=$?
miss2=$(env -i HOME="$EMPTYHOME" PATH=/usr/bin:/bin ./play.sh 2>&1); rc2=$?
if [ $rc1 -eq 3 ] && [ $rc2 -eq 3 ] && echo "$miss1" | grep -q 'cannot find your Empire at War' && echo "$miss2" | grep -q 'cannot find your Empire at War'; then
  case_ foc_missing PASS "exit 3 and 'cannot find your Empire at War' with an empty folder and with no argument in an empty HOME"
else case_ foc_missing FAIL "empty folder: exit $rc1, no argument: exit $rc2; $(echo "$miss2" | head -1 | cut -c1-200)"; fi
# 10. play.sh reaches the main menu and the game then exits by itself, 0, on the Escape key (the main menu's quit key; injected with XSendEvent, no extra package
#     on the box). A kill by `timeout` (exit 124) is a FAIL. Needs the game folder.
#     args is an ARRAY so a game root with spaces ("Star Wars Empire at War") stays one argument; the FU_PLAY_ARGS override is split on spaces.
if [ -n "${FU_PLAY_ARGS:-}" ]; then read -r -a args <<<"$FU_PLAY_ARGS"; else args=(--game-root "$GAME"); fi
# run_closing <log> <ready pattern> <command...>: runs the command in a virtual display, waits for the pattern in its log, then
# presses Escape in the game window and returns the game's own exit status (124 = still running at the 400 s limit).
run_closing() {
  # Robust under load (a loaded 4-CPU box once took longer than one fixed wait): wait for the ready marker (up to limit-180 s), settle 8 s,
  # then press Escape and keep pressing it every 15 s (up to 8 tries) for as long as the game is still running; the exit then has the
  # rest of the (generous, default 720 s) bound. Hitting the bound still ends in exit 124 = FAIL; a pass needs the game's own exit 0.
  local log=$1 pat=$2 limit=${RUN_LIMIT:-720}; shift 2
  timeout "$limit" "${USERENV[@]}" xvfb-run -a -s "-screen 0 1280x720x24" bash -c '
    log=$1; pat=$2; helper=$3; limit=$4; shift 4
    : >"$log.esc"
    "$@" >"$log" 2>&1 & gpid=$!
    ( for i in $(seq 1 $((limit - 180))); do grep -q -- "$pat" "$log" 2>/dev/null && break; kill -0 $gpid 2>/dev/null || exit 0; sleep 1; done
      sleep 8
      for n in 1 2 3 4 5 6 7 8; do kill -0 $gpid 2>/dev/null || break; python3 "$helper" >>"$log.esc" 2>&1; echo "press_escape exit $?" >>"$log.esc"; sleep 15; done ) &
    helperpid=$!
    wait $gpid; rc=$?; kill $helperpid 2>/dev/null; exit $rc' _ "$log" "$pat" "$GATE_DIR/press_escape.py" "$limit" "$@"
}
if [ $have_display -eq 1 ]; then
  run_closing "$OUT/run/start.log" 'EAWR front end ready' ./play.sh "${args[@]}"; rc=$?
  if [ $rc -eq 0 ] && grep -q 'EAWR front end screen: main_menu' "$OUT/run/start.log" && grep -q 'EAWR front end ready' "$OUT/run/start.log"; then
    case_ start_reaches_screen PASS "play.sh reached the main menu; $(grep -m1 -o 'Vulkan.*llvmpipe[^)]*)' "$OUT/run/start.log" | cut -c1-120)"
  else case_ start_reaches_screen FAIL "exit $rc, main menu not reported, see run/start.log: $(last_lines "$OUT/run/start.log")"; fi
  report="$HOMEDIR/.local/state/eaw-revised/last-run.json"
  reason=$(sed -n 's/.*"exit_reason": *"\([a-z_]*\)".*/\1/p' "$report" 2>/dev/null | head -1)
  esc_ok=0; grep -q "^sent Escape to [1-9][0-9]* window" "$OUT/run/start.log.esc" && grep -q "^press_escape exit 0$" "$OUT/run/start.log.esc" && esc_ok=1
  if [ $rc -eq 0 ] && [ "$reason" = clean_quit ] && [ $esc_ok -eq 1 ]; then
    case_ clean_quit PASS "exit 0 by the game itself after the Escape key, exit_reason clean_quit"
  else case_ clean_quit FAIL "exit $rc (124 means killed by the timeout), exit_reason '${reason:-none}' (must be clean_quit), Escape helper ok: $esc_ok"; fi
else need_tooling start_reaches_screen; need_tooling clean_quit; fi
# 11. the Online route loads at runtime: the front end's scripted Internet-lobby walk reports the online lobby's source. The walk
#     has no exit of its own, so the gate stops the game once the line is in the log (the clean exit is the clean_quit case above).
if [ $have_display -eq 1 ]; then
  timeout 900 "${USERENV[@]}" xvfb-run -a -s "-screen 0 1280x720x24" bash -c '
    log=$1; shift
    "$@" >"$log" 2>&1 & pid=$!
    for i in $(seq 1 850); do grep -q "EAWR online service lobby source:" "$log" 2>/dev/null && break; kill -0 $pid 2>/dev/null || break; sleep 1; done
    sleep 2; kill $pid 2>/dev/null; sleep 3; kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null; exit 0' _ "$OUT/run/online.log" ./play.sh "${args[@]}" -- --eawr-front-end-test-online; rc=$?
  # The source must be exactly "online": a LAN fallback prints "lan" and must FAIL.
  line=$(grep -m1 'EAWR online service lobby source:' "$OUT/run/online.log")
  if grep -qE 'EAWR online service lobby source: online[[:space:]]*$' "$OUT/run/online.log"; then case_ online_lib_runtime PASS "the engine reported: $line"
  elif [ -n "$line" ]; then case_ online_lib_runtime FAIL "the lobby source is not online (online route fell back): $line"
  else case_ online_lib_runtime FAIL "exit $rc, no online lobby line in run/online.log: $(last_lines "$OUT/run/online.log")"; fi
else need_tooling online_lib_runtime; fi
# 12. the runs wrote their logs into the empty HOME
report="$HOMEDIR/.local/state/eaw-revised/last-run.json"
enginelog=$(find "$HOMEDIR" -name '*.log' -size +0 2>/dev/null | head -1)
if [ -s "$report" ] && grep -q '"exit_reason"' "$report" && [ -n "$enginelog" ]; then
  case_ logs_written PASS "last-run.json and the engine log (${enginelog#$HOMEDIR/}) exist in the empty HOME"
else case_ logs_written FAIL "last-run.json $([ -s "$report" ] && echo present || echo missing), engine log ${enginelog:-missing} under $HOMEDIR"; fi
# 13. a skirmish against the AI (the menu path is GUI-only): the bundled engine runs the same project with the skirmish
#     options, 900 ticks (about 2 minutes under software rendering); the game exits by itself and the report must show equal sim hashes.
if [ $have_display -eq 1 ]; then
  # sargs is an array: the game root and the report path may hold spaces (the FU_SKIRMISH_ARGS override is split on spaces).
  if [ -n "${FU_SKIRMISH_ARGS:-}" ]; then read -r -a sargs <<<"$FU_SKIRMISH_ARGS"
  else sargs=(--eawr-game-root "$GAME" --eawr-live-session skirmish --eawr-map data/art/maps/_mp_space_kessel.ted --eawr-populate --eawr-skirmish-slot 1:Rebel:0:human --eawr-skirmish-slot 2:Empire:1:ai --eawr-live-ai on --eawr-live-ticks 900 --eawr-live-step 3 --eawr-report "$OUT/run/skirmish-report.json"); fi
  t0=$(date +%s); timeout 400 "${USERENV[@]}" xvfb-run -a -s "-screen 0 1280x720x24" ./godot/godot.linuxbsd.editor.x86_64 --path project --rendering-driver vulkan -- "${sargs[@]}" >"$OUT/run/skirmish.log" 2>&1; rc=$?; secs=$(( $(date +%s) - t0 ))
  if [ $rc -eq 0 ] && grep -q '"headless_hashes_equal": *true' "$OUT/run/skirmish-report.json" 2>/dev/null; then
    case_ skirmish_runs PASS "skirmish vs AI ran ${secs}s wall under software rendering, sim hashes equal"
  else case_ skirmish_runs FAIL "exit $rc after ${secs}s, see run/skirmish.log: $(last_lines "$OUT/run/skirmish.log")"; fi
else need_tooling skirmish_runs; fi
# 14. no writes outside the user dir: HOME holds only the game's state, settings and engine data; /tmp gained nothing of ours
stray=$(cd "$HOMEDIR" && find . -mindepth 1 \( -type f -o -type l \) 2>/dev/null | sed 's|^\./||' | grep -v -E '^(\.local/state/eaw-revised/|\.local/share/godot/|\.config/godot/|\.cache/)' | head -5 | tr '\n' ' ')
tmp_after=$(tmp_list)
tmp_new=$(comm -13 <(echo "$tmp_before") <(echo "$tmp_after") | head -5 | tr '\n' ' ')
if [ -z "$stray" ] && [ -z "$tmp_new" ]; then case_ user_dir_only PASS "HOME holds only .local/state/eaw-revised, engine data and caches; no new files of the user in /tmp"
else case_ user_dir_only FAIL "unexpected in HOME: ${stray:-none}; new in /tmp: ${tmp_new:-none}"; fi
# 15. the runs wrote nothing into the package folder
after=$(find . -type f -printf '%p %s %m\n' | sort | sha256sum | cut -d' ' -f1)
if [ "$before" = "$after" ]; then case_ package_unchanged PASS "package folder identical after the runs (read-only)"; else case_ package_unchanged FAIL "package folder changed"; fi
chmod -R u+w "$PKG" 2>/dev/null
exit 0
