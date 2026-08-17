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

---

# ADR-004 (2026-08-16): The world in a SubViewport

**Status: accepted and implemented (2026-08-17)**, as item 3D-0 of ADR-006's
plan — which is the "do it when" below arriving. Written down originally because
it kept being the right answer to unrelated questions, which is usually a sign
something should be taken on its own terms rather than half-adopted three times.
What landed is recorded at the end of this ADR.

## Context

MapView, its tile batches, glyph layers, animation overlay and particles all
live in the same canvas layer as the UI, separated only by `z_index`:

| | z |
|---|---|
| MapView | 0 |
| tile batches | layer + 1 (1–11) |
| field particles | 5 |
| animation overlay | 32 |
| minimap / terminal / menus / popups | 8–18 |
| status, api warning, fatal | 100–200 |

Two consequences have already been paid for:

- **The animation overlay draws over the sidebar.** At z 32 it is above every
  panel except the status label. Combat text and explosions render on top of the
  HUD. Nobody has complained yet because until recently none of it reached a
  frame at all.
- **The presentation grade could not be a full-screen pass.** A `CanvasLayer`
  over the viewport would tint the sidebar and the menus along with the world, so
  the grade lives inside the tile shader instead. That works, and it is why
  `field_particles.gd` is *not* graded: it has its own materials and no cheap way
  to receive the same treatment.

Both are the same missing distinction: there is no boundary between "the world"
and "the interface".

## What the change would be

Render the world — MapView and every child — into a `SubViewport`, and draw that
viewport's texture as a single `TextureRect` beneath the UI.

## What it buys

- A real full-screen pass over the world only: grade, vignette, colour blindness
  modes, screen shake, damage distortion — applied once, to everything including
  particles, without touching the UI.
- The overlay ordering problem disappears by construction. The world cannot draw
  over the HUD because it is composited first.
- Render scale becomes free: the world can render at a different resolution from
  the UI, which is the usual way to keep pixel art crisp while the interface
  stays at native resolution.
- It is also the natural place to hang a 2.5D camera later (see below), because
  the world would then have its own camera and its own projection.

## What it costs

- Input routing. MapView's `_unhandled_input` (Ctrl+wheel zoom) and any future
  mouse picking need coordinates transformed through the viewport.
- Sizing and DPI. The SubViewport must track the drawable area — currently
  computed as the viewport minus the sidebar reserve — and re-render on resize.
- One more texture copy per frame. Irrelevant at this resolution, worth stating.
- `WorldEnvironment` and glow would move inside it, which is arguably where they
  belong: it would make the HDR threshold a safety margin rather than the thing
  standing between us and a glowing sidebar.

## Why it is not being done now

Nothing is broken enough to justify it mid-stream, and it touches host.gd and
map_view.gd, which two agents have been editing. The cheap fix for the ungraded
particles (push the same modulate onto the particle nodes) costs a few lines and
does not foreclose this.

**Do it when** any of these becomes true: a second full-screen effect is wanted;
the overlay-over-sidebar ordering starts mattering; render scaling is wanted for
the pixel art; or the 2.5D work below begins, at which point it stops being
optional.

## What landed (2026-08-17)

`godot/scripts/world_viewport.gd`. MapView is reparented into a `SubViewport` at
startup and the result is composited as one texture under the UI. Four things
worth knowing, three of which this ADR did not anticipate:

- **It is a `TextureRect`, not a `SubViewportContainer`**, and that is the whole
  of the interesting part. `SubViewportContainer` sizes its viewport in *logical*
  units, and this project stretches the canvas (`canvas_items` over a 1280x720
  base in a 1600x900 window), so the obvious wiring would have rendered the world
  at 1280 wide and let the stretch upscale it — 80% of the resolution the world
  has today, silently, from a change whose entire purpose is to alter nothing. So
  the render target is sized in device pixels and `size_2d_override` hands the
  world back the logical space it has always drawn in. MapView's arithmetic is
  unchanged.

  Rendering the world at a *lower* resolution than the UI is a thing this ADR
  wanted, and it is now one factor in one place — but opt-in, not the accident of
  a default.
- **The input cost was real and was a silent one.** MapView's `_unhandled_input`
  (Ctrl+wheel zoom) could no longer receive anything, because the world viewport
  deliberately accepts no input — routing it in would double every mouse event
  the host already forwards, against a cell conversion that works in window
  coordinates. The binding moved to `host.gd` and MapView exposes `zoom_step`.
  Nothing would have reported the loss: a convenience that stops working looks
  like a feature nobody used.
- **Visibility has to be mirrored.** Half a dozen places set
  `map_view.visible` and none of them know the viewport exists. Left visible over
  a hidden world, the composite keeps showing the last frame the world drew, so
  the main menu opens over a still of the map. The rect follows the world's own
  flag — not `is_visible_in_tree()`, which includes the rect and would latch it
  off.
- **`use_hdr_2d` does not come with the project setting.** `viewport/hdr_2d`
  covers the root viewport only, and the fire glow keys on the tile shader
  writing above 1.0. Without it set on the SubViewport the glow pass has nothing
  to find and says nothing about it.

  Setting it turned out to matter more than that. Opening the project in the
  Godot editor rewrote `project.godot` and revealed that **`viewport/hdr_2d` had
  never been in effect**: the setting sat under a block of `#` comments, `#` is
  not a comment character in this file format, and the editor wrote the setting
  back out as a key named `#Letsthetileshader…viewport/hdr_2d` — the comment
  glued to the key it preceded. Godot can only write back a setting it loaded, so
  that is what it had been loading all along. The root viewport was never HDR, so
  the glow's whole premise — the shader writing above 1.0 and a threshold picking
  it out — had nothing to key on.

  This is the ADR-005 pattern again, in a new place: a value the code sets
  correctly and the *format* silently discards. Neither code review nor the
  shader could have found it; only the editor rewriting the file did. The setting
  is restored, `#` is documented as forbidden there, and the world viewport now
  declares HDR for itself regardless.

Two things this buys that are **not** taken yet: the presentation grade is still
inside the tile shader (so `field_particles.gd` is still ungraded), and the
animation overlay still carries its own z discipline. Both are now possible
rather than done — the grade's constants have never been looked at by anyone
(VER-1), and moving them and changing them in one step would leave nobody able
to say which did what.

Verified headlessly by `headless_probe.gd`'s `world_viewport` stage: the world is
inside the viewport, the rect is the drawable area in logical units, the render
target is that area in device pixels, and hiding the world hides the composite.
The stage is in `REQUIRED_STAGES`, so a run that skips it fails rather than
reporting green.

---

# ADR-005 (proposed, 2026-08-16): What 2.5D would actually require

**Status: analysis, no decision.** ADR-002 and ADR-003 both say the draw layer
stays swappable so that "2.5D/3D" remains open. This is what taking that option
would actually cost, written before rather than after someone starts.

## The constraint that decides everything

**UltimateCataclysm's sprites are drawn for one fixed viewing angle**, with
perspective and lighting already baked into each one. A tree is drawn as seen
from a particular elevation; a car roof is drawn as a roof seen from that same
elevation.

