extends Control
## The scores screen — achievements, conducts, scores, kills — as a Godot Control.
##
## The game thread is blocked in scores_ui_impl::run_in_godot while this is up,
## and that loop still owns every action: the tab, the two collapsible kill
## groups, and closing. This panel replaces where the action comes from and
## nothing else. Scrolling is local — the content never changes while the screen
## is open — but the tab and the group collapse round-trip through C++, because
## selected_tab is the member the BACKLOG lift created and the probe reads back.
##
## See src/godot_scores_snapshot.h.

const N := preload("res://scripts/nocturne.gd")
const TAGS := preload("res://scripts/color_tags.gd")

var _host: Node
var _generation: int = -1
var _tab: int = 0

var _title: Label
var _tab_row: HBoxContainer
var _tab_nodes: Array[Button] = []
var _scroll: ScrollContainer
var _body: VBoxContainer
var _status: Label

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _body != null:
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.55)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(dim)

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

	_title = Label.new()
	_title.text = "YOUR SCORES"
	_title.add_theme_font_size_override("font_size", 16)
	_title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(_title)

	_tab_row = HBoxContainer.new()
	_tab_row.add_theme_constant_override("separation", 3)
	col.add_child(_tab_row)

	col.add_child(N.fade_rule())

	_scroll = ScrollContainer.new()
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(_scroll)
	_body = VBoxContainer.new()
	_body.add_theme_constant_override("separation", N.SPACE_S)
	_body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_scroll.add_child(_body)

	col.add_child(N.fade_rule())

	_status = Label.new()
	_status.add_theme_font_size_override("font_size", 11)
	_status.add_theme_color_override("font_color", N.NEUTRAL_600)
	col.add_child(_status)

	col.add_child(N.micro_label("←→ tabs · ↑↓ scroll · Esc close", N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_scores_state"):
		return
	if _body == null:
		_build()
	var gen: int = int(_host.scores_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_scores_state()
	var title := str(d.get("title", ""))
	if title != "":
		_title.text = title.to_upper()
	_tab = int(d.get("tab", 0))
	_rebuild_tabs(d.get("tabs", []))
	_rebuild_body(d)

func _rebuild_tabs(tabs: Array) -> void:
	for child in _tab_row.get_children():
		_tab_row.remove_child(child)
		child.queue_free()
	_tab_nodes.clear()
	for i in tabs.size():
		var idx := i
		var btn := Button.new()
		btn.text = str(tabs[i])
		btn.focus_mode = Control.FOCUS_NONE
		btn.pressed.connect(func() -> void: _select_tab(idx))
		var on := i == _tab
		var sb := StyleBoxFlat.new()
		sb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.15) if on else Color(0, 0, 0, 0)
		sb.border_color = N.ACCENT if on else N.NEUTRAL_800
		sb.set_border_width_all(1)
		sb.border_width_bottom = 2 if on else 1
		sb.content_margin_left = 12
		sb.content_margin_right = 12
		sb.content_margin_top = 5
		sb.content_margin_bottom = 5
		for style in ["normal", "hover", "pressed"]:
			btn.add_theme_stylebox_override(style, sb)
		btn.add_theme_color_override("font_color", N.TEXT if on else N.NEUTRAL_500)
		btn.add_theme_font_size_override("font_size", 11)
		_tab_row.add_child(btn)
		_tab_nodes.append(btn)

func _rebuild_body(d: Dictionary) -> void:
	for child in _body.get_children():
		_body.remove_child(child)
		child.queue_free()
	_scroll.scroll_vertical = 0
	match _tab:
		0:
			_fill_text_tab(d.get("achievements", {}))
		1:
			_fill_text_tab(d.get("conducts", {}))
		2:
			_fill_text_tab(d.get("scores", {}))
		3:
			_fill_kills_tab(d.get("kills", {}))
	_status.text = "KILLS %d" % int(d.get("kills", {}).get("total", 0))

func _rich_line(bbcode: String) -> RichTextLabel:
	var rtl := RichTextLabel.new()
	rtl.bbcode_enabled = true
	rtl.fit_content = true
	rtl.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	rtl.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rtl.add_theme_color_override("default_color", N.NEUTRAL_300)
	rtl.add_theme_font_size_override("normal_font_size", 13)
	rtl.text = bbcode
	return rtl

func _fill_text_tab(t: Dictionary) -> void:
	var rows: Array = t.get("rows", [])
	if rows.is_empty():
		var empty := str(t.get("empty", ""))
		if empty != "":
			_body.add_child(_rich_line(TAGS.to_bbcode(empty)))
	else:
		for i in rows.size():
			_body.add_child(_rich_line(TAGS.to_bbcode(str(rows[i]))))
			if i < rows.size() - 1:
				_body.add_child(N.divider())
	var note := str(t.get("note", ""))
	if note != "":
		var l := _rich_line(TAGS.to_bbcode(note))
		l.add_theme_color_override("default_color", N.NEUTRAL_600)
		l.add_theme_font_size_override("normal_font_size", 11)
		_body.add_child(l)

func _group_header(text: String, collapsed: bool, action: String) -> Button:
	var btn := Button.new()
	btn.text = ("▸ " if collapsed else "▾ ") + text
	btn.focus_mode = Control.FOCUS_NONE
	btn.alignment = HORIZONTAL_ALIGNMENT_LEFT
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(N.SURFACE.r, N.SURFACE.g, N.SURFACE.b, 0.9)
	sb.border_color = N.NEUTRAL_800
	sb.set_border_width_all(1)
	sb.content_margin_left = 10
	sb.content_margin_right = 10
	sb.content_margin_top = 4
	sb.content_margin_bottom = 4
	for style in ["normal", "hover", "pressed"]:
		btn.add_theme_stylebox_override(style, sb)
	btn.add_theme_color_override("font_color", N.NEUTRAL_200)
	btn.add_theme_font_size_override("font_size", 12)
	btn.pressed.connect(func() -> void: _act(action))
	return btn

## The count and symbol columns are monospaced in the terminal; here the count is
## right-padded into a fixed column and the symbol keeps the monster's colour,
## which is what actually identifies it.
func _kill_line(count: int, symbol: String, color_name: String, name: String) -> RichTextLabel:
	var hex := TAGS.hex_for_name(color_name)
	var sym := symbol.replace("[", "[lb]")
	var who := name.replace("[", "[lb]")
	var sym_bb := ("[color=%s]%s[/color]" % [hex, sym]) if hex != "" else sym
	return _rich_line("[color=#75798c]%5d[/color] %s %s" % [count, sym_bb, who])

func _fill_kills_tab(k: Dictionary) -> void:
	var monster_collapsed := bool(k.get("monster_collapsed", false))
	_body.add_child(_group_header(str(k.get("monster_header", "")), monster_collapsed,
		"TOGGLE_MONSTER_GROUP"))
	if not monster_collapsed:
		var mrows: Array = k.get("monster_rows", [])
		if mrows.is_empty():
			_body.add_child(_rich_line(TAGS.to_bbcode(str(k.get("monster_empty", "")))))
		else:
			for row in mrows:
				_body.add_child(_kill_line(int(row.get("count", 0)), str(row.get("symbol", "")),
					str(row.get("color", "")), str(row.get("name", ""))))

	var npc_collapsed := bool(k.get("npc_collapsed", false))
	_body.add_child(_group_header(str(k.get("npc_header", "")), npc_collapsed,
		"TOGGLE_NPC_GROUP"))
	if not npc_collapsed:
		var nrows: Array = k.get("npc_rows", [])
		if nrows.is_empty():
			_body.add_child(_rich_line(TAGS.to_bbcode(str(k.get("npc_empty", "")))))
		else:
			for npc_name in nrows:
				_body.add_child(_kill_line(1, "@", "magenta", str(npc_name)))

# --- requests -----------------------------------------------------------------

func _act(action: String) -> void:
	if _host != null and _host.has_method("scores_action"):
		_host.scores_action(action)

func _select_tab(index: int) -> void:
	if _host != null and _host.has_method("scores_select_tab"):
		_host.scores_select_tab(index)

## While a prompt or menu raised on top is up (its own Godot panel), the keys
## belong to it, not to this screen.
func _yield_to_overlays() -> bool:
	if _host == null:
		return false
	if _host.has_method("popup_active") and _host.popup_active():
		return true
	if _host.has_method("uilist_active") and _host.uilist_active():
		return true
	return false

func _unhandled_input(event: InputEvent) -> void:
	if not visible or not (event is InputEventKey) or not event.pressed or event.echo:
		return
	if _yield_to_overlays():
		return
	match event.keycode:
		KEY_ESCAPE:
			_act("QUIT")
		KEY_TAB:
			_act("PREV_TAB" if event.shift_pressed else "NEXT_TAB")
		KEY_LEFT:
			_act("PREV_TAB")
		KEY_RIGHT:
			_act("NEXT_TAB")
		KEY_M:
			_act("TOGGLE_MONSTER_GROUP")
		KEY_N:
			_act("TOGGLE_NPC_GROUP")
		KEY_UP:
			_scroll.scroll_vertical -= 40
		KEY_DOWN:
			_scroll.scroll_vertical += 40
		KEY_PAGEUP:
			_scroll.scroll_vertical -= int(_scroll.size.y)
		KEY_PAGEDOWN:
			_scroll.scroll_vertical += int(_scroll.size.y)
		KEY_HOME:
			_scroll.scroll_vertical = 0
		KEY_END:
			_scroll.scroll_vertical = int(_body.size.y)
		_:
			return
	get_viewport().set_input_as_handled()
