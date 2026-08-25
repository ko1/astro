#include <pwd.h>
#include <fcntl.h>
/* koruby_precise — file.c: a minimal File class (POSIX path-string methods only,
 * no real I/O yet).  #included into korb_runtime.c's TU.  Enough to unblock the
 * mspec `fixture()` helper and the File path specs (expand_path / join / dirname
 * / basename / extname).  Paths are byte strings; '/' is the only separator. */
#include <unistd.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>   /* major() / minor() for File::Stat#dev_major etc. */
#include <dirent.h>
#include <glob.h>
#include <errno.h>


/* --- libc の差の吸収 ---------------------------------------------------
 * 「このプラットフォームは何か」ではなく「この libc に X はあるか」で書く。
 * そうすると WASI だけでなく musl / macOS の差も同じ仕組みで吸収される。 */
#ifndef PATH_MAX
#  define PATH_MAX 4096
#endif

/* 無い errno 名には番兵を与えて下の表を成立させる。X-macro のリストの中には
 * プリプロセッサ指令を書けないので、リストの手前で埋めるしかない。番兵は
 * 負値なのでプラットフォームが返すことはなく、逆引きは korb_errno_name の
 * ガードで飛ばす。Errno::<名前> 自体は (値 < 0 で) 定義されたままになる。 */
#ifndef EHOSTDOWN
#  define EHOSTDOWN (-1001)
#endif
#ifndef ENODATA
#  define ENODATA (-1002)
#endif
#ifndef ENOSR
#  define ENOSR (-1003)
#endif
#ifndef ENOSTR
#  define ENOSTR (-1004)
#endif
#ifndef EPFNOSUPPORT
#  define EPFNOSUPPORT (-1005)
#endif
#ifndef EREMOTE
#  define EREMOTE (-1006)
#endif
#ifndef ESHUTDOWN
#  define ESHUTDOWN (-1007)
#endif
#ifndef ESOCKTNOSUPPORT
#  define ESOCKTNOSUPPORT (-1008)
#endif
#ifndef ESRMNT
#  define ESRMNT (-1009)
#endif
#ifndef ETIME
#  define ETIME (-1010)
#endif
#ifndef ETOOMANYREFS
#  define ETOOMANYREFS (-1011)
#endif
#ifndef EUSERS
#  define EUSERS (-1012)
#endif
/* The Errno::* names koruby knows.  One list drives both directions: the
 * errno → class-name lookup used when raising, and the __errno_table builtin
 * the prelude uses to define the Errno constants.  Keeping them in step is why
 * this is a list and not two hand-written tables (they had drifted: the raise
 * side knew 12 names, the prelude defined 28, and the socket specs wanted
 * EAFNOSUPPORT and friends that neither had). */
#define KORB_ERRNO_LIST(X)                                                     \
    X(EPERM) X(ENOENT) X(ESRCH) X(EINTR) X(EIO) X(ENXIO) X(E2BIG) X(ENOEXEC)   \
    X(EBADF) X(ECHILD) X(EAGAIN) X(ENOMEM) X(EACCES) X(EFAULT) X(EBUSY)        \
    X(EEXIST) X(EXDEV) X(ENODEV) X(ENOTDIR) X(EISDIR) X(EINVAL) X(ENFILE)      \
    X(EMFILE) X(ENOTTY) X(ETXTBSY) X(EFBIG) X(ENOSPC) X(ESPIPE) X(EROFS)       \
    X(EMLINK) X(EPIPE) X(EDOM) X(ERANGE) X(EDEADLK) X(ENAMETOOLONG) X(ENOLCK)  \
    X(ENOSYS) X(ENOTEMPTY) X(ELOOP) X(ENOMSG) X(EIDRM) X(ENOLINK) X(EPROTO)    \
    X(EMULTIHOP) X(EBADMSG) X(EOVERFLOW) X(EILSEQ) X(EUSERS) X(ENOTSOCK)       \
    X(EDESTADDRREQ) X(EMSGSIZE) X(EPROTOTYPE) X(ENOPROTOOPT)                   \
    X(EPROTONOSUPPORT) X(ESOCKTNOSUPPORT) X(EOPNOTSUPP) X(EPFNOSUPPORT)        \
    X(EAFNOSUPPORT) X(EADDRINUSE) X(EADDRNOTAVAIL) X(ENETDOWN) X(ENETUNREACH)  \
    X(ENETRESET) X(ECONNABORTED) X(ECONNRESET) X(ENOBUFS) X(EISCONN)           \
    X(ENOTCONN) X(ESHUTDOWN) X(ETOOMANYREFS) X(ETIMEDOUT) X(ECONNREFUSED)      \
    X(EHOSTDOWN) X(EHOSTUNREACH) X(EALREADY) X(EINPROGRESS) X(ESTALE)          \
    X(EDQUOT) X(ECANCELED) X(ENOTSUP) X(ENODATA) X(ETIME) X(ENOSTR) X(ENOSR)   \
    X(EREMOTE) X(ESRMNT) X(EWOULDBLOCK)

static const struct { const char *name; int num; } korb_errno_tab[] = {
#define KORB_ERRNO_ENTRY(N) { #N, N },
    KORB_ERRNO_LIST(KORB_ERRNO_ENTRY)
#undef KORB_ERRNO_ENTRY
};

/* map an errno to its Errno::* constant name (NULL → generic).  Several names
 * share a number on Linux (EAGAIN/EWOULDBLOCK, ENOTSUP/EOPNOTSUPP); the first
 * listed wins, which is the one CRuby reports. */
static const char *korb_errno_name(int e) {
    for (size_t i = 0; i < sizeof korb_errno_tab / sizeof korb_errno_tab[0]; i++) {
        if (korb_errno_tab[i].num < 0) continue;                  /* この libc に無い名前 (番兵) */
        if (korb_errno_tab[i].num == e) return korb_errno_tab[i].name;
    }
    return NULL;
}

/* __strerror(num) → the platform's message for that errno (the default message
 * of an Errno::* exception raised without one). */
static RESULT korb_m_strerror(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE v = VALUE_SLICE_GET(a, 0);
    if (!FIXNUM_P(v)) return RESULT_OK(KORB_NIL);
    const char *const m = strerror((int)FIX2LONG(v));
    return korb_str_new(c, slots, m ? m : "", m ? (uint32_t)strlen(m) : 0);
}

/* __errno_table → { "ENOENT" => 2, ... }; the prelude turns it into the
 * Errno::* SystemCallError subclasses. */
static RESULT korb_m_errno_table(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; (void)a;
    slots[0] = UNWRAP(korb_hash_new(c, slots, 96));
    VALUE_REF h = VALUE_REF_AT(&slots[0]);
    for (size_t i = 0; i < sizeof korb_errno_tab / sizeof korb_errno_tab[0]; i++) {
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, korb_errno_tab[i].name,
                                       (uint32_t)strlen(korb_errno_tab[i].name)));
        CHECK(korb_hash_set(c, slots + 2, h, VALUE_REF_AT(&slots[1]), LONG2FIX(korb_errno_tab[i].num)));
    }
    return RESULT_OK(VALUE_REF_GET(h));
}
/* raise Errno::<errno> with CRuby's "<strerror> @ <func> - <path>" message; falls
 * back to SystemCallError/RuntimeError if the Errno class is absent. */
/* `func` NULL → CRuby's plain "message - path" shape (exec / kill / …); with a
 * func it is the "message @ func - path" shape the IO/File layer uses. */
static RESULT korb_raise_errno(CTX *c, VALUE *slots, int e, const char *func, const char *path) {
    char msg[4096];
    if (func) snprintf(msg, sizeof msg, "%s @ %s - %s", strerror(e), func, path);   /* format now: path is a movable-String interior ptr, korb_raise allocs */
    else      snprintf(msg, sizeof msg, "%s - %s", strerror(e), path);
    const char *cn = korb_errno_name(e);
    const VALUE cls = cn ? korb_const_get(c->vm, korb_intern(c->vm, cn, (uint32_t)strlen(cn))) : KORB_NIL;
    slots[0] = KORB_CLASS_P(cls) ? cls : KORB_NIL;
    RESULT r = korb_raise(c, slots + 1, KORB_E_RUNTIME, 0, "%s", msg);
    if (KORB_CLASS_P(slots[0]) && KORB_EXC_P(r.value))
        ARO_STORE(c, VAL2EXC(r.value), (VALUE *)(uintptr_t)&VAL2EXC(r.value)->exc_class, slots[0]);
    return r;
}

/* Coerce a path argument to a String the way CRuby does: a String passes
 * through, otherwise #to_path is tried (Pathname, and mspec's mock_to_path),
 * then #to_str.  The result is written back into the argument cell — that cell
 * is rooted, so callers can keep reading the path through the slice after this
 * (possibly GC-ing) dispatch.  `a` must be the caller's own argument slice.  */
static RESULT korb_path_coerce(CTX *c, VALUE *slots, VALUE_SLICE a, uint32_t idx) {
    VALUE v = VALUE_SLICE_GET(a, idx);
    if (LIKELY(KORB_STRING_P(v))) return RESULT_OK(v);
    const char *const tname = (v == KORB_NIL) ? "nil" : korb_type_name(v);   /* capture before any dispatch can move `v` */
    static const struct { const char *name; uint32_t len; } conv[] = { { "to_path", 7 }, { "to_str", 6 } };
    for (size_t i = 0; i < sizeof conv / sizeof conv[0] && !KORB_STRING_P(v); i++) {
        const uint32_t mid = korb_intern(c->vm, conv[i].name, conv[i].len);
        if (!korb_responds_to(c, v, mid)) continue;
        slots[0] = v;                              /* receiver, rooted across the dispatch */
        const RESULT r = korb_send_impl(c, slots + 1, mid, 0, 0, NULL, NULL, NULL);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        v = r.value;
    }
    if (UNLIKELY(!KORB_STRING_P(v)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", tname);
    VALUE_REF_SET(VALUE_SLICE_REF(a, idx), v);     /* keep the coerced String where the caller reads it */
    return RESULT_OK(v);
}

/* Coerce argument `idx` to a path String, propagating a raise. */
#define KORB_PATH_ARG(c, slots, a, idx, out)                                   \
    do {                                                                       \
        const RESULT _pr = korb_path_coerce((c), (slots), (a), (idx));         \
        if (UNLIKELY(_pr.state != KORB_NORMAL)) return _pr;                    \
        (out) = _pr.value;                                                     \
    } while (0)

/* Normalize an absolute path in `src` (length n) into `dst`: collapse "//", drop
 * "." segments, resolve ".." by popping, keep a single leading "/".  Returns the
 * normalized length.  `dst` must hold at least n+1 bytes. */
static size_t korb_path_normalize(const char *src, size_t n, char *dst) {
    /* segment offsets within dst (each segment is "/name"); a ".." pops the last. */
    size_t segs[1024]; uint32_t ns = 0;
    size_t d = 0;
    size_t i = 0;
    while (i < n) {
        while (i < n && src[i] == '/') i++;          /* skip separators */
        size_t s = i;
        while (i < n && src[i] != '/') i++;           /* one segment [s, i) */
        const size_t seglen = i - s;
        if (seglen == 0) continue;
        if (seglen == 1 && src[s] == '.') continue;   /* "." → drop */
        if (seglen == 2 && src[s] == '.' && src[s + 1] == '.') {
            if (ns > 0) { d = segs[--ns]; }           /* ".." → pop last segment */
            continue;
        }
        if (ns < 1024) segs[ns++] = d;
        dst[d++] = '/';
        memcpy(dst + d, src + s, seglen); d += seglen;
    }
    if (d == 0) { dst[d++] = '/'; }                   /* root */
    dst[d] = 0;
    return d;
}

ARO_BORROW static const char *korb_str_cstr_len(VALUE v, uint32_t *len) {
    *len = VAL2STR(v)->len;
    return korb_strbuf_data(VAL2STR(v)->buf);
}

/* File.expand_path(path, base = Dir.pwd) → an absolute, normalized path. */
/* File.realpath(path[, dir]) — canonical absolute path with symlinks resolved;
 * every component (incl. the last) must exist, else Errno::ENOENT. */
static RESULT korb_m_file_realpath(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
    char joined[4096]; size_t jl;
    if (plen > 0 && path[0] == '/') {                              /* absolute */
        jl = plen < sizeof joined ? plen : sizeof joined - 1; memcpy(joined, path, jl); joined[jl] = '\0';
    } else if (VALUE_SLICE_LEN(a) >= 2 && KORB_STRING_P(VALUE_SLICE_GET(a, 1))) {   /* base dir given */
        uint32_t dl; const char *d = korb_str_cstr_len(VALUE_SLICE_GET(a, 1), &dl);
        char db[4096]; if (dl >= sizeof db) dl = sizeof db - 1; memcpy(db, d, dl); db[dl] = '\0';
        char pb[4096]; if (plen >= sizeof pb) plen = sizeof pb - 1; memcpy(pb, path, plen); pb[plen] = '\0';
        snprintf(joined, sizeof joined, "%s/%s", db, pb);
    } else {                                                       /* relative to CWD */
        jl = plen < sizeof joined ? plen : sizeof joined - 1; memcpy(joined, path, jl); joined[jl] = '\0';
    }
    char real[4096];
    if (!realpath(joined, real)) return korb_raise_errno(c, slots, errno, "realpath", joined);
    return korb_str_new(c, slots, real, (uint32_t)strlen(real));
}
/* `tilde`: File.expand_path expands ~ / ~user; File.absolute_path does not. */
static RESULT korb_file_expand(CTX *c, VALUE *slots, VALUE_SLICE a, bool tilde) {
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);

    char raw[8192]; size_t r = 0;
    if (plen > 0 && path[0] == '/') {                 /* already absolute */
        memcpy(raw, path, plen); r = plen;
    } else if (tilde && plen > 0 && path[0] == '~' && (plen == 1 || path[1] == '/')) {
        const char *home = getenv("HOME"); if (!home) home = "/";
        size_t hl = strlen(home);
        memcpy(raw, home, hl); r = hl;
        if (plen > 1) { memcpy(raw + r, path + 1, plen - 1); r += plen - 1; }
    } else if (tilde && plen > 1 && path[0] == '~') {  /* ~user → that user's home */
        size_t nl = 1;
        while (nl < plen && path[nl] != '/') nl++;
        char user[256];
        if (nl - 1 >= sizeof user) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "user name too long");
        memcpy(user, path + 1, nl - 1); user[nl - 1] = '\0';
        const struct passwd *const pw = getpwnam(user);
        if (!pw) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "user %s doesn't exist", user);
        const size_t hl = strlen(pw->pw_dir);
        memcpy(raw, pw->pw_dir, hl); r = hl;
        if (nl < plen) { memcpy(raw + r, path + nl, plen - nl); r += plen - nl; }
    } else {                                          /* relative → resolve against base (or cwd) */
        char basebuf[8192]; size_t bl = 0;
        const bool have_base = VALUE_SLICE_LEN(a) >= 2 && KORB_STRING_P(VALUE_SLICE_GET(a, 1));
        if (have_base) {
            uint32_t blen; const char *base = korb_str_cstr_len(VALUE_SLICE_GET(a, 1), &blen);
            if (blen > 0 && base[0] == '/') { memcpy(basebuf, base, blen); bl = blen; }
            else if (blen > 0 && base[0] == '~' && (blen == 1 || base[1] == '/')) {
                const char *home = getenv("HOME"); if (!home) home = "/"; size_t hl = strlen(home);
                memcpy(basebuf, home, hl); bl = hl;
                if (blen > 1) { memcpy(basebuf + bl, base + 1, blen - 1); bl += blen - 1; }
            } else {                                  /* relative base → cwd + base */
                if (getcwd(basebuf, sizeof basebuf)) bl = strlen(basebuf); else { basebuf[0] = '/'; bl = 1; }
                basebuf[bl++] = '/'; memcpy(basebuf + bl, base, blen); bl += blen;
            }
        } else {
            if (getcwd(basebuf, sizeof basebuf)) bl = strlen(basebuf); else { basebuf[0] = '/'; bl = 1; }
        }
        memcpy(raw, basebuf, bl); r = bl;
        raw[r++] = '/';
        memcpy(raw + r, path, plen); r += plen;
    }
    if (r >= sizeof raw) r = sizeof raw - 1;
    char out[8192];
    size_t olen = korb_path_normalize(raw, r, out);
    /* CRuby keeps a leading run of separators verbatim ("////a" stays "////a"),
     * unlike #dirname; korb_path_normalize collapses it, so put it back. */
    size_t lead = 0;
    while (lead < r && raw[lead] == '/') lead++;
    if (lead < r && raw[lead] == '.') lead = 1;        /* "/" + "../x": the run is the base, not the path */
    if (lead > 1 && olen + lead < sizeof out) {
        memmove(out + lead - 1, out, olen + 1);
        for (size_t k = 0; k < lead - 1; k++) out[k] = '/';
        olen += lead - 1;
    }
    return korb_str_new(c, slots, out, (uint32_t)olen);
}
static RESULT korb_m_file_expand_path(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; return korb_file_expand(c, slots, a, true);
}
static RESULT korb_m_file_absolute_path(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; return korb_file_expand(c, slots, a, false);   /* no ~ expansion (CRuby) */
}

