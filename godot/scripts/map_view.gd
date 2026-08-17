extends Node2D
## Tileset MapView — paints C++ draw-list sprites (ADR-002 present, ADR-003 draw layer).
## Godot owns present; sprite resolution stays in C++ (godot_map_snapshot).
##
## Tiles are drawn with one MultiMeshInstance2D per (layer, atlas) rather than a
## draw_texture_rect_region per tile, so a full screen of terrain costs a handful
## of draw calls instead of thousands.

const TILE_SHADER := preload("res://shaders/map_tiles.gdshader")
const ANIM_OVERLAY := preload("res://scripts/anim_overlay.gd")
const GLYPH_LAYER := preload("res://scripts/glyph_layer.gd")
const FIELD_PARTICLES := preload("res://scripts/field_particles.gd")
const SHADOW_LAYER := preload("res://scripts/shadow_layer.gd")

## Ints per packed draw command: atlas, src x/y/w/h, dest x/y, layer, tint, rot_flags.
## Must match MapSnapshot::cmd_stride in src/godot_map_snapshot.h.
const CMD_STRIDE := 10
## Ints per packed fallback glyph: dest x/y, layer, codepoint, fg, bg.
## Must match MapSnapshot::glyph_stride.
const GLYPH_STRIDE := 6
## Ints per packed field emitter: dest x/y, kind, intensity.
## Must match MapSnapshot::field_stride.
const FIELD_STRIDE := 4

## Quarter-turn rotations, clockwise, matching what SDL's render_copy_ex does with
## the rotation bits of map_draw_cmd::rot_flags: 1 is +90 degrees, 2 is 180, 3 is -90.
const ROTATION_ANGLES := [0.0, PI * 0.5, PI, -PI * 0.5]

## Bit layout of map_draw_cmd::rot_flags; see enum cmd_flag in
## src/godot_map_snapshot.h.
const ROTATION_MASK := 0x3
const FLAG_SWAY := 1 << 2
## Mirror about the sprite's centre -- how a character faces left.
const FLAG_FLIP_X := 1 << 3
const PALETTE_SHIFT := 4
const PALETTE_MASK := 0xF << PALETTE_SHIFT
## The sprite overhangs its tile cell; see cmd_flag_tall.
const FLAG_TALL := 1 << 8
## Bits 9-12: z-levels below the avatar's own, 0-15; see cmd_z_below_shift.
const Z_BELOW_SHIFT := 9
const Z_BELOW_MASK := 0xF << Z_BELOW_SHIFT

## Everything MapView owns has to stay inside a narrow z window. The host stacks
## the UI over the map starting at the minimap panel on z 8 and running to 18,
## and SessionBg sits under the map on -1, so the map's whole budget is 0..7 --
## eight values for eleven layers. Spending z on layer ordering does not fit, and
## trying to (batches climbed to 43, particles sat on 19, the animation overlay
## on 64) is what put sprites on top of open menus.
##
## So z_index does not order the map at all. Same-z canvas items draw in tree
## order, _rebuild_batches already re-seats every batch every frame, and tree
## order has no range limit -- so the ordering is free and the z budget stays
## spent on separating map from UI, which is all it is good for.
## Everything the map draws into the world shares one z and orders by child
## order. The particles and the contact shadows are in here too: they are part
## of the world and have to interleave with it.
const Z_TILES := 1
## The animation overlay is not. Combat text and explosion frames are drawn over
## the world rather than in it, so they take the one z above it -- and no more
## than one, because they must still lose to a menu.
const Z_ANIM := 2
## Lowest z the host's UI panels use. Nothing under MapView may reach it.
const Z_UI_FLOOR := 8

## Where the tall band starts. Everything flat ranks below this, everything
## depth-sorted above it, and the few whole-map effect layers sit in the gap.
const TALL_BAND := 32
## Rows above the top of the view still have sprites hanging into it, so the row
## a rank is built from can be negative. Bias it so those stay in the tall band
## instead of wrapping down into the flat one.
const ROW_BIAS := 8
## Rank values one z-level occupies. A rank inside a level is
## TALL_BAND + (row + ROW_BIAS) * 16 + layer, so this has to clear the tallest
## view anyone will publish -- 4096 rows of it, which is twenty times the
## reality bubble.
const Z_LEVEL_SPAN := 1 << 16
## Deepest level below the avatar a command can name; matches max_z_below in
## src/godot_map_snapshot.h, and is what the four flag bits can hold.
const MAX_Z_BELOW := 15
## Where the avatar's own level starts. Anything seated by hand rather than
## from a command's flags belongs in this block: the whole-map effect layers
## draw on the floor the player is standing on, not on the bottom of a pit.
const TOP_LEVEL_BASE := MAX_Z_BELOW * Z_LEVEL_SPAN

## Draw order within the map; see Z_TILES for why this is a rank and not a
## z_index.
##
## Sprites are anchored to the bottom of their cell, so a sprite can only ever
## cover rows *above* its own -- which means a sprite that fits its cell cannot
## overlap another cell at all, and its order against other rows does not
## matter. Those rank by layer alone and draw first, all of them, cheaply.
##
## What is left is the content that stands up: tall sprites and creatures. Those
## rank by row, so a tree one row in front of a zombie covers it and the same
## tree one row behind it does not. This is the depth ordering of ADR-005 item 3,
## and it is why the map is no longer strictly layer-major: layer only breaks
## ties within a row now.
##
## Creatures count as standing even when their sprite fits its cell. A 32x32
## monster is still a thing at a position rather than part of the floor, and
## ranking it with the flat content would put it under every tree on the map.
##
## `z_below` is levels below the avatar's (ADR-005 item 1), and it outranks
## everything else: a whole level is either nearer the camera than another or it
## is not, and no arrangement of rows within one can change that. So each level
## gets its own block of ranks, deepest first, and the ordering inside a block is
## exactly what it was when there was only one.
static func depth_rank(layer: int, tall: bool, row: int, z_below: int = 0) -> int:
	var base := (MAX_Z_BELOW - clampi(z_below, 0, MAX_Z_BELOW)) * Z_LEVEL_SPAN
	if not tall:
		return base + layer * 2
	return base + TALL_BAND + (row + ROW_BIAS) * 16 + layer

