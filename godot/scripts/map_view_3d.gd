extends Node3D
## The 3D draw backend (ADR-006 item 3D-1), flat.
##
## Same draw list, same sprites, same fixed top-down view -- drawn as quads in a 3D
## scene under an orthographic camera instead of as canvas items. Nothing about the
## simulation, the snapshot or the tileset changes; this is the "swap the draw
## layer" that ADR-002 and ADR-003 both said would stay possible.
##
## **This is ADR-006's option A, the flat world.** Every quad stays parallel to the
## image plane, so the projection is exactly the one the artist drew for and the
## third axis carries depth only. The milestone is therefore a negative one: a
## screenshot must be *indistinguishable* from the 2D backend's. Everything after
## this is a deliberate departure from that baseline, which is the only way to tell
## an improvement from a bug.
##
## What it already buys, with the camera still pointing straight down:
##
##   - a place to put real lights, fog and volumetrics (3D-2, 3D-6);
##   - depth-buffer sorting, once sprites can go in the opaque pass (3D-1b), which
##     is what collapses `map_view.gd`'s per-row batching -- a few hundred draw
##     calls in a forest -- back to one batch per atlas;
##   - z-levels as elevation rather than as a tint (3D-4).
##
## What it does **not** buy, and why the tilt experiment (3D-3) is the decision
## point: every normal in a flat world points at the camera, so a light to the left
## of a wall shades it exactly as a light above it does. Until the world stands up,
## 3D lighting can only reproduce what CDDA's light texture already says.
##
## ## The four layers that are not tiles (3D-1c)
##
## A viewport composites its canvas *over* its 3D scene, so none of these could
## simply come along from the 2D backend. One decision each, and they went two ways:
##
##   - **Contact shadows are geometry**, a MultiMesh of blob quads in the world. On
##     the canvas they would have landed on top of the creatures casting them, which
##     reads as a bug rather than as a missing feature. In the world they are
##     depth-tested, which makes them better than the 2D pass by accident: a tree
##     between the camera and a blob now hides it, and a blob can sit on the floor of
##     a level below the avatar's -- which the 2D backend has to skip, having one
##     node and therefore one depth to spend.
##   - **Fire and smoke are `GPUParticles3D`**, in `field_particles_3d.gd`, a port
##     kept diffable against the 2D original.
##   - **The fallback glyphs and the animation overlay stay canvas items**, on a
##     `CanvasLayer` inside this viewport whose transform reproduces the camera. Font
##     and vector drawing gains nothing from being meshes. The overlay belongs over
##     the world anyway; the glyphs lose their per-layer ordering, which only matters
##     for tiles whose art is missing in the first place.
##
## The canvas transform is exact for an orthographic, unrotated camera and **wrong
## the moment the world tilts** (3D-3). At that point those two need
## `Camera3D.unproject_position` per point, or a home in the world proper.

## The 2D backend, for the two pieces of it that are worth having exactly one copy
## of: the sprite placement (rotation and the mirror case) and the depth ranking.
## Everything else here is genuinely different, node for node.
const MV := preload("res://scripts/map_view.gd")
const TILE_SHADER_3D := preload("res://shaders/map_tiles_3d.gdshader")
## The mesh library and its shader (3D-8d): furniture ids with a real model
## draw that instead of their sprite. See terrain_meshes.gd for the convention.
const TERRAIN_MESHES := preload("res://scripts/terrain_meshes.gd")
const MESH_SHADER_3D := preload("res://shaders/mesh_tiles_3d.gdshader")
## The 2D contact shadows, for their constants. The blob's proportions and the lift
## that puts it under a character's feet were tuned by eye; a second set of numbers
## would be a second thing to tune.
const SHADOW := preload("res://scripts/shadow_layer.gd")
const GLYPH_LAYER := preload("res://scripts/glyph_layer.gd")
const ANIM_OVERLAY := preload("res://scripts/anim_overlay.gd")
const FIELD_PARTICLES_3D := preload("res://scripts/field_particles_3d.gd")
## Rain, snow and acid falling through the scene (3D-6).
const WEATHER_PARTICLES_3D := preload("res://scripts/weather_particles_3d.gd")
## For its layer names only, so the first-frame report can spell out what the draw list
## contained without a second copy of the map_layer order.
const DEBUG_OVERLAY := preload("res://scripts/debug_overlay.gd")
## Creatures drawn as meshes rather than as sprites (3D-7c). Does nothing until a mesh
## exists for an id, which is the point: the migration is partial by design.
const CREATURE_MESHES := preload("res://scripts/creature_meshes.gd")

## World units between one sprite's depth and the next.
##
## The rank `MV.depth_rank` computes spans about a million values, and mapping that
## range onto z directly would ask float32 to tell 0.001 apart at a magnitude of a
## thousand. So rank decides the *order* and the order decides z: the distinct ranks
## in a frame are sorted and handed consecutive steps. The ordering is provably the
## same as the 2D backend's and the numbers stay small.
const Z_STEP := 0.25

## Alpha below this is discarded rather than blended (3D-1b).
##
## This is what puts the sprites in the opaque pass, and it is the whole mechanism
## behind the batch count: with per-pixel depth, two sprites interleave correctly
## whatever atlas they came from, so the row and the layer leave the batch key.
##
## It is also a visible change, and the only one this backend makes deliberately.
## Ultica's sprites are mostly hard-edged, but where one has a soft or a genuinely
## translucent edge -- glass, a window -- that edge now steps instead of fading.
## Judge it against a screenshot of the *blended* 3D backend rather than against the
## canvas, or two differences arrive at once.
##
## Lowering this does not undo it: writing the threshold at all is what makes the
## material a cutout, so a threshold of zero makes half-transparent pixels opaque
## instead of blending them. `map_tiles_3d.gdshader` says what to edit to get
## blending back, and names the third option (`depth_prepass_alpha`) if the hard
## edges rather than the sorting turn out to be the problem.
const ALPHA_SCISSOR := 0.5
## Where the camera sits on the depth axis, and how far it can see.
##
## Only has to clear the depth span a frame produces -- Z_STEP times the number of
## distinct depth ranks, so a few hundred units at worst. It used to be 4096 for no
## reason, and the reason to care is that volumetric fog fills a fixed length of the
## frustum from the camera: with the world a thousand units past the fog volume, a
## light inside it lights nothing.
const CAM_Z := 512.0
const CAM_NEAR := 1.0
const CAM_FAR := 1024.0

## Floats per published light source; see LightSnapshot::light_stride.
## x, y, radius, r, g, b, luminance, bearing, cone.
const LIGHT_STRIDE := 9
## Lights built at most. A burning building publishes a source per burning tile, and
## past a point the extra lights cost frames without changing what anyone sees --
## the same bound, for the same reason, as the field emitters.
const MAX_LIGHTS := 32
## CDDA luminance to Godot light energy. A guess, and the first thing to put in front
## of someone with a running game (VER-1): the game's scale runs from about 4 for a
## candle to 50-odd for a bonfire, and what those should *look* like is not a number
## the simulation has an opinion about.
const LIGHT_ENERGY_SCALE := 1.0 / 40.0

## How hard the sun shines at noon. A guess (VER-1); the direction is not one any more.
const SUN_ENERGY := 0.8

## How far a beam noses down toward the road, in degrees (VER-1).
##
## The channel gives a beam its bearing and nothing else, and a level beam from
## half a tile up lights nothing: the ground faces up, so a horizontal cone
## grazes it at ndl ~ 0. Fifteen degrees puts the pool's near edge about a tile
## and a half ahead of the lamp, which reads as a headlight; the game publishes
## no elevation for the cone, so this is the renderer's number, marked as such.
const BEAM_PITCH_DEGREES := 15.0

## Draw the shadow proxies, and draw them *instead of* the creature sprites (3D-7b).
##
## **Off, and ugly on purpose.** This is the cheapest possible answer to the question that
## has to be answered before anyone models anything: does a body, at this scale, in this
## world, sit correctly? A capsule hidden behind a sprite answers nothing, so turning this on
## hides the creature billboards and leaves the geometry standing in their place -- the first
## time this renderer draws a creature as a thing rather than as a picture of one.
##
## What to look at: whether the capsule's feet are where the sprite's were, whether its
## height reads as a person against the walls around it, and -- since it is lit and the
## sprites are not -- which way the sun appears to be coming from.
const SHOW_SHADOW_PROXIES := false

## How wide a creature's shadow proxy is, as a fraction of its sprite's width, and how
## much of the sprite's height it fills.
##
## A person is narrower than the cell they stand in and does not reach its top corner, so
## the capsule is inset on both axes. Guesses, and cheap to judge: stand next to a campfire
## at night and see whether the shadow is a person or a barrel (VER-1).
const PROXY_WIDTH_SCALE := 0.45
const PROXY_HEIGHT_SCALE := 0.92

## How far each level below the avatar sits beneath it, in tiles of height (3D-4).
##
## Applies only while tilted: coplanar levels are the flat world's baseline, and the 2D
## backend's, and both must stay where they are. Above zero and stood up, a level below
## is a floor lower down rather than a dimmed tint at the same height -- which is what
## makes a hole a hole, and what lets the light and the shadows above it fall into it.
##
## Two tiles because that is the convention the art already uses: Ultica draws a wall as
## a 64 px sprite in a 32 px cell, so one storey is two tiles of height. ADR-005 found
## that the tileset declares no height of its own -- `zlevel_height` is 0 and `height_3d`
## is on no tile -- so this is our number, and taking it from what the art does is the
## least invented answer available.
##
## The projection makes this exactly checkable, which `geometry_check.tscn` does: a level
## one down lands `LEVEL_DROP_TILES * tile_height` pixels further down the screen and
## moves in no other way. Whether that *reads* as a hole is a different question, and it
## needs someone standing at the top of a staircase -- the same person the backlog has
## been waiting for since ADR-005 item 1.
const LEVEL_DROP_TILES := 2.0

## Fill the world with fog the lights can glow through.
##
## On by default now, because the two things that made it a lie and a smear are both
## fixed. The lie: a light pool is bounded by CDDA's rays but a fog volume is bounded
## by shadow casters, and in a flat world every sprite was a camera-facing plane a
## lamp could glow straight through. The stood-up world made walls walls, and 3D-8
## made them boxes -- a shadow caster with actual depth. (Tilt-gated still, for
## exactly that reason: the flat world keeps the honest no-fog baseline.) The smear:
## the first density was read as "dims the whole frame" (VER-1), and it did -- see
## FOG_DENSITY.
##
## The density lives in a FogVolume box fitted over the map, not in the Environment's
## global density: the telephoto camera (PERSPECTIVE) stands kilometres back, and
## global fog fills the whole approach -- the map would be seen through
## e^-(density * distance) of it, which at any density worth having is not seen at
## all. The box also keeps the sky above the roofline clear instead of hazing the
## letterbox.
const VOLUMETRIC_FOG := true
## Fog density per world unit -- and a world unit is an artist's *pixel*, not a
## metre, which is what the first guess (0.02) got wrong: a 45-degree ray crosses
## about 210 units of fogged air on its way to the ground, so 0.02 meant
## e^-4.2 = 98% of every EMISSION-lit sprite eaten -- "dims the whole frame", as
## VER-1 put it. 0.0015 keeps extinction near 25%: enough for lamps and headlights
## to carve visible cones, faint enough that a moonless field still reads.
const FOG_DENSITY := 0.0015

## How far the camera is tilted off looking straight along the depth axis, in degrees
## (ADR-006 item 3D-3 -- the tilt experiment, and the decision point the whole ADR was
## written around).
##
## **0.0 is the flat world and the shipping behaviour.** Above zero the world stands
## up: ground sprites become horizontal quads, standing sprites become vertical ones,
## and the geometry is pre-stretched so the tilt cancels out and the artist's pixels
## come back unchanged -- `tile/sin` along the rows, `height/cos` upward. See
## `_place`, and `debug_projected_rect`, which is how that claim is checked rather
## than asserted.
##
## 45 because that is where the arithmetic above puts a wall at two tiles of height, which
## is what the art draws, and because the first person to look at it said the angle reads
## correctly. Anything in the 45-55 band is defensible; this is one constant.
##
## **On its own the tilt changes almost nothing you can see** -- the pre-stretch is precisely
## a guarantee of that. What it buys is real normals and real occluders, which is why engine
## light and cast shadows are switched on by it and off without it.
##
## Nothing is hidden by it any more. The fallback glyphs and the animation overlay were,
## for a day, on the assumption that their canvas could not follow a rotated camera -- and
## it does not have to, because everything on that canvas annotates the ground and a ground
## point's screen position is unchanged by the tilt by construction. `geometry_check.tscn`
## holds the canvas against the camera at six tilts to keep that true (3D-1d).
const TILT_DEGREES := 45.0

## Whether the stood-up world is looked at with perspective rather than orthographic
## projection (3D-9), and the vertical field of view when it is.
##
## Telephoto on purpose: the camera stands back far enough that this FOV spans
## exactly the extent the orthographic camera showed, so the centre of the frame
## keeps its scale -- one world unit is one pixel there, as ever -- and the
## projection arrives only as parallax: walls near the top of the frame show a
## little more of their tops, walls near the bottom a little more of their fronts.
## The narrower the angle, the further back the camera and the gentler the effect;
## at zero it would *be* the orthographic camera.
##
## What the telephoto deliberately trades away: the 2D canvas (glyphs, the animation
## overlay) maps map pixels to the screen affinely, which under perspective is only
## exact at the centre of the frame. Content drifts off its tile toward the edges --
## proportionally to FOV -- which is tolerable because everything interactive there
## hugs the centre (combat text rings the avatar, look mode recentres on its cursor)
## and fallback glyphs are rare under a full tileset. The geometry gate still asserts
## exact round-trips by forcing this off; a canvas that cannot drift needs its content
## homed in the world proper, which is where the animation overlay is headed anyway.
const PERSPECTIVE := true
const PERSPECTIVE_FOV_DEGREES := 12.0

## Depth separation between one rank and the next while tilted, along the camera's
## own axis.
##
## Along the *view* axis specifically, which under an orthographic projection is the
## one direction a sprite can be moved in without moving on screen at all. So the rank
## ordering survives the tilt, costs no pixels, and cannot fight the geometric depth
## it now sits inside: two rows are a whole tile apart along the ground, and every rank
## in a frame put together is a few units.
const TILT_DEPTH_STEP := 0.02

