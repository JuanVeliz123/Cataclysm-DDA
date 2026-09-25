extends Node
## Turn a `.glb` into a `.res` mesh -- or a `.scn` scene -- the game can load, without the editor.
##
##   godot --headless --path godot res://scenes/convert_creature_meshes.tscn
##
## Converts every `.glb` in `res://meshes/creatures/` to `<name>.res` beside it, normalised
## to the conventions in that directory's README: origin between the feet, centred on it,
## and scaled so the figure is `TARGET_HEIGHT` units tall.
##
## A *rigged* `.glb` -- one with a skeleton and at least one animation -- becomes
## `<name>.scn` instead: the imported scene kept whole, so the bones and the clips
## survive. The loader prefers a `.scn` over a `.res`, so both can sit beside the same
## source while a creature is being rigged.
##
## A rigged `.glb` that is *missing* clips -- some of the six, or all of them, which is
## what a bare character download is -- borrows the missing ones from the shared library,
## `_shared_clips.scn`/`.glb`, if one sits in the same directory. See `_borrow_clips` for
## the copy and the README for the same-skeleton contract that makes it legitimate.
##
## **Why this exists.** `.glb` and `.obj` are *source assets*: Godot converts them into
## `.godot/imported/` and needs a `.import` file beside them, which only the editor writes.
## So a `.glb` dropped into the project cannot be loaded by the running game at all -- and
## while the editor is unusable, that is every `.glb`. `GLTFDocument` reads one directly at
## runtime, which is the whole trick here; nothing else in this script is clever.
##
## `.obj` has no equivalent runtime reader, which is the answer to "which of the two is worth
## converting from": the `.glb`. It is also smaller for identical geometry -- 431 KB against
## 1 MB for the same 13,014 triangles -- and it carries its material, where an `.obj` refers
## to a `.mtl` sidecar that tends not to travel with it.

const MESH_DIR := "res://meshes/creatures"

## Units tall the figure is scaled to. One unit is one tile pixel, so this is the height
## of the *art* being replaced -- what the sprite paints, not the frame it paints in.
## Ultica paints a person only 32-33 opaque pixels of its 32x48 frame (measured from the
## composed atlas, 2026-08-18: player_male painted 15x33, player_female 15x32, mon_zombie
## 21x29), so meshes normalised to the frame's 48 towered half again over every sprite
## beside them -- and the first person to walk around one said so. The example generator
## scales its mannequin to the same 33.
const TARGET_HEIGHT := 33.0

## Degrees to turn the model about Y on the way in.
##
## The convention is that a creature faces +Z, toward the camera, because that is what its
## sprite does. Exporters disagree about which way "forward" is, and no inspection of a mesh
## can settle it -- so if the converted creature has its back to you, set this to 180 and run
## it again. The example mesh has a nose for exactly this reason.
const FACE_DEGREES := 0.0

## Triangles beyond which the extra detail cannot be resolved on screen.
##
## A creature is `TARGET_HEIGHT` units tall and about half that wide, and one unit is one
## screen pixel at default zoom -- so it occupies roughly five hundred pixels, and eight
## thousand at maximum zoom. This is not a performance limit; a GPU will not notice. It is a
## note that detail past it is being paid for and not seen.
const TRIANGLE_BUDGET := 1500

## Clip names the animated convention expects in the AnimationPlayer's default library.
## The runtime plays `idle` and `walk` constantly and the others on events, so a missing
## one is warned about rather than failed here -- a creature that cannot attack yet is
## still worth seeing walk. The gate (`geometry_check.gd`) is stricter about the two it
## cannot do without.
const EXPECTED_CLIPS := ["idle", "walk", "attack", "hit", "die"]

## Clips that repeat, `run` included when it exists. glTF has no loop flag, so every
## exporter ships every clip as LOOP_NONE -- and a non-looping idle plays once and
## freezes, which reads downstream as a T-pose bug in the renderer rather than as what
## it is. Fixed here rather than at load, so the saved asset is correct on its own.
const LOOPING_CLIPS := ["idle", "walk", "run"]

## The full animated contract, `run` included: what a finished creature carries, and so
## the set whose gaps `_borrow_clips` fills from the shared library. Distinct from
## `EXPECTED_CLIPS` above, which is only what is *warned* about -- `run` is optional to
## have, but if the library offers one there is no reason not to take it.
const CONTRACT_CLIPS := ["idle", "walk", "run", "attack", "hit", "die"]

