extends Node3D
## Weather as particles falling through the scene (ADR-006 item 3D-6).
##
## Rain, snow and acid, drawn as `GPUParticles3D` over the visible map. Follows
## `field_particles_3d.gd` where the two overlap -- built dot texture, unshaded
## billboard draw pass, wind folded into gravity from the same vector the sway
## shader uses -- and differs where weather differs from fire:
##
##   - One emitter, not a pool. Fire is somewhere; weather is everywhere, so the
##     emission shape is a box over the whole visible map and the amount scales
##     with precipitation and area rather than with a field's intensity.
##   - Tilted only. Flat, there is no "through the scene" for anything to fall
##     through -- a particle falling along -y would fall *up the screen* -- and the
##     flat world is the 2D backend's baseline and must stay on it. Same gate the
##     engine lights use, for the same reason.
##
## What falls comes from `conditions.weather_kind` (0 none, 1 rain, 2 snow,
## 3 acid), which C++ derives from the weather type's own `rains` /
## `tiles_animation` -- the key the SDL renderer animated from. How much comes
## from `conditions.precipitation`.

const KIND_NONE := 0
const KIND_RAIN := 1
const KIND_SNOW := 2
const KIND_ACID := 3

## Where the sheet of weather starts, in tiles of height above the ground. A
## storey and a half: high enough that drops fall past walls (Ultica draws a
## wall two tiles tall), low enough that the sheet is dense near the ground.
const TOP_TILES := 3.0

## Particles at full precipitation, before the area scale. Bounded for the same
## reason MAX_EMITTERS bounds the field pool: past a point more drops cost
## frames without reading as more rain. VER-1 material.
const MAX_PARTICLES := 700

var _emitter: GPUParticles3D
var _rain_material: ParticleProcessMaterial
var _snow_material: ParticleProcessMaterial
var _rain_draw: StandardMaterial3D
var _snow_draw: StandardMaterial3D
var _rain_quad: QuadMesh
var _snow_quad: QuadMesh
var _dot: GradientTexture2D
var _wind: Vector2 = Vector2.ZERO
var _tilted: bool = false
var _sin_tilt: float = 1.0
var _kind: int = KIND_NONE

func setup() -> void:
	_build_materials()

## Same contract as the field particles': MapView3D owns the camera and says
## which world this is.
func set_tilted(tilted: bool, sin_tilt: float) -> void:
	_tilted = tilted
	_sin_tilt = maxf(sin_tilt, 0.0001)
	if not _tilted and _emitter != null and is_instance_valid(_emitter):
		_emitter.emitting = false

## Called by MapView3D each refresh with what is falling, how hard, and the
## extent of the visible map in pixels.
func refresh(kind: int, precipitation: float, daylight: float, wind: Vector2,
		extent_px: Vector2, tile_h: float) -> void:
	if wind != _wind:
		_wind = wind
		_apply_wind()
	var wet := clampf(precipitation, 0.0, 1.0)
	if not _tilted or kind == KIND_NONE or wet <= 0.0 \
			or extent_px.x <= 0.0 or extent_px.y <= 0.0:
		if _emitter != null and is_instance_valid(_emitter):
			_emitter.emitting = false
		_kind = KIND_NONE
		return

	_ensure_emitter()
	var rainlike := kind != KIND_SNOW
	if kind != _kind:
		_kind = kind
		_emitter.process_material = _rain_material if rainlike else _snow_material
		_emitter.material_override = _rain_draw if rainlike else _snow_draw
		_emitter.draw_pass_1 = _ensure_quad(rainlike)
		# Rain crosses the volume in under a second; a flake takes several.
		_emitter.lifetime = 0.6 if rainlike else 5.0
		_apply_kind_colour()

	# The volume: the whole visible ground plane, from TOP_TILES up, falling to
	# the floor. Ground depth is pre-stretched like everything else here.
	var depth := extent_px.y / _sin_tilt
	var top := tile_h * TOP_TILES
	_emitter.position = Vector3(extent_px.x * 0.5, top, depth * 0.5)
	var mat: ParticleProcessMaterial = _emitter.process_material
	mat.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_BOX
	# A thin slab at the top of the volume; the fall is the particle's own.
	mat.emission_box_extents = Vector3(extent_px.x * 0.5, tile_h * 0.25, depth * 0.5)
	# Godot culls an emitter by this box, and the particles leave the emission
	# slab immediately -- without the full fall volume here, the rain vanishes
	# whenever the slab itself is off screen.
	_emitter.visibility_aabb = AABB(
		Vector3(-extent_px.x * 0.5, -top - tile_h, -depth * 0.5),
		Vector3(extent_px.x, top + tile_h * 2.0, depth))
	_emitter.amount = clampi(int(MAX_PARTICLES * wet), 24, MAX_PARTICLES)
	_emitter.emitting = true

