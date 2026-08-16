# Godot Migration — Changelog

Everything on the `godot-mig` branch, by area rather than in the order it
happened. The branch replaces Cataclysm-DDA's SDL/curses presentation layer with
Godot 4 while leaving game logic untouched: C++ decides *what* is on screen,
Godot decides *how* it is drawn.

The build is additive. `TILES` (SDL) and curses still build and run; `GODOT=1`
selects the new backend.

---

## Architecture

**A GDExtension, not an embedded engine.** The game builds as a shared library
(`libcataclysm-godot.*`) that Godot loads. `CDDAHost` (a `godot::Node`) owns
bootstrap, the session, and every C++/Godot boundary crossing. The game runs on
its own thread; Godot's main thread never blocks on it.

**Snapshots, not shared state.** The game thread fills mutex-guarded C++ structs
and bumps a generation counter; the Godot thread copies them into Dictionaries
when the counter moves. No game object is ever touched from the render thread,
and no `godot::Ref` is ever held by game-side code (see *Teardown* below for why
that second rule exists).

**An API version handshake.** `CDDAHost::api_version()` must equal `host.gd`'s
`REQUIRED_API_VERSION`, currently **11**. The GDExtension is compiled but the
scripts are read from disk every run, so a stale library against new scripts
otherwise *looks* like it works — the layout appears and every field the old
library does not emit reads back as zero or empty. Bump it whenever the
dictionaries or methods change.

Decisions and their alternatives: [`architecture_adr.md`](architecture_adr.md).

---

## Rendering

- **Map on the GPU.** `MapView` draws from `godot_map_snapshot` draw lists into
  one `MultiMeshInstance2D` per (layer, atlas, sway, palette). Forward+ renderer.
  Rotation and mirroring ride in per-instance `rot_flags`.
- **No CPU raster path.** The framebuffer / geometry / font contracts,
  `godot_font.*`, `godot_geometry.*`, `godot_tiles_rendering.*` and the `GameView`
  node were deleted rather than kept as a fallback. Cells go to `ViewSnapshot`,
  tiles to `MapSnapshot`, the minimap to its own RGBA snapshot. Do not
  reintroduce a CPU surface.
- **Light and memory as a pass, not as art.** One texel per tile in a small
  texture: R visibility (nearest), G light (linear), B fire (linear), sampled by
  `map_tiles.gdshader`. Tinting in the shader rather than compositing on the CPU.
- **Sprite resolution with a five-step fallback chain**, category-aware, with
  `describe_sprite()` able to explain any id's outcome. A tile that resolves to
  nothing at all draws its JSON symbol via `glyph_layer.gd` rather than a hole.
- **Character overlays**: worn, wielded and mutation sprites over the body, for
  the avatar, NPCs and monsters.
- **Fields as particles.** Fire and smoke are pooled `GPUParticles2D`, wind-driven.
- **Sway** shears only sprites that overhang their tile. An earlier version
  sheared everything and tore grass apart at the tile seams.
- **Overmap and pixel minimap** are Godot nodes drawing from their own snapshots.
- **Tileset is a build artifact.** `gfx/UltimateCataclysm/` is composed from
  upstream source art by `build-scripts/compose-tileset.sh`, not committed.
  Without it the backend falls back to `ASCIITiles`, which looks like a broken
  renderer rather than missing art.

---

## Screens migrated off the overlay

The ImGui/curses overlay still exists, but these no longer use it. Each is a
Godot `Control` fed by its own snapshot channel, with the game thread blocked
where it always blocked.

| Screen | Channel | Panel |
|---|---|---|
| Splash, main and load menus, world pick, chargen | `godot_chargen.*` | `chargen.gd`, `world_pick.gd` |
| Session HUD, inventory, character sheet | `godot_hud_snapshot.*` | `hud_panel.gd`, `inventory_panel.gd`, `character_panel.gd` |
| The Escape menu | `godot_game_commands.*` | `game_menu_panel.gd` |
| `uilist` — ~90% of the game's menus, with category tabs | `godot_uilist_snapshot.*` | `uilist_panel.gd` |
| `query_popup` prompts, notices, and "press any key" | `godot_popup_snapshot.*` | `popup_panel.gd` |
| Single-line text entry | `godot_popup_snapshot.*` | `popup_panel.gd` |
| Item info and extended tile description | `godot_textwin_snapshot.*` | `textwin_panel.gd` |
| Options | `godot_options_snapshot.*` | `options_panel.gd` |
| Keybindings | `godot_keybind_snapshot.*` | `keybind_panel.gd` |
| Crafting | `godot_crafting_snapshot.*` | `crafting_panel.gd` |

