#ifndef KESTREL_SECTOR_GEN_HPP
#define KESTREL_SECTOR_GEN_HPP

//
// sector_gen.hpp — Top-level sector generation pipeline.
//
// Integrates graph_gen (mission structure) + WFC (room interiors) + room_builder
// (3D geometry) into a single pipeline that produces a complete multi-room sector
// loadable by the Lodestone engine.
//
// Pipeline:
//   1. graph_gen: Generate mission graph (nodes=rooms, edges=connections)
//   2. Spatial layout: Assign (grid_x, grid_y) to each node via BFS spacing
//   3. Per-room WFC: Generate tile grid for each room, with door constraints
//   4. room_builder: Convert each room to 3D geometry, offset by room position
//   5. Merge: Combine all rooms into one Lodestone scene JSON
//
// Door alignment:
//   Each room knows which of its edges connect to neighbors. Doors are placed
//   on the wall facing the neighbor. The WFC tileset includes a Door tile that
//   the sector generator injects at the correct boundary position.
//
// Usage:
//   sector::SectorGenerator sg(42);  // seed
//   sg.generate_starfall_drift_sector();
//   sg.write_lodestone_scene(stdout);
//

#include "wfc.hpp"
#include "room_builder.hpp"
#include "graph_gen.hpp"
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <cmath>
#include <random>
#include <algorithm>

namespace sector {

// Room tileset definition — what tiles are available and how they connect
struct TilesetDef {
    std::vector<wfc::Tile> tiles;
    // Adjacency rules: (from_tile, to_tile, direction)
    std::vector<std::tuple<int, int, wfc::Dir>> adjacency;
    // Frequency weights
    std::unordered_map<int, double> frequencies;
};

// A generated room with its position and connections
struct SectorRoom {
    graph_gen::NodeId graph_node_id;
    int grid_x, grid_y;         // Sector grid position (not world coords)
    float world_x, world_z;     // World-space center of this room
    int room_w, room_h;         // Room interior dimensions (tiles)
    std::vector<int> tile_grid;  // WFC output
    room::RoomDefinition room_def;

    // Connections: direction → neighbor graph node ID
    // direction: 0=right, 1=left, 2=up, 3=down (matches room::DoorZone)
    struct Connection {
        int direction;
        graph_gen::NodeId neighbor;
        graph_gen::EdgeId edge_id;
    };
    std::vector<Connection> connections;
};

class SectorGenerator {
public:
    SectorGenerator(uint64_t seed = 0)
        : rng_(seed), seed_(seed),
          room_w_(8), room_h_(8), room_spacing_(2), tile_size_(1.0f),
          wall_height_(3.0f), floor_y_(0.0f), pit_depth_(2.0f)
    {}

    // === Configuration ===

    void set_room_size(int w, int h) { room_w_ = w; room_h_ = h; }
    void set_room_spacing(int s) { room_spacing_ = s; }
    void set_tile_size(float ts) { tile_size_ = ts; }
    void set_wall_height(float wh) { wall_height_ = wh; }

    // === Starfall Drift sector ===
    // Uses the 5-tile dungeon tileset with void biome theming

    void generate_starfall_drift_sector() {
        // Step 1: Generate mission graph
        graph_gen::Generator ggen(static_cast<uint32_t>(seed_ & 0xFFFFFFFF));
        auto [entrance, goal] = ggen.make_cycle(5 + (seed_ % 3), "void");

        // Rewrite rules (same as test_graph_gen)
        graph_gen::RewriteRule r_obstacle;
        r_obstacle.pattern_nodes = {graph_gen::NodeKind::Path};
        r_obstacle.replacement_nodes = {{graph_gen::NodeKind::Obstacle, "obstacle", "void"}, {graph_gen::NodeKind::Path, "path", "void"}};
        r_obstacle.replacement_edges = {{0, 1}};
        r_obstacle.rewire_map = {{0, 0}};
        r_obstacle.weight = 3;
        r_obstacle.max_applications = 4;

        graph_gen::RewriteRule r_reward;
        r_reward.pattern_nodes = {graph_gen::NodeKind::Path};
        r_reward.replacement_nodes = {{graph_gen::NodeKind::Path, "path", "void"}, {graph_gen::NodeKind::Reward, "reward", "void"}};
        r_reward.replacement_edges = {{0, 1}};
        r_reward.rewire_map = {{0, 0}};
        r_reward.weight = 2;
        r_reward.max_applications = 3;

        graph_gen::RewriteRule r_guardian;
        r_guardian.pattern_nodes = {graph_gen::NodeKind::Obstacle};
        r_guardian.replacement_nodes = {{graph_gen::NodeKind::Guardian, "guardian", "combat"}, {graph_gen::NodeKind::Obstacle, "obstacle", "void"}};
        r_guardian.replacement_edges = {{0, 1}};
        r_guardian.rewire_map = {{0, 0}};
        r_guardian.weight = 2;
        r_guardian.max_applications = 2;

        std::vector<graph_gen::RewriteRule> rules = {r_obstacle, r_reward, r_guardian};
        ggen.apply_rules_randomly(rules, 30);
        ggen.place_lock_key(entrance, goal);
        ggen.resolve_with_biome();

        graph_ = ggen;

        // Step 2: Spatial layout
        layout_rooms(entrance);

        // Step 3: Generate rooms
        tileset_ = make_dungeon_tileset();
        for (auto& [node_id, room] : rooms_) {
            generate_room(room);
        }
    }

