extends Control
## Auto pickup manager (MENU-14, `auto_pickup::user_interface::show()` in
## `src/auto_pickup.cpp`): a rule editor, as a Godot Control.
##
## Same shape as `safemode_panel.gd`, but a generic one: `user_interface` is
## shared by the player screen (1 or 2 tabs -- global, plus character only
## once a character is loaded) and the NPC pickup-rules screen (exactly 1
## tab, titled with the NPC's own name). Tab buttons are built from the
## published `tab_titles` list rather than hardcoded Global/Character labels.
## A cell click is addressed as (row, column), encoded into one request int
## as `row * 2 + column` -- column 0 is the rule text, column 1 is
## include/exclude -- the same encoding `SafemodePanel` uses for its own six
## columns.
##
## The rule-text edit is `string_input_popup_imgui`, already routed through
## the Godot text-prompt channel -- no bespoke panel needed. TEST_RULE's
## match list is a plain `uilist`, likewise already a Godot panel. Both are
## suspended around the same way `SafemodePanel`'s own GODOT_CONFIRM/
## GODOT_TEST suspend for theirs.
##
## ADD_RULE just appends a blank rule rather than opening the text prompt
## immediately -- click the new row's rule cell to edit it, the same
## simplified re-presentation `safemode`'s own ADD_RULE settled on.
##
## See src/godot_auto_pickup_snapshot.h.

const N := preload("res://scripts/nocturne.gd")

const COL_RULE := 0
const COL_EXCLUDE := 1
const NUM_COLUMNS := 2

var _host: Node
var _generation: int = -1

