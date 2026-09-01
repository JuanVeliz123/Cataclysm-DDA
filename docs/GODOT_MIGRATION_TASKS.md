# Cataclysm-DDA → Godot Rendering Migration: Sub-Task Breakdown

**Target engine:** Godot **4.7.1-stable** (latest stable release, Jul 14 2026)
**GDExtension bindings:** godot-cpp — use tag `godot-4.5-stable` (latest stable 4.x tag; a newer decoupled `10.0.0-rc1` also exists). GDExtension is backwards-compatible across all Godot 4.x, so godot-cpp built against the 4.5 API loads in 4.7.1.
**Branch:** `godot-mig`
**Remotes:** `origin` = CleverRaven/Cataclysm-DDA (upstream), `juan` = JuanVeliz123/Cataclysm-DDA (fork)
**Reference doc:** `docs/RENDERING_AND_GODOT_MIGRATION.md` — architecture summary; keep it updated as work lands.

> **How to use this file:** each task is written to be executed by a sub-agent. Work is strictly ordered: a task may start only when its `Dependencies` are done. A task is done only when its `Definition of Done` is met and `Verification` passes. Commit after each task on `godot-mig` with prefix `[godot-mig]`; push to `juan` only when instructed. Do NOT modify the protected game-logic files beyond what a task explicitly requires.

> **Architecture note (2026-08-13):** ADR-002 present = **tileset MapView** (C++ draw-list + UltimateCataclysm → `map_view.gd`). TerminalView / CPU framebuffer are debug-only. Prefer Godot draw/nodes over finishing `GodotCataTiles` CPU blitting. See `docs/godot_migration/architecture_adr.md`.

---

## Phase 1b — Native Godot present (ADR-002)

### T1b.1 — Cell snapshot + TerminalView (DONE 2026-08-13; demoted to debug)
- **Goal:** Godot draws in-session ASCII/UI from a C++ cell grid instead of uploading the CPU framebuffer.
- **Delivered:** `src/godot_view_snapshot.*`, `CDDAHost.get_view_*`, `godot/scripts/terminal_view.gd`; host gates via `USE_TERMINAL_DEBUG` (default false).
- **Note:** Superseded for product present by T1b.3 MapView.

### T1b.2 — TerminalView polish (LOW PRIORITY / debug only)
- **Goal:** acceptable frame cost + ImTui/query prompts if TerminalView debug is re-enabled.
- **Dependencies:** T1b.1. Prefer migrating prompts to Godot Controls instead.

### T1b.3 — Map-layer snapshot → Godot tiles (DONE 2026-08-13)
- **Goal:** expose visible map layers to Godot; paint sprites (not CPU sprite blit).
- **Delivered:** `src/godot_map_snapshot.*`, `ensure_tileset_loaded("UltimateCataclysm")`, `CDDAHost` atlas/draw-list APIs, `godot/scripts/map_view.gd`, host session → MapView (`USE_TERMINAL_DEBUG` / `USE_LEGACY_FRAMEBUFFER_VIEW` false). Layers v1: terrain bg/fg, furniture, trap, item, monster, player. NORMAL atlas only.
- **Tileset data:** composed `gfx/UltimateCataclysm/` (release zip); optional source symlink `gfx/UltimateCataclysm.src`.
- **Verification:** Play Now / Custom confirm show Ultica terrain (and furniture/player) in MapView; `make GODOT=1` builds.
- **Dependencies:** T1b.1. Supersedes the long-term intent of T2.2 CPU blitting.

### T1b.4 — MapView fidelity (PARTIAL — see ADR-003)
- **Goal:** fields, vehicles, overlays; lighting; memory tiles.
- **Delivered:** integer-snapped auto-fit zoom + Ctrl+wheel user zoom and
  viewport-resize camera (`map_view.gd`, commit `ac9a913`). Then 2026-08-14:
  batched `MultiMeshInstance2D` draw, per-sprite lighting tint from `lit_level`,
  map-memory tiles for anything out of sight, field and vehicle layers, whole
  item stacks, and SDL-matching sprite-variation seeds.
- **Retargeted 2026-08-14:** lighting is a GPU modulation derived from
  `lit_level`, *not* the SDL-era pre-tinted night/memory atlases. Currently
  per-sprite (via the MultiMesh instance colour); a smooth per-pixel light
  texture is the next step. See ADR-003.
