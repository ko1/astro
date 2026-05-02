/*
 * astrogre — grep front-end on top of the astrogre regex engine.
 *
 * Usage:
 *   astrogre [options] PATTERN [FILE...]
 *   astrogre [options] -e PATTERN [-e PATTERN ...] [FILE...]
 *
 * Options (subset of GNU grep):
 *   -i  case-insensitive
 *   -n  print line number
 *   -c  count matching lines
 *   -v  invert (print non-matching lines)
 *   -w  whole word (wrap pattern in \b...\b)
 *   -F  fixed-string (literal pattern, no regex meta)
 *   -l  files-with-matches
 *   -L  files-without-match
 *   -H  always print filename
 *   -h  never print filename
 *   -o  print only matched parts (one per line)
 *   -r  recurse into directories
 *   --color=WHEN     never|always|auto (default auto via isatty)
 *   --backend=NAME   astrogre | onigmo
 *
 * Modes (selected via --mode=, take precedence over the grep flow):
 *   --self-test      run engine self-tests
 *   --bench          run engine microbench
 *   --dump           dump pattern AST (then exit)
 *   --regex-dump     same as --dump
 */

/* _GNU_SOURCE before any system header — needed for memrchr (used in
 * the whole-file scan path).  context.h would also set this, but the
 * system <string.h> below is included before context.h's transitive
 * includes get a chance, so the macro must be visible at this point. */
#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <fnmatch.h>
#include <ctype.h>
#include <time.h>
#include "node.h"
#include "context.h"
#include "parse.h"
#include "backend.h"

/* --verbose phase timing — captures wall-clock between key points
 * (INIT, mmap, scan, munmap, …) so users can see where each ms goes
 * without strace.  Activated by `--verbose`; the per-mark cost is one
 * clock_gettime (~30 ns) which is invisible in normal runs. */
static struct timespec g_verbose_t0;
static bool g_verbose = false;
static double g_verbose_last_ms = 0;
static void verbose_start(void) {
    clock_gettime(CLOCK_MONOTONIC, &g_verbose_t0);
}
static void verbose_mark(const char *tag) {
    if (!g_verbose) return;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    double ms = (t.tv_sec - g_verbose_t0.tv_sec) * 1000.0
              + (t.tv_nsec - g_verbose_t0.tv_nsec) / 1.0e6;
    fprintf(stderr, "[verbose] %-24s %8.3f ms  (+%.3f)\n",
            tag, ms, ms - g_verbose_last_ms);
    g_verbose_last_ms = ms;
}

/* Code-store API (defined by astro_code_store.c included from node.c) */
extern void astro_cs_compile(NODE *entry, const char *file);
extern void astro_cs_build(const char *extra_cflags);
extern void astro_cs_reload(void);
extern bool astro_cs_load(NODE *n, const char *file);

struct astrogre_option OPTION = {0};

int astrogre_run_self_tests(void);
int astrogre_run_microbench(void);

/* ------------------------------------------------------------------ */
/* Options                                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    CS_MODE_DEFAULT = 0,    /* try cs_load on each pattern; otherwise interp */
    CS_MODE_AOT_COMPILE,    /* cs_compile + cs_build + cs_reload + cs_load */
    CS_MODE_PLAIN,          /* skip the code store entirely */
} cs_mode_t;

typedef struct grep_opt {
    const char **patterns;
    int n_patterns;
    int patterns_cap;

    bool ignore_case;
    bool line_numbers;
    bool count_only;
    bool invert;
    bool word_match;
    bool line_regexp;           /* -x: pattern must match the whole line */
    bool fixed_string;
    bool files_with_matches;
    bool files_without_match;
    bool with_filename_force;
    bool no_filename_force;
    bool only_matching;
    bool quiet;                 /* -q: suppress output, exit code only */
    bool null_separator;        /* -Z: NUL terminate filenames in output */
    bool recursive;
    int  color_mode;            /* 0 never, 1 always, 2 auto */

    /* -m N: stop after N matching lines per file.  0 = unlimited. */
    long max_count;

    /* -A N / -B N / -C N: context lines.  Activates the per-line
     * streaming path (whole-file mmap fast path is bypassed) since
     * we need to remember recent lines for -B and emit lines after
     * a match for -A. */
    long before_context;
    long after_context;

    /* --include=GLOB / --exclude=GLOB: file-name globs filtering
     * which files -r descends into.  Multiple --include / --exclude
     * accumulate (all-include must be matched, no-exclude must NOT
     * be matched).  Compared by fnmatch(3) against basename only. */
    const char **include_globs;
    int n_include_globs;
    int include_globs_cap;
    const char **exclude_globs;
    int n_exclude_globs;
    int exclude_globs_cap;

    /* Code-store mode (astrogre backend only — Onigmo ignores it).  */
    cs_mode_t cs_mode;
    bool cs_verbose;            /* print cs_load hit/miss + cs_compile */

    const backend_ops_t *backend;
} grep_opt_t;

static void
push_pattern(grep_opt_t *go, const char *pat)
{
    if (go->n_patterns == go->patterns_cap) {
        go->patterns_cap = go->patterns_cap ? go->patterns_cap * 2 : 4;
        go->patterns = (const char **)realloc(go->patterns, sizeof(char *) * go->patterns_cap);
    }
    go->patterns[go->n_patterns++] = pat;
}

/* Read patterns from a file, one per line (-f FILE).  Empty lines are
 * skipped per GNU grep convention. */
static void
load_patterns_file(grep_opt_t *go, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "astrogre: %s: %s\n", path, strerror(errno));
        exit(2);
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, fp)) >= 0) {
        if (n > 0 && line[n - 1] == '\n') { line[--n] = '\0'; }
        if (n > 0 && line[n - 1] == '\r') { line[--n] = '\0'; }
        if (n == 0) continue;
        char *dup = strdup(line);
        push_pattern(go, dup);
    }
    free(line);
    fclose(fp);
}

