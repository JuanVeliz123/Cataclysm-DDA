extends Node3D
## Creatures drawn as meshes instead of as sprites (ADR-006's mesh amendment, 3D-7c),
## now with locomotion and clips.
##
## The registry is a directory convention and a cache: a creature whose id has art under
## `res://meshes/creatures/` is drawn as that art, and one whose id has nothing is left to
## its sprite. **With no assets present, nothing here does anything** -- which is the whole
## design of this step. The migration is meant to be partial for a long time, so the
## fallback is not an error path, it is the normal case.
##
## Two kinds of art, by extension:
##   - `<id>.scn` -- a PackedScene with a Skeleton3D and an AnimationPlayer holding
##     `idle`/`walk`/`run` (looping) and `attack`/`hit`/`die` (one-shots). Animated.
##   - `<id>.tres`/`.res`/`.glb`/`.obj` -- a bare Mesh. Static, but it still walks:
##     position interpolation below applies to both, so even the T-pose slides
##     between tiles instead of teleporting.
##
## **The game owns position; this node owns presentation time.** CDDA moves creatures a
## whole tile at a time, once per turn, at whatever rate turns happen to pass -- many per
## second while travelling, one per minute while the player thinks. A walk that *reads*
## as a walk therefore has to be manufactured here: on a position change the node tweens
## to the new tile over a fraction of a second, plays `walk` while it does, and settles
## to `idle`. Turns arriving faster than tweens finish retarget the live tween rather
## than queueing a backlog, and a jump past SNAP_TILES is a teleport and snaps.
##
## The one trap in that plan, learned before it shipped: **movement must be detected in
## world space, never in view space.** The channel publishes feet in view-relative
## pixels, and the published origin recentres on the avatar -- so when the avatar moves,
## every stationary creature's view position shifts by the same amount, and a naive
## delta would send the whole map's creatures on little synchronised walks. Deltas are
## taken against absolute map pixels (origin + view position); a pure origin shift moves
## the node instantly with the world it is glued to, exactly as the tiles themselves do.
##
## Why this needs a channel of its own: a `map_draw_cmd` is an atlas sub-rect and a
## destination. That is everything a sprite needs and nothing a mesh can use -- picking a
## model and a clip for a zombie means knowing that it is a zombie, and telling this
## zombie from that one across frames means the `uid` the channel carries (API 24).
## Against an older library every uid reads 0 and the fallback is a per-tile pseudo-id:
## no movement is ever detected and behaviour degrades to the static placement this file
## always had.
##
## Sprites are suppressed by **tile**, not by command, for the same reason: the draw list
## cannot say which creature a command belongs to. CDDA allows one creature per tile, so a
## tile is an identity, and `suppressed_tiles()` is what the tile pass uses to leave a
## meshed creature's sprite undrawn.

## Where art for a creature id is looked for. `.scn` first -- an animated scene beats a
## static mesh for the same id -- then the mesh forms.
const MESH_DIR := "res://meshes/creatures"
const SCENE_EXTENSION := ".scn"
const MESH_EXTENSIONS := [".tres", ".res", ".glb", ".obj"]

## Facing: the sprite convention is a mirror, and a mesh has a back, so a mesh turns.
## Degrees to rotate a mesh that is facing left rather than right (flat world, or idle).
const FLIP_DEGREES := 180.0

## kind, from creature_record: 0 monster, 1 NPC, 2 the avatar.
const KIND_AVATAR := 2

## How long one tile of movement takes on screen, by gait. Presentation numbers, not
## simulation ones (VER-1): long enough to read as a step, short enough that a monster
## moving every turn never falls behind the turns.
const WALK_TWEEN_S := 0.22
const RUN_TWEEN_S := 0.14
## A step past this many tiles in one turn is not a step -- stairs, a vehicle, a debug
## teleport -- and snaps rather than gliding across the map.
const SNAP_TILES := 2.5
## How fast a mesh turns to face its movement, radians per second of lerp weight.
const TURN_SPEED := 12.0
## Once a tween lands, wait this long before settling into `idle` -- a creature walking
## a tile per turn retargets before this expires and never flickers idle mid-stride.
const IDLE_GRACE_S := 0.12
## Crossfade for loop changes and one-shots. Rigid placeholder rigs tolerate anything;
## real art may want it tuned (VER-1).
const BLEND_S := 0.15