- **Still open:** multitile / rotation / auto-join (walls and roads render
  unconnected), character overlays (worn / mutations / bionics), z-levels,
  overmap as a Godot node.
- **Dependencies:** T1b.3.

### T1b.7 — Curses UI overlay (DONE 2026-08-13)
- **Goal:** let un-migrated catacurses/ImGui screens reach the player on top of MapView.
- **Delivered:** `USE_CURSES_UI_OVERLAY` in `host.gd`, transparent-cell overlay in
  `terminal_view.gd`, `blit_imtui_screen` in `godot_curses_backend.cpp`, and a
  CDDA patch to `imtui-impl-text.cpp` so ImGui glyphs survive a 1px font atlas.
- **Note:** this is what currently carries eat / craft / examine / options /
  keybindings / debug menu / overmap — everything not yet a Godot Control.

### T1b.5 — In-game HUD / inventory / character (DONE 2026-08-13)
- **Goal:** Godot Controls for session HUD, inventory browse, character sheet.
- **Delivered:** `src/godot_hud_snapshot.*`, `CDDAHost.get_hud_state` / `get_character_sheet` / `get_inventory_state`, `hud_panel.gd`, `inventory_panel.gd`, `character_panel.gd`. Host intercepts `i` / `@` so curses inventory/player-info do not open.
- **Verification:** During a session, sidebar shows HP/stamina/needs/messages; `i` opens inventory overlay; `@` opens character overlay; Esc closes.
- **Dependencies:** T1b.3.

### T1b.6 — Inventory actions (NEXT)
- **Goal:** wear / drop / use / eat from Godot inventory via game-thread commands.
- **Dependencies:** T1b.5.

---

## Global Constraints (apply to every task)

- **Protected files (game logic — never touch unless a task explicitly requires it):** `src/game.cpp`, `src/map.cpp`, `src/do_turn.cpp`, `src/output.cpp`, `src/input.cpp`, `src/input_context.cpp`, `src/ui_manager.cpp`, `src/animation.cpp`, `src/overmap_ui.cpp`, `src/main.cpp`, `data/`, `gfx/`.
- The `catacurses` public API in `src/cursesdef.h` is the contract — **it must not change**. Only backend implementations change.
- Both renderer modes must keep working in the new build until SDL is deleted: **ASCII/curses** (cell buffers) and **tiles** (sprites). Tiles mode may land after ASCII mode.
- New code follows repo conventions: 4-space indent, `astyle` format (`.astylerc` exists), no gratuitous comments, no `using namespace std`.
- Every task adds/uses the `-DGODOT` compile definition so the build can be toggled; SDL code stays compilable until the final cleanup task.
- **Supported build:** `make GODOT=1` on macOS, Linux (`NATIVE=linux64`) and Windows (`CROSS=x86_64-w64-mingw32-`). **CMake `GODOT=ON` is unsupported** — it exists but is not maintained or verified. Build the bindings first with `build-scripts/get-godot-cpp.sh`. See `doc/c++/COMPILING-GODOT.md`.
- **Portability rule:** keep platform knowledge in the build system. No `src/godot_*` file currently contains a platform `#ifdef` and it should stay that way — if you need one, ask whether Godot already abstracts it.
- Keep `docs/godot_migration/` for per-task notes and agent handoff.

---

## Phase 0 — Environment & Build Scaffolding

### T0.1 — Baseline build verification
- **Goal:** prove the repo builds with `TILES=ON` (SDL2 and/or SDL3) on `godot-mig` before any change.
- **Steps:**
  1. Configure + build `TILES=ON` (`cmake -B build-tiles -DTILES=ON`, plus `-DUSE_SDL3=ON` if deps allow).
  2. Record exact CMake options, compiler, SDL version. Run briefly; confirm the main menu renders.
  3. Also build `CURSES=ON` (TUI) as the second baseline.
- **Definition of done:** both builds succeed; notes in `docs/godot_migration/phase0_build_baseline.md`.
- **Verification:** `cmake --build build-tiles` exits 0.

