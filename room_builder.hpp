#ifndef KESTREL_ROOM_BUILDER_HPP
#define KESTREL_ROOM_BUILDER_HPP

//
// room_builder.hpp — WFC tile grid → 3D room definition for Lodestone.
//
// Bridge layer: takes a collapsed WFC grid and produces structured 3D geometry
// descriptions (floor quads, wall boxes, door zones, spawn points) that can be
// fed into Lodestone's renderer and physics system.
//
// Output formats:
//   - In-memory RoomDefinition struct for direct engine use
//   - JSON export matching Lodestone's room format (TD-004)
//   - ASCII preview for debugging
//
// Tile convention (matches test_wfc2.cpp):
//   0: Floor (.)  → walkable surface
//   1: Wall  (#)  → full-height collision box, blocks movement/visibility
//   2: Door  (+)  → floor + transition zone (connects to adjacent room)
//   3: Pillar (O) → floor + decorative collision (smaller footprint)
//   4: Pit   (~)  → lowered floor or void, non-walkable
//
// Usage:
//   wfc::Generator gen(w, h, tiles);
//   // ... set adjacency, run WFC ...
//   auto grid = gen.run(seed);
//   room::RoomBuilder builder(gen, grid);
//   auto room = builder.build();
//   builder.write_json(room, stdout);
//

#include "wfc.hpp"
#include <vector>
#include <string>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace room {

struct Vec3 {
    float x, y, z;
};

struct Quad {  // floor or ceiling surface
    Vec3 corner[4];  // bl, br, tr, tl (viewed from above)
};

struct Box {  // collision body + visual mesh
    Vec3 center;
    Vec3 half_extents;  // x=width/2, y=height/2, z=depth/2
};

struct DoorZone {
    Vec3 center;
    float width;   // along the wall axis
    float height;  // vertical
    int direction; // 0=right, 1=left, 2=up, 3=down (which neighbor it connects)
    int grid_x, grid_y;  // grid position
};

struct SpawnPoint {
    Vec3 position;
    std::string label;
};

struct RoomDefinition {
    int grid_w, grid_h;
    float tile_size;      // meters per tile
    float wall_height;    // wall height in meters
    float floor_y;        // y coordinate of floor surface
    float pit_depth;      // how deep pits are below floor

    std::vector<Quad> floor_quads;    // walkable surfaces
    std::vector<Quad> pit_quads;      // lowered/void surfaces (visual)
    std::vector<Box> wall_boxes;      // full collision walls
    std::vector<Box> pillar_boxes;    // decorative collision
    std::vector<DoorZone> doors;      // transition zones
    std::vector<SpawnPoint> spawns;   // player/object spawn points
};

// Tile type constants (matching test_wfc2.cpp convention)
enum TileType {
    FLOOR   = 0,
    WALL    = 1,
    DOOR    = 2,
    PILLAR  = 3,
    PIT     = 4,
};

class RoomBuilder {
public:
    RoomBuilder(const wfc::Generator& gen, const std::vector<int>& grid,
                float tile_size = 1.0f, float wall_height = 3.0f,
                float floor_y = 0.0f, float pit_depth = 2.0f)
        : gen_(gen), grid_(grid),
          tile_size_(tile_size), wall_height_(wall_height),
          floor_y_(floor_y), pit_depth_(pit_depth)
    {
        w_ = gen.width();
        h_ = gen.height();
    }

