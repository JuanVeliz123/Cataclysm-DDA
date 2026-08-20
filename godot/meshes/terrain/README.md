# Terrain and furniture meshes (3D-8d)

Drop a `<id>.res` here named after a terrain or furniture id and the 3D backend
draws that mesh instead of the id's sprite — a MultiMesh per id, so a room of
chairs is one draw call. Anything without a mesh keeps its sprite (or its
extruded box); **the fallback is the normal case, not an error path**, exactly
as in `../creatures/`.

Unlike the creature directory, the `.res` files here **are the committed
artifacts** — an ArrayMesh needs no per-checkout conversion, so there is no
boot-time step and nothing derived. `import_quaternius_furniture.gd` is the
record of where the current set came from (Quaternius Furniture Pack, CC0) and
re-generates them from the pack when pointed at it via `QUATERNIUS_PACKS`.

## The convention (geometry_check.gd enforces it)

- **One unit is one tile.** The footprint fits in a 1×1 tile box, height at
  most 1.5 tiles, proportions kept.
- **Feet at the origin**, centred on x and z, +y up.
- **Colour lives in vertex COLOR.** The renderer's shader
  (`mesh_tiles_3d.gdshader`) carries CDDA's per-tile tint in INSTANCE_CUSTOM
  and samples no texture, so the vertex channel is the material. The importer
  bakes each surface's colour across its vertices.
- Underscore-prefixed files are skipped, same as everywhere else.

## How it is sized on screen

The renderer scales each instance so its *apparent* height — which at this
camera works out to `k * (mesh height + mesh depth)`, the pre-stretches
cancelling — matches the sprite's painted height, capped so the footprint
never exceeds 1.1 tiles. A bed that paints low and deep comes out low and
deep; a floor lamp comes out tall and thin. No per-id tuning table, on
purpose: the sprite the mesh replaces is the calibration.

## Scope, today

Only **standing furniture on the avatar's own level** routes to meshes
(`map_view_3d.gd`, the ident branch of `_rebuild_batches`): lower z-levels
keep sprites because the depth-fade uniform is per batch, and terrain ids are
interned and published but not yet consumed — walls are boxes (3D-8) and are
good at it. Remembered furniture keeps its mesh, drawn in the memory tint.
