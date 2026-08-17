extends Node
## Godot host for Cataclysm-DDA.
## Pre-game chrome (splash, main menu, world pick, custom chargen) is native Godot UI.
## In-session present is the world, drawn from the C++ draw list (ADR-002) inside its
## own viewport (ADR-004) -- MapView by default, or the 3D backend behind
## USE_3D_MAP (ADR-006). TerminalView remains an optional debug underlay.

@onready var cdda_host: Node = get_node("CDDAHost")
@onready var session_bg: ColorRect = %SessionBg
@onready var map_view: Node2D = %MapView
@onready var terminal_view: Control = %TerminalView
@onready var boot_splash: Control = %BootSplash
@onready var splash_label: Label = %SplashLabel
@onready var main_menu: Control = %MainMenu
@onready var load_panel: Control = %LoadPanel
@onready var world_list: ItemList = %WorldList
@onready var save_list: ItemList = %SaveList
@onready var new_game_panel: Control = %NewGamePanel
@onready var world_pick: Control = %WorldPick
@onready var chargen_panel: Control = %ChargenRoot
@onready var hud_panel: Control = %HudPanel
@onready var inventory_panel: Control = %InventoryPanel
@onready var character_panel: Control = %CharacterPanel
## The world's own viewport, which the world is moved into at startup (ADR-004).
## Created in `_setup_world_viewport`; see `world_viewport.gd`.
var world_viewport: TextureRect
## The active world: `map_view` (2D) or a MapView3D, per USE_3D_MAP.
##
## Untyped, because the two are a `Node2D` and a `Node3D` and everything this
## script asks of the world -- `visible`, `refresh`, `zoom_step` -- is on both and
## on neither of their common ancestors. Every backend-agnostic use goes through
## here; `map_view` is only touched where the 2D node specifically is meant.
var world
## The 3D backend, when USE_3D_MAP is on. Null otherwise.
var map_view_3d: Node3D
## Escape menu, created here rather than in the scene so it stays a pure script.
var game_menu_panel: Control
## Generic uilist renderer; shown whenever C++ hands a menu over.
var uilist_panel: Control
## query_popup renderer: yes/no prompts and display-only notices.
var popup_panel: Control
## Read-only text windows: item info, tile descriptions.
var textwin_panel: Control
var options_panel: Control
var keybind_panel: Control
var crafting_panel: Control
var dialogue_panel: Control
var surroundings_panel: Control

## Draw the world with the 3D backend (ADR-006, `BACKLOG.md` Part 4) instead of MapView.
##
## **On, since 2026-08-17.** It draws everything the 2D backend draws, it has been run on
## a real GPU, and the tilt experiment came back positive -- which is what ADR-006 was
## written to ask. MapView stays in the tree, hidden, and is still what the headless probe
## drives and what `--rendering-driver opengl3` would need, so this is a switch rather than
## a replacement.
##
## Two deliberate differences from the 2D backend: sprite edges are alpha-scissored rather
## than blended (3D-1b), and the fallback glyphs draw over the world rather than in their
## own layer's depth band.
##
## `map_view_3d.gd`'s `TILT_DEGREES` is the other half of this and is **45 degrees**: the
## world stands up, so the ground lies down, walls and trees are vertical, and the engine's
## lights and shadows have something with a shape to fall on. Set it to 0 for the flat
## world, which is pixel-identical to the 2D backend and is what the baseline means.
const USE_3D_MAP := true

## Optional debug present (off for product path).
const USE_TERMINAL_DEBUG := false
## Show leftover C++/curses menus (eat, craft, examine, wait, …) over MapView.
const USE_CURSES_UI_OVERLAY := true

## Contract version these scripts expect from the GDExtension. Must match
## CDDAHost::api_version(). Scripts are read from disk each run but the library
## is compiled, so running the two out of step shows the new UI filled with zeros
## for every field the old library does not emit -- which is indistinguishable
## from a bug unless we say so. Bump both sides together.
const REQUIRED_API_VERSION := 22

var last_host_size: Vector2i = Vector2i(0, 0)
var was_session_active: bool = false
var selected_world: String = ""
var _pending_world_load: bool = false
var _pending_game_start: bool = false
var _awaiting_session_present: bool = false
var _status_label: Label
var _minimap_panel: Control
var _overmap_view: Node2D
var _overmap_sidebar: Control
var _overmap_shown: bool = false
var _fatal_panel: Control
## Render debug overlay (SP-9), built on the first F3 press.
var _debug_overlay: Control
var _fatal_label: RichTextLabel

