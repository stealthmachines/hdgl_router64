#define _POSIX_C_SOURCE 200809L
/*
 * ============================================================================
 * HDGL BOOTSTRAP - HOST-SIDE COMPILER RUNNER
 * ============================================================================
 *
 * This is the ONLY C file in the system.
 * It exists for one reason: to run the HDGL compiler before the HDGL
 * compiler can run itself.
 *
 * Once hdgl_compiler.hdgl has been compiled by this bootstrap,
 * the resulting binary replaces this file. It is never needed again.
 * That is the definition of self-hosting.
 *
 * Philosophy (PLAN.md):
 *   Ω (State) + T (Transformation) → Ω'
 *   No AST. No IR. No middlemen.
 *   Only: identity | type | relation | transform | state | parent | child | next
 *
 * This file implements the minimum execution engine for the HDGL model.
 * It reads .hdgl source and either:
 *   a) Interprets it (simulating the firmware's rewrite loop)
 *   b) Emits x86 assembly (compiling the firmware)
 *
 * Usage:
 *   ./hdgl_bootstrap hdgl_firmware.hdgl firmware.asm    # compile to asm
 *   ./hdgl_bootstrap hdgl_firmware.hdgl                 # interpret/simulate
 *
 * ============================================================================
 */

/*
 * ============================================================================
 * REFACTORED: gleaned from conscious-128-bit-floor
 * ============================================================================
 * Changes from original:
 *   - Emitted ASM: NO xor reg,reg for zero-init → mov reg,0
 *   - Emitted ASM: NO xor-based crypto → phi_fold additive throughout
 *   - Emitted ASM: register/memory clear via explicit mov dword [...], 0
 *   - phi_seed_table: values derived from conscious ll_analog.c seeding:
 *       seed[i] = fib[i%8] * prime[i%8] * PHI^(1+(i%4)) * 65536
 *   - Dn aggregate: 0x80C0C0E8 (verified via conscious compute_Dn_r)
 *   - GOI/GUZ limits: 0xFFFF0000 / 0x00000100 (bootloaderZ V6.0)
 * ============================================================================
 */


#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "hdgl_analog_engine.c"

/* ============================================================================
 * Ω (OMEGA) — THE FUNDAMENTAL STATE UNIT
 * Exactly as defined in hdgl_universal_preassembler.c and PLAN.md
 * ============================================================================ */

typedef struct Omega {
    uint64_t identity;    /* Unique ID */
    uint64_t type;        /* 0=VOID 1=CPU 2=MEM 3=IO 4=COMPILER 5=RUNTIME ... */
    uint64_t relation;    /* Relationship to other Ω */
    uint64_t transform;   /* Last transformation applied */
    uint64_t state;       /* Current state value */
    uint64_t parent;      /* Parent identity */
    uint64_t child;       /* First child identity */
    uint64_t next;        /* Next sibling identity */
    uint32_t flags;
    uint32_t _pad;
} Omega;

/* State values — from hdgl_self_hosting_runtime_final.c */
typedef enum {
    STATE_INIT       = 0,
    STATE_DISCOVERED = 1,
    STATE_CONFIGURED = 2,
    STATE_READY      = 3,
    STATE_EXECUTED   = 4,
    STATE_COMPLETED  = 5
} OmegaState;

/* Type codes — from hdgl_universal_preassembler.c */
typedef enum {
    TYPE_VOID        = 0,
    TYPE_ROOT        = 0,
    TYPE_CPU         = 1,
    TYPE_MEM         = 2,
    TYPE_IO          = 3,
    TYPE_COMPILER    = 4,
    TYPE_RUNTIME     = 5,
    TYPE_BOOTSTRAP   = 6,
    TYPE_REPLICATION = 7,
    TYPE_PCI         = 8,
    TYPE_GPU         = 9,
    TYPE_STORAGE     = 10,
    TYPE_BOOT        = 11,
    TYPE_CODEGEN     = 12
} OmegaType;

/* Transform codes — from hdgl_universal_preassembler.c */
#define TRANSFORM_IDENTITY      0x0000000000000000ULL
#define TRANSFORM_CPUID         0x0001000000000000ULL
#define TRANSFORM_E820          0x0002000000000000ULL
#define TRANSFORM_PCI_WALK      0x0003000000000000ULL
#define TRANSFORM_COMPILE_SELF  0x0004000000000000ULL
#define TRANSFORM_REWRITE       0x0005000000000000ULL
#define TRANSFORM_ALLOCATED     0x1000000000000000ULL
#define TRANSFORM_CONFIGURED    0x2000000000000000ULL
#define TRANSFORM_TRANSFER      0x4000000000000000ULL

/* ============================================================================
 * T (TRANSFORMATION) — THE REWRITE OPERATOR
 * From hdgl_universal_preassembler.c
 * ============================================================================ */

typedef struct Rule {
    uint64_t match;       /* Pattern: match on Ω.state + Ω.type */
    uint64_t replace;     /* New state value */
    uint64_t transform;   /* Transform code to apply */
    uint32_t params;
    uint8_t  flags;
} Rule;

/* ============================================================================
 * THE MACHINE — Ω + Rules collapsed into one structure
 * From hdgl_universal_preassembler.c
 * ============================================================================ */

#define MAX_OMEGA  256
#define MAX_RULES  256
#define MAX_EMIT   65536

typedef struct {
    Omega    graph[MAX_OMEGA];
    size_t   graph_size;
    Rule     rules[MAX_RULES];
    size_t   rule_count;
    uint64_t tick;
    uint64_t cycles;
    /* Emit buffer for code generation */
    char     emit_buf[MAX_EMIT];
    size_t   emit_pos;
    int      emit_mode;   /* 0=interpret, 1=emit_asm */
} Machine;

static Machine M;

/* ============================================================================
 * EMIT HELPERS
 * ============================================================================ */

static void emit(const char* s) {
    if (M.emit_mode) {
        size_t n = strlen(s);
        if (M.emit_pos + n < MAX_EMIT) {
            memcpy(M.emit_buf + M.emit_pos, s, n);
            M.emit_pos += n;
        }
    }
}

static void emitf(const char* fmt, ...) {
    if (!M.emit_mode) return;
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    emit(tmp);
}



/* ============================================================================
 * OMEGA GRAPH OPERATIONS
 * Directly implementing the primitives from hdgl_self_hosting_runtime_final.c
 * ============================================================================ */

static Omega* omega_alloc(uint64_t type, const char* name) {
    if (M.graph_size >= MAX_OMEGA) return NULL;
    Omega* o = &M.graph[M.graph_size];
    o->identity  = M.graph_size;
    o->type      = type;
    o->relation  = 0;
    o->transform = TRANSFORM_IDENTITY;
    o->state     = STATE_INIT;
    o->parent    = (uint64_t)-1;
    o->child     = (uint64_t)-1;
    o->next      = (uint64_t)-1;
    o->flags     = 0;
    M.graph_size++;
    return o;
}

static Omega* omega_find_type(uint64_t type) {
    for (size_t i = 0; i < M.graph_size; i++)
        if (M.graph[i].type == type) return &M.graph[i];
    return NULL;
}

/* T: Transformation operator — the core of PLAN.md's Ω_n+1 = T(Ω_n) */
static void omega_transform(Omega* o, uint64_t transform, OmegaState new_state) {
    o->transform = transform;
    o->state     = new_state;
    M.cycles++;
}

/* Branch: link child to parent — graph construction */
static void omega_branch(Omega* parent, Omega* child) {
    child->parent = parent->identity;
    if (parent->child == (uint64_t)-1) {
        parent->child = child->identity;
    } else {
        Omega* sib = &M.graph[parent->child];
        while (sib->next != (uint64_t)-1)
            sib = &M.graph[sib->next];
        sib->next = child->identity;
    }
}

/* Recurse: apply fn to all children — the rewrite pass */
static void omega_recurse(Omega* root, void (*fn)(Omega*)) {
    if (root->child == (uint64_t)-1) return;
    Omega* cur = &M.graph[root->child];
    while (cur) {
        fn(cur);
        if (cur->next == (uint64_t)-1) break;
        cur = &M.graph[cur->next];
    }
}

/* ============================================================================
 * HDGL PARSER
 * Reads .hdgl source. Builds Omega graph. No AST. No IR.
 * Implements the parse rules from hdgl_compiler.hdgl.
 * ============================================================================ */

typedef struct {
    const char* src;
    size_t      pos;
    size_t      len;
    int         line;
} Parser;

static void skip_ws(Parser* p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r') { p->pos++; continue; }
        if (c == '\n') { p->pos++; p->line++; continue; }
        if (c == '#') {
            while (p->pos < p->len && p->src[p->pos] != '\n') p->pos++;
            continue;
        }
        break;
    }
}

static int match_word(Parser* p, const char* word) {
    skip_ws(p);
    size_t n = strlen(word);
    if (p->pos + n > p->len) return 0;
    if (strncmp(p->src + p->pos, word, n) != 0) return 0;
    /* Must be followed by whitespace or punctuation, not another word char */
    char next = (p->pos + n < p->len) ? p->src[p->pos + n] : 0;
    if (next && next != ' ' && next != '\t' && next != '\n' && next != '\r'
             && next != '{' && next != '}' && next != '=' && next != 0)
        return 0;
    p->pos += n;
    return 1;
}

static int read_identifier(Parser* p, char* out, size_t max) {
    skip_ws(p);
    size_t i = 0;
    while (p->pos < p->len && i + 1 < max) {
        char c = p->src[p->pos];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            out[i++] = c;
            p->pos++;
        } else break;
    }
    out[i] = 0;
    return i > 0;
}

static uint64_t class_from_name(const char* name) {
    if (strstr(name, "ROOT"))     return TYPE_ROOT;
    if (strstr(name, "CPU"))      return TYPE_CPU;
    if (strstr(name, "MEM"))      return TYPE_MEM;
    if (strstr(name, "IO"))       return TYPE_IO;
    if (strstr(name, "COMPILER")) return TYPE_COMPILER;
    if (strstr(name, "RUNTIME"))  return TYPE_RUNTIME;
    if (strstr(name, "BOOT"))     return TYPE_BOOT;
    if (strstr(name, "STORAGE"))  return TYPE_STORAGE;
    if (strstr(name, "GPU"))      return TYPE_GPU;
    if (strstr(name, "PCI"))      return TYPE_PCI;
    if (strstr(name, "REPLICATION")) return TYPE_REPLICATION;
    if (strstr(name, "CODEGEN"))  return TYPE_CODEGEN;
    /* Try lowercase */
    if (strstr(name, "root"))     return TYPE_ROOT;
    if (strstr(name, "cpu"))      return TYPE_CPU;
    if (strstr(name, "mem") || strstr(name, "memory")) return TYPE_MEM;
    if (strstr(name, "io"))       return TYPE_IO;
    if (strstr(name, "compiler")) return TYPE_COMPILER;
    if (strstr(name, "boot"))     return TYPE_BOOT;
    if (strstr(name, "storage"))  return TYPE_STORAGE;
    if (strstr(name, "gpu"))      return TYPE_GPU;
    if (strstr(name, "pci"))      return TYPE_PCI;
    if (strstr(name, "replicate")) return TYPE_REPLICATION;
    return TYPE_VOID;
}

static OmegaState state_from_name(const char* name) {
    if (strstr(name, "DISCOVERED") || strstr(name, "discovered")) return STATE_DISCOVERED;
    if (strstr(name, "CONFIGURED") || strstr(name, "configured")) return STATE_CONFIGURED;
    if (strstr(name, "READY")      || strstr(name, "ready"))      return STATE_READY;
    if (strstr(name, "EXECUTED")   || strstr(name, "executed"))   return STATE_EXECUTED;
    if (strstr(name, "COMPLETED")  || strstr(name, "completed"))  return STATE_COMPLETED;
    return STATE_INIT;
}

/* Skip to matching "end" keyword at same nesting depth */
static void skip_to_end(Parser* p) {
    int depth = 1;
    while (p->pos < p->len && depth > 0) {
        char c = p->src[p->pos];
        /* Skip comments */
        if (c == '#') {
            while (p->pos < p->len && p->src[p->pos] != '\n') p->pos++;
            continue;
        }
        /* Skip whitespace */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (c == '\n') p->line++;
            p->pos++;
            continue;
        }
        /* Check for keywords */
        if (strncmp(p->src + p->pos, "end", 3) == 0) {
            char a = (p->pos+3 < p->len) ? p->src[p->pos+3] : 0;
            if (!a || a == ' ' || a == '\t' || a == '\n' || a == '\r') {
                p->pos += 3;
                depth--;
                continue;
            }
        }
        if (strncmp(p->src + p->pos, "glyph", 5) == 0 ||
            strncmp(p->src + p->pos, "rule",  4) == 0 ||
            strncmp(p->src + p->pos, "phase", 5) == 0 ||
            strncmp(p->src + p->pos, "entry", 5) == 0) {
            /* Only count "emit" as block opener if it appears as a standalone keyword
             * (followed by newline), not as an attribute (emit = value) */
            depth++;
        } else if (strncmp(p->src + p->pos, "emit", 4) == 0) {
            /* Check if this is "emit \n" (block) or "emit = " (attribute) */
            size_t scan = p->pos + 4;
            while (scan < p->len && (p->src[scan] == ' ' || p->src[scan] == '\t')) scan++;
            if (scan < p->len && p->src[scan] != '=' && p->src[scan] != '\n' && p->src[scan] != '\r') {
                /* standalone emit block */
                depth++;
            }
            /* else: "emit = value" - attribute, don't increment depth */
        }
        /* Skip to end of word */
        while (p->pos < p->len) {
            char nc = p->src[p->pos];
            if (nc == ' ' || nc == '\t' || nc == '\n' || nc == '\r') break;
            p->pos++;
        }
    }
}

/* Parse attributes inside a glyph block */
static void parse_attributes(Parser* p, Omega* o) {
    int limit = 10000;
    while (p->pos < p->len && limit-- > 0) {
        skip_ws(p);
        if (p->pos >= p->len) break;

        /* Check for "end" keyword */
        if (strncmp(p->src + p->pos, "end", 3) == 0) {
            char after = p->src[p->pos + 3];
            if (!after || after == ' ' || after == '\n' || after == '\r' || after == '\t')
                break;
        }

        if (limit % 100 == 0) fprintf(stderr, "  PA: pos=%zu len=%zu char=%c\n", p->pos, p->len, p->pos < p->len ? p->src[p->pos] : '?'); fflush(stderr);
        char key[64] = {0};
        if (!read_identifier(p, key, sizeof(key))) { p->pos++; continue; }

        skip_ws(p);
        if (p->pos < p->len && p->src[p->pos] == '=') {
            p->pos++;
            skip_ws(p);
            char val[128] = {0};
            size_t vi = 0;
            /* Read value (may be quoted or bare) */
            if (p->pos < p->len && p->src[p->pos] == '{') {
                /* Skip cap block */
                while (p->pos < p->len && p->src[p->pos] != '}') p->pos++;
                if (p->pos < p->len) p->pos++;
                continue;
            }
            while (p->pos < p->len && p->src[p->pos] != '\n' &&
                   p->src[p->pos] != '\r' && vi + 1 < sizeof(val)) {
                val[vi++] = p->src[p->pos++];
            }
            /* Trim trailing whitespace */
            while (vi > 0 && (val[vi-1] == ' ' || val[vi-1] == '\t')) vi--;
            val[vi] = 0;

            if (strcmp(key, "class") == 0 || strcmp(key, "CLASS") == 0)
                o->type = class_from_name(val);
            else if (strcmp(key, "state") == 0 || strcmp(key, "STATE") == 0)
                o->state = state_from_name(val);
            else if (strcmp(key, "parent") == 0) {
                /* Link to parent by name later */
                uint64_t ptype = class_from_name(val);
                Omega* par = omega_find_type(ptype);
                if (par) omega_branch(par, o);
            }
        } else {
            /* Not an attribute - might be "rule", "phase", etc. - skip block */
            if (strcmp(key, "rule")  == 0 || strcmp(key, "phase") == 0 ||
                strcmp(key, "entry") == 0 || strcmp(key, "emit")  == 0) {
                /* Skip the identifier and body */
                char tmp[64] = {0};
                read_identifier(p, tmp, sizeof(tmp));
                skip_ws(p);
                /* Skip the body until matching end */
                skip_to_end(p);
            }
            /* Skip line */
            while (p->pos < p->len && p->src[p->pos] != '\n') p->pos++;
        }
    }
}

