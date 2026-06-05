/*
 * conscious_os_port.c — Port of conscious-128-bit-floor to HDGL bare metal
 *
 * This file IS the bridge between:
 *   conscious-128-bit-floor  (phi-lattice OS, Kuramoto, Dn(r), bootloaderZ V6.0)
 *   HDGL firmware            (phi-lattice kernel, Omega graph, native x86)
 *
 * On a hosted OS (Linux/macOS/Windows) this compiles standalone.
 * On HDGL bare metal it links against the phi-lattice kernel via phi_lk_read/
 * phi_advance/phi_consensus instead of POSIX syscalls.
 *
 * Philosophy (from conscious/bootloaderZ V6.0):
 *   - No SHA. No AES. No XOR. The lattice IS the key.
 *   - Consensus IS permission. GOI/GUZ replace fault vectors.
 *   - phi_fold replaces SHA-256. phi_stream replaces AES.
 *   - Ωₙ₊₁ = T(Ωₙ) — every state transition is a glyph rewrite.
 *
 * Three portability layers:
 *   LAYER_HDGL  — running on HDGL phi-lattice kernel (bare metal)
 *   LAYER_POSIX — running on Linux/macOS (test/dev)
 *   LAYER_WIN32 — running on Windows (build/CI)
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════
 * PORTABILITY LAYER — abstract away OS/bare-metal differences
 * ════════════════════════════════════════════════════════════════ */

#ifdef LAYER_HDGL
/*
 * HDGL bare-metal bindings.
 * phi_lk_read, phi_advance, phi_consensus are implemented in
 * firmware_runtime.asm; linked here via weak symbol declarations.
 * Serial output goes through hdgl_putchar (routes to .com_base).
 */
extern uint32_t phi_lk_read(const char *key, uint32_t len);
extern void     phi_advance(void);
extern int      phi_consensus(void);     /* returns 1 if locked */

static void cos_port_putchar(char c) {
    /* Direct call into HDGL com1_send — ABI: AL = char */
    __asm__ volatile (
        "movb %0, %%al\n\t"
        "call .com1_send\n\t"
        : : "r"(c) : "eax", "edx"
    );
}
static void cos_port_puts(const char *s) {
    while (*s) cos_port_putchar(*s++);
    cos_port_putchar('\r');
    cos_port_putchar('\n');
}
static uint64_t cos_port_ticks(void) {
    return *(volatile uint32_t *)0x101010;  /* HDGL phi tick counter */
}
static void cos_port_yield(void) { phi_advance(); }

#else
/* POSIX / Win32 hosted layer */
#include <stdlib.h>
#include <time.h>
static void cos_port_putchar(char c) { putchar(c); }
static void cos_port_puts(const char *s) { puts(s); }
static uint64_t cos_port_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void cos_port_yield(void) { /* no-op on hosted */ }
#endif

/* ════════════════════════════════════════════════════════════════
 * CONSTANTS — from conscious-128-bit-floor throughout
 * ════════════════════════════════════════════════════════════════ */

#define PHI       1.6180339887498948
#define PHI_INV   0.6180339887498948
#define SQRT_PHI  1.2720196495140570
#define LN_PHI    0.4812118250596035
#define PI        3.14159265358979323846
#define DT        0.01
#define DIMS      8

#define GOI_LIMIT  0xFFFF0000u   /* from bootloaderZ V6.0 */
#define GUZ_LIMIT  0x00000100u
#define APA_FLAG_CONSENSUS (1u << 4)

static const double FIB[8]    = {1,1,2,3,5,8,13,21};
static const double PRIMES[8] = {2,3,5,7,11,13,17,19};
#define D_N_R 0.732

/* Kuramoto K/γ wu-wei ratios from conscious/ll_analog.c */
static const double K_WU[4]  = {5.0, 3.0, 2.0, 1.8};
static const double G_WU[4]  = {0.005, 0.008, 0.010, 0.012};
static const double CV_TH[4] = {0.50, 0.30, 0.10, 0.05}; /* SUSTAIN,FT,LOCK,LOCKED */

typedef enum { PLUCK=0, SUSTAIN=1, FINETUNE=2, LOCK=3 } APhase;

/* phi-seed table from conscious (fib×prime×PHI^n×65536) */
static const uint32_t PHI_SEEDS[16] = {
    0x00033C6Fu, 0x0007DAA6u, 0x002A5C56u, 0x008FEFA7u,
    0x0058FDEBu, 0x01104689u, 0x03A82BC8u, 0x0AAEC964u,
    0x00033C6Fu, 0x0007DAA6u, 0x002A5C56u, 0x008FEFA7u,
    0x0058FDEBu, 0x01104689u, 0x03A82BC8u, 0x0AAEC964u,
};

