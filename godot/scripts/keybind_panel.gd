extends Control
## The keybindings screen, as a Godot Control.
##
## The game thread is blocked in input_context::display_menu() while this is up.
## It published the action list and is waiting: the panel asks for a change, the
## game applies it to the real input_manager and republishes, and the panel shows
## what came back.
##
## Adding a binding is the one operation that does not fit that shape, because the
## answer is a keypress rather than a choice. It has to reach the game as the
## exact `input_event` the game will later compare against, so the panel does not
## describe the key: while the "New key for X" notice is up, it forwards raw Godot
## events to push_input_event and lets the input bridge translate them, which is
## the same path a key takes during normal play. See
## godot_backend::run_anykey_popup_in_godot.
##
## Filtering happens entirely here — the panel has every row already.

const N := preload("res://scripts/nocturne.gd")

## Mirrors KeybindSnapshot::operation.
const OP_REMOVE := 0
const OP_RESET := 1
const OP_ADD_LOCAL := 2
const OP_ADD_GLOBAL := 3
const OP_EXECUTE := 4

## Mirrors KeybindSnapshot::row::scope.
const SCOPE_GLOBAL := 0
const SCOPE_LOCAL := 1
const SCOPE_UNBOUND := 2

var _host: Node
var _generation: int = -1
var _rows: Array = []
var _shown: Array = []
var _row_nodes: Array[PanelContainer] = []
var _selected: int = 0
## Remembered across republishes, so applying a change does not move the cursor.
var _selected_id: String = ""
var _filter: String = ""
var _permit_execute: bool = false

