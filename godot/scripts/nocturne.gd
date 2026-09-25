extends RefCounted
class_name Nocturne
## Nocturne design tokens.
##
## Transcribed from the design system's styles.css. The system's own guidance is
## to take every colour, radius and spacing from the variables rather than
## hard-coding them, so panels pull from here instead of repeating hex codes --
## which is also what makes a later retheme a one-file change.

const BG := Color("161826")
const SURFACE := Color("232532")
const TEXT := Color("e9e9ed")
const ACCENT := Color("9184d9")

# Neutral ramp. On this dark ground: 700-900 for tinted fills and subtle borders,
# 500 as the base, 100-300 for text sitting on those tints.
const NEUTRAL_200 := Color("e4e7f5")
const NEUTRAL_300 := Color("cfd3e5")
const NEUTRAL_400 := Color("b2b6ca")
const NEUTRAL_500 := Color("9397ab")
const NEUTRAL_600 := Color("75798c")
const NEUTRAL_700 := Color("595d6c")
const NEUTRAL_800 := Color("3f424d")
const NEUTRAL_900 := Color("292b31")

const ACCENT_300 := Color("d2cefd")
const ACCENT_400 := Color("b5abfc")
const ACCENT_700 := Color("5d5294")
const ACCENT_900 := Color("2b2741")

const DIVIDER := Color(0.914, 0.914, 0.929, 0.16)

## Status hues. Nocturne is monochrome and carries none of its own; the design
## introduces exactly these three, at the ramps' own lightness and low chroma,
## and uses them nowhere else. Keep it that way -- their meaning is their scarcity.
const GOOD := Color("8fd3ac")
const WARN := Color("dcc57e")
const BAD := Color("e39b93")

## Empty segment of a tick bar: darker than the ground, so "missing" reads as a
## hole rather than as a dim fill.
const TICK_EMPTY := Color(0.055, 0.06, 0.09)

static func tone_color(tone: String) -> Color:
	match tone:
		"bad": return BAD
		"warn": return WARN
		"good": return GOOD
		_: return NEUTRAL_500

## Chips and compass cells are a tinted wash inside a stronger border, never a
## solid fill.
static func tone_bg(tone: String) -> Color:
	var c := tone_color(tone)
	return Color(c.r, c.g, c.b, 0.13)

static func tone_border(tone: String) -> Color:
	var c := tone_color(tone)
	return Color(c.r, c.g, c.b, 0.42)

## The design's rules fade out at both ends instead of stopping square.
static func fade_rule() -> TextureRect:
	var grad := Gradient.new()
	grad.offsets = PackedFloat32Array([0.0, 0.1, 0.82, 1.0])
	var edge := Color(TEXT.r, TEXT.g, TEXT.b, 0.0)
	var mid := Color(TEXT.r, TEXT.g, TEXT.b, 0.13)
	grad.colors = PackedColorArray([edge, mid, mid, edge])
	var tex := GradientTexture1D.new()
	tex.gradient = grad
	tex.width = 256
	var rect := TextureRect.new()
	rect.texture = tex
	rect.stretch_mode = TextureRect.STRETCH_SCALE
	# Without this the rule's minimum width is the gradient texture's own 256px,
	# which custom_minimum_size does not override -- and since a rule sits inside
	# every section header, that pushed the whole 360px sidebar out to 493 and
	# clipped the right-hand side of every row against the panel edge.
	rect.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	rect.custom_minimum_size = Vector2(0, 1)
	rect.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return rect

## A bordered wash pill: effect chips, the temperature readout.
static func chip(text: String, tone: String) -> Label:
	var l := Label.new()
	l.text = text.to_upper()
	l.add_theme_font_size_override("font_size", 9)
	set_chip_tone(l, tone)
	return l

