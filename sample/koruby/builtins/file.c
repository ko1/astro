/* File / IO — moved from builtins.c. */

/* ---------- File ---------- */
static VALUE file_read(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
    const char *path = korb_str_cstr(argv[0]);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        korb_raise(c, NULL, "no such file -- %s", path);
        return Qnil;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = korb_xmalloc_atomic(sz + 1);
    long got = (long)fread(buf, 1, sz, fp);
    if (got < 0) got = 0;
    buf[got] = 0;
    fclose(fp);
    return korb_str_new(buf, got);
}

/* Simple FILE* wrapper.  We keep the raw FILE* on a fresh T_OBJECT
 * via an ivar (`@__fp__`) holding the pointer cast to Integer.  Not
 * elegant — but enough to support `File.open(path, mode) { |f| f.gets }`
 * patterns commonly seen in ruby scripts. */
static const ID korb_io_fp_id_(void) {
    static ID cached = 0;
    if (!cached) cached = korb_intern("@__fp__");
    return cached;
}

static FILE *korb_io_fp(VALUE io) {
    if (SPECIAL_CONST_P(io)) return NULL;
    VALUE v = korb_ivar_get(io, korb_io_fp_id_());
    if (UNDEF_P(v) || NIL_P(v) || !FIXNUM_P(v)) return NULL;
    return (FILE *)(uintptr_t)FIX2LONG(v);
}

static VALUE korb_io_new(struct korb_class *klass, FILE *fp) {
    VALUE io = (VALUE)korb_object_new(klass);
    korb_ivar_set(io, korb_io_fp_id_(), INT2FIX((long)(uintptr_t)fp));
    return io;
}

static VALUE io_close(CTX *c, VALUE self, int argc, VALUE *argv) {
    FILE *fp = korb_io_fp(self);
    if (fp) {
        fclose(fp);
        korb_ivar_set(self, korb_io_fp_id_(), Qnil);
    }
    return Qnil;
}

static VALUE io_read(CTX *c, VALUE self, int argc, VALUE *argv) {
    FILE *fp = korb_io_fp(self);
    if (!fp) return Qnil;
    /* Read everything remaining (or `argc=1` length bytes). */
    if (argc >= 1 && FIXNUM_P(argv[0])) {
        long n = FIX2LONG(argv[0]);
        char *buf = korb_xmalloc_atomic(n + 1);
        long got = (long)fread(buf, 1, n, fp);
        if (got <= 0) return Qnil;
        buf[got] = 0;
        return korb_str_new(buf, got);
    }
    long cap = 4096, len = 0;
    char *buf = korb_xmalloc_atomic(cap);
    while (true) {
        size_t got = fread(buf + len, 1, cap - len, fp);
        len += (long)got;
        if (got == 0) break;
        if ((long)len == cap) {
            cap *= 2;
            char *nb = korb_xmalloc_atomic(cap);
            memcpy(nb, buf, len);
            buf = nb;
        }
    }
    return korb_str_new(buf, len);
}

static VALUE io_gets(CTX *c, VALUE self, int argc, VALUE *argv) {
    FILE *fp = korb_io_fp(self);
    if (!fp) return Qnil;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, fp);
    if (n <= 0) { free(line); return Qnil; }
    VALUE r = korb_str_new(line, n);
    free(line);
    return r;
}

static VALUE io_each_line(CTX *c, VALUE self, int argc, VALUE *argv) {
    FILE *fp = korb_io_fp(self);
    if (!fp) return self;
    bool has_block = korb_block_given();
    VALUE collected = has_block ? Qnil : korb_ary_new();
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, fp)) > 0) {
        VALUE l = korb_str_new(line, n);
        if (has_block) {
            korb_yield(c, 1, &l);
            if (c->state != KORB_NORMAL) { free(line); return Qnil; }
        } else {
            korb_ary_push(collected, l);
        }
    }
    free(line);
    return has_block ? self : collected;
}

static VALUE io_puts(CTX *c, VALUE self, int argc, VALUE *argv) {
    FILE *fp = korb_io_fp(self);
    if (!fp) return Qnil;
    if (argc == 0) { fputc('\n', fp); return Qnil; }
    for (int i = 0; i < argc; i++) {
        VALUE s = korb_to_s_dispatch(c, argv[i]);
        const struct korb_string *str = (const struct korb_string *)s;
        fwrite(str->ptr, 1, str->len, fp);
        if (str->len == 0 || str->ptr[str->len-1] != '\n') fputc('\n', fp);
    }
    return Qnil;
}

static VALUE io_write(CTX *c, VALUE self, int argc, VALUE *argv) {
    FILE *fp = korb_io_fp(self);
    if (!fp) return INT2FIX(0);
    long total = 0;
    for (int i = 0; i < argc; i++) {
        VALUE s = korb_to_s_dispatch(c, argv[i]);
        const struct korb_string *str = (const struct korb_string *)s;
        total += (long)fwrite(str->ptr, 1, str->len, fp);
    }
    return INT2FIX(total);
}

