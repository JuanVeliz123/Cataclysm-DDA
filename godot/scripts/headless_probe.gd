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
var _talked := false
var _dialogue_seen := 0
var _shot_dialogue := false
var _om_lines := 0
var _surr_rows := 0

## VER-0. Three features shipped computed-and-discarded, and two fixtures ran
## against nothing, and every one of those runs exited 0. A harness that cannot
## tell "verified" from "never ran" reports the same green for both, and green is
## the answer nobody looks at twice.
##
## Two checks, both cheap. A stage that never executed proved nothing, and a
## generation counter that never moved means the producer's output reached no
## consumer -- the signature of the dead frame boundary. See
## docs/godot_migration/AGENT_HANDOFF.md.
var _stages := {}

## Stages that must run in any session. A miss here is a failure, not a note:
## it means the rest of the log is describing less than it appears to.
const REQUIRED_STAGES := ["session", "panels", "map_view", "world_viewport",
	"map_view_3d", "grab", "dialogue", "overmap_sidebar", "surroundings", "options",
	"keybind", "crafting"]

## Counters that must move in any session, because this fixture drives every one
## of them itself.
const REQUIRED_GENERATIONS := ["get_map_generation", "get_light_generation",
	"popup_generation", "options_layout_generation", "keybind_generation",
	"crafting_list_generation", "dialogue_generation", "surroundings_generation"]

## Counters that depend on the world rolling the right way -- a uilist only
## appears if the game decides to open one, and no NPC turned up. Reported so a
## quiet run is legible, never failed on: a check that cries wolf gets ignored,
## and then it is worth nothing when it is right.
const OPTIONAL_GENERATIONS := ["uilist_generation", "textwin_generation",
	"get_anim_generation", "get_overmap_generation"]

## Stages that need the world to cooperate. Reported at shutdown so a run that
## skipped one is legible, but never failed on -- this fixture cannot conjure an
## NPC to talk to.
const OPPORTUNISTIC_STAGES: Array[String] = []

func _stage(name: String) -> void:
	_stages[name] = true
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
			"res://scripts/keybind_panel.gd", "res://scripts/crafting_panel.gd",
			"res://scripts/dialogue_panel.gd", "res://scripts/surroundings_panel.gd"]:
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
	_stage("panels")
	print("[probe] panels refreshed: %d" % _panels.size())
	await _shoot_solo(0, "sidebar")
	await _shoot_solo(1, "inventory")
	await _shoot_solo(2, "character")
	await _shoot_solo(3, "gamemenu")
	# MapView asks for the tiles its viewport covers; check the request lands and
	# the published extent follows it, rather than staying at the curses grid.
	# Ordered by how much the world has to cooperate, quietest first. Each stage
	# leaves the avatar somewhere less pristine than it found it -- the NPC walk
	# wakes safe mode, a hostile conversation leaves prompts behind -- so a stage
	# that needs a calm game goes before the ones that disturb it. Running the
	# surroundings list last is how it came to be reported broken twice while
	# being fine.
	await _probe_surroundings()
	await _probe_overmap_sidebar()
	await _probe_grab_prompt()
	_stage("grab")
	await _summon_someone_to_talk_to()
	_dump_sidebar_layout()
	print("[map] extent before request = %s" % str(host.get_map_view_size()))
	host.set_map_view_tiles(101, 61)
	await get_tree().create_timer(0.6).timeout
	print("[map] extent after request  = %s (want 101x61)" % str(host.get_map_view_size()))
	await _probe_world_viewport()
	_exercise_map_view()
	# After the 2D backend, and against the same draw list: the two are meant to
	# agree, and the comparison only means anything if both saw the same frame.
	_probe_map_view_3d()
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
			_stage("options")
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
			_stage("keybind")
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
		# Every fortieth try, clear anything blocking first -- same reason as the
		# overmap: a prompt eats the key and the screen is blamed for not opening.
		if _craft_tries % 40 == 20 and host.has_method("popup_active") and host.popup_active():
			_press(KEY_ESCAPE)
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
				_stage("crafting")
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
		var u: Dictionary = host.get_uilist_state()
		var title := str(u.get("title", ""))
		# An NPC menu is the only way this fixture can reach a conversation --
		# nothing here can conjure an NPC, so dialogue coverage is opportunistic
		# and reported rather than required.
		if not _talked and title.findn("what to do with") >= 0:
			for e in (u.get("entries", []) as Array):
				if str(e.get("text", "")).findn("talk") >= 0:
					_talked = true
					print("[dlg] taking '%s' from '%s'" % [str(e.get("text", "")), title])
					host.uilist_confirm(int(e.get("index", -1)))
					break
		if title != _uilist_title:
			_uilist_title = title
			_uilist_frames = 0
		_uilist_frames += 1
		if _uilist_frames == 120:
			print("[uilist] cancelling unattended '%s'" % title)
			host.uilist_cancel()
	if host != null and host.has_method("dialogue_active") and host.dialogue_active():
		if _panels.size() > 10:
			_panels[10].refresh()
		_dialogue_seen += 1
		if _dialogue_seen == 15 and not _shot_dialogue:
			_shot_dialogue = true
			_stage("dialogue")
			_dump_dialogue()
			await _shoot_solo(10, "dialogue")
		# Move the cursor, then leave. Confirming a real response would take the
		# conversation somewhere unpredictable and could start a trade or a
		# fight, which the rest of the run then has to survive.
		if _dialogue_seen == 30:
			host.dialogue_action("DOWN")
		if _dialogue_seen == 45:
			print("[dlg] after DOWN: selected=%d" % int(
				(host.get_dialogue_state() as Dictionary).get("selected", -1)))
		if _dialogue_seen == 60:
			host.dialogue_action("QUIT")
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
				_stage("session")
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

