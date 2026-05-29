/* File / IO — moved from builtins.c. */

/* ---------- File ---------- */
/* Phase 8 RESULT 化: File.read(path) — RESULT-typed cfunc_r。  raise も
 * `return korb_raise(...)` で直接伝搬。 */
static RESULT file_read(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    if (argc < 1 || BUILTIN_TYPE(sp[-1]) != T_STRING) return RESULT_OK(Qnil);
    const char *path = korb_str_cstr(sp[-1]);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return korb_raise(c, NULL, "no such file -- %s", path);
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = korb_xmalloc_atomic(sz + 1);
    long got = (long)fread(buf, 1, sz, fp);
    if (got < 0) got = 0;
    buf[got] = 0;
    fclose(fp);
    return RESULT_OK(korb_str_new(c, sp, buf, got));
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

static VALUE korb_io_new(CTX *c, struct korb_class *klass, FILE *fp) {
    VALUE io = (VALUE)korb_object_new(c, c->sp_top, klass);
    korb_ivar_set(io, korb_io_fp_id_(), INT2FIX((long)(uintptr_t)fp));
    return io;
}

static RESULT io_close(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    FILE *fp = korb_io_fp(self);
    if (fp) {
        fclose(fp);
        korb_ivar_set(self, korb_io_fp_id_(), Qnil);
    }
    return RESULT_OK(Qnil);
}

static RESULT io_read(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    FILE *fp = korb_io_fp(self);
    if (!fp) return RESULT_OK(Qnil);
    /* Read everything remaining (or `argc=1` length bytes). */
    if (argc >= 1 && FIXNUM_P(argv[0])) {
        long n = FIX2LONG(argv[0]);
        char *buf = korb_xmalloc_atomic(n + 1);
        long got = (long)fread(buf, 1, n, fp);
        if (got <= 0) return RESULT_OK(Qnil);
        buf[got] = 0;
        return RESULT_OK(korb_str_new(c, c->sp_top, buf, got));
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
    return RESULT_OK(korb_str_new(c, c->sp_top, buf, len));
}

static RESULT io_gets(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    FILE *fp = korb_io_fp(self);
    if (!fp) {
        /* CRuby: if EOF/closed → $_ = nil. */
        korb_last_line_set(c, Qnil);
        return RESULT_OK(Qnil);
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, fp);
    if (n <= 0) { free(line); korb_last_line_set(c, Qnil); return RESULT_OK(Qnil); }
    VALUE r = korb_str_new(c, c->sp_top, line, n);
    free(line);
    korb_last_line_set(c, r);
    return RESULT_OK(r);
}

static RESULT io_each_line(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    FILE *fp = korb_io_fp(self);
    if (!fp) return RESULT_OK(self);
    bool has_block = korb_block_given(c);
    VALUE collected = has_block ? Qnil : korb_ary_new(c, c->sp_top);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, fp)) > 0) {
        VALUE l = korb_str_new(c, c->sp_top, line, n);
        korb_last_line_set(c, l);
        if (has_block) {
            RESULT _yr = korb_yield(c, 1, &l);
            if (_yr.state != KORB_NORMAL) { free(line); return _yr; }
        } else {
            korb_ary_push(collected, l);
        }
    }
    free(line);
    return RESULT_OK(has_block ? self : collected);
}

static RESULT io_puts(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    FILE *fp = korb_io_fp(self);
    if (!fp) return RESULT_OK(Qnil);
    if (argc == 0) { fputc('\n', fp); return RESULT_OK(Qnil); }
    for (int i = 0; i < argc; i++) {
        VALUE s = korb_to_s_dispatch(c, argv[i]);
        const struct korb_string *str = (const struct korb_string *)s;
        fwrite(str->ptr, 1, str->len, fp);
        if (str->len == 0 || str->ptr[str->len-1] != '\n') fputc('\n', fp);
    }
    return RESULT_OK(Qnil);
}

static RESULT io_write(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    FILE *fp = korb_io_fp(self);
    if (!fp) return RESULT_OK(INT2FIX(0));
    long total = 0;
    for (int i = 0; i < argc; i++) {
        VALUE s = korb_to_s_dispatch(c, argv[i]);
        const struct korb_string *str = (const struct korb_string *)s;
        total += (long)fwrite(str->ptr, 1, str->len, fp);
    }
    return RESULT_OK(INT2FIX(total));
}

static RESULT io_print(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    FILE *fp = korb_io_fp(self);
    if (!fp) return RESULT_OK(Qnil);
    for (int i = 0; i < argc; i++) {
        VALUE s = korb_to_s_dispatch(c, argv[i]);
        const struct korb_string *str = (const struct korb_string *)s;
        fwrite(str->ptr, 1, str->len, fp);
    }
    return RESULT_OK(Qnil);
}

static RESULT io_eof_p(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    FILE *fp = korb_io_fp(self);
    return RESULT_OK(KORB_BOOL(fp ? feof(fp) : true));
}

#include <unistd.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>

