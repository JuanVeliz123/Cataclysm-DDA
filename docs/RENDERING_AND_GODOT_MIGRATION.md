# Cataclysm-DDA: Rendering Architecture & Godot Migration Analysis

**Cataclysm: Dark Days Ahead** is a C++17 roguelike survival game (~half a million lines). Rendering is built on a layered design.

**Migration Status (2026-08-14):** Architecture is **ADR-002 — native Godot present**, with the draw layer refined by **ADR-003** (Forward+, batched MultiMesh, GPU lighting tint). In-session map uses **tileset MapView** (C++ draw-list + UltimateCataclysm). TerminalView renders the curses overlay for un-migrated screens; the CPU framebuffer → `TextureRect` path is debug-only.

## 1. The curses abstraction (`catacurses`)

All game code renders through a self-implemented ncurses-compatible API in `src/cursesdef.h`:

- `catacurses::window` — a shared-pointer wrapper over a native window (`cursesdef.h:54`)
- Cell-based windows created via `newwin(rows, cols, pos)` (`cursesdef.h:118`)
- Write ops: `mvwprintw`, `waddch`, `wattron/wattroff`, `werase`, `wborder` (`cursesdef.h:119-163`)
- Text is written **per-cell** with a UTF-8 string + FG/BG color attributes

| Build | Implementation | Files |
|---|---|---|
| TUI (`CURSES=ON`, no SDL) | real ncurses/PDCurses | `src/ncurses_def.cpp` |
| `TILES=ON` | custom cell-buffer "cursesport" | `src/cursesport.cpp`, `src/sdltiles.cpp` |
| **`GODOT=1` (NEW)** | map → **MapSnapshot** draw-list → Godot `MapView` | `src/godot_map_snapshot.*`, `src/godot_tileset_loader.*`, `godot/scripts/map_view.gd` |

## 2. Godot backend architecture (ADR-002)

```
CDDA game thread                         Godot main thread
────────────────                         ─────────────────
ensure_tileset_loaded(UltimateCataclysm)
do_turn → update_map_snapshot
  → resolve looks_like / tile_config
  → packed draw commands + atlas RGBA
                                         MapView.refresh/_draw
                                           get_tileset_atlas_image (once)
                                           get_map_draw_list
                                           MultiMesh per (layer, atlas)
```

### Core modules
- **`src/godot_map_snapshot.*`**: tileset load helper, atlas export, mutex draw list.
- **`godot/scripts/map_view.gd`** + **`godot/shaders/map_tiles.gdshader`**: product present (`Node2D` + batched MultiMesh).
- **`src/godot_tileset_loader.*`**: JSON + PNG → Godot textures (reuse for id resolution).
- **Curses overlay:** `godot_view_snapshot.*` + `terminal_view.gd` — carries every screen not yet a Godot Control.
- **Minimap:** `godot_pixel_minimap.*` + `minimap_panel.gd`.
- **Animations:** `godot_anim_snapshot.*` + `anim_overlay.gd` (child of `MapView`).
- **Overmap:** `godot_overmap_snapshot.*` + `overmap_view.gd`.
- **Input:** `godot_input_bridge.*` / `godot_input_backend.*`.

### Host project
- **`godot/`**: `host.gd`, `main.tscn`, chargen/world_pick scripts, `MapView`.
- Terminal / GameView gated off by default in `host.gd`.

## 3. Build Integration

Cross-platform since 2026-08-14. Full instructions:
[`doc/c++/COMPILING-GODOT.md`](../doc/c++/COMPILING-GODOT.md).

```bash
./build-scripts/get-godot-cpp.sh          # godot-cpp @ godot-4.5-stable, once

make GODOT=1 NATIVE=linux64 -j$(nproc)             # Linux x86_64
make GODOT=1 -j$(sysctl -n hw.ncpu)                # macOS
make GODOT=1 CROSS=x86_64-w64-mingw32- -j$(nproc)  # Windows x86_64 (cross)

godot --path godot
```

