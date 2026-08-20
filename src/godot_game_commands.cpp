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
#include "npc.h"
#include "overmapbuffer.h"
#include "calendar.h"
#include "condition.h"
#include "coordinates.h"
#include "dialogue.h"
#include "effect_on_condition.h"
#include "field_type.h"
#include "godot_hud_snapshot.h"
#include "godot_map_snapshot.h"
#include "godot_pixel_minimap.h"
#include "omdata.h"
#include "talker.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "weather.h"
#include "weather_type.h"

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
        // Adjacent preferred, because the point is to be able to reach it: a
        // monster across the room proves nothing about melee. But an avatar on
        // a staircase can be walled in on all eight sides, and a spawn that
        // quietly fails is a fixture that silently tests nothing -- widen until
        // somewhere works, exactly as the NPC spawn learned to.
        for( int radius = 1; radius <= 4; ++radius ) {
            for( const tripoint_bub_ms &p : here.points_in_radius( u.pos_bub( here ), radius ) ) {
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
        }
        add_msg( m_debug, "no free tile near the avatar for %s", id.str() );
    } );
    return std::string();
}

std::string request_debug_spawn_npc()
{
    post_game_command( []() {
        if( !g ) {
            return;
        }
        map &here = get_map();
        avatar &u = get_avatar();
        shared_ptr_fast<npc> temp = make_shared_fast<npc>();
        temp->normalize();
        temp->randomize();

        // Adjacent, unlike the debug menu's -4,-4: the point is that a fixture
        // can walk into them and get the interaction menu on the next step.
        tripoint_bub_ms where = u.pos_bub( here );
        bool placed = false;
        // Adjacent is preferred -- the caller wants to reach them in one step --
        // but a character standing in a shelter can be walled in on all eight
        // sides, and a spawn that quietly fails is a fixture that silently tests
        // nothing. Widen until somewhere works.
        for( int radius = 1; radius <= 4 && !placed; ++radius ) {
            for( const tripoint_bub_ms &p : here.points_in_radius( u.pos_bub( here ), radius ) ) {
                if( p == u.pos_bub( here ) || !here.inbounds( p ) ) {
                    continue;
                }
                if( here.impassable( p ) || get_creature_tracker().creature_at( p ) ) {
                    continue;
                }
                where = p;
                placed = true;
                break;
            }
        }
        if( !placed ) {
            add_msg( m_debug, "no free tile next to the avatar for an NPC" );
            return;
        }
        temp->spawn_at_precise( here.get_abs( where ) );
        overmap_buffer.insert_npc( temp );
        temp->form_opinion( u );
        temp->mission = NPC_MISSION_NULL;
        g->load_npcs();
        add_msg( m_debug, "spawned npc %s", temp->name );
    } );
    return std::string();
}

// --- The scenario harness (VER-2 item 1) ---

namespace
{

std::mutex g_scenario_mutex;
scenario_status g_scenario;

/// Record how a scenario command came out, for the fixture to poll. "Accepted"
/// was said on the calling thread; this is the game thread saying "done", which
/// for a teleport-by-search is the first moment failure is even knowable.
void scenario_done( const std::string &what, bool ok, const std::string &detail )
{
    std::lock_guard<std::mutex> lock( g_scenario_mutex );
    ++g_scenario.generation;
    g_scenario.ok = ok;
    g_scenario.last = what;
    g_scenario.detail = detail;
    add_msg( m_debug, "scenario %s: %s", what, detail );
}

/// Make a mutation visible. Commands run at the input wait, where nothing
/// republishes the world until the next player action -- and the published
/// lighting reads the map cache, which the game only rebuilds on changes it
/// made itself. Without this a fire lit by a command publishes the light of
/// the tile before the fire, and a time change publishes yesterday's sun.
void republish_world()
{
    map &here = get_map();
    const int z = here.get_abs_sub().z();
    here.invalidate_map_cache( z );
    here.build_map_cache( z );
    update_map_snapshot();
    update_hud_snapshot();
    update_pixel_minimap();
}

} // namespace

