#include "godot_keybind_snapshot.h"

#if defined(GODOT)

#include "action.h"
#include "godot_backend.h"
#include "input.h"
#include "input_context.h"
#include "output.h"
#include "translations.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <godot_cpp/variant/array.hpp>

namespace godot_backend
{

namespace
{

KeybindSnapshot g_keybind_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

KeybindSnapshot &get_keybind_snapshot()
{
    return g_keybind_snapshot;
}

bool KeybindSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t KeybindSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

void KeybindSnapshot::bump()
{
    ++generation_;
}

godot::Dictionary KeybindSnapshot::copy_state() const
{
    const_cast<KeybindSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_;
    d["context"] = gs( context_ );
    d["permit_execute"] = permit_execute_;
    d["generation"] = static_cast<int64_t>( generation_ );
    godot::Array rows;
    rows.resize( static_cast<int64_t>( rows_.size() ) );
    for( size_t i = 0; i < rows_.size(); ++i ) {
        godot::Dictionary r;
        r["action_id"] = gs( rows_[i].action_id );
        r["name"] = gs( rows_[i].name );
        r["keys"] = gs( rows_[i].keys );
        r["scope"] = rows_[i].scope;
        r["customized"] = rows_[i].customized;
        rows[static_cast<int64_t>( i )] = r;
    }
    d["rows"] = rows;
    return d;
}

void KeybindSnapshot::request( const std::string &action_id, const int op )
{
    if( op < static_cast<int>( operation::remove ) || op > static_cast<int>( operation::execute ) ) {
        return;
    }
    std::lock_guard<std::mutex> lock( mutex_ );
    requests_.push_back( pending{ action_id, static_cast<operation>( op ) } );
}

std::vector<KeybindSnapshot::pending> KeybindSnapshot::take_requests()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    std::vector<pending> out;
    out.swap( requests_ );
    return out;
}

void KeybindSnapshot::dismiss()
{
    dismissed_.store( true, std::memory_order_release );
}

bool KeybindSnapshot::dismissed() const
{
    return dismissed_.load( std::memory_order_acquire );
}

void KeybindSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool KeybindSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void KeybindSnapshot::publish( const std::string &context, const std::vector<row> &rows,
                               const bool permit_execute )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    context_ = context;
    rows_ = rows;
    permit_execute_ = permit_execute;
    bump();
}

void KeybindSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        rows_.clear();
        requests_.clear();
        bump();
    }
    dismissed_.store( false, std::memory_order_release );
    attended_.store( false, std::memory_order_relaxed );
}

} // namespace godot_backend

#endif // GODOT