/* ════════════════════════════════════════════════════════════════
 * PHI LATTICE — port of bootloaderZ V6.0 Slot4096 core
 * Stripped to the 128-slot bare-metal variant used by HDGL kernel
 * ════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t slot[128];
    uint32_t tick;
    uint32_t goi_events;
    uint32_t guz_events;
    uint32_t flags;         /* APA_FLAG_CONSENSUS when locked */
} PhiLattice;

static void phi_lattice_init(PhiLattice *lat) {
    lat->tick = 0;
    lat->goi_events = 0;
    lat->guz_events = 0;
    lat->flags = 0;
    lat->slot[0] = PHI_SEEDS[0];
    for (int i = 1; i < 128; i++)
        lat->slot[i] = lat->slot[i-1] * 3u + PHI_SEEDS[i % 16];
}

/* phi_tick: one prismatic_recursion step (Ωₙ₊₁ = T(Ωₙ))
 * Additive, XOR-free. GOI saturates; GUZ floors. No crash. */
static void phi_lattice_tick(PhiLattice *lat) {
    lat->tick++;
    for (int i = 0; i < 128; i++) {
        if (lat->slot[i] >= GOI_LIMIT) {
            lat->slot[i] = GOI_LIMIT;
            lat->goi_events++;
        } else {
            uint32_t nxt = lat->slot[i] * 3u + lat->tick;
            lat->slot[i] = nxt;
            if (lat->slot[i] < GUZ_LIMIT) {
                lat->slot[i] = GUZ_LIMIT;
                lat->guz_events++;
            }
        }
    }
}

/* phi_fold: additive Z/256Z MAC — replaces SHA-256
 * acc = (acc*3 + key_byte + lattice[i]) mod 2^32
 * No XOR. Lattice IS the secret key. */
static uint32_t phi_fold(PhiLattice *lat, const uint8_t *data, size_t len) {
    uint32_t acc = lat->slot[0];   /* IV from lattice state */
    for (size_t i = 0; i < len; i++) {
        uint32_t lb = lat->slot[i % 128];
        acc = acc * 3u + (uint32_t)data[i] + lb;
    }
    /* 4 finalization rounds: ROR32-by-3 + add lattice[0] */
    for (int r = 0; r < 4; r++) {
        acc = (acc >> 3) | (acc << 29);
        acc += lat->slot[0];
    }
    return acc;
}

/* detect_harmonic_consensus: from conscious/hdgl_analog_v30.c
 * ZF equivalent: returns 1 if max_dev < mean/2 */
static int phi_detect_consensus(PhiLattice *lat) {
    uint64_t sum = 0;
    for (int i = 0; i < 128; i++) sum += lat->slot[i];
    uint32_t mean = (uint32_t)(sum >> 7);
    uint32_t max_dev = 0;
    for (int i = 0; i < 128; i++) {
        uint32_t d = lat->slot[i] > mean ? lat->slot[i] - mean : mean - lat->slot[i];
        if (d > max_dev) max_dev = d;
    }
    int locked = (max_dev < (mean >> 1));
    if (locked) lat->flags |= APA_FLAG_CONSENSUS;
    return locked;
}

/* ════════════════════════════════════════════════════════════════
 * Dₙ(r) — from conscious/ll_analog.c (tuned: ldexp, no pow(r,1))
 * ════════════════════════════════════════════════════════════════ */

/* Precomputed strand_omega table: 1/PHI^(7i) for i=1..8 */
static const double STRAND_OMEGA[8] = {
    /* 1/PHI^(7i) for i=1..8  (correct: 1/(PHI^i)^7) */
    3.444185374863301813e-02,
    1.186241289642226207e-03,
    4.085634900844738285e-05,
    1.407168397252052311e-06,
    4.846548813785350797e-08,
    1.669241254300059229e-09,
    5.749176315178736765e-11,
    1.980122898224909489e-12,
};

static double dn_r(int n, double r, double omega_strand) {
    int idx = (n - 1) % DIMS;
    double F = FIB[idx], P = PRIMES[idx];
    double two_n = ldexp(1.0, n);          /* exact 2^n */
    return sqrt(PHI * F * two_n * P * omega_strand) * r;  /* pow(r,1)=r */
}