Produces the per-platform library named in
`godot/extensions/cataclysm.gdextension` (`.so` / `.dll` / `.dylib`) in
`godot/bin/`. SDL/`TILES` cannot combine with `GODOT`; the Godot build links no
SDL and no curses library.

All platform knowledge lives in the build system — no `src/godot_*` file
contains a platform `#ifdef`.

## 4. Migration Status Summary

| Component | Status | Details |
|---|---|---|
| Toolchain (Godot 4.7.1 + godot-cpp) | **DONE** | `build-scripts/get-godot-cpp.sh` |
| Cross-platform build (Linux / Windows / macOS) | **DONE** | 2026-08-14; Makefile only, CMake unsupported |
| Hybrid pre-game UI | **DONE** | Splash, main/load, custom chargen |
| Tileset MapView (ADR-002 present) | **DONE** | UltimateCataclysm draw-list → MapView; zoom/camera done |
| Session HUD / inventory / character | **DONE** | Godot Controls; inventory browse-only v1 |
| Curses UI overlay | **DONE** | Carries every un-migrated screen (craft, examine, options, overmap, debug) |
| ImGui in the Godot build | **DONE** | ImTui *text* backend → cell grid → overlay; no imgui-godot needed |
| Cell snapshot + TerminalView | **DONE** | Renders the curses overlay; `USE_TERMINAL_DEBUG` shows it standalone |
| Legacy framebuffer present | **REMOVED** | 2026-08-14; CPU raster path, GameView and the font/geometry rasterisers deleted |
| Forward+ renderer | **DONE** | 2026-08-14; GL Compatibility kept as fallback |
| Batched tile draw (ADR-003) | **DONE** | One `MultiMeshInstance2D` per (layer, atlas) + canvas shader, replacing per-tile `draw_texture_rect_region` |
| GPU lighting tint | **DONE (per-tile)** | `map_draw_cmd::tint` from `lit_level` → per-instance colour. Smooth per-pixel lightmap still open |
| Map fidelity | **PARTIAL** | Fields, vehicles, map memory, full item stacks, SDL-matching sprite seeds, and connected-terrain subtiles + rotation. Still open: character overlays, z-levels |
| Mouse input | **DONE** | Pixel→cell conversion + `MOUSE_MOVE` forwarding, 2026-08-14 |
| Animations | **DONE (glyph overlay)** | `godot_anim_snapshot.*` + `anim_overlay.gd`: explosions, bullets, hit markers, aim line, cursor, highlights. Particles/tweens for weather and smoke still open |
| Audio / SFX | **MISSING** | Silent: dummy `sfx::*` no-ops, no Godot audio node |
| Pixel minimap | **DONE** | Wired 2026-08-14: per-turn render → RGBA snapshot → `minimap_panel.gd` |
| Overmap as a Godot node | **DONE (v1)** | `godot_overmap_snapshot.*` + `overmap_view.gd`, own tileset from `OVERMAP_TILES`, with connected-terrain subtiles. Hordes, weather and vehicles still open |
| In-session command channel | **DONE** | `godot_game_commands.*`: Godot panels queue work that runs on the game thread at its input wait |
| Inventory actions | **DONE (v1)** | wield / wear / drop by item uid. Eat/use/reload still open |
| Remaining curses screens | **NEXT** | ~87 files open a blocking `uilist`; each needs a Godot Control to retire the overlay |
| 2.5D / 3D MapView backend | LATER | Swap draw layer only |
| SDL shim cleanup (T5.1) | **DONE** | Dead SDL C stubs and the CPU raster modules removed |

## 5. Next Steps

See [`docs/godot_migration/AGENT_HANDOFF.md`](godot_migration/AGENT_HANDOFF.md) and [`docs/GODOT_MIGRATION_TASKS.md`](GODOT_MIGRATION_TASKS.md).

1. Inventory actions (wear/drop/use) + remaining overlays.
2. MapView: character overlays, z-levels, smooth light texture.
3. Weather / smoke as `GPUParticles2D`, and glow via `WorldEnvironment`.
4. Optional: iso/mesh MapView backend.