That rules out the thing "2.5D" usually means. A tilted or orbitable 3D camera —
`Sprite3D` billboards on a ground plane, an orthographic camera at 45° — would
invalidate every sprite in the tileset at once. What you would get is cardboard
cutouts standing on a floor, lit twice: once by the engine and once by whoever
drew them. The pipeline document's §5 (Blender → sprite) is the art programme
that would make a movable camera viable, and it is deferred precisely because it
is a programme and not a task.

So for this tileset, 2.5D can only mean **depth cues within the fixed angle**.
That is a smaller and much more achievable thing, and it is where the value is
anyway: the top-down view does not need to rotate, it needs to stop looking flat.

## What is already loaded and ignored

The loader parses all of this and the renderer consumes none of it:

| Field | Purpose | Ultica's value |
|---|---|---|
| `height_3d` | per-sprite vertical stacking offset | declared on no tile |
| `zlevel_height` | vertical offset between z-levels | `0` |
| `offset_retracted` | alternate offset when a tall sprite must duck | present |
| `prevent_occlusion_min/max_dist` | when to retract | `-1.0` / `1.0` |
| `is_isometric` | isometric projection | `false` |

Worth noting what that table says: Ultica declares the retraction data and
essentially nothing else. `zlevel_height` is 0 and no tile declares `height_3d`,
so a stacking offset would be **our** number, not the tileset's. That is
allowed — SDL derives its own too — but it means height is a rendering decision
here, not data we can simply start honouring.

## What would be necessary, in dependency order

1. ~~**The map snapshot must publish more than one z-level.**~~ **Done.**
   `update_from_game` walks each column of the view downward from the avatar's
   level and stops at the first tile with a floor under it — the same walk, with
   the same stop condition, that `cata_tiles::draw` has always done.

   **It is not the expensive item.** The multiplier this list budgeted for —
   "per-tile work multiplies by the number of levels drawn" — never applies,
   because the stop condition is `map::dont_draw_lower_floor`, one bool out of
   the level cache. Every tile with a floor under it, which outdoors is every
   tile, pays exactly that one lookup and publishes one level. What descends is
   holes: a stairwell, a pit, the lip of a roof. `open_columns` in the render
   stats is that count per frame, and it is zero on an outdoor level.

   That number is published rather than measured once and written down, for the
   same reason the rest of this section exists: the estimate that was wrong here
   was wrong in the direction of not looking.

   The visibility rules did turn out to be the game's, and asking it was enough:
   `Character::sees` is already 3D (it gives up past `fov_3d_z_range`, which is
   10), and `map::dont_draw_lower_floor` is the same gate SDL stops on. Nothing
   about "what can you see of the floor below" had to be decided here.

   Two things the walk does *not* do, both because the channel is per column and
   has nowhere to put a second level: the light texture is written for the top
   level only, and field particles are emitted for the top level only. Lower
   levels therefore sit out the light pass (`receives_light = false`) and carry
   the CPU lighting in their tint instead, dimmed once per level crossed by
   `fog_for_depth` — which is SDL's per-level fog overlay, arriving as a multiply
   because there is no geometry pass here to lay a translucent rect with.
2. ~~**A depth field on the draw command.**~~ **Not needed for one z-level, and
   four bits for the rest.** The elevation range this item said was missing is
   `cmd_z_below_shift`: levels below the avatar, 0-15, riding in `rot_flags`
   beside the palette and the tall flag. The stride is still ten ints.

   The original finding stands for depth *within* a level: the depth of a sprite
   is the bottom edge of its quad, `dest_y + src_h`, and both of those already
   cross the boundary. `dest_y` alone is not the depth — it is the top edge
   *after* the sprite offset, so a 32×64 tree one row in front of a 32×32 rock
   has the same `dest_y` — which is what made this look like a missing field. It
   is the same anchor the contact shadows use.
3. ~~**A depth order that combines** z-level, layer, y, and height.~~ **Done**,
   for the single-level case: `map_view.gd`'s `depth_rank`. Two observations
   made it cheap. Sprites are anchored to the bottom of their cell, so a sprite
   can only cover rows *above* its own — which means anything that fits its
   cell cannot overlap another cell at all, and only the standing content
   (tall sprites and creatures) needs row ordering. And the ordering does not
   need `z_index`: same-z canvas items draw in tree order, which has no range
   limit, so it costs no z at all. Layer now only breaks ties within a row.

   **Elevation is in it now.** A level is either nearer the camera than another
   or it is not, and no arrangement of rows inside one can change that — so
   `z_below` outranks everything else and each level gets its own block of ranks,
   deepest first, with the within-level ordering untouched. The one thing this
   forced: the layers that draw the whole map at once and therefore sit at a
   single rank — contact shadows, field particles, the fallback glyphs — had to
   be seated explicitly in the *avatar's* block. Left at their old raw ranks they
   would have become the deepest thing on screen, painting shadows under the
   bottom of every pit in view.
4. **Shadows.** A blob under each creature and tall object is the cheapest thing
   on this list and probably the largest single gain in apparent depth. It needs
   no new art and no new data.
5. **Retraction** (see the Tier 3 item in `BACKLOG.md`) — tall sprites ducking
   when they would hide the player. Data already present; the only one of these
   that is nearly free.
6. **Parallax between levels**, once there is a camera concept: levels below
   shifting slightly less than the current one as the view moves. This is the
   cue that sells depth in a fixed-angle view, and it is why ADR-004 stops being
   optional here — parallax means the world needs its own camera, which means
   the world needs its own viewport.

## Recommended order if this is taken up

5 → 4 → 1 → 3 → 2 → 6. Retraction and shadows are cheap, need no architecture,
and can be judged by eye immediately; they will also reveal whether the fixed
angle can carry the illusion before anything expensive is built on the
assumption that it can. Z-levels are the big one and should not be first.

**What actually happened: 5 → 4 → 3 → 1, and 2 dissolved twice.** Item 3 was
pulled ahead of z-levels because it turned out not to be an item you can defer —
with the sort layer-major, every creature drew over every tree no matter where
either stood, which reads as a bug rather than as a missing feature. It was
reported as one. Only item 6 is left, and it is the one that waits on ADR-004.

Two of the three cheap items came out differently than this list assumed, and
in the same direction:

- **Retraction was inert.** `offset_retracted` is read per sheet and defaults
  to `sprite_offset`; no UltimateCataclysm sheet declares one, and the tileset
  ships no `_transparent` variants either. "Data already present" was wrong,
  and nothing about the code could have shown it — the check is a property of
  the art. What looked like retraction working was `glow_bloom` washing the
  canopies out.
- **The depth field was already there**, spelled as two fields that were each
  being read for something else.
- **And the distance band that gates occlusion handling is (-1, 1)**, which
  works out to 50% on the avatar's own tile and exactly zero one tile away — in
  SDL as much as here. Nearly caught out a second time: the fallback written to
  replace retraction was gated on that same band, and would have been inert for
  precisely the reason retraction is inert.

All three were answered by opening the tileset and reading a number, and all
three had been reasoned about at length beforehand without moving. The pattern
is specific enough to name: **every one of them was a value the tileset
declares and the code faithfully reads, where the reading is correct and the
value makes the feature do nothing.** Code review cannot find that. Only
printing the value can.