func _ready() -> void:
	print("CDDA Host ready")
	get_tree().set_auto_accept_quit(false)
	_apply_ui_scale()
	_setup_map_view()
	_setup_terminal_view()
	_setup_session_hud()
	_setup_status_label()
	_show_splash()
	get_viewport().size_changed.connect(_on_viewport_resized)
	# Bootstrap first so a UI script error cannot leave us stuck on the splash.
	_check_api_version()
	cdda_host.bootstrap_async()
	_on_viewport_resized()
	if world_pick.has_method("setup"):
		world_pick.setup(cdda_host)
	if world_pick.has_signal("world_chosen"):
		world_pick.world_chosen.connect(_on_world_chosen_for_chargen)
	if world_pick.has_signal("cancelled"):
		world_pick.cancelled.connect(_on_world_pick_cancelled)
	if chargen_panel.has_signal("confirmed"):
		chargen_panel.confirmed.connect(_on_chargen_confirmed)
	if chargen_panel.has_signal("cancelled"):
		chargen_panel.cancelled.connect(_on_chargen_cancelled)

func _process(_delta: float) -> void:
	if cdda_host.bootstrap_failed():
		# Writing to the boot splash was useless once a session had started: that
		# label is hidden, and returning here stops every panel updating, so a dead
		# game thread looked exactly like the game freezing with no explanation.
		_show_fatal_error()
		return

	if not cdda_host.is_ready():
		_update_splash_progress("Loading Cataclysm…")
		return

	_poll_async_chargen()

	if _awaiting_session_present and not _session_present_ready():
		if not boot_splash.visible:
			_show_splash()
		_update_splash_progress("Loading Cataclysm…")
		return

	if _awaiting_session_present:
		_awaiting_session_present = false
		_show_session()

	var session: bool = cdda_host.is_session_active()
	if session != was_session_active:
		was_session_active = session
		if session:
			_show_session()
		elif not world_pick.visible and not chargen_panel.visible and not _pending_world_load and not _pending_game_start:
			_show_main_menu()
	elif boot_splash.visible and not chargen_panel.visible and not world_pick.visible:
		_show_main_menu()

	_update_uilist_panel()
	_update_popup_panel()
	_update_textwin_panel()
	_update_options_panel()
	_update_keybind_panel()
	_update_crafting_panel()
	_update_dialogue_panel()
	_update_surroundings_panel()

	if session or world.visible or _pending_game_start:
		_update_overmap_visibility()
		if world.visible and world.has_method("refresh"):
			world.refresh()
		if _overmap_view != null and _overmap_view.visible:
			_overmap_view.refresh()
		if _overmap_sidebar != null:
			# Shown with the map and hidden with it: the sidebar has no life of
			# its own, and one left behind would sit over the game map.
			var om_up: bool = _overmap_view != null and _overmap_view.visible
			if om_up:
				_overmap_sidebar.refresh()
			if om_up != _overmap_sidebar.visible:
				_overmap_sidebar.visible = om_up
		if hud_panel.visible and hud_panel.has_method("refresh"):
			hud_panel.refresh()
		if _minimap_panel != null and _minimap_panel.visible:
			_minimap_panel.refresh()
		if terminal_view.visible and terminal_view.has_method("refresh"):
			terminal_view.refresh()
		if _status_label:
			_status_label.visible = session or _pending_game_start
			var tileset := ""
			var cmds := 0
			var ready := false
			if cdda_host.has_method("tileset_ready"):
				ready = cdda_host.tileset_ready()
			if cdda_host.has_method("get_tileset_id"):
				tileset = str(cdda_host.get_tileset_id())
			if cdda_host.has_method("get_map_command_count"):
				cmds = cdda_host.get_map_command_count()
			_status_label.text = "session=%s tileset=%s ready=%s cmds=%d" % [
				str(session), tileset, str(ready), cmds
			]

func _poll_async_chargen() -> void:
	if _pending_world_load and not cdda_host.is_chargen_busy():
		_pending_world_load = false
		_set_world_pick_busy(false)
		if cdda_host.is_chargen_active():
			_show_chargen()
		else:
			_hide_all_chrome()
			world_pick.visible = true
			var err: String = cdda_host.chargen_last_error()
			world_pick.status_label.text = err if not err.is_empty() else "Failed to load world."

	# Success keeps MapView — do not treat "session not active yet" as failure
	# (session_active flips true around the same time busy clears).
	if _pending_game_start and not cdda_host.is_chargen_busy():
		_pending_game_start = false
		var err2: String = cdda_host.chargen_last_error()
		if not err2.is_empty():
			if cdda_host.is_chargen_active():
				_show_chargen()
				if chargen_panel.has_method("_show_error"):
					chargen_panel._show_error(err2)
			else:
				_show_main_menu()

