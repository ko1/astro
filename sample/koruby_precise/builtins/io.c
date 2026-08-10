#include <errno.h>
#include <fcntl.h>
/* koruby_precise — io.c: a minimal IO/File-object layer over C stdio.
 * An IO/File instance stores its slot index (into vm->io_fps) in the @__io_fp
 * ivar; the FILE* is a raw C pointer kept off-heap, so no GC scanning is needed.
 * #included into korb_runtime.c after file.c. */

static uint32_t korb_io_register(struct korb_vm *vm, FILE *fp) {
    if (vm->io_cnt == vm->io_capa) {
        vm->io_capa = vm->io_capa ? vm->io_capa * 2 : 8;
        vm->io_fps = realloc(vm->io_fps, sizeof(FILE *) * vm->io_capa);
        if (!vm->io_fps) { fprintf(stderr, "koruby_precise: oom (io table)\n"); abort(); }
    }
    vm->io_fps[vm->io_cnt] = fp;
    return vm->io_cnt++;
}
static uint32_t korb_io_fp_mid(CTX *c) { return korb_intern(c->vm, "__io_fp", 7); }
static uint32_t korb_io_mode_mid(CTX *c) { return korb_intern(c->vm, "__io_mode", 9); }
static uint32_t korb_io_bin_mid(CTX *c) { return korb_intern(c->vm, "__io_bin", 8); }
/* true if the stream was opened in binary mode (the 'b' flag) — its reads yield
 * ASCII-8BIT (byte-indexed) strings, like CRuby, so binary parsing works. */
static bool korb_io_is_binary(CTX *c, VALUE self) {
    return korb_ivar_get(c, self, ID2SYM(korb_io_bin_mid(c))) == KORB_TRUE;
}
/* read/write permission bits from the @__io_mode ivar: 1 = readable, 2 = writable. */
static int korb_io_rw(CTX *c, VALUE self) {
    const VALUE v = korb_ivar_get(c, self, ID2SYM(korb_io_mode_mid(c)));
    return FIXNUM_P(v) ? (int)FIX2LONG(v) : 3;   /* unknown ⇒ assume read+write */
}
#define KORB_IO_NEED_READ(c, slots, self)  do { if (UNLIKELY(!(korb_io_rw(c, VALUE_REF_GET(self)) & 1))) \
    return korb_raise(c, slots, KORB_E_IOERROR, 0, "not opened for reading"); } while (0)
#define KORB_IO_NEED_WRITE(c, slots, self) do { if (UNLIKELY(!(korb_io_rw(c, VALUE_REF_GET(self)) & 2))) \
    return korb_raise(c, slots, KORB_E_IOERROR, 0, "not opened for writing"); } while (0)
static FILE *korb_io_fp(CTX *c, VALUE self) {
    const VALUE idxv = korb_ivar_get(c, self, ID2SYM(korb_io_fp_mid(c)));
    if (!FIXNUM_P(idxv)) return NULL;
    const intptr_t idx = FIX2LONG(idxv);
    if (idx < 0 || (uint32_t)idx >= c->vm->io_cnt) return NULL;
    return c->vm->io_fps[idx];
}
/* new IO/File instance of `klass` bound to `fp`.  slots[0..] scratch; result rooted by caller. */
static RESULT korb_io_make(CTX *c, VALUE *slots, VALUE klass, FILE *fp, int rw) {
    const uint32_t idx = korb_io_register(c->vm, fp);
    slots[0] = UNWRAP(korb_obj_new(c, slots, klass));
    CHECK(korb_ivar_set(c, slots + 1, VALUE_REF_AT(&slots[0]), ID2SYM(korb_io_fp_mid(c)), LONG2FIX((intptr_t)idx)));
    CHECK(korb_ivar_set(c, slots + 1, VALUE_REF_AT(&slots[0]), ID2SYM(korb_io_mode_mid(c)), LONG2FIX(rw)));
    return RESULT_OK(slots[0]);
}

