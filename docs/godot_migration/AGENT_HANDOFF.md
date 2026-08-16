# Godot Migration — Agent Handoff

**Audience:** future coding agents continuing the `godot-mig` branch.
**Read first:** [`architecture_adr.md`](architecture_adr.md), [`../GODOT_MIGRATION_TASKS.md`](../GODOT_MIGRATION_TASKS.md), [`../RENDERING_AND_GODOT_MIGRATION.md`](../RENDERING_AND_GODOT_MIGRATION.md).
**Picking up work:** [`BACKLOG.md`](BACKLOG.md) — sprite and menu tasks, sized and ordered, with the verdicts on the sprite-pipeline design doc.

## Current state (2026-08-16)

- **Architecture (ADR-002 + ADR-003):** Godot owns present. C++ keeps game logic + tileset
  id resolution. In-session map is drawn by **`MapView`** from
  `godot_map_snapshot` draw lists (UltimateCataclysm). Session HUD, inventory,
  and character sheet are Godot Controls from `godot_hud_snapshot`. TerminalView /
  framebuffer `TextureRect` are optional debug (`USE_TERMINAL_DEBUG` /
  `USE_LEGACY_FRAMEBUFFER_VIEW` = false).
- **Pre-game chrome:** Godot Controls (splash, main/load menus, world pick, custom chargen).
- **In-session screens now Godot Controls:** the game menu, `uilist` (with category
  tabs), `query_popup` prompts and notices, single-line text entry, read-only text
  windows (item info, extended description), **options**, **keybindings**, and
  **crafting**. Each follows the same takeover contract, and the two rules in it
  were both learned by breaking them:

  1. check `is_shutdown_requested()` **first** in the wait loop;
  2. give up after ~1.5s if no panel attends, and hand back to the legacy path.

  Two shapes of takeover, and the second is usually the better one. Where the
  screen is a data model with a loop around it (options), the loop is split out
  as `*_legacy()` and the Godot path stands in for exactly it, sharing the
  epilogue. Where the screen already has a state machine that knows more than you
  do (crafting), do **not** reimplement it: send back the same action strings and
  set the same pending-intent fields the ImGui layer sets, and let it decide. One
  state machine, two front ends.

  **Two traps in that second shape, both of which cost a day here.** They come
  from the same place: the ImGui loop runs every frame and does housekeeping as
  it passes, and an event-driven panel loop wakes only when asked.

  1. **The pending-intent fields are not all consumed in the same place.** In
     `crafting_ui_impl`, `pending_line_click` is consumed by `process_action()`
     but `pending_tab_index` and `pending_subtab_index` are consumed by
     `draw_controls()` — which never runs under Godot. Setting them looks right,
     compiles, and silently does nothing. Grep for where each field is *read*
     before assuming a driver will pick it up.
  2. **`recalc`-style deferred work needs somewhere to happen.** An action that
     changes a tab or a filter often only sets a flag, and the rebuild happens at
     the top of the *next* action. The ImGui loop always has a next one. Call the
     recalculation yourself after processing actions, or the panel publishes the
     previous state and looks like a screen that found nothing.

  Both fail the same way everything else in this doc fails: the half that does
  not run is indistinguishable from the half that does.
- **No CPU raster path (2026-08-14):** the `framebuffer` / `geometry` / `font`
  contracts, `godot_font.*`, `godot_geometry.*`, `godot_tiles_rendering.*`,
  `godot_animation.*` and the `GameView` node are gone. Cells go to
  `ViewSnapshot`, tiles to `MapSnapshot`, the minimap to its own RGBA snapshot.
  Do not reintroduce a CPU surface.
- **Tileset data:** `gfx/UltimateCataclysm/` is a **build artifact**, not something
  you download. `/gfx/*` stays gitignored; compose it from upstream source art:

  ```bash
  ./build-scripts/compose-tileset.sh            # UltimateCataclysm, ~115 MB fetched
  ./build-scripts/compose-tileset.sh --list     # what else upstream has
  ```

  It needs pyvips (`libvips` + `pip install pyvips`), which is what
  `tools/gfx_tools/compose.py` has always needed. Without a composed tileset the
  backend silently falls back to `ASCIITiles` and the map renders as coloured
  letters — which looks like the renderer failing rather than the art missing.
- **Build (supported):** root Makefile, macOS / Linux / Windows. Full guide:
  [`doc/c++/COMPILING-GODOT.md`](../../doc/c++/COMPILING-GODOT.md).

