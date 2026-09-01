#include "game.h" // IWYU pragma: associated

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "cata_imgui.h"
#include "color.h"
#include "dialogue.h"
#include "dialogue_helpers.h"
#include "display.h"
#include "game_constants.h"
#include "imgui/imgui.h"
#include "input_context.h"
#include "line.h"
#include "mission.h"
#include "npc.h"
#include "faction.h"
#include "output.h"
#include "point.h"
#include "string_formatter.h"
#include "talker.h"
#include "timed_event.h"
#include "translation.h"
#include "translations.h"
#include "ui_manager.h"
#include "units.h"
#include "units_utility.h"

#if defined(GODOT)
#include "godot_mission_snapshot.h"
#endif

static const dimension_id dimension_world_default( "default" );

static const faction_id faction_no_faction( "no_faction" );

namespace
{
enum class mission_ui_tab_enum : int {
    ACTIVE = 0,
    COMPLETED,
    FAILED,
    POINTS_OF_INTEREST,
    num_tabs
};
} // namespace

static mission_ui_tab_enum &operator++( mission_ui_tab_enum &c )
{
    c = static_cast<mission_ui_tab_enum>( static_cast<int>( c ) + 1 );
    if( c == mission_ui_tab_enum::num_tabs ) {
        c = static_cast<mission_ui_tab_enum>( 0 );
    }
    return c;
}

static mission_ui_tab_enum &operator--( mission_ui_tab_enum &c )
{
    if( c == static_cast<mission_ui_tab_enum>( 0 ) ) {
        c = mission_ui_tab_enum::num_tabs;
    }
    c = static_cast<mission_ui_tab_enum>( static_cast<int>( c ) - 1 );
    return c;
}

namespace
{
class mission_ui
{
        friend class mission_ui_impl;
    public:
        void draw_mission_ui();
};

class mission_ui_impl : public cataimgui::window
{
    public:
        std::string last_action;
        explicit mission_ui_impl() : cataimgui::window( _( "Your missions" ),
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav ) {
        }

#if defined(GODOT)
        /// Show this screen as a Godot panel and block until it is dismissed.
        /// @return false when no panel attended, so the caller must run the
        ///         legacy ImGui loop instead.
        bool run_in_godot();
#endif

    private:
        void draw_mission_names( bool need_adjust ) const;
        void draw_point_of_interest_names( bool need_adjust ) const;
        void draw_selected_description();
        void draw_selected_description_poi() const;
        void draw_label_with_value( const std::string &label, const std::string &value ) const;
        void draw_location( const std::string &label, const tripoint_abs_omt &loc ) const;

        /// Apply one action to the members: the tab, the row cursor, and the
        /// two actions that mutate game state (CONFIRM, DELETE_POINT_OF_INTEREST).
        /// Lifted out of draw_controls() so these decide from state the game
        /// thread owns rather than from BeginTabItem returning true or a
        /// listbox click inside the draw -- which nothing off the ImGui thread
        /// could read or drive.
        void process_action( const std::string &action );
        /// (Re)fetch the active tab's rows into umissions / upoints_of_interest.
        /// Called at the top of process_action, and once more after the tab bar
        /// draws in draw_controls() -- a mouse click on a different tab updates
        /// selected_tab from inside that draw, after process_action already ran.
        void refresh_lists();
        /// Mission detail, built the same way draw_selected_description() draws
        /// it but as a colour-tagged string instead of ImGui calls, so both the
        /// legacy pane and the Godot panel read from one place.
        std::string mission_detail( mission *miss );
        std::string poi_detail( const point_of_interest &poi ) const;
        std::string location_text( const std::string &label, const tripoint_abs_omt &loc ) const;

#if defined(GODOT)
        void publish_to_godot();
        void select_row( int index );
        void set_tab( int index );
#endif

        mission_ui_tab_enum selected_tab = mission_ui_tab_enum::ACTIVE;
        mission_ui_tab_enum switch_tab = mission_ui_tab_enum::num_tabs;
        int selected_mission = 0;
        std::vector<mission *> umissions;
        std::vector<point_of_interest> upoints_of_interest;
        /// Cache keyed the same way draw_selected_description() cached it: the
        /// parsed text is expensive (snippet + tag parsing) and must not be
        /// redone every publish while the same mission stays selected.
        std::string raw_description;
        std::string parsed_description;