var _host: Node
var _atlases: Array[Texture2D] = []
var _cmds: PackedInt32Array = PackedInt32Array()
## Fallback glyphs for the same frame; see glyph_layer.gd.
var _glyphs: PackedInt32Array = PackedInt32Array()
## map_layer -> glyph_layer.gd node, reused across frames.
var _glyph_layers: Dictionary = {}
var _tile_size: Vector2i = Vector2i(32, 32)
var _view_size: Vector2i = Vector2i.ZERO
var _atlases_loaded: bool = false
var _zoom: float = 1.0
## Manual zoom multiplier on top of auto fit (mouse wheel). 1.0 = fit-to-view.
var _user_zoom: float = 1.0

## Unit quad, (0,0)..(1,1) with matching UVs, so an instance transform that
## scales by the tile size and translates to the destination maps straight onto
## the tile's pixels.
var _quad: ArrayMesh
## "layer:atlas:sway:palette" -> MultiMeshInstance2D, reused across frames.
var _batches: Dictionary = {}
## Draw-list generation already batched. The host refreshes us every frame, but
## the map only changes per turn, so this skips the bulk of the work.
var _batched_generation: int = -1
## Viewport size the batches were laid out for; a resize needs a re-fit.
var _batched_area: Vector2 = Vector2.ZERO
## Animation overlay, parented here so it inherits the zoom/camera transform.
var _anim_overlay: Node2D
## Fire and smoke particles (SP-6), likewise a child so it shares the transform.
var _field_particles: Node2D
## Contact shadows under creatures (ADR-005 item 4).
var _shadow_layer: Node2D
## Map-tile coordinate of the top-left of the current view.
var _view_origin: Vector2i = Vector2i.ZERO
## Last extent published to C++, so we only ask when it actually changes.
var _requested_tiles: Vector2i = Vector2i.ZERO

## The light/visibility texture (SP-3, SP-4): one texel per published tile, R
## visibility and G light amount. Uploaded when C++ bumps its generation.
var _light_tex: ImageTexture
var _light_generation: int = -1
## Whether C++ has been told the pass is running. Until it is, C++ keeps baking
## light into the per-sprite tints and this shader must not darken again.
var _light_pass_announced: bool = false

## Palette ramps for palette-swap variants (SP-8), one row per palette plus an
## identity row. Null when the tileset declares none.
var _palette_tex: ImageTexture

## Glow over the world, so fire and explosions read as light sources rather than
## as bright squares. It lives here rather than on a CanvasLayer over everything
## because a viewport-wide environment would bloom the UI too; keyed on an HDR
## threshold, only what the tile shader writes above 1.0 blooms, and nothing
## else in the renderer does.
var _world_env: WorldEnvironment
## Held separately so it can be detached and reattached; see
## _sync_world_environment.
var _map_environment: Environment

## Layers the avatar draws on -- map_layer::player and ::player_overlay in
## src/godot_map_snapshot.h. Their batches opt out of the light pass: the player
## is the viewpoint, and dimming it to match the floor makes the character hard
## to find on a dark screen. The overlays opt out with the body, or the coat is
## lit and the person wearing it is not.
const PLAYER_LAYERS := [9, 10]
## Every layer a creature draws on, body and overlays. These are the ones whose
## instances can move between map rebuilds (SP-5), and a hit has to displace a
## character's clothes along with the character.
const CREATURE_LAYERS := [7, 8, 9, 10]
## The body halves of those pairs -- map_layer::monster and ::player. Overlays
## share their body's anchor, so anything that is per-creature rather than
## per-sprite must filter to these.
const MONSTER_LAYER_BODY := 7
const PLAYER_LAYER_BODY := 9
## map_layer::field, which the smoke particles seat themselves against.
const FIELD_LAYER := 4

## Ints per packed hit event: id, x, y, z, dir x/y, flash.
## Must match AnimSnapshot::hit_stride in src/godot_anim_snapshot.h.
const HIT_STRIDE := 7
## How long a hit reaction runs. Long enough to see, short enough not to still
## be playing when the next blow lands in a melee exchange.
const HIT_DURATION := 0.22
## Peak lunge, as a fraction of a tile.
const HIT_OFFSET := 0.34

## Hits being animated: { tile: Vector2i, dir: Vector2, flash: Color, t: float }.
var _hits: Array = []
## Highest hit id seen, so a poll only picks up blows we have not played.
var _hit_seen: int = 0
## Creature-layer instances, by batch key: [{ slot, tile, xform, color }].
## Rebuilt with the batches; this is what lets a hit find the right instance
## without lifting the creature out of the MultiMesh.
var _creature_slots: Dictionary = {}

func setup(host: Node) -> void:
	_host = host
	_atlases_loaded = false
	_atlases.clear()
	_cmds = PackedInt32Array()
	_glyphs = PackedInt32Array()
	_light_tex = null
	_light_generation = -1
	_light_pass_announced = false
	_palette_tex = null
	_hits.clear()
	_hit_seen = 0
	_creature_slots.clear()
	_user_zoom = 1.0
	_clear_batches()
	_batched_generation = -1
	_batched_area = Vector2.ZERO
	_ensure_anim_overlay()
	_ensure_field_particles()
	_ensure_shadow_layer()
	_ensure_world_environment()
	queue_redraw()

func _ensure_shadow_layer() -> void:
	if _shadow_layer != null and is_instance_valid(_shadow_layer):
		return
	_shadow_layer = Node2D.new()
	_shadow_layer.name = "ShadowLayer"
	_shadow_layer.set_script(SHADOW_LAYER)
	# Under the creatures that cast them and over the floor they fall on. That is
	# a position in the child order, not a z_index (see Z_TILES); the seating pass
	# in _rebuild_batches puts it just before the monster-body band.
	_shadow_layer.z_index = Z_TILES
	add_child(_shadow_layer)

