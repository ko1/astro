#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "node.h"
#include "context.h"

NODE *PARSE_FILE(CTX *c, const char *path);
extern uint32_t JSTRO_TOP_NLOCALS;
void jstro_install_stdlib(CTX *c);
void jstro_specialize_all(NODE *root, const char *file);
void jstro_optimize_stats(void);
void jstro_node_arr_publish(void);

struct jstro_option OPTION = {0};
extern unsigned long jstro_shape_find_count;
extern unsigned long jstro_object_set_count;
extern unsigned long jstro_call_ic_miss;
static bool g_dump_ic = false;

// File the parser is currently reading.  Threaded into OPTIMIZE so PGC
// (HORG, file, line) → HOPT lookup works.  jstro doesn't run a separate
// profile hash today (HOPT == HORG) so this is mostly a placeholder for
// future extension; the AOT path uses NULL anyway.
extern const char *jstro_current_src_file;

// =====================================================================
// Run the program body on a worker thread with a much larger C stack
// than the default 8 MB.  AOT-baked SDs nest the per-node call chain
// inline so each JS function call grows the C stack by hundreds of
// bytes — deep loops at high iteration counts blow through 8 MB.
// 4 GB virtual is plenty and is committed lazily.  Mirrors
// luastro_run_on_big_stack.
// =====================================================================
typedef struct { int (*entry)(void *); void *arg; int rc; } jstro_thread_arg;

static void *
jstro_thread_main(void *p)
{
    jstro_thread_arg *a = (jstro_thread_arg *)p;
    a->rc = a->entry(a->arg);
    return NULL;
}

static int
jstro_run_on_big_stack(int (*entry)(void *), void *arg)
{
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4ULL * 1024 * 1024 * 1024);  // 4 GiB virtual
    jstro_thread_arg ta = { entry, arg, 0 };
    pthread_t tid;
    if (pthread_create(&tid, &at, jstro_thread_main, &ta) != 0) {
        pthread_attr_destroy(&at);
        return entry(arg);   // fallback: run on main stack
    }
    pthread_attr_destroy(&at);
    pthread_join(tid, NULL);
    return ta.rc;
}

struct run_args {
    CTX     *c;
    NODE    *body;
    JsValue *frame;
    JsValue  result;
    int      rc;
};

static int
jstro_run_thunk(void *p)
{
    struct run_args *a = (struct run_args *)p;
    struct js_frame_link top_link = { a->frame, JSTRO_TOP_NLOCALS, NULL, 0, NULL };
    a->c->frame_stack = &top_link;
    a->result = EVAL_func(a->c, a->body, a->frame);
    a->rc = (JSTRO_BR == JS_BR_THROW) ? 1 : 0;
    return a->rc;
}

static void
usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [options] file.js\n"
        "options:\n"
        "  -q, --quiet          suppress non-essential output\n"
        "  -v                   verbose (cs hit/miss stats etc.)\n"
        "  --show-result        print the value of the last top-level expr\n"
        "  --dump               dump the parsed AST\n"
        "  --dump-ic            print IC and GC counters at exit\n"
        "  -c, --aot-compile-first\n"
        "                       AOT-bake SDs into code_store/, then run with them active\n"
        "  --aot-compile        AOT-bake SDs into code_store/ and exit (no run)\n"
        "  -p, --pg-compile     run first (profile), then bake SDs (currently == -c)\n"
        "  --no-compile         don't consult / write code store (pure interpreter)\n",
        argv0);
}

int
main(int argc, char *argv[])
{
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "-q") || !strcmp(a, "--quiet"))         OPTION.quiet = true;
        else if (!strcmp(a, "-v"))                                  OPTION.verbose = true;
        else if (!strcmp(a, "--show-result"))                       OPTION.show_result = true;
        else if (!strcmp(a, "--no-compile"))                        OPTION.no_compiled_code = true;
        else if (!strcmp(a, "--dump"))                              OPTION.dump_ast = true;
        else if (!strcmp(a, "--dump-ic"))                           g_dump_ic = true;
        else if (!strcmp(a, "-c") || !strcmp(a, "--aot-compile-first"))
                                                                    OPTION.compile_first = true;
        else if (!strcmp(a, "--aot-compile"))                       OPTION.aot_only = true;
        else if (!strcmp(a, "-p") || !strcmp(a, "--pg-compile"))    OPTION.pg_mode = true;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help"))          { usage(argv[0]); return 0; }
        else if (a[0] != '-')                                       path = a;
        else { fprintf(stderr, "%s: unknown option %s\n", argv[0], a); usage(argv[0]); return 1; }
    }
    if (!path) { usage(argv[0]); return 1; }

    INIT();

    CTX *c = js_create_context();
    js_init_globals(c);
    jstro_install_stdlib(c);

    jstro_current_src_file = path;
    NODE *body = PARSE_FILE(c, path);
    if (!body) { fprintf(stderr, "parse failed\n"); return 1; }
    jstro_node_arr_publish();

    if (OPTION.dump_ast) {
        DUMP(stderr, body, true);
        fputc('\n', stderr);
    }

    // Modes:
    //   default              — interpret with whatever code-store SDs are loaded
    //   -c / --aot-compile-first — bake SDs first (AOT), then run
    //   --aot-compile        — bake AOT and exit (used by benchmark setup)
    //   -p / --pg-compile    — run first, then bake.  jstro has no kind-
    //                          swapping today so PG bake is equivalent to AOT;
    //                          we keep the flag so the Makefile target works
    //                          and so future profile-driven specialization can
    //                          slot in without a CLI churn.
    if (OPTION.compile_first || OPTION.aot_only) {
        jstro_specialize_all(body, NULL);
        if (OPTION.aot_only) return 0;
    }

    JsValue *frame = (JsValue *)calloc(JSTRO_TOP_NLOCALS + 16, sizeof(JsValue));
    struct run_args ra = { c, body, frame, JV_UNDEFINED, 0 };
    jstro_run_on_big_stack(jstro_run_thunk, &ra);
    if (ra.rc) {
        fprintf(stderr, "Uncaught: ");
        js_print_value(c, stderr, JSTRO_BR_VAL);
        fputc('\n', stderr);
        return 1;
    }
    if (OPTION.show_result) {
        js_print_value(c, stdout, ra.result);
        fputc('\n', stdout);
    }

    if (OPTION.pg_mode) {
        // PG mode: pass the source filename so astro_cs_compile names
        // the baked SDs PGSD_<HOPT> (profile-aware).  Subsequent runs
        // load PGSDs via the (HORG, file, line) → HOPT index, picking
        // up post-swap_dispatcher specialisations.
        jstro_specialize_all(body, path);
    }
    jstro_optimize_stats();

    if (g_dump_ic) {
        extern unsigned long jstro_gc_run_count;
        fprintf(stderr, "[IC] js_shape_find_slot: %lu\n", jstro_shape_find_count);
        fprintf(stderr, "[IC] js_object_set:      %lu\n", jstro_object_set_count);
        fprintf(stderr, "[IC] call_ic_miss:       %lu\n", jstro_call_ic_miss);
        fprintf(stderr, "[GC] collections:        %lu\n", jstro_gc_run_count);
    }
    return 0;
}
