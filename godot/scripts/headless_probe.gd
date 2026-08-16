extends Node
## Headless harness: boot, Play Now, then drive keys and report what actually
## reached ViewSnapshot. Rendering is not involved -- this reads the same cell
## array TerminalView reads, so it separates "the C++ side never produced it"
## from "Godot never drew it".
##
## Run: godot --headless --path godot res://scenes/headless_probe.tscn

const CELL_STRIDE := 4

var host: Node
var _phase := "boot"
var _frames := 0
var _waited := 0
var _step := 0
var _uilist_seen := 0
var _uilist_title := ""
var _uilist_frames := 0
var _popup_sig := ""
var _textwin_sig := ""
var _options_seen := 0
var _shot_options := false
var _options_probe := {}
var _keybind_seen := 0
var _shot_keybind := false
var _keybind_dispatched := false
var _keybind_bind_target := ""
var _keybind_before := ""
var _craft_seen := 0
var _shot_craft := false
var _craft_dispatched := false
var _craft_found_at := 0
var _craft_tries := 0
var _textwin_seen := 0
var _picked_save := false
## Every distinct run of scrolling combat text seen at any point in the run.
## Collected continuously because SCT lives about a second and can be produced
## by anything -- the player hitting, or being hit -- so a single check at a
## chosen moment reliably misses it.
var _sct_seen: Array = []
## Highest animation-frame generation seen. If this never advances, the draw
## callbacks that produce combat text are not running at all -- a different
## problem from them running and having nothing to say.
var _anim_gen_max: int = -1
var _shot_uilist := false
## The prompt this fixture last answered. A plain "have we answered yet" latch
## silently swallowed every prompt after the first: options asks "Save changes?",
## and from then on the keybinding conflict prompt and the crafting screen's own
## prompts were never answered, so the game thread parked on one and the rest of
## the run tested nothing while still exiting 0.
var _answered_sig := ""

## key to press, label to report after it settles
var _script: Array = [
	[0, "baseline (no menu)"],
	[KEY_RIGHT, "after move"],
	# '!' is Shift+1: keycode KEY_1, unicode '!'. Bound to `safemode`, and the
	# only way to turn safe mode off -- '1' is movement.
	["!", "after ! (safe mode toggled)"],
	["!", "after ! again (toggled back)"],
	# An uppercase command: Shift+P is `messages`, one of 174 uppercase bindings.
	["P", "after P (message log)"],
	# ...and close it again, or it swallows every later step in this script.
	[KEY_ESCAPE, "after ESC (message log closed)"],
	# x enters look mode, e opens the extended description -- a Godot text window.
	[KEY_X, "after x (look mode)"],
	[KEY_E, "after e (expect text window)"],
	[KEY_LEFT, "after move back"],
	[KEY_ESCAPE, "after ESC (expect main menu)"],
	[KEY_DOWN, "after DOWN (expect highlight moved)"],
	[KEY_DOWN, "after DOWN again"],
	[KEY_ESCAPE, "after ESC (expect menu closed)"],
	# Popup-lifecycle check. The main menu's "e Quicksave" puts up a static_popup,
	# which is a cataimgui::window (query_popup_impl). Its frame has to be retired
	# once the popup is gone, or it stays painted and any_window_shown() keeps
	# swallowing input -- which is what "an autosave freezes the game" looked like.
	# Note: quicksave() itself early-returns unless something happened since the
	# last save, so this exercises the popup path, not necessarily a full save.
	[KEY_ESCAPE, "after ESC (menu reopened)"],
	[KEY_E, "after E (quicksave ran)"],
	[KEY_ESCAPE, "after ESC (input still live? expect menu again)"],
	[KEY_ESCAPE, "after ESC (expect closed again)"],
]

## The panels are exercised for real, not just parsed: a Control that only ever
## fails at runtime (a bad theme override, a null index) parses perfectly.
## Indices are referenced above (4 = uilist, 5 = popup): append, do not reorder.
var _panels: Array[Control] = []

## MapView, built and refreshed for the same reason as the panels. It is not a
## Control and is driven separately: batching, the glyph layers and the light
## texture all only run at refresh time, so nothing else here would catch a
## script error in them.
var _map_view: Node2D

## Render debug overlay (SP-9), exercised for the same reason.
var _debug_overlay: PanelContainer

func _ready() -> void:
	host = ClassDB.instantiate("CDDAHost")
	host.name = "CDDAHost"
	add_child(host)
	host.set_window_size(1280, 720)
	host.bootstrap_async()
	print("[probe] bootstrap requested")
	print("[probe] api_version=%s (host.gd requires %d)" % [
		str(host.api_version()) if host.has_method("api_version") else "missing",
		load("res://scripts/host.gd").get_script_constant_map().get("REQUIRED_API_VERSION", -1)])

	for path in ["res://scripts/hud_panel.gd", "res://scripts/inventory_panel.gd",
			"res://scripts/character_panel.gd", "res://scripts/game_menu_panel.gd",
			"res://scripts/uilist_panel.gd", "res://scripts/popup_panel.gd",
			"res://scripts/textwin_panel.gd", "res://scripts/options_panel.gd",
			"res://scripts/keybind_panel.gd", "res://scripts/crafting_panel.gd"]:
		var panel := Control.new()
		panel.set_script(load(path))
		add_child(panel)
		panel.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
		panel.size = get_viewport().get_visible_rect().size
		_panels.append(panel)

	_map_view = Node2D.new()
	_map_view.name = "MapView"
	_map_view.set_script(load("res://scripts/map_view.gd"))
	add_child(_map_view)

	# Not in _panels: it is a PanelContainer, and that list is built as Controls.
	_debug_overlay = PanelContainer.new()
	_debug_overlay.name = "DebugOverlay"
	_debug_overlay.set_script(load("res://scripts/debug_overlay.gd"))
	add_child(_debug_overlay)

