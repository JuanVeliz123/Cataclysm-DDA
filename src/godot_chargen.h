#pragma once
#ifndef CATA_SRC_GODOT_CHARGEN_H
#define CATA_SRC_GODOT_CHARGEN_H

#if defined(GODOT)

#include <string>

namespace godot_backend
{

struct chargen_result {
    bool ok = true;
    std::string error;   // human message on failure
    std::string json;    // JSON payload for lists/state (empty if N/A)
    std::string value;   // simple string result when useful
};

// All APIs run on the CDDA game thread. Do not call Godot APIs from these.

// World
chargen_result create_world_default( const std::string &name );
bool world_has_saves( const std::string &world_name );

// Session
chargen_result begin_custom_chargen( const std::string &world_name );
chargen_result cancel_chargen();
chargen_result confirm_chargen();
bool is_chargen_active();

// State / lists (JSON)
chargen_result get_state();
chargen_result list_scenarios();
chargen_result list_professions();
chargen_result list_hobbies();
chargen_result list_traits();
chargen_result list_skills();
chargen_result list_start_locations();
chargen_result list_cities();

// Mutators
chargen_result set_scenario( const std::string &id );
chargen_result set_profession( const std::string &id );
chargen_result toggle_hobby( const std::string &id );
chargen_result set_stat( const std::string &stat_name, int value );
chargen_result toggle_trait( const std::string &id );
chargen_result set_skill( const std::string &id, int level );
chargen_result set_name( const std::string &name );
chargen_result set_gender( bool male );
chargen_result set_outfit( bool outfit );
chargen_result set_age( int age );
chargen_result set_height( int height_cm );
chargen_result cycle_blood_type();
chargen_result set_start_location( const std::string &id );
chargen_result set_starting_city( const std::string &name );
chargen_result reset_calendar();
chargen_result nudge_calendar( const std::string &which, int hours );
chargen_result randomize_name();
chargen_result randomize_description();
chargen_result save_template( const std::string &name );

} // namespace godot_backend

#endif // defined(GODOT)

#endif // CATA_SRC_GODOT_CHARGEN_H
