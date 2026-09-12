extends PanelContainer
## Render debug overlay (SP-9). Toggle with F3.
##
## Answers one question: why did this tile draw what it drew? The renderer has
## several places a sprite can come from -- the id itself, a `looks_like`, the
## tileset's ASCII set, an "unknown_<category>" placeholder, a font glyph -- and
## from the outside they are indistinguishable. Four of the five look like art
## someone chose.
##
## So the counts here are per fallback level rather than per frame cost, and the
## list at the bottom is the sprite coverage report (SP-2): the ids this session
## has actually had to fall back for, most-drawn first. That list is the work
## queue for anyone filling gaps in a tileset.

const Nocturne := preload("res://scripts/nocturne.gd")

## Sprite misses to list. Long enough to be a work queue, short enough to read.
const MISS_ROWS := 12
## The overlay reads a mutex-guarded snapshot; refreshing it every frame would
## be pointless as well as rude, since the map only changes per turn.
const REFRESH_INTERVAL := 0.5

## map_layer, in the order src/godot_map_snapshot.h declares it.
const LAYER_NAMES := ["terrain_bg", "terrain_fg", "furniture", "trap", "field",
	"vehicle", "item", "monster", "monster_overlay", "player", "player_overlay"]

var _host: Node
var _body: VBoxContainer
var _since_refresh: float = 0.0

func setup(host: Node) -> void:
	_host = host
	set_anchors_preset(Control.PRESET_TOP_LEFT)
	position = Vector2(Nocturne.SPACE_L, Nocturne.SPACE_L)
	custom_minimum_size = Vector2(360, 0)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	# Above MapView and its layers, below anything modal.
	z_index = 120
	add_theme_stylebox_override("panel", Nocturne.panel_style(true))
	if _body == null:
		_body = VBoxContainer.new()
		_body.add_theme_constant_override("separation", 2)
		_body.mouse_filter = Control.MOUSE_FILTER_IGNORE
		add_child(_body)
	refresh()

func _process(delta: float) -> void:
	if not visible:
		return
	_since_refresh += delta
	if _since_refresh >= REFRESH_INTERVAL:
		_since_refresh = 0.0
		refresh()

func refresh() -> void:
	if _host == null or _body == null:
		return
	for child in _body.get_children():
		child.queue_free()

	if not _host.has_method("get_render_stats"):
		_body.add_child(Nocturne.micro_label("host has no render stats", Nocturne.BAD))
		return

	var st: Dictionary = _host.get_render_stats()
	_body.add_child(Nocturne.section_header(1, "RENDER"))
	_body.add_child(Nocturne.kv_row("tileset", "%s  (%d atlas)" % [
		str(st.get("tileset", "?")), int(st.get("atlases", 0))]))
	_body.add_child(Nocturne.kv_row("tile", str(st.get("tile_size", Vector2i.ZERO))))
	_body.add_child(Nocturne.kv_row("view", "%s at %s" % [
		str(st.get("view_size", Vector2i.ZERO)), str(st.get("view_origin", Vector2i.ZERO))]))
	_body.add_child(Nocturne.kv_row("draw commands", str(st.get("commands", 0))))
	_body.add_child(Nocturne.kv_row("fallback glyphs", str(st.get("glyphs", 0)),
		Nocturne.WARN if int(st.get("glyphs", 0)) > 0 else Nocturne.TEXT))
	_body.add_child(Nocturne.kv_row("fps", str(Engine.get_frames_per_second())))
	# What the z-level walk cost this frame (ADR-005 item 1). "open columns"
	# is the number that matters: it is how many of the view's columns had no
	# floor and made the walk descend. Zero of them outdoors is the whole
	# answer to what publishing more than one level costs there.
	var open_cols := int(st.get("open_columns", 0))
	_body.add_child(Nocturne.kv_row("open columns", str(open_cols),
		Nocturne.TEXT if open_cols > 0 else Nocturne.NEUTRAL_600))
	if open_cols > 0:
		_body.add_child(Nocturne.kv_row("levels below",
			"%d deep, %d commands" % [int(st.get("deepest_z_below", 0)),
				int(st.get("below_commands", 0))]))
		# What the 3D backend does with them (3D-4). Zero means coplanar, which is the
		# 2D backend's behaviour and what a flat 3D world keeps.
		var drop := float(st.get("level_drop", 0.0))
		if drop > 0.0:
			_body.add_child(Nocturne.kv_row("level drop",
				"%.1f tiles of height each" % drop))

	if _host.has_method("get_light_size"):
		var lsize: Vector2i = _host.get_light_size()
		_body.add_child(Nocturne.kv_row("light texture",
			"%dx%d" % [lsize.x, lsize.y] if lsize.x > 0 else "off",
			Nocturne.TEXT if lsize.x > 0 else Nocturne.NEUTRAL_600))

	# Light sources (ADR-006 item 3D-2). Worth a row of its own rather than a number
	# in a log: these are read out of state `level_cache.h` describes as valid only
	# inside generate_lightmap, so this is the counter that would notice that
	# contract breaking. Zero in daylight is ordinary; zero at night beside a
	# campfire is the bug.
	var sources := int(st.get("lights", 0))
	_body.add_child(Nocturne.kv_row("light sources", str(sources),
		Nocturne.TEXT if sources > 0 else Nocturne.NEUTRAL_600))

	_body.add_child(Nocturne.divider())
	_body.add_child(Nocturne.section_header(2, "BY LAYER"))
	var by_layer = st.get("by_layer", PackedInt32Array())
	for i in mini(LAYER_NAMES.size(), by_layer.size()):
		if by_layer[i] > 0:
			_body.add_child(Nocturne.kv_row(LAYER_NAMES[i], str(by_layer[i])))

	_body.add_child(Nocturne.divider())
	_body.add_child(Nocturne.section_header(3, "BY FALLBACK"))
	# Anything past "looks_like" is art the tileset does not have; colour it so
	# the difference between a covered map and a patched-up one is visible at a
	# glance rather than needing the numbers read.
	var tone := {
		"exact": Nocturne.GOOD, "looks_like": Nocturne.TEXT,
		"ascii": Nocturne.WARN, "category": Nocturne.WARN,
		"glyph": Nocturne.BAD, "missing": Nocturne.BAD,
	}
	var by_fallback: Dictionary = st.get("by_fallback", {})
	for level in ["exact", "looks_like", "ascii", "category", "glyph", "missing"]:
		var n := int(by_fallback.get(level, 0))
		if n > 0:
			_body.add_child(Nocturne.kv_row(level, str(n), tone.get(level, Nocturne.TEXT)))

	if not _host.has_method("get_sprite_coverage"):
		return
	var cov: Array = _host.get_sprite_coverage(MISS_ROWS)
	if cov.is_empty():
		return
	_body.add_child(Nocturne.divider())
	_body.add_child(Nocturne.section_header(4, "TOP SPRITE MISSES (%d ids)"
		% int(st.get("missing_ids", cov.size()))))
	for row in cov:
		_body.add_child(Nocturne.kv_row(
			"%s  %s" % [str(row.get("id", "")), str(row.get("level_name", ""))],
			str(row.get("hits", 0)),
			tone.get(str(row.get("level_name", "")), Nocturne.TEXT)))
