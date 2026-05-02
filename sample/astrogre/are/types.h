/*
 * are — file-type filter table.
 *
 * Maps a short language name (`-t rust`, `-t py`) to a list of glob
 * patterns matched against the basename of each file the recursive
 * walker visits.  Patterns are POSIX `fnmatch(3)` (no shell globstar
 * expansion needed — we only ever match a single basename).
 *
 * To add a type, append to `ARE_TYPE_TABLE` in types.c.  The table is
 * a static array — no runtime registration cost.  User-defined types
 * can be added at runtime via `--type-add NAME:GLOB[:GLOB ...]`.
 */

#ifndef ARE_TYPES_H
#define ARE_TYPES_H

#include <stdbool.h>
#include <stddef.h>

typedef struct are_type {
    const char  *name;        /* short, lowercase, hyphen-free */
    const char **globs;       /* NULL-terminated */
} are_type_t;

/* Return the type with this name, or NULL if unknown. */
const are_type_t *are_type_find(const char *name);

/* Iterate all known types in stable order. */
const are_type_t *are_type_at(size_t i);
size_t            are_type_count(void);

/* Add a user-defined type (`--type-add foo:*.foo:Foofile`).
 * `spec` is a NUL-terminated string the function copies; ownership of
 * the copy stays inside the registry. */
void              are_type_add(const char *spec);

/* True iff `basename` matches at least one glob of `t`. */
bool              are_type_matches(const are_type_t *t, const char *basename);

#endif  /* ARE_TYPES_H */