## Open the surroundings list and read it back. 'v' is the binding; the list is
## whatever happens to be nearby, so the check is that it opened and published
## tabs, not that it found any particular thing.
## Press a key that opens a screen, then *wait* for it rather than pressing
## again. These keys toggle: pressing six times in a row opens and closes the
## screen three times, and whether the check lands on an open one is luck. Both
## the overmap and the surroundings list were reported as never opening for
## exactly this reason -- they had opened, and been closed again by the retry.
func _open_screen(key: int, active_probe: String, label: String) -> bool:
	for attempt in 3:
		_press(key)
		# Generous: the game thread can be seconds behind under load, and being
		# slow to notice costs nothing next to pressing the key again.
		for tick in 24:
			await get_tree().create_timer(0.5).timeout
			if host.has_method(active_probe) and host.call(active_probe):
				return true
		await _clear_blocking_popup()
	print("[probe] %s did not open after 3 presses" % label)
	return false

func _probe_surroundings() -> void:
	# Entry and exit are traced because this chain runs detached: _process calls
	# _exercise_panels() without awaiting it, so a stage that stalls inside an
	# await produces no output at all and is indistinguishable from one that was
	# never reached. Four runs were spent not knowing which.
	print("[surr] stage entered")
	if not host.has_method("get_surroundings_state"):
		print("[surr] SKIPPED: no get_surroundings_state binding")
		return
	await _wait_until_idle()
	print("[surr] screen stack clear")
	await _clear_blocking_popup()
	print("[surr] popups clear; commands_ready=%s" % (
		str(host.commands_ready()) if host.has_method("commands_ready") else "?"))
	# Opened by keypress, and only by keypress. There was a debug command that
	# invoked list_surroundings() directly, and it wedged the game: a posted
	# command runs inside the input wait, so opening a modal screen from one
	# parks the game thread in that screen's loop, and nothing in a fixture run
	# ever dismisses it. The clock stops and every later stage fails.
	if not await _open_screen(KEY_V, "surroundings_active", "the surroundings list"):
		print("[surr] FAILED: the surroundings list never opened")
		return
	if _panels.size() > 11:
		_panels[11].refresh()
	var d: Dictionary = host.get_surroundings_state()
	var tabs: Array = d.get("tabs", [])
	var rows: Array = d.get("rows", [])
	_surr_rows = rows.size()
	print("[surr] tabs=%d rows=%d selected=%d generation=%d" % [
		tabs.size(), rows.size(), int(d.get("selected", -1)),
		int(host.surroundings_generation())])
	for t in tabs:
		print("[surr]   tab '%s' (%d)" % [str(t.get("title", "")), int(t.get("count", 0))])
	for i in mini(4, rows.size()):
		print("[surr]   %-40s %s" % [str(rows[i].get("text", "")).substr(0, 40),
			str(rows[i].get("distance", ""))])
	# Switching tab is the round trip worth proving: the panel sends NEXT_TAB and
	# the published tab index has to come back changed.
	var before := int(d.get("tab", -1))
	host.surroundings_action("NEXT_TAB")
	await get_tree().create_timer(1.0).timeout
	var after := int((host.get_surroundings_state() as Dictionary).get("tab", -1))
	print("[surr] NEXT_TAB: tab %d -> %d (%s)" % [before, after,
		"changed" if after != before else "UNCHANGED"])
	if tabs.size() > 0:
		_stage("surroundings")
	host.surroundings_action("QUIT")
	await get_tree().create_timer(1.0).timeout

## Open the overmap and read the sidebar the panel is now responsible for.
##
## The map itself has been a Godot node for a while; the sidebar beside it was
## still an ImGui window over the top. Reading it back proves the recording pass
## produced the same text the terminal drew, rather than an empty panel next to a
## working map -- which is exactly what "renders fine" looked like last time.
func _probe_overmap_sidebar() -> void:
	if not host.has_method("get_overmap_sidebar"):
		return
	# Two ways the m gets eaten, and both were mistaken for the sidebar failing.
	# The conversation started by the previous stage is still up and owns the
	# keyboard, and stepping around wakes safe mode, whose prompt swallows the
	# next key. Wait out the first, clear the second.
	await _wait_until_idle()
	await _clear_blocking_popup()
	if not await _open_screen(KEY_M, "overmap_active", "the overmap"):
		print("[omap] FAILED: the overmap never opened")
		return
	var d: Dictionary = host.get_overmap_sidebar()
	var lines: Array = d.get("lines", [])
	_om_lines = lines.size()
	var headers := 0
	for l in lines:
		if bool(l.get("header", false)):
			headers += 1
	print("[omap] sidebar lines=%d headers=%d generation=%d" % [
		lines.size(), headers, int(host.overmap_sidebar_generation())])
	for i in mini(6, lines.size()):
		print("[omap]   %s%s%s" % [
			"# " if bool(lines[i].get("header", false)) else "  ",
			"+ " if bool(lines[i].get("join", false)) else "",
			str(lines[i].get("text", "")).substr(0, 80)])
	if lines.is_empty():
		print("[omap] FAILED: the overmap is open but the sidebar published nothing")
	else:
		_stage("overmap_sidebar")
	# Confirm it actually closed rather than assuming one Escape did it. The next
	# stage presses a key that means something different on this screen.
	for attempt in 6:
		_press(KEY_ESCAPE)
		await get_tree().create_timer(0.8).timeout
		if not (host.has_method("overmap_active") and host.overmap_active()):
			return
	print("[omap] the overmap would not close; later stages will see its keymap")

