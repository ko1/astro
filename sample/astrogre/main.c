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
 *   --via-prism      parse PATTERN as Ruby source via prism, take the
 *                    first regex literal as the search pattern
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
#include "node.h"
#include "context.h"
#include "parse.h"
#include "backend.h"

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
    bool fixed_string;
    bool files_with_matches;
    bool files_without_match;
    bool with_filename_force;
    bool no_filename_force;
    bool only_matching;
    bool recursive;
    int  color_mode;            /* 0 never, 1 always, 2 auto */
    bool via_prism;

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

/* When -w (word-match) is requested, wrap the pattern in \b...\b at the
 * regex level.  -F + -w wraps the literal in a non-capturing-style
 * pattern that we still build via the regex parser.  Returns malloc'd
 * string, or NULL if no transformation needed. */
static char *
wrap_word(const char *pat, bool fixed)
{
    size_t len = strlen(pat);
    /* For -F we still need to escape regex metacharacters when wrapping.
     * Simpler: when both -F and -w are given, emit the regex parser
     * route with each metacharacter escaped. */
    size_t need = len + 6;
    if (fixed) need += len;  /* worst case every byte gets escaped */
    char *out = (char *)malloc(need);
    char *p = out;
    *p++ = '\\'; *p++ = 'b';
    if (fixed) {
        for (size_t i = 0; i < len; i++) {
            char c = pat[i];
            if (strchr("\\.^$|()[]{}*+?-/", c)) *p++ = '\\';
            *p++ = c;
        }
    } else {
        memcpy(p, pat, len); p += len;
    }
    *p++ = '\\'; *p++ = 'b';
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
    if (go->word_match) {
        wrapped = wrap_word(pat, go->fixed_string);
        use_pat = wrapped;
        use_fixed = false;       /* the wrap is regex syntax */
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

/* Print one line, with --color insertion of N matches.  `line` is the
 * line bytes (not null-terminated; len excludes any trailing newline). */
static void
print_line_with_color(grep_state_t *st, const char *fname, long lineno,
                      const char *line, size_t len)
{
    grep_opt_t *go = st->go;
    if (st->show_filename) {
        if (color_active) printf(COLOR_PURPLE "%s" COLOR_RESET ":", fname);
        else              printf("%s:", fname);
    }
    if (go->line_numbers) {
        if (color_active) printf(COLOR_GREEN "%ld" COLOR_RESET ":", lineno);
        else              printf("%ld:", lineno);
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
            if (color_active) printf(COLOR_PURPLE "%s" COLOR_RESET ":", fname);
            else              printf("%s:", fname);
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

/* Bare memmem loop for the special case where every pattern is a
 * pure literal (cap_start → lit → cap_end → succ).  Bypasses the
 * engine entirely: just memmem to find each match, memchr forward
 * to skip past the matching line.  ~25 % faster than going through
 * search_from on `-c /lit/` because we lose the per-call CTX init
 * + body chain dispatch. */
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
     * Mirrors the bare memmem+memchr loop benchmark — `pos`, `count`,
     * one branch per iter. */
    if (n == 1 && go->count_only && !go->files_with_matches && !go->files_without_match) {
        const char *needle = needles[0];
        size_t nlen = needle_lens[0];
        const char *p = buf;
        const char *end = buf + len;
        while (p < end) {
            const char *q = (const char *)memmem(p, end - p, needle, nlen);
            if (!q) break;
            matches_this_file++;
            const char *e = (const char *)memchr(q, '\n', end - q);
            if (!e) break;
            p = e + 1;
        }
        any_match = matches_this_file > 0;
        goto post_loop;
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
        pos = (line_end < len) ? line_end + 1 : len + 1;
    }

post_loop:
    if (go->files_with_matches) {
        if (any_match) printf("%s\n", fname);
    } else if (go->files_without_match) {
        if (!any_match) printf("%s\n", fname);
    } else if (go->count_only) {
        if (st->show_filename) printf("%s:", fname);
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
        if (any_match) printf("%s\n", fname);
    } else if (go->files_without_match) {
        if (!any_match) printf("%s\n", fname);
    } else if (go->count_only) {
        if (st->show_filename) printf("%s:", fname);
        printf("%ld\n", matches_this_file);
    }
    st->total_match_count += matches_this_file;
    return any_match;
}

/* Returns true if at least one match was found (regardless of -v). */
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
            if (go->files_with_matches || go->files_without_match) {
                /* Just need to know there is/isn't a match; bail early. */
                break;
            }
            if (go->count_only) continue;
            if (go->only_matching && !go->invert) {
                print_only_matching(st, fname, lineno, line, llen);
            } else {
                print_line_with_color(st, fname, lineno, line, llen);
            }
        }
    }
    free(line);

    if (go->files_with_matches) {
        if (any_match) printf("%s\n", fname);
    } else if (go->files_without_match) {
        if (!any_match) printf("%s\n", fname);
    } else if (go->count_only) {
        if (st->show_filename) printf("%s:", fname);
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
    bool all_pure_literal = !go->invert && go->backend == &backend_astrogre_ops
                          && st->n_patterns > 0 && st->n_patterns <= 16;
    if (all_pure_literal) {
        for (int i = 0; i < st->n_patterns; i++) {
            astrogre_pattern *ap = astrogre_backend_pattern_get(st->patterns[i]);
            if (!ap || !astrogre_pattern_pure_literal(ap, &pl_needles[i], &pl_needle_lens[i])) {
                all_pure_literal = false; break;
            }
        }
    }
    if (all_pure_literal) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            struct stat sb;
            if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size > 0) {
                void *map = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
                if (map != MAP_FAILED) {
                    process_buffer_pure_literal(st, (const char *)map, (size_t)sb.st_size,
                                                 path, pl_needles, pl_needle_lens, st->n_patterns);
                    munmap(map, (size_t)sb.st_size);
                    close(fd);
                    return 0;
                }
            }
            close(fd);
        }
    }

    if (!go->invert && fast) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "astrogre: %s: %s\n", path, strerror(errno));
            return 2;
        }
        struct stat sb;
        if (fstat(fd, &sb) == 0 && S_ISREG(sb.st_mode) && sb.st_size > 0) {
            void *map = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
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

static int
process_path(grep_state_t *st, const char *path)
{
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
            char sub[4096];
            snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
            process_path(st, sub);
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
        "  -w  whole word                  -F  fixed string\n"
        "  -l  files with matches          -L  files without\n"
        "  -H  always show filename        -h  never show filename\n"
        "  -o  only matching parts         -r  recursive\n"
        "  -e  PATTERN (repeatable)\n"
        "  --color=never|always|auto       --backend=astrogre|onigmo\n"
        "  -C, --aot-compile               specialize patterns to code_store/ then run\n"
        "  --plain, --no-cs                bypass code store entirely\n"
        "  --cs-verbose                    log cs_load / cs_compile activity\n"
        "Modes:\n"
        "  --self-test                     --bench\n"
        "  --dump PATTERN                  --via-prism\n",
        stderr);
}