## What the wild calls the contract's clips. Every pack names its clips its own
## way -- Quaternius ships "Armature|Walking", Mixamo ships "mixamo.com" strings --
## and asking a modeller to rename them by hand is asking for the one manual step
## that gets skipped. The converter normalises instead: strip any "Prefix|",
## lowercase, drop spaces/underscores, and map through this table. Names that
## match nothing (Jump, Working, SitIdle...) are kept as they are -- extra clips
## are harmless and someone may want them later.
const CLIP_ALIASES := {
	"idle": "idle",
	"walk": "walk", "walking": "walk",
	"run": "run", "running": "run", "sprint": "run",
	"attack": "attack", "punch": "attack", "melee": "attack", "swing": "attack",
	"hit": "hit", "hitreceive": "hit", "hitrecieve": "hit", "gethit": "hit",
	"takedamage": "hit", "hitreact": "hit", "hitreactleft": "hit",
	"hitreactright": "hit",
	"die": "die", "death": "die", "dying": "die",
}

## Per-id heights, in tile pixels, for ids whose art does not paint the default
## humanoid ~33 -- measured from the composed Ultica atlas exactly as the 33
## itself was (opaque-pixel extent, 2026-08-20). One global height made a
## German Shepherd as tall as a person the day the kit arrived. Anything not
## listed gets TARGET_HEIGHT; add a line when a new body plan lands.
const PAINTED_HEIGHTS := {
	"mon_zombie": 29.0,
	"mon_zombie_fat": 34.0,
	"mon_zombie_brute": 34.0,
	"mon_zombie_rot": 25.0,
	"mon_dog": 21.0,
	"mon_dog_gshepherd": 20.0,
}

## Basename of the shared clip library: one file of contract clips that every model
## rigged to the same skeleton borrows from, so the six are animated once rather than
## once per creature. The underscore prefix is what keeps it from being read as a
## creature id -- monster type ids never start with one -- and `_ready`'s loop skips it
## for the same reason. `.scn` is preferred over `.glb`, the loader's own preference.
const SHARED_CLIPS := "_shared_clips"

## The shared library, loaded at most once per run and shared by every model that
## borrows -- `_shared_library_player()` fills these, `_ready` frees the scene at exit.
var _lib_scene: Node = null
var _lib_player: AnimationPlayer = null
var _lib_tried := false

func _ready() -> void:
	# Standalone (the convert_creature_meshes.tscn scene): convert everything and
	# exit with a verdict. Embedded -- the host adds this script as a helper node
	# at boot so a fresh checkout plays without a manual converter run -- the
	# caller drives via convert_missing() and _ready must neither convert twice
	# nor quit the game's own tree.
	if get_tree().current_scene != self:
		return
	var r := convert_all(true)
	get_tree().quit(1 if int(r["failed"]) > 0 else 0)

## Convert only what is missing or older than its source -- the boot-time entry.
## A derived .scn/.res that is at least as new as its .glb is left alone, so a
## normal launch pays one file-stat per model and no conversion at all.
func convert_missing() -> Dictionary:
	return convert_all(false)

func convert_all(force: bool) -> Dictionary:
	var dir := DirAccess.open(MESH_DIR)
	if dir == null:
		push_error("[convert] no %s" % MESH_DIR)
		return { "converted": 0, "failed": 1, "fresh": 0 }
	var converted := 0
	var failed := 0
	var fresh := 0
	for file in dir.get_files():
		if file.get_extension().to_lower() != "glb":
			continue
		if file.begins_with("_"):
			# An underscore prefix marks a shared file, not a creature: `_shared_clips.glb`
			# is the library the animated path borrows from, and converting it would mint
			# a creature scene for an id no monster has.
			print("[convert] %s is shared, not a creature -- skipped" % file)
			continue
		var src := "%s/%s" % [MESH_DIR, file]
		if not force and _derived_is_fresh(src):
			fresh += 1
			continue
		if _convert(src):
			converted += 1
		else:
			failed += 1
	if converted == 0 and failed == 0 and fresh == 0:
		print("[convert] no .glb in %s -- nothing to do" % MESH_DIR)
	print("[convert] %d converted, %d failed, %d already fresh" % [converted, failed, fresh])
	# The shared library, if one was loaded, never joined the tree; freed by hand so the
	# run exits without leaked-instance noise.
	if _lib_scene != null:
		_lib_scene.free()
		_lib_scene = null
	return { "converted": converted, "failed": failed, "fresh": fresh }