## Put an NPC next to the avatar and step into them, which opens the interaction
## menu; the uilist fixture then takes its Talk entry. Without this, dialogue
## coverage depends on the world happening to offer somebody, which is a coin
## flip rather than a check -- and MENU-10 shipped unverified because of it.
func _summon_someone_to_talk_to() -> void:
	if not host.has_method("debug_spawn_npc"):
		return
	var err := str(host.debug_spawn_npc())
	print("[dlg] spawn npc = %s" % ("<accepted>" if err == "" else err))
	await get_tree().create_timer(1.5).timeout
	# Step into each neighbour in turn; whichever one they are standing on opens
	# the menu, and the others are just a step and a step back.
	# Stepping into them may open the interaction menu, or go straight into
	# conversation -- both count, and only one of them is a uilist. Checking for
	# the menu alone reported failure on a run where the conversation had already
	# started, which is a fixture crying wolf: the thing VER-0 exists to avoid.
	# Three passes, not one: the spawn prefers an adjacent tile but widens when
	# the avatar is walled in, and one step in each direction only reaches the
	# adjacent case. A sweep that gives up too early reports "met nobody" for a
	# spawn that worked.
	for pass_no in 3:
		for key in [KEY_KP_6, KEY_KP_4, KEY_KP_8, KEY_KP_2, KEY_KP_9, KEY_KP_7,
				KEY_KP_3, KEY_KP_1]:
			if _reached_an_npc():
				return
			_press(key)
			await get_tree().create_timer(0.4).timeout
	if not _reached_an_npc():
		# The spawn reported success, so somebody exists; the walk did not get to
		# them. Say that, rather than blaming the spawn for a failure upstream of
		# where it actually happened.
		print("[dlg] spawned, but 24 steps did not reach them")

## Wait for the screens other stages opened to close before pressing anything.
## A key sent while a Godot panel owns the keyboard goes to the panel, and the
## screen that never opened gets the blame.
func _wait_until_idle() -> void:
	for i in 60:
		var busy := false
		# The overmap belongs on this list as much as the panels do: it is a
		# screen with its own input context, and a key sent while it is up is
		# interpreted by it. Leaving it out is why the surroundings list appeared
		# not to open -- the v went to the map.
		for probe in ["dialogue_active", "uilist_active", "crafting_active",
				"options_active", "keybind_active", "overmap_active",
				"surroundings_active"]:
			if host.has_method(probe) and host.call(probe):
				busy = true
				break
		if not busy:
			return
		await get_tree().create_timer(0.5).timeout
	print("[probe] gave up waiting for the screen stack to clear")

## Escape whatever is on screen so the next keypress reaches the game. Used
## before opening a screen: a pending prompt silently eats the key, and the
## screen is then blamed for never appearing.
func _clear_blocking_popup() -> void:
	for i in 4:
		if not (host.has_method("popup_active") and host.popup_active()):
			return
		_press(KEY_ESCAPE)
		await get_tree().create_timer(0.4).timeout

func _reached_an_npc() -> bool:
	if host.has_method("dialogue_active") and host.dialogue_active():
		return true
	return host.has_method("uilist_active") and host.uilist_active()

## The reported bug: "Grab where?" renders correctly but never goes away.
##
## The first version of this checked host.popup_active() and reported DISMISSED,
## because the *game* does dismiss correctly -- it was the panel that never
## cleared. Checking the producer and calling it proof is the mistake this
## migration keeps making, so this drives the panel exactly as host.gd does and
## asserts on what is actually on screen.
func _probe_grab_prompt() -> void:
	var panel: Control = _panels[5] if _panels.size() > 5 else null
	if panel == null:
		return
	_press_shifted(KEY_G, "G")
	await get_tree().create_timer(1.0).timeout
	_pump_popup_panel(panel)
	print("[grab] after G: game=%s panel=%s notice='%s'" % [
		str(host.popup_active()), str(panel.showing()),
		str((host.get_popup_state() as Dictionary).get("notice", ""))])
	if not panel.showing():
		print("[grab] INCONCLUSIVE: the prompt never reached the panel")
		return
	_press(KEY_ESCAPE)
	await get_tree().create_timer(1.0).timeout
	_pump_popup_panel(panel)
	var game_clear := not bool(host.popup_active())
	var panel_clear := not bool(panel.showing())
	print("[grab] after ESC: game=%s panel=%s -> %s" % [
		"clear" if game_clear else "STUCK", "clear" if panel_clear else "STUCK",
		"DISMISSED" if (game_clear and panel_clear)
		else ("PANEL STUCK (game moved on, ribbon left behind)" if game_clear
			else "GAME STUCK (key never arrived)")])

