// Network.h -- netlist parser + modified-nodal-analysis nonlinear transient solver.
//
// Implements the reference approach of dossier Sec. 21/31:
//   * topology lives in a readable, versioned netlist (data/netlist/*.net), independent of solver code;
//   * unknowns are node voltages plus the winding currents of the output transformer;
//   * dynamic elements use a theta-method companion (theta = 1/2 trapezoidal, 1 backward Euler);
//   * valves and rectifier diodes stamp terminal currents whose algebraic sum is zero, with analytic
//     Jacobians including the moving cathode and screen nodes;
//   * Newton iteration with step limiting; failures are COUNTED, never hidden;
//   * the output transformer tracks core flux (flux-linkage state) with a saturating core law.
//
// Physical state (capacitor charge, inductor/winding flux, rail voltages) persists across calls.
#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Devices.h"
#include "SparseLU.h"
#if __has_include("LuKernel.h")
#include "LuKernel.h"
#define CD57_HAVE_LU_KERNEL 1
#endif

namespace cd57 {

struct ElemMeta {
    std::string name, evidence, source, note;
    double tol = 0.0;        // 1-sigma fractional tolerance for the optional specimen-variation feature
    bool dyn = false;        // value is driven at runtime (pots, jack switches, speaker, ...)
    bool novar = false;
};

class Network {
public:
    // ------------------------------------------------------------------ element records
    struct Res  { ElemMeta m; int a, b; double R, Rnom; double var = 1.0; };
    struct Cap  { ElemMeta m; int a, b; double C, Cnom, var = 1.0; double vprev = 0, iprev = 0, g = 0, ih = 0;
                  double thetaSel = -1.0, th = 0.5, cth = 1.0; };
    struct Ind  { ElemMeta m; int a, b; double L, Lnom, var = 1.0; double vprev = 0, iprev = 0, g = 0, ih = 0; };
    struct Tri  { ElemMeta m; int p, g, k; std::string type; TriodeParams par; int emGroup = 0; double scale = 1.0;
                  int s[9]; double ip = 0, ig = 0; };
    struct Pen  { ElemMeta m; int p, g, s, k; PentodeParams par; int emGroup = 1; double scale = 1.0;
                  int sl[16]; double ip = 0, is = 0, ig = 0; };
    struct Dio  { ElemMeta m; int a, k; DiodeParams par; int emGroup = 2; int s[4]; double i = 0; };
    struct Xfm  {
        ElemMeta m;
        int n1[3], n2[3];              // winding terminals (first, second), windings: A, B, S
        double N[3];                   // turns (secondary-referred: Ns = 1)
        double R[3], Ll[3];            // winding resistance, leakage inductance
        double P0, phiSat, Pair;       // core permeance, saturation flux per turn, post-saturation permeance
        bool satOn = false;            // saturating law active?  default OFF: dossier Sec. 12/29 (no saturation law before evidence)
        int row[3];                    // unknown index of each winding current
        int slotRowNode[3][2];         // (row_k, n1) (row_k, n2)
        int slotNodeRow[3][2];         // (n1,row_k) (n2,row_k)
        int slotBlock[3][3];           // (row_k,row_j)
        double lamPrev[3] = {0, 0, 0}, lamDotPrev[3] = {0, 0, 0};
        double Rvar[3] = {1, 1, 1};
        double i[3] = {0, 0, 0};
    };

    // ------------------------------------------------------------------ construction
    void parse(const std::string& text)
    {
        std::istringstream in(text);
        std::string line;
        int lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            try { parseLine(line); }
            catch (const std::exception& e) {
                throw std::runtime_error("netlist line " + std::to_string(lineNo) + ": " + e.what() + "  [" + line + "]");
            }
        }
    }

    int nodeId(const std::string& name) const
    {
        if (name == "0" || name == "gnd") return -1;
        auto it = nodeMap_.find(name);
        if (it == nodeMap_.end()) throw std::runtime_error("unknown node " + name);
        return it->second;
    }
    int nodeCount() const { return nn_; }
    const std::vector<std::string>& nodeNames() const { return nodeNames_; }
    int unknowns() const { return N_; }
    size_t luOps() const { return lu_.opCount(); }
    bool luSpecialised() const { return lu_.specialised(); }
    std::string emitLuKernel() const { return lu_.emitKernel(); }
    bool useKernel = true;
    int luNnz() const { return lu_.nnz(); }
    double tEval = 0, tFactor = 0, tSolve = 0; bool profile = false;
    void (*traceCb)(const Network&, int it, double maxdz, double eta) = nullptr;
    std::vector<uint64_t> failHist;   // per-unknown count of 'largest residual' at a failed solve (diagnostics)

    int resId(const std::string& n) const { return find(resMap_, n, "resistor"); }
    int capId(const std::string& n) const { return find(capMap_, n, "capacitor"); }
    int indId(const std::string& n) const { return find(indMap_, n, "inductor"); }
    bool hasRes(const std::string& n) const { return resMap_.count(n) != 0; }

    std::vector<Res>& res() { return res_; }
    std::vector<Cap>& cap() { return cap_; }
    std::vector<Ind>& ind() { return ind_; }
    std::vector<Tri>& tri() { return tri_; }
    std::vector<Pen>& pen() { return pen_; }
    std::vector<Dio>& dio() { return dio_; }
    Xfm& xfm() { return xfm_; }
    bool hasXfm() const { return hasXfm_; }

    // runtime value setters (call refreshLinear() afterwards, at control rate)
    void setR(int id, double R)
    {
        R = std::max(R, 1e-6);
        if (std::fabs(R - res_[id].R) > 1e-9 * res_[id].R) { res_[id].R = R; dirty_ = true; }
    }
    void setC(int id, double C) { C = std::max(C, 0.0); if (C != cap_[id].C) { cap_[id].C = C; dirty_ = true; } }
    void setL(int id, double L) { L = std::max(L, 1e-12); if (L != ind_[id].L) { ind_[id].L = L; dirty_ = true; } }
    double getR(int id) const { return res_[id].R; }

    void setEmission(int group, double e) { emission_[group] = e; }
    double emission(int group) const { return emission_[group]; }

