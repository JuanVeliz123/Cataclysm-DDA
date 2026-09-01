#include "godot_backend.h"
#include "godot_display.h"
#include "godot_anim_snapshot.h"
#include "godot_look_snapshot.h"
#include "godot_overmap_snapshot.h"
#include "godot_pixel_minimap.h"
#include "godot_view_snapshot.h"
#include "cursesport.h"
#include "cursesdef.h"
#include "output.h"
#include "catacharset.h"
#include "color_loader.h"
#include "point.h"
#include "color.h"
#include "cata_imgui.h"
#include "ui_manager.h"
#include "game.h"
#include "game_constants.h"
#include "path_info.h"

#if defined(GODOT)

#include <atomic>
#include <algorithm>
#include <memory>
#include <vector>

#include <imgui/imgui.h>
#include <imtui/imtui.h>

namespace godot_backend
{

static palette_array active_palette;

static int font_w = 8;
static int font_h = 16;
static int term_cols = EVEN_MINIMUM_TERM_WIDTH;
static int term_rows = EVEN_MINIMUM_TERM_HEIGHT;

// Set from the Godot main thread; applied on the game thread.
static std::atomic<int> pending_pixel_w{0};
static std::atomic<int> pending_pixel_h{0};

// Everything below down to the catacurses wrappers is internal to this
// translation unit; the module's public surface is in godot_backend.h.
color curses_color_to_color( const nc_color &color )
{
    const int pair_id = color.to_color_pair_index();
    const cata_cursesport::pairs pair = cata_cursesport::colorpairs[pair_id];

    int palette_index = pair.FG != 0 ? pair.FG : pair.BG;

    if( color.is_bold() ) {
        palette_index += color_loader<godot_backend::color>::COLOR_NAMES_COUNT / 2;
    }


    return active_palette[palette_index];
}

static int get_color_index( cata_cursesport::base_color c )
{
    if( c == 237 ) {
        return 8; // DGRAY
    }
    return static_cast<int>( c );
}

static void draw_window( const catacurses::window &w, const bool force_full )
{
    cata_cursesport::WINDOW *const win = w.get<cata_cursesport::WINDOW>();
    if( !win ) {
        return;
    }

    // MapView and the Godot panels own these regions, so their glyphs are not
    // wanted -- but the cells still have to be *erased*, not merely skipped.
    //
    // catacurses has no repaint-everything pass: game code calls wnoutrefresh per
    // window. SDL gets away with that because these windows really do paint, so a
    // closing menu's cells are overwritten by whatever was underneath. Skipping the
    // draw outright left the menu's glyphs in the snapshot with nothing to ever
    // overwrite them, so a dismissed menu stayed on screen over the map while the
    // game carried on underneath -- which reads exactly like a menu that failed to
    // block input.
    //
    // Erasing here is correct with respect to draw order: ui_manager redraws from
    // the bottom up, so the terrain and sidebar are cleared before any menu layered
    // on top of them draws.
    const auto erase_area = [&win]() {
        get_view_snapshot().clear_rect( win->pos.x, win->pos.y, win->width, win->height, 0 );
    };

    if( g ) {
        if( w == g->w_terrain ) {
            // The terrain window is refreshed once per game::draw, after every
            // draw callback has run, which makes this the frame boundary for the
            // animation overlay those callbacks fill.
            get_anim_snapshot().commit_frame();
            erase_area();
            return;
        }
        if( w == g->w_minimap || w == g->w_pixel_minimap ) {
            erase_area();
            return;
        }
        // OvermapView paints the overmap; overmap_ui already leaves this window
        // erased, but clear the region so the previous screen's cells go with it.
        if( w == g->w_overmap && get_overmap_snapshot().active() ) {
            erase_area();
            return;
        }
    }
    // look_around()'s w_info is nearly full-height and sidebar-width -- the
    // same shape the heuristic below uses to recognize (and erase) the real
    // game sidebar, which Godot's own hud_panel draws instead. Without this
    // check w_info's content was erased every frame like the sidebar is, so
    // TerminalView (host.gd's USE_CURSES_UI_OVERLAY) never saw it either.
    //
    // w_info is deliberately drawn at the *same rect* the real sidebar
    // occupies, so a rect match alone can't tell the two apart -- an earlier
    // version of this check matched by rect and ended up exempting the real
    // sidebar window from erasure too, leaking a second, stale copy of it
    // through underneath TerminalView's live rendering of w_info. is_window()
    // checks the window's own identity instead.
    const bool is_look_window = get_look_snapshot().is_window( win );
    const bool sidebar = !is_look_window && win->height >= term_rows - 2 &&
                         win->width * 3 < term_cols;
    const bool msgbar = win->height <= 12 && win->width * 2 > term_cols &&
                        win->pos.y + win->height >= term_rows - 1;
    if( sidebar || msgbar ) {
        erase_area();
        return;
    }

    ViewSnapshot &snap = get_view_snapshot();

    static const std::string space_string = " ";
    std::vector<view_cell> row_cells;
    row_cells.resize( static_cast<size_t>( std::max( 0, win->width ) ) );

    for( int j = 0; j < win->height; j++ ) {
        if( !force_full && !win->line[j].touched ) {
            continue;
        }

        win->line[j].touched = false;
        std::fill( row_cells.begin(), row_cells.end(), view_cell{} );

        for( int i = 0; i < win->width; i++ ) {
            const cata_cursesport::cursecell &cell = win->line[j].chars[i];
            int fg = get_color_index( cell.FG ) & 0xf;
            int bg = get_color_index( cell.BG ) & 0xf;

            if( cell.ch.empty() || cell.ch == space_string ) {
                // A blank cell claims the overlay only where the window actually
                // painted a background. Claiming every blank cell made an *erased*
                // full-screen window -- stdscr, refreshed before each input wait --
                // an opaque sheet over MapView, and catacurses has no
                // repaint-everything pass that would ever release it again.
                //
                // Menu interiors do not depend on this: uilist and every
                // cataimgui::window land in the ImGui layer, which claims what it
                // drew rather than inferring it from the colour.
                row_cells[static_cast<size_t>( i )] = view_cell{ U' ',
                        static_cast<uint8_t>( fg ), static_cast<uint8_t>( bg ), bg != 0 };
                continue;
            }

            const int codepoint = UTF8_getch( cell.ch );
            const int cw = ( codepoint == UNKNOWN_UNICODE ) ? 1 : utf8_width( cell.ch );
            if( cw < 1 ) {
                continue;
            }

            bool use_ascii_lines = false;
            unsigned char uc = static_cast<unsigned char>( cell.ch[0] );
            switch( codepoint ) {
                case LINE_XOXO_UNICODE: uc = LINE_XOXO_C; use_ascii_lines = true; break;
                case LINE_OXOX_UNICODE: uc = LINE_OXOX_C; use_ascii_lines = true; break;
                case LINE_XXOO_UNICODE: uc = LINE_XXOO_C; use_ascii_lines = true; break;
                case LINE_OXXO_UNICODE: uc = LINE_OXXO_C; use_ascii_lines = true; break;
                case LINE_OOXX_UNICODE: uc = LINE_OOXX_C; use_ascii_lines = true; break;
                case LINE_XOOX_UNICODE: uc = LINE_XOOX_C; use_ascii_lines = true; break;
                case LINE_XXXO_UNICODE: uc = LINE_XXXO_C; use_ascii_lines = true; break;
                case LINE_XXOX_UNICODE: uc = LINE_XXOX_C; use_ascii_lines = true; break;
                case LINE_XOXX_UNICODE: uc = LINE_XOXX_C; use_ascii_lines = true; break;
                case LINE_OXXX_UNICODE: uc = LINE_OXXX_C; use_ascii_lines = true; break;
                case LINE_XXXX_UNICODE: uc = LINE_XXXX_C; use_ascii_lines = true; break;
                case UNKNOWN_UNICODE:   use_ascii_lines = true; break;
                default: break;
            }

            // Map box-drawing line ids to printable stand-ins for Godot TextServer.
            char32_t snap_ch = static_cast<char32_t>( codepoint );
            if( use_ascii_lines ) {
                switch( uc ) {
                    case LINE_XOXO_C: snap_ch = U'|'; break;
                    case LINE_OXOX_C: snap_ch = U'-'; break;
                    default: snap_ch = U'+'; break;
                }
            }
            // Black-on-black glyphs are invisible in TerminalView; bump dim text up.
            if( fg == 0 && snap_ch > U' ' ) {
                fg = 7;
            }
            for( int k = 0; k < cw && i + k < win->width; ++k ) {
                row_cells[static_cast<size_t>( i + k )] = view_cell{
                    k == 0 ? snap_ch : U' ',
                    static_cast<uint8_t>( fg ),
                    static_cast<uint8_t>( bg ),
                    true
                };
            }

        }

        snap.blit_row( win->pos.x, win->pos.y + j, row_cells.data(), win->width );
    }
    win->draw = false;
    win->last_render_epoch = cata_cursesport::curses_render_epoch;
}

static void clear_window_area( const catacurses::window &w )
{
    cata_cursesport::WINDOW *const win = w.get<cata_cursesport::WINDOW>();
    if( !win ) {
        return;
    }
    get_view_snapshot().clear_rect( win->pos.x, win->pos.y, win->width, win->height, 0 );
}

static void apply_terminal_cells( int cols, int rows )
{
    cols = std::max( cols, EVEN_MINIMUM_TERM_WIDTH );
    rows = std::max( rows, EVEN_MINIMUM_TERM_HEIGHT );
    // Keep even widths like the SDL backend.
    if( cols % 2 != 0 ) {
        cols -= 1;
    }

    term_cols = cols;
    term_rows = rows;

    ViewSnapshot &snap = get_view_snapshot();
    snap.set_cell_pixel_size( font_w, font_h );
    snap.resize( term_cols, term_rows );

    if( display *d = get_display() ) {
        d->resize( term_cols * font_w, term_rows * font_h );
    }

    catacurses::stdscr = catacurses::newwin( term_rows, term_cols, point( 0, 0 ) );
    TERMX = term_cols;
    TERMY = term_rows;

    if( ImGui::GetCurrentContext() ) {
        ImGui::GetIO().DisplaySize = ImVec2( static_cast<float>( term_cols ),
                                             static_cast<float>( term_rows ) );
    }

    // Force every curses window to re-emit into the snapshot after a resize wipe.
    cata_cursesport::bump_curses_render_epoch();
    ui_manager::screen_resized();
}

void request_window_resize( int pixel_w, int pixel_h )
{
    if( pixel_w > 0 && pixel_h > 0 ) {
        pending_pixel_w.store( pixel_w );
        pending_pixel_h.store( pixel_h );
    }
}

void apply_pending_window_resize()
{
    const int pixel_w = pending_pixel_w.exchange( 0 );
    const int pixel_h = pending_pixel_h.exchange( 0 );
    if( pixel_w <= 0 || pixel_h <= 0 || font_w <= 0 || font_h <= 0 ) {
        return;
    }
    const int cols = std::max( EVEN_MINIMUM_TERM_WIDTH, pixel_w / font_w );
    const int rows = std::max( EVEN_MINIMUM_TERM_HEIGHT, pixel_h / font_h );
    // Same cell grid: nothing to do. The window changed size but the game still
    // has the same number of cells to fill, and TerminalView derives its own
    // scale from the viewport, so there is no C++-side surface to resize.
    if( cols == term_cols && rows == term_rows ) {
        return;
    }
    apply_terminal_cells( cols, rows );
}

void blit_imtui_screen()
{
    ImTui::ImplImtui_Data *bd = ImTui::ImTui_Impl_GetBackendData();
    if( !bd || !bd->Screen.data ) {
        return;
    }
    const ImTui::TScreen &screen = bd->Screen;
    const int nx = screen.nx;
    const int ny = screen.ny;
    if( nx <= 0 || ny <= 0 ) {
        return;
    }

    // ImTui stores ANSI-256 indices, not the 16-color game palette.
    const auto ansi256_to_rgb = []( unsigned char idx ) -> color {
        if( idx < 16 ) {
            // Match the common ANSI 16-color primaries; prefer the game palette when
            // it has been loaded.
            if( idx < active_palette.size() &&
                ( active_palette[idx].r | active_palette[idx].g | active_palette[idx].b ) != 0 ) {
                return active_palette[idx];
            }
            static const uint8_t ansi16[16][3] = {
                {0, 0, 0}, {128, 0, 0}, {0, 128, 0}, {128, 128, 0},
                {0, 0, 128}, {128, 0, 128}, {0, 128, 128}, {192, 192, 192},
                {128, 128, 128}, {255, 0, 0}, {0, 255, 0}, {255, 255, 0},
                {0, 0, 255}, {255, 0, 255}, {0, 255, 255}, {255, 255, 255}
            };
            return color{ ansi16[idx][0], ansi16[idx][1], ansi16[idx][2], 255 };
        }
        if( idx < 232 ) {
            const int i = idx - 16;
            const int r = i / 36;
            const int g = ( i / 6 ) % 6;
            const int b = i % 6;
            static const uint8_t levels[6] = {0, 95, 135, 175, 215, 255};
            return color{ levels[r], levels[g], levels[b], 255 };
        }
        const uint8_t gray = static_cast<uint8_t>( 8 + ( idx - 232 ) * 10 );
        return color{ gray, gray, gray, 255 };
    };

    const auto lum_to_pal = []( const color & c ) -> int {
        const int lum = ( static_cast<int>( c.r ) + c.g + c.b ) / 3;
        return lum >= 200 ? 15 : ( lum >= 100 ? 7 : ( lum >= 40 ? 8 : 0 ) );
    };

    const auto decode_ch = []( uint32_t raw ) -> char32_t {
        if( raw == 0 ) {
            return U' ';
        }
        // ImTui glyph path stores the codepoint in vtx.col. If IMTUI encoding
        // was missing, that field is a packed ABGR color (>= 0x01000000).
        if( raw < 0x110000u && !( raw >= 0xD800u && raw <= 0xDFFFu ) ) {
            return static_cast<char32_t>( raw );
        }
        return U' ';
    };

    ViewSnapshot &snap = get_view_snapshot();
    // ImTui re-renders its entire screen every frame, so the overlay layer is
    // rebuilt from scratch each blit. That is what removes a dismissed ImGui menu:
    // it stops appearing in the screen, so it stops appearing here.
    snap.clear_imgui();
    for( int y = 0; y < ny; y++ ) {
        for( int x = 0; x < nx; ) {
            const ImTui::TCell &cell = screen.data[y * nx + x];
            int cw = static_cast<int>( cell.chwidth );
            if( cw < 1 || cw > 2 ) {
                cw = 1;
            }
            const char32_t ch = decode_ch( cell.ch );
            int pal = cell.fg < 16 ? ( cell.fg & 0xf ) : lum_to_pal( ansi256_to_rgb( cell.fg ) );
            int bg_pal = cell.bg < 16 ? ( cell.bg & 0xf ) : lum_to_pal( ansi256_to_rgb( cell.bg ) );
            // Grey-on-grey after the 16-color crush makes ImTui windows look empty.
            if( ch > U' ' && pal == bg_pal ) {
                pal = bg_pal >= 8 ? 0 : 15;
            }
            // Only claim cells ImGui actually drew into. Its screen spans the whole
            // terminal, so claiming the blanks too would cover the map in an opaque
            // black sheet and hide everything below.
            if( ch > U' ' || cell.bg != 0 ) {
                snap.set_imgui_cell( x, y, ch, static_cast<uint8_t>( pal ),
                                     static_cast<uint8_t>( bg_pal ) );
            }
            x += cw;
        }
    }
}

static void init_interface()
{
    font_w = 8;
    font_h = 16;
    term_cols = EVEN_MINIMUM_TERM_WIDTH;
    term_rows = EVEN_MINIMUM_TERM_HEIGHT;

    create_display();
    color_loader<color>().load( active_palette );
    get_view_snapshot().set_palette( active_palette );
    get_view_snapshot().set_cell_pixel_size( font_w, font_h );

    // Honor a host resize that arrived before the game thread finished init.
    const int pixel_w = pending_pixel_w.exchange( 0 );
    const int pixel_h = pending_pixel_h.exchange( 0 );
    if( pixel_w > 0 && pixel_h > 0 ) {
        term_cols = std::max( EVEN_MINIMUM_TERM_WIDTH, pixel_w / font_w );
        term_rows = std::max( EVEN_MINIMUM_TERM_HEIGHT, pixel_h / font_h );
        if( term_cols % 2 != 0 ) {
            term_cols -= 1;
        }
    }

    if( display *d = get_display() ) {
        d->resize( term_cols * font_w, term_rows * font_h );
    }
    get_view_snapshot().resize( term_cols, term_rows );
    catacurses::stdscr = catacurses::newwin( term_rows, term_cols, point( 0, 0 ) );
    TERMX = term_cols;
    TERMY = term_rows;

    // Match SDL/ncurses: ImGui client must exist before color_manager init.
    imclient = std::make_unique<cataimgui::client>();
    init_colors();
}

static void endwin()
{
    // The minimap owns a submap cache keyed on world coordinates; drop it with the
    // interface so a later world does not inherit the previous one's tiles.
    get_pixel_minimap().reset();
    imclient.reset();
}

} // namespace godot_backend

template<>
godot_backend::color color_loader<godot_backend::color>::from_rgb(
    const int r, const int g, const int b )
{
    return godot_backend::color{ static_cast<uint8_t>( r ), static_cast<uint8_t>( g ),
                                 static_cast<uint8_t>( b ), 255 };
}

namespace catacurses
{
void init_interface()
{
    godot_backend::init_interface();
}
void endwin()
{
    godot_backend::endwin();
}
} // namespace catacurses

void clear_window_area( const catacurses::window &win )
{
    godot_backend::clear_window_area( win );
}

namespace cata_cursesport
{
void curses_drawwindow( const catacurses::window &win )
{
    WINDOW *const w = win.get<WINDOW>();
    const bool force_full = w && w->last_render_epoch != curses_render_epoch;
    godot_backend::draw_window( win, force_full );
}
} // namespace cata_cursesport

void refresh_display()
{
    // The Godot host uploads the display buffer to a texture and presents it.
}

#endif // GODOT