## The height this id's art paints, in tile pixels: its PAINTED_HEIGHTS entry,
## or the humanoid default.
func _height_for(path: String) -> float:
	return float(PAINTED_HEIGHTS.get(path.get_file().get_basename(), TARGET_HEIGHT))

## Whether a source .glb already has a derived asset at least as new as itself.
func _derived_is_fresh(src: String) -> bool:
	var src_time := FileAccess.get_modified_time(src)
	for ext in [".scn", ".res"]:
		var derived: String = src.get_basename() + str(ext)
		if FileAccess.file_exists(derived) and FileAccess.get_modified_time(derived) >= src_time:
			return true
	return false

func _convert(path: String) -> bool:
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	var err := doc.append_from_file(path, state)
	if err != OK:
		push_error("[convert] %s could not be read as glTF (error %d)" % [path, err])
		return false
	var scene := doc.generate_scene(state)
	if scene == null:
		push_error("[convert] %s produced no scene" % path)
		return false

	# A rig changes what the asset *is*. Bones own the vertices, so merging surfaces
	# through SurfaceTool would freeze the figure in its bind pose and drop the clips on
	# the floor -- which is what this script used to do on purpose, back when there was
	# no animation path to feed. A scene with a skeleton and at least one animation goes
	# down the animated path instead; everything else is flattened to a Mesh as before.
	var skeleton: Skeleton3D = null
	var player: AnimationPlayer = null
	for node in _walk(scene):
		if skeleton == null and node is Skeleton3D:
			skeleton = node
		if player == null and node is AnimationPlayer \
				and (node as AnimationPlayer).get_animation_list().size() > 0:
			player = node
	if skeleton != null and player != null:
		return _convert_animated(path, scene, skeleton, player)
	if skeleton != null and not _shared_library_path().is_empty():
		# A skeleton and no clips is exactly what a bare character download is -- Mixamo
		# ships the model and the animations as separate files on purpose. With the
		# shared library present the animated path can still dress it: it creates the
		# player and borrows all six contract clips (see _borrow_clips).
		return _convert_animated(path, scene, skeleton, null)
	# No rig -- or a rig with no clips and no library to borrow from, which is a statue,
	# and the static path draws statues better: a pooled mesh instead of a scene per
	# creature, and nothing a skeleton could add without a clip to move it.
	return _convert_static(path, scene)

