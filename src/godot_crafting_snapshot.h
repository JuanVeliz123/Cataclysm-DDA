#pragma once
#ifndef CATA_SRC_GODOT_CRAFTING_SNAPSHOT_H
#define CATA_SRC_GODOT_CRAFTING_SNAPSHOT_H

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
 * The crafting screen as a Godot Control.
 *
 * Unlike the other takeovers this one does not reimplement the screen's state
 * machine. `crafting_ui_impl` already owns the tabs, the filter, the batch mode
 * and the recipe list, and its `process_action` already knows what every action
 * does -- including the awkward parts, like auto-entering batch mode on the first
 * increment and keeping the selected recipe across a recalculation. So the panel
 * sends back the same action strings the curses screen produces, plus the
 * "pending intent" fields the ImGui layer sets for mouse clicks, and the existing
 * code does the work. A second UI layer is exactly what those fields are for.
 *
 * That leaves this channel with one job: describe what to draw.
 *
 * Two generations, for the same reason as the options screen. The recipe list
 * changes when the tab, the filter or the inventory changes; the detail pane
 * changes every time the selection moves. Rebuilding several hundred rows on each
 * arrow key would throw away the scroll position.
 */
class CraftingSnapshot
{
    public:
        bool active() const;
        uint64_t list_generation() const;
        uint64_t detail_generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_list() const;
        /// Cheap enough to poll every frame, unlike copy_list().
        int selected() const;
        godot::Dictionary copy_detail() const;
        /// Queue an action string, as produced by the crafting input context.
        void request_action( const std::string &action );
        /// Click a row / tab / subtab: sets the matching pending intent.
        void request_select( int row );
        void request_tab( int index );
        void request_subtab( int index );
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        struct row {
            std::string name;
            /// Nesting depth for a nested category.
            int indent = 0;
            /// True when the crafter could start this now.
            bool craftable = false;
            /// Craftable, but the components are rotten or favourited, or a
            /// proficiency is missing -- shown, but worth a warning colour.
            bool caveat = false;
            /// A folder rather than a recipe.
            bool nested = false;
        };
        struct tab {
            std::string id;
            std::string name;
        };
        /// One line of the detail pane. `text` may carry CDDA colour tags.
        struct detail_line {
            std::string text;
            /// A heading rather than body text.
            bool header = false;
        };

        void publish_list( const std::vector<tab> &tabs, int tab_index,
                           const std::vector<tab> &subtabs, int subtab_index,
                           const std::vector<row> &rows, int selected,
                           const std::string &filter, size_t hidden, bool batch_mode,
                           int batch_size );
        void publish_detail( const std::vector<detail_line> &lines );
        /// Move the cursor without republishing the list. The selection changes on
        /// every arrow key; the rows do not, and rebuilding them would cost the
        /// panel its scroll position.
        void publish_selection( int selected );
        void clear();

        std::vector<std::string> take_actions();
        /// -1 when nothing was clicked since the last call.
        int take_selected_row();
        int take_selected_tab();
        int take_selected_subtab();

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        std::vector<tab> tabs_;
        std::vector<tab> subtabs_;
        std::vector<row> rows_;
        std::vector<detail_line> detail_;
        std::vector<std::string> actions_;
        int tab_index_ = 0;
        int subtab_index_ = 0;
        int selected_ = 0;
        int batch_size_ = 1;
        bool batch_mode_ = false;
        size_t hidden_ = 0;
        std::string filter_;
        uint64_t list_generation_ = 0;
        uint64_t detail_generation_ = 0;
        std::atomic<int> pending_row_{ -1 };
        std::atomic<int> pending_tab_{ -1 };
        std::atomic<int> pending_subtab_{ -1 };
        std::atomic<bool> attended_{ false };
};

CraftingSnapshot &get_crafting_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_CRAFTING_SNAPSHOT_H
