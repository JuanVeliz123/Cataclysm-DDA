#include "help.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <iterator>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

#include "action.h"
#include "cata_path.h"
#include "cata_utility.h"
#include "color.h"
#include "cursesdef.h"
#include "debug.h"
#include "flexbuffer_json.h"
#include "input_context.h"
#include "input_enums.h"
#include "output.h"
#include "path_info.h"
#include "string_formatter.h"
#include "text_snippets.h"
#include "translations.h"
#include "uilist.h"
#include "ui_helpers.h"
#include "ui_manager.h"
#if defined(GODOT)
#include "godot_textwin_snapshot.h"
#endif

help &get_help()
{
    static help single_instance;
    return single_instance;
}

void help::load( const JsonObject &jo, const std::string &src )
{
    get_help().load_object( jo, src );
}

void help::reset()
{
    get_help().reset_instance();
}

void help::reset_instance()
{
    current_order_start = 0;
    current_src = "";
    help_texts.clear();
}

void help::load_object( const JsonObject &jo, const std::string &src )
{
    if( src == "dda" ) {
        jo.throw_error( string_format( "Vanilla help must be located in %s",
                                       PATH_INFO::jsondir().generic_u8string() ) );
    }
    if( src != current_src ) {
        current_order_start = help_texts.empty() ? 0 : help_texts.crbegin()->first + 1;
        current_src = src;
    }
    std::vector<translation> messages;
    jo.read( "messages", messages );

    translation name;
    jo.read( "name", name );
    const int modified_order = jo.get_int( "order" ) + current_order_start;
    if( !help_texts.try_emplace( modified_order, std::make_pair( name, messages ) ).second ) {
        jo.throw_error_at( "order", "\"order\" must be unique (per src)" );
    }
}

std::string help::get_dir_grid()
{
    static const std::array<action_id, 9> movearray = {{
            ACTION_MOVE_FORTH_LEFT, ACTION_MOVE_FORTH, ACTION_MOVE_FORTH_RIGHT,
            ACTION_MOVE_LEFT,  ACTION_PAUSE,  ACTION_MOVE_RIGHT,
            ACTION_MOVE_BACK_LEFT, ACTION_MOVE_BACK, ACTION_MOVE_BACK_RIGHT
        }
    };

    std::string movement = "<LEFTUP_0>  <UP_0>  <RIGHTUP_0>   <LEFTUP_1>  <UP_1>  <RIGHTUP_1>\n"
                           " \\ | /     \\ | /\n"
                           "  \\|/       \\|/\n"
                           "<LEFT_0>--<pause_0>--<RIGHT_0>   <LEFT_1>--<pause_1>--<RIGHT_1>\n"
                           "  /|\\       /|\\\n"
                           " / | \\     / | \\\n"
                           "<LEFTDOWN_0>  <DOWN_0>  <RIGHTDOWN_0>   <LEFTDOWN_1>  <DOWN_1>  <RIGHTDOWN_1>";

    for( action_id dir : movearray ) {
        std::vector<input_event> keys = keys_bound_to( dir, /*maximum_modifier_count=*/0 );
        for( size_t i = 0; i < 2; i++ ) {
            movement = string_replace( movement, "<" + action_ident( dir ) + string_format( "_%d>", i ),
                                       i < keys.size()
                                       ? string_format( "<color_light_blue>%s</color>",
                                               keys[i].short_description() )
                                       : "<color_red>?</color>" );
        }
    }

    return movement;
}

std::string help::get_note_colors()
{
    std::string text = _( "Note colors: " );
    for( const auto &color_pair : get_note_color_names() ) {
        // The color index is not translatable, but the name is.
        //~ %1$s: note color abbreviation, %2$s: note color name
        text += string_format( pgettext( "note color", "%1$s:%2$s, " ),
                               colorize( color_pair.first, color_pair.second.color ),
                               color_pair.second.name );
    }

    return text;
}

