#include "godot_input_bridge.h"

#if defined(GODOT)

#include "catacharset.h"
#include "input.h"
#include "point.h"

#include <algorithm>
#include <atomic>
#include <optional>

#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/variant/vector2.hpp>

namespace godot_backend
{

namespace
{

// On-screen geometry of the cell grid, published by the Godot host via
// GodotInputBridge::set_cell_geometry. Written from the Godot main thread and
// read on whichever thread translates an event, so keep it lock-free.
std::atomic<int> cell_origin_x{0};
std::atomic<int> cell_origin_y{0};
std::atomic<int> cell_pixel_w{0};
std::atomic<int> cell_pixel_h{0};

/// Convert a Godot event position in pixels to a cell coordinate.
///
/// Falls back to passing the pixel position through when the host has not
/// published a geometry yet: wrong, but no worse than the previous behaviour,
/// and it keeps events flowing instead of collapsing them all onto cell (0, 0).
point event_pixel_to_cell( const godot::Vector2 &position )
{
    const int cw = cell_pixel_w.load( std::memory_order_relaxed );
    const int ch = cell_pixel_h.load( std::memory_order_relaxed );
    if( cw <= 0 || ch <= 0 ) {
        return point( static_cast<int>( position.x ), static_cast<int>( position.y ) );
    }
    const int local_x = static_cast<int>( position.x ) - cell_origin_x.load( std::memory_order_relaxed );
    const int local_y = static_cast<int>( position.y ) - cell_origin_y.load( std::memory_order_relaxed );
    // Truncating a negative local offset would round toward zero and land on
    // cell 0 for the whole row/column just outside the grid, so floor instead.
    const int cell_x = local_x >= 0 ? local_x / cw : -( ( -local_x + cw - 1 ) / cw );
    const int cell_y = local_y >= 0 ? local_y / ch : -( ( -local_y + ch - 1 ) / ch );
    return point( cell_x, cell_y );
}

/// Map a Godot key to a CDDA @ref input_event_t::keyboard_char code
/// (curses-style KEY_* / ASCII). Used when TILES keycode mode is unavailable.
std::optional<int> godot_key_to_keychar( const godot::Key key )
{
    switch( key ) {
        case godot::Key::KEY_ESCAPE:
            return KEY_ESCAPE;
        case godot::Key::KEY_TAB:
            return '\t';
        case godot::Key::KEY_BACKTAB:
            return KEY_BTAB;
        case godot::Key::KEY_BACKSPACE:
            return KEY_BACKSPACE;
        case godot::Key::KEY_ENTER:
            // CONFIRM is bound to RETURN == '\n' in keyboard_char mode, not 0x0D.
            return '\n';
        case godot::Key::KEY_KP_ENTER:
            return KEY_ENTER;
        case godot::Key::KEY_HOME:
            return KEY_HOME;
        case godot::Key::KEY_END:
            return KEY_END;
        case godot::Key::KEY_LEFT:
            return KEY_LEFT;
        case godot::Key::KEY_UP:
            return KEY_UP;
        case godot::Key::KEY_RIGHT:
            return KEY_RIGHT;
        case godot::Key::KEY_DOWN:
            return KEY_DOWN;
        case godot::Key::KEY_PAGEUP:
            return KEY_PPAGE;
        case godot::Key::KEY_PAGEDOWN:
            return KEY_NPAGE;
        case godot::Key::KEY_DELETE:
            return KEY_DC;
        case godot::Key::KEY_F1:
            return KEY_F( 1 );
        case godot::Key::KEY_F2:
            return KEY_F( 2 );
        case godot::Key::KEY_F3:
            return KEY_F( 3 );
        case godot::Key::KEY_F4:
            return KEY_F( 4 );
        case godot::Key::KEY_F5:
            return KEY_F( 5 );
        case godot::Key::KEY_F6:
            return KEY_F( 6 );
        case godot::Key::KEY_F7:
            return KEY_F( 7 );
        case godot::Key::KEY_F8:
            return KEY_F( 8 );
        case godot::Key::KEY_F9:
            return KEY_F( 9 );
        case godot::Key::KEY_F10:
            return KEY_F( 10 );
        case godot::Key::KEY_F11:
            return KEY_F( 11 );
        case godot::Key::KEY_F12:
            return KEY_F( 12 );
        default:
            break;
    }
    if( key >= godot::Key::KEY_SPACE && key <= godot::Key::KEY_ASCIITILDE ) {
        return static_cast<int>( key );
    }
    return std::nullopt;
}

/// Map a Godot @ref godot::Key to CDDA's platform-independent keycode
/// (namespace keycode in input.h). Returns @ref std::nullopt for keys CDDA
/// has no code for.
std::optional<int> godot_key_to_keycode( const godot::Key key )
{
    switch( key ) {
        case godot::Key::KEY_ESCAPE:
            return keycode::escape;
        case godot::Key::KEY_TAB:
        case godot::Key::KEY_BACKTAB:
            return keycode::tab;
        case godot::Key::KEY_BACKSPACE:
            return keycode::backspace;
        case godot::Key::KEY_ENTER:
            return keycode::return_;
        case godot::Key::KEY_KP_ENTER:
            return keycode::kp_enter;
        case godot::Key::KEY_HOME:
            return keycode::home;
        case godot::Key::KEY_END:
            return keycode::end;
        case godot::Key::KEY_LEFT:
            return keycode::left;
        case godot::Key::KEY_UP:
            return keycode::up;
        case godot::Key::KEY_RIGHT:
            return keycode::right;
        case godot::Key::KEY_DOWN:
            return keycode::down;
        case godot::Key::KEY_PAGEUP:
            return keycode::ppage;
        case godot::Key::KEY_PAGEDOWN:
            return keycode::npage;
        case godot::Key::KEY_F1:
            return keycode::f1;
        case godot::Key::KEY_F2:
            return keycode::f2;
        case godot::Key::KEY_F3:
            return keycode::f3;
        case godot::Key::KEY_F4:
            return keycode::f4;
        case godot::Key::KEY_F5:
            return keycode::f5;
        case godot::Key::KEY_F6:
            return keycode::f6;
        case godot::Key::KEY_F7:
            return keycode::f7;
        case godot::Key::KEY_F8:
            return keycode::f8;
        case godot::Key::KEY_F9:
            return keycode::f9;
        case godot::Key::KEY_F10:
            return keycode::f10;
        case godot::Key::KEY_F11:
            return keycode::f11;
        case godot::Key::KEY_F12:
            return keycode::f12;
        case godot::Key::KEY_F13:
            return keycode::f13;
        case godot::Key::KEY_F14:
            return keycode::f14;
        case godot::Key::KEY_F15:
            return keycode::f15;
        case godot::Key::KEY_F16:
            return keycode::f16;
        case godot::Key::KEY_F17:
            return keycode::f17;
        case godot::Key::KEY_F18:
            return keycode::f18;
        case godot::Key::KEY_F19:
            return keycode::f19;
        case godot::Key::KEY_F20:
            return keycode::f20;
        case godot::Key::KEY_F21:
            return keycode::f21;
        case godot::Key::KEY_F22:
            return keycode::f22;
        case godot::Key::KEY_F23:
            return keycode::f23;
        case godot::Key::KEY_F24:
            return keycode::f24;
        case godot::Key::KEY_KP_MULTIPLY:
            return keycode::kp_multiply;
        case godot::Key::KEY_KP_DIVIDE:
            return keycode::kp_divide;
        case godot::Key::KEY_KP_SUBTRACT:
            return keycode::kp_minus;
        case godot::Key::KEY_KP_PERIOD:
            return keycode::kp_period;
        case godot::Key::KEY_KP_ADD:
            return keycode::kp_plus;
        case godot::Key::KEY_KP_0:
            return keycode::kp_0;
        case godot::Key::KEY_KP_1:
            return keycode::kp_1;
        case godot::Key::KEY_KP_2:
            return keycode::kp_2;
        case godot::Key::KEY_KP_3:
            return keycode::kp_3;
        case godot::Key::KEY_KP_4:
            return keycode::kp_4;
        case godot::Key::KEY_KP_5:
            return keycode::kp_5;
        case godot::Key::KEY_KP_6:
            return keycode::kp_6;
        case godot::Key::KEY_KP_7:
            return keycode::kp_7;
        case godot::Key::KEY_KP_8:
            return keycode::kp_8;
        case godot::Key::KEY_KP_9:
            return keycode::kp_9;
        default:
            break;
    }

    // Printable ASCII keys map to themselves, mirroring the SDL backend
    // where keysym.sym is the ASCII value of the key.
    if( key >= godot::Key::KEY_SPACE && key <= godot::Key::KEY_ASCIITILDE ) {
        return static_cast<int>( key );
    }
    return std::nullopt;
}

/// Translate a Godot key event into a CDDA @ref input_event, or
/// @ref std::nullopt if the event should not be forwarded (key release,
/// modifier-only keys, keys CDDA has no binding for).
std::optional<input_event> key_event_to_cata( const godot::InputEventKey *key_ev )
{
    // Only key-down (including auto-repeat) events are fed to the game,
    // mirroring the SDL backend's CATA_KEYDOWN handling.
    if( !key_ev->is_pressed() ) {
        return std::nullopt;
    }
    // Modifier keys are represented through the modifiers set, not as events.
    const godot::Key key = key_ev->get_keycode();
    switch( key ) {
        case godot::Key::KEY_SHIFT:
        case godot::Key::KEY_CTRL:
        case godot::Key::KEY_ALT:
        case godot::Key::KEY_META:
            return std::nullopt;
        default:
            break;
    }

    const bool use_keycode = is_keycode_mode_supported();
    const std::optional<int> code = use_keycode ? godot_key_to_keycode( key )
                                    : godot_key_to_keychar( key );
    if( !code ) {
        return std::nullopt;
    }

    input_event evt;
    // GODOT builds do not enable TILES keycode mode; emit keyboard_char so
    // bindings and YES/NO filters match the curses/TUI expectations.
    evt.type = use_keycode ? input_event_t::keyboard_code : input_event_t::keyboard_char;

    int cata_code = *code;
    // For printable keys the code is the produced character (e.g. 'a' vs 'A'),
    // taken from the unicode value, matching the SDL backend.
    if( key >= godot::Key::KEY_SPACE && key <= godot::Key::KEY_ASCIITILDE ) {
        const char32_t unicode = key_ev->get_unicode();
        if( unicode != 0 ) {
            cata_code = static_cast<int>( unicode );
            evt.text = utf32_to_utf8( static_cast<uint32_t>( unicode ) );
        }
    }

    if( use_keycode ) {
        // Keycode mode is the only one where a modifier is part of the event.
        if( key_ev->is_ctrl_pressed() ) {
            evt.modifiers.emplace( keymod_t::ctrl );
        }
        if( key_ev->is_alt_pressed() ) {
            evt.modifiers.emplace( keymod_t::alt );
        }
        if( key_ev->is_shift_pressed() ) {
            evt.modifiers.emplace( keymod_t::shift );
        }
    } else {
        // Character mode carries no modifiers at all -- the shift is already in
        // the character, and input_event::operator== compares the modifier set,
        // so attaching one makes the event match no binding. Every shifted key
        // was silently dead: '!' (safe mode), '~', '?', '>', '<', and all 174
        // bindings on an uppercase letter -- eat, read, butcher, chat, grab,
        // save. Both reference backends build these with the two-argument
        // input_event constructor, which leaves modifiers empty.
        //
        // Ctrl is the exception, and it is expressed as a control code rather
        // than a modifier: input.cpp names char c as "CTRL+<c+64>". Godot
        // reports no unicode for those combinations, so derive it.
        if( key_ev->is_ctrl_pressed() && key >= godot::Key::KEY_A &&
            key <= godot::Key::KEY_Z ) {
            cata_code = static_cast<int>( key ) - static_cast<int>( godot::Key::KEY_A ) + 1;
            evt.text.clear();
        }
    }

    evt.sequence.emplace_back( cata_code );
    return evt;
}

/// Translate a Godot mouse button event into a CDDA @ref input_event, or
/// @ref std::nullopt for buttons CDDA has no binding for.
std::optional<input_event> mouse_button_event_to_cata(
    const godot::InputEventMouseButton *mouse_ev )
{
    input_event evt;
    evt.type = input_event_t::mouse;
    evt.mouse_pos = event_pixel_to_cell( mouse_ev->get_position() );
    switch( mouse_ev->get_button_index() ) {
        case godot::MouseButton::MOUSE_BUTTON_LEFT:
            evt.add_input( mouse_ev->is_pressed() ? MouseInput::LeftButtonPressed :
                           MouseInput::LeftButtonReleased );
            break;
        case godot::MouseButton::MOUSE_BUTTON_RIGHT:
            evt.add_input( mouse_ev->is_pressed() ? MouseInput::RightButtonPressed :
                           MouseInput::RightButtonReleased );
            break;
        case godot::MouseButton::MOUSE_BUTTON_WHEEL_UP:
            // Godot emits a separate event for the wheel release; only the
            // press counts as a scroll, matching the SDL wheel handler.
            if( !mouse_ev->is_pressed() ) {
                return std::nullopt;
            }
            evt.add_input( MouseInput::ScrollWheelUp );
            break;
        case godot::MouseButton::MOUSE_BUTTON_WHEEL_DOWN:
            if( !mouse_ev->is_pressed() ) {
                return std::nullopt;
            }
            evt.add_input( MouseInput::ScrollWheelDown );
            break;
        case godot::MouseButton::MOUSE_BUTTON_XBUTTON1:
            evt.add_input( mouse_ev->is_pressed() ? MouseInput::X1ButtonPressed :
                           MouseInput::X1ButtonReleased );
            break;
        case godot::MouseButton::MOUSE_BUTTON_XBUTTON2:
            evt.add_input( mouse_ev->is_pressed() ? MouseInput::X2ButtonPressed :
                           MouseInput::X2ButtonReleased );
            break;
        default:
            return std::nullopt;
    }
    return evt;
}

/// Translate a Godot mouse motion event into a CDDA MouseInput::Move event.
///
/// Hover-driven UIs (inventory, character sheet) test the cursor against window
/// rects every frame and stay inert without this.
input_event mouse_motion_event_to_cata( const godot::InputEventMouseMotion *motion_ev )
{
    input_event evt;
    evt.type = input_event_t::mouse;
    evt.mouse_pos = event_pixel_to_cell( motion_ev->get_position() );
    evt.add_input( MouseInput::Move );
    return evt;
}

} // namespace

void GodotInputBridge::push_event( const godot::Ref<godot::InputEvent> &event )
{
    if( !event.is_valid() ) {
        return;
    }
    std::optional<input_event> translated;
    if( const godot::InputEventKey *key_ev =
            godot::Object::cast_to<godot::InputEventKey>( event.ptr() ) ) {
        translated = key_event_to_cata( key_ev );
    } else if( const godot::InputEventMouseButton *mouse_ev =
                   godot::Object::cast_to<godot::InputEventMouseButton>( event.ptr() ) ) {
        translated = mouse_button_event_to_cata( mouse_ev );
    } else if( const godot::InputEventMouseMotion *motion_ev =
                   godot::Object::cast_to<godot::InputEventMouseMotion>( event.ptr() ) ) {
        translated = mouse_motion_event_to_cata( motion_ev );
    }
    if( !translated ) {
        return;
    }
    std::lock_guard<std::mutex> lock( mutex_ );
    // Motion events can arrive far faster than the game loop drains them, and a
    // stale trail of them is useless: collapse consecutive moves into the latest.
    if( !queue_.empty() && translated->type == input_event_t::mouse &&
        translated->sequence.size() == 1 &&
        translated->sequence.front() == static_cast<int>( MouseInput::Move ) &&
        queue_.back().type == input_event_t::mouse &&
        queue_.back().sequence.size() == 1 &&
        queue_.back().sequence.front() == static_cast<int>( MouseInput::Move ) ) {
        queue_.back() = *translated;
        return;
    }
    queue_.emplace_back( *translated );
}

void GodotInputBridge::set_cell_geometry( const int origin_x, const int origin_y,
        const int cell_w, const int cell_h )
{
    cell_origin_x.store( origin_x, std::memory_order_relaxed );
    cell_origin_y.store( origin_y, std::memory_order_relaxed );
    cell_pixel_w.store( std::max( 0, cell_w ), std::memory_order_relaxed );
    cell_pixel_h.store( std::max( 0, cell_h ), std::memory_order_relaxed );
}

std::optional<input_event> GodotInputBridge::pop_event()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( queue_.empty() ) {
        return std::nullopt;
    }
    const input_event evt = queue_.front();
    queue_.pop_front();
    return evt;
}

GodotInputBridge &get_input_bridge()
{
    static GodotInputBridge instance;
    return instance;
}

} // namespace godot_backend

#endif // GODOT
