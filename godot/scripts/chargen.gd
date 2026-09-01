extends Control
## Godot custom character creation UI (seven tabs + identity chrome).

signal confirmed
signal cancelled

const TABS := [
	"Scenario", "Profession", "Background", "Stats", "Traits", "Skills", "Summary"
]

@onready var tabs: TabContainer = %ChargenTabs
@onready var status_label: Label = %ChargenStatus
@onready var name_edit: LineEdit = %ChargenName
@onready var gender_btn: Button = %ChargenGender
@onready var outfit_btn: Button = %ChargenOutfit
@onready var age_spin: SpinBox = %ChargenAge
@onready var height_spin: SpinBox = %ChargenHeight
@onready var blood_btn: Button = %ChargenBlood
@onready var rating_label: Label = %ChargenRating
@onready var summary_label: Label = %ChargenScenarioProf
@onready var detail_label: RichTextLabel = %ChargenDetail
@onready var scenario_list: ItemList = %ScenarioList
@onready var profession_list: ItemList = %ProfessionList
@onready var hobby_list: ItemList = %HobbyList
@onready var trait_list: ItemList = %TraitList
@onready var skill_list: ItemList = %SkillList
@onready var location_list: ItemList = %LocationList
@onready var city_list: ItemList = %CityList
@onready var trait_filter: OptionButton = %TraitFilter
@onready var str_spin: SpinBox = %StatStr
@onready var dex_spin: SpinBox = %StatDex
@onready var int_spin: SpinBox = %StatInt
@onready var per_spin: SpinBox = %StatPer
@onready var skill_level_spin: SpinBox = %SkillLevelSpin
@onready var summary_text: RichTextLabel = %SummaryText
@onready var city_row: HBoxContainer = %CityRow
@onready var confirm_btn: Button = %ChargenConfirm
@onready var template_name_edit: LineEdit = %TemplateNameEdit

var _host: Node
var _refreshing := false
var _scenarios: Array = []
var _professions: Array = []
var _hobbies: Array = []
var _traits: Array = []
var _skills: Array = []
var _locations: Array = []
var _cities: Array = []
var _state: Dictionary = {}
var _selected_skill_id: String = ""

func setup(host: Node) -> void:
	_host = host
	_ensure_tab_titles()
	if trait_filter.item_count == 0:
		for label in ["All", "Positive", "Negative", "Neutral", "Cosmetic"]:
			trait_filter.add_item(label)
	age_spin.min_value = 16
	age_spin.max_value = 100
	height_spin.min_value = 140
	height_spin.max_value = 250
	str_spin.min_value = 4
	str_spin.max_value = 20
	dex_spin.min_value = 4
	dex_spin.max_value = 20
	int_spin.min_value = 4
	int_spin.max_value = 20
	per_spin.min_value = 4
	per_spin.max_value = 20
	skill_level_spin.min_value = 0
	skill_level_spin.max_value = 10
	full_refresh()

func _ensure_tab_titles() -> void:
	for i in mini(tabs.get_tab_count(), TABS.size()):
		tabs.set_tab_title(i, TABS[i])

func _parse_list(result: Dictionary) -> Array:
	if not result.get("ok", false):
		_show_error(String(result.get("error", "Request failed.")))
		return []
	var raw := String(result.get("json", "[]"))
	var parsed = JSON.parse_string(raw)
	if typeof(parsed) != TYPE_ARRAY:
		return []
	return parsed

func _parse_state(result: Dictionary) -> Dictionary:
	if not result.get("ok", false):
		_show_error(String(result.get("error", "Request failed.")))
		return {}
	var raw := String(result.get("json", "{}"))
	var parsed = JSON.parse_string(raw)
	if typeof(parsed) != TYPE_DICTIONARY:
		return {}
	return parsed

func _show_error(msg: String) -> void:
	status_label.text = msg

func _show_ok(msg: String = "") -> void:
	status_label.text = msg

func _call_mut(result: Dictionary) -> bool:
	if not result.get("ok", false):
		_show_error(String(result.get("error", "Action failed.")))
		return false
	_show_ok()
	return true