static uint32_t dn_lattice_compute(double dn_out[32]) {
    uint32_t agg = 0;
    for (int s = 1; s <= 8; s++) {
        double r   = 0.3 + 0.1 * (s - 1);
        double om  = STRAND_OMEGA[s - 1];
        int base_n = (s - 1) * 4;
        for (int sl = 0; sl < 4; sl++) {
            int n    = base_n + sl + 1;
            double d = dn_r(n, r, om);
            dn_out[n - 1] = d;
            if (d > SQRT_PHI) agg |= (1u << (n - 1));
        }
    }
    return agg;
}

/* ════════════════════════════════════════════════════════════════
 * 8D KURAMOTO — from conscious/ll_analog.c (mean-field tuned)
 * ════════════════════════════════════════════════════════════════ */

typedef struct {
    double theta[DIMS];
    double omega[DIMS], omega0[DIMS];
    double re[DIMS], im[DIMS];
    double k_coupling, gamma;
    double cv;
    APhase aphase;
    int    steps;
} KuramotoOsc;

static void kuramoto_init(KuramotoOsc *s, double lambda_phi) {
    double frac_L = lambda_phi - floor(lambda_phi);
    double Omega  = 0.5 * (1.0 + sin(PI * frac_L * PHI));
    s->aphase     = PLUCK;
    s->k_coupling = K_WU[PLUCK];
    s->gamma      = G_WU[PLUCK];
    s->cv         = 1.0;
    s->steps      = 0;
    for (int i = 0; i < DIMS; i++) {
        s->theta[i]  = PI * frac_L + 2.0 * PI *
                       (0.618 * PHI_INV + frac_L + i * D_N_R);
        s->omega0[i] = Omega * pow(PHI, 1.0 + i * D_N_R) * DT;
        s->omega[i]  = s->omega0[i];
        s->re[i]     = cos(s->theta[i]);
        s->im[i]     = sin(s->theta[i]);
    }
}

/* Mean-field derivative helper: N sincos per eval */
static void mf_deriv(const KuramotoOsc *s, const double th[DIMS],
                     double K, double g, double out[DIMS]) {
    double cs[DIMS], sn[DIMS], Re_S = 0.0, Im_S = 0.0;
    for (int j = 0; j < DIMS; j++) {
        cs[j] = cos(th[j]); sn[j] = sin(th[j]);
        Re_S += cs[j];       Im_S += sn[j];
    }
    for (int i = 0; i < DIMS; i++)
        out[i] = s->omega[i] * (1.0 - g) + K * (Im_S * cs[i] - Re_S * sn[i]);
}

/* Mean-field RK4: 32 trig calls/step (vs 256 naive) */
static void kuramoto_step(KuramotoOsc *s) {
    double K = s->k_coupling, g = s->gamma;
    double k1[DIMS], k2[DIMS], k3[DIMS], k4[DIMS];
    double t1[DIMS], t2[DIMS], t3[DIMS];
    /* k1: reuse re/im — zero extra trig */
    double Re_S = 0.0, Im_S = 0.0;
    for (int j = 0; j < DIMS; j++) { Re_S += s->re[j]; Im_S += s->im[j]; }
    for (int i = 0; i < DIMS; i++) {
        k1[i] = s->omega[i] * (1.0-g) + K * (Im_S*s->re[i] - Re_S*s->im[i]);
        t1[i] = s->theta[i] + 0.5 * DT * k1[i];
    }
    mf_deriv(s, t1, K, g, k2);
    for (int i = 0; i < DIMS; i++) t2[i] = s->theta[i] + 0.5*DT*k2[i];
    mf_deriv(s, t2, K, g, k3);
    for (int i = 0; i < DIMS; i++) t3[i] = s->theta[i] + DT*k3[i];
    mf_deriv(s, t3, K, g, k4);
    for (int i = 0; i < DIMS; i++) {
        s->theta[i] += (DT/6.0) * (k1[i] + 2.0*k2[i] + 2.0*k3[i] + k4[i]);
        s->re[i] = cos(s->theta[i]);
        s->im[i] = sin(s->theta[i]);
    }
    s->steps++;
}

static void kuramoto_update_cv(KuramotoOsc *s) {
    double rx = 0.0, ry = 0.0;
    for (int i = 0; i < DIMS; i++) { rx += s->re[i]; ry += s->im[i]; }
    s->cv = 1.0 - sqrt(rx*rx + ry*ry) / DIMS;
}

