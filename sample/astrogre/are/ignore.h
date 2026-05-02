/*
 * are — .gitignore-style filtering for the recursive walker.
 *
 * The matcher answers one question for every dirent entry the walker
 * sees: "should I skip this name?".  It's stack-shaped: each opendir
 * pushes the local `.gitignore` (if any) onto the stack; each
 * closedir pops it back off.  Lookups walk the stack from innermost
 * to outermost — the closest .gitignore wins, in line with git's own
 * semantics.  Within a single .gitignore, the LAST matching rule
 * decides; that's why the matcher walks rules in reverse insertion
 * order and short-circuits.
 *
 * What's covered:
 *   - basename and trailing-slash patterns (`foo`, `*.log`, `build/`)
 *   - leading-slash anchored patterns (`/.envrc`, `/dist/`)
 *   - leading double-star "any depth" prefix
 *   - negation with `!pattern`
 *   - comments (`#…`) and blank lines
 *
 * What we deliberately skip in v1:
 *   - global gitignore (`core.excludesFile`)
 *   - `.git/info/exclude`
 *   - mid-path double-star (e.g. src/[double-star]/test.rb)
 *   - `.ignore` / `.rgignore` (one ignore source is enough; gitignore
 *     coverage on real codebases is nearly universal already).
 *
 * `.git/` itself is hard-coded to be skipped at the dirent level —
 * even with `--no-ignore` we don't recurse into it (no sane grep
 * use-case wants to scan packfiles).
 */

#ifndef ARE_IGNORE_H
#define ARE_IGNORE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct ignore_rule {
    char  *pattern;          /* allocated, no trailing newline / slash */
    bool   negate;           /* leading `!` — un-ignores a previously matched name */
    bool   dir_only;         /* trailing `/` — only matches directories */
    bool   anchored;         /* leading `/` — only matches at the dir of this .gitignore */
    bool   has_slash;        /* pattern contains `/` (excluding leading/trailing) — matched against rel-path, not basename */
} ignore_rule_t;

typedef struct ignore_layer {
    /* Directory this `.gitignore` lived in, as an absolute or
     * walker-relative path with no trailing slash.  Used to compute
     * the entry's path RELATIVE to this layer when matching anchored
     * or slash-bearing rules. */
    char           *base_dir;
    ignore_rule_t  *rules;
    size_t          n_rules;
    size_t          cap_rules;
} ignore_layer_t;

typedef struct ignore_stack {
    ignore_layer_t *layers;
    size_t          n_layers;
    size_t          cap_layers;
    bool            disabled;     /* --no-ignore */
} ignore_stack_t;

/* Initialise an empty stack.  Pass `disabled=true` from the CLI's
 * `--no-ignore` to make every match query return "not ignored". */
void  ignore_stack_init(ignore_stack_t *st, bool disabled);
void  ignore_stack_free(ignore_stack_t *st);

/* Find the enclosing git repo root by walking up from `start_dir`
 * looking for a `.git` directory; for each .gitignore encountered on
 * the way up (root included), push it onto the stack so descendant
 * lookups inherit them.  Safe to call on a non-repo path — the
 * function just walks up to `/` finding nothing.
 *
 * `start_dir` may be relative (e.g. "."); we resolve via getcwd. */
void  ignore_stack_seed_from_repo_root(ignore_stack_t *st, const char *start_dir);

/* Push the .gitignore in `dir_path` (if it exists) onto the stack.
 * `dir_path` is duped internally.  No-op when the file isn't there.
 * No-op when the stack is `disabled`. */
void  ignore_stack_push_dir(ignore_stack_t *st, const char *dir_path);

/* Pop one layer.  Caller balances push/pop around opendir/closedir. */
void  ignore_stack_pop(ignore_stack_t *st);

/* Return true iff `entry_basename` inside `parent_dir` should be
 * skipped given the current stack.  `is_dir` matters for trailing-
 * slash ("dir-only") rules.  When the stack is `disabled`, only
 * `.git/` itself (which the walker also hard-codes) returns true. */
bool  ignore_should_skip(ignore_stack_t *st, const char *parent_dir,
                         const char *entry_basename, bool is_dir);

#endif  /* ARE_IGNORE_H */