var _title: Label
var _tab_row: HBoxContainer
var _tab_buttons: Array = []
var _on_label: Label
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
	frame.offset_left = -520.0
	frame.offset_right = 520.0
	frame.offset_top = -340.0
	frame.offset_bottom = 340.0
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
	_title.text = "AUTO PICKUP MANAGER"
	_title.add_theme_font_size_override("font_size", 16)
	_title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(_title)

	var top_row := HBoxContainer.new()
	top_row.add_theme_constant_override("separation", N.SPACE_S)
	col.add_child(top_row)
	_tab_row = HBoxContainer.new()
	_tab_row.add_theme_constant_override("separation", N.SPACE_S)
	top_row.add_child(_tab_row)
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	top_row.add_child(spacer)
	_on_label = Label.new()
	_on_label.add_theme_color_override("font_color", N.TEXT)
	top_row.add_child(_on_label)

	var add_row := HBoxContainer.new()
	add_row.add_theme_constant_override("separation", N.SPACE_S)
	col.add_child(add_row)
	var add_btn := Button.new()
	add_btn.text = "Add Rule"
	add_btn.focus_mode = Control.FOCUS_NONE
	N.apply_button(add_btn)
	add_btn.pressed.connect(func() -> void: _act("ADD_RULE"))
	add_row.add_child(add_btn)
	var toggle_btn := Button.new()
	toggle_btn.text = "Toggle Auto Pickup"
	toggle_btn.focus_mode = Control.FOCUS_NONE
	N.apply_button(toggle_btn)
	toggle_btn.pressed.connect(func() -> void: _act("SWITCH_AUTO_PICKUP_OPTION"))
	add_row.add_child(toggle_btn)

	col.add_child(N.fade_rule())

	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(scroll)
	_list = VBoxContainer.new()
	_list.add_theme_constant_override("separation", 3)
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_list)

	col.add_child(N.fade_rule())
	col.add_child(N.micro_label(
			"Esc close · click a cell to edit it · Up/Dn/Cp/T/x act on a row",
			N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_auto_pickup_state"):
		return
	if _list == null:
		_build()
	var gen: int = int(_host.auto_pickup_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_auto_pickup_state()
	var title := str(d.get("title", ""))
	if title != "":
		_title.text = title.to_upper()

	var tab := int(d.get("tab", 0))
	var titles: Array = d.get("tab_titles", [])
	_rebuild_tabs(titles, tab)

	var on := bool(d.get("auto_pickup_on", false))
	_on_label.text = "Auto pickup: %s" % ("On" if on else "Off")

	_last_show_swap = bool(d.get("show_swap", false))
	_rebuild_rows(d.get("rows", []))

func _rebuild_tabs(titles: Array, current: int) -> void:
	if titles.size() != _tab_buttons.size():
		for btn in _tab_buttons:
			_tab_row.remove_child(btn)
			btn.queue_free()
		_tab_buttons.clear()
		for i in titles.size():
			var tab_index := i
			var btn := Button.new()
			btn.focus_mode = Control.FOCUS_NONE
			N.apply_button(btn)
			btn.pressed.connect(func() -> void: _set_tab(tab_index))
			_tab_row.add_child(btn)
			_tab_buttons.append(btn)
	for i in titles.size():
		var btn: Button = _tab_buttons[i]
		btn.text = str(titles[i])
		btn.disabled = i == current

func _rebuild_rows(rows: Array) -> void:
	for child in _list.get_children():
		_list.remove_child(child)
		child.queue_free()
	for i in rows.size():
		var rd: Dictionary = rows[i]
		var row_index := i
		var line := HBoxContainer.new()
		line.add_theme_constant_override("separation", 3)

		var num := Label.new()
		num.text = str(i + 1)
		num.custom_minimum_size = Vector2(24, 0)
		num.add_theme_color_override("font_color", N.NEUTRAL_600 if not bool(rd.get("active", true)) else N.TEXT)
		line.add_child(num)

		var rule_text := str(rd.get("rule", ""))
		line.add_child(_cell(row_index, COL_RULE,
				rule_text if rule_text != "" else "<empty rule>", 320))
		line.add_child(_cell(row_index, COL_EXCLUDE,
				"Exclude" if bool(rd.get("exclude", false)) else "Include", 90))

		var active_box := CheckBox.new()
		active_box.button_pressed = bool(rd.get("active", true))
		active_box.focus_mode = Control.FOCUS_NONE
		active_box.toggled.connect(func(pressed: bool) -> void: _set_active(row_index, pressed))
		line.add_child(active_box)

		line.add_child(_small_btn("^", "Move up", func() -> void: _host.auto_pickup_move_up(row_index)))
		line.add_child(_small_btn("v", "Move down", func() -> void: _host.auto_pickup_move_down(row_index)))
		line.add_child(_small_btn("Cp", "Copy", func() -> void: _host.auto_pickup_copy(row_index)))
		if bool(_last_show_swap):
			line.add_child(_small_btn("Sw", "Swap tab", func() -> void: _host.auto_pickup_swap(row_index)))
		line.add_child(_small_btn("T", "Test", func() -> void: _host.auto_pickup_test(row_index)))
		line.add_child(_small_btn("x", "Remove", func() -> void: _host.auto_pickup_remove(row_index)))

		_list.add_child(line)

var _last_show_swap := false

func _cell(row_index: int, col_index: int, text: String, min_width: int) -> Button:
	var btn := Button.new()
	btn.text = text
	btn.focus_mode = Control.FOCUS_NONE
	btn.custom_minimum_size = Vector2(min_width, 0)
	btn.clip_text = true
	N.apply_button(btn)
	btn.pressed.connect(func() -> void: _confirm(row_index, col_index))
	return btn

func _small_btn(text: String, tip: String, cb: Callable) -> Button:
	var btn := Button.new()
	btn.text = text
	btn.tooltip_text = tip
	btn.focus_mode = Control.FOCUS_NONE
	btn.custom_minimum_size = Vector2(28, 0)
	N.apply_button(btn)
	btn.pressed.connect(cb)
	return btn

# --- actions ------------------------------------------------------------------

func _act(action: String) -> void:
	if _host != null and _host.has_method("auto_pickup_action"):
		_host.auto_pickup_action(action)

func _confirm(row_index: int, col_index: int) -> void:
	if _host != null and _host.has_method("auto_pickup_confirm"):
		_host.auto_pickup_confirm(row_index * NUM_COLUMNS + col_index)

func _set_tab(tab: int) -> void:
	if _host != null and _host.has_method("auto_pickup_tab"):
		_host.auto_pickup_tab(tab)

func _set_active(row_index: int, active: bool) -> void:
	if _host == null:
		return
	if active and _host.has_method("auto_pickup_enable"):
		_host.auto_pickup_enable(row_index)
	elif not active and _host.has_method("auto_pickup_disable"):
		_host.auto_pickup_disable(row_index)

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
