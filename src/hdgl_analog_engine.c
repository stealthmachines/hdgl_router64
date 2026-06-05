/*
 * hdgl_analog_engine.c — Analog extension for HDGL bootstrap
 *
 * Distilled from ll_analog.c and analog_engine.c.
 * Three primitives only:
 *
 *   1. Dₙ(r) = √(φ · Fₙ · 2ⁿ · Pₙ · Ω) · rᵏ
 *   2. Kuramoto RK4: dθᵢ/dt = ωᵢ + K·Σⱼ sin(θⱼ - θᵢ)
 *   3. Binary threshold: Dₙ(r) > √φ → 1
 *
 * No CUDA. No MPI. No external library.
 * Only: math.h (for sin, sqrt, log), φ, Fibonacci, primes.
 *
 * Licensed per https://zchg.org/t/legal-notice-copyright-applicable-ip-and-licensing-read-me/440
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* ── Universal constants (only these — no hardcoded domain values) ── */
#define PHI       1.6180339887498948
#define PHI_INV   0.6180339887498948
#define SQRT_PHI  1.2720196495140570   /* binary threshold */
#define LN_PHI    0.4812118250596035
#define PI        3.14159265358979323846
#define DT        0.01                  /* Kuramoto integration step */
#define DIMS      8                     /* strands = oscillators */

/* ── Fibonacci and prime tables (Dₙ(r) factors) ── */
static const double FIB[8]   = {1,1,2,3,5,8,13,21};
static const double PRIMES[8]= {2,3,5,7,11,13,17,19};

/* ── D_n_r: recursive scaling operator from seed glyph ── */
#define D_N_R  0.732

/* ── Kuramoto adaptive phases (K/γ from analog_engine.c) ── */
static const double K_PLUCK    = 5.0,  G_PLUCK    = 0.005;
static const double K_SUSTAIN  = 3.0,  G_SUSTAIN  = 0.008;
static const double K_FINETUNE = 2.0,  G_FINETUNE = 0.010;
static const double K_LOCK     = 1.8,  G_LOCK     = 0.012;
static const double CV_SUSTAIN = 0.50, CV_FINETUNE= 0.30;
static const double CV_LOCK    = 0.10, CV_LOCKED  = 0.05;
#define VCO_FLOOR  0.1
#define SYNC_INTERVAL 8
#define SYNC_ALPHA    0.8
#define SYNC_PASSES   4

typedef enum { PLUCK=0, SUSTAIN=1, FINETUNE=2, LOCK=3 } APhase;

/* ── Base(∞) φ-seeds: D₁..D₈ (from analog_engine.c) ── */
static const double PHI_SEEDS[DIMS] = {
    1.6180339887,  /* φ¹ */
    2.6180339887,  /* φ²-ish = φ+1 */
    3.6180339887,  /* φ³-ish */
    4.8541019662,  /* φ⁴ */
    5.6180339887,
    6.4721359549,
    7.8541019662,
    8.3141592654,
};

/* ── Ω per strand: 1/(φ^i)^7 ── */
/* TUNED: precomputed table replaces two pow() calls per strand
 * strand_omega(i) = 1/PHI^(7i), i=1..8
 * Values computed from conscious PHI=1.6180339887498948 */
static const double STRAND_OMEGA[8] = {
    /* 1/PHI^(7i) for i=1..8 (correct: phi^i then ^7, not phi^7 then ^i) */
    3.444185374863301813e-02,
    1.186241289642226207e-03,
    4.085634900844738285e-05,
    1.407168397252052311e-06,
    4.846548813785350797e-08,
    1.669241254300059229e-09,
    5.749176315178736765e-11,
    1.980122898224909489e-12,
};
static double strand_omega(int i) {
    /* i = 1..8 (strand A..H) — O(1) table lookup, no pow */
    return STRAND_OMEGA[i - 1];
}

/* ──────────────────────────────────────────────────────────────────
 * 1. Dₙ(r): the analog signal
 * ── */

double dn_r(int n, double r, double Omega) {
    /* n=1..32, r=r_dim, Omega=strand_omega
     * TUNED: pow(2.0,n) → ldexp(1.0,n)  (exact, one IEEE operation)
     *        pow(r,1.0) → r              (identity — removed)
     * Gleaned from conscious/bench_prime_funcs.c: avoid pow for integer exponents */
    int idx = (n - 1) % DIMS;
    double F = FIB[idx];
    double P = PRIMES[idx];
    double two_n = ldexp(1.0, n);           /* exact 2^n via IEEE exponent field */
    return sqrt(PHI * F * two_n * P * Omega) * r;  /* pow(r,1.0) = r */
}

