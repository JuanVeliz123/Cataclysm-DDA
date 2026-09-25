extends Control
## In-session HUD sidebar, following the Nocturne "CDDA Sidebar v2" design.
##
## Fixed 360px column, and deliberately not scrollable: the design fits in the
## height it has by giving each section a fixed cost and letting the log live in
## a collapsible drawer pinned to the bottom. Anything that could grow without
## bound -- contacts, log lines -- is capped rather than allowed to push the
## layout past the screen.
##
## Every node is built once in _build(); refresh() only writes values. The panel
## refreshes at frame rate, so rebuilding the tree would be pure churn.

const N := preload("res://scripts/nocturne.gd")

const WIDTH := 360.0
const TICKS := 10
## Six main limbs plus Power, as the design lists them.
const VITAL_ROWS := 7
const MAX_CONTACTS := 5
const MAX_LOG := 12
## Open by default, and short enough that the compass and contacts above it keep
## their room -- an open drawer at the design's 292px ate that whole section.
## Overflow scrolls instead of pushing the layout.
const DRAWER_OPEN := 176.0
const DRAWER_SHUT := 39.0

var _host: Node

# Header
var _date: Label
var _place: Label
var _clock: Label
var _weather: Label
var _alert_band: ColorRect

# 01 Vitals
var _pain_status: Label
var _vital_rows: Array = []          # [{name: Label, ticks: Array[ColorRect], value: Label}]
var _metric_values: Array = []       # 4 Labels: Focus Speed Sound Pain
var _morale_fill: ColorRect
var _morale_value: Label
var _stat_values: Array = []         # STR DEX INT PER
var _chips: HBoxContainer

# Weapon strip
var _weapon_glyph: Label
var _weapon_name: Label
var _weapon_style: Label
var _temp_chip: Label

# 02 Contacts
var _compass_cells: Array = []       # 9 dicts {panel: PanelContainer, dir: Label, n: Label}
var _threat: Label
var _contact_rows: Array = []        # [{row: Control, sym: Label, name: Label, meta: Label}]

# 03 Log
## The column of sections above the drawer; its bottom moves with the drawer.
var _body_column: VBoxContainer
var _drawer: PanelContainer
var _log_hint: Label
var _caret: Label
var _log_rows: Array = []            # [{row, stamp, mark, text}]
var _log_scroll: ScrollContainer
var _log_open := true

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

# --- construction -----------------------------------------------------------

func _build() -> void:
	if _date != null:
		return

	var side := Control.new()
	side.set_anchors_preset(Control.PRESET_RIGHT_WIDE)
	side.offset_left = -WIDTH
	side.offset_right = 0.0
	side.mouse_filter = Control.MOUSE_FILTER_STOP
	side.clip_contents = true
	add_child(side)

	_add_backdrop(side)

	# Top alert band: accent normally, hazard stripes when things are dire.
	_alert_band = ColorRect.new()
	_alert_band.set_anchors_preset(Control.PRESET_TOP_WIDE)
	_alert_band.offset_bottom = 3.0
	_alert_band.color = N.ACCENT_700
	_alert_band.mouse_filter = Control.MOUSE_FILTER_IGNORE
	side.add_child(_alert_band)

	_add_corner_bracket(side, true)
	_add_corner_bracket(side, false)

	var col := VBoxContainer.new()
	col.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	col.offset_left = 22.0
	col.offset_right = -22.0
	col.offset_top = 20.0
	col.offset_bottom = -DRAWER_OPEN
	col.add_theme_constant_override("separation", N.SPACE_M)
	col.mouse_filter = Control.MOUSE_FILTER_IGNORE
	side.add_child(col)

	_build_header(col)
	col.add_child(N.fade_rule())
	_build_vitals(col)
	col.add_child(N.fade_rule())
	_build_metrics(col)
	_build_morale_and_stats(col)
	col.add_child(N.fade_rule())
	_build_weapon(col)
	col.add_child(N.fade_rule())
	_build_contacts(col)

	var spacer := Control.new()
	spacer.size_flags_vertical = Control.SIZE_EXPAND_FILL
	spacer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	col.add_child(spacer)

	_build_log_drawer(side)
	_body_column = col
	_apply_drawer()

