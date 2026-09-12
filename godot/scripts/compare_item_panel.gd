extends Control
## `compare_item_menu`: two items' info panes side by side, each independently
## scrolled, with an optional CONFIRM/QUIT bar underneath -- as a Godot
## Control.
##
## No selection state, and no tabs: `textwin_panel.gd`'s single-pane shape
## doubled, since two items need to stay visible at once rather than
## switching between them. Reused `format_item_info()` + `remove_color_tags()`
## on the C++ side, the same conversion `textwin_panel.gd`'s own data already
## goes through -- no colored-span support exists here either.
##
## The game thread is blocked in compare_item_menu::show() while this is up;
## see src/godot_compare_snapshot.h.

const N := preload("res://scripts/nocturne.gd")

var _host: Node
var _generation: int = -1
var _has_confirm := false

var _title: Label
var _left_name: Label
var _right_name: Label
var _left_body: RichTextLabel
var _right_body: RichTextLabel
var _left_scroll: ScrollContainer
var _right_scroll: ScrollContainer
var _confirm_row: HBoxContainer
var _confirm_label: Label

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build_pane() -> Dictionary:
	var pane_col := VBoxContainer.new()
	pane_col.add_theme_constant_override("separation", 4)
	pane_col.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var name_lbl := Label.new()
	name_lbl.add_theme_font_size_override("font_size", 14)
	name_lbl.add_theme_color_override("font_color", N.TEXT)
	name_lbl.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	pane_col.add_child(name_lbl)

	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	pane_col.add_child(scroll)

	var body := RichTextLabel.new()
	body.bbcode_enabled = false
	body.fit_content = true
	body.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	body.add_theme_color_override("default_color", N.NEUTRAL_300)
	body.add_theme_font_size_override("normal_font_size", 12)
	scroll.add_child(body)

	return {"col": pane_col, "name": name_lbl, "scroll": scroll, "body": body}

func _build() -> void:
	if _left_body != null:
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.5)
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
	_title.text = "COMPARE"
	_title.add_theme_font_size_override("font_size", 16)
	_title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(_title)

	col.add_child(N.fade_rule())

	var panes := HBoxContainer.new()
	panes.add_theme_constant_override("separation", N.SPACE_M)
	panes.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(panes)

	var left := _build_pane()
	panes.add_child(left["col"])
	_left_name = left["name"]
	_left_scroll = left["scroll"]
	_left_body = left["body"]

	var divider := ColorRect.new()
	divider.color = N.NEUTRAL_800
	divider.custom_minimum_size = Vector2(1, 0)
	panes.add_child(divider)

	var right := _build_pane()
	panes.add_child(right["col"])
	_right_name = right["name"]
	_right_scroll = right["scroll"]
	_right_body = right["body"]

	col.add_child(N.fade_rule())

	_confirm_row = HBoxContainer.new()
	_confirm_row.add_theme_constant_override("separation", N.SPACE_M)
	_confirm_row.visible = false
	col.add_child(_confirm_row)
	_confirm_label = Label.new()
	_confirm_label.add_theme_color_override("font_color", N.TEXT)
	_confirm_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_confirm_row.add_child(_confirm_label)
	var confirm_btn := Button.new()
	confirm_btn.text = "Confirm"
	confirm_btn.focus_mode = Control.FOCUS_NONE
	N.apply_button(confirm_btn)
	confirm_btn.pressed.connect(func() -> void: _act("CONFIRM"))
	_confirm_row.add_child(confirm_btn)
	var quit_btn := Button.new()
	quit_btn.text = "Cancel"
	quit_btn.focus_mode = Control.FOCUS_NONE
	N.apply_button(quit_btn)
	quit_btn.pressed.connect(func() -> void: _act("QUIT"))
	_confirm_row.add_child(quit_btn)

	col.add_child(N.micro_label("↑↓ scroll · Esc close", N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_compare_item_state"):
		return
	if _left_body == null:
		_build()
	var gen: int = int(_host.compare_item_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_compare_item_state()
	_left_name.text = str(d.get("first_name", ""))
	_right_name.text = str(d.get("second_name", ""))
	_left_body.text = str(d.get("first_body", ""))
	_right_body.text = str(d.get("second_body", ""))
	_left_scroll.scroll_vertical = 0
	_right_scroll.scroll_vertical = 0

	var confirm_message := str(d.get("confirm_message", ""))
	_has_confirm = confirm_message != ""
	_confirm_row.visible = _has_confirm
	_confirm_label.text = confirm_message

# --- actions ------------------------------------------------------------------

func _act(action: String) -> void:
	if _host != null and _host.has_method("compare_item_action"):
		_host.compare_item_action(action)

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
		KEY_ENTER, KEY_KP_ENTER:
			if _has_confirm:
				_act("CONFIRM")
			else:
				return
		KEY_UP:
			_left_scroll.scroll_vertical -= 40
			_right_scroll.scroll_vertical -= 40
		KEY_DOWN:
			_left_scroll.scroll_vertical += 40
			_right_scroll.scroll_vertical += 40
		_:
			return
	get_viewport().set_input_as_handled()