var _host: Node
## id -> {"scene": PackedScene} or {"mesh": Mesh}, or {} for "looked, nothing there".
## An EMPTY dictionary, never null: the lookup's return type is Dictionary, and a
## typed GDScript function that returns null does not return null -- it raises a
## script error and hands the caller a default-constructed value, which made the
## null-check downstream dead code and corrupted every instance spawned after the
## first artless creature. Cached both ways, because a miss is the common case and
## probing the filesystem per zombie per turn would be the wrong kind of thorough.
var _registry: Dictionary = {}
## uid -> the live instance record; see _spawn for the fields.
var _active: Dictionary = {}
## Tile keys whose creature is drawn as a mesh, so its sprite can be left out.
var _suppressed: Dictionary = {}
var _tile: Vector2i = Vector2i(32, 32)
var _feet_to_world: Callable
var _height_scale: float = 1.0
var _tilted: bool = false
var _sin_tilt: float = 1.0
## Clip name -> times started, for the probe: "the attack clip has never once played"
## must be a number, not an impression (the dead-frame-boundary rule).
var _clips_played: Dictionary = {}
## The avatar's uid, when one has been seen (creature_record kind 2), so the
## camera can ride its tween. Zero until then.
var _avatar_uid: int = 0

func setup(host: Node) -> void:
	_host = host
	set_process(true)

## Tiles whose creature is being drawn as a mesh this frame, keyed as in `tile_key`.
func suppressed_tiles() -> Dictionary:
	return _suppressed

## Pack a tile's pixel position into a key. The channel publishes feet in pixels and the
## draw list publishes sprite corners, so both sides quantise to the tile to meet.
static func tile_key(px: float, py: float, tile: Vector2i) -> int:
	var tx := int(floor(px / float(maxi(1, tile.x))))
	var ty := int(floor(py / float(maxi(1, tile.y))))
	return tx * 4096 + ty

## Which world this is, from the backend that owns the camera. Facing-by-yaw only means
## anything stood up; flat, a yaw spins the model in the screen plane, so the flat world
## keeps the sprite's mirror convention.
func set_tilted(tilted: bool, sin_tilt: float) -> void:
	_tilted = tilted
	_sin_tilt = maxf(sin_tilt, 0.0001)

## Place or move an instance for every creature that has art.
##
## @param feet_to_world converts feet in view-relative pixels to a world position; owned
##        by the backend because the placement rules belong to whoever owns the camera.
## @param origin_px the published view origin in map pixels -- what turns view-relative
##        feet into the absolute positions movement is detected against.
func refresh(creatures: Array, tile: Vector2i, feet_to_world: Callable,
		height_scale: float, origin_px: Vector2) -> void:
	_tile = tile
	_feet_to_world = feet_to_world
	_height_scale = height_scale
	_suppressed.clear()
	for uid in _active:
		_active[uid]["seen"] = false
	for entry in creatures:
		var id := str((entry as Dictionary).get("id", ""))
		if id.is_empty():
			continue
		var res := _resource_for(id)
		if res.is_empty():
			continue
		var px := float(entry.get("x", 0))
		var py := float(entry.get("y", 0))
		var z_below := int(entry.get("z_below", 0))
		var uid := int(entry.get("uid", 0))
		if uid == 0:
			# An old library publishes no identity. A tile is one, for one frame:
			# movement can never be detected, which degrades to static placement.
			uid = tile_key(px, py - 1.0, tile)
		var world_px := origin_px + Vector2(px, py)
		var target := _feet_to_world.call(px, py, z_below) as Vector3
		if int(entry.get("kind", -1)) == KIND_AVATAR:
			_avatar_uid = uid
		var inst: Dictionary = _active.get(uid, {})
		if inst.is_empty():
			inst = _spawn(uid, id, res, target, bool(entry.get("flip", false)))
			_active[uid] = inst
		else:
			_move(inst, px, py, z_below, world_px, target,
				int(entry.get("move_mode", 0)), bool(entry.get("flip", false)))
		inst["seen"] = true
		inst["world_px"] = world_px
		inst["view_px"] = Vector2(px, py)
		inst["z_below"] = z_below
		_suppressed[tile_key(px, py - 1.0, tile)] = true
	# Creatures that left the channel: dead ones finish their `die` clip where they
	# fell; the rest walked out of view and are simply released.
	var gone: Array = []
	for uid in _active:
		var inst: Dictionary = _active[uid]
		if not inst["seen"] and not inst["dying"]:
			gone.append(uid)
	for uid in gone:
		_release(uid)

