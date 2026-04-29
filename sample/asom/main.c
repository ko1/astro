// asom: entry point.
//
// Mirrors sample/abruby/exe/abruby for vocabulary:
//
//   asom <Class> [args...]                — interpret
//   asom -c <Class> ...                   — AOT-bake every method, then run
//   asom -p <Class> ...                   — run, then PG-bake hot entries
//   asom -c -p <Class> ...                — both (AOT before, PG after)
//   asom --aot-only ...                   — skip PGC index lookup at cs_load
//   asom --plain ...                      — bypass code store entirely
//   asom --code-store=DIR ...             — override code-store location
//   asom --pg-threshold=N ...             — only PG-bake entries hit ≥ N times
//   asom --verbose ...                    — trace cs_* operations
//
// AOT vs PG vocabulary follows abruby:
//   AOT  — astro_cs_compile(body, NULL) → SD_<Horg>.c (structural)
//   PG   — astro_cs_compile(body, file) → PGSD_<Hopt>.c when Hopt != Horg,
//          else SD_<Horg>.c. Indexed via hopt_index.txt at cs_load time.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "context.h"
#include "node.h"
#include "asom_runtime.h"
#include "astro_code_store.h"

struct asom_option OPTION;

extern void asom_install_primitives(CTX *c);
extern void asom_merge_stdlib(CTX *c);

static void
usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [options] <ClassName> [args...]\n"
            "\n"
            "Mode flags (compose):\n"
            "  -c, --aot-compile-first   AOT-bake every method (SD_<Horg>) before run\n"
            "  -p, --pg, --pg-compile    PG-bake hot entries after a clean run\n"
            "      --plain               run without the code store at all\n"
            "      --aot-only            skip PGC index lookup at cs_load time\n"
            "      --compiled-only       warn if interpreter dispatcher is used\n"
            "\n"
            "Tuning:\n"
            "      --pg-threshold=N      only PG-bake entries dispatched ≥ N times\n"
            "                            (default 100, env ASOM_PG_THRESHOLD)\n"
            "      --code-store=DIR      code-store directory\n"
            "                            (default code_store/, env ASOM_CODE_STORE)\n"
            "      --classpath=PATH, -cp colon-separated .som search dirs\n"
            "      --verbose             trace cs_* operations\n"
            "  -q, --quiet               suppress informational lines\n",
            argv0);
}

// Compile one entry. Mirrors abruby's `cs_compile` lambda: NULL `file` =
// AOT (SD_<Horg>); non-NULL `file` = PGC (records (Horg, file) → Hopt in
// hopt_index.txt and emits PGSD_<Hopt>.c when Hopt != Horg).
static void
cs_compile_one(NODE *body, const char *label, const char *file)
{
    if (OPTION.verbose) {
        fprintf(stderr, "cs_compile: %s%s\n", label, file ? " [pgc]" : "");
    }
    astro_cs_compile(body, file);
}

static int
parse_int_env(const char *name, int dflt)
{
    const char *v = getenv(name);
    if (!v || !*v) return dflt;
    return atoi(v);
}

