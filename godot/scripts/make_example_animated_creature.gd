extends Node
## Build the example animated creature, and be the spec of the animated-creature conventions.
##
##   godot --headless --path godot res://scenes/make_example_animated_creature.tscn
##
## Writes `res://meshes/creatures/mon_zombie.scn`: a rigged, skinned, animated box person,
## ugly on purpose, whose only job is to be a correct example of the conventions a real
## animated creature has to follow. A modeller can open it, scrub the clips, measure it,
## and throw it away. The conventions -- this file is the executable form of them:
##
##   - **The asset is a PackedScene**, `<id>.scn`: a root Node3D named after the id, holding
##     a Skeleton3D with a skinned MeshInstance3D and an AnimationPlayer. The id is the one
##     the tileset keys on, the one `CDDAHost::get_creatures()` publishes.
##   - **Six clips in the player's default library**, these names exactly: `idle`, `walk`
##     and `run` loop; `attack`, `hit` and `die` are one-shots.
##   - **Every clip is in-place.** Bones may swing and the hips may bob vertically, but the
##     root must not travel across the ground plane: the game moves the creature, and a clip
##     that also moved it would disagree with the game about where the creature is.
##   - **`die` holds its end pose**: last keyframe exactly at the clip's end, no loop,
##     because the renderer keeps the corpse pose until it frees the node.
##   - **Rest pose faces +Z, feet on the origin, centred on x and z, and as tall as the
##     sprite PAINTS, not as its frame.** One unit is one tile pixel; Ultica paints a
##     person ~32-33 px of its 32x48 frame (measured, 2026-08-18), so a humanoid is
##     ~33 units tall -- a 48-unit figure towers half again over every sprite beside it.
##     Same base rules as the static meshes (see `meshes/creatures/README.md`), plus the rig.
##
## Why `.scn` and not `.glb`: the same answer as `.tres` for the static mesh -- a saved
## scene loads with no import step, which matters while the editor is unusable. A modeller's
## `.glb` will eventually arrive through a converter, and this scene is the reference for
## what that converter has to produce.

## The id the scene is saved under and its root node is named after. `mon_zombie`, because
## it is the most numerous thing in the game: wherever the renderer is pointed, the test
## asset is probably on screen. A const so retargeting the placeholder is a one-line edit.
## Underscore-prefixed: a library file, not a creature. The mannequin WAS
## mon_zombie until real art arrived (the Zombie Apocalypse Kit, 2026-08-20);
## it stays as the executable spec a modeller measures against, under a name
## the loader never looks up and the geometry gate's creature pass skips.
const CREATURE_ID := "_example_mannequin"
const OUT_PATH := "res://meshes/creatures/" + CREATURE_ID + ".scn"

## The Skeleton3D node's name, which is also the node half of every animation track path
## ("Skeleton3D:hips"). Named what the glTF importer names it, so a clip authored against
## this scene retargets onto an imported one without editing paths.
const SKELETON_NAME := "Skeleton3D"

## name -> loops. The fixed clip contract; `_build_player()` writes it, the self-check
## reads it back off the saved file.
const CLIPS := {
	"idle": true,
	"walk": true,
	"run": true,
	"attack": false,
	"hit": false,
	"die": false,
}

## One unit is one tile pixel, so these are the numbers a 32x48 character sprite occupies.
## Joint heights are world y at rest; bone rests are stored relative to the parent joint
## and computed from these, because world heights are what a modeller measures against.
const TOTAL_HEIGHT := 48.0

## What the figure is scaled to on the way out, in tile pixels -- the height Ultica
## actually PAINTS, not the frame it paints in. Measured from the composed atlas
## (2026-08-18): a person is 32-33 opaque pixels of the 32x48 frame, a plain zombie 29.
## The first mannequins shipped at the frame's 48 and towered half again over every
## sprite around them, which is what "measured, not assumed" keeps costing this branch.
## The proportions above stay authored against 48 because they are easier to read that
## way; one uniform scale on the rig makes the saved figure match the art.
const PAINTED_HEIGHT := 33.0
const HIPS_Y := 22.0 # the root joint, and the only bone a clip may move -- vertically
const SPINE_Y := 24.0
const SHOULDER_Y := 38.0
const HEAD_Y := 40.0
const LEG_TOP_Y := 20.0
const ARM_X := 8.0 # shoulder distance from centre; `_l` is the creature's own left, +X when facing +Z
const LEG_X := 3.0

