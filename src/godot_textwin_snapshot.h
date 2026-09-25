#pragma once
#ifndef CATA_SRC_GODOT_TEXTWIN_SNAPSHOT_H
#define CATA_SRC_GODOT_TEXTWIN_SNAPSHOT_H

#if defined(GODOT)

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <godot_cpp/variant/dictionary.hpp>

namespace godot_backend
{

/**
 * Read-only text windows rendered as a Godot Control.
 *
 * A surprising share of the game's remaining C++ screens are just formatted text
 * you scroll and dismiss: item info, the extended description of a tile, help
 * pages. They differ in how the text is produced, not in how it behaves, so one
 * panel serves all of them and each caller only has to hand over strings.
 *
 * Tabs are part of the model because the extended description has four of them
 * (creature / furniture / terrain / vehicle). A window with one unnamed tab is
 * the ordinary case.
 */
class TextWinSnapshot
{
    public:
        struct tab {
            std::string label;
            /// Body text, colour tags already stripped.
            std::string body;
        };

        bool active() const;
        uint64_t generation() const;
        godot::Dictionary copy_state() const;

        // --- Godot thread -------------------------------------------------
        void select_tab( int index );
        void dismiss();
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        void publish( const std::string &title, const std::string &subtitle,
                      const std::vector<tab> &tabs, int current );
        void clear();
        /// -1 while the window is still up; otherwise the tab last shown.
        bool dismissed() const;
        /// Tab the panel wants, or -1 if unchanged.
        int requested_tab() const;

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        std::string title_;
        std::string subtitle_;
        std::vector<tab> tabs_;
        int current_ = 0;
        uint64_t generation_ = 0;
        std::atomic<int> requested_tab_{ -1 };
        std::atomic<bool> dismissed_{ false };
        std::atomic<bool> attended_{ false };
};

TextWinSnapshot &get_textwin_snapshot();

/**
 * Show @p tabs as a Godot text window and block until dismissed.
 *
 * @param current in/out: the tab to open on, and the tab left showing.
 * @return false when no panel attended, so the caller must use its own UI.
 */
bool run_textwin_in_godot( const std::string &title, const std::string &subtitle,
                           const std::vector<TextWinSnapshot::tab> &tabs, int &current );

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_TEXTWIN_SNAPSHOT_H