    RoomDefinition build() const {
        RoomDefinition room;
        room.grid_w = w_;
        room.grid_h = h_;
        room.tile_size = tile_size_;
        room.wall_height = wall_height_;
        room.floor_y = floor_y_;
        room.pit_depth = pit_depth_;

        for (int y = 0; y < h_; y++) {
            for (int x = 0; x < w_; x++) {
                int tile = grid_[y * w_ + x];
                float wx = x * tile_size_;       // world x (center of tile)
                float wz = y * tile_size_;       // world z
                float ts = tile_size_;

                switch (tile) {
                    case FLOOR:
                        add_floor_quad(room, wx, wz, floor_y_);
                        break;
                    case WALL:
                        add_wall_box(room, wx, wz);
                        break;
                    case DOOR:
                        add_floor_quad(room, wx, wz, floor_y_);
                        add_door_zone(room, x, y, wx, wz);
                        break;
                    case PILLAR:
                        add_floor_quad(room, wx, wz, floor_y_);
                        add_pillar_box(room, wx, wz);
                        break;
                    case PIT:
                        add_pit_quad(room, wx, wz, floor_y_ - pit_depth_);
                        break;
                }
            }
        }

        // Auto-generate spawn point: centermost floor tile
        int best_dist = -1;
        for (int y = 0; y < h_; y++) {
            for (int x = 0; x < w_; x++) {
                if (grid_[y * w_ + x] != FLOOR) continue;
                int dx = x - w_ / 2;
                int dy = y - h_ / 2;
                int dist = dx * dx + dy * dy;
                if (best_dist < 0 || dist < best_dist) {
                    best_dist = dist;
                    SpawnPoint sp;
                    sp.position = {x * tile_size_, floor_y_ + 0.5f, y * tile_size_};
                    sp.label = "player_spawn";
                    room.spawns = {sp};  // keep only the best one
                }
            }
        }

        return room;
    }

    void write_json(const RoomDefinition& room, FILE* out) const {
        fprintf(out, "{\n");
        fprintf(out, "  \"grid\": { \"w\": %d, \"h\": %d },\n", room.grid_w, room.grid_h);
        fprintf(out, "  \"dimensions\": { \"tile_size\": %.3f, \"wall_height\": %.3f, \"floor_y\": %.3f, \"pit_depth\": %.3f },\n",
                room.tile_size, room.wall_height, room.floor_y, room.pit_depth);

        // Floor quads
        fprintf(out, "  \"floor_quads\": [\n");
        for (size_t i = 0; i < room.floor_quads.size(); i++) {
            const Quad& q = room.floor_quads[i];
            fprintf(out, "    { \"corners\": [");
            for (int c = 0; c < 4; c++) {
                fprintf(out, "[%.3f, %.3f, %.3f]%s", q.corner[c].x, q.corner[c].y, q.corner[c].z,
                        c < 3 ? ", " : "");
            }
            fprintf(out, "] }%s\n", i < room.floor_quads.size() - 1 ? "," : "");
        }
        fprintf(out, "  ],\n");

        // Pit quads
        fprintf(out, "  \"pit_quads\": [\n");
        for (size_t i = 0; i < room.pit_quads.size(); i++) {
            const Quad& q = room.pit_quads[i];
            fprintf(out, "    { \"corners\": [");
            for (int c = 0; c < 4; c++) {
                fprintf(out, "[%.3f, %.3f, %.3f]%s", q.corner[c].x, q.corner[c].y, q.corner[c].z,
                        c < 3 ? ", " : "");
            }
            fprintf(out, "] }%s\n", i < room.pit_quads.size() - 1 ? "," : "");
        }
        fprintf(out, "  ],\n");

        // Wall boxes
        fprintf(out, "  \"wall_boxes\": [\n");
        for (size_t i = 0; i < room.wall_boxes.size(); i++) {
            const Box& b = room.wall_boxes[i];
            fprintf(out, "    { \"center\": [%.3f, %.3f, %.3f], \"half_extents\": [%.3f, %.3f, %.3f] }%s\n",
                    b.center.x, b.center.y, b.center.z,
                    b.half_extents.x, b.half_extents.y, b.half_extents.z,
                    i < room.wall_boxes.size() - 1 ? "," : "");
        }
        fprintf(out, "  ],\n");

        // Pillar boxes
        fprintf(out, "  \"pillar_boxes\": [\n");
        for (size_t i = 0; i < room.pillar_boxes.size(); i++) {
            const Box& b = room.pillar_boxes[i];
            fprintf(out, "    { \"center\": [%.3f, %.3f, %.3f], \"half_extents\": [%.3f, %.3f, %.3f] }%s\n",
                    b.center.x, b.center.y, b.center.z,
                    b.half_extents.x, b.half_extents.y, b.half_extents.z,
                    i < room.pillar_boxes.size() - 1 ? "," : "");
        }
        fprintf(out, "  ],\n");

        // Doors
        fprintf(out, "  \"doors\": [\n");
        for (size_t i = 0; i < room.doors.size(); i++) {
            const DoorZone& d = room.doors[i];
            fprintf(out, "    { \"center\": [%.3f, %.3f, %.3f], \"width\": %.3f, \"height\": %.3f, \"direction\": %d, \"grid\": [%d, %d] }%s\n",
                    d.center.x, d.center.y, d.center.z, d.width, d.height, d.direction,
                    d.grid_x, d.grid_y,
                    i < room.doors.size() - 1 ? "," : "");
        }
        fprintf(out, "  ],\n");

        // Spawns
        fprintf(out, "  \"spawns\": [\n");
        for (size_t i = 0; i < room.spawns.size(); i++) {
            const SpawnPoint& s = room.spawns[i];
            fprintf(out, "    { \"position\": [%.3f, %.3f, %.3f], \"label\": \"%s\" }%s\n",
                    s.position.x, s.position.y, s.position.z, s.label.c_str(),
                    i < room.spawns.size() - 1 ? "," : "");
        }
        fprintf(out, "  ]\n");

        fprintf(out, "}\n");
    }

