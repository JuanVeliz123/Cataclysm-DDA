extends RefCounted
## The terrain/furniture mesh library (3D-8d): `meshes/terrain/<id>.res`, one
## ArrayMesh per CDDA id, drawn by MapView3D in place of that id's sprite.
##
## The convention, which geometry_check.gd enforces:
##   - one unit is one TILE: footprint inside a 1x1 tile box, height at most
##     1.5 tiles, proportions kept;
##   - feet at the origin, centred on x and z, +y up;
##   - material colour baked into vertex COLOR -- the renderer's shader carries
##     CDDA's tint in INSTANCE_CUSTOM and has no atlas to sample;
##   - underscore-prefixed files are libraries or fixtures, not furniture, and
##     are skipped -- the same rule the creature directory follows.
##
## `import_quaternius_furniture.gd` writes assets in this shape; anything else
## that follows the rules above joins the library by being in the directory.

const MESH_DIR := "res://meshes/terrain"

## id -> { mesh: Mesh, box: AABB }. Static, scanned once per run: the library
## is committed content, not per-session state.
static var _lib: Dictionary = {}
static var _scanned := false

static func library() -> Dictionary:
	if _scanned:
		return _lib
	_scanned = true
	var dir := DirAccess.open(MESH_DIR)
	if dir == null:
		return _lib
	for file in dir.get_files():
		if file.get_extension() != "res":
			continue
		var id := file.get_basename()
		if id.is_empty() or id.begins_with("_"):
			continue
		var mesh := ResourceLoader.load("%s/%s" % [MESH_DIR, file]) as Mesh
		if mesh == null:
			push_warning("terrain_meshes: %s did not load as a Mesh" % file)
			continue
		_lib[id] = {"mesh": mesh, "box": mesh.get_aabb()}
	if not _lib.is_empty():
		print("terrain_meshes: %d meshes in the library" % _lib.size())
	return _lib
