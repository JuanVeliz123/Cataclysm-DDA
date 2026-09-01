# Backlog — sprites and the remaining menus

**Audience:** agents continuing `godot-mig`. Read [`AGENT_HANDOFF.md`](AGENT_HANDOFF.md) first.

**Status:** Part 1 (sprites) is done: SP-1…SP-10, plus character overlays.
Part 2 (menus): MENU-1…8 and MENU-10…13 (all of them, as of 2026-08-30) are
done. **MENU-14 is now done too** (`help` landed 2026-08-31, the last of its
six screens). What remains is `end_screen` and `compare_item_menu` (the last
two rows of MENU-13's screen table) and MENU-9, gated on all of them -- which
MENU-14 finishing now unblocks. Part 3
(verification) is new and mostly open — read VER-1 before doing more rendering
work. Part 4 (the 3D backend) is a **proposal**, not a queue: read ADR-006
before taking anything out of it, and note that its own third milestone is
allowed to cancel the rest.

Every task below is verifiable without a display, though not all by the same
route — see Part 3 for what each one can and cannot answer:

```bash
./build-scripts/check-godot-scripts.sh                         # compile gate, first
godot --headless --path godot res://scenes/headless_probe.tscn
godot --headless --path godot res://scenes/shader_check.tscn   # shaders only
godot --headless --path godot res://scenes/geometry_check.tscn # 3D placement only
```

Run the compile gate **first**. It is the only one of the three that needs neither
the GDExtension nor a GPU, and it catches the failure the other two report as
something else: a script that does not compile leaves its node scriptless, so every
`has_method()` guard around it skips silently and the symptom is a blank map or a
stage that "never ran".

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

### Follow-up: particles are not graded

The presentation grade lives in the tile shader, because a full-screen
`CanvasLayer` would tint the sidebar and menus too — MapView sits at z 0 with its
animation overlay at 32 while the panels run 8 to 18. The consequence is that
`field_particles.gd` nodes have their own materials and are not graded, so smoke
stays its own colour at night and in rain. Pushing the same modulate onto those
nodes is the fix; the alternative, moving the world into a `SubViewport` so a
real full-screen pass becomes possible, is the larger and probably better answer
and would also fix the animation overlay drawing over the sidebar.

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
- ~~**Memory has not been observed.**~~ **It could not be: it did not exist**
  (found and fixed 2026-08-18 -- see `CHANGELOG.md`, the recurring bug's eighth
  instance). The write half and the region preparation both lived only in the
  SDL/curses draw paths. The snapshot walk now prepares and memorizes; the
  scenario probe walks out of a house and counts remembered texels, and its
  `09_house_memory` still shows the tint. Whether the tint reads *well* is a
  VER-1 question (first finding: too dark).
- **Shaders are only parse-checked.** `res://scenes/shader_check.tscn` catches
  language-level errors headlessly (the dummy driver still runs Godot's shader
  front end) but cannot catch a GPU backend rejecting valid code.
- **§5 of the pipeline document is still deferred.** SP-10 removed its
  prerequisite — atlases are now regenerable from source — but it remains an
  art-production programme, not a rendering task.
- **The occlusion fade has never been seen.** Depth ordering means a tree in
  front of the avatar now covers it, so something has to get out of the way.
  The tileset's own mechanism cannot: UltimateCataclysm declares no retracted
  offsets and ships no `_transparent` variants, so `retracted` and
  `transparent` are both structurally zero and will stay zero. The fallback
  dims the occluder's alpha instead, driven by the same `retract_at` value and
  therefore by the same `PREVENT_OCCLUSION` options. `min_occluder_alpha` (90)
  is a guess, and the probe's starting shelter has nothing tall standing
  between the avatar and the camera, so `faded` is zero there too — the counter
  proves the plumbing, not the policy. **Needs a forest and a person.**

- **A hole has now been looked down, barely.** The scenario probe stands the
  avatar on a staircase (`scenario_stand_on("GOES_DOWN")`) and `open_columns=1`:
  the walk descends, for the first time since ADR-005 item 1 landed. What one
  stair tile cannot show is the *look* -- 32 px of basement reads as a dark
  square, so `level_fade` and the two-tile drop are still unjudged. **Needs a
  rooftop lip or an open pit, and a person**; the render overlay's `open
  columns` / `levels below` numbers work as documented.

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
| ~~MENU-1~~ | S | ~~`query_popup` as a Godot panel~~ **done** — landed with the initial menu takeover work; this row was never struck through even though `godot_popup_snapshot.*` / `popup_panel.gd` have existed since the first squash commit. Confirmed done 2026-08-29 while auditing this file against the code | `src/popup.cpp`, `godot_popup_snapshot.*`, `popup_panel.gd` | A save prompt draws with `occupied=0` in the overlay |
| ~~MENU-2~~ | S | ~~`input_popup` as a Godot panel~~ **done** (1ca765ec47) | `src/input_popup.*`, new `text_prompt_panel.gd` | Naming a world uses no overlay (after MENU-1) |
| ~~MENU-3~~ | M | ~~Item info: `iteminfo_window` and `extended_description_window`~~ **done** — both route through the shared `godot_textwin_snapshot.*` / `textwin_panel.gd` read-only text window; this row was stale for the same reason MENU-1's was. Confirmed done 2026-08-29 | `src/ui_iteminfo.*`, `ui_extended_description.*`, `godot_textwin_snapshot.*`, `textwin_panel.gd` | Examining an item never touches the overlay |
| ~~MENU-4~~ | M | **Done.** uilist callbacks — see below; the original plan here was wrong | `src/uilist.h`, `veh_utils.cpp`, `godot_uilist_snapshot.cpp` | `pointmenu_cb` menus (20 call sites) render in Godot |
| ~~MENU-5~~ | S | ~~uilist category tabs~~ **done** (3fa74f947a) — near-zero reach, see the commit body: tab strip in the panel, publish the category list | `godot_uilist_snapshot.*`, `uilist_panel.gd` | `can_take_over` no longer tests for categories |
| ~~MENU-6a~~ | L | ~~Crafting screen~~ **done** — the panel drives the existing `crafting_ui_impl` by sending back its own action strings and pending-click intents, rather than reimplementing the state machine. Tabs, subtabs, list with availability colouring, filter, batch, detail pane | `crafting_gui.cpp`, `godot_crafting_snapshot.*`, `crafting_panel.gd`, `color_tags.gd` | Crafting opens as a Godot panel |
| MENU-6c | M | The interactive step/variant table for recipes with selectable steps. The Godot detail pane currently *names* the steps and says they cannot be chosen there yet, rather than silently showing a shorter recipe than the one the player will get | `crafting_gui.cpp` (`draw_modifier_table`) | Step recipes are fully usable from the Godot screen |
| MENU-6d | S | Unread-recipe highlighting (the `+` markers and "unread first"), and the info-nav mode that lets the detail pane be navigated component by component | `crafting_gui.cpp` | Parity with the ImGui pane's remaining affordances |
| ~~MENU-7a~~ | M | ~~Options screen~~ **done** — `show()` split into `show_legacy()` plus a shared epilogue; panel edits the real `cOpt`s and reads back. `world_options_only` (worldgen-embedded) deliberately stays legacy | `src/options.*`, `godot_options_snapshot.*`, `options_panel.gd` | 5 pages, 116 options, round trip verified |
| ~~MENU-7b~~ | M | ~~Keybindings~~ **done** — same split (`display_menu_legacy` + shared epilogue). Key capture goes out as a raw Godot event through the input bridge, so a new binding is the exact `input_event` the game later matches | `input_context.*`, `godot_keybind_snapshot.*`, `keybind_panel.gd` | 145 actions, bind/remove/reset/filter |
| ~~MENU-8~~ | M | ~~Advanced inventory — two-pane item mover~~ **done** (2026-08-24, API 32) — sized as M, ran closer to L: no `cataimgui::window` to hook, and no tab either -- both panes are always on screen, so `LEFT`/`RIGHT`/`TOGGLE_TAB` just move which one is `src`. `process_action()` is a real, pre-existing state machine keyed on the same action strings its own `input_context` produces (`"MOVE_SINGLE_ITEM"`, `"SORT"`, `"ITEMS_NW"`, ...) -- the channel is a pure pass-through to it, same shape as the crafting and uilist takeovers: nothing about move/sort/filter/examine logic was reimplemented. Several actions (`FILTER`, `SORT`, the three `MOVE_*`, `EXAMINE*`, `UNLOAD_CONTAINER`, `TOGGLE_VEH`, `SAVE_DEFAULT`) block synchronously inside `process_action()` on a nested legacy screen (an inline curses filter box, a uilist, the item-info window, an amount prompt), so the channel suspends around exactly those, same move `MedicalSnapshot`'s APPLY makes. `recalc_pane()` -- which actually repopulates each pane's item list -- only ever ran from inside the `ui_adaptor`'s `on_redraw` callback, so a naive port called `ui_manager::redraw_invalidated()` every iteration like the legacy loop does; that also painted the pane borders and item table into the curses windows this panel sits above, ghosting through the dim beneath it. `recalc_pane()` and `pane.fix_index()` are both public and pure data, so the fix calls them directly instead -- same fresh `pane.items`, nothing drawn. Read the dead-frame-boundary section in `AGENT_HANDOFF.md` before assuming a read-only screen like this one has no version of that trap. The area picker (15 `ITEMS_*` locations) and the row/column table are simplified re-presentations, not a pixel port of the curses ASCII layout -- the minimap, the 3x3 bracket grid and pagination are gone in favour of a scrolling list; category headers ride a per-row field instead of interleaved pseudo-rows so a click's index never has to be re-mapped | `src/advanced_inv*.cpp`, `godot_advanced_inv_snapshot.*`, `advanced_inv_panel.gd` | Moving items between panes needs no overlay (after MENU-4) |
| ~~MENU-10~~ | S | ~~`dialogue_imgui`~~ **done** (d98e4c5f87) — only the source of the action moved; the loop keeps its own re-verification, consequence prompts and trade-window hiding. `request_debug_spawn_npc()` makes it verifiable rather than lucky, and unblocks MENU-13 for the same reason | `dialogue_imgui.*`, `npctalk.cpp`, `godot_dialogue_snapshot.*`, `dialogue_panel.gd` | Verified against a live conversation, DOWN round trip included |
| ~~MENU-11~~ | M | ~~`overmap_sidebar`~~ **verified 2026-08-30, via a new `scenario_probe.gd` stage** — the code (`overmap_sidebar::record()`, `publish_sidebar()`, `overmap_sidebar_panel.gd`) had been in place since the pre-squash foundational work but was never exercised end to end; the backlog note asking for "an eyeball first" was really asking whether the publish, which rides the overmap loop's own `ui->on_redraw()`, actually reaches a live generation counter rather than being a dead-frame-boundary case like the pixel minimap/SCT/animation overlay were. It is not: opening the overmap ('m'), reading `get_overmap_sidebar()` (36 lines: tile info, settings, mission, quick reference, layers), then moving the cursor (→) and re-reading shows `overmap_sidebar_generation()` move (`1 -> 2`). **Minor, separate, unfixed:** one recorded line double-wraps a colour tag (`<color_c_brown><color_c_brown>field</color> ...`) -- cosmetic, worth a look next time this file is touched | `src/overmap_ui.*`, `godot_overmap_snapshot.*`, `overmap_sidebar_panel.gd`, `scenario_probe.gd` | The overmap screen is Godot end to end |
| ~~MENU-12~~ | M | ~~`surroundings_menu`~~ **observed running for the first time (2026-08-29).** The reason nine prior attempts (eight logged below, plus a ninth that misidentified the cause as an ambient "soliloquy window" -- see the retired SOLILOQUY-1 entry) never got past this screen: `publish_to_godot()` had a genuine infinite loop (`surroundings_menu.cpp`, fixed 2026-08-29), triggered every single time the screen published, regardless of platform or fixture. It was never a timing, attend-contract, or watchdog problem. Now opens, publishes 3 tabs and real rows, and closes cleanly (~0.2s). **NEXT_TAB round trip fixed (2026-08-30):** confirmed as the same tab-lift class of bug `mission_ui`/`faction_ui` hit -- `switch_tab` only takes effect inside `draw_item_tab()`/`draw_monster_tab()`/`draw_terfurn_tab()`, which `draw_controls()` skips entirely once the Godot panel has attended (`hide_ui = attended(); if (hide_ui) return;`), so `selected_tab` never moved. Fixed the same way as those two screens: NEXT_TAB/PREV_TAB now move `selected_tab` directly and keep `switch_tab` in step for the rare frame the ImGui draw does run. **Confirmed by `scenario_probe.gd`:** `surroundings NEXT_TAB round trip ok -- tab 0 -> 1`. MENU-12 is now fully done | `surroundings_menu.*`, `godot_surroundings_snapshot.*`, `surroundings_panel.gd` | `[surr] tabs=3 rows=N` and a NEXT_TAB round trip that actually changes tab |

> **Update 2026-08-30:** the NEXT_TAB fix above was verified with a full
> `scenario_probe.gd` run (`arch -arm64`, since the plain Godot.app launch on
> this machine picks the x86_64 slice under Rosetta and then refuses the
> arm64-only `libcataclysm-godot.dylib`). The run surfaced two **REQUIRED**
> failures unrelated to MENU-12 or this fix, first seen here because nothing
> had run the full probe end to end before: `look mode pans the view` (origin
> does not move after six steps east) and `hit clip played` (the melee-hit
> animation clip never registers). Also two WARN-level ones, likely benign:
> `scores published content: 0 rows` and `lamp light published: 0 sources`.
> Not investigated — flagging so the next person doesn't reopen MENU-12 by
> mistake when they see red in the coverage summary.
>
> **MENU-12's screen is committed and builds; it has not been observed running.**
> The verification attempts are worth reading only as a cautionary tale, because
> the blocker turned out to be self-inflicted.
>
> I added a debug command that called `list_surroundings()` directly, to sidestep
> keypress races. Queued commands are drained inside `wait_for_event()`, which is
> inside `handle_input()` -- so it opened a **modal screen with its own input loop
> from inside the input wait**, and nothing in a headless run dismisses it. The
> game thread parked there permanently: clock frozen at 8:00:00, `commits: 1`,
> every later stage failing, and teardown refusing to stop the thread. It also
> left an ImGui window shown, which made `commands_safe_to_run()` false and
> starved the command queue -- which I then wrote up here as a pre-existing
> mystery window. There was no such window. That entry was wrong; this replaces
> it.
>
> The hook is removed. **The rule it cost us: a queued command must not open a
> blocking screen.** `request_menu_action` gets away with it only because a Godot
> panel reliably attends and can dismiss what it opened, so the safety property is
> "someone will dismiss this", not "this is a legal thing to queue". In a headless
> run nobody attends, which makes the fixture the strict case rather than the
> lenient one.
>
> **Update (2026-08-29): it was never ordinary.** The reason a run that plays
> never got here either is `publish_to_godot()`'s infinite loop -- see the
> retired SOLILOQUY-1 entry and the MENU-12 row above. Fixed now; tabs and rows
> read back correctly, NEXT_TAB still does not.
| ~~MENU-13~~ | M | The mid-sized panels, one channel each (`request_debug_spawn_npc()` gives the NPC-dependent ones something to talk to). **All seven done: `martialarts`, `scores_ui`, `medical_ui`** (2026-08-21, API 28), **`study_zone_ui`** (2026-08-23, API 29), **`mission_ui`** (2026-08-23, API 30), **`faction_ui`** (2026-08-23, API 31), **`npctalk_rules`** (2026-08-30) -- snapshots, panels, loop splits and bindings, each with the attend contract. `study_zone_ui` needed no tab-lifting (no tabs to begin with) and no cursor state: a checkbox toggle mutates `npc_skill_preferences` directly, addressed by an absolute skill index so it can never land on the wrong row after a filter change. `mission_ui` combines the tab shape (`scores_ui`) with the row-cursor shape (`medical_ui`) for the first time, plus a wrinkle neither had: switching tabs here changes which *collection* is selected from (active/completed/failed missions and points of interest are different lists, not different views of one), so a tab switch always refetches and resets the cursor rather than just re-filtering. `faction_ui` turned out to need no tab-lift at all -- unlike every other tab screen here, it already used `cataimgui::BeginTabItem(label, selected_tab == X)` instead of reading the return value, so `selected_tab` was already a real source of truth -- but pairs *four* different pointer types with one cursor each (`basecamp *`, `npc *`, `const faction *`, `const mtype_id *`), and its CONFIRM opens a nested screen (dialogue, a uilist, a rename prompt) that the panel must get out from under, suspended the same way `MedicalSnapshot`'s APPLY suspends for the item picker. `npctalk_rules` was flagged as needing a whole new selection model first and turned out not to: see its row in the table below for why. Verified with a debug hook (`debug_open_follower_rules()`) since its real path is a dialogue topic the probe does not navigate, and a `scenario_probe.gd` stage | those files | All MENU-13 screens off the overlay |
| ~~MENU-13a~~ | S | ~~**The diary**~~ **done and observed (2026-08-29, API 33)** — `godot_diary_snapshot.*` + `diary_panel.gd`, same loop-split takeover as the rest of MENU-13. The legacy three-pane keyboard-focus cycle (pages / changes / text) is dropped in favour of a click addressing a page or change row directly -- the same simplified re-presentation the advanced-inventory and martial-arts panels made; the row cursor (`selected_change`) survives only because the summary page uses it to pick which entry's text shows. Two actions leave for a screen this channel is not and suspend around it: "Edit text" opens `string_editor_window` (still a raw `catacurses::window`), and "View scores" opens the already-migrated scores screen -- which this unblocks: nothing else opened it with a key, so it could not be observed running until now. `_probe_diary_and_scores()` in `scenario_probe.gd` opens with the real `)` key, checks published pages, round-trips NEW_PAGE, opens scores via VIEW_SCORES, checks the suspend/resume pair, and closes both -- **all now pass**, once the real blocker (an unrelated infinite loop in `surroundings_menu.cpp`, tested earlier in the same probe run -- see the retired SOLILOQUY-1 entry) was fixed. First runs had this stage failing for a cause outside the diary code entirely; worth remembering next time something later in a probe run looks broken | `diary_ui.cpp`, `godot_diary_snapshot.*`, `diary_panel.gd`, `scenario_probe.gd` | The scores screen is reachable, and its probe stage can assert rows |

### The remaining ImGui screens, sorted by where their state lives

Sorting by line count was the wrong instinct. What decides the cost of migrating
one of these is not its size but **whether its state exists anywhere other than
inside ImGui**. Every screen migrated so far had a model or a driving loop to
stand beside; the ones left do not all have that.

| Screen | Selection state | Shape | Cost |
|---|---|---|---|
| `martialarts` | 42 refs, no tabs | model + loop | tractable — largest file, but the window is a detail pane |
| ~~`faction_ui`~~ | 35 refs, 4 tabs | model, tab in widget | **done** (2026-08-23) -- the one screen here that turned out to need no tab-lift at all, since it already used `cataimgui::BeginTabItem(label, selected_tab == X)`; the cost went instead into four independent picked-pointer types (one per tab) and CONFIRM's nested screens |
| ~~`mission_ui`~~ | 41 refs, 4 tabs | model, tab in widget | **done** (2026-08-23) -- combined the tab lift with a row cursor (`medical_ui`'s shape) for the first time; see BACKLOG's MENU-13 entry for the collection-per-tab wrinkle that added |
| `medical_ui` | 11 refs, 2 tabs | model, tab in widget | same |
| `scores_ui` | 9 refs, 4 tabs | model, tab in widget | same |
| ~~`study_zone_ui`~~ | 5 refs, no tabs | model + its own `execute()` | **done** (2026-08-23) -- was the easiest, the dialogue shape exactly, and needed no tab-lifting or cursor state at all |
| `compare_item_menu` | — | opens from other screens | follows whatever opens it |
| ~~`npctalk_rules`~~ | **none** | selection is ImGui *nav focus* | **done (2026-08-30)** -- the "needs a model introduced first" turned out not to be true: every rule was *already* directly hotkey-addressable (`draw_controls()`'s `pressed_key == assigned_hotkey` path, alongside the nav-focus path CONFIRM uses), so a mouse-driven panel never needed a cursor at all. A boolean rule publishes and is toggled by its own stable `ally_rule` flag value; a radio group (engagement/aim/CBM recharge/CBM reserve) by its enum's own int value -- never by row position, so a click can't land on the wrong rule after a republish. IMPORT/EXPORT (the rule-transfer popup between two followers) is not carried over -- it is its own sub-screen, not a simple action, and needs 2+ followers to even reach; left for a follow-up. Verified via a new `debug_open_follower_rules()` game-command hook (the real path is a dialogue topic, which needs a conversation-tree walk the probe does not drive -- same shortcut `debug_spawn_npc()` is for the screens it unblocks) and a `scenario_probe.gd` stage: 18 rules / 2 groups published, a boolean toggle flips the right flag (`enabled false -> true`), and a radio-group set lands on the requested value (`engagement: 1 -> 0`) | `npctalk_rules.*`, `godot_follower_rules_snapshot.*`, `follower_rules_panel.gd`, `godot_game_commands.*` |
| `end_screen` | **none** | ImGui nav | same |

Two things this changes:

- **`BeginTabItem` returning true is how those screens choose a tab.** There is no
  `selected_tab` to publish until someone lifts it into a member. That is a small
  refactor, but it is a refactor of upstream code and should be a separate commit
  from the migration so it can be reviewed on its own.
- **`npctalk_rules` and `end_screen` have no selection variable at all** — the
  highlighted row is ImGui's internal nav state, moved with
  `NavMoveRequestSubmit`. Nothing can publish *that*. **Turned out to be the
  wrong worry for `npctalk_rules` (done 2026-08-30):** CONFIRM reads nav focus,
  but every rule is *also* directly hotkey-addressable
  (`pressed_key == assigned_hotkey`), and that path already identifies a rule
  by its own stable id, never by position. A mouse click can use the same
  addressing a keypress does; no model needed. `end_screen` has not been
  checked for the same escape hatch yet — do that before assuming it needs one.

Suggested order was `study_zone_ui` then `martialarts`, both now done, then
`mission_ui` and `faction_ui`, also both done, then `npctalk_rules`, also now
done. What is left of this table: `end_screen`, and `compare_item_menu` with
whatever opens it.

| ~~MENU-14~~ | M | The six `catacurses::window` settings screens — safe mode, auto pickup, auto notes, distractions, colors, help. **All six done** (2026-08-31): `distraction_manager`, `auto_note`, `color_manager`, `safemode`, `auto_pickup`, `help`. **These were why the cell buffer still existed.** "One channel probably serves all six" (the original note here) undersold `help`, which turned out to need none of its own -- see its row below. `color_loader.cpp` in the file list above never existed, the colors screen is `color_manager::show_gui()` in `color.cpp` | those files | **Done** -- nothing draws into `catacurses::window` in a session |

### MENU-14's six screens, sorted the same way MENU-13's were

| Screen | Shape | Cost |
|---|---|---|
| ~~`distraction_manager`~~ | flat toggle list, no tabs, no nested popup, no save prompt (writes straight into `uistate`) | **done (2026-08-30)** -- the pilot for this batch: `godot_distraction_snapshot.*`, `distraction_panel.gd`. A toggle is addressed by absolute row index (stable -- the list never filters or reorders while open); the "Toggle all" row's cascade to every other flag was lifted into a shared `apply_toggle()` so the legacy loop and the takeover run the exact same code, not two copies of a 16-line assignment list. Reached the same way the real in-game menu reaches it (`request_menu_action(DISTRACTIONS)`), not a debug hook -- unlike follower rules, nothing here is dialogue-gated. Verified via `scenario_probe.gd`: 17 rows published, toggling row 0 ("Noise") flips its own `enabled` bit |
| ~~`auto_note`~~ | two tabs (character/global), row toggle + enable/disable hotkeys, a nested rename-symbol popup that itself opens a colour-pick `uilist`, an explicit "Save changes?" `query_yn` at the end | **done (2026-08-30)** -- `bCharacter` and `currentLine` were promoted from `show()` locals to members (the `medical_ui`/`bp` move), and the whole 45-line `CHANGE_MAPEXTRA_CHARACTER` handler was lifted into its own `change_symbol()` method so the legacy loop and the takeover call one copy, not two. The nested symbol prompt and colour `uilist` are already Godot panels, but this channel still suspends around the call (`set_suspended`) the same way `MedicalSnapshot`'s APPLY does -- without it the hidden half of this screen answers the door for the popup on top of it. Nothing here writes to the real settings until the post-loop epilogue's "Save changes?" prompt, and that epilogue is untouched: `show()` splits into `if( !godot_handled ) { <legacy loop> }` followed by the same epilogue code either loop produced changes for, the "show_legacy() plus a shared epilogue" shape options/keybindings used. **A fresh fixture has nothing in its display cache** (only *discovered* map extras show), so a new scenario command, `scenario_discover_map_extra()`, marks one seen first, the same way walking into it would. Verified via `scenario_probe.gd`: 1 row after discovering `mx_crater`, a toggle flips that row's own `enabled` bit, and a tab switch actually changes `tab` (character and global really are different lists, not different views of one -- same wrinkle `mission_ui` hit) | `auto_note.*`, `godot_auto_note_snapshot.*`, `auto_note_panel.gd`, `godot_game_commands.*` |
| `distraction_manager` | see above | done |
| ~~`safemode`~~ | two tabs (global/character), full rule editor: add/edit/remove, whitelist/blacklist, attitude, proximity, category, movement mode, a "test this rule against a name" nested window | **done (2026-08-31)** -- largest of the six (874 lines), but most of that size was three things that turned out to already have a Godot-native answer rather than needing bespoke UI: (1) `string_input_popup` (the *non*-imgui class) was swapped for `string_input_popup_imgui` for both the rule-text and proximity edits -- `auto_pickup.cpp` already uses it for its own rule-text edit, and its `query()` already routes through the generic Godot text-prompt channel (`godot_popup_snapshot.h`) when GODOT is defined, so this needed no new panel, just the swap; the rule-text edit's nested "wildcard help" `ui_adaptor` (a second curses window shown alongside the prompt) was dropped in favor of `.set_description()`, which the Godot text prompt already renders -- one less window to migrate. (2) `TEST_RULE`'s ~60-line bespoke scrollable curses window was replaced outright by a plain read-only `uilist` (already a migrated Godot panel), the MENU-4 "drive the existing implementation" insight applied a second time. (3) `gui_tab` (mirroring `auto_note::bCharacter`) and `gui_changes_made` (mirroring `color_manager::gui_stuff_changed`) were promoted to members; a cell click is encoded as `row * 6 + column` (`Columns` enum order), the same scheme `color_manager` uses for its own (row, column) picks. What was left after that -- add/remove/copy/swap/enable/disable/move/toggle-in-place -- is genuinely new channel code (`godot_safemode_snapshot.*`), but a small fraction of the original 874 lines. **Also applied the `color_manager` lazy-window fix proactively**, since `safemode::show()` had the identical eager `ui_helpers::full_screen_window()` call before the Godot branch. **Found a second, unrelated latent bug while verifying this one, in the probe's own shared `_answer_popups()` watchdog:** it de-duplicated an already-answered popup by `"notice|text"` signature but never cleared that signature once the popup closed, so a *later, different* popup with identical text (two unrelated `query_yn(_("Save changes?"))` calls, one from `color_manager`'s epilogue and one that would have come from `safemode`'s) was mistaken for the same still-open instance and never answered -- a parked game thread and, since `drain_game_commands()` runs one command at a time, every command queued afterward (including opening the next screen) timed out in cascade. Fixed by resetting the signature whenever `popup_active()` goes false. Verified via `scenario_probe.gd`: 0 rows on a fresh fixture, `ADD_RULE` round-trips to 1, editing the new rule's text through the (generic, not safemode-specific) text-prompt channel round-trips to `'zombie'`, the whitelist/blacklist toggle round-trips, and removing the row returns to 0 | `safemode_ui.h`, `safemode_ui.cpp`, `godot_safemode_snapshot.*`, `safemode_panel.gd`, `scenario_probe.gd` (the `_answer_popups()` fix) |
| ~~`auto_pickup`~~ | same rule-editor shape as `safemode` (whitelist/blacklist by name/material/flag), but generic: `auto_pickup::user_interface` is shared by the player screen (1 or 2 tabs -- global, plus character only once a character is loaded, not safemode's always-two-but-locked shape) and the NPC pickup-rules screen (exactly 1 tab, titled with the NPC's own name) | **done (2026-08-31)** -- turned out much smaller than `safemode`'s 1014 lines suggested: only two columns (rule text, include/exclude), no attitude/proximity/category/movement-mode. The Godot channel (`godot_auto_pickup_snapshot.*`) publishes a generic `tab_titles` list plus the current tab's rows, rather than assuming "global/character" -- the "tab switch refetches" shape `mission_ui`/`safemode` use, but title-driven instead of index-driven so the NPC screen's single tab needs no special case. `gui_tab` was promoted to a member the same way `safemode::gui_tab` was, and the rule-text edit (`gui_edit_rule_text()`) was factored out of the legacy loop so both loops call one copy -- it now returns `bool` (whether the player actually entered text) so the legacy loop's existing "roll back an abandoned ADD_RULE" logic keeps working unmodified. **`rule::test_pattern()`'s ~90-line bespoke scrollable curses window was replaced outright by a plain read-only `uilist`**, unconditionally (not just under GODOT) -- the same MENU-4 "drive the existing implementation" insight safemode's own TEST_RULE applied a second time, which means this needed no per-row Godot channel of its own, just a suspend around the call. **ADD_RULE is simplified from the legacy screen's immediate text-prompt-on-add**: the Godot takeover just appends a blank rule and lets a click on its rule cell edit it, the same simplified re-presentation `safemode`'s own ADD_RULE settled on -- avoids threading the "rollback if cancelled" logic through a suspend/resume cycle. Also applied the `color_manager`/`safemode` lazy-window fix proactively (`ui.mark_resize()` instead of an eager `init_windows(ui)` call), since `user_interface::show()` had the identical eager `ui_helpers::full_screen_window()` call before the Godot branch existed. Verified via a new `scenario_probe.gd` stage (`_probe_auto_pickup()`, chained after `_probe_safemode()`): `ADD_RULE` round-trips to +1 row, editing the new rule's text through the shared text-prompt channel round-trips to `'rock'`, the include/exclude toggle round-trips, and removing the row returns to the starting count | `auto_pickup.h`, `auto_pickup.cpp`, `godot_auto_pickup_snapshot.*`, `auto_pickup_panel.gd`, `scenario_probe.gd` |
| ~~`color_manager`~~ (`color.cpp`, the "colors" screen) | row + column cursor (name / invert), `REMOVE_CUSTOM`, and two nested `uilist` pickers (load a color template, load a base theme) | **done (2026-08-30)** -- no tabs, so no `bCharacter`-style member to promote, but `gui_name_color_map` (the working copy of every named colour) and `gui_stuff_changed` were: both were locals the legacy loop, the Godot takeover, and the shared "Save changes?" epilogue all need to see the same value of, the same `medical_ui`/`bp` move auto_note used for its tab/cursor. The row+column cursor click is encoded into one request int (`row * 2 + (col == 2 ? 1 : 0)`) since there's no atomic pair to send two numbers at once. `REMOVE_CUSTOM`, the "pick a custom color" `uilist`, and the two file-list `uilist`s (template/theme) were each lifted out of the legacy loop into their own `gui_*` method so both loops call one copy; the channel suspends around each nested `uilist` the same way `AutoNoteSnapshot`'s `GODOT_SYMBOL` does. **Found and fixed a latent, pre-existing GODOT-port bug in the process, not specific to this screen:** the original code called `ui_helpers::full_screen_window()` directly (not lazily via `on_screen_resize` + `mark_resize()`, the pattern every other migrated MENU-13/14 screen uses), so it ran unconditionally before the Godot-vs-legacy branch was even decided. Under GODOT, `FULL_SCREEN_WIDTH`/`FULL_SCREEN_HEIGHT` are never initialized -- that happens in `main.cpp`'s terminal-size negotiation, which the GODOT backend does not run -- so `FULL_SCREEN_WIDTH` was `0`, `content_width` went negative, and `cursesport.cpp`'s `newwin()` called `std::vector::resize()` with a negative count, throwing (libc++'s message for both `vector::at()` out-of-range and `vector::resize()` length-error is literally `"vector"`) and killing the whole game thread. No prior MENU-13/14 screen had hit this because each of them builds its windows lazily and the Godot takeover always succeeds before any redraw would trigger it; `color_manager` is the first of this batch shaped like the *original*, unmigrated code (eager window construction), and switching it to the lazy pattern was the fix, not a new safety check. Verified via `scenario_probe.gd`: 145 rows published, picking row 0's Normal cell drives the nested colour `uilist` end-to-end (`has_custom` flips `false` → `true`), and removing it flips back | `color.h`, `color.cpp`, `godot_color_manager_snapshot.*`, `color_manager_panel.gd` |
| ~~`help`~~ | not a toggle screen at all: a hotkey/mouse category grid that opens a page of scrollable text per topic | **done (2026-08-31) -- needed no new channel at all.** The plan here ("rewrite the picker as a real `uilist`") turned out exactly right, and it went further than expected: `draw_menu()`'s hand-rolled clickable grid (the `inclusive_rectangle` hit-testing, the manually-allocated hotkey map, the two-column layout math) is deleted outright and replaced by a plain `uilist` built fresh each loop iteration -- MENU-4's "drive the existing implementation" insight, but unconditional this time (not gated on GODOT), the same way `safemode`'s and `auto_pickup`'s `TEST_RULE`/`test_pattern()` replaced their own bespoke windows with a `uilist` unconditionally. The detail view was *not* already wired to `godot_textwin_snapshot.*` as this row previously claimed -- `scrollable_text()` itself has no GODOT branch, only `ui_iteminfo.cpp` and `ui_extended_description.cpp` called the snapshot directly. Fixed the claim by doing the same here: under GODOT, `run_textwin_in_godot()` is tried first (single unnamed tab, same shape `ui_iteminfo.cpp`'s `execute()` uses for item info) and only falls back to `scrollable_text()` if unattended. That fallback window is now built lazily (`init_windows` never called eagerly), the same `color_manager`/`safemode` GODOT fix applied a third time -- the original always called it eagerly, so it was carrying the same latent FULL_SCREEN_WIDTH=0 crash those two hit, just never observed because nothing had exercised the fallback path yet. Verified via a new `scenario_probe.gd` stage (`_probe_help()`): opens via `request_menu_action(8)`, confirms the category picker publishes entries, `uilist_confirm(0)` opens the detail pane with non-empty text, `textwin_dismiss()` returns to the picker, and `uilist_cancel()` closes the screen -- driving the two already-migrated generic panels directly, the same way `_probe_color_manager()` drives its own nested picker (and sets the same `_uilist_stage_active` guard against the per-frame unattended-uilist watchdog) | `help.h`, `help.cpp`, `scenario_probe.gd` (no new snapshot or panel file) |
| MENU-9 | S | Delete the overlay: `terminal_view.gd`, the ImTui blit, the ImGui cell layer, `USE_CURSES_UI_OVERLAY` | `terminal_view.gd`, `godot_view_snapshot.*`, `godot_curses_backend.cpp` | The cell buffer is gone (after everything above) |
| ~~SOLILOQUY-1~~ | M | ~~The legacy soliloquy window~~ **retracted and fixed as something else entirely (2026-08-29).** What looked like an ambient monologue parking the game thread was misdiagnosed: `cataimgui::window` instrumentation showed the stuck window's id was `'SURROUNDINGS'` -- MENU-12's own screen, opened by this probe's scripted `V` press, not by anything ambient. The real bug: `surroundings_menu::publish_to_godot()` looped `for (t = items; t != num_tabs; ++t)`, but `operator++(surroundings_menu_tab_enum&)` (defined for cyclic NEXT_TAB/PREV_TAB navigation) wraps `num_tabs` back to `0` and never produces the value the loop was waiting to see -- an infinite loop, on the game thread, every single time the surroundings screen published, on any platform, since the day it was written. Nothing about the "attend" contract, the watchdog timeout, or the fixture was at fault; the screen had simply never survived long enough for anyone to notice it never finished opening. Fixed by walking the underlying ints instead of the cyclic operator. Confirmed by re-running `scenario_probe.gd`: surroundings, martial arts, medical, diary and scores all now open, publish, round-trip an action and close, each in ~0.2s. See AGENT_HANDOFF.md for the full misdiagnosis-to-fix trail -- worth reading before trusting the next "stuck legacy screen" theory | `surroundings_menu.cpp` (`publish_to_godot`) | Fixed -- surroundings, and everything downstream of it in a probe run, now completes |

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
| Fire glow | `fire_glow_gain` 2.4, `glow_intensity` 0.7, `glow_bloom` 0.15 | — | Does fire read as a light source, or as a smear? Does anything bloom that should not? **Judge this one from scratch:** the glow keys on an HDR target and `viewport/hdr_2d` was never actually in effect until 2026-08-17 (see ADR-004's landed notes), so whatever the glow looked like before, it was not looking at what these numbers describe. `glow_bloom` is 0.0, not 0.15 — it bypasses the threshold and blooms the whole frame. |
| Night grade | `night_tint`, `night_desaturation` 0.45 | `0.62,0.72,1.0` | Does night read as night rather than as a blue filter? |
| Rain grade | `wet_tint`, `wet_desaturation` 0.30 | `0.82,0.88,1.0` | Is a downpour distinguishable from dusk? |
| Pain / vignette | `pain_tint`, `vignette_strength` 0.35 | — | Does being hurt register without making the map unreadable? |
| Particles | `field_particles.gd` `scale_min/max`, `lifetime`, `amount` | — | Is smoke smoke, or a cloud of dots? |
| Hit reaction | `map_view.gd` `HIT_DURATION` 0.22, `HIT_OFFSET` 0.34 | — | Does a melee hit read as a blow, or as a glitch? |

Two things specifically **never observed at all**, because the probe never
leaves the starting shelter: map memory (walk away from a lit room and look
back) and a lantern gradient at night (drop a light source in the dark).

Please write findings back into this table rather than into a commit message —
the next person needs the numbers, not the history.

### First findings (2026-08-18, scenario-probe stills under lavapipe)

The scenario probe (VER-2 item 1) produced the first screenshots of most of the
scenes this table asks about. Stills under a software rasteriser are a floor,
not a verdict — motion, feel and colour on a real monitor still need a person —
but they answer some questions and sharpen others:

| What | Observed |
|---|---|
| Night grade | **Reads as night**, not as a blue filter. Ground detail survives but is near the readability floor; borderline, keep the numbers. |
| Map memory | **Exists on screen for the first time** (it was structurally dead before this date — see `CHANGELOG.md`). Remembered rooms are clearly distinct from never-seen void, but read close to black beside noon grass; `memory_color` likely wants brightening. |
| **The night mask has no headroom** | The load-bearing find of the sitting: at 1am, **1072 of 1075 texels carry G > 0.5 before any light exists**. A moonlit CDDA night reports outdoor tiles as `LIT` (`light_at` is honest; a bright moon clears `LIGHT_AMBIENT_LIT`), so the per-tile mask saturates and **no lamp, fire pool or headlight cone can brighten outdoor ground at night** — the visible darkness is entirely the daylight grade, which dims lit and unlit alike. Not a bug — the game is the authority — but it means every "gradient at night" question must be asked somewhere genuinely dark (a basement, a new-moon night), and the probe now does. |
| Fire | The +22 brightness measured around it is the particles and the B-channel flicker, not a G pool (see above). Visually **fire reads as a pale smoke puff**: smoke particles out-draw the fire puffs ~10:1 by area (scale 0.6–1.6×2.2s vs 0.25–0.7×0.7s), and the night grade turns the blob blue-white. No orange survives. The tuning sitting should start here. |
| Lantern gradient | Outdoors: unanswerable under a bright moon (mask saturated, above) — and the fixture's `electric_lantern_on` spawns chargeless anyway, so the game says dark twice over. Asked in the **basement** instead: lit texels went 0 → 48 around a spawned fire, and the `10a_basement_fire` still is the answer this table has waited for — an orange core, a room lit with a believable falloff, black past the pool's edge, the avatar mesh correctly inside it. Mild banding where the pool meets the dark (the per-tile texture's bilinear seam); the bands themselves read well. Fire also reads as *fire* here — the outdoor blue-puff problem is the moonlit mask plus the night grade, not the fire pass itself. |
| Headlights | Two beams built, **nothing lit**, for two stacked reasons: the mask saturation above (a cone adds nothing to an already-LIT moonlit field), and a level beam half a tile up grazing the up-facing ground at ndl≈0. `BEAM_PITCH_DEGREES` (15°) added for the second; the first needs a dark night or interior to judge at all. |
| Volumetric fog | Works under lavapipe (no banding), and the fire gains a genuine glow-through-air halo — but density 0.02 dims the whole tilted frame (fog attenuates the EMISSION-lit tiles). Fog length now tracks `camera.far`. Default stays off. |
| Rain | Streaks fall and lean with the wind; at night the wet grade is barely distinguishable from the night grade — the streaks are what says "rain". |
| Levels below | `open_columns=1` on a staircase — the walk descends — but one 32 px stair tile shows nothing legible of the floor below. Judging `level_fade` needs a rooftop lip or an open pit, not a stairwell. |
| Creature mesh | The converted `player_female.res` sits right: feet planted, ~1.5 tiles tall against 2-tile walls, black silhouette at night and grey in daylight (lit geometry among unlit sprites, as intended). It is a T-pose, which is the art's problem, not the renderer's. |
| Anomalies noted | Creature sprites occasionally draw beyond the map's west edge, in the void left of the ground plane; and a lone black quad sits past the SE corner. Both seen in every field-scene still; neither attributed yet. |

### ~~VER-0~~ — **Done** (e2ce7ced1b)

The probe now fails when a required fixture stage never executed, or a required
generation counter is still at zero. Exit code, not a printed note: the failure
being guarded against is that nobody reads a green log closely. Optional
counters (uilist, textwin, anim, overmap) are reported but never failed on --
they depend on the world rolling the right way, and a check that cries wolf gets
ignored.

It went red on its first run and caught real crafting-fixture flakiness.

**And its exit code never survived a session (found and fixed 2026-08-18).**
Shutdown with a live session ends in `std::_Exit( 0 )` on the game thread --
the input wait's shutdown check, or `~CDDAHost` when the thread will not stop --
which *overwrites the exit code Godot was about to return*. So every failing
probe run since VER-0 landed has exited 0, and "exit code, not a printed note"
was a printed note after all. (`_Exit` also skips stdio flushing, so the
diagnostic that would have revealed it was swallowed with it.) Both `_Exit`
sites now carry the code the fixture registers via `CDDAHost.note_exit_code()`
before quitting; both probes register theirs. Diagnosed by three
one-minute fixtures: a plain scene propagates `quit(3)`, a bootstrapped host
propagates it, a live session turned it into 0.

### OPEN CRASH — pixel minimap walks a freed vehicle (found 2026-08-19)

SIGSEGV on the game thread, first caught by the scenario probe the moment two
conditions finally co-occurred in a fixture: the minimap sized (it publishes
nothing unsized, so no headless run before 2026-08-19 ever executed this code)
and a melee fight next to a spawned car. Symbolicated:

    game_do_turn -> update_pixel_minimap -> GodotPixelMinimap::draw
      -> process_cache -> update_cache_at (godot_pixel_minimap.cpp:583)
      -> get_map_color_at (:323) -> vehicle::get_display_of_tile
      -> vehicle::part_displayed_at (vehicle.cpp:3521)
      -> SEGV inside relative_parts.find()

`get_map_color_at` uses a live `veh_at(p)`, so the dangling pointer is in the
map's vehicle cache: something destroyed or split a vehicle without the cache
hearing about it, and the minimap is merely the first reader per turn. Suspect
window: zombies bashing the scenario car during the combat stage. Kills the
whole game thread, so every later fixture times out -- a run that dies here
looks like four unrelated failures. Not deterministic (three sized-minimap runs
survived before one died). Needs its own investigation; the trace above is the
starting point.

### Session-end and look-mode coverage (2026-08-18)

Two player reports, both verified fixed by fixtures the same day:

- **"Look freezes the game"**: the snapshot centred on the avatar and ignored
  `view_offset`, and nothing republished from inside `look_around`'s own loop.
  Fixed in C++ (centre = pos + view_offset; the input wait republishes when the
  centre moves); the scenario probe's look stage asserts the published origin
  pans and comes back.
- **"Quitting sometimes works"**: `g->uquit` set from the command drain was
  never re-read by the infinite input wait -- the quit landed on the next
  keypress, whenever that was. Fixed by a synthetic keypress from the wait when
  uquit is set (a TIMEOUT event is swallowed by handle_action's idle loop -- the
  fixture refuted that version). `session_end_probe.tscn --mode
  quicksave|save_quit|quit_nosave` is the coverage: quits must land with NO key
  pressed. The game menu also Escapes a starving legacy screen shut before
  dispatching, since a shown C++ window (the soliloquy) blocks the drain.

### VER-2 — Tooling that would move work off the person

Ranked by how much verification each unlocks per unit of effort. None exist yet.

1. ~~**A scripted scenario harness.**~~ **Built (2026-08-18, API 23).** Nine
   scenario commands on `godot_game_commands.*` / `CDDAHost` -- teleport to an
   OMT type by prefix, relative teleport, stand-on-flag (how a fixture finds a
   staircase), set time, force weather, spawn field / vehicle (lights switched
   on) / item, set avatar sex -- plus `get_scenario_status()`, a generation
   counter the fixture polls because a teleport-by-search can only fail on the
   game thread. Each command ends by rebuilding the map cache and republishing
   the snapshots, or the mutation is invisible until the next player action.
   None of them opens a screen, which is the rule that matters.

   `res://scenes/scenario_probe.tscn` drives them end to end: field at noon,
   night, a lit lantern, a fire, a car with headlights on, volumetric fog for
   one frame, rain, a house entered and left (memory), a basement and the
   staircase above it, a zombie. VER-0-style REQUIRED/WARN checks, honest exit
   code, and one PNG per scene under the xvfb `--screenshot` recipe. First
   runs immediately produced: `open_columns=1` (the first look down a hole
   since ADR-005 item 1), `beams=2` (the first headlight cones ever built),
   rain particles, and two of the dead-frame-boundary class of bug -- see
   VER-0's exit-code note and the map-memory entry in `CHANGELOG.md`.