static VALUE io_print(CTX *c, VALUE self, int argc, VALUE *argv) {
    FILE *fp = korb_io_fp(self);
    if (!fp) return Qnil;
    for (int i = 0; i < argc; i++) {
        VALUE s = korb_to_s_dispatch(c, argv[i]);
        const struct korb_string *str = (const struct korb_string *)s;
        fwrite(str->ptr, 1, str->len, fp);
    }
    return Qnil;
}

static VALUE io_eof_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    FILE *fp = korb_io_fp(self);
    return KORB_BOOL(fp ? feof(fp) : true);
}

/* File.open(path[, mode]) [{ |f| ... }]
 * With a block: yield the IO, ensure close on exit, return block value.
 * Without a block: return the IO; caller must close. */
extern struct korb_class *korb_vm_file_class_(void);
static VALUE file_open(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
    const char *path = korb_str_cstr(argv[0]);
    const char *mode = "r";
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
        mode = korb_str_cstr(argv[1]);
    }
    FILE *fp = fopen(path, mode);
    if (!fp) {
        VALUE eErrno = korb_const_get(korb_vm->object_class, korb_intern("Errno"));
        if (UNDEF_P(eErrno) || !eErrno) eErrno = (VALUE)NULL;
        korb_raise(c, NULL, "Errno::ENOENT: no such file -- %s", path);
        return Qnil;
    }
    /* `self` here is the File class object — use it as the IO's class. */
    VALUE io = korb_io_new((struct korb_class *)self, fp);
    if (!korb_block_given()) return io;
    VALUE r = korb_yield(c, 1, &io);
    /* Always close on block exit, even on raise. */
    fclose(fp);
    korb_ivar_set(io, korb_io_fp_id_(), Qnil);
    return r;
}

/* File.write(path, str[, mode]) — write str to path, return bytes written. */
static VALUE file_write(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2 || BUILTIN_TYPE(argv[0]) != T_STRING) return INT2FIX(0);
    const char *path = korb_str_cstr(argv[0]);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        korb_raise(c, NULL, "could not open for writing: %s", path);
        return Qnil;
    }
    VALUE s = korb_to_s_dispatch(c, argv[1]);
    const struct korb_string *str = (const struct korb_string *)s;
    long got = (long)fwrite(str->ptr, 1, str->len, fp);
    fclose(fp);
    return INT2FIX(got);
}

static VALUE file_join(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE r = korb_str_new("", 0);
    for (int i = 0; i < argc; i++) {
        VALUE s = BUILTIN_TYPE(argv[i]) == T_STRING ? argv[i] : korb_to_s(argv[i]);
        if (i > 0) korb_str_concat(r, korb_str_new_cstr("/"));
        korb_str_concat(r, s);
    }
    return r;
}

static VALUE file_exist_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qfalse;
    return KORB_BOOL(korb_file_exists(korb_str_cstr(argv[0])));
}

static VALUE file_dirname(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new(".", 1);
    return korb_str_new_cstr(korb_dirname(korb_str_cstr(argv[0])));
}

static VALUE file_basename(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new("", 0);
    const char *s = korb_str_cstr(argv[0]);
    const char *slash = strrchr(s, '/');
    return korb_str_new_cstr(slash ? slash + 1 : s);
}

static VALUE file_extname(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new("", 0);
    const char *s = korb_str_cstr(argv[0]);
    const char *dot = strrchr(s, '.');
    if (!dot || dot == s) return korb_str_new("", 0);
    /* Don't include if dot is in dirname only */
    const char *slash = strrchr(s, '/');
    if (slash && dot < slash) return korb_str_new("", 0);
    return korb_str_new_cstr(dot);
}

static VALUE file_binread(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Same as File.read but ensures binary mode */
    return file_read(c, self, argc, argv);
}

static VALUE file_expand_path(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new("", 0);
    /* simplistic: if absolute, return as-is; else prepend dir */
    const char *s = korb_str_cstr(argv[0]);
    if (s[0] == '/') return argv[0];
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
        return korb_str_new_cstr(korb_join_path(korb_str_cstr(argv[1]), s));
    }
    return argv[0];
}

/* IO (stubbed via STDOUT / $stdout) */

#include <time.h>
VALUE proc_clock_gettime_stub(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double t = ts.tv_sec + ts.tv_nsec / 1e9;
    return korb_float_new(t);
}

VALUE time_now_stub(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* return Float seconds since epoch (we just use Process clock, not real epoch) */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double t = ts.tv_sec + ts.tv_nsec / 1e9;
    return korb_float_new(t);
}