## The environment is viewport-global, so it must follow MapView's visibility
## rather than its position in the tree. Called from refresh and on show/hide.
##
## Detaching the Environment resource, not hiding the node: WorldEnvironment
## derives from Node and has no `visible` property, so assigning one throws and
## leaves the glow on everywhere -- which is how the first attempt at this fix
## silently did nothing. A null environment is the supported way to switch it
## off.
func _sync_world_environment() -> void:
	if _world_env == null or not is_instance_valid(_world_env):
		return
	_world_env.environment = _map_environment if visible else null

func _notification(what: int) -> void:
	if what == NOTIFICATION_VISIBILITY_CHANGED:
		_sync_world_environment()

func _ensure_world_environment() -> void:
	if _world_env != null and is_instance_valid(_world_env):
		return
	var env := Environment.new()
	env.background_mode = Environment.BG_CANVAS
	env.glow_enabled = true
	env.glow_hdr_threshold = 1.0
	env.glow_intensity = 0.7
	# glow_bloom must be zero. It is a constant lift applied to every pixel
	# *before* the HDR threshold is considered, so any value above zero blooms
	# the entire frame -- which is why every letter in the game started shining,
	# menus included. The threshold is the whole safety mechanism here and
	# glow_bloom bypasses it.
	env.glow_bloom = 0.0
	env.glow_blend_mode = Environment.GLOW_BLEND_MODE_ADDITIVE
	_map_environment = env
	_world_env = WorldEnvironment.new()
	_world_env.name = "WorldEnvironment"
	add_child(_world_env)
	# A WorldEnvironment applies to the whole viewport no matter where it sits in
	# the tree, so parenting it to MapView does not scope it to the map. It has
	# to be switched off by hand whenever the map is not being shown, or the
	# main menu and every other screen get the map's post-processing.
	#
	# Under the host that viewport is now the world's own (ADR-004), which scopes
	# the environment to exactly what it was always meant to cover. The switching
	# stays: "the whole viewport" is still the rule, and MapView still has to work
	# parented to an ordinary canvas, which is how headless_probe.gd drives it.
	_sync_world_environment()

func _ensure_field_particles() -> void:
	if _field_particles != null and is_instance_valid(_field_particles):
		return
	_field_particles = Node2D.new()
	_field_particles.name = "FieldParticles"
	_field_particles.set_script(FIELD_PARTICLES)
	# Ordered by the seating pass into the field layer's tall band, not by z.
	_field_particles.z_index = Z_TILES
	add_child(_field_particles)
	_field_particles.setup()

func _ensure_anim_overlay() -> void:
	if _anim_overlay != null and is_instance_valid(_anim_overlay):
		return
	_anim_overlay = Node2D.new()
	_anim_overlay.name = "AnimOverlay"
	_anim_overlay.set_script(ANIM_OVERLAY)
	# Over every tile, under every panel. Combat text and explosions belong on
	# top of the world but must not survive a menu opening over them.
	_anim_overlay.z_index = Z_ANIM
	add_child(_anim_overlay)
	_anim_overlay.setup(_host)

func refresh() -> void:
	if _host == null:
		return
	if not _host.has_method("tileset_ready") or not _host.tileset_ready():
		return
	if not _atlases_loaded:
		_load_atlases()
	if not _atlases_loaded:
		return
	# The overlay has its own generation counter: animations advance many times
	# within a single turn, so it must be polled even when the tiles are unchanged.
	if _anim_overlay != null and is_instance_valid(_anim_overlay):
		_anim_overlay.refresh(_view_origin, _tile_size)

	var generation: int = -1
	if _host.has_method("get_map_generation"):
		generation = _host.get_map_generation()
	var area := get_viewport_rect().size
	if generation >= 0 and generation == _batched_generation and area == _batched_area:
		return

	_tile_size = _host.get_tileset_tile_size()
	if _host.has_method("get_map_view_origin"):
		_view_origin = _host.get_map_view_origin()
	_view_size = _host.get_map_view_size()
	_cmds = _host.get_map_draw_list()
	# Guarded rather than assumed: an older library has no glyph fallback, and a
	# missing method there should cost the glyphs, not the whole map.
	if _host.has_method("get_map_glyph_list"):
		_glyphs = _host.get_map_glyph_list()
	if _field_particles != null and is_instance_valid(_field_particles) \
			and _host.has_method("get_map_field_list"):
		var wind := Vector2.ZERO
		if _host.has_method("get_wind_vector"):
			wind = _host.get_wind_vector()
		_field_particles.refresh(_host.get_map_field_list(), _tile_size, wind)
		if _host.has_method("get_conditions"):
			var pc: Dictionary = _host.get_conditions()
			_field_particles.set_conditions(float(pc.get("daylight", 1.0)),
				float(pc.get("precipitation", 0.0)), float(pc.get("pain", 0.0)))
	_update_zoom_and_camera()
	_rebuild_batches()
	_rebuild_glyph_layers()
	_update_light_texture()
	_update_light_uniforms()
	_sync_world_environment()
	_batched_generation = generation
	_batched_area = area
	queue_redraw()

## Step the manual zoom one notch: +1 in, -1 out.
##
## Driven from outside rather than from an `_unhandled_input` here, because the
## world now renders inside a SubViewport that deliberately accepts no input
## (see `world_viewport.gd`) -- so no event reaches this node any more. The host
## owns the Ctrl+wheel binding and calls this.
func zoom_step(direction: int) -> void:
	if direction == 0:
		return
	_user_zoom = clampf(_user_zoom * (1.1 if direction > 0 else 1.0 / 1.1), 0.5, 4.0)
	_update_zoom_and_camera()
	queue_redraw()

