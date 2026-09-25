#include "godot_faction_snapshot.h"

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

FactionSnapshot g_faction_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

FactionSnapshot &get_faction_snapshot()
{
    return g_faction_snapshot;
}

bool FactionSnapshot::active() const
{
    if( suspended_.load( std::memory_order_relaxed ) ) {
        return false;
    }
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t FactionSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary FactionSnapshot::copy_state() const
{
    const_cast<FactionSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_ && !suspended_.load( std::memory_order_relaxed );
    d["generation"] = static_cast<int64_t>( generation_ );
    d["title"] = gs( data_.title );
    d["tab"] = data_.selected_tab;
    d["selected"] = data_.selected_row;
    d["detail"] = gs( data_.detail );

    godot::Array tabs;
    tabs.resize( static_cast<int64_t>( data_.tab_titles.size() ) );
    for( size_t i = 0; i < data_.tab_titles.size(); ++i ) {
        tabs[static_cast<int64_t>( i )] = gs( data_.tab_titles[i] );
    }
    d["tabs"] = tabs;

    godot::Array rows;
    rows.resize( static_cast<int64_t>( data_.rows.size() ) );
    for( size_t i = 0; i < data_.rows.size(); ++i ) {
        rows[static_cast<int64_t>( i )] = gs( data_.rows[i] );
    }
    d["rows"] = rows;
    return d;
}

void FactionSnapshot::request_action( const std::string &action )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    pending_actions_.push_back( action );
}

void FactionSnapshot::request_select( const int index )
{
    pending_select_.store( index, std::memory_order_relaxed );
}

void FactionSnapshot::request_tab( const int index )
{
    pending_tab_.store( index, std::memory_order_relaxed );
}

void FactionSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool FactionSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void FactionSnapshot::publish( const data &d )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    data_ = d;
    ++generation_;
}

void FactionSnapshot::set_suspended( const bool suspended )
{
    suspended_.store( suspended, std::memory_order_relaxed );
    std::lock_guard<std::mutex> lock( mutex_ );
    // The panel polls the generation before re-reading state, so a resume
    // must look like news even though the data has not changed.
    ++generation_;
}

void FactionSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        data_ = data();
        pending_actions_.clear();
        ++generation_;
    }
    pending_select_.store( -1, std::memory_order_relaxed );
    pending_tab_.store( -1, std::memory_order_relaxed );
    suspended_.store( false, std::memory_order_relaxed );
    attended_.store( false, std::memory_order_relaxed );
}

std::string FactionSnapshot::next_action( int &select, int &tab )
{
    select = -1;
    tab = -1;
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
        const int want_select = pending_select_.exchange( -1, std::memory_order_relaxed );
        if( want_select >= 0 ) {
            select = want_select;
            return "GODOT_SELECT";
        }
        const int want_tab = pending_tab_.exchange( -1, std::memory_order_relaxed );
        if( want_tab >= 0 ) {
            tab = want_tab;
            return "GODOT_TAB";
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
