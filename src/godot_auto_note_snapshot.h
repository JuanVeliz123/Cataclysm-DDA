#pragma once
#ifndef CATA_SRC_GODOT_AUTO_NOTE_SNAPSHOT_H
#define CATA_SRC_GODOT_AUTO_NOTE_SNAPSHOT_H

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
 * The auto notes manager (MENU-14) as a Godot Control.
 *
 * Same loop-split takeover as `medical_ui`: two tabs (character / global),
 * lifted into `bCharacter` the way `medical_ui`'s tab was lifted into a
 * member, and a row cursor addressed by absolute index into whichever tab's
 * display cache is current -- exactly like `mission_ui`'s "a tab switch
 * always refetches" rule, since character and global really are two
 * different lists, not two views of one.
 *
 * "Change symbol" opens a nested `string_input_popup_imgui` and (if the typed
 * symbol is non-empty) a colour-pick `uilist` -- both already Godot panels,
 * but this channel is *suspended* around the call the same way `MedicalSnapshot`
 * suspends for its item picker: without it the hidden half of this screen
 * would keep answering the door for the popup underneath.
 *
 * Nothing here writes to the real settings until the legacy epilogue's
 * "Save changes?" prompt -- this channel only ever touches the same in-memory
 * caches (`char_mapExtraCache` / `global_mapExtraCache` / the symbol caches)
 * the ImGui loop did, so `show()`'s post-loop code runs unmodified whichever
 * loop produced the changes.
 */
class AutoNoteSnapshot
{
    public:
        /// False while suspended, so the host hides the panel for the
        /// duration of the nested symbol/colour popups.
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_state() const;
        void request_action( const std::string &action );
        /// A clicked row, absolute index into the active tab's display list.
        void request_toggle( int index );
        void request_enable( int index );
        void request_disable( int index );
        /// A clicked tab: 0 = character, 1 = global.
        void request_tab( int tab );
        /// A clicked "change symbol" button on row @p index.
        void request_symbol( int index );
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        struct row {
            std::string name;
            std::string symbol;
            std::string symbol_color;
            bool has_custom_symbol = false;
            bool enabled = false;
        };
        struct data {
            std::string title;
            int selected_tab = 0;
            bool empty_mode = false;
            bool auto_notes_map_extras = false;
            std::vector<row> rows;
        };

        void publish( const data &d );
        void set_suspended( bool suspended );
        void clear();

        /**
         * Block until the panel asks for something.
         *
         * @param index out: the row half of a toggle/enable/disable/symbol
         *        request, or -1.
         * @param tab out: the tab half of a "GODOT_TAB" request, or -1.
         * @return "GODOT_TOGGLE", "GODOT_ENABLE", "GODOT_DISABLE",
         *         "GODOT_TAB", "GODOT_SYMBOL", "SWITCH_OPTION", "QUIT", or ""
         *         when no panel attended -- the caller must then run the
         *         legacy ImGui loop.
         */
        std::string next_action( int &index, int &tab );

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        data data_;
        uint64_t generation_ = 0;
        std::vector<std::string> pending_actions_;
        std::atomic<int> pending_toggle_{ -1 };
        std::atomic<int> pending_enable_{ -1 };
        std::atomic<int> pending_disable_{ -1 };
        std::atomic<int> pending_symbol_{ -1 };
        std::atomic<int> pending_tab_{ -1 };
        std::atomic<bool> suspended_{ false };
        std::atomic<bool> attended_{ false };
};

AutoNoteSnapshot &get_auto_note_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_AUTO_NOTE_SNAPSHOT_H
