extends Node2D
## Overmap view (T2.4) — paints the C++ overmap draw list.
##
## The overmap has its own tileset (the OVERMAP_TILES option, Larwick_Overmap by
## default) and its own draw list from src/godot_overmap_snapshot.*, but the draw
## mechanism is the same batched MultiMesh as MapView.

const TILE_SHADER := preload("res://shaders/map_tiles.gdshader")

## Ints per packed command; shares MapSnapshot::cmd_stride.
const CMD_STRIDE := 10

## Quarter-turn rotations, clockwise; see map_view.gd.
const ROTATION_ANGLES := [0.0, PI * 0.5, PI, -PI * 0.5]

var _host: Node
var _atlases: Array[Texture2D] = []
var _cmds: PackedInt32Array = PackedInt32Array()
var _tile_size: Vector2i = Vector2i(32, 32)
var _view_size: Vector2i = Vector2i.ZERO
var _atlases_loaded: bool = false
var _zoom: float = 1.0

var _quad: ArrayMesh
## "layer:atlas" -> MultiMeshInstance2D
var _batches: Dictionary = {}
var _generation: int = -1
var _area: Vector2 = Vector2.ZERO

func setup(host: Node) -> void:
	_host = host
	_atlases_loaded = false
	_atlases.clear()
	_cmds = PackedInt32Array()
	_generation = -1
	_clear_batches()
	queue_redraw()

func refresh() -> void:
	if _host == null or not _host.has_method("overmap_tileset_ready"):
		return
	if not _host.overmap_tileset_ready():
		return
	if not _atlases_loaded:
		_load_atlases()
	if not _atlases_loaded:
		return

	var generation: int = _host.get_overmap_generation()
	var area := get_viewport_rect().size
	if generation == _generation and area == _area:
		return
	_generation = generation
	_area = area

	_tile_size = _host.get_overmap_tile_size()
	_view_size = _host.get_overmap_view_size()
	_cmds = _host.get_overmap_draw_list()
	_update_zoom_and_camera()
	_rebuild_batches()
	queue_redraw()

func _load_atlases() -> void:
	_atlases.clear()
	var count: int = _host.get_overmap_atlas_count()
	if count <= 0:
		return
	for i in count:
		var img: Image = _host.get_overmap_atlas_image(i)
		if img == null or img.get_width() <= 0:
			push_warning("OvermapView: empty atlas %d" % i)
			_atlases.append(null)
			continue
		_atlases.append(ImageTexture.create_from_image(img))
	_atlases_loaded = _atlases.size() > 0
	_clear_batches()
	_generation = -1
	if _atlases_loaded:
		print("OvermapView: loaded ", _atlases.size(), " atlases tile=", _host.get_overmap_tile_size())

func _update_zoom_and_camera() -> void:
	if _view_size.x <= 0 or _view_size.y <= 0:
		return
	var area := get_viewport_rect().size
	if area.x < 2.0 or area.y < 2.0:
		return
	var map_w := float(_view_size.x * _tile_size.x)
	var map_h := float(_view_size.y * _tile_size.y)
	if map_w < 1.0 or map_h < 1.0:
		return
	# Fit the whole overmap window; unlike the map there is no reason to prefer
	# integer zoom, the sprites are read as a chart rather than as pixel art.
	_zoom = minf(area.x / map_w, area.y / map_h) * 0.96
	position = Vector2((area.x - map_w * _zoom) * 0.5, (area.y - map_h * _zoom) * 0.5)
	scale = Vector2(_zoom, _zoom)

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
	_quad.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {},
		Mesh.ARRAY_FLAG_USE_2D_VERTICES)
	return _quad

func _clear_batches() -> void:
	for node in _batches.values():
		if is_instance_valid(node):
			node.queue_free()
	_batches.clear()

func _batch_for(layer: int, atlas_i: int) -> MultiMeshInstance2D:
	var key := "%d:%d" % [layer, atlas_i]
	var existing = _batches.get(key)
	if existing != null and is_instance_valid(existing):
		return existing

	var tex: Texture2D = _atlases[atlas_i]
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_2D
	mm.use_custom_data = true
	mm.use_colors = true
	mm.mesh = _ensure_quad()

	var mat := ShaderMaterial.new()
	mat.shader = TILE_SHADER
	mat.set_shader_parameter("atlas_texel",
		Vector2(1.0 / float(tex.get_width()), 1.0 / float(tex.get_height())))

	var node := MultiMeshInstance2D.new()
	node.name = "OmBatch_%d_%d" % [layer, atlas_i]
	node.multimesh = mm
	node.texture = tex
	node.material = mat
	node.z_index = layer + 1
	add_child(node)
	_batches[key] = node
	return node

func _tile_transform(x: float, y: float, w: float, h: float, rotation: int) -> Transform2D:
	if rotation == 0:
		return Transform2D(Vector2(w, 0.0), Vector2(0.0, h), Vector2(x, y))
	var ang: float = ROTATION_ANGLES[rotation & 3]
	var c := cos(ang)
	var s := sin(ang)
	var basis_x := Vector2(c, s) * w
	var basis_y := Vector2(-s, c) * h
	var centre := Vector2(x + w * 0.5, y + h * 0.5)
	return Transform2D(basis_x, basis_y, centre - (basis_x + basis_y) * 0.5)

func _unpack_tint(packed: int) -> Color:
	return Color8(
		(packed >> 24) & 0xFF,
		(packed >> 16) & 0xFF,
		(packed >> 8) & 0xFF,
		packed & 0xFF
	)

func _rebuild_batches() -> void:
	var buckets: Dictionary = {}
	var n := _cmds.size()
	var i := 0
	while i + CMD_STRIDE - 1 < n:
		var atlas_i: int = _cmds[i]
		if atlas_i >= 0 and atlas_i < _atlases.size() and _atlases[atlas_i] != null:
			var key := "%d:%d" % [_cmds[i + 7], atlas_i]
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

	for key in buckets:
		var parts := (key as String).split(":")
		var layer := int(parts[0])
		var atlas_i := int(parts[1])
		var node := _batch_for(layer, atlas_i)
		var tex: Texture2D = _atlases[atlas_i]
		var inv_w := 1.0 / float(tex.get_width())
		var inv_h := 1.0 / float(tex.get_height())
		var offsets: PackedInt32Array = buckets[key]
		var mm := node.multimesh
		mm.instance_count = offsets.size()
		for slot in offsets.size():
			var o: int = offsets[slot]
			var src_w: int = _cmds[o + 3]
			var src_h: int = _cmds[o + 4]
			mm.set_instance_transform_2d(slot, _tile_transform(
				float(_cmds[o + 5]), float(_cmds[o + 6]),
				float(src_w), float(src_h), _cmds[o + 9]
			))
			mm.set_instance_custom_data(slot, Color(
				float(_cmds[o + 1]) * inv_w,
				float(_cmds[o + 2]) * inv_h,
				float(src_w) * inv_w,
				float(src_h) * inv_h
			))
			mm.set_instance_color(slot, _unpack_tint(_cmds[o + 8]))

func _draw() -> void:
	if _view_size.x > 0 and _view_size.y > 0:
		draw_rect(Rect2(Vector2.ZERO,
			Vector2(_view_size.x * _tile_size.x, _view_size.y * _tile_size.y)),
			Color(0.02, 0.02, 0.04))
