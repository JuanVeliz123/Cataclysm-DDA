#include "godot_view_snapshot.h"

#if defined(GODOT)

#include <algorithm>

namespace godot_backend
{

namespace
{

ViewSnapshot g_view_snapshot;

uint8_t clamp_pal( int idx )
{
    if( idx < 0 ) {
        return 0;
    }
    if( idx > 15 ) {
        return 15;
    }
    return static_cast<uint8_t>( idx );
}

} // namespace

ViewSnapshot &get_view_snapshot()
{
    return g_view_snapshot;
}

void ViewSnapshot::resize( int cols, int rows )
{
    cols = std::max( 0, cols );
    rows = std::max( 0, rows );
    std::lock_guard<std::mutex> lock( mutex_ );
    if( cols_ == cols && rows_ == rows &&
        static_cast<int>( cells_.size() ) == cols * rows ) {
        return;
    }
    cols_ = cols;
    rows_ = rows;
    cells_.assign( static_cast<size_t>( cols_ ) * static_cast<size_t>( rows_ ), view_cell{} );
    imgui_cells_.assign( cells_.size(), view_cell{} );
}

void ViewSnapshot::set_palette( const palette_array &pal )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    palette_ = pal;
}

void ViewSnapshot::set_cell( int x, int y, char32_t ch, uint8_t fg, uint8_t bg )
{
    view_cell cell{ ch == 0 ? U' ' : ch, clamp_pal( fg ), clamp_pal( bg ), true };
    blit_row( x, y, &cell, 1 );
}

void ViewSnapshot::clear_rect( int x, int y, int w, int h, uint8_t bg )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( cols_ <= 0 || rows_ <= 0 ) {
        return;
    }
    const int x0 = std::max( 0, x );
    const int y0 = std::max( 0, y );
    const int x1 = std::min( cols_, x + std::max( 0, w ) );
    const int y1 = std::min( rows_, y + std::max( 0, h ) );
    const uint8_t bgc = clamp_pal( bg );
    for( int row = y0; row < y1; ++row ) {
        for( int col = x0; col < x1; ++col ) {
            view_cell &cell = cells_[static_cast<size_t>( row ) * static_cast<size_t>( cols_ ) +
                                                               static_cast<size_t>( col )];
            cell.ch = U' ';
            cell.fg = 0;
            cell.bg = bgc;
            // Erased means "no UI here", so MapView shows through rather than a
            // black rectangle.
            cell.occupied = false;
        }
    }
}

void ViewSnapshot::clear_all()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    std::fill( cells_.begin(), cells_.end(), view_cell{} );
    std::fill( imgui_cells_.begin(), imgui_cells_.end(), view_cell{} );
    imgui_active_ = false;
}

void ViewSnapshot::clear_imgui()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    std::fill( imgui_cells_.begin(), imgui_cells_.end(), view_cell{} );
    imgui_active_ = false;
}

bool ViewSnapshot::imgui_active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return imgui_active_;
}

bool ViewSnapshot::any_content() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( imgui_active_ ) {
        return true;
    }
    for( const view_cell &cell : cells_ ) {
        if( cell.occupied ) {
            return true;
        }
    }
    return false;
}

void ViewSnapshot::set_imgui_cell( int x, int y, char32_t ch, uint8_t fg, uint8_t bg )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    if( x < 0 || y < 0 || x >= cols_ || y >= rows_ ) {
        return;
    }
    view_cell &cell = imgui_cells_[static_cast<size_t>( y ) * static_cast<size_t>( cols_ ) +
                                                            static_cast<size_t>( x )];
    cell.ch = ch == 0 ? U' ' : ch;
    cell.fg = clamp_pal( fg );
    cell.bg = clamp_pal( bg );
    cell.occupied = true;
    imgui_active_ = true;
}

void ViewSnapshot::blit_row( int x, int y, const view_cell *cells, int n )
{
    if( !cells || n <= 0 ) {
        return;
    }
    std::lock_guard<std::mutex> lock( mutex_ );
    if( y < 0 || y >= rows_ || x >= cols_ ) {
        return;
    }
    int col = x;
    int i = 0;
    if( col < 0 ) {
        i = -col;
        col = 0;
    }
    for( ; i < n && col < cols_; ++i, ++col ) {
        view_cell cell = cells[i];
        cell.ch = cell.ch == 0 ? U' ' : cell.ch;
        cell.fg = clamp_pal( cell.fg );
        cell.bg = clamp_pal( cell.bg );
        cells_[static_cast<size_t>( y ) * static_cast<size_t>( cols_ ) +
                                         static_cast<size_t>( col )] = cell;
    }
}

int ViewSnapshot::cols() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return cols_;
}

int ViewSnapshot::rows() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return rows_;
}

int ViewSnapshot::cell_pixel_w() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return cell_w_;
}

int ViewSnapshot::cell_pixel_h() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return cell_h_;
}

void ViewSnapshot::set_cell_pixel_size( int w, int h )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    cell_w_ = std::max( 1, w );
    cell_h_ = std::max( 1, h );
}

void ViewSnapshot::copy_cells( std::vector<int32_t> &out ) const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    out.resize( static_cast<size_t>( cols_ ) * static_cast<size_t>( rows_ ) *
                static_cast<size_t>( cell_stride ) );
    size_t i = 0;
    for( size_t n = 0; n < cells_.size(); ++n ) {
        // ImGui sits above the curses cells; where it claims nothing, the curses
        // cell shows, and where neither does, the cell is transparent.
        const view_cell &cell = ( n < imgui_cells_.size() && imgui_cells_[n].occupied )
                                ? imgui_cells_[n]
                                : cells_[n];
        out[i++] = static_cast<int32_t>( cell.ch );
        out[i++] = cell.fg;
        out[i++] = cell.bg;
        out[i++] = cell.occupied ? 1 : 0;
    }
}

void ViewSnapshot::copy_palette_rgba( std::vector<uint8_t> &out ) const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    out.resize( palette_.size() * 4u );
    for( size_t i = 0; i < palette_.size(); ++i ) {
        const color &c = palette_[i];
        out[i * 4 + 0] = c.r;
        out[i * 4 + 1] = c.g;
        out[i * 4 + 2] = c.b;
        out[i * 4 + 3] = c.a ? c.a : 255;
    }
}

int ViewSnapshot::count_glyph_cells() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    int n = 0;
    for( size_t i = 0; i < cells_.size(); ++i ) {
        const view_cell &cell = ( i < imgui_cells_.size() && imgui_cells_[i].occupied )
                                ? imgui_cells_[i]
                                : cells_[i];
        if( cell.ch > U' ' ) {
            ++n;
        }
    }
    return n;
}

} // namespace godot_backend

#endif // GODOT
