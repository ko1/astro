/* koruby_precise — file.c: a minimal File class (POSIX path-string methods only,
 * no real I/O yet).  #included into korb_runtime.c's TU.  Enough to unblock the
 * mspec `fixture()` helper and the File path specs (expand_path / join / dirname
 * / basename / extname).  Paths are byte strings; '/' is the only separator. */
#include <unistd.h>

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
}
