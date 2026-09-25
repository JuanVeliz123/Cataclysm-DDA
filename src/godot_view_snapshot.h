#pragma once
#ifndef CATA_SRC_GODOT_VIEW_SNAPSHOT_H
#define CATA_SRC_GODOT_VIEW_SNAPSHOT_H

#if defined(GODOT)

#include "godot_backend.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace godot_backend
{

/// One terminal cell for the Godot-owned present path (ADR-002).
struct view_cell {
    char32_t ch = U' ';
    uint8_t fg = 0;
    uint8_t bg = 0;
    /**
     * Whether any UI actually claims this cell.
     *
     * Needed because catacurses has no notion of transparency: black is a real
     * background colour, so bg == 0 cannot mean both "black" and "nothing here".
     * Inferring emptiness from bg == 0 made every menu interior transparent and
     * unreadable over MapView.
     */
    bool occupied = false;
};

/**
 * Thread-safe ASCII/UI cell grid written by the CDDA game thread and read by
 * the Godot main thread. This is the bridge away from CPU framebuffer upload:
 * C++ still fills catacurses windows; Godot draws the cells with TextServer.
 */
class ViewSnapshot
{
    public:
        void resize( int cols, int rows );
        void set_palette( const palette_array &pal );

        void set_cell( int x, int y, char32_t ch, uint8_t fg, uint8_t bg );
        void clear_rect( int x, int y, int w, int h, uint8_t bg = 0 );
        /// Wipe every cell (space / black). Used after a blocking C++ menu returns.
        void clear_all();
        /// Write @p n cells starting at (x,y) under a single lock.
        void blit_row( int x, int y, const view_cell *cells, int n );

        /**
         * ImGui overlay layer, composited above the curses cells on read.
         *
         * ImGui is a separate layer, not more curses content: ImTui re-renders its
         * whole screen every frame, so blitting it into the curses grid both
         * clobbered windows drawn earlier in the same pass and left a closed menu's
         * cells behind with nothing to overwrite them. Keeping it separate means
         * clear_imgui() at the start of each blit removes a dismissed menu for free.
         */
        void clear_imgui();
        /// True while the ImGui layer holds at least one claimed cell.
        bool imgui_active() const;
        /// True while *any* layer claims a cell, i.e. a C++ screen is on screen.
        /// Godot uses this to know when to stop intercepting keys and let the
        /// legacy UI have them, so a screen it opened can still be closed.
        bool any_content() const;
        void set_imgui_cell( int x, int y, char32_t ch, uint8_t fg, uint8_t bg );

        int cols() const;
        int rows() const;
        int cell_pixel_w() const;
        int cell_pixel_h() const;
        void set_cell_pixel_size( int w, int h );

        /// Pack [codepoint, fg, bg, occupied] quads into @p out (cols*rows*4),
        /// with the ImGui layer composited over the curses layer.
        void copy_cells( std::vector<int32_t> &out ) const;
        /// Ints per cell in @ref copy_cells; keep terminal_view.gd in step.
        static constexpr int cell_stride = 4;
        /// Pack palette as RGBA bytes (palette_array size * 4).
        void copy_palette_rgba( std::vector<uint8_t> &out ) const;
        /// Non-space / non-nul cells (for host diagnostics).
        int count_glyph_cells() const;

    private:
        mutable std::mutex mutex_;
        int cols_ = 0;
        int rows_ = 0;
        int cell_w_ = 8;
        int cell_h_ = 16;
        std::vector<view_cell> cells_;
        std::vector<view_cell> imgui_cells_;
        /// Whether the ImGui layer currently claims anything. Lets the game thread
        /// tell "no menu is up" from "a menu was up and its frame is stale".
        bool imgui_active_ = false;
        palette_array palette_{};
};

ViewSnapshot &get_view_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_VIEW_SNAPSHOT_H
