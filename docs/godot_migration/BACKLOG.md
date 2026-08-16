# Backlog — sprites and the remaining menus

**Audience:** agents continuing `godot-mig`. Read [`AGENT_HANDOFF.md`](AGENT_HANDOFF.md) first.

**Status:** Part 1 (sprites) is done: SP-1…SP-10, plus character overlays.
Part 2 (menus) has MENU-1, MENU-3 and MENU-4 done and six left. Part 3
(verification) is new and mostly open — read VER-1 before doing more rendering
work.

Every task below is verifiable without a display, though not all by the same
route — see Part 3 for what each one can and cannot answer:

```bash
godot --headless --path godot res://scenes/headless_probe.tscn
godot --headless --path godot res://scenes/shader_check.tscn   # shaders only
```

And you *can* see the result — this box has Xvfb and Mesa lavapipe:

```bash
xvfb-run -a -s "-screen 0 1600x900x24" \
  env VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  godot --rendering-driver vulkan --path godot \
        res://scenes/headless_probe.tscn -- --screenshot /tmp/ui.png
```

That writes one PNG per subject, including each panel on its own via
`_shoot_solo`. Use it for anything visual: the numeric checks cannot tell you a
row is clipped, and the sidebar shipped clipped for two rounds because of it.

Sizes are relative (S/M/L), not hour estimates.

---

## Part 1 — The sprite pipeline document

`Sprite Pipeline.dc.html` (in the design zip) proposes an art + rendering
pipeline. Its own advice — build the renderer effects first, because they need
no art decisions — is right here, and for a reason it could not have known:
those effects map onto gaps this port actually has.

### State of the code the verdicts were written against

Kept because the verdicts below only make sense against it. All four have since
been addressed — see the sprite task table.

- Lighting was a **flat per-sprite tint**. `tint_for_light()` wrote one colour
  per draw command. There was no `Light2D`, `CanvasModulate` or visibility
  texture anywhere in the project.
- Memory tiles got a **constant tint**, not a pass.
- Creatures are **inside the batched draw list** as ordinary tiles
  (`map_layer::monster`, `map_layer::player`), so nothing tweened independently.
  They still are; the reaction moves the instance rather than the node.
- `looks_like` resolution **already worked** — the fallback chain was started
  but stopped at step 2 of 5.

### Verdicts

| Section | Verdict | Why |
|---|---|---|
| §3 Motion without frames | **Fits — do first** | Targets real gaps, needs no art decisions. The memory pass and light pass are the two biggest available wins. |
| §7 Fallback chain + coverage report | **Fits** | Steps 3–5 missing; a miss currently draws nothing. The coverage report turns "thousands of ids" into a list ordered by what the player sees. |
| §8 Palette swaps, debug overlay | **Fits** | Cheap, slot into the existing tile shader. |
| §8 "Creatures are Node2Ds with y_sort" | **Adapt** | Goal right, mechanism heavy. A `MultiMesh` instance carries its own transform, so a lunge is a per-instance offset — no need to give up batching. |
| §4 Paperdoll slot matrix | **Adapt** | CDDA already ships a slot model in `overlay_ordering.cpp` plus the `worn_`/`wielded_` sprite convention. Read that rather than inventing a parallel z-list. |
| §8 "Terrain and furniture go in TileMapLayers" | **Does not fit** | Which sprite a tile draws depends on map memory, connected-terrain masks, overrides and lighting that C++ computes per turn. A TileMapLayer would need the tileset and that resolution logic mirrored into Godot. The draw-list → `MultiMeshInstance2D` path is already batched and already correct. **Do not attempt.** |
| §5 Blender 3D → sprite pipeline | **Defer** | Coherent, and probably right if the project ever commits to producing its own art. But it is an art-production programme, not a rendering task, and nothing is blocked on it — Ultica supplies the sprites. Revisit when there is art capacity. |

### Two claims to check before following the document

- **"Never frame-animate terrain"** conflicts with CDDA tilesets, which already
  ship multi-frame animated tiles. The rule is right for *new* art; do not
  enforce it against the tileset the game loads today.
- **"Every atlas is a regenerable build artifact"** was not true here, and now
  is: `build-scripts/compose-tileset.sh` composes `gfx/` from upstream source
  art (SP-10). `gfx/` stays gitignored; the script is the record of how to
  rebuild it.

### Sprite tasks

**All ten are done** (2026-08-15). What each turned into, and what it left open:

