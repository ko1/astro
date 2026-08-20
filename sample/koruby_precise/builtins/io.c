#include <sys/socket.h>
#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h>
/* koruby_precise — io.c: the IO/File object layer, driven straight from a file
 * descriptor.  An IO/File instance stores its slot index (into vm->io_reps) in
 * the @__io_fp ivar; the rep itself is a raw C pointer kept off-heap, so no GC
 * scanning is needed.  #included into korb_runtime.c after file.c.
 *
 * No stdio: a FILE* keeps buffered bytes the scheduler cannot see and only
 * offers blocking reads, so a green thread parked inside one would stall every
 * other thread.  Owning the buffers is what makes a park possible. */

/* One open stream.  libc-allocated and never moved (like KorbFiberRep), so the
 * read-ahead and write-behind buffers living here stay put across a park — the
 * IO object holds only the table index, and a String never lends its (movable)
 * bytes to an operation that can suspend. */
typedef struct KorbIORep {
    int      fd;                     /* -1 = closed; KORB_IO_FD_MEM = in-memory sink */
    uint8_t  eof;                    /* a read(2) returned 0 */
    uint8_t  sync;                   /* flush after every write */
    uint8_t  nonblk;                 /* O_NONBLOCK: block by parking, not by stalling */
    uint32_t lineno;                 /* IO#lineno: lines handed out by #gets */
    char    *rbuf; uint32_t rpos, rlen, rcapa;   /* read-ahead: live bytes are [rpos, rlen) */
    char    *wbuf; uint32_t wlen, wcapa;         /* write-behind: wlen bytes pending */
} KorbIORep;

/* An in-memory sink: writes accumulate in wbuf and are never flushed.  Used
 * where output has to be captured as a String (a reassigned $stdout) rather
 * than reaching a descriptor — same writer code, no descriptor involved. */
#define KORB_IO_FD_MEM (-2)
#define KORB_IO_BUFSZ  8192u

static uint32_t korb_io_register_rep(struct korb_vm *vm, KorbIORep *rep) {
    if (vm->io_cnt == vm->io_capa) {
        vm->io_capa = vm->io_capa ? vm->io_capa * 2 : 8;
        vm->io_reps = realloc(vm->io_reps, sizeof(KorbIORep *) * vm->io_capa);
        if (!vm->io_reps) { fprintf(stderr, "koruby_precise: oom (io table)\n"); abort(); }
    }
    vm->io_reps[vm->io_cnt] = rep;
    return vm->io_cnt++;
}

static uint32_t korb_io_register(struct korb_vm *vm, int fd, bool sync) {
    KorbIORep *rep = calloc(1, sizeof(KorbIORep));
    if (!rep) { fprintf(stderr, "koruby_precise: oom (io rep)\n"); abort(); }
    rep->fd = fd;
    rep->sync = sync ? 1 : 0;
    return korb_io_register_rep(vm, rep);
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
static KorbIORep *korb_io_rep(CTX *c, VALUE self) {
    const VALUE idxv = korb_ivar_get(c, self, ID2SYM(korb_io_fp_mid(c)));
    if (!FIXNUM_P(idxv)) return NULL;
    const intptr_t idx = FIX2LONG(idxv);
    if (idx < 0 || (uint32_t)idx >= c->vm->io_cnt) return NULL;
    return c->vm->io_reps[idx];
}
/* Put a stream into the parking regime: its descriptor goes non-blocking, so a
 * would-be blocking read/write parks the green thread instead of stalling every
 * thread in the process.  Only for descriptors koruby owns both ends of the
 * policy for (pipes, sockets) — a shared std stream keeps kernel blocking, since
 * O_NONBLOCK there is visible to whatever else holds the same open file. */
static void korb_io_set_nonblock(KorbIORep *const rep) {
    if (!rep || rep->fd < 0) return;
    const int fl = fcntl(rep->fd, F_GETFL);
    if (fl >= 0 && fcntl(rep->fd, F_SETFL, fl | O_NONBLOCK) == 0) rep->nonblk = 1;
}

/* The descriptor, straight from the rep — no stdio needed.  -1 when closed. */
static int korb_io_fd(CTX *c, VALUE self) {
    const KorbIORep *const rep = korb_io_rep(c, self);
    return rep ? rep->fd : -1;
}
/* true while the stream is usable (an in-memory sink counts as open). */
static bool korb_io_open_p(const KorbIORep *const rep) { return rep != NULL && rep->fd != -1; }

/* The std streams occupy the first three rep slots: 0 = stdin, 1 = stdout,
 * 2 = stderr.  Kernel#puts and friends write through these rather than through
 * stdio, so their output interleaves correctly with $stdout.write. */
static KorbIORep *korb_io_std_rep(struct korb_vm *const vm, uint32_t which) {
    return which < vm->io_cnt ? vm->io_reps[which] : NULL;
}
/* A scratch in-memory sink: `KorbIORep mem = KORB_IO_MEM_SINK;`, write into it,
 * read the bytes off mem.wbuf/mem.wlen, then free(mem.wbuf). */
#define KORB_IO_MEM_SINK ((KorbIORep){ .fd = KORB_IO_FD_MEM })

/* ---- the byte layer -----------------------------------------------------
 * Every read and write in the interpreter funnels through these.  They are the
 * only place that touches a descriptor, which is what keeps the eventual
 * "try → EAGAIN → park → retry" change confined to two functions. */

/* Wait for `fd` to become ready, letting every other green thread run.
 *
 * Discipline, and the reason it is spelled out here: the order is always
 * "try the syscall → EAGAIN → park → retry", never "park → syscall".  poll
 * reporting a descriptor readable does not promise the next read will not
 * block (a spurious wakeup, or another thread draining the same fd first),
 * so a blocking call after a park would stall every thread in the process.
 *
 * Parking may GC — other threads allocate while we are away — so no caller
 * may hold a pointer into a movable object across this.  That is why the
 * transfer buffers live in the rep, which libc allocated and never moves. */
static RESULT korb_io_park(CTX *c, VALUE *slots, const KorbIORep *const rep, short events) {
    struct pollfd pf; pf.fd = rep->fd; pf.events = events; pf.revents = 0;
    ssize_t ready = 0;
    return korb_blop_poll_wait(c, slots, &pf, 1, -1.0, &ready);
}
/* true when the errno from a just-failed syscall means "would block". */
static bool korb_io_would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK; }

/* Push the pending output out.  false (with errno set) on a write error; an
 * in-memory sink never flushes, so its bytes simply accumulate.
 *
 * `c` NULL means "must not park" (exit and close paths, where suspending is
 * either impossible or pointless); such a call falls back to blocking.
 * `perr` collects a raise delivered while parked (Thread#raise / #kill). */
static bool korb_io_flush_rep_p(CTX *c, VALUE *slots, KorbIORep *const rep, RESULT *perr) {
    if (rep->fd == KORB_IO_FD_MEM) return true;
    if (UNLIKELY(rep->fd < 0)) return false;
    uint32_t off = 0;
    while (off < rep->wlen) {
        const ssize_t w = write(rep->fd, rep->wbuf + off, rep->wlen - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (korb_io_would_block(errno) && rep->nonblk && c != NULL) {
                const RESULT pr = korb_io_park(c, slots, rep, POLLOUT);   /* may GC */
                if (UNLIKELY(pr.state != KORB_NORMAL)) {
                    if (perr) *perr = pr;
                    /* keep the untransferred tail so a retry can still send it */
                    memmove(rep->wbuf, rep->wbuf + off, rep->wlen - off);
                    rep->wlen -= off;
                    return false;
                }
                continue;
            }
            /* Drop what could not be written: retrying it on the next flush
               would emit bytes out of order behind the caller's back. */
            rep->wlen = 0;
            return false;
        }
        off += (uint32_t)w;
    }
    rep->wlen = 0;
    return true;
}
/* non-parking shorthand for the paths that cannot suspend */
static bool korb_io_flush_rep(KorbIORep *const rep) { return korb_io_flush_rep_p(NULL, NULL, rep, NULL); }

/* Grow the write-behind buffer to hold at least `need` bytes. */
static bool korb_io_wbuf_grow(KorbIORep *const rep, uint32_t need) {
    uint32_t capa = rep->wcapa ? rep->wcapa : KORB_IO_BUFSZ;
    while (capa < need) capa *= 2;
    char *const nb = realloc(rep->wbuf, capa);
    if (!nb) return false;
    rep->wbuf = nb; rep->wcapa = capa;
    return true;
}

/* Buffer n bytes for output.
 *
 * `p` typically points into a String, so it must not be read across a park: a
 * GC there would move it.  For a stream that can park we therefore take the
 * whole run into the rep's buffer first and only then drain, so every park
 * happens with no borrowed pointer live. */
static bool korb_io_wr_p(CTX *c, VALUE *slots, KorbIORep *const rep, const char *p, size_t n, RESULT *perr) {
    if (UNLIKELY(!korb_io_open_p(rep))) return false;
    const bool parks = rep->nonblk && c != NULL;
    if (rep->fd == KORB_IO_FD_MEM || parks) {            /* capture, or copy-then-drain */
        if (rep->wlen + n > rep->wcapa && !korb_io_wbuf_grow(rep, (uint32_t)(rep->wlen + n))) return false;
        memcpy(rep->wbuf + rep->wlen, p, n);
        rep->wlen += (uint32_t)n;                        /* `p` is dead from here on */
        if (rep->fd == KORB_IO_FD_MEM) return true;
        return (rep->sync || rep->wlen >= KORB_IO_BUFSZ) ? korb_io_flush_rep_p(c, slots, rep, perr) : true;
    }
    /* A bulk write goes straight to the descriptor: copying it through the
       buffer would cost a memcpy per byte and buy nothing. */
    if (n >= KORB_IO_BUFSZ) {
        if (!korb_io_flush_rep_p(c, slots, rep, perr)) return false;
        size_t off = 0;
        while (off < n) {
            const ssize_t w = write(rep->fd, p + off, n - off);
            if (w < 0) { if (errno == EINTR) continue; return false; }
            off += (size_t)w;
        }
        return true;
    }
    if (rep->wcapa == 0 && !korb_io_wbuf_grow(rep, KORB_IO_BUFSZ)) return false;
    while (n > 0) {
        if (rep->wlen == rep->wcapa && !korb_io_flush_rep_p(c, slots, rep, perr)) return false;
        const uint32_t room = rep->wcapa - rep->wlen;
        const uint32_t take = n < room ? (uint32_t)n : room;
        memcpy(rep->wbuf + rep->wlen, p, take);
        rep->wlen += take; p += take; n -= take;
    }
    return rep->sync ? korb_io_flush_rep_p(c, slots, rep, perr) : true;
}
/* non-parking shorthand (diagnostics, exit paths, in-memory sinks) */
static bool korb_io_wr(KorbIORep *const rep, const char *p, size_t n) {
    return korb_io_wr_p(NULL, NULL, rep, p, n, NULL);
}

/* Bytes available in the read-ahead buffer, refilling it when empty.  0 = EOF.
 *
 * MAY GC when it parks (see korb_io_park): a caller must re-derive any pointer
 * into a movable object afterwards.  The destination buffer here is the rep's
 * own, which is why the refill itself is safe.  `c` NULL = must not park. */
static uint32_t korb_io_fill_p(CTX *c, VALUE *slots, KorbIORep *const rep, RESULT *perr) {
    if (rep->rpos < rep->rlen) return rep->rlen - rep->rpos;
    rep->rpos = rep->rlen = 0;
    if (UNLIKELY(!korb_io_open_p(rep)) || rep->fd == KORB_IO_FD_MEM || rep->eof) return 0;
    /* A duplex stream (socketpair, "r+" popen, a socket) may hold buffered
     * output that the peer is waiting for; blocking in read(2) without sending
     * it first deadlocks both sides. */
    if (rep->wlen > 0) (void)korb_io_flush_rep(rep);
    if (rep->rcapa == 0) {
        rep->rbuf = malloc(KORB_IO_BUFSZ);
        if (!rep->rbuf) return 0;
        rep->rcapa = KORB_IO_BUFSZ;
    }
    for (;;) {
        const ssize_t n = read(rep->fd, rep->rbuf, rep->rcapa);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (korb_io_would_block(errno) && rep->nonblk && c != NULL) {
                const RESULT pr = korb_io_park(c, slots, rep, POLLIN);   /* may GC */
                if (UNLIKELY(pr.state != KORB_NORMAL)) { if (perr) *perr = pr; return 0; }
                continue;                                /* ready (maybe) → try again, never block */
            }
            return 0;
        }
        if (n == 0) { rep->eof = 1; return 0; }
        rep->rlen = (uint32_t)n;
        return rep->rlen;
    }
}

/* One byte, or -1 at EOF.  Parks rather than stalling when the stream can. */
static int korb_io_getb_p(CTX *c, VALUE *slots, KorbIORep *const rep, RESULT *perr) {
    if (rep->rpos >= rep->rlen && korb_io_fill_p(c, slots, rep, perr) == 0) return -1;
    return (unsigned char)rep->rbuf[rep->rpos++];
}

/* Write to a sink, reporting a vanished reader as Errno::EPIPE.
 *
 * SIGPIPE is ignored process-wide (see korb_init_io), so nothing kills us on a
 * broken pipe any more — the write path itself has to notice.  Without this a
 * `loop { puts }` into a closed pipe would spin forever instead of ending the
 * program, which is what CRuby does. */
