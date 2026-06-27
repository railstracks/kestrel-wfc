#ifndef KESTREL_GRAPH_GEN_HPP
#define KESTREL_GRAPH_GEN_HPP

//
// kestrel-graph-gen: Minimal graph rewrite generator for game level structure.
//
// Designed for Lodestone sector/level generation (Starfall Drift, platformer, etc.).
// Inspired by Unexplored's cyclic dungeon generation and the Dormans & Bakes
// mission+space generation approach.
//
// Architecture:
//   1. Generate a base graph (tree, loop, or custom)
//   2. Apply rewrite rules to transform the graph (add cycles, dead ends, locks/keys)
//   3. Resolve non-terminal symbols to concrete types
//   4. Export as JSON for consumption by game engines
//
// Usage:
//   graph_gen::Generator gen;
//   gen.make_cycle(5);  // 5-node loop
//   gen.apply_rule(Rule{...});
//   auto json = gen.to_json();
//

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <random>
#include <algorithm>
#include <sstream>
#include <functional>

namespace graph_gen {

using NodeId = uint32_t;
using EdgeId = uint32_t;

static constexpr NodeId INVALID_NODE = 0;
static constexpr EdgeId INVALID_EDGE = 0;

enum class NodeKind {
    // Structural
    Empty,         // Unassigned grid cell
    Entrance,      // Level entry point
    Goal,          // Level exit / objective
    Path,          // Generic traversable node

    // Non-terminals (resolved later)
    Obstacle,      // Something impedes progress
    Lock,          // Requires a key
    Key,           // Unlocks a lock
    Reward,        // Treasure / loot
    Guardian,      // Enemy guarding something

    // Concrete (for Starfall Drift)
    Station,       // Trade/refuel
    Derelict,      // Salvage opportunity
    Anomaly,       // Strange phenomenon
    Combat,        // Hostile encounter
    Trade,         // Merchant
    Empty_,        // Empty sector (renamed to avoid macro clash)

    // Special
    Door,          // Transition between areas
    Hub,           // Multi-connection central area
};

inline const char* node_kind_str(NodeKind k) {
    switch (k) {
        case NodeKind::Empty:    return "empty";
        case NodeKind::Entrance: return "entrance";
        case NodeKind::Goal:     return "goal";
        case NodeKind::Path:     return "path";
        case NodeKind::Obstacle: return "obstacle";
        case NodeKind::Lock:     return "lock";
        case NodeKind::Key:      return "key";
        case NodeKind::Reward:   return "reward";
        case NodeKind::Guardian: return "guardian";
        case NodeKind::Station:  return "station";
        case NodeKind::Derelict: return "derelict";
        case NodeKind::Anomaly:  return "anomaly";
        case NodeKind::Combat:   return "combat";
        case NodeKind::Trade:    return "trade";
        case NodeKind::Empty_:   return "empty_sector";
        case NodeKind::Door:     return "door";
        case NodeKind::Hub:      return "hub";
    }
    return "unknown";
}

struct Node {
    NodeId id = INVALID_NODE;
    NodeKind kind = NodeKind::Empty;
    std::string label;           // Human-readable name
    std::string biome;          // Theme/biome tag (e.g., "fire", "void", "commerce")
    int depth = 0;              // Distance from entrance (for difficulty pacing)
    int difficulty = 0;         // Encoded difficulty rating
    std::unordered_map<std::string, std::string> attrs;  // Arbitrary attributes

    // For lock/key pairing
    NodeId paired_node = INVALID_NODE;  // Lock→Key or Key→Lock

    // Grid position (for spatial layout)
    int grid_x = -1;
    int grid_y = -1;
};

struct Edge {
    EdgeId id = INVALID_EDGE;
    NodeId a = INVALID_NODE;
    NodeId b = INVALID_NODE;
    bool directed = false;      // One-way?
    bool locked = false;         // Requires key to traverse?
    NodeId lock_ref = INVALID_NODE;  // Which lock controls this edge
    std::string label;
};

// A rewrite rule: find a pattern, replace with new subgraph.
// Pattern matching is on node kinds + edge structure.
struct RewriteRule {
    std::string name;

    // Pattern: node kinds to match (in order)
    // Edge pattern: pairs of indices into pattern_nodes
    std::vector<NodeKind> pattern_nodes;
    std::vector<std::pair<int, int>> pattern_edges;  // (index_a, index_b)