func _ensure_emitter() -> void:
	if _emitter != null and is_instance_valid(_emitter):
		return
	_emitter = GPUParticles3D.new()
	_emitter.name = "Weather"
	# World space, like the field emitters: a camera pan must not drag the rain.
	_emitter.local_coords = false
	_emitter.emitting = false
	add_child(_emitter)

func _build_materials() -> void:
	if _rain_material != null:
		return
	_rain_material = ParticleProcessMaterial.new()
	_rain_material.direction = Vector3(0.0, -1.0, 0.0)
	_rain_material.spread = 2.0
	_rain_material.initial_velocity_min = 180.0
	_rain_material.initial_velocity_max = 260.0
	_rain_material.gravity = Vector3.ZERO

	_snow_material = ParticleProcessMaterial.new()
	_snow_material.direction = Vector3(0.0, -1.0, 0.0)
	_snow_material.spread = 25.0
	_snow_material.initial_velocity_min = 14.0
	_snow_material.initial_velocity_max = 30.0
	_snow_material.gravity = Vector3.ZERO

	_rain_draw = _draw_material()
	_snow_draw = _draw_material()
	_apply_kind_colour()
	_apply_wind()

func _apply_kind_colour() -> void:
	if _rain_draw == null:
		return
	# Acid is rain that is the wrong colour; the mechanics are identical.
	_rain_draw.albedo_color = Color(0.55, 0.95, 0.25, 0.5) if _kind == KIND_ACID \
		else Color(0.62, 0.72, 0.9, 0.45)
	_snow_draw.albedo_color = Color(0.95, 0.96, 1.0, 0.8)

func _draw_material() -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.blend_mode = BaseMaterial3D.BLEND_MODE_MIX
	# BILLBOARD_ENABLED keeps the quad's own +y up on screen, which is what makes
	# a tall thin quad read as a falling streak rather than a smear.
	mat.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	# Weather must not occlude the world, only wash it.
	mat.depth_draw_mode = BaseMaterial3D.DEPTH_DRAW_DISABLED
	mat.albedo_texture = _dot_texture()
	return mat

func _ensure_quad(rainlike: bool) -> QuadMesh:
	if rainlike:
		if _rain_quad == null:
			_rain_quad = QuadMesh.new()
			# A streak: tall and thin, in tile pixels.
			_rain_quad.size = Vector2(0.8, 7.0)
		return _rain_quad
	if _snow_quad == null:
		_snow_quad = QuadMesh.new()
		_snow_quad.size = Vector2(2.2, 2.2)
	return _snow_quad

## Wind leans the fall. Same vector as the sway shader and the smoke, so the
## whole frame agrees which way the weather is going; screen-space y is a ground
## direction in the stood-up world.
func _apply_wind() -> void:
	if _rain_material == null:
		return
	var w := Vector3(_wind.x, 0.0, _wind.y)
	_rain_material.gravity = w * 60.0
	_snow_material.gravity = w * 40.0

## A soft dot, built rather than shipped, exactly as the field particles do it.
func _dot_texture() -> Texture2D:
	if _dot != null:
		return _dot
	var g := Gradient.new()
	g.offsets = PackedFloat32Array([0.0, 1.0])
	g.colors = PackedColorArray([Color(1, 1, 1, 1), Color(1, 1, 1, 0)])
	_dot = GradientTexture2D.new()
	_dot.gradient = g
	_dot.fill = GradientTexture2D.FILL_RADIAL
	_dot.fill_from = Vector2(0.5, 0.5)
	_dot.fill_to = Vector2(1.0, 0.5)
	_dot.width = 16
	_dot.height = 16
	return _dot

## What is falling and how many drops carry it, for the probe and the first-frame
## report. kind NONE with the emitter parked is the normal clear-sky state.
func debug_stats() -> Dictionary:
	var active := _emitter != null and is_instance_valid(_emitter) and _emitter.emitting
	return {
		"kind": _kind,
		"active": active,
		"amount": _emitter.amount if active else 0,
	}
