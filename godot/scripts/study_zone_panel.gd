extends Control
## Study zone skill preferences: a skills x followers checkbox grid, as a Godot
## Control.
##
## The game thread is blocked in study_zone_ui's run_in_godot while this is up.
## A checkbox click round-trips through C++ rather than being toggled locally --
## the same rule every other MENU-13 panel follows, because the row a click
## lands on is decided by the filter C++ is applying, not by what this panel
## drew last frame.
##
## See src/godot_study_zone_snapshot.h.

const N := preload("res://scripts/nocturne.gd")

var _host: Node
var _generation: int = -1
var _npc_count: int = 0

var _title: Label
var _filter: LineEdit
var _header_row: HBoxContainer
var _grid: VBoxContainer
var _status: Label

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _grid != null:
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.55)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(dim)

	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -500.0
	frame.offset_right = 500.0
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
	_title.text = "STUDY ZONE SKILL PREFERENCES"
	_title.add_theme_font_size_override("font_size", 16)
	_title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(_title)

	var filter_row := HBoxContainer.new()
	filter_row.add_theme_constant_override("separation", 8)
	col.add_child(filter_row)
	filter_row.add_child(N.micro_label("Filter skills:", N.NEUTRAL_600))
	_filter = LineEdit.new()
	_filter.flat = true
	_filter.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_filter.add_theme_color_override("font_color", N.TEXT)
	_filter.text_changed.connect(func(t: String) -> void:
		if _host != null and _host.has_method("study_zone_set_filter"):
			_host.study_zone_set_filter(t))
	filter_row.add_child(_filter)

	col.add_child(N.fade_rule())

	# Header: a blank corner cell plus one column per follower.
	_header_row = HBoxContainer.new()
	_header_row.add_theme_constant_override("separation", 4)
	col.add_child(_header_row)

	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(scroll)
	_grid = VBoxContainer.new()
	_grid.add_theme_constant_override("separation", 2)
	_grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_grid)

	col.add_child(N.fade_rule())

	var button_row := HBoxContainer.new()
	button_row.add_theme_constant_override("separation", N.SPACE_S)
	col.add_child(button_row)
	for spec in [["Check All", "CHECK_ALL"], ["Clear All", "CLEAR_ALL"], ["Done", "DONE"]]:
		var btn := Button.new()
		btn.text = str(spec[0])
		btn.focus_mode = Control.FOCUS_NONE
		N.apply_button(btn)
		var action := str(spec[1])
		btn.pressed.connect(func() -> void: _act(action))
		button_row.add_child(btn)

	_status = Label.new()
	_status.add_theme_font_size_override("font_size", 11)
	_status.add_theme_color_override("font_color", N.NEUTRAL_600)
	col.add_child(_status)

	col.add_child(N.micro_label("Esc close · click a box to toggle", N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

const _SKILL_COL_WIDTH := 220
const _NPC_COL_WIDTH := 96

func refresh() -> void:
	if _host == null or not _host.has_method("get_study_zone_state"):
		return
	if _grid == null:
		_build()
	var gen: int = int(_host.study_zone_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_study_zone_state()
	var title := str(d.get("title", ""))
	if title != "":
		_title.text = title.to_upper()

	var npc_names: Array = d.get("npc_names", [])
	_npc_count = npc_names.size()
	_rebuild_header(npc_names)
	_rebuild_grid(d.get("rows", []), npc_names.size())

	var filter_text := str(d.get("filter", ""))
	if not _filter.has_focus() and _filter.text != filter_text:
		_filter.text = filter_text

	_status.text = "%d skill%s shown" % [d.get("rows", []).size(),
			"" if d.get("rows", []).size() == 1 else "s"]

func _rebuild_header(npc_names: Array) -> void:
	for child in _header_row.get_children():
		_header_row.remove_child(child)
		child.queue_free()
	var corner := Control.new()
	corner.custom_minimum_size = Vector2(_SKILL_COL_WIDTH, 0)
	_header_row.add_child(corner)
	for npc_name in npc_names:
		var lbl := Label.new()
		lbl.text = str(npc_name)
		lbl.custom_minimum_size = Vector2(_NPC_COL_WIDTH, 0)
		lbl.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		lbl.add_theme_font_size_override("font_size", 11)
		lbl.add_theme_color_override("font_color", N.NEUTRAL_500)
		_header_row.add_child(lbl)

func _rebuild_grid(rows: Array, npc_count: int) -> void:
	for child in _grid.get_children():
		_grid.remove_child(child)
		child.queue_free()
	if npc_count == 0:
		var empty := Label.new()
		empty.text = "No followers to study anything."
		empty.add_theme_color_override("font_color", N.NEUTRAL_600)
		_grid.add_child(empty)
		return
	for row in rows:
		var rd: Dictionary = row
		var skill_index := int(rd.get("skill_index", -1))
		var line := HBoxContainer.new()
		line.add_theme_constant_override("separation", 4)
		var name_lbl := Label.new()
		name_lbl.text = str(rd.get("name", ""))
		name_lbl.custom_minimum_size = Vector2(_SKILL_COL_WIDTH, 0)
		name_lbl.add_theme_font_size_override("font_size", 12)
		name_lbl.add_theme_color_override("font_color", N.TEXT)
		line.add_child(name_lbl)
		var checked: Array = rd.get("checked", [])
		for i in checked.size():
			var npc_index := i
			var box := CheckBox.new()
			box.button_pressed = bool(checked[i])
			box.focus_mode = Control.FOCUS_NONE
			box.custom_minimum_size = Vector2(_NPC_COL_WIDTH, 0)
			box.alignment = HORIZONTAL_ALIGNMENT_CENTER
			box.toggled.connect(func(_pressed: bool) -> void: _toggle(skill_index, npc_index))
			line.add_child(box)
		_grid.add_child(line)

# --- actions ------------------------------------------------------------------

func _act(action: String) -> void:
	if _host != null and _host.has_method("study_zone_action"):
		_host.study_zone_action(action)

func _toggle(skill_index: int, npc_index: int) -> void:
	if _host != null and _host.has_method("study_zone_toggle"):
		_host.study_zone_toggle(skill_index, npc_index)

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
	# The filter box keeps its own typing; Escape still reaches the screen so
	# the first press can clear the filter and the second can close it.
	if event.keycode == KEY_ESCAPE:
		_act("QUIT")
		get_viewport().set_input_as_handled()
		return
	if _filter != null and _filter.has_focus():
		return
	if event.keycode == KEY_F:
		_filter.grab_focus()
		get_viewport().set_input_as_handled()