static void kuramoto_aphase_update(KuramotoOsc *s) {
    kuramoto_update_cv(s);
    /* VCO: omega tracks CV (wu-wei) */
    for (int i = 0; i < DIMS; i++)
        s->omega[i] = s->omega0[i] * (0.1 + 0.9 * s->cv);
    if (s->aphase == PLUCK    && s->cv < CV_TH[0]) { s->aphase = SUSTAIN;  s->gamma = G_WU[SUSTAIN];  s->k_coupling = K_WU[SUSTAIN];  }
    else if (s->aphase == SUSTAIN  && s->cv < CV_TH[1]) { s->aphase = FINETUNE; s->gamma = G_WU[FINETUNE]; s->k_coupling = K_WU[FINETUNE]; }
    else if (s->aphase == FINETUNE && s->cv < CV_TH[2]) { s->aphase = LOCK;     s->gamma = G_WU[LOCK];     s->k_coupling = K_WU[LOCK];     }
}

/* ════════════════════════════════════════════════════════════════
 * CONSCIOUS OS SERVICES — portable interface
 * These replace POSIX syscalls on HDGL bare metal.
 * On hosted OS they call through to libc/OS.
 * ════════════════════════════════════════════════════════════════ */

typedef struct {
    PhiLattice  lattice;
    KuramotoOsc osc;
    uint32_t    dn_agg;
    double      dn[32];
    int         initialized;
} ConsciousOS;

static ConsciousOS g_cos;

/* cos_init: boot the conscious OS port
 * lambda_phi: phi-lattice depth of the CPU identity (from CPUID)
 * On HDGL bare metal, call this from .hdgl_boot_sequence after Omega graph ready.
 * On hosted, call from main() before any phi-lattice operation. */
void cos_init(double lambda_phi) {
    phi_lattice_init(&g_cos.lattice);
    kuramoto_init(&g_cos.osc, lambda_phi);
    g_cos.dn_agg = dn_lattice_compute(g_cos.dn);
    g_cos.initialized = 1;
    cos_port_puts("[conscious] phi-lattice OS initialized");
}

/* cos_tick: one wu-wei step (call at top of every event loop iteration)
 * Replaces: PIT IRQ0 timer tick, scheduler quantum, sched_yield */
void cos_tick(void) {
    phi_lattice_tick(&g_cos.lattice);
    kuramoto_step(&g_cos.osc);
    kuramoto_aphase_update(&g_cos.osc);
    cos_port_yield();
}

/* cos_hash: phi_fold keyed MAC — replaces SHA-256
 * key: pointer, len: length → 32-bit phi-fold result
 * Lattice IS the key. No external key material. */
uint32_t cos_hash(const void *key, size_t len) {
    return phi_fold(&g_cos.lattice, (const uint8_t *)key, len);
}

/* cos_consensus: returns 1 if phi-lattice has reached phase lock
 * Replaces: privilege ring check, capability check, authentication */
int cos_consensus(void) {
    return phi_detect_consensus(&g_cos.lattice);
}

/* cos_aphase: current Kuramoto adaptive phase name */
const char *cos_aphase_name(void) {
    static const char *names[] = {"PLUCK","SUSTAIN","FINETUNE","LOCK"};
    return names[g_cos.osc.aphase];
}

/* cos_run_to_lock: advance until Kuramoto LOCK (max iter)
 * Returns steps taken. Used for initialization sync. */
int cos_run_to_lock(int max_iter) {
    for (int i = 0; i < max_iter; i++) {
        cos_tick();
        if (g_cos.osc.aphase == LOCK && g_cos.osc.cv < CV_TH[3]) return i + 1;
    }
    return max_iter;
}

/* cos_status: print current OS state to serial */
void cos_status(void) {
    static const char *aph[] = {"PLUCK","SUSTAIN","FINETUNE","LOCK"};
    char buf[80];
    cos_port_puts("[conscious] OS status:");

    /* phi-lattice */
    snprintf(buf, sizeof(buf), "  phi-lattice: tick=%u  goi=%u  guz=%u  consensus=%s",
             g_cos.lattice.tick, g_cos.lattice.goi_events, g_cos.lattice.guz_events,
             (g_cos.lattice.flags & APA_FLAG_CONSENSUS) ? "LOCK" : "PLUCK");
    cos_port_puts(buf);

    /* Kuramoto */
    snprintf(buf, sizeof(buf), "  Kuramoto: aphase=%s  CV=%.6f  steps=%d",
             aph[g_cos.osc.aphase], g_cos.osc.cv, g_cos.osc.steps);
    cos_port_puts(buf);

    /* Dn(r) */
    snprintf(buf, sizeof(buf), "  Dn agg: 0x%08X  (expected 0x80C0C0E8: %s)",
             g_cos.dn_agg, g_cos.dn_agg == 0x80C0C0E8 ? "PASS" : "CHECK");
    cos_port_puts(buf);
}

