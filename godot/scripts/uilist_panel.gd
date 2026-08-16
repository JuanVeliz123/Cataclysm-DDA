extends Control
## Generic menu panel: renders any `uilist` as a Godot Control.
##
## uilist is the single mechanism behind most of the game's menus -- 254 call
## sites -- so this one panel moves nearly all of them off the curses/ImGui
## overlay at once. The game thread is blocked inside uilist::query() the whole
## time this is up, polling the same snapshot for the answer; see
## src/godot_uilist_snapshot.h.
##
## Category tabs are supported. Menus whose uilist_callback needs the C++ UI --
## one that draws its own pane or intercepts keys -- are not routed here; C++
## declines those and they still use the legacy path.

const N := preload("res://scripts/nocturne.gd")

const MAX_VISIBLE := 18

var _host: Node
var _title: Label
var _subtitle: Label
var _tab_row: HBoxContainer
var _tab_nodes: Array[Button] = []
var _current_tab: int = 0
var _rows_box: VBoxContainer
var _desc: RichTextLabel
var _desc_pane: Control
var _footer: Label
var _filter: LineEdit
var _filter_row: Control

## Mirrors the C++ entry list: [{text, ctxt, desc, hotkey, enabled, index}]
var _entries: Array = []
## Index into uilist::entries of the highlighted row.
var _selected: int = -1
var _generation: int = -1
var _row_nodes: Array[Button] = []

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _rows_box != null:
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.5)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(dim)

	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -330.0
	frame.offset_top = -260.0
	frame.offset_right = 330.0
	frame.offset_bottom = 260.0
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(N.BG.r, N.BG.g, N.BG.b, 0.985)
	sb.border_color = N.NEUTRAL_800
	sb.set_border_width_all(1)
	sb.border_width_top = 3
	sb.set_content_margin_all(0)
	frame.add_theme_stylebox_override("panel", sb)
	add_child(frame)

	var pad := MarginContainer.new()
	pad.add_theme_constant_override("margin_left", 22)
	pad.add_theme_constant_override("margin_right", 22)
	pad.add_theme_constant_override("margin_top", 18)
	pad.add_theme_constant_override("margin_bottom", 16)
	frame.add_child(pad)

	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", N.SPACE_S)
	pad.add_child(col)

	_title = Label.new()
	_title.add_theme_font_size_override("font_size", 16)
	_title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(_title)
	_subtitle = Label.new()
	_subtitle.add_theme_font_size_override("font_size", 11)
	_subtitle.add_theme_color_override("font_color", N.NEUTRAL_500)
	_subtitle.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	col.add_child(_subtitle)
	# Category tabs, drawn only for menus that have them.
	_tab_row = HBoxContainer.new()
	_tab_row.add_theme_constant_override("separation", 4)
	_tab_row.visible = false
	col.add_child(_tab_row)

	col.add_child(N.fade_rule())

	var body := HBoxContainer.new()
	body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.add_theme_constant_override("separation", N.SPACE_L)
	col.add_child(body)

	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.add_child(scroll)
	_rows_box = VBoxContainer.new()
	_rows_box.add_theme_constant_override("separation", 2)
	_rows_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(_rows_box)

	# Only shown for menus that set desc_enabled.
	_desc_pane = VBoxContainer.new()
	_desc_pane.custom_minimum_size = Vector2(240, 0)
	_desc_pane.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.add_child(_desc_pane)
	_desc = RichTextLabel.new()
	_desc.bbcode_enabled = false
	_desc.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_desc.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_desc.add_theme_color_override("default_color", N.NEUTRAL_400)
	_desc_pane.add_child(_desc)

	_footer = Label.new()
	_footer.add_theme_font_size_override("font_size", 10)
	_footer.add_theme_color_override("font_color", N.NEUTRAL_600)
	_footer.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	col.add_child(_footer)

	_filter_row = HBoxContainer.new()
	_filter_row.add_theme_constant_override("separation", 8)
	col.add_child(_filter_row)
	var glass := Label.new()
	glass.text = "⌕"
	glass.add_theme_color_override("font_color", N.ACCENT_400)
	_filter_row.add_child(glass)
	_filter = LineEdit.new()
	_filter.placeholder_text = "Filter"
	_filter.flat = true
	_filter.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_filter.add_theme_color_override("font_color", N.TEXT)
	_filter.text_changed.connect(func(t: String) -> void:
		if _host != null and _host.has_method("uilist_set_filter"):
			_host.uilist_set_filter(t))
	_filter_row.add_child(_filter)

	col.add_child(N.fade_rule())
	var hint := N.micro_label("↑↓ move · Enter choose · Esc cancel", N.NEUTRAL_700)
	col.add_child(hint)

