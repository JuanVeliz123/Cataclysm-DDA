extends Node3D
## Fire and smoke as particles, for the 3D backend (SP-6 under ADR-006 item 3D-1c).
##
## A port of `field_particles.gd`, and worth reading beside it: the emitter pool,
## the two process materials, the intensity-driven amounts and the conditions grade
## are the same decisions, and only what a 3D emitter needs differs. Keep them in
## step.
##
## What is genuinely different, and why:
##
##   - `GPUParticles3D` has no `modulate` -- that is a CanvasItem property. Colour
##     goes on the draw pass's material instead, and since the 2D version's
##     per-emitter `base` colour is decided by nothing but the kind, one material
##     per kind covers it. The per-emitter colour array it kept was already only
##     ever holding one of two values.
##   - The draw pass is an explicit quad and a billboard material, where a
##     `GPUParticles2D` just takes a texture.
##   - Up is +y. The 2D script says "-y is up on the map, and gravity pushes the
##     other way"; here the sign flips and so does the wind's.
##
## Emitters sit at the field layer's depth, which the caller passes in, so smoke
## drifts over the fire it came from and a creature walking through it still draws
## in front -- the same ordering the 2D backend gets from child order.

const FIELD_STRIDE := 4

const KIND_FIRE := 0
const KIND_SMOKE := 1

## Emitters above this are not built; a burning building can fill the screen.
const MAX_EMITTERS := 48

var _pool: Array[GPUParticles3D] = []
## The kind each pool slot is currently configured for, so the grade knows which
## material to hand it without re-reading the field list.
var _kinds: Array[int] = []
var _grade: Color = Color.WHITE
var _fire_material: ParticleProcessMaterial
var _smoke_material: ParticleProcessMaterial
## One draw material per kind, holding that kind's colour times the grade.
var _fire_draw: StandardMaterial3D
var _smoke_draw: StandardMaterial3D
var _quad: QuadMesh
var _dot: GradientTexture2D
var _wind: Vector2 = Vector2.ZERO
var _tile_size: Vector2i = Vector2i(32, 32)
## Depth the emitters sit at, from the caller's compacted depth map.
var _depth: float = 0.0
## Whether the world is stood up (3D-3), and the sine of the tilt that pre-stretches
## the ground. Both arrive from MapView3D, which owns the camera.
var _tilted: bool = false
var _sin_tilt: float = 1.0

func setup() -> void:
	_build_materials()

## Apply the presentation grade to the particles.
##
## The 3D backend still grades in the tile shader rather than as a pass over the
## world, so these nodes are outside it exactly as their 2D counterparts are, and
## for the same reason: their materials are their own. Approximated with a colour
## multiply rather than by duplicating the shader -- particles are soft, additive
## and already semi-transparent.
func set_conditions(daylight: float, precipitation: float, pain: float) -> void:
	var night := 1.0 - clampf(daylight, 0.0, 1.0)
	var wet := clampf(precipitation, 0.0, 1.0)
	var grade := Color.WHITE
	grade = grade.lerp(Color(0.62, 0.72, 1.0), night * 0.8)
	grade = grade.lerp(Color(0.82, 0.88, 1.0), wet * 0.5)
	# Rain also thins smoke: a plume does not stand up in a downpour.
	grade.a = 1.0 - wet * 0.35
	if grade != _grade:
		_grade = grade
		_apply_grade()

func _apply_grade() -> void:
	if _fire_draw == null:
		return
	# Fire is a light source, so it is graded far less than smoke is: a campfire
	# does not go blue at night, the air around it does.
	_fire_draw.albedo_color = Color(1.0, 0.75, 0.35, 0.9) \
		* Color.WHITE.lerp(_grade, 0.25)
	_smoke_draw.albedo_color = Color(0.72, 0.72, 0.75, 0.32) * _grade

