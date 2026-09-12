extends Control
## The end-of-game screen: an ascii-art tombstone plus an optional "last
## words" field, as a Godot Control.
##
## No selection state -- CONFIRM is the only outcome. The body is plain text
## (color tags stripped on the C++ side, matching `textwin_panel.gd`'s own
## precedent) with any per-screen overlay text already spliced into it; see
## `compose_end_screen_body()` in src/end_screen.cpp.
##
## The game thread is blocked in end_screen_data::draw_end_screen_ui while
## this is up; see src/godot_end_screen_snapshot.h.

const N := preload("res://scripts/nocturne.gd")

var _host: Node
var _generation: int = -1
var _has_text_input := false

var _body: RichTextLabel
var _scroll: ScrollContainer
var _entry_row: HBoxContainer
var _entry_label: Label
var _entry_field: LineEdit

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _body != null:
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.75)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(dim)

	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -420.0
	frame.offset_right = 420.0
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

	_scroll = ScrollContainer.new()
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(_scroll)
	_body = RichTextLabel.new()
	_body.bbcode_enabled = false
	_body.fit_content = true
	_body.autowrap_mode = TextServer.AUTOWRAP_OFF
	_body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_body.add_theme_color_override("default_color", N.NEUTRAL_300)
	_body.add_theme_font_size_override("normal_font_size", 13)
	_scroll.add_child(_body)

	_entry_row = HBoxContainer.new()
	_entry_row.add_theme_constant_override("separation", N.SPACE_M)
	_entry_row.visible = false
	col.add_child(_entry_row)
	_entry_label = Label.new()
	_entry_label.add_theme_font_size_override("font_size", 12)
	_entry_label.add_theme_color_override("font_color", N.ACCENT_300)
	_entry_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_entry_row.add_child(_entry_label)
	_entry_field = LineEdit.new()
	_entry_field.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_entry_field.add_theme_color_override("font_color", N.TEXT)
	var fsb := StyleBoxFlat.new()
	fsb.bg_color = Color(0.03, 0.035, 0.055)
	fsb.border_color = N.ACCENT_700
	fsb.set_border_width_all(1)
	fsb.content_margin_left = 10
	fsb.content_margin_right = 10
	fsb.content_margin_top = 7
	fsb.content_margin_bottom = 7
	_entry_field.add_theme_stylebox_override("normal", fsb)
	_entry_field.add_theme_stylebox_override("focus", fsb)
	_entry_field.text_submitted.connect(func(_t: String) -> void: _confirm())
	_entry_row.add_child(_entry_field)

	var confirm_btn := Button.new()
	confirm_btn.text = "Confirm"
	confirm_btn.focus_mode = Control.FOCUS_NONE
	N.apply_button(confirm_btn)
	confirm_btn.pressed.connect(func() -> void: _confirm())
	_entry_row.add_child(confirm_btn)

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_end_screen_state"):
		return
	if _body == null:
		_build()
	var gen: int = int(_host.end_screen_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_end_screen_state()
	_body.text = str(d.get("body", ""))
	_scroll.scroll_vertical = 0

	_has_text_input = bool(d.get("has_text_input", false))
	_entry_row.visible = _has_text_input
	if _has_text_input:
		_entry_label.text = str(d.get("text_label", ""))
		_entry_field.text = ""
		_entry_field.grab_focus()

# --- actions ------------------------------------------------------------------

func _confirm() -> void:
	if _host == null or not _host.has_method("end_screen_confirm"):
		return
	var text := _entry_field.text if _has_text_input else ""
	_host.end_screen_confirm(text)

func _unhandled_input(event: InputEvent) -> void:
	if not visible or not (event is InputEventKey) or not event.pressed or event.echo:
		return
	if not _has_text_input and (event.keycode == KEY_ENTER or event.keycode == KEY_KP_ENTER
			or event.keycode == KEY_SPACE):
		_confirm()
		get_viewport().set_input_as_handled()
