// ASTro build orchestrator implementation.
//
// #include this once from a sample's node.c (alongside astro_node.c and
// astro_code_store.c).  Stand-alone unit; no NODE / specializer hooks.

#include "astro_build.h"
#include "astro_code_store.h"
#include "astro_node.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

// ---------------------------------------------------------------------------
// Small heap arrays (used to accumulate --cflag / --ldflag)
// ---------------------------------------------------------------------------

struct astro_strarr {
    const char **data;
    size_t size;
    size_t capa;
};

static void
astro_strarr_push(struct astro_strarr *a, const char *s)
{
    if (a->size + 1 >= a->capa) {
        size_t capa = a->capa == 0 ? 4 : a->capa * 2;
        a->data = realloc(a->data, sizeof(const char *) * capa);
        if (!a->data) {
            fprintf(stderr, "astro_build: oom\n");
            exit(1);
        }
        a->capa = capa;
    }
    a->data[a->size++] = s ? strdup(s) : NULL;
}

static void
astro_strarr_terminate(struct astro_strarr *a)
{
    astro_strarr_push(a, NULL);
    a->size--;   // sentinel not counted
}

// ---------------------------------------------------------------------------
// Arg parsing
// ---------------------------------------------------------------------------
//
// All flags here are long-form, designed not to collide with per-sample
// short options.  Matched arguments are removed from argv (compacted in
// place); the host's own getopt-style loop runs on the remainder.

// Match "--flag" or "--flag=VAL".  When "--flag VAL" (separate arg), returns
// 1 and the caller bumps i by 1.  When "--flag=VAL", returns 1 and *value
// points at VAL.  Returns 0 on no match.
static int
match_long_kv(const char *arg, const char *flag, const char **value, char **next)
{
    size_t flen = strlen(flag);
    if (strncmp(arg, flag, flen) != 0) return 0;
    if (arg[flen] == '=') {
        *value = arg + flen + 1;
        return 1;  // value in same arg
    }
    if (arg[flen] == '\0') {
        if (!next) return 0;       // expected --flag=VAL form
        *value = *next;
        return 2;  // value in next arg
    }
    return 0;
}

// Parse C-toolchain knobs from a tokenised string (whitespace-split
// ASTRO_BUILD_OPTS value or similar).  Mutates `cfg`.  Returns 0 on success.
//
// Tokens are heap-allocated copies; the caller passes ownership of
// `tokens` (array and strings); we either consume them into cfg->extra_*
// or free them ourselves.  `tokens` MUST be a NULL-terminated array.
static int
parse_c_toolchain_tokens(struct astro_build_config *cfg, char **tokens)
{
    struct astro_strarr cflags = {0};
    struct astro_strarr ldflags = {0};

    for (int i = 0; tokens[i]; i++) {
        const char *a = tokens[i];
        const char *val = NULL;

        if (match_long_kv(a, "--cc", &val, NULL)) {
            cfg->cc = strdup(val);
            continue;
        }
        if (match_long_kv(a, "--sanitize", &val, NULL)) {
            cfg->sanitize = strdup(val);
            continue;
        }
        if (match_long_kv(a, "--cflag", &val, NULL)) {
            astro_strarr_push(&cflags, val);
            continue;
        }
        if (match_long_kv(a, "--ldflag", &val, NULL)) {
            astro_strarr_push(&ldflags, val);
            continue;
        }
        if (match_long_kv(a, "--opt", &val, NULL)) {
            if      (strcmp(val, "0") == 0) cfg->opt_level = 0;
            else if (strcmp(val, "1") == 0) cfg->opt_level = 1;
            else if (strcmp(val, "2") == 0) cfg->opt_level = 2;
            else if (strcmp(val, "3") == 0) cfg->opt_level = 3;
            else if (strcmp(val, "s") == 0) cfg->opt_level = 5;
            else if (strcmp(val, "g") == 0) cfg->opt_level = 6;
            else { fprintf(stderr, "ASTRO_BUILD_OPTS: unknown --opt=%s\n", val); return 1; }
            continue;
        }
        if (strcmp(a, "-O0") == 0) { cfg->opt_level = 0; continue; }
        if (strcmp(a, "-O1") == 0) { cfg->opt_level = 1; continue; }
        if (strcmp(a, "-O2") == 0) { cfg->opt_level = 2; continue; }
        if (strcmp(a, "-O3") == 0) { cfg->opt_level = 3; continue; }
        if (strcmp(a, "-Os") == 0) { cfg->opt_level = 5; continue; }
        if (strcmp(a, "-Og") == 0) { cfg->opt_level = 6; continue; }

        if (strcmp(a, "--debug")       == 0) { cfg->debug = true;  continue; }
        if (strcmp(a, "--no-debug")    == 0) { cfg->debug = false; continue; }
        if (strcmp(a, "--strip")       == 0) { cfg->strip = true;  continue; }
        if (strcmp(a, "--no-strip")    == 0) { cfg->strip = false; continue; }
        if (strcmp(a, "--lto")         == 0) { cfg->lto = true;    continue; }
        if (strcmp(a, "--no-lto")      == 0) { cfg->lto = false;   continue; }
        if (strcmp(a, "--static")      == 0) { cfg->static_link = true;  continue; }
        if (strcmp(a, "--gc-sections") == 0) { cfg->gc_sections = true;  continue; }
        if (strcmp(a, "--show-cmd")    == 0) { cfg->show_cmd = true; continue; }
        if (strcmp(a, "--keep")        == 0) { cfg->keep_intermediates = true; continue; }

        fprintf(stderr, "ASTRO_BUILD_OPTS: unknown token: %s\n", a);
        return 1;
    }

    if (cflags.size > 0) {
        astro_strarr_terminate(&cflags);
        cfg->extra_cflags = cflags.data;
    }
    if (ldflags.size > 0) {
        astro_strarr_terminate(&ldflags);
        cfg->extra_ldflags = ldflags.data;
    }
    return 0;
}

