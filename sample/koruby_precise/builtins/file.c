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
    for (size_t i = 0; i < sizeof korb_errno_tab / sizeof korb_errno_tab[0]; i++)
        if (korb_errno_tab[i].num == e) return korb_errno_tab[i].name;
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
static RESULT korb_raise_errno(CTX *c, VALUE *slots, int e, const char *func, const char *path) {
    char msg[4096];
    snprintf(msg, sizeof msg, "%s @ %s - %s", strerror(e), func, path);   /* format now: path is a movable-String interior ptr, korb_raise allocs */
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
        const RESULT r = korb_send_impl(c, slots + 1, mid, 0, 0, NULL, NULL, KORB_NIL);
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
static RESULT korb_m_file_expand_path(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);

    char raw[8192]; size_t r = 0;
    if (plen > 0 && path[0] == '/') {                 /* already absolute */
        memcpy(raw, path, plen); r = plen;
    } else if (plen > 0 && path[0] == '~' && (plen == 1 || path[1] == '/')) {
        const char *home = getenv("HOME"); if (!home) home = "/";
        size_t hl = strlen(home);
        memcpy(raw, home, hl); r = hl;
        if (plen > 1) { memcpy(raw + r, path + 1, plen - 1); r += plen - 1; }
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
    return korb_str_new(c, slots, out, (uint32_t)olen);
}

/* File.join(*parts) → parts joined with a single '/'. */
/* Append one File.join component.  Separator rule (CRuby's rb_file_join): for
 * every component after the first, a leading separator on the right absorbs any
 * trailing separators on the left, otherwise one is inserted when the left does
 * not already end with it.  No allocation happens here, so the interior string
 * pointers stay valid for the whole walk. */
static RESULT korb_file_join_str(CTX *c, VALUE *slots, VALUE pv, char *buf, size_t cap, size_t *d, bool *first) {
    if (UNLIKELY(!KORB_STRING_P(pv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
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
    return korb_str_new(c, slots, out, (uint32_t)last);
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

/* File.fnmatch(pat, path[, flags]) — glob match via POSIX fnmatch(3), with the
 * Ruby flag bits translated to glibc's (different numeric values).  Ruby's `**`
 * and `{a,b}` (FNM_EXTGLOB) aren't POSIX, so those edge cases may differ. */
static RESULT korb_m_file_fnmatch(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 2 || !KORB_STRING_P(VALUE_SLICE_GET(a, 0)) || !KORB_STRING_P(VALUE_SLICE_GET(a, 1))))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    const KorbString *const pat = VAL2STR(VALUE_SLICE_GET(a, 0));
    const KorbString *const str = VAL2STR(VALUE_SLICE_GET(a, 1));
    const long rf = (VALUE_SLICE_LEN(a) >= 3 && FIXNUM_P(VALUE_SLICE_GET(a, 2))) ? FIX2LONG(VALUE_SLICE_GET(a, 2)) : 0;
    char pbuf[4096], sbuf[4096];
    if (UNLIKELY(pat->len >= sizeof pbuf || str->len >= sizeof sbuf)) return RESULT_OK(KORB_FALSE);
    memcpy(pbuf, korb_strbuf_data(pat->buf), pat->len); pbuf[pat->len] = '\0';
    memcpy(sbuf, korb_strbuf_data(str->buf), str->len); sbuf[str->len] = '\0';
    int cf = 0;                                        /* Ruby bits → glibc bits */
    if (rf & 1)  cf |= FNM_NOESCAPE;                   /* FNM_NOESCAPE  (Ruby 1) */
    if (rf & 2)  cf |= FNM_PATHNAME;                   /* FNM_PATHNAME  (Ruby 2) */
    if (rf & 8)  cf |= FNM_CASEFOLD;                   /* FNM_CASEFOLD  (Ruby 8) */
    if (!(rf & 4)) cf |= FNM_PERIOD;                   /* no FNM_DOTMATCH → leading '.' not matched by '*' */
    return RESULT_OK(fnmatch(pbuf, sbuf, cf) == 0 ? KORB_TRUE : KORB_FALSE);
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
      default: return RESULT_OK(LONG2FIX((intptr_t)st.st_size));            /* size */
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
    CHECK(korb_ary_push_val(c, slots + 1, ar, LONG2FIX((intptr_t)st.st_mode)));
    CHECK(korb_ary_push_val(c, slots + 1, ar, LONG2FIX((intptr_t)st.st_uid)));
    CHECK(korb_ary_push_val(c, slots + 1, ar, LONG2FIX((intptr_t)st.st_gid)));
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
        return RESULT_OK(LONG2FIX((intptr_t)umask((mode_t)FIX2LONG(VALUE_SLICE_GET(a, 0)))));
    const mode_t cur = umask(0); umask(cur);                  /* read current without changing it */
    return RESULT_OK(LONG2FIX((intptr_t)cur));
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

/* File.read(path[, length[, offset]]) → the file (or a slice) as a String. */
static RESULT korb_m_file_read(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return korb_raise_errno(c, slots, errno, "rb_sysopen", path);
    const bool has_len = VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1));
    if (VALUE_SLICE_LEN(a) >= 3 && FIXNUM_P(VALUE_SLICE_GET(a, 2)))   /* offset */
        (void)lseek(fd, (off_t)FIX2LONG(VALUE_SLICE_GET(a, 2)), SEEK_SET);
    if (has_len) {                                                    /* bounded read */
        intptr_t n = FIX2LONG(VALUE_SLICE_GET(a, 1)); if (n < 0) n = 0;
        char *b = malloc((size_t)n + 1);
        if (!b) { close(fd); return korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of memory"); }
        const size_t got = korb_fd_read_n(fd, b, (size_t)n); close(fd);
        if (got == 0 && n > 0) { free(b); return RESULT_OK(KORB_NIL); }   /* EOF */
        RESULT r = korb_str_new(c, slots, b, (uint32_t)got);
        free(b);
        return r;
    }
    size_t len = 0;
    char *const buf = korb_fd_slurp(fd, &len);
    close(fd);
    if (!buf) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of memory reading %s", path);
    RESULT r = korb_str_new(c, slots, buf, (uint32_t)len);
    free(buf);
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

/* File.write(path, string, mode: "w") → byte count (mode: "a" appends). */
static RESULT korb_m_file_write(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    uint32_t n = VALUE_SLICE_LEN(a);
    const char *mode = "wb";
    if (n >= 3 && KORB_HASH_P(VALUE_SLICE_GET(a, n - 1))) {   /* trailing kwarg: mode: */
        const KorbHash *h = VAL2HASH(VALUE_SLICE_GET(a, n - 1));
        const int32_t hx = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "mode", 4)));
        if (hx >= 0 && KORB_STRING_P(korb_items_data(h->items)[2 * hx + 1])) {
            uint32_t ml; const char *m = korb_str_cstr_len(korb_items_data(h->items)[2 * hx + 1], &ml);
            if (ml >= 1 && m[0] == 'a') mode = "ab";
        }
        n--;
    }
    const VALUE pv = VALUE_SLICE_GET(a, 0), sv = VALUE_SLICE_GET(a, 1);
    if (UNLIKELY(!KORB_STRING_P(pv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
    if (UNLIKELY(!KORB_STRING_P(sv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(sv));
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
    uint32_t dlen; const char *data = korb_str_cstr_len(sv, &dlen);
    const int fd = open(path, O_WRONLY | O_CREAT | (mode[0] == 'a' ? O_APPEND : O_TRUNC), 0666);
    if (fd < 0) return korb_raise_errno(c, slots, errno, "rb_sysopen", path);
    const size_t w = korb_fd_write_all(fd, data, dlen);
    close(fd);
    return RESULT_OK(LONG2FIX((intptr_t)w));
}

/* split `buf` (len bytes) into line Strings (keeping the trailing '\n'), pushed
 * onto the rooted array `arr`. */
static RESULT korb_file_push_lines_c(CTX *c, VALUE *slots, VALUE_REF arr, const char *buf, size_t len, bool chomp) {
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            size_t end = i + 1;
            if (chomp) { end = i; if (end > start && buf[end - 1] == '\r') end--; }   /* drop \n (and \r\n) */
            slots[0] = UNWRAP(korb_str_new(c, slots, buf + start, (uint32_t)(end - start)));
            CHECK(korb_ary_push_val(c, slots + 1, arr, slots[0]));
            start = i + 1;
        }
    }
    if (start < len) {   /* trailing line without newline */
        slots[0] = UNWRAP(korb_str_new(c, slots, buf + start, (uint32_t)(len - start)));
        CHECK(korb_ary_push_val(c, slots + 1, arr, slots[0]));
    }
    return RESULT_OK(VALUE_REF_GET(arr));
}
static RESULT korb_file_push_lines(CTX *c, VALUE *slots, VALUE_REF arr, const char *buf, size_t len) {
    return korb_file_push_lines_c(c, slots, arr, buf, len, false);
}

/* File.readlines(path) → array of line Strings. */
static RESULT korb_m_file_readlines(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
    bool chomp = false;   /* trailing `chomp: true` kwarg strips line terminators */
    const uint32_t na = VALUE_SLICE_LEN(a);
    if (na >= 2 && KORB_HASH_P(VALUE_SLICE_GET(a, na - 1))) {
        const KorbHash *h = VAL2HASH(VALUE_SLICE_GET(a, na - 1));
        const int32_t hx = korb_hash_find(h, ID2SYM(korb_intern(c->vm, "chomp", 5)));
        if (hx >= 0) { const VALUE v = korb_items_data(h->items)[2 * hx + 1]; chomp = (v != KORB_NIL && v != KORB_FALSE); }
    }
    size_t len; char *buf = korb_file_slurp(path, &len);
    if (!buf) return korb_raise_errno(c, slots, errno, "rb_sysopen", path);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 16));
    RESULT r = korb_file_push_lines_c(c, slots + 1, VALUE_REF_AT(&slots[0]), buf, len, chomp);
    free(buf);
    return r;
}

/* File.foreach(path) { |line| ... } → yields each line; returns nil. */
static RESULT korb_m_file_foreach(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                  struct Node *block, VALUE *def_env, VALUE *captured_self) {
    (void)self;
    VALUE pv;
    KORB_PATH_ARG(c, slots, a, 0, pv);
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
    if (block == NULL) {                                             /* no block → an Enumerator of the lines */
        size_t len2; char *buf2 = korb_file_slurp(path, &len2);
        if (!buf2) return korb_raise_errno(c, slots, errno, "rb_sysopen", path);
        slots[0] = UNWRAP(korb_ary_new(c, slots, 16));
        RESULT lr = korb_file_push_lines(c, slots + 1, VALUE_REF_AT(&slots[0]), buf2, len2);
        free(buf2);
        if (UNLIKELY(lr.state != KORB_NORMAL)) return lr;
        return korb_enum_new(c, slots + 1, slots[0], KORB_NIL);
    }
    size_t len; char *buf = korb_file_slurp(path, &len);
    if (!buf) return korb_raise_errno(c, slots, errno, "rb_sysopen", path);
    size_t start = 0;
    RESULT rr = RESULT_OK(KORB_NIL);
    for (size_t i = 0; i <= len; i++) {
        if (i == len ? (start < len) : (buf[i] == '\n')) {
            const size_t end = (i == len) ? len : i + 1;
            slots[0] = korb_str_new(c, slots, buf + start, (uint32_t)(end - start)).value;
            rr = korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, captured_self);
            if (rr.state != KORB_NORMAL) break;
            start = end;
        }
    }
    free(buf);
    if (rr.state != KORB_NORMAL) return rr;
    return RESULT_OK(KORB_NIL);
}

/* File.delete(*paths) / File.unlink → number removed. */
static RESULT korb_m_file_delete(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    intptr_t cnt = 0;
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
    #define SETI(nm, v) CHECK(korb_ivar_set(c, slots + 1, o, korb_stat_iv(c, nm), LONG2FIX((intptr_t)(v))))
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
static intptr_t korb_stat_field(CTX *c, VALUE self, const char *nm) {
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
    (void)a; const intptr_t t = korb_stat_field(c, VALUE_REF_GET(self), field); \
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
    (void)slots; (void)a; return RESULT_OK(korb_stat_field(c, VALUE_REF_GET(self), "@__uid") == (intptr_t)geteuid() ? KORB_TRUE : KORB_FALSE);
}
static RESULT korb_m_stat_grouped_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)a; return RESULT_OK(korb_stat_field(c, VALUE_REF_GET(self), "@__gid") == (intptr_t)getegid() ? KORB_TRUE : KORB_FALSE);
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
    const intptr_t sz = korb_stat_field(c, VALUE_REF_GET(self), "@__size");
    return RESULT_OK(sz == 0 ? KORB_NIL : LONG2FIX(sz));
}
#define STAT_DEVPART_M(fn, field, part) static RESULT fn(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { \
    (void)slots; (void)a; const dev_t d = (dev_t)korb_stat_field(c, VALUE_REF_GET(self), field); \
    return RESULT_OK(LONG2FIX((intptr_t)part(d))); }
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
    const intptr_t m1 = korb_stat_field(c, VALUE_REF_GET(self), "@__mtime"), m2 = korb_stat_field(c, o, "@__mtime");
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
    if (VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1))) mode = (long)FIX2LONG(VALUE_SLICE_GET(a, 1));
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
    const intptr_t pos = FIXNUM_P(posv) ? FIX2LONG(posv) : 0;
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
/* Push the glob(3) matches of one concrete pattern into `arr`.  When `strip` is
 * set, a synthetic "./" base prefix (added for a leading `**`) is removed so the
 * results match CRuby's bare relative paths. */
