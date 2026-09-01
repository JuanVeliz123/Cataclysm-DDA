extends Control
## The medical screen — limbs, effects, wounds — as a Godot Control.
##
## The game thread is blocked in medical_ui's run_in_godot while this is up, and
## that loop still owns every action: the tab, which limb is selected, and
## closing. This panel replaces where the action comes from and nothing else.
##
## Selection round-trips through C++ rather than being echoed locally, because
## the detail pane on the right is built by the game from the selected limb --
## a local echo would show one limb's name against another's wounds for a frame,
## which is the class of lie this migration keeps refusing.
##
## See src/godot_medical_snapshot.h.

const N := preload("res://scripts/nocturne.gd")
const TAGS := preload("res://scripts/color_tags.gd")

var _host: Node
var _generation: int = -1
var _tab: int = 0
var _selected: int = 0

var _title: Label
var _tab_row: HBoxContainer
var _limb_col: VBoxContainer
var _limb_nodes: Array[Button] = []
var _detail_title: Label
var _detail_scroll: ScrollContainer
var _detail: VBoxContainer
var _status: Label

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
	frame.offset_left = -450.0
	frame.offset_right = 450.0
	frame.offset_top = -300.0
	frame.offset_bottom = 300.0
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
	_title.text = "MEDICAL"
	_title.add_theme_font_size_override("font_size", 16)
	_title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(_title)

	_tab_row = HBoxContainer.new()
	_tab_row.add_theme_constant_override("separation", 3)
	col.add_child(_tab_row)

	col.add_child(N.fade_rule())

	# Limbs on the left, the selected limb's detail on the right: the same split
	# the ImGui screen draws, so a player who knows one knows the other.
	var split := HBoxContainer.new()
	split.add_theme_constant_override("separation", N.SPACE_M)
	split.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(split)

	var left := ScrollContainer.new()
	left.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	left.custom_minimum_size = Vector2(240, 0)
	split.add_child(left)
	_limb_col = VBoxContainer.new()
	_limb_col.add_theme_constant_override("separation", 2)
	_limb_col.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	left.add_child(_limb_col)

	var right := VBoxContainer.new()
	right.add_theme_constant_override("separation", N.SPACE_S)
	right.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	split.add_child(right)
	_detail_title = Label.new()
	_detail_title.add_theme_font_size_override("font_size", 13)
	_detail_title.add_theme_color_override("font_color", N.ACCENT)
	right.add_child(_detail_title)
	_detail_scroll = ScrollContainer.new()
	_detail_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_detail_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	right.add_child(_detail_scroll)
	_detail = VBoxContainer.new()
	_detail.add_theme_constant_override("separation", N.SPACE_S)
	_detail.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_detail_scroll.add_child(_detail)

	col.add_child(N.fade_rule())

	_status = Label.new()
	_status.add_theme_font_size_override("font_size", 11)
	_status.add_theme_color_override("font_color", N.NEUTRAL_600)
	col.add_child(_status)

	col.add_child(N.micro_label("↑↓ limb · ←→ tabs · Esc close", N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_medical_state"):
		return
	if _detail == null:
		_build()
	var gen: int = int(_host.medical_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_medical_state()
	var title := str(d.get("title", ""))
	if title != "":
		_title.text = title.to_upper()
	_tab = int(d.get("tab", 0))
	_selected = int(d.get("selected", 0))
	_rebuild_tabs(d.get("tabs", []))
	_rebuild_limbs(d.get("limbs", []))
	_rebuild_detail(d)
	_status.text = "%s   %s" % [str(d.get("speed", "")), str(d.get("weight", ""))]

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

func _rebuild_limbs(limbs: Array) -> void:
	for child in _limb_col.get_children():
		_limb_col.remove_child(child)
		child.queue_free()
	_limb_nodes.clear()
	for i in limbs.size():
		var idx := i
		var btn := Button.new()
		# Colour-tagged: the game writes each limb's condition into its own name
		# (bleeding red, bitten green), and stripping that would throw away the
		# one thing the row is for.
		btn.text = TAGS.strip(str(limbs[i]))
		btn.alignment = HORIZONTAL_ALIGNMENT_LEFT
		btn.focus_mode = Control.FOCUS_NONE
		btn.pressed.connect(func() -> void: _select_limb(idx))
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
		_limb_col.add_child(btn)
		_limb_nodes.append(btn)

func _rebuild_detail(d: Dictionary) -> void:
	for child in _detail.get_children():
		_detail.remove_child(child)
		child.queue_free()
	_detail_title.text = TAGS.strip(str(d.get("detail_title", ""))).to_upper()
	var section := 0
	for pair in [["Effects", d.get("effects", "")], ["Wounds", d.get("wounds", "")],
			["Encumbrance and protection", d.get("scores", "")],
			["Stats", d.get("stats", "")]]:
		var body := str(pair[1])
		if body.strip_edges().is_empty():
			continue
		section += 1
		_detail.add_child(N.section_header(section, str(pair[0])))
		var text := RichTextLabel.new()
		text.bbcode_enabled = true
		text.fit_content = true
		text.selection_enabled = true
		text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		text.add_theme_font_size_override("normal_font_size", 12)
		text.text = TAGS.to_bbcode(body)
		_detail.add_child(text)

# --- actions ------------------------------------------------------------------

func _act(action: String) -> void:
	if _host != null and _host.has_method("medical_action"):
		_host.medical_action(action)

func _select_tab(index: int) -> void:
	if index != _tab and _host != null and _host.has_method("medical_select_tab"):
		_host.medical_select_tab(index)

func _select_limb(index: int) -> void:
	if index != _selected and _host != null and _host.has_method("medical_select_limb"):
		_host.medical_select_limb(index)

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
		KEY_PAGEUP:
			_detail_scroll.scroll_vertical -= int(_detail_scroll.size.y)
		KEY_PAGEDOWN:
			_detail_scroll.scroll_vertical += int(_detail_scroll.size.y)
		_:
			return
	get_viewport().set_input_as_handled()
