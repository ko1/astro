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
    return korb_str_new(c, c->sp, buf, got);
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
    VALUE io = (VALUE)korb_object_new(c, c->sp, klass);
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
        return korb_str_new(c, c->sp, buf, got);
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
    return korb_str_new(c, c->sp, buf, len);
}

static VALUE io_gets(CTX *c, VALUE self, int argc, VALUE *argv) {
    FILE *fp = korb_io_fp(self);
    if (!fp) {
        /* CRuby: if EOF/closed → $_ = nil. */
        korb_last_line_set(c, Qnil);
        return Qnil;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, fp);
    if (n <= 0) { free(line); korb_last_line_set(c, Qnil); return Qnil; }
    VALUE r = korb_str_new(c, c->sp, line, n);
    free(line);
    korb_last_line_set(c, r);
    return r;
}

static VALUE io_each_line(CTX *c, VALUE self, int argc, VALUE *argv) {
    FILE *fp = korb_io_fp(self);
    if (!fp) return self;
    bool has_block = korb_block_given(c);
    VALUE collected = has_block ? Qnil : korb_ary_new(c, c->sp);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, fp)) > 0) {
        VALUE l = korb_str_new(c, c->sp, line, n);
        korb_last_line_set(c, l);
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

#include <unistd.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>

/* IO.pipe → [reader, writer] pair of IO objects.  Mirrors CRuby. */
VALUE io_class_pipe(CTX *c, VALUE self, int argc, VALUE *argv) {
    int fds[2];
    if (pipe(fds) != 0) {
        korb_raise(c, NULL, "IO.pipe failed: %s", strerror(errno));
        return Qnil;
    }
    FILE *r = fdopen(fds[0], "rb");
    FILE *w = fdopen(fds[1], "wb");
    if (!r || !w) {
        if (r) fclose(r); else close(fds[0]);
        if (w) fclose(w); else close(fds[1]);
        korb_raise(c, NULL, "IO.pipe fdopen failed");
        return Qnil;
    }
    /* Unbuffer both ends so the writer's bytes immediately reach the
     * kernel pipe and the reader's read/readpartial unblock without
     * waiting for a newline.  CRuby's IO.pipe is also unbuffered.
     * Without this, test_io / test_optimization hang on
     *   w.write "."; r.readpartial(n, "")
     * because line-buffered fputs holds back the single-char write. */
    setvbuf(w, NULL, _IONBF, 0);
    setvbuf(r, NULL, _IONBF, 0);
    VALUE rio = korb_io_new(c, (struct korb_class *)self, r);
    VALUE wio = korb_io_new(c, (struct korb_class *)self, w);
    VALUE arr = korb_ary_new_capa(c, c->sp, 2);
    korb_ary_push(arr, rio);
    korb_ary_push(arr, wio);
    return arr;
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
    VALUE out = korb_ary_new(c, c->sp);
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
VALUE io_class_popen(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) {
        korb_raise(c, NULL, "IO.popen: command String required");
        return Qnil;
    }
    const char *cmd = korb_str_cstr(argv[0]);
    const char *mode = "r";
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
        mode = korb_str_cstr(argv[1]);
    }
    FILE *fp = popen(cmd, mode);
    if (!fp) {
        korb_raise(c, NULL, "popen failed: %s", strerror(errno));
        return Qnil;
    }
    VALUE io = korb_io_new(c, (struct korb_class *)self, fp);
    if (!korb_block_given(c)) return io;
    VALUE r = korb_yield(c, 1, &io);
    pclose(fp);
    korb_ivar_set(io, korb_io_fp_id_(), Qnil);
    return r;
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

VALUE io_class_copy_stream(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2) {
        korb_raise(c, NULL, "IO.copy_stream(src, dst[, len[, src_offset]])");
        return Qnil;
    }
    long max_len = (argc >= 3 && FIXNUM_P(argv[2])) ? FIX2LONG(argv[2]) : -1;
    /* Resolve src to fd. */
    int src_fd = -1; bool src_close = false;
    if (BUILTIN_TYPE(argv[0]) == T_STRING) {
        src_fd = open(korb_str_cstr(argv[0]), O_RDONLY);
        if (src_fd < 0) {
            korb_raise(c, NULL, "open(%s) failed: %s",
                       korb_str_cstr(argv[0]), strerror(errno));
            return Qnil;
        }
        src_close = true;
    } else {
        FILE *fp = korb_io_fp(argv[0]);
        if (!fp) {
            korb_raise(c, NULL, "IO.copy_stream: src must be IO or path");
            return Qnil;
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
            korb_raise(c, NULL, "open(%s) failed: %s",
                       korb_str_cstr(argv[1]), strerror(errno));
            return Qnil;
        }
        dst_close = true;
    } else {
        FILE *fp = korb_io_fp(argv[1]);
        if (!fp) {
            if (src_close) close(src_fd);
            korb_raise(c, NULL, "IO.copy_stream: dst must be IO or path");
            return Qnil;
        }
        fflush(fp);
        dst_fd = fileno(fp);
    }
    /* Optional offset (4th arg) — lseek src before copying. */
    if (argc >= 4 && FIXNUM_P(argv[3])) {
        if (lseek(src_fd, (off_t)FIX2LONG(argv[3]), SEEK_SET) < 0) {
            if (src_close) close(src_fd);
            if (dst_close) close(dst_fd);
            korb_raise(c, NULL, "lseek failed: %s", strerror(errno));
            return Qnil;
        }
    }
    long n = korb_copy_fd_(src_fd, dst_fd, max_len);
    if (src_close) close(src_fd);
    if (dst_close) close(dst_fd);
    if (n < 0) {
        korb_raise(c, NULL, "IO.copy_stream failed: %s", strerror(errno));
        return Qnil;
    }
    return INT2FIX(n);
}

