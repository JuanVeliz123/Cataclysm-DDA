extends Control
## Inventory overlay, following the Nocturne "CDDA Inventory" design: a header
## with the two capacity gauges, carried and worn side by side in their own
## columns, and a filter with a key legend along the bottom.
##
## Game logic stays in C++; actions are requested by item uid through
## CDDAHost.request_item_action (src/godot_game_commands.h).

signal closed

const N := preload("res://scripts/nocturne.gd")

## Must match godot_backend::item_action.
const ACTION_WIELD := 0
const ACTION_WEAR := 1
const ACTION_DROP := 2

## Segments per capacity gauge, as the design draws them.
const GAUGE_TICKS := 14

var _host: Node
var _title_sub: Label
var _gauges: Array = []          # 2 dicts {value: Label, ticks: Array[ColorRect]}
var _carried_box: VBoxContainer
var _worn_box: VBoxContainer
var _filter: LineEdit
var _status: Label

var _items: Array = []
## uid of the highlighted row, so selection survives a rebuild.
var _selected_uid: int = 0
## Signature of the last list built, so the columns are not rebuilt every frame.
var _signature: String = ""

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

# --- construction -----------------------------------------------------------

func _build() -> void:
	if _carried_box != null:
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.55)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(dim)

	var frame := Control.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -512.0
	frame.offset_top = -350.0
	frame.offset_right = 512.0
	frame.offset_bottom = 350.0
	frame.clip_contents = true
	add_child(frame)

	_add_backdrop(frame)
	_add_alert_band(frame)
	_add_corner_bracket(frame, true)
	_add_corner_bracket(frame, false)

	var col := VBoxContainer.new()
	col.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	col.add_theme_constant_override("separation", 0)
	frame.add_child(col)

	_build_header(col)
	col.add_child(N.fade_rule())
	_build_columns(col)
	col.add_child(N.fade_rule())
	_build_footer(col)

## Same ground treatment as the sidebar: gradient, scanlines, vignette in one
## shader rather than three stacked nodes.
func _add_backdrop(parent: Control) -> void:
	var sh := Shader.new()
	sh.code = """
shader_type canvas_item;
uniform vec3 ground;
uniform vec3 lift;
void fragment() {
	float v = UV.y;
	vec3 base = mix(mix(ground + lift, ground, smoothstep(0.0, 0.46, v)),
	                ground * 0.72, smoothstep(0.46, 1.0, v));
	float line = step(3.0, mod(FRAGCOORD.y, 4.0)) * 0.09;
	vec2 d = abs(UV - vec2(0.5));
	float vig = smoothstep(0.4, 1.0, length(d) * 1.4) * 0.4;
	COLOR = vec4(base * (1.0 - line) * (1.0 - vig), 0.99);
}
"""
	var mat := ShaderMaterial.new()
	mat.shader = sh
	mat.set_shader_parameter("ground", Vector3(N.BG.r, N.BG.g, N.BG.b))
	mat.set_shader_parameter("lift", Vector3(0.055, 0.055, 0.07))
	var bg := ColorRect.new()
	bg.material = mat
	bg.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	bg.mouse_filter = Control.MOUSE_FILTER_IGNORE
	parent.add_child(bg)

	var border := Panel.new()
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0, 0, 0, 0)
	sb.border_color = N.NEUTRAL_800
	sb.set_border_width_all(1)
	border.add_theme_stylebox_override("panel", sb)
	border.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	border.mouse_filter = Control.MOUSE_FILTER_IGNORE
	parent.add_child(border)

func _add_alert_band(parent: Control) -> void:
	var band := ColorRect.new()
	band.set_anchors_preset(Control.PRESET_TOP_WIDE)
	band.offset_bottom = 3.0
	band.color = N.ACCENT_700
	band.mouse_filter = Control.MOUSE_FILTER_IGNORE
	parent.add_child(band)

func _add_corner_bracket(parent: Control, top_left: bool) -> void:
	var h := ColorRect.new()
	var v := ColorRect.new()
	for r in [h, v]:
		r.color = N.ACCENT_700
		r.mouse_filter = Control.MOUSE_FILTER_IGNORE
		parent.add_child(r)
	if top_left:
		h.set_anchors_preset(Control.PRESET_TOP_LEFT)
		h.offset_left = 14.0
		h.offset_top = 14.0
		h.offset_right = 28.0
		h.offset_bottom = 15.0
		v.set_anchors_preset(Control.PRESET_TOP_LEFT)
		v.offset_left = 14.0
		v.offset_top = 14.0
		v.offset_right = 15.0
		v.offset_bottom = 28.0
	else:
		h.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
		h.offset_left = -28.0
		h.offset_top = -15.0
		h.offset_right = -14.0
		h.offset_bottom = -14.0
		v.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
		v.offset_left = -15.0
		v.offset_top = -28.0
		v.offset_right = -14.0
		v.offset_bottom = -14.0

