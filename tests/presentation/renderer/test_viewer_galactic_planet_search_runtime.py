"""PS-01..06 and PS-08: real text, keyboard and pointer input and the live row pictures, at 1080p and ultrawide."""
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support

DRIVER = '''extends SceneTree
var elapsed = 0.0
var stage = 0
var directory = %s
var viewport_size = Vector2i(%d, %d)
var extra_query = %s
var facts = {}
func _initialize(): call_deferred("start")
func start():
    # This is an input probe with fixed capture identities, so desktop tiling
    # may resize the OS window without changing the 1080p/ultrawide viewport.
    root.content_scale_factor = 1.0
    root.content_scale_aspect = Window.CONTENT_SCALE_ASPECT_KEEP
    root.content_scale_size = viewport_size
    root.content_scale_mode = Window.CONTENT_SCALE_MODE_VIEWPORT
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func tap(code, unicode_value = 0, control = false):
    for down in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.ctrl_pressed = control
        event.unicode = unicode_value if down else 0
        event.pressed = down
        Input.parse_input_event(event)
func click(point):
    # Rectangles are in the fixed viewport, which may be letterboxed inside
    # the rig's smaller desktop window. Deliver native GUI events in that space.
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    root.push_input(motion, true)
    for down in [true, false]:
        var event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_LEFT
        event.position = point
        event.global_position = point
        event.pressed = down
        root.push_input(event, true)
func shot(label): root.get_texture().get_image().save_png(directory + "/" + label + ".png")
func pictures(list, label):
    # Every onscreen row's icon is a cell of the live atlas; save each cell for the eye check and
    # count the cells the planet actually covers (alpha), so an empty or squared-off cell fails.
    var atlas_rows = 0
    var covered = []
    for row in range(list.item_count):
        var rect = list.get_item_rect(row, true)
        if rect.position.y >= list.size.y: break
        var icon = list.get_item_icon(row)
        if not (icon is AtlasTexture): continue
        atlas_rows += 1
        var image = icon.get_image()
        image.save_png(directory + "/cell-" + label + "-" + str(row) + ".png")
        var opaque = 0
        var corners = 0
        var low = Vector2i(image.get_width(), image.get_height())
        var high = Vector2i(-1, -1)
        for y in range(image.get_height()):
            for x in range(image.get_width()):
                var alpha = image.get_pixel(x, y).a
                if alpha > 0.5: opaque += 1
                if alpha > 0.05:
                    low = Vector2i(mini(low.x, x), mini(low.y, y))
                    high = Vector2i(maxi(high.x, x), maxi(high.y, y))
        var span = float(maxi(high.x - low.x + 1, high.y - low.y + 1)) / float(image.get_width()) if high.x >= 0 else 0.0
        for corner in [Vector2i(0, 0), Vector2i(image.get_width() - 1, 0), Vector2i(0, image.get_height() - 1),
                Vector2i(image.get_width() - 1, image.get_height() - 1)]:
            corners += 1 if image.get_pixelv(corner).a > 0.1 else 0
        covered.append({"row": row, "edge": image.get_width(), "opaque": float(opaque) / float(image.get_width() * image.get_height()), "span": span, "corners": corners})
    facts["pictures_" + label] = {"atlas_rows": atlas_rows, "cells": covered}
func _process(delta):
    elapsed += delta
    var edit = root.find_child("IDC_PLANET_SEARCH_TEXT", true, false)
    var list = root.find_child("IDC_PLANET_SEARCH_RESULTS", true, false)
    if stage == 0 and elapsed > 14.0:
        tap(KEY_F, 0, true)
        stage = 1
    elif stage == 1 and elapsed > 15.0:
        assert(edit != null and edit.is_visible_in_tree(), "Ctrl+F did not open the XML dialog")
        assert(list.item_count > 1)
        facts["all_count"] = list.item_count
        facts["initial_selected"] = Array(list.get_selected_items())
        facts["icon_size"] = [list.fixed_icon_size.x, list.fixed_icon_size.y]
        facts["font_height"] = list.get_theme_font("font").get_height(list.get_theme_font_size("font_size"))
        facts["emblems"] = 0
        for child in list.get_children():
            if child is TextureRect and child.is_visible_in_tree() and child.texture != null:
                facts["emblems"] += 1
        facts["frame"] = [edit.get_parent().global_position.x, edit.get_parent().global_position.y,
            edit.get_parent().size.x, edit.get_parent().size.y]
        shot("all-results")
        stage = 10
    elif stage == 10 and elapsed > 18.0:
        # PS-08: by now the onscreen rows' models have decoded and uploaded (one per frame).
        pictures(list, "a")
        stage = 11
    elif stage == 11 and elapsed > 19.0:
        pictures(list, "b")
        shot("pictures")
        if extra_query.is_empty():
            stage = 13
        else:
            # PS-08 framing: rows whose model is a multi-body system (the caller's mod query).
            for character in extra_query: tap(character.to_upper().unicode_at(0), character.unicode_at(0))
            stage = 12
            elapsed = 16.0
    elif stage == 12 and elapsed > 19.0:
        pictures(list, "extra")
        var rect = Rect2i(list.get_global_rect())
        root.get_texture().get_image().get_region(rect).save_png(directory + "/list-extra.png")
        for i in range(32): tap(KEY_BACKSPACE)
        stage = 13
        elapsed = 18.5
    elif stage == 13 and elapsed > 19.0:
        var query = list.get_item_text(list.item_count - 1).to_lower()
        facts["query"] = query
        # Native character events, including any localized Unicode glyphs.
        for character in query:
            tap(character.to_upper().unicode_at(0), character.unicode_at(0))
        stage = 2
    elif stage == 2 and elapsed > 20.0:
        facts["typed"] = edit.text
        facts["filtered_count"] = list.item_count
        facts["filtered_names"] = []
        for row in range(list.item_count): facts["filtered_names"].append(list.get_item_text(row))
        shot("filtered-results")
        tap(KEY_DOWN)
        tap(KEY_UP)
        tap(KEY_ENTER)
        stage = 3
    elif stage == 3 and elapsed > 21.0:
        # PS-06: the pick moved the camera but the box stays open with its filter and text focus.
        facts["open_after_enter"] = edit.is_visible_in_tree()
        facts["focus_after_enter"] = edit.has_focus() or list.has_focus()
        facts["filter_after_enter"] = edit.text
        facts["tooltips"] = 0
        for row in range(list.item_count): facts["tooltips"] += 1 if list.is_item_tooltip_enabled(row) else 0
        var button = root.find_child("IDC_PLANET_SEARCH_CLOSE", true, false)
        click(button.get_global_rect().get_center())
        stage = 31
    elif stage == 31 and elapsed > 21.5:
        facts["closed_after_close"] = not edit.is_visible_in_tree()
        tap(KEY_F, 0, true)
        stage = 4
    elif stage == 4 and elapsed > 22.0:
        facts["reopened"] = edit.is_visible_in_tree() and edit.text.is_empty()
        tap(KEY_ESCAPE)
        stage = 5
    elif stage == 5 and elapsed > 23.0:
        facts["closed_after_escape"] = not edit.is_visible_in_tree()
        tap(KEY_F, 0, true)
        stage = 6
    elif stage == 6 and elapsed > 24.0:
        facts["clicked_name"] = list.get_item_text(0)
        click(list.get_global_transform() * list.get_item_rect(0, true).get_center())
        # The native click applies both selection and camera synchronously.
        # Freeze this pick frame before shutdown writes the report: private
        # desktops cannot supply a reliable OS cursor for later edge scrolling.
        current_scene.process_mode = Node.PROCESS_MODE_DISABLED
        facts["open_after_click"] = edit.is_visible_in_tree()
        var file = FileAccess.open(directory + "/input.json", FileAccess.WRITE)
        file.store_string(JSON.stringify(facts))
        file.close()
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''


COST_DRIVER = '''extends SceneTree
# PS-08 cost: the counters F3 reads (frame draw calls, measured render time), for the whole frame and per
# picture viewport, with the box open on an empty filter (every visible row pictured) and then closed.
var elapsed = 0.0
var stage = 0
var directory = %s
var viewport_size = Vector2i(%d, %d)
var samples = {"before": [], "open": [], "closed": []}
var facts = {}
var queries = ["sector", "nebula", "cluster"]
var last_usec = 0
var frame_ms = 0.0
var opening = []
func _initialize(): call_deferred("start")
func start():
    root.content_scale_factor = 1.0
    root.content_scale_aspect = Window.CONTENT_SCALE_ASPECT_KEEP
    root.content_scale_size = viewport_size
    root.content_scale_mode = Window.CONTENT_SCALE_MODE_VIEWPORT
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
    RenderingServer.viewport_set_measure_render_time(root.get_viewport_rid(), true)