/* IO.pipe → [reader, writer] pair of IO objects.  Mirrors CRuby. */
RESULT io_class_pipe(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    int fds[2];
    if (pipe(fds) != 0) {
        return korb_raise(c, NULL, "IO.pipe failed: %s", strerror(errno));
    }
    FILE *r = fdopen(fds[0], "rb");
    FILE *w = fdopen(fds[1], "wb");
    if (!r || !w) {
        if (r) fclose(r); else close(fds[0]);
        if (w) fclose(w); else close(fds[1]);
        return korb_raise(c, NULL, "IO.pipe fdopen failed");
    }
    /* Unbuffer both ends so the writer's bytes immediately reach the
     * kernel pipe and the reader's read/readpartial unblock without
     * waiting for a newline. */
    setvbuf(w, NULL, _IONBF, 0);
    setvbuf(r, NULL, _IONBF, 0);
    VALUE rio = korb_io_new(c, (struct korb_class *)self, r);
    VALUE wio = korb_io_new(c, (struct korb_class *)self, w);
    VALUE arr = korb_ary_new_capa(c, sp, 2);
    korb_ary_push(arr, rio);
    korb_ary_push(arr, wio);
    return RESULT_OK(arr);
}

/* IO.select(read_array, write_array=nil, error_array=nil, timeout=nil)
 * → [readable, writable, errored] arrays, or nil on timeout. */
static void korb_select_fill_set(VALUE arr, fd_set *set, int *maxfd) {
    if (NIL_P(arr) || SPECIAL_CONST_P(arr) || BUILTIN_TYPE(arr) != T_ARRAY) return;
    struct korb_array *a = (struct korb_array *)arr;
    for (long i = 0; i < a->len; i++) {
        FILE *fp = korb_io_fp(a->ptr[i]);
        if (!fp) continue;
        int fd = fileno(fp);
        if (fd < 0) continue;
        FD_SET(fd, set);
        if (fd > *maxfd) *maxfd = fd;
    }
}

static VALUE korb_select_collect_ready(CTX *c, VALUE arr, fd_set *set) {
    VALUE out = korb_ary_new(c, c->sp_top);
    if (NIL_P(arr) || SPECIAL_CONST_P(arr) || BUILTIN_TYPE(arr) != T_ARRAY) return out;
    struct korb_array *a = (struct korb_array *)arr;
    for (long i = 0; i < a->len; i++) {
        FILE *fp = korb_io_fp(a->ptr[i]);
        if (!fp) continue;
        int fd = fileno(fp);
        if (fd < 0) continue;
        if (FD_ISSET(fd, set)) korb_ary_push(out, a->ptr[i]);
    }
    return out;
}

/* IO.popen(cmd[, mode]) [{|io| ...}]
 * - With block: yield reader IO; close + waitpid on exit; return block value.
 * - Without block: return reader IO; caller must close.
 * Mode "r" (default) reads from cmd's stdout; "w" writes to cmd's stdin. */
RESULT io_class_popen(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) {
        return korb_raise(c, NULL, "IO.popen: command String required");
    }
    const char *cmd = korb_str_cstr(argv[0]);
    const char *mode = "r";
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
        mode = korb_str_cstr(argv[1]);
    }
    FILE *fp = popen(cmd, mode);
    if (!fp) {
        return korb_raise(c, NULL, "popen failed: %s", strerror(errno));
    }
    VALUE io = korb_io_new(c, (struct korb_class *)self, fp);
    if (!korb_block_given(c)) return RESULT_OK(io);
    VALUE r = UNWRAP(korb_yield_r(c, 1, &io));
    pclose(fp);
    korb_ivar_set(io, korb_io_fp_id_(), Qnil);
    return RESULT_OK(r);
}

/* IO.copy_stream(src, dst[, len[, src_offset]]) — copy bytes between
 * IO/path arguments.  Returns the number of bytes copied. */
static long korb_copy_fd_(int from_fd, int to_fd, long max_len) {
    char buf[8192];
    long total = 0;
    while (max_len < 0 || total < max_len) {
        size_t want = sizeof(buf);
        if (max_len >= 0 && (long)want > max_len - total) want = (size_t)(max_len - total);
        ssize_t got = read(from_fd, buf, want);
        if (got < 0) return -1;
        if (got == 0) break;
        ssize_t written = 0;
        while (written < got) {
            ssize_t w = write(to_fd, buf + written, (size_t)(got - written));
            if (w < 0) return -1;
            written += w;
        }
        total += got;
    }
    return total;
}

RESULT io_class_copy_stream(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    if (argc < 2) {
        return korb_raise(c, NULL, "IO.copy_stream(src, dst[, len[, src_offset]])");
    }
    long max_len = (argc >= 3 && FIXNUM_P(argv[2])) ? FIX2LONG(argv[2]) : -1;
    int src_fd = -1; bool src_close = false;
    if (BUILTIN_TYPE(argv[0]) == T_STRING) {
        src_fd = open(korb_str_cstr(argv[0]), O_RDONLY);
        if (src_fd < 0) {
            return korb_raise(c, NULL, "open(%s) failed: %s",
                       korb_str_cstr(argv[0]), strerror(errno));
        }
        src_close = true;
    } else {
        FILE *fp = korb_io_fp(argv[0]);
        if (!fp) {
            return korb_raise(c, NULL, "IO.copy_stream: src must be IO or path");
        }
        fflush(fp);
        src_fd = fileno(fp);
    }
    int dst_fd = -1; bool dst_close = false;
    if (BUILTIN_TYPE(argv[1]) == T_STRING) {
        dst_fd = open(korb_str_cstr(argv[1]),
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dst_fd < 0) {
            if (src_close) close(src_fd);
            return korb_raise(c, NULL, "open(%s) failed: %s",
                       korb_str_cstr(argv[1]), strerror(errno));
        }
        dst_close = true;
    } else {
        FILE *fp = korb_io_fp(argv[1]);
        if (!fp) {
            if (src_close) close(src_fd);
            return korb_raise(c, NULL, "IO.copy_stream: dst must be IO or path");
        }
        fflush(fp);
        dst_fd = fileno(fp);
    }
    if (argc >= 4 && FIXNUM_P(argv[3])) {
        if (lseek(src_fd, (off_t)FIX2LONG(argv[3]), SEEK_SET) < 0) {
            if (src_close) close(src_fd);
            if (dst_close) close(dst_fd);
            return korb_raise(c, NULL, "lseek failed: %s", strerror(errno));
        }
    }
    long n = korb_copy_fd_(src_fd, dst_fd, max_len);
    if (src_close) close(src_fd);
    if (dst_close) close(dst_fd);
    if (n < 0) {
        return korb_raise(c, NULL, "IO.copy_stream failed: %s", strerror(errno));
    }
    return RESULT_OK(INT2FIX(n));
}

