extends Control
## The surroundings list — "look around" — as a Godot Control.
##
## Three tabs of what is near you: items, monsters, terrain and furniture. The
## game thread is blocked in surroundings_menu::execute while this is up, and
## that loop still owns every action. This panel replaces where the action comes
## from and nothing else, so routing to a destination, firing at the selection,
## the examine pane and the filter popups all still run where they always did —
## and the last two open their own Godot panels on top of this one.
##
## Rows come from the entity stacks rather than from the old drawing code; see
## src/godot_surroundings_snapshot.h for why that distinction mattered here and
## not for the overmap sidebar.

const N := preload("res://scripts/nocturne.gd")
const TAGS := preload("res://scripts/color_tags.gd")

var _host: Node
var _generation: int = -1
var _rows: Array = []
var _selected: int = -1

var _tab_row: HBoxContainer
var _list: VBoxContainer
var _scroll: ScrollContainer
var _status: Label
var _row_nodes: Array[PanelContainer] = []
var _tab_nodes: Array[Button] = []

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _list != null:
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.5)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(dim)

	# Anchored right, like the terminal version: this is a list *about* the map,
	# and the map stays visible beside it while you scroll.
	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_RIGHT_WIDE)
	frame.offset_left = -420.0
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(N.BG.r, N.BG.g, N.BG.b, 0.97)
	sb.border_color = N.NEUTRAL_800
	sb.border_width_left = 1
	sb.content_margin_left = 18
	sb.content_margin_right = 16
	sb.content_margin_top = 16
	sb.content_margin_bottom = 12
	frame.add_theme_stylebox_override("panel", sb)
	add_child(frame)

	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", N.SPACE_S)
	frame.add_child(col)

	var title := Label.new()
	title.text = "SURROUNDINGS"
	title.add_theme_font_size_override("font_size", 16)
	title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(title)

	_tab_row = HBoxContainer.new()
	_tab_row.add_theme_constant_override("separation", 3)
	col.add_child(_tab_row)

	col.add_child(N.fade_rule())

	_scroll = ScrollContainer.new()
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(_scroll)
	_list = VBoxContainer.new()
	_list.add_theme_constant_override("separation", 1)
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_scroll.add_child(_list)

	col.add_child(N.fade_rule())

	_status = Label.new()
	_status.add_theme_font_size_override("font_size", 11)
	_status.add_theme_color_override("font_color", N.NEUTRAL_600)
	col.add_child(_status)

	col.add_child(N.micro_label(
		"↑↓ move · Tab list · Enter examine · t travel · f fire · / filter · Esc close",
		N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_surroundings_state"):
		return
	if _list == null:
		_build()
	var gen: int = int(_host.surroundings_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_surroundings_state()
	_rows = d.get("rows", [])
	_selected = int(d.get("selected", -1))
	_rebuild_tabs(d.get("tabs", []), int(d.get("tab", 0)))
	_rebuild_rows()
	var bits: Array[String] = ["%d nearby" % _rows.size()]
	if str(d.get("filter", "")) != "":
		bits.append("filtered")
	_status.text = " · ".join(bits).to_upper()

func _rebuild_tabs(tabs: Array, current: int) -> void:
	for child in _tab_row.get_children():
		_tab_row.remove_child(child)
		child.queue_free()
	_tab_nodes.clear()
	for i in tabs.size():
		var btn := Button.new()
		btn.text = "%s (%d)" % [str(tabs[i].get("title", "")), int(tabs[i].get("count", 0))]
		btn.focus_mode = Control.FOCUS_NONE
		# Tabs are moved through relatively — the loop only offers NEXT_TAB and
		# PREV_TAB — so a click asks for the number of steps to get there.
		var delta := i - current
		btn.pressed.connect(func() -> void: _step_tab(delta))
		var on := i == current
		var sb := StyleBoxFlat.new()
		sb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.15) if on else Color(0, 0, 0, 0)
		sb.border_color = N.ACCENT if on else N.NEUTRAL_800
		sb.set_border_width_all(1)
		sb.border_width_bottom = 2 if on else 1
		sb.content_margin_left = 10
		sb.content_margin_right = 10
		sb.content_margin_top = 4
		sb.content_margin_bottom = 4
		for style in ["normal", "hover", "pressed"]:
			btn.add_theme_stylebox_override(style, sb)
		btn.add_theme_color_override("font_color", N.TEXT if on else N.NEUTRAL_500)
		btn.add_theme_font_size_override("font_size", 10)
		_tab_row.add_child(btn)
		_tab_nodes.append(btn)

func _rebuild_rows() -> void:
	for child in _list.get_children():
		_list.remove_child(child)
		child.queue_free()
	_row_nodes.clear()

	var last_category := ""
	for i in _rows.size():
		var entry: Dictionary = _rows[i]
		var category := str(entry.get("category", ""))
		if category != "" and category != last_category:
			last_category = category
			var head := Label.new()
			head.text = TAGS.strip(category).to_upper()
			head.add_theme_font_size_override("font_size", 10)
			head.add_theme_color_override("font_color", N.ACCENT_300)
			_list.add_child(head)

		var idx := i
		var holder := PanelContainer.new()
		holder.custom_minimum_size = Vector2(0, 22)
		holder.gui_input.connect(func(event: InputEvent) -> void:
			if event is InputEventMouseButton and event.pressed \
					and event.button_index == MOUSE_BUTTON_LEFT:
				_select(idx))
		_list.add_child(holder)

		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", N.SPACE_M)
		holder.add_child(row)

		var name_label := Label.new()
		var count := int(entry.get("count", 1))
		name_label.text = (("%d " % count) if count > 1 else "") + str(entry.get("text", ""))
		name_label.add_theme_font_size_override("font_size", 12)
		name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		name_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
		name_label.clip_text = true
		row.add_child(name_label)

		# The distance column is how you decide whether something is worth
		# walking to, so it keeps its own fixed column rather than wrapping.
		var dist := Label.new()
		dist.text = str(entry.get("distance", ""))
		dist.add_theme_font_size_override("font_size", 11)
		dist.add_theme_color_override("font_color", N.NEUTRAL_500)
		dist.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
		dist.custom_minimum_size = Vector2(64, 0)
		dist.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
		row.add_child(dist)

		holder.set_meta("label", name_label)
		var hex := TAGS.hex_for_name(str(entry.get("color", "")))
		holder.set_meta("base_color", Color(hex) if hex != "" else N.NEUTRAL_200)
		_row_nodes.append(holder)
	_highlight()

func _highlight() -> void:
	for i in _row_nodes.size():
		var on := i == _selected
		var sb := StyleBoxFlat.new()
		sb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.16) if on else Color(0, 0, 0, 0)
		sb.border_color = N.ACCENT
		sb.border_width_left = 2 if on else 0
		sb.content_margin_left = 8
		sb.content_margin_right = 6
		_row_nodes[i].add_theme_stylebox_override("panel", sb)
		var label: Label = _row_nodes[i].get_meta("label")
		label.add_theme_color_override("font_color",
			N.TEXT if on else _row_nodes[i].get_meta("base_color"))
	if _selected >= 0 and _selected < _row_nodes.size():
		_ensure_visible(_row_nodes[_selected])

func _ensure_visible(node: Control) -> void:
	if _scroll == null or node == null:
		return
	var top := node.position.y
	var bottom := top + node.size.y
	if top < _scroll.scroll_vertical:
		_scroll.scroll_vertical = int(top)
	elif bottom > _scroll.scroll_vertical + _scroll.size.y:
		_scroll.scroll_vertical = int(bottom - _scroll.size.y)

# --- requests -----------------------------------------------------------------

func _act(action: String) -> void:
	if _host != null and _host.has_method("surroundings_action"):
		_host.surroundings_action(action)

func _select(index: int) -> void:
	if _host != null and _host.has_method("surroundings_select"):
		_host.surroundings_select(index)

func _step_tab(delta: int) -> void:
	for i in absi(delta):
		_act("NEXT_TAB" if delta > 0 else "PREV_TAB")

func _unhandled_input(event: InputEvent) -> void:
	if not visible or not (event is InputEventKey) or not event.pressed or event.echo:
		return
	match event.keycode:
		KEY_ESCAPE:
			_act("QUIT")
		KEY_UP:
			_act("UP")
		KEY_DOWN:
			_act("DOWN")
		KEY_PAGEUP:
			_act("PAGE_UP")
		KEY_PAGEDOWN:
			_act("PAGE_DOWN")
		KEY_TAB:
			_act("PREV_TAB" if event.shift_pressed else "NEXT_TAB")
		KEY_ENTER, KEY_KP_ENTER:
			_act("EXAMINE")
		KEY_T:
			_act("TRAVEL_TO")
		KEY_F:
			_act("fire")
		KEY_I:
			_act("COMPARE")
		KEY_SLASH:
			_act("FILTER")
		KEY_BACKSPACE:
			_act("RESET_FILTER")
		KEY_S:
			_act("SORT")
		_:
			return
	get_viewport().set_input_as_handled()