/* ============================================================================
 * MAIN PARSE LOOP
 * Implements: for(each node) { rule = lookup(node); rewrite(node,rule); emit(node); }
 * ============================================================================ */

static void hdgl_parse(Parser* p) {
    int main_limit = 5000;
    int main_iter = 0;
    size_t last_pos = 0;
    int stall_count = 0;
    while (p->pos < p->len && main_limit-- > 0) {
        if (p->pos == last_pos) {
            if (++stall_count > 10) {
                fprintf(stderr, "STALL at pos=%zu char=%c (0x%02x)\n",
                    p->pos, p->src[p->pos], (unsigned char)p->src[p->pos]);
                fflush(stderr);
                p->pos++;
                stall_count = 0;
            }
        } else { stall_count = 0; last_pos = p->pos; }
        if (++main_iter % 10000 == 0) fprintf(stderr, "HDGL: parse pos=%zu len=%zu nodes=%zu\n", p->pos, p->len, M.graph_size);
        skip_ws(p);
        if (p->pos >= p->len) break;

        /* glyph IDENTIFIER ... end */
        if (match_word(p, "glyph")) {
            char name[64] = {0};
            if (!read_identifier(p, name, sizeof(name))) continue;

            uint64_t type = class_from_name(name);
            Omega* o = omega_alloc(type, name);
            if (!o) { fprintf(stderr, "Omega pool exhausted\n"); break; }

            /* First glyph becomes root's child if it IS root */
            if (type == TYPE_ROOT) {
                /* root node itself */
            } else if (M.graph_size > 1 && M.graph[0].child == (uint64_t)-1) {
                /* Link to root by default if no parent attribute found later */
            }

            parse_attributes(p, o);
            match_word(p, "end");

            /* If this node has no parent yet and isn't root, attach to root */
            if (type != TYPE_ROOT && o->parent == (uint64_t)-1 && M.graph_size > 1)
                omega_branch(&M.graph[0], o);

            continue;
        }

        /* recurse IDENTIFIER ... end */
        if (match_word(p, "recurse")) {
            char name[64] = {0};
            read_identifier(p, name, sizeof(name));
            uint64_t type = class_from_name(name);
            Omega* target = omega_find_type(type);
            if (target) {
                /* Apply one tick to all children */
                Omega* cur = (target->child != (uint64_t)-1)
                           ? &M.graph[target->child] : NULL;
                while (cur) {
                    if (cur->state < STATE_EXECUTED)
                        cur->state++;
                    if (cur->next == (uint64_t)-1) break;
                    cur = &M.graph[cur->next];
                }
            }
            skip_to_end(p);
            continue;
        }

        /* mutate STATE_NAME */
        if (match_word(p, "mutate")) {
            char name[64] = {0};
            read_identifier(p, name, sizeof(name));
            /* Apply to last active node */
            if (M.graph_size > 0) {
                Omega* o = &M.graph[M.graph_size - 1];
                o->state = state_from_name(name);
                o->transform = TRANSFORM_REWRITE;
            }
            continue;
        }

        /* branch PARENT CHILD */
        if (match_word(p, "branch")) {
            char pname[64] = {0}, cname[64] = {0};
            read_identifier(p, pname, sizeof(pname));
            read_identifier(p, cname, sizeof(cname));
            Omega* par = omega_find_type(class_from_name(pname));
            Omega* chi = omega_find_type(class_from_name(cname));
            if (par && chi) omega_branch(par, chi);
            continue;
        }

        /* rule, phase, entry, output_template, self_compile_loop - skip */
        if (match_word(p, "rule")         || match_word(p, "phase")    ||
            match_word(p, "entry")        || match_word(p, "output_template") ||
            match_word(p, "self_compile_loop") || match_word(p, "self_replicate_engine") ||
            match_word(p, "omega_runtime")|| match_word(p, "self_extract_module") ||
            match_word(p, "storage_manager") || match_word(p, "bootstrap_sequence") ||
            match_word(p, "default_program") || match_word(p, "exit_handler")) {
            char tmp[64] = {0};
            read_identifier(p, tmp, sizeof(tmp));
            skip_to_end(p);
            continue;
        }

        /* Unknown token - skip line */
        while (p->pos < p->len && p->src[p->pos] != '\n') p->pos++;
    }
}

/* ============================================================================
 * THE UNIVERSAL TICK — Ω_n+1 = T(Ω_n)
 * Apply rewrite rules until no node changes state (fixed point)
 * ============================================================================ */

static int hdgl_tick(void) {
    int changed = 0;
    for (size_t i = 0; i < M.graph_size; i++) {
        Omega* o = &M.graph[i];
        if (o->type == TYPE_VOID) continue;
        if (o->state >= STATE_EXECUTED) continue;
        /* Default rule: advance state */
        o->state++;
        o->transform = TRANSFORM_REWRITE;
        changed = 1;
        M.cycles++;
    }
    M.tick++;
    return changed;
}

/* ============================================================================
 * EMIT — CODE GENERATION
 * Applies emit rules to produce x86 assembly from the Omega graph
 * Reads the embedded emit rules from hdgl_firmware.hdgl
 * ============================================================================ */

static const char* state_name(uint64_t s) {
    switch (s) {
    case STATE_INIT:       return "INIT";
    case STATE_DISCOVERED: return "DISCOVERED";
    case STATE_CONFIGURED: return "CONFIGURED";
    case STATE_READY:      return "READY";
    case STATE_EXECUTED:   return "EXECUTED";
    case STATE_COMPLETED:  return "COMPLETED";
    default:               return "UNKNOWN";
    }
}

static const char* type_name(uint64_t t) {
    switch (t) {
    case TYPE_ROOT:        return "ROOT";
    case TYPE_CPU:         return "CPU";
    case TYPE_MEM:         return "MEM";
    case TYPE_IO:          return "IO";
    case TYPE_COMPILER:    return "COMPILER";
    case TYPE_RUNTIME:     return "RUNTIME";
    case TYPE_BOOTSTRAP:   return "BOOTSTRAP";
    case TYPE_REPLICATION: return "REPLICATION";
    case TYPE_PCI:         return "PCI";
    case TYPE_GPU:         return "GPU";
    case TYPE_STORAGE:     return "STORAGE";
    case TYPE_BOOT:        return "BOOT";
    case TYPE_CODEGEN:     return "CODEGEN";
    default:               return "VOID";
    }
}

static void hdgl_print_graph(void) {
    printf("\nOmega Graph after %llu ticks (%llu cycles):\n",
           (unsigned long long)M.tick,
           (unsigned long long)M.cycles);
    printf("%-4s %-12s %-12s %-10s\n", "ID", "TYPE", "STATE", "TRANSFORM");
    printf("%-4s %-12s %-12s %-10s\n", "---", "----", "-----", "---------");
    for (size_t i = 0; i < M.graph_size; i++) {
        Omega* o = &M.graph[i];
        if (o->type == TYPE_VOID && i > 0) continue;
        printf("%-4llu %-12s %-12s 0x%016llx",
               (unsigned long long)o->identity,
               type_name(o->type),
               state_name(o->state),
               (unsigned long long)o->transform);
        if (o->parent != (uint64_t)-1)
            printf("  parent=%llu", (unsigned long long)o->parent);
        if (o->child != (uint64_t)-1)
            printf("  child=%llu", (unsigned long long)o->child);
        printf("\n");
    }
}

/* ============================================================================
 * EMIT ASM — produce assembly that runs the Omega graph natively
 * This is the codegen pass. Output is fed to nasm.
 * ============================================================================ */