    // Replacement: new node kinds
    // -1 = delete this node, 0+ = new node index
    // External connections from deleted/matched nodes route to new nodes
    struct ReplacementNode {
        NodeKind kind;
        std::string label;
        std::string biome;
    };
    std::vector<ReplacementNode> replacement_nodes;

    // How to connect replacement nodes to each other
    std::vector<std::pair<int, int>> replacement_edges;

    // How to reconnect to the rest of the graph:
    // pattern_index → replacement_index (external edges from pattern node N go to replacement node M)
    std::unordered_map<int, int> rewire_map;

    int weight = 1;  // Selection probability weight
    int max_applications = -1;  // -1 = unlimited
};

class Generator {
public:
    Generator(uint32_t seed = 0) : rng_(seed) {}

    // --- Graph construction ---

    NodeId add_node(NodeKind kind, const std::string& label = "", const std::string& biome = "") {
        Node n;
        n.id = next_node_id_++;
        n.kind = kind;
        n.label = label.empty() ? node_kind_str(kind) : label;
        n.biome = biome;
        nodes_[n.id] = n;
        return n.id;
    }

    EdgeId add_edge(NodeId a, NodeId b, bool directed = false, const std::string& label = "") {
        Edge e;
        e.id = next_edge_id_++;
        e.a = a;
        e.b = b;
        e.directed = directed;
        e.label = label;
        edges_[e.id] = e;
        adjacency_[a].push_back(e.id);
        if (!directed) adjacency_[b].push_back(e.id);
        return e.id;
    }

    // --- Graph generators ---

    // Create a circular loop of N nodes, returns entrance and goal node IDs
    std::pair<NodeId, NodeId> make_cycle(int n, const std::string& biome = "") {
        if (n < 3) n = 3;
        std::vector<NodeId> nodes;
        for (int i = 0; i < n; i++) {
            nodes.push_back(add_node(NodeKind::Path, "", biome));
        }
        // Connect into a loop
        for (int i = 0; i < n; i++) {
            add_edge(nodes[i], nodes[(i + 1) % n]);
        }
        // Add entrance and goal
        NodeId entrance = add_node(NodeKind::Entrance, "entrance", biome);
        NodeId goal = add_node(NodeKind::Goal, "goal", biome);
        add_edge(entrance, nodes[0]);
        add_edge(nodes[n / 2], goal);

        // Set depths from entrance
        compute_depths(entrance);

        return {entrance, goal};
    }

    // Create a tree via random walk
    NodeId make_tree(int depth, int max_branches = 3) {
        NodeId root = add_node(NodeKind::Entrance, "entrance");
        make_tree_recursive(root, depth, max_branches);
        compute_depths(root);
        return root;
    }

    // Create a linear chain
    std::pair<NodeId, NodeId> make_chain(int n, const std::string& biome = "") {
        if (n < 2) n = 2;
        NodeId entrance = add_node(NodeKind::Entrance, "entrance", biome);
        std::vector<NodeId> chain;
        chain.push_back(entrance);
        for (int i = 1; i < n - 1; i++) {
            chain.push_back(add_node(NodeKind::Path, "", biome));
        }
        NodeId goal = add_node(NodeKind::Goal, "goal", biome);
        chain.push_back(goal);
        for (size_t i = 0; i < chain.size() - 1; i++) {
            add_edge(chain[i], chain[i + 1]);
        }
        compute_depths(entrance);
        return {entrance, goal};
    }

    // --- Rewriting ---

    // Apply a single rule once. Returns true if matched and applied.
    bool apply_rule(const RewriteRule& rule) {
        // Find all matches
        std::vector<std::vector<NodeId>> matches;
        find_matches(rule, matches);

        if (matches.empty()) return false;

        // Pick a random match
        int idx = std::uniform_int_distribution<int>(0, matches.size() - 1)(rng_);
        auto& match = matches[idx];

        apply_replacement(rule, match);
        return true;
    }

    // Apply a rule repeatedly until no more matches or max reached
    int apply_rule_repeatedly(const RewriteRule& rule, int max_apps = 100) {
        int count = 0;
        int limit = rule.max_applications >= 0 ? rule.max_applications : max_apps;
        while (count < limit && apply_rule(rule)) {
            count++;
        }
        return count;
    }

