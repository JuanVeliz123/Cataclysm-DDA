#if defined(GODOT)

// -Wunused-private-field is clang-only; GCC rejects the unknown option outright.
#if defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-private-field"
#endif
#include "cata_imgui.h"
#if defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "cata_utility.h"
#include "cursesdef.h"
#include "input.h"
#include "output.h"
#include "point.h"

// Normally defined in sdltiles.cpp / ncurses_def.cpp
std::unique_ptr<cataimgui::client> imclient;

// Stand-ins for functions the game calls unconditionally but that sdltiles.cpp
// would normally provide. They are declared locally rather than through a
// header because their real declarations live in the SDL-only sdltiles.h; see
// ncurses_def.cpp for the same pattern in the curses build.
void ensure_term_size(); // NOLINT(cata-static-declarations,misc-use-internal-linkage)
void check_encoding(); // NOLINT(cata-static-declarations,misc-use-internal-linkage)
bool window_contains_point_relative( const catacurses::window &win,
                                     const point &p ); // NOLINT(cata-static-declarations,misc-use-internal-linkage)

// Godot sizes the terminal from the viewport, so there is no minimum to enforce.
void ensure_term_size() {}
// Godot's TextServer handles encoding; there is no terminal locale to validate.
void check_encoding() {}
// TODO: honour the mouse options once pixel->cell mapping lands (see T3.3).
void refresh_mouse_config() {}
// SDL device-loss recovery has no Godot equivalent; Godot owns renderer lifetime.
void drain_renderer_recovery() {}
// TODO: real hit-testing needs pixel->cell mapping; returning false disables
// mouse hover in the curses overlays (see T3.3).
bool window_contains_point_relative( const catacurses::window &, const point & )
{
    return false;
}
// The Godot host owns the window; the title is set from GDScript.
void set_title( const std::string & ) {}

// catacurses stubs (normally provided by sdltiles.cpp / ncurses_def.cpp)
catacurses::window catacurses::newscr;
bool catacurses::supports_256_colors()
{
    return false;
}

// input_manager::{pump_events,set_timeout,get_input_event} live in
// godot_input_backend.cpp so they can block on GodotInputBridge.

#endif // GODOT
