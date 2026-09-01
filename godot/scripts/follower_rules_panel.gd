extends Control
## Follower rules (MENU-13's last screen): a follower's boolean rules plus a
## few radio-button groups (engagement, aim, and -- if they have bionics --
## CBM recharge/reserve), as a Godot Control.
##
## The game thread is blocked in follower_rules_ui_impl::run_in_godot while
## this is up. Every rule is addressed by its own stable id (an `ally_rule`
## flag value for a checkbox, an enum's own int value for a radio option) --
## never by row position -- the same rule every other MENU-13 panel follows.
##
## IMPORT/EXPORT (copying rules to/from another follower) is not in this
## panel; see BACKLOG.md.
##
## See src/godot_follower_rules_snapshot.h.

const N := preload("res://scripts/nocturne.gd")

var _host: Node
var _generation: int = -1

var _title: Label
var _rule_list: VBoxContainer
var _group_list: VBoxContainer

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _rule_list != null:
		return

	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.55)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(dim)

	var frame := PanelContainer.new()
	frame.set_anchors_preset(Control.PRESET_CENTER)
	frame.offset_left = -420.0
	frame.offset_right = 420.0
	frame.offset_top = -320.0
	frame.offset_bottom = 320.0
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

	var top_row := HBoxContainer.new()
	top_row.add_theme_constant_override("separation", N.SPACE_S)
	col.add_child(top_row)
	_title = Label.new()
	_title.text = "FOLLOWER RULES"
	_title.add_theme_font_size_override("font_size", 16)
	_title.add_theme_color_override("font_color", N.TEXT)
	_title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	top_row.add_child(_title)
	var default_all := Button.new()
	default_all.text = "Default ALL"
	default_all.focus_mode = Control.FOCUS_NONE
	N.apply_button(default_all)
	default_all.pressed.connect(func() -> void: _act("DEFAULT_ALL"))
	top_row.add_child(default_all)

	col.add_child(N.fade_rule())

	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	col.add_child(scroll)
	var scroll_col := VBoxContainer.new()
	scroll_col.add_theme_constant_override("separation", N.SPACE_S)
	scroll_col.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(scroll_col)

	_rule_list = VBoxContainer.new()
	_rule_list.add_theme_constant_override("separation", 2)
	_rule_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll_col.add_child(_rule_list)

	scroll_col.add_child(N.fade_rule())

	_group_list = VBoxContainer.new()
	_group_list.add_theme_constant_override("separation", N.SPACE_S)
	_group_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll_col.add_child(_group_list)

	col.add_child(N.fade_rule())
	col.add_child(N.micro_label("Esc close · click a box or option to change it", N.NEUTRAL_700))

# --- update -------------------------------------------------------------------

func refresh() -> void:
	if _host == null or not _host.has_method("get_follower_rules_state"):
		return
	if _rule_list == null:
		_build()
	var gen: int = int(_host.follower_rules_generation())
	if gen == _generation:
		return
	_generation = gen

	var d: Dictionary = _host.get_follower_rules_state()
	var title := str(d.get("title", ""))
	if title != "":
		_title.text = title.to_upper()

	_rebuild_rules(d.get("rules", []))
	_rebuild_groups(d.get("groups", []))

func _rebuild_rules(rules: Array) -> void:
	for child in _rule_list.get_children():
		_rule_list.remove_child(child)
		child.queue_free()
	for rule in rules:
		var rd: Dictionary = rule
		var flag := int(rd.get("flag", 0))
		var line := HBoxContainer.new()
		line.add_theme_constant_override("separation", 6)

		var hotkey := str(rd.get("hotkey", ""))
		var hk := Label.new()
		hk.text = hotkey
		hk.custom_minimum_size = Vector2(16, 0)
		hk.add_theme_font_size_override("font_size", 11)
		hk.add_theme_color_override("font_color", N.GOOD)
		line.add_child(hk)

		var box := CheckBox.new()
		box.text = str(rd.get("label", ""))
		box.button_pressed = bool(rd.get("enabled", false))
		box.focus_mode = Control.FOCUS_NONE
		box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		box.add_theme_color_override("font_color", N.TEXT)
		box.toggled.connect(func(_pressed: bool) -> void: _toggle(flag))
		line.add_child(box)

		var default_btn := Button.new()
		default_btn.text = "Default"
		default_btn.focus_mode = Control.FOCUS_NONE
		N.apply_button(default_btn)
		default_btn.pressed.connect(func() -> void: _default_rule(flag))
		line.add_child(default_btn)

		_rule_list.add_child(line)

func _rebuild_groups(groups: Array) -> void:
	for child in _group_list.get_children():
		_group_list.remove_child(child)
		child.queue_free()
	for group in groups:
		var gd: Dictionary = group
		var id := str(gd.get("id", ""))
		var current := int(gd.get("current", 0))

		var header := HBoxContainer.new()
		header.add_theme_constant_override("separation", 6)
		var hk := Label.new()
		hk.text = str(gd.get("hotkey", ""))
		hk.custom_minimum_size = Vector2(16, 0)
		hk.add_theme_font_size_override("font_size", 11)
		hk.add_theme_color_override("font_color", N.GOOD)
		header.add_child(hk)
		var title_lbl := Label.new()
		title_lbl.text = str(gd.get("title", ""))
		title_lbl.add_theme_color_override("font_color", N.TEXT)
		header.add_child(title_lbl)
		_group_list.add_child(header)

		var options: Array = gd.get("options", [])
		var btn_group := ButtonGroup.new()
		for option in options:
			var od: Dictionary = option
			var value := int(od.get("value", 0))
			var radio := CheckBox.new()
			radio.text = str(od.get("label", ""))
			radio.button_group = btn_group
			radio.button_pressed = value == current
			radio.focus_mode = Control.FOCUS_NONE
			radio.add_theme_color_override("font_color", N.TEXT)
			radio.toggled.connect(func(pressed: bool) -> void:
				if pressed:
					_set_group(id, value))
			_group_list.add_child(radio)

# --- actions ------------------------------------------------------------------

func _act(action: String) -> void:
	if _host != null and _host.has_method("follower_rules_action"):
		_host.follower_rules_action(action)

func _toggle(flag: int) -> void:
	if _host != null and _host.has_method("follower_rules_toggle"):
		_host.follower_rules_toggle(flag)

func _default_rule(flag: int) -> void:
	if _host != null and _host.has_method("follower_rules_default_rule"):
		_host.follower_rules_default_rule(flag)

func _set_group(group: String, value: int) -> void:
	if _host != null and _host.has_method("follower_rules_set"):
		_host.follower_rules_set(group, value)

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
