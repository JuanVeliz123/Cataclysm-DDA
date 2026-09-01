#if defined(GODOT)

#include "godot_chargen.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

#include "avatar.h"
#include "bionics.h"
#include "calendar.h"
#include "cata_utility.h"
#include "character.h"
#include "character_creator_ui.h"
#include "city.h"
#include "enum_conversions.h"
#include "enums.h"
#include "game.h"
#include "game_constants.h"
#include "json.h"
#include "mapbuffer.h"
#include "mapsharing.h"
#include "mod_manager.h"
#include "mutation.h"
#include "options.h"
#include "output.h"
#include "overmapbuffer.h"
#include "player_difficulty.h"
#include "profession.h"
#include "rng.h"
#include "scenario.h"
#include "skill.h"
#include "start_location.h"
#include "string_formatter.h"
#include "translations.h"
#include "type_id.h"
#include "worldfactory.h"

// Friend of game; exposes private start_game() for confirm_chargen.
class godot_chargen_access
{
    public:
        static bool start_game() {
            return g->start_game();
        }
};

namespace godot_backend
{
namespace
{

character_creator_uistate g_cc;
std::atomic<bool> chargen_active{ false };

static const std::string flag_CITY_START( "CITY_START" );

chargen_result ok_result()
{
    return chargen_result{};
}

chargen_result ok_value( const std::string &value )
{
    chargen_result r;
    r.value = value;
    return r;
}

chargen_result ok_json( std::string json )
{
    chargen_result r;
    r.json = std::move( json );
    return r;
}

chargen_result err_result( const std::string &message )
{
    chargen_result r;
    r.ok = false;
    r.error = message;
    return r;
}

bool cities_enabled()
{
    if( world_generator == nullptr || world_generator->active_world == nullptr ) {
        return false;
    }
    options_manager::options_container &wopts = world_generator->active_world->WORLD_OPTIONS;
    return wopts["CITY_SIZE"].getValue() != "0";
}

bool require_active( chargen_result &out )
{
    if( !chargen_active ) {
        out = err_result( "Character creation is not active." );
        return false;
    }
    return true;
}

std::string trait_category( const mutation_branch &trait )
{
    if( trait.vanity ) {
        return "cosmetic";
    }
    if( trait.points > 0 ) {
        return "positive";
    }
    if( trait.points < 0 ) {
        return "negative";
    }
    return "neutral";
}

bool trait_is_locked( const avatar &u, const trait_id &trait )
{
    if( get_scenario()->is_locked_trait( trait ) || u.prof->is_locked_trait( trait ) ) {
        return true;
    }
    for( const profession *hobby : u.hobbies ) {
        if( hobby->is_locked_trait( trait ) ) {
            return true;
        }
    }
    return false;
}

std::string hobby_conflict_errors( const avatar &u, const profession &hobby )
{
    std::string errors;
    bool conflict_found = false;
    bool conflict_reason_found = false;
    for( const trait_and_var &new_trait : hobby.get_locked_traits() ) {
        if( u.has_conflicting_trait( new_trait.trait ) ) {
            conflict_found = true;
            for( const trait_and_var &suspect : u.prof->get_locked_traits() ) {
                if( are_conflicting_traits( new_trait.trait, suspect.trait ) ) {
                    conflict_reason_found = true;
                    if( !errors.empty() ) {
                        errors += "\n";
                    }
                    errors += string_format(
                                  _( "The trait [%1$s] conflicts with profession [%2$s]'s trait [%3$s]." ),
                                  new_trait.name(),
                                  u.prof->gender_appropriate_name( u.male ), suspect.name() );
                }
            }
            for( const profession *hby : u.hobbies ) {
                for( const trait_and_var &suspect : hby->get_locked_traits() ) {
                    if( are_conflicting_traits( new_trait.trait, suspect.trait ) ) {
                        conflict_reason_found = true;
                        if( !errors.empty() ) {
                            errors += "\n";
                        }
                        errors += string_format(
                                      _( "The trait [%1$s] conflicts with background [%2$s]'s trait [%3$s]." ),
                                      new_trait.name(),
                                      hby->gender_appropriate_name( u.male ), suspect.name() );
                    }
                }
            }
        }
    }
    if( conflict_found && !conflict_reason_found ) {
        if( !errors.empty() ) {
            errors += "\n";
        }
        errors += string_format( _( "A conflicting trait is preventing you from taking %s" ),
                                 hobby.gender_appropriate_name( u.male ) );
    }
    return errors;
}

void cleanup_chargen_session()
{
    get_avatar() = avatar();
    if( world_generator ) {
        world_generator->set_active_world( nullptr );
    }
    MAPBUFFER.clear();
    overmap_buffer.clear();
    chargen_active = false;
    g_cc.reset();
}

} // namespace

chargen_result create_world_default( const std::string &name )
{
    if( !world_generator ) {
        return err_result( "World generator is not available." );
    }
    world_generator->init();

    WORLD *world = nullptr;
    if( name.empty() ) {
        // Auto-name via WORLD constructor / make_new_world(false).
        world = world_generator->make_new_world( false );
    } else {
        world = world_generator->make_new_world(
                    name, world_generator->get_mod_manager().get_default_mods() );
    }
    if( world == nullptr ) {
        return err_result( name.empty() ? "Failed to create a new world."
                           : string_format( "Failed to create world '%s'.", name ) );
    }

    chargen_result r;
    r.value = world->world_name;
    std::ostringstream stream;
    JsonOut jsout( stream );
    jsout.start_object();
    jsout.member( "world_name", world->world_name );
    jsout.end_object();
    r.json = stream.str();
    return r;
}

bool world_has_saves( const std::string &world_name )
{
    if( !world_generator ) {
        return false;
    }
    WORLD *world = world_generator->get_world( world_name );
    return world != nullptr && !world->world_saves.empty();
}

chargen_result begin_custom_chargen( const std::string &world_name )
{
    if( chargen_active ) {
        return err_result( "Character creation is already active." );
    }
    if( !world_generator ) {
        return err_result( "World generator is not available." );
    }
    if( !g ) {
        return err_result( "Game is not available." );
    }

    WORLD *world = world_generator->get_world( world_name );
    if( world == nullptr ) {
        return err_result( string_format( "World '%s' not found.", world_name ) );
    }

    world_generator->set_active_world( world );
    try {
        g->setup();
    } catch( const std::exception &err ) {
        cleanup_chargen_session();
        return err_result( string_format( "Failed to set up game: %s", err.what() ) );
    }

    avatar &u = get_avatar();
    u = avatar();
    u.prepare_custom_chargen();
    g_cc.reset();
    g_cc.generation_type = character_type::CUSTOM;
    chargen_active = true;
    return ok_value( world_name );
}

chargen_result cancel_chargen()
{
    if( !chargen_active ) {
        return err_result( "Character creation is not active." );
    }
    cleanup_chargen_session();
    return ok_result();
}

chargen_result confirm_chargen()
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }
    if( !g ) {
        return err_result( "Game is not available." );
    }

    avatar &u = get_avatar();
    if( u.name.empty() ) {
        u.pick_name();
    }

    // Exit chargen mode before starting the game; restore on failure.
    chargen_active = false;
    u.finalize_custom_chargen();

    if( !godot_chargen_access::start_game() ) {
        chargen_active = true;
        return err_result( "Failed to start the game." );
    }

    g_cc.reset();
    return ok_result();
}