func _setup_map_view() -> void:
	if USE_3D_MAP:
		map_view_3d = Node3D.new()
		map_view_3d.name = "MapView3D"
		map_view_3d.set_script(load("res://scripts/map_view_3d.gd"))
		map_view_3d.visible = false
		add_child(map_view_3d)
		world = map_view_3d
		# The 2D map stays in the scene, hidden and never refreshed, so switching
		# backends is one constant and not a scene edit. It is also what the probe
		# still drives, since MapView has to keep working on an ordinary canvas.
		map_view.visible = false
	else:
		map_view.z_index = 0
		world = map_view
	_setup_world_viewport()
	if world.has_method("setup"):
		world.setup(cdda_host)
	_sync_map_reserved_width()

## The world gets its own viewport (ADR-004), composited under the UI.
##
## Built here rather than declared in main.tscn for two reasons: the scene stays
## a flat list of screens, and MapView keeps working when parented to an ordinary
## canvas -- which is how `headless_probe.gd` drives it, and is the only reason
## its internal z discipline still matters.
##
## Every place that toggles `world.visible` keeps working untouched, because the
## world viewport mirrors it: left visible over a hidden world it would go on
## compositing the last frame the world drew, and the main menu would open over a
## still of the map.
func _setup_world_viewport() -> void:
	if world_viewport != null:
		return
	world_viewport = TextureRect.new()
	world_viewport.name = "WorldViewport"
	world_viewport.set_script(load("res://scripts/world_viewport.gd"))
	# Where MapView itself sat: over SessionBg at -1, under every panel from 8 up.
	world_viewport.z_index = 0
	add_child(world_viewport)
	world_viewport.set_3d_enabled(USE_3D_MAP)
	world_viewport.setup(world)

## Keep the world's viewport in step with the sidebar, so the world fills
## everything the sidebar is not covering and nothing is drawn underneath it.
func _sync_map_reserved_width() -> void:
	if world_viewport == null:
		return
	var reserved := 0.0
	if hud_panel.visible and "WIDTH" in hud_panel:
		reserved = float(hud_panel.WIDTH)
	world_viewport.set_reserved_right(reserved)

func _setup_session_hud() -> void:
	if hud_panel.has_method("setup"):
		hud_panel.setup(cdda_host)
	if inventory_panel.has_method("setup"):
		inventory_panel.setup(cdda_host)
	if inventory_panel.has_signal("closed"):
		inventory_panel.closed.connect(_close_session_overlays)
	if character_panel.has_method("setup"):
		character_panel.setup(cdda_host)
	game_menu_panel = Control.new()
	game_menu_panel.set_script(load("res://scripts/game_menu_panel.gd"))
	game_menu_panel.name = "GameMenuPanel"
	game_menu_panel.z_index = 14
	game_menu_panel.visible = false
	add_child(game_menu_panel)
	game_menu_panel.setup(cdda_host)
	game_menu_panel.closed.connect(func() -> void: game_menu_panel.visible = false)
	game_menu_panel.open_inventory.connect(func() -> void:
		game_menu_panel.visible = false
		_toggle_inventory())
	game_menu_panel.open_character.connect(func() -> void:
		game_menu_panel.visible = false
		_toggle_character())
	uilist_panel = Control.new()
	uilist_panel.set_script(load("res://scripts/uilist_panel.gd"))
	uilist_panel.name = "UilistPanel"
	uilist_panel.z_index = 16
	uilist_panel.visible = false
	add_child(uilist_panel)
	uilist_panel.setup(cdda_host)
	popup_panel = Control.new()
	popup_panel.set_script(load("res://scripts/popup_panel.gd"))
	popup_panel.name = "PopupPanel"
	# Above the uilist panel: a prompt can be raised from inside a menu.
	popup_panel.z_index = 18
	add_child(popup_panel)
	popup_panel.setup(cdda_host)
	textwin_panel = Control.new()
	textwin_panel.set_script(load("res://scripts/textwin_panel.gd"))
	textwin_panel.name = "TextWinPanel"
	textwin_panel.z_index = 17
	textwin_panel.visible = false
	add_child(textwin_panel)
	textwin_panel.setup(cdda_host)

	options_panel = Control.new()
	options_panel.set_script(load("res://scripts/options_panel.gd"))
	options_panel.name = "OptionsPanel"
	options_panel.z_index = 17
	options_panel.visible = false
	add_child(options_panel)
	options_panel.setup(cdda_host)

	keybind_panel = Control.new()
	keybind_panel.set_script(load("res://scripts/keybind_panel.gd"))
	keybind_panel.name = "KeybindPanel"
	keybind_panel.z_index = 17
	keybind_panel.visible = false
	add_child(keybind_panel)
	keybind_panel.setup(cdda_host)

	crafting_panel = Control.new()
	crafting_panel.set_script(load("res://scripts/crafting_panel.gd"))
	crafting_panel.name = "CraftingPanel"
	crafting_panel.z_index = 17
	crafting_panel.visible = false
	add_child(crafting_panel)
	crafting_panel.setup(cdda_host)

	dialogue_panel = Control.new()
	dialogue_panel.set_script(load("res://scripts/dialogue_panel.gd"))
	dialogue_panel.name = "DialoguePanel"
	dialogue_panel.z_index = 17
	dialogue_panel.visible = false
	add_child(dialogue_panel)
	dialogue_panel.setup(cdda_host)

	surroundings_panel = Control.new()
	surroundings_panel.set_script(load("res://scripts/surroundings_panel.gd"))
	surroundings_panel.name = "SurroundingsPanel"
	surroundings_panel.z_index = 17
	surroundings_panel.visible = false
	add_child(surroundings_panel)
	surroundings_panel.setup(cdda_host)
	if character_panel.has_signal("closed"):
		character_panel.closed.connect(_close_session_overlays)
	_setup_minimap_panel()
	_setup_overmap_view()