## Glide the camera on the avatar's own tween instead of snapping with the turn.
##
## The simulation moves a whole tile at a time and the published block recentres
## with it, so the world content jumps a tile per step while the avatar's mesh
## glides -- which reads exactly as wrong as it sounds, and was reported so the
## first time anyone walked with it. The camera (and the world canvas, which must
## stay glued to the ground it annotates) rides `avatar_visual_offset()`: while
## the avatar's body is still en route to where the game says it stands, the view
## trails it by the same amount, and both arrive together. Costs nothing when
## nothing is tweening, and a fixture with no avatar mesh gets a zero offset.
const SMOOTH_CAMERA := true

## The dark the map is drawn against, matching the rect `map_view.gd` paints under
## its tiles and the SessionBg behind it. An opaque background rather than a
## transparent one: the colour is the same either way, and this way there is one
## fewer thing that can differ between the two backends.
const BG_COLOR := Color(0.02, 0.02, 0.04)

var _host: Node
var _atlases: Array[Texture2D] = []
## The same atlases as CPU-side images, kept because the box pass reads pixels the
## GPU never hands back: `_painted_rect` measures where a sprite's paint actually is.
var _atlas_images: Array[Image] = []
## "atlas:x:y:w:h" -> Rect2i of the sprite's opaque pixels, sprite-local. Lazy, one
## scan per distinct sub-rect ever seen; cleared with the atlases it was read from.
var _painted: Dictionary = {}
var _cmds: PackedInt32Array = PackedInt32Array()
var _tile_size: Vector2i = Vector2i(32, 32)
var _view_size: Vector2i = Vector2i.ZERO
var _view_origin: Vector2i = Vector2i.ZERO
var _atlases_loaded: bool = false
var _zoom: float = 1.0
var _user_zoom: float = 1.0
var _requested_tiles: Vector2i = Vector2i.ZERO

## Unit quad in the XY plane, (0,0)..(1,1) with matching UVs -- the same quad the
## 2D backend uses, given a third coordinate of zero.
var _quad: ArrayMesh
## The quad grown depth for standing terrain (3D-8): front face identical to
## `_quad`, sides and top sampling the sprite's edge pixels. Unit-deep, like the
## quad is unit-square: each instance's transform carries its own depth in the
## basis z column, which is what lets a wall be a tile deep and a chair only as
## deep as the chair (see `_rebuild_batches`), all in one batch of one mesh.
var _box: ArrayMesh
## "atlas:sway:palette:lit" -> MultiMeshInstance3D. One entry per distinct set of
## shader uniforms, which after 3D-1b is all a batch has to be uniform in.
var _batches: Dictionary = {}
## depth rank -> z, compacted per frame; see Z_STEP and `_rank_of`.
var _depths: Dictionary = {}
## How far the compacted depths reach, for the batch bounds.
var _depth_span: float = 0.0
var _batched_generation: int = -1
var _batched_area: Vector2 = Vector2.ZERO

## Tilt in radians, and its sine and cosine, recomputed with the camera. Cached
## because every sprite placed in a frame divides by them.
## The tilt actually in force, in degrees. `TILT_DEGREES` is its default and not its
## value: reading the constant inside `_update_camera` made the tilt impossible to drive
## from outside, which silently reduced the geometry gate to testing the flat path
## thirty times. It is also what lets someone try an angle on a running game.
var _tilt_degrees: float = TILT_DEGREES
var _tilt: float = 0.0
var _sin_tilt: float = 0.0
var _cos_tilt: float = 1.0
## Whether the world is stood up. Not `_tilt > 0.0`: the flat path is a different
## construction rather than the limit of this one -- a horizontal ground quad at zero
## tilt would have to be infinitely deep to project to anything.
var _tilted: bool = false

var _camera: Camera3D
## Where _update_camera put the camera, before the smooth-follow offset -- the
## per-frame glide must compose with the per-turn placement, not fight it.
## INF until the camera has been placed once, so a frame that ticks before any
## refresh cannot park the camera at a zero it was never given.
var _camera_base: Vector3 = Vector3.INF
var _canvas_base_origin: Vector2 = Vector2.INF
var _world_env: WorldEnvironment
## The box of fogged air over the map; see VOLUMETRIC_FOG for why a volume and not
## the Environment's global density.
var _fog_volume: FogVolume
## Contact shadows (ADR-005 item 4): one MultiMesh of blob quads.
var _shadow_batch: MultiMeshInstance3D
var _shadow_material: StandardMaterial3D
## Shadow proxies: one invisible capsule per creature, casting the shadow its billboard
## cannot. See _ensure_shadow_proxy.
var _shadow_proxy: MultiMeshInstance3D
## Creatures that have a mesh (3D-7c). Empty until art exists.
var _creature_meshes: Node3D
## Fire and smoke (SP-6), as GPUParticles3D.
var _field_particles: Node3D
## Real lights, one OmniLight3D per source the game publishes (ADR-006 item 3D-2).
var _light_root: Node3D
## The sun, when the world is stood up. Aimed from the game's own
## `sun_azimuth_altitude()` -- see `_refresh_sun`.
var _sun: DirectionalLight3D
var _light_pool: Array[OmniLight3D] = []
## Beams, kept in their own pool: a headlight is a SpotLight3D and a lamp is not, and a
## node cannot change its mind about which it is.
var _beam_pool: Array[SpotLight3D] = []
var _beams_used: int = 0
## How many of the pool are in use this frame, and how many the game published, so
## the diagnostics can say "capped at MAX_LIGHTS" rather than just a number.
var _lights_used: int = 0
var _lights_published: int = 0
## map_layer -> commands this frame that named an atlas we could not draw from.
var _skipped: Dictionary = {}
## Creature-layer commands this frame left undrawn because their creature is drawn
## as a mesh (3D-7c). Counted so "left to a mesh" and "lost" stay distinguishable:
## a suppressed body takes its overlays with it, which from outside looks exactly
## like the draw list losing content -- and was reported as that.
var _mesh_suppressed: int = 0
## The interned id table from get_map_ident_table (3D-8d), re-copied only when a
## command names an index past its end -- the table is append-only in C++.
var _ident_table: PackedStringArray = PackedStringArray()
## id -> MultiMeshInstance3D for the mesh library's batches; one per furniture
## id on screen, reused across rebuilds like the sprite batches.
var _ident_batches: Dictionary = {}
## Furniture commands drawn as library meshes this frame, for diagnostics --
## the same "left to a mesh" vs "lost" distinction _mesh_suppressed makes.
var _ident_routed: int = 0
## Runtime state of VOLUMETRIC_FOG, same pattern as the tilt: the const is the
## default, the var is what a running game (or a fixture) can drive.
var _volumetric_fog: bool = VOLUMETRIC_FOG
## Runtime state of PERSPECTIVE and its FOV, same pattern again -- and the geometry
## gate *depends* on being able to drive these: its round-trip claim is about the
## orthographic pre-stretch, so it forces perspective off before measuring.
var _perspective: bool = PERSPECTIVE
var _fov_degrees: float = PERSPECTIVE_FOV_DEGREES
## Weather as particles falling through the scene (3D-6): rain, snow, acid.
var _weather_particles: Node3D
## Everything about the world that is still drawn in 2D: the fallback glyphs and the
## animation overlay. A CanvasLayer rather than child nodes, because a Node3D cannot
## parent a canvas item usefully -- and because a viewport composites its canvas over
## its 3D scene, which is where these two belong anyway.
var _canvas: CanvasLayer
## map_layer -> glyph_layer.gd node, under _canvas.
var _glyph_layers: Dictionary = {}
var _glyphs: PackedInt32Array = PackedInt32Array()
var _anim_overlay: Node2D
var _light_tex: ImageTexture
## Level blocks in that texture; see the shader's `light_texture_levels`.
var _light_levels: int = 1
var _light_generation: int = -1
var _light_pass_announced: bool = false
var _palette_tex: ImageTexture

## Creature-layer instances by batch key, for hit reactions (SP-5): the same
## records the 2D backend keeps, with a Transform3D instead of a Transform2D.
var _creature_slots: Dictionary = {}
var _hits: Array = []
var _hit_seen: int = 0
## Whether the camera ever had to be re-made-current after setup; see _ensure_camera.
var _recovered_camera: bool = false
## One diagnostic line has been printed for this session.
var _reported: bool = false

func setup(host: Node) -> void:
	_host = host
	_atlases_loaded = false
	_atlases.clear()
	_cmds = PackedInt32Array()
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
	_glyphs = PackedInt32Array()
	_ensure_camera()
	_ensure_environment()
	_ensure_shadow_batch()
	_ensure_shadow_proxy()
	_ensure_creature_meshes()
	_ensure_field_particles()
	_ensure_weather_particles()
	_ensure_lights()
	_ensure_canvas()

## Orthographic and pointing straight down the depth axis, which is what makes the
## flat world pixel-faithful: an orthographic projection ignores z entirely, so
## using z for depth ordering cannot move a sprite by so much as a pixel.
func _ensure_camera() -> void:
	if _camera != null and is_instance_valid(_camera):
		return
	_camera = Camera3D.new()
	_camera.name = "MapCamera"
	_camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	# KEEP_HEIGHT is the default and is what `size` being a vertical extent
	# depends on; stated because the whole scale relies on it.
	_camera.keep_aspect = Camera3D.KEEP_HEIGHT
	_camera.near = CAM_NEAR
	_camera.far = CAM_FAR
	add_child(_camera)
	# Godot makes the first camera in a viewport current on its own, and relying on
	# that would be relying on there never being a second one.
	#
	# But asking here is not enough, and this is what a grey map turned out to be:
	# `make_current()` returns early unless the camera is inside a World3D, and a
	# Node3D subtree is only inside one while it is **visible**. The host creates the
	# world hidden and shows it when a session starts, so this call lands while the
	# camera is nowhere and does nothing but set a flag. A viewport whose 3D camera is
	# unset renders neither the scene nor the environment -- it clears to the
	# project's default clear colour, which is a flat mid-grey, and says nothing.
	#
	# So it is re-asserted from _update_camera, which runs on every refresh; see
	# _ensure_current.
	_camera.make_current()

## Make sure the viewport is actually looking through our camera.
##
## Cheap and idempotent, and called per refresh rather than once at setup because
## "once" happens while the world is hidden -- see _ensure_camera. Returns whether it
## had to intervene, so the diagnostic can say so.
##
## Asks the **viewport** what its camera is, not the camera whether it is current.
## Those are different questions in exactly the case this exists for: `make_current()`
## sets the camera's own `current` flag *before* the early return, so a camera that
## never reached the viewport still answers `is_current() == true`. Checking the flag
## would have been a fix that changed nothing and read as one that did.
func _ensure_current() -> bool:
	var vp := get_viewport()
	if vp == null or _camera == null or not is_instance_valid(_camera):
		return false
	if vp.get_camera_3d() == _camera:
		return false
	_camera.make_current()
	return true

## Contact shadows, as one MultiMesh of blob quads in the world rather than a canvas
## pass over it.
##
## This is the layer that could not simply come along from the 2D backend: a
## viewport composites its canvas over its 3D scene, so a canvas shadow would land
## on top of the creature casting it. In the world it is depth-tested instead, which
## also makes it better than the 2D version by accident -- a tree standing between
## the camera and a shadow now hides it, where the 2D pass drew the blob over the
## tree.
##
## Blended and depth-write-disabled: a shadow tints the floor rather than occluding
## it. The blob texture and its proportions are `shadow_layer.gd`'s, unchanged.
func _ensure_shadow_batch() -> void:
	if _shadow_batch != null and is_instance_valid(_shadow_batch):
		return
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_colors = true
	mm.mesh = _ensure_quad()

	_shadow_material = StandardMaterial3D.new()
	_shadow_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_shadow_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_shadow_material.blend_mode = BaseMaterial3D.BLEND_MODE_MIX
	_shadow_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	_shadow_material.depth_draw_mode = BaseMaterial3D.DEPTH_DRAW_DISABLED
	_shadow_material.albedo_texture = _blob_texture()
	# The blob is white; the instance colour carries black and the strength.
	_shadow_material.vertex_color_use_as_albedo = true

	_shadow_batch = MultiMeshInstance3D.new()
	_shadow_batch.name = "ContactShadows"
	_shadow_batch.multimesh = mm
	_shadow_batch.material_override = _shadow_material
	_shadow_batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_shadow_batch)

## A soft round blob, built rather than shipped, exactly as `shadow_layer.gd` builds
## its own. Duplicated because that one is a `_`-private of a Node2D script and this
## needs the texture rather than the node.
func _blob_texture() -> Texture2D:
	var g := Gradient.new()
	g.offsets = PackedFloat32Array([0.0, 0.55, 1.0])
	g.colors = PackedColorArray([
		Color(1, 1, 1, 1), Color(1, 1, 1, 0.7), Color(1, 1, 1, 0),
	])
	var tex := GradientTexture2D.new()
	tex.gradient = g
	tex.fill = GradientTexture2D.FILL_RADIAL
	tex.fill_from = Vector2(0.5, 0.5)
	tex.fill_to = Vector2(1.0, 0.5)
	tex.width = 32
	tex.height = 32
	return tex

## An invisible capsule per creature, casting the shadow a billboard cannot.
##
## The sprites already cast their own silhouette -- alpha-scissored in the opaque pass, so
## the shadow is the artwork's outline -- and that is right for a tree and wrong for a
## person. A billboard's silhouette never changes, so a figure lit from the side still casts
## a front view of itself: the shadow says nothing about where the light is, which is the
## one thing a shadow is for.
##
## So creatures stop casting and a capsule casts for them. `SHADOWS_ONLY` means it is never
## drawn, only occludes; the contact blob stays, because a blob works when nothing is
## casting at all and the two cues are answering different questions -- "it is standing
## here" and "the light is over there".
##
## **This is also the first mesh.** The intended direction is real meshes for creatures
## (see the amendment at the end of ADR-006), and an invisible capsule is exactly the node
## a modelled body will replace: same place in the tree, same per-creature transform, same
## shadow setting. What changes later is the mesh resource and whether it is visible.
func _ensure_shadow_proxy() -> void:
	if _shadow_proxy != null and is_instance_valid(_shadow_proxy):
		return
	# A unit capsule: radius 0.5 and total height 2, so it spans -1..1 in y and -0.5..0.5
	# across. Every instance is that shape scaled, which keeps the per-creature arithmetic
	# to one multiply per axis. Coarse on purpose -- nothing ever looks at it, only the
	# shadow map does.
	var capsule := CapsuleMesh.new()
	capsule.radius = 0.5
	capsule.height = 2.0
	capsule.radial_segments = 6
	capsule.rings = 2

	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = capsule

	_shadow_proxy = MultiMeshInstance3D.new()
	_shadow_proxy.name = "ShadowProxies"
	_shadow_proxy.multimesh = mm
	if SHOW_SHADOW_PROXIES:
		# Lit, per-pixel, and deliberately plain: the point of looking at it is the shape
		# and the shading, so anything decorative would be in the way. Being lit while the
		# sprites are not is itself informative -- it is the first thing in this renderer
		# that shows where the sun is rather than where an artist decided it was.
		var mat := StandardMaterial3D.new()
		mat.albedo_color = Color(0.72, 0.70, 0.66)
		_shadow_proxy.material_override = mat
		_shadow_proxy.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	else:
		_shadow_proxy.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_SHADOWS_ONLY
	add_child(_shadow_proxy)