/* IO#tty? — true iff backed by a terminal fd. */
static RESULT io_tty_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    FILE *fp = korb_io_fp(self);
    if (!fp) return RESULT_OK(Qfalse);
    return RESULT_OK(KORB_BOOL(isatty(fileno(fp))));
}

/* IO#fileno — underlying fd, useful for IO.select sanity etc. */
static RESULT io_fileno(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    FILE *fp = korb_io_fp(self);
    if (!fp) return RESULT_OK(INT2FIX(-1));
    return RESULT_OK(INT2FIX(fileno(fp)));
}

RESULT io_class_select(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    VALUE rs = (argc >= 1) ? argv[0] : Qnil;
    VALUE ws = (argc >= 2) ? argv[1] : Qnil;
    VALUE es = (argc >= 3) ? argv[2] : Qnil;
    fd_set rset, wset, eset;
    FD_ZERO(&rset); FD_ZERO(&wset); FD_ZERO(&eset);
    int maxfd = -1;
    korb_select_fill_set(rs, &rset, &maxfd);
    korb_select_fill_set(ws, &wset, &maxfd);
    korb_select_fill_set(es, &eset, &maxfd);
    struct timeval tv;
    struct timeval *tvp = NULL;
    if (argc >= 4 && !NIL_P(argv[3])) {
        double t;
        if (FIXNUM_P(argv[3])) t = (double)FIX2LONG(argv[3]);
        else if (KORB_IS_FLOAT(argv[3])) t = korb_num2dbl(argv[3]);
        else t = 0.0;
        tv.tv_sec = (long)t;
        tv.tv_usec = (long)((t - (long)t) * 1.0e6);
        tvp = &tv;
    }
    int n = select(maxfd + 1, &rset, &wset, &eset, tvp);
    if (n < 0) {
        return korb_raise(c, NULL, "IO.select failed: %s", strerror(errno));
    }
    if (n == 0) return RESULT_OK(Qnil);
    VALUE ret = korb_ary_new_capa(c, sp, 3);
    korb_ary_push(ret, korb_select_collect_ready(c, rs, &rset));
    korb_ary_push(ret, korb_select_collect_ready(c, ws, &wset));
    korb_ary_push(ret, korb_select_collect_ready(c, es, &eset));
    return RESULT_OK(ret);
}

/* File.open(path[, mode]) [{ |f| ... }]
 * With a block: yield the IO, ensure close on exit, return block value.
 * Without a block: return the IO; caller must close. */
extern struct korb_class *korb_vm_file_class_(void);
static RESULT file_open(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(Qnil);
    const char *path = korb_str_cstr(argv[0]);
    const char *mode = "r";
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
        mode = korb_str_cstr(argv[1]);
    }
    FILE *fp = fopen(path, mode);
    if (!fp) {
        VALUE eErrno = korb_const_get(KORB_VM(c)->object_class, korb_intern("Errno"));
        if (UNDEF_P(eErrno) || !eErrno) eErrno = (VALUE)NULL;
        return korb_raise(c, NULL, "Errno::ENOENT: no such file -- %s", path);
    }
    /* `self` here is the File class object — use it as the IO's class. */
    VALUE io = korb_io_new(c, (struct korb_class *)self, fp);
    if (!korb_block_given(c)) return RESULT_OK(io);
    VALUE r = UNWRAP(korb_yield_r(c, 1, &io));
    /* Always close on block exit, even on raise (UNWRAP already propagates
     * raise, leaving fp leaked — TODO: ensure/rescue equivalent). */
    fclose(fp);
    korb_ivar_set(io, korb_io_fp_id_(), Qnil);
    return RESULT_OK(r);
}

/* File.write(path, str[, mode]) — write str to path, return bytes written. */
static RESULT file_write(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    if (argc < 2 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(INT2FIX(0));
    const char *path = korb_str_cstr(argv[0]);
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return korb_raise(c, NULL, "could not open for writing: %s", path);
    }
    VALUE s = korb_to_s_dispatch(c, argv[1]);
    const struct korb_string *str = (const struct korb_string *)s;
    long got = (long)fwrite(str->ptr, 1, str->len, fp);
    fclose(fp);
    return RESULT_OK(INT2FIX(got));
}

