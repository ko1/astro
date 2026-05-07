/*
 * runtime.c — semantic helpers for node.def evaluators (EMIT form).
 *
 * Each helper takes c->input as the current input, appends emits to
 * c->pool, and returns an EMIT slice descriptor.
 *
 * Hot-path helpers were inlined into node.def NODE_DEF bodies so the
 * SD specializer can fold them.  The helpers here are the longer /
 * less common ones (object construction, interp, format, *_by, etc.).
 * They use EVAL (runtime-resolved) on sub-nodes — the SD specializer
 * can't fold past these calls, but for less-frequent operations the
 * cost is acceptable.
 */
#include "context.h"
#include "node.h"
#include <ctype.h>

/* --- side tables --------------------------------------------------------- */

static VALUE *lit_tab = NULL; static size_t lit_len = 0, lit_capa = 0;

uint32_t
nuq_lit_intern(VALUE v)
{
    if (lit_len == lit_capa) {
        lit_capa = lit_capa ? lit_capa * 2 : 16;
        lit_tab = (VALUE *)GC_realloc(lit_tab, lit_capa * sizeof(VALUE));
    }
    lit_tab[lit_len] = v;
    return (uint32_t)lit_len++;
}

VALUE nuq_lit_get(uint32_t id) { return lit_tab[id]; }

struct interp_entry { size_t cnt; struct Node **parts; };
static struct interp_entry *interp_tab = NULL;
static size_t interp_len = 0, interp_capa = 0;

uint32_t
nuq_interp_intern(struct Node **parts, size_t cnt)
{
    if (interp_len == interp_capa) {
        interp_capa = interp_capa ? interp_capa * 2 : 16;
        interp_tab = (struct interp_entry *)GC_realloc(interp_tab, interp_capa * sizeof(*interp_tab));
    }
    interp_tab[interp_len].cnt = cnt;
    interp_tab[interp_len].parts = parts;
    return (uint32_t)interp_len++;
}

struct obj_ctor { size_t cnt; struct nuq_obj_entry *items; };
static struct obj_ctor *obj_tab = NULL;
static size_t obj_tab_len = 0, obj_tab_capa = 0;

uint32_t
nuq_obj_ctor_intern(struct nuq_obj_entry *items, size_t cnt)
{
    if (obj_tab_len == obj_tab_capa) {
        obj_tab_capa = obj_tab_capa ? obj_tab_capa * 2 : 16;
        obj_tab = (struct obj_ctor *)GC_realloc(obj_tab, obj_tab_capa * sizeof(*obj_tab));
    }
    obj_tab[obj_tab_len].cnt = cnt;
    obj_tab[obj_tab_len].items = items;
    return (uint32_t)obj_tab_len++;
}

struct args_entry { size_t cnt; struct Node **args; };
static struct args_entry *args_tab = NULL;
static size_t args_tab_len = 0, args_tab_capa = 0;

uint32_t
nuq_args_intern(struct Node **args, size_t cnt)
{
    if (args_tab_len == args_tab_capa) {
        args_tab_capa = args_tab_capa ? args_tab_capa * 2 : 16;
        args_tab = (struct args_entry *)GC_realloc(args_tab, args_tab_capa * sizeof(*args_tab));
    }
    args_tab[args_tab_len].cnt = cnt;
    args_tab[args_tab_len].args = args;
    return (uint32_t)args_tab_len++;
}

struct def_block {
    size_t cnt;
    struct nuq_def_entry *items;
    /* Cache of `nuq_func_def`s materialised by `nuq_defs_eval` —
     * allocated lazily on first use, reused across runs (the def_entry
     * fields are immutable, scope_top is the same for the same call
     * site).  Without this we'd leak `cnt` fds per `nuq_run`. */
    struct nuq_func_def **fds_cache;
    size_t                fds_cache_scope_top;
};
static struct def_block *def_tab = NULL;
static size_t def_tab_len = 0, def_tab_capa = 0;

/* For nuq_compile_all_def_bodies / nuq_load_all_def_bodies below. */
#include "astro_code_store.h"

uint32_t
nuq_def_block_intern(struct nuq_def_entry *items, size_t cnt)
{
    if (def_tab_len == def_tab_capa) {
        def_tab_capa = def_tab_capa ? def_tab_capa * 2 : 16;
        def_tab = (struct def_block *)GC_realloc(def_tab, def_tab_capa * sizeof(*def_tab));
    }
    /* Zero-init this slot — realloc doesn't zero the extension. */
    memset(&def_tab[def_tab_len], 0, sizeof(*def_tab));
    def_tab[def_tab_len].cnt = cnt;
    def_tab[def_tab_len].items = items;
    return (uint32_t)def_tab_len++;
}

void
nuq_compile_all_def_bodies(void)
{
    for (size_t i = 0; i < def_tab_len; i++) {
        const struct def_block *const db = &def_tab[i];
        for (size_t j = 0; j < db->cnt; j++) {
            NODE *const body = db->items[j].body;
            if (!body->head.flags.is_specialized) {
                astro_cs_compile(body, NULL);
            }
        }
    }
}

/* Run linearity analysis on every interned user-def body.  Called
 * after parse so the rewrite is visible to AOT compile / dump. */
void
nuq_linearity_analyze_all_defs(void)
{
    extern void nuq_linearity_analyze_def_body(struct Node *body);
    for (size_t i = 0; i < def_tab_len; i++) {
        const struct def_block *const db = &def_tab[i];
        for (size_t j = 0; j < db->cnt; j++) {
            nuq_linearity_analyze_def_body(db->items[j].body);
        }
    }
}

void
nuq_load_all_def_bodies(void)
{
    for (size_t i = 0; i < def_tab_len; i++) {
        const struct def_block *const db = &def_tab[i];
        for (size_t j = 0; j < db->cnt; j++) {
            astro_cs_load(db->items[j].body, NULL);
        }
    }
}

/* Destructuring pattern interning.  Patterns are heap-allocated and
 * referenced by id from `node_as_pattern`. */
static struct nuq_pat **pat_tab = NULL;
static size_t pat_tab_len = 0, pat_tab_capa = 0;

uint32_t
nuq_pat_intern(struct nuq_pat *p)
{
    if (pat_tab_len == pat_tab_capa) {
        pat_tab_capa = pat_tab_capa ? pat_tab_capa * 2 : 16;
        pat_tab = (struct nuq_pat **)GC_realloc(
            pat_tab, pat_tab_capa * sizeof(*pat_tab));
    }
    pat_tab[pat_tab_len] = p;
    return (uint32_t)pat_tab_len++;
}

struct nuq_pat *
nuq_pat_get(uint32_t id)
{
    return pat_tab[id];
}

static void
pat_bind_inner(CTX *c, struct nuq_pat *p, VALUE v)
{
    if (p->kind == NUQ_PAT_VAR) {
        nuq_var_push(c, p->u.var_id, v);
    } else if (p->kind == NUQ_PAT_ARRAY) {
        for (size_t i = 0; i < p->u.arr.len; i++) {
            VALUE elem = NUQ_NULL;
            if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY &&
                i < NUQ_PTR(v)->arr.len)
                elem = NUQ_PTR(v)->arr.items[i];
            pat_bind_inner(c, p->u.arr.items[i], elem);
        }
    } else {                              /* NUQ_PAT_OBJECT */
        for (size_t i = 0; i < p->u.obj.len; i++) {
            struct nuq_pat_obj_entry *ent = &p->u.obj.items[i];
            VALUE child = NUQ_NULL;
            if (ent->key_expr) {
                /* Dynamic key: evaluate against the value's input. */
                size_t t0 = c->pool_top;
                EMIT ke = EVAL(c, ent->key_expr);
                if (c->error == NUQ_NULL && ke.count > 0) {
                    VALUE k = ke.items[0];
                    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT &&
                        NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_STRING)
                        child = nuq_object_get(v, k);
                }
                c->pool_top = t0;
            } else if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
                child = nuq_object_get_cstr(v, ent->key);
            }
            pat_bind_inner(c, ent->val, child);
        }
    }
}

size_t
nuq_pat_bind(CTX *c, struct nuq_pat *p, VALUE v)
{
    size_t saved = c->var_top;
    pat_bind_inner(c, p, v);
    return saved;
}

/* `?//` alt-pattern support.  Each alt's pattern is tried in order;
 * the first whose top-level shape matches the value is used to bind.
 * Variables referenced by the body but not bound in the chosen alt
 * default to null — so we pre-bind every var seen across all alts
 * to null, then layer the chosen alt's bindings on top.  Last-binding
 * wins in nuq_var_get's top-down scan. */

/* Collect all var_ids referenced by a pattern into out[] (no dedup). */
static void
pat_collect_vars(const struct nuq_pat *p, uint32_t *out, size_t *cnt, size_t cap)
{
    if (p->kind == NUQ_PAT_VAR) {
        if (*cnt < cap) out[(*cnt)++] = p->u.var_id;
    } else if (p->kind == NUQ_PAT_ARRAY) {
        for (size_t i = 0; i < p->u.arr.len; i++)
            pat_collect_vars(p->u.arr.items[i], out, cnt, cap);
    } else {
        for (size_t i = 0; i < p->u.obj.len; i++)
            pat_collect_vars(p->u.obj.items[i].val, out, cnt, cap);
    }
}

/* Does this pattern's top-level shape match the value's type?
 * Variable patterns always match.  Array patterns require array.
 * Object patterns require object.  We don't recurse — jq only needs
 * a single-level type check to choose between alternatives. */
static bool
pat_fits(const struct nuq_pat *p, VALUE v)
{
    if (p->kind == NUQ_PAT_VAR) return true;
    if (p->kind == NUQ_PAT_ARRAY)
        return NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY;
    return NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT;
}

/* Side table: each entry holds an array of pat_ids forming an alt set. */
struct pat_alt { uint32_t *pids; size_t cnt; };
static struct pat_alt *pat_alt_tab = NULL;
static size_t pat_alt_len = 0, pat_alt_capa = 0;

uint32_t
nuq_pat_alt_intern(uint32_t *pids, size_t cnt)
{
    if (pat_alt_len == pat_alt_capa) {
        pat_alt_capa = pat_alt_capa ? pat_alt_capa * 2 : 8;
        pat_alt_tab = (struct pat_alt *)GC_realloc(
            pat_alt_tab, pat_alt_capa * sizeof(*pat_alt_tab));
    }
    uint32_t *copy = (uint32_t *)GC_malloc(cnt * sizeof(uint32_t));
    memcpy(copy, pids, cnt * sizeof(uint32_t));
    pat_alt_tab[pat_alt_len].pids = copy;
    pat_alt_tab[pat_alt_len].cnt = cnt;
    return (uint32_t)pat_alt_len++;
}

EMIT
nuq_as_alt_eval(CTX *c, struct Node *lhs, uint32_t alt_id, struct Node *body)
{
    struct pat_alt *pa = &pat_alt_tab[alt_id];
    size_t outer_top = c->pool_top;
    EMIT le = EVAL(c, lhs);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t lc = le.count;
    VALUE small[16];
    VALUE *lhs_local = (lc <= 16) ? small : (VALUE *)nuq_scratch_alloc(lc * sizeof(VALUE));
    memcpy(lhs_local, le.items, lc * sizeof(VALUE));
    c->pool_top = outer_top;

    /* Collect union of all var_ids across alternatives so unbound
     * vars referenced by `body` resolve to null. */
    uint32_t all_vars[64]; size_t all_cnt = 0;
    for (size_t i = 0; i < pa->cnt; i++) {
        struct nuq_pat *p = nuq_pat_get(pa->pids[i]);
        pat_collect_vars(p, all_vars, &all_cnt, sizeof(all_vars)/sizeof(all_vars[0]));
    }

    VALUE saved_input = c->input;
    for (uint32_t i = 0; i < lc; i++) {
        VALUE v = lhs_local[i];
        size_t v_top = c->var_top;
        for (size_t j = 0; j < all_cnt; j++) nuq_var_push(c, all_vars[j], NUQ_NULL);
        bool bound = false;
        for (size_t j = 0; j < pa->cnt; j++) {
            struct nuq_pat *p = nuq_pat_get(pa->pids[j]);
            if (pat_fits(p, v)) {
                pat_bind_inner(c, p, v);
                bound = true;
                break;
            }
        }
        if (!bound) {
            /* No alternative matched — jq raises an error.  We mimic. */
            char d[80], msg[160];
            nuq_value_descr(v, d, sizeof(d));
            int w = snprintf(msg, sizeof(msg), "%s could not be matched against any alternative", d);
            c->error = nuq_make_string(msg, (size_t)w);
            nuq_var_pop(c, v_top);
            c->input = saved_input;
            return EMIT_EMPTY;
        }
        c->input = v;
        EMIT be = EVAL(c, body);
        if (c->error != NUQ_NULL) {
            nuq_var_pop(c, v_top); c->input = saved_input;
            return EMIT_EMPTY;
        }
        (void)be;     /* body pushes onto pool — slice below captures it */
        nuq_var_pop(c, v_top);
    }
    c->input = saved_input;
    return nuq_emit_slice(c, outer_top);
}


static const char **fmt_tab = NULL;
static size_t fmt_tab_len = 0, fmt_tab_capa = 0;

uint32_t
nuq_fmt_intern(const char *name)
{
    for (size_t i = 0; i < fmt_tab_len; i++) {
        if (strcmp(fmt_tab[i], name) == 0) return (uint32_t)i;
    }
    if (fmt_tab_len == fmt_tab_capa) {
        fmt_tab_capa = fmt_tab_capa ? fmt_tab_capa * 2 : 8;
        fmt_tab = (const char **)GC_realloc(fmt_tab, fmt_tab_capa * sizeof(*fmt_tab));
    }
    char *dup = (char *)GC_malloc_atomic(strlen(name) + 1);
    strcpy(dup, name);
    fmt_tab[fmt_tab_len] = dup;
    return (uint32_t)fmt_tab_len++;
}

const char *nuq_fmt_lookup(uint32_t id) { return fmt_tab[id]; }

/* --- CLI-supplied bindings (--arg / --argjson / --slurpfile / --rawfile) -- */

struct nuq_user_arg { uint32_t name_id; VALUE value; };
static struct nuq_user_arg *user_args = NULL;
static size_t user_args_len = 0, user_args_capa = 0;

static char *
slurp_file(const char *path, size_t *len_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = (char *)GC_malloc_atomic(cap);
    for (;;) {
        if (len + 4096 > cap) { cap *= 2; buf = (char *)GC_realloc(buf, cap); }
        size_t n = fread(buf + len, 1, cap - len, fp);
        if (n == 0) break;
        len += n;
    }
    fclose(fp);
    *len_out = len;
    return buf;
}

static void
user_args_push(uint32_t name_id, VALUE v)
{
    if (user_args_len == user_args_capa) {
        user_args_capa = user_args_capa ? user_args_capa * 2 : 8;
        user_args = (struct nuq_user_arg *)GC_realloc(
            user_args, user_args_capa * sizeof(*user_args));
    }
    user_args[user_args_len].name_id = name_id;
    user_args[user_args_len].value = v;
    user_args_len++;
}

/* Bind a pre-built VALUE to $name — used by `import "X" as $var;` to
 * inject the parsed JSON content of a data module. */
void
nuq_user_arg_add_value(const char *name, VALUE v)
{
    user_args_push(nuq_intern(name), v);
}

void
nuq_user_arg_add(const char *name, const char *value, bool json)
{
    VALUE v;
    if (json) {
        const char *endp;
        char *err = NULL;
        v = nuq_json_parse(value, strlen(value), &endp, &err);
        if (err) {
            fprintf(stderr, "nuq: --argjson %s: parse error: %s\n", name, err);
            exit(2);
        }
    } else {
        v = nuq_make_string(value, strlen(value));
    }
    user_args_push(nuq_intern(name), v);
}

bool
nuq_user_arg_add_file(const char *name, const char *path, bool raw)
{
    size_t L;
    char *buf = slurp_file(path, &L);
    if (!buf) {
        fprintf(stderr, "nuq: cannot open %s\n", path);
        return false;
    }
    VALUE v;
    if (raw) {
        v = nuq_make_string(buf, L);
    } else {
        /* --slurpfile: parse all JSON values, wrap into a single array. */
        VALUE arr = nuq_make_array(0);
        const char *p = buf, *end = buf + L;
        while (p < end) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
            if (p >= end) break;
            const char *np;
            char *err = NULL;
            VALUE jv = nuq_json_parse(p, end - p, &np, &err);
            if (err) {
                fprintf(stderr, "nuq: --slurpfile %s: parse error: %s\n", path, err);
                return false;
            }
            nuq_array_push(arr, jv);
            p = np;
        }
        v = arr;
    }
    user_args_push(nuq_intern(name), v);
    return true;
}

void
nuq_user_args_bind(CTX *c)
{
    /* `--arg foo bar` exposes both `$foo` (jq style) and `$ENV.foo`...
     * We only do `$foo` for now (no $ENV table). */
    for (size_t i = 0; i < user_args_len; i++) {
        nuq_var_push(c, user_args[i].name_id, user_args[i].value);
    }
}

/* --- input source (used by main loop + `input` / `inputs` builtins) ------
 *
 * Two modes:
 *
 *  - Cursor / lazy parse (default streaming): callers set a raw text
 *    buffer with `nuq_input_set_text`; each `nuq_input_pull` parses
 *    the next JSON value at the cursor.  Used by the per-value default
 *    loop — values land in the per-run arena (caller flips
 *    `nuq_alloc_perm = false`) and get freed by `nuq_arena_reset`.
 *
 *  - Queue / pre-parsed (used for `-n + inputs` aggregation, and any
 *    other case where a snapshot of every input value must survive
 *    multiple arena GC cycles): callers populate a permanent VALUE[]
 *    via `nuq_input_queue_push` and `nuq_input_pull` returns from it.
 *
 * Pull prefers the queue if non-empty; otherwise falls back to the
 * cursor.  Both `input` / `inputs` builtins go through `nuq_input_pull`
 * regardless, so the choice of source is transparent to the filter. */
static const char *input_text     = NULL;
static const char *input_text_end = NULL;
static const char *input_text_pos = NULL;

static VALUE *input_queue     = NULL;
static size_t input_queue_pos = 0;
static size_t input_queue_len = 0;
static size_t input_queue_capa = 0;

void
nuq_input_set_text(const char *src, size_t len)
{
    input_text = input_text_pos = src;
    input_text_end = src + (src ? len : 0);
    /* NB: doesn't touch the queue.  The two sources are independent —
     * pull prefers the queue when non-empty.  Callers wanting a fresh
     * start should call `nuq_input_reset` instead. */
}

void
nuq_input_reset(void)
{
    input_text = input_text_pos = input_text_end = NULL;
    input_queue_pos = 0;
    input_queue_len = 0;
}

void
nuq_input_queue_push(VALUE v)
{
    if (input_queue_len == input_queue_capa) {
        input_queue_capa = input_queue_capa ? input_queue_capa * 2 : 64;
        input_queue = (VALUE *)realloc(input_queue,
                                       input_queue_capa * sizeof(VALUE));
        if (!input_queue) abort();
    }
    input_queue[input_queue_len++] = v;
}

static void
input_skip_ws(void)
{
    while (input_text_pos < input_text_end &&
           (*input_text_pos == ' '  || *input_text_pos == '\t' ||
            *input_text_pos == '\n' || *input_text_pos == '\r'))
        input_text_pos++;
}

bool
nuq_input_pull(VALUE *out)
{
    if (input_queue_pos < input_queue_len) {
        *out = input_queue[input_queue_pos++];
        return true;
    }
    input_skip_ws();
    if (input_text_pos >= input_text_end) return false;
    const char *np;
    char *err = NULL;
    VALUE v = nuq_json_parse(input_text_pos,
                             input_text_end - input_text_pos, &np, &err);
    if (err) {
        fprintf(stderr, "nuq: parse error: %s\n", err);
        return false;
    }
    input_text_pos = np;
    *out = v;
    return true;
}

/* --- helpers ------------------------------------------------------------- */

static EMIT
err_emit(CTX *c, const char *msg)
{
    c->error = nuq_make_string(msg, strlen(msg));
    return EMIT_EMPTY;
}

bool
nuq_field_lookup(VALUE in, const char *name, bool optional, VALUE *out)
{
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_OBJECT) {
        *out = nuq_object_get_cstr(in, name);
        return true;
    }
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_NULL) {
        *out = NUQ_NULL;
        return true;
    }
    if (optional) return false;
    {
        VALUE k = nuq_make_string(name, strlen(name));
        char d[80];
        nuq_value_descr(k, d, sizeof(d));
        nuq_helper_error("Cannot index %s with %s", nuq_type_name(in), d);
    }
    return false;
}

bool
nuq_to_number(VALUE v, VALUE *out)
{
    if (NUQ_IS_FIX(v)) { *out = v; return true; }
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_DOUBLE) { *out = v; return true; }
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING) {
        struct nuq_obj *o = NUQ_PTR(v);
        /* jq's tonumber is strict: rejects leading/trailing whitespace
         * and trailing garbage.  Leading `+` and `-` are accepted. */
        if (o->str.len == 0) return false;
        const char *p = o->str.bytes;
        size_t left = o->str.len;
        if (*p == '-' || *p == '+') { p++; left--; }
        if (left == 0 || !(isdigit((unsigned char)*p) || *p == '.')) return false;
        char *e; double d = strtod(o->str.bytes, &e);
        if ((size_t)(e - o->str.bytes) != o->str.len) return false;
        *out = nuq_make_double(d);
        return true;
    }
    return false;
}

static int64_t
to_int64(VALUE v)
{
    if (NUQ_IS_FIX(v)) return NUQ_FIX_VAL(v);
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_DOUBLE) return (int64_t)NUQ_PTR(v)->dbl;
    return 0;
}

/* --- slice ------------------------------------------------------------- */

EMIT
nuq_slice_eval(CTX *c, struct Node *startn, struct Node *stopn, uint32_t flags, bool optional)
{
    VALUE in = c->input;
    int64_t length;
    bool is_str = false;
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_NULL)
        return nuq_emit_one(c, NUQ_NULL);
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_ARRAY) length = (int64_t)NUQ_PTR(in)->arr.len;
    else if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_STRING) {
        /* String slice indices are codepoint offsets, not byte offsets. */
        struct nuq_obj *o = NUQ_PTR(in);
        int64_t cp = 0;
        for (size_t i = 0; i < o->str.len; ) {
            unsigned char x = (unsigned char)o->str.bytes[i];
            if (x < 0x80) i += 1;
            else if ((x & 0xE0) == 0xC0) i += 2;
            else if ((x & 0xF0) == 0xE0) i += 3;
            else if ((x & 0xF8) == 0xF0) i += 4;
            else i += 1;
            cp++;
        }
        length = cp;
        is_str = true;
    }
    else {
        if (optional) return EMIT_EMPTY;
        return err_emit(c, "type error: cannot slice");
    }
    int64_t start = 0, stop = length;
    if (flags & SLICE_HAS_START) {
        size_t top0 = c->pool_top;
        EMIT buf = EVAL(c, startn);
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
        if (buf.count == 0) { c->pool_top = top0; return EMIT_EMPTY; }
        VALUE v = buf.items[0];
        c->pool_top = top0;
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL) start = 0;
        else if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_DOUBLE) {
            double d = NUQ_PTR(v)->dbl;
            if (isnan(d)) start = 0;
            else start = (int64_t)floor(d);
        } else start = to_int64(v);
    }
    if (flags & SLICE_HAS_STOP) {
        size_t top0 = c->pool_top;
        EMIT buf = EVAL(c, stopn);
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
        if (buf.count == 0) { c->pool_top = top0; return EMIT_EMPTY; }
        VALUE v = buf.items[0];
        c->pool_top = top0;
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL) stop = length;
        else if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_DOUBLE) {
            double d = NUQ_PTR(v)->dbl;
            /* nan stop → use length (jq behavior). */
            if (isnan(d)) stop = length;
            else stop = (int64_t)ceil(d);
        } else stop = to_int64(v);
    }
    if (start < 0) start += length;
    if (stop  < 0) stop  += length;
    if (start < 0) start = 0;
    if (stop  > length) stop = length;
    if (stop  < start) stop = start;

    if (is_str) {
        /* Convert codepoint indices to byte offsets in a single pass. */
        struct nuq_obj *o = NUQ_PTR(in);
        size_t bs = 0, be = o->str.len;
        bool bs_set = false, be_set = false;
        int64_t cp = 0;
        size_t i = 0;
        while (i <= o->str.len) {
            if (!bs_set && cp == start) { bs = i; bs_set = true; }
            if (!be_set && cp == stop)  { be = i; be_set = true; break; }
            if (i == o->str.len) break;
            unsigned char x = (unsigned char)o->str.bytes[i];
            if (x < 0x80) i += 1;
            else if ((x & 0xE0) == 0xC0) i += 2;
            else if ((x & 0xF0) == 0xE0) i += 3;
            else if ((x & 0xF8) == 0xF0) i += 4;
            else i += 1;
            cp++;
        }
        if (!bs_set) bs = o->str.len;
        if (!be_set) be = o->str.len;
        return nuq_emit_one(c, nuq_make_string(o->str.bytes + bs, be - bs));
    }
    struct nuq_obj *o = NUQ_PTR(in);
    VALUE arr = nuq_make_array((size_t)(stop - start));
    for (int64_t i = start; i < stop; i++) nuq_array_push(arr, o->arr.items[i]);
    return nuq_emit_one(c, arr);
}

/* --- object constructor ------------------------------------------------ */

