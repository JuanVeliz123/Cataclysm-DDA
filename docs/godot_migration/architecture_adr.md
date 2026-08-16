# ADR-001 / ADR-002 / ADR-003: Godot Integration Architecture

- **Status:** Accepted (ADR-002 supersedes present-path of ADR-001; ADR-003 refines the ADR-002 draw layer)
- **Dates:** 2026-08-10 (ADR-001), 2026-08-13 (ADR-002; tileset MapView amendment same day), 2026-08-14 (ADR-003)
- **Decision:** Native Godot present; C++ keeps game logic + catacurses buffers

## Context

Cataclysm-DDA renders through a self-implemented `catacurses` API (`src/cursesdef.h`)
with a cell-buffer implementation in `src/cursesport.cpp` and an SDL tiles backend in
`src/sdltiles.cpp` / `src/cata_tiles.cpp`. We are migrating rendering to Godot
4.7.1-stable (GDExtension, godot-cpp tag `godot-4.5-stable`).

Two integration shapes were evaluated originally:

- **(A) Render-to-texture host:** CDDA writes a CPU RGBA framebuffer; Godot uploads it
  to a `TextureRect` each frame.
- **(B) Full GDExtension embed:** catacurses windows / map become Godot nodes; the
  scene tree owns present.

ADR-001 chose **(A)** for minimal diff. In practice the CPU framebuffer present path
has been fragile (gray/empty `GameView`, no-op `refresh_display`, font/thread issues),
and it does not move us toward “logic in C++, rendering in Godot.”

## Decision (ADR-002 — 2026-08-13)

**Target architecture:** Godot owns all present/rendering. C++ owns simulation,
`catacurses` cell buffers, and (for now) the game loop on a worker thread.

### End state

- C++: game logic, world/map state, input consumption, sprite id resolution
  (`looks_like` / tileset), UI *content* (catacurses or future view models).
- Godot: window, **tileset MapView** (and later 2.5D/3D backends swapping only
  the draw layer), lighting shaders, HUD chrome, audio.
- No gameplay dependence on CPU framebuffer → `TextureRect`, nor on TerminalView
  as the product present path.

### Incremental path (current slice — tileset MapView)

1. **Tileset load** (`src/godot_tileset_loader.*` + `godot_backend::ensure_tileset_loaded`):
   default **UltimateCataclysm** under `gfx/` (release compose or symlink from
   `../CDDA-Tilesets`). Export atlas RGBA once for Godot `ImageTexture`s.
2. **Map draw-list snapshot** (`src/godot_map_snapshot.*`): after each turn,
   resolve visible cells to packed commands
   `{atlas,src_x,src_y,src_w,src_h,dest_x,dest_y,layer}` (terrain → furniture →
   trap → item → monster → player). Exposed on `CDDAHost`.
3. **`MapView`** (`godot/scripts/map_view.gd`): `Node2D` paints with
   `draw_texture_rect_region`. Host session present shows MapView;
   `USE_TERMINAL_DEBUG` / `USE_LEGACY_FRAMEBUFFER_VIEW` default **false**.
4. **Deprecated bridge:** `TerminalView` + `godot_view_snapshot` kept for optional
   debug only. Do **not** finish `godot_tiles_rendering.cpp` CPU blit as the
   long-term path.
5. **HUD / in-game menus (this slice):** `src/godot_hud_snapshot.*` + Godot
   Controls (`hud_panel.gd`, `inventory_panel.gd`, `character_panel.gd`). `i` /
   `@` open Godot overlays and are not forwarded into curses. Inventory v1 is
   browse-only.
6. **Next slices:** inventory actions (wear/drop/use); remaining overlays
   (craft, examine, messages full); night/memory atlases; later replace MapView
   draw backend with iso/mesh (2.5D/3D) without rewriting game logic.

### Still from ADR-001 (kept)

- `catacurses` public API in `cursesdef.h` unchanged.
- Game loop remains on the CDDA worker thread (command queue from Godot).
- SDL draw/present stays out of the GODOT build.
- Hybrid pre-game UI (splash / main menu / load / custom chargen) stays Godot Controls.

## Amendment (2026-08-12): Hybrid pre-game UI

Pre-game chrome (boot splash, main menu, load-game picker) is **Godot Controls**.
The CDDA game thread bootstraps (`load_static_data`) without
`main_menu::opening_screen()`, then waits for Godot commands (`request_new_game`,
`request_load_game`, chargen APIs).

Custom chargen (world pick + seven tabs) is Godot Controls; mutations run on the
game thread via host APIs.

## Threading model

- **CDDA game thread:** bootstrap, chargen mutations, `do_turn`, tileset load,
  `MapSnapshot::update_from_game` / `HudSnapshot::update_from_game`.
- **Godot main thread:** pre-game Controls, `MapView` + HUD overlays, input →
  `GodotInputBridge` (session `i`/`@` intercepted), never blocks on `g->setup()` / `start_game` (async commands +
  `is_chargen_busy` polling).
