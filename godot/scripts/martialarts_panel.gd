extends Control
## The martial-arts style picker, as a Godot Control.
##
## The legacy screen is a uilist whose row 0 is the keep-hands-free toggle, plus
## a separate ImGui window the player opens per style for techniques, buffs and
## compatible weapons. Here the two are one screen: the list on the left, and
## the full details of whatever the cursor is on as a permanent pane on the
## right. The game thread is blocked inside pick_style() while this is up; the
## panel answers with row indices and the same action strings the uilist took,
## and the game republishes after every applied action so the highlight and the
## detail pane track the cursor.
##
## Selecting a row applies it and closes, exactly as the uilist did: a style row
## activates that style, the toggle row flips keep-hands-free.
##
## See src/godot_martialarts_snapshot.h.

const N := preload("res://scripts/nocturne.gd")
const TAGS := preload("res://scripts/color_tags.gd")

var _host: Node
var _generation: int = -1
var _rows: Array = []
var _selected: int = 0

var _title: Label
var _subtitle: RichTextLabel
var _list: VBoxContainer
var _list_scroll: ScrollContainer
var _detail: VBoxContainer
var _detail_scroll: ScrollContainer
var _row_nodes: Array[PanelContainer] = []

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

	# List and detail side by side; the detail pane carries long buff and weapon
	# paragraphs, so it gets the slack.
	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -480.0
	frame.offset_right = 480.0
	frame.offset_top = -310.0
	frame.offset_bottom = 310.0
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

	_title = Label.new()
	_title.text = "MARTIAL ARTS"
	_title.add_theme_font_size_override("font_size", 17)
	_title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(_title)

	# The stat line ("STR: 8, DEX: 9, …") arrives colour-tagged.
	_subtitle = RichTextLabel.new()
	_subtitle.bbcode_enabled = true
	_subtitle.fit_content = true
	_subtitle.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_subtitle.add_theme_font_size_override("normal_font_size", 12)
	_subtitle.add_theme_color_override("default_color", N.NEUTRAL_500)
	col.add_child(_subtitle)

	col.add_child(N.fade_rule())

	var panes := HBoxContainer.new()
	panes.add_theme_constant_override("separation", N.SPACE_L)
	panes.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(panes)

	_list_scroll = ScrollContainer.new()
	_list_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_list_scroll.custom_minimum_size = Vector2(300, 0)
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

	col.add_child(N.micro_label(
		"↑↓ choose · Enter activate · h hands-free · double-click activate · Esc close",
		N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_martialarts_state"):
		return
	if _list == null:
		_build()
	var gen: int = int(_host.martialarts_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_martialarts_state()
	var title := TAGS.strip(str(d.get("title", "")))
	_title.text = title.to_upper() if title != "" else "MARTIAL ARTS"
	_subtitle.text = TAGS.to_bbcode(str(d.get("subtitle", "")))
	_selected = int(d.get("selection", 0))
	_rebuild_rows(d.get("rows", []))
	_rebuild_detail(d.get("detail", []))

func _rebuild_rows(list: Array) -> void:
	for child in _list.get_children():
		_list.remove_child(child)
		child.queue_free()
	_row_nodes.clear()
	_rows = list

	for i in list.size():
		var entry: Dictionary = list[i]
		var idx := i
		var holder := PanelContainer.new()
		holder.custom_minimum_size = Vector2(0, 24)
		holder.gui_input.connect(func(event: InputEvent) -> void:
			if event is InputEventMouseButton and event.pressed \
					and event.button_index == MOUSE_BUTTON_LEFT:
				if event.double_click:
					_activate(idx)
				else:
					_move_to(idx))
		_list.add_child(holder)

		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", N.SPACE_S)
		holder.add_child(row)

		var name_label := Label.new()
		name_label.text = TAGS.strip(str(entry.get("name", "")))
		name_label.add_theme_font_size_override("font_size", 13)
		name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		name_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
		name_label.clip_text = true
		row.add_child(name_label)

		var base := N.NEUTRAL_200
		if bool(entry.get("toggle", false)):
			base = N.NEUTRAL_400
		if bool(entry.get("active", false)):
			base = N.ACCENT_300
			row.add_child(N.chip("ACTIVE", "good"))

		holder.set_meta("label", name_label)
		holder.set_meta("base_color", base)
		_row_nodes.append(holder)
	_highlight()

func _rebuild_detail(lines: Array) -> void:
	for child in _detail.get_children():
		_detail.remove_child(child)
		child.queue_free()
	_detail_scroll.scroll_vertical = 0
	for entry in lines:
		var text := str(entry.get("text", ""))
		if bool(entry.get("header", false)):
			var head := Label.new()
			head.text = TAGS.strip(text).to_upper()
			head.add_theme_font_size_override("font_size", 12)
			head.add_theme_color_override("font_color", N.ACCENT_300)
			head.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
			_detail.add_child(head)
			continue
		# Body lines carry CDDA colour tags -- the same text the ImGui details
		# window renders, formatted by the same C++ (replace_colors et al).
		var body := RichTextLabel.new()
		body.bbcode_enabled = true
		body.fit_content = true
		body.text = TAGS.to_bbcode(text)
		body.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		body.add_theme_color_override("default_color", N.NEUTRAL_300)
		body.add_theme_font_size_override("normal_font_size", 12)
		_detail.add_child(body)

func _highlight() -> void:
	for i in _row_nodes.size():
		var on := i == _selected
		var sb := StyleBoxFlat.new()
		sb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.16) if on else Color(0, 0, 0, 0)
		sb.border_color = N.ACCENT
		sb.border_width_left = 2 if on else 0
		sb.content_margin_left = 8
		sb.content_margin_right = 6
		sb.content_margin_top = 3
		sb.content_margin_bottom = 3
		_row_nodes[i].add_theme_stylebox_override("panel", sb)
		var label: Label = _row_nodes[i].get_meta("label")
		label.add_theme_color_override("font_color",
			N.TEXT if on else _row_nodes[i].get_meta("base_color"))
	if _selected >= 0 and _selected < _row_nodes.size():
		_ensure_visible(_row_nodes[_selected])

func _ensure_visible(node: Control) -> void:
	if _list_scroll == null or node == null:
		return
	var top := node.position.y
	var bottom := top + node.size.y
	if top < _list_scroll.scroll_vertical:
		_list_scroll.scroll_vertical = int(top)
	elif bottom > _list_scroll.scroll_vertical + _list_scroll.size.y:
		_list_scroll.scroll_vertical = int(bottom - _list_scroll.size.y)

# --- requests -----------------------------------------------------------------

func _act(action: String) -> void:
	if _host != null and _host.has_method("martialarts_action"):
		_host.martialarts_action(action)

## Activate a row: cursor moved and confirmed in one step, so the game cannot
## act on a half-applied selection. This closes the screen, as the uilist did.
func _activate(index: int) -> void:
	if index < 0 or index >= _rows.size():
		return
	_selected = index
	_highlight()
	if _host != null and _host.has_method("martialarts_select"):
		_host.martialarts_select(index)

## A single click only moves the cursor, so a style can be read before it is
## committed to. The game republishes with the new detail pane.
func _move_to(index: int) -> void:
	if index < 0 or index >= _rows.size():
		return
	_selected = index
	_highlight()
	if _host != null and _host.has_method("martialarts_move_to"):
		_host.martialarts_move_to(index)

## Moving the cursor is answered locally as well as sent, so the highlight tracks
## the key rather than waiting a round trip. Clamped, not wrapped, and the game
## clamps the same way -- the two must agree or the highlight would jump.
func _move(delta: int) -> void:
	if _rows.is_empty():
		return
	_selected = clampi(_selected + delta, 0, _rows.size() - 1)
	_highlight()
	var action := "UP" if delta < 0 else "DOWN"
	if absi(delta) > 1:
		action = "PAGE_UP" if delta < 0 else "PAGE_DOWN"
	_act(action)

func _unhandled_input(event: InputEvent) -> void:
	if not visible or not (event is InputEventKey) or not event.pressed or event.echo:
		return
	match event.keycode:
		KEY_ESCAPE:
			_act("QUIT")
		KEY_UP:
			_move(-1)
		KEY_DOWN:
			_move(1)
		KEY_PAGEUP:
			_move(-10)
		KEY_PAGEDOWN:
			_move(10)
		KEY_HOME:
			_act("HOME")
		KEY_END:
			_act("END")
		KEY_ENTER, KEY_KP_ENTER:
			_activate(_selected)
		KEY_H:
			# The uilist's 'h' hotkey: flip keep-hands-free (row 0) and close.
			_activate(0)
		_:
			# Modal: any other key is swallowed rather than reaching the game
			# (or a dialogue underneath, when an NPC's style is being picked).
			pass
	get_viewport().set_input_as_handled()