## OvermapView is created at runtime like the minimap: it is a self-contained
## Node2D with no scene-side configuration.
func _setup_overmap_view() -> void:
	if _overmap_view != null:
		return
	_overmap_view = Node2D.new()
	_overmap_view.name = "OvermapView"
	_overmap_view.set_script(load("res://scripts/overmap_view.gd"))
	_overmap_view.visible = false
	# Above MapView, below the curses overlay so prompts still land on top.
	_overmap_view.z_index = 2
	add_child(_overmap_view)
	_overmap_view.setup(cdda_host)

	_overmap_sidebar = Control.new()
	_overmap_sidebar.name = "OvermapSidebar"
	_overmap_sidebar.set_script(load("res://scripts/overmap_sidebar_panel.gd"))
	_overmap_sidebar.visible = false
	# Above OvermapView, which it sits beside rather than over.
	_overmap_sidebar.z_index = 3
	add_child(_overmap_sidebar)
	_overmap_sidebar.setup(cdda_host)

## Built at runtime rather than in main.tscn: it is a self-contained overlay with
## no scene-side configuration to edit.
func _setup_minimap_panel() -> void:
	if _minimap_panel != null:
		return
	_minimap_panel = Control.new()
	_minimap_panel.name = "MinimapPanel"
	_minimap_panel.set_script(load("res://scripts/minimap_panel.gd"))
	# Above MapView, below the sidebar. Left at the default it shared a z_index
	# with the map and the tiles drew over it.
	_minimap_panel.z_index = 8
	_minimap_panel.visible = false
	add_child(_minimap_panel)
	_minimap_panel.setup(cdda_host)

func _setup_terminal_view() -> void:
	terminal_view.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	terminal_view.mouse_filter = Control.MOUSE_FILTER_IGNORE
	terminal_view.z_index = 12
	if terminal_view.has_method("setup"):
		terminal_view.setup(cdda_host)

func _setup_status_label() -> void:
	_status_label = Label.new()
	_status_label.name = "PresentDebug"
	_status_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_status_label.z_index = 100
	_status_label.position = Vector2(8, 8)
	_status_label.add_theme_color_override("font_color", Color(0.4, 1, 0.5))
	_status_label.add_theme_color_override("font_shadow_color", Color(0, 0, 0, 0.9))
	_status_label.add_theme_constant_override("shadow_offset_x", 1)
	_status_label.add_theme_constant_override("shadow_offset_y", 1)
	_status_label.visible = false
	add_child(_status_label)

func _session_present_ready() -> bool:
	if not cdda_host.is_session_active():
		return false
	if cdda_host.has_method("tileset_ready") and not cdda_host.tileset_ready():
		return false
	return true

func _update_splash_progress(title: String) -> void:
	if splash_label == null:
		return
	var ctx := ""
	var step := ""
	if cdda_host.has_method("get_loading_context"):
		ctx = str(cdda_host.get_loading_context())
	if cdda_host.has_method("get_loading_step"):
		step = str(cdda_host.get_loading_step())
	var detail := ("%s %s" % [ctx, step]).strip_edges()
	if detail.is_empty():
		splash_label.text = title
	else:
		splash_label.text = "%s\n\n%s" % [title, detail]

func _begin_session_load() -> void:
	_awaiting_session_present = true
	_show_splash()
	_update_splash_progress("Loading Cataclysm…")

func _hide_all_chrome() -> void:
	boot_splash.visible = false
	main_menu.visible = false
	load_panel.visible = false
	new_game_panel.visible = false
	world_pick.visible = false
	chargen_panel.visible = false
	session_bg.visible = false
	world.visible = false
	hud_panel.visible = false
	if _minimap_panel != null:
		_minimap_panel.visible = false
		# Leaving a size published would keep the game thread rendering minimap
		# frames outside a session.
		_minimap_panel.release()
	if _overmap_view != null:
		_overmap_view.visible = false
	_overmap_shown = false
	inventory_panel.visible = false
	character_panel.visible = false
	terminal_view.visible = false

