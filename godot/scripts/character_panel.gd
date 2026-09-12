extends Control
## Character stats overlay (stats, skills, traits, bionics).

signal closed

var _host: Node
var _tabs: TabContainer
var _stats: RichTextLabel
var _skills: ItemList
var _traits: ItemList
var _bionics: ItemList
var _detail: RichTextLabel
var _title: Label
var _trait_rows: Array = []
var _bionic_rows: Array = []

func setup(host: Node) -> void:
	_host = host
	mouse_filter = Control.MOUSE_FILTER_STOP
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build()

func _build() -> void:
	if _tabs != null:
		return
	var dim := ColorRect.new()
	dim.color = Color(0, 0, 0, 0.45)
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(dim)

	var panel := PanelContainer.new()
	panel.set_anchors_preset(Control.PRESET_CENTER)
	panel.offset_left = -440.0
	panel.offset_top = -300.0
	panel.offset_right = 440.0
	panel.offset_bottom = 300.0
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.08, 0.09, 0.11, 0.96)
	sb.border_color = Color(0.4, 0.45, 0.55, 1)
	sb.set_border_width_all(2)
	sb.set_content_margin_all(12)
	panel.add_theme_stylebox_override("panel", sb)
	add_child(panel)

	var vbox := VBoxContainer.new()
	panel.add_child(vbox)

	var top := HBoxContainer.new()
	vbox.add_child(top)
	_title = Label.new()
	_title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_title.add_theme_font_size_override("font_size", 20)
	top.add_child(_title)
	var close_btn := Button.new()
	close_btn.text = "Close (Esc / @)"
	close_btn.pressed.connect(func() -> void: closed.emit())
	top.add_child(close_btn)

	var body := HBoxContainer.new()
	body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	vbox.add_child(body)

	_tabs = TabContainer.new()
	_tabs.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	body.add_child(_tabs)

	_stats = RichTextLabel.new()
	_stats.name = "Stats"
	_stats.bbcode_enabled = true
	_tabs.add_child(_stats)

	_skills = ItemList.new()
	_skills.name = "Skills"
	_tabs.add_child(_skills)

	_traits = ItemList.new()
	_traits.name = "Traits"
	_traits.item_selected.connect(_on_trait_selected)
	_tabs.add_child(_traits)

	_bionics = ItemList.new()
	_bionics.name = "Bionics"
	_bionics.item_selected.connect(_on_bionic_selected)
	_tabs.add_child(_bionics)

	_detail = RichTextLabel.new()
	_detail.custom_minimum_size = Vector2(260, 0)
	_detail.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	body.add_child(_detail)

func refresh() -> void:
	if _host == null or not _host.has_method("get_character_sheet"):
		return
	if _tabs == null:
		_build()
	var d: Dictionary = _host.get_character_sheet()
	_title.text = str(d.get("name", "Character"))
	var lines: PackedStringArray = PackedStringArray()
	lines.append("[b]Attributes[/b]")
	lines.append("STR %d   DEX %d   INT %d   PER %d" % [
		int(d.get("str", 0)), int(d.get("dex", 0)), int(d.get("int", 0)), int(d.get("per", 0))
	])
	lines.append("")
	lines.append("[b]Body[/b]")
	for limb in d.get("limbs", []):
		if typeof(limb) != TYPE_DICTIONARY:
			continue
		lines.append("%s  %d / %d" % [
			str(limb.get("name", "")), int(limb.get("hp", 0)), int(limb.get("hp_max", 0))
		])
	lines.append("")
	lines.append("[b]Status[/b]")
	for row in d.get("status", []):
		if typeof(row) != TYPE_DICTIONARY:
			continue
		lines.append("%s: %s" % [str(row.get("name", "")), str(row.get("detail", ""))])
	_stats.text = "\n".join(lines)

	_skills.clear()
	for sk in d.get("skills", []):
		if typeof(sk) != TYPE_DICTIONARY:
			continue
		_skills.add_item("%s  %d  (know %d)" % [
			str(sk.get("name", "")), int(sk.get("level", 0)), int(sk.get("knowledge", 0))
		])

	_trait_rows = d.get("traits", [])
	_traits.clear()
	for row in _trait_rows:
		if typeof(row) != TYPE_DICTIONARY:
			continue
		_traits.add_item(str(row.get("name", "")))

	_bionic_rows = d.get("bionics", [])
	_bionics.clear()
	for row in _bionic_rows:
		if typeof(row) != TYPE_DICTIONARY:
			continue
		_bionics.add_item(str(row.get("name", "")))

	_detail.text = "Select a trait or bionic for details."

func _on_trait_selected(index: int) -> void:
	if index < 0 or index >= _trait_rows.size():
		return
	var row: Dictionary = _trait_rows[index]
	_detail.text = "%s\n\n%s" % [str(row.get("name", "")), str(row.get("detail", ""))]

func _on_bionic_selected(index: int) -> void:
	if index < 0 or index >= _bionic_rows.size():
		return
	var row: Dictionary = _bionic_rows[index]
	_detail.text = "%s\n\n%s" % [str(row.get("name", "")), str(row.get("detail", ""))]

func _unhandled_input(event: InputEvent) -> void:
	if not visible:
		return
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_ESCAPE or event.keycode == KEY_AT or event.unicode == 64:
			closed.emit()
			get_viewport().set_input_as_handled()