func _spawn(uid: int, id: String, res: Dictionary, at: Vector3, flip: bool) -> Dictionary:
	var node: Node3D
	var anim: AnimationPlayer = null
	if res.has("scene"):
		node = (res["scene"] as PackedScene).instantiate() as Node3D
		anim = _find_anim(node)
	else:
		var mi := MeshInstance3D.new()
		mi.mesh = res["mesh"]
		# A mesh is a real occluder: it casts instead of the capsule proxy standing
		# in for the same creature, and instead of the sprite no longer drawn.
		mi.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
		node = mi
	node.name = "Creature_%d" % uid
	node.position = at
	# The world is anisotropic -- height is pre-stretched -- so a mesh is stretched
	# with it or it is the only thing in the scene that is not. On the node above
	# any skeleton, where it stays a display transform and never a skinning input.
	node.scale = Vector3(1.0, _height_scale, 1.0)
	node.rotation = Vector3(0.0, deg_to_rad(FLIP_DEGREES) if flip else 0.0, 0.0)
	add_child(node)
	var inst := {
		"node": node, "anim": anim, "id": id,
		"world_px": Vector2.ZERO, "view_px": Vector2.ZERO, "z_below": 0,
		"from": at, "to": at, "t": 0.0, "dur": 0.0, "moving": false,
		"yaw_target": node.rotation.y,
		"state": "idle", "oneshot_left": 0.0, "idle_grace": 0.0,
		"dying": false, "die_left": 0.0, "seen": true,
	}
	_play_loop(inst, "idle")
	return inst

