extends Node
## Playtest of the legacy (curses/ImGui) menus under the Godot renderer.
##
## Unlike headless_probe, this runs the real main.tscn and injects keys through
## Godot's Input, so they pass through host.gd exactly as a player's would. After
## each step it reports whether a legacy screen is up, what that screen painted
## into the curses overlay, and whether any Godot panel claimed the key instead.
##
## Meant for a build without the per-screen Godot panels, where every menu is the
## legacy one. With the panels, i / @ / Escape open Godot's own screens and are
## reported here as takeovers.
##
## Run (windowed, so screenshots rasterise; on Apple Silicon prefix
## `arch -arm64` so Godot matches the extension's architecture):
##   Godot --path godot res://scenes/legacy_playtest.tscn -- --shots /some/dir
##
## Exits 0 when every step behaved as expected, 1 otherwise.

const CELL_STRIDE := 4
const SETTLE_SEC := 2.5

## [key or shifted char, label, expectation]
## expectation: "legacy" = a legacy screen should be up afterwards,
##              "clear"  = nothing should be drawn over the map,
##              ""       = just record.
var _steps: Array = [
	[0, "baseline", "clear"],
	[KEY_RIGHT, "move right", "clear"],
	[KEY_LEFT, "move back", "clear"],
	["P", "message log", "legacy"],
	[KEY_ESCAPE, "close message log", "clear"],
	[KEY_I, "inventory", "legacy"],
	[KEY_ESCAPE, "close inventory", "clear"],
	["@", "character sheet", "legacy"],
	[KEY_ESCAPE, "close character sheet", "clear"],
	["&", "crafting", "legacy"],
	[KEY_ESCAPE, "close crafting", "clear"],
	["V", "surroundings list", "legacy"],
	[KEY_ESCAPE, "close surroundings", "clear"],
	["?", "help", "legacy"],
	[KEY_ESCAPE, "close help", "clear"],
	[KEY_ESCAPE, "escape menu", "legacy"],
	[KEY_DOWN, "escape menu: move highlight", "legacy"],
	[KEY_ESCAPE, "close escape menu", "clear"],
	[KEY_X, "look mode", ""],
	[KEY_ESCAPE, "leave look mode", "clear"],
	[KEY_M, "overmap", ""],
	[KEY_ESCAPE, "close overmap", "clear"],
	[KEY_RIGHT, "move after menus (input still live)", "clear"],
]

var _main: Node
var _host: Node
var _shots_dir := ""
var _failures: Array[String] = []

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var at := args.find("--shots")
	if at >= 0 and at + 1 < args.size():
		_shots_dir = args[at + 1]
		DirAccess.make_dir_recursive_absolute(_shots_dir)
	_main = load("res://scenes/main.tscn").instantiate()
	add_child(_main)
	_host = _main.get_node("CDDAHost")
	_run.call_deferred()

func _run() -> void:
	var t0 := Time.get_ticks_msec()
	if not _host.has_method("is_ready"):
		print("[play] FAIL: the GDExtension did not load (architecture mismatch or stale build?)")
		get_tree().quit(1)
		return
	while not _host.is_ready():
		if _host.bootstrap_failed() or Time.get_ticks_msec() - t0 > 900000:
			_fail("bootstrap: " + str(_host.get_error_message()))
			_finish()
			return
		await get_tree().process_frame
	print("[play] data loaded in %.1fs" % ((Time.get_ticks_msec() - t0) / 1000.0))
	await _wait(1.0)
	await _shot("00_main_menu")
	_main._on_new_now_pressed()
	t0 = Time.get_ticks_msec()
	while not (_host.is_session_active() and str(_host.get_hud_state().get("name", "")) != ""
			and _main.world.visible):
		if Time.get_ticks_msec() - t0 > 600000:
			_fail("session never started")
			_finish()
			return
		await get_tree().process_frame
	print("[play] session up in %.1fs, avatar=%s" % [
		(Time.get_ticks_msec() - t0) / 1000.0, str(_host.get_hud_state().get("name", ""))])
	await _wait(4.0)
	var i := 0
	for step in _steps:
		i += 1
		await _do_step(i, step)
	_finish()