### T0.2 — Godot engine + godot-cpp toolchain install
- **Goal:** working Godot 4.7.1-stable binary and godot-cpp (tag `godot-4.5-stable`) on the dev machine (macOS here).
- **Steps:**
  1. Download Godot 4.7.1-stable from `https://godotengine.org/download/` (or GitHub releases). Verify `godot --version` → `4.7.1.stable.official`.
  2. Clone `https://github.com/godotengine/godot-cpp` at tag `godot-4.5-stable`.
  3. Build godot-cpp with the same compiler/toolchain as the CDDA build (`cmake -S godot-cpp -B godot-cpp/build`). Verify it produces `libgodot-cpp.*`.
- **Definition of done:** `godot --version` works and `godot-cpp/build/bin/libgodot-cpp.*` exists.
- **Verification:** T0.3's smoke project loads.

### T0.3 — Minimal GDExtension smoke test inside the repo
- **Goal:** prove the CDDA build system can produce a Godot library and that a runnable Godot project lives in the repo.
- **Steps:**
  1. Create `godot/`: `project.godot`, `scenes/`, `extensions/`.
  2. In `godot/extensions/`, create a GDExtension entry (`godot_ext.cpp`) exposing a `Node` subclass, wired to a `.gdextension` file with `compatibility_minimum="4.5"`.
  3. Add CMake option `GODOT` (default OFF) in `CMakeLists.txt` beside `TILES` (line ~3), and a target `cataclysm-godot` in `src/CMakeLists.txt` compiling `godot_ext.cpp` and linking godot-cpp.
  4. Make `godot/` runnable: `godot --path godot --headless --quit` exits 0; editor shows the smoke node.
- **Definition of done:** `cmake -B build-godot -DGODOT=ON` builds; `godot --path godot` runs the test node without errors.
- **Verification:** `godot --path godot --headless --quit` exits 0.
- **Dependencies:** T0.2.

### T0.4 — Migration spike: choose the integration architecture
- **Goal:** write an ADR in `docs/godot_migration/architecture_adr.md` committing to ONE of:
  - **(A) Render-to-texture host:** CDDA keeps its own loop on a thread and produces a final image buffer each frame; a Godot `TextureRect`/`Sprite2D` presents it. Smallest diff, least Godot benefit.
  - **(B) Full GDExtension embed:** the game is compiled as a Godot extension; `catacurses` windows are implemented as Godot nodes/controls; the scene tree owns the frame.
- **Steps:** prototype both against a small slice of `cursesport` output in `godot/`. Document the choice, the threading model, frame pacing (`_process` vs `_physics_process`, `process_mode`), and how `catacurses::doupdate()` triggers a Godot repaint.
- **Definition of done:** ADR written with a clear decision, threading diagram, and frame-pacing approach.
- **Dependencies:** T0.1, T0.3.

---

## Phase 1 — The `catacurses` Backend on Godot (ASCII mode)

### T1.1 — Godot font/glyph engine (replaces `src/sdl_font.cpp`)
- **Input files:** `src/sdl_font.cpp`, `src/sdl_font.h`, `src/font_loader.h`.
- **Goal:** a `godot_font.{h,cpp}` module mapping a UTF-8 codepoint + FG/BG color to glyph geometry via Godot `TextServer`/`FontFile` instead of SDL_ttf.
- **Steps:**
  1. Mirror the `Font` / `FontFallbackList` / `CachedTTFFont` / `BitmapFont` structure (`sdl_font.h:28-175`) as `godot_font.{h,cpp}` with the same interface (width/height, `OutputChar`, `draw_ascii_lines`).
  2. Implement glyph lookup with `FontFile::get_char_size`/`TextServer::font_draw_glyph`; cache per-glyph textures like `glyph_cache_map` (`sdl_font.cpp:363`).
  3. Implement `draw_ascii_lines` for box-drawing characters using the geometry renderer (T1.3).
  4. Wire the four font instances (`font`, `gui_font`, `map_font`, `overmap_font`) from `font_loader` config into the Godot build.
- **Definition of done:** ASCII text renders correctly through the new font module in the smoke scene; glyph cache invalidates on renderer recreate.
- **Verification:** headless render of a sample window shows correct glyphs/colors.
- **Dependencies:** T0.4.

