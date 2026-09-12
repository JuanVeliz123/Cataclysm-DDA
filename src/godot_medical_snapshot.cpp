#include "godot_medical_snapshot.h"

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

MedicalSnapshot g_medical_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

MedicalSnapshot &get_medical_snapshot()
{
    return g_medical_snapshot;
}

bool MedicalSnapshot::active() const
{
    if( suspended_.load( std::memory_order_relaxed ) ) {
        return false;
    }
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t MedicalSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary MedicalSnapshot::copy_state() const
{
    const_cast<MedicalSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_ && !suspended_.load( std::memory_order_relaxed );
    d["generation"] = static_cast<int64_t>( generation_ );
    d["title"] = gs( data_.title );
    d["tab"] = data_.selected_tab;
    d["selected"] = data_.selected_limb;

    godot::Array tabs;
    tabs.resize( static_cast<int64_t>( data_.tab_titles.size() ) );
    for( size_t i = 0; i < data_.tab_titles.size(); ++i ) {
        tabs[static_cast<int64_t>( i )] = gs( data_.tab_titles[i] );
    }
    d["tabs"] = tabs;

    godot::Array limbs;
    limbs.resize( static_cast<int64_t>( data_.limb_names.size() ) );
    for( size_t i = 0; i < data_.limb_names.size(); ++i ) {
        limbs[static_cast<int64_t>( i )] = gs( data_.limb_names[i] );
    }
    d["limbs"] = limbs;

    d["detail_title"] = gs( data_.detail_title );
    d["effects"] = gs( data_.effects );
    d["wounds"] = gs( data_.wounds );
    d["scores"] = gs( data_.scores );
    d["speed"] = gs( data_.speed_summary );
    d["stats"] = gs( data_.stats_summary );
    d["weight"] = gs( data_.weight_line );
    return d;
}

void MedicalSnapshot::request_action( const std::string &action )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    // Queued rather than last-one-wins: the loop republishes after every applied
    // action, and a quick DOWN DOWN must move two limbs, not one.
    pending_actions_.push_back( action );
}

void MedicalSnapshot::request_select( const int index )
{
    pending_select_.store( index, std::memory_order_relaxed );
}

void MedicalSnapshot::request_tab( const int index )
{
    pending_tab_.store( index, std::memory_order_relaxed );
}

void MedicalSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool MedicalSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void MedicalSnapshot::publish( const data &d )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    data_ = d;
    ++generation_;
}

void MedicalSnapshot::set_suspended( const bool suspended )
{
    suspended_.store( suspended, std::memory_order_relaxed );
    std::lock_guard<std::mutex> lock( mutex_ );
    // The panel polls the generation before re-reading state, so a resume must
    // look like news even though the data has not changed.
    ++generation_;
}

void MedicalSnapshot::clear()
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

std::string MedicalSnapshot::next_action( int &select, int &tab )
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
