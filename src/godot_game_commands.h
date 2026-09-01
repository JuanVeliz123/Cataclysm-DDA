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

/**
 * Open the follower-rules screen on the nearest ally NPC.
 *
 * The real path to this screen is a dialogue topic ("Set follower rules..."),
 * which needs a full conversation-tree walk a fixture does not have. This is
 * the same shortcut @ref request_debug_spawn_npc is for the screens it
 * unblocks: skip the path, not the screen.
 *
 * @return a failure reason ("no ally nearby"), or empty when queued.
 */
std::string request_debug_open_follower_rules();

/**
 * Scenario harness (BACKLOG.md VER-2 item 1): dress the world for verification.
 *
 * Every renderer question still open needs a scene the fresh evac-shelter spawn
 * cannot offer: night for the light pass, a lamp for its gradient, fire for the
 * glow, a forest for sway and occlusion, a staircase for the levels below. The
 * debug menu has all of these behind uilist pickers, and a queued command must
 * not open a blocking screen -- so these are the same primitives with the UI
 * stripped off, callable from a fixture.
 *
 * Shared rules:
 *  - ids are validated on the calling thread; game state is touched only inside
 *    the queued lambda ("accepted != done", as for every command here);
 *  - each command records its outcome in the scenario status (see
 *    @ref get_scenario_status) -- a teleport that found nothing can only fail on
 *    the game thread, so the caller polls the generation rather than the return;
 *  - each command ends by rebuilding the map cache and republishing the map,
 *    HUD and minimap snapshots. A mutation made at the input wait is otherwise
 *    invisible until the next player action, and the published lighting reads
 *    the map cache -- a fire lit by a command that skips the rebuild publishes
 *    the light of the tile before the fire.
 */

/// Teleport the avatar to the closest overmap terrain whose id starts with
/// @p omt_type ("forest", "basement", "house"...), searching @p search_range
/// overmap tiles out. May generate new overmaps, which takes game-thread time.
std::string request_scenario_teleport_omt( const std::string &omt_type, int search_range );

/// Teleport the avatar @p dx, @p dy, @p dz tiles from where it stands.
std::string request_scenario_teleport_rel( int dx, int dy, int dz );

/// Move the avatar onto the nearest tile with terrain/furniture flag @p flag
/// ("GOES_DOWN", "GOES_UP"...) within @p radius tiles. This is how a fixture
/// stands at the top of a staircase without knowing where one is.
std::string request_scenario_stand_on( const std::string &flag, int radius );

/// Set the time of day, only ever moving forward (timed events fire on
/// catch-up; moving backwards would replay them).
std::string request_scenario_set_time( int hour, int minute );

/// Force a weather pattern by id ("rain", "snowing"...), or clear the override
/// with an empty string. Mirrors the debug menu's non-UI tail, EOCs included.
std::string request_scenario_set_weather( const std::string &weather_id );

/// Drop a field ("fd_fire", "fd_smoke"...) of @p intensity at the avatar's
/// position plus (dx, dy).
std::string request_scenario_spawn_field( const std::string &field_id, int intensity,
        int dx, int dy );

/// Spawn a vehicle by prototype id at the avatar's position plus (dx, dy),
/// undamaged and fuelled, with every light part switched on -- the point of a
/// scenario vehicle is its headlights at night.
std::string request_scenario_spawn_vehicle( const std::string &vproto, int dx, int dy );

/// Spawn one item ("atomic_lamp"...) at the avatar's position plus (dx, dy).
std::string request_scenario_spawn_item( const std::string &itype, int dx, int dy );
/// Set furniture at an offset from the avatar (3D-8d's fixture: worldgen owes
/// no scene a bed, so the probe places its own).
std::string request_scenario_spawn_furniture( const std::string &furn, int dx, int dy );

/// Mark a map extra ("mx_helicopter"...) as discovered, the way walking into
/// one during play would -- the auto notes screen only lists extras the
/// player has actually seen, so a fresh fixture's list is otherwise empty.
std::string request_scenario_discover_map_extra( const std::string &map_extra );

/// Set the avatar's sex. The renderer publishes the avatar as
/// player_male/player_female, so a fixture that verifies a specific creature
/// mesh needs the roll to be a choice rather than a coin flip.
std::string request_scenario_set_avatar_sex( bool male );

/// Outcome of the most recent scenario command that finished on the game thread.
struct scenario_status {
    /// Increments when a scenario command finishes (well or badly). The caller
    /// polls this the way it polls every other generation counter.
    int generation = 0;
    bool ok = false;
    /// Which command, and what it has to say about how it went.
    std::string last;
    std::string detail;
};
scenario_status get_scenario_status();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_GAME_COMMANDS_H