static RESULT korb_io_wr_checked(CTX *c, VALUE *slots, KorbIORep *const rep, const char *p, size_t n) {
    RESULT werr = RESULT_OK(KORB_NIL);
    errno = 0;
    if (!korb_io_wr_p(c, slots, rep, p, n, &werr)) {
        if (UNLIKELY(werr.state != KORB_NORMAL)) return werr;
        if (errno == EPIPE) return korb_raise_errno(c, slots, EPIPE, "write", "");
    }
    return RESULT_OK(KORB_NIL);
}

/* Write, propagating a raise delivered while parked (Thread#raise / #kill). */
#define KORB_IO_WR(c_, slots_, rep_, p_, n_) do {                              \
    RESULT werr__ = RESULT_OK(KORB_NIL);                                       \
    errno = 0;                                                                 \
    const bool wok__ = korb_io_wr_p((c_), (slots_), (rep_), (p_), (n_), &werr__); \
    if (UNLIKELY(werr__.state != KORB_NORMAL)) return werr__;                  \
    /* Only EPIPE: CRuby reports a vanished reader, but a write that fails for  \
       any other reason (a closed or bad descriptor) has always been silent     \
       here, and turning those into raises changes unrelated paths. */          \
    if (UNLIKELY(!wok__ && errno == EPIPE))                                    \
        return korb_raise_errno((c_), (slots_), EPIPE, "write", "");           \
} while (0)

/* Push bytes back so the next read returns them.  Unlike a FILE*'s one-byte
 * pushback this takes a whole run, because the buffer is ours to shift. */
static bool korb_io_unget(KorbIORep *const rep, const char *p, uint32_t n) {
    if (n == 0) return true;
    if (n <= rep->rpos) {                                /* fits in the space already consumed */
        rep->rpos -= n;
        memcpy(rep->rbuf + rep->rpos, p, n);
        return true;
    }
    const uint32_t live = rep->rlen - rep->rpos;
    if (live + n > rep->rcapa) {                         /* make room ahead of the live bytes */
        uint32_t capa = rep->rcapa ? rep->rcapa : KORB_IO_BUFSZ;
        while (capa < live + n) capa *= 2;
        char *const nb = realloc(rep->rbuf, capa);
        if (!nb) return false;
        rep->rbuf = nb; rep->rcapa = capa;
    }
    memmove(rep->rbuf + n, rep->rbuf + rep->rpos, live);
    memcpy(rep->rbuf, p, n);
    rep->rpos = 0; rep->rlen = live + n;
    rep->eof = 0;                                        /* there is data again */
    return true;
}

/* Discard the read-ahead — the descriptor is about to move under us. */
static void korb_io_drop_rbuf(KorbIORep *const rep) { rep->rpos = rep->rlen = 0; rep->eof = 0; }

/* The logical file position: where the descriptor is, less the bytes we read
 * ahead of the caller, plus the bytes we have not written out yet. */
static off_t korb_io_tell_rep(KorbIORep *const rep) {
    if (UNLIKELY(!korb_io_open_p(rep)) || rep->fd == KORB_IO_FD_MEM) return 0;
    const off_t at = lseek(rep->fd, 0, SEEK_CUR);
    if (at < 0) return 0;
    return at - (off_t)(rep->rlen - rep->rpos) + (off_t)rep->wlen;
}

static off_t korb_io_seek_rep(KorbIORep *const rep, off_t off, int whence) {
    if (UNLIKELY(!korb_io_open_p(rep)) || rep->fd == KORB_IO_FD_MEM) return -1;
    if (!korb_io_flush_rep(rep)) return -1;
    /* SEEK_CUR is relative to what the caller has seen, not to where the
       descriptor sits after a refill. */
    if (whence == SEEK_CUR) off -= (off_t)(rep->rlen - rep->rpos);
    korb_io_drop_rbuf(rep);
    return lseek(rep->fd, off, whence);
}

/* Flush, close the descriptor and release the buffers.  Idempotent. */
static void korb_io_close_rep(KorbIORep *const rep) {
    if (!rep) return;
    if (rep->fd >= 0) { (void)korb_io_flush_rep(rep); close(rep->fd); }
    rep->fd = -1;
    free(rep->rbuf); rep->rbuf = NULL; rep->rcapa = rep->rpos = rep->rlen = 0;
    free(rep->wbuf); rep->wbuf = NULL; rep->wcapa = rep->wlen = 0;
    rep->eof = 0;
}

/* Push out whatever the std streams still hold.  With stdio gone nothing does
 * this for us at exit, so every path that leaves the process goes through it. */
void korb_io_flush_std(struct korb_vm *const vm) {
    for (uint32_t i = 0; i < 3 && i < vm->io_cnt; i++)
        if (vm->io_reps[i]) (void)korb_io_flush_rep(vm->io_reps[i]);
}
/* new IO/File instance of `klass` bound to `fd`.  slots[0..] scratch; result rooted by caller. */
static RESULT korb_io_make(CTX *c, VALUE *slots, VALUE klass, int fd, int rw) {
    const uint32_t idx = korb_io_register(c->vm, fd, false);
    slots[0] = UNWRAP(korb_obj_new(c, slots, klass));
    CHECK(korb_ivar_set(c, slots + 1, VALUE_REF_AT(&slots[0]), ID2SYM(korb_io_fp_mid(c)), LONG2FIX((intptr_t)idx)));
    CHECK(korb_ivar_set(c, slots + 1, VALUE_REF_AT(&slots[0]), ID2SYM(korb_io_mode_mid(c)), LONG2FIX(rw)));
    return RESULT_OK(slots[0]);
}

/* ---- String-facing read helpers -----------------------------------------
 * The transfer target is a freshly allocated String's buffer: korb_str_alloc
 * hands back an uninitialized run of bytes and korb_io_rd allocates nothing, so
 * that (movable) buffer cannot be relocated underneath the copy.  The stream's
 * own read-ahead lives in the rep, which never moves — so once a read learns to
 * park, only the rep-side buffer is in play and this stays correct. */

/* Read up to `want` bytes.  *got_out is the byte count actually transferred. */
static RESULT korb_io_read_bytes(CTX *c, VALUE *slots, KorbIORep *rep, uint32_t want, uint32_t *got_out) {
    slots[0] = (VALUE)korb_str_alloc(c, slots, want);     /* root before any later alloc */
    const VALUE_REF sref = VALUE_REF_AT(&slots[0]);
    uint32_t len = 0;
    RESULT err = RESULT_OK(KORB_NIL);
    while (len < want) {
        const uint32_t avail = korb_io_fill_p(c, slots + 1, rep, &err);   /* may park → may GC */
        if (UNLIKELY(err.state != KORB_NORMAL)) return err;
        if (avail == 0) break;
        const uint32_t take = (want - len) < avail ? (want - len) : avail;
        KorbString *const s = VAL2STR(VALUE_REF_GET(sref));   /* re-derive: the fill may have moved it */
        memcpy(korb_strbuf_data(s->buf) + len, rep->rbuf + rep->rpos, take);
        rep->rpos += take;
        len += take;
        s->len = len;
    }
    KorbString *const s = VAL2STR(VALUE_REF_GET(sref));
    s->len = len;                                         /* shrink in place; capa stays */
    korb_strbuf_data(s->buf)[len] = '\0';
    *got_out = len;
    return RESULT_OK(VALUE_REF_GET(sref));
}

/* Read to EOF into one String, growing as needed. */
static RESULT korb_io_read_all_bytes(CTX *c, VALUE *slots, KorbIORep *rep) {
    slots[0] = (VALUE)korb_str_alloc(c, slots, 0);
    VALUE_REF sref = VALUE_REF_AT(&slots[0]);
    uint32_t len = 0;
    RESULT err = RESULT_OK(KORB_NIL);
    for (;;) {
        const uint32_t avail = korb_io_fill_p(c, slots + 1, rep, &err);   /* may park → may GC */
        if (UNLIKELY(err.state != KORB_NORMAL)) return err;
        if (avail == 0) break;
        /* keep s->len in step: korb_str_ensure's grow copies only s->len bytes,
           so a stale 0 here would drop everything read so far. */
        KorbString *const s = korb_str_ensure(c, slots + 1, sref, len + avail);
        memcpy(korb_strbuf_data(s->buf) + len, rep->rbuf + rep->rpos, avail);
        rep->rpos += avail;
        len += avail;
        s->len = len;
    }
    KorbString *s = VAL2STR(VALUE_REF_GET(sref));
    s->len = len;
    korb_strbuf_data(s->buf)[len] = '\0';
    return RESULT_OK(VALUE_REF_GET(sref));
}

/* Read one line, up to and including '\n' (nil at EOF).  The scan runs over the
 * rep's buffer, which stays put across the String growth below. */

/* coerce v to a String (to_s if needed) and write it; accumulate bytes. */
static RESULT korb_io_emit(CTX *c, VALUE *slots, VALUE v, KorbIORep *rep, size_t *nbytes) {
    if (!KORB_STRING_P(v)) {
        slots[0] = v;
        RESULT r = korb_send(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        v = r.value;
    }
    if (KORB_STRING_P(v)) {
        const KorbString *const s = VAL2STR(v);
        KORB_IO_WR(c, slots, rep, korb_strbuf_data(s->buf), s->len);
        *nbytes += s->len;
    }
    return RESULT_OK(KORB_NIL);
}

/* The receiver's class name for a message ("Wrap", not the generic "Object"
 * korb_type_name gives a user instance). */
static void korb_io_class_name(CTX *c, VALUE v, char *const buf, size_t sz) {
    if (KORB_OBJECT_P(v)) {
        const VALUE cls = VAL2OBJ(v)->klass;
        if (KORB_CLASS_P(cls) && VAL2CLASS(cls)->name_sym) { korb_class_qname_into(c, cls, buf, sz); return; }
    }
    snprintf(buf, sz, "%s", korb_type_name(v));
}

/* Integer argument of an IO method: Integer / Float (truncated) as-is, anything
 * else through #to_int (CRuby's rb_to_int).  May dispatch → may GC, so callers
 * must re-read any VALUE they still need afterwards. */
static RESULT korb_io_arg_int(CTX *c, VALUE *slots, VALUE v, intptr_t *out) {
    if (LIKELY(korb_to_index(v, out))) return RESULT_OK(KORB_TRUE);
    const char *const cls = korb_type_name(v);         /* capture before dispatch (v may move) */
    VALUE t = v;
    const RESULT r = korb_coerce_to_int(c, slots, &t);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (UNLIKELY(r.value != KORB_TRUE) || !korb_to_index(t, out))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", cls);
    return RESULT_OK(KORB_TRUE);
}

/* IO#truncate(len) → 0 (ftruncate the descriptor). */
static RESULT korb_m_io_truncate(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    intptr_t len;
    CHECK(korb_io_arg_int(c, slots, VALUE_SLICE_GET(a, 0), &len));
    (void)korb_io_flush_rep(rep);
    if (ftruncate(korb_io_fd(c, VALUE_REF_GET(self)), (off_t)len) != 0) return korb_raise_errno(c, slots, errno, "ftruncate", "");
    return RESULT_OK(LONG2FIX(0));
}
/* IO#fileno → the integer file descriptor. */
static RESULT korb_m_io_fileno(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    return RESULT_OK(LONG2FIX(korb_io_fd(c, VALUE_REF_GET(self))));
}
static RESULT korb_m_io_tty_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; (void)slots;
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    return RESULT_OK((korb_io_open_p(rep) && isatty(rep->fd)) ? KORB_TRUE : KORB_FALSE);
}
/* IO#stat → File::Stat of the open descriptor (fstat). */
static RESULT korb_m_io_stat(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    struct stat st;
    if (fstat(korb_io_fd(c, VALUE_REF_GET(self)), &st) != 0) return korb_raise_errno(c, slots, errno, "fstat", "");
    return korb_stat_make(c, slots, &st);
}
static RESULT korb_bi_format(CTX *c, VALUE *slots, VALUE_SLICE args);   /* fwd (korb_runtime.c) */
/* IO#printf(format, *args) → nil: write the sprintf-formatted string to self
 * (was falling back to Kernel#printf, which writes to $stdout, not the file). */
