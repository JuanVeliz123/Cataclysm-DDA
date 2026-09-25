#include "godot_follower_rules_snapshot.h"

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

FollowerRulesSnapshot g_follower_rules_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

FollowerRulesSnapshot &get_follower_rules_snapshot()
{
    return g_follower_rules_snapshot;
}

bool FollowerRulesSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t FollowerRulesSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary FollowerRulesSnapshot::copy_state() const
{
    const_cast<FollowerRulesSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_;
    d["generation"] = static_cast<int64_t>( generation_ );
    d["title"] = gs( data_.title );

    godot::Array rules;
    rules.resize( static_cast<int64_t>( data_.rules.size() ) );
    for( size_t i = 0; i < data_.rules.size(); ++i ) {
        const bool_rule &r = data_.rules[i];
        godot::Dictionary rd;
        rd["flag"] = r.flag;
        rd["label"] = gs( r.label );
        rd["hotkey"] = gs( r.hotkey );
        rd["enabled"] = r.enabled;
        rules[static_cast<int64_t>( i )] = rd;
    }
    d["rules"] = rules;

    godot::Array groups;
    groups.resize( static_cast<int64_t>( data_.groups.size() ) );
    for( size_t i = 0; i < data_.groups.size(); ++i ) {
        const radio_group &g = data_.groups[i];
        godot::Dictionary gd;
        gd["id"] = gs( g.id );
        gd["title"] = gs( g.title );
        gd["hotkey"] = gs( g.hotkey );
        gd["current"] = g.current;
        godot::Array options;
        options.resize( static_cast<int64_t>( g.options.size() ) );
        for( size_t j = 0; j < g.options.size(); ++j ) {
            godot::Dictionary od;
            od["value"] = g.options[j].value;
            od["label"] = gs( g.options[j].label );
            options[static_cast<int64_t>( j )] = od;
        }
        gd["options"] = options;
        groups[static_cast<int64_t>( i )] = gd;
    }
    d["groups"] = groups;
    return d;
}

void FollowerRulesSnapshot::request_action( const std::string &action )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    pending_actions_.push_back( action );
}

void FollowerRulesSnapshot::request_toggle( const int rule_flag )
{
    pending_toggle_flag_.store( rule_flag, std::memory_order_relaxed );
}

void FollowerRulesSnapshot::request_default_rule( const int rule_flag )
{
    pending_default_flag_.store( rule_flag, std::memory_order_relaxed );
}

void FollowerRulesSnapshot::request_set( const std::string &group, const int value )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    pending_set_group_ = group;
    pending_set_value_ = value;
    set_dirty_ = true;
}

void FollowerRulesSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool FollowerRulesSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void FollowerRulesSnapshot::publish( const data &d )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    data_ = d;
    ++generation_;
}

void FollowerRulesSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        data_ = data();
        pending_actions_.clear();
        pending_set_group_.clear();
        pending_set_value_ = 0;
        set_dirty_ = false;
        ++generation_;
    }
    pending_toggle_flag_.store( 0, std::memory_order_relaxed );
    pending_default_flag_.store( 0, std::memory_order_relaxed );
    attended_.store( false, std::memory_order_relaxed );
}

std::string FollowerRulesSnapshot::next_action( int &rule_flag, std::string &group, int &value )
{
    rule_flag = 0;
    group.clear();
    value = 0;
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
        const int want_toggle = pending_toggle_flag_.exchange( 0, std::memory_order_relaxed );
        if( want_toggle != 0 ) {
            rule_flag = want_toggle;
            return "GODOT_TOGGLE";
        }
        const int want_default = pending_default_flag_.exchange( 0, std::memory_order_relaxed );
        if( want_default != 0 ) {
            rule_flag = want_default;
            return "GODOT_DEFAULT_RULE";
        }
        {
            std::lock_guard<std::mutex> lock( mutex_ );
            if( set_dirty_ ) {
                set_dirty_ = false;
                group = pending_set_group_;
                value = pending_set_value_;
                return "GODOT_SET";
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
