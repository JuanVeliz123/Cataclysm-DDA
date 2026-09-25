extends Control
## The Escape menu, as a Godot Control.
##
## This replaces the C++ `uilist` that ACTION_MAIN_MENU used to open. Escape is
## intercepted by host.gd and never reaches the game, so no blocking C++ menu is
## opened for it at all -- which is the point: that menu ran its own input loop
## inside the game thread and had to be drawn through the curses/ImGui overlay.
##
## Entries in the first group are handled entirely on this side or through the
## command channel. Entries in "Legacy screens" still open a C++ UI over MapView;
## they are listed here so nothing becomes unreachable while those screens are
## migrated one at a time.

signal closed
signal open_inventory
signal open_character

const N := preload("res://scripts/nocturne.gd")

## Must match godot_backend::menu_action.
const QUICKSAVE := 0
const SAVE_AND_QUIT := 1
const QUIT_NO_SAVE := 2
const OPTIONS := 3
const KEYBINDINGS := 4
const SAFE_MODE := 5
const AUTO_PICKUP := 6
const COLORS := 7
const HELP := 8
const AUTO_NOTES := 9
const DISTRACTIONS := 10

var _host: Node
var _rows: Array[Button] = []
var _status: Label
var _selected := 0
## Set while a confirmation is pending, so a destructive action needs two keys.
var _confirming := -1

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if not _rows.is_empty():
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.55)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(dim)

	var frame := Control.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -220.0
	# Tall enough that today's entries need no scrolling -- a menu that looks
	# complete will not invite anyone to scroll it. The rows still sit in a
	# ScrollContainer so a shorter viewport, or more entries, degrades to
	# scrolling rather than to silently clipping the last one.
	frame.offset_top = -358.0
	frame.offset_right = 220.0
	frame.offset_bottom = 358.0
	frame.clip_contents = true
	add_child(frame)

	var bg := ColorRect.new()
	bg.color = Color(N.BG.r, N.BG.g, N.BG.b, 0.985)
	bg.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	bg.mouse_filter = Control.MOUSE_FILTER_IGNORE
	frame.add_child(bg)
	var border := Panel.new()
	var bsb := StyleBoxFlat.new()
	bsb.bg_color = Color(0, 0, 0, 0)
	bsb.border_color = N.NEUTRAL_800
	bsb.set_border_width_all(1)
	border.add_theme_stylebox_override("panel", bsb)
	border.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	border.mouse_filter = Control.MOUSE_FILTER_IGNORE
	frame.add_child(border)
	var band := ColorRect.new()
	band.set_anchors_preset(Control.PRESET_TOP_WIDE)
	band.offset_bottom = 3.0
	band.color = N.ACCENT_700
	band.mouse_filter = Control.MOUSE_FILTER_IGNORE
	frame.add_child(band)

	var pad := MarginContainer.new()
	pad.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	pad.add_theme_constant_override("margin_left", 26)
	pad.add_theme_constant_override("margin_right", 26)
	pad.add_theme_constant_override("margin_top", 24)
	pad.add_theme_constant_override("margin_bottom", 20)
	frame.add_child(pad)

	var outer := VBoxContainer.new()
	outer.add_theme_constant_override("separation", N.SPACE_S)
	pad.add_child(outer)

	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	outer.add_child(scroll)

	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", N.SPACE_S)
	col.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(col)

	var title := Label.new()
	title.text = "GAME MENU"
	title.add_theme_font_size_override("font_size", 19)
	title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(title)
	col.add_child(N.micro_label("Esc to resume", N.NEUTRAL_600))
	col.add_child(N.fade_rule())

	col.add_child(N.section_header(1, "Session"))
	_add_row(col, "Resume", "ESC", func() -> void: closed.emit())
	_add_row(col, "Inventory", "I", func() -> void: open_inventory.emit())
	_add_row(col, "Character", "@", func() -> void: open_character.emit())
	_add_row(col, "Quicksave", "", func() -> void: _dispatch(QUICKSAVE, "Saving…"))
	_add_row(col, "Save and quit", "", func() -> void: _confirm(SAVE_AND_QUIT, "Save and quit"))
	_add_row(col, "Quit without saving", "", func() -> void:
		_confirm(QUIT_NO_SAVE, "Quit and LOSE progress"))

	col.add_child(N.fade_rule())
	col.add_child(N.section_header(2, "Settings"))
	_add_row(col, "Options", "", func() -> void: _dispatch(OPTIONS, "Opening options…"))
	_add_row(col, "Keybindings", "", func() -> void: _dispatch(KEYBINDINGS, "Opening keybindings…"))

	col.add_child(N.fade_rule())
	# Still C++ UI. Named as such rather than mixed in silently, so the state of
	# the migration is visible from the menu itself.
	col.add_child(N.section_header(3, "Legacy screens"))
	_add_row(col, "Safe mode", "", func() -> void: _dispatch(SAFE_MODE, "Opening safe mode…"))
	_add_row(col, "Auto pickup", "", func() -> void: _dispatch(AUTO_PICKUP, "Opening auto pickup…"))
	_add_row(col, "Auto notes", "", func() -> void: _dispatch(AUTO_NOTES, "Opening auto notes…"))
	_add_row(col, "Distractions", "", func() -> void: _dispatch(DISTRACTIONS, "Opening distractions…"))
	_add_row(col, "Colors", "", func() -> void: _dispatch(COLORS, "Opening colors…"))
	_add_row(col, "Help", "", func() -> void: _dispatch(HELP, "Opening help…"))

	_status = N.micro_label("", N.NEUTRAL_600)
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	outer.add_child(_status)
	_highlight()