func _load_atlases() -> void:
	_atlases.clear()
	var count: int = _host.get_tileset_atlas_count()
	if count <= 0:
		return
	for i in count:
		var img: Image = _host.get_tileset_atlas_image(i)
		if img == null or img.get_width() <= 0:
			push_warning("MapView: empty atlas %d" % i)
			_atlases.append(null)
			continue
		_atlases.append(ImageTexture.create_from_image(img))
	_atlases_loaded = _atlases.size() > 0
	# Palette ramps travel with the tileset, so they load with the atlases.
	_palette_tex = null
	if _host.has_method("get_palette_image"):
		var pal: Image = _host.get_palette_image()
		if pal != null and pal.get_width() > 0:
			_palette_tex = ImageTexture.create_from_image(pal)
			print("MapView: %d palette ramps" % (pal.get_height() - 1))
	# Atlas identity changed, so any batch still holding the old texture is stale.
	_clear_batches()
	_batched_generation = -1
	if _atlases_loaded:
		print("MapView: loaded ", _atlases.size(), " atlases tile=", _host.get_tileset_tile_size())

## Tiles the viewport covers at the current zoom, which is what we ask C++ to
## publish. Rounded up and given a one-tile margin so the edges are covered
## rather than half-drawn, and made odd so the player sits on the centre tile.
func _tiles_for_viewport(area: Vector2) -> Vector2i:
	var tw := float(_tile_size.x) * _zoom
	var th := float(_tile_size.y) * _zoom
	if tw < 1.0 or th < 1.0:
		return Vector2i.ZERO
	var w := int(ceil(area.x / tw)) + 2
	var h := int(ceil(area.y / th)) + 2
	if w % 2 == 0:
		w += 1
	if h % 2 == 0:
		h += 1
	return Vector2i(w, h)

## Zoom is the player's choice alone.
##
## It used to be fit-to-view, derived from the published tile count -- so the map
## was scaled to whatever the curses grid happened to hand over and then centred,
## which is where the black bars either side came from. Now the zoom is fixed and
## the tile count follows it, so the draw list always covers the whole viewport.
##
## "The whole viewport" is the drawable area exactly, with no sidebar to subtract
## from it: the world has its own SubViewport and that viewport is sized to what
## the sidebar leaves (ADR-004, `world_viewport.gd`). MapView used to carry a
## `_reserved_right` for this and no longer needs to know the sidebar exists.
func _update_zoom_and_camera() -> void:
	var area := get_viewport_rect().size
	if area.x < 2.0 or area.y < 2.0:
		return
	_zoom = _user_zoom
	scale = Vector2(_zoom, _zoom)

	var want := _tiles_for_viewport(area)
	if want != _requested_tiles and want.x > 0:
		_requested_tiles = want
		if _host != null and _host.has_method("set_map_view_tiles"):
			_host.set_map_view_tiles(want.x, want.y)

	if _view_size.x <= 0 or _view_size.y <= 0:
		return
	# C++ centres the player in the published block, so centre the block here and
	# the player lands in the middle of the viewport.
	var map_w := float(_view_size.x * _tile_size.x) * _zoom
	var map_h := float(_view_size.y * _tile_size.y) * _zoom
	position = Vector2((area.x - map_w) * 0.5, (area.y - map_h) * 0.5)
	# The light uniforms are in world space, so they move with this transform.
	# The mouse-wheel zoom path comes through here without a refresh().
	_update_light_uniforms()

## Upload the per-tile light/visibility texture when C++ rebuilds it (SP-3, SP-4).
##
## The first successful upload is also the handshake: from then on C++ stops
## folding light into the per-sprite tints and this shader owns brightness. Doing
## it in that order means a host that never gets here -- older scripts, a
## headless run, a driver that refuses the shader -- still sees a lit map.
func _update_light_texture() -> void:
	if _host == null or not _host.has_method("get_light_generation"):
		return
	var generation: int = _host.get_light_generation()
	if generation == _light_generation:
		return
	var img: Image = _host.get_light_image()
	if img == null or img.get_width() <= 0:
		return
	_light_generation = generation
	# Recreating the texture every turn would drop the sampler state with it;
	# update() reuses the same texture when the size is unchanged.
	if _light_tex != null and _light_tex.get_size() == Vector2(img.get_size()):
		_light_tex.update(img)
	else:
		_light_tex = ImageTexture.create_from_image(img)
	if not _light_pass_announced and _host.has_method("set_light_pass_enabled"):
		_host.set_light_pass_enabled(true)
		_light_pass_announced = true

## Push the light texture and the world-space rect it covers to every batch.
func _update_light_uniforms() -> void:
	if _batches.is_empty():
		return
	var enabled := _light_tex != null and _view_size.x > 0 and _view_size.y > 0
	var origin := Vector2.ZERO
	var inv_size := Vector2.ZERO
	if enabled:
		var gx := get_global_transform()
		origin = gx.origin
		var world := gx.basis_xform(
			Vector2(float(_view_size.x * _tile_size.x), float(_view_size.y * _tile_size.y)))
		if is_zero_approx(world.x) or is_zero_approx(world.y):
			enabled = false
		else:
			inv_size = Vector2(1.0 / world.x, 1.0 / world.y)
			# The light image is one block of rows per z-level now (ADR-006 item 3D-4),
			# deepest last. This backend draws every level at one height and wants the
			# avatar's, which is block 0 -- so V has to be scaled into that block rather
			# than stretched over the whole texture, or a hole coming into view would
			# quietly squash the lighting of everything.
			var levels := 1
			if _host != null and _host.has_method("get_light_levels"):
				levels = maxi(1, int(_host.get_light_levels()))
			inv_size.y /= float(levels)
	var wind := Vector2.ZERO
	if _host != null and _host.has_method("get_wind_vector"):
		wind = _host.get_wind_vector()
	# Time of day, weather and pain. Read once and pushed to every batch rather
	# than sampled per material, since they are the same for the whole world.
	var daylight := 1.0
	var precipitation := 0.0
	var pain := 0.0
	if _host != null and _host.has_method("get_conditions"):
		var c: Dictionary = _host.get_conditions()
		daylight = float(c.get("daylight", 1.0))
		precipitation = float(c.get("precipitation", 0.0))
		pain = float(c.get("pain", 0.0))
	for key in _batches:
		var node: MultiMeshInstance2D = _batches[key]
		if not is_instance_valid(node) or node.material == null:
			continue
		var mat: ShaderMaterial = node.material
		mat.set_shader_parameter("wind", wind)
		mat.set_shader_parameter("daylight", daylight)
		mat.set_shader_parameter("precipitation", precipitation)
		mat.set_shader_parameter("pain", pain)
		mat.set_shader_parameter("light_enabled", enabled)
		mat.set_shader_parameter("light_origin", origin)
		mat.set_shader_parameter("light_inv_size", inv_size)
		mat.set_shader_parameter("vis_tex", _light_tex)
		mat.set_shader_parameter("light_tex", _light_tex)

