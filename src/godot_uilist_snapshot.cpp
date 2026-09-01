#include "godot_uilist_snapshot.h"

#if defined(GODOT)

#include "godot_backend.h"
#include "godot_map_snapshot.h"
#include "output.h"
#include "uilist.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <godot_cpp/variant/array.hpp>

namespace godot_backend
{

namespace
{

UilistSnapshot g_uilist_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

/// Menus the Godot panel can reproduce faithfully.
///
/// A uilist_callback can draw its own side panes and intercept keys, and
/// category tabs are a second axis of navigation; neither survives being
/// re-rendered from a flat entry list, so those keep the legacy path rather than
/// being shown subtly wrong.
bool can_take_over( const uilist &menu )
{
    // A callback is allowed through when it says it does not need the C++ menu
    // UI -- see uilist_callback::needs_own_ui. Those only act on game state, and
    // this path runs them on the game thread where that is safe.
    const bool callback_ok = menu.callback == nullptr || !menu.callback->needs_own_ui();
    return callback_ok && menu.additional_actions.empty() && !menu.allow_anykey &&
           !menu.entries.empty();
}

} // namespace

UilistSnapshot &get_uilist_snapshot()
{
    return g_uilist_snapshot;
}

bool UilistSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t UilistSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

void UilistSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool UilistSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

godot::Dictionary UilistSnapshot::copy_state() const
{
    const_cast<UilistSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_;
    d["title"] = gs( title_ );
    d["text"] = gs( text_ );
    d["footer"] = gs( footer_ );
    d["desc_enabled"] = desc_enabled_;
    d["filtering"] = filtering_;
    d["filter"] = gs( filter_ );
    d["selected"] = selected_;
    d["generation"] = static_cast<int64_t>( generation_ );
    godot::Array rows;
    rows.resize( static_cast<int64_t>( entries_.size() ) );
    for( size_t i = 0; i < entries_.size(); ++i ) {
        godot::Dictionary e;
        e["text"] = gs( entries_[i].text );
        e["ctxt"] = gs( entries_[i].ctxt );
        e["desc"] = gs( entries_[i].desc );
        e["hotkey"] = gs( entries_[i].hotkey );
        e["enabled"] = entries_[i].enabled;
        e["index"] = entries_[i].index;
        rows[static_cast<int64_t>( i )] = e;
    }
    d["entries"] = rows;
    godot::Array cats;
    cats.resize( static_cast<int64_t>( categories_.size() ) );
    for( size_t i = 0; i < categories_.size(); ++i ) {
        cats[static_cast<int64_t>( i )] = gs( categories_[i] );
    }
    d["categories"] = cats;
    d["current_category"] = current_category_;
    return d;
}

void UilistSnapshot::set_selected( const int index )
{
    requested_selection_.store( index, std::memory_order_relaxed );
}

int UilistSnapshot::requested_selection() const
{
    return requested_selection_.load( std::memory_order_relaxed );
}

void UilistSnapshot::select_category( const int index )
{
    requested_category_.store( index, std::memory_order_relaxed );
}

int UilistSnapshot::requested_category() const
{
    return requested_category_.load( std::memory_order_relaxed );
}

void UilistSnapshot::set_categories( const std::vector<std::string> &labels, const int current )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    categories_ = labels;
    current_category_ = current;
}

void UilistSnapshot::confirm( const int index )
{
    answer_index_.store( index, std::memory_order_relaxed );
    answer_.store( static_cast<int8_t>( answer::chosen ), std::memory_order_release );
}

void UilistSnapshot::cancel()
{
    answer_.store( static_cast<int8_t>( answer::cancelled ), std::memory_order_release );
}

void UilistSnapshot::set_filter( const std::string &text )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( filter_ == text ) {
        return;
    }
    filter_ = text;
    filter_dirty_ = true;
}

bool UilistSnapshot::take_filter( std::string &out )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( !filter_dirty_ ) {
        return false;
    }
    filter_dirty_ = false;
    out = filter_;
    return true;
}

void UilistSnapshot::publish( const uilist &menu, const std::vector<entry> &rows,
                              const int selected )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    // settext() puts the heading in `text`, and plenty of menus use that instead
    // of `title`; showing an empty heading over a body line reads as a bug.
    title_ = remove_color_tags( menu.title );
    text_ = remove_color_tags( menu.text );
    if( title_.empty() ) {
        title_ = text_;
        text_.clear();
    }
    footer_ = remove_color_tags( menu.footer_text );
    desc_enabled_ = menu.desc_enabled;
    filtering_ = menu.filtering;
    entries_ = rows;
    selected_ = selected;
    ++generation_;
}

void UilistSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        entries_.clear();
        categories_.clear();
        current_category_ = 0;
        filter_.clear();
        filter_dirty_ = false;
        ++generation_;
    }
    requested_selection_.store( -1, std::memory_order_relaxed );
    requested_category_.store( -1, std::memory_order_relaxed );
    answer_index_.store( -1, std::memory_order_relaxed );
    answer_.store( static_cast<int8_t>( answer::pending ), std::memory_order_release );
    attended_.store( false, std::memory_order_relaxed );
}

UilistSnapshot::answer UilistSnapshot::take_answer( int &index )
{
    const answer a = static_cast<answer>( answer_.load( std::memory_order_acquire ) );
    if( a == answer::chosen ) {
        index = answer_index_.load( std::memory_order_relaxed );
    }
    return a;
}