## Mirror host.gd's _update_popup_panel, including the refresh on the way down.
## Refreshing only while active is exactly the bug this catches.
func _pump_popup_panel(panel: Control) -> void:
	if host.popup_active() or panel.showing():
		panel.refresh()

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

func _dump_dialogue() -> void:
	var d: Dictionary = host.get_dialogue_state()
	var history: Array = d.get("history", [])
	var responses: Array = d.get("responses", [])
	print("[dlg] header='%s' history=%d responses=%d selected=%d" % [
		str(d.get("header", "")), history.size(), responses.size(),
		int(d.get("selected", -1))])
	for i in mini(3, history.size()):
		print("[dlg]   said: %s" % str(history[i].get("text", "")).substr(0, 90))
	for i in mini(4, responses.size()):
		print("[dlg]   [%s] %s" % [str(responses[i].get("hotkey", "")),
			str(responses[i].get("text", "")).substr(0, 80)])

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
	await _maybe_screenshot_of(get_viewport(), tag)

## Screenshot a specific viewport, under the same --screenshot gate.
##
## Exists because "the map" and "what players see" are different viewports here:
## the probe's screenshots captured the root (the 2D MapView and the panels)
## while the shipping renderer draws into a SubViewport that was never captured
## -- so no screenshot showed the 3D backend, the tilt, or a creature mesh.
func _maybe_screenshot_of(vp: Viewport, tag: String) -> void:
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
	var img: Image = vp.get_texture().get_image()
	if img == null:
		print("[shot] no viewport image")
		return
	var path: String = args[at + 1]
	if tag != "":
		path = path.get_basename() + "." + tag + "." + path.get_extension()
	var err := img.save_png(path)
	print("[shot] %s -> %s (%s, err=%d)" % [tag, path, str(img.get_size()), err])

## Screenshot the 3D probe's SubViewport, then free it.
##
## A SubViewport nothing composites does not render on its own -- its update mode
## waits for visibility that never comes -- so it is switched to UPDATE_ALWAYS for
## the two frames the capture needs. Freed here rather than in the stage so the
## stage can stay synchronous; under a headless display server the capture is
## skipped and this is just a deferred free.
func _finish_map_view_3d(sub: SubViewport) -> void:
	sub.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	await _maybe_screenshot_of(sub, "map3d")
	sub.queue_free()

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
	_stage("map_view")
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
	_check_z_budget()
	var shadows = _map_view.get_node_or_null("ShadowLayer")
	print("[map] contact shadows = %s (strength %.2f)" % [
		str(shadows._blobs.size()) if shadows != null else "no layer",
		float(shadows._strength) if shadows != null else 0.0])
	await _maybe_screenshot("map")
	# A second frame a beat later. Nothing in the game advances in between, so
	# every pixel that differs between the two is something the renderer moved
	# on its own -- sway, particles, a hit reaction. Diffing them is how you find
	# out whether something is animating that should be standing still.
	await get_tree().create_timer(0.45).timeout
	await _maybe_screenshot("map2")
	_dump_light_pass()
	if host.has_method("get_conditions"):
		print("[grade] conditions = %s" % str(host.get_conditions()))
	if _map_view != null:
		var env = _map_view.get_node_or_null("WorldEnvironment")
		# Reports the *attached* environment, which is the thing that decides
		# whether the glow is running -- WorldEnvironment has no `visible`.
		print("[grade] world environment: %s glow=%s threshold=%s" % [
			"attached" if (env != null and env.environment != null) else "detached",
			str(env.environment.glow_enabled) if env != null else "-",
			str(env.environment.glow_hdr_threshold) if env != null else "-"])