/* File.join(*parts) → parts joined with a single '/'. */
/* Append one File.join component.  Separator rule (CRuby's rb_file_join): for
 * every component after the first, a leading separator on the right absorbs any
 * trailing separators on the left, otherwise one is inserted when the left does
 * not already end with it.  No allocation happens here, so the interior string
 * pointers stay valid for the whole walk. */
static RESULT korb_file_join_str(CTX *c, VALUE *slots, VALUE pv, char *buf, size_t cap, size_t *d, bool *first) {
    if (UNLIKELY(!KORB_STRING_P(pv))) {
        /* #to_path first, then #to_str (CRuby's rb_get_path_check).  The result
         * is parked so the borrowed bytes below stay valid. */
        static const char *const conv[2] = { "to_path", "to_str" };
        for (uint32_t k = 0; k < 2 && !KORB_STRING_P(pv); k++) {
            const uint32_t mid = korb_intern(c->vm, conv[k], (uint32_t)strlen(conv[k]));
            if (!AROH_IS_GC_OBJECT(pv) || !korb_responds_to(c, pv, mid)) continue;
            slots[0] = pv;
            const RESULT r = korb_send(c, slots + 1, mid, 0, 0);
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
            pv = r.value;
        }
        if (UNLIKELY(!KORB_STRING_P(pv)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
        slots[0] = pv;                                  /* root: buf below borrows its bytes */
    }
    uint32_t plen; const char *p = korb_str_cstr_len(pv, &plen);
    if (!*first) {
        if (plen > 0 && p[0] == '/') { while (*d > 0 && buf[*d - 1] == '/') (*d)--; }
        else if (!(*d > 0 && buf[*d - 1] == '/') && *d + 1 < cap) buf[(*d)++] = '/';
    }
    *first = false;
    if (*d + plen >= cap) plen = (uint32_t)(cap - 1 - *d);
    memcpy(buf + *d, p, plen); *d += plen;
    return RESULT_OK(KORB_NIL);
}

/* Arrays are flattened; an empty one still counts as an (empty) component, so
 * File.join("a", []) is "a/" just as in CRuby. */
static RESULT korb_file_join_val(CTX *c, VALUE *slots, VALUE pv, char *buf, size_t cap, size_t *d, bool *first, uint32_t depth) {
    if (!KORB_ARRAY_P(pv)) return korb_file_join_str(c, slots, pv, buf, cap, d, first);
    if (UNLIKELY(depth > 16)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "recursive array");
    const uint32_t n = VAL2ARY(pv)->len;
    if (n == 0) {
        if (!*first && !(*d > 0 && buf[*d - 1] == '/') && *d + 1 < cap) buf[(*d)++] = '/';
        *first = false;
        return RESULT_OK(KORB_NIL);
    }
    for (uint32_t i = 0; i < n; i++)
        CHECK(korb_file_join_val(c, slots, korb_items_data(VAL2ARY(pv)->items)[i], buf, cap, d, first, depth + 1));
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_file_join(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    char buf[8192]; size_t d = 0; bool first = true;
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++)
        CHECK(korb_file_join_val(c, slots, VALUE_SLICE_GET(a, i), buf, sizeof buf, &d, &first, 0));
    return korb_str_new(c, slots, buf, (uint32_t)d);
}

/* File.dirname(path) → everything before the last '/', or "." if none. */
static RESULT korb_m_file_dirname(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    uint32_t n; const char *p = korb_str_cstr_len(pv, &n);
    while (n > 1 && p[n - 1] == '/') n--;             /* strip trailing slashes */
    size_t last = (size_t)-1;
    for (size_t i = 0; i < n; i++) if (p[i] == '/') last = i;
    if (last == (size_t)-1) return korb_str_new(c, slots, ".", 1);
    while (last > 0 && p[last - 1] == '/') last--;     /* collapse the separator run */
    if (last == 0) return korb_str_new(c, slots, "/", 1);
    char out[8192]; if (last >= sizeof out) last = sizeof out - 1;   /* copy off the (movable) source before alloc */
    memcpy(out, p, last);
    size_t off = 0;                                    /* a leading "//..." run collapses to one "/" (CRuby) */
    while (off + 1 < last && out[off] == '/' && out[off + 1] == '/') off++;
    return korb_str_new(c, slots, out + off, (uint32_t)(last - off));
}

static RESULT korb_m_file_basename(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* fwd */
/* File.split(path) → [File.dirname(path), File.basename(path)]. */
static RESULT korb_m_file_split(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    RESULT dr = korb_m_file_dirname(c, slots, self, a);
    if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
    slots[0] = dr.value;                                  /* dirname (rooted across basename's alloc) */
    RESULT br = korb_m_file_basename(c, slots + 1, self, a);
    if (UNLIKELY(br.state != KORB_NORMAL)) return br;
    slots[1] = br.value;
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF arr = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, arr, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, arr, slots[1]));
    return RESULT_OK(VALUE_REF_GET(arr));
}

/* File.basename(path, suffix = nil) → last component, optionally minus a suffix
 * (".*" strips any extension). */
static RESULT korb_m_file_basename(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    uint32_t n; const char *p = korb_str_cstr_len(pv, &n);
    if (n == 0) return korb_str_new(c, slots, "", 0);   /* basename("") → "" */
    while (n > 1 && p[n - 1] == '/') n--;             /* strip trailing slashes */
    size_t s = 0;
    for (size_t i = 0; i < n; i++) if (p[i] == '/') s = i + 1;
    const char *base = p + s; uint32_t blen = (uint32_t)(n - s);
    if (blen == 0) return korb_str_new(c, slots, "/", 1);
    if (VALUE_SLICE_LEN(a) >= 2 && KORB_STRING_P(VALUE_SLICE_GET(a, 1))) {
        uint32_t sl; const char *suf = korb_str_cstr_len(VALUE_SLICE_GET(a, 1), &sl);
        if (sl == 2 && suf[0] == '.' && suf[1] == '*') {           /* ".*" → drop any extension */
            for (int32_t i = (int32_t)blen - 1; i > 0; i--) if (base[i] == '.') { blen = (uint32_t)i; break; }
        } else if (sl > 0 && sl < blen && memcmp(base + blen - sl, suf, sl) == 0) {
            blen -= sl;
        }
    }
    char out[8192]; if (blen >= sizeof out) blen = sizeof out - 1;   /* copy off the (movable) source before alloc */
    memcpy(out, base, blen);
    return korb_str_new(c, slots, out, blen);
}

/* File.extname(path) → ".ext" of the basename, or "" (a leading dot is not one). */
static RESULT korb_m_file_extname(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    uint32_t n; const char *p = korb_str_cstr_len(pv, &n);
    while (n > 1 && p[n - 1] == '/') n--;
    size_t s = 0;
    for (size_t i = 0; i < n; i++) if (p[i] == '/') s = i + 1;
    const char *base = p + s; uint32_t blen = (uint32_t)(n - s);
    size_t dot = (size_t)-1;
    for (size_t i = 1; i < blen; i++) if (base[i] == '.') dot = i;     /* skip a leading dot (i starts at 1) */
    if (dot == (size_t)-1 || dot == (size_t)blen - 1) return korb_str_new(c, slots, "", 0);  /* none / trailing dot */
    char out[512]; uint32_t el = (uint32_t)(blen - dot); if (el >= sizeof out) el = sizeof out - 1;
    memcpy(out, base + dot, el);                       /* copy off the (movable) source before alloc */
    return korb_str_new(c, slots, out, el);
}

/* File.fnmatch — Ruby semantics implemented directly (POSIX fnmatch(3) lacks
 * `**` recursion and `{a,b}` FNM_EXTGLOB, and differs on unterminated `[`). */
#define KFNM_NOESCAPE 1
#define KFNM_PATHNAME 2
#define KFNM_DOTMATCH 4
#define KFNM_CASEFOLD 8
#define KFNM_EXTGLOB  16
static bool kfnm_core(const char *p, const char *pe, const char *s, const char *se, int flags);
/* one segment (or the whole string when !PATHNAME): leading-dot guard + core. */
static bool kfnm_helper(const char *p, const char *pe, const char *s, const char *se, int flags) {
    if (!(flags & KFNM_DOTMATCH) && s < se && *s == '.') {
        const char *q = p; char pc = 0;                /* pattern must OPEN with a literal '.' */
        if (q < pe) { pc = *q; if (pc == '\\' && !(flags & KFNM_NOESCAPE) && q + 1 < pe) pc = q[1]; }
        if (pc != '.' && q < pe && (*q == '*' || *q == '?' || *q == '[')) return false;
    }
    return kfnm_core(p, pe, s, se, flags);
}
static bool kfnm_core(const char *p, const char *pe, const char *s, const char *se, int flags) {
    const bool noesc = flags & KFNM_NOESCAPE, fold = flags & KFNM_CASEFOLD;
    while (p < pe) {
        char pc = *p;
        if (pc == '*') {
            while (p < pe && *p == '*') p++;           /* collapse runs of '*' */
            if (p == pe) return true;
            for (const char *t = s; t <= se; t++)
                if (kfnm_core(p, pe, t, se, flags)) return true;
            return false;
        }
        if (pc == '[') {
            if (s >= se) return false;
            const char *q = p + 1; bool neg = false;
            if (q < pe && (*q == '!' || *q == '^')) { neg = true; q++; }
            const char sc = fold ? (char)tolower((unsigned char)*s) : *s;
            bool matched = false, closed = false, first = true;
            const char *r = q;
            while (r < pe) {
                if (*r == ']' && !first) { closed = true; break; }
                first = false;
                char lo = *r;
                if (lo == '\\' && !noesc && r + 1 < pe) { r++; lo = *r; }
                char hi = lo;
                if (r + 2 < pe && r[1] == '-' && r[2] != ']') {
                    hi = r[2]; r += 2;
                    if (hi == '\\' && !noesc && r + 1 < pe) { r++; hi = *r; }
                }
                const char flo = fold ? (char)tolower((unsigned char)lo) : lo;
                const char fhi = fold ? (char)tolower((unsigned char)hi) : hi;
                if (sc >= flo && sc <= fhi) matched = true;
                r++;
            }
            if (!closed) return false;                 /* unterminated '[' never matches (CRuby) */
            if (matched == neg) return false;
            p = r + 1; s++; continue;
        }
        if (s >= se) return false;
        if (pc == '?') { p++; s++; continue; }
        if (pc == '\\' && !noesc && p + 1 < pe) { p++; pc = *p; }
        {
            const char b = fold ? (char)tolower((unsigned char)*s) : *s;
            const char a2 = fold ? (char)tolower((unsigned char)pc) : pc;
            if (a2 != b) return false;
        }
        p++; s++;
    }
    return s == se;
}
/* PATHNAME: match '/'-separated segments; a "**\/" pattern segment matches zero
 * or more directories. */
static bool kfnm_segs(const char *p, const char *pe, const char *s, const char *se, int flags) {
    const char *psl = memchr(p, '/', (size_t)(pe - p));
    const char *pend1 = psl ? psl : pe;
    if (pend1 - p == 2 && p[0] == '*' && p[1] == '*' && psl) {   /* "**\/rest" */
        const char *t = s;
        for (;;) {
            if (kfnm_segs(psl + 1, pe, t, se, flags)) return true;
            if (!(flags & KFNM_DOTMATCH) && t < se && *t == '.') return false;   /* don't recurse into dot-dirs */
            const char *sl = memchr(t, '/', (size_t)(se - t));
            if (!sl) return false;
            t = sl + 1;
        }
    }
    const char *ssl = memchr(s, '/', (size_t)(se - s));
    const char *send1 = ssl ? ssl : se;
    if (!kfnm_helper(p, pend1, s, send1, flags)) return false;
    if (psl && ssl) return kfnm_segs(psl + 1, pe, ssl + 1, se, flags);
    return !psl && !ssl;
}
/* FNM_EXTGLOB {a,b} brace alternation (nesting-aware), then match. */
static bool kfnm_match(const char *p, uint32_t plen, const char *s, uint32_t slen, int flags, int depth) {
    if ((flags & KFNM_EXTGLOB) && depth < 16) {
        const bool noesc = flags & KFNM_NOESCAPE;
        for (uint32_t i = 0; i < plen; i++) {
            if (p[i] == '\\' && !noesc) { i++; continue; }
            if (p[i] != '{') continue;
            int nest = 0; uint32_t close = 0;
            for (uint32_t j = i; j < plen; j++) {
                if (p[j] == '\\' && !noesc) { j++; continue; }
                if (p[j] == '{') nest++;
                else if (p[j] == '}') { nest--; if (nest == 0) { close = j; break; } }
            }
            if (close == 0) break;                     /* unmatched '{' → literal */
            /* try each top-level comma alternative */
            uint32_t alt = i + 1;
            for (uint32_t j = i + 1; j <= close; j++) {
                if (p[j] == '\\' && !noesc) { j++; continue; }
                if (p[j] == '{') { int n2 = 1; while (++j <= close && n2) { if (p[j]=='{') n2++; else if (p[j]=='}') n2--; else if (p[j]=='\\' && !noesc) j++; } j--; continue; }
                if (j == close || p[j] == ',') {
                    char buf[4096];
                    const uint32_t al = j - alt;
                    if (i + al + (plen - close - 1) + 1 < sizeof buf) {
                        memcpy(buf, p, i);
                        memcpy(buf + i, p + alt, al);
                        memcpy(buf + i + al, p + close + 1, plen - close - 1);
                        if (kfnm_match(buf, i + al + (plen - close - 1), s, slen, flags, depth + 1)) return true;
                    }
                    alt = j + 1;
                }
            }
            return false;
        }
    }
    if (flags & KFNM_PATHNAME) return kfnm_segs(p, p + plen, s, s + slen, flags);
    return kfnm_helper(p, p + plen, s, s + slen, flags);
}
static RESULT korb_m_file_fnmatch(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 2))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 2..3)", VALUE_SLICE_LEN(a));
    for (int i = 0; i < 2; i++) {                      /* #to_path / #to_str coercion */
        VALUE v = VALUE_SLICE_GET(a, i);
        if (KORB_STRING_P(v)) continue;
        if (KORB_OBJECT_P(v)) {
            static const char *const mids[2] = { "to_path", "to_str" };
            for (int m = 0; m < 2; m++) {
                VALUE cv = v;
                if (korb_responds_to_coerce_p(c, slots, &cv, korb_intern(c->vm, mids[m], (uint32_t)strlen(mids[m])))) {
                    slots[0] = cv;
                    RESULT sr = korb_send_impl(c, slots + 1, korb_intern(c->vm, mids[m], (uint32_t)strlen(mids[m])), 0, 0, NULL, NULL, NULL);
                    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                    if (KORB_STRING_P(sr.value)) { VALUE_REF_SET(VALUE_SLICE_REF(a, i), sr.value); v = sr.value; break; }
                }
            }
        }
        if (UNLIKELY(!KORB_STRING_P(v)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(VALUE_SLICE_GET(a, i)));
    }
    const KorbString *const pat = VAL2STR(VALUE_SLICE_GET(a, 0));
    const KorbString *const str = VAL2STR(VALUE_SLICE_GET(a, 1));
    const long rf = (VALUE_SLICE_LEN(a) >= 3 && FIXNUM_P(VALUE_SLICE_GET(a, 2))) ? FIX2LONG(VALUE_SLICE_GET(a, 2)) : 0;
    char pbuf[4096], sbuf[4096];
    if (UNLIKELY(pat->len >= sizeof pbuf || str->len >= sizeof sbuf)) return RESULT_OK(KORB_FALSE);
    memcpy(pbuf, korb_strbuf_data(pat->buf), pat->len);
    memcpy(sbuf, korb_strbuf_data(str->buf), str->len);
    return RESULT_OK(kfnm_match(pbuf, pat->len, sbuf, str->len, (int)rf, 0) ? KORB_TRUE : KORB_FALSE);
}