static RESULT korb_m_io_printf(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) == 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no format string given");
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_WRITE(c, slots, self);
    slots[0] = VALUE_REF_GET(self);                      /* root self across the format dispatch (may GC) */
    RESULT fr = korb_bi_format(c, slots + 1, a);
    if (UNLIKELY(fr.state != KORB_NORMAL)) return fr;
    KorbIORep *const rep2 = korb_io_rep(c, slots[0]);    /* re-fetch after possible GC */
    if (!korb_io_open_p(rep2)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    if (KORB_STRING_P(fr.value)) { const KorbString *const s = VAL2STR(fr.value); KORB_IO_WR(c, slots + 1, rep2, korb_strbuf_data(s->buf), s->len); }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_io_capture_default_internal(CTX *c, VALUE *slots, VALUE_REF io);   /* fwd (defined below) */
/* IO.pipe → [r, w]  (block form: yield r, w; ensure both closed).
 * 書き込み側は sync (buffer に溜めない) にして「write → 相手が即 read できる」
 * という pipe の期待通りに振る舞わせる。 */
static RESULT korb_m_io_s_pipe(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                               NODE *block, VALUE *def_env, VALUE *cself) {
    int fds[2];
    if (pipe(fds) != 0) return korb_raise_errno(c, slots, errno, "pipe", "");
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    slots[0] = VALUE_REF_GET(self);                       /* IO class (root) */
    slots[1] = UNWRAP(korb_io_make(c, slots + 2, slots[0], fds[0], 1));   /* r (read) */
    slots[2] = UNWRAP(korb_io_make(c, slots + 3, slots[0], fds[1], 2));   /* w (write) */
    { KorbIORep *const rd = korb_io_rep(c, slots[1]);
      KorbIORep *const wr = korb_io_rep(c, slots[2]);
      if (wr) wr->sync = 1;                              /* write → the peer can read it now */
      korb_io_set_nonblock(rd); korb_io_set_nonblock(wr); }
    /* the encoding arguments (and any options Hash) apply to the read end */
    CHECK(korb_io_capture_default_internal(c, slots + 3, VALUE_REF_AT(&slots[1])));
    {
        const uint32_t an = VALUE_SLICE_LEN(a);
        uint32_t pos = an;                                /* positional encoding args */
        bool has_opts = false;
        if (pos > 0 && KORB_HASH_P(VALUE_SLICE_GET(a, pos - 1))) { pos--; has_opts = true; }
        if (pos > 2) pos = 2;
        if (pos > 0) {
            slots[3] = slots[1];                          /* recv */
            for (uint32_t i = 0; i < pos; i++) slots[4 + i] = VALUE_SLICE_GET(a, i);
            const RESULT sr = korb_send(c, slots + 4 + pos, korb_intern(c->vm, "set_encoding", 12), 0, pos);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        }
        if (has_opts) {
            slots[3] = slots[1];
            slots[4] = VALUE_SLICE_GET(a, an - 1);
            const RESULT or_ = korb_send(c, slots + 5, korb_intern(c->vm, "__apply_open_opts", 17), 0, 1);
            if (UNLIKELY(or_.state != KORB_NORMAL)) return or_;
        }
    }
    slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));
    { VALUE_REF pr = VALUE_REF_AT(&slots[3]);
      CHECK(korb_ary_push_val(c, slots + 4, pr, slots[1]));
      CHECK(korb_ary_push_val(c, slots + 4, pr, slots[2])); }
    if (block == NULL) return RESULT_OK(slots[3]);
    /* block form: yield(r, w) して ensure 相当で両方 close */
    RESULT r = korb_block_yield(c, slots + 4, block, def_env, &slots[1], 2, cself);
    korb_io_close_rep(korb_io_rep(c, slots[1]));
    korb_io_close_rep(korb_io_rep(c, slots[2]));
    return r;
}

/* IO#write(*args) → total bytes written. */
static RESULT korb_m_io_write(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_WRITE(c, slots, self);
    size_t nb = 0;
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        const RESULT r = korb_io_emit(c, slots, VALUE_SLICE_GET(a, i), rep, &nb);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(LONG2FIX((intptr_t)nb));
}
/* IO#print(*args) → nil. */
/* Is the receiver's #write something other than the builtin?  CRuby routes
 * IO#puts through #write, which is what lets `def io.write` (or a subclass)
 * capture the output; the builtin case writes straight to the descriptor. */
static bool korb_io_write_redefined(CTX *c, VALUE io) {
    VALUE def = KORB_NIL;
    const struct korb_method *const m =
        korb_class_find_method(korb_dispatch_class(c, io), korb_intern(c->vm, "write", 5), &def);
    return m != NULL && !(m->kind == KORB_METHOD_CFUNC && m->rfn == korb_m_io_write);
}
/* The String value of a global ($, / $\), or NULL when unset. */
static VALUE korb_io_sep_global(CTX *c, const char *name, uint32_t len) {
    const VALUE v = korb_const_get(c->vm, korb_intern(c->vm, name, len));
    return KORB_STRING_P(v) ? v : KORB_NIL;
}
static RESULT korb_m_io_print(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    const bool via_write = korb_io_write_redefined(c, VALUE_REF_GET(self));
    if (!via_write) {
        if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
        KORB_IO_NEED_WRITE(c, slots, self);
    }
    KorbIORep mem = KORB_IO_MEM_SINK;
    KorbIORep *const out = via_write ? &mem : rep;
    size_t nb = 0;
    RESULT r = RESULT_OK(KORB_NIL);
    if (VALUE_SLICE_LEN(a) == 0) {                     /* no args → the last line read ($_) */
        slots[0] = korb_const_get(c->vm, korb_intern(c->vm, "$_", 2));
        if (slots[0] != KORB_NIL) r = korb_io_emit(c, slots + 1, slots[0], out, &nb);
    }
    for (uint32_t i = 0; r.state == KORB_NORMAL && i < VALUE_SLICE_LEN(a); i++) {
        if (i > 0) {                                   /* $, separates the fields */
            slots[0] = korb_io_sep_global(c, "$,", 2);
            if (slots[0] != KORB_NIL) { r = korb_io_emit(c, slots + 1, slots[0], out, &nb); if (r.state != KORB_NORMAL) break; }
        }
        r = korb_io_emit(c, slots, VALUE_SLICE_GET(a, i), out, &nb);
        if (UNLIKELY(r.state != KORB_NORMAL)) break;
    }
    if (r.state == KORB_NORMAL) {                      /* $\ terminates the record */
        slots[0] = korb_io_sep_global(c, "$\\", 2);
        if (slots[0] != KORB_NIL) r = korb_io_emit(c, slots + 1, slots[0], out, &nb);
    }
    if (!via_write) return r.state == KORB_NORMAL ? RESULT_OK(KORB_NIL) : r;
    if (UNLIKELY(r.state != KORB_NORMAL)) { free(mem.wbuf); return r; }
    if (mem.wlen == 0) { free(mem.wbuf); return RESULT_OK(KORB_NIL); }
    slots[0] = VALUE_REF_GET(self);                    /* recv */
    RESULT sr = korb_str_new(c, slots + 1, mem.wbuf, (uint32_t)mem.wlen);
    free(mem.wbuf);
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    slots[1] = sr.value;
    CHECK(korb_send(c, slots + 2, korb_intern(c->vm, "write", 5), 0, 1));
    return RESULT_OK(KORB_NIL);
}
/* IO#<<(obj) → self. */
static RESULT korb_m_io_lshift(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_WRITE(c, slots, self);
    size_t nb = 0;
    const RESULT r = korb_io_emit(c, slots, VALUE_SLICE_GET(a, 0), rep, &nb);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(VALUE_REF_GET(self));
}
/* IO#puts(*args) → nil. */
static RESULT korb_m_io_puts(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    const uint32_t n = VALUE_SLICE_LEN(a);
    if (korb_io_write_redefined(c, VALUE_REF_GET(self))) {   /* render into memory, hand the text to #write */
        KorbIORep mem = KORB_IO_MEM_SINK;
        if (n == 0) (void)korb_io_wr(&mem, "\n", 1);
        else for (uint32_t i = 0; i < n; i++) {
            RESULT pr = korb_puts_one_to(c, slots, VALUE_SLICE_GET(a, i), &mem);
            if (UNLIKELY(pr.state != KORB_NORMAL)) { free(mem.wbuf); return pr; }
        }
        if (mem.wlen == 0) { free(mem.wbuf); return RESULT_OK(KORB_NIL); }   /* e.g. puts([]) writes nothing at all */
        slots[0] = VALUE_REF_GET(self);                      /* recv */
        RESULT sr = korb_str_new(c, slots + 1, mem.wbuf ? mem.wbuf : "", (uint32_t)mem.wlen);
        free(mem.wbuf);
        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        slots[1] = sr.value;
        CHECK(korb_send(c, slots + 2, korb_intern(c->vm, "write", 5), 0, 1));
        return RESULT_OK(KORB_NIL);
    }
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_WRITE(c, slots, self);
    if (n == 0) { KORB_IO_WR(c, slots, rep, "\n", 1); return RESULT_OK(KORB_NIL); }
    for (uint32_t i = 0; i < n; i++)
        CHECK(korb_puts_one_to(c, slots, VALUE_SLICE_GET(a, i), rep));
    return RESULT_OK(KORB_NIL);
}
/* IO#read([length[, outbuf]]) → `length` bytes (nil at EOF), or the whole rest.
 * With `outbuf` the data replaces that String's contents and the buffer itself is
 * returned; it is cleared (and nil returned) when the read hits EOF. */
static RESULT korb_m_io_read(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_READ(c, slots, self);
    const uint32_t na = VALUE_SLICE_LEN(a);
    bool bounded = false;
    intptr_t want = 0;
    if (na >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        const VALUE lv = VALUE_SLICE_GET(a, 0);
        if (FIXNUM_P(lv)) want = FIX2LONG(lv);
        else if (!korb_to_index(lv, &want)) {
            if (!KORB_OBJECT_P(lv) || !korb_responds_to(c, lv, korb_intern(c->vm, "to_int", 6)))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_coerce_name(c, lv));
            slots[0] = lv;
            const RESULT ir = korb_send(c, slots + 1, korb_intern(c->vm, "to_int", 6), 0, 0);
            if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
            if (!FIXNUM_P(ir.value))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_coerce_name(c, lv));
            want = FIX2LONG(ir.value);
        }
        if (UNLIKELY(want < 0))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative length %ld given", (long)want);
        bounded = true;
    }
    /* the output buffer is checked (and its #to_str run) before any read, so a
     * frozen buffer raises even when nothing would be read */
    slots[0] = (na >= 2) ? VALUE_SLICE_GET(a, 1) : KORB_NIL;
    if (slots[0] != KORB_NIL) {
        if (!KORB_STRING_P(slots[0])) {
            if (!KORB_OBJECT_P(slots[0]) || !korb_responds_to(c, slots[0], korb_intern(c->vm, "to_str", 6)))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, slots[0]));
            slots[1] = slots[0];
            const RESULT sr = korb_send(c, slots + 2, korb_intern(c->vm, "to_str", 6), 0, 0);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (!KORB_STRING_P(sr.value))
                return korb_raise(c, slots + 1, KORB_E_TYPE, 0, "no implicit conversion into String");
            slots[0] = sr.value;
        }
        KORB_CHECK_FROZEN(c, slots + 1, slots[0]);
    }
    const VALUE_REF bufref = VALUE_REF_AT(&slots[0]);
    rep = korb_io_rep(c, VALUE_REF_GET(self));          /* the coercions above dispatch */
    uint32_t got = 0;
    RESULT r = bounded ? korb_io_read_bytes(c, slots + 1, rep, (uint32_t)want, &got)
                       : korb_io_read_all_bytes(c, slots + 1, rep);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    slots[1] = r.value;
    const bool eof = bounded && got == 0 && want > 0;
    /* a read with a size is always ASCII-8BIT (it counts bytes, not characters) */
    if (bounded || korb_io_is_binary(c, VALUE_REF_GET(self)))
        KORB_STR_ENC_SET(slots[1], KORB_ENC_BINARY);
    if (VALUE_REF_GET(bufref) == KORB_NIL)
        return eof ? RESULT_OK(KORB_NIL) : RESULT_OK(slots[1]);
    VAL2STR(VALUE_REF_GET(bufref))->len = 0;            /* replace the buffer's contents */
    if (!eof) CHECK(korb_str_append_str(c, slots + 2, bufref, VALUE_REF_AT(&slots[1])));
    /* a bounded read leaves the buffer's own encoding alone, and so does hitting
     * EOF; an unbounded read takes the encoding of what it read (ruby-lang #20416) */
    if (!bounded && !eof) KORB_STR_ENC_SET(VALUE_REF_GET(bufref), KORB_STR_ENC(slots[1]));
    return eof ? RESULT_OK(KORB_NIL) : RESULT_OK(VALUE_REF_GET(bufref));
}
/* Read one record: bytes up to and including the separator held in `sepref`
 * (nil = slurp to EOF), at most `la->limit` bytes when that is >= 0.  Paragraph
 * mode matches "\n\n" and then swallows the run of newlines that follows, so the
 * next read starts at the next paragraph.  nil at EOF.
 *
 * Both the separator and the accumulating result are read back through their
 * slots on every pass: filling the buffer and growing the String are GC points,
 * and a cached `char *` into either would go stale under a moving collector. */