func tap(code, control = false, unicode_value = 0):
    for down in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.ctrl_pressed = control
        event.unicode = unicode_value if down else 0
        event.pressed = down
        Input.parse_input_event(event)
func pictures():
    return [root.find_child("PlanetSearchPictures", true, false), root.find_child("PlanetSearchPictureScene", true, false)]
func sample(label):
    var row = {"draw_calls": Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME),
        "frame_gpu_ms": RenderingServer.viewport_get_measured_render_time_gpu(root.get_viewport_rid()),
        "frame_cpu_ms": RenderingServer.viewport_get_measured_render_time_cpu(root.get_viewport_rid()),
        "process_ms": Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0, "frame_ms": frame_ms,
        "pictures_gpu_ms": 0.0, "pictures_draw_calls": 0}
    for view in pictures():
        if view == null: continue
        row["pictures_gpu_ms"] += RenderingServer.viewport_get_measured_render_time_gpu(view.get_viewport_rid())
        row["pictures_draw_calls"] += RenderingServer.viewport_get_render_info(view.get_viewport_rid(),
            RenderingServer.VIEWPORT_RENDER_INFO_TYPE_VISIBLE, RenderingServer.VIEWPORT_RENDER_INFO_DRAW_CALLS_IN_FRAME)
    samples[label].append(row)