    // === Output ===

    void write_lodestone_scene(FILE* out) const {
        // Count total entities across all rooms
        int total_entities = 0;
        for (const auto& [id, room] : rooms_) {
            total_entities += count_room_entities(room);
        }
        total_entities += 1; // light

        fprintf(out, "{\n");
        fprintf(out, "  \"schemaVersion\": 2,\n");
        fprintf(out, "  \"id\": \"sector.%llu\",\n", (unsigned long long)seed_);
        fprintf(out, "  \"name\": \"Generated Sector (seed %llu)\",\n", (unsigned long long)seed_);
        fprintf(out, "  \"dimensionality\": \"3D\",\n");
        fprintf(out, "  \"settings\": {\n");
        fprintf(out, "    \"viewportCameraId\": 0,\n");
        fprintf(out, "    \"primaryCameraEntityId\": 0,\n");
        fprintf(out, "    \"ambientColor\": [0.12, 0.12, 0.18],\n");
        fprintf(out, "    \"gravity\": [0.0, -9.8, 0.0]\n");
        fprintf(out, "  },\n");

        // Build entities
        std::vector<std::string> entity_jsons;
        int entity_id = 1;

        // First: find entrance room for player spawn
        graph_gen::NodeId entrance_node = find_entrance();

        for (const auto& [node_id, room] : rooms_) {
            // Generate room entities with offset
            auto room_entities = build_room_entities(room, entity_id, node_id == entrance_node);
            entity_id += room_entities.size();
            for (auto& e : room_entities) {
                entity_jsons.push_back(e);
            }
        }

        // Directional light (sector-level)
        {
            char buf[512];
            snprintf(buf, sizeof(buf),
                "    {\n"
                "      \"entityId\": %d,\n"
                "      \"name\": \"sun_light\",\n"
                "      \"parentEntityId\": 0,\n"
                "      \"transform\": { \"position\": [0, 15, 0], \"orientation\": [0, 0, 0, 1], \"scale\": [1, 1, 1] },\n"
                "      \"components\": {\n"
                "        \"light\": {\n"
                "          \"type\": \"directional\",\n"
                "          \"color\": [1.0, 0.95, 0.85],\n"
                "          \"intensity\": 0.7,\n"
                "          \"direction\": [-0.3, -1.0, -0.2]\n"
                "        }\n"
                "      }\n"
                "    }",
                entity_id);
            entity_jsons.push_back(std::string(buf));
            entity_id++;
        }

        fprintf(out, "  \"entities\": [\n");
        for (size_t i = 0; i < entity_jsons.size(); i++) {
            fprintf(out, "%s%s\n", entity_jsons[i].c_str(), i < entity_jsons.size() - 1 ? "," : "");
        }
        fprintf(out, "  ]\n");
        fprintf(out, "}\n");
    }

    // === Diagnostics ===

    void print_summary() const {
        printf("=== Sector Summary (seed %llu) ===\n", (unsigned long long)seed_);
        printf("  Rooms: %zu\n", rooms_.size());
        printf("  Graph nodes: %zu, edges: %zu\n", graph_.nodes().size(), graph_.edges().size());

        for (const auto& [id, room] : rooms_) {
            const auto& node = graph_.nodes().at(id);
            printf("  Room [%d] %s at grid(%d,%d) world(%.1f,%.1f) size(%dx%d) connections=%zu\n",
                   id, graph_gen::node_kind_str(node.kind),
                   room.grid_x, room.grid_y, room.world_x, room.world_z,
                   room.room_w, room.room_h, room.connections.size());

            for (const auto& conn : room.connections) {
                const char* dir_str = conn.direction == 0 ? "right" : conn.direction == 1 ? "left" :
                                      conn.direction == 2 ? "up" : "down";
                printf("    -> %s (node %d)\n", dir_str, conn.neighbor);
            }
        }
    }

