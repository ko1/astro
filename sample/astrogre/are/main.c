/*
 * are — modern grep front-end on top of the astrogre regex engine.
 *
 * `are` is to `astrogre` what `rg` is to the rust-regex crate: the
 * production CLI tool that exposes the underlying engine with sane
 * 2020s defaults — recursive by default, hidden / binary skipped,
 * coloured output on a tty, file-type filters via `-t LANG`.
 *
 * Usage:
 *   are [options] PATTERN [PATH...]
 *   are [options] -e PATTERN [-e PAT...] [PATH...]
 *
 * If no PATH is given and stdin is a terminal, `are` recursively
 * searches the current directory.  If stdin is a pipe, it is
 * searched instead — same convention as ripgrep.
 *
 * Modern defaults (vs. GNU grep):
 *   - PATH defaults to `.` and is descended recursively.
 *   - Hidden files/directories (basename starts with `.`) are skipped.
 *   - Binary files (NUL byte in first 8 KiB) are skipped.
 *   - Output is coloured on a tty.
 *   - Pattern is Ruby/Onigmo regex (lookaround, named captures, etc.).
 *
 * Options:
 *   -i  case-insensitive            -n  print line numbers
 *   -c  count                       -v  invert match
 *   -w  whole word                  -x  match whole line
 *   -F  fixed string                -q  quiet (exit code only)
 *   -l  files with matches          -L  files without
 *   -H  always show filename        -h  never show filename
 *   -o  only matching parts         -Z  NUL-separated filenames
 *   -m N  stop after N matches/file
 *   -A N / -B N / -C N              context lines (after/before/both)
 *   -e  PATTERN (repeatable)        -f  read patterns from FILE
 *   -t LANG / -T LANG               filter by file type (see --type-list)
 *   --type-add NAME:GLOB[:GLOB...]  define a custom type
 *   --type-list                     dump the file-type table and exit
 *   --hidden                        descend into hidden dirs/files
 *   --no-ignore                     do NOT honour .gitignore files
 *   --no-recursive                  do NOT descend into directories
 *   -a / --text                     do NOT skip binary files
 *   --include=GLOB / --exclude=GLOB extra basename filters
 *   --color=never|always|auto       (default auto via isatty)
 *   --engine=astrogre|onigmo        backend select (default astrogre)
 *   --aot                           AOT-specialise patterns to code_store/
 *   -j N                            parallel workers (default: NCPU)
 *   -V / --version                  print version and exit
 *
 * The library-internal `--self-test`, `--bench`, `--bench-file`,
 * `--dump`, `--verbose`, `--cs-verbose` etc. live on the legacy
 * `astrogre` binary in the parent directory.  `are` keeps its
 * surface clean.
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
#include "types.h"
#include "ignore.h"
#include "work.h"

/* `verbose_mark` is wired throughout the search loop in the parent
 * astrogre/main.c for `--verbose` profiling.  In `are` we leave it as
 * a no-op stub so the rest of the (forked) code compiles unchanged;
 * the user-facing flag is gone — for that, run the underlying
 * `astrogre` binary with `--verbose`. */
static inline void verbose_mark(const char *tag) { (void)tag; }

/* Code-store API (defined by astro_code_store.c included from node.c) */
extern void astro_cs_compile(NODE *entry, const char *file);
extern void astro_cs_build(const char *extra_cflags);
extern void astro_cs_reload(void);
extern bool astro_cs_load(NODE *n, const char *file);

struct astrogre_option OPTION = {0};

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
    bool recursive;             /* defaults to true — see arg parser */
    bool no_recursive;          /* --no-recursive / -d skip: turn it off */
    bool show_hidden;           /* --hidden: descend into dot-prefixed entries */
    bool include_binary;        /* -a / --text: do NOT skip binary files */
    bool no_ignore;             /* --no-ignore: disable .gitignore filtering */
    int  color_mode;            /* 0 never, 1 always, 2 auto */

    /* -t LANG / -T LANG: file-type filter via the table in types.[ch].
     * `included` are positive selectors (file must match at least one);
     * `excluded` are negative.  Resolved at arg-parse time so the walk
     * loop only sees pointers to are_type_t. */
    const are_type_t **included_types;
    int n_included_types;
    int included_types_cap;
    const are_type_t **excluded_types;
    int n_excluded_types;
    int excluded_types_cap;

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

    /* `-j N` parallel workers.  Default = sysconf(_SC_NPROCESSORS_ONLN);
     * 1 means serial mode and bypasses the pool entirely. */
    int n_jobs;

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
        fprintf(stderr, "are: %s: %s\n", path, strerror(errno));
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

static void
push_type(const are_type_t ***arr, int *n, int *cap, const are_type_t *t)
{
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *arr = (const are_type_t **)realloc(*arr, sizeof(*arr) * (size_t)*cap);
    }
    (*arr)[(*n)++] = t;
}