bool is_chargen_active()
{
    return chargen_active;
}

chargen_result get_state()
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    avatar &u = get_avatar();
    g_cc.recalc_rating = true;

    std::ostringstream stream;
    JsonOut jsout( stream, true );
    jsout.start_object();
    jsout.member( "name", u.name );
    jsout.member( "male", u.male );
    jsout.member( "outfit", get_chargen_outfit() );
    jsout.member( "age", u.base_age() );
    jsout.member( "height", u.base_height() );
    jsout.member( "blood", io::enum_to_string( u.my_blood_type ) + ( u.blood_rh_factor ? "+" : "-" ) );

    const scenario *scen = get_scenario();
    jsout.member( "scenario_id", scen ? scen->ident().str() : "" );
    jsout.member( "scenario_name", scen ? scen->gender_appropriate_name( u.male ) : "" );
    jsout.member( "profession_id", u.prof ? u.prof->ident().str() : "" );
    jsout.member( "profession_name",
                  u.prof ? u.prof->gender_appropriate_name( u.male ) : "" );

    jsout.member( "hobbies" );
    jsout.start_array();
    for( const profession *hobby : u.hobbies ) {
        jsout.start_object();
        jsout.member( "id", hobby->ident().str() );
        jsout.member( "name", hobby->gender_appropriate_name( u.male ) );
        jsout.end_object();
    }
    jsout.end_array();

    jsout.member( "str", u.get_str_base() );
    jsout.member( "dex", u.get_dex_base() );
    jsout.member( "int", u.get_int_base() );
    jsout.member( "per", u.get_per_base() );

    jsout.member( "traits" );
    jsout.start_array();
    for( const trait_id &trait : u.get_mutations() ) {
        jsout.write( trait.str() );
    }
    jsout.end_array();

    jsout.member( "skills" );
    jsout.start_object();
    g_cc.recalc_skill_list();
    for( const Skill *sk : g_cc.sorted_skills ) {
        const int level = u.get_skill_level( sk->ident() );
        if( level > 0 ) {
            jsout.member( sk->ident().str(), level );
        }
    }
    jsout.end_object();

    jsout.member( "random_start_location", u.random_start_location );
    jsout.member( "start_location_id",
                  u.random_start_location ? "random" : u.start_location.str() );
    std::string start_loc_name;
    if( u.random_start_location ) {
        start_loc_name = _( "Random location" );
    } else if( u.start_location.is_valid() ) {
        start_loc_name = u.start_location.obj().name();
    }
    jsout.member( "start_location_name", start_loc_name );

    jsout.member( "starting_city",
                  u.starting_city.has_value() ? u.starting_city->name : "" );
    jsout.member( "cataclysm_start",
                  scen ? to_string( scen->start_of_cataclysm() ) : "" );
    jsout.member( "game_start",
                  scen ? to_string( scen->start_of_game() ) : "" );
    jsout.member( "rating",
                  player_difficulty::getInstance().difficulty_to_string( u ) );
    jsout.member( "cities_enabled", cities_enabled() );
    jsout.end_object();

    return ok_json( stream.str() );
}