```bash
./build-scripts/get-godot-cpp.sh                    # bindings, once per platform

make GODOT=1 -j$(sysctl -n hw.ncpu)                 # macOS
make GODOT=1 NATIVE=linux64 -j$(nproc)              # Linux x86_64
make GODOT=1 CROSS=x86_64-w64-mingw32- BACKTRACE=0 -j$(nproc)   # Windows x86_64 (cross)
# BACKTRACE=0 on the Windows cross-build: -lbacktrace has no MinGW package.
# GODOT_CPP_DIR=~/godot-cpp   # default
# GODOT_ARCH=arm64            # default on Apple Silicon, even under Rosetta
```

- Output: the per-platform name from `cataclysm.gdextension`
  (`.dylib` / `.so` / `.dll`) → copied to `godot/bin/` (gitignored).
- Run: `godot --path godot`
  (macOS: `arch -arm64 /Applications/Godot.app/Contents/MacOS/Godot --path godot`)
- Load check without a window: `godot --headless --path godot --editor --quit`.
  Use `--editor`: a plain `--quit` run on a project with no `.godot/` cache
  never verifies GDExtensions and exits 0 even when the library cannot load.
- **Actually seeing it, without a display.** The probe reports what was
  published and built, not what it looks like. A virtual display plus Mesa's
  software Vulkan renders for real and saves a PNG:

  ```bash
  xvfb-run -a -s "-screen 0 1600x900x24" \
    env VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
    godot --rendering-driver vulkan --path godot \
          res://scenes/headless_probe.tscn -- --screenshot /tmp/map.png
  ```

  Slow (llvmpipe manages ~45 fps at 1600x900 on this map) and no use for
  judging feel, but it is the difference between "the numbers are right" and
  "the picture is right". It caught the light gradient working and would have
  caught a black screen. Requires `Xvfb` and the `lvp_icd.json` Vulkan ICD, both
  present here.

  **`timeout` does not kill Godot under `xvfb-run`.** It kills the wrapper; the
  grandchild survives and keeps running at 200-400% CPU forever. Three of those
  accumulated in one session took the machine to load 10+, and the symptom is
  that a probe run which used to take ten minutes appears to hang in worldgen —
  which reads as a bug in whatever you just changed. Always finish with:

  ```bash
  pgrep -a -f headless_probe.tscn     # look first
  pkill -9 -f headless_probe.tscn || true
  ```

  and check `ps` before believing a stall is yours. Look before you `pkill`, and
  match on the **scene path**, not the binary: a broader pattern also matches the
  wrapper shell running your own compound command, so the cleanup kills the job
  doing the cleaning. That has happened twice here.

  `xvfb-run` leaks its X server too — one abandoned `Xvfb` per run that did not
  exit cleanly, plus a stale `/tmp/xvfb-run.*` auth dir. Harmless but they
  accumulate; `pkill Xvfb` when nothing is running.
- **Shader check:** `godot --headless --path godot res://scenes/shader_check.tscn`.
  Nothing else notices a broken `.gdshader` — the probe builds materials but never
  draws. The dummy rendering driver still runs Godot's shader language front end,
  so syntax errors, unknown identifiers and type mismatches are caught; a GPU
  backend rejecting valid code is not.
- **Behaviour check without a window:**
  `godot --headless --path godot res://scenes/headless_probe.tscn`.
  Boots, plays now, drives keys through the input bridge, and reports what
  reached `ViewSnapshot` plus the HUD/inventory dictionaries. It also builds and
  refreshes the real panels, so a Control that only fails at runtime is caught --
  parsing alone will not catch it. Use this before claiming any UI change works;
  the load check above only proves the library loads.
- No CI job by design: the maintainer builds on their own platforms.

## What works enough to build