## Where a creature's shadow proxy goes: standing on @p anchor, as wide and as tall as the
## sprite it stands in for.
##
## The world is anisotropic -- height is pre-stretched by 1/cos and rows by 1/sin -- so a
## capsule placed here is not a round capsule, and that is correct: it has to match the
## *apparent* size of the sprite it replaces, which is what the stretch is for.
func proxy_transform(anchor: Vector3, width: float, height: float) -> Transform3D:
	var r := width * PROXY_WIDTH_SCALE * 0.5
	var half := height * PROXY_HEIGHT_SCALE * 0.5
	return Transform3D(
		Vector3(r * 2.0, 0.0, 0.0), Vector3(0.0, half, 0.0), Vector3(0.0, 0.0, r * 2.0),
		Vector3(anchor.x, anchor.y + half, anchor.z))

func _ensure_creature_meshes() -> void:
	if _creature_meshes != null and is_instance_valid(_creature_meshes):
		return
	_creature_meshes = Node3D.new()
	_creature_meshes.name = "CreatureMeshes"
	_creature_meshes.set_script(CREATURE_MESHES)
	add_child(_creature_meshes)
	_creature_meshes.setup(_host)

## A creature's feet, in view-relative pixels, as a world position (3D-7c).
##
## Handed to the mesh layer as a Callable rather than reimplemented there, because these are
## the placement rules and they belong to whoever owns the camera. It is the same mapping the
## tiles use: pixels across, rows into depth through the pre-stretch, levels below dropped by
## their floor height.
func feet_to_world(px: float, py: float, z_below: int) -> Vector3:
	if not _tilted:
		return Vector3(px, -py, _depths.get(_shadow_rank(z_below), 0.0))
	return Vector3(px, _level_y(z_below), py / maxf(_sin_tilt, 0.0001))

## Ask the mesh layer to place whatever it has art for, and take back the list of tiles whose
## sprites it is standing in for.
func _refresh_creature_meshes() -> void:
	if _creature_meshes == null or not is_instance_valid(_creature_meshes):
		return
	if _host == null or not _host.has_method("get_creatures"):
		return
	# Heights are pre-stretched in the stood-up world, so a mesh has to be stretched with
	# them or it will be the only thing in the scene that is not.
	var height_scale := 1.0 / maxf(_cos_tilt, 0.0001) if _tilted else 1.0
	_creature_meshes.set_tilted(_tilted, _sin_tilt)
	# The origin in map pixels is what lets the mesh layer tell a creature that
	# moved from a view that recentred -- movement is detected in world space or
	# every stationary creature takes a little walk whenever the avatar does.
	_creature_meshes.refresh(_host.get_creatures(), _tile_size, feet_to_world, height_scale,
		Vector2(_view_origin) * Vector2(_tile_size))

func _ensure_field_particles() -> void:
	if _field_particles != null and is_instance_valid(_field_particles):
		return
	_field_particles = Node3D.new()
	_field_particles.name = "FieldParticles"
	_field_particles.set_script(FIELD_PARTICLES_3D)
	add_child(_field_particles)
	_field_particles.setup()

func _ensure_lights() -> void:
	if _light_root != null and is_instance_valid(_light_root):
		return
	_light_root = Node3D.new()
	_light_root.name = "Lights"
	add_child(_light_root)
	_sun = DirectionalLight3D.new()
	_sun.name = "Sun"
	_sun.visible = false
	_sun.shadow_bias = 0.05
	_sun.shadow_normal_bias = 2.0
	_light_root.add_child(_sun)

## Aim the sun, and decide whether it shines at all.
##
## Off in the flat world, and that is the rule for every engine light here: with every
## normal facing the camera, a directional light adds a uniform term to the whole frame,
## which would move the flat backend off the baseline it exists to match. The stood-up
## world is where a direction means something, so it is where the lights are on.
##
## The direction is the game's now, not a constant: `sun_azimuth_altitude()` has always
## known it and `calendar.cpp` already built the vector. Shadows move with the time of
## day for the price of two floats on a channel that was already being published.
func _refresh_sun() -> void:
	if _sun == null or not is_instance_valid(_sun):
		return
	var daylight := 1.0
	var azimuth := 135.0
	var altitude := -90.0
	if _host != null and _host.has_method("get_conditions"):
		var c: Dictionary = _host.get_conditions()
		daylight = clampf(float(c.get("daylight", 1.0)), 0.0, 1.0)
		azimuth = float(c.get("sun_azimuth", azimuth))
		altitude = float(c.get("sun_altitude", altitude))
	# Below the horizon is no sun, said by the game rather than inferred from how much
	# light it is giving. The two are not the same thing under an overcast noon.
	_sun.visible = _tilted and altitude > 0.5 and daylight > 0.02
	if not _sun.visible:
		return
	_sun.light_energy = SUN_ENERGY * daylight
	_sun.shadow_enabled = true
	# Where the sun actually is. `sun_azimuth_altitude()` gives compass degrees with 0 at
	# north, and a Node3D's y rotation turns the same way, so the bearing goes in as it
	# comes; the altitude is a pitch downward from the horizontal.
	_sun.rotation = Vector3(-deg_to_rad(altitude), deg_to_rad(azimuth), 0.0)

## Put an OmniLight3D where the game says light is coming from.
##
## The light *texture* stays the authority on how lit a tile is -- CDDA casts rays and
## respects occluders, and ADR-003 already refused to let the renderer re-derive that.
## These say where the light comes from, which is what a per-tile value cannot express
## and what a 3D scene needs to have lights at all.
##
## **Nothing lit by them is visible yet**, because the tile shader is `unshaded`: in a
## flat world every normal points at the camera, so a light could only multiply the
## whole frame by a constant. They are here, positioned and counted, because the shader
## going lit is the next step and this is what it consumes. `VOLUMETRIC_FOG` is the one
## switch that makes them show up before then, and the comment on it says why it is off.
##
## Shadows are off for the same reason: a camera-facing plane casting a shadow is not a
## wall casting a shadow, and pretending otherwise would look like a bug. Standing the
## world up is what earns them.
func _refresh_lights() -> void:
	if _light_root == null or not is_instance_valid(_light_root):
		return
	if _host == null or not _host.has_method("get_light_sources"):
		return
	var src: PackedFloat32Array = _host.get_light_sources()
	var n := src.size()
	_lights_published = int(n / LIGHT_STRIDE)
	var i := 0
	var used := 0
	var beams := 0
	while i + LIGHT_STRIDE - 1 < n and used + beams < MAX_LIGHTS:
		# A cone means a beam. `apply_light_arc`'s wideness is the *full* width of it --
		# it halves the angle itself -- and Godot's spot_angle is the half, so this is one
		# of the few places where taking the game's number at face value would be wrong.
		var cone: float = src[i + 8]
		if cone > 0.0:
			var beam := _beam_at(beams)
			beam.position = _light_position(src[i], src[i + 1])
			beam.spot_range = maxf(1.0, src[i + 2])
			beam.spot_angle = clampf(cone * 0.5, 2.0, 89.0)
			beam.light_color = Color(src[i + 3], src[i + 4], src[i + 5])
			beam.light_energy = clampf(src[i + 6] * LIGHT_ENERGY_SCALE, 0.15, 6.0)
			# Compass bearing, 0 at north, and a SpotLight3D shines along its own -Z:
			# rotating by -bearing about Y is what turns one into the other. Pitched
			# down as well, because a dead-level beam half a tile up never meets the
			# ground -- the up-facing floor sees it at a grazing angle and lights not
			# at all, which is what the first headlight screenshot showed: two beams
			# built, nothing lit. Godot's YXZ Euler order makes this yaw-then-pitch,
			# which is exactly how a headlight is mounted.
			beam.rotation = Vector3(-deg_to_rad(BEAM_PITCH_DEGREES),
				-deg_to_rad(src[i + 7]), 0.0)
			beam.shadow_enabled = _tilted
			beam.visible = _tilted
			beams += 1
			i += LIGHT_STRIDE
			continue
		var light := _light_at(used)
		# x and y arrive in the same view-relative pixels the draw commands use, so
		# the y flip is the one every placement here makes. The depth is the field
		# layer's: a light belongs in the world, not on the floor under it.
		light.position = _light_position(src[i], src[i + 1])
		light.omni_range = maxf(1.0, src[i + 2])
		light.light_color = Color(src[i + 3], src[i + 4], src[i + 5])
		light.light_energy = clampf(src[i + 6] * LIGHT_ENERGY_SCALE, 0.15, 4.0)
		# A light only casts where the world has shape. Flat, every sprite is a plane
		# facing the camera and its shadow would be a rectangle laid over the floor.
		light.shadow_enabled = _tilted
		# See _refresh_sun: engine light is for the world that has shape.
		light.visible = _tilted
		used += 1
		i += LIGHT_STRIDE
	# Spare lights are hidden rather than freed: sources come and go every turn as
	# fires spread and lamps are switched off, and rebuilding a light is more work
	# than leaving one dark.
	for j in range(used, _light_pool.size()):
		_light_pool[j].visible = false
	for j in range(beams, _beam_pool.size()):
		_beam_pool[j].visible = false
	_lights_used = used
	_beams_used = beams

## Where a published light goes, in whichever world. Half a tile up when the world has
## one, so a lamp lights the facades around it rather than only the floor it stands on.
func _light_position(x: float, y: float) -> Vector3:
	if _tilted:
		return Vector3(x, float(_tile_size.y) * 0.5, y / maxf(_sin_tilt, 0.0001))
	return Vector3(x, -y, _depths.get(_field_rank(), 0.0))

func _beam_at(index: int) -> SpotLight3D:
	while _beam_pool.size() <= index:
		var beam := SpotLight3D.new()
		beam.name = "Beam_%d" % _beam_pool.size()
		beam.shadow_enabled = false
		beam.visible = false
		beam.shadow_bias = 0.1
		beam.shadow_normal_bias = 2.0
		_light_root.add_child(beam)
		_beam_pool.append(beam)
	return _beam_pool[index]

func _light_at(index: int) -> OmniLight3D:
	while _light_pool.size() <= index:
		var light := OmniLight3D.new()
		light.name = "Light_%d" % _light_pool.size()
		light.shadow_enabled = false
		# Billboards self-shadow badly at grazing angles, and every sprite here is a
		# billboard. Biased away from itself rather than tuned later.
		light.shadow_bias = 0.1
		light.shadow_normal_bias = 2.0
		light.visible = false
		_light_root.add_child(light)
		_light_pool.append(light)
	return _light_pool[index]

## The world's 2D remainder: fallback glyphs and the animation overlay.
##
## Both are font and vector drawing over the world, and both stay canvas items --
## ported they would need a font atlas and a mesh per line, for no gain. A
## CanvasLayer inside the world's viewport is where they can be: over the 3D scene,
## because a viewport composites its canvas last, and still unable to reach the UI,
## because the viewport boundary is the UI's protection (ADR-004).
##
## The transform reproduces the camera's projection, so both keep drawing in
## map-local pixels exactly as they do under the 2D backend. That works because the
## camera is orthographic and unrotated; **the tilt experiment (3D-3) breaks it**,
## and at that point these two need `Camera3D.unproject_position` per point or a home
## in the world proper.
func _ensure_canvas() -> void:
	if _canvas != null and is_instance_valid(_canvas):
		return
	_canvas = CanvasLayer.new()
	_canvas.name = "WorldCanvas"
	# Above the viewport's default canvas, which nothing else in here uses.
	_canvas.layer = 1
	add_child(_canvas)
	_anim_overlay = Node2D.new()
	_anim_overlay.name = "AnimOverlay"
	_anim_overlay.set_script(ANIM_OVERLAY)
	_canvas.add_child(_anim_overlay)
	_anim_overlay.setup(_host)

func _ensure_environment() -> void:
	if _world_env != null and is_instance_valid(_world_env):
		return
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = BG_COLOR
	# No tonemapping: the shader hands over colours that are already graded, and a
	# tone curve would be a second grade nobody asked for.
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.glow_enabled = true
	env.glow_hdr_threshold = 1.0
	env.glow_intensity = 0.7
	# Zero for the same reason as in the 2D backend: glow_bloom is a constant lift
	# applied before the threshold, so any value above zero blooms the whole frame
	# and the threshold stops being a safety mechanism.
	env.glow_bloom = 0.0
	env.glow_blend_mode = Environment.GLOW_BLEND_MODE_ADDITIVE
	_world_env = WorldEnvironment.new()
	_world_env.name = "WorldEnvironment"
	_world_env.environment = env
	add_child(_world_env)
	_refresh_environment()
	# No visibility dance here, unlike the 2D backend's: this environment is inside
	# the world's own viewport, which is the thing ADR-004 bought.

## Apply the parts of the environment that depend on runtime state: today, the fog.
##
## Fog only while tilted, for the reason VOLUMETRIC_FOG documents -- flat, every
## light is invisible anyway and a fog volume is bounded by shadow casters the flat
## world does not have, so a lamp behind a wall glows through it. Stood up, walls
## cast, and the objection retires with the flat default.
##
## The volume is a fixed length of frustum measured from the camera, so the length
## has to reach the world's far edge -- which while tilted is `camera.far`, not the
## `CAM_Z + 2` that fit the flat world. Getting this wrong fogs the near half of
## the map and leaves lights in the far half shining into clear air.
func _refresh_environment() -> void:
	if _world_env == null or not is_instance_valid(_world_env) or _world_env.environment == null:
		return
	var env: Environment = _world_env.environment
	var want := _volumetric_fog and _tilted
	env.volumetric_fog_enabled = want
	if _fog_volume != null and is_instance_valid(_fog_volume):
		_fog_volume.visible = want
	if want:
		# All of the density is in the box below; the global term would fog the
		# telephoto's empty approach. See VOLUMETRIC_FOG.
		env.volumetric_fog_density = 0.0
		var reach := CAM_Z + CAM_NEAR * 2.0
		if _camera != null and is_instance_valid(_camera):
			reach = _camera.far
		env.volumetric_fog_length = reach
		_ensure_fog_volume()
		# Fitted to the map: its footprint plus a tile of skirt, from two levels
		# below the floor (a basement fire wants its haze too) up to four tiles of
		# painted height, which clears every roofline the art draws.
		var map_w := float(_view_size.x * _tile_size.x)
		var depth := float(_view_size.y * _tile_size.y) / maxf(_sin_tilt, 0.0001)
		var top := 4.0 * float(_tile_size.y) / maxf(_cos_tilt, 0.0001)
		var bottom := _level_y(2)
		var pad := float(_tile_size.y) * 2.0
		_fog_volume.size = Vector3(map_w + pad, top - bottom, depth + pad)
		_fog_volume.position = Vector3(map_w * 0.5, (top + bottom) * 0.5, depth * 0.5)

