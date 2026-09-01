extends Control
## Auto notes manager (MENU-14): a per-map-extra toggle list with two tabs
## (character / global) and a per-row "change symbol" popup, as a Godot
## Control.
##
## Same loop-split takeover as `medical_ui`: the tab and the row cursor are
## both members on the game side (`auto_note_manager_gui::bCharacter` /
## `currentLine`), addressed here by absolute index into whichever tab's
## display list is current -- a tab switch always shows a different list, not
## a filtered view of one. "Change symbol" opens a nested symbol prompt and
## colour picker, both already Godot panels; this channel suspends around
## that call the same way MedicalSnapshot's APPLY suspends for its item
## picker, so this panel does not answer the door while they are up.
##
## See src/godot_auto_note_snapshot.h.

const N := preload("res://scripts/nocturne.gd")
const CT := preload("res://scripts/color_tags.gd")

var _host: Node
var _generation: int = -1

var _title: Label
var _tab_char: Button
var _tab_global: Button
var _switch_btn: Button
var _list: VBoxContainer
var _empty_label: Label

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
	_title.text = "AUTO NOTES MANAGER"
	_title.add_theme_font_size_override("font_size", 16)
	_title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(_title)

	var tab_row := HBoxContainer.new()
	tab_row.add_theme_constant_override("separation", N.SPACE_S)
	col.add_child(tab_row)
	_tab_char = Button.new()
	_tab_char.text = "Character"
	_tab_char.focus_mode = Control.FOCUS_NONE
	N.apply_button(_tab_char)
	_tab_char.pressed.connect(func() -> void: _set_tab(0))
	tab_row.add_child(_tab_char)
	_tab_global = Button.new()
	_tab_global.text = "Global"
	_tab_global.focus_mode = Control.FOCUS_NONE
	N.apply_button(_tab_global)
	_tab_global.pressed.connect(func() -> void: _set_tab(1))
	tab_row.add_child(_tab_global)
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	tab_row.add_child(spacer)
	_switch_btn = Button.new()
	_switch_btn.focus_mode = Control.FOCUS_NONE
	N.apply_button(_switch_btn)
	_switch_btn.pressed.connect(func() -> void: _act("SWITCH_OPTION"))
	tab_row.add_child(_switch_btn)

	col.add_child(N.fade_rule())

	_empty_label = Label.new()
	_empty_label.text = "Discover more special encounters to populate this list"
	_empty_label.add_theme_color_override("font_color", N.NEUTRAL_600)
	_empty_label.visible = false
	col.add_child(_empty_label)

	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(scroll)
	_list = VBoxContainer.new()
	_list.add_theme_constant_override("separation", 2)
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_list)

	col.add_child(N.fade_rule())
	col.add_child(N.micro_label(
			"Esc close · click a box to toggle · Symbol to change how it marks the map",
			N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_auto_note_state"):
		return
	if _list == null:
		_build()
	var gen: int = int(_host.auto_note_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_auto_note_state()
	var title := str(d.get("title", ""))
	if title != "":
		_title.text = title.to_upper()

	var tab := int(d.get("tab", 0))
	_tab_char.disabled = tab == 0
	_tab_global.disabled = tab == 1

	var map_extras_on := bool(d.get("auto_notes_map_extras", false))
	_switch_btn.text = "Auto notes: %s" % ("On" if map_extras_on else "Off")

	var empty_mode := bool(d.get("empty_mode", false))
	_empty_label.visible = empty_mode
	_rebuild_rows(d.get("rows", []) if not empty_mode else [])

func _rebuild_rows(rows: Array) -> void:
	for child in _list.get_children():
		_list.remove_child(child)
		child.queue_free()
	for i in rows.size():
		var rd: Dictionary = rows[i]
		var index := i
		var line := HBoxContainer.new()
		line.add_theme_constant_override("separation", 6)

		var box := CheckBox.new()
		box.text = str(rd.get("name", ""))
		box.button_pressed = bool(rd.get("enabled", false))
		box.focus_mode = Control.FOCUS_NONE
		box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		box.add_theme_color_override("font_color", N.TEXT)
		box.toggled.connect(func(_pressed: bool) -> void: _toggle(index))
		line.add_child(box)

		var sym := Label.new()
		sym.text = str(rd.get("symbol", ""))
		var hex := CT.hex_for_name(str(rd.get("symbol_color", "")))
		sym.add_theme_color_override("font_color", Color(hex) if hex != "" else N.TEXT)
		sym.custom_minimum_size = Vector2(24, 0)
		sym.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		line.add_child(sym)

		var symbol_btn := Button.new()
		symbol_btn.text = "Symbol"
		symbol_btn.focus_mode = Control.FOCUS_NONE
		N.apply_button(symbol_btn)
		symbol_btn.pressed.connect(func() -> void: _symbol(index))
		line.add_child(symbol_btn)

		_list.add_child(line)

# --- actions ------------------------------------------------------------------

func _act(action: String) -> void:
	if _host != null and _host.has_method("auto_note_action"):
		_host.auto_note_action(action)

func _toggle(index: int) -> void:
	if _host != null and _host.has_method("auto_note_toggle"):
		_host.auto_note_toggle(index)

func _symbol(index: int) -> void:
	if _host != null and _host.has_method("auto_note_symbol"):
		_host.auto_note_symbol(index)

func _set_tab(tab: int) -> void:
	if _host != null and _host.has_method("auto_note_tab"):
		_host.auto_note_tab(tab)

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
