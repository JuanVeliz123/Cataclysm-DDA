# Creature meshes

Drop a mesh here named after the creature id and that creature is drawn as geometry
instead of as its sprite. Anything without a mesh keeps its sprite. **The migration is
meant to be partial for a long time** — the fallback is the normal case, not an error path.

    <id>.scn    an *animated* creature: a scene with skeleton and clips. Preferred over all
    <id>.res    a Mesh resource, binary. What the game loads for a *static* creature
    <id>.tres   the same, text: readable and diffable, worth it for hand-made meshes
    <id>.glb    the source to keep. Converted to .res -- or .scn if rigged -- by the script below
    <id>.obj    works, but is worse than .glb in every way that matters here

The loader tries them in that order: **`.scn` → `.tres`/`.res` → `.glb`/`.obj`** — so a
`.res` left beside a newer `.scn` is stale but harmless.

**Commit the `.glb`, not the `.res` or `.scn`.** Both are derived and gitignored, the same
way `gfx/` is composed rather than committed; run the converter after a fresh checkout.

### Which format, and why it is not a matter of taste

`.glb` and `.obj` are *source assets*: Godot converts them into `.godot/imported/` and needs
an `.import` file beside them, which only the editor writes. **A `.glb` dropped in here
cannot be loaded by the running game** — so while the editor is unusable, it cannot be loaded
at all. `.tres` and `.res` are resources and load directly, which is why the loader tries
them first and why the converter exists.

Size is not decided by the container. The same 96-triangle mesh is 8,456 bytes as `.tres` and
9,400 as `.res` — binary is *larger*, because the text form base64s the vertex arrays while
the binary form pads them. About 88 bytes a triangle either way, so **vertex count is the only
real lever**. For comparison, one real character: 431 KB as `.glb`, 1,013 KB as `.obj` for
identical geometry, 319 KB as the converted `.res`.

Detail is decided by screen pixels, not by modelling convention. A creature is ~33 units tall
and about half that wide, and one unit is one screen pixel at default zoom — so it occupies
roughly five hundred pixels, eight thousand at maximum zoom. **300 to 1,500 triangles**, one
64–128 px texture or just vertex colours, one material, and no normal or roughness maps: at
twenty pixels wide they cannot be resolved. The converter warns past 1,500 rather than
decimating, because decimating well is a modelling decision.

Rigging is no longer discarded: a `.glb` with a skeleton and at least one animation is kept
whole as `<id>.scn` instead of being flattened — see **Animated creatures** below. So is a
`.glb` with a skeleton and *no* animations, when the shared clip library can dress it (see
**The shared clip library**). An unrigged `.glb` still becomes a `.res`, and a creature that
has no clips to play should stay one: a scene per zombie costs more than a pooled mesh and
buys nothing without animation.

### Converting

    godot --headless --path godot res://scenes/convert_creature_meshes.tscn

Reads every `.glb` here with `GLTFDocument` — at runtime, no editor, no import step — merges
its parts with their own transforms, scales it so the figure is 33 units tall (the height the
art *paints*, not the 48 of its frame — see **What to know before modelling**), puts its feet
on the origin, centres it, and writes `<id>.res` beside it. If the result has its back to you, set
`FACE_DEGREES` to 180 in the script and run it again: exporters disagree about which way
forward is, and no inspection of a mesh can settle it.

A *rigged* `.glb` — a skeleton plus at least one animation, or a bare skeleton with the
shared clip library present — takes a different exit from the same run and becomes
`<id>.scn` instead; see the next two sections.

The ids are the ones the tileset keys on, and the log says which it has seen: the 3D
backend prints `creature meshes: N drawn, M ids seen, K with art` on its first frame, and
`[mesh] <id> -> <path>` for each one it finds. So the way to learn the name of the thing
you are looking at is to run the game and read the id list.

Examples, from `CDDAHost::get_creatures()`:

    mon_zombie.glb          a monster type id
    player_male.glb         the avatar; `player_female` for the other
    npc_male.glb            every male NPC, until they are told apart

## Animated creatures

`<id>.scn` is the animated form of an asset, and its shape is a contract with the runtime
loader:

- A `PackedScene` whose root is a `Node3D` named for the id. Below it, anywhere, sit
  **exactly one `Skeleton3D`** with its skinned `MeshInstance3D`(s) and **one
  `AnimationPlayer`**.
- The player's default library holds clips named `idle`, `walk`, `attack`, `hit`, `die`,
  plus an optional `run`. `idle`/`walk`/`run` loop; `attack`/`hit`/`die` play once.
- Clips are **in place** — no root translation across the ground. The game owns position
  and the renderer tweens the node between tiles, so a walk cycle that also travels moves
  the creature twice.
- The rest pose follows the static conventions exactly: faces +Z, stands on the origin
  (feet at y = 0, centred on x and z), ~33 units tall — one unit is one tile pixel, and
  33 is the height the sprite art paints, not the 48 of its frame.