func _show_splash() -> void:
	_hide_all_chrome()
	boot_splash.visible = true
	splash_label.text = "Loading Cataclysm…"

func _show_main_menu() -> void:
	_awaiting_session_present = false
	_hide_all_chrome()
	main_menu.visible = true

func _show_session() -> void:
	_hide_all_chrome()
	session_bg.visible = true
	world.visible = true
	hud_panel.visible = true
	_sync_map_reserved_width()
	if _minimap_panel != null:
		_minimap_panel.visible = true
	inventory_panel.visible = false
	character_panel.visible = false
	terminal_view.visible = USE_TERMINAL_DEBUG or USE_CURSES_UI_OVERLAY
	if world.has_method("refresh"):
		world.refresh()
	if hud_panel.has_method("refresh"):
		hud_panel.refresh()
	if terminal_view.visible and terminal_view.has_method("refresh"):
		terminal_view.refresh()

func _show_world_pick() -> void:
	_hide_all_chrome()
	world_pick.visible = true
	world_pick.setup(cdda_host)

func _show_chargen() -> void:
	_awaiting_session_present = false
	_hide_all_chrome()
	chargen_panel.visible = true
	chargen_panel.setup(cdda_host)

func _check_api_version() -> void:
	var found := -1
	if cdda_host.has_method("api_version"):
		found = int(cdda_host.api_version())
	if found == REQUIRED_API_VERSION:
		return
	var msg := ("Stale build: these scripts expect API v%d, the loaded library "
		+ "reports v%s.\nRebuild the GDExtension:\n"
		+ "    make GODOT=1 -j$(sysctl -n hw.ncpu)\n"
		+ "Until then, values the old library does not send read as 0 or empty.") % [
			REQUIRED_API_VERSION, "missing" if found < 0 else str(found)]
	push_error(msg)
	print(msg)
	_show_api_warning(msg)

func _show_api_warning(msg: String) -> void:
	var bar := PanelContainer.new()
	bar.name = "ApiWarning"
	bar.set_anchors_preset(Control.PRESET_TOP_WIDE)
	bar.z_index = 200
	bar.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.35, 0.10, 0.10, 0.96)
	sb.set_content_margin_all(10)
	bar.add_theme_stylebox_override("panel", sb)
	var label := Label.new()
	label.text = msg
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.add_theme_color_override("font_color", Color(1, 0.92, 0.9))
	bar.add_child(label)
	add_child(bar)

func _apply_ui_scale() -> void:
	# Stretch mode (project.godot canvas_items + expand) scales Controls with the
	# window. Keep content_scale_factor at 1 so we do not double-scale.
	var win := get_window()
	if win == null:
		return
	win.content_scale_factor = 1.0
	var screen := DisplayServer.screen_get_size(win.current_screen)
	print("UI stretch active; screen=", screen, " window=", win.size)

func _on_viewport_resized() -> void:
	_apply_ui_scale()
	var size := get_viewport().get_visible_rect().size
	var next := Vector2i(int(size.x), int(size.y))
	if next.x > 0 and next.y > 0 and next != last_host_size:
		last_host_size = next
		cdda_host.set_window_size(next.x, next.y)
	if world.visible and world.has_method("refresh"):
		world.refresh()

## Show what killed the game thread, over whatever was on screen.
##
## The thread is gone at this point and nothing will update again, so the only
## useful thing left is to say why rather than appear hung.
func _show_fatal_error() -> void:
	if _fatal_panel != null:
		return
	_fatal_panel = Control.new()
	_fatal_panel.name = "FatalError"
	_fatal_panel.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_fatal_panel.mouse_filter = Control.MOUSE_FILTER_STOP
	_fatal_panel.z_index = 200
	add_child(_fatal_panel)

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.72)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_fatal_panel.add_child(dim)

	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -440.0
	frame.offset_top = -160.0
	frame.offset_right = 440.0
	frame.offset_bottom = 160.0
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.10, 0.06, 0.07, 0.98)
	sb.border_color = Color(0.75, 0.25, 0.25)
	sb.set_border_width_all(2)
	sb.set_content_margin_all(16)
	frame.add_theme_stylebox_override("panel", sb)
	_fatal_panel.add_child(frame)

	var vbox := VBoxContainer.new()
	frame.add_child(vbox)
	var title := Label.new()
	title.text = "The game thread stopped"
	title.add_theme_font_size_override("font_size", 22)
	vbox.add_child(title)

	_fatal_label = RichTextLabel.new()
	_fatal_label.bbcode_enabled = false
	_fatal_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_fatal_label.size_flags_vertical = Control.SIZE_EXPAND_FILL
	var detail := ""
	if cdda_host.has_method("get_error_message"):
		detail = str(cdda_host.get_error_message())
	if detail == "":
		detail = "No message was recorded. See the console output or crash.log."
	_fatal_label.text = "%s\n\nThe world was not necessarily saved. Console output and crash.log have more." % detail
	vbox.add_child(_fatal_label)