## Gradient ground, scanlines and vignette in one shader rather than three
## stacked nodes -- it is one draw and the CRT feel is what the design is after.
func _add_backdrop(parent: Control) -> void:
	var sh := Shader.new()
	sh.code = """
shader_type canvas_item;
uniform vec3 ground;
uniform vec3 lift;
void fragment() {
	float v = UV.y;
	// Lighter at the top, sinking to near-black at the bottom.
	vec3 base = mix(mix(ground + lift, ground, smoothstep(0.0, 0.48, v)),
	                ground * 0.72, smoothstep(0.48, 1.0, v));
	// Scanlines: one darker row every 4 device pixels.
	float line = step(3.0, mod(FRAGCOORD.y, 4.0)) * 0.09;
	// Vignette, strongest in the corners.
	vec2 d = abs(UV - vec2(0.5));
	float vig = smoothstep(0.35, 0.95, length(d) * 1.45) * 0.45;
	COLOR = vec4(base * (1.0 - line) * (1.0 - vig), 0.97);
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

	var edge := ColorRect.new()
	edge.set_anchors_preset(Control.PRESET_LEFT_WIDE)
	edge.offset_right = 1.0
	edge.color = N.NEUTRAL_800
	edge.mouse_filter = Control.MOUSE_FILTER_IGNORE
	parent.add_child(edge)

func _add_corner_bracket(parent: Control, top_left: bool) -> void:
	var h := ColorRect.new()
	var v := ColorRect.new()
	for r in [h, v]:
		r.color = N.ACCENT_700
		r.mouse_filter = Control.MOUSE_FILTER_IGNORE
		parent.add_child(r)
	if top_left:
		h.set_anchors_preset(Control.PRESET_TOP_LEFT)
		h.offset_left = 12.0
		h.offset_top = 14.0
		h.offset_right = 26.0
		h.offset_bottom = 15.0
		v.set_anchors_preset(Control.PRESET_TOP_LEFT)
		v.offset_left = 12.0
		v.offset_top = 14.0
		v.offset_right = 13.0
		v.offset_bottom = 28.0
	else:
		h.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
		h.offset_left = -26.0
		h.offset_top = -15.0
		h.offset_right = -12.0
		h.offset_bottom = -14.0
		v.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
		v.offset_left = -13.0
		v.offset_top = -28.0
		v.offset_right = -12.0
		v.offset_bottom = -14.0

func _build_header(col: VBoxContainer) -> void:
	var row := HBoxContainer.new()
	col.add_child(row)

	var left := VBoxContainer.new()
	left.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	left.add_theme_constant_override("separation", 5)
	row.add_child(left)
	_date = Label.new()
	_date.add_theme_font_size_override("font_size", 15)
	_date.add_theme_color_override("font_color", N.TEXT)
	left.add_child(_date)
	_place = N.micro_label("", N.NEUTRAL_600)
	left.add_child(_place)

	var right := VBoxContainer.new()
	right.alignment = BoxContainer.ALIGNMENT_BEGIN
	right.add_theme_constant_override("separation", 5)
	row.add_child(right)
	_clock = Label.new()
	_clock.add_theme_font_size_override("font_size", 13)
	_clock.add_theme_color_override("font_color", N.NEUTRAL_300)
	_clock.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	right.add_child(_clock)
	_weather = N.micro_label("", N.NEUTRAL_500)
	_weather.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	right.add_child(_weather)

func _build_vitals(col: VBoxContainer) -> void:
	var head := N.section_header(1, "Vitals")
	_pain_status = head.get_child(head.get_child_count() - 1)
	col.add_child(head)

	var rows := VBoxContainer.new()
	rows.add_theme_constant_override("separation", 4)
	col.add_child(rows)
	for i in VITAL_ROWS:
		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", N.SPACE_M)
		rows.add_child(row)

		var name_label := N.micro_label("", N.NEUTRAL_500)
		name_label.custom_minimum_size = Vector2(50, 0)
		row.add_child(name_label)

		var bar := HBoxContainer.new()
		bar.add_theme_constant_override("separation", 2)
		bar.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		bar.custom_minimum_size = Vector2(0, 9)
		row.add_child(bar)
		var ticks: Array = []
		for t in TICKS:
			var tick := ColorRect.new()
			tick.color = N.TICK_EMPTY
			tick.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			bar.add_child(tick)
			ticks.append(tick)

		var value := Label.new()
		value.add_theme_font_size_override("font_size", 10)
		value.custom_minimum_size = Vector2(30, 0)
		value.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
		row.add_child(value)

		_vital_rows.append({"name": name_label, "ticks": ticks, "value": value})

## Four tonal cells sharing 1px gridlines, drawn by letting the container's own
## colour show through the gaps.
func _build_metrics(col: VBoxContainer) -> void:
	var grid := HBoxContainer.new()
	grid.add_theme_constant_override("separation", 1)
	col.add_child(grid)
	for key in ["Focus", "Speed", "Sound", "Pain"]:
		var cell := PanelContainer.new()
		var sb := StyleBoxFlat.new()
		sb.bg_color = Color(N.SURFACE.r, N.SURFACE.g, N.SURFACE.b, 0.72)
		sb.content_margin_left = 10
		sb.content_margin_right = 10
		sb.content_margin_top = 9
		sb.content_margin_bottom = 9
		cell.add_theme_stylebox_override("panel", sb)
		cell.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		grid.add_child(cell)

		var box := VBoxContainer.new()
		box.add_theme_constant_override("separation", 6)
		cell.add_child(box)
		box.add_child(N.micro_label(key, N.NEUTRAL_600))
		var value := Label.new()
		value.add_theme_font_size_override("font_size", 16)
		value.add_theme_color_override("font_color", N.TEXT)
		box.add_child(value)
		_metric_values.append(value)

func _build_morale_and_stats(col: VBoxContainer) -> void:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 11)
	col.add_child(box)

	# Morale reads against a centre line: left of it is negative, right positive.
	var mrow := HBoxContainer.new()
	mrow.add_theme_constant_override("separation", N.SPACE_M)
	box.add_child(mrow)
	var mkey := N.micro_label("Morale", N.NEUTRAL_600)
	mkey.custom_minimum_size = Vector2(50, 0)
	mrow.add_child(mkey)
	var track := Control.new()
	track.custom_minimum_size = Vector2(0, 7)
	track.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	track.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	mrow.add_child(track)
	var trough := ColorRect.new()
	trough.color = Color(0.03, 0.035, 0.055)
	trough.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	track.add_child(trough)
	_morale_fill = ColorRect.new()
	_morale_fill.color = N.NEUTRAL_600
	track.add_child(_morale_fill)
	var centre := ColorRect.new()
	centre.color = N.NEUTRAL_700
	centre.set_anchors_preset(Control.PRESET_CENTER_LEFT)
	centre.anchor_left = 0.5
	centre.anchor_right = 0.5
	centre.anchor_top = 0.0
	centre.anchor_bottom = 1.0
	centre.offset_left = 0.0
	centre.offset_right = 1.0
	centre.offset_top = -4.0
	centre.offset_bottom = 4.0
	track.add_child(centre)
	_morale_value = Label.new()
	_morale_value.add_theme_font_size_override("font_size", 10)
	_morale_value.custom_minimum_size = Vector2(28, 0)
	_morale_value.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	mrow.add_child(_morale_value)

	var srow := HBoxContainer.new()
	srow.alignment = BoxContainer.ALIGNMENT_BEGIN
	box.add_child(srow)
	for key in ["Str", "Dex", "Int", "Per"]:
		var pair := HBoxContainer.new()
		pair.add_theme_constant_override("separation", 6)
		pair.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		srow.add_child(pair)
		pair.add_child(N.micro_label(key, N.NEUTRAL_600))
		var value := Label.new()
		value.add_theme_font_size_override("font_size", 14)
		value.add_theme_color_override("font_color", N.TEXT)
		pair.add_child(value)
		_stat_values.append(value)

	_chips = HBoxContainer.new()
	_chips.add_theme_constant_override("separation", 5)
	box.add_child(_chips)

func _build_weapon(col: VBoxContainer) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", N.SPACE_M)
	col.add_child(row)

	_weapon_glyph = Label.new()
	_weapon_glyph.custom_minimum_size = Vector2(24, 24)
	_weapon_glyph.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_weapon_glyph.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_weapon_glyph.add_theme_font_size_override("font_size", 13)
	_weapon_glyph.add_theme_color_override("font_color", N.ACCENT_400)
	var gsb := StyleBoxFlat.new()
	gsb.bg_color = Color(0, 0, 0, 0)
	gsb.border_color = N.ACCENT_700
	gsb.set_border_width_all(1)
	_weapon_glyph.add_theme_stylebox_override("normal", gsb)
	row.add_child(_weapon_glyph)

	var names := VBoxContainer.new()
	names.add_theme_constant_override("separation", 4)
	names.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(names)
	_weapon_name = Label.new()
	_weapon_name.add_theme_font_size_override("font_size", 11)
	_weapon_name.add_theme_color_override("font_color", N.TEXT)
	_weapon_name.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	names.add_child(_weapon_name)
	_weapon_style = N.micro_label("", N.NEUTRAL_600)
	names.add_child(_weapon_style)

	_temp_chip = N.chip("", "dim")
	_temp_chip.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	row.add_child(_temp_chip)

func _build_contacts(col: VBoxContainer) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 16)
	col.add_child(row)

	# 3x3 compass. Centre is the player and never carries a count.
	var grid := GridContainer.new()
	grid.columns = 3
	grid.add_theme_constant_override("h_separation", 2)
	grid.add_theme_constant_override("v_separation", 2)
	row.add_child(grid)
	for i in 9:
		var cell := PanelContainer.new()
		cell.custom_minimum_size = Vector2(29, 25)
		grid.add_child(cell)
		var box := VBoxContainer.new()
		box.alignment = BoxContainer.ALIGNMENT_CENTER
		box.add_theme_constant_override("separation", 2)
		cell.add_child(box)
		var dir := Label.new()
		dir.add_theme_font_size_override("font_size", 7)
		dir.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		box.add_child(dir)
		var n := Label.new()
		n.add_theme_font_size_override("font_size", 11)
		n.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		box.add_child(n)
		_compass_cells.append({"panel": cell, "dir": dir, "n": n})

	var side := VBoxContainer.new()
	side.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	side.add_theme_constant_override("separation", 7)
	row.add_child(side)
	var head := N.section_header(2, "Contacts")
	_threat = head.get_child(head.get_child_count() - 1)
	side.add_child(head)

	for i in MAX_CONTACTS:
		var crow := HBoxContainer.new()
		crow.add_theme_constant_override("separation", 8)
		crow.visible = false
		side.add_child(crow)
		var sym := Label.new()
		sym.custom_minimum_size = Vector2(17, 17)
		sym.add_theme_font_size_override("font_size", 10)
		sym.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		sym.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
		crow.add_child(sym)
		var cname := Label.new()
		cname.add_theme_font_size_override("font_size", 10)
		cname.add_theme_color_override("font_color", N.NEUTRAL_300)
		cname.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		cname.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
		crow.add_child(cname)
		var meta := Label.new()
		meta.add_theme_font_size_override("font_size", 9)
		meta.add_theme_color_override("font_color", N.NEUTRAL_500)
		crow.add_child(meta)
		_contact_rows.append({"row": crow, "sym": sym, "name": cname, "meta": meta})

## The log is a drawer rather than a section: collapsed it shows only the newest
## line, so the sections above always fit without scrolling.
func _build_log_drawer(parent: Control) -> void:
	_drawer = PanelContainer.new()
	_drawer.set_anchors_preset(Control.PRESET_BOTTOM_WIDE)
	_drawer.offset_top = -DRAWER_OPEN
	_drawer.offset_bottom = 0.0
	_drawer.clip_contents = true
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.04, 0.045, 0.07, 0.95)
	sb.border_color = N.NEUTRAL_800
	sb.border_width_top = 1
	_drawer.add_theme_stylebox_override("panel", sb)
	parent.add_child(_drawer)

	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", 8)
	_drawer.add_child(col)

	var toggle := Button.new()
	toggle.flat = true
	toggle.focus_mode = Control.FOCUS_NONE
	toggle.pressed.connect(_toggle_log)
	col.add_child(toggle)
	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", 9)
	head.mouse_filter = Control.MOUSE_FILTER_IGNORE
	head.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	head.offset_left = 22.0
	head.offset_right = -22.0
	toggle.add_child(head)
	toggle.custom_minimum_size = Vector2(0, DRAWER_SHUT)
	_caret = Label.new()
	_caret.text = "▲"
	_caret.add_theme_font_size_override("font_size", 9)
	_caret.add_theme_color_override("font_color", N.ACCENT_400)
	_caret.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	head.add_child(_caret)
	var title := N.micro_label("03 Log", N.NEUTRAL_500)
	title.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	head.add_child(title)
	_log_hint = Label.new()
	_log_hint.add_theme_font_size_override("font_size", 10)
	_log_hint.add_theme_color_override("font_color", N.NEUTRAL_500)
	_log_hint.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_log_hint.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_log_hint.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	head.add_child(_log_hint)

	_log_scroll = ScrollContainer.new()
	_log_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_log_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(_log_scroll)
	var body_pad := MarginContainer.new()
	body_pad.add_theme_constant_override("margin_left", 22)
	body_pad.add_theme_constant_override("margin_right", 22)
	body_pad.add_theme_constant_override("margin_bottom", 12)
	body_pad.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_log_scroll.add_child(body_pad)
	var body := VBoxContainer.new()
	body.add_theme_constant_override("separation", 8)
	body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	body_pad.add_child(body)
	for i in MAX_LOG:
		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", 9)
		row.visible = false
		body.add_child(row)
		var stamp := Label.new()
		stamp.custom_minimum_size = Vector2(34, 0)
		stamp.add_theme_font_size_override("font_size", 9)
		stamp.add_theme_color_override("font_color", N.NEUTRAL_700)
		row.add_child(stamp)
		var mark := ColorRect.new()
		mark.custom_minimum_size = Vector2(2, 0)
		mark.size_flags_vertical = Control.SIZE_FILL
		row.add_child(mark)
		var text := Label.new()
		text.add_theme_font_size_override("font_size", 11)
		text.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		row.add_child(text)
		_log_rows.append({"row": row, "stamp": stamp, "mark": mark, "text": text})

func _toggle_log() -> void:
	_log_open = not _log_open
	_apply_drawer()

## Keep the drawer height and the space reserved for it above in step, so the
## sections never end up underneath it.
func _apply_drawer() -> void:
	var h := DRAWER_OPEN if _log_open else DRAWER_SHUT
	_drawer.offset_top = -h
	if _body_column != null:
		_body_column.offset_bottom = -h
	_caret.text = "▼" if _log_open else "▲"
	_log_hint.visible = not _log_open
	_log_scroll.visible = _log_open

# --- update -----------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_hud_state"):
		return
	if _date == null:
		_build()
	var d: Dictionary = _host.get_hud_state()

	_date.text = str(d.get("date", "")).to_upper()
	_place.text = str(d.get("location", "")).to_upper()
	_clock.text = str(d.get("time", ""))
	_weather.text = str(d.get("weather", "")).to_upper()

	_update_vitals(d)
	_update_metrics(d)
	_update_morale_and_stats(d)
	_update_weapon(d)
	_update_contacts(d)
	_update_log(d)

func _update_vitals(d: Dictionary) -> void:
	var pain: int = int(d.get("pain_level", 0))
	_pain_status.text = "PAIN %d" % pain
	_pain_status.add_theme_color_override("font_color", _pain_color(pain))

	var limbs: Array = d.get("limbs", [])
	for i in VITAL_ROWS:
		var row: Dictionary = _vital_rows[i]
		if i < limbs.size():
			var limb: Dictionary = limbs[i]
			var hp_max: int = maxi(1, int(limb.get("hp_max", 1)))
			var pct: int = int(round(float(int(limb.get("hp", 0))) / hp_max * 100.0))
			var c := _hp_color(pct)
			row["name"].text = str(limb.get("name", "")).to_upper()
			row["value"].text = str(pct)
			row["value"].add_theme_color_override("font_color", c)
			_set_ticks(row["ticks"], pct, c)
		elif i == VITAL_ROWS - 1:
			# Last row is bionic power, which most characters do not have.
			var power: int = int(d.get("power_pct", -1))
			row["name"].text = "POWER"
			if power < 0:
				row["value"].text = "—"
				row["value"].add_theme_color_override("font_color", N.NEUTRAL_600)
				_set_ticks(row["ticks"], 0, N.NEUTRAL_600)
			else:
				row["value"].text = str(power)
				row["value"].add_theme_color_override("font_color", N.ACCENT_400)
				_set_ticks(row["ticks"], power, N.ACCENT)
		else:
			row["name"].text = ""
			row["value"].text = ""
			_set_ticks(row["ticks"], 0, N.NEUTRAL_600)

## Round up so that "some health left" never renders as an empty bar.
func _set_ticks(ticks: Array, pct: int, fill: Color) -> void:
	var on: int = maxi(1 if pct > 0 else 0, int(round(float(pct) / 100.0 * TICKS)))
	for i in ticks.size():
		ticks[i].color = fill if i < on else N.TICK_EMPTY

func _hp_color(pct: int) -> Color:
	if pct >= 70:
		return N.GOOD
	return N.WARN if pct >= 35 else N.BAD

func _pain_color(pain: int) -> Color:
	if pain == 0:
		return N.NEUTRAL_600
	return N.BAD if pain > 50 else N.WARN

func _update_metrics(d: Dictionary) -> void:
	var focus: int = int(d.get("focus", 0))
	var speed: int = int(d.get("speed", 0))
	var sound: int = int(d.get("sound", 0))
	var pain: int = int(d.get("pain_level", 0))
	_metric_values[0].text = str(focus)
	_metric_values[0].add_theme_color_override("font_color",
		N.TEXT if focus >= 90 else N.WARN)
	_metric_values[1].text = str(speed)
	_metric_values[1].add_theme_color_override("font_color",
		N.TEXT if speed >= 95 else (N.WARN if speed >= 60 else N.BAD))
	_metric_values[2].text = str(sound)
	_metric_values[2].add_theme_color_override("font_color",
		N.WARN if sound > 6 else N.NEUTRAL_400)
	_metric_values[3].text = str(pain)
	_metric_values[3].add_theme_color_override("font_color", _pain_color(pain))

	# The band goes hazard-striped only when things are genuinely dire, so that
	# it still means something when it does.
	_alert_band.color = N.BAD if pain > 50 else N.ACCENT_700

func _update_morale_and_stats(d: Dictionary) -> void:
	var m: int = int(d.get("morale_level", 0))
	var mag: float = minf(absf(m), 20.0) / 20.0 * 0.5
	_morale_fill.anchor_top = 0.0
	_morale_fill.anchor_bottom = 1.0
	_morale_fill.anchor_left = 0.5 if m >= 0 else 0.5 - mag
	_morale_fill.anchor_right = 0.5 + mag if m >= 0 else 0.5
	_morale_fill.offset_left = 0.0
	_morale_fill.offset_right = 0.0
	_morale_fill.offset_top = 0.0
	_morale_fill.offset_bottom = 0.0
	var mc := N.NEUTRAL_600
	if m > 0:
		mc = N.GOOD
	elif m < 0:
		mc = N.BAD if m < -6 else N.WARN
	_morale_fill.color = mc
	_morale_value.text = ("+%d" % m) if m > 0 else str(m)
	_morale_value.add_theme_color_override("font_color", mc)

	var stats: Array = [
		int(d.get("str", 0)), int(d.get("dex", 0)),
		int(d.get("int", 0)), int(d.get("per", 0))
	]
	for i in _stat_values.size():
		_stat_values[i].text = str(stats[i])

	var effects: Array = d.get("effects", [])
	_sync_chips(effects)

## Chips vary in count, so they are the one list that is rebuilt -- but only
## when the set actually changed.
var _chip_signature := ""

func _sync_chips(effects: Array) -> void:
	var parts := PackedStringArray()
	for e in effects:
		if typeof(e) == TYPE_DICTIONARY:
			parts.append("%s/%s" % [str(e.get("label", "")), str(e.get("tone", ""))])
	var sig := "|".join(parts)
	if sig == _chip_signature:
		return
	_chip_signature = sig
	for child in _chips.get_children():
		_chips.remove_child(child)
		child.queue_free()
	for e in effects:
		if typeof(e) != TYPE_DICTIONARY:
			continue
		_chips.add_child(N.chip(str(e.get("label", "")), str(e.get("tone", "dim"))))

func _update_weapon(d: Dictionary) -> void:
	_weapon_glyph.text = str(d.get("weapon_glyph", ""))
	_weapon_name.text = str(d.get("weapon", "")).to_upper()
	_weapon_style.text = str(d.get("style", "")).to_upper()
	var temp := str(d.get("temperature", ""))
	var hot := temp.to_lower()
	var tone := "warn" if (hot.contains("hot") or hot.contains("cold")
		or hot.contains("chill") or hot.contains("freez")) else "dim"
	_temp_chip.text = temp.to_upper()
	N.set_chip_tone(_temp_chip, tone)

func _update_contacts(d: Dictionary) -> void:
	# compass arrives in grid order (NW N NE W E SW S SE); index 4 is the player.
	var counts: Array = d.get("compass", [])
	const LABELS := ["NW", "N", "NE", "W", "", "E", "SW", "S", "SE"]
	for i in 9:
		var cell: Dictionary = _compass_cells[i]
		var sb := StyleBoxFlat.new()
		sb.set_border_width_all(1)
		if i == 4:
			cell["dir"].text = ""
			cell["n"].text = "@"
			cell["n"].add_theme_color_override("font_color", N.ACCENT_300)
			sb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.14)
			sb.border_color = N.ACCENT_700
		else:
			# Grid slots skip the centre, so shift past it when indexing counts.
			var src: int = i if i < 4 else i - 1
			var n: int = int(counts[src]) if src < counts.size() else 0
			cell["dir"].text = LABELS[i]
			if n > 0:
				cell["n"].text = str(n)
				cell["n"].add_theme_color_override("font_color", N.BAD)
				cell["dir"].add_theme_color_override("font_color", N.NEUTRAL_400)
				sb.bg_color = N.tone_bg("bad")
				sb.border_color = N.tone_border("bad")
			else:
				cell["n"].text = "·"
				cell["n"].add_theme_color_override("font_color", N.NEUTRAL_700)
				cell["dir"].add_theme_color_override("font_color", N.NEUTRAL_700)
				sb.bg_color = Color(0.03, 0.035, 0.055)
				sb.border_color = Color(N.NEUTRAL_800.r, N.NEUTRAL_800.g, N.NEUTRAL_800.b, 0.7)
		cell["panel"].add_theme_stylebox_override("panel", sb)

	var summary := str(d.get("threat_summary", ""))
	_threat.text = summary.to_upper()
	_threat.add_theme_color_override("font_color",
		N.GOOD if summary.begins_with("clear") else N.BAD)

	var contacts: Array = d.get("contacts", [])
	for i in MAX_CONTACTS:
		var row: Dictionary = _contact_rows[i]
		if i >= contacts.size() or typeof(contacts[i]) != TYPE_DICTIONARY:
			row["row"].visible = false
			continue
		var c: Dictionary = contacts[i]
		var tone := str(c.get("tone", "dim"))
		row["row"].visible = true
		row["sym"].text = str(c.get("symbol", ""))
		row["sym"].add_theme_color_override("font_color", N.tone_color(tone))
		var sb := StyleBoxFlat.new()
		sb.bg_color = N.tone_bg(tone)
		sb.border_color = N.tone_border(tone)
		sb.set_border_width_all(1)
		row["sym"].add_theme_stylebox_override("normal", sb)
		row["name"].text = str(c.get("name", "")).to_upper()
		row["meta"].text = str(c.get("meta", ""))

func _update_log(d: Dictionary) -> void:
	var log_lines: Array = d.get("messages", [])
	# Newest first: the drawer is read from the top and only the top line shows
	# when it is shut.
	var newest: Array = []
	for i in range(log_lines.size() - 1, -1, -1):
		if typeof(log_lines[i]) == TYPE_DICTIONARY:
			newest.append(log_lines[i])
		if newest.size() >= MAX_LOG:
			break

	_log_hint.text = str(newest[0].get("text", "")) if not newest.is_empty() else ""
	if not newest.is_empty():
		_log_hint.add_theme_color_override("font_color",
			N.tone_color(str(newest[0].get("tone", "dim"))))

	for i in MAX_LOG:
		var row: Dictionary = _log_rows[i]
		if i >= newest.size():
			row["row"].visible = false
			continue
		var line: Dictionary = newest[i]
		var tone := str(line.get("tone", "dim"))
		row["row"].visible = true
		row["stamp"].text = str(line.get("stamp", ""))
		row["mark"].color = N.tone_border(tone)
		row["text"].text = str(line.get("text", ""))
		row["text"].add_theme_color_override("font_color",
			N.NEUTRAL_500 if tone == "dim" else N.tone_color(tone))