## The static path: every surface merged into one ArrayMesh with each part's own
## transform, normalised by baking scale and rotation into the vertices, saved `<id>.res`.
func _convert_static(path: String, scene: Node) -> bool:
	# In the tree before anything reads a global_transform, or every one of them comes back as
	# identity -- with a console error per call and parts merged where they are not. A model
	# whose pieces happen to sit at the origin survives that; one whose head is a child node
	# offset up from its body does not, and the failure is a head inside a chest.
	add_child(scene)

	# Merged with each part's own transform, because a model is usually several nodes -- this
	# one is three meshes across ten nodes -- and the renderer draws one mesh per creature.
	var raw := SurfaceTool.new()
	raw.begin(Mesh.PRIMITIVE_TRIANGLES)
	var material: Material = null
	var surfaces := 0
	for node in _walk(scene):
		var mi := node as MeshInstance3D
		if mi == null or mi.mesh == null:
			continue
		for si in mi.mesh.get_surface_count():
			raw.append_from(mi.mesh, si, mi.global_transform)
			surfaces += 1
			if material == null:
				material = mi.mesh.surface_get_material(si)
				if material == null:
					material = mi.get_active_material(si)
	remove_child(scene)
	scene.queue_free()
	if surfaces == 0:
		push_error("[convert] %s holds no mesh surfaces" % path)
		return false
	var merged: ArrayMesh = raw.commit()

	# Normalise: scale to the target height, put the base on y = 0 and centre x and z. Done
	# as one transform through append_from rather than by walking vertices, so normals are
	# carried and rotated by the same maths.
	var box := merged.get_aabb()
	if box.size.y <= 0.0:
		push_error("[convert] %s has no height" % path)
		return false
	var scale := _height_for(path) / box.size.y
	var basis := Basis(Vector3.UP, deg_to_rad(FACE_DEGREES)).scaled(Vector3.ONE * scale)
	# Where the scaled-and-turned box lands, so the offset can put its feet on the floor.
	var placed := Transform3D(basis, Vector3.ZERO) * box
	var fix := Transform3D(basis, Vector3(
		-(placed.position.x + placed.size.x * 0.5),
		-placed.position.y,
		-(placed.position.z + placed.size.z * 0.5)))

	var out := SurfaceTool.new()
	out.begin(Mesh.PRIMITIVE_TRIANGLES)
	out.append_from(merged, 0, fix)
	var final: ArrayMesh = out.commit()
	if material != null:
		final.surface_set_material(0, material)

	var indices: PackedInt32Array = final.surface_get_arrays(0)[Mesh.ARRAY_INDEX]
	var tris := indices.size() / 3
	var final_box := final.get_aabb()
	var out_path := "%s.res" % path.get_basename()
	var save_err := ResourceSaver.save(final, out_path)
	if save_err != OK:
		push_error("[convert] could not write %s (error %d)" % [out_path, save_err])
		return false

	print("[convert] %s -> %s" % [path.get_file(), out_path.get_file()])
	print("[convert]   %d surfaces merged, %d triangles, material=%s" % [
		surfaces, tris, "yes" if material != null else "none"])
	print("[convert]   %.1f x %.1f x %.1f units, base y %+.2f, scaled %.3fx" % [
		final_box.size.x, final_box.size.y, final_box.size.z, final_box.position.y, scale])
	if tris > TRIANGLE_BUDGET:
		# Said rather than fixed: decimating well is a modelling decision, and a DCC tool
		# does it better than anything that could be written here.
		push_warning(("[convert] %s has %d triangles against a budget of %d. It will render "
			+ "correctly; it is drawn at about five hundred screen pixels, so most of that "
			+ "detail cannot be resolved. Decimate in the modelling tool.")
			% [path.get_file(), tris, TRIANGLE_BUDGET])
	return true