### T1.2 — Godot cell-buffer backend (replaces `src/cursesport.cpp` rendering half)
- **Input files:** `src/cursesport.cpp`, `src/cursesport.h`, `src/cursesdef.h`.
- **Goal:** keep the `cata_cursesport::WINDOW` cell buffers EXACTLY as-is (they are engine-agnostic) but implement the drawing path (`curses_drawwindow`, `clear_window_area`) with Godot primitives. SDL's `WINDOW` struct and cell write functions are unchanged — only `curses_drawwindow` and `clear_window_area` move behind a `#if defined(GODOT)` branch.
- **Steps:**
  1. Extract `curses_drawwindow`'s current body (`sdltiles.cpp:3644-3800`) and split it: game-side `wnoutrefresh`/`doupdate` logic stays in `cursesport.cpp`; the actual pixel drawing becomes a `godot_curses_backend` module.
  2. Implement `draw_window(win, font, pos, force_full)` equivalent: iterate `curseline`/`cursecell`, call `godot_font::OutputChar` per cell, honoring the `touched` + epoch optimization.
  3. Implement `clear_window_area` and `handle_resize`/`resize_term`/`get_scaling_factor` equivalents.
- **Definition of done:** `catacurses` windows (menus, sidebar, dialogs) render via Godot in ASCII mode; dirty-line optimization still works.
- **Verification:** game main menu renders in the Godot build in ASCII mode; resize works.
- **Dependencies:** T1.1.

### T1.3 — Godot geometry renderer (replaces `src/sdl_geometry.cpp`)
- **Input files:** `src/sdl_geometry.cpp`, `src/sdl_geometry.h`.
- **Goal:** a `godot_geometry.{h,cpp}` implementing the `GeometryRenderer` interface (`rect`, `horizontal_line`, `vertical_line`) with Godot `CanvasItem::draw_rect`/`draw_line` (or `_draw` on a Node2D/Control).
- **Steps:**
  1. Implement the three methods; keep the same signature so `cata_tiles`/`draw_window` callers are unchanged.
  2. Handle color modulation fallback the way `ColorModulatedGeometryRenderer` does.
- **Definition of done:** rects, lines, and window backgrounds render correctly in ASCII mode and in the minimap.
- **Verification:** visual check of UI borders/backgrounds in the Godot build.
- **Dependencies:** T1.2.

