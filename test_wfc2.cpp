// test_wfc2.cpp — Refined dungeon tileset with tighter constraints
// g++ -std=c++17 -O2 -o test_wfc2 test_wfc2.cpp && ./test_wfc2

#include "wfc.hpp"
#include <cstdio>

//
// Refined tileset: walls should form boundaries, floors should be open,
// doors should bridge wall/floor transitions, pillars sit in floor.
//

// Helper: bidirectional adjacency
#define BIADJ(gen, a, b, dir) do { \
    gen.set_adjacency(a, b, dir); \
    gen.set_adjacency(b, a, wfc::opposite(dir)); \
} while(0)

int main() {
    using namespace wfc;

    // 0: Floor (.)  1: Wall (#)  2: Door (+)  3: Pillar (O)  4: Pit (~)
    std::vector<Tile> tiles = {
        {0, "floor",   '.'},
        {1, "wall",    '#'},
        {2, "door",    '+'},
        {3, "pillar",  'O'},
        {4, "pit",     '~'},
    };

    // Tighter constraints:
    // Floor connects to: floor, pillar, door, pit(rare)
    // Wall connects to: wall, door, floor(border), pit(border)
    // Door connects to: floor, wall (door is a transition)
    // Pillar connects to: floor only (standalone in rooms)
    // Pit connects to: pit, floor(edge), wall(edge)

    for (int seed = 1; seed <= 10; seed++) {
        Generator gen(24, 14, tiles);

        // Floor-Floor (all dirs)
        for (int d = 0; d < 4; d++) {
            BIADJ(gen, 0, 0, static_cast<Dir>(d));
        }
        // Floor-Pillar (all dirs — pillars are in rooms)
        for (int d = 0; d < 4; d++) {
            BIADJ(gen, 0, 3, static_cast<Dir>(d));
        }
        // Floor-Door (all dirs)
        for (int d = 0; d < 4; d++) {
            BIADJ(gen, 0, 2, static_cast<Dir>(d));
        }
        // Floor-Pit (all dirs — floor can edge a pit)
        for (int d = 0; d < 4; d++) {
            BIADJ(gen, 0, 4, static_cast<Dir>(d));
        }
        // Floor-Wall (all dirs — floor can be next to wall)
        for (int d = 0; d < 4; d++) {
            BIADJ(gen, 0, 1, static_cast<Dir>(d));
        }
        // Wall-Wall (all dirs — walls form continuous structures)
        for (int d = 0; d < 4; d++) {
            BIADJ(gen, 1, 1, static_cast<Dir>(d));
        }
        // Wall-Door (all dirs — doors in walls)
        for (int d = 0; d < 4; d++) {
            BIADJ(gen, 1, 2, static_cast<Dir>(d));
        }
        // Wall-Pit (all dirs — walls can border pits)
        for (int d = 0; d < 4; d++) {
            BIADJ(gen, 1, 4, static_cast<Dir>(d));
        }
        // Door-Door (all dirs — double doors)
        for (int d = 0; d < 4; d++) {
            BIADJ(gen, 2, 2, static_cast<Dir>(d));
        }
        // Pit-Pit (all dirs — pits form pools)
        for (int d = 0; d < 4; d++) {
            BIADJ(gen, 4, 4, static_cast<Dir>(d));
        }
        // Door-Pit (door can border pit — floodgate?)
        for (int d = 0; d < 4; d++) {
            BIADJ(gen, 2, 4, static_cast<Dir>(d));
        }

        // Frequency: floor dominant, walls common, pits uncommon, pillars rare, doors rare
        gen.set_frequency(0, 60.0);  // floor
        gen.set_frequency(1, 25.0);  // wall
        gen.set_frequency(2, 2.0);   // door
        gen.set_frequency(3, 4.0);   // pillar
        gen.set_frequency(4, 9.0);   // pit

        auto grid = gen.run(seed);
        printf("=== Seed %d ===\n", seed);
        printf("%s\n", gen.to_ascii(grid).c_str());
    }

    return 0;
}