static RESULT
korb_io_read_sep(CTX *c, VALUE *slots, KorbIORep *rep, VALUE_REF sepref, const struct korb_line_args *la)
{
    RESULT err = RESULT_OK(KORB_NIL);
    if (la->paragraph) {                               /* skip a leading run of blank lines */
        for (;;) {
            const uint32_t avail = korb_io_fill_p(c, slots, rep, &err);
            if (UNLIKELY(err.state != KORB_NORMAL)) return err;
            if (avail == 0) break;
            uint32_t i = 0;
            while (i < avail && rep->rbuf[rep->rpos + i] == '\n') i++;
            rep->rpos += i;
            if (i < avail) break;
        }
    }
    if (korb_io_fill_p(c, slots, rep, &err) == 0)
        return UNLIKELY(err.state != KORB_NORMAL) ? err : RESULT_OK(KORB_NIL);
    if (la->limit == 0) return korb_str_new(c, slots, "", 0);
    slots[0] = (VALUE)korb_str_alloc(c, slots, 0);
    const VALUE_REF sref = VALUE_REF_AT(&slots[0]);
    uint32_t len = 0;
    bool hit_sep = false;
    for (;;) {
        const uint32_t avail = korb_io_fill_p(c, slots + 1, rep, &err);
        if (UNLIKELY(err.state != KORB_NORMAL)) return err;
        if (avail == 0) break;
        uint32_t take = avail;
        if (la->limit >= 0 && len + take > (uint32_t)la->limit) take = (uint32_t)la->limit - len;
        KorbString *s = korb_str_ensure(c, slots + 1, sref, len + take);   /* may GC; rep is libc-stable */
        memcpy(korb_strbuf_data(s->buf) + len, rep->rbuf + rep->rpos, take);
        rep->rpos += take;
        len += take;
        s->len = len;
        if (!la->slurp) {
            const KorbString *const sp = VAL2STR(VALUE_REF_GET(sepref));
            const uint32_t sl = sp->len;
            if (len >= sl) {
                /* a match may straddle the chunk boundary, so start (sl-1) bytes
                 * back into what was already accumulated */
                const char *const data = korb_strbuf_data(VAL2STR(VALUE_REF_GET(sref))->buf);
                const uint32_t from = (len - take >= sl - 1) ? (len - take) - (sl - 1) : 0;
                const char *const hit = memmem(data + from, len - from, korb_strbuf_data(sp->buf), sl);
                if (hit) {
                    const uint32_t end = (uint32_t)(hit - data) + sl;
                    rep->rpos -= len - end;            /* hand the overshoot back to the buffer */
                    len = end;
                    hit_sep = true;
                    break;
                }
            }
        }
        if (la->limit >= 0 && len >= (uint32_t)la->limit) break;
    }
    if (hit_sep && la->paragraph) {                    /* swallow the blank lines after the record */
        for (;;) {
            const uint32_t avail = korb_io_fill_p(c, slots + 1, rep, &err);
            if (UNLIKELY(err.state != KORB_NORMAL)) return err;
            if (avail == 0) break;
            uint32_t i = 0;
            while (i < avail && rep->rbuf[rep->rpos + i] == '\n') i++;
            rep->rpos += i;
            if (i < avail) break;
        }
    }
    if (la->chomp && !la->slurp) {                     /* drop the separator we just matched */
        const uint32_t sl = VAL2STR(VALUE_REF_GET(sepref))->len;
        const char *const data = korb_strbuf_data(VAL2STR(VALUE_REF_GET(sref))->buf);
        if (la->paragraph) { while (len > 0 && data[len - 1] == '\n') len--; }
        else if (len >= sl && !memcmp(data + len - sl, korb_strbuf_data(VAL2STR(VALUE_REF_GET(sepref))->buf), sl)) {
            len -= sl;
            if (sl == 1 && data[len] == '\n' && len > 0 && data[len - 1] == '\r') len--;   /* "\r\n" */
        }
    }
    KorbString *const s = VAL2STR(VALUE_REF_GET(sref));
    s->len = len;
    korb_strbuf_data(s->buf)[len] = '\0';
    return RESULT_OK(VALUE_REF_GET(sref));
}

/* IO#gets(sep = $/, limit = nil, chomp: false) → the next record, nil at EOF. */
static RESULT korb_m_io_gets(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_READ(c, slots, self);
    struct korb_line_args la;
    CHECK(korb_io_line_args(c, slots, a, 0, &la));      /* separator → slots[0] */
    rep = korb_io_rep(c, VALUE_REF_GET(self));          /* the parse may dispatch #to_str/#to_int */
    const RESULT r = korb_io_read_sep(c, slots + 1, rep, VALUE_REF_AT(&slots[0]), &la);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    /* IO#lineno counts the records #gets handed out; $. mirrors it and $_ holds
     * the last line (nil once the stream is exhausted).  Re-fetch the rep: the
     * read may have GC'd, and it lives in a libc allocation the IO points at. */
    if (r.value != KORB_NIL) {
        const uint32_t ln = ++korb_io_rep(c, VALUE_REF_GET(self))->lineno;
        korb_const_define(c, korb_intern(c->vm, "$.", 2), LONG2FIX((intptr_t)ln));
    }
    korb_const_define(c, korb_intern(c->vm, "$_", 2), r.value);
    return r;
}
/* IO#lineno / IO#lineno= — the #gets counter (not affected by other reads). */
static RESULT korb_m_io_lineno(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    return RESULT_OK(LONG2FIX((intptr_t)rep->lineno));
}
static RESULT korb_m_io_lineno_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    const VALUE v = VALUE_SLICE_GET(a, 0);
    intptr_t n;
    if (FIXNUM_P(v)) n = FIX2LONG(v);
    else if (KORB_FLOAT_P(v)) n = (intptr_t)korb_float_val(v);
    else if (!korb_to_index(v, &n))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_type_name(v));
    rep->lineno = (uint32_t)(n < 0 ? 0 : n);
    return RESULT_OK(v);
}
/* IO#readlines / IO#each_line — the remaining records, same arguments as #gets. */
static RESULT korb_m_io_readlines(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_READ(c, slots, self);
    struct korb_line_args la;
    CHECK(korb_io_line_args(c, slots, a, 0, &la));      /* separator → slots[0] */
    if (UNLIKELY(la.limit == 0))                        /* would yield "" forever */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid limit: 0 for readlines");
    const VALUE_REF sepref = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 16));
    const VALUE_REF arr = VALUE_REF_AT(&slots[1]);
    for (;;) {
        rep = korb_io_rep(c, VALUE_REF_GET(self));
        slots[2] = UNWRAP(korb_io_read_sep(c, slots + 2, rep, sepref, &la));
        if (slots[2] == KORB_NIL) break;
        CHECK(korb_ary_push_val(c, slots + 3, arr, slots[2]));
    }
    return RESULT_OK(VALUE_REF_GET(arr));
}
static RESULT korb_m_io_each_line(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                  struct Node *block, VALUE *def_env, VALUE *captured_self) {
    KorbIORep *rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_READ(c, slots, self);
    struct korb_line_args la;
    CHECK(korb_io_line_args(c, slots, a, 0, &la));      /* separator → slots[0] */
    if (UNLIKELY(la.limit == 0))                        /* would yield "" forever */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid limit: 0 for each_line");
    const VALUE_REF sepref = VALUE_REF_AT(&slots[0]);
    if (block == NULL) {   /* no block → an Enumerator over the remaining records */
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 16));
        const VALUE_REF arr = VALUE_REF_AT(&slots[1]);
        for (;;) {
            rep = korb_io_rep(c, VALUE_REF_GET(self));
            slots[2] = UNWRAP(korb_io_read_sep(c, slots + 2, rep, sepref, &la));
            if (slots[2] == KORB_NIL) break;
            CHECK(korb_ary_push_val(c, slots + 3, arr, slots[2]));
        }
        return korb_enum_new(c, slots + 2, VALUE_REF_GET(arr), KORB_NIL);
    }
    RESULT rr = RESULT_OK(KORB_NIL);
    for (;;) {
        /* the rep pointer stays valid across the yield (libc-allocated); a block
           that closes the stream just makes the next read report EOF. */
        rep = korb_io_rep(c, VALUE_REF_GET(self));
        slots[1] = UNWRAP(korb_io_read_sep(c, slots + 1, rep, sepref, &la));
        if (slots[1] == KORB_NIL) break;
        korb_io_rep(c, VALUE_REF_GET(self))->lineno++;
        rr = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, captured_self);
        if (rr.state != KORB_NORMAL) break;
    }
    if (rr.state != KORB_NORMAL) return rr;
    return RESULT_OK(VALUE_REF_GET(self));
}
/* IO#close — fclose (never on the std streams); marks the slot closed. */
/* IO#close_read / IO#close_write — drop one direction of a duplex stream.  koruby
 * has a single descriptor per IO, so the direction bits in @__io_mode are cleared
 * and the descriptor is closed once neither direction is left (which is what a
 * pipe end or a one-way File does on the first call). */
static RESULT korb_m_io_close(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);
static RESULT korb_io_close_half(CTX *c, VALUE *slots, VALUE_REF self, int keep_bit, const char *what) {
    const int rw = korb_io_rw(c, VALUE_REF_GET(self));
    const int drop_bit = (keep_bit == 1) ? 2 : 1;
    if (!(rw & drop_bit)) {
        /* Already dropped (or never had it): a repeat call is a no-op, but a
         * stream that never had this direction at all is "non-duplex". */
        const KorbIORep *const rep0 = korb_io_rep(c, VALUE_REF_GET(self));
        if (rw == 0 || !korb_io_open_p(rep0)) return RESULT_OK(KORB_NIL);
        return korb_raise(c, slots, KORB_E_IOERROR, 0, "closing non-duplex IO for %s", what);
    }
    {
        KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
        if (korb_io_open_p(rep)) {
            if (drop_bit == 2) (void)korb_io_flush_rep(rep);   /* flush pending output first */
            /* On a socket (a duplex IO.popen, a socketpair, …) closing one
             * direction must reach the peer, or the other end never sees EOF. */
            int sty; socklen_t stl = sizeof sty;
            if (getsockopt(rep->fd, SOL_SOCKET, SO_TYPE, &sty, &stl) == 0)
                (void)shutdown(rep->fd, drop_bit == 2 ? SHUT_WR : SHUT_RD);
        }
    }
    const int left = rw & ~drop_bit;
    CHECK(korb_ivar_set(c, slots, self, ID2SYM(korb_io_mode_mid(c)), LONG2FIX(left)));
    if (left == 0) return korb_m_io_close(c, slots, self, VALUE_SLICE_MAKE(NULL, 0));
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_io_close_read(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; return korb_io_close_half(c, slots, self, 2, "reading");
}
static RESULT korb_m_io_close_write(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; return korb_io_close_half(c, slots, self, 1, "writing");
}
static RESULT korb_m_io_close(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const VALUE idxv = korb_ivar_get(c, VALUE_REF_GET(self), ID2SYM(korb_io_fp_mid(c)));
    if (FIXNUM_P(idxv)) {
        const intptr_t idx = FIX2LONG(idxv);
        KorbIORep *const rep = ((uint32_t)idx < c->vm->io_cnt) ? c->vm->io_reps[idx] : NULL;
        if (idx >= 3 && korb_io_open_p(rep)) {   /* 0/1/2 = std streams, never close */
            /* A popen'd stream needs pclose so the child is reaped; keep its
               exit status for $? the way IO.popen's caller expects. */
            const VALUE pidv = korb_ivar_get(c, VALUE_REF_GET(self), ID2SYM(korb_intern(c->vm, "@__io_pid", 9)));
            korb_io_close_rep(rep);
            if (FIXNUM_P(pidv)) {          /* popen'd: reap the child and publish $? */
                int raw = 0;
                const pid_t got = waitpid((pid_t)FIX2LONG(pidv), &raw, 0);
                if (got > 0) {
                    slots[0] = korb_const_get(c->vm, korb_intern(c->vm, "Process", 7));
                    if (slots[0] != KORB_NIL) {
                        slots[1] = LONG2FIX(got);
                        slots[2] = LONG2FIX(raw);
                        const RESULT sr = korb_send(c, slots + 3, korb_intern(c->vm, "__mkstatus", 10), 0, 2);
                        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                        korb_const_define(c, korb_intern(c->vm, "$?", 2), sr.value);
                    }
                }
            }
        }
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_m_io_closed_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    return RESULT_OK(korb_io_open_p(korb_io_rep(c, VALUE_REF_GET(self))) ? KORB_FALSE : KORB_TRUE);
}
static RESULT korb_m_io_flush(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (rep) (void)korb_io_flush_rep(rep);
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_io_eof_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return RESULT_OK(KORB_TRUE);
    RESULT err = RESULT_OK(KORB_NIL);
    const uint32_t avail = korb_io_fill_p(c, slots, rep, &err);   /* blocks (by parking) until data or EOF */
    if (UNLIKELY(err.state != KORB_NORMAL)) return err;
    return RESULT_OK(avail == 0 ? KORB_TRUE : KORB_FALSE);
}
/* IO#sync → whether every write goes straight to the descriptor. */
static RESULT korb_m_io_sync(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    const KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    return RESULT_OK((rep && rep->sync) ? KORB_TRUE : KORB_FALSE);
}
/* IO#sync=(bool) — turning it on drains what is already buffered, so the
 * setting takes effect for output written before the assignment too. */
static RESULT korb_m_io_sync_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    const VALUE v = VALUE_SLICE_LEN(a) >= 1 ? VALUE_SLICE_GET(a, 0) : KORB_TRUE;
    if (rep) {
        rep->sync = KORB_TRUTHY(v) ? 1 : 0;
        if (rep->sync) (void)korb_io_flush_rep(rep);
    }
    return RESULT_OK(v);
}
/* IO#getc → the next UTF-8 character, or nil at EOF. */
static RESULT korb_m_io_getc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_READ(c, slots, self);
    RESULT gerr = RESULT_OK(KORB_NIL);
    const int b0 = korb_io_getb_p(c, slots, rep, &gerr);   /* cbuf below is a C local: safe across a park */
    if (UNLIKELY(gerr.state != KORB_NORMAL)) return gerr;
    if (b0 < 0) return RESULT_OK(KORB_NIL);
    char cbuf[8]; cbuf[0] = (char)b0;
    const unsigned char u = (unsigned char)b0;
    uint32_t cl = u < 0x80 ? 1 : u >= 0xF0 ? 4 : u >= 0xE0 ? 3 : u >= 0xC0 ? 2 : 1;
    for (uint32_t k = 1; k < cl; k++) { const int b = korb_io_getb_p(c, slots, rep, &gerr); if (b < 0) { cl = k; break; } cbuf[k] = (char)b; }
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
/* IO#getbyte — one byte as an Integer, nil at EOF. */
static RESULT korb_m_io_getbyte(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_READ(c, slots, self);
    RESULT gerr = RESULT_OK(KORB_NIL);
    const int b = korb_io_getb_p(c, slots, rep, &gerr);
    if (UNLIKELY(gerr.state != KORB_NORMAL)) return gerr;
    return RESULT_OK(b < 0 ? KORB_NIL : LONG2FIX(b));
}

/* IO#sysseek — lseek(2) without touching the buffers; CRuby raises when there
 * are unread buffered bytes rather than silently desyncing them. */
static RESULT korb_m_io_sysseek(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    intptr_t off;
    CHECK(korb_io_arg_int(c, slots, VALUE_SLICE_GET(a, 0), &off));
    const int whence = (VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1)))
                         ? (int)FIX2LONG(VALUE_SLICE_GET(a, 1)) : SEEK_SET;
    if (rep->rpos < rep->rlen) return korb_raise(c, slots, KORB_E_IOERROR, 0, "sysseek for buffered IO");
    korb_io_flush_rep(rep);
    const off_t r = lseek(rep->fd, (off_t)off, whence);
    if (r < 0) return korb_raise_errno(c, slots, errno, "sysseek", "");
    return RESULT_OK(LONG2FIX((intptr_t)r));
}