std::string request_scenario_teleport_omt( const std::string &omt_type, int search_range )
{
    if( omt_type.empty() ) {
        return "Empty overmap terrain type.";
    }
    const int range = std::max( 1, search_range );
    post_game_command( [omt_type, range]() {
        if( !g ) {
            return;
        }
        avatar &u = get_avatar();
        omt_find_params params;
        params.types = { { omt_type, ot_match_type::prefix } };
        params.search_range = range;
        const tripoint_abs_omt found = overmap_buffer.find_closest( u.pos_abs_omt(), params );
        if( found == tripoint_abs_omt::invalid ) {
            scenario_done( "teleport_omt", false,
                           "no '" + omt_type + "' within " + std::to_string( range ) + " OMTs" );
            return;
        }
        g->place_player_overmap( found );
        scenario_done( "teleport_omt", true, omt_type + " found" );
        republish_world();
    } );
    return std::string();
}

std::string request_scenario_teleport_rel( int dx, int dy, int dz )
{
    post_game_command( [dx, dy, dz]() {
        if( !g ) {
            return;
        }
        avatar &u = get_avatar();
        map &here = get_map();
        const tripoint_bub_ms dest = u.pos_bub( here ) + tripoint( dx, dy, dz );
        // place_player handles the z change itself -- it calls vertical_shift
        // for a dest on another level -- so the whole move is one call.
        g->place_player( dest );
        scenario_done( "teleport_rel", true, "moved" );
        republish_world();
    } );
    return std::string();
}

std::string request_scenario_stand_on( const std::string &flag, int radius )
{
    if( flag.empty() ) {
        return "Empty terrain flag.";
    }
    const int r = std::clamp( radius, 1, 60 );
    post_game_command( [flag, r]() {
        if( !g ) {
            return;
        }
        avatar &u = get_avatar();
        map &here = get_map();
        for( const tripoint_bub_ms &p : here.points_in_radius( u.pos_bub( here ), r ) ) {
            if( !here.inbounds( p ) || !here.has_flag( flag, p ) ) {
                continue;
            }
            g->place_player( p );
            scenario_done( "stand_on", true, flag + " at " + std::to_string( p.x() ) +
                           "," + std::to_string( p.y() ) );
            republish_world();
            return;
        }
        scenario_done( "stand_on", false,
                       "no " + flag + " within " + std::to_string( r ) + " tiles" );
    } );
    return std::string();
}

std::string request_scenario_set_time( int hour, int minute )
{
    if( hour < 0 || hour > 23 || minute < 0 || minute > 59 ) {
        return "Time out of range.";
    }
    post_game_command( [hour, minute]() {
        if( !g ) {
            return;
        }
        // Only ever forward: timed events fire on catch-up, and replaying the
        // ones between the new time and now is worse than waiting a day.
        const time_duration target = time_duration::from_hours( hour ) +
                                     time_duration::from_minutes( minute );
        time_duration ahead = target - time_past_midnight( calendar::turn );
        if( ahead < 0_seconds ) {
            ahead += 1_days;
        }
        calendar::turn += ahead;
        get_weather().set_nextweather( calendar::turn );
        scenario_done( "set_time", true, std::to_string( hour ) + ":" + std::to_string( minute ) );
        republish_world();
    } );
    return std::string();
}

std::string request_scenario_set_weather( const std::string &weather_id )
{
    const weather_type_id id( weather_id );
    if( !weather_id.empty() && !id.is_valid() ) {
        return "No such weather id.";
    }
    post_game_command( [id, weather_id]() {
        if( !g ) {
            return;
        }
        weather_manager &weather = get_weather();
        // The debug menu's non-UI tail, EOCs included: portal storms and their
        // kin do their work through these, and skipping them forces a weather
        // whose effects never arrive.
        if( weather.weather_id->debug_leave_eoc.has_value() ) {
            dialogue d( get_talker_for( get_avatar() ), nullptr );
            effect_on_condition_id( weather.weather_id->debug_leave_eoc.value() )->activate( d );
        }
        if( !weather_id.empty() && id->debug_cause_eoc.has_value() ) {
            dialogue d( get_talker_for( get_avatar() ), nullptr );
            effect_on_condition_id( id->debug_cause_eoc.value() )->activate( d );
        }
        weather.weather_override = weather_id.empty() ? WEATHER_NULL : id;
        weather.set_nextweather( calendar::turn );
        // The debug menu leaves this to the next do_turn; a fixture wants the
        // change in the very snapshot republish_world is about to publish.
        weather.update_weather();
        scenario_done( "set_weather", true, weather_id.empty() ? "cleared" : weather_id );
        republish_world();
    } );
    return std::string();
}

