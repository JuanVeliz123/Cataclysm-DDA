extends Control
## NPC conversation, as a Godot Control.
##
## The game thread is blocked inside dialogue::opt_imgui while this is up. That
## loop still owns everything it owned before — which responses are really
## selectable, the "you may be attacked" confirmation, hiding itself when the
## trade window opens — and this panel replaces exactly one thing: where its next
## action comes from. So the keys below are the DIALOGUE input context's own
## action strings, and clicking a line is sent as an index meaning "this one,
## confirmed", which the game applies as a selection plus CONFIRM so it goes
## through the same re-verification a keyboard CONFIRM does.
##
## See src/godot_dialogue_snapshot.h.

const N := preload("res://scripts/nocturne.gd")
const TAGS := preload("res://scripts/color_tags.gd")

var _host: Node
var _generation: int = -1
var _responses: Array = []
var _selected: int = 0

var _speaker: Label
var _history: VBoxContainer
var _history_scroll: ScrollContainer
var _list: VBoxContainer
var _list_scroll: ScrollContainer
var _row_nodes: Array[PanelContainer] = []

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _history != null:
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.6)
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

	_speaker = Label.new()
	_speaker.add_theme_font_size_override("font_size", 17)
	_speaker.add_theme_color_override("font_color", N.TEXT)
	_speaker.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	col.add_child(_speaker)

	col.add_child(N.fade_rule())

	# History above, responses below, as in the terminal layout. History takes
	# the slack: a long conversation is worth more room than a short reply list.
	_history_scroll = ScrollContainer.new()
	_history_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_history_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(_history_scroll)
	_history = VBoxContainer.new()
	_history.add_theme_constant_override("separation", 6)
	_history.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_history_scroll.add_child(_history)

	col.add_child(N.fade_rule())

	_list_scroll = ScrollContainer.new()
	_list_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_list_scroll.custom_minimum_size = Vector2(0, 200)
	col.add_child(_list_scroll)
	_list = VBoxContainer.new()
	_list.add_theme_constant_override("separation", 2)
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_list_scroll.add_child(_list)

	col.add_child(N.micro_label(
		"↑↓ choose · Enter say · click to say · PgUp/PgDn scroll · Esc leave",
		N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_dialogue_state"):
		return
	if _history == null:
		_build()
	var gen: int = int(_host.dialogue_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_dialogue_state()
	var who := str(d.get("header", ""))
	_speaker.text = who if who != "" else "CONVERSATION"
	_selected = int(d.get("selected", 0))
	_rebuild_history(d.get("history", []))
	_rebuild_responses(d.get("responses", []))

func _rebuild_history(lines: Array) -> void:
	for child in _history.get_children():
		_history.remove_child(child)
		child.queue_free()
	for entry in lines:
		var body := RichTextLabel.new()
		body.bbcode_enabled = true
		body.fit_content = true
		body.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		body.add_theme_font_size_override("normal_font_size", 13)
		body.add_theme_color_override("default_color", _colour_of(entry, N.NEUTRAL_300))
		body.text = TAGS.to_bbcode(str(entry.get("text", "")))
		_history.add_child(body)
	# A conversation reads bottom-up: the newest line is the one being answered.
	await get_tree().process_frame
	if is_instance_valid(_history_scroll):
		_history_scroll.scroll_vertical = int(_history.size.y)

func _rebuild_responses(list: Array) -> void:
	for child in _list.get_children():
		_list.remove_child(child)
		child.queue_free()
	_row_nodes.clear()
	_responses = list

	for i in list.size():
		var entry: Dictionary = list[i]
		var idx := i
		var holder := PanelContainer.new()
		holder.custom_minimum_size = Vector2(0, 24)
		holder.gui_input.connect(func(event: InputEvent) -> void:
			if event is InputEventMouseButton and event.pressed \
					and event.button_index == MOUSE_BUTTON_LEFT:
				_say(idx))
		_list.add_child(holder)

		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", N.SPACE_M)
		holder.add_child(row)

		var key := Label.new()
		key.text = str(entry.get("hotkey", ""))
		key.add_theme_font_size_override("font_size", 11)
		key.add_theme_color_override("font_color", N.NEUTRAL_600)
		key.custom_minimum_size = Vector2(26, 0)
		key.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
		row.add_child(key)

		var text := Label.new()
		text.text = TAGS.strip(str(entry.get("text", "")))
		text.add_theme_font_size_override("font_size", 13)
		text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		text.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		row.add_child(text)

		holder.set_meta("label", text)
		# The response colour carries meaning here -- a trial response, a hostile
		# one and an ordinary one are different colours in the terminal.
		holder.set_meta("base_color", _colour_of(entry, N.NEUTRAL_200))
		_row_nodes.append(holder)
	_highlight()

func _colour_of(entry: Dictionary, fallback: Color) -> Color:
	var hex := TAGS.hex_for_name(str(entry.get("color", "")))
	return Color(hex) if hex != "" else fallback

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
	if _host != null and _host.has_method("dialogue_action"):
		_host.dialogue_action(action)

## Say a specific line. Sent as an index rather than as "move then confirm" so
## the game applies both in one step and cannot act on a half-applied selection.
func _say(index: int) -> void:
	if index < 0 or index >= _responses.size():
		return
	_selected = index
	_highlight()
	if _host != null and _host.has_method("dialogue_select"):
		_host.dialogue_select(index)

## Moving the cursor is answered locally as well as sent, so the highlight tracks
## the key rather than waiting a round trip for the game to publish it back.
func _move(delta: int) -> void:
	if _responses.is_empty():
		return
	_selected = clampi(_selected + delta, 0, _responses.size() - 1)
	_highlight()
	_act("DOWN" if delta > 0 else "UP")

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
		KEY_ENTER, KEY_KP_ENTER:
			_say(_selected)
		KEY_PAGEUP:
			_act("PAGE_UP")
		KEY_PAGEDOWN:
			_act("PAGE_DOWN")
		KEY_HOME:
			_act("HOME")
		KEY_END:
			_act("END")
		_:
			# Responses carry their own hotkey, shown in the left column. Match it
			# so the conversation stays answerable the way players expect.
			if event.unicode > 0:
				var ch := String.chr(event.unicode)
				for i in _responses.size():
					if str(_responses[i].get("hotkey", "")) == ch:
						_say(i)
						get_viewport().set_input_as_handled()
						return
			return
	get_viewport().set_input_as_handled()