/* stat-based File predicates.  The path pointer is used before any allocation,
 * so it stays valid (no moving-GC hazard). */
static RESULT korb_m_file_stat_pred(CTX *c, VALUE *slots, VALUE_SLICE a, int kind) {
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
    struct stat st;
    if (stat(path, &st) != 0) return RESULT_OK(kind == 3 ? KORB_NIL : KORB_FALSE);
    switch (kind) {
      case 0: return RESULT_OK(KORB_TRUE);                                  /* exist? */
      case 1: return RESULT_OK(S_ISREG(st.st_mode) ? KORB_TRUE : KORB_FALSE);/* file? */
      case 2: return RESULT_OK(S_ISDIR(st.st_mode) ? KORB_TRUE : KORB_FALSE);/* directory? */
      default: return RESULT_OK(LONG2FIX((korb_sword_t)st.st_size));            /* size */
    }
}
static RESULT korb_m_file_symlink_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
    struct stat st;
    if (lstat(path, &st) != 0) return RESULT_OK(KORB_FALSE);
    return RESULT_OK(S_ISLNK(st.st_mode) ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_file_exist_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)     { (void)self; return korb_m_file_stat_pred(c, slots, a, 0); }
static RESULT korb_m_file_file_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)      { (void)self; return korb_m_file_stat_pred(c, slots, a, 1); }
static RESULT korb_m_file_directory_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self; return korb_m_file_stat_pred(c, slots, a, 2); }
static RESULT korb_m_file_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)        { (void)self; return korb_m_file_stat_pred(c, slots, a, 3); }
/* File.readable?/writable?/executable?(path) → access(2) with R_OK/W_OK/X_OK. */
static RESULT korb_m_file_access(CTX *c, VALUE *slots, VALUE_SLICE a, int amode) {
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
    return RESULT_OK(access(path, amode) == 0 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_file_readable_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)self; return korb_m_file_access(c, slots, a, R_OK); }
static RESULT korb_m_file_writable_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)   { (void)self; return korb_m_file_access(c, slots, a, W_OK); }
static RESULT korb_m_file_executable_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self; return korb_m_file_access(c, slots, a, X_OK); }
/* File.chown(owner, group, *paths) — nil/-1 for "leave unchanged". */
static RESULT korb_m_file_chown(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE ov = VALUE_SLICE_GET(a, 0), gv = VALUE_SLICE_GET(a, 1);
    const uid_t uid = FIXNUM_P(ov) ? (uid_t)FIX2LONG(ov) : (uid_t)-1;
    const gid_t gid = FIXNUM_P(gv) ? (gid_t)FIX2LONG(gv) : (gid_t)-1;
    uint32_t n = 0;
    for (uint32_t i = 2; i < VALUE_SLICE_LEN(a); i++) {
        VALUE pv;
        KORB_PATH_ARG(c, slots, a, i, pv);
        uint32_t plen; const char *const path = korb_str_cstr_len(pv, &plen);
        if (chown(path, uid, gid) != 0) return korb_raise_errno(c, slots, errno, "chown", path);
        n++;
    }
    return RESULT_OK(LONG2FIX(n));
}

/* __file_utime(atime_f, mtime_f, follow, *paths) — File.utime / File.lutime.
 * Times arrive as epoch Floats (nil → now). */
static RESULT korb_m_file_utime(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    struct timespec ts[2];
    for (int i = 0; i < 2; i++) {
        const VALUE tv = VALUE_SLICE_GET(a, i);
        double d;
        if (tv == KORB_NIL) { ts[i].tv_nsec = UTIME_NOW; ts[i].tv_sec = 0; }
        else if (korb_num_to_d(tv, &d)) { ts[i].tv_sec = (time_t)d; ts[i].tv_nsec = (long)((d - (double)(time_t)d) * 1e9); }
        else return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into time");
    }
    const bool follow = KORB_TRUTHY(VALUE_SLICE_GET(a, 2));
    uint32_t n = 0;
    for (uint32_t i = 3; i < VALUE_SLICE_LEN(a); i++) {
        VALUE pv;
        KORB_PATH_ARG(c, slots, a, i, pv);
        uint32_t plen; const char *const path = korb_str_cstr_len(pv, &plen);
        if (utimensat(AT_FDCWD, path, ts, follow ? 0 : AT_SYMLINK_NOFOLLOW) != 0)
            return korb_raise_errno(c, slots, errno, "utime", path);
        n++;
    }
    return RESULT_OK(LONG2FIX(n));
}

/* File.mkfifo(path, mode = 0666) */
static RESULT korb_m_file_mkfifo(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    uint32_t plen; const char *const path = korb_str_cstr_len(pv, &plen);
    const mode_t m = (VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1)))
                       ? (mode_t)FIX2LONG(VALUE_SLICE_GET(a, 1)) : 0666;
    if (mkfifo(path, m) != 0) return korb_raise_errno(c, slots, errno, "mkfifo", path);
    return RESULT_OK(LONG2FIX(0));
}

/* __file_mode_bits(path, follow) → [mode, uid, gid] or nil (for the
 * world_readable?/world_writable? family, which reports the permission bits). */
static RESULT korb_m_file_mode_bits(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    uint32_t plen; const char *const path = korb_str_cstr_len(pv, &plen);
    struct stat st;
    const bool follow = VALUE_SLICE_LEN(a) < 2 || KORB_TRUTHY(VALUE_SLICE_GET(a, 1));
    if ((follow ? stat(path, &st) : lstat(path, &st)) != 0) return RESULT_OK(KORB_NIL);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 3));
    VALUE_REF ar = VALUE_REF_AT(&slots[0]);
    CHECK(korb_ary_push_val(c, slots + 1, ar, LONG2FIX((korb_sword_t)st.st_mode)));
    CHECK(korb_ary_push_val(c, slots + 1, ar, LONG2FIX((korb_sword_t)st.st_uid)));
    CHECK(korb_ary_push_val(c, slots + 1, ar, LONG2FIX((korb_sword_t)st.st_gid)));
    return RESULT_OK(VALUE_REF_GET(ar));
}

/* File.chmod(mode, *paths) → chmod each; returns the number of files. */
/* A mode argument: Integer as is, else #to_int; a Bignum cannot be a mode. */
static RESULT korb_file_mode_arg(CTX *c, VALUE *slots, VALUE v, mode_t *out) {
    if (UNLIKELY(KORB_BIGNUM_P(v)))
        return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into 'unsigned long'");
    if (!FIXNUM_P(v)) {
        const char *const cls = korb_type_name(v);
        VALUE t = v;
        const RESULT cr = korb_coerce_to_int(c, slots, &t);
        if (UNLIKELY(cr.state != KORB_NORMAL)) return cr;
        if (cr.value != KORB_TRUE || !FIXNUM_P(t)) {
            if (KORB_BIGNUM_P(t))
                return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into 'unsigned long'");
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", cls);
        }
        v = t;
    }
    *out = (mode_t)FIX2LONG(v);
    return RESULT_OK(KORB_TRUE);
}
static RESULT korb_m_file_chmod(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1+)");
    mode_t m = 0;
    CHECK(korb_file_mode_arg(c, slots, VALUE_SLICE_GET(a, 0), &m));
    uint32_t n = 0;
    for (uint32_t i = 1; i < VALUE_SLICE_LEN(a); i++) {
        const VALUE pv = VALUE_SLICE_GET(a, i);
        if (UNLIKELY(!KORB_STRING_P(pv))) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
        uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
        if (chmod(path, m) != 0) return korb_raise_errno(c, slots, errno, "chmod", path);
        n++;
    }
    return RESULT_OK(LONG2FIX(n));
}
/* File.umask([mask]) → sets and/or returns the process file-creation mask. */
static RESULT korb_m_file_umask(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots; (void)self;
    if (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0)))
        return RESULT_OK(LONG2FIX((korb_sword_t)umask((mode_t)FIX2LONG(VALUE_SLICE_GET(a, 0)))));
    const mode_t cur = umask(0); umask(cur);                  /* read current without changing it */
    return RESULT_OK(LONG2FIX((korb_sword_t)cur));
}

/* ---- one-shot descriptor I/O ---------------------------------------------
 * These slurp or spill a whole file in one call and close it again.  They use
 * read(2)/write(2) directly for the same reason the stream layer does: a FILE*
 * would reintroduce a buffer the interpreter does not control. */

/* Read `fd` to EOF into a malloc'd, NUL-terminated buffer (caller frees). */
static char *korb_fd_slurp(int fd, size_t *out_len) {
    char *buf = NULL; size_t cap = 0, len = 0;
    for (;;) {
        if (len + 65536 + 1 > cap) {
            cap = cap ? cap * 2 : 131072;
            char *const nb = realloc(buf, cap);
            if (!nb) { free(buf); *out_len = 0; return NULL; }
            buf = nb;
        }
        const ssize_t got = read(fd, buf + len, cap - len - 1);
        if (got < 0) { if (errno == EINTR) continue; break; }
        if (got == 0) break;
        len += (size_t)got;
    }
    if (!buf) { buf = malloc(1); if (!buf) { *out_len = 0; return NULL; } }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

/* Read exactly up to `want` bytes into `dst`; returns the count transferred. */
static size_t korb_fd_read_n(int fd, char *dst, size_t want) {
    size_t got = 0;
    while (got < want) {
        const ssize_t n = read(fd, dst + got, want - got);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        got += (size_t)n;
    }
    return got;
}

/* Write all n bytes, retrying short writes; returns the count written. */
static size_t korb_fd_write_all(int fd, const char *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        const ssize_t w = write(fd, p + off, n - off);
        if (w < 0) { if (errno == EINTR) continue; break; }
        off += (size_t)w;
    }
    return off;
}

/* ---- open options for the IO class methods -------------------------------
 *
 * IO.read / IO.write and friends open the file themselves, so they take the
 * options `open` would: `mode:`, `encoding:` / `external_encoding:`, and
 * `open_args:` (an Array of literal arguments to `open`, which supersedes every
 * other option).  An `open_args:` without a mode leaves the default "r" in
 * place — that is exactly why `IO.write(f, s, open_args: [{…}])` is an IOError. */
static const char *korb_io_bom_at(const char *p, uint32_t n, uint32_t *blen);   /* fwd (builtins/io.c) */
struct korb_open_args { char mode[32]; int enc; bool bom; };   /* bom: a "BOM|enc" mode prefix */

/* the encoding name of an `encoding:` value (an Encoding or a String) */
static RESULT korb_open_enc_name(CTX *c, VALUE *slots, VALUE v, char *out, size_t cap) {
    out[0] = '\0';
    if (KORB_STRING_P(v)) { snprintf(out, cap, "%.*s", (int)VAL2STR(v)->len, korb_strbuf_data(VAL2STR(v)->buf)); return RESULT_OK(KORB_NIL); }
    if (v == KORB_NIL) return RESULT_OK(KORB_NIL);
    slots[0] = v;
    const RESULT r = korb_send(c, slots + 1, korb_intern(c->vm, "to_s", 4), 0, 0);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (KORB_STRING_P(r.value)) snprintf(out, cap, "%.*s", (int)VAL2STR(r.value)->len, korb_strbuf_data(VAL2STR(r.value)->buf));
    return RESULT_OK(KORB_NIL);
}

/* mode / encoding out of one options Hash (also used for an open_args element) */
static RESULT korb_open_args_hash(CTX *c, VALUE *slots, VALUE h, struct korb_open_args *o) {
    const KorbHash *const hh = VAL2HASH(h);
    int32_t ix = korb_hash_find(hh, ID2SYM(korb_intern(c->vm, "mode", 4)));
    if (ix >= 0) {
        const VALUE mv = korb_items_data(hh->items)[2 * ix + 1];
        if (KORB_STRING_P(mv)) snprintf(o->mode, sizeof o->mode, "%.*s", (int)VAL2STR(mv)->len, korb_strbuf_data(VAL2STR(mv)->buf));
    }
    static const struct { const char *nm; uint32_t len; } ek[] = { { "encoding", 8 }, { "external_encoding", 17 } };
    for (size_t i = 0; i < 2; i++) {
        ix = korb_hash_find(VAL2HASH(h), ID2SYM(korb_intern(c->vm, ek[i].nm, ek[i].len)));
        if (ix < 0) continue;
        char nm[64];
        CHECK(korb_open_enc_name(c, slots, korb_items_data(VAL2HASH(h)->items)[2 * ix + 1], nm, sizeof nm));
        if (nm[0]) o->enc = (int)korb_enc_index_for_name(c->vm, nm);
    }
    return RESULT_OK(KORB_NIL);
}

static RESULT
korb_io_open_args(CTX *c, VALUE *slots, VALUE opts, const char *defmode, struct korb_open_args *o) {
    snprintf(o->mode, sizeof o->mode, "%s", defmode);
    o->enc = -1;
    o->bom = false;
    if (!KORB_HASH_P(opts)) return RESULT_OK(KORB_NIL);
    const int32_t ox = korb_hash_find(VAL2HASH(opts), ID2SYM(korb_intern(c->vm, "open_args", 9)));
    if (ox >= 0 && KORB_ARRAY_P(korb_items_data(VAL2HASH(opts)->items)[2 * ox + 1])) {
        slots[0] = korb_items_data(VAL2HASH(opts)->items)[2 * ox + 1];   /* park: the Hash walk dispatches */
        snprintf(o->mode, sizeof o->mode, "r");                          /* `open`'s own default */
        const uint32_t n = VAL2ARY(slots[0])->len;
        for (uint32_t i = 0; i < n; i++) {
            const VALUE el = korb_items_data(VAL2ARY(slots[0])->items)[i];
            if (KORB_STRING_P(el)) snprintf(o->mode, sizeof o->mode, "%.*s", (int)VAL2STR(el)->len, korb_strbuf_data(VAL2STR(el)->buf));
            else if (KORB_HASH_P(el)) CHECK(korb_open_args_hash(c, slots + 1, el, o));
        }
    } else {
        CHECK(korb_open_args_hash(c, slots, opts, o));
    }
    /* a "r:UTF-8" style mode carries the external encoding; a "BOM|" prefix on it
     * asks that a byte-order mark decide (and be stripped) instead */
    char *const colon = strchr(o->mode, ':');
    if (colon) {
        *colon = '\0';
        char *ename = colon + 1;
        if (strncasecmp(ename, "BOM|", 4) == 0) { o->bom = true; ename += 4; }
        if (*ename) o->enc = (int)korb_enc_index_for_name(c->vm, ename);
    }
    return RESULT_OK(KORB_NIL);
}

static bool korb_mode_readable(const char *m) { return m[0] == 'r' || strchr(m, '+') != NULL; }
static bool korb_mode_writable(const char *m) { return m[0] == 'w' || m[0] == 'a' || strchr(m, '+') != NULL; }