func _exercise_panels() -> void:
	for panel in _panels:
		if panel.has_method("setup"):
			panel.setup(host)
		if panel.has_method("refresh"):
			panel.refresh()
			panel.refresh()
	print("[probe] panels refreshed: %d" % _panels.size())
	await _shoot_solo(0, "sidebar")
	await _shoot_solo(1, "inventory")
	await _shoot_solo(2, "character")
	await _shoot_solo(3, "gamemenu")
	# MapView asks for the tiles its viewport covers; check the request lands and
	# the published extent follows it, rather than staying at the curses grid.
	_dump_sidebar_layout()
	print("[map] extent before request = %s" % str(host.get_map_view_size()))
	host.set_map_view_tiles(101, 61)
	await get_tree().create_timer(0.6).timeout
	print("[map] extent after request  = %s (want 101x61)" % str(host.get_map_view_size()))
	_exercise_map_view()
	_dump_render_stats()
	_dump_effects()
	if _debug_overlay != null:
		_debug_overlay.setup(host)
		_debug_overlay.refresh()
		print("[render] debug overlay rows = %d" % _debug_overlay._body.get_child_count())
	print("[menu] quicksave dispatch = %s"
		% ("<accepted>" if str(host.request_menu_action(0)) == "" else str(host.request_menu_action(0))))
	print("[menu] bad action = %s" % str(host.request_menu_action(99)))
	# MENU-7: open the options screen. The game thread blocks in
	# options_manager::show() until the block in _process below dismisses it.
	print("[menu] options dispatch = %s"
		% ("<accepted>" if str(host.request_menu_action(3)) == "" else "<refused>"))
	var inv: Dictionary = host.get_inventory_state()
	print("[inv] carry=%s (%d%%) volume=%s (%d%%) items=%d" % [
		str(inv.get("carry", "")), int(inv.get("carry_pct", -1)),
		str(inv.get("volume", "")), int(inv.get("volume_pct", -1)),
		(inv.get("items", []) as Array).size()])
	var items: Array = inv.get("items", [])
	for i in mini(4, items.size()):
		print("[inv] row %d = %s" % [i, str(items[i])])
	# Count the rows the panel actually built, to prove the columns are populated.
	for panel in _panels:
		if "_carried_box" in panel and panel._carried_box != null:
			print("[inv] built carried=%d worn=%d" % [
				panel._carried_box.get_child_count(), panel._worn_box.get_child_count()])
			# Exercise selection, which rebuilds with a highlighted row.
			if not items.is_empty():
				panel._select(int(items[0].get("uid", 0)))
				print("[inv] after select carried=%d" % panel._carried_box.get_child_count())

