#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "godot_display.h"
#include "godot_input_bridge.h"
#include "godot_backend.h"
#include "godot_crafting_snapshot.h"
#include "godot_dialogue_snapshot.h"
#include "godot_surroundings_snapshot.h"
#include "godot_keybind_snapshot.h"
#include "godot_options_snapshot.h"
#include "godot_popup_snapshot.h"
#include "godot_textwin_snapshot.h"
#include "godot_uilist_snapshot.h"
#include "godot_view_snapshot.h"
#include "godot_anim_snapshot.h"
#include "godot_light_snapshot.h"
#include "godot_map_snapshot.h"
#include "godot_overmap_snapshot.h"
#include "godot_pixel_minimap.h"
#include "godot_hud_snapshot.h"
#include "godot_chargen.h"
#include "godot_game_commands.h"
#include "godot_game_thread.h"
#include "loading_ui.h"

#include "cached_options.h"
#include "filesystem.h"
#include "game.h"
#include "path_info.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward declaration (not in godot_backend namespace)
void exit_handler( int );

namespace godot_backend
{

// Declarations for the above live in src/godot_game_thread.h.

enum class host_command_type {
    none,
    new_game,
    load_game,
    run_session,
    begin_chargen,
    confirm_chargen,
    quit,
};

struct host_command {
    host_command_type type = host_command_type::none;
    std::string mode;
    std::string world;
    std::string save;
};

/// Root host node for the Cataclysm-DDA render-to-texture bridge.
/// Godot owns pre-game chrome; this node bootstraps CDDA and runs session commands.
class CDDAHost : public godot::Node
{
        GDCLASS( CDDAHost, godot::Node )