/* One optional Integer argument (length / offset), with #to_int coercion. */
static RESULT korb_io_int_arg(CTX *c, VALUE *slots, VALUE v, bool *given, korb_sword_t *out) {
    *given = false;
    if (v == KORB_NIL) return RESULT_OK(KORB_NIL);
    if (FIXNUM_P(v)) { *out = FIX2LONG(v); *given = true; return RESULT_OK(KORB_NIL); }
    if (!KORB_OBJECT_P(v) || !korb_responds_to(c, v, korb_intern(c->vm, "to_int", 6)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_coerce_name(c, v));
    slots[0] = v;
    const RESULT r = korb_send(c, slots + 1, korb_intern(c->vm, "to_int", 6), 0, 0);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (!FIXNUM_P(r.value))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_coerce_name(c, v));
    *out = FIX2LONG(r.value); *given = true;
    return RESULT_OK(KORB_NIL);
}

/* IO.read(path[, length[, offset]], **opts) → the file (or a slice) as a String. */
static RESULT korb_m_file_read(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    slots[0] = pv;                                     /* park: the option walk dispatches */
    uint32_t na = VALUE_SLICE_LEN(a);
    VALUE opts = KORB_NIL;
    if (na >= 2 && KORB_HASH_P(VALUE_SLICE_GET(a, na - 1))) { opts = VALUE_SLICE_GET(a, na - 1); na--; }
    struct korb_open_args oa;
    CHECK(korb_io_open_args(c, slots + 1, opts, "r", &oa));
    if (UNLIKELY(!korb_mode_readable(oa.mode)))
        return korb_raise(c, slots + 1, KORB_E_IOERROR, 0, "not opened for reading");
    bool has_len = false, has_off = false;
    korb_sword_t want = 0, off = 0;
    if (na >= 2) CHECK(korb_io_int_arg(c, slots + 1, VALUE_SLICE_GET(a, 1), &has_len, &want));
    if (na >= 3) CHECK(korb_io_int_arg(c, slots + 1, VALUE_SLICE_GET(a, 2), &has_off, &off));
    if (UNLIKELY(has_off && off < 0))
        return korb_raise(c, slots + 1, KORB_E_ARGUMENT, 0, "negative offset %ld given", (long)off);
    if (UNLIKELY(has_len && want < 0))
        return korb_raise(c, slots + 1, KORB_E_ARGUMENT, 0, "negative length %ld given", (long)want);
    uint32_t plen; const char *const path = korb_str_cstr_len(slots[0], &plen);   /* re-read: it may have moved */
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return korb_raise_errno(c, slots + 1, errno, "rb_sysopen", path);
    if (has_off) (void)lseek(fd, (off_t)off, SEEK_SET);
    RESULT r;
    if (has_len) {                                                    /* bounded read */
        char *const b = malloc((size_t)want + 1);
        if (!b) { close(fd); return korb_raise(c, slots + 1, KORB_E_RUNTIME, 0, "out of memory"); }
        const size_t got = korb_fd_read_n(fd, b, (size_t)want); close(fd);
        if (got == 0 && want > 0) { free(b); return RESULT_OK(KORB_NIL); }   /* EOF */
        r = korb_str_new(c, slots + 1, b, (uint32_t)got);
        free(b);
        if (LIKELY(r.state == KORB_NORMAL)) KORB_STR_ENC_SET(r.value, KORB_ENC_BINARY);   /* a sized read counts bytes */
        return r;
    }
    size_t len = 0;
    char *const buf = korb_fd_slurp(fd, &len);
    close(fd);
    if (!buf) return korb_raise(c, slots + 1, KORB_E_RUNTIME, 0, "out of memory reading %s", path);
    const char *data = buf;
    int enc = oa.enc;
    if (oa.bom) {                                        /* "r:BOM|enc": the mark decides and is dropped */
        uint32_t blen = 0;
        const char *const bname = korb_io_bom_at(buf, (uint32_t)len, &blen);
        if (bname) { data += blen; len -= blen; enc = (int)korb_enc_index_for_name(c->vm, bname); }
    }
    r = korb_str_new(c, slots + 1, data, (uint32_t)len);
    free(buf);
    if (LIKELY(r.state == KORB_NORMAL) && enc >= 0) KORB_STR_ENC_SET(r.value, (uint32_t)enc);
    return r;
}

/* slurp the whole file into a malloc'd buffer (caller frees); NULL on open fail. */
static char *korb_file_slurp(const char *path, size_t *out_len) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) { *out_len = 0; return NULL; }
    char *const buf = korb_fd_slurp(fd, out_len);
    close(fd);
    return buf;
}

/* IO.write(path, string[, offset], **opts) → the byte count.  An offset writes
 * into the existing file instead of truncating it. */
static RESULT korb_m_file_write(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    slots[0] = pv;                                     /* park: the coercions below dispatch */
    uint32_t na = VALUE_SLICE_LEN(a);
    VALUE opts = KORB_NIL;
    if (na >= 3 && KORB_HASH_P(VALUE_SLICE_GET(a, na - 1))) { opts = VALUE_SLICE_GET(a, na - 1); na--; }
    bool has_off = false; korb_sword_t off = 0;
    if (na >= 3) CHECK(korb_io_int_arg(c, slots + 1, VALUE_SLICE_GET(a, 2), &has_off, &off));
    struct korb_open_args oa;
    /* an offset without an explicit mode means "r+": writing at a position is
     * meant to leave the rest of the file alone */
    CHECK(korb_io_open_args(c, slots + 1, opts, has_off ? "r+" : "w", &oa));
    if (UNLIKELY(!korb_mode_writable(oa.mode)))
        return korb_raise(c, slots + 1, KORB_E_IOERROR, 0, "not opened for writing");
    slots[1] = VALUE_SLICE_GET(a, 1);
    if (!KORB_STRING_P(slots[1])) {                    /* CRuby writes obj.to_s */
        const RESULT sr = korb_send(c, slots + 2, korb_intern(c->vm, "to_s", 4), 0, 0);
        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        if (UNLIKELY(!KORB_STRING_P(sr.value)))
            return korb_raise(c, slots + 2, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, VALUE_SLICE_GET(a, 1)));
        slots[1] = sr.value;
    }
    uint32_t plen; const char *const path = korb_str_cstr_len(slots[0], &plen);   /* re-read: they may have moved */
    const int oflags = O_WRONLY | O_CREAT |
                       (oa.mode[0] == 'a' ? O_APPEND : (oa.mode[0] == 'w' ? O_TRUNC : 0));
    const int fd = open(path, oflags, 0666);
    if (fd < 0) return korb_raise_errno(c, slots + 2, errno, "rb_sysopen", path);
    if (has_off) (void)lseek(fd, (off_t)off, SEEK_SET);
    const KorbString *const ds = VAL2STR(slots[1]);
    const size_t w = korb_fd_write_all(fd, korb_strbuf_data(ds->buf), ds->len);   /* libc only: no GC below */
    close(fd);
    return RESULT_OK(LONG2FIX((korb_sword_t)w));
}

/* ---- line arguments (IO#gets / #readlines / #each_line, IO.foreach, …) -----
 *
 * Every line reader takes the same `(sep = $/, limit = nil, chomp: false)`.  A
 * lone argument is the separator when it is (or converts to) a String and the
 * byte limit otherwise; `nil` slurps to EOF and `""` selects paragraph mode
 * (records end at a blank line, and the run of newlines after it is consumed).
 * The limit counts bytes of the returned String, separator included. */
struct korb_line_args { bool slurp, paragraph, chomp; korb_sword_t limit; };

/* Parse them, starting at positional index `from`.  The separator is left in
 * slots[0] (rooted, so a reader that allocates can re-read its bytes through the
 * slot instead of caching a pointer across a GC); paragraph mode leaves the
 * "\n\n" it actually matches there.  The caller therefore works from slots+1. */
static RESULT
korb_io_line_args(CTX *c, VALUE *slots, VALUE_SLICE a, uint32_t from, struct korb_line_args *o)
{
    o->slurp = o->paragraph = o->chomp = false;
    o->limit = -1;
    uint32_t n = VALUE_SLICE_LEN(a);
    /* only real keyword args are options — a positional Hash falls through to
     * the limit position and TypeErrors there (CRuby) */
    if (n > from && KORB_HASH_P(VALUE_SLICE_GET(a, n - 1)) && korb_kwargs_hash_p(VALUE_SLICE_GET(a, n - 1))) {
        const KorbHash *const h = VAL2HASH(VALUE_SLICE_GET(a, n - 1));
        const int32_t hx = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "chomp", 5)));
        if (hx >= 0) o->chomp = KORB_TRUTHY(korb_items_data(h->items)[2 * hx + 1]);
        n--;
    }
    const uint32_t np = (n > from) ? n - from : 0;
    if (UNLIKELY(np > 2))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 0..2)", np);
    VALUE sep = KORB_UNDEF, lim = KORB_NIL;
    if (np == 1) {
        const VALUE v = VALUE_SLICE_GET(a, from);
        /* String-ish → separator, anything else → limit (CRuby tries the String
         * conversion first, non-raising) */
        if (v == KORB_NIL || KORB_STRING_P(v) ||
            (KORB_OBJECT_P(v) && korb_responds_to(c, v, korb_intern(c->vm, "to_str", 6)))) sep = v;
        else lim = v;
    } else if (np == 2) {
        sep = VALUE_SLICE_GET(a, from);
        lim = VALUE_SLICE_GET(a, from + 1);
    }
    if (lim != KORB_NIL) {
        korb_sword_t n2;
        if (UNLIKELY(KORB_BIGNUM_P(lim)))
            return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into `long'");
        if (FIXNUM_P(lim)) n2 = FIX2LONG(lim);
        else if (!korb_to_index(lim, &n2)) {
            if (!KORB_OBJECT_P(lim) || !korb_responds_to(c, lim, korb_intern(c->vm, "to_int", 6)))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into Integer", korb_coerce_name(c, lim));
            slots[0] = lim;
            const RESULT ir = korb_send(c, slots + 1, korb_intern(c->vm, "to_int", 6), 0, 0);
            if (UNLIKELY(ir.state != KORB_NORMAL)) return ir;
            if (!FIXNUM_P(ir.value))
                return korb_raise(c, slots, KORB_E_RANGE, 0, "bignum too big to convert into `long'");
            n2 = FIX2LONG(ir.value);
        }
        o->limit = n2 < 0 ? -1 : n2;                                   /* a negative limit means "no limit" */
    }
    if (sep == KORB_NIL) { o->slurp = true; slots[0] = KORB_NIL; return RESULT_OK(KORB_NIL); }
    if (sep == KORB_UNDEF) {                                           /* default: $/ */
        const VALUE rs = korb_const_get(c->vm, korb_intern(c->vm, "$/", 2));
        if (rs == KORB_NIL) { o->slurp = true; slots[0] = KORB_NIL; return RESULT_OK(KORB_NIL); }
        sep = KORB_STRING_P(rs) ? rs : KORB_UNDEF;
        if (sep == KORB_UNDEF) {                                       /* $/ unset → "\n" */
            slots[0] = UNWRAP(korb_str_new(c, slots, "\n", 1));
            return RESULT_OK(slots[0]);
        }
        slots[0] = sep;
        return RESULT_OK(sep);
    }
    if (!KORB_STRING_P(sep)) {                                         /* #to_str */
        slots[0] = sep;
        const RESULT sr = korb_send(c, slots + 1, korb_intern(c->vm, "to_str", 6), 0, 0);
        if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
        if (!KORB_STRING_P(sr.value))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, sep));
        sep = sr.value;
    }
    if (VAL2STR(sep)->len == 0) {                                      /* "" → paragraph mode */
        o->paragraph = true;
        slots[0] = UNWRAP(korb_str_new(c, slots, "\n\n", 2));
        return RESULT_OK(slots[0]);
    }
    slots[0] = sep;
    return RESULT_OK(sep);
}

/* Next record in `buf` from *pos, per `la` and the separator [sep, seplen).
 * false at EOF; otherwise [*rs, *re) is the record (separator and all) and *pos
 * moves past everything consumed — in paragraph mode that includes the run of
 * blank lines after the record, which belongs to no record at all. */
static bool
korb_line_next(const char *buf, size_t len, size_t *pos, const char *sep, size_t seplen,
               const struct korb_line_args *la, size_t *rs, size_t *re)
{
    size_t i = *pos;
    if (la->paragraph) while (i < len && buf[i] == '\n') i++;
    if (i >= len) { *pos = len; return false; }
    size_t end;
    bool matched = false;
    if (la->slurp) end = len;
    else {
        const char *const h = memmem(buf + i, len - i, sep, seplen);
        if (h) { end = (size_t)(h - buf) + seplen; matched = true; }
        else end = len;
    }
    if (la->limit >= 0 && end - i > (size_t)la->limit) { end = i + (size_t)la->limit; matched = false; }
    *rs = i; *re = end; *pos = end;
    if (matched && la->paragraph) while (*pos < len && buf[*pos] == '\n') (*pos)++;
    if (la->chomp && !la->slurp) {                     /* drop the separator we matched */
        if (la->paragraph) { while (*re > *rs && buf[*re - 1] == '\n') (*re)--; }
        else if (matched) {
            *re -= seplen;
            if (seplen == 1 && sep[0] == '\n' && *re > *rs && buf[*re - 1] == '\r') (*re)--;
        }
    }
    return true;
}


/* File.readlines(path, sep = $/, limit = nil, chomp: false) → the records. */
static RESULT korb_m_file_readlines(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    slots[0] = pv;                                     /* park: parsing the arguments dispatches */
    struct korb_line_args la;
    CHECK(korb_io_line_args(c, slots + 1, a, 1, &la));  /* separator → slots[1] */
    if (UNLIKELY(la.limit == 0))
        return korb_raise(c, slots + 2, KORB_E_ARGUMENT, 0, "invalid limit: 0 for readlines");
    const VALUE_REF sepref = VALUE_REF_AT(&slots[1]);
    uint32_t plen; const char *const path = korb_str_cstr_len(slots[0], &plen);   /* re-read: it may have moved */
    size_t len; char *const buf = korb_file_slurp(path, &len);                    /* libc only, no GC */
    if (!buf) return korb_raise_errno(c, slots + 2, errno, "rb_sysopen", path);
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 16));
    const VALUE_REF arr = VALUE_REF_AT(&slots[2]);
    RESULT r = RESULT_OK(KORB_NIL);
    size_t pos = 0, rs, re;
    /* the separator's bytes are re-read every pass: pushing a String can GC */
    while (korb_line_next(buf, len, &pos,
                          la.slurp ? "" : korb_strbuf_data(VAL2STR(VALUE_REF_GET(sepref))->buf),
                          la.slurp ? 0 : VAL2STR(VALUE_REF_GET(sepref))->len, &la, &rs, &re)) {
        slots[3] = UNWRAP(korb_str_new(c, slots + 3, buf + rs, (uint32_t)(re - rs)));
        r = korb_ary_push_val(c, slots + 4, arr, slots[3]);
        if (UNLIKELY(r.state != KORB_NORMAL)) break;
    }
    free(buf);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(VALUE_REF_GET(arr));
}

