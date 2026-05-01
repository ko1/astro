#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>

#include "node.h"
#include "context.h"
#include "astro_code_store.h"

// =====================================================================
// node_allocate — every NODE comes through here so we can track them
// for the AOT bake walker (compiles each registered body + every parser
// side-array node so dlsym(SD_<hash>) resolves the whole AST).
// =====================================================================
extern size_t node_cnt;
static struct {
    NODE   **arr;
    uint32_t cnt;
    uint32_t cap;
} g_all_nodes = {0};

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)calloc(1, size);
    if (!n) { fprintf(stderr, "node_allocate: out of memory\n"); exit(1); }
    if (g_all_nodes.cnt == g_all_nodes.cap) {
        g_all_nodes.cap = g_all_nodes.cap ? g_all_nodes.cap * 2 : 256;
        g_all_nodes.arr = (NODE **)realloc(g_all_nodes.arr,
                                           g_all_nodes.cap * sizeof(NODE *));
        if (!g_all_nodes.arr) { fprintf(stderr, "node_allocate: oom (track)\n"); exit(1); }
    }
    g_all_nodes.arr[g_all_nodes.cnt++] = n;
    node_cnt++;
    return n;
}

static void
dispatch_info(CTX *c, NODE *n, bool end)
{
    (void)c; (void)n; (void)end;
}

void
clear_hash(NODE *n)
{
    while (n) { n->head.flags.has_hash_value = false; n = n->head.parent; }
}

// HORG: structural hash, canonicalised via @canonical so swap_dispatcher
// family members share a lookup key.  HOPT: profile-aware hash that
// uses the *actual* (post-swap) kind name; PGC bake names SDs by HOPT.
typedef uint64_t node_hash_t;
node_hash_t HORG(NODE *n);

node_hash_t
HOPT(NODE *n)
{
    if (n == NULL) return 0;
    if (n->head.flags.has_hash_opt) return n->head.hash_opt;
    if (n->head.kind->hopt_func) {
        n->head.flags.has_hash_opt = true;
        return n->head.hash_opt = (*n->head.kind->hopt_func)(n);
    }
    return 0;
}

node_hash_t
hash_node_opt(NODE *n)
{
    if (!n) return 0;
    if (n->head.flags.has_hash_opt) return n->head.hash_opt;
    return HOPT(n);
}

size_t node_cnt = 0;

// EVAL function — kept for callers that take the function-pointer form.
// Hot internal callers use the EVAL macro from node.h.
RESULT
EVAL_func(CTX *c, NODE *n, JsValue *frame)
{
    return (*n->head.dispatcher)(c, n, frame);
}

// =====================================================================
// ASTro common infrastructure (HASH, DUMP, hash_*, alloc_dispatcher_name).
// astro_node.c references node_allocate / dispatch_info so it must be
// included AFTER those definitions, but BEFORE astro_code_store.c and
// the generated files.
// =====================================================================
#include "astro_node.c"

// HORG implementation now that HASH is defined.
node_hash_t HORG(NODE *n) { return HASH(n); }

#define EVAL(c, n, frame) ((*(n)->head.dispatcher)((c), (n), (frame)))

// Regex engine — included here so it sees JsRegex etc.
#include "js_regex.c"

// =====================================================================
// OPTIMIZE — called from each ALLOC_<name>.  Asks the code store for
// an SD matching this node's hash; if found, patches head.dispatcher
// to the specialized function, otherwise leaves DISPATCH_<name>.
// =====================================================================
static int g_opt_hit = 0;
static int g_opt_miss = 0;
const char *jstro_current_src_file = NULL;

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.no_compiled_code) return n;
    if (astro_cs_load(n, jstro_current_src_file)) g_opt_hit++; else g_opt_miss++;
    return n;
}

void
jstro_optimize_stats(void)
{
    if (OPTION.verbose) fprintf(stderr, "jstro: cs hit=%d miss=%d\n", g_opt_hit, g_opt_miss);
}

// SPECIALIZE is provided by astro_code_store.c.

// =====================================================================
// code_repo: track every closure body so the AOT bake step has a list
// of entry points to specialize (recursive call sites otherwise stay on
// DISPATCH_node_func instead of jumping into a baked SD).
// =====================================================================
struct code_entry {
    const char *name;
    NODE       *body;
};
static struct {
    struct code_entry *entries;
    uint32_t cnt, cap;
} CR = {0};