static RESULT korb_glob_push(CTX *c, VALUE *slots, VALUE_REF arr, const char *pat, int flags, bool strip) {
    glob_t g; memset(&g, 0, sizeof g);
    glob(pat, flags, NULL, &g);
    for (size_t i = 0; i < g.gl_pathc; i++) {
        const char *m = g.gl_pathv[i];
        if (strip && m[0] == '.' && m[1] == '/') m += 2;
        slots[0] = UNWRAP(korb_str_new(c, slots, m, (uint32_t)strlen(m)));
        if (korb_ary_push_val(c, slots + 1, arr, slots[0]).state != KORB_NORMAL) break;
    }
    globfree(&g);
    return RESULT_OK(KORB_NIL);
}
/* Glob one pattern into `arr`, expanding a recursive double-star segment (which
 * glob(3) does not support) by trying prefix + N intermediate wildcard dirs +
 * suffix, for N = 0..24. */
static RESULT korb_glob_one(CTX *c, VALUE *slots, VALUE_REF arr, const char *pat) {
    const int flags = GLOB_BRACE | GLOB_TILDE;
    const char *ss = strstr(pat, "**");
    if (!ss) return korb_glob_push(c, slots, arr, pat, flags, false);
    /* split into the prefix before the double-star and the suffix after it. */
    char prefix[4096], suffix[4096];
    size_t plen = (size_t)(ss - pat);
    while (plen > 0 && pat[plen - 1] == '/') plen--;                 /* trim the '/' before ** */
    if (plen >= sizeof prefix) plen = sizeof prefix - 1;
    memcpy(prefix, pat, plen); prefix[plen] = '\0';
    const bool strip = (plen == 0);                                 /* leading `**`: drop the synthetic "./" base */
    const char *suf = ss + 2;                                        /* after "**" */
    while (*suf == '/') suf++;                                       /* skip the slash after the stars */
    snprintf(suffix, sizeof suffix, "%s", suf);
    for (int depth = 0; depth <= 24; depth++) {                      /* ** matches 0..24 directory levels */
        char pbuf[8192]; int n = 0;
        n += snprintf(pbuf + n, sizeof pbuf - n, "%s", prefix[0] ? prefix : ".");
        for (int d = 0; d < depth; d++) n += snprintf(pbuf + n, sizeof pbuf - (size_t)n, "/*");
        if (suffix[0]) n += snprintf(pbuf + n, sizeof pbuf - (size_t)n, "/%s", suffix);
        CHECK(korb_glob_push(c, slots, arr, pbuf, flags, strip));
        /* stop once no directory exists at this depth (nothing deeper to match). */
        char dbuf[8192]; int m = 0;
        m += snprintf(dbuf + m, sizeof dbuf - (size_t)m, "%s", prefix[0] ? prefix : ".");
        for (int d = 0; d <= depth; d++) m += snprintf(dbuf + m, sizeof dbuf - (size_t)m, "/*");
        glob_t gd; memset(&gd, 0, sizeof gd);
        glob(dbuf, flags | GLOB_ONLYDIR, NULL, &gd);
        const size_t dirs = gd.gl_pathc; globfree(&gd);
        if (dirs == 0) break;
    }
    return RESULT_OK(KORB_NIL);
}
/* Dir.glob(pattern | [patterns]) [ { |path| } ] / Dir[pattern] → matched paths.
 * Supports `**` recursion, brace/tilde expansion, and multiple patterns. */
