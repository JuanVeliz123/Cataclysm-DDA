extends Node2D
## Fire and smoke as particles (SP-6).
##
## These are the two things on the map that move continuously, and a tileset
## animates them with a handful of frames at whatever rate its author chose.
## Particles cost no art, run at the frame rate rather than the turn rate, and
## can be driven by the field's intensity -- which the game already tracks and a
## spritesheet has no way to express.
##
## The field sprite is still drawn underneath: Ultica's fire is good art, and
## replacing it wholesale would be a downgrade. What this adds is the motion the
## sprite cannot have -- embers rising off it, smoke drifting on the wind -- and
## the flicker in the tile shader is the light the same fire casts.
##
## A child of MapView, so the map's zoom and camera transform apply for free.

## Ints per packed field: dest x/y, kind, intensity.
## Must match MapSnapshot::field_stride in src/godot_map_snapshot.h.
const FIELD_STRIDE := 4

const KIND_FIRE := 0
const KIND_SMOKE := 1

## Emitters above this are not built. A burning building can fill the screen
## with fire tiles, and past a certain point the extra emitters cost frames
## without changing what anyone sees.
const MAX_EMITTERS := 48

var _pool: Array[GPUParticles2D] = []
## Each emitter's own colour, before the conditions grade is folded in. Kept
## separately so the grade can be re-applied without compounding.
var _base_modulate: Array[Color] = []
var _grade: Color = Color.WHITE
var _fire_material: ParticleProcessMaterial
var _smoke_material: ParticleProcessMaterial
var _dot: GradientTexture2D
var _wind: Vector2 = Vector2.ZERO
var _tile_size: Vector2i = Vector2i(32, 32)

func setup() -> void:
	# Position in the map's draw order -- above the field sprites, below monsters,
	# so smoke drifts over the fire it comes from but does not hide the zombie
	# walking through it -- is MapView's to decide, and it does so by child order
	# rather than z_index. Setting a z here is what put smoke over open menus.
	_build_materials()

## Apply the presentation grade to the particles.
##
## The grade lives in the tile shader rather than in a full-screen pass, because
## a CanvasLayer over the viewport would tint the sidebar and menus too (see
## ADR-004). The cost is that these nodes have their own materials and are not
## reached by it, so without this smoke keeps its daylight colour at midnight in
## a downpour while the ground it sits on does not.
##
## Approximated with modulate rather than duplicating the shader: particles are
## soft, additive and already semi-transparent, so the difference between a
## correct grade and a tinted one is not visible on them.
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
	for i in _pool.size():
		var base: Color = _base_modulate[i] if i < _base_modulate.size() else Color.WHITE
		_pool[i].modulate = base * _grade

## Called by MapView after it rebuilds, with the frame's field list.
func refresh(cmds: PackedInt32Array, tile_size: Vector2i, wind: Vector2) -> void:
	_tile_size = tile_size
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
		_configure(_emitter(used), kind, intensity,
			Vector2(float(x) + tile_size.x * 0.5, float(y) + tile_size.y * 0.5))
		used += 1

	# Emitters beyond what this frame needs stop emitting rather than being
	# freed: the fields they were on go out and come back constantly, and
	# rebuilding a GPUParticles2D each time would cost more than keeping it.
	for j in range(used, _pool.size()):
		_pool[j].emitting = false

func _emitter(index: int) -> GPUParticles2D:
	while _pool.size() <= index:
		var p := GPUParticles2D.new()
		p.name = "Field_%d" % _pool.size()
		# World-space particles: moving the emitter to a different tile then
		# leaves the existing puffs where they were, instead of dragging the
		# whole plume across the map when the pool slot is reused.
		p.local_coords = false
		p.texture = _dot_texture()
		p.emitting = false
		add_child(p)
		_pool.append(p)
	return _pool[index]

func _configure(p: GPUParticles2D, kind: int, intensity: int, at: Vector2) -> void:
	p.position = at
	p.emitting = true
	var base: Color
	if kind == KIND_FIRE:
		p.process_material = _fire_material
		p.lifetime = 0.7
		p.amount = clampi(6 * intensity, 6, 24)
		base = Color(1.0, 0.75, 0.35, 0.9)
	else:
		p.process_material = _smoke_material
		p.lifetime = 2.2
		p.amount = clampi(3 * intensity, 3, 12)
		base = Color(0.72, 0.72, 0.75, 0.32)
	var index := _pool.find(p)
	while _base_modulate.size() <= index:
		_base_modulate.append(Color.WHITE)
	if index >= 0:
		_base_modulate[index] = base
	# Fire is a light source, so it is graded far less than smoke is: a campfire
	# does not go blue at night, the air around it does.
	p.modulate = base * (Color.WHITE.lerp(_grade, 0.25) if kind == KIND_FIRE else _grade)
	# Particles are sized against the tile, so they stay right at any zoom and
	# for a tileset whose tiles are not 32 pixels.
	var scale_to := float(_tile_size.y) / 32.0
	p.process_material.scale_min = (0.25 if kind == KIND_FIRE else 0.6) * scale_to
	p.process_material.scale_max = (0.7 if kind == KIND_FIRE else 1.6) * scale_to

func _build_materials() -> void:
	if _fire_material != null:
		return
	_fire_material = ParticleProcessMaterial.new()
	_fire_material.particle_flag_disable_z = true
	_fire_material.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE_SURFACE
	_fire_material.emission_sphere_radius = 8.0
	# Embers rise. -y is up on the map, and gravity pushes the other way.
	_fire_material.direction = Vector3(0.0, -1.0, 0.0)
	_fire_material.spread = 25.0
	_fire_material.initial_velocity_min = 14.0
	_fire_material.initial_velocity_max = 34.0
	_fire_material.gravity = Vector3(0.0, -22.0, 0.0)
	_fire_material.color_ramp = _ramp([
		Color(1.0, 0.95, 0.6, 1.0), Color(1.0, 0.55, 0.15, 0.85),
		Color(0.6, 0.15, 0.05, 0.0),
	])

	_smoke_material = ParticleProcessMaterial.new()
	_smoke_material.particle_flag_disable_z = true
	_smoke_material.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_SPHERE_SURFACE
	_smoke_material.emission_sphere_radius = 10.0
	_smoke_material.direction = Vector3(0.0, -1.0, 0.0)
	_smoke_material.spread = 45.0
	_smoke_material.initial_velocity_min = 4.0
	_smoke_material.initial_velocity_max = 12.0
	_smoke_material.gravity = Vector3(0.0, -6.0, 0.0)
	_smoke_material.color_ramp = _ramp([
		Color(0.5, 0.5, 0.52, 0.0), Color(0.55, 0.55, 0.58, 0.55),
		Color(0.6, 0.6, 0.62, 0.0),
	])
	_apply_wind()

## Wind pushes both plumes sideways. Same vector the sway shader uses, so the
## grass and the smoke agree about which way the weather is going.
func _apply_wind() -> void:
	if _fire_material == null:
		return
	var w := Vector3(_wind.x, _wind.y, 0.0)
	_fire_material.gravity = Vector3(0.0, -22.0, 0.0) + w * 30.0
	_smoke_material.gravity = Vector3(0.0, -6.0, 0.0) + w * 45.0

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