## Fill the world with light-catching fog, or drain it, at runtime. Same shape as
## `set_tilt_degrees`: the const is the default, this is the experiment's handle.
func set_volumetric_fog(on: bool) -> void:
	if on == _volumetric_fog:
		return
	_volumetric_fog = on
	_refresh_environment()

## The fogged air itself. Lazy, like the camera and the environment: built the first
## refresh that wants it, resized every refresh after.
func _ensure_fog_volume() -> void:
	if _fog_volume != null and is_instance_valid(_fog_volume):
		return
	_fog_volume = FogVolume.new()
	_fog_volume.name = "MapFog"
	# A box, said out loud: the default shape is an ellipsoid, which would thin the
	# fog toward the map's edges and corners for no reason anyone asked for.
	_fog_volume.shape = RenderingServer.FOG_VOLUME_SHAPE_BOX
	var mat := FogMaterial.new()
	mat.density = FOG_DENSITY
	_fog_volume.material = mat
	add_child(_fog_volume)

## Tune the density on a running game (VER-1); FOG_DENSITY documents the unit.
func set_fog_density(density: float) -> void:
	_ensure_fog_volume()
	var mat := _fog_volume.material as FogMaterial
	if mat != null:
		mat.density = clampf(density, 0.0, 1.0)

## Drive the projection on a running game, and let the geometry gate force the
## orthographic baseline it asserts about. No rebuild: the world's geometry is the
## same either way, only the camera looking at it moves.
func set_perspective(on: bool) -> void:
	if on == _perspective:
		return
	_perspective = on
	_update_camera()

func perspective_enabled() -> bool:
	return _perspective

func set_perspective_fov(degrees: float) -> void:
	var want := clampf(degrees, 2.0, 45.0)
	if is_equal_approx(want, _fov_degrees):
		return
	_fov_degrees = want
	_update_camera()

func refresh() -> void:
	if _host == null:
		return
	if not _host.has_method("tileset_ready") or not _host.tileset_ready():
		_report_idle("the tileset is not loaded")
		return
	if not _atlases_loaded:
		_load_atlases()
	if not _atlases_loaded:
		_report_idle("no atlas could be uploaded")
		return

	# The overlay has its own generation counter: animations advance many times
	# within a turn, so it is polled even when the tiles are unchanged.
	if _anim_overlay != null and is_instance_valid(_anim_overlay):
		_anim_overlay.refresh(_view_origin, _tile_size)

	var generation: int = -1
	if _host.has_method("get_map_generation"):
		generation = _host.get_map_generation()
	var area := get_viewport().get_visible_rect().size
	if generation >= 0 and generation == _batched_generation and area == _batched_area:
		return

	_tile_size = _host.get_tileset_tile_size()
	if _host.has_method("get_map_view_origin"):
		_view_origin = _host.get_map_view_origin()
	_view_size = _host.get_map_view_size()
	_cmds = _host.get_map_draw_list()
	if _host.has_method("get_map_glyph_list"):
		_glyphs = _host.get_map_glyph_list()
	_update_camera()
	# Before the tiles, because it decides which creature sprites they must leave out.
	_refresh_creature_meshes()
	_rebuild_batches()
	_rebuild_glyph_layers()
	_rebuild_shadows()
	_refresh_field_particles()
	_refresh_weather_particles()
	_refresh_lights()
	_refresh_sun()
	_update_light_texture()
	_update_uniforms()
	_batched_generation = generation
	_batched_area = area
	_report_once()

## Say why the world is drawing nothing, once, if it never gets as far as drawing.
##
## The two ways out of `refresh` before anything is built are both legitimate and both
## silent, and silence is what a broken backend looks like too. One line each, said
## once, so "waiting for the tileset" is never mistaken for "producing an empty frame".
func _report_idle(why: String) -> void:
	if _reported:
		return
	_reported = true
	print("[map3d] nothing drawn: ", why)

## One line, once per session, naming everything a blank 3D map could be.
##
## This backend cannot be judged without a GPU and there is none where it is written,
## so the first time it draws anything it says what it drew and what it drew it with.
## Two rounds went into a blank map whose cause was a script that did not compile --
## which this line cannot catch, because a script that does not compile never reaches
## it (`build-scripts/check-godot-scripts.sh` is that gate). What it does catch is the
## next layer down: geometry built but not rendered, a camera the viewport is not
## using, an environment that never attached, an atlas that never loaded.
func _report_once() -> void:
	if _reported:
		return
	_reported = true
	var vp := get_viewport()
	var stats := debug_stats()
	print("[map3d] first frame: %d batches, %d instances, %d depths, span %.1f" % [
		int(stats["batches"]), int(stats["instances"]), int(stats["depths"]),
		float(stats["depth_span"])])
	# "The viewport is using this camera", not "the camera thinks it is current" --
	# see _ensure_current for why those differ and which one a grey map answers.
	print("[map3d] viewport's camera is ours=%s (recovered=%s) size=%.0f at %s" % [
		str(_camera != null and vp.get_camera_3d() == _camera), str(_recovered_camera),
		float(stats["camera_size"]), str(stats["camera_position"])])
	var sub := vp as SubViewport
	print("[map3d] viewport %s, 2d space %s, 3d=%s own world=%s transparent=%s" % [
		str(sub.size) if sub != null else "(not a SubViewport)",
		str(vp.get_visible_rect().size),
		str(not vp.disable_3d), str(vp.own_world_3d), str(vp.transparent_bg)])
	print("[map3d] lights=%d + beams=%d of %d published (cap %d), sun=%s, fog=%s" % [
		_lights_used, _beams_used, _lights_published, MAX_LIGHTS,
		str(_sun != null and _sun.visible), str(_volumetric_fog and _tilted)])
	if _creature_meshes != null and is_instance_valid(_creature_meshes):
		var ms: Dictionary = _creature_meshes.debug_stats()
		print("[map3d] creature meshes: %d drawn, %d ids seen, %d with art (%d animated)" % [
			int(ms.get("drawn", 0)), int(ms.get("ids_seen", 0)),
			int(ms.get("ids_with_meshes", 0)), int(ms.get("animated_ids", 0))])
	if SHOW_SHADOW_PROXIES and _tilted:
		print("[map3d] SHOW_SHADOW_PROXIES: creature sprites are hidden and their capsules "
			+ "are drawn in their place (3D-7b). This is meant to look wrong.")
	print("[map3d] tilt=%.1f deg, engine light %s" % [_tilt_degrees,
		"on (the world has shape)" if _tilted else "off (flat: every normal faces the camera)"])
	# What the draw list actually contained, and what the game published to light it.
	# Both were being asked for by hand off the F3 overlay, which is not reachable on a
	# desktop that keeps F3 for itself -- and a number worth asking for twice is a number
	# that belongs in the log.
	if _host.has_method("get_render_stats"):
		var st: Dictionary = _host.get_render_stats()
		var by_layer = st.get("by_layer", PackedInt32Array())
		var parts: Array[String] = []
		for li in mini(DEBUG_OVERLAY.LAYER_NAMES.size(), by_layer.size()):
			if by_layer[li] > 0:
				parts.append("%s=%d" % [DEBUG_OVERLAY.LAYER_NAMES[li], by_layer[li]])
		print("[map3d] draw list by layer: ", " ".join(parts))
		print("[map3d] light sources from the game = %d, open columns = %d" % [
			int(st.get("lights", 0)), int(st.get("open_columns", 0))])
	if _mesh_suppressed > 0:
		# Said out loud because from outside it is indistinguishable from losing
		# content: the suppressed body takes every clothing overlay with it, and
		# "the avatar draws naked" was reported as a regression when it was this.
		print("[map3d] %d creature sprite command(s) left to their meshes (3D-7c), "
			% _mesh_suppressed + "overlays included -- a meshed creature wears nothing yet")
	if _skipped.is_empty():
		print("[map3d] every published command was drawn"
			+ ("" if _mesh_suppressed == 0 else " or meshed"))
	else:
		var lost: Array[String] = []
		for sl in _skipped:
			var name := str(DEBUG_OVERLAY.LAYER_NAMES[sl]) if sl < DEBUG_OVERLAY.LAYER_NAMES.size() \
				else str(sl)
			lost.append("%s=%d" % [name, int(_skipped[sl])])
		print("[map3d] DROPPED for a missing atlas: ", " ".join(lost))
		var loaded := 0
		for tex in _atlases:
			loaded += 1 if tex != null else 0
		print("[map3d] atlases: %d of %d uploaded" % [loaded, _atlases.size()])
	print("[map3d] environment=%s, atlases=%d, light pass=%s, scissor=%.2f" % [
		str(_world_env != null and _world_env.environment != null), _atlases.size(),
		str(_light_pass_announced), float(stats["scissor"])])

## Stand the world up, or lay it back down, at runtime (3D-3).
##
## Forces a rebuild rather than only moving the camera: the tilt is baked into every
## instance transform by `_place`, so a camera that tilts without the geometry following
## is a camera looking at the flat world from the wrong angle.
func set_tilt_degrees(degrees: float) -> void:
	var want := clampf(degrees, 0.0, 80.0)
	if is_equal_approx(want, _tilt_degrees):
		return
	_tilt_degrees = want
	_batched_generation = -1
	_update_camera()
	# The fog is gated on the tilt and sized by the camera, both of which just moved.
	_refresh_environment()

func tilt_degrees() -> float:
	return _tilt_degrees

func zoom_step(direction: int) -> void:
	if direction == 0:
		return
	_user_zoom = clampf(_user_zoom * (1.1 if direction > 0 else 1.0 / 1.1), 0.5, 4.0)
	_update_camera()

func _load_atlases() -> void:
	_atlases.clear()
	_atlas_images.clear()
	_painted.clear()
	var count: int = _host.get_tileset_atlas_count()
	if count <= 0:
		return
	for i in count:
		var img: Image = _host.get_tileset_atlas_image(i)
		if img == null or img.get_width() <= 0:
			push_warning("MapView3D: empty atlas %d" % i)
			_atlases.append(null)
			_atlas_images.append(null)
			continue
		_atlases.append(ImageTexture.create_from_image(img))
		_atlas_images.append(img)
	_atlases_loaded = _atlases.size() > 0
	_palette_tex = null
	if _host.has_method("get_palette_image"):
		var pal: Image = _host.get_palette_image()
		if pal != null and pal.get_width() > 0:
			_palette_tex = ImageTexture.create_from_image(pal)
	_clear_batches()
	_batched_generation = -1
	if _atlases_loaded:
		print("MapView3D: loaded ", _atlases.size(), " atlases tile=",
			_host.get_tileset_tile_size())

## Tiles the viewport covers at the current zoom, asked of C++ exactly as the 2D
## backend asks. One world unit is one tile pixel, so the arithmetic is the same.
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

## Frame the map with the camera rather than by moving the map.
##
## The 2D backend scales and offsets MapView itself; here the geometry stays at one
## world unit per tile pixel and the camera does both jobs -- `size` is the vertical
## extent it shows, so dividing the viewport height by the zoom shows more world at
## a smaller zoom. That is what keeps the light uniforms trivial (the map is at the
## origin) and what makes a perspective camera a one-line change later, which is
## how parallax arrives in 3D-4.
func _update_camera() -> void:
	var area := get_viewport().get_visible_rect().size
	if area.x < 2.0 or area.y < 2.0 or _camera == null or not is_instance_valid(_camera):
		return
	_recovered_camera = _ensure_current() or _recovered_camera
	_zoom = _user_zoom

	var want := _tiles_for_viewport(area)
	if want != _requested_tiles and want.x > 0:
		_requested_tiles = want
		if _host != null and _host.has_method("set_map_view_tiles"):
			_host.set_map_view_tiles(want.x, want.y)

	# The tilt, and its trigonometry, before anything that divides by it.
	_tilt = deg_to_rad(clampf(_tilt_degrees, 0.0, 80.0))
	_tilted = _tilt_degrees > 0.01
	_sin_tilt = sin(_tilt)
	_cos_tilt = cos(_tilt)
	# Unchanged by the tilt, and that is the point: the pre-stretch is chosen so that
	# one world unit still projects to one pixel along the camera's own axes.
	_camera.size = maxf(1.0, area.y / maxf(_zoom, 0.01))
	# C++ centres the player in the published block, so look at the middle of the
	# block and the player lands in the middle of the viewport.
	var map_w := float(_view_size.x * _tile_size.x)
	var map_h := float(_view_size.y * _tile_size.y)
	# The canvas is synced before the extent is checked, so its *scale* is right from
	# the first frame even when nothing has been published yet. Its offset needs the
	# extent and corrects itself on the refresh that brings one; nothing is drawn on
	# it before then anyway.
	_sync_canvas_transform(area, map_w, map_h)
	if _view_size.x <= 0 or _view_size.y <= 0:
		return
	if _tilted:
		# Aim at the middle of the map on the floor, and stand back along the view
		# axis. How far back does not matter to an orthographic projection; what
		# matters is that near and far contain a world that is now deep.
		var depth := map_h / maxf(_sin_tilt, 0.0001)
		var target := Vector3(map_w * 0.5, 0.0, depth * 0.5)
		# Pitch down. Negative about X takes the camera's forward from -Z toward -Y.
		_camera.rotation = Vector3(-_tilt, 0.0, 0.0)
		if _perspective:
			# The telephoto (see PERSPECTIVE): the distance is derived so the FOV
			# spans at the target exactly what the orthographic size spanned
			# everywhere, which keeps the centre of the frame at ortho scale and
			# makes zoom a dolly.
			var span := maxf(1.0, area.y / maxf(_zoom, 0.01))
			var dist := (span * 0.5) / tan(deg_to_rad(_fov_degrees) * 0.5)
			# Half the map's own reach along the view axis, plus headroom for
			# standing heights: what near and far must clear around the target.
			var half := map_h * 0.5 + float(_tile_size.y) * 8.0
			_camera.projection = Camera3D.PROJECTION_PERSPECTIVE
			_camera.fov = _fov_degrees
			_camera.position = target + _view_axis() * dist
			# The near plane rides just in front of the world rather than at the
			# camera: nothing exists in the kilometres of approach a telephoto
			# stands back through, and starting the frustum at the map is what
			# keeps depth precision -- and later the volumetric fog's froxel
			# buffer -- spent on the part with a world in it.
			_camera.near = maxf(CAM_NEAR, dist - half)
			_camera.far = dist + half + CAM_FAR
		else:
			_camera.projection = Camera3D.PROJECTION_ORTHOGONAL
			_camera.position = target + _view_axis() * CAM_Z
			_camera.near = CAM_NEAR
			_camera.far = CAM_Z + depth + CAM_FAR
	else:
		_camera.projection = Camera3D.PROJECTION_ORTHOGONAL
		_camera.position = Vector3(map_w * 0.5, -map_h * 0.5, CAM_Z)
		# Looking straight down -Z with +Y up: no rotation at all. Set rather than
		# assumed, because the tilt above is what changes it.
		_camera.rotation = Vector3.ZERO
		_camera.near = CAM_NEAR
		_camera.far = CAM_FAR
	# What the smooth follow composes onto, next frame and every frame after.
	_camera_base = _camera.position
	# The fog volume is a length of this camera's frustum, so it follows the far plane.
	_refresh_environment()
	_update_uniforms()