    // Apply a set of rules in random order for N iterations
    int apply_rules_randomly(const std::vector<RewriteRule>& rules, int iterations = 50) {
        int total = 0;
        for (int i = 0; i < iterations; i++) {
            // Weighted random rule selection
            int total_weight = 0;
            for (const auto& r : rules) total_weight += r.weight;
            int pick = std::uniform_int_distribution<int>(0, total_weight - 1)(rng_);
            int acc = 0;
            const RewriteRule* chosen = nullptr;
            for (const auto& r : rules) {
                acc += r.weight;
                if (pick < acc) { chosen = &r; break; }
            }
            if (chosen && apply_rule(*chosen)) total++;
        }
        return total;
    }

    // --- Non-terminal resolution ---

    // Resolve all non-terminal nodes using a callback
    void resolve_non_terminals(std::function<NodeKind(NodeKind, Node&)> resolver) {
        for (auto& [id, node] : nodes_) {
            if (is_non_terminal(node.kind)) {
                node.kind = resolver(node.kind, node);
            }
        }
    }

    // Resolve based on biome + depth
    void resolve_with_biome() {
        for (auto& [id, node] : nodes_) {
            switch (node.kind) {
                case NodeKind::Obstacle:
                    if (node.biome == "combat") node.kind = NodeKind::Combat;
                    else if (node.biome == "void") node.kind = NodeKind::Anomaly;
                    else if (node.biome == "salvage") node.kind = NodeKind::Derelict;
                    else node.kind = NodeKind::Combat;
                    break;
                case NodeKind::Reward:
                    if (node.biome == "commerce") node.kind = NodeKind::Trade;
                    else node.kind = NodeKind::Station;
                    break;
                case NodeKind::Guardian:
                    node.kind = NodeKind::Combat;
                    node.difficulty = node.depth;  // Deeper = harder
                    break;
                default:
                    break;
            }
        }
    }

    // --- Lock/Key placement ---

    // Place a lock on a random edge and a key at a shallower node
    void place_lock_key(NodeId entrance, NodeId goal) {
        // Find edges that are on paths from entrance to goal
        std::vector<EdgeId> candidates;
        for (auto& [eid, edge] : edges_) {
            if (!edge.locked && edge.a != entrance && edge.b != goal) {
                candidates.push_back(eid);
            }
        }
        if (candidates.empty()) return;

        // Pick a random edge to lock
        int idx = std::uniform_int_distribution<int>(0, candidates.size() - 1)(rng_);
        EdgeId lock_edge = candidates[idx];
        auto& edge = edges_[lock_edge];

        // Place the lock at the deeper node
        NodeId lock_node = nodes_[edge.a].depth > nodes_[edge.b].depth ? edge.a : edge.b;
        NodeId key_node = add_node(NodeKind::Key, "key");

        // Key goes at a shallower point
        NodeId shallower = nodes_[edge.a].depth <= nodes_[edge.b].depth ? edge.a : edge.b;
        add_edge(shallower, key_node);

        // Link them
        nodes_[lock_node].kind = NodeKind::Lock;
        nodes_[lock_node].paired_node = key_node;
        nodes_[key_node].paired_node = lock_node;
        edge.locked = true;
        edge.lock_ref = lock_node;
    }

    // --- Query ---

    const std::unordered_map<NodeId, Node>& nodes() const { return nodes_; }
    const std::unordered_map<EdgeId, Edge>& edges() const { return edges_; }
    const std::vector<EdgeId>& adjacency(NodeId n) const {
        static const std::vector<EdgeId> empty;
        auto it = adjacency_.find(n);
        return it != adjacency_.end() ? it->second : empty;
    }

    // --- Serialization ---

    std::string to_json() const {
        std::ostringstream ss;
        ss << "{\n  \"schemaVersion\": 1,\n";
        ss << "  \"nodes\": [\n";
        bool first = true;
        for (const auto& [id, node] : nodes_) {
            if (!first) ss << ",\n";
            first = false;
            ss << "    {\"id\": " << id << ", "
               << "\"kind\": \"" << node_kind_str(node.kind) << "\", "
               << "\"label\": \"" << node.label << "\", "
               << "\"depth\": " << node.depth << ", "
               << "\"difficulty\": " << node.difficulty;
            if (!node.biome.empty()) ss << ", \"biome\": \"" << node.biome << "\"";
            if (node.paired_node != INVALID_NODE) ss << ", \"pairedWith\": " << node.paired_node;
            if (node.grid_x >= 0) ss << ", \"gridX\": " << node.grid_x << ", \"gridY\": " << node.grid_y;
            ss << "}";
        }
        ss << "\n  ],\n  \"edges\": [\n";
        first = true;
        for (const auto& [id, edge] : edges_) {
            if (!first) ss << ",\n";
            first = false;
            ss << "    {\"id\": " << id << ", "
               << "\"from\": " << edge.a << ", "
               << "\"to\": " << edge.b << ", "
               << "\"directed\": " << (edge.directed ? "true" : "false") << ", "
               << "\"locked\": " << (edge.locked ? "true" : "false");
            if (edge.lock_ref != INVALID_NODE) ss << ", \"lockRef\": " << edge.lock_ref;
            if (!edge.label.empty()) ss << ", \"label\": \"" << edge.label << "\"";
            ss << "}";
        }
        ss << "\n  ]\n}";
        return ss.str();
    }