/* IO#tty? — true iff backed by a terminal fd. */
static VALUE io_tty_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    FILE *fp = korb_io_fp(self);
    if (!fp) return Qfalse;
    return KORB_BOOL(isatty(fileno(fp)));
}

/* IO#fileno — underlying fd, useful for IO.select sanity etc. */
static VALUE io_fileno(CTX *c, VALUE self, int argc, VALUE *argv) {
    FILE *fp = korb_io_fp(self);
    if (!fp) return INT2FIX(-1);
    return INT2FIX(fileno(fp));
}

VALUE io_class_select(CTX *c, VALUE self, int argc, VALUE *argv) {
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
        korb_raise(c, NULL, "IO.select failed: %s", strerror(errno));
        return Qnil;
    }
    if (n == 0) return Qnil;
    VALUE ret = korb_ary_new_capa(c, c->sp, 3);
    korb_ary_push(ret, korb_select_collect_ready(c, rs, &rset));
    korb_ary_push(ret, korb_select_collect_ready(c, ws, &wset));
    korb_ary_push(ret, korb_select_collect_ready(c, es, &eset));
    return ret;
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
    VALUE io = korb_io_new(c, (struct korb_class *)self, fp);
    if (!korb_block_given(c)) return io;
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
    /* Pin r (result) and per-iter s/sep against the cascade of
     * korb_str_new / korb_to_s / korb_str_concat allocations that
     * each fire GC under STRESS. */
    VALUE ret = Qnil;
    ARO_ROOT_SCOPE_START(c, rs, 3) {
        rs[0] = korb_str_new(c, c->sp, "", 0);  /* r */
        for (int i = 0; i < argc; i++) {
            rs[1] = argv[i];
            if (BUILTIN_TYPE(rs[1]) != T_STRING) rs[1] = korb_to_s(c, c->sp, rs[1]);
            if (i > 0) {
                rs[2] = korb_str_new_cstr(c, c->sp, "/");
                korb_str_concat(c, c->sp, rs[0], rs[2]);
            }
            korb_str_concat(c, c->sp, rs[0], rs[1]);
        }
        ret = rs[0];
    } ARO_ROOT_SCOPE_END(c, rs);
    return ret;
}

