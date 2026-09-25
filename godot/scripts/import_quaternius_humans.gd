extends Node
## Turn the Quaternius "Animated Man" / "Animated Woman" packs into this project's
## committed creature sources -- the record of where the human models came from and
## how their skin tones were assigned, in the same spirit as compose-tileset.sh.
##
##   QUATERNIUS_PACKS=/path/to/downloads \
##   godot --headless --path godot res://scenes/import_quaternius_humans.tscn
##
## The packs (CC0, quaternius.com/packs/animatedman.html and .../animatedwoman.html)
## ship FBX plus skin-tone textures as loose PNGs -- the clothes are painted in the
## texture, so one mesh wears every outfit. This reads each FBX through FBXDocument
## (no Blender, no editor import), bakes the chosen tone's PNG into the material,
## and exports one self-contained `<id>.glb` per creature id below. The ordinary
## converter then takes those to `.scn` exactly as it takes any rigged model --
## including renaming the packs' "Armature|Walking"-style clips through its alias
## table, so nothing here touches animations at all.
##
## Expected inside QUATERNIUS_PACKS (the Drive folders, downloaded as-is):
##   Man Animated - Oct 2017/FBX/Animated Human.fbx
##   Man Animated - Oct 2017/Blend/Textures/Clothed{Light,Dark}Skin*.png
##   Woman Animated - Dec 2017/FBX/Animated Woman.fbx
##   Woman Animated - Dec 2017/Blends/{Light,Dark}Skin*.png

## id -> [fbx relative path, texture relative path, upright degrees about X]. The
## tone split is the point: the player characters wear the light-skin texture and
## the NPC ids the dark, so the two people on screen in any conversation read as
## two people. Arbitrary, and trivially re-assignable -- each id is one line and a
## re-run.
##
const MODELS := {
	"player_male": ["Man Animated - Oct 2017/FBX/Animated Human.fbx",
		"Man Animated - Oct 2017/Blend/Textures/ClothedLightSkin.png"],
	"npc_male": ["Man Animated - Oct 2017/FBX/Animated Human.fbx",
		"Man Animated - Oct 2017/Blend/Textures/ClothedDarkSkin1.png"],
	"player_female": ["Woman Animated - Dec 2017/FBX/Animated Woman.fbx",
		"Woman Animated - Dec 2017/Blends/LightSkin.png"],
	"npc_female": ["Woman Animated - Dec 2017/FBX/Animated Woman.fbx",
		"Woman Animated - Dec 2017/Blends/DarkSkin1.png"],
}

const OUT_DIR := "res://meshes/creatures"

func _ready() -> void:
	var src := OS.get_environment("QUATERNIUS_PACKS")
	if src.is_empty():
		push_error("[packs] set QUATERNIUS_PACKS to the directory holding the two "
			+ "downloaded pack folders (see this script's header)")
		get_tree().quit(1)
		return
	var failed := 0
	for id in MODELS:
		if not _import(id, src.path_join(MODELS[id][0]), src.path_join(MODELS[id][1])):
			failed += 1
	print("[packs] %d of %d imported" % [MODELS.size() - failed, MODELS.size()])
	get_tree().quit(1 if failed > 0 else 0)