        float window_width = std::clamp( float( str_width_to_pixels( EVEN_MINIMUM_TERM_WIDTH ) ),
                                         ImGui::GetMainViewport()->Size.x / 2,
                                         ImGui::GetMainViewport()->Size.x );
        float window_height = std::clamp( float( str_height_to_pixels( EVEN_MINIMUM_TERM_HEIGHT ) ),
                                          ImGui::GetMainViewport()->Size.y / 2,
                                          ImGui::GetMainViewport()->Size.y );
        float table_column_width = window_width / 2;

        cataimgui::scroll s = cataimgui::scroll::none;

    protected:
        void draw_controls() override;
};

/// Text shown instead of the table when a tab's collection is empty. Shared
/// by the ImGui pane and the Godot panel so the wording cannot drift between
/// them.
static translation empty_text_for( mission_ui_tab_enum tab )
{
    static const std::map<mission_ui_tab_enum, translation> nope = {
        { mission_ui_tab_enum::ACTIVE, to_translation( "You have no active missions!" ) },
        { mission_ui_tab_enum::COMPLETED, to_translation( "You haven't completed any missions!" ) },
        { mission_ui_tab_enum::FAILED, to_translation( "You haven't failed any missions!" ) },
        { mission_ui_tab_enum::POINTS_OF_INTEREST, to_translation( "You don't have any points of interest.  Add those from the overmap." ) }
    };
    return nope.at( tab );
}

void mission_ui::draw_mission_ui()
{
    input_context ctxt( "MISSION_UI" );
    mission_ui_impl p_impl;

    ctxt.register_navigate_ui_list();
    ctxt.register_leftright();
    ctxt.register_action( "NEXT_TAB" );
    ctxt.register_action( "PREV_TAB" );
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "MOUSE_MOVE" );
    ctxt.register_action( "CONFIRM",
                          to_translation( "Set selected mission/point of interest as current objective" ) );
    ctxt.register_action( "DELETE_POINT_OF_INTEREST", to_translation( "Delete Point of Interest" ) );
    // We don't actually have a way to remap this right now
    //ctxt.register_action( "DOUBLE_CLICK", to_translation( "Set selected mission as current objective" ) );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "QUIT" );
    // Smooths out our handling, makes tabs load immediately after input instead of waiting for next.
    ctxt.set_timeout( 10 );

#if defined(GODOT)
    // The Godot panel drives the same members this loop drives -- the tab, the
    // row cursor, CONFIRM, DELETE_POINT_OF_INTEREST -- through the same
    // process_action(), so the screen behaves identically either way. It
    // declines when no panel is attending, and then the legacy ImGui loop
    // below runs.
    if( p_impl.run_in_godot() ) {
        return;
    }
#endif

    while( true ) {
        ui_manager::redraw_invalidated();


        p_impl.last_action = ctxt.handle_input();

        if( p_impl.last_action == "QUIT" || !p_impl.get_is_open() ) {
            break;
        }
    }
}