func _process(delta):
    elapsed += delta
    # Wall time between frames (the Performance monitors refresh about once a second and hide spikes).
    var now = Time.get_ticks_usec()
    frame_ms = (now - last_usec) / 1000.0 if last_usec else 0.0
    last_usec = now
    if stage == 1: opening.append(frame_ms)
    var list = root.find_child("IDC_PLANET_SEARCH_RESULTS", true, false)
    if stage == 0 and elapsed > 14.0:
        # Baseline: the same scene before the search was ever opened.
        sample("before")
        if samples["before"].size() >= 180:
            tap(KEY_F, true)
            stage = 1
            elapsed = 14.0
    elif stage == 1 and elapsed > 18.0:
        for view in pictures():
            if view != null: RenderingServer.viewport_set_measure_render_time(view.get_viewport_rid(), true)
        facts["rows"] = list.item_count
        root.get_texture().get_image().save_png(directory + "/open.png")
        stage = 2
    elif stage == 2:
        sample("open")
        if samples["open"].size() >= 180:
            stage = 20
            elapsed = 50.0
    elif stage >= 20 and stage < 20 + queries.size() * 2:
        # Eye check: rows whose names hold each query (sector/cluster art next to ordinary planets).
        var step = stage - 20
        var query = queries[step / 2]
        if step %% 2 == 0:
            for i in range(24): tap(KEY_BACKSPACE)
            for character in query: tap(character.to_upper().unicode_at(0), false, character.unicode_at(0))
            stage += 1
            elapsed = 50.0
        elif elapsed > 53.0:
            var rect = Rect2i(list.get_global_rect())
            root.get_texture().get_image().get_region(rect).save_png(directory + "/list-" + query + ".png")
            var names = []
            for row in range(list.item_count):
                names.append(list.get_item_text(row))
                var icon = list.get_item_icon(row)
                if icon is AtlasTexture and list.get_item_rect(row, true).position.y < list.size.y:
                    icon.get_image().save_png(directory + "/cell-" + query + "-" + str(row) + ".png")
            facts["query_" + query] = names
            stage += 1
            elapsed = 50.0
    elif stage == 20 + queries.size() * 2:
        tap(KEY_ESCAPE)
        stage = 3
        elapsed = 100.0
    elif stage == 3 and elapsed > 101.0:
        sample("closed")
        if samples["closed"].size() >= 180:
            var modes = []
            for view in pictures(): modes.append(view.render_target_update_mode if view else -1)
            facts["update_mode_closed"] = modes
            facts["samples"] = samples
            facts["opening_frame_ms"] = opening
            var file = FileAccess.open(directory + "/cost.json", FileAccess.WRITE)
            file.store_string(JSON.stringify(facts))
            file.close()
            current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
            quit()
    return false
'''


EYE_DRIVER = '''extends SceneTree
# PS-08 eye check: for each query, the search row (list crop and its cells) next to the map after picking
# that row (the camera centres on the planet; the map crop is the screen centre).
var elapsed = 0.0
var stage = 0
var directory = %s
var viewport_size = Vector2i(%d, %d)
var queries = %s
var index = 0
var facts = {"queries": []}
func _initialize(): call_deferred("start")
func start():
    root.content_scale_factor = 1.0
    root.content_scale_aspect = Window.CONTENT_SCALE_ASPECT_KEEP
    root.content_scale_size = viewport_size
    root.content_scale_mode = Window.CONTENT_SCALE_MODE_VIEWPORT
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func tap(code, control = false, unicode_value = 0):
    for down in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.ctrl_pressed = control
        event.unicode = unicode_value if down else 0
        event.pressed = down
        Input.parse_input_event(event)