/* coerce v to a String (to_s if needed) and fwrite it; accumulate bytes. */
static RESULT korb_io_emit(CTX *c, VALUE *slots, VALUE v, FILE *fp, size_t *nbytes) {
    if (!KORB_STRING_P(v)) {
        slots[0] = v;
        RESULT r = korb_send(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        v = r.value;
    }
    if (KORB_STRING_P(v)) { const KorbString *s = VAL2STR(v); *nbytes += fwrite(korb_strbuf_data(s->buf), 1, s->len, fp); }
    return RESULT_OK(KORB_NIL);
}

/* IO#truncate(len) → 0 (ftruncate the descriptor). */
static RESULT korb_m_io_truncate(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    const VALUE lv = VALUE_SLICE_GET(a, 0);
    if (!FIXNUM_P(lv)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    fflush(fp);
    if (ftruncate(fileno(fp), (off_t)FIX2LONG(lv)) != 0) return korb_raise_errno(c, slots, errno, "ftruncate", "");
    return RESULT_OK(LONG2FIX(0));
}
/* IO#fileno → the integer file descriptor. */
static RESULT korb_m_io_fileno(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    return RESULT_OK(LONG2FIX(fileno(fp)));
}
static RESULT korb_m_io_tty_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; (void)slots;
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    return RESULT_OK((fp && isatty(fileno(fp))) ? KORB_TRUE : KORB_FALSE);
}
/* IO#stat → File::Stat of the open descriptor (fstat). */
static RESULT korb_m_io_stat(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    struct stat st;
    if (fstat(fileno(fp), &st) != 0) return korb_raise_errno(c, slots, errno, "fstat", "");
    return korb_stat_make(c, slots, &st);
}
static RESULT korb_bi_format(CTX *c, VALUE *slots, VALUE_SLICE args);   /* fwd (korb_runtime.c) */
/* IO#printf(format, *args) → nil: write the sprintf-formatted string to self
 * (was falling back to Kernel#printf, which writes to $stdout, not the file). */
static RESULT korb_m_io_printf(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) == 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no format string given");
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_WRITE(c, slots, self);
    slots[0] = VALUE_REF_GET(self);                      /* root self across the format dispatch (may GC) */
    RESULT fr = korb_bi_format(c, slots + 1, a);
    if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
    fp = korb_io_fp(c, slots[0]);                        /* re-fetch after possible GC */
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    if (KORB_STRING_P(fr.value)) { const KorbString *const s = VAL2STR(fr.value); fwrite(korb_strbuf_data(s->buf), 1, s->len, fp); }
    return RESULT_OK(KORB_NIL);
}
/* IO.pipe → [r, w]  (block form: yield r, w; ensure both closed).
 * fd を pipe(2) で作り fdopen で FILE* 化。w は unbuffered (setvbuf _IONBF) にして
 * 「write → 相手が即 read できる」という pipe の期待通りに振る舞わせる。 */