static void
push_glob(const char ***arr, int *n, int *cap, const char *glob)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *arr = (const char **)realloc(*arr, sizeof(char *) * (size_t)*cap);
    }
    (*arr)[(*n)++] = glob;
}

static const backend_ops_t *
backend_by_name(const char *name)
{
    if (!name || !*name || strcmp(name, "astrogre") == 0) return &backend_astrogre_ops;
#ifdef USE_ONIGMO
    if (strcmp(name, "onigmo") == 0) return &backend_onigmo_ops;
#endif
    fprintf(stderr, "astrogre: unknown backend '%s'\n", name);
    exit(2);
}

/* ------------------------------------------------------------------ */
/* Compile pattern through the chosen backend                          */
/* ------------------------------------------------------------------ */

/* Escape regex metacharacters in a literal, so we can splice an
 * `-F` literal into a regex skeleton (-w / -x wrapping).  Caller-
 * sized buffer; returns the byte count written.  Worst case = 2× input. */
static size_t
escape_regex_meta(const char *pat, size_t len, char *out)
{
    char *p = out;
    for (size_t i = 0; i < len; i++) {
        char c = pat[i];
        if (strchr("\\.^$|()[]{}*+?-/", c)) *p++ = '\\';
        *p++ = c;
    }
    return (size_t)(p - out);
}

/* When -w (word-match) is requested, wrap the pattern in \b...\b at the
 * regex level.  When -x (line-regexp) is requested, wrap in \A...\z so
 * the pattern must consume the whole line (we feed lines to the matcher
 * separately so \A/\z are line bounds in this context).  Both can stack
 * (`-wx`).  -F + -w/-x escapes regex metacharacters in the literal first.
 * Returns malloc'd string, or NULL if no transformation needed. */
static char *
wrap_word(const char *pat, bool fixed)
{
    size_t len = strlen(pat);
    /* For -F we still need to escape regex metacharacters when wrapping. */
    size_t need = len + 6;
    if (fixed) need += len;  /* worst case every byte gets escaped */
    char *out = (char *)malloc(need);
    char *p = out;
    *p++ = '\\'; *p++ = 'b';
    if (fixed) {
        p += escape_regex_meta(pat, len, p);
    } else {
        memcpy(p, pat, len); p += len;
    }
    *p++ = '\\'; *p++ = 'b';
    *p = 0;
    return out;
}

/* -x: wrap the pattern as `\A(?:...)\z` so the matcher only succeeds
 * when the whole line is consumed.  -F + -x escapes regex metacharacters
 * first.  Returns malloc'd string. */
static char *
wrap_line(const char *pat, bool fixed)
{
    size_t len = strlen(pat);
    size_t need = len + 10;
    if (fixed) need += len;
    char *out = (char *)malloc(need);
    char *p = out;
    *p++ = '\\'; *p++ = 'A';
    *p++ = '(';  *p++ = '?'; *p++ = ':';
    if (fixed) {
        p += escape_regex_meta(pat, len, p);
    } else {
        memcpy(p, pat, len); p += len;
    }
    *p++ = ')';
    *p++ = '\\'; *p++ = 'z';
    *p = 0;
    return out;
}

static backend_pattern_t *
compile_pattern(grep_opt_t *go, const char *pat)
{
    backend_flags_t f = {
        .case_insensitive = go->ignore_case,
        .multiline        = false,
        .extended         = false,
        .fixed_string     = go->fixed_string,
    };
    char *wrapped = NULL;
    const char *use_pat = pat;
    bool use_fixed = go->fixed_string;
    /* -w then -x compose: word-bounded match, anchored to whole line.
     * `\b...\b` first, then `\A...\z` outside.  -F escapes literals
     * inside the inner wrap, the outer wrap is plain regex. */
    if (go->word_match) {
        wrapped = wrap_word(pat, go->fixed_string);
        use_pat = wrapped;
        use_fixed = false;       /* the wrap is regex syntax */
        f.fixed_string = false;
    }
    if (go->line_regexp) {
        char *outer = wrap_line(use_pat, use_fixed);
        if (wrapped) free(wrapped);
        wrapped = outer;
        use_pat = wrapped;
        use_fixed = false;
        f.fixed_string = false;
    }
    backend_pattern_t *bp = go->backend->compile(use_pat, strlen(use_pat), f);
    if (!bp) {
        fprintf(stderr, "astrogre: failed to compile pattern: %s\n", pat);
        if (wrapped) free(wrapped);
        return NULL;
    }
    (void)use_fixed;
    if (wrapped) free(wrapped);
    return bp;
}

/* ------------------------------------------------------------------ */
/* Color helpers                                                       */
/* ------------------------------------------------------------------ */

static bool color_active = false;

#define COLOR_RED    "\x1b[01;31m\x1b[K"
#define COLOR_PURPLE "\x1b[35m\x1b[K"
#define COLOR_GREEN  "\x1b[32m\x1b[K"
#define COLOR_RESET  "\x1b[m\x1b[K"

static void
color_init(int mode)
{
    color_active = false;
    if (mode == 1) color_active = true;
    else if (mode == 2) color_active = isatty(STDOUT_FILENO);
}

/* ------------------------------------------------------------------ */
/* Per-line / per-file matching                                        */
/* ------------------------------------------------------------------ */

typedef struct grep_state {
    grep_opt_t *go;
    backend_pattern_t **patterns;
    int n_patterns;
    bool show_filename;
    long total_match_count;
} grep_state_t;

/* Filename separator used after the filename / line-number prefix.
 * GNU grep's -Z swaps the trailing ':' for a NUL byte so the output
 * is safe to feed `xargs -0`. */
static char
fname_sep(grep_opt_t *go)
{
    return go->null_separator ? '\0' : ':';
}

/* Print one line, with --color insertion of N matches.  `line` is the
 * line bytes (not null-terminated; len excludes any trailing newline). */