func _ensure_quad() -> ArrayMesh:
	if _quad != null:
		return _quad
	var vertices := PackedVector2Array([
		Vector2(0.0, 0.0), Vector2(1.0, 0.0), Vector2(1.0, 1.0), Vector2(0.0, 1.0),
	])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_TEX_UV] = vertices
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2, 0, 2, 3])
	_quad = ArrayMesh.new()
	# Be explicit about 2D vertices rather than relying on Godot inferring it from
	# the PackedVector2Array.
	_quad.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {},
		Mesh.ARRAY_FLAG_USE_2D_VERTICES)
	return _quad

func _clear_batches() -> void:
	for node in _batches.values():
		if is_instance_valid(node):
			node.queue_free()
	_batches.clear()
	for node in _glyph_layers.values():
		if is_instance_valid(node):
			node.queue_free()
	_glyph_layers.clear()

## Hand each layer's fallback glyphs to its own node.
##
## Split by layer rather than drawn in one pass because a glyph has to keep the
## z-order its sprite would have had: a floor with no art must still sit under
## the monster standing on it.
func _rebuild_glyph_layers() -> void:
	var buckets: Dictionary = {}
	var n := _glyphs.size()
	var i := 0
	while i + GLYPH_STRIDE - 1 < n:
		var layer: int = _glyphs[i + 2]
		var bucket: PackedInt32Array = buckets.get(layer, PackedInt32Array())
		bucket.append_array(_glyphs.slice(i, i + GLYPH_STRIDE))
		buckets[layer] = bucket
		i += GLYPH_STRIDE

	# Layers that had glyphs last frame and have none now must be emptied, or the
	# stale ones keep drawing.
	for layer in _glyph_layers:
		if not buckets.has(layer):
			var idle = _glyph_layers[layer]
			if is_instance_valid(idle):
				idle.set_commands(PackedInt32Array(), _tile_size)

	for layer in buckets:
		_glyph_layer_for(layer).set_commands(buckets[layer], _tile_size)

func _glyph_layer_for(layer: int) -> Node2D:
	var existing = _glyph_layers.get(layer)
	if existing != null and is_instance_valid(existing):
		return existing
	var node := Node2D.new()
	node.name = "GlyphLayer_%d" % layer
	node.set_script(GLYPH_LAYER)
	# Ordered by the seating pass, not by z (see Z_TILES): it lands in its own
	# layer's flat band, under every layer above and over every layer below.
	node.z_index = Z_TILES
	add_child(node)
	_glyph_layers[layer] = node
	return node

## One batch per (layer, atlas, sway, palette).
##
## Sway and palette are shader uniforms rather than per-instance data because
## INSTANCE_CUSTOM is already full of the atlas sub-rect and the instance colour
## is the lighting tint. Splitting the batch instead costs one extra draw call
## per distinct combination, and only foliage and declared palette variants use
## anything but the default -- so in practice it is one or two.
func _batch_for(layer: int, atlas_i: int, sway: bool, palette: int,
		tall: bool, row: int, z_below: int) -> MultiMeshInstance2D:
	var key := "%d:%d:%d:%d:%d:%d:%d" % [layer, atlas_i, 1 if sway else 0, palette,
		1 if tall else 0, row, z_below]
	var existing = _batches.get(key)
	if existing != null and is_instance_valid(existing):
		return existing

	var tex: Texture2D = _atlases[atlas_i]
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_2D
	mm.use_custom_data = true
	# Per-instance colour carries the lighting tint the C++ side computed from
	# lit_level; the shader multiplies it into the sampled texel.
	mm.use_colors = true
	mm.mesh = _ensure_quad()

	var mat := ShaderMaterial.new()
	mat.shader = TILE_SHADER
	mat.set_shader_parameter("atlas_texel",
		Vector2(1.0 / float(tex.get_width()), 1.0 / float(tex.get_height())))
	# The avatar is the viewpoint. Dimming it to match the floor it stands on
	# makes it hard to find on a dark screen, so its layer sits out the pass.
	#
	# A level below the avatar sits it out for an unrelated reason: the light
	# texture holds one texel per column and that texel belongs to the tile
	# the avatar can see, so applying it to what is underneath would light a
	# basement floor with the daylight falling down the hole above it. C++
	# sends those commands with the CPU lighting baked into the tint instead
	# (see fog_for_depth in src/godot_map_snapshot.cpp).
	mat.set_shader_parameter("receives_light",
		not PLAYER_LAYERS.has(layer) and z_below == 0)

	mat.set_shader_parameter("sway_enabled", sway)
	mat.set_shader_parameter("palette_row", palette)
	if _palette_tex != null:
		mat.set_shader_parameter("palette_tex", _palette_tex)
		mat.set_shader_parameter("palette_rows", float(_palette_tex.get_height()))

	var node := MultiMeshInstance2D.new()
	# Every field of the batch key belongs in the name, tall included: without it
	# a layer's flat and tall batches collide and Godot quietly renames one, which
	# makes the child order unreadable in a debugger at exactly the moment you are
	# trying to read it.
	node.name = "TileBatch_%d_%d_%d_%d_%d_%d_%d" % [layer, atlas_i, 1 if sway else 0,
		palette, 1 if tall else 0, row, z_below]
	node.multimesh = mm
	node.texture = tex
	node.material = mat
	# The C++ draw list is layer-sorted; the seating pass reproduces that ordering
	# as child order, with a separate band for sprites that overhang their cell.
	node.z_index = Z_TILES
	add_child(node)
	_batches[key] = node
	return node