/* File.foreach(path, sep = $/, limit = nil, chomp: false) { |rec| … } → nil. */
static RESULT korb_m_file_foreach(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                  struct Node *block, VALUE *def_env, VALUE *captured_self) {
    (void)self;
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    slots[0] = pv;                                     /* park: parsing the arguments dispatches */
    struct korb_line_args la;
    CHECK(korb_io_line_args(c, slots + 1, a, 1, &la));  /* separator → slots[1] */
    if (UNLIKELY(la.limit == 0))
        return korb_raise(c, slots + 2, KORB_E_ARGUMENT, 0, "invalid limit: 0 for foreach");
    const VALUE_REF sepref = VALUE_REF_AT(&slots[1]);
    uint32_t plen; const char *const path = korb_str_cstr_len(slots[0], &plen);   /* re-read: it may have moved */
    size_t len; char *const buf = korb_file_slurp(path, &len);                    /* libc only, no GC */
    if (!buf) return korb_raise_errno(c, slots + 2, errno, "rb_sysopen", path);
    slots[2] = KORB_NIL;
    VALUE_REF arr = VALUE_REF_AT(&slots[2]);
    if (block == NULL) {                               /* no block → an Enumerator of the records */
        slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 16));
        arr = VALUE_REF_AT(&slots[2]);
    }
    RESULT r = RESULT_OK(KORB_NIL);
    size_t pos = 0, rs, re;
    uint32_t lineno = 0;
    while (korb_line_next(buf, len, &pos,
                          la.slurp ? "" : korb_strbuf_data(VAL2STR(VALUE_REF_GET(sepref))->buf),
                          la.slurp ? 0 : VAL2STR(VALUE_REF_GET(sepref))->len, &la, &rs, &re)) {
        slots[3] = UNWRAP(korb_str_new(c, slots + 3, buf + rs, (uint32_t)(re - rs)));
        if (block) korb_const_define(c, korb_intern(c->vm, "$.", 2), LONG2FIX((korb_sword_t)++lineno));   /* CRuby updates $. per yield */
        r = block ? korb_block_yield(c, slots + 4, block, def_env, &slots[3], 1, captured_self)
                  : korb_ary_push_val(c, slots + 4, arr, slots[3]);
        if (UNLIKELY(r.state != KORB_NORMAL)) break;
    }
    free(buf);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (block) korb_const_define(c, korb_intern(c->vm, "$_", 2), KORB_NIL);   /* foreach leaves $_ nil */
    if (block == NULL) {
        const RESULT er = korb_enum_new(c, slots + 3, VALUE_REF_GET(arr), KORB_NIL);
        if (LIKELY(er.state == KORB_NORMAL)) VAL2ENUM(er.value)->size_unknown = 1;   /* CRuby: #size is nil */
        return er;
    }
    return RESULT_OK(KORB_NIL);
}

/* File.delete(*paths) / File.unlink → number removed. */
static RESULT korb_m_file_delete(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    korb_sword_t cnt = 0;
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        const VALUE pv = VALUE_SLICE_GET(a, i);
        if (UNLIKELY(!KORB_STRING_P(pv)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
        uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
        if (unlink(path) != 0)   /* CRuby raises Errno::ENOENT; koruby has no Errno, so RuntimeError (still a StandardError) */
            return korb_raise_errno(c, slots, errno, "apply2files", path);
        cnt++;
    }
    return RESULT_OK(LONG2FIX(cnt));
}

/* copy a String arg into a stack buffer (path args are movable interior ptrs). */
static bool korb_file_pathbuf(VALUE v, char *buf, size_t bufsz) {
    if (!KORB_STRING_P(v)) return false;
    uint32_t l; const char *p = korb_str_cstr_len(v, &l);
    if (l >= bufsz) l = (uint32_t)bufsz - 1;
    memcpy(buf, p, l); buf[l] = '\0';
    return true;
}
/* File.link(old, new) → 0 (hard link). */
static RESULT korb_m_file_link(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; char ob[4096], nb[4096];
    if (!korb_file_pathbuf(VALUE_SLICE_GET(a, 0), ob, sizeof ob) || !korb_file_pathbuf(VALUE_SLICE_GET(a, 1), nb, sizeof nb))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    if (link(ob, nb) != 0) return korb_raise_errno(c, slots, errno, "link", ob);
    return RESULT_OK(LONG2FIX(0));
}
/* File.symlink(old, new) → 0 (symbolic link). */
static RESULT korb_m_file_symlink(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; char ob[4096], nb[4096];
    if (!korb_file_pathbuf(VALUE_SLICE_GET(a, 0), ob, sizeof ob) || !korb_file_pathbuf(VALUE_SLICE_GET(a, 1), nb, sizeof nb))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    if (symlink(ob, nb) != 0) return korb_raise_errno(c, slots, errno, "symlink", ob);
    return RESULT_OK(LONG2FIX(0));
}
/* File.readlink(path) → the symlink's target. */
static RESULT korb_m_file_readlink(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; char pb[4096], buf[4096];
    if (!korb_file_pathbuf(VALUE_SLICE_GET(a, 0), pb, sizeof pb))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    const ssize_t n = readlink(pb, buf, sizeof buf - 1);
    if (n < 0) return korb_raise_errno(c, slots, errno, "readlink", pb);
    return korb_str_new(c, slots, buf, (uint32_t)n);
}
/* --- File::Stat --- a stat(2)/lstat(2) result.  Fields are stored as ivars at
 * construction (numeric fields as Fixnums, the three times as epoch seconds);
 * methods read them back.  No live handle, so nothing to GC-scan. */
static VALUE korb_stat_iv(CTX *c, const char *n) { return ID2SYM(korb_intern(c->vm, n, (uint32_t)strlen(n))); }
/* `path` (may be NULL) is remembered so #birthtime can re-query it via statx. */
static RESULT korb_stat_make_path(CTX *c, VALUE *slots, const struct stat *st, const char *path) {
    const VALUE cls = korb_const_get(c->vm, korb_intern(c->vm, "Stat", 4));
    if (!KORB_CLASS_P(cls)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "File::Stat is not defined");
    slots[0] = UNWRAP(korb_obj_new(c, slots, cls));
    VALUE_REF o = VALUE_REF_AT(&slots[0]);
    #define SETI(nm, v) CHECK(korb_ivar_set(c, slots + 1, o, korb_stat_iv(c, nm), LONG2FIX((korb_sword_t)(v))))
    SETI("@__size", st->st_size);   SETI("@__mode", st->st_mode);   SETI("@__ino", st->st_ino);
    SETI("@__dev",  st->st_dev);    SETI("@__nlink", st->st_nlink); SETI("@__uid", st->st_uid);
    SETI("@__gid",  st->st_gid);    SETI("@__blksize", st->st_blksize); SETI("@__blocks", st->st_blocks);
    SETI("@__rdev", st->st_rdev);   SETI("@__mtime", st->st_mtime); SETI("@__atime", st->st_atime);
    SETI("@__ctime", st->st_ctime);
    #undef SETI
    if (path != NULL) {
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, path, (uint32_t)strlen(path)));
        CHECK(korb_ivar_set(c, slots + 2, o, korb_stat_iv(c, "@__path"), slots[1]));
    }
    return RESULT_OK(VALUE_REF_GET(o));
}
static RESULT korb_stat_make(CTX *c, VALUE *slots, const struct stat *st) {
    return korb_stat_make_path(c, slots, st, NULL);
}
static korb_sword_t korb_stat_field(CTX *c, VALUE self, const char *nm) {
    const VALUE v = korb_ivar_get(c, self, korb_stat_iv(c, nm));
    return FIXNUM_P(v) ? FIX2LONG(v) : 0;
}
#define STAT_INT_M(fn, field) static RESULT fn(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { \
    (void)slots; (void)a; return RESULT_OK(LONG2FIX(korb_stat_field(c, VALUE_REF_GET(self), field))); }
STAT_INT_M(korb_m_stat_size,    "@__size")
STAT_INT_M(korb_m_stat_mode,    "@__mode")
STAT_INT_M(korb_m_stat_ino,     "@__ino")
STAT_INT_M(korb_m_stat_dev,     "@__dev")
STAT_INT_M(korb_m_stat_nlink,   "@__nlink")
STAT_INT_M(korb_m_stat_uid,     "@__uid")
STAT_INT_M(korb_m_stat_gid,     "@__gid")
STAT_INT_M(korb_m_stat_blksize, "@__blksize")
STAT_INT_M(korb_m_stat_blocks,  "@__blocks")
STAT_INT_M(korb_m_stat_rdev,    "@__rdev")
#undef STAT_INT_M
#define STAT_TIME_M(fn, field) static RESULT fn(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { \
    (void)a; const korb_sword_t t = korb_stat_field(c, VALUE_REF_GET(self), field); \
    return korb_time_make(c, slots, korb_const_get(c->vm, korb_intern(c->vm, "Time", 4)), (double)t, false); }
STAT_TIME_M(korb_m_stat_mtime, "@__mtime")
STAT_TIME_M(korb_m_stat_atime, "@__atime")
STAT_TIME_M(korb_m_stat_ctime, "@__ctime")
#undef STAT_TIME_M
#define STAT_PRED_M(fn, expr) static RESULT fn(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { \
    (void)slots; (void)a; const mode_t m = (mode_t)korb_stat_field(c, VALUE_REF_GET(self), "@__mode"); \
    return RESULT_OK((expr) ? KORB_TRUE : KORB_FALSE); }
STAT_PRED_M(korb_m_stat_dir_p,   S_ISDIR(m))
STAT_PRED_M(korb_m_stat_file_p,  S_ISREG(m))
STAT_PRED_M(korb_m_stat_link_p,  S_ISLNK(m))
STAT_PRED_M(korb_m_stat_sock_p,  S_ISSOCK(m))
STAT_PRED_M(korb_m_stat_blk_p,   S_ISBLK(m))
STAT_PRED_M(korb_m_stat_chr_p,   S_ISCHR(m))
STAT_PRED_M(korb_m_stat_pipe_p,  S_ISFIFO(m))
STAT_PRED_M(korb_m_stat_setuid_p, (m & S_ISUID) != 0)
STAT_PRED_M(korb_m_stat_setgid_p, (m & S_ISGID) != 0)
STAT_PRED_M(korb_m_stat_sticky_p, (m & S_ISVTX) != 0)
STAT_PRED_M(korb_m_stat_wreadable_p, (m & S_IROTH) != 0)
STAT_PRED_M(korb_m_stat_wwritable_p, (m & S_IWOTH) != 0)
#undef STAT_PRED_M
static RESULT korb_m_stat_zero_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a; return RESULT_OK(korb_stat_field(c, VALUE_REF_GET(self), "@__size") == 0 ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_stat_owned_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a; return RESULT_OK(korb_stat_field(c, VALUE_REF_GET(self), "@__uid") == (korb_sword_t)geteuid() ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_stat_grouped_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a; return RESULT_OK(korb_stat_field(c, VALUE_REF_GET(self), "@__gid") == (korb_sword_t)getegid() ? KORB_TRUE : KORB_FALSE);
}
/* Permission predicates.  CRuby answers them from the cached stat fields, not
 * from access(2): pick the owner / group / other triad by comparing the file's
 * uid+gid against the process's effective (or real, for the *_real? variants)
 * ids, then test the requested bits.  root reads and writes anything, and can
 * execute a file with any x bit set. */
static bool korb_stat_permits(CTX *c, VALUE self, mode_t owner_bit, mode_t group_bit, mode_t other_bit, bool real_ids) {
    const uid_t uid = real_ids ? getuid() : geteuid();
    const gid_t gid = real_ids ? getgid() : getegid();
    const mode_t m = (mode_t)korb_stat_field(c, self, "@__mode");
    if (uid == 0) {                                   /* root */
        if (owner_bit == S_IXUSR) return (m & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
        return true;
    }
    if ((uid_t)korb_stat_field(c, self, "@__uid") == uid) return (m & owner_bit) != 0;
    if ((gid_t)korb_stat_field(c, self, "@__gid") == gid) return (m & group_bit) != 0;
    {   /* supplementary groups count too */
        gid_t gs[64];
        const int n = getgroups((int)(sizeof gs / sizeof gs[0]), gs);
        const gid_t fgid = (gid_t)korb_stat_field(c, self, "@__gid");
        for (int i = 0; i < n; i++) if (gs[i] == fgid) return (m & group_bit) != 0;
    }
    return (m & other_bit) != 0;
}
#define STAT_PERM_M(fn, ob, gb, xb, real) static RESULT fn(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { \
    (void)slots; (void)a; \
    return RESULT_OK(korb_stat_permits(c, VALUE_REF_GET(self), ob, gb, xb, real) ? KORB_TRUE : KORB_FALSE); }
STAT_PERM_M(korb_m_stat_readable_p,        S_IRUSR, S_IRGRP, S_IROTH, false)
STAT_PERM_M(korb_m_stat_readable_real_p,   S_IRUSR, S_IRGRP, S_IROTH, true)
STAT_PERM_M(korb_m_stat_writable_p,        S_IWUSR, S_IWGRP, S_IWOTH, false)
STAT_PERM_M(korb_m_stat_writable_real_p,   S_IWUSR, S_IWGRP, S_IWOTH, true)
STAT_PERM_M(korb_m_stat_executable_p,      S_IXUSR, S_IXGRP, S_IXOTH, false)
STAT_PERM_M(korb_m_stat_executable_real_p, S_IXUSR, S_IXGRP, S_IXOTH, true)
#undef STAT_PERM_M
/* #size? — the size, or nil for an empty file (so `if stat.size?` works). */
static RESULT korb_m_stat_size_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a;
    const korb_sword_t sz = korb_stat_field(c, VALUE_REF_GET(self), "@__size");
    return RESULT_OK(sz == 0 ? KORB_NIL : LONG2FIX(sz));
}
#define STAT_DEVPART_M(fn, field, part) static RESULT fn(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { \
    (void)slots; (void)a; const dev_t d = (dev_t)korb_stat_field(c, VALUE_REF_GET(self), field); \
    return RESULT_OK(LONG2FIX((korb_sword_t)part(d))); }
STAT_DEVPART_M(korb_m_stat_dev_major,  "@__dev",  major)
STAT_DEVPART_M(korb_m_stat_dev_minor,  "@__dev",  minor)
STAT_DEVPART_M(korb_m_stat_rdev_major, "@__rdev", major)
STAT_DEVPART_M(korb_m_stat_rdev_minor, "@__rdev", minor)
#undef STAT_DEVPART_M
/* Birth ("creation") time.  stat(2) has no such field on Linux; statx(2) does,
 * but only some filesystems fill it in.  Kernel/filesystem without it →
 * NotImplementedError, exactly as CRuby reports. */
static RESULT korb_birthtime_of(CTX *c, VALUE *slots, const char *path) {
#if defined(__linux__) && defined(STATX_BTIME)
    struct statx stx;
    if (statx(AT_FDCWD, path, 0, STATX_BTIME, &stx) == 0 && (stx.stx_mask & STATX_BTIME))
        return korb_time_make(c, slots, korb_const_get(c->vm, korb_intern(c->vm, "Time", 4)),
                              (double)stx.stx_btime.tv_sec + (double)stx.stx_btime.tv_nsec / 1e9, false);
#else
    (void)path;
#endif
    return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "birthtime() function is unimplemented on this machine");
}
/* File::Stat#birthtime — the stat was taken from a path we no longer hold, so
 * re-query it by the path recorded at construction (absent → unimplemented). */
static RESULT korb_m_stat_birthtime(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const VALUE pv = korb_ivar_get(c, VALUE_REF_GET(self), korb_stat_iv(c, "@__path"));
    char pb[4096];
    if (!KORB_STRING_P(pv) || !korb_file_pathbuf(pv, pb, sizeof pb))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "birthtime() function is unimplemented on this machine");
    return korb_birthtime_of(c, slots, pb);
}
static RESULT korb_m_stat_ftype(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; const mode_t m = (mode_t)korb_stat_field(c, VALUE_REF_GET(self), "@__mode");
    const char *t = S_ISDIR(m) ? "directory" : S_ISCHR(m) ? "characterSpecial" : S_ISBLK(m) ? "blockSpecial"
                  : S_ISFIFO(m) ? "fifo" : S_ISLNK(m) ? "link" : S_ISSOCK(m) ? "socket" : S_ISREG(m) ? "file" : "unknown";
    return korb_str_new(c, slots, t, (uint32_t)strlen(t));
}
static RESULT korb_m_stat_spaceship(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; const VALUE o = VALUE_SLICE_GET(a, 0);
    if (!KORB_OBJECT_P(o)) return RESULT_OK(KORB_NIL);
    const korb_sword_t m1 = korb_stat_field(c, VALUE_REF_GET(self), "@__mtime"), m2 = korb_stat_field(c, o, "@__mtime");
    return RESULT_OK(LONG2FIX(m1 < m2 ? -1 : m1 > m2 ? 1 : 0));
}
/* A path argument: a String as is, otherwise #to_path then #to_str (CRuby's
 * FilePathValue).  Dispatches → may GC, so the caller must re-read its VALUEs. */
