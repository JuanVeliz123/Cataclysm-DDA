#pragma once
#ifndef CATA_SRC_GODOT_SURROUNDINGS_SNAPSHOT_H
#define CATA_SRC_GODOT_SURROUNDINGS_SNAPSHOT_H

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
 * The surroundings list ("look around") as a Godot Control.
 *
 * Same shape as the dialogue takeover: `surroundings_menu::execute` is a loop
 * that reads an action and dispatches, so only the source of the action moves.
 * Everything the loop does with one stays where it is -- routing to a travel
 * destination, firing at the selection, the examine pane, the filter popups.
 *
 * Unlike the overmap sidebar this is not recorded from the drawing. The rows
 * there funnel through one text helper; here they are built inline with tables,
 * per-row ImGui ids and width arithmetic, so there is nothing clean to intercept.
 * The rows are published from `map_entity_stack` instead, which is where the
 * name, distance, colour and category actually come from -- the same source the
 * drawing reads.
 *
 * A clicked row arrives as an index and is applied by moving the selection to
 * it, because that is the only way the tab data lets a caller select: there is
 * no set-by-index, only move_selection( delta ).
 */
class SurroundingsSnapshot
{
    public:
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_state() const;
        void request_action( const std::string &action );
        void request_select( int index );
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        struct row {
            std::string text;
            /// "12 NE" and the like, already formatted.
            std::string distance;
            std::string color;
            std::string category;
            int count = 1;
        };
        struct tab {
            std::string title;
            int count = 0;
        };

        void publish( const std::vector<tab> &tabs, int tab_index,
                      const std::vector<row> &rows, int selected,
                      const std::string &filter );
        void clear();

        /**
         * Block until the panel asks for something.
         *
         * @param select out: a row to move the selection to, or -1.
         * @return the action string, "GODOT_SELECT" for a clicked row, or ""
         *         when no panel attended -- the caller must then drive this
         *         turn from its own UI. The sentinel is prefixed because the
         *         menu has its own action namespace and a collision would be
         *         silent.
         */
        std::string next_action( int &select );

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        std::vector<tab> tabs_;
        std::vector<row> rows_;
        int tab_index_ = 0;
        int selected_ = 0;
        std::string filter_;
        uint64_t generation_ = 0;
        std::string pending_action_;
        std::atomic<int> pending_select_{ -1 };
        std::atomic<bool> attended_{ false };
};

SurroundingsSnapshot &get_surroundings_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_SURROUNDINGS_SNAPSHOT_H
