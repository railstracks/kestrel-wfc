// test_room_builder.cpp — WFC → 3D room definition test
// g++ -std=c++17 -O2 -o test_room_builder test_room_builder.cpp && ./test_room_builder
//
// Generates WFC rooms, converts to 3D RoomDefinitions, outputs ASCII preview
// and JSON for one seed. Validates that geometry is well-formed.

#include "wfc.hpp"
#include "room_builder.hpp"
#include <cstdio>
#include <string>
#include <cstdlib>

// Helper: bidirectional adjacency
#define BIADJ(gen, a, b, dir) do { \
    gen.set_adjacency(a, b, dir); \
    gen.set_adjacency(b, a, wfc::opposite(dir)); \
} while(0)

// Build the dungeon tileset generator (same as test_wfc2.cpp)
static wfc::Generator make_dungeon_gen(int w, int h) {
    using namespace wfc;
    std::vector<Tile> tiles = {
        {0, "floor",   '.'},
        {1, "wall",    '#'},
        {2, "door",    '+'},
        {3, "pillar",  'O'},
        {4, "pit",     '~'},
    };

    Generator gen(w, h, tiles);

    // Floor connects to everything
    for (int d = 0; d < 4; d++) {
        BIADJ(gen, 0, 0, static_cast<Dir>(d));
        BIADJ(gen, 0, 3, static_cast<Dir>(d));
        BIADJ(gen, 0, 2, static_cast<Dir>(d));
        BIADJ(gen, 0, 4, static_cast<Dir>(d));
        BIADJ(gen, 0, 1, static_cast<Dir>(d));
    }
    // Wall connects to wall, door, floor, pit
    for (int d = 0; d < 4; d++) {
        BIADJ(gen, 1, 1, static_cast<Dir>(d));
        BIADJ(gen, 1, 2, static_cast<Dir>(d));
        BIADJ(gen, 1, 4, static_cast<Dir>(d));
    }
    // Door connects to door, pit
    for (int d = 0; d < 4; d++) {
        BIADJ(gen, 2, 2, static_cast<Dir>(d));
        BIADJ(gen, 2, 4, static_cast<Dir>(d));
    }
    // Pit connects to pit
    for (int d = 0; d < 4; d++) {
        BIADJ(gen, 4, 4, static_cast<Dir>(d));
    }

    gen.set_frequency(0, 60.0);
    gen.set_frequency(1, 25.0);
    gen.set_frequency(2, 2.0);
    gen.set_frequency(3, 4.0);
    gen.set_frequency(4, 9.0);

    return gen;
}

int main(int argc, char* argv[]) {
    int target_seed = 1;
    int grid_w = 20, grid_h = 14;
    bool output_json = false;

    if (argc > 1) target_seed = atoi(argv[1]);
    bool lodestone_json = false;
    if (argc > 2) {
        std::string mode(argv[2]);
        output_json = (mode == "--json");
        lodestone_json = (mode == "--lodestone");
    }

    if (!output_json && !lodestone_json)
        printf("=== WFC Room Builder Test ===\n\n");

    // Generate multiple seeds, show stats
    int successes = 0, failures = 0;
    int total_floors = 0, total_walls = 0, total_doors = 0, total_pillars = 0, total_pits = 0;

    for (int seed = 1; seed <= 20; seed++) {
        auto gen = make_dungeon_gen(grid_w, grid_h);
        auto grid = gen.run(seed);
        if (grid.empty()) {
            failures++;
            continue;
        }
        successes++;

        // Count tile types
        int counts[5] = {0, 0, 0, 0, 0};
        for (int t : grid) counts[t]++;

        total_floors += counts[0];
        total_walls += counts[1];
        total_doors += counts[2];
        total_pillars += counts[3];
        total_pits += counts[4];

        if (seed == target_seed) {
            room::RoomBuilder builder(gen, grid, 1.0f, 3.0f, 0.0f, 2.0f);
            auto room = builder.build();

            if (output_json) {
                builder.write_json(room, stdout);
            } else if (lodestone_json) {
                builder.write_lodestone_json(room, stdout, seed);
            } else {
                printf("--- Target seed %d ---\n", seed);
                printf("%s\n", gen.to_ascii(grid).c_str());
                printf("%s\n", builder.to_elevation_ascii(room).c_str());

                // Validate room
                printf("\n--- Validation ---\n");
                printf("Floor quads: %zu  (expected ~%d)\n", room.floor_quads.size(), counts[0] + counts[2] + counts[3]);
                printf("Wall boxes:  %zu  (expected %d)\n", room.wall_boxes.size(), counts[1]);
                printf("Pillar boxes: %zu  (expected %d)\n", room.pillar_boxes.size(), counts[3]);
                printf("Pit quads:   %zu  (expected %d)\n", room.pit_quads.size(), counts[4]);
                printf("Door zones:  %zu  (expected %d)\n", room.doors.size(), counts[2]);
                printf("Spawns:      %zu\n", room.spawns.size());

                // Check spawn is on a floor tile
                if (!room.spawns.empty()) {
                    int sx = static_cast<int>(room.spawns[0].position.x);
                    int sy = static_cast<int>(room.spawns[0].position.z);
                    if (sx >= 0 && sx < grid_w && sy >= 0 && sy < grid_h) {
                        int tile = grid[sy * grid_w + sx];
                        printf("Spawn at (%d, %d) on tile type %d — %s\n",
                               sx, sy, tile,
                               (tile == 0 || tile == 2 || tile == 3) ? "OK (walkable)" : "BAD (non-walkable!)");
                    }
                }

                // Check door directions
                int bad_doors = 0;
                for (const auto& d : room.doors) {
                    int gx = d.grid_x, gy = d.grid_y;
                    bool has_wall_neighbor = false;
                    if (gx + 1 < grid_w && grid[gy * grid_w + gx + 1] == 1) has_wall_neighbor = true;
                    if (gx > 0 && grid[gy * grid_w + gx - 1] == 1) has_wall_neighbor = true;
                    if (gy > 0 && grid[(gy - 1) * grid_w + gx] == 1) has_wall_neighbor = true;
                    if (gy + 1 < grid_h && grid[(gy + 1) * grid_w + gx] == 1) has_wall_neighbor = true;
                    if (!has_wall_neighbor) bad_doors++;
                }
                printf("Doors without wall neighbors: %d\n", bad_doors);
            }
        }
    }

    if (!output_json && !lodestone_json) {
        printf("\n=== Summary (20 seeds, %dx%d) ===\n", grid_w, grid_h);
        printf("Success rate: %d/20 (%.0f%%)\n", successes, successes / 20.0 * 100);
        printf("Avg tiles: floor=%d  wall=%d  door=%d  pillar=%d  pit=%d\n",
               total_floors / 20, total_walls / 20, total_doors / 20,
               total_pillars / 20, total_pits / 20);
        printf("Avg floor coverage: %.1f%%\n",
               (double)total_floors / (20.0 * grid_w * grid_h) * 100.0);
    }

    return 0;
}