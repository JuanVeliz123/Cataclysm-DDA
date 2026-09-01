extends Control
## Color manager (MENU-14, `color_manager::show_gui()` in `src/color.cpp`): a
## flat list of every named color with two independently-pickable cells
## (Normal / Invert), as a Godot Control.
##
## No tabs, unlike most of MENU-14's other screens, but each cell click opens
## a nested "pick a custom color" `uilist` -- already a Godot panel; this
## channel suspends around that call the same way `AutoNoteSnapshot`'s
## `GODOT_SYMBOL` suspends for its symbol/colour popups, so this panel does
## not answer the door while the picker is up. A row/column pair is encoded
## into one request int (`row * 2 + (col == 2 ? 1 : 0)`), matching
## `ColorManagerSnapshot`'s encoding on the C++ side.
##
## See src/godot_color_manager_snapshot.h.

const N := preload("res://scripts/nocturne.gd")
const CT := preload("res://scripts/color_tags.gd")

var _host: Node
var _generation: int = -1

var _title: Label
var _list: VBoxContainer

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _list != null:
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.55)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(dim)

	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -460.0
	frame.offset_right = 460.0
	frame.offset_top = -320.0
	frame.offset_bottom = 320.0
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(N.BG.r, N.BG.g, N.BG.b, 0.99)
	sb.border_color = N.NEUTRAL_800
	sb.set_border_width_all(1)
	sb.border_width_top = 3
	sb.content_margin_left = 24
	sb.content_margin_right = 24
	sb.content_margin_top = 20
	sb.content_margin_bottom = 16
	frame.add_theme_stylebox_override("panel", sb)
	add_child(frame)

	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", N.SPACE_S)
	frame.add_child(col)

	_title = Label.new()
	_title.text = "COLOR MANAGER"
	_title.add_theme_font_size_override("font_size", 16)
	_title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(_title)

	var header_row := HBoxContainer.new()
	header_row.add_theme_constant_override("separation", N.SPACE_S)
	col.add_child(header_row)
	var template_btn := Button.new()
	template_btn.text = "Load Template"
	template_btn.focus_mode = Control.FOCUS_NONE
	N.apply_button(template_btn)
	template_btn.pressed.connect(func() -> void: _act("GODOT_TEMPLATE"))
	header_row.add_child(template_btn)
	var theme_btn := Button.new()
	theme_btn.text = "Load Base Theme"
	theme_btn.focus_mode = Control.FOCUS_NONE
	N.apply_button(theme_btn)
	theme_btn.pressed.connect(func() -> void: _act("GODOT_THEME"))
	header_row.add_child(theme_btn)

	col.add_child(N.fade_rule())

	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(scroll)
	_list = VBoxContainer.new()
	_list.add_theme_constant_override("separation", 2)
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_list)

	col.add_child(N.fade_rule())
	col.add_child(N.micro_label(
			"Esc close · click a color to pick a custom one · x removes it", N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_color_manager_state"):
		return
	if _list == null:
		_build()
	var gen: int = int(_host.color_manager_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_color_manager_state()
	var title := str(d.get("title", ""))
	if title != "":
		_title.text = title.to_upper()

	_rebuild_rows(d.get("rows", []))

func _rebuild_rows(rows: Array) -> void:
	for child in _list.get_children():
		_list.remove_child(child)
		child.queue_free()
	for i in rows.size():
		var rd: Dictionary = rows[i]
		var row_index := i
		var line := HBoxContainer.new()
		line.add_theme_constant_override("separation", 6)

		var name_label := Label.new()
		name_label.text = str(rd.get("name", ""))
		name_label.add_theme_color_override("font_color", N.TEXT)
		name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		line.add_child(name_label)

		line.add_child(_build_cell(rd.get("normal", {}), row_index, 1))
		line.add_child(_build_cell(rd.get("invert", {}), row_index, 2))

		_list.add_child(line)

func _build_cell(cd: Dictionary, row_index: int, col_index: int) -> Control:
	var cell := HBoxContainer.new()
	cell.add_theme_constant_override("separation", 2)
	cell.custom_minimum_size = Vector2(160, 0)

	var swatch := Button.new()
	swatch.text = str(cd.get("label", "default"))
	swatch.focus_mode = Control.FOCUS_NONE
	N.apply_button(swatch)
	var hex := CT.hex_for_name(str(cd.get("color_name", "")))
	if hex != "":
		swatch.add_theme_color_override("font_color", Color(hex))
	swatch.pressed.connect(func() -> void: _pick(row_index, col_index))
	cell.add_child(swatch)

	if bool(cd.get("has_custom", false)):
		var remove_btn := Button.new()
		remove_btn.text = "x"
		remove_btn.focus_mode = Control.FOCUS_NONE
		N.apply_button(remove_btn)
		remove_btn.pressed.connect(func() -> void: _remove(row_index, col_index))
		cell.add_child(remove_btn)

	return cell

# --- actions ------------------------------------------------------------------

func _act(action: String) -> void:
	if _host != null and _host.has_method("color_manager_action"):
		_host.color_manager_action(action)

func _pick(row_index: int, col_index: int) -> void:
	if _host != null and _host.has_method("color_manager_pick"):
		_host.color_manager_pick(row_index * 2 + (1 if col_index == 2 else 0))

func _remove(row_index: int, col_index: int) -> void:
	if _host != null and _host.has_method("color_manager_remove"):
		_host.color_manager_remove(row_index * 2 + (1 if col_index == 2 else 0))

## While a prompt or menu raised on top is up (its own Godot panel), the keys
## belong to it, not to this screen.
func _yield_to_overlays() -> bool:
	if _host == null:
		return false
	if _host.has_method("popup_active") and _host.popup_active():
		return true
	if _host.has_method("uilist_active") and _host.uilist_active():
		return true
	return false

func _unhandled_input(event: InputEvent) -> void:
	if not visible or not (event is InputEventKey) or not event.pressed or event.echo:
		return
	if _yield_to_overlays():
		return
	if event.keycode == KEY_ESCAPE:
		_act("QUIT")
		get_viewport().set_input_as_handled()
