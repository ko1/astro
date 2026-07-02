/* koruby_precise — file.c: a minimal File class (POSIX path-string methods only,
 * no real I/O yet).  #included into korb_runtime.c's TU.  Enough to unblock the
 * mspec `fixture()` helper and the File path specs (expand_path / join / dirname
 * / basename / extname).  Paths are byte strings; '/' is the only separator. */
#include <unistd.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <dirent.h>
#include <glob.h>

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

static const char *korb_str_cstr_len(VALUE v, uint32_t *len) {
    *len = VAL2STR(v)->len;
    return VAL2STR(v)->buf->data;
}

/* File.expand_path(path, base = Dir.pwd) → an absolute, normalized path. */
static RESULT korb_m_file_expand_path(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
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
static RESULT korb_m_file_join(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    char buf[8192]; size_t d = 0;
    for (uint32_t i = 0; i < VALUE_SLICE_LEN(a); i++) {
        const VALUE pv = VALUE_SLICE_GET(a, i);
        if (UNLIKELY(!KORB_STRING_P(pv)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
        uint32_t plen; const char *p = korb_str_cstr_len(pv, &plen);
        if (i > 0) {                                  /* drop a trailing sep on the left + a leading sep on the right */
            while (d > 0 && buf[d - 1] == '/') d--;
            while (plen > 0 && p[0] == '/') { p++; plen--; }
            if (d + 1 < sizeof buf) buf[d++] = '/';
        }
        if (d + plen >= sizeof buf) plen = (uint32_t)(sizeof buf - 1 - d);
        memcpy(buf + d, p, plen); d += plen;
    }
    return korb_str_new(c, slots, buf, (uint32_t)d);
}

/* File.dirname(path) → everything before the last '/', or "." if none. */
static RESULT korb_m_file_dirname(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
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

/* File.basename(path, suffix = nil) → last component, optionally minus a suffix
 * (".*" strips any extension). */
static RESULT korb_m_file_basename(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
    uint32_t n; const char *p = korb_str_cstr_len(pv, &n);
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
    const VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
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
    memcpy(pbuf, pat->buf->data, pat->len); pbuf[pat->len] = '\0';
    memcpy(sbuf, str->buf->data, str->len); sbuf[str->len] = '\0';
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
    const VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
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
static RESULT korb_m_file_exist_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)     { (void)self; return korb_m_file_stat_pred(c, slots, a, 0); }
static RESULT korb_m_file_file_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)      { (void)self; return korb_m_file_stat_pred(c, slots, a, 1); }
static RESULT korb_m_file_directory_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) { (void)self; return korb_m_file_stat_pred(c, slots, a, 2); }
static RESULT korb_m_file_size(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)        { (void)self; return korb_m_file_stat_pred(c, slots, a, 3); }

/* File.read(path) → the whole file as a String. */
static RESULT korb_m_file_read(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
    FILE *f = fopen(path, "rb");
    if (!f) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "No such file or directory @ rb_sysopen - %s", path);
    char *buf = NULL; size_t cap = 0, len = 0;
    for (;;) {
        if (len + 65536 > cap) { cap = cap ? cap * 2 : 131072; char *nb = realloc(buf, cap); if (!nb) { free(buf); fclose(f); return korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of memory reading %s", path); } buf = nb; }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) break;
    }
    fclose(f);
    RESULT r = korb_str_new(c, slots, buf ? buf : "", (uint32_t)len);
    free(buf);
    return r;
}

/* slurp the whole file into a malloc'd buffer (caller frees); NULL on open fail. */
static char *korb_file_slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL; size_t cap = 0, len = 0;
    for (;;) {
        if (len + 65536 > cap) { cap = cap ? cap * 2 : 131072; char *nb = realloc(buf, cap); if (!nb) { free(buf); fclose(f); *out_len = 0; return NULL; } buf = nb; }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) break;
    }
    fclose(f);
    if (!buf) { buf = malloc(1); buf[0] = 0; }
    *out_len = len;
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
        if (hx >= 0 && KORB_STRING_P(h->items->data[2 * hx + 1])) {
            uint32_t ml; const char *m = korb_str_cstr_len(h->items->data[2 * hx + 1], &ml);
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
    FILE *f = fopen(path, mode);
    if (!f) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "No such file or directory @ rb_sysopen - %s", path);
    size_t w = fwrite(data, 1, dlen, f);
    fclose(f);
    return RESULT_OK(LONG2FIX((intptr_t)w));
}

/* split `buf` (len bytes) into line Strings (keeping the trailing '\n'), pushed
 * onto the rooted array `arr`. */
static RESULT korb_file_push_lines(CTX *c, VALUE *slots, VALUE_REF arr, const char *buf, size_t len) {
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            slots[0] = UNWRAP(korb_str_new(c, slots, buf + start, (uint32_t)(i - start + 1)));
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

/* File.readlines(path) → array of line Strings. */
static RESULT korb_m_file_readlines(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
    size_t len; char *buf = korb_file_slurp(path, &len);
    if (!buf) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "No such file or directory @ rb_sysopen - %s", path);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 16));
    RESULT r = korb_file_push_lines(c, slots + 1, VALUE_REF_AT(&slots[0]), buf, len);
    free(buf);
    return r;
}