## The world's own viewport (3D-0 / ADR-004): is the boundary actually there, is
## it the size of the drawable area, and does it disappear with the world?
##
## Run against a throwaway world rather than against the probe's MapView. What is
## being checked is the container -- its sizing, its visibility mirror, and that
## it takes no input -- and reparenting the live MapView mid-run would change the
## viewport every later stage measures itself against.
##
## The visibility mirror is the part worth a check. Half a dozen places in
## host.gd set `map_view.visible`, none of them know this container exists, and a
## container left visible over a hidden world composites the last frame the world
## drew: the main menu would open over a still of the map.
func _probe_world_viewport() -> void:
	var rect := TextureRect.new()
	rect.name = "WorldViewportProbe"
	rect.set_script(load("res://scripts/world_viewport.gd"))
	add_child(rect)
	var world := Node2D.new()
	world.name = "ProbeWorld"
	add_child(world)
	rect.setup(world)
	var sub: SubViewport = rect.world_viewport()
	var logical := get_viewport().get_visible_rect().size
	const RESERVE := 200.0
	rect.set_reserved_right(RESERVE)
	await get_tree().process_frame
	var want := Vector2(logical.x - RESERVE, logical.y)
	print("[world] inside the viewport = %s" % str(sub != null and world.get_parent() == sub))
	print("[world] rect %s, want %s (viewport %s less %d reserved)" % [
		str(rect.size), str(want), str(logical), int(RESERVE)])
	if not rect.size.is_equal_approx(want):
		push_error("world viewport is %s, drawable area is %s" % [str(rect.size), str(want)])
	# The two sizes that must differ when the canvas is stretched, and the reason
	# this check exists: the render target is in device pixels and the 2D override
	# is in logical units. Getting that backwards renders the world at the
	# stretch's base resolution and lets it be upscaled -- which looks like
	# nothing in the log and like a softer game on screen.
	if sub != null:
		var win := get_window()
		var expect := Vector2(want.x * float(win.size.x) / logical.x,
			want.y * float(win.size.y) / logical.y)
		print("[world] render target %s (device, expect ~%s), 2d override %s (logical)" % [
			str(sub.size), str(expect), str(sub.size_2d_override)])
		if Vector2(sub.size).distance_to(expect) > 2.0:
			push_error("world render target %s is not the drawable area in device pixels (%s)"
				% [str(sub.size), str(expect)])
		if sub.size_2d_override != Vector2i(want):
			push_error("world 2d override %s is not the drawable area in logical units (%s)"
				% [str(sub.size_2d_override), str(Vector2i(want))])
	# Takes no input, which is what keeps the host's existing mouse routing -- and
	# the cell conversion that depends on window coordinates -- untouched.
	print("[world] mouse_filter = %d (ignore=%d), hdr=%s, transparent=%s" % [
		rect.mouse_filter, Control.MOUSE_FILTER_IGNORE,
		str(sub.use_hdr_2d) if sub != null and "use_hdr_2d" in sub else "n/a",
		str(sub.transparent_bg) if sub != null else "-"])
	world.visible = false
	await get_tree().process_frame
	print("[world] hidden world hides the composite = %s" % str(not rect.visible))
	if rect.visible:
		push_error("world viewport still composited with the world hidden")
	_stage("world_viewport")
	world.queue_free()
	rect.queue_free()

