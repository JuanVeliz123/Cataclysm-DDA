extends Node2D
## Contact shadows under creatures (ADR-005 item 4).
##
## The cheapest depth cue available to a fixed-angle top-down view, and the
## largest single gain in apparent depth per line of code: without one, a
## creature is a sprite pasted on the floor rather than a thing standing on it.
##
## Derived entirely from what MapView already tracks. Creature instances record
## an anchor -- the bottom centre of the sprite's quad, which is the point that
## says which tile something is standing on whatever its sprite offset -- so a
## shadow is that anchor plus a soft blob. No new draw command, no new snapshot
## field, and no art: the blob is a radial gradient built at runtime.
##
## Deliberately not moved by hit reactions. The body recoils; the shadow is on
## the ground and stays where the feet were.

## Blob width as a fraction of the sprite's own width.
const WIDTH_SCALE := 0.62
## Shadows are flattened, because the view is not straight down.
const FLATTEN := 0.42
const MAX_ALPHA := 0.38
## How far up from the sprite's base the blob sits, as a fraction of a tile.
##
## The anchor is the bottom edge of the sprite's quad, which for a character is
## the bottom edge of its tile -- but in a three-quarter view the feet read as
## resting nearer the middle of the tile, so a blob centred on the anchor sits
## visibly below them and the character looks like it is floating above its own
## shadow. Lifting it puts the contact point back under the feet.
const LIFT := 0.20

var _blobs: Array = []
var _tile_size: Vector2i = Vector2i(32, 32)
## Scaled by daylight: an overcast midnight casts no sun shadow, and drawing one
## anyway reads as grime on the floor rather than as depth. Firelight ought to
## cast its own and does not yet -- noted in ADR-005.
var _strength: float = 1.0
var _dot: GradientTexture2D

func set_shadows(blobs: Array, tile_size: Vector2i, strength: float) -> void:
	_blobs = blobs
	_tile_size = tile_size
	_strength = clampf(strength, 0.0, 1.0)
	queue_redraw()

func _draw() -> void:
	if _blobs.is_empty() or _strength <= 0.02:
		return
	var tex := _blob_texture()
	for b in _blobs:
		var anchor: Vector2 = b["anchor"]
		var w: float = float(b["width"]) * WIDTH_SCALE
		var h: float = w * FLATTEN
		# Centred on the contact point, not on the anchor: see LIFT.
		var cy: float = anchor.y - float(_tile_size.y) * LIFT
		var rect := Rect2(anchor.x - w * 0.5, cy - h * 0.5, w, h)
		draw_texture_rect(tex, rect, false, Color(0, 0, 0, MAX_ALPHA * _strength))

## A soft round blob, built rather than shipped so there is no art dependency.
func _blob_texture() -> Texture2D:
	if _dot != null:
		return _dot
	var g := Gradient.new()
	g.offsets = PackedFloat32Array([0.0, 0.55, 1.0])
	g.colors = PackedColorArray([
		Color(1, 1, 1, 1), Color(1, 1, 1, 0.7), Color(1, 1, 1, 0),
	])
	_dot = GradientTexture2D.new()
	_dot.gradient = g
	_dot.fill = GradientTexture2D.FILL_RADIAL
	_dot.fill_from = Vector2(0.5, 0.5)
	_dot.fill_to = Vector2(1.0, 0.5)
	_dot.width = 32
	_dot.height = 32
	return _dot
