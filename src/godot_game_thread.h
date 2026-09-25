#pragma once
#ifndef CATA_SRC_GODOT_GAME_THREAD_H
#define CATA_SRC_GODOT_GAME_THREAD_H

#if defined(GODOT)

#include <string>
#include <vector>

/**
 * Thunks the Godot host (godot/extensions/godot_ext.cpp) uses to drive the CDDA
 * game thread. They wrap engine entry points that the GDExtension translation
 * unit should not include game headers to reach.
 *
 * These live in a header rather than as forward declarations in godot_ext.cpp so
 * both sides agree on the signatures -- and so GCC's -Wmissing-declarations is
 * satisfied, which clang on macOS does not enforce the same way.
 */
namespace godot_backend
{

// Process-level bootstrap, in call order.
void cata_init_allocator();
void ordered_static_globals();
void init_crash_handlers();
void reset_floating_point_mode();
void PATH_INFO_init_base_path( const std::string &path );
void PATH_INFO_init_user_dir( const std::string &path );
void PATH_INFO_set_standard_filenames();
void MAP_SHARING_setDefaults();
void setupDebug( int output );
void json_error_output_colors_t_init();
void setlocale_wrapper();
void get_options_init();
void get_options_load();
bool catacurses_init_interface();
void game_ui_init_ui();
void game_load_static_data();
void cataimgui_init_colors();
void set_language_from_options();

// Session control.
void rng_set_engine_seed( int seed );
void event_bus_send_game_begin( const char *version );
bool game_do_turn();
bool start_new_game( const std::string &mode );
bool start_load_game( const std::string &worldname, const std::string &save_id );

// World / save enumeration for the Godot load-game picker.
std::vector<std::string> list_world_names();
std::vector<std::string> list_save_names( const std::string &worldname );

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_GAME_THREAD_H