Worth remembering for item 1, which this list calls the expensive one on the
basis of the same kind of reasoning: check what the game actually publishes per
z-level before budgeting for it.

**Item 1 was the fourth.** It was budgeted as the expensive one because per-tile
work would multiply by the number of levels drawn. It does not multiply, and the
reason was one function away the whole time: SDL's own walk stops at
`map::dont_draw_lower_floor`, so a tile with a floor under it publishes one level
and pays one bool from the level cache for the privilege. A tile without a floor
is a hole, and holes are rare enough to count — which the renderer now does, per
frame, as `open_columns`.

So the pattern generalises past the tileset. The three before it were a value
the art declares and the code reads correctly to no effect; this one was a cost
the design assumed and the reference implementation had already refuted. Both
are the same failure: **reasoning about a number instead of reading it.** The
counter is in the render stats for that reason and not because anyone needs to
watch it.

What this does not settle is whether it *looks* right. `open_columns` is zero in
the shelter the probe starts in, so the headless run proves the walk compiles and
declines to descend — not that a basement seen from above reads as a basement.
The fog constants in `fog_for_depth` are first guesses in exactly the way
`sway_amount` was, and the same thing settles them: someone standing at the top
of a staircase.

## What this is not

Not isometric. The loader supports `iso` and Ultica is not isometric; switching
projection is a tileset choice, not a renderer upgrade, and would need an
isometric tileset to mean anything.

---

# ADR-006 (2026-08-17): The world as 3D geometry

**Status: accepted, and the product path since the same day.** `host.gd`'s `USE_3D_MAP`
is on: the world is drawn as quads in a 3D scene, and MapView stays in the tree as the
fallback and as what the headless probe drives.

The decision this ADR called genuinely open — how far the world stands up — was settled
the way it proposed, by a parameter and a look rather than by more writing: the angle
reads correctly on a real GPU, so option A (stay flat, and get most of it in 2D) is
retired.

**`TILT_DEGREES` defaults to 45.** The last thing keeping it at zero was that the
fallback glyphs and the animation overlay were hidden while tilted — and that turned out
to be a fix for a problem that was not there. An affine canvas transform cannot follow a
rotated camera in general, but everything on that canvas annotates the *ground*, and a
ground point's screen position is unchanged by the tilt **by construction**: it is what
the pre-stretch is for. The gate had been printing the proof for two days — a floor sprite
lands on the same screen pixel at 0° and at 75° — and now holds the canvas transform
against the camera's projection directly, at six tilts, failing on a 5% error in either.

An estimate of M, delivered by deleting eleven lines. Worth noticing which direction that
error ran in: every other surprise on this branch cost more than the estimate, and this one
cost less because the property that made it free had already been measured and written
down.

## What is being asked, and what ADR-005 already answered

The ask is to keep the game top-down and use Godot's 3D renderer to make it look
better. ADR-005 appears to have refused that and did not: what it refused was a
**movable or tilted camera under unchanged art**, on the grounds that Ultica
bakes one viewing angle into every sprite. That verdict stands and nothing below
disturbs it.

Three different things travel under the word "3D" and they have three prices:

| | What it means | Price |
|---|---|---|
| 3D **rendering** | real lights, cast shadows, depth-buffered occlusion, fog, volumetrics, a post chain over the world only | the subject of this ADR |
| 3D **scene** | every sprite has a world coordinate and a normal instead of a screen rect and a sort rank | the mechanism that buys the row above |
| 3D **art** | meshes instead of sprites | the pipeline document's §5, an art programme, still deferred |

The first is what is wanted. The second is how to get it. The third is not on the
table and is not required by either of the others — which is the whole finding
here.

## The projection is not the obstacle, because the art already resolved it

Ultica's projection is oblique and internally inconsistent. A floor cell is a
square seen from directly overhead. A wall in the same image is a facade seen
from the side, 64 px tall inside a 32 px cell. No single camera produces both,
which is exactly why rotating one invalidates the tileset.

It does not follow that the sprites cannot live in a 3D scene. There are two
mappings that put them there, and they differ in one thing only: whether a
standing sprite is a picture of a standing thing, or a standing thing.

### A — the flat world

Every quad stays parallel to the image plane, exactly as today. The camera is
orthographic and looks straight down the depth axis. Screen x/y are unchanged, so
**the output is pixel-identical by construction**; the third axis carries nothing
but depth, assigned from what `depth_rank` already computes.

What that buys:

- **The depth buffer replaces `depth_rank`.** With alpha-scissor sprites in the
  opaque pass, the hardware sorts. That deletes the per-row batching in
  `_rebuild_batches` — the one whose own comment budgets "a few hundred [draw
  calls] in a dense forest, against six for the whole map before" — and takes it
  back to one batch per atlas.