/* IO#fsync / #fdatasync — flush our buffer, then ask the kernel. */
static RESULT korb_m_io_fsync(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    korb_io_flush_rep(rep);
    if (rep->fd >= 0 && fsync(rep->fd) != 0 && errno != EINVAL)   /* EINVAL: pipes/ttys can't sync */
        return korb_raise_errno(c, slots, errno, "fsync", "");
    return RESULT_OK(LONG2FIX(0));
}

/* IO#advise(advice, offset = 0, len = 0) — posix_fadvise; the advice symbols
 * follow CRuby's names.  Advice is advisory: unknown offsets never raise. */
static RESULT korb_m_io_advise(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    const VALUE av = VALUE_SLICE_GET(a, 0);
    if (!SYMBOL_P(av)) return korb_raise(c, slots, KORB_E_TYPE, 0, "advice must be a Symbol");
    const char *const nm = korb_sym_name(c->vm, SYM2ID(av));
    int adv;
    if      (!strcmp(nm, "normal"))     adv = POSIX_FADV_NORMAL;
    else if (!strcmp(nm, "sequential")) adv = POSIX_FADV_SEQUENTIAL;
    else if (!strcmp(nm, "random"))     adv = POSIX_FADV_RANDOM;
    else if (!strcmp(nm, "willneed"))   adv = POSIX_FADV_WILLNEED;
    else if (!strcmp(nm, "dontneed"))   adv = POSIX_FADV_DONTNEED;
    else if (!strcmp(nm, "noreuse"))    adv = POSIX_FADV_NOREUSE;
    else return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "unsupported advice: %s", nm);
    off_t off = 0, len = 0;
    if (VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1))) off = (off_t)FIX2LONG(VALUE_SLICE_GET(a, 1));
    if (VALUE_SLICE_LEN(a) >= 3 && FIXNUM_P(VALUE_SLICE_GET(a, 2))) len = (off_t)FIX2LONG(VALUE_SLICE_GET(a, 2));
    if (rep->fd >= 0) (void)posix_fadvise(rep->fd, off, len, adv);
    return RESULT_OK(KORB_NIL);
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
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    KORB_IO_NEED_READ(c, slots, self);
    KORB_IO_NEED_READ(c, slots, self);
    const intptr_t off = FIXNUM_P(VALUE_SLICE_GET(a, 0)) ? FIX2LONG(VALUE_SLICE_GET(a, 0)) : 0;
    const int whence = (VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1))) ? (int)FIX2LONG(VALUE_SLICE_GET(a, 1)) : SEEK_SET;
    if (korb_io_seek_rep(rep, (off_t)off, whence) < 0) return korb_raise_errno(c, slots, errno, "seek", "");
    return RESULT_OK(LONG2FIX(0));
}
static RESULT korb_m_io_pos(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    return RESULT_OK(LONG2FIX(rep ? (intptr_t)korb_io_tell_rep(rep) : 0));
}
static RESULT korb_m_io_pos_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots;
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (rep && FIXNUM_P(VALUE_SLICE_GET(a, 0))) (void)korb_io_seek_rep(rep, (off_t)FIX2LONG(VALUE_SLICE_GET(a, 0)), SEEK_SET);
    return RESULT_OK(VALUE_SLICE_GET(a, 0));
}
static RESULT korb_m_io_rewind(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (rep) (void)korb_io_seek_rep(rep, 0, SEEK_SET);
    return RESULT_OK(LONG2FIX(0));
}
/* IO#each_char { |ch| } — yield each UTF-8 character (of the rest); no block → Enumerator. */
static RESULT korb_m_io_each_char(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                  struct Node *block, VALUE *def_env, VALUE *captured_self) {
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (!korb_io_open_p(rep)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
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

/* IO#binmode? — the prelude's encoding accessors need to see the 'b' flag,
 * which lives in a non-@ internal ivar. */
static RESULT korb_m_io_binmode_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    return RESULT_OK(korb_io_is_binary(c, VALUE_REF_GET(self)) ? KORB_TRUE : KORB_FALSE);
}

/* IO#__io_writable? — the encoding accessors need the write permission bit,
 * which lives in the same non-@ internal ivar as the mode. */
static RESULT korb_m_io_writable_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    return RESULT_OK((korb_io_rw(c, VALUE_REF_GET(self)) & 2) ? KORB_TRUE : KORB_FALSE);
}

/* A Ruby mode string ("r", "w+", "ab", …) → open(2) flags.  false = not a mode
 * string koruby understands. */
static bool korb_io_mode_to_flags(const char *mode, int *out) {
    const bool plus = strchr(mode, '+') != NULL;
    switch (mode[0]) {
      case 'r': *out = plus ? O_RDWR : O_RDONLY;                          return true;
      case 'w': *out = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;    return true;
      case 'a': *out = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;   return true;
      default:  return false;
    }
}

/* The rw bits korb_io_make wants: 1 read, 2 write, 3 both. */
static int korb_io_mode_rw(const char *mode) {
    const bool plus = strchr(mode, '+') != NULL;
    if (mode[0] == 'r') return plus ? 3 : 1;
    return plus ? 3 : 2;
}

/* open(2) flags for a fopen(3)-style mode string ("r", "w+", "ab", …). */
static int korb_io_open_flags(const char *mode) {
    const bool plus = strchr(mode, '+') != NULL;
    switch (mode[0]) {
      case 'w': return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
      case 'a': return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
      default:  return plus ? O_RDWR : O_RDONLY;
    }
}

/* Read a mode argument (String or Integer O_* flags) into a fopen(3)-style
 * string.  Returns false for a mode koruby rejects. */
static bool korb_io_mode_arg(VALUE mv, char *mode, size_t cap) {
    if (FIXNUM_P(mv)) {
        const int fl = (int)FIX2LONG(mv);
        const int acc = fl & 3;
        const char *m = acc == 1 ? ((fl & O_APPEND) ? "a" : "w") : acc == 2 ? "r+" : "r";
        if (strlen(m) >= cap) return false;
        strcpy(mode, m);
        return true;
    }
    if (!KORB_STRING_P(mv)) return false;
    uint32_t ml; const char *m = korb_str_cstr_len(mv, &ml);
    /* "r:utf-8:euc-jp" — only the part before the first ':' is the access mode;
       the encoding suffix is resolved by the prelude from @__io_modestr. */
    for (uint32_t i = 0; i < ml; i++) if (m[i] == ':') { ml = i; break; }
    if (ml == 0 || ml >= cap) return false;
    memcpy(mode, m, ml); mode[ml] = '\0';
    return strchr("rwa", mode[0]) != NULL;
}

/* korb_io_mode_arg for an arbitrary object: a String / Integer is used as is,
 * anything else goes through #to_str then #to_int (CRuby's rb_io_extract_modeenc).
 * *mv is updated to the coerced value (the caller may want to record it). */
static RESULT korb_io_mode_coerce(CTX *c, VALUE *slots, VALUE *mv, char *mode, size_t cap) {
#define KORB_IO_BAD_MODE(sl)                                                              \
    (KORB_STRING_P(*mv)                                                                   \
       ? korb_raise(c, (sl), KORB_E_ARGUMENT, 0, "invalid access mode %.*s",               \
                    (int)VAL2STR(*mv)->len, korb_strbuf_data(VAL2STR(*mv)->buf))          \
       : korb_raise(c, (sl), KORB_E_ARGUMENT, 0, "invalid access mode"))
    if (LIKELY(korb_io_mode_arg(*mv, mode, cap))) return RESULT_OK(KORB_TRUE);
    if (KORB_STRING_P(*mv) || FIXNUM_P(*mv)) return KORB_IO_BAD_MODE(slots);
    if (KORB_OBJECT_P(*mv)) {
        VALUE recv = *mv;
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (korb_responds_to_coerce_p(c, slots, &recv, to_str)) {
            slots[0] = recv;
            const RESULT r = korb_send(c, slots + 1, to_str, 0, 0);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            *mv = r.value;
            if (KORB_STRING_P(*mv) && korb_io_mode_arg(*mv, mode, cap)) return RESULT_OK(KORB_TRUE);
            return KORB_IO_BAD_MODE(slots + 1);
        }
        recv = *mv;
        const uint32_t to_int = korb_intern(c->vm, "to_int", 6);
        if (korb_responds_to_coerce_p(c, slots, &recv, to_int)) {
            slots[0] = recv;
            const RESULT r = korb_send(c, slots + 1, to_int, 0, 0);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            *mv = r.value;
            if (FIXNUM_P(*mv) && korb_io_mode_arg(*mv, mode, cap)) return RESULT_OK(KORB_TRUE);
            return KORB_IO_BAD_MODE(slots + 1);
        }
    }
    /* neither a mode string/Integer nor convertible to one */
    return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(*mv));
#undef KORB_IO_BAD_MODE
}

/* IO#reopen(path, mode) / IO#reopen(io) — make this stream refer to another
 * file or descriptor, keeping the same IO object identity. */
static RESULT korb_m_io_reopen(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (UNLIKELY(!korb_io_open_p(rep))) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    const uint32_t na_ = VALUE_SLICE_LEN(a);
    if (UNLIKELY(na_ < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    if (UNLIKELY(na_ > 2 && !KORB_HASH_P(VALUE_SLICE_GET(a, na_ - 1))))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 1..2)", na_);
    slots[0] = VALUE_SLICE_GET(a, 0);
    /* A non-String, non-IO target that answers #to_path is a path (CRuby tries
       #to_path before #to_io), so convert it up front. */
    if (!KORB_STRING_P(slots[0]) && KORB_OBJECT_P(slots[0]) && korb_io_rep(c, slots[0]) == NULL &&
        korb_responds_to(c, slots[0], korb_intern(c->vm, "to_path", 7))) {
        const RESULT pr = korb_send(c, slots + 1, korb_intern(c->vm, "to_path", 7), 0, 0);
        if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
        if (KORB_STRING_P(pr.value)) slots[0] = pr.value;
    }
    if (KORB_STRING_P(slots[0])) {
        /* Take the path onto the stack: the flush below can park (and so GC),
           which would move the String's bytes out from under a borrow. */
        uint32_t pl; const char *const pbytes = korb_str_cstr_len(slots[0], &pl);
        char path[4096];
        if (UNLIKELY(pl >= sizeof path)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "path too long");
        memcpy(path, pbytes, pl); path[pl] = '\0';
        /* No mode given → keep the receiver's own mode: reopening a write-only
           IO onto a path must still write (and create), not switch to reading. */
        char mode[16] = "r";
        const VALUE ms_ = korb_ivar_get(c, VALUE_REF_GET(self), ID2SYM(korb_intern(c->vm, "@__io_modestr", 13)));
        if (KORB_STRING_P(ms_)) (void)korb_io_mode_arg(ms_, mode, sizeof mode);
        else { const int rw_ = korb_io_rw(c, VALUE_REF_GET(self));
               if (rw_ == 2) strcpy(mode, "w"); else if (rw_ == 3) strcpy(mode, "r+"); }
        if (na_ >= 2 && VALUE_SLICE_GET(a, 1) != KORB_NIL && !KORB_HASH_P(VALUE_SLICE_GET(a, 1)) &&
            !korb_io_mode_arg(VALUE_SLICE_GET(a, 1), mode, sizeof mode))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid access mode");
        /* Re-point the same descriptor at another file, keeping the fd number
           (a reopened $stdout must stay fd 1).  dup2 does that atomically. */
        const int nfd = open(path, korb_io_open_flags(mode), 0666);
        if (nfd < 0) return korb_raise_errno(c, slots, errno, "rb_sysopen", path);
        (void)korb_io_flush_rep(rep);
        if (dup2(nfd, rep->fd) < 0) { const int e = errno; close(nfd); return korb_raise_errno(c, slots, e, "dup2", path); }
        close(nfd);
        korb_io_drop_rbuf(rep);
        (void)fcntl(rep->fd, F_SETFD, FD_CLOEXEC);
        /* the reopened stream takes the new mode's direction: `f.reopen(p, "r")`
           on a write-only IO is readable afterwards */
        CHECK(korb_ivar_set(c, slots + 1, self, ID2SYM(korb_io_mode_mid(c)), LONG2FIX(korb_io_mode_rw(mode))));
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, mode, (uint32_t)strlen(mode)));
        CHECK(korb_ivar_set(c, slots + 2, self, ID2SYM(korb_intern(c->vm, "@__io_modestr", 13)), slots[1]));
        return RESULT_OK(VALUE_REF_GET(self));
    }
    /* Not a path: an IO, or anything that converts to one via #to_io. */
    if (!KORB_OBJECT_P(slots[0]) || korb_io_rep(c, slots[0]) == NULL) {
        const uint32_t to_io = korb_intern(c->vm, "to_io", 5);
        char cls[192];                                            /* the class name, captured before dispatch */
        korb_io_class_name(c, slots[0], cls, sizeof cls);
        VALUE recv = slots[0];
        if (UNLIKELY(!(KORB_OBJECT_P(recv) && korb_responds_to_coerce_p(c, slots + 1, &recv, to_io))))
            return korb_raise(c, slots + 1, KORB_E_TYPE, 0, "no implicit conversion of %s into String", cls);
        slots[0] = recv;
        const RESULT cr = korb_send(c, slots + 1, to_io, 0, 0);   /* receiver at slots[0] */
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        slots[0] = cr.value;
        if (UNLIKELY(!KORB_OBJECT_P(slots[0]) || korb_io_rep(c, slots[0]) == NULL))
            return korb_raise(c, slots + 1, KORB_E_TYPE, 0, "can't convert %s to IO (%s#to_io gives %s)",
                              cls, cls, korb_type_name(slots[0]));
    }
    KorbIORep *const other = korb_io_rep(c, slots[0]);
    if (UNLIKELY(!korb_io_open_p(other))) return korb_raise(c, slots + 1, KORB_E_IOERROR, 0, "closed stream");
    (void)korb_io_flush_rep(other);
    (void)korb_io_flush_rep(rep);
    if (dup2(other->fd, rep->fd) < 0) return korb_raise_errno(c, slots, errno, "dup2", "");
    korb_io_drop_rbuf(rep);
    return RESULT_OK(VALUE_REF_GET(self));
}