std::string request_scenario_spawn_field( const std::string &field_id, int intensity,
        int dx, int dy )
{
    const field_type_id id( field_id );
    if( !id.is_valid() ) {
        return "No such field id.";
    }
    const int level = std::clamp( intensity, 1, 9 );
    post_game_command( [id, field_id, level, dx, dy]() {
        if( !g ) {
            return;
        }
        map &here = get_map();
        const tripoint_bub_ms p = get_avatar().pos_bub( here ) + tripoint( dx, dy, 0 );
        if( !here.inbounds( p ) ) {
            scenario_done( "spawn_field", false, "out of bounds" );
            return;
        }
        here.add_field( p, id, level );
        scenario_done( "spawn_field", true, field_id );
        republish_world();
    } );
    return std::string();
}

std::string request_scenario_spawn_vehicle( const std::string &vproto, int dx, int dy )
{
    const vproto_id id( vproto );
    if( !id.is_valid() ) {
        return "No such vehicle prototype.";
    }
    post_game_command( [id, dx, dy]() {
        if( !g ) {
            return;
        }
        map &here = get_map();
        const tripoint_bub_ms p = get_avatar().pos_bub( here ) + tripoint( dx, dy, 0 );
        if( !here.inbounds( p ) ) {
            scenario_done( "spawn_vehicle", false, "out of bounds" );
            return;
        }
        vehicle *veh = here.add_vehicle( id, p, -90_degrees, 100,
                                         veh_spawn_status::UNDAMAGED );
        if( veh == nullptr ) {
            scenario_done( "spawn_vehicle", false, "no room for " + id.str() );
            return;
        }
        // Lights on. The whole point of a scenario vehicle is headlight cones
        // at night, and vehicle::lights() only reports parts that are enabled.
        int lit = 0;
        for( const vpart_bitflags flag : { VPFLAG_CONE_LIGHT, VPFLAG_WIDE_CONE_LIGHT,
                                           VPFLAG_CIRCLE_LIGHT, VPFLAG_HALF_CIRCLE_LIGHT
                                         } ) {
            for( const vpart_reference &vp : veh->get_avail_parts( flag ) ) {
                vp.part().enabled = true;
                ++lit;
            }
        }
        veh->refresh();
        scenario_done( "spawn_vehicle", true,
                       id.str() + ", " + std::to_string( lit ) + " lights on" );
        republish_world();
    } );
    return std::string();
}

std::string request_scenario_spawn_item( const std::string &itype, int dx, int dy )
{
    const itype_id id( itype );
    if( !id.is_valid() ) {
        return "No such item type.";
    }
    post_game_command( [id, dx, dy]() {
        if( !g ) {
            return;
        }
        map &here = get_map();
        const tripoint_bub_ms p = get_avatar().pos_bub( here ) + tripoint( dx, dy, 0 );
        if( !here.inbounds( p ) ) {
            scenario_done( "spawn_item", false, "out of bounds" );
            return;
        }
        here.spawn_item( p, id, 1 );
        scenario_done( "spawn_item", true, id.str() );
        republish_world();
    } );
    return std::string();
}

std::string request_scenario_spawn_furniture( const std::string &furn, int dx, int dy )
{
    const furn_str_id id( furn );
    if( !id.is_valid() ) {
        return "No such furniture.";
    }
    post_game_command( [id, dx, dy]() {
        if( !g ) {
            return;
        }
        map &here = get_map();
        const tripoint_bub_ms p = get_avatar().pos_bub( here ) + tripoint( dx, dy, 0 );
        if( !here.inbounds( p ) ) {
            scenario_done( "spawn_furniture", false, "out of bounds" );
            return;
        }
        here.furn_set( p, id );
        scenario_done( "spawn_furniture", true, id.str() );
        republish_world();
    } );
    return std::string();
}

std::string request_scenario_set_avatar_sex( bool male )
{
    post_game_command( [male]() {
        if( !g ) {
            return;
        }
        get_avatar().male = male;
        scenario_done( "set_avatar_sex", true, male ? "male" : "female" );
        republish_world();
    } );
    return std::string();
}

scenario_status get_scenario_status()
{
    std::lock_guard<std::mutex> lock( g_scenario_mutex );
    return g_scenario;
}

} // namespace godot_backend

#endif // GODOT
