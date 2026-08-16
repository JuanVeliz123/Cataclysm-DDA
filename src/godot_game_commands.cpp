#include "godot_game_commands.h"

#if defined(GODOT)

#include "avatar.h"
#include "cata_imgui.h"
#include "character.h"
#include "game.h"
#include "inventory.h"
#include "item.h"
#include "item_location.h"
#include "creature_tracker.h"
#include "map.h"
#include "mtype.h"
#include "type_id.h"
#include "messages.h"
#include "action.h"
#include "auto_note.h"
#include "auto_pickup.h"
#include "color.h"
#include "distraction_manager.h"
#include "help.h"
#include "input_context.h"
#include "options.h"
#include "safemode_ui.h"
#include "ui_manager.h"
#include "translations.h"

#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace godot_backend
{

namespace
{

std::mutex g_mutex;
std::deque<std::function<void()>> g_queue;

/// Every item_location the avatar can act on: wielded, worn, and carried.
/// Mirrors the walk HudSnapshot uses to build the panel's list, so anything the
/// player can see there can be addressed here.
std::vector<item_location> all_actionable_items( avatar &u )
{
    std::vector<item_location> out;
    if( item_location wielded = u.get_wielded_item() ) {
        out.push_back( wielded );
    }
    for( const item_location &loc : u.get_visible_worn_items() ) {
        out.push_back( loc );
    }
    for( item_location &loc : u.all_items_loc() ) {
        out.push_back( loc );
    }
    return out;
}

item_location find_item_by_uid( avatar &u, const int64_t uid )
{
    for( item_location &loc : all_actionable_items( u ) ) {
        const item *it = loc.get_item();
        if( it && static_cast<int64_t>( it->uid().get_value() ) == uid ) {
            return loc;
        }
    }
    return item_location();
}

} // namespace

void post_game_command( std::function<void()> fn )
{
    if( !fn ) {
        return;
    }
    std::lock_guard<std::mutex> lock( g_mutex );
    g_queue.push_back( std::move( fn ) );
}

bool commands_safe_to_run()
{
    if( !g ) {
        return false;
    }
    // A C++ menu being up means this input wait is nested inside that menu's loop.
    if( imclient && imclient->any_window_shown() ) {
        return false;
    }
    return true;
}

bool drain_game_commands()
{
    std::deque<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> lock( g_mutex );
        if( g_queue.empty() ) {
            return false;
        }
        work.swap( g_queue );
    }
    for( std::function<void()> &fn : work ) {
        fn();
    }
    return true;
}

std::string request_menu_action( const menu_action action )
{
    post_game_command( [action]() {
        if( !g ) {
            return;
        }
        switch( action ) {
            case menu_action::quicksave:
                g->quicksave();
                break;
            case menu_action::save_and_quit:
                if( g->save() ) {
                    get_avatar().set_moves( 0 );
                    g->uquit = QUIT_SAVED;
                } else {
                    add_msg( m_bad, _( "Unable to save." ) );
                }
                break;
            case menu_action::quit_without_saving:
                get_avatar().set_moves( 0 );
                g->uquit = QUIT_NOSAVED;
                break;
            // Everything below is still a C++ screen. It opens over MapView
            // through the curses overlay; each disappears from here as it is
            // migrated to a Godot Control.
            case menu_action::options:
                get_options().show( true );
                break;
            case menu_action::keybindings:
                get_default_mode_input_context().display_menu();
                break;
            case menu_action::safe_mode:
                get_safemode().show();
                break;
            case menu_action::auto_pickup:
                get_auto_pickup().show();
                break;
            case menu_action::colors:
                all_colors.show_gui();
                break;
            case menu_action::help:
                get_help().display_help();
                break;
            case menu_action::auto_notes:
                get_auto_notes_settings().show_gui();
                break;
            case menu_action::distractions:
                get_distraction_manager().show();
                break;
        }
    } );
    return std::string();
}

std::string request_item_action( const int64_t uid, const item_action action )
{
    if( uid == 0 ) {
        return _( "No item selected." );
    }
    post_game_command( [uid, action]() {
        if( !g ) {
            return;
        }
        avatar &u = get_avatar();
        item_location loc = find_item_by_uid( u, uid );
        if( !loc ) {
            // The item is gone, or was never the player's. Saying so beats acting
            // on whatever happens to occupy that slot now.
            add_msg( m_bad, _( "That item is no longer available." ) );
            return;
        }
        switch( action ) {
            case item_action::wield:
                u.wield( loc );
                break;
            case item_action::wear:
                // Non-interactive: a prompt here would open a curses window from
                // underneath a Godot panel.
                u.wear( loc, /*interactive=*/false );
                break;
            case item_action::drop:
                u.drop( loc, u.pos_bub() );
                break;
        }
    } );
    return std::string();
}

std::string request_debug_spawn( const std::string &mtype )
{
    const mtype_id id( mtype );
    if( !id.is_valid() ) {
        return "No such monster id.";
    }
    post_game_command( [id]() {
        if( !g ) {
            return;
        }
        map &here = get_map();
        avatar &u = get_avatar();
        // First free adjacent tile. Adjacent because the point is to be able to
        // reach it: a monster across the room proves nothing about melee.
        for( const tripoint_bub_ms &p : here.points_in_radius( u.pos_bub( here ), 1 ) ) {
            if( p == u.pos_bub( here ) || !here.inbounds( p ) ) {
                continue;
            }
            if( here.impassable( p ) || get_creature_tracker().creature_at( p ) ) {
                continue;
            }
            if( g->place_critter_at( id, p ) ) {
                add_msg( m_debug, "spawned %s", id.str() );
                return;
            }
        }
        add_msg( m_debug, "no free tile next to the avatar for %s", id.str() );
    } );
    return std::string();
}

} // namespace godot_backend

#endif // GODOT
