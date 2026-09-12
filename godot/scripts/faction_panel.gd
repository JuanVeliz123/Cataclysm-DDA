extends Control
## The faction screen ("Faction") -- four tabs (your camps, your followers,
## other factions, known creatures), a row list, a detail pane -- as a Godot
## Control.
##
## The game thread is blocked in faction_ui's run_in_godot while this is up.
## Each tab keeps its own `picked_*` pointer on the C++ side (a camp, an npc,
## a faction, a creature type -- four different types, not one shared
## cursor), so a row click here always means "this index, in whichever list
## the current tab published" rather than carrying any local identity.
##
## CONFIRM always closes this screen (talks to a follower, renames a camp,
## radios another faction, or does nothing on the creature tab) and may open
## another screen first -- the channel is suspended around that on the C++
## side, so this panel gets out of the way rather than sitting on top of it.
##
## See src/godot_faction_snapshot.h.

const N := preload("res://scripts/nocturne.gd")
const TAGS := preload("res://scripts/color_tags.gd")

var _host: Node
var _generation: int = -1
var _tab: int = 0
var _selected: int = 0

var _title: Label
var _tab_row: HBoxContainer
var _row_col: VBoxContainer
var _row_nodes: Array[Button] = []
var _detail: RichTextLabel
var _detail_scroll: ScrollContainer

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _detail != null:
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.55)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(dim)

	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -480.0
	frame.offset_right = 480.0
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
	_title.text = "FACTION"
	_title.add_theme_font_size_override("font_size", 16)
	_title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(_title)

	_tab_row = HBoxContainer.new()
	_tab_row.add_theme_constant_override("separation", 3)
	col.add_child(_tab_row)

	col.add_child(N.fade_rule())

	var split := HBoxContainer.new()
	split.add_theme_constant_override("separation", N.SPACE_M)
	split.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(split)

	var left := ScrollContainer.new()
	left.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	left.custom_minimum_size = Vector2(260, 0)
	split.add_child(left)
	_row_col = VBoxContainer.new()
	_row_col.add_theme_constant_override("separation", 2)
	_row_col.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	left.add_child(_row_col)

	var right := VBoxContainer.new()
	right.add_theme_constant_override("separation", N.SPACE_S)
	right.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	split.add_child(right)
	_detail_scroll = ScrollContainer.new()
	_detail_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_detail_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	right.add_child(_detail_scroll)
	_detail = RichTextLabel.new()
	_detail.bbcode_enabled = true
	_detail.fit_content = true
	_detail.selection_enabled = true
	_detail.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_detail.add_theme_font_size_override("normal_font_size", 12)
	_detail_scroll.add_child(_detail)

	col.add_child(N.fade_rule())

	var confirm_btn := Button.new()
	confirm_btn.text = "Confirm"
	confirm_btn.focus_mode = Control.FOCUS_NONE
	N.apply_button(confirm_btn)
	confirm_btn.pressed.connect(func() -> void: _act("CONFIRM"))
	col.add_child(confirm_btn)

	col.add_child(N.micro_label("↑↓ select · ←→ tabs · Enter confirm · Esc close", N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_faction_state"):
		return
	if _detail == null:
		_build()
	var gen: int = int(_host.faction_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_faction_state()
	var title := str(d.get("title", ""))
	if title != "":
		_title.text = title.to_upper()
	_tab = int(d.get("tab", 0))
	_selected = int(d.get("selected", 0))
	_rebuild_tabs(d.get("tabs", []))
	_rebuild_rows(d.get("rows", []))
	_detail.text = TAGS.to_bbcode(str(d.get("detail", "")))

func _rebuild_tabs(tabs: Array) -> void:
	for child in _tab_row.get_children():
		_tab_row.remove_child(child)
		child.queue_free()
	for i in tabs.size():
		var idx := i
		var btn := Button.new()
		btn.text = str(tabs[i])
		btn.focus_mode = Control.FOCUS_NONE
		btn.pressed.connect(func() -> void: _select_tab(idx))
		var on := i == _tab
		var sb := StyleBoxFlat.new()
		sb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.15) if on else Color(0, 0, 0, 0)
		sb.border_color = N.ACCENT if on else N.NEUTRAL_800
		sb.set_border_width_all(1)
		sb.border_width_bottom = 2 if on else 1
		sb.content_margin_left = 12
		sb.content_margin_right = 12
		sb.content_margin_top = 5
		sb.content_margin_bottom = 5
		for style in ["normal", "hover", "pressed"]:
			btn.add_theme_stylebox_override(style, sb)
		btn.add_theme_color_override("font_color", N.TEXT if on else N.NEUTRAL_500)
		btn.add_theme_font_size_override("font_size", 11)
		_tab_row.add_child(btn)

func _rebuild_rows(rows: Array) -> void:
	for child in _row_col.get_children():
		_row_col.remove_child(child)
		child.queue_free()
	_row_nodes.clear()
	if rows.is_empty():
		var empty := Label.new()
		empty.text = "(none)"
		empty.add_theme_color_override("font_color", N.NEUTRAL_600)
		_row_col.add_child(empty)
		return
	for i in rows.size():
		var idx := i
		var btn := Button.new()
		btn.text = TAGS.strip(str(rows[i]))
		btn.alignment = HORIZONTAL_ALIGNMENT_LEFT
		btn.autowrap_mode = TextServer.AUTOWRAP_WORD
		btn.focus_mode = Control.FOCUS_NONE
		btn.pressed.connect(func() -> void: _select_row(idx))
		var on := i == _selected
		var sb := StyleBoxFlat.new()
		sb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.12) if on else Color(0, 0, 0, 0)
		sb.border_color = N.ACCENT if on else Color(0, 0, 0, 0)
		sb.set_border_width_all(0)
		sb.border_width_left = 2 if on else 0
		sb.content_margin_left = 10
		sb.content_margin_right = 8
		sb.content_margin_top = 4
		sb.content_margin_bottom = 4
		for style in ["normal", "hover", "pressed"]:
			btn.add_theme_stylebox_override(style, sb)
		btn.add_theme_color_override("font_color", N.TEXT if on else N.NEUTRAL_400)
		btn.add_theme_font_size_override("font_size", 12)
		_row_col.add_child(btn)
		_row_nodes.append(btn)

# --- actions ------------------------------------------------------------------

func _act(action: String) -> void:
	if _host != null and _host.has_method("faction_action"):
		_host.faction_action(action)

func _select_tab(index: int) -> void:
	if index != _tab and _host != null and _host.has_method("faction_select_tab"):
		_host.faction_select_tab(index)

func _select_row(index: int) -> void:
	if index != _selected and _host != null and _host.has_method("faction_select_row"):
		_host.faction_select_row(index)

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
	match event.keycode:
		KEY_ESCAPE:
			_act("QUIT")
		KEY_TAB:
			_act("PREV_TAB" if event.shift_pressed else "NEXT_TAB")
		KEY_LEFT:
			_act("PREV_TAB")
		KEY_RIGHT:
			_act("NEXT_TAB")
		KEY_UP:
			_act("UP")
		KEY_DOWN:
			_act("DOWN")
		KEY_ENTER, KEY_KP_ENTER:
			_act("CONFIRM")
		KEY_PAGEUP:
			_detail_scroll.scroll_vertical -= int(_detail_scroll.size.y)
		KEY_PAGEDOWN:
			_detail_scroll.scroll_vertical += int(_detail_scroll.size.y)
		_:
			return
	get_viewport().set_input_as_handled()