static RESULT korb_file_path_arg(CTX *c, VALUE *slots, VALUE *v) {
    if (LIKELY(KORB_STRING_P(*v))) return RESULT_OK(KORB_TRUE);
    const char *const cls = korb_type_name(*v);            /* capture before dispatch */
    static const char *const conv[2] = { "to_path", "to_str" };
    static const uint32_t convlen[2] = { 7, 6 };
    for (int i = 0; i < 2; i++) {
        VALUE recv = *v;
        const uint32_t mid = korb_intern(c->vm, conv[i], convlen[i]);
        if (!(KORB_OBJECT_P(recv) && korb_responds_to_coerce_p(c, slots, &recv, mid))) continue;
        slots[0] = recv;
        const RESULT pr = korb_send(c, slots + 1, mid, 0, 0);
        if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
        *v = pr.value;
        if (KORB_STRING_P(*v)) return RESULT_OK(KORB_TRUE);
    }
    return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", cls);
}

/* File.stat(path) / File.lstat(path) → a File::Stat. */
static RESULT korb_file_stat_impl(CTX *c, VALUE *slots, VALUE_SLICE a, bool is_l) {
    char pb[4096];
    VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv))) { CHECK(korb_file_path_arg(c, slots, &pv)); slots[0] = pv; }
    if (!korb_file_pathbuf(pv, pb, sizeof pb))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    struct stat st;
    if ((is_l ? lstat(pb, &st) : stat(pb, &st)) != 0) return korb_raise_errno(c, slots, errno, is_l ? "lstat" : "stat", pb);
    return korb_stat_make_path(c, slots, &st, pb);
}
static RESULT korb_m_file_stat(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)self; return korb_file_stat_impl(c, slots, a, false); }
static RESULT korb_m_file_lstat(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self; return korb_file_stat_impl(c, slots, a, true); }
/* File.atime/ctime/mtime(path) → the corresponding Time (via stat). */
static RESULT korb_file_time_impl(CTX *c, VALUE *slots, VALUE_SLICE a, int which) {
    char pb[4096];
    if (!korb_file_pathbuf(VALUE_SLICE_GET(a, 0), pb, sizeof pb))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    struct stat st;
    if (stat(pb, &st) != 0) return korb_raise_errno(c, slots, errno, "stat", pb);
    const time_t t = which == 0 ? st.st_atime : which == 1 ? st.st_ctime : st.st_mtime;
    return korb_time_make(c, slots, korb_const_get(c->vm, korb_intern(c->vm, "Time", 4)), (double)t, false);
}
/* File.truncate(path, len) → 0 (resize the file). */
static RESULT korb_m_file_truncate(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; char pb[4096];
    if (!korb_file_pathbuf(VALUE_SLICE_GET(a, 0), pb, sizeof pb))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    const VALUE lv = VALUE_SLICE_GET(a, 1);
    if (!FIXNUM_P(lv)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    if (truncate(pb, (off_t)FIX2LONG(lv)) != 0) return korb_raise_errno(c, slots, errno, "truncate", pb);
    return RESULT_OK(LONG2FIX(0));
}
/* File.absolute_path?(path) → true iff `path` is an absolute path. */
static RESULT korb_m_file_abs_path_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; (void)slots;
    const VALUE pv = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(pv)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    const KorbString *const s = VAL2STR(pv);
    return RESULT_OK((s->len > 0 && korb_strbuf_data(s->buf)[0] == '/') ? KORB_TRUE : KORB_FALSE);
}
/* File.birthtime(path) — statx's birth time (NotImplementedError without it). */
static RESULT korb_m_file_birthtime(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; char pb[4096];
    VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv))) { CHECK(korb_file_path_arg(c, slots, &pv)); slots[0] = pv; }
    if (!korb_file_pathbuf(pv, pb, sizeof pb))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    struct stat st;                                    /* ENOENT etc. surface as CRuby's errno */
    if (stat(pb, &st) != 0) return korb_raise_errno(c, slots, errno, "rb_file_s_birthtime", pb);
    return korb_birthtime_of(c, slots + 1, pb);
}
static RESULT korb_m_file_atime(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self; return korb_file_time_impl(c, slots, a, 0); }
static RESULT korb_m_file_ctime(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self; return korb_file_time_impl(c, slots, a, 1); }
static RESULT korb_m_file_mtime(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self; return korb_file_time_impl(c, slots, a, 2); }

/* Dir.pwd → getcwd; Dir.exist?(path) → stat + S_ISDIR. */
static RESULT korb_m_dir_pwd(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; (void)a;
    char buf[8192];
    if (!getcwd(buf, sizeof buf)) return RESULT_OK(KORB_NIL);
    return korb_str_new(c, slots, buf, (uint32_t)strlen(buf));
}
static RESULT korb_m_dir_exist_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; return korb_m_file_stat_pred(c, slots, a, 2);
}
/* the sole String path arg as a NUL-terminated cstr (TypeError otherwise). */
ARO_BORROW static const char *korb_path_arg(CTX *c, VALUE *slots, VALUE_SLICE a, RESULT *err) {
    const RESULT pr = korb_path_coerce(c, slots, a, 0);   /* String / #to_path / #to_str */
    if (UNLIKELY(pr.state != KORB_NORMAL)) { *err = pr; return NULL; }
    err->state = KORB_NORMAL; uint32_t plen; return korb_str_cstr_len(pr.value, &plen);
}
/* Dir.mkdir(path[, mode]) → 0 (raises on failure). */
static RESULT korb_m_dir_mkdir(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    RESULT err; const char *path = korb_path_arg(c, slots, a, &err); if (!path) return err;
    long mode = 0777;
    if (VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1))) mode = (long long)FIX2LONG(VALUE_SLICE_GET(a, 1));
    if (mkdir(path, (mode_t)mode) != 0) return korb_raise_errno(c, slots, errno, "dir_s_mkdir", path);
    return RESULT_OK(LONG2FIX(0));
}
/* Dir.rmdir(path) → 0 (raises on failure). */
static RESULT korb_m_dir_rmdir(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    RESULT err; const char *path = korb_path_arg(c, slots, a, &err); if (!path) return err;
    if (rmdir(path) != 0) return korb_raise_errno(c, slots, errno, "dir_s_rmdir", path);
    return RESULT_OK(LONG2FIX(0));
}
/* Dir.entries(path) [with_dots] / Dir.children(path) [without]. */
static RESULT korb_dir_list(CTX *c, VALUE *slots, VALUE_SLICE a, bool with_dots) {
    RESULT err; const char *path = korb_path_arg(c, slots, a, &err); if (!path) return err;
    DIR *d = opendir(path);
    if (!d) return korb_raise_errno(c, slots, errno, "opendir", path);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 16));
    VALUE_REF arr = VALUE_REF_AT(&slots[0]);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!with_dots && (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)) continue;
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, ent->d_name, (uint32_t)strlen(ent->d_name)));
        if (korb_ary_push_val(c, slots + 2, arr, slots[1]).state != KORB_NORMAL) break;
    }
    closedir(d);
    return RESULT_OK(VALUE_REF_GET(arr));
}
static RESULT korb_m_dir_entries(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)  { (void)self; return korb_dir_list(c, slots, a, true); }
static RESULT korb_m_dir_children(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self; return korb_dir_list(c, slots, a, false); }

/* --- Dir instance objects (Dir.open / Dir.new / #read / #each / …) ---
 * A Dir reads all its entries eagerly at open into an @__dir_entries array and
 * keeps a cursor @__dir_pos; @__dir_path holds the path.  No live DIR* handle,
 * so nothing to GC-scan and #close is a no-op. */
static uint32_t korb_dir_ents_id(CTX *c) { return korb_intern(c->vm, "__dir_entries", 13); }
static uint32_t korb_dir_pos_id(CTX *c)  { return korb_intern(c->vm, "__dir_pos", 9); }
static uint32_t korb_dir_path_id(CTX *c) { return korb_intern(c->vm, "__dir_path", 10); }
/* Build a Dir instance over `path` (rooted in the caller's slots on return). */
static RESULT korb_dir_make(CTX *c, VALUE *slots, const char *path, uint32_t plen) {
    DIR *d = opendir(path);
    if (!d) return korb_raise_errno(c, slots, errno, "opendir", path);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 16));            /* entries (rooted) */
    { VALUE_REF arr = VALUE_REF_AT(&slots[0]); struct dirent *ent;
      while ((ent = readdir(d)) != NULL) {
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, ent->d_name, (uint32_t)strlen(ent->d_name)));
        if (korb_ary_push_val(c, slots + 2, arr, slots[1]).state != KORB_NORMAL) break;
      } }
    closedir(d);
    slots[1] = UNWRAP(korb_str_new(c, slots + 2, path, plen));   /* path (rooted) */
    const VALUE dcls = korb_const_get(c->vm, korb_intern(c->vm, "Dir", 3));
    slots[2] = UNWRAP(korb_obj_new(c, slots + 3, dcls));         /* the Dir (rooted) */
    VALUE_REF obj = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ivar_set(c, slots + 3, obj, ID2SYM(korb_dir_ents_id(c)), slots[0]));
    CHECK(korb_ivar_set(c, slots + 3, obj, ID2SYM(korb_dir_path_id(c)), slots[1]));
    CHECK(korb_ivar_set(c, slots + 3, obj, ID2SYM(korb_dir_pos_id(c)),  LONG2FIX(0)));
    return RESULT_OK(VALUE_REF_GET(obj));
}
/* Dir.new(path) / Dir.open(path) [ { |dir| } ] */
static RESULT korb_m_dir_open(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                              struct Node *block, VALUE *def_env, VALUE *captured_self) {
    (void)self;
    RESULT err; const char *path = korb_path_arg(c, slots, a, &err); if (!path) return err;
    char pbuf[4096]; size_t pl = strlen(path); if (pl >= sizeof pbuf) pl = sizeof pbuf - 1;
    memcpy(pbuf, path, pl); pbuf[pl] = '\0';                     /* path is a movable interior ptr */
    slots[0] = UNWRAP(korb_dir_make(c, slots, pbuf, (uint32_t)pl));
    VALUE_REF dir = VALUE_REF_AT(&slots[0]);
    if (block == NULL) return RESULT_OK(VALUE_REF_GET(dir));
    slots[1] = VALUE_REF_GET(dir);                               /* Dir.open(block) → block value, dir "closed" after */
    RESULT br = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, captured_self);
    return br;
}
static const KorbArray *korb_dir_ents(CTX *c, VALUE self) { const VALUE e = korb_ivar_get(c, self, ID2SYM(korb_dir_ents_id(c))); return KORB_ARRAY_P(e) ? VAL2ARY(e) : NULL; }
/* Dir#read → next entry name (advancing the cursor), or nil at end. */
static RESULT korb_m_dir_read(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; const VALUE s = VALUE_REF_GET(self);
    const KorbArray *ents = korb_dir_ents(c, s); if (!ents) return RESULT_OK(KORB_NIL);
    const VALUE posv = korb_ivar_get(c, s, ID2SYM(korb_dir_pos_id(c)));
    const korb_sword_t pos = FIXNUM_P(posv) ? FIX2LONG(posv) : 0;
    if (pos < 0 || (uint32_t)pos >= ents->len) return RESULT_OK(KORB_NIL);
    slots[0] = korb_items_data(ents->items)[pos];                          /* the entry (rooted) */
    CHECK(korb_ivar_set(c, slots + 1, self, ID2SYM(korb_dir_pos_id(c)), LONG2FIX(pos + 1)));
    return RESULT_OK(slots[0]);
}
/* Dir#each { |name| } → self (Enumerator over entries if no block). */
static RESULT korb_m_dir_each(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                              struct Node *block, VALUE *def_env, VALUE *captured_self) {
    (void)a; const VALUE s = VALUE_REF_GET(self);
    const VALUE ev = korb_ivar_get(c, s, ID2SYM(korb_dir_ents_id(c)));
    if (block == NULL) return korb_enum_new(c, slots, KORB_ARRAY_P(ev) ? ev : KORB_NIL, KORB_NIL);
    slots[0] = KORB_ARRAY_P(ev) ? ev : KORB_NIL;                 /* root the entries array */
    if (!KORB_ARRAY_P(slots[0])) return RESULT_OK(VALUE_REF_GET(self));
    const uint32_t n = VAL2ARY(slots[0])->len;
    for (uint32_t i = 0; i < n; i++) {
        slots[1] = korb_items_data(VAL2ARY(slots[0])->items)[i];           /* re-read: yield may GC */
        RESULT r = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, captured_self);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_dir_path(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a; return RESULT_OK(korb_ivar_get(c, VALUE_REF_GET(self), ID2SYM(korb_dir_path_id(c))));
}
static RESULT korb_m_dir_pos(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a; const VALUE p = korb_ivar_get(c, VALUE_REF_GET(self), ID2SYM(korb_dir_pos_id(c)));
    return RESULT_OK(FIXNUM_P(p) ? p : LONG2FIX(0));
}
static RESULT korb_m_dir_rewind(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a; CHECK(korb_ivar_set(c, slots, self, ID2SYM(korb_dir_pos_id(c)), LONG2FIX(0)));
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_dir_seek(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE nv = VALUE_SLICE_GET(a, 0);
    CHECK(korb_ivar_set(c, slots, self, ID2SYM(korb_dir_pos_id(c)), FIXNUM_P(nv) ? nv : LONG2FIX(0)));
    return RESULT_OK(VALUE_REF_GET(self));
}
static RESULT korb_m_dir_pos_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    const VALUE nv = VALUE_SLICE_GET(a, 0);
    CHECK(korb_ivar_set(c, slots, self, ID2SYM(korb_dir_pos_id(c)), FIXNUM_P(nv) ? nv : LONG2FIX(0)));
    return RESULT_OK(nv);
}
static RESULT korb_m_dir_close(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots; (void)self; (void)a; return RESULT_OK(KORB_NIL);   /* entries already read; no handle */
}
/* Dir#children / #each_child — entries excluding "." and "..". */
static RESULT korb_m_dir_i_children(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)a;
    const KorbArray *ents = korb_dir_ents(c, VALUE_REF_GET(self));
    const uint32_t n = ents ? ents->len : 0;                 /* read len BEFORE korb_ary_new GCs `ents` away */
    slots[0] = UNWRAP(korb_ary_new(c, slots, n));
    if (n == 0) return RESULT_OK(slots[0]);
    VALUE_REF out = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < n; i++) {
        const KorbArray *e = korb_dir_ents(c, VALUE_REF_GET(self));   /* re-read: push GCs */
        const VALUE nm = korb_items_data(e->items)[i];
        if (KORB_STRING_P(nm)) { const KorbString *s = VAL2STR(nm);
            if ((s->len == 1 && korb_strbuf_data(s->buf)[0] == '.') || (s->len == 2 && korb_strbuf_data(s->buf)[0] == '.' && korb_strbuf_data(s->buf)[1] == '.')) continue; }
        slots[1] = nm; CHECK(korb_ary_push_val(c, slots + 2, out, slots[1]));
    }
    return RESULT_OK(VALUE_REF_GET(out));
}
/* ---- Dir.glob --------------------------------------------------------------
 *
 * A directory walk rather than glob(3): Ruby's `**` (recursive), its dotfile
 * rules and the `base:` option have no POSIX equivalent.  The pattern is split
 * on '/' into segments matched one directory level at a time; a "**" segment
 * that is followed by a '/' consumes zero or more directory levels.
 *
 * Results are relative to `base` (the process cwd when there is none), which is
 * what CRuby returns, so the walk keeps the result path and the filesystem path
 * separate. */