static void
print_line_with_color(grep_state_t *st, const char *fname, long lineno,
                      const char *line, size_t len)
{
    grep_opt_t *go = st->go;
    char sep = fname_sep(go);
    if (st->show_filename) {
        if (color_active) printf(COLOR_PURPLE "%s" COLOR_RESET, fname);
        else              fputs(fname, stdout);
        fputc(sep, stdout);
    }
    if (go->line_numbers) {
        if (color_active) printf(COLOR_GREEN "%ld" COLOR_RESET, lineno);
        else              printf("%ld", lineno);
        fputc(sep, stdout);
    }

    if (!color_active) {
        fwrite(line, 1, len, stdout);
        fputc('\n', stdout);
        return;
    }

    /* Walk the line, splicing red around every match found by ANY pattern. */
    size_t pos = 0;
    while (pos <= len) {
        size_t best_start = (size_t)-1, best_end = 0;
        for (int i = 0; i < st->n_patterns; i++) {
            backend_match_t m;
            if (st->go->backend->search_from(st->patterns[i], line, len, pos, &m) && m.matched) {
                if (m.start < best_start) { best_start = m.start; best_end = m.end; }
            }
        }
        if (best_start == (size_t)-1) {
            fwrite(line + pos, 1, len - pos, stdout);
            break;
        }
        if (best_start > pos) fwrite(line + pos, 1, best_start - pos, stdout);
        fputs(COLOR_RED, stdout);
        fwrite(line + best_start, 1, best_end - best_start, stdout);
        fputs(COLOR_RESET, stdout);
        if (best_end == best_start) { /* zero-width — advance one byte */
            if (best_end < len) fputc(line[best_end], stdout);
            pos = best_end + 1;
        } else {
            pos = best_end;
        }
    }
    fputc('\n', stdout);
}

static void
print_only_matching(grep_state_t *st, const char *fname, long lineno,
                    const char *line, size_t len)
{
    grep_opt_t *go = st->go;
    size_t pos = 0;
    while (pos <= len) {
        size_t best_start = (size_t)-1, best_end = 0;
        for (int i = 0; i < st->n_patterns; i++) {
            backend_match_t m;
            if (st->go->backend->search_from(st->patterns[i], line, len, pos, &m) && m.matched) {
                if (m.start < best_start) { best_start = m.start; best_end = m.end; }
            }
        }
        if (best_start == (size_t)-1) break;
        if (st->show_filename) {
            if (color_active) printf(COLOR_PURPLE "%s" COLOR_RESET, fname);
            else              fputs(fname, stdout);
            fputc(fname_sep(st->go), stdout);
        }
        if (go->line_numbers) {
            if (color_active) printf(COLOR_GREEN "%ld" COLOR_RESET ":", lineno);
            else              printf("%ld:", lineno);
        }
        if (color_active) {
            fputs(COLOR_RED, stdout);
            fwrite(line + best_start, 1, best_end - best_start, stdout);
            fputs(COLOR_RESET, stdout);
        } else {
            fwrite(line + best_start, 1, best_end - best_start, stdout);
        }
        fputc('\n', stdout);
        if (best_end == best_start) pos = best_end + 1;
        else                        pos = best_end;
    }
}

/* Whole-file scan for the special case where every pattern is a
 * pure literal (cap_start → lit → cap_end → succ).  The single-pattern
 * count-only path goes through node_grep_count_lines_lit — an AST
 * node that owns the entire scan + verify + line-skip + count loop —
 * so the framework bakes the needle as immediate AVX2 operands and
 * the loop runs in one SD with no per-line dispatch.  Multi-pattern
 * and modes that need per-line printing fall through to a memmem-
 * earliest-match loop below. */
