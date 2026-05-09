/* arcel_lib.c — implementation of the embedding API in arcel.h.
 *
 * This file is a thin packaging layer over the internal evaluator
 * (context.h / value.h / parser.h / node.h).  Its job is to:
 *   - own the global one-time INIT() (per process)
 *   - bundle a CTX + AST root into an arcel_program handle
 *   - turn arcel_activation_set_* calls into a real arcel_map at
 *     eval time
 *   - expose VALUE through the opaque arcel_value type without copy
 *
 * The CLI in main.c uses these calls; embedders use the same calls
 * via arcel.h.  Everything else (NODE, EVAL, AOT bake, arena) is
 * unchanged.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "arcel.h"
#include "context.h"
#include "value.h"
#include "input.h"
#include "parser.h"
#include "node.h"
#include "astro_code_store.h"

/* arcel_value is documented as opaque {uint64_t[2]}.  Internally it
 * IS a VALUE — same 16-byte tagged union — so we cast/memcpy without
 * marshalling cost.  Keep this assertion alive so future VALUE growth
 * forces an arcel_value bump in the public ABI. */
_Static_assert(sizeof(arcel_value) == sizeof(VALUE),
               "arcel_value layout must match internal VALUE");

static inline arcel_value
to_av(VALUE v)
{
    arcel_value av;
    memcpy(&av, &v, sizeof v);
    return av;
}

static inline VALUE
to_v(arcel_value av)
{
    VALUE v;
    memcpy(&v, &av, sizeof av);
    return v;
}

/* OPTION is referenced by the generated allocator and by the AOT
 * code-store hit/miss tracing in node.c; keep a single instance
 * here so both the CLI and library callers share it. */
struct arcel_option OPTION;

/* ---- env --------------------------------------------------------- */

struct arcel_env_struct {
    bool initialized;
    bool no_compile;
};

/* Single-shot INIT (sets up the AOT code store).  Calling it twice
 * would re-open the .so; the runtime tolerates that but we'd rather
 * gate it. */
static bool g_inited = false;

arcel_env *
arcel_env_new(void)
{
    if (!g_inited) {
        OPTION.quiet = true;     /* default for embedders; CLI can flip back */
        INIT();
        g_inited = true;
    }
    arcel_env *const e = (arcel_env *)calloc(1, sizeof *e);
    e->initialized = true;
    return e;
}

void
arcel_env_free(arcel_env *const env)
{
    free(env);
}

void
arcel_env_set_no_compile(arcel_env *const env, const bool no_compile)
{
    env->no_compile = no_compile;
    /* The AOT decision lives in the global OPTION today; mirror the
     * env's setting onto it so DISPATCH_xxx + the CS bake path see
     * the same value.  Multi-env per process would need decoupling. */
    OPTION.no_compiled_code = no_compile;
}

/* ---- program ----------------------------------------------------- */

struct arcel_program_struct {
    NODE     *root;
    arcel_env *env;
    CTX       ctx;          /* per-eval transient arena + macro stack */
    arcel_map active_bindings;  /* shape used at eval time; entries borrowed */
};

arcel_program *
arcel_compile(arcel_env *const env,
              const char *const src, const ptrdiff_t src_len,
              char *const err_buf, const size_t err_buf_cap)
{
    const uint32_t n = src_len < 0 ? (uint32_t)strlen(src) : (uint32_t)src_len;
    const char *err = NULL;
    NODE *const root = arcel_parse_n(src, n, &err);
    if (!root) {
        if (err_buf && err_buf_cap > 0) {
            snprintf(err_buf, err_buf_cap, "%s", err ? err : "parse failed");
        }
        return NULL;
    }
    if (!env->no_compile && !root->head.flags.is_specialized) {
        astro_cs_compile(root, NULL);
        astro_cs_build(NULL);
        astro_cs_reload();
        astro_cs_load(root, NULL);
    }
    arcel_program *const prg = (arcel_program *)calloc(1, sizeof *prg);
    prg->root = root;
    prg->env  = env;
    arcel_arena_init(&prg->ctx.arena);
    arcel_arena_init(&prg->ctx.bind_arena);
    return prg;
}