## Tell the emitters which world they are in.
##
## Two things change when it stands up. Smoke that must not drift toward the camera in a
## flat world *must* drift along the ground in a stood-up one -- wind blows north and
## south, and in this frame that is the z axis -- so `particle_flag_disable_z` comes
## off. And the wind's screen-space y becomes a ground direction rather than a vertical
## one, which is the same reflection every placement here makes.
func set_tilted(tilted: bool, sin_tilt: float) -> void:
	var was := _tilted
	_tilted = tilted
	_sin_tilt = maxf(sin_tilt, 0.0001)
	if was == tilted or _fire_material == null:
		return
	_fire_material.particle_flag_disable_z = not tilted
	_smoke_material.particle_flag_disable_z = not tilted
	_apply_wind()

## Called by MapView3D after it rebuilds, with the frame's field list and the depth
## the field layer sits at.
func refresh(cmds: PackedInt32Array, tile_size: Vector2i, wind: Vector2,
		depth: float) -> void:
	_tile_size = tile_size
	_depth = depth
	if wind != _wind:
		_wind = wind
		_apply_wind()

	var used := 0
	var n := cmds.size()
	var i := 0
	while i + FIELD_STRIDE - 1 < n and used < MAX_EMITTERS:
		var x: int = cmds[i]
		var y: int = cmds[i + 1]
		var kind: int = cmds[i + 2]
		var intensity: int = cmds[i + 3]
		i += FIELD_STRIDE
		var cx := float(x) + float(tile_size.x) * 0.5
		var cy := float(y) + float(tile_size.y) * 0.5
		# Flat: the tile centre with y flipped, at the field layer's depth. Stood up:
		# a little above the floor of that tile, because smoke comes off the top of
		# what is burning rather than out of the ground.
		var at := Vector3(cx, float(tile_size.y) * 0.35, cy / _sin_tilt) if _tilted \
			else Vector3(cx, -cy, _depth)
		_configure(_emitter(used), used, kind, intensity, at)
		used += 1

	# Emitters beyond what this frame needs stop emitting rather than being freed:
	# the fields they were on go out and come back constantly.
	for j in range(used, _pool.size()):
		_pool[j].emitting = false

func _emitter(index: int) -> GPUParticles3D:
	while _pool.size() <= index:
		var p := GPUParticles3D.new()
		p.name = "Field_%d" % _pool.size()
		# World-space particles: moving the emitter to another tile then leaves the
		# existing puffs where they were, instead of dragging the whole plume across
		# the map when the pool slot is reused.
		p.local_coords = false
		p.draw_pass_1 = _ensure_quad()
		p.emitting = false
		add_child(p)
		_pool.append(p)
		_kinds.append(-1)
	return _pool[index]

func _configure(p: GPUParticles3D, index: int, kind: int, intensity: int,
		at: Vector3) -> void:
	p.position = at
	p.emitting = true
	if kind == KIND_FIRE:
		p.process_material = _fire_material
		p.material_override = _fire_draw
		p.lifetime = 0.7
		p.amount = clampi(6 * intensity, 6, 24)
	else:
		p.process_material = _smoke_material
		p.material_override = _smoke_draw
		p.lifetime = 2.2
		p.amount = clampi(3 * intensity, 3, 12)
	while _kinds.size() <= index:
		_kinds.append(-1)
	_kinds[index] = kind
	# Particles are sized against the tile, so they stay right at any zoom and for a
	# tileset whose tiles are not 32 pixels.
	var scale_to := float(_tile_size.y) / 32.0
	p.process_material.scale_min = (0.25 if kind == KIND_FIRE else 0.6) * scale_to
	p.process_material.scale_max = (0.7 if kind == KIND_FIRE else 1.6) * scale_to