## Movement, split into what the creature did and what the camera did.
##
## In the frame the channel publishes, the creature's PREVIOUS location sits at
## `view_px_new - world_delta`, whatever the origin did -- so both ends of the step can
## be expressed in today's frame and yesterday's frame never needs converting. A pure
## origin shift (the avatar moved, this creature did not) makes world_delta zero and
## the node just follows the world it is glued to, mid-tween included.
func _move(inst: Dictionary, px: float, py: float, z_below: int, world_px: Vector2,
		target: Vector3, move_mode: int, flip: bool) -> void:
	var node: Node3D = inst["node"]
	node.scale = Vector3(1.0, _height_scale, 1.0)
	if inst["dying"]:
		node.position = target
		return
	var world_delta: Vector2 = world_px - inst["world_px"]
	var tiles := world_delta.length() / float(maxi(1, _tile.x))
	var z_changed: bool = z_below != int(inst["z_below"])
	if tiles < 0.01:
		# Only the view moved (or nothing did). Shift the node -- and any live
		# tween's endpoints -- by however far the frame slid under it.
		var shift: Vector3 = target - (inst["to"] as Vector3)
		if inst["moving"]:
			inst["from"] = (inst["from"] as Vector3) + shift
			inst["to"] = target
			node.position += shift
		else:
			node.position = target
			inst["to"] = target
		return
	if tiles > SNAP_TILES or z_changed:
		# Not a step: stairs, a vehicle, a teleport. Pretending otherwise sends a
		# mesh gliding through walls across half the screen.
		inst["moving"] = false
		node.position = target
		inst["to"] = target
		_settle(inst)
		return
	# A step (or a couple, coalesced): tween from where the previous position sits
	# in TODAY'S frame -- or from wherever the node is mid-flight, carried across
	# the origin shift -- to the new tile.
	var prev_view := Vector2(px, py) - world_delta
	var from: Vector3 = _feet_to_world.call(prev_view.x, prev_view.y, z_below)
	if inst["moving"]:
		# Retarget a live tween: `from` is the old goal expressed in TODAY'S frame
		# and inst["to"] is the same point in yesterday's, so their difference is
		# exactly how far the frame slid underneath the node. Carry the node by it,
		# then tween from wherever it visually is -- continuous, no rubber-band.
		var frame_slide: Vector3 = from - (inst["to"] as Vector3)
		node.position += frame_slide
		inst["from"] = node.position
	else:
		inst["from"] = from
		node.position = from
	inst["to"] = target
	inst["t"] = 0.0
	var running: bool = move_mode == 1 or tiles >= 1.8
	# The tween paces itself to the PLAYER, not to a constant: a step arriving
	# every 0.1s must cross its tile in 0.1s or the body falls ever further
	# behind the keys, which is what fast tapping looked like. The gap between
	# this step and the last is the honest speed; the constants are the ceiling
	# for a leisurely stroll, the floor keeps a burst from teleporting.
	var now_ms := Time.get_ticks_msec()
	var gap := float(now_ms - int(inst.get("last_move_ms", 0))) / 1000.0
	inst["last_move_ms"] = now_ms
	var base := RUN_TWEEN_S if running else WALK_TWEEN_S
	inst["dur"] = clampf(gap, 0.08, base) * maxf(1.0, tiles)
	inst["moving"] = true
	inst["idle_grace"] = IDLE_GRACE_S
	if inst["oneshot_left"] <= 0.0:
		_play_loop(inst, "run" if running else "walk")
		# And the legs pace themselves to the tween: the clip was authored for
		# one unhurried cycle, so a body crossing tiles twice as fast plays it
		# twice as fast, up to a cap past which it reads as flailing (VER-1).
		var anim: AnimationPlayer = inst["anim"]
		if anim != null and is_instance_valid(anim):
			anim.speed_scale = clampf(base / maxf(0.05, float(inst["dur"]) / maxf(1.0, tiles)),
				0.6, 2.5)
	# Face the way it is going, in GAME space, not world space: the stood-up
	# ground is pre-stretched 1/sin along its rows, and a yaw computed from the
	# stretched delta skews every direction toward east-west -- due south read
	# as south-east, which is exactly how it was reported. The mesh node itself
	# is not ground-stretched (only its height is), so the game-space angle is
	# the right one to stand it at. Flat, a yaw is a spin in the screen plane,
	# so the flat world keeps the sprite's mirror.
	if _tilted:
		if world_delta.length_squared() > 0.0001:
			inst["yaw_target"] = atan2(world_delta.x, world_delta.y)
	else:
		inst["yaw_target"] = deg_to_rad(FLIP_DEGREES) if flip else 0.0

## Hit, death and swing events, forwarded by MapView3D from the anim channel.
## kind 0: `hit` on whoever was struck -- and only that. The attack clip rides
##         the swing, which fires whether or not the blow lands; playing it here
##         too would restart it mid-motion on every connecting blow.
## kind 1: `die` on the fallen; the node stays for the clip and is then released.
## kind 2: `attack` on whoever swung (a swing carries no flinch and no lunge).
func on_hit_event(attacker_uid: int, target_uid: int, kind: int) -> void:
	if kind == 1:
		var inst: Dictionary = _active.get(target_uid, {})
		if inst.is_empty() or inst["dying"]:
			return
		inst["dying"] = true
		inst["moving"] = false
		inst["die_left"] = _play_oneshot(inst, "die", 0.9)
		return
	if kind == 2:
		var atk: Dictionary = _active.get(attacker_uid, {})
		if not atk.is_empty() and not atk["dying"]:
			atk["oneshot_left"] = _play_oneshot(atk, "attack", 0.4)
		return
	var tgt: Dictionary = _active.get(target_uid, {})
	if not tgt.is_empty() and not tgt["dying"]:
		tgt["oneshot_left"] = _play_oneshot(tgt, "hit", 0.25)