chargen_result list_scenarios()
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    avatar &u = get_avatar();
    g_cc.recalc_scenarios = true;
    g_cc.recalc_scenario_list( u );

    std::ostringstream stream;
    JsonOut jsout( stream );
    jsout.start_array();
    for( const scenario *scen : g_cc.sorted_scenarios ) {
        const ret_val<void> can_pick = scen->can_pick();
        const bool city_ok = !scen->has_flag( flag_CITY_START ) || cities_enabled();
        jsout.start_object();
        jsout.member( "id", scen->ident().str() );
        jsout.member( "name", scen->gender_appropriate_name( u.male ) );
        jsout.member( "description", scen->description( u.male ) );
        jsout.member( "points", scen->point_cost() );
        jsout.member( "enabled", can_pick.success() && city_ok );
        jsout.member( "reason", can_pick.success()
                      ? ( city_ok ? "" : _( "Cities are disabled in this world." ) )
                      : can_pick.str() );
        jsout.member( "taken", scen == get_scenario() );
        jsout.end_object();
    }
    jsout.end_array();
    return ok_json( stream.str() );
}

chargen_result list_professions()
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    avatar &u = get_avatar();
    g_cc.recalc_professions = true;
    g_cc.recalc_profession_list( u );

    std::ostringstream stream;
    JsonOut jsout( stream );
    jsout.start_array();
    for( const profession_id &prof_id : g_cc.sorted_professions ) {
        const profession &prof = prof_id.obj();
        const ret_val<void> can_pick = prof.can_pick();
        jsout.start_object();
        jsout.member( "id", prof_id.str() );
        jsout.member( "name", prof.gender_appropriate_name( u.male ) );
        jsout.member( "description", prof.description( u.male ) );
        jsout.member( "points", prof.point_cost() );
        jsout.member( "enabled", can_pick.success() );
        jsout.member( "reason", can_pick.success() ? "" : can_pick.str() );
        jsout.member( "taken", u.prof && u.prof->ident() == prof_id );
        jsout.end_object();
    }
    jsout.end_array();
    return ok_json( stream.str() );
}