| Id | Landed as | Notes for whoever touches it next |
|---|---|---|
| SP-1 | Five-step chain in `resolve_sprite`, `src/godot_map_snapshot.cpp` | Order follows `cata_tiles::draw_from_id_string` so a miss looks the same as under SDL: id → `looks_like` → the tileset's ASCII sprite → `unknown_<category>` → a font glyph. The glyph step is the only new one; SDL stops at a question mark. Resolution is now category-aware, and `looks_like` covers items, vehicle parts and fields, which it did not before. |
| SP-2 | `get_sprite_coverage(limit)` | Counts by draw command, not by distinct id, so the ordering is "what is the player looking at". Accumulates per session and resets with the tileset. |
| SP-3 | `src/godot_light_snapshot.*`, R channel | Nearest-sampled, deliberately: memory is per-tile and must not bleed. Replaces the `lit_level::MEMORIZED` colour key. |
| SP-4 | Same texture, G channel | Linear-sampled. The handshake matters: C++ keeps baking light into tints until `set_light_pass_enabled(true)`, so a host that never builds the texture still gets a lit map. Do not remove that path. |
| SP-5 | `AnimSnapshot::add_hit`, `MapView.hit_response` | Creatures stay in the batch; the reaction is a per-instance offset. `draw_hit_player` has no attacker to point at, so the avatar gets a straight recoil. |
| SP-6 | `field_particles.gd`, B channel of the light texture | Chosen from `field_type::has_fire` / `phase == GAS`, not an id list. The field *sprite* still draws underneath. The flicker is in the shader because the game's lightmap only updates per turn. |
| SP-7 | `cmd_flag_sway`, vertex displacement | Two conditions, both required: the id is vegetation (flags TREE/YOUNG/SHRUB/FLOWER/PLANT, plus five id prefixes) **and** its sprite overhangs its tile cell. The second is not optional — shearing a sprite that exactly fills its cell tears it away from its neighbours, which is what a field of grass did before the check existed. Wind comes from the weather manager via the light snapshot. |
| SP-8 | `data/godot/palettes.json`, `godot_tileset_loader::load_palettes` | Thirteen fungal zombie variants redirect onto the sprite they are a variant of. A tileset can override with its own `godot_palettes.json`. |
| SP-9 | `debug_overlay.gd` (F3), `CDDAHost::describe_sprite` | `describe_sprite` is also how SP-7 and SP-8 are testable at all without a display. |
| SP-10 | `build-scripts/compose-tileset.sh` | Sparse blobless clone + `tools/gfx_tools/compose.py`. Note it must copy `tileset.txt` across itself; compose.py does not, and without it CDDA cannot see the tileset. |

### Character overlays (the §4 "Adapt" verdict)

Done. Ultica ships about 3,400 `overlay_*` sprites and the renderer drew none of
them: the avatar was a bare body, and NPCs were not drawn at all because the
creature loop only handled the monster cast.

`Character::get_overlay_ids()` already returns the whole slot model in draw
order — effects, mutations by `overlay_ordering`, bionics, worn items innermost
first, then the weapon — so the §4 warning against inventing a parallel z-list
was right and there was nothing to invent. What was needed was the id convention
(`overlay_[male_|female_][worn_|wielded_]<id>[_var_<variant>]`, each step
falling back), ported from `cata_tiles::find_overlay_looks_like` so every
existing tileset's art fits.

Two structural consequences worth knowing:

- **Characters take two layers**, `player`/`player_overlay` and
  `monster`/`monster_overlay`, because MapView batches by atlas and Ultica
  spreads overlays over five sheets — two sprites on one layer from different
  atlases had no defined order between them. `map_layer::player` moved 8 → 9.
- **Within a layer, batches are re-seated in draw-list order** each rebuild
  (`move_child`), which is what makes clothing z-order hold across atlases. The
  old code called cross-atlas ordering "not significant"; with overlays it is.

Still missing: `override_look` mutations — the handful that replace the whole
body sprite — draw as an ordinary body plus overlays, which is what they did
before.

Sway and palette ride in spare bits of `map_draw_cmd::rot_flags` (see `enum
cmd_flag`) and become part of MapView's batch key. `INSTANCE_CUSTOM` is full and
the stride is a contract with `map_view.gd`, so a new per-tile effect should
take the next flag bit rather than widen the command.

### Follow-up: ground foliage does not move

Plain `t_grass` is a 32x32 ground cell and is now excluded from sway, so a lawn
is static; `t_grass_tall`, `t_grass_long` and the trees still move. Getting
motion out of seamless ground needs a different technique — scrolling UVs with a
phase shared across the whole field, so the texture never leaves its cell and no
seam can open. That only works on genuinely tileable art, and whether Ultica's
grass qualifies has not been checked. Do not solve it by shearing.

