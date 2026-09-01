extends Control
## The options screen, as a Godot Control.
##
## The game thread is blocked in options_manager::show() while this is up. It
## published the pages once and is now waiting: every change the player makes is
## sent over as a request, applied to the real option object, and read back. The
## panel never decides what a value became -- an option can clamp a number, refuse
## an unknown choice, or ignore a write whose prerequisite is unmet, and showing
## the typed value would be showing something the game does not hold.
##
## Layout and values arrive on separate generations. Rebuilding a few hundred rows
## on every keystroke would also throw away the scroll position and the keyboard
## row, so the rows are built once per layout and only their value widgets are
## refreshed. See src/godot_options_snapshot.h.
##
## Groups collapse and tabs switch entirely on this side: the panel has the whole
## model, so neither needs the game thread.

const N := preload("res://scripts/nocturne.gd")

## Mirrors OptionsSnapshot::row::kind.
const ROW_BLANK := 0
const ROW_GROUP := 1
const ROW_OPTION := 2

var _host: Node
var _layout_generation: int = -1
var _values_generation: int = -1

var _pages: Array = []
var _page: int = 0
var _allow_tabs: bool = true
## Group id -> whether it is expanded. Groups start collapsed, as in curses.
var _expanded: Dictionary = {}

var _tab_row: HBoxContainer
var _rows_box: VBoxContainer
var _scroll: ScrollContainer
var _tooltip: Label
var _tab_buttons: Array[Button] = []

