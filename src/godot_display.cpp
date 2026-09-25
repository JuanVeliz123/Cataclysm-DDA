#include "godot_display.h"

#if defined(GODOT)

namespace godot_backend
{

namespace
{

/// The single process-lifetime display bridge, created by @ref create_display
/// and owned here.
display *g_display = nullptr;

} // namespace

display *get_display()
{
    return g_display;
}

bool GodotDisplay::init()
{
    return true;
}

void GodotDisplay::resize( int, int )
{
    // Godot owns the window; the cell grid is resized by
    // apply_pending_window_resize on the game thread instead.
}

void GodotDisplay::toggle_fullscreen()
{
    // The Godot host owns the window; fullscreen is toggled from the host side.
}

display *create_display()
{
    if( !g_display ) {
        g_display = new GodotDisplay();
    }
    return g_display;
}

} // namespace godot_backend

#endif // GODOT
