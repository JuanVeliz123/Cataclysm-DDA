#include "godot_advanced_inv_snapshot.h"

#if defined(GODOT)

#include "godot_backend.h"

#include <chrono>
#include <thread>
#include <utility>

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot_backend
{

namespace
{

AdvancedInvSnapshot g_advanced_inv_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

godot::Dictionary rows_to_dict( const AdvancedInvSnapshot::pane_data &p )
{
    godot::Dictionary d;
    d["area_name"] = gs( p.area_name );
    d["area_desc"] = gs( p.area_desc );
    d["capacity"] = gs( p.capacity );
    d["filter"] = gs( p.filter );
    d["sort_label"] = gs( p.sort_label );
    d["selected"] = p.selected;
    d["item_count"] = p.item_count;
    d["max_count"] = p.max_count;
    d["active"] = p.active;

    godot::Array rows;
    rows.resize( static_cast<int64_t>( p.rows.size() ) );
    for( size_t i = 0; i < p.rows.size(); ++i ) {
        const AdvancedInvSnapshot::row &r = p.rows[i];
        godot::Dictionary rd;
        rd["name"] = gs( r.name );
        rd["amount"] = gs( r.amount );
        rd["weight"] = gs( r.weight );
        rd["volume"] = gs( r.volume );
        rd["category"] = gs( r.category );
        rd["favorite"] = r.favorite;
        rd["autopickup"] = r.autopickup;
        rows[static_cast<int64_t>( i )] = rd;
    }
    d["rows"] = rows;
    return d;
}

} // namespace

AdvancedInvSnapshot &get_advanced_inv_snapshot()
{
    return g_advanced_inv_snapshot;
}

bool AdvancedInvSnapshot::active() const
{
    if( suspended_.load( std::memory_order_relaxed ) ) {
        return false;
    }
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t AdvancedInvSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary AdvancedInvSnapshot::copy_state() const
{
    const_cast<AdvancedInvSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_ && !suspended_.load( std::memory_order_relaxed );
    d["generation"] = static_cast<int64_t>( generation_ );
    d["title"] = gs( data_.title );
    d["category_mode"] = data_.category_mode;
    d["left"] = rows_to_dict( data_.left );
    d["right"] = rows_to_dict( data_.right );

    godot::Array areas;
    areas.resize( static_cast<int64_t>( data_.areas.size() ) );
    for( size_t i = 0; i < data_.areas.size(); ++i ) {
        const area_button &a = data_.areas[i];
        godot::Dictionary ad;
        ad["action"] = gs( a.action );
        ad["key"] = gs( a.key );
        ad["name"] = gs( a.name );
        ad["enabled"] = a.enabled;
        areas[static_cast<int64_t>( i )] = ad;
    }
    d["areas"] = areas;
    return d;
}

void AdvancedInvSnapshot::request_action( const std::string &action )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    pending_actions_.push_back( action );
}

void AdvancedInvSnapshot::request_select( const int side, const int index )
{
    pending_side_.store( side, std::memory_order_relaxed );
    // Written last: next_action() treats a non-negative index as "the pair
    // is ready", so side must already be visible by the time it is.
    pending_index_.store( index, std::memory_order_relaxed );
}

void AdvancedInvSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool AdvancedInvSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void AdvancedInvSnapshot::publish( const data &d )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    data_ = d;
    ++generation_;
}

void AdvancedInvSnapshot::set_suspended( const bool suspended )
{
    suspended_.store( suspended, std::memory_order_relaxed );
    std::lock_guard<std::mutex> lock( mutex_ );
    // The panel polls the generation before re-reading state, so a resume
    // must look like news even though the data has not changed.
    ++generation_;
}

void AdvancedInvSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        data_ = data();
        pending_actions_.clear();
        ++generation_;
    }
    pending_side_.store( -1, std::memory_order_relaxed );
    pending_index_.store( -1, std::memory_order_relaxed );
    suspended_.store( false, std::memory_order_relaxed );
    attended_.store( false, std::memory_order_relaxed );
}

std::string AdvancedInvSnapshot::next_action( int &side, int &index )
{
    side = -1;
    index = -1;
    // Same contract as the other takeovers: shutdown wins, and a screen nothing
    // is drawing is handed back rather than blocking the game thread forever.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 1500 );
    while( true ) {
        if( is_shutdown_requested() ) {
            return "QUIT";
        }
        if( !attended() && std::chrono::steady_clock::now() > deadline ) {
            return std::string();
        }
        const int want_index = pending_index_.exchange( -1, std::memory_order_relaxed );
        if( want_index >= 0 ) {
            side = pending_side_.load( std::memory_order_relaxed );
            index = want_index;
            return "GODOT_SELECT";
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