static const backend_ops_t *
backend_by_name(const char *name)
{
    if (!name || !*name || strcmp(name, "astrogre") == 0) return &backend_astrogre_ops;
#ifdef USE_ONIGMO
    if (strcmp(name, "onigmo") == 0) return &backend_onigmo_ops;
#endif
    fprintf(stderr, "are: unknown engine '%s' (try astrogre or onigmo)\n", name);
    exit(2);
}

/* Small-file threshold below which we read() into a heap buffer
 * instead of mmap()ing.  Linux's per-process mmap_lock is a
 * write-exclusive rwsem — every mmap+munmap pair has to acquire it,
 * which serialises across all worker threads in the same process.
 * Switching small files to read() avoids that contention path
 * entirely (read() touches its own per-fd lock, not mm_struct).
 * 256 KiB covers ~99% of source files in practice. */
#define ARE_MMAP_THRESHOLD (256 * 1024)

/* Number of bytes peeked at the head of a file to decide "binary or
 * not" (NUL byte → binary).  Same convention as ripgrep / git / GNU
 * grep.  These bytes also seed the read() path so we never re-issue
 * a syscall to re-fetch them. */
#define ARE_BINARY_PEEK 8192

/* Return codes for `load_text_file`.  Distinct from "load failed"
 * (which is silent error reporting) so the caller can branch on
 * "binary skipped" without re-doing the open. */
#define ARE_LOAD_OK            0
#define ARE_LOAD_BINARY_SKIP   1
#define ARE_LOAD_EMPTY         2
#define ARE_LOAD_ERROR        -1

/* Open `path`, slurp into memory, optionally skip binaries.  Combined
 * binary check + load — the previous `is_binary_file()` opened the
 * file once just to peek 8 KiB and then closed it before
 * `process_file()` reopened it for the real scan; on a 9 k-file
 * recursive walk that doubled openat / close syscall counts (~30 % of
 * total CPU per `strace -c`).  Now: open once, peek 8 KiB into a
 * stack buffer, decide binary, then either return BINARY_SKIP or
 * fold the peek into the final buffer.
 *
 * Caller MUST release via:
 *   munmap(buf, len)  if is_mmap
 *   free(buf)         otherwise
 *
 * `skip_binary` of false acts as -a / --text. */
static int
load_text_file(const char *path, bool skip_binary,
               const char **out_buf, size_t *out_len, bool *out_is_mmap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "are: %s: %s\n", path, strerror(errno));
        return ARE_LOAD_ERROR;
    }
    struct stat sb;
    if (fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size <= 0) {
        close(fd);
        return ARE_LOAD_EMPTY;
    }

    /* Peek the head into a stack buffer for the binary check.  We
     * reuse these bytes as the start of the final buffer — no
     * second read covering the same range. */
    char peek[ARE_BINARY_PEEK];
    const size_t peek_cap = sizeof(peek);
    const size_t to_peek  = (size_t)sb.st_size < peek_cap ? (size_t)sb.st_size : peek_cap;
    ssize_t pn = 0;
    while (pn < (ssize_t)to_peek) {
        ssize_t n = read(fd, peek + pn, to_peek - (size_t)pn);
        if (n < 0) { if (errno == EINTR) continue; close(fd); return ARE_LOAD_ERROR; }
        if (n == 0) break;
        pn += n;
    }
    if (pn <= 0) { close(fd); return ARE_LOAD_EMPTY; }

    if (skip_binary && memchr(peek, 0, (size_t)pn) != NULL) {
        close(fd);
        return ARE_LOAD_BINARY_SKIP;
    }

    if ((size_t)sb.st_size <= ARE_MMAP_THRESHOLD) {
        /* read() path — avoids mmap_lock contention.  Heap allocation
         * scales with per-thread arenas in glibc malloc. */
        char *buf = (char *)malloc((size_t)sb.st_size);
        if (!buf) { close(fd); return ARE_LOAD_ERROR; }
        memcpy(buf, peek, (size_t)pn);
        ssize_t got = pn;
        while (got < sb.st_size) {
            ssize_t n = read(fd, buf + got, (size_t)(sb.st_size - got));
            if (n < 0) { if (errno == EINTR) continue; free(buf); close(fd); return ARE_LOAD_ERROR; }
            if (n == 0) break;  /* short read — file shrunk under us */
            got += n;
        }
        close(fd);
        *out_buf = buf;
        *out_len = (size_t)got;
        *out_is_mmap = false;
        return ARE_LOAD_OK;
    }

    /* Big file: mmap from start.  The peek bytes get re-fetched via
     * the page cache (free), so no need to splice them in.  We seek
     * isn't necessary because mmap ignores the file offset. */
    void *map = mmap(NULL, (size_t)sb.st_size, PROT_READ,
                      MAP_PRIVATE | MAP_POPULATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return ARE_LOAD_ERROR;
    *out_buf = (const char *)map;
    *out_len = (size_t)sb.st_size;
    *out_is_mmap = true;
    return ARE_LOAD_OK;
}