## The animated path: a rigged `.glb` becomes `<id>.scn`, a whole PackedScene, because a
## bare Mesh cannot carry a skeleton or a clip.
##
## The imported hierarchy goes into the new root *intact*. AnimationPlayer tracks address
## their bones and meshes by relative NodePath, so reparenting anything inside it leaves
## every track pointing at nothing -- silently, one track at a time -- and the clips play
## against thin air. All this function may do to the import is wrap it and set one
## transform on it.
##
## @p player may arrive null: the bare-rig case, routed here because the shared library
## exists. One is adopted or created below and every contract clip borrowed into it.
func _convert_animated(path: String, scene: Node, skeleton: Skeleton3D,
		player: AnimationPlayer) -> bool:
	var scene3d := scene as Node3D
	if scene3d == null:
		# glTF roots are Node3Ds; if one ever is not, there is nowhere to hang the
		# normalise transform without restructuring, which the comment above forbids.
		push_error("[convert] %s: rigged, but its root is not a Node3D" % path)
		scene.queue_free()
		return false
	var id := path.get_file().get_basename()
	var root := Node3D.new()
	root.name = id
	root.add_child(scene)
	# In the tree before anything reads a global_transform, for the same reason as the
	# static path: outside it they all come back identity, and the box is measured wrong.
	add_child(root)

	# The combined rest-pose box, from each part's global transform -- the same sum the
	# static path feeds through SurfaceTool, minus the merge.
	var box := AABB()
	var boxed := false
	for node in _walk(scene):
		var mi := node as MeshInstance3D
		if mi == null or mi.mesh == null:
			continue
		var part: AABB = mi.global_transform * mi.mesh.get_aabb()
		box = part if not boxed else box.merge(part)
		boxed = true
	if not boxed or box.size.y <= 0.0:
		remove_child(root)
		root.queue_free()
		push_error("[convert] %s is rigged but holds no mesh with height" % path)
		return false

	# The bare-rig case: no player came in (see _convert's routing). Adopt an empty one
	# the exporter left behind, or create one under the wrapper -- the borrowed clips
	# need a node to live in, and the runtime looks for exactly one AnimationPlayer.
	if player == null:
		for node in _walk(scene):
			if node is AnimationPlayer:
				player = node
				break
	if player == null:
		player = AnimationPlayer.new()
		player.name = "AnimationPlayer"
		root.add_child(player)

	# First give the clips their contract names -- what the pack calls "Armature|Walking"
	# the runtime plays as "walk" -- so the missing-set and the borrow below reason
	# about the same vocabulary the renderer speaks.
	_normalise_clip_names(player, path.get_file())

	# Whatever the model lacks of the six-clip contract, the shared library supplies. A
	# model that has all six borrows nothing; with no library, the warnings below stand.
	var clips := player.get_animation_list()
	var missing: Array[String] = []
	for wanted in CONTRACT_CLIPS:
		if not clips.has(wanted):
			missing.append(wanted)
	if not missing.is_empty() and not _shared_library_path().is_empty():
		_borrow_clips(player, skeleton, missing, id)
		clips = player.get_animation_list()
	if clips.is_empty():
		# Rigged, but nothing plays and nothing could be borrowed -- a statue after all
		# (see _convert). Unwound rather than failed: nothing has been transformed yet,
		# so the import can still take the static exit it would have taken alone.
		push_warning(("[convert] %s is rigged but ends with no clips -- none of its own, "
			+ "none borrowable -- so it converts as a static mesh instead.")
			% path.get_file())
		root.remove_child(scene)
		remove_child(root)
		root.free()
		return _convert_static(path, scene)

	# Re-measure in the space the CLIPS play in, not the rest pose the exporter left.
	# The two can disagree wholesale: the Quaternius woman's FBX keeps a rest pose a
	# few centimetres tall while her clips pose bones in a space ~700x larger, so a
	# rest-box normalisation scaled her by her lying thickness and the idle clip then
	# filled the camera with dress. A mesh box cannot see any of that -- skinning
	# happens on the GPU and MeshInstance3D's AABB stays rest-shaped whatever plays --
	# but the BONES can: their posed global origins at idle's first frame span the
	# body the runtime will actually show, crown to sole (HeadTop_End and the toe
	# ends are real bones on these rigs). Grown a few percent for flesh over bone.
	if player.has_animation("idle"):
		player.play("idle")
		player.advance(0.0)
		var bone_box := AABB()
		var boned := false
		for node in _walk(scene):
			var sk := node as Skeleton3D
			if sk == null:
				continue
			for i in sk.get_bone_count():
				var p: Vector3 = (sk.global_transform * sk.get_bone_global_pose(i)).origin
				bone_box = AABB(p, Vector3.ZERO) if not boned else bone_box.expand(p)
				boned = true
		player.stop()
		if boned and bone_box.size.y > 0.0001:
			bone_box = bone_box.grow(bone_box.size.y * 0.03)
			# Union with the mesh rest box rather than a replacement: a rig whose
			# joints stop above the ankles (no foot bones) measures short on bones
			# alone, and a degenerate mesh box is too small to drag the union
			# anywhere. Whichever half is honest wins each face.
			box = bone_box if box.size.y < bone_box.size.y * 0.01 else bone_box.merge(box)

	# Normalise with the same maths as the static path, but carried on the imported
	# root's transform instead of baked into the vertices -- the bones own the vertices
	# now, and a bake would fight the skin. Composed onto whatever transform the exporter
	# left on that root, because that transform is part of what the box just measured.
	var scale := _height_for(path) / box.size.y
	var basis := Basis(Vector3.UP, deg_to_rad(FACE_DEGREES)).scaled(Vector3.ONE * scale)
	var placed := Transform3D(basis, Vector3.ZERO) * box
	var fix := Transform3D(basis, Vector3(
		-(placed.position.x + placed.size.x * 0.5),
		-placed.position.y,
		-(placed.position.z + placed.size.z * 0.5)))
	scene3d.transform = fix * scene3d.transform

	for clip_name in clips:
		if LOOPING_CLIPS.has(clip_name):
			var anim := player.get_animation(clip_name)
			if anim.loop_mode == Animation.LOOP_NONE:
				anim.loop_mode = Animation.LOOP_LINEAR
	for wanted in EXPECTED_CLIPS:
		if not clips.has(wanted):
			push_warning(("[convert] %s has no '%s' clip. It converts and loads anyway; "
				+ "the runtime falls back to the clips that exist, so add it when there "
				+ "is one.") % [path.get_file(), wanted])

	# Owner, recursively, or the .scn is one lonely Node3D: `PackedScene.pack` keeps a
	# node only if its owner is the node being packed, and `generate_scene` set every
	# owner to *its* root -- which is now a child. This is the classic trap of building
	# scenes in code, and it fails without an error: the save succeeds, just smaller.
	for node in _walk(root):
		if node != root:
			node.owner = root

	var packed := PackedScene.new()
	var pack_err := packed.pack(root)
	remove_child(root)
	root.queue_free()
	if pack_err != OK:
		push_error("[convert] %s could not be packed (error %d)" % [path, pack_err])
		return false
	var out_path := "%s.scn" % path.get_basename()
	# BUNDLE_RESOURCES, or the .scn stores *references* to sub-resources of a .glb the
	# running game cannot load (see the header) and every mesh and clip arrives null.
	var save_err := ResourceSaver.save(packed, out_path, ResourceSaver.FLAG_BUNDLE_RESOURCES)
	if save_err != OK:
		push_error("[convert] could not write %s (error %d)" % [out_path, save_err])
		return false

	var final_box: AABB = fix * box
	print("[convert] %s -> %s (animated)" % [path.get_file(), out_path.get_file()])
	print("[convert]   clips: %s; %d bones" % [", ".join(clips), skeleton.get_bone_count()])
	print("[convert]   %.1f x %.1f x %.1f units, base y %+.2f, scaled %.3fx" % [
		final_box.size.x, final_box.size.y, final_box.size.z, final_box.position.y, scale])
	if FileAccess.file_exists("%s.res" % path.get_basename()):
		print("[convert]   %s.res sits beside it from before the rig. Left alone: the loader prefers the .scn." % id)
	return true