// The action handling from draw_controls(), lifted so it works on members
// instead of on ImGui state. NEXT_TAB / PREV_TAB move selected_tab directly
// (switch_tab is kept in step so the ImGui tab bar follows it via
// ImGuiTabItemFlags_SetSelected, same as scores_ui); refresh_lists() then
// fetches whichever tab is now current, and the row cursor is clamped against
// that -- exactly the order draw_controls() used to apply inline.
void mission_ui_impl::process_action( const std::string &action )
{
    if( action == "NEXT_TAB" || action == "RIGHT" ) {
        selected_mission = 0;
        s = cataimgui::scroll::begin;
        ++selected_tab;
        switch_tab = selected_tab;
    } else if( action == "PREV_TAB" || action == "LEFT" ) {
        selected_mission = 0;
        s = cataimgui::scroll::begin;
        --selected_tab;
        switch_tab = selected_tab;
    } else if( action == "PAGE_UP" ) {
        s = cataimgui::scroll::page_up;
    } else if( action == "PAGE_DOWN" ) {
        s = cataimgui::scroll::page_down;
    }

    refresh_lists();

    const size_t num_entries = selected_tab == mission_ui_tab_enum::POINTS_OF_INTEREST
                                ? upoints_of_interest.size() : umissions.size();
    const int last_entry = num_entries == 0 ? 0 : ( static_cast<int>( num_entries ) - 1 );
    if( action == "UP" ) {
        --selected_mission;
    } else if( action == "DOWN" ) {
        ++selected_mission;
    } else if( action == "HOME" ) {
        selected_mission = 0;
    } else if( action == "END" ) {
        selected_mission = last_entry;
    }
    if( selected_mission < 0 ) {
        selected_mission = last_entry;
    } else if( selected_mission > last_entry ) {
        selected_mission = 0;
    }

    // Needs the lists refresh_lists() just fetched above, same as the
    // original inline handling did after its own clamp.
    if( action == "CONFIRM" ) {
        if( selected_tab == mission_ui_tab_enum::ACTIVE && !umissions.empty() ) {
            get_avatar().set_active_mission( *umissions[selected_mission] );
        } else if( selected_tab == mission_ui_tab_enum::POINTS_OF_INTEREST &&
                   !upoints_of_interest.empty() ) {
            get_avatar().add_point_of_interest( upoints_of_interest[selected_mission] );
        }
    }
    if( action == "DELETE_POINT_OF_INTEREST" &&
        selected_tab == mission_ui_tab_enum::POINTS_OF_INTEREST && !upoints_of_interest.empty() ) {
        get_avatar().delete_point_of_interest( upoints_of_interest[selected_mission].pos );
    }
}

void mission_ui_impl::refresh_lists()
{
    umissions.clear();
    upoints_of_interest.clear();
    switch( selected_tab ) {
        case mission_ui_tab_enum::ACTIVE:
            umissions = get_avatar().get_active_missions();
            break;
        case mission_ui_tab_enum::COMPLETED:
            umissions = get_avatar().get_completed_missions();
            break;
        case mission_ui_tab_enum::FAILED:
            umissions = get_avatar().get_failed_missions();
            break;
        case mission_ui_tab_enum::POINTS_OF_INTEREST:
            upoints_of_interest = get_avatar().get_points_of_interest();
            break;
        default:
            break;
    }
}

