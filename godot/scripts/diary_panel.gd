extends Control
## The diary screen (`diary::show_diary_ui`) as a Godot Control.
##
## The legacy screen cycles keyboard focus across three panes (pages /
## changes / text) with LEFT/RIGHT. This panel drops that: a click on a page
## or a change row addresses it directly, the same simplified
## re-presentation the martial-arts and advanced-inventory panels made. The
## game thread is blocked inside `run_in_godot` while this is up.
##
## "Edit text" and "View scores" both leave for a screen this channel is not:
## the text editor is still a raw curses window, and scores has its own
## channel. Both suspend this one first (`suspended` on the C++ side, which
## also hides this panel), so no key here fights the screen underneath.
##
## See src/godot_diary_snapshot.h.

const N := preload("res://scripts/nocturne.gd")

var _host: Node
var _generation: int = -1

var _title: Label
var _head: Label
var _pages_list: VBoxContainer
var _changes_list: VBoxContainer
var _text_label: RichTextLabel
var _hint: Label
var _edit_button: Button
var _current_page: int = -1
var _selected_change: int = -1

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _pages_list != null:
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.55)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(dim)

	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -520.0
	frame.offset_right = 520.0
	frame.offset_top = -330.0
	frame.offset_bottom = 330.0
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
	_title.text = "DIARY"
	_title.add_theme_font_size_override("font_size", 16)
	_title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(_title)

	_head = Label.new()
	_head.add_theme_font_size_override("font_size", 12)
	_head.add_theme_color_override("font_color", N.NEUTRAL_500)
	col.add_child(_head)

	col.add_child(N.fade_rule())

	var body := HBoxContainer.new()
	body.add_theme_constant_override("separation", N.SPACE_S)
	body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(body)

	# Pages, narrow column on the left.
	var pages_col := VBoxContainer.new()
	pages_col.custom_minimum_size = Vector2(180, 0)
	body.add_child(pages_col)
	pages_col.add_child(N.micro_label("Pages", N.NEUTRAL_600))
	var pages_scroll := ScrollContainer.new()
	pages_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	pages_col.add_child(pages_scroll)
	_pages_list = VBoxContainer.new()
	_pages_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	pages_scroll.add_child(_pages_list)

	body.add_child(VSeparator.new())

	# Changes, narrow column in the middle.
	var changes_col := VBoxContainer.new()
	changes_col.custom_minimum_size = Vector2(240, 0)
	body.add_child(changes_col)
	changes_col.add_child(N.micro_label("Changes", N.NEUTRAL_600))
	var changes_scroll := ScrollContainer.new()
	changes_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	changes_col.add_child(changes_scroll)
	_changes_list = VBoxContainer.new()
	_changes_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	changes_scroll.add_child(_changes_list)

	body.add_child(VSeparator.new())

	# Text, the wide column on the right.
	var text_col := VBoxContainer.new()
	text_col.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	body.add_child(text_col)
	text_col.add_child(N.micro_label("Text", N.NEUTRAL_600))
	var text_scroll := ScrollContainer.new()
	text_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	text_col.add_child(text_scroll)
	_text_label = RichTextLabel.new()
	_text_label.bbcode_enabled = false
	_text_label.fit_content = true
	_text_label.scroll_active = false
	_text_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_text_label.add_theme_color_override("default_color", N.TEXT)
	text_scroll.add_child(_text_label)

	col.add_child(N.fade_rule())

	var button_row := HBoxContainer.new()
	button_row.add_theme_constant_override("separation", N.SPACE_S)
	col.add_child(button_row)
	for spec in [["New Page", "NEW_PAGE"], ["Delete Page", "DELETE_PAGE"],
			["Export Diary", "EXPORT_DIARY"], ["View Scores", "VIEW_SCORES"]]:
		var btn := Button.new()
		btn.text = str(spec[0])
		btn.focus_mode = Control.FOCUS_NONE
		N.apply_button(btn)
		var action := str(spec[1])
		btn.pressed.connect(func() -> void: _act(action))
		button_row.add_child(btn)

	_edit_button = Button.new()
	_edit_button.text = "Edit Text"
	_edit_button.focus_mode = Control.FOCUS_NONE
	N.apply_button(_edit_button)
	_edit_button.pressed.connect(func() -> void: _act("EDIT_TEXT"))
	button_row.add_child(_edit_button)

	_hint = Label.new()
	_hint.add_theme_font_size_override("font_size", 11)
	_hint.add_theme_color_override("font_color", N.NEUTRAL_700)
	col.add_child(_hint)

	col.add_child(N.micro_label("Esc close · click a page or change to select it", N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_diary_state"):
		return
	if _pages_list == null:
		_build()
	var gen: int = int(_host.diary_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_diary_state()
	var title := str(d.get("title", ""))
	if title != "":
		_title.text = title.to_upper()
	_head.text = str(d.get("head_text", ""))
	_hint.text = str(d.get("hint", ""))

	_current_page = int(d.get("current_page", -1))
	_selected_change = int(d.get("selected_change", -1))
	var is_summary: bool = bool(d.get("is_summary", false))
	_edit_button.disabled = is_summary or int(d.get("current_page", -1)) < 0

	_rebuild_pages(d.get("pages", []))
	_rebuild_changes(d.get("changes", []))

	if is_summary:
		var changes: Array = d.get("changes", [])
		var desc := ""
		if _selected_change >= 0 and _selected_change < changes.size():
			desc = str((changes[_selected_change] as Dictionary).get("desc", ""))
		_text_label.text = desc
	else:
		_text_label.text = str(d.get("page_text", ""))

func _rebuild_pages(pages: Array) -> void:
	for child in _pages_list.get_children():
		_pages_list.remove_child(child)
		child.queue_free()
	for i in pages.size():
		var page_index := i
		var btn := Button.new()
		btn.text = str(pages[i])
		btn.flat = true
		btn.alignment = HORIZONTAL_ALIGNMENT_LEFT
		btn.focus_mode = Control.FOCUS_NONE
		btn.add_theme_color_override("font_color",
				N.TEXT if page_index == _current_page else N.NEUTRAL_600)
		if page_index == _current_page:
			btn.add_theme_color_override("font_color_hover", N.TEXT)
		btn.pressed.connect(func() -> void: _select_page(page_index))
		_pages_list.add_child(btn)

func _rebuild_changes(changes: Array) -> void:
	for child in _changes_list.get_children():
		_changes_list.remove_child(child)
		child.queue_free()
	for i in changes.size():
		var change_index := i
		var cd: Dictionary = changes[i]
		var btn := Button.new()
		var text := str(cd.get("text", ""))
		btn.text = text if text != "" else " "
		btn.flat = true
		btn.alignment = HORIZONTAL_ALIGNMENT_LEFT
		btn.focus_mode = Control.FOCUS_NONE
		btn.add_theme_color_override("font_color",
				N.TEXT if change_index == _selected_change else N.NEUTRAL_600)
		btn.pressed.connect(func() -> void: _select_change(change_index))
		_changes_list.add_child(btn)

# --- actions ------------------------------------------------------------------

func _act(action: String) -> void:
	if _host != null and _host.has_method("diary_action"):
		_host.diary_action(action)

func _select_page(index: int) -> void:
	if _host != null and _host.has_method("diary_select_page"):
		_host.diary_select_page(index)

func _select_change(index: int) -> void:
	if _host != null and _host.has_method("diary_select_change"):
		_host.diary_select_change(index)

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
	if event.keycode == KEY_ESCAPE:
		_act("QUIT")
		get_viewport().set_input_as_handled()