func _process(_delta: float) -> void:
	_frames += 1
	# Stand in for host.gd: attend any menu C++ hands over, or the game thread
	# waits on an answer nobody is going to give.
	if host != null and host.has_method("textwin_active") and host.textwin_active():
		if _panels.size() > 6:
			_panels[6].refresh()
		var td: Dictionary = host.get_textwin_state()
		var tsig := "%s|%d" % [str(td.get("title", "")), int(td.get("current", -1))]
		if tsig != _textwin_sig:
			_textwin_sig = tsig
			var tt: Array = td.get("tabs", [])
			print("[textwin] '%s' tabs=%d current=%d" % [str(td.get("title", "")),
				tt.size(), int(td.get("current", -1))])
			if _textwin_seen == 0:
				await _shoot_solo(6, "textwin")
			for i in tt.size():
				var body := str(tt[i].get("body", ""))
				print("[textwin]   tab '%s' %d chars: %s" % [str(tt[i].get("label", "")),
					body.length(), body.substr(0, 60).replace("\n", " / ")])
			_textwin_seen += 1
			# Flip to the next tab once, then dismiss.
			if _textwin_seen == 1 and tt.size() > 1:
				host.textwin_select_tab(2)
			else:
				host.textwin_dismiss()
	if host != null and host.has_method("options_active") and host.options_active():
		if _panels.size() > 7:
			_panels[7].refresh()
		_options_seen += 1
		if _options_seen == 20 and not _shot_options:
			_shot_options = true
			_dump_options()
			await _shoot_solo(7, "options")
		# The point of the round trip: ask for a step, then read the value the
		# game thread actually produced. A panel that echoed its own guess would
		# pass this without the game ever having been told.
		if _options_seen == 40 and not _options_probe.is_empty():
			host.options_step(str(_options_probe.get("id", "")), 1)
		if _options_seen == 60 and not _options_probe.is_empty():
			var now: Dictionary = host.get_options_values()
			var got: Dictionary = now.get(str(_options_probe.get("id", "")), {})
			print("[options] %s: %s -> %s (%s)" % [
				str(_options_probe.get("id", "")), str(_options_probe.get("before", "")),
				str(got.get("current", "?")),
				"changed" if str(got.get("current", "")) != str(_options_probe.get("before", ""))
					else "UNCHANGED"])
		if _options_seen == 80:
			host.options_dismiss()
	elif not _keybind_dispatched and _options_seen > 0:
		# Only once options has closed: the game thread runs one screen at a time.
		_keybind_dispatched = true
		print("[menu] keybindings dispatch = %s"
			% ("<accepted>" if str(host.request_menu_action(4)) == "" else "<refused>"))
	if host != null and host.has_method("keybind_active") and host.keybind_active():
		if _panels.size() > 8:
			_panels[8].refresh()
		_keybind_seen += 1
		if _keybind_seen == 20 and not _shot_keybind:
			_shot_keybind = true
			_dump_keybinds()
			await _shoot_solo(8, "keybinds")
		# The add path is the one worth proving: the key has to travel out as a raw
		# Godot event, through the input bridge, and come back as a binding. A
		# panel that described the key instead would look identical here and
		# produce a binding that never matches in play.
		if _keybind_seen == 30 and _keybind_bind_target != "":
			print("[keybind] requesting bind for '%s' (was: %s)"
				% [_keybind_bind_target, _keybind_before])
			host.keybind_request(_keybind_bind_target, 2)
		if _keybind_seen == 45 and _keybind_bind_target != "":
			print("[keybind] capture prompt up = %s" % str(host.popup_active()))
			_press(KEY_F9)
		# The key may already be in use, in which case action_add puts up a second
		# "press any key" popup to acknowledge it. Nothing else will clear that,
		# and while it is up the game thread cannot see the dismiss request, so
		# the whole screen would hang and the rest of the run would test nothing.
		if _keybind_seen == 58 and _keybind_bind_target != "" and host.popup_active():
			print("[keybind] follow-up popup still up; clearing it")
			_press(KEY_SPACE)
		if _keybind_seen == 70 and _keybind_bind_target != "":
			var after := ""
			for r in (host.get_keybind_state().get("rows", []) as Array):
				if str(r.get("action_id", "")) == _keybind_bind_target:
					after = str(r.get("keys", ""))
			print("[keybind] '%s': %s -> %s (%s)" % [_keybind_bind_target,
				_keybind_before, after,
				"BOUND" if after != _keybind_before else "UNCHANGED"])
		if _keybind_seen == 90:
			host.keybind_dismiss()
	elif _keybind_seen > 0 and not _craft_dispatched:
		# The crafting screen is opened by the game's own '&' action rather than a
		# menu action, so it goes in as a keypress -- and a single press is not
		# enough. The key-drive sequence below is running at the same time, and if
		# it walks into an NPC the turn is spent on "What to do with X?" and the
		# '&' is never acted on. Retry until the screen actually opens.
		_craft_tries += 1
		if _craft_tries % 40 == 1:
			print("[menu] crafting: pressing & (attempt %d)" % (1 + _craft_tries / 40))
			_press_shifted(KEY_7, "&")
		if _craft_tries > 600:
			_craft_dispatched = true
			print("[craft] FAILED: crafting screen never opened after 15 attempts")
	if host != null and host.has_method("crafting_active") and host.crafting_active():
		if _panels.size() > 9:
			_panels[9].refresh()
		_craft_seen += 1
		_craft_dispatched = true
		# The screen opens on the "*" category, whose first subcategory is
		# Favorites -- legitimately empty for a fresh character. Walking the tabs
		# until one has recipes is the difference between proving the panel draws
		# a list and proving only that it draws an empty one.
		if _craft_seen % 15 == 0 and _craft_seen <= 165 and not _shot_craft:
			var rows: Array = (host.get_crafting_list() as Dictionary).get("rows", [])
			if rows.is_empty():
				host.crafting_action("NEXT_TAB")
			else:
				_shot_craft = true
				_craft_found_at = _craft_seen
				_dump_crafting("populated category")
				await _shoot_solo(9, "crafting")
		# Move the cursor and confirm the detail pane follows it: the pane is
		# published on its own generation, so a stuck one looks exactly like a
		# working screen that simply never changes.
		if _shot_craft and _craft_seen == _craft_found_at + 20:
			host.crafting_action("DOWN")
		if _shot_craft and _craft_seen == _craft_found_at + 35:
			_dump_crafting_detail("after DOWN")
		if _craft_seen == 200:
			if not _shot_craft:
				print("[craft] FAILED: no category had any recipes after 11 tabs")
			host.crafting_action("QUIT")
	if host != null and host.has_method("popup_active") and host.popup_active():
		if _panels.size() > 5:
			_panels[5].refresh()
		var pd: Dictionary = host.get_popup_state()
		var sig := "%s|%s|%s" % [str(pd.get("notice", "")), str(pd.get("text", "")),
			str(pd.get("options", []))]
		if sig != _popup_sig:
			_popup_sig = sig
			if bool(pd.get("notice_active", false)):
				print("[popup] NOTICE: %s" % str(pd.get("notice", "")))
			if bool(pd.get("prompt_active", false)):
				print("[popup] PROMPT: %s  options=%s cancel=%s"
					% [str(pd.get("text", "")), str(pd.get("options", [])),
					str(pd.get("allow_cancel", false))])
				var pop: Array = pd.get("options", [])
				if not pop.is_empty() and sig != _answered_sig:
					_answered_sig = sig
					# Answer the last option -- "No" -- so the run continues.
					print("[popup] answering '%s'" % str(pop[pop.size() - 1]))
					host.popup_answer(pop.size() - 1)
	if host != null and host.has_method("uilist_active") and host.uilist_active():
		if _panels.size() > 4:
			_panels[4].refresh()
		_uilist_seen += 1
		# Exercise the round trip, and reach a yes/no prompt: "Save and quit"
		# runs query_yn, which is the query_popup path.
		if _uilist_seen == 20 and not _shot_uilist:
			_shot_uilist = true
			await _shoot_solo(4, "uilist")
		if _uilist_seen == 40 and not _picked_save:
			_picked_save = true
			var u2: Dictionary = host.get_uilist_state()
			for e in (u2.get("entries", []) as Array):
				if str(e.get("text", "")).findn("save and quit") >= 0:
					print("[uilist] confirming '%s'" % str(e.get("text", "")))
					host.uilist_confirm(int(e.get("index", -1)))
					break
		# Nothing in this fixture drives a menu the *game* opens on its own, and it
		# opens plenty: walking into an NPC brings up "What to do with X?". Left
		# alone the game thread parks on it forever, every later stage silently
		# does not run, and the probe still exits 0. Counted per menu rather than
		# globally, so a long-lived menu the fixture *is* driving is not cut short.
		var title := str((host.get_uilist_state() as Dictionary).get("title", ""))
		if title != _uilist_title:
			_uilist_title = title
			_uilist_frames = 0
		_uilist_frames += 1
		if _uilist_frames == 120:
			print("[uilist] cancelling unattended '%s'" % title)
			host.uilist_cancel()
	if host != null and host.has_method("get_anim_generation"):
		var ag: int = host.get_anim_generation()
		if ag > _anim_gen_max:
			_anim_gen_max = ag
	if host != null and host.has_method("get_anim_texts"):
		for t in host.get_anim_texts():
			var row := "%-14s @%s run=%s life=%.2f" % [str(t.get("text", "")),
				str(t.get("pos", "")), str(t.get("run", 0)), float(t.get("life", 0.0))]
			if not _sct_seen.has(row):
				_sct_seen.append(row)
	if host.bootstrap_failed():
		print("[probe] FATAL: ", host.get_error_message())
		get_tree().quit(1)
		return

	match _phase:
		"boot":
			if host.is_ready():
				print("[probe] ready after %d frames" % _frames)
				host.request_new_game("now")
				_phase = "await_session"
				_waited = 0
		"await_session":
			_waited += 1
			var hud: Dictionary = host.get_hud_state()
			if host.is_session_active() and str(hud.get("name", "")) != "":
				print("[probe] session active after %d frames, name=%s"
					% [_waited, str(hud.get("name", ""))])
				_exercise_panels()
				var h: Dictionary = host.get_hud_state()
				for k in ["date", "time", "location", "weather", "focus", "speed",
						"sound", "pain_level", "morale_level", "power_pct",
						"weapon", "weapon_glyph", "style", "temperature",
						"compass", "threat_summary"]:
					print("[hud] %s = %s" % [k, str(h.get(k, "<missing>"))])
				print("[hud] effects = %s" % str(h.get("effects", [])))
				print("[hud] contacts = %s" % str(h.get("contacts", [])))
				var lg: Array = h.get("messages", [])
				print("[hud] messages[0] = %s" % (str(lg[0]) if not lg.is_empty() else "<none>"))
				print("[hud] limbs = %d" % (h.get("limbs", []) as Array).size())
				_phase = "run"
				_waited = 0
			elif _waited > 20000:
				print("[probe] TIMEOUT; active=%s" % str(host.is_session_active()))
				get_tree().quit(1)
		"run":
			_waited += 1
			if _waited < 400:
				return
			_waited = 0
			if _step >= _script.size():
				_shutdown()
				return
			var entry: Array = _script[_step]
			_step += 1
			if typeof(entry[0]) == TYPE_STRING:
				var ch := str(entry[0])
				# Shift + the key that produces this character.
				var base: int = KEY_1 if ch == "!" else ch.to_upper().unicode_at(0)
				_press_shifted(base, ch)
				_script.insert(_step, [0, entry[1]])
			elif entry[0] != 0:
				_press(entry[0])
				# Report on the NEXT visit so the game thread has drawn a frame.
				_script.insert(_step, [0, entry[1]])
			else:
				for panel in _panels:
					if panel.has_method("refresh"):
						panel.refresh()
				_report(entry[1])