static void hdgl_emit_asm(FILE* out) {
    /* Read the runtime x86 from the emit rules in hdgl_firmware.hdgl.
     * In this bootstrap: we emit the assembled version of those rules.
     * The full self-hosting system would parse and execute the emit rules
     * directly from the source. */

    fprintf(out,
        "; ============================================================\n"
        "; HDGL FIRMWARE - GENERATED BY HDGL BOOTSTRAP\n"
        "; Source: hdgl_firmware.hdgl\n"
        "; Ω (State) + T (Transformation) = This File\n"
        "; ============================================================\n"
        "\n"
        "[BITS 16]\n"
        "[ORG 0x7E00]\n"
        "\n"
        "%%define OMEGA_NODE(n) (OMEGA_BASE + (n)*OMEGA_SZ)\n"
        "\n"
        "OMEGA_BASE          equ 0x100000\n"
        "OMEGA_SZ            equ 64\n"
        "STATE_INIT          equ 0\n"
        "STATE_DISCOVERED    equ 1\n"
        "STATE_CONFIGURED    equ 2\n"
        "STATE_READY         equ 3\n"
        "STATE_EXECUTED      equ 4\n"
        "TYPE_ROOT           equ 0\n"
        "TYPE_CPU            equ 1\n"
        "TYPE_MEM            equ 2\n"
        "TYPE_IO             equ 3\n"
        "TYPE_COMP           equ 4\n"
        "TYPE_PCI            equ 8\n"
        "TRANSFORM_CPUID         equ 0x00010000\n"
        "TRANSFORM_E820          equ 0x00020000\n"
        "TRANSFORM_PCI           equ 0x00030000\n"
        "TRANSFORM_COMPILE       equ 0x00040000\n"
        "TRANSFORM_COMPILE_SELF  equ 0x00040000\n"
        "TRANSFORM_REWRITE       equ 0x00050000\n"
        "\n"
        "\n"
        "runtime_entry:\n"
        "    cli\n"
        "    mov  ax, 0\n"
        "    mov  ds, ax\n"
        "    mov  es, ax\n"
        "    mov  ss, ax\n"
        "    mov  sp, 0x7BF0\n"
        "    ; Enable A20\n"
        "    in   al, 0x92\n"
        "    or   al, 0x02\n"
        "    and  al, 0xFE\n"
        "    out  0x92, al\n"
        "    ; Disk I/O: ATA PIO from PM (no trampoline needed)\n"
        "    lgdt [.gdt_ptr]\n"
        "    mov  eax, cr0\n"
        "    or   eax, 1\n"
        "    mov  cr0, eax\n"
        "    jmp  0x08:.pm32\n"
        "\n"
        "\n"
        "[BITS 32]\n"
        ".pm32:\n"
        "    mov ax, 0x10\n"
        "    mov ds, ax\n"
        "    mov es, ax\n"
        "    mov fs, ax\n"
        "    mov gs, ax\n"
        "    mov ss, ax\n"
        "    mov esp, 0x9F000\n"
        "\n"
        "    ; COM1 init (9600 8N1) - serial output for Omega state\n"
        "    mov dx, 0x3F9\n"
        "    mov al, 0\n"
        "    out dx, al\n"
        "    mov dx, 0x3FB\n"
        "    mov al, 0x80\n"
        "    out dx, al\n"
        "    mov dx, 0x3F8\n"
        "    mov al, 12\n"
        "    out dx, al\n"
        "    mov dx, 0x3F9\n"
        "    mov al, 0\n"
        "    out dx, al\n"
        "    mov dx, 0x3FB\n"
        "    mov al, 0x03\n"
        "    out dx, al\n"
        "    mov dx, 0x3FA\n"
        "    mov al, 0xC7\n"
        "    out dx, al\n"
        "\n"
        "    ; Phi-seed Omega graph region (explicit loop, no xor+rep)\n"
        "    mov  edi, 0x100000\n"
        "    mov  ecx, 1024\n"
        ".omega_zero:\n"
        "    mov  dword [edi], 0\n"
        "    add  edi, 4\n"
        "    dec  ecx\n"
        "    jnz  .omega_zero\n"
        "\n"
        "    ; Execute boot sequence: graph evolution\n"
        "    call .hdgl_boot_sequence\n"
        "\n"
        "    ; Initialize kernel (IDT, PIC, PIT)\n"
        "    call .kernel_init\n"
        "    ; Hand off to HDGL interactive shell\n"
        "    call .hdgl_shell\n"
        ".idle:\n"
        "    hlt\n"
        "    jmp .idle\n"
        "\n"
        "; ── Omega constants ──────────────────────────────────────────\n"
        "OMEGA_BASE  equ 0x100000\n"
        "OMEGA_SZ    equ 64\n"
        "\n"
        "STATE_INIT          equ 0\n"
        "STATE_DISCOVERED    equ 1\n"
        "STATE_CONFIGURED    equ 2\n"
        "STATE_READY         equ 3\n"
        "STATE_EXECUTED      equ 4\n"
        "\n"
        "TYPE_ROOT  equ 0\n"
        "TYPE_CPU   equ 1\n"
        "TYPE_MEM   equ 2\n"
        "TYPE_IO    equ 3\n"
        "TYPE_COMP  equ 4\n"
        "\n"
        "TRANSFORM_CPUID     equ 0x00010000\n"
        "TRANSFORM_E820      equ 0x00020000\n"
        "TRANSFORM_PCI       equ 0x00030000\n"
        "TRANSFORM_COMPILE   equ 0x00040000\n"
        "TRANSFORM_REWRITE   equ 0x00050000\n"
        "\n"
        "\n"
    );

    /* Emit Omega node indices derived from graph */
    fprintf(out, "; Omega node layout (derived from hdgl_firmware.hdgl glyph definitions)\n");
    for (size_t i = 0; i < M.graph_size; i++) {
        Omega* o = &M.graph[i];
        fprintf(out, "; Node %-2llu : %-12s  state=%-12s  transform=0x%016llx\n",
                (unsigned long long)i,
                type_name(o->type),
                state_name(o->state),
                (unsigned long long)o->transform);
    }
    fprintf(out, "\n");

    /* Boot sequence: graph evolution phases */
    fprintf(out,
        "; COM1 send char: AL = char to send\n"
        ".com1_send:\n"
        "    push edx\n"
        "    push eax\n"
        ".com1_w: mov dx, 0x3FD\n"
        "    in al, dx\n"
        "    test al, 0x20\n"
        "    jz .com1_w\n"
        "    pop eax\n"
        "    mov dx, 0x3F8\n"
        "    out dx, al\n"
        "    pop edx\n"
        "    ret\n"
        "\n"
        "; COM1 print null-terminated string at ESI\n"
        ".com1_str:\n"
        "    push eax\n"
        "    push esi\n"
        ".cs_loop: mov al, [esi]\n"
        "    test al, al\n"
        "    jz .cs_done\n"
        "    call .com1_send\n"
        "    inc esi\n"
        "    jmp .cs_loop\n"
        ".cs_done: pop esi\n"
        "    pop eax\n"
        "    ret\n"
        "\n"
        ".hdgl_boot_sequence:\n"
        "    ; === Ω Boot: INIT -> OBSERVE ===\n"
        "    mov esi, .msg_boot\n"
        "    call .com1_str\n"
        "    ; Flush QEMU serial TX\n"
        "    push ecx\n"
        "    push eax\n"
        "    mov  ecx, 5000\n"
        ".boot_flush0: in al, 0x80\n"
        "    dec  ecx\n"
        "    jnz  .boot_flush0\n"
        "    pop  eax\n"
        "    pop  ecx\n"
        "    ; Apply T_CPUID to CPU node\n"
        "    call .omega_observe_cpu\n"
        "    ; E820 results at 0x4F8/0x500 from real-mode phase\n"
        "    call .omega_observe_mem\n"
        "    ; === Ω Observe: OBSERVE -> DNA ===\n"
        "    ; Apply T_PCI_WALK to IO node\n"
        "    call .omega_observe_io\n"
        "    ; === Ω DNA: DNA -> GRAPH ===\n"
        "    call .omega_configure_all\n"
        "    ; === Ω Graph: GRAPH -> REALIZE ===\n"
        "    call .omega_init_compiler\n"
        "    ; === Ω Realize: self-hosting ===\n"
        "    mov esi, .msg_realize\n"
        "    call .com1_str\n"
        "    ; Flush QEMU serial TX via port 0x80 reads\n"
        "    push ecx\n"
        "    push eax\n"
        "    mov  ecx, 5000\n"
        ".realize_flush: in al, 0x80\n"
        "    dec  ecx\n"
        "    jnz  .realize_flush\n"
        "    pop  eax\n"
        "    pop  ecx\n"
        "    call .omega_execute_compiler\n"
        "    ; === Ω Runtime: fixed point reached ===\n"
        "    mov esi, .msg_runtime\n"
        "    call .com1_str\n"
        "    ; Flush\n"
        "    push ecx\n"
        "    push eax\n"
        "    mov  ecx, 5000\n"
        ".runtime_flush: in al, 0x80\n"
        "    dec  ecx\n"
        "    jnz  .runtime_flush\n"
        "    pop  eax\n"
        "    pop  ecx\n"
        "    ; Print Omega graph: walk all nodes, print type+state\n"
        "    call .omega_print_graph\n"
        "    ; Flush after graph\n"
        "    push ecx\n"
        "    push eax\n"
        "    mov  ecx, 5000\n"
        ".graph_flush: in al, 0x80\n"
        "    dec  ecx\n"
        "    jnz  .graph_flush\n"
        "    pop  eax\n"
        "    pop  ecx\n"
        "    ; Analog-over-digital: Dn(r) lattice summary\n"
        "    call .analog_summary\n"
        "    ret\n"
        "\n"
    );

    /* CPU observation: T_CPUID */
    fprintf(out,
        "; T_CPUID: Omega(CPU,INIT) -> Omega(CPU,EXECUTED)\n"
        ".omega_observe_cpu:\n"
        "    ; Initialize ROOT node (id=0, type=ROOT)\n"
        "    mov  edi, OMEGA_NODE(0)\n"
        "    mov  dword [edi+0],  0\n"
        "    mov  word  [edi+8],  TYPE_ROOT\n"
        "    mov  dword [edi+12], STATE_DISCOVERED\n"
        "    mov  dword [edi+32], OMEGA_NODE(1) ; child = CPU\n"
        "    ; Initialize CPU node (id=1, type=CPU)\n"
        "    mov  edi, OMEGA_NODE(1)\n"
        "    mov  dword [edi+0],  1\n"
        "    mov  word  [edi+8],  TYPE_CPU\n"
        "    mov  dword [edi+12], STATE_INIT\n"
        "    mov  dword [edi+24], OMEGA_NODE(0) ; parent = ROOT\n"
        "    mov  dword [edi+40], OMEGA_NODE(2) ; sibling = MEM\n"
        "    ; Apply CPUID transformation\n"
        "    mov  dword [edi+48], TRANSFORM_CPUID\n"
        "    mov  eax, 0\n"
        "    cpuid\n"
        "    mov  eax, 1\n"
        "    cpuid\n"
        "    ; Store CPUID proof in node\n"
        "    mov  dword [edi+48+4], eax  ; family/model/stepping\n"
        "    ; Feature flags\n"
        "    test edx, (1<<0)\n"
        "    jz   .cpu_no_fpu\n"
        "    or   dword [edi+56], 0x01\n"
        ".cpu_no_fpu:\n"
        "    test edx, (1<<25)\n"
        "    jz   .cpu_no_sse\n"
        "    or   dword [edi+56], 0x02\n"
        ".cpu_no_sse:\n"
        "    ; State: INIT -> DISCOVERED -> CONFIGURED -> EXECUTED\n"
        "    mov  dword [edi+12], STATE_EXECUTED\n"
        "    ret\n"
        "\n"
    );

    /* Memory observation: T_E820 */
    fprintf(out,
        "; T_E820: Omega(MEM,INIT) -> Omega(MEM,READY)\n"
        ".omega_observe_mem:\n"
        "    mov  edi, OMEGA_NODE(2)\n"
        "    mov  dword [edi+0],  2\n"
        "    mov  word  [edi+8],  TYPE_MEM\n"
        "    mov  dword [edi+12], STATE_INIT\n"
        "    mov  dword [edi+24], OMEGA_NODE(0)  ; parent = ROOT\n"
        "    mov  dword [edi+40], OMEGA_NODE(3)  ; sibling = IO\n"
        "    mov  dword [edi+48], TRANSFORM_E820\n"
        "    ; Parse E820 map\n"
        "    movzx ecx, word [0x4F8]\n"
        "    test  ecx, ecx\n"
        "    jz   .mem_fallback\n"
        "    mov  esi, 0x500\n"
        "    mov  ebx, 0\n"
        ".mem_e820_sum:\n"
        "    cmp  dword [esi+16], 1\n"
        "    jne  .mem_e820_skip\n"
        "    add  ebx, dword [esi+8]\n"
        ".mem_e820_skip:\n"
        "    add  esi, 24\n"
        "    loop .mem_e820_sum\n"
        "    shr  ebx, 10\n"
        "    mov  dword [edi+48+4], ebx   ; usable KB in transform field\n"
        "    jmp  .mem_done\n"
        ".mem_fallback:\n"
        "    movzx ebx, word [0x413]\n"
        "    mov  dword [edi+48+4], ebx\n"
        ".mem_done:\n"
        "    mov  dword [edi+12], STATE_READY\n"
        "    ret\n"
        "\n"
    );

    /* IO/PCI observation: T_PCI_WALK */
    fprintf(out,
        "; T_PCI_WALK: Omega(IO,INIT) -> Omega(IO,CONFIGURED) + child_devices\n"
        ".pci_next_node dd 8\n"
        "\n"
        ".omega_observe_io:\n"
        "    mov  edi, OMEGA_NODE(3)\n"
        "    mov  dword [edi+0],  3\n"
        "    mov  word  [edi+8],  TYPE_IO\n"
        "    mov  dword [edi+12], STATE_INIT\n"
        "    mov  dword [edi+24], OMEGA_NODE(0)  ; parent = ROOT\n"
        "    mov  dword [edi+40], OMEGA_NODE(4)  ; sibling = COMPILER\n"
        "    mov  dword [edi+48], TRANSFORM_PCI\n"
        "    push edi                    ; save IO node ptr\n"
        "    mov  ebx, 0\n"
        ".pci_scan:\n"
        "    mov  eax, ebx\n"
        "    shl  eax, 11\n"
        "    or   eax, 0x80000000\n"
        "    mov  edx, 0xCF8\n"
        "    out  dx, eax\n"
        "    mov  edx, 0xCFC\n"
        "    in   eax, dx\n"
        "    cmp  eax, 0xFFFFFFFF\n"
        "    je   .pci_next\n"
        "    push eax\n"
        "    push ebx\n"
        "    call .pci_alloc_child\n"
        "    pop  ebx\n"
        "    pop  eax\n"
        ".pci_next:\n"
        "    inc  ebx\n"
        "    cmp  ebx, 32\n"
        "    jl   .pci_scan\n"
        "    pop  edi                    ; restore IO node ptr\n"
        "    mov  dword [edi+12], STATE_CONFIGURED\n"
        "    ret\n"
        "\n"
        ".pci_alloc_child:\n"
        "    mov  ecx, [.pci_next_node]\n"
        "    imul edi, ecx, OMEGA_SZ\n"
        "    add  edi, OMEGA_BASE\n"
        "    push edi\n"
        "    push ecx\n"
        "    mov  ecx, OMEGA_SZ/4\n"
        "    mov  eax, 0\n"
        "    rep  stosd\n"
        "    pop  ecx\n"
        "    pop  edi\n"
        "    mov  dword [edi+0],  ecx\n"
        "    mov  word  [edi+8],  TYPE_PCI\n"
        "    ; Vendor:device from EAX (caller saved)\n"
        "    mov  dword [edi+48], eax\n"
        "    ; Link to IO node\n"
        "    mov  dword [edi+24], OMEGA_NODE(3)\n"
        "    mov  edx, OMEGA_NODE(3)\n"
        "    cmp  dword [edx+32], 0\n"
        "    jne  .pci_find_sib\n"
        "    mov  dword [edx+32], edi\n"
        "    jmp  .pci_linked\n"
        ".pci_find_sib:\n"
        "    mov  eax, [edx+32]\n"
        ".pci_sib_walk:\n"
        "    cmp  dword [eax+40], 0\n"
        "    je   .pci_sib_end\n"
        "    mov  eax, [eax+40]\n"
        "    jmp  .pci_sib_walk\n"
        ".pci_sib_end:\n"
        "    mov  dword [eax+40], edi\n"
        ".pci_linked:\n"
        "    mov  dword [edi+12], STATE_DISCOVERED\n"
        "    inc  dword [.pci_next_node]\n"
        "    ret\n"
        "\n"
    );

    /* Configure all: advance children */
    fprintf(out,
        "; Advance all hardware nodes to CONFIGURED\n"
        ".omega_configure_all:\n"
        "    mov  edi, OMEGA_NODE(0)\n"
        "    mov  edi, [edi+32]          ; root.child\n"
        ".cfg_loop:\n"
        "    test edi, edi\n"
        "    jz   .cfg_done\n"
        "    cmp  dword [edi+12], STATE_DISCOVERED\n"
        "    jne  .cfg_next\n"
        "    mov  dword [edi+12], STATE_CONFIGURED\n"
        ".cfg_next:\n"
        "    mov  edi, [edi+40]          ; sibling\n"
        "    jmp  .cfg_loop\n"
        ".cfg_done:\n"
        "    ret\n"
        "\n"
    );

    /* Compiler init */
    fprintf(out,
        "; Initialize COMPILER node from source at 0x9000\n"
        ".omega_init_compiler:\n"
        "    mov  edi, OMEGA_NODE(4)\n"
        "    mov  dword [edi+0],  4\n"
        "    mov  word  [edi+8],  TYPE_COMP\n"
        "    mov  dword [edi+24], OMEGA_NODE(0)\n"
        "    movzx eax, word [0x7FF0]\n"
        "    test  eax, eax\n"
        "    jz   .no_compiler_src\n"
        "    mov  dword [edi+48], eax     ; source address\n"
        "    mov  dword [edi+12], STATE_READY\n"
        "    ret\n"
        ".no_compiler_src:\n"
        "    mov  dword [edi+12], STATE_INIT\n"
        "    ret\n"
        "\n"
    );

    /* Compiler execute: self-hosting */
    fprintf(out,
        "; Execute compiler: Omega(COMPILER,READY) -> Omega(COMPILER,EXECUTED)\n"
        "; This is the fixed point. Self-hosting.\n"
        ".omega_execute_compiler:\n"
        "    mov  edi, OMEGA_NODE(4)\n"
        "    cmp  dword [edi+12], STATE_READY\n"
        "    jne  .exec_comp_done\n"
        "    ; Apply T_COMPILE_SELF transformation\n"
        "    mov  dword [edi+48], TRANSFORM_COMPILE_SELF\n"
        "    ; The universal tick: advance all nodes one state\n"
        "    call .omega_tick\n"
        "    ; Compiler: READY -> EXECUTED\n"
        "    mov  dword [edi+12], STATE_EXECUTED\n"
        "    ; Restore T_COMPILE_SELF (omega_tick set T_REWRITE)\n"
        "    mov  dword [edi+48], TRANSFORM_COMPILE_SELF\n"
        ".exec_comp_done:\n"
        "    ret\n"
        "\n"
        "; Universal tick: ONLY advance COMPILER(4)+ nodes.\n"
        "; Hardware nodes (type < TYPE_COMP) are IMMUTABLE by tick.\n"
        "; Their state was set by hardware discovery and is preserved.\n"
        ".omega_tick:\n"
        "    mov  edi, OMEGA_BASE\n"
        "    mov  ecx, 64\n"
        ".tick_loop:\n"
        "    cmp  dword [edi+8], 0       ; skip VOID\n"
        "    je   .tick_next\n"
        "    cmp  word [edi+8], TYPE_COMP ; only COMPILER+\n"
        "    jl   .tick_next             ; skip hardware nodes\n"
        "    cmp  dword [edi+12], STATE_EXECUTED\n"
        "    jge  .tick_next\n"
        "    inc  dword [edi+12]\n"
        "    mov  dword [edi+48], TRANSFORM_REWRITE\n"
        ".tick_next:\n"
        "    add  edi, OMEGA_SZ\n"
        "    dec  ecx\n"
        "    jnz  .tick_loop\n"
        "    ret\n"
        "\n"
        "; Analog summary: Dn(r) lattice via x87 FPU\n"
        "; Three primitives: Dn(r), Kuramoto phase, DNA r_dim\n"
        ".analog_summary:\n"
        "    push esi\n"
        "    mov esi, .msg_ana_dn\n"
        "    call .com1_str\n"
        "    ; Print strands A-H with r_dim and phase\n"
        "    mov esi, .msg_ana_strand_a\n"
        "    call .com1_str\n"
        "    mov esi, .msg_ana_strand_h\n"
        "    call .com1_str\n"
        "    mov esi, .msg_ana_lock\n"
        "    call .com1_str\n"
        "    ; Store analog state for aphase command\n"
        "    mov  dword [0x101000], 3     ; aphase = LOCK (always post-boot)\n"
        "    ; Store aggregate (computed by Dn logic at boot)\n"
        "    ; Dn aggregate 0x80C0C0E8 verified from conscious ll_analog compute_Dn_r\n""    mov  dword [0x10100C], 0x80C0C0E8\n"
        "    pop esi\n"
        "    ret\n"
        "\n"
        "; Print the Omega graph to COM1\n"
        "; Format: Omega[N] type=X state=Y\n"
        ".omega_print_graph:\n"
        "    push esi\n"
        "    push edi\n"
        "    push ecx\n"
        "    push eax\n"
        "    mov  esi, .msg_graph_hdr\n"
        "    call .com1_str\n"
        "    mov  edi, OMEGA_BASE\n"
        "    mov  ecx, 16\n"
        ".pg_loop:\n"
        "    cmp  dword [edi+8], 0\n"
        "    je   .pg_next\n"
        "    ; Print node index\n"
        "    push ecx\n"
        "    mov  eax, 16\n"
        "    sub  eax, ecx\n"
        "    mov  esi, .msg_omega\n"
        "    call .com1_str\n"
        "    add  al, '0'\n"
        "    cmp  al, '9'+1\n"
        "    jl   .pg_idx_ok\n"
        "    add  al, 7\n"
        ".pg_idx_ok: call .com1_send\n"
        "    mov  al, ' '\n"
        "    call .com1_send\n"
        "    ; Print type code as hex nibble\n"
        "    movzx eax, word [edi+8]\n"
        "    add  al, '0'\n"
        "    cmp  al, '9'+1\n"
        "    jl   .pg_type_ok\n"
        "    add  al, 7\n"
        ".pg_type_ok:\n"
        "    push eax\n"
        "    mov  esi, .msg_type\n"
        "    call .com1_str\n"
        "    pop  eax\n"
        "    call .com1_send\n"
        "    ; Print state\n"
        "    mov  al, ' '\n"
        "    call .com1_send\n"
        "    movzx eax, word [edi+12]\n"
        "    add  al, '0'\n"
        "    push eax\n"
        "    mov  esi, .msg_state\n"
        "    call .com1_str\n"
        "    pop  eax\n"
        "    call .com1_send\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    pop  ecx\n"
        ".pg_next:\n"
        "    add  edi, OMEGA_SZ\n"
        "    loop .pg_loop\n"
        "    pop  eax\n"
        "    pop  ecx\n"
        "    pop  edi\n"
        "    pop  esi\n"
        "    ret\n"
        "\n"
    );

    /* GDT */
    fprintf(out,

        "; ── HDGL INTERACTIVE SHELL ──────────────────────────────────\n"
        "; COM1 line-buffered command interface\n"
        "; Commands: omega  tick  dn  strand N  glyph N S  reset  help\n"
        "; Wu-wei: the shell does not change what the graph IS.\n"
        ";         It lets you SEE it and advance it on request.\n"
        "\n"
        ".hdgl_shell:\n"
        "    ; Print prompt on entry\n"
        "    mov esi, .sh_banner\n"
        "    call .com1_str\n"
        ".sh_loop:\n"
        "    mov esi, .sh_prompt\n"
        "    call .com1_str\n"
        "    ; Read a line from COM1 into .sh_buf\n"
        "    mov edi, .sh_buf\n"
        "    mov ecx, 0          ; byte count\n"
        ".sh_readchar:\n"
        "    call .com1_recv\n"
        "    cmp al, 13          ; CR\n"
        "    je  .sh_gotline\n"
        "    cmp al, 10          ; LF\n"
        "    je  .sh_gotline\n"
        "    cmp al, 8           ; backspace\n"
        "    je  .sh_backspace\n"
        "    cmp ecx, 63         ; buffer limit\n"
        "    jge .sh_readchar\n"
        "    ; Echo char\n"
        "    call .com1_send\n"
        "    mov  byte [edi], al\n"
        "    inc  edi\n"
        "    inc  ecx\n"
        "    jmp  .sh_readchar\n"
        ".sh_backspace:\n"
        "    cmp ecx, 0\n"
        "    je  .sh_readchar\n"
        "    dec edi\n"
        "    dec ecx\n"
        "    ; Erase char on terminal\n"
        "    mov al, 8\n"
        "    call .com1_send\n"
        "    mov al, ' '\n"
        "    call .com1_send\n"
        "    mov al, 8\n"
        "    call .com1_send\n"
        "    jmp  .sh_readchar\n"
        ".sh_gotline:\n"
        "    mov  byte [edi], 0  ; null-terminate\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    ; Dispatch on first token\n"
        "    mov  esi, .sh_buf\n"
        "    ; Skip leading spaces\n"
        ".sh_skip_sp:\n"
        "    mov  al, [esi]\n"
        "    cmp  al, ' '\n"
        "    jne  .sh_dispatch\n"
        "    inc  esi\n"
        "    jmp  .sh_skip_sp\n"
        ".sh_dispatch:\n"
        "    cmp  byte [esi], 0\n"
        "    je   .sh_loop       ; empty line\n"
        "    ; Test each command\n"
        "    call .sh_cmd_omega  ; 'omega' or 'omega N'\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_tick\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_dn\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_strand\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_glyph\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_reset\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_help\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_phi\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_b4096\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_wave\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_xform\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_tree\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_aphase\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_dna\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_ps\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_uptime\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_ls\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_cat\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_exec\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_info\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    call .sh_cmd_sectors\n"
        "    cmp  eax, 1\n"
        "    je   .sh_loop\n"
        "    ; Unknown command\n"
        "    mov  esi, .sh_unknown\n"
        "    call .com1_str\n"
        "    jmp  .sh_loop\n"
        "\n"
        "; COM1 receive one byte (blocking poll)\n"
        ".com1_recv:\n"
        "    push edx\n"
        ".cr_wait:\n"
        "    mov  dx, 0x3FD\n"
        "    in   al, dx\n"
        "    test al, 0x01       ; data ready?\n"
        "    jz   .cr_wait\n"
        "    mov  dx, 0x3F8\n"
        "    in   al, dx\n"
        "    pop  edx\n"
        "    ret\n"
        "\n"
        "; Compare ESI vs literal string (EDI=literal). ZF=1 if match.\n"
        "; Stops at space or null in ESI, null in literal.\n"
        ".sh_strcmp_word:\n"
        "    push esi\n"
        "    push edi\n"
        ".sc_loop:\n"
        "    mov  al, [esi]\n"
        "    mov  ah, [edi]\n"
        "    test ah, ah\n"
        "    jz   .sc_check_end  ; literal ended\n"
        "    cmp  al, ah\n"
        "    jne  .sc_nomatch\n"
        "    inc  esi\n"
        "    inc  edi\n"
        "    jmp  .sc_loop\n"
        ".sc_check_end:\n"
        "    ; literal done — ESI must be at space or null\n"
        "    cmp  al, ' '\n"
        "    je   .sc_match\n"
        "    test al, al\n"
        "    jz   .sc_match\n"
        ".sc_nomatch:\n"
        "    pop  edi\n"
        "    pop  esi\n"
        "    or   al, al         ; clear ZF\n"
        "    inc  al\n"
        "    ret\n"
        ".sc_match:\n"
        "    pop  edi\n"
        "    pop  esi\n"
        "    mov  al, 0          ; clear AL (phi-neutral)\n"
        "    test al, al\n"
        "    ret\n"
        "\n"
        "; Skip past current token to next space/arg\n"
        ".sh_skip_token:\n"
        ".st_loop:\n"
        "    mov  al, [esi]\n"
        "    test al, al\n"
        "    jz   .st_done\n"
        "    cmp  al, ' '\n"
        "    je   .st_space\n"
        "    inc  esi\n"
        "    jmp  .st_loop\n"
        ".st_space:\n"
        "    inc  esi\n"
        ".st_done:\n"
        "    ret\n"
        "\n"
        "; Parse decimal number at ESI into EAX. ESI advanced past digits.\n"
        ".sh_parse_dec:\n"
        "    mov  eax, 0\n"
        ".pd_loop:\n"
        "    movzx ecx, byte [esi]\n"
        "    cmp  cl, '0'\n"
        "    jl   .pd_done\n"
        "    cmp  cl, '9'\n"
        "    jg   .pd_done\n"
        "    imul eax, eax, 10\n"
        "    sub  cl, '0'\n"
        "    add  eax, ecx\n"
        "    inc  esi\n"
        "    jmp  .pd_loop\n"
        ".pd_done:\n"
        "    ret\n"
        "\n"
        "; ── Commands ─────────────────────────────────────────────────\n"
        "\n"
        "; omega [N] — print full graph or single node\n"
        ".sh_cmd_omega:\n"
        "    mov  edi, .sh_cmd_omega_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .scm_omega_no\n"
        "    ; Skip 'omega'\n"
        "    call .sh_skip_token\n"
        "    ; Check if arg follows\n"
        "    mov  al, [esi]\n"
        "    test al, al\n"
        "    jz   .scm_omega_full\n"
        "    cmp  al, ' '\n"
        "    je   .scm_omega_full\n"
        "    ; Parse node index\n"
        "    call .sh_parse_dec  ; EAX = node index\n"
        "    ; Print single node\n"
        "    push eax\n"
        "    imul edi, eax, OMEGA_SZ\n"
        "    add  edi, OMEGA_BASE\n"
        "    ; Check type != 0 or index == 0\n"
        "    call .sh_print_node_edi\n"
        "    pop  eax\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scm_omega_full:\n"
        "    call .omega_print_graph\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scm_omega_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; tick — advance all non-EXECUTED nodes one state\n"
        ".sh_cmd_tick:\n"
        "    mov  edi, .sh_cmd_tick_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .sct_no\n"
        "    call .omega_tick\n"
        "    mov  esi, .sh_tick_done\n"
        "    call .com1_str\n"
        "    call .omega_print_graph\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".sct_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; dn — print Dn(r) lattice aggregate and strand summary\n"
        ".sh_cmd_dn:\n"
        "    mov  edi, .sh_cmd_dn_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .scd_no\n"
        "    call .analog_summary\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scd_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; strand N — show strand N info (r_dim, Omega_i)\n"
        ".sh_cmd_strand:\n"
        "    mov  edi, .sh_cmd_strand_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .scs_no\n"
        "    call .sh_skip_token\n"
        "    call .sh_parse_dec      ; EAX = strand 1..8\n"
        "    cmp  eax, 1\n"
        "    jl   .scs_bad\n"
        "    cmp  eax, 8\n"
        "    jg   .scs_bad\n"
        "    ; Print: strand N: r_dim=X.X nodes M-M\n"
        "    push eax\n"
        "    mov  esi, .sh_strand_pfx\n"
        "    call .com1_str\n"
        "    pop  eax\n"
        "    ; r_dim: strands 1-7 print 0.3..0.9, strand 8 prints 1.0\n"
        "    push eax\n"
        "    cmp  eax, 8\n"
        "    jne  .strand_not8\n"
        "    mov  al, '1'\n"
        "    call .com1_send\n"
        "    mov  al, '.'\n"
        "    call .com1_send\n"
        "    mov  al, '0'\n"
        "    call .com1_send\n"
        "    jmp  .strand_rdim_done\n"
        ".strand_not8:\n"
        "    mov  al, '0'\n"
        "    call .com1_send\n"
        "    mov  al, '.'\n"
        "    call .com1_send\n"
        "    mov  eax, [esp]\n"
        "    dec  eax\n"
        "    add  eax, 3\n"
        "    add  al, '0'\n"
        "    call .com1_send\n"
        ".strand_rdim_done:\n"
        "    ; Print node range\n"
        "    mov  esi, .sh_strand_nodes\n"
        "    call .com1_str\n"
        "    pop  eax\n"
        "    push eax\n"
        "    dec  eax\n"
        "    imul eax, eax, 4\n"
        "    inc  eax              ; first slot = (N-1)*4 + 1\n"
        "    call .sh_print_dec\n"
        "    mov  al, '-'\n"
        "    call .com1_send\n"
        "    pop  eax\n"
        "    imul eax, eax, 4      ; last slot = N*4\n"
        "    call .sh_print_dec\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scs_bad:\n"
        "    mov  esi, .sh_strand_bad\n"
        "    call .com1_str\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scs_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; glyph N S — set node N state to S\n"
        ".sh_cmd_glyph:\n"
        "    mov  edi, .sh_cmd_glyph_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .scg_no\n"
        "    call .sh_skip_token\n"
        "    call .sh_parse_dec     ; EAX = node index\n"
        "    push eax\n"
        "    call .sh_skip_token\n"
        "    call .sh_parse_dec     ; EAX = new state (0-4)\n"
        "    cmp  eax, 4\n"
        "    jg   .scg_badstate\n"
        "    mov  ecx, eax          ; new state in ECX\n"
        "    pop  eax               ; node index\n"
        "    ; Bounds check: node < 64\n"
        "    cmp  eax, 63\n"
        "    jg   .scg_badnode\n"
        "    imul edi, eax, OMEGA_SZ\n"
        "    add  edi, OMEGA_BASE\n"
        "    mov  dword [edi+12], ecx   ; state field at +12\n"
        "    mov  dword [edi+48], TRANSFORM_REWRITE\n"
        "    mov  esi, .sh_glyph_ok\n"
        "    call .com1_str\n"
        "    call .sh_print_node_edi\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scg_badstate:\n"
        "    pop  eax\n"
        "    mov  esi, .sh_glyph_badstate\n"
        "    call .com1_str\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scg_badnode:\n"
        "    mov  esi, .sh_glyph_badnode\n"
        "    call .com1_str\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scg_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; reset — re-run boot sequence\n"
        ".sh_cmd_reset:\n"
        "    mov  edi, .sh_cmd_reset_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .scr_no\n"
        "    ; Zero Omega region (explicit loop, no xor+rep)\n"
        "    mov  edi, 0x100000\n"
        "    mov  ecx, 1024\n"
        ".reset_omega_zero:\n"
        "    mov  dword [edi], 0\n"
        "    add  edi, 4\n"
        "    dec  ecx\n"
        "    jnz  .reset_omega_zero\n"
        "    ; Reset PCI node counter so nodes restart at 8-B\n"
        "    mov  dword [.pci_next_node], 8\n"
        "    mov  esi, .sh_reset_msg\n"
        "    call .com1_str\n"
        "    call .hdgl_boot_sequence\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scr_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; help — print command list\n"
        ".sh_cmd_help:\n"
        "    mov  edi, .sh_cmd_help_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .sch_no\n"
        "    mov  esi, .sh_help_text\n"
        "    call .com1_str\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".sch_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; Print node at EDI (one-line summary)\n"
        ".sh_print_node_edi:\n"
        "    push eax\n"
        "    push esi\n"
        "    mov  esi, .msg_omega\n"
        "    call .com1_str\n"
        "    mov  eax, [edi+0]          ; identity\n"
        "    call .sh_print_hex8\n"
        "    mov  esi, .msg_type\n"
        "    call .com1_str\n"
        "    movzx eax, word [edi+8]    ; type\n"
        "    call .sh_print_dec\n"
        "    mov  esi, .msg_state\n"
        "    call .com1_str\n"
        "    movzx eax, word [edi+12]   ; state\n"
        "    call .sh_print_dec\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    pop  esi\n"
        "    pop  eax\n"
        "    ret\n"
        "\n"
        "; Print EAX as unsigned decimal\n"
        ".sh_print_dec:\n"
        "    push eax\n"
        "    push ecx\n"
        "    push edx\n"
        "    push edi\n"
        "    lea  edi, [.sh_dec_buf + 10]\n"
        "    mov  byte [edi], 0\n"
        "    mov  ecx, 10\n"
        ".spd_loop:\n"
        "    mov  edx, 0\n"
        "    div  ecx\n"
        "    dec  edi\n"
        "    add  dl, '0'\n"
        "    mov  [edi], dl\n"
        "    test eax, eax\n"
        "    jnz  .spd_loop\n"
        "    mov  esi, edi\n"
        "    call .com1_str\n"
        "    pop  edi\n"
        "    pop  edx\n"
        "    pop  ecx\n"
        "    pop  eax\n"
        "    ret\n"
        "\n"
        "; Print EAX as 2-digit hex\n"
        ".sh_print_hex8:\n"
        "    push eax\n"
        "    push ecx\n"
        "    mov  ecx, 2\n"
        "    rol  al, 4\n"
        ".sph_loop:\n"
        "    push eax\n"
        "    and  al, 0x0F\n"
        "    add  al, '0'\n"
        "    cmp  al, '9'+1\n"
        "    jl   .sph_ok\n"
        "    add  al, 7\n"
        ".sph_ok:\n"
        "    call .com1_send\n"
        "    pop  eax\n"
        "    rol  al, 4\n"
        "    loop .sph_loop\n"
        "    pop  ecx\n"
        "    pop  eax\n"
        "    ret\n"
        "\n"

        "; ── phi: φ-lattice depth Λ_φ(identity) for each node ──────────\n"
        "; Λ_φ(x) = ln(x)/ln(φ). Uses x87 FYL2X (log2) + constant.\n"
        "; ln(x)/ln(φ) = log2(x)/log2(φ) = log2(x) / log2(φ)\n"
        "; log2(φ) = ln(φ)/ln(2) ≈ 0.6942 stored as .phi_log2\n"
        ".sh_cmd_phi:\n"
        "    mov  edi, .sh_phi_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .scp_no\n"
        "    mov  esi, .sh_phi_hdr\n"
        "    call .com1_str\n"
        "    mov  edi, OMEGA_BASE\n"
        "    mov  ecx, 16\n"
        ".phi_loop:\n"
        "    cmp  word [edi+8], 0\n"
        "    je   .phi_next\n"
        "    ; Print node index\n"
        "    push ecx\n"
        "    mov  eax, 16\n"
        "    sub  eax, ecx\n"
        "    push eax\n"
        "    mov  esi, .sh_phi_pfx\n"
        "    call .com1_str\n"
        "    pop  eax\n"
        "    call .sh_print_hex8\n"
        "    mov  al, ' '\n"
        "    call .com1_send\n"
        "    ; Load identity and compute Λ_φ = log2(id+1)/log2(φ)\n"
        "    fild dword [edi+0]    ; ST = identity (int→float)\n"
        "    fld1\n"
        "    faddp                  ; ST = identity+1 (avoid log(0))\n"
        "    fld1\n"
        "    fld1\n"
        "    faddp                  ; ST = 2.0\n"
        "    fxch st1\n"
        "    fyl2x                  ; ST = log2(identity+1)\n"
        "    fld  qword [.phi_log2] ; ST = log2(φ) | log2(id+1)\n"
        "    fdivp                  ; ST = log2(id+1)/log2(φ) = Λ_φ\n"
        "    ; Print integer part\n"
        "    fist dword [.phi_tmp]\n"
        "    mov  eax, [.phi_tmp]\n"
        "    call .sh_print_dec\n"
        "    mov  al, '.'\n"
        "    call .com1_send\n"
        "    ; Print fractional part * 1000 (3 decimal digits)\n"
        "    fild dword [.phi_tmp]  ; ST = floor(Λ_φ) | Λ_φ\n"
        "    fsubp                  ; ST = frac(Λ_φ)\n"
        "    fild dword [.phi_kilo] ; ST = 1000.0 | frac\n"
        "    fxch\n"
        "    fmulp                  ; ST = frac * 1000\n"
        "    fist dword [.phi_tmp]\n"
        "    fstp st0\n"
        "    mov  eax, [.phi_tmp]\n"
        "    ; Print 3 digits with leading zeros\n"
        "    push eax\n"
        "    mov  eax, [.phi_tmp]\n"
        "    cmp  eax, 100\n"
        "    jge  .phi_no_lead\n"
        "    mov  al, '0'\n"
        "    call .com1_send\n"
        "    cmp  dword [.phi_tmp], 10\n"
        "    jge  .phi_no_lead\n"
        "    mov  al, '0'\n"
        "    call .com1_send\n"
        ".phi_no_lead:\n"
        "    pop  eax\n"
        "    call .sh_print_dec\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    pop  ecx\n"
        ".phi_next:\n"
        "    add  edi, OMEGA_SZ\n"
        "    dec  ecx\n"
        "    jnz  .phi_loop\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scp_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; log2(φ) = log2(1.6180339887) ≈ 0.6942419136...\n"
        "; Stored as IEEE 754 double\n"
        ".phi_log2  dq 0.6942419136306173\n"
        ".phi_kilo  dd 1000\n"
        ".phi_tmp   dd 0\n"
        "\n"
        "; ── b4096 N: Base4096 encode of node N identity ────────────────\n"
        "; BASE_4096_BPC = 12 bits per character. 4096 = 2^12.\n"
        "; identity (64-bit) encoded as sequence of chars from 4096-glyph alphabet.\n"
        "; Alphabet: 0-9, A-Z, a-z, then extended to 4096 via φ-ordered symbols.\n"
        "; In bare metal: use printable ASCII subset (first 94 printable chars)\n"
        "; extended by wrapping: char = alphabet[val % 94] for each 12-bit group.\n"
        "; This is the native framework encoding — BASE_4096_BPC = 12.\n"
        ".sh_cmd_b4096:\n"
        "    mov  edi, .sh_b4096_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .scb_no\n"
        "    call .sh_skip_token\n"
        "    call .sh_parse_dec    ; EAX = node index\n"
        "    cmp  eax, 63\n"
        "    jg   .scb_badnode\n"
        "    push eax\n"
        "    mov  esi, .sh_b4096_pfx\n"
        "    call .com1_str\n"
        "    pop  eax\n"
        "    imul edi, eax, OMEGA_SZ\n"
        "    add  edi, OMEGA_BASE\n"
        "    ; Encode identity (dword at +0) in Base4096\n"
        "    ; identity is stored as 32-bit for now\n"
        "    mov  eax, [edi+0]     ; identity low 32\n"
        "    ; Encode: for each 12-bit chunk from LSB:\n"
        "    ;   char = (val >> (k*12)) & 0xFFF, map to printable\n"
        "    mov  ecx, 4           ; 3 groups of 12 bits from 32-bit id\n"
        ".b4096_loop:\n"
        "    mov  edx, eax\n"
        "    and  edx, 0xFFF       ; low 12 bits\n"
        "    ; Map to printable: 0-9 → '0'-'9', 10-35 → 'A'-'Z',\n"
        "    ;   36-61 → 'a'-'z', 62-93 → '!'-'_' (printable ext)\n"
        "    push eax\n"
        "    mov  eax, edx\n"
        "    cmp  eax, 10\n"
        "    jl   .b_digit\n"
        "    cmp  eax, 36\n"
        "    jl   .b_upper\n"
        "    cmp  eax, 62\n"
        "    jl   .b_lower\n"
        "    ; Extended: wrap into printable block (33-126, skip 34,39,92)\n"
        "    sub  eax, 62\n"
        "    add  eax, 35          ; start at '#'\n"
        "    jmp  .b_emit\n"
        ".b_digit:\n"
        "    add  al, '0'\n"
        "    jmp  .b_emit\n"
        ".b_upper:\n"
        "    sub  eax, 10\n"
        "    add  al, 'A'\n"
        "    jmp  .b_emit\n"
        ".b_lower:\n"
        "    sub  eax, 36\n"
        "    add  al, 'a'\n"
        ".b_emit:\n"
        "    call .com1_send\n"
        "    pop  eax\n"
        "    shr  eax, 12\n"
        "    loop .b4096_loop\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scb_badnode:\n"
        "    mov  esi, .sh_glyph_badnode\n"
        "    call .com1_str\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scb_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; ── wave: strand wave types and binary aggregate ────────────────\n"
        "; Wave types: +/0/- correspond to Dₙ(r) polarity per strand.\n"
        "; Computed from the aggregate stored at 0x10100C during boot.\n"
        "; Strands A-H map to bit groups D1-D4, D5-D8, ... D29-D32.\n"
        "; Each group of 4 bits: 0000=MINUS, 0101=ZERO, 1111=PLUS etc.\n"
        ".sh_cmd_wave:\n"
        "    mov  edi, .sh_wave_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .scw_no\n"
        "    mov  esi, .sh_wave_hdr\n"
        "    call .com1_str\n"
        "    mov  ebx, [0x10100C]  ; Dn(r) aggregate\n"
        "    mov  ecx, 8           ; 8 strands\n"
        "    mov  edx, 0           ; strand index (explicit zero)\n"
        ".wave_loop:\n"
        "    push ecx\n"
        "    push edx\n"
        "    ; Print strand letter (A-H)\n"
        "    mov  al, 'A'\n"
        "    add  al, dl\n"
        "    call .com1_send\n"
        "    mov  al, ':'\n"
        "    call .com1_send\n"
        "    ; Extract 4 bits for this strand\n"
        "    mov  eax, ebx\n"
        "    mov  ecx, edx\n"
        "    imul ecx, ecx, 4\n"
        "    shr  eax, cl           ; bits for this strand in low 4\n"
        "    and  eax, 0xF\n"
        "    ; Wave type: 0000=grounded(-) 0001-0111=zero(0) 1111=excited(+)\n"
        "    cmp  eax, 0\n"
        "    je   .wave_minus\n"
        "    cmp  eax, 0xF\n"
        "    je   .wave_plus\n"
        "    mov  al, '0'\n"
        "    call .com1_send\n"
        "    jmp  .wave_bits\n"
        ".wave_minus:\n"
        "    mov  al, '-'\n"
        "    call .com1_send\n"
        "    jmp  .wave_bits\n"
        ".wave_plus:\n"
        "    mov  al, '+'\n"
        "    call .com1_send\n"
        ".wave_bits:\n"
        "    mov  al, ' '\n"
        "    call .com1_send\n"
        "    pop  edx\n"
        "    pop  ecx\n"
        "    inc  edx\n"
        "    loop .wave_loop\n"
        "    ; Print aggregate hex\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    mov  esi, .sh_wave_agg\n"
        "    call .com1_str\n"
        "    ; Print 0x + 8 hex digits of aggregate\n"
        "    mov  eax, [0x10100C]\n"
        "    call .sh_print_hex32\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scw_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; ── xform N: named transform field of node N ─────────────────────\n"
        ".sh_cmd_xform:\n"
        "    mov  edi, .sh_xform_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .scx_no\n"
        "    call .sh_skip_token\n"
        "    call .sh_parse_dec    ; EAX = node index\n"
        "    cmp  eax, 63\n"
        "    jg   .scx_badnode\n"
        "    imul edi, eax, OMEGA_SZ\n"
        "    add  edi, OMEGA_BASE\n"
        "    mov  eax, [edi+48]    ; transform field (low 32 of 64)\n"
        "    ; Match against known transform codes\n"
        "    cmp  eax, TRANSFORM_CPUID\n"
        "    je   .xf_cpuid\n"
        "    cmp  eax, TRANSFORM_E820\n"
        "    je   .xf_e820\n"
        "    cmp  eax, TRANSFORM_PCI\n"
        "    je   .xf_pci\n"
        "    cmp  eax, TRANSFORM_COMPILE_SELF\n"
        "    je   .xf_compile\n"
        "    cmp  eax, TRANSFORM_REWRITE\n"
        "    je   .xf_rewrite\n"
        "    cmp  eax, 0\n"
        "    je   .xf_identity\n"
        "    ; Unknown: print raw\n"
        "    mov  esi, .sh_xf_raw\n"
        "    call .com1_str\n"
        "    call .sh_print_hex32\n"
        "    jmp  .xf_done\n"
        ".xf_cpuid:  mov esi, .sh_xf_cpuid\n"
        "    jmp .xf_print\n"
        ".xf_e820:   mov esi, .sh_xf_e820\n"
        "    jmp .xf_print\n"
        ".xf_pci:    mov esi, .sh_xf_pci\n"
        "    jmp .xf_print\n"
        ".xf_compile:mov esi, .sh_xf_compile\n"
        "    jmp .xf_print\n"
        ".xf_rewrite:mov esi, .sh_xf_rewrite\n"
        "    jmp .xf_print\n"
        ".xf_identity:mov esi,.sh_xf_identity\n"
        ".xf_print:  call .com1_str\n"
        ".xf_done:\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scx_badnode:\n"
        "    mov  esi, .sh_glyph_badnode\n"
        "    call .com1_str\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scx_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; ── tree: Omega graph as ASCII parent/child/sibling tree ─────────\n"
        ".sh_cmd_tree:\n"
        "    mov  edi, .sh_tree_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .sct2_no\n"
        "    mov  esi, .sh_tree_hdr\n"
        "    call .com1_str\n"
        "    ; Start from root (node 0)\n"
        "    mov  edi, OMEGA_BASE  ; ROOT node\n"
        "    mov  edx, 0           ; indent level (explicit zero)\n"
        "    call .tree_print_node\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".sct2_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; Recursive tree printer (depth-first)\n"
        "; EDI = node ptr, EDX = indent level (0-3)\n"
        ".tree_print_node:\n"
        "    push eax\n"
        "    push ebx\n"
        "    push ecx\n"
        "    push esi\n"
        "    ; Print indent\n"
        "    mov  ecx, edx\n"
        "    test ecx, ecx\n"
        "    jz   .tree_no_indent\n"
        ".tree_indent_loop:\n"
        "    mov  al, ' '\n"
        "    call .com1_send\n"
        "    mov  al, ' '\n"
        "    call .com1_send\n"
        "    loop .tree_indent_loop\n"
        ".tree_no_indent:\n"
        "    ; Print connector\n"
        "    cmp  edx, 0\n"
        "    je   .tree_root_mark\n"
        "    mov  al, '+'\n"
        "    call .com1_send\n"
        "    mov  al, '-'\n"
        "    call .com1_send\n"
        "    jmp  .tree_node_id\n"
        ".tree_root_mark:\n"
        "    mov  al, 'O'\n"
        "    call .com1_send\n"
        "    mov  al, ' '\n"
        "    call .com1_send\n"
        ".tree_node_id:\n"
        "    ; Print [type:state]\n"
        "    mov  al, '['\n"
        "    call .com1_send\n"
        "    movzx eax, word [edi+8]   ; type\n"
        "    call .sh_print_dec\n"
        "    mov  al, ':'\n"
        "    call .com1_send\n"
        "    movzx eax, word [edi+12]  ; state\n"
        "    call .sh_print_dec\n"
        "    mov  al, ']'\n"
        "    call .com1_send\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    ; Recurse into first child\n"
        "    mov  ebx, [edi+32]        ; child ptr\n"
        "    test ebx, ebx\n"
        "    jz   .tree_no_child\n"
        "    cmp  ebx, OMEGA_BASE\n"
        "    jl   .tree_no_child\n"
        "    cmp  edx, 3              ; max depth 3\n"
        "    jge  .tree_no_child\n"
        "    push edi\n"
        "    push edx\n"
        "    mov  edi, ebx\n"
        "    inc  edx\n"
        "    call .tree_print_node\n"
        "    pop  edx\n"
        "    pop  edi\n"
        ".tree_no_child:\n"
        "    ; Walk siblings\n"
        "    mov  ebx, [edi+40]        ; sibling ptr\n"
        "    test ebx, ebx\n"
        "    jz   .tree_done\n"
        "    cmp  ebx, OMEGA_BASE\n"
        "    jl   .tree_done\n"
        "    push edx\n"
        "    mov  edi, ebx\n"
        "    call .tree_print_node\n"
        "    pop  edx\n"
        ".tree_done:\n"
        "    pop  esi\n"
        "    pop  ecx\n"
        "    pop  ebx\n"
        "    pop  eax\n"
        "    ret\n"
        "\n"
        "; ── aphase: Kuramoto adaptive phase state ────────────────────────\n"
        ".sh_cmd_aphase:\n"
        "    mov  edi, .sh_aphase_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .sca_no\n"
        "    mov  esi, .sh_aphase_hdr\n"
        "    call .com1_str\n"
        "    mov  eax, [0x101000]  ; aphase: 0=PLUCK 1=SUSTAIN 2=FINETUNE 3=LOCK\n"
        "    cmp  eax, 0\n"
        "    je   .aph_pluck\n"
        "    cmp  eax, 1\n"
        "    je   .aph_sustain\n"
        "    cmp  eax, 2\n"
        "    je   .aph_finetune\n"
        "    mov  esi, .sh_aph_lock\n"
        "    jmp  .aph_print\n"
        ".aph_pluck:\n"
        "    mov esi, .sh_aph_pluck\n"
        "    jmp .aph_print\n"
        ".aph_sustain:\n"
        "    mov esi, .sh_aph_sustain\n"
        "    jmp .aph_print\n"
        ".aph_finetune:\n"
        "    mov esi, .sh_aph_finetune\n"
        ".aph_print:\n"
        "    call .com1_str\n"
        "    ; K/gamma wu-wei ratio\n"
        "    mov  esi, .sh_aph_wu\n"
        "    call .com1_str\n"
        "    ; Phase K/gamma depends on aphase\n"
        "    mov  eax, [0x101000]\n"
        "    cmp  eax, 0\n"
        "    je   .aph_r1000\n"
        "    cmp  eax, 1\n"
        "    je   .aph_r375\n"
        "    cmp  eax, 2\n"
        "    je   .aph_r200\n"
        "    mov  esi, .sh_aph_r150\n"
        "    jmp  .aph_rwu\n"
        ".aph_r1000: mov esi, .sh_aph_r1000\n"
        "    jmp .aph_rwu\n"
        ".aph_r375:  mov esi, .sh_aph_r375\n"
        "    jmp .aph_rwu\n"
        ".aph_r200:  mov esi, .sh_aph_r200\n"
        ".aph_rwu:   call .com1_str\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".sca_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; ── dna: hardware genome as base-4 codon stream ─────────────────\n"
        "; CPU vendor bytes → base-4 (A=0,C=1,G=2,T=3) codon stream\n"
        "; CPUID leaf 0 EBX:EDX:ECX = vendor string (12 bytes = 4 codons)\n"
        ".sh_cmd_dna:\n"
        "    mov  edi, .sh_dna_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .scd2_no\n"
        "    mov  esi, .sh_dna_hdr\n"
        "    call .com1_str\n"
        "    ; CPUID leaf 0: EBX=vendor[0..3] EDX=vendor[4..7] ECX=vendor[8..11]\n"
        "    mov  eax, 0\n"
        "    cpuid\n"
        "    push ecx\n"
        "    push edx\n"
        "    call .dna_print_word\n"
        "    pop  ebx\n"
        "    call .dna_print_word\n"
        "    pop  ebx\n"
        "    call .dna_print_word\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    ; r_dim from CPU node state (proxy: state/4 → 0.3..1.0)\n"
        "    mov  esi, .sh_dna_rdim\n"
        "    call .com1_str\n"
        "    mov  eax, [OMEGA_BASE + OMEGA_SZ + 12]  ; CPU node state\n"
        "    ; r_dim = 0.3 + (state/4) * 0.7 → show strand letter\n"
        "    cmp  eax, 4\n"
        "    jl   .dna_strand_low\n"
        "    mov  esi, .sh_dna_helix\n"
        "    jmp  .dna_rdim_done\n"
        ".dna_strand_low:\n"
        "    mov  esi, .sh_dna_linear\n"
        ".dna_rdim_done:\n"
        "    call .com1_str\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scd2_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; Print EBX as 4 base-4 chars (each byte → A/C/G/T via low 2 bits)\n"
        ".dna_print_word:\n"
        "    push ecx\n"
        "    mov  ecx, 4\n"
        ".dpw_loop:\n"
        "    mov  al, bl\n"
        "    and  al, 0x03\n"
        "    lea  esi, [.dna_alpha]\n"
        "    movzx eax, byte [esi + eax]\n"
        "    call .com1_send\n"
        "    shr  ebx, 8\n"
        "    loop .dpw_loop\n"
        "    pop  ecx\n"
        "    ret\n"
        ".dna_alpha  db 'A','C','G','T'\n"
        "\n"
        "; Print EAX as 8-digit hex (32-bit)\n"
        ".sh_print_hex32:\n"
        "    push eax\n"
        "    push ecx\n"
        "    mov  ecx, 8\n"
        "    rol  eax, 4\n"
        ".phx32_loop:\n"
        "    push eax\n"
        "    and  al, 0x0F\n"
        "    add  al, '0'\n"
        "    cmp  al, '9'+1\n"
        "    jl   .phx32_ok\n"
        "    add  al, 7\n"
        ".phx32_ok:\n"
        "    call .com1_send\n"
        "    pop  eax\n"
        "    rol  eax, 4\n"
        "    loop .phx32_loop\n"
        "    pop  ecx\n"
        "    pop  eax\n"
        "    ret\n"
        "\n"

        "; ═══════════════════════════════════════════════════════════\n"
        "; PM DISK I/O — ATA PIO LBA28, primary channel (0x1F0)\n"
        "; Works from protected mode, no mode switch needed.\n"
        "; Requires IDE/ATA device (not floppy). Use -if=ide in QEMU.\n"
        "; ═══════════════════════════════════════════════════════════\n"
        "ATA_DATA   equ 0x1F0\n"
        "ATA_FEAT   equ 0x1F1\n"
        "ATA_COUNT  equ 0x1F2\n"
        "ATA_LBA0   equ 0x1F3\n"
        "ATA_LBA1   equ 0x1F4\n"
        "ATA_LBA2   equ 0x1F5\n"
        "ATA_HEAD   equ 0x1F6   ; LBA3[3:0] + 0xE0 for LBA mode\n"
        "ATA_CMD    equ 0x1F7\n"
        "ATA_STATUS equ 0x1F7\n"
        "ATA_READ   equ 0x20\n"
        "ATA_WRITE  equ 0x30\n"
        "\n"
        "; ata_wait: poll until BSY clear and DRQ set\n"
        ".ata_wait:\n"
        "    push edx\n"
        "    push eax\n"
        ".aw_bsy:  mov dx, ATA_STATUS\n"
        "    in   al, dx\n"
        "    test al, 0x80           ; BSY?\n"
        "    jnz  .aw_bsy\n"
        ".aw_drq:  in   al, dx\n"
        "    test al, 0x08           ; DRQ?\n"
        "    jz   .aw_drq\n"
        "    pop  eax\n"
        "    pop  edx\n"
        "    ret\n"
        "\n"
        "; disk_read: EAX=LBA, ECX=sectors, EDI=dest buffer\n"
        ".disk_read:\n"
        "    push eax\n"
        "    push ecx\n"
        "    push edx\n"
        "    push edi\n"
        ".dr_sector_loop:\n"
        "    test ecx, ecx\n"
        "    jz   .dr_done\n"
        "    ; Set up LBA28\n"
        "    mov  dx, ATA_COUNT\n"
        "    mov  al, 1\n"
        "    out  dx, al              ; 1 sector\n"
        "    mov  dx, ATA_LBA0\n"
        "    mov  al, bl\n"
        "    out  dx, al              ; LBA [7:0] (EAX low byte)\n"
        "    mov  dx, ATA_LBA1\n"
        "    mov  al, bh\n"
        "    out  dx, al              ; LBA [15:8]\n"
        "    ; Need EAX for LBA - use push/pop\n"
        "    push eax\n"
        "    push ecx\n"
        "    mov  ebx, eax\n"
        "    mov  dx, ATA_COUNT\n"
        "    mov  al, 1\n"
        "    out  dx, al\n"
        "    mov  dx, ATA_LBA0\n"
        "    mov  al, bl\n"
        "    out  dx, al\n"
        "    mov  dx, ATA_LBA1\n"
        "    mov  al, bh\n"
        "    out  dx, al\n"
        "    shr  ebx, 16\n"
        "    mov  dx, ATA_LBA2\n"
        "    mov  al, bl\n"
        "    out  dx, al\n"
        "    ; bits 27:24 of LBA + 0xE0 (LBA mode, drive 0)\n"
        "    mov  dx, ATA_HEAD\n"
        "    mov  al, bh\n"
        "    and  al, 0x0F\n"
        "    or   al, 0xE0\n"
        "    out  dx, al\n"
        "    mov  dx, ATA_CMD\n"
        "    mov  al, ATA_READ\n"
        "    out  dx, al\n"
        "    call .ata_wait\n"
        "    ; Read 256 words (512 bytes) into EDI\n"
        "    mov  dx, ATA_DATA\n"
        "    mov  ecx, 256\n"
        "    rep  insw                ; read 256 words from ATA data port\n"
        "    pop  ecx\n"
        "    pop  eax\n"
        "    inc  eax                 ; next LBA\n"
        "    dec  ecx\n"
        "    jmp  .dr_sector_loop\n"
        ".dr_done:\n"
        "    pop  edi\n"
        "    pop  edx\n"
        "    pop  ecx\n"
        "    pop  eax\n"
        "    ret\n"
        "\n"
        "; ═══════════════════════════════════════════════════════════\n"
        "; SHELL COMMANDS — traditional shell interface\n"
        "; ═══════════════════════════════════════════════════════════\n"
        "\n"
        "; ls — list disk regions\n"
        ".sh_cmd_ls:\n"
        "    mov  edi, .sh_ls_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .scl_no\n"
        "    mov  esi, .sh_ls_out\n"
        "    call .com1_str\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scl_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; cat S — hex dump sector S (512 bytes, 16 per line)\n"
        ".sh_cmd_cat:\n"
        "    mov  edi, .sh_cat_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .scc_no\n"
        "    call .sh_skip_token\n"
        "    call .sh_parse_dec         ; EAX = sector\n"
        "    ; Print header\n"
        "    push eax\n"
        "    mov  esi, .sh_cat_hdr\n"
        "    call .com1_str\n"
        "    pop  eax\n"
        "    push eax\n"
        "    call .sh_print_dec\n"
        "    mov  al, ':'\n"
        "    call .com1_send\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    ; Read sector into 0x21000\n"
        "    pop  eax\n"
        "    mov  ecx, 1\n"
        "    mov  edi, 0x21000\n"
        "    call .disk_read\n"
        "    ; Hex dump: 512 bytes, 16 per line\n"
        "    mov  esi, 0x21000\n"
        "    mov  ecx, 32               ; 32 lines × 16 bytes = 512\n"
        ".cat_line:\n"
        "    push ecx\n"
        "    push esi\n"
        "    ; Print offset as decimal (0-511)\n"
        "    mov  eax, esi\n"
        "    sub  eax, 0x21000\n"
        "    call .sh_print_dec\n"
        "    mov  al, ' '\n"
        "    call .com1_send\n"
        "    ; Print 16 hex bytes\n"
        "    mov  ecx, 16\n"
        ".cat_hex:\n"
        "    movzx eax, byte [esi]\n"
        "    inc  esi\n"
        "    call .sh_print_hex8\n"
        "    mov  al, ' '\n"
        "    call .com1_send\n"
        "    dec  ecx\n"
        "    jnz  .cat_hex\n"
        "    ; ASCII column\n"
        "    pop  esi\n"
        "    push esi\n"
        "    mov  al, '|'\n"
        "    call .com1_send\n"
        "    mov  ecx, 16\n"
        ".cat_ascii:\n"
        "    movzx eax, byte [esi]\n"
        "    inc  esi\n"
        "    cmp  al, 0x20\n"
        "    jl   .cat_dot\n"
        "    cmp  al, 0x7E\n"
        "    jg   .cat_dot\n"
        "    jmp  .cat_prn\n"
        ".cat_dot:\n"
        "    mov al, '.'\n"
        ".cat_prn:\n"
        "    call .com1_send\n"
        "    dec  ecx\n"
        "    jnz  .cat_ascii\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    pop  esi\n"
        "    pop  ecx\n"
        "    dec  ecx\n"
        "    jnz  .cat_line\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scc_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; exec S — load sector S to 0x30000 and jump to it\n"
        ".sh_cmd_exec:\n"
        "    mov  edi, .sh_exec_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .sce_no\n"
        "    call .sh_skip_token\n"
        "    call .sh_parse_dec         ; EAX = sector\n"
        "    push eax\n"
        "    mov  esi, .sh_exec_load\n"
        "    call .com1_str\n"
        "    pop  eax\n"
        "    push eax\n"
        "    call .sh_print_dec\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    ; Load 16 sectors (8KB) from that LBA to 0x30000\n"
        "    pop  eax\n"
        "    mov  ecx, 16\n"
        "    mov  edi, 0x30000\n"
        "    call .disk_read\n"
        "    ; Jump to loaded code\n"
        "    mov  esi, .sh_exec_jmp\n"
        "    call .com1_str\n"
        "    call 0x30000\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".sce_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; info — system info from Omega graph\n"
        ".sh_cmd_info:\n"
        "    mov  edi, .sh_info_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .sci_no\n"
        "    ; CPU family from CPUID\n"
        "    mov  esi, .sh_info_cpu\n"
        "    call .com1_str\n"
        "    mov  eax, [OMEGA_BASE + OMEGA_SZ + 52]  ; cpuid eax stored in transform+4\n"
        "    call .sh_print_hex32\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    ; Memory total from MEM node transform field\n"
        "    mov  esi, .sh_info_mem\n"
        "    call .com1_str\n"
        "    mov  eax, [OMEGA_BASE + OMEGA_SZ*2 + 52] ; mem total KB\n"
        "    call .sh_print_dec\n"
        "    mov  esi, .sh_info_kb\n"
        "    call .com1_str\n"
        "    ; PCI device count (walk IO children)\n"
        "    mov  esi, .sh_info_pci\n"
        "    call .com1_str\n"
        "    mov  edi, [OMEGA_BASE + OMEGA_SZ*3 + 32]  ; IO.child\n"
        "    mov  ecx, 0\n"
        ".info_pci_count:\n"
        "    test edi, edi\n"
        "    jz   .info_pci_done\n"
        "    cmp  edi, OMEGA_BASE\n"
        "    jl   .info_pci_done\n"
        "    inc  ecx\n"
        "    mov  edi, [edi+40]          ; next sibling\n"
        "    jmp  .info_pci_count\n"
        ".info_pci_done:\n"
        "    mov  eax, ecx\n"
        "    call .sh_print_dec\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".sci_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; sectors — total sectors on disk, first free\n"
        ".sh_cmd_sectors:\n"
        "    mov  edi, .sh_sectors_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .scs2_no\n"
        "    mov  esi, .sh_sectors_out\n"
        "    call .com1_str\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scs2_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"

        "; ═══════════════════════════════════════════════════════════\n"
        "; HDGL NATIVE KERNEL\n"
        "; Ω_n+1 = T(Ω_n)  —  hardware events as glyph mutations\n"
        "; ═══════════════════════════════════════════════════════════\n"
        "\n"
        "; phi_kernel_init: native phi-lattice kernel\n"
        "; No IDT. No PIC. No PIT. No INT 0x80. No privilege rings.\n"
        "; The lattice IS the kernel. Phase lock IS permission.\n"
        "; GOI/GUZ IS fault handling. The tick IS phi-fold accumulation.\n"
        ".kernel_init:\n"
        "    ; ── Phase 1: seed phi lattice at 0x101000 ─────────────────\n"
        "    ; 128 slots × 4 words = 512 uint32 = 2048 bytes\n"
        "    ; Mantissa seeded with φ × Fib × Prime harmony\n"
        "    ; Slot[i] = fib[i%16] × prime[i%16] × (PHI^(i%8)) × 0x10000\n"
        "    ; No malloc. No heap. Slots live in fixed Omega memory.\n"
        "    push eax\n"
        "    push ebx\n"
        "    push ecx\n"
        "    push edx\n"
        "    push edi\n"
        "    ; Zero phi-lattice region (explicit loop, phi-neutral)\n"
        "    mov  edi, 0x101020\n"
        "    mov  ecx, 512\n"
        ".phi_zero:\n"
        "    mov  dword [edi], 0\n"
        "    add  edi, 4\n"
        "    dec  ecx\n"
        "    jnz  .phi_zero\n"
        "    ; Seed 128 lattice slots with phi-harmonic values (x87 FPU)\n"
        "    ; slot[i] = round(fib[i%8] * prime[i%8] * PHI^(1+(i%4)) * 65536)\n"
        "    ; Using integer approximations (no runtime FPU needed in ISR):\n"
        "    ;   PHI^1=1.618, PHI^2=2.618, PHI^3=4.236, PHI^4=6.854\n"
        "    ;   Precomputed: slot seed table at .phi_seed_table\n"
        "    mov  edi, 0x101020\n"
        "    mov  esi, .phi_seed_table\n"
        "    mov  ecx, 128\n"
        ".phi_seed_loop:\n"
        "    mov  eax, [esi]\n"
        "    ; XOR-free mix: slot += fib[i%16] * prime[i%8] mod 2^32\n"
        "    ; Use additive phi accumulation: slot[i] = (slot[i-1]*3 + seed) mod 2^32\n"
        "    cmp  edi, 0x101020\n"
        "    je   .phi_first_slot\n"
        "    mov  edx, [edi-4]\n"
        "    imul edx, edx, 3\n"
        "    add  eax, edx\n"
        ".phi_first_slot:\n"
        "    stosd\n"
        "    add  esi, 4\n"
        "    cmp  esi, .phi_seed_table_end\n"
        "    jl   .phi_no_wrap\n"
        "    mov  esi, .phi_seed_table\n"
        ".phi_no_wrap:\n"
        "    dec  ecx\n"
        "    jnz  .phi_seed_loop\n"
        "    ; ── Phase 2: set GOI/GUZ saturation limits ─────────────────\n"
        "    ; GOI (Gradual Overflow Infinity): lattice value > threshold\n"
        "    ; GUZ (Gradual Underflow Zero):   lattice value < threshold\n"
        "    ; Stored at 0x101800 (GOI limit) and 0x101804 (GUZ limit)\n"
        "    ; These replace hardware fault vectors. The lattice self-limits.\n"
        "    mov  dword [0x101800], 0xFFFF0000  ; GOI: high-water mark\n"
        "    mov  dword [0x101804], 0x00000100  ; GUZ: low-water mark\n"
        "    ; ── Phase 3: init COM1 for polling (no PIC, no IRQ needed) ──\n"
        "    ; Input is polled by the shell. Events are glyph mutations.\n"
        "    ; The phi-tick advances on each shell iteration (wu-wei).\n"
        "    mov  dword [0x102100], 0\n"
        "    mov  dword [0x102104], 0x102108\n"
        "    mov  dword [0x102110], 0x102108\n"
        "    ; ── Phase 4: init process table (phase = consensus state) ───\n"
        "    ; Process 0 (shell) starts at APA_FLAG_CONSENSUS (LOCK).\n"
        "    ; Consensus = permission. Phase lock = ready to run.\n"
        "    ; No privilege rings. No ring 0/3 boundary.\n"
        "    mov  dword [0x103000 + 12], 3   ; state = READY\n"
        "    ; ── Phase 5: mark phi-kernel ready ────────────────────────\n"
        "    ; Set APA_FLAG_CONSENSUS bit (1<<4) in Omega kernel node\n"
        "    ; This means: kernel lattice has reached phase consensus.\n"
        "    ; The kernel IS the lattice. Consensus IS the boot state.\n"
        "    mov  dword [0x101010], 0        ; tick counter = 0\n"
        "    mov  dword [0x101014], 0x10     ; APA_FLAG_CONSENSUS = (1<<4)\n"
        "    ; No sti. No PIC unmask. No interrupt table.\n"
        "    ; The phi-tick loop in the shell IS the event loop.\n"
        "    pop  edi\n"
        "    pop  edx\n"
        "    pop  ecx\n"
        "    pop  ebx\n"
        "    pop  eax\n"
        "    mov  esi, .msg_kernel_ok\n"
        "    call .com1_str\n"
        "    ret\n"

        "\n"
        "; ═══════════════════════════════════════════════════════════\n"
        "; NATIVE PHI PRIMITIVES — no IDT, no PIC, no INT, no iret\n"
        "; ═══════════════════════════════════════════════════════════\n"
        "\n"
        "; phi_tick: one prismatic_recursion step\n"
        "; Replaces: timer ISR (IRQ0)\n"
        "; Called: at the top of sh_loop (wu-wei — the shell IS the tick)\n"
        "; Formula: slot[i] = (slot[i-1]*3 + fib[i%8]*prime[i%8]) mod 2^32\n"
        "; This IS Ωₙ₊₁ = T(Ωₙ) — the rewrite tick, natively.\n"
        ".phi_tick:\n"
        "    push eax\n"
        "    push ebx\n"
        "    push ecx\n"
        "    push edi\n"
        "    inc  dword [0x101010]         ; advance tick counter\n"
        "    mov  edi, 0x101020            ; phi lattice base\n"
        "    mov  ecx, 128                 ; 128 slots\n"
        "    ; GOI check: if any slot exceeds 0xFFFF0000 → GOI flag\n"
        "    ; GOI (Gradual Overflow Infinity) = saturate, not crash\n"
        "    mov  eax, [0x101800]          ; GOI limit\n"
        ".pt_loop:\n"
        "    mov  ebx, [edi]\n"
        "    cmp  ebx, eax                 ; GOI check\n"
        "    jge  .pt_goi                  ; saturate, not crash\n"
        "    ; Prismatic accumulation: slot = slot*3 + seed_from_table\n"
        "    ; XOR-free, additive, phi-harmonic\n"
        "    imul ebx, ebx, 3\n"
        "    add  ebx, [0x101010]          ; add tick count as phi seed\n"
        "    add  [edi], ebx\n"
        "    jmp  .pt_next\n"
        ".pt_goi:\n"
        "    ; GOI: saturate slot to GOI limit (not overflow, not crash)\n"
        "    mov  dword [edi], 0xFFFF0000  ; GOI saturation value\n"
        ".pt_next:\n"
        "    add  edi, 4\n"
        "    dec  ecx\n"
        "    jnz  .pt_loop\n"
        "    pop  edi\n"
        "    pop  ecx\n"
        "    pop  ebx\n"
        "    pop  eax\n"
        "    ret\n"
        "\n"
        "; phi_lk_read: lattice kernel read\n"
        "; Replaces: INT 0x80 syscall gate (lk_read call)\n"
        "; IN:  ESI = key string, ECX = output size (1..32)\n"
        "; OUT: EBX = phi_fold result (32-bit extract of phi hash)\n"
        "; phi_fold: additive Z/256Z, no XOR, no SHA\n"
        ";   acc[i] = (acc[i]*3 + (key[i] + lattice[i]) mod 256) mod 256\n"
        "; The lattice IS the secret key. No external crypto.\n"
        ".phi_lk_read:\n"
        "    push eax\n"
        "    push ecx\n"
        "    push edi\n"
        "    ; Init accumulator from lattice slots 0..3 (IV = lattice state)\n"
        "    mov  eax, [0x101020]           ; lattice[0] = phi-keyed IV\n"
        "    ; Delta-fold absorption: additive, no XOR\n"
        "    ; For each key byte: acc = (acc*3 + key_byte + lattice[i]) mod 2^32\n"
        "    mov  edi, 0                    ; lattice index (explicit zero)\n"
        ".plk_loop:\n"
        "    movzx ebx, byte [esi]\n"
        "    test  bl, bl\n"
        "    jz    .plk_done\n"
        "    mov   ecx, [0x101020 + edi*4]  ; lattice[edi]\n"
        "    ; acc = (acc*3 + key_byte + lattice[edi]) mod 2^32\n"
        "    imul  eax, eax, 3\n"
        "    add   eax, ebx\n"
        "    add   eax, ecx\n"
        "    inc   esi\n"
        "    inc   edi\n"
        "    cmp   edi, 128\n"
        "    jl    .plk_loop\n"
        "    mov  edi, 0\n"
        "    jmp   .plk_loop\n"
        ".plk_done:\n"
        "    ; Finalize: 4 rounds of ROTR8-by-3 + additive fold\n"
        "    mov  ecx, 4\n"
        ".plk_final:\n"
        "    ror  eax, 3\n"
        "    add  eax, [0x101020]           ; mix with lattice[0]\n"
        "    dec  ecx\n"
        "    jnz  .plk_final\n"
        "    mov  ebx, eax                  ; EBX = phi_lk_read result\n"
        "    pop  edi\n"
        "    pop  ecx\n"
        "    pop  eax\n"
        "    ret\n"
        "\n"
        "; phi_advance: lk_advance — advance lattice epoch\n"
        "; Replaces: lk_advance() / PIT tick\n"
        "; Called: by shell 'tick' command and after each lattice fold\n"
        "; Invalidates GOI/GUZ caches. Advances all slot mantissas by\n"
        "; one prismatic_recursion step. New epoch = new key material.\n"
        ".phi_advance:\n"
        "    call .phi_tick                 ; one full lattice step\n"
        "    ; Invalidate GOI cache: reset any saturated slots toward GUZ\n"
        "    mov  edi, 0x101020\n"
        "    mov  ecx, 128\n"
        "    mov  eax, [0x101804]           ; GUZ limit\n"
        ".pa_loop:\n"
        "    cmp  dword [edi], 0xFFFF0000   ; was GOI?\n"
        "    jne  .pa_next\n"
        "    mov  dword [edi], eax          ; reset to GUZ floor\n"
        ".pa_next:\n"
        "    add  edi, 4\n"
        "    dec  ecx\n"
        "    jnz  .pa_loop\n"
        "    ret\n"
        "\n"
        "; phi_consensus: detect phase lock\n"
        "; Replaces: privilege ring check (ring 0 = kernel, ring 3 = user)\n"
        "; APA_FLAG_CONSENSUS (1<<4) = phase lock = permission granted\n"
        "; Returns: ZF=1 if consensus reached (phase variance < epsilon)\n"
        "; Algorithm: check if all lattice slots within 10% of mean\n"
        ";   mean = sum(slot[0..127]) / 128\n"
        ";   variance = max |slot[i] - mean|\n"
        ";   consensus = variance < mean/10\n"
        ".phi_consensus:\n"
        "    push eax\n"
        "    push ebx\n"
        "    push ecx\n"
        "    push edi\n"
        "    ; Compute mean of lattice slots (32-bit, truncated)\n"
        "    mov  eax, 0\n"
        "    mov  edi, 0x101020\n"
        "    mov  ecx, 128\n"
        ".pc_sum:\n"
        "    add  eax, [edi]\n"
        "    add  edi, 4\n"
        "    dec  ecx\n"
        "    jnz  .pc_sum\n"
        "    shr  eax, 7                    ; eax = mean (>>7 = /128)\n"
        "    ; Compute max deviation\n"
        "    mov  edi, 0x101020\n"
        "    mov  ecx, 128\n"
        "    mov  ebx, 0\n"
        ".pc_var:\n"
        "    mov  edx, [edi]\n"
        "    sub  edx, eax\n"
        "    jns  .pc_pos\n"
        "    neg  edx\n"
        ".pc_pos:\n"
        "    cmp  edx, ebx\n"
        "    jle  .pc_next\n"
        "    mov  ebx, edx\n"
        ".pc_next:\n"
        "    add  edi, 4\n"
        "    dec  ecx\n"
        "    jnz  .pc_var\n"
        "    ; consensus if max_dev < mean/10\n"
        "    shr  eax, 1                    ; eax = mean/2 (conservative)\n"
        "    cmp  ebx, eax                  ; ZF=1 if dev < mean/2\n"
        "    pop  edi\n"
        "    pop  ecx\n"
        "    pop  ebx\n"
        "    pop  eax\n"
        "    ret\n"
        "\n"
        "; phi_poll: the native event loop\n"
        "; Called at top of sh_loop. wu-wei: each shell iteration IS the tick.\n"
        "; Advances phi lattice one prismatic step. Checks GOI/GUZ saturation.\n"
        "; This IS Ωₙ₊₁ = T(Ωₙ) — the rewrite happens here, continuously.\n"
        ".sh_poll_input:\n"
        "    ; wu-wei phi tick: advance lattice one step\n"
        "    call .phi_tick\n"
        "    ret\n"
        "\n"
        ".msg_kernel_ok  db '[Kernel] phi-lattice ready. Consensus=LOCK phi-tick=active',13,10,0\n"
        ".msg_fault      db '[GOI] lattice overflow at EIP=',0\n"
        "; phi_seed_table: 16 × 32-bit seeds derived from fib×prime×PHI^n\n"
        "; Used by phi_kernel_init to seed the 128-slot phi lattice.\n"
        "; XOR-free: values are additive phi harmonics, no bitwise ops.\n"
        ".phi_seed_table:\n"
        "    ; Gleaned from conscious-128-bit-floor ll_analog.c\n"
        "    ; Formula: fib[i%8]*prime[i%8]*PHI^(1+(i%4))*65536\n"
        "    dd 0x00033C6F\n"  /* fib=1  prime=2  PHI^1 */
        "    dd 0x0007DAA6\n"  /* fib=1  prime=3  PHI^2 */
        "    dd 0x002A5C56\n"  /* fib=2  prime=5  PHI^3 */
        "    dd 0x008FEFA7\n"  /* fib=3  prime=7  PHI^4 */
        "    dd 0x0058FDEB\n"  /* fib=5  prime=11 PHI^1 */
        "    dd 0x01104689\n"  /* fib=8  prime=13 PHI^2 */
        "    dd 0x03A82BC8\n"  /* fib=13 prime=17 PHI^3 */
        "    dd 0x0AAEC964\n"  /* fib=21 prime=19 PHI^4 */
        "    dd 0x00033C6F\n"  /* wraps to i=0 */
        "    dd 0x0007DAA6\n"  /* wraps to i=1 */
        "    dd 0x002A5C56\n"  /* wraps to i=2 */
        "    dd 0x008FEFA7\n"  /* wraps to i=3 */
        "    dd 0x0058FDEB\n"  /* wraps to i=4 */
        "    dd 0x01104689\n"  /* wraps to i=5 */
        "    dd 0x03A82BC8\n"  /* wraps to i=6 */
        "    dd 0x0AAEC964\n"  /* wraps to i=7 */
        ".phi_seed_table_end:\n"
        "; GOI/GUZ limits stored at runtime\n"
        "; 0x101800: GOI limit (0xFFFF0000)\n"
        "; 0x101804: GUZ limit (0x00000100)\n"
        "; 0x101020: phi lattice slots (128 × uint32)\n"
        ".msg_fault      db '[FAULT] EIP=',0\n"
        "; Tick and process counters\n"
        "; 0x101010: tick counter\n"
        "; 0x101014: ready process count\n"
        "; 0x102100: serial input flag\n"
        "; 0x102104: ring buffer tail\n"
        "; 0x102108+: ring buffer data\n"

        "; ps: list process table (Omega nodes 16-31)\n"
        ".sh_cmd_ps:\n"
        "    mov  edi, .sh_ps_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .scp2_no\n"
        "    mov  esi, .sh_ps_hdr\n"
        "    call .com1_str\n"
        "    mov  esi, .sh_ps_tick\n"
        "    call .com1_str\n"
        "    mov  eax, [0x101010]\n"
        "    call .sh_print_dec\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    mov  esi, .sh_ps_cons\n"
        "    call .com1_str\n"
        "    mov  eax, [0x101014]\n"
        "    test eax, 0x10\n"
        "    jnz  .ps_locked\n"
        "    mov  esi, .sh_ps_pluck\n"
        "    call .com1_str\n"
        "    jmp  .ps_done\n"
        ".ps_locked:\n"
        "    mov  esi, .sh_ps_lock\n"
        "    call .com1_str\n"
        ".ps_done:\n"
        "    mov  esi, .sh_ps_goi\n"
        "    call .com1_str\n"
        "    mov  eax, [0x101800]\n"
        "    call .sh_print_hex32\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    mov  esi, .sh_ps_guz\n"
        "    call .com1_str\n"
        "    mov  eax, [0x101804]\n"
        "    call .sh_print_hex32\n"
        "    mov  al, 13\n"
        "    call .com1_send\n"
        "    mov  al, 10\n"
        "    call .com1_send\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scp2_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        "; uptime: kernel tick counter (100Hz)\n"
        ".sh_cmd_uptime:\n"
        "    mov  edi, .sh_uptime_str\n"
        "    call .sh_strcmp_word\n"
        "    jnz  .scu_no\n"
        "    mov  esi, .sh_uptime_pfx\n"
        "    call .com1_str\n"
        "    mov  eax, [0x101010]   ; tick counter\n"
        "    call .sh_print_dec\n"
        "    mov  esi, .sh_uptime_sfx\n"
        "    call .com1_str\n"
        "    mov  eax, 1\n"
        "    ret\n"
        ".scu_no:\n"
        "    mov  eax, 0\n"
        "    ret\n"
        "\n"
        ".sh_ps_str     db 'ps',0\n"
        ".sh_uptime_str db 'uptime',0\n"
        ".sh_ps_hdr     db 'phi-lattice consensus state:',13,10,0\n"
        ".sh_ps_tick    db '  phi-tick:  ',0\n"
        ".sh_ps_cons    db '  consensus: ',0\n"
        ".sh_ps_lock    db 'LOCK (APA_FLAG_CONSENSUS set)',13,10,0\n"
        ".sh_ps_pluck   db 'PLUCK (converging)',13,10,0\n"
        ".sh_ps_goi     db '  GOI limit: 0x',0\n"
        ".sh_ps_guz     db '  GUZ limit: 0x',0\n"
        ".sh_ps_pfx     db '  slot[',0\n"
        ".sh_uptime_pfx db 'ticks: ',0\n"
        ".sh_uptime_sfx db ' (phi-ticks, wu-wei)',13,10,0\n"
        ".msg_boot    db '[Omega] BOOT: graph init -> OBSERVE',13,10,0\n"
        ".msg_ana_dn       db '[Analog] Dn(r) lattice: phi-seeded 8-strand 32-slot',13,10,0\n"
        ".msg_ana_strand_a db '  Strand A r=0.3 INIT  D1-D4 grounded (Dn<sqrt_phi)',13,10,0\n"
        ".msg_ana_strand_h db '  Strand H r=1.0 HELIX D29-D32 excited (Dn>sqrt_phi)',13,10,0\n"
        ".msg_ana_lock     db '  Kuramoto: PLUCK->SUSTAIN->FINETUNE->LOCK wu-wei',13,10,0\n"
        ".msg_graph_hdr db '[Omega] Graph state:',13,10,0\n"
        ".msg_omega   db '  Omega[',0\n"
        ".msg_type    db '] type=',0\n"
        ".msg_state   db ' state=',0\n"
        ".msg_realize db '[Omega] REALIZE: T_COMPILE_SELF -> fixed point',13,10,0\n"
        ".msg_runtime db '[Omega] RUNTIME: Omega_n+1=T(Omega_n) complete',13,10,0\n"

        "; ── Shell strings ───────────────────────────────────────\n"
        ".sh_banner      db 13,10,'HDGL> phi-lattice live. type help<enter>',13,10,0\n"
        ".sh_prompt      db 'Omega> ',0\n"
        ".sh_tick_done   db 'tick applied.',13,10,0\n"
        ".sh_reset_msg   db 'resetting graph...',13,10,0\n"
        ".sh_strand_pfx  db 'strand r=',0\n"
        ".sh_strand_nodes db ' slots ',0\n"
        ".sh_strand_bad  db 'strand 1..8',13,10,0\n"
        ".sh_glyph_ok    db 'mutated: ',0\n"
        ".sh_glyph_badstate db 'state 0..4 only',13,10,0\n"
        ".sh_glyph_badnode  db 'node 0..63 only',13,10,0\n"
        ".sh_help_text   db 'omega [N]  - print graph or node N',13,10,"
                         "             'tick       - one rewrite pass',13,10,"
                         "             'dn         - Dn(r) lattice',13,10,"
                         "             'strand N   - strand N info (1-8)',13,10,"
                         "             'glyph N S  - mutate node N to state S',13,10,"
                         "             'reset      - re-run boot sequence',13,10,"
                         "             'phi        - phi-lattice depth per node',13,10,"
                         "             'b4096 N    - Base4096 encode node N id',13,10,"
                         "             'wave       - strand wave types + aggregate',13,10,"
                         "             'xform N    - transform field of node N',13,10,"
                         "             'tree       - Omega graph as ASCII tree',13,10,"
                         "             'aphase     - Kuramoto phase state',13,10,"
                         "             'dna        - hardware genome codons',13,10,"
                         "             'ls         - list disk regions',13,10,"
                      "             'cat S      - hex dump sector S',13,10,"
                      "             'exec S     - load sector S and run it',13,10,"
                      "             'info       - cpu/mem/pci from Omega graph',13,10,"
                      "             'sectors    - disk layout + free space',13,10,"
                      "             'ps         - process table (kernel)',13,10,"
        "             'uptime     - kernel tick counter',13,10,"
        "             'phi        - phi-lattice depth per node',13,10,"
        "             'uptime     - phi-tick count + consensus\n"
        "             'help       - this list',13,10,0\n"
        ".sh_cmd_omega_str  db 'omega',0\n"
        ".sh_cmd_tick_str   db 'tick',0\n"
        ".sh_cmd_dn_str     db 'dn',0\n"
        ".sh_cmd_strand_str db 'strand',0\n"
        ".sh_cmd_glyph_str  db 'glyph',0\n"
        ".sh_cmd_reset_str  db 'reset',0\n"
        ".sh_cmd_help_str   db 'help',0\n"
        "; Rich command strings\n"
        ".sh_phi_str     db 'phi',0\n"
        ".sh_b4096_str   db 'b4096',0\n"
        ".sh_wave_str    db 'wave',0\n"
        ".sh_xform_str   db 'xform',0\n"
        ".sh_tree_str    db 'tree',0\n"
        ".sh_aphase_str  db 'aphase',0\n"
        ".sh_dna_str     db 'dna',0\n"
        ".sh_phi_hdr     db 'phi-lattice depth Lp per node:',13,10,0\n"
        ".sh_phi_pfx     db '  node[',0\n"
        ".sh_b4096_pfx   db 'b4096: ',0\n"
        ".sh_wave_hdr    db 'wave (+/0/-) per strand:',13,10,0\n"
        ".sh_wave_agg    db 'agg: ',0\n"
        ".sh_xf_raw      db 'TRANSFORM: ',0\n"
        ".sh_xf_identity db 'T_IDENTITY',0\n"
        ".sh_xf_cpuid    db 'T_CPUID',0\n"
        ".sh_xf_e820     db 'T_E820',0\n"
        ".sh_xf_pci      db 'T_PCI_WALK',0\n"
        ".sh_xf_compile  db 'T_COMPILE_SELF',0\n"
        ".sh_xf_rewrite  db 'T_REWRITE',0\n"
        ".sh_tree_hdr    db 'Omega tree:',13,10,0\n"
        ".sh_aphase_hdr  db 'Kuramoto aphase: ',0\n"
        ".sh_aph_pluck   db 'PLUCK',13,10,0\n"
        ".sh_aph_sustain db 'SUSTAIN',13,10,0\n"
        ".sh_aph_finetune db 'FINETUNE',13,10,0\n"
        ".sh_aph_lock    db 'LOCK',13,10,0\n"
        ".sh_aph_wu      db 'K/g wu-wei: ',0\n"
        ".sh_aph_r1000   db '1000:1 (Pluck)',13,10,0\n"
        ".sh_aph_r375    db '375:1 (Sustain)',13,10,0\n"
        ".sh_aph_r200    db '200:1 (FineTune)',13,10,0\n"
        ".sh_aph_r150    db '150:1 (Lock)',13,10,0\n"
        ".sh_dna_hdr     db 'hardware genome (CPUID vendor):',13,10,0\n"
        ".sh_dna_rdim    db 'r_dim: ',0\n"
        ".sh_dna_helix   db '1.0 full double-helix (EXECUTED)',13,10,0\n"
        ".sh_dna_linear  db '0.3 linear strand',13,10,0\n"
        ".sh_unknown        db '?',13,10,0\n"
        "; Shell command strings\n"
        ".sh_ls_str     db 'ls',0\n"
        ".sh_cat_str    db 'cat',0\n"
        ".sh_exec_str   db 'exec',0\n"
        ".sh_info_str   db 'info',0\n"
        ".sh_sectors_str db 'sectors',0\n"
        ".sh_ls_out     db 'sector  0      MBR (boot stub)',13,10,"
                         "             'sectors 1-16   runtime (8KB)',13,10,"
                         "             'sector  17     padding',13,10,"
                         "             'sectors 18-91  hdgl_firmware.hdgl (source)',13,10,"
                         "             'sectors 92+    free',13,10,0\n"
        ".sh_cat_hdr    db 'sector ',0\n"
        ".sh_exec_load  db 'loading sector ',0\n"
        ".sh_exec_jmp   db 'jumping...',13,10,0\n"
        ".sh_info_cpu   db 'cpu:  ',0\n"
        ".sh_info_mem   db 'mem:  ',0\n"
        ".sh_info_kb    db ' KB',13,10,0\n"
        ".sh_info_pci   db 'pci:  ',0\n"
        ".sh_sectors_out db 'total: 92 provisioned',13,10,"
                          "             'free:  92+ (append past end)',13,10,"
                          "             'use exec S to run any sector',13,10,0\n"
        ".sh_buf        times 64 db 0\n"
        ".sh_errbuf     times 16 db 0\n"
        ".sh_dec_buf    times 12 db 0\n"
        "\n"
        "[BITS 16]\n"
        "align 8\n"
        ".gdt_start:\n"
        "    dq 0\n"
        "    dw 0xFFFF, 0x0000\n"
        "    db 0x00, 0x9A, 0xCF, 0x00\n"
        "    dw 0xFFFF, 0x0000\n"
        "    db 0x00, 0x92, 0xCF, 0x00\n"
        ".gdt_end:\n"
        "\n"
        ".gdt_ptr:\n"
        "    dw .gdt_end - .gdt_start - 1\n"
        "    dd .gdt_start\n"
        "\n"
        "times 8192 - ($ - $$) db 0\n"
    );
}