func _process(delta):
    elapsed += delta
    var list = root.find_child("IDC_PLANET_SEARCH_RESULTS", true, false)
    if stage == 0 and elapsed > 14.0:
        if index >= queries.size():
            var file = FileAccess.open(directory + "/eye.json", FileAccess.WRITE)
            file.store_string(JSON.stringify(facts))
            file.close()
            current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
            quit()
            return false
        tap(KEY_F, true)
        stage = 1
        elapsed = 14.0
    elif stage == 1 and elapsed > 15.0:
        for character in queries[index]: tap(character.to_upper().unicode_at(0), false, character.unicode_at(0))
        stage = 2
        elapsed = 14.0
    elif stage == 2 and elapsed > 18.0:
        var label = "q" + str(index)
        var row = {"query": queries[index], "names": [], "cells": 0}
        if list != null and list.item_count > 0:
            root.get_texture().get_image().get_region(Rect2i(list.get_global_rect())).save_png(directory + "/" + label + "-row.png")
            for item in range(min(list.item_count, 4)):
                row["names"].append(list.get_item_text(item))
                var icon = list.get_item_icon(item)
                if icon is AtlasTexture:
                    icon.get_image().save_png(directory + "/" + label + "-cell-" + str(item) + ".png")
                    row["cells"] += 1
            tap(KEY_ENTER)
        else:
            tap(KEY_ESCAPE)
        facts["queries"].append(row)
        stage = 3
        elapsed = 14.0
    elif stage == 3 and elapsed > 17.0:
        var image = root.get_texture().get_image()
        var side = int(min(viewport_size.x, viewport_size.y) * 0.4)
        image.get_region(Rect2i(viewport_size.x / 2 - side / 2, viewport_size.y / 2 - side / 2, side, side)).save_png(directory + "/q" + str(index) + "-map.png")
        if list != null and list.is_visible_in_tree(): tap(KEY_ESCAPE)
        index += 1
        stage = 0
        elapsed = 14.0
    return false
'''


ROWS_DRIVER = '''extends SceneTree
# PS-08: every visible discovered row gets its live picture within a bounded number of frames, and no cell is
# blank or black. Records the first frame each onscreen row showed an atlas cell, then each cell's coverage.
var elapsed = 0.0
var stage = 0
var directory = %s
var viewport_size = Vector2i(%d, %d)
var start_name = %s
var facts = {"rows": []}
var opened = 0
var first = {}
func _initialize(): call_deferred("start")
func start():
    root.content_scale_factor = 1.0
    root.content_scale_aspect = Window.CONTENT_SCALE_ASPECT_KEEP
    root.content_scale_size = viewport_size
    root.content_scale_mode = Window.CONTENT_SCALE_MODE_VIEWPORT
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func tap(code, control = false):
    for down in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.ctrl_pressed = control
        event.pressed = down
        Input.parse_input_event(event)
func onscreen(list):
    # Item rectangles are in unscrolled content space.
    var scroll = list.get_v_scroll_bar().value
    var rows = []
    for row in range(list.item_count):
        var rect = list.get_item_rect(row, true)
        if rect.position.y + rect.size.y - scroll <= 0: continue
        if rect.position.y - scroll >= list.size.y: break
        rows.append(row)
    return rows
func _process(delta):
    elapsed += delta
    var list = root.find_child("IDC_PLANET_SEARCH_RESULTS", true, false)
    if stage == 0 and elapsed > 14.0:
        tap(KEY_F, true)
        stage = 10
    elif stage == 10 and elapsed > 14.5:
        # Scroll so the first row at or after start_name is the top row (an empty name keeps the top).
        var top = 0
        for row in range(list.item_count):
            if list.get_item_text(row).to_lower() >= start_name:
                top = row
                break
        facts["top"] = top
        list.get_v_scroll_bar().value = list.get_item_rect(top, true).position.y
        opened = Engine.get_process_frames()
        stage = 1
    elif stage == 1:
        var frames = Engine.get_process_frames() - opened
        if list != null and list.is_visible_in_tree():
            for row in onscreen(list):
                if not first.has(row) and list.get_item_icon(row) is AtlasTexture: first[row] = frames
        if elapsed > 30.0:
            facts["frames"] = frames
            root.get_texture().get_image().get_region(Rect2i(list.get_global_rect())).save_png(directory + "/rows.png")
            for row in onscreen(list):
                var icon = list.get_item_icon(row)
                var entry = {"row": row, "name": list.get_item_text(row), "atlas": icon is AtlasTexture,
                    "first_frame": first.get(row, -1)}
                if icon is AtlasTexture:
                    var image = icon.get_image()
                    image.save_png(directory + "/cell-" + str(row) + ".png")
                    var drawn = 0
                    var light = 0.0
                    var peak = 0.0
                    var low = Vector2i(image.get_width(), image.get_height())
                    var high = Vector2i(-1, -1)
                    for y in range(image.get_height()):
                        for x in range(image.get_width()):
                            var pixel = image.get_pixel(x, y)
                            peak = max(peak, pixel.a)
                            if pixel.a > 0.05:
                                drawn += 1
                                light += pixel.get_luminance()
                                low = Vector2i(mini(low.x, x), mini(low.y, y))
                                high = Vector2i(maxi(high.x, x), maxi(high.y, y))
                    entry["edge"] = image.get_width()
                    entry["drawn"] = float(drawn) / float(image.get_width() * image.get_height())
                    entry["luminance"] = light / float(max(drawn, 1))
                    entry["peak_alpha"] = peak
                    entry["span"] = float(maxi(high.x - low.x + 1, high.y - low.y + 1)) / float(image.get_width()) if high.x >= 0 else 0.0
                facts["rows"].append(entry)
            var file = FileAccess.open(directory + "/rows.json", FileAccess.WRITE)
            file.store_string(JSON.stringify(facts))
            file.close()
            current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
            quit()
            stage = 2
    return false
