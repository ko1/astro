// Runtime value model for AnPy (see value.h).
#include <gc.h>
#include <stdarg.h>
#include "node.h"

// --- heap allocation --------------------------------------------------

anpy_str *
anpy_str_new(const char *s, int32_t len)
{
    anpy_str *o = (anpy_str *)GC_MALLOC(sizeof(anpy_str) + (size_t)len + 1);
    o->hdr.kind = K_STR;
    o->len = len;
    if (s) memcpy(o->data, s, (size_t)len);
    o->data[len] = '\0';
    return o;
}

anpy_list *
anpy_list_new(int32_t len)
{
    anpy_list *o = (anpy_list *)GC_MALLOC(sizeof(anpy_list));
    o->hdr.kind = K_LIST;
    o->len = len;
    o->elems = len ? (VALUE *)GC_MALLOC(sizeof(VALUE) * (size_t)len) : NULL;
    for (int32_t i = 0; i < len; i++) o->elems[i] = ANPY_NONE;
    return o;
}

// --- arithmetic / + (overloaded for str/list) ------------------------

VALUE
anpy_add(CTX *c, VALUE a, VALUE b)
{
    if (IS_INT(a) && IS_INT(b)) return INT2VAL(VAL2INT(a) + VAL2INT(b));
    if (is_str(a) && is_str(b)) {
        anpy_str *sa = (anpy_str *)a, *sb = (anpy_str *)b;
        anpy_str *r = anpy_str_new(NULL, sa->len + sb->len);
        memcpy(r->data, sa->data, (size_t)sa->len);
        memcpy(r->data + sa->len, sb->data, (size_t)sb->len);
        return (VALUE)r;
    }
    if (is_list(a) && is_list(b)) {
        anpy_list *la = (anpy_list *)a, *lb = (anpy_list *)b;
        anpy_list *r = anpy_list_new(la->len + lb->len);
        for (int32_t i = 0; i < la->len; i++) r->elems[i] = la->elems[i];
        for (int32_t i = 0; i < lb->len; i++) r->elems[la->len + i] = lb->elems[i];
        return (VALUE)r;
    }
    anpy_runtime_error(c, "Operation on None");   // only None can reach here past the type-checker
    return ANPY_NONE;
}

// --- indexing ---------------------------------------------------------

VALUE
anpy_index(CTX *c, VALUE seq, VALUE idx)
{
    if (IS_NONE(seq)) { anpy_runtime_error(c, "Operation on None"); return ANPY_NONE; }
    int32_t i = (int32_t)VAL2INT(idx);
    if (is_str(seq)) {
        anpy_str *s = (anpy_str *)seq;
        if (i < 0 || i >= s->len) { anpy_runtime_error(c, "Index out of bounds"); return ANPY_NONE; }
        return (VALUE)anpy_str_new(s->data + i, 1);
    }
    anpy_list *l = (anpy_list *)seq;
    if (i < 0 || i >= l->len) { anpy_runtime_error(c, "Index out of bounds"); return ANPY_NONE; }
    return l->elems[i];
}

void
anpy_index_set(CTX *c, VALUE seq, VALUE idx, VALUE val)
{
    if (IS_NONE(seq)) { anpy_runtime_error(c, "Operation on None"); return; }
    anpy_list *l = (anpy_list *)seq;
    int32_t i = (int32_t)VAL2INT(idx);
    if (i < 0 || i >= l->len) { anpy_runtime_error(c, "Index out of bounds"); return; }
    l->elems[i] = val;
}

VALUE
anpy_len(CTX *c, VALUE v)
{
    if (is_str(v))  return INT2VAL(((anpy_str *)v)->len);
    if (is_list(v)) return INT2VAL(((anpy_list *)v)->len);
    if (IS_NONE(v)) { anpy_runtime_error(c, "Operation on None"); return INT2VAL(0); }
    anpy_runtime_error(c, "Invalid argument");
    return INT2VAL(0);
}

// --- equality (== / != for int, bool, str) ---------------------------

int32_t
anpy_strcmp_eq(VALUE a, VALUE b)
{
    anpy_str *sa = (anpy_str *)a, *sb = (anpy_str *)b;
    return sa->len == sb->len && memcmp(sa->data, sb->data, (size_t)sa->len) == 0;
}

bool
anpy_eq(CTX *c, VALUE a, VALUE b)
{
    (void)c;
    if (IS_INT(a) && IS_INT(b)) return a == b;
    if (IS_BOOL(a) && IS_BOOL(b)) return a == b;
    if (is_str(a) && is_str(b)) return anpy_strcmp_eq(a, b);
    return a == b;
}

// --- print / input ----------------------------------------------------

void
anpy_print(CTX *c, VALUE v)
{
    if (IS_INT(v))       printf("%ld\n", (long)VAL2INT(v));
    else if (v == ANPY_TRUE)  printf("True\n");
    else if (v == ANPY_FALSE) printf("False\n");
    else if (is_str(v))  { anpy_str *s = (anpy_str *)v; fwrite(s->data, 1, (size_t)s->len, stdout); putchar('\n'); }
    else anpy_runtime_error(c, "Invalid argument");
}

VALUE
anpy_input(CTX *c)
{
    (void)c;
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return (VALUE)anpy_str_new("", 0);
    return (VALUE)anpy_str_new(buf, (int32_t)strlen(buf));
}

// --- runtime error ----------------------------------------------------

void
anpy_runtime_error(CTX *c, const char *fmt, ...)
{
    fflush(stdout);
    va_list ap; va_start(ap, fmt);
    fputs("Exception: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    if (c) c->returning = false;
    extern int anpy_jmp_is_active(void);
    if (anpy_jmp_is_active()) longjmp(*anpy_get_jmp(), 1);
    exit(1);
}