void help::display_help() const
{
    if( help_texts.empty() ) {
        return;
    }

    catacurses::window w_help;
    catacurses::window w_help_border;

    ui_adaptor ui;
    const auto init_windows = [&]( ui_adaptor & ui ) {
        ui_helpers::full_screen_window( ui, &w_help, &w_help_border, nullptr, nullptr, nullptr, 1 );
    };
    // Never called eagerly -- the same fix color_manager's and safemode's
    // migrations needed: FULL_SCREEN_WIDTH is never initialized under GODOT
    // (that happens in main.cpp's terminal-size negotiation, which the
    // GODOT backend does not run). `get_w_help_border` below calls it only
    // when the detail view actually falls back to `scrollable_text`, which
    // happens only once an unattended `run_textwin_in_godot()` call declines.

    while( true ) {
        // The category picker: a plain `uilist` -- already a migrated Godot
        // panel -- stands in for the bespoke clickable grid `draw_menu()`
        // used to build, the same MENU-4 "drive the existing implementation"
        // insight the `help` row in BACKLOG.md proposed. It gets mouse
        // selection, hotkey assignment and Godot routing for free.
        uilist menu;
        menu.title = _( "Help" );
        menu.text = _( "Please press one of the following for help on that topic:" );
        for( const auto &text : help_texts ) {
            menu.addentry( text.first, true, MENU_AUTOASSIGN, text.second.first.translated() );
        }
        menu.query();
        const int selection = menu.ret;
        if( selection < 0 ) {
            break;
        }

        const auto help_text_it = help_texts.find( selection );
        if( help_text_it == help_texts.end() ) {
            continue;
        }

        std::vector<std::string> i18n_help_texts;
        i18n_help_texts.reserve( help_text_it->second.second.size() );
        std::transform( help_text_it->second.second.begin(), help_text_it->second.second.end(),
        std::back_inserter( i18n_help_texts ), [&]( const translation & line ) {
            std::string line_proc = line.translated();
            if( line_proc == "<DRAW_NOTE_COLORS>" ) {
                line_proc = get_note_colors();
            } else if( line_proc == "<HELP_DRAW_DIRECTIONS>" ) {
                line_proc = get_dir_grid();
            }
            size_t pos = line_proc.find( "<press_", 0, 7 );
            while( pos != std::string::npos ) {
                size_t pos2 = line_proc.find( ">", pos, 1 );

                std::string action = line_proc.substr( pos + 7, pos2 - pos - 7 );
                std::string replace = "<color_light_blue>" +
                                      press_x( look_up_action( action ), "", "" ) + "</color>";

                if( replace.empty() ) {
                    debugmsg( "Help json: Unknown action: %s", action );
                } else {
                    line_proc = string_replace(
                                    line_proc, "<press_" + std::move( action ) + ">", replace );
                }

                pos = line_proc.find( "<press_", pos2, 7 );
            }
            return line_proc;
        } );

        if( i18n_help_texts.empty() ) {
            continue;
        }

        const std::string body = std::accumulate( i18n_help_texts.begin() + 1, i18n_help_texts.end(),
                                  i18n_help_texts.front(),
        []( std::string lhs, const std::string & rhs ) {
            return std::move( lhs ) + "\n\n" + rhs;
        } );

#if defined(GODOT)
        // Formatted text you scroll and dismiss, so it goes to the shared
        // Godot text window (MENU-3's `godot_textwin_snapshot.*`) rather
        // than `scrollable_text`'s curses/ImGui window -- the same move
        // `ui_iteminfo.cpp`'s `execute()` makes for item info.
        {
            std::vector<godot_backend::TextWinSnapshot::tab> tabs;
            tabs.push_back( { std::string(), remove_color_tags( body ) } );
            int current = 0;
            if( godot_backend::run_textwin_in_godot( _( "Help" ),
                    remove_color_tags( help_text_it->second.first.translated() ), tabs, current ) ) {
                continue;
            }
        }
#endif

        const auto get_w_help_border = [&]() {
            init_windows( ui );
            return w_help_border;
        };
        scrollable_text( get_w_help_border, _( "Help" ), body );
    }
}

std::string get_hint()
{
    return SNIPPET.random_from_category( "hint" ).value_or( translation() ).translated();
}
