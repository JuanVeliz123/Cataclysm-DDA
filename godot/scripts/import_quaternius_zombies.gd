extends Node
## Turn the Quaternius "Zombie Apocalypse Kit" (March 2024, CC0,
## quaternius.com/packs/zombieapocalypsekit.html) into committed creature sources --
## the record of which kit file became which monster, like the humans importer
## beside it.
##
##   QUATERNIUS_PACKS=/path/to/downloads \
##   godot --headless --path godot res://scenes/import_quaternius_zombies.tscn
##
## Far less to do than the humans needed: the kit ships self-contained glTF --
## buffers AND textures embedded as data URIs, clips cleanly named (Idle, Walk,
## Run, Punch, Death, HitReact...), correct side up -- so this only re-packs each
## .gltf as the smaller binary .glb under its CDDA monster id and lets the
## ordinary converter do everything else, including the alias renames.

## id -> path under QUATERNIUS_PACKS. The fits: Chubby is the fat zombie,
## Arm (the big-arm mutant) reads as the brute, Ribcage as the rotting one.
## The shepherd is a freebie the kit happened to carry, and dogs are
## everywhere in-game; the plain mon_dog gets the same model because a
## generic dog beats no dog.
const MODELS := {
	"mon_zombie": "Zombie Apocalypse Kit - March 2024/Characters/glTF/Zombie_Basic.gltf",
	"mon_zombie_fat": "Zombie Apocalypse Kit - March 2024/Characters/glTF/Zombie_Chubby.gltf",
	"mon_zombie_brute": "Zombie Apocalypse Kit - March 2024/Characters/glTF/Zombie_Arm.gltf",
	"mon_zombie_rot": "Zombie Apocalypse Kit - March 2024/Characters/glTF/Zombie_Ribcage.gltf",
	"mon_dog_gshepherd": "Zombie Apocalypse Kit - March 2024/Characters/glTF/Characters_GermanShepherd.gltf",
	"mon_dog": "Zombie Apocalypse Kit - March 2024/Characters/glTF/Characters_GermanShepherd.gltf",
}

const OUT_DIR := "res://meshes/creatures"

func _ready() -> void:
	var src := OS.get_environment("QUATERNIUS_PACKS")
	if src.is_empty():
		push_error("[packs] set QUATERNIUS_PACKS to the directory holding the kit folder")
		get_tree().quit(1)
		return
	var failed := 0
	for id in MODELS:
		if not _import(id, src.path_join(MODELS[id])):
			failed += 1
	print("[packs] %d of %d imported" % [MODELS.size() - failed, MODELS.size()])
	get_tree().quit(1 if failed > 0 else 0)

func _import(id: String, gltf_path: String) -> bool:
	if not FileAccess.file_exists(gltf_path):
		push_error("[packs] missing %s" % gltf_path)
		return false
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	if doc.append_from_file(gltf_path, state) != OK:
		push_error("[packs] %s did not read as glTF" % gltf_path)
		return false
	var scene := doc.generate_scene(state)
	if scene == null:
		push_error("[packs] %s produced no scene" % gltf_path)
		return false
	add_child(scene)
	# Same defence the humans importer needed: a MeshInstance3D whose skeleton
	# NodePath is empty skins fine at runtime and exports NO skin through
	# GLTFDocument -- the failure is a statue that looks converted.
	var skeleton: Skeleton3D = scene.find_child("Skeleton3D", true, false)
	if skeleton != null:
		for node in scene.find_children("*", "MeshInstance3D", true, false):
			(node as MeshInstance3D).skeleton = (node as MeshInstance3D).get_path_to(skeleton)
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
	print("[packs] %s <- %s" % [out.get_file(), gltf_path.get_file()])
	return true