### The takeover contract

Two invariants, both learned by breaking them:

1. Check `is_shutdown_requested()` **first** in the wait loop. Otherwise quitting
   with a menu open hangs.
2. Give up after ~1.5s if no panel attends, and hand back to the legacy path. A
   game thread waiting on an answer nobody will give is indistinguishable from a
   crash.

### Two shapes of takeover

**Split the loop out** where the screen is a data model with a loop around it.
`options_manager::show()` became `show_legacy()` plus a shared epilogue, and the
Godot path stands in for exactly the loop — so change detection, "Save changes?",
world-vs-global option routing, language reload and terminal resize all still run
once, in one place. `input_context::display_menu()` got the same treatment.

**Drive the existing state machine** where the screen already knows more than you
do. `crafting_ui_impl` owns tabs, filter, batch mode and the recipe list, and
`process_action()` already knows that batch mode auto-engages on the first
increment and that the selection must survive a recalculation. The crafting panel
reimplements none of it: it sends back the same action strings and sets the same
pending-intent fields the ImGui layer sets. One state machine, two front ends.

### Notable details

- **Keybinding capture does not describe the key.** A rebind has to produce the
  exact `input_event` the game will later compare against; a described-and-
  reconstructed key looks right in the list and silently fails in play. The
  prompt goes up on the notice channel, the panel forwards the raw Godot event to
  the input bridge, and the game thread reads the translated event back.
- **Crafting requirements come from `requirement_data`.** `get_folded_*_list()`
  has already resolved each line against the crafting inventory and coloured it.
  Recomputing availability panel-side would be a second opinion about what the
  player is carrying.
- **Colour that carries meaning survives the trip.** `color_tags.gd` converts
  CDDA `<color_c_light_green>` markup to BBCode. Other panels strip tags, which
  is right when colour decorates — in a requirement list, green versus red *is*
  the answer to "do I have this?".

### Not migrated

Safe mode, auto pickup, auto notes, distractions, colors and help still draw in
the overlay, and are labelled "Legacy screens" in the game menu so the state of
the migration is visible from the menu itself. Advanced inventory (MENU-8) and
deleting the overlay (MENU-9) are open. Crafting's interactive step/variant table
is MENU-6c; the panel names a recipe's steps and says they cannot be chosen there
yet rather than silently showing a shorter recipe than the one the player gets.

---

## Input

`GodotInputBridge` translates `godot::InputEvent` into CDDA `input_event` and
queues them for the game loop. Two bugs worth remembering:

- **Character events must carry no modifiers.** `input_event::operator==`
  compares the modifier set, and the shift is already in the character, so
  attaching Shift made every shifted key match no binding at all — which is why
  safe mode could not be turned off and the game appeared stuck. Modifiers are
  attached for `keyboard_code` only; Ctrl in character mode becomes a control code.
- **Mouse coordinates are cells, not pixels.** The Godot build leaves `TILES`
  undefined, so `input_context::get_coordinates` takes the TUI branch and reads
  `mouse_pos` as cell coordinates. The bridge is told the grid geometry.

`host.gd` yields the keyboard whenever a C++ screen or any popup is up, including
non-modal notices — without that, "grab what?" and "examine where?" had no way to
be dismissed, because Escape opened the Godot menu instead of reaching the game
that was waiting for it.

---

## Build

Root `Makefile`, `GODOT=1`. macOS, Linux and Windows (MinGW cross from Linux).
The CMake `GODOT=ON` target is unsupported. Full guide:
[`doc/c++/COMPILING-GODOT.md`](../../doc/c++/COMPILING-GODOT.md).

```bash
./build-scripts/get-godot-cpp.sh                                # bindings, once per platform
make GODOT=1 -j$(sysctl -n hw.ncpu)                             # macOS
make GODOT=1 NATIVE=linux64 -j$(nproc)                          # Linux x86_64
make GODOT=1 CROSS=x86_64-w64-mingw32- BACKTRACE=0 -j$(nproc)   # Windows x86_64 (cross)
```

- `BACKTRACE=0` on Windows: `-lbacktrace` has no MinGW package.
- **Header dependencies are generated in a second, PCH-free pass.** GCC emits no
  header deps under `-include <pch>`, so edited headers did not trigger rebuilds
  and produced stale objects that crashed in ways that looked like memory
  corruption. Objects also depend on the `Makefile` itself.
