#pragma once
#ifndef CATA_SRC_GODOT_GAME_COMMANDS_H
#define CATA_SRC_GODOT_GAME_COMMANDS_H

#if defined(GODOT)

#include <cstdint>
#include <functional>
#include <string>

namespace godot_backend
{

/**
 * In-session command channel: Godot UI -> game thread.
 *
 * A Godot panel cannot call into game logic directly. The game state belongs to
 * the CDDA thread, and touching it from Godot's main thread would race with
 * do_turn. The existing chargen path (CDDAHost::run_sync) does not help either:
 * it refuses outright once a session is active, because it relies on the game
 * thread sitting idle in the bootstrap command loop.
 *
 * So commands are queued here and run on the game thread at the one point during a
 * session where it is reliably between actions and safe to touch: the input wait
 * in godot_input_backend. That is where CDDA itself would be applying a keypress.
 *
 * Without this, a Godot menu can only ever *display* state -- which is why the
 * inventory panel was browse-only and every screen that needs to *do* something
 * still went through curses.
 */

/// Queue @p fn to run on the game thread. Safe to call from the Godot thread.
void post_game_command( std::function<void()> fn );

/**
 * Run every queued command. Game thread only, and only when it is safe to
 * re-enter game logic -- see @ref commands_safe_to_run.
 *
 * @return true if any command ran.
 */
bool drain_game_commands();

/**
 * Whether the game thread is somewhere a queued command can safely run.
 *
 * False while a C++ menu is up: the input wait is then nested inside that menu's
 * own loop, and running an action from there would re-enter game logic from
 * underneath a UI that expects to still own the interaction.
 */
bool commands_safe_to_run();

/// Menu actions the Godot game menu can ask for. Same reasoning as
/// @ref item_action: an enum so a typo fails to compile rather than silently
/// doing nothing.
enum class menu_action : int32_t {
    quicksave = 0,
    save_and_quit = 1,
    quit_without_saving = 2,
    /// Screens that are still C++ UI. They open over MapView through the curses
    /// overlay until each is migrated to a Godot Control.
    options = 3,
    keybindings = 4,
    safe_mode = 5,
    auto_pickup = 6,
    colors = 7,
    help = 8,
    auto_notes = 9,
    distractions = 10,
};

/**
 * Queue a menu action. Returns an error string, or empty on accept.
 *
 * "Accepted" is not "done": the command runs later, on the game thread.
 */
std::string request_menu_action( menu_action action );

/// Item actions a Godot panel can ask for. Kept as an enum rather than a string
/// so a typo in GDScript fails loudly instead of silently doing nothing.
enum class item_action : int32_t {
    wield = 0,
    wear = 1,
    drop = 2,
};

/**
 * Act on the carried item with @p uid.
 *
 * Items are addressed by item::uid rather than by list index: the inventory
 * snapshot the panel is showing may be a turn or two stale, and acting on the
 * wrong item because the list shifted is far worse than doing nothing.
 *
 * @return a human-readable failure reason, or an empty string when queued.
 */
std::string request_item_action( int64_t uid, item_action action );

/**
 * Spawn a monster next to the avatar. Test fixture, not gameplay.
 *
 * Everything the renderer does that involves another creature -- hit reactions,
 * scrolling combat text, monster overlays -- is unreachable from the headless
 * probe, because a fresh character stands alone in an evac shelter and nothing
 * in the probe's repertoire produces a fight. So those code paths shipped
 * without ever having run.
 *
 * This is the smallest hook that fixes that: one monster, adjacent, on the game
 * thread like every other queued command. It is the first piece of the scenario
 * harness in BACKLOG.md VER-2; teleport and set-time belong on the same channel
 * when something needs them.
 *
 * @param mtype the monster id, e.g. "mon_zombie".
 * @return a failure reason, or empty when queued.
 */
std::string request_debug_spawn( const std::string &mtype );

/**
 * Spawn a random NPC next to the avatar, for verification.
 *
 * The dialogue, faction, mission and follower screens all need somebody to talk
 * to, and a fresh character stands alone in a shelter. Without this the fixtures
 * for those screens can only run when the world happens to offer an NPC, which
 * is the difference between a check and a coin flip -- MENU-10 shipped
 * unverified for exactly that reason.
 *
 * Placed adjacent rather than at the debug menu's -4,-4, so the probe can reach
 * it by walking one step.
 */
std::string request_debug_spawn_npc();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_GAME_COMMANDS_H