EMIT
nuq_object_eval(CTX *c, uint32_t entries_id)
{
    const struct obj_ctor *const e = &obj_tab[entries_id];
    if (e->cnt > 32) return err_emit(c, "object literal too large");
    const size_t outer_top = c->pool_top;

    /* Fast path — emit exactly one object.  Build it directly into
     * the pool and bail to the cartesian path the first time we hit
     * a multi-emit (or zero-emit) entry.  This avoids all per-entry
     * GC_malloc(sizeof(VALUE)) buffers in the common case. */
    VALUE k_fast[32], v_fast[32];
    size_t fast_done = 0;
    for (size_t i = 0; i < e->cnt; i++) {
        const struct nuq_obj_entry *const ie = &e->items[i];
        VALUE k, v;
        if (ie->kkind == 2 && ie->vexpr != NULL) {
            /* `{$x: V}` — key is the variable's VALUE (jq semantics). */
            k = nuq_var_get(c, ie->var_id);
        } else if (ie->kkind == 0 || ie->kkind == 2) {
            k = ie->kname_value;
        } else {
            const size_t t0 = c->pool_top;
            const EMIT ks = EVAL(c, ie->kexpr);
            if (c->error != NUQ_NULL) return EMIT_EMPTY;
            if (ks.count != 1) goto cartesian;
            k = ks.items[0];
            c->pool_top = t0;
        }
        if (ie->vexpr == NULL) {
            if (ie->kkind == 2) v = nuq_var_get(c, ie->var_id);
            else if (ie->kkind == 1) {
                /* Computed key with shorthand value — look up `k`
                 * (already evaluated) in the input object. */
                v = (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_OBJECT)
                    ? nuq_object_get(c->input, k) : NUQ_NULL;
            }
            else                v = nuq_object_get_cstr(c->input, ie->kname);
        } else {
            const size_t t0 = c->pool_top;
            const EMIT vs = EVAL(c, ie->vexpr);
            if (c->error != NUQ_NULL) return EMIT_EMPTY;
            if (vs.count != 1) goto cartesian;
            v = vs.items[0];
            c->pool_top = t0;
        }
        if (UNLIKELY(!(NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_STRING)))
            return err_emit(c, "object key must be string");
        k_fast[i] = k;
        v_fast[i] = v;
        fast_done++;
    }
    {
        VALUE obj = nuq_make_object(e->cnt);
        for (size_t i = 0; i < e->cnt; i++)
            nuq_object_set(obj, k_fast[i], v_fast[i]);
        nuq_pool_push(c, obj);
        return nuq_emit_slice(c, outer_top);
    }

cartesian: {
    /* Multi-emit (or zero-emit) somewhere — fall back to the
     * cartesian build.  Restore pool_top so the partial fast-path
     * EVAL slots become free again. */
    c->pool_top = outer_top;
    struct stream { VALUE *items; uint32_t count; };
    struct stream kss[32], vss[32];
    /* Re-evaluate from scratch.  This is the rare path. */
    for (size_t i = 0; i < e->cnt; i++) {
        const struct nuq_obj_entry *const ie = &e->items[i];
        if (ie->kkind == 2 && ie->vexpr != NULL) {
            VALUE *const b = (VALUE *)GC_malloc(sizeof(VALUE));
            b[0] = nuq_var_get(c, ie->var_id);
            kss[i].items = b; kss[i].count = 1;
        } else if (ie->kkind == 0 || ie->kkind == 2) {
            VALUE *const b = (VALUE *)GC_malloc(sizeof(VALUE));
            b[0] = ie->kname_value;
            kss[i].items = b; kss[i].count = 1;
        } else {
            const size_t t0 = c->pool_top;
            const EMIT ks = EVAL(c, ie->kexpr);
            if (c->error != NUQ_NULL) return EMIT_EMPTY;
            VALUE *const b = (VALUE *)GC_malloc(ks.count * sizeof(VALUE) + 1);
            memcpy(b, ks.items, ks.count * sizeof(VALUE));
            kss[i].items = b; kss[i].count = ks.count;
            c->pool_top = t0;
        }
        if (ie->vexpr == NULL) {
            VALUE *const b = (VALUE *)GC_malloc(sizeof(VALUE));
            b[0] = ie->kkind == 2 ? nuq_var_get(c, ie->var_id)
                                  : nuq_object_get_cstr(c->input, ie->kname);
            vss[i].items = b; vss[i].count = 1;
        } else {
            const size_t t0 = c->pool_top;
            const EMIT vs = EVAL(c, ie->vexpr);
            if (c->error != NUQ_NULL) return EMIT_EMPTY;
            VALUE *const b = (VALUE *)GC_malloc(vs.count * sizeof(VALUE) + 1);
            memcpy(b, vs.items, vs.count * sizeof(VALUE));
            vss[i].items = b; vss[i].count = vs.count;
            c->pool_top = t0;
        }
    }
    for (size_t i = 0; i < e->cnt; i++)
        if (kss[i].count == 0 || vss[i].count == 0)
            return EMIT_EMPTY;

    size_t kidx[32] = {0}, vidx[32] = {0};
    for (;;) {
        VALUE obj = nuq_make_object(e->cnt);
        for (size_t i = 0; i < e->cnt; i++) {
            VALUE k = kss[i].items[kidx[i]];
            VALUE v = vss[i].items[vidx[i]];
            if (UNLIKELY(!(NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_STRING)))
                return err_emit(c, "object key must be string");
            nuq_object_set(obj, k, v);
        }
        nuq_pool_push(c, obj);

        ssize_t pos = (ssize_t)e->cnt - 1;
        for (; pos >= 0; pos--) {
            vidx[pos]++;
            if (vidx[pos] < vss[pos].count) break;
            vidx[pos] = 0;
            kidx[pos]++;
            if (kidx[pos] < kss[pos].count) break;
            kidx[pos] = 0;
        }
        if (pos < 0) break;
    }
    (void)fast_done;
    return nuq_emit_slice(c, outer_top);
}
}

/* --- error / user-call / def ---------------------------------------- */

EMIT
nuq_error_eval(CTX *c, struct Node *expr)
{
    size_t top0 = c->pool_top;
    EMIT v = EVAL(c, expr);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (v.count > 0) c->error = v.items[0];
    else c->error = NUQ_NULL;
    c->pool_top = top0;
    return EMIT_EMPTY;
}

EMIT
nuq_user_call(CTX *c, uint32_t name_id, uint32_t arity, uint32_t args_id)
{
    struct Node **args = NULL;
    if (arity > 0) args = args_tab[args_id].args;

    struct nuq_func_def *fd = nuq_func_lookup(c, name_id, (int)arity);
    if (fd) {
        if (arity > 16) return err_emit(c, "too many args");
        /* Pre-evaluate every value-arg into a list (jq semantics:
         * `f($a; $b)` runs `arg_a as $a | arg_b as $b | body`, which
         * cartesian-expands multi-emit args).  Expr-args are lazy
         * (closures), evaluated each time they are referenced. */
        VALUE *val_lists[16] = {0};
        uint32_t val_counts[16] = {0};
        size_t outer_top = c->pool_top;
        for (uint32_t i = 0; i < arity; i++) {
            if (!fd->param_is_value[i]) continue;
            size_t t0 = c->pool_top;
            EMIT buf = EVAL(c, args[i]);
            if (c->error != NUQ_NULL) { c->pool_top = outer_top; return EMIT_EMPTY; }
            uint32_t cnt = buf.count;
            VALUE *list = (VALUE *)GC_malloc((cnt > 0 ? cnt : 1) * sizeof(VALUE));
            if (cnt == 0) {
                /* Empty args make the whole call empty. */
                c->pool_top = outer_top;
                return EMIT_EMPTY;
            }
            memcpy(list, buf.items, cnt * sizeof(VALUE));
            val_lists[i] = list;
            val_counts[i] = cnt;
            c->pool_top = t0;
        }
        c->pool_top = outer_top;

        /* Cartesian iteration over value-arg combinations. */
        uint32_t idx[16] = {0};
        for (;;) {
            size_t var_top = c->var_top;
            size_t func_top = c->func_cnt;
            for (uint32_t i = 0; i < arity; i++) {
                if (fd->param_is_value[i]) {
                    nuq_var_push(c, fd->param_ids[i], val_lists[i][idx[i]]);
                } else {
                    struct nuq_func_def *pfd = (struct nuq_func_def *)GC_malloc(sizeof(*pfd));
                    pfd->name_id = fd->param_ids[i];
                    pfd->arity = 0;
                    pfd->param_ids = NULL;
                    pfd->param_is_value = NULL;
                    pfd->body = args[i];
                    pfd->scope_top = c->func_cnt > 0 ? c->func_cnt - 1 : 0;
                    if (pfd->scope_top == 0) pfd->scope_top = (size_t)-1;
                    /* Snapshot the caller's var stack so each
                     * reference to the param re-evaluates `body` in
                     * caller scope (jq's call-by-name).  Clone is
                     * fine — the snap is read-only on subsequent
                     * invocations (we make a working copy each call). */
                    if (c->var_top > 0) {
                        pfd->var_snap = (struct nuq_var_slot *)GC_malloc(
                            c->var_top * sizeof(struct nuq_var_slot));
                        memcpy(pfd->var_snap, c->var_stack,
                               c->var_top * sizeof(struct nuq_var_slot));
                        pfd->var_snap_cnt = c->var_top;
                    } else {
                        pfd->var_snap = NULL;
                        pfd->var_snap_cnt = 0;
                    }
                    nuq_func_define(c, pfd);
                    if (pfd->scope_top == (size_t)-1) pfd->scope_top = 0;
                }
            }
            size_t saved_skip_s = c->func_skip_start;
            size_t saved_skip_e = c->func_skip_end;
            c->func_skip_start = fd->scope_top + 1;
            c->func_skip_end   = func_top;
            /* Call-by-name closure: temporarily swap the live var
             * stack for a clone of the captured snapshot so
             * `$name` references inside the thunk's body resolve
             * to the bindings active at definition time. */
            struct nuq_var_slot *cb_saved_stack = c->var_stack;
            size_t cb_saved_top  = c->var_top;
            size_t cb_saved_capa = c->var_capa;
            if (fd->var_snap) {
                size_t need = fd->var_snap_cnt + 16;
                struct nuq_var_slot *temp = (struct nuq_var_slot *)GC_malloc(
                    need * sizeof(*temp));
                memcpy(temp, fd->var_snap,
                       fd->var_snap_cnt * sizeof(*temp));
                c->var_stack = temp;
                c->var_top = fd->var_snap_cnt;
                c->var_capa = need;
            }
            (void)EVAL(c, fd->body);
            if (fd->var_snap) {
                c->var_stack = cb_saved_stack;
                c->var_top = cb_saved_top;
                c->var_capa = cb_saved_capa;
            }
            c->func_skip_start = saved_skip_s;
            c->func_skip_end   = saved_skip_e;
            nuq_var_pop(c, var_top);
            c->func_cnt = func_top;
            if (c->error != NUQ_NULL) return EMIT_EMPTY;
            /* Advance cartesian indices. */
            ssize_t pos = (ssize_t)arity - 1;
            while (pos >= 0) {
                if (!fd->param_is_value[pos]) { pos--; continue; }
                idx[pos]++;
                if (idx[pos] < val_counts[pos]) break;
                idx[pos] = 0;
                pos--;
            }
            if (pos < 0) break;
        }
        return nuq_emit_slice(c, outer_top);
    }
    fprintf(stderr, "nuq error: %s/%u is not defined\n", nuq_intern_lookup(name_id), arity);
    return err_emit(c, "undefined");
}

EMIT
nuq_defs_eval(CTX *c, uint32_t defs_id, struct Node *body)
{
    struct def_block *db = &def_tab[defs_id];
    size_t saved = c->func_cnt;
    /* Materialise / reuse the per-block fd cache.  Without the cache
     * we'd allocate `db->cnt` fresh nuq_func_defs on every nuq_run,
     * leaking them per input.  Cache key is the call-site
     * `c->func_cnt` (sets `scope_top` for the lexical-resolution
     * cutoff) — when the same defs_id is invoked from the same scope
     * across runs, fds are reusable verbatim. */
    if (db->fds_cache == NULL || db->fds_cache_scope_top != saved) {
        if (db->fds_cache == NULL) {
            db->fds_cache = (struct nuq_func_def **)
                            calloc(db->cnt, sizeof(struct nuq_func_def *));
        }
        db->fds_cache_scope_top = saved;
        for (size_t i = 0; i < db->cnt; i++) {
            struct nuq_func_def *fd = db->fds_cache[i];
            if (!fd) {
                fd = (struct nuq_func_def *)calloc(1, sizeof(*fd));
                db->fds_cache[i] = fd;
            }
            struct nuq_def_entry *de = &db->items[i];
            fd->name_id = de->name_id;
            fd->arity = de->arity;
            fd->param_ids = de->param_ids;
            fd->param_is_value = de->param_is_value;
            fd->body = de->body;
            fd->scope_top = 0;     /* nuq_func_define resets this */
        }
    }
    for (size_t i = 0; i < db->cnt; i++) {
        nuq_func_define(c, db->fds_cache[i]);
    }
    EMIT r = EVAL(c, body);
    c->func_cnt = saved;
    return r;
}

/* --- reduce / foreach -------------------------------------------------- */

EMIT
nuq_reduce_eval(CTX *c, struct Node *src, uint32_t var_id, struct Node *init, struct Node *update)
{
    size_t outer_top = c->pool_top;

    size_t t0 = c->pool_top;
    EMIT init_e = EVAL(c, init);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t ic = init_e.count;
    if (ic == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE inits_small[16];
    VALUE *inits = (ic <= 16) ? inits_small : (VALUE *)nuq_scratch_alloc(ic * sizeof(VALUE));
    memcpy(inits, init_e.items, ic * sizeof(VALUE));
    NUQ_GC_PIN_ARR(inits, ic);
    c->pool_top = t0;

    EMIT src_e = EVAL(c, src);
    if (c->error != NUQ_NULL) { NUQ_GC_UNPIN_ARR(); return EMIT_EMPTY; }
    uint32_t sc = src_e.count;
    VALUE small[16];
    VALUE *src_local = (sc <= 16) ? small : (VALUE *)nuq_scratch_alloc(sc * sizeof(VALUE));
    memcpy(src_local, src_e.items, sc * sizeof(VALUE));
    NUQ_GC_PIN_ARR(src_local, sc);
    c->pool_top = outer_top;

    VALUE saved_input = c->input;
    for (uint32_t k = 0; k < ic; k++) {
        VALUE acc = inits[k];
        NUQ_GC_PIN1(acc);          /* survives across many update evals */
        for (uint32_t i = 0; i < sc; i++) {
            size_t t1 = c->pool_top;
            size_t v_top = c->var_top;
            nuq_var_push(c, var_id, src_local[i]);
            c->input = acc;
            EMIT up = EVAL(c, update);
            nuq_var_pop(c, v_top);
            if (c->error != NUQ_NULL) {
                c->input = saved_input;
                c->pool_top = outer_top;
                NUQ_GC_UNPIN(1);
                NUQ_GC_UNPIN_ARR(); NUQ_GC_UNPIN_ARR();
                return EMIT_EMPTY;
            }
            if (up.count > 0) acc = up.items[up.count - 1];
            c->pool_top = t1;
        }
        nuq_pool_push(c, acc);
        NUQ_GC_UNPIN(1);
    }
    c->input = saved_input;
    NUQ_GC_UNPIN_ARR(); NUQ_GC_UNPIN_ARR();
    return nuq_emit_slice(c, outer_top);
}

EMIT
nuq_foreach_eval(CTX *c, struct Node *src, uint32_t var_id, struct Node *init, struct Node *update, struct Node *extract)
{
    size_t outer_top = c->pool_top;

    /* `init` may emit multiple values (jq runs the foreach once per
     * init).  Snapshot the inits and the source array up front so the
     * pool is free for body evaluation.  All three buffers (inits,
     * src_local, up_local) plus the live `acc` outlive multiple
     * arena allocator calls (update / extract eval) — pin everything. */
    size_t t0 = c->pool_top;
    EMIT init_e = EVAL(c, init);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t ic = init_e.count;
    if (ic == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE inits_small[16];
    VALUE *inits = (ic <= 16) ? inits_small : (VALUE *)nuq_scratch_alloc(ic * sizeof(VALUE));
    memcpy(inits, init_e.items, ic * sizeof(VALUE));
    NUQ_GC_PIN_ARR(inits, ic);
    c->pool_top = t0;

    EMIT src_e = EVAL(c, src);
    if (c->error != NUQ_NULL) { NUQ_GC_UNPIN_ARR(); return EMIT_EMPTY; }
    uint32_t sc = src_e.count;
    VALUE small[16];
    VALUE *src_local = (sc <= 16) ? small : (VALUE *)nuq_scratch_alloc(sc * sizeof(VALUE));
    memcpy(src_local, src_e.items, sc * sizeof(VALUE));
    NUQ_GC_PIN_ARR(src_local, sc);
    c->pool_top = outer_top;

    VALUE saved_input = c->input;
    for (uint32_t k = 0; k < ic; k++) {
        VALUE acc = inits[k];
        NUQ_GC_PIN1(acc);
        for (uint32_t i = 0; i < sc; i++) {
            size_t v_top = c->var_top;
            nuq_var_push(c, var_id, src_local[i]);
            c->input = acc;
            size_t t1 = c->pool_top;
            EMIT up = EVAL(c, update);
            if (c->error != NUQ_NULL) {
                nuq_var_pop(c, v_top); c->input = saved_input; c->pool_top = outer_top;
                NUQ_GC_UNPIN(1); NUQ_GC_UNPIN_ARR(); NUQ_GC_UNPIN_ARR();
                return EMIT_EMPTY;
            }
            if (c->break_label != 0) {
                nuq_var_pop(c, v_top); c->input = saved_input;
                NUQ_GC_UNPIN(1); NUQ_GC_UNPIN_ARR(); NUQ_GC_UNPIN_ARR();
                return nuq_emit_slice(c, outer_top);
            }
            uint32_t uc = up.count;
            VALUE usmall[16];
            VALUE *up_local = (uc <= 16) ? usmall : (VALUE *)nuq_scratch_alloc(uc * sizeof(VALUE));
            memcpy(up_local, up.items, uc * sizeof(VALUE));
            NUQ_GC_PIN_ARR(up_local, uc);
            c->pool_top = t1;
            for (uint32_t j = 0; j < uc; j++) {
                acc = up_local[j];
                c->input = acc;
                (void)EVAL(c, extract);
                if (c->error != NUQ_NULL) {
                    nuq_var_pop(c, v_top); c->input = saved_input; c->pool_top = outer_top;
                    NUQ_GC_UNPIN_ARR();   /* up_local */
                    NUQ_GC_UNPIN(1); NUQ_GC_UNPIN_ARR(); NUQ_GC_UNPIN_ARR();
                    return EMIT_EMPTY;
                }
                if (c->break_label != 0) {
                    nuq_var_pop(c, v_top); c->input = saved_input;
                    NUQ_GC_UNPIN_ARR();
                    NUQ_GC_UNPIN(1); NUQ_GC_UNPIN_ARR(); NUQ_GC_UNPIN_ARR();
                    return nuq_emit_slice(c, outer_top);
                }
            }
            NUQ_GC_UNPIN_ARR();   /* up_local */
            nuq_var_pop(c, v_top);
        }
        NUQ_GC_UNPIN(1);   /* acc */
    }
    c->input = saved_input;
    NUQ_GC_UNPIN_ARR(); NUQ_GC_UNPIN_ARR();
    return nuq_emit_slice(c, outer_top);
}

EMIT
nuq_reduce_pat_eval(CTX *c, struct Node *src, uint32_t pat_id,
                    struct Node *init, struct Node *update)
{
    size_t outer_top = c->pool_top;
    size_t t0 = c->pool_top;
    EMIT init_e = EVAL(c, init);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    VALUE acc = init_e.count > 0 ? init_e.items[0] : NUQ_NULL;
    c->pool_top = t0;

    EMIT src_e = EVAL(c, src);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t sc = src_e.count;
    VALUE small[16];
    VALUE *src_local = (sc <= 16) ? small : (VALUE *)nuq_scratch_alloc(sc * sizeof(VALUE));
    memcpy(src_local, src_e.items, sc * sizeof(VALUE));
    c->pool_top = outer_top;

    struct nuq_pat *pat = nuq_pat_get(pat_id);
    VALUE saved_input = c->input;
    for (uint32_t i = 0; i < sc; i++) {
        size_t t1 = c->pool_top;
        size_t v_top = nuq_pat_bind(c, pat, src_local[i]);
        c->input = acc;
        EMIT up = EVAL(c, update);
        nuq_var_pop(c, v_top);
        if (c->error != NUQ_NULL) { c->input = saved_input; return EMIT_EMPTY; }
        if (up.count > 0) acc = up.items[up.count - 1];
        c->pool_top = t1;
    }
    c->input = saved_input;
    return nuq_emit_one(c, acc);
}

EMIT
nuq_foreach_pat_eval(CTX *c, struct Node *src, uint32_t pat_id,
                     struct Node *init, struct Node *update, struct Node *extract)
{
    size_t outer_top = c->pool_top;
    size_t t0 = c->pool_top;
    EMIT init_e = EVAL(c, init);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    VALUE acc = init_e.count > 0 ? init_e.items[0] : NUQ_NULL;
    c->pool_top = t0;

    EMIT src_e = EVAL(c, src);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t sc = src_e.count;
    VALUE small[16];
    VALUE *src_local = (sc <= 16) ? small : (VALUE *)nuq_scratch_alloc(sc * sizeof(VALUE));
    memcpy(src_local, src_e.items, sc * sizeof(VALUE));
    c->pool_top = outer_top;

    struct nuq_pat *pat = nuq_pat_get(pat_id);
    VALUE saved_input = c->input;
    for (uint32_t i = 0; i < sc; i++) {
        size_t v_top = nuq_pat_bind(c, pat, src_local[i]);
        c->input = acc;
        size_t t1 = c->pool_top;
        EMIT up = EVAL(c, update);
        if (c->error != NUQ_NULL) { nuq_var_pop(c, v_top); c->input = saved_input; return EMIT_EMPTY; }
        uint32_t uc = up.count;
        VALUE usmall[16];
        VALUE *up_local = (uc <= 16) ? usmall : (VALUE *)nuq_scratch_alloc(uc * sizeof(VALUE));
        memcpy(up_local, up.items, uc * sizeof(VALUE));
        c->pool_top = t1;
        for (uint32_t j = 0; j < uc; j++) {
            acc = up_local[j];
            c->input = acc;
            (void)EVAL(c, extract);
            if (c->error != NUQ_NULL) {
                nuq_var_pop(c, v_top); c->input = saved_input; return EMIT_EMPTY;
            }
        }
        nuq_var_pop(c, v_top);
    }
    c->input = saved_input;
    return nuq_emit_slice(c, outer_top);
}

/* --- string interp ----------------------------------------------------- */

EMIT
nuq_interp_eval(CTX *c, uint32_t parts_id)
{
    struct interp_entry *e = &interp_tab[parts_id];
    if (e->cnt > 16) return err_emit(c, "interp too long");

    struct stream { VALUE *items; uint32_t count; };
    struct stream parts[16];

    for (size_t i = 0; i < e->cnt; i++) {
        size_t t0 = c->pool_top;
        EMIT pe = EVAL(c, e->parts[i]);
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
        VALUE *b = (VALUE *)GC_malloc(pe.count * sizeof(VALUE) + 1);
        for (uint32_t j = 0; j < pe.count; j++)
            b[j] = nuq_to_json_string(pe.items[j]);
        parts[i].items = b;
        parts[i].count = pe.count;
        c->pool_top = t0;
        if (parts[i].count == 0) return EMIT_EMPTY;
    }

    size_t outer_top = c->pool_top;
    size_t idx[16] = {0};
    for (;;) {
        size_t total = 0;
        size_t lens[16];
        VALUE strs[16];
        for (size_t i = 0; i < e->cnt; i++) {
            VALUE s = parts[i].items[idx[i]];
            strs[i] = s;
            lens[i] = NUQ_PTR(s)->str.len;
            total += lens[i];
        }
        char *buf = (char *)GC_malloc_atomic(total + 1);
        char *p = buf;
        for (size_t i = 0; i < e->cnt; i++) {
            memcpy(p, NUQ_PTR(strs[i])->str.bytes, lens[i]);
            p += lens[i];
        }
        buf[total] = '\0';
        nuq_pool_push(c, nuq_make_string_take(buf, total));

        ssize_t pos = (ssize_t)e->cnt - 1;
        for (; pos >= 0; pos--) {
            idx[pos]++;
            if (idx[pos] < parts[pos].count) break;
            idx[pos] = 0;
        }
        if (pos < 0) break;
    }
    return nuq_emit_slice(c, outer_top);
}

/* --- format -------------------------------------------------------- */

static VALUE
fmt_apply(uint32_t fmt_id, VALUE v)
{
    const char *fmt = nuq_fmt_lookup(fmt_id);
    if (strcmp(fmt, "text") == 0) return nuq_to_json_string(v);
    if (strcmp(fmt, "json") == 0) {
        /* @json always produces a JSON encoding — strings get quoted,
         * unlike @text which leaves strings as-is. */
        char *buf = NULL; size_t bn = 0;
        FILE *fp = open_memstream(&buf, &bn);
        nuq_json_print(fp, v, 0);
        fclose(fp);
        VALUE r = nuq_make_string(buf, bn);
        free(buf);
        return r;
    }
    if (strcmp(fmt, "csv") == 0 || strcmp(fmt, "tsv") == 0) {
        bool tsv = (fmt[0] == 't');
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY))
            return nuq_make_string("", 0);
        struct nuq_obj *ao = NUQ_PTR(v);
        char *buf = NULL; size_t bn = 0;
        FILE *fp = open_memstream(&buf, &bn);
        for (size_t i = 0; i < ao->arr.len; i++) {
            if (i) fputc(tsv ? '\t' : ',', fp);
            VALUE x = ao->arr.items[i];
            if (NUQ_IS_PTR(x) && NUQ_PTR(x)->type == NUQ_T_STRING) {
                struct nuq_obj *so = NUQ_PTR(x);
                if (tsv) {
                    for (size_t k = 0; k < so->str.len; k++) {
                        char ch = so->str.bytes[k];
                        if (ch == '\t') fputs("\\t", fp);
                        else if (ch == '\r') fputs("\\r", fp);
                        else if (ch == '\n') fputs("\\n", fp);
                        else if (ch == '\\') fputs("\\\\", fp);
                        else fputc(ch, fp);
                    }
                } else {
                    fputc('"', fp);
                    for (size_t k = 0; k < so->str.len; k++) {
                        if (so->str.bytes[k] == '"') fputc('"', fp);
                        fputc(so->str.bytes[k], fp);
                    }
                    fputc('"', fp);
                }
            } else if (NUQ_IS_PTR(x) && NUQ_PTR(x)->type == NUQ_T_NULL) {
                /* empty */
            } else {
                nuq_json_print(fp, x, 0);
            }
        }
        fclose(fp);
        VALUE r = nuq_make_string(buf, bn);
        free(buf);
        return r;
    }
    if (strcmp(fmt, "uri") == 0) {
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING)) v = nuq_to_json_string(v);
        struct nuq_obj *so = NUQ_PTR(v);
        char *buf = (char *)GC_malloc_atomic(so->str.len * 3 + 1);
        size_t bl = 0;
        static const char hex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < so->str.len; i++) {
            unsigned char ch = (unsigned char)so->str.bytes[i];
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
                buf[bl++] = ch;
            } else {
                buf[bl++] = '%';
                buf[bl++] = hex[ch >> 4];
                buf[bl++] = hex[ch & 0xF];
            }
        }
        buf[bl] = '\0';
        return nuq_make_string_take(buf, bl);
    }
    if (strcmp(fmt, "urid") == 0) {
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING)) v = nuq_to_json_string(v);
        struct nuq_obj *so = NUQ_PTR(v);
        char *buf = (char *)GC_malloc_atomic(so->str.len + 1);
        size_t bl = 0;
        for (size_t i = 0; i < so->str.len; i++) {
            unsigned char ch = (unsigned char)so->str.bytes[i];
            if (ch == '%' && i + 2 < so->str.len) {
                int hi = -1, lo = -1;
                char c1 = so->str.bytes[i + 1], c2 = so->str.bytes[i + 2];
                if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
                else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
                else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
                if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
                else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
                else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
                if (hi >= 0 && lo >= 0) {
                    buf[bl++] = (char)((hi << 4) | lo);
                    i += 2;
                    continue;
                }
            }
            buf[bl++] = (char)ch;
        }
        buf[bl] = '\0';
        return nuq_make_string_take(buf, bl);
    }
    if (strcmp(fmt, "html") == 0) {
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING)) v = nuq_to_json_string(v);
        struct nuq_obj *so = NUQ_PTR(v);
        char *buf = NULL; size_t bn = 0;
        FILE *fp = open_memstream(&buf, &bn);
        for (size_t i = 0; i < so->str.len; i++) {
            char ch = so->str.bytes[i];
            switch (ch) {
              case '<': fputs("&lt;", fp); break;
              case '>': fputs("&gt;", fp); break;
              case '&': fputs("&amp;", fp); break;
              case '\'': fputs("&apos;", fp); break;
              case '"': fputs("&quot;", fp); break;
              default: fputc(ch, fp); break;
            }
        }
        fclose(fp);
        VALUE r = nuq_make_string(buf, bn);
        free(buf);
        return r;
    }
    if (strcmp(fmt, "sh") == 0) {
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING)) v = nuq_to_json_string(v);
        struct nuq_obj *so = NUQ_PTR(v);
        char *buf = NULL; size_t bn = 0;
        FILE *fp = open_memstream(&buf, &bn);
        fputc('\'', fp);
        for (size_t i = 0; i < so->str.len; i++) {
            if (so->str.bytes[i] == '\'') fputs("'\\''", fp);
            else fputc(so->str.bytes[i], fp);
        }
        fputc('\'', fp);
        fclose(fp);
        VALUE r = nuq_make_string(buf, bn);
        free(buf);
        return r;
    }
    if (strcmp(fmt, "base64") == 0) {
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING)) v = nuq_to_json_string(v);
        struct nuq_obj *so = NUQ_PTR(v);
        static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const unsigned char *src = (const unsigned char *)so->str.bytes;
        size_t sl = so->str.len;
        size_t ol = 4 * ((sl + 2) / 3);
        char *out = (char *)GC_malloc_atomic(ol + 1);
        size_t op = 0;
        for (size_t i = 0; i < sl; i += 3) {
            int b0 = src[i];
            int b1 = i+1 < sl ? src[i+1] : 0;
            int b2 = i+2 < sl ? src[i+2] : 0;
            out[op++] = tab[b0 >> 2];
            out[op++] = tab[((b0 & 3) << 4) | (b1 >> 4)];
            out[op++] = i+1 < sl ? tab[((b1 & 0xF) << 2) | (b2 >> 6)] : '=';
            out[op++] = i+2 < sl ? tab[b2 & 0x3F] : '=';
        }
        out[op] = '\0';
        return nuq_make_string_take(out, op);
    }
    if (strcmp(fmt, "base64d") == 0) {
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING)) v = nuq_to_json_string(v);
        struct nuq_obj *so = NUQ_PTR(v);
        const char *src = so->str.bytes;
        size_t sl = so->str.len;
        char *out = (char *)GC_malloc_atomic(sl + 1);
        size_t ol = 0;
        int buf = 0, bits = 0;
        for (size_t i = 0; i < sl; i++) {
            int x; char ch = src[i];
            if (ch >= 'A' && ch <= 'Z') x = ch - 'A';
            else if (ch >= 'a' && ch <= 'z') x = ch - 'a' + 26;
            else if (ch >= '0' && ch <= '9') x = ch - '0' + 52;
            else if (ch == '+') x = 62;
            else if (ch == '/') x = 63;
            else if (ch == '=') break;
            else continue;
            buf = (buf << 6) | x;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out[ol++] = (char)((buf >> bits) & 0xFF);
            }
        }
        out[ol] = '\0';
        return nuq_make_string_take(out, ol);
    }
    return v;
}