## The overmap is a blocking C++ UI, so its own guard tells us when it is up.
## While it is, MapView is pointless: the overmap covers the screen.
func _update_overmap_visibility() -> void:
	if _overmap_view == null or not cdda_host.has_method("overmap_active"):
		return
	var active: bool = cdda_host.overmap_active()
	if active == _overmap_shown:
		return
	_overmap_shown = active
	_overmap_view.visible = active
	world.visible = not active
	# The sidebar and minimap describe the local view; leaving them up over the
	# overmap covers the map with information that does not apply to it.
	hud_panel.visible = not active
	_sync_map_reserved_width()
	if _minimap_panel != null:
		_minimap_panel.visible = not active

## A uilist handed over by C++ owns the screen while it is up: the game thread is
## blocked inside uilist::query() waiting for this panel to answer.
func _update_uilist_panel() -> void:
	if uilist_panel == null or not cdda_host.has_method("uilist_active"):
		return
	var want: bool = cdda_host.uilist_active()
	if want:
		uilist_panel.refresh()
	if want != uilist_panel.visible:
		uilist_panel.visible = want

## Popups are polled every frame, not only during a session: a notice can be up
## during world generation, and a prompt can be raised from inside another menu.
func _update_popup_panel() -> void:
	if popup_panel == null or not cdda_host.has_method("popup_active"):
		return
	# Refresh while anything is up *and* on the frame it goes away. The panel
	# clears its own notice and prompt from the published state, so it has to be
	# given the empty state to see. Testing prompt_open() here missed notices
	# entirely: "Grab where?" answered correctly, the game moved on, and the
	# ribbon stayed on screen forever because nothing asked the panel again.
	if cdda_host.popup_active() or popup_panel.showing():
		popup_panel.refresh()

func _update_textwin_panel() -> void:
	if textwin_panel == null or not cdda_host.has_method("textwin_active"):
		return
	var want: bool = cdda_host.textwin_active()
	if want:
		textwin_panel.refresh()
	if want != textwin_panel.visible:
		textwin_panel.visible = want

func _textwin_open() -> bool:
	return textwin_panel != null and textwin_panel.visible

func _update_options_panel() -> void:
	if options_panel == null or not cdda_host.has_method("options_active"):
		return
	var want: bool = cdda_host.options_active()
	if want:
		options_panel.refresh()
	if want != options_panel.visible:
		options_panel.visible = want

func _options_open() -> bool:
	return options_panel != null and options_panel.visible

func _update_keybind_panel() -> void:
	if keybind_panel == null or not cdda_host.has_method("keybind_active"):
		return
	var want: bool = cdda_host.keybind_active()
	if want:
		keybind_panel.refresh()
	if want != keybind_panel.visible:
		keybind_panel.visible = want

func _keybind_open() -> bool:
	return keybind_panel != null and keybind_panel.visible

func _update_crafting_panel() -> void:
	if crafting_panel == null or not cdda_host.has_method("crafting_active"):
		return
	var want: bool = cdda_host.crafting_active()
	if want:
		crafting_panel.refresh()
	if want != crafting_panel.visible:
		crafting_panel.visible = want

func _crafting_open() -> bool:
	return crafting_panel != null and crafting_panel.visible

func _update_dialogue_panel() -> void:
	if dialogue_panel == null or not cdda_host.has_method("dialogue_active"):
		return
	var want: bool = cdda_host.dialogue_active()
	if want:
		dialogue_panel.refresh()
	if want != dialogue_panel.visible:
		dialogue_panel.visible = want

func _dialogue_open() -> bool:
	return dialogue_panel != null and dialogue_panel.visible

func _update_surroundings_panel() -> void:
	if surroundings_panel == null or not cdda_host.has_method("surroundings_active"):
		return
	var want: bool = cdda_host.surroundings_active()
	if want:
		surroundings_panel.refresh()
	if want != surroundings_panel.visible:
		surroundings_panel.visible = want

func _surroundings_open() -> bool:
	return surroundings_panel != null and surroundings_panel.visible

func _popup_modal() -> bool:
	return popup_panel != null and popup_panel.prompt_open()

func _uilist_open() -> bool:
	return uilist_panel != null and uilist_panel.visible

func _session_overlay_open() -> bool:
	return inventory_panel.visible or character_panel.visible \
		or (game_menu_panel != null and game_menu_panel.visible)

func _close_session_overlays() -> void:
	inventory_panel.visible = false
	character_panel.visible = false
	if game_menu_panel != null:
		game_menu_panel.visible = false

