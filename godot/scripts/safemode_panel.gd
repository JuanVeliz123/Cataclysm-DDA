extends Control
## Safe mode manager (MENU-14, `safemode::show()` in `src/safemode_ui.cpp`): a
## two-tab (global / character) rule editor, as a Godot Control.
##
## Same loop-split takeover as `auto_note`: the tab is authoritative on the
## game side (`safemode::gui_tab`), addressed here by absolute row index into
## whichever tab's rule list is current. A cell click is addressed as
## (row, column), encoded into one request int as `row * 6 + column` --
## `column` in the same order the legacy screen's `Columns` enum used (Rule,
## Attitude, Proximity, Whitelist/Blacklist, Category, Movement mode) -- the
## same encoding `ColorManagerSnapshot` uses for its own (row, column) picks.
##
## The rule-text and proximity-distance edits are `string_input_popup_imgui`,
## already routed through the Godot text-prompt channel -- no bespoke panel
## needed. TEST_RULE's match list is a plain `uilist`, likewise already a
## Godot panel. Both are suspended around the same way `AutoNoteSnapshot`'s
## GODOT_SYMBOL suspends for its nested popups.
##
## See src/godot_safemode_snapshot.h.

const N := preload("res://scripts/nocturne.gd")

const COL_RULE := 0
const COL_ATTITUDE := 1
const COL_PROXIMITY := 2
const COL_WHITELIST := 3
const COL_CATEGORY := 4
const COL_MOVEMENT := 5
const NUM_COLUMNS := 6

var _host: Node
var _generation: int = -1

var _title: Label
var _tab_global: Button
var _tab_character: Button
var _safe_mode_label: Label
var _locked_label: Label
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
	frame.offset_left = -560.0
	frame.offset_right = 560.0
	frame.offset_top = -360.0
	frame.offset_bottom = 360.0
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
	_title.text = "SAFE MODE MANAGER"
	_title.add_theme_font_size_override("font_size", 16)
	_title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(_title)

	var tab_row := HBoxContainer.new()
	tab_row.add_theme_constant_override("separation", N.SPACE_S)
	col.add_child(tab_row)
	_tab_global = Button.new()
	_tab_global.text = "Global"
	_tab_global.focus_mode = Control.FOCUS_NONE
	N.apply_button(_tab_global)
	_tab_global.pressed.connect(func() -> void: _set_tab(0))
	tab_row.add_child(_tab_global)
	_tab_character = Button.new()
	_tab_character.text = "Character"
	_tab_character.focus_mode = Control.FOCUS_NONE
	N.apply_button(_tab_character)
	_tab_character.pressed.connect(func() -> void: _set_tab(1))
	tab_row.add_child(_tab_character)
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	tab_row.add_child(spacer)
	_safe_mode_label = Label.new()
	_safe_mode_label.add_theme_color_override("font_color", N.TEXT)
	tab_row.add_child(_safe_mode_label)

	var add_row := HBoxContainer.new()
	add_row.add_theme_constant_override("separation", N.SPACE_S)
	col.add_child(add_row)
	var add_btn := Button.new()
	add_btn.text = "Add Rule"
	add_btn.focus_mode = Control.FOCUS_NONE
	N.apply_button(add_btn)
	add_btn.pressed.connect(func() -> void: _act("ADD_RULE"))
	add_row.add_child(add_btn)
	var add_default_btn := Button.new()
	add_default_btn.text = "Add Default Ruleset"
	add_default_btn.focus_mode = Control.FOCUS_NONE
	N.apply_button(add_default_btn)
	add_default_btn.pressed.connect(func() -> void: _act("ADD_DEFAULT_RULESET"))
	add_row.add_child(add_default_btn)

	col.add_child(N.fade_rule())

	_locked_label = Label.new()
	_locked_label.text = "Please load a character first to use this page"
	_locked_label.add_theme_color_override("font_color", N.NEUTRAL_600)
	_locked_label.visible = false
	col.add_child(_locked_label)

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
	if _host == null or not _host.has_method("get_safemode_state"):
		return
	if _list == null:
		_build()
	var gen: int = int(_host.safemode_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_safemode_state()
	var title := str(d.get("title", ""))
	if title != "":
		_title.text = title.to_upper()

	var tab := int(d.get("tab", 0))
	_tab_global.disabled = tab == 0
	_tab_character.disabled = tab == 1

	var on := bool(d.get("safe_mode_on", false))
	_safe_mode_label.text = "Safe mode: %s" % ("On" if on else "Off")

	var locked := bool(d.get("character_locked", false))
	_locked_label.visible = locked
	_last_show_swap = bool(d.get("show_swap", false))
	_rebuild_rows(d.get("rows", []) if not locked else [])

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
				rule_text if rule_text != "" else "<empty rule>", 180))
		line.add_child(_cell(row_index, COL_ATTITUDE, str(rd.get("attitude", "")), 90))
		line.add_child(_cell(row_index, COL_PROXIMITY, str(rd.get("proximity", "")), 50))
		line.add_child(_cell(row_index, COL_WHITELIST,
				"Whitelist" if bool(rd.get("whitelist", false)) else "Blacklist", 90))
		line.add_child(_cell(row_index, COL_CATEGORY, str(rd.get("category", "")), 80))
		line.add_child(_cell(row_index, COL_MOVEMENT, str(rd.get("movement_mode", "")), 80))

		var active_box := CheckBox.new()
		active_box.button_pressed = bool(rd.get("active", true))
		active_box.focus_mode = Control.FOCUS_NONE
		active_box.toggled.connect(func(pressed: bool) -> void: _set_active(row_index, pressed))
		line.add_child(active_box)

		line.add_child(_small_btn("^", "Move up", func() -> void: _host.safemode_move_up(row_index)))
		line.add_child(_small_btn("v", "Move down", func() -> void: _host.safemode_move_down(row_index)))
		line.add_child(_small_btn("Cp", "Copy", func() -> void: _host.safemode_copy(row_index)))
		if bool(_last_show_swap):
			line.add_child(_small_btn("Sw", "Swap global/character", func() -> void: _host.safemode_swap(row_index)))
		line.add_child(_small_btn("T", "Test", func() -> void: _host.safemode_test(row_index)))
		line.add_child(_small_btn("x", "Remove", func() -> void: _host.safemode_remove(row_index)))

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
	if _host != null and _host.has_method("safemode_action"):
		_host.safemode_action(action)

func _confirm(row_index: int, col_index: int) -> void:
	if _host != null and _host.has_method("safemode_confirm"):
		_host.safemode_confirm(row_index * NUM_COLUMNS + col_index)

func _set_tab(tab: int) -> void:
	if _host != null and _host.has_method("safemode_tab"):
		_host.safemode_tab(tab)

func _set_active(row_index: int, active: bool) -> void:
	if _host == null:
		return
	if active and _host.has_method("safemode_enable"):
		_host.safemode_enable(row_index)
	elif not active and _host.has_method("safemode_disable"):
		_host.safemode_disable(row_index)

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
