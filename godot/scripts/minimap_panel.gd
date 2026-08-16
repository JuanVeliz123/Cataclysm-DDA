extends Control
## Pixel minimap panel.
##
## src/godot_pixel_minimap.cpp renders the minimap on the game thread into an RGBA8
## buffer; this panel asks for a size, polls the generation counter, and uploads a
## texture only when a new frame exists.

## Minimap edge length in pixels. The C++ projector fits the whole view distance
## into whatever size it is given, so this is a quality/cost knob.
const SIZE_PX := 192

var _host: Node
var _texture_rect: TextureRect
var _texture: ImageTexture
var _last_generation: int = -1
var _requested_size: Vector2i = Vector2i.ZERO

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _texture_rect != null:
		return
	# Bottom-left: the HUD sidebar owns the right edge and the message log the
	# bottom strip beside it.
	var frame := PanelContainer.new()
	frame.name = "MinimapFrame"
	frame.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	frame.offset_left = 8.0
	frame.offset_top = -float(SIZE_PX) - 24.0
	frame.offset_right = float(SIZE_PX) + 24.0
	frame.offset_bottom = -8.0
	frame.mouse_filter = Control.MOUSE_FILTER_IGNORE

	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.05, 0.06, 0.08, 0.82)
	sb.border_color = Color(0.25, 0.28, 0.32, 0.9)
	sb.set_border_width_all(1)
	sb.set_content_margin_all(6)
	frame.add_theme_stylebox_override("panel", sb)

	_texture_rect = TextureRect.new()
	_texture_rect.name = "MinimapTexture"
	_texture_rect.custom_minimum_size = Vector2(SIZE_PX, SIZE_PX)
	_texture_rect.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	_texture_rect.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	# The minimap is one pixel per map tile; smoothing it just blurs the grid.
	_texture_rect.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	_texture_rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	frame.add_child(_texture_rect)
	add_child(frame)

func refresh() -> void:
	if _host == null or _texture_rect == null:
		return

	# Publishing a size is what enables rendering at all, so keep it current.
	var want := Vector2i(SIZE_PX, SIZE_PX)
	if want != _requested_size:
		_requested_size = want
		_host.set_minimap_size(want.x, want.y)

	var generation: int = _host.get_minimap_generation()
	if generation == _last_generation:
		return
	_last_generation = generation

	var img: Image = _host.get_minimap_image()
	if img == null or img.get_width() <= 0:
		return
	# Reuse the texture unless the frame size changed; update() avoids a realloc.
	if _texture != null and _texture.get_size() == Vector2(img.get_width(), img.get_height()):
		_texture.update(img)
	else:
		_texture = ImageTexture.create_from_image(img)
		_texture_rect.texture = _texture

## Stop the game thread rendering frames nobody will look at.
func release() -> void:
	if _host != null:
		_host.set_minimap_size(0, 0)
	_requested_size = Vector2i.ZERO
	_last_generation = -1