static RESULT korb_m_io_s_pipe(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                               NODE *block, VALUE *def_env, VALUE *cself) {
    (void)a;
    int fds[2];
    if (pipe(fds) != 0) return korb_raise_errno(c, slots, errno, "pipe", "");
    FILE *rf = fdopen(fds[0], "rb");
    FILE *wf = fdopen(fds[1], "wb");
    if (!rf || !wf) {
        if (rf) fclose(rf); else close(fds[0]);
        if (wf) fclose(wf); else close(fds[1]);
        return korb_raise_errno(c, slots, errno, "fdopen", "");
    }
    setvbuf(wf, NULL, _IONBF, 0);
    slots[0] = VALUE_REF_GET(self);                       /* IO class (root) */
    slots[1] = UNWRAP(korb_io_make(c, slots + 2, slots[0], rf, 1));   /* r (read) */
    slots[2] = UNWRAP(korb_io_make(c, slots + 3, slots[0], wf, 2));   /* w (write) */
    slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));
    { VALUE_REF pr = VALUE_REF_AT(&slots[3]);
      CHECK(korb_ary_push_val(c, slots + 4, pr, slots[1]));
      CHECK(korb_ary_push_val(c, slots + 4, pr, slots[2])); }
    if (block == NULL) return RESULT_OK(slots[3]);
    /* block form: yield(r, w) して ensure 相当で両方 close */
    RESULT r = korb_block_yield(c, slots + 4, block, def_env, &slots[1], 2, cself);
    { FILE *f1 = korb_io_fp(c, slots[1]); if (f1) { fclose(f1); c->vm->io_fps[FIX2LONG(korb_ivar_get(c, slots[1], ID2SYM(korb_io_fp_mid(c))))] = NULL; } }
    { FILE *f2 = korb_io_fp(c, slots[2]); if (f2) { fclose(f2); c->vm->io_fps[FIX2LONG(korb_ivar_get(c, slots[2], ID2SYM(korb_io_fp_mid(c))))] = NULL; } }
    return r;
}

