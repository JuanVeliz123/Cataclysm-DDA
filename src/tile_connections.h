#pragma once
#ifndef CATA_SRC_TILE_CONNECTIONS_H
#define CATA_SRC_TILE_CONNECTIONS_H

/**
 * Multitile connection geometry: which subtile a tile should draw, and how far
 * to turn it, given which neighbours it connects to and which it rotates toward.
 *
 * Lives apart from cata_tiles because two backends need it and only one of them
 * has SDL. The Godot backend resolves its own tile ids and has to produce
 * exactly the same answer; when it kept a transcribed copy of this table, the
 * copy silently handled only the no-rotation case and roads drew the wrong
 * variant. One definition, two callers.
 */

/// Which neighbour a bit in a connection mask stands for. The values are the
/// bit weights map::get_known_connections and get_known_rotates_to produce.
enum class NEIGHBOUR {
    SOUTH = 1,
    EAST = 2,
    WEST = 4,
    NORTH = 8,
    last
};

namespace tile_connections
{

/**
 * @param val neighbours this tile connects to (a NEIGHBOUR bitmask)
 * @param rot_to neighbours this tile rotates toward, or CHAR_MAX for none
 * @param rotation out: quarter turns, or an index into a multi-sprite variant
 *                 list when the tileset supplies 8 or 16 of them
 * @param subtile out: a MULTITILE_TYPE
 */
void get_rotation_and_subtile( char val, char rot_to, int &rotation, int &subtile );
int get_rotation_unconnected( char rot_to );
int get_rotation_edge_ns( char rot_to );
int get_rotation_edge_ew( char rot_to );

} // namespace tile_connections

#endif // CATA_SRC_TILE_CONNECTIONS_H