## Presentation time: tweens, one-shot clocks, settling, turning. All of it here and
## none of it in refresh, because refresh happens per turn and this happens per frame.
func _process(delta: float) -> void:
	if _active.is_empty():
		return
	var done: Array = []
	for uid in _active:
		var inst: Dictionary = _active[uid]
		var node: Node3D = inst["node"]
		if not is_instance_valid(node):
			done.append(uid)
			continue
		if inst["dying"]:
			inst["die_left"] = float(inst["die_left"]) - delta
			if float(inst["die_left"]) <= 0.0:
				done.append(uid)
			continue
		if inst["moving"]:
			inst["t"] = float(inst["t"]) + delta
			var u := clampf(float(inst["t"]) / maxf(0.001, float(inst["dur"])), 0.0, 1.0)
			node.position = (inst["from"] as Vector3).lerp(inst["to"], u)
			if u >= 1.0:
				inst["moving"] = false
		elif float(inst["idle_grace"]) > 0.0 and inst["oneshot_left"] <= 0.0:
			# Between steps: hold the walk for a beat so a tile-per-turn stroll
			# does not flicker idle at every boundary.
			inst["idle_grace"] = float(inst["idle_grace"]) - delta
			if float(inst["idle_grace"]) <= 0.0:
				_settle(inst)
		if float(inst["oneshot_left"]) > 0.0:
			inst["oneshot_left"] = float(inst["oneshot_left"]) - delta
			if float(inst["oneshot_left"]) <= 0.0:
				_settle(inst)
		# Turn toward the movement, gently; idle keeps whatever it last faced.
		node.rotation.y = lerp_angle(node.rotation.y, float(inst["yaw_target"]),
			minf(1.0, TURN_SPEED * delta))
	for uid in done:
		_release(uid)

## Back to the loop that matches what the creature is doing now. Settling into
## idle also hands the clip its authored tempo back -- the walk may have been
## sped to match the player's tapping, and a sped-up idle reads as twitching.
func _settle(inst: Dictionary) -> void:
	if not inst["moving"]:
		var anim: AnimationPlayer = inst["anim"]
		if anim != null and is_instance_valid(anim):
			anim.speed_scale = 1.0
	_play_loop(inst, "walk" if inst["moving"] else "idle")

func _release(uid: int) -> void:
	var inst: Dictionary = _active.get(uid, {})
	if not inst.is_empty():
		var node: Node3D = inst["node"]
		if is_instance_valid(node):
			node.queue_free()
	_active.erase(uid)

## Start a looping clip if the rig has it and is not already in it.
func _play_loop(inst: Dictionary, clip: String) -> void:
	var anim: AnimationPlayer = inst["anim"]
	if anim == null or not is_instance_valid(anim) or not anim.has_animation(clip):
		return
	if inst["state"] == clip and anim.is_playing():
		return
	inst["state"] = clip
	anim.play(clip, BLEND_S)
	_clips_played[clip] = int(_clips_played.get(clip, 0)) + 1

## Start a one-shot; returns how long presentation time should hold it. Rigs without
## the clip still get the hold, so a static mesh "attacks" by simply pausing -- wrong
## in a way nobody sees, and cheaper than a second code path.
func _play_oneshot(inst: Dictionary, clip: String, fallback_s: float) -> float:
	var anim: AnimationPlayer = inst["anim"]
	if anim == null or not is_instance_valid(anim) or not anim.has_animation(clip):
		return fallback_s
	inst["state"] = clip
	# Authored tempo: the walk may be running sped-up to match the player's
	# tapping, and the one-shot clocks below assume the clip's own length.
	anim.speed_scale = 1.0
	anim.play(clip, 0.08)
	_clips_played[clip] = int(_clips_played.get(clip, 0)) + 1
	return maxf(0.05, anim.get_animation(clip).length)

