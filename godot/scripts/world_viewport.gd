extends TextureRect
## The world's own viewport (ADR-004; item 3D-0 of ADR-006's plan).
##
## Everything that is *the world* renders in here -- MapView, its tile batches,
## glyph layers, contact shadows, field particles, animation overlay -- and the
## result is composited as a single texture beneath the UI. Before this the world
## and the interface shared one canvas and were separated only by `z_index`,
## which cost two things that are now impossible by construction:
##
##   - the animation overlay could draw over the sidebar. At z 32 it was above
##     every panel, so combat text and explosions landed on the HUD;
##   - no full-screen effect could be applied to the world, because a
##     `CanvasLayer` over the viewport would tint the sidebar and the menus with
##     it. That is why the presentation grade lives inside the tile shader, and
##     why `field_particles.gd` -- which has its own materials -- is not graded.
##
## This change buys the boundary and nothing else. It does not move the grade out
## of the tile shader: those constants have never been looked at by anyone (VER-1)
## and moving them and changing them in one step would leave nobody able to say
## which did what.
##
## The rect is sized to the **drawable area** -- the window minus whatever the
## sidebar reserves -- so the world's idea of its own extent is simply its
## viewport. That is why `set_reserved_right` lives here now instead of on
## MapView, which no longer has to subtract the sidebar from its own arithmetic.
##
## When the 3D backend arrives (3D-1) it goes in here: `own_world_3d`, a
## `Camera3D`, and the tile batches as `MultiMeshInstance3D`. Nothing outside this
## node needs to know.

## Never narrower than this, so a pathological sidebar width cannot collapse the
## world to nothing and take the zoom arithmetic with it.
const MIN_WIDTH := 64.0

var _sub: SubViewport
## The node whose visibility this rect mirrors; see _mirror_visibility.
##
## Deliberately untyped. It is a `Node2D` under the 2D backend and a `Node3D` under
## the 3D one, and their only common ancestor is `Node`, which declares neither
## `visible` nor `visibility_changed` -- so a `Node`-typed reference would fail the
## static check on both. Untyped keeps the access dynamic, which is what a duck type
## needs.
var _world
## Width the sidebar covers, and therefore the width the world does not get.
var _reserved: float = 0.0

## Whether the world in here is a 3D scene (ADR-006's backend) or the 2D canvas.
##
## Called before `setup`, because it decides what the viewport renders at all: with
## 3D disabled the viewport skips the 3D pass entirely, and with it enabled the
## viewport takes its own `World3D` so the map's camera and environment cannot
## reach anything outside this node.
func set_3d_enabled(on: bool) -> void:
	_ensure_viewport()
	_sub.disable_3d = not on
	_sub.own_world_3d = on
	# A 3D scene has no `queue_redraw` to mark the viewport dirty with, and this
	# viewport's texture is held by hand on a TextureRect rather than by a
	# SubViewportContainer, so "update when the texture is visible" is resting on
	# usage detection that nothing here verifies. Redraw every frame while the world
	# is up; it is one texture at window resolution and the world animates anyway.
	_sub.render_target_update_mode = SubViewport.UPDATE_ALWAYS if on \
		else SubViewport.UPDATE_WHEN_VISIBLE
	# The 3D pipeline paints its own background from the environment, and the 2D one
	# lets SessionBg show through. Both end up the same colour; the difference is
	# which layer owns it.
	_sub.transparent_bg = not on

## Move @p world into the viewport and start tracking its size and visibility.
##
## Typed as `Node` rather than `CanvasItem` because the world is a `Node2D` under
## the 2D backend and a `Node3D` under the 3D one. Both have `visible` and
## `visibility_changed`, which is all this needs of it.
func setup(world: Node) -> void:
	_ensure_viewport()
	_world = world
	if world.get_parent() != _sub:
		world.reparent(_sub, false)
	# Connected by name rather than through the signal object, for the same reason
	# `_world` is untyped: `visibility_changed` is declared on CanvasItem and on
	# Node3D, and on neither of their common ancestors.
	if not world.is_connected("visibility_changed", _mirror_visibility):
		world.connect("visibility_changed", _mirror_visibility)
	_mirror_visibility()
	_resize()

## The SubViewport the world renders into. Exposed for the probe, which has no
## other way to ask whether the boundary is actually there.
func world_viewport() -> SubViewport:
	return _sub

func _ready() -> void:
	_ensure_viewport()
	# Resizing the window changes the drawable area, and the drawable area is
	# this rect's size.
	get_viewport().size_changed.connect(_resize)
	_resize()