## Place a sprite of w x h at (x, y), rotated about its own centre. SDL rotates
## about the destination rect's centre, so the origin has to compensate rather than
## being the top-left corner.
##
## Static, and named without the underscore, because `map_view_3d.gd` builds its
## Transform3D out of this. Of everything the two backends have in common this is
## the piece least worth having two copies of: the flip case took two attempts to
## get right, and a second copy would be a second chance to get it wrong.
static func tile_transform(x: float, y: float, w: float, h: float, rotation: int,
		flip_x: bool = false) -> Transform2D:
	# Mirroring is a negative x basis about the sprite's centre, which is how a
	# character faces left. SDL spells the same thing as render_copy_ex's
	# rota = -1; a quarter-turn field cannot express it, so it rides in a flag.
	if flip_x:
		var basis := Vector2(-w, 0.0)
		var ang: float = ROTATION_ANGLES[rotation & 3]
		if rotation != 0:
			basis = Vector2(-cos(ang), -sin(ang)) * w
		var bx := basis
		var by := Vector2(0.0, h) if rotation == 0 else Vector2(-sin(ang), cos(ang)) * h
		var centre := Vector2(x + w * 0.5, y + h * 0.5)
		return Transform2D(bx, by, centre - (bx + by) * 0.5)
	if rotation == 0:
		return Transform2D(Vector2(w, 0.0), Vector2(0.0, h), Vector2(x, y))
	var ang: float = ROTATION_ANGLES[rotation & 3]
	var c := cos(ang)
	var s := sin(ang)
	var basis_x := Vector2(c, s) * w
	var basis_y := Vector2(-s, c) * h
	var centre := Vector2(x + w * 0.5, y + h * 0.5)
	return Transform2D(basis_x, basis_y, centre - (basis_x + basis_y) * 0.5)

## Unpack map_draw_cmd::tint (0xRRGGBBAA, delivered as a signed int32).
func _unpack_tint(packed: int) -> Color:
	return Color8(
		(packed >> 24) & 0xFF,
		(packed >> 16) & 0xFF,
		(packed >> 8) & 0xFF,
		packed & 0xFF
	)

## The row a command's sprite stands on, for the command at offset `i`.
##
## dest_y is the sprite's top edge *after* its offset has been applied, so it is
## not the row: a 32x64 tree at row 10 has the same dest_y as a 32x32 rock at row
## 9. The bottom edge is the row, and it is already derivable from what the
## command carries -- which is why depth ordering needed no new field on
## map_draw_cmd and no change to the stride.
##
## The same anchor the contact shadows use, for the same reason: bottom centre is
## the one point on a sprite that says which tile it is standing on.
func _row_of(i: int) -> int:
	var th: int = maxi(1, _tile_size.y)
	return int(floor(float(_cmds[i + 6] + _cmds[i + 4]) / float(th)))