static func set_chip_tone(l: Label, tone: String) -> void:
	var sb := StyleBoxFlat.new()
	sb.bg_color = tone_bg(tone)
	sb.border_color = tone_border(tone)
	sb.set_border_width_all(1)
	sb.content_margin_left = 8
	sb.content_margin_right = 8
	sb.content_margin_top = 5
	sb.content_margin_bottom = 5
	l.add_theme_stylebox_override("normal", sb)
	l.add_theme_color_override("font_color", tone_color(tone))

## Small uppercase key, as used above every metric and section.
static func micro_label(text: String, color: Color = NEUTRAL_600) -> Label:
	var l := Label.new()
	l.text = text.to_upper()
	l.add_theme_font_size_override("font_size", 9)
	l.add_theme_color_override("font_color", color)
	return l

const RADIUS := 8
## Density 0.70x is baked into the design's spacing scale.
const SPACE_S := 6
const SPACE_M := 10
const SPACE_L := 16

## Panel ground. `accented` draws the accent as a line on the left edge -- the
## system uses the accent as a line and a glow, never as a flood.
static func panel_style(accented: bool = false) -> StyleBoxFlat:
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(SURFACE.r, SURFACE.g, SURFACE.b, 0.96)
	sb.border_color = DIVIDER
	sb.set_border_width_all(1)
	sb.set_corner_radius_all(RADIUS)
	sb.content_margin_left = SPACE_L
	sb.content_margin_right = SPACE_L
	sb.content_margin_top = SPACE_M
	sb.content_margin_bottom = SPACE_M
	if accented:
		sb.border_width_left = 2
		sb.border_color = ACCENT
	return sb

## Buttons are outlined, not solid-filled.
static func button_style(pressed: bool = false) -> StyleBoxFlat:
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(ACCENT_900.r, ACCENT_900.g, ACCENT_900.b, 0.55 if pressed else 0.0)
	sb.border_color = ACCENT if not pressed else ACCENT_300
	sb.set_border_width_all(1)
	sb.set_corner_radius_all(RADIUS)
	sb.content_margin_left = SPACE_M
	sb.content_margin_right = SPACE_M
	sb.content_margin_top = SPACE_S
	sb.content_margin_bottom = SPACE_S
	return sb

static func apply_button(btn: Button) -> void:
	btn.add_theme_stylebox_override("normal", button_style())
	btn.add_theme_stylebox_override("hover", button_style(true))
	btn.add_theme_stylebox_override("pressed", button_style(true))
	btn.add_theme_stylebox_override("focus", button_style(true))
	btn.add_theme_color_override("font_color", ACCENT_300)
	btn.add_theme_color_override("font_hover_color", TEXT)

## Numbered section heading: an accent dash, "01 VITALS", a rule that runs out
## to the edge, and a right-aligned status word. The returned row's last child is
## that status Label, so callers can keep updating it.
static func section_header(index: int, title: String) -> HBoxContainer:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", SPACE_M)
	var dash := ColorRect.new()
	dash.color = ACCENT
	dash.custom_minimum_size = Vector2(12, 2)
	dash.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	row.add_child(dash)
	var label := Label.new()
	label.text = "%02d %s" % [index, title.to_upper()]
	label.add_theme_color_override("font_color", NEUTRAL_500)
	label.add_theme_font_size_override("font_size", 9)
	row.add_child(label)
	var rule := fade_rule()
	rule.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	row.add_child(rule)
	var status := Label.new()
	status.add_theme_color_override("font_color", NEUTRAL_600)
	status.add_theme_font_size_override("font_size", 9)
	row.add_child(status)
	return row

static func divider() -> Control:
	var line := ColorRect.new()
	line.color = DIVIDER
	line.custom_minimum_size = Vector2(0, 1)
	return line

## A dim label / bright value pair, the sidebar's basic row.
static func kv_row(key: String, value: String, value_color: Color = TEXT) -> HBoxContainer:
	var row := HBoxContainer.new()
	var k := Label.new()
	k.text = key
	k.add_theme_color_override("font_color", NEUTRAL_500)
	k.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(k)
	var v := Label.new()
	v.text = value
	v.add_theme_color_override("font_color", value_color)
	v.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	row.add_child(v)
	return row