static VALUE file_exist_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qfalse;
    return KORB_BOOL(korb_file_exists(korb_str_cstr(argv[0])));
}

#include <sys/stat.h>
static VALUE file_directory_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qfalse;
    struct stat st;
    if (stat(korb_str_cstr(argv[0]), &st) != 0) return Qfalse;
    return KORB_BOOL(S_ISDIR(st.st_mode));
}
static VALUE file_file_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qfalse;
    struct stat st;
    if (stat(korb_str_cstr(argv[0]), &st) != 0) return Qfalse;
    return KORB_BOOL(S_ISREG(st.st_mode));
}
static VALUE file_size(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return INT2FIX(0);
    struct stat st;
    if (stat(korb_str_cstr(argv[0]), &st) != 0) {
        korb_raise(c, NULL, "no such file -- %s", korb_str_cstr(argv[0]));
        return Qnil;
    }
    return INT2FIX((long)st.st_size);
}

static VALUE file_unlink(CTX *c, VALUE self, int argc, VALUE *argv) {
    long n = 0;
    for (int i = 0; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_STRING) continue;
        if (unlink(korb_str_cstr(argv[i])) != 0) {
            korb_raise(c, NULL, "unlink failed: %s -- %s",
                       strerror(errno), korb_str_cstr(argv[i]));
            return Qnil;
        }
        n++;
    }
    return INT2FIX(n);
}

static VALUE file_rename(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2 || BUILTIN_TYPE(argv[0]) != T_STRING ||
        BUILTIN_TYPE(argv[1]) != T_STRING) {
        korb_raise(c, NULL, "File.rename: two String args expected");
        return Qnil;
    }
    if (rename(korb_str_cstr(argv[0]), korb_str_cstr(argv[1])) != 0) {
        korb_raise(c, NULL, "rename failed: %s", strerror(errno));
        return Qnil;
    }
    return INT2FIX(0);
}

static VALUE file_chmod(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2 || !FIXNUM_P(argv[0])) {
        korb_raise(c, NULL, "File.chmod(mode, *paths)");
        return Qnil;
    }
    long mode = FIX2LONG(argv[0]);
    long n = 0;
    for (int i = 1; i < argc; i++) {
        if (BUILTIN_TYPE(argv[i]) != T_STRING) continue;
        if (chmod(korb_str_cstr(argv[i]), (mode_t)mode) != 0) {
            korb_raise(c, NULL, "chmod failed: %s", strerror(errno));
            return Qnil;
        }
        n++;
    }
    return INT2FIX(n);
}

#include <limits.h>
static VALUE file_realpath(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
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
        korb_raise(c, NULL, "realpath failed: %s -- %s",
                   strerror(errno), resolved_in);
        return Qnil;
    }
    return korb_str_new_cstr(c, c->sp, buf);
}

static VALUE file_dirname(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new(c, c->sp, ".", 1);
    return korb_str_new_cstr(c, c->sp, korb_dirname(korb_str_cstr(argv[0])));
}

static VALUE file_basename(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new(c, c->sp, "", 0);
    const char *s = korb_str_cstr(argv[0]);
    const char *slash = strrchr(s, '/');
    return korb_str_new_cstr(c, c->sp, slash ? slash + 1 : s);
}

static VALUE file_extname(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new(c, c->sp, "", 0);
    const char *s = korb_str_cstr(argv[0]);
    const char *dot = strrchr(s, '.');
    if (!dot || dot == s) return korb_str_new(c, c->sp, "", 0);
    /* Don't include if dot is in dirname only */
    const char *slash = strrchr(s, '/');
    if (slash && dot < slash) return korb_str_new(c, c->sp, "", 0);
    return korb_str_new_cstr(c, c->sp, dot);
}

static VALUE file_binread(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* Same as File.read but ensures binary mode */
    return file_read(c, self, argc, argv);
}

static VALUE file_expand_path(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_str_new(c, c->sp, "", 0);
    /* simplistic: if absolute, return as-is; else prepend dir */
    const char *s = korb_str_cstr(argv[0]);
    if (s[0] == '/') return argv[0];
    if (argc >= 2 && BUILTIN_TYPE(argv[1]) == T_STRING) {
        return korb_str_new_cstr(c, c->sp, korb_join_path(korb_str_cstr(argv[1]), s));
    }
    return argv[0];
}