| Area | Files | Notes |
|------|-------|-------|
| GDExtension entry | `godot/extensions/godot_ext.cpp` | bootstrap, session, chargen, map/tileset APIs |
| Godot UI host | `godot/scripts/host.gd`, `scenes/main.tscn` | Splash → menu → MapView |
| Tileset present | `godot/scripts/map_view.gd`, `src/godot_map_snapshot.*`, `godot/shaders/map_tiles.gdshader` | Draw-list → one `MultiMeshInstance2D` per (layer, atlas, sway, palette) |
| Sprite resolution | `src/godot_map_snapshot.cpp` (`resolve_sprite`) | Five-step fallback chain, category-aware; `describe_sprite()` explains any id |
| Character overlays | `resolve_overlay_id` in `godot_map_snapshot.cpp` | Worn / wielded / mutation sprites over the body, for the avatar, NPCs and monsters. `get_avatar_overlays()` reports what resolved |
| Light / memory pass | `src/godot_light_snapshot.*`, `map_tiles.gdshader` | One texel per tile: R visibility (nearest), G light (linear), B fire (linear) |
| Fallback glyphs | `godot/scripts/glyph_layer.gd` | A tile with no sprite anywhere draws its JSON symbol, one node per layer |
| Field particles | `godot/scripts/field_particles.gd` | Fire and smoke as `GPUParticles2D`, pooled, wind-driven |
| Render debug overlay | `godot/scripts/debug_overlay.gd` | F3. Fallback levels, per-layer counts, top sprite misses |
| Session HUD | `hud_panel.gd`, `inventory_panel.gd`, `character_panel.gd`, `src/godot_hud_snapshot.*` | Sidebar + overlays; `i` / `@` intercepted; inventory can wield/wear/drop |
| Design system | `godot/scripts/nocturne.gd` | Nocturne tokens + `panel_style` / `apply_button` / `section_header` / `divider` / `kv_row`. Sidebar and inventory follow it; take colours and spacing from here rather than hard-coding, so a retheme stays a one-file change |
| Tileset load | `src/godot_tileset_loader.*` | Atlas → Godot ImageTexture; one atlas only, lighting is a GPU tint |
| Overmap | `godot_overmap_snapshot.*`, `overmap_view.gd` | Own tileset (`OVERMAP_TILES`); RAII guard marks the UI on screen |
| UI commands | `src/godot_game_commands.*` | Godot panels queue work run on the game thread at its input wait |
| Curses overlay | `terminal_view.gd`, `godot_view_snapshot.*` | Carries every screen not yet a Godot Control. Two layers: curses cells, and an ImGui layer blitted from ImTui. A cell shows only if something claimed it |
| Headless probe | `godot/scripts/headless_probe.gd`, `scenes/headless_probe.tscn` | Boots + drives keys + dumps snapshot state; the only way to verify UI here |
| Pixel minimap | `godot_pixel_minimap.*`, `minimap_panel.gd` | Per-turn RGBA snapshot → TextureRect |
| Animations | `godot_anim_snapshot.*`, `anim_overlay.gd` | Draw callbacks publish map-positioned glyphs; overlay is a MapView child |
| Input bridge | `godot_input_bridge.*`, `godot_input_backend.*` | Active while MapView/session shown |
| Custom chargen | `godot_chargen.*`, `chargen.gd`, `world_pick.gd` | Async begin/confirm |

## Highest-priority gaps (pick these next)

### 1. MapView fidelity

Done: batching, map memory, fields, vehicles, item stacks, zoom,
connected-terrain subtiles + rotation, the full sprite fallback chain, the
per-tile light/visibility texture, hit reactions, field particles, sway,
palette swaps, and character overlays (SP-1…SP-10 plus overlays, see
[`BACKLOG.md`](BACKLOG.md)).
Open: z-levels, `override_look` mutations, and the T2.6 remainder — weather,
scrolling combat text, and explosions as particles rather than ASCII rings.

**Read [`BACKLOG.md`](BACKLOG.md) Part 3 before doing more rendering work.** It
lists what each verification route can and cannot answer, the constants nobody
has ever looked at (VER-1), and the tooling that would move verification off a
person (VER-2).

**Most of the sprite work has not been seen in motion.** It was built on a
machine with no display. Still frames were checked through the Xvfb + lavapipe
route above — the map draws, the light gradient falls off correctly around a lit
doorway, and the debug overlay is legible — but nothing animated was: sway,
particles, and the hit reaction have only ever been verified as numbers. First
job for anyone with a real window is to watch those three and tune
`sway_amount`, `sway_speed` and the particle `scale`/`lifetime` values in
`field_particles.gd`.

### 2. Replacing curses screens with Godot Controls

This is the path to "no rendering outside Godot", and it is per-screen work: about
87 files open a blocking `uilist`, and there are ~22 `cataimgui::window` subclasses.
The curses overlay exists to keep those playable until each is migrated, not as a
destination. Every fix to the overlay is scaffolding that gets deleted with the
screen it serves.

The blocker was never rendering -- it was that a Godot panel had no way to *act*.
`src/godot_game_commands.*` now provides that: commands queue from the Godot thread
and run on the game thread at its input wait. The inventory panel's wield/wear/drop
is the worked example; the pattern for any other screen is the same.

Order by how much curses each removes: inventory actions (done), then examine,
craft, options and keybindings.

New Godot panels should follow `nocturne.gd` rather than inventing their own
styling — the sidebar and inventory are the worked examples, both built from the
mockups in `Cataclysm-DDA menu redesign (2).zip`.

