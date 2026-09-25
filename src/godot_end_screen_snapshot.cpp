#include "godot_end_screen_snapshot.h"

#if defined(GODOT)

#include "godot_backend.h"

#include <chrono>
#include <thread>

#include <godot_cpp/variant/string.hpp>

namespace godot_backend
{

namespace
{

EndScreenSnapshot g_end_screen_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

EndScreenSnapshot &get_end_screen_snapshot()
{
    return g_end_screen_snapshot;
}

bool EndScreenSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t EndScreenSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary EndScreenSnapshot::copy_state() const
{
    const_cast<EndScreenSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_;
    d["generation"] = static_cast<int64_t>( generation_ );
    d["body"] = gs( data_.body );
    d["has_text_input"] = data_.has_text_input;
    d["text_label"] = gs( data_.text_label );
    return d;
}

void EndScreenSnapshot::request_confirm( const std::string &text )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    pending_text_ = text;
    confirm_pending_ = true;
}

void EndScreenSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool EndScreenSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void EndScreenSnapshot::publish( const data &d )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    data_ = d;
    ++generation_;
}

void EndScreenSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        data_ = data();
        confirm_pending_ = false;
        pending_text_.clear();
        ++generation_;
    }
    attended_.store( false, std::memory_order_relaxed );
}

std::string EndScreenSnapshot::next_action( std::string &text_out )
{
    text_out.clear();
    // Same contract as every other takeover: shutdown wins, and a screen
    // nothing is drawing is handed back rather than blocking the game thread
    // forever.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 1500 );
    while( true ) {
        if( is_shutdown_requested() ) {
            return "QUIT";
        }
        if( !attended() && std::chrono::steady_clock::now() > deadline ) {
            return std::string();
        }
        {
            std::lock_guard<std::mutex> lock( mutex_ );
            if( confirm_pending_ ) {
                confirm_pending_ = false;
                text_out = pending_text_;
                return "CONFIRM";
            }
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 4 ) );
    }
}

} // namespace godot_backend

#endif // GODOT