- Depth fog, which is what `fog_for_depth` is hand-rolling as a tint multiply.
- Lights with a real position and a real falloff, in colour.
- `GPUParticles3D` in a volume rather than on a plane.
- A post chain over the world and not the UI (which is ADR-004's gain, not this
  one's).

What it does not buy: **any shading that depends on orientation.** Every normal
in a flat world points at the camera, so a lamp to the left of a wall lights it
exactly as a lamp above it does. No cast shadows either — every silhouette is
coplanar with the ground it would fall on.

### B — the stood-up world

Ground quads lie horizontal. Standing sprites stand vertical. The camera tilts to
an elevation φ, and the geometry is **pre-stretched so that the tilt exactly
cancels**: a ground cell is scaled to `tile/sin φ` along the row axis, a facade to
`height/cos φ` in world height. Both project back to the pixels the artist drew,
because the stretch is the inverse of the foreshortening.

So option B is also pixel-faithful. It is not pixel-identical — the camera is a
real projection and the two axes disagree about scale — but it reproduces the
same sprite sizes at the same screen positions.

What φ can be is not free, and the stretch says why. The height a facade ends up
with, measured in tiles, is `2·tan φ` for a 64 px sprite in a 32 px cell:

| φ | ground cell depth | 64 px facade height | that facade, in tiles |
|---|---|---|---|
| 90° | 32 | ∞ | degenerate — this is option A |
| 60° | 37 | 128 | 3.5 |
| 51° | 41 | 102 | 2.5 |
| 45° | 45 | 91 | 2.0 |
| 30° | 64 | 74 | 1.2 |

A CDDA tile is one metre and a wall is two to two and a half, so the band that
makes the stood-up world physically sensible is roughly **45°–55°**. That is a
derived starting value rather than a guess, but it is derived from one assumed
correspondence — that Ultica's tall sprites are 32×64 and that such a sprite is a
wall-height object — and that assumption is unverified, because `gfx/` is a build
artifact and is not composed in this checkout. **Compose the tileset and read the
sheet dimensions before trusting the table.** That is the ADR-005 habit and this
is precisely its shape: a number reasoned about at length that one command
settles.

What B buys on top of A:

- **Correct normals.** The floor faces up, the facade faces the viewer. A lamp on
  the floor lights the floor near it and the wall it stands against, and the
  shading changes when it moves. This is the difference that makes 3D lighting
  read as 3D at all.
- **Real cast shadows.** An alpha-scissored billboard casts its own silhouette.
  A tree's shadow stretching across the ground as the sun moves needs no art and
  no data — see the sun row in the table below.
- **Real occlusion.** A wall hides what is behind it because it is in the way,
  not because a rank said so.
- **Volumetric fog**, which is where light shafts through smoke and rain come
  from, and which is the single most "3D-looking" thing available for free.
- **Elevation, and therefore parallax** — ADR-005 item 6, the only one of its
  list still open. Levels below become geometry at a lower height instead of a
  tint dimmed by `fog_for_depth`, and a camera with any perspective at all gives
  the parallax for nothing.

Because A is B at φ = 90°, they are not two projects. **Build the scaffold with
φ and the ground-plane orientation as parameters, and look.**

### The line between them

Almost everything option A buys is also available without leaving 2D: `Light2D`
already has position and falloff, `CanvasItem` already takes a normal map, and
the batching win could be had by other means. **3D pays for itself only if the
world stands up.** If the tilt experiment fails — if Ultica's implied angle turns
out to vary too much between a tree, a wall and a table top for one φ to serve —
then the honest outcome is to stay in 2D and spend the effort on `Light2D` plus
generated normal maps instead. That is the kill criterion, and it arrives at
milestone M3 below, before anything expensive is built on top.

## The constraint that holds either way

**Godot must not become the authority on where light reaches.** CDDA casts rays
and respects occluders; an `OmniLight3D` beside a wall drawn as a billboard will
happily light the room behind it. This is ADR-003's rule about field of view,
arriving a second time in a new costume.

So the per-tile light texture stays, and stays in charge:

- **R (visibility / memory) cannot be re-derived at all** and is untouched.
- **G (light amount) remains the authority on brightness.** The 3D lights
  contribute an additive, clamped colour-and-direction term *masked by* G, so a
  tile the game says is dark cannot be lit by the renderer.

There is a second reason for that split, specific to option B. The pre-stretched
world is anisotropic — one axis is scaled and the other is not — so a spherical
light falls off elliptically in it, and a lamp's pool on the ground would read as
an oval on a floor drawn from straight above. Keeping the pool's *shape and
reach* in CDDA's texture and letting the 3D lights do facades, colour and
sparkle avoids the artifact instead of tuning it. Shadows keep the same
distortion and it does not matter: a shadow is a cue, not a measurement.

Third, the sprites have lighting baked in already. Engine light on top
double-shades. The same handshake `set_light_pass_enabled` uses applies —
calibrate so that the net result at full daylight equals what ships today, and
let the lights be a delta from that.

## What is already computed and ignored

In the ADR-005 tradition, because this is the part that decides the cost:

| Where | What it holds | Consequence |
|---|---|---|
| `level_cache::light_source_buffer` (`src/level_cache.h:37`) | per-tile `{ float luminance, light_color_rgb color }`, filled every turn by `map::generate_lightmap` from terrain / furniture / field `light_emitted` | **The point-light list is read, not derived.** Walk the view extent, emit the non-zero entries. Fires and lamps arrive with colour attached. |
| `map::apply_light_source` callers | glowing critters, the avatar's held light — these bypass the buffer | Needs its own small tap; three call sites in `lightmap.cpp`. |
| `map::apply_light_arc` (`src/map.h:2196`) | position, angle, luminance, cone width — vehicle headlights | Maps onto `SpotLight3D` almost field for field. Cheap, and headlight cones at night are the flashiest thing on this list. |
| `sun_azimuth_altitude( time_point )` (`src/calendar.h:664`) | the sun's actual azimuth and altitude; `calendar.cpp:252` already builds the 3D direction vector | One `DirectionalLight3D`, aimed. Shadows that move with the time of day, for two floats on the existing conditions channel. |
| `map_draw_cmd` | tile = `dest / tile_size`, height = `src_h`, standing = `cmd_flag_tall`, level = the `z_below` bits | **Phase 1 needs no new field and no stride change.** The 3D transform is derivable from what is already published. |
| `zlevel_height` = 0, `height_3d` on no tile | ADR-005 found this already | Elevation is our number. Same conclusion, now load-bearing. |

The one thing not in this table is Ultica's implied elevation angle and how
consistent it is across sprite kinds. That is unmeasurable from here — `gfx/` is
composed, not committed — and it is the input the A/B decision turns on.

## ADR-004 stops being optional

A 3D world needs a `Camera3D` in a viewport, and the UI has to stay in the 2D
canvas. ADR-004 said this would become mandatory when the depth work started;
this is that. Do it first, on its own, as its own change.

What has to be re-homed when the world moves into a 3D viewport:

- `glyph_layer.gd` — a fallback glyph must keep its sprite's depth. Either a 2D
  layer inside the world viewport, or `Label3D` at the tile.
- `anim_overlay.gd` — stays screen space, positioned through
  `Camera3D.unproject_position`. It stops being able to draw over the sidebar,
  which is ADR-004's other stated gain.
- `shadow_layer.gd` — superseded outright under option B, kept under A.
- `field_particles.gd` — `GPUParticles2D` → `GPUParticles3D`, and it becomes
  gradeable, which fixes the "particles are not graded" follow-up in `BACKLOG.md`.
- `WorldEnvironment` — moves inside the world viewport, taking `hdr_2d`, the glow
  threshold and the `glow_bloom = 0.0` scar with it.

## Pixel-art hazards, which are the real risk

The 3D renderer defaults are all wrong for a 32 px sprite and every one of them
degrades quietly:

- Nearest filtering, **no mipmaps**, no anisotropy. A mipmapped atlas bleeds
  neighbouring sprites the way `default_texture_filter=0` exists to prevent.
- No MSAA on the world, no FXAA/TAA. TAA on a nearest-sampled sprite smears it.
- **Snap the camera to whole pixels** and keep the viewport an integer size, or
  the whole scene shimmers on every step the avatar takes.
- Alpha: **alpha-scissor**, so sprites go in the opaque pass and the depth buffer
  sorts them. This is the mechanism the batching win depends on. Genuinely
  translucent things — glass, smoke — stay in the transparent pass and keep an
  explicit order, so the existing depth logic does not disappear so much as
  shrink to the cases that need it.
- Shadow maps cost one pass per casting light per frame. Cap the casters (the
  nearest N, plus the sun) rather than letting a lit room decide the frame rate.

## Plan, with a milestone that can fail

- **M0 — ADR-004.** World in a `SubViewport`. No 3D yet. Independently valuable
  and independently reviewable.
- **M1 — the 3D backend at φ = 90°, flat.** `MultiMeshInstance3D` per atlas, a
  spatial shader taking the same `INSTANCE_CUSTOM` sub-rect, alpha-scissor,
  orthographic camera. **The milestone is that a screenshot is indistinguishable
  from the 2D backend's**, diffed under the existing recipe:

  ```bash
  xvfb-run -a -s "-screen 0 1600x900x24" \
    env VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
    godot --rendering-driver vulkan --path godot \
          res://scenes/headless_probe.tscn -- --screenshot /tmp/3d.png
  ```

  A pixel diff is a real pass/fail, which is more than any other rendering
  milestone on this branch has had. Everything after it is a deliberate departure
  from a known baseline.
- **M2 — lights.** Read `light_source_buffer` into a point-light channel; masked
  by the G channel per the rule above. Under A this is where the gains stop.
- **M3 — the tilt experiment.** φ and ground orientation as parameters, one
  forest and one room, screenshots across 45°–60°. **This is the decision point
  and it is allowed to end the project**; if one φ cannot serve trees, walls and
  table tops at once, go back to 2D with `Light2D` and normal maps.
- **M4 — z-levels as elevation.** Levels below become geometry; `fog_for_depth`
  gives way to `Environment` fog, and lower levels stop sitting out the light
  pass because a light two floors down is simply a light with a position.
  Parallax (ADR-005 item 6) falls out.
- **M5 — the things only 3D has.** Sun and moon as a `DirectionalLight3D` from
  `sun_azimuth_altitude`, volumetric fog, headlight `SpotLight3D` cones, weather
  as particles that fall through the scene.

M0–M2 are verifiable headlessly. M3 onward needs a person on a real GPU, which is
the same limit ADR-003 recorded and the same one `VER-1` exists for.

## What it costs

- **Two draw backends for a while.** GL Compatibility 3D has no volumetrics and
  weak glow, so the 2D path stays as the low-tier fallback. That is a real
  maintenance tax and the reason M1's pixel-diff milestone matters: two backends
  that agree are maintainable, two that drift are not.
- The snapshot gains one channel (lights) and loses none.
- Everything in the hazards section is a way to ship a blurry game.

## Alternatives considered

- **Real 3D meshes.** The pipeline document's §5. Still an art programme, and
  worse than either extreme in the interim: a mesh wall beside a painted wall
  looks wrong in a way that neither does alone.
- **Heightfield terrain.** CDDA has no elevation between z-levels, so a
  heightfield could only express the steps option B already gives.
- **Isometric.** Answered in ADR-005: a tileset choice, not a renderer upgrade.
- **Stay in 2D, add `Light2D` and generated normal maps.** The honest rival, and
  the fallback M3 selects if the tilt fails. It gets most of option A and none of
  option B.

## What this is not

Not a rotatable camera — the tilt is fixed, chosen once, and cancelled by the
geometry. Not new art. Not a change to anything the game decides: CDDA still owns
what is visible, what is lit and how brightly, and Godot still only decides how
that looks.

## Amendment (2026-08-17): meshes are the destination after all

This ADR argued twice against 3D art — that §5 of the pipeline document is a programme
rather than a task, and that a mesh wall beside a painted wall looks worse than either
choice made consistently. **The second argument has been overruled deliberately**, and by
the person whose game it is: the intent is to move creatures, then everything, to meshes,
accepting that a half-migrated map looks wrong while it is half-migrated. That is a
different trade from the one this ADR weighed, not a mistake in it — the ADR treated
consistency as a constraint, and it turns out to be a cost someone is willing to pay for a
destination.

What that changes here is small, which is the useful part: **nothing about the renderer has
to be undone.** The stood-up world is what makes meshes worth having at all — a mesh in a
flat world would be a mesh photographed head-on — and every piece of it stays: the camera,
the pre-stretch, the depth per sprite, the light channel, the level elevations.

**The first mesh already exists.** `map_view_3d.gd`'s shadow proxies are one invisible
capsule per creature, and a modelled body replaces exactly that node: same place in the
tree, same per-creature transform from the same anchor, same shadow setting. What changes
is the mesh resource and whether it is drawn. The staged path from here, in the order that
keeps something judgeable at every step:

1. **Shadow proxies** (done). A mesh per creature, invisible, casting. Proves the placement
   and immediately buys a shadow that says where the light is.
2. **Make them visible behind a constant.** Ugly on purpose, and the cheapest possible
   answer to "does a body at this scale sit correctly in this world" before any art exists.
3. **A mesh per creature id, defaulting to the billboard.** This is the step that needs
   C++: the draw command carries an atlas rect and no identity, so a renderer cannot know
   *what* it is drawing. A creature channel — id, position, facing, one entry per visible
   creature — is the prerequisite, and it is small next to what it unblocks. Anything with
   a mesh uses it; everything else stays a sprite. Mixed by construction, which is the
   trade being accepted.
4. **Art.** Which is the programme, and now has somewhere to arrive.

Two things were named as the ones that would hurt, and both have been answered by the
person who gets to answer them (2026-08-17):

- **The ~3,400 clothing overlays: start naked.** A mesh body wears nothing for now, which
  costs less than it sounds like because the overlays are not reaching the screen at
  present anyway — the missing-clothing report is still open. Worth keeping those two
  states apart in the record though: *naked because it is not implemented* and *naked
  because something is dropping the commands* look identical and are not the same, and the
  first-frame `DROPPED for a missing atlas` line is what tells them apart.
- **Ultica's baked lighting: disregard it for now.** A lit mesh beside sprites that carry
  their own painted sun will disagree about where the light is, and the intent is to
  replace that art rather than to reconcile with it. So the disagreement is expected and
  temporary rather than a bug to design around, and nothing in the renderer should grow a
  reconciliation layer for it.

Neither answer needs code today. Both are the kind of decision that gets re-litigated in
six months by whoever finds the mismatch, which is the only reason they are written here.

## What landed: 3D-1a, the flat world (2026-08-17)

`godot/scripts/map_view_3d.gd` and `godot/shaders/map_tiles_3d.gdshader`, behind
`host.gd`'s `USE_3D_MAP`, **off by default**. Same draw list, same ten-int
command, no C++ change — which is the claim this ADR made about phase 1 and it
held.

- **The camera is orthographic and unrotated**, so z cannot move a sprite by a
  pixel. That is the whole reason the flat world can be pixel-faithful, and it is
  also why the shader is `unshaded`: every normal points at the camera, so engine
  lighting could only multiply the frame by a constant. Lighting is still CDDA's,
  through the same light texture, sampled by a line-for-line port of the canvas
  shader.
- **Rank decides the order and the order decides z.** `depth_rank` spans about a
  million values and mapping that onto z directly would ask float32 to resolve
  0.001 at a magnitude of a thousand. So the seats are sorted exactly as the 2D
  backend sorts them, then handed consecutive `Z_STEP`s. The ordering is provably
  the same and the numbers stay small.
- **Then scissored (3D-1b, same day).** Alpha-scissor puts the sprites in the
  opaque pass with per-pixel depth, so interleaving stops being a property of draw
  order and each sprite can carry its own z. The batch key collapses to the shader
  uniforms alone — atlas, sway, palette, receives-light — because those are the
  only things that genuinely cannot vary within a batch. About a dozen batches
  against the 2D backend's few hundred in a forest, and the depth relationships are
  still `depth_rank`'s, now per sprite rather than per batch.

  **This overtook the tilt experiment, and the ADR's own recommended order was
  wrong about it.** A cast shadow is a silhouette cast by an opaque-pass material,
  so 3D-5 needs this; and since the flat world is `unshaded`, standing the world up
  shows nothing at all until there are lights and shadows to show it with. The
  order in this ADR treated 3D-1b as a performance nicety and the tilt as
  independent. They are not: the cheap experiment depends on the expensive-looking
  plumbing.

  The cost is real and is the only deliberate visual change so far: alpha below the
  threshold is discarded rather than blended, so a soft sprite edge steps. Judge it
  against a screenshot of 3D-1a rather than of the canvas. And note what the
  uniform cannot do — writing `ALPHA_SCISSOR_THRESHOLD` at all is what makes the
  material a cutout, so lowering it to zero makes half-transparent pixels opaque
  instead of restoring blending.
- **One thing could not be settled from here and is a flag, not a guess.** The
  canvas pipeline treats colours as sRGB and writes them out; the 3D pipeline
  treats them as linear and encodes on output. The grade was authored against the
  canvas, so the shader computes in sRGB and decodes on the way out, and
  `pipeline_encodes_srgb` turns that off if this target is not converted after
  all. One flag, one screenshot, one answer — which is the discipline this ADR's
  predecessor was written about, applied to something genuinely unreadable without
  a GPU.
- **And then the other four layers (3D-1c, same day).** A viewport composites its
  canvas *over* its 3D scene, so none of the non-tile layers could simply come
  along, and the decision split by whether depth matters. Contact shadows became
  geometry — a MultiMesh of blob quads in the world — because on the canvas they
  would land on top of the creature casting them. Fire and smoke became
  `GPUParticles3D`. The fallback glyphs and the animation overlay stayed canvas
  items, on a `CanvasLayer` inside the world's viewport whose transform reproduces
  the camera: font and vector drawing gains nothing from being meshes, and the
  overlay belongs over the world anyway.

  Two things fell out of this that the plan did not anticipate. Depth-testing the
  shadows made them **better** than the 2D pass rather than merely equal: a tree
  between the camera and a blob now hides it, and a blob can sit on the floor of a
  level below the avatar's — which `map_view.gd` has to skip, because one node has
  one depth to spend. And the canvas transform turned out to be *exactly* the
  `position` and `scale` the 2D backend puts on MapView, derived independently from
  the camera, which makes each a check on the other.

  What it costs: the glyphs lose their per-layer depth band, which only affects
  tiles whose art is missing in the first place. And the canvas transform is exact
  only while the camera is unrotated, so **3D-3 breaks it** — at which point those
  two layers need `unproject_position` per point, or a home in the world.

Verified headlessly by the probe's `map_view_3d` stage: every draw command becomes
an instance, the batch count is down with the depths still present, the four
non-tile layers exist, the canvas is scaled to the camera's zoom (checked after a
zoom step, because at 1.0 an identity transform passes while proving nothing), and
the camera is orthogonal and unrotated. `res://scenes/shader_check.tscn` compiles
the new shader through Godot's own front end. None of that says it looks right —
that needs the flag on, a real driver, and two screenshots.