chargen_result list_hobbies()
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    avatar &u = get_avatar();
    g_cc.recalc_hobbies = true;
    g_cc.recalc_hobby_list( u );

    std::ostringstream stream;
    JsonOut jsout( stream );
    jsout.start_array();
    for( const profession_id &hobby_id : g_cc.sorted_hobbies ) {
        const profession &hobby = hobby_id.obj();
        jsout.start_object();
        jsout.member( "id", hobby_id.str() );
        jsout.member( "name", hobby.gender_appropriate_name( u.male ) );
        jsout.member( "description", hobby.description( u.male ) );
        jsout.member( "points", hobby.point_cost() );
        jsout.member( "taken", u.hobbies.count( &hobby ) != 0 );
        jsout.end_object();
    }
    jsout.end_array();
    return ok_json( stream.str() );
}

chargen_result list_traits()
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    avatar &u = get_avatar();
    g_cc.recalc_traits = true;
    g_cc.recalc_trait_list( u );

    std::ostringstream stream;
    JsonOut jsout( stream );
    jsout.start_array();
    for( const trait_id &trait : g_cc.sorted_traits ) {
        const mutation_branch &data = trait.obj();
        const bool taken = u.has_trait( trait );
        const bool locked = trait_is_locked( u, trait );
        const bool forbidden = get_scenario()->is_forbidden_trait( trait ) ||
                               u.prof->is_forbidden_trait( trait );
        const bool conflicting = u.has_conflicting_trait( trait );
        jsout.start_object();
        jsout.member( "id", trait.str() );
        jsout.member( "name", data.name() );
        jsout.member( "description", data.desc() );
        jsout.member( "points", data.points );
        jsout.member( "category", trait_category( data ) );
        jsout.member( "taken", taken );
        jsout.member( "locked", locked );
        jsout.member( "enabled", taken || ( !forbidden && !conflicting ) );
        jsout.end_object();
    }
    jsout.end_array();
    return ok_json( stream.str() );
}

chargen_result list_skills()
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    avatar &u = get_avatar();
    g_cc.recalc_skills = true;
    g_cc.recalc_skill_list();

    std::ostringstream stream;
    JsonOut jsout( stream );
    jsout.start_array();
    for( const Skill *sk : g_cc.sorted_skills ) {
        jsout.start_object();
        jsout.member( "id", sk->ident().str() );
        jsout.member( "name", sk->name() );
        jsout.member( "description", sk->description() );
        jsout.member( "category", sk->display_category()->display_string() );
        jsout.member( "level", static_cast<int>( u.get_skill_level( sk->ident() ) ) );
        jsout.end_object();
    }
    jsout.end_array();
    return ok_json( stream.str() );
}

chargen_result list_start_locations()
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    avatar &u = get_avatar();
    const scenario *scen = get_scenario();

    std::ostringstream stream;
    JsonOut jsout( stream );
    jsout.start_array();

    jsout.start_object();
    jsout.member( "id", "random" );
    jsout.member( "name", _( "Random location" ) );
    jsout.member( "description",
                  string_format( n_gettext( "%d variant", "%d variants",
                                            scen->start_location_targets_count() ),
                                 scen->start_location_targets_count() ) );
    jsout.member( "taken", u.random_start_location );
    jsout.end_object();

    for( const start_location &loc : start_locations::get_all() ) {
        if( !scen->allowed_start( loc.id ) ) {
            continue;
        }
        jsout.start_object();
        jsout.member( "id", loc.id.str() );
        jsout.member( "name", loc.name() );
        jsout.member( "description",
                      loc.targets_count() > 1
                      ? string_format( n_gettext( "%d variant", "%d variants", loc.targets_count() ),
                                       loc.targets_count() )
                      : "" );
        jsout.member( "taken", !u.random_start_location && u.start_location == loc.id );
        jsout.end_object();
    }
    jsout.end_array();
    return ok_json( stream.str() );
}

chargen_result list_cities()
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    avatar &u = get_avatar();
    std::vector<city> cities( city::get_all() );
    const auto cities_cmp_population = []( const city & a, const city & b ) {
        return std::tie( a.population, a.name ) > std::tie( b.population, b.name );
    };
    std::sort( cities.begin(), cities.end(), cities_cmp_population );

    std::ostringstream stream;
    JsonOut jsout( stream );
    jsout.start_array();
    for( const city &c : cities ) {
        jsout.start_object();
        jsout.member( "id", c.id.str() );
        jsout.member( "name", c.name );
        jsout.member( "population", c.population );
        jsout.member( "size", c.size );
        jsout.member( "taken", u.starting_city.has_value() && *u.starting_city == c );
        jsout.end_object();
    }
    jsout.end_array();
    return ok_json( stream.str() );
}

