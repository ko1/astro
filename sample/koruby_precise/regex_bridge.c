/* koruby_precise regex bridge — wraps the astrogre regex engine so koruby can
 * do Regexp matching without hand-writing one (memory: Regexp via astrogre).
 *
 * Built into a SEPARATE shared object (koruby_regex.so) with -fvisibility=hidden
 * so astrogre's generic ASTro symbols (OPTIMIZE / HASH / node_eval / NODE / ...)
 * stay internal and never clash with koruby's identically-named symbols.  koruby
 * dlopen()s it RTLD_LOCAL and calls only koruby_re_search.
 *
 * Compiled with astrogre's headers + engine (node.c/parse.c/match.c/
 * backend_astrogre.c/aho_corasick.c); the engine expects the embedder to define
 * the OPTION global. */
#include <stddef.h>
#include "../astrogre/context.h"   /* astrogre's (NOT koruby's) — struct astrogre_option */
#include "../astrogre/backend.h"

struct astrogre_option OPTION = {0};

/* Search `str` for `pat`.  Returns 1 on match (filling *ms/*me with the byte
 * span), 0 on no match, -1 on compile error. */
__attribute__((visibility("default")))
int koruby_re_search(const char *pat, size_t patlen, const char *str, size_t slen,
                     int ci, long *ms, long *me) {
    backend_flags_t fl;
    fl.case_insensitive = (ci != 0);
    fl.multiline = false; fl.extended = false;
    fl.fixed_string = false; fl.ascii_8bit = false;
    backend_pattern_t *p = backend_astrogre_ops.compile(pat, patlen, fl);
    if (p == NULL) return -1;
    backend_match_t m; m.matched = false; m.start = 0; m.end = 0;
    bool ok = backend_astrogre_ops.search(p, str, slen, &m);
    backend_astrogre_ops.free(p);
    if (ok) { *ms = (long)m.start; *me = (long)m.end; return 1; }
    return 0;
}
