// test_graph_gen.cpp — Starfall Drift sector map generator test
// g++ -std=c++17 -O2 -o test_graph_gen test_graph_gen.cpp && ./test_graph_gen
//
// Generates a cyclic sector map, applies rewrite rules for Starfall Drift,
// resolves non-terminals, places lock/key, and prints the result.

#include "graph_gen.hpp"
#include <cstdio>
#include <vector>

using namespace graph_gen;

int main() {
    // === Step 1: Generate base cycle (5 nodes + entrance + goal) ===
    Generator gen(42);
    auto [entrance, goal] = gen.make_cycle(5, "void");

    printf("=== After base cycle ===\n");
    printf("%s\n\n", gen.to_ascii().c_str());

    // === Step 2: Define rewrite rules ===

    // Rule 1: Path → Obstacle + Path (insert an obstacle on a path)
    RewriteRule r_insert_obstacle;
    r_insert_obstacle.name = "insert_obstacle";
    r_insert_obstacle.pattern_nodes = {NodeKind::Path};
    r_insert_obstacle.replacement_nodes = {
        {NodeKind::Obstacle, "obstacle", "void"},
        {NodeKind::Path, "path", "void"},
    };
    r_insert_obstacle.replacement_edges = {{0, 1}};
    r_insert_obstacle.rewire_map = {{0, 0}};  // External edges from Path → obstacle
    r_insert_obstacle.weight = 3;
    r_insert_obstacle.max_applications = 4;

    // Rule 2: Path → Reward (dead-end treasure)
    RewriteRule r_add_reward;
    r_add_reward.name = "add_reward";
    r_add_reward.pattern_nodes = {NodeKind::Path};
    r_add_reward.replacement_nodes = {
        {NodeKind::Path, "path", "void"},
        {NodeKind::Reward, "reward", "void"},
    };
    r_add_reward.replacement_edges = {{0, 1}};
    r_add_reward.rewire_map = {{0, 0}};
    r_add_reward.weight = 2;
    r_add_reward.max_applications = 3;

    // Rule 3: Obstacle → Guardian + Obstacle (escalate: guardian before obstacle)
    RewriteRule r_guardian;
    r_guardian.name = "add_guardian";
    r_guardian.pattern_nodes = {NodeKind::Obstacle};
    r_guardian.replacement_nodes = {
        {NodeKind::Guardian, "guardian", "combat"},
        {NodeKind::Obstacle, "obstacle", "void"},
    };
    r_guardian.replacement_edges = {{0, 1}};
    r_guardian.rewire_map = {{0, 0}};
    r_guardian.weight = 2;
    r_guardian.max_applications = 2;

    // Rule 4: Path → Door (one-way shortcut)
    RewriteRule r_door;
    r_door.name = "add_door";
    r_door.pattern_nodes = {NodeKind::Path};
    r_door.replacement_nodes = {
        {NodeKind::Door, "door", ""},
        {NodeKind::Path, "path", "void"},
    };
    r_door.replacement_edges = {{0, 1}};
    r_door.rewire_map = {{0, 0}};
    r_door.weight = 1;
    r_door.max_applications = 2;

    std::vector<RewriteRule> rules = {r_insert_obstacle, r_add_reward, r_guardian, r_door};

    // === Step 3: Apply rules randomly ===
    int applied = gen.apply_rules_randomly(rules, 30);
    printf("=== After %d rule applications ===\n", applied);
    printf("%s\n\n", gen.to_ascii().c_str());

    // === Step 4: Place a lock/key ===
    gen.place_lock_key(entrance, goal);
    printf("=== After lock/key placement ===\n");
    printf("%s\n\n", gen.to_ascii().c_str());

    // === Step 5: Resolve non-terminals ===
    gen.resolve_with_biome();
    printf("=== After resolution ===\n");
    printf("%s\n\n", gen.to_ascii().c_str());

    // === Step 6: Output JSON ===
    printf("=== JSON Output ===\n");
    printf("%s\n", gen.to_json().c_str());

    // === Validation ===
    printf("\n=== Validation ===\n");

    // Check: entrance exists
    bool has_entrance = false, has_goal = false;
    int obstacle_count = 0, reward_count = 0, lock_count = 0, key_count = 0;
    for (const auto& [id, node] : gen.nodes()) {
        if (node.kind == NodeKind::Entrance) has_entrance = true;
        if (node.kind == NodeKind::Goal) has_goal = true;
        if (node.kind == NodeKind::Combat || node.kind == NodeKind::Anomaly ||
            node.kind == NodeKind::Derelict) obstacle_count++;
        if (node.kind == NodeKind::Station || node.kind == NodeKind::Trade) reward_count++;
        if (node.kind == NodeKind::Lock) lock_count++;
        if (node.kind == NodeKind::Key) key_count++;
    }

    printf("  Entrance: %s\n", has_entrance ? "YES" : "NO");
    printf("  Goal: %s\n", has_goal ? "YES" : "NO");
    printf("  Obstacles: %d\n", obstacle_count);
    printf("  Rewards: %d\n", reward_count);
    printf("  Locks: %d, Keys: %d %s\n", lock_count, key_count,
           (lock_count == key_count) ? "(MATCHED)" : "(MISMATCH!)");
    printf("  Total nodes: %zu\n", gen.nodes().size());
    printf("  Total edges: %zu\n", gen.edges().size());

    // Multiple seeds
    printf("\n=== 5-seed sweep ===\n");
    for (uint32_t seed = 1; seed <= 5; seed++) {
        Generator g(seed);
        auto [ent, goal] = g.make_cycle(5 + seed % 3, "void");
        g.apply_rules_randomly(rules, 30);
        g.place_lock_key(ent, goal);
        g.resolve_with_biome();

        int n = g.nodes().size();
        int e = g.edges().size();
        int obs = 0, rew = 0;
        for (const auto& [id, node] : g.nodes()) {
            if (node.kind == NodeKind::Combat || node.kind == NodeKind::Anomaly ||
                node.kind == NodeKind::Derelict) obs++;
            if (node.kind == NodeKind::Station || node.kind == NodeKind::Trade) rew++;
        }
        printf("  Seed %u: %d nodes, %d edges, %d obstacles, %d rewards\n", seed, n, e, obs, rew);
    }

    return 0;
}