NODE *code_repo_find(node_hash_t h) { (void)h; return NULL; }
NODE *code_repo_find_by_name(const char *name) { (void)name; return NULL; }

void
code_repo_add(const char *name, NODE *body, bool force)
{
    (void)force;
    if (!body) return;
    if (CR.cnt == CR.cap) {
        CR.cap = CR.cap ? CR.cap * 2 : 16;
        CR.entries = (struct code_entry *)realloc(CR.entries, CR.cap * sizeof(struct code_entry));
    }
    CR.entries[CR.cnt++] = (struct code_entry){name, body};
}

// =====================================================================
// Code store
// =====================================================================
#include "astro_code_store.c"

// =====================================================================
// Generated files
// =====================================================================
// Forward-declare kinds referenced from node_eval.c's swap_dispatcher
// calls before the kind definitions land via node_alloc.c.
extern const struct NodeKind kind_node_smi_lt_ii;
extern const struct NodeKind kind_node_smi_le_ii;
extern const struct NodeKind kind_node_smi_add_ii;

#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_hopt.c"
#include "node_specialize.c"
#include "node_replace.c"
#include "node_alloc.c"

// node_specialized.c is overwritten by `./jstro -c` / `--aot-compile` to
// embed baked SDs at link time.  In the plain interpreter build it is a
// near-empty placeholder produced by the Makefile rule.
#include "node_specialized.c"

// =====================================================================
// SD wrapper post-process: ASTroGen emits inner SD bodies as `static
// inline` so the in-source function-pointer chain devirtualizes inside
// the SD module.  But `static` hides them from `dlsym`, so at runtime
// `astro_cs_load` only finds the externally-named root SD and every
// other AST node falls back to the host binary's DISPATCH_<name>.  We
// rewrite each `SD_<hash>` reference to `SD_<hash>_INL` and append a
// thin `__attribute__((weak)) RESULT SD_<hash>(...)` wrapper that
// forwards to the _INL version — `dlsym` now finds every node and the
// in-source chain still inlines.  Mirrors luastro_export_sd_wrappers.
// =====================================================================
static void
jstro_export_sd_wrappers(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *src = (char *)malloc(sz + 1);
    if (!src) { fclose(fp); return; }
    if (fread(src, 1, sz, fp) != (size_t)sz) { free(src); fclose(fp); return; }
    src[sz] = '\0';
    fclose(fp);

    #define SD_PREFIX_LEN(p) ((p)[0] == 'S' && (p)[1] == 'D' && (p)[2] == '_' ? 3 \
                              : (p)[0] == 'P' && (p)[1] == 'G' && (p)[2] == 'S' \
                                && (p)[3] == 'D' && (p)[4] == '_' ? 5 : 0)
    size_t name_cap = 256, name_cnt = 0;
    char (*names)[24] = (char (*)[24])malloc(name_cap * 24);
    for (const char *p = src; *p; ) {
        bool at_line_start = (p == src) || (p[-1] == '\n');
        size_t plen = at_line_start ? SD_PREFIX_LEN(p) : 0;
        if (plen) {
            const char *q = p + plen;
            while (isxdigit((unsigned char)*q)) q++;
            size_t len = q - p;
            if (len > plen && len < 24 && *q == '(') {
                bool dup = false;
                for (size_t i = 0; i < name_cnt; i++) {
                    if (strncmp(names[i], p, len) == 0 && names[i][len] == '\0') { dup = true; break; }
                }
                if (!dup) {
                    if (name_cnt >= name_cap) {
                        name_cap *= 2;
                        names = (char (*)[24])realloc(names, name_cap * 24);
                    }
                    memcpy(names[name_cnt], p, len);
                    names[name_cnt][len] = '\0';
                    name_cnt++;
                }
            }
            p = q;
        } else {
            p++;
        }
    }

    size_t out_cap = sz + name_cnt * 8 + 4096;
    char *out = (char *)malloc(out_cap);
    size_t out_len = 0;
    for (const char *p = src; *p; ) {
        size_t plen = SD_PREFIX_LEN(p);
        if (plen && (p == src || !(isalnum((unsigned char)p[-1]) || p[-1] == '_'))) {
            const char *q = p + plen;
            while (isxdigit((unsigned char)*q)) q++;
            size_t len = q - p;
            if (len > plen && len < 24) {
                memcpy(out + out_len, p, len);
                out_len += len;
                memcpy(out + out_len, "_INL", 4);
                out_len += 4;
                p = q;
                continue;
            }
        }
        out[out_len++] = *p++;
    }
    #undef SD_PREFIX_LEN

    const char *banner =
        "\n// Externally-visible thin wrappers — make every SD reachable via\n"
        "// dlsym so astro_cs_load can patch every node, not just the chunk root.\n";
    size_t banner_len = strlen(banner);
    if (out_len + banner_len + 1 >= out_cap) {
        out_cap = out_len + banner_len + name_cnt * 128 + 1024;
        out = (char *)realloc(out, out_cap);
    }
    memcpy(out + out_len, banner, banner_len);
    out_len += banner_len;
    for (size_t i = 0; i < name_cnt; i++) {
        char line[256];
        int n = snprintf(line, sizeof(line),
            "__attribute__((weak)) RESULT %s(CTX *c, NODE *n, JsValue *frame) { return %s_INL(c, n, frame); }\n",
            names[i], names[i]);
        if (out_len + n + 1 >= out_cap) {
            out_cap = (out_len + n + 1) * 2;
            out = (char *)realloc(out, out_cap);
        }
        memcpy(out + out_len, line, n);
        out_len += n;
    }
    out[out_len] = '\0';

    fp = fopen(path, "w");
    if (fp) {
        fwrite(out, 1, out_len, fp);
        fclose(fp);
    }
    free(out);
    free(names);
    free(src);
}