static void
release_file(const char *buf, size_t len, bool is_mmap)
{
    if (!buf) return;
    if (is_mmap) munmap((void *)buf, len);
    else         free  ((void *)buf);
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
        fprintf(stderr, "are: failed to compile pattern: %s\n", pat);
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
    /* `.gitignore` stack — pushed/popped around opendir/closedir in
     * the recursive walker.  Seeded once from the cwd's repo root at
     * search start (`ignore_stack_seed_from_repo_root`). */
    ignore_stack_t ignore;
    /* Output sink for the per-file print functions.  Default is
     * stdout; in parallel mode the worker overrides this with a
     * per-task `open_memstream` buffer that gets flushed to the real
     * stdout under a mutex once the file's scan is complete.  All
     * print_line / print_only_matching / process_buffer / process_stream
     * write here rather than to `stdout` directly. */
    FILE *out;
    /* Worker pool — set when `-j N` (N > 1).  When non-NULL,
     * process_path's file branch enqueues the path instead of
     * scanning inline; workers pop and call process_file with their
     * own per-thread grep_state copy. */
    work_pool_t *pool;
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
        if (color_active) fprintf(st->out, COLOR_PURPLE "%s" COLOR_RESET, fname);
        else              fputs(fname, st->out);
        fputc(sep, st->out);
    }
    if (go->line_numbers) {
        if (color_active) fprintf(st->out, COLOR_GREEN "%ld" COLOR_RESET, lineno);
        else              fprintf(st->out, "%ld", lineno);
        fputc(sep, st->out);
    }

    if (!color_active) {
        fwrite(line, 1, len, st->out);
        fputc('\n', st->out);
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
            fwrite(line + pos, 1, len - pos, st->out);
            break;
        }
        if (best_start > pos) fwrite(line + pos, 1, best_start - pos, st->out);
        fputs(COLOR_RED, st->out);
        fwrite(line + best_start, 1, best_end - best_start, st->out);
        fputs(COLOR_RESET, st->out);
        if (best_end == best_start) { /* zero-width — advance one byte */
            if (best_end < len) fputc(line[best_end], st->out);
            pos = best_end + 1;
        } else {
            pos = best_end;
        }
    }
    fputc('\n', st->out);
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
            if (color_active) fprintf(st->out, COLOR_PURPLE "%s" COLOR_RESET, fname);
            else              fputs(fname, st->out);
            fputc(fname_sep(st->go), st->out);
        }
        if (go->line_numbers) {
            if (color_active) fprintf(st->out, COLOR_GREEN "%ld" COLOR_RESET ":", lineno);
            else              fprintf(st->out, "%ld:", lineno);
        }
        if (color_active) {
            fputs(COLOR_RED, st->out);
            fwrite(line + best_start, 1, best_end - best_start, st->out);
            fputs(COLOR_RESET, st->out);
        } else {
            fwrite(line + best_start, 1, best_end - best_start, st->out);
        }
        fputc('\n', st->out);
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
            long c = astrogre_pattern_print_lines(ap, buf, len, fname, st->out, emit_opts);
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
        if (any_match) { fputs(fname, st->out); fputc(go->null_separator ? '\0' : '\n', st->out); }
    } else if (go->files_without_match) {
        if (!any_match) { fputs(fname, st->out); fputc(go->null_separator ? '\0' : '\n', st->out); }
    } else if (go->count_only) {
        if (st->show_filename) { fputs(fname, st->out); fputc(fname_sep(go), st->out); }
        fprintf(st->out, "%ld\n", matches_this_file);
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
        if (any_match) { fputs(fname, st->out); fputc(go->null_separator ? '\0' : '\n', st->out); }
    } else if (go->files_without_match) {
        if (!any_match) { fputs(fname, st->out); fputc(go->null_separator ? '\0' : '\n', st->out); }
    } else if (go->count_only) {
        if (st->show_filename) { fputs(fname, st->out); fputc(fname_sep(go), st->out); }
        fprintf(st->out, "%ld\n", matches_this_file);
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
                    if (earliest > last_emitted_lineno + 1) fputs("--\n", st->out);
                }
                for (long k = 0; k < bring_count; k++) {
                    long idx = (bring_head - bring_count + k + bsize) % bsize;
                    print_line_with_color(st, fname, bring_lineno[idx],
                                          bring[idx], bring_llen[idx]);
                }
                bring_count = 0;
            } else if (need_separator && lineno > last_emitted_lineno + 1) {
                fputs("--\n", st->out);
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
        if (any_match) { fputs(fname, st->out); fputc(go->null_separator ? '\0' : '\n', st->out); }
    } else if (go->files_without_match) {
        if (!any_match) { fputs(fname, st->out); fputc(go->null_separator ? '\0' : '\n', st->out); }
    } else if (go->count_only) {
        if (st->show_filename) { fputs(fname, st->out); fputc(fname_sep(go), st->out); }
        fprintf(st->out, "%ld\n", matches_this_file);
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
    /* Combined binary-skip + load: one open syscall instead of two
     * (the previous design opened once for is_binary_file then again
     * for the scan).  ARE_LOAD_BINARY_SKIP takes the early exit. */
    const bool skip_binary = !go->include_binary;

    if (all_pure_literal) {
        const char *buf = NULL;
        size_t len = 0;
        bool is_mmap = false;
        const int rc = load_text_file(path, skip_binary, &buf, &len, &is_mmap);
        if (rc == ARE_LOAD_OK) {
            process_buffer_pure_literal(st, buf, len, path,
                                        pl_needles, pl_needle_lens, st->n_patterns);
            release_file(buf, len, is_mmap);
            return 0;
        }
        if (rc == ARE_LOAD_BINARY_SKIP || rc == ARE_LOAD_EMPTY) return 0;
        /* ARE_LOAD_ERROR — fall through to fopen path for diagnostic. */
    }

    /* Fall through to streaming when context lines are requested
     * (the mmap path's per-match search loop doesn't carry the
     * before-buffer / after-counter that -A/-B/-C need), or when
     * -x is set (the \A...\z wrap only matches at FILE boundaries
     * if the matcher sees the whole buffer; we want LINE boundaries). */
    if (!go->invert && fast
        && go->before_context == 0 && go->after_context == 0
        && !go->line_regexp) {
        const char *buf = NULL;
        size_t len = 0;
        bool is_mmap = false;
        const int rc = load_text_file(path, skip_binary, &buf, &len, &is_mmap);
        if (rc == ARE_LOAD_OK) {
            process_buffer(st, buf, len, path);
            release_file(buf, len, is_mmap);
            return 0;
        }
        if (rc == ARE_LOAD_BINARY_SKIP || rc == ARE_LOAD_EMPTY) return 0;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "are: %s: %s\n", path, strerror(errno));
        return 2;
    }
    process_stream(st, fp, path);
    fclose(fp);
    return 0;
}