### The grey map (2026-08-17), and two rounds spent not looking

`USE_3D_MAP` rendered a flat grey world with a working UI over it, and the probe's
3D stage reported that it had never run. **One cause: `map_view_3d.gd` did not
compile.** Line 862 read `xform.x.length()` on a `Transform3D`, which has no `x` —
`Transform2D` exposes its axes directly and `Transform3D` keeps them on `basis`. The
port carried the expression across and it parses fine.

What a failed *compile* looks like from the outside is the part worth recording,
because nothing in it says "script":

- `set_script()` on a broken script leaves the node with no script, so
  `has_method("setup")` answers **false**, so `host.gd` skips building the world and
  skips refreshing it — silently, because both calls are guarded exactly as they
  should be.
- The viewport therefore has no camera, no environment and no geometry, and paints
  the project's default clear colour, which is a flat mid-grey.
- The probe's stage died on its first call into the script, so its `_stage()` marker
  was never set and VER-0 reported "never ran" — accurately, and unhelpfully.

**And the tool that answers this in seconds was on the machine the whole time.**
`gdparse` (gdtoolkit) was being run as the gate; it checks syntax only, so it passed
every version of this file. Godot's own front end — `--headless --check-only
--script` — needs neither the GDExtension nor a GPU and names the file and line.
That is now `build-scripts/check-godot-scripts.sh`, which also runs the shader scene,
and which was verified by breaking a script and watching it go red.

This is ADR-005's pattern again and in its purest form so far: two rounds were spent
reasoning about which Godot subsystem could produce a grey frame, and a mechanism was
*documented as the cause* on the strength of that reasoning, while the actual answer
was one command away. The counter-habit is not "reason better", it is **run the thing
that knows**.

Two changes made while chasing the wrong cause are worth keeping on their own merits:

- **The camera is re-asserted per refresh** (`_ensure_current`). The hypothesis was
  that `Camera3D.make_current()` had done nothing, because it returns early unless
  the camera is inside a `World3D` and a `Node3D` subtree is only inside one while
  visible — and the host builds the world hidden. That was never the live bug and is
  still true of the code, so the guard stays. Note that the *obvious* form of it
  would have been a placebo: `make_current()` sets the camera's own `current` flag
  before the early return, so `is_current()` answers true for a camera the viewport
  has never heard of. The question has to go to the viewport, `get_camera_3d() ==
  camera`, and both are printed side by side so they can be seen to disagree.
- **The probe builds the world hidden and then shows it**, as the host does, instead
  of building it visible. A fixture that constructs things more conveniently than the
  product does is a fixture that agrees with whatever it built.

The backend prints one diagnostic line on its first frame — batches, instances,
depths, whose camera the viewport is using, the viewport's flags, whether the
environment is attached — and one line if it never gets that far, because both ways
out of `refresh` before anything is built are legitimate and silent, and silence is
what a broken backend looks like too. The step-by-step trace that found nothing came
out once the world was confirmed drawing (2026-08-17).

### What landed: 3D-2, the light channel (2026-08-17)

`LightSnapshot::add_light` / `CDDAHost::get_light_sources`, seven floats per source:
position and reach in the view-relative pixels the draw commands already use, colour,
and luminance in the game's own units. **API version 20** — the first step of this ADR
that needs a rebuild.

The estimate for this said discrete lights would have to be recovered from the
lightmap. They did not: `level_cache::light_source_buffer` is a per-tile
`{ luminance, colour }` that `map::generate_lightmap` fills every turn from terrain,
furniture and field `light_emitted`. The walk that writes the light texture reads it
on the way past. ADR-005's habit applied before the fact instead of after it, for
once.

Three things a reader should know before building on it:

- **It reads state `level_cache.h` calls "only valid during
  `generate_lightmap`".** That buffer is scratch: filled at the top of the function,
  consumed inside it, and never cleared at the end — which is why it still holds this
  turn's sources when the snapshot runs, and is not what it promises. The alternative
  was to re-derive the sources from `light_emitted` here, duplicating a rule the game
  owns. Reading the game's own answer wins, but the render overlay now counts
  `light sources` so that contract breaking announces itself instead of the lights
  quietly never arriving.
- **The unbuffered sources are still missing.** `apply_light_source` callers bypass
  the buffer — glowing critters and the light the avatar is carrying — and so does
  `apply_light_arc`, which is vehicle headlights and which maps onto a `SpotLight3D`
  almost field for field.
- **Nothing lit by them is visible yet**, and that is the shape of the step rather
  than an oversight. The tile shader is `unshaded`, so in a flat world a light can
  only multiply the frame by a constant. The lights are positioned, capped, counted
  in the overlay and checked by the probe; the lit shader is what consumes them.
  `VOLUMETRIC_FOG` in `map_view_3d.gd` is the one switch that shows them early, and
  it is off because a fog volume in a flat world is bounded by shadow casters rather
  than by CDDA's rays — so a lamp behind a wall glows through it, which is the exact
  class of lie ADR-003 refused for field of view.

While in there: `CAM_Z` came down from 4096 to 512. It was arbitrary, and the reason
to care is that a volumetric fog volume is a fixed length of frustum measured from
the camera — with the world a thousand units beyond it, a light inside it lights
nothing.

### What landed: 3D-3, the world stands up (2026-08-17)

`TILT_DEGREES` in `map_view_3d.gd`, **0.0 by default**, with `set_tilt_degrees()` to
drive it. Above zero: ground sprites become horizontal quads, standing sprites become
vertical ones, the camera pitches down by the tilt, and each axis is pre-divided by its
own factor -- `1/sin` along the rows, `1/cos` upward -- so the tilt cancels and the
artist's pixels come back.

**The arithmetic is verified, and that turned out not to need light after all.** The
section below says the tilt could not be judged before a lit shader existed, and that
is true of whether it *looks* right. It is not true of whether the geometry is right,
because that is arithmetic: place a sprite, project the quad's corners back through the
camera with `unproject_position`, and see whether the rectangle comes home. No GPU, no
screenshot, no game. `res://scenes/geometry_check.tscn` does exactly that for a floor, a
road, a wall, a tree and a creature across 0°, 30°, 45°, 51°, 60° and 75°, and all
thirty placements land **0.00 px** from where the 2D backend draws them.

