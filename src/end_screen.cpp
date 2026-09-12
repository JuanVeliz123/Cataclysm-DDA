#include <imgui/imgui.h>
#include <algorithm>
#include <memory>

#include "avatar.h"
#include "ascii_art.h"
#include "cata_imgui.h"
#include "condition.h"
#include "dialogue.h"
#include "end_screen.h"
#include "event.h"
#include "event_bus.h"
#include "game.h"
#include "generic_factory.h"
#include "imgui/imgui_stdlib.h"
#include "input_context.h"
#include "npc.h" // for parse_tags()! Why!
#include "output.h"
#include "talker.h"
#include "ui_manager.h"

#if defined(GODOT)
#include "godot_end_screen_snapshot.h"
#endif

static const ascii_art_id ascii_art_ascii_tombstone( "ascii_tombstone" );

namespace
{
generic_factory<end_screen> end_screen_factory( "end_screen" );
} // namespace

template<>
const end_screen &string_id<end_screen>::obj()const
{
    return end_screen_factory.obj( *this );
}

template<>
bool string_id<end_screen>::is_valid() const
{
    return end_screen_factory.is_valid( *this );
}

void end_screen::load_end_screen( const JsonObject &jo, const std::string &src )
{
    end_screen_factory.load( jo, src );
}

void end_screen::load( const JsonObject &jo, std::string_view )
{
    mandatory( jo, was_loaded, "id", id );
    mandatory( jo, was_loaded, "picture_id", picture_id );
    mandatory( jo, was_loaded, "priority", priority );
    read_condition( jo, "condition", condition, false );

    optional( jo, was_loaded, "added_info", added_info );
    optional( jo, was_loaded, "last_words_label", last_words_label );
}

const std::vector<end_screen> &end_screen::get_all()
{
    return end_screen_factory.get_all();
}

namespace
{

struct end_screen_selection {
    ascii_art_id art = ascii_art_ascii_tombstone;
    std::vector<std::pair<std::pair<int, int>, std::string>> added_info;
    std::string input_label;
};

/// Picks the highest-priority `end_screen` whose condition matches. Shared by
/// the legacy ImGui draw and the Godot takeover so the two never disagree
/// about which screen is showing.
end_screen_selection select_end_screen()
{
    end_screen_selection sel;
    dialogue d( get_talker_for( get_avatar() ), nullptr );

    std::vector<end_screen> sorted_screens = end_screen::get_all();
    std::sort( sorted_screens.begin(), sorted_screens.end(), []( end_screen const & a,
    end_screen const & b ) {
        return a.priority > b.priority;
    } );

    for( const end_screen &e_screen : sorted_screens ) {
        if( e_screen.condition( d ) ) {
            sel.art = e_screen.picture_id;
            if( !e_screen.added_info.empty() ) {
                sel.added_info = e_screen.added_info;
            }
            if( !e_screen.last_words_label.empty() ) {
                sel.input_label = e_screen.last_words_label;
            }
            break;
        }
    }
    return sel;
}

#if defined(GODOT)
/// Plain-text render of the same art + overlay `select_end_screen()` picks,
/// for the Godot panel -- a `RichTextLabel` with no color support, matching
/// every other migrated text pane (`textwin_panel.gd`). Each overlay entry is
/// spliced into its row at its column by padding, then appended -- a
/// simplified re-presentation of the ImGui path's `ImGui::SameLine` cursor
/// placement, which can overwrite characters already drawn there; nothing in
/// this game's own `added_info` data ever expects that.
std::string compose_end_screen_body( const end_screen_selection &sel, avatar &u )
{
    std::string result;
    if( !sel.art.is_valid() ) {
        return result;
    }
    int row = 1;
    for( const std::string &line : sel.art->picture ) {
        std::string plain_line = remove_color_tags( line );
        for( const std::pair<std::pair<int, int>, std::string> &info : sel.added_info ) {
            if( row == info.first.second ) {
                std::string translated_info = _( info.second );
                parse_tags( translated_info, u, u );
                translated_info = remove_color_tags( translated_info );
                const size_t col = static_cast<size_t>( std::max( 0, info.first.first ) );
                if( plain_line.size() < col ) {
                    plain_line.resize( col, ' ' );
                }
                plain_line += translated_info;
            }
        }
        result += plain_line;
        result += "\n";
        row++;
    }
    return result;
}
#endif // GODOT

} // namespace

void end_screen_data::draw_end_screen_ui( bool actually_dead )
{
    input_context ctxt;
    ctxt.register_action( "TEXT.CONFIRM" );
    ctxt.set_timeout( 50 );
    end_screen_ui_impl p_impl;
    avatar &u = get_avatar();
    bool godot_handled = false;

#if defined(GODOT)
    {
        end_screen_selection sel = select_end_screen();
        godot_backend::EndScreenSnapshot &snap = godot_backend::get_end_screen_snapshot();
        godot_backend::EndScreenSnapshot::data d;
        d.body = compose_end_screen_body( sel, u );
        d.has_text_input = !sel.input_label.empty();
        d.text_label = sel.input_label;
        snap.clear();
        snap.publish( d );
        std::string text_out;
        const std::string action = snap.next_action( text_out );
        snap.clear();
        if( action == "CONFIRM" ) {
            p_impl.text = text_out;
            godot_handled = true;
        } else if( action == "QUIT" ) {
            // Shutdown requested -- resolve rather than parking the game
            // thread in the legacy loop below.
            godot_handled = true;
        }
    }
#endif

    if( !godot_handled ) {
        while( true ) {
            ui_manager::redraw_invalidated();
            std::string action = ctxt.handle_input();
            if( action == "TEXT.CONFIRM" || !p_impl.get_is_open() ) {
                break;
            }
        }
    }

    if( actually_dead ) {
        const bool is_suicide = g->uquit == QUIT_SUICIDE;
        get_event_bus().send<event_type::game_avatar_death>( u.getID(), u.name, is_suicide, p_impl.text );
    }
}

void end_screen_ui_impl::draw_controls()
{
    avatar &u = get_avatar();
    end_screen_selection sel = select_end_screen();

    if( sel.art.is_valid() ) {
        cataimgui::PushMonoFont();
        int row = 1;
        for( const std::string &line : sel.art->picture ) {
            cataimgui::draw_colored_text( line );

            for( const std::pair<std::pair<int, int>, std::string> &info : sel.added_info ) {
                if( row == info.first.second ) {
                    std::string translated_info = _( info.second );
                    parse_tags( translated_info, u, u );
                    ImGui::SameLine( str_width_to_pixels( info.first.first ), 0 );
                    cataimgui::draw_colored_text( translated_info );
                }
            }
            row++;
        }
        ImGui::PopFont();
    }

    if( !sel.input_label.empty() ) {
        ImGui::NewLine();
        ImGui::AlignTextToFramePadding();
        cataimgui::draw_colored_text( _( sel.input_label ) );
        ImGui::SameLine( str_width_to_pixels( sel.input_label.size() + 2 ), 0 );
        ImGui::InputText( "##LAST_WORD_BOX", &text );
        ImGui::SetKeyboardFocusHere( -1 );
    }

}
