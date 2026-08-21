extends Node
## The scenario probe: dress the world, look at it, and say so with an exit code.
##
##   godot --headless --path godot res://scenes/scenario_probe.tscn
##   xvfb-run -a -s "-screen 0 1600x900x24" \
##     env VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
##     godot --rendering-driver vulkan --path godot \
##           res://scenes/scenario_probe.tscn -- --screenshot /tmp/scn.png
##
## This is BACKLOG.md VER-2 item 1 made runnable. `headless_probe.tscn` verifies
## the UI and the render *plumbing*, but every one of its checks happens at a
## fresh evac-shelter spawn at 8am in clear weather -- so night, a lantern's
## gradient, fire, map memory, a hole to look down, headlights and rain had never
## been produced by any run. This probe produces them, in one deterministic
## sequence, using the scenario commands the GDExtension exposes (API 23), and
## screenshots the 3D world after each dressing step.
##
## Deliberately NOT a copy of the headless probe: it opens no menus and presses
## almost no keys, because a C++ screen owning the input loop is what starves the
## command queue -- the exact failure mode MENU-12's post-mortem documents. The
## only key it ever sends is Escape, to clear a popup.
##
## Checks are two-tier, the same policy as VER-0: REQUIRED failures set the exit
## code, WARN lines report what a varying world cannot promise (a basement within
## range, a tree standing south of the avatar).

const MV := preload("res://scripts/map_view.gd")

## Give a teleport that has to generate overmaps time to do it.
const COMMAND_TIMEOUT_S := 90.0

var host: Node
var _phase := "boot"
var _frames := 0
var _sub: SubViewport
var _view: Node3D
var _scn_generation := 0
var _failures: Array[String] = []
var _warnings: Array[String] = []
var _popup_sig := ""

func _ready() -> void:
	print("[scn] scenario probe starting")
	# The same on-demand asset build the host does at boot, so a fresh checkout's
	# probe run exercises the same meshes a player would see rather than none.
	var conv := Node.new()
	conv.set_script(load("res://scripts/convert_creature_meshes.gd"))
	add_child(conv)
	print("[scn] creature assets: %s" % str(conv.convert_missing()))
	conv.queue_free()
	host = ClassDB.instantiate("CDDAHost")
	host.name = "CDDAHost"
	add_child(host)
	if host.has_method("set_window_size"):
		host.set_window_size(1280, 720)
	print("[scn] api_version=%s (this probe needs >= 24 for animation, >= 23 to run)" % [
		str(host.api_version()) if host.has_method("api_version") else "missing"])
	if not host.has_method("scenario_teleport_omt"):
		push_error("the library has no scenario commands; rebuild the GDExtension")
		get_tree().quit(1)
		return
	host.bootstrap_async()

func _process(_delta: float) -> void:
	_frames += 1
	if host == null:
		return
	if host.has_method("bootstrap_failed") and host.bootstrap_failed():
		push_error("bootstrap failed: %s" % str(host.get_error_message()))
		get_tree().quit(1)
		return
	_answer_popups()
	_cancel_uilists()
	_heal_legacy_ui(_delta)
	match _phase:
		"boot":
			if host.is_ready():
				_phase = "await_session"
				print("[scn] ready after %d frames, requesting Play Now" % _frames)
				host.request_new_game("now")
		"await_session":
			if host.is_session_active():
				_phase = "run"
				print("[scn] session active after %d frames" % _frames)
				# Sized, or the minimap never publishes at all ("nobody is
				# showing the panel; do not pay for a render") and its
				# generation cannot be asserted on.
				if host.has_method("set_minimap_size"):
					host.set_minimap_size(200, 200)
				_run_scenarios()
			elif _frames > 20000:
				push_error("no session after %d frames" % _frames)
				get_tree().quit(1)
		"run":
			# Keep the world fresh: the light texture, particles and lights all
			# ride on refresh, exactly as host.gd drives them.
			if _view != null and is_instance_valid(_view) and _view.visible:
				_view.refresh()

## Answer or dismiss whatever query_popup raises, so the game thread can never
## park on a prompt nobody attends. Same policy as the headless probe: answer
## the last option, which is "No".
func _answer_popups() -> void:
	if not (host.has_method("popup_active") and host.popup_active()):
		return
	var pd: Dictionary = host.get_popup_state()
	var sig := "%s|%s" % [str(pd.get("notice", "")), str(pd.get("text", ""))]
	if sig == _popup_sig:
		return
	_popup_sig = sig
	if bool(pd.get("prompt_active", false)):
		var pop: Array = pd.get("options", [])
		if not pop.is_empty():
			print("[scn] popup '%s' -> answering '%s'" % [str(pd.get("text", "")),
				str(pop[pop.size() - 1])])
			host.popup_answer(pop.size() - 1)