    // ------------------------------------------------------------------ build / assemble
    // Compile the pattern, factorisation order and buffers for step h (seconds) and theta.
    void build(double h, double theta)
    {
        h_ = h; hAct_ = h; divIdx_ = 0; theta_ = theta; cth_ = (1.0 - theta) / theta;
        for (auto& c : cap_) { c.th = c.thetaSel > 0 ? c.thetaSel : theta; c.cth = (1.0 - c.th) / c.th; }
        nn_ = (int)nodeNames_.size();
        N_ = nn_ + (hasXfm_ ? 3 : 0);
        if (hasXfm_) for (int k = 0; k < 3; ++k) xfm_.row[k] = nn_ + k;

        std::vector<std::pair<int, int>> touches;
        auto T = [&](int r, int c) { if (r >= 0 && c >= 0) touches.emplace_back(r, c); };
        for (int i = 0; i < N_; ++i) T(i, i);
        auto T2 = [&](int a, int b) { T(a, a); T(b, b); T(a, b); T(b, a); };
        for (auto& r : res_) T2(r.a, r.b);
        for (auto& c : cap_) T2(c.a, c.b);
        for (auto& l : ind_) T2(l.a, l.b);
        for (auto& t : tri_) { int n[3] = {t.p, t.g, t.k}; for (int i : n) for (int j : n) T(i, j); }
        for (auto& p : pen_) { int n[4] = {p.p, p.g, p.s, p.k}; for (int i : n) for (int j : n) T(i, j); }
        for (auto& d : dio_) T2(d.a, d.k);
        if (hasXfm_) {
            for (int k = 0; k < 3; ++k) {
                int rk = xfm_.row[k];
                T(xfm_.n1[k], rk); T(xfm_.n2[k], rk); T(rk, xfm_.n1[k]); T(rk, xfm_.n2[k]);
                for (int j = 0; j < 3; ++j) T(rk, xfm_.row[j]);
            }
        }
        lu_.init(N_, touches);
        nnz_ = lu_.nnz();
#ifdef CD57_HAVE_LU_KERNEL
        if (useKernel) lu_.installKernel(lukernel::kSignature, &lukernel::factor, &lukernel::solve);
#endif

        // slot tables
        auto S = [&](int r, int c) { return (r < 0 || c < 0) ? -1 : lu_.slot(r, c); };
        for (auto& t : tri_) { int n[3] = {t.p, t.g, t.k}; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) t.s[i * 3 + j] = S(n[i], n[j]); }
        for (auto& p : pen_) { int n[4] = {p.p, p.g, p.s, p.k}; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) p.sl[i * 4 + j] = S(n[i], n[j]); }
        for (auto& d : dio_) { d.s[0] = S(d.a, d.a); d.s[1] = S(d.a, d.k); d.s[2] = S(d.k, d.a); d.s[3] = S(d.k, d.k); }
        if (hasXfm_) {
            for (int k = 0; k < 3; ++k) {
                int rk = xfm_.row[k];
                xfm_.slotRowNode[k][0] = S(rk, xfm_.n1[k]); xfm_.slotRowNode[k][1] = S(rk, xfm_.n2[k]);
                xfm_.slotNodeRow[k][0] = S(xfm_.n1[k], rk); xfm_.slotNodeRow[k][1] = S(xfm_.n2[k], rk);
                for (int j = 0; j < 3; ++j) xfm_.slotBlock[k][j] = S(rk, xfm_.row[j]);
            }
        }
        diagSlot_.assign(N_, -1);
        for (int i = 0; i < N_; ++i) diagSlot_[i] = lu_.slot(i, i);

        linVals_.assign(nnz_, 0.0);
        vals_.assign(nnz_, 0.0);
        linFlag_.assign(nnz_, 0);
        z_.assign(N_, 0.0); zprev_.assign(N_, 0.0); zprev2_.assign(N_, 0.0); zprev3_.assign(N_, 0.0);
        b_.assign(N_, 0.0); F_.assign(N_, 0.0); dz_.assign(N_, 0.0);
        inj_.assign(nn_, 0.0);
        // everything step() / newton() / the sub-step fallback touch is sized here, so the audio thread never allocates
        zOld_.assign(N_, 0.0); dzOld_.assign(N_, 0.0);
        injOld_ = inj_; injNew_.assign(nn_, 0.0);
        failHist.assign(N_, 0);
        snapshot(snap_); snapshot(xfer_);
        // slot->(row,col) inverse for linear matvec
        slotRow_.assign(nnz_, -1); slotCol_.assign(nnz_, -1);
        for (int i = 0; i < N_; ++i) for (int j = 0; j < N_; ++j) { int s = lu_.slot(i, j); if (s >= 0) { slotRow_[s] = i; slotCol_[s] = j; } }
        built_ = true;
        assembleLinear(false);
    }

    // Recompute the constant (linear) part of the Jacobian from current element values.
    void refreshLinear() { assembleLinear(dcMode_); dirty_ = false; }
    bool refreshIfDirty() { if (!dirty_) return false; refreshLinear(); return true; }
    void markDirty() { dirty_ = true; }

    // ------------------------------------------------------------------ state access
    double V(int node) const { return node < 0 ? 0.0 : z_[node]; }
    double& inj(int node) { return inj_[node]; }
    std::vector<double>& injVec() { return inj_; }
    const std::vector<double>& state() const { return z_; }
    std::vector<double>& stateMut() { return z_; }
    double h() const { return h_; }
    double theta() const { return theta_; }

    // solver statistics (dossier Sec. 21: "Record solver failures and iteration statistics")
    struct Stats { uint64_t steps = 0, iterations = 0, failures = 0, substepped = 0, maxDivisor = 1, guardedPivots = 0, maxIterSeen = 0; double maxResidual = 0; };
    Stats stats;
    int maxIter = 20;
    double absTolV = 1e-2, relTol = 1e-4;
    // Nodal KCL acceptance threshold (amperes).  The step-size test above scales with the absolute node potential, which is blind to
    // stiff devices at high common-mode voltage (1e-4 * 457 V = 46 mV is one e-fold of a silicon diode's current), so a solve is
    // accepted only if the nodal residual is also below this value.  Normal operation sits 2-3 decades below it.
    double resTolA = 2e-5;
    double stepLimitV = 400.0, gridStepLimit = 120.0;
    double predictorGain = 1.0;
    int predictorOrder = 2;                 // 1 = linear, 2 = quadratic extrapolation of the previous accepted states
    bool trackResidual = false;
    double lastResidual = 0.0;
    int lastIterations = 0;

    // ------------------------------------------------------------------ DC operating point
    // Source-stepped Newton solve with capacitors open, inductors/windings shorted (R only).
    // inj must hold the DC source currents (Norton) at full scale.
    bool solveDC(int steps = 30)
    {
        const std::vector<double> full = inj_;
        dcMode_ = true;
        assembleLinear(true);
        std::fill(z_.begin(), z_.end(), 0.0);
        bool ok = true;
        auto dcRhs = [&]() { std::fill(b_.begin(), b_.end(), 0.0); for (int i = 0; i < nn_; ++i) b_[i] = inj_[i]; };
        for (int s = 1; s <= steps; ++s) {
            const double sc = double(s) / steps;
            for (int i = 0; i < nn_; ++i) inj_[i] = full[i] * sc;
            dcRhs();
            ok = newton(true, 80) && ok;
        }
        inj_ = full;
        dcRhs();
        ok = newton(true, 200) && ok;
        // consistent initial state for transient
        dcMode_ = false;
        assembleLinear(false);
        for (auto& c : cap_) { c.vprev = V(c.a) - V(c.b); c.iprev = 0.0; }
        for (auto& l : ind_) { l.vprev = V(l.a) - V(l.b); l.iprev = (V(l.a) - V(l.b)) * 1e4; }
        if (hasXfm_) {
            double F = 0;
            for (int k = 0; k < 3; ++k) { xfm_.i[k] = z_[xfm_.row[k]]; F += xfm_.N[k] * xfm_.i[k]; }
            const double phi = corePhi(F);
            for (int k = 0; k < 3; ++k) { xfm_.lamPrev[k] = xfm_.Ll[k] * xfm_.i[k] + xfm_.N[k] * phi; xfm_.lamDotPrev[k] = 0.0; }
        }
        zprev_ = z_; zprev2_ = z_; zprev3_ = z_;
        return ok;
    }

    // ------------------------------------------------------------------ transient step
    struct Snapshot {
        std::vector<double> z, zp, zp2, zp3, cv, ci, lv, li;
        double lam[3], lamd[3], cur[3];
    };
    void snapshot(Snapshot& S) const
    {
        S.z = z_; S.zp = zprev_; S.zp2 = zprev2_; S.zp3 = zprev3_;
        S.cv.resize(cap_.size()); S.ci.resize(cap_.size()); S.lv.resize(ind_.size()); S.li.resize(ind_.size());
        for (size_t i = 0; i < cap_.size(); ++i) { S.cv[i] = cap_[i].vprev; S.ci[i] = cap_[i].iprev; }
        for (size_t i = 0; i < ind_.size(); ++i) { S.lv[i] = ind_[i].vprev; S.li[i] = ind_[i].iprev; }
        for (int k = 0; k < 3; ++k) { S.lam[k] = xfm_.lamPrev[k]; S.lamd[k] = xfm_.lamDotPrev[k]; S.cur[k] = xfm_.i[k]; }
    }
    void restore(const Snapshot& S)
    {
        z_ = S.z; zprev_ = S.zp; zprev2_ = S.zp2; zprev3_ = S.zp3;
        for (size_t i = 0; i < cap_.size(); ++i) { cap_[i].vprev = S.cv[i]; cap_[i].iprev = S.ci[i]; }
        for (size_t i = 0; i < ind_.size(); ++i) { ind_[i].vprev = S.lv[i]; ind_[i].iprev = S.li[i]; }
        for (int k = 0; k < 3; ++k) { xfm_.lamPrev[k] = S.lam[k]; xfm_.lamDotPrev[k] = S.lamd[k]; xfm_.i[k] = S.cur[k]; }
    }

    // Carry the running circuit state over from another Network built from the SAME netlist at a different step h (e.g. after an
    // oversampling change).  Only rate-independent physical quantities are copied: node voltages and winding currents, capacitor
    // and inductor voltages/currents, transformer flux linkages.  The predictor histories restart from the copied state.  Allocation-free
    // (the transfer buffer is sized in build()); returns false, copying nothing, if the two circuits differ.
    bool copyStateFrom(const Network& o)
    {
        if (N_ != o.N_ || cap_.size() != o.cap_.size() || ind_.size() != o.ind_.size() || !built_ || !o.built_) return false;
        o.snapshot(xfer_);
        restore(xfer_);
        zprev_ = z_; zprev2_ = z_; zprev3_ = z_;
        injOld_ = inj_;
        return true;
    }

    // One audio-step.  Fast path: single Newton solve.  Fallback (dossier Sec. 21): retry the same interval as
    // 2,4,8,16,32 sub-steps with the injected sources interpolated; statistics record every fallback.
    bool step()
    {
        if (injOld_.size() != inj_.size()) injOld_ = inj_;
        buildRhs();
        if (predictorGain != 0.0) {
            if (predictorOrder >= 2)
                for (int i = 0; i < N_; ++i) z_[i] = 3.0 * zprev_[i] - 3.0 * zprev2_[i] + zprev3_[i];
            else
                for (int i = 0; i < N_; ++i) z_[i] += predictorGain * (zprev_[i] - zprev2_[i]);
        }
        bool ok = newton(false, maxIter);
        ++stats.steps;
        if (ok) { commit(); injOld_ = inj_; return true; }

        // ---- fallback: adaptive sub-stepping from the last accepted state
        ++stats.substepped;
        z_ = zprev_;
        Snapshot& S0 = snap_;                    // preallocated in build(): this path runs on the audio thread
        snapshot(S0);
        injNew_ = inj_;
        const std::vector<double>& injNew = injNew_;
        bool done = false;
        for (int idx = 1; idx < kDivs && !done; ++idx) {
            restore(S0);
            setDivisor(idx);
            const int m = 1 << idx;
            bool all = true;
            for (int k = 1; k <= m; ++k) {
                const double f = double(k) / m;
                for (size_t i = 0; i < inj_.size(); ++i) inj_[i] = injOld_[i] + f * (injNew[i] - injOld_[i]);
                buildRhs();
                if (!newton(false, maxIter)) { all = false; break; }
                commit();
            }
            if (all) { done = true; stats.maxDivisor = std::max<uint64_t>(stats.maxDivisor, (uint64_t)m); }
        }
        setDivisor(0);
        inj_ = injNew;
        if (!done) {
            // unrecoverable: hold the last accepted state (bounded fallback) and COUNT it
            restore(S0);
            ++stats.failures;
            int worst = 0; double wv = 0;
            for (int i = 0; i < N_; ++i) if (std::fabs(dz_[i]) > wv) { wv = std::fabs(dz_[i]); worst = i; }
            ++failHist[worst];
        }
        injOld_ = inj_;
        return done;
    }


    // Development check (dossier Sec. 31): compare the assembled analytic Jacobian with central finite
    // differences of the residual at state zt.  Returns the worst absolute error; location via out params.
    double checkJacobian(const std::vector<double>& zt, int& wr, int& wc, double& analytic, double& numeric, bool dc = false, double eps = 1e-4)
    {
        buildRhsForCheck(dc);
        z_ = zt;
        evalSystem(dc, true);
        std::vector<double> J((size_t)N_ * N_, 0.0);
        for (int i = 0; i < N_; ++i) for (int j = 0; j < N_; ++j) { int sl = lu_.slot(i, j); if (sl >= 0) J[(size_t)i * N_ + j] = vals_[sl]; }
        double worst = 0; wr = wc = 0; analytic = numeric = 0;
        std::vector<double> Fp, Fm;
        for (int j = 0; j < N_; ++j) {
            const double d = eps * (1.0 + std::fabs(zt[j]) * 0.0);
            z_ = zt; z_[j] += d; evalSystem(dc, false); Fp = F_;
            z_ = zt; z_[j] -= d; evalSystem(dc, false); Fm = F_;
            for (int i = 0; i < N_; ++i) {
                const double nd = (Fp[i] - Fm[i]) / (2 * d);
                const double a = J[(size_t)i * N_ + j];
                const double err = std::fabs(nd - a) / (1e-6 + std::fabs(a) + std::fabs(nd));
                if (err > worst) { worst = err; wr = i; wc = j; analytic = a; numeric = nd; }
            }
        }
        z_ = zt;
        return worst;
    }
    void buildRhsForCheck(bool dc) { if (dc) { std::fill(b_.begin(), b_.end(), 0.0); } else buildRhs(); }


    // ------------------------------------------------------------------ independent AC solver (validation)
    // Dense complex MNA of the circuit linearised at the CURRENT state z_ (call after the transient/DC has settled).
    // Deliberately shares no code with the sparse transient solver: element stamps are rebuilt from the element
    // lists and valve derivatives, so agreement between the two is an independent check (dossier Sec. 21).
    // Injects 1 A (AC) into node `injNode`; returns the complex node voltage vector (size nn_) of the response.
    std::vector<std::complex<double>> acSolve(double freq, int injNode) const
    {
        using C = std::complex<double>;
        const int n = N_;
        const double w = 2.0 * 3.14159265358979323846 * freq;
        std::vector<C> A((size_t)n * n, C(0, 0)), b(n, C(0, 0));
        auto at = [&](int r, int c) -> C& { return A[(size_t)r * n + c]; };
        auto st2 = [&](int a, int bb, C y) { if (a >= 0) at(a, a) += y; if (bb >= 0) at(bb, bb) += y; if (a >= 0 && bb >= 0) { at(a, bb) -= y; at(bb, a) -= y; } };
        for (int i = 0; i < nn_; ++i) at(i, i) += 1e-12;
        for (auto& r : res_) st2(r.a, r.b, C(1.0 / (r.R * r.var), 0));
        for (auto& c : cap_) st2(c.a, c.b, C(0, w * c.C * c.var));
        for (auto& l : ind_) st2(l.a, l.b, C(1.0, 0) / C(0, w * l.L * l.var));
        auto Vn = [&](int k) { return k < 0 ? 0.0 : z_[k]; };
        for (auto& t : tri_) {
            TriodeOut o; evalTriode(t.par, Vn(t.g) - Vn(t.k), Vn(t.p) - Vn(t.k), emission_[t.emGroup] * t.scale, o);
            const int P = t.p, G = t.g, K = t.k;
            auto add = [&](int r, int c, double x) { if (r >= 0 && c >= 0) at(r, c) += x; };
            add(P, P, o.dip_dvpk); add(P, G, o.dip_dvgk); add(P, K, -(o.dip_dvgk + o.dip_dvpk));
            add(G, G, o.dig_dvgk); add(G, K, -o.dig_dvgk);
            add(K, P, -o.dip_dvpk); add(K, G, -(o.dip_dvgk + o.dig_dvgk)); add(K, K, o.dip_dvgk + o.dip_dvpk + o.dig_dvgk);
        }
        for (auto& q : pen_) {
            PentodeOut o; evalPentode(q.par, Vn(q.g) - Vn(q.k), Vn(q.p) - Vn(q.k), Vn(q.s) - Vn(q.k), emission_[q.emGroup] * q.scale, o);
            const int nd[4] = {q.p, q.g, q.s, q.k};
            const double ip[4] = {o.dip[1], o.dip[0], o.dip[2], -(o.dip[0] + o.dip[1] + o.dip[2])};
            const double is[4] = {o.dis[1], o.dis[0], o.dis[2], -(o.dis[0] + o.dis[1] + o.dis[2])};
            const double ig[4] = {0.0, o.dig_dvgk, 0.0, -o.dig_dvgk};
            for (int c = 0; c < 4; ++c) {
                if (nd[c] < 0) continue;
                if (nd[0] >= 0) at(nd[0], nd[c]) += ip[c];
                if (nd[2] >= 0) at(nd[2], nd[c]) += is[c];
                if (nd[1] >= 0) at(nd[1], nd[c]) += ig[c];
                if (nd[3] >= 0) at(nd[3], nd[c]) -= (ip[c] + is[c] + ig[c]);
            }
        }
        for (auto& d : dio_) {
            double i, di; evalDiode(d.par, Vn(d.a) - Vn(d.k), emission_[d.emGroup], i, di);
            st2(d.a, d.k, C(di, 0));
        }
        if (hasXfm_) {
            double Fm = 0; for (int k = 0; k < 3; ++k) Fm += xfm_.N[k] * z_[xfm_.row[k]];
            const double dphi = corePhiPrime(Fm);
            for (int k = 0; k < 3; ++k) {
                const int rk = xfm_.row[k];
                if (xfm_.n1[k] >= 0) at(xfm_.n1[k], rk) += 1.0;
                if (xfm_.n2[k] >= 0) at(xfm_.n2[k], rk) -= 1.0;
                if (xfm_.n1[k] >= 0) at(rk, xfm_.n1[k]) -= 1.0;
                if (xfm_.n2[k] >= 0) at(rk, xfm_.n2[k]) += 1.0;
                at(rk, rk) += xfm_.R[k] * xfm_.Rvar[k] + C(0, w * xfm_.Ll[k]);
                for (int j = 0; j < 3; ++j) at(rk, xfm_.row[j]) += C(0, w) * xfm_.N[k] * dphi * xfm_.N[j];
            }
        }
        b[injNode] = C(1.0, 0);
        // Gaussian elimination, partial pivoting
        std::vector<int> piv(n);
        for (int i = 0; i < n; ++i) piv[i] = i;
        for (int k = 0; k < n; ++k) {
            int pr = k; double best = std::abs(at(k, k));
            for (int r = k + 1; r < n; ++r) if (std::abs(at(r, k)) > best) { best = std::abs(at(r, k)); pr = r; }
            if (pr != k) { for (int c = 0; c < n; ++c) std::swap(at(k, c), at(pr, c)); std::swap(b[k], b[pr]); }
            const C d = at(k, k);
            for (int r = k + 1; r < n; ++r) {
                const C f = at(r, k) / d;
                if (std::abs(f) == 0.0) continue;
                for (int c = k; c < n; ++c) at(r, c) -= f * at(k, c);
                b[r] -= f * b[k];
            }
        }
        std::vector<C> x(n);
        for (int k = n - 1; k >= 0; --k) {
            C s = b[k];
            for (int c = k + 1; c < n; ++c) s -= at(k, c) * x[c];
            x[k] = s / at(k, k);
        }
        x.resize(nn_);
        return x;
    }

    // power flowing out of node-injected sources etc. is left to the caller; expose element currents:
    double inductorCurrent(int id) const { return ind_[id].iprev; }
    double capCurrent(int id) const { return cap_[id].iprev; }
    double capVoltage(int id) const { return cap_[id].vprev; }
    double windingCurrent(int k) const { return xfm_.i[k]; }
    double corePhiNow() const { return lastPhi_; }
    double resCurrent(int id) const { const auto& r = res_[id]; return (V(r.a) - V(r.b)) / (r.R * r.var); }

    void resetState()
    {
        std::fill(z_.begin(), z_.end(), 0.0);
        zprev_ = z_; zprev2_ = z_; zprev3_ = z_;
        for (auto& c : cap_) { c.vprev = c.iprev = 0; }
        for (auto& l : ind_) { l.vprev = l.iprev = 0; }
        if (hasXfm_) for (int k = 0; k < 3; ++k) { xfm_.lamPrev[k] = xfm_.lamDotPrev[k] = 0; xfm_.i[k] = 0; }
    }

    // Core law.  Default (dossier Sec. 12, 29: "begin with ... a magnetizing branch", "do not assign a saturation law before the
    // evidence is available"): linear, phi(F) = (P0 + Pair) F -- the exact small-signal limit of the saturating law, so the low-level
    // response is identical.  Optional lab feature (ASSUMED law, ASSUMED phiSat): phi(F) = phiSat tanh(P0 F/phiSat) + Pair F.
    double corePhi(double F) const
    {
        if (!xfm_.satOn) return (xfm_.P0 + xfm_.Pair) * F;
        return xfm_.phiSat * std::tanh(xfm_.P0 * F / xfm_.phiSat) + xfm_.Pair * F;
    }
    double corePhiPrime(double F) const
    {
        if (!xfm_.satOn) return xfm_.P0 + xfm_.Pair;
        const double t = std::tanh(xfm_.P0 * F / xfm_.phiSat);
        return xfm_.P0 * (1.0 - t * t) + xfm_.Pair;
    }
    // Switch the saturating law on/off at run time without a flux discontinuity: the winding flux linkages are re-derived from
    // the present winding currents under the new law.
    void setCoreSaturation(bool on)
    {
        if (!hasXfm_ || xfm_.satOn == on) return;
        xfm_.satOn = on;
        double F = 0;
        for (int k = 0; k < 3; ++k) F += xfm_.N[k] * xfm_.i[k];
        const double phi = corePhi(F);
        for (int k = 0; k < 3; ++k) xfm_.lamPrev[k] = xfm_.Ll[k] * xfm_.i[k] + xfm_.N[k] * phi;
    }
    bool coreSaturation() const { return hasXfm_ && xfm_.satOn; }

    // ------------------------------------------------------------------ introspection (netlist doc)
    std::string describe() const
    {
        std::ostringstream o;
        auto nm = [&](int n) { return n < 0 ? std::string("0") : nodeNames_[n]; };
        char buf[256];
        o << "| Ref | Type | Nodes | Value | Evidence | Source | Note |\n|---|---|---|---|---|---|---|\n";
        auto row = [&](const ElemMeta& m, const char* ty, const std::string& nodes, const std::string& val) {
            o << "| " << m.name << " | " << ty << " | " << nodes << " | " << val << " | " << m.evidence << " | " << m.source << " | " << m.note << (m.dyn ? " (runtime-driven)" : "") << " |\n";
        };
        for (auto& r : res_) { std::snprintf(buf, sizeof buf, "%.6g Ω", r.Rnom); row(r.m, "R", nm(r.a) + "–" + nm(r.b), buf); }
        for (auto& c : cap_) { std::snprintf(buf, sizeof buf, "%.6g F", c.Cnom); row(c.m, "C", nm(c.a) + "–" + nm(c.b), buf); }
        for (auto& l : ind_) { std::snprintf(buf, sizeof buf, "%.6g H", l.Lnom); row(l.m, "L", nm(l.a) + "–" + nm(l.b), buf); }
        for (auto& t : tri_) row(t.m, "TRIODE", "P=" + nm(t.p) + " G=" + nm(t.g) + " K=" + nm(t.k), t.type);
        for (auto& p : pen_) row(p.m, "PENTODE", "P=" + nm(p.p) + " G=" + nm(p.g) + " S=" + nm(p.s) + " K=" + nm(p.k), "6V6");
        for (auto& d : dio_) row(d.m, "DIODE", "A=" + nm(d.a) + " K=" + nm(d.k), d.par.type == RectifierType::Tube5Y3 ? "5Y3 anode" : "silicon");
        if (hasXfm_) {
            std::snprintf(buf, sizeof buf, "Nh=%.4f Ns=%.1f P0=%.4g H phiSat=%.4g Vs", xfm_.N[0], xfm_.N[2], xfm_.P0, xfm_.phiSat);
            row(xfm_.m, "XFMR", "A:" + nm(xfm_.n1[0]) + "–" + nm(xfm_.n2[0]) + " B:" + nm(xfm_.n1[1]) + "–" + nm(xfm_.n2[1]) + " S:" + nm(xfm_.n1[2]) + "–" + nm(xfm_.n2[2]), buf);
        }
        return o.str();
    }

    // reference-node helpers for the validation tools
    double kclResidualNow() const { return lastResidual; }