func _build_header(col: VBoxContainer) -> void:
	var pad := MarginContainer.new()
	pad.add_theme_constant_override("margin_left", 26)
	pad.add_theme_constant_override("margin_right", 26)
	pad.add_theme_constant_override("margin_top", 22)
	pad.add_theme_constant_override("margin_bottom", 14)
	col.add_child(pad)

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 32)
	pad.add_child(row)

	var titles := VBoxContainer.new()
	titles.add_theme_constant_override("separation", 7)
	titles.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(titles)
	var title := Label.new()
	title.text = "INVENTORY"
	title.add_theme_font_size_override("font_size", 19)
	title.add_theme_color_override("font_color", N.TEXT)
	titles.add_child(title)
	_title_sub = N.micro_label("", N.NEUTRAL_600)
	titles.add_child(_title_sub)

	for key in ["Weight", "Volume"]:
		var g := VBoxContainer.new()
		g.custom_minimum_size = Vector2(186, 0)
		g.add_theme_constant_override("separation", 7)
		row.add_child(g)
		var head := HBoxContainer.new()
		g.add_child(head)
		var k := N.micro_label(key, N.NEUTRAL_600)
		k.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		head.add_child(k)
		var value := Label.new()
		value.add_theme_font_size_override("font_size", 12)
		value.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
		head.add_child(value)
		var bar := HBoxContainer.new()
		bar.add_theme_constant_override("separation", 2)
		bar.custom_minimum_size = Vector2(0, 8)
		g.add_child(bar)
		var ticks: Array = []
		for i in GAUGE_TICKS:
			var t := ColorRect.new()
			t.color = N.TICK_EMPTY
			t.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			bar.add_child(t)
			ticks.append(t)
		_gauges.append({"value": value, "ticks": ticks})

## Carried on the left, worn on the right, split by a rule that fades at both
## ends. Each column scrolls on its own -- the design's one scrolling region.
func _build_columns(col: VBoxContainer) -> void:
	var body := HBoxContainer.new()
	body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.add_theme_constant_override("separation", 0)
	col.add_child(body)

	_carried_box = _add_column(body, 26, 22)

	var rule := ColorRect.new()
	rule.color = N.DIVIDER
	rule.custom_minimum_size = Vector2(1, 0)
	body.add_child(rule)

	_worn_box = _add_column(body, 22, 26)

func _add_column(parent: Control, left: int, right: int) -> VBoxContainer:
	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	parent.add_child(scroll)
	var pad := MarginContainer.new()
	pad.add_theme_constant_override("margin_left", left)
	pad.add_theme_constant_override("margin_right", right)
	pad.add_theme_constant_override("margin_top", 16)
	pad.add_theme_constant_override("margin_bottom", 18)
	pad.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(pad)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 4)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	pad.add_child(box)
	return box