chargen_result set_scenario( const std::string &id )
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    const string_id<scenario> scen_id( id );
    if( !scen_id.is_valid() ) {
        return err_result( string_format( "Unknown scenario '%s'.", id ) );
    }
    const scenario *selected = &scen_id.obj();
    const ret_val<void> can_pick = selected->can_pick();
    if( !can_pick.success() ) {
        return err_result( can_pick.str() );
    }
    if( selected->has_flag( flag_CITY_START ) && !cities_enabled() ) {
        return err_result( _( "Cities are disabled in this world." ) );
    }

    avatar &u = get_avatar();
    reset_scenario( u, selected );
    g_cc.recalc_professions = true;
    g_cc.recalc_hobbies = true;
    g_cc.recalc_traits = true;
    g_cc.recalc_rating = true;
    return ok_result();
}

chargen_result set_profession( const std::string &id )
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    const profession_id prof_id( id );
    if( !prof_id.is_valid() ) {
        return err_result( string_format( "Unknown profession '%s'.", id ) );
    }

    avatar &u = get_avatar();
    const ret_val<void> can_pick = prof_id->can_pick();
    if( !can_pick.success() ) {
        return err_result( can_pick.str() );
    }

    for( const trait_and_var &old : u.prof->get_locked_traits() ) {
        u.toggle_trait_deps( old.trait );
    }

    u.prof = &*prof_id;

    for( const trait_and_var &new_trait : prof_id->get_locked_traits() ) {
        if( u.has_conflicting_trait( new_trait.trait ) ) {
            for( const trait_id &suspect_trait : u.get_mutations() ) {
                if( are_conflicting_traits( new_trait.trait, suspect_trait ) ) {
                    u.toggle_trait_deps( suspect_trait );
                }
            }
        }
    }
    u.add_traits();

    g_cc.recalc_hobbies = true;
    g_cc.recalc_traits = true;
    g_cc.recalc_rating = true;
    g_cc.cached_profession_inventory.clear();
    return ok_result();
}

chargen_result toggle_hobby( const std::string &id )
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    const profession_id hobby_id( id );
    if( !hobby_id.is_valid() ) {
        return err_result( string_format( "Unknown hobby '%s'.", id ) );
    }

    avatar &u = get_avatar();
    const profession *selected_hobby = &*hobby_id;
    const bool enabling = u.hobbies.count( selected_hobby ) == 0;

    if( enabling ) {
        const std::string conflicts = hobby_conflict_errors( u, *selected_hobby );
        if( !conflicts.empty() ) {
            return err_result( conflicts );
        }
        u.hobbies.insert( selected_hobby );
    } else {
        u.hobbies.erase( selected_hobby );
    }

    for( const trait_and_var &cur : selected_hobby->get_locked_traits() ) {
        const trait_id &trait = cur.trait;
        if( enabling ) {
            if( !u.has_trait( trait ) ) {
                u.toggle_trait_deps( trait );
            }
            continue;
        }
        int from_other_hobbies = u.prof->is_locked_trait( trait ) ? 1 : 0;
        for( const profession *hby : u.hobbies ) {
            if( hby->ident() != selected_hobby->ident() && hby->is_locked_trait( trait ) ) {
                from_other_hobbies++;
            }
        }
        if( from_other_hobbies > 0 ) {
            continue;
        }
        u.toggle_trait_deps( trait );
    }

    g_cc.recalc_traits = true;
    g_cc.recalc_hobbies_taken = true;
    g_cc.recalc_rating = true;
    return ok_result();
}

chargen_result set_stat( const std::string &stat_name, int value )
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    const int clamped = std::clamp( value, CHARACTER_STAT_MIN, CHARACTER_STAT_MAX );
    avatar &u = get_avatar();
    if( stat_name == "str" || stat_name == "strength" ) {
        u.set_str_base( clamped );
    } else if( stat_name == "dex" || stat_name == "dexterity" ) {
        u.set_dex_base( clamped );
    } else if( stat_name == "int" || stat_name == "intelligence" ) {
        u.set_int_base( clamped );
    } else if( stat_name == "per" || stat_name == "perception" ) {
        u.set_per_base( clamped );
    } else {
        return err_result( string_format( "Unknown stat '%s'.", stat_name ) );
    }
    g_cc.recalc_rating = true;
    return ok_result();
}