## The 3D backend (3D-1), built and refreshed against the same draw list.
##
## What can be checked without a GPU is narrow but it is the part most likely to be
## silently wrong: that the batches exist at all, that they hold as many instances
## as the draw list has commands, that they are stacked along z in the order
## `depth_rank` computes -- the flat world's whole depth model -- and that the camera
## is orthographic and pointing straight down, which is what makes the projection
## pixel-faithful.
##
## What it cannot answer is whether it *looks* the same. That is the milestone, and
## it needs `host.gd`'s USE_3D_MAP on, a real driver, and two screenshots.
func _probe_map_view_3d() -> void:
	# What C++ is publishing right now, to be put back at the end: this stage drives a
	# view of its own and the view asks for the extent that fits it, which would leave
	# the game publishing a different map to every stage that runs after this one.
	var extent_before: Vector2i = host.get_map_view_size()

	var sub := SubViewport.new()
	sub.name = "World3DProbe"
	sub.size = Vector2i(640, 480)
	sub.own_world_3d = true
	add_child(sub)
	var view := Node3D.new()
	view.name = "MapView3D"
	view.set_script(load("res://scripts/map_view_3d.gd"))
	# Hidden first, shown after setup, because that is the order host.gd uses -- the
	# world is built before a session exists and revealed when one starts. It matters:
	# `Camera3D.make_current()` does nothing while its subtree is outside a World3D,
	# and a Node3D subtree is only inside one while visible. Getting this wrong renders
	# the default clear colour and nothing else, which is what a flat grey map was.
	view.visible = false
	sub.add_child(view)
	# Synchronous from here on, deliberately. This stage used to `await` a frame in
	# the middle, and a GDScript runtime error inside a coroutine never resolves the
	# await -- so one bad line in here silently killed every fixture that
	# _exercise_panels dispatches after it, options and crafting included. A stage
	# that can fail must not be able to take its caller with it.
	print("[map3d] building (hidden, as host.gd does)")
	view.setup(host)
	print("[map3d] setup returned")
	view.refresh()
	view.visible = true
	view.refresh()
	print("[map3d] two refreshes returned")

	var stats: Dictionary = view.debug_stats()
	print("[map3d] batches=%d instances=%d depths=%d span=%.2f scissor=%.2f zoom=%.2f" % [
		int(stats.get("batches", 0)), int(stats.get("instances", 0)),
		int(stats.get("depths", 0)), float(stats.get("depth_span", 0.0)),
		float(stats.get("scissor", 0.0)), float(stats.get("zoom", 0.0))])
	# The light channel (3D-2). Reported, never failed on: whether the game is casting
	# any light at all depends on the time of day and on there being a lamp in view,
	# and a check that reddens at noon gets ignored. What it does catch is the shape --
	# a stride that does not divide, or a channel the library does not have.
	if host.has_method("get_light_sources"):
		var lights: PackedFloat32Array = host.get_light_sources()
		var stride: int = view.LIGHT_STRIDE
		print("[map3d] light sources published = %d (%d floats, stride %d)" % [
			int(lights.size() / stride), lights.size(), stride])
		if lights.size() % stride != 0:
			push_error("light channel is %d floats, which is not a multiple of the "
				% lights.size() + "stride %d" % stride)
	else:
		push_error("the library has no get_light_sources; rebuild the GDExtension")
	print("[map3d] camera size=%.1f at %s, light pass=%s" % [
		float(stats.get("camera_size", 0.0)), str(stats.get("camera_position", "-")),
		str(stats.get("light_pass", false))])

	# Every command the draw list carries has to become an instance somewhere. A
	# batch count on its own proves only that the loop ran.
	var cmds: PackedInt32Array = host.get_map_draw_list()
	var want_instances := int(cmds.size() / 10)
	var got_instances := int(stats.get("instances", 0))
	# A command whose creature is drawn as a mesh is left undrawn on purpose
	# (3D-7c), overlays included. Without this term the check reddened the moment
	# the first committed mesh existed -- 73 instances for 88 commands, all 15 of
	# them the meshed avatar's body and clothing.
	var meshed := int(stats.get("suppressed", 0))
	print("[map3d] draw list holds %d commands, batches hold %d instances (%d meshed)" % [
		want_instances, got_instances, meshed])
	if want_instances > 0 and got_instances != want_instances - meshed:
		push_error("3D backend built %d instances for %d commands (%d left to meshes)" % [
			got_instances, want_instances, meshed])

	# The batching claim, which is the point of 3D-1b and is a number rather than an
	# opinion: with the depth on each sprite instead of on each batch, the key is
	# down to the shader uniforms and the count should be about a dozen against the
	# 2D backend's hundreds. Reported both ways round, because "few batches" is only
	# good news if the depths are still there.
	var batches := int(stats.get("batches", 0))
	var depths := int(stats.get("depths", 0))
	print("[map3d] %d batches carrying %d distinct depths (2D backend needs one batch "
		% [batches, depths] + "per depth; this needs none)")
	if batches > 0 and depths < batches:
		push_error("3D backend has %d batches but only %d depths, so sprites that "
			% [batches, depths] + "should interleave cannot")
	# The four layers of 3D-1c, each of which exists for a different reason and any
	# of which could be silently missing: the shadows are geometry, the particles are
	# a ported node, and the glyphs and the overlay live on a canvas whose transform
	# has to track the camera.
	var shadows: MultiMeshInstance3D = view.get_node_or_null("ContactShadows")
	var fields := view.get_node_or_null("FieldParticles")
	var canvas: CanvasLayer = view.get_node_or_null("WorldCanvas")
	var overlay := canvas.get_node_or_null("AnimOverlay") if canvas != null else null
	print("[map3d] shadows=%s blobs=%d, particles=%s emitting=%d, canvas=%s overlay=%s" % [
		str(shadows != null), shadows.multimesh.instance_count if shadows != null else -1,
		str(fields != null), fields.active_emitters() if fields != null else -1,
		str(canvas != null), str(overlay != null)])
	if shadows == null or fields == null or canvas == null or overlay == null:
		push_error("3D backend is missing one of the 3D-1c layers")
	else:
		# The canvas has to be scaled by the same zoom the camera is showing, or the
		# glyphs and the overlay are drawn at a different size from the world they
		# annotate. Zoomed first, deliberately: at the default zoom of 1.0 an identity
		# transform passes this check while proving nothing, which is the shape of
		# test that reports green for code that never ran.
		view.zoom_step(1)
		var zoomed: Dictionary = view.debug_stats()
		var canvas_zoom := canvas.transform.get_scale().x
		var camera_zoom := float(zoomed.get("zoom", 0.0))
		print("[map3d] after one zoom step: canvas scale=%.3f, camera zoom=%.3f" % [
			canvas_zoom, camera_zoom])
		if absf(canvas_zoom - camera_zoom) > 0.01:
			push_error("3D backend's canvas is scaled %.3f against a camera zoom of "
				% canvas_zoom + "%.3f" % camera_zoom)
	var cam: Camera3D = view.get_node_or_null("MapCamera")
	if cam != null:
		var ortho := cam.projection == Camera3D.PROJECTION_ORTHOGONAL
		# Flat at tilt 0, pitched down by exactly the tilt above it -- TILT_DEGREES
		# defaults to 45 now, so "unrotated" stopped being the healthy state. The
		# geometry itself is geometry_check.tscn's job; this only checks the camera
		# is where the view says it is.
		var tilt_now := float(stats.get("tilt", 0.0))
		var want_rot := Vector3(-deg_to_rad(tilt_now), 0.0, 0.0)
		var aimed := cam.rotation.is_equal_approx(want_rot)
		print("[map3d] camera orthogonal=%s, pitched to tilt %.1f=%s" % [
			str(ortho), tilt_now, str(aimed)])
		if not ortho or not aimed:
			push_error("3D backend camera is not the orthographic view its tilt asks for")
		# The whole of what a viewport needs to render 3D at all, checked after the
		# hide-then-show above because that is the sequence that breaks it.
		#
		# Asked of the viewport, not of the camera: `make_current()` sets the camera's
		# own flag before it early-returns for being outside a World3D, so
		# `cam.is_current()` answers true for a camera the viewport never heard of.
		# The flag is printed beside it precisely so the two can be seen to disagree.
		var active := sub.get_camera_3d() == cam
		print("[map3d] viewport's camera is ours=%s (camera's own flag=%s, world 3d=%s)"
			% [str(active), str(cam.is_current()), str(sub.world_3d != null)])
		if not active:
			push_error("3D backend's camera is not the viewport's, so it renders the "
				+ "clear colour and nothing else -- this is the grey map")
	else:
		push_error("3D backend built no camera")
	# The tilt's geometry is not checked here. It has a gate of its own --
	# res://scenes/geometry_check.tscn -- which needs neither this fixture nor the
	# GDExtension, and an inline copy would need the camera and the placement driven
	# together to mean anything. The first attempt at one here injected the
	# trigonometry without moving the camera and would have compared a stood-up sprite
	# against a flat view.
	print("[map3d] tilt = %.1f degrees (geometry_check.tscn is what verifies it)"
		% float(stats.get("tilt", 0.0)))
	_stage("map_view_3d")
	# Detached, so this stage stays synchronous (see the comment at its top): the
	# finisher screenshots the 3D world -- the one viewport no probe screenshot
	# ever captured, though it is what players see -- and then frees it.
	_finish_map_view_3d(sub)
	# Put the extent back, so the fixtures after this one measure the map the probe
	# asked for rather than the one this stage's viewport happened to fit. Requested
	# without waiting for it to land: the key-drive fixtures run concurrently in
	# _process, and every second this stage spends waiting is a second of theirs spent
	# somewhere the world moved on.
	if extent_before.x > 0 and extent_before.y > 0:
		host.set_map_view_tiles(extent_before.x, extent_before.y)
		print("[map3d] extent %s requested back (this stage fits a 640x480 view)"
			% str(extent_before))