func _find_anim(root: Node) -> AnimationPlayer:
	if root is AnimationPlayer:
		return root
	for child in root.get_children():
		var found := _find_anim(child)
		if found != null:
			return found
	return null

## The art for an id, or null. Probed once per id per session; `.scn` outranks a mesh
## for the same id, because an animated body beats a static one wherever both exist.
func _resource_for(id: String) -> Dictionary:
	if _registry.has(id):
		return _registry[id]
	var found: Dictionary = {}
	var scene_path := "%s/%s%s" % [MESH_DIR, id, SCENE_EXTENSION]
	if ResourceLoader.exists(scene_path):
		var ps := ResourceLoader.load(scene_path)
		if ps is PackedScene:
			found = { "scene": ps }
			print("[mesh] %s -> %s (animated)" % [id, scene_path])
	if found.is_empty():
		var mesh := _load_mesh(id)
		if mesh != null:
			found = { "mesh": mesh }
	_registry[id] = found
	return found

## The static-mesh half of the lookup, kept as its own function (and `_mesh_for` kept
## alive below it) because `geometry_check.tscn` resolves meshes through the same code
## the game runs -- that is the point of it.
func _load_mesh(id: String) -> Mesh:
	for ext in MESH_EXTENSIONS:
		var path := "%s/%s%s" % [MESH_DIR, id, ext]
		if not ResourceLoader.exists(path):
			continue
		var res := ResourceLoader.load(path)
		var found: Mesh = null
		if res is Mesh:
			found = res
		elif res is PackedScene:
			# What an imported .glb is: a scene whose first MeshInstance3D holds the
			# mesh. Taking the mesh keeps a static creature a single node.
			var scene := (res as PackedScene).instantiate()
			for child in scene.get_children():
				if child is MeshInstance3D:
					found = (child as MeshInstance3D).mesh
					break
			scene.queue_free()
		if found != null:
			print("[mesh] %s -> %s" % [id, path])
			return found
	return null

## The mesh for an id, or null -- the pre-animation lookup, still used by the geometry
## gate for static assets. Does not consult `.scn`; the gate checks scenes itself.
func _mesh_for(id: String) -> Mesh:
	var res := _resource_for(id)
	if res.has("mesh"):
		return res["mesh"]
	return null

## How far the avatar's visible body is from where the game says it stands, in
## world units. Zero when settled, or when the avatar has no mesh. This is what
## lets the camera glide while the simulation teleports: the world recentres a
## whole tile per turn, and a camera that rides this offset arrives exactly as
## the avatar's tween does.
func avatar_visual_offset() -> Vector3:
	var inst: Dictionary = _active.get(_avatar_uid, {})
	if inst.is_empty() or not inst["moving"]:
		return Vector3.ZERO
	var node: Node3D = inst["node"]
	if not is_instance_valid(node):
		return Vector3.ZERO
	return node.position - (inst["to"] as Vector3)

## For the first-frame report and the probes. `clips_played` is how a fixture asserts
## an attack was ever seen; `tweens_active` is how it asserts a step became motion.
func debug_stats() -> Dictionary:
	var meshes := 0
	var animated := 0
	for id in _registry:
		var res: Dictionary = _registry[id]
		if not res.is_empty():
			meshes += 1
			if res.has("scene"):
				animated += 1
	var tweens := 0
	for uid in _active:
		if _active[uid]["moving"]:
			tweens += 1
	return {
		"drawn": _active.size(), "ids_seen": _registry.size(),
		"ids_with_meshes": meshes, "animated_ids": animated,
		"tweens_active": tweens, "clips_played": _clips_played,
	}