func _build_footer(col: VBoxContainer) -> void:
	var pad := MarginContainer.new()
	pad.add_theme_constant_override("margin_left", 26)
	pad.add_theme_constant_override("margin_right", 26)
	pad.add_theme_constant_override("margin_top", 13)
	pad.add_theme_constant_override("margin_bottom", 14)
	col.add_child(pad)

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 16)
	pad.add_child(row)

	var box := PanelContainer.new()
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.03, 0.035, 0.055)
	sb.border_color = N.NEUTRAL_800
	sb.set_border_width_all(1)
	sb.content_margin_left = 11
	sb.content_margin_right = 11
	sb.content_margin_top = 9
	sb.content_margin_bottom = 9
	box.add_theme_stylebox_override("panel", sb)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(box)
	var fr := HBoxContainer.new()
	fr.add_theme_constant_override("separation", 9)
	box.add_child(fr)
	var glass := Label.new()
	glass.text = "⌕"
	glass.add_theme_font_size_override("font_size", 13)
	glass.add_theme_color_override("font_color", N.ACCENT_400)
	fr.add_child(glass)
	_filter = LineEdit.new()
	_filter.placeholder_text = "Filter items"
	_filter.flat = true
	_filter.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_filter.add_theme_color_override("font_color", N.TEXT)
	_filter.text_changed.connect(func(_t: String) -> void: _signature = "")
	fr.add_child(_filter)

	for pair in [["Enter", "Wield"], ["W", "Wear"], ["D", "Drop"], ["Esc", "Close"]]:
		var legend := HBoxContainer.new()
		legend.add_theme_constant_override("separation", 6)
		row.add_child(legend)
		var cap := Label.new()
		cap.text = pair[0]
		cap.add_theme_font_size_override("font_size", 10)
		cap.add_theme_color_override("font_color", N.NEUTRAL_400)
		var csb := StyleBoxFlat.new()
		csb.bg_color = Color(0, 0, 0, 0)
		csb.border_color = N.NEUTRAL_800
		csb.set_border_width_all(1)
		csb.content_margin_left = 5
		csb.content_margin_right = 5
		csb.content_margin_top = 3
		csb.content_margin_bottom = 3
		cap.add_theme_stylebox_override("normal", csb)
		legend.add_child(cap)
		legend.add_child(N.micro_label(pair[1], N.NEUTRAL_600))

	_status = N.micro_label("", N.NEUTRAL_600)
	col.add_child(_status)

# --- update -----------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_inventory_state"):
		return
	if _carried_box == null:
		_build()
	var d: Dictionary = _host.get_inventory_state()
	_items = d.get("items", [])

	_set_gauge(0, str(d.get("carry", "")), int(d.get("carry_pct", 0)))
	_set_gauge(1, str(d.get("volume", "")), int(d.get("volume_pct", 0)))
	_title_sub.text = "%d ITEMS CARRIED" % _items.size()

	var sig := _list_signature()
	if sig != _signature:
		_signature = sig
		_rebuild()

func _set_gauge(index: int, text: String, pct: int) -> void:
	var g: Dictionary = _gauges[index]
	g["value"].text = text
	var fill := N.ACCENT
	var label_color := N.ACCENT_400
	if pct > 90:
		fill = N.BAD
		label_color = N.BAD
	elif pct > 70:
		fill = N.WARN
		label_color = N.WARN
	g["value"].add_theme_color_override("font_color", label_color)
	var on: int = maxi(1, int(round(float(clampi(pct, 0, 100)) / 100.0 * GAUGE_TICKS)))
	for i in g["ticks"].size():
		g["ticks"][i].color = fill if i < on else N.TICK_EMPTY

func _list_signature() -> String:
	var parts := PackedStringArray()
	parts.append(_filter.text if _filter != null else "")
	for it in _items:
		if typeof(it) == TYPE_DICTIONARY:
			parts.append("%s|%s|%s" % [str(it.get("uid", 0)), str(it.get("name", "")),
				str(it.get("meta", ""))])
	parts.append(str(_selected_uid))
	return "\n".join(parts)

func _matches_filter(row: Dictionary) -> bool:
	var needle := _filter.text.strip_edges().to_lower() if _filter != null else ""
	if needle == "":
		return true
	return str(row.get("name", "")).to_lower().contains(needle)

func _rebuild() -> void:
	for box in [_carried_box, _worn_box]:
		for child in box.get_children():
			box.remove_child(child)
			child.queue_free()

	# Carried on the left grouped by item category; worn on the right as one
	# group, which is how the design splits them.
	var carried: Dictionary = {}
	var order: Array = []
	var worn: Array = []
	for it in _items:
		if typeof(it) != TYPE_DICTIONARY or not _matches_filter(it):
			continue
		if str(it.get("where", "")) == "worn":
			worn.append(it)
			continue
		var cat := str(it.get("category", "")).to_upper()
		if cat == "":
			cat = "OTHER"
		if not carried.has(cat):
			carried[cat] = []
			order.append(cat)
		carried[cat].append(it)

	for cat in order:
		_carried_box.add_child(_make_category(cat))
		for row in carried[cat]:
			_carried_box.add_child(_make_row(row))
	if not worn.is_empty():
		_worn_box.add_child(_make_category("ITEMS WORN"))
		for row in worn:
			_worn_box.add_child(_make_row(row))