That gate took three attempts, and the two failures are the more instructive part:

1. **It compared against raw map pixels** and reported every case 17.89 px out — the
   same 17.89 px at every tilt, which is the signature of a wrong expectation rather
   than a wrong subject. MapView centres the published block in the drawable area, so a
   map wider than the viewport starts at a negative offset; the claim is "where the 2D
   backend would have drawn it", not "at its map coordinates".
2. **It could not drive the tilt.** `_update_camera` recomputed the angle from the
   constant, overwriting what the check had injected, so all thirty cases ran the *flat*
   path and thirty green lines meant the tilt had never been tested once. A 5% error
   deliberately injected into the ground stretch was still reported as 0.00 px out. The
   fix made the tilt state with the constant as its default — which is also what allows
   trying an angle on a running game — and the same injected 5% error now fails the
   floor cases at every tilt above zero and leaves the standing ones green, while a 5%
   error in the height stretch does the opposite. A gate that cannot fail in the shape
   of the bug is not a gate.

Three things the stood-up world changes for the better on its own, without any light:

- **Contact shadows lose both their fudges.** `FLATTEN` existed to fake the
  foreshortening of a view that is not straight down, and the projection now does it
  for real; `LIFT` existed because a blob centred on the bottom of a sprite's cell sits
  below where the feet read, and the anchor of a standing sprite *is* its feet. A round
  blob on the floor, and the camera flattens it.