void
arcel_program_free(arcel_program *const prg)
{
    if (!prg) return;
    arcel_arena_free(&prg->ctx.arena);
    arcel_arena_free(&prg->ctx.bind_arena);
    /* AST nodes live in the leak-by-design ALLOC_node_* heap pool
     * (see node_allocate in node.c).  We don't free them here —
     * matching the project's short-lived-process assumption.  Long-
     * running embedders that compile many programs will accumulate
     * AST memory; that's the per-parse leak called out in
     * docs/perf.md.  Same applies to interned identifier names. */
    free(prg);
}

/* ---- activation -------------------------------------------------- */

struct arcel_activation_struct {
    arcel_env *env;
    arcel_arena arena;          /* owns name copies + string-binding storage */
    uint32_t    len;
    uint32_t    cap;
    arcel_map_entry *entries;
};

arcel_activation *
arcel_activation_new(arcel_env *const env)
{
    arcel_activation *const a = (arcel_activation *)calloc(1, sizeof *a);
    a->env = env;
    arcel_arena_init(&a->arena);
    return a;
}

void
arcel_activation_free(arcel_activation *const act)
{
    if (!act) return;
    arcel_arena_free(&act->arena);
    free(act->entries);
    free(act);
}

void
arcel_activation_reset(arcel_activation *const act)
{
    arcel_arena_reset(&act->arena);
    act->len = 0;
}

static arcel_map_entry *
act_slot(arcel_activation *const act)
{
    if (act->len == act->cap) {
        act->cap = act->cap ? act->cap * 2 : 8;
        act->entries = (arcel_map_entry *)realloc(act->entries,
                                                  sizeof(arcel_map_entry) * act->cap);
    }
    return &act->entries[act->len++];
}

static VALUE
act_str_key(arcel_activation *const act, const char *const name)
{
    /* Copy `name` into act->arena so it survives caller-side buffer
     * reuse.  Most callers pass string literals which would survive
     * naturally, but the copy keeps the API permissive. */
    const uint32_t n = (uint32_t)strlen(name);
    const char *const owned = arcel_arena_strdup(&act->arena, name, n);
    return V_STR(owned, n);
}

void
arcel_activation_set_null(arcel_activation *const act, const char *const name)
{
    arcel_map_entry *const e = act_slot(act);
    e->key = act_str_key(act, name);
    e->val = V_NULL();
}

void
arcel_activation_set_bool(arcel_activation *const act, const char *const name, const bool v)
{
    arcel_map_entry *const e = act_slot(act);
    e->key = act_str_key(act, name);
    e->val = V_BOOL(v);
}

void
arcel_activation_set_int(arcel_activation *const act, const char *const name, const int64_t v)
{
    arcel_map_entry *const e = act_slot(act);
    e->key = act_str_key(act, name);
    e->val = V_INT(v);
}

void
arcel_activation_set_uint(arcel_activation *const act, const char *const name, const uint64_t v)
{
    arcel_map_entry *const e = act_slot(act);
    e->key = act_str_key(act, name);
    e->val = V_UINT(v);
}

void
arcel_activation_set_double(arcel_activation *const act, const char *const name, const double v)
{
    arcel_map_entry *const e = act_slot(act);
    e->key = act_str_key(act, name);
    e->val = V_DOUBLE(v);
}

void
arcel_activation_set_string(arcel_activation *const act, const char *const name,
                            const char *const s, const size_t len)
{
    arcel_map_entry *const e = act_slot(act);
    e->key = act_str_key(act, name);
    const char *const owned = arcel_arena_strdup(&act->arena, s, (uint32_t)len);
    e->val = V_STR(owned, (uint32_t)len);
}