## Real Godot key events carry a unicode value, and the input bridge prefers it
## for printable keys so that "e" is 'e' and not the raw keycode 'E'. A synthetic
## event with unicode 0 falls back to the keycode, which is uppercase -- so set
## it here or every letter hotkey is tested wrong.
## The sidebar is a fixed-width column; anything whose minimum size exceeds it
## overflows and is clipped by the panel edge. Print the offenders rather than
## trying to read them off a screenshot.
## Print enough of the published model to tell a working screen from an empty
## one, and pick a bool to round-trip.
func _dump_options() -> void:
	var lay: Dictionary = host.get_options_layout()
	var pages: Array = lay.get("pages", [])
	var vals: Dictionary = host.get_options_values()
	print("[options] pages=%d current=%d values=%d" % [
		pages.size(), int(lay.get("current_page", -1)), vals.size()])
	for p in pages:
		var rows: Array = p.get("rows", [])
		var opts := 0
		var groups := 0
		for r in rows:
			if int(r.get("type", 0)) == 2:
				opts += 1
			elif int(r.get("type", 0)) == 1:
				groups += 1
		print("[options]   '%s' rows=%d options=%d groups=%d" % [
			str(p.get("name", "")), rows.size(), opts, groups])
	# A bool is the cleanest round trip: stepping it must flip the value.
	for p in pages:
		for r in p.get("rows", []):
			if str(r.get("value_type", "")) == "bool":
				var id := str(r.get("id", ""))
				_options_probe = {"id": id, "before": str(vals.get(id, {}).get("current", ""))}
				print("[options] round-tripping '%s' (%s) = %s" % [
					str(r.get("text", "")), id, str(_options_probe["before"])])
				return

func _dump_crafting(tag: String = "") -> void:
	var d: Dictionary = host.get_crafting_list()
	var rows: Array = d.get("rows", [])
	var craftable := 0
	for r in rows:
		if bool(r.get("craftable", false)):
			craftable += 1
	print("[craft] %stabs=%d subtabs=%d rows=%d craftable=%d selected=%d batch=%s" % [
		("%s: " % tag) if tag != "" else "",
		(d.get("tabs", []) as Array).size(), (d.get("subtabs", []) as Array).size(),
		rows.size(), craftable, int(d.get("selected", -1)),
		str(d.get("batch_mode", false))])
	for i in mini(3, rows.size()):
		print("[craft]   %-40s craftable=%s nested=%s" % [
			str(rows[i].get("name", "")), str(rows[i].get("craftable", false)),
			str(rows[i].get("nested", false))])
	_dump_crafting_detail(tag)

func _dump_crafting_detail(tag: String) -> void:
	var lines: Array = (host.get_crafting_detail() as Dictionary).get("lines", [])
	print("[craft] detail%s: %d lines" % [(" %s" % tag) if tag != "" else "", lines.size()])
	for i in mini(5, lines.size()):
		print("[craft]   %s%s" % ["# " if bool(lines[i].get("header", false)) else "  ",
			str(lines[i].get("text", "")).substr(0, 90)])