/* IO#dup / IO#clone — a genuine descriptor dup (dup(2)), not just a copy of the
 * wrapper object: mspec's output_to_fd saves a stream this way and restores it
 * with #reopen, which only works if the saved IO owns its own fd. */
static RESULT korb_m_io_dup(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (UNLIKELY(!korb_io_open_p(rep))) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    (void)korb_io_flush_rep(rep);
    /* Take the access mode from the descriptor itself: the std streams are built
       without the rw ivar. */
    const int fl = fcntl(rep->fd, F_GETFL);
    const int acc = (fl < 0) ? O_RDONLY : (fl & O_ACCMODE);
    const int rw = acc == O_WRONLY ? 2 : acc == O_RDWR ? 3 : 1;
    const int fd = dup(rep->fd);
    if (fd < 0) return korb_raise_errno(c, slots, errno, "dup", "");
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    return korb_io_make(c, slots, korb_class_obj_of(c, VALUE_REF_GET(self)), fd, rw);
}

/* IO#binmode — switch to byte semantics (reads produce ASCII-8BIT). */
static RESULT korb_m_io_binmode(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    CHECK(korb_ivar_set(c, slots, self, ID2SYM(korb_io_bin_mid(c)), KORB_TRUE));
    return RESULT_OK(VALUE_REF_GET(self));
}

/* IO#ungetc / IO#ungetbyte — push one byte back onto the read buffer. */
static RESULT korb_m_io_ungetc(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (UNLIKELY(!korb_io_open_p(rep))) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    const VALUE v = VALUE_SLICE_GET(a, 0);
    if (FIXNUM_P(v)) {
        const char b = (char)(FIX2LONG(v) & 0xff);
        if (!korb_io_unget(rep, &b, 1)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "ungetc failed");
    } else if (KORB_STRING_P(v)) {
        uint32_t n; const char *const p = korb_str_cstr_len(v, &n);
        if (n == 0) return RESULT_OK(KORB_NIL);
        /* the whole run goes back — the pushback buffer is ours, so unlike a
           FILE* this is not limited to a single byte */
        if (!korb_io_unget(rep, p, n)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "ungetc failed");
    } else if (v == KORB_NIL) return RESULT_OK(KORB_NIL);
    else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(v));
    return RESULT_OK(KORB_NIL);
}

/* IO#syswrite / IO#sysread — unbuffered write(2)/read(2) on the descriptor. */
static RESULT korb_m_io_syswrite(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (UNLIKELY(!korb_io_open_p(rep))) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    slots[0] = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(slots[0])) {                       /* #to_s first (may GC) */
        const RESULT r = korb_send(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;
    }
    (void)korb_io_flush_rep(rep);
    ssize_t w;
    for (;;) {
        uint32_t n; const char *const p = korb_str_cstr_len(slots[0], &n);   /* re-derive after any park */
        w = write(rep->fd, p, n);
        if (w >= 0) break;
        if (errno == EINTR) continue;
        if (korb_io_would_block(errno) && rep->nonblk) {
            const RESULT pr = korb_io_park(c, slots + 1, rep, POLLOUT);
            if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
            continue;
        }
        return korb_raise_errno(c, slots, errno, "syswrite", "");
    }
    return RESULT_OK(LONG2FIX((intptr_t)w));
}
static RESULT korb_m_io_sysread(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (UNLIKELY(!korb_io_open_p(rep))) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    intptr_t want = 4096;
    if (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) want = FIX2LONG(VALUE_SLICE_GET(a, 0));
    if (want < 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative length");
    slots[0] = (VALUE)korb_str_alloc(c, slots, (uint32_t)want);
    ssize_t r;
    for (;;) {
        /* re-derive each round: the park below may GC and move the String.
           read(2) itself allocates nothing, so the borrow is safe up to it. */
        KorbString *const sb = VAL2STR(slots[0]);
        r = read(rep->fd, korb_strbuf_data(sb->buf), (size_t)want);
        if (r >= 0) break;
        if (errno == EINTR) continue;
        /* sysread blocks in CRuby; on a descriptor koruby made non-blocking we
           reproduce that by parking rather than surfacing EAGAIN. */
        if (korb_io_would_block(errno) && rep->nonblk) {
            const RESULT pr = korb_io_park(c, slots + 1, rep, POLLIN);
            if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
            continue;
        }
        return korb_raise_errno(c, slots + 1, errno, "sysread", "");
    }
    if (r == 0 && want > 0) return korb_raise(c, slots + 1, KORB_E_IOERROR, 0, "end of file reached");
    KorbString *const s = VAL2STR(slots[0]);
    s->len = (uint32_t)r;
    korb_strbuf_data(s->buf)[r] = '\0';
    KORB_STR_ENC_SET(slots[0], KORB_ENC_BINARY);
    return RESULT_OK(slots[0]);
}

/* IO#__init_fd(fd, mode) — wire an already-allocated IO (or subclass) to a
 * descriptor.  IO.new is a C singleton, so a subclass that needs a real
 * #initialize (the socket classes) allocates and calls this instead. */
static RESULT korb_m_io_init_fd(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    intptr_t fdv;
    CHECK(korb_io_arg_int(c, slots, VALUE_SLICE_GET(a, 0), &fdv));
    const int fd = (int)fdv;
    char mode[16] = "r";
    if (VALUE_SLICE_LEN(a) >= 2 && VALUE_SLICE_GET(a, 1) != KORB_NIL &&
        !korb_io_mode_arg(VALUE_SLICE_GET(a, 1), mode, sizeof mode))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid access mode");
    if (fcntl(fd, F_GETFD) < 0) return korb_raise_errno(c, slots, errno, "", "");
    const uint32_t idx = korb_io_register(c->vm, fd, false);
    /* A socket is the case where blocking really costs: it can stay unreadable
       for as long as the peer likes.  Detect it from the descriptor rather than
       from the class, so IO.new(socket_fd) benefits too. */
    { int sty; socklen_t stl = sizeof sty;
      if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &sty, &stl) == 0) {
          korb_io_set_nonblock(c->vm->io_reps[idx]);
          c->vm->io_reps[idx]->sync = 1;   /* BasicSocket is sync in CRuby: a buffered
                                              write the peer never sees is a deadlock */
      } }
    CHECK(korb_ivar_set(c, slots, self, ID2SYM(korb_io_fp_mid(c)), LONG2FIX((intptr_t)idx)));
    CHECK(korb_ivar_set(c, slots, self, ID2SYM(korb_io_mode_mid(c)), LONG2FIX(korb_io_mode_rw(mode))));
    if (strchr(mode, 'b'))
        CHECK(korb_ivar_set(c, slots, self, ID2SYM(korb_io_bin_mid(c)), KORB_TRUE));
    return RESULT_OK(VALUE_REF_GET(self));
}

/* IO#pid — the child's pid for a popen'd stream, else nil. */
static RESULT korb_m_io_pid(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    return RESULT_OK(korb_ivar_get(c, VALUE_REF_GET(self), ID2SYM(korb_intern(c->vm, "@__io_pid", 9))));
}

static RESULT korb_m_io_close_on_exec_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (UNLIKELY(!korb_io_open_p(rep))) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    const int fl = fcntl(korb_io_fd(c, VALUE_REF_GET(self)), F_GETFD);
    if (fl < 0) return korb_raise_errno(c, slots, errno, "fcntl", "");
    return RESULT_OK((fl & FD_CLOEXEC) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_io_close_on_exec_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (UNLIKELY(!korb_io_open_p(rep))) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    const int fd = rep->fd;
    int fl = fcntl(fd, F_GETFD);
    if (fl < 0) return korb_raise_errno(c, slots, errno, "fcntl", "");
    if (KORB_TRUTHY(VALUE_SLICE_GET(a, 0))) fl |= FD_CLOEXEC; else fl &= ~FD_CLOEXEC;
    if (fcntl(fd, F_SETFD, fl) < 0) return korb_raise_errno(c, slots, errno, "fcntl", "");
    return RESULT_OK(VALUE_SLICE_GET(a, 0));
}

/* Encoding.default_external / default_internal are captured when the stream is
 * created: changing them afterwards must not affect an already-open IO (the
 * prelude resolves the pair from these). */
static RESULT korb_io_capture_default_internal(CTX *c, VALUE *slots, VALUE_REF io) {
    slots[0] = korb_const_get(c->vm, korb_intern(c->vm, "Encoding", 8));
    if (!KORB_CLASS_P(slots[0])) return RESULT_OK(KORB_NIL);
    slots[1] = slots[0];
    const RESULT r = korb_send(c, slots + 2, korb_intern(c->vm, "default_internal", 16), 0, 0);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    slots[2] = r.value;
    CHECK(korb_ivar_set(c, slots + 3, io, ID2SYM(korb_intern(c->vm, "@__int_enc0", 11)), slots[2]));
    slots[2] = slots[1];
    const RESULT er = korb_send(c, slots + 3, korb_intern(c->vm, "default_external", 16), 0, 0);
    if (UNLIKELY(er.state != KORB_NORMAL)) return er;
    slots[2] = er.value;
    return korb_ivar_set(c, slots + 3, io, ID2SYM(korb_intern(c->vm, "@__ext_enc0", 11)), slots[2]);
}

/* Hand back up to `want` bytes already sitting in the read buffer as a String,
 * optionally replacing the caller's buffer.  Assumes the buffer is non-empty. */
