// ascheme — S-expression reader.
//
// Lifted from main.c.  Pure text-to-VALUE: tokens, comments,
// quote/quasiquote/unquote sugar, hash literals (#t / #f / #\char /
// vector), string literals with escapes, integer / rational /
// flonum atoms (via GMP for big ints / rationals), and identifier
// interning.  Allocates VALUE objects through `scm_cons` /
// `scm_make_*` from main.c, which all go through libgc.

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gmp.h>
#include "context.h"
#include "precise_gc/gc.h"
#include "node.h"
#include "parse.h"

struct reader {
    const char *src;
    size_t pos;
    size_t len;
    int line;
    FILE *fp;     // alternative input source (NULL → src/pos)
    int ungot;    // pushed-back char (-2 → none)
};

static int
reader_getc(struct reader *r)
{
    if (r->ungot != -2) { int c = r->ungot; r->ungot = -2; return c; }
    if (r->fp) {
        int c = fgetc(r->fp);
        if (c == '\n') r->line++;
        return c;
    }
    if (r->pos >= r->len) return EOF;
    int c = (unsigned char)r->src[r->pos++];
    if (c == '\n') r->line++;
    return c;
}

static void
reader_ungetc(struct reader *r, int c)
{
    r->ungot = c;
}

static void
reader_skip_ws(struct reader *r)
{
    for (;;) {
        int c = reader_getc(r);
        if (c == EOF) return;
        if (c == ';') {
            while (c != EOF && c != '\n') c = reader_getc(r);
            continue;
        }
        if (isspace(c)) continue;
        reader_ungetc(r, c);
        return;
    }
}

static bool
is_delim(int c)
{
    return c == EOF || isspace(c) || c == '(' || c == ')' ||
           c == '"' || c == ';' || c == '\'' || c == '`' || c == ',';
}

static VALUE read_form(CTX *c, struct reader *r);

static VALUE
read_list(CTX *c, struct reader *r)
{
    reader_skip_ws(r);
    int ch = reader_getc(r);
    if (ch == ')') return SCM_NIL;
    if (ch == EOF) scm_error(c, "unexpected EOF in list");
    reader_ungetc(r, ch);
    /* Park car / cdr across the recursive read_form / read_list calls (each
     * may trigger arbitrarily many allocations).  C locals would go stale
     * under a moving GC. */
    SP_PUSH(c, sp, 2);   /* sp[0]=car, sp[1]=cdr */
    sp[0] = read_form(c, r);
    reader_skip_ws(r);
    ch = reader_getc(r);
    if (ch == '.') {
        int next = reader_getc(r);
        if (is_delim(next)) {
            reader_ungetc(r, next);
            sp[1] = read_form(c, r);
            reader_skip_ws(r);
            int close = reader_getc(r);
            if (close != ')') scm_error(c, "expected ')' after dotted tail");
            VALUE rv = scm_cons(c, sp[0], sp[1]);
            SP_POP(c, sp);
            return rv;
        }
        // not a dotted-tail '.', push back both characters and treat as identifier
        reader_ungetc(r, next);
        reader_ungetc(r, '.');
    } else {
        reader_ungetc(r, ch);
    }
    sp[1] = read_list(c, r);
    VALUE rv = scm_cons(c, sp[0], sp[1]);
    SP_POP(c, sp);
    return rv;
}

static VALUE
read_string(CTX *c, struct reader *r)
{
    char buf[8192];
    size_t n = 0;
    for (;;) {
        int ch = reader_getc(r);
        if (ch == EOF) scm_error(c, "unexpected EOF in string");
        if (ch == '"') break;
        if (ch == '\\') {
            int esc = reader_getc(r);
            switch (esc) {
            case 'n':  ch = '\n'; break;
            case 't':  ch = '\t'; break;
            case 'r':  ch = '\r'; break;
            case '\\': ch = '\\'; break;
            case '"':  ch = '"';  break;
            case '0':  ch = '\0'; break;
            default:   ch = esc;  break;
            }
        }
        if (n + 1 >= sizeof(buf)) scm_error(c, "string literal too long");
        buf[n++] = (char)ch;
    }
    buf[n] = '\0';
    return scm_make_string(c, buf, n);
}