void mission_ui_impl::draw_controls()
{
    ImGui::SetWindowSize( ImVec2( window_width, window_height ), ImGuiCond_Once );

    if( last_action == "QUIT" ) {
        return;
    }
    process_action( last_action );

    const bool adjust_selected = ( last_action == "NEXT_TAB" || last_action == "RIGHT" ||
                                    last_action == "PREV_TAB" || last_action == "LEFT" ||
                                    last_action == "UP" || last_action == "DOWN" ||
                                    last_action == "HOME" || last_action == "END" );

    ImGuiTabItemFlags_ flags = ImGuiTabItemFlags_None;

    if( ImGui::BeginTabBar( "##TAB_BAR" ) ) {
        flags = ImGuiTabItemFlags_None;
        if( switch_tab == mission_ui_tab_enum::ACTIVE ) {
            flags = ImGuiTabItemFlags_SetSelected;
            switch_tab = mission_ui_tab_enum::num_tabs;
        }
        if( ImGui::BeginTabItem( _( "ACTIVE" ), nullptr, flags ) ) {
            selected_tab = mission_ui_tab_enum::ACTIVE;
            ImGui::EndTabItem();
        }
        flags = ImGuiTabItemFlags_None;
        if( switch_tab == mission_ui_tab_enum::COMPLETED ) {
            flags = ImGuiTabItemFlags_SetSelected;
            switch_tab = mission_ui_tab_enum::num_tabs;
        }
        if( ImGui::BeginTabItem( _( "COMPLETED" ), nullptr, flags ) ) {
            selected_tab = mission_ui_tab_enum::COMPLETED;
            ImGui::EndTabItem();
        }
        flags = ImGuiTabItemFlags_None;
        if( switch_tab == mission_ui_tab_enum::FAILED ) {
            flags = ImGuiTabItemFlags_SetSelected;
            switch_tab = mission_ui_tab_enum::num_tabs;
        }
        if( ImGui::BeginTabItem( _( "FAILED" ), nullptr, flags ) ) {
            selected_tab = mission_ui_tab_enum::FAILED;
            ImGui::EndTabItem();
        }
        flags = ImGuiTabItemFlags_None;
        if( switch_tab == mission_ui_tab_enum::POINTS_OF_INTEREST ) {
            flags = ImGuiTabItemFlags_SetSelected;
            switch_tab = mission_ui_tab_enum::num_tabs;
        }
        if( ImGui::BeginTabItem( _( "POINTS OF INTEREST" ), nullptr, flags ) ) {
            selected_tab = mission_ui_tab_enum::POINTS_OF_INTEREST;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    // A mouse click may have just switched selected_tab inside the tab bar
    // above; process_action() ran before it and fetched the previous tab's
    // rows, so refetch here to keep the list and the tab in step regardless
    // of which path changed it.
    refresh_lists();

    if( ( selected_tab != mission_ui_tab_enum::POINTS_OF_INTEREST && umissions.empty() ) ||
        ( selected_tab == mission_ui_tab_enum::POINTS_OF_INTEREST && upoints_of_interest.empty() ) ) {
        ImGui::TextWrapped( "%s", empty_text_for( selected_tab ).translated().c_str() );
        return;
    }

    if( get_avatar().get_active_mission() ) {
        ImGui::TextWrapped( _( "Current objective: %s" ),
                            get_avatar().get_active_mission()->name().c_str() );
    } else if( get_avatar().get_active_point_of_interest().pos != tripoint_abs_omt::invalid ) {
        ImGui::TextWrapped( _( "Current point of interest: %s" ),
                            get_avatar().get_active_point_of_interest().text.c_str() );
    }

    if( ImGui::BeginTable( "##MISSION_TABLE", 2, ImGuiTableFlags_None,
                           ImGui::GetContentRegionAvail() ) ) {
        // Missions selection is purposefully thinner than the description, it has less to convey.
        if( selected_tab != mission_ui_tab_enum::POINTS_OF_INTEREST ) {
            ImGui::TableSetupColumn( _( "Missions" ), ImGuiTableColumnFlags_WidthStretch,
                                     table_column_width * 0.8 );
        } else {
            ImGui::TableSetupColumn( _( "Points of Interest" ), ImGuiTableColumnFlags_WidthStretch,
                                     table_column_width * 0.8 );
        }
        ImGui::TableSetupColumn( _( "Description" ), ImGuiTableColumnFlags_WidthStretch,
                                 table_column_width * 1.2 );
        ImGui::TableHeadersRow();
        ImGui::TableNextColumn();
        if( selected_tab != mission_ui_tab_enum::POINTS_OF_INTEREST ) {
            draw_mission_names( adjust_selected );
        } else {
            draw_point_of_interest_names( adjust_selected );
        }
        ImGui::TableNextColumn();
        if( selected_tab != mission_ui_tab_enum::POINTS_OF_INTEREST ) {
            draw_selected_description();
        } else {
            draw_selected_description_poi();
        }
        ImGui::EndTable();
    }

    cataimgui::set_scroll( s );
}

void mission_ui_impl::draw_mission_names( bool need_adjust ) const
{
    const int num_missions = umissions.size();

    if( ImGui::BeginListBox( "##LISTBOX", ImVec2( table_column_width * 0.75,
                             ImGui::GetContentRegionAvail().y ) ) ) {
        for( int i = 0; i < num_missions; i++ ) {
            const bool is_selected = selected_mission == i;
            ImGui::PushID( i );
            if( ImGui::Selectable( umissions[i]->name().c_str(), is_selected,
                                   ImGuiSelectableFlags_AllowDoubleClick ) ) {
                if( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) ) {
                    get_avatar().set_active_mission( *umissions[i] );
                }
            }

            if( is_selected && need_adjust ) {
                ImGui::SetScrollHereY();
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndListBox();
    }
}

void mission_ui_impl::draw_point_of_interest_names( bool need_adjust ) const
{
    const int num_missions = upoints_of_interest.size();

    if( ImGui::BeginListBox( "##LISTBOX", ImVec2( table_column_width * 0.75,
                             ImGui::GetContentRegionAvail().y ) ) ) {
        for( int i = 0; i < num_missions; i++ ) {
            const bool is_selected = selected_mission == i;
            ImGui::PushID( i );
            if( ImGui::Selectable( upoints_of_interest[i].text.c_str(), is_selected,
                                   ImGuiSelectableFlags_AllowDoubleClick ) ) {
                if( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) ) {
                    get_avatar().set_active_point_of_interest( upoints_of_interest[i] );
                }
            }

            if( is_selected && need_adjust ) {
                ImGui::SetScrollHereY();
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndListBox();
    }
}

void mission_ui_impl::draw_label_with_value( const std::string &label,
        const std::string &value ) const
{
    ImGui::TextColored( c_white, "%s", label.c_str() );
    const float label_width = table_column_width * 0.3f;
    ImGui::SameLine( label_width );
    ImGui::TextColored( c_light_gray, "%s", value.c_str() );
}

void mission_ui_impl::draw_selected_description()
{
    mission *miss = umissions[selected_mission];
    ImGui::TextWrapped( _( "Mission: %s" ), miss->name().c_str() );
    npc *mission_giver = nullptr;
    if( miss->get_npc_id().is_valid() ) {
        mission_giver = g->find_npc( miss->get_npc_id() );
        if( mission_giver ) {
            draw_label_with_value( _( "Given by:" ), mission_giver->disp_name() );
            if( mission_giver->get_faction() && mission_giver->get_fac_id() != faction_no_faction ) {
                draw_label_with_value( _( "Faction:" ), mission_giver->get_faction()->get_name() );
            }
            const tripoint_abs_omt npc_location = mission_giver->pos_abs_omt();
            draw_location( _( "Map location:" ), npc_location );
        }
    }
    ImGui::Separator();
    // mission_detail() shares raw_description / parsed_description with this
    // pane -- both cache against the same "did the selected mission change"
    // check, so calling it here does not reparse.
    cataimgui::draw_colored_text( mission_detail( miss ), c_unset, table_column_width * 1.15 );
}

void mission_ui_impl::draw_selected_description_poi() const
{
    const point_of_interest &selected_point_of_interest = upoints_of_interest[selected_mission];
    ImGui::TextWrapped( _( "Point of Interest: %s" ), selected_point_of_interest.text.c_str() );
    ImGui::Separator();
    draw_location( _( "Target:" ), selected_point_of_interest.pos );
}

void mission_ui_impl::draw_location( const std::string &label,
                                     const tripoint_abs_omt &loc ) const
{
    const tripoint_abs_omt pos = get_player_character().pos_abs_omt();
    draw_label_with_value( label, display::overmap_position_text( loc ) );
    if( !you_know_where_you_are() ) {
        // Don't display "Distance:" or direction arrow if we don't know where we are
        return;
    }
    int omt_distance = rl_dist( pos, loc );
    if( omt_distance > 0 ) {
        // One OMT is 24 tiles across, at 1x1 meters each, so we can simply do number of OMTs * 24
        units::length actual_distance = omt_distance * 24_meter;
        const std::string dir_arrow = direction_arrow( direction_from( pos.xy(), loc.xy() ) );
        //~Parenthesis is a real-world value for distance. Example string: "223 tiles (5.35km) ⇗"
        const std::string distance_str = string_format( _( "%1$d tiles (%2$s) %3$s" ),
                                         omt_distance, length_to_string_approx( actual_distance ), dir_arrow );
        draw_label_with_value( _( "Distance:" ), distance_str );
    }
}

// Below: a text-blob version of the detail panes above, and the location
// helper they share, used by publish_to_godot(). The two draw_selected_*
// functions above stay ImGui-only and are not called off the ImGui thread;
// these are what the Godot panel reads instead.

std::string mission_ui_impl::location_text( const std::string &label,
        const tripoint_abs_omt &loc ) const
{
    const tripoint_abs_omt pos = get_player_character().pos_abs_omt();
    std::string out = colorize( label, c_white ) + " " +
                       colorize( display::overmap_position_text( loc ), c_light_gray ) + "\n";
    if( !you_know_where_you_are() ) {
        return out;
    }
    int omt_distance = rl_dist( pos, loc );
    if( omt_distance > 0 ) {
        units::length actual_distance = omt_distance * 24_meter;
        const std::string dir_arrow = direction_arrow( direction_from( pos.xy(), loc.xy() ) );
        const std::string distance_str = string_format( _( "%1$d tiles (%2$s) %3$s" ),
                                         omt_distance, length_to_string_approx( actual_distance ), dir_arrow );
        out += colorize( _( "Distance:" ), c_white ) + " " + colorize( distance_str, c_light_gray ) + "\n";
    }
    return out;
}

std::string mission_ui_impl::mission_detail( mission *miss )
{
    std::string out = string_format( _( "Mission: %s" ), miss->name() ) + "\n";
    npc *mission_giver = nullptr;
    if( miss->get_npc_id().is_valid() ) {
        mission_giver = g->find_npc( miss->get_npc_id() );
        if( mission_giver ) {
            out += colorize( _( "Given by:" ), c_white ) + " " +
                   colorize( mission_giver->disp_name(), c_light_gray ) + "\n";
            if( mission_giver->get_faction() && mission_giver->get_fac_id() != faction_no_faction ) {
                out += colorize( _( "Faction:" ), c_white ) + " " +
                       colorize( mission_giver->get_faction()->get_name(), c_light_gray ) + "\n";
            }
            out += location_text( _( "Map location:" ), mission_giver->pos_abs_omt() );
        }
    }
    out += "\n";
    // Same cache as the ImGui pane's, and shared with it: both key off "did
    // the selected mission's raw description change", so switching between
    // this and the legacy pane on the same mission never reparses twice.
    if( raw_description != miss->get_description() ) {
        raw_description = miss->get_description();
        parsed_description = raw_description;
        dialogue d( get_talker_for( get_avatar() ), nullptr, {} );
        const talk_effect_fun_t::likely_rewards_t &rewards = miss->get_likely_rewards();
        for( const auto &reward : rewards ) {
            std::string token = "<reward_count:" + itype_id( reward.second.evaluate( d ) ).str() + ">";
            parsed_description = string_replace( parsed_description, token, string_format( "%g",
                                                 reward.first.evaluate( d ) ) );
        }
        const Character &other_talker = mission_giver ? *mission_giver : get_player_character();
        parse_tags( parsed_description, get_player_character(), other_talker );
    }
    out += parsed_description + "\n";
    if( miss->has_deadline() ) {
        const time_point deadline = miss->get_deadline();
        if( selected_tab == mission_ui_tab_enum::ACTIVE ) {
            out += string_format( _( "Deadline: %s" ), to_string( deadline ) ) + "\n";
            const time_duration remaining = deadline - calendar::turn;
            std::string remaining_time;
            if( remaining <= 0_turns ) {
                remaining_time = _( "None!" );
            } else if( get_player_character().has_watch() ) {
                remaining_time = to_string( remaining );
            } else {
                remaining_time = to_string_approx( remaining );
            }
            out += string_format( _( "Time remaining: %s" ), remaining_time ) + "\n";
        } else {
            const time_duration time_in_past = calendar::turn - deadline;
            std::string time_in_past_string;
            if( get_player_character().has_watch() ) {
                time_in_past_string = to_string( time_in_past );
            } else {
                time_in_past_string = to_string_approx( time_in_past );
            }
            if( deadline != calendar::turn_zero ) {
                if( selected_tab == mission_ui_tab_enum::COMPLETED ) {
                    //~The replaced string is a calendar date, such as "Year 1, May 12, 08:04:32"
                    out += colorize( string_format( _( "Completed: %s" ), to_string( deadline ) ),
                                     c_green ) + "\n";
                } else if( selected_tab == mission_ui_tab_enum::FAILED ) {
                    //~The replaced string is a calendar date, such as "Year 1, May 12, 08:04:32"
                    out += colorize( string_format( _( "Failed at: %s" ), to_string( deadline ) ),
                                     c_red ) + "\n";
                }
                //~The replaced string is a time duration, such as "12 hours", or "5 minutes"
                out += string_format( _( "%s ago" ), time_in_past_string ) + "\n";
            }
        }
    }
    if( miss->has_target() ) {
        // TODO: target does not contain a z-component, targets are assumed to be on z=0
        out += location_text( _( "Target:" ), miss->get_target() );
    }
    dimension_id mission_dimension = miss->get_dimension();
    if( mission_dimension != dimension_world_default ) {
        out += colorize( _( "Dimension:" ), c_white ) + " " +
               colorize( mission_dimension.str(), c_light_gray ) + "\n";
    }
    return out;
}

std::string mission_ui_impl::poi_detail( const point_of_interest &poi ) const
{
    std::string out = string_format( _( "Point of Interest: %s" ), poi.text ) + "\n";
    out += location_text( _( "Target:" ), poi.pos );
    return out;
}

#if defined(GODOT)

void mission_ui_impl::select_row( const int index )
{
    const size_t count = selected_tab == mission_ui_tab_enum::POINTS_OF_INTEREST
                          ? upoints_of_interest.size() : umissions.size();
    if( index >= 0 && index < static_cast<int>( count ) ) {
        selected_mission = index;
    }
}

void mission_ui_impl::set_tab( const int index )
{
    if( index < 0 || index >= static_cast<int>( mission_ui_tab_enum::num_tabs ) ) {
        return;
    }
    selected_tab = static_cast<mission_ui_tab_enum>( index );
    switch_tab = selected_tab;
    selected_mission = 0;
    refresh_lists();
}

void mission_ui_impl::publish_to_godot()
{
    using snapshot = godot_backend::MissionSnapshot;
    snapshot::data d;
    d.title = _( "Your missions" );
    d.tab_titles = { _( "ACTIVE" ), _( "COMPLETED" ), _( "FAILED" ), _( "POINTS OF INTEREST" ) };
    d.selected_tab = static_cast<int>( selected_tab );
    d.selected_row = selected_mission;

    if( selected_tab == mission_ui_tab_enum::POINTS_OF_INTEREST ) {
        d.rows.reserve( upoints_of_interest.size() );
        for( const point_of_interest &poi : upoints_of_interest ) {
            d.rows.push_back( poi.text );
        }
        if( upoints_of_interest.empty() ) {
            d.empty_text = empty_text_for( selected_tab ).translated();
        } else if( selected_mission >= 0 &&
                   selected_mission < static_cast<int>( upoints_of_interest.size() ) ) {
            d.detail = poi_detail( upoints_of_interest[selected_mission] );
            d.can_delete = true;
        }
    } else {
        d.rows.reserve( umissions.size() );
        for( mission *m : umissions ) {
            d.rows.push_back( m->name() );
        }
        if( umissions.empty() ) {
            d.empty_text = empty_text_for( selected_tab ).translated();
        } else if( selected_mission >= 0 && selected_mission < static_cast<int>( umissions.size() ) ) {
            d.detail = mission_detail( umissions[selected_mission] );
        }
    }

    if( get_avatar().get_active_mission() ) {
        d.current_objective = string_format( _( "Current objective: %s" ),
                                             get_avatar().get_active_mission()->name() );
    } else if( get_avatar().get_active_point_of_interest().pos != tripoint_abs_omt::invalid ) {
        d.current_objective = string_format( _( "Current point of interest: %s" ),
                                             get_avatar().get_active_point_of_interest().text );
    }

    godot_backend::get_mission_snapshot().publish( d );
}

bool mission_ui_impl::run_in_godot()
{
    godot_backend::MissionSnapshot &snap = godot_backend::get_mission_snapshot();
    snap.clear();
    refresh_lists();
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
        } else {
            process_action( action );
        }
        publish_to_godot();
    }
    snap.clear();
    return true;
}

#endif // GODOT

} // namespace

void game::list_missions()
{
    mission_ui new_instance;
    new_instance.draw_mission_ui();
}