func full_refresh() -> void:
	if _host == null:
		return
	_refreshing = true
	_state = _parse_state(_host.chargen_get_state())
	_scenarios = _parse_list(_host.chargen_list_scenarios())
	_professions = _parse_list(_host.chargen_list_professions())
	_hobbies = _parse_list(_host.chargen_list_hobbies())
	_traits = _parse_list(_host.chargen_list_traits())
	_skills = _parse_list(_host.chargen_list_skills())
	_locations = _parse_list(_host.chargen_list_start_locations())
	_cities = _parse_list(_host.chargen_list_cities())
	_apply_state_to_chrome()
	_fill_item_list(scenario_list, _scenarios, String(_state.get("scenario_id", "")))
	_fill_item_list(profession_list, _professions, String(_state.get("profession_id", "")))
	_fill_hobbies()
	_fill_traits()
	_fill_skills()
	_fill_item_list(location_list, _locations, String(_state.get("start_location_id", "random")))
	_fill_item_list(city_list, _cities, String(_state.get("starting_city", "")))
	city_row.visible = bool(_state.get("cities_enabled", false))
	_update_summary()
	_update_detail_for_current_tab()
	confirm_btn.disabled = tabs.current_tab != TABS.size() - 1
	_refreshing = false

func _apply_state_to_chrome() -> void:
	name_edit.text = String(_state.get("name", ""))
	gender_btn.text = "Male" if bool(_state.get("male", true)) else "Female"
	outfit_btn.text = "Outfit: Male" if bool(_state.get("outfit", true)) else "Outfit: Female"
	age_spin.value = int(_state.get("age", 21))
	height_spin.value = int(_state.get("height", 175))
	blood_btn.text = "Blood: %s" % String(_state.get("blood", "?"))
	rating_label.text = _strip_color_tags(String(_state.get("rating", "")))
	summary_label.text = "%s / %s" % [
		String(_state.get("scenario_name", "")),
		String(_state.get("profession_name", ""))
	]
	str_spin.value = int(_state.get("str", 8))
	dex_spin.value = int(_state.get("dex", 8))
	int_spin.value = int(_state.get("int", 8))
	per_spin.value = int(_state.get("per", 8))

func _strip_color_tags(text: String) -> String:
	var re := RegEx.new()
	re.compile("<[^>]+>")
	return re.sub(text, "", true)

func _fill_item_list(list: ItemList, items: Array, selected_id: String) -> void:
	list.clear()
	var select_idx := -1
	for i in items.size():
		var item: Dictionary = items[i]
		var name := String(item.get("name", item.get("id", "?")))
		if item.has("points"):
			name += " (%s)" % str(item.get("points"))
		if item.get("taken", false):
			name = "[x] " + name
		if item.has("enabled") and not bool(item.get("enabled", true)):
			name = "(locked) " + name
		list.add_item(name)
		list.set_item_metadata(i, item)
		if String(item.get("id", "")) == selected_id:
			select_idx = i
	if select_idx >= 0:
		list.select(select_idx)

func _fill_hobbies() -> void:
	hobby_list.clear()
	for i in _hobbies.size():
		var item: Dictionary = _hobbies[i]
		var name := String(item.get("name", "?"))
		if bool(item.get("taken", false)):
			name = "[x] " + name
		hobby_list.add_item(name)
		hobby_list.set_item_metadata(i, item)

func _fill_traits() -> void:
	var filter: int = trait_filter.selected
	var filter_keys: Array[String] = ["all", "positive", "negative", "neutral", "cosmetic"]
	var filter_key: String = filter_keys[clampi(filter, 0, filter_keys.size() - 1)]
	trait_list.clear()
	var idx := 0
	for item in _traits:
		var cat := String(item.get("category", "neutral"))
		if filter_key != "all" and cat != filter_key:
			continue
		var name := String(item.get("name", "?"))
		var points: Variant = item.get("points", 0)
		name += " (%s)" % str(points)
		if bool(item.get("taken", false)):
			name = "[x] " + name
		if bool(item.get("locked", false)):
			name = "[L] " + name
		trait_list.add_item(name)
		trait_list.set_item_metadata(idx, item)
		idx += 1

func _fill_skills() -> void:
	skill_list.clear()
	for i in _skills.size():
		var item: Dictionary = _skills[i]
		var name := "%s [%s] — %d" % [
			String(item.get("name", "?")),
			String(item.get("category", "")),
			int(item.get("level", 0))
		]
		skill_list.add_item(name)
		skill_list.set_item_metadata(i, item)
		if String(item.get("id", "")) == _selected_skill_id:
			skill_list.select(i)
			skill_level_spin.value = int(item.get("level", 0))

