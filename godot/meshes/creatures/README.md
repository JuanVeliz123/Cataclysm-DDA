# Creature meshes

Drop a mesh here named after the creature id and that creature is drawn as geometry
instead of as its sprite. Anything without a mesh keeps its sprite. **The migration is
meant to be partial for a long time** — the fallback is the normal case, not an error path.

    <id>.tres   a Mesh resource saved directly
    <id>.res    the same, binary
    <id>.glb    what an art pipeline produces
    <id>.obj    the simplest thing Godot will import as a mesh

The ids are the ones the tileset keys on, and the log says which it has seen: the 3D
backend prints `creature meshes: N drawn, M ids seen, K with art` on its first frame, and
`[mesh] <id> -> <path>` for each one it finds. So the way to learn the name of the thing
you are looking at is to run the game and read the id list.

Examples, from `CDDAHost::get_creatures()`:

    mon_zombie.glb          a monster type id
    player_male.glb         the avatar; `player_female` for the other
    npc_male.glb            every male NPC, until they are told apart

## What to know before modelling

- **Faces +Z, stands on the origin.** A creature is placed at its feet, so the mesh's
  origin belongs between them, not at its centre. It is rotated 180° about Y to face left,
  because a sprite faces left by being mirrored and a mesh cannot.
- **One unit is one tile pixel.** A 32-pixel tile is 32 units across, so a person is about
  32 wide and 48 to 64 tall. The world's *height* axis is pre-stretched by `1/cos(tilt)`
  and the mesh is scaled to match, so model at true proportions and let the renderer
  stretch.
- **No clothes.** Character overlays are not drawn on meshed creatures; that is a decision
  and not an omission (see ADR-006's mesh amendment).
- **The lighting will disagree with the sprites around it.** Ultica paints its own sun into
  every tile, and a lit mesh does not. That is expected and temporary — the art is meant to
  be replaced rather than reconciled with.

## The example

`player_male.tres` is a blocky humanoid whose only job is to be a correct example of the
conventions above. Open it, measure it, and throw it away.

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

    [geom] mesh player_male   21 x 48 x 11, base y +0.0  stands=true centred=true sized=true  ok

It catches the two mistakes that actually get made: an origin at the body's centre, which
plants the creature half underground, and a scale in metres rather than tile pixels, which
makes a person the size of a doorframe. So a new mesh is checked by dropping it in here and
running the gate — no game, no GPU, no editor.

`godot/scripts/creature_meshes.gd` is the loader, and it caches misses, so adding a file
needs a restart rather than only a turn.