## Box sizes per body part. Boxes rather than a modelled body for the same reason as the
## static example: the conventions are easier to read off a shape that obviously is not art.
const TORSO := Vector3(12.0, 18.0, 7.0) # hips to shoulders, one box, driven by `spine`
const HEAD := Vector3(9.0, 8.0, 9.0)
const ARM := Vector3(4.0, 18.0, 4.0)
const LEG := Vector3(5.0, 20.0, 6.0)
const NOSE := Vector3(2.0, 2.0, 2.5)

func _ready() -> void:
	# Standalone (its own scene): build, save, self-check, exit with a verdict.
	# Embedded at boot by the host, generate() is called instead and quitting the
	# game's tree would be a bug, not a verdict.
	if get_tree().current_scene != self:
		return
	get_tree().quit(0 if generate() and _self_check() else 1)

## Build and save the mannequin. True on success. The self-check stays with the
## standalone run: at boot, the geometry gate already stands behind this file.
func generate() -> bool:
	var root := Node3D.new()
	root.name = CREATURE_ID

	# The one uniform scale that reconciles the authored 48-unit proportions with
	# the ~33 px Ultica actually paints. On a wrapper above the skeleton, where it
	# is a display transform: bone rests, skins and clip keys all stay in the
	# authored units a modeller can read.
	var rig := Node3D.new()
	rig.name = "Rig"
	rig.scale = Vector3.ONE * (PAINTED_HEIGHT / TOTAL_HEIGHT)
	root.add_child(rig)

	var skeleton := _build_skeleton()
	rig.add_child(skeleton)

	var mesh_instance := MeshInstance3D.new()
	mesh_instance.name = "Mesh"
	mesh_instance.mesh = _build_mesh(skeleton)
	# The vertices are authored in rest pose, so the skin -- the mapping from mesh space
	# into each bone's space -- is exactly the inverse of the rest transforms, which is
	# what this builds. MeshInstance3D's `skeleton` path defaults to "..", so being a
	# child of the Skeleton3D is the whole hookup.
	mesh_instance.skin = skeleton.create_skin_from_rest_transforms()
	skeleton.add_child(mesh_instance)

	rig.add_child(_build_player())

	# Owner must be set on every node in the tree, or PackedScene.pack() *silently drops
	# it*: no error, no warning, the node is simply absent from the saved file. The root
	# itself keeps no owner.
	_own(root, root)

	var packed := PackedScene.new()
	var err := packed.pack(root)
	root.free()
	if err != OK:
		push_error("[anim] could not pack the scene (error %d)" % err)
		return false
	# Bundled, so the mesh, skin, materials and animations travel inside the one .scn
	# instead of as loose sidecar resources.
	err = ResourceSaver.save(packed, OUT_PATH, ResourceSaver.FLAG_BUNDLE_RESOURCES)
	if err != OK:
		push_error("[anim] could not write %s (error %d)" % [OUT_PATH, err])
		return false
	print("[anim] wrote ", OUT_PATH)
	return true

## The rig: seven bones, joints where a wooden doll has hinges. Arms and head hang off the
## spine, spine and legs off the hips, so `hips` is the one bone that carries the whole
## figure -- which is why `die` rotates it, and why the in-place rule is stated about it.
func _build_skeleton() -> Skeleton3D:
	var skeleton := Skeleton3D.new()
	skeleton.name = SKELETON_NAME
	var hips := _bone(skeleton, "hips", -1, Vector3(0.0, HIPS_Y, 0.0))
	var spine := _bone(skeleton, "spine", hips, Vector3(0.0, SPINE_Y - HIPS_Y, 0.0))
	_bone(skeleton, "head", spine, Vector3(0.0, HEAD_Y - SPINE_Y, 0.0))
	_bone(skeleton, "arm_l", spine, Vector3(ARM_X, SHOULDER_Y - SPINE_Y, 0.0))
	_bone(skeleton, "arm_r", spine, Vector3(-ARM_X, SHOULDER_Y - SPINE_Y, 0.0))
	_bone(skeleton, "leg_l", hips, Vector3(LEG_X, LEG_TOP_Y - HIPS_Y, 0.0))
	_bone(skeleton, "leg_r", hips, Vector3(-LEG_X, LEG_TOP_Y - HIPS_Y, 0.0))
	# Poses start at identity, not at rest: without this the skin -- built from the rests
	# -- would fold the whole figure into the origin, and the scene would save collapsed.
	skeleton.reset_bone_poses()
	return skeleton

