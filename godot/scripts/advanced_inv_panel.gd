extends Control
## Advanced inventory manager ("AIM", MENU-8) -- two panes, an area picker,
## and the action bar -- as a Godot Control.
##
## The game thread is blocked in advanced_inventory::run_in_godot() while
## this is up, driving the same process_action() the legacy loop drives --
## this panel only changes where an action string comes from. Both panes are
## always visible (there is no tab; LEFT/RIGHT/TOGGLE_TAB just move which
## pane is the source), so both are rebuilt on every refresh.
##
## See src/godot_advanced_inv_snapshot.h.

const N := preload("res://scripts/nocturne.gd")
const TAGS := preload("res://scripts/color_tags.gd")

var _host: Node
var _generation: int = -1

var _title: Label
var _area_row: HFlowContainer
var _action_row: HFlowContainer
var _pane_cols: Array[VBoxContainer] = []
var _pane_area: Array[Label] = []
var _pane_desc: Array[Label] = []
var _pane_capacity: Array[Label] = []
var _pane_filter: Array[Label] = []
var _pane_sort: Array[Label] = []
var _pane_list: Array[VBoxContainer] = []
var _status: Label

const _ACTIONS := [
	["Move", "MOVE_SINGLE_ITEM"], ["Move stack", "MOVE_ITEM_STACK"],
	["Move all", "MOVE_ALL_ITEMS"], ["Sort", "SORT"], ["Filter", "FILTER"],
	["Reset filter", "RESET_FILTER"], ["Examine", "EXAMINE"],
	["Unload", "UNLOAD_CONTAINER"], ["Favorite", "TOGGLE_FAVORITE"],
	["Auto-pickup", "TOGGLE_AUTO_PICKUP"], ["Vehicle", "TOGGLE_VEH"],
	["Category mode", "CATEGORY_SELECTION"], ["Swap panes", "TOGGLE_TAB"],
]

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if not _pane_cols.is_empty():
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.55)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(dim)

	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -640.0
	frame.offset_right = 640.0
	frame.offset_top = -350.0
	frame.offset_bottom = 350.0
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(N.BG.r, N.BG.g, N.BG.b, 0.99)
	sb.border_color = N.NEUTRAL_800
	sb.set_border_width_all(1)
	sb.border_width_top = 3
	sb.content_margin_left = 20
	sb.content_margin_right = 20
	sb.content_margin_top = 16
	sb.content_margin_bottom = 14
	frame.add_theme_stylebox_override("panel", sb)
	add_child(frame)

	var col := VBoxContainer.new()
	col.add_theme_constant_override("separation", N.SPACE_S)
	frame.add_child(col)

	_title = Label.new()
	_title.text = "ADVANCED INVENTORY MANAGER"
	_title.add_theme_font_size_override("font_size", 16)
	_title.add_theme_color_override("font_color", N.TEXT)
	col.add_child(_title)

	_area_row = HFlowContainer.new()
	_area_row.add_theme_constant_override("h_separation", 4)
	_area_row.add_theme_constant_override("v_separation", 4)
	col.add_child(_area_row)

	_action_row = HFlowContainer.new()
	_action_row.add_theme_constant_override("h_separation", 6)
	_action_row.add_theme_constant_override("v_separation", 4)
	col.add_child(_action_row)
	for spec in _ACTIONS:
		var btn := Button.new()
		btn.text = str(spec[0])
		btn.focus_mode = Control.FOCUS_NONE
		N.apply_button(btn)
		var action := str(spec[1])
		btn.pressed.connect(func() -> void: _act(action))
		_action_row.add_child(btn)

	col.add_child(N.fade_rule())

	var split := HBoxContainer.new()
	split.add_theme_constant_override("separation", N.SPACE_M)
	split.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(split)

	for side in 2:
		var pane_col := VBoxContainer.new()
		pane_col.add_theme_constant_override("separation", 2)
		pane_col.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		split.add_child(pane_col)

		var area_lbl := Label.new()
		area_lbl.add_theme_font_size_override("font_size", 13)
		area_lbl.add_theme_color_override("font_color", N.ACCENT)
		pane_col.add_child(area_lbl)
		_pane_area.append(area_lbl)

		var desc_lbl := Label.new()
		desc_lbl.add_theme_font_size_override("font_size", 11)
		desc_lbl.add_theme_color_override("font_color", N.NEUTRAL_500)
		desc_lbl.autowrap_mode = TextServer.AUTOWRAP_WORD
		pane_col.add_child(desc_lbl)
		_pane_desc.append(desc_lbl)

		var cap_lbl := Label.new()
		cap_lbl.add_theme_font_size_override("font_size", 11)
		cap_lbl.add_theme_color_override("font_color", N.NEUTRAL_400)
		pane_col.add_child(cap_lbl)
		_pane_capacity.append(cap_lbl)

		var meta_row := HBoxContainer.new()
		meta_row.add_theme_constant_override("separation", N.SPACE_S)
		pane_col.add_child(meta_row)
		var sort_lbl := Label.new()
		sort_lbl.add_theme_font_size_override("font_size", 11)
		sort_lbl.add_theme_color_override("font_color", N.NEUTRAL_500)
		meta_row.add_child(sort_lbl)
		_pane_sort.append(sort_lbl)
		var filter_lbl := Label.new()
		filter_lbl.add_theme_font_size_override("font_size", 11)
		filter_lbl.add_theme_color_override("font_color", N.NEUTRAL_500)
		meta_row.add_child(filter_lbl)
		_pane_filter.append(filter_lbl)

		var header_row := HBoxContainer.new()
		header_row.add_theme_constant_override("separation", 4)
		pane_col.add_child(header_row)
		var name_hdr := Label.new()
		name_hdr.text = "Name"
		name_hdr.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		name_hdr.add_theme_font_size_override("font_size", 10)
		name_hdr.add_theme_color_override("font_color", N.NEUTRAL_600)
		header_row.add_child(name_hdr)
		for hdr in ["amt", "weight", "vol"]:
			var col_hdr := Label.new()
			col_hdr.text = hdr
			col_hdr.custom_minimum_size = Vector2(52, 0)
			col_hdr.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
			col_hdr.add_theme_font_size_override("font_size", 10)
			col_hdr.add_theme_color_override("font_color", N.NEUTRAL_600)
			header_row.add_child(col_hdr)

		var scroll := ScrollContainer.new()
		scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
		scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
		pane_col.add_child(scroll)
		var list := VBoxContainer.new()
		list.add_theme_constant_override("separation", 1)
		list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		scroll.add_child(list)
		_pane_list.append(list)

		_pane_cols.append(pane_col)

	col.add_child(N.fade_rule())

	_status = Label.new()
	_status.add_theme_font_size_override("font_size", 11)
	_status.add_theme_color_override("font_color", N.NEUTRAL_600)
	col.add_child(_status)

	col.add_child(N.micro_label(
			"↑↓ select · ←→ swap panes · Enter move · Esc close", N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_advanced_inv_state"):
		return
	if _pane_cols.is_empty():
		_build()
	var gen: int = int(_host.advanced_inv_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_advanced_inv_state()
	var title := str(d.get("title", ""))
	if title != "":
		_title.text = title.to_upper()
	_rebuild_areas(d.get("areas", []))
	_rebuild_pane(0, d.get("left", {}))
	_rebuild_pane(1, d.get("right", {}))
	_status.text = ("Category mode ON" if bool(d.get("category_mode", false))
			else "Category mode off")

func _rebuild_areas(areas: Array) -> void:
	for child in _area_row.get_children():
		_area_row.remove_child(child)
		child.queue_free()
	for entry in areas:
		var ad: Dictionary = entry
		var btn := Button.new()
		btn.text = str(ad.get("key", "?"))
		btn.tooltip_text = str(ad.get("name", ""))
		btn.focus_mode = Control.FOCUS_NONE
		btn.disabled = not bool(ad.get("enabled", false))
		btn.custom_minimum_size = Vector2(36, 0)
		var action := str(ad.get("action", ""))
		btn.pressed.connect(func() -> void: _act(action))
		_area_row.add_child(btn)

func _rebuild_pane(side: int, pd: Dictionary) -> void:
	var active := bool(pd.get("active", false))
	_pane_cols[side].modulate = Color(1, 1, 1, 1) if active else Color(1, 1, 1, 0.7)
	_pane_area[side].text = str(pd.get("area_name", "")).to_upper()
	_pane_desc[side].text = str(pd.get("area_desc", ""))
	var item_count := int(pd.get("item_count", 0))
	var max_count := int(pd.get("max_count", 0))
	var cap := str(pd.get("capacity", ""))
	_pane_capacity[side].text = (cap + "   %d/%d" % [item_count, max_count]) if max_count > 0 else cap
	_pane_sort[side].text = "Sort: %s" % str(pd.get("sort_label", "none"))
	var filter := str(pd.get("filter", ""))
	_pane_filter[side].text = ("Filter: %s" % filter) if filter != "" else ""

	var list := _pane_list[side]
	for child in list.get_children():
		list.remove_child(child)
		child.queue_free()

	var rows: Array = pd.get("rows", [])
	var selected := int(pd.get("selected", -1))
	if rows.is_empty():
		var empty := Label.new()
		empty.text = "(empty)"
		empty.add_theme_color_override("font_color", N.NEUTRAL_600)
		list.add_child(empty)
		return

	for i in rows.size():
		var rd: Dictionary = rows[i]
		var category := str(rd.get("category", ""))
		if category != "":
			var hdr := Label.new()
			hdr.text = category.to_upper()
			hdr.add_theme_font_size_override("font_size", 10)
			hdr.add_theme_color_override("font_color", N.ACCENT_300)
			list.add_child(hdr)

		var idx := i
		var row_side := side
		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", 4)
		var btn := Button.new()
		btn.text = TAGS.strip(str(rd.get("name", "")))
		btn.alignment = HORIZONTAL_ALIGNMENT_LEFT
		btn.clip_text = true
		btn.focus_mode = Control.FOCUS_NONE
		btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		btn.pressed.connect(func() -> void: _select(row_side, idx))
		var on := active and i == selected
		var sb := StyleBoxFlat.new()
		sb.bg_color = Color(N.ACCENT.r, N.ACCENT.g, N.ACCENT.b, 0.12) if on else Color(0, 0, 0, 0)
		sb.border_color = N.ACCENT if on else Color(0, 0, 0, 0)
		sb.set_border_width_all(0)
		sb.border_width_left = 2 if on else 0
		sb.content_margin_left = 6
		sb.content_margin_top = 2
		sb.content_margin_bottom = 2
		for style in ["normal", "hover", "pressed"]:
			btn.add_theme_stylebox_override(style, sb)
		btn.add_theme_color_override("font_color", N.TEXT if on else N.NEUTRAL_400)
		btn.add_theme_font_size_override("font_size", 12)
		row.add_child(btn)
		for field in ["amount", "weight", "volume"]:
			var v := Label.new()
			v.text = str(rd.get(field, ""))
			v.custom_minimum_size = Vector2(52, 0)
			v.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
			v.add_theme_font_size_override("font_size", 11)
			v.add_theme_color_override("font_color", N.TEXT if on else N.NEUTRAL_500)
			row.add_child(v)
		list.add_child(row)

# --- actions ------------------------------------------------------------------

func _act(action: String) -> void:
	if _host != null and _host.has_method("advanced_inv_action"):
		_host.advanced_inv_action(action)

func _select(side: int, index: int) -> void:
	if _host != null and _host.has_method("advanced_inv_select"):
		_host.advanced_inv_select(side, index)

## While a prompt or menu raised on top is up (its own Godot panel), the keys
## belong to it, not to this screen.
func _yield_to_overlays() -> bool:
	if _host == null:
		return false
	if _host.has_method("popup_active") and _host.popup_active():
		return true
	if _host.has_method("uilist_active") and _host.uilist_active():
		return true
	if _host.has_method("textwin_active") and _host.textwin_active():
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
		KEY_LEFT:
			_act("LEFT")
		KEY_RIGHT:
			_act("RIGHT")
		KEY_UP:
			_act("UP")
		KEY_DOWN:
			_act("DOWN")
		KEY_ENTER, KEY_KP_ENTER:
			_act("MOVE_SINGLE_ITEM")
		KEY_TAB:
			_act("TOGGLE_TAB")
		KEY_PAGEUP:
			_act("PAGE_UP")
		KEY_PAGEDOWN:
			_act("PAGE_DOWN")
		KEY_HOME:
			_act("HOME")
		KEY_END:
			_act("END")
		_:
			return
	get_viewport().set_input_as_handled()