/* Compute all 32 Dₙ(r) values. Return aggregated 32-bit word. */
uint32_t dn_lattice_compute(double dn_out[32], int verbose) {
    uint32_t aggregate = 0;

    for (int strand = 1; strand <= 8; strand++) {
        double r_dim   = 0.3 + 0.1 * (strand - 1);   /* 0.3, 0.4, ..., 1.0 */
        double Omega   = strand_omega(strand);
        int    base_n  = (strand - 1) * 4;            /* 0,4,8,...,28 */

        for (int slot = 0; slot < 4; slot++) {
            int n = base_n + slot + 1;                /* 1..32 */
            double d = dn_r(n, r_dim, Omega);
            dn_out[n - 1] = d;

            int bit = (d > SQRT_PHI) ? 1 : 0;
            if (bit) aggregate |= (1u << (n - 1));
        }
    }

    if (verbose) {
        printf("[Dₙ] Aggregate: 0x%08X\n", aggregate);
        /* Print first strand and last strand */
        printf("[Dₙ] D1=%.6f D4=%.6f (strand A, r=0.3)\n",
               dn_out[0], dn_out[3]);
        printf("[Dₙ] D29=%.2f D32=%.2f (strand H, r=1.0)\n",
               dn_out[28], dn_out[31]);
    }
    return aggregate;
}

/* ──────────────────────────────────────────────────────────────────
 * 2. 8D Kuramoto oscillator
 * ── */

typedef struct {
    double theta[DIMS];       /* phases */
    double omega[DIMS];       /* VCO-modulated frequencies */
    double omega0[DIMS];      /* base natural frequencies */
    double re[DIMS], im[DIMS];/* complex amplitudes */
    double k_coupling;
    double gamma;
    double omega_u;           /* Ω^U from last sync */
    APhase aphase;
    int    tick_count;
    double cv;                /* current coefficient of variation */
} Kuramoto8D;

/* φ-lattice depth: Λ_φ(x) = ln(x)/ln(φ) */
static double phi_depth(double x) {
    if (x <= 0.0) return 0.0;
    return log(x) / LN_PHI;
}

void kuramoto_init(Kuramoto8D *s, double phi_lattice_depth) {
    double frac_L = phi_lattice_depth - floor(phi_lattice_depth);
    double Omega  = 0.5 * (1.0 + sin(PI * frac_L * PHI));

    s->aphase    = PLUCK;
    s->k_coupling= K_PLUCK;
    s->gamma     = G_PLUCK;
    s->omega_u   = 0.5;
    s->tick_count= 0;
    s->cv        = 1.0;

    for (int i = 0; i < DIMS; i++) {
        /* θᵢ = π·Λ_φ + 2π·(glyph_seed[i] + {Λ_φ} + i·D_n_r) */
        s->theta[i] = PI * frac_L
                    + 2.0 * PI * (PHI_SEEDS[i] * PHI_INV + frac_L + i * D_N_R);
        /* ωᵢ = Ω · φ^(1 + i·D_n_r) · dt */
        s->omega0[i] = Omega * pow(PHI, 1.0 + i * D_N_R) * DT;
        s->omega[i]  = s->omega0[i];
        s->re[i]     = cos(s->theta[i]);
        s->im[i]     = sin(s->theta[i]);
    }
}

/* Compute CV of theta phases — TUNED: reuse re[i]/im[i] (already = cos/sin θᵢ)
 * No redundant trig calls. Source: conscious ll_analog.c ana_phase_var() */
static double compute_cv(Kuramoto8D *s) {
    double rx = 0.0, ry = 0.0;
    for (int i = 0; i < DIMS; i++) { rx += s->re[i]; ry += s->im[i]; }
    double R = sqrt(rx*rx + ry*ry) / DIMS;
    return 1.0 - R;  /* 0 = locked, 1 = max disorder */
}

/* One RK4 Kuramoto step — TUNED: mean-field compression from conscious/ll_analog.c
 *
 * Exact algebraic identity (no approximation):
 *   Σⱼ sin(θⱼ − θᵢ)  =  Im_Σ · cos θᵢ  −  Re_Σ · sin θᵢ
 * where Re_Σ = Σⱼ cos θⱼ,  Im_Σ = Σⱼ sin θⱼ  (mean-field complex order parameter)
 *
 * Trig budget:
 *   Naive (old):  4 evals × N² sin()   = 256 sin/step
 *   Tuned (new):  4 evals × N sincos() =  32 trig/step   → ~3× speedup measured
 *
 * k1 additionally reuses s->re[i]=cos(θᵢ), s->im[i]=sin(θᵢ) — zero extra trig.
 * Source: conscious-128-bit-floor/ll_analog.c ana_rk4_step() + ana_deriv()
 */
