#include "cata_allocator.h"
#include "ordered_static_globals.h"
#include "crash.h"
#include "compatibility.h"
#include "cursesdef.h"
#include "path_info.h"
#include "mapsharing.h"
#include "debug.h"
#include "json_loader.h"
#include "options.h"
#include "game_ui.h"
#include "game.h"
#include "cata_imgui.h"
#include "translations.h"
#include "rng.h"
#include "event.h"
#include "event_bus.h"
#include "main_menu.h"
#include "get_version.h"
#include "input.h"
#include "output.h"
#include "filesystem.h"
#include "worldfactory.h"
#include "avatar.h"
#include "godot_map_snapshot.h"
#include "godot_pixel_minimap.h"
#include "godot_hud_snapshot.h"
#include "godot_view_snapshot.h"
#include "ui_manager.h"
#include "godot_game_thread.h"

#include <filesystem>
#include <system_error>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>

// Forward declaration from main.cpp
void exit_handler( int s );

namespace godot_backend
{

void cata_init_allocator()
{
    cata::init_allocator();
}

void ordered_static_globals()
{
    ::ordered_static_globals();
}

void init_crash_handlers()
{
    ::init_crash_handlers();
}

void reset_floating_point_mode()
{
    ::reset_floating_point_mode();
}

void PATH_INFO_init_base_path( const std::string &path )
{
    // Use the parent directory of the godot project as the base path
    // so that data/raw/colors.json can be found
    std::string base_path = path;
    if( base_path.empty() ) {
        // Default to repo root if empty (parent of godot/)
        base_path = "../";
    }
    // Resolve to an absolute path. The Godot host runs with cwd=godot/, and a
    // relative base path leaks into every path derived from it -- including the
    // JSON flexbuffer cache directories -- so anything that changes the working
    // directory would silently point them somewhere else.
    std::error_code ec;
    const std::filesystem::path absolute_base =
        std::filesystem::absolute( std::filesystem::u8path( base_path ), ec );
    if( !ec ) {
        base_path = absolute_base.lexically_normal().generic_u8string();
    }
    // Ensure it's normalized
    base_path = as_norm_dir( base_path );
    PATH_INFO::init_base_path( base_path );
}

void PATH_INFO_init_user_dir( const std::string &path )
{
    // Same hazard as the base path above, and it matters more here: every save,
    // config and memorial path derives from this one. An empty path means "use
    // the platform's per-user location", which is already absolute, so only a
    // caller-supplied relative path needs resolving.
    std::string user_dir = path;
    if( !user_dir.empty() ) {
        std::error_code ec;
        const std::filesystem::path absolute_user =
            std::filesystem::absolute( std::filesystem::u8path( user_dir ), ec );
        if( !ec ) {
            user_dir = absolute_user.lexically_normal().generic_u8string();
        }
    }
    PATH_INFO::init_user_dir( user_dir );
}

void PATH_INFO_set_standard_filenames()
{
    PATH_INFO::set_standard_filenames();
}

void MAP_SHARING_setDefaults()
{
    MAP_SHARING::setDefaults();
}

void setupDebug( int output )
{
    ::setupDebug( static_cast<DebugOutput>( output ) );
}

void json_error_output_colors_t_init()
{
    // NOLINTNEXTLINE(cata-tests-must-restore-global-state)
    json_error_output_colors = json_error_output_colors_t::color_tags;
}

void setlocale_wrapper()
{
#if !defined(MACOSX)
    if( setlocale( LC_ALL, "" ) == nullptr ) {
        DebugLog( D_WARNING, D_MAIN ) << "Error while setlocale(LC_ALL, '').";
    } else {
#endif
        try {
            std::locale::global( std::locale( "" ) );
        } catch( const std::exception & ) {
            try {
                std::locale::global( std::locale::classic() );
            } catch( const std::exception &err ) {
                debugmsg( "%s", err.what() );
                ::exit_handler( -999 );
            }
        }
#if !defined(MACOSX)
    }
#endif
}

void get_options_init()
{
    get_options().init();
}

void get_options_load()
{
    get_options().load();
}

bool catacurses_init_interface()
{
    try {
        catacurses::init_interface();
        return true;
    } catch( const std::exception &err ) {
        debugmsg( "%s", err.what() );
        return false;
    }
}

void game_ui_init_ui()
{
    game_ui::init_ui();
}

void game_load_static_data()
{
    g = std::make_unique<game>();
    g->load_static_data();
}

void cataimgui_init_colors()
{
    // ImGui context is created by cataimgui::client in catacurses::init_interface.
    cataimgui::init_colors();
}

void set_language_from_options()
{
    ::set_language_from_options();
}

void rng_set_engine_seed( int seed )
{
    ::rng_set_engine_seed( seed );
}

void event_bus_send_game_begin( const char *version )
{
    ::get_event_bus().send<event_type::game_begin>( version );
}

bool game_do_turn()
{
    const bool done = g->do_turn();
#if defined(GODOT)
    // Refresh Godot MapView / HUD after each turn. Keep an open ImGui/curses
    // menu in the overlay; only wipe leftover cells when nothing is up.
    if( !imclient || !imclient->any_window_shown() ) {
        ::godot_backend::get_view_snapshot().clear_all();
    }
    ::godot_backend::update_map_snapshot();
    ::godot_backend::update_hud_snapshot();
    ::godot_backend::update_pixel_minimap();
#endif
    return done;
}

/// Map Godot mode string to main_menu new-game submenu index.
static int new_game_mode_to_sel2( const std::string &mode )
{
    if( mode == "now" ) {
        return 3;
    }
    if( mode == "random" ) {
        return 2;
    }
    if( mode == "full_random" ) {
        return 4;
    }
    if( mode == "preset" ) {
        return 1;
    }
    // "custom" and anything else
    return 0;
}

bool start_new_game( const std::string &mode )
{
    main_menu menu;
    return menu.start_new_character( new_game_mode_to_sel2( mode ) );
}

bool start_load_game( const std::string &worldname, const std::string &save_id )
{
    main_menu menu;
    return menu.start_load_game( worldname, save_id );
}

std::vector<std::string> list_world_names()
{
    if( !world_generator ) {
        return {};
    }
    world_generator->init();
    return world_generator->all_worldnames();
}

std::vector<std::string> list_save_names( const std::string &worldname )
{
    if( !world_generator ) {
        return {};
    }
    WORLD *world = world_generator->get_world( worldname );
    if( world == nullptr ) {
        return {};
    }
    std::vector<std::string> names;
    names.reserve( world->world_saves.size() );
    for( const save_t &save : world->world_saves ) {
        names.push_back( save.decoded_name() );
    }
    return names;
}

} // namespace godot_backend

// Minimal exit_handler for GODOT builds (replaces main.cpp's version)
void exit_handler( int s )
{
    // In GODOT builds, we don't have the full SDL/curses cleanup.
    // Just report and exit cleanly.
    if( s != 0 ) {
        DebugLog( D_ERROR, D_MAIN ) << "exit_handler called with code " << s;
    }
    // Destroy fonts/display while Godot TextServer is still alive.
    try {
        catacurses::endwin();
    } catch( ... ) {
    }
    // Deinit debug logging
    deinitDebug();
    // Reset the game if it exists
    g.reset();
}