## Per visible row, in draw order: { "kind", "id", "node", "value_node", "tooltip" }.
var _visible: Array = []
var _selected: int = 0
## Set while a LineEdit has the keyboard, so arrow keys type instead of stepping.
var _editing: bool = false

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _rows_box != null:
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.55)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(dim)

	# Wider than the other panels: an option row is a label and a control side by
	# side, and the names run long.
	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -430.0
	frame.offset_right = 430.0
	frame.offset_top = -300.0
	frame.offset_bottom = 300.0
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(N.BG.r, N.BG.g, N.BG.b, 0.99)
	sb.border_color = N.NEUTRAL_800
	sb.set_border_width_all(1)
	sb.border_width_top = 3
	sb.content_margin_left = 24
	sb.content_margin_right = 24
	sb.content_margin_top = 20
	sb.content_margin_bottom = 16
	frame.add_theme_stylebox_override("panel", sb)
	add_child(frame)

	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", N.SPACE_S)
	frame.add_child(col)

	var title := Label.new()
	title.text = "OPTIONS"
	title.add_theme_font_size_override("font_size", 17)
	title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(title)

	_tab_row = HBoxContainer.new()
	_tab_row.add_theme_constant_override("separation", 4)
	col.add_child(_tab_row)

	col.add_child(N.fade_rule())

	_scroll = ScrollContainer.new()
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(_scroll)
	_rows_box = VBoxContainer.new()
	_rows_box.add_theme_constant_override("separation", 2)
	_rows_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_scroll.add_child(_rows_box)

	col.add_child(N.fade_rule())

	# The tooltip is a fixed two lines so the rows below do not shuffle as the
	# selection moves between options with descriptions of different lengths.
	_tooltip = Label.new()
	_tooltip.add_theme_font_size_override("font_size", 11)
	_tooltip.add_theme_color_override("font_color", N.NEUTRAL_500)
	_tooltip.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_tooltip.custom_minimum_size = Vector2(0, 32)
	_tooltip.clip_text = true
	col.add_child(_tooltip)

	col.add_child(N.micro_label(
		"↑↓ move · ←→ change · Enter edit · Tab page · Esc close", N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_options_layout"):
		return
	if _rows_box == null:
		_build()

	var lay_gen: int = int(_host.options_layout_generation())
	if lay_gen != _layout_generation:
		_layout_generation = lay_gen
		var d: Dictionary = _host.get_options_layout()
		_pages = d.get("pages", [])
		_page = clampi(int(d.get("current_page", 0)), 0, maxi(0, _pages.size() - 1))
		_allow_tabs = bool(d.get("allow_tabs", true))
		_expanded.clear()
		_selected = 0
		_rebuild_tabs()
		_rebuild_rows()

	var val_gen: int = int(_host.options_values_generation())
	if val_gen != _values_generation:
		_values_generation = val_gen
		_apply_values(_host.get_options_values())

func _rebuild_tabs() -> void:
	for child in _tab_row.get_children():
		_tab_row.remove_child(child)
		child.queue_free()
	_tab_buttons.clear()
	_tab_row.visible = _allow_tabs and _pages.size() > 1
	if not _tab_row.visible:
		return
	for i in _pages.size():
		var idx := i
		var btn := Button.new()
		btn.text = str(_pages[i].get("name", ""))
		btn.focus_mode = Control.FOCUS_NONE
		btn.pressed.connect(func() -> void: _select_page(idx))
		_tab_row.add_child(btn)
		_tab_buttons.append(btn)
	_paint_tabs()

func _paint_tabs() -> void:
	for i in _tab_buttons.size():
		var on := i == _page
		var sb := StyleBoxFlat.new()
		sb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.15) if on else Color(0, 0, 0, 0)
		sb.border_color = N.ACCENT if on else N.NEUTRAL_800
		sb.set_border_width_all(1)
		sb.border_width_bottom = 2 if on else 1
		sb.content_margin_left = 11
		sb.content_margin_right = 11
		sb.content_margin_top = 5
		sb.content_margin_bottom = 5
		for style in ["normal", "hover", "pressed"]:
			_tab_buttons[i].add_theme_stylebox_override(style, sb)
		_tab_buttons[i].add_theme_color_override("font_color", N.TEXT if on else N.NEUTRAL_500)
		_tab_buttons[i].add_theme_font_size_override("font_size", 11)

func _select_page(index: int) -> void:
	if index == _page or index < 0 or index >= _pages.size():
		return
	_page = index
	_selected = 0
	_paint_tabs()
	_rebuild_rows()
	if _host != null and _host.has_method("get_options_values"):
		_apply_values(_host.get_options_values())

## Rows are rebuilt on a page change or a group collapse, never on a value change.
func _rebuild_rows() -> void:
	for child in _rows_box.get_children():
		_rows_box.remove_child(child)
		child.queue_free()
	_visible.clear()
	if _page < 0 or _page >= _pages.size():
		return

	for entry in _pages[_page].get("rows", []):
		var kind := int(entry.get("type", ROW_BLANK))
		var group := str(entry.get("group", ""))
		if kind == ROW_GROUP:
			_add_group_row(entry)
		elif kind == ROW_OPTION:
			# A row inside a collapsed group is not built at all, so the keyboard
			# cannot land on something the player cannot see.
			if group == "" or bool(_expanded.get(group, false)):
				_add_option_row(entry)
		# Blank lines are separators in a fixed-width terminal; here the row gap
		# already does that job, so they are dropped.
	_highlight()

func _add_group_row(entry: Dictionary) -> void:
	var gid := str(entry.get("id", ""))
	var open: bool = bool(_expanded.get(gid, false))
	var btn := Button.new()
	btn.flat = true
	btn.focus_mode = Control.FOCUS_NONE
	btn.custom_minimum_size = Vector2(0, 26)
	btn.text = ("▾  " if open else "▸  ") + str(entry.get("text", "")).to_upper()
	btn.alignment = HORIZONTAL_ALIGNMENT_LEFT
	btn.add_theme_font_size_override("font_size", 11)
	btn.add_theme_color_override("font_color", N.ACCENT_300)
	var index := _visible.size()
	btn.pressed.connect(func() -> void:
		_selected = index
		_toggle_group(gid))
	_rows_box.add_child(btn)
	_visible.append({
		"kind": ROW_GROUP, "id": gid, "node": btn, "value_node": null,
		"tooltip": str(entry.get("tooltip", "")),
	})

func _toggle_group(gid: String) -> void:
	_expanded[gid] = not bool(_expanded.get(gid, false))
	var keep := _selected
	_rebuild_rows()
	_selected = clampi(keep, 0, maxi(0, _visible.size() - 1))
	if _host != null and _host.has_method("get_options_values"):
		_apply_values(_host.get_options_values())
	_highlight()

func _add_option_row(entry: Dictionary) -> void:
	var name_id := str(entry.get("id", ""))
	var vtype := str(entry.get("value_type", ""))

	# The row is wrapped in a PanelContainer purely so the selection has something
	# to paint: a box container has no panel stylebox, and overriding one on it
	# fails silently rather than visibly.
	var holder := PanelContainer.new()
	holder.custom_minimum_size = Vector2(0, 26)
	_rows_box.add_child(holder)

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", N.SPACE_M)
	holder.add_child(row)
	# Indent options that belong to a group, so the header reads as a parent.
	if str(entry.get("group", "")) != "":
		var indent := Control.new()
		indent.custom_minimum_size = Vector2(14, 0)
		row.add_child(indent)

	var label := Label.new()
	label.text = str(entry.get("text", ""))
	label.add_theme_font_size_override("font_size", 12)
	label.add_theme_color_override("font_color", N.NEUTRAL_300)
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	label.clip_text = true
	row.add_child(label)

	var value_node: Control = _make_value_node(name_id, vtype, entry)
	value_node.custom_minimum_size = Vector2(210, 24)
	row.add_child(value_node)

	_visible.append({
		"kind": ROW_OPTION, "id": name_id, "node": holder, "label": label,
		"value_node": value_node, "tooltip": str(entry.get("tooltip", "")),
		"value_type": vtype,
	})

func _make_value_node(name_id: String, vtype: String, entry: Dictionary) -> Control:
	if vtype == "string_select":
		var opt := OptionButton.new()
		opt.focus_mode = Control.FOCUS_NONE
		opt.add_theme_font_size_override("font_size", 11)
		opt.set_meta("values", [])
		var values: Array = []
		for item in entry.get("items", []):
			opt.add_item(str(item.get("label", "")))
			values.append(str(item.get("value", "")))
		opt.set_meta("values", values)
		opt.item_selected.connect(func(i: int) -> void:
			var vals: Array = opt.get_meta("values", [])
			if i >= 0 and i < vals.size():
				_request_set(name_id, str(vals[i])))
		return opt

	if vtype == "string_input":
		var edit := LineEdit.new()
		edit.add_theme_font_size_override("font_size", 11)
		edit.max_length = int(entry.get("max_length", 0))
		edit.focus_entered.connect(func() -> void: _editing = true)
		edit.focus_exited.connect(func() -> void: _editing = false)
		edit.text_submitted.connect(func(t: String) -> void:
			_request_set(name_id, t)
			edit.release_focus())
		return edit

	# bool, int, float and int_map are all "step to the next value": the option
	# object owns the range, the wrap and the step size, so the panel only ever
	# asks for one place forward or back and prints what came back.
	var stepper := HBoxContainer.new()
	stepper.add_theme_constant_override("separation", 2)
	var prev := _step_button("‹", func() -> void: _request_step(name_id, -1))
	var shown := Label.new()
	shown.add_theme_font_size_override("font_size", 11)
	shown.add_theme_color_override("font_color", N.TEXT)
	shown.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	shown.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	shown.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	shown.clip_text = true
	var next := _step_button("›", func() -> void: _request_step(name_id, 1))
	stepper.add_child(prev)
	stepper.add_child(shown)
	stepper.add_child(next)
	stepper.set_meta("display", shown)
	stepper.set_meta("buttons", [prev, next])
	return stepper

func _step_button(glyph: String, action: Callable) -> Button:
	var btn := Button.new()
	btn.text = glyph
	btn.flat = true
	btn.focus_mode = Control.FOCUS_NONE
	btn.custom_minimum_size = Vector2(22, 0)
	btn.add_theme_font_size_override("font_size", 13)
	btn.add_theme_color_override("font_color", N.NEUTRAL_500)
	btn.pressed.connect(action)
	return btn

## Push the values the game thread reported into the widgets already on screen.
func _apply_values(values: Dictionary) -> void:
	for item in _visible:
		if item["kind"] != ROW_OPTION:
			continue
		var v: Dictionary = values.get(item["id"], {})
		if v.is_empty():
			continue
		var enabled := bool(v.get("enabled", true))
		var node: Control = item["value_node"]
		node.modulate = Color(1, 1, 1, 1.0 if enabled else 0.4)
		if node is OptionButton:
			var vals: Array = node.get_meta("values", [])
			# select() does not emit item_selected, which is what we want: this is
			# the game reporting a value, not the player choosing one.
			node.select(vals.find(str(v.get("current", ""))))
			node.disabled = not enabled
		elif node is LineEdit:
			# Never overwrite text the player is in the middle of typing.
			if not node.has_focus():
				node.text = str(v.get("current", ""))
			node.editable = enabled
		elif node.has_meta("display"):
			var shown: Label = node.get_meta("display")
			shown.text = str(v.get("display", ""))
			for btn in node.get_meta("buttons", []):
				btn.disabled = not enabled

func _highlight() -> void:
	for i in _visible.size():
		var on := i == _selected
		var node: Control = _visible[i]["node"]
		if node is Button:
			var sb := StyleBoxFlat.new()
			sb.bg_color = (Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.15) if on
				else Color(0, 0, 0, 0))
			sb.border_color = N.ACCENT
			sb.border_width_left = 2 if on else 0
			sb.content_margin_left = 8
			for style in ["normal", "hover", "pressed"]:
				node.add_theme_stylebox_override(style, sb)
		else:
			var panel := StyleBoxFlat.new()
			panel.bg_color = (Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.10) if on
				else Color(0, 0, 0, 0))
			panel.border_color = N.ACCENT
			panel.border_width_left = 2 if on else 0
			panel.content_margin_left = 8
			panel.content_margin_right = 6
			node.add_theme_stylebox_override("panel", panel)
			var label: Label = _visible[i]["label"]
			label.add_theme_color_override("font_color", N.TEXT if on else N.NEUTRAL_300)
	if _selected >= 0 and _selected < _visible.size():
		_tooltip.text = str(_visible[_selected]["tooltip"])
		_ensure_visible(_visible[_selected]["node"])
	else:
		_tooltip.text = ""

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