## Add a bone and return its index. @p rest is the joint position *relative to the parent
## joint*, because bone rests chain; the world heights live in the constants above.
func _bone(skeleton: Skeleton3D, bone_name: String, parent: int, rest: Vector3) -> int:
	skeleton.add_bone(bone_name)
	var idx := skeleton.get_bone_count() - 1
	if parent >= 0:
		skeleton.set_bone_parent(idx, parent)
	skeleton.set_bone_rest(idx, Transform3D(Basis(), rest))
	return idx

## One ArrayMesh, two surfaces: body and head, so the head can wear its own colour --
## facing has to stay visible in motion, and the nose alone is small. Every box is rigidly
## skinned to a single bone: the parts hinge like a wooden doll, which is all the
## deformation a placeholder should claim.
func _build_mesh(skeleton: Skeleton3D) -> ArrayMesh:
	var body := SurfaceTool.new()
	body.begin(Mesh.PRIMITIVE_TRIANGLES)
	# Torso from just below the hips joint to just under the head, one box on `spine`.
	_skinned_box(body, TORSO, Vector3(0.0, HIPS_Y - 1.0 + TORSO.y * 0.5, 0.0),
		skeleton.find_bone("spine"))
	_skinned_box(body, ARM, Vector3(ARM_X, SHOULDER_Y - ARM.y * 0.5, 0.0),
		skeleton.find_bone("arm_l"))
	_skinned_box(body, ARM, Vector3(-ARM_X, SHOULDER_Y - ARM.y * 0.5, 0.0),
		skeleton.find_bone("arm_r"))
	_skinned_box(body, LEG, Vector3(LEG_X, LEG.y * 0.5, 0.0), skeleton.find_bone("leg_l"))
	_skinned_box(body, LEG, Vector3(-LEG_X, LEG.y * 0.5, 0.0), skeleton.find_bone("leg_r"))
	var mesh: ArrayMesh = body.commit()

	var head := SurfaceTool.new()
	head.begin(Mesh.PRIMITIVE_TRIANGLES)
	var head_bone := skeleton.find_bone("head")
	_skinned_box(head, HEAD, Vector3(0.0, HEAD_Y + HEAD.y * 0.5, 0.0), head_bone)
	# The nose, again: the one thing in the mesh that says which way is front. On a rig it
	# earns its keep twice, because `attack` and `die` only read correctly from the front.
	_skinned_box(head, NOSE, Vector3(0.0, HEAD_Y + HEAD.y * 0.5, (HEAD.z + NOSE.z) * 0.5),
		head_bone)
	head.commit(mesh) # appends as surface 1 of the same mesh

	# Plain and matte, like the static example and for the same reason: the mesh is lit by
	# the world's own sun and lamps, and anything shiny would be reading the lighting
	# instead of showing it. Rot-green body; the head paler, so facing survives distance.
	mesh.surface_set_material(0, _flat(Color(0.30, 0.44, 0.26)))
	mesh.surface_set_material(1, _flat(Color(0.62, 0.64, 0.34)))
	return mesh

func _flat(albedo: Color) -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.albedo_color = albedo
	mat.roughness = 0.9
	return mat

## Append a box of @p size centred at @p at -- in rest-pose model space, because that is
## the space `create_skin_from_rest_transforms()` binds -- every vertex weighted 1.0 to
## @p bone. The triangles are borrowed from a BoxMesh rather than emitted by hand, so the
## winding -- the easiest thing to get wrong about a face, and invisible until a box
## renders inside-out -- is Godot's own.
func _skinned_box(tool: SurfaceTool, size: Vector3, at: Vector3, bone: int) -> void:
	var box := BoxMesh.new()
	box.size = size
	var arrays := box.get_mesh_arrays()
	var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL]
	var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX]
	# Rigid skinning: one bone, full weight, padded to the four slots SurfaceTool expects.
	# Like normals, bones and weights are per-vertex state and must be set *before* each
	# add_vertex, or the vertex is emitted unskinned and commit() rejects the mix.
	var bones := PackedInt32Array([bone, 0, 0, 0])
	var weights := PackedFloat32Array([1.0, 0.0, 0.0, 0.0])
	for i in indices:
		tool.set_normal(normals[i])
		tool.set_bones(bones)
		tool.set_weights(weights)
		tool.add_vertex(vertices[i] + at)