chargen_result toggle_trait( const std::string &id )
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    const trait_id cur_trait( id );
    if( !cur_trait.is_valid() ) {
        return err_result( string_format( "Unknown trait '%s'.", id ) );
    }

    avatar &u = get_avatar();
    int inc_type = 0;
    std::string variant;

    std::vector<bionic_id> cbms_blocking_trait = bionics_cancelling_trait( u.prof->CBMs(),
            cur_trait );
    const std::unordered_set<trait_id> conflicting_traits = u.get_conflicting_traits( cur_trait );

    if( u.has_trait( cur_trait ) ) {
        if( !cur_trait->variants.empty() ) {
            // No variant menu: just remove.
            inc_type = -1;
        } else {
            inc_type = -1;
            if( get_scenario()->is_locked_trait( cur_trait ) ) {
                return err_result( string_format(
                                       _( "Your scenario of %s prevents you from removing this trait." ),
                                       get_scenario()->gender_appropriate_name( u.male ) ) );
            }
            if( u.prof->is_locked_trait( cur_trait ) ) {
                return err_result( string_format(
                                       _( "Your profession of %s prevents you from removing this trait." ),
                                       u.prof->gender_appropriate_name( u.male ) ) );
            }
            for( const profession *hobby : u.hobbies ) {
                if( hobby->is_locked_trait( cur_trait ) ) {
                    return err_result( string_format(
                                           _( "Your background of %s prevents you from removing this trait." ),
                                           hobby->gender_appropriate_name( u.male ) ) );
                }
            }
        }
    } else if( !conflicting_traits.empty() ) {
        std::vector<std::string> conflict_names;
        conflict_names.reserve( conflicting_traits.size() );
        for( const trait_id &trait : conflicting_traits ) {
            conflict_names.emplace_back( u.mutation_name( trait ) );
        }
        return err_result( string_format( _( "You already picked some conflicting traits: %s." ),
                                          enumerate_as_string( conflict_names ) ) );
    } else if( get_scenario()->is_forbidden_trait( cur_trait ) ) {
        return err_result( _( "The scenario you picked prevents you from taking this trait!" ) );
    } else if( u.prof->is_forbidden_trait( cur_trait ) ) {
        return err_result( string_format(
                               _( "Your profession of %s prevents you from taking this trait." ),
                               u.prof->gender_appropriate_name( u.male ) ) );
    } else if( !cbms_blocking_trait.empty() ) {
        std::vector<std::string> conflict_names;
        conflict_names.reserve( cbms_blocking_trait.size() );
        for( const bionic_id &conflict : cbms_blocking_trait ) {
            conflict_names.emplace_back( conflict->name.translated() );
        }
        return err_result( string_format(
                               _( "The following bionics prevent you from taking this trait: %s." ),
                               enumerate_as_string( conflict_names ) ) );
    } else {
        inc_type = 1;
        if( !cur_trait->variants.empty() ) {
            // No variant menu: empty variant, else first variant id.
            variant = cur_trait->variants.begin()->first;
        }
    }

    if( inc_type != 0 ) {
        u.toggle_trait_deps( cur_trait, variant );
    }
    g_cc.recalc_rating = true;
    return ok_result();
}

chargen_result set_skill( const std::string &id, int level )
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    const skill_id sk( id );
    if( !sk.is_valid() ) {
        return err_result( string_format( "Unknown skill '%s'.", id ) );
    }

    avatar &u = get_avatar();
    const int clamped = std::clamp( level, MIN_SKILL, MAX_SKILL );
    u.set_skill_level( sk, clamped );
    u.set_knowledge_level( sk, clamped );
    g_cc.recalc_rating = true;
    return ok_result();
}

chargen_result set_name( const std::string &name )
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }
    if( MAP_SHARING::isSharing() ) {
        return err_result( "Name cannot be changed while map sharing." );
    }
    avatar &u = get_avatar();
    u.name = name.substr( 0, NAME_CHARACTER_LIMIT );
    return ok_result();
}

chargen_result set_gender( bool male )
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }
    get_avatar().male = male;
    g_cc.recalc_rating = true;
    return ok_result();
}