- **Depth stops being a ranking and becomes a position.** Two rows are a tile apart
  along the ground. The rank survives only as a nudge along the camera's own axis --
  the one direction an orthographic projection cannot see -- to order sprites within a
  row.
- **Smoke drifts along the ground.** Wind's screen-space y was a vertical in the flat
  world and is a ground direction here, which is what it always meant.

What it costs while it is on: the fallback glyphs and the animation overlay are hidden,
because a `CanvasLayer` transform can reproduce an unrotated camera and nothing else.
3D-1c predicted this and named the fix. Combat text goes with them, so the tilt is an
experiment and not yet a setting.

### What landed: the lit shader, and the end of the coupled unit (2026-08-17)

`map_tiles_3d.gdshader` stops being `unshaded`. The rule it follows is ADR-003's,
transposed from field of view to light:

- **What the shader already computed goes to `EMISSION`** -- sprite, tint, CDDA's
  per-tile light, memory, the grade. So a sprite with no light near it comes out exactly
  as it did before, which is what keeps the baseline intact.
- **Engine lights add a directional term in `light()`**, multiplied by the same per-tile
  light amount. CDDA keeps saying *which* tiles are lit and how much; the renderer only
  says what that looks like from a direction. A lamp on the far side of a wall cannot
  brighten the room it is not in, because the mask is zero there -- a shadow map knows
  about geometry and CDDA knows about walls.
- **Engine light is off in the flat world**, sun and points alike. With every normal
  facing the camera a light can only add a uniform wash, which would move the flat
  backend off the baseline it exists to match. So the lights turn on with the tilt.

Two things the shader gate caught that reading would not have: `return` is rejected
outright inside `light()` (the exemptions became a factor instead of an early exit), and
the quad had no normals at all -- a lit material with no `ARRAY_NORMAL` is a lit material
with nothing to light. The normals need no per-case handling, which is the reward for
having chosen the placement bases deliberately: the quad faces +Z in its own space, and
`_to_world_tilted` gives a ground quad a third column of +Y and a standing one +Z, so a
floor points up and a wall points at the viewer for free.

**Cast shadows** are on for standing sprites while tilted, double-sided (a single quad
that only casts from its front face vanishes when the light moves behind it), and the
batch key regains `tall` for exactly this reason -- casting is a property of the node,
and a ground quad that casts is a quad shadowing the floor it lies on.

**The sun is the one invented number in this backend, and it is marked as such.**
Elevation follows the published `daylight`; the bearing is a constant, because the real
answer is `sun_azimuth_altitude( time_point )` and publishing it is a one-line addition
to the conditions channel that belongs with the next C++ change rather than with a
second rebuild on the same day. ADR-006 argued against generated normal maps on the
grounds that fabricating what the game already knows is the wrong trade; this is that
trade, taken knowingly, in one constant, and reversible in two lines.

### The tilt experiment's answer (2026-08-17)

**The angle looks fine.** Reported by the first person to run the stood-up world on a
real GPU, which is the only place that judgement could ever have come from.

That is ADR-006's central question answered in the affirmative: UltimateCataclysm's
fixed-angle art *can* carry a stood-up world, so the ground/standing split taken from
`cmd_flag_tall` is a good enough reading of which sprites are floor and which are
things. Part 4 is therefore worth continuing, and the option-A fallback -- stay in 2D
with `Light2D` and generated normal maps -- is retired rather than pending.

Two caveats on the strength of that answer. It was made at one tilt, on one scene, in
daylight; walls and trees agreeing there does not promise that a table top does. And a
regression was noticed in the same session -- character overlays missing, so the avatar
draws naked -- which has not yet been attributed to this backend or to the game. It
matters which: the 3D path dropping a layer would be the first thing here to lose
content rather than to move it. The render overlay's `BY LAYER` counts settle it in one
look, and the answer belongs in this section when it arrives.

### What landed: 3D-4, levels below get a floor of their own (2026-08-17)