static RESULT file_join(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    /* Pin r (result) and per-iter s/sep against the cascade of
     * korb_str_new / korb_to_s / korb_str_concat allocations that
     * each fire GC under STRESS. */
    VALUE ret = Qnil;
    ARO_ROOT_SCOPE_START(c, rs, 3) {
        rs[0] = korb_str_new(c, c->sp_top, "", 0);  /* r */
        for (int i = 0; i < argc; i++) {
            rs[1] = argv[i];
            if (BUILTIN_TYPE(rs[1]) != T_STRING) rs[1] = korb_to_s(c, c->sp_top, rs[1]);
            if (i > 0) {
                rs[2] = korb_str_new_cstr(c, c->sp_top, "/");
                korb_str_concat(c, c->sp_top, rs[0], rs[2]);
            }
            korb_str_concat(c, c->sp_top, rs[0], rs[1]);
        }
        ret = rs[0];
    } ARO_ROOT_SCOPE_END(c, rs);
    return RESULT_OK(ret);
}

static RESULT file_exist_p(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(Qfalse);
    return RESULT_OK(KORB_BOOL(korb_file_exists(korb_str_cstr(argv[0]))));
}

#include <sys/stat.h>
static RESULT file_directory_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(Qfalse);
    struct stat st;
    if (stat(korb_str_cstr(argv[0]), &st) != 0) return RESULT_OK(Qfalse);
    return RESULT_OK(KORB_BOOL(S_ISDIR(st.st_mode)));
}
static RESULT file_file_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(Qfalse);
    struct stat st;
    if (stat(korb_str_cstr(argv[0]), &st) != 0) return RESULT_OK(Qfalse);
    return RESULT_OK(KORB_BOOL(S_ISREG(st.st_mode)));
}
static RESULT file_size(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(INT2FIX(0));
    struct stat st;
    if (stat(korb_str_cstr(argv[0]), &st) != 0) {
        return korb_raise(c, NULL, "no such file -- %s", korb_str_cstr(argv[0]));
    }
    return RESULT_OK(INT2FIX((long)st.st_size));
}

static RESULT file_unlink(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    long n = 0;
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_STRING) continue;
        if (unlink(korb_str_cstr(argv[i])) != 0) {
            return korb_raise(c, NULL, "unlink failed: %s -- %s",
                       strerror(errno), korb_str_cstr(argv[i]));
        }
        n++;
    }
    return RESULT_OK(INT2FIX(n));
}

static RESULT file_rename(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    if (argc < 2 || BUILTIN_TYPE(argv[0]) != T_STRING ||
        BUILTIN_TYPE(argv[1]) != T_STRING) {
        return korb_raise(c, NULL, "File.rename: two String args expected");
    }
    if (rename(korb_str_cstr(argv[0]), korb_str_cstr(argv[1])) != 0) {
        return korb_raise(c, NULL, "rename failed: %s", strerror(errno));
    }
    return RESULT_OK(INT2FIX(0));
}

static RESULT file_chmod(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    if (argc < 2 || !FIXNUM_P(argv[0])) {
        return korb_raise(c, NULL, "File.chmod(mode, *paths)");
    }
    long mode = FIX2LONG(argv[0]);
    long n = 0;
    for (int i = 1; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_STRING) continue;
        if (chmod(korb_str_cstr(argv[i]), (mode_t)mode) != 0) {
            return korb_raise(c, NULL, "chmod failed: %s", strerror(errno));
        }
        n++;
    }
    return RESULT_OK(INT2FIX(n));
}

#include <limits.h>
static RESULT file_realpath(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(Qnil);
    /* CRuby: File.realpath(path, [base_dir]) — when path is relative
     * and base_dir is given, resolve path relative to base_dir before
     * calling realpath.  Without this 2-arg support, ~/ruby spec
     * helper's `File.realpath("fixtures/code", root)` raises with
     * "realpath failed: No such file or directory". */
    const char *path = korb_str_cstr(argv[0]);
    char abs_buf[PATH_MAX];
    const char *resolved_in = path;
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING && path[0] != '/') {
        const char *base = korb_str_cstr(argv[1]);
        size_t bl = strlen(base);
        size_t pl = strlen(path);
        if (bl + 1 + pl + 1 < sizeof(abs_buf)) {
            memcpy(abs_buf, base, bl);
            abs_buf[bl] = '/';
            memcpy(abs_buf + bl + 1, path, pl + 1);
            resolved_in = abs_buf;
        }
    }
    char buf[PATH_MAX];
    if (!realpath(resolved_in, buf)) {
        return korb_raise(c, NULL, "realpath failed: %s -- %s",
                   strerror(errno), resolved_in);
    }
    return RESULT_OK(korb_str_new_cstr(c, sp, buf));
}

static RESULT file_dirname(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(korb_str_new(c, c->sp_top, ".", 1));
    return RESULT_OK(korb_str_new_cstr(c, c->sp_top, korb_dirname(korb_str_cstr(argv[0]))));
}

static RESULT file_basename(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(korb_str_new(c, c->sp_top, "", 0));
    const char *s = korb_str_cstr(argv[0]);
    const char *slash = strrchr(s, '/');
    return RESULT_OK(korb_str_new_cstr(c, c->sp_top, slash ? slash + 1 : s));
}

