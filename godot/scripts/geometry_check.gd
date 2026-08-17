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
	var loader := Node3D.new()
	loader.set_script(preload("res://scripts/creature_meshes.gd"))
	add_child(loader)
	loader.setup(null)
	var dir := DirAccess.open(loader.MESH_DIR)
	if dir != null:
		for file in dir.get_files():
			var id := file.get_basename()
			if file.get_extension() == "md" or id.is_empty():
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

	if failures.is_empty():
		print("[geom] %d placements round-tripped through the camera" % checked)
		get_tree().quit(0)
	else:
		for f in failures:
			push_error("[geom] " + f)
			print("[geom] FAIL " + f)
		get_tree().quit(1)

func _point(p: Vector2) -> String:
	return "(%.0f,%.0f)" % [p.x, p.y]

func _terse(r: Rect2) -> String:
	return "(%.0f,%.0f %.0fx%.0f)" % [r.position.x, r.position.y, r.size.x, r.size.y]
