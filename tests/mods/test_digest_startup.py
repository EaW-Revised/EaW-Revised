"""Guard startup priming and screen wait wiring."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
viewer = root / 'apps/viewer/src'
startup = (viewer / 'viewer_host_ready.cpp').read_text()
assert startup.index('prime_mounted_mods(options_->game_root);') < startup.index('if (options_->skirmish_setup || options_->front_end)')
occurrences = {p.relative_to(viewer).as_posix() for p in viewer.rglob('*') if p.is_file() and 'prime_mounted_mods(' in p.read_text(encoding='utf-8')}
assert occurrences == {'viewer_host_ready.cpp', 'viewer_mounted_mods.hpp'}, occurrences
header = (viewer / 'viewer_mounted_mods.hpp').read_text()
assert 'digests->apply(copy, true)' in header
assert 's.digests->lookup(record, false)' in header
lobby = (viewer / 'lan_lobby_screen_galactic.cpp').read_text().split('void EawrLanLobby::review_host_mods()', 1)[1]
assert 'matching_portable_trees(*host, *local)' in lobby and 'if (!cheap_match && !mounted_mods_ready())' in lobby
save = (viewer / 'galactic/galactic_save.cpp').read_text().split('void GalacticSaveControls::service()', 1)[1]
assert 'modal_ == Modal::checking_mods && !state_.options.mounted_mods && !mounted_mods_ready()' in save
assert 'modal_ != Modal::checking_mods || state_.options.mounted_mods || mounted_mods_ready()' in save
assert save.index('screen_wait_lease()') < save.index('viewer::saved_game_mod_prompt')
load = (root / 'src/presentation/godot/ui/load_game_view.cpp').read_text()
wait = load.split('if (context_.review_ready && !context_.review_ready())', 1)[1]
assert wait.index('context_.review_wait_lease()') < wait.index('return;') < wait.index('context_.review_save(')
front = (viewer / 'front_end_mode.cpp').read_text()
assert 'context.review_ready = ' in front and 'mounted_mods_ready()' in front
assert 'context.review_wait_lease = ' in front and 'screen_wait_lease()' in front

refresh = (viewer / 'lan_lobby_screen_refresh.cpp').read_text()
assert 'progress.files_total == 0 ? "Reading mod folders"' in refresh
assert 'EAWR_MOD_REVIEW_STATUS' in refresh
# Mod checks stay in the progress header; other notices pass through the
# counting-status throttle before reaching chat.
status_changed = refresh.split('if (status != impl.status) {', 1)[1].split('\nvoid EawrLanLobby::', 1)[0]
assert 'if (!checking_mods) {' in status_changed
chat_status = status_changed.split('if (!checking_mods) {', 1)[1]
assert 'const bool in_place = preparing && progress && status == room->prepare_status;' in chat_status
assert 'impl.status_chat.note(status, Time::get_singleton()->get_ticks_msec(), in_place)' in chat_status
assert chat_status.index('if (post != model::StatusChatPost::none) {') < chat_status.index('note_chat(model::ChatKind::notice, line);')
assert 'mod_progress_notice' not in refresh
assert 'deferred_mod_review' in lobby

assert 'if (impl_->box) { impl_->review_digest_wait.reset(); return; }' in lobby

# #6468: the "Mod list not shared" message raised when a host room opens must survive until the box is shown. open_staging runs right
# after host_galactic_game and must not clear it; a room starts with a clean notice in host_galactic_game, a room ends it in
# leave_staging, and review_host_mods turns it into the box (also from the per-frame staging refresh).
galactic = (viewer / 'lan_lobby_screen_galactic.cpp').read_text()
staging = (viewer / 'lan_lobby_screen_staging.cpp').read_text()
open_staging = staging.split('void EawrLanLobby::open_staging(', 1)[1].split('\nvoid EawrLanLobby::', 1)[0]
assert 'host_mod_notice' not in open_staging, 'open_staging must not clear a notice raised at room open'
leave_staging = staging.split('void EawrLanLobby::leave_staging()', 1)[1].split('\nvoid EawrLanLobby::', 1)[0]
assert 'host_mod_notice.clear()' in leave_staging
host_game = galactic.split('EawrLanLobby::host_galactic_game(', 1)[1].split('\nvoid EawrLanLobby::refresh_galactic_staging()', 1)[0]
assert host_game.index('host_mod_notice.clear()') < host_game.index('report_host_mod_list_not_shared('), 'a room starts clean, then may raise a notice'
assert 'viewer::host_mod_list_unsendable_reason(encoded)' in host_game
begin_hosting = staging.index('if (const auto refusal = host_galactic_game())')
assert begin_hosting < staging.index('open_staging(true, *cleaned, {});', begin_hosting)
shown = galactic.split('void EawrLanLobby::review_host_mods()', 1)[1]
assert 'host_mod_notice.empty()' in shown and 'caption = "Mod list not shared"' in shown
refresh_staging = galactic.split('void EawrLanLobby::refresh_galactic_staging()', 1)[1].split('\nvoid EawrLanLobby::', 1)[0]
assert 'review_host_mods();' in refresh_staging
