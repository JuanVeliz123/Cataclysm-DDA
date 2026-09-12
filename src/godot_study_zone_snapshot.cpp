#include "godot_study_zone_snapshot.h"

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

StudyZoneSnapshot g_study_zone_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

StudyZoneSnapshot &get_study_zone_snapshot()
{
    return g_study_zone_snapshot;
}

bool StudyZoneSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t StudyZoneSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary StudyZoneSnapshot::copy_state() const
{
    const_cast<StudyZoneSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_;
    d["generation"] = static_cast<int64_t>( generation_ );
    d["title"] = gs( data_.title );
    d["filter"] = gs( data_.filter );

    godot::Array npc_names;
    npc_names.resize( static_cast<int64_t>( data_.npc_names.size() ) );
    for( size_t i = 0; i < data_.npc_names.size(); ++i ) {
        npc_names[static_cast<int64_t>( i )] = gs( data_.npc_names[i] );
    }
    d["npc_names"] = npc_names;

    godot::Array rows;
    rows.resize( static_cast<int64_t>( data_.rows.size() ) );
    for( size_t i = 0; i < data_.rows.size(); ++i ) {
        const row &r = data_.rows[i];
        godot::Dictionary rd;
        rd["skill_index"] = r.skill_index;
        rd["name"] = gs( r.skill_name );
        godot::Array checked;
        checked.resize( static_cast<int64_t>( r.checked.size() ) );
        for( size_t j = 0; j < r.checked.size(); ++j ) {
            checked[static_cast<int64_t>( j )] = static_cast<bool>( r.checked[j] );
        }
        rd["checked"] = checked;
        rows[static_cast<int64_t>( i )] = rd;
    }
    d["rows"] = rows;
    return d;
}

void StudyZoneSnapshot::request_action( const std::string &action )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    pending_actions_.push_back( action );
}

void StudyZoneSnapshot::request_toggle( const int skill_index, const int npc_index )
{
    pending_toggle_skill_.store( skill_index, std::memory_order_relaxed );
    pending_toggle_npc_.store( npc_index, std::memory_order_relaxed );
}

void StudyZoneSnapshot::set_filter( const std::string &text )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( filter_ == text ) {
        return;
    }
    filter_ = text;
    filter_dirty_ = true;
}

void StudyZoneSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool StudyZoneSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void StudyZoneSnapshot::publish( const data &d )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    data_ = d;
    ++generation_;
}

void StudyZoneSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        data_ = data();
        pending_actions_.clear();
        filter_.clear();
        filter_dirty_ = false;
        ++generation_;
    }
    pending_toggle_skill_.store( -1, std::memory_order_relaxed );
    pending_toggle_npc_.store( -1, std::memory_order_relaxed );
    attended_.store( false, std::memory_order_relaxed );
}

std::string StudyZoneSnapshot::next_action( int &skill_index, int &npc_index,
        std::string &filter_text )
{
    skill_index = -1;
    npc_index = -1;
    filter_text.clear();
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
        const int want_skill = pending_toggle_skill_.exchange( -1, std::memory_order_relaxed );
        const int want_npc = pending_toggle_npc_.exchange( -1, std::memory_order_relaxed );
        if( want_skill >= 0 && want_npc >= 0 ) {
            skill_index = want_skill;
            npc_index = want_npc;
            return "GODOT_TOGGLE";
        }
        {
            std::lock_guard<std::mutex> lock( mutex_ );
            if( filter_dirty_ ) {
                filter_dirty_ = false;
                filter_text = filter_;
                return "GODOT_FILTER";
            }
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