/* `@fmt str_with_interp` — only the interpolated parts are formatted;
 * the literal string parts of the template pass through unchanged.
 * jq semantics: `@html "<b>\(.)</b>"` keeps `<b>` and `</b>` raw and
 * @html-escapes only `\(.)`'s output. */
extern const struct NodeKind kind_node_interp;
extern const struct NodeKind kind_node_str;

EMIT
nuq_format_eval(CTX *c, uint32_t fmt_id, struct Node *body)
{
    if (body == NULL) {
        return nuq_emit_one(c, fmt_apply(fmt_id, c->input));
    }
    /* Per-part formatting when body is an interp node. */
    if (body->head.kind == &kind_node_interp) {
        uint32_t parts_id = body->u.node_interp.parts_id;
        struct interp_entry *e = &interp_tab[parts_id];
        if (e->cnt > 16) return err_emit(c, "interp too long");
        struct { VALUE *items; uint32_t count; } parts[16];
        for (size_t i = 0; i < e->cnt; i++) {
            size_t t0 = c->pool_top;
            EMIT pe = EVAL(c, e->parts[i]);
            if (c->error != NUQ_NULL) return EMIT_EMPTY;
            uint32_t pc = pe.count;
            VALUE *b = (VALUE *)GC_malloc(pc * sizeof(VALUE) + 1);
            bool is_literal = (e->parts[i]->head.kind == &kind_node_str);
            for (uint32_t j = 0; j < pc; j++) {
                /* Literal pieces of the template: emit verbatim string;
                 * everything else: feed through @fmt so we get the
                 * formatted-and-quoted representation, then if it's a
                 * string strip the surrounding "" so we can splice
                 * directly. (fmt_apply for non-string types always
                 * wraps in quotes; we want the inner text.) */
                if (is_literal) {
                    b[j] = nuq_to_json_string(pe.items[j]);
                } else {
                    VALUE fv = fmt_apply(fmt_id, pe.items[j]);
                    /* Strip leading/trailing " from the json-encoded
                     * formatted string so it concats as text content. */
                    struct nuq_obj *fo = NUQ_PTR(fv);
                    if (fo->str.len >= 2 && fo->str.bytes[0] == '"' &&
                        fo->str.bytes[fo->str.len-1] == '"') {
                        b[j] = nuq_make_string(fo->str.bytes + 1, fo->str.len - 2);
                    } else {
                        b[j] = fv;
                    }
                }
            }
            parts[i].items = b;
            parts[i].count = pc;
            c->pool_top = t0;
            if (pc == 0) return EMIT_EMPTY;
        }
        size_t outer_top = c->pool_top;
        size_t idx[16] = {0};
        for (;;) {
            size_t total = 0; size_t lens[16]; VALUE strs[16];
            for (size_t i = 0; i < e->cnt; i++) {
                VALUE s = parts[i].items[idx[i]];
                strs[i] = s; lens[i] = NUQ_PTR(s)->str.len; total += lens[i];
            }
            char *buf = (char *)GC_malloc_atomic(total + 1);
            char *p = buf;
            for (size_t i = 0; i < e->cnt; i++) {
                memcpy(p, NUQ_PTR(strs[i])->str.bytes, lens[i]);
                p += lens[i];
            }
            buf[total] = '\0';
            nuq_pool_push(c, nuq_make_string_take(buf, total));
            ssize_t pos = (ssize_t)e->cnt - 1;
            for (; pos >= 0; pos--) {
                idx[pos]++;
                if (idx[pos] < parts[pos].count) break;
                idx[pos] = 0;
            }
            if (pos < 0) break;
        }
        return nuq_emit_slice(c, outer_top);
    }
    size_t top0 = c->pool_top;
    EMIT bo = EVAL(c, body);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = bo.count;
    VALUE small[16];
    VALUE *local = (cnt <= 16) ? small : (VALUE *)nuq_scratch_alloc(cnt * sizeof(VALUE));
    memcpy(local, bo.items, cnt * sizeof(VALUE));
    c->pool_top = top0;
    for (uint32_t i = 0; i < cnt; i++) nuq_pool_push(c, fmt_apply(fmt_id, local[i]));
    return nuq_emit_slice(c, top0);
}

/* --- 1-arg builtins (with sub-expression body) ---------------------- */

EMIT
nuq_map_values_eval(CTX *c, struct Node *body)
{
    VALUE in = c->input;
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_ARRAY) {
        struct nuq_obj *o = NUQ_PTR(in);
        VALUE out = nuq_make_array(o->arr.len);
        VALUE saved = c->input;
        for (size_t i = 0; i < o->arr.len; i++) {
            c->input = o->arr.items[i];
            size_t t0 = c->pool_top;
            EMIT bo = EVAL(c, body);
            if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
            if (bo.count > 0) nuq_array_push(out, bo.items[0]);
            c->pool_top = t0;
        }
        c->input = saved;
        return nuq_emit_one(c, out);
    }
    if (NUQ_IS_PTR(in) && NUQ_PTR(in)->type == NUQ_T_OBJECT) {
        struct nuq_obj *o = NUQ_PTR(in);
        VALUE out = nuq_make_object(o->obj.len);
        VALUE saved = c->input;
        for (size_t i = 0; i < o->obj.len; i++) {
            c->input = o->obj.vals[i];
            size_t t0 = c->pool_top;
            EMIT bo = EVAL(c, body);
            if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
            if (bo.count > 0) nuq_object_set(out, o->obj.keys[i], bo.items[0]);
            c->pool_top = t0;
        }
        c->input = saved;
        return nuq_emit_one(c, out);
    }
    return err_emit(c, "map_values: not array/object");
}

EMIT
nuq_with_entries_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_OBJECT))
        return err_emit(c, "with_entries: not object");
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE out = nuq_make_object(o->obj.len);
    VALUE saved = c->input;
    for (size_t i = 0; i < o->obj.len; i++) {
        VALUE e = nuq_make_object(2);
        nuq_object_set_cstr(e, "key", o->obj.keys[i]);
        nuq_object_set_cstr(e, "value", o->obj.vals[i]);
        c->input = e;
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        for (uint32_t j = 0; j < bo.count; j++) {
            VALUE ne = bo.items[j];
            if (!(NUQ_IS_PTR(ne) && NUQ_PTR(ne)->type == NUQ_T_OBJECT)) continue;
            VALUE nk = nuq_object_get_cstr(ne, "key");
            VALUE nv = nuq_object_get_cstr(ne, "value");
            if (!(NUQ_IS_PTR(nk) && NUQ_PTR(nk)->type == NUQ_T_STRING)) nk = nuq_to_json_string(nk);
            nuq_object_set(out, nk, nv);
        }
        c->pool_top = t0;
    }
    c->input = saved;
    return nuq_emit_one(c, out);
}

/* Recursively transform input bottom-up: descend into containers,
 * rebuild with transformed children, then apply body to the rebuilt
 * value.  When body emits empty (e.g. via `select(...)`), the entry
 * is DROPPED from the parent container — this propagates via *dropped. */
static VALUE
nuq_walk_recurse(CTX *c, struct Node *body, VALUE v, bool *dropped)
{
    VALUE rebuilt = v;
    *dropped = false;
    if (NUQ_IS_PTR(v)) {
        struct nuq_obj *o = NUQ_PTR(v);
        if (o->type == NUQ_T_ARRAY) {
            VALUE arr = nuq_make_array(o->arr.len);
            for (size_t i = 0; i < o->arr.len; i++) {
                bool child_drop = false;
                VALUE child = nuq_walk_recurse(c, body, o->arr.items[i], &child_drop);
                if (c->error != NUQ_NULL) return NUQ_NULL;
                if (!child_drop) nuq_array_push(arr, child);
            }
            rebuilt = arr;
        } else if (o->type == NUQ_T_OBJECT) {
            VALUE obj = nuq_make_object(o->obj.len > 4 ? o->obj.len : 4);
            for (size_t i = 0; i < o->obj.len; i++) {
                bool child_drop = false;
                VALUE child = nuq_walk_recurse(c, body, o->obj.vals[i], &child_drop);
                if (c->error != NUQ_NULL) return NUQ_NULL;
                if (!child_drop) nuq_object_set(obj, o->obj.keys[i], child);
            }
            rebuilt = obj;
        }
    }
    VALUE saved = c->input;
    c->input = rebuilt;
    size_t t0 = c->pool_top;
    EMIT bo = EVAL(c, body);
    if (c->error != NUQ_NULL) {
        c->pool_top = t0; c->input = saved;
        return NUQ_NULL;
    }
    VALUE out;
    if (bo.count == 0) {
        *dropped = true;
        out = rebuilt;
    } else {
        out = bo.items[0];
    }
    c->pool_top = t0;
    c->input = saved;
    return out;
}

EMIT
nuq_walk_eval(CTX *c, struct Node *body)
{
    /* At each internal level, walk takes only the first emit of body
     * (jq semantics for the C-implemented walk).  But at the OUTER
     * level, all emits propagate — so `walk(.,1)` on a leaf produces
     * both the rebuilt value and 1. */
    VALUE input = c->input;
    VALUE rebuilt = input;
    if (NUQ_IS_PTR(input)) {
        struct nuq_obj *o = NUQ_PTR(input);
        if (o->type == NUQ_T_ARRAY) {
            VALUE arr = nuq_make_array(o->arr.len);
            for (size_t i = 0; i < o->arr.len; i++) {
                bool child_drop = false;
                VALUE child = nuq_walk_recurse(c, body, o->arr.items[i], &child_drop);
                if (c->error != NUQ_NULL) return EMIT_EMPTY;
                if (!child_drop) nuq_array_push(arr, child);
            }
            rebuilt = arr;
        } else if (o->type == NUQ_T_OBJECT) {
            VALUE obj = nuq_make_object(o->obj.len > 4 ? o->obj.len : 4);
            for (size_t i = 0; i < o->obj.len; i++) {
                bool child_drop = false;
                VALUE child = nuq_walk_recurse(c, body, o->obj.vals[i], &child_drop);
                if (c->error != NUQ_NULL) return EMIT_EMPTY;
                if (!child_drop) nuq_object_set(obj, o->obj.keys[i], child);
            }
            rebuilt = obj;
        }
    }
    VALUE saved = c->input;
    c->input = rebuilt;
    EMIT bo = EVAL(c, body);
    c->input = saved;
    return bo;
}

/* recurse(f) / recurse(f; cond):
 *
 *   def recurse(f):    def r: ., (f | r); r;
 *   def recurse(f;c):  def r: ., (f | select(c) | r); r;
 *
 * We hand-roll: emit current, evaluate body, for each emit (filtered
 * by cond if given), recurse on it.  Iteration is depth-first to
 * match jq's order. */
static void
nuq_recurse_dfs(CTX *c, struct Node *body, struct Node *cond, VALUE v)
{
    nuq_pool_push(c, v);
    VALUE saved = c->input;
    c->input = v;
    size_t t0 = c->pool_top;
    EMIT bo = EVAL(c, body);
    if (c->error != NUQ_NULL) { c->input = saved; return; }
    /* Snapshot — recursion below grows the pool so we can't iterate
     * a live slice. */
    uint32_t cnt = bo.count;
    VALUE small[16];
    VALUE *local = (cnt <= 16) ? small
                               : (VALUE *)nuq_scratch_alloc(cnt * sizeof(VALUE));
    memcpy(local, bo.items, cnt * sizeof(VALUE));
    c->pool_top = t0;
    for (uint32_t i = 0; i < cnt; i++) {
        if (cond != NULL) {
            c->input = local[i];
            size_t tt = c->pool_top;
            EMIT co = EVAL(c, cond);
            if (c->error != NUQ_NULL) { c->input = saved; return; }
            bool any = false;
            for (uint32_t j = 0; j < co.count; j++)
                if (nuq_truthy(co.items[j])) { any = true; break; }
            c->pool_top = tt;
            if (!any) continue;
        }
        nuq_recurse_dfs(c, body, cond, local[i]);
        if (c->error != NUQ_NULL) { c->input = saved; return; }
    }
    c->input = saved;
}

EMIT
nuq_recurse_eval(CTX *c, struct Node *body, struct Node *cond)
{
    size_t outer = c->pool_top;
    nuq_recurse_dfs(c, body, cond, c->input);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    return nuq_emit_slice(c, outer);
}

/* `env` — build an object of environment variables (lazy, once per
 * call; for hot use the caller can bind it to a variable). */
extern char **environ;
VALUE
nuq_env_object(void)
{
    VALUE r = nuq_make_object(16);
    for (char **p = environ; *p; p++) {
        const char *eq = strchr(*p, '=');
        if (!eq) continue;
        VALUE k = nuq_make_string(*p, eq - *p);
        VALUE v = nuq_make_string(eq + 1, strlen(eq + 1));
        nuq_object_set(r, k, v);
    }
    return r;
}

/* gsub(s; r) — replace ALL non-overlapping occurrences of substring s
 * with r.  Matches jq's gsub when both args are literal (non-regex). */
EMIT
nuq_gsub_eval(CTX *c, struct Node *pat_n, struct Node *repl_n)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return err_emit(c, "gsub: not string");
    size_t t0 = c->pool_top;
    EMIT pe = EVAL(c, pat_n);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (pe.count == 0 || !(NUQ_IS_PTR(pe.items[0]) && NUQ_PTR(pe.items[0])->type == NUQ_T_STRING))
        return err_emit(c, "gsub: pattern not string");
    VALUE pv = pe.items[0];
    c->pool_top = t0;
    EMIT re = EVAL(c, repl_n);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (re.count == 0 || !(NUQ_IS_PTR(re.items[0]) && NUQ_PTR(re.items[0])->type == NUQ_T_STRING))
        return err_emit(c, "gsub: replacement not string");
    VALUE rv = re.items[0];
    c->pool_top = t0;
    struct nuq_obj *si = NUQ_PTR(c->input);
    struct nuq_obj *sp = NUQ_PTR(pv);
    struct nuq_obj *sr = NUQ_PTR(rv);
    if (sp->str.len == 0) return nuq_emit_one(c, c->input);   /* empty pat: identity */
    char *out = NULL; size_t on = 0;
    FILE *fp = open_memstream(&out, &on);
    size_t i = 0;
    while (i + sp->str.len <= si->str.len) {
        if (memcmp(si->str.bytes + i, sp->str.bytes, sp->str.len) == 0) {
            fwrite(sr->str.bytes, 1, sr->str.len, fp);
            i += sp->str.len;
        } else {
            fputc(si->str.bytes[i], fp);
            i++;
        }
    }
    while (i < si->str.len) { fputc(si->str.bytes[i], fp); i++; }
    fclose(fp);
    VALUE result = nuq_make_string(out, on);
    free(out);
    return nuq_emit_one(c, result);
}

/* sub(s; r) — replace FIRST occurrence only. */
EMIT
nuq_sub_eval(CTX *c, struct Node *pat_n, struct Node *repl_n)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return err_emit(c, "sub: not string");
    size_t t0 = c->pool_top;
    EMIT pe = EVAL(c, pat_n);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (pe.count == 0 || !(NUQ_IS_PTR(pe.items[0]) && NUQ_PTR(pe.items[0])->type == NUQ_T_STRING))
        return err_emit(c, "sub: pattern not string");
    VALUE pv = pe.items[0];
    c->pool_top = t0;
    EMIT re = EVAL(c, repl_n);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (re.count == 0 || !(NUQ_IS_PTR(re.items[0]) && NUQ_PTR(re.items[0])->type == NUQ_T_STRING))
        return err_emit(c, "sub: replacement not string");
    VALUE rv = re.items[0];
    c->pool_top = t0;
    struct nuq_obj *si = NUQ_PTR(c->input);
    struct nuq_obj *sp = NUQ_PTR(pv);
    struct nuq_obj *sr = NUQ_PTR(rv);
    if (sp->str.len == 0) return nuq_emit_one(c, c->input);
    /* find first */
    size_t hit = (size_t)-1;
    for (size_t i = 0; i + sp->str.len <= si->str.len; i++) {
        if (memcmp(si->str.bytes + i, sp->str.bytes, sp->str.len) == 0) {
            hit = i; break;
        }
    }
    if (hit == (size_t)-1) return nuq_emit_one(c, c->input);
    size_t out_len = si->str.len - sp->str.len + sr->str.len;
    char *buf = (char *)GC_malloc_atomic(out_len + 1);
    memcpy(buf, si->str.bytes, hit);
    memcpy(buf + hit, sr->str.bytes, sr->str.len);
    memcpy(buf + hit + sr->str.len,
           si->str.bytes + hit + sp->str.len,
           si->str.len - hit - sp->str.len);
    buf[out_len] = '\0';
    return nuq_emit_one(c, nuq_make_string_take(buf, out_len));
}

/* Recursively flatten `arr` to `depth` levels into `out`.
 * `depth == 0` ⇒ append elements as-is, no recursion. */
static void
flatten_into(VALUE arr, int depth, VALUE out)
{
    struct nuq_obj *o = NUQ_PTR(arr);
    for (size_t i = 0; i < o->arr.len; i++) {
        VALUE v = o->arr.items[i];
        if (depth > 0 && NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
            flatten_into(v, depth - 1, out);
        } else {
            nuq_array_push(out, v);
        }
    }
}

VALUE
nuq_flatten_eval(VALUE arr, int depth)
{
    VALUE out = nuq_make_array(NUQ_PTR(arr)->arr.len);
    flatten_into(arr, depth, out);
    return out;
}

EMIT
nuq_flatten1_eval(CTX *c, struct Node *depth_expr)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_emit(c, "flatten: not array");
    size_t outer = c->pool_top;
    EMIT bo = EVAL(c, depth_expr);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t dc = bo.count;
    if (dc == 0) { c->pool_top = outer; return EMIT_EMPTY; }
    int64_t small[16];
    int64_t *ds = (dc <= 16) ? small : (int64_t *)nuq_scratch_alloc(dc * sizeof(int64_t));
    for (uint32_t i = 0; i < dc; i++) {
        VALUE dv = bo.items[i];
        if (!NUQ_IS_FIX(dv)) { c->pool_top = outer; return err_emit(c, "flatten: depth not int"); }
        ds[i] = NUQ_FIX_VAL(dv);
    }
    c->pool_top = outer;
    for (uint32_t i = 0; i < dc; i++) {
        if (ds[i] < 0) return err_emit(c, "flatten depth must not be negative");
        nuq_pool_push(c, nuq_flatten_eval(c->input, (int)ds[i]));
    }
    return nuq_emit_slice(c, outer);
}

/* `while(cond; update)`: emit ., then if cond(.) truthy apply update,
 * loop.  Stops at first non-truthy cond (without emitting the
 * post-stop value). */
