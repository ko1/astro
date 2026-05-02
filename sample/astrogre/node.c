/* memrchr is a GNU extension used by node_action_emit_match_line; needs
 * _GNU_SOURCE set before any system header pulls in <string.h>. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>
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

/* --- Dispatch tracing (no-op) --- */

static void
dispatch_info(CTX *c, NODE *n, bool end)
{
    (void)c; (void)n; (void)end;
}

/* --- ASTro infrastructure (HASH, DUMP, hash funcs, alloc_dispatcher_name) --- */
#include "astro_node.c"

/* --- Code Store (SPECIALIZE, astro_cs_*) --- */
#include "astro_code_store.c"

/* --- User-provided EVAL / OPTIMIZE --- */

VALUE
EVAL(CTX *c, NODE *n)
{
    return (*n->head.dispatcher)(c, n);
}

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

/* Post-process generated code_store/c/SD_<hash>.c files: rename every
 * `SD_<hash>` token in the file to `SD_<hash>_INL` (so internal calls
 * keep the static-inline path) and append externally-visible thin
 * wrappers `SD_<hash>(...)` that just tail-call the renamed body.
 *
 * Why: ASTroGen emits inner SDs as `static inline` so gcc devirtualizes
 * the function-pointer chain inside the SD module.  But `static` makes
 * them invisible to dlsym, so at runtime astro_cs_load only finds the
 * single externally-named root SD — every other AST node falls back to
 * its host-side DISPATCH_ pointer and the chain bounces between the SD
 * and the host's interpreter on every per-node touch.
 *
 * Borrowed wholesale from luastro/node.c (luastro_export_sd_wrappers).
 * The signature here is the simpler `(CTX *, NODE *)` since astrogre
 * doesn't carry an extra frame param. */
static void
astrogre_export_sd_wrappers(const char *path)
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

    /* Idempotency guard: if we've already rewritten this file (our
     * banner is present), skip.  Re-applying stacks `_INL` suffixes —
     * after N runs the originally-named `SD_<hash>` is gone, only
     * `SD_<hash>_INL_INL_..._INL` remains, and dlsym lookups by the
     * canonical name miss.  Encountered when the bench script runs
     * `-C` 8 times in a row across different patterns. */
    if (strstr(src, "// Externally-visible thin wrappers")) {
        free(src);
        return;
    }

    #define SD_PREFIX_LEN(p) ((p)[0] == 'S' && (p)[1] == 'D' && (p)[2] == '_' ? 3 \
                              : (p)[0] == 'P' && (p)[1] == 'G' && (p)[2] == 'S' \
                                && (p)[3] == 'D' && (p)[4] == '_' ? 5 : 0)
    size_t name_cap = 256, name_cnt = 0;
    char (*names)[24] = (char (*)[24])malloc(name_cap * 24);

    /* Collect SD names that appear as function definitions (start of
     * line, immediately followed by `(`). */
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

    /* Rewrite SD_<hash> → SD_<hash>_INL throughout the file. */
    size_t out_cap = (size_t)sz + name_cnt * 8 + 4096;
    char *out = (char *)malloc(out_cap);
    size_t out_len = 0;
    for (const char *p = src; *p; ) {
        size_t plen = SD_PREFIX_LEN(p);
        if (plen && (p == src || !(isalnum((unsigned char)p[-1]) || p[-1] == '_'))) {
            const char *q = p + plen;
            while (isxdigit((unsigned char)*q)) q++;
            size_t len = q - p;
            if (len > plen && len < 24) {
                memcpy(out + out_len, p, len); out_len += len;
                memcpy(out + out_len, "_INL", 4); out_len += 4;
                p = q;
                continue;
            }
        }
        out[out_len++] = *p++;
    }
    #undef SD_PREFIX_LEN

    /* Append extern wrappers. */
    const char *banner =
        "\n// Externally-visible thin wrappers — make every SD reachable\n"
        "// via dlsym so astro_cs_load patches every node, not just the root.\n";
    size_t banner_len = strlen(banner);
    if (out_len + banner_len + 1 >= out_cap) {
        out_cap = out_len + banner_len + name_cnt * 256 + 1024;
        out = (char *)realloc(out, out_cap);
    }
    memcpy(out + out_len, banner, banner_len);
    out_len += banner_len;
    for (size_t i = 0; i < name_cnt; i++) {
        char line[256];
        int n = snprintf(line, sizeof(line),
            "__attribute__((weak)) VALUE %s(CTX *c, NODE *n) { return %s_INL(c, n); }\n",
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

void
astrogre_export_all_sds(void)
{
    DIR *d = opendir("code_store/c");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *name = de->d_name;
        size_t nlen = strlen(name);
        if (nlen <= 2 || strcmp(name + nlen - 2, ".c") != 0) continue;
        if (strncmp(name, "SD_", 3) != 0 && strncmp(name, "PGSD_", 5) != 0) continue;
        char path[512];
        snprintf(path, sizeof(path), "code_store/c/%s", name);
        astrogre_export_sd_wrappers(path);
    }
    closedir(d);
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