    std::string to_ascii_map() const {
        // Build a sector-level ASCII map showing room positions and connections
        if (rooms_.empty()) return "(empty sector)\n";

        int min_x = 0, max_x = 0, min_y = 0, max_y = 0;
        for (const auto& [id, room] : rooms_) {
            min_x = std::min(min_x, room.grid_x);
            max_x = std::max(max_x, room.grid_x);
            min_y = std::min(min_y, room.grid_y);
            max_y = std::max(max_y, room.grid_y);
        }

        int w = max_x - min_x + 1;
        int h = max_y - min_y + 1;
        std::string out;
        out += "=== Sector Map ===\n\n";

        // Build grid
        std::vector<std::string> grid(h * 2 + 1, std::string(w * 4 + 1, ' '));

        for (const auto& [id, room] : rooms_) {
            int gx = room.grid_x - min_x;
            int gy = room.grid_y - min_y;
            const auto& node = graph_.nodes().at(id);
            char glyph = 'R';
            switch (node.kind) {
                case graph_gen::NodeKind::Entrance: glyph = 'E'; break;
                case graph_gen::NodeKind::Goal: glyph = 'G'; break;
                case graph_gen::NodeKind::Combat: glyph = 'C'; break;
                case graph_gen::NodeKind::Anomaly: glyph = 'A'; break;
                case graph_gen::NodeKind::Station: glyph = 'S'; break;
                case graph_gen::NodeKind::Trade: glyph = 'T'; break;
                case graph_gen::NodeKind::Derelict: glyph = 'D'; break;
                case graph_gen::NodeKind::Lock: glyph = 'L'; break;
                case graph_gen::NodeKind::Key: glyph = 'K'; break;
                default: glyph = 'R'; break;
            }

            // Place room symbol
            int cx = gx * 4 + 2;
            int cy = gy * 2 + 1;
            if (cy >= 0 && cy < (int)grid.size() && cx >= 0 && cx < (int)grid[0].size()) {
                grid[cy][cx] = glyph;
                // Draw box around room
                if (cx - 2 >= 0) grid[cy][cx-2] = '[';
                if (cx + 2 < (int)grid[0].size()) grid[cy][cx+2] = ']';
            }

            // Draw connections
            for (const auto& conn : room.connections) {
                int dx = conn.direction == 0 ? 1 : conn.direction == 1 ? -1 : 0;
                int dy = conn.direction == 2 ? -1 : conn.direction == 3 ? 1 : 0;
                int ex = cx + dx * 2;
                int ey = cy + dy;
                if (ex >= 0 && ex < (int)grid[0].size() && ey >= 0 && ey < (int)grid.size()) {
                    char arrow = conn.direction == 0 ? '-' : conn.direction == 1 ? '-' :
                                 conn.direction == 2 ? '|' : '|';
                    if (grid[ey][ex] == ' ') grid[ey][ex] = arrow;
                }
            }
        }

        for (const auto& row : grid) {
            // Trim trailing spaces
            std::string trimmed = row;
            while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
            if (!trimmed.empty()) {
                out += trimmed;
                out += '\n';
            }
        }

        out += "\nLegend: E=Entrance G=Goal C=Combat A=Anomaly S=Station T=Trade\n";
        out += "       D=Derelict L=Lock K=Key R=Room -=horizontal |=vertical\n";
        return out;
    }

private:
    uint64_t seed_;
    std::mt19937_64 rng_;
    graph_gen::Generator graph_;
    TilesetDef tileset_;
    std::unordered_map<graph_gen::NodeId, SectorRoom> rooms_;

    int room_w_, room_h_, room_spacing_;
    float tile_size_, wall_height_, floor_y_, pit_depth_;

    // === Tileset ===

