extends Control
## Distractions manager (MENU-14's first screen): a flat list of activity
## interrupts, each a plain toggle, as a Godot Control.
##
## Simplest shape in MENU-14: no tabs, no filter, no nested popup, nothing to
## save explicitly -- every row is a `bool *` straight into `uistate`. A
## clicked row round-trips through C++ rather than being toggled locally, same
## rule every other migrated panel follows.
##
## See src/godot_distraction_snapshot.h.

const N := preload("res://scripts/nocturne.gd")

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
	frame.offset_left = -360.0
	frame.offset_right = 360.0
	frame.offset_top = -280.0
	frame.offset_bottom = 280.0
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
	_title.text = "DISTRACTIONS MANAGER"
	_title.add_theme_font_size_override("font_size", 16)
	_title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(_title)

	col.add_child(N.fade_rule())

	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(scroll)
	_list = VBoxContainer.new()
	_list.add_theme_constant_override("separation", 2)
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_list)

	col.add_child(N.fade_rule())
	col.add_child(N.micro_label("Esc close · click a box to toggle", N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_distraction_state"):
		return
	if _list == null:
		_build()
	var gen: int = int(_host.distraction_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_distraction_state()
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
		var index := i
		var box := CheckBox.new()
		box.text = str(rd.get("name", ""))
		box.tooltip_text = str(rd.get("description", ""))
		box.button_pressed = bool(rd.get("enabled", false))
		box.focus_mode = Control.FOCUS_NONE
		box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		box.add_theme_color_override("font_color", N.TEXT)
		box.toggled.connect(func(_pressed: bool) -> void: _toggle(index))
		_list.add_child(box)

# --- actions ------------------------------------------------------------------

func _act(action: String) -> void:
	if _host != null and _host.has_method("distraction_action"):
		_host.distraction_action(action)

func _toggle(index: int) -> void:
	if _host != null and _host.has_method("distraction_toggle"):
		_host.distraction_toggle(index)

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