/* ---------- Dir ---------- */
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

static VALUE dir_mkdir(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) {
        korb_raise(c, NULL, "Dir.mkdir(path[, mode])");
        return Qnil;
    }
    long mode = (argc >= 2 && FIXNUM_P(argv[1])) ? FIX2LONG(argv[1]) : 0755;
    if (mkdir(korb_str_cstr(argv[0]), (mode_t)mode) != 0) {
        korb_raise(c, NULL, "mkdir failed: %s -- %s",
                   strerror(errno), korb_str_cstr(argv[0]));
        return Qnil;
    }
    return INT2FIX(0);
}

static VALUE dir_rmdir(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
    if (rmdir(korb_str_cstr(argv[0])) != 0) {
        korb_raise(c, NULL, "rmdir failed: %s -- %s",
                   strerror(errno), korb_str_cstr(argv[0]));
        return Qnil;
    }
    return INT2FIX(0);
}

static VALUE dir_pwd(CTX *c, VALUE self, int argc, VALUE *argv) {
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return korb_str_new_cstr(c, c->sp, ".");
    return korb_str_new_cstr(c, c->sp, buf);
}

static VALUE dir_entries(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_ary_new(c, c->sp);
    const char *path = korb_str_cstr(argv[0]);
    DIR *d = opendir(path);
    if (!d) {
        korb_raise(c, NULL, "no such directory -- %s", path);
        return Qnil;
    }
    VALUE out = korb_ary_new(c, c->sp);
    struct dirent *de;
    while ((de = readdir(d))) {
        korb_ary_push(out, korb_str_new_cstr(c, c->sp, de->d_name));
    }
    closedir(d);
    return out;
}

static VALUE dir_chdir(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
    const char *path = korb_str_cstr(argv[0]);
    if (korb_block_given(c)) {
        char prev[4096];
        if (!getcwd(prev, sizeof(prev))) return Qnil;
        if (chdir(path) != 0) {
            korb_raise(c, NULL, "could not chdir to %s", path);
            return Qnil;
        }
        VALUE r = korb_yield(c, 0, NULL);
        if (chdir(prev) != 0) { /* unlikely; best-effort restore */ }
        return r;
    }
    if (chdir(path) != 0) {
        korb_raise(c, NULL, "could not chdir to %s", path);
        return Qnil;
    }
    return INT2FIX(0);
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
            korb_ary_push(out, korb_str_new_cstr(c, c->sp, path));
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

static VALUE dir_glob(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_ary_new(c, c->sp);
    const char *pat = korb_str_cstr(argv[0]);
    VALUE out = korb_ary_new(c, c->sp);
    /* Detect double-star + slash + rest recursive form. */
    if (strncmp(pat, "**/", 3) == 0) {
        korb_glob_walk(c, ".", pat + 3, out, true);
        return out;
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
    return out;
}

/* ---------- Process ---------- */
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

static VALUE process_pid(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX((long)getpid());
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
    VALUE cStatus = korb_const_get(korb_vm->object_class, korb_intern("Process"));
    VALUE cs = korb_const_get((struct korb_class *)cStatus, korb_intern("Status"));
    if (UNDEF_P(cs) || NIL_P(cs)) cs = (VALUE)korb_vm->object_class;
    VALUE obj = korb_object_new(c, c->sp, (struct korb_class *)cs);
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
static VALUE kernel_system(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) {
        VALUE eA = korb_const_get(korb_vm->object_class, korb_intern("ArgumentError"));
        korb_raise(c, (struct korb_class *)eA, "wrong number of arguments");
        return Qnil;
    }
    bool use_shell;
    char **xargv = build_exec_argv(c, argv, argc, &use_shell);
    if (!xargv) return Qnil;
    pid_t pid = fork();
    if (pid < 0) return Qnil;
    if (pid == 0) {
        execvp(xargv[0], xargv);
        _exit(127);
    }
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    VALUE st = make_process_status(c, wstatus, pid);
    korb_gvar_set(korb_intern("$?"), st);
    if (WIFEXITED(wstatus)) {
        return WEXITSTATUS(wstatus) == 0 ? Qtrue : Qfalse;
    }
    return Qfalse;
}

/* Kernel#`cmd` (backtick) — run command, return stdout as a String. */
static VALUE kernel_xstring(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || SPECIAL_CONST_P(argv[0]) || BUILTIN_TYPE(argv[0]) != T_STRING)
        return korb_str_new_cstr(c, c->sp, "");
    int pipefd[2];
    if (pipe(pipefd) < 0) return korb_str_new_cstr(c, c->sp, "");
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return korb_str_new_cstr(c, c->sp, "");
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
    VALUE r = korb_str_new_cstr(c, c->sp, "");
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        korb_str_concat(c, c->sp, r, korb_str_new(c, c->sp, buf, n));
    }
    close(pipefd[0]);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    korb_gvar_set(korb_intern("$?"), make_process_status(c, wstatus, pid));
    return r;
}

