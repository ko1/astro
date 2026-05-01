#ifndef ASTROGRE_BACKEND_H
#define ASTROGRE_BACKEND_H 1

/*
 * Backend abstraction for the regex matcher.
 *
 * The grep CLI talks only to this interface; concrete implementations
 * live in backend_astrogre.c (the in-house engine) and backend_onigmo.c
 * (Onigmo via onigmo.h, gated on USE_ONIGMO).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct backend_pattern backend_pattern_t;

typedef enum {
    BACKEND_ASTROGRE = 0,
    BACKEND_ONIGMO   = 1,
} backend_kind_t;

typedef struct backend_match {
    bool matched;
    size_t start;   /* byte offset of overall match */
    size_t end;
} backend_match_t;

typedef struct backend_flags {
    bool case_insensitive;
    bool multiline;
    bool extended;
    bool fixed_string;   /* -F */
} backend_flags_t;

/* Backend ops table — populated per implementation, picked at runtime. */
typedef struct backend_ops {
    const char *name;
    backend_pattern_t *(*compile)(const char *pat, size_t len, backend_flags_t flags);
    bool               (*search)  (backend_pattern_t *p, const char *str, size_t len, backend_match_t *out);
    bool               (*search_from)(backend_pattern_t *p, const char *str, size_t len, size_t start, backend_match_t *out);
    void               (*free)    (backend_pattern_t *p);
    /* Optional: drive AOT-compile of this pattern.  NULL means the
     * backend doesn't participate in ASTro's code store (Onigmo). */
    void               (*aot_compile)(backend_pattern_t *p, bool verbose);
} backend_ops_t;

extern const backend_ops_t backend_astrogre_ops;
#ifdef USE_ONIGMO
extern const backend_ops_t backend_onigmo_ops;
#endif

#endif