2. **Screenshot comparison against stored baselines.** The Xvfb + lavapipe route
   (see `AGENT_HANDOFF.md`) renders real frames; what is missing is remembering
   what they looked like last time. Commit a handful of reference PNGs, compare
   with a tolerance, and fail on drift. This turns "does it still look right"
   from a person's job into a diff, and would have caught the sway tearing.
3. **A frame-motion assertion.** The probe already shoots two frames 0.45s apart
   and can diff them. Give it an expected answer — "nothing on the terrain
   layers may differ between frames unless a sway or particle command exists" --
   and the tearing class of bug fails the run rather than needing an eye.
4. ~~**A compile gate for GDScript and shaders.**~~ **Built** (2026-08-17):
   `build-scripts/check-godot-scripts.sh`. `gdparse` was the gate and checks syntax
   only, so a script that names a member no class has passed it and then failed to
   compile at load -- which reads as a blank map and a stage that "never ran", with
   no error pointing at a script. Godot's own `--check-only` names the file and line
   and needs neither the extension nor a GPU. Verified by breaking a script and
   watching the gate go red.
5. **Live uniform editing without a rebuild.** Expose the tuning constants above
   as `@export` on a Godot resource rather than shader defaults and GDScript
   constants, so VER-1 becomes one sitting with a slider instead of a
   build-run-look loop. Cheap, and it makes VER-1 something a non-programmer can
   do.