    // Write Lodestone scene JSON (schemaVersion 2) — directly loadable by the engine.
    // Floor tiles → plane meshes, walls → box meshes + static physics bodies,
    // pillars → smaller box meshes + physics, pits → lowered planes,
    // doors → floor mesh + interactable (type=door), spawn → actor entity.
    void write_lodestone_json(const RoomDefinition& room, FILE* out, uint64_t seed) const {
        fprintf(out, "{\n");
        fprintf(out, "  \"schemaVersion\": 2,\n");
        fprintf(out, "  \"id\": \"wfc.room.%llu\",\n", (unsigned long long)seed);
        fprintf(out, "  \"name\": \"WFC Generated Room (seed %llu)\",\n", (unsigned long long)seed);
        fprintf(out, "  \"dimensionality\": \"3D\",\n");
        fprintf(out, "  \"settings\": {\n");
        fprintf(out, "    \"viewportCameraId\": 0,\n");
        fprintf(out, "    \"primaryCameraEntityId\": 0,\n");
        fprintf(out, "    \"ambientColor\": [0.15, 0.15, 0.2],\n");
        fprintf(out, "    \"gravity\": [0.0, -9.8, 0.0]\n");
        fprintf(out, "  },\n");

        // Collect all entities
        std::vector<std::string> entity_jsons;
        int entity_id = 1;

        // Floor tiles (merge into one mesh per row-strip for efficiency)
        // For now: one entity per floor tile — engine can optimize later
        for (size_t i = 0; i < room.floor_quads.size(); i++) {
            const Quad& q = room.floor_quads[i];
            float cx = (q.corner[0].x + q.corner[2].x) * 0.5f;
            float cz = (q.corner[0].z + q.corner[2].z) * 0.5f;
            char buf[512];
            snprintf(buf, sizeof(buf),
                "    {\n"
                "      \"entityId\": %d,\n"
                "      \"name\": \"floor_%d\",\n"
                "      \"parentEntityId\": 0,\n"
                "      \"transform\": { \"position\": [%.3f, %.3f, %.3f], \"orientation\": [0, 0, 0, 1], \"scale\": [1, 1, 1] },\n"
                "      \"components\": {\n"
                "        \"mesh\": {\n"
                "          \"primitive\": \"plane\",\n"
                "          \"dimensions\": [%.3f, 1.0, %.3f],\n"
                "          \"material\": {\n"
                "            \"albedo\": [0.5, 0.45, 0.4],\n"
                "            \"opacity\": 1.0\n"
                "          }\n"
                "        }\n"
                "      }\n"
                "    }",
                entity_id, (int)i,
                cx, room.floor_y, cz,
                tile_size_, tile_size_);
            entity_jsons.push_back(std::string(buf));
            entity_id++;
        }

        // Wall boxes (mesh + static physics body)
        for (size_t i = 0; i < room.wall_boxes.size(); i++) {
            const Box& b = room.wall_boxes[i];
            char buf[700];
            snprintf(buf, sizeof(buf),
                "    {\n"
                "      \"entityId\": %d,\n"
                "      \"name\": \"wall_%d\",\n"
                "      \"parentEntityId\": 0,\n"
                "      \"transform\": { \"position\": [%.3f, %.3f, %.3f], \"orientation\": [0, 0, 0, 1], \"scale\": [1, 1, 1] },\n"
                "      \"components\": {\n"
                "        \"mesh\": {\n"
                "          \"primitive\": \"box\",\n"
                "          \"dimensions\": [%.3f, %.3f, %.3f],\n"
                "          \"material\": {\n"
                "            \"albedo\": [0.3, 0.28, 0.25],\n"
                "            \"opacity\": 1.0\n"
                "          }\n"
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
                entity_id, (int)i,
                b.center.x, b.center.y, b.center.z,
                b.half_extents.x * 2, b.half_extents.y * 2, b.half_extents.z * 2,
                b.half_extents.x, b.half_extents.y, b.half_extents.z);
            entity_jsons.push_back(std::string(buf));
            entity_id++;
        }

        // Pillar boxes (mesh + static physics, narrower)
        for (size_t i = 0; i < room.pillar_boxes.size(); i++) {
            const Box& b = room.pillar_boxes[i];
            char buf[700];
            snprintf(buf, sizeof(buf),
                "    {\n"
                "      \"entityId\": %d,\n"
                "      \"name\": \"pillar_%d\",\n"
                "      \"parentEntityId\": 0,\n"
                "      \"transform\": { \"position\": [%.3f, %.3f, %.3f], \"orientation\": [0, 0, 0, 1], \"scale\": [1, 1, 1] },\n"
                "      \"components\": {\n"
                "        \"mesh\": {\n"
                "          \"primitive\": \"box\",\n"
                "          \"dimensions\": [%.3f, %.3f, %.3f],\n"
                "          \"material\": {\n"
                "            \"albedo\": [0.4, 0.38, 0.35],\n"
                "            \"opacity\": 1.0\n"
                "          }\n"
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
                entity_id, (int)i,
                b.center.x, b.center.y, b.center.z,
                b.half_extents.x * 2, b.half_extents.y * 2, b.half_extents.z * 2,
                b.half_extents.x, b.half_extents.y, b.half_extents.z);
            entity_jsons.push_back(std::string(buf));
            entity_id++;
        }

        // Pit quads (lowered floor, dark material)
        for (size_t i = 0; i < room.pit_quads.size(); i++) {
            const Quad& q = room.pit_quads[i];
            float cx = (q.corner[0].x + q.corner[2].x) * 0.5f;
            float cz = (q.corner[0].z + q.corner[2].z) * 0.5f;
            char buf[512];
            snprintf(buf, sizeof(buf),
                "    {\n"
                "      \"entityId\": %d,\n"
                "      \"name\": \"pit_%d\",\n"
                "      \"parentEntityId\": 0,\n"
                "      \"transform\": { \"position\": [%.3f, %.3f, %.3f], \"orientation\": [0, 0, 0, 1], \"scale\": [1, 1, 1] },\n"
                "      \"components\": {\n"
                "        \"mesh\": {\n"
                "          \"primitive\": \"plane\",\n"
                "          \"dimensions\": [%.3f, 1.0, %.3f],\n"
                "          \"material\": {\n"
                "            \"albedo\": [0.1, 0.08, 0.06],\n"
                "            \"opacity\": 1.0\n"
                "          }\n"
                "        }\n"
                "      }\n"
                "    }",
                entity_id, (int)i,
                cx, q.corner[0].y, cz,
                tile_size_, tile_size_);
            entity_jsons.push_back(std::string(buf));
            entity_id++;
        }

        // Door zones (interactable doors)
        for (size_t i = 0; i < room.doors.size(); i++) {
            const DoorZone& d = room.doors[i];
            char buf[600];
            snprintf(buf, sizeof(buf),
                "    {\n"
                "      \"entityId\": %d,\n"
                "      \"name\": \"door_%d\",\n"
                "      \"parentEntityId\": 0,\n"
                "      \"transform\": { \"position\": [%.3f, %.3f, %.3f], \"orientation\": [0, 0, 0, 1], \"scale\": [1, 1, 1] },\n"
                "      \"components\": {\n"
                "        \"mesh\": {\n"
                "          \"primitive\": \"box\",\n"
                "          \"dimensions\": [%.3f, %.3f, 0.1],\n"
                "          \"material\": {\n"
                "            \"albedo\": [0.35, 0.2, 0.1],\n"
                "            \"opacity\": 1.0\n"
                "          }\n"
                "        },\n"
                "        \"interactable\": {\n"
                "          \"type\": \"door\",\n"
                "          \"prompt\": \"Enter next room\",\n"
                "          \"range\": 2.5\n"
                "        }\n"
                "      }\n"
                "    }",
                entity_id, (int)i,
                d.center.x, d.center.y, d.center.z,
                d.width, d.height);
            entity_jsons.push_back(std::string(buf));
            entity_id++;
        }

        // Directional light
        {
            char buf[512];
            snprintf(buf, sizeof(buf),
                "    {\n"
                "      \"entityId\": %d,\n"
                "      \"name\": \"sun_light\",\n"
                "      \"parentEntityId\": 0,\n"
                "      \"transform\": { \"position\": [0, 10, 0], \"orientation\": [0, 0, 0, 1], \"scale\": [1, 1, 1] },\n"
                "      \"components\": {\n"
                "        \"light\": {\n"
                "          \"type\": \"directional\",\n"
                "          \"color\": [1.0, 0.95, 0.85],\n"
                "          \"intensity\": 0.8,\n"
                "          \"direction\": [-0.3, -1.0, -0.2]\n"
                "        }\n"
                "      }\n"
                "    }",
                entity_id);
            entity_jsons.push_back(std::string(buf));
            entity_id++;
        }

        // Player spawn (if exists)
        if (!room.spawns.empty()) {
            const SpawnPoint& sp = room.spawns[0];
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
                sp.position.x, sp.position.y, sp.position.z);
            entity_jsons.push_back(std::string(buf));
            entity_id++;
        }

