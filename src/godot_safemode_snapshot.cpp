#include "godot_safemode_snapshot.h"

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

SafemodeSnapshot g_safemode_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

SafemodeSnapshot &get_safemode_snapshot()
{
    return g_safemode_snapshot;
}

bool SafemodeSnapshot::active() const
{
    if( suspended_.load( std::memory_order_relaxed ) ) {
        return false;
    }
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t SafemodeSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary SafemodeSnapshot::copy_state() const
{
    const_cast<SafemodeSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_ && !suspended_.load( std::memory_order_relaxed );
    d["generation"] = static_cast<int64_t>( generation_ );
    d["title"] = gs( data_.title );
    d["tab"] = data_.tab;
    d["character_locked"] = data_.character_locked;
    d["safe_mode_on"] = data_.safe_mode_on;
    d["show_swap"] = data_.show_swap;

    godot::Array rows;
    rows.resize( static_cast<int64_t>( data_.rows.size() ) );
    for( size_t i = 0; i < data_.rows.size(); ++i ) {
        const row &r = data_.rows[i];
        godot::Dictionary rd;
        rd["rule"] = gs( r.rule );
        rd["active"] = r.active;
        rd["attitude"] = gs( r.attitude );
        rd["proximity"] = gs( r.proximity );
        rd["whitelist"] = r.whitelist;
        rd["category"] = gs( r.category );
        rd["movement_mode"] = gs( r.movement_mode );
        rows[static_cast<int64_t>( i )] = rd;
    }
    d["rows"] = rows;
    return d;
}

void SafemodeSnapshot::request_action( const std::string &action )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    pending_actions_.push_back( action );
}

void SafemodeSnapshot::request_tab( const int tab )
{
    pending_tab_.store( tab, std::memory_order_relaxed );
}

void SafemodeSnapshot::request_confirm( const int encoded )
{
    pending_confirm_.store( encoded, std::memory_order_relaxed );
}

void SafemodeSnapshot::request_remove( const int row )
{
    pending_remove_.store( row, std::memory_order_relaxed );
}

void SafemodeSnapshot::request_copy( const int row )
{
    pending_copy_.store( row, std::memory_order_relaxed );
}

void SafemodeSnapshot::request_swap( const int row )
{
    pending_swap_.store( row, std::memory_order_relaxed );
}

void SafemodeSnapshot::request_enable( const int row )
{
    pending_enable_.store( row, std::memory_order_relaxed );
}

void SafemodeSnapshot::request_disable( const int row )
{
    pending_disable_.store( row, std::memory_order_relaxed );
}

void SafemodeSnapshot::request_move_up( const int row )
{
    pending_move_up_.store( row, std::memory_order_relaxed );
}

void SafemodeSnapshot::request_move_down( const int row )
{
    pending_move_down_.store( row, std::memory_order_relaxed );
}

void SafemodeSnapshot::request_test( const int row )
{
    pending_test_.store( row, std::memory_order_relaxed );
}

void SafemodeSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool SafemodeSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void SafemodeSnapshot::publish( const data &d )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    data_ = d;
    ++generation_;
}

void SafemodeSnapshot::set_suspended( const bool suspended )
{
    suspended_.store( suspended, std::memory_order_relaxed );
}

void SafemodeSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        data_ = data();
        pending_actions_.clear();
        ++generation_;
    }
    pending_tab_.store( -1, std::memory_order_relaxed );
    pending_confirm_.store( -1, std::memory_order_relaxed );
    pending_remove_.store( -1, std::memory_order_relaxed );
    pending_copy_.store( -1, std::memory_order_relaxed );
    pending_swap_.store( -1, std::memory_order_relaxed );
    pending_enable_.store( -1, std::memory_order_relaxed );
    pending_disable_.store( -1, std::memory_order_relaxed );
    pending_move_up_.store( -1, std::memory_order_relaxed );
    pending_move_down_.store( -1, std::memory_order_relaxed );
    pending_test_.store( -1, std::memory_order_relaxed );
    suspended_.store( false, std::memory_order_relaxed );
    attended_.store( false, std::memory_order_relaxed );
}

std::string SafemodeSnapshot::next_action( int &row, int &col )
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
        const int want_tab = pending_tab_.exchange( -1, std::memory_order_relaxed );
        if( want_tab >= 0 ) {
            col = want_tab;
            return "GODOT_TAB";
        }
        const int want_confirm = pending_confirm_.exchange( -1, std::memory_order_relaxed );
        if( want_confirm >= 0 ) {
            row = want_confirm / MAX_COLUMN;
            col = want_confirm % MAX_COLUMN;
            return "GODOT_CONFIRM";
        }
        const int want_remove = pending_remove_.exchange( -1, std::memory_order_relaxed );
        if( want_remove >= 0 ) {
            row = want_remove;
            return "GODOT_REMOVE";
        }
        const int want_copy = pending_copy_.exchange( -1, std::memory_order_relaxed );
        if( want_copy >= 0 ) {
            row = want_copy;
            return "GODOT_COPY";
        }
        const int want_swap = pending_swap_.exchange( -1, std::memory_order_relaxed );
        if( want_swap >= 0 ) {
            row = want_swap;
            return "GODOT_SWAP";
        }
        const int want_enable = pending_enable_.exchange( -1, std::memory_order_relaxed );
        if( want_enable >= 0 ) {
            row = want_enable;
            return "GODOT_ENABLE";
        }
        const int want_disable = pending_disable_.exchange( -1, std::memory_order_relaxed );
        if( want_disable >= 0 ) {
            row = want_disable;
            return "GODOT_DISABLE";
        }
        const int want_up = pending_move_up_.exchange( -1, std::memory_order_relaxed );
        if( want_up >= 0 ) {
            row = want_up;
            return "GODOT_MOVE_UP";
        }
        const int want_down = pending_move_down_.exchange( -1, std::memory_order_relaxed );
        if( want_down >= 0 ) {
            row = want_down;
            return "GODOT_MOVE_DOWN";
        }
        const int want_test = pending_test_.exchange( -1, std::memory_order_relaxed );
        if( want_test >= 0 ) {
            row = want_test;
            return "GODOT_TEST";
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