EMIT
nuq_while_eval(CTX *c, struct Node *cond, struct Node *update)
{
    size_t outer = c->pool_top;
    VALUE cur = c->input;
    VALUE saved = c->input;
    for (;;) {
        c->input = cur;
        size_t t0 = c->pool_top;
        EMIT co = EVAL(c, cond);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        bool truthy = false;
        for (uint32_t i = 0; i < co.count; i++)
            if (nuq_truthy(co.items[i])) { truthy = true; break; }
        c->pool_top = t0;
        if (!truthy) break;
        nuq_pool_push(c, cur);
        c->input = cur;
        size_t t1 = c->pool_top;
        EMIT uo = EVAL(c, update);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        if (uo.count == 0) { c->pool_top = t1; break; }
        cur = uo.items[0];
        c->pool_top = t1;
    }
    c->input = saved;
    return nuq_emit_slice(c, outer);
}

/* `until(cond; update)`: apply update until cond(.) is truthy, emit
 * the final ..  Note: jq's until emits exactly one value. */
EMIT
nuq_until_eval(CTX *c, struct Node *cond, struct Node *update)
{
    VALUE cur = c->input;
    VALUE saved = c->input;
    for (;;) {
        c->input = cur;
        size_t t0 = c->pool_top;
        EMIT co = EVAL(c, cond);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        bool truthy = false;
        for (uint32_t i = 0; i < co.count; i++)
            if (nuq_truthy(co.items[i])) { truthy = true; break; }
        c->pool_top = t0;
        if (truthy) break;
        c->input = cur;
        size_t t1 = c->pool_top;
        EMIT uo = EVAL(c, update);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        if (uo.count == 0) { c->pool_top = t1; break; }
        cur = uo.items[0];
        c->pool_top = t1;
    }
    c->input = saved;
    return nuq_emit_one(c, cur);
}

EMIT
nuq_range2_eval(CTX *c, struct Node *from, struct Node *to)
{
    size_t outer = c->pool_top;
    size_t t0 = c->pool_top;
    EMIT m = EVAL(c, from); if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t mc = m.count; VALUE msm[16]; VALUE *ml = (mc<=16)?msm:(VALUE*)GC_malloc(mc*sizeof(VALUE));
    memcpy(ml, m.items, mc*sizeof(VALUE)); c->pool_top = t0;
    EMIT nv = EVAL(c, to); if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t nc = nv.count; VALUE nsm[16]; VALUE *nl = (nc<=16)?nsm:(VALUE*)GC_malloc(nc*sizeof(VALUE));
    memcpy(nl, nv.items, nc*sizeof(VALUE)); c->pool_top = outer;
    for (uint32_t i = 0; i < mc; i++)
    for (uint32_t j = 0; j < nc; j++) {
        int64_t lo = to_int64(ml[i]), hi = to_int64(nl[j]);
        for (int64_t k = lo; k < hi; k++) nuq_pool_push(c, nuq_make_int(k));
    }
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_range3_eval(CTX *c, struct Node *from, struct Node *to, struct Node *step)
{
    size_t outer = c->pool_top;
    size_t t0 = c->pool_top;
    EMIT m = EVAL(c, from); if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t mc = m.count; VALUE msm[16]; VALUE *ml = (mc<=16)?msm:(VALUE*)GC_malloc(mc*sizeof(VALUE));
    memcpy(ml, m.items, mc*sizeof(VALUE)); c->pool_top = t0;
    EMIT nv = EVAL(c, to); if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t nc = nv.count; VALUE nsm[16]; VALUE *nl = (nc<=16)?nsm:(VALUE*)GC_malloc(nc*sizeof(VALUE));
    memcpy(nl, nv.items, nc*sizeof(VALUE)); c->pool_top = t0;
    EMIT s = EVAL(c, step); if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t sc = s.count; VALUE ssm[16]; VALUE *sl = (sc<=16)?ssm:(VALUE*)GC_malloc(sc*sizeof(VALUE));
    memcpy(sl, s.items, sc*sizeof(VALUE)); c->pool_top = outer;
    for (uint32_t i = 0; i < mc; i++)
    for (uint32_t j = 0; j < nc; j++)
    for (uint32_t k = 0; k < sc; k++) {
        int64_t lo = to_int64(ml[i]), hi = to_int64(nl[j]), st = to_int64(sl[k]);
        if (st == 0) continue;
        if (st > 0) for (int64_t x = lo; x < hi; x += st) nuq_pool_push(c, nuq_make_int(x));
        else        for (int64_t x = lo; x > hi; x += st) nuq_pool_push(c, nuq_make_int(x));
    }
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_has_eval(CTX *c, struct Node *key)
{
    size_t outer = c->pool_top;
    size_t t0 = c->pool_top;
    EMIT buf = EVAL(c, key);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = buf.count;
    VALUE small[16];
    VALUE *local = (cnt<=16)?small:(VALUE*)GC_malloc(cnt*sizeof(VALUE));
    memcpy(local, buf.items, cnt*sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t i = 0; i < cnt; i++) {
        VALUE k = local[i];
        bool t;
        if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_OBJECT) {
            t = nuq_object_has(c->input, k);
        } else if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY) {
            /* `has(k)` on an array — k must be a non-negative integer. */
            if (NUQ_IS_FIX(k)) {
                int64_t idx = NUQ_FIX_VAL(k);
                t = idx >= 0 && idx < (int64_t)NUQ_PTR(c->input)->arr.len;
            } else if (NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_DOUBLE) {
                double d = NUQ_PTR(k)->dbl;
                if (isnan(d) || d < 0 || d != (int64_t)d) t = false;
                else t = (int64_t)d < (int64_t)NUQ_PTR(c->input)->arr.len;
            } else t = false;
        } else { t = false; }
        nuq_pool_push(c, t ? NUQ_TRUE : NUQ_FALSE);
    }
    (void)t0;
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_in_eval(CTX *c, struct Node *container)
{
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, container);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = buf.count;
    VALUE small[16];
    VALUE *local = (cnt<=16)?small:(VALUE*)GC_malloc(cnt*sizeof(VALUE));
    memcpy(local, buf.items, cnt*sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t i = 0; i < cnt; i++) {
        VALUE cn = local[i];
        bool t;
        if (NUQ_IS_PTR(cn) && NUQ_PTR(cn)->type == NUQ_T_OBJECT) {
            t = nuq_object_has(cn, c->input);
        } else if (NUQ_IS_PTR(cn) && NUQ_PTR(cn)->type == NUQ_T_ARRAY) {
            int64_t idx = NUQ_IS_FIX(c->input) ? NUQ_FIX_VAL(c->input) : 0;
            t = idx >= 0 && idx < (int64_t)NUQ_PTR(cn)->arr.len;
        } else { t = false; }
        nuq_pool_push(c, t ? NUQ_TRUE : NUQ_FALSE);
    }
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_contains_eval(CTX *c, struct Node *rhs)
{
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, rhs);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = buf.count;
    VALUE small[16];
    VALUE *local = (cnt<=16)?small:(VALUE*)GC_malloc(cnt*sizeof(VALUE));
    memcpy(local, buf.items, cnt*sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t i = 0; i < cnt; i++) {
        /* jq raises an error on type mismatch; we mirror that
         * to stay compatible.  Booleans / null / numbers are
         * homogeneous-only, anything mixed is a type error. */
        VALUE a = c->input, b = local[i];
        bool a_fix = NUQ_IS_FIX(a), b_fix = NUQ_IS_FIX(b);
        enum nuq_type ta = a_fix ? NUQ_T_DOUBLE : NUQ_PTR(a)->type;
        enum nuq_type tb = b_fix ? NUQ_T_DOUBLE : NUQ_PTR(b)->type;
        if ((a_fix || ta == NUQ_T_DOUBLE) && (b_fix || tb == NUQ_T_DOUBLE)) {
            /* both numeric: equality */
            nuq_pool_push(c, nuq_eq(a, b) ? NUQ_TRUE : NUQ_FALSE);
            continue;
        }
        if (ta != tb) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "%s and %s cannot have their containment checked",
                     nuq_type_name(a), nuq_type_name(b));
            return err_emit(c, msg);
        }
        bool r = nuq_contains(a, b);
        if (UNLIKELY(c->error != NUQ_NULL)) {
            c->pool_top = outer;
            return EMIT_EMPTY;
        }
        nuq_pool_push(c, r ? NUQ_TRUE : NUQ_FALSE);
    }
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_split_eval(CTX *c, struct Node *sep)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return err_emit(c, "split: not string");
    size_t t0 = c->pool_top;
    EMIT buf = EVAL(c, sep);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (buf.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE s = buf.items[0];
    c->pool_top = t0;
    if (!(NUQ_IS_PTR(s) && NUQ_PTR(s)->type == NUQ_T_STRING)) return err_emit(c, "split: sep not string");
    return nuq_emit_one(c, nuq_op_div(c->input, s));
}

/* `splits(s)` — like split(s) but emits each piece as a separate
 * value rather than as one array.  jq compatibility. */
EMIT
nuq_splits_eval(CTX *c, struct Node *sep)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return err_emit(c, "splits: not string");
    size_t t0 = c->pool_top;
    EMIT buf = EVAL(c, sep);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (buf.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE s = buf.items[0];
    c->pool_top = t0;
    if (!(NUQ_IS_PTR(s) && NUQ_PTR(s)->type == NUQ_T_STRING))
        return err_emit(c, "splits: sep not string");
    /* Reuse op_div which returns an array of pieces, then unpack. */
    VALUE arr = nuq_op_div(c->input, s);
    if (!(NUQ_IS_PTR(arr) && NUQ_PTR(arr)->type == NUQ_T_ARRAY))
        return err_emit(c, "splits: internal error");
    struct nuq_obj *ao = NUQ_PTR(arr);
    size_t outer = c->pool_top;
    for (size_t i = 0; i < ao->arr.len; i++) nuq_pool_push(c, ao->arr.items[i]);
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_join_eval(CTX *c, struct Node *sep)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_emit(c, "join: not array");
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, sep);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    /* Snapshot the separator values — we'll iterate, each producing a
     * result string.  Multi-emit support: `join(",","/")` yields two
     * concatenations. */
    uint32_t sc = buf.count;
    if (sc == 0) { c->pool_top = outer; return EMIT_EMPTY; }
    VALUE seps[16];
    VALUE *seps_ptr = (sc <= 16) ? seps : (VALUE *)nuq_scratch_alloc(sc * sizeof(VALUE));
    memcpy(seps_ptr, buf.items, sc * sizeof(VALUE));
    c->pool_top = outer;
    struct nuq_obj *ao = NUQ_PTR(c->input);
    for (uint32_t k = 0; k < sc; k++) {
        VALUE s = seps_ptr[k];
        if (!(NUQ_IS_PTR(s) && NUQ_PTR(s)->type == NUQ_T_STRING))
            return err_emit(c, "join: sep not string");
        struct nuq_obj *so = NUQ_PTR(s);
        char *out = NULL; size_t on = 0;
        FILE *fp = open_memstream(&out, &on);
        bool errored = false;
        for (size_t i = 0; i < ao->arr.len; i++) {
            if (i) fwrite(so->str.bytes, 1, so->str.len, fp);
            VALUE v = ao->arr.items[i];
            if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL) continue;
            if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING) {
                fwrite(NUQ_PTR(v)->str.bytes, 1, NUQ_PTR(v)->str.len, fp);
            } else if (NUQ_IS_FIX(v) ||
                       (NUQ_IS_PTR(v) &&
                        (NUQ_PTR(v)->type == NUQ_T_DOUBLE ||
                         NUQ_PTR(v)->type == NUQ_T_BOOL))) {
                /* numbers / booleans get their JSON string form. */
                VALUE js = nuq_to_json_string(v);
                fwrite(NUQ_PTR(js)->str.bytes, 1, NUQ_PTR(js)->str.len, fp);
            } else {
                /* Arrays / objects: jq raises "cannot be added" against
                 * the partial concatenation of the previously-joined
                 * strings (with separators). */
                fclose(fp);
                free(out);
                char da[80], db[80], msg[200];
                char *pbuf = NULL; size_t pbn = 0;
                FILE *pfp = open_memstream(&pbuf, &pbn);
                for (size_t j = 0; j <= i; j++) {
                    if (j > 0) fwrite(so->str.bytes, 1, so->str.len, pfp);
                    if (j == i) break;
                    VALUE w = ao->arr.items[j];
                    if (NUQ_IS_PTR(w) && NUQ_PTR(w)->type == NUQ_T_STRING) {
                        fwrite(NUQ_PTR(w)->str.bytes, 1, NUQ_PTR(w)->str.len, pfp);
                    }
                }
                fclose(pfp);
                VALUE partial = nuq_make_string(pbuf, pbn);
                free(pbuf);
                nuq_value_descr(partial, da, sizeof(da));
                nuq_value_descr(v, db, sizeof(db));
                int w = snprintf(msg, sizeof(msg), "%s and %s cannot be added", da, db);
                c->error = nuq_make_string(msg, (size_t)w);
                errored = true;
                break;
            }
        }
        if (errored) return EMIT_EMPTY;
        fclose(fp);
        nuq_pool_push(c, nuq_make_string(out, on));
        free(out);
    }
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_startswith_eval(CTX *c, struct Node *prefix)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return err_emit(c, "startswith: not string");
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, prefix);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = buf.count;
    VALUE small[16];
    VALUE *local = (cnt<=16)?small:(VALUE*)GC_malloc(cnt*sizeof(VALUE));
    memcpy(local, buf.items, cnt*sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t i = 0; i < cnt; i++) {
        VALUE p = local[i];
        if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING)) { nuq_pool_push(c, NUQ_FALSE); continue; }
        struct nuq_obj *po = NUQ_PTR(p);
        struct nuq_obj *io = NUQ_PTR(c->input);
        bool t = io->str.len >= po->str.len && memcmp(io->str.bytes, po->str.bytes, po->str.len) == 0;
        nuq_pool_push(c, t ? NUQ_TRUE : NUQ_FALSE);
    }
    return nuq_emit_slice(c, outer);
}

/* `ltrimstr(s)`: if input starts with s, strip prefix; else input. */
EMIT
nuq_ltrimstr_eval(CTX *c, struct Node *prefix)
{
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, prefix);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = buf.count;
    VALUE small[16];
    VALUE *local = (cnt<=16)?small:(VALUE*)GC_malloc(cnt*sizeof(VALUE));
    memcpy(local, buf.items, cnt*sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t i = 0; i < cnt; i++) {
        VALUE p = local[i];
        bool in_is_str = NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING;
        bool p_is_str  = NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING;
        if (!in_is_str || !p_is_str) {
            /* jq's ltrimstr errors when either operand isn't a string. */
            c->error = nuq_make_string("startswith() requires string inputs", 35);
            return EMIT_EMPTY;
        }
        struct nuq_obj *po = NUQ_PTR(p);
        struct nuq_obj *io = NUQ_PTR(c->input);
        if (io->str.len >= po->str.len &&
            memcmp(io->str.bytes, po->str.bytes, po->str.len) == 0) {
            nuq_pool_push(c, nuq_make_string(io->str.bytes + po->str.len,
                                             io->str.len - po->str.len));
        } else {
            nuq_pool_push(c, c->input);
        }
    }
    return nuq_emit_slice(c, outer);
}

/* `rtrimstr(s)`: if input ends with s, strip suffix; else input. */
EMIT
nuq_rtrimstr_eval(CTX *c, struct Node *suffix)
{
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, suffix);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = buf.count;
    VALUE small[16];
    VALUE *local = (cnt<=16)?small:(VALUE*)GC_malloc(cnt*sizeof(VALUE));
    memcpy(local, buf.items, cnt*sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t i = 0; i < cnt; i++) {
        VALUE p = local[i];
        bool in_is_str = NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING;
        bool p_is_str  = NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING;
        if (!in_is_str || !p_is_str) {
            /* jq's rtrimstr errors when either operand isn't a string. */
            c->error = nuq_make_string("endswith() requires string inputs", 33);
            return EMIT_EMPTY;
        }
        struct nuq_obj *po = NUQ_PTR(p);
        struct nuq_obj *io = NUQ_PTR(c->input);
        if (io->str.len >= po->str.len &&
            memcmp(io->str.bytes + io->str.len - po->str.len,
                   po->str.bytes, po->str.len) == 0) {
            nuq_pool_push(c, nuq_make_string(io->str.bytes,
                                             io->str.len - po->str.len));
        } else {
            nuq_pool_push(c, c->input);
        }
    }
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_endswith_eval(CTX *c, struct Node *suffix)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING))
        return err_emit(c, "endswith: not string");
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, suffix);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = buf.count;
    VALUE small[16];
    VALUE *local = (cnt<=16)?small:(VALUE*)GC_malloc(cnt*sizeof(VALUE));
    memcpy(local, buf.items, cnt*sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t i = 0; i < cnt; i++) {
        VALUE p = local[i];
        if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING)) { nuq_pool_push(c, NUQ_FALSE); continue; }
        struct nuq_obj *po = NUQ_PTR(p);
        struct nuq_obj *io = NUQ_PTR(c->input);
        bool t = io->str.len >= po->str.len &&
            memcmp(io->str.bytes + io->str.len - po->str.len, po->str.bytes, po->str.len) == 0;
        nuq_pool_push(c, t ? NUQ_TRUE : NUQ_FALSE);
    }
    return nuq_emit_slice(c, outer);
}

/* --- *_by builtins -------------------------------------------------- */

/* Flat (key, original_idx) pair — sort over this struct directly is
 * substantially faster than chasing `nuq_obj.arr.items[0]` through
 * a wrapper array each compare (no double indirection, fits in a
 * single cache line per pair). */
struct nuq_kv { VALUE key; uint32_t idx; uint32_t _pad; };

/* Specialised in-place sort over `struct nuq_kv[]` and `VALUE[]` —
 * calls `nuq_cmp` inline (no qsort function-pointer dispatch) and uses
 * introsort (quicksort with median-of-three pivot, insertion sort for
 * small partitions).  ~3× faster than libc qsort for the fixnum-heavy
 * keys typical of `sort` / `sort_by` / `group_by`.  Not stable. */
static inline void
val_swap(VALUE *a, VALUE *b)
{
    VALUE t = *a; *a = *b; *b = t;
}

static void
val_insertion_sort(VALUE *a, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        VALUE x = a[i];
        size_t j = i;
        while (j > 0 && nuq_cmp(a[j-1], x) > 0) {
            a[j] = a[j-1];
            j--;
        }
        a[j] = x;
    }
}

void
nuq_value_sort(VALUE *a, size_t n)
{
    while (n > 16) {
        size_t mi = n / 2;
        if (nuq_cmp(a[0], a[mi]) > 0) val_swap(&a[0], &a[mi]);
        if (nuq_cmp(a[0], a[n-1]) > 0) val_swap(&a[0], &a[n-1]);
        if (nuq_cmp(a[mi], a[n-1]) > 0) val_swap(&a[mi], &a[n-1]);
        VALUE pivot = a[mi];
        val_swap(&a[mi], &a[n-2]);
        size_t i = 0, j = n - 2;
        for (;;) {
            while (nuq_cmp(a[++i], pivot) < 0) {}
            while (nuq_cmp(a[--j], pivot) > 0) {}
            if (i >= j) break;
            val_swap(&a[i], &a[j]);
        }
        val_swap(&a[i], &a[n-2]);
        if (i < n - i - 1) {
            nuq_value_sort(a, i);
            a += i + 1;
            n -= i + 1;
        } else {
            nuq_value_sort(a + i + 1, n - i - 1);
            n = i;
        }
    }
    val_insertion_sort(a, n);
}

static inline void
kv_swap(struct nuq_kv *a, struct nuq_kv *b)
{
    struct nuq_kv t = *a;
    *a = *b;
    *b = t;
}

static void
kv_insertion_sort(struct nuq_kv *a, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        struct nuq_kv x = a[i];
        size_t j = i;
        while (j > 0 && nuq_cmp(a[j-1].key, x.key) > 0) {
            a[j] = a[j-1];
            j--;
        }
        a[j] = x;
    }
}

static void
kv_quicksort(struct nuq_kv *a, size_t n)
{
    while (n > 16) {
        /* median-of-three pivot at indices 0, n/2, n-1. */
        size_t mi = n / 2;
        if (nuq_cmp(a[0].key, a[mi].key) > 0) kv_swap(&a[0], &a[mi]);
        if (nuq_cmp(a[0].key, a[n-1].key) > 0) kv_swap(&a[0], &a[n-1]);
        if (nuq_cmp(a[mi].key, a[n-1].key) > 0) kv_swap(&a[mi], &a[n-1]);
        VALUE pivot = a[mi].key;
        kv_swap(&a[mi], &a[n-2]);  /* park pivot at n-2 */
        size_t i = 0, j = n - 2;
        for (;;) {
            while (nuq_cmp(a[++i].key, pivot) < 0) {}
            while (nuq_cmp(a[--j].key, pivot) > 0) {}
            if (i >= j) break;
            kv_swap(&a[i], &a[j]);
        }
        kv_swap(&a[i], &a[n-2]);    /* restore pivot */
        /* Recurse on smaller side, iterate on larger to bound stack. */
        if (i < n - i - 1) {
            kv_quicksort(a, i);
            a += i + 1;
            n -= i + 1;
        } else {
            kv_quicksort(a + i + 1, n - i - 1);
            n = i;
        }
    }
    kv_insertion_sort(a, n);
}

EMIT
nuq_sort_by_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_emit(c, "sort_by: not array");
    /* Hold the input array via a local pinned slot — c->input gets
     * mutated mid-loop to feed each element to the body, so we can't
     * just pin &c->input.  `kvs` lives in scratch (non-Cheney) and
     * the keys go into a parallel pinned VALUE[] so a body-eval GC
     * can forward them.  `kvs[i].key` is reread from the pinned
     * array after the loop. */
    VALUE input_arr = c->input;
    NUQ_GC_PIN1(input_arr);
    size_t N = NUQ_PTR(input_arr)->arr.len;
    struct nuq_kv *kvs = (struct nuq_kv *)nuq_scratch_alloc(N * sizeof(struct nuq_kv));
    VALUE *keys_pin = (VALUE *)nuq_scratch_alloc(N * sizeof(VALUE));
    for (size_t i = 0; i < N; i++) keys_pin[i] = NUQ_NULL;
    NUQ_GC_PIN_ARR(keys_pin, N);
    VALUE saved = c->input;
    for (size_t i = 0; i < N; i++) {
        c->input = NUQ_PTR(input_arr)->arr.items[i];
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; NUQ_GC_UNPIN_ARR(); NUQ_GC_UNPIN(1); return EMIT_EMPTY; }
        VALUE k;
        if (bo.count == 0) k = NUQ_NULL;
        else if (bo.count == 1) k = bo.items[0];
        else {
            /* Multi-key sort: wrap in array for lexicographic tie-break. */
            k = nuq_make_array(bo.count);
            for (uint32_t j = 0; j < bo.count; j++) nuq_array_push(k, bo.items[j]);
        }
        c->pool_top = t0;
        keys_pin[i] = k;
        kvs[i].idx = (uint32_t)i;
    }
    c->input = saved;
    /* Build kvs[].key from the (post-GC-forwarded) pinned keys. */
    for (size_t i = 0; i < N; i++) kvs[i].key = keys_pin[i];
    kv_quicksort(kvs, N);
    VALUE result = nuq_make_array(N);
    struct nuq_obj *r = NUQ_PTR(result);
    struct nuq_obj *src = NUQ_PTR(input_arr);
    for (size_t i = 0; i < N; i++) r->arr.items[i] = src->arr.items[kvs[i].idx];
    r->arr.len = N;
    NUQ_GC_UNPIN_ARR();
    NUQ_GC_UNPIN(1);
    return nuq_emit_one(c, result);
}

EMIT
nuq_group_by_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_emit(c, "group_by: not array");
    VALUE input_arr = c->input;
    NUQ_GC_PIN1(input_arr);
    size_t N = NUQ_PTR(input_arr)->arr.len;
    struct nuq_kv *kvs = (struct nuq_kv *)nuq_scratch_alloc(N * sizeof(struct nuq_kv));
    VALUE *keys_pin = (VALUE *)nuq_scratch_alloc(N * sizeof(VALUE));
    for (size_t i = 0; i < N; i++) keys_pin[i] = NUQ_NULL;
    NUQ_GC_PIN_ARR(keys_pin, N);
    VALUE saved = c->input;
    for (size_t i = 0; i < N; i++) {
        c->input = NUQ_PTR(input_arr)->arr.items[i];
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; NUQ_GC_UNPIN_ARR(); NUQ_GC_UNPIN(1); return EMIT_EMPTY; }
        keys_pin[i] = bo.count > 0 ? bo.items[0] : NUQ_NULL;
        kvs[i].idx = (uint32_t)i;
        c->pool_top = t0;
    }
    c->input = saved;
    for (size_t i = 0; i < N; i++) kvs[i].key = keys_pin[i];
    kv_quicksort(kvs, N);   /* sort doesn't allocate, no GC */

    /* Result-building loop allocates per group → may GC.  We can't
     * use kvs[i].key directly across allocs because kvs is in scratch
     * (not scanned).  Walk via keys_pin (still pinned + forwarded). */
    VALUE result = nuq_make_array(0);
    NUQ_GC_PIN1(result);
    VALUE cur_group = NUQ_NULL;
    VALUE cur_key = NUQ_NULL;
    NUQ_GC_PIN2(cur_group, cur_key);
    bool has = false;
    for (size_t i = 0; i < N; i++) {
        VALUE k = keys_pin[kvs[i].idx];
        VALUE v = NUQ_PTR(input_arr)->arr.items[kvs[i].idx]; /* refetch */
        if (!has || nuq_cmp(k, cur_key) != 0) {
            cur_group = nuq_make_array(0);
            cur_key = k;
            has = true;
            nuq_array_push(cur_group, v);
            nuq_array_push(result, cur_group);
        } else {
            nuq_array_push(cur_group, v);
        }
    }
    NUQ_GC_UNPIN(3);   /* cur_group, cur_key, result */
    NUQ_GC_UNPIN_ARR();
    NUQ_GC_UNPIN(1);   /* input_arr */
    return nuq_emit_one(c, result);
}