int
astro_build_load_env_opts(struct astro_build_config *cfg)
{
    const char *env = getenv("ASTRO_BUILD_OPTS");
    if (!env || !*env) return 0;

    // Tokenise on whitespace (no quoting support for now — embedded
    // spaces in values aren't expected for typical opts; if needed
    // later we can move to a real shell-like tokenizer).
    char *dup = strdup(env);
    if (!dup) { fprintf(stderr, "ASTRO_BUILD_OPTS: oom\n"); return 1; }

    size_t tcap = 16, tn = 0;
    char **tokens = malloc(sizeof(*tokens) * tcap);
    if (!tokens) { free(dup); return 1; }

    char *p = dup, *tok;
    while ((tok = strtok_r(p, " \t\n\r", &p)) != NULL) {
        if (tn + 1 >= tcap) {
            tcap *= 2;
            tokens = realloc(tokens, sizeof(*tokens) * tcap);
        }
        tokens[tn++] = tok;
    }
    tokens[tn] = NULL;

    int rc = parse_c_toolchain_tokens(cfg, tokens);
    free(dup);
    free(tokens);
    return rc;
}

int
astro_build_extract_flags(int *argc_io, char **argv,
                          struct astro_build_config *cfg)
{
    int argc = *argc_io;
    int wi = 1;   // write index; argv[0] stays

    for (int ri = 1; ri < argc; ri++) {
        const char *a = argv[ri];

        // Stop at the first non-flag positional (= source file).
        // Per Unix convention, tokens after it are passed to the running
        // program as ARGV; we never touch them.
        if (a[0] != '-' || a[1] == '\0') {
            // Copy this and everything after it through unchanged.
            while (ri < argc) argv[wi++] = argv[ri++];
            break;
        }

        // --build PATH: consume next argv element as PATH.
        if (strcmp(a, "--build") == 0) {
            if (ri + 1 >= argc) {
                fprintf(stderr, "astro: --build requires PATH\n");
                return 1;
            }
            cfg->out_exe = argv[ri + 1];
            ri++;
            continue;
        }
        // Attribute / action flags.
        if (strcmp(a, "--plain")       == 0) { cfg->plain = true;       continue; }
        if (strcmp(a, "--compiled-only") == 0) { cfg->compiled_only = true; continue; }
        if (strcmp(a, "--aot-compile") == 0) { cfg->aot_compile = true; continue; }
        if (strcmp(a, "--pg-compile")  == 0) { cfg->pg_compile = true;
                                               cfg->run = true;         continue; }
        if (strcmp(a, "--run")         == 0) { cfg->run = true;         continue; }

        // Universal CLI knobs.
        if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet")   == 0) { cfg->quiet = true;             continue; }
        if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) { cfg->verbose = true;           continue; }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help")    == 0) { cfg->help_requested = true;    continue; }
        if (strcmp(a, "--version") == 0)                         { cfg->version_requested = true; continue; }

