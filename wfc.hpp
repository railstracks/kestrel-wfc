#ifndef KESTREL_WFC_HPP
#define KESTREL_WFC_HPP

//
// kestrel-wfc: Minimal Wave Function Collapse for tile-based generation.
//
// Single header, no dependencies. Designed for Lodestone room generation.
// Usage:
//   wfc::Generator gen(16, 16, tiles);
//   gen.set_adjacency(tile_a, tile_b, wfc::Dir::Right);
//   gen.set_frequency(tile_a, 10);
//   auto grid = gen.run(seed);
//   if (grid.empty()) { /* contradiction — retry with new seed */ }
//

#include <vector>
#include <cstdint>
#include <string>
#include <algorithm>
#include <random>
#include <cmath>

namespace wfc {

enum class Dir : int {
    Right = 0,
    Left = 1,
    Up = 2,
    Down = 3,
};

constexpr Dir opposite(Dir d) {
    switch (d) {
        case Dir::Right: return Dir::Left;
        case Dir::Left:  return Dir::Right;
        case Dir::Up:    return Dir::Down;
        case Dir::Down:  return Dir::Up;
    }
    return Dir::Right;
}

constexpr int dir_dx(Dir d) {
    return d == Dir::Right ? 1 : d == Dir::Left ? -1 : 0;
}

constexpr int dir_dy(Dir d) {
    return d == Dir::Down ? 1 : d == Dir::Up ? -1 : 0;
}

struct Tile {
    int id;
    std::string name;
    char glyph;  // for ASCII output
};

class Generator {
public:
    Generator(int width, int height, const std::vector<Tile>& tiles)
        : w_(width), h_(height), tiles_(tiles), rng_(0)
    {
        int n = static_cast<int>(tiles.size());
        // Domain: for each cell, a bool array of size n (true = possible)
        domain_.assign(w_ * h_, std::vector<bool>(n, true));
        // Adjacency: adj_[tile_a][dir] = set of tiles that can be in that direction
        adj_.assign(n, std::vector<std::vector<int>>(4));
        // Frequency: default 1 for each tile
        freq_.assign(n, 1.0);
    }

    void set_adjacency(int from_tile, int to_tile, Dir dir) {
        adj_[from_tile][static_cast<int>(dir)].push_back(to_tile);
    }

    void set_frequency(int tile, double weight) {
        freq_[tile] = weight;
    }

    void set_seed(uint64_t seed) { rng_.seed(seed); }

    // Run WFC. Returns flattened grid of tile indices, or empty on contradiction.
    std::vector<int> run(uint64_t seed) {
        set_seed(seed);
        int n = static_cast<int>(tiles_.size());
        domain_.assign(w_ * h_, std::vector<bool>(n, true));

        while (true) {
            // Find cell with minimum entropy (>1 possibility)
            int best_cell = -1;
            double best_entropy = std::numeric_limits<double>::infinity();

            for (int i = 0; i < w_ * h_; i++) {
                int count = 0;
                double entropy = 0;
                for (int t = 0; t < n; t++) {
                    if (domain_[i][t]) {
                        count++;
                        double p = freq_[t];
                        entropy -= p * std::log(p);
                    }
                }
                if (count == 0) return {};  // contradiction
                if (count > 1 && entropy < best_entropy) {
                    best_entropy = entropy;
                    best_cell = i;
                }
            }

            if (best_cell == -1) break;  // all cells collapsed

            // Collapse: pick a random tile from domain, weighted by frequency
            std::vector<double> weights(n, 0);
            double total = 0;
            for (int t = 0; t < n; t++) {
                if (domain_[best_cell][t]) {
                    weights[t] = freq_[t];
                    total += freq_[t];
                }
            }
            std::uniform_real_distribution<double> dist(0, total);
            double r = dist(rng_);
            int chosen = -1;
            double acc = 0;
            for (int t = 0; t < n; t++) {
                acc += weights[t];
                if (r < acc) { chosen = t; break; }
            }
            if (chosen == -1) chosen = n - 1;

            // Collapse this cell to chosen
            std::fill(domain_[best_cell].begin(), domain_[best_cell].end(), false);
            domain_[best_cell][chosen] = true;

            // Propagate
            if (!propagate(best_cell)) return {};  // contradiction during propagation
        }

        // Extract solution
        std::vector<int> result(w_ * h_);
        for (int i = 0; i < w_ * h_; i++) {
            for (int t = 0; t < n; t++) {
                if (domain_[i][t]) { result[i] = t; break; }
            }
        }
        return result;
    }

    std::string to_ascii(const std::vector<int>& grid) const {
        if (grid.empty()) return "(contradiction)\n";
        std::string out;
        for (int y = 0; y < h_; y++) {
            for (int x = 0; x < w_; x++) {
                out.push_back(tiles_[grid[y * w_ + x]].glyph);
            }
            out.push_back('\n');
        }
        return out;
    }

    int width() const { return w_; }
    int height() const { return h_; }
    const std::vector<Tile>& tiles() const { return tiles_; }

private:
    int w_, h_;
    std::vector<Tile> tiles_;
    std::vector<std::vector<bool>> domain_;
    std::vector<std::vector<std::vector<int>>> adj_;
    std::vector<double> freq_;
    std::mt19937_64 rng_;

    bool propagate(int start_cell) {
        std::vector<int> queue = {start_cell};
        while (!queue.empty()) {
            int cell = queue.back();
            queue.pop_back();
            int cx = cell % w_;
            int cy = cell / w_;

            for (int d = 0; d < 4; d++) {
                Dir dir = static_cast<Dir>(d);
                int nx = cx + dir_dx(dir);
                int ny = cy + dir_dy(dir);
                if (nx < 0 || nx >= w_ || ny < 0 || ny >= h_) continue;
                int ncell = ny * w_ + nx;

                bool changed = false;
                // For each tile possible in neighbor, check if any tile in current cell supports it
                for (int nt = 0; nt < static_cast<int>(tiles_.size()); nt++) {
                    if (!domain_[ncell][nt]) continue;
                    // Check: is there any tile in current cell that allows nt in this direction?
                    bool supported = false;
                    for (int ct = 0; ct < static_cast<int>(tiles_.size()); ct++) {
                        if (!domain_[cell][ct]) continue;
                        // Does ct allow nt in direction dir?
                        auto& allowed = adj_[ct][d];
                        if (std::find(allowed.begin(), allowed.end(), nt) != allowed.end()) {
                            supported = true;
                            break;
                        }
                    }
                    if (!supported) {
                        domain_[ncell][nt] = false;
                        changed = true;
                    }
                }
                if (changed) queue.push_back(ncell);
            }
        }
        // Check for any cell with empty domain
        for (auto& d : domain_) {
            if (std::none_of(d.begin(), d.end(), [](bool v) { return v; })) return false;
        }
        return true;
    }
};

} // namespace wfc

#endif // KESTREL_WFC_HPP