# --- update -----------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_uilist_state"):
		return
	if _rows_box == null:
		_build()
	var gen: int = int(_host.uilist_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_uilist_state()
	_entries = d.get("entries", [])
	_selected = int(d.get("selected", -1))
	_title.text = str(d.get("title", "")).to_upper()
	var text := str(d.get("text", ""))
	_subtitle.text = text
	_subtitle.visible = text != ""
	var footer := str(d.get("footer", ""))
	_footer.text = footer
	_footer.visible = footer != ""
	_desc_pane.visible = bool(d.get("desc_enabled", false))
	_filter_row.visible = bool(d.get("filtering", false))
	_rebuild_tabs(d.get("categories", []), int(d.get("current_category", 0)))
	_rebuild_rows()
	_update_desc()

func _rebuild_tabs(labels: Array, current: int) -> void:
	_current_tab = current
	if labels.size() < 2:
		# One category is the same as none; a lone tab is chrome, not navigation.
		_tab_row.visible = false
		_tab_nodes.clear()
		for child in _tab_row.get_children():
			_tab_row.remove_child(child)
			child.queue_free()
		return
	_tab_row.visible = true
	# Rebuild only when the label set changes: the tabs are fixed for the life of
	# a menu, and the highlight is repainted separately.
	if _tab_nodes.size() != labels.size():
		for child in _tab_row.get_children():
			_tab_row.remove_child(child)
			child.queue_free()
		_tab_nodes.clear()
		for i in labels.size():
			var idx := i
			var btn := Button.new()
			btn.text = str(labels[i])
			btn.focus_mode = Control.FOCUS_NONE
			btn.pressed.connect(func() -> void: _select_tab(idx))
			_tab_row.add_child(btn)
			_tab_nodes.append(btn)
	else:
		for i in labels.size():
			_tab_nodes[i].text = str(labels[i])
	_paint_tabs()

func _paint_tabs() -> void:
	for i in _tab_nodes.size():
		var on := i == _current_tab
		var sb := StyleBoxFlat.new()
		sb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.15) if on else Color(0, 0, 0, 0)
		sb.border_color = N.ACCENT if on else N.NEUTRAL_800
		sb.set_border_width_all(1)
		sb.border_width_bottom = 2 if on else 1
		sb.content_margin_left = 11
		sb.content_margin_right = 11
		sb.content_margin_top = 4
		sb.content_margin_bottom = 4
		for style in ["normal", "hover", "pressed"]:
			_tab_nodes[i].add_theme_stylebox_override(style, sb)
		_tab_nodes[i].add_theme_color_override("font_color", N.TEXT if on else N.NEUTRAL_500)
		_tab_nodes[i].add_theme_font_size_override("font_size", 11)

func _select_tab(index: int) -> void:
	if index == _current_tab or _host == null or not _host.has_method("uilist_select_category"):
		return
	_current_tab = index
	_paint_tabs()
	_host.uilist_select_category(index)

func _rebuild_rows() -> void:
	for child in _rows_box.get_children():
		_rows_box.remove_child(child)
		child.queue_free()
	_row_nodes.clear()

	for e in _entries:
		if typeof(e) != TYPE_DICTIONARY:
			continue
		var idx := int(e.get("index", -1))
		var enabled := bool(e.get("enabled", true))
		var btn := Button.new()
		btn.flat = true
		btn.focus_mode = Control.FOCUS_NONE
		btn.custom_minimum_size = Vector2(0, 26)
		btn.pressed.connect(func() -> void: _activate(idx, enabled))
		btn.mouse_entered.connect(func() -> void: _set_selected(idx))
		_rows_box.add_child(btn)

		var row := HBoxContainer.new()
		row.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
		row.offset_left = 8.0
		row.offset_right = -8.0
		row.add_theme_constant_override("separation", 10)
		row.mouse_filter = Control.MOUSE_FILTER_IGNORE
		btn.add_child(row)

		var key := Label.new()
		key.text = str(e.get("hotkey", ""))
		key.custom_minimum_size = Vector2(18, 0)
		key.add_theme_font_size_override("font_size", 11)
		key.add_theme_color_override("font_color", N.ACCENT_400 if enabled else N.NEUTRAL_700)
		key.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
		row.add_child(key)

		var label := Label.new()
		label.text = str(e.get("text", ""))
		label.add_theme_font_size_override("font_size", 12)
		label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
		label.clip_text = true
		row.add_child(label)

		var ctxt := str(e.get("ctxt", ""))
		if ctxt != "":
			var meta := Label.new()
			meta.text = ctxt
			meta.add_theme_font_size_override("font_size", 10)
			meta.add_theme_color_override("font_color", N.NEUTRAL_600)
			meta.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
			row.add_child(meta)

		_row_nodes.append(btn)
	_paint_rows()