func _request_set(option: String, value: String) -> void:
	if _host != null and _host.has_method("options_set"):
		_host.options_set(option, value)

func _request_step(option: String, delta: int) -> void:
	if _host != null and _host.has_method("options_step"):
		_host.options_step(option, delta)

func _dismiss() -> void:
	if _host != null and _host.has_method("options_dismiss"):
		_host.options_dismiss()

# --- input --------------------------------------------------------------------

func _move(delta: int) -> void:
	if _visible.is_empty():
		return
	_selected = wrapi(_selected + delta, 0, _visible.size())
	_highlight()

func _activate() -> void:
	if _selected < 0 or _selected >= _visible.size():
		return
	var item: Dictionary = _visible[_selected]
	if item["kind"] == ROW_GROUP:
		_toggle_group(str(item["id"]))
		return
	var node: Control = item["value_node"]
	if node is OptionButton:
		node.show_popup()
	elif node is LineEdit:
		node.grab_focus()
		node.caret_column = node.text.length()
	else:
		_request_step(str(item["id"]), 1)

func _step_selected(delta: int) -> void:
	if _selected < 0 or _selected >= _visible.size():
		return
	var item: Dictionary = _visible[_selected]
	if item["kind"] == ROW_GROUP:
		_toggle_group(str(item["id"]))
	elif item["value_node"] is LineEdit:
		pass  # nothing to step; the text is edited in place
	else:
		_request_step(str(item["id"]), delta)