static bool
process_buffer_pure_literal(grep_state_t *st, const char *buf, size_t len,
                             const char *fname,
                             const char **needles, const size_t *needle_lens, int n)
{
    grep_opt_t *go = st->go;
    long matches_this_file = 0;
    bool any_match = false;
    long lineno = 0;
    size_t lineno_pos = 0;

    /* Tightest path: single pattern, count-only, no filename modes.
     * Goes through node_grep_count_lines_lit — an AST node that
     * folds the entire scan + verify + line-skip + count loop into
     * one SD via the framework.  The needle is baked as immediate
     * AVX2 operands at AOT-compile time (set1_epi8 of needle[0] /
     * needle[needle_len-1] for a Hyperscan-style dual-byte filter,
     * memcmp of the middle bytes lowered to direct cmpl/cmpb).
     * Same architectural trick as node_grep_search itself folding
     * the start-position search loop into the AST. */
    if (n == 1 && go->count_only
        && !go->files_with_matches && !go->files_without_match
        && go->max_count == 0          /* -m needs early break, not whole-buffer count */
        && !go->quiet                  /* -q needs early bail too */) {
        extern astrogre_pattern *astrogre_backend_pattern_get(backend_pattern_t *);
        astrogre_pattern *ap = astrogre_backend_pattern_get(st->patterns[0]);
        long c = ap ? astrogre_pattern_count_lines(ap, buf, len) : -1;
        if (c >= 0) {
            matches_this_file = c;
            any_match = c > 0;
            goto post_loop;
        }
        /* Fall through to the multi-pattern memmem loop below if the
         * count-lines node didn't apply (shouldn't happen here since
         * the caller already proved pure-literal, but the paranoid
         * fallback keeps the contract simple). */
    }

    /* Default print fast path: single pattern, no count-only, no
     * filename-only modes, no -o (only-matching) — use the
     * `node_scan_lit_dual_byte → count → emit_match_line → lineskip
     * → continue` AST chain for the same architectural win as `-c`.
     * The whole per-line scan + emit folds into one SD with the
     * needle baked as AVX2 immediates. */
    if (n == 1
        && !go->count_only
        && !go->files_with_matches
        && !go->files_without_match
        && !go->only_matching
        && !go->invert
        && !go->ignore_case
        && !go->quiet                  /* -q needs early bail */
        && go->max_count == 0          /* -m needs early break */
        && go->before_context == 0     /* context lines need ring buffer */
        && go->after_context == 0) {
        extern astrogre_pattern *astrogre_backend_pattern_get(backend_pattern_t *);
        astrogre_pattern *ap = astrogre_backend_pattern_get(st->patterns[0]);
        if (ap) {
            uint32_t emit_opts = 0;
            if (st->show_filename)  emit_opts |= ASTROGRE_EMIT_FNAME;
            if (go->line_numbers)   emit_opts |= ASTROGRE_EMIT_LINENO;
            if (color_active)       emit_opts |= ASTROGRE_EMIT_COLOR;
            long c = astrogre_pattern_print_lines(ap, buf, len, fname, stdout, emit_opts);
            if (c >= 0) {
                matches_this_file = c;
                any_match = c > 0;
                goto post_loop;
            }
        }
    }

    size_t pos = 0;
    while (pos < len) {
        size_t earliest_start = SIZE_MAX, earliest_end = 0;
        for (int i = 0; i < n; i++) {
            const char *q = (const char *)memmem(buf + pos, len - pos,
                                                  needles[i], needle_lens[i]);
            if (q) {
                size_t s = (size_t)(q - buf);
                if (s < earliest_start) {
                    earliest_start = s;
                    earliest_end = s + needle_lens[i];
                }
            }
        }
        if (earliest_start == SIZE_MAX) break;

        const char *line_end_ptr = (const char *)memchr(buf + earliest_start, '\n',
                                                          len - earliest_start);
        size_t line_end = line_end_ptr ? (size_t)(line_end_ptr - buf) : len;

        any_match = true;
        matches_this_file++;
        if (go->files_with_matches || go->files_without_match) break;
        if (go->quiet) break;

        if (!go->count_only) {
            const char *line_start_ptr = (earliest_start > 0)
                ? (const char *)memrchr(buf, '\n', earliest_start)
                : NULL;
            size_t line_start = line_start_ptr ? (size_t)(line_start_ptr - buf) + 1 : 0;
            /* lazy lineno tracking */
            const char *_lp = buf + lineno_pos;
            const char *_te = buf + line_start;
            while (_lp < _te) {
                const char *_nl = (const char *)memchr(_lp, '\n', _te - _lp);
                if (!_nl) break;
                lineno++; _lp = _nl + 1;
            }
            lineno_pos = line_start;
            lineno++;
            if (go->only_matching) {
                print_only_matching(st, fname, lineno,
                                    buf + line_start, line_end - line_start);
            } else {
                print_line_with_color(st, fname, lineno,
                                      buf + line_start, line_end - line_start);
            }
            lineno_pos = (line_end < len) ? line_end + 1 : line_end;
        }
        if (go->max_count > 0 && matches_this_file >= go->max_count) break;
        pos = (line_end < len) ? line_end + 1 : len + 1;
    }

post_loop:
    if (go->files_with_matches) {
        if (any_match) { fputs(fname, stdout); fputc(go->null_separator ? '\0' : '\n', stdout); }
    } else if (go->files_without_match) {
        if (!any_match) { fputs(fname, stdout); fputc(go->null_separator ? '\0' : '\n', stdout); }
    } else if (go->count_only) {
        if (st->show_filename) { fputs(fname, stdout); fputc(fname_sep(go), stdout); }
        printf("%ld\n", matches_this_file);
    }
    st->total_match_count += matches_this_file;
    return any_match;
}

/* Whole-file scan path.  Uses backend->search_from over the full
 * mmap'd buffer in a single CTX, identifies the line containing
 * each match via memrchr/memchr, and emits / counts.  This avoids
 * the per-line getline + per-call CTX-init overhead that bottle-
 * necks the streaming path; for literal-led patterns this is what
 * lets astrogre approach grep speed.
 *
 * Used only for default and -c modes — -v (invert) needs all
 * non-matching lines, and the streaming path's per-line model is
 * already the right fit there. */
static bool
process_buffer(grep_state_t *st, const char *buf, size_t len, const char *fname)
{
    grep_opt_t *go = st->go;
    long matches_this_file = 0;
    bool any_match = false;

    /* Lazy lineno tracking: we count newlines from `lineno_pos` to
     * the start of each printed line, on demand.  Avoids a full
     * pre-pass for files that have very few matches. */
    long lineno = 0;
    size_t lineno_pos = 0;
    #define ADVANCE_LINENO(target) do {                                \
        const char *_lp = buf + lineno_pos;                            \
        const char *_te = buf + (target);                              \
        while (_lp < _te) {                                            \
            const char *nl = (const char *)memchr(_lp, '\n', _te - _lp);\
            if (!nl) break;                                            \
            lineno++; _lp = nl + 1;                                    \
        }                                                              \
        lineno_pos = (target);                                         \
    } while (0)

    size_t pos = 0;
    while (pos <= len) {
        /* Find earliest match at or after pos across all patterns. */
        backend_match_t m;
        size_t earliest_start = SIZE_MAX, earliest_end = 0;
        bool found = false;
        for (int i = 0; i < st->n_patterns; i++) {
            if (st->go->backend->search_from(st->patterns[i], buf, len, pos, &m) && m.matched) {
                if (m.start < earliest_start) {
                    earliest_start = m.start;
                    earliest_end = m.end;
                    found = true;
                }
            }
        }
        if (!found) break;

        /* Line bounds. */
        const char *line_start_ptr = (earliest_start > 0)
            ? (const char *)memrchr(buf, '\n', earliest_start)
            : NULL;
        size_t line_start = line_start_ptr ? (size_t)(line_start_ptr - buf) + 1 : 0;
        const char *line_end_ptr = (const char *)memchr(buf + earliest_start, '\n',
                                                        len - earliest_start);
        size_t line_end = line_end_ptr ? (size_t)(line_end_ptr - buf) : len;

        any_match = true;
        matches_this_file++;
        if (go->files_with_matches || go->files_without_match) break;
        if (go->quiet) break;
        if (go->max_count > 0 && matches_this_file >= go->max_count) {
            /* Print this match (-m N includes the Nth) but stop after. */
            if (!go->count_only) {
                ADVANCE_LINENO(line_start);
                lineno++;
                if (go->only_matching) {
                    print_only_matching(st, fname, lineno,
                                        buf + line_start, line_end - line_start);
                } else {
                    print_line_with_color(st, fname, lineno,
                                          buf + line_start, line_end - line_start);
                }
            }
            break;
        }

        if (!go->count_only) {
            ADVANCE_LINENO(line_start);
            lineno++;        /* current line is one beyond any prior newline */
            if (go->only_matching) {
                print_only_matching(st, fname, lineno,
                                    buf + line_start, line_end - line_start);
            } else {
                print_line_with_color(st, fname, lineno,
                                      buf + line_start, line_end - line_start);
            }
            lineno_pos = line_end;
            if (line_end < len) { lineno_pos = line_end + 1; lineno++; }
            /* Already counted current line; -1 because the next
             * ADVANCE_LINENO will count newlines starting at
             * lineno_pos. */
            lineno--;
        }
        pos = (line_end < len) ? line_end + 1 : len + 1;
    }

    if (go->files_with_matches) {
        if (any_match) { fputs(fname, stdout); fputc(go->null_separator ? '\0' : '\n', stdout); }
    } else if (go->files_without_match) {
        if (!any_match) { fputs(fname, stdout); fputc(go->null_separator ? '\0' : '\n', stdout); }
    } else if (go->count_only) {
        if (st->show_filename) { fputs(fname, stdout); fputc(fname_sep(go), stdout); }
        printf("%ld\n", matches_this_file);
    }
    st->total_match_count += matches_this_file;
    return any_match;
}