bool run_uilist_in_godot( uilist &menu )
{
    if( !can_take_over( menu ) ) {
        return false;
    }

    // Hotkeys and default retvals are normally settled inside calc_data(),
    // which is ImGui measurement we are skipping. Without this every entry
    // arrives with no hotkey and, where the caller left it at the default,
    // a retval of -1.
    menu.assign_hotkeys();

    UilistSnapshot &snap = get_uilist_snapshot();
    snap.clear();

    std::string filter;
    int selected = std::max( 0, menu.selected );
    size_t category = menu.get_current_category();

    const auto publish_categories = [&]() {
        std::vector<std::string> labels;
        labels.reserve( menu.get_categories().size() );
        for( const std::pair<std::string, std::string> &c : menu.get_categories() ) {
            labels.push_back( remove_color_tags( c.second ) );
        }
        snap.set_categories( labels, static_cast<int>( category ) );
    };

    // Rebuild the visible rows from the current filter. uilist's own filtering
    // lives in filterlist(), which is tangled up with the ImGui layout state, so
    // do the (simple) matching here rather than reaching into it.
    const auto build_rows = [&]() {
        std::vector<UilistSnapshot::entry> rows;
        rows.reserve( menu.entries.size() );
        const std::string needle = to_lower_case( filter );
        for( size_t i = 0; i < menu.entries.size(); ++i ) {
            const uilist_entry &src = menu.entries[i];
            if( !menu.entry_in_category( src, category ) ) {
                continue;
            }
            const std::string plain = remove_color_tags( src.txt );
            if( !needle.empty() &&
                to_lower_case( plain ).find( needle ) == std::string::npos ) {
                continue;
            }
            UilistSnapshot::entry e;
            e.text = plain;
            e.ctxt = remove_color_tags( src.ctxt );
            e.desc = remove_color_tags( src.desc );
            e.enabled = src.enabled;
            e.index = static_cast<int>( i );
            if( src.hotkey.has_value() && src.hotkey.value() != input_event() ) {
                e.hotkey = src.hotkey.value().short_description();
            }
            rows.push_back( std::move( e ) );
        }
        return rows;
    };

    std::vector<UilistSnapshot::entry> rows = build_rows();
    if( rows.empty() ) {
        snap.clear();
        return false;
    }
    // Land the highlight on a row that is actually visible.
    if( std::none_of( rows.begin(), rows.end(),
    [selected]( const UilistSnapshot::entry & e ) {
    return e.index == selected;
} ) ) {
        selected = rows.front().index;
    }
    publish_categories();
    snap.publish( menu, rows, selected );

    // If no Godot panel picks the menu up, hand it back rather than waiting on an
    // answer that will never come. A blocked game thread is the exact failure
    // this whole migration has been chasing, and it must not be reintroduced by
    // a host that is not running the panel -- an older project, a script error,
    // or the headless harness.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 1500 );

    int chosen = -1;
    while( true ) {
        // Shutdown is checked before anything else: if the host is going away,
        // returning normally would let the game thread run on into game logic
        // while the extension is being torn down underneath it.
        if( is_shutdown_requested() ) {
            // Same contract as the input backend: a host shutdown must not be
            // able to strand the game thread inside a menu.
            snap.clear();
            menu.ret = UILIST_CANCEL;
            return true;
        }
        if( !snap.attended() && std::chrono::steady_clock::now() > deadline ) {
            snap.clear();
            return false;
        }
        const UilistSnapshot::answer a = snap.take_answer( chosen );
        if( a == UilistSnapshot::answer::cancelled ) {
            snap.clear();
            menu.ret = UILIST_CANCEL;
            return true;
        }
        if( a == UilistSnapshot::answer::chosen ) {
            break;
        }
        const int req = snap.requested_selection();
        if( req >= 0 && req != selected ) {
            selected = req;
            if( menu.callback != nullptr ) {
                // Mirror query_once: set the selection first, because the
                // callback reads menu->selected. These move the camera to
                // preview the highlighted point, so republish the map --
                // the game thread is parked here, not at the input wait
                // where the idle refresh runs, and without this the preview
                // would move and never be seen.
                menu.selected = selected;
                menu.callback->select( &menu );
                update_map_snapshot();
            }
        }
        const int want_cat = snap.requested_category();
        if( want_cat >= 0 && static_cast<size_t>( want_cat ) != category &&
            static_cast<size_t>( want_cat ) < menu.get_categories().size() ) {
            category = static_cast<size_t>( want_cat );
            // Keep uilist's own idea of the tab in step, so a caller reading it
            // after the menu closes sees where the player ended up.
            menu.set_current_category( category );
            rows = build_rows();
            if( !rows.empty() ) {
                selected = rows.front().index;
            }
            publish_categories();
            snap.publish( menu, rows, selected );
        }
        if( snap.take_filter( filter ) ) {
            rows = build_rows();
            if( !rows.empty() &&
                std::none_of( rows.begin(), rows.end(),
            [selected]( const UilistSnapshot::entry & e ) {
            return e.index == selected;
            } ) ) {
                selected = rows.front().index;
            }
            snap.publish( menu, rows, selected );
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 4 ) );
    }

    snap.clear();
    if( chosen < 0 || chosen >= static_cast<int>( menu.entries.size() ) ) {
        menu.ret = UILIST_CANCEL;
        return true;
    }
    const uilist_entry &picked = menu.entries[chosen];
    if( !picked.enabled && !menu.allow_disabled ) {
        menu.ret = UILIST_CANCEL;
        return true;
    }
    menu.selected = chosen;
    menu.ret = picked.retval;
    if( menu.callback != nullptr ) {
        menu.callback->confirm( &menu );
    }
    return true;
}

} // namespace godot_backend

#endif // GODOT