func _rebuild_batches() -> void:
	# Bucket the draw list by everything that has to be uniform across a batch:
	# the atlas and the two shader uniforms, plus the depth the batch sits at,
	# because a MultiMesh draws as one unit and cannot interleave with another.
	var buckets: Dictionary = {}
	var n := _cmds.size()
	var i := 0
	while i + CMD_STRIDE - 1 < n:
		var atlas_i: int = _cmds[i]
		if atlas_i >= 0 and atlas_i < _atlases.size() and _atlases[atlas_i] != null:
			var flags: int = _cmds[i + 9]
			var layer: int = _cmds[i + 7]
			var tall := (flags & FLAG_TALL) != 0 or CREATURE_LAYERS.has(layer)
			# A batch may not span depths, or its instances cannot interleave
			# with another batch's -- so the row goes in the key. Only for the
			# standing content: bucketing the flat sprites per row too would
			# multiply the draw calls by the height of the view for an ordering
			# that provably cannot matter (see depth_rank).
			#
			# Cost: one draw call per (row, atlas, sway, palette, layer) that
			# holds standing content, so the bound is the view height times the
			# atlases in use -- a few hundred in a dense forest, against six for
			# the whole map before. Fine for 2D, but if it ever bites, drop the
			# layer from the tall key: a MultiMesh draws its instances in array
			# order and the C++ list is layer-major within a row, so a row's
			# layers stay ordered inside one batch. That trades roughly a third
			# of the calls for having to carry the layer in _creature_slots,
			# which the shadow pass currently reads off the key.
			# The level goes in the key for two reasons at once: it is part of
			# the depth -- a batch may not span two levels any more than it
			# may span two rows -- and it decides receives_light, which is a
			# shader uniform and therefore per batch.
			var key := "%d:%d:%d:%d:%d:%d:%d" % [layer, atlas_i,
				1 if (flags & FLAG_SWAY) != 0 else 0,
				(flags & PALETTE_MASK) >> PALETTE_SHIFT,
				1 if tall else 0,
				_row_of(i) if tall else 0,
				(flags & Z_BELOW_MASK) >> Z_BELOW_SHIFT]
			if not buckets.has(key):
				buckets[key] = PackedInt32Array()
			var bucket: PackedInt32Array = buckets[key]
			bucket.append(i)
			buckets[key] = bucket
		i += CMD_STRIDE

	for key in _batches:
		if not buckets.has(key):
			var idle: MultiMeshInstance2D = _batches[key]
			if is_instance_valid(idle):
				idle.multimesh.instance_count = 0

	# Batches at the same rank draw in tree order, so a coat on human_body.png and
	# a rifle on tall.png -- same layer, same row, different atlas -- would have
	# no defined order between them. Sort the keys by the first command each
	# holds, so "later in the draw list" breaks that tie the way the game meant
	# it. The C++ list is layer-then-row sorted, which within one rank is exactly
	# the order the overlays were emitted in.
	var ordered_keys: Array = buckets.keys()
	ordered_keys.sort_custom(func(a, b): return (buckets[a] as PackedInt32Array)[0] \
		< (buckets[b] as PackedInt32Array)[0])

	for key in ordered_keys:
		var parts := (key as String).split(":")
		var layer := int(parts[0])
		var atlas_i := int(parts[1])
		var node := _batch_for(layer, atlas_i, int(parts[2]) != 0, int(parts[3]),
			int(parts[4]) != 0, int(parts[5]), int(parts[6]))
		var tex: Texture2D = _atlases[atlas_i]
		var inv_w := 1.0 / float(tex.get_width())
		var inv_h := 1.0 / float(tex.get_height())
		var offsets: PackedInt32Array = buckets[key]
		var mm := node.multimesh
		mm.instance_count = offsets.size()
		# Creature layers keep a record of what each slot is and where it started,
		# so a hit reaction can move that one instance and put it back (SP-5).
		var creatures: Array = []
		var is_creature := CREATURE_LAYERS.has(layer)
		# NOTE: set_instance_* is one native call per instance. If profiling shows
		# that dominating, pack MultiMesh.buffer directly instead (8 transform
		# floats + 4 custom-data floats per instance, in that order).
		for slot in offsets.size():
			var o: int = offsets[slot]
			var src_w: int = _cmds[o + 3]
			var src_h: int = _cmds[o + 4]
			var xf := tile_transform(
				float(_cmds[o + 5]), float(_cmds[o + 6]),
				float(src_w), float(src_h), _cmds[o + 9] & ROTATION_MASK,
				(_cmds[o + 9] & FLAG_FLIP_X) != 0)
			mm.set_instance_transform_2d(slot, xf)
			mm.set_instance_custom_data(slot, Color(
				float(_cmds[o + 1]) * inv_w,
				float(_cmds[o + 2]) * inv_h,
				float(src_w) * inv_w,
				float(src_h) * inv_h
			))
			mm.set_instance_color(slot, _unpack_tint(_cmds[o + 8]))
			if is_creature:
				# The transform we just wrote, not one read back: a MultiMesh
				# read-back goes through the rendering driver, and the dummy
				# driver a headless run uses returns identity for all of them.
				creatures.append({
					"slot": slot,
					# Bottom centre of the sprite, not its origin. Sprite offsets
					# exist so that a 32x48 creature's feet land on the tile, so
					# the base is the one point that reliably says which tile a
					# creature is standing on -- dest_y alone does not.
					"anchor": xf * Vector2(0.5, 1.0),
					"xform": xf,
					"color": _unpack_tint(_cmds[o + 8]),
				})
		if is_creature:
			_creature_slots[key] = creatures
		else:
			_creature_slots.erase(key)
	# Shadows come straight from the creature records: an anchor and a width are
	# all a blob needs, so nothing extra has to be published for them.
	if _shadow_layer != null and is_instance_valid(_shadow_layer):
		var blobs: Array = []
		for key in _creature_slots:
			# Bodies only. A character is one body plus a dozen clothing
			# overlays, each its own instance at the same anchor, so taking
			# every creature instance stacked twelve shadows on one pair of
			# feet -- 36 blobs for what should have been three. The overlay
			# layers are the odd ones: monster 7 / monster_overlay 8,
			# player 9 / player_overlay 10.
			var parts := (key as String).split(":")
			var layer := int(parts[0])
			if layer != MONSTER_LAYER_BODY and layer != PLAYER_LAYER_BODY:
				continue
			# One shadow layer draws the whole map, so it can only sit at one
			# depth, and that depth is the avatar's level. A blob for a creature
			# two floors down would be painted on the floor the player is
			# standing on, beside the hole rather than at the bottom of it.
			if int(parts[6]) != 0:
				continue
			for entry in _creature_slots[key]:
				var xf: Transform2D = entry["xform"]
				blobs.append({
					"anchor": entry["anchor"],
					# The quad's x basis is the sprite's width in pixels.
					"width": xf.x.length(),
				})
		var strength := 1.0
		if _host != null and _host.has_method("get_conditions"):
			strength = float((_host.get_conditions() as Dictionary).get("daylight", 1.0))
		_shadow_layer.set_shadows(blobs, _tile_size, strength)

	# Now that every batch exists, put the map in draw order. Child order is the
	# only thing ordering it (see Z_TILES), so this pass has to seat the shadow
	# and glyph layers as well -- while z_index still carried the layers, those
	# two sorted themselves and being left at the end of the child list cost
	# nothing. They are the parts that break quietly if this is ever narrowed
	# back to batches only.
	var seats: Array = []
	for key in ordered_keys:
		var node = _batches.get(key)
		if node != null and is_instance_valid(node):
			var parts := (key as String).split(":")
			seats.append({
				"node": node,
				"rank": depth_rank(int(parts[0]), int(parts[4]) != 0, int(parts[5]),
					int(parts[6])),
				"tie": (buckets[key] as PackedInt32Array)[0],
			})
	for glyph_layer in _glyph_layers:
		var gnode = _glyph_layers[glyph_layer]
		if gnode != null and is_instance_valid(gnode):
			# Last within its layer's flat band. A glyph is only ever emitted for
			# a tile that produced no sprite, so nothing in the band can hide it.
			#
			# Not depth-sorted: one node holds a whole layer's glyphs, so it can
			# only sit at one rank. Glyphs are the missing-art path and a tileset
			# with full coverage emits none, so the approximation is not worth a
			# node per row to remove.
			#
			# Same approximation across z: a glyph for a tile two floors down is
			# seated on the avatar's level. The command list carries the level
			# and the glyph list does not (map_glyph_cmd has no flags field), so
			# fixing it means either a node per level or a wider glyph command,
			# for art that is missing in the first place. C++ dims those glyphs
			# with the same depth fog it dims the sprites with, which is what
			# keeps a basement question mark from reading as a nearby one.
			seats.append({
				"node": gnode,
				"rank": depth_rank(int(glyph_layer), false, 0),
				"tie": 0x7FFFFFFF,
			})
	# The two whole-map effect layers go in the gap between the flat content and
	# the standing content. Neither can be depth-sorted -- each is a single node
	# drawing the whole map -- so this is the one position that is right more
	# often than not: over the ground, under anything standing on it.
	if _shadow_layer != null and is_instance_valid(_shadow_layer):
		# A shadow is on the floor, so being under every tree is correct.
		seats.append({"node": _shadow_layer,
			"rank": TOP_LEVEL_BASE + TALL_BAND - 2, "tie": 0})
	if _field_particles != null and is_instance_valid(_field_particles):
		# Smoke drifts over the fire it comes from but does not hide the zombie
		# walking through it.
		seats.append({"node": _field_particles,
			"rank": TOP_LEVEL_BASE + TALL_BAND - 1, "tie": 0})
	seats.sort_custom(func(a, b):
		return a["rank"] < b["rank"] if a["rank"] != b["rank"] else a["tie"] < b["tie"])
	var seat := 0
	for entry in seats:
		move_child(entry["node"], seat)
		seat += 1

	# A hit that was mid-flight when the map changed now refers to instances that
	# have been renumbered, so re-seat it against the new slots.
	if not _hits.is_empty():
		_apply_hit_reactions()