    TilesetDef make_dungeon_tileset() {
        // 5 tiles: floor, wall, door, pillar, pit
        TilesetDef ts;
        ts.tiles = {
            {0, "floor",  '.'},
            {1, "wall",   '#'},
            {2, "door",   '+'},
            {3, "pillar", 'O'},
            {4, "pit",    '~'},
        };

        // Adjacency rules (Wang-style edge constraints)
        // Floor connects to floor, door, pillar (not wall, not pit directly)
        ts.adjacency.push_back({0, 0, wfc::Dir::Right});  // floor → floor
        ts.adjacency.push_back({0, 0, wfc::Dir::Left});
        ts.adjacency.push_back({0, 0, wfc::Dir::Up});
        ts.adjacency.push_back({0, 0, wfc::Dir::Down});
        ts.adjacency.push_back({0, 1, wfc::Dir::Right});  // floor → wall (boundary)
        ts.adjacency.push_back({0, 1, wfc::Dir::Left});
        ts.adjacency.push_back({0, 1, wfc::Dir::Up});
        ts.adjacency.push_back({0, 1, wfc::Dir::Down});
        ts.adjacency.push_back({0, 2, wfc::Dir::Right});  // floor → door
        ts.adjacency.push_back({0, 2, wfc::Dir::Left});
        ts.adjacency.push_back({0, 2, wfc::Dir::Up});
        ts.adjacency.push_back({0, 2, wfc::Dir::Down});
        ts.adjacency.push_back({0, 3, wfc::Dir::Right});  // floor → pillar
        ts.adjacency.push_back({0, 3, wfc::Dir::Left});
        ts.adjacency.push_back({0, 3, wfc::Dir::Up});
        ts.adjacency.push_back({0, 3, wfc::Dir::Down});
        ts.adjacency.push_back({0, 4, wfc::Dir::Right});  // floor → pit (edge)
        ts.adjacency.push_back({0, 4, wfc::Dir::Left});
        ts.adjacency.push_back({0, 4, wfc::Dir::Up});
        ts.adjacency.push_back({0, 4, wfc::Dir::Down});

        // Wall connects to wall, floor, door, pit, pillar
        ts.adjacency.push_back({1, 1, wfc::Dir::Right});
        ts.adjacency.push_back({1, 1, wfc::Dir::Left});
        ts.adjacency.push_back({1, 1, wfc::Dir::Up});
        ts.adjacency.push_back({1, 1, wfc::Dir::Down});
        ts.adjacency.push_back({1, 0, wfc::Dir::Right});
        ts.adjacency.push_back({1, 0, wfc::Dir::Left});
        ts.adjacency.push_back({1, 0, wfc::Dir::Up});
        ts.adjacency.push_back({1, 0, wfc::Dir::Down});
        ts.adjacency.push_back({1, 2, wfc::Dir::Right});
        ts.adjacency.push_back({1, 2, wfc::Dir::Left});
        ts.adjacency.push_back({1, 2, wfc::Dir::Up});
        ts.adjacency.push_back({1, 2, wfc::Dir::Down});
        ts.adjacency.push_back({1, 4, wfc::Dir::Right});
        ts.adjacency.push_back({1, 4, wfc::Dir::Left});
        ts.adjacency.push_back({1, 4, wfc::Dir::Up});
        ts.adjacency.push_back({1, 4, wfc::Dir::Down});
        ts.adjacency.push_back({1, 3, wfc::Dir::Right});
        ts.adjacency.push_back({1, 3, wfc::Dir::Left});
        ts.adjacency.push_back({1, 3, wfc::Dir::Up});
        ts.adjacency.push_back({1, 3, wfc::Dir::Down});

        // Door connects to floor, wall (door is in a wall opening)
        ts.adjacency.push_back({2, 0, wfc::Dir::Right});
        ts.adjacency.push_back({2, 0, wfc::Dir::Left});
        ts.adjacency.push_back({2, 0, wfc::Dir::Up});
        ts.adjacency.push_back({2, 0, wfc::Dir::Down});
        ts.adjacency.push_back({2, 1, wfc::Dir::Right});
        ts.adjacency.push_back({2, 1, wfc::Dir::Left});
        ts.adjacency.push_back({2, 1, wfc::Dir::Up});
        ts.adjacency.push_back({2, 1, wfc::Dir::Down});
        ts.adjacency.push_back({2, 2, wfc::Dir::Right});
        ts.adjacency.push_back({2, 2, wfc::Dir::Left});
        ts.adjacency.push_back({2, 2, wfc::Dir::Up});
        ts.adjacency.push_back({2, 2, wfc::Dir::Down});

        // Pillar connects to floor (pillar sits on floor)
        ts.adjacency.push_back({3, 0, wfc::Dir::Right});
        ts.adjacency.push_back({3, 0, wfc::Dir::Left});
        ts.adjacency.push_back({3, 0, wfc::Dir::Up});
        ts.adjacency.push_back({3, 0, wfc::Dir::Down});
        ts.adjacency.push_back({3, 1, wfc::Dir::Right});
        ts.adjacency.push_back({3, 1, wfc::Dir::Left});
        ts.adjacency.push_back({3, 1, wfc::Dir::Up});
        ts.adjacency.push_back({3, 1, wfc::Dir::Down});

        // Pit connects to pit, wall, floor (pit is open area bounded by walls/floor)
        ts.adjacency.push_back({4, 4, wfc::Dir::Right});
        ts.adjacency.push_back({4, 4, wfc::Dir::Left});
        ts.adjacency.push_back({4, 4, wfc::Dir::Up});
        ts.adjacency.push_back({4, 4, wfc::Dir::Down});
        ts.adjacency.push_back({4, 1, wfc::Dir::Right});
        ts.adjacency.push_back({4, 1, wfc::Dir::Left});
        ts.adjacency.push_back({4, 1, wfc::Dir::Up});
        ts.adjacency.push_back({4, 1, wfc::Dir::Down});
        ts.adjacency.push_back({4, 0, wfc::Dir::Right});
        ts.adjacency.push_back({4, 0, wfc::Dir::Left});
        ts.adjacency.push_back({4, 0, wfc::Dir::Up});
        ts.adjacency.push_back({4, 0, wfc::Dir::Down});

        // Frequencies: floor common, wall common, door rare, pillar rare, pit uncommon
        ts.frequencies[0] = 10.0;  // floor
        ts.frequencies[1] = 4.0;   // wall
        ts.frequencies[2] = 0.5;   // door
        ts.frequencies[3] = 0.8;   // pillar
        ts.frequencies[4] = 1.5;   // pit

        return ts;
    }

    // === Layout ===

