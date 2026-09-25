#include "godot_textwin_snapshot.h"

#if defined(GODOT)

#include "godot_backend.h"
#include "output.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <godot_cpp/variant/array.hpp>

namespace godot_backend
{

namespace
{

TextWinSnapshot g_textwin_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

TextWinSnapshot &get_textwin_snapshot()
{
    return g_textwin_snapshot;
}

bool TextWinSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t TextWinSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary TextWinSnapshot::copy_state() const
{
    const_cast<TextWinSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_;
    d["title"] = gs( title_ );
    d["subtitle"] = gs( subtitle_ );
    d["current"] = current_;
    d["generation"] = static_cast<int64_t>( generation_ );
    godot::Array tabs;
    tabs.resize( static_cast<int64_t>( tabs_.size() ) );
    for( size_t i = 0; i < tabs_.size(); ++i ) {
        godot::Dictionary t;
        t["label"] = gs( tabs_[i].label );
        t["body"] = gs( tabs_[i].body );
        tabs[static_cast<int64_t>( i )] = t;
    }
    d["tabs"] = tabs;
    return d;
}

void TextWinSnapshot::select_tab( const int index )
{
    requested_tab_.store( index, std::memory_order_relaxed );
}

int TextWinSnapshot::requested_tab() const
{
    return requested_tab_.load( std::memory_order_relaxed );
}

void TextWinSnapshot::dismiss()
{
    dismissed_.store( true, std::memory_order_release );
}

bool TextWinSnapshot::dismissed() const
{
    return dismissed_.load( std::memory_order_acquire );
}

void TextWinSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool TextWinSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void TextWinSnapshot::publish( const std::string &title, const std::string &subtitle,
                               const std::vector<tab> &tabs, const int current )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    title_ = title;
    subtitle_ = subtitle;
    tabs_ = tabs;
    current_ = current;
    ++generation_;
}

void TextWinSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        tabs_.clear();
        title_.clear();
        subtitle_.clear();
        ++generation_;
    }
    requested_tab_.store( -1, std::memory_order_relaxed );
    dismissed_.store( false, std::memory_order_release );
    attended_.store( false, std::memory_order_relaxed );
}

bool run_textwin_in_godot( const std::string &title, const std::string &subtitle,
                           const std::vector<TextWinSnapshot::tab> &tabs, int &current )
{
    if( tabs.empty() ) {
        return false;
    }
    TextWinSnapshot &snap = get_textwin_snapshot();
    snap.clear();
    current = std::clamp( current, 0, static_cast<int>( tabs.size() ) - 1 );
    snap.publish( title, subtitle, tabs, current );

    // Same contract as the other takeovers: shutdown wins, and an unattended
    // window is handed back rather than blocking the game thread forever.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 1500 );
    while( !snap.dismissed() ) {
        if( is_shutdown_requested() ) {
            snap.clear();
            return true;
        }
        if( !snap.attended() && std::chrono::steady_clock::now() > deadline ) {
            snap.clear();
            return false;
        }
        const int want = snap.requested_tab();
        if( want >= 0 && want < static_cast<int>( tabs.size() ) && want != current ) {
            current = want;
            snap.publish( title, subtitle, tabs, current );
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 4 ) );
    }
    snap.clear();
    return true;
}

} // namespace godot_backend

#endif // GODOT