        // Emit entities array
        fprintf(out, "  \"entities\": [\n");
        for (size_t i = 0; i < entity_jsons.size(); i++) {
            fprintf(out, "%s%s\n", entity_jsons[i].c_str(), i < entity_jsons.size() - 1 ? "," : "");
        }
        fprintf(out, "  ]\n");
        fprintf(out, "}\n");
    }

    // ASCII preview showing 3D side view (elevation map)
    std::string to_elevation_ascii(const RoomDefinition& room) const {
        std::string out;
        out += "=== Elevation Map (top-down with height markers) ===\n\n";
        for (int y = 0; y < h_; y++) {
            for (int x = 0; x < w_; x++) {
                int tile = grid_[y * w_ + x];
                switch (tile) {
                    case FLOOR:  out += '.'; break;
                    case WALL:   out += '#'; break;
                    case DOOR:   out += '+'; break;
                    case PILLAR: out += 'O'; break;
                    case PIT:    out += '~'; break;
                    default:     out += '?'; break;
                }
            }
            out += '\n';
        }
        out += "\nLegend: . = floor  # = wall  + = door  O = pillar  ~ = pit\n";
        out += "Tile size: " + std::to_string(tile_size_) + "m  Wall height: " + std::to_string(wall_height_) + "m\n";
        out += "Floor quads: " + std::to_string(room.floor_quads.size()) + "\n";
        out += "Wall boxes: " + std::to_string(room.wall_boxes.size()) + "\n";
        out += "Pillar boxes: " + std::to_string(room.pillar_boxes.size()) + "\n";
        out += "Door zones: " + std::to_string(room.doors.size()) + "\n";
        out += "Spawn points: " + std::to_string(room.spawns.size()) + "\n";
        return out;
    }