6. **A tile-inspector under the cursor.** `describe_sprite()` already answers
   "why did this tile draw what it drew" for an id typed by hand. Wiring it to
   the moused-over tile makes the debug overlay answer it for the thing the
   person is actually looking at.

### What each verification route can and cannot answer

| Route | Answers | Blind to |
|---|---|---|
| `check-godot-scripts.sh` | Whether every script and shader *compiles*, and every property and enum name in them exists | Anything about behaviour. But run it first: a script that does not compile leaves its node scriptless and every `has_method()` guard skips it silently |
| `geometry_check.tscn` | Whether the 3D backend places sprites where the 2D one draws them, at any tilt, by projecting them back through the camera | Colour, lighting, order, and whether the placement it verifies is the placement the game asks for |
| `headless_probe.tscn` | What C++ published, what MapView built, whether panels and scripts run | Anything about pixels |
| `shader_check.tscn` | Shader language errors | GPU backend rejections; whether the shader is *right* |
| Xvfb + lavapipe screenshot | Layout, colour, whether something is on screen at all | Motion, feel, frame pacing |
| Two-frame diff | What moves that should not | Whether motion that should happen does |
| A person playing | Everything above, and only this | Nothing — but it does not scale, and it is the scarce resource |

---

## Part 4 — The 3D backend (ADR-006)