static RESULT file_extname(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(korb_str_new(c, c->sp_top, "", 0));
    const char *s = korb_str_cstr(argv[0]);
    const char *dot = strrchr(s, '.');
    if (!dot || dot == s) return RESULT_OK(korb_str_new(c, c->sp_top, "", 0));
    /* Don't include if dot is in dirname only */
    const char *slash = strrchr(s, '/');
    if (slash && dot < slash) return RESULT_OK(korb_str_new(c, c->sp_top, "", 0));
    return RESULT_OK(korb_str_new_cstr(c, c->sp_top, dot));
}

static RESULT file_binread(CTX *c, int argc, VALUE *sp) {
    /* Same as File.read but ensures binary mode */
    return file_read(c, argc, sp);
}

static RESULT file_expand_path(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(korb_str_new(c, c->sp_top, "", 0));
    /* simplistic: if absolute, return RESULT_OK(as-is); else prepend dir */
    const char *s = korb_str_cstr(argv[0]);
    if (s[0] == '/') return RESULT_OK(argv[0]);
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
        return RESULT_OK(korb_str_new_cstr(c, c->sp_top, korb_join_path(korb_str_cstr(argv[1]), s)));
    }
    return RESULT_OK(argv[0]);
}

/* ---------- Dir ---------- */
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

static RESULT dir_mkdir(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) {
        return korb_raise(c, NULL, "Dir.mkdir(path[, mode])");
    }
    long mode = (argc >= 2 && FIXNUM_P(argv[1])) ? FIX2LONG(argv[1]) : 0755;
    if (mkdir(korb_str_cstr(argv[0]), (mode_t)mode) != 0) {
        return korb_raise(c, NULL, "mkdir failed: %s -- %s",
                   strerror(errno), korb_str_cstr(argv[0]));
    }
    return RESULT_OK(INT2FIX(0));
}

static RESULT dir_rmdir(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(Qnil);
    if (rmdir(korb_str_cstr(argv[0])) != 0) {
        return korb_raise(c, NULL, "rmdir failed: %s -- %s",
                   strerror(errno), korb_str_cstr(argv[0]));
    }
    return RESULT_OK(INT2FIX(0));
}

static RESULT dir_pwd(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return RESULT_OK(korb_str_new_cstr(c, c->sp_top, "."));
    return RESULT_OK(korb_str_new_cstr(c, c->sp_top, buf));
}

static RESULT dir_entries(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(korb_ary_new(c, sp));
    const char *path = korb_str_cstr(argv[0]);
    DIR *d = opendir(path);
    if (!d) {
        return korb_raise(c, NULL, "no such directory -- %s", path);
    }
    VALUE out = korb_ary_new(c, sp);
    struct dirent *de;
    while ((de = readdir(d))) {
        korb_ary_push(out, korb_str_new_cstr(c, sp, de->d_name));
    }
    closedir(d);
    return RESULT_OK(out);
}

static RESULT dir_chdir(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(Qnil);
    const char *path = korb_str_cstr(argv[0]);
    if (korb_block_given(c)) {
        char prev[4096];
        if (!getcwd(prev, sizeof(prev))) return RESULT_OK(Qnil);
        if (chdir(path) != 0) {
            return korb_raise(c, NULL, "could not chdir to %s", path);
        }
        VALUE r = UNWRAP(korb_yield_r(c, 0, NULL));
        if (chdir(prev) != 0) { /* unlikely; best-effort restore */ }
        return RESULT_OK(r);
    }
    if (chdir(path) != 0) {
        return korb_raise(c, NULL, "could not chdir to %s", path);
    }
    return RESULT_OK(INT2FIX(0));
}

/* Dir.glob — minimal pattern matching: literal paths, single star
 * (no /), and the common recursive double-star form.  Real fnmatch
 * would need a tracked dependency; keep this small. */
static bool korb_glob_simple_match(const char *pat, const char *name) {
    /* Walk pat; * matches any run of non-/ chars.  No bracket exprs. */
    while (*pat && *name) {
        if (*pat == '*') {
            pat++;
            if (!*pat) {
                /* trailing * — match rest unless name has '/' */
                while (*name && *name != '/') name++;
                return *name == 0;
            }
            while (*name) {
                if (korb_glob_simple_match(pat, name)) return true;
                if (*name == '/') break;
                name++;
            }
            return false;
        }
        if (*pat != *name) return false;
        pat++; name++;
    }
    while (*pat == '*') pat++;
    return !*pat && !*name;
}

static void korb_glob_walk(CTX *c, const char *dir, const char *pat, VALUE out, bool recursive) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (korb_glob_simple_match(pat, de->d_name)) {
            korb_ary_push(out, korb_str_new_cstr(c, c->sp_top, path));
        }
        if (recursive) {
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                korb_glob_walk(c, path, pat, out, true);
            }
        }
    }
    closedir(d);
}

static RESULT dir_glob(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return RESULT_OK(korb_ary_new(c, c->sp_top));
    const char *pat = korb_str_cstr(argv[0]);
    VALUE out = korb_ary_new(c, c->sp_top);
    /* Detect double-star + slash + rest recursive form. */
    if (strncmp(pat, "**/", 3) == 0) {
        korb_glob_walk(c, ".", pat + 3, out, true);
        return RESULT_OK(out);
    }
    /* Otherwise look in `.` if no /; else split last component. */
    const char *slash = strrchr(pat, '/');
    if (!slash) {
        korb_glob_walk(c, ".", pat, out, false);
    } else {
        char dir[4096];
        long dl = slash - pat;
        if (dl >= (long)sizeof(dir)) dl = sizeof(dir) - 1;
        memcpy(dir, pat, dl); dir[dl] = 0;
        korb_glob_walk(c, dir, slash + 1, out, false);
    }
    return RESULT_OK(out);
}

