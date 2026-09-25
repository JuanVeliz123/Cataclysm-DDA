extends Node
## Compile every shader in res://shaders/ and fail loudly if one is broken.
##
##   godot --headless --path godot res://scenes/shader_check.tscn
##
## Exit status is 0 when every shader compiled and 1 otherwise; the engine
## prints the compiler's own "SHADER ERROR:" line above this script's summary.
##
## Nothing else in this project would notice a broken shader. The headless probe
## builds MapView and its materials, but never draws, and a ShaderMaterial with
## a doomed shader is constructed perfectly happily. The editor load check only
## parses GDScript. So a typo in a .gdshader passes every gate and then shows up
## as an unlit black map on the first machine with a display.
##
## What makes this work headlessly is that the dummy rendering driver still runs
## Godot's own shader language front end when the code is assigned -- it rejects
## syntax errors, unknown identifiers and type mismatches, which is nearly every
## way a shader is broken by hand. What it cannot catch is the GPU backend
## refusing otherwise-valid code; that still needs a real driver.

const SHADER_DIR := "res://shaders"

func _ready() -> void:
	var failed: Array[String] = []
	var checked := 0
	for path in _shader_paths():
		checked += 1
		var sh: Shader = load(path)
		if sh == null:
			failed.append("%s: failed to load" % path)
			continue
		# Assigning the code to a material is what pushes it through the GPU
		# backend's compiler. Reading it back off the resource would not.
		var mat := ShaderMaterial.new()
		mat.shader = sh
		# Touch every uniform the shader declares, so a wrong type or a name the
		# scripts use but the shader dropped shows up here rather than as a
		# silently ignored set_shader_parameter at runtime.
		var names: Array[String] = []
		for u in sh.get_shader_uniform_list():
			names.append(str(u.get("name", "?")))
		names.sort()
		print("[shader] %s\n           uniforms: %s" % [path, ", ".join(names)])
		if names.is_empty():
			failed.append("%s: no uniforms parsed (source is probably malformed)" % path)

	# The compiler reports errors asynchronously through the engine's error
	# stream, so give it frames to run before deciding it was quiet.
	await get_tree().process_frame
	await get_tree().process_frame

	if failed.is_empty():
		print("[shader] %d shader(s) compiled" % checked)
		get_tree().quit(0)
	else:
		for f in failed:
			push_error("[shader] " + f)
			print("[shader] FAIL " + f)
		get_tree().quit(1)

func _shader_paths() -> Array[String]:
	var out: Array[String] = []
	var dir := DirAccess.open(SHADER_DIR)
	if dir == null:
		push_error("[shader] cannot open " + SHADER_DIR)
		return out
	for name in dir.get_files():
		# An exported project serves these as .remap; the source name is what
		# load() wants either way.
		var clean := name.trim_suffix(".remap")
		if clean.ends_with(".gdshader"):
			out.append("%s/%s" % [SHADER_DIR, clean])
	out.sort()
	return out
