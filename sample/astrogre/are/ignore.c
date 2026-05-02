/*
 * Stack-shaped .gitignore matcher.  See ignore.h for scope and the
 * semantic notes that drive these heuristics.
 *
 * Match query model (`ignore_should_skip`):
 *   For each layer, innermost first:
 *     For each rule in that layer, LAST first:
 *       If rule matches the entry → its `negate` decides:
 *         negate=false → skip (return true)
 *         negate=true  → un-skip (return false), short-circuit
 *   Default → not skipped.
 *
 * Pattern matching is fnmatch(3) with FNM_PATHNAME for slash-bearing
 * patterns and plain fnmatch(0) for basename-only ones.  The leading
 * `**\/` shorthand is normalised at parse time to a basename match
 * (since `**\/foo` semantically means "any `foo` at any depth").
 */

#define _GNU_SOURCE 1

#include "ignore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

static char *
xstrdup(const char *s)
{
    char *d = strdup(s);
    if (!d) { perror("are: strdup"); exit(2); }
    return d;
}

void
ignore_stack_init(ignore_stack_t *st, bool disabled)
{
    st->layers   = NULL;
    st->n_layers = 0;
    st->cap_layers = 0;
    st->disabled = disabled;
}

static void
free_layer(ignore_layer_t *l)
{
    for (size_t i = 0; i < l->n_rules; i++) free(l->rules[i].pattern);
    free(l->rules);
    free(l->base_dir);
}

void
ignore_stack_free(ignore_stack_t *st)
{
    for (size_t i = 0; i < st->n_layers; i++) free_layer(&st->layers[i]);
    free(st->layers);
    st->layers = NULL;
    st->n_layers = st->cap_layers = 0;
}

/* Append a rule to a layer, normalising the raw `.gitignore` line
 * into the structural fields we match against.  Returns false on a
 * blank/comment line so the caller can move on without touching the
 * `n_rules` counter. */
static bool
parse_line(ignore_layer_t *l, const char *raw)
{
    /* Strip leading whitespace.  Trailing whitespace is allowed by
     * git unless escaped; we follow git and strip plain trailing
     * spaces (tabs unaffected). */
    while (*raw == ' ' || *raw == '\t') raw++;
    if (*raw == '\0' || *raw == '#' || *raw == '\n' || *raw == '\r') return false;

    char buf[PATH_MAX];
    size_t n = strnlen(raw, sizeof(buf) - 1);
    memcpy(buf, raw, n);
    buf[n] = '\0';
    /* Trim trailing newline / unescaped trailing spaces. */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    while (n > 0 && buf[n - 1] == ' ' && (n < 2 || buf[n - 2] != '\\')) buf[--n] = '\0';
    if (n == 0) return false;

    char *p = buf;
    bool negate = false;
    if (*p == '!') { negate = true; p++; }

    bool dir_only = false;
    if (n > 0 && p[strlen(p) - 1] == '/') {
        dir_only = true;
        p[strlen(p) - 1] = '\0';
    }

    bool anchored = false;
    if (*p == '/') { anchored = true; p++; }

    /* Leading "**" + slash collapses to basename-anywhere — that's
     * already what a no-slash, no-leading-slash pattern means in
     * gitignore, so we just drop the prefix.  Mid-path "**" forms
     * (a slash, then ** then slash, e.g. src to test.rb at any
     * depth) aren't handled in v1 — too rare to justify the
     * path-segment matcher right now. */
    if (strncmp(p, "**/", 3) == 0) p += 3;

    bool has_slash = (strchr(p, '/') != NULL);

    if (l->n_rules == l->cap_rules) {
        l->cap_rules = l->cap_rules ? l->cap_rules * 2 : 16;
        l->rules = (ignore_rule_t *)realloc(l->rules, sizeof(*l->rules) * l->cap_rules);
        if (!l->rules) { perror("are: realloc"); exit(2); }
    }
    ignore_rule_t *r = &l->rules[l->n_rules++];
    r->pattern   = xstrdup(p);
    r->negate    = negate;
    r->dir_only  = dir_only;
    r->anchored  = anchored;
    r->has_slash = has_slash;
    return true;
}

/* Read .gitignore in `dir_path` (if it exists) into a layer.  Returns
 * true iff the layer was actually pushed (file existed and was
 * non-empty after parsing).  On any IO error we silently skip — git
 * does the same for unreadable files (you just lose those rules). */
static bool
load_layer(ignore_layer_t *out, const char *dir_path)
{
    char file_path[PATH_MAX];
    snprintf(file_path, sizeof(file_path), "%s/.gitignore", dir_path);
    FILE *fp = fopen(file_path, "r");
    if (!fp) return false;

    out->base_dir = xstrdup(dir_path);
    out->rules = NULL;
    out->n_rules = out->cap_rules = 0;

    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, fp) >= 0) parse_line(out, line);
    free(line);
    fclose(fp);

    if (out->n_rules == 0) {
        free(out->base_dir);
        return false;
    }
    return true;
}

static void
push_layer(ignore_stack_t *st, ignore_layer_t layer)
{
    if (st->n_layers == st->cap_layers) {
        st->cap_layers = st->cap_layers ? st->cap_layers * 2 : 8;
        st->layers = (ignore_layer_t *)realloc(st->layers, sizeof(*st->layers) * st->cap_layers);
        if (!st->layers) { perror("are: realloc"); exit(2); }
    }
    st->layers[st->n_layers++] = layer;
}

void
ignore_stack_push_dir(ignore_stack_t *st, const char *dir_path)
{
    if (st->disabled) return;
    ignore_layer_t l;
    if (load_layer(&l, dir_path)) push_layer(st, l);
}