`LEVEL_DROP_TILES` in `map_view_3d.gd`: while tilted, each level below the avatar sits
two tiles of height beneath the one above it, so a hole is a hole rather than a dimmed
tint at the same height. Flat, levels stay coplanar -- that is the 2D backend's
behaviour and the baseline.

Two tiles because the art already says so. ADR-005 found the tileset declares no height
of its own (`zlevel_height` is 0, `height_3d` is on no tile), so the number is ours, and
Ultica draws a wall as a 64 px sprite in a 32 px cell -- one storey, two tiles. Taking it
from what the art does is the least invented answer available.

The projection turns that into an exactly checkable claim, and `geometry_check.tscn`
checks it: **a level one down lands `LEVEL_DROP_TILES * tile_height` pixels further down
the screen, the same number at every tilt, and moves sideways not at all.** The
tilt-independence is the part that matters -- the drop is a height, so it is pre-stretched
by `1/cos` like every other height here, and dropping that division makes the fall
64 px at 0°, 45 px at 45° and 32 px at 60°. That is what the negative test produced when
the division was removed, and what the gate now refuses.

What it does **not** fix, and cannot without C++:

- **`fog_for_depth` is still in the tint.** Levels below are dimmed by the game before
  they reach the renderer, so adding `Environment` fog now would dim them twice.
  Retiring the C++ fog in favour of real distance fog is one edit and belongs with the
  next rebuild.
- **Lower levels still receive no engine light.** The light texture holds one texel per
  *column*, belonging to the tile the avatar can see, so applying it a storey down would
  light a basement with the daylight falling on the roof above it. A per-level light
  texture is the fix and it is a C++ change.
- **Nobody has looked down a hole.** Everything above is geometry and arithmetic. The
  backlog has been waiting since ADR-005 item 1 for someone standing at the top of a
  staircase, and it still is.

### What landed: the four things the light channel was missing (2026-08-17, API 21)

**The sun's bearing is the game's now.** `sun_azimuth_altitude()` on the conditions
channel, two floats, and `SUN_AZIMUTH_DEGREES` is gone. Altitude is published as well as
`daylight` because they are different facts: an overcast noon has a high sun and little
light, and "is there a sun at all" should be the game's answer rather than inferred from
brightness.

**The sources that bypass the buffer are tapped where the snapshot already had them.**
What the avatar is carrying (`Character::active_light`), what an NPC is carrying, what a
glowing monster emits (`mtype::luminance`), and vehicle headlights. The channel is nine
floats now: a bearing and a cone width joined it, and a cone of zero means a lamp. That is
the whole difference between an `OmniLight3D` and a `SpotLight3D`, and `apply_light_arc`
had both numbers already -- though note its `wideangle` is the *full* width and Godot's
`spot_angle` is the half, which is one of the few places where taking the game's number at
face value would have been wrong.

The vehicle pass is the one duplicated rule in the channel, and it is marked as such: an
arc goes straight into the lightmap and is never buffered, so there is no result to read
and the loop from `map::generate_lightmap` is mirrored instead -- including its two passes,
where the first sums what a vehicle's cone lights come to with each further lamp counting
for a little less. If headlights ever stop agreeing with what the map says is lit, that is
where to look.

**`fog_for_depth` moved into the shader.** Same handshake shape as the light pass: the
renderer says it fades lower levels itself, and C++ stops baking the fade into their
tints. The constants live in `level_fade` now, which means they can be turned on a running
game (VER-1) and that a backend which fades levels *and* receives pre-faded tints can no
longer dim a basement twice. A host that never announces keeps the baked fog, so the 2D
backend does not notice this exists.

**The light texture holds one block of rows per z-level.** It held one texel per column,
which is why a level below had to sit out the light pass entirely -- the texel above it
belonged to the tile the avatar could see, and applying it a storey down would light a
cellar with the daylight falling on the roof. Now each level has its own block, deepest
last, only the levels reached are published, and the fire blur runs per block so a fire
upstairs is not a glow on the ceiling below. Lower levels are in the pass, and the only
light exemption left is the avatar's own layers.

That last one caught a trap on the way out: **the 2D backend samples the same texture.**
Its UVs mapped one block over the whole image, so the moment a hole came into view its
lighting would have been silently squashed. It scales V into block 0 now. A change to a
shared channel is a change to every consumer of it, and the other consumer here is the
backend nobody was looking at.

### The floating characters (2026-08-17)

Reported after the tilt became the default: characters read as hovering, with their contact
blobs well below their feet. Two mistakes in one block, compounding.

**The blob reconstructed its height instead of taking the anchor's.** Every sprite is nudged
along the camera's own axis by its depth rank, and that nudge is invisible *because* its y
and z halves cancel under an orthographic projection. The blob took z from the anchor and y
from the level's floor — keeping one half and dropping the other, so the cancellation broke
and the blob slid down the screen by the whole nudge. This is the second bug caused by that
one line: it also displaced the tilted light UV, which the earlier fix caught. A quantity
that is invisible on screen is not harmless; it is invisible.

**And LIFT was dropped on a wrong reading.** The claim in the comment was that "the anchor of
a standing sprite *is* its feet". It is not: it is the bottom edge of the sprite's *quad*, and
the artist drew the contact point some way above that — which is the entire reason the 2D
backend has the constant. Stood up, "above the feet on screen" is "away from the camera along
the ground", so it converts by `1/sin` like every other row distance. FLATTEN staying gone was
right for the reason given; LIFT going with it was not.

Both are now one function with a gate case: the blob's projected centre must sit directly
above the sprite's projected base, `LIFT * tile_height` pixels up the screen, at every tilt
and in both worlds. Restoring the old placement fails it at 6.52 px, which is what "floating"
measured.

### What this leaves, and a correction to the plan above

**The tilt experiment cannot be judged on its own, and the plan's ordering assumed
it could.** The pre-stretch that makes option B pixel-faithful is precisely a
guarantee that standing the world up *changes nothing visible*: a ground quad
scaled by `1/sin φ` projects back to the square the artist drew, and a facade
scaled by `1/cos φ` projects back to its original height. Under an `unshaded`
shader the only differences left are occlusion and where a sprite lands if the
ground-versus-standing split gets it wrong.

So the thing 3D-3 was supposed to answer — whether one φ can serve trees, walls
and table tops — is only visible once light falls across the geometry. The
experiment is therefore not the cheap item this ADR listed. The coupled unit is:

1. **the light channel** — `level_cache::light_source_buffer` published as point
   lights (3D-2). C++, so it needs a rebuild, and it is the one part that is
   independently useful: `PointLight2D` for the 2D backend is on ADR-003's flourish
   list and has never been built either;
2. **the stood-up geometry** — φ as a parameter defaulting to flat, ground quads
   horizontal and standing quads vertical, in a coordinate frame where row runs
   along a third axis rather than up the screen. This is a real refactor of the
   placement path, the light uniforms, the shadows and the particles;
3. **a lit shader and one shadow-casting light**, without which 1 and 2 are
   computed and discarded — the exact failure this branch keeps recording.

None of it can be sliced into something judgeable earlier, which is worth saying
plainly because every step so far could be. Whoever starts it should expect the
first useful screenshot at the end of all three.