func _unhandled_input(event: InputEvent) -> void:
	if not visible or not (event is InputEventKey) or not event.pressed or event.echo:
		return
	# While a LineEdit has the keyboard, arrows move the caret and Esc should give
	# the field back rather than close the whole screen.
	if _editing:
		if event.keycode == KEY_ESCAPE:
			get_viewport().gui_release_focus()
			_editing = false
			get_viewport().set_input_as_handled()
		return
	match event.keycode:
		KEY_ESCAPE:
			_dismiss()
		KEY_UP:
			_move(-1)
		KEY_DOWN:
			_move(1)
		KEY_LEFT:
			_step_selected(-1)
		KEY_RIGHT:
			_step_selected(1)
		KEY_ENTER, KEY_KP_ENTER, KEY_SPACE:
			_activate()
		KEY_TAB:
			if _allow_tabs and _pages.size() > 1:
				_select_page(wrapi(_page + (-1 if event.shift_pressed else 1), 0, _pages.size()))
		KEY_PAGEUP:
			_move(-8)
		KEY_PAGEDOWN:
			_move(8)
		KEY_HOME:
			_selected = 0
			_highlight()
		KEY_END:
			_selected = maxi(0, _visible.size() - 1)
			_highlight()
		_:
			return
	get_viewport().set_input_as_handled()
