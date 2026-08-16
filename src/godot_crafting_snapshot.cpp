#include "godot_crafting_snapshot.h"

#if defined(GODOT)

#include <utility>

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot_backend
{

namespace
{

CraftingSnapshot g_crafting_snapshot;

godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

godot::Array tabs_to_array( const std::vector<CraftingSnapshot::tab> &tabs )
{
    godot::Array out;
    out.resize( static_cast<int64_t>( tabs.size() ) );
    for( size_t i = 0; i < tabs.size(); ++i ) {
        godot::Dictionary d;
        d["id"] = gs( tabs[i].id );
        d["name"] = gs( tabs[i].name );
        out[static_cast<int64_t>( i )] = d;
    }
    return out;
}

} // namespace

CraftingSnapshot &get_crafting_snapshot()
{
    return g_crafting_snapshot;
}

bool CraftingSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

uint64_t CraftingSnapshot::list_generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return list_generation_;
}

uint64_t CraftingSnapshot::detail_generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return detail_generation_;
}

godot::Dictionary CraftingSnapshot::copy_list() const
{
    const_cast<CraftingSnapshot *>( this )->note_attended();
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["active"] = active_;
    d["generation"] = static_cast<int64_t>( list_generation_ );
    d["tabs"] = tabs_to_array( tabs_ );
    d["tab"] = tab_index_;
    d["subtabs"] = tabs_to_array( subtabs_ );
    d["subtab"] = subtab_index_;
    d["selected"] = selected_;
    d["filter"] = gs( filter_ );
    d["hidden"] = static_cast<int64_t>( hidden_ );
    d["batch_mode"] = batch_mode_;
    d["batch_size"] = batch_size_;
    godot::Array rows;
    rows.resize( static_cast<int64_t>( rows_.size() ) );
    for( size_t i = 0; i < rows_.size(); ++i ) {
        godot::Dictionary r;
        r["name"] = gs( rows_[i].name );
        r["indent"] = rows_[i].indent;
        r["craftable"] = rows_[i].craftable;
        r["caveat"] = rows_[i].caveat;
        r["nested"] = rows_[i].nested;
        rows[static_cast<int64_t>( i )] = r;
    }
    d["rows"] = rows;
    return d;
}

godot::Dictionary CraftingSnapshot::copy_detail() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["generation"] = static_cast<int64_t>( detail_generation_ );
    godot::Array lines;
    lines.resize( static_cast<int64_t>( detail_.size() ) );
    for( size_t i = 0; i < detail_.size(); ++i ) {
        godot::Dictionary l;
        l["text"] = gs( detail_[i].text );
        l["header"] = detail_[i].header;
        lines[static_cast<int64_t>( i )] = l;
    }
    d["lines"] = lines;
    return d;
}

void CraftingSnapshot::request_action( const std::string &action )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    actions_.push_back( action );
}

std::vector<std::string> CraftingSnapshot::take_actions()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    std::vector<std::string> out;
    out.swap( actions_ );
    return out;
}

void CraftingSnapshot::request_select( const int row )
{
    pending_row_.store( row, std::memory_order_relaxed );
}

void CraftingSnapshot::request_tab( const int index )
{
    pending_tab_.store( index, std::memory_order_relaxed );
}

void CraftingSnapshot::request_subtab( const int index )
{
    pending_subtab_.store( index, std::memory_order_relaxed );
}

int CraftingSnapshot::take_selected_row()
{
    return pending_row_.exchange( -1, std::memory_order_relaxed );
}

int CraftingSnapshot::take_selected_tab()
{
    return pending_tab_.exchange( -1, std::memory_order_relaxed );
}

int CraftingSnapshot::take_selected_subtab()
{
    return pending_subtab_.exchange( -1, std::memory_order_relaxed );
}

void CraftingSnapshot::note_attended()
{
    attended_.store( true, std::memory_order_relaxed );
}

bool CraftingSnapshot::attended() const
{
    return attended_.load( std::memory_order_relaxed );
}

void CraftingSnapshot::publish_list( const std::vector<tab> &tabs, const int tab_index,
                                     const std::vector<tab> &subtabs, const int subtab_index,
                                     const std::vector<row> &rows, const int selected,
                                     const std::string &filter, const size_t hidden,
                                     const bool batch_mode, const int batch_size )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    tabs_ = tabs;
    tab_index_ = tab_index;
    subtabs_ = subtabs;
    subtab_index_ = subtab_index;
    rows_ = rows;
    selected_ = selected;
    filter_ = filter;
    hidden_ = hidden;
    batch_mode_ = batch_mode;
    batch_size_ = batch_size;
    ++list_generation_;
}

int CraftingSnapshot::selected() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return selected_;
}

void CraftingSnapshot::publish_selection( const int selected )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    selected_ = selected;
}

void CraftingSnapshot::publish_detail( const std::vector<detail_line> &lines )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    detail_ = lines;
    ++detail_generation_;
}

void CraftingSnapshot::clear()
{
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        active_ = false;
        tabs_.clear();
        subtabs_.clear();
        rows_.clear();
        detail_.clear();
        actions_.clear();
        selected_ = 0;
        ++list_generation_;
        ++detail_generation_;
    }
    pending_row_.store( -1, std::memory_order_relaxed );
    pending_tab_.store( -1, std::memory_order_relaxed );
    pending_subtab_.store( -1, std::memory_order_relaxed );
    attended_.store( false, std::memory_order_relaxed );
}

} // namespace godot_backend

#endif // GODOT
