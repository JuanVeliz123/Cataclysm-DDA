#include "faction_ui.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <imgui/imgui_internal.h>

#include "avatar.h"
#include "basecamp.h"
#include "calendar.h"
#include "cata_imgui.h"
#include "character.h"
#include "character_id.h"
#include "color.h"
#include "coordinates.h"
#include "dialogue_chatbin.h"
#include "display.h"
#include "enum_traits.h"
#include "faction.h"
#include "faction_camp.h"
#include "game.h"
#include "input_context.h"
#include "line.h"
#include "localized_comparator.h"
#include "map.h"
#include "mission_companion.h"
#include "mod_manager.h"
#include "mtype.h"
#include "npc.h"
#include "output.h"
#include "overmapbuffer.h"
#include "pimpl.h"
#include "point.h"
#include "skill.h"
#include "stomach.h"
#include "string_formatter.h"
#include "talker.h" // IWYU pragma: keep
#include "translation.h"
#include "translations.h"
#include "type_id.h"
#include "ui_manager.h"
#include "uilist.h"
#include "vitamin.h"

#if defined(GODOT)
#include "godot_faction_snapshot.h"
#endif

template <typename T> struct enum_traits;

static const faction_id faction_your_followers( "your_followers" );

static const flag_id json_flag_TWO_WAY_RADIO( "TWO_WAY_RADIO" );

template<>
struct enum_traits<tab_mode> {
    static constexpr tab_mode last = tab_mode::NUM_TABS;
    static constexpr tab_mode first = tab_mode::TAB_MYFACTION;
};

void faction_manager::display() const
{
    faction_ui ui;
#if defined(GODOT)
    // The Godot panel drives the same picked_* members this loop drives,
    // through the same process_action()/select_row() the row clicks and key
    // presses both go through. It declines when no panel is attending, and
    // then the legacy ImGui loop below runs.
    if( ui.run_in_godot() ) {
        return;
    }
#endif
    ui.execute();
}

static bool has_radio( const Character &guy )
{
    return guy.cache_has_item_with_flag( json_flag_TWO_WAY_RADIO, true );
}

// is physically close enough to contact the beta without much hassle
// mainly for followers
static bool can_contact( const Character &alpha, const Character &beta )
{
    const map &here = get_map();

    const bool too_far_omt = rl_dist( alpha.pos_abs_omt(), beta.pos_abs_omt() ) > 3;
    const bool see_each_other = alpha.sees( here, beta );
    return !too_far_omt || see_each_other ;
}

// opposite to previouis one, far enough to check for radio contact
static radio_contact_result can_radio_contact( const Character &alpha, const Character &beta )
{
    bool u_has_radio = has_radio( alpha );
    bool guy_has_radio = has_radio( beta );
    if( u_has_radio && guy_has_radio ) {
        if( !( alpha.posz() >= 0 && beta.posz() >= 0 ) &&
            !( alpha.posz() == beta.posz() ) ) {
            //Early exit
            return radio_contact_result::TOO_FAR;
        } else {
            // TODO: better range calculation than just elevation.
            const int base_range = 200;
            float send_elev_boost = ( 1 + ( alpha.posz() * 0.1 ) );
            float recv_elev_boost = ( 1 + ( beta.posz() * 0.1 ) );
            if( ( square_dist( alpha.pos_abs_sm(),
                               beta.pos_abs_sm() ) <= base_range * send_elev_boost * recv_elev_boost ) ) {
                //Direct radio contact, both of their elevation are in effect
                return radio_contact_result::YES;
            } else {
                //contact via camp radio tower
                int recv_range = base_range * recv_elev_boost;
                int send_range = base_range * send_elev_boost;
                const int radio_tower_boost = 5;
                // find camps that are near player or npc
                const std::vector<camp_reference> &camps_near_player = overmap_buffer.get_camps_near(
                            alpha.pos_abs_sm(), send_range * radio_tower_boost );
                const std::vector<camp_reference> &camps_near_npc = overmap_buffer.get_camps_near(
                            beta.pos_abs_sm(), recv_range * radio_tower_boost );
                bool camp_to_npc = false;
                bool camp_to_camp = false;
                for( const camp_reference &i : camps_near_player ) {
                    if( !i.camp->has_provides( "radio" ) ) {
                        continue;
                    }
                    if( camp_to_camp ||
                        square_dist( i.abs_sm_pos, beta.pos_abs_sm() ) <= recv_range * radio_tower_boost ) {
                        //one radio tower relay
                        camp_to_npc = true;
                        break;
                    }
                    for( const camp_reference &j : camps_near_npc ) {
                        //two radio tower relays
                        if( ( j.camp )->has_provides( "radio" ) &&
                            ( square_dist( i.abs_sm_pos, j.abs_sm_pos ) <= base_range * radio_tower_boost *
                              radio_tower_boost ) ) {
                            camp_to_camp = true;
                            break;
                        }
                    }
                }
                if( camp_to_npc || camp_to_camp ) {
                    return radio_contact_result::YES;
                } else {
                    return radio_contact_result::TOO_FAR;
                }
            }
        }
    }

    if( guy_has_radio && !u_has_radio ) {
        return radio_contact_result::ALPHA_NO_RADIO;
    }

    if( !guy_has_radio && u_has_radio ) {
        return radio_contact_result::BETA_NO_RADIO;
    }

    return radio_contact_result::BOTH_NO_RADIO;
}

static std::vector<npc *> get_known_faction_radio_representative( const faction *fac )
{
    const std::set<character_id> &known_fac_repres = get_avatar().faction_representatives;

    std::vector<npc *> applicable_repres;
    for( const character_id &guy : known_fac_repres ) {
        npc *npc = g->find_npc( guy );
        if( npc != nullptr &&
            !npc->is_dead() &&
            npc->faction_representative &&
            npc->get_faction_id() == fac->id ) {
            applicable_repres.emplace_back( npc );
        }
    }

    return applicable_repres;
}

bool faction_ui::execute()
{
    ctxt.register_action( "UP", to_translation( "Move cursor up" ) );
    ctxt.register_action( "DOWN", to_translation( "Move cursor down" ) );
    ctxt.register_action( "SCROLL_UP", to_translation( "Move cursor up" ) );
    ctxt.register_action( "SCROLL_DOWN", to_translation( "Move cursor down" ) );
    ctxt.register_action( "PAGE_UP", to_translation( "Scroll description up" ) );
    ctxt.register_action( "PAGE_DOWN", to_translation( "Scroll description down" ) );
    ctxt.register_action( "HOME", to_translation( "Scroll description to top" ) );
    ctxt.register_action( "END", to_translation( "Scroll description to bottom" ) );
    ctxt.register_leftright();
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.set_timeout( 16 );

    while( true ) {
        ui_manager::redraw_invalidated();
        last_action = ctxt.handle_input();
        if( last_action == "QUIT" || !get_is_open() ) {
            break;
        }

        if( last_action == "CONFIRM" ) {

            if( selected_tab == tab_mode::TAB_FOLLOWERS && picked_follower != nullptr ) {
                const bool interactable = can_contact( get_avatar(), *picked_follower );
                const bool radio_interactable =
                    can_radio_contact( get_avatar(), *picked_follower ) == radio_contact_result::YES;
                if( interactable || radio_interactable ) {
                    hide_ui = true;

                    if( picked_follower->has_companion_mission() ) {
                        talk_function::basecamp_mission( *picked_follower );
                        return true;
                    } else {
                        get_avatar().talk_to( get_talker_for( *picked_follower ), true, false, false,
                                              picked_follower->chatbin.talk_radio );
                        return true;
                    }
                } else {
                    popup( string_format( _( "You cannot contact %s right now." ), picked_follower->get_name() ) );
                }
            }
            if( selected_tab == tab_mode::TAB_MYFACTION && picked_camp != nullptr ) {
                picked_camp->query_new_name();
                return false;
            }

            if( selected_tab == tab_mode::TAB_OTHERFACTIONS && picked_faction != nullptr ) {
                hide_ui = true;
                radio_the_faction();
                return true;
            }
            return true;
        }
    }
    return false;
}