private:
    // ------------------------------------------------------------------ parsing
    static double parseValue(const std::string& s)
    {
        char* end = nullptr;
        double v = std::strtod(s.c_str(), &end);
        if (end == s.c_str()) throw std::runtime_error("bad number '" + s + "'");
        std::string suf(end);
        for (auto& ch : suf) ch = (char)std::tolower((unsigned char)ch);
        double m = 1.0;
        if (suf.rfind("meg", 0) == 0) m = 1e6;
        else if (!suf.empty()) {
            switch (suf[0]) {
                case 't': m = 1e12; break; case 'g': m = 1e9; break; case 'k': m = 1e3; break;
                case 'm': m = 1e-3; break; case 'u': m = 1e-6; break; case 'n': m = 1e-9; break;
                case 'p': m = 1e-12; break; case 'f': m = 1e-15; break;
                default: break;
            }
        }
        return v * m;
    }

    int getNode(const std::string& name)
    {
        if (name == "0" || name == "gnd") return -1;
        auto it = nodeMap_.find(name);
        if (it != nodeMap_.end()) return it->second;
        int id = (int)nodeNames_.size();
        nodeNames_.push_back(name);
        nodeMap_[name] = id;
        return id;
    }

    static int find(const std::map<std::string, int>& m, const std::string& n, const char* what)
    {
        auto it = m.find(n);
        if (it == m.end()) throw std::runtime_error(std::string("unknown ") + what + " " + n);
        return it->second;
    }

    void parseLine(std::string line)
    {
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        std::vector<std::string> tok;
        {
            std::istringstream ss(line);
            std::string t;
            while (ss >> t) tok.push_back(t);
        }
        if (tok.empty()) return;
        if (tok[0][0] == '.') {                       // directives
            if (tok[0] == ".title") { title = line.substr(line.find(".title") + 6); }
            else if (tok[0] == ".rev" && tok.size() > 1) revision = tok[1];
            return;
        }
        std::map<std::string, std::string> kv;
        std::vector<std::string> pos;
        for (size_t i = 1; i < tok.size(); ++i) {
            auto eq = tok[i].find('=');
            if (eq != std::string::npos) kv[tok[i].substr(0, eq)] = tok[i].substr(eq + 1);
            else if (tok[i] == "dyn" || tok[i] == "novar") kv[tok[i]] = "1";
            else pos.push_back(tok[i]);
        }
        const std::string type = tok[0];
        ElemMeta meta;
        auto fillMeta = [&](const std::string& name) {
            meta.name = name;
            meta.evidence = kv.count("ev") ? kv["ev"] : "";
            meta.source = kv.count("src") ? kv["src"] : "";
            meta.note = kv.count("note") ? kv["note"] : "";
            for (auto& ch : meta.note) if (ch == '_') ch = ' ';
            meta.tol = kv.count("tol") ? parseValue(kv["tol"]) : 0.0;
            meta.dyn = kv.count("dyn") != 0;
            meta.novar = kv.count("novar") != 0;
        };
        auto need = [&](size_t n) { if (pos.size() < n) throw std::runtime_error("too few fields"); };

        if (type == "R") {
            need(4); fillMeta(pos[0]);
            Res r{meta, getNode(pos[1]), getNode(pos[2]), parseValue(pos[3]), parseValue(pos[3])};
            resMap_[r.m.name] = (int)res_.size(); res_.push_back(r);
        } else if (type == "C") {
            need(4); fillMeta(pos[0]);
            Cap c{}; c.m = meta; c.a = getNode(pos[1]); c.b = getNode(pos[2]); c.C = c.Cnom = parseValue(pos[3]);
            if (kv.count("th")) c.thetaSel = parseValue(kv["th"]);
            capMap_[c.m.name] = (int)cap_.size(); cap_.push_back(c);
        } else if (type == "L") {
            need(4); fillMeta(pos[0]);
            Ind l{}; l.m = meta; l.a = getNode(pos[1]); l.b = getNode(pos[2]); l.L = l.Lnom = parseValue(pos[3]);
            indMap_[l.m.name] = (int)ind_.size(); ind_.push_back(l);
        } else if (type == "TRIODE") {
            need(4); fillMeta(pos[0]);
            Tri t{}; t.m = meta; t.p = getNode(pos[1]); t.g = getNode(pos[2]); t.k = getNode(pos[3]);
            t.type = kv.count("type") ? kv["type"] : "12AX7"; t.par = triodeParams(t.type);
            tri_.push_back(t);
        } else if (type == "PENTODE") {
            need(5); fillMeta(pos[0]);
            Pen p{}; p.m = meta; p.p = getNode(pos[1]); p.g = getNode(pos[2]); p.s = getNode(pos[3]); p.k = getNode(pos[4]);
            p.par = pentodeParams();
            pen_.push_back(p);
        } else if (type == "DIODE") {
            need(3); fillMeta(pos[0]);
            Dio d{}; d.m = meta; d.a = getNode(pos[1]); d.k = getNode(pos[2]);
            dio_.push_back(d);
        } else if (type == "XFMR") {
            need(1); fillMeta(pos[0]);
            auto g = [&](const char* k) { if (!kv.count(k)) throw std::runtime_error(std::string("XFMR missing ") + k); return kv[k]; };
            Xfm x{}; x.m = meta;
            x.n1[0] = getNode(g("a1")); x.n2[0] = getNode(g("a2"));
            x.n1[1] = getNode(g("b1")); x.n2[1] = getNode(g("b2"));
            x.n1[2] = getNode(g("s1")); x.n2[2] = getNode(g("s2"));
            double Nh = parseValue(g("Nh")), Ns = parseValue(g("Ns"));
            x.N[0] = Nh; x.N[1] = Nh; x.N[2] = Ns;
            x.R[0] = parseValue(g("Ra")); x.R[1] = parseValue(g("Rb")); x.R[2] = parseValue(g("Rs"));
            x.Ll[0] = parseValue(g("Lla")); x.Ll[1] = parseValue(g("Llb")); x.Ll[2] = parseValue(g("Lls"));
            x.P0 = parseValue(g("P0")); x.phiSat = parseValue(g("phisat")); x.Pair = parseValue(g("Pair"));
            x.satOn = kv.count("sat") && parseValue(kv["sat"]) != 0.0;     // optional key; absent = linear core
            xfm_ = x; hasXfm_ = true;
        } else {
            throw std::runtime_error("unknown element type " + type);
        }
    }

    // ------------------------------------------------------------------ assembly
    void addLin(int r, int c, double x)
    {
        if (r < 0 || c < 0) return;
        const int s = lu_.slot(r, c);
        linVals_[s] += x;
        linFlag_[s] = 1;
    }
    void stamp2(int a, int b, double g) { addLin(a, a, g); addLin(b, b, g); addLin(a, b, -g); addLin(b, a, -g); }

    void assembleLinearOne(bool dc)
    {
        std::fill(linVals_.begin(), linVals_.end(), 0.0);
        std::fill(linFlag_.begin(), linFlag_.end(), 0);
        for (int i = 0; i < N_; ++i) addLin(i, i, 1e-12);                   // GMIN: keeps floating nodes defined
        for (auto& r : res_) stamp2(r.a, r.b, 1.0 / (r.R * r.var));
        for (auto& c : cap_) {
            c.g = dc ? 0.0 : (c.C * c.var) / (c.th * hAct_);
            if (c.g != 0.0) stamp2(c.a, c.b, c.g);
        }
        for (auto& l : ind_) {
            l.g = dc ? 1e4 : theta_ * hAct_ / (l.L * l.var);
            stamp2(l.a, l.b, l.g);
        }
        if (hasXfm_) {
            for (int k = 0; k < 3; ++k) {
                const int rk = xfm_.row[k];
                addLin(xfm_.n1[k], rk, 1.0);  addLin(xfm_.n2[k], rk, -1.0);
                addLin(rk, xfm_.n1[k], -1.0); addLin(rk, xfm_.n2[k], 1.0);
                const double diag = xfm_.R[k] * xfm_.Rvar[k] + (dc ? 0.0 : xfm_.Ll[k] / (theta_ * hAct_));
                addLin(rk, rk, diag);
            }
        }
    }

    void assembleLinear(bool dc)
    {
        if (!built_) return;
        const int nv = dc ? 1 : kDivs;
        for (int v = 0; v < nv; ++v) {
            hAct_ = h_ / double(1 << v);
            assembleLinearOne(dc);
            linCache_[v] = linVals_;
        }
        hAct_ = h_; divIdx_ = 0;
        linVals_ = linCache_[0];
        // list of linear entries for the residual matvec (identical for all divisors)
        linList_.clear();
        for (int s = 0; s < nnz_; ++s) if (linFlag_[s]) linList_.push_back({slotRow_[s], slotCol_[s], s});
        dcMode_ = dc;
    }

    void setDivisor(int idx)
    {
        divIdx_ = idx;
        hAct_ = h_ / double(1 << idx);
        linVals_ = linCache_[idx];
    }

    void buildRhs()
    {
        std::fill(b_.begin(), b_.end(), 0.0);
        for (int i = 0; i < nn_; ++i) b_[i] += inj_[i];
        for (auto& c : cap_) {
            c.g = (c.C * c.var) / (c.th * hAct_);
            c.ih = -c.g * c.vprev - c.cth * c.iprev;
            if (c.a >= 0) b_[c.a] -= c.ih;
            if (c.b >= 0) b_[c.b] += c.ih;
        }
        for (auto& l : ind_) {
            l.g = theta_ * hAct_ / (l.L * l.var);
            l.ih = l.iprev + (1.0 - theta_) * hAct_ / (l.L * l.var) * l.vprev;
            if (l.a >= 0) b_[l.a] -= l.ih;
            if (l.b >= 0) b_[l.b] += l.ih;
        }
        if (hasXfm_) {
            for (int k = 0; k < 3; ++k)
                b_[xfm_.row[k]] += xfm_.lamPrev[k] / (theta_ * hAct_) + cth_ * xfm_.lamDotPrev[k];
        }
    }

    // Evaluate residual F(z) and (optionally) assemble the Jacobian into vals_.
    void evalSystem(bool dc, bool jac)
    {
        // linear part
        for (int i = 0; i < N_; ++i) F_[i] = -b_[i];
        for (const auto& e : linList_) F_[e.r] += linVals_[e.s] * z_[e.c];
        if (jac) std::copy(linVals_.begin(), linVals_.end(), vals_.begin());
        auto add = [&](int s, double x) { if (s >= 0) vals_[s] += x; };

        for (auto& t : tri_) {
            const double vgk = V(t.g) - V(t.k), vpk = V(t.p) - V(t.k);
            TriodeOut o;
            evalTriode(t.par, vgk, vpk, emission_[t.emGroup] * t.scale, o);
            t.ip = o.ip; t.ig = o.ig;
            if (t.p >= 0) F_[t.p] += o.ip;
            if (t.g >= 0) F_[t.g] += o.ig;
            if (t.k >= 0) F_[t.k] -= o.ip + o.ig;
            if (jac) {
                const double gp = o.dip_dvpk, gg = o.dip_dvgk, gi = o.dig_dvgk;
                // rows P,G,K ; cols P,G,K  (index = row*3+col)
                add(t.s[0], gp);            add(t.s[1], gg);             add(t.s[2], -(gg + gp));
                add(t.s[4], gi);            add(t.s[5], -gi);
                add(t.s[6], -gp);           add(t.s[7], -(gg + gi));     add(t.s[8], gg + gp + gi);
            }
        }
        for (auto& q : pen_) {
            const double vgk = V(q.g) - V(q.k), vpk = V(q.p) - V(q.k), vsk = V(q.s) - V(q.k);
            PentodeOut o;
            evalPentode(q.par, vgk, vpk, vsk, emission_[q.emGroup] * q.scale, o);
            q.ip = o.ip; q.is = o.is; q.ig = o.ig;
            if (q.p >= 0) F_[q.p] += o.ip;
            if (q.s >= 0) F_[q.s] += o.is;
            if (q.g >= 0) F_[q.g] += o.ig;
            if (q.k >= 0) F_[q.k] -= o.ip + o.is + o.ig;
            if (jac) {
                // node order: 0=P 1=G 2=S 3=K ; derivative wrt node voltage of current i with partials (vgk,vpk,vsk)
                const double ddip[4] = {o.dip[1], o.dip[0], o.dip[2], -(o.dip[0] + o.dip[1] + o.dip[2])};
                const double ddis[4] = {o.dis[1], o.dis[0], o.dis[2], -(o.dis[0] + o.dis[1] + o.dis[2])};
                const double ddig[4] = {0.0, o.dig_dvgk, 0.0, -o.dig_dvgk};
                for (int c = 0; c < 4; ++c) {
                    add(q.sl[0 * 4 + c], ddip[c]);
                    add(q.sl[2 * 4 + c], ddis[c]);
                    add(q.sl[1 * 4 + c], ddig[c]);
                    add(q.sl[3 * 4 + c], -(ddip[c] + ddis[c] + ddig[c]));
                }
            }
        }
        for (auto& d : dio_) {
            const double v = V(d.a) - V(d.k);
            double i, di;
            evalDiode(d.par, v, emission_[d.emGroup], i, di);
            d.i = i;
            if (d.a >= 0) F_[d.a] += i;
            if (d.k >= 0) F_[d.k] -= i;
            if (jac) { add(d.s[0], di); add(d.s[1], -di); add(d.s[2], -di); add(d.s[3], di); }
        }
        if (hasXfm_) {
            double Fm = 0.0;
            for (int k = 0; k < 3; ++k) Fm += xfm_.N[k] * z_[xfm_.row[k]];
            if (!dc) {
                const double phi = corePhi(Fm);
                lastPhi_ = phi;
                const double dphi = corePhiPrime(Fm);
                const double c = 1.0 / (theta_ * hAct_);
                for (int k = 0; k < 3; ++k) {
                    F_[xfm_.row[k]] += xfm_.N[k] * phi * c;
                    if (jac)
                        for (int j = 0; j < 3; ++j) add(xfm_.slotBlock[k][j], c * xfm_.N[k] * dphi * xfm_.N[j]);
                }
            }
        }
    }

    // weighted residual merit: node KCL rows in mA, winding rows in volts
    double merit() const
    {
        double m = 0.0;
        for (int i = 0; i < nn_; ++i) { const double f = F_[i] * 1e3; m += f * f; }
        for (int i = nn_; i < N_; ++i) m += F_[i] * F_[i];
        return m;
    }

    // Damped Newton with a lazy backtracking line search on the residual merit (globalisation).
    // The line search costs nothing when the full step reduces the residual (the normal case); a
    // loose step cap remains only as an emergency stop.
    bool newton(bool dc, int maxIt)
    {
        lastIterations = 0;
        double meritPrev = 0.0, etaPrev = 1.0;
        bool havePrev = false;
        for (int it = 0; it < maxIt; ++it) {
            auto T0 = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            evalSystem(dc, true);
            auto T1 = profile ? std::chrono::steady_clock::now() : T0;
            const double mer = merit();
            if (havePrev && mer > meritPrev * (1.0 - 1e-4 * etaPrev) && etaPrev > 2e-3) {
                // the last step did not reduce the residual: backtrack along it
                etaPrev *= 0.5;
                for (int i = 0; i < N_; ++i) z_[i] = zOld_[i] + etaPrev * dzOld_[i];
                ++lastIterations; ++stats.iterations;
                continue;
            }
            for (int i = 0; i < N_; ++i) dz_[i] = -F_[i];
            stats.guardedPivots += lu_.factor(vals_.data());
            auto T2 = profile ? std::chrono::steady_clock::now() : T0;
            lu_.solve(vals_.data(), dz_.data());
            if (profile) {
                auto T3 = std::chrono::steady_clock::now();
                tEval += std::chrono::duration<double>(T1 - T0).count();
                tFactor += std::chrono::duration<double>(T2 - T1).count();
                tSolve += std::chrono::duration<double>(T3 - T2).count();
            }
            // emergency step cap (loose) -- the line search is the real globalisation
            double eta = 1.0, mx = 0.0;
            for (int i = 0; i < nn_; ++i) mx = std::max(mx, std::fabs(dz_[i]));
            if (mx > stepLimitV) eta = stepLimitV / mx;
            for (auto& t : tri_) {
                const double dg = std::fabs(dzv(t.g) - dzv(t.k));
                if (dg > gridStepLimit) eta = std::min(eta, gridStepLimit / dg);
            }
            for (auto& q : pen_) {
                const double dg = std::fabs(dzv(q.g) - dzv(q.k));
                if (dg > gridStepLimit) eta = std::min(eta, gridStepLimit / dg);
            }
            bool conv = (eta == 1.0);
            for (int i = 0; i < N_; ++i) {
                zOld_[i] = z_[i];
                const double tol = (i < nn_ ? absTolV : absTolV * 1e-3) + relTol * std::fabs(z_[i]);
                if (std::fabs(dz_[i]) > tol) conv = false;
            }
            dzOld_ = dz_;
            for (int i = 0; i < N_; ++i) z_[i] += eta * dz_[i];
            meritPrev = mer; etaPrev = eta; havePrev = true;
            if (traceCb) { double m = 0; for (int i = 0; i < nn_; ++i) m = std::max(m, std::fabs(dz_[i])); traceCb(*this, it, m, eta); }
            ++lastIterations; ++stats.iterations;
            if ((uint64_t)lastIterations > stats.maxIterSeen) stats.maxIterSeen = lastIterations;
            if (conv) {
                // step test passed: also require a small nodal residual.  The pre-step residual is already in F_; the post-step
                // one costs one extra evaluation and is taken only when the pre-step value is not conclusive (or when tracked).
                double r = 0; for (int i = 0; i < nn_; ++i) r = std::max(r, std::fabs(F_[i]));
                if (r > resTolA || trackResidual) {
                    evalSystem(dc, false);
                    r = 0; for (int i = 0; i < nn_; ++i) r = std::max(r, std::fabs(F_[i]));
                }
                if (r <= resTolA) {
                    if (trackResidual) { lastResidual = r; stats.maxResidual = std::max(stats.maxResidual, r); }
                    return true;
                }
            }
        }
        return false;
    }

    double dzv(int n) const { return n < 0 ? 0.0 : dz_[n]; }

    void commit()
    {
        for (auto& c : cap_) {
            const double v = V(c.a) - V(c.b);
            c.iprev = c.g * v + c.ih;
            c.vprev = v;
        }
        for (auto& l : ind_) {
            const double v = V(l.a) - V(l.b);
            l.iprev = l.g * v + l.ih;
            l.vprev = v;
        }
        if (hasXfm_) {
            double Fm = 0.0;
            for (int k = 0; k < 3; ++k) { xfm_.i[k] = z_[xfm_.row[k]]; Fm += xfm_.N[k] * xfm_.i[k]; }
            const double phi = corePhi(Fm);
            for (int k = 0; k < 3; ++k) {
                xfm_.lamPrev[k] = xfm_.Ll[k] * xfm_.i[k] + xfm_.N[k] * phi;
                xfm_.lamDotPrev[k] = (V(xfm_.n1[k]) - V(xfm_.n2[k])) - xfm_.R[k] * xfm_.Rvar[k] * xfm_.i[k];
            }
        }
        zprev3_ = zprev2_;
        zprev2_ = zprev_;
        zprev_ = z_;
    }