struct korb_glob {
    CTX *c;
    VALUE_REF arr;                     /* the result array (rooted by the caller) */
    int fnm;                           /* fnmatch(3) flags */
    bool dotmatch;
    const char *base;                  /* "" = the process cwd */
};

/* the filesystem path for a result path (which is relative to base) */
static void korb_glob_fspath(const struct korb_glob *g, const char *rel, char *out, size_t outsz) {
    if (g->base[0] == '\0') snprintf(out, outsz, "%s", rel[0] ? rel : ".");
    else if (rel[0] == '\0')  snprintf(out, outsz, "%s", g->base);
    else                      snprintf(out, outsz, "%s/%s", g->base, rel);
}

static bool korb_glob_isdir(const struct korb_glob *g, const char *rel) {
    char fp[PATH_MAX]; korb_glob_fspath(g, rel, fp, sizeof fp);
    struct stat st;
    return stat(fp, &st) == 0 && S_ISDIR(st.st_mode);
}

static RESULT korb_glob_emit(struct korb_glob *g, VALUE *slots, const char *rel, bool dir_only) {
    char buf[PATH_MAX];
    if (dir_only) snprintf(buf, sizeof buf, "%s/", rel);      /* empty rel becomes "./", for a dot double-star */
    else if (rel[0] == '\0') return RESULT_OK(KORB_NIL);
    else snprintf(buf, sizeof buf, "%s", rel);
    if (dir_only && rel[0] == '\0') snprintf(buf, sizeof buf, "./");
    slots[0] = UNWRAP(korb_str_new(g->c, slots, buf, (uint32_t)strlen(buf)));
    return korb_ary_push_val(g->c, slots + 1, g->arr, slots[0]);
}

/* "." and ".." are only ever produced by a segment that spells them out (".*"
 * counts as spelling "." — CRuby returns it, but never ".."). */
static bool korb_glob_skip_dot(const char *name, const char *seg, bool dotmatch) {
    if (name[0] != '.') return false;
    if (name[1] == '\0') return seg[0] != '.' && !dotmatch;             /* "."  */
    if (name[1] == '.' && name[2] == '\0') return strcmp(seg, "..");    /* ".." */
    return false;
}

static int korb_glob_dsel(const struct dirent *d) { (void)d; return 1; }

static RESULT
korb_glob_walk(struct korb_glob *g, VALUE *slots, char *rel, size_t rlen,
               char *const *segs, const bool *slashed, int nseg, int si, bool dir_only)
{
    if (si == nseg) {                                  /* pattern exhausted → a hit */
        if (dir_only && !korb_glob_isdir(g, rel)) return RESULT_OK(KORB_NIL);
        return korb_glob_emit(g, slots, rel, dir_only);
    }
    const char *const seg = segs[si];
    /* a double star recurses only when a '/' follows it; a trailing one is just a star */
    const bool rec = slashed[si] && (!strcmp(seg, "**") || !strcmp(seg, ".**"));
    const bool recdot = rec && seg[0] == '.';
    char fp[PATH_MAX]; korb_glob_fspath(g, rel, fp, sizeof fp);
    struct dirent **ents = NULL;
    const int nent = scandir(fp, &ents, korb_glob_dsel, alphasort);     /* sorted, as CRuby lists */
    RESULT r = RESULT_OK(KORB_NIL);
    if (rec) {
        /* zero directories: the dot form also yields the base itself ("./") */
        /* at the base only the dot form yields "./"; deeper, every level is a hit */
        if (si + 1 < nseg || recdot || rel[0] != '\0')
            r = korb_glob_walk(g, slots, rel, rlen, segs, slashed, nseg, si + 1, dir_only);
        for (int i = 0; nent > 0 && i < nent && r.state == KORB_NORMAL; i++) {
            const char *const nm = ents[i]->d_name;
            if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
            if (!recdot && !g->dotmatch && nm[0] == '.') continue;
            const size_t nl = strlen(nm);
            if (rlen + nl + 2 >= PATH_MAX) continue;
            char sub[PATH_MAX]; struct stat lst;
            snprintf(sub, sizeof sub, "%s/%s", fp, nm);
            if (lstat(sub, &lst) != 0 || !S_ISDIR(lst.st_mode)) continue;   /* lstat: never follow symlinks */
            const size_t save = rlen;
            if (rlen) rel[rlen++] = '/';
            memcpy(rel + rlen, nm, nl); rlen += nl; rel[rlen] = '\0';
            r = korb_glob_walk(g, slots, rel, rlen, segs, slashed, nseg, si, dir_only);   /* still `**` */
            rlen = save; rel[rlen] = '\0';
        }
    } else {
        for (int i = 0; nent > 0 && i < nent && r.state == KORB_NORMAL; i++) {
            const char *const nm = ents[i]->d_name;
            if (korb_glob_skip_dot(nm, seg, g->dotmatch)) continue;
            if (fnmatch(seg, nm, g->fnm) != 0) continue;
            const size_t nl = strlen(nm);
            if (rlen + nl + 2 >= PATH_MAX) continue;
            const size_t save = rlen;
            if (rlen) rel[rlen++] = '/';
            memcpy(rel + rlen, nm, nl); rlen += nl; rel[rlen] = '\0';
            if (si + 1 == nseg || korb_glob_isdir(g, rel))                  /* only a dir can carry more */
                r = korb_glob_walk(g, slots, rel, rlen, segs, slashed, nseg, si + 1, dir_only);
            rlen = save; rel[rlen] = '\0';
        }
    }
    if (nent > 0) { for (int i = 0; i < nent; i++) free(ents[i]); }
    free(ents);
    return r;
}

/* Glob one brace-free pattern.  A leading '/' makes the walk absolute (the base
 * is then the root and the results carry it). */
static RESULT korb_glob_one(struct korb_glob *g, VALUE *slots, const char *pat) {
    char segbuf[PATH_MAX];
    snprintf(segbuf, sizeof segbuf, "%s", pat);
    char *p = segbuf;
    const bool absolute = (p[0] == '/');
    struct korb_glob gl = *g;
    char abase[PATH_MAX];
    if (absolute) {
        while (*p == '/') p++;
        snprintf(abase, sizeof abase, "/");
        gl.base = abase;
    }
    const size_t plen = strlen(p);
    const bool dir_only = plen > 0 && p[plen - 1] == '/';
    char *segs[256]; bool slashed[256];
    int nseg = 0;
    for (char *q = p; *q && nseg < 256; ) {
        char *const sl = strchr(q, '/');
        if (sl) *sl = '\0';
        if (*q) { segs[nseg] = q; slashed[nseg] = (sl != NULL); nseg++; }
        if (!sl) break;
        q = sl + 1;
    }
    if (nseg == 0) return RESULT_OK(KORB_NIL);
    char rel[PATH_MAX]; rel[0] = '\0';
    RESULT r = korb_glob_walk(&gl, slots, rel, 0, segs, slashed, nseg, 0, dir_only);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    if (absolute) {                                    /* re-prefix the '/' the walk dropped */
        const uint32_t n = VAL2ARY(VALUE_REF_GET(g->arr))->len;
        for (uint32_t i = 0; i < n; i++) {
            const VALUE v = korb_items_data(VAL2ARY(VALUE_REF_GET(g->arr))->items)[i];
            if (!KORB_STRING_P(v) || korb_strbuf_data(VAL2STR(v)->buf)[0] == '/') continue;
            char buf[PATH_MAX];
            snprintf(buf, sizeof buf, "/%.*s", (int)VAL2STR(v)->len, korb_strbuf_data(VAL2STR(v)->buf));
            slots[0] = UNWRAP(korb_str_new(g->c, slots, buf, (uint32_t)strlen(buf)));
            ARO_STORE(g->c, VAL2ARY(VALUE_REF_GET(g->arr))->items,
                      &korb_items_data(VAL2ARY(VALUE_REF_GET(g->arr))->items)[i], slots[0]);
        }
    }
    return RESULT_OK(KORB_NIL);
}

/* Expand `{a,b}` left-to-right (CRuby's order) and glob each result. */
static RESULT korb_glob_brace(struct korb_glob *g, VALUE *slots, const char *pat, int depth) {
    const char *ob = NULL;
    int nest = 0;
    for (const char *q = pat; *q; q++) {               /* the left-most outermost brace group */
        if (*q == '\\' && q[1]) { q++; continue; }
        if (*q == '{') { if (nest++ == 0) ob = q; }
        else if (*q == '}' && nest && --nest == 0) {
            char head[PATH_MAX], alt[PATH_MAX], out[PATH_MAX];
            const size_t hl = (size_t)(ob - pat);
            if (hl >= sizeof head || depth > 16) break;
            memcpy(head, pat, hl); head[hl] = '\0';
            const char *const tail = q + 1;
            const char *a = ob + 1;
            int an = 0;
            for (const char *e = a;; e++) {            /* split the group on top-level commas */
                if (*e == '\\' && e[1]) { e++; continue; }
                if (*e == '{') an++;
                else if (*e == '}' && an) an--;
                if ((*e == ',' && an == 0) || e == q) {
                    const size_t al = (size_t)(e - a);
                    if (al < sizeof alt) {
                        memcpy(alt, a, al); alt[al] = '\0';
                        snprintf(out, sizeof out, "%s%s%s", head, alt, tail);
                        CHECK(korb_glob_brace(g, slots, out, depth + 1));
                    }
                    if (e == q) break;
                    a = e + 1;
                }
            }
            return RESULT_OK(KORB_NIL);
        }
    }
    return korb_glob_one(g, slots, pat);
}

/* Dir.glob(pattern | [patterns] [, flags] [, base:, sort:, flags:]) [ { |p| } ]
 * / Dir[...] → the matching paths. */
static RESULT korb_m_dir_glob(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                              struct Node *block, VALUE *def_env, VALUE *captured_self) {
    (void)self;
    uint32_t na = VALUE_SLICE_LEN(a);
    long rflags = 0;
    char basebuf[PATH_MAX]; basebuf[0] = '\0';
    if (na >= 1 && KORB_HASH_P(VALUE_SLICE_GET(a, na - 1))) {          /* base: / sort: / flags: */
        const KorbHash *const h = VAL2HASH(VALUE_SLICE_GET(a, na - 1));
        int32_t ix = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "sort", 4)));
        if (ix >= 0) {
            const VALUE sv = korb_items_data(h->items)[2 * ix + 1];
            if (UNLIKELY(sv != KORB_TRUE && sv != KORB_FALSE)) {
                char *ib = NULL; size_t il = 0;
                FILE *const ms = open_memstream(&ib, &il);
                if (ms) { korb_fprint_inspect_s(c, slots, ms, sv); fclose(ms); }
                const RESULT er = korb_raise(c, slots, KORB_E_ARGUMENT, 0, "expected true or false as sort: %s", ib ? ib : "");
                free(ib);
                return er;
            }
        }
        ix = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "flags", 5)));
        if (ix >= 0) {
            const VALUE fv = korb_items_data(h->items)[2 * ix + 1];
            if (FIXNUM_P(fv)) rflags = FIX2LONG(fv);
        } else if (na >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1))) {
            rflags = FIX2LONG(VALUE_SLICE_GET(a, 1));
        }
        ix = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "base", 4)));
        if (ix >= 0) {
            VALUE bv = korb_items_data(h->items)[2 * ix + 1];
            if (bv != KORB_NIL) {
                if (!KORB_STRING_P(bv)) {
                    slots[0] = bv;
                    const uint32_t to_path = korb_intern(c->vm, "to_path", 7);
                    const uint32_t mid = korb_responds_to(c, bv, to_path) ? to_path : korb_intern(c->vm, "to_str", 6);
                    if (!korb_responds_to(c, bv, mid))
                        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, bv));
                    const RESULT br = korb_send(c, slots + 1, mid, 0, 0);
                    if (UNLIKELY(br.state != KORB_NORMAL)) return br;
                    bv = br.value;
                    if (!KORB_STRING_P(bv))
                        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
                }
                if (VAL2STR(bv)->len > 0)
                    snprintf(basebuf, sizeof basebuf, "%.*s", (int)VAL2STR(bv)->len, korb_strbuf_data(VAL2STR(bv)->buf));
            }
        }
        na--;
    } else if (na >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1))) {
        rflags = FIX2LONG(VALUE_SLICE_GET(a, 1));
    }
    /* a base: that is not a directory matches nothing */
    if (basebuf[0]) { struct stat bst; if (stat(basebuf, &bst) != 0 || !S_ISDIR(bst.st_mode)) return korb_ary_new(c, slots, 0); }

    slots[0] = UNWRAP(korb_ary_new(c, slots, 8));
    struct korb_glob g = { c, VALUE_REF_AT(&slots[0]), 0, (rflags & 4) != 0, basebuf };
    if (rflags & 1) g.fnm |= FNM_NOESCAPE;             /* Ruby's bits differ from glibc's */
    if (rflags & 8) g.fnm |= FNM_CASEFOLD;
    if (!(rflags & 4)) g.fnm |= FNM_PERIOD;            /* no FNM_DOTMATCH → '*' skips a leading '.' */

    /* the patterns: one String, an Array of them, or anything with #to_path */
    slots[1] = VALUE_SLICE_GET(a, 0);
    const uint32_t np = KORB_ARRAY_P(slots[1]) ? VAL2ARY(slots[1])->len : 1u;
    for (uint32_t i = 0; i < np; i++) {
        VALUE pv = KORB_ARRAY_P(slots[1]) ? korb_items_data(VAL2ARY(slots[1])->items)[i] : slots[1];
        if (!KORB_STRING_P(pv)) {
            slots[2] = pv;
            const uint32_t to_path = korb_intern(c->vm, "to_path", 7);
            const uint32_t mid = korb_responds_to(c, pv, to_path) ? to_path : korb_intern(c->vm, "to_str", 6);
            if (!korb_responds_to(c, pv, mid))
                return korb_raise(c, slots + 2, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, pv));
            const RESULT pr = korb_send(c, slots + 3, mid, 0, 0);
            if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
            pv = pr.value;
            if (!KORB_STRING_P(pv))
                return korb_raise(c, slots + 2, KORB_E_TYPE, 0, "no implicit conversion into String");
        }
        const KorbString *const ps = VAL2STR(pv);
        if (UNLIKELY(memchr(korb_strbuf_data(ps->buf), '\0', ps->len) != NULL))
            return korb_raise(c, slots + 2, KORB_E_ARGUMENT, 0, "nul-separated glob pattern is deprecated");
        char pbuf[PATH_MAX];
        if (ps->len >= sizeof pbuf) continue;
        memcpy(pbuf, korb_strbuf_data(ps->buf), ps->len); pbuf[ps->len] = '\0';   /* copy: the walk allocs */
        CHECK(korb_glob_brace(&g, slots + 2, pbuf, 0));
    }
    if (block != NULL) {                                            /* yield each, return nil */
        const uint32_t n = VAL2ARY(slots[0])->len;
        for (uint32_t i = 0; i < n; i++) {
            slots[1] = korb_items_data(VAL2ARY(slots[0])->items)[i];
            CHECK(korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, captured_self));
        }
        return RESULT_OK(KORB_NIL);
    }
    return RESULT_OK(slots[0]);
}
/* Dir.chdir(path) [ { ... } ] — with a block, restores the old cwd after. */
static RESULT korb_m_dir_chdir(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                               struct Node *block, VALUE *def_env, VALUE *captured_self) {
    (void)self;
    RESULT err; const char *path = korb_path_arg(c, slots, a, &err); if (!path) return err;
    char old[8192];
    if (block != NULL && !getcwd(old, sizeof old)) old[0] = '\0';
    if (chdir(path) != 0) return korb_raise_errno(c, slots, errno, "chdir", path);
    if (block == NULL) return RESULT_OK(LONG2FIX(0));
    slots[0] = VALUE_SLICE_GET(a, 0);   /* block arg = the path String itself (no re-alloc — str_new from the arg's interior would use a moved pointer) */
    RESULT br = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
    if (old[0]) { int rc = chdir(old); (void)rc; }
    return br;
}