void faction_ui::draw_hint_section() const
{
    const std::string desc = string_format(
                                 _( "[<color_yellow>%s</color>] Keybindings" ), ctxt.get_desc( "HELP_KEYBINDINGS" ) );
    cataimgui::draw_colored_text( desc, c_unset );
    ImGui::Separator();
}

void faction_ui::draw_your_faction_tab()
{
    if( ImGui::BeginTable( "##Your_Faction", 2, ImGuiTableFlags_SizingFixedFit ) ) {
        ImGui::TableSetupColumn( "##Your camp list", ImGuiTableColumnFlags_WidthFixed,
                                 get_table_column_width() );

        ImGui::TableSetupColumn( "##Picked camp", ImGuiTableColumnFlags_WidthStretch );

        ImGui::TableNextColumn();
        draw_your_factions_list();

        ImGui::TableNextColumn();
        ImGui::BeginChild( "##Camp_Info" );

        cataimgui::set_scroll( s );

        your_faction_display();
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

void faction_ui::draw_your_factions_list()
{
    Character &player_character = get_player_character();
    std::vector<basecamp *> camps;
    g->validate_camps();
    for( tripoint_abs_omt elem : player_character.camps ) {
        std::optional<basecamp *> p = overmap_buffer.find_camp( elem.xy() );
        if( !p ) {
            continue;
        }
        basecamp *temp_camp = *p;
        if( temp_camp->get_owner() != player_character.get_faction()->id ) {
            // Don't display NPC camps as ours
            continue;
        }
        camps.push_back( temp_camp );
    }

    size_t num_entries = camps.size();
    const int last_entry = num_entries == 0 ? 0 : ( static_cast<int>( num_entries ) - 1 );

    // initialize selected_camp either from already set camp, or from zero if not set
    auto it = std::find( camps.begin(), camps.end(), picked_camp );
    int selected_camp = ( it != camps.end() ) ? std::distance( camps.begin(), it ) : 0;
    if( last_action == "UP" ) {
        selected_camp--;
    } else if( last_action == "DOWN" ) {
        selected_camp++;
    }

    if( selected_camp < 0 ) {
        selected_camp = last_entry;
    } else if( selected_camp > last_entry ) {
        selected_camp = 0;
    };

    if( !camps.empty() ) {
        picked_camp = camps[selected_camp];
    }

    if( ImGui::BeginListBox( "##LISTBOX", ImGui::GetContentRegionAvail() ) ) {

        for( size_t i = 0; i < camps.size(); i++ ) {
            basecamp *it_camp = camps[i];
            const bool is_selected = picked_camp != nullptr && picked_camp == it_camp;
            ImGui::PushID( i );

            if( ImGui::Selectable( "", is_selected ) ) {
                // if true, the option was selected, so set bp to what was selected
                picked_camp = it_camp;
            }

            if( is_selected ) {
                ImGui::SetScrollHereY();
                ImGui::SetItemDefaultFocus();
            }
            ImGui::SameLine();
            cataimgui::draw_colored_text( it_camp->camp_name(), ImGui::GetContentRegionAvail().x );

            ImGui::PopID();
        }
        ImGui::EndListBox();
    }
}

void faction_ui::your_faction_display() const
{
    const float col_width = ImGui::GetContentRegionAvail().x;

    if( picked_camp == nullptr ) {
        cataimgui::draw_colored_text( _( "You have no camps" ), c_red, col_width );
        return;
    }

    Character &player_character = get_player_character();
    const tripoint_abs_omt player_abspos = player_character.pos_abs_omt();
    tripoint_abs_omt camp_pos = picked_camp->camp_omt_pos();
    std::string direction = direction_name( direction_from( player_abspos, camp_pos ) );
    faction *yours = player_character.get_faction();

    // Hint
    const std::string hint_desc = string_format(
                                      _( "Press [%s] to rename this camp" ), ctxt.get_desc( "CONFIRM" ) );
    cataimgui::draw_colored_text( hint_desc, c_light_gray, col_width );

    if( direction != "center" ) {
        const std::string desc_dir = string_format( _( "Direction: to the %s" ), direction );
        cataimgui::draw_colored_text( desc_dir, col_width );
    }

    const std::string desc_loc = string_format( _( "Location: %s" ), camp_pos.to_string() );
    cataimgui::draw_colored_text( desc_loc, col_width );

    const std::string food_text = string_format( _( "Food Supply: %s (%d kcal)" ),
                                  yours->food_supply_text(), yours->food_supply().kcal() );
    const nc_color food_col = yours->food_supply_color();
    cataimgui::draw_colored_text( food_text, food_col, col_width );

    const auto &[vit_color, vit] = yours->vitamin_stores( vitamin_type::VITAMIN );
    const std::string worst_vitamin = string_format( "%s: %s", _( "Worst vitamin" ), vit );
    cataimgui::draw_colored_text( worst_vitamin, vit_color, col_width );

    const auto &[toxin_color, toxin] = yours->vitamin_stores( vitamin_type::TOXIN );
    const std::string worst_toxin = string_format( "%s: %s", _( "Worst toxin" ), toxin );
    cataimgui::draw_colored_text( worst_toxin, toxin_color, col_width );

    const std::string bldg = picked_camp->next_upgrade( base_camps::base_dir, 1 );
    const std::string bldg_full = string_format( "%s: %s", _( "Next Upgrade" ), bldg );
    cataimgui::draw_colored_text( bldg_full, col_width );
}

void faction_ui::draw_your_followers_tab()
{
    if( ImGui::BeginTable( "##Your_Followers", 2, ImGuiTableFlags_SizingFixedFit ) ) {
        ImGui::TableSetupColumn( "##Follower list", ImGuiTableColumnFlags_WidthFixed,
                                 get_table_column_width() );

        ImGui::TableSetupColumn( "##Picked follower", ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableNextColumn();

        draw_your_followers_list();

        ImGui::TableNextColumn();
        ImGui::BeginChild( "##Camp_Info" );

        cataimgui::set_scroll( s );

        your_follower_display();
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

void faction_ui::draw_your_followers_list()
{
    std::vector<npc *> followers;
    overmap_buffer.populate_followers_vec( followers );

    size_t num_entries = followers.size();
    const int last_entry = num_entries == 0 ? 0 : ( static_cast<int>( num_entries ) - 1 );

    // initialize selected_camp either from already set camp, or from zero if not set
    auto it = std::find( followers.begin(), followers.end(), picked_follower );
    int selected_npc = ( it != followers.end() ) ? std::distance( followers.begin(), it ) : 0;
    if( last_action == "UP" ) {
        selected_npc--;
    } else if( last_action == "DOWN" ) {
        selected_npc++;
    }

    if( selected_npc < 0 ) {
        selected_npc = last_entry;
    } else if( selected_npc > last_entry ) {
        selected_npc = 0;
    };

    if( !followers.empty() ) {
        picked_follower = followers[selected_npc];
    }

    if( ImGui::BeginListBox( "##LISTBOX", ImGui::GetContentRegionAvail() ) ) {

        for( size_t i = 0; i < followers.size(); i++ ) {
            npc *it_npc = followers[i];
            const bool is_selected = picked_follower != nullptr && picked_follower == it_npc;
            ImGui::PushID( i );

            if( ImGui::Selectable( "", is_selected ) ) {
                // if true, the option was selected, so set bp to what was selected
                picked_follower = it_npc;
            }

            if( is_selected ) {
                ImGui::SetScrollHereY();
                ImGui::SetItemDefaultFocus();
            }
            ImGui::SameLine();
            cataimgui::draw_colored_text( it_npc->get_name(), ImGui::GetContentRegionAvail().x );

            ImGui::PopID();
        }
        ImGui::EndListBox();
    }
}

void faction_ui::your_follower_display()
{
    const float col_width = ImGui::GetContentRegionAvail().x;

    if( picked_follower == nullptr ) {
        cataimgui::draw_colored_text( _( "You have no followers." ), c_red, col_width );
        return;
    }

    Character &player_character = get_player_character();
    const tripoint_abs_omt player_abspos = player_character.pos_abs_omt();

    // Mission
    std::string mission_string;
    if( picked_follower->has_companion_mission() ) {
        std::string dest_string;
        std::optional<tripoint_abs_omt> dest = picked_follower->get_mission_destination();
        if( dest ) {
            basecamp *dest_camp;
            std::optional<basecamp *> temp_camp = overmap_buffer.find_camp( dest->xy() );
            if( temp_camp ) {
                dest_camp = *temp_camp;
                dest_string = _( "traveling to: " ) + dest_camp->camp_name();
            } else {
                dest_string = string_format( _( "traveling to: %s" ), dest->to_string() );
            }
            mission_string = _( "Current Mission: " ) + dest_string;
        } else {
            npc_companion_mission c_mission = picked_follower->get_companion_mission();
            mission_string = _( "Current Mission: " ) + action_of( c_mission.miss_id.id );
        }

        cataimgui::draw_colored_text( mission_string, col_width );

        // Determine remaining time in mission, and display it
        std::string mission_eta;
        if( picked_follower->companion_mission_time_ret < calendar::turn ) {
            mission_eta = _( "JOB COMPLETED" );
        } else {
            mission_eta = _( "ETA: " ) + to_string( picked_follower->companion_mission_time_ret -
                                                    calendar::turn );
        }

        cataimgui::draw_colored_text( mission_eta, col_width );
    }

    tripoint_abs_omt guy_abspos = picked_follower->pos_abs_omt();
    basecamp *temp_camp = nullptr;
    if( picked_follower->assigned_camp ) {
        std::optional<basecamp *> bcp = overmap_buffer.find_camp( (
                                            *picked_follower->assigned_camp ).xy() );
        if( bcp ) {
            temp_camp = *bcp;
        }
    }

    // Direction
    const bool is_stationed = picked_follower->assigned_camp && temp_camp;
    std::string direction = direction_name( direction_from( player_abspos, guy_abspos ) );
    std::string dir_text;
    if( direction != "center" ) {
        dir_text = string_format( _( "Direction: to the %s" ), direction );
    } else {
        dir_text = _( "Direction: Nearby" );
    }
    cataimgui::draw_colored_text( dir_text, col_width );

    // Location
    std::string stationed_text;
    if( is_stationed ) {
        stationed_text = string_format( _( "Location: %s, at camp: %s" ), guy_abspos.to_string(),
                                        temp_camp->camp_name() );
    } else {
        stationed_text = string_format( _( "Location: %s" ), guy_abspos.to_string() );
    }
    cataimgui::draw_colored_text( stationed_text, col_width );

    // Can be radio-ed
    std::string can_see;
    nc_color see_color;
    if( can_contact( get_avatar(), *picked_follower ) ) {
        if( picked_follower->has_companion_mission() ) {
            can_see = _( "Press enter to recall from their mission." );
            see_color = c_light_red;
        } else {
            can_see = _( "Within interaction range" );
            see_color = c_light_green;
        }
    } else {
        const radio_contact_result rad = can_radio_contact( get_avatar(), *picked_follower );

        switch( rad ) {
            case radio_contact_result::ALPHA_NO_RADIO:
                can_see = _( "You do not have a radio" );
                see_color = c_light_red;
                break;
            case radio_contact_result::BETA_NO_RADIO:
                can_see = _( "Follower does not have a radio" );
                see_color = c_light_red;
                break;
            case radio_contact_result::BOTH_NO_RADIO:
                can_see = _( "Both you and follower need a radio" );
                see_color = c_light_red;
                break;
            case radio_contact_result::TOO_FAR:
                can_see = _( "Not within radio range" );
                see_color = c_light_red;
                break;
            default:
                can_see = _( "Within radio range" );
                see_color = c_light_green;
                break;
        }
    }

    cataimgui::draw_colored_text( colorize( can_see, see_color ), col_width );
    if( see_color == c_light_green ) {
        const std::string hint_desc = string_format(
                                          _( "Press [%s] to talk to this follower" ), ctxt.get_desc( "CONFIRM" ) );
        cataimgui::draw_colored_text( hint_desc, c_light_gray, col_width );
    }


    // Status/activity
    nc_color status_col = c_white;
    if( picked_follower->current_target() != nullptr ) {
        status_col = c_light_red;
    }
    const std::string status_text = string_format( _( "Status: %s" ),
                                    picked_follower->get_current_status() );
    cataimgui::draw_colored_text( colorize( status_text, status_col ), col_width );

    // Job and station
    if( is_stationed && picked_follower->has_job() ) {
        cataimgui::draw_colored_text( _( "Working at camp" ), col_width );
    } else if( is_stationed ) {
        cataimgui::draw_colored_text( _( "Idling at camp" ), col_width );
    }

    // HP description
    const auto &[hp_desc, hp_color] = picked_follower->hp_description();
    const std::string cond_text = string_format( _( "%s: %s" ), _( "Condition" ), hp_desc );
    cataimgui::draw_colored_text( cond_text, hp_color, col_width );

    // Hunger, thirst, sleepiness
    const auto &[hunger, hunger_color] = display::hunger_text_color( *picked_follower );
    const auto &[thirst, thirst_color] = display::thirst_text_color( *picked_follower );
    const auto &[sleepiness, sleepiness_color] = display::sleepiness_text_color( *picked_follower );
    const std::string nominal = pgettext( "needs", "Nominal" );
    const std::string hunger_text =
        string_format( "%s: %s", _( "Hunger" ), hunger.empty() ? nominal : hunger );
    const std::string thirst_text =
        string_format( "%s: %s", _( "Thirst" ), thirst.empty() ? nominal : thirst );
    const std::string sleepiness_text =
        string_format( _( "%s: %s" ), _( "Sleepiness" ), sleepiness.empty() ? nominal : sleepiness );
    cataimgui::draw_colored_text( hunger_text, hunger_color, col_width );
    cataimgui::draw_colored_text( thirst_text, thirst_color, col_width );
    cataimgui::draw_colored_text( sleepiness_text, sleepiness_color, col_width );

    // Wield
    const std::string wielding_text = string_format( _( "Wielding: %s" ),
                                      picked_follower->weapname_simple() );
    cataimgui::draw_colored_text( wielding_text, col_width );

    const auto skillslist = Skill::get_skills_sorted_by( [&]( const Skill & a, const Skill & b ) {
        const int level_a = picked_follower->get_skill_level( a.ident() );
        const int level_b = picked_follower->get_skill_level( b.ident() );
        return localized_compare( std::make_pair( -level_a, a.name() ),
                                  std::make_pair( -level_b, b.name() ) );
    } );
    size_t count = 0;
    std::vector<std::string> skill_strs;
    for( size_t i = 0; i < skillslist.size() && count < 3; i++ ) {
        if( !skillslist[ i ]->is_combat_skill() ) {
            std::string skill_str = string_format( "%s: %d", skillslist[i]->name(),
                                                   static_cast<int>( picked_follower->get_skill_level( skillslist[i]->ident() ) ) );
            skill_strs.push_back( skill_str );
            count += 1;
        }
    }
    const std::string best_combat_skill_text = string_format( "%s: %d",
            picked_follower->best_combat_skill( combat_skills::NO_GENERAL ).first.obj().name(),
            picked_follower->best_combat_skill( combat_skills::NO_GENERAL ).second );

    const std::string space = std::string( "  " );

    cataimgui::draw_colored_text( _( "Best combat skill" ) + std::string( ": " ), col_width );
    cataimgui::draw_colored_text( space + best_combat_skill_text, col_width );

    cataimgui::draw_colored_text( _( "Best other skills" ) + std::string( ": " ), col_width );
    cataimgui::draw_colored_text( space + skill_strs[0], col_width );
    cataimgui::draw_colored_text( space + skill_strs[1], col_width );
    cataimgui::draw_colored_text( space + skill_strs[2], col_width );
}

void faction_ui::draw_other_factions_tab()
{
    if( ImGui::BeginTable( "##Other_Factions", 2, ImGuiTableFlags_SizingFixedFit ) ) {
        ImGui::TableSetupColumn( "##Faction list", ImGuiTableColumnFlags_WidthFixed,
                                 get_table_column_width() );

        ImGui::TableSetupColumn( "##Picked faction", ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableNextColumn();

        draw_other_factions_list();

        ImGui::TableNextColumn();
        ImGui::BeginChild( "##Camp_Info" );

        cataimgui::set_scroll( s );

        other_faction_display();
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

void faction_ui::draw_other_factions_list()
{
    std::vector<const faction *> factions;
    for( const auto &elem : g->faction_manager_ptr->all() ) {
        if( elem.second.known_by_u &&
            elem.second.id != faction_your_followers &&
            !elem.second.lone_wolf_faction ) {
            factions.emplace_back( &elem.second );
        }
    }

    size_t num_entries = factions.size();
    const int last_entry = num_entries == 0 ? 0 : ( static_cast<int>( num_entries ) - 1 );

    // initialize selected_camp either from already set camp, or from zero if not set
    auto it = std::find( factions.begin(), factions.end(), picked_faction );
    int selected_faction = ( it != factions.end() ) ? std::distance( factions.begin(), it ) : 0;
    if( last_action == "UP" ) {
        selected_faction--;
    } else if( last_action == "DOWN" ) {
        selected_faction++;
    }

    if( selected_faction < 0 ) {
        selected_faction = last_entry;
    } else if( selected_faction > last_entry ) {
        selected_faction = 0;
    };

    if( !factions.empty() ) {
        picked_faction = factions[selected_faction];
    }

    if( ImGui::BeginListBox( "##LISTBOX", ImGui::GetContentRegionAvail() ) ) {

        for( size_t i = 0; i < factions.size(); i++ ) {
            const faction *it_faction = factions[i];
            const bool is_selected = picked_faction != nullptr && picked_faction == it_faction;
            ImGui::PushID( i );

            if( ImGui::Selectable( "", is_selected ) ) {
                // if true, the option was selected, so set bp to what was selected
                picked_faction = it_faction;
            }

            if( is_selected ) {
                ImGui::SetScrollHereY();
                ImGui::SetItemDefaultFocus();
            }
            ImGui::SameLine();
            cataimgui::draw_colored_text( it_faction->get_name(), ImGui::GetContentRegionAvail().x );

            ImGui::PopID();
        }
        ImGui::EndListBox();
    }
}

void faction_ui::other_faction_display()
{
    const float col_width = ImGui::GetContentRegionAvail().x;

    if( picked_faction == nullptr ) {
        cataimgui::draw_colored_text( _( "You don't know of any factions." ), col_width );
        return;
    }

    // Attitude towards you
    const std::string attitude_text = string_format( _( "Attitude to you: %s" ),
                                      fac_ranking_text( picked_faction->likes_u ) );
    cataimgui::draw_colored_text( attitude_text, col_width );


    // Can you contact them by radio
    const std::vector<npc *> applicable_repres = get_known_faction_radio_representative(
                picked_faction );
    if( !applicable_repres.empty() ) {
        cataimgui::draw_colored_text( _( "You know someone from this faction you can contact by radio." ),
                                      c_green, col_width );
        if( !has_radio( get_avatar() ) ) {
            cataimgui::draw_colored_text( _( "Even if you do not have a radio right now." ),
                                          c_red, col_width );
        } else {
            const std::string hint_desc = string_format(
                                              _( "Press [%s] to radio the faction representative" ), ctxt.get_desc( "CONFIRM" ) );
            cataimgui::draw_colored_text( hint_desc, c_light_gray, col_width );
        }
    }

    // Description
    cataimgui::draw_colored_text( picked_faction->desc.translated(), col_width );
}

void faction_ui::draw_creatures_tab()
{
    if( ImGui::BeginTable( "##Creatures", 2, ImGuiTableFlags_SizingFixedFit ) ) {
        ImGui::TableSetupColumn( "##Creature list", ImGuiTableColumnFlags_WidthFixed,
                                 get_table_column_width() );

        ImGui::TableSetupColumn( "##Picked creature", ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableNextColumn();

        draw_creature_list();

        ImGui::TableNextColumn();
        ImGui::BeginChild( "##Creature info" );

        cataimgui::set_scroll( s );

        creature_display();
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

void faction_ui::draw_creature_list()
{
    Character &player_character = get_player_character();
    std::vector<const mtype_id *> creatures;
    const std::set<mtype_id> &known_monsters = player_character.get_known_monsters();
    creatures.reserve( known_monsters.size() );
    for( const mtype_id &m : known_monsters ) {
        creatures.emplace_back( &m );
    }
    std::sort( creatures.begin(), creatures.end(), []( const mtype_id * a, const mtype_id * b ) {
        return localized_compare( a->obj().nname(), b->obj().nname() );
    } );

    size_t num_entries = creatures.size();
    const int last_entry = num_entries == 0 ? 0 : ( static_cast<int>( num_entries ) - 1 );

    // initialize selected_creature either from already set camp, or from zero if not set
    auto it = std::find( creatures.begin(), creatures.end(), picked_creature );
    int selected_creature = ( it != creatures.end() ) ? std::distance( creatures.begin(), it ) : 0;
    if( last_action == "UP" ) {
        selected_creature--;
    } else if( last_action == "DOWN" ) {
        selected_creature++;
    }

    if( selected_creature < 0 ) {
        selected_creature = last_entry;
    } else if( selected_creature > last_entry ) {
        selected_creature = 0;
    };

    if( !creatures.empty() ) {
        picked_creature = creatures[selected_creature];
    }

    if( ImGui::BeginListBox( "##LISTBOX", ImGui::GetContentRegionAvail() ) ) {

        for( size_t i = 0; i < creatures.size(); i++ ) {
            const mtype_id *it_creature = creatures[i];
            const bool is_selected = picked_creature != nullptr && picked_creature == it_creature;
            ImGui::PushID( i );

            if( ImGui::Selectable( "", is_selected ) ) {
                // if true, the option was selected, so set bp to what was selected
                picked_creature = it_creature;
            }

            if( is_selected ) {
                ImGui::SetScrollHereY();
                ImGui::SetItemDefaultFocus();
            }
            ImGui::SameLine();
            cataimgui::draw_colored_text( it_creature->obj().nname(), ImGui::GetContentRegionAvail().x );

            ImGui::PopID();
        }
        ImGui::EndListBox();
    }
}

void faction_ui::creature_display() const
{
    const float col_width = ImGui::GetContentRegionAvail().x;

    if( picked_creature == nullptr ) {
        cataimgui::draw_colored_text(
            _( "You haven't recorded sightings of any creatures.  Taking photos can be a good way to keep track of them." ),
            col_width );
        return;
    }

    const mtype &cr = picked_creature->obj();

    // Name & symbol
    cataimgui::draw_colored_text( string_format( "%s  %s", colorize( cr.sym, cr.color ), cr.nname() ),
                                  col_width );

    // Difficulty
    const std::string diff_str = cr.get_difficulty_description();
    cataimgui::draw_colored_text( string_format( "%s: %s", _( "Difficulty" ), diff_str ), col_width );

    // Origin
    const std::string origin_list = string_format( "%s: %s", _( "Origin" ),
                                    enumerate_as_string( cr.src.begin(), cr.src.end(),
    []( const std::pair<mtype_id, mod_id> &source ) {
        return string_format( "'%s'", source.second->name() );
    }, enumeration_conjunction::arrow ) );
    cataimgui::draw_colored_text( origin_list, c_light_gray, col_width );

    // Size
    cataimgui::draw_colored_text( string_format( "%s: %s", _( "Size" ), colorize( cr.get_size_name(),
                                  c_light_gray ) ), col_width );

    // Species
    const std::string species_list = string_format( "%s: %s", colorize( _( "Species" ), c_white ),
    enumerate_as_string( cr.species_descriptions(), []( const std::string & sp ) {
        return colorize( sp, c_yellow );
    } ) );
    cataimgui::draw_colored_text( species_list, c_light_gray, col_width );

    // Senses
    std::vector<std::string> senses_str;
    if( cr.has_flag( mon_flag_HEARS ) ) {
        senses_str.emplace_back( colorize( _( "hearing" ), c_yellow ) );
    }
    if( cr.has_flag( mon_flag_SEES ) ) {
        senses_str.emplace_back( colorize( _( "sight" ), c_yellow ) );
    }
    if( cr.has_flag( mon_flag_SMELLS ) ) {
        senses_str.emplace_back( colorize( _( "smell" ), c_yellow ) );
    }
    const std::string senses_text =
        string_format( "%s: %s", colorize( _( "Senses" ), c_white ),
                       enumerate_as_string( senses_str ) );
    cataimgui::draw_colored_text( senses_text, c_light_gray, col_width );

    // Abilities
    if( cr.has_flag( mon_flag_SWIMS ) || cr.move_skills.swim.has_value() ) {
        cataimgui::draw_colored_text( _( "It can swim." ), col_width );
    }
    if( cr.has_flag( mon_flag_FLIES ) ) {
        cataimgui::draw_colored_text( _( "It can fly." ), col_width );
    }
    if( cr.has_flag( mon_flag_DIGS ) || cr.move_skills.dig.has_value() ) {
        cataimgui::draw_colored_text( _( "It can burrow underground." ), col_width );
    }
    if( cr.has_flag( mon_flag_CLIMBS ) || cr.move_skills.climb.has_value() ) {
        cataimgui::draw_colored_text( _( "It can climb." ), col_width );
    }
    if( cr.has_flag( mon_flag_GRABS ) ) {
        cataimgui::draw_colored_text( _( "It can grab." ), col_width );
    }
    if( cr.has_flag( mon_flag_VENOM ) ) {
        cataimgui::draw_colored_text( _( "It can inflict poison." ), col_width );
    }
    if( cr.has_flag( mon_flag_PARALYZEVENOM ) ) {
        cataimgui::draw_colored_text( _( "It can inflict paralysis." ), col_width );
    }

    ImGui::NewLine();
    // Description
    cataimgui::draw_colored_text( cr.get_description(), c_light_gray, col_width );
}

void faction_ui::draw_controls()
{
    if( hide_ui ) {
        ImGuiWindow *w = ImGui::GetCurrentWindowRead();
        ImGui::SetWindowHiddenAndSkipItemsForCurrentFrame( w );
    }

    // no need to draw anything if we are exiting
    if( last_action == "QUIT" ) {
        return;
    } else if( last_action == "NEXT_TAB" || last_action == "RIGHT" ) {
        s = cataimgui::scroll::begin;
        ++selected_tab;
    } else if( last_action == "PREV_TAB" || last_action == "LEFT" ) {
        s = cataimgui::scroll::begin;
        --selected_tab;
    }

    if( last_action == "PAGE_UP" || last_action == "SCROLL_UP" ) {
        s = cataimgui::scroll::page_up;
    } else if( last_action == "PAGE_DOWN" || last_action == "SCROLL_DOWN" ) {
        s = cataimgui::scroll::page_down;
    } else if( last_action == "HOME" ) {
        s = cataimgui::scroll::begin;
    } else if( last_action == "END" ) {
        s = cataimgui::scroll::end;
    }

    draw_hint_section();

    if( ImGui::BeginTabBar( "##Tabs" ) ) {
        if( cataimgui::BeginTabItem( _( "Your Faction" ), selected_tab == tab_mode::TAB_MYFACTION ) ) {
            // if we picked the tab via mouse imput, update selected_tab
            // TODO there seems to be some lag appearing when you pick it with mouse?
            selected_tab = ImGui::IsItemActive() ? tab_mode::TAB_MYFACTION : selected_tab;
            draw_your_faction_tab();
            ImGui::EndTabItem();
        }
        if( cataimgui::BeginTabItem( _( "Your Followers" ),
                                     selected_tab == tab_mode::TAB_FOLLOWERS ) ) {
            selected_tab = ImGui::IsItemActive() ? tab_mode::TAB_FOLLOWERS : selected_tab;
            draw_your_followers_tab();
            ImGui::EndTabItem();
        }
        if( cataimgui::BeginTabItem( _( "Other Factions" ),
                                     selected_tab == tab_mode::TAB_OTHERFACTIONS ) ) {
            selected_tab = ImGui::IsItemActive() ? tab_mode::TAB_OTHERFACTIONS : selected_tab;
            draw_other_factions_tab();
            ImGui::EndTabItem();
        }
        if( cataimgui::BeginTabItem( _( "Creatures" ),
                                     selected_tab == tab_mode::TAB_CREATURES ) ) {
            selected_tab = ImGui::IsItemActive() ? tab_mode::TAB_CREATURES : selected_tab;
            draw_creatures_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void faction_ui::radio_the_faction()
{
    if( !has_radio( get_avatar() ) ) {
        popup( _( "You do not have a radio to contact anyone." ) );
        return;
    }

    std::vector<npc *> applicable_repres = get_known_faction_radio_representative( picked_faction );

    if( applicable_repres.empty() ) {
        popup( string_format( _( "You do not know anyone you can contact." ) ) );
        return;
    }

    uilist call_npc_query;
    call_npc_query.text = _( "Contact who?" );
    for( const npc *npc : applicable_repres ) {
        const radio_contact_result res = can_radio_contact( get_avatar(), *npc );
        std::string desc = npc->disp_name();
        if( res == radio_contact_result::YES ) {
            call_npc_query.addentry( MENU_AUTOASSIGN, true, MENU_AUTOASSIGN, npc->disp_name() );
        } else {
            // ALPHA_NO_RADIO & BOTH_NO_RADIO are excluded by check above
            // so this fires only for BETA_NO_RADIO and TOO_FAR
            const std::string desc = string_format( "%s (%s)", npc->disp_name(), _( "No response" ) );
            call_npc_query.addentry( MENU_AUTOASSIGN, false, MENU_AUTOASSIGN, desc );
        }
    }
    call_npc_query.query();

    if( call_npc_query.ret < 0 ) {
        return;
    }
    npc *n = applicable_repres[call_npc_query.ret];
    get_avatar().talk_to( get_talker_for( n ), true, false, false,
                          n->chatbin.talk_radio );
}

// --- fetch-only list builders, shared with the Godot panel -----------------
// Exact fetch logic of the four draw_*_list()s above, minus the ImGui draw
// calls and the row-cursor math (the latter lives in process_action() for
// the Godot path; the legacy draw functions keep their own inline copy).

std::vector<basecamp *> faction_ui::get_camps() const
{
    Character &player_character = get_player_character();
    std::vector<basecamp *> camps;
    g->validate_camps();
    for( tripoint_abs_omt elem : player_character.camps ) {
        std::optional<basecamp *> p = overmap_buffer.find_camp( elem.xy() );
        if( !p ) {
            continue;
        }
        basecamp *temp_camp = *p;
        if( temp_camp->get_owner() != player_character.get_faction()->id ) {
            continue;
        }
        camps.push_back( temp_camp );
    }
    return camps;
}

std::vector<npc *> faction_ui::get_followers() const
{
    std::vector<npc *> followers;
    overmap_buffer.populate_followers_vec( followers );
    return followers;
}

std::vector<const faction *> faction_ui::get_other_factions() const
{
    std::vector<const faction *> factions;
    for( const auto &elem : g->faction_manager_ptr->all() ) {
        if( elem.second.known_by_u &&
            elem.second.id != faction_your_followers &&
            !elem.second.lone_wolf_faction ) {
            factions.emplace_back( &elem.second );
        }
    }
    return factions;
}

std::vector<const mtype_id *> faction_ui::get_creatures() const
{
    Character &player_character = get_player_character();
    std::vector<const mtype_id *> creatures;
    const std::set<mtype_id> &known_monsters = player_character.get_known_monsters();
    creatures.reserve( known_monsters.size() );
    for( const mtype_id &m : known_monsters ) {
        creatures.emplace_back( &m );
    }
    std::sort( creatures.begin(), creatures.end(), []( const mtype_id * a, const mtype_id * b ) {
        return localized_compare( a->obj().nname(), b->obj().nname() );
    } );
    return creatures;
}

// --- text-blob detail panes, shared with the Godot panel --------------------
// Same content as the four *_display()s above, as colour-tagged strings
// instead of ImGui calls.

std::string faction_ui::camp_detail( basecamp *camp ) const
{
    if( camp == nullptr ) {
        return colorize( _( "You have no camps" ), c_red );
    }
    Character &player_character = get_player_character();
    const tripoint_abs_omt player_abspos = player_character.pos_abs_omt();
    tripoint_abs_omt camp_pos = camp->camp_omt_pos();
    std::string direction = direction_name( direction_from( player_abspos, camp_pos ) );
    faction *yours = player_character.get_faction();

    std::string out = colorize( string_format( _( "Press [%s] to rename this camp" ),
                                ctxt.get_desc( "CONFIRM" ) ), c_light_gray ) + "\n";
    if( direction != "center" ) {
        out += string_format( _( "Direction: to the %s" ), direction ) + "\n";
    }
    out += string_format( _( "Location: %s" ), camp_pos.to_string() ) + "\n";
    out += colorize( string_format( _( "Food Supply: %s (%d kcal)" ), yours->food_supply_text(),
                                    yours->food_supply().kcal() ), yours->food_supply_color() ) + "\n";
    const auto &[vit_color, vit] = yours->vitamin_stores( vitamin_type::VITAMIN );
    out += colorize( string_format( "%s: %s", _( "Worst vitamin" ), vit ), vit_color ) + "\n";
    const auto &[toxin_color, toxin] = yours->vitamin_stores( vitamin_type::TOXIN );
    out += colorize( string_format( "%s: %s", _( "Worst toxin" ), toxin ), toxin_color ) + "\n";
    out += string_format( "%s: %s", _( "Next Upgrade" ),
                          camp->next_upgrade( base_camps::base_dir, 1 ) ) + "\n";
    return out;
}

std::string faction_ui::follower_detail( npc *follower ) const
{
    if( follower == nullptr ) {
        return colorize( _( "You have no followers." ), c_red );
    }
    Character &player_character = get_player_character();
    const tripoint_abs_omt player_abspos = player_character.pos_abs_omt();

    std::string out;
    if( follower->has_companion_mission() ) {
        std::string dest_string;
        std::optional<tripoint_abs_omt> dest = follower->get_mission_destination();
        if( dest ) {
            std::optional<basecamp *> temp_camp = overmap_buffer.find_camp( dest->xy() );
            if( temp_camp ) {
                dest_string = _( "traveling to: " ) + ( *temp_camp )->camp_name();
            } else {
                dest_string = string_format( _( "traveling to: %s" ), dest->to_string() );
            }
            out += _( "Current Mission: " ) + dest_string + "\n";
        } else {
            npc_companion_mission c_mission = follower->get_companion_mission();
            out += _( "Current Mission: " ) + action_of( c_mission.miss_id.id ) + "\n";
        }
        std::string mission_eta;
        if( follower->companion_mission_time_ret < calendar::turn ) {
            mission_eta = _( "JOB COMPLETED" );
        } else {
            mission_eta = _( "ETA: " ) + to_string( follower->companion_mission_time_ret -
                                                    calendar::turn );
        }
        out += mission_eta + "\n";
    }

    tripoint_abs_omt guy_abspos = follower->pos_abs_omt();
    basecamp *temp_camp = nullptr;
    if( follower->assigned_camp ) {
        std::optional<basecamp *> bcp = overmap_buffer.find_camp( ( *follower->assigned_camp ).xy() );
        if( bcp ) {
            temp_camp = *bcp;
        }
    }
    const bool is_stationed = follower->assigned_camp && temp_camp;
    std::string direction = direction_name( direction_from( player_abspos, guy_abspos ) );
    if( direction != "center" ) {
        out += string_format( _( "Direction: to the %s" ), direction ) + "\n";
    } else {
        out += std::string( _( "Direction: Nearby" ) ) + "\n";
    }
    if( is_stationed ) {
        out += string_format( _( "Location: %s, at camp: %s" ), guy_abspos.to_string(),
                              temp_camp->camp_name() ) + "\n";
    } else {
        out += string_format( _( "Location: %s" ), guy_abspos.to_string() ) + "\n";
    }

    std::string can_see;
    nc_color see_color;
    if( can_contact( get_avatar(), *follower ) ) {
        if( follower->has_companion_mission() ) {
            can_see = _( "Press enter to recall from their mission." );
            see_color = c_light_red;
        } else {
            can_see = _( "Within interaction range" );
            see_color = c_light_green;
        }
    } else {
        const radio_contact_result rad = can_radio_contact( get_avatar(), *follower );
        switch( rad ) {
            case radio_contact_result::ALPHA_NO_RADIO:
                can_see = _( "You do not have a radio" );
                see_color = c_light_red;
                break;
            case radio_contact_result::BETA_NO_RADIO:
                can_see = _( "Follower does not have a radio" );
                see_color = c_light_red;
                break;
            case radio_contact_result::BOTH_NO_RADIO:
                can_see = _( "Both you and follower need a radio" );
                see_color = c_light_red;
                break;
            case radio_contact_result::TOO_FAR:
                can_see = _( "Not within radio range" );
                see_color = c_light_red;
                break;
            default:
                can_see = _( "Within radio range" );
                see_color = c_light_green;
                break;
        }
    }
    out += colorize( can_see, see_color ) + "\n";
    if( see_color == c_light_green ) {
        out += colorize( string_format( _( "Press [%s] to talk to this follower" ),
                                        ctxt.get_desc( "CONFIRM" ) ), c_light_gray ) + "\n";
    }

    nc_color status_col = c_white;
    if( follower->current_target() != nullptr ) {
        status_col = c_light_red;
    }
    out += colorize( string_format( _( "Status: %s" ), follower->get_current_status() ),
                     status_col ) + "\n";

    if( is_stationed && follower->has_job() ) {
        out += std::string( _( "Working at camp" ) ) + "\n";
    } else if( is_stationed ) {
        out += std::string( _( "Idling at camp" ) ) + "\n";
    }

    const auto &[hp_desc, hp_color] = follower->hp_description();
    out += colorize( string_format( "%s: %s", _( "Condition" ), hp_desc ), hp_color ) + "\n";

    const auto &[hunger, hunger_color] = display::hunger_text_color( *follower );
    const auto &[thirst, thirst_color] = display::thirst_text_color( *follower );
    const auto &[sleepiness, sleepiness_color] = display::sleepiness_text_color( *follower );
    const std::string nominal = pgettext( "needs", "Nominal" );
    out += colorize( string_format( "%s: %s", _( "Hunger" ), hunger.empty() ? nominal : hunger ),
                     hunger_color ) + "\n";
    out += colorize( string_format( "%s: %s", _( "Thirst" ), thirst.empty() ? nominal : thirst ),
                     thirst_color ) + "\n";
    out += colorize( string_format( _( "%s: %s" ), _( "Sleepiness" ),
                                    sleepiness.empty() ? nominal : sleepiness ), sleepiness_color ) + "\n";

    out += string_format( _( "Wielding: %s" ), follower->weapname_simple() ) + "\n";

    const auto skillslist = Skill::get_skills_sorted_by( [&]( const Skill & a, const Skill & b ) {
        const int level_a = follower->get_skill_level( a.ident() );
        const int level_b = follower->get_skill_level( b.ident() );
        return localized_compare( std::make_pair( -level_a, a.name() ),
                                  std::make_pair( -level_b, b.name() ) );
    } );
    size_t count = 0;
    std::vector<std::string> skill_strs;
    for( size_t i = 0; i < skillslist.size() && count < 3; i++ ) {
        if( !skillslist[i]->is_combat_skill() ) {
            skill_strs.push_back( string_format( "%s: %d", skillslist[i]->name(),
                                                 static_cast<int>( follower->get_skill_level( skillslist[i]->ident() ) ) ) );
            count += 1;
        }
    }
    out += string_format( "%s: %d",
                          follower->best_combat_skill( combat_skills::NO_GENERAL ).first.obj().name(),
                          follower->best_combat_skill( combat_skills::NO_GENERAL ).second ) + "\n";
    out += std::string( _( "Best other skills" ) ) + ":\n";
    // The ImGui pane indexes skill_strs[0..2] unconditionally, which is fine
    // there only because a character always has at least three non-combat
    // skills; bounded here rather than carried the same way into new code.
    for( size_t i = 0; i < skill_strs.size() && i < 3; ++i ) {
        out += "  " + skill_strs[i] + "\n";
    }
    return out;
}

std::string faction_ui::other_faction_detail( const faction *fac ) const
{
    if( fac == nullptr ) {
        return _( "You don't know of any factions." );
    }
    std::string out = string_format( _( "Attitude to you: %s" ),
                                     fac_ranking_text( fac->likes_u ) ) + "\n";
    const std::vector<npc *> applicable_repres = get_known_faction_radio_representative( fac );
    if( !applicable_repres.empty() ) {
        out += colorize( _( "You know someone from this faction you can contact by radio." ),
                         c_green ) + "\n";
        if( !has_radio( get_avatar() ) ) {
            out += colorize( _( "Even if you do not have a radio right now." ), c_red ) + "\n";
        } else {
            out += colorize( string_format( _( "Press [%s] to radio the faction representative" ),
                                            ctxt.get_desc( "CONFIRM" ) ), c_light_gray ) + "\n";
        }
    }
    out += fac->desc.translated() + "\n";
    return out;
}

std::string faction_ui::creature_detail( const mtype_id *creature ) const
{
    if( creature == nullptr ) {
        return _( "You haven't recorded sightings of any creatures.  Taking photos can be a good way to keep track of them." );
    }
    const mtype &cr = creature->obj();
    std::string out = string_format( "%s  %s", colorize( cr.sym, cr.color ), cr.nname() ) + "\n";
    out += string_format( "%s: %s", _( "Difficulty" ), cr.get_difficulty_description() ) + "\n";
    out += colorize( string_format( "%s: %s", _( "Origin" ),
                                    enumerate_as_string( cr.src.begin(), cr.src.end(),
    []( const std::pair<mtype_id, mod_id> &source ) {
        return string_format( "'%s'", source.second->name() );
    }, enumeration_conjunction::arrow ) ), c_light_gray ) + "\n";
    out += string_format( "%s: %s", _( "Size" ), colorize( cr.get_size_name(), c_light_gray ) ) + "\n";
    out += colorize( string_format( "%s: %s", colorize( _( "Species" ), c_white ),
                                    enumerate_as_string( cr.species_descriptions(), []( const std::string & sp ) {
        return colorize( sp, c_yellow );
    } ) ), c_light_gray ) + "\n";
    std::vector<std::string> senses_str;
    if( cr.has_flag( mon_flag_HEARS ) ) {
        senses_str.emplace_back( colorize( _( "hearing" ), c_yellow ) );
    }
    if( cr.has_flag( mon_flag_SEES ) ) {
        senses_str.emplace_back( colorize( _( "sight" ), c_yellow ) );
    }
    if( cr.has_flag( mon_flag_SMELLS ) ) {
        senses_str.emplace_back( colorize( _( "smell" ), c_yellow ) );
    }
    out += colorize( string_format( "%s: %s", colorize( _( "Senses" ), c_white ),
                                    enumerate_as_string( senses_str ) ), c_light_gray ) + "\n";
    if( cr.has_flag( mon_flag_SWIMS ) || cr.move_skills.swim.has_value() ) {
        out += std::string( _( "It can swim." ) ) + "\n";
    }
    if( cr.has_flag( mon_flag_FLIES ) ) {
        out += std::string( _( "It can fly." ) ) + "\n";
    }
    if( cr.has_flag( mon_flag_DIGS ) || cr.move_skills.dig.has_value() ) {
        out += std::string( _( "It can burrow underground." ) ) + "\n";
    }
    if( cr.has_flag( mon_flag_CLIMBS ) || cr.move_skills.climb.has_value() ) {
        out += std::string( _( "It can climb." ) ) + "\n";
    }
    if( cr.has_flag( mon_flag_GRABS ) ) {
        out += std::string( _( "It can grab." ) ) + "\n";
    }
    if( cr.has_flag( mon_flag_VENOM ) ) {
        out += std::string( _( "It can inflict poison." ) ) + "\n";
    }
    if( cr.has_flag( mon_flag_PARALYZEVENOM ) ) {
        out += std::string( _( "It can inflict paralysis." ) ) + "\n";
    }
    out += "\n" + cr.get_description();
    return out;
}

#if defined(GODOT)

namespace
{
/// Move a `picked` pointer within `list` by one UP/DOWN/HOME/END step,
/// mirroring the find-clamp-reassign block every draw_*_list() above runs
/// inline every frame. A `picked` outside `list` (or null) is treated as
/// "nothing selected yet" and lands on the front of the list, same as the
/// `std::find` returning `end()` case those functions already handle.
template<typename T>
void select_from_list( const std::vector<T> &list, T &picked, const std::string &action )
{
    if( list.empty() ) {
        return;
    }
    const int last_entry = static_cast<int>( list.size() ) - 1;
    auto it = std::find( list.begin(), list.end(), picked );
    int idx = ( it != list.end() ) ? static_cast<int>( std::distance( list.begin(), it ) ) : 0;
    if( action == "UP" ) {
        --idx;
    } else if( action == "DOWN" ) {
        ++idx;
    } else if( action == "HOME" ) {
        idx = 0;
    } else if( action == "END" ) {
        idx = last_entry;
    }
    if( idx < 0 ) {
        idx = last_entry;
    } else if( idx > last_entry ) {
        idx = 0;
    }
    picked = list[idx];
}
} // namespace

void faction_ui::process_action( const std::string &action )
{
    if( action == "NEXT_TAB" || action == "RIGHT" ) {
        s = cataimgui::scroll::begin;
        ++selected_tab;
    } else if( action == "PREV_TAB" || action == "LEFT" ) {
        s = cataimgui::scroll::begin;
        --selected_tab;
    }

    switch( selected_tab ) {
        case tab_mode::TAB_MYFACTION:
            select_from_list( get_camps(), picked_camp, action );
            break;
        case tab_mode::TAB_FOLLOWERS:
            select_from_list( get_followers(), picked_follower, action );
            break;
        case tab_mode::TAB_OTHERFACTIONS:
            select_from_list( get_other_factions(), picked_faction, action );
            break;
        case tab_mode::TAB_CREATURES:
            select_from_list( get_creatures(), picked_creature, action );
            break;
        default:
            break;
    }
}

void faction_ui::select_row( const int index )
{
    if( index < 0 ) {
        return;
    }
    switch( selected_tab ) {
        case tab_mode::TAB_MYFACTION: {
            const std::vector<basecamp *> camps = get_camps();
            if( index < static_cast<int>( camps.size() ) ) {
                picked_camp = camps[index];
            }
            break;
        }
        case tab_mode::TAB_FOLLOWERS: {
            const std::vector<npc *> followers = get_followers();
            if( index < static_cast<int>( followers.size() ) ) {
                picked_follower = followers[index];
            }
            break;
        }
        case tab_mode::TAB_OTHERFACTIONS: {
            const std::vector<const faction *> factions = get_other_factions();
            if( index < static_cast<int>( factions.size() ) ) {
                picked_faction = factions[index];
            }
            break;
        }
        case tab_mode::TAB_CREATURES: {
            const std::vector<const mtype_id *> creatures = get_creatures();
            if( index < static_cast<int>( creatures.size() ) ) {
                picked_creature = creatures[index];
            }
            break;
        }
        default:
            break;
    }
}

void faction_ui::set_tab( const int index )
{
    if( index < 0 || index >= static_cast<int>( tab_mode::NUM_TABS ) ) {
        return;
    }
    selected_tab = static_cast<tab_mode>( index );
}

void faction_ui::publish_to_godot()
{
    using snapshot = godot_backend::FactionSnapshot;
    snapshot::data d;
    d.title = _( "Faction" );
    d.tab_titles = { _( "Your Faction" ), _( "Your Followers" ), _( "Other Factions" ), _( "Creatures" ) };
    d.selected_tab = static_cast<int>( selected_tab );

    switch( selected_tab ) {
        case tab_mode::TAB_MYFACTION: {
            const std::vector<basecamp *> camps = get_camps();
            d.rows.reserve( camps.size() );
            for( basecamp *c : camps ) {
                d.rows.push_back( c->camp_name() );
                if( c == picked_camp ) {
                    d.selected_row = static_cast<int>( d.rows.size() ) - 1;
                }
            }
            d.detail = camp_detail( picked_camp );
            break;
        }
        case tab_mode::TAB_FOLLOWERS: {
            const std::vector<npc *> followers = get_followers();
            d.rows.reserve( followers.size() );
            for( npc *n : followers ) {
                d.rows.push_back( n->get_name() );
                if( n == picked_follower ) {
                    d.selected_row = static_cast<int>( d.rows.size() ) - 1;
                }
            }
            d.detail = follower_detail( picked_follower );
            break;
        }
        case tab_mode::TAB_OTHERFACTIONS: {
            const std::vector<const faction *> factions = get_other_factions();
            d.rows.reserve( factions.size() );
            for( const faction *f : factions ) {
                d.rows.push_back( f->get_name() );
                if( f == picked_faction ) {
                    d.selected_row = static_cast<int>( d.rows.size() ) - 1;
                }
            }
            d.detail = other_faction_detail( picked_faction );
            break;
        }
        case tab_mode::TAB_CREATURES: {
            const std::vector<const mtype_id *> creatures = get_creatures();
            d.rows.reserve( creatures.size() );
            for( const mtype_id *m : creatures ) {
                d.rows.push_back( m->obj().nname() );
                if( m == picked_creature ) {
                    d.selected_row = static_cast<int>( d.rows.size() ) - 1;
                }
            }
            d.detail = creature_detail( picked_creature );
            break;
        }
        default:
            break;
    }

    godot_backend::get_faction_snapshot().publish( d );
}

bool faction_ui::run_in_godot()
{
    godot_backend::FactionSnapshot &snap = godot_backend::get_faction_snapshot();
    snap.clear();
    // Pick the initial row on each tab the way the first ImGui draw would.
    process_action( std::string() );
    publish_to_godot();
    while( true ) {
        int row = -1;
        int tab = -1;
        const std::string action = snap.next_action( row, tab );
        if( action.empty() ) {
            // No panel attended; the caller runs the legacy ImGui loop.
            snap.clear();
            return false;
        }
        if( action == "QUIT" ) {
            break;
        }
        if( action == "GODOT_SELECT" ) {
            select_row( row );
        } else if( action == "GODOT_TAB" ) {
            set_tab( tab );
            process_action( std::string() );
        } else if( action == "CONFIRM" ) {
            // Mirrors execute()'s CONFIRM handling exactly: every branch
            // ends the screen, whether or not it did anything. The nested
            // calls open their own screens (dialogue, a uilist, a rename
            // prompt) -- Godot or legacy -- so this channel is suspended
            // around them, the same move MedicalSnapshot's APPLY makes for
            // the item picker, or this panel would sit on top of whatever
            // opens next instead of getting out of its way.
            if( selected_tab == tab_mode::TAB_FOLLOWERS && picked_follower != nullptr ) {
                const bool interactable = can_contact( get_avatar(), *picked_follower );
                const bool radio_interactable =
                    can_radio_contact( get_avatar(), *picked_follower ) == radio_contact_result::YES;
                if( interactable || radio_interactable ) {
                    snap.set_suspended( true );
                    if( picked_follower->has_companion_mission() ) {
                        talk_function::basecamp_mission( *picked_follower );
                    } else {
                        get_avatar().talk_to( get_talker_for( *picked_follower ), true, false, false,
                                              picked_follower->chatbin.talk_radio );
                    }
                } else {
                    popup( string_format( _( "You cannot contact %s right now." ),
                                          picked_follower->get_name() ) );
                }
            } else if( selected_tab == tab_mode::TAB_MYFACTION && picked_camp != nullptr ) {
                snap.set_suspended( true );
                picked_camp->query_new_name();
            } else if( selected_tab == tab_mode::TAB_OTHERFACTIONS && picked_faction != nullptr ) {
                snap.set_suspended( true );
                radio_the_faction();
            }
            break;
        } else {
            process_action( action );
        }
        publish_to_godot();
    }
    snap.clear();
    return true;
}

#endif // GODOT