EMIT
nuq_unique_by_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_emit(c, "unique_by: not array");
    struct nuq_obj *o = NUQ_PTR(c->input);
    VALUE seen = nuq_make_array(0);
    VALUE result = nuq_make_array(0);
    VALUE saved = c->input;
    for (size_t i = 0; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        VALUE k = bo.count > 0 ? bo.items[0] : NUQ_NULL;
        c->pool_top = t0;
        bool found = false;
        struct nuq_obj *so = NUQ_PTR(seen);
        for (size_t j = 0; j < so->arr.len; j++) if (nuq_eq(so->arr.items[j], k)) { found = true; break; }
        if (!found) {
            nuq_array_push(seen, k);
            nuq_array_push(result, o->arr.items[i]);
        }
    }
    c->input = saved;
    return nuq_emit_one(c, result);
}

EMIT
nuq_min_by_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_emit(c, "min_by: not array");
    struct nuq_obj *o = NUQ_PTR(c->input);
    if (o->arr.len == 0) return nuq_emit_one(c, NUQ_NULL);
    VALUE bestv = o->arr.items[0], bestk = NUQ_NULL;
    VALUE saved = c->input;
    {
        c->input = o->arr.items[0];
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        bestk = bo.count > 0 ? bo.items[0] : NUQ_NULL;
        c->pool_top = t0;
    }
    for (size_t i = 1; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        VALUE k = bo.count > 0 ? bo.items[0] : NUQ_NULL;
        c->pool_top = t0;
        if (nuq_cmp(k, bestk) < 0) { bestk = k; bestv = o->arr.items[i]; }
    }
    c->input = saved;
    return nuq_emit_one(c, bestv);
}

EMIT
nuq_max_by_eval(CTX *c, struct Node *body)
{
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY))
        return err_emit(c, "max_by: not array");
    struct nuq_obj *o = NUQ_PTR(c->input);
    if (o->arr.len == 0) return nuq_emit_one(c, NUQ_NULL);
    VALUE bestv = o->arr.items[0], bestk = NUQ_NULL;
    VALUE saved = c->input;
    {
        c->input = o->arr.items[0];
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        bestk = bo.count > 0 ? bo.items[0] : NUQ_NULL;
        c->pool_top = t0;
    }
    for (size_t i = 1; i < o->arr.len; i++) {
        c->input = o->arr.items[i];
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) { c->input = saved; return EMIT_EMPTY; }
        VALUE k = bo.count > 0 ? bo.items[0] : NUQ_NULL;
        c->pool_top = t0;
        /* >= so ties tie-break to the LAST element (jq semantics). */
        if (nuq_cmp(k, bestk) >= 0) { bestk = k; bestv = o->arr.items[i]; }
    }
    c->input = saved;
    return nuq_emit_one(c, bestv);
}

/* --- string search builtins ---------------------------------------- */

static int64_t byte_to_cp_index(const char *s, size_t blen, size_t bytepos);

EMIT
nuq_indices_eval(CTX *c, struct Node *pat)
{
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, pat);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t pc = buf.count;
    if (pc == 0) { c->pool_top = outer; return EMIT_EMPTY; }
    VALUE psmall[16];
    VALUE *ps = (pc <= 16) ? psmall : (VALUE *)nuq_scratch_alloc(pc * sizeof(VALUE));
    memcpy(ps, buf.items, pc * sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t k = 0; k < pc; k++) {
        VALUE p = ps[k];
        VALUE arr;
        if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING &&
            NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING) {
            struct nuq_obj *io = NUQ_PTR(c->input);
            struct nuq_obj *po = NUQ_PTR(p);
            arr = nuq_make_array(0);
            if (po->str.len > 0) {
                for (size_t i = 0; i + po->str.len <= io->str.len; i++) {
                    if (memcmp(io->str.bytes + i, po->str.bytes, po->str.len) == 0)
                        nuq_array_push(arr, nuq_make_int(byte_to_cp_index(io->str.bytes, io->str.len, i)));
                }
            }
        } else if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY) {
            struct nuq_obj *io = NUQ_PTR(c->input);
            arr = nuq_make_array(0);
            if (NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_ARRAY) {
                struct nuq_obj *po = NUQ_PTR(p);
                if (po->arr.len > 0 && po->arr.len <= io->arr.len) {
                    for (size_t i = 0; i + po->arr.len <= io->arr.len; i++) {
                        bool match = true;
                        for (size_t j = 0; j < po->arr.len; j++) {
                            if (!nuq_eq(io->arr.items[i+j], po->arr.items[j])) {
                                match = false; break;
                            }
                        }
                        if (match) nuq_array_push(arr, nuq_make_int((int64_t)i));
                    }
                }
            } else {
                for (size_t i = 0; i < io->arr.len; i++) {
                    if (nuq_eq(io->arr.items[i], p))
                        nuq_array_push(arr, nuq_make_int((int64_t)i));
                }
            }
        } else if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_NULL) {
            arr = NUQ_NULL;
        } else {
            return err_emit(c, "indices: input not string or array");
        }
        nuq_pool_push(c, arr);
    }
    return nuq_emit_slice(c, outer);
}

/* Convert a byte index into a codepoint index by counting UTF-8 lead
 * bytes (start of multi-byte) up to that position.  Used so index /
 * indices / rindex emit codepoint-aligned positions on strings, as
 * jq does. */
static int64_t
byte_to_cp_index(const char *s, size_t blen, size_t bytepos)
{
    int64_t cp = 0;
    for (size_t i = 0; i < bytepos && i < blen; ) {
        unsigned char x = (unsigned char)s[i];
        if (x < 0x80) i += 1;
        else if ((x & 0xE0) == 0xC0) i += 2;
        else if ((x & 0xF0) == 0xE0) i += 3;
        else if ((x & 0xF8) == 0xF0) i += 4;
        else i += 1;
        cp++;
    }
    return cp;
}

EMIT
nuq_index1_eval(CTX *c, struct Node *pat)
{
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, pat);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t pc = buf.count;
    if (pc == 0) { c->pool_top = outer; return EMIT_EMPTY; }
    /* Snapshot pat values so we can re-evaluate per emit cleanly. */
    VALUE psmall[16];
    VALUE *ps = (pc <= 16) ? psmall : (VALUE *)nuq_scratch_alloc(pc * sizeof(VALUE));
    memcpy(ps, buf.items, pc * sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t k = 0; k < pc; k++) {
        VALUE p = ps[k];
        VALUE r = NUQ_NULL;
        if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING &&
            NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING) {
            struct nuq_obj *io = NUQ_PTR(c->input);
            struct nuq_obj *po = NUQ_PTR(p);
            if (po->str.len > 0) {
                for (size_t i = 0; i + po->str.len <= io->str.len; i++) {
                    if (memcmp(io->str.bytes + i, po->str.bytes, po->str.len) == 0) {
                        r = nuq_make_int(byte_to_cp_index(io->str.bytes, io->str.len, i));
                        break;
                    }
                }
            }
        } else if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_ARRAY) {
            struct nuq_obj *io = NUQ_PTR(c->input);
            if (NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_ARRAY) {
                struct nuq_obj *po = NUQ_PTR(p);
                if (po->arr.len > 0 && po->arr.len <= io->arr.len) {
                    for (size_t i = 0; i + po->arr.len <= io->arr.len; i++) {
                        bool match = true;
                        for (size_t j = 0; j < po->arr.len; j++) {
                            if (!nuq_eq(io->arr.items[i+j], po->arr.items[j])) {
                                match = false; break;
                            }
                        }
                        if (match) { r = nuq_make_int((int64_t)i); break; }
                    }
                }
            } else {
                for (size_t i = 0; i < io->arr.len; i++) {
                    if (nuq_eq(io->arr.items[i], p)) {
                        r = nuq_make_int((int64_t)i); break;
                    }
                }
            }
        } else if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_NULL) {
            /* r already NUQ_NULL */
        } else {
            return err_emit(c, "index: input not string or array");
        }
        nuq_pool_push(c, r);
    }
    return nuq_emit_slice(c, outer);
}

/* `rindex(s)` — last position of `s` in input (per pat emit), or null. */
EMIT
nuq_rindex_eval(CTX *c, struct Node *pat)
{
    size_t outer = c->pool_top;
    EMIT buf = EVAL(c, pat);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t pc = buf.count;
    if (pc == 0) { c->pool_top = outer; return EMIT_EMPTY; }
    VALUE psmall[16];
    VALUE *ps = (pc <= 16) ? psmall : (VALUE *)nuq_scratch_alloc(pc * sizeof(VALUE));
    memcpy(ps, buf.items, pc * sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t k = 0; k < pc; k++) {
        VALUE p = ps[k];
        VALUE r = NUQ_NULL;
        if (NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING &&
            NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING) {
            struct nuq_obj *io = NUQ_PTR(c->input);
            struct nuq_obj *po = NUQ_PTR(p);
            if (po->str.len > 0 && po->str.len <= io->str.len) {
                for (ssize_t i = (ssize_t)(io->str.len - po->str.len); i >= 0; i--) {
                    if (memcmp(io->str.bytes + i, po->str.bytes, po->str.len) == 0) {
                        r = nuq_make_int(byte_to_cp_index(io->str.bytes, io->str.len, (size_t)i));
                        break;
                    }
                }
            }
        } else {
            return err_emit(c, "rindex: only string-in-string");
        }
        nuq_pool_push(c, r);
    }
    return nuq_emit_slice(c, outer);
}

EMIT
nuq_test_eval(CTX *c, struct Node *pat)
{
    size_t t0 = c->pool_top;
    EMIT buf = EVAL(c, pat);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (buf.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE p = buf.items[0];
    c->pool_top = t0;
    if (!(NUQ_IS_PTR(c->input) && NUQ_PTR(c->input)->type == NUQ_T_STRING) ||
        !(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_STRING))
        return err_emit(c, "test: only string-in-string");
    struct nuq_obj *io = NUQ_PTR(c->input);
    struct nuq_obj *po = NUQ_PTR(p);
    bool t = po->str.len == 0;
    if (!t) {
        for (size_t i = 0; i + po->str.len <= io->str.len; i++) {
            if (memcmp(io->str.bytes + i, po->str.bytes, po->str.len) == 0) { t = true; break; }
        }
    }
    return nuq_emit_one(c, t ? NUQ_TRUE : NUQ_FALSE);
}

/* Recursive setpath helper: returns a new value with `keys[start..]`
 * set to `new_val` inside `v`.  jq semantics: missing intermediate
 * keys auto-create objects (strings) or arrays (ints). */
static VALUE
setpath_recurse(VALUE v, VALUE *keys, size_t cnt, VALUE new_val)
{
    if (cnt == 0) return new_val;
    VALUE k = keys[0];
    bool key_is_str = NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_STRING;
    bool key_is_int = NUQ_IS_FIX(k);
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL) {
        if (key_is_int) {
            int64_t idx0 = NUQ_FIX_VAL(k);
            /* Auto-vivify with negative index is invalid in jq. */
            if (idx0 < 0) {
                if (nuq_active_ctx && nuq_active_ctx->error == NUQ_NULL)
                    nuq_active_ctx->error = nuq_make_string(
                        "Out of bounds negative array index", 34);
                return v;
            }
        }
        v = key_is_str ? nuq_make_object(4) : nuq_make_array(0);
    }
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
        if (!key_is_str) {
            char kd[80], msg[160];
            nuq_value_descr(k, kd, sizeof(kd));
            int w = snprintf(msg, sizeof(msg), "Cannot index object with %s", kd);
            if (nuq_active_ctx && nuq_active_ctx->error == NUQ_NULL)
                nuq_active_ctx->error = nuq_make_string(msg, (size_t)w);
            return v;
        }
        VALUE child = nuq_object_get(v, k);
        VALUE updated = setpath_recurse(child, keys + 1, cnt - 1, new_val);
        VALUE clone = nuq_clone(v);
        nuq_object_set(clone, k, updated);
        return clone;
    }
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
        if (!key_is_int) {
            if (nuq_active_ctx && nuq_active_ctx->error == NUQ_NULL)
                nuq_active_ctx->error = nuq_make_string(
                    "Cannot update field at array index of array", 43);
            return v;
        }
        int64_t idx = NUQ_FIX_VAL(k);
        struct nuq_obj *o = NUQ_PTR(v);
        if (idx < 0) idx += (int64_t)o->arr.len;
        if (idx < 0) return v;
        VALUE clone = nuq_clone(v);
        struct nuq_obj *co = NUQ_PTR(clone);
        while ((int64_t)co->arr.len <= idx) nuq_array_push(clone, NUQ_NULL);
        VALUE child = co->arr.items[idx];
        VALUE updated = setpath_recurse(child, keys + 1, cnt - 1, new_val);
        co->arr.items[idx] = updated;
        return clone;
    }
    return v;
}

/* `setpath(p; v)` — return input with value at path p replaced by v. */
EMIT
nuq_setpath_eval(CTX *c, struct Node *path, struct Node *value)
{
    size_t t0 = c->pool_top;
    EMIT pe = EVAL(c, path);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (pe.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE pv = pe.items[0];
    if (!(NUQ_IS_PTR(pv) && NUQ_PTR(pv)->type == NUQ_T_ARRAY))
        return err_emit(c, "setpath: path not array");
    /* Snapshot path keys before evaluating value (which grows pool). */
    struct nuq_obj *po = NUQ_PTR(pv);
    size_t kcnt = po->arr.len;
    if (kcnt > 10000) {
        c->pool_top = t0;
        return err_emit(c, "Path too deep");
    }
    VALUE small[16];
    VALUE *keys = (kcnt <= 16) ? small : (VALUE *)nuq_scratch_alloc(kcnt * sizeof(VALUE));
    memcpy(keys, po->arr.items, kcnt * sizeof(VALUE));
    c->pool_top = t0;
    EMIT ve = EVAL(c, value);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (ve.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE nv = ve.items[0];
    c->pool_top = t0;
    return nuq_emit_one(c, setpath_recurse(c->input, keys, kcnt, nv));
}

/* delpath_recurse: return v with `keys[start..]` deleted.
 * Slice-paths look like `{"start":i, "end":j}`. */
static bool
is_slice_path_key(VALUE k, int64_t *lo, int64_t *hi)
{
    if (!(NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_OBJECT)) return false;
    VALUE start_v = nuq_object_get_cstr(k, "start");
    VALUE end_v = nuq_object_get_cstr(k, "end");
    if (!NUQ_IS_FIX(start_v) || !NUQ_IS_FIX(end_v)) return false;
    *lo = NUQ_FIX_VAL(start_v);
    *hi = NUQ_FIX_VAL(end_v);
    return true;
}

static VALUE
delpath_recurse(VALUE v, VALUE *keys, size_t cnt)
{
    if (cnt == 0) return NUQ_NULL;     /* shouldn't reach: caller handles */
    VALUE k = keys[0];
    int64_t slo, shi;
    if (cnt == 1 && is_slice_path_key(k, &slo, &shi)) {
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
            struct nuq_obj *o = NUQ_PTR(v);
            int64_t total = (int64_t)o->arr.len;
            if (slo < 0) slo = 0;
            if (shi > total) shi = total;
            if (slo >= shi) return v;
            VALUE clone = nuq_make_array(total - (shi - slo));
            for (int64_t i = 0; i < slo; i++) nuq_array_push(clone, o->arr.items[i]);
            for (int64_t i = shi; i < total; i++) nuq_array_push(clone, o->arr.items[i]);
            return clone;
        }
        return v;
    }
    if (cnt == 1) {
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
            VALUE clone = nuq_clone(v);
            struct nuq_obj *co = NUQ_PTR(clone);
            for (size_t i = 0; i < co->obj.len; i++) {
                if (nuq_eq(co->obj.keys[i], k)) {
                    /* shift down */
                    for (size_t j = i; j + 1 < co->obj.len; j++) {
                        co->obj.keys[j] = co->obj.keys[j+1];
                        co->obj.vals[j] = co->obj.vals[j+1];
                    }
                    co->obj.len--;
                    co->obj.idx = NULL;     /* invalidate hash idx */
                    co->obj.idx_mask = 0;
                    break;
                }
            }
            return clone;
        }
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY && NUQ_IS_FIX(k)) {
            int64_t idx = NUQ_FIX_VAL(k);
            struct nuq_obj *o = NUQ_PTR(v);
            if (idx < 0) idx += (int64_t)o->arr.len;
            if (idx < 0 || (size_t)idx >= o->arr.len) return v;
            VALUE clone = nuq_clone(v);
            struct nuq_obj *co = NUQ_PTR(clone);
            for (size_t j = (size_t)idx; j + 1 < co->arr.len; j++)
                co->arr.items[j] = co->arr.items[j+1];
            co->arr.len--;
            return clone;
        }
        return v;
    }
    /* descend, splice updated child back.  If the key isn't present in
     * the container, jq's delpaths is a no-op for that path — we must
     * NOT auto-vivify like setpath does. */
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
        if (!nuq_object_has(v, k)) return v;
        VALUE child = nuq_object_get(v, k);
        VALUE updated = delpath_recurse(child, keys + 1, cnt - 1);
        VALUE clone = nuq_clone(v);
        nuq_object_set(clone, k, updated);
        return clone;
    }
    if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY && NUQ_IS_FIX(k)) {
        int64_t idx = NUQ_FIX_VAL(k);
        struct nuq_obj *o = NUQ_PTR(v);
        if (idx < 0) idx += (int64_t)o->arr.len;
        if (idx < 0 || (size_t)idx >= o->arr.len) return v;
        VALUE child = o->arr.items[idx];
        VALUE updated = delpath_recurse(child, keys + 1, cnt - 1);
        VALUE clone = nuq_clone(v);
        NUQ_PTR(clone)->arr.items[idx] = updated;
        return clone;
    }
    return v;
}

/* `delpaths(paths)` — delete each path in turn (jq sorts paths
 * descending to avoid index drift; we do too for arrays). */
EMIT
nuq_delpaths_eval(CTX *c, struct Node *paths_expr)
{
    size_t t0 = c->pool_top;
    EMIT pe = EVAL(c, paths_expr);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (pe.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE pv = pe.items[0];
    if (!(NUQ_IS_PTR(pv) && NUQ_PTR(pv)->type == NUQ_T_ARRAY)) {
        c->pool_top = t0;
        return err_emit(c, "Paths must be specified as an array");
    }
    struct nuq_obj *po = NUQ_PTR(pv);
    /* Snapshot since we'll re-use pool. */
    size_t cnt = po->arr.len;
    VALUE small[64];
    VALUE *paths = (cnt <= 64) ? small : (VALUE *)nuq_scratch_alloc(cnt * sizeof(VALUE));
    memcpy(paths, po->arr.items, cnt * sizeof(VALUE));
    c->pool_top = t0;
    /* Sort paths descending lex order so deeper / later paths are
     * removed first (avoids index shift in arrays). */
    for (size_t i = 1; i < cnt; i++) {
        VALUE x = paths[i];
        size_t j = i;
        while (j > 0 && nuq_cmp(paths[j-1], x) < 0) {
            paths[j] = paths[j-1];
            j--;
        }
        paths[j] = x;
    }
    VALUE cur = c->input;
    for (size_t i = 0; i < cnt; i++) {
        VALUE p = paths[i];
        if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_ARRAY)) continue;
        struct nuq_obj *pp = NUQ_PTR(p);
        if (pp->arr.len > 10000) return err_emit(c, "Path too deep");
        if (pp->arr.len == 0) { cur = NUQ_NULL; continue; }
        cur = delpath_recurse(cur, pp->arr.items, pp->arr.len);
    }
    return nuq_emit_one(c, cur);
}

/* Extract a path (= array of string/int keys) from a sub-AST that
 * looks like a chain of static accessors.  Returns true on success
 * and pushes path components into `path` (a nuq_array).  Returns
 * false if the AST contains a shape we don't know how to lift to
 * a path (e.g. a `.[expr]` whose expression isn't a literal int,
 * `.[]` iteration, or any non-accessor node). */
extern const struct NodeKind kind_node_identity;
extern const struct NodeKind kind_node_pipe;
extern const struct NodeKind kind_node_field;
extern const struct NodeKind kind_node_field_opt;
extern const struct NodeKind kind_node_index;
extern const struct NodeKind kind_node_index_opt;
extern const struct NodeKind kind_node_iter;
extern const struct NodeKind kind_node_iter_opt;
extern const struct NodeKind kind_node_int;
extern const struct NodeKind kind_node_str;
extern const struct NodeKind kind_node_call;
extern const struct NodeKind kind_node_slice;
extern const struct NodeKind kind_node_comma;
extern const struct NodeKind kind_node_as;
extern const struct NodeKind kind_node_b_select;
extern const struct NodeKind kind_node_b_getpath;
extern const struct NodeKind kind_node_b_first0;
extern const struct NodeKind kind_node_b_last0;
extern const struct NodeKind kind_node_empty;

/* ---------- path-mode walk (used by `=` `|=` `+=` ... and `del`) ---------
 *
 * Given a path expression (an AST chain of identity / field / index /
 * iter / pipe), walk the input tree once, applying `leaf_fn(v, ud)`
 * at each leaf path, and rebuild containers with the new values.
 * Each container along the path is cloned exactly once; for an
 * `.foo[]` pattern over an N-element array we do O(2 + N) container
 * allocs, not O(N * depth) — way better than enumerating paths and
 * calling setpath once per path.
 *
 * The leaf function may signal "drop this entry" by setting
 * `*dropped = true`; the parent then removes the key/index instead
 * of replacing.  Used by `del`.  Drop is otherwise silently
 * ignored at non-leaf positions.
 *
 * Errors: if a path step demands a structure that the input doesn't
 * have (e.g. `.foo` on a number), we set c->error and return v
 * unchanged.  `_opt` variants (`.foo?`, `.[]?`) treat type errors
 * as "skip this path" — they leave v unchanged silently. */

typedef VALUE (*nuq_leaf_fn)(VALUE v, void *ud, bool *dropped);

static VALUE walk_path(CTX *c, struct Node *n, VALUE v, nuq_leaf_fn fn, void *ud);

/* Helper: clone object and set/delete one key.  nuq_clone may GC, so
 * `k` and `new_v` need pinning across it. */
static VALUE
obj_with_key(VALUE obj, VALUE k, VALUE new_v, bool drop)
{
    NUQ_GC_PIN3(obj, k, new_v);
    VALUE clone = nuq_clone(obj);
    if (drop) {
        struct nuq_obj *co = NUQ_PTR(clone);
        for (size_t i = 0; i < co->obj.len; i++) {
            if (nuq_eq(co->obj.keys[i], k)) {
                for (size_t j = i; j + 1 < co->obj.len; j++) {
                    co->obj.keys[j] = co->obj.keys[j+1];
                    co->obj.vals[j] = co->obj.vals[j+1];
                }
                co->obj.len--;
                co->obj.idx = NULL;
                co->obj.idx_mask = 0;
                break;
            }
        }
    } else {
        nuq_object_set(clone, k, new_v);
    }
    NUQ_GC_UNPIN(3);
    return clone;
}

/* Helper: clone array and set/delete one index.  Auto-vivifies up to
 * `idx`; idx beyond a sanity limit (NUQ_PATH_MAX_AUTOVIV) errors via
 * `nuq_active_ctx->error` instead of trying to grow the array to
 * billions of slots. */
#define NUQ_PATH_MAX_AUTOVIV (1u << 24)   /* 16M slots — generous */
static VALUE
arr_with_idx(VALUE arr, int64_t idx, VALUE new_v, bool drop)
{
    struct nuq_obj *o = NUQ_PTR(arr);
    if (idx < 0) idx += (int64_t)o->arr.len;
    if (drop) {
        if (idx < 0 || (size_t)idx >= o->arr.len) return arr;
        VALUE clone = nuq_clone(arr);
        struct nuq_obj *co = NUQ_PTR(clone);
        for (size_t j = (size_t)idx; j + 1 < co->arr.len; j++)
            co->arr.items[j] = co->arr.items[j+1];
        co->arr.len--;
        return clone;
    }
    if (idx < 0) {
        if (nuq_active_ctx && nuq_active_ctx->error == NUQ_NULL)
            nuq_active_ctx->error = nuq_make_string(
                "Out of bounds negative array index", 34);
        return arr;
    }
    if ((size_t)idx > NUQ_PATH_MAX_AUTOVIV) {
        if (nuq_active_ctx && nuq_active_ctx->error == NUQ_NULL)
            nuq_active_ctx->error = nuq_make_string(
                "Array index too large", 21);
        return arr;
    }
    VALUE clone = nuq_clone(arr);
    struct nuq_obj *co = NUQ_PTR(clone);
    while ((int64_t)co->arr.len <= idx) nuq_array_push(clone, NUQ_NULL);
    co->arr.items[idx] = new_v;
    return clone;
}

/* Wrapper that bundles "next path step + next leaf fn + next ud" so
 * the pipe case can keep recursing through the chain. */
struct nested_ud {
    CTX        *c;
    struct Node *next;     /* path expression for the rest of the chain */
    nuq_leaf_fn  next_fn;  /* leaf fn applied at end of the rest */
    void       *next_ud;
};