/* IO#write(*args) → total bytes written. */
static RESULT korb_m_io_write(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_WRITE(c, slots, self);
    size_t nb = 0;
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        const RESULT r = korb_io_emit(c, slots, VALUE_SLICE_GET(a, i), fp, &nb);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(LONG2FIX((intptr_t)nb));
}
/* IO#print(*args) → nil. */
static RESULT korb_m_io_print(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_WRITE(c, slots, self);
    size_t nb = 0;
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        const RESULT r = korb_io_emit(c, slots, VALUE_SLICE_GET(a, i), fp, &nb);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(KORB_NIL);
}
/* IO#<<(obj) → self. */
static RESULT korb_m_io_lshift(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_WRITE(c, slots, self);
    size_t nb = 0;
    const RESULT r = korb_io_emit(c, slots, VALUE_SLICE_GET(a, 0), fp, &nb);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(VALUE_REF_GET(self));
}
/* IO#puts(*args) → nil. */
static RESULT korb_m_io_puts(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_WRITE(c, slots, self);
    if (VALUE_SLICE_LEN(a) == 0) { fputc('\n', fp); return RESULT_OK(KORB_NIL); }
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++)
        CHECK(korb_puts_one_to(c, slots, VALUE_SLICE_GET(a, i), fp));
    return RESULT_OK(KORB_NIL);
}
/* IO#read([length]) → `length` bytes (nil at EOF), or the whole rest. */
static RESULT korb_m_io_read(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_READ(c, slots, self);
    if (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) {   /* bounded read */
        intptr_t n = FIX2LONG(VALUE_SLICE_GET(a, 0));
        if (n < 0) n = 0;
        char *b = malloc((size_t)n + 1);
        if (!b) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of memory");
        size_t got = fread(b, 1, (size_t)n, fp);
        if (got == 0 && n > 0) { free(b); return RESULT_OK(KORB_NIL); }   /* EOF */
        RESULT r = korb_str_new(c, slots, b, (uint32_t)got);
        free(b);
        if (r.state == KORB_NORMAL && korb_io_is_binary(c, VALUE_REF_GET(self)))
            KORB_STR_ENC_SET(r.value, KORB_ENC_BINARY);                   /* 'rb' → ASCII-8BIT (byte-indexed) */
        return r;
    }
    char *buf = NULL; size_t cap = 0, len = 0;
    for (;;) {
        if (len + 65536 > cap) { cap = cap ? cap * 2 : 131072; char *nb = realloc(buf, cap); if (!nb) { free(buf); return korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of memory"); } buf = nb; }
        size_t got = fread(buf + len, 1, cap - len, fp);
        len += got;
        if (got == 0) break;
    }
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)len);
    free(buf);
    if (r.state == KORB_NORMAL && korb_io_is_binary(c, VALUE_REF_GET(self)))
        KORB_STR_ENC_SET(r.value, KORB_ENC_BINARY);
    return r;
}
/* IO#gets → the next line (with '\n'), or nil at EOF. */
static RESULT korb_m_io_gets(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_READ(c, slots, self);
    char *line = NULL; size_t cap = 0;
    ssize_t n = getline(&line, &cap, fp);
    if (n < 0) { free(line); return RESULT_OK(KORB_NIL); }
    RESULT r = korb_str_new(c, slots, line, (uint32_t)n);
    free(line);
    return r;
}
/* IO#readlines / IO#each_line — remaining lines. */
static RESULT korb_m_io_readlines(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_READ(c, slots, self);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 16));
    VALUE_REF arr = VALUE_REF_AT(&slots[0]);
    char *line = NULL; size_t cap = 0; ssize_t n;
    while ((n = getline(&line, &cap, fp)) >= 0) {
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, line, (uint32_t)n));
        if (korb_ary_push_val(c, slots + 2, arr, slots[1]).state != KORB_NORMAL) { free(line); return RESULT_OK(VALUE_REF_GET(arr)); }
    }
    free(line);
    return RESULT_OK(VALUE_REF_GET(arr));
}
static RESULT korb_m_io_each_line(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                  struct Node *block, VALUE *def_env, VALUE *captured_self) {
    (void)a;
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_READ(c, slots, self);
    if (block == NULL) {   /* no block → an Enumerator over the remaining lines */
        slots[0] = UNWRAP(korb_ary_new(c, slots, 16));
        VALUE_REF arr = VALUE_REF_AT(&slots[0]);
        char *ln = NULL; size_t lc = 0; ssize_t k;
        while ((k = getline(&ln, &lc, fp)) >= 0) {
            slots[1] = UNWRAP(korb_str_new(c, slots + 1, ln, (uint32_t)k));
            CHECK(korb_ary_push_val(c, slots + 2, arr, slots[1]));
        }
        free(ln);
        return korb_enum_new(c, slots + 1, VALUE_REF_GET(arr), KORB_NIL);
    }
    char *line = NULL; size_t cap = 0; ssize_t n; RESULT rr = RESULT_OK(KORB_NIL);
    while ((n = getline(&line, &cap, fp)) >= 0) {
        slots[0] = korb_str_new(c, slots, line, (uint32_t)n).value;
        rr = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
        if (rr.state != KORB_NORMAL) break;
    }
    free(line);
    if (rr.state != KORB_NORMAL) return rr;
    return RESULT_OK(VALUE_REF_GET(self));
}
/* IO#close — fclose (never on the std streams); marks the slot closed. */
static RESULT korb_m_io_close(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    const VALUE idxv = korb_ivar_get(c, VALUE_REF_GET(self), ID2SYM(korb_io_fp_mid(c)));
    if (FIXNUM_P(idxv)) {
        const intptr_t idx = FIX2LONG(idxv);
        if (idx >= 3 && (uint32_t)idx < c->vm->io_cnt && c->vm->io_fps[idx]) {   /* 0/1/2 = std streams, never close */
            fclose(c->vm->io_fps[idx]);
            c->vm->io_fps[idx] = NULL;
        }
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_io_closed_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    return RESULT_OK(korb_io_fp(c, VALUE_REF_GET(self)) ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_io_flush(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (fp) fflush(fp);
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_io_eof_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return RESULT_OK(KORB_TRUE);
    int ch = fgetc(fp);
    if (ch == EOF) return RESULT_OK(KORB_TRUE);
    ungetc(ch, fp);
    return RESULT_OK(KORB_FALSE);
}
static RESULT korb_m_io_sync_noop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots; (void)self;
    return RESULT_OK(VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : KORB_TRUE);
}
/* IO#getc → the next UTF-8 character, or nil at EOF. */
static RESULT korb_m_io_getc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_READ(c, slots, self);
    int b0 = fgetc(fp);
    if (b0 == EOF) return RESULT_OK(KORB_NIL);
    char cbuf[8]; cbuf[0] = (char)b0;
    const unsigned char u = (unsigned char)b0;
    uint32_t cl = u < 0x80 ? 1 : u >= 0xF0 ? 4 : u >= 0xE0 ? 3 : u >= 0xC0 ? 2 : 1;
    for (uint32_t k = 1; k < cl; k++) { int b = fgetc(fp); if (b == EOF) { cl = k; break; } cbuf[k] = (char)b; }
    return korb_str_new(c, slots, cbuf, cl);
}
static RESULT korb_io_raise_eof(CTX *c, VALUE *slots) {
    const VALUE cls = korb_const_get(c->vm, korb_intern(c->vm, "EOFError", 8));
    slots[0] = KORB_CLASS_P(cls) ? cls : KORB_NIL;
    RESULT r = korb_raise(c, slots + 1, KORB_E_RUNTIME, 0, "end of file reached");
    if (KORB_CLASS_P(slots[0]) && KORB_EXC_P(r.value))
        ARO_STORE(c, VAL2EXC(r.value), (VALUE *)(uintptr_t)&VAL2EXC(r.value)->exc_class, slots[0]);
    return r;
}
/* IO#readline — like gets but raises EOFError at end of file. */
static RESULT korb_m_io_readline(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    RESULT r = korb_m_io_gets(c, slots, self, a);
    if (r.state == KORB_NORMAL && r.value == KORB_NIL) return korb_io_raise_eof(c, slots);
    return r;
}
/* IO#readchar — like getc but raises EOFError at end of file. */
static RESULT korb_m_io_readchar(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    RESULT r = korb_m_io_getc(c, slots, self, a);
    if (r.state == KORB_NORMAL && r.value == KORB_NIL) return korb_io_raise_eof(c, slots);
    return r;
}
/* IO#seek(offset, whence = SEEK_SET) → 0. */
static RESULT korb_m_io_seek(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_READ(c, slots, self);
    KORB_IO_NEED_READ(c, slots, self);
    const intptr_t off = FIXNUM_P(VALUE_SLICE_GET(a, 0)) ? FIX2LONG(VALUE_SLICE_GET(a, 0)) : 0;
    const int whence = (VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1))) ? (int)FIX2LONG(VALUE_SLICE_GET(a, 1)) : SEEK_SET;
    fseek(fp, (long)off, whence);
    return RESULT_OK(LONG2FIX(0));
}
static RESULT korb_m_io_pos(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    return RESULT_OK(LONG2FIX(fp ? (intptr_t)ftell(fp) : 0));
}
static RESULT korb_m_io_pos_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (fp && FIXNUM_P(VALUE_SLICE_GET(a, 0))) fseek(fp, (long)FIX2LONG(VALUE_SLICE_GET(a, 0)), SEEK_SET);
    return RESULT_OK(VALUE_SLICE_GET(a, 0));
}
static RESULT korb_m_io_rewind(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (fp) rewind(fp);
    return RESULT_OK(LONG2FIX(0));
}
/* IO#each_char { |ch| } — yield each UTF-8 character (of the rest); no block → Enumerator. */
static RESULT korb_m_io_each_char(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                  struct Node *block, VALUE *def_env, VALUE *captured_self) {
    FILE *fp = korb_io_fp(c, VALUE_REF_GET(self));
    if (!fp) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_READ(c, slots, self);
    RESULT collect = korb_m_io_read(c, slots, self, a);   /* slurp the rest */
    if (UNLIKELY(collect.state != KORB_NORMAL)) return collect;
    slots[0] = collect.value;                             /* the String (rooted) */
    VALUE_REF sref = VALUE_REF_AT(&slots[0]);
    if (block == NULL) {                                  /* Enumerator over the characters */
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 16));
        VALUE_REF arr = VALUE_REF_AT(&slots[1]);
        const KorbString *s = VAL2STR(VALUE_REF_GET(sref));
        for (uint32_t i = 0; i < s->len; ) {
            const unsigned char b = (unsigned char)korb_strbuf_data(s->buf)[i];
            uint32_t cl = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
            if (i + cl > s->len) cl = 1;
            char cbuf[8]; memcpy(cbuf, korb_strbuf_data(s->buf) + i, cl);   /* copy before str_new's alloc moves `s` */
            slots[2] = UNWRAP(korb_str_new(c, slots + 2, cbuf, cl));
            CHECK(korb_ary_push_val(c, slots + 3, arr, slots[2]));
            s = VAL2STR(VALUE_REF_GET(sref));            /* re-read: push GC'd */
            i += cl;
        }
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(arr), KORB_NIL);
    }
    const KorbString *s = VAL2STR(VALUE_REF_GET(sref));
    for (uint32_t i = 0; i < s->len; ) {
        const unsigned char b = (unsigned char)korb_strbuf_data(s->buf)[i];
        uint32_t cl = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
        if (i + cl > s->len) cl = 1;
        char cbuf[8]; memcpy(cbuf, korb_strbuf_data(s->buf) + i, cl);   /* copy before str_new's alloc moves `s` */
        slots[1] = korb_str_new(c, slots + 1, cbuf, cl).value;
        RESULT yr = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, captured_self);
        if (yr.state != KORB_NORMAL) return yr;
        s = VAL2STR(VALUE_REF_GET(sref));                /* re-read after yield GC */
        i += cl;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* File.open(path, mode = "r") [ { |io| ... } ] — with a block, yields the IO and
 * closes it after (returning the block value); without, returns the IO. */
