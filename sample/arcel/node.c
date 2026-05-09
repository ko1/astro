#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "node.h"
#include "context.h"

/* User-provided: AST node allocator (kept noinline so flame graphs
 * show ALLOC_node_* call sites even after LTO).  The arena lives in
 * code_repo for now — every parsed expression burns a fresh chunk. */
static __attribute__((noinline)) NODE *
node_allocate(size_t size)
{
    NODE *const n = (NODE *)malloc(size);
    if (n == NULL) {
        fprintf(stderr, "arcel: out of memory (%zu bytes)\n", size);
        exit(EXIT_FAILURE);
    }
    return n;
}

/* Binary-safe C string literal emitter, used by SPECIALIZE_xxx for
 * `const char *` operands.  Every operand of type `const char *` in
 * arcel is paired (by naming convention) with a `uint32_t` length
 * operand; this lets the SD reproduce arbitrary bytes.
 *
 * Why we don't use the framework's `astro_fprint_cstr`: it uses
 * `for (; *s; s++)`, so it truncates at the first NUL and never
 * escapes non-printables — a CEL `'\000\xff'` literal becomes `""`,
 * producing invalid SD source.
 *
 * Why we use OCTAL `\NNN` (exactly 3 digits) and not hex `\xHH`:
 * C's `\x` escape consumes ALL following hex digits, so a UTF-8
 * sequence like `"\xc3\x9fe"` ("ße") is parsed as `\xc39f` followed
 * by `e` — i.e. the implementation tries to encode 0xC39F into a
 * single char, which is implementation-defined / out of range.
 * Octal is capped at 3 digits, so `\303\237e` parses unambiguously
 * as 0xC3, 0x9F, 'e'. */
static void
arcel_fprint_blob_lit(FILE *fp, const char *p, uint32_t len)
{
    fputs("        \"", fp);
    for (uint32_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];
        switch (c) {
            case '\\': fputs("\\\\", fp); break;
            case '"':  fputs("\\\"", fp); break;
            case '\n': fputs("\\n",  fp); break;
            case '\r': fputs("\\r",  fp); break;
            case '\t': fputs("\\t",  fp); break;
            default:
                if (c >= 0x20 && c < 0x7F) fputc(c, fp);
                else                       fprintf(fp, "\\%03o", c);
        }
    }
    fputc('"', fp);
}

#include "astro_node.c"
#include "astro_code_store.c"

VALUE
EVAL(CTX *const c, NODE *const n)
{
    return (*n->head.dispatcher)(c, n);
}

NODE *
OPTIMIZE(NODE *n)
{
    if (OPTION.no_compiled_code) {
        return n;
    }

    if (astro_cs_load(n, NULL)) {
        if (!OPTION.quiet) {
            fprintf(stderr, "hit!: h:%016lx %s ",
                    (unsigned long)hash_node(n),
                    n->head.kind->default_dispatcher_name);
            DUMP(stderr, n, true);
            fprintf(stderr, "\n");
        }
    } else {
        if (!OPTION.quiet) {
            fprintf(stderr, "miss: h:%016lx %s ",
                    (unsigned long)hash_node(n),
                    n->head.kind->default_dispatcher_name);
            DUMP(stderr, n, true);
            fprintf(stderr, "\n");
        }
    }
    return n;
}

void
code_repo_add(const char *name, NODE *body, bool force)
{
    (void)name; (void)body; (void)force;
}

#include "node_eval.c"
#include "node_dispatch.c"
#include "node_dump.c"
#include "node_hash.c"
#include "node_specialize.c"
#include "node_replace.c"
#include "node_alloc.c"

void
INIT(void)
{
    astro_cs_init("code_store", ".", 0);
}
