#include "godot_diary_snapshot.h"

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

DiarySnapshot g_diary_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

DiarySnapshot &get_diary_snapshot()
{
    return g_diary_snapshot;
}

bool DiarySnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t DiarySnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary DiarySnapshot::copy_state() const
{
    const_cast<DiarySnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_;
    d["generation"] = static_cast<int64_t>( generation_ );
    d["title"] = gs( data_.title );
    d["head_text"] = gs( data_.head_text );
    d["current_page"] = data_.current_page;
    d["is_summary"] = data_.is_summary;
    d["selected_change"] = data_.selected_change;
    d["page_text"] = gs( data_.page_text );
    d["hint"] = gs( data_.hint );

    godot::Array pages;
    pages.resize( static_cast<int64_t>( data_.pages.size() ) );
    for( size_t i = 0; i < data_.pages.size(); ++i ) {
        pages[static_cast<int64_t>( i )] = gs( data_.pages[i] );
    }
    d["pages"] = pages;

    godot::Array changes;
    changes.resize( static_cast<int64_t>( data_.changes.size() ) );
    for( size_t i = 0; i < data_.changes.size(); ++i ) {
        godot::Dictionary cd;
        cd["text"] = gs( data_.changes[i] );
        cd["desc"] = gs( i < data_.change_desc.size() ? data_.change_desc[i] : std::string() );
        changes[static_cast<int64_t>( i )] = cd;
    }
    d["changes"] = changes;
    return d;
}

void DiarySnapshot::request_action( const std::string &action )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    pending_actions_.push_back( action );
}

void DiarySnapshot::request_select_page( const int page_index )
{
    pending_select_page_.store( page_index, std::memory_order_relaxed );
}

void DiarySnapshot::request_select_change( const int change_index )
{
    pending_select_change_.store( change_index, std::memory_order_relaxed );
}

void DiarySnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool DiarySnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

bool DiarySnapshot::suspended() const
{
    return suspended_.load( std::memory_order_relaxed );
}

void DiarySnapshot::set_suspended( const bool suspended )
{
    suspended_.store( suspended, std::memory_order_relaxed );
}

void DiarySnapshot::publish( const data &d )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    data_ = d;
    ++generation_;
}

void DiarySnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        data_ = data();
        pending_actions_.clear();
        ++generation_;
    }
    pending_select_page_.store( -1, std::memory_order_relaxed );
    pending_select_change_.store( -1, std::memory_order_relaxed );
    attended_.store( false, std::memory_order_relaxed );
    suspended_.store( false, std::memory_order_relaxed );
}

std::string DiarySnapshot::next_action( int &page_index, int &change_index )
{
    page_index = -1;
    change_index = -1;
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
        const int want_page = pending_select_page_.exchange( -1, std::memory_order_relaxed );
        if( want_page >= 0 ) {
            page_index = want_page;
            return "GODOT_SELECT_PAGE";
        }
        const int want_change = pending_select_change_.exchange( -1, std::memory_order_relaxed );
        if( want_change >= 0 ) {
            change_index = want_change;
            return "GODOT_SELECT_CHANGE";
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