        // Other `-...` token — leave for the sample's parser.
        argv[wi++] = argv[ri];
    }
    argv[wi] = NULL;
    *argc_io = wi;

    // Common rule: a *bare* `-v` / `--verbose` — given with no other
    // arguments at all (no files, no sample flags, no build output) — is
    // treated as a version request, so `prog -v` prints the version and
    // exits.  Combined with real work (`prog -v foo`, `prog -v -e ...`),
    // `-v` keeps its "verbose" meaning.
    if (cfg->verbose && wi == 1 && !cfg->out_exe &&
        !cfg->help_requested && !cfg->version_requested) {
        cfg->version_requested = true;
    }

    // Contradiction checks (only relevant if any build-related flag was set).
    if (cfg->plain && cfg->compiled_only) {
        fprintf(stderr, "astro: --plain and --compiled-only are mutually exclusive\n");
        return 1;
    }
    if (cfg->plain && cfg->aot_compile) {
        fprintf(stderr, "astro: --plain and --aot-compile are mutually exclusive\n");
        return 1;
    }
    if (cfg->plain && cfg->pg_compile) {
        fprintf(stderr, "astro: --plain and --pg-compile are mutually exclusive\n");
        return 1;
    }
    if (cfg->aot_compile && cfg->pg_compile) {
        fprintf(stderr, "astro: --aot-compile and --pg-compile are mutually exclusive\n");
        return 1;
    }

    // Load C-toolchain knobs from env.
    return astro_build_load_env_opts(cfg);
}

void
astro_build_config_dispose(struct astro_build_config *cfg)
{
    // Free heap-allocated strings (cc / sanitize from env parse) and
    // the heap arrays for cflags / ldflags.
    free((void *)(uintptr_t)cfg->cc);
    cfg->cc = NULL;
    free((void *)(uintptr_t)cfg->sanitize);
    cfg->sanitize = NULL;
    if (cfg->extra_cflags) {
        for (const char *const *p = cfg->extra_cflags; *p; p++) {
            free((void *)(uintptr_t)*p);
        }
        free((void *)(uintptr_t)cfg->extra_cflags);
        cfg->extra_cflags = NULL;
    }
    if (cfg->extra_ldflags) {
        for (const char *const *p = cfg->extra_ldflags; *p; p++) {
            free((void *)(uintptr_t)*p);
        }
        free((void *)(uintptr_t)cfg->extra_ldflags);
        cfg->extra_ldflags = NULL;
    }
}

// ---------------------------------------------------------------------------
// Compile invocation
// ---------------------------------------------------------------------------
//
// We build one command line as a string and hand it to /bin/sh -c.  This
// keeps the implementation small and lets users pass shell-expandable
// extra-flags if they really want.  Each path is run through a minimal
// quoting (single quotes + escape inner single quote) to avoid most
// surprises.

static char *
sh_squote(const char *s)
{
    size_t len = strlen(s);
    // Worst case: every char becomes "'\''" = 4 chars; plus surrounding ''.
    char *q = malloc(len * 4 + 3);
    if (!q) return NULL;
    char *p = q;
    *p++ = '\'';
    for (const char *c = s; *c; c++) {
        if (*c == '\'') {
            *p++ = '\''; *p++ = '\\'; *p++ = '\''; *p++ = '\'';
        } else {
            *p++ = *c;
        }
    }
    *p++ = '\'';
    *p = '\0';
    return q;
}

