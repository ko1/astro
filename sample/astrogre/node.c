/* memrchr is a GNU extension used by node_action_emit_match_line; needs
 * _GNU_SOURCE set before any system header pulls in <string.h>. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include "node.h"
#include "context.h"
#include "aho_corasick.h"

/* --- Allocation --- */

/* Side array of every allocated NODE.  Lets astrogre_pattern_aot_compile
 * re-resolve each node's dispatcher after a fresh code-store build,
 * not just the AST root — without it, only the root SD would ever be
 * picked up by dlsym and inner nodes would still bounce through the
 * host's interpreter on every per-node touch. */
size_t astrogre_node_cnt;
static struct {
    NODE **arr;
    size_t cnt, cap;
} astrogre_all_nodes;

static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *n = (NODE *)calloc(1, size);
    if (n == NULL) {
        fprintf(stderr, "astrogre: out of memory\n");
        exit(EXIT_FAILURE);
    }
    astrogre_node_cnt++;
    if (astrogre_all_nodes.cnt == astrogre_all_nodes.cap) {
        size_t cap = astrogre_all_nodes.cap ? astrogre_all_nodes.cap * 2 : 64;
        astrogre_all_nodes.arr = (NODE **)realloc(astrogre_all_nodes.arr, cap * sizeof(NODE *));
        astrogre_all_nodes.cap = cap;
    }
    astrogre_all_nodes.arr[astrogre_all_nodes.cnt++] = n;
    return n;
}

/* defined later — astro_cs_load lives inside astro_code_store.c which is
 * included below.  This function is also defined below. */

/* --- ASTro infrastructure (HASH, DUMP, hash funcs, alloc_dispatcher_name) --- */
#include "astro_node.c"

/* --- Code Store (SPECIALIZE, astro_cs_*) --- */
#include "astro_code_store.c"

/* --- User-provided OPTIMIZE / specialize hooks --- */

/* EVAL is a macro defined in node.h. */

static int g_cs_hit = 0, g_cs_miss = 0;

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.no_compiled_code) return n;
    bool hit = astro_cs_load(n, NULL);
    if (hit) g_cs_hit++;
    else     g_cs_miss++;
    if (OPTION.cs_verbose) {
        fprintf(stderr, "%s: h=%016lx %s\n",
                hit ? "cs_hit " : "cs_miss",
                (unsigned long)hash_node(n),
                n->head.kind->default_dispatcher_name);
    }
    return n;
}

void
astrogre_cs_stats(void)
{
    if (g_cs_hit || g_cs_miss) {
        fprintf(stderr, "astrogre: cs hit=%d miss=%d\n", g_cs_hit, g_cs_miss);
    }
}

void
astrogre_reload_all_dispatchers(void)
{
    for (size_t i = 0; i < astrogre_all_nodes.cnt; i++) {
        astro_cs_load(astrogre_all_nodes.arr[i], NULL);
    }
}

/* code_repo: stub for the framework's record_all option */
void
code_repo_add(const char *name, NODE *body, bool force)
{
    (void)name; (void)body; (void)force;
}

/* --- Generated code --- */
#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"
#include "node_replace.c"
#include "node_alloc.c"

/* --- INIT --- */

void
INIT(void)
{
    /* astro_cs_build invokes `make` via system().  The default cc on
     * many distros is a ccache wrapper which falls over when the cache
     * dir is read-only (sandboxed builds, CI, …).  Disable ccache for
     * the whole process so make's compiler probe and its child cc both
     * see it; harmless when ccache isn't present. */
    setenv("CCACHE_DISABLE", "1", 0);

    /* Resolve the source directory (where node.h lives) once at INIT.
     * Using "." would tie cs_compile to the cwd of the *invoking shell*,
     * which breaks when the user runs astrogre from a different
     * directory (e.g. bench/) — the generated SD .c files would
     * `#include "<cwd>/node.h"` and compilation would fail.  We
     * read the binary's own path via /proc/self/exe and strip the
     * filename, which gives us a stable absolute src dir even when
     * cwd != bin dir. */
    char src_dir[PATH_MAX];
    src_dir[0] = '.'; src_dir[1] = '\0';
    char exe_path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (n > 0) {
        exe_path[n] = '\0';
        char *slash = strrchr(exe_path, '/');
        if (slash) {
            *slash = '\0';
            strncpy(src_dir, exe_path, sizeof(src_dir) - 1);
            src_dir[sizeof(src_dir) - 1] = '\0';
        }
    }
    astro_cs_init("code_store", src_dir, 0);
}
