// test_wfc.cpp — Dungeon room generation test for kestrel-wfc
// g++ -std=c++17 -O2 -o test_wfc test_wfc.cpp && ./test_wfc

#include "wfc.hpp"
#include <cstdio>
#include <string>

// Dungeon tileset:
// 0: Floor  (.)  - connects to floor, door, wall(top of door)
// 1: Wall   (#)  - connects to wall, floor(via door), pit edge
// 2: Door   (+)  - connects floor-to-floor, wall-to-wall
// 3: Pillar (O)  - connects to floor only
// 4: Pit    (~)  - connects to pit, wall(edge), floor(bridge?)

int main() {
    using namespace wfc;

    std::vector<Tile> tiles = {
        {0, "floor",   '.'},
        {1, "wall",    '#'},
        {2, "door",    '+'},
        {3, "pillar",  'O'},
        {4, "pit",     '~'},
    };

    Generator gen(20, 12, tiles);

    // Adjacency: which tile can be in [Dir] relative to from_tile
    // Floor connects to floor, door, pillar, wall, pit
    gen.set_adjacency(0, 0, Dir::Right);
    gen.set_adjacency(0, 0, Dir::Left);
    gen.set_adjacency(0, 0, Dir::Up);
    gen.set_adjacency(0, 0, Dir::Down);
    gen.set_adjacency(0, 2, Dir::Right);  // floor -> door
    gen.set_adjacency(0, 2, Dir::Left);
    gen.set_adjacency(0, 2, Dir::Up);
    gen.set_adjacency(0, 2, Dir::Down);
    gen.set_adjacency(0, 3, Dir::Right);  // floor -> pillar
    gen.set_adjacency(0, 3, Dir::Left);
    gen.set_adjacency(0, 3, Dir::Up);
    gen.set_adjacency(0, 3, Dir::Down);
    gen.set_adjacency(0, 1, Dir::Right);  // floor -> wall
    gen.set_adjacency(0, 1, Dir::Left);
    gen.set_adjacency(0, 1, Dir::Up);
    gen.set_adjacency(0, 1, Dir::Down);
    gen.set_adjacency(0, 4, Dir::Right);  // floor -> pit
    gen.set_adjacency(0, 4, Dir::Left);
    gen.set_adjacency(0, 4, Dir::Up);
    gen.set_adjacency(0, 4, Dir::Down);

    // Wall connects to wall, door, floor, pit
    gen.set_adjacency(1, 1, Dir::Right);
    gen.set_adjacency(1, 1, Dir::Left);
    gen.set_adjacency(1, 1, Dir::Up);
    gen.set_adjacency(1, 1, Dir::Down);
    gen.set_adjacency(1, 2, Dir::Right);
    gen.set_adjacency(1, 2, Dir::Left);
    gen.set_adjacency(1, 2, Dir::Up);
    gen.set_adjacency(1, 2, Dir::Down);
    gen.set_adjacency(1, 0, Dir::Right);
    gen.set_adjacency(1, 0, Dir::Left);
    gen.set_adjacency(1, 0, Dir::Up);
    gen.set_adjacency(1, 0, Dir::Down);
    gen.set_adjacency(1, 4, Dir::Right);
    gen.set_adjacency(1, 4, Dir::Left);
    gen.set_adjacency(1, 4, Dir::Up);
    gen.set_adjacency(1, 4, Dir::Down);

    // Door connects to floor and wall
    gen.set_adjacency(2, 0, Dir::Right);
    gen.set_adjacency(2, 0, Dir::Left);
    gen.set_adjacency(2, 0, Dir::Up);
    gen.set_adjacency(2, 0, Dir::Down);
    gen.set_adjacency(2, 1, Dir::Right);
    gen.set_adjacency(2, 1, Dir::Left);
    gen.set_adjacency(2, 1, Dir::Up);
    gen.set_adjacency(2, 1, Dir::Down);
    gen.set_adjacency(2, 2, Dir::Right);
    gen.set_adjacency(2, 2, Dir::Left);
    gen.set_adjacency(2, 2, Dir::Up);
    gen.set_adjacency(2, 2, Dir::Down);

    // Pillar connects to floor only
    gen.set_adjacency(3, 0, Dir::Right);
    gen.set_adjacency(3, 0, Dir::Left);
    gen.set_adjacency(3, 0, Dir::Up);
    gen.set_adjacency(3, 0, Dir::Down);

    // Pit connects to pit, floor, wall
    gen.set_adjacency(4, 4, Dir::Right);
    gen.set_adjacency(4, 4, Dir::Left);
    gen.set_adjacency(4, 4, Dir::Up);
    gen.set_adjacency(4, 4, Dir::Down);
    gen.set_adjacency(4, 0, Dir::Right);
    gen.set_adjacency(4, 0, Dir::Left);
    gen.set_adjacency(4, 0, Dir::Up);
    gen.set_adjacency(4, 0, Dir::Down);
    gen.set_adjacency(4, 1, Dir::Right);
    gen.set_adjacency(4, 1, Dir::Left);
    gen.set_adjacency(4, 1, Dir::Up);
    gen.set_adjacency(4, 1, Dir::Down);

    // Frequency: floor common, walls common, doors rare, pillars rare, pits uncommon
    gen.set_frequency(0, 50.0);  // floor
    gen.set_frequency(1, 30.0);  // wall
    gen.set_frequency(2, 3.0);   // door
    gen.set_frequency(3, 5.0);   // pillar
    gen.set_frequency(4, 12.0);  // pit

    // Try multiple seeds
    for (int seed = 1; seed <= 10; seed++) {
        auto grid = gen.run(seed);
        printf("=== Seed %d ===\n", seed);
        printf("%s\n", gen.to_ascii(grid).c_str());
    }

    return 0;
}