    void layout_rooms(graph_gen::NodeId entrance) {
        // BFS from entrance, assigning grid positions
        // Use a simple spacing scheme: each room is 1 grid cell apart
        std::vector<graph_gen::NodeId> queue = {entrance};
        std::unordered_set<graph_gen::NodeId> visited = {entrance};

        // Assign entrance to (0, 0)
        auto& entrance_room = rooms_[entrance];
        entrance_room.graph_node_id = entrance;
        entrance_room.grid_x = 0;
        entrance_room.grid_y = 0;
        entrance_room.room_w = room_w_;
        entrance_room.room_h = room_h_;

        // BFS direction preferences: try right, down, left, up in order
        const int dx[] = {1, 0, -1, 0};
        const int dy[] = {0, 1, 0, -1};

        std::unordered_set<int> used_positions;  // encoded as y*1000 + x
        used_positions.insert(0);

        while (!queue.empty()) {
            std::vector<graph_gen::NodeId> next;
            for (graph_gen::NodeId current : queue) {
                auto& room = rooms_[current];
                // Get all neighbors
                auto& adj = graph_.adjacency(current);
                int dir_idx = 0;
                for (graph_gen::EdgeId eid : adj) {
                    const auto& edge = graph_.edges().at(eid);
                    graph_gen::NodeId other = (edge.a == current) ? edge.b : edge.a;
                    if (visited.count(other)) {
                        // Already placed — record connection direction
                        auto& other_room = rooms_[other];
                        int dir = direction_between(room.grid_x, room.grid_y,
                                                    other_room.grid_x, other_room.grid_y);
                        if (dir >= 0) {
                            room.connections.push_back({dir, other, eid});
                            // Also add reverse connection
                            int rev_dir = (dir + 1) % 2 == 0 ? dir + 1 : dir - 1;
                            bool found = false;
                            for (const auto& c : other_room.connections) {
                                if (c.neighbor == current) { found = true; break; }
                            }
                            if (!found) {
                                other_room.connections.push_back({rev_dir, current, eid});
                            }
                        }
                        continue;
                    }

                    // Find a free grid position
                    int nx = room.grid_x + dx[dir_idx % 4];
                    int ny = room.grid_y + dy[dir_idx % 4];
                    dir_idx++;

                    // Search spirally if immediate position taken
                    bool found_pos = false;
                    for (int radius = 0; radius < 10 && !found_pos; radius++) {
                        for (int d = 0; d < 4 && !found_pos; d++) {
                            int tx = room.grid_x + dx[d] * (1 + radius / 2);
                            int ty = room.grid_y + dy[d] * (1 + radius / 2);
                            int key = ty * 1000 + tx;
                            if (!used_positions.count(key)) {
                                nx = tx; ny = ty;
                                found_pos = true;
                            }
                        }
                    }

                    int key = ny * 1000 + nx;
                    if (used_positions.count(key)) continue;  // skip if still taken
                    used_positions.insert(key);
                    visited.insert(other);

                    auto& new_room = rooms_[other];
                    new_room.graph_node_id = other;
                    new_room.grid_x = nx;
                    new_room.grid_y = ny;
                    new_room.room_w = room_w_;
                    new_room.room_h = room_h_;

                    // Record connection (current → other)
                    int dir = direction_between(room.grid_x, room.grid_y, nx, ny);
                    if (dir >= 0) {
                        room.connections.push_back({dir, other, eid});
                        int rev_dir = (dir + 1) % 2 == 0 ? dir + 1 : dir - 1;
                        new_room.connections.push_back({rev_dir, current, eid});
                    }

                    next.push_back(other);
                }
            }
            queue = std::move(next);
        }

        // Compute world positions
        // Each room occupies room_w * tile_size meters, plus room_spacing * tile_size gap
        float room_world_size_w = (room_w_ + room_spacing_) * tile_size_;
        float room_world_size_h = (room_h_ + room_spacing_) * tile_size_;

        for (auto& [id, room] : rooms_) {
            room.world_x = room.grid_x * room_world_size_w;
            room.world_z = room.grid_y * room_world_size_h;
        }
    }

    int direction_between(int x1, int y1, int x2, int y2) const {
        int dx = x2 - x1;
        int dy = y2 - y1;
        if (dx > 0 && dy == 0) return 0;  // right
        if (dx < 0 && dy == 0) return 1;  // left
        if (dx == 0 && dy < 0) return 2;  // up (negative y = up in grid)
        if (dx == 0 && dy > 0) return 3;  // down
        // Diagonal — pick dominant axis
        if (abs(dx) > abs(dy)) return dx > 0 ? 0 : 1;
        return dy > 0 ? 3 : 2;
    }

    // === Room generation ===

