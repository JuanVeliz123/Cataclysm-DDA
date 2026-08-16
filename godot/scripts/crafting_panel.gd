extends Control
## The crafting screen, as a Godot Control.
##
## This one deliberately owns less than the other panels. `crafting_ui_impl`
## already knows what every action does — how batch mode auto-engages on the
## first increment, how the selection survives a recalculation, when the recipe
## list has to be rebuilt — so the panel sends back the same action strings the
## curses screen produces and lets that code decide. Reimplementing any of it here
## would be a second state machine that has to agree with the first.
##
## The game thread is blocked in select_crafter_and_crafting_recipe() while this
## is up. See src/godot_crafting_snapshot.h.

const N := preload("res://scripts/nocturne.gd")
const TAGS := preload("res://scripts/color_tags.gd")

var _host: Node
var _list_generation: int = -1
var _detail_generation: int = -1

var _rows: Array = []
var _selected: int = -1

var _tab_row: HBoxContainer
var _subtab_row: HBoxContainer
var _list: VBoxContainer
var _list_scroll: ScrollContainer
var _detail: VBoxContainer
var _detail_scroll: ScrollContainer
var _status: Label
var _row_nodes: Array[PanelContainer] = []
var _tab_nodes: Array[Button] = []
var _subtab_nodes: Array[Button] = []

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _list != null:
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.55)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(dim)

	# Two panes side by side: the recipe list, and what the selected recipe needs.
	# Wider than the other panels because the component lines are long.
	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -520.0
	frame.offset_right = 520.0
	frame.offset_top = -320.0
	frame.offset_bottom = 320.0
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(N.BG.r, N.BG.g, N.BG.b, 0.99)
	sb.border_color = N.NEUTRAL_800
	sb.set_border_width_all(1)
	sb.border_width_top = 3
	sb.content_margin_left = 22
	sb.content_margin_right = 22
	sb.content_margin_top = 18
	sb.content_margin_bottom = 14
	frame.add_theme_stylebox_override("panel", sb)
	add_child(frame)

	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", N.SPACE_S)
	frame.add_child(col)

	var title := Label.new()
	title.text = "CRAFTING"
	title.add_theme_font_size_override("font_size", 17)
	title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(title)

	_tab_row = HBoxContainer.new()
	_tab_row.add_theme_constant_override("separation", 3)
	col.add_child(_tab_row)
	_subtab_row = HBoxContainer.new()
	_subtab_row.add_theme_constant_override("separation", 3)
	col.add_child(_subtab_row)

	col.add_child(N.fade_rule())

	var panes := HBoxContainer.new()
	panes.add_theme_constant_override("separation", N.SPACE_L)
	panes.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(panes)

	_list_scroll = ScrollContainer.new()
	_list_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_list_scroll.custom_minimum_size = Vector2(430, 0)
	panes.add_child(_list_scroll)
	_list = VBoxContainer.new()
	_list.add_theme_constant_override("separation", 1)
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_list_scroll.add_child(_list)

	_detail_scroll = ScrollContainer.new()
	_detail_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_detail_scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panes.add_child(_detail_scroll)
	_detail = VBoxContainer.new()
	_detail.add_theme_constant_override("separation", 3)
	_detail.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_detail_scroll.add_child(_detail)

	col.add_child(N.fade_rule())

	_status = Label.new()
	_status.add_theme_font_size_override("font_size", 11)
	_status.add_theme_color_override("font_color", N.NEUTRAL_600)
	col.add_child(_status)

	col.add_child(N.micro_label(
		"↑↓ move · ←→ subcat · Tab cat · Enter craft · b/+/- batch · / filter · "
		+ "r related · c crafter · ? help · Esc close",
		N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_crafting_list"):
		return
	if _list == null:
		_build()

	var gen: int = int(_host.crafting_list_generation())
	if gen != _list_generation:
		_list_generation = gen
		var d: Dictionary = _host.get_crafting_list()
		_rows = d.get("rows", [])
		_selected = int(d.get("selected", 0))
		_rebuild_tabs(d)
		_rebuild_list()
		_update_status(d)
	else:
		# The cursor moves far more often than the list changes, so it is polled
		# on its own rather than through a list generation.
		var sel: int = int(_host.crafting_selected())
		if sel != _selected:
			_selected = sel
			_highlight()

	var dgen: int = int(_host.crafting_detail_generation())
	if dgen != _detail_generation:
		_detail_generation = dgen
		_rebuild_detail(_host.get_crafting_detail())

func _update_status(d: Dictionary) -> void:
	var bits: Array[String] = ["%d recipes" % _rows.size()]
	var filter := str(d.get("filter", ""))
	if filter != "":
		bits.append("filter: %s" % filter)
	var hidden := int(d.get("hidden", 0))
	if hidden > 0:
		bits.append("%d hidden" % hidden)
	if bool(d.get("batch_mode", false)):
		bits.append("batch x%d" % int(d.get("batch_size", 1)))
	_status.text = " · ".join(bits).to_upper()

func _rebuild_tabs(d: Dictionary) -> void:
	_tab_nodes = _fill_tab_row(_tab_row, d.get("tabs", []), int(d.get("tab", 0)),
		func(i: int) -> void: _host.crafting_select_tab(i))
	_subtab_nodes = _fill_tab_row(_subtab_row, d.get("subtabs", []), int(d.get("subtab", 0)),
		func(i: int) -> void: _host.crafting_select_subtab(i))

func _fill_tab_row(row: HBoxContainer, tabs: Array, current: int,
		on_click: Callable) -> Array[Button]:
	for child in row.get_children():
		row.remove_child(child)
		child.queue_free()
	var out: Array[Button] = []
	row.visible = not tabs.is_empty()
	for i in tabs.size():
		var idx := i
		var btn := Button.new()
		btn.text = str(tabs[i].get("name", ""))
		btn.focus_mode = Control.FOCUS_NONE
		btn.pressed.connect(func() -> void: on_click.call(idx))
		var on := i == current
		var sb := StyleBoxFlat.new()
		sb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.15) if on else Color(0, 0, 0, 0)
		sb.border_color = N.ACCENT if on else N.NEUTRAL_800
		sb.set_border_width_all(1)
		sb.border_width_bottom = 2 if on else 1
		sb.content_margin_left = 9
		sb.content_margin_right = 9
		sb.content_margin_top = 4
		sb.content_margin_bottom = 4
		for style in ["normal", "hover", "pressed"]:
			btn.add_theme_stylebox_override(style, sb)
		btn.add_theme_color_override("font_color", N.TEXT if on else N.NEUTRAL_500)
		btn.add_theme_font_size_override("font_size", 10)
		row.add_child(btn)
		out.append(btn)
	return out

func _rebuild_list() -> void:
	for child in _list.get_children():
		_list.remove_child(child)
		child.queue_free()
	_row_nodes.clear()

	for i in _rows.size():
		var entry: Dictionary = _rows[i]
		var idx := i
		var holder := PanelContainer.new()
		holder.custom_minimum_size = Vector2(0, 22)
		holder.gui_input.connect(func(event: InputEvent) -> void:
			if event is InputEventMouseButton and event.pressed \
					and event.button_index == MOUSE_BUTTON_LEFT:
				_host.crafting_select_row(idx))
		_list.add_child(holder)

		var line := HBoxContainer.new()
		holder.add_child(line)
		var indent := int(entry.get("indent", 0))
		if indent > 0:
			var pad := Control.new()
			pad.custom_minimum_size = Vector2(14 * indent, 0)
			line.add_child(pad)

		var label := Label.new()
		var nested := bool(entry.get("nested", false))
		label.text = ("▸ " if nested else "") + str(entry.get("name", ""))
		label.add_theme_font_size_override("font_size", 12)
		label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
		label.clip_text = true
		# The same three states the curses list shows: craftable, craftable with a
		# caveat (rotten or favourited ingredients, a missing optional
		# proficiency), and not craftable now.
		if nested:
			label.add_theme_color_override("font_color", N.ACCENT_300)
		elif not bool(entry.get("craftable", false)):
			label.add_theme_color_override("font_color", N.NEUTRAL_600)
		elif bool(entry.get("caveat", false)):
			label.add_theme_color_override("font_color", N.WARN)
		else:
			label.add_theme_color_override("font_color", N.NEUTRAL_200)
		line.add_child(label)

		holder.set_meta("label", label)
		holder.set_meta("base_color", label.get_theme_color("font_color"))
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

func _rebuild_detail(d: Dictionary) -> void:
	for child in _detail.get_children():
		_detail.remove_child(child)
		child.queue_free()
	_detail_scroll.scroll_vertical = 0
	for entry in d.get("lines", []):
		var text := str(entry.get("text", ""))
		if bool(entry.get("header", false)):
			var head := Label.new()
			head.text = TAGS.strip(text).to_upper()
			head.add_theme_font_size_override("font_size", 12)
			head.add_theme_color_override("font_color", N.ACCENT_300)
			head.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
			_detail.add_child(head)
			continue
		# Body lines carry CDDA colour tags, already resolved against the
		# crafting inventory by requirement_data.
		var body := RichTextLabel.new()
		body.bbcode_enabled = true
		body.fit_content = true
		body.text = TAGS.to_bbcode(text)
		body.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		body.add_theme_color_override("default_color", N.NEUTRAL_300)
		body.add_theme_font_size_override("normal_font_size", 12)
		_detail.add_child(body)

func _ensure_visible(node: Control) -> void:
	if _list_scroll == null or node == null:
		return
	var top := node.position.y
	var bottom := top + node.size.y
	if top < _list_scroll.scroll_vertical:
		_list_scroll.scroll_vertical = int(top)
	elif bottom > _list_scroll.scroll_vertical + _list_scroll.size.y:
		_list_scroll.scroll_vertical = int(bottom - _list_scroll.size.y)

# --- input --------------------------------------------------------------------

## Every key becomes the action string the crafting input context would have
## produced, so the C++ side sees exactly what it sees from the curses screen.
func _act(action: String) -> void:
	if _host != null and _host.has_method("crafting_action"):
		_host.crafting_action(action)

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
		KEY_LEFT:
			_act("LEFT")
		KEY_RIGHT:
			_act("RIGHT")
		KEY_PAGEUP:
			_act("PAGE_UP")
		KEY_PAGEDOWN:
			_act("PAGE_DOWN")
		KEY_HOME:
			_act("HOME")
		KEY_END:
			_act("END")
		KEY_TAB:
			_act("PREV_TAB" if event.shift_pressed else "NEXT_TAB")
		KEY_ENTER, KEY_KP_ENTER:
			_act("CONFIRM")
		KEY_B:
			_act("CYCLE_BATCH")
		KEY_EQUAL, KEY_PLUS, KEY_KP_ADD:
			_act("BATCH_SIZE_UP")
		KEY_MINUS, KEY_KP_SUBTRACT:
			_act("BATCH_SIZE_DOWN")
		KEY_R:
			_act("RELATED_RECIPES")
		KEY_C:
			_act("CHOOSE_CRAFTER")
		KEY_BACKSPACE:
			_act("RESET_FILTER")
		# These three open other screens, and each already has a Godot panel: the
		# related-recipe browser is a uilist, recipe help and comparison are text
		# windows. They arrive over their own channels and draw on top of this.
		KEY_QUESTION:
			_act("HELP_RECIPE")
		KEY_I:
			_act("COMPARE")
		KEY_P:
			_act("PRIORITIZE_MISSING_COMPONENTS" if not event.shift_pressed
				else "DEPRIORITIZE_COMPONENTS")
		KEY_U:
			_act("TOGGLE_RECIPE_UNREAD")
		KEY_SLASH:
			_act("FILTER")
		KEY_H:
			_act("HIDE_SHOW_RECIPE")
		KEY_F:
			_act("TOGGLE_FAVORITE")
		_:
			return
	get_viewport().set_input_as_handled()