func _do_step(i: int, step: Array) -> void:
	var key = step[0]
	var label: String = step[1]
	var expect: String = step[2]
	var before_time := str(_host.get_hud_state().get("time", ""))
	if typeof(key) == TYPE_STRING:
		_send_char(key)
	elif key != 0:
		_send_key(key)
	await _wait(SETTLE_SEC)
	var legacy: bool = _host.legacy_ui_active()
	var panels := _godot_panels_visible()
	var dump := _overlay_text()
	var hud: Dictionary = _host.get_hud_state()
	var msgs: Array = hud.get("messages", [])
	var last := str(msgs[msgs.size() - 1].get("text", "")) if not msgs.is_empty() else ""
	print("\n[step %02d] %s -> legacy_ui=%s overlay_rows=%d time %s -> %s | godot panels: %s"
		% [i, label, str(legacy), dump.size(), before_time, str(hud.get("time", "")),
		", ".join(panels) if not panels.is_empty() else "none"])
	print("          log: %s" % last)
	for row in dump.slice(0, 14):
		print("          | %s" % row)
	if not panels.is_empty():
		_fail("step %d (%s): Godot panel(s) took over: %s" % [i, label, ", ".join(panels)])
	if expect == "legacy" and not legacy:
		_fail("step %d (%s): expected a legacy screen, none is up" % [i, label])
	if expect == "clear" and legacy:
		_fail("step %d (%s): a legacy screen is still up" % [i, label])
	await _shot("%02d_%s" % [i, label.to_snake_case().replace(":", "")])

## Every Godot-drawn panel a key could have opened instead of the legacy screen.
func _godot_panels_visible() -> Array[String]:
	var out: Array[String] = []
	for n in ["inventory_panel", "character_panel", "game_menu_panel", "uilist_panel",
			"popup_panel", "textwin_panel", "options_panel", "keybind_panel",
			"crafting_panel", "dialogue_panel", "surroundings_panel"]:
		var c = _main.get(n)
		if c != null and c is CanvasItem and c.visible:
			out.append(n)
	return out

## The curses overlay as text, one string per row that has anything drawn.
func _overlay_text() -> Array[String]:
	var out: Array[String] = []
	var cols: int = _host.get_view_cols()
	var rows: int = _host.get_view_rows()
	var cells: PackedInt32Array = _host.get_view_cells()
	if cols <= 0 or rows <= 0 or cells.size() < cols * rows * CELL_STRIDE:
		return out
	for y in rows:
		var text := ""
		var any := false
		for x in cols:
			var n: int = (y * cols + x) * CELL_STRIDE
			if cells[n + 3] != 0:
				any = true
			var cp: int = cells[n]
			text += String.chr(cp) if cp > 32 and cp < 0x10FFFF else " "
		if any and text.strip_edges() != "":
			out.append(text.strip_edges(false, true))
	return out

func _send_key(keycode: int) -> void:
	var ev := InputEventKey.new()
	ev.keycode = keycode
	ev.physical_keycode = keycode
	ev.pressed = true
	if keycode >= KEY_SPACE and keycode <= KEY_ASCIITILDE:
		ev.unicode = char(keycode).to_lower().unicode_at(0)
	Input.parse_input_event(ev)
	var up: InputEventKey = ev.duplicate()
	up.pressed = false
	Input.parse_input_event(up)

## A shifted character the way a real keyboard delivers it.
func _send_char(ch: String) -> void:
	var base := {"!": KEY_1, "@": KEY_2, "&": KEY_7, "?": KEY_SLASH}
	var keycode: int = base.get(ch, ch.to_upper().unicode_at(0))
	var ev := InputEventKey.new()
	ev.keycode = keycode
	ev.physical_keycode = keycode
	ev.pressed = true
	ev.shift_pressed = true
	ev.unicode = ch.unicode_at(0)
	Input.parse_input_event(ev)
	var up: InputEventKey = ev.duplicate()
	up.pressed = false
	Input.parse_input_event(up)

func _wait(sec: float) -> void:
	await get_tree().create_timer(sec).timeout

func _shot(tag: String) -> void:
	if _shots_dir == "" or DisplayServer.get_name() == "headless":
		return
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	var img := get_viewport().get_texture().get_image()
	if img != null:
		img.save_png(_shots_dir.path_join(tag + ".png"))

func _fail(msg: String) -> void:
	_failures.append(msg)

func _finish() -> void:
	print("\n[play] ================ result ================")
	if _failures.is_empty():
		print("[play] OK: every step behaved as expected")
	for f in _failures:
		print("[play] FAIL: " + f)
	var code := 0 if _failures.is_empty() else 1
	if _host.has_method("note_exit_code"):
		_host.note_exit_code(code)
	_host.request_quit()
	get_tree().quit(code)