/* ---------- Process ---------- */
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

static RESULT process_pid(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    return RESULT_OK(INT2FIX((long)getpid()));
}

/* Build a NUL-terminated argv array from `cmd, *args`.  Handles the
 * shell-form `system("ls -l")` (single string with spaces → /bin/sh -c)
 * and the exec-form `system("ls", "-l")` (no shell). */
static char **build_exec_argv(CTX *c, VALUE *strs, int n, bool *use_shell) {
    *use_shell = false;
    if (n == 0) return NULL;
    /* Single-string with shell metachars → shell form. */
    if (n == 1 && !SPECIAL_CONST_P(strs[0]) && BUILTIN_TYPE(strs[0]) == T_STRING) {
        const char *s = ((struct korb_string *)strs[0])->ptr;
        for (long i = 0; s[i]; i++) {
            char ch = s[i];
            if (ch == ' ' || ch == '\t' || ch == '*' || ch == '?' ||
                ch == '$' || ch == '|' || ch == '&' || ch == '<' ||
                ch == '>' || ch == '(' || ch == ')' || ch == '[' ||
                ch == ']' || ch == '{' || ch == '}' || ch == '`' ||
                ch == ';' || ch == '\\' || ch == '"' || ch == '\'' ||
                ch == '~' || ch == '#') {
                *use_shell = true;
                break;
            }
        }
        if (*use_shell) {
            char **argv = korb_xmalloc(sizeof(char *) * 4);
            argv[0] = (char *)"/bin/sh";
            argv[1] = (char *)"-c";
            argv[2] = (char *)((struct korb_string *)strs[0])->ptr;
            argv[3] = NULL;
            return argv;
        }
    }
    /* exec form: each arg is one argv element. */
    char **argv = korb_xmalloc(sizeof(char *) * (n + 1));
    for (int i = 0; i < n; i++) {
        if (SPECIAL_CONST_P(strs[i]) || BUILTIN_TYPE(strs[i]) != T_STRING) {
            VALUE s = korb_to_s_dispatch(c, strs[i]);
            argv[i] = (char *)((struct korb_string *)s)->ptr;
        } else {
            argv[i] = (char *)((struct korb_string *)strs[i])->ptr;
        }
    }
    argv[n] = NULL;
    return argv;
}

/* Process::Status — minimal struct exposed via $? after system() etc. */
static VALUE make_process_status(CTX *c, int wstatus, pid_t pid) {
    VALUE cStatus = korb_const_get(KORB_VM(c)->object_class, korb_intern("Process"));
    VALUE cs = korb_const_get((struct korb_class *)cStatus, korb_intern("Status"));
    if (UNDEF_P(cs) || NIL_P(cs)) cs = (VALUE)KORB_VM(c)->object_class;
    VALUE obj = korb_object_new(c, c->sp_top, (struct korb_class *)cs);
    korb_ivar_set(obj, korb_intern("@pid"), INT2FIX((long)pid));
    int exit_status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
    korb_ivar_set(obj, korb_intern("@exitstatus"), INT2FIX((long)exit_status));
    korb_ivar_set(obj, korb_intern("@success"), KORB_BOOL(exit_status == 0));
    korb_ivar_set(obj, korb_intern("@signaled"), KORB_BOOL(WIFSIGNALED(wstatus)));
    if (WIFSIGNALED(wstatus)) {
        korb_ivar_set(obj, korb_intern("@termsig"), INT2FIX((long)WTERMSIG(wstatus)));
    }
    korb_ivar_set(obj, korb_intern("@to_i"), INT2FIX((long)wstatus));
    return obj;
}

/* Kernel#system(cmd, *args) — run a subprocess.  Returns true if exit
 * status is 0, false if non-zero, nil if the process failed to start.
 * Also sets $? to a Process::Status. */
static RESULT kernel_system(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    if (argc < 1) {
        return korb_raise_argument_error(c, "wrong number of arguments");
    }
    bool use_shell;
    char **xargv = build_exec_argv(c, argv, argc, &use_shell);
    if (!xargv) return RESULT_OK(Qnil);
    pid_t pid = fork();
    if (pid < 0) return RESULT_OK(Qnil);
    if (pid == 0) {
        execvp(xargv[0], xargv);
        _exit(127);
    }
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    VALUE st = make_process_status(c, wstatus, pid);
    korb_gvar_set(korb_intern("$?"), st);
    if (WIFEXITED(wstatus)) {
        return RESULT_OK(WEXITSTATUS(wstatus) == 0 ? Qtrue : Qfalse);
    }
    return RESULT_OK(Qfalse);
}