    void generate_room(SectorRoom& room) {
        int w = room.room_w;
        int h = room.room_h;

        // Create WFC generator with dungeon tileset
        wfc::Generator wgen(w, h, tileset_.tiles);
        for (auto& [from, to, dir] : tileset_.adjacency) {
            wgen.set_adjacency(from, to, dir);
        }
        for (auto& [tile, freq] : tileset_.frequencies) {
            wgen.set_frequency(tile, freq);
        }

        // Run WFC with a seed derived from the global seed + room position
        uint64_t room_seed = seed_ ^ (static_cast<uint64_t>(room.graph_node_id) * 0x9E3779B97F4A7C15ULL);
        auto grid = wgen.run(room_seed);

        if (grid.empty()) {
            // WFC contradiction — retry with different seed, or use fallback grid
            // Fallback: simple room with walls on border, floor inside
            grid.assign(w * h, room::FLOOR);
            for (int x = 0; x < w; x++) {
                grid[0 * w + x] = room::WALL;
                grid[(h-1) * w + x] = room::WALL;
            }
            for (int y = 0; y < h; y++) {
                grid[y * w + 0] = room::WALL;
                grid[y * w + (w-1)] = room::WALL;
            }
        }

        // Inject doors at connection points
        inject_doors(room, grid);

        room.tile_grid = std::move(grid);

        // Build room definition using room_builder
        wfc::Generator wgen2(w, h, tileset_.tiles);
        room::RoomBuilder builder(wgen2, room.tile_grid, tile_size_, wall_height_, floor_y_, pit_depth_);
        room.room_def = builder.build();
    }

    void inject_doors(SectorRoom& room, std::vector<int>& grid) {
        int w = room.room_w;
        int h = room.room_h;

        for (const auto& conn : room.connections) {
            int door_x = -1, door_y = -1;

            switch (conn.direction) {
                case 0: // right — door on right wall, middle height
                    door_x = w - 1;
                    door_y = h / 2;
                    break;
                case 1: // left — door on left wall, middle height
                    door_x = 0;
                    door_y = h / 2;
                    break;
                case 2: // up — door on top wall, middle width
                    door_x = w / 2;
                    door_y = 0;
                    break;
                case 3: // down — door on bottom wall, middle width
                    door_x = w / 2;
                    door_y = h - 1;
                    break;
            }

            if (door_x >= 0 && door_x < w && door_y >= 0 && door_y < h) {
                grid[door_y * w + door_x] = room::DOOR;
                // Also clear adjacent tiles to make a passage
                if (conn.direction == 0 || conn.direction == 1) {
                    // Horizontal door: clear floor tile next to it
                    int inner_x = (conn.direction == 0) ? door_x - 1 : door_x + 1;
                    if (inner_x >= 0 && inner_x < w) {
                        grid[door_y * w + inner_x] = room::FLOOR;
                    }
                } else {
                    // Vertical door: clear floor tile next to it
                    int inner_y = (conn.direction == 2) ? door_y + 1 : door_y - 1;
                    if (inner_y >= 0 && inner_y < h) {
                        grid[inner_y * w + door_x] = room::FLOOR;
                    }
                }
            }
        }
    }

    // === Entity building for Lodestone JSON ===

    int count_room_entities(const SectorRoom& room) const {
        // Count floor strips, wall strips, pillars, pits, doors
        int count = 0;
        int w = room.room_w, h = room.room_h;
        const auto& grid = room.tile_grid;

        // Count floor strips (X-axis merged)
        for (int y = 0; y < h; y++) {
            int x = 0;
            while (x < w) {
                int t = grid[y * w + x];
                if (t == room::FLOOR || t == room::DOOR || t == room::PILLAR) {
                    while (x < w && (grid[y * w + x] == room::FLOOR || grid[y * w + x] == room::DOOR || grid[y * w + x] == room::PILLAR)) x++;
                    count++;
                } else { x++; }
            }
        }
        // Wall strips
        for (int y = 0; y < h; y++) {
            int x = 0;
            while (x < w) {
                if (grid[y * w + x] == room::WALL) {
                    while (x < w && grid[y * w + x] == room::WALL) x++;
                    count++;
                } else { x++; }
            }
        }
        // Pit strips
        for (int y = 0; y < h; y++) {
            int x = 0;
            while (x < w) {
                if (grid[y * w + x] == room::PIT) {
                    while (x < w && grid[y * w + x] == room::PIT) x++;
                    count++;
                } else { x++; }
            }
        }
        // Pillars
        count += room.room_def.pillar_boxes.size();
        // Doors
        count += room.room_def.doors.size();
        // Spawn (only for entrance)
        // Light is added at sector level
        return count;
    }

