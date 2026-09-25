#include "godot_auto_note_snapshot.h"

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

AutoNoteSnapshot g_auto_note_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

AutoNoteSnapshot &get_auto_note_snapshot()
{
    return g_auto_note_snapshot;
}

bool AutoNoteSnapshot::active() const
{
    if( suspended_.load( std::memory_order_relaxed ) ) {
        return false;
    }
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t AutoNoteSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary AutoNoteSnapshot::copy_state() const
{
    const_cast<AutoNoteSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_ && !suspended_.load( std::memory_order_relaxed );
    d["generation"] = static_cast<int64_t>( generation_ );
    d["title"] = gs( data_.title );
    d["tab"] = data_.selected_tab;
    d["empty_mode"] = data_.empty_mode;
    d["auto_notes_map_extras"] = data_.auto_notes_map_extras;

    godot::Array rows;
    rows.resize( static_cast<int64_t>( data_.rows.size() ) );
    for( size_t i = 0; i < data_.rows.size(); ++i ) {
        const row &r = data_.rows[i];
        godot::Dictionary rd;
        rd["name"] = gs( r.name );
        rd["symbol"] = gs( r.symbol );
        rd["symbol_color"] = gs( r.symbol_color );
        rd["has_custom_symbol"] = r.has_custom_symbol;
        rd["enabled"] = r.enabled;
        rows[static_cast<int64_t>( i )] = rd;
    }
    d["rows"] = rows;
    return d;
}

void AutoNoteSnapshot::request_action( const std::string &action )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    pending_actions_.push_back( action );
}

void AutoNoteSnapshot::request_toggle( const int index )
{
    pending_toggle_.store( index, std::memory_order_relaxed );
}

void AutoNoteSnapshot::request_enable( const int index )
{
    pending_enable_.store( index, std::memory_order_relaxed );
}

void AutoNoteSnapshot::request_disable( const int index )
{
    pending_disable_.store( index, std::memory_order_relaxed );
}

void AutoNoteSnapshot::request_tab( const int tab )
{
    pending_tab_.store( tab, std::memory_order_relaxed );
}

void AutoNoteSnapshot::request_symbol( const int index )
{
    pending_symbol_.store( index, std::memory_order_relaxed );
}

void AutoNoteSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool AutoNoteSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void AutoNoteSnapshot::publish( const data &d )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    data_ = d;
    ++generation_;
}

void AutoNoteSnapshot::set_suspended( const bool suspended )
{
    suspended_.store( suspended, std::memory_order_relaxed );
}

void AutoNoteSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        data_ = data();
        pending_actions_.clear();
        ++generation_;
    }
    pending_toggle_.store( -1, std::memory_order_relaxed );
    pending_enable_.store( -1, std::memory_order_relaxed );
    pending_disable_.store( -1, std::memory_order_relaxed );
    pending_symbol_.store( -1, std::memory_order_relaxed );
    pending_tab_.store( -1, std::memory_order_relaxed );
    suspended_.store( false, std::memory_order_relaxed );
    attended_.store( false, std::memory_order_relaxed );
}

std::string AutoNoteSnapshot::next_action( int &index, int &tab )
{
    index = -1;
    tab = -1;
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
        const int want_toggle = pending_toggle_.exchange( -1, std::memory_order_relaxed );
        if( want_toggle >= 0 ) {
            index = want_toggle;
            return "GODOT_TOGGLE";
        }
        const int want_enable = pending_enable_.exchange( -1, std::memory_order_relaxed );
        if( want_enable >= 0 ) {
            index = want_enable;
            return "GODOT_ENABLE";
        }
        const int want_disable = pending_disable_.exchange( -1, std::memory_order_relaxed );
        if( want_disable >= 0 ) {
            index = want_disable;
            return "GODOT_DISABLE";
        }
        const int want_symbol = pending_symbol_.exchange( -1, std::memory_order_relaxed );
        if( want_symbol >= 0 ) {
            index = want_symbol;
            return "GODOT_SYMBOL";
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