static void
jstro_export_all_sds(void)
{
    DIR *d = opendir("code_store/c");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *name = de->d_name;
        size_t nlen = strlen(name);
        if (nlen <= 2 || strcmp(name + nlen - 2, ".c") != 0) continue;
        if (strncmp(name, "SD_", 3) != 0 && strncmp(name, "PGSD_", 5) != 0) continue;
        char path[ASTRO_CS_PATH_MAX];
        snprintf(path, sizeof(path), "code_store/c/%s", name);
        jstro_export_sd_wrappers(path);
    }
    closedir(d);
}

// Variadic-operand children live in JSTRO_NODE_ARR (parser side array)
// — ASTroGen's specializer walks only typed `NODE *` operands, so
// these never get an SD baked.  Compile each one directly so every node
// in the program ends up addressable by dlsym(SD_<hash>).
extern struct Node **JSTRO_NODE_ARR;
extern uint32_t      JSTRO_NODE_ARR_CNT;

static void
jstro_specialize_side_array(const char *file)
{
    for (uint32_t i = 0; i < JSTRO_NODE_ARR_CNT; i++) {
        if (JSTRO_NODE_ARR[i]) astro_cs_compile(JSTRO_NODE_ARR[i], file);
    }
}

void
jstro_specialize_all(NODE *root, const char *file)
{
    if (root) astro_cs_compile(root, file);
    for (uint32_t i = 0; i < CR.cnt; i++) {
        astro_cs_compile(CR.entries[i].body, file);
    }
    jstro_specialize_side_array(file);
    jstro_export_all_sds();
    // Disable stack-clash + stack-protector probes: each alloca'd
    // callee frame in the AOT-inlined call chain otherwise emits
    // ~10 stack-probe stores per function entry, which lights up on
    // recursive benches like fib (~9 M calls).  jstro tracks GC roots
    // via frame_stack so the kernel's guard page is sufficient.
    astro_cs_build("-fno-stack-protector -fno-stack-clash-protection");
    astro_cs_reload();
    if (root) astro_cs_load(root, file);
    for (uint32_t i = 0; i < CR.cnt; i++) {
        astro_cs_load(CR.entries[i].body, file);
    }
    for (uint32_t i = 0; i < JSTRO_NODE_ARR_CNT; i++) {
        if (JSTRO_NODE_ARR[i]) astro_cs_load(JSTRO_NODE_ARR[i], file);
    }
}

// =====================================================================
// INIT — runs once before parsing.
// =====================================================================
void
INIT(void)
{
    astro_cs_init("code_store", ".", 0);
}