/* True iff `name` (basename only) passes the --include / --exclude
 * globs and the -t / -T type filters.
 *
 * The order matters: --exclude wins over --include and -t (so a user
 * can always pull a file out of consideration with --exclude), and
 * type filters only apply to files (directories are always descended,
 * since the type of files inside isn't visible from the dir name). */
static bool
glob_filter_pass(grep_opt_t *go, const char *name, bool is_dir)
{
    /* --exclude basename glob — both files and dirs.  Useful for
     * `--exclude=node_modules` in recursive mode. */
    for (int i = 0; i < go->n_exclude_globs; i++) {
        if (fnmatch(go->exclude_globs[i], name, 0) == 0) return false;
    }
    if (!is_dir) {
        /* -T LANG: drop the file if it matches an excluded type. */
        for (int i = 0; i < go->n_excluded_types; i++) {
            if (are_type_matches(go->excluded_types[i], name)) return false;
        }
        /* -t LANG: when set, file must match at least one. */
        if (go->n_included_types > 0) {
            bool ok = false;
            for (int i = 0; i < go->n_included_types; i++) {
                if (are_type_matches(go->included_types[i], name)) { ok = true; break; }
            }
            if (!ok) return false;
        }
        /* --include=GLOB: when set, file must match at least one. */
        if (go->n_include_globs > 0) {
            bool ok = false;
            for (int i = 0; i < go->n_include_globs; i++) {
                if (fnmatch(go->include_globs[i], name, 0) == 0) { ok = true; break; }
            }
            if (!ok) return false;
        }
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
        fprintf(stderr, "are: %s: %s\n", path, strerror(errno));
        return 2;
    }
    if (S_ISDIR(sb.st_mode)) {
        if (!st->go->recursive) {
            fprintf(stderr, "are: %s: Is a directory\n", path);
            return 2;
        }
        DIR *d = opendir(path);
        if (!d) { fprintf(stderr, "are: %s: %s\n", path, strerror(errno)); return 2; }

        /* Push this directory's `.gitignore` (if any) so descendants
         * see its rules.  We track the stack depth before the push
         * so we know whether to pop on closedir without leaking the
         * "no .gitignore here" case. */
        const size_t pre_push_depth = st->ignore.n_layers;
        ignore_stack_push_dir(&st->ignore, path);
        const bool pushed = (st->ignore.n_layers > pre_push_depth);

        struct dirent *e;
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            /* Skip hidden dot-prefixed entries unless --hidden was set.
             * Same rule for files and dirs — `.cache/`, `.DS_Store`
             * all stay invisible.  `.git/` is also force-skipped by
             * `ignore_should_skip` even with --hidden, since scanning
             * pack files is never useful. */
            if (!st->go->show_hidden && e->d_name[0] == '.') continue;
            const bool is_dir = (e->d_type == DT_DIR);
            if (ignore_should_skip(&st->ignore, path, e->d_name, is_dir)) continue;
            if (!glob_filter_pass(st->go, e->d_name, is_dir)) continue;
            char sub[4096];
            snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
            process_path(st, sub);
            if (st->go->quiet && st->total_match_count > 0) break;
        }
        closedir(d);
        if (pushed) ignore_stack_pop(&st->ignore);
        return 0;
    }
    /* Binary detection now happens inside `process_file` ->
     * `load_text_file`, which folds the 8 KiB peek into the same
     * open() that loads the file's content.  No upfront probe here.
     * Parallel mode just enqueues the path; the worker calls
     * process_file with its own grep_state copy. */
    if (st->pool) {
        work_pool_submit(st->pool, path);
        return 0;
    }
    return process_file(st, path);
}