    protected:
        static void _bind_methods()
        {
            // Native Godot present path (ADR-002): cell grid + palette.
            godot::ClassDB::bind_method( godot::D_METHOD( "get_view_cols" ),
                                        &CDDAHost::get_view_cols );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_view_rows" ),
                                        &CDDAHost::get_view_rows );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_view_cell_width" ),
                                        &CDDAHost::get_view_cell_width );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_view_cell_height" ),
                                        &CDDAHost::get_view_cell_height );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_view_cells" ),
                                        &CDDAHost::get_view_cells );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_view_palette_rgba" ),
                                        &CDDAHost::get_view_palette_rgba );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_view_glyph_count" ),
                                        &CDDAHost::get_view_glyph_count );
            // Tileset MapView (ADR-002)
            godot::ClassDB::bind_method( godot::D_METHOD( "tileset_ready" ),
                                        &CDDAHost::tileset_ready );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_tileset_id" ),
                                        &CDDAHost::get_tileset_id );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_tileset_tile_size" ),
                                        &CDDAHost::get_tileset_tile_size );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_tileset_atlas_count" ),
                                        &CDDAHost::get_tileset_atlas_count );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_tileset_atlas_image", "index" ),
                                        &CDDAHost::get_tileset_atlas_image );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_map_draw_list" ),
                                        &CDDAHost::get_map_draw_list );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_map_ident_table" ),
                                        &CDDAHost::get_map_ident_table );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_map_view_origin" ),
                                        &CDDAHost::get_map_view_origin );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_map_view_size" ),
                                        &CDDAHost::get_map_view_size );
            godot::ClassDB::bind_method(
                godot::D_METHOD( "set_map_view_tiles", "width", "height" ),
                &CDDAHost::set_map_view_tiles );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_map_command_count" ),
                                        &CDDAHost::get_map_command_count );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_map_generation" ),
                                        &CDDAHost::get_map_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_map_glyph_list" ),
                                        &CDDAHost::get_map_glyph_list );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_map_field_list" ),
                                        &CDDAHost::get_map_field_list );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_sprite_coverage", "limit" ),
                                        &CDDAHost::get_sprite_coverage );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_render_stats" ),
                                        &CDDAHost::get_render_stats );
            godot::ClassDB::bind_method(
                godot::D_METHOD( "describe_sprite", "id", "category", "variant" ),
                &CDDAHost::describe_sprite );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_avatar_overlays" ),
                                        &CDDAHost::get_avatar_overlays );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_light_generation" ),
                                        &CDDAHost::get_light_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_light_image" ),
                                        &CDDAHost::get_light_image );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_light_size" ),
                                        &CDDAHost::get_light_size );
            godot::ClassDB::bind_method(
                godot::D_METHOD( "set_light_pass_enabled", "enabled" ),
                &CDDAHost::set_light_pass_enabled );
            godot::ClassDB::bind_method(
                godot::D_METHOD( "set_depth_fog_enabled", "enabled" ),
                &CDDAHost::set_depth_fog_enabled );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_creatures" ),
                                        &CDDAHost::get_creatures );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_light_levels" ),
                                        &CDDAHost::get_light_levels );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_light_sources" ),
                                        &CDDAHost::get_light_sources );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_wind_vector" ),
                                        &CDDAHost::get_wind_vector );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_conditions" ),
                                        &CDDAHost::get_conditions );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_palette_image" ),
                                        &CDDAHost::get_palette_image );
            godot::ClassDB::bind_method( godot::D_METHOD( "set_minimap_size", "width", "height" ),
                                        &CDDAHost::set_minimap_size );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_minimap_generation" ),
                                        &CDDAHost::get_minimap_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_minimap_image" ),
                                        &CDDAHost::get_minimap_image );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_anim_generation" ),
                                        &CDDAHost::get_anim_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_anim_commands" ),
                                        &CDDAHost::get_anim_commands );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_anim_texts" ),
                                        &CDDAHost::get_anim_texts );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_anim_stats" ),
                                        &CDDAHost::get_anim_stats );
            godot::ClassDB::bind_method( godot::D_METHOD( "debug_spawn_monster", "mtype" ),
                                        &CDDAHost::debug_spawn_monster );
            godot::ClassDB::bind_method( godot::D_METHOD( "debug_spawn_npc" ),
                                        &CDDAHost::debug_spawn_npc );
            // Scenario harness (VER-2 item 1)
            godot::ClassDB::bind_method( godot::D_METHOD( "scenario_teleport_omt", "omt_type",
                                         "search_range" ),
                                        &CDDAHost::scenario_teleport_omt );
            godot::ClassDB::bind_method( godot::D_METHOD( "scenario_teleport_rel", "dx", "dy", "dz" ),
                                        &CDDAHost::scenario_teleport_rel );
            godot::ClassDB::bind_method( godot::D_METHOD( "scenario_stand_on", "flag", "radius" ),
                                        &CDDAHost::scenario_stand_on );
            godot::ClassDB::bind_method( godot::D_METHOD( "scenario_set_time", "hour", "minute" ),
                                        &CDDAHost::scenario_set_time );
            godot::ClassDB::bind_method( godot::D_METHOD( "scenario_set_weather", "weather_id" ),
                                        &CDDAHost::scenario_set_weather );
            godot::ClassDB::bind_method( godot::D_METHOD( "scenario_spawn_field", "field_id",
                                         "intensity", "dx", "dy" ),
                                        &CDDAHost::scenario_spawn_field );
            godot::ClassDB::bind_method( godot::D_METHOD( "scenario_spawn_vehicle", "vproto",
                                         "dx", "dy" ),
                                        &CDDAHost::scenario_spawn_vehicle );
            godot::ClassDB::bind_method( godot::D_METHOD( "scenario_spawn_item", "itype",
                                         "dx", "dy" ),
                                        &CDDAHost::scenario_spawn_item );
            godot::ClassDB::bind_method( godot::D_METHOD( "scenario_spawn_furniture", "furn",
                                         "dx", "dy" ),
                                        &CDDAHost::scenario_spawn_furniture );
            godot::ClassDB::bind_method( godot::D_METHOD( "scenario_set_avatar_sex", "male" ),
                                        &CDDAHost::scenario_set_avatar_sex );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_scenario_status" ),
                                        &CDDAHost::get_scenario_status );
            godot::ClassDB::bind_method( godot::D_METHOD( "note_exit_code", "code" ),
                                        &CDDAHost::note_exit_code );
            godot::ClassDB::bind_method( godot::D_METHOD( "commands_ready" ),
                                        &CDDAHost::commands_ready );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_hit_generation" ),
                                        &CDDAHost::get_hit_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_hit_events" ),
                                        &CDDAHost::get_hit_events );
            // Overmap (T2.4)
            godot::ClassDB::bind_method( godot::D_METHOD( "overmap_active" ),
                                        &CDDAHost::overmap_active );
            godot::ClassDB::bind_method( godot::D_METHOD( "overmap_tileset_ready" ),
                                        &CDDAHost::overmap_tileset_ready );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_overmap_tile_size" ),
                                        &CDDAHost::get_overmap_tile_size );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_overmap_atlas_count" ),
                                        &CDDAHost::get_overmap_atlas_count );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_overmap_atlas_image", "index" ),
                                        &CDDAHost::get_overmap_atlas_image );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_overmap_view_size" ),
                                        &CDDAHost::get_overmap_view_size );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_overmap_draw_list" ),
                                        &CDDAHost::get_overmap_draw_list );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_overmap_generation" ),
                                        &CDDAHost::get_overmap_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_overmap_sidebar" ),
                                        &CDDAHost::get_overmap_sidebar );
            godot::ClassDB::bind_method( godot::D_METHOD( "overmap_sidebar_generation" ),
                                        &CDDAHost::overmap_sidebar_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_hud_state" ),
                                        &CDDAHost::get_hud_state );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_character_sheet" ),
                                        &CDDAHost::get_character_sheet );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_inventory_state" ),
                                        &CDDAHost::get_inventory_state );
            godot::ClassDB::bind_method( godot::D_METHOD( "request_item_action", "uid", "action" ),
                                        &CDDAHost::request_item_action );
            godot::ClassDB::bind_method( godot::D_METHOD( "request_menu_action", "action" ),
                                        &CDDAHost::request_menu_action );
            godot::ClassDB::bind_method( godot::D_METHOD( "legacy_ui_active" ),
                                        &CDDAHost::legacy_ui_active );
            godot::ClassDB::bind_method( godot::D_METHOD( "api_version" ),
                                        &CDDAHost::api_version );
            godot::ClassDB::bind_method( godot::D_METHOD( "uilist_active" ),
                                        &CDDAHost::uilist_active );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_uilist_state" ),
                                        &CDDAHost::get_uilist_state );
            godot::ClassDB::bind_method( godot::D_METHOD( "uilist_generation" ),
                                        &CDDAHost::uilist_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "uilist_select", "index" ),
                                        &CDDAHost::uilist_select );
            godot::ClassDB::bind_method( godot::D_METHOD( "uilist_select_category", "index" ),
                                        &CDDAHost::uilist_select_category );
            godot::ClassDB::bind_method( godot::D_METHOD( "uilist_confirm", "index" ),
                                        &CDDAHost::uilist_confirm );
            godot::ClassDB::bind_method( godot::D_METHOD( "uilist_cancel" ),
                                        &CDDAHost::uilist_cancel );
            godot::ClassDB::bind_method( godot::D_METHOD( "uilist_set_filter", "text" ),
                                        &CDDAHost::uilist_set_filter );
            godot::ClassDB::bind_method( godot::D_METHOD( "popup_active" ),
                                        &CDDAHost::popup_active );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_popup_state" ),
                                        &CDDAHost::get_popup_state );
            godot::ClassDB::bind_method( godot::D_METHOD( "popup_generation" ),
                                        &CDDAHost::popup_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "popup_answer", "index" ),
                                        &CDDAHost::popup_answer );
            godot::ClassDB::bind_method( godot::D_METHOD( "popup_cancel" ),
                                        &CDDAHost::popup_cancel );
            godot::ClassDB::bind_method( godot::D_METHOD( "popup_answer_text", "text" ),
                                        &CDDAHost::popup_answer_text );
            godot::ClassDB::bind_method( godot::D_METHOD( "textwin_active" ),
                                        &CDDAHost::textwin_active );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_textwin_state" ),
                                        &CDDAHost::get_textwin_state );
            godot::ClassDB::bind_method( godot::D_METHOD( "textwin_generation" ),
                                        &CDDAHost::textwin_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "textwin_select_tab", "index" ),
                                        &CDDAHost::textwin_select_tab );
            godot::ClassDB::bind_method( godot::D_METHOD( "textwin_dismiss" ),
                                        &CDDAHost::textwin_dismiss );
            godot::ClassDB::bind_method( godot::D_METHOD( "options_active" ),
                                        &CDDAHost::options_active );
            godot::ClassDB::bind_method( godot::D_METHOD( "options_layout_generation" ),
                                        &CDDAHost::options_layout_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "options_values_generation" ),
                                        &CDDAHost::options_values_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_options_layout" ),
                                        &CDDAHost::get_options_layout );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_options_values" ),
                                        &CDDAHost::get_options_values );
            godot::ClassDB::bind_method( godot::D_METHOD( "options_set", "option", "value" ),
                                        &CDDAHost::options_set );
            godot::ClassDB::bind_method( godot::D_METHOD( "options_step", "option", "delta" ),
                                        &CDDAHost::options_step );
            godot::ClassDB::bind_method( godot::D_METHOD( "options_dismiss" ),
                                        &CDDAHost::options_dismiss );
            godot::ClassDB::bind_method( godot::D_METHOD( "keybind_active" ),
                                        &CDDAHost::keybind_active );
            godot::ClassDB::bind_method( godot::D_METHOD( "keybind_generation" ),
                                        &CDDAHost::keybind_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_keybind_state" ),
                                        &CDDAHost::get_keybind_state );
            godot::ClassDB::bind_method( godot::D_METHOD( "keybind_request", "action_id", "op" ),
                                        &CDDAHost::keybind_request );
            godot::ClassDB::bind_method( godot::D_METHOD( "keybind_dismiss" ),
                                        &CDDAHost::keybind_dismiss );
            godot::ClassDB::bind_method( godot::D_METHOD( "crafting_active" ),
                                        &CDDAHost::crafting_active );
            godot::ClassDB::bind_method( godot::D_METHOD( "crafting_list_generation" ),
                                        &CDDAHost::crafting_list_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "crafting_detail_generation" ),
                                        &CDDAHost::crafting_detail_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_crafting_list" ),
                                        &CDDAHost::get_crafting_list );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_crafting_detail" ),
                                        &CDDAHost::get_crafting_detail );
            godot::ClassDB::bind_method( godot::D_METHOD( "crafting_selected" ),
                                        &CDDAHost::crafting_selected );
            godot::ClassDB::bind_method( godot::D_METHOD( "crafting_action", "action" ),
                                        &CDDAHost::crafting_action );
            godot::ClassDB::bind_method( godot::D_METHOD( "crafting_select_row", "row" ),
                                        &CDDAHost::crafting_select_row );
            godot::ClassDB::bind_method( godot::D_METHOD( "crafting_select_tab", "index" ),
                                        &CDDAHost::crafting_select_tab );
            godot::ClassDB::bind_method( godot::D_METHOD( "crafting_select_subtab", "index" ),
                                        &CDDAHost::crafting_select_subtab );
            godot::ClassDB::bind_method( godot::D_METHOD( "dialogue_active" ),
                                        &CDDAHost::dialogue_active );
            godot::ClassDB::bind_method( godot::D_METHOD( "dialogue_generation" ),
                                        &CDDAHost::dialogue_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_dialogue_state" ),
                                        &CDDAHost::get_dialogue_state );
            godot::ClassDB::bind_method( godot::D_METHOD( "dialogue_action", "action" ),
                                        &CDDAHost::dialogue_action );
            godot::ClassDB::bind_method( godot::D_METHOD( "dialogue_select", "index" ),
                                        &CDDAHost::dialogue_select );
            godot::ClassDB::bind_method( godot::D_METHOD( "surroundings_active" ),
                                        &CDDAHost::surroundings_active );
            godot::ClassDB::bind_method( godot::D_METHOD( "surroundings_generation" ),
                                        &CDDAHost::surroundings_generation );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_surroundings_state" ),
                                        &CDDAHost::get_surroundings_state );
            godot::ClassDB::bind_method( godot::D_METHOD( "surroundings_action", "action" ),
                                        &CDDAHost::surroundings_action );
            godot::ClassDB::bind_method( godot::D_METHOD( "surroundings_select", "index" ),
                                        &CDDAHost::surroundings_select );
            godot::ClassDB::bind_method( godot::D_METHOD( "push_input_event", "event" ),
                                        &CDDAHost::push_input_event );
            godot::ClassDB::bind_method( godot::D_METHOD( "set_window_size", "width", "height" ),
                                        &CDDAHost::set_window_size );
            godot::ClassDB::bind_method( godot::D_METHOD( "set_terminal_cell_geometry",
                                        "origin_x", "origin_y", "cell_w", "cell_h" ),
                                        &CDDAHost::set_terminal_cell_geometry );
            godot::ClassDB::bind_method( godot::D_METHOD( "toggle_fullscreen" ),
                                        &CDDAHost::toggle_fullscreen );
            godot::ClassDB::bind_method( godot::D_METHOD( "bootstrap_async" ),
                                        &CDDAHost::bootstrap_async );
            godot::ClassDB::bind_method( godot::D_METHOD( "is_ready" ), &CDDAHost::is_ready );
            godot::ClassDB::bind_method( godot::D_METHOD( "is_session_active" ),
                                        &CDDAHost::is_session_active );
            godot::ClassDB::bind_method( godot::D_METHOD( "is_loading" ),
                                        &CDDAHost::is_loading );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_loading_context" ),
                                        &CDDAHost::get_loading_context );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_loading_step" ),
                                        &CDDAHost::get_loading_step );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_loading_tip" ),
                                        &CDDAHost::get_loading_tip );
            godot::ClassDB::bind_method( godot::D_METHOD( "bootstrap_failed" ),
                                        &CDDAHost::bootstrap_failed );
            godot::ClassDB::bind_method( godot::D_METHOD( "get_error_message" ),
                                        &CDDAHost::get_error_message );
            godot::ClassDB::bind_method( godot::D_METHOD( "request_new_game", "mode" ),
                                        &CDDAHost::request_new_game );
            godot::ClassDB::bind_method( godot::D_METHOD( "request_load_game", "world", "save" ),
                                        &CDDAHost::request_load_game );
            godot::ClassDB::bind_method( godot::D_METHOD( "request_quit" ), &CDDAHost::request_quit );
            godot::ClassDB::bind_method( godot::D_METHOD( "list_worlds" ), &CDDAHost::list_worlds );
            godot::ClassDB::bind_method( godot::D_METHOD( "list_saves", "world" ),
                                        &CDDAHost::list_saves );
            // Legacy alias used by older host scripts.
            godot::ClassDB::bind_method( godot::D_METHOD( "start_game" ), &CDDAHost::bootstrap_async );

            godot::ClassDB::bind_method( godot::D_METHOD( "is_chargen_active" ),
                                        &CDDAHost::is_chargen_active );
            godot::ClassDB::bind_method( godot::D_METHOD( "world_has_saves", "world" ),
                                        &CDDAHost::world_has_saves );
            godot::ClassDB::bind_method( godot::D_METHOD( "create_world_default", "name" ),
                                        &CDDAHost::create_world_default );
            godot::ClassDB::bind_method( godot::D_METHOD( "begin_custom_chargen", "world" ),
                                        &CDDAHost::begin_custom_chargen );
            godot::ClassDB::bind_method( godot::D_METHOD( "request_begin_custom_chargen", "world" ),
                                        &CDDAHost::request_begin_custom_chargen );
            godot::ClassDB::bind_method( godot::D_METHOD( "cancel_chargen" ),
                                        &CDDAHost::cancel_chargen );
            godot::ClassDB::bind_method( godot::D_METHOD( "confirm_chargen" ),
                                        &CDDAHost::confirm_chargen );
            godot::ClassDB::bind_method( godot::D_METHOD( "request_confirm_chargen" ),
                                        &CDDAHost::request_confirm_chargen );
            godot::ClassDB::bind_method( godot::D_METHOD( "is_chargen_busy" ),
                                        &CDDAHost::is_chargen_busy );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_last_error" ),
                                        &CDDAHost::chargen_last_error );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_get_state" ),
                                        &CDDAHost::chargen_get_state );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_list_scenarios" ),
                                        &CDDAHost::chargen_list_scenarios );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_list_professions" ),
                                        &CDDAHost::chargen_list_professions );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_list_hobbies" ),
                                        &CDDAHost::chargen_list_hobbies );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_list_traits" ),
                                        &CDDAHost::chargen_list_traits );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_list_skills" ),
                                        &CDDAHost::chargen_list_skills );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_list_start_locations" ),
                                        &CDDAHost::chargen_list_start_locations );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_list_cities" ),
                                        &CDDAHost::chargen_list_cities );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_set_scenario", "id" ),
                                        &CDDAHost::chargen_set_scenario );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_set_profession", "id" ),
                                        &CDDAHost::chargen_set_profession );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_toggle_hobby", "id" ),
                                        &CDDAHost::chargen_toggle_hobby );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_set_stat", "stat", "value" ),
                                        &CDDAHost::chargen_set_stat );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_toggle_trait", "id" ),
                                        &CDDAHost::chargen_toggle_trait );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_set_skill", "id", "level" ),
                                        &CDDAHost::chargen_set_skill );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_set_name", "name" ),
                                        &CDDAHost::chargen_set_name );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_set_gender", "male" ),
                                        &CDDAHost::chargen_set_gender );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_set_outfit", "outfit" ),
                                        &CDDAHost::chargen_set_outfit );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_set_age", "age" ),
                                        &CDDAHost::chargen_set_age );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_set_height", "height" ),
                                        &CDDAHost::chargen_set_height );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_cycle_blood_type" ),
                                        &CDDAHost::chargen_cycle_blood_type );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_set_start_location", "id" ),
                                        &CDDAHost::chargen_set_start_location );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_set_starting_city", "name" ),
                                        &CDDAHost::chargen_set_starting_city );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_reset_calendar" ),
                                        &CDDAHost::chargen_reset_calendar );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_nudge_calendar", "which", "hours" ),
                                        &CDDAHost::chargen_nudge_calendar );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_randomize_name" ),
                                        &CDDAHost::chargen_randomize_name );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_randomize_description" ),
                                        &CDDAHost::chargen_randomize_description );
            godot::ClassDB::bind_method( godot::D_METHOD( "chargen_save_template", "name" ),
                                        &CDDAHost::chargen_save_template );
        }

    public:
        CDDAHost() = default;
        ~CDDAHost() override
        {
            request_quit();
            // Drop the backend's godot::Ref state now, while the engine is still
            // standing. Left to static destruction it unrefs Objects after Godot
            // has torn down its ObjectDB, which faults on freed memory -- and
            // does it intermittently, with whatever signal the reused allocation
            // produces, always after the run's real work is done. That is the
            // "leaked ObjectDB instances at exit" warning, and the exit-time
            // SIGSEGV/SIGILL that cost a day of bisecting things that were fine.
            godot_backend::release_godot_resources();

            // An unconditional join freezes Godot on window close, because the
            // game thread may be parked in chargen, a menu, or input. An
            // unconditional detach is what used to happen, and it exits 139:
            //
            //   Thread 1  ~generic_factory<oter_type_t>   (static destruction)
            //   Thread 21 game_do_turn -> update_hud_snapshot
            //               -> Character::get_stamina_max
            //               -> get_option<int>( <garbage std::string> )
            //
            // The game thread had *just* finished a turn and was publishing the
            // HUD, microseconds from the loop's own quit check, while the main
            // thread was already destroying the option map out from under it.
            // That is why an earlier unconditional 250ms sleep did not help and
            // was rightly removed: sleeping does not tell you the thread stopped,
            // and it does nothing at all when the thread is blocked in input.
            //
            // So wait on the thread's own "I am done" flag instead, and only
            // join once it is set -- which it always will be for the common case
            // of quitting from the game loop.
            for( int i = 0; i < 200 && !thread_finished_.load(); ++i ) {
                std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
            }
            if( thread_finished_.load() ) {
                if( game_thread_.joinable() ) {
                    game_thread_.join();
                }
                return;
            }
            // A second was not enough, so the thread is parked somewhere that
            // ignores the shutdown flag. Returning from here would run static
            // destructors against a live thread, which is the crash above. The
            // process is on its way out and the game has already saved, so there
            // is nothing left to flush: leave without running them.
            godot::UtilityFunctions::printerr(
                "CDDA game thread did not stop within 1s; exiting without static teardown" );
            if( game_thread_.joinable() ) {
                game_thread_.detach();
            }
            // The code the host registered, not 0: _Exit from here used to
            // overwrite the exit code Godot was about to return, so a failing
            // probe run reported success. (It also skips stdio flushing, which
            // is why the printerr above was never seen doing it.)
            std::_Exit( godot_backend::host_exit_code() );
        }

        int get_view_cols() const
        {
            return get_view_snapshot().cols();
        }

        int get_view_rows() const
        {
            return get_view_snapshot().rows();
        }

        int get_view_cell_width() const
        {
            return get_view_snapshot().cell_pixel_w();
        }

        int get_view_cell_height() const
        {
            return get_view_snapshot().cell_pixel_h();
        }

        // PackedInt32Array of [codepoint, fg, bg] * cols * rows.
        godot::PackedInt32Array get_view_cells() const
        {
            std::vector<int32_t> cells;
            get_view_snapshot().copy_cells( cells );
            godot::PackedInt32Array arr;
            arr.resize( static_cast<int64_t>( cells.size() ) );
            int32_t *dst = arr.ptrw();
            if( !cells.empty() ) {
                std::memcpy( dst, cells.data(), cells.size() * sizeof( int32_t ) );
            }
            return arr;
        }

        // PackedByteArray of RGBA bytes for the 16-color game palette.
        godot::PackedByteArray get_view_palette_rgba() const
        {
            std::vector<uint8_t> rgba;
            get_view_snapshot().copy_palette_rgba( rgba );
            godot::PackedByteArray arr;
            arr.resize( static_cast<int64_t>( rgba.size() ) );
            uint8_t *dst = arr.ptrw();
            if( !rgba.empty() ) {
                std::memcpy( dst, rgba.data(), rgba.size() );
            }
            return arr;
        }

        int get_view_glyph_count() const
        {
            return get_view_snapshot().count_glyph_cells();
        }

        bool tileset_ready() const
        {
            return get_map_snapshot().tileset_ready();
        }

        godot::String get_tileset_id() const
        {
            return godot::String::utf8( get_map_snapshot().tileset_id().c_str() );
        }

        godot::Vector2i get_tileset_tile_size() const
        {
            return godot::Vector2i( get_map_snapshot().tile_width(),
                                    get_map_snapshot().tile_height() );
        }

        int get_tileset_atlas_count() const
        {
            return get_map_snapshot().atlas_count();
        }

        godot::Ref<godot::Image> get_tileset_atlas_image( int index ) const
        {
            return get_map_snapshot().copy_atlas_image( index );
        }

        godot::PackedInt32Array get_map_draw_list() const
        {
            return get_map_snapshot().copy_draw_list();
        }

        /// The interned id table for the draw list's ident bits (3D-8d):
        /// element N is the base terrain/furniture id whose commands carry
        /// N + 1 in rot_flags bits 16-30. Append-only, so the caller can cache
        /// and re-copy only when the size grows.
        godot::PackedStringArray get_map_ident_table() const
        {
            return get_map_snapshot().copy_ident_table();
        }

        /// Tiles that resolved to no sprite at all and are drawn as their JSON
        /// symbol instead (SP-1). Same frame as the draw list; see
        /// MapSnapshot::glyph_stride.
        godot::PackedInt32Array get_map_glyph_list() const
        {
            return get_map_snapshot().copy_glyph_list();
        }

        /// Fields worth animating with particles -- fire and anything gaseous
        /// (SP-6). Same frame as the draw list; see MapSnapshot::field_stride.
        godot::PackedInt32Array get_map_field_list() const
        {
            return get_map_snapshot().copy_field_list();
        }

        /// Every creature in view, by identity (ADR-006's mesh amendment, 3D-7c): each
        /// entry is { id, kind, uid, move_mode, x, y, z_below, flip }, with x and y the
        /// view-relative pixels of the creature's feet.
        ///
        /// The draw list carries no identity on purpose -- a command is an atlas sub-rect,
        /// which is all a sprite needs and nothing a mesh can use. This is how a renderer
        /// gets to know that the thing at those pixels is a zombie.
        godot::Array get_creatures() const
        {
            return get_map_snapshot().copy_creatures();
        }

        /// The session's sprite misses, most-drawn first (SP-2). Each entry is
        /// { id, category, level, level_name, hits }. Pass limit <= 0 for all.
        godot::Array get_sprite_coverage( int limit ) const
        {
            return get_map_snapshot().copy_sprite_coverage( limit );
        }

        /// Everything the render debug overlay shows (SP-9).
        godot::Dictionary get_render_stats() const
        {
            return get_map_snapshot().copy_render_stats();
        }

        /// Why one id draws what it draws (SP-9): its resolution level, any
        /// palette redirect, and whether it sways.
        godot::Dictionary describe_sprite( const godot::String &id,
                                           const godot::String &category,
                                           const godot::String &variant ) const
        {
            return get_map_snapshot().describe_sprite(
                       std::string( id.utf8().get_data() ),
                       std::string( category.utf8().get_data() ),
                       std::string( variant.utf8().get_data() ) );
        }

        /// The avatar's character overlays in draw order, each with the sprite
        /// it resolved to and whether the tileset had one at all.
        godot::Array get_avatar_overlays() const
        {
            return get_map_snapshot().copy_avatar_overlays();
        }

        // --- Light pass (SP-3, SP-4) -----------------------------------------
        /// Bumped by every rebuilt light frame; poll before re-uploading.
        int64_t get_light_generation() const
        {
            return static_cast<int64_t>( godot_backend::get_light_snapshot().generation() );
        }

        /// Per-tile lighting as an RGBA8 Image over the same extent as the draw
        /// list: R visibility, G light amount. See LightSnapshot.
        godot::Ref<godot::Image> get_light_image() const
        {
            return godot_backend::get_light_snapshot().copy_image();
        }

        godot::Vector2i get_light_size() const
        {
            return godot_backend::get_light_snapshot().size();
        }

        /// Level blocks in the light image (ADR-006 item 3D-4). The image is one block of
        /// `get_light_size().y` rows per z-level published, deepest last, so a consumer
        /// that only wants the avatar's level must scale its V by this rather than
        /// stretching one block over the whole texture.
        int get_light_levels() const
        {
            return godot_backend::get_light_snapshot().levels();
        }

        /// The frame's light *sources*, for the 3D backend's real lights (ADR-006
        /// item 3D-2): nine floats each -- x, y, radius, r, g, b, luminance, bearing,
        /// cone -- with x, y and radius in view-relative pixels, luminance in the
        /// game's own units, and a cone of zero meaning a lamp rather than a beam.
        /// See LightSnapshot::add_light for where they come from and what is
        /// deliberately missing from them.
        ///
        /// The light *texture* remains the authority on how lit a tile is. This says
        /// where the light comes from, which is what a per-tile value cannot express
        /// and what a 3D renderer needs to put a light in the world.
        godot::PackedFloat32Array get_light_sources() const
        {
            return godot_backend::get_light_snapshot().copy_lights();
        }

        /// MapView reports whether it is running the light pass. Until it does,
        /// C++ keeps baking light into each sprite's tint, so a host that never
        /// builds the texture still gets a lit map rather than a flat one.
        void set_light_pass_enabled( bool enabled )
        {
            godot_backend::get_light_snapshot().set_pass_enabled( enabled );
        }

        /// The renderer reports that it dims lower z-levels itself, so C++ stops baking
        /// that dimming into the tints (ADR-006 item 3D-4). A host that never calls this
        /// keeps the baked fog, which is what the 2D backend wants.
        void set_depth_fog_enabled( bool enabled )
        {
            godot_backend::get_light_snapshot().set_depth_fog_enabled( enabled );
        }

        /// Screen-space wind for the sway shader (SP-7): unit direction times
        /// 0..1 strength. Published by the game thread with the light frame.
        godot::Vector2 get_wind_vector() const
        {
            return godot_backend::get_light_snapshot().wind();
        }

        /// What the presentation pass grades by: daylight, precipitation and
        /// pain, each 0..1. See LightSnapshot::conditions.
        godot::Dictionary get_conditions() const
        {
            const auto c = godot_backend::get_light_snapshot().get_conditions();
            godot::Dictionary d;
            d["daylight"] = c.daylight;
            d["precipitation"] = c.precipitation;
            d["pain"] = c.pain;
            // Compass degrees and degrees above the horizon. Altitude is negative at
            // night, which is how "no sun" is said without a second flag.
            d["sun_azimuth"] = c.sun_azimuth;
            d["sun_altitude"] = c.sun_altitude;
            // What is falling: 0 nothing, 1 rain, 2 snow, 3 acid. Precipitation
            // says how much; this says what the particle pass should draw.
            d["weather_kind"] = c.weather_kind;
            return d;
        }

        /// The tileset's palette ramps (SP-8), one row per palette. Null when
        /// the tileset declares none.
        godot::Ref<godot::Image> get_palette_image() const
        {
            return get_map_snapshot().copy_palette_image();
        }

        godot::Vector2i get_map_view_origin() const
        {
            return get_map_snapshot().view_origin_tiles();
        }

        godot::Vector2i get_map_view_size() const
        {
            return get_map_snapshot().view_size_tiles();
        }

        int get_map_command_count() const
        {
            return get_map_snapshot().command_count();
        }

        /// Changes whenever the draw list is rebuilt. MapView polls this to avoid
        /// re-batching an unchanged map on every frame.
        int64_t get_map_generation() const
        {
            return static_cast<int64_t>( get_map_snapshot().generation() );
        }

        /// Ask for a minimap of this pixel size. Zero disables rendering, so the
        /// game thread does not pay for a panel nobody is showing.
        void set_minimap_size( int width, int height )
        {
            set_pixel_minimap_size( point( width, height ) );
        }

        // --- Overmap (T2.4) ---------------------------------------------------
        /// True while the overmap UI is on screen, so the host knows when to show
        /// OvermapView instead of MapView.
        bool overmap_active() const
        {
            return get_overmap_snapshot().active();
        }

        bool overmap_tileset_ready() const
        {
            return get_overmap_snapshot().tileset_ready();
        }

        godot::Vector2i get_overmap_tile_size() const
        {
            return godot::Vector2i( get_overmap_snapshot().tile_width(),
                                    get_overmap_snapshot().tile_height() );
        }

        int get_overmap_atlas_count() const
        {
            return get_overmap_snapshot().atlas_count();
        }

        godot::Ref<godot::Image> get_overmap_atlas_image( int index ) const
        {
            return get_overmap_snapshot().copy_atlas_image( index );
        }

        godot::Vector2i get_overmap_view_size() const
        {
            return get_overmap_snapshot().view_size_tiles();
        }

        godot::PackedInt32Array get_overmap_draw_list() const
        {
            return get_overmap_snapshot().copy_draw_list();
        }

        int64_t get_overmap_generation() const
        {
            return static_cast<int64_t>( get_overmap_snapshot().generation() );
        }

        /// The sidebar's text, recorded from the functions that used to draw it
        /// through the overlay. See overmap_sidebar::record.
        godot::Dictionary get_overmap_sidebar() const
        {
            return godot_backend::get_overmap_snapshot().copy_sidebar();
        }
        int64_t overmap_sidebar_generation() const
        {
            return static_cast<int64_t>(
                       godot_backend::get_overmap_snapshot().sidebar_generation() );
        }

        /// Bumped by each committed animation frame (explosions, bullets, hit
        /// markers, aim cursor). Poll before copying the commands.
        int64_t get_anim_generation() const
        {
            return static_cast<int64_t>( get_anim_snapshot().generation() );
        }

        /// Packed animation overlay primitives; see AnimSnapshot::cmd_stride.
        godot::PackedInt32Array get_anim_commands() const
        {
            return get_anim_snapshot().copy_commands();
        }

        /// Scrolling combat text for the current animation frame: damage
        /// numbers, "Critical!", healing, XP. Each entry is
        /// { pos, text, fg, align, life, run }.
        godot::Array get_anim_texts() const
        {
            return get_anim_snapshot().copy_texts();
        }

        /// Which link of the animation chain is failing, when nothing appears.
        godot::Dictionary get_anim_stats() const
        {
            return get_anim_snapshot().copy_stats();
        }

        /// Spawn a monster next to the avatar (test fixture, see
        /// godot_backend::request_debug_spawn). Returns "" when queued.
        godot::String debug_spawn_monster( const godot::String &mtype )
        {
            const std::string err = godot_backend::request_debug_spawn(
                                        std::string( mtype.utf8().get_data() ) );
            return godot::String::utf8( err.c_str() );
        }

        /// Put an NPC next to the avatar, so the conversation, faction, mission
        /// and follower screens can be verified rather than waited for.
        godot::String debug_spawn_npc()
        {
            return godot::String::utf8( godot_backend::request_debug_spawn_npc().c_str() );
        }

        // --- Scenario harness (BACKLOG.md VER-2 item 1) ---
        // Dress the world for verification: night, a lamp, a fire, a forest, a
        // staircase. Thin wrappers; the rules live in godot_game_commands.cpp,
        // including the one that matters most -- none of these may open a
        // blocking screen. "" back means queued, not done: poll
        // get_scenario_status() for the outcome.

        godot::String scenario_teleport_omt( const godot::String &omt_type, int search_range )
        {
            return godot::String::utf8( godot_backend::request_scenario_teleport_omt(
                                            std::string( omt_type.utf8().get_data() ),
                                            search_range ).c_str() );
        }

        godot::String scenario_teleport_rel( int dx, int dy, int dz )
        {
            return godot::String::utf8(
                       godot_backend::request_scenario_teleport_rel( dx, dy, dz ).c_str() );
        }

        godot::String scenario_stand_on( const godot::String &flag, int radius )
        {
            return godot::String::utf8( godot_backend::request_scenario_stand_on(
                                            std::string( flag.utf8().get_data() ),
                                            radius ).c_str() );
        }

        godot::String scenario_set_time( int hour, int minute )
        {
            return godot::String::utf8(
                       godot_backend::request_scenario_set_time( hour, minute ).c_str() );
        }

        godot::String scenario_set_weather( const godot::String &weather_id )
        {
            return godot::String::utf8( godot_backend::request_scenario_set_weather(
                                            std::string( weather_id.utf8().get_data() ) ).c_str() );
        }

        godot::String scenario_spawn_field( const godot::String &field_id, int intensity,
                                            int dx, int dy )
        {
            return godot::String::utf8( godot_backend::request_scenario_spawn_field(
                                            std::string( field_id.utf8().get_data() ),
                                            intensity, dx, dy ).c_str() );
        }

        godot::String scenario_spawn_vehicle( const godot::String &vproto, int dx, int dy )
        {
            return godot::String::utf8( godot_backend::request_scenario_spawn_vehicle(
                                            std::string( vproto.utf8().get_data() ),
                                            dx, dy ).c_str() );
        }

        godot::String scenario_spawn_item( const godot::String &itype, int dx, int dy )
        {
            return godot::String::utf8( godot_backend::request_scenario_spawn_item(
                                            std::string( itype.utf8().get_data() ),
                                            dx, dy ).c_str() );
        }

        godot::String scenario_spawn_furniture( const godot::String &furn, int dx, int dy )
        {
            return godot::String::utf8( godot_backend::request_scenario_spawn_furniture(
                                            std::string( furn.utf8().get_data() ),
                                            dx, dy ).c_str() );
        }

        godot::String scenario_set_avatar_sex( bool male )
        {
            return godot::String::utf8(
                       godot_backend::request_scenario_set_avatar_sex( male ).c_str() );
        }

        /// Outcome of the last scenario command that finished on the game thread.
        /// `generation` increments per completion; poll it, then read `ok`.
        godot::Dictionary get_scenario_status() const
        {
            const godot_backend::scenario_status s = godot_backend::get_scenario_status();
            godot::Dictionary d;
            d["generation"] = s.generation;
            d["ok"] = s.ok;
            d["last"] = godot::String::utf8( s.last.c_str() );
            d["detail"] = godot::String::utf8( s.detail.c_str() );
            return d;
        }

        /// Register the exit code the process should carry.
        ///
        /// Shutdown with a live session ends in std::_Exit from the game
        /// thread's input wait, which cannot read the code SceneTree.quit was
        /// given -- so a fixture that means to exit non-zero must say so here
        /// *before* quitting, or its failure exits 0 and reads as a pass.
        void note_exit_code( int code )
        {
            godot_backend::set_host_exit_code( code );
        }

        /// Whether a queued command would actually run if posted now.
        ///
        /// Commands are refused while any C++ window is shown, because the input
        /// wait is then nested inside that window's own loop and running one
        /// there would re-enter the game from inside a menu. A caller that posts
        /// without checking gets silence: the command sits in the queue, and the
        /// screen it would have opened is reported as never opening.
        bool commands_ready() const
        {
            return godot_backend::commands_safe_to_run();
        }


        /// Id of the most recent creature hit (SP-5). Poll this rather than the
        /// events themselves: a fight lands several per turn and MapView only
        /// needs to know when one it has not played yet exists.
        int64_t get_hit_generation() const
        {
            return static_cast<int64_t>( get_anim_snapshot().hit_generation() );
        }

        /// Recent creature hits; see AnimSnapshot::hit_stride.
        godot::PackedInt32Array get_hit_events() const
        {
            return get_anim_snapshot().copy_hits();
        }

        /// Bumped by each rendered minimap frame; poll before rebuilding a texture.
        int64_t get_minimap_generation() const
        {
            return static_cast<int64_t>( get_pixel_minimap().generation() );
        }

        /// The last rendered minimap frame as an RGBA8 Image, or null if there is
        /// none yet. The pixels are copied under the minimap's mutex, so this is
        /// safe to call while the game thread renders the next frame.
        godot::Ref<godot::Image> get_minimap_image() const
        {
            int w = 0;
            int h = 0;
            const std::vector<uint8_t> rgba = get_pixel_minimap().copy_rgba( w, h );
            if( w <= 0 || h <= 0 || rgba.size() != static_cast<size_t>( w ) * h * 4 ) {
                return {};
            }
            godot::PackedByteArray bytes;
            bytes.resize( static_cast<int64_t>( rgba.size() ) );
            std::memcpy( bytes.ptrw(), rgba.data(), rgba.size() );
            return godot::Image::create_from_data( w, h, false, godot::Image::FORMAT_RGBA8, bytes );
        }

        godot::Dictionary get_hud_state() const
        {
            return get_hud_snapshot().copy_hud();
        }

        godot::Dictionary get_character_sheet() const
        {
            return get_hud_snapshot().copy_character();
        }

        /**
         * Act on a carried item from a Godot panel.
         *
         * @param action 0 wield, 1 wear, 2 drop -- godot_backend::item_action.
         * @return a failure reason, or "" when the command was queued. Queued is not
         *         the same as done: it runs when the game thread next reaches its
         *         input wait, and can still fail there (the item may be gone).
         */
        // --- read-only text windows (item info, tile description) --------
        bool textwin_active() const
        {
            return godot_backend::get_textwin_snapshot().active();
        }
        godot::Dictionary get_textwin_state() const
        {
            return godot_backend::get_textwin_snapshot().copy_state();
        }
        int64_t textwin_generation() const
        {
            return static_cast<int64_t>( godot_backend::get_textwin_snapshot().generation() );
        }
        void textwin_select_tab( int index )
        {
            godot_backend::get_textwin_snapshot().select_tab( index );
        }
        void textwin_dismiss()
        {
            godot_backend::get_textwin_snapshot().dismiss();
        }

        // --- the options screen ------------------------------------------
        // Layout and values are read separately: the layout is fixed while the
        // screen is up, the values change with every edit. See the snapshot
        // header for why that split matters.
        bool options_active() const
        {
            return godot_backend::get_options_snapshot().active();
        }
        int64_t options_layout_generation() const
        {
            return static_cast<int64_t>( godot_backend::get_options_snapshot().layout_generation() );
        }
        int64_t options_values_generation() const
        {
            return static_cast<int64_t>( godot_backend::get_options_snapshot().values_generation() );
        }
        godot::Dictionary get_options_layout() const
        {
            return godot_backend::get_options_snapshot().copy_layout();
        }
        godot::Dictionary get_options_values() const
        {
            return godot_backend::get_options_snapshot().copy_values();
        }
        void options_set( const godot::String &option, const godot::String &value )
        {
            godot_backend::get_options_snapshot().request_set( option.utf8().get_data(),
                    value.utf8().get_data() );
        }
        void options_step( const godot::String &option, int delta )
        {
            godot_backend::get_options_snapshot().request_step( option.utf8().get_data(), delta );
        }
        void options_dismiss()
        {
            godot_backend::get_options_snapshot().dismiss();
        }

        // --- the keybindings screen --------------------------------------
        bool keybind_active() const
        {
            return godot_backend::get_keybind_snapshot().active();
        }
        int64_t keybind_generation() const
        {
            return static_cast<int64_t>( godot_backend::get_keybind_snapshot().generation() );
        }
        godot::Dictionary get_keybind_state() const
        {
            return godot_backend::get_keybind_snapshot().copy_state();
        }
        void keybind_request( const godot::String &action_id, int op )
        {
            godot_backend::get_keybind_snapshot().request( action_id.utf8().get_data(), op );
        }
        void keybind_dismiss()
        {
            godot_backend::get_keybind_snapshot().dismiss();
        }

        // --- the crafting screen -------------------------------------------
        bool crafting_active() const
        {
            return godot_backend::get_crafting_snapshot().active();
        }
        int64_t crafting_list_generation() const
        {
            return static_cast<int64_t>( godot_backend::get_crafting_snapshot().list_generation() );
        }
        int64_t crafting_detail_generation() const
        {
            return static_cast<int64_t>( godot_backend::get_crafting_snapshot().detail_generation() );
        }
        godot::Dictionary get_crafting_list() const
        {
            return godot_backend::get_crafting_snapshot().copy_list();
        }
        godot::Dictionary get_crafting_detail() const
        {
            return godot_backend::get_crafting_snapshot().copy_detail();
        }
        int crafting_selected() const
        {
            return godot_backend::get_crafting_snapshot().selected();
        }
        void crafting_action( const godot::String &action )
        {
            godot_backend::get_crafting_snapshot().request_action( action.utf8().get_data() );
        }
        void crafting_select_row( int row )
        {
            godot_backend::get_crafting_snapshot().request_select( row );
        }
        void crafting_select_tab( int index )
        {
            godot_backend::get_crafting_snapshot().request_tab( index );
        }
        void crafting_select_subtab( int index )
        {
            godot_backend::get_crafting_snapshot().request_subtab( index );
        }

        // --- NPC conversation ----------------------------------------------
        bool dialogue_active() const
        {
            return godot_backend::get_dialogue_snapshot().active();
        }
        int64_t dialogue_generation() const
        {
            return static_cast<int64_t>( godot_backend::get_dialogue_snapshot().generation() );
        }
        godot::Dictionary get_dialogue_state() const
        {
            return godot_backend::get_dialogue_snapshot().copy_state();
        }
        void dialogue_action( const godot::String &action )
        {
            godot_backend::get_dialogue_snapshot().request_action( action.utf8().get_data() );
        }
        void dialogue_select( int index )
        {
            godot_backend::get_dialogue_snapshot().request_select( index );
        }

        // --- the surroundings list ("look around") -------------------------
        bool surroundings_active() const
        {
            return godot_backend::get_surroundings_snapshot().active();
        }
        int64_t surroundings_generation() const
        {
            return static_cast<int64_t>( godot_backend::get_surroundings_snapshot().generation() );
        }
        godot::Dictionary get_surroundings_state() const
        {
            return godot_backend::get_surroundings_snapshot().copy_state();
        }
        void surroundings_action( const godot::String &action )
        {
            godot_backend::get_surroundings_snapshot().request_action( action.utf8().get_data() );
        }
        void surroundings_select( int index )
        {
            godot_backend::get_surroundings_snapshot().request_select( index );
        }

        // --- query_popup, rendered as a Godot Control --------------------
        bool popup_active() const
        {
            return godot_backend::get_popup_snapshot().active();
        }
        godot::Dictionary get_popup_state() const
        {
            return godot_backend::get_popup_snapshot().copy_state();
        }
        int64_t popup_generation() const
        {
            return static_cast<int64_t>( godot_backend::get_popup_snapshot().generation() );
        }
        void popup_answer( int index )
        {
            godot_backend::get_popup_snapshot().answer( index );
        }
        void popup_cancel()
        {
            godot_backend::get_popup_snapshot().cancel();
        }
        void popup_answer_text( const godot::String &text )
        {
            godot_backend::get_popup_snapshot().answer_text(
                std::string( text.utf8().get_data() ) );
        }

        // --- uilist, rendered as a Godot Control -------------------------
        // The game thread is blocked inside uilist::query() while these are live;
        // it polls the same snapshot for the answer.
        bool uilist_active() const
        {
            return godot_backend::get_uilist_snapshot().active();
        }
        godot::Dictionary get_uilist_state() const
        {
            return godot_backend::get_uilist_snapshot().copy_state();
        }
        int64_t uilist_generation() const
        {
            return static_cast<int64_t>( godot_backend::get_uilist_snapshot().generation() );
        }
        void uilist_select( int index )
        {
            godot_backend::get_uilist_snapshot().set_selected( index );
        }
        void uilist_select_category( int index )
        {
            godot_backend::get_uilist_snapshot().select_category( index );
        }
        void uilist_confirm( int index )
        {
            godot_backend::get_uilist_snapshot().confirm( index );
        }
        void uilist_cancel()
        {
            godot_backend::get_uilist_snapshot().cancel();
        }
        void uilist_set_filter( const godot::String &text )
        {
            godot_backend::get_uilist_snapshot().set_filter(
                std::string( text.utf8().get_data() ) );
        }

        /// Version of the C++/GDScript contract.
        ///
        /// The Godot project loads its scripts from disk every run, but the
        /// GDExtension is a compiled library. Running new scripts against an old
        /// library therefore *looks* like it works: the new layout appears, and
        /// every field the old library does not emit silently reads back as zero
        /// or empty. Bump this whenever the dictionaries or methods change, and
        /// host.gd says so instead of leaving the reader to guess.
        int api_version() const
        {
            // 27: rot_flags bits 16-30 carry the interned tile id and
            // get_map_ident_table() resolves them (3D-8d); 26 added cmd_shape
            // in bits 13-15 (3D-8c). An old library leaves both zero, which
            // reads as "no claim" and quietly costs shapes and meshes alike --
            // exactly the mixed state the handshake turns into a loud boot
            // error.
            return 27;
        }

        /// Whether a C++ screen is currently drawn in the overlay.
        ///
        /// While one is, Godot must stop claiming keys for its own panels --
        /// otherwise a legacy screen opened from the Godot game menu could never
        /// be closed, because Escape would never reach it.
        bool legacy_ui_active() const
        {
            return godot_backend::get_view_snapshot().any_content();
        }

        /// Menu actions from the Godot game menu. Same channel as item actions:
        /// queued, then run on the game thread at its input wait.
        godot::String request_menu_action( int action )
        {
            using godot_backend::menu_action;
            if( action < static_cast<int>( menu_action::quicksave ) ||
                action > static_cast<int>( menu_action::distractions ) ) {
                return godot::String( "Unknown menu action." );
            }
            const std::string err =
                godot_backend::request_menu_action( static_cast<menu_action>( action ) );
            return godot::String::utf8( err.c_str() );
        }

        godot::String request_item_action( int64_t uid, int action )
        {
            if( action < 0 || action > static_cast<int>( item_action::drop ) ) {
                return godot::String( "Unknown item action." );
            }
            const std::string err = godot_backend::request_item_action(
                                        uid, static_cast<item_action>( action ) );
            return godot::String::utf8( err.c_str() );
        }

        godot::Dictionary get_inventory_state() const
        {
            return get_hud_snapshot().copy_inventory();
        }

        void push_input_event( const godot::Ref<godot::InputEvent> &event )
        {
            if( event.is_valid() ) {
                get_input_bridge().push_event( event );
            }
        }

        /// MapView tells us how many tiles its viewport covers at the current
        /// zoom, so the draw list matches the screen instead of the curses grid.
        void set_map_view_tiles( int width, int height )
        {
            get_map_snapshot().set_requested_view_tiles( width, height );
        }

        void set_window_size( int width, int height )
        {
            request_window_resize( width, height );
        }

        /// Tell the input bridge where the cell grid is drawn, so mouse events
        /// can be converted from Godot pixels to CDDA cell coordinates.
        /// TerminalView calls this whenever its layout changes.
        void set_terminal_cell_geometry( int origin_x, int origin_y, int cell_w, int cell_h )
        {
            get_input_bridge().set_cell_geometry( origin_x, origin_y, cell_w, cell_h );
        }

        void toggle_fullscreen()
        {
            if( display *d = get_display() ) {
                d->toggle_fullscreen();
            }
        }

        void bootstrap_async()
        {
            if( game_running_.exchange( true ) ) {
                return;
            }
            ready_ = false;
            failed_ = false;
            session_active_ = false;
            game_thread_ = std::thread( [this]() {
                run_cdda_game();
            } );
        }

        bool is_ready() const
        {
            return ready_.load();
        }

        bool is_session_active() const
        {
            return session_active_.load();
        }

        bool is_loading() const
        {
            return loading_ui::active();
        }

        godot::String get_loading_context() const
        {
            return godot::String::utf8( loading_ui::context().c_str() );
        }

        godot::String get_loading_step() const
        {
            return godot::String::utf8( loading_ui::step().c_str() );
        }

        godot::String get_loading_tip() const
        {
            return godot::String::utf8( loading_ui::tip().c_str() );
        }

        bool bootstrap_failed() const
        {
            return failed_.load();
        }

        /// What actually went wrong, for the host to show. Printing it to the
        /// console is not enough: a player running the app normally never sees it,
        /// and a dead game thread otherwise looks exactly like a frozen game.
        godot::String get_error_message() const
        {
            std::lock_guard<std::mutex> lock( error_mutex_ );
            return godot::String::utf8( error_message_.c_str() );
        }

        void request_new_game( const godot::String &mode )
        {
            queue_command( host_command{
                host_command_type::new_game,
                std::string( mode.utf8().get_data() ),
                {},
                {}
            } );
        }

        void request_load_game( const godot::String &world, const godot::String &save )
        {
            queue_command( host_command{
                host_command_type::load_game,
                {},
                std::string( world.utf8().get_data() ),
                std::string( save.utf8().get_data() )
            } );
        }

        void request_quit()
        {
            game_running_ = false;
            request_shutdown();
            // If a session is mid-turn, mark quit so do_turn can return promptly.
            if( g ) {
                g->uquit = QUIT_NOSAVED;
            }
            queue_command( host_command{ host_command_type::quit, {}, {}, {} } );
            sync_cv_.notify_all();
        }

        godot::PackedStringArray list_worlds() const
        {
            godot::PackedStringArray out;
            if( !ready_.load() || session_active_.load() ) {
                return out;
            }
            for( const std::string &name : list_world_names() ) {
                // World names are player-supplied and routinely non-ASCII; the
                // implicit const char * conversion would decode them as latin-1.
                out.push_back( godot::String::utf8( name.c_str() ) );
            }
            return out;
        }

        godot::PackedStringArray list_saves( const godot::String &world ) const
        {
            godot::PackedStringArray out;
            if( !ready_.load() || session_active_.load() ) {
                return out;
            }
            const std::string world_name( world.utf8().get_data() );
            for( const std::string &name : list_save_names( world_name ) ) {
                out.push_back( godot::String::utf8( name.c_str() ) );
            }
            return out;
        }

        bool is_chargen_active() const
        {
            return godot_backend::is_chargen_active();
        }

        // Read-only snapshot; same thread policy as list_worlds (no game-thread
        // round-trip — that froze the world picker for every listed world).
        bool world_has_saves( const godot::String &world ) const
        {
            if( !ready_.load() || session_active_.load() ) {
                return false;
            }
            const std::string world_name( world.utf8().get_data() );
            return godot_backend::world_has_saves( world_name );
        }

        godot::Dictionary create_world_default( const godot::String &name )
        {
            const std::string n( name.utf8().get_data() );
            return result_to_dict( invoke_on_game_thread( [n]() {
                return godot_backend::create_world_default( n );
            } ) );
        }

        // Async: queues world setup on the game thread. Poll is_chargen_busy /
        // is_chargen_active / chargen_last_error from Godot.
        void request_begin_custom_chargen( const godot::String &world )
        {
            clear_chargen_status();
            chargen_busy_ = true;
            queue_command( host_command{
                host_command_type::begin_chargen,
                {},
                std::string( world.utf8().get_data() ),
                {}
            } );
        }

        // Legacy sync wrapper kept for callers that still expect a Dictionary;
        // prefer request_begin_custom_chargen to avoid freezing the UI.
        godot::Dictionary begin_custom_chargen( const godot::String &world )
        {
            request_begin_custom_chargen( world );
            godot::Dictionary d;
            d["ok"] = true;
            d["error"] = "";
            d["json"] = "";
            d["value"] = world;
            return d;
        }

        godot::Dictionary cancel_chargen()
        {
            return result_to_dict( invoke_on_game_thread( []() {
                return godot_backend::cancel_chargen();
            } ) );
        }

        // Async: finalize + start_game + do_turn on the game thread. Show GameView
        // first so loading UI / style prompts can present and receive input.
        void request_confirm_chargen()
        {
            clear_chargen_status();
            chargen_busy_ = true;
            queue_command( host_command{ host_command_type::confirm_chargen, {}, {}, {} } );
        }

        godot::Dictionary confirm_chargen()
        {
            request_confirm_chargen();
            godot::Dictionary d;
            d["ok"] = true;
            d["error"] = "";
            d["json"] = "";
            d["value"] = "";
            return d;
        }

        bool is_chargen_busy() const
        {
            return chargen_busy_.load();
        }

        godot::String chargen_last_error() const
        {
            std::lock_guard<std::mutex> lock( chargen_status_mutex_ );
            return godot::String::utf8( chargen_last_error_.c_str() );
        }

        godot::Dictionary chargen_get_state()
        {
            return result_to_dict( invoke_on_game_thread( []() {
                return get_state();
            } ) );
        }

        godot::Dictionary chargen_list_scenarios()
        {
            return result_to_dict( invoke_on_game_thread( []() {
                return list_scenarios();
            } ) );
        }

        godot::Dictionary chargen_list_professions()
        {
            return result_to_dict( invoke_on_game_thread( []() {
                return list_professions();
            } ) );
        }

        godot::Dictionary chargen_list_hobbies()
        {
            return result_to_dict( invoke_on_game_thread( []() {
                return list_hobbies();
            } ) );
        }

        godot::Dictionary chargen_list_traits()
        {
            return result_to_dict( invoke_on_game_thread( []() {
                return list_traits();
            } ) );
        }

        godot::Dictionary chargen_list_skills()
        {
            return result_to_dict( invoke_on_game_thread( []() {
                return list_skills();
            } ) );
        }

        godot::Dictionary chargen_list_start_locations()
        {
            return result_to_dict( invoke_on_game_thread( []() {
                return list_start_locations();
            } ) );
        }

        godot::Dictionary chargen_list_cities()
        {
            return result_to_dict( invoke_on_game_thread( []() {
                return list_cities();
            } ) );
        }

        godot::Dictionary chargen_set_scenario( const godot::String &id )
        {
            const std::string s( id.utf8().get_data() );
            return result_to_dict( invoke_on_game_thread( [s]() {
                return set_scenario( s );
            } ) );
        }

        godot::Dictionary chargen_set_profession( const godot::String &id )
        {
            const std::string s( id.utf8().get_data() );
            return result_to_dict( invoke_on_game_thread( [s]() {
                return set_profession( s );
            } ) );
        }

        godot::Dictionary chargen_toggle_hobby( const godot::String &id )
        {
            const std::string s( id.utf8().get_data() );
            return result_to_dict( invoke_on_game_thread( [s]() {
                return toggle_hobby( s );
            } ) );
        }

        godot::Dictionary chargen_set_stat( const godot::String &stat, int value )
        {
            const std::string s( stat.utf8().get_data() );
            return result_to_dict( invoke_on_game_thread( [s, value]() {
                return set_stat( s, value );
            } ) );
        }

        godot::Dictionary chargen_toggle_trait( const godot::String &id )
        {
            const std::string s( id.utf8().get_data() );
            return result_to_dict( invoke_on_game_thread( [s]() {
                return toggle_trait( s );
            } ) );
        }

        godot::Dictionary chargen_set_skill( const godot::String &id, int level )
        {
            const std::string s( id.utf8().get_data() );
            return result_to_dict( invoke_on_game_thread( [s, level]() {
                return set_skill( s, level );
            } ) );
        }

        godot::Dictionary chargen_set_name( const godot::String &name )
        {
            const std::string s( name.utf8().get_data() );
            return result_to_dict( invoke_on_game_thread( [s]() {
                // Qualify to avoid Node::set_name.
                return ::godot_backend::set_name( s );
            } ) );
        }

        godot::Dictionary chargen_set_gender( bool male )
        {
            return result_to_dict( invoke_on_game_thread( [male]() {
                return set_gender( male );
            } ) );
        }

        godot::Dictionary chargen_set_outfit( bool outfit )
        {
            return result_to_dict( invoke_on_game_thread( [outfit]() {
                return set_outfit( outfit );
            } ) );
        }

        godot::Dictionary chargen_set_age( int age )
        {
            return result_to_dict( invoke_on_game_thread( [age]() {
                return set_age( age );
            } ) );
        }

        godot::Dictionary chargen_set_height( int height )
        {
            return result_to_dict( invoke_on_game_thread( [height]() {
                return set_height( height );
            } ) );
        }

        godot::Dictionary chargen_cycle_blood_type()
        {
            return result_to_dict( invoke_on_game_thread( []() {
                return cycle_blood_type();
            } ) );
        }

        godot::Dictionary chargen_set_start_location( const godot::String &id )
        {
            const std::string s( id.utf8().get_data() );
            return result_to_dict( invoke_on_game_thread( [s]() {
                return set_start_location( s );
            } ) );
        }

        godot::Dictionary chargen_set_starting_city( const godot::String &name )
        {
            const std::string s( name.utf8().get_data() );
            return result_to_dict( invoke_on_game_thread( [s]() {
                return set_starting_city( s );
            } ) );
        }

        godot::Dictionary chargen_reset_calendar()
        {
            return result_to_dict( invoke_on_game_thread( []() {
                return reset_calendar();
            } ) );
        }

        godot::Dictionary chargen_nudge_calendar( const godot::String &which, int hours )
        {
            const std::string s( which.utf8().get_data() );
            return result_to_dict( invoke_on_game_thread( [s, hours]() {
                return nudge_calendar( s, hours );
            } ) );
        }

        godot::Dictionary chargen_randomize_name()
        {
            return result_to_dict( invoke_on_game_thread( []() {
                return randomize_name();
            } ) );
        }

        godot::Dictionary chargen_randomize_description()
        {
            return result_to_dict( invoke_on_game_thread( []() {
                return randomize_description();
            } ) );
        }

        godot::Dictionary chargen_save_template( const godot::String &name )
        {
            const std::string s( name.utf8().get_data() );
            return result_to_dict( invoke_on_game_thread( [s]() {
                return save_template( s );
            } ) );
        }

    private:
        chargen_result invoke_on_game_thread( std::function<chargen_result()> fn )
        {
            if( !ready_.load() || !game_running_.load() ) {
                chargen_result r;
                r.ok = false;
                r.error = "Host is not ready.";
                return r;
            }
            if( session_active_.load() ) {
                chargen_result r;
                r.ok = false;
                r.error = "Game session is active.";
                return r;
            }
            if( chargen_busy_.load() ) {
                chargen_result r;
                r.ok = false;
                r.error = "Character creation is busy.";
                return r;
            }
            std::unique_lock<std::mutex> lock( cmd_mutex_ );
            sync_fn_ = std::move( fn );
            sync_pending_ = true;
            cmd_cv_.notify_one();
            sync_cv_.wait( lock, [this]() {
                return !sync_pending_ || !game_running_.load();
            } );
            return sync_result_;
        }

        static godot::Dictionary result_to_dict( const chargen_result &r )
        {
            godot::Dictionary d;
            d["ok"] = r.ok;
            // String::utf8, not String(const char *): the latter decodes latin-1,
            // which mangles every non-ASCII name carried in the chargen payload.
            d["error"] = godot::String::utf8( r.error.c_str() );
            d["json"] = godot::String::utf8( r.json.c_str() );
            d["value"] = godot::String::utf8( r.value.c_str() );
            return d;
        }

        // Drain sync work; must be called with cmd_mutex_ held. Returns true if work ran.
        bool drain_sync_work( std::unique_lock<std::mutex> &lock )
        {
            if( !sync_pending_ ) {
                return false;
            }
            auto fn = std::move( sync_fn_ );
            sync_fn_ = nullptr;
            lock.unlock();
            chargen_result result = fn ? fn() : chargen_result{};
            lock.lock();
            sync_result_ = std::move( result );
            sync_pending_ = false;
            sync_cv_.notify_all();
            return true;
        }

        void queue_command( host_command cmd )
        {
            {
                std::lock_guard<std::mutex> lock( cmd_mutex_ );
                pending_ = std::move( cmd );
            }
            cmd_cv_.notify_one();
        }

        void clear_chargen_status()
        {
            std::lock_guard<std::mutex> lock( chargen_status_mutex_ );
            chargen_last_error_.clear();
        }

        void set_chargen_status( const chargen_result &r )
        {
            std::lock_guard<std::mutex> lock( chargen_status_mutex_ );
            chargen_last_error_ = r.ok ? std::string{} : r.error;
        }

        host_command wait_for_command()
        {
            std::unique_lock<std::mutex> lock( cmd_mutex_ );
            if( !is_chargen_active() ) {
                session_active_ = false;
            }
            while( game_running_.load() ) {
                cmd_cv_.wait( lock, [this]() {
                    return pending_.type != host_command_type::none || sync_pending_ ||
                           pending_run_session_ || !game_running_.load();
                } );
                if( !game_running_.load() ) {
                    host_command q;
                    q.type = host_command_type::quit;
                    return q;
                }
                if( drain_sync_work( lock ) ) {
                    continue;
                }
                if( pending_run_session_ ) {
                    pending_run_session_ = false;
                    host_command c;
                    c.type = host_command_type::run_session;
                    return c;
                }
                if( pending_.type != host_command_type::none ) {
                    host_command cmd = pending_;
                    pending_ = host_command{};
                    return cmd;
                }
            }
            host_command q;
            q.type = host_command_type::quit;
            return q;
        }

        void run_cdda_game()
        {
            try {
                cata_init_allocator();
                ordered_static_globals();
                init_crash_handlers();
                reset_floating_point_mode();

                PATH_INFO_init_base_path( "" );
                godot::UtilityFunctions::print( "Base path: ",
                                                PATH_INFO::base_path().get_unrelative_path().generic_string().c_str() );
                // Saves, config and memorial live here. Overridable so two runs
                // can coexist: the headless probe starts a new world every run,
                // and two of them against one user dir write worlds side by side
                // through a shared config, which reads as engine flakiness rather
                // than as the collision it is.
                const char *user_dir = std::getenv( "CDDA_GODOT_USER_DIR" );
                PATH_INFO_init_user_dir( user_dir && *user_dir ? user_dir : "." );
                PATH_INFO_set_standard_filenames();
                // Mirror main.cpp assure_essential_dirs_exist — SDL creates these before play.
                for( const std::string &path : {
                    PATH_INFO::config_dir(),
                    PATH_INFO::savedir(),
                    PATH_INFO::templatedir(),
                    PATH_INFO::user_font(),
                    PATH_INFO::user_sound().get_unrelative_path().u8string(),
                    PATH_INFO::user_gfx().get_unrelative_path().u8string()
                } ) {
                    if( !assure_dir_exist( path ) ) {
                        godot::UtilityFunctions::printerr( "Unable to make directory: ", path.c_str() );
                        failed_ = true;
                        ready_ = false;
                        game_running_ = false;
                        return;
                    }
                }
                MAP_SHARING_setDefaults();

                setupDebug( 1 );
                json_error_output_colors_t_init();
                setlocale_wrapper();

                get_options_init();
                get_options_load();
                // GODOT build has no tile blitter wired into the live draw path.
                // A leftover USE_TILES=true from SDL configs skips ASCII map::draw.
                use_tiles = false;
                use_tiles_overmap = false;

                if( !catacurses_init_interface() ) {
                    failed_ = true;
                    ready_ = false;
                    game_running_ = false;
                    return;
                }

                set_language_from_options();
                rng_set_engine_seed( 0 );
                game_ui_init_ui();
                game_load_static_data();
                cataimgui_init_colors();
                loading_ui::done();

                // Bootstrap complete — Godot main menu takes over (no C++ opening_screen).
                ready_ = true;
                godot::UtilityFunctions::print( "CDDA bootstrap ready" );

                while( game_running_.load() ) {
                    const host_command cmd = wait_for_command();
                    if( cmd.type == host_command_type::quit || !game_running_.load() ) {
                        break;
                    }

                    if( cmd.type == host_command_type::begin_chargen ) {
                        // Keep Godot UI responsive: do not set session_active.
                        const chargen_result r = godot_backend::begin_custom_chargen( cmd.world );
                        set_chargen_status( r );
                        chargen_busy_ = false;
                        continue;
                    }

                    if( cmd.type == host_command_type::confirm_chargen ) {
                        // Mark session active before finalize/start_game so Godot keeps
                        // MapView + input forwarding while style/worldgen prompts run.
                        session_active_ = true;
                        const chargen_result r = godot_backend::confirm_chargen();
                        set_chargen_status( r );
                        chargen_busy_ = false;
                        if( r.ok && game_running_.load() ) {
                            ensure_tileset_loaded( "UltimateCataclysm" );
                            update_map_snapshot();
                            update_hud_snapshot();
                            event_bus_send_game_begin( "0.C-Godot" );
                            while( game_running_.load() && !game_do_turn() ) {}
                        }
                        session_active_ = false;
                        continue;
                    }

                    if( cmd.type == host_command_type::run_session ) {
                        session_active_ = true;
                        ensure_tileset_loaded( "UltimateCataclysm" );
                        update_map_snapshot();
                            update_hud_snapshot();
                        event_bus_send_game_begin( "0.C-Godot" );
                        while( game_running_.load() && !game_do_turn() ) {}
                        session_active_ = false;
                        continue;
                    }

                    session_active_ = true;
                    bool started = false;
                    if( cmd.type == host_command_type::new_game ) {
                        started = start_new_game( cmd.mode.empty() ? "custom" : cmd.mode );
                    } else if( cmd.type == host_command_type::load_game ) {
                        started = start_load_game( cmd.world, cmd.save );
                    }

                    if( started && game_running_.load() ) {
                        ensure_tileset_loaded( "UltimateCataclysm" );
                        update_map_snapshot();
                            update_hud_snapshot();
                        event_bus_send_game_begin( "0.C-Godot" );
                        while( game_running_.load() && !game_do_turn() ) {
                            // Game loop
                        }
                    }
                    session_active_ = false;
                }

                ::exit_handler( 0 );
            } catch( const std::exception &err ) {
                {
                    std::lock_guard<std::mutex> lock( error_mutex_ );
                    error_message_ = err.what();
                }
                failed_ = true;
                ready_ = false;
                session_active_ = false;
                godot::UtilityFunctions::printerr( "CDDA game thread error: ", err.what() );
                ::exit_handler( -999 );
            }
            game_running_ = false;
            ready_ = false;
            session_active_ = false;
            // Last thing the thread does. The destructor waits on this before
            // letting the process run its static destructors; see the comment
            // there for what happens if it does not.
            thread_finished_ = true;
            sync_cv_.notify_all();
        }

        std::thread game_thread_;
        std::atomic<bool> thread_finished_{ false };
        std::atomic<bool> game_running_{ false };
        std::atomic<bool> ready_{ false };
        std::atomic<bool> failed_{ false };
        std::atomic<bool> session_active_{ false };
        std::atomic<bool> chargen_busy_{ false };

        std::mutex cmd_mutex_;
        std::condition_variable cmd_cv_;
        host_command pending_;

        mutable std::mutex error_mutex_;
        std::string error_message_;
        std::function<chargen_result()> sync_fn_;
        bool sync_pending_ = false;
        chargen_result sync_result_;
        std::condition_variable sync_cv_;
        bool pending_run_session_ = false;

        mutable std::mutex chargen_status_mutex_;
        std::string chargen_last_error_;
};

} // namespace godot_backend