var _title: Label
var _filter_label: Label
var _list: VBoxContainer
var _scroll: ScrollContainer
var _capture_note: Label
var _capturing: bool = false

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

	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -400.0
	frame.offset_right = 400.0
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
	_title.text = "KEYBINDINGS"
	_title.add_theme_font_size_override("font_size", 17)
	_title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(_title)

	_filter_label = Label.new()
	_filter_label.add_theme_font_size_override("font_size", 11)
	_filter_label.add_theme_color_override("font_color", N.ACCENT_300)
	col.add_child(_filter_label)

	col.add_child(N.fade_rule())

	_scroll = ScrollContainer.new()
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(_scroll)
	_list = VBoxContainer.new()
	_list.add_theme_constant_override("separation", 2)
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_scroll.add_child(_list)

	col.add_child(N.fade_rule())

	_capture_note = Label.new()
	_capture_note.add_theme_font_size_override("font_size", 12)
	_capture_note.add_theme_color_override("font_color", N.WARN)
	_capture_note.custom_minimum_size = Vector2(0, 18)
	col.add_child(_capture_note)

	col.add_child(N.micro_label(
		"↑↓ move · A bind · G bind globally · R remove · D reset · / filter · Esc close",
		N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_keybind_state"):
		return
	if _list == null:
		_build()

	# A notice while this panel is open means the game is waiting for a key to
	# bind. It is the only time the panel gives the keyboard away.
	var was_capturing := _capturing
	_capturing = _host.has_method("popup_active") and _host.popup_active()
	if _capturing != was_capturing:
		_capture_note.text = "PRESS A KEY TO BIND, OR ESC TO CANCEL" if _capturing else ""

	var gen: int = int(_host.keybind_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_keybind_state()
	_rows = d.get("rows", [])
	_permit_execute = bool(d.get("permit_execute", false))
	_title.text = "KEYBINDINGS — %s" % str(d.get("context", "")).to_upper()
	_rebuild()

func _rebuild() -> void:
	for child in _list.get_children():
		_list.remove_child(child)
		child.queue_free()
	_row_nodes.clear()
	_shown.clear()

	var needle := _filter.to_lower()
	for r in _rows:
		if needle != "" and str(r.get("name", "")).to_lower().find(needle) < 0:
			continue
		_shown.append(r)
	_filter_label.text = ("FILTER: %s  (%d of %d)" % [_filter, _shown.size(), _rows.size()]
		if _filter != "" else "%d actions" % _rows.size())

	# Restore the cursor by identity, not by row number: applying a change
	# republishes the list, and an index would drift under the player.
	var restored := -1
	for i in _shown.size():
		_add_row(_shown[i])
		if str(_shown[i].get("action_id", "")) == _selected_id:
			restored = i
	_selected = restored if restored >= 0 else clampi(_selected, 0, maxi(0, _shown.size() - 1))
	_highlight()

func _add_row(entry: Dictionary) -> void:
	var holder := PanelContainer.new()
	holder.custom_minimum_size = Vector2(0, 24)
	_list.add_child(holder)

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", N.SPACE_M)
	holder.add_child(row)

	# One glyph, always present so the columns line up: * marks a binding the
	# player has changed from the shipped default.
	var mark := Label.new()
	mark.text = "*" if bool(entry.get("customized", false)) else " "
	mark.add_theme_font_size_override("font_size", 12)
	mark.add_theme_color_override("font_color", N.ACCENT)
	mark.custom_minimum_size = Vector2(10, 0)
	row.add_child(mark)

	var name_label := Label.new()
	name_label.text = str(entry.get("name", ""))
	name_label.add_theme_font_size_override("font_size", 12)
	name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	name_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	name_label.clip_text = true
	row.add_child(name_label)

	var keys := Label.new()
	var scope := int(entry.get("scope", SCOPE_GLOBAL))
	keys.text = str(entry.get("keys", "")) if scope != SCOPE_UNBOUND else "unbound"
	keys.add_theme_font_size_override("font_size", 12)
	# The colours carry the same meaning as the curses screen: green for a
	# binding local to this context, grey for the global default, red for none.
	keys.add_theme_color_override("font_color",
		N.GOOD if scope == SCOPE_LOCAL else (N.BAD if scope == SCOPE_UNBOUND else N.NEUTRAL_400))
	keys.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	keys.custom_minimum_size = Vector2(230, 0)
	keys.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	keys.clip_text = true
	row.add_child(keys)

	holder.set_meta("name_label", name_label)
	_row_nodes.append(holder)

func _highlight() -> void:
	for i in _row_nodes.size():
		var on := i == _selected
		var sb := StyleBoxFlat.new()
		sb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.14) if on else Color(0, 0, 0, 0)
		sb.border_color = N.ACCENT
		sb.border_width_left = 2 if on else 0
		sb.content_margin_left = 8
		sb.content_margin_right = 6
		_row_nodes[i].add_theme_stylebox_override("panel", sb)
		var label: Label = _row_nodes[i].get_meta("name_label")
		label.add_theme_color_override("font_color", N.TEXT if on else N.NEUTRAL_300)
	if _selected >= 0 and _selected < _shown.size():
		_selected_id = str(_shown[_selected].get("action_id", ""))
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

func _request(op: int) -> void:
	if _selected < 0 or _selected >= _shown.size():
		return
	if _host != null and _host.has_method("keybind_request"):
		_host.keybind_request(str(_shown[_selected].get("action_id", "")), op)

func _dismiss() -> void:
	if _host != null and _host.has_method("keybind_dismiss"):
		_host.keybind_dismiss()

func _move(delta: int) -> void:
	if _shown.is_empty():
		return
	_selected = wrapi(_selected + delta, 0, _shown.size())
	_highlight()

func _set_filter(text: String) -> void:
	_filter = text
	_rebuild()

# --- input --------------------------------------------------------------------

func _unhandled_input(event: InputEvent) -> void:
	if not visible or not (event is InputEventKey) or not event.pressed or event.echo:
		return

	# While the game is asking for a key, every key belongs to it -- including the
	# ones this panel would otherwise treat as commands. Escape included: the
	# waiting popup is what has to see it, or the prompt could never be cancelled.
	if _capturing:
		if _host != null and _host.has_method("push_input_event"):
			_host.push_input_event(event)
		get_viewport().set_input_as_handled()
		return

	# Typing into the filter takes priority over the letter commands.
	if _filter != "" or event.keycode == KEY_SLASH:
		if event.keycode == KEY_SLASH and _filter == "":
			_set_filter(" ")
			get_viewport().set_input_as_handled()
			return
		if event.keycode == KEY_BACKSPACE:
			_set_filter(_filter.substr(0, maxi(0, _filter.length() - 1)))
			get_viewport().set_input_as_handled()
			return
		if event.keycode == KEY_ESCAPE:
			_set_filter("")
			get_viewport().set_input_as_handled()
			return
		if event.keycode == KEY_ENTER or event.keycode == KEY_KP_ENTER:
			# Keep the filter, hand the keyboard back to the list.
			if _filter.strip_edges() == "":
				_set_filter("")
			get_viewport().set_input_as_handled()
			return
		if event.unicode >= 32:
			_set_filter((_filter if _filter != " " else "") + String.chr(event.unicode))
			get_viewport().set_input_as_handled()
			return

	match event.keycode:
		KEY_ESCAPE:
			_dismiss()
		KEY_UP:
			_move(-1)
		KEY_DOWN:
			_move(1)
		KEY_PAGEUP:
			_move(-10)
		KEY_PAGEDOWN:
			_move(10)
		KEY_HOME:
			_selected = 0
			_highlight()
		KEY_END:
			_selected = maxi(0, _shown.size() - 1)
			_highlight()
		KEY_A, KEY_ENTER, KEY_KP_ENTER:
			_request(OP_ADD_LOCAL)
		KEY_G:
			_request(OP_ADD_GLOBAL)
		KEY_R:
			_request(OP_REMOVE)
		KEY_D:
			_request(OP_RESET)
		KEY_X:
			if _permit_execute:
				_request(OP_EXECUTE)
		_:
			return
	get_viewport().set_input_as_handled()