static VALUE
nested_apply(VALUE v, void *ud, bool *dropped)
{
    struct nested_ud *nu = (struct nested_ud *)ud;
    nu->c->path_drop_pending = false;
    VALUE r = walk_path(nu->c, nu->next, v, nu->next_fn, nu->next_ud);
    if (nu->c->path_drop_pending) {
        *dropped = true;
        nu->c->path_drop_pending = false;
        return v;
    }
    return r;
}

extern const struct NodeKind kind_node_neg;
extern const struct NodeKind kind_node_var;

/* Evaluate a static-ish index expression to get the key VALUE.
 * Accepts `node_int`, `node_neg(node_int)` (so `.[-1]` works),
 * `node_str`, and `node_var($name)` (looked up in the active CTX —
 * works for `.[$k]` patterns where `$k` is bound by `as` or
 * `--arg`).  Returns true on success. */
static bool
eval_static_key(struct Node *idx, VALUE *out)
{
    if (idx->head.kind == &kind_node_int) {
        *out = nuq_make_int((int64_t)idx->u.node_int.v);
        return true;
    }
    if (idx->head.kind == &kind_node_neg) {
        struct Node *inner = idx->u.node_neg.expr;
        VALUE iv;
        if (!eval_static_key(inner, &iv)) return false;
        if (NUQ_IS_FIX(iv)) { *out = NUQ_FIX(-NUQ_FIX_VAL(iv)); return true; }
        return false;
    }
    if (idx->head.kind == &kind_node_str) {
        const char *s = idx->u.node_str.s;
        *out = nuq_make_string(s, idx->u.node_str.len);
        return true;
    }
    if (idx->head.kind == &kind_node_var) {
        if (!nuq_active_ctx) return false;
        *out = nuq_var_get(nuq_active_ctx, idx->u.node_var.var_id);
        return true;
    }
    return false;
}

/* Evaluate a path-expression's index sub-AST against the current
 * outer input (`c->input`).  Used as a fallback when `eval_static_key`
 * can't constant-fold (e.g. `.[$i|tostring]`, `.[range(3)]`).  Only the
 * first emit is taken — single-output path semantics.  Returns false
 * (and sets c->error) on multi-output / error.  Path mode keeps
 * c->input == outer for the duration of walk_path, so EVAL here sees
 * the same input the caller's filter sees. */
static bool
eval_dynamic_key(CTX *c, struct Node *idx, VALUE *out)
{
    if (eval_static_key(idx, out)) return true;
    size_t t0 = c->pool_top;
    EMIT e = EVAL(c, idx);
    if (c->error != NUQ_NULL) { c->pool_top = t0; return false; }
    if (e.count != 1) {
        c->pool_top = t0;
        c->error = nuq_make_string("path expression: index must produce exactly one value", 53);
        return false;
    }
    *out = e.items[0];
    c->pool_top = t0;
    return true;
}

static VALUE
walk_path(CTX *c, struct Node *n, VALUE v, nuq_leaf_fn fn, void *ud)
{
    /* identity / `.` — leaf */
    if (n->head.kind == &kind_node_identity) {
        bool dropped = false;
        return fn(v, ud, &dropped);
    }

    /* 0-arity user-def call — inline the def body so e.g. `del(f)`
     * inside `def x(f): ...` traverses f's static path. */
    if (n->head.kind == &kind_node_call && n->u.node_call.arity == 0) {
        struct nuq_func_def *fd = nuq_func_lookup(c, n->u.node_call.name_id, 0);
        if (fd && fd->body) return walk_path(c, fd->body, v, fn, ud);
    }

    /* `getpath([keys...])` as a path component — descend through each
     * key with auto-vivify; apply fn at the bottom; rebuild containers. */
    if (n->head.kind == &kind_node_b_getpath) {
        size_t t0 = c->pool_top;
        EMIT pe = EVAL(c, n->u.node_b_getpath.path);
        if (c->error != NUQ_NULL) return v;
        if (pe.count == 0) { c->pool_top = t0; return v; }
        VALUE pv = pe.items[0];
        if (!(NUQ_IS_PTR(pv) && NUQ_PTR(pv)->type == NUQ_T_ARRAY)) {
            c->pool_top = t0;
            c->error = nuq_make_string("path: getpath argument is not array", 35);
            return v;
        }
        struct nuq_obj *po = NUQ_PTR(pv);
        size_t klen = po->arr.len;
        VALUE small[16];
        VALUE *keys = (klen <= 16) ? small : (VALUE *)nuq_scratch_alloc(klen * sizeof(VALUE));
        memcpy(keys, po->arr.items, klen * sizeof(VALUE));
        c->pool_top = t0;
        /* Walk down recursively; at the leaf apply fn. */
        VALUE *trail_small[16]; (void)trail_small;
        VALUE cur = v;
        VALUE trail[16];
        if (klen > 16) {
            c->error = nuq_make_string("path too deep", 13);
            return v;
        }
        trail[0] = cur;
        /* Descend, recording values at each level. */
        for (size_t i = 0; i < klen; i++) {
            VALUE k = keys[i];
            VALUE child = NUQ_NULL;
            if (NUQ_IS_PTR(cur) && NUQ_PTR(cur)->type == NUQ_T_OBJECT) {
                if (!(NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_STRING)) {
                    char kd[80], msg[160];
                    nuq_value_descr(k, kd, sizeof(kd));
                    int w = snprintf(msg, sizeof(msg), "Cannot index object with %s", kd);
                    c->error = nuq_make_string(msg, (size_t)w);
                    return v;
                }
                child = nuq_object_get(cur, k);
            } else if (NUQ_IS_PTR(cur) && NUQ_PTR(cur)->type == NUQ_T_ARRAY) {
                if (!NUQ_IS_FIX(k)) {
                    char kd[80], msg[160];
                    nuq_value_descr(k, kd, sizeof(kd));
                    int w = snprintf(msg, sizeof(msg), "Cannot index array with %s", kd);
                    c->error = nuq_make_string(msg, (size_t)w);
                    return v;
                }
                int64_t idx = NUQ_FIX_VAL(k);
                struct nuq_obj *o = NUQ_PTR(cur);
                int64_t real_i = idx < 0 ? idx + (int64_t)o->arr.len : idx;
                child = (real_i >= 0 && (size_t)real_i < o->arr.len)
                        ? o->arr.items[real_i] : NUQ_NULL;
            } else if (NUQ_IS_PTR(cur) && NUQ_PTR(cur)->type == NUQ_T_NULL) {
                /* auto-vivify on null along the path */
                child = NUQ_NULL;
            } else {
                /* Cannot descend through scalar — jq error. */
                char td[80], kd[80], msg[200];
                nuq_value_descr(cur, td, sizeof(td));
                nuq_value_descr(k, kd, sizeof(kd));
                int w = snprintf(msg, sizeof(msg), "Cannot index %s with %s", nuq_type_name(cur), kd);
                (void)td;
                c->error = nuq_make_string(msg, (size_t)w);
                return v;
            }
            trail[i + 1] = child;
            cur = child;
        }
        /* Apply fn at the leaf. */
        bool dropped = false;
        VALUE new_leaf = fn(cur, ud, &dropped);
        if (c->error != NUQ_NULL) return v;
        /* Rebuild from leaf to root using setpath_recurse semantics. */
        VALUE built = new_leaf;
        for (ssize_t i = (ssize_t)klen - 1; i >= 0; i--) {
            VALUE container = trail[i];
            VALUE k = keys[i];
            if (NUQ_IS_PTR(container) && NUQ_PTR(container)->type == NUQ_T_NULL) {
                container = NUQ_IS_FIX(k) ? nuq_make_array(0) : nuq_make_object(4);
            }
            if (NUQ_IS_PTR(container) && NUQ_PTR(container)->type == NUQ_T_OBJECT) {
                VALUE clone = nuq_clone(container);
                if (dropped && i + 1 == (ssize_t)klen) {
                    obj_with_key(clone, k, NUQ_NULL, true);
                    /* obj_with_key returns a new clone; unfortunate. */
                } else {
                    nuq_object_set(clone, k, built);
                }
                built = clone;
            } else if (NUQ_IS_PTR(container) && NUQ_PTR(container)->type == NUQ_T_ARRAY) {
                int64_t idx = NUQ_FIX_VAL(k);
                struct nuq_obj *o = NUQ_PTR(container);
                int64_t real_i = idx < 0 ? idx + (int64_t)o->arr.len : idx;
                if (real_i < 0) { c->error = nuq_make_string("Out of bounds negative array index", 34); return v; }
                VALUE clone = nuq_clone(container);
                struct nuq_obj *co = NUQ_PTR(clone);
                while ((int64_t)co->arr.len <= real_i) nuq_array_push(clone, NUQ_NULL);
                co->arr.items[real_i] = built;
                built = clone;
            } else {
                /* Scalar — should have errored earlier; fall through. */
                return v;
            }
        }
        return built;
    }
    /* `..` (recurse) as a path component — apply fn at each visited
     * sub-tree; rebuild bottom-up so `(.. | select(P) | .b) |= F`
     * mutates every matching nested .b in one pass. */
    {
        extern const struct NodeKind kind_node_recurse;
        if (n->head.kind == &kind_node_recurse) {
            VALUE updated = v;
            if (NUQ_IS_PTR(v)) {
                struct nuq_obj *o = NUQ_PTR(v);
                if (o->type == NUQ_T_ARRAY) {
                    VALUE arr = nuq_make_array(o->arr.len);
                    for (size_t i = 0; i < o->arr.len; i++) {
                        VALUE child = walk_path(c, n, o->arr.items[i], fn, ud);
                        if (c->error != NUQ_NULL) return v;
                        nuq_array_push(arr, child);
                    }
                    updated = arr;
                } else if (o->type == NUQ_T_OBJECT) {
                    VALUE obj = nuq_make_object(o->obj.len > 4 ? o->obj.len : 4);
                    for (size_t i = 0; i < o->obj.len; i++) {
                        VALUE child = walk_path(c, n, o->obj.vals[i], fn, ud);
                        if (c->error != NUQ_NULL) return v;
                        nuq_object_set(obj, o->obj.keys[i], child);
                    }
                    updated = obj;
                }
            }
            bool dropped = false;
            VALUE r = fn(updated, ud, &dropped);
            if (c->error != NUQ_NULL) return v;
            if (dropped) return updated;
            return r;
        }
    }

    /* `select(cond)` as a path component — when cond is truthy apply
     * the leaf normally (propagating any drop from `|= empty` etc.);
     * when cond is falsy the path doesn't apply here, leave v alone. */
    if (n->head.kind == &kind_node_b_select) {
        VALUE saved = c->input;
        c->input = v;
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, n->u.node_b_select.body);
        c->input = saved;
        if (c->error != NUQ_NULL) return v;
        bool any = false;
        for (uint32_t i = 0; i < bo.count; i++)
            if (nuq_truthy(bo.items[i])) { any = true; break; }
        c->pool_top = t0;
        if (!any) return v;     /* select rejected — keep v unchanged */
        bool dropped = false;
        VALUE r = fn(v, ud, &dropped);
        if (dropped) c->path_drop_pending = true;
        return r;
    }
    /* `as(src, var, body)` — when used as a path component (which the
     * parser inserts to capture outer-`.` for index/slice args), the
     * src is `.` and we recurse into body with the var bound. */
    if (n->head.kind == &kind_node_as) {
        size_t v_top = c->var_top;
        nuq_var_push(c, n->u.node_as.var_id, v);
        VALUE r = walk_path(c, n->u.node_as.body, v, fn, ud);
        nuq_var_pop(c, v_top);
        return r;
    }

    /* pipe(a, b) — recurse into a with "do b then fn" as the leaf */
    if (n->head.kind == &kind_node_pipe) {
        struct nested_ud nu = { c, n->u.node_pipe.rhs, fn, ud };
        return walk_path(c, n->u.node_pipe.lhs, v, nested_apply, &nu);
    }

    /* field(name) — descend into v.<name> */
    if (n->head.kind == &kind_node_field || n->head.kind == &kind_node_field_opt) {
        bool optional = (n->head.kind == &kind_node_field_opt);
        const char *name = (n->head.kind == &kind_node_field)
            ? n->u.node_field.name : n->u.node_field_opt.name;
        VALUE k = nuq_make_string(name, strlen(name));
        /* fn() invokes user filter — many allocs, GC may fire. v and k
         * live across the call; pin them. */
        NUQ_GC_PIN2(v, k);
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
            VALUE child = nuq_object_get(v, k);
            bool dropped = false;
            VALUE new_child = fn(child, ud, &dropped);
            if (c->error != NUQ_NULL) { NUQ_GC_UNPIN(2); return v; }
            NUQ_GC_PIN1(new_child);
            VALUE r = obj_with_key(v, k, new_child, dropped);
            NUQ_GC_UNPIN(3);
            return r;
        }
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL) {
            /* auto-vivify: treat null as empty object */
            VALUE empty_obj = nuq_make_object(4);
            NUQ_GC_PIN1(empty_obj);
            bool dropped = false;
            VALUE new_child = fn(NUQ_NULL, ud, &dropped);
            if (c->error != NUQ_NULL) { NUQ_GC_UNPIN(3); return v; }
            if (dropped) { NUQ_GC_UNPIN(3); return v; }
            NUQ_GC_PIN1(new_child);
            nuq_object_set(empty_obj, k, new_child);
            NUQ_GC_UNPIN(4);
            return empty_obj;
        }
        NUQ_GC_UNPIN(2);
        if (optional) return v;
        c->error = nuq_make_string("path expression: not object", 27);
        return v;
    }

    /* index(expr) — `.[N]` or `.[<expr>]` (we only support static int / str) */
    if (n->head.kind == &kind_node_index || n->head.kind == &kind_node_index_opt) {
        bool optional = (n->head.kind == &kind_node_index_opt);
        struct Node *idx_expr = (n->head.kind == &kind_node_index)
            ? n->u.node_index.expr : n->u.node_index_opt.expr;
        VALUE k;
        if (!eval_dynamic_key(c, idx_expr, &k)) {
            return v;     /* error already set */
        }
        /* `.[nan] = X` — jq error message. */
        if (NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_DOUBLE && isnan(NUQ_PTR(k)->dbl)
            && NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
            c->error = nuq_make_string("Cannot set array element at NaN index", 37);
            return v;
        }
        /* Float-valued indices: jq accepts ints (treats float-as-int via floor). */
        if (NUQ_IS_PTR(k) && NUQ_PTR(k)->type == NUQ_T_DOUBLE) {
            double d = NUQ_PTR(k)->dbl;
            if (!isnan(d)) k = nuq_make_int((int64_t)floor(d));
        }
        /* Reject non-string/int indices in path mode (e.g. `.[{}] = 0`). */
        if (!NUQ_IS_FIX(k) &&
            !(NUQ_IS_PTR(k) && (NUQ_PTR(k)->type == NUQ_T_STRING ||
                                NUQ_PTR(k)->type == NUQ_T_DOUBLE))) {
            if (optional) return v;
            const char *t = NUQ_IS_PTR(k) ? nuq_type_name(k) : "number";
            char buf[80];
            snprintf(buf, sizeof(buf),
                     "%s is not a valid path expression", t);
            c->error = nuq_make_string(buf, strlen(buf));
            return v;
        }
        if (NUQ_IS_FIX(k)) {
            /* int index — array */
            int64_t i = NUQ_FIX_VAL(k);
            if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
                struct nuq_obj *o = NUQ_PTR(v);
                int64_t real_i = i < 0 ? i + (int64_t)o->arr.len : i;
                VALUE child = (real_i >= 0 && (size_t)real_i < o->arr.len)
                              ? o->arr.items[real_i] : NUQ_NULL;
                bool dropped = false;
                VALUE new_child = fn(child, ud, &dropped);
                if (c->error != NUQ_NULL) return v;
                return arr_with_idx(v, i, new_child, dropped);
            }
            if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL) {
                bool dropped = false;
                VALUE new_child = fn(NUQ_NULL, ud, &dropped);
                if (c->error != NUQ_NULL) return v;
                if (dropped) return v;
                VALUE arr = nuq_make_array(0);
                return arr_with_idx(arr, i, new_child, false);
            }
            if (optional) return v;
            c->error = nuq_make_string("path expression: not array", 26);
            return v;
        }
        /* string index → object */
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
            VALUE child = nuq_object_get(v, k);
            bool dropped = false;
            VALUE new_child = fn(child, ud, &dropped);
            if (c->error != NUQ_NULL) return v;
            return obj_with_key(v, k, new_child, dropped);
        }
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_NULL) {
            bool dropped = false;
            VALUE new_child = fn(NUQ_NULL, ud, &dropped);
            if (c->error != NUQ_NULL) return v;
            if (dropped) return v;
            VALUE empty = nuq_make_object(4);
            nuq_object_set(empty, k, new_child);
            return empty;
        }
        if (optional) return v;
        c->error = nuq_make_string("path expression: not object", 27);
        return v;
    }

    /* iter (`.[]`) — for each element, apply fn; rebuild container.
     * fn() runs arbitrary user filter and may GC.  Pin v and result
     * across the loop; refetch element pointers from v each iter. */
    if (n->head.kind == &kind_node_iter || n->head.kind == &kind_node_iter_opt) {
        bool optional = (n->head.kind == &kind_node_iter_opt);
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
            size_t len = NUQ_PTR(v)->arr.len;
            VALUE result = nuq_make_array(len);
            NUQ_GC_PIN2(v, result);
            for (size_t i = 0; i < len; i++) {
                bool dropped = false;
                VALUE item = NUQ_PTR(v)->arr.items[i];   /* refetch */
                VALUE new_v = fn(item, ud, &dropped);
                if (c->error != NUQ_NULL) { NUQ_GC_UNPIN(2); return v; }
                if (!dropped) nuq_array_push(result, new_v);
                /* dropped → just skip pushing, effectively removing */
            }
            NUQ_GC_UNPIN(2);
            return result;
        }
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
            size_t len = NUQ_PTR(v)->obj.len;
            VALUE result = nuq_make_object(len);
            NUQ_GC_PIN2(v, result);
            for (size_t i = 0; i < len; i++) {
                bool dropped = false;
                VALUE val = NUQ_PTR(v)->obj.vals[i];     /* refetch */
                VALUE key = NUQ_PTR(v)->obj.keys[i];     /* refetch */
                VALUE new_v = fn(val, ud, &dropped);
                if (c->error != NUQ_NULL) { NUQ_GC_UNPIN(2); return v; }
                /* GC inside fn may have moved key — refetch from v. */
                if (!dropped) {
                    key = NUQ_PTR(v)->obj.keys[i];
                    nuq_object_set(result, key, new_v);
                }
            }
            NUQ_GC_UNPIN(2);
            return result;
        }
        if (optional) return v;
        c->error = nuq_make_string("path expression: cannot iterate", 31);
        return v;
    }

    /* slice (`.[i:j]`) — apply fn to the sliced sub-array, splice back. */
    if (n->head.kind == &kind_node_slice) {
        struct Node *startn = n->u.node_slice.startn;
        struct Node *stopn  = n->u.node_slice.stopn;
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING) {
            c->error = nuq_make_string("Cannot update string slices", 27);
            return v;
        }
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY)) {
            c->error = nuq_make_string("path expression: not array (slice)", 34);
            return v;
        }
        struct nuq_obj *o = NUQ_PTR(v);
        int64_t total = (int64_t)o->arr.len;
        int64_t lo = 0, hi = total;
        if (startn) {
            VALUE sv;
            if (!eval_dynamic_key(c, startn, &sv)) return v;
            if (NUQ_IS_FIX(sv)) lo = NUQ_FIX_VAL(sv);
            else if (NUQ_IS_PTR(sv) && NUQ_PTR(sv)->type == NUQ_T_DOUBLE) {
                double d = NUQ_PTR(sv)->dbl;
                lo = isnan(d) ? 0 : (int64_t)floor(d);
            } else { c->error = nuq_make_string("slice start not int", 19); return v; }
        }
        if (stopn) {
            VALUE sv;
            if (!eval_dynamic_key(c, stopn, &sv)) return v;
            if (NUQ_IS_FIX(sv)) hi = NUQ_FIX_VAL(sv);
            else if (NUQ_IS_PTR(sv) && NUQ_PTR(sv)->type == NUQ_T_DOUBLE) {
                double d = NUQ_PTR(sv)->dbl;
                hi = isnan(d) ? total : (int64_t)ceil(d);
            } else { c->error = nuq_make_string("slice stop not int", 18); return v; }
        }
        if (lo < 0) lo += total;
        if (hi < 0) hi += total;
        if (lo < 0) lo = 0;
        if (hi > total) hi = total;
        if (lo > hi) lo = hi;
        VALUE sub = nuq_make_array(hi - lo);
        for (int64_t i = lo; i < hi; i++) nuq_array_push(sub, o->arr.items[i]);
        bool dropped = false;
        VALUE new_sub = fn(sub, ud, &dropped);
        if (c->error != NUQ_NULL) return v;
        VALUE clone = nuq_make_array(total);
        for (int64_t i = 0; i < lo; i++) nuq_array_push(clone, o->arr.items[i]);
        if (!dropped) {
            if (NUQ_IS_PTR(new_sub) && NUQ_PTR(new_sub)->type == NUQ_T_ARRAY) {
                struct nuq_obj *no = NUQ_PTR(new_sub);
                for (size_t i = 0; i < no->arr.len; i++)
                    nuq_array_push(clone, no->arr.items[i]);
            } else {
                /* non-array replacement: append as single value (rare). */
                nuq_array_push(clone, new_sub);
            }
        }
        for (int64_t i = hi; i < total; i++) nuq_array_push(clone, o->arr.items[i]);
        return clone;
    }

    /* Hit a non-accessor in the path chain (e.g. `map(...)` mid-path).
     * Format jq's "Invalid path expression" diagnostic with the current
     * value and, if available, the next step in the chain.  jq reports
     * the value AFTER the unsupported node ran, so evaluate it once. */
    VALUE result_v = v;
    {
        VALUE saved_in = c->input;
        c->input = v;
        size_t t0 = c->pool_top;
        EMIT pe = EVAL(c, n);
        c->input = saved_in;
        if (c->error == NUQ_NULL && pe.count > 0) result_v = pe.items[0];
        c->pool_top = t0;
        c->error = NUQ_NULL;
    }
    {
        char *json_buf = NULL; size_t jl = 0;
        FILE *jfp = open_memstream(&json_buf, &jl);
        nuq_json_print(jfp, result_v, 0);
        fclose(jfp);
        char vd[80], msg[256];
        size_t lim = sizeof(vd) - 1;
        size_t copy = jl <= lim ? jl : lim;
        memcpy(vd, json_buf, copy); vd[copy] = 0;
        free(json_buf);
        struct Node *nxt = NULL;
        if (fn == nested_apply) {
            nxt = ((struct nested_ud *)ud)->next;
            /* Skip past pipes / `as` wrappers to the first concrete step. */
            while (nxt) {
                if (nxt->head.kind == &kind_node_pipe) { nxt = nxt->u.node_pipe.lhs; continue; }
                if (nxt->head.kind == &kind_node_as) { nxt = nxt->u.node_as.body; continue; }
                if (nxt->head.kind == &kind_node_identity) { nxt = NULL; break; }
                break;
            }
        }
        if (!nxt) {
            int w = snprintf(msg, sizeof(msg),
                             "Invalid path expression with result %s", vd);
            c->error = nuq_make_string(msg, (size_t)w);
        } else if (nxt->head.kind == &kind_node_field || nxt->head.kind == &kind_node_field_opt) {
            const char *name = (nxt->head.kind == &kind_node_field)
                ? nxt->u.node_field.name : nxt->u.node_field_opt.name;
            int w = snprintf(msg, sizeof(msg),
                             "Invalid path expression near attempt to access element \"%s\" of %s", name, vd);
            c->error = nuq_make_string(msg, (size_t)w);
        } else if (nxt->head.kind == &kind_node_index || nxt->head.kind == &kind_node_index_opt) {
            struct Node *ix = (nxt->head.kind == &kind_node_index)
                ? nxt->u.node_index.expr : nxt->u.node_index_opt.expr;
            VALUE kv;
            char kd[40] = "?";
            if (eval_static_key(ix, &kv)) {
                if (NUQ_IS_FIX(kv)) snprintf(kd, sizeof(kd), "%lld", (long long)NUQ_FIX_VAL(kv));
                else if (NUQ_IS_PTR(kv) && NUQ_PTR(kv)->type == NUQ_T_STRING) {
                    struct nuq_obj *so = NUQ_PTR(kv);
                    snprintf(kd, sizeof(kd), "\"%.*s\"", (int)so->str.len, so->str.bytes);
                }
            }
            int w = snprintf(msg, sizeof(msg),
                             "Invalid path expression near attempt to access element %s of %s", kd, vd);
            c->error = nuq_make_string(msg, (size_t)w);
        } else if (nxt->head.kind == &kind_node_iter || nxt->head.kind == &kind_node_iter_opt) {
            int w = snprintf(msg, sizeof(msg),
                             "Invalid path expression near attempt to iterate through %s", vd);
            c->error = nuq_make_string(msg, (size_t)w);
        } else {
            int w = snprintf(msg, sizeof(msg),
                             "Invalid path expression with result %s", vd);
            c->error = nuq_make_string(msg, (size_t)w);
        }
    }
    return v;
}

