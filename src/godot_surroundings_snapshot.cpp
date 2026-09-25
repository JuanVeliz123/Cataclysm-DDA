#include "godot_surroundings_snapshot.h"

#if defined(GODOT)

#include "godot_backend.h"

#include <chrono>
#include <thread>

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot_backend
{

namespace
{

SurroundingsSnapshot g_surroundings_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

SurroundingsSnapshot &get_surroundings_snapshot()
{
    return g_surroundings_snapshot;
}

bool SurroundingsSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t SurroundingsSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary SurroundingsSnapshot::copy_state() const
{
    const_cast<SurroundingsSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_;
    d["tab"] = tab_index_;
    d["selected"] = selected_;
    d["filter"] = gs( filter_ );
    d["generation"] = static_cast<int64_t>( generation_ );

    godot::Array tabs;
    tabs.resize( static_cast<int64_t>( tabs_.size() ) );
    for( size_t i = 0; i < tabs_.size(); ++i ) {
        godot::Dictionary t;
        t["title"] = gs( tabs_[i].title );
        t["count"] = tabs_[i].count;
        tabs[static_cast<int64_t>( i )] = t;
    }
    d["tabs"] = tabs;

    godot::Array rows;
    rows.resize( static_cast<int64_t>( rows_.size() ) );
    for( size_t i = 0; i < rows_.size(); ++i ) {
        godot::Dictionary r;
        r["text"] = gs( rows_[i].text );
        r["distance"] = gs( rows_[i].distance );
        r["color"] = gs( rows_[i].color );
        r["category"] = gs( rows_[i].category );
        r["count"] = rows_[i].count;
        rows[static_cast<int64_t>( i )] = r;
    }
    d["rows"] = rows;
    return d;
}

void SurroundingsSnapshot::request_action( const std::string &action )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    // Last one wins rather than queueing: the loop takes one action per pass and
    // a backlog would replay stale keys against a list that has since changed.
    pending_action_ = action;
}

void SurroundingsSnapshot::request_select( const int index )
{
    pending_select_.store( index, std::memory_order_relaxed );
}

void SurroundingsSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool SurroundingsSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void SurroundingsSnapshot::publish( const std::vector<tab> &tabs, const int tab_index,
                                    const std::vector<row> &rows, const int selected,
                                    const std::string &filter )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    tabs_ = tabs;
    tab_index_ = tab_index;
    rows_ = rows;
    selected_ = selected;
    filter_ = filter;
    ++generation_;
}

void SurroundingsSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        tabs_.clear();
        rows_.clear();
        selected_ = 0;
        filter_.clear();
        pending_action_.clear();
        ++generation_;
    }
    pending_select_.store( -1, std::memory_order_relaxed );
    attended_.store( false, std::memory_order_relaxed );
}

std::string SurroundingsSnapshot::next_action( int &select )
{
    select = -1;
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
        const int want = pending_select_.exchange( -1, std::memory_order_relaxed );
        if( want >= 0 ) {
            select = want;
            return "GODOT_SELECT";
        }
        {
            std::lock_guard<std::mutex> lock( mutex_ );
            if( !pending_action_.empty() ) {
                std::string out;
                out.swap( pending_action_ );
                return out;
            }
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 4 ) );
    }
}

} // namespace godot_backend

#endif // GODOT