## Put the 2D remainder where the camera puts the world.
##
## Derived rather than borrowed: for an orthographic camera the mapping from map-local
## pixels to viewport pixels is a scale by the zoom and an offset, and it works out to
## exactly the `position` and `scale` the 2D backend sets on MapView itself. Which is the
## useful check on it -- if these two ever disagree, one of them has the camera wrong.
##
## **And it holds when the world stands up**, which is not obvious and was assumed
## otherwise for a day: these layers were hidden while tilted on the grounds that an
## affine transform cannot reproduce a rotated camera. It does not have to. Everything on
## this canvas annotates the *ground* -- a glyph fills a tile cell, an explosion is on the
## floor, combat text hangs over a tile -- and a ground point's screen position is
## unchanged by the tilt *by construction*, because the pre-stretch was chosen to make it
## so. `geometry_check.tscn` reports that a floor sprite at map pixel (96,128) lands at
## screen (80,120) at every tilt from 0 to 75 degrees, and now checks this transform
## against the camera's own projection directly.
##
## What is left is one approximation, and it is the 2D backend's too: content that ought to
## be at a height -- combat text above a head, a glyph standing in for a wall -- is drawn on
## the ground plane. That was already true flat, where there is no height to be at.
func _sync_canvas_transform(area: Vector2, map_w: float, map_h: float) -> void:
	if _canvas == null or not is_instance_valid(_canvas):
		return
	_canvas.visible = true
	_canvas_base_origin = Vector2(
		(area.x - map_w * _zoom) * 0.5, (area.y - map_h * _zoom) * 0.5)
	_canvas.transform = Transform2D(0.0, Vector2(_zoom, _zoom), 0.0, _canvas_base_origin)

func _ensure_quad() -> ArrayMesh:
	if _quad != null:
		return _quad
	var vertices := PackedVector3Array([
		Vector3(0.0, 0.0, 0.0), Vector3(1.0, 0.0, 0.0),
		Vector3(1.0, 1.0, 0.0), Vector3(0.0, 1.0, 0.0),
	])
	var uvs := PackedVector2Array([
		Vector2(0.0, 0.0), Vector2(1.0, 0.0), Vector2(1.0, 1.0), Vector2(0.0, 1.0),
	])
	# The quad faces +Z in its own space, and the placement transform decides what that
	# means in the world: `_to_world_tilted` gives a ground quad a third basis column of
	# +Y and a standing one +Z, so a floor's normal points up and a wall's points at the
	# viewer without either being a special case. That is the payoff for having chosen
	# those columns deliberately -- and it is also why the flat world needs no normals
	# of its own, since every quad there faces the camera by construction.
	var normals := PackedVector3Array([
		Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, 1.0),
		Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, 1.0),
	])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2, 0, 2, 3])
	_quad = ArrayMesh.new()
	_quad.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return _quad

## Which mesh a sprite batch draws (3D-8). Standing terrain gets the box; everything
## else -- ground, creatures, the whole flat world -- keeps the quad. Vegetation is
## excluded by its sway flag on purpose: a canopy painted out to the frame's edge
## would grow walls and a roof of smeared leaf pixels, where a wall's edge pixels
## *are* its material. Furniture that stops short of the frame edge degrades to the
## quad look on its own, because its edge pixels are transparent and the scissor
## discards the faces they cover.
func _sprite_mesh(tall: bool, creature: bool, sway: bool) -> ArrayMesh:
	if _tilted and tall and not creature and not sway:
		return _ensure_box()
	return _ensure_quad()

## Where a sprite's paint actually is, as a sprite-local rect (3D-8's fitting pass).
##
## The frame is the artist's canvas, not the object's outline: a chair uses a third
## of its cell and a full-frame box around it reads as a chair painted on a crate.
## This is the measurement that lets the box shrink to the chair. `get_used_rect`
## counts any pixel with alpha, which is slightly generous next to the shader's 0.5
## scissor -- a soft halo inflates the fit by a pixel -- and generous is the right
## direction to be wrong in. Falls back to the full frame when the atlas has no
## CPU-side image or the region is entirely transparent.
func _painted_rect(atlas_i: int, sx: int, sy: int, sw: int, sh: int) -> Rect2i:
	var key := "%d:%d:%d:%d:%d" % [atlas_i, sx, sy, sw, sh]
	var hit = _painted.get(key)
	if hit != null:
		return hit
	var rect := Rect2i(0, 0, sw, sh)
	if atlas_i >= 0 and atlas_i < _atlas_images.size() and _atlas_images[atlas_i] != null:
		var img: Image = _atlas_images[atlas_i]
		var used: Rect2i = img.get_region(Rect2i(sx, sy, sw, sh)).get_used_rect()
		if used.size.x > 0 and used.size.y > 0:
			rect = used
	_painted[key] = rect
	return rect

## The standing quad extruded away from the camera (3D-8), so a north-south wall
## run meets the next tile's front face with no gap.
##
## Local space matches `_ensure_quad` -- x across, y down the sprite, unit-sized --
## and the depth is unit too: local z spans [-1, 0], and the instance transform's
## basis z column stretches it to the real depth in world units (a tile of ground
## is `tile / sin` there, the same pre-stretch the ground rows get). Baking the
## depth into the mesh was the first version, and it meant one depth for every
## sprite and a rebuild whenever the tilt moved; on the instance, a wall and a
## fitted chair share this mesh in the same batch.
##
## Faces: front (the original quad, pixel-identical), two sides sampling the sprite's
## edge columns, a top sampling its top row. The UV pinning is the whole trick: the
## fragment shader already clamps samples half a texel inside the sprite's rect, so
## u = 0 exactly is the left column and v = 0 the top row, smeared across the face.
## No bottom -- it stands on the ground -- and no back: the camera's yaw is fixed, so
## a back face could only ever be seen *through* transparent front pixels, where it
## would read as a second copy of the sprite a tile deeper.
func _ensure_box() -> ArrayMesh:
	if _box != null:
		return _box
	var d := 1.0
	var vertices := PackedVector3Array([
		# Front, z = 0: vertex for vertex the quad above.
		Vector3(0.0, 0.0, 0.0), Vector3(1.0, 0.0, 0.0),
		Vector3(1.0, 1.0, 0.0), Vector3(0.0, 1.0, 0.0),
		# Left side, x = 0.
		Vector3(0.0, 0.0, 0.0), Vector3(0.0, 0.0, -d),
		Vector3(0.0, 1.0, -d), Vector3(0.0, 1.0, 0.0),
		# Right side, x = 1.
		Vector3(1.0, 0.0, 0.0), Vector3(1.0, 1.0, 0.0),
		Vector3(1.0, 1.0, -d), Vector3(1.0, 0.0, -d),
		# Top, y = 0 -- the sprite's top edge, which is up in the world.
		Vector3(0.0, 0.0, 0.0), Vector3(1.0, 0.0, 0.0),
		Vector3(1.0, 0.0, -d), Vector3(0.0, 0.0, -d),
	])
	var uvs := PackedVector2Array([
		Vector2(0.0, 0.0), Vector2(1.0, 0.0), Vector2(1.0, 1.0), Vector2(0.0, 1.0),
		# Sides pin u to the edge column; v still runs down the sprite.
		Vector2(0.0, 0.0), Vector2(0.0, 0.0), Vector2(0.0, 1.0), Vector2(0.0, 1.0),
		Vector2(1.0, 0.0), Vector2(1.0, 1.0), Vector2(1.0, 1.0), Vector2(1.0, 0.0),
		# Top pins v to the top row; u still runs across it.
		Vector2(0.0, 0.0), Vector2(1.0, 0.0), Vector2(1.0, 0.0), Vector2(0.0, 0.0),
	])
	# Local y grows *down* the sprite (the placement's y column carries the flip), so
	# world-up is local -y. The shader turns any of these toward the viewer anyway.
	var normals := PackedVector3Array([
		Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, 1.0),
		Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, 1.0),
		Vector3(-1.0, 0.0, 0.0), Vector3(-1.0, 0.0, 0.0),
		Vector3(-1.0, 0.0, 0.0), Vector3(-1.0, 0.0, 0.0),
		Vector3(1.0, 0.0, 0.0), Vector3(1.0, 0.0, 0.0),
		Vector3(1.0, 0.0, 0.0), Vector3(1.0, 0.0, 0.0),
		Vector3(0.0, -1.0, 0.0), Vector3(0.0, -1.0, 0.0),
		Vector3(0.0, -1.0, 0.0), Vector3(0.0, -1.0, 0.0),
	])
	var indices := PackedInt32Array([
		0, 1, 2, 0, 2, 3,
		4, 5, 6, 4, 6, 7,
		8, 9, 10, 8, 10, 11,
		12, 13, 14, 12, 14, 15,
	])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_INDEX] = indices
	_box = ArrayMesh.new()
	_box.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return _box

func _clear_batches() -> void:
	for node in _batches.values():
		if is_instance_valid(node):
			node.queue_free()
	_batches.clear()
	for node in _ident_batches.values():
		if is_instance_valid(node):
			node.queue_free()
	_ident_batches.clear()

## Screen pixels to world units, flat: y flips, because screen y grows downward and 3D
## y grows up. Everything the snapshot publishes is in screen pixels, so this is where
## the two conventions meet.
static func _to_world(xf: Transform2D, z: float) -> Transform3D:
	return Transform3D(
		Vector3(xf.x.x, -xf.x.y, 0.0),
		Vector3(xf.y.x, -xf.y.y, 0.0),
		Vector3(0.0, 0.0, 1.0),
		Vector3(xf.origin.x, -xf.origin.y, z))

## Screen pixels to world units, stood up: the ground lies down and standing things
## stand (ADR-006 option B).
##
## The axes are x = east, y = up, z = south, and the camera is pitched down by `_tilt`.
## The projection then scales anything horizontal by `sin` along the rows and anything
## vertical by `cos`, so **each is pre-divided by its own factor** and the artist's
## pixels come back exactly. That is the whole of option B, and it is why standing the
## world up is not supposed to look like anything on its own.
##
##   - **Flat sprites** (a floor, a road) lie in the ground plane. A screen row maps to
##     depth: `z = screen_y / sin`.
##   - **Standing sprites** (a wall, a tree, a creature) keep their own plane and rise
##     from the floor of the tile their base is on. Height maps as
##     `y = (base_screen_y - screen_y) / cos`, so the base lands at y = 0 and the top
##     lands exactly `src_h` pixels up the screen from it.
##
## Both mappings are linear in the screen coordinates, which is why they can be applied
## to a Transform2D's basis and origin rather than to each corner -- rotation and the
## mirror case come along for free, and there is still one copy of that arithmetic.
##
## @param base the screen y of the sprite's bottom edge, which is the row it stands on.
func _to_world_tilted(xf: Transform2D, tall: bool, base: float, z_below: int) -> Transform3D:
	var floor_y := _level_y(z_below)
	if not tall:
		var depth := 1.0 / maxf(_sin_tilt, 0.0001)
		return Transform3D(
			Vector3(xf.x.x, 0.0, xf.x.y * depth),
			Vector3(xf.y.x, 0.0, xf.y.y * depth),
			Vector3(0.0, 1.0, 0.0),
			Vector3(xf.origin.x, floor_y, xf.origin.y * depth))
	var rise := 1.0 / maxf(_cos_tilt, 0.0001)
	var z := base / maxf(_sin_tilt, 0.0001)
	return Transform3D(
		Vector3(xf.x.x, -xf.x.y * rise, 0.0),
		Vector3(xf.y.x, -xf.y.y * rise, 0.0),
		Vector3(0.0, 0.0, 1.0),
		Vector3(xf.origin.x, ( base - xf.origin.y ) * rise + floor_y, z))

## World height of the floor of the level @p z_below levels under the avatar (3D-4).
##
## Divided by the cosine like every other height here, so the drop is expressed in the
## same pre-stretched units the sprites are: `LEVEL_DROP_TILES` tiles of *screen* height
## per level, which projects back to exactly that many pixels.
func _level_y(z_below: int) -> float:
	if not _tilted or z_below <= 0:
		return 0.0
	return -float(z_below) * LEVEL_DROP_TILES * float(_tile_size.y) \
		/ maxf(_cos_tilt, 0.0001)

## Place one sprite, whichever world it is going into.
##
## @param depth the rank's ordinal, which becomes a nudge along the view axis when
##        tilted and the z coordinate itself when flat.
func _place(xf: Transform2D, tall: bool, base: float, depth: float,
		z_below: int = 0) -> Transform3D:
	if not _tilted:
		return _to_world(xf, depth)
	var placed := _to_world_tilted(xf, tall, base, z_below)
	# Along the camera's own axis, which is the one direction an orthographic projection
	# cannot see: the rank keeps ordering sprites within a row without moving any of them
	# by a pixel.
	#
	# Scaled down to TILT_DEPTH_STEP, which is what that constant was declared for and
	# then not used for. Passing the compacted z straight in moved a high-ranked sprite up
	# to a hundred units up and toward the camera -- invisible on screen, which is why the
	# geometry gate was happy, and not invisible anywhere else: a contact shadow reads its
	# creature's anchor out of this transform, and the tilted light UV reads world z out of
	# it, so both were being taken from up to two tiles away.
	placed.origin += _view_axis() * (depth * (TILT_DEPTH_STEP / Z_STEP))
	return placed