'''


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires the GPU viewer and installed data")
class GalacticPlanetSearchRuntime(unittest.TestCase):
    def check_search(self, width, height, mod_root=None, campaign="Sandbox_Equal_Footing_Underworld", extra_query=""):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-planet-search-"))
        layer = directory / "layer"
        (layer / "Data/XML").mkdir(parents=True)
        shutil.copyfile(support.ROOT / "tools/revised_planet_search/RevisedPlanetSearch.xml",
                        layer / "Data/XML/RevisedPlanetSearch.xml")
        script = directory / "input.gd"
        script.write_text(DRIVER % (json.dumps(directory.as_posix()), width, height, json.dumps(extra_query)), encoding="utf-8")
        report = directory / "report.json"
        settings = directory / "settings.txt"
        settings.write_text("eawr-settings 1\nrevised_layer_enabled=1\nrevised_layer_path=" + layer.as_posix() + "\n", encoding="utf-8")
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate",
            "--resolution", f"{width}x{height}", "--path", str(support.ROOT / "apps/viewer/project"),
            "--script", str(script), "--", "--eawr-galactic", campaign,
            "--eawr-game-root", support.GAME_ROOT, "--eawr-mod-root", str(mod_root or layer),
            "--eawr-settings-file", str(settings),
            "--eawr-report", str(report), "--eawr-galactic-paused", "yes",
            "--eawr-galactic-dismiss-story", "yes", "--eawr-lighting", "sh",
            "--eawr-environment", "map", "--eawr-shadows", "on"],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        log = result.stdout + result.stderr
        (directory / "viewer.log").write_text(log, encoding="utf-8")
        self.assertEqual(result.returncode, 0, log)
        self.assertTrue((directory / "input.json").is_file(), log)
        facts = json.loads((directory / "input.json").read_text(encoding="utf-8"))
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["camera"]["width"], width)
        self.assertEqual(data["camera"]["height"], height)
        self.assertEqual(facts["typed"], facts["query"], facts)
        self.assertLess(facts["filtered_count"], facts["all_count"], facts)
        self.assertGreater(facts["filtered_count"], 0, facts)
        for name in facts["filtered_names"]:
            self.assertIn(facts["query"], name.lower())
        for key in ("open_after_enter", "focus_after_enter", "closed_after_close", "reopened", "closed_after_escape",
                    "open_after_click"):
            self.assertTrue(facts[key], (key, facts))
        self.assertEqual(facts["filter_after_enter"], facts["typed"], "a pick keeps the filter")
        self.assertEqual(facts["tooltips"], 0, "a row tooltip repeats the name beside the cursor")
        # Stock English text table: TEXT_BUTTON_CLOSE, not the Cancel text.
        self.assertEqual(data["planet_search"]["close_text"].strip().lower(), "close", data["planet_search"])
        self.assertEqual(facts["icon_size"][0], facts["icon_size"][1])
        self.assertAlmostEqual(facts["icon_size"][1], facts["font_height"], delta=1.0)
        self.assertGreater(facts["emblems"], 0, "known owners must have visible faction emblems")
        x, y, w, h = facts["frame"]
        self.assertGreater(x, width / 2)
        self.assertLessEqual(x + w, width)
        self.assertGreaterEqual(y, 0)
        self.assertLess(y + h, height * 0.8, "search overlaps the radar/command-bar band")
        # PS-08: live pictures on the onscreen rows, framed inside their cells (the cost probe covers the closed box).
        pictures = facts["pictures_b"]
        self.assertGreaterEqual(pictures["atlas_rows"], min(4, facts["all_count"]), facts)
        for cell in pictures["cells"]:
            self.assertGreater(cell["opaque"], 0.05, ("the planet body is missing from its cell", cell))
            self.assertLess(cell["opaque"], 0.95, ("the cell is squared off rather than a framed planet", cell))
            self.assertEqual(cell["corners"], 0, ("a cell corner is covered: clipping or an opaque square", cell))
            self.assertGreaterEqual(cell["edge"], facts["icon_size"][1], cell)
        if extra_query:
            # A system row draws what the map draws there (a mod's sector spiral is a faint additive disc
            # with small planets): its drawn extent fills the cell without clipping.
            rows = facts["pictures_extra"]
            self.assertGreaterEqual(rows["atlas_rows"], 1, facts)
            for cell in rows["cells"]:
                self.assertGreaterEqual(cell["span"], 0.6, ("a system row's drawn extent does not fill its cell", cell))
                self.assertEqual(cell["corners"], 0, ("a system row spills or clips", cell))
        live = data["planet_search_pictures"]
        # Only the galaxy map's own material-plan gaps may fail: the map draws nothing for those objects either (#7552).
        self.assertEqual([row for row in live["failures"] if not row["cause"].startswith("no legacy selector")], [], live)
        # Planets that share a model share one upload.
        self.assertGreaterEqual(live["uploads"], len({row["model"] for row in live["shown"]}), live)
        self.assertGreaterEqual(len(live["shown"]), 1, live)
        self.assertLessEqual(live["resident"], live["resident_cap"], live)
        search = data["planet_search"]
        self.assertEqual(search["opens"], 3)
        self.assertEqual(search["picks"], 2)
        self.assertEqual(data["map_input"]["selected"], search["last_pick"])
        planet = next(row for row in data["planets"] if row["id"] == search["last_pick"])
        self.assertEqual(planet.get("label") or planet["name"], facts["clicked_name"])
        # The normal map camera-to-planet path maps source XY to render X,-Z.
        self.assertAlmostEqual(data["camera"]["target"][0], planet["position"][0], delta=0.1)
        self.assertAlmostEqual(data["camera"]["target"][2], -planet["position"][1], delta=0.1)
        self.assertTrue(data["paused"], "search must remain usable while paused")
        self.assertEqual(data["live_session"]["submitted_orders"], 1, "the search submitted a simulation order")
        self.assertFalse(data["cinematic_tour"]["active"])
        if mod_root:
            self.assertGreaterEqual(len(data["planets"]), 100, "the mod proof must load a large galaxy")

    def large_campaign(self, chain, minimum=100):
        from tools.inventory.corpus import Corpus

        roots = [chain] if pathlib.Path(chain).is_dir() else chain.split(";")
        corpus = Corpus(support.GAME_ROOT, mod_root=roots)
        registry = corpus.read_effective("remake", "data/xml/campaignfiles.xml")
        self.assertIsNotNone(registry, "the loaded mod chain needs a campaign registry")
        campaigns = []
        for entry in ET.fromstring(registry.data):
            name = (entry.text or "").strip().replace("\\", "/")
            if not name:
                continue
            if not name.lower().startswith("data/"):
                name = "data/xml/" + name
            source = corpus.read_effective("remake", name)
            if source is None:
                continue
            for campaign in ET.fromstring(source.data):
                count = sum(len((node.text or "").replace(",", " ").split()) for node in campaign.findall("Locations"))
                if count >= minimum and campaign.findtext("Starting_Active_Player"):
                    campaigns.append((count, campaign.attrib["Name"]))
        self.assertTrue(campaigns, f"the mod chain must supply a {minimum}+ planet campaign")
        # Pick from the actual registry, without a stock/mod campaign-name pin: the largest is the worst case.
        _, campaign = max(campaigns)
        return campaign

    def measure_cost(self, campaign, mod_root, pictures=True, width=1920, height=1080):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-planet-pictures-cost-"))
        layer = directory / "layer"
        (layer / "Data/XML").mkdir(parents=True)
        definition = (support.ROOT / "tools/revised_planet_search/RevisedPlanetSearch.xml").read_text(encoding="utf-8")
        if not pictures:
            # The same dialog without the picture column (the layout's own switch): the A/B control.
            self.assertIn("<Planet_Picture>True</Planet_Picture>", definition)
            definition = definition.replace("<Planet_Picture>True</Planet_Picture>", "<Planet_Picture>False</Planet_Picture>")
        (layer / "Data/XML/RevisedPlanetSearch.xml").write_text(definition, encoding="utf-8")
        script = directory / "cost.gd"
        script.write_text(COST_DRIVER % (json.dumps(directory.as_posix()), width, height), encoding="utf-8")
        settings = directory / "settings.txt"
        settings.write_text("eawr-settings 1\nrevised_layer_enabled=1\nrevised_layer_path=" + layer.as_posix() + "\n", encoding="utf-8")
        report = directory / "report.json"
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate", "--resolution", f"{width}x{height}",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(script), "--", "--eawr-galactic", campaign,
            "--eawr-game-root", support.GAME_ROOT, "--eawr-mod-root", str(mod_root), "--eawr-settings-file", str(settings),
            "--eawr-report", str(report), "--eawr-galactic-paused", "yes", "--eawr-galactic-dismiss-story", "yes",
            "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
            cwd=support.ROOT, capture_output=True, text=True, timeout=400, check=False)
        log = result.stdout + result.stderr
        (directory / "viewer.log").write_text(log, encoding="utf-8")
        self.assertEqual(result.returncode, 0, log)
        facts = json.loads((directory / "cost.json").read_text(encoding="utf-8"))
        data = json.loads(report.read_text(encoding="utf-8"))

        def median(rows, key):
            values = sorted(row[key] for row in rows)
            return values[len(values) // 2]
        summary = {state: {key: median(facts["samples"][state], key) for key in facts["samples"][state][0]}
                   for state in ("before", "open", "closed")}
        opening = facts["opening_frame_ms"]
        summary["opening"] = {"frames": len(opening), "max_frame_ms": max(opening), "median_frame_ms": sorted(opening)[len(opening) // 2]}
        summary["rows"] = facts["rows"]
        summary["planets"] = len(data["planets"])
        summary["pictures"] = data["planet_search_pictures"]
        summary["opening_hitches_ms"] = [round(value, 1) for value in opening if value > 50.0]
        summary["open_hitches_ms"] = [round(row["frame_ms"], 1) for row in facts["samples"]["open"] if row["frame_ms"] > 50.0]
        (directory / "cost-summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
        print("PS-08 cost", "pictures" if pictures else "no pictures", json.dumps(summary))
        if not pictures:
            self.assertEqual(data["planet_search_pictures"]["requests"], 0, "no picture column, no picture work")
            return summary
        # Closed: both picture viewports are disabled and the frame hook is detached (Godot keeps a
        # disabled viewport's last render info, so the per-viewport counters mean something only while
        # open). The paused scene is static, so the closed frame draws exactly what it drew before the
        # search was ever opened.
        self.assertEqual(facts["update_mode_closed"], [0, 0], facts["update_mode_closed"])
        self.assertEqual(summary["closed"]["draw_calls"], summary["before"]["draw_calls"], summary)
        self.assertFalse(summary["pictures"]["rendering"], summary)
        self.assertGreater(summary["open"]["pictures_draw_calls"], 0, summary)
        return summary

    @unittest.skipUnless(os.environ.get("EAWR_REMAKE_MOD_ROOT"), "requires a mod host (--NeedsMod)")
    def test_picture_cost_on_a_large_mod_campaign(self):
        chain = os.environ["EAWR_REMAKE_MOD_ROOT"]
        campaign = self.large_campaign(chain)
        summary = self.measure_cost(campaign, chain)
        control = self.measure_cost(campaign, chain, pictures=False)
        self.assertGreaterEqual(summary["planets"], 100, summary)
        print("PS-08 cost A/B", json.dumps({"pictures": summary, "no_pictures": control}))

    def eye_check(self, queries, mod_root=None, campaign="Sandbox_Equal_Footing_Underworld", width=1920, height=1080):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-planet-pictures-eye-"))
        layer = directory / "layer"
        (layer / "Data/XML").mkdir(parents=True)
        shutil.copyfile(support.ROOT / "tools/revised_planet_search/RevisedPlanetSearch.xml", layer / "Data/XML/RevisedPlanetSearch.xml")
        script = directory / "eye.gd"
        script.write_text(EYE_DRIVER % (json.dumps(directory.as_posix()), width, height, json.dumps(queries)), encoding="utf-8")
        settings = directory / "settings.txt"
        settings.write_text("eawr-settings 1\nrevised_layer_enabled=1\nrevised_layer_path=" + layer.as_posix() + "\n", encoding="utf-8")
        report = directory / "report.json"
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate", "--resolution", f"{width}x{height}",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(script), "--", "--eawr-galactic", campaign,
            "--eawr-game-root", support.GAME_ROOT, "--eawr-mod-root", str(mod_root or layer), "--eawr-settings-file", str(settings),
            "--eawr-report", str(report), "--eawr-galactic-paused", "yes", "--eawr-galactic-dismiss-story", "yes",
            "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
            cwd=support.ROOT, capture_output=True, text=True, timeout=400, check=False)
        log = result.stdout + result.stderr
        (directory / "viewer.log").write_text(log, encoding="utf-8")
        self.assertEqual(result.returncode, 0, log)
        facts = json.loads((directory / "eye.json").read_text(encoding="utf-8"))
        data = json.loads(report.read_text(encoding="utf-8"))
        print("PS-08 eye", json.dumps({"queries": facts["queries"], "pictures": data["planet_search_pictures"]}))
        for row in facts["queries"]:
            self.assertTrue(row["names"], ("the eye-check query found no row", row))
        return facts

    def every_row_pictured(self, mod_root, start_name="", width=1920, height=1080):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-planet-rows-"))
        layer = directory / "layer"
        (layer / "Data/XML").mkdir(parents=True)
        shutil.copyfile(support.ROOT / "tools/revised_planet_search/RevisedPlanetSearch.xml", layer / "Data/XML/RevisedPlanetSearch.xml")
        script = directory / "rows.gd"
        script.write_text(ROWS_DRIVER % (json.dumps(directory.as_posix()), width, height, json.dumps(start_name)), encoding="utf-8")
        settings = directory / "settings.txt"
        settings.write_text("eawr-settings 1\nrevised_layer_enabled=1\nrevised_layer_path=" + layer.as_posix() + "\n", encoding="utf-8")
        report = directory / "report.json"
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate", "--resolution", f"{width}x{height}",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(script), "--", "--eawr-galactic",
            self.large_campaign(str(mod_root), minimum=1), "--eawr-game-root", support.GAME_ROOT, "--eawr-mod-root", str(mod_root),
            "--eawr-settings-file", str(settings), "--eawr-report", str(report), "--eawr-galactic-paused", "yes",
            "--eawr-galactic-dismiss-story", "yes", "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
            cwd=support.ROOT, capture_output=True, text=True, timeout=400, check=False)
        log = result.stdout + result.stderr
        (directory / "viewer.log").write_text(log, encoding="utf-8")
        self.assertEqual(result.returncode, 0, log)
        facts = json.loads((directory / "rows.json").read_text(encoding="utf-8"))
        live = json.loads(report.read_text(encoding="utf-8"))["planet_search_pictures"]
        print("PS-08 rows", json.dumps({"rows": facts["rows"], "frames": facts["frames"], "pictures": live}))
        self.assertGreater(len(facts["rows"]), 4, facts)
        for row in facts["rows"]:
            self.assertTrue(row["atlas"], ("a visible row never got its picture", row, live["failures"]))
            self.assertLessEqual(row["first_frame"], 240, ("a visible row waited too long for its picture", row))
            self.assertGreaterEqual(row["span"], 0.5, ("a row's picture is blank or a dot", row))
            self.assertGreater(row["luminance"], 0.04, ("a row's picture is black", row))
        return facts

    @unittest.skipUnless(os.environ.get("EAWR_REMAKE_MOD_ROOT"), "requires a mod host (--NeedsMod)")
    def test_rows_all_pictured_on_other_mods(self):
        # Test input: Republic at War and BFC from the same workshop folder as the large-campaign mod
        # (the owner's net-3 list, Bonadan..Christophsis, is one of the two).
        workshop = pathlib.Path(os.environ["EAWR_REMAKE_MOD_ROOT"]).parent
        mods = [workshop / item for item in ("1129810972", "2712013423") if (workshop / item).is_dir()]
        self.assertTrue(mods, "the mod host lacks both test mods")
        for mod in mods:
            with self.subTest(mod=mod.name):
                # The owner's screenshot page: Bespin, Bestine, Bonadan .. Christophsis.
                self.every_row_pictured(mod, start_name="bespin")

    def test_eye_check_stock_rows(self):
        # Test input: the first row of the full list and a gas giant.
        self.eye_check(["", "bespin"])

    @unittest.skipUnless(os.environ.get("EAWR_REMAKE_MOD_ROOT"), "requires a mod host (--NeedsMod)")
    def test_eye_check_mod_system_rows(self):
        # Test input for the large-campaign mod (Remake 4.0): its multi-body sectors, clusters, nebulae and systems.
        chain = os.environ["EAWR_REMAKE_MOD_ROOT"]
        self.eye_check(["sector", "cluster", "nebula", "kessel"], mod_root=chain, campaign=self.large_campaign(chain))

    def test_search_at_1080p(self):
        self.check_search(1920, 1080)

    def test_search_at_ultrawide(self):
        self.check_search(3440, 1440)

    @unittest.skipUnless(os.environ.get("EAWR_REMAKE_MOD_ROOT"), "requires a mod host (--NeedsMod)")
    def test_search_on_a_large_mod_chain(self):
        chain = os.environ["EAWR_REMAKE_MOD_ROOT"]
        campaign = self.large_campaign(chain)
        # Test input, not runtime data: the large-campaign mod (Remake 4.0) names its multi-body system
        # models "... Sector"; those rows must show their largest body filling the cell.
        self.check_search(1920, 1080, mod_root=chain, campaign=campaign, extra_query="sector")


if __name__ == "__main__":
    unittest.main()