func _build_materials() -> void:
	if _fire_material != null:
		return
	_fire_material = ParticleProcessMaterial.new()
	# The world is flat, so keep the particles in the plane their emitter is in;
	# otherwise a puff drifts toward the camera and out of its own depth band.
	_fire_material.particle_flag_disable_z = true
	_fire_material.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE_SURFACE
	_fire_material.emission_sphere_radius = 8.0
	# Embers rise, and +y is up here.
	_fire_material.direction = Vector3(0.0, 1.0, 0.0)
	_fire_material.spread = 25.0
	_fire_material.initial_velocity_min = 14.0
	_fire_material.initial_velocity_max = 34.0
	_fire_material.gravity = Vector3(0.0, 22.0, 0.0)
	_fire_material.color_ramp = _ramp([
		Color(1.0, 0.95, 0.6, 1.0), Color(1.0, 0.55, 0.15, 0.85),
		Color(0.6, 0.15, 0.05, 0.0),
	])

	_smoke_material = ParticleProcessMaterial.new()
	_smoke_material.particle_flag_disable_z = true
	_smoke_material.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE_SURFACE
	_smoke_material.emission_sphere_radius = 10.0
	_smoke_material.direction = Vector3(0.0, 1.0, 0.0)
	_smoke_material.spread = 45.0
	_smoke_material.initial_velocity_min = 4.0
	_smoke_material.initial_velocity_max = 12.0
	_smoke_material.gravity = Vector3(0.0, 6.0, 0.0)
	_smoke_material.color_ramp = _ramp([
		Color(0.5, 0.5, 0.52, 0.0), Color(0.55, 0.55, 0.58, 0.55),
		Color(0.6, 0.6, 0.62, 0.0),
	])

	_fire_draw = _draw_material(true)
	_smoke_draw = _draw_material(false)
	_apply_grade()
	_apply_wind()

## The material a particle quad is drawn with: unshaded, because the light in a
## flame is the flame, and billboarded so a puff faces the camera whatever the
## camera does later.
func _draw_material(fire: bool) -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	# Fire adds light; smoke covers what is behind it.
	mat.blend_mode = BaseMaterial3D.BLEND_MODE_ADD if fire else BaseMaterial3D.BLEND_MODE_MIX
	mat.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	# A puff must not occlude what is behind it, only tint it.
	mat.depth_draw_mode = BaseMaterial3D.DEPTH_DRAW_DISABLED
	mat.albedo_texture = _dot_texture()
	mat.vertex_color_use_as_albedo = true
	return mat

func _ensure_quad() -> QuadMesh:
	if _quad != null:
		return _quad
	_quad = QuadMesh.new()
	# One tile-pixel across; the process material's scale does the rest, exactly as
	# the 2D version scales its texture.
	_quad.size = Vector2(1.0, 1.0)
	return _quad

## Wind pushes both plumes sideways. Same vector the sway shader uses, so the grass
## and the smoke agree about which way the weather is going -- with y flipped, since
## the wind arrives in screen space.
func _apply_wind() -> void:
	if _fire_material == null:
		return
	# Screen-space wind becomes a world direction: sideways is x either way, and the
	# other component is height when the world is flat and ground when it stands up.
	var w := Vector3(_wind.x, 0.0, _wind.y) if _tilted else Vector3(_wind.x, -_wind.y, 0.0)
	_fire_material.gravity = Vector3(0.0, 22.0, 0.0) + w * 30.0
	_smoke_material.gravity = Vector3(0.0, 6.0, 0.0) + w * 45.0

func _ramp(colors: Array) -> GradientTexture1D:
	var g := Gradient.new()
	g.offsets = PackedFloat32Array([0.0, 0.45, 1.0])
	g.colors = PackedColorArray(colors)
	var t := GradientTexture1D.new()
	t.gradient = g
	return t

## A soft round dot, built rather than shipped so there is no art dependency.
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

## How many emitters are alive, for the probe. The pool outlives the fields, so its
## size says nothing; what matters is how many are emitting.
func active_emitters() -> int:
	var n := 0
	for p in _pool:
		if is_instance_valid(p) and p.emitting:
			n += 1
	return n