/* Kernel#`cmd` (backtick) — run command, return stdout as a String. */
static RESULT kernel_xstring(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1 || SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_STRING)
        return RESULT_OK(korb_str_new_cstr(c, c->sp_top, ""));
    int pipefd[2];
    if (pipe(pipefd) < 0) return RESULT_OK(korb_str_new_cstr(c, c->sp_top, ""));
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return RESULT_OK(korb_str_new_cstr(c, c->sp_top, ""));
    }
    if (pid == 0) {
        dup2(pipefd[1], 1);
        close(pipefd[0]); close(pipefd[1]);
        const char *cmd = ((struct korb_string *)argv[0])->ptr;
        execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    char buf[4096];
    VALUE r = korb_str_new_cstr(c, c->sp_top, "");
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        korb_str_concat(c, c->sp_top, r, korb_str_new(c, c->sp_top, buf, n));
    }
    close(pipefd[0]);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    korb_gvar_set(korb_intern("$?"), make_process_status(c, wstatus, pid));
    return RESULT_OK(r);
}

/* Kernel#exec — replace the current process. */
static RESULT kernel_exec(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE *argv = sp - argc;
    if (argc < 1) return RESULT_OK(Qnil);
    bool use_shell;
    char **xargv = build_exec_argv(c, argv, argc, &use_shell);
    if (!xargv) return RESULT_OK(Qnil);
    execvp(xargv[0], xargv);
    /* Reach here only if exec failed. */
    VALUE eErrno = korb_const_get(KORB_VM(c)->object_class, korb_intern("Errno"));
    if (!UNDEF_P(eErrno) && !NIL_P(eErrno)) {
        return korb_raise(c, NULL, "exec failed: %s", xargv[0]);
    }
    return RESULT_OK(Qnil);
}

/* Process.spawn(cmd, *args) — fork + exec, return pid (don't wait). */
static RESULT process_spawn(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    bool use_shell;
    char **xargv = build_exec_argv(c, argv, argc, &use_shell);
    if (!xargv) return RESULT_OK(Qnil);
    pid_t pid = fork();
    if (pid < 0) return RESULT_OK(Qnil);
    if (pid == 0) {
        execvp(xargv[0], xargv);
        _exit(127);
    }
    return RESULT_OK(INT2FIX((long)pid));
}

/* Process.fork { ... } — fork; in child, run block then exit.  In
 * parent, return child pid. */
static RESULT process_fork(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    pid_t pid = fork();
    if (pid < 0) return RESULT_OK(Qnil);
    if (pid == 0) {
        RESULT _yr = RESULT_OK(Qnil);
        if (korb_block_given(c)) _yr = korb_yield(c, 0, NULL);
        if (_yr.state == KORB_RAISE) {
            VALUE s = korb_inspect(c, c->sp_top, _yr.value);
            fprintf(stderr, "fork child: %s\n", korb_str_cstr(s));
            _exit(1);
        }
        _exit(0);
    }
    return RESULT_OK(INT2FIX((long)pid));
}

/* Process.wait([pid [, flags]]) — waitpid; sets $? and returns the pid
 * (or -1 on error). */
static RESULT process_wait(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    pid_t want = -1;
    int flags = 0;
    if (argc >= 1 && FIXNUM_P(argv[0])) want = (pid_t)FIX2LONG(argv[0]);
    if (argc >= 2 && FIXNUM_P(argv[1])) flags = (int)FIX2LONG(argv[1]);
    int wstatus = 0;
    pid_t got = waitpid(want, &wstatus, flags);
    if (got <= 0) return RESULT_OK(Qnil);
    korb_gvar_set(korb_intern("$?"), make_process_status(c, wstatus, got));
    return RESULT_OK(INT2FIX((long)got));
}

static RESULT process_kill(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    if (argc < 2) return RESULT_OK(INT2FIX(0));
    int sig = 0;
    if (FIXNUM_P(argv[0])) {
        sig = (int)FIX2LONG(argv[0]);
    } else if (SYMBOL_P(argv[0])) {
        const char *n = korb_id_name(korb_sym2id(argv[0]));
        if (!strcmp(n, "INT")) sig = SIGINT;
        else if (!strcmp(n, "TERM")) sig = SIGTERM;
        else if (!strcmp(n, "KILL")) sig = SIGKILL;
        else if (!strcmp(n, "USR1")) sig = SIGUSR1;
        else if (!strcmp(n, "USR2")) sig = SIGUSR2;
        else if (!strcmp(n, "HUP")) sig = SIGHUP;
        else if (!strcmp(n, "QUIT")) sig = SIGQUIT;
    }
    int sent = 0;
    for (int i = 1; i < argc; i++) {
        if (FIXNUM_P(argv[i])) {
            if (kill((pid_t)FIX2LONG(argv[i]), sig) == 0) sent++;
        }
    }
    return RESULT_OK(INT2FIX((long)sent));
}

/* Process::Status methods. */
static RESULT pstatus_exitstatus(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_ivar_get(self, korb_intern("@exitstatus")));
}
static RESULT pstatus_pid(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_ivar_get(self, korb_intern("@pid")));
}
static RESULT pstatus_success_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_ivar_get(self, korb_intern("@success")));
}
static RESULT pstatus_signaled_p(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_ivar_get(self, korb_intern("@signaled")));
}
static RESULT pstatus_termsig(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    VALUE v = korb_ivar_get(self, korb_intern("@termsig"));
    return RESULT_OK(UNDEF_P(v) ? Qnil : v);
}
static RESULT pstatus_to_i(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    return RESULT_OK(korb_ivar_get(self, korb_intern("@to_i")));
}

/* Signal — minimal: trap (stub: stores handler), list (constant map). */
static struct {
    int signum;
    VALUE handler;
} g_signal_handlers[32] = {{0}};
static int g_signal_handlers_cnt = 0;