func _update_summary() -> void:
	var lines: PackedStringArray = []
	lines.append("Name: %s" % String(_state.get("name", "")))
	lines.append("Gender: %s" % ("Male" if bool(_state.get("male", true)) else "Female"))
	lines.append("Age: %s  Height: %s  Blood: %s" % [
		str(_state.get("age", "")), str(_state.get("height", "")), String(_state.get("blood", ""))
	])
	lines.append("Scenario: %s" % String(_state.get("scenario_name", "")))
	lines.append("Profession: %s" % String(_state.get("profession_name", "")))
	var hobby_names: PackedStringArray = []
	for h in _state.get("hobbies", []):
		hobby_names.append(String(h.get("name", "")))
	lines.append("Backgrounds: %s" % (", ".join(hobby_names) if hobby_names.size() else "(none)"))
	lines.append("Stats: STR %s / DEX %s / INT %s / PER %s" % [
		str(_state.get("str", 8)), str(_state.get("dex", 8)),
		str(_state.get("int", 8)), str(_state.get("per", 8))
	])
	lines.append("Start: %s" % String(_state.get("start_location_name", "")))
	if bool(_state.get("cities_enabled", false)):
		lines.append("City: %s" % String(_state.get("starting_city", "(default)")))
	lines.append("Cataclysm: %s" % String(_state.get("cataclysm_start", "")))
	lines.append("Game start: %s" % String(_state.get("game_start", "")))
	lines.append("Rating: %s" % _strip_color_tags(String(_state.get("rating", ""))))
	var traits: Array = _state.get("traits", [])
	lines.append("Traits: %s" % (", ".join(PackedStringArray(traits)) if traits.size() else "(none)"))
	var skills: Dictionary = _state.get("skills", {})
	var skill_bits: PackedStringArray = []
	for k in skills.keys():
		skill_bits.append("%s %s" % [str(k), str(skills[k])])
	lines.append("Skills: %s" % (", ".join(skill_bits) if skill_bits.size() else "(none)"))
	summary_text.text = "\n".join(lines)

func _update_detail_for_current_tab() -> void:
	var list: ItemList = null
	match tabs.current_tab:
		0: list = scenario_list
		1: list = profession_list
		2: list = hobby_list
		4: list = trait_list
		5: list = skill_list
		_:
			detail_label.text = ""
			return
	var idxs := list.get_selected_items()
	if idxs.is_empty():
		detail_label.text = ""
		return
	var item: Dictionary = list.get_item_metadata(idxs[0])
	var text := String(item.get("description", ""))
	if item.has("reason") and String(item.get("reason", "")) != "":
		text += "\n\n" + String(item.get("reason"))
	detail_label.text = _strip_color_tags(text)

func _on_tab_changed(tab: int) -> void:
	confirm_btn.disabled = tab != TABS.size() - 1
	_update_detail_for_current_tab()

func _on_scenario_selected(index: int) -> void:
	if _refreshing:
		return
	var item: Dictionary = scenario_list.get_item_metadata(index)
	_update_detail_for_current_tab()
	if _call_mut(_host.chargen_set_scenario(String(item.get("id", "")))):
		full_refresh()

func _on_profession_selected(index: int) -> void:
	if _refreshing:
		return
	var item: Dictionary = profession_list.get_item_metadata(index)
	_update_detail_for_current_tab()
	if _call_mut(_host.chargen_set_profession(String(item.get("id", "")))):
		full_refresh()

func _on_hobby_activated(index: int) -> void:
	_toggle_hobby_at(index)

func _on_hobby_selected(index: int) -> void:
	if _refreshing:
		return
	_update_detail_for_current_tab()

func _toggle_hobby_at(index: int) -> void:
	if _refreshing or index < 0:
		return
	var item: Dictionary = hobby_list.get_item_metadata(index)
	if _call_mut(_host.chargen_toggle_hobby(String(item.get("id", "")))):
		full_refresh()

func _on_hobby_toggle_pressed() -> void:
	var idxs := hobby_list.get_selected_items()
	if not idxs.is_empty():
		_toggle_hobby_at(idxs[0])

func _on_trait_activated(index: int) -> void:
	_toggle_trait_at(index)

func _on_trait_selected(index: int) -> void:
	if _refreshing:
		return
	_update_detail_for_current_tab()

func _toggle_trait_at(index: int) -> void:
	if _refreshing or index < 0:
		return
	var item: Dictionary = trait_list.get_item_metadata(index)
	if _call_mut(_host.chargen_toggle_trait(String(item.get("id", "")))):
		full_refresh()