/* `path(f)` — emit each path that `f` would visit on the input.
 * Supports linear accessor chains: identity / pipe / field /
 * field_opt / index / index_opt / iter / iter_opt.
 *
 * Strategy: linearise the chain (left-leaning or right-leaning pipe
 * tree → flat array of "step" nodes).  Then DFS-walk: at step k,
 * push the corresponding key onto cur; if k is the last step, emit
 * cur as a path; else recurse into the sub-value for step k+1. */

struct path_ud {
    CTX     *c;
    VALUE   *cur;
    size_t   cur_len;
    size_t   cur_capa;
};

static void
path_push(struct path_ud *pu, VALUE k)
{
    if (pu->cur_len == pu->cur_capa) {
        pu->cur_capa = pu->cur_capa ? pu->cur_capa * 2 : 4;
        pu->cur = (VALUE *)GC_realloc(pu->cur, pu->cur_capa * sizeof(VALUE));
    }
    pu->cur[pu->cur_len++] = k;
}

static void
path_emit(struct path_ud *pu)
{
    VALUE arr = nuq_make_array(pu->cur_len);
    for (size_t i = 0; i < pu->cur_len; i++) nuq_array_push(arr, pu->cur[i]);
    nuq_pool_push(pu->c, arr);
}

/* Flatten a possibly-pipe-nested AST into a linear array of steps. */
static void
path_flatten(struct Node *n, struct Node ***steps, size_t *cnt, size_t *capa)
{
    if (n->head.kind == &kind_node_pipe) {
        path_flatten(n->u.node_pipe.lhs, steps, cnt, capa);
        path_flatten(n->u.node_pipe.rhs, steps, cnt, capa);
        return;
    }
    if (n->head.kind == &kind_node_identity) return;   /* identity: no-op */
    /* The parser wraps `acc[args]` with `. as $V | acc | ...` so args
     * see outer `.`.  In path-mode we treat the whole thing as a step
     * — emit it so path_dfs can bind $V before descending. */
    if (n->head.kind == &kind_node_as) {
        if (*cnt == *capa) {
            *capa = *capa ? *capa * 2 : 8;
            *steps = (struct Node **)GC_realloc(*steps, *capa * sizeof(**steps));
        }
        (*steps)[(*cnt)++] = n;
        return;
    }
    /* User-def call to a 0-arity expr-param (e.g. `path(f)` inside a
     * `def pick(f): ...` where `f` is `.a.b.c`): inline the def's body
     * so static-path traversal sees the actual accessors. */
    if (n->head.kind == &kind_node_call && n->u.node_call.arity == 0) {
        struct nuq_func_def *fd = nuq_func_lookup(nuq_active_ctx,
                                                  n->u.node_call.name_id, 0);
        if (fd && fd->body) {
            path_flatten(fd->body, steps, cnt, capa);
            return;
        }
    }
    if (*cnt == *capa) {
        *capa = *capa ? *capa * 2 : 8;
        *steps = (struct Node **)GC_realloc(*steps, *capa * sizeof(**steps));
    }
    (*steps)[(*cnt)++] = n;
}

static void
path_dfs(CTX *c, struct Node **steps, size_t step_cnt, size_t k,
         VALUE v, struct path_ud *pu)
{
    if (k == step_cnt) {
        path_emit(pu);
        return;
    }
    struct Node *step = steps[k];
    if (step->head.kind == &kind_node_field || step->head.kind == &kind_node_field_opt) {
        const char *name = (step->head.kind == &kind_node_field)
            ? step->u.node_field.name : step->u.node_field_opt.name;
        VALUE k_str = nuq_make_string(name, strlen(name));
        VALUE child = NUQ_NULL;
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT)
            child = nuq_object_get(v, k_str);
        path_push(pu, k_str);
        path_dfs(c, steps, step_cnt, k + 1, child, pu);
        pu->cur_len--;
        return;
    }
    if (step->head.kind == &kind_node_index || step->head.kind == &kind_node_index_opt) {
        struct Node *idx_expr = (step->head.kind == &kind_node_index)
            ? step->u.node_index.expr : step->u.node_index_opt.expr;
        /* Evaluate the index expression — it may emit multiple values
         * (e.g. `path(.foo[0,1])`).  Each emission spawns a separate
         * path branch. */
        size_t t0 = c->pool_top;
        EMIT ie = EVAL(c, idx_expr);
        if (c->error != NUQ_NULL) return;
        uint32_t kc = ie.count;
        VALUE small[16];
        VALUE *kvs = (kc <= 16) ? small : (VALUE *)nuq_scratch_alloc(kc * sizeof(VALUE));
        memcpy(kvs, ie.items, kc * sizeof(VALUE));
        c->pool_top = t0;
        for (uint32_t i = 0; i < kc; i++) {
            VALUE kv = kvs[i];
            VALUE child = NUQ_NULL;
            VALUE path_kv = kv;
            if (NUQ_IS_FIX(kv) && NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
                int64_t idx = NUQ_FIX_VAL(kv);
                child = nuq_array_get(v, idx);
                if (idx < 0) {
                    int64_t abs_idx = idx + (int64_t)NUQ_PTR(v)->arr.len;
                    if (abs_idx >= 0) path_kv = nuq_make_int(abs_idx);
                }
            } else if (NUQ_IS_PTR(kv) && NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT)
                child = nuq_object_get(v, kv);
            path_push(pu, path_kv);
            path_dfs(c, steps, step_cnt, k + 1, child, pu);
            pu->cur_len--;
            if (c->error != NUQ_NULL) return;
        }
        return;
    }
    if (step->head.kind == &kind_node_iter || step->head.kind == &kind_node_iter_opt) {
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
            struct nuq_obj *o = NUQ_PTR(v);
            for (size_t i = 0; i < o->arr.len; i++) {
                path_push(pu, nuq_make_int((int64_t)i));
                path_dfs(c, steps, step_cnt, k + 1, o->arr.items[i], pu);
                pu->cur_len--;
                if (c->error != NUQ_NULL) return;
            }
        } else if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) {
            struct nuq_obj *o = NUQ_PTR(v);
            for (size_t i = 0; i < o->obj.len; i++) {
                path_push(pu, o->obj.keys[i]);
                path_dfs(c, steps, step_cnt, k + 1, o->obj.vals[i], pu);
                pu->cur_len--;
                if (c->error != NUQ_NULL) return;
            }
        }
        return;
    }
    if (step->head.kind == &kind_node_slice) {
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY)) return;
        struct nuq_obj *o = NUQ_PTR(v);
        int64_t total = (int64_t)o->arr.len;
        int64_t lo = 0, hi = total;
        if (step->u.node_slice.startn) {
            VALUE sv;
            if (!eval_dynamic_key(c, step->u.node_slice.startn, &sv)) return;
            if (NUQ_IS_FIX(sv)) lo = NUQ_FIX_VAL(sv);
        }
        if (step->u.node_slice.stopn) {
            VALUE sv;
            if (!eval_dynamic_key(c, step->u.node_slice.stopn, &sv)) return;
            if (NUQ_IS_FIX(sv)) hi = NUQ_FIX_VAL(sv);
        }
        if (lo < 0) lo += total;
        if (hi < 0) hi += total;
        if (lo < 0) lo = 0;
        if (hi > total) hi = total;
        if (lo > hi) lo = hi;
        /* Emit a single slice-path object: {"start":lo,"end":hi}.
         * jq represents slices in path output exactly this way. */
        VALUE obj = nuq_make_object(2);
        nuq_object_set(obj, nuq_make_string("start", 5), nuq_make_int(lo));
        nuq_object_set(obj, nuq_make_string("end", 3), nuq_make_int(hi));
        path_push(pu, obj);
        if (k + 1 == step_cnt) path_emit(pu);
        pu->cur_len--;
        return;
    }
    /* `first` / `last` as path components — equivalent to `.[0]` / `.[-1]`.
     * jq emits the literal int (0 for first, -1 for last) so a later
     * setpath of -1 properly errors. */
    if (step->head.kind == &kind_node_b_first0 ||
        step->head.kind == &kind_node_b_last0) {
        bool is_first = (step->head.kind == &kind_node_b_first0);
        int64_t lit_idx = is_first ? 0 : -1;
        if (!(NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY)) {
            /* Apply `last` on null/non-array yields error in jq. */
            c->error = nuq_make_string("Out of bounds negative array index", 34);
            return;
        }
        struct nuq_obj *o = NUQ_PTR(v);
        if (!is_first && o->arr.len == 0) {
            c->error = nuq_make_string("Out of bounds negative array index", 34);
            return;
        }
        size_t real_idx = is_first ? 0 : o->arr.len - 1;
        VALUE child = o->arr.items[real_idx];
        path_push(pu, nuq_make_int(lit_idx));
        path_dfs(c, steps, step_cnt, k + 1, child, pu);
        pu->cur_len--;
        return;
    }
    /* `select(cond)` as a path component — keep the path if cond is
     * truthy on the current value; otherwise drop. */
    if (step->head.kind == &kind_node_b_select) {
        VALUE saved = c->input;
        c->input = v;
        size_t t0 = c->pool_top;
        EMIT bo = EVAL(c, step->u.node_b_select.body);
        c->input = saved;
        if (c->error != NUQ_NULL) return;
        bool any = false;
        for (uint32_t i = 0; i < bo.count; i++)
            if (nuq_truthy(bo.items[i])) { any = true; break; }
        c->pool_top = t0;
        if (!any) return;     /* select dropped this path */
        path_dfs(c, steps, step_cnt, k + 1, v, pu);
        return;
    }
    /* `empty` step — emit no paths. */
    if (step->head.kind == &kind_node_empty) {
        return;
    }
    /* `comma(a, b)` step — fork: explore each branch as a separate
     * path, sharing the surrounding steps before/after. */
    if (step->head.kind == &kind_node_comma) {
        struct Node *branches[2] = { step->u.node_comma.lhs, step->u.node_comma.rhs };
        for (int br = 0; br < 2; br++) {
            struct Node **inner_steps = NULL; size_t inner_cnt = 0, inner_capa = 0;
            path_flatten(branches[br], &inner_steps, &inner_cnt, &inner_capa);
            struct Node *combined_small[64]; struct Node **combined;
            size_t combined_cnt = step_cnt + inner_cnt - 1;
            combined = (combined_cnt <= 64) ? combined_small
                                            : (struct Node **)nuq_scratch_alloc(combined_cnt * sizeof(struct Node *));
            for (size_t i = 0; i < k; i++) combined[i] = steps[i];
            for (size_t i = 0; i < inner_cnt; i++) combined[k + i] = inner_steps[i];
            for (size_t i = k + 1; i < step_cnt; i++) combined[i + inner_cnt - 1] = steps[i];
            path_dfs(c, combined, combined_cnt, k, v, pu);
            if (c->error != NUQ_NULL) return;
        }
        return;
    }
    /* `as(., $V, body)` step — bind $V to current value, descend into
     * body's flattened steps. */
    if (step->head.kind == &kind_node_as) {
        size_t v_top = c->var_top;
        nuq_var_push(c, step->u.node_as.var_id, v);
        struct Node **inner_steps = NULL; size_t inner_cnt = 0, inner_capa = 0;
        path_flatten(step->u.node_as.body, &inner_steps, &inner_cnt, &inner_capa);
        struct Node *combined_small[64]; struct Node **combined;
        size_t combined_cnt = step_cnt + inner_cnt - 1;
        combined = (combined_cnt <= 64) ? combined_small
                                        : (struct Node **)nuq_scratch_alloc(combined_cnt * sizeof(struct Node *));
        for (size_t i = 0; i < k; i++) combined[i] = steps[i];
        for (size_t i = 0; i < inner_cnt; i++) combined[k + i] = inner_steps[i];
        for (size_t i = k + 1; i < step_cnt; i++) combined[i + inner_cnt - 1] = steps[i];
        path_dfs(c, combined, combined_cnt, k, v, pu);
        nuq_var_pop(c, v_top);
        return;
    }
    /* 0-arity user-def call — inline. */
    if (step->head.kind == &kind_node_call && step->u.node_call.arity == 0) {
        struct nuq_func_def *fd = nuq_func_lookup(c, step->u.node_call.name_id, 0);
        if (fd && fd->body) {
            struct Node **inner_steps = NULL; size_t inner_cnt = 0, inner_capa = 0;
            path_flatten(fd->body, &inner_steps, &inner_cnt, &inner_capa);
            /* Splice inner_steps in place of this step. */
            struct Node *combined_small[64]; struct Node **combined;
            size_t combined_cnt = step_cnt + inner_cnt - 1;
            combined = (combined_cnt <= 64) ? combined_small : (struct Node **)nuq_scratch_alloc(combined_cnt * sizeof(struct Node *));
            for (size_t i = 0; i < k; i++) combined[i] = steps[i];
            for (size_t i = 0; i < inner_cnt; i++) combined[k + i] = inner_steps[i];
            for (size_t i = k + 1; i < step_cnt; i++) combined[i + inner_cnt - 1] = steps[i];
            path_dfs(c, combined, combined_cnt, k, v, pu);
            return;
        }
    }
    /* Path traversal hit a non-accessor step.  Format jq's diagnostic
     * including the current value and (if any) the attempted next step.
     * jq uses the raw JSON rendering for these messages (no `<type> (..)`
     * wrapping). */
    {
        char *json_buf = NULL; size_t jl = 0;
        FILE *jfp = open_memstream(&json_buf, &jl);
        nuq_json_print(jfp, v, 0);
        fclose(jfp);
        char vd[80], msg[256];
        size_t lim = sizeof(vd) - 1;
        size_t copy = jl <= lim ? jl : lim;
        memcpy(vd, json_buf, copy); vd[copy] = 0;
        free(json_buf);
        if (k + 1 >= step_cnt) {
            int w = snprintf(msg, sizeof(msg),
                             "Invalid path expression with result %s", vd);
            c->error = nuq_make_string(msg, (size_t)w);
        } else {
            struct Node *nxt = steps[k + 1];
            /* Skip the parser-inserted `as(., $V, body)` wrappers added
             * for outer-input capture so we report the actual user
             * accessor (`.foo` / `.[N]` / `.[]`).  `as` body is
             * `pipe(., index/slice)`; descend into the rhs of that pipe. */
            while (nxt) {
                if (nxt->head.kind == &kind_node_as) { nxt = nxt->u.node_as.body; continue; }
                if (nxt->head.kind == &kind_node_pipe) {
                    /* If the pipe's lhs is identity, the meaningful step is
                     * the rhs.  Otherwise lhs is the meaningful step. */
                    if (nxt->u.node_pipe.lhs->head.kind == &kind_node_identity)
                        nxt = nxt->u.node_pipe.rhs;
                    else
                        nxt = nxt->u.node_pipe.lhs;
                    continue;
                }
                if (nxt->head.kind == &kind_node_identity) { nxt = NULL; }
                break;
            }
            if (!nxt) {
                int w = snprintf(msg, sizeof(msg),
                                 "Invalid path expression with result %s", vd);
                c->error = nuq_make_string(msg, (size_t)w);
                goto path_dfs_err_done;
            }
            if (nxt->head.kind == &kind_node_field || nxt->head.kind == &kind_node_field_opt) {
                const char *name = (nxt->head.kind == &kind_node_field)
                    ? nxt->u.node_field.name : nxt->u.node_field_opt.name;
                int w = snprintf(msg, sizeof(msg),
                                 "Invalid path expression near attempt to access element \"%s\" of %s",
                                 name, vd);
                c->error = nuq_make_string(msg, (size_t)w);
            } else if (nxt->head.kind == &kind_node_index || nxt->head.kind == &kind_node_index_opt) {
                struct Node *ix = (nxt->head.kind == &kind_node_index)
                    ? nxt->u.node_index.expr : nxt->u.node_index_opt.expr;
                VALUE kv;
                bool got = false;
                /* The parser's outer-scope wrap turns `.[N]` into
                 * `as(., $V, pipe(., index($V | N)))`.  Strip the
                 * `$V | ` prefix so eval_static_key can fold N. */
                struct Node *inner_ix = ix;
                if (inner_ix->head.kind == &kind_node_pipe &&
                    inner_ix->u.node_pipe.lhs->head.kind == &kind_node_var) {
                    inner_ix = inner_ix->u.node_pipe.rhs;
                }
                if (eval_static_key(inner_ix, &kv)) got = true;
                if (got) {
                    char kd[40] = "?";
                    if (NUQ_IS_FIX(kv)) snprintf(kd, sizeof(kd), "%lld", (long long)NUQ_FIX_VAL(kv));
                    else if (NUQ_IS_PTR(kv) && NUQ_PTR(kv)->type == NUQ_T_STRING) {
                        struct nuq_obj *so = NUQ_PTR(kv);
                        snprintf(kd, sizeof(kd), "\"%.*s\"", (int)so->str.len, so->str.bytes);
                    }
                    int w = snprintf(msg, sizeof(msg),
                                     "Invalid path expression near attempt to access element %s of %s",
                                     kd, vd);
                    c->error = nuq_make_string(msg, (size_t)w);
                } else {
                    int w = snprintf(msg, sizeof(msg),
                                     "Invalid path expression near attempt to access element of %s", vd);
                    c->error = nuq_make_string(msg, (size_t)w);
                }
            } else if (nxt->head.kind == &kind_node_iter || nxt->head.kind == &kind_node_iter_opt) {
                int w = snprintf(msg, sizeof(msg),
                                 "Invalid path expression near attempt to iterate through %s", vd);
                c->error = nuq_make_string(msg, (size_t)w);
            } else {
                int w = snprintf(msg, sizeof(msg),
                                 "Invalid path expression with result %s", vd);
                c->error = nuq_make_string(msg, (size_t)w);
            }
path_dfs_err_done: ;
        }
    }
}

EMIT
nuq_path_eval(CTX *c, struct Node *body)
{
    /* Comma at the top splits into multiple independent paths — handle
     * each branch separately so the path-flattening machinery doesn't
     * choke on the comma node. */
    if (body->head.kind == &kind_node_comma) {
        size_t outer = c->pool_top;
        EMIT a = nuq_path_eval(c, body->u.node_comma.lhs);
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
        (void)a;     /* slice already on pool */
        EMIT b = nuq_path_eval(c, body->u.node_comma.rhs);
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
        (void)b;
        return nuq_emit_slice(c, outer);
    }
    struct Node **steps = NULL; size_t cnt = 0, capa = 0;
    path_flatten(body, &steps, &cnt, &capa);
    size_t outer = c->pool_top;
    struct path_ud pu = { c, NULL, 0, 0 };
    path_dfs(c, steps, cnt, 0, c->input, &pu);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    return nuq_emit_slice(c, outer);
}

/* Leaf functions for the assign-op variants. */
struct assign_leaf_ud {
    CTX        *c;
    struct Node *rhs;
    int         op_kind;
    /* For PLAIN, eval rhs once globally (against the original input)
     * and reuse — `(.foo[]) = X` should put the SAME X at every leaf,
     * not re-evaluate.  For UPDATE / arith ops we re-evaluate per
     * leaf with that leaf as input. */
    VALUE       precomputed;
    bool        precomputed_set;
};

static VALUE
assign_leaf(VALUE v, void *ud, bool *dropped)
{
    struct assign_leaf_ud *al = (struct assign_leaf_ud *)ud;
    *dropped = false;
    if (al->op_kind == NUQ_ASSIGN_PLAIN) {
        return al->precomputed;
    }
    /* For op-assign variants other than `|=` (UPDATE), RHS was
     * pre-evaluated in the caller scope and stored in `precomputed`.
     * `|=` still evaluates rhs against the path-element value `v`. */
    if (al->precomputed_set && al->op_kind != NUQ_ASSIGN_UPDATE) {
        VALUE rv = al->precomputed;
        switch (al->op_kind) {
          case NUQ_ASSIGN_PLUS:   return nuq_op_add(v, rv);
          case NUQ_ASSIGN_MINUS:  return nuq_op_sub(v, rv);
          case NUQ_ASSIGN_MUL:    return nuq_op_mul(v, rv);
          case NUQ_ASSIGN_DIV:    return nuq_op_div(v, rv);
          case NUQ_ASSIGN_MOD:    return nuq_op_mod(v, rv);
          case NUQ_ASSIGN_ALT:    return nuq_truthy(v) ? v : rv;
        }
        return v;
    }
    /* Evaluate rhs with `v` as input (|= path). */
    VALUE saved = al->c->input;
    al->c->input = v;
    size_t t0 = al->c->pool_top;
    EMIT re = EVAL(al->c, al->rhs);
    al->c->input = saved;
    if (al->c->error != NUQ_NULL) return v;
    /* `path |= select(...)` style: when the rhs emits nothing, jq's
     * update semantics is to DROP the entry (dropped=true).  For arith
     * variants we still need a value, so an empty rhs there falls back
     * to null. */
    if (al->op_kind == NUQ_ASSIGN_UPDATE && re.count == 0) {
        al->c->pool_top = t0;
        *dropped = true;
        return v;
    }
    VALUE rv = re.count > 0 ? re.items[0] : NUQ_NULL;
    al->c->pool_top = t0;
    switch (al->op_kind) {
      case NUQ_ASSIGN_UPDATE: return rv;
      case NUQ_ASSIGN_PLUS:   return nuq_op_add(v, rv);
      case NUQ_ASSIGN_MINUS:  return nuq_op_sub(v, rv);
      case NUQ_ASSIGN_MUL:    return nuq_op_mul(v, rv);
      case NUQ_ASSIGN_DIV:    return nuq_op_div(v, rv);
      case NUQ_ASSIGN_MOD:    return nuq_op_mod(v, rv);
      case NUQ_ASSIGN_ALT:    return nuq_truthy(v) ? v : rv;
    }
    return v;
}

static VALUE
delete_leaf(VALUE v, void *ud, bool *dropped)
{
    (void)v; (void)ud;
    *dropped = true;
    return NUQ_NULL;
}

/* `del(path-expr)` — walk the path tree, dropping each leaf entry.
 * For `.[]` paths this drops each iterated element, yielding an
 * empty container.  Single-pass, one clone per container along
 * the path. */
EMIT
nuq_del_eval(CTX *c, struct Node *path_expr)
{
    /* del(.) returns null in jq. */
    if (path_expr->head.kind == &kind_node_identity)
        return nuq_emit_one(c, NUQ_NULL);
    /* `del(p1, p2, ...)` — collect paths from each comma branch, sort
     * descending, delete one-by-one (array-index shift safe).  Use
     * `nuq_path_eval` to handle each branch (which gives us the same
     * path normalization as path()). */
    if (path_expr->head.kind == &kind_node_comma) {
        size_t outer = c->pool_top;
        EMIT pe = nuq_path_eval(c, path_expr);
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
        uint32_t pcnt = pe.count;
        VALUE small[64];
        VALUE *paths = (pcnt <= 64) ? small : (VALUE *)nuq_scratch_alloc(pcnt * sizeof(VALUE));
        memcpy(paths, pe.items, pcnt * sizeof(VALUE));
        c->pool_top = outer;
        for (uint32_t i = 1; i < pcnt; i++) {
            VALUE x = paths[i];
            uint32_t j = i;
            while (j > 0 && nuq_cmp(paths[j-1], x) < 0) {
                paths[j] = paths[j-1]; j--;
            }
            paths[j] = x;
        }
        VALUE cur = c->input;
        for (uint32_t i = 0; i < pcnt; i++) {
            VALUE p = paths[i];
            if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_ARRAY)) continue;
            struct nuq_obj *po = NUQ_PTR(p);
            if (po->arr.len == 0) { cur = NUQ_NULL; continue; }
            cur = delpath_recurse(cur, po->arr.items, po->arr.len);
        }
        return nuq_emit_one(c, cur);
    }
    /* Try the cheap walk_path first — single-clone per container.
     * If it fails with a multi-emit error inside the path expression
     * (e.g. `.[nan, nan]`), fall back to path-collection + delpaths. */
    VALUE saved_err = c->error;
    VALUE result = walk_path(c, path_expr, c->input, delete_leaf, NULL);
    if (c->error == NUQ_NULL) return nuq_emit_one(c, result);
    /* Try path()+delpaths fallback. */
    c->error = saved_err;
    size_t outer = c->pool_top;
    EMIT pe = nuq_path_eval(c, path_expr);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t pcnt = pe.count;
    VALUE small[64];
    VALUE *paths = (pcnt <= 64) ? small : (VALUE *)nuq_scratch_alloc(pcnt * sizeof(VALUE));
    memcpy(paths, pe.items, pcnt * sizeof(VALUE));
    c->pool_top = outer;
    for (uint32_t i = 1; i < pcnt; i++) {
        VALUE x = paths[i];
        uint32_t j = i;
        while (j > 0 && nuq_cmp(paths[j-1], x) < 0) { paths[j] = paths[j-1]; j--; }
        paths[j] = x;
    }
    VALUE cur = c->input;
    for (uint32_t i = 0; i < pcnt; i++) {
        VALUE p = paths[i];
        if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_ARRAY)) continue;
        struct nuq_obj *po = NUQ_PTR(p);
        if (po->arr.len == 0) { cur = NUQ_NULL; continue; }
        cur = delpath_recurse(cur, po->arr.items, po->arr.len);
    }
    return nuq_emit_one(c, cur);
}

