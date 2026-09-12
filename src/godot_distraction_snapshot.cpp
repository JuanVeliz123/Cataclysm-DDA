#include "godot_distraction_snapshot.h"

#if defined(GODOT)

#include "godot_backend.h"

#include <chrono>
#include <thread>
#include <utility>

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot_backend
{

namespace
{

DistractionSnapshot g_distraction_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

DistractionSnapshot &get_distraction_snapshot()
{
    return g_distraction_snapshot;
}

bool DistractionSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t DistractionSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary DistractionSnapshot::copy_state() const
{
    const_cast<DistractionSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_;
    d["generation"] = static_cast<int64_t>( generation_ );
    d["title"] = gs( data_.title );

    godot::Array rows;
    rows.resize( static_cast<int64_t>( data_.rows.size() ) );
    for( size_t i = 0; i < data_.rows.size(); ++i ) {
        const row &r = data_.rows[i];
        godot::Dictionary rd;
        rd["name"] = gs( r.name );
        rd["description"] = gs( r.description );
        rd["enabled"] = r.enabled;
        rows[static_cast<int64_t>( i )] = rd;
    }
    d["rows"] = rows;
    return d;
}

void DistractionSnapshot::request_action( const std::string &action )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    pending_actions_.push_back( action );
}

void DistractionSnapshot::request_toggle( const int index )
{
    pending_toggle_.store( index, std::memory_order_relaxed );
}

void DistractionSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool DistractionSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void DistractionSnapshot::publish( const data &d )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    data_ = d;
    ++generation_;
}

void DistractionSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        data_ = data();
        pending_actions_.clear();
        ++generation_;
    }
    pending_toggle_.store( -1, std::memory_order_relaxed );
    attended_.store( false, std::memory_order_relaxed );
}

std::string DistractionSnapshot::next_action( int &index )
{
    index = -1;
    // Same contract as the other takeovers: shutdown wins, and a screen
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
        const int want = pending_toggle_.exchange( -1, std::memory_order_relaxed );
        if( want >= 0 ) {
            index = want;
            return "GODOT_TOGGLE";
        }
        {
            std::lock_guard<std::mutex> lock( mutex_ );
            if( !pending_actions_.empty() ) {
                std::string out = std::move( pending_actions_.front() );
                pending_actions_.erase( pending_actions_.begin() );
                return out;
            }
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 4 ) );
    }
}

} // namespace godot_backend

#endif // GODOT