## Poll for new hits and advance the ones already running (SP-5).
##
## This runs per frame rather than per turn: the map is only rebuilt when the
## game changes, but a recoil has to move between those rebuilds or it is not an
## animation. Nothing happens at all while no hit is active, which is nearly
## always.
func _process(delta: float) -> void:
	if not visible or _host == null:
		return
	_poll_hits()
	if _hits.is_empty():
		return
	var still: Array = []
	for hit in _hits:
		hit["t"] += delta
		if hit["t"] < HIT_DURATION:
			still.append(hit)
	var ended := still.size() != _hits.size()
	_hits = still
	# One last pass when the final hit expires, to put the instances back.
	if not _hits.is_empty() or ended:
		_apply_hit_reactions()

func _poll_hits() -> void:
	if not _host.has_method("get_hit_generation"):
		return
	var generation: int = _host.get_hit_generation()
	if generation <= _hit_seen:
		return
	var events: PackedInt32Array = _host.get_hit_events()
	var n := events.size()
	var i := 0
	while i + HIT_STRIDE - 1 < n:
		var id: int = events[i]
		if id > _hit_seen:
			_hits.append({
				"tile": Vector2i(events[i + 1], events[i + 2]),
				"dir": Vector2(float(events[i + 4]), float(events[i + 5])),
				"flash": _unpack_tint(events[i + 6]),
				"t": 0.0,
			})
		i += HIT_STRIDE
	_hit_seen = generation

## The reaction curve: out fast, back slower, zero at both ends.
func _hit_curve(t: float) -> float:
	var u := clampf(t / HIT_DURATION, 0.0, 1.0)
	return sin(u * PI) * (1.0 - u * 0.35)

## Displace and flash the creature instances a hit landed on.
##
## Every creature instance is rewritten, not just the struck ones, because the
## ones that were struck a frame ago have to be put back. There are tens of
## creatures on screen at worst, so this is cheaper than tracking which slots
## are dirty.
func _apply_hit_reactions() -> void:
	for key in _creature_slots:
		var node = _batches.get(key)
		if node == null or not is_instance_valid(node):
			continue
		var mm: MultiMesh = node.multimesh
		for entry in _creature_slots[key]:
			var slot: int = entry["slot"]
			if slot >= mm.instance_count:
				continue
			var response := hit_response(entry["anchor"])
			var offset: Vector2 = response["offset"]
			var xform: Transform2D = entry["xform"]
			if offset != Vector2.ZERO:
				xform = xform.translated(offset)
			mm.set_instance_transform_2d(slot, xform)
			var base: Color = entry["color"]
			var flash: float = response["flash"]
			mm.set_instance_color(slot,
				base.lerp(response["color"], flash * 0.75) if flash > 0.0 else base)

## Displacement and flash currently applying to a sprite whose base sits at
## @a anchor -- the bottom centre of its quad, which is the point that says
## which tile a creature is standing on whatever its sprite offset.
##
## Split out of _apply_hit_reactions so the reaction can be inspected without
## reading a transform back out of the MultiMesh, which goes through the
## rendering driver and returns identity under the dummy one.
func hit_response(anchor: Vector2) -> Dictionary:
	var offset := Vector2.ZERO
	var flash := 0.0
	var flash_color := Color.WHITE
	for hit in _hits:
		var tile: Vector2i = hit["tile"] - _view_origin
		var foot := Vector2((float(tile.x) + 0.5) * float(_tile_size.x),
			float(tile.y + 1) * float(_tile_size.y))
		if anchor.distance_squared_to(foot) > float(_tile_size.x * _tile_size.x):
			continue
		var amount := _hit_curve(hit["t"])
		var dir: Vector2 = hit["dir"]
		# A hit with no direction still has to read as a hit: shove the sprite
		# down, the way a flinch looks from above.
		if dir == Vector2.ZERO:
			dir = Vector2(0.0, 1.0)
		offset += dir.normalized() * amount * HIT_OFFSET \
			* float(maxi(_tile_size.x, _tile_size.y))
		if amount > flash:
			flash = amount
			flash_color = hit["flash"]
	return { "offset": offset, "flash": flash, "color": flash_color }

func _draw() -> void:
	var area := get_viewport_rect().size
	# Background in local space covers the map footprint (parent may be offset).
	if _view_size.x > 0 and _view_size.y > 0:
		draw_rect(Rect2(Vector2.ZERO, Vector2(_view_size.x * _tile_size.x, _view_size.y * _tile_size.y)),
			Color(0.02, 0.02, 0.04))
	elif area.x > 2.0:
		draw_rect(Rect2(Vector2.ZERO, area), Color(0.02, 0.02, 0.04))