void
ignore_stack_pop(ignore_stack_t *st)
{
    if (st->n_layers == 0) return;
    /* Layers are only pushed when load_layer succeeded; pops only
     * happen after a matching push (caller balances).  But the dir
     * walker doesn't know whether THIS dir contributed a layer, so
     * `ignore_should_skip` exposes a base_dir comparison to let the
     * walker decide.  See main.c. */
    free_layer(&st->layers[--st->n_layers]);
}

/* Walk up from `start_dir` looking for a directory that contains
 * either `.git/` or `.gitignore`.  As we go, stash any .gitignores
 * we pass.  When we find `.git`, that's the repo root — we stop
 * (don't go above it).  If we hit `/` first, we stop too — `start_dir`
 * isn't in a repo, but we still respect any .gitignores we passed. */
void
ignore_stack_seed_from_repo_root(ignore_stack_t *st, const char *start_dir)
{
    if (st->disabled) return;

    char abs[PATH_MAX];
    if (!realpath(start_dir, abs)) {
        if (!getcwd(abs, sizeof(abs))) return;
    }

    /* Stash layers as we walk up so we can push them in OUTERMOST-FIRST
     * order at the end (matches the lookup model: closer .gitignore wins). */
    char        *paths[256];
    int          n_paths = 0;
    bool         hit_git = false;

    /* Keep the per-iteration `probe` buffer larger than `cur` so the
     * compiler can prove the `%s/.gitignore` formatting can never
     * overflow (otherwise -O2 emits an overflow warning under
     * -D_FORTIFY_SOURCE). */
    char cur[PATH_MAX - 32];
    strncpy(cur, abs, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = '\0';

    while (n_paths < (int)(sizeof(paths) / sizeof(paths[0]))) {
        /* Is this a repo root?  `.git` may be a directory or a file
         * (worktree pointer); both count. */
        char probe[PATH_MAX];
        snprintf(probe, sizeof(probe), "%s/.git", cur);
        struct stat sb;
        if (stat(probe, &sb) == 0) hit_git = true;

        snprintf(probe, sizeof(probe), "%s/.gitignore", cur);
        if (access(probe, R_OK) == 0) paths[n_paths++] = xstrdup(cur);

        if (hit_git) break;

        /* Go up one. */
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) break;     /* reached "/" */
        *slash = '\0';
    }

    /* Push outermost-first — but layer-stack lookup goes innermost-
     * first, so reverse the order here.  At dir descent time, the
     * walker pushes the current dir's .gitignore on top, and that
     * one wins because it's checked first. */
    for (int i = n_paths - 1; i >= 0; i--) {
        ignore_stack_push_dir(st, paths[i]);
        free(paths[i]);
    }
}

/* Match one rule against a candidate.  `entry_rel` is the entry's
 * path RELATIVE to the layer's base_dir (no leading `/`); for
 * basename-only rules we also have `entry_basename` precomputed. */
static bool
rule_matches(const ignore_rule_t *r, const char *entry_basename,
             const char *entry_rel, bool is_dir)
{
    if (r->dir_only && !is_dir) return false;

    const char *target;
    int flags;
    if (r->anchored || r->has_slash) {
        /* Match against the layer-relative path.  FNM_PATHNAME so
         * `*` doesn't cross `/`. */
        target = entry_rel;
        flags  = FNM_PATHNAME;
    } else {
        /* Match against the basename only — git semantics for
         * "no slash anywhere" patterns. */
        target = entry_basename;
        flags  = 0;
    }
    return fnmatch(r->pattern, target, flags) == 0;
}

bool
ignore_should_skip(ignore_stack_t *st, const char *parent_dir,
                   const char *entry_basename, bool is_dir)
{
    /* `.git/` is always invisible to grep, regardless of --no-ignore. */
    if (is_dir && strcmp(entry_basename, ".git") == 0) return true;

    if (st->disabled) return false;

    /* Innermost layer first. */
    for (size_t li = st->n_layers; li-- > 0; ) {
        const ignore_layer_t *layer = &st->layers[li];

        /* Compute the entry's path relative to this layer's base.
         * If parent_dir doesn't start with base_dir, this layer's
         * scope doesn't cover us — skip it (shouldn't happen since
         * pushes are balanced, but defensive). */
        const size_t base_len = strlen(layer->base_dir);
        const size_t parent_len = strlen(parent_dir);
        const char *rel;
        char rel_buf[PATH_MAX];
        if (strncmp(parent_dir, layer->base_dir, base_len) == 0
            && (parent_dir[base_len] == '/' || parent_dir[base_len] == '\0')) {
            const char *suffix = parent_dir + base_len;
            if (*suffix == '/') suffix++;
            if (*suffix == '\0') {
                /* Entry is directly inside the layer. */
                rel = entry_basename;
            } else {
                snprintf(rel_buf, sizeof(rel_buf), "%s/%s", suffix, entry_basename);
                rel = rel_buf;
            }
        } else if (parent_len == base_len
                   && strcmp(parent_dir, layer->base_dir) == 0) {
            rel = entry_basename;
        } else {
            continue;
        }

        /* Within a single .gitignore, last matching rule wins → walk
         * rules in reverse order and return on the first hit. */
        for (size_t ri = layer->n_rules; ri-- > 0; ) {
            const ignore_rule_t *r = &layer->rules[ri];
            if (rule_matches(r, entry_basename, rel, is_dir)) {
                return !r->negate;
            }
        }
    }
    return false;
}