/* Returns true if at least one match was found (regardless of -v).
 *
 * This path also implements -A/-B/-C context lines via a small ring
 * buffer of recent lines.  Layout: when a match is hit, emit the
 * preceding `before_context` lines, the matching line, and then the
 * next `after_context` non-matching lines.  Adjacent match groups
 * are separated by a `--` line (matches GNU grep's default). */
static bool
process_stream(grep_state_t *st, FILE *fp, const char *fname)
{
    grep_opt_t *go = st->go;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    long lineno = 0;
    long matches_this_file = 0;
    bool any_match = false;

    /* Ring buffer for -B (before-context).  Allocated lazily and only
     * when before_context > 0.  Each slot owns its own malloc'd line. */
    long bsize = go->before_context > 0 ? go->before_context : 0;
    char **bring = bsize ? (char **)calloc((size_t)bsize, sizeof(char *)) : NULL;
    long *bring_lineno = bsize ? (long *)calloc((size_t)bsize, sizeof(long)) : NULL;
    size_t *bring_llen = bsize ? (size_t *)calloc((size_t)bsize, sizeof(size_t)) : NULL;
    long bring_head = 0;        /* next slot to overwrite */
    long bring_count = 0;       /* current valid entries (≤ bsize) */

    long after_remaining = 0;   /* -A: lines still to emit after a match */
    long last_emitted_lineno = 0;  /* for the `--` separator */
    bool need_separator = false;

    while ((n = getline(&line, &cap, fp)) != -1) {
        lineno++;
        size_t llen = (size_t)n;
        if (llen > 0 && line[llen - 1] == '\n') llen--;

        bool any = false;
        for (int i = 0; i < st->n_patterns; i++) {
            backend_match_t m;
            if (st->go->backend->search(st->patterns[i], line, llen, &m) && m.matched) {
                any = true; break;
            }
        }
        bool keep = go->invert ? !any : any;

        if (keep) {
            matches_this_file++;
            any_match = true;
            if (go->files_with_matches || go->files_without_match) break;
            if (go->quiet) break;
            if (go->count_only) {
                if (go->max_count > 0 && matches_this_file >= go->max_count) break;
                continue;
            }

            /* -B: flush the buffered preceding lines. */
            if (bsize > 0 && bring_count > 0) {
                if (need_separator && last_emitted_lineno > 0) {
                    long earliest = bring_lineno[(bring_head - bring_count + bsize) % bsize];
                    if (earliest > last_emitted_lineno + 1) fputs("--\n", stdout);
                }
                for (long k = 0; k < bring_count; k++) {
                    long idx = (bring_head - bring_count + k + bsize) % bsize;
                    print_line_with_color(st, fname, bring_lineno[idx],
                                          bring[idx], bring_llen[idx]);
                }
                bring_count = 0;
            } else if (need_separator && lineno > last_emitted_lineno + 1) {
                fputs("--\n", stdout);
            }

            /* Emit this matching line. */
            if (go->only_matching && !go->invert) {
                print_only_matching(st, fname, lineno, line, llen);
            } else {
                print_line_with_color(st, fname, lineno, line, llen);
            }
            last_emitted_lineno = lineno;
            after_remaining = go->after_context;
            need_separator = (go->before_context > 0 || go->after_context > 0);

            if (go->max_count > 0 && matches_this_file >= go->max_count) {
                /* Continue only as long as -A still wants lines. */
                if (after_remaining == 0) break;
            }
        } else if (after_remaining > 0) {
            /* -A: trailing context after a match. */
            print_line_with_color(st, fname, lineno, line, llen);
            last_emitted_lineno = lineno;
            after_remaining--;
            if (go->max_count > 0 && matches_this_file >= go->max_count
                && after_remaining == 0) break;
        } else if (bsize > 0) {
            /* Stash into the before-context ring buffer. */
            long slot = bring_head;
            free(bring[slot]);
            bring[slot] = (char *)malloc(llen);
            if (bring[slot]) memcpy(bring[slot], line, llen);
            bring_llen[slot] = llen;
            bring_lineno[slot] = lineno;
            bring_head = (bring_head + 1) % bsize;
            if (bring_count < bsize) bring_count++;
        }
    }
    free(line);
    if (bring) {
        for (long i = 0; i < bsize; i++) free(bring[i]);
        free(bring); free(bring_lineno); free(bring_llen);
    }

    if (go->files_with_matches) {
        if (any_match) { fputs(fname, stdout); fputc(go->null_separator ? '\0' : '\n', stdout); }
    } else if (go->files_without_match) {
        if (!any_match) { fputs(fname, stdout); fputc(go->null_separator ? '\0' : '\n', stdout); }
    } else if (go->count_only) {
        if (st->show_filename) { fputs(fname, stdout); fputc(fname_sep(go), stdout); }
        printf("%ld\n", matches_this_file);
    }
    st->total_match_count += matches_this_file;
    return any_match;
}

