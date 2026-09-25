extends Control
## World picker for custom character creation.

signal world_chosen(world_name: String)
signal cancelled

@onready var world_list: ItemList = %WorldPickList
@onready var name_edit: LineEdit = %WorldNameEdit
@onready var status_label: Label = %WorldPickStatus

var _host: Node

func setup(host: Node) -> void:
	_host = host
	refresh()

func refresh() -> void:
	world_list.clear()
	status_label.text = ""
	if _host == null:
		return
	var worlds: PackedStringArray = _host.list_worlds()
	for w in worlds:
		var label := String(w)
		if _host.world_has_saves(w):
			label += " (has saves)"
		world_list.add_item(label)
		world_list.set_item_metadata(world_list.item_count - 1, w)

func _selected_world() -> String:
	var idxs := world_list.get_selected_items()
	if idxs.is_empty():
		return ""
	return String(world_list.get_item_metadata(idxs[0]))

func _on_continue_pressed() -> void:
	var world := _selected_world()
	if world.is_empty():
		status_label.text = "Select a world first."
		return
	if _host.world_has_saves(world):
		var dlg := ConfirmationDialog.new()
		dlg.dialog_text = "Many game features will not work correctly with multiple characters in the same world. Create a new character anyway?"
		add_child(dlg)
		dlg.confirmed.connect(func() -> void:
			dlg.queue_free()
			world_chosen.emit(world)
		)
		dlg.canceled.connect(func() -> void: dlg.queue_free())
		dlg.popup_centered()
		return
	world_chosen.emit(world)

func _on_create_pressed() -> void:
	if _host == null:
		return
	var name := name_edit.text.strip_edges()
	var result: Dictionary = _host.create_world_default(name)
	if not result.get("ok", false):
		status_label.text = String(result.get("error", "Failed to create world."))
		return
	var created := String(result.get("value", ""))
	refresh()
	# Select the new world.
	for i in world_list.item_count:
		if String(world_list.get_item_metadata(i)) == created:
			world_list.select(i)
			break
	status_label.text = "Created world: %s" % created

func _on_back_pressed() -> void:
	cancelled.emit()
