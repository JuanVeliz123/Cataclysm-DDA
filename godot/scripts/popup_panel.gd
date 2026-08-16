extends Control
## query_popup as a Godot Control — every yes/no prompt, and every notice.
##
## Two shapes, because they are the same C++ class (src/godot_popup_snapshot.h):
##
##   Prompt — the game thread is blocked in query_popup::query() waiting for a
##            button. Modal, takes the keyboard.
##   Notice — display only, no input, on screen while a static_popup lives.
##            "Saving game, this may take a while." is one of these, which is why
##            it must never steal focus or block anything.
##
## Both can be up at once (a prompt raised while a notice shows), so they are
## separate boxes rather than one that switches mode.

const N := preload("res://scripts/nocturne.gd")

var _host: Node

var _prompt_root: Control
var _prompt_text: Label
var _buttons_row: HBoxContainer
var _button_nodes: Array[Button] = []
var _entry_title: Label
var _entry_label: Label
var _entry_field: LineEdit
var _entry_row: Control
var _text_entry := false
var _selected: int = 0
var _allow_cancel: bool = false

var _notice_root: Control
var _notice_text: Label

var _generation: int = -1

func setup(host: Node) -> void:
	_host = host
	# IGNORE at the root: a bare notice must not swallow clicks meant for the
	# game. The prompt's own dim layer takes input back when it appears.
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _prompt_root != null:
		return

	# --- notice: a quiet strip, top-centre, never modal ----------------------
	# Centred by a container, not by anchors. PRESET_CENTER_TOP anchors both
	# edges to 0.5 and leaves the offsets at zero, so the panel is zero pixels
	# wide -- the label then wraps to roughly one character per line and the
	# notice renders as a tall thin ribbon down the screen.
	var notice_band := CenterContainer.new()
	notice_band.set_anchors_preset(Control.PRESET_TOP_WIDE)
	notice_band.offset_top = 24.0
	notice_band.offset_bottom = 200.0
	notice_band.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(notice_band)

	_notice_root = PanelContainer.new()
	_notice_root.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_notice_root.visible = false
	var nsb := StyleBoxFlat.new()
	nsb.bg_color = Color(N.SURFACE.r, N.SURFACE.g, N.SURFACE.b, 0.96)
	nsb.border_color = N.ACCENT_700
	nsb.set_border_width_all(1)
	nsb.border_width_left = 2
	nsb.content_margin_left = 18
	nsb.content_margin_right = 18
	nsb.content_margin_top = 11
	nsb.content_margin_bottom = 11
	_notice_root.add_theme_stylebox_override("panel", nsb)
	notice_band.add_child(_notice_root)
	_notice_text = Label.new()
	_notice_text.add_theme_font_size_override("font_size", 13)
	_notice_text.add_theme_color_override("font_color", N.NEUTRAL_200)
	_notice_text.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_notice_text.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	# Wide enough not to wrap a short prompt, bounded so a long one does.
	_notice_text.custom_minimum_size = Vector2(260, 0)
	_notice_text.size_flags_horizontal = Control.SIZE_SHRINK_CENTER
	_notice_root.add_child(_notice_text)

	# --- prompt: modal ------------------------------------------------------
	_prompt_root = Control.new()
	_prompt_root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_prompt_root.mouse_filter = Control.MOUSE_FILTER_STOP
	_prompt_root.visible = false
	add_child(_prompt_root)

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.45)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	_prompt_root.add_child(dim)

	var box := PanelContainer.new()
	box.set_anchors_preset(Control.PRESET_CENTER)
	box.offset_left = -260.0
	box.offset_right = 260.0
	box.offset_top = -110.0
	box.offset_bottom = 110.0
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(N.BG.r, N.BG.g, N.BG.b, 0.99)
	sb.border_color = N.NEUTRAL_800
	sb.set_border_width_all(1)
	sb.border_width_top = 3
	sb.content_margin_left = 26
	sb.content_margin_right = 26
	sb.content_margin_top = 24
	sb.content_margin_bottom = 20
	box.add_theme_stylebox_override("panel", sb)
	_prompt_root.add_child(box)

	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", N.SPACE_L)
	box.add_child(col)

	_entry_title = Label.new()
	_entry_title.add_theme_font_size_override("font_size", 16)
	_entry_title.add_theme_color_override("font_color", N.TEXT)
	_entry_title.visible = false
	col.add_child(_entry_title)

	_prompt_text = Label.new()
	_prompt_text.add_theme_font_size_override("font_size", 15)
	_prompt_text.add_theme_color_override("font_color", N.TEXT)
	_prompt_text.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_prompt_text.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(_prompt_text)

	# Text-entry variant of the same modal slot.
	_entry_row = HBoxContainer.new()
	_entry_row.add_theme_constant_override("separation", N.SPACE_M)
	_entry_row.visible = false
	col.add_child(_entry_row)
	_entry_label = Label.new()
	_entry_label.add_theme_font_size_override("font_size", 12)
	_entry_label.add_theme_color_override("font_color", N.ACCENT_300)
	_entry_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_entry_row.add_child(_entry_label)
	_entry_field = LineEdit.new()
	_entry_field.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_entry_field.add_theme_color_override("font_color", N.TEXT)
	var fsb := StyleBoxFlat.new()
	fsb.bg_color = Color(0.03, 0.035, 0.055)
	fsb.border_color = N.ACCENT_700
	fsb.set_border_width_all(1)
	fsb.content_margin_left = 10
	fsb.content_margin_right = 10
	fsb.content_margin_top = 7
	fsb.content_margin_bottom = 7
	_entry_field.add_theme_stylebox_override("normal", fsb)
	_entry_field.add_theme_stylebox_override("focus", fsb)
	_entry_field.text_submitted.connect(func(t: String) -> void: _submit_text(t))
	_entry_row.add_child(_entry_field)

	col.add_child(N.fade_rule())

	_buttons_row = HBoxContainer.new()
	_buttons_row.alignment = BoxContainer.ALIGNMENT_END
	_buttons_row.add_theme_constant_override("separation", N.SPACE_M)
	col.add_child(_buttons_row)