/* Kernel#exec — replace the current process. */
static VALUE kernel_exec(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    bool use_shell;
    char **xargv = build_exec_argv(c, argv, argc, &use_shell);
    if (!xargv) return Qnil;
    execvp(xargv[0], xargv);
    /* Reach here only if exec failed. */
    VALUE eErrno = korb_const_get(korb_vm->object_class, korb_intern("Errno"));
    if (!UNDEF_P(eErrno) && !NIL_P(eErrno)) {
        korb_raise(c, NULL, "exec failed: %s", xargv[0]);
    }
    return Qnil;
}

/* Process.spawn(cmd, *args) — fork + exec, return pid (don't wait). */
static VALUE process_spawn(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    bool use_shell;
    char **xargv = build_exec_argv(c, argv, argc, &use_shell);
    if (!xargv) return Qnil;
    pid_t pid = fork();
    if (pid < 0) return Qnil;
    if (pid == 0) {
        execvp(xargv[0], xargv);
        _exit(127);
    }
    return INT2FIX((long)pid);
}

/* Process.fork { ... } — fork; in child, run block then exit.  In
 * parent, return child pid. */
static VALUE process_fork(CTX *c, VALUE self, int argc, VALUE *argv) {
    pid_t pid = fork();
    if (pid < 0) return Qnil;
    if (pid == 0) {
        if (korb_block_given(c)) {
            korb_yield(c, 0, NULL);
        }
        if (c->state == KORB_RAISE) {
            VALUE s = korb_inspect(c, c->sp, c->state_value);
            fprintf(stderr, "fork child: %s\n", korb_str_cstr(s));
            _exit(1);
        }
        _exit(0);
    }
    return INT2FIX((long)pid);
}

/* Process.wait([pid [, flags]]) — waitpid; sets $? and returns the pid
 * (or -1 on error). */
static VALUE process_wait(CTX *c, VALUE self, int argc, VALUE *argv) {
    pid_t want = -1;
    int flags = 0;
    if (argc >= 1 && FIXNUM_P(argv[0])) want = (pid_t)FIX2LONG(argv[0]);
    if (argc >= 2 && FIXNUM_P(argv[1])) flags = (int)FIX2LONG(argv[1]);
    int wstatus = 0;
    pid_t got = waitpid(want, &wstatus, flags);
    if (got <= 0) return Qnil;
    korb_gvar_set(korb_intern("$?"), make_process_status(c, wstatus, got));
    return INT2FIX((long)got);
}

static VALUE process_kill(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 2) return INT2FIX(0);
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
    return INT2FIX((long)sent);
}