func _dump_keybinds() -> void:
	var d: Dictionary = host.get_keybind_state()
	var rows: Array = d.get("rows", [])
	var bound := 0
	var local := 0
	for r in rows:
		if int(r.get("scope", 2)) != 2:
			bound += 1
		if int(r.get("scope", 2)) == 1:
			local += 1
	print("[keybind] context='%s' rows=%d bound=%d local=%d execute=%s" % [
		str(d.get("context", "")), rows.size(), bound, local,
		str(d.get("permit_execute", false))])
	for i in mini(4, rows.size()):
		print("[keybind]   %-28s %s" % [str(rows[i].get("name", "")),
			str(rows[i].get("keys", ""))])
	# Bind an unbound action rather than rebinding a live one, so a failure here
	# cannot leave the probe unable to drive the game afterwards.
	for r in rows:
		if int(r.get("scope", 0)) == 2:
			_keybind_bind_target = str(r.get("action_id", ""))
			_keybind_before = str(r.get("keys", ""))
			return

func _dump_sidebar_layout() -> void:
	var panel: Control = _panels[0]
	panel.size = Vector2(1280, 807)
	panel.refresh()
	await get_tree().process_frame
	await get_tree().process_frame
	var col: Control = panel._body_column
	if col == null:
		print("[layout] no column")
		return
	var avail := col.size.x
	print("[layout] column width=%.0f (sidebar %.0f)" % [avail, panel.WIDTH])
	for child in col.get_children():
		var c := child as Control
		if c == null:
			continue
		var mw: float = c.get_combined_minimum_size().x
		var flag := "  OVER" if mw > avail else ""
		print("[layout]   %-22s min=%.0f actual=%.0f%s"
			% [c.get_class(), mw, c.size.x, flag])
		if mw > avail:
			for g in c.get_children():
				var gc := g as Control
				if gc != null:
					print("[layout]       %-18s min=%.0f  %s"
						% [gc.get_class(), gc.get_combined_minimum_size().x,
						(gc.text if "text" in gc else "")])

## Save what MapView actually rasterised, when there is a renderer to do it.
##
## Everything else here reports what C++ published and what MapView built, which
## is not the same as what the map looks like. With a virtual display and a
## software rasteriser there is no reason to stop short of the picture:
##
##   xvfb-run -s "-screen 0 1600x900x24" \
##     env VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
##     godot --rendering-driver vulkan --path godot \
##           res://scenes/headless_probe.tscn -- --screenshot /tmp/map.png
##
## Skipped entirely without that argument, so the ordinary headless run is
## unaffected.
func _maybe_screenshot(tag: String) -> void:
	var args := OS.get_cmdline_user_args()
	var at := args.find("--screenshot")
	if at < 0 or at + 1 >= args.size():
		return
	if DisplayServer.get_name() == "headless":
		print("[shot] headless display server: nothing was rasterised, skipping")
		return
	# One full frame has to complete before the viewport texture holds anything.
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	var img: Image = get_viewport().get_texture().get_image()
	if img == null:
		print("[shot] no viewport image")
		return
	var path: String = args[at + 1]
	if tag != "":
		path = path.get_basename() + "." + tag + "." + path.get_extension()
	var err := img.save_png(path)
	print("[shot] %s -> %s (%s, err=%d)" % [tag, path, str(img.get_size()), err])

## Screenshot one panel on its own.
##
## The probe builds every panel as a sibling and leaves them all visible, which
## is fine for exercising code but useless to look at. This hides the rest for a
## frame so each panel can be seen laid out on its own -- the only way to check
## spacing, overflow and whether a row is clipped, none of which the cell counts
## can tell you.
func _shoot_solo(index: int, tag: String) -> void:
	if OS.get_cmdline_user_args().find("--screenshot") < 0:
		return
	if index < 0 or index >= _panels.size():
		return
	var was: Array[bool] = []
	for p in _panels:
		was.append(p.visible)
		p.visible = false
	var panel: Control = _panels[index]
	panel.visible = true
	panel.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	panel.size = get_viewport().get_visible_rect().size
	# Above MapView and the debug overlay, which are not in _panels and would
	# otherwise draw over the panel being photographed.
	var z_was: int = panel.z_index
	panel.z_index = 200
	if panel.has_method("refresh"):
		panel.refresh()
	await get_tree().process_frame
	print("[shot] %s rect=%s children=%d" % [tag, str(panel.get_rect()),
		panel.get_child_count()])
	await _maybe_screenshot(tag)
	panel.z_index = z_was
	for i in _panels.size():
		_panels[i].visible = was[i]

## Build and refresh MapView twice, so both the first-time path (atlas upload,
## batch creation) and the steady-state path (unchanged generation) run.
func _exercise_map_view() -> void:
	if _map_view == null:
		return
	_map_view.setup(host)
	_map_view.refresh()
	_map_view.refresh()
	var batches := 0
	var glyph_layers := 0
	var instances := 0
	for child in _map_view.get_children():
		if child is MultiMeshInstance2D:
			batches += 1
			instances += child.multimesh.instance_count
		elif str(child.name).begins_with("GlyphLayer"):
			glyph_layers += 1
	print("[map] batches=%d instances=%d glyph_layers=%d" % [batches, instances, glyph_layers])
	await _maybe_screenshot("map")
	# A second frame a beat later. Nothing in the game advances in between, so
	# every pixel that differs between the two is something the renderer moved
	# on its own -- sway, particles, a hit reaction. Diffing them is how you find
	# out whether something is animating that should be standing still.
	await get_tree().create_timer(0.45).timeout
	await _maybe_screenshot("map2")
	_dump_light_pass()