func _add_row(col: VBoxContainer, label: String, key: String, action: Callable) -> void:
	var index := _rows.size()
	var btn := Button.new()
	btn.flat = true
	btn.focus_mode = Control.FOCUS_NONE
	btn.custom_minimum_size = Vector2(0, 26)
	btn.pressed.connect(func() -> void:
		_selected = index
		_highlight()
		action.call())
	btn.mouse_entered.connect(func() -> void:
		_selected = index
		_highlight())
	col.add_child(btn)

	var row := HBoxContainer.new()
	row.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	row.offset_left = 10.0
	row.offset_right = -10.0
	row.add_theme_constant_override("separation", 10)
	row.mouse_filter = Control.MOUSE_FILTER_IGNORE
	btn.add_child(row)

	var text := Label.new()
	text.text = label
	text.add_theme_font_size_override("font_size", 12)
	text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	text.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	row.add_child(text)
	if key != "":
		var cap := Label.new()
		cap.text = key
		cap.add_theme_font_size_override("font_size", 10)
		cap.add_theme_color_override("font_color", N.NEUTRAL_600)
		cap.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
		row.add_child(cap)

	_rows.append(btn)

func _highlight() -> void:
	for i in _rows.size():
		var on := i == _selected
		var sb := StyleBoxFlat.new()
		sb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.15) if on else Color(0, 0, 0, 0)
		sb.border_color = N.ACCENT
		sb.border_width_left = 2 if on else 0
		_rows[i].add_theme_stylebox_override("normal", sb)
		_rows[i].add_theme_stylebox_override("hover", sb)
		_rows[i].add_theme_stylebox_override("pressed", sb)
		var label: Label = _rows[i].get_child(0).get_child(0)
		label.add_theme_color_override("font_color", N.TEXT if on else N.NEUTRAL_300)

## Losing a game to a stray keypress is not recoverable, so the two destructive
## entries want the same choice made twice.
func _confirm(action: int, what: String) -> void:
	if _confirming == action:
		_confirming = -1
		_dispatch(action, "%s…" % what)
		return
	_confirming = action
	_status.text = "%s? CHOOSE AGAIN TO CONFIRM" % what.to_upper()
	_status.add_theme_color_override("font_color", N.WARN)

func _dispatch(action: int, note: String) -> void:
	if _host == null or not _host.has_method("request_menu_action"):
		return
	# A queued action only runs when the game thread's command drain is willing,
	# and it refuses while any legacy C++ screen is shown -- including ones the
	# player never opened (the ambient soliloquy window). A quit clicked then
	# starved silently, which read as "quitting sometimes works". The player
	# asked to leave: closing whatever legacy screen is up is what they meant,
	# so nudge it shut with Escape until the queue is willing, bounded.
	if _host.has_method("commands_ready") and not _host.commands_ready() \
			and _host.has_method("push_input_event"):
		for nudge in 4:
			var esc := InputEventKey.new()
			esc.keycode = KEY_ESCAPE
			esc.physical_keycode = KEY_ESCAPE
			esc.pressed = true
			_host.push_input_event(esc)
			await get_tree().create_timer(0.3).timeout
			if _host.commands_ready():
				break
	var err := str(_host.request_menu_action(action))
	if err != "":
		_status.text = err.to_upper()
		_status.add_theme_color_override("font_color", N.BAD)
		return
	_status.text = note.to_upper()
	_status.add_theme_color_override("font_color", N.NEUTRAL_600)
	# A legacy screen needs the menu out of the way before it draws, and every
	# other action ends the interaction anyway.
	closed.emit()

func open() -> void:
	visible = true
	_selected = 0
	_confirming = -1
	if _status != null:
		_status.text = ""
	_highlight()

func _unhandled_input(event: InputEvent) -> void:
	if not visible or not (event is InputEventKey) or not event.pressed or event.echo:
		return
	match event.keycode:
		KEY_ESCAPE:
			closed.emit()
		KEY_UP:
			_selected = (_selected - 1 + _rows.size()) % _rows.size()
			_confirming = -1
			_highlight()
		KEY_DOWN:
			_selected = (_selected + 1) % _rows.size()
			_confirming = -1
			_highlight()
		KEY_ENTER, KEY_KP_ENTER, KEY_SPACE:
			if _selected >= 0 and _selected < _rows.size():
				_rows[_selected].pressed.emit()
		_:
			return
	get_viewport().set_input_as_handled()