## A movement key that wanders into the wrong tile opens a uilist (a vehicle's
## examine menu, a pile of items), and a uilist nobody attends is a parked game
## thread and a starved command queue -- the headless probe learned this and
## cancels them; this probe walks during combat and needs the same reflex.
func _cancel_uilists() -> void:
	if host.has_method("uilist_active") and host.uilist_active():
		print("[scn] cancelling an unattended uilist")
		host.uilist_cancel()

var _legacy_ui_since := 0.0
var _legacy_ui_dumped := false

## The watchdog for the C++ screen nobody opened. Some legacy ImGui window
## appears mid-session (it took the baseline headless run's stages 7..12 with it
## too), and while any_window_shown() is true the command queue starves -- which
## reads as unrelated timeouts two checks later. Name it once, from the cell
## overlay it draws into, then keep pressing Escape until it goes.
func _heal_legacy_ui(delta: float) -> void:
	if not (host.has_method("legacy_ui_active") and host.legacy_ui_active()):
		_legacy_ui_since = 0.0
		return
	_legacy_ui_since += delta
	if _legacy_ui_since < 1.5:
		return
	if not _legacy_ui_dumped:
		_legacy_ui_dumped = true
		_dump_overlay_rows()
	print("[scn] legacy C++ screen is up; pressing Escape")
	_press(KEY_ESCAPE)
	_legacy_ui_since = 0.0

## What the mystery window says, decoded from the curses cell overlay -- the id
## of the screen is not published anywhere, but its own text names it.
func _dump_overlay_rows() -> void:
	var cols: int = host.get_view_cols()
	var rows: int = host.get_view_rows()
	var cells: PackedInt32Array = host.get_view_cells()
	const STRIDE := 4
	if cols <= 0 or rows <= 0 or cells.size() < cols * rows * STRIDE:
		print("[scn] legacy screen dump: no cell data")
		return
	var printed := 0
	for r in rows:
		var line := ""
		var claimed := 0
		for c in cols:
			var at := (r * cols + c) * STRIDE
			if cells[at + 3] != 0:
				claimed += 1
				var ch := cells[at]
				line += char(ch) if ch >= 32 and ch < 0x2FFFF else " "
			else:
				line += " "
		if claimed > 4 and printed < 10:
			printed += 1
			print("[scn] overlay row %2d | %s" % [r, line.substr(0, 100).strip_edges(false, true)])

func _build_world_view() -> void:
	_sub = SubViewport.new()
	_sub.name = "ScenarioWorld"
	_sub.size = Vector2i(1280, 720)
	_sub.own_world_3d = true
	# Nothing composites this viewport, so it must render on its own clock or the
	# screenshots capture the clear colour.
	_sub.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(_sub)
	_view = Node3D.new()
	_view.name = "MapView3D"
	_view.set_script(load("res://scripts/map_view_3d.gd"))
	# Hidden first, shown after setup: host.gd's order, and the one that works --
	# a camera outside a World3D cannot be made current.
	_view.visible = false
	_sub.add_child(_view)
	_view.setup(host)
	_view.refresh()
	_view.visible = true
	_view.refresh()

## One scenario command: post it, then poll the status generation until the game
## thread reports it done. "Accepted" is not "done" -- the command runs at the
## input wait, and a teleport that searches the overmap can take real seconds.
func _cmd(method: String, args: Array) -> Dictionary:
	var err: String = host.callv(method, args)
	if err != "":
		return { "ok": false, "detail": "refused: " + err }
	var want := _scn_generation + 1
	var waited := 0.0
	while waited < COMMAND_TIMEOUT_S:
		await get_tree().create_timer(0.25).timeout
		waited += 0.25
		var st: Dictionary = host.get_scenario_status()
		if int(st.get("generation", 0)) >= want:
			_scn_generation = int(st.get("generation", 0))
			return st
	return { "ok": false, "detail": "timed out after %.0fs" % waited }

func _check(name: String, ok: bool, required: bool, detail: String) -> void:
	var tier := "REQUIRED" if required else "WARN"
	print("[scn] %-28s %s%s" % [name, "ok" if ok else "FAILED (%s)" % tier,
		"" if detail.is_empty() else "  -- " + detail])
	if ok:
		return
	if required:
		_failures.append("%s: %s" % [name, detail])
	else:
		_warnings.append("%s: %s" % [name, detail])

