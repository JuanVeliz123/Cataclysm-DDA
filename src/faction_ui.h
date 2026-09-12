#pragma once
#ifndef CATA_SRC_FACTION_UI_H
#define CATA_SRC_FACTION_UI_H

#include <algorithm>
#include <string>
#include <vector>

#include <imgui/imgui.h>

#include "cata_imgui.h"
#include "game_constants.h"
#include "input_context.h"
#include "translations.h"
#include "type_id.h"

class basecamp;
class faction;
class npc;

enum class tab_mode : int {
    TAB_MYFACTION = 0,
    TAB_FOLLOWERS,
    TAB_OTHERFACTIONS,
    TAB_CREATURES,
    NUM_TABS
};

enum class radio_contact_result : int {
    ALPHA_NO_RADIO,
    BETA_NO_RADIO,
    BOTH_NO_RADIO,
    TOO_FAR,
    YES,
    last
};

class faction_ui : public cataimgui::window
{
    public:
        explicit faction_ui( ) : cataimgui::window( _( "Faction" ),
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav ) {
        };

        bool execute();

#if defined(GODOT)
        /// Show this screen as a Godot panel and block until it is dismissed.
        /// @return false when no panel attended, so the caller must run the
        ///         legacy ImGui loop instead.
        bool run_in_godot();
#endif

        void draw_hint_section() const;

        void draw_your_faction_tab();
        void draw_your_factions_list();
        void your_faction_display() const;

        void draw_your_followers_tab();
        void draw_your_followers_list();
        void your_follower_display();

        void draw_other_factions_tab();
        void draw_other_factions_list();
        void other_faction_display();

        // i think creature tab better be migrated to diary and adjacent UI
        void draw_creatures_tab();
        void draw_creature_list();
        void creature_display() const;

        void radio_the_faction();

        // Fetch-only versions of the four draw_*_list()s above, and
        // text-blob versions of the four *_display()s, shared by the Godot
        // panel. The legacy draw functions are untouched and keep doing
        // their own fetching -- these exist alongside them rather than
        // replacing anything, so the ImGui path carries zero risk from this.
        std::vector<basecamp *> get_camps() const;
        std::vector<npc *> get_followers() const;
        std::vector<const faction *> get_other_factions() const;
        std::vector<const mtype_id *> get_creatures() const;
        std::string camp_detail( basecamp *camp ) const;
        std::string follower_detail( npc *follower ) const;
        std::string other_faction_detail( const faction *fac ) const;
        std::string creature_detail( const mtype_id *creature ) const;

#if defined(GODOT)
        /// Move the row cursor for the active tab (UP/DOWN/HOME/END) and
        /// switch tabs (NEXT_TAB/PREV_TAB) -- the part of draw_controls()'s
        /// per-tab list functions that is not itself an ImGui draw call.
        void process_action( const std::string &action );
        void publish_to_godot();
        void select_row( int index );
        void set_tab( int index );
#endif

        std::string last_action;
    protected:

        void draw_controls() override;
        cataimgui::bounds get_bounds() override {

            const float window_width = std::clamp( float( str_width_to_pixels( EVEN_MINIMUM_TERM_WIDTH ) ),
                                                   ImGui::GetMainViewport()->Size.x / 2,
                                                   ImGui::GetMainViewport()->Size.x );
            const float window_height = std::clamp( float( str_height_to_pixels( EVEN_MINIMUM_TERM_HEIGHT ) ),
                                                    ImGui::GetMainViewport()->Size.y / 2,
                                                    ImGui::GetMainViewport()->Size.y );

            const cataimgui::bounds bounds{ -1.f, -1.f, window_width, window_height };
            return bounds;
        }

    private:
        input_context ctxt;
        cataimgui::scroll s = cataimgui::scroll::none;
        tab_mode selected_tab = tab_mode::TAB_MYFACTION;
        // hack, hide the window if picked talking to someone
        // remove when npc dialogue menu will be made imgui
        bool hide_ui = false;

        basecamp *picked_camp = nullptr;
        npc *picked_follower = nullptr;
        const faction *picked_faction = nullptr;
        const mtype_id *picked_creature = nullptr;

        float get_table_column_width() const {
            return std::min( ImGui::GetWindowSize().x / 2.5f, 256.f );
        }
};

#endif // CATA_SRC_FACTION_UI_H