Like the `.res`, a `.scn` is a **generated artifact**: derived and gitignored, rebuilt after
a fresh checkout. Two ways to produce one:

- drop a rigged `.glb` here and run the converter above — a skeleton plus at least one
  animation is what routes it down the animated path;
- run the example generator, `res://scenes/make_example_animated_creature.tscn`, which
  builds an animated `mon_zombie.scn` programmatically and is the record of its numbers,
  the way `make_example_creature_mesh.tscn` is for the static example.

The converter normalises by setting **one transform on the imported scene's root** — scale
and rotation are not baked into the vertices, because the bones own the vertices now — and
marks `idle`/`walk`/`run` as looping, because glTF has no loop flag and every exporter ships
clips as play-once. A play-once idle freezes after the first play and reads elsewhere as a
T-pose bug. Recommended clips that are missing are warned about at convert time, not failed:
a creature that cannot attack yet is still worth seeing walk — and when the shared clip
library below sits in this directory, they are not missing for long, because the converter
borrows them from it.

## The shared clip library

`_shared_clips.scn` or `_shared_clips.glb`, in this directory (the converter tries the
`.scn` first, the loader's own preference). One file holding the six contract clips, which
every rigged model that lacks some — or all — of them borrows from at convert time. **The
underscore prefix is what marks it as shared rather than a creature**: no monster type id
starts with one, and the converter's file loop skips it instead of minting a creature scene
for it.

What borrows when:

- a rigged `.glb` missing some of the six gets exactly the missing ones — a model that
  carries all six borrows nothing, and its own clips always win over the library's;
- a rigged `.glb` with **no clips at all** — a skeleton and nothing else, which is exactly
  what a stock character download is — gets an AnimationPlayer created for it and all six
  borrowed. This is the intended shape of the pipeline: rig each model once, ship it bare,
  and animate the whole roster from one file;
- no library and clips missing: the convert-time warnings stand, and a rig with no clips at
  all stays a static `.res` — a statue, which the static path draws better;
- borrowed clips get the same loop-flag fix as native ones.

The borrow is a **track-name copy**, not a retarget: each track's node path is rewritten to
point at the model's own skeleton and its `:bone_name` half is kept as-is. That is only
correct when every model is rigged to **one standard skeleton — same bone names, same rest
orientations**. This is the contract, not advice: **borrowing across different skeletons
produces garbage silently** — a clip keyed for someone else's `hips` plays on yours without
a single error and looks like a seizure. Mixamo's skeleton works; pick one and rig
everything to it. Tracks naming bones the model does not have are dropped with a warning
(a partial match is almost always a different rig wearing familiar names), and a clip whose
every track would be dropped is refused outright, because an empty clip passes every check
and plays a T-pose.

## Getting real art in (the workflow)

For a human with Blender and/or a Mixamo account. The one rule that matters: **every model
rides the same skeleton** — rig new models on it rather than inventing rigs per creature.

1. **Pick the standard skeleton once.** Mixamo's works: upload any humanoid to Mixamo and
   let it auto-rig, or build one rig in Blender and reuse it. Every later model must carry
   the same bone names and rest orientations — that is what makes the clip library apply.
2. **Get the six clips, once.** Apply an animation pack to any *one* rigged character —
   a Mixamo pack does fine — or author them in Blender. Name the clips exactly `idle`,
   `walk`, `run`, `attack`, `hit`, `die` (rename Mixamo's `mixamo.com` actions in Blender's
   NLA/Action editor). Clips must be **in place** — no root travel; the game owns position.
   Export that one character, clips and all, as glTF Binary to
   `godot/meshes/creatures/_shared_clips.glb`. Every other model then ships bare.
3. **Per model:** rig it on the standard skeleton, face it +Z, apply transforms
   (Ctrl+A in Blender), and export glTF Binary (`.glb`) to
   `godot/meshes/creatures/<creature_id>.glb` — `<creature_id>` being the game's monster
   type id (`mon_zombie_brute`) or `player_male`/`player_female`/`npc_male`/`npc_female`.
   No animations needed in the export; scale is normalised by the converter (to the painted
   33 units), so proportions matter and metres do not.
4. **Convert:** `godot --headless --path godot res://scenes/convert_creature_meshes.tscn`.
   Expect one `borrowed '<clip>' from _shared_clips` line per missing clip. Loop flags are
   fixed here — glTF exports everything as play-once, so do not fight that in the DCC tool.
5. **Check:** `./build-scripts/check-godot-scripts.sh` and read the `[geom] anim` line for
   your id — it fails on the mistakes that actually get made (origin at the body's centre,
   metres instead of tile pixels, missing `idle`/`walk`, a non-looping loop).
6. **Commit the `.glb` only.** The derived `.scn`/`.res` are gitignored; they are rebuilt
   after a fresh checkout by the same converter run.

## What to know before modelling

- **Faces +Z, stands on the origin.** A creature is placed at its feet, so the mesh's
  origin belongs between them, not at its centre. It is rotated 180° about Y to face left,
  because a sprite faces left by being mirrored and a mesh cannot.
- **One unit is one tile pixel, and the target height is what the art *paints*.** Ultica
  paints a person only 32-33 opaque pixels of its 32x48 frame — measured from the composed
  atlas (2026-08-18): player_male painted 15x33, player_female 15x32, mon_zombie 21x29 —
  so the converter scales every figure to **33 units**, not the frame's 48. The first
  meshes shipped at 48 and towered half again over every sprite beside them, and the first
  person to walk around one said so. Model at true proportions and let the converter scale;
  the world's *height* axis is pre-stretched by `1/cos(tilt)` and the mesh is scaled to
  match, so there is nothing to compensate for by hand.
- **No clothes.** Character overlays are not drawn on meshed creatures; that is a decision
  and not an omission (see ADR-006's mesh amendment).
- **The lighting will disagree with the sprites around it.** Ultica paints its own sun into
  every tile, and a lit mesh does not. That is expected and temporary — the art is meant to
  be replaced rather than reconciled with.

## The humans

`player_male.glb`, `npc_male.glb`, `player_female.glb` and `npc_female.glb` are the
Quaternius "Animated Man" (Oct 2017) and "Animated Woman" (Dec 2017) packs, **CC0**
(quaternius.com/packs/animatedman.html and .../animatedwoman.html), with a skin tone
baked into each: the packs paint clothes and skin in the texture, so one mesh wears
every look, and the player ids carry the light-skin texture while the npc ids carry
the dark -- two people in any conversation read as two people. The clips arrive named
"Armature|Walking" and the converter's alias table renames them to the contract; the
packs ship no `hit` clip, so that one falls back to the runtime's timed flinch.

`godot/scripts/import_quaternius_humans.gd` is the record of how the .glbs were made
from the downloaded packs (FBX + tone PNGs -> textured .glb, through FBXDocument, no
Blender): point `QUATERNIUS_PACKS` at the two downloaded folders and run its scene.
Re-assigning a tone is one line and a re-run. It uprights each figure automatically
from its own named bones -- the two packs disagree about which way is up, and the
woman's mesh rest box is ~85x smaller than her skeleton, which is why everything here
measures bones, never the mesh.

**Derived assets build themselves.** The host converts any committed `.glb` whose
`.scn`/`.res` is missing or older at boot (and generates the mannequin if absent), so
a fresh checkout plays with meshes without any manual converter run. The converter
scene remains the manual path, and forces a full rebuild.

## The example

`player_male.tres` is a blocky humanoid whose only job is to be a correct example of the
conventions above. Open it, measure it, and throw it away. (With `player_male.scn` now
converted from the Quaternius man, the loader never draws this file -- `.scn` outranks
it -- but it remains the smallest complete example of the static conventions.)

It is a build artefact, and the script that builds it is the record of the numbers:

    godot --headless --path godot res://scenes/make_example_creature_mesh.tscn

`godot/scripts/make_example_creature_mesh.gd` is that script. The proportions are named
constants at the top -- head, torso, hips, legs, arms -- so changing one and re-running is
how to ask "what would a taller figure look like" without modelling anything. It also has a
nose, which is not decoration: the front is the convention most easily got wrong, and it is
the only part of a box person that says which way they are looking.

## What is checked automatically

`res://scenes/geometry_check.tscn` -- run by `build-scripts/check-godot-scripts.sh` -- loads
**every** mesh in this directory through the game's own loader and fails if it breaks a
convention:

    [geom] mesh player_female   31 x 33 x 5, base y +0.0  stands=true centred=true sized=true  ok

It catches the two mistakes that actually get made: an origin at the body's centre, which
plants the creature half underground, and a scale in metres rather than tile pixels, which
makes a person the size of a doorframe. So a new mesh is checked by dropping it in here and
running the gate — no game, no GPU, no editor.

An id with a `.scn` is checked as that scene and nothing else — the scene is what the game
will be handed. Same box conventions, plus what only a scene can get wrong: a `Skeleton3D`
must exist (an unrigged asset should have been a `.res`), `idle` and `walk` must exist and
loop. Missing `attack`/`hit`/`die` are WARN lines only, two-tier like the scenario probe:

    [geom] anim mon_zombie       14 x 33 x 8, base y +0.0  stands=true centred=true sized=true rig=true clips=[idle, walk] loops=true  ok
    [geom] anim mon_zombie       WARN no 'attack' clip -- optional, the runtime falls back

`godot/scripts/creature_meshes.gd` is the loader, and it caches misses, so adding a file
needs a restart rather than only a turn.
