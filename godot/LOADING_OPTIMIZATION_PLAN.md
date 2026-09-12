# Godot Migration — Loading Performance Plan

This document is split into **independent, parallelizable tasks**. Each task is
written to be picked up by a separate agent with no shared context beyond this
file. Tasks are ordered by effort/risk, not by dependency — most can be done in
parallel, but read the "Depends on" line of each.

## Context (read once)

The render is migrated to Godot; game logic stays in C++ and talks to Godot via
a GDExtension (`godot/extensions/godot_ext.cpp`, built into
`godot/bin/libcataclysm-godot.dylib`). Godot `host.gd` drives a C++ game thread
through `CDDAHost`.

Loading happens in two phases:

1. **Bootstrap** (cold start, once): `godot_ext.cpp:1808` runs
   `game_load_static_data()` (`godot_game_thread.cpp:176` → `game_io.cpp:106`).
   This is small (input config, auto-pickup/notes/safemode). Fast.
2. **Session start** (every New Game / Load Game): `godot_ext.cpp:1876-1893`
   calls `main_menu` (which loads core + mod JSON) and then
   `ensure_tileset_loaded("UltimateCataclysm")` at `:1885`. Both are fully
   serial today.

`host.gd` `_session_present_ready()` (`host.gd:424`) blocks the first frame on
`cdda_host.tileset_ready()` — so the tileset load is on the critical path of
every session start.

The flexbuffer JSON cache (`src/json_loader.cpp`) disk-caches parsed JSON for
`data/`, `config/`, etc., so *repeated* launches are faster, but save data is
explicitly NOT cached (`cache_for_save`, `json_loader.cpp:53-75`).

---

## TASK 1 — Hoist tileset load into bootstrap

**Goal:** Remove the first-frame stall by loading the tileset once during
bootstrap instead of on every session start.

**Why it's safe:** `ensure_tileset_loaded` already caches the result in the
module-level `g_tileset` (`godot_map_snapshot.cpp`), keyed by tileset id
(`UltimateCataclysm`). Session-start currently re-runs it but it short-circuits
to the same cached data. Moving it earlier just front-loads the work that
otherwise blocks the first frame.

**Files to change:**
- `godot/extensions/godot_ext.cpp` — after `game_load_static_data();`
  (`godot_ext.cpp:1825`), before `ready_ = true;` (`:1830`), call
  `get_map_snapshot().ensure_tileset_loaded("UltimateCataclysm");`.
  Wrap in try/catch so a tileset failure doesn't abort bootstrap — log and
  continue (the session start path already handles failure).
- `godot/scripts/host.gd` — `_session_present_ready()` (`host.gd:424`) already
  checks `tileset_ready()`. No change strictly needed, but confirm the splash
  still dismisses correctly when the tileset is ready before the session starts.

**Acceptance:**
- New Game and Load Game no longer show a stall before the first world frame.
- `ensure_tileset_loaded` is still idempotent (second call at session start
  returns early via the `ready_ && tileset_id_ == tileset_id` guard at
  `godot_map_snapshot.cpp:875`).
- Build runs (`make GODOT=1 -j$(sysctl -n hw.ncpu)`); no new console errors.

**Depends on:** none.

---

## TASK 2 — Parallelize atlas decode + texture upload

**Goal:** Decode atlas PNGs and build `ImageTexture`s across a thread pool
instead of serially.

**Where the time is:** `godot_tileset_loader.cpp`:
- `parse_atlases` (`:485`) decodes each atlas PNG to RGBA8 (`load_atlas_image`,
  `:355`) — I/O + decode, independent per atlas.
- `upload_atlases()` (`:586`, loop `:1247-1306`) builds an `ImageTexture` per
  atlas × per light-level filter (`make_atlas_texture`, `:391`). These are
  independent of each other.

**Approach:**
- Decode atlases concurrently (thread pool / `std::async`). Keep the *parse*
  pass that records descriptors (`atlas_descriptor`, `:811-828`) as-is.
- For the texture build loop, decode+filter each atlas on a worker, then
  `create_from_image` on the main thread (Godot `Ref<ImageTexture>` creation is
  main-thread-bound in many builds). If safe in your Godot build, also create on
  workers and collect.
- Preserve the existing correctness invariants: the synthetic `ITEM_HIGHLIGHT`
  slot (`:557-566`) and the unfiltered-atlas-only path (`:1260-1267`).

**Files to change:** `src/godot_tileset_loader.cpp` (`load`, `parse_atlases`,
`upload_atlases`, `make_atlas_texture`).

**Acceptance:**
- Visual output identical to serial path (compare a captured frame / sprite
  coverage report `get_sprite_coverage()`).
- No data races (run under ThreadSanitizer or assert `atlas_textures` writes are
  synchronized).