void
arcel_activation_set_bytes(arcel_activation *const act, const char *const name,
                           const char *const s, const size_t len)
{
    arcel_map_entry *const e = act_slot(act);
    e->key = act_str_key(act, name);
    const char *const owned = arcel_arena_strdup(&act->arena, s, (uint32_t)len);
    e->val = V_BYTES(owned, (uint32_t)len);
}

void
arcel_activation_set_json(arcel_activation *const act, const char *const name,
                          const char *const json, const size_t len)
{
    arcel_map_entry *const e = act_slot(act);
    e->key = act_str_key(act, name);
    e->val = arcel_parse_json(&act->arena, json, (uint32_t)len);
}

void
arcel_activation_set_object(arcel_activation *const act, const char *const name,
                            const void *const obj, const arcel_object_desc *const desc)
{
    arcel_map_entry *const e = act_slot(act);
    e->key = act_str_key(act, name);
    e->val = V_OBJECT(obj, (const struct arcel_object_desc *)desc);
}

int
arcel_activation_load_json(arcel_activation *const act, const char *const json, const size_t len,
                           char *const err_buf, const size_t err_buf_cap)
{
    VALUE v = arcel_parse_json(&act->arena, json, (uint32_t)len);
    if (v.tag == AC_ERR) {
        if (err_buf && err_buf_cap > 0) snprintf(err_buf, err_buf_cap, "%s", v.err ? v.err : "json error");
        return -1;
    }
    if (v.tag == AC_MAP) {
        for (uint32_t i = 0; i < v.map->len; i++) {
            arcel_map_entry *const e = act_slot(act);
            *e = v.map->entries[i];
        }
        return (int)v.map->len;
    }
    /* Non-object input — wrap under "input" (matches CLI behaviour). */
    arcel_map_entry *const e = act_slot(act);
    e->key = V_STR(arcel_arena_strdup(&act->arena, "input", 5), 5);
    e->val = v;
    return 1;
}

/* ---- eval ------------------------------------------------------- */

arcel_value
arcel_eval(arcel_program *const prg, arcel_activation *const act)
{
    /* Reset transient state.  The activation's bindings are NOT
     * copied — we just point the eval ctx at the activation's
     * pre-built entries array.  This is what makes per-iter eval
     * cheap (no O(N_bindings) work per call). */
    arcel_arena_reset(&prg->ctx.arena);
    prg->ctx.bind_top = 0;
    prg->ctx.last_err = NULL;
    if (act && act->len > 0) {
        prg->active_bindings.len     = act->len;
        prg->active_bindings.entries = act->entries;
        prg->ctx.bindings = &prg->active_bindings;
    } else {
        prg->ctx.bindings = NULL;
    }
    return to_av(EVAL(&prg->ctx, prg->root));
}

/* ---- value inspection ------------------------------------------ */

arcel_type
arcel_type_of(arcel_value av)
{
    return (arcel_type)to_v(av).tag;
}

bool        arcel_get_bool  (arcel_value av) { return to_v(av).b; }
int64_t     arcel_get_int   (arcel_value av) { return to_v(av).i; }
uint64_t    arcel_get_uint  (arcel_value av) { return to_v(av).u; }
double      arcel_get_double(arcel_value av) { return to_v(av).d; }

const char *
arcel_get_string(arcel_value av, size_t *const out_len)
{
    VALUE v = to_v(av);
    if (out_len) *out_len = v.s.len;
    return v.s.p;
}

const char *
arcel_get_error(arcel_value av) { return to_v(av).err; }

uint32_t
arcel_list_len(arcel_value av) { return to_v(av).list->len; }

arcel_value
arcel_list_at(arcel_value av, const uint32_t i)
{
    return to_av(to_v(av).list->items[i]);
}

uint32_t
arcel_map_len(arcel_value av) { return to_v(av).map->len; }