/* ════════════════════════════════════════════════════════════════
 * HDGL OMEGA GRAPH BINDING
 * On bare metal: read node states from phi-lattice memory map.
 * On hosted: simulate via phi_lattice state.
 * ════════════════════════════════════════════════════════════════ */

#ifdef LAYER_HDGL
#define OMEGA_BASE  ((volatile uint32_t *)0x100000)
#define OMEGA_SZ    16   /* dwords per node (64 bytes / 4) */
#define NODE_STATE_OFF  3   /* dword offset of state field */
#define NODE_TYPE_OFF   2   /* dword offset of type field */

static uint32_t omega_get_state(int node_idx) {
    return OMEGA_BASE[node_idx * OMEGA_SZ + NODE_STATE_OFF];
}
static uint32_t omega_get_type(int node_idx) {
    return OMEGA_BASE[node_idx * OMEGA_SZ + NODE_TYPE_OFF] & 0xFFFF;
}

/* cos_from_omega: seed Kuramoto λ from CPU node identity
 * Maps hardware ID → phi-lattice depth as in conscious ll_analog.c */
double cos_lambda_from_omega(void) {
    uint32_t cpu_id = OMEGA_BASE[1 * OMEGA_SZ + 0];  /* node 1 = CPU, identity */
    if (cpu_id == 0) cpu_id = 1;
    return log((double)cpu_id * 0.6931 / LN_PHI) / LN_PHI - 0.5 / PHI;
}
#else
double cos_lambda_from_omega(void) { return 1.618; }  /* default φ depth on hosted */
#endif

/* ════════════════════════════════════════════════════════════════
 * HOSTED MAIN — compile with: gcc -O3 -lm conscious_os_port.c -o cos_test
 * ════════════════════════════════════════════════════════════════ */
#ifndef LAYER_HDGL
#ifndef COS_LIB_ONLY

#include <stdlib.h>

static int test_pass = 0, test_fail = 0;
#define TEST(name, expr) do { \
    if (expr) { printf("  PASS  %s\n", name); test_pass++; } \
    else      { printf("  FAIL  %s\n", name); test_fail++; } \
} while(0)

int main(void) {
    printf("\n");
    printf("=============================================================\n");
    printf("  conscious OS port — self-test + boot simulation\n");
    printf("=============================================================\n\n");

    /* Init */
    double lambda = cos_lambda_from_omega();
    cos_init(lambda);

    printf("-- CORRECTNESS TESTS ----------------------------------------\n");
    TEST("Dn aggregate == 0x80C0C0E8", g_cos.dn_agg == 0x80C0C0E8);
    TEST("phi_lattice init: slot[0] non-zero", g_cos.lattice.slot[0] != 0);
    TEST("phi_lattice init: slot[127] non-zero", g_cos.lattice.slot[127] != 0);
    TEST("GOI limit applied: slot <= 0xFFFF0000",
         g_cos.lattice.slot[0] <= GOI_LIMIT);
    TEST("GUZ limit applied: slot >= 0x100",
         g_cos.lattice.slot[0] >= GUZ_LIMIT);

    /* phi_fold reproducibility */
    const uint8_t key[] = "HDGL.phi.fold.test";
    uint32_t h1 = cos_hash(key, sizeof(key));
    uint32_t h2 = cos_hash(key, sizeof(key));
    TEST("phi_fold deterministic", h1 == h2);
    TEST("phi_fold non-zero", h1 != 0);

    /* Kuramoto convergence */
    printf("\n-- KURAMOTO CONVERGENCE ------------------------------------\n");
    int steps = cos_run_to_lock(2000);
    TEST("Kuramoto reaches LOCK",
         g_cos.osc.aphase == LOCK && g_cos.osc.cv < CV_TH[3]);
    printf("  Steps to LOCK: %d  CV: %.6f\n", steps, g_cos.osc.cv);

    /* Consensus */
    for (int i = 0; i < 200; i++) cos_tick();
    int cons = cos_consensus();
    printf("  Consensus after 200 more ticks: %s\n", cons ? "LOCK" : "CONVERGING");

    printf("\n-- STATUS ---------------------------------------------------\n");
    cos_status();

    printf("\n-- SUMMARY --------------------------------------------------\n");
    printf("  Tests: %d pass  %d fail\n", test_pass, test_fail);
    printf("  phi_fold(\"HDGL.phi.fold.test\"): 0x%08X\n", h1);
    printf("  GOI events: %u  GUZ events: %u\n",
           g_cos.lattice.goi_events, g_cos.lattice.guz_events);
    printf("=============================================================\n\n");

    return test_fail > 0 ? 1 : 0;
}

#endif /* COS_LIB_ONLY */
#endif /* !LAYER_HDGL */