**Read [`architecture_adr.md`](architecture_adr.md) ADR-006 first.** This is a
proposal with a kill criterion in the middle of it, not a list to work through.
The one-line summary: keep the top-down view and the existing sprites, put them
in a 3D scene, and use Godot's 3D renderer for the light, shadow, fog and depth
the 2D path cannot reach. Nothing in it requires new art.

**Do this before anything else in this part**, and before believing ADR-006's
table of tilt angles:

```bash
./build-scripts/compose-tileset.sh
python3 -c 'import json
c = json.load(open("gfx/UltimateCataclysm/tile_config.json"))
tw, th = c["tile_info"][0]["width"], c["tile_info"][0]["height"]
print("tile", tw, "x", th)
for s in c["tiles-new"]:
    # sprite_width / sprite_height default to the tile size when absent, which
    # is how a sheet says "these sprites fit their cell".
    print(s.get("file"), s.get("sprite_width", tw), "x", s.get("sprite_height", th),
          "offset", s.get("sprite_offset_x", 0), s.get("sprite_offset_y", 0))'
```

Then open a wall, a tree and a table in an image viewer and ask the one question
the whole part turns on: **do they agree about where the camera is?** ADR-006's
45°–55° band is arithmetic performed on an assumption, and four features in a row
on this branch were settled by printing a value instead of arguing about it.

