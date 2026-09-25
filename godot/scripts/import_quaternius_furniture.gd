extends Node
## Turn the Quaternius "Furniture Pack" into this project's committed terrain
## meshes (3D-8d) -- the record of where the furniture models came from, in the
## same spirit as import_quaternius_humans.gd.
##
##   QUATERNIUS_PACKS=/path/to/downloads \
##   godot --headless --path godot res://scenes/import_quaternius_furniture.tscn
##
## The pack (CC0, quaternius.com) ships flat-coloured FBX models -- colours live
## in the materials, no textures at all. Each model is read through FBXDocument
## (no Blender, no editor import), flattened to a single ArrayMesh with the
## material colours baked into vertex COLOR (the mesh batches carry CDDA's tint
## in INSTANCE_CUSTOM, so the vertex channel is free to be the material), and
## normalized to the library convention geometry_check enforces:
##
##   - one unit is one TILE: the footprint fits in a 1x1 tile box, height capped
##     at 1.5 tiles, proportions kept (the cap is what stops a floor lamp scaled
##     by its footprint from standing four tiles tall);
##   - feet at the origin, centred on x and z, +y up.
##
## The .res files it writes are the committed artifacts; nothing rebuilds them at
## boot, because unlike the creature pipeline there is no per-checkout step -- an
## ArrayMesh resource is portable as-is.

## CDDA id -> model file, relative to the pack. One line per id, trivially
## re-assignable. BedKing and the sofa variants sit unused until a multi-tile
## furniture story exists; the vases until someone wants clutter.
const MODELS := {
	"f_bed": "Furniture Pack - Oct 2017/FBX/Bed.fbx",
	"f_table": "Furniture Pack - Oct 2017/FBX/Table.fbx",
	"f_coffee_table": "Furniture Pack - Oct 2017/FBX/CoffeeTable.fbx",
	"f_chair": "Furniture Pack - Oct 2017/FBX/Chair.fbx",
	"f_armchair": "Furniture Pack - Oct 2017/FBX/ChairCushioned.fbx",
	"f_stool": "Furniture Pack - Oct 2017/FBX/Stool.fbx",
	"f_sofa": "Furniture Pack - Oct 2017/FBX/Sofa.fbx",
	"f_bookcase": "Furniture Pack - Oct 2017/FBX/BookCaseBooks.fbx",
	"f_wardrobe": "Furniture Pack - Oct 2017/FBX/Closet.fbx",
	"f_dresser": "Furniture Pack - Oct 2017/FBX/Closet2.fbx",
	"f_floor_lamp": "Furniture Pack - Oct 2017/FBX/Lamp.fbx",
	"f_indoor_plant": "Furniture Pack - Oct 2017/FBX/Plant.fbx",
}

const OUT_DIR := "res://meshes/terrain"
const MAX_HEIGHT_TILES := 1.5

## Material name -> colour. The pack's colours never left Blender -- every FBX
## and OBJ material carries the same grey albedo -- but the material NAMES
## survived ("DarkWood.009", "Red", "Sheets"), so the palette is reconstructed
## from them, in Quaternius's own flat-colour idiom. Numeric suffixes are
## stripped before lookup; an unknown name falls back to the grey it shipped
## with, plus a warning naming it, which is how the table grows.
const PALETTE := {
	"Wood": Color(0.71, 0.53, 0.35),
	"DarkWood": Color(0.43, 0.29, 0.18),
	"DarkBrown": Color(0.36, 0.25, 0.20),
	"Sheets": Color(0.85, 0.87, 0.91),
	"White": Color(0.92, 0.92, 0.90),
	"Red": Color(0.70, 0.23, 0.23),
	"Sofa": Color(0.29, 0.43, 0.54),
	"Green": Color(0.31, 0.55, 0.29),
	"Metal": Color(0.35, 0.37, 0.40),
	"Top": Color(0.91, 0.86, 0.71),
	"Pages": Color(0.90, 0.88, 0.80),
	"Cover2": Color(0.62, 0.28, 0.24),
	"Cover3": Color(0.26, 0.40, 0.60),
	"Cover4": Color(0.36, 0.52, 0.30),
}