static RESULT korb_m_dir_glob(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                              struct Node *block, VALUE *def_env, VALUE *captured_self) {
    (void)self;
    slots[0] = UNWRAP(korb_ary_new(c, slots, 8));
    VALUE_REF arr = VALUE_REF_AT(&slots[0]);
    const VALUE p0 = VALUE_SLICE_GET(a, 0);
    if (KORB_ARRAY_P(p0)) {                                          /* array of patterns */
        slots[1] = p0;                                              /* root the pattern array */
        const uint32_t np = VAL2ARY(slots[1])->len;
        for (uint32_t i = 0; i < np; i++) {
            const VALUE pv = korb_items_data(VAL2ARY(slots[1])->items)[i];
            if (!KORB_STRING_P(pv)) continue;
            uint32_t pl; char pbuf[8192]; const char *ps = korb_str_cstr_len(pv, &pl);
            if (pl >= sizeof pbuf) continue;
            memcpy(pbuf, ps, pl); pbuf[pl] = '\0';                   /* copy: korb_glob_one allocs */
            CHECK(korb_glob_one(c, slots + 2, arr, pbuf));
        }
    } else {
        if (UNLIKELY(!KORB_STRING_P(p0)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(p0));
        uint32_t pl; char pbuf[8192]; const char *ps = korb_str_cstr_len(p0, &pl);
        if (pl < sizeof pbuf) { memcpy(pbuf, ps, pl); pbuf[pl] = '\0'; CHECK(korb_glob_one(c, slots + 2, arr, pbuf)); }
    }
    if (block != NULL) {                                            /* yield each, return nil */
        const uint32_t n = VAL2ARY(VALUE_REF_GET(arr))->len;
        for (uint32_t i = 0; i < n; i++) {
            slots[1] = korb_items_data(VAL2ARY(VALUE_REF_GET(arr))->items)[i];
            CHECK(korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, captured_self));
        }
        return RESULT_OK(KORB_NIL);
    }
    return RESULT_OK(VALUE_REF_GET(arr));
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
    korb_class_def_cfn(c, slots[1], "absolute_path", korb_m_file_expand_path, -1);
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