## The light pass (SP-3, SP-4). Two things can go wrong silently here: the
## texture never gets built, or it gets built uniform -- and a uniform light
## texture looks exactly like the flat tint it replaced. So report the histogram,
## not just the size.
func _dump_light_pass() -> void:
	if not host.has_method("get_light_image"):
		print("[light] host has no light snapshot")
		return
	var img: Image = host.get_light_image()
	if img == null or img.get_width() <= 0:
		print("[light] no image (size=%s gen=%s)" % [str(host.get_light_size()),
			str(host.get_light_generation())])
		return
	var seen := 0
	var remembered := 0
	var unknown := 0
	var lmin := 255
	var lmax := 0
	var lsum := 0
	for y in img.get_height():
		for x in img.get_width():
			var c := img.get_pixel(x, y)
			var vis := int(round(c.r * 255.0))
			var lum := int(round(c.g * 255.0))
			if vis >= 200:
				seen += 1
				lmin = mini(lmin, lum)
				lmax = maxi(lmax, lum)
				lsum += lum
			elif vis > 0:
				remembered += 1
			else:
				unknown += 1
	print("[light] gen=%s size=%s seen=%d remembered=%d unknown=%d" % [
		str(host.get_light_generation()), str(img.get_size()), seen, remembered, unknown])
	print("[light] light over seen tiles: min=%d max=%d avg=%d" % [
		lmin if seen > 0 else 0, lmax, (lsum / seen) if seen > 0 else 0])
	var levels := {}
	for y in img.get_height():
		for x in img.get_width():
			var c := img.get_pixel(x, y)
			if c.r > 0.75:
				levels[int(round(c.g * 255.0))] = true
	print("[light] distinct light levels over seen tiles = %d %s"
		% [levels.size(), str(levels.keys())])
	print("[light] pass announced by MapView = %s"
		% str(_map_view != null and _map_view._light_pass_announced))
	var fire := 0
	for y in img.get_height():
		for x in img.get_width():
			if img.get_pixel(x, y).b > 0.0:
				fire += 1
	print("[light] fire-lit tiles (B channel, after blur) = %d" % fire)
	print("[light] wind = %s" % str(host.get_wind_vector()
		if host.has_method("get_wind_vector") else "<missing>"))
	_check_shaders()

## Scrolling combat text, end to end through a real fight.
##
## The whole reason this went unnoticed for so long is that a fresh character
## stands alone in an evac shelter, so nothing the probe could do produced
## combat. debug_spawn_monster puts a zombie adjacent; walking into it is a
## melee attack, which is what makes the game generate SCT. Nothing here fakes
## the text -- if the numbers appear, the real path produced them.
func _exercise_combat_text() -> void:
	if not host.has_method("debug_spawn_monster"):
		print("[sct] host has no debug_spawn_monster")
		return
	var err := str(host.debug_spawn_monster("mon_zombie"))
	print("[sct] spawn = %s" % ("<queued>" if err == "" else err))
	# The command runs on the game thread at its next input wait.
	await get_tree().create_timer(0.8).timeout

	# Safe mode stops the player dead the moment the zombie is spotted, so every
	# movement key is swallowed and no attack ever happens. Turn it off first.
	_press_shifted(KEY_1, "!")
	await get_tree().create_timer(0.4).timeout

	# Walk into each neighbouring tile: one of them holds the zombie, and moving
	# into an enemy is an attack. Keep going until a blow actually lands rather
	# than pressing a fixed number of times -- where the zombie spawns and
	# whether it closes are both up to the game, so a fixed sequence produces a
	# fight only sometimes, and a run with no fight in it reports "no combat
	# text" while looking exactly like a pass.
	var dirs := [KEY_LEFT, KEY_UP, KEY_RIGHT, KEY_DOWN]
	var attempts := 0
	while attempts < 24 and _sct_seen.is_empty():
		_press(dirs[attempts % dirs.size()])
		await get_tree().create_timer(0.3).timeout
		attempts += 1

	if _sct_seen.is_empty():
		var hits := 0
		if host.has_method("get_anim_stats"):
			hits = int((host.get_anim_stats() as Dictionary).get("hits_added", 0))
		# Say which of the two it is. "No combat text" is a bug; "no combat" is a
		# fixture that failed to set the scene, and reading the second as the
		# first is how a working feature gets reverted.
		print("[sct] FIXTURE DID NOT PRODUCE COMBAT after %d attempts (hits=%d) -- "
			% [attempts, hits] + "this run proves nothing about combat text")
	else:
		print("[sct] after fixture: %d run(s) seen, in %d attempts"
			% [_sct_seen.size(), attempts])

	# Publishing is only half of it. The overlay draws combat text from
	# anim_overlay.gd, which is refreshed by MapView -- and MapView is exercised
	# earlier in this run, before any fighting, so _draw_combat_text would
	# otherwise never execute even once. Drive it here, while text is live.
	if _map_view != null:
		_map_view.refresh()
		await get_tree().process_frame
		await get_tree().process_frame
		var overlay = _map_view.get_node_or_null("AnimOverlay")
		print("[sct] overlay drew with %d text run(s)"
			% (overlay._texts.size() if overlay != null else -1))

## Character overlays: clothing, the weapon in hand, mutations, effects.
##
## The avatar starts dressed, so unlike sway and particles this one is exercised
## by the ordinary spawn. What matters is the pairing -- what the game asked for
## against what the tileset had -- because a slot with no art is normal and a
## slot the resolver fumbles is not, and they look identical from the outside.
func _dump_character_overlays() -> void:
	if not host.has_method("get_avatar_overlays"):
		print("[worn] host has no get_avatar_overlays")
		return
	var rows: Array = host.get_avatar_overlays()
	var drawn := 0
	for row in rows:
		if bool(row.get("drawn", false)):
			drawn += 1
	print("[worn] %d overlay slots, %d resolved to a sprite" % [rows.size(), drawn])
	for row in rows:
		print("[worn]   %-34s -> %s" % [
			str(row.get("slot", "")),
			str(row.get("sprite", "")) if bool(row.get("drawn", false)) else "<no art>"])