EMIT
nuq_assign_eval(CTX *c, struct Node *lhs, struct Node *rhs, uint32_t op_kind)
{
    struct assign_leaf_ud ud = { c, rhs, (int)op_kind, NUQ_NULL, false };

    /* `+=` / `-=` / `*=` / `/=` / `%=` / `//=` evaluate RHS against the
     * ORIGINAL input (not the path-element value).  jq compiles
     * `.a += b` as `. as $o | $o | .a = ($o | .a + ($o | b))` — i.e.
     * `b` sees `$o` as its input.  Pre-compute the RHS once and reuse
     * the result at each leaf.
     * `|=` (UPDATE) is the exception: jq evaluates rhs against the
     * path-element value, so we keep the per-leaf eval for it. */
    if (op_kind == NUQ_ASSIGN_PLUS || op_kind == NUQ_ASSIGN_MINUS ||
        op_kind == NUQ_ASSIGN_MUL  || op_kind == NUQ_ASSIGN_DIV   ||
        op_kind == NUQ_ASSIGN_MOD  || op_kind == NUQ_ASSIGN_ALT) {
        size_t t0 = c->pool_top;
        EMIT re = EVAL(c, rhs);
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
        ud.precomputed = re.count > 0 ? re.items[0] : NUQ_NULL;
        ud.precomputed_set = true;
        c->pool_top = t0;
    }

    /* For `=` (PLAIN), RHS is evaluated ONCE in the original input
     * scope and stamped at every leaf — so `(.foo[]) = .bar` puts the
     * original input's `.bar` value at every `.foo[i]`.  When RHS
     * emits multiple values, jq runs the assignment once per value
     * and emits each resulting input — `.[2:4] = (a, b, c)` produces
     * three outputs. */
    if (op_kind == NUQ_ASSIGN_PLAIN) {
        size_t outer = c->pool_top;
        EMIT re = EVAL(c, rhs);
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
        uint32_t rc = re.count;
        if (rc == 0) { c->pool_top = outer; return EMIT_EMPTY; }
        VALUE rsmall[16];
        VALUE *rs = (rc <= 16) ? rsmall : (VALUE *)nuq_scratch_alloc(rc * sizeof(VALUE));
        memcpy(rs, re.items, rc * sizeof(VALUE));
        c->pool_top = outer;
        ud.precomputed_set = true;
        for (uint32_t i = 0; i < rc; i++) {
            ud.precomputed = rs[i];
            VALUE result = walk_path(c, lhs, c->input, assign_leaf, &ud);
            if (c->error != NUQ_NULL) {
                /* Multi-emit path — fallback via path()+setpath. */
                if (strstr(nuq_string_cstr(c->error), "exactly one") == NULL)
                    return EMIT_EMPTY;
                c->error = NUQ_NULL;
                size_t inner_outer = c->pool_top;
                EMIT pe = nuq_path_eval(c, lhs);
                if (c->error != NUQ_NULL) return EMIT_EMPTY;
                uint32_t pcnt = pe.count;
                VALUE psmall[64];
                VALUE *paths = (pcnt <= 64) ? psmall : (VALUE *)nuq_scratch_alloc(pcnt * sizeof(VALUE));
                memcpy(paths, pe.items, pcnt * sizeof(VALUE));
                c->pool_top = inner_outer;
                for (uint32_t a = 1; a < pcnt; a++) {
                    VALUE x = paths[a]; uint32_t b = a;
                    while (b > 0 && nuq_cmp(paths[b-1], x) < 0) { paths[b] = paths[b-1]; b--; }
                    paths[b] = x;
                }
                VALUE cur = c->input;
                for (uint32_t a = 0; a < pcnt; a++) {
                    VALUE p = paths[a];
                    if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_ARRAY)) continue;
                    struct nuq_obj *po = NUQ_PTR(p);
                    if (po->arr.len == 0) continue;
                    cur = setpath_recurse(cur, po->arr.items, po->arr.len, ud.precomputed);
                    if (c->error != NUQ_NULL) return EMIT_EMPTY;
                }
                nuq_pool_push(c, cur);
            } else {
                nuq_pool_push(c, result);
            }
        }
        return nuq_emit_slice(c, outer);
    }

    VALUE result = walk_path(c, lhs, c->input, assign_leaf, &ud);
    if (c->error == NUQ_NULL) return nuq_emit_one(c, result);
    /* Multi-emit path index — fall back to path()+per-path setpath.
     * Only retry on the specific "exactly one" path-index error so we
     * don't swallow legitimate type errors. */
    if (strstr(nuq_string_cstr(c->error), "exactly one") == NULL)
        return EMIT_EMPTY;
    c->error = NUQ_NULL;
    size_t outer = c->pool_top;
    EMIT pe = nuq_path_eval(c, lhs);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t pcnt = pe.count;
    VALUE small[64];
    VALUE *paths = (pcnt <= 64) ? small : (VALUE *)nuq_scratch_alloc(pcnt * sizeof(VALUE));
    memcpy(paths, pe.items, pcnt * sizeof(VALUE));
    c->pool_top = outer;
    /* Sort descending to keep array indices stable while we update. */
    for (uint32_t i = 1; i < pcnt; i++) {
        VALUE x = paths[i];
        uint32_t j = i;
        while (j > 0 && nuq_cmp(paths[j-1], x) < 0) { paths[j] = paths[j-1]; j--; }
        paths[j] = x;
    }
    VALUE cur = c->input;
    for (uint32_t i = 0; i < pcnt; i++) {
        VALUE p = paths[i];
        if (!(NUQ_IS_PTR(p) && NUQ_PTR(p)->type == NUQ_T_ARRAY)) continue;
        struct nuq_obj *po = NUQ_PTR(p);
        if (po->arr.len == 0) continue;
        /* Fetch current value at path. */
        VALUE leaf = cur;
        for (size_t j = 0; j < po->arr.len; j++) {
            VALUE k = po->arr.items[j];
            if (NUQ_IS_PTR(leaf) && NUQ_PTR(leaf)->type == NUQ_T_OBJECT)
                leaf = nuq_object_get(leaf, k);
            else if (NUQ_IS_PTR(leaf) && NUQ_PTR(leaf)->type == NUQ_T_ARRAY) {
                int64_t idx = NUQ_IS_FIX(k) ? NUQ_FIX_VAL(k) : 0;
                if (idx < 0) idx += (int64_t)NUQ_PTR(leaf)->arr.len;
                if (idx < 0 || (size_t)idx >= NUQ_PTR(leaf)->arr.len) {
                    leaf = NUQ_NULL; break;
                }
                leaf = NUQ_PTR(leaf)->arr.items[idx];
            } else { leaf = NUQ_NULL; break; }
        }
        bool dropped = false;
        VALUE new_leaf = assign_leaf(leaf, &ud, &dropped);
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
        if (dropped) {
            cur = delpath_recurse(cur, po->arr.items, po->arr.len);
        } else {
            cur = setpath_recurse(cur, po->arr.items, po->arr.len, new_leaf);
        }
    }
    return nuq_emit_one(c, cur);
}

EMIT
nuq_getpath_eval(CTX *c, struct Node *path)
{
    size_t t0 = c->pool_top;
    EMIT buf = EVAL(c, path);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (buf.count == 0) { c->pool_top = t0; return EMIT_EMPTY; }
    VALUE pv = buf.items[0];
    c->pool_top = t0;
    if (!(NUQ_IS_PTR(pv) && NUQ_PTR(pv)->type == NUQ_T_ARRAY))
        return err_emit(c, "getpath: not array");
    struct nuq_obj *po = NUQ_PTR(pv);
    /* Path-depth guard: jq raises "Path too deep" for paths > 10000 deep. */
    if (po->arr.len > 10000) return err_emit(c, "Path too deep");
    VALUE v = c->input;
    for (size_t i = 0; i < po->arr.len; i++) {
        VALUE k = po->arr.items[i];
        if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_OBJECT) v = nuq_object_get(v, k);
        else if (NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_ARRAY) {
            int64_t idx = NUQ_IS_FIX(k) ? NUQ_FIX_VAL(k) : 0;
            v = nuq_array_get(v, idx);
        } else { v = NUQ_NULL; break; }
    }
    return nuq_emit_one(c, v);
}

/* ---------- streaming body evaluation -----------------------------------
 *
 * `nuq_stream_eval(c, body, cb, ud)` walks `body` as a generator,
 * calling `cb(c, v, ud)` for each emitted value.  cb returning false
 * stops the walk early — so `limit(1; 1, error)` can take `1` then
 * abandon evaluation before `error` runs.
 *
 * Lazy structurally over `comma` (sequence) and `pipe` (chain).
 * Other kinds fall back to a full EVAL — generators like `range`
 * still materialize their full output (the failing tests don't need
 * that case to be lazy). */
typedef bool (*nuq_stream_cb)(CTX *c, VALUE v, void *ud);

static bool nuq_stream_eval(CTX *c, struct Node *body, nuq_stream_cb cb, void *ud);

struct stream_pipe_ud { struct Node *rhs; nuq_stream_cb cb; void *ud; };

static bool
stream_pipe_inner(CTX *c, VALUE v, void *ud_)
{
    struct stream_pipe_ud *p = (struct stream_pipe_ud *)ud_;
    VALUE saved = c->input;
    c->input = v;
    bool r = nuq_stream_eval(c, p->rhs, p->cb, p->ud);
    c->input = saved;
    return r;
}

static bool
nuq_stream_eval(CTX *c, struct Node *body, nuq_stream_cb cb, void *ud)
{
    if (c->error != NUQ_NULL) return false;
    if (body->head.kind == &kind_node_comma) {
        if (!nuq_stream_eval(c, body->u.node_comma.lhs, cb, ud)) return false;
        return nuq_stream_eval(c, body->u.node_comma.rhs, cb, ud);
    }
    if (body->head.kind == &kind_node_pipe) {
        struct stream_pipe_ud sp = { body->u.node_pipe.rhs, cb, ud };
        return nuq_stream_eval(c, body->u.node_pipe.lhs, stream_pipe_inner, &sp);
    }
    /* Fallback: materialise the body's output, callback per emit. */
    size_t t0 = c->pool_top;
    EMIT e = EVAL(c, body);
    if (c->error != NUQ_NULL) { c->pool_top = t0; return false; }
    uint32_t cnt = e.count;
    VALUE small[16];
    VALUE *vs = (cnt <= 16) ? small : (VALUE *)nuq_scratch_alloc(cnt * sizeof(VALUE));
    memcpy(vs, e.items, cnt * sizeof(VALUE));
    c->pool_top = t0;
    for (uint32_t i = 0; i < cnt; i++) {
        if (!cb(c, vs[i], ud)) return false;
    }
    return true;
}

struct limit_ud { int64_t remaining; };

static bool
limit_cb(CTX *c, VALUE v, void *ud_)
{
    struct limit_ud *u = (struct limit_ud *)ud_;
    if (u->remaining <= 0) return false;
    nuq_pool_push(c, v);
    u->remaining--;
    return u->remaining > 0;
}

EMIT
nuq_limit_eval(CTX *c, struct Node *cnt, struct Node *body)
{
    /* jq semantics: `limit(N; f)` runs the count expression first.
     * If count emits multiple values (e.g. `limit(2,3; range(9))`),
     * each is treated as an independent invocation: the body is
     * re-run per count value and outputs concatenated.
     *
     * Lazy: stops streaming after N emits, so `limit(1; 1, error)` → 1. */
    size_t outer = c->pool_top;
    EMIT nb = EVAL(c, cnt);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t ccnt = nb.count;
    int64_t small_ns[16];
    int64_t *ns = (ccnt <= 16) ? small_ns
                               : (int64_t *)nuq_scratch_alloc(ccnt * sizeof(int64_t));
    for (uint32_t i = 0; i < ccnt; i++) ns[i] = to_int64(nb.items[i]);
    c->pool_top = outer;

    for (uint32_t k = 0; k < ccnt; k++) {
        int64_t n = ns[k];
        if (n < 0) {
            c->error = nuq_make_string("limit doesn't support negative count", 36);
            return EMIT_EMPTY;
        }
        if (n == 0) continue;     /* zero count → empty */
        struct limit_ud ud = { n };
        nuq_stream_eval(c, body, limit_cb, &ud);
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
    }
    return nuq_emit_slice(c, outer);
}

/* `skip(N; f)` — skip first N emits of f, then emit the rest.
 * Multi-emit N runs body once per N (like limit / nth). */
EMIT
nuq_skip_eval(CTX *c, struct Node *cnt_n, struct Node *body)
{
    size_t outer = c->pool_top;
    EMIT nb = EVAL(c, cnt_n);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = nb.count;
    int64_t small_ns[16];
    int64_t *ns = (cnt <= 16) ? small_ns
                              : (int64_t *)nuq_scratch_alloc(cnt * sizeof(int64_t));
    for (uint32_t i = 0; i < cnt; i++) ns[i] = to_int64(nb.items[i]);
    c->pool_top = outer;

    for (uint32_t k = 0; k < cnt; k++) {
        int64_t n = ns[k];
        if (n < 0) return err_emit(c, "skip doesn't support negative count");
        size_t before = c->pool_top;
        EMIT bo = EVAL(c, body);
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
        size_t skip = (size_t)n;
        if (skip >= bo.count) {
            c->pool_top = before;        /* drop everything */
        } else {
            /* shift down */
            size_t kept = bo.count - skip;
            for (size_t i = 0; i < kept; i++)
                c->pool[before + i] = c->pool[before + skip + i];
            c->pool_top = before + kept;
        }
    }
    return nuq_emit_slice(c, outer);
}

struct nth_ud { int64_t target; int64_t cnt; bool got; VALUE result; };

static bool
nth_cb(CTX *c, VALUE v, void *ud_)
{
    (void)c;
    struct nth_ud *u = (struct nth_ud *)ud_;
    if (u->cnt == u->target) {
        u->result = v;
        u->got = true;
        return false;     /* stop walking */
    }
    u->cnt++;
    return true;
}

EMIT
nuq_nth_eval(CTX *c, struct Node *idx, struct Node *body)
{
    /* `nth(N; f)`: emit the N-th value of f.  Multi-emit N runs the
     * body once per N value, like `limit`.  Lazy: stops at the N-th
     * emit so subsequent `error` etc. is never evaluated. */
    size_t outer = c->pool_top;
    EMIT nb = EVAL(c, idx);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    uint32_t cnt = nb.count;
    int64_t small_ns[16];
    int64_t *ns = (cnt <= 16) ? small_ns
                              : (int64_t *)nuq_scratch_alloc(cnt * sizeof(int64_t));
    for (uint32_t i = 0; i < cnt; i++) ns[i] = to_int64(nb.items[i]);
    c->pool_top = outer;

    for (uint32_t k = 0; k < cnt; k++) {
        int64_t n = ns[k];
        if (n < 0) return err_emit(c, "nth doesn't support negative indices");
        struct nth_ud ud = { n, 0, false, NUQ_NULL };
        nuq_stream_eval(c, body, nth_cb, &ud);
        if (c->error != NUQ_NULL) return EMIT_EMPTY;
        if (ud.got) nuq_pool_push(c, ud.result);
    }
    return nuq_emit_slice(c, outer);
}

/* ---------- lazy first / last / isempty / any / all -------------------- */

struct first_ud { bool got; VALUE result; };
static bool first_cb(CTX *c, VALUE v, void *ud_) {
    (void)c;
    struct first_ud *u = (struct first_ud *)ud_;
    u->result = v;
    u->got = true;
    return false;       /* stop */
}

EMIT
nuq_first1_eval(CTX *c, struct Node *body)
{
    struct first_ud ud = { false, NUQ_NULL };
    nuq_stream_eval(c, body, first_cb, &ud);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (!ud.got) return EMIT_EMPTY;
    return nuq_emit_one(c, ud.result);
}

/* `last(f)` — must walk the entire stream (no shortcut), but reuse
 * the streaming walker so deeply-nested pipe/comma still works. */
static bool last_cb(CTX *c, VALUE v, void *ud_) {
    (void)c;
    struct first_ud *u = (struct first_ud *)ud_;
    u->result = v;
    u->got = true;
    return true;
}

EMIT
nuq_last1_eval(CTX *c, struct Node *body)
{
    struct first_ud ud = { false, NUQ_NULL };
    nuq_stream_eval(c, body, last_cb, &ud);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    if (!ud.got) return EMIT_EMPTY;
    return nuq_emit_one(c, ud.result);
}

static bool isempty_cb(CTX *c, VALUE v, void *ud_) {
    (void)c; (void)v;
    bool *got = (bool *)ud_;
    *got = true;
    return false;       /* one emit is enough — stop */
}

EMIT
nuq_isempty_eval(CTX *c, struct Node *body)
{
    bool got = false;
    nuq_stream_eval(c, body, isempty_cb, &got);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    return nuq_emit_one(c, got ? NUQ_FALSE : NUQ_TRUE);
}

struct anyall_ud { struct Node *cond; bool result; bool stop_on; };

static bool anyall_cb(CTX *c, VALUE v, void *ud_) {
    struct anyall_ud *u = (struct anyall_ud *)ud_;
    VALUE saved = c->input;
    c->input = v;
    size_t t = c->pool_top;
    EMIT co = EVAL(c, u->cond);
    c->input = saved;
    if (c->error != NUQ_NULL) return false;
    bool truthy = false;
    for (uint32_t i = 0; i < co.count; i++)
        if (nuq_truthy(co.items[i])) { truthy = true; break; }
    c->pool_top = t;
    /* For `any`: stop_on=true → stop & set result on first truthy.
     * For `all`: stop_on=false → stop & set result on first falsy. */
    if (truthy == u->stop_on) {
        u->result = u->stop_on;
        return false;
    }
    return true;
}

EMIT
nuq_any2_eval(CTX *c, struct Node *gen, struct Node *cond)
{
    struct anyall_ud ud = { cond, false, true };
    nuq_stream_eval(c, gen, anyall_cb, &ud);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    return nuq_emit_one(c, ud.result ? NUQ_TRUE : NUQ_FALSE);
}

EMIT
nuq_all2_eval(CTX *c, struct Node *gen, struct Node *cond)
{
    struct anyall_ud ud = { cond, true, false };
    nuq_stream_eval(c, gen, anyall_cb, &ud);
    if (c->error != NUQ_NULL) return EMIT_EMPTY;
    return nuq_emit_one(c, ud.result ? NUQ_TRUE : NUQ_FALSE);
}

/* --- top-level run ---------------------------------------------------- */

/* Track whether any truthy output has been emitted across the run —
 * used by `-e` / `--exit-status` to set exit code 5 when none. */
bool nuq_had_truthy_output = false;
/* Counter — non-zero suppresses stderr prints in value helpers. */
int  nuq_suppress_error_print = 0;
/* Set by `nuq_run` so value-level helpers can route errors back to
 * the running CTX.  When NULL, helpers degrade to NUQ_NULL-return. */
CTX *nuq_active_ctx = NULL;

/* Single error-emit helper for value-level functions in value.c /
 * builtin.c that don't take a CTX*.  Sets nuq_active_ctx->error so
 * `try` / `?` / `isvalid` can catch, and prints to stderr unless
 * suppressed.  Returns NUQ_NULL for chaining. */
VALUE
nuq_helper_error(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (nuq_active_ctx && nuq_active_ctx->error == NUQ_NULL)
        nuq_active_ctx->error = nuq_make_string(buf, strlen(buf));
    if (!nuq_suppress_error_print)
        fprintf(stderr, "nuq error: %s\n", buf);
    return NUQ_NULL;
}
/* Track whether any error has been raised — used to set exit code 1. */
bool nuq_had_error = false;

void
nuq_run(CTX *const c, struct Node *const filter, VALUE input)
{
    /* Re-bind --arg style variables on each top-level run so they stay
     * visible after var_top has been used (and trimmed) on prior runs. */
    c->var_top = 0;
    nuq_user_args_bind(c);
    c->input = input;
    c->error = NUQ_NULL;
    c->break_label = 0;
    c->pool_top = 0;
    nuq_active_ctx = c;
    /* Per-run arena: route VALUE allocs through the bump pointer so
     * intermediate values from this run can be dropped en masse after
     * print.  Permanent state (AST, literals, --argjson, --slurpfile,
     * module data) was set up under perm=true and stays in Boehm. */
    nuq_alloc_perm = false;
    EMIT emits = EVAL(c, filter);
    nuq_active_ctx = NULL;
    /* Even when the filter ultimately errored, jq still prints any
     * emits that came before the error.  Output them first, then
     * surface the error. */
    for (uint32_t i = 0; i < emits.count; i++) {
        VALUE v = emits.items[i];
        if (OPTION.seq_output) fputc_unlocked('\x1e', stdout);  /* RFC 7464 RS */
        if (OPTION.raw_output && NUQ_IS_PTR(v) && NUQ_PTR(v)->type == NUQ_T_STRING) {
            struct nuq_obj *o = NUQ_PTR(v);
            fwrite_unlocked(o->str.bytes, 1, o->str.len, stdout);
        } else {
            nuq_json_print(stdout, v, OPTION.compact_output ? 0 : OPTION.indent);
        }
        fputc_unlocked('\n', stdout);
        if (nuq_truthy(v)) nuq_had_truthy_output = true;
    }
    if (c->error != NUQ_NULL) {
        fflush(stdout);     /* keep stderr after stdout */
        if (NUQ_IS_PTR(c->error) && NUQ_PTR(c->error)->type == NUQ_T_STRING)
            fprintf(stderr, "nuq: error: %s\n", nuq_string_cstr(c->error));
        else {
            fprintf(stderr, "nuq: error: ");
            nuq_json_print(stderr, c->error, 0);
            fputc('\n', stderr);
        }
        c->error = NUQ_NULL;
        nuq_had_error = true;
    }
    /* End-of-run: drop intermediates en masse and re-arm permanent
     * mode for any setup that runs before the next nuq_run. */
    nuq_alloc_perm = true;
    nuq_arena_reset();
}

/* --- recurse / paths pool versions -------------------------------- */

void
nuq_recurse_collect_pool(CTX *c, VALUE v)
{
    nuq_pool_push(c, v);
    if (NUQ_IS_PTR(v)) {
        struct nuq_obj *o = NUQ_PTR(v);
        if (o->type == NUQ_T_ARRAY) {
            for (size_t i = 0; i < o->arr.len; i++) nuq_recurse_collect_pool(c, o->arr.items[i]);
        } else if (o->type == NUQ_T_OBJECT) {
            for (size_t i = 0; i < o->obj.len; i++) nuq_recurse_collect_pool(c, o->obj.vals[i]);
        }
    }
}

static void
paths_walk_pool(CTX *c, VALUE v, VALUE path)
{
    if (!NUQ_IS_PTR(v)) return;
    int t = NUQ_PTR(v)->type;
    if (t != NUQ_T_ARRAY && t != NUQ_T_OBJECT) return;
    /* PIN v and path: nuq_clone / nuq_array_push / nuq_pool_push and
     * the recursive call may allocate and trigger GC, which would
     * invalidate any raw `struct nuq_obj *` cached from NUQ_PTR(v). */
    NUQ_GC_PIN2(v, path);
    if (t == NUQ_T_ARRAY) {
        size_t len = NUQ_PTR(v)->arr.len;
        for (size_t i = 0; i < len; i++) {
            VALUE p = nuq_clone(path);
            NUQ_GC_PIN1(p); /* nuq_array_push may grow → GC */
            nuq_array_push(p, nuq_make_int((int64_t)i));
            nuq_pool_push(c, p);
            VALUE child = NUQ_PTR(v)->arr.items[i]; /* refetch after allocs */
            paths_walk_pool(c, child, p);
            NUQ_GC_UNPIN(1);
        }
    } else {
        size_t len = NUQ_PTR(v)->obj.len;
        for (size_t i = 0; i < len; i++) {
            VALUE p = nuq_clone(path);
            NUQ_GC_PIN1(p);
            nuq_array_push(p, NUQ_PTR(v)->obj.keys[i]); /* refetch */
            nuq_pool_push(c, p);
            VALUE child = NUQ_PTR(v)->obj.vals[i]; /* refetch */
            paths_walk_pool(c, child, p);
            NUQ_GC_UNPIN(1);
        }
    }
    NUQ_GC_UNPIN(2);
}

void
nuq_paths_collect_pool(CTX *c, VALUE v)
{
    paths_walk_pool(c, v, nuq_make_array(0));
}

/* leaf_paths: only emit paths that point to scalar leaves
 * (i.e. not arrays / objects). */
static void
leaf_paths_walk(CTX *c, VALUE v, VALUE path)
{
    if (!NUQ_IS_PTR(v)) {
        nuq_pool_push(c, path);
        return;
    }
    int t = NUQ_PTR(v)->type;
    if (t != NUQ_T_ARRAY && t != NUQ_T_OBJECT) {
        nuq_pool_push(c, path);
        return;
    }
    NUQ_GC_PIN2(v, path);
    if (t == NUQ_T_ARRAY) {
        size_t len = NUQ_PTR(v)->arr.len;
        for (size_t i = 0; i < len; i++) {
            VALUE p = nuq_clone(path);
            NUQ_GC_PIN1(p);
            nuq_array_push(p, nuq_make_int((int64_t)i));
            VALUE child = NUQ_PTR(v)->arr.items[i];
            leaf_paths_walk(c, child, p);
            NUQ_GC_UNPIN(1);
        }
    } else {
        size_t len = NUQ_PTR(v)->obj.len;
        for (size_t i = 0; i < len; i++) {
            VALUE p = nuq_clone(path);
            NUQ_GC_PIN1(p);
            nuq_array_push(p, NUQ_PTR(v)->obj.keys[i]);
            VALUE child = NUQ_PTR(v)->obj.vals[i];
            leaf_paths_walk(c, child, p);
            NUQ_GC_UNPIN(1);
        }
    }
    NUQ_GC_UNPIN(2);
}

void
nuq_leaf_paths_collect_pool(CTX *c, VALUE v)
{
    leaf_paths_walk(c, v, nuq_make_array(0));
}