## Unit vector from the map toward the camera. Straight down the depth axis when flat;
## up and back by the tilt when stood up.
func _view_axis() -> Vector3:
	if not _tilted:
		return Vector3(0.0, 0.0, 1.0)
	return Vector3(0.0, _sin_tilt, _cos_tilt)

func _row_of(i: int) -> int:
	var th: int = maxi(1, _tile_size.y)
	return int(floor(float(_cmds[i + 6] + _cmds[i + 4]) / float(th)))

func _batch_for(key: String, atlas_i: int, sway: bool, palette: int,
		lit: bool, tall: bool, creature: bool, z_below: int) -> MultiMeshInstance3D:
	var existing = _batches.get(key)
	if existing != null and is_instance_valid(existing):
		# Re-chosen on reuse, because batch nodes outlive the thing the choice
		# depends on: a tilt toggle rebuilds instances but keeps the nodes, and a
		# node built flat would keep drawing quads where the stood-up world wants
		# boxes. Assigning the same mesh back is free.
		existing.multimesh.mesh = _sprite_mesh(tall, creature, sway)
		return existing

	var tex: Texture2D = _atlases[atlas_i]
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_custom_data = true
	mm.use_colors = true
	mm.mesh = _sprite_mesh(tall, creature, sway)

	var mat := ShaderMaterial.new()
	mat.shader = TILE_SHADER_3D
	mat.set_shader_parameter("atlas_tex", tex)
	mat.set_shader_parameter("atlas_texel",
		Vector2(1.0 / float(tex.get_width()), 1.0 / float(tex.get_height())))
	mat.set_shader_parameter("receives_light", lit)
	mat.set_shader_parameter("level_below", z_below)
	mat.set_shader_parameter("alpha_scissor", ALPHA_SCISSOR)
	mat.set_shader_parameter("sway_enabled", sway)
	mat.set_shader_parameter("palette_row", palette)
	if _palette_tex != null:
		mat.set_shader_parameter("palette_tex", _palette_tex)
		mat.set_shader_parameter("palette_rows", float(_palette_tex.get_height()))

	var node := MultiMeshInstance3D.new()
	node.name = "TileBatch_" + key.replace(":", "_")
	node.multimesh = mm
	node.material_override = mat
	# Only standing sprites in a stood-up world, and then double-sided: these are single
	# quads, so a shadow caster that only counts its front face vanishes as soon as the light
	# is behind it. Creatures are excluded because their capsule proxies cast for them.
	var casts := tall and _tilted and not creature
	node.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_DOUBLE_SIDED if casts \
		else GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	# Hidden when the proxies are being shown in their place; see SHOW_SHADOW_PROXIES.
	node.visible = not (creature and SHOW_SHADOW_PROXIES and _tilted)
	add_child(node)
	_batches[key] = node
	return node

## Which library id a command's ident bits name, or "" when the id has no mesh.
## The table is append-only in C++, so it is re-copied only when a command names
## an index past the cached end -- one call per new id ever seen, not per frame.
func _ident_id(ident: int) -> String:
	var index := ident - 1
	if index >= _ident_table.size():
		if _host != null and _host.has_method("get_map_ident_table"):
			_ident_table = _host.get_map_ident_table()
		if index >= _ident_table.size():
			return ""
	var id := _ident_table[index]
	return id if TERRAIN_MESHES.library().has(id) else ""

## One MultiMesh per furniture id on screen, drawing that id's library mesh with
## the CDDA tint in INSTANCE_CUSTOM (COLOR is the baked material; see
## mesh_tiles_3d.gdshader). Real geometry casts real shadows.
func _ident_batch_for(id: String, mesh: Mesh) -> MultiMeshInstance3D:
	var existing = _ident_batches.get(id)
	if existing != null and is_instance_valid(existing):
		return existing
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_custom_data = true
	mm.mesh = mesh
	var mat := ShaderMaterial.new()
	mat.shader = MESH_SHADER_3D
	var node := MultiMeshInstance3D.new()
	node.name = "IdentMesh_" + id
	node.multimesh = mm
	node.material_override = mat
	node.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	add_child(node)
	_ident_batches[id] = node
	return node

## Place every routed furniture command as its library mesh (3D-8d).
##
## Sizing is two rules and a min. Height-match: at this camera a mesh's apparent
## height on screen is `k * (box.y + box.z)` -- the pre-stretches cancel so the
## rise contributes its true height and the depth its true depth -- so k is
## chosen to reproduce the sprite's painted height, and a bed that paints low
## and deep comes out low and deep. Footprint cap: k may not push the footprint
## past 1.1 tiles, which is what stops the height rule from growing a bed past
## the tile it owns. The min of the two is the scale.
func _fill_ident_batches(mesh_buckets: Dictionary) -> void:
	for id in _ident_batches:
		if not mesh_buckets.has(id):
			var idle: MultiMeshInstance3D = _ident_batches[id]
			if is_instance_valid(idle):
				idle.multimesh.instance_count = 0
	if mesh_buckets.is_empty():
		return
	var rise := 1.0 / maxf(_cos_tilt, 0.0001)
	var ground := 1.0 / maxf(_sin_tilt, 0.0001)
	for id in mesh_buckets:
		var entry: Dictionary = TERRAIN_MESHES.library()[id]
		var node := _ident_batch_for(id, entry["mesh"])
		var box: AABB = entry["box"]
		var offsets: PackedInt32Array = mesh_buckets[id]
		var mm := node.multimesh
		mm.instance_count = offsets.size()
		for slot in offsets.size():
			var o: int = offsets[slot]
			var src_w: int = _cmds[o + 3]
			var src_h: int = _cmds[o + 4]
			# The sprite's own placement, depth-rank zero: real geometry needs
			# no ordering nudge, the depth buffer has its actual shape.
			var flat := MV.tile_transform(
				float(_cmds[o + 5]), float(_cmds[o + 6]),
				float(src_w), float(src_h), 0, false)
			var placed := _place(flat, true, float(_cmds[o + 6] + src_h), 0.0,
				(_cmds[o + 9] & MV.Z_BELOW_MASK) >> MV.Z_BELOW_SHIFT)
			# Bottom-centre of the sprite's front face is the tile's front row;
			# the mesh stands in the middle of the tile's ground, half a
			# stretched tile further north.
			var anchor := placed * Vector3(0.5, 1.0, 0.0)
			anchor.z -= float(_tile_size.y) * ground * 0.5
			var fitr := _painted_rect(_cmds[o], _cmds[o + 1], _cmds[o + 2],
				src_w, src_h)
			var k := minf(
				float(fitr.size.y) / maxf(0.05, box.size.y + box.size.z),
				1.1 * float(_tile_size.y) / maxf(0.05, maxf(box.size.x, box.size.z)))
			# World anisotropy on the world's axes, the piece's quarter-turn
			# inside it -- the same order every placement here uses.
			var rot: int = _cmds[o + 9] & MV.ROTATION_MASK
			var basis := Basis(
				Vector3(k, 0.0, 0.0),
				Vector3(0.0, k * rise, 0.0),
				Vector3(0.0, 0.0, k * ground)) \
				* Basis(Vector3.UP, -PI * 0.5 * float(rot))
			mm.set_instance_transform(slot, Transform3D(basis, anchor))
			mm.set_instance_custom_data(slot, _unpack_tint(_cmds[o + 8]))