### T1.4 — Godot window/terminal lifecycle (replaces `WinCreate`/`WinDestroy`/`refresh_display` in `src/sdltiles.cpp`)
- **Input files:** `src/sdltiles.cpp:591-1064` (WinCreate, SetupRenderTarget, refresh_display, apply_resize_layout).
- **Goal:** a `godot_display.{h,cpp}` owning the Godot `Window`/`SubViewport`/`ViewportTexture` and the per-frame present path.
- **Steps:**
  1. Replace `display_buffer` (SDL offscreen texture) with a Godot `SubViewport` + `ViewportTexture` (or the ADR's chosen alternative).
  2. Implement `refresh_display()` equivalent: present the sub-viewport texture, integer scaling (`get_display_buffer_render_rect` equivalent, `sdltiles.cpp:311`), and `scaling_factor` handling.
  3. Implement resize/fullscreen (the old `renderer_resource_coordinator` recovery machinery in `sdltiles.cpp:1383-2829` is DELETED — Godot owns renderer lifecycle; keep only the hooks that bump `curses_render_epoch` on recreate).
  4. Wire `catacurses::init_interface()` (`sdltiles.cpp:6256`) and `catacurses::endwin()` to the new module for the Godot build.
- **Definition of done:** window creation, fullscreen toggle, resize, and present all work via Godot; the ~1,400-line SDL recovery state machine is gone from the Godot path.
- **Verification:** full game session in ASCII mode with window resize and fullscreen toggle.
- **Dependencies:** T1.2.

### T1.5 — Display-buffer scope & clipping (replaces `display_buffer_draw_scope` in `src/sdltiles.h:134-162`)
- **Input files:** `src/sdltiles.h:89-162`, `src/sdltiles.cpp:1274-1359`.
- **Goal:** provide an equivalent RAII draw-scope in the Godot module (bind/unbind sub-viewport target, abort on failure, `should_draw()`).
- **Steps:**
  1. Implement `godot_draw_scope` with the same semantics (outermost-only bind, `abort_unbind`).
  2. Implement clip-rect handling per window (`cata_tiles::draw` clip setup) with Godot `draw_set_transform`/clip.
- **Definition of done:** nested draw scopes and clipping behave as before; draw code in `cata_tiles` port compiles unchanged.
- **Dependencies:** T1.4.

---

## Phase 2 — Tiles Rendering on Godot

### T2.1 — Tileset/atlas loading (replaces the texture-upload half of `src/tileset_loader.cpp`)
- **Input files:** `src/tileset_loader.cpp`, `src/tileset_loader.h`, `src/cata_tiles.h:283-538` (`tileset`, `tileset_cache`).
- **Goal:** keep all JSON parsing (`tile_config.json`, tile ids, `tile_type` records, subtiles, weighted lists) but replace `SDL_Surface`/`SDL_Texture` upload with Godot `Image`/`Texture2D`.
- **Steps:**
  1. Replace atlas creation (`IMG_Load` → `CreateTextureFromSurface`) with `Image::load_from_file` → `ImageTexture::create_from_image`.
  2. Keep the six parallel atlas arrays (`tile_values`, `shadow_`, `night_`, `overexposed_`, `memory_`, `silhouette_`) — they map directly to Godot textures.
  3. Rework `atlas_descriptors`/replay logic for renderer rebuild into Godot's texture lifetime (mostly disappears).
- **Definition of done:** a tileset loads into Godot textures; `texture::render_copy_ex` equivalent draws correct sprite rects.
- **Verification:** load the ASCII tileset and the default tileset in the Godot build.
- **Dependencies:** T1.5.

### T2.2 — `cata_tiles::draw` core map pipeline (replaces the SDL half of `src/cata_tiles.cpp`)
- **Input files:** `src/cata_tiles.cpp:561-1405` (draw point cache, layer loop `cata_tiles.cpp:998-1001`, `draw_from_id_string_internal` `:2174-2630`), `src/cata_tiles.h:567-1067`.
- **Goal:** port the map draw pipeline to Godot drawing primitives, preserving the layer order and lighting dispatch.
- **Steps:**
  1. Port `draw_sprite_at`/`draw_tile_at` to draw the fg/bg sprite lists via Godot `draw_texture_rect_region` (or `TextureRect` nodes per the ADR).
  2. Port the 11-layer draw loop (`draw_terrain` → `draw_zombie_revival_indicators`, `cata_tiles.cpp:998-1001`) to Godot z-ordering or sequential draw calls; preserve `height_3d` vertical offsets.
  3. Port the lighting dispatch (`lit_level` → texture variant: normal/shadow/night/overexposed/memory) — as texture swap first, shader later (T2.3).
  4. Port coordinate math (`tile_to_player`, `player_to_screen`, `screen_to_player`, `set_draw_scale`, zoom).
- **Definition of done:** the map renders with correct terrain/furniture/creatures/items in tiles mode, with lighting variants.
- **Verification:** in-game movement shows correct tiles at all zoom levels.
- **Dependencies:** T2.1, T1.3.

### T2.3 — Lighting/vision shaders (replaces `src/cata_shader.h` SDL3 variant_pass and multi-atlas sampling)
- **Input files:** `src/cata_shader.h`, `data/shaders/`.
- **Goal:** reimplement the `variant_kind` pipeline (NORMAL/SHADOW/NIGHT/OVEREXPOSED/MEMORY, `cata_shader.h:38`) as Godot canvas shaders (`CanvasItem` material) or `CanvasModulate`/`Light2D` where fitting; keep the memory-map shader path.
- **Steps:**
  1. Convert each SDL variant fragment to Godot shader (`.gdshader`); hook selection into the texture-dispatch from T2.2.
  2. Implement `select_memory_preset` equivalent.
- **Definition of done:** night vision, low light, overexposure, and memory-map rendering visually match SDL output.
- **Verification:** toggle NV goggles, light conditions, and memory map in-game; compare to SDL build screenshots.
- **Dependencies:** T2.2.

### T2.4 — Overmap rendering (replaces `cata_tiles::draw_om` in `src/sdltiles.cpp:2994-3230`)
- **Input files:** `src/sdltiles.cpp:2994-3230`, `src/overmap_ui.cpp:1321-1334`.
- **Goal:** port `draw_om` (overmap terrain, hordes, highlights, notes, mission arrows) to Godot.
- **Steps:**
  1. Port the grid iteration + `draw_from_id_string(OVERMAP_TERRAIN/OVERMAP_VISION_LEVEL)` calls.
  2. Port the overlay layers (hordes, notes, vehicle icons, fast-travel path, arrows).
- **Definition of done:** overmap renders in tiles mode with overlays.
- **Verification:** open overmap in the Godot build; visuals match SDL.
- **Dependencies:** T2.2.

### T2.5 — Pixel minimap (replaces `src/pixel_minimap.cpp`)
- **Input files:** `src/pixel_minimap.cpp`, `src/pixel_minimap.h`.
- **Goal:** reimplement the submap texture cache/compositor as a Godot `SubViewport` or texture-cached `TextureRect` draw.
- **Steps:**
  1. Port `process_cache`/`render_cache`/`render_critters`/`draw_beacon` (`pixel_minimap.cpp:603` area) to Godot draw calls; keep the submap cache data structures.
  2. Wire into `curses_drawwindow`'s `w_pixel_minimap` branch.
- **Definition of done:** minimap shows explored map, critters, and beacon in the Godot build.
- **Verification:** minimap visually matches SDL build.
- **Dependencies:** T2.2, T1.3.

### T2.6 — Animations & overlay effects (replaces the tiles half of `src/animation.cpp`)
- **Input files:** `src/animation.cpp` (all `*_tiles` paths: `init_explosion`, `init_draw_bullet`, `init_draw_weather`, `init_draw_sct`, `init_draw_async_anim`, zones, cursors), `src/game.cpp:3363` (callback storage).
- **Goal:** reimplement the tile-path animation overlays as Godot tweens/particles/draw callbacks, preserving `game::draw_callback_t` storage so `game.cpp` is untouched.
- **Steps:**
  1. Port `init_*` + per-frame draw methods onto the `cata_tiles` port; use Godot `Tween`/`particles` where timing-based.
  2. Ensure the draw callback list in `game::draw` (`game.cpp:3363`) still drives them.
- **Definition of done:** explosions, bullets, hits, weather, damage SCT, zones, and blinking cursors animate correctly.
- **Verification:** in-game effects match SDL visuals.
- **Dependencies:** T2.2.

### T2.7 — Color blocks & overlay strings (replaces `color_block_overlay_container` handling in `sdltiles.cpp:3661-3717`)
- **Goal:** port the terrain-window color-block overlays and text-overlay strings (drawn with `map_font` on top of tiles) to the Godot draw path.
- **Steps:** replicate `sdltiles.cpp:3677-3717` (blend mode save/set for blocks; alignment handling for centered/right strings) with Godot draw calls.
- **Definition of done:** zone highlights and floating text-over-tiles render identically.
- **Dependencies:** T2.2.

---

## Phase 3 — Input Migration

### T3.1 — Godot input → `input_event` bridge (replaces `CheckMessages`/`sdl_keysym_to_keycode_evt` in `src/sdltiles.cpp:4138-4180, 5180-6114`)
- **Input files:** `src/sdltiles.cpp:5180-6114` (CheckMessages, event handling), `src/input.h`, `src/input.cpp` (the `input_event` struct), `src/sdlsound.cpp` (unrelated, ignore).
- **Goal:** a `godot_input.{h,cpp}` that converts Godot `InputEvent` (key/mouse/wheel/gamepad/touch) into CDDA `input_event`s, delivered through the existing `input_manager::get_input_event()` seam without touching `input.cpp`.
- **Steps:**
  1. Implement the `input_event` conversion (keycode tables equivalent to `sdl_keysym_to_keycode_evt`).
  2. Implement `get_input_event()` equivalent honoring the existing `inputdelay` semantics (blocking/timeout).
  3. Map keyboard/mouse/wheel/gamepad/Android touch & virtual joystick; reimplement IME/text input against Godot's `TextServer`.
- **Definition of done:** full keyboard + mouse navigation works in the Godot build; input delay options honored.
- **Verification:** play a short session using movement, menus, mouse look, and text entry (rename char).
- **Dependencies:** T1.4 (window exists).

---

## Phase 4 — Sound, Screenshot, Debug UI

### T4.1 — Sound backend (optional; keep SDL_mixer if simplest)
- **Input files:** `src/sdlsound.cpp`, `src/sound_backend_sdl2.cpp`, `src/sound_backend_sdl3.cpp`.
- **Goal:** decide and implement: either keep SDL_mixer linked only for audio (SDK scope note — SDL removed from rendering only), or port to Godot `AudioStreamPlayer`/`AudioServer`.
- **Steps:** if porting, reimplement the `sound::sound_thread`/`sfx` dispatch (`sdlsound.cpp`) with Godot audio; else document the decision to keep SDL_mixer for audio only.
- **Definition of done:** either sound works via Godot, or the decision + rationale is recorded in the ADR.
- **Dependencies:** T0.4.

### T4.2 — Screenshot & misc utilities (replaces `save_screenshot` in `src/sdltiles.cpp`)
- **Goal:** reimplement `save_screenshot` using `Viewport::get_texture()->get_image()->save_png`; verify fullscreen toggle (already in T1.4).
- **Definition of done:** `S` key screenshot saves a correct PNG.
- **Dependencies:** T1.4.

### T4.3 — Debug/ImGui overlay (replaces `src/third-party/imgui/` + `imgui_impl_sdl*`)
- **Input files:** `src/third-party/imgui/`, `src/cata_imgui.cpp` (if present).
- **Goal:** either port the ImGui SDL backends to Godot (imgui_impl_godot exists in the ecosystem) or gate debug UI off in the Godot build initially; record decision.
- **Definition of done:** debug windows render (or are cleanly disabled) in the Godot build.
- **Dependencies:** T1.4.

---

## Phase 5 — Cleanup, Docs, CI

### T5.1 — Remove the SDL backend from the Godot build
- **Goal:** in the `GODOT` build, exclude SDL sources (`sdltiles.cpp`, `sdl_font.cpp`, `sdl_geometry.cpp`, `sdl_wrappers.cpp`, `sdl_utils.cpp`, `sdl_gamepad.cpp`, `pixel_minimap.cpp`, `tileset_loader.cpp` SDL halves, `sdlsound.cpp` if ported) from compilation; keep them only for the legacy `TILES` build.
- **Steps:** update `src/CMakeLists.txt` source lists and `#if` guards; verify nothing SDL is referenced by the Godot build.
- **Definition of done:** `-DGODOT` build compiles with zero SDL symbols; `-DTILES` build still compiles unchanged.
- **Dependencies:** T2.x, T3.x, T4.x all done.

### T5.2 — Build docs & user-facing docs
- **Goal:** document the Godot build (options, toolchain, godot-cpp tag) in `COMPILING.md`-style notes; update `docs/RENDERING_AND_GODOT_MIGRATION.md` and this file's status checkboxes; write `docs/godot_migration/` summaries per phase.
- **Definition of done:** a fresh checkout on `godot-mig` can build the Godot target from the docs alone.

### T5.3 — CI + regression baseline
- **Goal:** add a CI job (GitHub Actions) for the Godot build on a runner with Godot 4.7.1; run the existing unit-test suite (`make tests`/ctest) on the Godot build; record screenshot-diff baseline between SDL and Godot builds for a fixed seed.
- **Definition of done:** CI green; tests pass; baseline screenshots committed to `docs/godot_migration/baseline/`.
- **Dependencies:** T5.1.

### T5.4 — Performance profiling & frame pacing
- **Goal:** profile the Godot build (draw calls, glyph cache, sub-viewport blit, tiles draw). Compare FPS with the SDL build; tune glyph caching and the tiles draw path.
- **Definition of done:** performance parity (or documented, accepted delta) with SDL build in a standard scene.
- **Dependencies:** T5.1.

---

## Task Dependency Graph

```
T0.1 ─┐
T0.2 ─┴→ T0.3 → T0.4 ─┬→ T1.1 → T1.2 → T1.3 → T1.4 → T1.5
                      │                             │
                      └→ (ADR decision)             ↓
                                              T2.1 → T2.2 → T2.3 ─┬→ T2.4
                                                  │                ├→ T2.5
                                                  ↓                └→ T2.6 → T2.7
                                              T3.1 (after T1.4)
                                              T4.1/T4.2/T4.3 (after T1.4)
                                              T5.1 → T5.2 → T5.3 → T5.4
```

## Status Tracking

| Task | Status | Agent | Notes |
|------|--------|-------|-------|
| T0.1 | ☑ | Kilo | Completed: Download Godot 4.7.1-stable toolchain |
| T0.2 | ☑ | Kilo | Completed: Clone & build godot-cpp GDExtension bindings |
| T0.3 | ☑ | Kilo | Completed: Create godot/ scaffold + GDExtension boilerplate + factories |
| T0.4 | ☑ | Kilo | Completed: Write render-to-texture host architecture ADR |
| T1.1 | ☑ | Kilo | Completed: GDExtension font rasterizer using TextServer & FontFile |
| T1.2 | ☑ | Kilo | Completed: Curses backend bridge mapping windows to framebuffer |
| T1.3 | ☑ | Kilo | Completed: Geometry renderer drawing shape primitives |
| T1.4 | ☑ | Kilo | Completed: Display presenter with frame-ready signal callback |
| T1.5 | ☑ | Kilo | Completed: Cursesport integration and color palette loaders |
| T2.1 | ☑ | Kilo | Completed: Godot tileset loader converting sheets to ImageTextures |
| T2.2 | ☑ partial | Kilo | Skeleton + id lookup land; **actual sprite blit / layer draw still TODO** (see AGENT_HANDOFF) |
| T2.3 | ☐ | | Superseded by ADR-003 B3: GPU light texture + canvas shader, not per-variant atlases |
| T2.4 | ☑ partial | | Implemented 2026-08-14 for real: `godot_overmap_snapshot.*` builds an OMT draw list from its own tileset (the `OVERMAP_TILES` option), `overmap_view.gd` batches it, and an RAII guard in `overmap_ui::display` tells the host when to show it. **Open:** connected-terrain subtiles and rotation, hordes, weather, vehicles, notes text, mission/fast-travel arrows. |
| T2.5 | ☑ | | Wired 2026-08-14: retargeted off the CPU framebuffer to a mutex-guarded RGBA snapshot, rendered per turn from `game_do_turn`, shown by `minimap_panel.gd`. |
| T2.6 | ☑ partial | | Implemented 2026-08-14 as a glyph overlay: `#elif defined(GODOT)` branches in `animation.cpp` publish map-positioned primitives to `godot_anim_snapshot.*`, drawn by `anim_overlay.gd` as a MapView child. Covers explosions, custom explosions, bullets, hit markers, aim line, trajectory, cursor and highlights. **Open:** weather and smoke as `GPUParticles2D`, SCT, and explosions as particles rather than ASCII rings. |
| T2.7 | ☐ | | |
| T3.1 | ☑ | Kilo | Completed: Input Bridge translating Godot InputEvents to CDDA event models |
| T3.2 | ☑ | Kilo | Completed: Global input `n()` function mapping thread-safe queue to CDDA core |
| T3.3 | ☐ | | **New.** Mouse `input_event`s carry Godot pixel coords while `input_context` reads them as cells (`pixel_to_cell` is an identity pass-through), and `InputEventMouseMotion` is never forwarded. Mouse input lands on the wrong cell. |
| T4.1 | ☐ | | No audio at all: `SDL_SOUND` undefined → `sounds.cpp` dummy `sfx::*` no-ops, and no Godot `AudioStreamPlayer` exists |
| T4.2 | ☐ | | |
| T4.3 | ☑ | | **Resolved 2026-08-14:** ImGui renders through the ImTui *text* backend into the cell grid and is composited by `blit_imtui_screen` → `TerminalView`. imgui-godot not needed. Fidelity (16-colour crush, no mouse) tracked separately. |
| T5.1 | ☑ | | Dead SDL C shims removed, and with them the whole CPU raster path: the `framebuffer`/`geometry`/`font` contracts, `godot_font.*`, `godot_geometry.*`, `godot_tiles_rendering.*`, `godot_animation.*`, `GodotDisplay::present`, the `get_framebuffer_*` API and the `GameView` node. |
| T5.2 | ☑ | | Cross-platform `make GODOT=1` (Linux / Windows-MinGW / macOS) + `doc/c++/COMPILING-GODOT.md` + `build-scripts/get-godot-cpp.sh`. CMake is explicitly unsupported. |
| T5.3 | ☐ | | Deliberately skipped: the maintainer builds on their own platforms, so no CI job. Local verification recipe is in `doc/c++/COMPILING-GODOT.md`. |
| T5.4 | ☐ | | |
| Build | ☑ | | Linux x86_64, Windows x86_64 (MinGW cross) and macOS all build; see ADR-003 / COMPILING-GODOT.md |