## Screenshot the world viewport, when --screenshot is on and something rasterises.
func _shot(tag: String) -> void:
	var args := OS.get_cmdline_user_args()
	var at := args.find("--screenshot")
	if at < 0 or at + 1 >= args.size() or DisplayServer.get_name() == "headless":
		return
	_view.refresh()
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	var img: Image = _sub.get_texture().get_image()
	if img == null:
		print("[scn] shot %s: no image" % tag)
		return
	var path: String = args[at + 1]
	path = path.get_basename() + "." + tag + "." + path.get_extension()
	var err := img.save_png(path)
	print("[scn] shot %s -> %s (err=%d)" % [tag, path, err])

## Key injection, the headless probe's shapes exactly: unicode must be set or a
## letter key matches the wrong binding (its comment tells the story).
func _press(keycode: int) -> void:
	var down := InputEventKey.new()
	down.keycode = keycode
	down.physical_keycode = keycode
	down.pressed = true
	if keycode >= KEY_SPACE and keycode <= KEY_ASCIITILDE:
		down.unicode = char(keycode).to_lower().unicode_at(0)
	host.push_input_event(down)

func _press_shifted(keycode: int, produced: String) -> void:
	var down := InputEventKey.new()
	down.keycode = keycode
	down.physical_keycode = keycode
	down.pressed = true
	down.shift_pressed = true
	down.unicode = produced.unicode_at(0)
	host.push_input_event(down)

## Whether any published hit event names @p uid as the one struck (or killed).
func _uid_was_struck(uid: int) -> bool:
	if uid == 0:
		return false
	var events: PackedInt32Array = host.get_hit_events()
	var i := 0
	while i + 9 < events.size():
		if events[i + 8] == uid:
			return true
		i += 10
	return false

func _sway_commands() -> int:
	var cmds: PackedInt32Array = host.get_map_draw_list()
	var n := 0
	var i := 0
	while i + MV.CMD_STRIDE - 1 < cmds.size():
		if (cmds[i + 9] & MV.FLAG_SWAY) != 0:
			n += 1
		i += MV.CMD_STRIDE
	return n

## Texels on the avatar's level whose G (light amount) exceeds @p floor -- "how
## many tiles does the game say are lit". At night this isolates a light's pool
## from the ambient: a fire is a handful of texels, a headlight cone tens.
func _lit_texels(floor_g: float) -> int:
	var img: Image = host.get_light_image()
	if img == null:
		return -1
	var levels := 1
	if host.has_method("get_light_levels"):
		levels = maxi(1, host.get_light_levels())
	var block_h := int(img.get_height() / levels)
	var n := 0
	for y in block_h:
		for x in img.get_width():
			if img.get_pixel(x, y).g > floor_g:
				n += 1
	return n

## The R channel of the avatar-level block of the light image, bucketed into
## seen / remembered / unknown -- the memory question is "does remembered ground
## exist in view at all", which no run before this one could answer.
func _visibility_histogram() -> Dictionary:
	var img: Image = host.get_light_image()
	if img == null:
		return {}
	var levels := 1
	if host.has_method("get_light_levels"):
		levels = maxi(1, host.get_light_levels())
	var block_h := int(img.get_height() / levels)
	var seen := 0
	var remembered := 0
	var unknown := 0
	for y in block_h:
		for x in img.get_width():
			var r := img.get_pixel(x, y).r
			if r > 0.75:
				seen += 1
			elif r > 0.1:
				remembered += 1
			else:
				unknown += 1
	return { "seen": seen, "remembered": remembered, "unknown": unknown }

