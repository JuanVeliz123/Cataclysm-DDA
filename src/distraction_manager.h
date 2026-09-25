#pragma once
#ifndef CATA_SRC_DISTRACTION_MANAGER_H
#define CATA_SRC_DISTRACTION_MANAGER_H

namespace distraction_manager
{

class distraction_manager_gui
{
    public:
        void show();

#if defined(GODOT)
    private:
        /// Show this screen as a Godot panel and block until it is dismissed.
        /// @return false when no panel attended, so the caller must run the
        ///         legacy ImGui loop instead.
        bool run_in_godot();
        void publish_to_godot();
#endif
};

} // namespace distraction_manager

distraction_manager::distraction_manager_gui &get_distraction_manager();

#endif // CATA_SRC_DISTRACTION_MANAGER_H