# --- update -----------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_popup_state"):
		return
	if _prompt_root == null:
		_build()
	var gen: int = int(_host.popup_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_popup_state()

	_notice_root.visible = bool(d.get("notice_active", false))
	if _notice_root.visible:
		_notice_text.text = str(d.get("notice", ""))

	var was_open := _prompt_root.visible
	var open := bool(d.get("prompt_active", false))
	_prompt_root.visible = open
	if not open:
		return

	_prompt_text.text = str(d.get("text", ""))
	_allow_cancel = bool(d.get("allow_cancel", false))
	_text_entry = bool(d.get("text_entry", false))

	_entry_row.visible = _text_entry
	_entry_title.visible = _text_entry and str(d.get("entry_title", "")) != ""
	_entry_title.text = str(d.get("entry_title", ""))
	_prompt_text.visible = str(d.get("text", "")) != ""
	if _text_entry:
		_entry_label.text = str(d.get("entry_label", ""))
		_entry_label.visible = _entry_label.text != ""
		var maxlen := int(d.get("entry_max_length", 0))
		_entry_field.max_length = maxlen if maxlen > 0 else 0
		if not was_open:
			_entry_field.text = str(d.get("entry_initial", ""))
			_entry_field.grab_focus()
			_entry_field.caret_column = _entry_field.text.length()

	var options: Array = d.get("options", [])
	_rebuild_buttons(options)
	if not was_open:
		# A fresh prompt starts on its first option rather than wherever the last
		# one happened to leave the highlight.
		_selected = 0
	_paint_buttons()

func _rebuild_buttons(options: Array) -> void:
	for child in _buttons_row.get_children():
		_buttons_row.remove_child(child)
		child.queue_free()
	_button_nodes.clear()
	for i in options.size():
		var idx := i
		var btn := Button.new()
		btn.text = str(options[i])
		btn.focus_mode = Control.FOCUS_NONE
		N.apply_button(btn)
		btn.pressed.connect(func() -> void: _answer(idx))
		btn.mouse_entered.connect(func() -> void:
			_selected = idx
			_paint_buttons())
		_buttons_row.add_child(btn)
		_button_nodes.append(btn)
	_selected = clampi(_selected, 0, maxi(0, _button_nodes.size() - 1))

func _paint_buttons() -> void:
	for i in _button_nodes.size():
		var on := i == _selected
		var sb := N.button_style(on)
		_button_nodes[i].add_theme_stylebox_override("normal", sb)
		_button_nodes[i].add_theme_color_override("font_color",
			N.TEXT if on else N.ACCENT_300)

func _submit_text(text: String) -> void:
	if _host != null and _host.has_method("popup_answer_text"):
		_host.popup_answer_text(text)

func _answer(index: int) -> void:
	if _host != null and _host.has_method("popup_answer"):
		_host.popup_answer(index)

func _cancel() -> void:
	if _host != null and _host.has_method("popup_cancel"):
		_host.popup_cancel()

## True while a modal prompt is up, so the host knows to stop routing keys to
## the game. A notice on its own is not modal and must not report true.
func prompt_open() -> bool:
	return _prompt_root != null and _prompt_root.visible

func _unhandled_input(event: InputEvent) -> void:
	if not prompt_open() or not (event is InputEventKey) or not event.pressed or event.echo:
		return
	if _text_entry:
		# The field owns every other key, including plain letters -- this is a
		# text box, so nothing here may treat typing as a shortcut.
		if event.keycode == KEY_ESCAPE:
			_cancel()
			get_viewport().set_input_as_handled()
		elif event.keycode == KEY_ENTER or event.keycode == KEY_KP_ENTER:
			_submit_text(_entry_field.text)
			get_viewport().set_input_as_handled()
		return
	match event.keycode:
		KEY_LEFT:
			_selected = wrapi(_selected - 1, 0, maxi(1, _button_nodes.size()))
			_paint_buttons()
		KEY_RIGHT:
			_selected = wrapi(_selected + 1, 0, maxi(1, _button_nodes.size()))
			_paint_buttons()
		KEY_ENTER, KEY_KP_ENTER, KEY_SPACE:
			_answer(_selected)
		KEY_ESCAPE:
			if _allow_cancel:
				_cancel()
			else:
				return
		_:
			# The option labels carry their own key, e.g. "(Y)es" — match it so
			# these prompts stay answerable the way players expect.
			if event.unicode > 0 and _answer_by_letter(char(event.unicode)):
				get_viewport().set_input_as_handled()
			return
	get_viewport().set_input_as_handled()

func _answer_by_letter(ch: String) -> bool:
	if ch.strip_edges() == "":
		return false
	var needle := ch.to_lower()
	for i in _button_nodes.size():
		var label := _button_nodes[i].text.to_lower()
		# Labels mark their key with brackets, and which bracket varies by
		# option and locale -- "[Y]es", "(Y)es". Take the first bracketed letter,
		# and fall back to the first letter of the label.
		var marked := ""
		for j in label.length():
			if label[j] == "[" or label[j] == "(":
				if j + 1 < label.length():
					marked = label[j + 1]
				break
		if marked != "":
			if marked == needle:
				_answer(i)
				return true
		elif label.begins_with(needle):
			_answer(i)
			return true
	return false
