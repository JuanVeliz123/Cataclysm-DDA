extends Node
## Build the example creature mesh, and be the record of how it was built.
##
##   godot --headless --path godot res://scenes/make_example_creature_mesh.tscn
##
## Writes `res://meshes/creatures/player_male.tres`: a blocky humanoid, deliberately crude,
## whose only job is to be a correct example of the *conventions* a real mesh has to follow.
## A modeller can open it, measure it, and throw it away.
##
## Committed as a script rather than only as a mesh for the reason `gfx/` is composed by a
## script rather than committed: the artefact is derivable, and the derivation is the part
## worth reading. Change a number here, re-run, and the numbers below are the spec.
##
## Why `.tres` and not `.glb`: a Mesh resource loads with no import step, and `.glb` needs
## Godot to have imported it into `.godot/` first. That matters while the editor is unusable,
## and it is why `creature_meshes.gd` tries `.tres` first.

## One unit is one tile pixel, so these are the numbers a 32x48 character sprite occupies.
## A person is narrower than the cell they stand in -- that is why the sprite has space
## either side of them -- so nothing here is 32 wide.
const TILE := 32.0
const TOTAL_HEIGHT := 48.0

## What the figure is scaled to on the way out: the height Ultica PAINTS, not the frame
## it paints in. Measured from the composed atlas (2026-08-18): a person is 32-33 opaque
## pixels of the 32x48 frame. The proportions below stay authored against 48 because
## they read better that way; one uniform scale at commit makes the saved mesh match
## the art instead of towering half again over it.
const PAINTED_HEIGHT := 33.0

const HEAD := Vector3(9.0, 9.0, 9.0)
const TORSO := Vector3(14.0, 17.0, 8.0)
const HIPS := Vector3(11.0, 4.0, 8.0)
const LEG := Vector3(5.0, 18.0, 6.0)
const ARM := Vector3(3.5, 15.0, 4.0)

## Where the mesh is saved. The id is the one the tileset keys on, which is the one
## `CDDAHost::get_creatures()` publishes.
const OUT_PATH := "res://meshes/creatures/player_male.tres"

func _ready() -> void:
	var tool := SurfaceTool.new()
	tool.begin(Mesh.PRIMITIVE_TRIANGLES)

	# Stacked from the ground up, because the origin belongs between the feet: a creature is
	# placed at its feet by `feet_to_world`, so a mesh centred on its middle would stand
	# half a body underground.
	var leg_y := LEG.y * 0.5
	_box(tool, LEG, Vector3(-3.0, leg_y, 0.0))
	_box(tool, LEG, Vector3(3.0, leg_y, 0.0))

	var hips_y := LEG.y + HIPS.y * 0.5
	_box(tool, HIPS, Vector3(0.0, hips_y, 0.0))

	var torso_y := LEG.y + HIPS.y + TORSO.y * 0.5
	_box(tool, TORSO, Vector3(0.0, torso_y, 0.0))

	# Arms hang beside the torso, slightly forward, so the silhouette reads as a person
	# from the side as well as from the front. A billboard never had a side.
	var arm_y := LEG.y + HIPS.y + TORSO.y - ARM.y * 0.5 - 1.0
	_box(tool, ARM, Vector3(-(TORSO.x * 0.5 + ARM.x * 0.5), arm_y, 0.5))
	_box(tool, ARM, Vector3(TORSO.x * 0.5 + ARM.x * 0.5, arm_y, 0.5))

	var head_y := LEG.y + HIPS.y + TORSO.y + HEAD.y * 0.5
	_box(tool, HEAD, Vector3(0.0, head_y, 0.0))

	# A nose, and it is not decoration: it is the only thing in the mesh that says which way
	# is front. Facing is the convention most easily got wrong -- a sprite faces left by
	# being mirrored, a mesh has to turn -- so the example makes its front visible.
	_box(tool, Vector3(2.0, 2.0, 2.5), Vector3(0.0, head_y, HEAD.z * 0.5 + 1.0))

	tool.generate_normals()
	# The painted-height scale, applied to the baked vertices -- a bare Mesh has no
	# wrapper node to carry a transform, so the rescale happens here or nowhere.
	var authored: ArrayMesh = tool.commit()
	var sized := SurfaceTool.new()
	sized.begin(Mesh.PRIMITIVE_TRIANGLES)
	sized.append_from(authored, 0,
		Transform3D(Basis.from_scale(Vector3.ONE * (PAINTED_HEIGHT / TOTAL_HEIGHT)),
			Vector3.ZERO))
	var mesh: ArrayMesh = sized.commit()

	# Plain and matte. The mesh is lit by the world's own sun and lamps, which is the point
	# of it being geometry, so anything shiny would be reading the lighting rather than
	# showing it.
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.78, 0.66, 0.56)
	mat.roughness = 0.9
	mesh.surface_set_material(0, mat)

	var top := (head_y + HEAD.y * 0.5) * (PAINTED_HEIGHT / TOTAL_HEIGHT)
	print("[mesh] built %d surface(s), %.1f units tall (the art paints %.0f), origin at the feet"
		% [mesh.get_surface_count(), top, PAINTED_HEIGHT])
	if absf(top - PAINTED_HEIGHT) > 1.0:
		push_warning("the example mesh is %.1f units tall but the art paints %.0f: it "
			% [top, PAINTED_HEIGHT] + "will not stand the same height as the sprites beside it")

	var err := ResourceSaver.save(mesh, OUT_PATH)
	if err != OK:
		push_error("could not write %s (error %d)" % [OUT_PATH, err])
		get_tree().quit(1)
		return
	print("[mesh] wrote ", OUT_PATH)
	get_tree().quit(0)

## Append a box of @p size centred at @p at. Boxes rather than a modelled body because the
## example is about the conventions, and a convention is easier to read off a shape that
## obviously is not art.
func _box(tool: SurfaceTool, size: Vector3, at: Vector3) -> void:
	var box := BoxMesh.new()
	box.size = size
	tool.append_from(box, 0, Transform3D(Basis(), at))