static void
sb_append(char **buf, size_t *len, size_t *capa, const char *s)
{
    size_t sl = strlen(s);
    if (*len + sl + 1 >= *capa) {
        size_t capa_new = *capa == 0 ? 256 : *capa * 2;
        while (capa_new < *len + sl + 1) capa_new *= 2;
        *buf = realloc(*buf, capa_new);
        *capa = capa_new;
    }
    memcpy(*buf + *len, s, sl);
    *len += sl;
    (*buf)[*len] = '\0';
}

static void
sb_append_arg(char **buf, size_t *len, size_t *capa, const char *arg)
{
    sb_append(buf, len, capa, " ");
    char *q = sh_squote(arg);
    sb_append(buf, len, capa, q);
    free(q);
}

static const char *
default_cc(void)
{
    const char *v = getenv("ASTRO_CC");
    if (v && *v) return v;
    v = getenv("CC");
    if (v && *v) return v;
    return "cc";
}

int
astro_build_executable(const struct astro_build_config *cfg)
{
    if (!cfg || !cfg->out_exe) {
        fprintf(stderr, "astro_build: out_exe is required\n");
        return 1;
    }
    if (!cfg->src_dir || !cfg->runtime_dir || !cfg->sources) {
        fprintf(stderr, "astro_build: src_dir/runtime_dir/sources are required\n");
        return 1;
    }

    char *cmd = NULL;
    size_t len = 0, capa = 0;

    const char *cc = cfg->cc ? cfg->cc : default_cc();
    sb_append(&cmd, &len, &capa, cc);

    // Optimization.
    int opt = cfg->opt_level;
    if (opt < 0) {
        const char *env = getenv("ASTRO_OPT_LEVEL");
        if (env && *env) opt = atoi(env);
        else opt = 2;
    }
    switch (opt) {
      case 0: sb_append(&cmd, &len, &capa, " -O0"); break;
      case 1: sb_append(&cmd, &len, &capa, " -O1"); break;
      case 2: sb_append(&cmd, &len, &capa, " -O2"); break;
      case 3: sb_append(&cmd, &len, &capa, " -O3"); break;
      case 5: sb_append(&cmd, &len, &capa, " -Os"); break;
      case 6: sb_append(&cmd, &len, &capa, " -Og"); break;
      default: sb_append(&cmd, &len, &capa, " -O2"); break;
    }

    if (cfg->debug)        sb_append(&cmd, &len, &capa, " -ggdb3");
    if (cfg->lto)          sb_append(&cmd, &len, &capa, " -flto");
    if (cfg->static_link)  sb_append(&cmd, &len, &capa, " -static");
    if (cfg->gc_sections) {
        sb_append(&cmd, &len, &capa,
                  " -ffunction-sections -fdata-sections"
                  " -Wl,--gc-sections");
    }
    if (cfg->sanitize) {
        sb_append(&cmd, &len, &capa, " -fsanitize=");
        sb_append(&cmd, &len, &capa, cfg->sanitize);
    }

    // Include paths.
    sb_append(&cmd, &len, &capa, " -I");
    char *q = sh_squote(cfg->src_dir);
    sb_append(&cmd, &len, &capa, q);
    free(q);
    sb_append(&cmd, &len, &capa, " -I");
    q = sh_squote(cfg->runtime_dir);
    sb_append(&cmd, &len, &capa, q);
    free(q);

    // Extra cflags (from env ASTRO_BUILD_OPTS).
    if (cfg->extra_cflags) {
        for (const char *const *p = cfg->extra_cflags; *p; p++) {
            sb_append_arg(&cmd, &len, &capa, *p);
        }
    }
    // Sample-supplied (non-heap, never freed by dispose).
    if (cfg->sample_cflags) {
        for (const char *const *p = cfg->sample_cflags; *p; p++) {
            sb_append_arg(&cmd, &len, &capa, *p);
        }
    }
    // Legacy ASTRO_EXTRA_CFLAGS env (kept for Code Store back-compat).
    const char *ec = getenv("ASTRO_EXTRA_CFLAGS");
    if (ec && *ec) {
        sb_append(&cmd, &len, &capa, " ");
        sb_append(&cmd, &len, &capa, ec);   // raw, allow shell expansion
    }

    // Output exe.
    sb_append(&cmd, &len, &capa, " -o ");
    q = sh_squote(cfg->out_exe);
    sb_append(&cmd, &len, &capa, q);
    free(q);

    // Sources (relative to src_dir).
    for (const char *const *p = cfg->sources; *p; p++) {
        sb_append(&cmd, &len, &capa, " ");
        size_t dl = strlen(cfg->src_dir);
        char *joined = malloc(dl + 1 + strlen(*p) + 1);
        sprintf(joined, "%s/%s", cfg->src_dir, *p);
        char *qj = sh_squote(joined);
        sb_append(&cmd, &len, &capa, qj);
        free(qj);
        free(joined);
    }
    // Extra absolute sources (e.g. generated _embed.c, _static_table.c).
    if (cfg->extra_sources_abs) {
        for (const char *const *p = cfg->extra_sources_abs; *p; p++) {
            sb_append_arg(&cmd, &len, &capa, *p);
        }
    }
    // Extra objects / archives.
    if (cfg->extra_objects) {
        for (const char *const *p = cfg->extra_objects; *p; p++) {
            sb_append_arg(&cmd, &len, &capa, *p);
        }
    }

    // Linker flags (from env ASTRO_BUILD_OPTS).
    if (cfg->extra_ldflags) {
        for (const char *const *p = cfg->extra_ldflags; *p; p++) {
            sb_append_arg(&cmd, &len, &capa, *p);
        }
    }
    // Sample-supplied linker flags.
    if (cfg->sample_ldflags) {
        for (const char *const *p = cfg->sample_ldflags; *p; p++) {
            sb_append_arg(&cmd, &len, &capa, *p);
        }
    }
    const char *el = getenv("ASTRO_EXTRA_LDFLAGS");
    if (el && *el) {
        sb_append(&cmd, &len, &capa, " ");
        sb_append(&cmd, &len, &capa, el);
    }
    // dl is needed by code store dlopen path even when we never call
    // dlopen — astro_code_store.c references dlsym etc.  Targets without
    // libdl (wasm32) opt out via cfg->no_libdl.
    if (!cfg->no_libdl) sb_append(&cmd, &len, &capa, " -ldl");

    if (cfg->show_cmd) {
        fprintf(stderr, "astro_build: %s\n", cmd);
    }

    int ret = system(cmd);
    free(cmd);

    if (ret == 0 && cfg->strip) {
        char *strip_cmd = NULL;
        size_t sl = 0, sc = 0;
        const char *strip_prog = "strip";
        // Match strip to cross prefix when --cc is something like
        // aarch64-linux-gnu-gcc.
        if (cfg->cc) {
            const char *dash = strrchr(cfg->cc, '-');
            if (dash && strncmp(dash + 1, "gcc", 3) == 0) {
                size_t plen = (size_t)(dash - cfg->cc) + 1;
                char *cross = malloc(plen + 6);
                memcpy(cross, cfg->cc, plen);
                memcpy(cross + plen, "strip", 6);
                strip_prog = cross;
            }
        }
        sb_append(&strip_cmd, &sl, &sc, strip_prog);
        sb_append(&strip_cmd, &sl, &sc, " ");
        char *q2 = sh_squote(cfg->out_exe);
        sb_append(&strip_cmd, &sl, &sc, q2);
        free(q2);
        if (cfg->show_cmd) fprintf(stderr, "astro_build: %s\n", strip_cmd);
        int sret = system(strip_cmd);
        if (sret != 0) {
            fprintf(stderr, "astro_build: strip failed (exit %d)\n", sret);
        }
        free(strip_cmd);
        if (strip_prog != (const char *)"strip") free((void *)(uintptr_t)strip_prog);
    }

    return ret;
}