## Bucket by what a batch's uniforms are, put the depth on each sprite, and let the
## depth buffer do the interleaving (3D-1b).
##
## This is where the 3D backend stops imitating the 2D one and starts paying for
## itself. `map_view.gd` has to put the row and the layer in its batch key, because
## a MultiMesh draws as one unit and two of them cannot interleave -- which costs "a
## few hundred [draw calls] in a dense forest, against six for the whole map
## before", in its own words. Alpha-scissored sprites go in the *opaque* pass with
## per-pixel depth, so interleaving stops being a property of draw order: each
## sprite carries its own z and the depth buffer sorts them, whatever atlas they
## came from.
##
## What is left in the key is exactly the shader uniforms, because those genuinely
## cannot vary within a batch: the atlas, sway, the palette row, and whether the
## batch receives light. Layer and row are gone. The count goes from a few hundred
## to about a dozen.
##
## The ordering is still `depth_rank`'s, sprite for sprite -- see `_rank_of`.
func _rebuild_batches() -> void:
	var buckets: Dictionary = {}
	# Distinct depth ranks this frame, for the compaction below.
	var ranks: Dictionary = {}
	# Which levels below the avatar appear at all, so the shadows can have a depth
	# on each of them.
	var levels: Dictionary = {}
	var n := _cmds.size()
	var i := 0
	_skipped.clear()
	_mesh_suppressed = 0
	_ident_routed = 0
	# id -> command offsets drawn from the mesh library instead of sprites.
	var mesh_buckets: Dictionary = {}
	var suppressed: Dictionary = {}
	if _creature_meshes != null and is_instance_valid(_creature_meshes):
		suppressed = _creature_meshes.suppressed_tiles()
	while i + MV.CMD_STRIDE - 1 < n:
		var atlas_i: int = _cmds[i]
		if atlas_i < 0 or atlas_i >= _atlases.size() or _atlases[atlas_i] == null:
			# The only way a published command can leave this loop undrawn, and it did so
			# without a word: an atlas that failed to upload takes every sprite on that
			# sheet with it, and Ultica spreads character overlays over five sheets. So
			# "the avatar is naked" and "one sheet is missing" look the same from outside.
			var missing_layer: int = _cmds[i + 7]
			_skipped[missing_layer] = int(_skipped.get(missing_layer, 0)) + 1
			i += MV.CMD_STRIDE
			continue
		var flags: int = _cmds[i + 9]
		var layer: int = _cmds[i + 7]
		var z_below: int = (flags & MV.Z_BELOW_MASK) >> MV.Z_BELOW_SHIFT
		# The one light exemption left: the avatar's own layers, because it is the
		# viewpoint and dimming it to match the floor makes it hard to find on a
		# dark screen. Levels below were exempt too until 3D-4 -- the light texture
		# held one texel per column and it belonged to the tile the avatar could
		# see -- and now each level has a block of its own, so they are in the pass.
		var lit := not MV.PLAYER_LAYERS.has(layer)
		# Standing or flat rejoins the key, but only while tilted, and only because
		# shadow casting is a property of the node rather than of the instance: a
		# ground quad that casts is a quad shadowing the floor it lies on, which is
		# acne rather than shadow. Flat, nothing casts and the key stays at four.
		#
		# Creatures are excluded from casting even though they stand: their capsule
		# proxies cast for them, and two shadows for one body is worse than either.
		# Terrain that stands -- a wall, a tree -- keeps its own silhouette, which is
		# right for a thing whose outline is the same from every side.
		var creature := MV.CREATURE_LAYERS.has(layer)
		if creature and not suppressed.is_empty():
			# This creature is being drawn as a mesh, so its sprite is not drawn at all
			# (3D-7c). Matched by tile because the draw list carries no identity -- and a
			# tile is an identity, since CDDA allows one creature on it. The sprite's
			# bottom edge is the tile it stands on, which is the same anchor everything
			# else here uses.
			# Centre and bottom edge, not the corner: a sprite wider than its cell has a
			# negative x offset, so quantising its left edge lands a tile to the west. The
			# channel publishes tile centres for the same reason.
			var centre_x := float(_cmds[i + 5]) + float(_cmds[i + 3]) * 0.5
			var foot_y := float(_cmds[i + 6] + _cmds[i + 4]) - 1.0
			if suppressed.has(CREATURE_MESHES.tile_key(centre_x, foot_y, _tile_size)):
				# Counted, because it is invisible by design and was mistaken for a
				# regression: the body's overlays go with it, so a meshed avatar
				# "loses" a dozen commands every frame, on purpose.
				_mesh_suppressed += 1
				i += MV.CMD_STRIDE
				continue
		var tall := (flags & MV.FLAG_TALL) != 0 or creature
		# A furniture id with a model in the library draws the model instead of
		# the sprite (3D-8d) -- tall or flat: a bed Ultica paints as a 32x32
		# ground quad is exactly as much a bed as a locker it paints standing,
		# and the mesh stands either way. Only the avatar's own level:
		# level_below is a per-batch shader uniform and one id's mesh batch must
		# not span levels, so lower floors keep their sprites -- dimmed and
		# distant, which is where a sprite is at its best anyway.
		if _tilted and z_below == 0 and layer == MV.FURNITURE_LAYER:
			var ident: int = (flags & MV.IDENT_MASK) >> MV.IDENT_SHIFT
			if ident > 0:
				var mesh_id := _ident_id(ident)
				if not mesh_id.is_empty():
					if not mesh_buckets.has(mesh_id):
						mesh_buckets[mesh_id] = PackedInt32Array()
					var mb: PackedInt32Array = mesh_buckets[mesh_id]
					mb.append(i)
					mesh_buckets[mesh_id] = mb
					_ident_routed += 1
					i += MV.CMD_STRIDE
					continue
		# The level is back in the key because it is a shader uniform again: the
		# depth fade moved out of C++ into `level_fade`, so a batch may not span two
		# levels any more than it may span two palettes. Levels are few.
		# `tall` and `creature` are separate fields rather than one derived flag, because
		# the node needs both: whether it casts (standing terrain does, creatures do not --
		# their proxies cast for them) and whether it is drawn at all (SHOW_SHADOW_PROXIES
		# hides creature sprites so the geometry can be seen in their place).
		var key := "%d:%d:%d:%d:%d:%d:%d" % [atlas_i,
			1 if (flags & MV.FLAG_SWAY) != 0 else 0,
			(flags & MV.PALETTE_MASK) >> MV.PALETTE_SHIFT,
			1 if lit else 0,
			1 if tall else 0,
			1 if creature else 0,
			z_below]
		if not buckets.has(key):
			buckets[key] = PackedInt32Array()
		var bucket: PackedInt32Array = buckets[key]
		bucket.append(i)
		buckets[key] = bucket
		ranks[_rank_of(i)] = true
		levels[z_below] = true
		i += MV.CMD_STRIDE

	# The two whole-map layers get depths of their own, seated where the 2D backend
	# seats them: over the ground, under anything standing on it. A shadow rank per
	# level present, because with the depth on each instance there is no reason for
	# every blob to share one -- which is the approximation the 2D backend had to
	# make, and why it skips creatures below the avatar's level entirely.
	for level in levels:
		ranks[_shadow_rank(level)] = true
	ranks[_field_rank()] = true

	for key in _batches:
		if not buckets.has(key):
			var idle: MultiMeshInstance3D = _batches[key]
			if is_instance_valid(idle):
				idle.multimesh.instance_count = 0

	# Rank decides the order and the order decides z, exactly as when the depth was
	# per batch: the distinct ranks are sorted and handed consecutive steps, so a
	# range of about a million values becomes a few hundred and float32 can still
	# tell one step from the next at camera distance.
	_depths.clear()
	var ordered_ranks: Array = ranks.keys()
	ordered_ranks.sort()
	for ordinal in ordered_ranks.size():
		_depths[ordered_ranks[ordinal]] = float(ordinal) * Z_STEP
	_depth_span = float(maxi(1, ordered_ranks.size())) * Z_STEP

	for key in buckets:
		var parts := (key as String).split(":")
		var atlas_i := int(parts[0])
		var node := _batch_for(key, atlas_i, int(parts[1]) != 0, int(parts[2]),
			int(parts[3]) != 0, int(parts[4]) != 0, int(parts[5]) != 0, int(parts[6]))
		var tex: Texture2D = _atlases[atlas_i]
		var inv_w := 1.0 / float(tex.get_width())
		var inv_h := 1.0 / float(tex.get_height())
		var offsets: PackedInt32Array = buckets[key]
		var mm := node.multimesh
		mm.instance_count = offsets.size()
		# Whether this batch draws the extruded box (`_sprite_mesh`'s condition,
		# spelled in key fields): standing terrain, tilted, not vegetation. Only
		# these instances get fitted and given a depth.
		var boxed := _tilted and int(parts[4]) != 0 and int(parts[5]) == 0 \
			and int(parts[1]) == 0
		var creatures: Array = []
		for slot in offsets.size():
			var o: int = offsets[slot]
			var src_w: int = _cmds[o + 3]
			var src_h: int = _cmds[o + 4]
			var flat := MV.tile_transform(
				float(_cmds[o + 5]), float(_cmds[o + 6]),
				float(src_w), float(src_h), _cmds[o + 9] & MV.ROTATION_MASK,
				(_cmds[o + 9] & MV.FLAG_FLIP_X) != 0)
			# The atlas sub-rect this instance samples; the fitting below may
			# narrow it together with the geometry, which is what keeps the front
			# face's pixels exactly the 2D backend's -- the same sub-rect over the
			# same screen rectangle.
			var cut := Rect2i(_cmds[o + 1], _cmds[o + 2], src_w, src_h)
			# One tile of ground: the depth a connector's box gets.
			var depth_px := float(_tile_size.y)
			if boxed:
				# What the game says this tile *is* (3D-8c) -- the claim paint
				# cannot make. Zero is an old library or an unclassified tile,
				# and falls through to the fitting heuristic below.
				var shape: int = (_cmds[o + 9] & MV.SHAPE_MASK) >> MV.SHAPE_SHIFT
				if shape == MV.SHAPE_DOOR or shape == MV.SHAPE_THIN:
					# A panel at the tile's face: a door is not a metre of oak,
					# and a fence is something to see over and past -- it was a
					# full connector box before these bits, because chain-link
					# paint spans its frame exactly the way a wall's does.
					depth_px = maxf(2.0, float(_tile_size.y) / 8.0)
				if shape == MV.SHAPE_WALL or shape == MV.SHAPE_WINDOW:
					# Walls and windows keep the full frame and the full tile
					# whatever their paint says: runs must seal, and a curtained
					# window is still a wall's worth of building.
					pass
				else:
					var fit := _painted_rect(atlas_i, cut.position.x, cut.position.y,
						src_w, src_h)
					# Paint that spans the frame's full width keeps the full frame
					# -- a counter run must not open seams. Anything narrower is
					# an object: geometry and sampled pixels shrink to the paint,
					# so a chair is a chair-sized block with chair-coloured sides
					# and a *visible* painted top, where the full frame's top row
					# was transparent and the scissor ate the face. An open door
					# fits down to its swung panel the same way.
					if fit.position.x > 1 or fit.end.x < src_w - 1:
						if shape == MV.SHAPE_NONE:
							# With no claim from the game, the paint's width is
							# also the best guess at its depth, capped at a tile.
							depth_px = minf(float(fit.size.x), depth_px)
						flat = flat * Transform2D(
							Vector2(float(fit.size.x) / float(src_w), 0.0),
							Vector2(0.0, float(fit.size.y) / float(src_h)),
							Vector2(float(fit.position.x) / float(src_w),
								float(fit.position.y) / float(src_h)))
						cut = Rect2i(cut.position + fit.position, fit.size)
			var xf := _place(flat, MV.CREATURE_LAYERS.has(_cmds[o + 7])
				or (_cmds[o + 9] & MV.FLAG_TALL) != 0,
				float(_cmds[o + 6] + src_h), _depths.get(_rank_of(o), 0.0),
				(_cmds[o + 9] & MV.Z_BELOW_MASK) >> MV.Z_BELOW_SHIFT)
			if boxed:
				# The unit-deep box takes its real depth here, in the same
				# pre-stretched units the ground rows use.
				xf.basis.z *= depth_px / maxf(_sin_tilt, 0.0001)
			mm.set_instance_transform(slot, xf)
			mm.set_instance_custom_data(slot, Color(
				float(cut.position.x) * inv_w,
				float(cut.position.y) * inv_h,
				float(cut.size.x) * inv_w,
				float(cut.size.y) * inv_h
			))
			mm.set_instance_color(slot, _unpack_tint(_cmds[o + 8]))
			# Creatures are the instances that move between rebuilds (SP-5), and
			# with the layer out of the key a batch now holds creatures and terrain
			# together -- so the filter is per instance rather than per batch.
			if MV.CREATURE_LAYERS.has(_cmds[o + 7]):
				var layer: int = _cmds[o + 7]
				creatures.append({
					"slot": slot,
					# Bottom centre in world units, the point that says which tile
					# a creature is standing on whatever its sprite offset.
					"anchor": (xf * Vector3(0.5, 1.0, 0.0)),
					"xform": xf,
					"color": _unpack_tint(_cmds[o + 8]),
					# Bodies only cast a shadow. A character is one body plus a
					# dozen clothing overlays at the same anchor, and taking every
					# creature instance stacked twelve blobs on one pair of feet.
					"body": layer == MV.MONSTER_LAYER_BODY or layer == MV.PLAYER_LAYER_BODY,
					"shadow_z": _depths.get(_shadow_rank(
						(_cmds[o + 9] & MV.Z_BELOW_MASK) >> MV.Z_BELOW_SHIFT), 0.0),
					# The height of the floor this creature is standing on, so its blob
					# lands on that floor rather than on the avatar's (3D-4).
					"level_y": _level_y((_cmds[o + 9] & MV.Z_BELOW_MASK) >> MV.Z_BELOW_SHIFT),
				})
		if creatures.is_empty():
			_creature_slots.erase(key)
		else:
			_creature_slots[key] = creatures

	_fill_ident_batches(mesh_buckets)

	# A MultiMesh recomputes its own bounds from every instance and a batch is
	# rebuilt every turn, so hand the bounds over instead: the map's extent is known
	# here and is the same for every batch. The margin covers sprites that overhang
	# the view's edge and vertices the sway shader displaces.
	var map_w := float(maxi(1, _view_size.x * _tile_size.x))
	var map_h := float(maxi(1, _view_size.y * _tile_size.y))
	var margin := float(maxi(_tile_size.x, _tile_size.y) * 4)
	var bounds := AABB(
		Vector3(-margin, -map_h - margin, -Z_STEP),
		Vector3(map_w + margin * 2.0, map_h + margin * 2.0, _depth_span + Z_STEP * 2.0))
	for key in _batches:
		var node: MultiMeshInstance3D = _batches[key]
		if is_instance_valid(node):
			node.custom_aabb = bounds
	for id in _ident_batches:
		var node: MultiMeshInstance3D = _ident_batches[id]
		if is_instance_valid(node):
			node.custom_aabb = bounds

	if not _hits.is_empty():
		_apply_hit_reactions()

## Where a contact shadow sits on level @p z_below: on the floor of that level, under
## everything standing on it. `map_view.gd` seats its whole shadow layer at the
## avatar's own -- it has one node and therefore one depth to spend.
func _shadow_rank(z_below: int) -> int:
	return (MV.MAX_Z_BELOW - clampi(z_below, 0, MV.MAX_Z_BELOW)) * MV.Z_LEVEL_SPAN \
		+ MV.TALL_BAND - 2

## Where the field particles sit: over the fire they come from, under the creature
## walking through it. The field list carries no level, so this is the avatar's, as
## it is in the 2D backend.
func _field_rank() -> int:
	return MV.TOP_LEVEL_BASE + MV.TALL_BAND - 1

## Hand each layer's fallback glyphs to its own node, as the 2D backend does.
##
## Split by layer there so a glyph keeps the z-order its sprite would have had. Here
## they are all on the canvas above the world instead, so the split buys only the
## reuse of `glyph_layer.gd` -- and it costs the ordering, which is the one thing
## these do worse than the 2D backend. A glyph is drawn for a tile that resolved to
## no sprite at all, so nothing of that tile is there to hide it; what can now happen
## is a glyph for a distant tile drawing over a sprite in front of it. A tileset with
## full coverage emits none.
func _rebuild_glyph_layers() -> void:
	if _canvas == null or not is_instance_valid(_canvas):
		return
	var buckets: Dictionary = {}
	var n := _glyphs.size()
	var i := 0
	while i + MV.GLYPH_STRIDE - 1 < n:
		var layer: int = _glyphs[i + 2]
		var bucket: PackedInt32Array = buckets.get(layer, PackedInt32Array())
		bucket.append_array(_glyphs.slice(i, i + MV.GLYPH_STRIDE))
		buckets[layer] = bucket
		i += MV.GLYPH_STRIDE

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
	_canvas.add_child(node)
	# Under the animation overlay, which is added first and must stay on top.
	_canvas.move_child(node, 0)
	_glyph_layers[layer] = node
	return node

## A blob under every creature body, from the records the batch pass already keeps.
##
## `shadow_layer.gd`'s geometry, in the world: the blob is centred on the contact
## point rather than on the anchor (a bottom-of-tile anchor sits visibly below the
## feet in a three-quarter view), flattened, and scaled to the sprite's own width.
## Its depth comes from the instance, so each blob is on the floor of the level its
## creature is standing on.
##
## Deliberately not moved by hit reactions, exactly as in 2D: the body recoils, the
## shadow is on the ground and stays where the feet were.
func _rebuild_shadows() -> void:
	if _shadow_batch == null or not is_instance_valid(_shadow_batch):
		return
	var mm := _shadow_batch.multimesh
	var strength := 1.0
	if _host != null and _host.has_method("get_conditions"):
		strength = clampf(float((_host.get_conditions() as Dictionary).get("daylight",
			1.0)), 0.0, 1.0)
	if strength <= 0.02:
		# An overcast midnight casts no sun shadow, and drawing one anyway reads as
		# grime on the floor rather than as depth. The proxies go with the blobs: a
		# capsule left standing would keep occluding whatever light there is.
		mm.instance_count = 0
		if _shadow_proxy != null and is_instance_valid(_shadow_proxy):
			_shadow_proxy.multimesh.instance_count = 0
		return
	var bodies: Array = []
	for key in _creature_slots:
		for entry in _creature_slots[key]:
			if entry["body"]:
				bodies.append(entry)
	mm.instance_count = bodies.size()
	if bodies.is_empty():
		return
	var colour := Color(0.0, 0.0, 0.0, SHADOW.MAX_ALPHA * strength)
	# The proxies stand beside the blobs and answer a different question, so they are filled
	# from the same records rather than from a pass of their own. Only while tilted: a flat
	# world has nothing for a shadow to fall across.
	var proxies: MultiMesh = null
	if _shadow_proxy != null and is_instance_valid(_shadow_proxy):
		if _tilted:
			proxies = _shadow_proxy.multimesh
			proxies.instance_count = bodies.size()
		else:
			_shadow_proxy.multimesh.instance_count = 0
	for slot in bodies.size():
		var entry = bodies[slot]
		var xform: Transform3D = entry["xform"]
		# `basis.x`, not `.x`: Transform2D exposes its axes directly and Transform3D does
		# not, which is the sort of difference that ports quietly and then fails to parse --
		# taking the whole script, and with it the map, down with it. The x basis of a
		# sprite's quad is its width in pixels.
		mm.set_instance_transform(slot, shadow_transform(entry["anchor"],
			xform.basis.x.length(), float(entry["shadow_z"])))
		mm.set_instance_color(slot, colour)
		if proxies != null:
			# As wide and as tall as the sprite it stands in for, so a zombie's shadow is
			# a zombie's height. The y basis of a standing quad *is* that height.
			proxies.set_instance_transform(slot, proxy_transform(entry["anchor"],
				xform.basis.x.length(), xform.basis.y.length()))

## Where a creature's contact blob goes, given the bottom-centre @p anchor of its sprite in
## world space and the sprite's @p width in pixels.
##
## Two things this got wrong while the world was standing up, and they compounded into
## characters that looked as though they were floating above their own shadows.
##
## **It reconstructed the height instead of using the anchor's.** Every sprite is nudged
## along the camera's own axis by its depth rank, which is invisible *because* the y and z
## halves of that nudge cancel under an orthographic projection. Taking z from the anchor
## and y from the level's floor kept the z half and dropped the y half, so the cancellation
## broke and the blob slid down the screen by the whole of it. Using the anchor for both is
## the fix, and it is also just simpler: the blob goes where the feet are.
##
## **And it dropped LIFT on a wrong reading.** The claim was that the anchor of a standing
## sprite *is* its feet. It is not -- it is the bottom edge of the sprite's *quad*, and the
## artist drew the contact point some way above that, which is the whole reason the 2D
## backend has this constant. Stood up, "above the feet on screen" is "away from the camera
## along the ground", and the ground is pre-stretched, so it converts by 1/sin like every
## other row distance.
##
## FLATTEN stays gone, and that reasoning was right: it faked the foreshortening of a view
## that is not straight down, and a tilted camera does that for real.
func shadow_transform(anchor: Vector3, width: float, flat_depth: float) -> Transform3D:
	var w := width * SHADOW.WIDTH_SCALE
	if _tilted:
		var lift := float(_tile_size.y) * SHADOW.LIFT / maxf(_sin_tilt, 0.0001)
		return Transform3D(
			Vector3(w, 0.0, 0.0), Vector3(0.0, 0.0, w), Vector3(0.0, 1.0, 0.0),
			# A hair above the floor so it does not fight it for depth.
			Vector3(anchor.x - w * 0.5, anchor.y + 0.05, anchor.z - lift - w * 0.5))
	var h := w * SHADOW.FLATTEN
	var cy := anchor.y + float(_tile_size.y) * SHADOW.LIFT
	return Transform3D(
		Vector3(w, 0.0, 0.0), Vector3(0.0, -h, 0.0), Vector3(0.0, 0.0, 1.0),
		Vector3(anchor.x - w * 0.5, cy + h * 0.5, flat_depth))