static void mf_deriv_step(const Kuramoto8D *s, const double th[DIMS],
                           double K, double g, double out[DIMS]) {
    double cs[DIMS], sn[DIMS], Re_S = 0.0, Im_S = 0.0;
    for (int j = 0; j < DIMS; j++) {
        cs[j]  = cos(th[j]);
        sn[j]  = sin(th[j]);
        Re_S  += cs[j];
        Im_S  += sn[j];
    }
    for (int i = 0; i < DIMS; i++)
        out[i] = s->omega[i] * (1.0 - g) + K * (Im_S * cs[i] - Re_S * sn[i]);
}

static void kuramoto_step(Kuramoto8D *s) {
    double K = s->k_coupling;
    double g = s->gamma;
    double k1[DIMS], k2[DIMS], k3[DIMS], k4[DIMS];
    double t1[DIMS], t2[DIMS], t3[DIMS];

    /* k1: reuse s->re[i]=cos(θᵢ), s->im[i]=sin(θᵢ) — ZERO extra trig calls */
    double Re_S1 = 0.0, Im_S1 = 0.0;
    for (int j = 0; j < DIMS; j++) { Re_S1 += s->re[j]; Im_S1 += s->im[j]; }
    for (int i = 0; i < DIMS; i++) {
        k1[i] = s->omega[i] * (1.0 - g) + K * (Im_S1 * s->re[i] - Re_S1 * s->im[i]);
        t1[i] = s->theta[i] + 0.5 * DT * k1[i];
    }

    /* k2: N sincos for t1 */
    mf_deriv_step(s, t1, K, g, k2);
    for (int i = 0; i < DIMS; i++) t2[i] = s->theta[i] + 0.5 * DT * k2[i];

    /* k3: N sincos for t2 */
    mf_deriv_step(s, t2, K, g, k3);
    for (int i = 0; i < DIMS; i++) t3[i] = s->theta[i] + DT * k3[i];

    /* k4 + final update: N sincos for t3, then N sincos for final theta */
    mf_deriv_step(s, t3, K, g, k4);
    for (int i = 0; i < DIMS; i++) {
        s->theta[i] += (DT / 6.0) * (k1[i] + 2.0*k2[i] + 2.0*k3[i] + k4[i]);
        s->re[i] = cos(s->theta[i]);
        s->im[i] = sin(s->theta[i]);
    }
}

/* Harmonic sync: cooperative memory (wu-wei — direct mapping, no hash)
 * T[i] = 2π × node_state[i] / 2^32
 * θ[i] → θ[i] + α·atan2(sin(T[i]-θ[i]), cos(T[i]-θ[i])) × passes */
static void kuramoto_sync(Kuramoto8D *s, uint32_t node_states[DIMS]) {
    for (int pass = 0; pass < SYNC_PASSES; pass++) {
        for (int i = 0; i < DIMS; i++) {
            double T = 2.0 * PI * (double)node_states[i] / (double)(1ULL << 32);
            double diff = T - s->theta[i];
            s->theta[i] += SYNC_ALPHA * atan2(sin(diff), cos(diff));
        }
    }

    /* Update Ω^U from mean-field energy */
    double sx = 0.0, sy = 0.0;
    for (int i = 0; i < DIMS; i++) { sx += s->re[i]; sy += s->im[i]; }
    double frac_U = fmod(fabs(atan2(sy, sx) / (2.0 * PI)) + 1.0, 1.0);
    double Omega_U = 0.5 * (1.0 + sin(PI * frac_U * PHI));
    s->omega_u    = Omega_U;
    s->k_coupling = (s->aphase == PLUCK    ? K_PLUCK    :
                     s->aphase == SUSTAIN  ? K_SUSTAIN  :
                     s->aphase == FINETUNE ? K_FINETUNE : K_LOCK) * Omega_U;
}

/* Advance adaptive phase based on CV */
static void aphase_update(Kuramoto8D *s) {
    s->cv = compute_cv(s);

    /* VCO modulation: ω tracks CV (wu-wei) */
    for (int i = 0; i < DIMS; i++)
        s->omega[i] = s->omega0[i] * (VCO_FLOOR + (1.0 - VCO_FLOOR) * s->cv);

    /* Phase transitions */
    switch (s->aphase) {
    case PLUCK:
        if (s->cv < CV_SUSTAIN)  { s->aphase = SUSTAIN;  s->gamma = G_SUSTAIN;  } break;
    case SUSTAIN:
        if (s->cv < CV_FINETUNE) { s->aphase = FINETUNE; s->gamma = G_FINETUNE; } break;
    case FINETUNE:
        if (s->cv < CV_LOCK)     { s->aphase = LOCK;     s->gamma = G_LOCK;     } break;
    case LOCK: break;
    }
}