## Nothing MapView owns may reach the z the host's UI panels start at, or the
## map draws over open menus -- which it did, with tile batches on 43, particles
## on 19 and the animation overlay on 64 against a minimap panel on 8.
##
## Still asserted with the world in its own viewport, where a stray z can no
## longer reach a panel: MapView must keep working parented to an ordinary
## canvas, which is exactly how this probe drives it.
##
## Worth asserting rather than remembering because the failure is invisible from
## inside the map: every screenshot of the world looks right, and the damage only
## shows when something else is on screen. A count of children is no use here --
## the number that decides the behaviour is the maximum, so report that.
func _check_z_budget() -> void:
	if _map_view == null:
		return
	var worst := -9999
	var worst_name := "-"
	for child in _map_view.get_children():
		if not (child is CanvasItem):
			continue
		# z_as_relative is the default, so a child's effective z is MapView's plus
		# its own. Reading the effective value is the point: a child that looks
		# safe on its own is not safe if the parent is lifted.
		var eff: int = child.z_index + (_map_view.z_index if child.z_as_relative else 0)
		if eff > worst:
			worst = eff
			worst_name = str(child.name)
	var floor_z: int = _map_view.Z_UI_FLOOR
	var ok := worst < floor_z
	print("[map] z budget: max=%d (%s) ui_floor=%d -> %s" % [
		worst, worst_name, floor_z, "ok" if ok else "OVERLAPS UI"])
	if not ok:
		push_error("map z %d on %s reaches the UI band at %d" % [
			worst, worst_name, floor_z])
	_check_depth_order()