using namespace godot_backend;

// Only referenced by the entry point below, so keep them out of the dynamic
// symbol table.
static void initialize_godot_backend_module( godot::ModuleInitializationLevel p_level )
{
    if( p_level != godot::MODULE_INITIALIZATION_LEVEL_SCENE ) {
        return;
    }
    GDREGISTER_CLASS( CDDAHost );
}

static void uninitialize_godot_backend_module( godot::ModuleInitializationLevel p_level )
{
    if( p_level != godot::MODULE_INITIALIZATION_LEVEL_SCENE ) {
        return;
    }
}

extern "C"
{

// GDExtension entry point referenced by godot/extensions/cataclysm.gdextension.
// Declared before it is defined so -Wmissing-declarations is satisfied: this is
// the one symbol the library deliberately exports.
GDExtensionBool GDE_EXPORT godot_backend_extension_init(
    GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library,
    GDExtensionInitialization *r_initialization );

GDExtensionBool GDE_EXPORT godot_backend_extension_init(
    GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library,
    GDExtensionInitialization *r_initialization )
{
    godot::GDExtensionBinding::InitObject init_obj( p_get_proc_address, p_library,
            r_initialization );

    init_obj.register_initializer( initialize_godot_backend_module );
    init_obj.register_terminator( uninitialize_godot_backend_module );
    init_obj.set_minimum_library_initialization_level( godot::MODULE_INITIALIZATION_LEVEL_SCENE );

    return init_obj.init();
}

} // extern "C"