/* File.foreach(path) { |line| ... } → yields each line; returns nil. */
static RESULT korb_m_file_foreach(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                  struct Node *block, VALUE *def_env, VALUE *captured_self) {
    (void)self;
    const VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv));
    if (block == NULL) return RESULT_OK(KORB_NIL);
    uint32_t plen; const char *path = korb_str_cstr_len(pv, &plen);
    size_t len; char *buf = korb_file_slurp(path, &len);
    if (!buf) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "No such file or directory @ rb_sysopen - %s", path);
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
            return korb_raise(c, slots, KORB_E_RUNTIME, 0, "No such file or directory @ apply2files - %s", path);
        cnt++;
    }
    return RESULT_OK(LONG2FIX(cnt));
}

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
static const char *korb_path_arg(CTX *c, VALUE *slots, VALUE_SLICE a, RESULT *err) {
    const VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(pv))) { *err = korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(pv)); return NULL; }
    err->state = KORB_NORMAL; uint32_t plen; return korb_str_cstr_len(pv, &plen);
}
/* Dir.mkdir(path[, mode]) → 0 (raises on failure). */
static RESULT korb_m_dir_mkdir(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    RESULT err; const char *path = korb_path_arg(c, slots, a, &err); if (!path) return err;
    long mode = 0777;
    if (VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1))) mode = (long)FIX2LONG(VALUE_SLICE_GET(a, 1));
    if (mkdir(path, (mode_t)mode) != 0) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "File exists @ dir_s_mkdir - %s", path);
    return RESULT_OK(LONG2FIX(0));
}
/* Dir.rmdir(path) → 0 (raises on failure). */
static RESULT korb_m_dir_rmdir(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    RESULT err; const char *path = korb_path_arg(c, slots, a, &err); if (!path) return err;
    if (rmdir(path) != 0) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "No such file or directory @ dir_s_rmdir - %s", path);
    return RESULT_OK(LONG2FIX(0));
}
/* Dir.entries(path) [with_dots] / Dir.children(path) [without]. */
static RESULT korb_dir_list(CTX *c, VALUE *slots, VALUE_SLICE a, bool with_dots) {
    RESULT err; const char *path = korb_path_arg(c, slots, a, &err); if (!path) return err;
    DIR *d = opendir(path);
    if (!d) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "No such file or directory @ dir_initialize - %s", path);
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
/* Dir.glob(pattern) / Dir[pattern] → matched paths (POSIX glob). */
static RESULT korb_m_dir_glob(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    RESULT err; const char *pat = korb_path_arg(c, slots, a, &err); if (!pat) return err;
    glob_t g; memset(&g, 0, sizeof g);
    glob(pat, GLOB_BRACE | GLOB_TILDE, NULL, &g);
    slots[0] = UNWRAP(korb_ary_new(c, slots, (uint32_t)(g.gl_pathc ? g.gl_pathc : 1)));
    VALUE_REF arr = VALUE_REF_AT(&slots[0]);
    for (size_t i = 0; i < g.gl_pathc; i++) {
        slots[1] = UNWRAP(korb_str_new(c, slots + 1, g.gl_pathv[i], (uint32_t)strlen(g.gl_pathv[i])));
        if (korb_ary_push_val(c, slots + 2, arr, slots[1]).state != KORB_NORMAL) break;
    }
    globfree(&g);
    return RESULT_OK(VALUE_REF_GET(arr));
}
/* Dir.chdir(path) [ { ... } ] — with a block, restores the old cwd after. */
static RESULT korb_m_dir_chdir(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                               struct Node *block, VALUE *def_env, VALUE *captured_self) {
    (void)self;
    RESULT err; const char *path = korb_path_arg(c, slots, a, &err); if (!path) return err;
    char old[8192];
    if (block != NULL && !getcwd(old, sizeof old)) old[0] = '\0';
    if (chdir(path) != 0) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "No such file or directory @ dir_s_chdir - %s", path);
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
    slots[1] = korb_obj_singleton(c, slots + 1, slots[0]).value;   /* class methods on File's singleton */
    korb_class_def_cfn(c, slots[1], "expand_path", korb_m_file_expand_path, -1);
    korb_class_def_cfn(c, slots[1], "join",        korb_m_file_join,        -1);
    korb_class_def_cfn(c, slots[1], "dirname",     korb_m_file_dirname,     -1);
    korb_class_def_cfn(c, slots[1], "basename",    korb_m_file_basename,    -1);
    korb_class_def_cfn(c, slots[1], "extname",     korb_m_file_extname,     1);
    korb_class_def_cfn(c, slots[1], "fnmatch",     korb_m_file_fnmatch,     -1);
    korb_class_def_cfn(c, slots[1], "fnmatch?",    korb_m_file_fnmatch,     -1);
    korb_class_def_cfn(c, slots[1], "exist?",      korb_m_file_exist_p,     1);
    korb_class_def_cfn(c, slots[1], "exists?",     korb_m_file_exist_p,     1);
    korb_class_def_cfn(c, slots[1], "file?",       korb_m_file_file_p,      1);
    korb_class_def_cfn(c, slots[1], "directory?",  korb_m_file_directory_p, 1);
    korb_class_def_cfn(c, slots[1], "size",        korb_m_file_size,        1);
    korb_class_def_cfn(c, slots[1], "size?",       korb_m_file_size,        1);
    korb_class_def_cfn(c, slots[1], "read",        korb_m_file_read,        -1);
    korb_class_def_cfn(c, slots[1], "write",       korb_m_file_write,       -1);
    korb_class_def_cfn(c, slots[1], "readlines",   korb_m_file_readlines,   -1);
    korb_class_def_cfn_blk(c, slots[1], "foreach", korb_m_file_foreach,     -1);
    korb_class_def_cfn(c, slots[1], "delete",      korb_m_file_delete,      -1);
    korb_class_def_cfn(c, slots[1], "unlink",      korb_m_file_delete,      -1);
    /* Dir — pwd / exist?. */
    slots[2] = (korb_class_new(c, slots + 2, korb_intern(vm, "Dir", 3), KORB_NIL)).value;
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
    korb_class_def_cfn(c, slots[3], "glob",     korb_m_dir_glob,     -1);
    korb_class_def_cfn(c, slots[3], "[]",       korb_m_dir_glob,     -1);
    korb_class_def_cfn_blk(c, slots[3], "chdir", korb_m_dir_chdir,   -1);
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