## Escape opens the Godot game menu instead of the C++ one.
##
## ACTION_MAIN_MENU opened a blocking `uilist` on the game thread that could only
## be drawn through the curses/ImGui overlay. Intercepting Escape here means that
## menu is never opened, so there is nothing to draw and nothing to get stuck in.
func _toggle_game_menu() -> void:
	if game_menu_panel == null:
		return
	inventory_panel.visible = false
	character_panel.visible = false
	if game_menu_panel.visible:
		game_menu_panel.visible = false
	else:
		game_menu_panel.open()

func _toggle_inventory() -> void:
	if character_panel.visible:
		character_panel.visible = false
	inventory_panel.visible = not inventory_panel.visible
	if inventory_panel.visible and inventory_panel.has_method("refresh"):
		inventory_panel.refresh()

func _toggle_character() -> void:
	if inventory_panel.visible:
		inventory_panel.visible = false
	character_panel.visible = not character_panel.visible
	if character_panel.visible and character_panel.has_method("refresh"):
		character_panel.refresh()

func _is_session_hotkey(event: InputEvent) -> bool:
	if not (event is InputEventKey) or not event.pressed or event.echo:
		return false
	if event.keycode == KEY_I and not event.ctrl_pressed and not event.alt_pressed and not event.meta_pressed:
		return true
	if event.keycode == KEY_AT or event.unicode == 64:
		return true
	if event.keycode == KEY_ESCAPE:
		return true
	return false

func _handle_session_hotkey(event: InputEvent) -> bool:
	if (_popup_modal() or _textwin_open() or _uilist_open() or _options_open()
			or _keybind_open() or _crafting_open() or _dialogue_open()
			or _surroundings_open()):
		return false
	# A notice on screen means the game thread is mid-prompt and waiting on a key
	# -- "grab what?", "examine where?". Those are answered with a direction or
	# with Escape, so Godot must not claim Escape for its own menu here; doing so
	# left the prompt on screen with no way to dismiss it.
	if cdda_host.has_method("popup_active") and cdda_host.popup_active():
		return false
	if not (cdda_host.is_session_active() or _pending_game_start) or not world.visible:
		return false
	# A C++ screen is drawn: it owns the keyboard until it closes. Without this,
	# a legacy screen opened from the Godot game menu could never be dismissed --
	# Escape would be swallowed here and never reach it.
	if not _session_overlay_open() and cdda_host.has_method("legacy_ui_active") \
			and cdda_host.legacy_ui_active():
		return false
	if not _is_session_hotkey(event):
		return false
	if event.keycode == KEY_ESCAPE:
		# An overlay is up: Escape backs out of it. Otherwise it is the game menu.
		if inventory_panel.visible or character_panel.visible \
				or (game_menu_panel != null and game_menu_panel.visible):
			_close_session_overlays()
		else:
			_toggle_game_menu()
		return true
	if event.keycode == KEY_I:
		_toggle_inventory()
		return true
	if event.keycode == KEY_AT or event.unicode == 64:
		_toggle_character()
		return true
	return false

func _forward_input(event: InputEvent) -> void:
	# Godot owns inventory / character overlays; do not send those keys (or any
	# input while an overlay is open) into the curses game loop.
	if _handle_session_hotkey(event):
		return
	# The uilist panel handles its own keys and answers C++ directly; forwarding
	# them as well would drive the game underneath the menu.
	if (_popup_modal() or _textwin_open() or _uilist_open() or _options_open()
			or _keybind_open() or _crafting_open() or _dialogue_open()
			or _surroundings_open() or _session_overlay_open()):
		return
	if world.visible or terminal_view.visible:
		cdda_host.push_input_event(event)

## Render debug overlay (SP-9): what the tile renderer just did, and which
## sprite ids it could not find. Built on first use -- it costs nothing until
## someone asks for it, and most sessions never will.
func _toggle_debug_overlay() -> void:
	if _debug_overlay == null:
		_debug_overlay = PanelContainer.new()
		_debug_overlay.name = "DebugOverlay"
		_debug_overlay.set_script(load("res://scripts/debug_overlay.gd"))
		add_child(_debug_overlay)
		_debug_overlay.setup(cdda_host)
	_debug_overlay.visible = not _debug_overlay.visible
	if _debug_overlay.visible:
		_debug_overlay.refresh()
	# Said out loud, because the failure this had was silence: the key was being eaten by
	# the desktop, and an overlay that never appears is indistinguishable from one that
	# appeared empty, from one whose script died on the way up.
	print("[host] render overlay ", "shown" if _debug_overlay.visible else "hidden")