static VALUE
read_hash(CTX *c, struct reader *r)
{
    int ch = reader_getc(r);
    if (ch == 't') return SCM_TRUE;
    if (ch == 'f') return SCM_FALSE;
    if (ch == '(') {
        // vector
        /* Park `list` across scm_make_vector — it's a heap pair chain we
         * must keep alive while the vec sobj + items[] are allocated.
         * Park `vec` too while we walk `list` reading car cells (no inner
         * alloc but a defensive root is cheap and survives future edits). */
        SP_PUSH(c, sp, 2);     /* sp[0]=list, sp[1]=vec */
        sp[0] = read_list(c, r);
        size_t len = 0;
        for (VALUE p = sp[0]; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr) len++;
        sp[1] = scm_make_vector(c, len, SCM_UNSPEC);
        {
            /* No further alloc — vobj/items_base stay valid for the loop. */
            struct sobj *vobj = SCM_PTR(sp[1]);
            char *items_base = (char *)vobj->vec.items - sizeof(AroObjectHeader);
            size_t i = 0;
            for (VALUE p = sp[0]; scm_is_pair(p); p = SCM_PTR(p)->pair.cdr, i++) {
                aro_gc_wb(c, items_base, &vobj->vec.items[i],
                          SCM_PTR(p)->pair.car);
            }
        }
        VALUE rv = sp[1];
        SP_POP(c, sp);
        return rv;
    }
    if (ch == '\\') {
        // character
        int first = reader_getc(r);
        if (first == EOF) scm_error(c, "EOF in #\\");
        // Read identifier-like name if first is alpha and next isn't delim.
        char buf[32];
        size_t n = 0;
        buf[n++] = (char)first;
        int next = reader_getc(r);
        if (isalpha(first) && !is_delim(next)) {
            buf[n++] = (char)next;
            for (;;) {
                int e = reader_getc(r);
                if (is_delim(e)) { reader_ungetc(r, e); break; }
                if (n + 1 >= sizeof(buf)) scm_error(c, "char name too long");
                buf[n++] = (char)e;
            }
            buf[n] = '\0';
            if (strcmp(buf, "space")   == 0) return scm_make_char(c, ' ');
            if (strcmp(buf, "newline") == 0) return scm_make_char(c, '\n');
            if (strcmp(buf, "tab")     == 0) return scm_make_char(c, '\t');
            if (strcmp(buf, "return")  == 0) return scm_make_char(c, '\r');
            if (strcmp(buf, "nul")     == 0) return scm_make_char(c, 0);
            if (strcmp(buf, "null")    == 0) return scm_make_char(c, 0);
            if (strcmp(buf, "delete")  == 0) return scm_make_char(c, 127);
            if (strcmp(buf, "escape")  == 0) return scm_make_char(c, 27);
            scm_error(c, "unknown character name #\\%s", buf);
        }
        reader_ungetc(r, next);
        return scm_make_char(c, (uint32_t)(unsigned char)first);
    }
    scm_error(c, "unsupported # syntax: #%c", ch);
}

static VALUE
read_atom(CTX *c, struct reader *r, int first)
{
    char buf[256];
    size_t n = 0;
    buf[n++] = (char)first;
    for (;;) {
        int ch = reader_getc(r);
        if (is_delim(ch)) { reader_ungetc(r, ch); break; }
        if (n + 1 >= sizeof(buf)) scm_error(c, "atom too long");
        buf[n++] = (char)ch;
    }
    buf[n] = '\0';

    // try parse number
    if (n > 0 && (isdigit((unsigned char)buf[0]) ||
                  ((buf[0] == '-' || buf[0] == '+') && n > 1 && (isdigit((unsigned char)buf[1]) || buf[1] == '.')) ||
                  (buf[0] == '.' && n > 1 && isdigit((unsigned char)buf[1])))) {
        // rational: P/Q (integer over integer)
        char *slash = strchr(buf, '/');
        if (slash) {
            *slash = '\0';
            mpz_t num, den;
            if (mpz_init_set_str(num, buf, 10) == 0) {
                if (mpz_init_set_str(den, slash + 1, 10) == 0 && mpz_sgn(den) != 0) {
                    VALUE rv = scm_make_rational_zz(c, num, den);
                    mpz_clear(num); mpz_clear(den);
                    return rv;
                }
                mpz_clear(num);
            }
            *slash = '/';   // restore
        }
        // integer: try int64 fast path, then bignum
        char *end;
        long long ll = strtoll(buf, &end, 10);
        if (*end == '\0' && errno != ERANGE) {
            return scm_make_int(c, (int64_t)ll);
        }
        // try mpz parse for large integers
        mpz_t z;
        if (mpz_init_set_str(z, buf, 10) == 0) {
            VALUE rv = scm_normalize_int(c, z); mpz_clear(z); return rv;
        }
        mpz_clear(z);
        // try double
        double d = strtod(buf, &end);
        if (*end == '\0') return scm_make_double(c, d);
    }
    return scm_intern(c, buf);
}