## The standing content must be seated in row order, because that ordering is
## the whole of the 2.5D depth cue (ADR-005 item 3): it is what makes a tree one
## row in front of a zombie cover it and the same tree one row behind it not.
##
## Checked against the child order actually in the tree rather than against the
## rank function, because the rank function being right is not the claim -- the
## claim is that the nodes ended up in that order, and the seating pass is what
## can drop one. A batch left out of the pass sorts nowhere in particular and
## the map still looks plausible in a screenshot.
func _check_depth_order() -> void:
	if _map_view == null:
		return
	var rows: Array[int] = []
	var flat_after_tall := 0
	var seen_tall := false
	for child in _map_view.get_children():
		var parts := str(child.name).split("_")
		# TileBatch_layer_atlas_sway_palette_tall_row
		if parts.size() != 7 or parts[0] != "TileBatch":
			continue
		if int(parts[5]) == 0:
			if seen_tall:
				flat_after_tall += 1
			continue
		seen_tall = true
		rows.append(int(parts[6]))
	var monotonic := true
	for i in range(1, rows.size()):
		if rows[i] < rows[i - 1]:
			monotonic = false
	print("[map] depth order: %d standing batches, rows %s..%s, monotonic=%s, flat_after_tall=%d" % [
		rows.size(),
		str(rows[0]) if not rows.is_empty() else "-",
		str(rows[-1]) if not rows.is_empty() else "-",
		"yes" if monotonic else "NO", flat_after_tall])
	if not monotonic:
		push_error("standing batches are not seated in row order: %s" % str(rows))
	if flat_after_tall > 0:
		push_error("%d flat batches seated above standing content" % flat_after_tall)

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
		# The vehicle rows are the regression check for the grey-box bug: a part
		# with a variant must resolve exactly, not fall back. vp_frame with
		# "horizontal_2_front" should walk down to the longest sprite Ultica has.
		# Ultica's 44 "_transparent" sprites turn out to all be shell casings,
		# so the occlusion-transparency swap has no targets in this tileset and
		# "transparent: 0" above is the correct reading, not a failure. Kept as a
		# probe so that a tileset which *does* ship them shows up as a change.
		for probe in [["t_tree", "terrain"],
				["vp_frame", "vehicle_part", "horizontal_2_front"],
				["vp_frame", "vehicle_part", "cover"],
				["vp_aisle", "vehicle_part", "horizontal"],
				["vp_frame", "vehicle_part", "no_such_variant"],
				["t_grass", "terrain"], ["t_grass_tall", "terrain"],
				["t_shrub", "terrain"], ["t_tree_young", "terrain"],
				["t_tree", "terrain"], ["t_wall", "terrain"],
				["mon_zombie_dusted", "monster"], ["mon_zombie_fungalize", "monster"],
				["mon_zombie", "monster"], ["mon_totally_made_up", "monster"]]:
			var d: Dictionary = host.describe_sprite(probe[0], probe[1],
				probe[2] if probe.size() > 2 else "")
			var matched := str(d.get("matched", ""))
			print("[fx] %-14s%-22s -> %-30s %-11s palette=%s sway=%s" % [
				probe[0],
				("+" + str(probe[2])) if probe.size() > 2 else "",
				matched if matched != "" else "<none>",
				str(d.get("level_name", "")),
				str(d.get("palette_row", 0)), str(d.get("sways", false))])

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
	# Three mechanisms, and which one is carrying the load says what the tileset
	# can do: retracted/transparent need art that declares itself, faded is the
	# renderer-side fallback for the tilesets that do not. All zero in an
	# interior with nothing tall standing between the avatar and the camera.
	print("[render] occlusion: %s retracted, %s swapped to _transparent, %s faded of %s tall"
		% [str(st.get("retracted", 0)), str(st.get("transparent", 0)),
			str(st.get("faded", 0)), str(st.get("tall_candidates", 0))])
	# ADR-005 item 1. The ADR asked for this number before budgeting for
	# z-levels and it is cheaper to keep printing it than to measure it
	# again: open_columns is how many view columns had no floor, so a run
	# that reports zero has not exercised the multi-level path at all.
	print("[render] z-levels: %s open columns, %s deep, %s commands below"
		% [str(st.get("open_columns", 0)), str(st.get("deepest_z_below", 0)),
			str(st.get("below_commands", 0))])
	print("[render] by_fallback=%s" % str(st.get("by_fallback", {})))
	print("[render] by_layer=%s" % str(st.get("by_layer", [])))
	var cov: Array = host.get_sprite_coverage(10)
	print("[render] top sprite misses (%d shown):" % cov.size())
	for row in cov:
		print("[render]   %-28s %-13s %s x%s" % [str(row.get("id", "")),
			str(row.get("category", "")), str(row.get("level_name", "")),
			str(row.get("hits", 0))])

## VER-0. Report what this run actually proved, and fail if it proved less than
## it looks like it did.
func _report_coverage() -> int:
	var missing: Array[String] = []
	for name in REQUIRED_STAGES:
		if not _stages.has(name):
			missing.append(name)
	var dead: Array[String] = []
	for getter in REQUIRED_GENERATIONS:
		if host.has_method(getter) and int(host.call(getter)) == 0:
			dead.append(getter)

	print("[ver] stages run: %d/%d" % [
		REQUIRED_STAGES.size() - missing.size(), REQUIRED_STAGES.size()])
	if not missing.is_empty():
		print("[ver] FAILED: stage(s) never ran: %s" % ", ".join(missing))
		print("[ver]   everything after them in the log describes a run that did "
			+ "not happen")
	if not dead.is_empty():
		print("[ver] FAILED: generation(s) still at zero: %s" % ", ".join(dead))
		print("[ver]   a counter that never moved means nothing consumed that "
			+ "snapshot -- see 'the dead frame boundary' in AGENT_HANDOFF.md")
	var quiet: Array[String] = []
	for getter in OPTIONAL_GENERATIONS:
		if host.has_method(getter) and int(host.call(getter)) == 0:
			quiet.append(getter)
	if not quiet.is_empty():
		print("[ver] note: optional generation(s) at zero this run: %s" % ", ".join(quiet))
	var skipped: Array[String] = []
	for name in OPPORTUNISTIC_STAGES:
		if not _stages.has(name):
			skipped.append(name)
	if not skipped.is_empty():
		print("[ver] note: opportunistic stage(s) the world did not offer: %s"
			% ", ".join(skipped))

	if missing.is_empty() and dead.is_empty():
		print("[ver] OK: every required stage ran and every required generation moved")
		return 0
	return 1

func _shutdown() -> void:
	var coverage := _report_coverage()
	if host.has_method("get_anim_stats"):
		print("[sct] anim chain: %s" % str(host.get_anim_stats()))
	print("[sct] %d combat text run(s) observed over the whole run" % _sct_seen.size())
	for row in _sct_seen:
		print("[sct]   %s" % row)

	# Register the exit code with the backend BEFORE quitting. Shutdown with a
	# live session ends in std::_Exit from the game thread's input wait, which
	# used to hard-code 0 -- so every failing run of this probe exited green,
	# and VER-0's "exit code, not a printed note" was a printed note after all.
	if host.has_method("note_exit_code"):
		host.note_exit_code(coverage)
	# Order matters: request_quit first, so a game thread parked in a menu sees
	# the shutdown flag and exits there instead of cancelling and running on into
	# game logic while the extension is being torn down.
	host.request_quit()
	if host.has_method("uilist_cancel"):
		host.uilist_cancel()
	get_tree().quit(coverage)

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