func _paint_rows() -> void:
	for i in _row_nodes.size():
		if i >= _entries.size():
			break
		var e: Dictionary = _entries[i]
		var on := int(e.get("index", -1)) == _selected
		var enabled := bool(e.get("enabled", true))
		var sb := StyleBoxFlat.new()
		sb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.16) if on else Color(0, 0, 0, 0)
		sb.border_color = N.ACCENT
		sb.border_width_left = 2 if on else 0
		for style in ["normal", "hover", "pressed"]:
			_row_nodes[i].add_theme_stylebox_override(style, sb)
		var label: Label = _row_nodes[i].get_child(0).get_child(1)
		if not enabled:
			label.add_theme_color_override("font_color", N.NEUTRAL_700)
		else:
			label.add_theme_color_override("font_color", N.TEXT if on else N.NEUTRAL_300)

func _update_desc() -> void:
	if not _desc_pane.visible:
		return
	for e in _entries:
		if typeof(e) == TYPE_DICTIONARY and int(e.get("index", -1)) == _selected:
			_desc.text = str(e.get("desc", ""))
			return
	_desc.text = ""

# --- interaction ------------------------------------------------------------

func _set_selected(index: int) -> void:
	if index == _selected:
		return
	_selected = index
	if _host != null and _host.has_method("uilist_select"):
		_host.uilist_select(index)
	_paint_rows()
	_update_desc()

func _activate(index: int, enabled: bool) -> void:
	if not enabled:
		# Matches uilist: a disabled entry can be highlighted but not chosen.
		_set_selected(index)
		return
	if _host != null and _host.has_method("uilist_confirm"):
		_host.uilist_confirm(index)

func _visible_position() -> int:
	for i in _entries.size():
		if int(_entries[i].get("index", -1)) == _selected:
			return i
	return 0

func _move(delta: int) -> void:
	if _entries.is_empty():
		return
	var pos := wrapi(_visible_position() + delta, 0, _entries.size())
	_set_selected(int(_entries[pos].get("index", -1)))
	_scroll_into_view(pos)

func _scroll_into_view(pos: int) -> void:
	if pos < 0 or pos >= _row_nodes.size():
		return
	var scroll := _rows_box.get_parent() as ScrollContainer
	if scroll == null:
		return
	scroll.ensure_control_visible(_row_nodes[pos])

func _unhandled_input(event: InputEvent) -> void:
	if not visible or not (event is InputEventKey) or not event.pressed or event.echo:
		return
	# The filter box keeps its own typing; Escape and the arrows still work.
	var typing: bool = _filter != null and _filter_row.visible and _filter.has_focus()

	match event.keycode:
		KEY_ESCAPE:
			if _host != null and _host.has_method("uilist_cancel"):
				_host.uilist_cancel()
		KEY_UP:
			_move(-1)
		KEY_DOWN:
			_move(1)
		KEY_LEFT:
			if _tab_nodes.size() > 1:
				_select_tab(wrapi(_current_tab - 1, 0, _tab_nodes.size()))
			else:
				return
		KEY_RIGHT:
			if _tab_nodes.size() > 1:
				_select_tab(wrapi(_current_tab + 1, 0, _tab_nodes.size()))
			else:
				return
		KEY_PAGEUP:
			_move(-MAX_VISIBLE)
		KEY_PAGEDOWN:
			_move(MAX_VISIBLE)
		KEY_HOME:
			if not _entries.is_empty():
				_set_selected(int(_entries[0].get("index", -1)))
				_scroll_into_view(0)
		KEY_END:
			if not _entries.is_empty():
				_set_selected(int(_entries[-1].get("index", -1)))
				_scroll_into_view(_entries.size() - 1)
		KEY_ENTER, KEY_KP_ENTER:
			_confirm_selected()
		_:
			if typing:
				return
			# A printable key picks the entry carrying it as a hotkey, which is
			# how these menus are actually driven.
			if event.unicode > 0 and _pick_by_hotkey(char(event.unicode)):
				get_viewport().set_input_as_handled()
			return
	get_viewport().set_input_as_handled()

func _confirm_selected() -> void:
	for e in _entries:
		if typeof(e) == TYPE_DICTIONARY and int(e.get("index", -1)) == _selected:
			_activate(_selected, bool(e.get("enabled", true)))
			return

func _pick_by_hotkey(ch: String) -> bool:
	if ch == "":
		return false
	for e in _entries:
		if typeof(e) != TYPE_DICTIONARY:
			continue
		if str(e.get("hotkey", "")) == ch:
			_activate(int(e.get("index", -1)), bool(e.get("enabled", true)))
			return true
	return false