func _make_category(text: String) -> Control:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(0, 26)
	row.add_theme_constant_override("separation", 10)
	var mark := ColorRect.new()
	mark.color = N.ACCENT
	mark.custom_minimum_size = Vector2(2, 0)
	row.add_child(mark)
	var label := Label.new()
	label.text = text
	label.add_theme_font_size_override("font_size", 9)
	label.add_theme_color_override("font_color", N.NEUTRAL_500)
	label.vertical_alignment = VERTICAL_ALIGNMENT_BOTTOM
	row.add_child(label)
	return row

func _make_row(data: Dictionary) -> Control:
	var uid := int(data.get("uid", 0))
	var selected := uid == _selected_uid

	var button := Button.new()
	button.flat = true
	button.focus_mode = Control.FOCUS_NONE
	button.custom_minimum_size = Vector2(0, 30)
	var sb := StyleBoxFlat.new()
	# Selection is an accent wash plus a bar on the leading edge, never a fill.
	sb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.15) if selected else Color(0, 0, 0, 0)
	sb.border_color = N.ACCENT
	sb.border_width_left = 2 if selected else 0
	button.add_theme_stylebox_override("normal", sb)
	var hover := StyleBoxFlat.new()
	hover.bg_color = Color(N.TEXT.r, N.TEXT.g, N.TEXT.b, 0.05)
	hover.border_color = N.ACCENT
	hover.border_width_left = 2 if selected else 0
	button.add_theme_stylebox_override("hover", hover)
	button.add_theme_stylebox_override("pressed", hover)
	button.pressed.connect(func() -> void: _select(uid))

	var row := HBoxContainer.new()
	row.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	row.offset_left = 8.0
	row.offset_right = -8.0
	row.add_theme_constant_override("separation", 10)
	row.mouse_filter = Control.MOUSE_FILTER_IGNORE
	button.add_child(row)

	var key := Label.new()
	key.text = str(data.get("key", ""))
	key.custom_minimum_size = Vector2(19, 19)
	key.add_theme_font_size_override("font_size", 11)
	key.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	key.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	key.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	var ksb := StyleBoxFlat.new()
	ksb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.13) if selected else Color(0, 0, 0, 0)
	ksb.border_color = N.ACCENT_700 if selected else N.NEUTRAL_800
	ksb.set_border_width_all(1)
	key.add_theme_stylebox_override("normal", ksb)
	key.add_theme_color_override("font_color", N.ACCENT_300 if selected else N.NEUTRAL_400)
	key.visible = key.text != ""
	row.add_child(key)

	var name_label := Label.new()
	name_label.text = str(data.get("name", ""))
	name_label.add_theme_font_size_override("font_size", 11)
	name_label.add_theme_color_override("font_color", N.TEXT if selected else N.NEUTRAL_300)
	name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	name_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	name_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	row.add_child(name_label)

	var meta := Label.new()
	meta.text = str(data.get("meta", ""))
	meta.add_theme_font_size_override("font_size", 10)
	meta.add_theme_color_override("font_color", N.NEUTRAL_500 if selected else N.NEUTRAL_600)
	meta.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	row.add_child(meta)

	return button

func _select(uid: int) -> void:
	if _selected_uid == uid:
		return
	_selected_uid = uid
	_signature = ""
	refresh()

## Ask C++ to act on the selected item. The command is queued for the game thread,
## so a blank return means "accepted", not "done" -- the item could be gone by the
## time it runs, and the game reports that through the message log.
func _request(action: int) -> void:
	if _host == null or not _host.has_method("request_item_action"):
		return
	if _selected_uid == 0:
		_status.text = "SELECT AN ITEM FIRST"
		return
	var err := str(_host.request_item_action(_selected_uid, action))
	_status.text = err.to_upper() if err != "" else "SENT"

func _unhandled_input(event: InputEvent) -> void:
	if not visible:
		return
	if not (event is InputEventKey) or not event.pressed or event.echo:
		return
	# Let the filter field keep its own typing; Escape still closes.
	if _filter != null and _filter.has_focus() and event.keycode != KEY_ESCAPE:
		return
	match event.keycode:
		KEY_ESCAPE, KEY_I:
			closed.emit()
		KEY_ENTER, KEY_KP_ENTER:
			_request(ACTION_WIELD)
		KEY_W:
			_request(ACTION_WEAR)
		KEY_D:
			_request(ACTION_DROP)
		KEY_SLASH:
			if _filter != null:
				_filter.grab_focus()
		_:
			return
	get_viewport().set_input_as_handled()