// ---------------------------------------------------------------------------
// One-shot AOT executable builder
// ---------------------------------------------------------------------------

void
astro_build_begin_aot_session(void)
{
    astro_cs_reset_compile_log();
    astro_cs_log_compiles = true;
}

void
astro_build_end_aot_session(void)
{
    astro_cs_log_compiles = false;
    astro_cs_reset_compile_log();
}

int
astro_build_aot_executable(struct Node *root,
                           struct astro_build_config *cfg,
                           const char *code_store_dir)
{
    if (!root || !cfg || !cfg->out_exe) {
        fprintf(stderr, "astro_build_aot_executable: missing root / cfg / out_exe\n");
        return 1;
    }
    if (!code_store_dir) code_store_dir = "code_store";

    // 1. Emit _embed.c.  astro_emit_ast_c_program walks the compile log
    // and bakes dispatcher pointers directly, plus ASTRO_SD_PROTO
    // forward decls for every SD it links to.
    const char *embed_path = "_embed.c";
    FILE *fp = fopen(embed_path, "w");
    if (!fp) { perror(embed_path); return 1; }
    astro_emit_ast_c_program(fp, root, "astro_build_embedded_ast", "node.h");
    fclose(fp);

    // 2. Compose the SD path list from the compile log.
    uint32_t n_sd = astro_cs_compile_log_size();
    const char **sd_paths = NULL;
    if (n_sd > 0) {
        sd_paths = calloc(n_sd, sizeof(*sd_paths));
        for (uint32_t i = 0; i < n_sd; i++) {
            node_hash_t h;
            const char *sd_name = NULL;
            astro_cs_compile_log_get(i, &h, &sd_name);
            (void)h;
            if (!sd_name) continue;
            size_t cap = strlen(code_store_dir) + 4 + strlen(sd_name) + 4;
            char *path = malloc(cap);
            snprintf(path, cap, "%s/c/%s.c", code_store_dir, sd_name);
            sd_paths[i] = path;
        }
    }

    // 3. Build extra_sources_abs = [_embed.c, sd_paths..., NULL].
    size_t extras_cap = 1 + n_sd + 1;
    const char **extras = malloc(sizeof(*extras) * extras_cap);
    extras[0] = embed_path;
    for (uint32_t i = 0; i < n_sd; i++) extras[1 + i] = sd_paths[i];
    extras[1 + n_sd] = NULL;

    // Preserve any extras already set by the caller and chain them in.
    const char *const *prev_extras = cfg->extra_sources_abs;
    if (prev_extras) {
        // Reallocate with room for both.
        size_t prev_n = 0;
        while (prev_extras[prev_n]) prev_n++;
        const char **merged = malloc(sizeof(*merged) * (1 + n_sd + prev_n + 1));
        size_t k = 0;
        merged[k++] = embed_path;
        for (uint32_t i = 0; i < n_sd; i++) merged[k++] = sd_paths[i];
        for (size_t i = 0; i < prev_n; i++) merged[k++] = prev_extras[i];
        merged[k] = NULL;
        free(extras);
        extras = merged;
    }
    cfg->extra_sources_abs = extras;

    // 4. Invoke the toolchain.
    int rc = astro_build_executable(cfg);

    // 5. Cleanup intermediates (unless caller asked to keep).
    if (!cfg->keep_intermediates) {
        unlink(embed_path);
    }
    cfg->extra_sources_abs = prev_extras;
    for (uint32_t i = 0; i < n_sd; i++) free((void *)(uintptr_t)sd_paths[i]);
    free(sd_paths);
    free(extras);
    return rc;
}

void
astro_print_build_help(FILE *fp)
{
    fprintf(fp,
        "ASTro common flags (handled by framework):\n"
        "  --plain          run without using compiled code\n"
        "  --compiled-only  run only compiled code; abort on interpreter dispatch (compile-miss detect)\n"
        "  --aot-compile    AOT-compile (does not run unless --run is given)\n"
        "  --pg-compile     profile-guided compile (implies --run)\n"
        "  --run            execute the program\n"
        "  --build OUT      produce a standalone executable at OUT\n"
        "  -q, --quiet      suppress informational output\n"
        "  -v, --verbose    verbose output (alone, prints version and exits)\n"
        "  -h, --help       show help\n"
        "      --version    show version\n"
        "\n"
        "C-toolchain knobs go in $ASTRO_BUILD_OPTS:\n"
        "  --cc=PATH, -O0..-O3, -Os, -Og, --opt=N,\n"
        "  --debug/--no-debug, --strip/--no-strip, --lto/--no-lto,\n"
        "  --static, --gc-sections, --sanitize=LIST,\n"
        "  --cflag=ARG, --ldflag=ARG, --show-cmd, --keep\n"
    );
}