func _on_trait_toggle_pressed() -> void:
	var idxs := trait_list.get_selected_items()
	if not idxs.is_empty():
		_toggle_trait_at(idxs[0])

func _on_skill_selected(index: int) -> void:
	var item: Dictionary = skill_list.get_item_metadata(index)
	_selected_skill_id = String(item.get("id", ""))
	skill_level_spin.value = int(item.get("level", 0))
	_update_detail_for_current_tab()

func _on_skill_level_changed(value: float) -> void:
	if _refreshing or _selected_skill_id.is_empty():
		return
	if _call_mut(_host.chargen_set_skill(_selected_skill_id, int(value))):
		full_refresh()

func _on_location_selected(index: int) -> void:
	if _refreshing:
		return
	var item: Dictionary = location_list.get_item_metadata(index)
	if _call_mut(_host.chargen_set_start_location(String(item.get("id", "")))):
		full_refresh()

func _on_city_selected(index: int) -> void:
	if _refreshing:
		return
	var item: Dictionary = city_list.get_item_metadata(index)
	if _call_mut(_host.chargen_set_starting_city(String(item.get("name", item.get("id", ""))))):
		full_refresh()

func _on_trait_filter_changed(_index: int) -> void:
	_fill_traits()

func _on_stat_changed(_value: float = 0.0) -> void:
	if _refreshing:
		return
	_call_mut(_host.chargen_set_stat("str", int(str_spin.value)))
	_call_mut(_host.chargen_set_stat("dex", int(dex_spin.value)))
	_call_mut(_host.chargen_set_stat("int", int(int_spin.value)))
	_call_mut(_host.chargen_set_stat("per", int(per_spin.value)))
	full_refresh()

func _on_name_submitted(new_text: String) -> void:
	if _call_mut(_host.chargen_set_name(new_text)):
		full_refresh()

func _on_name_focus_exited() -> void:
	_on_name_submitted(name_edit.text)

func _on_gender_pressed() -> void:
	if _call_mut(_host.chargen_set_gender(not bool(_state.get("male", true)))):
		full_refresh()

func _on_outfit_pressed() -> void:
	if _call_mut(_host.chargen_set_outfit(not bool(_state.get("outfit", true)))):
		full_refresh()

func _on_age_changed(value: float) -> void:
	if _refreshing:
		return
	if _call_mut(_host.chargen_set_age(int(value))):
		full_refresh()

func _on_height_changed(value: float) -> void:
	if _refreshing:
		return
	if _call_mut(_host.chargen_set_height(int(value))):
		full_refresh()

func _on_blood_pressed() -> void:
	if _call_mut(_host.chargen_cycle_blood_type()):
		full_refresh()

func _on_random_name_pressed() -> void:
	if _call_mut(_host.chargen_randomize_name()):
		full_refresh()

func _on_random_desc_pressed() -> void:
	if _call_mut(_host.chargen_randomize_description()):
		full_refresh()

func _on_reset_calendar_pressed() -> void:
	if _call_mut(_host.chargen_reset_calendar()):
		full_refresh()

func _on_nudge_cataclysm(hours: int) -> void:
	if _call_mut(_host.chargen_nudge_calendar("cataclysm", hours)):
		full_refresh()

func _on_nudge_game(hours: int) -> void:
	if _call_mut(_host.chargen_nudge_calendar("game", hours)):
		full_refresh()

func _on_save_template_pressed() -> void:
	var tname := template_name_edit.text.strip_edges()
	if tname.is_empty():
		_show_error("Enter a template name.")
		return
	if _call_mut(_host.chargen_save_template(tname)):
		_show_ok("Saved template: %s" % tname)

func _on_prev_pressed() -> void:
	if tabs.current_tab > 0:
		tabs.current_tab -= 1

func _on_next_pressed() -> void:
	if tabs.current_tab < TABS.size() - 1:
		tabs.current_tab += 1

func _on_cancel_pressed() -> void:
	var result: Dictionary = _host.cancel_chargen()
	if not result.get("ok", false):
		_show_error(String(result.get("error", "Cancel failed.")))
		return
	cancelled.emit()

func _on_confirm_pressed() -> void:
	# Commit name, then hand off asynchronously. Host shows GameView and starts
	# the game on the CDDA thread so loading / prompts don't freeze Godot.
	if _host.is_chargen_busy():
		_show_error("Still working…")
		return
	_host.chargen_set_name(name_edit.text)
	_show_ok("Starting game…")
	confirmed.emit()