void korb_init_file(CTX *c, VALUE *slots) {
    struct korb_vm *const vm = c->vm;
    slots[0] = (korb_class_new(c, slots, korb_intern(vm, "File", 4), KORB_NIL)).value;
    korb_const_define(c, korb_intern(vm, "File", 4), slots[0]);
    korb_class_def_cfn(c, korb_builtin_class_obj(vm, KORB_C_OBJECT), "__errno_table", korb_m_errno_table, 0);
    korb_class_def_cfn(c, korb_builtin_class_obj(vm, KORB_C_OBJECT), "__strerror",    korb_m_strerror,     1);
    slots[1] = korb_obj_singleton(c, slots + 1, slots[0]).value;   /* class methods on File's singleton */
    korb_class_def_cfn(c, slots[1], "expand_path", korb_m_file_expand_path, -1);
    korb_class_def_cfn(c, slots[1], "realpath", korb_m_file_realpath, -1);
    korb_class_def_cfn(c, slots[1], "realdirpath", korb_m_file_realpath, -1);
    korb_class_def_cfn(c, slots[1], "join",        korb_m_file_join,        -1);
    korb_class_def_cfn(c, slots[1], "dirname",     korb_m_file_dirname,     -1);
    korb_class_def_cfn(c, slots[1], "basename",    korb_m_file_basename,    -1);
    korb_class_def_cfn(c, slots[1], "split",       korb_m_file_split,        1);
    korb_class_def_cfn(c, slots[1], "extname",     korb_m_file_extname,     1);
    korb_class_def_cfn(c, slots[1], "fnmatch",     korb_m_file_fnmatch,     -1);
    korb_class_def_cfn(c, slots[1], "fnmatch?",    korb_m_file_fnmatch,     -1);
    korb_class_def_cfn(c, slots[1], "exist?",      korb_m_file_exist_p,     1);
    korb_class_def_cfn(c, slots[1], "chown",       korb_m_file_chown,      -1);
    korb_class_def_cfn(c, slots[1], "__utime",     korb_m_file_utime,      -1);
    korb_class_def_cfn(c, slots[1], "mkfifo",      korb_m_file_mkfifo,     -1);
    korb_class_def_cfn(c, slots[1], "__mode_bits", korb_m_file_mode_bits,  -1);
    korb_class_def_cfn(c, slots[1], "symlink?",    korb_m_file_symlink_p,   1);
    korb_class_def_cfn(c, slots[1], "exists?",     korb_m_file_exist_p,     1);
    korb_class_def_cfn(c, slots[1], "file?",       korb_m_file_file_p,      1);
    korb_class_def_cfn(c, slots[1], "directory?",  korb_m_file_directory_p, 1);
    korb_class_def_cfn(c, slots[1], "size",        korb_m_file_size,        1);
    korb_class_def_cfn(c, slots[1], "size?",       korb_m_file_size,        1);
    korb_class_def_cfn(c, slots[1], "readable?",   korb_m_file_readable_p,  1);
    korb_class_def_cfn(c, slots[1], "writable?",   korb_m_file_writable_p,  1);
    korb_class_def_cfn(c, slots[1], "executable?", korb_m_file_executable_p, 1);
    korb_class_def_cfn(c, slots[1], "chmod",       korb_m_file_chmod,       -1);
    korb_class_def_cfn(c, slots[1], "umask",       korb_m_file_umask,       -1);
    korb_class_def_cfn(c, slots[1], "read",        korb_m_file_read,        -1);
    korb_class_def_cfn(c, slots[1], "write",       korb_m_file_write,       -1);
    korb_class_def_cfn(c, slots[1], "binread",     korb_m_file_read,        -1);   /* koruby I/O is already binary */
    korb_class_def_cfn(c, slots[1], "binwrite",    korb_m_file_write,       -1);
    korb_class_def_cfn(c, slots[1], "readlines",   korb_m_file_readlines,   -1);
    korb_class_def_cfn_blk(c, slots[1], "foreach", korb_m_file_foreach,     -1);
    korb_class_def_cfn(c, slots[1], "delete",      korb_m_file_delete,      -1);
    korb_class_def_cfn(c, slots[1], "unlink",      korb_m_file_delete,      -1);
    korb_class_def_cfn(c, slots[1], "link",        korb_m_file_link,         2);
    korb_class_def_cfn(c, slots[1], "symlink",     korb_m_file_symlink,      2);
    korb_class_def_cfn(c, slots[1], "readlink",    korb_m_file_readlink,     1);
    korb_class_def_cfn(c, slots[1], "stat",        korb_m_file_stat,         1);
    korb_class_def_cfn(c, slots[1], "lstat",       korb_m_file_lstat,        1);
    korb_class_def_cfn(c, slots[1], "atime",       korb_m_file_atime,        1);
    korb_class_def_cfn(c, slots[1], "ctime",       korb_m_file_ctime,        1);
    korb_class_def_cfn(c, slots[1], "mtime",       korb_m_file_mtime,        1);
    korb_class_def_cfn(c, slots[1], "birthtime",   korb_m_file_birthtime,    1);
    korb_class_def_cfn(c, slots[1], "truncate",    korb_m_file_truncate,     2);
    korb_class_def_cfn(c, slots[1], "absolute_path?", korb_m_file_abs_path_p, 1);
    korb_class_def_cfn(c, slots[1], "absolute_path", korb_m_file_absolute_path, -1);
    /* File::Stat — a stat(2) result value class (Object subclass). */
    slots[2] = (korb_class_new(c, slots + 2, korb_intern(vm, "File::Stat", 10),
                               korb_const_get(vm, korb_intern(vm, "Object", 6)))).value;
    korb_const_define(c, korb_intern(vm, "Stat", 4), slots[2]);         /* bare + File::Stat resolution */
    korb_const_define(c, korb_intern(vm, "File::Stat", 10), slots[2]);
    korb_class_def_cfn(c, slots[2], "size",      korb_m_stat_size,     0);
    korb_class_def_cfn(c, slots[2], "mode",      korb_m_stat_mode,     0);
    korb_class_def_cfn(c, slots[2], "ino",       korb_m_stat_ino,      0);
    korb_class_def_cfn(c, slots[2], "dev",       korb_m_stat_dev,      0);
    korb_class_def_cfn(c, slots[2], "nlink",     korb_m_stat_nlink,    0);
    korb_class_def_cfn(c, slots[2], "uid",       korb_m_stat_uid,      0);
    korb_class_def_cfn(c, slots[2], "gid",       korb_m_stat_gid,      0);
    korb_class_def_cfn(c, slots[2], "blksize",   korb_m_stat_blksize,  0);
    korb_class_def_cfn(c, slots[2], "blocks",    korb_m_stat_blocks,   0);
    korb_class_def_cfn(c, slots[2], "rdev",      korb_m_stat_rdev,     0);
    korb_class_def_cfn(c, slots[2], "mtime",     korb_m_stat_mtime,    0);
    korb_class_def_cfn(c, slots[2], "atime",     korb_m_stat_atime,    0);
    korb_class_def_cfn(c, slots[2], "ctime",     korb_m_stat_ctime,    0);
    korb_class_def_cfn(c, slots[2], "ftype",     korb_m_stat_ftype,    0);
    korb_class_def_cfn(c, slots[2], "directory?", korb_m_stat_dir_p,   0);
    korb_class_def_cfn(c, slots[2], "file?",     korb_m_stat_file_p,   0);
    korb_class_def_cfn(c, slots[2], "symlink?",  korb_m_stat_link_p,   0);
    korb_class_def_cfn(c, slots[2], "socket?",   korb_m_stat_sock_p,   0);
    korb_class_def_cfn(c, slots[2], "blockdev?", korb_m_stat_blk_p,    0);
    korb_class_def_cfn(c, slots[2], "chardev?",  korb_m_stat_chr_p,    0);
    korb_class_def_cfn(c, slots[2], "pipe?",     korb_m_stat_pipe_p,   0);
    korb_class_def_cfn(c, slots[2], "setuid?",   korb_m_stat_setuid_p, 0);
    korb_class_def_cfn(c, slots[2], "setgid?",   korb_m_stat_setgid_p, 0);
    korb_class_def_cfn(c, slots[2], "sticky?",   korb_m_stat_sticky_p, 0);
    korb_class_def_cfn(c, slots[2], "world_readable?", korb_m_stat_wreadable_p, 0);
    korb_class_def_cfn(c, slots[2], "world_writable?", korb_m_stat_wwritable_p, 0);
    korb_class_def_cfn(c, slots[2], "zero?",     korb_m_stat_zero_p,   0);
    korb_class_def_cfn(c, slots[2], "owned?",    korb_m_stat_owned_p,  0);
    korb_class_def_cfn(c, slots[2], "grouped?",  korb_m_stat_grouped_p, 0);
    korb_class_def_cfn(c, slots[2], "<=>",       korb_m_stat_spaceship, 1);
    korb_class_def_cfn(c, slots[2], "readable?",        korb_m_stat_readable_p,        0);
    korb_class_def_cfn(c, slots[2], "readable_real?",   korb_m_stat_readable_real_p,   0);
    korb_class_def_cfn(c, slots[2], "writable?",        korb_m_stat_writable_p,        0);
    korb_class_def_cfn(c, slots[2], "writable_real?",   korb_m_stat_writable_real_p,   0);
    korb_class_def_cfn(c, slots[2], "executable?",      korb_m_stat_executable_p,      0);
    korb_class_def_cfn(c, slots[2], "executable_real?", korb_m_stat_executable_real_p, 0);
    korb_class_def_cfn(c, slots[2], "size?",      korb_m_stat_size_p,    0);
    korb_class_def_cfn(c, slots[2], "dev_major",  korb_m_stat_dev_major, 0);
    korb_class_def_cfn(c, slots[2], "dev_minor",  korb_m_stat_dev_minor, 0);
    korb_class_def_cfn(c, slots[2], "rdev_major", korb_m_stat_rdev_major, 0);
    korb_class_def_cfn(c, slots[2], "rdev_minor", korb_m_stat_rdev_minor, 0);
    korb_class_def_cfn(c, slots[2], "birthtime",  korb_m_stat_birthtime, 0);
    /* Dir — pwd / exist? + instance objects (Dir.open/new).  Superclass = Object
     * so Dir instances inherit the universal methods (class, is_a?, …). */
    slots[2] = (korb_class_new(c, slots + 2, korb_intern(vm, "Dir", 3),
                               korb_const_get(vm, korb_intern(vm, "Object", 6)))).value;
    korb_const_define(c, korb_intern(vm, "Dir", 3), slots[2]);
    slots[3] = korb_obj_singleton(c, slots + 3, slots[2]).value;
    korb_class_def_cfn(c, slots[3], "pwd",     korb_m_dir_pwd,     0);
    korb_class_def_cfn(c, slots[3], "getwd",   korb_m_dir_pwd,     0);
    korb_class_def_cfn(c, slots[3], "exist?",  korb_m_dir_exist_p, 1);
    korb_class_def_cfn(c, slots[3], "exists?", korb_m_dir_exist_p, 1);
    korb_class_def_cfn(c, slots[3], "mkdir",    korb_m_dir_mkdir,    -1);
    korb_class_def_cfn(c, slots[3], "rmdir",    korb_m_dir_rmdir,    1);
    korb_class_def_cfn(c, slots[3], "delete",   korb_m_dir_rmdir,    1);
    korb_class_def_cfn(c, slots[3], "unlink",   korb_m_dir_rmdir,    1);
    korb_class_def_cfn(c, slots[3], "entries",  korb_m_dir_entries,  1);
    korb_class_def_cfn(c, slots[3], "children", korb_m_dir_children, 1);
    korb_class_def_cfn_blk(c, slots[3], "glob", korb_m_dir_glob, -1);
    korb_class_def_cfn_blk(c, slots[3], "[]",   korb_m_dir_glob, -1);
    korb_class_def_cfn_blk(c, slots[3], "chdir", korb_m_dir_chdir,   -1);
    korb_class_def_cfn_blk(c, slots[3], "open", korb_m_dir_open,    -1);   /* Dir.open [ {|d|} ] */
    korb_class_def_cfn_blk(c, slots[3], "new",  korb_m_dir_open,    -1);   /* Dir.new */
    /* Dir instance methods (eager-entry cursor object). */
    korb_class_def_cfn(c, slots[2], "read",       korb_m_dir_read,       0);
    korb_class_def_cfn_blk(c, slots[2], "each",   korb_m_dir_each,       0);
    korb_class_def_cfn(c, slots[2], "path",       korb_m_dir_path,       0);
    korb_class_def_cfn(c, slots[2], "to_path",    korb_m_dir_path,       0);
    korb_class_def_cfn(c, slots[2], "pos",        korb_m_dir_pos,        0);
    korb_class_def_cfn(c, slots[2], "tell",       korb_m_dir_pos,        0);
    korb_class_def_cfn(c, slots[2], "pos=",       korb_m_dir_pos_set,    1);
    korb_class_def_cfn(c, slots[2], "seek",       korb_m_dir_seek,       1);
    korb_class_def_cfn(c, slots[2], "rewind",     korb_m_dir_rewind,     0);
    korb_class_def_cfn(c, slots[2], "close",      korb_m_dir_close,      0);
    korb_class_def_cfn(c, slots[2], "children",   korb_m_dir_i_children, 0);
    /* File::Constants module + open/seek/fnmatch/lock flags (Linux values).
     * koruby's const table is flat, so these resolve from File / File::Constants
     * / bare alike. */
    slots[1] = (korb_class_new(c, slots + 1, korb_intern(vm, "Constants", 9), KORB_NIL)).value;
    VAL2CLASS(slots[1])->is_module = 1;                /* File::Constants is a module */
    korb_const_define(c, korb_intern(vm, "Constants", 9), slots[1]);
    static const struct { const char *n; long v; } fc[] = {
        {"RDONLY",0},{"WRONLY",1},{"RDWR",2},{"APPEND",1024},{"CREAT",64},{"EXCL",128},
        {"NOCTTY",256},{"TRUNC",512},{"NONBLOCK",2048},{"SYNC",1052672},{"DSYNC",4096},
        {"RSYNC",1052672},{"DIRECT",16384},{"NOFOLLOW",131072},{"BINARY",0},
        {"SHARE_DELETE",0},{"TMPFILE",4259840},{"NOATIME",262144},
        {"FNM_NOESCAPE",1},{"FNM_PATHNAME",2},{"FNM_DOTMATCH",4},{"FNM_CASEFOLD",8},
        {"FNM_EXTGLOB",16},{"FNM_SYSCASE",0},{"FNM_SHORTNAME",0},
        {"LOCK_SH",1},{"LOCK_EX",2},{"LOCK_UN",8},{"LOCK_NB",4},
        {"SEEK_SET",0},{"SEEK_CUR",1},{"SEEK_END",2},
    };
    for (size_t i = 0; i < sizeof(fc) / sizeof(fc[0]); i++)
        korb_const_define(c, korb_intern(vm, fc[i].n, (uint32_t)strlen(fc[i].n)), LONG2FIX(fc[i].v));
    korb_const_define(c, korb_intern(vm, "SEPARATOR", 9),      korb_str_new(c, slots + 2, "/", 1).value);
    korb_const_define(c, korb_intern(vm, "Separator", 9),      korb_str_new(c, slots + 2, "/", 1).value);
    korb_const_define(c, korb_intern(vm, "PATH_SEPARATOR", 14), korb_str_new(c, slots + 2, ":", 1).value);
    korb_const_define(c, korb_intern(vm, "ALT_SEPARATOR", 13),  KORB_NIL);   /* nil on POSIX */
}
