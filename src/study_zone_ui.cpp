#include "study_zone_ui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cata_imgui.h"
#include "character_id.h"
#include "game.h"
#include "godot_study_zone_snapshot.h"
#include "imgui/imgui.h"
#include "input_context.h"
#include "localized_comparator.h"
#include "memory_fast.h"
#include "npc.h"
#include "overmapbuffer.h"
#include "skill.h"
#include "translations.h"
#include "ui_manager.h"


static int filter_skill_input_callback( ImGuiInputTextCallbackData *data )
{
    if( data->EventChar < 256 ) {
        char c = static_cast<char>( data->EventChar );
        if( !std::islower( c ) && c != ' ' ) {
            return 1;
        }
    }
    return 0;
}

namespace
{
class study_zone_window : public cataimgui::window
{
    public:
        explicit study_zone_window( std::map<std::string, std::set<skill_id>> &npc_skill_preferences ) :
            cataimgui::window( _( "Study Zone Skill Preferences" ), ImGuiWindowFlags_NoNavInputs ),
            npc_skill_preferences( npc_skill_preferences ) {
            ctxt = input_context( "STUDY_ZONE_UI" );
            ctxt.register_action( "CONFIRM" );
            ctxt.register_action( "FILTER" );
            ctxt.register_action( "QUIT" );
            ctxt.set_timeout( 10 );

            // Get all skills
            for( const Skill &s : Skill::skills ) {
                all_skills.push_back( s.ident() );
            }
            std::sort( all_skills.begin(), all_skills.end(),
            []( const skill_id & a, const skill_id & b ) {
                return localized_compare( a->name(), b->name() );
            } );

            // get followers
            std::set<character_id> follower_ids = g->get_follower_list();
            for( const character_id &id : follower_ids ) {
                shared_ptr_fast<npc> npc_ptr = overmap_buffer.find_npc( id );
                if( npc_ptr && !npc_ptr->is_hallucination() && npc_ptr->is_player_ally() ) {
                    npc_names.push_back( npc_ptr->name );
                }
            }
            // include NPCs that are already in preferences but not in follower list
            for( const auto &pair : npc_skill_preferences ) {
                if( std::find( npc_names.begin(), npc_names.end(), pair.first ) == npc_names.end() ) {
                    npc_names.push_back( pair.first );
                }
            }
            std::sort( npc_names.begin(), npc_names.end(), localized_compare );

            // for new NPCs set all checkboxes
            for( const std::string &npc_name : npc_names ) {
                if( npc_skill_preferences.find( npc_name ) == npc_skill_preferences.end() ) {
                    std::set<skill_id> &npc_skills = npc_skill_preferences[npc_name];
                    for( const skill_id &skill : all_skills ) {
                        npc_skills.insert( skill );
                    }
                    preferences_changed = true;
                }
            }

            max_skill_name_width = 0.0f;
            for( const skill_id &skill : all_skills ) {
                ImVec2 text_size = ImGui::CalcTextSize( skill->name().c_str() );
                float width = text_size.x + ImGui::GetStyle().FramePadding.x * 2.0f;
                max_skill_name_width = std::max( max_skill_name_width, width );
            }

            max_npc_name_width = 0.0f;
            for( const std::string &npc_name : npc_names ) {
                ImVec2 text_size = ImGui::CalcTextSize( npc_name.c_str() );
                float width = text_size.x + ImGui::GetStyle().FramePadding.x * 2.0f;
                if( width > max_npc_name_width ) {
                    max_npc_name_width = width;
                }
            }
        }

        study_zone_ui_result execute() {
#if defined(GODOT)
            // Same loop-split takeover as the other MENU-13 screens; only the
            // source of a toggle/filter moves. It declines when no panel is
            // attending, and the legacy ImGui loop below runs instead.
            {
                study_zone_ui_result godot_result = study_zone_ui_result::canceled;
                if( run_in_godot( godot_result ) ) {
                    return godot_result;
                }
            }
#endif
            bool canceled_result = false;
            bool confirmed = false;
            while( get_is_open() ) {
                ui_manager::redraw();
                std::string action = ctxt.handle_input();

                if( action == "CONFIRM" ) {
                    confirmed = true;
                    break;
                }

                if( action == "QUIT" ) {
                    if( !skill_filter.empty() ) {
                        skill_filter.clear();
                    } else {
                        canceled_result = true;
                        break;
                    }
                }
                if( action == "FILTER" ) {
                    filter_just_focused = true;
                }
            }

            if( done_clicked ) {
                confirmed = true;
            }

            if( !get_is_open() && !confirmed && !canceled_result ) {
                canceled_result = true;
            }

            if( canceled_result ) {
                return study_zone_ui_result::canceled;
            }
            return preferences_changed ? study_zone_ui_result::changed : study_zone_ui_result::successful;
        }

    protected:
        void draw_skill_row( const skill_id &skill ) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text( "%s", skill->name().c_str() );