## Fire and smoke particles (SP-6) with a synthetic field list.
##
## Nothing burns at a fresh spawn, so left alone this code would ship without
## having run a single line. Driving it with made-up emitters at least proves
## the materials build and the pool works, which is the half of it a machine
## with no display can be told anything about.
func _exercise_particles() -> void:
	if _map_view == null:
		return
	var particles: Node2D = _map_view.get_node_or_null("FieldParticles")
	if particles == null:
		print("[fx] no FieldParticles node")
		return
	var fake := PackedInt32Array([
		320, 320, 0, 3,   # fire, intensity 3
		352, 320, 0, 1,   # fire, intensity 1
		320, 352, 1, 2,   # smoke, intensity 2
	])
	particles.refresh(fake, Vector2i(32, 32), Vector2(0.4, -0.2))
	var emitting := 0
	for child in particles.get_children():
		if child is GPUParticles2D and child.emitting:
			emitting += 1
	print("[fx] synthetic fields: %d nodes, %d emitting" % [
		particles.get_child_count(), emitting])
	# And the other half of the pool contract: fewer emitters next frame must
	# quiet the surplus rather than leave it running.
	particles.refresh(PackedInt32Array(), Vector2i(32, 32), Vector2.ZERO)
	var still := 0
	for child in particles.get_children():
		if child is GPUParticles2D and child.emitting:
			still += 1
	print("[fx] after empty list: %d still emitting (want 0)" % still)

## Hit reactions (SP-5) with a synthetic hit, for the same reason: nothing
## attacks the player in the first few turns of a probe run.
func _exercise_hit_reaction() -> void:
	if _map_view == null or _map_view._creature_slots.is_empty():
		print("[fx] no creature instances to react")
		return
	var key = _map_view._creature_slots.keys()[0]
	var entry: Dictionary = _map_view._creature_slots[key][0]
	# Aim the hit at the tile that instance is standing on, worked back from the
	# anchor the same way MapView.hit_response does.
	var anchor: Vector2 = entry["anchor"]
	var origin: Vector2i = _map_view._view_origin
	var tsize: Vector2i = _map_view._tile_size
	# Built component-wise: inferring Vector2i arithmetic from members reached
	# through an untyped node is fragile, and the parser does reject it.
	var tx: int = int(anchor.x / float(tsize.x)) + origin.x
	var ty: int = int(anchor.y / float(tsize.y)) - 1 + origin.y
	_map_view._hits = [{
		"tile": Vector2i(tx, ty), "dir": Vector2(1, 0),
		"flash": Color(1, 0.3, 0.3, 1), "t": _map_view.HIT_DURATION * 0.5,
	}]
	_map_view._apply_hit_reactions()
	# Asking MapView what it applied, rather than reading the transform back out
	# of the MultiMesh: a read-back goes through the rendering driver, and the
	# dummy driver a headless run uses answers identity for every instance.
	var during: Dictionary = _map_view.hit_response(anchor)
	print("[fx] hit reaction at tile %s: offset %.1f px flash %.2f (want both > 0)"
		% [str(Vector2i(tx, ty)), (during["offset"] as Vector2).length(),
		float(during["flash"])])
	_map_view._hits = []
	_map_view._apply_hit_reactions()
	var after: Dictionary = _map_view.hit_response(anchor)
	print("[fx] after hit ends: offset %.1f px (want 0)"
		% (after["offset"] as Vector2).length())

## Sway (SP-7), palettes (SP-8), particles (SP-6) and hit reactions (SP-5) all
## express themselves as counts of things the C++ side published, which is the
## only part of them a machine with no display can check.
func _dump_effects() -> void:
	var sway := 0
	var paletted := 0
	var cmds: PackedInt32Array = host.get_map_draw_list()
	var i := 0
	while i + 9 < cmds.size():
		var flags: int = cmds[i + 9]
		if (flags & 0x4) != 0:
			sway += 1
		if (flags & 0xF0) != 0:
			paletted += 1
		i += 10
	print("[fx] sway commands=%d palette-swapped commands=%d" % [sway, paletted])
	if host.has_method("get_palette_image"):
		var pal: Image = host.get_palette_image()
		print("[fx] palette texture = %s"
			% (str(pal.get_size()) if pal != null else "<none>"))
	if host.has_method("get_map_field_list"):
		var fl: PackedInt32Array = host.get_map_field_list()
		print("[fx] field emitters = %d" % (fl.size() / 4))
	if host.has_method("get_hit_generation"):
		print("[fx] hits so far = %d" % host.get_hit_generation())
	if _map_view != null:
		var batches := 0
		var particles := 0
		for child in _map_view.get_children():
			if child is MultiMeshInstance2D:
				batches += 1
			elif str(child.name) == "FieldParticles":
				particles = child.get_child_count()
		print("[fx] tile batches=%d particle emitters built=%d" % [batches, particles])
	_exercise_particles()
	_exercise_hit_reaction()
	_dump_character_overlays()
	await _exercise_combat_text()
	# Sway and palettes only show up in the draw list when the player happens to
	# be looking at grass or at a fungal zombie, which at a fresh evac-shelter
	# spawn they are not. Ask the resolver directly instead, so the wiring is
	# checked rather than the starting location.
	if host.has_method("describe_sprite"):
		# The grass pair is the regression check for the tearing bug: t_grass is
		# a 32x32 ground cell and must not shear, t_grass_tall is 32x64 and may.
		for probe in [["t_grass", "terrain"], ["t_grass_tall", "terrain"],
				["t_shrub", "terrain"], ["t_tree_young", "terrain"],
				["t_tree", "terrain"], ["t_wall", "terrain"],
				["mon_zombie_dusted", "monster"], ["mon_zombie_fungalize", "monster"],
				["mon_zombie", "monster"], ["mon_totally_made_up", "monster"]]:
			var d: Dictionary = host.describe_sprite(probe[0], probe[1])
			print("[fx] %-18s -> %-18s %-11s palette=%s veg=%-5s overhangs=%-5s sway=%s" % [
				probe[0], str(d.get("resolved", "")), str(d.get("level_name", "")),
				str(d.get("palette_row", 0)), str(d.get("vegetation", false)),
				str(d.get("overhangs_cell", false)), str(d.get("sways", false))])

