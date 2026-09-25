#include "godot_compare_snapshot.h"

#if defined(GODOT)

#include "godot_backend.h"

#include <chrono>
#include <thread>

#include <godot_cpp/variant/string.hpp>

namespace godot_backend
{

namespace
{

CompareSnapshot g_compare_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

CompareSnapshot &get_compare_snapshot()
{
    return g_compare_snapshot;
}

bool CompareSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t CompareSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary CompareSnapshot::copy_state() const
{
    const_cast<CompareSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_;
    d["generation"] = static_cast<int64_t>( generation_ );
    d["title"] = gs( data_.title );
    d["first_name"] = gs( data_.first_name );
    d["first_body"] = gs( data_.first_body );
    d["second_name"] = gs( data_.second_name );
    d["second_body"] = gs( data_.second_body );
    d["confirm_message"] = gs( data_.confirm_message );
    return d;
}

void CompareSnapshot::request_action( const std::string &action )
{
    if( action == "CONFIRM" ) {
        pending_action_.store( 1, std::memory_order_relaxed );
    } else if( action == "QUIT" ) {
        pending_action_.store( 2, std::memory_order_relaxed );
    }
}

void CompareSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool CompareSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void CompareSnapshot::publish( const data &d )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    data_ = d;
    ++generation_;
}

void CompareSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        data_ = data();
        ++generation_;
    }
    pending_action_.store( 0, std::memory_order_relaxed );
    attended_.store( false, std::memory_order_relaxed );
}

std::string CompareSnapshot::next_action()
{
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
        const int want = pending_action_.exchange( 0, std::memory_order_relaxed );
        if( want == 1 ) {
            return "CONFIRM";
        } else if( want == 2 ) {
            return "QUIT";
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 4 ) );
    }
}

} // namespace godot_backend

#endif // GODOT
