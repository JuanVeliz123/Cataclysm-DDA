extends Node
## The session-end probe: end a live game on purpose, and prove it actually ends.
##
##   godot --headless --path godot res://scenes/session_end_probe.tscn -- --mode quicksave
##   godot --headless --path godot res://scenes/session_end_probe.tscn -- --mode save_quit
##   godot --headless --path godot res://scenes/session_end_probe.tscn -- --mode quit_nosave
##
## The game-menu lifecycle actions had no coverage, and the field report is
## "sometimes works, sometimes not". The known causes: uquit set from inside the
## input wait was never re-checked until another key arrived, and a legacy ImGui
## window starves the command queue. So this probe dispatches each action through
## the same channel the menu panel uses (request_menu_action, the menu_action
## enum in src/godot_game_commands.h) and asserts on session state under a
## deadline -- while pressing NO keys, because a keypress is exactly what used to
## paper over the bug.
##
## No world view: session state is the subject, so no SubViewport and no
## MapView3D -- the scenario probe owns the visual coverage.

## quicksave = 0, save_and_quit = 1, quit_without_saving = 2 -- the menu_action
## enum in src/godot_game_commands.h, dispatched as game_menu_panel.gd does.
const ACTIONS := { "quicksave": 0, "save_quit": 1, "quit_nosave": 2 }

var host: Node
var _mode := "quit_nosave"
var _phase := "boot"
var _frames := 0
var _dispatched := false
var _quit_wait := false
var _failures: Array[String] = []
var _warnings: Array[String] = []
var _popup_sig := ""

func _ready() -> void:
	var args := OS.get_cmdline_user_args()
	var at := args.find("--mode")
	if at >= 0 and at + 1 < args.size():
		_mode = args[at + 1]
	if not ACTIONS.has(_mode):
		push_error("unknown --mode '%s' (want quicksave | save_quit | quit_nosave)" % _mode)
		get_tree().quit(1)
		return
	print("[end] session-end probe starting, mode=%s" % _mode)
	host = ClassDB.instantiate("CDDAHost")
	host.name = "CDDAHost"
	add_child(host)
	if host.has_method("set_window_size"):
		host.set_window_size(1280, 720)
	if not host.has_method("request_menu_action"):
		push_error("the library has no menu actions; rebuild the GDExtension")
		get_tree().quit(1)
		return
	host.bootstrap_async()

func _process(delta: float) -> void:
	_frames += 1
	if host == null:
		return
	if host.has_method("bootstrap_failed") and host.bootstrap_failed():
		push_error("bootstrap failed: %s" % str(host.get_error_message()))
		get_tree().quit(1)
		return
	# The watchdogs run every frame, through boot, dispatch and the deadlines:
	# a prompt or screen nobody attends is a parked game thread, and a parked
	# game thread reads as this fixture's own timeout.
	_answer_popups()
	_cancel_uilists()
	_heal_legacy_ui(delta)
	match _phase:
		"boot":
			if host.is_ready():
				_phase = "await_session"
				print("[end] ready after %d frames, requesting Play Now" % _frames)
				host.request_new_game("now")
		"await_session":
			if host.is_session_active():
				_phase = "run"
				print("[end] session active after %d frames" % _frames)
				_run()
			elif _frames > 20000:
				push_error("no session after %d frames" % _frames)
				get_tree().quit(1)
		"run":
			pass

## Answer whatever query_popup raises, so the game thread can never park on a
## prompt nobody attends. The scenario probe always answers the LAST option --
## "No", the answer that keeps playing -- but the quit modes exist to STOP
## playing: quit_without_saving raises a "Really quit?"-shaped confirmation on
## this very channel, and answering it "No" would turn the fixture into a no-op
## that fails by never quitting. So once a quit action is dispatched, answer the
## FIRST option (yes/confirm); before dispatch, and in quicksave mode, keep the
## scenario probe's keep-playing default.
func _answer_popups() -> void:
	if not (host.has_method("popup_active") and host.popup_active()):
		return
	var pd: Dictionary = host.get_popup_state()
	var sig := "%s|%s" % [str(pd.get("notice", "")), str(pd.get("text", ""))]
	if sig == _popup_sig:
		return
	_popup_sig = sig
	if bool(pd.get("prompt_active", false)):
		var pop: Array = pd.get("options", [])
		if pop.is_empty():
			return
		var pick := 0 if _mode != "quicksave" and _dispatched else pop.size() - 1
		print("[end] popup '%s' -> answering '%s'" % [str(pd.get("text", "")),
			str(pop[pick])])
		host.popup_answer(pick)

## A uilist nobody attends is a parked game thread and a starved command queue --
## the same reflex the other probes carry.
func _cancel_uilists() -> void:
	if host.has_method("uilist_active") and host.uilist_active():
		print("[end] cancelling an unattended uilist")
		host.uilist_cancel()