private:
    const wfc::Generator& gen_;
    const std::vector<int>& grid_;
    int w_, h_;
    float tile_size_, wall_height_, floor_y_, pit_depth_;

    void add_floor_quad(RoomDefinition& room, float wx, float wz, float y) const {
        float h = tile_size_ * 0.5f;
        Quad q;
        q.corner[0] = {wx - h, y, wz - h};  // bl
        q.corner[1] = {wx + h, y, wz - h};  // br
        q.corner[2] = {wx + h, y, wz + h};  // tr
        q.corner[3] = {wx - h, y, wz + h};  // tl
        room.floor_quads.push_back(q);
    }

    void add_pit_quad(RoomDefinition& room, float wx, float wz, float y) const {
        float h = tile_size_ * 0.5f;
        Quad q;
        q.corner[0] = {wx - h, y, wz - h};
        q.corner[1] = {wx + h, y, wz - h};
        q.corner[2] = {wx + h, y, wz + h};
        q.corner[3] = {wx - h, y, wz + h};
        room.pit_quads.push_back(q);
    }

    void add_wall_box(RoomDefinition& room, float wx, float wz) const {
        float h = tile_size_ * 0.5f;
        Box b;
        b.center = {wx, floor_y_ + wall_height_ * 0.5f, wz};
        b.half_extents = {h, wall_height_ * 0.5f, h};
        room.wall_boxes.push_back(b);
    }

    void add_pillar_box(RoomDefinition& room, float wx, float wz) const {
        float h = tile_size_ * 0.3f;  // pillars are narrower than walls
        Box b;
        b.center = {wx, floor_y_ + wall_height_ * 0.4f, wz};
        b.half_extents = {h, wall_height_ * 0.4f, h};
        room.pillar_boxes.push_back(b);
    }

    void add_door_zone(RoomDefinition& room, int gx, int gy, float wx, float wz) const {
        // Determine door direction by checking neighbors
        // Priority: find a wall neighbor (door connects through walls)
        int dir = -1;
        // Check: is there a wall to the right?
        if (gx + 1 < w_ && grid_[gy * w_ + gx + 1] == WALL) dir = 0;
        else if (gx > 0 && grid_[gy * w_ + gx - 1] == WALL) dir = 1;
        else if (gy > 0 && grid_[(gy - 1) * w_ + gx] == WALL) dir = 2;
        else if (gy + 1 < h_ && grid_[(gy + 1) * w_ + gx] == WALL) dir = 3;
        else dir = 0;  // default

        DoorZone d;
        d.center = {wx, floor_y_ + wall_height_ * 0.5f, wz};
        d.width = tile_size_;
        d.height = wall_height_;
        d.direction = dir;
        d.grid_x = gx;
        d.grid_y = gy;
        room.doors.push_back(d);
    }
};

} // namespace room

#endif // KESTREL_ROOM_BUILDER_HPP