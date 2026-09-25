#pragma once
#ifndef CATA_SRC_GODOT_DISTRACTION_SNAPSHOT_H
#define CATA_SRC_GODOT_DISTRACTION_SNAPSHOT_H

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
 * The distractions manager (MENU-14's first screen) as a Godot Control.
 *
 * The simplest shape in MENU-14: one flat list of toggles, no tabs, no filter,
 * no nested popup, and nothing to save explicitly -- each row is a `bool *`
 * straight into `uistate`, already persisted the way the rest of `uistate` is.
 * A toggle is addressed by absolute row index, which is stable here because
 * the row list never filters or reorders while the screen is open.
 */
class DistractionSnapshot
{
    public:
        bool active() const;
        uint64_t generation() const;

        // --- Godot thread -------------------------------------------------
        godot::Dictionary copy_state() const;
        void request_action( const std::string &action );
        /// A clicked row, by absolute index into the published list.
        void request_toggle( int index );
        void note_attended();
        bool attended() const;

        // --- game thread ---------------------------------------------------
        struct row {
            std::string name;
            std::string description;
            bool enabled = false;
        };
        struct data {
            std::string title;
            std::vector<row> rows;
        };

        void publish( const data &d );
        void clear();

        /**
         * Block until the panel asks for something.
         *
         * @param index out: the toggled row, or -1.
         * @return "GODOT_TOGGLE", "QUIT", or "" when no panel attended -- the
         *         caller must then run the legacy ImGui loop.
         */
        std::string next_action( int &index );

    private:
        mutable std::mutex mutex_;
        bool active_ = false;
        data data_;
        uint64_t generation_ = 0;
        std::vector<std::string> pending_actions_;
        std::atomic<int> pending_toggle_{ -1 };
        std::atomic<bool> attended_{ false };
};

DistractionSnapshot &get_distraction_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_DISTRACTION_SNAPSHOT_H