    // --- ASCII visualization ---

    std::string to_ascii() const {
        std::ostringstream ss;
        ss << "=== Graph (nodes: " << nodes_.size() << ", edges: " << edges_.size() << ") ===\n\n";
        ss << "Nodes:\n";
        for (const auto& [id, node] : nodes_) {
            ss << "  [" << id << "] " << node_kind_str(node.kind);
            if (!node.label.empty()) ss << " \"" << node.label << "\"";
            ss << " (depth=" << node.depth << ")";
            if (!node.biome.empty()) ss << " biome=" << node.biome;
            if (node.paired_node != INVALID_NODE) ss << " paired=" << node.paired_node;
            ss << "\n";
        }
        ss << "\nEdges:\n";
        for (const auto& [id, edge] : edges_) {
            ss << "  [" << edge.a;
            if (edge.directed) ss << " -> ";
            else ss << " -- ";
            ss << edge.b << "]";
            if (edge.locked) ss << " LOCKED(ref=" << edge.lock_ref << ")";
            if (!edge.label.empty()) ss << " \"" << edge.label << "\"";
            ss << "\n";
        }
        return ss.str();
    }

private:
    std::unordered_map<NodeId, Node> nodes_;
    std::unordered_map<EdgeId, Edge> edges_;
    std::unordered_map<NodeId, std::vector<EdgeId>> adjacency_;
    NodeId next_node_id_ = 1;
    EdgeId next_edge_id_ = 1;
    std::mt19937 rng_;

    bool is_non_terminal(NodeKind k) const {
        return k == NodeKind::Obstacle ||
               k == NodeKind::Lock ||
               k == NodeKind::Key ||
               k == NodeKind::Reward ||
               k == NodeKind::Guardian;
    }

    void compute_depths(NodeId root) {
        // BFS from root
        std::vector<NodeId> queue = {root};
        std::unordered_set<NodeId> visited = {root};
        nodes_[root].depth = 0;
        while (!queue.empty()) {
            std::vector<NodeId> next;
            for (NodeId n : queue) {
                for (EdgeId eid : adjacency(n)) {
                    const Edge& e = edges_[eid];
                    NodeId other = (e.a == n) ? e.b : e.a;
                    if (visited.count(other)) continue;
                    visited.insert(other);
                    nodes_[other].depth = nodes_[n].depth + 1;
                    next.push_back(other);
                }
            }
            queue = std::move(next);
        }
    }

    void make_tree_recursive(NodeId parent, int depth, int max_branches) {
        if (depth <= 0) return;
        int branches = std::uniform_int_distribution<int>(1, max_branches)(rng_);
        for (int i = 0; i < branches; i++) {
            NodeKind kind = (depth == 1) ? NodeKind::Goal : NodeKind::Path;
            NodeId child = add_node(kind);
            add_edge(parent, child);
            make_tree_recursive(child, depth - 1, max_branches);
        }
    }

    // Find all matches of a rule's pattern in the graph
    void find_matches(const RewriteRule& rule, std::vector<std::vector<NodeId>>& matches) const {
        // Simple matching: find nodes matching pattern_nodes[0], then check neighbors
        // for pattern_nodes[1], etc. This is exponential in pattern size but patterns are small.
        if (rule.pattern_nodes.empty()) return;

        for (const auto& [id, node] : nodes_) {
            if (node.kind != rule.pattern_nodes[0]) continue;
            std::vector<NodeId> current = {id};
            std::unordered_set<NodeId> used = {id};
            extend_match(rule, 1, current, used, matches);
        }
    }