static int
process_file(grep_state_t *st, const char *path)
{
    /* Whole-file mmap path is preferred for regular files when:
     *   - the mode supports it (no -v invert — that needs to
     *     enumerate every line, including non-matching ones), and
     *   - the backend's pattern has a fast scan primitive (memchr /
     *     memmem / SIMD class scan).  For plain backtracking
     *     patterns (\w-led, /i without prefilter, etc.) the per-
     *     line streaming loop wins because each line is short and
     *     a single 36-byte naive scan is faster than a whole-file
     *     naive sweep.  has_fast_scan == NULL on the backend means
     *     "always yes" (Onigmo has its own internal optimisation). */
    grep_opt_t *go = st->go;
    bool fast = true;
    if (go->backend->has_fast_scan) {
        fast = false;
        for (int i = 0; i < st->n_patterns; i++) {
            if (go->backend->has_fast_scan(st->patterns[i])) { fast = true; break; }
        }
    }

    /* Pure-literal short-circuit: when *every* pattern is a single
     * literal with no other side-effects, bypass the engine and run
     * a tight memmem loop.  Saves the per-call CTX init + body chain
     * dispatch on every match. */
    extern astrogre_pattern *astrogre_backend_pattern_get(backend_pattern_t *);
    const char *pl_needles[16];
    size_t pl_needle_lens[16];
    /* Pure-literal mmap fast paths require the matcher to operate on
     * the whole file buffer.  -x wraps the pattern in \A...\z which
     * only fires at file boundaries (we want LINE boundaries), so it
     * must drop to the per-line streaming path.  Same for context
     * options that need a ring buffer of recent lines. */
    bool all_pure_literal = !go->invert && go->backend == &backend_astrogre_ops
                          && st->n_patterns > 0 && st->n_patterns <= 16
                          && !go->line_regexp
                          && go->before_context == 0
                          && go->after_context == 0;
    if (all_pure_literal) {
        for (int i = 0; i < st->n_patterns; i++) {
            astrogre_pattern *ap = astrogre_backend_pattern_get(st->patterns[i]);
            if (!ap || !astrogre_pattern_pure_literal(ap, &pl_needles[i], &pl_needle_lens[i])) {
                all_pure_literal = false; break;
            }
        }
    }
    if (all_pure_literal) {
        verbose_mark("before mmap");
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            struct stat sb;
            if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size > 0) {
                /* MAP_POPULATE pre-faults the page tables in the kernel
                 * — saves the per-page minor-fault cost when we then scan
                 * the whole file linearly.  GNU grep does the same.
                 * (Tried MADV_HUGEPAGE here: file-backed THP isn't
                 * enabled on the test kernel and the madvise actually
                 * slowed the populate slightly, so we don't issue it.) */
                void *map = mmap(NULL, (size_t)sb.st_size, PROT_READ,
                                  MAP_PRIVATE | MAP_POPULATE, fd, 0);
                verbose_mark("after mmap");
                if (map != MAP_FAILED) {
                    process_buffer_pure_literal(st, (const char *)map, (size_t)sb.st_size,
                                                 path, pl_needles, pl_needle_lens, st->n_patterns);
                    verbose_mark("after scan");
                    /* munmap explicitly so --verbose reports honestly.
                     * Skipping it shifts ~5 ms to kernel exit-time
                     * teardown (which happens after our last
                     * verbose_mark and is invisible to it) but doesn't
                     * actually shrink wall-time — the kernel pays the
                     * same PTE-teardown cost either way. */
                    munmap(map, (size_t)sb.st_size);
                    verbose_mark("after munmap");
                    close(fd);
                    return 0;
                }
            }
            close(fd);
        }
    }

    /* Fall through to streaming when context lines are requested
     * (the mmap path's per-match search loop doesn't carry the
     * before-buffer / after-counter that -A/-B/-C need), or when
     * -x is set (the \A...\z wrap only matches at FILE boundaries
     * if the matcher sees the whole buffer; we want LINE boundaries). */
    if (!go->invert && fast
        && go->before_context == 0 && go->after_context == 0
        && !go->line_regexp) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "astrogre: %s: %s\n", path, strerror(errno));
            return 2;
        }
        struct stat sb;
        if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size > 0) {
            void *map = mmap(NULL, (size_t)sb.st_size, PROT_READ,
                              MAP_PRIVATE | MAP_POPULATE, fd, 0);
            if (map != MAP_FAILED) {
                process_buffer(st, (const char *)map, (size_t)sb.st_size, path);
                munmap(map, (size_t)sb.st_size);
                close(fd);
                return 0;
            }
        }
        close(fd);
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "astrogre: %s: %s\n", path, strerror(errno));
        return 2;
    }
    process_stream(st, fp, path);
    fclose(fp);
    return 0;
}

/* True iff `name` (basename only) passes the --include / --exclude
 * filters.  --include: when set, name must match at least one.
 * --exclude: when matched, the entry is dropped. */
static bool
glob_filter_pass(grep_opt_t *go, const char *name, bool is_dir)
{
    /* Excludes apply to both files and directories — useful for
     * `--exclude=node_modules` in -r. */
    for (int i = 0; i < go->n_exclude_globs; i++) {
        if (fnmatch(go->exclude_globs[i], name, 0) == 0) return false;
    }
    /* Includes apply only to files; directories always descended. */
    if (!is_dir && go->n_include_globs > 0) {
        bool ok = false;
        for (int i = 0; i < go->n_include_globs; i++) {
            if (fnmatch(go->include_globs[i], name, 0) == 0) { ok = true; break; }
        }
        if (!ok) return false;
    }
    return true;
}