static RESULT korb_m_file_open(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                               struct Node *block, VALUE *def_env, VALUE *captured_self) {
    const VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
    int rw = 0; bool binary = false; FILE *fp;
    if (VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1))) {   /* integer O_* flags → open(2) */
        const int fl = (int)FIX2LONG(VALUE_SLICE_GET(a, 1));
        const mode_t perm = (VALUE_SLICE_LEN(a) >= 3 && FIXNUM_P(VALUE_SLICE_GET(a, 2))) ? (mode_t)FIX2LONG(VALUE_SLICE_GET(a, 2)) : 0666;
        const int acc = fl & 3;   /* O_RDONLY=0 / O_WRONLY=1 / O_RDWR=2 */
        rw = acc == 1 ? 2 : acc == 2 ? 3 : 1;
        const int fd = open(path, fl, perm);
        if (fd < 0) return korb_raise_errno(c, slots, errno, "rb_sysopen", path);
        fp = fdopen(fd, acc == 1 ? ((fl & O_APPEND) ? "a" : "w") : acc == 2 ? "r+" : "r");   /* wraps the fd; no re-truncate */
        if (!fp) { close(fd); return korb_raise_errno(c, slots, errno, "rb_sysopen", path); }
    } else {
        char mode[8] = "r";
        if (VALUE_SLICE_LEN(a) >= 2 && KORB_STRING_P(VALUE_SLICE_GET(a, 1))) {
            uint32_t ml; const char *m = korb_str_cstr_len(VALUE_SLICE_GET(a, 1), &ml);
            if (ml > 0 && ml < sizeof(mode)) { memcpy(mode, m, ml); mode[ml] = '\0'; }
        }
        const char b = mode[0]; const bool plus = strchr(mode, '+') != NULL;   /* validate + derive rw bits */
        binary = strchr(mode, 'b') != NULL;                                     /* 'rb'/'wb'/… → byte-encoded reads */
        if (b == 'r') rw = plus ? 3 : 1;
        else if (b == 'w' || b == 'a') rw = plus ? 3 : 2;
        else return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid access mode %s", mode);
        fp = fopen(path, mode);
        if (!fp) return korb_raise_errno(c, slots, errno, "rb_sysopen", path);
    }
    slots[0] = UNWRAP(korb_io_make(c, slots, VALUE_REF_GET(self), fp, rw));   /* self = the File class */
    VALUE_REF io = VALUE_REF_AT(&slots[0]);
    if (binary)   /* remember binary mode → reads produce ASCII-8BIT strings */
        CHECK(korb_ivar_set(c, slots + 1, io, ID2SYM(korb_io_bin_mid(c)), KORB_TRUE));
    CHECK(korb_ivar_set(c, slots + 1, io, ID2SYM(korb_intern(c->vm, "@__io_path", 10)),
                        VALUE_SLICE_GET(a, 0)));   /* File#path / #to_path */
    if (block == NULL) return RESULT_OK(VALUE_REF_GET(io));
    slots[1] = VALUE_REF_GET(io);
    RESULT br = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, captured_self);
    /* ensure close via the object's #close (so a subclass override runs and is
     * observable); the block value is the return, but a close error propagates
     * when the block itself succeeded (CRuby's ensure semantics). */
    slots[1] = br.value;                      /* root the block's value/exception across close's GC */
    slots[2] = VALUE_REF_GET(io);             /* receiver for #close */
    RESULT cr = korb_send(c, slots + 3, korb_intern(c->vm, "close", 5), 0, 0);
    if (br.state != KORB_NORMAL) { br.value = slots[1]; return br; }   /* block error wins (re-read moved value) */
    if (cr.state != KORB_NORMAL) return cr;   /* else a genuine close error propagates */
    return RESULT_OK(slots[1]);               /* success → the block's (possibly moved) value */
}