static RESULT korb_io_take_buffered(CTX *c, VALUE *slots, KorbIORep *const rep,
                                    uint32_t want, VALUE bufv) {
    const uint32_t avail = rep->rlen - rep->rpos;
    const uint32_t take = want < avail ? want : avail;
    const RESULT sr = korb_str_new(c, slots, rep->rbuf + rep->rpos, take);   /* copies before any GC */
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    rep->rpos += take;
    KORB_STR_ENC_SET(sr.value, KORB_ENC_BINARY);
    if (bufv == KORB_NIL) return sr;
    if (UNLIKELY(!KORB_STRING_P(bufv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    slots[0] = sr.value;
    slots[1] = bufv;
    return korb_m_str_replace(c, slots + 2, VALUE_REF_AT(&slots[1]), VALUE_SLICE_MAKE(&slots[0], 1));
}

/* IO#readpartial(maxlen[, buf]) — at least one byte, but only what is already
 * there: buffered bytes win, otherwise one read(2) (parking rather than
 * stalling on a stream koruby made non-blocking).  EOF raises EOFError. */
static RESULT korb_m_io_readpartial(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (UNLIKELY(!korb_io_open_p(rep))) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    intptr_t want;
    CHECK(korb_io_arg_int(c, slots, VALUE_SLICE_GET(a, 0), &want));
    rep = korb_io_rep(c, VALUE_REF_GET(self));                    /* #to_int may have GC'd */
    if (UNLIKELY(want < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative length %ld given", (long)want);
    const VALUE bufv = VALUE_SLICE_LEN(a) >= 2 ? VALUE_SLICE_GET(a, 1) : KORB_NIL;
    if (want == 0) {
        if (bufv == KORB_NIL) return korb_str_new(c, slots, "", 0);
        slots[0] = UNWRAP(korb_str_new(c, slots, "", 0));
        slots[1] = bufv;
        return korb_m_str_replace(c, slots + 2, VALUE_REF_AT(&slots[1]), VALUE_SLICE_MAKE(&slots[0], 1));
    }
    RESULT err = RESULT_OK(KORB_NIL);
    const uint32_t avail = korb_io_fill_p(c, slots, rep, &err);   /* may park → may GC */
    if (UNLIKELY(err.state != KORB_NORMAL)) return err;
    rep = korb_io_rep(c, VALUE_REF_GET(self));                    /* re-fetch after a possible GC */
    if (avail == 0) return korb_io_raise_eof(c, slots);
    return korb_io_take_buffered(c, slots, rep, (uint32_t)want, bufv);
}

/* Raise IO::EAGAINWaitReadable / ::EAGAINWaitWritable (both are Errno::EAGAIN
 * subclasses that include IO::WaitReadable / IO::WaitWritable). */
static RESULT korb_io_raise_wait(CTX *c, VALUE *slots, bool readable) {
    const char *const nm = readable ? "EAGAINWaitReadable" : "EAGAINWaitWritable";
    const uint32_t sym = korb_intern(c->vm, nm, (uint32_t)strlen(nm));
    const VALUE iocls = korb_const_get(c->vm, korb_intern(c->vm, "IO", 2));
    VALUE cls = KORB_NIL;
    if (KORB_CLASS_P(iocls)) {
        const uint32_t ci = korb_const_index_owned(c->vm, sym, iocls);
        if (ci != UINT32_MAX) cls = c->vm->const_vals[ci];
    }
    if (!KORB_CLASS_P(cls)) cls = korb_const_get(c->vm, sym);   /* flat namespace fallback */
    if (!KORB_CLASS_P(cls)) return korb_raise_errno(c, slots, EAGAIN, readable ? "read" : "write", "");
    slots[0] = cls;
    RESULT r = korb_raise(c, slots + 1, KORB_E_RUNTIME, 0, "Resource temporarily unavailable");
    if (KORB_CLASS_P(slots[0]) && KORB_EXC_P(r.value))
        ARO_STORE(c, VAL2EXC(r.value), (VALUE *)(uintptr_t)&VAL2EXC(r.value)->exc_class, slots[0]);
    return r;
}

/* IO#read_nonblock(maxlen[, buf][, exception: true]) — never parks: EAGAIN
 * surfaces as IO::EAGAINWaitReadable (or :wait_readable). */
static RESULT korb_m_io_read_nonblock(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (UNLIKELY(!korb_io_open_p(rep))) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..3)");
    intptr_t want;
    CHECK(korb_io_arg_int(c, slots, VALUE_SLICE_GET(a, 0), &want));
    rep = korb_io_rep(c, VALUE_REF_GET(self));                    /* #to_int may have GC'd */
    if (UNLIKELY(want < 0)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative length %ld given", (long)want);
    bool exc = true;
    VALUE bufv = KORB_NIL;
    for (uint32_t i = 1; i < VALUE_SLICE_LEN(a); i++) {
        const VALUE v = VALUE_SLICE_GET(a, i);
        if (KORB_HASH_P(v)) {
            const KorbHash *const h = VAL2HASH(v);
            const int32_t hx = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "exception", 9)));
            if (hx >= 0) exc = KORB_TRUTHY(korb_items_data(h->items)[2 * hx + 1]);
        } else bufv = v;
    }
    if (want == 0) return bufv == KORB_NIL ? korb_str_new(c, slots, "", 0) : RESULT_OK(bufv);
    if (rep->rpos < rep->rlen) return korb_io_take_buffered(c, slots, rep, (uint32_t)want, bufv);
    if (rep->eof) {
        if (!exc) return RESULT_OK(KORB_NIL);
        return korb_io_raise_eof(c, slots);
    }
    char stackbuf[8192];
    const size_t cap = (size_t)want < sizeof stackbuf ? (size_t)want : sizeof stackbuf;
    ssize_t n;
    do { n = read(rep->fd, stackbuf, cap); } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (korb_io_would_block(errno))
            return exc ? korb_io_raise_wait(c, slots, true)
                       : RESULT_OK(ID2SYM(korb_intern(c->vm, "wait_readable", 13)));
        return korb_raise_errno(c, slots, errno, "read", "");
    }
    if (n == 0) {
        rep->eof = 1;
        if (!exc) return RESULT_OK(KORB_NIL);
        return korb_io_raise_eof(c, slots);
    }
    const RESULT sr = korb_str_new(c, slots, stackbuf, (uint32_t)n);
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    KORB_STR_ENC_SET(sr.value, KORB_ENC_BINARY);
    if (bufv == KORB_NIL) return sr;
    if (UNLIKELY(!KORB_STRING_P(bufv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    slots[0] = sr.value;
    slots[1] = bufv;
    return korb_m_str_replace(c, slots + 2, VALUE_REF_AT(&slots[1]), VALUE_SLICE_MAKE(&slots[0], 1));
}

/* IO#write_nonblock(string[, exception: true]) */
static RESULT korb_m_io_write_nonblock(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *const rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (UNLIKELY(!korb_io_open_p(rep))) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    bool exc = true;
    slots[0] = VALUE_SLICE_GET(a, 0);
    for (uint32_t i = 1; i < VALUE_SLICE_LEN(a); i++) {
        const VALUE v = VALUE_SLICE_GET(a, i);
        if (KORB_HASH_P(v)) {
            const KorbHash *const h = VAL2HASH(v);
            const int32_t hx = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "exception", 9)));
            if (hx >= 0) exc = KORB_TRUTHY(korb_items_data(h->items)[2 * hx + 1]);
        }
    }
    if (!KORB_STRING_P(slots[0])) {
        const RESULT sr = korb_send(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0);
        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        slots[0] = sr.value;
        if (UNLIKELY(!KORB_STRING_P(slots[0]))) return korb_raise(c, slots + 1, KORB_E_TYPE, 0, "no implicit conversion into String");
    }
    korb_io_flush_rep(rep);                         /* keep ordering with buffered writes */
    uint32_t n; const char *const p = korb_str_cstr_len(slots[0], &n);
    ssize_t w;
    do { w = write(rep->fd, p, n); } while (w < 0 && errno == EINTR);
    if (w < 0) {
        if (korb_io_would_block(errno))
            return exc ? korb_io_raise_wait(c, slots + 1, false)
                       : RESULT_OK(ID2SYM(korb_intern(c->vm, "wait_writable", 13)));
        return korb_raise_errno(c, slots + 1, errno, "write", "");
    }
    return RESULT_OK(LONG2FIX((intptr_t)w));
}

/* IO#pread(maxlen, offset[, buf]) — read at an absolute offset without moving
 * the file position.  Buffered writes are drained first so the descriptor holds
 * everything the program has written; the read buffer is untouched (pread does
 * not consume the sequential stream). */
static RESULT korb_m_io_pread(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (UNLIKELY(!korb_io_open_p(rep))) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 2))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 2..3)", VALUE_SLICE_LEN(a));
    intptr_t want, off;
    CHECK(korb_io_arg_int(c, slots, VALUE_SLICE_GET(a, 0), &want));
    CHECK(korb_io_arg_int(c, slots, VALUE_SLICE_GET(a, 1), &off));
    rep = korb_io_rep(c, VALUE_REF_GET(self));                    /* #to_int may have GC'd */
    if (want < 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative string size (or size too big)");
    korb_io_flush_rep(rep);
    const VALUE bufv = VALUE_SLICE_LEN(a) >= 3 ? VALUE_SLICE_GET(a, 2) : KORB_NIL;
    if (want == 0)          /* CRuby reads nothing and leaves the buffer alone */
        return bufv == KORB_NIL ? korb_str_new(c, slots, "", 0) : RESULT_OK(bufv);
    slots[0] = (VALUE)korb_str_alloc(c, slots, (uint32_t)want);   /* scratch, rooted */
    ssize_t r;
    do {
        KorbString *const sb = VAL2STR(slots[0]);                 /* re-derive: nothing allocs in between */
        r = pread(rep->fd, korb_strbuf_data(sb->buf), (size_t)want, (off_t)off);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return korb_raise_errno(c, slots + 1, errno, "pread", "");
    if (r == 0 && want > 0) return korb_io_raise_eof(c, slots + 1);
    { KorbString *const s = VAL2STR(slots[0]);
      s->len = (uint32_t)r;
      korb_strbuf_data(s->buf)[r] = '\0'; }
    if (bufv == KORB_NIL) return RESULT_OK(slots[0]);
    if (UNLIKELY(!KORB_STRING_P(bufv))) return korb_raise(c, slots + 1, KORB_E_TYPE, 0, "no implicit conversion into String");
    slots[1] = bufv;                                              /* replace into the caller's buffer, keeping its encoding */
    return korb_m_str_replace(c, slots + 2, VALUE_REF_AT(&slots[1]), VALUE_SLICE_MAKE(&slots[0], 1));
}

/* IO#pwrite(string, offset) — write at an absolute offset without moving the
 * file position. */
static RESULT korb_m_io_pwrite(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    KorbIORep *rep = korb_io_rep(c, VALUE_REF_GET(self));
    if (UNLIKELY(!korb_io_open_p(rep))) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 2))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 2)", VALUE_SLICE_LEN(a));
    slots[0] = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(slots[0])) {                               /* CRuby writes obj.to_s */
        const RESULT sr = korb_send(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0);
        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        slots[0] = sr.value;
        if (UNLIKELY(!KORB_STRING_P(slots[0]))) return korb_raise(c, slots + 1, KORB_E_TYPE, 0, "no implicit conversion into String");
    }
    intptr_t off;
    CHECK(korb_io_arg_int(c, slots + 1, VALUE_SLICE_GET(a, 1), &off));   /* slots[0] holds the String */
    rep = korb_io_rep(c, VALUE_REF_GET(self));                           /* #to_int / #to_s may have GC'd */
    korb_io_flush_rep(rep);
    uint32_t n; const char *const p = korb_str_cstr_len(slots[0], &n);   /* no alloc before the write */
    ssize_t w;
    do { w = pwrite(rep->fd, p, n, (off_t)off); } while (w < 0 && errno == EINTR);
    if (w < 0) return korb_raise_errno(c, slots + 1, errno, "pwrite", "");
    return RESULT_OK(LONG2FIX((intptr_t)w));
}

/* IO.sysopen(path, mode = "r", perm = 0666) → the raw fd, unwrapped. */
static RESULT korb_m_io_s_sysopen(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv))) {                    /* #to_path then #to_str */
        CHECK(korb_file_path_arg(c, slots, &pv));
        slots[0] = pv;                                     /* root the coerced path */
    }
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
    int flags = O_RDONLY;
    if (VALUE_SLICE_LEN(a) >= 2 && VALUE_SLICE_GET(a, 1) != KORB_NIL) {
        const VALUE mv = VALUE_SLICE_GET(a, 1);
        if (FIXNUM_P(mv)) flags = (int)FIX2LONG(mv);
        else {
            char mode[16];
            if (!korb_io_mode_arg(mv, mode, sizeof mode) || !korb_io_mode_to_flags(mode, &flags))
                return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid access mode");
        }
    }
    mode_t perm = 0666;
    if (VALUE_SLICE_LEN(a) >= 3 && FIXNUM_P(VALUE_SLICE_GET(a, 2))) perm = (mode_t)FIX2LONG(VALUE_SLICE_GET(a, 2));
    const int fd = open(path, flags, perm);
    if (fd < 0) return korb_raise_errno(c, slots, errno, "rb_sysopen", path);
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    return RESULT_OK(LONG2FIX(fd));
}

/* IO.new(fd, mode = "r") / IO.for_fd — wrap an already-open descriptor.  A
 * trailing options Hash (autoclose:, encoding:, …) is accepted and ignored. */