## The six clips of the contract, in the *default* ("") library: that is what leaves clip
## names unprefixed, so the renderer asks for "walk" and not "library/walk".
func _build_player() -> AnimationPlayer:
	var player := AnimationPlayer.new()
	player.name = "AnimationPlayer"
	var library := AnimationLibrary.new()
	library.add_animation("idle", _make_idle())
	library.add_animation("walk", _make_gait(0.7, 25.0, 20.0, 0.4))
	library.add_animation("run", _make_gait(0.45, 40.0, 35.0, 0.8))
	library.add_animation("attack", _make_attack())
	library.add_animation("hit", _make_hit())
	library.add_animation("die", _make_die())
	player.add_animation_library("", library)
	return player

func _clip(length: float, loops: bool) -> Animation:
	var anim := Animation.new()
	anim.length = length
	# LOOP_LINEAR wraps the interpolation across the ends; the one-shots use LOOP_NONE and
	# hold their last key when playback runs off the end, which `die` depends on.
	anim.loop_mode = Animation.LOOP_LINEAR if loops else Animation.LOOP_NONE
	return anim

## idle: standing without being a statue -- a slow side-to-side spine sway and half a unit
## of breathing bob. Subtle on purpose; at rest a creature is background.
func _make_idle() -> Animation:
	var anim := _clip(2.0, true)
	_rot_track(anim, "spine", Vector3.BACK,
		[0.0, 0.5, 1.0, 1.5, 2.0], [0.0, 3.0, 0.0, -3.0, 0.0])
	_bob_track(anim, [0.0, 1.0, 2.0], [HIPS_Y, HIPS_Y - 0.5, HIPS_Y])
	return anim

## walk and run are one gait with the dials turned, which is what a placeholder should be:
## legs swing about X in counter-phase, arms counter-swing their own side's leg -- that
## opposition is what makes a walk read as a walk -- and the hips dip once per footfall.
func _make_gait(length: float, leg_deg: float, arm_deg: float, dip: float) -> Animation:
	var anim := _clip(length, true)
	var half := length * 0.5
	var swing := [0.0, half, length]
	_rot_track(anim, "leg_l", Vector3.RIGHT, swing, [leg_deg, -leg_deg, leg_deg])
	_rot_track(anim, "leg_r", Vector3.RIGHT, swing, [-leg_deg, leg_deg, -leg_deg])
	_rot_track(anim, "arm_l", Vector3.RIGHT, swing, [-arm_deg, arm_deg, -arm_deg])
	_rot_track(anim, "arm_r", Vector3.RIGHT, swing, [arm_deg, -arm_deg, arm_deg])
	_bob_track(anim, [0.0, length * 0.25, half, length * 0.75, length],
		[HIPS_Y, HIPS_Y - dip, HIPS_Y, HIPS_Y - dip, HIPS_Y])
	return anim

## attack: the right arm goes up past the shoulder and comes down through the target, with
## a little spine twist behind it. One-shot; whatever plays it decides what plays next.
func _make_attack() -> Animation:
	var anim := _clip(0.4, false)
	_rot_track(anim, "arm_r", Vector3.RIGHT,
		[0.0, 0.15, 0.32, 0.4], [0.0, -95.0, -15.0, 0.0])
	_rot_track(anim, "spine", Vector3.UP,
		[0.0, 0.15, 0.32, 0.4], [0.0, 8.0, -6.0, 0.0])
	return anim

## hit: a flinch -- the spine snaps back and recovers. Short on purpose, because it plays
## for every landed blow and melee lands a lot of them.
func _make_hit() -> Animation:
	var anim := _clip(0.25, false)
	_rot_track(anim, "spine", Vector3.RIGHT, [0.0, 0.1, 0.25], [0.0, -15.0, 0.0])
	return anim

## die: the whole figure pitches backward about the hips to lying, the hips dropping so the
## body ends at box-person floor height instead of pivoting in mid-air. The last keys sit
## exactly at the clip's end: with LOOP_NONE the player holds them, and the renderer keeps
## that corpse pose until it frees the node.
func _make_die() -> Animation:
	var anim := _clip(0.9, false)
	_rot_track(anim, "hips", Vector3.RIGHT,
		[0.0, 0.45, 0.72, 0.9], [0.0, -55.0, -88.0, -85.0])
	_bob_track(anim, [0.0, 0.45, 0.9], [HIPS_Y, HIPS_Y - 6.0, 4.5])
	return anim