/* ------------------------------------------------------------------ */
/* Worker pool plumbing                                                */
/* ------------------------------------------------------------------ */

/* Setup payload shared with the workers — just the base grep_state
 * (read-only fields like `go`, `patterns`, `n_patterns`, `show_filename`).
 * The pool itself is delivered to setup as a separate argument by the
 * work_pool runtime, so we don't need to hold a (potentially still-NULL)
 * pool pointer in here. */
typedef struct worker_setup {
    const grep_state_t *base;
} worker_setup_t;

/* Per-thread state.  Allocated once per worker (in `worker_setup_fn`)
 * and reused across all tasks the thread runs.  Holds its own
 * grep_state plus a thread-local batch buffer that accumulates
 * per-file output until it crosses ARE_BATCH_FLUSH_BYTES, at which
 * point it's fwritten to stdout under the pool's shared mutex —
 * one mutex acquisition per batch instead of per file.
 *
 * The batch is flushed in three places:
 *   1. After each file scan, IF the accumulated size crosses the
 *      threshold (worker_task_fn).
 *   2. When the worker exits (worker_teardown_fn) — leftover bytes.
 *   3. join_and_destroy waits for both before returning.
 *
 * Output ordering: per-batch FIFO (file order within a batch);
 * across batches and across workers it's interleaved at batch
 * boundaries — same trade-off as ripgrep's `--sort=none` default. */
#define ARE_BATCH_FLUSH_BYTES (64 * 1024)

typedef struct worker_state {
    grep_state_t  st;
    work_pool_t  *pool;
    char         *batch_buf;       /* heap, grown via realloc */
    size_t        batch_len;
    size_t        batch_cap;
    long          batch_match_delta;
} worker_state_t;

static void *
worker_setup_fn(work_pool_t *pool, void *user)
{
    const worker_setup_t *us = (const worker_setup_t *)user;
    worker_state_t *w = (worker_state_t *)calloc(1, sizeof(*w));
    /* Copy the base grep_state — patterns/go/show_filename are
     * read-only after compile, so the shallow copy is safe to share.
     * `total_match_count`, `out`, `pool` are per-thread. */
    w->st = *us->base;
    w->st.total_match_count = 0;
    w->st.out = NULL;
    w->st.pool = NULL;        /* worker dispatches process_file directly */
    w->pool = pool;
    /* Pre-size the batch buffer to one flush threshold so the first
     * few files don't trigger any realloc.  realloc beyond this is
     * fine (memory-bandwidth bound, not lock-bound). */
    w->batch_cap = ARE_BATCH_FLUSH_BYTES;
    w->batch_buf = (char *)malloc(w->batch_cap);
    w->batch_len = 0;
    w->batch_match_delta = 0;
    return w;
}

/* Flush the worker's accumulated batch to stdout under the pool's
 * shared mutex.  Resets the local counters so the next file starts
 * fresh.  No-op when the batch is empty. */
static void
worker_flush(worker_state_t *w)
{
    if (w->batch_len == 0 && w->batch_match_delta == 0) return;
    work_pool_flush_batch(w->pool, w->batch_buf, w->batch_len, w->batch_match_delta);
    w->batch_len = 0;
    w->batch_match_delta = 0;
}

static void
worker_teardown_fn(void *worker_arg)
{
    worker_state_t *w = (worker_state_t *)worker_arg;
    if (!w) return;
    worker_flush(w);              /* push leftover bytes before exit */
    free(w->batch_buf);
    free(w);
}

/* The per-task work function.  Runs in a worker thread.  We re-do
 * the binary skip here (process_path skipped it for parallel mode)
 * then run process_file with `w->st.out` pointed at a per-task
 * memstream so the print helpers append into a small heap buffer.
 * After the scan we copy that buffer onto the worker's batch (no
 * cross-thread sync) and decide whether to flush. */
