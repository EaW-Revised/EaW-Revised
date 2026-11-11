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
assert 'if (!checking_mods && !status.empty()) note_chat(model::ChatKind::notice, status);' in refresh
assert 'mod_progress_notice' not in refresh
assert 'deferred_mod_review' in lobby

assert 'if (impl_->box) { impl_->review_digest_wait.reset(); return; }' in lobby
