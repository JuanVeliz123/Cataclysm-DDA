extends Control
## Read-only text windows: item info, the extended description of a tile, and
## anything else that is formatted text you scroll and dismiss.
##
## One panel serves all of them — they differ in how the text is produced, not in
## how they behave. Tabs exist because the extended description has four
## (creature / furniture / terrain / vehicle); the ordinary case is one unnamed
## tab, and then no tab strip is drawn.
##
## The game thread is blocked in the caller while this is up; see
## src/godot_textwin_snapshot.h.

const N := preload("res://scripts/nocturne.gd")

var _host: Node
var _title: Label
var _subtitle: Label
var _tab_row: HBoxContainer
var _tab_nodes: Array[Button] = []
var _body: RichTextLabel
var _scroll: ScrollContainer
var _generation: int = -1
var _current: int = 0
var _tab_count: int = 0

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _body != null:
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.5)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(dim)

	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -350.0
	frame.offset_right = 350.0
	frame.offset_top = -270.0
	frame.offset_bottom = 270.0
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
	_title.add_theme_font_size_override("font_size", 17)
	_title.add_theme_color_override("font_color", N.TEXT)
	_title.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	col.add_child(_title)

	_subtitle = Label.new()
	_subtitle.add_theme_font_size_override("font_size", 11)
	_subtitle.add_theme_color_override("font_color", N.NEUTRAL_500)
	col.add_child(_subtitle)

	_tab_row = HBoxContainer.new()
	_tab_row.add_theme_constant_override("separation", 4)
	col.add_child(_tab_row)

	col.add_child(N.fade_rule())

	_scroll = ScrollContainer.new()
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(_scroll)
	_body = RichTextLabel.new()
	_body.bbcode_enabled = false
	_body.fit_content = true
	_body.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_body.add_theme_color_override("default_color", N.NEUTRAL_300)
	_body.add_theme_font_size_override("normal_font_size", 13)
	_scroll.add_child(_body)

	col.add_child(N.fade_rule())
	col.add_child(N.micro_label("↑↓ scroll · ←→ tabs · Esc close", N.NEUTRAL_700))

# --- update -----------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_textwin_state"):
		return
	if _body == null:
		_build()
	var gen: int = int(_host.textwin_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_textwin_state()
	var title := str(d.get("title", ""))
	_title.text = title
	_title.visible = title != ""
	var sub := str(d.get("subtitle", ""))
	_subtitle.text = sub
	_subtitle.visible = sub != ""

	var tabs: Array = d.get("tabs", [])
	_tab_count = tabs.size()
	_current = clampi(int(d.get("current", 0)), 0, maxi(0, _tab_count - 1))
	_rebuild_tabs(tabs)
	if _current < tabs.size():
		_body.text = str(tabs[_current].get("body", ""))
		_scroll.scroll_vertical = 0

## Only draw a strip when there is more than one tab, and only when they are
## named — a single unnamed tab is the ordinary case and needs no chrome.
func _rebuild_tabs(tabs: Array) -> void:
	for child in _tab_row.get_children():
		_tab_row.remove_child(child)
		child.queue_free()
	_tab_nodes.clear()
	if tabs.size() < 2:
		_tab_row.visible = false
		return
	_tab_row.visible = true
	for i in tabs.size():
		var idx := i
		var label := str(tabs[i].get("label", ""))
		if label == "":
			label = "%d" % (i + 1)
		var btn := Button.new()
		btn.text = label
		btn.focus_mode = Control.FOCUS_NONE
		btn.pressed.connect(func() -> void: _select(idx))
		_tab_row.add_child(btn)
		_tab_nodes.append(btn)
	_paint_tabs()

func _paint_tabs() -> void:
	for i in _tab_nodes.size():
		var on := i == _current
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
			_tab_nodes[i].add_theme_stylebox_override(style, sb)
		_tab_nodes[i].add_theme_color_override("font_color",
			N.TEXT if on else N.NEUTRAL_500)
		_tab_nodes[i].add_theme_font_size_override("font_size", 11)

func _select(index: int) -> void:
	if index == _current or _host == null or not _host.has_method("textwin_select_tab"):
		return
	_current = index
	_paint_tabs()
	_host.textwin_select_tab(index)

func _dismiss() -> void:
	if _host != null and _host.has_method("textwin_dismiss"):
		_host.textwin_dismiss()

func _unhandled_input(event: InputEvent) -> void:
	if not visible or not (event is InputEventKey) or not event.pressed or event.echo:
		return
	match event.keycode:
		KEY_ESCAPE, KEY_ENTER, KEY_KP_ENTER:
			_dismiss()
		KEY_LEFT:
			if _tab_count > 1:
				_select(wrapi(_current - 1, 0, _tab_count))
		KEY_RIGHT:
			if _tab_count > 1:
				_select(wrapi(_current + 1, 0, _tab_count))
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
