extends Node
## Check that the 3D backend puts sprites where the artist drew them.
##
##   godot --headless --path godot res://scenes/geometry_check.tscn
##
## Exit status is 0 when every placement round-tripped and 1 otherwise.
##
## This is the gate for ADR-006's option B, and the reason the tilt could be built
## before the light that makes it visible. Standing the world up is *supposed* to change
## nothing about where anything lands: ground quads are pre-stretched by `1/sin` along
## the rows, standing quads by `1/cos` upward, and the camera's tilt cancels both
## exactly. "Supposed to" is the part that has cost this branch the most, and this is
## arithmetic -- so run it. Place a sprite, project it back through the camera, and see
## whether the rectangle comes home.
##
## Needs no GDExtension, no GPU and no game: `Camera3D.unproject_position` is maths on a
## transform. That is what makes it a gate rather than a screenshot.
##
## It drives the tilt directly rather than reading `MapView3D.TILT_DEGREES`, so it covers
## angles nobody has switched on yet -- including the ones the next person will try.

const MAP_VIEW_3D := preload("res://scripts/map_view_3d.gd")

## Tilts to check. 0 is the shipping flat world; the rest bracket the band ADR-006's
## arithmetic says is physically sensible for a 64px sprite in a 32px cell.
const TILTS := [0.0, 30.0, 45.0, 51.0, 60.0, 75.0]

## One pixel of slack, for float and for unproject's own rounding. A stretch that is
## wrong is wrong by tens of pixels; there is no useful failure at 1.1.
const TOLERANCE := 1.0

## Sprites to place, in the two shapes the placement branches on.
const CASES := [
	{"name": "floor 32x32", "w": 32.0, "h": 32.0, "tall": false},
	{"name": "road 32x32 offset", "w": 32.0, "h": 32.0, "tall": false, "x": 64.0, "y": 32.0},
	{"name": "wall 32x64", "w": 32.0, "h": 64.0, "tall": true},
	{"name": "tree 64x96", "w": 64.0, "h": 96.0, "tall": true},
	{"name": "creature 32x48", "w": 32.0, "h": 48.0, "tall": true},
]