void korb_init_io(CTX *c, VALUE *slots) {
    struct korb_vm *const vm = c->vm;
    /* index 0/1/2 = std streams (never fclose'd). */
    korb_io_register(vm, stdin); korb_io_register(vm, stdout); korb_io_register(vm, stderr);

    const VALUE obj_cls = korb_builtin_class_obj(vm, KORB_C_OBJECT);   /* IO < Object → inherits class/inspect/... */
    slots[0] = (korb_class_new(c, slots, korb_intern(vm, "IO", 2), obj_cls)).value;
    korb_const_define(c, korb_intern(vm, "IO", 2), slots[0]);
    const VALUE io_cls = slots[0];
#define IOM(nm, fn, ar)  korb_class_def_cfn(c, io_cls, nm, korb_m_io_##fn, ar)
#define IOB(nm, fn, ar)  korb_class_def_cfn_blk(c, io_cls, nm, korb_m_io_##fn, ar)
    IOM("write", write, -1);     IOM("print", print, -1);   IOM("<<", lshift, 1);
    IOM("printf", printf, -1);
    IOM("puts", puts, -1);       IOM("read", read, -1);     IOM("gets", gets, -1);
    IOM("readlines", readlines, -1);                        IOB("each_line", each_line, -1);
    IOB("each", each_line, -1);
    IOM("close", close, 0);      IOM("closed?", closed_p, 0);
    IOM("stat", stat, 0);        IOM("fileno", fileno, 0);
    IOM("tty?", tty_p, 0);       IOM("isatty", tty_p, 0);
    IOM("truncate", truncate, 1);
    IOM("flush", flush, 0);      IOM("eof?", eof_p, 0);     IOM("eof", eof_p, 0);
    IOM("sync", sync_noop, 0);   IOM("sync=", sync_noop, 1);
    IOM("seek", seek, -1);       IOM("pos", pos, 0);        IOM("tell", pos, 0);
    IOM("pos=", pos_set, 1);     IOM("rewind", rewind, 0);
    IOB("each_char", each_char, 0);   IOM("getc", getc, 0);
    IOM("readline", readline, -1);    IOM("readchar", readchar, 0);
    IOM("wait_readable", wait_readable, -1);   /* POLL blop (builtins/thread.c) */
    IOM("wait_writable", wait_writable, -1);
    IOM("__io_poll", poll_raw, -1);            /* 汎用 events poll (IO#wait 用) */
    korb_const_define(c, korb_intern(vm, "SEEK_SET", 8), LONG2FIX(SEEK_SET));
    korb_const_define(c, korb_intern(vm, "SEEK_CUR", 8), LONG2FIX(SEEK_CUR));
    korb_const_define(c, korb_intern(vm, "SEEK_END", 8), LONG2FIX(SEEK_END));
    /* IO.read/write/readlines/foreach/binread/binwrite — the File class methods. */
    const VALUE io_sing = korb_obj_singleton(c, slots + 1, io_cls).value;
    korb_class_def_cfn(c, io_sing, "read",      korb_m_file_read,      -1);
    korb_class_def_cfn(c, io_sing, "binread",   korb_m_file_read,      -1);
    korb_class_def_cfn(c, io_sing, "write",     korb_m_file_write,     -1);
    korb_class_def_cfn(c, io_sing, "binwrite",  korb_m_file_write,     -1);
    korb_class_def_cfn(c, io_sing, "readlines", korb_m_file_readlines, -1);
    korb_class_def_cfn_blk(c, io_sing, "foreach", korb_m_file_foreach, -1);
    korb_class_def_cfn(c, io_sing, "select",    korb_m_io_s_select,    -1);   /* POLL blop (thread.c) */
    korb_class_def_cfn_blk(c, io_sing, "pipe",  korb_m_io_s_pipe,      -1);
#undef IOM
#undef IOB
    /* reparent File under IO so File.open's instances inherit these methods.
     * Re-fetch IO from the (rooted) const table: the `io_cls` C local was cached
     * at line ~430 and the korb_obj_singleton allocations above can move the IO
     * class under a moving GC, leaving `io_cls` stale — writing that stale value
     * into File.superclass loses the edge under STRESS+PURGE. */
    const VALUE file_cls = korb_const_get(vm, korb_intern(vm, "File", 4));
    const VALUE io_cls_live = korb_const_get(vm, korb_intern(vm, "IO", 2));
    if (KORB_CLASS_P(file_cls) && KORB_CLASS_P(io_cls_live)) {
        ARO_STORE(c, VAL2CLASS(file_cls), (VALUE *)(uintptr_t)&VAL2CLASS(file_cls)->superclass, io_cls_live);   /* GC edge: barriered write */
        vm->method_serial++;
        const VALUE fsing = korb_obj_singleton(c, slots + 1, file_cls).value;
        korb_class_def_cfn_blk(c, fsing, "open", korb_m_file_open, -1);   /* File.open (block) */
        korb_class_def_cfn_blk(c, fsing, "new", korb_m_file_open, -1);    /* File.new = open, no block-close */
    }
    /* $stdout / $stderr / $stdin + STDOUT / STDERR / STDIN — IO objects on the std slots. */
    struct { const char *gv, *cn; uint32_t idx; } sv[] = {
        {"$stdin", "STDIN", 0}, {"$stdout", "STDOUT", 1}, {"$stderr", "STDERR", 2},
    };
    for (size_t i = 0; i < 3; i++) {
        slots[1] = korb_obj_new(c, slots + 1, slots[0]).value;   /* re-read slots[0]: korb_obj_new GCs and moves the IO class (a stale local would mis-klass the instance) */
        (void)korb_ivar_set(c, slots + 2, VALUE_REF_AT(&slots[1]), ID2SYM(korb_io_fp_mid(c)), LONG2FIX((intptr_t)sv[i].idx));
        korb_const_define(c, korb_intern(vm, sv[i].gv, (uint32_t)strlen(sv[i].gv)), slots[1]);
        korb_const_define(c, korb_intern(vm, sv[i].cn, (uint32_t)strlen(sv[i].cn)), slots[1]);
        if (sv[i].idx >= 1 && AROH_IS_GC_OBJECT(slots[1]))   /* mark default $stdout/$stderr for the fast fwrite path */
            ((AroObjectHeader *)(uintptr_t)slots[1])->flags |= KORB_FL_DEFAULT_IO;
    }
}