func _refresh_field_particles() -> void:
	if _field_particles == null or not is_instance_valid(_field_particles):
		return
	if _host == null or not _host.has_method("get_map_field_list"):
		return
	var wind := Vector2.ZERO
	if _host.has_method("get_wind_vector"):
		wind = _host.get_wind_vector()
	_field_particles.set_tilted(_tilted, _sin_tilt)
	_field_particles.refresh(_host.get_map_field_list(), _tile_size, wind,
		_depths.get(_field_rank(), 0.0))
	if _host.has_method("get_conditions"):
		var pc: Dictionary = _host.get_conditions()
		_field_particles.set_conditions(float(pc.get("daylight", 1.0)),
			float(pc.get("precipitation", 0.0)), float(pc.get("pain", 0.0)))

## Weather falling through the scene (3D-6). Tilted only, like the engine lights:
## flat has no "through the scene" for anything to fall through, and the flat world
## is the 2D backend's baseline and must stay on it.
func _refresh_weather_particles() -> void:
	if _weather_particles == null or not is_instance_valid(_weather_particles):
		return
	if _host == null or not _host.has_method("get_conditions"):
		return
	var wind := Vector2.ZERO
	if _host.has_method("get_wind_vector"):
		wind = _host.get_wind_vector()
	var cond: Dictionary = _host.get_conditions()
	var map_w := float(_view_size.x * _tile_size.x)
	var map_h := float(_view_size.y * _tile_size.y)
	_weather_particles.set_tilted(_tilted, _sin_tilt)
	_weather_particles.refresh(int(cond.get("weather_kind", 0)),
		float(cond.get("precipitation", 0.0)), float(cond.get("daylight", 1.0)),
		wind, Vector2(map_w, map_h), float(_tile_size.y))

func _ensure_weather_particles() -> void:
	if _weather_particles != null and is_instance_valid(_weather_particles):
		return
	_weather_particles = Node3D.new()
	_weather_particles.name = "WeatherParticles"
	_weather_particles.set_script(WEATHER_PARTICLES_3D)
	add_child(_weather_particles)
	_weather_particles.setup()

## The depth rank of the command at offset @p i -- `map_view.gd`'s, unchanged, so
## the two backends agree about what is in front of what.
func _rank_of(i: int) -> int:
	var flags: int = _cmds[i + 9]
	var layer: int = _cmds[i + 7]
	var tall := (flags & MV.FLAG_TALL) != 0 or MV.CREATURE_LAYERS.has(layer)
	return MV.depth_rank(layer, tall, _row_of(i) if tall else 0,
		(flags & MV.Z_BELOW_MASK) >> MV.Z_BELOW_SHIFT)

func _unpack_tint(packed: int) -> Color:
	return Color8(
		(packed >> 24) & 0xFF,
		(packed >> 16) & 0xFF,
		(packed >> 8) & 0xFF,
		packed & 0xFF
	)

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
	if _host.has_method("get_light_levels"):
		_light_levels = maxi(1, int(_host.get_light_levels()))
	if _light_tex != null and _light_tex.get_size() == Vector2(img.get_size()):
		_light_tex.update(img)
	else:
		_light_tex = ImageTexture.create_from_image(img)
	if not _light_pass_announced and _host.has_method("set_light_pass_enabled"):
		_host.set_light_pass_enabled(true)
		_light_pass_announced = true
		# And that this backend fades lower levels itself, so C++ stops baking
		# `fog_for_depth` into their tints and nothing is dimmed twice. Announced with
		# the light pass because it is the same kind of claim and the same handshake;
		# a host that never gets here keeps both baked, which is what the 2D backend
		# relies on.
		if _host.has_method("set_depth_fog_enabled"):
			_host.set_depth_fog_enabled(true)

## Push the light texture, its extent, and the world conditions to every batch.
##
## The extent is simpler here than in the 2D backend, which has to ask for its own
## global transform because the map itself is scaled and offset. The map is at the
## world origin and the camera moves instead, so the rect is just the map's size --
## with a negative y scale, because the light texture's rows run downward and world
## y runs up.
func _update_uniforms() -> void:
	if _batches.is_empty():
		return
	var enabled := _light_tex != null and _view_size.x > 0 and _view_size.y > 0
	var origin := Vector2.ZERO
	var inv_size := Vector2.ZERO
	if enabled:
		var w := float(_view_size.x * _tile_size.x)
		var h := float(_view_size.y * _tile_size.y)
		if is_zero_approx(w) or is_zero_approx(h):
			enabled = false
		elif _tilted:
			# Rows run along +z now, and the ground is pre-stretched by 1/sin -- so the
			# texture covers that much more world than it does pixels.
			inv_size = Vector2(1.0 / w, _sin_tilt / h)
		else:
			inv_size = Vector2(1.0 / w, -1.0 / h)
	var wind := Vector2.ZERO
	if _host != null and _host.has_method("get_wind_vector"):
		wind = _host.get_wind_vector()
	var daylight := 1.0
	var precipitation := 0.0
	var pain := 0.0
	if _host != null and _host.has_method("get_conditions"):
		var c: Dictionary = _host.get_conditions()
		daylight = float(c.get("daylight", 1.0))
		precipitation = float(c.get("precipitation", 0.0))
		pain = float(c.get("pain", 0.0))
	# The sprite batches and the mesh library batches share every uniform name
	# on purpose (see mesh_tiles_3d.gdshader), so one loop feeds both.
	for key in _batches.keys() + _ident_batches.keys():
		var node: MultiMeshInstance3D = _batches.get(key, _ident_batches.get(key))
		if not is_instance_valid(node) or node.material_override == null:
			continue
		var mat: ShaderMaterial = node.material_override
		mat.set_shader_parameter("wind", wind)
		mat.set_shader_parameter("daylight", daylight)
		mat.set_shader_parameter("precipitation", precipitation)
		mat.set_shader_parameter("pain", pain)
		mat.set_shader_parameter("light_enabled", enabled)
		mat.set_shader_parameter("light_origin", origin)
		mat.set_shader_parameter("light_inv_size", inv_size)
		mat.set_shader_parameter("world_tilted", _tilted)
		mat.set_shader_parameter("vis_tex", _light_tex)
		mat.set_shader_parameter("light_tex", _light_tex)
		mat.set_shader_parameter("light_texture_levels", float(_light_levels))

func _process(delta: float) -> void:
	if not visible or _host == null:
		return
	_poll_hits()
	_follow_avatar()
	if _hits.is_empty():
		return
	var still: Array = []
	for hit in _hits:
		hit["t"] += delta
		if hit["t"] < MV.HIT_DURATION:
			still.append(hit)
	var ended := still.size() != _hits.size()
	_hits = still
	if not _hits.is_empty() or ended:
		_apply_hit_reactions()

## The smooth camera (SMOOTH_CAMERA): compose the avatar's tween offset onto the
## per-turn camera placement, and shift the world canvas by the matching screen
## delta so the glyphs and the animation overlay stay glued to the ground they
## annotate. Screen px per world unit is the zoom for an orthographic camera; a
## ground step of world z projects to z*sin(tilt) px, and the flat world's
## vertical is -y -- the same two mappings every placement here makes.
func _follow_avatar() -> void:
	if not SMOOTH_CAMERA or _camera == null or not is_instance_valid(_camera):
		return
	if _camera_base == Vector3.INF:
		return
	var off := Vector3.ZERO
	if _creature_meshes != null and is_instance_valid(_creature_meshes) \
			and _creature_meshes.has_method("avatar_visual_offset"):
		off = _creature_meshes.avatar_visual_offset()
	_camera.position = _camera_base + off
	if _canvas == null or not is_instance_valid(_canvas) or _canvas_base_origin == Vector2.INF:
		return
	var screen := Vector2(off.x, off.z * _sin_tilt) if _tilted else Vector2(off.x, -off.y)
	var t := _canvas.transform
	t.origin = _canvas_base_origin - screen * _zoom
	_canvas.transform = t

func _poll_hits() -> void:
	if not _host.has_method("get_hit_generation"):
		return
	var generation: int = _host.get_hit_generation()
	if generation <= _hit_seen:
		return
	var events: PackedInt32Array = _host.get_hit_events()
	var n := events.size()
	if n % MV.HIT_STRIDE != 0:
		# Pre-API-24 packing; the handshake reports it, this declines to smear it.
		return
	var i := 0
	while i + MV.HIT_STRIDE - 1 < n:
		var id: int = events[i]
		if id > _hit_seen:
			var kind: int = events[i + 9]
			# The mesh layer gets every event -- attack and hit clips on kind 0,
			# the death clip on kind 1 -- addressed by the uids the channel
			# carries since API 24.
			if _creature_meshes != null and is_instance_valid(_creature_meshes) \
					and _creature_meshes.has_method("on_hit_event"):
				_creature_meshes.on_hit_event(events[i + 7], events[i + 8], kind)
			# The sprite lunge stays for creatures still drawn as sprites, and
			# only for real hits: a dead sprite is gone from the next draw list,
			# and a swing's position is the attacker's own tile.
			if kind == MV.HIT_KIND_HIT:
				_hits.append({
					"tile": Vector2i(events[i + 1], events[i + 2]),
					"dir": Vector2(float(events[i + 4]), float(events[i + 5])),
					"flash": _unpack_tint(events[i + 6]),
					"t": 0.0,
				})
		i += MV.HIT_STRIDE
	_hit_seen = generation

func _hit_curve(t: float) -> float:
	var u := clampf(t / MV.HIT_DURATION, 0.0, 1.0)
	return sin(u * PI) * (1.0 - u * 0.35)

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
			var offset: Vector3 = response["offset"]
			var xform: Transform3D = entry["xform"]
			if offset != Vector3.ZERO:
				xform = xform.translated(offset)
			mm.set_instance_transform(slot, xform)
			var base: Color = entry["color"]
			var flash: float = response["flash"]
			mm.set_instance_color(slot,
				base.lerp(response["color"], flash * 0.75) if flash > 0.0 else base)

## Displacement and flash currently applying to a sprite whose base sits at
## @a anchor, in world units.
##
## The hit direction arrives in screen space, so its y is negated on the way into
## the world for the same reason every placement is.
func hit_response(anchor: Vector3) -> Dictionary:
	var offset := Vector3.ZERO
	var flash := 0.0
	var flash_color := Color.WHITE
	for hit in _hits:
		var tile: Vector2i = hit["tile"] - _view_origin
		var foot := Vector3((float(tile.x) + 0.5) * float(_tile_size.x),
			-float(tile.y + 1) * float(_tile_size.y), anchor.z)
		if anchor.distance_squared_to(foot) > float(_tile_size.x * _tile_size.x):
			continue
		var amount := _hit_curve(hit["t"])
		var dir: Vector2 = hit["dir"]
		if dir == Vector2.ZERO:
			dir = Vector2(0.0, 1.0)
		dir = dir.normalized()
		offset += Vector3(dir.x, -dir.y, 0.0) * amount * MV.HIT_OFFSET \
			* float(maxi(_tile_size.x, _tile_size.y))
		if amount > flash:
			flash = amount
			flash_color = hit["flash"]
	return { "offset": offset, "flash": flash, "color": flash_color }

## Where a sprite of @p w x @p h at (@p x, @p y) screen pixels actually lands on screen,
## by placing it in the world and projecting it back through the camera.
##
## This is how option B's central claim gets checked instead of asserted. The
## pre-stretch is supposed to cancel the tilt exactly -- ground scaled by `1/sin`,
## height by `1/cos` -- and "supposed to" is the part that has cost this branch the
## most. Round-tripping through `Camera3D.unproject_position` needs no GPU and no
## screenshot: if the arithmetic is right the rect comes back where it started, at any
## tilt, and if it is wrong it says by how much.
##
## Empty when the camera has not been placed yet.
func debug_projected_rect(x: float, y: float, w: float, h: float, tall: bool,
		z_below: int = 0) -> Rect2:
	if _camera == null or not is_instance_valid(_camera) or not _camera.is_inside_tree():
		return Rect2()
	var xf := MV.tile_transform(x, y, w, h, 0, false)
	var placed := _place(xf, tall, y + h, 0.0, z_below)
	# The quad's own corners, so this measures the placement rather than repeating it.
	var a := _camera.unproject_position(placed * Vector3(0.0, 0.0, 0.0))
	var b := _camera.unproject_position(placed * Vector3(1.0, 1.0, 0.0))
	return Rect2(Vector2(minf(a.x, b.x), minf(a.y, b.y)), (b - a).abs())

## Where the world canvas puts a map pixel, in viewport pixels.
##
## Exposed so the geometry gate can hold it against `debug_projected_rect`: the canvas and
## the camera are derived separately from the same inputs, and the claim that the glyphs and
## the animation overlay can stay on a canvas at any tilt is exactly the claim that these
## two agree. Empty when the canvas has not been built.
func debug_canvas_point(map_px: Vector2) -> Vector2:
	if _canvas == null or not is_instance_valid(_canvas):
		return Vector2.INF
	return _canvas.transform * map_px

## What the 3D backend just built, for the probe and the render overlay. Reported
## rather than inferred: "the batches exist" and "the batches are in the right
## order" are different claims and the second is the one that can regress.
func debug_stats() -> Dictionary:
	var instances := 0
	for key in _batches:
		var node: MultiMeshInstance3D = _batches[key]
		if not is_instance_valid(node):
			continue
		instances += node.multimesh.instance_count
	return {
		"batches": _batches.size(),
		"instances": instances,
		# Reported from what was computed, never read back out of the MultiMesh: a
		# read-back goes through the rendering driver and the dummy driver returns
		# identity for every instance, so a headless check of instance depths would
		# pass whatever the code did.
		"depths": _depths.size(),
		"depth_span": _depth_span,
		"scissor": ALPHA_SCISSOR,
		"proxies_shown": SHOW_SHADOW_PROXIES and _tilted,
		"tilt": _tilt_degrees,
		"level_drop": LEVEL_DROP_TILES if _tilted else 0.0,
		"tilted": _tilted,
		"suppressed": _mesh_suppressed,
		"furniture_meshes": _ident_routed,
		"lights": _lights_used,
		"beams": _beams_used,
		"lights_published": _lights_published,
		"fog": _volumetric_fog and _tilted,
		"zoom": _zoom,
		"camera_size": _camera.size if _camera != null and is_instance_valid(_camera) else 0.0,
		"camera_position": _camera.position if _camera != null and is_instance_valid(_camera) \
			else Vector3.ZERO,
		"light_pass": _light_pass_announced,
	}