func _ready() -> void:
	if get_tree().current_scene != self:
		return
	var packs := OS.get_environment("QUATERNIUS_PACKS")
	if packs.is_empty():
		push_error("QUATERNIUS_PACKS is not set; nothing to import")
		get_tree().quit(1)
		return
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(OUT_DIR))
	var failures := 0
	for id in MODELS:
		if not _import_one(id, packs.path_join(MODELS[id])):
			failures += 1
	print("[furn] done, %d/%d imported" % [MODELS.size() - failures, MODELS.size()])
	get_tree().quit(1 if failures > 0 else 0)

func _import_one(id: String, fbx_path: String) -> bool:
	var doc := FBXDocument.new()
	var state := FBXState.new()
	if doc.append_from_file(fbx_path, state) != OK:
		push_error("%s: could not read %s" % [id, fbx_path])
		return false
	var scene := doc.generate_scene(state)
	if scene == null:
		push_error("%s: FBX produced no scene" % id)
		return false
	add_child(scene)

	# Flatten every surface of every mesh into one vertex soup, with the node
	# transforms applied and each surface's material colour painted onto its
	# vertices. One surface out, so a MultiMesh can draw the piece in one call.
	var verts := PackedVector3Array()
	var normals := PackedVector3Array()
	var colors := PackedColorArray()
	var indices := PackedInt32Array()
	for node in scene.find_children("*", "MeshInstance3D", true, false):
		var mi := node as MeshInstance3D
		if mi.mesh == null:
			continue
		var xf := mi.global_transform
		for s in mi.mesh.get_surface_count():
			var albedo := Color(0.7, 0.7, 0.7)
			var mat := mi.get_active_material(s) as BaseMaterial3D
			if mat != null:
				var mat_name := mat.resource_name.split(".")[0]
				if PALETTE.has(mat_name):
					albedo = PALETTE[mat_name]
				else:
					albedo = mat.albedo_color
					push_warning("%s: material '%s' not in PALETTE, keeping %s"
						% [id, mat.resource_name, str(albedo)])
			var arrays := mi.mesh.surface_get_arrays(s)
			var sv: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
			var sn: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL] \
				if arrays[Mesh.ARRAY_NORMAL] != null else PackedVector3Array()
			var si: PackedInt32Array = arrays[Mesh.ARRAY_INDEX] \
				if arrays[Mesh.ARRAY_INDEX] != null else PackedInt32Array()
			var base := verts.size()
			for v in sv:
				verts.append(xf * v)
			var nxf := xf.basis.inverse().transposed()
			for i in sv.size():
				normals.append((nxf * sn[i]).normalized() if i < sn.size()
					else Vector3.UP)
				colors.append(albedo)
			if si.is_empty():
				for i in sv.size():
					indices.append(base + i)
			else:
				for i in si:
					indices.append(base + i)
	remove_child(scene)
	scene.queue_free()
	if verts.is_empty():
		push_error("%s: no geometry found" % id)
		return false

	# Normalize to the library convention: fit the footprint in one tile, cap
	# the height, feet on the origin, centred. Scale-agnostic on purpose -- FBX
	# unit conventions vary and none of them survive contact with a tileset.
	var box := AABB(verts[0], Vector3.ZERO)
	for v in verts:
		box = box.expand(v)
	var scale := 1.0 / maxf(0.0001, maxf(maxf(box.size.x, box.size.z),
		box.size.y / MAX_HEIGHT_TILES))
	var centre := Vector3(box.position.x + box.size.x * 0.5, box.position.y,
		box.position.z + box.size.z * 0.5)
	for i in verts.size():
		verts[i] = (verts[i] - centre) * scale

	var arrays_out := []
	arrays_out.resize(Mesh.ARRAY_MAX)
	arrays_out[Mesh.ARRAY_VERTEX] = verts
	arrays_out[Mesh.ARRAY_NORMAL] = normals
	arrays_out[Mesh.ARRAY_COLOR] = colors
	arrays_out[Mesh.ARRAY_INDEX] = indices
	var out := ArrayMesh.new()
	out.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays_out)
	var path := "%s/%s.res" % [OUT_DIR, id]
	var err := ResourceSaver.save(out, path)
	var final := AABB(verts[0], Vector3.ZERO)
	for v in verts:
		final = final.expand(v)
	print("[furn] %-16s %5d verts  %.2f x %.2f x %.2f tiles -> %s (err=%d)" % [
		id, verts.size(), final.size.x, final.size.y, final.size.z, path, err])
	return err == OK