var _legacy_ui_since := 0.0
var _legacy_ui_dumped := false

## The watchdog for the C++ screen nobody opened, kept from the scenario probe:
## while any_window_shown() is true the command queue starves, and during a quit
## wait that starvation IS the user's "sometimes not". So this watchdog stays on
## while the quit deadline is pending -- healing the window is part of the
## scenario -- but it announces itself loudly there, so a pass that needed the
## heal is attributable instead of silently flaky.
func _heal_legacy_ui(delta: float) -> void:
	if not (host.has_method("legacy_ui_active") and host.legacy_ui_active()):
		_legacy_ui_since = 0.0
		return
	_legacy_ui_since += delta
	if _legacy_ui_since < 1.5:
		return
	if not _legacy_ui_dumped:
		_legacy_ui_dumped = true
		_dump_overlay_rows()
	if _quit_wait:
		print("[end] LOUD: a legacy C++ screen appeared DURING the quit wait; it was starving the quit and this run needed the Escape heal to pass")
	print("[end] legacy C++ screen is up; pressing Escape")
	_press(KEY_ESCAPE)
	_legacy_ui_since = 0.0

## What the mystery window says, decoded from the curses cell overlay -- the id
## of the screen is not published anywhere, but its own text names it.
func _dump_overlay_rows() -> void:
	var cols: int = host.get_view_cols()
	var rows: int = host.get_view_rows()
	var cells: PackedInt32Array = host.get_view_cells()
	const STRIDE := 4
	if cols <= 0 or rows <= 0 or cells.size() < cols * rows * STRIDE:
		print("[end] legacy screen dump: no cell data")
		return
	var printed := 0
	for r in rows:
		var line := ""
		var claimed := 0
		for c in cols:
			var at := (r * cols + c) * STRIDE
			if cells[at + 3] != 0:
				claimed += 1
				var ch := cells[at]
				line += char(ch) if ch >= 32 and ch < 0x2FFFF else " "
			else:
				line += " "
		if claimed > 4 and printed < 10:
			printed += 1
			print("[end] overlay row %2d | %s" % [r, line.substr(0, 100).strip_edges(false, true)])

## Key injection, the probes' shape exactly: unicode must be set or a letter key
## matches the wrong binding. This fixture only ever sends Escape, and only into
## a legacy screen's own input loop -- see the no-keys rule in _assert below.
func _press(keycode: int) -> void:
	var down := InputEventKey.new()
	down.keycode = keycode
	down.physical_keycode = keycode
	down.pressed = true
	if keycode >= KEY_SPACE and keycode <= KEY_ASCIITILDE:
		down.unicode = char(keycode).to_lower().unicode_at(0)
	host.push_input_event(down)

## Where Play Now writes its world: the game's userdir is the project dir, so
## the saves land under res://save/<World>/, reachable from here. The mtime
## comparison stays WARN-tier all the same -- get_modified_time has one-second
## resolution and the world's files are also written at boot; the session-state
## assertions are the real check.
func _newest_save_mtime() -> int:
	var root := ProjectSettings.globalize_path("res://save")
	var dir := DirAccess.open(root)
	if dir == null:
		return -1
	var newest := 0
	for world in dir.get_directories():
		var wd := DirAccess.open(root.path_join(world))
		if wd == null:
			continue
		for f in wd.get_files():
			newest = maxi(newest, int(FileAccess.get_modified_time(
				root.path_join(world).path_join(f))))
	return newest

func _run() -> void:
	# Let the first turns settle: the HUD snapshot and the command queue both
	# need the game thread parked at the input wait before a baseline means much.
	await get_tree().create_timer(2.0).timeout
	var hud: Dictionary = host.get_hud_state()
	print("[end] avatar '%s' at %s, %s" % [str(hud.get("name", "?")),
		str(hud.get("location", "?")), str(hud.get("time", "?"))])
	var msgs_before: int = (hud.get("messages", []) as Array).size()
	print("[end] baseline commands_ready=%s" % str(host.commands_ready()))
	var mtime_before := -1
	if _mode != "quit_nosave":
		mtime_before = _newest_save_mtime()
		print("[end] newest save mtime before = %d" % mtime_before)

	var action: int = ACTIONS[_mode]
	var err := str(host.request_menu_action(action))
	print("[end] request_menu_action(%d) -> '%s' (empty = queued)" % [action, err])
	_check("action queued", err == "", true, err)
	_dispatched = true
	# The starvation triad, once, right after dispatch: a queued action that
	# never runs is one of these three, and a hang in a CI log should name it.
	print("[end] after dispatch: commands_ready=%s legacy_ui_active=%s popup_active=%s" % [
		str(host.commands_ready()),
		str(host.legacy_ui_active()) if host.has_method("legacy_ui_active") else "?",
		str(host.popup_active()) if host.has_method("popup_active") else "?"])

	if _mode == "quicksave":
		await _assert_quicksave(msgs_before, mtime_before)
	else:
		await _assert_session_ends()
		if _mode == "save_quit":
			var mtime_after := _newest_save_mtime()
			_check("save files advanced", mtime_after > mtime_before, false,
				"newest mtime %d -> %d" % [mtime_before, mtime_after])

	# Let teardown noise land before the verdict, so a crash in the wind-down
	# shows up in this run's log instead of after its last line.
	await get_tree().create_timer(3.0).timeout
	_report()

