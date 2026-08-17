extends Node3D
## Creatures drawn as meshes instead of as sprites (ADR-006's mesh amendment, 3D-7c).
##
## The registry is a directory convention and a cache: a creature whose id has a mesh under
## `res://meshes/creatures/` is drawn as that mesh, and one whose id has nothing is left to
## its sprite. **With no meshes present, nothing here does anything** -- which is the whole
## design of this step. The migration is meant to be partial for a long time, so the
## fallback is not an error path, it is the normal case.
##
## Why this needs a channel of its own: a `map_draw_cmd` is an atlas sub-rect and a
## destination. That is everything a sprite needs and nothing a mesh can use -- picking a
## model for a zombie means knowing that it is a zombie -- so `CDDAHost::get_creatures()`
## publishes identity beside the draw list rather than smuggling it into the command.
##
## Sprites are suppressed by **tile**, not by command, for the same reason: the draw list
## cannot say which creature a command belongs to. CDDA allows one creature per tile, so a
## tile is an identity, and `suppressed_tiles()` is what the tile pass uses to leave a
## meshed creature's sprite undrawn.

## Where a mesh for a creature id is looked for. `<id>.tres`, `<id>.res`, `<id>.glb` and
## `<id>.obj` are tried in that order -- the first two because a Mesh resource can be saved
## directly, the others because they are what an art pipeline produces.
const MESH_DIR := "res://meshes/creatures"
const MESH_EXTENSIONS := [".tres", ".res", ".glb", ".obj"]

## Facing: the sprite convention is a mirror, and a mesh has a back, so a mesh turns instead.
## Degrees to rotate a mesh that is facing left rather than right.
const FLIP_DEGREES := 180.0

## kind, from creature_record: 0 monster, 1 NPC, 2 the avatar.
const KIND_AVATAR := 2

var _host: Node
## id -> Mesh, or null for "looked and there is nothing". Cached both ways, because a miss
## is the common case and probing the filesystem for every zombie every turn would be a
## filesystem call per zombie per turn.
var _registry: Dictionary = {}
## Pooled MeshInstance3D nodes, reused across frames like every other pool here.
var _pool: Array[MeshInstance3D] = []
var _used: int = 0
## Tile keys whose creature is drawn as a mesh, so its sprite can be left out.
var _suppressed: Dictionary = {}

func setup(host: Node) -> void:
	_host = host

## Tiles whose creature is being drawn as a mesh this frame, keyed as in `tile_key`.
func suppressed_tiles() -> Dictionary:
	return _suppressed

## Pack a tile's pixel position into a key. The channel publishes feet in pixels and the
## draw list publishes sprite corners, so both sides quantise to the tile to meet.
static func tile_key(px: float, py: float, tile: Vector2i) -> int:
	var tx := int(floor(px / float(maxi(1, tile.x))))
	var ty := int(floor(py / float(maxi(1, tile.y))))
	return tx * 4096 + ty

## Place a mesh for every creature that has one.
##
## @param feet_to_world converts a creature's feet, in view-relative pixels, to a world
##        position. Passed in rather than recomputed because the placement rules belong to
##        the backend that owns the camera, and there is exactly one copy of them there.
func refresh(creatures: Array, tile: Vector2i, feet_to_world: Callable,
		height_scale: float) -> void:
	_suppressed.clear()
	_used = 0
	for entry in creatures:
		var id := str((entry as Dictionary).get("id", ""))
		if id.is_empty():
			continue
		var mesh := _mesh_for(id)
		if mesh == null:
			continue
		var px := float(entry.get("x", 0))
		var py := float(entry.get("y", 0))
		var node := _instance(_used)
		node.mesh = mesh
		node.position = feet_to_world.call(px, py, int(entry.get("z_below", 0)))
		# A sprite faces left by being mirrored; a mesh has a back, so it turns.
		node.rotation = Vector3(0.0, deg_to_rad(FLIP_DEGREES) \
			if bool(entry.get("flip", false)) else 0.0, 0.0)
		# The world is anisotropic -- height is pre-stretched -- so a mesh has to be
		# stretched with it or it will be the only thing in the scene that is not.
		node.scale = Vector3(1.0, height_scale, 1.0)
		node.visible = true
		_suppressed[tile_key(px, py - 1.0, tile)] = true
		_used += 1
	for i in range(_used, _pool.size()):
		_pool[i].visible = false

func _instance(index: int) -> MeshInstance3D:
	while _pool.size() <= index:
		var node := MeshInstance3D.new()
		node.name = "Creature_%d" % _pool.size()
		node.visible = false
		# A mesh is a real occluder: it casts instead of the capsule proxy standing in for
		# the same creature, and instead of the sprite that is no longer drawn.
		node.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
		add_child(node)
		_pool.append(node)
	return _pool[index]

## The mesh for an id, or null. Probed once per id per session.
func _mesh_for(id: String) -> Mesh:
	if _registry.has(id):
		return _registry[id]
	var found: Mesh = null
	for ext in MESH_EXTENSIONS:
		var path := "%s/%s%s" % [MESH_DIR, id, ext]
		if not ResourceLoader.exists(path):
			continue
		var res := ResourceLoader.load(path)
		if res is Mesh:
			found = res
		elif res is PackedScene:
			# What an imported .glb is: a scene whose first MeshInstance3D holds the mesh.
			# Taking the mesh rather than instancing the scene keeps every creature a single
			# node here, which is what the pooling is for.
			var scene := (res as PackedScene).instantiate()
			for child in scene.get_children():
				if child is MeshInstance3D:
					found = (child as MeshInstance3D).mesh
					break
			scene.queue_free()
		if found != null:
			print("[mesh] %s -> %s" % [id, path])
			break
	# Cached either way: a miss is the common case for a long time yet, and probing the
	# filesystem per creature per turn would be the wrong kind of thorough.
	_registry[id] = found
	return found

## How many creatures were drawn as meshes, and how many ids have been looked up, for the
## first-frame report. A registry of many ids and zero meshes is the expected state.
func debug_stats() -> Dictionary:
	var meshes := 0
	for id in _registry:
		meshes += 1 if _registry[id] != null else 0
	return { "drawn": _used, "ids_seen": _registry.size(), "ids_with_meshes": meshes }