int
main(int argc, char **argv)
{
    OPTION.classpath = "./SOM/Smalltalk:./SOM/Examples:./SOM/TestSuite:.";
    OPTION.pg_threshold = parse_int_env("ASOM_PG_THRESHOLD", 100);
    OPTION.code_store_dir = getenv("ASOM_CODE_STORE");

    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != '\0') {
        const char *a = argv[i];
        if (strcmp(a, "-cp") == 0 && i + 1 < argc) { OPTION.classpath = argv[++i]; i++; continue; }
        if (strncmp(a, "--classpath=", 12) == 0)   { OPTION.classpath = a + 12; i++; continue; }
        if (strcmp(a, "--dump-ast") == 0)          { OPTION.dump_ast = true; i++; continue; }
        if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) { OPTION.quiet = true; i++; continue; }
        if (strcmp(a, "--plain") == 0)             { OPTION.no_compiled_code = true; i++; continue; }
        if (strcmp(a, "-c") == 0 || strcmp(a, "--aot-compile-first") == 0) {
            OPTION.aot_compile_first = true; i++; continue;
        }
        if (strcmp(a, "-p") == 0 || strcmp(a, "--pg") == 0 || strcmp(a, "--pg-compile") == 0) {
            OPTION.pgc_at_exit = true; i++; continue;
        }
        if (strncmp(a, "--pg-threshold=", 15) == 0) { OPTION.pg_threshold = atoi(a + 15); i++; continue; }
        if (strcmp(a, "--aot-only") == 0)          { OPTION.aot_only = true; i++; continue; }
        if (strcmp(a, "--compiled-only") == 0)     { OPTION.compiled_only = true; i++; continue; }
        if (strncmp(a, "--code-store=", 13) == 0)  { OPTION.code_store_dir = a + 13; i++; continue; }
        if (strncmp(a, "--preload=", 10) == 0)     { OPTION.preload = a + 10; i++; continue; }
        if (strcmp(a, "--verbose") == 0)           { OPTION.verbose = true; i++; continue; }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(argv[0]); return 0; }
        fprintf(stderr, "asom: unknown flag '%s'\n", a);
        usage(argv[0]); return 1;
    }
    if (i >= argc) { usage(argv[0]); return 1; }
    OPTION.entry_class = argv[i++];

    INIT();
    CTX *c = calloc(1, sizeof(*c));
    asom_runtime_init(c);
    asom_install_primitives(c);
    asom_merge_stdlib(c);

    struct asom_class *entry = asom_load_class(c, OPTION.entry_class);
    if (!entry) {
        fprintf(stderr, "asom: cannot find class %s in classpath '%s'\n",
                OPTION.entry_class, OPTION.classpath ? OPTION.classpath : ".");
        return 1;
    }

    // --preload: eagerly load extra classes before AOT bake. Benchmarks
    // that resolve their target class via `system global:` need the target
    // loaded before -c sees it; this lets `make compare` pre-stage the
    // benchmark class for AOT.
    if (OPTION.preload) {
        char *buf = strdup(OPTION.preload);
        for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
            if (!asom_load_class(c, tok) && OPTION.verbose) {
                fprintf(stderr, "preload: class %s not found\n", tok);
            }
        }
        free(buf);
    }

    // ----- AOT-compile-first (-c): bake every accumulated entry, build,
    // reload, and rebind dispatchers. Same flow as abruby's --aot-compile-first.
    if (OPTION.aot_compile_first && !OPTION.no_compiled_code) {
        uint32_t n;
        struct asom_entry *ents = asom_entries(&n);
        if (OPTION.verbose) fprintf(stderr, "aot: %u entries\n", n);
        for (uint32_t k = 0; k < n; k++) {
            cs_compile_one(ents[k].body, ents[k].label, NULL); // SD_<Horg>
        }
        if (OPTION.verbose) fprintf(stderr, "cs_build\n");
        astro_cs_build(NULL);
        astro_cs_reload();
        // Rebind every accumulated entry so dispatchers point at the new
        // image's SD_<Horg>. Without this, ASTroGen-generated dispatchers
        // installed at parse time would still target the in-process
        // default; the dlopen image is only used after cs_load swaps it in.
        uint32_t ok = 0;
        for (uint32_t k = 0; k < n; k++) {
            // -c always uses AOT-style lookup (NULL file) — there is no
            // PGC index built yet on the first run.
            bool loaded = astro_cs_load(ents[k].body, NULL);
            if (loaded) ok++;
            if (OPTION.verbose && !loaded) {
                fprintf(stderr, "cs_load miss: %s (hash=%lx)\n",
                        ents[k].label, (unsigned long)HASH(ents[k].body));
            }
        }
        if (OPTION.verbose) fprintf(stderr, "cs_load: %u/%u dispatchers swapped\n", ok, n);
    }

    if (!OPTION.quiet) {
        fprintf(stderr, "asom: loaded %s (%u methods)\n",
                entry->name, entry->methods.cnt);
    }

    VALUE recv = asom_object_new(c, entry);
    struct asom_frame top = { .self = recv };
    top.home = &top;
    c->frame = &top;

    struct asom_method *run_args = asom_class_lookup(entry, asom_intern_cstr("run:"));
    if (run_args) {
        int extra = argc - i;
        int total = extra + 1;
        VALUE arr = asom_array_new(c, (uint32_t)total);
        struct asom_array *a = (struct asom_array *)ASOM_VAL2OBJ(arr);
        a->data[0] = asom_string_new(c, OPTION.entry_class, strlen(OPTION.entry_class));
        for (int j = 0; j < extra; j++) {
            a->data[j + 1] = asom_string_new(c, argv[i + j], strlen(argv[i + j]));
        }
        VALUE args[1] = { arr };
        asom_send(c, recv, asom_intern_cstr("run:"), 1, args, NULL);
    } else {
        struct asom_method *run = asom_class_lookup(entry, asom_intern_cstr("run"));
        if (!run) {
            fprintf(stderr, "asom: %s has no 'run' or 'run:' method\n", entry->name);
            return 1;
        }
        asom_send(c, recv, asom_intern_cstr("run"), 0, NULL, NULL);
    }

    // ----- PG-compile (-p): after a clean run, partition entries by
    // dispatch_count and bake the hot ones. Cold entries stay
    // interpreter-dispatched on the next run.
    if (OPTION.pgc_at_exit && !OPTION.no_compiled_code) {
        uint32_t n;
        struct asom_entry *ents = asom_entries(&n);
        uint32_t hot = 0, cold = 0, pgc = 0, aot = 0;
        for (uint32_t k = 0; k < n; k++) {
            unsigned int dc = ents[k].body->head.dispatch_cnt;
            if ((int)dc < OPTION.pg_threshold) { cold++; continue; }
            hot++;
            // Hopt vs Horg: when profile-aware hash equals structural hash
            // we just emit SD_<Horg>.c (no need for a separate PGSD_).
            if (HOPT(ents[k].body) != HORG(ents[k].body)) {
                cs_compile_one(ents[k].body, ents[k].label, ents[k].file ? ents[k].file : "-");
                pgc++;
            } else {
                cs_compile_one(ents[k].body, ents[k].label, NULL);
                aot++;
            }
        }
        if (OPTION.verbose) {
            fprintf(stderr, "pgc bake: pgc=%u aot=%u cold(skipped)=%u of %u (threshold=%d)\n",
                    pgc, aot, cold, n, OPTION.pg_threshold);
        }
        if (hot > 0) {
            if (OPTION.verbose) fprintf(stderr, "cs_build\n");
            astro_cs_build(NULL);
        }
    }
    extern void asom_print_optimize_stats(void);
    extern unsigned int asom_swap_dispatcher_count(void);
    if (OPTION.verbose) {
        asom_print_optimize_stats();
        fprintf(stderr, "asom: swap_dispatcher fired %u times\n",
                asom_swap_dispatcher_count());
    }
    return 0;
}