chargen_result set_outfit( bool outfit )
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }
    set_chargen_outfit( outfit );
    g_cc.cached_profession_inventory.clear();
    return ok_result();
}

chargen_result set_age( int age )
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }
    get_avatar().set_base_age( std::clamp( age, CHARACTER_AGE_MIN, CHARACTER_AGE_MAX ) );
    return ok_result();
}

chargen_result set_height( int height_cm )
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }
    const int min_h = Character::min_height();
    const int max_h = Character::max_height();
    get_avatar().set_base_height( std::clamp( height_cm, min_h, max_h ) );
    return ok_result();
}

chargen_result cycle_blood_type()
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    avatar &u = get_avatar();
    if( !u.blood_rh_factor ) {
        u.blood_rh_factor = true;
    } else {
        if( static_cast<blood_type>( static_cast<int>( u.my_blood_type ) + 1 ) <
            blood_type::blood_acid ) {
            u.my_blood_type = static_cast<blood_type>( static_cast<int>( u.my_blood_type ) + 1 );
            u.blood_rh_factor = false;
        } else {
            u.my_blood_type = static_cast<blood_type>( 0 );
            u.blood_rh_factor = false;
        }
    }
    return ok_value( io::enum_to_string( u.my_blood_type ) + ( u.blood_rh_factor ? "+" : "-" ) );
}

chargen_result set_start_location( const std::string &id )
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    avatar &u = get_avatar();
    if( id.empty() || id == "random" ) {
        u.random_start_location = true;
        return ok_result();
    }

    const start_location_id loc_id( id );
    if( !loc_id.is_valid() ) {
        return err_result( string_format( "Unknown start location '%s'.", id ) );
    }
    if( !get_scenario()->allowed_start( loc_id ) ) {
        return err_result( string_format( "Start location '%s' is not allowed for this scenario.",
                                          id ) );
    }
    u.random_start_location = false;
    u.start_location = loc_id;
    return ok_result();
}

chargen_result set_starting_city( const std::string &name )
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    avatar &u = get_avatar();
    if( name.empty() ) {
        u.starting_city = std::nullopt;
        u.world_origin = std::nullopt;
        return ok_result();
    }

    for( const city &c : city::get_all() ) {
        if( c.name == name || c.id.str() == name ) {
            u.starting_city = c;
            u.world_origin = c.pos_om;
            return ok_result();
        }
    }
    return err_result( string_format( "City '%s' not found.", name ) );
}

chargen_result reset_calendar()
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }
    get_scenario()->reset_calendar();
    return ok_result();
}

chargen_result nudge_calendar( const std::string &which, int hours )
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    const scenario *scen = get_scenario();
    const time_duration delta = time_duration::from_hours( hours );
    if( which == "cataclysm" ) {
        scen->change_start_of_cataclysm( scen->start_of_cataclysm() + delta );
    } else if( which == "game" ) {
        scen->change_start_of_game( scen->start_of_game() + delta );
    } else {
        return err_result( string_format( "Unknown calendar field '%s'.", which ) );
    }
    return ok_result();
}

chargen_result randomize_name()
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }
    if( MAP_SHARING::isSharing() ) {
        return err_result( "Name cannot be changed while map sharing." );
    }
    avatar &u = get_avatar();
    u.pick_name();
    return ok_value( u.name );
}

chargen_result randomize_description()
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }

    avatar &u = get_avatar();
    const bool gender_selection = one_in( 2 );
    u.male = gender_selection;
    if( one_in( 10 ) ) {
        set_chargen_outfit( !gender_selection );
    } else {
        set_chargen_outfit( gender_selection );
    }
    if( !MAP_SHARING::isSharing() ) {
        u.pick_name();
    }
    u.set_base_age( rng( CHARACTER_AGE_MIN, CHARACTER_AGE_MAX ) );
    u.randomize_height();
    u.randomize_blood();
    u.randomize_heartrate();
    g_cc.recalc_rating = true;
    return ok_result();
}

chargen_result save_template( const std::string &name )
{
    chargen_result gate;
    if( !require_active( gate ) ) {
        return gate;
    }
    if( name.empty() ) {
        return err_result( "Template name must not be empty." );
    }
    get_avatar().save_template( name, pool_type::FREEFORM );
    return ok_value( name );
}

} // namespace godot_backend

#endif // defined(GODOT)