| Id | Size | What | Notes |
|---|---|---|---|
| ~~3D-0~~ | M | **Done (2026-08-17): ADR-004, the world in a `SubViewport`.** `godot/scripts/world_viewport.gd`; MapView is reparented into it at startup. | Read the "What landed" section of ADR-004 before building on it. Three things it cost that were not on this list: the world viewport is a `TextureRect` with a device-pixel render target and a `size_2d_override`, because the obvious `SubViewportContainer` wiring would have quietly dropped the world to the canvas stretch's base resolution; MapView's Ctrl+wheel zoom had to move to `host.gd`, because the world viewport takes no input; and the composite has to mirror `map_view.visible`, or the main menu opens over a still of the map. The overlay-over-sidebar ordering is fixed by construction. **The grade is still in the tile shader**, so `field_particles.gd` is still ungraded — that is now possible, not done, and it should not be moved in the same step as VER-1 changes its constants. |
| ~~3D-1a~~ | L | **Built (2026-08-17): the flat 3D backend.** `map_view_3d.gd` + `map_tiles_3d.gdshader`, behind `host.gd`'s `USE_3D_MAP`, off by default. Orthographic unrotated camera, `MultiMeshInstance3D` per batch, spatial port of the tile shader, no C++ change. | **Built, not judged.** The milestone is still open and needs a person: turn `USE_3D_MAP` on and diff a screenshot against the 2D backend's with the Xvfb + lavapipe recipe. First thing to check if the map comes out too dark: flip `pipeline_encodes_srgb` in the shader — the canvas pipeline writes sRGB and the 3D one encodes on output, and which applies here could not be read without a GPU. Depth comes from `depth_rank`'s *order*, mapped to consecutive z steps rather than to the rank value, because the rank range would outrun float32 precision at camera distance. |
| ~~3D-1b~~ | M | **Done (2026-08-17): alpha-scissor, the opaque pass, and depth per sprite.** The batch key is down to the shader uniforms — atlas, sway, palette, receives-light — and each sprite carries its own z. | Done **before** the tilt, against this ADR's own recommended order, for a dependency the order missed: a cast shadow (3D-5) is a silhouette from an opaque-pass material, so the tilt experiment cannot show anything until this exists. The depth ranks in a frame are compacted to consecutive z steps, which is what keeps float32 able to resolve them at camera distance. The visible cost is hard sprite edges where Ultica has soft ones; judge that against a screenshot of 3D-1a, not of the canvas, and read `alpha_scissor` in the shader before assuming a threshold of zero undoes it — it does not. |
| ~~3D-1c~~ | M | **Done (2026-08-17): the four layers that are not tiles.** Contact shadows are a MultiMesh of blob quads in the world; fire and smoke are `field_particles_3d.gd` (`GPUParticles3D`); the fallback glyphs and the animation overlay stay canvas items on a `CanvasLayer` inside the world viewport. | The split is by whether depth matters. Shadows had to become geometry — on the canvas they land on top of the creature casting them — and being depth-tested made them better than the 2D pass: a tree between camera and blob now hides it, and a blob can sit on the floor of a level *below* the avatar, which the 2D backend skips because one node has one depth to spend. The glyphs lose their per-layer ordering, which only affects tiles whose art is missing anyway. **The canvas transform is exact only while the camera is unrotated — 3D-3 breaks it**, and then those two need `unproject_position` per point or a home in the world. |
| ~~3D-1d~~ | S | **Done (2026-08-17), and it was not the job it looked like.** The glyphs and the animation overlay stay on their canvas at any tilt; `TILT_DEGREES` defaults to 45. | They were hidden while tilted on the assumption that an affine canvas transform cannot follow a rotated camera. **It does not have to.** Everything on that canvas annotates the *ground*, and a ground point's screen position is unchanged by the tilt by construction -- that is what the pre-stretch is for, and the gate had already been printing the proof (a floor sprite lands on the same screen pixel at 0 and at 75 degrees). `geometry_check.tscn` now holds the canvas transform against the camera's own projection at six tilts, and a 5% error in either fails it. Estimated M, cost a deletion: the expensive fix was for a problem that was not there. |
| ~~3D-2~~ | M | **Done (2026-08-17): the light channel.** `LightSnapshot::add_light` + `get_light_sources()`, seven floats per source, walked out of `level_cache::light_source_buffer`. The 3D backend builds an `OmniLight3D` per source, capped at 32. API version 20 -- **needs a `make GODOT=1`**. | **Nothing lit by them is visible yet**, and that is not an oversight: the tile shader is `unshaded`, so in a flat world a light can only multiply the frame by a constant. They are positioned, counted in the render overlay and checked by the probe, and the lit shader is what consumes them. Two things to know: the buffer is documented as valid only inside `generate_lightmap` (it is not cleared afterwards, which is why this works -- the counter is there to notice if that changes), and the unbuffered sources are still missing (glowing critters, the avatar's own torch, headlight arcs). `VOLUMETRIC_FOG` in `map_view_3d.gd` is the one switch that shows the lights early, and it is off because a fog volume in a flat world glows through walls. |
| ~~3D-3~~ | S | **Done (2026-08-17): the world stands up.** `TILT_DEGREES` in `map_view_3d.gd`, 0.0 by default, `set_tilt_degrees()` to drive it. Ground quads horizontal, standing quads vertical, camera pitched, each axis pre-divided by its own factor. | **The geometry is verified; the look is not.** `res://scenes/geometry_check.tscn` round-trips a floor, a road, a wall, a tree and a creature through the camera at six tilts -- all thirty land 0.00 px from where the 2D backend draws them. So this ADR's claim that the tilt could not be checked without light was half wrong: the arithmetic is arithmetic. What still needs a person is whether one angle *suits* trees, walls and table tops, and that needs the lit shader. While tilted the glyphs and the animation overlay are hidden (3D-1c's debt), so it is an experiment, not a setting. |
| ~~3D-4~~ | M | **Done (2026-08-17): levels below get a floor.** `LEVEL_DROP_TILES`, two tiles of height per level while tilted, coplanar when flat. Verified by `geometry_check.tscn`: a level one down falls exactly that many pixels, the same at every tilt, and slides sideways not at all. | Two tiles because Ultica draws a wall as 64 px in a 32 px cell and ADR-005 found the tileset declares no height of its own. **Still C++ work to finish it:** `fog_for_depth` dims lower levels in the tint, so real distance fog would dim them twice; and lower levels still get no engine light, because the light texture holds one texel per column and applying it a storey down would light a basement with the daylight on the roof. And nobody has looked down a hole yet -- that is still ADR-005 item 1's open question. |
| ~~3D-5~~ | M | **Done (2026-08-17): the lit shader, cast shadows and a sun.** The tile shader keeps its own result in EMISSION and lets engine lights add a directional term in `light()`, masked by CDDA's per-tile light. Standing sprites cast double-sided shadows while tilted; the batch key regains `tall` so the ground does not cast onto itself. | Engine light is **off in the flat world** by design: with every normal facing the camera it could only add a uniform wash and would move the backend off its baseline. **The sun's bearing is invented** -- elevation follows the published `daylight`, azimuth is `SUN_AZIMUTH_DEGREES`. The real value is `sun_azimuth_altitude()` in `src/calendar.h`; publishing it is one line on the conditions channel and should ride with the next C++ change. `engine_light_gain` and the two `SUN_*` constants are VER-1 material. |
| ~~3D-7a~~ | S | **Done (2026-08-17): shadow proxies.** One invisible capsule per creature, `SHADOWS_ONLY`, sized from its sprite; creature billboards stop casting so there is one shadow rather than two. Terrain that stands keeps its own silhouette. | A billboard's silhouette never changes, so a figure lit from the side cast a front view of itself -- the shadow said nothing about where the light was, which is the one thing a shadow is for. The contact blob stays: it works when nothing is casting, and the two cues answer different questions. Gated: the capsule's foot must project onto the creature's feet. |
| ~~3D-7b~~ | S | **Done (2026-08-17): `SHOW_SHADOW_PROXIES`.** Off by default. On, the creature *sprites* are hidden and their capsules are drawn in their place, lit -- not a capsule behind a sprite, which would answer nothing. | The cheapest answer to "does a body at this scale sit correctly in this world", and it needs no art to exist first. What to look at: feet in the right place, height reading as a person against the walls, and -- since the capsule is lit and the sprites are not -- which way the sun appears to come from. That last one is the first thing in this renderer that shows where the light actually is rather than where an artist decided it was. |
| ~~3D-7c~~ | M | **Done (2026-08-17, API 22): a mesh per creature id, defaulting to the sprite.** `CDDAHost::get_creatures()` publishes identity -- id, kind, feet in pixels, level, facing -- beside the draw list; `creature_meshes.gd` draws whatever has art under `res://meshes/creatures/<id>.*` and leaves the rest to their sprites. | **Nothing changes until a mesh exists**, which is the design: partial is the normal state. Two things worth knowing. Identity needed a channel of its own because a draw command is an atlas sub-rect -- everything a sprite needs and nothing a mesh can use. And a sprite is suppressed by *tile*, because the draw list still cannot say which creature a command belongs to; CDDA allows one creature per tile, so a tile is an identity. That correspondence is the fragile part and is gated: both sides quantise from the sprite's centre, and quantising its corner instead -- the first draft -- passes for flush sprites and fails for every overhanging one. `README.md` in the mesh directory is what a modeller needs. |
| ~~3D-8~~ | L | **Done (2026-08-18, API 24): the meshes animate.** Creature `uid` + `move_mode` on the channel; hit events carry attacker/target uids and a kind, with death taps in `monster::die`/`Character::die`; `creature_meshes.gd` tweens each step (world-space deltas — view-relative ones lie whenever the avatar recentres the origin — with live retargeting, snap past 2.5 tiles), yaws a mesh toward its movement, and plays `idle`/`walk`/`run`/`attack`/`hit`/`die`; the converter preserves rigs as `<id>.scn`; `make_example_animated_creature.tscn` writes the rigged mannequin that is both fixture and the modeller's executable spec. Verified by the scenario probe's combat stage: the mannequin zombie walked and flinched on the avatar's blows, `clips_played = { idle, walk, hit }`, exit 0. | **Follow-up (same day):** swings are events now (kind 2, tapped in both melee entry points — a monster's attack clip plays whether or not it connects); the shared clip library landed (`_shared_clips.scn`/`.glb`, borrow-by-bone-name, same-skeleton contract — rig each model once, one clip pack animates them all; README has the human workflow); heights recalibrated to what the art PAINTS (33, measured: a person is 32-33 opaque px of the 32x48 frame — the 48-unit figures towered half again over their sprites); and `SMOOTH_CAMERA` rides the avatar's tween so the world stops snapping a tile ahead of the glide. **Still open:** real rigged art (sourcing is the user's — free/open rigged models on ONE standard skeleton, Mixamo auto-rig included; the pipeline is ready for the drop); gait timings (`WALK_TWEEN_S` 0.22 …, VER-1) on a real GPU; the avatar's own rig (player_female converts but is unrigged — Mixamo auto-rigging that very model is the shortest path). Also found on the way: the legacy ImGui **soliloquy window** (ambient snippet monologue) still opens over the session with no Godot channel — it parks the game thread and starved two probes before being named; migrating it is Part 2 work, and the scenario probe Escapes past it meanwhile. |
| ~~3D-6~~ | M | **Done in structure, open in judgement (2026-08-18).** Weather falls through the scene (`weather_particles_3d.gd` — rain streaks lean with the wind, snow drifts, acid is rain the wrong colour; kind from the new `weather_kind` condition). Volumetric fog is a runtime toggle gated on the tilt, its length tracking `camera.far` (the CAM_Z-sized volume left the far half of the tilted map unfogged); lavapipe shows it working — the basement-fire halo glows through air — at the cost of dimming the whole EMISSION-lit frame at density 0.02. Headlight beams were already built end to end and had never had a vehicle: the scenario probe spawns one, and the beams needed the downward pitch a level cone turned out to lack (`BEAM_PITCH_DEGREES`). | What still needs eyes on a real GPU: fog density (0.02 dims the map), beam pitch/pool, snow (never forced), and everything against a *dark* night — a moonlit CDDA night is `LIT` nearly everywhere, so light pools only exist indoors or under a new moon (see VER-1's findings table). |

**The hazards are the real risk, not the geometry.** Every 3D renderer default is
wrong for a 32 px sprite and each one fails quietly: mipmaps bleed the atlas
(the reason `default_texture_filter=0` exists), TAA/FXAA smear nearest-sampled
pixels, and a camera on a fractional pixel makes the whole scene shimmer on every
step the avatar takes. ADR-006 lists them; treat that list as a checklist for
3D-1 rather than as commentary.
