"""Drive the production dialog and read its popup, scroll bar and framebuffer."""

SCRIPT = r'''
extends SceneTree

func descendants(node, kind):
    var result = []
    if node.is_class(kind): result.append(node)
    for child in node.get_children(true): result.append_array(descendants(child, kind))
    return result

func settle():
    for frame in range(8): await process_frame
    await RenderingServer.frame_post_draw

func shot(path):
    root.get_texture().get_image().save_png(path)

func _initialize():
    call_deferred("run")

func run():
    var output = ""
    var resolution = Vector2i(1280, 720)
    var resource_height = 120.0
    var control_id = "IDC_PLAYER_FACTION_0"
    for arg in OS.get_cmdline_user_args():
        if arg.begins_with("--popup-output="): output = arg.get_slice("=", 1)
        if arg.begins_with("--popup-height="): resource_height = float(arg.get_slice("=", 1))
        if arg.begins_with("--popup-control="): control_id = arg.get_slice("=", 1)
        if arg.begins_with("--popup-resolution="):
            var parts = arg.get_slice("=", 1).split("x")
            resolution = Vector2i(int(parts[0]), int(parts[1]))
    root.content_scale_size = resolution
    root.content_scale_aspect = Window.CONTENT_SCALE_ASPECT_KEEP
    root.content_scale_mode = Window.CONTENT_SCALE_MODE_VIEWPORT
    var scene = load("res://main.tscn").instantiate()
    root.add_child(scene)
    current_scene = scene
    var combo = null
    for frame in range(1200):
        await process_frame
        combo = root.find_child(control_id, true, false)
        if combo != null: break
    if combo == null:
        print("POPUP ERROR no combo")
        quit(2)
        return
    await settle()
    combo.show()
    combo.disabled = false
    var original = combo.global_position
    var scale = combo.get_global_transform_with_canvas().get_scale().y
    var budget = (resource_height * 720.0 / 768.0 - combo.size.y) * scale
    var skin = combo.get_theme_icon("Small_Frame_Background").get_image()
    var authored_alpha = skin.get_pixel(skin.get_width()/2, skin.get_height()/2).a
    var reports = []
    for count in [3, 10, 25]:
        for edge in [false, true]:
            combo.clear()
            for i in range(count): combo.add_item("Row %02d" % i)
            combo.select(0)
            combo.global_position = Vector2(resolution) - combo.size * scale - Vector2(4,4) if edge else original
            await settle()
            var stem = output.path_join("%d-%s" % [count, "edge" if edge else "normal"])
            combo.show_popup()
            await settle()
            shot(stem + "-top.png")
            var popup = combo.get_popup()
            var rect = Rect2(popup.position, popup.size)
            var panel = popup.get_theme_stylebox("panel")
            var bars = descendants(popup, "VScrollBar")
            var bar = bars[0]
            var first_value = bar.value
            var frame = panel.get_minimum_size().y
            var stride = ceil(popup.get_theme_font("font").get_height(popup.get_theme_font_size("font_size"))) + popup.get_theme_constant("v_separation")
            var capacity = floor((popup.size.y / scale - frame + 0.01) / stride)
            for i in range(count): popup.set_item_text(i, "Text %02d" % i)
            await settle()
            shot(stem + "-changed.png")
            popup.set_focused_item(count - 1)
            popup.scroll_to_item(count - 1)
            await settle()
            shot(stem + "-last.png")
            reports.append({"control": control_id, "count": count, "edge": edge, "stem": stem.get_file(),
                "rect": [rect.position.x, rect.position.y, rect.size.x, rect.size.y],
                "after_rect": [popup.position.x,popup.position.y,popup.size.x,popup.size.y],
                "budget": budget, "scale": scale, "capacity": capacity,
                "frame": frame, "stride": stride, "scroll": bar.is_visible_in_tree(),
                "first_value": first_value, "last_value": bar.value, "page": bar.page,
                "focused": popup.get_focused_item(), "authored_alpha": authored_alpha,
                "alpha": panel.bg_color.a, "viewport": [resolution.x, resolution.y]})
            popup.hide()
            await settle()
    FileAccess.open(output.path_join("popup.json"),FileAccess.WRITE).store_string(JSON.stringify(reports,"  "))
    print("POPUP CONTRACT complete: ", reports.size())
    quit()
'''

COMMON = SCRIPT[:SCRIPT.index("func run():")]