func _import(id: String, fbx_path: String, tex_path: String) -> bool:
	if not FileAccess.file_exists(fbx_path):
		push_error("[packs] missing %s" % fbx_path)
		return false
	var doc := FBXDocument.new()
	var state := FBXState.new()
	if doc.append_from_file(fbx_path, state) != OK:
		push_error("[packs] %s did not read as FBX" % fbx_path)
		return false
	var scene := doc.generate_scene(state)
	if scene == null:
		push_error("[packs] %s produced no scene" % fbx_path)
		return false
	add_child(scene)
	_auto_upright(scene, id)

	# The tone. The FBX references no texture at all -- the material arrives bare,
	# which is what makes the tone a choice here rather than an edit in a DCC tool.
	var img := Image.load_from_file(tex_path)
	if img == null:
		push_error("[packs] could not read %s" % tex_path)
		scene.queue_free()
		return false
	var tex := ImageTexture.create_from_image(img)
	var skeleton: Skeleton3D = null
	var skinned := 0
	for node in _walk(scene):
		if node is Skeleton3D:
			skeleton = node
	for node in _walk(scene):
		var mi := node as MeshInstance3D
		if mi == null or mi.mesh == null:
			continue
		for s in mi.mesh.get_surface_count():
			var mat := StandardMaterial3D.new()
			mat.albedo_texture = tex
			mat.roughness = 0.9
			mi.mesh.surface_set_material(s, mat)
		# The glTF exporter writes NO skin for a MeshInstance3D whose skeleton
		# NodePath is empty -- runtime skinning tolerates the empty path, the
		# exporter does not, and the failure is a statue that looks converted.
		# (Learned the hard way by the shared-clips test fixture.)
		if skeleton != null:
			mi.skeleton = mi.get_path_to(skeleton)
			skinned += 1

	var out := "%s/%s.glb" % [OUT_DIR, id]
	var gltf := GLTFDocument.new()
	var gstate := GLTFState.new()
	var err := gltf.append_from_scene(scene, gstate)
	if err == OK:
		err = gltf.write_to_filesystem(gstate, out)
	remove_child(scene)
	scene.queue_free()
	if err != OK:
		push_error("[packs] could not write %s (error %d)" % [out, err])
		return false
	print("[packs] %s <- %s + %s (%d skinned mesh(es))" % [
		out.get_file(), fbx_path.get_file(), tex_path.get_file(), skinned])
	return true

## Stand the figure up and face it +Z, measured off its own named bones.
##
## The two packs disagree about up -- the man's FBX arrives standing, the woman's
## lying on her back; same author, two months apart, different exporter settings --
## and hand-tuned per-file degrees is the fix that has to be re-tuned for every
## future pack. The rig itself says which way is which: up is feet-to-head, the
## character's left is the hand span, forward is their cross product (for a Y-up,
## +Z-forward character: left x up = forward). The corrective rotation is then the
## inverse of the measured frame, applied as a display transform on the scene root
## so bones, skins and clips all turn together. Bones, not the mesh: skinning
## follows bones, and this woman's mesh rest box is ~85x smaller than her skeleton.
func _auto_upright(scene: Node3D, id: String) -> void:
	var sk: Skeleton3D = scene.find_child("Skeleton3D", true, false)
	if sk == null:
		return
	# Shoulders, not hands, for the character's left: arms hang wherever the
	# rest pose felt like leaving them -- this man's hang forward-and-across,
	# which twisted his measured frame ~25 degrees and every converted model
	# with it, so due south on screen read as south-east. Shoulder joints are
	# rigid on the torso and say what the body actually faces. Hands remain the
	# fallback for rigs without shoulder bones.
	var left_name := "LeftShoulder" if sk.find_bone("LeftShoulder") >= 0 else "LeftHand"
	var right_name := "RightShoulder" if sk.find_bone("RightShoulder") >= 0 else "RightHand"
	var need := ["Head", "LeftFoot", left_name, right_name]
	var at := {}
	for want in need:
		var i := sk.find_bone(want)
		if i < 0:
			print("[packs] %s: no '%s' bone; leaving orientation as shipped" % [id, want])
			return
		at[want] = (sk.global_transform * sk.get_bone_global_pose(i)).origin
	var up: Vector3 = (at["Head"] - at["LeftFoot"]).normalized()
	var left: Vector3 = (at[left_name] - at[right_name]).normalized()
	# Orthogonalise: hands are rarely exactly level with each other.
	left = (left - up * left.dot(up)).normalized()
	var forward := left.cross(up).normalized()
	var measured := Basis(left, up, forward)
	var fix := measured.inverse()
	if fix.is_equal_approx(Basis.IDENTITY):
		return
	scene.transform = Transform3D(fix, Vector3.ZERO) * scene.transform
	print("[packs] %s: uprighted (up was %s, forward was %s)" % [id,
		str(up.snapped(Vector3.ONE * 0.01)), str(forward.snapped(Vector3.ONE * 0.01))])

func _walk(root: Node) -> Array[Node]:
	var out: Array[Node] = [root]
	for child in root.get_children():
		out.append_array(_walk(child))
	return out