static int signal_name_to_num(const char *n) {
    if (!n) return -1;
    /* Allow "SIGFOO" or "FOO". */
    if (strncmp(n, "SIG", 3) == 0) n += 3;
    if (!strcmp(n, "INT")) return SIGINT;
    if (!strcmp(n, "TERM")) return SIGTERM;
    if (!strcmp(n, "USR1")) return SIGUSR1;
    if (!strcmp(n, "USR2")) return SIGUSR2;
    if (!strcmp(n, "HUP")) return SIGHUP;
    if (!strcmp(n, "QUIT")) return SIGQUIT;
    if (!strcmp(n, "PIPE")) return SIGPIPE;
    if (!strcmp(n, "ALRM")) return SIGALRM;
    if (!strcmp(n, "CHLD")) return SIGCHLD;
    return -1;
}

static RESULT signal_trap(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    if (argc < 1) return RESULT_OK(Qnil);
    int signum = -1;
    if (FIXNUM_P(argv[0])) signum = (int)FIX2LONG(argv[0]);
    else if (SYMBOL_P(argv[0])) signum = signal_name_to_num(korb_id_name(korb_sym2id(argv[0])));
    else if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING)
        signum = signal_name_to_num(((struct korb_string *)argv[0])->ptr);
    if (signum < 0) return RESULT_OK(Qnil);
    /* Block argument (if any) is the handler.  Otherwise argv[1] (a
     * Proc / String like "DEFAULT" / "IGNORE").  We don't actually
     * install a real signal handler — stub so user code can register
     * without errors. */
    VALUE handler = (argc >= 2) ? argv[1]
                  : (korb_block_given(c) ? Qnil : Qnil);
    /* Look up previous handler so we can return it. */
    VALUE prev = Qnil;
    for (int i = 0; i < g_signal_handlers_cnt; i++) {
        if (g_signal_handlers[i].signum == signum) {
            prev = g_signal_handlers[i].handler;
            g_signal_handlers[i].handler = handler;
            return RESULT_OK(prev);
        }
    }
    if (g_signal_handlers_cnt < (int)(sizeof(g_signal_handlers)/sizeof(g_signal_handlers[0]))) {
        g_signal_handlers[g_signal_handlers_cnt].signum = signum;
        g_signal_handlers[g_signal_handlers_cnt].handler = handler;
        g_signal_handlers_cnt++;
    }
    return RESULT_OK(Qnil);
}

static RESULT signal_list(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    VALUE h = korb_hash_new(c, c->sp_top);
    /* CRuby includes "EXIT" with value 0 — pseudo-signal used by at_exit
     * dispatch.  Always present even when the OS doesn't define it. */
    korb_hash_aset(c, h, korb_str_new_cstr(c, c->sp_top, "EXIT"), INT2FIX(0));
    korb_hash_aset(c, h, korb_str_new_cstr(c, c->sp_top, "INT"), INT2FIX(SIGINT));
    korb_hash_aset(c, h, korb_str_new_cstr(c, c->sp_top, "TERM"), INT2FIX(SIGTERM));
    korb_hash_aset(c, h, korb_str_new_cstr(c, c->sp_top, "USR1"), INT2FIX(SIGUSR1));
    korb_hash_aset(c, h, korb_str_new_cstr(c, c->sp_top, "USR2"), INT2FIX(SIGUSR2));
    korb_hash_aset(c, h, korb_str_new_cstr(c, c->sp_top, "HUP"), INT2FIX(SIGHUP));
    korb_hash_aset(c, h, korb_str_new_cstr(c, c->sp_top, "QUIT"), INT2FIX(SIGQUIT));
    korb_hash_aset(c, h, korb_str_new_cstr(c, c->sp_top, "KILL"), INT2FIX(SIGKILL));
    return RESULT_OK(h);
}

/* IO (stubbed via STDOUT / $stdout) */

#include <time.h>
/* Kernel#sleep — pause for N seconds (Float or Integer).  No timer
 * accuracy goal beyond what nanosleep gives. */
RESULT kernel_sleep(CTX *c, int argc, VALUE *sp) {
    c->sp_top = sp;
    VALUE self = sp[-argc - 1];
    VALUE *argv = sp - argc;

    double secs = 0;
    if (argc >= 1) {
        if (FIXNUM_P(argv[0])) secs = (double)FIX2LONG(argv[0]);
        else if (FLONUM_P(argv[0]) || (BUILTIN_TYPE(argv[0]) == T_FLOAT))
            secs = korb_num2dbl(argv[0]);
    }
    if (secs <= 0) return RESULT_OK(INT2FIX(0));
    struct timespec ts = { (time_t)secs, (long)((secs - (long)secs) * 1e9) };
    nanosleep(&ts, NULL);
    return RESULT_OK(INT2FIX((long)secs));
}

RESULT proc_clock_gettime_stub(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double t = ts.tv_sec + ts.tv_nsec / 1e9;
    return RESULT_OK(korb_float_new(c, c->sp_top, t));
}

RESULT time_now_stub(CTX *c, int argc, VALUE *sp) {

    c->sp_top = sp;

    VALUE self = sp[-argc - 1];

    VALUE *argv = sp - argc;

    /* return Float seconds since epoch (we just use Process clock, not real epoch) */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double t = ts.tv_sec + ts.tv_nsec / 1e9;
    return RESULT_OK(korb_float_new(c, c->sp_top, t));
}


