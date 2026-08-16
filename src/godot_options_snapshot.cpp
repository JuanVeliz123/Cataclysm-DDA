#include "godot_options_snapshot.h"

#if defined(GODOT)

#include "godot_backend.h"
#include "translations.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <utility>

#include <godot_cpp/variant/array.hpp>

namespace godot_backend
{

namespace
{

OptionsSnapshot g_options_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

} // namespace

OptionsSnapshot &get_options_snapshot()
{
    return g_options_snapshot;
}

bool OptionsSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t OptionsSnapshot::layout_generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return layout_generation_;
}

uint64_t OptionsSnapshot::values_generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return values_generation_;
}

godot::Dictionary OptionsSnapshot::copy_layout() const
{
    const_cast<OptionsSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_;
    d["current_page"] = current_page_;
    d["allow_tabs"] = allow_tabs_;
    d["generation"] = static_cast<int64_t>( layout_generation_ );

    godot::Array pages;
    pages.resize( static_cast<int64_t>( pages_.size() ) );
    for( size_t p = 0; p < pages_.size(); ++p ) {
        const page &src = pages_[p];
        godot::Dictionary pd;
        pd["id"] = gs( src.id );
        pd["name"] = gs( src.name );
        godot::Array rows;
        rows.resize( static_cast<int64_t>( src.rows.size() ) );
        for( size_t r = 0; r < src.rows.size(); ++r ) {
            const row &src_row = src.rows[r];
            godot::Dictionary rd;
            rd["type"] = static_cast<int>( src_row.type );
            rd["id"] = gs( src_row.id );
            rd["text"] = gs( src_row.text );
            rd["tooltip"] = gs( src_row.tooltip );
            rd["group"] = gs( src_row.group );
            rd["value_type"] = gs( src_row.value_type );
            rd["default_text"] = gs( src_row.default_text );
            rd["max_length"] = src_row.max_length;
            godot::Array items;
            items.resize( static_cast<int64_t>( src_row.items.size() ) );
            for( size_t i = 0; i < src_row.items.size(); ++i ) {
                godot::Dictionary item;
                item["value"] = gs( src_row.items[i].first );
                item["label"] = gs( src_row.items[i].second );
                items[static_cast<int64_t>( i )] = item;
            }
            rd["items"] = items;
            rows[static_cast<int64_t>( r )] = rd;
        }
        pd["rows"] = rows;
        pages[static_cast<int64_t>( p )] = pd;
    }
    d["pages"] = pages;
    return d;
}

godot::Dictionary OptionsSnapshot::copy_values() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    for( const std::pair<std::string, value> &entry : values_ ) {
        godot::Dictionary v;
        v["current"] = gs( entry.second.current );
        v["display"] = gs( entry.second.display );
        v["enabled"] = entry.second.enabled;
        d[gs( entry.first )] = v;
    }
    return d;
}

void OptionsSnapshot::request_set( const std::string &option, const std::string &value )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    edits_.push_back( edit{ option, 0, value } );
}

void OptionsSnapshot::request_step( const std::string &option, const int delta )
{
    if( delta == 0 ) {
        return;
    }
    std::lock_guard<std::mutex> lock( mutex_ );
    edits_.push_back( edit{ option, delta, {} } );
}

std::vector<OptionsSnapshot::edit> OptionsSnapshot::take_edits()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    std::vector<edit> out;
    out.swap( edits_ );
    return out;
}

void OptionsSnapshot::dismiss()
{
    dismissed_.store( true, std::memory_order_release );
}

bool OptionsSnapshot::dismissed() const
{
    return dismissed_.load( std::memory_order_acquire );
}

void OptionsSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool OptionsSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void OptionsSnapshot::publish( const std::vector<page> &pages, const int current_page,
                               const bool allow_tabs )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    pages_ = pages;
    current_page_ = current_page;
    allow_tabs_ = allow_tabs;
    ++layout_generation_;
}

void OptionsSnapshot::publish_values( std::vector<std::pair<std::string, value>> values )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    values_ = std::move( values );
    ++values_generation_;
}

void OptionsSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        pages_.clear();
        values_.clear();
        edits_.clear();
        current_page_ = 0;
        ++layout_generation_;
        ++values_generation_;
    }
    dismissed_.store( false, std::memory_order_release );
    attended_.store( false, std::memory_order_relaxed );
}

