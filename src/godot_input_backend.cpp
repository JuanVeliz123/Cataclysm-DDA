#include "godot_backend.h"
#include "godot_game_commands.h"
#include "godot_input_bridge.h"
#include "godot_hud_snapshot.h"
#include "godot_map_snapshot.h"
#include "godot_view_snapshot.h"
#include "cached_options.h"
#include "cata_imgui.h"
#include "cursesdef.h"
#include "input.h"
#include "output.h"
#include "ui_manager.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#if defined(GODOT)

void exit_handler( int s );

namespace godot_backend
{

namespace
{
std::atomic<bool> shutdown_requested{ false };
} // namespace

void request_shutdown()
{
    shutdown_requested.store( true );
}

bool is_shutdown_requested()
{
    return shutdown_requested.load();
}

} // namespace godot_backend

namespace
{
int inputdelay = -1;

[[noreturn]] void exit_on_host_shutdown()
{
    // Match SDL CATA_QUIT: tear down and hard-exit so the Godot host cannot
    // hang joining a game thread blocked in UI / input.
    ::exit_handler( 0 );
    std::_Exit( 0 );
}
} // namespace

void input_manager::pump_events()
{
    // Godot delivers events via GodotInputBridge::push_event on the main thread.
    if( godot_backend::is_shutdown_requested() ) {
        exit_on_host_shutdown();
    }
}

void input_manager::set_timeout( const int delay )
{
    input_timeout = delay;
    inputdelay = delay;
}

input_event input_manager::get_input_event( const keyboard_mode /*preferred_keyboard_mode*/ )
{
    if( test_mode ) {
        throw std::runtime_error( "input_manager::get_input_event called in test mode" );
    }

    if( godot_backend::is_shutdown_requested() ) {
        exit_on_host_shutdown();
    }

    godot_backend::apply_pending_window_resize();

    // Retire a stale ImGui frame before blocking.
    //
    // The ImGui overlay is persistent state on the Godot side: it is only rewritten
    // when an ImGui frame is rendered, from cataimgui::client::end_frame(). But
    // ui_adaptor::redraw_invalidated() returns early while the UI stack is empty,
    // which is exactly the state the game returns to when the last menu closes. So
    // nothing ever retired the frame that drew that menu: it stayed painted over
    // MapView, and because ImGui only clears ImGuiWindow::Active in NewFrame(),
    // any_window_shown() stayed true and kept routing keys into a menu that was
    // already gone.
    //
    // One empty frame fixes both: ImTui re-renders its whole screen, so the blit
    // clears the layer, and NewFrame() drops the stale Active flags. main.cpp does
    // the same new_frame/end_frame pair to prime ImGui at startup.
    //
    // Two independent signals that nothing is driving ImGui any more, because
    // either alone has a hole: the UI stack can be empty while a window object
    // still exists, and a long-lived window object can outlive the stack entry
    // that was redrawing it. cataimgui::live_window_count() is maintained by
    // window's constructor and destructor, so it does not depend on ImGui's own
    // Active flags -- which are exactly what goes stale here.
    if( imclient && godot_backend::get_view_snapshot().imgui_active() &&
        ( cataimgui::live_window_count() == 0 || ui_adaptor::ui_stack_size() == 0 ) ) {
        imclient->new_frame();
        imclient->end_frame();
    }

    // Mirror SDL: refresh before blocking so the host can present the frame.
    wnoutrefresh( catacurses::stdscr );

    previously_pressed_key = 0;

    auto wait_for_event = []( const int timeout_ms ) -> input_event {
        using clock = std::chrono::steady_clock;
        const auto start = clock::now();
        // Zero (the clock's own epoch), not time_point::min(): min() is about
        // -9.2e18ns, so `now - last` overflows a signed 64-bit nanosecond count
        // and the throttle never fires at all.
        static clock::time_point last_hud_refresh{};
        while( true ) {
            if( godot_backend::is_shutdown_requested() ) {
                exit_on_host_shutdown();
            }
            // Between actions and safe to touch game state: this is where CDDA
            // would be applying a keypress, so it is where queued Godot UI commands
            // run. See src/godot_game_commands.h.
            if( godot_backend::commands_safe_to_run() ) {
                godot_backend::drain_game_commands();
            }
            if( auto evt = godot_backend::get_input_bridge().pop_event() ) {
                return *evt;
            }
            // Keep the Godot sidebar live while the game sits here.
            //
            // The HUD snapshot was only rebuilt from game_do_turn(), which returns
            // once per player action -- so every value on the sidebar froze for as
            // long as the player was deciding what to do, which is most of a
            // turn-based game. Reading avatar state is cheap and this is the game
            // thread, so refresh on a timer instead.
            {
                const auto now = clock::now();
                // 5Hz: the snapshot walks every visible creature, and this is a
                // turn-based game -- more often buys nothing and costs more the
                // busier the screen gets.
                if( now - last_hud_refresh >= std::chrono::milliseconds( 200 ) ) {
                    last_hud_refresh = now;
                    godot_backend::update_hud_snapshot();
                    // A window resize or a zoom changes how much map MapView needs.
                    // Rebuilding the draw list is expensive, so only when it asks
                    // for an extent it has not been given.
                    if( godot_backend::get_map_snapshot().view_extent_stale() ) {
                        godot_backend::update_map_snapshot();
                    }
                }
            }
            if( timeout_ms == 0 ) {
                return input_event{ std::set<keymod_t>(), 0, input_event_t::error };
            }
            if( timeout_ms > 0 ) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        clock::now() - start ).count();
                if( elapsed >= timeout_ms ) {
                    return input_event{ std::set<keymod_t>(), 0, input_event_t::timeout };
                }
            }
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
        }
    };

    input_event result;
    if( inputdelay < 0 ) {
        result = wait_for_event( -1 );
    } else if( inputdelay > 0 ) {
        result = wait_for_event( inputdelay );
    } else {
        result = wait_for_event( 0 );
    }

    if( result.type == input_event_t::keyboard_char ||
        result.type == input_event_t::keyboard_code ) {
        previously_pressed_key = result.get_first_input();
    }

    // Hand the event to ImGui, but only while one of its windows is already up.
    //
    // ImGui needs key state or its keyboard navigation cannot be driven, and
    // nothing was feeding it. But feeding it *every* event is wrong: the keypress
    // that opens a window would then also be delivered to that window on its very
    // first frame. For arrow keys that is harmless; for Escape -- the key that both
    // opens the game menu and dismisses it -- the menu closed the instant it
    // opened, which looked exactly like Escape doing nothing at all.
    //
    // A window being shown is the signal that the input belongs to ImGui rather
    // than to the game. sdltiles.cpp is no guide here: its process_cata_input call
    // is inside an __ANDROID__ block, and desktop SDL feeds ImGui from the raw SDL
    // event stream instead, which this backend does not have.
    if( imclient && imclient->any_window_shown() ) {
        imclient->process_cata_input( result );
    }
    return result;
}

#endif // GODOT