**Overlay invariant worth keeping.** ImGui menus live in their own snapshot layer
and are only rewritten when an ImGui frame renders. `ui_adaptor::redraw_invalidated()`
returns early with an empty UI stack, so when the last menu closes nothing would
retire the frame that drew it — the menu stays painted and `any_window_shown()`
stays true, swallowing input. `get_input_event()` renders one empty frame before
blocking to prevent that. Anything that changes when ImGui frames are produced
must preserve it. Equally: a blank curses cell claims the overlay only where the
window painted a background, or an erased full-screen window becomes an opaque
sheet over MapView that nothing ever releases.

### 3. Audio, and animation presentation

Audio is absent rather than half-done: no audio node at all, so the game is silent.
That is now the largest single missing subsystem.

Animations land as a glyph overlay (`godot_anim_snapshot.*`). Smoke and fire now
have `GPUParticles2D` (`field_particles.gd`), and melee has a per-instance
reaction rather than only a marker glyph. What is still open there: weather, and
explosions — which would look better as particles plus `WorldEnvironment` glow
than as expanding rings of `/-\|` characters. `field_particles.gd` is the
worked example; it pools emitters and takes the same wind vector the sway shader
uses.

### 4. Lighting polish / cleanup

The per-sprite tint is no longer the lighting model — `src/godot_light_snapshot.*`
is (ADR-003). What is left is tuning it against a real screen, and deciding
whether the tint should carry anything beyond the night-vision hue.

Still tracked in [`GODOT_MIGRATION_TASKS.md`](../GODOT_MIGRATION_TASKS.md) (T2.3+, T4.x, T5.x).
Prefer Godot shaders / nodes over CPU framebuffer compositing. Later 2.5D/3D =
swap MapView draw backend only.

## The dead frame boundary

Read this before adding anything that draws. It has cost us three features, and
the third was documented in this file as working for two days.

The migration moved *present* to Godot. MapView draws from a snapshot, the HUD is
a Godot panel, and nothing asks the main `ui_adaptor` to redraw. But several
producers were left wired to the **curses frame boundary** — they publish at the
end of `game::draw()`, or into `w_terrain`, or on a `ui_adaptor` redraw callback.
Under GODOT none of those run. The code still executes: it is computed, stepped
every turn, and then discarded before anything can show it.

Three instances so far:

| Feature | Wired to | Found by |
|---|---|---|
| Pixel minimap | never drawn by any Godot node | reading the call graph |
| Scrolling combat text | `w_terrain`, via `draw_sct()`'s `#else` | instrumenting the chain |
| The whole animation overlay | `commit_frame()` on the `w_terrain` refresh | `commits: 0` |

The measurement that made the third one legible, over a full session containing a
real fight:

    commits: 0   glyphs_added: 0   texts_added: 0   hits_added: 1   generation: 0

`hits_added: 1` is the control: that one path is called directly from game logic
rather than from a draw callback, so it works. Everything else on the overlay —
explosions, bullets, the aim line and cursor, trajectory highlights, hit markers —
has never reached a frame.

**Why our tooling missed it.** The headless probe asserts that snapshots are
*published*. It has never asserted that anything *consumes* them, and a generation
counter stuck at zero looks exactly like a quiet turn. "Computed and thrown away"
and "not computed at all" are indistinguishable from outside, and both look like
"no bug" to a probe that only reads the producer side.

So, two rules:

1. **A new producer is not done until something has drawn it.** Not "the snapshot
   is populated" — a screenshot, or a consumed-generation count that moves.
2. **Suspect anything hanging off a `draw()` callback or a `catacurses::window`.**
   If a feature has never been seen on screen, assume it is on a dead path before
   assuming it is subtly wrong.

Found jointly while migrating the menus and repairing the animation overlay.

## Constraints (do not violate)

- Do **not** change `catacurses` public API in `cursesdef.h`.
- Do **not** touch protected game-logic files unless a task explicitly requires it.
- Keep `TILES` (SDL) and curses builds working; GODOT is additive until cleanup.
- Use `make GODOT=1`. The CMake `GODOT=ON` target is unsupported.
- Commit prefix: `[godot-mig] …` on `godot-mig`; push to fork remote `juan` only when asked.

## Suggested first agent session

1. `./build-scripts/compose-tileset.sh` if `gfx/UltimateCataclysm/` is missing,
   then `make GODOT=1 -j…` and run Godot — Play Now shows Ultica + HUD;
   `i` inventory, `@` character, `F3` render debug overlay.
2. **Look at the map.** The sprite pipeline (SP-1…SP-10) was built without a
   display; the first pass with a window should tune the light, memory and sway
   constants rather than add anything.
3. Migrate another curses screen to a Godot Control using
   `src/godot_game_commands.*`. MENU-4 (the uilist callback contract) unlocks 26
   screens at once and is the highest-leverage one left.
4. Extend MapView: character overlays, z-levels.
5. Update this file + task list when a gap closes.
