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
    if (!fp) {
        /* CRuby: if EOF/closed → $_ = nil. */
        korb_last_line_set(c, Qnil);
        return Qnil;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, fp);
    if (n <= 0) { free(line); korb_last_line_set(c, Qnil); return Qnil; }
    VALUE r = korb_str_new(line, n);
    free(line);
    korb_last_line_set(c, r);
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
    /* Line-buffer the writer so puts/print show up promptly. */
    setvbuf(w, NULL, _IOLBF, 0);
    VALUE rio = korb_io_new((struct korb_class *)self, r);
    VALUE wio = korb_io_new((struct korb_class *)self, w);
    VALUE arr = korb_ary_new_capa(2);
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

static VALUE korb_select_collect_ready(VALUE arr, fd_set *set) {
    VALUE out = korb_ary_new();
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
    VALUE io = korb_io_new((struct korb_class *)self, fp);
    if (!korb_block_given()) return io;
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
    VALUE ret = korb_ary_new_capa(3);
    korb_ary_push(ret, korb_select_collect_ready(rs, &rset));
    korb_ary_push(ret, korb_select_collect_ready(ws, &wset));
    korb_ary_push(ret, korb_select_collect_ready(es, &eset));
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
    char buf[PATH_MAX];
    if (!realpath(korb_str_cstr(argv[0]), buf)) {
        korb_raise(c, NULL, "realpath failed: %s -- %s",
                   strerror(errno), korb_str_cstr(argv[0]));
        return Qnil;
    }
    return korb_str_new_cstr(buf);
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
    if (!getcwd(buf, sizeof(buf))) return korb_str_new_cstr(".");
    return korb_str_new_cstr(buf);
}

static VALUE dir_entries(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_ary_new();
    const char *path = korb_str_cstr(argv[0]);
    DIR *d = opendir(path);
    if (!d) {
        korb_raise(c, NULL, "no such directory -- %s", path);
        return Qnil;
    }
    VALUE out = korb_ary_new();
    struct dirent *de;
    while ((de = readdir(d))) {
        korb_ary_push(out, korb_str_new_cstr(de->d_name));
    }
    closedir(d);
    return out;
}

static VALUE dir_chdir(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return Qnil;
    const char *path = korb_str_cstr(argv[0]);
    if (korb_block_given()) {
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

static void korb_glob_walk(const char *dir, const char *pat, VALUE out, bool recursive) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (korb_glob_simple_match(pat, de->d_name)) {
            korb_ary_push(out, korb_str_new_cstr(path));
        }
        if (recursive) {
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                korb_glob_walk(path, pat, out, true);
            }
        }
    }
    closedir(d);
}

static VALUE dir_glob(CTX *c, VALUE self, int argc, VALUE *argv) {
    if (argc < 1 || BUILTIN_TYPE(argv[0]) != T_STRING) return korb_ary_new();
    const char *pat = korb_str_cstr(argv[0]);
    VALUE out = korb_ary_new();
    /* Detect double-star + slash + rest recursive form. */
    if (strncmp(pat, "**/", 3) == 0) {
        korb_glob_walk(".", pat + 3, out, true);
        return out;
    }
    /* Otherwise look in `.` if no /; else split last component. */
    const char *slash = strrchr(pat, '/');
    if (!slash) {
        korb_glob_walk(".", pat, out, false);
    } else {
        char dir[4096];
        long dl = slash - pat;
        if (dl >= (long)sizeof(dir)) dl = sizeof(dir) - 1;
        memcpy(dir, pat, dl); dir[dl] = 0;
        korb_glob_walk(dir, slash + 1, out, false);
    }
    return out;
}

/* ---------- Process ---------- */
#include <sys/types.h>

static VALUE process_pid(CTX *c, VALUE self, int argc, VALUE *argv) {
    return INT2FIX((long)getpid());
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
    return korb_float_new(t);
}

VALUE time_now_stub(CTX *c, VALUE self, int argc, VALUE *argv) {
    /* return Float seconds since epoch (we just use Process clock, not real epoch) */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double t = ts.tv_sec + ts.tv_nsec / 1e9;
    return korb_float_new(t);
}