static int
process_path(grep_state_t *st, const char *path)
{
    /* -q: once we've seen a match, the exit code is decided.  Bail
     * before we touch any more files. */
    if (st->go->quiet && st->total_match_count > 0) return 0;

    struct stat sb;
    if (stat(path, &sb) != 0) {
        fprintf(stderr, "astrogre: %s: %s\n", path, strerror(errno));
        return 2;
    }
    if (S_ISDIR(sb.st_mode)) {
        if (!st->go->recursive) {
            fprintf(stderr, "astrogre: %s: Is a directory\n", path);
            return 2;
        }
        DIR *d = opendir(path);
        if (!d) { fprintf(stderr, "astrogre: %s: %s\n", path, strerror(errno)); return 2; }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            if (e->d_name[0] == '.') continue;  /* skip dotfiles */
            if (!glob_filter_pass(st->go, e->d_name, e->d_type == DT_DIR)) continue;
            char sub[4096];
            snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
            process_path(st, sub);
            if (st->go->quiet && st->total_match_count > 0) break;
        }
        closedir(d);
        return 0;
    }
    return process_file(st, path);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static void
usage(void)
{
    fputs(
        "astrogre — grep on top of the astrogre regex engine\n"
        "Usage: astrogre [options] PATTERN [FILE...]\n"
        "       astrogre [options] -e PATTERN [-e PAT...] [FILE...]\n"
        "Options:\n"
        "  -i  case-insensitive            -n  print line numbers\n"
        "  -c  count                       -v  invert match\n"
        "  -w  whole word                  -x  match whole line\n"
        "  -F  fixed string                -q  quiet (exit code only)\n"
        "  -l  files with matches          -L  files without\n"
        "  -H  always show filename        -h  never show filename\n"
        "  -o  only matching parts         -r  recursive\n"
        "  -Z  NUL-separated filenames     -m N  stop after N matches/file\n"
        "  -A N / -B N / -C N              context lines (after/before/both)\n"
        "  -e  PATTERN (repeatable)        -f  read patterns from FILE\n"
        "  --include=GLOB / --exclude=GLOB filter for -r\n"
        "  --color=never|always|auto       --backend=astrogre|onigmo\n"
        "  -C, --aot-compile               specialize patterns to code_store/ then run\n"
        "  --plain, --no-cs                bypass code store entirely\n"
        "  --cs-verbose                    log cs_load / cs_compile activity\n"
        "  --verbose                       phase-by-phase wall-clock timing on stderr\n"
        "Modes:\n"
        "  --self-test                     --bench\n"
        "  --dump PATTERN\n",
        stderr);
}

