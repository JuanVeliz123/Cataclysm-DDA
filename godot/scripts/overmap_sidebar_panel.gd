extends Control
## The overmap sidebar, as a Godot Control.
##
## The overmap map itself has been an `OvermapView` node for a while; the sidebar
## next to it was still an ImGui window drawn over the top, which is the sort of
## seam that only shows up when you look at the screen. This is that sidebar.
##
## It is read-only. The overmap's input loop is unchanged and still owns every
## key — nothing here sends anything back. The text arrives already recorded from
## the same functions that used to draw it (see overmap_sidebar::record), so the
## content cannot drift from the terminal version; only the layout is ours.
##
## Anchored to the right edge rather than centred: it sits *beside* the map, and
## the map is a live node underneath, not a backdrop.

const N := preload("res://scripts/nocturne.gd")
const TAGS := preload("res://scripts/color_tags.gd")

## Matches OVERMAP_LEGEND_WIDTH's intent: a fifth of the screen, within reason.
const MIN_WIDTH := 300.0

var _host: Node
var _generation: int = -1
var _body: VBoxContainer
var _scroll: ScrollContainer

func setup(host: Node) -> void:
	_host = host
	# The map is a live node behind this, and clicks belong to it everywhere the
	# sidebar is not.
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _body != null:
		return

	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_RIGHT_WIDE)
	frame.offset_left = -MIN_WIDTH
	frame.mouse_filter = Control.MOUSE_FILTER_STOP
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(N.BG.r, N.BG.g, N.BG.b, 0.94)
	sb.border_color = N.NEUTRAL_800
	sb.border_width_left = 1
	sb.content_margin_left = 16
	sb.content_margin_right = 14
	sb.content_margin_top = 14
	sb.content_margin_bottom = 12
	frame.add_theme_stylebox_override("panel", sb)
	add_child(frame)

	_scroll = ScrollContainer.new()
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	frame.add_child(_scroll)
	_body = VBoxContainer.new()
	_body.add_theme_constant_override("separation", 2)
	_body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_scroll.add_child(_body)

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_overmap_sidebar"):
		return
	if _body == null:
		_build()
	var gen: int = int(_host.overmap_sidebar_generation())
	if gen == _generation:
		return
	_generation = gen
	_rebuild((_host.get_overmap_sidebar() as Dictionary).get("lines", []))

func _rebuild(lines: Array) -> void:
	for child in _body.get_children():
		_body.remove_child(child)
		child.queue_free()

	# A recorded line may continue the previous one — the terminal did that with
	# SameLine, e.g. a terrain glyph followed by its description. Consecutive
	# joined lines are gathered into one label so they read as one sentence.
	var i := 0
	while i < lines.size():
		var entry: Dictionary = lines[i]
		if bool(entry.get("header", false)):
			_add_header(str(entry.get("text", "")))
			i += 1
			continue
		var parts: Array[String] = [_markup(entry)]
		var indent := int(entry.get("indent", 0))
		var j := i + 1
		while j < lines.size() and bool(lines[j].get("join", false)) \
				and not bool(lines[j].get("header", false)):
			parts.append(_markup(lines[j]))
			j += 1
		_add_body("".join(parts), indent)
		i = j

func _markup(entry: Dictionary) -> String:
	var text := TAGS.to_bbcode(str(entry.get("text", "")))
	var hex := TAGS.hex_for_name(str(entry.get("color", "")))
	# The recorded colour is the line's own; any markup inside it wins locally.
	return ("[color=%s]%s[/color]" % [hex, text]) if hex != "" else text

func _add_header(title: String) -> void:
	if _body.get_child_count() > 0:
		_body.add_child(N.fade_rule())
	var head := Label.new()
	head.text = TAGS.strip(title).to_upper()
	head.add_theme_font_size_override("font_size", 11)
	head.add_theme_color_override("font_color", N.ACCENT_300)
	_body.add_child(head)

func _add_body(markup: String, indent: int) -> void:
	var row := MarginContainer.new()
	if indent > 0:
		row.add_theme_constant_override("margin_left", 12 * indent)
	_body.add_child(row)
	var label := RichTextLabel.new()
	label.bbcode_enabled = true
	label.fit_content = true
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	label.add_theme_font_size_override("normal_font_size", 12)
	label.add_theme_color_override("default_color", N.NEUTRAL_300)
	label.text = markup
	row.add_child(label)
