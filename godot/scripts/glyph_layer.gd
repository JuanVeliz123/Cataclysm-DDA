extends Node2D
## Fallback glyphs for one map layer (SP-1, step 5 of the resolution chain).
##
## When an id resolves to no sprite anywhere -- not itself, not its `looks_like`,
## not the tileset's ASCII set, not an "unknown_<category>" placeholder -- C++
## publishes the symbol the type's JSON declares instead of dropping the tile.
## Those arrive in MapSnapshot::copy_glyph_list and are painted here with a font.
##
## One of these exists per map_layer that has glyphs, seated in that layer's flat
## band by MapView's ordering pass, so a fallback floor still draws under a
## monster that did find a sprite. Not depth-sorted -- one node holds a whole
## layer, so it can only sit at one depth -- which is an approximation a tileset
## with full sprite coverage never sees, because it emits no glyphs at all.
##
## Positions are map-local pixels, exactly like the draw list, so MapView's zoom
## and camera transform apply for free.

## Ints per packed glyph: dest_x, dest_y, layer, codepoint, fg, bg.
## Must match MapSnapshot::glyph_stride in src/godot_map_snapshot.h.
const GLYPH_STRIDE := 6

var _cmds: PackedInt32Array = PackedInt32Array()
var _tile_size: Vector2i = Vector2i(32, 32)
var _font: Font

func set_commands(cmds: PackedInt32Array, tile_size: Vector2i) -> void:
	_cmds = cmds
	_tile_size = tile_size
	queue_redraw()

func _draw() -> void:
	if _cmds.is_empty():
		return
	if _font == null:
		_font = ThemeDB.fallback_font
	# A glyph stands in for a whole tile, so it is sized to fill one.
	var font_size: int = maxi(8, int(_tile_size.y * 0.9))
	var n := _cmds.size()
	var i := 0
	while i + GLYPH_STRIDE - 1 < n:
		var x: int = _cmds[i]
		var y: int = _cmds[i + 1]
		var codepoint: int = _cmds[i + 3]
		var fg: int = _cmds[i + 4]
		var bg: int = _cmds[i + 5]
		i += GLYPH_STRIDE

		var rect := Rect2(float(x), float(y), float(_tile_size.x), float(_tile_size.y))
		if bg != 0:
			draw_rect(rect, _unpack_rgba(bg))
		# Surrogates and control characters are not drawable; String.chr would
		# either fail or emit a replacement box.
		if codepoint <= 32 or codepoint > 0x10FFFF or (codepoint >= 0xD800 and codepoint <= 0xDFFF):
			continue
		var glyph := String.chr(codepoint)
		if glyph.is_empty():
			continue
		draw_string(_font, Vector2(rect.position.x, rect.position.y + _tile_size.y * 0.82),
			glyph, HORIZONTAL_ALIGNMENT_CENTER, float(_tile_size.x), font_size,
			_unpack_rgba(fg))

## Unpack a map_glyph_cmd colour (0xRRGGBBAA, delivered as a signed int32).
func _unpack_rgba(packed: int) -> Color:
	var a: int = packed & 0xFF
	return Color8(
		(packed >> 24) & 0xFF,
		(packed >> 16) & 0xFF,
		(packed >> 8) & 0xFF,
		255 if a == 0 else a
	)