namespace
{

using options_container = options_manager::options_container;
using cOpt = options_manager::cOpt;

/**
 * Which container an option lives in. The curses screen decides this per page
 * rather than per option, and getting it wrong would edit the global default
 * while the player is looking at the world's value, so it is worth mirroring
 * exactly: the world container for the world_default page while in a game.
 */
options_container &container_for( const std::string &page_id, const bool ingame,
                                  options_container &global, options_container &world )
{
    return ingame && page_id == "world_default" ? world : global;
}

/// Build the rows for one page. Hidden options are dropped, and a group header
/// or blank line with nothing left under it would only add chrome.
std::vector<OptionsSnapshot::row> build_rows( options_manager &opts,
        const options_manager::Page &page, options_container &cont )
{
    std::vector<OptionsSnapshot::row> rows;
    for( const options_manager::PageItem &item : page.items_ ) {
        OptionsSnapshot::row r;
        r.group = item.group;
        switch( item.type ) {
            case options_manager::ItemType::BlankLine:
                r.type = OptionsSnapshot::row::kind::blank;
                break;
            case options_manager::ItemType::GroupHeader: {
                const options_manager::Group &g = opts.find_group( item.data );
                r.type = OptionsSnapshot::row::kind::group;
                r.id = item.data;
                r.text = g.name_.translated();
                r.tooltip = g.tooltip_.translated();
                break;
            }
            case options_manager::ItemType::Option: {
                const options_container::const_iterator it = cont.find( item.data );
                if( it == cont.end() || it->second.is_hidden() ) {
                    continue;
                }
                const cOpt &opt = it->second;
                r.type = OptionsSnapshot::row::kind::option;
                r.id = item.data;
                r.text = opt.getMenuText();
                r.tooltip = item.fmt_tooltip( item.group, cont );
                r.value_type = opt.getType();
                r.default_text = opt.getDefaultText();
                r.max_length = opt.getMaxLength();
                if( r.value_type == "string_select" ) {
                    for( const options_manager::id_and_option &choice : opt.getItems() ) {
                        r.items.emplace_back( choice.first, choice.second.translated() );
                    }
                }
                break;
            }
            default:
                continue;
        }
        rows.push_back( std::move( r ) );
    }
    return rows;
}

void apply_edit( const OptionsSnapshot::edit &e, const std::vector<OptionsSnapshot::page> &pages,
                 const bool ingame, options_container &global, options_container &world )
{
    for( const OptionsSnapshot::page &p : pages ) {
        for( const OptionsSnapshot::row &r : p.rows ) {
            if( r.type != OptionsSnapshot::row::kind::option || r.id != e.option ) {
                continue;
            }
            options_container &cont = container_for( p.id, ingame, global, world );
            const options_container::iterator it = cont.find( r.id );
            if( it == cont.end() ) {
                return;
            }
            cOpt &opt = it->second;
            // The curses screen puts up a popup here and refuses the edit. The
            // panel greys the row instead, so there is nothing to say.
            if( !opt.checkPrerequisite() ) {
                return;
            }
            if( e.delta == 0 ) {
                // setValue is the only path that validates: an unknown value for
                // a select is ignored, and a number outside the range is clamped.
                if( r.value_type == "int" ) {
                    opt.setValue( atoi( e.value.c_str() ) );
                } else if( r.value_type == "float" ) {
                    opt.setValue( static_cast<float>( atof( e.value.c_str() ) ) );
                } else {
                    opt.setValue( e.value );
                }
            } else {
                for( int i = 0; i < std::abs( e.delta ); ++i ) {
                    if( e.delta > 0 ) {
                        opt.setNext();
                    } else {
                        opt.setPrev();
                    }
                }
            }
            return;
        }
    }
}

std::vector<std::pair<std::string, OptionsSnapshot::value>> collect_values(
            const std::vector<OptionsSnapshot::page> &pages, const bool ingame,
            options_container &global, options_container &world )
{
    std::vector<std::pair<std::string, OptionsSnapshot::value>> out;
    for( const OptionsSnapshot::page &p : pages ) {
        options_container &cont = container_for( p.id, ingame, global, world );
        for( const OptionsSnapshot::row &r : p.rows ) {
            if( r.type != OptionsSnapshot::row::kind::option ) {
                continue;
            }
            const options_container::const_iterator it = cont.find( r.id );
            if( it == cont.end() ) {
                continue;
            }
            OptionsSnapshot::value v;
            v.current = it->second.getValue();
            v.display = it->second.getValueName();
            v.enabled = it->second.checkPrerequisite();
            out.emplace_back( r.id, std::move( v ) );
        }
    }
    return out;
}

} // namespace

bool run_options_in_godot( options_manager &opts, options_container &global,
                           options_container &world, const bool ingame, const bool with_tabs )
{
    std::vector<OptionsSnapshot::page> pages;
    for( const options_manager::Page &src : opts.get_pages() ) {
        OptionsSnapshot::page p;
        p.id = src.id_;
        p.name = src.name_.translated();
        p.rows = build_rows( opts, src, container_for( src.id_, ingame, global, world ) );
        // A page whose every option is hidden in this build has nothing to show.
        const bool any_option = std::any_of( p.rows.begin(), p.rows.end(),
        []( const OptionsSnapshot::row & r ) {
            return r.type == OptionsSnapshot::row::kind::option;
        } );
        if( any_option ) {
            pages.push_back( std::move( p ) );
        }
    }
    if( pages.empty() ) {
        return false;
    }

    OptionsSnapshot &snap = get_options_snapshot();
    snap.clear();
    snap.publish( pages, 0, with_tabs );
    snap.publish_values( collect_values( pages, ingame, global, world ) );

    // Same contract as the other takeovers: shutdown wins, and a screen nothing
    // is drawing is handed back rather than blocking the game thread forever.
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
        const std::vector<OptionsSnapshot::edit> pending = snap.take_edits();
        if( !pending.empty() ) {
            for( const OptionsSnapshot::edit &e : pending ) {
                apply_edit( e, pages, ingame, global, world );
            }
            // One option can gate another, so every value is re-read rather than
            // just the one that was touched.
            snap.publish_values( collect_values( pages, ingame, global, world ) );
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 4 ) );
    }
    snap.clear();
    return true;
}

} // namespace godot_backend

#endif // GODOT