## Where the shared clip library sits, or "" when there is none. `.scn` before `.glb`,
## the creature loader's own preference: the saved scene is the converted or authored
## form, the `.glb` the raw source.
func _shared_library_path() -> String:
	for ext in ["scn", "glb"]:
		var lib_path := "%s/%s.%s" % [MESH_DIR, SHARED_CLIPS, ext]
		if FileAccess.file_exists(lib_path):
			return lib_path
	return ""

## The library's AnimationPlayer, loaded once per run and cached: every model in the
## directory borrows from the same file, and a glTF parse per model would be paid for
## nothing. A `.scn` loads as the PackedScene it is; a `.glb` goes through GLTFDocument
## at runtime, exactly as `_convert` reads the models and for the same reason (see the
## header: no editor, no import step). Null -- warned about once -- when the library is
## unreadable or holds no clips.
func _shared_library_player() -> AnimationPlayer:
	if _lib_tried:
		return _lib_player
	_lib_tried = true
	var lib_path := _shared_library_path()
	if lib_path.is_empty():
		return null
	if lib_path.ends_with(".scn"):
		var packed := ResourceLoader.load(lib_path) as PackedScene
		if packed != null:
			_lib_scene = packed.instantiate()
	else:
		var doc := GLTFDocument.new()
		var state := GLTFState.new()
		if doc.append_from_file(lib_path, state) == OK:
			_lib_scene = doc.generate_scene(state)
	if _lib_scene == null:
		push_warning("[convert] %s exists but would not load -- nothing borrows from it"
			% lib_path)
		return null
	for node in _walk(_lib_scene):
		if node is AnimationPlayer \
				and (node as AnimationPlayer).get_animation_list().size() > 0:
			_lib_player = node
			break
	if _lib_player == null:
		push_warning("[convert] %s holds no AnimationPlayer with clips -- nothing to borrow"
			% lib_path)
	return _lib_player

