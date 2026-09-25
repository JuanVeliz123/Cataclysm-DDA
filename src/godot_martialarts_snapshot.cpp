#include "godot_martialarts_snapshot.h"

#if defined(GODOT)

#include "godot_backend.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "character.h"
#include "martialarts.h"
#include "string_formatter.h"
#include "translations.h"

namespace godot_backend
{

namespace
{

MartialArtsSnapshot g_martialarts_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

MartialArtsSnapshot &get_martialarts_snapshot()
{
    return g_martialarts_snapshot;
}

bool MartialArtsSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t MartialArtsSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

godot::Dictionary MartialArtsSnapshot::copy_state() const
{
    const_cast<MartialArtsSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_;
    d["generation"] = static_cast<int64_t>( generation_ );
    d["title"] = gs( title_ );
    d["subtitle"] = gs( subtitle_ );
    d["selection"] = selection_;
    d["keep_hands_free"] = keep_hands_free_;

    godot::Array rows;
    rows.resize( static_cast<int64_t>( rows_.size() ) );
    for( size_t i = 0; i < rows_.size(); ++i ) {
        godot::Dictionary r;
        r["name"] = gs( rows_[i].name );
        r["active"] = rows_[i].active;
        r["toggle"] = rows_[i].toggle;
        rows[static_cast<int64_t>( i )] = r;
    }
    d["rows"] = rows;

    godot::Array detail;
    detail.resize( static_cast<int64_t>( detail_.size() ) );
    for( size_t i = 0; i < detail_.size(); ++i ) {
        godot::Dictionary l;
        l["text"] = gs( detail_[i].text );
        l["header"] = detail_[i].header;
        detail[static_cast<int64_t>( i )] = l;
    }
    d["detail"] = detail;
    return d;
}

void MartialArtsSnapshot::request_action( const std::string &action )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    pending_actions_.push_back( action );
}

void MartialArtsSnapshot::request_move_to( const int index )
{
    pending_move_to_.store( index, std::memory_order_relaxed );
}

void MartialArtsSnapshot::request_select( const int index )
{
    pending_select_.store( index, std::memory_order_relaxed );
}

void MartialArtsSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool MartialArtsSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void MartialArtsSnapshot::publish( const std::string &title, const std::string &subtitle,
                                   const std::vector<row> &rows, const int selection,
                                   const std::vector<detail_line> &detail,
                                   const bool keep_hands_free )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    title_ = title;
    subtitle_ = subtitle;
    rows_ = rows;
    selection_ = selection;
    detail_ = detail;
    keep_hands_free_ = keep_hands_free;
    ++generation_;
}

void MartialArtsSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        title_.clear();
        subtitle_.clear();
        rows_.clear();
        selection_ = 0;
        detail_.clear();
        keep_hands_free_ = false;
        pending_actions_.clear();
        ++generation_;
    }
    pending_move_to_.store( -1, std::memory_order_relaxed );
    pending_select_.store( -1, std::memory_order_relaxed );
    attended_.store( false, std::memory_order_relaxed );
}

std::vector<std::string> MartialArtsSnapshot::take_actions()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    std::vector<std::string> out;
    out.swap( pending_actions_ );
    return out;
}

int MartialArtsSnapshot::take_move_to()
{
    return pending_move_to_.exchange( -1, std::memory_order_relaxed );
}

int MartialArtsSnapshot::take_select()
{
    return pending_select_.exchange( -1, std::memory_order_relaxed );
}

bool run_martialarts_in_godot( const Character &you, const std::vector<matype_id> &styles,
                               const matype_id &current_style, const bool keep_hands_free,
                               const int initial_selection, int &ret )
{
    MartialArtsSnapshot &snap = get_martialarts_snapshot();
    snap.clear();

    std::vector<MartialArtsSnapshot::row> rows;
    rows.reserve( styles.size() + 1 );
    MartialArtsSnapshot::row toggle_row;
    toggle_row.name = keep_hands_free ? _( "Keep hands free (on)" ) : _( "Keep hands free (off)" );
    toggle_row.toggle = true;
    rows.push_back( toggle_row );
    for( const matype_id &style : styles ) {
        MartialArtsSnapshot::row r;
        r.name = style.obj().name.translated();
        r.active = style == current_style;
        rows.push_back( r );
    }

    const std::string title = _( "Select a style" );
    // The legacy screen's stat line, without its "press X for details" tail --
    // the details are the permanent right-hand pane here.
    const std::string subtitle = string_format(
                                     _( "STR: <color_white>%d</color>, DEX: <color_white>%d</color>, "
                                        "PER: <color_white>%d</color>, INT: <color_white>%d</color>" ),
                                     you.get_str(), you.get_dex(), you.get_per(), you.get_int() );

    const int count = static_cast<int>( rows.size() );
    int selection = std::max( 0, std::min( initial_selection, count - 1 ) );

    // Row 0 explains the toggle; a style row gets its description followed by
    // the same details the ImGui window shows.
    const auto detail_for = [&styles]( const int index ) {
        std::vector<MartialArtsSnapshot::detail_line> lines;
        if( index <= 0 || index > static_cast<int>( styles.size() ) ) {
            lines.push_back( { _( "When this is enabled, player won't wield things unless explicitly told to." ), false } );
            return lines;
        }
        const matype_id &style = styles[index - 1];
        const std::string desc = style.obj().description.translated();
        if( !desc.empty() ) {
            lines.push_back( { desc, false } );
        }
        std::vector<MartialArtsSnapshot::detail_line> details = ma_style_details_lines( style );
        lines.insert( lines.end(), details.begin(), details.end() );
        return lines;
    };

    const auto publish = [&]() {
        snap.publish( title, subtitle, rows, selection, detail_for( selection ), keep_hands_free );
    };
    publish();

    // Same contract as the other takeovers: shutdown wins, and a screen nothing
    // is drawing is handed back rather than blocking the game thread forever.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 1500 );
    while( true ) {
        if( is_shutdown_requested() ) {
            snap.clear();
            ret = -1;
            return true;
        }
        if( !snap.attended() && std::chrono::steady_clock::now() > deadline ) {
            snap.clear();
            return false;
        }
        bool moved = false;
        const int jump = snap.take_move_to();
        if( jump >= 0 && jump < count && jump != selection ) {
            selection = jump;
            moved = true;
        }
        std::vector<std::string> actions = snap.take_actions();
        const int activate = snap.take_select();
        if( activate >= 0 && activate < count ) {
            selection = activate;
            actions.emplace_back( "SELECT" );
        }
        for( const std::string &action : actions ) {
            // Clamped rather than wrapped, matching what the panel echoes
            // locally -- the two must agree or the highlight would jump.
            if( action == "UP" ) {
                selection = std::max( 0, selection - 1 );
                moved = true;
            } else if( action == "DOWN" ) {
                selection = std::min( count - 1, selection + 1 );
                moved = true;
            } else if( action == "PAGE_UP" ) {
                selection = std::max( 0, selection - 10 );
                moved = true;
            } else if( action == "PAGE_DOWN" ) {
                selection = std::min( count - 1, selection + 10 );
                moved = true;
            } else if( action == "HOME" ) {
                selection = 0;
                moved = true;
            } else if( action == "END" ) {
                selection = count - 1;
                moved = true;
            } else if( action == "SELECT" ) {
                snap.clear();
                ret = selection;
                return true;
            } else if( action == "QUIT" ) {
                snap.clear();
                ret = -1;
                return true;
            }
        }
        if( moved ) {
            publish();
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 4 ) );
    }
}

} // namespace godot_backend

#endif // GODOT