static VALUE
read_form(CTX *c, struct reader *r)
{
    reader_skip_ws(r);
    int ch = reader_getc(r);
    if (ch == EOF) return SCM_EOFV;
    switch (ch) {
    case '(': return read_list(c, r);
    case ')': scm_error(c, "unexpected ')'");
    case '"': return read_string(c, r);
    case '#': return read_hash(c, r);
    case '\'': {
        /* Park v + (v) across scm_intern + scm_cons allocations.  Evaluate
         * each alloc serially and re-park between, to avoid C-arg-order
         * surprises where scm_intern could trigger GC after sp[1] is read. */
        SP_PUSH(c, sp, 2);    /* sp[0]=v, sp[1]=tail-cons */
        sp[0] = read_form(c, r);
        sp[1] = scm_cons(c, sp[0], SCM_NIL);
        VALUE q = scm_intern(c, "quote");
        VALUE rv = scm_cons(c, q, sp[1]);
        SP_POP(c, sp);
        return rv;
    }
    case '`': {
        SP_PUSH(c, sp, 2);
        sp[0] = read_form(c, r);
        sp[1] = scm_cons(c, sp[0], SCM_NIL);
        VALUE q = scm_intern(c, "quasiquote");
        VALUE rv = scm_cons(c, q, sp[1]);
        SP_POP(c, sp);
        return rv;
    }
    case ',': {
        int next = reader_getc(r);
        const char *which = "unquote";
        if (next == '@') which = "unquote-splicing";
        else reader_ungetc(r, next);
        SP_PUSH(c, sp, 2);
        sp[0] = read_form(c, r);
        sp[1] = scm_cons(c, sp[0], SCM_NIL);
        VALUE q = scm_intern(c, which);
        VALUE rv = scm_cons(c, q, sp[1]);
        SP_POP(c, sp);
        return rv;
    }
    default:
        return read_atom(c, r, ch);
    }
}

VALUE
scm_read(CTX *c, FILE *fp)
{
    struct reader r = { .fp = fp, .ungot = -2 };
    return read_form(c, &r);
}

// Convenience: read all forms from a string into a (begin ...) list.
VALUE
scm_read_all_string(CTX *c, const char *src, size_t len)
{
    struct reader r = { .src = src, .len = len, .ungot = -2 };
    /* Precise rooting: park (head, last cons cell) on c->sp across the
     * cons allocations so a moving GC can rewrite them.  An interior
     * pointer `&pair.cdr` would go stale the instant scm_cons triggers a
     * GC and moves the owning sobj.  Strategy: hold the last cons cell
     * VALUE in sp[1], and update its .cdr via field access. */
    SP_PUSH(c, sp, 3);   /* sp[0]=head (forms),  sp[1]=last cell,  sp[2]=v */
    for (;;) {
        reader_skip_ws(&r);
        int ch = reader_getc(&r);
        if (ch == EOF) break;
        reader_ungetc(&r, ch);
        sp[2] = read_form(c, &r);
        if (sp[2] == SCM_EOFV) break;
        VALUE cell = scm_cons(c, sp[2], SCM_NIL);   /* sp[2] read AFTER alloc-safe park */
        if (sp[0] == 0 || sp[0] == SCM_NIL) {
            sp[0] = cell;
        } else {
            struct sobj *last = SCM_PTR(sp[1]);
            aro_gc_wb(c, last, (VALUE *)&last->pair.cdr, cell);
        }
        sp[1] = cell;
    }
    VALUE forms = (sp[0] == 0) ? SCM_NIL : sp[0];
    SP_POP(c, sp);
    return forms;
}