## Rename @p player's clips to the contract vocabulary, through CLIP_ALIASES.
##
## Handles the two shapes an import produces: clips in the default library under
## raw names ("Human Armature|Walk"), and clips parked in a named library
## ("lib/name" in the list). Everything that aliases moves into the DEFAULT
## library under its contract name -- that is where the loader and the shared
## library both look -- first match wins, and non-aliasing clips stay put.
func _normalise_clip_names(player: AnimationPlayer, label: String) -> void:
	for listed in player.get_animation_list():
		var lib_name := ""
		var anim_name := String(listed)
		if anim_name.contains("/"):
			lib_name = anim_name.get_slice("/", 0)
			anim_name = anim_name.get_slice("/", 1)
		# "Human Armature|Walk" -> "Walk" -> "walk"
		var base := anim_name.get_slice("|", anim_name.count("|")) \
			.to_lower().replace(" ", "").replace("_", "")
		if not CLIP_ALIASES.has(base):
			continue
		var target: String = CLIP_ALIASES[base]
		if player.has_animation(target):
			if anim_name != target:
				push_warning("[convert] %s: clip '%s' also aliases to '%s', which is taken; kept as is"
					% [label, listed, target])
			continue
		var lib := player.get_animation_library(lib_name)
		var anim := lib.get_animation(anim_name)
		lib.remove_animation(anim_name)
		var default_lib := player.get_animation_library("")
		if default_lib == null:
			default_lib = AnimationLibrary.new()
			player.add_animation_library("", default_lib)
		default_lib.add_animation(target, anim)
		print("[convert]   clip '%s' -> '%s'" % [listed, target])

## Copy the @p missing contract clips from the shared library into @p player's default
## library, retargeted onto @p skeleton *by track name*: each track's node path is
## rewritten to point at this model's skeleton, its `:bone_name` subname kept. That is
## the whole retarget, and it is only correct because every model is rigged to the same
## skeleton -- same bone names, same rest orientations. Across different rigs the same
## copy produces garbage with no error, which is why the README states one standard
## skeleton as a contract rather than as advice. Tracks naming bones this skeleton does
## not have are dropped (warned, once per clip): a partial match is almost always a
## different rig wearing familiar names.
func _borrow_clips(player: AnimationPlayer, skeleton: Skeleton3D,
		missing: Array[String], id: String) -> void:
	var source := _shared_library_player()
	if source == null:
		return
	# The node half of every rewritten path: from the player's animation root -- the node
	# its `root_node` points at, its parent by default -- to this model's skeleton. Bone
	# tracks address "node_path:bone", so this plus the kept subname is the whole path.
	var skel_path := String(player.get_node(player.root_node).get_path_to(skeleton))
	if not player.has_animation_library(""):
		# The *default* library, named "", which is what leaves clip names unprefixed --
		# the runtime asks for "walk", not "library/walk". A player created above (the
		# bare-rig case) starts with no libraries at all.
		player.add_animation_library("", AnimationLibrary.new())
	var library := player.get_animation_library("")
	for wanted in missing:
		if not source.has_animation(wanted):
			continue # stays missing; the warnings below say so, same as with no library
		var anim: Animation = source.get_animation(wanted).duplicate(true)
		var dropped := PackedStringArray()
		for track in range(anim.get_track_count() - 1, -1, -1):
			var bone := anim.track_get_path(track).get_concatenated_subnames()
			if bone.is_empty() or skeleton.find_bone(bone) < 0:
				# A track with no subname names a node of the library scene, and no node
				# of that scene exists here -- listed by its whole path, dropped the same.
				dropped.append(String(anim.track_get_path(track)) if bone.is_empty() else bone)
				anim.remove_track(track)
			else:
				anim.track_set_path(track, NodePath("%s:%s" % [skel_path, bone]))
		dropped.reverse() # collected walking backwards; listed in track order
		if anim.get_track_count() == 0:
			# Nothing survived, so this is not the library's skeleton. An empty clip
			# would pass every downstream check and play a T-pose, so it is refused.
			push_warning(("[convert] %s: no bone in the library's '%s' exists on this "
				+ "skeleton (%s) -- not borrowed. Borrowing needs the one standard "
				+ "skeleton; see the README.") % [id, wanted, ", ".join(dropped)])
			continue
		if not dropped.is_empty():
			push_warning(("[convert] %s: '%s' dropped tracks for bones this skeleton "
				+ "does not have: %s. A partial match usually means a different rig, "
				+ "and the clip will play wrong rather than fail.")
				% [id, wanted, ", ".join(dropped)])
		library.add_animation(wanted, anim)
		print("[convert]   borrowed '%s' from %s (%d tracks, %d dropped)" % [
			wanted, SHARED_CLIPS, anim.get_track_count(), dropped.size()])

## Every node under @p root, itself included.
func _walk(root: Node) -> Array[Node]:
	var out: Array[Node] = [root]
	for child in root.get_children():
		out.append_array(_walk(child))
	return out