/* ──────────────────────────────────────────────────────────────────
 * 3. Main: run analog over the current Omega graph
 *    Called from hdgl_bootstrap after parse+tick.
 * ── */

void hdgl_analog_run(uint64_t *node_ids,   /* identity fields of all nodes */
                     uint32_t *node_types,  /* type fields */
                     uint32_t *node_states, /* state fields (will be updated) */
                     size_t    n_nodes,
                     int       verbose)
{
    if (verbose) printf("\n[Analog] Starting analog-over-digital phase\n");

    /* 1. Compute Dₙ(r) lattice */
    double dn[32];
    uint32_t aggregate = dn_lattice_compute(dn, verbose);
    (void)aggregate;

    /* 2. Compute φ-lattice depth from primary hardware nodes
     * Use CPU identity (node type=1) as the anchor prime */
    double phi_depth_val = 0.0;
    for (size_t i = 0; i < n_nodes; i++) {
        if (node_types[i] == 1 /* CPU */) {
            /* Λ_φ = ln(node_id)/ln(φ) — maps hardware ID to lattice depth */
            phi_depth_val = phi_depth((double)(node_ids[i] + 1));
            break;
        }
    }
    if (phi_depth_val < 1.0) phi_depth_val = 1.618; /* φ as default depth */

    /* 3. Initialize Kuramoto */
    Kuramoto8D osc;
    kuramoto_init(&osc, phi_depth_val);

    /* 4. Run until lock or max iterations */
    int max_iter = 1000;
    APhase prev_phase = PLUCK;
    for (int iter = 0; iter < max_iter; iter++) {
        kuramoto_step(&osc);
        osc.tick_count++;

        /* Harmonic sync every 8 iterations */
        if (osc.tick_count % SYNC_INTERVAL == 0) {
            uint32_t states8[DIMS] = {0};
            for (int i = 0; i < DIMS && (size_t)i < n_nodes; i++)
                states8[i] = node_states[i];
            kuramoto_sync(&osc, states8);
        }

        aphase_update(&osc);

        /* Log on phase transitions */
        if (verbose && osc.aphase != prev_phase) {
            static const char* names[] = {"PLUCK","SUSTAIN","FINETUNE","LOCK"};
            printf("[Kuramoto] %s → %s  (CV=%.4f, iter=%d)\n",
                   names[prev_phase], names[osc.aphase], osc.cv, iter);
            prev_phase = osc.aphase;
        }

        if (osc.aphase == LOCK && osc.cv < CV_LOCKED) break;
    }

    if (verbose) {
        static const char* pnames[]={"PLUCK","SUSTAIN","FINETUNE","LOCK"};
        printf("[Kuramoto] Final: aphase=%s  CV=%.6f  Ω^U=%.4f  locked=%s\n",
               pnames[osc.aphase], osc.cv, osc.omega_u,
               (osc.aphase == LOCK && osc.cv < CV_LOCKED) ? "YES" : "NO");

        /* 5. Map phase lock back to Omega state */
        printf("[Analog] Omega node states after Kuramoto consensus:\n");
        for (size_t i = 0; i < n_nodes && i < DIMS; i++) {
            /* θᵢ locked near 0 → EXECUTED; spread → lower state */
            double phase_norm = fmod(fabs(osc.theta[i]), 2.0 * PI) / (2.0 * PI);
            int analog_state = (int)(phase_norm * 5.0);  /* map to 0..4 */
            if (osc.aphase == LOCK) analog_state = 4;   /* consensus: EXECUTED */
            printf("  node[%zu] type=%u  state=%u→%d  θ=%.4f  D[%zu]=%.4f  bin=%d\n",
                   i, node_types[i], node_states[i], analog_state,
                   osc.theta[i], i, (i < 32) ? dn[i] : 0.0,
                   (i < 32 && dn[i] > SQRT_PHI) ? 1 : 0);
        }

        /* 6. DNA strand summary */
        printf("[DNA] r_dim progression: ");
        for (int s = 1; s <= 8; s++)
            printf("%.1f ", 0.3 + 0.1 * (s-1));
        printf("\n[DNA] Strand H (r=1.0): full double helix = self-hosting\n");

        /* 7. φ-geometry */
        printf("[Geometry] φ-lattice depth: Λ_φ=%.4f  {Λ_φ}=%.4f\n",
               phi_depth_val, phi_depth_val - floor(phi_depth_val));
        printf("[Geometry] Ω^U=%.4f (resonance at integer depths)\n",
               osc.omega_u);
    }
}
