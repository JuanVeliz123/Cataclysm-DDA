extends Node2D
## Animation overlay: explosions, bullets, hit markers, aim line and cursor.
##
## The GODOT branches in src/animation.cpp publish these as primitives in map
## coordinates (src/godot_anim_snapshot.*) instead of writing glyphs into the
## terrain window, which the backend does not present.
##
## This is a child of MapView, so MapView's zoom and camera transform apply for
## free and a map position is just `(pos - view_origin) * tile_size`.

## Ints per packed command: kind, x, y, z, codepoint, fg, bg.
## Must match AnimSnapshot::cmd_stride in src/godot_anim_snapshot.h.
const CMD_STRIDE := 7

const KIND_GLYPH := 0
const KIND_HIGHLIGHT := 1

## anim_align in src/godot_anim_snapshot.h.
const ALIGN_LEFT := 0
const ALIGN_CENTER := 1
const ALIGN_RIGHT := 2

## Combat text is read at a glance while something is trying to kill you, so it
## is sized against the tile rather than the UI and always outlined.
const SCT_FONT_SCALE := 0.62

var _host: Node
var _cmds: PackedInt32Array = PackedInt32Array()
var _view_origin: Vector2i = Vector2i.ZERO
var _tile_size: Vector2i = Vector2i(32, 32)
var _generation: int = -1
var _font: Font
## Scrolling combat text for this frame; see AnimSnapshot::anim_text.
var _texts: Array = []

func setup(host: Node) -> void:
	_host = host
	# Above every MapView tile layer. map_view.gd uses z_index = layer + 1, and
	# there are fewer than 16 layers.
	z_index = 32
	_cmds = PackedInt32Array()
	_generation = -1
	queue_redraw()

## Called by MapView after it refreshes, so origin and tile size agree with the
## frame the tiles were batched from.
func refresh(view_origin: Vector2i, tile_size: Vector2i) -> void:
	if _host == null or not _host.has_method("get_anim_generation"):
		return

	var generation: int = _host.get_anim_generation()
	var geometry_changed := view_origin != _view_origin or tile_size != _tile_size
	if generation == _generation and not geometry_changed:
		return

	_generation = generation
	_view_origin = view_origin
	_tile_size = tile_size
	_cmds = _host.get_anim_commands()
	# Guarded: an older library has no combat text, and its absence should cost
	# the numbers rather than the whole overlay.
	_texts = _host.get_anim_texts() if _host.has_method("get_anim_texts") else []
	queue_redraw()

func _draw() -> void:
	_draw_combat_text()
	if _cmds.is_empty():
		return
	if _font == null:
		_font = ThemeDB.fallback_font
	# Animation glyphs are single characters meant to fill a tile.
	var font_size: int = maxi(8, int(_tile_size.y * 0.9))

	var n := _cmds.size()
	var i := 0
	while i + CMD_STRIDE - 1 < n:
		var kind: int = _cmds[i]
		var tx: int = _cmds[i + 1] - _view_origin.x
		var ty: int = _cmds[i + 2] - _view_origin.y
		var codepoint: int = _cmds[i + 4]
		var fg: int = _cmds[i + 5]
		i += CMD_STRIDE

		var rect := Rect2(
			float(tx * _tile_size.x), float(ty * _tile_size.y),
			float(_tile_size.x), float(_tile_size.y)
		)
		if kind == KIND_HIGHLIGHT:
			# A translucent wash, so the tile underneath stays readable.
			draw_rect(rect, Color(0.45, 0.75, 1.0, 0.28))
			continue
		if kind != KIND_GLYPH:
			continue
		if codepoint <= 32 or codepoint > 0x10FFFF or (codepoint >= 0xD800 and codepoint <= 0xDFFF):
			continue

		var glyph := String.chr(codepoint)
		if glyph.is_empty():
			continue
		var color := _unpack_rgba(fg)
		# Animation markers sit on top of arbitrary terrain; an outline keeps them
		# legible without needing to know what is underneath.
		var baseline := Vector2(rect.position.x, rect.position.y + _tile_size.y * 0.82)
		draw_string_outline(_font, baseline, glyph, HORIZONTAL_ALIGNMENT_CENTER,
			float(_tile_size.x), font_size, 3, Color(0, 0, 0, 0.85))
		draw_string(_font, baseline, glyph, HORIZONTAL_ALIGNMENT_CENTER,
			float(_tile_size.x), font_size, color)

## Unpack an anim_cmd colour (0xRRGGBBAA, delivered as a signed int32).
func _unpack_rgba(packed: int) -> Color:
	var a: int = packed & 0xFF
	return Color8(
		(packed >> 24) & 0xFF,
		(packed >> 16) & 0xFF,
		(packed >> 8) & 0xFF,
		255 if a == 0 else a
	)

## Scrolling combat text: damage numbers, "Critical!", healing, XP.
##
## The C++ side hands over bubble map coordinates that already include the
## scroll offset for the current step, so the text rises on its own as the game
## advances the animation; nothing here needs a clock.
##
## The two runs of a message ("17", then "Critical!") share an anchor and are
## laid out adjacent, with the alignment applied to their combined width. That
## differs from SDL, which emplaces both at the same point and lets them
## overlap -- the intent there is clearly one message, so this draws one.
func _draw_combat_text() -> void:
	if _texts.is_empty():
		return
	if _font == null:
		_font = ThemeDB.fallback_font
	var font_size: int = maxi(9, int(_tile_size.y * SCT_FONT_SCALE))

	# Group the runs by anchor so a message is measured and placed as a whole.
	var groups: Dictionary = {}
	for t in _texts:
		var key: Vector3i = t.get("pos", Vector3i.ZERO)
		if not groups.has(key):
			groups[key] = []
		groups[key].append(t)

	for key in groups:
		var runs: Array = groups[key]
		runs.sort_custom(func(a, b): return int(a.get("run", 0)) < int(b.get("run", 0)))

		var total := 0.0
		for t in runs:
			total += _font.get_string_size(str(t.get("text", "")),
				HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x

		var pos: Vector3i = key
		# Anchor at the centre of the tile's top edge: the text belongs to the
		# tile but should not sit on top of whatever is standing there.
		var origin := Vector2(
			(float(pos.x - _view_origin.x) + 0.5) * float(_tile_size.x),
			float(pos.y - _view_origin.y) * float(_tile_size.y))

		var align: int = int(runs[0].get("align", ALIGN_CENTER))
		var x := origin.x
		if align == ALIGN_CENTER:
			x -= total * 0.5
		elif align == ALIGN_RIGHT:
			x -= total

		for t in runs:
			var text := str(t.get("text", ""))
			if text.is_empty():
				continue
			var color := _unpack_rgba(int(t.get("fg", 0)))
			color.a *= clampf(float(t.get("life", 1.0)), 0.0, 1.0)
			var at := Vector2(x, origin.y)
			# Over arbitrary terrain and often over a creature, so the outline is
			# not decoration -- without it the number is unreadable half the time.
			draw_string_outline(_font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1,
				font_size, 4, Color(0, 0, 0, color.a * 0.9))
			draw_string(_font, at, text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, color)
			x += _font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x