func _ready() -> void:
	var failures: Array[String] = []
	var checked := 0

	var sub := SubViewport.new()
	sub.name = "GeometryWorld"
	sub.size = Vector2i(1280, 720)
	sub.own_world_3d = true
	add_child(sub)

	var view := Node3D.new()
	view.name = "MapView3D"
	view.set_script(MAP_VIEW_3D)
	sub.add_child(view)

	# Everything the placement needs, without a game to ask: a tile size and a
	# published extent. `setup()` is skipped deliberately -- it wants a host, and none
	# of what it builds (atlases, particles, canvas) has an opinion about geometry.
	view._tile_size = Vector2i(32, 32)
	view._view_size = Vector2i(41, 23)
	view._ensure_camera()
	# The orthographic baseline is the claim under test: the pre-stretch cancels the
	# tilt *under an orthographic projection*, so the perspective default is forced
	# off before anything is measured. The telephoto's drift off this baseline is a
	# chosen trade (see PERSPECTIVE in map_view_3d.gd), not a defect to catch -- it
	# is reported with numbers further down instead of asserted about.
	view.set_perspective(false)

	for tilt in TILTS:
		# Through the setter, so the camera and the placement agree about the angle.
		# Setting the trigonometry directly was this check's second wrong answer:
		# `_update_camera` recomputed it all from the constant, so every case ran the
		# flat path and thirty green lines meant the tilt had never been tested. A 5%
		# error injected into the ground stretch was still reported as 0.00 px out.
		view.set_tilt_degrees(tilt)
		view._update_camera()
		# Where the 2D backend would have drawn it, which is the actual claim: MapView
		# centres the published block in the drawable area and draws map pixels inside
		# it, so a map bigger than the viewport starts at a negative offset. Comparing
		# against raw map pixels instead was this check's first answer, and it reported
		# every case as 17.89 px out -- the same 17.89 px, at every tilt, which is what
		# a wrong expectation looks like next to a right one.
		var area := Vector2(sub.size)
		var map_size := Vector2(view._view_size) * Vector2(view._tile_size)
		var origin := (area - map_size) * 0.5
		for c in CASES:
			checked += 1
			var drawn := Rect2(float(c.get("x", 96.0)), float(c.get("y", 128.0)),
				float(c["w"]), float(c["h"]))
			var want := Rect2(drawn.position + origin, drawn.size)
			var got: Rect2 = view.debug_projected_rect(drawn.position.x, drawn.position.y,
				drawn.size.x, drawn.size.y, bool(c["tall"]))
			var off := (got.position - want.position).length() \
				+ (got.size - want.size).length()
			var ok := off <= TOLERANCE
			print("[geom] tilt %5.1f  %-22s want %s  got %s  off %5.2f px  %s" % [
				tilt, c["name"], _terse(want), _terse(got), off, "ok" if ok else "FAIL"])
			if not ok:
				failures.append("%s at %.1f deg is %.2f px out (want %s, got %s)" % [
					c["name"], tilt, off, _terse(want), _terse(got)])

	# The world canvas (3D-1d). The fallback glyphs and the animation overlay stay canvas
	# items at any tilt, and the whole of that claim is that the canvas transform agrees
	# with the camera about where a ground pixel goes. The two are derived separately -- one
	# from the zoom and the map's extent, the other from a camera position and a rotation --
	# so agreeing is a fact about them rather than a restatement.
	#
	# They were hidden while tilted for a day on the assumption that an affine transform
	# cannot follow a rotated camera. It does not have to: a ground point's screen position
	# is unchanged by the tilt by construction, which is what the floor cases above measure.
	view._ensure_canvas()
	for tilt in TILTS:
		view.set_tilt_degrees(tilt)
		view._update_camera()
		checked += 1
		var probe := Vector2(96.0, 128.0)
		var on_canvas: Vector2 = view.debug_canvas_point(probe)
		var by_camera: Vector2 = view.debug_projected_rect(probe.x, probe.y, 32.0, 32.0,
			false).position
		var gap := (on_canvas - by_camera).length()
		var ok := gap <= TOLERANCE
		print("[geom] tilt %5.1f  canvas puts %s, camera puts %s, gap %5.2f px  %s" % [
			tilt, _point(on_canvas), _point(by_camera), gap, "ok" if ok else "FAIL"])
		if not ok:
			failures.append("the world canvas and the camera disagree by %.2f px at %.1f deg"
				% [gap, tilt])

	# The telephoto's cost, in numbers rather than adjectives: how far a ground tile's
	# screen position moves when the perspective default switches on, at the centre of
	# the map and at its corners -- the worst case for the affine canvas. Reported, not
	# asserted: the drift is the documented trade at PERSPECTIVE, and this line is what
	# a FOV change gets judged against.
	view.set_tilt_degrees(MAP_VIEW_3D.TILT_DEGREES)
	view._update_camera()
	for probe_xy in [Vector2(656.0, 368.0), Vector2(0.0, 0.0), Vector2(1280.0, 704.0)]:
		var flat_rect: Rect2 = view.debug_projected_rect(probe_xy.x, probe_xy.y,
			32.0, 32.0, false)
		view.set_perspective(true)
		var tele_rect: Rect2 = view.debug_projected_rect(probe_xy.x, probe_xy.y,
			32.0, 32.0, false)
		view.set_perspective(false)
		print("[geom] telephoto fov %.0f: ground (%4.0f,%3.0f) drifts %5.1f px off the canvas"
			% [MAP_VIEW_3D.PERSPECTIVE_FOV_DEGREES, probe_xy.x, probe_xy.y,
				(tele_rect.position - flat_rect.position).length()])

	# Contact shadows (ADR-005 item 4, under a tilted camera). A blob must land on the
	# creature's own feet: directly under its sprite's base horizontally, and exactly
	# LIFT tiles' worth *up* the screen from it, which is where the art puts the contact
	# point. Two separate bugs made characters look as though they floated above their own
	# shadows -- the blob reconstructed its height instead of taking the anchor's, which
	# broke the cancellation that makes the depth-rank nudge invisible, and LIFT had been
	# dropped on the reading that a standing sprite's anchor is its feet. It is the bottom
	# edge of its quad.
	var lift_px := 32.0 * float(MAP_VIEW_3D.SHADOW.LIFT)
	for tilt in TILTS:
		view.set_tilt_degrees(tilt)
		view._update_camera()
		checked += 1
		# A creature-sized standing sprite, placed as the batch pass would place it,
		# including the view-axis nudge that the blob has to agree with.
		var placed: Transform3D = view._place(
			MAP_VIEW_3D.MV.tile_transform(96.0, 112.0, 32.0, 48.0, 0, false),
			true, 160.0, 4.0, 0)
		var anchor: Vector3 = placed * Vector3(0.5, 1.0, 0.0)
		var blob: Transform3D = view.shadow_transform(anchor, 32.0, 0.0)
		# Typed, because `view` is a Node3D as far as the compiler knows and an inferred
		# type cannot come through an unsafe call.
		var feet: Vector2 = view._camera.unproject_position(anchor)
		# The blob quad's centre: half of each basis vector from its origin. Both modes
		# build it that way, so one expectation covers them.
		var centre: Vector2 = view._camera.unproject_position(blob * Vector3(0.5, 0.5, 0.0))
		var gap := Vector2(centre.x - feet.x, centre.y - (feet.y - lift_px))
		var ok := gap.length() <= TOLERANCE
		print("[geom] tilt %5.1f  feet %s, blob %s, want %.1f px above  off %5.2f px  %s" % [
			tilt, _point(feet), _point(centre), lift_px, gap.length(), "ok" if ok else "FAIL"])
		if not ok:
			failures.append("the contact blob is %.2f px from the feet at %.1f deg" % [
				gap.length(), tilt])

	# The shadow proxy standing where the creature stands. Its base must land on the feet:
	# a capsule floating over them casts a shadow that reads as a hovering object, which is
	# the bug the blob just had in a different form.
	for tilt in [45.0, 60.0]:
		view.set_tilt_degrees(tilt)
		view._update_camera()
		checked += 1
		var placed: Transform3D = view._place(
			MAP_VIEW_3D.MV.tile_transform(96.0, 112.0, 32.0, 48.0, 0, false),
			true, 160.0, 4.0, 0)
		var anchor: Vector3 = placed * Vector3(0.5, 1.0, 0.0)
		var proxy: Transform3D = view.proxy_transform(anchor, 32.0,
			placed.basis.y.length())
		# The unit capsule spans -1..1 in its own y, so its foot is at local (0,-1,0).
		var foot: Vector2 = view._camera.unproject_position(proxy * Vector3(0.0, -1.0, 0.0))
		var feet: Vector2 = view._camera.unproject_position(anchor)
		var off := (foot - feet).length()
		var ok := off <= TOLERANCE
		print("[geom] tilt %5.1f  proxy foot %s, creature feet %s, off %5.2f px  %s" % [
			tilt, _point(foot), _point(feet), off, "ok" if ok else "FAIL"])
		if not ok:
			failures.append("the shadow proxy stands %.2f px off its creature's feet at %.1f deg"
				% [off, tilt])

	# Levels below the avatar (3D-4). The drop is a height, so the projection turns it
	# into an exact number of pixels down the screen -- LEVEL_DROP_TILES tiles' worth per
	# level -- and into no sideways movement at all. Both halves are the claim: a level
	# that also slid sideways would be a level in the wrong place.
	var drop_px := float(MAP_VIEW_3D.LEVEL_DROP_TILES) * 32.0
	for tilt in [0.0, 45.0, 60.0]:
		view.set_tilt_degrees(tilt)
		view._update_camera()
		for level in [1, 2]:
			checked += 1
			var top: Rect2 = view.debug_projected_rect(96.0, 128.0, 32.0, 32.0, false, 0)
			var under: Rect2 = view.debug_projected_rect(96.0, 128.0, 32.0, 32.0, false,
				level)
			# Flat, levels stay coplanar: that is the 2D backend's behaviour and the
			# baseline this one is measured against.
			var want_shift := 0.0 if tilt <= 0.01 else drop_px * float(level)
			var shift := under.position.y - top.position.y
			var sideways := absf(under.position.x - top.position.x)
			var ok := absf(shift - want_shift) <= TOLERANCE and sideways <= TOLERANCE
			print("[geom] tilt %5.1f  level -%d drops %6.2f px (want %6.2f), sideways %.2f  %s"
				% [tilt, level, shift, want_shift, sideways, "ok" if ok else "FAIL"])
			if not ok:
				failures.append("level -%d at %.1f deg drops %.2f px and slides %.2f (want %.2f, 0)"
					% [level, tilt, shift, sideways, want_shift])

	# Every creature mesh that exists, against the conventions a mesh has to follow (3D-7c).
	#
	# Loaded through `creature_meshes.gd`'s own loader rather than a copy of it, so this
	# checks the path the game uses. Two mistakes are worth catching automatically because
	# they are the two a modeller actually makes: an origin at the body's centre, which
	# plants the creature half underground, and a scale in metres rather than in tile
	# pixels, which makes a person the size of a doorframe or of a mouse.
	#
	# An id with a `.scn` beside (or instead of) its mesh is an *animated* asset and is
	# checked as a scene in `_check_animated` below -- same box conventions, plus the
	# failures only a scene can have.
	var loader := Node3D.new()
	loader.set_script(preload("res://scripts/creature_meshes.gd"))
	add_child(loader)
	loader.setup(null)
	var dir := DirAccess.open(loader.MESH_DIR)
	if dir != null:
		var seen: Dictionary = {}
		for file in dir.get_files():
			var id := file.get_basename()
			# Only the extensions the loader itself resolves, plus `.scn` -- the
			# animated scene, which the loader prefers over all of them. The
			# directory also holds a README and, after any editor run, `.import`
			# sidecars -- whose basename still ends in .glb and reads as a
			# brand-new id that then "fails to load".
			var ext := "." + file.get_extension()
			if id.is_empty() or (ext != ".scn" and not loader.MESH_EXTENSIONS.has(ext)):
				continue
			# Underscore-prefixed files are libraries, not creatures: _shared_clips
			# is typically a skeleton and its clips with no mesh at all (a Mixamo
			# animation pack), and judging it as a body would fail it for being
			# exactly what it is meant to be.
			if id.begins_with("_"):
				continue
			# One check per id, not per file: a creature usually has a source beside its
			# converted mesh -- .glb and .res and sometimes .obj all named for the same
			# thing -- and the loader resolves all of them to whichever it prefers. An id
			# with a `.scn` is checked as that scene and nothing else, because the scene
			# is what the game will be handed.
			if seen.has(id):
				continue
			seen[id] = true
			var scn_path := "%s/%s.scn" % [loader.MESH_DIR, id]
			if ResourceLoader.exists(scn_path):
				checked += 1
				_check_animated(id, scn_path, failures)
				continue
			var mesh: Mesh = loader._mesh_for(id)
			if mesh == null:
				failures.append("%s/%s did not load as a mesh" % [loader.MESH_DIR, file])
				continue
			checked += 1
			var box := mesh.get_aabb()
			# Standing on the origin, centred on it, and a creature's size in tile pixels.
			var stands := absf(box.position.y) <= 1.0
			var centred := absf(box.position.x + box.size.x * 0.5) <= 4.0 \
				and absf(box.position.z + box.size.z * 0.5) <= 6.0
			var sized := box.size.y >= 8.0 and box.size.y <= 160.0
			var ok := stands and centred and sized
			print("[geom] mesh %-16s %.0f x %.0f x %.0f, base y %+.1f  stands=%s centred=%s sized=%s  %s"
				% [id, box.size.x, box.size.y, box.size.z, box.position.y, str(stands),
					str(centred), str(sized), "ok" if ok else "FAIL"])
			if not ok:
				failures.append(("%s: base y %+.1f (wants 0, the feet), %.0f x %.0f x %.0f "
					+ "in tile pixels") % [id, box.position.y, box.size.x, box.size.y,
					box.size.z])
	else:
		print("[geom] no %s yet -- nothing to check" % loader.MESH_DIR)

	# The terrain mesh library (3D-8d), against terrain_meshes.gd's convention:
	# one unit is one tile, footprint inside the tile, height capped, feet on the
	# origin and centred. The importer normalizes to exactly this, so a failure
	# here is a hand-added asset that skipped the importer -- the mistake worth
	# catching before it renders as a wardrobe the size of a house.
	var tlib: Dictionary = preload("res://scripts/terrain_meshes.gd").library()
	for id in tlib:
		checked += 1
		var tbox: AABB = (tlib[id] as Dictionary)["box"]
		var grounded := absf(tbox.position.y) <= 0.05
		var fits := tbox.size.x <= 1.05 and tbox.size.z <= 1.05 and tbox.size.y <= 1.55
		var tcentred := absf(tbox.position.x + tbox.size.x * 0.5) <= 0.05 \
			and absf(tbox.position.z + tbox.size.z * 0.5) <= 0.05
		var tok := grounded and fits and tcentred
		print("[geom] furn %-16s %.2f x %.2f x %.2f tiles, base y %+.2f  %s" % [
			id, tbox.size.x, tbox.size.y, tbox.size.z, tbox.position.y,
			"ok" if tok else "FAIL"])
		if not tok:
			failures.append("%s: %.2f x %.2f x %.2f tiles at base y %+.2f breaks the library convention"
				% [id, tbox.size.x, tbox.size.y, tbox.size.z, tbox.position.y])

	if failures.is_empty():
		print("[geom] %d placements round-tripped through the camera" % checked)
		get_tree().quit(0)
	else:
		for f in failures:
			push_error("[geom] " + f)
			print("[geom] FAIL " + f)
		get_tree().quit(1)