func _ensure_viewport() -> void:
	if _sub != null and is_instance_valid(_sub):
		return
	# Takes no input at all, which is why MapView's Ctrl+wheel zoom moved to
	# host.gd. Routing input in here instead would mean the game sees each mouse
	# event twice -- once forwarded through the world, once through the host's own
	# path, which converts pixels to cells in *window* coordinates and would be
	# measuring the wrong rectangle.
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	stretch_mode = TextureRect.STRETCH_SCALE
	# The composite is 1:1 device pixels by construction (see _resize), so nearest
	# is exact rather than a choice. Linear here is how a pixel-art frame goes soft
	# without anyone touching a sprite.
	texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST

	_sub = SubViewport.new()
	_sub.name = "World"
	# SessionBg shows through where the map does not reach, which is what the
	# window looked like before the world had a viewport of its own. An opaque
	# SubViewport would clear to the project's colour instead.
	_sub.transparent_bg = true
	# Nothing in here is a Control, and saying so keeps Godot from walking the
	# world looking for something to focus.
	_sub.gui_disable_input = true
	# Render while it is on screen, and not while the world is hidden -- which is
	# most of a session's lifetime, since the main menu and the overmap both hide
	# the world.
	_sub.render_target_update_mode = SubViewport.UPDATE_WHEN_VISIBLE
	# Tiles are atlas sub-rects; linear filtering samples across a region edge and
	# bleeds the neighbouring sprite in. The project sets this for the root
	# viewport (`textures/canvas_textures/default_texture_filter`) and a
	# SubViewport does not inherit it, so set it here or the whole map softens.
	_sub.canvas_item_default_texture_filter = \
		Viewport.DEFAULT_CANVAS_ITEM_TEXTURE_FILTER_NEAREST
	# The fire glow keys on the tile shader writing above 1.0, which needs an HDR
	# render target. `viewport/hdr_2d` in project.godot covers the root viewport
	# only; without this the world clamps at 1.0 and the glow pass silently has
	# nothing to find. Guarded because it is the one property here that a Godot
	# version could plausibly not have.
	if "use_hdr_2d" in _sub:
		_sub.use_hdr_2d = true
	add_child(_sub)
	texture = _sub.get_texture()

## Width the sidebar occupies on the right, which the world must not draw into.
func set_reserved_right(px: float) -> void:
	var want := maxf(0.0, px)
	if is_equal_approx(want, _reserved):
		return
	_reserved = want
	_resize()

## Size the world to the drawable area, and its render target to the pixels that
## area actually occupies on the display.
##
## Those are two different numbers, and conflating them is the trap here. The
## project stretches the canvas (`window/stretch/mode="canvas_items"` over a
## 1280x720 base in a 1600x900 window), so a Control's coordinates are logical
## units and the frame is rasterised at 1.25x that. A `SubViewport` sized in
## logical units would render the world at 1280 wide and let the stretch upscale
## it -- 80% of the resolution the world has today, which is a regression bought
## by a change that is supposed to alter nothing.
##
## So the render target is sized in device pixels and `size_2d_override` hands the
## world back the logical coordinate space it has always drawn in. MapView's
## arithmetic is then unchanged: `get_viewport_rect()` reports the same logical
## extent it read from the root viewport before.
##
## Rendering the world at a *lower* resolution than the UI is the other half of
## what this buys (ADR-004), and it is now one factor in one place -- but it is
## opt-in, not the accident of a default.
func _resize() -> void:
	var logical := get_viewport().get_visible_rect().size
	if logical.x < 2.0 or logical.y < 2.0:
		return
	position = Vector2.ZERO
	# Whole logical units: a rect on a half pixel resamples everything inside it.
	size = Vector2(floorf(maxf(MIN_WIDTH, logical.x - _reserved)), floorf(logical.y))
	if _sub == null or not is_instance_valid(_sub):
		return
	var device := _canvas_scale()
	_sub.size_2d_override_stretch = true
	_sub.size_2d_override = Vector2i(size)
	# Two pixels is the documented floor for a SubViewport: below it nothing is
	# displayed at all, which is a blank world rather than a small one.
	_sub.size = Vector2i(maxi(2, int(round(size.x * device.x))),
		maxi(2, int(round(size.y * device.y))))

## Logical units to device pixels, which is what the canvas stretch is doing.
##
## Derived from the window against the viewport rather than read from a property,
## because the number that matters is the ratio actually in force: stretch mode,
## aspect and any HiDPI factor all land in it. A window not yet sized reads as 1.
func _canvas_scale() -> Vector2:
	var win := get_window()
	var logical := get_viewport().get_visible_rect().size
	if win == null or logical.x < 1.0 or logical.y < 1.0:
		return Vector2.ONE
	return Vector2(
		clampf(float(win.size.x) / logical.x, 1.0, 4.0),
		clampf(float(win.size.y) / logical.y, 1.0, 4.0))

## The world's visibility is the host's switch -- `map_view.visible` is set in
## half a dozen places -- and a visible rect over a hidden world would go on
## compositing the last frame the world drew, so the main menu would open over a
## still of the map.
##
## Mirrors the world's *own* flag rather than `is_visible_in_tree()`: in-tree
## visibility includes this node, so reading it would make the mirror reflect
## itself and latch the world off.
func _mirror_visibility() -> void:
	if _world == null or not is_instance_valid(_world):
		return
	var want: bool = _world.visible
	if want != visible:
		visible = want