static RESULT korb_m_io_s_new_fd(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    uint32_t n = VALUE_SLICE_LEN(a);
    VALUE opts = KORB_NIL;
    if (n >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, n - 1))) { opts = VALUE_SLICE_GET(a, n - 1); n--; }
    if (UNLIKELY(n < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    if (UNLIKELY(n > 2)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 1..2)", n);
    intptr_t fdv;
    CHECK(korb_io_arg_int(c, slots, VALUE_SLICE_GET(a, 0), &fdv));
    const int fd = (int)fdv;   /* a negative or stale fd falls out of the fcntl check below as EBADF */
    /* The mode may come from the positional argument or from `mode:`; a
     * positional nil defers to the option (CRuby accepts both spellings). */
    VALUE modev = (n >= 2) ? VALUE_SLICE_GET(a, 1) : KORB_NIL;
    if (modev == KORB_NIL && KORB_HASH_P(opts)) {
        const int32_t mi = korb_hash_find(VAL2HASH(opts), ID2SYM(korb_intern(c->vm, "mode", 4)));
        if (mi >= 0) modev = korb_items_data(VAL2HASH(opts)->items)[2 * mi + 1];
    }
    /* slots[0] = opts, slots[1] = the (possibly coerced) mode value — both
     * rooted from here on, since the coercion and korb_io_make below allocate. */
    slots[0] = opts;
    slots[1] = modev;
    char mode[16] = "r";
    if (slots[1] != KORB_NIL)
        CHECK(korb_io_mode_coerce(c, slots + 2, &slots[1], mode, sizeof mode));
    if (KORB_HASH_P(slots[0])) {                                   /* binmode: true → 'b' */
        const int32_t bi = korb_hash_find(VAL2HASH(slots[0]), ID2SYM(korb_intern(c->vm, "binmode", 7)));
        if (bi >= 0 && KORB_TRUTHY(korb_items_data(VAL2HASH(slots[0])->items)[2 * bi + 1]) &&
            strchr(mode, 'b') == NULL && strlen(mode) + 1 < sizeof mode)
            strcat(mode, "b");
    }
    if (fcntl(fd, F_GETFD) < 0) return korb_raise_errno(c, slots + 2, errno, "", "");
    /* the IO wraps the caller's descriptor; nothing is duplicated */
    const bool binary = strchr(mode, 'b') != NULL;
    slots[2] = UNWRAP(korb_io_make(c, slots + 2, VALUE_REF_GET(self), fd, korb_io_mode_rw(mode)));
    VALUE_REF nio = VALUE_REF_AT(&slots[2]);
    if (binary)
        CHECK(korb_ivar_set(c, slots + 3, nio, ID2SYM(korb_io_bin_mid(c)), KORB_TRUE));
    if (KORB_STRING_P(slots[1]))
        CHECK(korb_ivar_set(c, slots + 3, nio, ID2SYM(korb_intern(c->vm, "@__io_modestr", 13)), slots[1]));
    CHECK(korb_io_capture_default_internal(c, slots + 3, nio));
    if (KORB_HASH_P(slots[0])) {                                   /* encoding: / autoclose: → prelude */
        slots[3] = VALUE_REF_GET(nio);
        slots[4] = slots[0];
        CHECK(korb_send(c, slots + 5, korb_intern(c->vm, "__apply_open_opts", 17), 0, 1));
    }
    return RESULT_OK(VALUE_REF_GET(nio));
}

/* File.open(path, mode = "r") [ { |io| ... } ] — with a block, yields the IO and
 * closes it after (returning the block value); without, returns the IO. */
/* open(2) that never stalls the scheduler on a FIFO.  Opening a FIFO blocks
 * until the other end appears — inside one native thread that would freeze
 * every green thread, including the one that was about to open the other end.
 * So FIFOs are opened O_NONBLOCK: reads succeed immediately; writes get ENXIO
 * until a reader exists, which we turn into park-and-retry.  The flag is
 * cleared again after the open (the rep layer manages its own nonblocking). */
static int korb_open_no_stall(CTX *c, VALUE *slots, const char *path, int flags, mode_t perm, RESULT *perr) {
    perr->state = KORB_NORMAL;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISFIFO(st.st_mode))
        return open(path, flags, perm);                    /* not a FIFO — plain open */
    for (;;) {
        const int fd = open(path, flags | O_NONBLOCK, perm);
        if (fd >= 0) {
            if ((flags & O_ACCMODE) == O_RDONLY) {
                /* A blocking read-open waits for a writer.  POLLHUP without
                 * POLLIN means "no writer attached (yet)" — park and re-poll
                 * until a writer opens (POLLHUP clears) or data arrives. */
                for (;;) {
                    struct pollfd pf; pf.fd = fd; pf.events = POLLIN; pf.revents = 0;
                    ssize_t ready = 0;
                    const RESULT pr = korb_blop_poll_wait(c, slots, &pf, 1, 0.02, &ready);
                    if (UNLIKELY(pr.state != KORB_NORMAL)) { *perr = pr; close(fd); return -1; }
                    if (!(ready > 0 && (pf.revents & POLLHUP) && !(pf.revents & POLLIN))) break;
                }
            }
            const int fl = fcntl(fd, F_GETFL);
            if (fl >= 0) (void)fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
            return fd;
        }
        if (errno != ENXIO) return -1;                     /* writer with no reader yet → wait */
        struct pollfd pf; pf.fd = -1; pf.events = 0; pf.revents = 0;   /* fd -1 is ignored: pure timed park */
        ssize_t ready = 0;
        const RESULT pr = korb_blop_poll_wait(c, slots, &pf, 1, 0.02, &ready);
        if (UNLIKELY(pr.state != KORB_NORMAL)) { *perr = pr; return -1; }
    }
}

static RESULT korb_m_io_s_new_fd(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* fwd (below) */
static RESULT korb_m_file_open(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                               struct Node *block, VALUE *def_env, VALUE *captured_self) {
    VALUE pv = VALUE_SLICE_GET(a, 0);
    /* File.new(fd[, mode]) wraps an existing descriptor, exactly like IO.new. */
    if (FIXNUM_P(pv)) return korb_m_io_s_new_fd(c, slots, self, a);
    if (UNLIKELY(!KORB_STRING_P(pv))) {                    /* #to_path then #to_str */
        CHECK(korb_file_path_arg(c, slots, &pv));
        slots[0] = pv;                                     /* root the coerced path */
    }
    /* A trailing Hash is keyword options (mode:, flags:, encoding: …), not a
     * positional argument; only 3 positionals are allowed. */
    uint32_t npos = VALUE_SLICE_LEN(a);
    VALUE fopts = KORB_NIL;
    if (npos >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, npos - 1))) { fopts = VALUE_SLICE_GET(a, npos - 1); npos--; }
    if (UNLIKELY(npos > 3))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 1..3)", npos);
    int extra_flags = 0;
    if (KORB_HASH_P(fopts)) {                              /* flags: OR'd into the open(2) flags */
        const int32_t fi = korb_hash_find(VAL2HASH(fopts), ID2SYM(korb_intern(c->vm, "flags", 5)));
        if (fi >= 0) {
            const VALUE fv = korb_items_data(VAL2HASH(fopts)->items)[2 * fi + 1];
            if (FIXNUM_P(fv)) extra_flags = (int)FIX2LONG(fv);
        }
        if (npos < 2) {                                    /* mode: when no positional mode */
            const int32_t mi = korb_hash_find(VAL2HASH(fopts), ID2SYM(korb_intern(c->vm, "mode", 4)));
            if (mi >= 0) {
                const VALUE mv = korb_items_data(VAL2HASH(fopts)->items)[2 * mi + 1];
                if (mv != KORB_NIL) { slots[1] = mv; a = VALUE_SLICE_MAKE(a.p, 2); ((VALUE *)a.p)[1] = mv; npos = 2; }
            }
        }
    }
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
    int rw = 0; bool binary = false; int fd;
    if (VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1))) {   /* integer O_* flags → open(2) */
        const int fl = (int)FIX2LONG(VALUE_SLICE_GET(a, 1));
        const mode_t perm = (VALUE_SLICE_LEN(a) >= 3 && FIXNUM_P(VALUE_SLICE_GET(a, 2))) ? (mode_t)FIX2LONG(VALUE_SLICE_GET(a, 2)) : 0666;
        const int acc = fl & 3;   /* O_RDONLY=0 / O_WRONLY=1 / O_RDWR=2 */
        rw = acc == 1 ? 2 : acc == 2 ? 3 : 1;
        char pbuf[4096];                                   /* stack copy: the open may park → GC moves the String */
        snprintf(pbuf, sizeof pbuf, "%.*s", (int)plen, path);
        RESULT operr;
        fd = korb_open_no_stall(c, slots, pbuf, fl | extra_flags, perm, &operr);
        if (UNLIKELY(operr.state != KORB_NORMAL)) return operr;
        if (fd < 0) return korb_raise_errno(c, slots, errno, "rb_sysopen", pbuf);
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    } else {
        char mode[8] = "r";
        if (VALUE_SLICE_LEN(a) >= 2 && KORB_STRING_P(VALUE_SLICE_GET(a, 1))) {
            uint32_t ml; const char *m = korb_str_cstr_len(VALUE_SLICE_GET(a, 1), &ml);
            /* "r:utf-8:euc-jp" — the encoding suffix is not part of the fopen
               mode; the prelude reads it back off @__io_modestr. */
            for (uint32_t i = 0; i < ml; i++) if (m[i] == ':') { ml = i; break; }
            if (ml > 0 && ml < sizeof(mode)) { memcpy(mode, m, ml); mode[ml] = '\0'; }
        }
        const char b = mode[0]; const bool plus = strchr(mode, '+') != NULL;   /* validate + derive rw bits */
        binary = strchr(mode, 'b') != NULL;                                     /* 'rb'/'wb'/… → byte-encoded reads */
        if (b == 'r') rw = plus ? 3 : 1;
        else if (b == 'w' || b == 'a') rw = plus ? 3 : 2;
        else return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid access mode %s", mode);
        char pbuf[4096];
        snprintf(pbuf, sizeof pbuf, "%.*s", (int)plen, path);
        RESULT operr;
        const mode_t perm2 = (npos >= 3 && FIXNUM_P(VALUE_SLICE_GET(a, 2))) ? (mode_t)FIX2LONG(VALUE_SLICE_GET(a, 2)) : 0666;
        fd = korb_open_no_stall(c, slots, pbuf, korb_io_open_flags(mode) | extra_flags, perm2, &operr);
        if (UNLIKELY(operr.state != KORB_NORMAL)) return operr;
        if (fd < 0) return korb_raise_errno(c, slots, errno, "rb_sysopen", pbuf);
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    slots[0] = UNWRAP(korb_io_make(c, slots, VALUE_REF_GET(self), fd, rw));   /* self = the File class */
    VALUE_REF io = VALUE_REF_AT(&slots[0]);
    if (binary)   /* remember binary mode → reads produce ASCII-8BIT strings */
        CHECK(korb_ivar_set(c, slots + 1, io, ID2SYM(korb_io_bin_mid(c)), KORB_TRUE));
    CHECK(korb_ivar_set(c, slots + 1, io, ID2SYM(korb_intern(c->vm, "@__io_path", 10)),
                        VALUE_SLICE_GET(a, 0)));   /* File#path / #to_path */
    if (VALUE_SLICE_LEN(a) >= 2 && KORB_STRING_P(VALUE_SLICE_GET(a, 1)))
        CHECK(korb_ivar_set(c, slots + 1, io, ID2SYM(korb_intern(c->vm, "@__io_modestr", 13)),
                            VALUE_SLICE_GET(a, 1)));
    CHECK(korb_io_capture_default_internal(c, slots + 1, io));
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
    /* index 0/1/2 = the std streams (never closed).  stderr is sync so a
       diagnostic is on the descriptor before anything that follows it. */
    /* A write to a pipe whose reader has gone must surface as Errno::EPIPE, not
       kill the interpreter — same as CRuby, which also runs with SIGPIPE
       ignored.  Without this, `koruby ... | head` dies on signal 13. */
    signal(SIGPIPE, SIG_IGN);
    korb_io_register(vm, STDIN_FILENO, false);
    /* On a terminal stdout is sync so a prompt written without a newline is
       visible before the read that follows it; piped output stays buffered. */
    korb_io_register(vm, STDOUT_FILENO, isatty(STDOUT_FILENO) != 0);
    korb_io_register(vm, STDERR_FILENO, true);

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
    IOM("close_read", close_read, 0);   IOM("close_write", close_write, 0);
    IOM("stat", stat, 0);        IOM("fileno", fileno, 0);
    IOM("tty?", tty_p, 0);       IOM("isatty", tty_p, 0);
    IOM("truncate", truncate, 1);
    IOM("flush", flush, 0);      IOM("eof?", eof_p, 0);     IOM("eof", eof_p, 0);
    IOM("sync", sync, 0);        IOM("sync=", sync_set, 1);
    IOM("seek", seek, -1);       IOM("pos", pos, 0);        IOM("tell", pos, 0);
    IOM("pos=", pos_set, 1);     IOM("rewind", rewind, 0);
    IOB("each_char", each_char, 0);   IOM("getc", getc, 0);
    IOM("getbyte", getbyte, 0);
    IOM("sysseek", sysseek, -1);      IOM("fsync", fsync, 0);
    IOM("fdatasync", fsync, 0);       IOM("advise", advise, -1);
    IOM("readline", readline, -1);    IOM("readchar", readchar, 0);
    IOM("binmode?", binmode_p, 0);
    IOM("__io_writable?", writable_p, 0);
    IOM("reopen", reopen, -1);       IOM("pid", pid, 0);
    IOM("dup", dup, 0);              IOM("clone", dup, 0);
    IOM("__init_fd", init_fd, -1);
    IOM("binmode", binmode, 0);      IOM("ungetc", ungetc, 1);
    IOM("ungetbyte", ungetc, 1);
    IOM("syswrite", syswrite, 1);    IOM("sysread", sysread, -1);
    IOM("pread", pread, -1);         IOM("pwrite", pwrite, -1);
    IOM("readpartial", readpartial, -1);
    IOM("read_nonblock", read_nonblock, -1);
    IOM("write_nonblock", write_nonblock, -1);
    IOM("lineno", lineno, 0);        IOM("lineno=", lineno_set, 1);
    IOM("close_on_exec?", close_on_exec_p, 0);
    IOM("close_on_exec=", close_on_exec_set, 1);
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
    korb_class_def_cfn(c, io_sing, "sysopen",   korb_m_io_s_sysopen,   -1);
    korb_class_def_cfn(c, io_sing, "new",       korb_m_io_s_new_fd,    -1);
    korb_class_def_cfn(c, io_sing, "for_fd",    korb_m_io_s_new_fd,    -1);
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
        slots[3] = korb_obj_singleton(c, slots + 4, file_cls).value;
        /* File's singleton was built back in file.c, while File still inherited
         * from Object, so its metaclass still points at Object's singleton and
         * File.for_fd / File.sysopen / File.pipe would miss.  Re-parent the
         * metaclass to match the class re-parenting above. */
        slots[4] = korb_obj_singleton(c, slots + 5, korb_const_get(vm, korb_intern(vm, "IO", 2))).value;
        ARO_STORE(c, VAL2CLASS(slots[3]), (VALUE *)(uintptr_t)&VAL2CLASS(slots[3])->superclass, slots[4]);
        vm->method_serial++;
        const VALUE fsing = slots[3];
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