## A rotation track on @p bone, keyed as (time, degrees about @p axis) pairs. Track paths
## name the skeleton *node* and the bone after the colon -- "Skeleton3D:hips" -- which is
## how AnimationPlayer addresses bones; the node half is relative to the player's
## root_node, which defaults to its parent, the scene root here.
func _rot_track(anim: Animation, bone: String, axis: Vector3, times: Array,
		degrees: Array) -> void:
	var track := anim.add_track(Animation.TYPE_ROTATION_3D)
	anim.track_set_path(track, "%s:%s" % [SKELETON_NAME, bone])
	for i in times.size():
		anim.rotation_track_insert_key(track, times[i], Quaternion(axis, deg_to_rad(degrees[i])))

## The hips' vertical bob -- the only position track any clip is allowed. A position key
## *replaces* the bone pose rather than adding to it, so every key restates the hips' rest
## x and z of zero: the in-place rule, enforced by construction.
func _bob_track(anim: Animation, times: Array, heights: Array) -> void:
	var track := anim.add_track(Animation.TYPE_POSITION_3D)
	anim.track_set_path(track, "%s:hips" % SKELETON_NAME)
	for i in times.size():
		anim.position_track_insert_key(track, times[i], Vector3(0.0, heights[i], 0.0))

## pack() saves a node only if its owner is the scene root; anything unowned is *silently
## dropped* -- no error, the node is just missing from the file. So: everything, recursively.
func _own(node: Node, root: Node) -> void:
	if node != root:
		node.owner = root
	for child in node.get_children():
		_own(child, root)

## Load the saved scene back and prove the contract, so a broken save fails this run
## instead of the first play-test. Checked in-tree, because global transforms are what the
## renderer will see.
func _self_check() -> bool:
	# CACHE_MODE_IGNORE, or load() hands back the very object just saved and the
	# round-trip tests nothing.
	var packed := ResourceLoader.load(OUT_PATH, "PackedScene",
		ResourceLoader.CACHE_MODE_IGNORE) as PackedScene
	if not _check(packed != null, "reloads as a PackedScene"):
		return false
	var creature := packed.instantiate()
	add_child(creature)
	var ok := _check(creature is Node3D and String(creature.name) == CREATURE_ID,
		"root is a Node3D named \"%s\"" % CREATURE_ID)

	var skeleton := creature.find_child(SKELETON_NAME, true, false) as Skeleton3D
	ok = _check(skeleton != null and skeleton.get_bone_count() >= 7,
		"skeleton has %d bones (contract needs 7)"
			% (skeleton.get_bone_count() if skeleton != null else 0)) and ok

	var player := creature.find_child("AnimationPlayer", true, false) as AnimationPlayer
	ok = _check(player != null, "AnimationPlayer present") and ok
	if player != null:
		for clip_name in CLIPS:
			var loops: bool = CLIPS[clip_name]
			var good := false
			var described := "missing"
			if player.has_animation(clip_name):
				var anim := player.get_animation(clip_name)
				good = anim.loop_mode \
					== (Animation.LOOP_LINEAR if loops else Animation.LOOP_NONE)
				described = "%.2fs %s" % [anim.length, "loops" if loops else "one-shot"]
			ok = _check(good, "clip %-6s %s" % [clip_name, described]) and ok

	# The rest-pose bounds, from the mesh data rather than the rendering server, so the
	# check means the same thing headless as it does with a GPU.
	var merged := AABB()
	var boxes := 0
	for node in creature.find_children("*", "MeshInstance3D", true, false):
		var mi := node as MeshInstance3D
		if mi.mesh == null:
			continue
		var mesh_box: AABB = mi.global_transform * mi.mesh.get_aabb()
		merged = mesh_box if boxes == 0 else merged.merge(mesh_box)
		boxes += 1
	ok = _check(boxes > 0 and absf(merged.position.y) <= 1.0
			and merged.size.y >= PAINTED_HEIGHT - 3.0 and merged.size.y <= PAINTED_HEIGHT + 3.0,
		"rest pose %.1f x %.1f x %.1f units, base y %+.2f (feet on origin, ~%d tall as painted)"
			% [merged.size.x, merged.size.y, merged.size.z, merged.position.y,
				int(PAINTED_HEIGHT)]) and ok

	creature.queue_free()
	return ok

## One line per contract clause, so the headless log *is* the checklist.
func _check(passed: bool, what: String) -> bool:
	if passed:
		print("[anim] ok   ", what)
	else:
		push_error("[anim] FAIL " + what)
	return passed