int
main(int argc, char *argv[])
{
    INIT();
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

        if (strcmp(a, "--via-prism") == 0)            { go.via_prism = true; argi++; continue; }
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
        if (strcmp(a, "--aot-compile") == 0 || strcmp(a, "-C") == 0) {
            go.cs_mode = CS_MODE_AOT_COMPILE;
            argi++; continue;
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
        if (strcmp(a, "-e") == 0) {
            if (argi + 1 >= argc) { usage(); return 2; }
            push_pattern(&go, argv[argi + 1]);
            argi += 2; continue;
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

    /* If --via-prism, replace each PATTERN by the body of the first
     * /.../ found inside it (parsed as Ruby source by prism). */
    if (go.via_prism) {
        for (int i = 0; i < go.n_patterns; i++) {
            astrogre_pattern *p = astrogre_parse_via_prism(go.patterns[i], strlen(go.patterns[i]));
            if (!p) return 2;
            go.patterns[i] = strdup(p->pat);
            if (p->case_insensitive) go.ignore_case = true;
            astrogre_pattern_free(p);
        }
    }

    color_init(go.color_mode);

    /* Compile patterns. */
    backend_pattern_t **bps = (backend_pattern_t **)calloc(go.n_patterns, sizeof(*bps));
    for (int i = 0; i < go.n_patterns; i++) {
        bps[i] = compile_pattern(&go, go.patterns[i]);
        if (!bps[i]) return 2;
    }

    /* AOT compile mode: ask the backend to specialize each pattern
     * before any input is processed.  Onigmo's `.aot_compile` is NULL
     * and we silently skip it there. */
    if (go.cs_mode == CS_MODE_AOT_COMPILE && go.backend->aot_compile) {
        for (int i = 0; i < go.n_patterns; i++) {
            go.backend->aot_compile(bps[i], go.cs_verbose);
        }
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
    return rc;
}