USERS_SCRIPT = COMMON + r'''
func run():
    var output = ""
    var resolution = Vector2i(1280,720)
    for arg in OS.get_cmdline_user_args():
        if arg.begins_with("--popup-output="): output = arg.get_slice("=",1)
        if arg.begins_with("--popup-resolution="):
            var parts = arg.get_slice("=",1).split("x")
            resolution = Vector2i(int(parts[0]),int(parts[1]))
    root.content_scale_size = resolution
    root.content_scale_aspect = Window.CONTENT_SCALE_ASPECT_KEEP
    root.content_scale_mode = Window.CONTENT_SCALE_MODE_VIEWPORT
    var scene = load("res://main.tscn").instantiate()
    root.add_child(scene)
    current_scene = scene
    var combos = []
    for frame in range(1200):
        await process_frame
        combos = descendants(scene,"EawrUiCombo")
        if not combos.is_empty(): break
    if combos.is_empty():
        quit(2)
        return
    await settle()
    # Direct consumers (gallery, LAN seat/difficulty/connection controls) share
    # this constructor path, without a resource list budget.
    var direct = ClassDB.instantiate("EawrUiCombo")
    direct.name = "DirectCombo"
    combos[0].get_parent().add_child(direct)
    direct.size = combos[0].size
    combos.append(direct)
    var reports = []
    for combo in combos:
        combo.show()
        combo.disabled = false
        var original = combo.global_position
        combo.global_position = Vector2(100,100)
        combo.clear()
        for i in range(3): combo.add_item("Choice %d" % i)
        combo.select(0)
        await settle()
        combo.show_popup()
        await settle()
        var popup = combo.get_popup()
        var bar = descendants(popup,"VScrollBar")[0]
        reports.append({"id": str(combo.name), "height": popup.size.y,
            "page": bar.page, "scroll":bar.is_visible_in_tree(),
            "minimum": popup.get_contents_minimum_size().y * popup.content_scale_factor,
            "rect": [popup.position.x,popup.position.y,popup.size.x,popup.size.y]})
        popup.hide()
        combo.global_position = original
        await settle()
    FileAccess.open(output.path_join("users.json"),FileAccess.WRITE).store_string(JSON.stringify(reports,"  "))
    print("POPUP USERS complete: ", reports.size())
    quit()
'''

INPUT_SCRIPT = COMMON + r'''
func key(code):
    for down in [true,false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.pressed = down
        Input.parse_input_event(event)

func pointer(point, button):
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    Input.parse_input_event(motion)
    for down in [true,false]:
        var event = InputEventMouseButton.new()
        event.position = point
        event.global_position = point
        event.button_index = button
        event.pressed = down
        Input.parse_input_event(event)

func run():
    var output = ""
    var resolution = Vector2i(1280,720)
    for arg in OS.get_cmdline_user_args():
        if arg.begins_with("--popup-output="): output = arg.get_slice("=",1)
        if arg.begins_with("--popup-resolution="):
            var parts = arg.get_slice("=",1).split("x")
            resolution = Vector2i(int(parts[0]),int(parts[1]))
    root.content_scale_size = resolution
    root.content_scale_aspect = Window.CONTENT_SCALE_ASPECT_KEEP
    root.content_scale_mode = Window.CONTENT_SCALE_MODE_VIEWPORT
    var scene = load("res://main.tscn").instantiate()
    root.add_child(scene)
    current_scene = scene
    var combo = null
    for frame in range(1200):
        await process_frame
        combo = root.find_child("IDC_PLAYER_FACTION_0",true,false)
        if combo != null: break
    if combo == null or combo.item_count < 10:
        quit(2)
        return
    await settle()
    var popup = combo.get_popup()
    var last = combo.item_count-1
    var expected_last = combo.get_item_text(last)
    var expected_key = combo.get_item_text(2)
    combo.select(0)
    pointer(combo.get_global_rect().get_center(),MOUSE_BUTTON_LEFT)
    await settle()
    var bar = descendants(popup,"VScrollBar")[0]
    var opened = popup.visible
    var first_value = bar.value
    for step in range(45):
        pointer(Vector2(popup.position) + Vector2(popup.size)*0.5,MOUSE_BUTTON_WHEEL_DOWN)
        await process_frame
    await settle()
    var wheel_value = bar.value
    var panel = popup.get_theme_stylebox("panel")
    var scale = popup.content_scale_factor
    var row = (popup.get_theme_font("font").get_height(popup.get_theme_font_size("font_size")) + popup.get_theme_constant("v_separation"))*scale
    var last_point = Vector2(popup.position) + Vector2(popup.size.x/2.0,
        popup.size.y - panel.get_content_margin(SIDE_BOTTOM)*scale - row/2.0)
    pointer(last_point,MOUSE_BUTTON_LEFT)
    await settle()
    var report = {"opened":opened,"first_value":first_value,"wheel_value":wheel_value,
        "last_index":last,"expected_last":expected_last,"click_selected":combo.selected,
        "click_faction":combo.get_text(),"click_closed":not popup.visible}
    combo.grab_focus()
    key(KEY_SPACE)
    await settle()
    report["keyboard_opened"] = popup.visible
    report["keyboard_initial_focused"] = popup.get_focused_item()
    # Keyboard opening focuses the selected final entry. Down wraps to the
    # first entry, then reaches the third through the native input route.
    key(KEY_DOWN)
    await process_frame
    key(KEY_DOWN)
    await process_frame
    key(KEY_DOWN)
    await process_frame
    key(KEY_ENTER)
    await settle()
    report["key_selected"] = combo.selected
    report["key_faction"] = combo.get_text()
    report["expected_key"] = expected_key
    report["key_closed"] = not popup.visible
    pointer(combo.get_global_rect().get_center(),MOUSE_BUTTON_LEFT)
    await settle()
    key(KEY_ESCAPE)
    await settle()
    report["escape_closed"] = not popup.visible
    report["escape_selected"] = combo.selected
    pointer(combo.get_global_rect().get_center(),MOUSE_BUTTON_LEFT)
    await settle()
    pointer(Vector2(8,8),MOUSE_BUTTON_LEFT)
    await settle()
    report["outside_closed"] = not popup.visible
    report["outside_selected"] = combo.selected
    shot(output.path_join("input-final.png"))
    FileAccess.open(output.path_join("input.json"),FileAccess.WRITE).store_string(JSON.stringify(report,"  "))
    print("POPUP INPUT complete: ",JSON.stringify(report))
    quit()
'''