            for( const std::string &npc_name : npc_names ) {
                ImGui::TableNextColumn();

                std::set<skill_id> &npc_skills = npc_skill_preferences[npc_name];

                bool is_selected = npc_skills.count( skill ) > 0;
                bool was_selected = is_selected;

                // center the checkbox
                float offset = ( ImGui::GetColumnWidth() - ImGui::GetFrameHeight() ) * 0.5f;
                ImGui::SetCursorPosX( ImGui::GetCursorPosX() + offset );

                ImGui::PushID( checkbox_id_counter++ );
                if( ImGui::Checkbox( "##checkbox", &is_selected ) ) {
                    preferences_changed = true;
                }
                ImGui::PopID();
                if( is_selected != was_selected ) {
                    if( is_selected ) {
                        npc_skills.insert( skill );
                    } else {
                        npc_skills.erase( skill );
                    }
                    if( npc_skills.empty() ) {
                        npc_skill_preferences.erase( npc_name );
                    }
                }
            }
        }

        void draw_footer( const std::vector<skill_id> &filtered_skills ) {
            // filter input, buttons, and Done button
            std::string filter_label = _( "Filter skills: " );
            ImGui::TextUnformatted( filter_label.c_str() );
            ImGui::SameLine();

            if( filter_just_focused ) {
                ImGui::SetKeyboardFocusHere();
                filter_just_focused = false;
            }

            ImGui::SetNextItemWidth( 300.0f );
            std::array<char, 256> filter_buffer = {0};
            strncpy( filter_buffer.data(), skill_filter.c_str(), filter_buffer.size() - 1 );
            ImGui::InputText( "##skill_filter", filter_buffer.data(), filter_buffer.size(),
                              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
                              filter_skill_input_callback );
            skill_filter = filter_buffer.data();

            ImGui::SameLine();
            if( ImGui::Button( _( "Check All" ) ) ) {
                for( const std::string &npc_name : npc_names ) {
                    std::set<skill_id> &npc_skills = npc_skill_preferences[npc_name];
                    for( const skill_id &skill : filtered_skills ) {
                        npc_skills.insert( skill );
                    }
                }
                preferences_changed = true;
            }

            ImGui::SameLine();
            if( ImGui::Button( _( "Clear All" ) ) ) {
                for( const std::string &npc_name : npc_names ) {
                    std::set<skill_id> &npc_skills = npc_skill_preferences[npc_name];
                    for( const skill_id &skill : filtered_skills ) {
                        npc_skills.erase( skill );
                    }
                    if( npc_skills.empty() ) {
                        npc_skill_preferences.erase( npc_name );
                    }
                }
                preferences_changed = true;
            }

            ImGui::SameLine();
            if( ImGui::Button( _( "Done" ) ) ) {
                done_clicked = true;
                is_open = false;
            }
        }

        cataimgui::bounds get_bounds() override {
            ImVec2 viewport_size = ImGui::GetMainViewport()->Size;
            float width = std::min( 1200.0f, viewport_size.x * 0.9f );
            float height = viewport_size.y;
            return { -1.f, -1.f, width, height };
        }

        std::vector<skill_id> compute_filtered_skills() const {
            if( skill_filter.empty() ) {
                return all_skills;
            }
            std::vector<skill_id> filtered_skills;
            for( const skill_id &skill : all_skills ) {
                if( skill->name().find( skill_filter ) != std::string::npos ) {
                    filtered_skills.push_back( skill );
                }
            }
            return filtered_skills;
        }