static void
worker_task_fn(const char *path, void *worker_arg)
{
    worker_state_t *w = (worker_state_t *)worker_arg;
    /* Binary detection happens inside process_file -> load_text_file.
     * No separate is_binary_file() peek (that doubled per-file
     * openat / close traffic). */

    /* Per-task memstream so process_file's existing fprintf'/fwrite
     * helpers don't have to know about batching.  The buffer lives
     * only for this scan; we copy it into the worker's batch right
     * after fclose. */
    char  *file_buf  = NULL;
    size_t file_blen = 0;
    FILE  *out = open_memstream(&file_buf, &file_blen);
    if (!out) {
        /* OOM territory — open_memstream only fails on allocation
         * failure.  Drop this file's output (the per-thread batch
         * may still hold useful prior bytes); the caller will see
         * a fresh OOM on the next syscall anyway. */
        return;
    }
    w->st.out = out;
    const long pre = w->st.total_match_count;
    process_file(&w->st, path);
    fclose(out);
    const long delta = w->st.total_match_count - pre;

    /* Append the file's output to the worker's batch.  Grow the
     * batch buffer geometrically so realloc cost amortises to O(1)
     * per byte across the worker's lifetime. */
    if (w->batch_len + file_blen > w->batch_cap) {
        size_t need = w->batch_len + file_blen;
        size_t cap  = w->batch_cap ? w->batch_cap : ARE_BATCH_FLUSH_BYTES;
        while (cap < need) cap *= 2;
        w->batch_buf = (char *)realloc(w->batch_buf, cap);
        w->batch_cap = cap;
    }
    memcpy(w->batch_buf + w->batch_len, file_buf, file_blen);
    w->batch_len += file_blen;
    w->batch_match_delta += delta;
    free(file_buf);

    if (w->batch_len >= ARE_BATCH_FLUSH_BYTES) worker_flush(w);
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

#ifndef ARE_VERSION
#define ARE_VERSION "0.1.0-dev"
#endif

static void
print_type_list(void)
{
    /* Render as `name: glob1 glob2 …`, padding the name column to the
     * widest known type for column alignment. */
    size_t pad = 0;
    for (size_t i = 0; i < are_type_count(); i++) {
        const size_t l = strlen(are_type_at(i)->name);
        if (l > pad) pad = l;
    }
    for (size_t i = 0; i < are_type_count(); i++) {
        const are_type_t *t = are_type_at(i);
        printf("%-*s:", (int)pad, t->name);
        for (const char **g = t->globs; *g; g++) printf(" %s", *g);
        printf("\n");
    }
}

static void
usage(void)
{
    fputs(
        "are — modern grep on the astrogre regex engine\n"
        "Usage: are [options] PATTERN [PATH...]\n"
        "       are [options] -e PATTERN [-e PAT...] [PATH...]\n"
        "\n"
        "Defaults differ from grep: directories are recursed into, hidden\n"
        "(dot-prefixed) entries are skipped, binary files are skipped, and\n"
        "output is coloured on a tty.  PATH defaults to `.` when omitted.\n"
        "\n"
        "Match options:\n"
        "  -i  case-insensitive            -n  print line numbers\n"
        "  -c  count matching lines        -v  invert match\n"
        "  -w  whole word                  -x  match whole line\n"
        "  -F  fixed string                -q  quiet (exit code only)\n"
        "  -l  files with matches          -L  files without\n"
        "  -H  always show filename        -h  never show filename\n"
        "  -o  only matching parts         -Z  NUL-separated filenames\n"
        "  -m N  stop after N matches/file\n"
        "  -A N / -B N / -C N              context lines (after/before/both)\n"
        "  -e PATTERN (repeatable)         -f FILE  read patterns from FILE\n"
        "\n"
        "Walking options:\n"
        "  -t LANG / -T LANG               include / exclude file type\n"
        "  --type-add NAME:GLOB[:GLOB...]  define a custom type\n"
        "  --type-list                     list known types and exit\n"
        "  --include=GLOB / --exclude=GLOB extra basename glob filters\n"
        "  --hidden                        descend into hidden entries\n"
        "  --no-ignore                     ignore .gitignore files\n"
        "  -a, --text                      do NOT skip binary files\n"
        "  --no-recursive                  do NOT descend into directories\n"
        "  -j N                            parallel workers (default: NCPU)\n"
        "\n"
        "Other:\n"
        "  --color=never|always|auto       (default auto via isatty)\n"
        "  --engine=astrogre|onigmo        backend selection (default astrogre)\n"
        "  --aot                           AOT-specialise patterns to code_store/\n"
        "  -V, --version                   print version and exit\n"
        "      --help                      print this help and exit\n"
        "\n"
        "For library-internal commands (--self-test, --bench, --dump,\n"
        "...) use the legacy `astrogre` binary in the same directory.\n",
        stderr);
}

int
main(int argc, char *argv[])
{
    /* astrogre's `node.c` auto-detects the source dir from
     * `/proc/self/exe`'s parent — for `astrogre` itself that's
     * `<engine_dir>/`, but for `are` it's `<engine_dir>/are/`,
     * which doesn't have node.h.  Tell astro_cs to look one
     * level up before INIT() runs.  The framework respects
     * `ASTRO_CS_SRC_DIR` and the env var takes precedence over
     * the auto-detected value. */
    {
        char p[PATH_MAX];
        const ssize_t n = readlink("/proc/self/exe", p, sizeof(p) - 1);
        if (n > 0) {
            p[n] = '\0';
            char *slash = strrchr(p, '/');     /* strip "are" binary */
            if (slash) *slash = '\0';
            slash = strrchr(p, '/');           /* strip "are" subdir */
            if (slash) { *slash = '\0'; setenv("ASTRO_CS_SRC_DIR", p, 1); }
        }
    }
    INIT();
    /* Bypass the code-store auto-load by default.  Otherwise an
     * `are` invoked from a directory that happens to contain a
     * stale `code_store/all.so` (e.g. a leftover from a previous
     * `--aot` run or a different build) will dlopen it and dispatch
     * patterns through dispatchers that no longer match the
     * current binary's NODE layout — segfaults that take an hour
     * to track down.  `--aot` opts back into the cache. */
    OPTION.no_compiled_code = true;

    grep_opt_t go = {0};
    go.color_mode  = 2;        /* auto */
    go.backend     = &backend_astrogre_ops;
    go.recursive   = true;     /* `are` defaults: descend dirs */

    int argi = 1;

    /* Parse flags. */
    while (argi < argc) {
        const char *a = argv[argi];
        if (a[0] != '-' || strcmp(a, "-") == 0) break;
        if (strcmp(a, "--") == 0) { argi++; break; }

        if (strcmp(a, "--help") == 0) { usage(); return 0; }
        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("are %s (astrogre regex engine)\n", ARE_VERSION);
            return 0;
        }
        if (strcmp(a, "--type-list") == 0) { print_type_list(); return 0; }
        if (strcmp(a, "--type-add") == 0) {
            if (argi + 1 >= argc) { usage(); return 2; }
            are_type_add(argv[argi + 1]);
            argi += 2; continue;
        }
        if (strcmp(a, "-t") == 0) {
            if (argi + 1 >= argc) { usage(); return 2; }
            const are_type_t *t = are_type_find(argv[argi + 1]);
            if (!t) {
                fprintf(stderr, "are: unknown type '%s' (try --type-list)\n", argv[argi + 1]);
                return 2;
            }
            push_type(&go.included_types, &go.n_included_types, &go.included_types_cap, t);
            argi += 2; continue;
        }
        if (strcmp(a, "-T") == 0) {
            if (argi + 1 >= argc) { usage(); return 2; }
            const are_type_t *t = are_type_find(argv[argi + 1]);
            if (!t) {
                fprintf(stderr, "are: unknown type '%s' (try --type-list)\n", argv[argi + 1]);
                return 2;
            }
            push_type(&go.excluded_types, &go.n_excluded_types, &go.excluded_types_cap, t);
            argi += 2; continue;
        }
        if (strcmp(a, "--hidden") == 0)        { go.show_hidden    = true;  argi++; continue; }
        if (strcmp(a, "--text") == 0)          { go.include_binary = true;  argi++; continue; }
        if (strcmp(a, "--no-recursive") == 0)  { go.recursive      = false; argi++; continue; }
        if (strcmp(a, "--no-ignore") == 0)     { go.no_ignore      = true;  argi++; continue; }
        if (strcmp(a, "-j") == 0) {
            if (argi + 1 >= argc) { usage(); return 2; }
            go.n_jobs = (int)strtol(argv[argi + 1], NULL, 10);
            if (go.n_jobs < 1) go.n_jobs = 1;
            argi += 2; continue;
        }

        if (strncmp(a, "--color", 7) == 0) {
            const char *val = (a[7] == '=') ? a + 8 : "always";
            if (strcmp(val, "never") == 0)       go.color_mode = 0;
            else if (strcmp(val, "always") == 0) go.color_mode = 1;
            else                                  go.color_mode = 2;
            argi++; continue;
        }
        if (strncmp(a, "--engine=", 9) == 0) {
            go.backend = backend_by_name(a + 9);
            argi++; continue;
        }
        if (strcmp(a, "--aot") == 0) {
            go.cs_mode = CS_MODE_AOT_COMPILE;
            argi++; continue;
        }
        if (strcmp(a, "-C") == 0) {
            /* `are` keeps -C as the GNU `--context=NUM` form (no AOT
             * collision; AOT is the long-form `--aot`). */
            if (argi + 1 >= argc) { usage(); return 2; }
            const long n = strtol(argv[argi + 1], NULL, 10);
            go.before_context = n;
            go.after_context  = n;
            argi += 2; continue;
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
                case 'r': go.recursive = true; break;  /* compat no-op (default) */
                case 'a': go.include_binary = true; break;
                case 'q': go.quiet = true; break;
                case 'x': go.line_regexp = true; break;
                case 'Z': go.null_separator = true; break;
                case 'E': /* compatibility no-op (we always use Ruby regex) */ break;
                case 's': /* suppress error msgs — not yet */ break;
                default:
                    fprintf(stderr, "are: unknown option -%c\n", *q);
                    usage(); return 2;
                }
            }
            argi++; continue;
        }
        fprintf(stderr, "are: unknown option %s\n", a);
        usage(); return 2;
    }

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

    /* AOT-compile each pattern via the backend.  Onigmo's
     * `.aot_compile` slot is NULL → silent skip.  Re-enable the
     * code-store load so the freshly-compiled SDs actually get
     * picked up. */
    if (go.cs_mode == CS_MODE_AOT_COMPILE && go.backend->aot_compile) {
        OPTION.no_compiled_code = false;
        for (int i = 0; i < go.n_patterns; i++) {
            go.backend->aot_compile(bps[i], false);
        }
    }

    grep_state_t st = {0};
    st.go  = &go;
    st.out = stdout;
    st.patterns = bps;
    st.n_patterns = go.n_patterns;

    /* `are` decides the filename column the way ripgrep does: shown
     * iff we walked more than one file (recursive mode usually does)
     * OR multiple PATHs were named on the command line.  -H / -h
     * override either way. */
    const int n_paths = argc - argi;

    int rc;
    if (n_paths == 0 && !isatty(STDIN_FILENO)) {
        /* Stdin is piped → behave like grep, search the stream. */
        if (go.no_filename_force)        st.show_filename = false;
        else if (go.with_filename_force) st.show_filename = true;
        else                             st.show_filename = false;
        process_stream(&st, stdin, "(standard input)");
        rc = (st.total_match_count > 0) ? 0 : 1;
    } else {
        /* Walk explicit PATHs, or default to `.` when stdin is a tty. */
        const char *default_paths[] = { "." };
        const char **paths;
        int n;
        if (n_paths == 0) {
            paths = default_paths;
            n     = 1;
        } else {
            paths = (const char **)&argv[argi];
            n     = n_paths;
        }
        if (go.no_filename_force)        st.show_filename = false;
        else if (go.with_filename_force) st.show_filename = true;
        else                             st.show_filename = (n > 1) || go.recursive;

        /* Default `-j N` to the number of online CPUs; clamp to 1 if
         * sysconf fails.  N=1 keeps the serial path (no thread setup,
         * no per-task memstream — same numbers as the pre-parallel
         * code on a single core). */
        if (go.n_jobs <= 0) {
            const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
            go.n_jobs = (ncpu > 0) ? (int)ncpu : 1;
        }

        /* Seed the .gitignore stack from the enclosing repo root —
         * picks up the cwd's .gitignore + every parent .gitignore up
         * to the dir that contains `.git`.  Per-dir layers are
         * pushed/popped inside the recursive walker (process_path).
         * `--no-ignore` puts the stack into pass-through mode but
         * still hard-skips `.git/`. */
        ignore_stack_init(&st.ignore, go.no_ignore);
        ignore_stack_seed_from_repo_root(&st.ignore, paths[0]);

        worker_setup_t ws = { .base = &st };
        if (go.n_jobs > 1) {
            st.pool = work_pool_create(go.n_jobs, worker_task_fn,
                                       worker_setup_fn, worker_teardown_fn, &ws);
        }

        for (int i = 0; i < n; i++) {
            process_path(&st, paths[i]);
            /* `-q` early-exit only kicks in for the serial path; in
             * parallel mode the workers may still be processing
             * already-enqueued tasks, but we don't enqueue more. */
            if (!st.pool && go.quiet && st.total_match_count > 0) break;
        }

        if (st.pool) {
            /* `join_and_destroy` waits for all in-flight tasks to
             * drain (workers exit on `closed && empty`), captures
             * the accumulated match count, then frees the pool.
             * Roll the count into the local total so the exit-code
             * decision below works the same as the serial path. */
            st.total_match_count += work_pool_join_and_destroy(st.pool);
        }
        rc = (st.total_match_count > 0) ? 0 : 1;
        ignore_stack_free(&st.ignore);
    }

    for (int i = 0; i < go.n_patterns; i++) go.backend->free(bps[i]);
    free(bps);
    free(go.patterns);
    return rc;
}