## quicksave must leave the session alive and the game thread back at the input
## wait. Polled with no key pressed, same rule as the quit modes: the fix under
## test is precisely "an action taken at the input wait acts without another
## keypress", and the rule is cheap to hold everywhere.
func _assert_quicksave(msgs_before: int, mtime_before: int) -> void:
	var stayed_true := true
	var came_back := false
	var waited := 0.0
	while waited < 15.0:
		await get_tree().create_timer(0.25).timeout
		waited += 0.25
		if not host.is_session_active():
			stayed_true = false
			break
		# "Came back" only counts once the action had time to run; an immediate
		# true would just be the queue before the save.
		if waited >= 2.0 and host.commands_ready():
			came_back = true
	_check("session stayed active", stayed_true, true,
		("held true through %.1fs of polling" % waited) if stayed_true
		else ("is_session_active went false %.1fs after quicksave" % waited))
	_check("game came back", came_back, true,
		"commands_ready never true again within 15s of quicksave")
	var msgs_after: int = (host.get_hud_state().get("messages", []) as Array).size()
	_check("hud log gained a line", msgs_after > msgs_before, false,
		"messages %d -> %d (the save toast may not reach the log)" % [msgs_before, msgs_after])
	var mtime_after := _newest_save_mtime()
	_check("save files advanced", mtime_after > mtime_before, false,
		"newest mtime %d -> %d" % [mtime_before, mtime_after])

## The whole bug, as one check: before the fix, uquit set from inside the input
## wait was only re-read when ANOTHER key arrived, so save & quit and quit
## without saving "worked" only when the player happened to keep typing. Poll
## session state under a deadline and press NOTHING -- an accidental key here
## would mask exactly the regression this fixture exists to catch. (The
## legacy-ui watchdog's Escape is the one sanctioned exception: it feeds a
## nested screen's own input loop, not the main wait, and a soliloquy window
## left standing would starve the quit outright -- it logs loudly for a reason.)
func _assert_session_ends() -> void:
	_quit_wait = true
	var waited := 0.0
	# 45 seconds, not 20: a queued quit only runs when the game thread reaches
	# its input wait, and the first turns of a fresh Play Now keep it off the
	# wait for ~20s on this box (measured 23.2s dispatch-to-inactive with a 4s
	# settle). The deadline is for the whole journey; the quit itself lands the
	# moment the thread first waits, keyless -- which is the thing under test.
	while waited < 45.0 and host.is_session_active():
		await get_tree().create_timer(0.25).timeout
		waited += 0.25
	_quit_wait = false
	var ended: bool = not host.is_session_active()
	_check("session ended", ended, true,
		("keyless, %.1fs after %s" % [waited, _mode]) if ended
		else ("is_session_active still true %.0fs after %s, no key pressed" % [waited, _mode]))
	if ended:
		print("[end] session inactive %.1fs after dispatch" % waited)

func _check(name: String, ok: bool, required: bool, detail: String) -> void:
	var tier := "REQUIRED" if required else "WARN"
	print("[end] %-28s %s%s" % [name, "ok" if ok else "FAILED (%s)" % tier,
		"" if detail.is_empty() else "  -- " + detail])
	if ok:
		return
	if required:
		_failures.append("%s: %s" % [name, detail])
	else:
		_warnings.append("%s: %s" % [name, detail])

func _report() -> void:
	print("")
	print("[end] ============= session-end coverage (%s) =============" % _mode)
	for w in _warnings:
		print("[end] WARN     %s" % w)
	for f in _failures:
		print("[end] REQUIRED %s" % f)
	if _failures.is_empty():
		print("[end] all required checks passed (%d warnings)" % _warnings.size())
	else:
		print("[end] FAILED: %d required check(s)" % _failures.size())
	var code := 0 if _failures.is_empty() else 1
	# The backend must know the code: session teardown ends in std::_Exit from
	# the game thread, which cannot read what SceneTree.quit was given and used
	# to hard-code 0 -- a failing run that reports success.
	if host.has_method("note_exit_code"):
		host.note_exit_code(code)
	get_tree().quit(code)