int
main(int argc, char *argv[])
{
    verbose_start();
    /* Pre-scan for --verbose so timing covers INIT() too. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) { g_verbose = true; break; }
    }
    verbose_mark("main entry");
    INIT();
    verbose_mark("after INIT()");
    grep_opt_t go = {0};
    go.color_mode = 2;
    go.backend = &backend_astrogre_ops;

    int argi = 1;

    /* First-pass scan for explicit modes.  These short-circuit the
     * grep flow entirely. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--self-test") == 0) return astrogre_run_self_tests();
        if (strcmp(argv[i], "--bench") == 0)     return astrogre_run_microbench();
        if (strcmp(argv[i], "--bench-file") == 0) {
            extern int astrogre_run_file_bench(const char *file, const char *pat,
                                                int iters, bool aot, bool plain);
            if (i + 2 >= argc) {
                fprintf(stderr, "usage: astrogre --bench-file FILE PATTERN [ITERS] [--aot|--plain]\n");
                return 2;
            }
            int iters = (i + 3 < argc && argv[i+3][0] != '-') ? atoi(argv[i+3]) : 50;
            bool aot = false, plain = false;
            for (int j = i + 1; j < argc; j++) {
                if (strcmp(argv[j], "--aot") == 0) aot = true;
                else if (strcmp(argv[j], "--plain") == 0) plain = true;
            }
            return astrogre_run_file_bench(argv[i+1], argv[i+2], iters, aot, plain);
        }
    }

    /* Parse flags. */
    while (argi < argc) {
        const char *a = argv[argi];
        if (a[0] != '-' || strcmp(a, "-") == 0) break;
        if (strcmp(a, "--") == 0) { argi++; break; }

        if (strcmp(a, "--dump") == 0 || strcmp(a, "--regex-dump") == 0) {
            if (argi + 1 >= argc) { usage(); return 2; }
            astrogre_pattern *p = astrogre_parse_literal(argv[argi + 1], strlen(argv[argi + 1]));
            if (!p) return 2;
            DUMP(stdout, p->root, true);
            printf("\n");
            astrogre_pattern_free(p);
            return 0;
        }
        if (strncmp(a, "--color", 7) == 0) {
            const char *val = (a[7] == '=') ? a + 8 : "always";
            if (strcmp(val, "never") == 0)       go.color_mode = 0;
            else if (strcmp(val, "always") == 0) go.color_mode = 1;
            else                                  go.color_mode = 2;
            argi++; continue;
        }
        if (strncmp(a, "--backend=", 10) == 0) {
            go.backend = backend_by_name(a + 10);
            argi++; continue;
        }
        if (strcmp(a, "--aot-compile") == 0) {
            go.cs_mode = CS_MODE_AOT_COMPILE;
            argi++; continue;
        }
        if (strcmp(a, "-C") == 0) {
            /* GNU grep's -C is `--context=NUM`; ours doubles as
             * --aot-compile when no numeric arg follows.  Disambiguate
             * by peeking at the next argv. */
            if (argi + 1 < argc && isdigit((unsigned char)argv[argi + 1][0])) {
                long n = strtol(argv[argi + 1], NULL, 10);
                go.before_context = n;
                go.after_context = n;
                argi += 2;
            } else {
                go.cs_mode = CS_MODE_AOT_COMPILE;
                argi++;
            }
            continue;
        }
        if (strcmp(a, "--pg-compile") == 0 || strcmp(a, "-P") == 0) {
            /* For v1 we do not have a meaningful profile signal for
             * regex matching — `Hopt == Horg` in node.h, so the baked
             * SD bytes are the same as `--aot-compile` would produce.
             * The flag is accepted to match abruby's CLI shape; warn
             * once so the user understands the equivalence. */
            fprintf(stderr, "astrogre: --pg-compile: no profile signal yet, behaves as --aot-compile\n");
            go.cs_mode = CS_MODE_AOT_COMPILE;
            argi++; continue;
        }
        if (strcmp(a, "--plain") == 0 || strcmp(a, "--no-cs") == 0) {
            go.cs_mode = CS_MODE_PLAIN;
            argi++; continue;
        }
        if (strcmp(a, "--cs-verbose") == 0) {
            go.cs_verbose = true;
            argi++; continue;
        }
        if (strcmp(a, "--verbose") == 0) {
            /* g_verbose is already set in the pre-scan in main(); we
             * still consume the flag here so it doesn't get treated
             * as a pattern. */
            argi++; continue;
        }
        if (strcmp(a, "-e") == 0) {
            if (argi + 1 >= argc) { usage(); return 2; }
            push_pattern(&go, argv[argi + 1]);
            argi += 2; continue;
        }
        if (strcmp(a, "-f") == 0) {
            if (argi + 1 >= argc) { usage(); return 2; }
            load_patterns_file(&go, argv[argi + 1]);
            argi += 2; continue;
        }
        if (strcmp(a, "-m") == 0) {
            if (argi + 1 >= argc) { usage(); return 2; }
            go.max_count = strtol(argv[argi + 1], NULL, 10);
            argi += 2; continue;
        }
        if (strcmp(a, "-A") == 0) {
            if (argi + 1 >= argc) { usage(); return 2; }
            go.after_context = strtol(argv[argi + 1], NULL, 10);
            argi += 2; continue;
        }
        if (strcmp(a, "-B") == 0) {
            if (argi + 1 >= argc) { usage(); return 2; }
            go.before_context = strtol(argv[argi + 1], NULL, 10);
            argi += 2; continue;
        }
        if (strncmp(a, "--include=", 10) == 0) {
            push_glob(&go.include_globs, &go.n_include_globs, &go.include_globs_cap, a + 10);
            argi++; continue;
        }
        if (strncmp(a, "--exclude=", 10) == 0) {
            push_glob(&go.exclude_globs, &go.n_exclude_globs, &go.exclude_globs_cap, a + 10);
            argi++; continue;
        }
        if (a[0] == '-' && a[1] != '-' && a[1] != 0) {
            for (const char *q = a + 1; *q; q++) {
                switch (*q) {
                case 'i': go.ignore_case = true; break;
                case 'n': go.line_numbers = true; break;
                case 'c': go.count_only = true; break;
                case 'v': go.invert = true; break;
                case 'w': go.word_match = true; break;
                case 'F': go.fixed_string = true; break;
                case 'l': go.files_with_matches = true; break;
                case 'L': go.files_without_match = true; break;
                case 'H': go.with_filename_force = true; break;
                case 'h': go.no_filename_force = true; break;
                case 'o': go.only_matching = true; break;
                case 'r': go.recursive = true; break;
                case 'q': go.quiet = true; break;
                case 'x': go.line_regexp = true; break;
                case 'Z': go.null_separator = true; break;
                case 'E': /* compatibility no-op (we always use Ruby regex) */ break;
                case 's': /* suppress error msgs — not yet */ break;
                default:
                    fprintf(stderr, "astrogre: unknown option -%c\n", *q);
                    usage(); return 2;
                }
            }
            argi++; continue;
        }
        fprintf(stderr, "astrogre: unknown option %s\n", a);
        usage(); return 2;
    }

    /* Code-store options — apply to the astrogre backend only.  Onigmo
     * gets all matches done in its own engine. */
    if (go.cs_mode == CS_MODE_PLAIN) OPTION.no_compiled_code = true;
    if (go.cs_verbose) OPTION.cs_verbose = true;

    /* If no -e patterns given, the next positional is the pattern. */
    if (go.n_patterns == 0) {
        if (argi >= argc) { usage(); return 2; }
        push_pattern(&go, argv[argi++]);
    }

    color_init(go.color_mode);

    /* Compile patterns. */
    backend_pattern_t **bps = (backend_pattern_t **)calloc(go.n_patterns, sizeof(*bps));
    for (int i = 0; i < go.n_patterns; i++) {
        bps[i] = compile_pattern(&go, go.patterns[i]);
        if (!bps[i]) return 2;
    }
    verbose_mark("after pattern compile");

    /* AOT compile mode: ask the backend to specialize each pattern
     * before any input is processed.  Onigmo's `.aot_compile` is NULL
     * and we silently skip it there. */
    if (go.cs_mode == CS_MODE_AOT_COMPILE && go.backend->aot_compile) {
        for (int i = 0; i < go.n_patterns; i++) {
            go.backend->aot_compile(bps[i], go.cs_verbose);
        }
        verbose_mark("after AOT compile");
    }

    grep_state_t st = {0};
    st.go = &go;
    st.patterns = bps;
    st.n_patterns = go.n_patterns;

    /* Decide if the filename column is shown. */
    int n_files = argc - argi;
    if (go.no_filename_force) st.show_filename = false;
    else if (go.with_filename_force) st.show_filename = true;
    else st.show_filename = (n_files > 1) || go.recursive;

    int rc = 1;
    if (n_files == 0) {
        process_stream(&st, stdin, "(standard input)");
        rc = (st.total_match_count > 0) ? 0 : 1;
    } else {
        for (int i = argi; i < argc; i++) {
            process_path(&st, argv[i]);
            /* -q: stop iterating files as soon as we know the answer. */
            if (go.quiet && st.total_match_count > 0) break;
        }
        rc = (st.total_match_count > 0) ? 0 : 1;
    }

    if (go.cs_verbose) {
        extern void astrogre_cs_stats(void);
        astrogre_cs_stats();
    }

    for (int i = 0; i < go.n_patterns; i++) go.backend->free(bps[i]);
    free(bps);
    free(go.patterns);
    verbose_mark("at exit");
    return rc;
}
