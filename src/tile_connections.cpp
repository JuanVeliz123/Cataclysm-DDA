#include "tile_connections.h"

#include <climits>

#include "enums.h"

namespace tile_connections
{

void get_rotation_and_subtile( const char val, const char rot_to, int &rotation,
        int &subtile )
{
    const bool no_rotation = rot_to == CHAR_MAX;
    switch( val ) {
        // no connections
        case 0:
            subtile = unconnected;
            if( no_rotation ) {
                rotation = 0;
                break;
            }
            rotation = get_rotation_unconnected( rot_to );
            break;
        // all connections
        case 15:
            subtile = center;
            rotation = 0;
            break;
        // end pieces
        // rotations:
        //
        // --> edge index
        // Nw, Ws, Sw, Es,
        // Ne, Wn, Se, En,  |
        // N+, W+, S+, E+,  V  get_rotation_... return index
        // N-, W-, S-, E-
        //
        // (Nw = north end piece, rotated to west)
        case 8:
            // vertical end piece S
            subtile = end_piece;
            if( no_rotation ) {
                rotation = 2;
                break;
            }
            rotation = 2 + 4 * get_rotation_edge_ns( rot_to );
            break;
        case 4:
            // horizontal end piece E
            subtile = end_piece;
            if( no_rotation ) {
                rotation = 1;
                break;
            }
            rotation = 1 + 4 * get_rotation_edge_ew( rot_to );
            break;
        case 2:
            // horizontal end piece W
            subtile = end_piece;
            if( no_rotation ) {
                rotation = 3;
                break;
            }
            rotation = 3 + 4 * get_rotation_edge_ew( rot_to );
            break;
        case 1:
            // vertical end piece N
            subtile = end_piece;
            if( no_rotation ) {
                rotation = 0;
                break;
            }
            rotation = 4 * get_rotation_edge_ns( rot_to );
            break;
        // edges
        // rotations:
        //
        // --> edge index
        // NSw, EWs,
        // NSe, EWn,  |
        // NS+, EW+,  V  get_rotation_... return index
        // NS-, EW-,
        //
        // (NSw = north-south edge, rotated to west)
        case 9:
            // vertical edge
            subtile = edge;
            if( no_rotation ) {
                rotation = 0;
                break;
            }
            rotation = 2 * get_rotation_edge_ns( rot_to );
            break;
        case 6:
            // horizontal edge
            subtile = edge;
            if( no_rotation ) {
                rotation = 1;
                break;
            }
            rotation = 1 + 2 * get_rotation_edge_ew( rot_to );
            break;
        // corners
        case 12:
            subtile = corner;
            rotation = 2;
            break;
        case 10:
            subtile = corner;
            rotation = 3;
            break;
        case 3:
            subtile = corner;
            rotation = 0;
            break;
        case 5:
            subtile = corner;
            rotation = 1;
            break;
        // all t_connections
        case 14:
            subtile = t_connection;
            rotation = 2;
            break;
        case 11:
            subtile = t_connection;
            rotation = 3;
            break;
        case 7:
            subtile = t_connection;
            rotation = 0;
            break;
        case 13:
            subtile = t_connection;
            rotation = 1;
            break;
    }
}

int get_rotation_edge_ns( const char rot_to )
{
    if( ( rot_to & static_cast<int>( NEIGHBOUR::EAST ) ) == static_cast<int>( NEIGHBOUR::EAST ) ) {
        if( ( rot_to & static_cast<int>( NEIGHBOUR::WEST ) ) == static_cast<int>( NEIGHBOUR::WEST ) ) {
            // EW
            return 2;
        } else {
            // Ew
            return 1;
        }
    } else { // east -
        if( ( rot_to & static_cast<int>( NEIGHBOUR::WEST ) ) == static_cast<int>( NEIGHBOUR::WEST ) ) {
            // eW
            return 0;
        } else {
            // ew
            return 3;
        }
    }
}

int get_rotation_edge_ew( const char rot_to )
{
    if( ( rot_to & static_cast<int>( NEIGHBOUR::NORTH ) ) == static_cast<int>( NEIGHBOUR::NORTH ) ) {
        if( ( rot_to & static_cast<int>( NEIGHBOUR::SOUTH ) ) == static_cast<int>( NEIGHBOUR::SOUTH ) ) {
            // NS
            return 2;
        } else {
            // Ns
            return 1;
        }
    } else { // north -
        if( ( rot_to & static_cast<int>( NEIGHBOUR::SOUTH ) ) == static_cast<int>( NEIGHBOUR::SOUTH ) ) {
            // nS
            return 0;
        } else {
            // ns
            return 3;
        }
    }
}

int get_rotation_unconnected( const char rot_to )
{
    int rotation = 0;
    switch( rot_to ) {
        // Catch no and all first for performance; these are the last sprites!
        case 0: // NONE
            rotation = 15;
            break;
        case 15: // ALL
            rotation = 12;
            break;

        // Cases for single tile to rotate to -> easy
        case static_cast<int>( NEIGHBOUR::NORTH ):
            rotation = 2;
            break;
        case static_cast<int>( NEIGHBOUR::EAST ):
            rotation = 1;
            break;
        case static_cast<int>( NEIGHBOUR::SOUTH ):
            rotation = 0;
            break;
        case static_cast<int>( NEIGHBOUR::WEST ):
            rotation = 3;
            break;
        // Two tiles, resulting in diagonal
        case 10: // NE
            rotation = 6;
            break;
        case 3: // SE
            rotation = 5;
            break;
        case 5: // SW
            rotation = 4;
            break;
        case 12: // NW
            rotation = 7;
            break;
        // Cases for three tiles to rotate to -> easy
        // Arranged to fallback / modulo to fitting index 0-4
        case 14: // 3 but south --> modulo = north
            rotation = 10;
            break;
        case 11: // 3 but west --> modulo = east
            rotation = 9;
            break;
        case 7: // 3 but north --> modulo = south
            rotation = 8;
            break;
        case 13: // 3 but east --> modulo = west
            rotation = 11;
            break;
        // Two opposing tiles, (No tiles, all tiles; see first cases)
        case 9: // N-S
            rotation = 14;
            break;
        case 6: // E-W
            rotation = 13;
            break;
    }

    return rotation;
}

} // namespace tile_connections