## Shader source is not GDScript: nothing else in this harness would notice a
## typo in it. Loading the resource parses the source, and a shader that failed
## to parse reports no uniforms -- so the count is the check.
func _check_shaders() -> void:
	for path in ["res://shaders/map_tiles.gdshader"]:
		var sh: Shader = load(path)
		if sh == null:
			print("[shader] %s FAILED TO LOAD" % path)
			continue
		var names: Array = []
		for u in sh.get_shader_uniform_list():
			names.append(str(u.get("name", "?")))
		names.sort()
		print("[shader] %s uniforms=%d %s" % [path, names.size(), str(names)])

## The sprite coverage report (SP-2) and render stats (SP-9). With a tileset that
## is missing most of the game's art this is the whole point: it says which of
## the thousands of absent ids the player is actually looking at.
func _dump_render_stats() -> void:
	if not host.has_method("get_render_stats"):
		print("[render] host has no get_render_stats")
		return
	var st: Dictionary = host.get_render_stats()
	print("[render] tileset=%s atlases=%s tile=%s view=%s cmds=%s glyphs=%s missing_ids=%s" % [
		str(st.get("tileset", "")), str(st.get("atlases", 0)), str(st.get("tile_size", "")),
		str(st.get("view_size", "")), str(st.get("commands", 0)), str(st.get("glyphs", 0)),
		str(st.get("missing_ids", 0))])
	print("[render] by_fallback=%s" % str(st.get("by_fallback", {})))
	print("[render] by_layer=%s" % str(st.get("by_layer", [])))
	var cov: Array = host.get_sprite_coverage(10)
	print("[render] top sprite misses (%d shown):" % cov.size())
	for row in cov:
		print("[render]   %-28s %-13s %s x%s" % [str(row.get("id", "")),
			str(row.get("category", "")), str(row.get("level_name", "")),
			str(row.get("hits", 0))])

func _shutdown() -> void:
	if host.has_method("get_anim_stats"):
		print("[sct] anim chain: %s" % str(host.get_anim_stats()))
	print("[sct] %d combat text run(s) observed over the whole run" % _sct_seen.size())
	for row in _sct_seen:
		print("[sct]   %s" % row)

	# Order matters: request_quit first, so a game thread parked in a menu sees
	# the shutdown flag and exits there instead of cancelling and running on into
	# game logic while the extension is being torn down.
	host.request_quit()
	if host.has_method("uilist_cancel"):
		host.uilist_cancel()
	get_tree().quit(0)

## Press a shifted key the way a real one arrives: keycode is the unshifted key,
## unicode is the produced character, and shift is reported held. This is the
## combination that used to match no binding at all.
func _press_shifted(keycode: int, produced: String) -> void:
	var down := InputEventKey.new()
	down.keycode = keycode
	down.physical_keycode = keycode
	down.pressed = true
	down.shift_pressed = true
	down.unicode = produced.unicode_at(0)
	host.push_input_event(down)

func _press(keycode: int) -> void:
	var down := InputEventKey.new()
	down.keycode = keycode
	down.physical_keycode = keycode
	down.pressed = true
	if keycode >= KEY_SPACE and keycode <= KEY_ASCIITILDE:
		down.unicode = char(keycode).to_lower().unicode_at(0)
	host.push_input_event(down)

func _report(label: String) -> void:
	var cols: int = host.get_view_cols()
	var rows: int = host.get_view_rows()
	var cells: PackedInt32Array = host.get_view_cells()
	if cols <= 0 or rows <= 0 or cells.size() < cols * rows * CELL_STRIDE:
		print("[probe] %s: no cell data" % label)
		return
	var occupied := 0
	var glyphs := 0
	for n in cols * rows:
		if cells[n * CELL_STRIDE + 3] != 0:
			occupied += 1
		if cells[n * CELL_STRIDE] > 32:
			glyphs += 1
	if host.has_method("uilist_active") and host.uilist_active():
		var u: Dictionary = host.get_uilist_state()
		var urows: Array = u.get("entries", [])
		print("[uilist] ACTIVE title=%s entries=%d selected=%d desc=%s filter=%s"
			% [str(u.get("title", "")), urows.size(), int(u.get("selected", -1)),
			str(u.get("desc_enabled", false)), str(u.get("filtering", false))])
		for i in mini(3, urows.size()):
			print("[uilist]   [%s] %s" % [str(urows[i].get("hotkey", "")),
				str(urows[i].get("text", ""))])
		# Drive it the way the panel does.
		var p3: Control = _panels[4]
		p3.refresh()
	var lg: Array = host.get_hud_state().get("messages", [])
	var last := str(lg[lg.size() - 1].get("text", "")) if not lg.is_empty() else ""
	print("[probe] %s: occupied=%d/%d glyphs=%d | t=%s | log: %s"
		% [label, occupied, cols * rows, glyphs,
		str(host.get_hud_state().get("time", "")), last])
	# Glyph + background map of the band a centred menu lands in.
	for y in range(int(rows * 0.3), int(rows * 0.72)):
		var text := ""
		var bgmap := ""
		var any := false
		for x in range(45, mini(cols, 125)):
			var i: int = (y * cols + x) * CELL_STRIDE
			var cp: int = cells[i]
			var bg: int = cells[i + 2]
			if cp > 32:
				any = true
			text += String.chr(cp) if cp > 32 and cp < 0x10FFFF else " "
			bgmap += "." if bg == 0 else String.num_int64(bg, 16)
		if any:
			print("  %2d |%s|  bg |%s|" % [y, text, bgmap])