### What the sprite work left open

- **Motion has been checked, not judged.** Still frames rendered under Xvfb with
  Mesa's software Vulkan (recipe in `AGENT_HANDOFF.md`) confirmed the map draws
  and the light gradient falls off correctly around a lit doorway, and a
  two-frame diff confirms nothing moves that should not. Whether the motion
  *looks* right is still unknown: `sway_amount`, `sway_speed` and the particle
  `scale`/`lifetime` values are first guesses. The first sway attempt shipped a
  bug that only a person watching the game found, so treat this area as
  under-verified until someone has.
- **Memory has not been observed.** The probe never leaves the starting shelter,
  so no tile has gone out of view and come back as memory. The channel and the
  shader path are exercised; the look is not.
- **Shaders are only parse-checked.** `res://scenes/shader_check.tscn` catches
  language-level errors headlessly (the dummy driver still runs Godot's shader
  front end) but cannot catch a GPU backend rejecting valid code.
- **§5 of the pipeline document is still deferred.** SP-10 removed its
  prerequisite — atlases are now regenerable from source — but it remains an
  art-production programme, not a rendering task.

---

## Part 2 — Menus

`uilist` now renders as a Godot Control (`src/godot_uilist_snapshot.*`,
`godot/scripts/uilist_panel.gd`), which covered most of the surface in one move.
What remains is two named lists and nothing open-ended. When both reach zero,
`terminal_view.gd` and the whole ImTui cell-buffer path can be deleted.

| Remaining surface | Count | Why it still uses the overlay |
|---|---|---|
| `cataimgui::window` subclasses | 22 | Bespoke layouts; each needs its own Godot panel |
| uilists with a callback | 26 | Callback draws extra panes and intercepts keys |
| uilists with category tabs | 7 | Second navigation axis the panel has no model for |

Counts come from `grep -c "public cataimgui::window"` and the callback/category
scans over `src/*.cpp`; re-run them rather than trusting these numbers later.

**Order by traffic, not by size.** `query_popup` is one class and every yes/no
prompt in the game, including the one shown during an autosave. It is both the
highest-traffic item and among the smallest.

### Menu tasks

| Id | Size | Task | Files | Done when |
|---|---|---|---|---|
| MENU-1 | S | `query_popup` as a Godot panel — same snapshot pattern and attend-timeout fallback as the uilist takeover | `src/popup.cpp`, new `godot_popup_snapshot.*`, new `popup_panel.gd` | A save prompt draws with `occupied=0` in the overlay |
| MENU-2 | S | `input_popup` as a Godot panel (single-line text entry) | `src/input_popup.*`, new `text_prompt_panel.gd` | Naming a world uses no overlay (after MENU-1) |
| MENU-3 | M | Item info: `iteminfo_window` and `extended_description_window` — mostly formatted text | `src/ui_iteminfo.*`, `ui_extended_description.*` | Examining an item never touches the overlay |
| ~~MENU-4~~ | M | **Done.** uilist callbacks — see below; the original plan here was wrong | `src/uilist.h`, `veh_utils.cpp`, `godot_uilist_snapshot.cpp` | `pointmenu_cb` menus (20 call sites) render in Godot |
| ~~MENU-5~~ | S | ~~uilist category tabs~~ **done** (3fa74f947a) — near-zero reach, see the commit body: tab strip in the panel, publish the category list | `godot_uilist_snapshot.*`, `uilist_panel.gd` | `can_take_over` no longer tests for categories |
| ~~MENU-6a~~ | L | ~~Crafting screen~~ **done** — the panel drives the existing `crafting_ui_impl` by sending back its own action strings and pending-click intents, rather than reimplementing the state machine. Tabs, subtabs, list with availability colouring, filter, batch, detail pane | `crafting_gui.cpp`, `godot_crafting_snapshot.*`, `crafting_panel.gd`, `color_tags.gd` | Crafting opens as a Godot panel |
| MENU-6c | M | The interactive step/variant table for recipes with selectable steps. The Godot detail pane currently *names* the steps and says they cannot be chosen there yet, rather than silently showing a shorter recipe than the one the player will get | `crafting_gui.cpp` (`draw_modifier_table`) | Step recipes are fully usable from the Godot screen |
| MENU-6d | S | Unread-recipe highlighting (the `+` markers and "unread first"), and the info-nav mode that lets the detail pane be navigated component by component | `crafting_gui.cpp` | Parity with the ImGui pane's remaining affordances |
| ~~MENU-7a~~ | M | ~~Options screen~~ **done** — `show()` split into `show_legacy()` plus a shared epilogue; panel edits the real `cOpt`s and reads back. `world_options_only` (worldgen-embedded) deliberately stays legacy | `src/options.*`, `godot_options_snapshot.*`, `options_panel.gd` | 5 pages, 116 options, round trip verified |
| ~~MENU-7b~~ | M | ~~Keybindings~~ **done** — same split (`display_menu_legacy` + shared epilogue). Key capture goes out as a raw Godot event through the input bridge, so a new binding is the exact `input_event` the game later matches | `input_context.*`, `godot_keybind_snapshot.*`, `keybind_panel.gd` | 145 actions, bind/remove/reset/filter |
| MENU-8 | M | Advanced inventory — two-pane item mover | `src/advanced_inv*.cpp` | Moving items between panes needs no overlay (after MENU-4) |
| MENU-9 | S | Delete the overlay: `terminal_view.gd`, the ImTui blit, the ImGui cell layer, `USE_CURSES_UI_OVERLAY` | `terminal_view.gd`, `godot_view_snapshot.*`, `godot_curses_backend.cpp` | The cell buffer is gone (after everything above) |

