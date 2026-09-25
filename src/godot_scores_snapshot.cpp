#include "godot_scores_snapshot.h"

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

ScoresSnapshot g_scores_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

godot::Dictionary text_tab_dict( const ScoresSnapshot::text_tab &t )
{
    godot::Dictionary d;
    godot::Array rows;
    rows.resize( static_cast<int64_t>( t.rows.size() ) );
    for( size_t i = 0; i < t.rows.size(); ++i ) {
        rows[static_cast<int64_t>( i )] = gs( t.rows[i] );
    }
    d["rows"] = rows;
    d["empty"] = gs( t.empty_text );
    d["note"] = gs( t.note );
    return d;
}

} // namespace

ScoresSnapshot &get_scores_snapshot()
{
    return g_scores_snapshot;
}

bool ScoresSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t ScoresSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary ScoresSnapshot::copy_state() const
{
    const_cast<ScoresSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_;
    d["generation"] = static_cast<int64_t>( generation_ );
    d["title"] = gs( data_.title );
    d["tab"] = data_.selected_tab;

    godot::Array tabs;
    tabs.resize( static_cast<int64_t>( data_.tab_titles.size() ) );
    for( size_t i = 0; i < data_.tab_titles.size(); ++i ) {
        tabs[static_cast<int64_t>( i )] = gs( data_.tab_titles[i] );
    }
    d["tabs"] = tabs;

    d["achievements"] = text_tab_dict( data_.achievements );
    d["conducts"] = text_tab_dict( data_.conducts );
    d["scores"] = text_tab_dict( data_.scores );

    godot::Dictionary kills;
    kills["monster_header"] = gs( data_.monster_header );
    kills["monster_empty"] = gs( data_.monster_empty );
    godot::Array mrows;
    mrows.resize( static_cast<int64_t>( data_.monster_kills.size() ) );
    for( size_t i = 0; i < data_.monster_kills.size(); ++i ) {
        const kill_row &src = data_.monster_kills[i];
        godot::Dictionary r;
        r["count"] = src.count;
        r["symbol"] = gs( src.symbol );
        r["color"] = gs( src.color );
        r["name"] = gs( src.name );
        mrows[static_cast<int64_t>( i )] = r;
    }
    kills["monster_rows"] = mrows;
    kills["npc_header"] = gs( data_.npc_header );
    kills["npc_empty"] = gs( data_.npc_empty );
    godot::Array nrows;
    nrows.resize( static_cast<int64_t>( data_.npc_kills.size() ) );
    for( size_t i = 0; i < data_.npc_kills.size(); ++i ) {
        nrows[static_cast<int64_t>( i )] = gs( data_.npc_kills[i] );
    }
    kills["npc_rows"] = nrows;
    kills["monster_collapsed"] = data_.monster_collapsed;
    kills["npc_collapsed"] = data_.npc_collapsed;
    kills["total"] = data_.total_kills;
    d["kills"] = kills;
    return d;
}

void ScoresSnapshot::request_action( const std::string &action )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    // Queued rather than last-one-wins: the loop republishes after every applied
    // action, and a burst (a clicked tab three away, sent as three NEXT_TABs)
    // must land whole or the panel stops one tab short.
    pending_actions_.push_back( action );
}

void ScoresSnapshot::request_tab( const int index )
{
    pending_tab_.store( index, std::memory_order_relaxed );
}

void ScoresSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool ScoresSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void ScoresSnapshot::publish( const data &d )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    data_ = d;
    ++generation_;
}

void ScoresSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        data_ = data();
        pending_actions_.clear();
        ++generation_;
    }
    pending_tab_.store( -1, std::memory_order_relaxed );
    attended_.store( false, std::memory_order_relaxed );
}

std::string ScoresSnapshot::next_action( int &tab )
{
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
        const int want = pending_tab_.exchange( -1, std::memory_order_relaxed );
        if( want >= 0 ) {
            tab = want;
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