/* Process::Status methods. */
static VALUE pstatus_exitstatus(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_ivar_get(self, korb_intern("@exitstatus"));
}
static VALUE pstatus_pid(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_ivar_get(self, korb_intern("@pid"));
}
static VALUE pstatus_success_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_ivar_get(self, korb_intern("@success"));
}
static VALUE pstatus_signaled_p(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_ivar_get(self, korb_intern("@signaled"));
}
static VALUE pstatus_termsig(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE v = korb_ivar_get(self, korb_intern("@termsig"));
    return UNDEF_P(v) ? Qnil : v;
}
static VALUE pstatus_to_i(CTX *c, VALUE self, int argc, VALUE *argv) {
    return korb_ivar_get(self, korb_intern("@to_i"));
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

static VALUE signal_trap(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1) return Qnil;
    int signum = -1;
    if (FIXNUM_P(argv[0])) signum = (int)FIX2LONG(argv[0]);
    else if (SYMBOL_P(argv[0])) signum = signal_name_to_num(korb_id_name(korb_sym2id(argv[0])));
    else if (!SPECIAL_CONST_P(argv[0]) && BUILTIN_TYPE(argv[0]) == T_STRING)
        signum = signal_name_to_num(((struct korb_string *)argv[0])->ptr);
    if (signum < 0) return Qnil;
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
            return prev;
        }
    }
    if (g_signal_handlers_cnt < (int)(sizeof(g_signal_handlers)/sizeof(g_signal_handlers[0]))) {
        g_signal_handlers[g_signal_handlers_cnt].signum = signum;
        g_signal_handlers[g_signal_handlers_cnt].handler = handler;
        g_signal_handlers_cnt++;
    }
    return Qnil;
}

static VALUE signal_list(CTX *c, VALUE self, int argc, VALUE *argv) {
    VALUE h = korb_hash_new(c, c->sp);
    /* CRuby includes "EXIT" with value 0 — pseudo-signal used by at_exit
     * dispatch.  Always present even when the OS doesn't define it. */
    korb_hash_aset(h, korb_str_new_cstr(c, c->sp, "EXIT"), INT2FIX(0));
    korb_hash_aset(h, korb_str_new_cstr(c, c->sp, "INT"), INT2FIX(SIGINT));
    korb_hash_aset(h, korb_str_new_cstr(c, c->sp, "TERM"), INT2FIX(SIGTERM));
    korb_hash_aset(h, korb_str_new_cstr(c, c->sp, "USR1"), INT2FIX(SIGUSR1));
    korb_hash_aset(h, korb_str_new_cstr(c, c->sp, "USR2"), INT2FIX(SIGUSR2));
    korb_hash_aset(h, korb_str_new_cstr(c, c->sp, "HUP"), INT2FIX(SIGHUP));
    korb_hash_aset(h, korb_str_new_cstr(c, c->sp, "QUIT"), INT2FIX(SIGQUIT));
    korb_hash_aset(h, korb_str_new_cstr(c, c->sp, "KILL"), INT2FIX(SIGKILL));
    return h;
}

/* IO (stubbed via STDOUT / $stdout) */

#include <time.h>
/* Kernel#sleep — pause for N seconds (Float or Integer).  No timer
 * accuracy goal beyond what nanosleep gives. */
VALUE kernel_sleep(CTX *c, VALUE self, int argc, VALUE *argv) {
    double secs = 0;
    if (argc >= 1) {
        if (FIXNUM_P(argv[0])) secs = (double)FIX2LONG(argv[0]);
        else if (FLONUM_P(argv[0]) || (BUILTIN_TYPE(argv[0]) == T_FLOAT))
            secs = korb_num2dbl(argv[0]);
    }
    if (secs <= 0) return INT2FIX(0);
    struct timespec ts = { (time_t)secs, (long)((secs - (long)secs) * 1e9) };
    nanosleep(&ts, NULL);
    return INT2FIX((long)secs);
}

VALUE proc_clock_gettime_stub(CTX *c, VALUE self, int argc, VALUE *argv) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double t = ts.tv_sec + ts.tv_nsec / 1e9;
    return korb_float_new(c, c->sp, t);
}

VALUE time_now_stub(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* return Float seconds since epoch (we just use Process clock, not real epoch) */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double t = ts.tv_sec + ts.tv_nsec / 1e9;
    return korb_float_new(c, c->sp, t);
}