## An animated creature scene, against the conventions in the meshes README (3D-7c's
## animation amendment). Same box checks as a bare mesh -- an origin at the body's centre
## and a scale in metres are still the mistakes that get made -- plus the ones only a
## scene can get wrong: no Skeleton3D (an unrigged asset should have been a `.res`),
## no `idle` or `walk` (the two the renderer plays constantly, so their absence is a hole
## on screen rather than a missing flourish), and a repeating clip left LOOP_NONE, which
## plays once and freezes -- read elsewhere as a T-pose bug, not as the export slip it is.
## Missing `attack`/`hit`/`die` are WARN lines only, the same two-tier policy as the
## scenario probe: a creature that cannot act yet is still worth standing up.
func _check_animated(id: String, path: String, failures: Array[String]) -> void:
	var packed := ResourceLoader.load(path) as PackedScene
	if packed == null:
		failures.append("%s did not load as a PackedScene" % path)
		return
	var inst := packed.instantiate()
	# In the tree before anything reads a global_transform, same as everywhere else in
	# this branch: outside it they come back identity and the box measures the wrong pose.
	add_child(inst)

	# The posed BONE box, exactly as the converter now measures: a skinned mesh's
	# AABB stays rest-shaped whatever plays, and rest and clip space can disagree
	# wholesale -- the Quaternius woman's mesh rest box is ~85x smaller than her
	# skeleton, so a mesh-box check failed her as "0 x 0 x 0 at y +1.7" while she
	# stood on screen at a perfect 33. Judging by the mesh here while the
	# converter judges by the bones would fail every asset the converter got
	# right. Falls back to the mesh box only when there is nothing to pose.
	var box := AABB()
	var boxed := false
	for node in inst.find_children("*", "AnimationPlayer", true, false):
		var ap := node as AnimationPlayer
		if ap.has_animation("idle"):
			ap.play("idle")
			ap.advance(0.0)
		break
	for node in inst.find_children("*", "Skeleton3D", true, false):
		var sk := node as Skeleton3D
		for i in sk.get_bone_count():
			var p: Vector3 = (sk.global_transform * sk.get_bone_global_pose(i)).origin
			box = AABB(p, Vector3.ZERO) if not boxed else box.expand(p)
			boxed = true
	if boxed:
		box = box.grow(box.size.y * 0.03)
	# UNION with the mesh box, not either alone: bone origins alone shave a rig
	# whose joints stop above the ankles (the 7-bone mannequin has no foot bones
	# and measured as floating at +13), and the mesh alone is the degenerate box
	# the comment above is about. The union stands wherever either is honest,
	# and a mesh box that is nonsense is tiny, so it cannot drag the union far.
	for node in inst.find_children("*", "MeshInstance3D", true, false):
		var mi := node as MeshInstance3D
		if mi.mesh == null:
			continue
		var part: AABB = mi.global_transform * mi.mesh.get_aabb()
		box = part if not boxed else box.merge(part)
		boxed = true

	var stands := boxed and absf(box.position.y) <= 1.0
	var centred := boxed and absf(box.position.x + box.size.x * 0.5) <= 4.0 \
		and absf(box.position.z + box.size.z * 0.5) <= 6.0
	var sized := boxed and box.size.y >= 8.0 and box.size.y <= 160.0
	if not (stands and centred and sized):
		failures.append(("%s: base y %+.1f (wants 0, the feet), %.0f x %.0f x %.0f "
			+ "in tile pixels") % [id, box.position.y, box.size.x, box.size.y,
			box.size.z])

	var rigged := inst.find_children("*", "Skeleton3D", true, false).size() > 0
	if not rigged:
		failures.append("%s.scn has no Skeleton3D -- an unrigged asset should be a .res" % id)

	var players := inst.find_children("*", "AnimationPlayer", true, false)
	var clips := PackedStringArray()
	if players.is_empty():
		failures.append("%s.scn has no AnimationPlayer" % id)
	else:
		clips = (players[0] as AnimationPlayer).get_animation_list()
	var animated := true
	for wanted in ["idle", "walk"]:
		if not clips.has(wanted):
			animated = false
			if not players.is_empty():
				failures.append("%s.scn has no '%s' clip, and the renderer always plays one"
					% [id, wanted])
	var loops := true
	for clip_name in ["idle", "walk", "run"]:
		if not clips.has(clip_name):
			continue
		var anim: Animation = (players[0] as AnimationPlayer).get_animation(clip_name)
		if anim.loop_mode == Animation.LOOP_NONE:
			loops = false
			failures.append("%s: '%s' does not loop -- it plays once and the figure freezes"
				% [id, clip_name])

	var ok := stands and centred and sized and rigged and not players.is_empty() \
		and animated and loops
	print("[geom] anim %-16s %.0f x %.0f x %.0f, base y %+.1f  stands=%s centred=%s sized=%s rig=%s clips=[%s] loops=%s  %s"
		% [id, box.size.x, box.size.y, box.size.z, box.position.y, str(stands),
			str(centred), str(sized), str(rigged), ", ".join(clips), str(loops),
			"ok" if ok else "FAIL"])
	for wanted in ["attack", "hit", "die"]:
		if not clips.has(wanted):
			print("[geom] anim %-16s WARN no '%s' clip -- optional, the runtime falls back"
				% [id, wanted])

	remove_child(inst)
	inst.queue_free()

func _point(p: Vector2) -> String:
	return "(%.0f,%.0f)" % [p.x, p.y]

func _terse(r: Rect2) -> String:
	return "(%.0f,%.0f %.0fx%.0f)" % [r.position.x, r.position.y, r.size.x, r.size.y]
