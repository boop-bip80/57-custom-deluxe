// SparseLU.h -- static-pattern sparse LU for the nodal solver.
//
// The circuit's sparsity pattern is fixed once the netlist is built (dossier Sec. 21:
// "Statically allocated matrices and cached factorizations are useful optimizations").
// The symbolic phase (minimum-degree ordering + fill-in) runs once; the numeric phase is a flat
// list of multiply-subtract operations, so the per-iteration cost is a few hundred flops
// rather than the O(n^3) of a dense solve.
//
// No numerical pivoting: the nodal matrices built here carry positive diagonals (conductances,
// companion conductances, winding R + L/h terms).  A tiny pivot is guarded and *counted* so a
// degenerate factorisation is detected instead of silently producing garbage (dossier Sec. 21:
// "returning an unconverged solution without detection is not an acceptable fidelity strategy").
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace cd57 {

class SparseLU {
public:
    // entries: structural nonzeros (row, col) of the unsymmetric matrix.  Diagonals are added.
    void init(int n, const std::vector<std::pair<int, int>>& entries)
    {
        n_ = n;
        // ---- symmetrised adjacency for ordering / fill
        std::vector<std::vector<char>> adj(n, std::vector<char>(n, 0));
        for (auto& e : entries) {
            adj[e.first][e.second] = 1;
            adj[e.second][e.first] = 1;
        }
        for (int i = 0; i < n; ++i) adj[i][i] = 1;

        // ---- minimum-degree ordering (greedy, O(n^3) once)
        std::vector<char> done(n, 0);
        perm_.assign(n, 0);
        iperm_.assign(n, 0);
        auto work = adj;
        for (int k = 0; k < n; ++k) {
            int best = -1, bestDeg = 1 << 30;
            for (int i = 0; i < n; ++i) {
                if (done[i]) continue;
                int deg = 0;
                for (int j = 0; j < n; ++j)
                    if (!done[j] && j != i && work[i][j]) ++deg;
                if (deg < bestDeg) { bestDeg = deg; best = i; }
            }
            perm_[k] = best;
            iperm_[best] = k;
            done[best] = 1;
            // eliminate: neighbours of best become a clique
            std::vector<int> nb;
            for (int j = 0; j < n; ++j)
                if (!done[j] && work[best][j]) nb.push_back(j);
            for (int a : nb)
                for (int b : nb) work[a][b] = 1;
        }

        // ---- symbolic factorisation in permuted index space (symmetric pattern incl. fill)
        std::vector<std::vector<char>> pat(n, std::vector<char>(n, 0));
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                if (adj[perm_[i]][perm_[j]]) pat[i][j] = 1;
        for (int k = 0; k < n; ++k)
            for (int i = k + 1; i < n; ++i)
                if (pat[i][k])
                    for (int j = k + 1; j < n; ++j)
                        if (pat[k][j]) pat[i][j] = 1;

        // ---- slot numbering (row-major over pattern)
        slot_.assign(n * n, -1);
        int cnt = 0;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                if (pat[i][j]) slot_[i * n + j] = cnt++;
        nnz_ = cnt;

        // ---- numeric op lists
        diag_.assign(n, 0);
        lcols_.assign(n, {});
        urows_.assign(n, {});
        ops_.clear();
        opStart_.assign(n + 1, 0);
        lStart_.assign(n + 1, 0);
        lIdx_.clear();
        lSlot_.clear();
        for (int k = 0; k < n; ++k) {
            diag_[k] = slot_[k * n + k];
            opStart_[k] = (int)ops_.size();
            lStart_[k] = (int)lIdx_.size();
            std::vector<int> rows, cols;
            for (int i = k + 1; i < n; ++i) if (pat[i][k]) rows.push_back(i);
            for (int j = k + 1; j < n; ++j) if (pat[k][j]) cols.push_back(j);
            for (int i : rows) {
                lIdx_.push_back(i);
                lSlot_.push_back(slot_[i * n + k]);
                for (int j : cols) ops_.push_back({slot_[i * n + j], slot_[i * n + k], slot_[k * n + j]});
            }
            lcols_[k] = rows;
            urows_[k] = cols;
        }
        opStart_[n] = (int)ops_.size();
        lStart_[n] = (int)lIdx_.size();
        // back-substitution row lists (flat)
        uStart_.assign(n + 1, 0);
        uIdx_.clear();
        uSlot_.clear();
        for (int k = 0; k < n; ++k) {
            uStart_[k] = (int)uIdx_.size();
            for (int j : urows_[k]) { uIdx_.push_back(j); uSlot_.push_back(slot_[k * n + j]); }
        }
        uStart_[n] = (int)uIdx_.size();
        y_.assign(n, 0.0);
        inv_.assign(n, 1.0);
    }

    int n() const { return n_; }
    int nnz() const { return nnz_; }
    size_t opCount() const { return ops_.size(); }

    // slot of original-index entry (i,j); -1 if outside pattern
    int slot(int i, int j) const { return slot_[iperm_[i] * n_ + iperm_[j]]; }

    // in-place numeric factorisation of vals (size nnz).  Returns number of guarded pivots.
    int factor(double* v)
    {
        int guarded = 0;
        const int n = n_;
        for (int k = 0; k < n; ++k) {
            double piv = v[diag_[k]];
            if (std::fabs(piv) < 1e-30) {
                piv = piv < 0 ? -1e-30 : 1e-30;
                v[diag_[k]] = piv;
                ++guarded;
            }
            const double inv = 1.0 / piv;
            inv_[k] = inv;
            for (int q = lStart_[k]; q < lStart_[k + 1]; ++q) v[lSlot_[q]] *= inv;
            for (int q = opStart_[k]; q < opStart_[k + 1]; ++q) {
                const Op& o = ops_[q];
                v[o.t] -= v[o.l] * v[o.u];
            }
        }
        return guarded;
    }

    // solve LU x = b ; b is in original ordering and is overwritten with x.
    void solve(const double* v, double* b)
    {
        const int n = n_;
        for (int k = 0; k < n; ++k) y_[k] = b[perm_[k]];
        for (int k = 0; k < n; ++k) {
            const double yk = y_[k];
            for (int q = lStart_[k]; q < lStart_[k + 1]; ++q) y_[lIdx_[q]] -= v[lSlot_[q]] * yk;
        }
        for (int k = n - 1; k >= 0; --k) {
            double s = y_[k];
            for (int q = uStart_[k]; q < uStart_[k + 1]; ++q) s -= v[uSlot_[q]] * y_[uIdx_[q]];
            y_[k] = s * inv_[k];
        }
        for (int k = 0; k < n; ++k) b[perm_[k]] = y_[k];
    }

private:
    struct Op { int t, l, u; };
    int n_ = 0, nnz_ = 0;
    std::vector<int> perm_, iperm_, slot_, diag_;
    std::vector<std::vector<int>> lcols_, urows_;
    std::vector<Op> ops_;
    std::vector<int> opStart_, lStart_, lIdx_, lSlot_, uStart_, uIdx_, uSlot_;
    std::vector<double> y_, inv_;
};

} // namespace cd57