## Whether @p event opens the render debug overlay (SP-9).
##
## Two bindings, because F3 alone is not reachable on every desktop: macOS takes it for
## Mission Control before any application sees it, so the overlay simply "did nothing"
## for whoever was trying to read a number off it. Ctrl+Shift+D is the one no window
## manager wants, and CDDA binds nothing to it either.
static func _is_debug_overlay_key(event: InputEvent) -> bool:
	if not (event is InputEventKey) or not event.pressed or event.echo:
		return false
	if event.keycode == KEY_F3:
		return true
	return event.keycode == KEY_D and event.ctrl_pressed and event.shift_pressed

func _input(event: InputEvent) -> void:
	if _is_debug_overlay_key(event):
		_toggle_debug_overlay()
		get_viewport().set_input_as_handled()
		return
	if event is InputEventKey and event.pressed and event.keycode == KEY_F11:
		cdda_host.toggle_fullscreen()
		var window := get_window()
		if window.mode == Window.MODE_FULLSCREEN:
			window.mode = Window.MODE_WINDOWED
		else:
			window.mode = Window.MODE_FULLSCREEN
		get_viewport().set_input_as_handled()
		return
	# Ctrl+wheel zooms the map, without stealing plain wheel from CDDA.
	#
	# This was MapView's own `_unhandled_input` until the world moved into a
	# SubViewport that accepts no input at all, at which point no event could
	# reach it. Nothing would have reported that: zoom is a convenience, and a
	# convenience that stops working looks like a feature nobody used.
	if event is InputEventMouseButton and event.pressed and event.ctrl_pressed \
			and world.visible and world.has_method("zoom_step"):
		var zoom_dir := 0
		if event.button_index == MOUSE_BUTTON_WHEEL_UP:
			zoom_dir = 1
		elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			zoom_dir = -1
		if zoom_dir != 0:
			world.zoom_step(zoom_dir)
			get_viewport().set_input_as_handled()
			return
	# Handle session overlays here so they win over MapView, then mark handled
	# so _unhandled_input does not double-feed the game (which closed menus).
	if _handle_session_hotkey(event):
		get_viewport().set_input_as_handled()

func _unhandled_input(event: InputEvent) -> void:
	_forward_input(event)

func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST:
		_quit_app()

func _quit_app() -> void:
	cdda_host.request_quit()
	get_tree().quit()

func _on_new_game_pressed() -> void:
	main_menu.visible = false
	new_game_panel.visible = true

func _on_new_custom_pressed() -> void:
	_show_world_pick()

func _set_world_pick_busy(busy: bool) -> void:
	world_pick.mouse_filter = Control.MOUSE_FILTER_IGNORE if busy else Control.MOUSE_FILTER_STOP
	for child in world_pick.find_children("*", "BaseButton", true, false):
		(child as BaseButton).disabled = busy
	var list := world_pick.get_node_or_null("%WorldPickList") as ItemList
	if list:
		list.mouse_filter = Control.MOUSE_FILTER_IGNORE if busy else Control.MOUSE_FILTER_STOP

func _on_world_chosen_for_chargen(world_name: String) -> void:
	world_pick.status_label.text = "Loading world…"
	_set_world_pick_busy(true)
	_pending_world_load = true
	cdda_host.request_begin_custom_chargen(world_name)

func _on_world_pick_cancelled() -> void:
	_pending_world_load = false
	_set_world_pick_busy(false)
	_hide_all_chrome()
	new_game_panel.visible = true

func _on_chargen_confirmed() -> void:
	_pending_game_start = true
	_begin_session_load()
	cdda_host.request_confirm_chargen()

func _on_chargen_cancelled() -> void:
	_pending_game_start = false
	_awaiting_session_present = false
	_show_main_menu()

func _on_new_now_pressed() -> void:
	_begin_session_load()
	cdda_host.request_new_game("now")

func _on_new_random_pressed() -> void:
	_begin_session_load()
	cdda_host.request_new_game("random")

func _on_new_back_pressed() -> void:
	_show_main_menu()

func _on_load_game_pressed() -> void:
	main_menu.visible = false
	load_panel.visible = true
	_populate_worlds()

func _populate_worlds() -> void:
	world_list.clear()
	save_list.clear()
	selected_world = ""
	var worlds: PackedStringArray = cdda_host.list_worlds()
	for w in worlds:
		world_list.add_item(w)

func _on_world_selected(index: int) -> void:
	if index < 0:
		return
	selected_world = world_list.get_item_text(index)
	save_list.clear()
	var saves: PackedStringArray = cdda_host.list_saves(selected_world)
	for s in saves:
		save_list.add_item(s)

func _on_load_confirm_pressed() -> void:
	var idxs := save_list.get_selected_items()
	if selected_world.is_empty() or idxs.is_empty():
		return
	var save_name := save_list.get_item_text(idxs[0])
	_begin_session_load()
	cdda_host.request_load_game(selected_world, save_name)

func _on_load_back_pressed() -> void:
	_show_main_menu()

func _on_quit_pressed() -> void:
	_quit_app()