### MENU-4, and what it did not cover

The plan above — "give the snapshot a callback text field so pane-drawing
callbacks join the Godot path" — was written before reading the callbacks and is
wrong. `refresh()` implementations issue **direct ImGui draw calls**
(`ImGui::Text`, `TableSetColumnIndex`, even `draw_overmap_chunk_imgui`). There is
no text to hand over.

Classifying all 15 `uilist_callback` subclasses:

| Kind | Count | Overrides | Can Godot run it? |
|---|---|---|---|
| draws | 10 | `refresh`, `desired_extra_space_*` | No — immediate-mode ImGui |
| keys | 3 | `key` | Not yet — the panel forwards no keys to C++ |
| simple | 2 | `select` only | **Yes** |

The two simple ones are the win, because one is `pointmenu_cb` with **20 call
sites**, and both only move `view_offset` to preview the highlighted point.

What landed: `uilist_callback::needs_own_ui()`, defaulting to **true** so an
unchecked callback stays on the C++ path, overridden false by `pointmenu_cb` and
`veh_menu_cb`. `can_take_over` admits a callback that returns false, and the
Godot uilist loop runs `select()` and `confirm()` on the game thread — then
republishes the map snapshot, because the game thread is parked in that loop
rather than at the input wait where the idle refresh runs, and without it the
camera preview would move and never be seen.

Left for a follow-up task:

- **The 10 drawing callbacks** need a per-callback text or structured contract,
  and some (the overmap chunk render in `magic_teleporter_list`, the mutation
  preview in `wish.cpp`) genuinely cannot be text. Treat them as individual
  screens, not as one task.
- **The 3 key-intercepting callbacks** need the panel to forward unhandled keys
  to C++, which the channel does not do yet.
- **Nobody has driven a `pointmenu_cb` menu at runtime.** The admission is
  compile-verified and the loop is the same one proven for callback-free menus,
  but every call site needs a game situation the probe cannot reach (debug
  overlays, NPC dialogue, specific item use). First person who can reach one:
  check that the camera preview actually follows the highlight.

### Pattern to follow

`src/godot_uilist_snapshot.*` is the worked example for all of MENU-1…3. The
shape is: the game thread blocks where it always did, publishes state to a
snapshot, and polls for an answer. Two rules it must keep:

- **Shutdown is checked first** in the wait loop, or a quitting host leaves the
  game thread running into torn-down code.
- **An attend timeout.** If no panel picks the work up within ~1.5s, hand it back
  and let the legacy path run. A game thread waiting forever on an answer nobody
  will give is the failure this whole migration has been chasing; a host that is
  not running the panel must not be able to cause it.

---

## Part 3 — Verification

Most of the renderer was built on a machine with no display. That is workable
further than it sounds — see below — but it has a hard edge, and the SP-7 sway
bug found it: grass tore itself apart in a way every numeric check passed. This
part is what closes that gap, split into what only a person can do and what
could be built so that less of it needs a person.

### VER-1 — The tuning pass (a person, one sitting, no code)

Everything below is a constant somewhere, set by guess, that has never been
looked at. None of it needs a rebuild to evaluate — the shader uniforms can be
edited live in the Godot inspector on a running game, and the C++ ones need one
`make GODOT=1` each.