public:
    std::string title, revision;

private:
    struct LinEntry { int r, c, s; };
    std::vector<Res> res_; std::vector<Cap> cap_; std::vector<Ind> ind_;
    std::vector<Tri> tri_; std::vector<Pen> pen_; std::vector<Dio> dio_;
    Xfm xfm_{}; bool hasXfm_ = false;
    std::map<std::string, int> nodeMap_, resMap_, capMap_, indMap_;
    std::vector<std::string> nodeNames_;
    int nn_ = 0, N_ = 0, nnz_ = 0;
    double h_ = 1e-5, hAct_ = 1e-5, theta_ = 0.5, cth_ = 1.0;
    static constexpr int kDivs = 6;                       // step divisors 1,2,4,8,16,32
    std::vector<double> linCache_[kDivs];
    int divIdx_ = 0;
    std::vector<double> injOld_, injNew_;
    Snapshot snap_, xfer_;
    bool built_ = false, dcMode_ = false, dirty_ = false;
    SparseLU lu_;
    std::vector<double> linVals_, vals_, z_, zprev_, zprev2_, zprev3_, b_, F_, dz_, inj_, zOld_, dzOld_;
    std::vector<char> linFlag_;
    std::vector<int> slotRow_, slotCol_, diagSlot_;
    std::vector<LinEntry> linList_;
    double emission_[3] = {1.0, 1.0, 1.0};
    double lastPhi_ = 0.0;
};

} // namespace cd57