arcel_value
arcel_map_key_at(arcel_value av, const uint32_t i)
{
    return to_av(to_v(av).map->entries[i].key);
}

arcel_value
arcel_map_val_at(arcel_value av, const uint32_t i)
{
    return to_av(to_v(av).map->entries[i].val);
}

/* ---- value constructors (for arcel_object_desc::field callbacks) -- */

arcel_value arcel_value_null  (void)                              { return to_av(V_NULL()); }
arcel_value arcel_value_bool  (bool v)                            { return to_av(V_BOOL(v)); }
arcel_value arcel_value_int   (int64_t v)                         { return to_av(V_INT(v)); }
arcel_value arcel_value_uint  (uint64_t v)                        { return to_av(V_UINT(v)); }
arcel_value arcel_value_double(double v)                          { return to_av(V_DOUBLE(v)); }
arcel_value arcel_value_string(const char *s, size_t len)         { return to_av(V_STR(s, (uint32_t)len)); }
arcel_value arcel_value_bytes (const char *s, size_t len)         { return to_av(V_BYTES(s, (uint32_t)len)); }
arcel_value arcel_value_object(const void *obj, const arcel_object_desc *desc)
{
    return to_av(V_OBJECT(obj, (const struct arcel_object_desc *)desc));
}
arcel_value arcel_value_error (const char *msg)                   { return to_av(V_ERR(msg)); }

/* List builder used inside descriptor::field callbacks.  Allocates
 * the arcel_list + items array in the per-eval arena (handed in via
 * the opaque arena handle).  If `items` is non-NULL, copies the
 * provided values; else leaves the items uninitialised for the
 * caller to populate (rare — most callbacks build the items array
 * on the stack and pass it). */
arcel_value
arcel_value_list_new(arcel_arena_handle *const arena_h, const uint32_t len,
                     const arcel_value *const items)
{
    arcel_arena *const arena = (arcel_arena *)arena_h;
    arcel_list *const l = arcel_list_new(arena, len);
    if (items) {
        for (uint32_t i = 0; i < len; i++) l->items[i] = to_v(items[i]);
    }
    return to_av(V_LIST(l));
}

/* Copy a borrowed string into the per-eval arena and return a string
 * value pointing at the owned copy.  Use when the source buffer's
 * lifetime is shorter than the eval (e.g., a std::string scratch
 * inside a descriptor callback). */
arcel_value
arcel_value_string_copy(arcel_arena_handle *const arena_h, const char *const s, const size_t len)
{
    arcel_arena *const arena = (arcel_arena *)arena_h;
    const char *const owned = arcel_arena_strdup(arena, s, (uint32_t)len);
    return to_av(V_STR(owned, (uint32_t)len));
}

arcel_value
arcel_value_bytes_copy(arcel_arena_handle *const arena_h, const char *const s, const size_t len)
{
    arcel_arena *const arena = (arcel_arena *)arena_h;
    const char *const owned = arcel_arena_strdup(arena, s, (uint32_t)len);
    return to_av(V_BYTES(owned, (uint32_t)len));
}

size_t
arcel_format_json(arcel_value av, char *const buf, const size_t buf_cap)
{
    /* Render via FILE* pipe-like memstream so we share the existing
     * arcel_print_json formatter (which writes to FILE*).  open_memstream
     * is GNU; with _GNU_SOURCE on we have it. */
    char  *out_buf = NULL;
    size_t out_len = 0;
    FILE  *fp      = open_memstream(&out_buf, &out_len);
    if (!fp) return 0;
    arcel_print_json(fp, to_v(av));
    fclose(fp);

    if (buf && buf_cap > 0) {
        const size_t n = out_len < buf_cap - 1 ? out_len : buf_cap - 1;
        memcpy(buf, out_buf, n);
        buf[n] = '\0';
    }
    free(out_buf);
    return out_len;
}