- **Sync:** `MapSnapshot` / `ViewSnapshot` mutexes; command queue + `chargen_busy` /
  `session_active`. Atlas `Image` bytes copied once at load; draw list copied each frame.

## Consequences

- Product present is **tileset-first** (matches SDL sprite model); Godot paints a
  draw list so C++ keeps `tile_config` / looks_like resolution.
- 2.5D/3D can replace MapView’s draw backend without rewriting simulation.
- TerminalView / CPU framebuffer are debug-only; remove when HUD migrates off
  catacurses present.

---

# ADR-003 (2026-08-14): GPU-side rendering for MapView

- **Status:** Accepted
- **Scope:** replaces the *draw layer* of ADR-002's MapView. The snapshot
  contract and the "logic in C++, present in Godot" split are unchanged.

## Context

ADR-002 got sprites on screen, which was the point. But the implementation
inherited the SDL renderer's shape rather than Godot's:

- `godot/project.godot` selected the **GL Compatibility** renderer, which rules
  out 2D glow and most post-processing.
- `map_view.gd` drew in immediate mode: one `draw_texture_rect_region` per tile
  per layer, every frame. That is thousands of draw calls, and it is why the view
  is clamped to 60×40 tiles in `godot_map_snapshot.cpp`.
- Lighting reused SDL's trick of pre-tinting the whole atlas per light level.
  `godot_tileset_loader.cpp` dutifully builds all six variants
  (normal / shadow / night / overexposed / memory / silhouette) — and the
  snapshot exports only NORMAL. Five atlases' worth of memory, rendering nothing.

The last point is the tell: the pre-tinted atlas exists because SDL2 had no
cheap way to shade a sprite per-pixel. Godot does.

## Decision

**Feed the GPU CDDA's simulation data and let shaders do the shading.**

1. **Renderer:** `forward_plus`, with `mobile` as the fallback and GL
   Compatibility kept working (`--rendering-driver opengl3`) for old GPUs.
   This is what makes 2D glow and post-processing available.
2. **Batching:** one `MultiMeshInstance2D` per layer instead of per-tile
   `draw_*` calls. A unit-quad `MultiMesh` with `use_custom_data = true` carries
   the atlas UV rect in `INSTANCE_CUSTOM`; the whole layer uploads as a single
   `PackedFloat32Array`. One draw call per layer, and the existing packed
   `map_draw_cmd` already has the right shape to fill it.
3. **Lighting:** CDDA already computes vision and light per tile (`lit_level`,
   `u.sees()`), so the snapshot ships that value and the GPU multiplies it in.
   Night / low light / overexposure / night-vision / remembered-not-seen all
   become one continuous modulation colour instead of five pre-tinted atlases.

   **As implemented (2026-08-14):** the modulation travels per sprite, in
   `map_draw_cmd::tint`, and is applied through the MultiMesh's per-instance
   colour. That is per-tile, matching SDL's granularity but with continuous
   values rather than five buckets, and it needs no second texture.

   *Not yet done:* a tile-resolution light **texture** sampled bilinearly, which
   is what would make light gradients smooth per-pixel rather than stepped per
   tile. The per-instance path is the correct first step and does not block it --
   the shader gains a second term. Deferred deliberately: it cannot be verified
   without a GPU, and this environment is headless.
4. **Consequence:** the five non-NORMAL atlas variants become dead weight.
   `godot_tileset_loader.cpp` still builds them; removing that is a follow-up
   once the tint path has been confirmed on screen.
5. **Flourish on top, not instead:** `CanvasModulate` for time-of-day ambient,
   `PointLight2D` for fires and lamps, `WorldEnvironment` 2D glow for bloom,
   `GPUParticles2D` for weather / smoke / explosions, `Tween` for projectiles and
   markers. None of these are implemented yet.

Godot is **not** asked to re-derive shadows or field-of-view. CDDA remains the
authority on what the character can see; the GPU only decides how it looks.

## Alternatives considered

- **`TileMapLayer`** is the idiomatic Godot node for a tile grid, but it wants a
  static `TileSet` resource. CDDA resolves sprites at runtime through
  `looks_like`, weighted variant lists, multitile subtiles and rotation, so the
  tile identity is not knowable up front. MultiMesh keeps that resolution in C++
  where `tile_config` already lives.
- **Finishing the per-variant atlases** (the ADR-002 backlog item) would have
  matched SDL output exactly, at 6× atlas memory and with per-tile stepping
  baked in permanently. Rejected.
- **Godot-side FOV via `LightOccluder2D`** would look impressive and be wrong:
  it would disagree with the simulation about what is visible.

## Consequences

- The draw layer stays swappable, so ADR-002's 2.5D/3D option is unaffected —
  arguably closer, since lighting is already a shader concern.
- Forward+ needs Vulkan. Headless CI cannot exercise the visual path, so glow /
  lighting / particle work needs a human on a real GPU; build and
  extension-load checks stay automatable.
- `godot_tiles_rendering.*` (empty-bodied, never registered) and
  `godot_animation.*` (pixel-placeholder sketch) are deleted rather than
  finished. ADR-002 already said not to finish the former.