    std::vector<std::string> build_room_entities(const SectorRoom& room, int start_id, bool is_entrance) const {
        std::vector<std::string> entities;
        int entity_id = start_id;
        int w = room.room_w, h = room.room_h;
        const auto& grid = room.tile_grid;
        float ox = room.world_x;  // offset x
        float oz = room.world_z;  // offset z

        // Floor strips (X-axis merged)
        for (int y = 0; y < h; y++) {
            int x = 0;
            while (x < w) {
                int t = grid[y * w + x];
                bool has_floor = (t == room::FLOOR || t == room::DOOR || t == room::PILLAR);
                if (!has_floor) { x++; continue; }
                int x_start = x;
                while (x < w) {
                    int tt = grid[y * w + x];
                    if (!(tt == room::FLOOR || tt == room::DOOR || tt == room::PILLAR)) break;
                    x++;
                }
                int strip_len = x - x_start;
                float cx = (x_start + (x - 1)) * 0.5f * tile_size_ + ox;
                float cz = y * tile_size_ + oz;
                float strip_width = strip_len * tile_size_;

                char buf[512];
                snprintf(buf, sizeof(buf),
                    "    {\n"
                    "      \"entityId\": %d,\n"
                    "      \"name\": \"r%d_floor_y%d_x%d\",\n"
                    "      \"parentEntityId\": 0,\n"
                    "      \"transform\": { \"position\": [%.3f, %.3f, %.3f], \"orientation\": [0, 0, 0, 1], \"scale\": [1, 1, 1] },\n"
                    "      \"components\": {\n"
                    "        \"mesh\": {\n"
                    "          \"primitive\": \"plane\",\n"
                    "          \"dimensions\": [%.3f, 1.0, %.3f],\n"
                    "          \"material\": { \"albedo\": [0.5, 0.45, 0.4], \"opacity\": 1.0 }\n"
                    "        }\n"
                    "      }\n"
                    "    }",
                    entity_id, room.graph_node_id, y, x_start,
                    cx, floor_y_, cz, strip_width, tile_size_);
                entities.push_back(std::string(buf));
                entity_id++;
            }
        }

        // Wall strips (X-axis merged)
        for (int y = 0; y < h; y++) {
            int x = 0;
            while (x < w) {
                if (grid[y * w + x] != room::WALL) { x++; continue; }
                int x_start = x;
                while (x < w && grid[y * w + x] == room::WALL) x++;
                int strip_len = x - x_start;
                float cx = (x_start + (x - 1)) * 0.5f * tile_size_ + ox;
                float cz = y * tile_size_ + oz;
                float strip_width = strip_len * tile_size_;
                float half_w = strip_width * 0.5f;
                float half_h = wall_height_ * 0.5f;
                float half_d = tile_size_ * 0.5f;

                char buf[700];
                snprintf(buf, sizeof(buf),
                    "    {\n"
                    "      \"entityId\": %d,\n"
                    "      \"name\": \"r%d_wall_y%d_x%d\",\n"
                    "      \"parentEntityId\": 0,\n"
                    "      \"transform\": { \"position\": [%.3f, %.3f, %.3f], \"orientation\": [0, 0, 0, 1], \"scale\": [1, 1, 1] },\n"
                    "      \"components\": {\n"
                    "        \"mesh\": {\n"
                    "          \"primitive\": \"box\",\n"
                    "          \"dimensions\": [%.3f, %.3f, %.3f],\n"
                    "          \"material\": { \"albedo\": [0.3, 0.28, 0.25], \"opacity\": 1.0 }\n"
                    "        },\n"
                    "        \"physicsBody\": {\n"
                    "          \"motionType\": \"static\",\n"
                    "          \"shapeType\": \"box\",\n"
                    "          \"mass\": 0.0,\n"
                    "          \"friction\": 0.8,\n"
                    "          \"restitution\": 0.0,\n"
                    "          \"halfExtents\": [%.3f, %.3f, %.3f]\n"
                    "        }\n"
                    "      }\n"
                    "    }",
                    entity_id, room.graph_node_id, y, x_start,
                    cx, half_h, cz,
                    strip_width, wall_height_, tile_size_,
                    half_w, half_h, half_d);
                entities.push_back(std::string(buf));
                entity_id++;
            }
        }

        // Pit strips
        for (int y = 0; y < h; y++) {
            int x = 0;
            while (x < w) {
                if (grid[y * w + x] != room::PIT) { x++; continue; }
                int x_start = x;
                while (x < w && grid[y * w + x] == room::PIT) x++;
                int strip_len = x - x_start;
                float cx = (x_start + (x - 1)) * 0.5f * tile_size_ + ox;
                float cz = y * tile_size_ + oz;
                float strip_width = strip_len * tile_size_;
                float pit_y = floor_y_ - pit_depth_;

                char buf[512];
                snprintf(buf, sizeof(buf),
                    "    {\n"
                    "      \"entityId\": %d,\n"
                    "      \"name\": \"r%d_pit_y%d_x%d\",\n"
                    "      \"parentEntityId\": 0,\n"
                    "      \"transform\": { \"position\": [%.3f, %.3f, %.3f], \"orientation\": [0, 0, 0, 1], \"scale\": [1, 1, 1] },\n"
                    "      \"components\": {\n"
                    "        \"mesh\": {\n"
                    "          \"primitive\": \"plane\",\n"
                    "          \"dimensions\": [%.3f, 1.0, %.3f],\n"
                    "          \"material\": { \"albedo\": [0.1, 0.08, 0.06], \"opacity\": 1.0 }\n"
                    "        }\n"
                    "      }\n"
                    "    }",
                    entity_id, room.graph_node_id, y, x_start,
                    cx, pit_y, cz, strip_width, tile_size_);
                entities.push_back(std::string(buf));
                entity_id++;
            }
        }

        // Pillars
        for (size_t i = 0; i < room.room_def.pillar_boxes.size(); i++) {
            const auto& b = room.room_def.pillar_boxes[i];
            float bx = b.center.x + ox;
            float bz = b.center.z + oz;

            char buf[700];
            snprintf(buf, sizeof(buf),
                "    {\n"
                "      \"entityId\": %d,\n"
                "      \"name\": \"r%d_pillar_%zu\",\n"
                "      \"parentEntityId\": 0,\n"
                "      \"transform\": { \"position\": [%.3f, %.3f, %.3f], \"orientation\": [0, 0, 0, 1], \"scale\": [1, 1, 1] },\n"
                "      \"components\": {\n"
                "        \"mesh\": {\n"
                "          \"primitive\": \"box\",\n"
                "          \"dimensions\": [%.3f, %.3f, %.3f],\n"
                "          \"material\": { \"albedo\": [0.4, 0.38, 0.35], \"opacity\": 1.0 }\n"
                "        },\n"
                "        \"physicsBody\": {\n"
                "          \"motionType\": \"static\",\n"
                "          \"shapeType\": \"box\",\n"
                "          \"mass\": 0.0,\n"
                "          \"friction\": 0.6,\n"
                "          \"restitution\": 0.1,\n"
                "          \"halfExtents\": [%.3f, %.3f, %.3f]\n"
                "        }\n"
                "      }\n"
                "    }",
                entity_id, room.graph_node_id, i,
                bx, b.center.y, bz,
                b.half_extents.x * 2, b.half_extents.y * 2, b.half_extents.z * 2,
                b.half_extents.x, b.half_extents.y, b.half_extents.z);
            entities.push_back(std::string(buf));
            entity_id++;
        }

        // Doors
        for (size_t i = 0; i < room.room_def.doors.size(); i++) {
            const auto& d = room.room_def.doors[i];
            float dx = d.center.x + ox;
            float dz = d.center.z + oz;

            char buf[600];
            snprintf(buf, sizeof(buf),
                "    {\n"
                "      \"entityId\": %d,\n"
                "      \"name\": \"r%d_door_%zu\",\n"
                "      \"parentEntityId\": 0,\n"
                "      \"transform\": { \"position\": [%.3f, %.3f, %.3f], \"orientation\": [0, 0, 0, 1], \"scale\": [1, 1, 1] },\n"
                "      \"components\": {\n"
                "        \"mesh\": {\n"
                "          \"primitive\": \"box\",\n"
                "          \"dimensions\": [%.3f, %.3f, 0.1],\n"
                "          \"material\": { \"albedo\": [0.35, 0.2, 0.1], \"opacity\": 1.0 }\n"
                "        },\n"
                "        \"interactable\": {\n"
                "          \"type\": \"door\",\n"
                "          \"prompt\": \"Enter next sector\",\n"
                "          \"range\": 2.5\n"
                "        }\n"
                "      }\n"
                "    }",
                entity_id, room.graph_node_id, i,
                dx, d.center.y, dz,
                d.width, d.height);
            entities.push_back(std::string(buf));
            entity_id++;
        }

        // Player spawn (only for entrance room)
        if (is_entrance && !room.room_def.spawns.empty()) {
            const auto& sp = room.room_def.spawns[0];
            float sx = sp.position.x + ox;
            float sz = sp.position.z + oz;

            char buf[1024];
            snprintf(buf, sizeof(buf),
                "    {\n"
                "      \"entityId\": %d,\n"
                "      \"name\": \"%s\",\n"
                "      \"parentEntityId\": 0,\n"
                "      \"transform\": { \"position\": [%.3f, %.3f, %.3f], \"orientation\": [0, 0, 0, 1], \"scale\": [1, 1, 1] },\n"
                "      \"components\": {\n"
                "        \"mesh\": {\n"
                "          \"primitive\": \"capsule\",\n"
                "          \"dimensions\": [0.4, 1.8, 0.4]\n"
                "        },\n"
                "        \"physicsBody\": {\n"
                "          \"motionType\": \"kinematic\",\n"
                "          \"shapeType\": \"capsule\",\n"
                "          \"mass\": 70.0,\n"
                "          \"friction\": 0.0,\n"
                "          \"restitution\": 0.0,\n"
                "          \"radius\": 0.4,\n"
                "          \"height\": 1.8,\n"
                "          \"fixedRotation\": true\n"
                "        },\n"
                "        \"actor\": {\n"
                "          \"actorType\": \"player\",\n"
                "          \"isPlayer\": true,\n"
                "          \"controllerType\": \"fps\",\n"
                "          \"eyeOffset\": [0, 0.8, 0]\n"
                "        }\n"
                "      }\n"
                "    }",
                entity_id, sp.label.c_str(),
                sx, sp.position.y, sz);
            entities.push_back(std::string(buf));
            entity_id++;
        }

        return entities;
    }

    graph_gen::NodeId find_entrance() const {
        for (const auto& [id, node] : graph_.nodes()) {
            if (node.kind == graph_gen::NodeKind::Entrance) return id;
        }
        // Fallback: first node
        return rooms_.begin()->first;
    }
};

} // namespace sector

#endif // KESTREL_SECTOR_GEN_HPP