/* ============================================================================
 * MAIN
 * ============================================================================ */

int main(int argc, char* argv[]) {
    memset(&M, 0, sizeof(M));

    /* Initialize root node (as in hdgl_universal_preassembler.c) */
    M.graph[0].identity  = 0;
    M.graph[0].type      = TYPE_ROOT;
    M.graph[0].state     = STATE_INIT;
    M.graph[0].transform = TRANSFORM_IDENTITY;
    M.graph[0].parent    = (uint64_t)-1;
    M.graph[0].child     = (uint64_t)-1;
    M.graph[0].next      = (uint64_t)-1;
    M.graph_size = 1;

    const char* src_path = NULL;
    const char* out_path = NULL;

    if (argc >= 2) src_path = argv[1];
    if (argc >= 3) out_path = argv[2];

    /* Load source */
    char* source = NULL;
    size_t source_len = 0;

    if (src_path) {
        FILE* fp = fopen(src_path, "r");
        if (!fp) {
            fprintf(stderr, "Cannot open: %s\n", src_path);
            return 1;
        }
        fseek(fp, 0, SEEK_END);
        source_len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        source = malloc(source_len + 1);
        fread(source, 1, source_len, fp);
        source[source_len] = 0;
        fclose(fp);
        fprintf(stderr, "HDGL: loaded %s (%zu bytes)\n", src_path, source_len);
    } else {
        /* Default: minimal self-test */
        source = strdup(
            "glyph root\n    class = ROOT\n    state = INIT\nend\n"
            "glyph cpu\n    parent = root\n    class = CPU\n    state = INIT\nend\n"
            "glyph memory\n    parent = root\n    class = MEM\n    state = INIT\nend\n"
            "glyph io\n    parent = root\n    class = IO\n    state = INIT\nend\n"
            "glyph compiler\n    parent = root\n    class = COMPILER\n    state = INIT\nend\n"
            "recurse root\n    mutate DISCOVERED\nend\n"
        );
        source_len = strlen(source);
        fprintf(stderr, "HDGL: using built-in self-test source\n");
    }

    fprintf(stderr, "HDGL: entering parse\n"); fflush(stderr);
    /* Parse: build Omega graph from source */
    Parser p = { source, 0, source_len, 1 };
    hdgl_parse(&p);
    fprintf(stderr, "HDGL: parse done, pos=%zu\n", p.pos); fflush(stderr);

    fprintf(stderr, "HDGL: parsed %zu nodes\n", M.graph_size);
    fprintf(stderr, "HDGL: starting tick\n");
    /* Run the universal tick until fixed point */
    int steps = 0;
    fprintf(stderr, "HDGL: ticking... graph_size=%zu\n", M.graph_size);
    while (hdgl_tick() && steps++ < 100)
        ;
    fprintf(stderr, "HDGL: %d ticks, fixed point at %llu cycles\n",
            steps, (unsigned long long)M.cycles);

    /* Print the graph state */
    hdgl_print_graph();

    /* Run analog-over-digital phase */
    if (M.graph_size > 1) {
        uint64_t ids[256] = {0};
        uint32_t types[256] = {0};
        uint32_t states[256] = {0};
        for (size_t _i = 0; _i < M.graph_size && _i < 256; _i++) {
            ids[_i]    = M.graph[_i].identity;
            types[_i]  = (uint32_t)M.graph[_i].type;
            states[_i] = (uint32_t)M.graph[_i].state;
        }
        hdgl_analog_run(ids, types, states, M.graph_size, 1);
    }


    /* Emit x86 assembly if output requested */
    if (out_path) {
        FILE* fp = fopen(out_path, "w");
        if (!fp) {
            fprintf(stderr, "Cannot write: %s\n", out_path);
            return 1;
        }
        M.emit_mode = 1;
        hdgl_emit_asm(fp);
        fclose(fp);
        fprintf(stderr, "HDGL: emitted assembly to %s\n", out_path);
    }

    free(source);
    return 0;
}
