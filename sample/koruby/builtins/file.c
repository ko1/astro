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