- Measurable wall-clock reduction on tileset load.

**Depends on:** none.

---

## TASK 3 — Parallelize core/mod JSON data loading

**Goal:** Fan out `DynamicDataLoader` mod loading across threads; merge +
`finalize_loaded_data()` once.

**Where the time is:** The dominant CDDA load cost. `DynamicDataLoader` has **no
threading** — `load_data_from_path` (`src/init.cpp:521`) runs every mod's JSON
one after another on a single thread. The JSON itself goes through the
flexbuffer cache (`src/json_loader.cpp`), so the *first* parse per mod is the
cost.

**Approach (requires care):**
- The loader is driven via `game::load_core_data()` (`game_io.cpp:218`) →
  `load_data_from_dir` (`game_io.cpp:227`) →
  `DynamicDataLoader::load_data_from_path`.
- Identify the unit of parallelism: a *mod directory* or a top-level JSON file
  group. Load each independently, then call `finalize_loaded_data()` once at the
  end (it has cross-mod dependencies today — e.g. recipes referencing items in
  other mods — so finalization must stay single-threaded and happen after all
  loads).
- Keep the existing `check_mod_data` validation flow (`game_io.cpp:127`) working;
  do not parallelize the *unload*/`delete_world` teardown, only the load.

**Files to change:** `src/init.cpp` (`DynamicDataLoader::load_data_from_path` and
the directory walk), `src/game_io.cpp` (`load_core_data`, `load_data_from_dir`),
`src/dynamic_data_loader.h` if shared state needs protecting.

**Acceptance:**
- Same loaded data as serial (assert `DynamicDataLoader::is_data_finalized()` and
  spot-check a dictionary size, e.g. `item_controller` count).
- No use-after-finalize or ordering bugs (the flexbuffer cache means repeated
  runs exercise this well).
- Build + a quick New Game succeeds.

**Depends on:** none. **Risk: high** — finalization ordering. Agent should keep
finalize single-threaded and only parallelize the parse stage.

---

## TASK 4 — Disk-cache parsed save data

**Goal:** Enable the flexbuffer cache for saves so repeated loads skip re-parse.

**Where:** `src/json_loader.cpp`, `cache_for_save` (`json_loader.cpp:53-75`).
The comment there says "no measurable need to persist flatbuffers for save data,
so just create a per-world 'cache' which parses but doesn't disk-cache."

**Approach:**
- Give `flexbuffer_cache` for saves a real on-disk path (e.g. under the world's
  save dir) instead of an empty `std::filesystem::path()`, so it writes the
  parsed flatbuffer like `data_cache()` / `config_cache()` do.
- Confirm `flexbuffer_cache` invalidates by file mtime/size (it should already,
  matching the other caches) so a changed save re-parses correctly.
- Watch total cache size per world; if unbounded, add a simple
  least-recently-written cleanup or cap.

**Files to change:** `src/json_loader.cpp` (`cache_for_save`), possibly
`src/flexbuffer_cache.h/.cpp` if the empty-path branch needs an opt-in flag.

**Acceptance:**
- After one load, a `cache/` dir appears under the world save path; second load
  reads from it (faster, confirm via timing or a debug counter).
- Editing/saving the game invalidates the cache (re-parse on next load).
- No stale data served after a save.

**Depends on:** none.

---

## TASK 5 — Parallelize `game::load` save entries

**Goal:** Read the independent save files concurrently; keep Finalizing last.

**Where:** `src/game_io.cpp:322-451`. The `vector<named_entry>` (master,
dimension, character, map_memory, diary, memorial, finalizing) runs strictly in
order.

**Approach:**
- Group the independent, file-backed entries (Master save `:335`, Dimension data
  `:342`, Character save `:349`, Map memory `:372`, Diary `:378`, Memorial
  `:384`) to read concurrently (thread pool / `std::async`). Each reads into its
  own buffer; the parse/deserialize callbacks run after the reads complete.
- "Finalizing" (`:396`) has ordering dependencies (recalc_sight_limits,
  reload_npcs, update_map) and must run strictly after the others.

**Files to change:** `src/game_io.cpp` (`game::load(const save_t&)`).

**Acceptance:**
- Loaded game identical to serial (savehash / quick playthrough sanity check).
- No concurrent mutation of shared state in the read phase.
- Load time reduced on spinning disks / large saves.

**Depends on:** none.

---

## Suggested execution order

- **Parallel-safe to run now, independently:** Task 1, Task 2, Task 4, Task 5,
  Task 3 (Task 3 is the highest-risk; isolate it).
- **Recommended first PRs:** Task 1 (removes the visible stall, smallest) + Task
  4 (trivial) + Task 2 (contained win). Task 3 is the architecture-level project.