func _run_scenarios() -> void:
	# The view's one-shot first-frame report latches on its first refresh, and a
	# refresh before the tileset is up latches it on "nothing drawn". Wait, as
	# host.gd's session-present gate does.
	var waited := 0.0
	while not host.tileset_ready() and waited < 60.0:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	_build_world_view()
	await get_tree().create_timer(1.0).timeout
	print("[scn] commands_ready=%s" % str(host.commands_ready()))

	# The avatar's sex is a coin flip under Play Now, and the creature-mesh check
	# below wants the one id a converted mesh exists for. A choice, not a roll.
	var st: Dictionary = await _cmd("scenario_set_avatar_sex", [false])
	_check("set_avatar_sex", bool(st.get("ok", false)), true, str(st.get("detail", "")))

	var cmds0: PackedInt32Array = host.get_map_draw_list()
	_check("draw list non-empty", cmds0.size() > 0, true, "%d ints" % cmds0.size())
	await _shot("00_shelter_day")

	# --- The furniture mesh library (3D-8d), deterministically: worldgen owes
	# no scene a bed, so place one. Two pieces with different silhouettes, both
	# in the committed library, spawned beside the avatar where the shelter is
	# guaranteed floor. The count is read from the same debug channel the
	# avatar-mesh check uses, after a refresh has rebuilt the batches.
	if host.has_method("scenario_spawn_furniture"):
		st = await _cmd("scenario_spawn_furniture", ["f_bed", 2, 0])
		_check("spawn f_bed", bool(st.get("ok", false)), true, str(st.get("detail", "")))
		st = await _cmd("scenario_spawn_furniture", ["f_armchair", 2, 1])
		_check("spawn f_armchair", bool(st.get("ok", false)), true,
			str(st.get("detail", "")))
		await _shot("00b_furniture_meshes")
		var furn_routed := int(_view.debug_stats().get("furniture_meshes", 0))
		_check("furniture meshes routed", furn_routed >= 2, true,
			"%d furniture commands routed to the mesh library" % furn_routed)

	# --- Open ground. "field" is the commonest OMT there is; requiring it is safe.
	st = await _cmd("scenario_teleport_omt", ["field", 100])
	_check("teleport field", bool(st.get("ok", false)), true, str(st.get("detail", "")))
	await get_tree().create_timer(0.5).timeout
	_view.refresh()
	print("[scn] sway commands in view = %d (grass only sways when it overhangs)"
		% _sway_commands())
	await _shot("01_field_day")

	# --- Look mode. Pressing 'x' used to read as the game freezing: the snapshot
	# centred on the avatar alone (ignoring view_offset, which is all the look
	# cursor moves), and nothing republished from inside look_around's own input
	# loop. The published origin moving is the whole fix, observed end to end.
	var origin_before: Vector2i = host.get_map_view_origin()
	_press(KEY_X)
	await get_tree().create_timer(1.0).timeout
	for i in 6:
		_press(KEY_RIGHT)
		await get_tree().create_timer(0.35).timeout
	var origin_panned: Vector2i = host.get_map_view_origin()
	_check("look mode pans the view", origin_panned.x > origin_before.x, true,
		"origin %s -> %s after six steps east" % [str(origin_before), str(origin_panned)])
	_press(KEY_ESCAPE)
	await get_tree().create_timer(1.0).timeout
	var origin_after: Vector2i = host.get_map_view_origin()
	_check("look mode hands the view back", origin_after == origin_before, false,
		"origin %s (was %s)" % [str(origin_after), str(origin_before)])

	# --- And WALKING after look must still update everything. The field report
	# that matters: after using look, "the minimap gets stuck and vision no
	# longer updates so you can walk into black areas" -- i.e. some generation
	# stops advancing once look has been open. Take a few real steps and demand
	# the map, light and minimap counters all move past their post-look values.
	var gens_before := Vector3i(int(host.get_map_generation()),
		int(host.get_light_generation()),
		int(host.get_minimap_generation()) if host.has_method("get_minimap_generation") else 0)
	# Arrow steps, no safe-mode toggle. Two fixture lessons in one place: a '!'
	# here flipped safe mode OFF so the combat block's own '!' flipped it back ON
	# and thirty attack keys were eaten (keys toggle -- the probe's oldest
	# lesson); and '.'-pauses right after look advance nothing (something in the
	# look exit path swallows unbound keys for a beat), while arrows are proven.
	# Safe mode only stops on a visible hostile and none exists yet, so plain
	# steps pass turns; loop until the generations actually move. The directions
	# cycle because a bump into something impassable consumes no turn at all:
	# ten steps east against a locker froze all three counters and read as the
	# look regression this check hunts, when it was the furniture's doing.
	var gens_after := gens_before
	var walk_keys := [KEY_RIGHT, KEY_DOWN, KEY_LEFT, KEY_UP]
	for i in 12:
		if i > 0:
			# A wandering hostile trips safe mode, and a stopped avatar consumes
			# arrows in every direction without passing a turn -- the third way
			# this check froze with the pipeline healthy. Ignore-enemy (')
			# resumes movement without touching the safe-mode toggle itself,
			# which the combat stage's own '!' depends on; with nothing spotted
			# it matches no binding and falls through.
			_press(KEY_APOSTROPHE)
			await get_tree().create_timer(0.2).timeout
		_press(walk_keys[i % walk_keys.size()])
		await get_tree().create_timer(0.5).timeout
		gens_after = Vector3i(int(host.get_map_generation()),
			int(host.get_light_generation()),
			int(host.get_minimap_generation()) if host.has_method("get_minimap_generation") else 0)
		if gens_after.x > gens_before.x and gens_after.y > gens_before.y \
				and gens_after.z > gens_before.z:
			break
	_check("turns after look update vision", gens_after.x > gens_before.x
		and gens_after.y > gens_before.y and gens_after.z > gens_before.z, true,
		"map/light/minimap generations %s -> %s" % [str(gens_before), str(gens_after)])

	# --- Night. The whole light pass has only ever been computed for 8am.
	st = await _cmd("scenario_set_time", [1, 0])
	_check("set_time 01:00", bool(st.get("ok", false)), true, str(st.get("detail", "")))
	var cond: Dictionary = host.get_conditions()
	_check("night daylight", float(cond.get("daylight", 1.0)) < 0.15, true,
		"daylight=%.3f" % float(cond.get("daylight", 1.0)))
	# The night baseline for the counts below: if this is already most of the
	# view, the per-tile light mask has no headroom at night and no lamp's pool
	# can ever brighten the ground -- night-ness would be living entirely in the
	# daylight grade, which dims lit and unlit tiles alike.
	print("[scn] lit texels (G>0.5) at night, before any light = %d" % _lit_texels(0.5))
	await _shot("02_field_night")

	# --- A lantern at night: the gradient BACKLOG.md says was never observed.
	st = await _cmd("scenario_spawn_item", ["electric_lantern_on", 2, 0])
	_check("spawn lit lantern", bool(st.get("ok", false)), true, str(st.get("detail", "")))
	var lights_lamp := int(host.get_light_sources().size() / 9)
	_check("lamp light published", lights_lamp >= 1, false,
		"%d sources (a ground item may not reach the source buffer; the texture gradient is the test)" % lights_lamp)
	await _shot("03_lamp_night")

	# --- Fire: the B channel, the flicker, the glow, the particles, a real light.
	st = await _cmd("scenario_spawn_field", ["fd_fire", 3, -3, 1])
	_check("spawn fd_fire", bool(st.get("ok", false)), true, str(st.get("detail", "")))
	var fields: PackedInt32Array = host.get_map_field_list()
	_check("fire in field list", fields.size() > 0, true, "%d ints" % fields.size())
	var lights_fire := int(host.get_light_sources().size() / 9)
	_check("fire light published", lights_fire >= 1, true, "%d sources" % lights_fire)
	print("[scn] lit texels (G>0.5) with fire = %d" % _lit_texels(0.5))
	_view.refresh()
	await _shot("04_fire_night")

	# --- Headlights. The SpotLight3D path has been implemented end to end since
	# API 21 and no run has ever had a vehicle to show it with.
	st = await _cmd("scenario_spawn_vehicle", ["car", 4, 3])
	_check("spawn car", bool(st.get("ok", false)), false, str(st.get("detail", "")))
	_view.refresh()
	await get_tree().create_timer(0.5).timeout
	var vstats: Dictionary = _view.debug_stats()
	_check("headlight beams built", int(vstats.get("beams", 0)) >= 1, false,
		"beams=%d of %d sources (u.sees and the 32 cap both gate this)" % [
		int(vstats.get("beams", 0)), int(vstats.get("lights_published", 0))])
	# Whether the game's own lightmap has the cone: if this number does not jump
	# well past the fire's, the beams are being published for lamps the lightmap
	# is not lighting -- the drift the mirrored vehicle loop is documented to risk.
	print("[scn] lit texels (G>0.5) with headlights = %d" % _lit_texels(0.5))
	await _shot("05_headlights_night")

	# --- A creature, adjacent: the mesh path (its capsule shadow), the hit
	# machinery's target, and something for the sun to light. Spawned HERE, in
	# the open field, because the spawn searches four tiles out and fails
	# quietly in a cramped stairwell -- which it did, and the coverage went red
	# on a check that was really about floor plan.
	var err: String = host.debug_spawn_monster("mon_zombie")
	_check("spawn mon_zombie", err == "", true, err)
	await get_tree().create_timer(1.0).timeout
	# debug_spawn_monster predates the scenario channel and does not republish
	# the snapshot -- without a player action the zombie exists but is never
	# published. A zero-length teleport is the cheapest republish there is.
	await _cmd("scenario_teleport_rel", [0, 0, 0])
	_view.refresh()
	var creatures: Array = host.get_creatures()
	_check("creatures published", creatures.size() >= 2, true,
		"%d creatures (avatar + adjacent zombie)" % creatures.size())
	await _shot("05b_zombie_night")

	# --- Combat, so the animation clips have a reason to play: swing at the
	# zombie. The avatar's swing lands as a hit event (attacker = avatar, target
	# = zombie, API 24), the zombie's rig plays `hit`, its own approach plays
	# `walk`, and its death -- if the jackhammer obliges -- plays `die`. Checks
	# are gated on an animated rig existing at all: mon_zombie.scn is a
	# generated artifact (make_example_animated_creature.tscn), and a check
	# that fails for a missing generated file is a check that cries wolf.
	var apos := Vector2.ZERO
	var zpos := Vector2.ZERO
	for c in creatures:
		if int((c as Dictionary).get("kind", -1)) == 2:
			apos = Vector2(float(c.get("x", 0)), float(c.get("y", 0)))
		elif str((c as Dictionary).get("id", "")) == "mon_zombie":
			zpos = Vector2(float(c.get("x", 0)), float(c.get("y", 0)))
	var meshes_node := _view.get_node_or_null("CreatureMeshes")
	var animated := meshes_node != null and int(meshes_node.debug_stats().get(
		"animated_ids", 0)) >= 1
	print("[scn] animated rigs loaded = %s" % str(animated))
	var zuid := 0
	for c in creatures:
		if str((c as Dictionary).get("id", "")) == "mon_zombie":
			zuid = int((c as Dictionary).get("uid", 0))
	if zpos != Vector2.ZERO:
		# Safe mode eats movement keys the moment a hostile is visible.
		_press_shifted(KEY_1, "!")
		await get_tree().create_timer(0.6).timeout
		# Arrow keys, not numpad: the bridge translates numpad into kp_*
		# keycodes the default bindings never match -- the headless probe's
		# numpad NPC walk reports "did not reach them" in every logged run,
		# while its combat fixture fights fine with arrows. Zombie's side
		# first, then cycle: where it stands and whether it closes are the
		# game's business, and a fixed sequence produces a fight only
		# sometimes. Loop until a blow actually lands, like that fixture does.
		var dir := zpos - apos
		var ordered: Array = [
			(KEY_RIGHT if dir.x > 0.0 else KEY_LEFT) if absf(dir.x) >= absf(dir.y)
			else (KEY_DOWN if dir.y > 0.0 else KEY_UP),
			KEY_LEFT, KEY_UP, KEY_RIGHT, KEY_DOWN,
		]
		# Loop until a blow lands ON THE ZOMBIE, not on anyone: the first run of
		# this exited on hit_generation alone, which the zombie's own attack on
		# the avatar satisfied -- one hit event, target uid 1, and the animated
		# rig had been struck zero times while the check believed combat was
		# proven.
		var attempts := 0
		while attempts < 30 and not _uid_was_struck(zuid):
			_press(ordered[attempts % ordered.size()])
			await get_tree().create_timer(0.35).timeout
			attempts += 1
		# What state the melee left the game thread in, before anything waits on
		# it: a C++ screen opened by a stray movement key starves the command
		# queue, and the starvation reads as unrelated timeouts two checks later.
		print("[scn] after melee: commands_ready=%s legacy_ui=%s uilist=%s popup=%s textwin=%s" % [
			str(host.commands_ready()),
			str(host.legacy_ui_active()) if host.has_method("legacy_ui_active") else "?",
			str(host.uilist_active()) if host.has_method("uilist_active") else "?",
			str(host.popup_active()) if host.has_method("popup_active") else "?",
			str(host.textwin_active()) if host.has_method("textwin_active") else "?"])
		print("[scn] raw hit events = %s" % str(host.get_hit_events()))
		for c2 in host.get_creatures():
			print("[scn] creature %s uid=%s" % [str((c2 as Dictionary).get("id", "?")),
				str((c2 as Dictionary).get("uid", "?"))])
		# The zombie's own swing is the attack-clip fixture, and it only swings
		# on its turns -- pass a few by pausing, unless it is already dead.
		var waits := 0
		while waits < 6 and meshes_node != null:
			var played_now: Dictionary = meshes_node.debug_stats().get("clips_played", {})
			if int(played_now.get("attack", 0)) >= 1 or int(played_now.get("die", 0)) >= 1:
				break
			_press(KEY_PERIOD)
			await get_tree().create_timer(0.5).timeout
			waits += 1
		# Publish whatever the fight changed, then let presentation time run a
		# beat so one-shot clocks and tweens are observed, not raced.
		await _cmd("scenario_teleport_rel", [0, 0, 0])
		_view.refresh()
		await get_tree().create_timer(1.0).timeout
		var hits_gen := int(host.get_hit_generation()) if host.has_method(
			"get_hit_generation") else 0
		_check("zombie was struck", _uid_was_struck(zuid), true,
			"hit_generation=%d after %d key(s)" % [hits_gen, attempts])
		if meshes_node != null:
			var played: Dictionary = meshes_node.debug_stats().get("clips_played", {})
			print("[scn] clips played = %s" % str(played))
			if animated:
				_check("hit clip played", int(played.get("hit", 0)) >= 1
					or int(played.get("die", 0)) >= 1, true, str(played))
				# Required since swings (kind 2) exist: the zombie swings back
				# during any melee exchange, whether or not it connects, and its
				# rig is the animated one -- an attack that never plays is a
				# broken channel, not an unlucky world. The one honest out is a
				# zombie killed before its first turn, whose die clip stands in.
				_check("attack clip played", int(played.get("attack", 0)) >= 1
					or int(played.get("die", 0)) >= 1, true, str(played))
				_check("walk clip played", int(played.get("walk", 0)) >= 1
					or int(played.get("run", 0)) >= 1, false,
					"a zombie that died adjacent never had to walk: %s" % str(played))
				_check("die clip played", int(played.get("die", 0)) >= 1, false,
					"the zombie may simply have survived: %s" % str(played))
		await _shot("05c_combat_night")

	# --- The surroundings list (MENU-12), observed at last. The screen was
	# committed and building for days without one run ever reaching it: the
	# headless probe's attempt shares an avatar with nine other stages and was
	# starved by the soliloquy window. Here the queue is healthy, the watchdogs
	# run, and the ground is rich -- a zombie (or its corpse), a car, a fire and
	# whatever the fight dropped. The attend contract matters: reading the state
	# is what attends, and an unattended takeover falls back to legacy ImGui
	# after 1.5s -- so the state is read the moment active flips, not politely
	# later.
	if host.has_method("get_surroundings_state"):
		# Capital V -- the binding is keyboard_char "V", and a lowercased press
		# is the single letter that kept this screen unobserved through eight
		# fixture runs across two probes.
		_press_shifted(KEY_V, "V")
		var opened := false
		var waited_v := 0.0
		while waited_v < 8.0:
			await get_tree().create_timer(0.2).timeout
			waited_v += 0.2
			if host.surroundings_active():
				opened = true
				break
		if not opened:
			# One retry: the first press can be eaten by a soliloquy window the
			# watchdog is still escaping.
			_press_shifted(KEY_V, "V")
			while waited_v < 16.0:
				await get_tree().create_timer(0.2).timeout
				waited_v += 0.2
				if host.surroundings_active():
					opened = true
					break
		_check("surroundings opened", opened, true, "after %.1fs" % waited_v)
		if opened:
			var sd: Dictionary = host.get_surroundings_state()
			var s_tabs: Array = sd.get("tabs", [])
			var s_rows: Array = sd.get("rows", [])
			print("[scn] [surr] tabs=%d rows=%d selected=%d" % [s_tabs.size(),
				s_rows.size(), int(sd.get("selected", -1))])
			for i in mini(4, s_rows.size()):
				print("[scn] [surr]   %s" % str(s_rows[i].get("text", "")).substr(0, 50))
			_check("surroundings has tabs", s_tabs.size() >= 3, true,
				"%d tabs" % s_tabs.size())
			_check("surroundings lists something", s_rows.size() >= 1, true,
				"%d rows (a zombie and a car stand right there)" % s_rows.size())
			var tab_before := int(sd.get("tab", -1))
			host.surroundings_action("NEXT_TAB")
			await get_tree().create_timer(1.0).timeout
			var tab_after := int((host.get_surroundings_state() as Dictionary).get("tab", -2))
			_check("surroundings NEXT_TAB round trip", tab_after != tab_before, true,
				"tab %d -> %d" % [tab_before, tab_after])
			host.surroundings_action("QUIT")
			await get_tree().create_timer(1.0).timeout
			_check("surroundings closed", not host.surroundings_active(), true, "after QUIT")

	# --- Volumetric fog, the default since 3D-8/3D-9 made it honest and affordable:
	# 06 is the night as shipped, 06b the same frame with the fog drained, so every
	# run carries its own A/B for the density number. The default comes back on
	# after, because the later stages should look like the game does.
	if _view.has_method("set_volumetric_fog"):
		_view.set_volumetric_fog(true)
		_view.refresh()
		await _shot("06_fog_night")
		_view.set_volumetric_fog(false)
		_view.refresh()
		await _shot("06b_fog_off_night")
		_view.set_volumetric_fog(true)

	# --- Rain: the wet grade has never been seen next to the night grade, and the
	# weather particles are new outright.
	st = await _cmd("scenario_set_weather", ["rain"])
	_check("set_weather rain", bool(st.get("ok", false)), true, str(st.get("detail", "")))
	cond = host.get_conditions()
	_check("rain published", float(cond.get("precipitation", 0.0)) > 0.0
		and int(cond.get("weather_kind", 0)) == 1, true,
		"precipitation=%.2f kind=%d" % [float(cond.get("precipitation", 0.0)),
		int(cond.get("weather_kind", 0))])
	_view.refresh()
	var wp := _view.get_node_or_null("WeatherParticles")
	if wp != null:
		var ws: Dictionary = wp.debug_stats()
		_check("weather particles", bool(ws.get("active", false)), true,
			"kind=%d amount=%d" % [int(ws.get("kind", 0)), int(ws.get("amount", 0))])
	await _shot("07_rain_night")
	await _cmd("scenario_set_weather", [""])

	# --- Daylight again for the interior scenes.
	await _cmd("scenario_set_time", [12, 0])

	# --- Memory: walk into a house, then step well away from it. The rooms stay
	# in the view extent, out of sight, remembered -- the other thing BACKLOG.md
	# says no run has ever produced.
	st = await _cmd("scenario_teleport_omt", ["house", 100])
	_check("teleport house", bool(st.get("ok", false)), false, str(st.get("detail", "")))
	if bool(st.get("ok", false)):
		_view.refresh()
		await _shot("08_house_inside")
		# A short walk inside, so more of the interior is seen from more angles
		# before it goes out of sight -- what is never seen cannot be remembered.
		await _cmd("scenario_teleport_rel", [2, 0, 0])
		await _cmd("scenario_teleport_rel", [-2, 2, 0])
		# Then step out, but not too far: the view is only ~25 tiles wide, so a
		# 14-tile step put the whole remembered interior outside the histogram's
		# window and 0 remembered tiles said nothing about memory at all.
		await _cmd("scenario_teleport_rel", [9, -2, 0])
		_view.refresh()
		var hist := _visibility_histogram()
		_check("remembered tiles in view", int(hist.get("remembered", 0)) > 0, false,
			str(hist))
		await _shot("09_house_memory")

	# --- A hole. Basement first, up one, then stand on the staircase itself:
	# open_columns has been zero in every run since ADR-005 item 1 landed.
	st = await _cmd("scenario_teleport_omt", ["basement", 100])
	_check("teleport basement", bool(st.get("ok", false)), false, str(st.get("detail", "")))
	if bool(st.get("ok", false)):
		# The light-gradient question has to be asked here, not outdoors: a
		# moonlit CDDA night says LIT for nearly every outdoor tile (1072 of
		# 1075 texels above G 0.5 at 1am, measured), so the mask has no
		# headroom and no lamp's pool can show against it. A basement is the
		# genuinely dark place the shelter spawn never offered.
		var dark_before := _lit_texels(0.5)
		st = await _cmd("scenario_spawn_field", ["fd_fire", 2, 2, 0])
		_check("fire in basement", bool(st.get("ok", false)), false, str(st.get("detail", "")))
		_view.refresh()
		var dark_after := _lit_texels(0.5)
		_check("light pool in the dark", dark_after > dark_before, false,
			"lit texels %d -> %d" % [dark_before, dark_after])
		await _shot("10a_basement_fire")
		await _cmd("scenario_teleport_rel", [0, 0, 1])
		st = await _cmd("scenario_stand_on", ["GOES_DOWN", 30])
		_check("stand on stairs", bool(st.get("ok", false)), false, str(st.get("detail", "")))
		_view.refresh()
		var rs: Dictionary = host.get_render_stats()
		_check("open columns below", int(rs.get("open_columns", 0)) > 0, false,
			"open_columns=%d" % int(rs.get("open_columns", 0)))
		await _shot("10_hole")

	# --- The mesh: player_female.res is converted and the sex was set above, so
	# this is deterministic -- the one headless check 3D-7c never had. The
	# suppressed count is the load-bearing signal: a mesh that draws takes the
	# avatar's sprite commands (body and overlays) out of the batches.
	var meshes_drawn := int(_view.debug_stats().get("suppressed", 0))
	_check("avatar drawn as mesh", meshes_drawn >= 1, true,
		"%d creature sprite commands left to meshes" % meshes_drawn)

	_report()

func _report() -> void:
	print("")
	print("[scn] ================ scenario coverage ================")
	for w in _warnings:
		print("[scn] WARN     %s" % w)
	for f in _failures:
		print("[scn] REQUIRED %s" % f)
	if _failures.is_empty():
		print("[scn] all required checks passed (%d warnings)" % _warnings.size())
	else:
		print("[scn] FAILED: %d required check(s)" % _failures.size())
	var code := 0 if _failures.is_empty() else 1
	# The backend must know the code: session teardown ends in std::_Exit from
	# the game thread, which cannot read what SceneTree.quit was given and used
	# to hard-code 0 -- a failing run that reports success.
	if host.has_method("note_exit_code"):
		host.note_exit_code(code)
	get_tree().quit(code)
