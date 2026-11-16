extends SceneTree

var output := ""
var label := ""
var scene
var phase := 0
var elapsed := 0.0
var beat := 0.0
var tick := 0
var first_tick := -1
var first_frame_us := 0
var started_us := 0
var load_requested_us := 0
var finishing := false
var last_seen_frame := -1
var gate_error := ""
var progress_tick := 0
var begun := false
var ready_tick := -1
var window_ticks := 1100
var end_tick := -1
var save_name := ""

func _initialize():
    started_us = Time.get_ticks_usec()
    for arg in OS.get_cmdline_user_args():
        if arg.begins_with("--bench-output="): output = arg.substr(15)
        if arg.begins_with("--bench-label="): label = arg.substr(14)
        if arg.begins_with("--bench-ticks="): window_ticks = arg.substr(14).to_int()
        if arg.begins_with("--bench-save="): save_name = arg.substr(13)
    scene = load("res://main.tscn").instantiate()
    root.add_child(scene)
    current_scene = scene

func click(node):
    var point = node.get_global_rect().get_center()
    for pressed in [true, false]:
        var event = InputEventMouseButton.new()
        event.position = point
        event.global_position = point
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = pressed
        Input.parse_input_event(event)

func latest_tick():
    var file = FileAccess.open(output.path_join(label + ".csv"), FileAccess.READ)
    if file == null: return -1
    var columns = Array(file.get_line().split(","))
    var tick_idx = columns.find("tick")
    var growth_idx = columns.find("slots_grown")
    var frame_idx = columns.find("frame")
    var size = file.get_length()
    if tick_idx < 0 or size == 0: return -1
    file.seek(maxi(0, size - 65536))
    var lines = file.get_buffer(size - file.get_position()).get_string_from_utf8().split("\n")
    if lines.size() < 3: return -1
    var result := -1
    for index in range(1, lines.size() - 1):
        var fields = lines[index].split(",")
        if fields.size() <= maxi(tick_idx, maxi(frame_idx, growth_idx)): continue
        if not fields[frame_idx].is_valid_int(): continue
        var frame = fields[frame_idx].to_int()
        result = fields[tick_idx].to_int()
        if frame < last_seen_frame: last_seen_frame = -1
        if frame <= last_seen_frame: continue
        last_seen_frame = frame
        if ready_tick < 0: ready_tick = result
        if result > ready_tick and label.begins_with("head") and growth_idx >= 0 and fields[growth_idx].to_int() > 0:
            gate_error = "HEAD zero-growth gate failed at tick " + fields[tick_idx]
    return result

func _process(delta):
    if finishing: return false
    elapsed += delta
    beat += delta
    if elapsed > 1800:
        finish("load/battle timeout")
        return false
    if phase >= 1 and phase < 3 and int(elapsed) % 5 == 0:
        var report = FileAccess.open(output.path_join(label + ".json"), FileAccess.READ)
        if report != null:
            var text = report.get_as_text()
            if text.contains('"status": "failed"'):
                finish("load failed: " + text.substr(text.find('"failure"'), 300))
                return false
    if beat < 0.5: return false
    beat = 0.0
    if phase == 0:
        var button = scene.find_child("IDC_LOAD_GAME_MAIN", true, false)
        if button != null and button.is_visible_in_tree() and not button.disabled:
            click(button)
            phase = 1
    elif phase == 1:
        var list = scene.find_child("IDC_SAVE_GAME_LIST", true, false)
        var pick := -1
        if list != null and list.is_visible_in_tree():
            if save_name.is_empty():
                if list.item_count == 1: pick = 0
            else:
                for i in range(list.item_count):
                    if list.get_item_text(i) == save_name: pick = i
        if pick >= 0:
            print("EAWR preload bench selecting save: ", list.get_item_text(pick))
            load_requested_us = Time.get_ticks_usec()
            list.emit_signal("item_activated", pick)
            phase = 2
    elif phase == 4:
        var leave = scene.find_child("IDC_GAME_OPTIONS_QUIT", true, false)
        if leave != null and leave.is_visible_in_tree():
            click(leave)
            phase = 5
    elif phase == 5:
        var yes = scene.find_child("IDC_DIALOG_BOX_BUTTON_LEFT", true, false)
        if yes != null and yes.is_visible_in_tree():
            click(yes)
            phase = 6
    elif phase == 6:
        var main = scene.find_child("IDC_LOAD_GAME_MAIN", true, false)
        if main != null and main.is_visible_in_tree():
            var report_path = output.path_join(label + ".json")
            var report = FileAccess.open(report_path, FileAccess.READ)
            if report == null or not report.get_as_text().contains('"launch_slots"'):
                finish("battle report missing before front-end exit")
            else:
                report.close()
                DirAccess.copy_absolute(report_path, output.path_join(label + ".battle.json"))
                finish(gate_error)
    elif phase == 2:
        var begin = scene.find_child("IDC_BEGIN_BUTTON", true, false)
        if begin != null and begin.is_visible_in_tree() and not begin.disabled:
            if not begun:
                var restore_tick = latest_tick()
                if restore_tick >= 0: ready_tick = restore_tick
            click(begin)
            begun = true
        elif begun:
            var advanced = latest_tick()
            if advanced >= 0 and ready_tick >= 0 and advanced > ready_tick:
                tick = advanced
                first_tick = advanced
                first_frame_us = Time.get_ticks_usec()
                print("EAWR preload bench resumed battle tick ", advanced)
                end_tick = advanced + window_ticks
                phase = 3
    else:
        var observed = latest_tick()
        if observed >= 0:
            tick = observed
            if tick - progress_tick >= 1000:
                progress_tick = tick
                print("EAWR preload bench ", label, " tick ", tick)
            if tick >= end_tick:
                root.get_texture().get_image().save_png(output.path_join(label + ".end.png"))
                finish("")
    return false

func finish(error):
    if finishing: return
    finishing = true
    var receipt = FileAccess.open(output.path_join(label + ".observer-end.json"), FileAccess.WRITE)
    receipt.store_string(JSON.stringify({"error": error, "phase": phase, "target_tick": end_tick,
        "held_tick": tick, "first_tick": first_tick, "first_frame_us": first_frame_us,
        "started_us": started_us, "load_requested_us": load_requested_us,
        "load_detection_uncertainty_ms": 500, "adapter": RenderingServer.get_video_adapter_name()}))
    receipt.close()
    root.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit(0 if error == "" else 2)