    void extend_match(const RewriteRule& rule, int idx,
                      std::vector<NodeId>& current,
                      std::unordered_set<NodeId>& used,
                      std::vector<std::vector<NodeId>>& matches) const {
        if (idx >= (int)rule.pattern_nodes.size()) {
            // Verify all pattern edges exist
            for (auto& [pa, pb] : rule.pattern_edges) {
                NodeId na = current[pa], nb = current[pb];
                bool found = false;
                for (EdgeId eid : adjacency(na)) {
                    const Edge& e = edges_.at(eid);
                    if ((e.a == na && e.b == nb) || (e.a == nb && e.b == na)) {
                        found = true;
                        break;
                    }
                }
                if (!found) return;
            }
            matches.push_back(current);
            return;
        }

        // Find neighbors of already-matched nodes that match pattern_nodes[idx]
        // and are connected via pattern_edges
        for (const auto& [id, node] : nodes_) {
            if (used.count(id)) continue;
            if (node.kind != rule.pattern_nodes[idx]) continue;

            // Check if this node is connected to any already-matched node
            // as required by pattern_edges
            bool connected_ok = true;
            for (auto& [pa, pb] : rule.pattern_edges) {
                int check_idx = -1;
                int other_idx = -1;
                if (pa == idx && pb < idx) { check_idx = idx; other_idx = pb; }
                else if (pb == idx && pa < idx) { check_idx = idx; other_idx = pa; }
                if (check_idx == idx) {
                    NodeId other = current[other_idx];
                    bool found = false;
                    for (EdgeId eid : adjacency(other)) {
                        const Edge& e = edges_.at(eid);
                        if ((e.a == other && e.b == id) || (e.a == id && e.b == other)) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) { connected_ok = false; break; }
                }
            }

            if (connected_ok) {
                current.push_back(id);
                used.insert(id);
                extend_match(rule, idx + 1, current, used, matches);
                used.erase(id);
                current.pop_back();
            }
        }
    }

    void apply_replacement(const RewriteRule& rule, std::vector<NodeId>& match) {
        // Create new nodes
        std::vector<NodeId> new_nodes;
        for (auto& rn : rule.replacement_nodes) {
            NodeId nid = add_node(rn.kind, rn.label, rn.biome);
            new_nodes.push_back(nid);
        }

        // Connect new nodes to each other
        for (auto& [ra, rb] : rule.replacement_edges) {
            if (ra < (int)new_nodes.size() && rb < (int)new_nodes.size()) {
                add_edge(new_nodes[ra], new_nodes[rb]);
            }
        }

        // Rewire external connections
        // For each matched node, find its external edges (edges to nodes outside the match)
        // and redirect them to the corresponding replacement node
        std::unordered_set<NodeId> match_set(match.begin(), match.end());

        for (size_t i = 0; i < match.size(); i++) {
            auto it = rule.rewire_map.find((int)i);
            if (it == rule.rewire_map.end()) continue;
            int repl_idx = it->second;
            if (repl_idx < 0 || repl_idx >= (int)new_nodes.size()) continue;

            NodeId old_node = match[i];
            NodeId new_node = new_nodes[repl_idx];

            // Find all edges from old_node to nodes outside match
            std::vector<EdgeId> to_rewire;
            for (EdgeId eid : adjacency_[old_node]) {
                const Edge& e = edges_[eid];
                NodeId other = (e.a == old_node) ? e.b : e.a;
                if (!match_set.count(other)) {
                    to_rewire.push_back(eid);
                }
            }

            // Redirect these edges to new_node
            for (EdgeId eid : to_rewire) {
                Edge& e = edges_[eid];
                if (e.a == old_node) e.a = new_node;
                else e.b = new_node;
                // Update adjacency
                adjacency_[new_node].push_back(eid);
            }
        }

        // Delete matched nodes and their internal edges
        for (NodeId nid : match) {
            // Remove all edges involving this node
            std::vector<EdgeId> to_remove;
            for (EdgeId eid : adjacency_[nid]) {
                const Edge& e = edges_[eid];
                NodeId other = (e.a == nid) ? e.b : e.a;
                if (match_set.count(other)) {
                    to_remove.push_back(eid);
                }
            }
            for (EdgeId eid : to_remove) {
                edges_.erase(eid);
            }
            adjacency_.erase(nid);
            nodes_.erase(nid);
        }

        // Recompute depths if we have an entrance
        for (auto& [id, node] : nodes_) {
            if (node.kind == NodeKind::Entrance) {
                compute_depths(id);
                break;
            }
        }
    }
};

} // namespace graph_gen

#endif // KESTREL_GRAPH_GEN_HPP