        void draw_controls() override {
            checkbox_id_counter = 0;
            const std::vector<skill_id> filtered_skills = compute_filtered_skills();

            const float footer_height_to_reserve = ImGui::GetFrameHeightWithSpacing() * 3;

            // inner scroll
            if( ImGui::BeginChild( "table_scroll_region", ImVec2( 0, -footer_height_to_reserve ), false,
                                   ImGuiWindowFlags_HorizontalScrollbar ) ) {
                // Create a table with Skills as rows and npc names as columns
                if( ImGui::BeginTable( "skill_npc_table", static_cast<int>( npc_names.size() ) + 1,
                                       ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_RowBg ) ) {
                    // table header row
                    ImGui::TableSetupColumn( "Skill", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoHide,
                                             max_skill_name_width );
                    for( const std::string &npc_name : npc_names ) {
                        ImGui::TableSetupColumn( npc_name.c_str(), ImGuiTableColumnFlags_WidthFixed,
                                                 max_npc_name_width );
                    }
                    // freeze skill column horizontally and header vertically
                    ImGui::TableSetupScrollFreeze( 1, 1 );
                    ImGui::TableHeadersRow();

                    // skill rows
                    ImGuiListClipper clipper;
                    clipper.Begin( filtered_skills.size() );
                    while( clipper.Step() ) {
                        for( int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++ ) {
                            draw_skill_row( filtered_skills[row] );
                        }
                    }

                    ImGui::EndTable();
                }
            }
            ImGui::EndChild();

            draw_footer( filtered_skills );
        }

#if defined(GODOT)
        /// Show this screen as a Godot panel and block until it is dismissed.
        /// @return false when no panel attended, so the caller must run the
        ///         legacy ImGui loop instead.
        bool run_in_godot( study_zone_ui_result &result ) {
            godot_backend::StudyZoneSnapshot &snap = godot_backend::get_study_zone_snapshot();
            snap.clear();
            publish_to_godot();
            while( true ) {
                int skill_index = -1;
                int npc_index = -1;
                std::string filter_text;
                const std::string action = snap.next_action( skill_index, npc_index, filter_text );
                if( action.empty() ) {
                    // No panel attended; the caller runs the legacy ImGui loop.
                    snap.clear();
                    return false;
                }
                if( action == "QUIT" ) {
                    // Same two-stage escape as the ImGui loop: clear the filter
                    // first, and only close the screen on a second QUIT.
                    if( !skill_filter.empty() ) {
                        skill_filter.clear();
                        publish_to_godot();
                        continue;
                    }
                    result = study_zone_ui_result::canceled;
                    break;
                }
                if( action == "GODOT_TOGGLE" ) {
                    toggle( skill_index, npc_index );
                } else if( action == "GODOT_FILTER" ) {
                    skill_filter = filter_text;
                } else if( action == "CHECK_ALL" || action == "CLEAR_ALL" ) {
                    set_all( compute_filtered_skills(), action == "CHECK_ALL" );
                } else if( action == "DONE" ) {
                    result = preferences_changed ? study_zone_ui_result::changed :
                              study_zone_ui_result::successful;
                    break;
                }
                publish_to_godot();
            }
            snap.clear();
            return true;
        }
#endif

    private:
#if defined(GODOT)
        /// Toggle one checkbox, addressed by absolute indices -- mirrors the
        /// mutation `draw_skill_row()` makes from an ImGui checkbox click.
        void toggle( const int skill_index, const int npc_index ) {
            if( skill_index < 0 || skill_index >= static_cast<int>( all_skills.size() ) ||
                npc_index < 0 || npc_index >= static_cast<int>( npc_names.size() ) ) {
                return;
            }
            const skill_id &skill = all_skills[skill_index];
            const std::string &npc_name = npc_names[npc_index];
            std::set<skill_id> &npc_skills = npc_skill_preferences[npc_name];
            if( npc_skills.count( skill ) > 0 ) {
                npc_skills.erase( skill );
            } else {
                npc_skills.insert( skill );
            }
            if( npc_skills.empty() ) {
                npc_skill_preferences.erase( npc_name );
            }
            preferences_changed = true;
        }

        void set_all( const std::vector<skill_id> &skills, const bool checked ) {
            for( const std::string &npc_name : npc_names ) {
                std::set<skill_id> &npc_skills = npc_skill_preferences[npc_name];
                for( const skill_id &skill : skills ) {
                    if( checked ) {
                        npc_skills.insert( skill );
                    } else {
                        npc_skills.erase( skill );
                    }
                }
                if( npc_skills.empty() ) {
                    npc_skill_preferences.erase( npc_name );
                }
            }
            preferences_changed = true;
        }

        void publish_to_godot() {
            using snapshot = godot_backend::StudyZoneSnapshot;
            snapshot::data d;
            d.title = _( "Study Zone Skill Preferences" );
            d.npc_names = npc_names;
            d.filter = skill_filter;

            const std::vector<skill_id> filtered_skills = compute_filtered_skills();
            d.rows.reserve( filtered_skills.size() );
            for( const skill_id &skill : filtered_skills ) {
                snapshot::row r;
                r.skill_index = static_cast<int>(
                                     std::distance( all_skills.begin(),
                                                     std::find( all_skills.begin(), all_skills.end(), skill ) ) );
                r.skill_name = skill->name();
                r.checked.reserve( npc_names.size() );
                for( const std::string &npc_name : npc_names ) {
                    const std::set<skill_id> &npc_skills = npc_skill_preferences[npc_name];
                    r.checked.push_back( npc_skills.count( skill ) > 0 );
                }
                d.rows.push_back( std::move( r ) );
            }
            godot_backend::get_study_zone_snapshot().publish( d );
        }
#endif

        std::map<std::string, std::set<skill_id>> &npc_skill_preferences;
        std::vector<skill_id> all_skills;
        std::vector<std::string> npc_names;
        std::string skill_filter;
        bool filter_just_focused = false;
        bool preferences_changed = false;
        bool done_clicked = false;
        int checkbox_id_counter = 0;
        float max_skill_name_width = 0.0f;
        float max_npc_name_width = 0.0f;
        input_context ctxt;
};
} // namespace

study_zone_ui_result query_study_zone_skills( std::map<std::string, std::set<skill_id>>
        &npc_skill_preferences )
{
    study_zone_window window( npc_skill_preferences );
    return window.execute();
}