Run the game, press **F3** for the render overlay, and judge:

| What | Where | Set to | Question to answer |
|---|---|---|---|
| Night floor colour | `map_tiles.gdshader` `dark_color` | `0.17, 0.18, 0.26` | Is an unlit room readable without looking lit? |
| Memory tint | `memory_color`, `memory_saturation` | `0.34,0.36,0.46`, `0.18` | Does remembered ground read as memory at a glance, next to seen ground? |
| Light bands | `encode_light_level` in `godot_light_snapshot.cpp` | LIT 0.88, LOW 0.30, DARK 0.10 | Does a lantern's edge fall off smoothly, and is a LOW tile clearly dimmer than a LIT one? |
| Sway | `sway_amount` 0.09, `sway_speed` 1.6 | — | Do trees read as wind rather than as wobble? Does anything tear? (see the SP-7 note) |
| Fire flicker | `fire_color`, `fire_strength` 0.9 | — | Does a campfire light the room, or strobe it? |
| Particles | `field_particles.gd` `scale_min/max`, `lifetime`, `amount` | — | Is smoke smoke, or a cloud of dots? |
| Hit reaction | `map_view.gd` `HIT_DURATION` 0.22, `HIT_OFFSET` 0.34 | — | Does a melee hit read as a blow, or as a glitch? |

Two things specifically **never observed at all**, because the probe never
leaves the starting shelter: map memory (walk away from a lit room and look
back) and a lantern gradient at night (drop a light source in the dark).

Please write findings back into this table rather than into a commit message —
the next person needs the numbers, not the history.

### VER-0 — Assert that snapshots are *consumed*, not just published

**Five lines, and it would have caught three shipped-broken features.** The probe
checks that producers fill snapshots and never that anything draws them, so a
feature wired to the dead curses frame boundary looks identical to a working one.
See "The dead frame boundary" in AGENT_HANDOFF.md. Fail the probe when a
generation counter that should move across a session stays at zero;
`get_anim_stats()` already exposes what it needs.

### VER-2 — Tooling that would move work off the person

Ranked by how much verification each unlocks per unit of effort. None exist yet.

1. **A scripted scenario harness.** The single biggest limitation is that every
   check happens at a fresh evac-shelter spawn: no grass, no night, no fire, no
   monsters, nothing remembered. A debug hook that can place the avatar at a
   given overmap location, set the time of day, spawn a monster or a fire, and
   *then* run the probe would make sway, memory, lighting, particles and hit
   reactions all directly checkable. `debug_menu.cpp` already has teleport,
   time-set and spawn commands; the work is exposing a few of them through
   `godot_game_commands.*` and driving them from the probe. **This is the one to
   build first** — everything else in this list is worth less without it.
2. **Screenshot comparison against stored baselines.** The Xvfb + lavapipe route
   (see `AGENT_HANDOFF.md`) renders real frames; what is missing is remembering
   what they looked like last time. Commit a handful of reference PNGs, compare
   with a tolerance, and fail on drift. This turns "does it still look right"
   from a person's job into a diff, and would have caught the sway tearing.
3. **A frame-motion assertion.** The probe already shoots two frames 0.45s apart
   and can diff them. Give it an expected answer — "nothing on the terrain
   layers may differ between frames unless a sway or particle command exists" --
   and the tearing class of bug fails the run rather than needing an eye.
4. **Live uniform editing without a rebuild.** Expose the tuning constants above
   as `@export` on a Godot resource rather than shader defaults and GDScript
   constants, so VER-1 becomes one sitting with a slider instead of a
   build-run-look loop. Cheap, and it makes VER-1 something a non-programmer can
   do.
5. **A tile-inspector under the cursor.** `describe_sprite()` already answers
   "why did this tile draw what it drew" for an id typed by hand. Wiring it to
   the moused-over tile makes the debug overlay answer it for the thing the
   person is actually looking at.

### What each verification route can and cannot answer

| Route | Answers | Blind to |
|---|---|---|
| `headless_probe.tscn` | What C++ published, what MapView built, whether panels and scripts run | Anything about pixels |
| `shader_check.tscn` | Shader language errors | GPU backend rejections; whether the shader is *right* |
| Xvfb + lavapipe screenshot | Layout, colour, whether something is on screen at all | Motion, feel, frame pacing |
| Two-frame diff | What moves that should not | Whether motion that should happen does |
| A person playing | Everything above, and only this | Nothing — but it does not scale, and it is the scarce resource |