- **Install is write-then-rename, in a single recipe line.** `cp` rewrites in
  place, which invalidates the pages of a running Godot's mmap and gives it
  SIGBUS. The temp name is unique per build. It must be one line because make
  gives each line its own shell, so `$$` differs between them.
- `GODOT_EXCLUDE_SOURCES` drops SDL-only translation units that would otherwise
  produce zero-symbol objects.

---

## Teardown

Quitting used to exit 139. The backtrace:

```
Thread 1   ~generic_factory<oter_type_t>       (static destruction)
Thread 21  game_do_turn -> update_hud_snapshot
             -> Character::get_stamina_max
             -> get_option<int>( <garbage std::string> )
```

The game thread had just finished a turn and was publishing the HUD, microseconds
from the game loop's own quit check, while the main thread was already destroying
the option map underneath it.

Fixed by waiting on a flag the thread sets as its last act, then joining; if it
never arrives within a second the thread is parked somewhere that ignores the
shutdown flag, and the process says so and `_Exit(0)`s rather than running static
destructors against a live thread. A separate fix releases the backend's
`godot::Ref` state in `~CDDAHost` — left to static destruction it unrefs Objects
after Godot has torn down its ObjectDB, which is where the "leaked ObjectDB
instances at exit" warning came from.

---

## Verification

No CI by design. Four things to run:

| What | Command | Catches |
|---|---|---|
| Library loads | `godot --headless --path godot --editor --quit` | GDExtension registration. `--editor` is required; a plain `--quit` on a project with no `.godot/` cache exits 0 even when the library cannot load |
| Behaviour | `godot --headless --path godot res://scenes/headless_probe.tscn` | Boots, plays now, drives keys, builds and refreshes every real panel, and reports what reached each snapshot |
| Appearance | the `xvfb-run` + `lvp_icd.json` recipe in [`AGENT_HANDOFF.md`](AGENT_HANDOFF.md) | What it actually looks like. Slow, but it is the difference between "the numbers are right" and "the picture is right" |
| Shaders | `godot --headless --path godot res://scenes/shader_check.tscn` | Nothing else notices a broken `.gdshader` — the probe builds materials but never draws |

`CDDA_GODOT_USER_DIR` gives a run its own saves and config, so two agents can
verify at once without their worlds colliding.

**`timeout` does not kill Godot under `xvfb-run`** — it kills the wrapper and the
grandchild survives at 200-400% CPU. Finish with `pkill -9 -f headless_probe.tscn`,
and `pgrep -a` first: a broader pattern also matches the wrapper shell running the
cleanup.

---

## The recurring bug of this migration

Worth reading before adding anything that draws, because it cost seven separate
bugs and every one of them reported success.

The migration moved *present* to Godot, but several producers stayed wired to the
**curses frame boundary** — the end of `game::draw()`, `w_terrain`, a `ui_adaptor`
redraw callback. Under `GODOT` none of those run. The code still executes: it is
computed, stepped every turn, and discarded before anything can show it.

Three features shipped that way — the pixel minimap, scrolling combat text, and
the entire animation overlay (explosions, bullets, the aim cursor, hit markers).
The measurement that made the third legible, over a session containing a real
fight:

```
commits: 0   glyphs_added: 0   texts_added: 0   hits_added: 1   generation: 0
```

`hits_added: 1` is the control — that one path is called from game logic rather
than a draw callback, so it works, which is what makes the zeroes a finding
rather than a quiet session.

The verification harness had the same disease twice over: a one-shot latch meant
every prompt after the first went unanswered, and an unattended NPC menu parked
the game thread — each silently skipping every later stage while still exiting 0.
And the crafting takeover had it twice more: pending tab intents are consumed by
`draw_controls()` rather than `process_action()` and so did nothing, and a
`recalc` flag was never acted on because an event-driven loop has no next frame
to do it in.

Seven instances, one shape: **the half that does not run looks identical to the
half that does.** Two rules follow.

1. A producer is not done until something has *drawn* it. Not "the snapshot is
   populated" — a screenshot, or a consumed-generation count that moves.
2. Suspect anything hanging off a `draw()` callback or a `catacurses::window`.
   If a feature has never been seen on screen, assume it is on a dead path before
   assuming it is subtly wrong.

Filed as **VER-0** in [`BACKLOG.md`](BACKLOG.md): fail the probe when a generation
counter that should move stays at zero, and when a fixture stage never executed.
About five lines, and it would have caught all seven.

---

## Where to pick up

[`BACKLOG.md`](BACKLOG.md) has the remaining work sized and ordered.
[`AGENT_HANDOFF.md`](AGENT_HANDOFF.md) has the constraints, the build recipes, and
the traps.
