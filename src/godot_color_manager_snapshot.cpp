#include "godot_color_manager_snapshot.h"

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

ColorManagerSnapshot g_color_manager_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

ColorManagerSnapshot &get_color_manager_snapshot()
{
    return g_color_manager_snapshot;
}

bool ColorManagerSnapshot::active() const
{
    if( suspended_.load( std::memory_order_relaxed ) ) {
        return false;
    }
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t ColorManagerSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

namespace
{
godot::Dictionary cell_to_dict( const ColorManagerSnapshot::cell &c )
{
    godot::Dictionary cd;
    cd["label"] = gs( c.label );
    cd["color_name"] = gs( c.color_name );
    cd["has_custom"] = c.has_custom;
    return cd;
}
} // namespace

godot::Dictionary ColorManagerSnapshot::copy_state() const
{
    const_cast<ColorManagerSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_ && !suspended_.load( std::memory_order_relaxed );
    d["generation"] = static_cast<int64_t>( generation_ );
    d["title"] = gs( data_.title );

    godot::Array rows;
    rows.resize( static_cast<int64_t>( data_.rows.size() ) );
    for( size_t i = 0; i < data_.rows.size(); ++i ) {
        const row &r = data_.rows[i];
        godot::Dictionary rd;
        rd["name"] = gs( r.name );
        rd["normal"] = cell_to_dict( r.normal );
        rd["invert"] = cell_to_dict( r.invert );
        rows[static_cast<int64_t>( i )] = rd;
    }
    d["rows"] = rows;
    return d;
}

void ColorManagerSnapshot::request_action( const std::string &action )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    pending_actions_.push_back( action );
}

void ColorManagerSnapshot::request_pick( const int encoded )
{
    pending_pick_.store( encoded, std::memory_order_relaxed );
}

void ColorManagerSnapshot::request_remove( const int encoded )
{
    pending_remove_.store( encoded, std::memory_order_relaxed );
}

void ColorManagerSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool ColorManagerSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void ColorManagerSnapshot::publish( const data &d )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    data_ = d;
    ++generation_;
}

void ColorManagerSnapshot::set_suspended( const bool suspended )
{
    suspended_.store( suspended, std::memory_order_relaxed );
}

void ColorManagerSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        data_ = data();
        pending_actions_.clear();
        ++generation_;
    }
    pending_pick_.store( -1, std::memory_order_relaxed );
    pending_remove_.store( -1, std::memory_order_relaxed );
    suspended_.store( false, std::memory_order_relaxed );
    attended_.store( false, std::memory_order_relaxed );
}

std::string ColorManagerSnapshot::next_action( int &row, int &col )
{
    row = -1;
    col = -1;
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
        const int want_pick = pending_pick_.exchange( -1, std::memory_order_relaxed );
        if( want_pick >= 0 ) {
            row = want_pick / 2;
            col = ( want_pick % 2 == 0 ) ? 1 : 2;
            return "GODOT_PICK";
        }
        const int want_remove = pending_remove_.exchange( -1, std::memory_order_relaxed );
        if( want_remove >= 0 ) {
            row = want_remove / 2;
            col = ( want_remove % 2 == 0 ) ? 1 : 2;
            return "GODOT_REMOVE";
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
