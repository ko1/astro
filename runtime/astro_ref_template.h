/* astro_ref_template.h — rooted-reference types derived from a sample's
 * value type name (v2 slots ABI; see sample/koruby_precise/docs/v2_design.md
 * §5).  Include from the sample's context.h AFTER typedef'ing the value
 * type:
 *
 *     typedef intptr_t VALUE;
 *     #define ASTRO_REF_VALUE VALUE
 *     #include "astro_ref_template.h"
 *
 * Generated (for ASTRO_REF_VALUE == VALUE):
 *
 *     VALUE_REF                rooted reference to ONE value cell
 *     VALUE_REF_AT(p)          construct from a stable-root cell address
 *     VALUE_REF_GET(r)         checked read  (audit hook)
 *     VALUE_REF_SET(r, v)      checked write (audit hook / future WB site)
 *     VALUE_SLICE              contiguous rooted cell run + count
 *     VALUE_SLICE_MAKE(p, n)
 *     VALUE_SLICE_GET(s, i) / VALUE_SLICE_SET(s, i, v) / VALUE_SLICE_LEN(s)
 *     VALUE_SLICE_REF(s, i)    VALUE_REF to the i-th cell
 *
 * The names are derived from the value type so a language that names its
 * value type `LV` gets `LV_REF` / `LV_SLICE` — the framework never bakes
 * a fixed value-type name into a public identifier.
 *
 * Raw `VALUE *` disappears from the sample ABI: a reference may only be
 * constructed via <T>_REF_AT from a stable-root cell (slots buffer, CTX
 * root field, immortal object field) — never `&local` or a movable
 * object's payload.  Static checking of construction sites is the
 * sample's CodeQL layer; the audit build hook below catches stale reads.
 *
 * Audit hook: define ASTRO_REF_CHECK(p) before including to run a
 * stable-root / stale-value check on every deref (no-op default).
 */

#ifndef ASTRO_REF_VALUE
#  error "define ASTRO_REF_VALUE (the sample's value type name) before including astro_ref_template.h"
#endif

#ifndef ASTRO_REF_CHECK
#  define ASTRO_REF_CHECK(p) ((void)0)
#endif

#define ASTRO_REF_CAT_(a, b) a##b
#define ASTRO_REF_CAT(a, b)  ASTRO_REF_CAT_(a, b)
#define ASTRO_REF_T          ASTRO_REF_CAT(ASTRO_REF_VALUE, _REF)
#define ASTRO_SLICE_T        ASTRO_REF_CAT(ASTRO_REF_VALUE, _SLICE)
#define ASTRO_REF_FN(suffix) ASTRO_REF_CAT(ASTRO_REF_VALUE, suffix)

/* 1-member struct → passed in a register; constructing one is free. */
typedef struct { ASTRO_REF_VALUE *p; } ASTRO_REF_T;
typedef struct { ASTRO_REF_VALUE *p; uint32_t cnt; } ASTRO_SLICE_T;

static inline ASTRO_REF_T
ASTRO_REF_FN(_REF_AT)(ASTRO_REF_VALUE *cell)
{
    return (ASTRO_REF_T){ cell };
}

static inline ASTRO_REF_VALUE
ASTRO_REF_FN(_REF_GET)(ASTRO_REF_T r)
{
    ASTRO_REF_CHECK(r.p);
    return *r.p;
}

static inline void
ASTRO_REF_FN(_REF_SET)(ASTRO_REF_T r, ASTRO_REF_VALUE v)
{
    ASTRO_REF_CHECK(r.p);
    *r.p = v;
}

static inline ASTRO_SLICE_T
ASTRO_REF_FN(_SLICE_MAKE)(ASTRO_REF_VALUE *cells, uint32_t cnt)
{
    return (ASTRO_SLICE_T){ cells, cnt };
}

static inline uint32_t
ASTRO_REF_FN(_SLICE_LEN)(ASTRO_SLICE_T s)
{
    return s.cnt;
}

static inline ASTRO_REF_VALUE
ASTRO_REF_FN(_SLICE_GET)(ASTRO_SLICE_T s, uint32_t i)
{
    ASTRO_REF_CHECK(&s.p[i]);
    return s.p[i];
}

static inline void
ASTRO_REF_FN(_SLICE_SET)(ASTRO_SLICE_T s, uint32_t i, ASTRO_REF_VALUE v)
{
    ASTRO_REF_CHECK(&s.p[i]);
    s.p[i] = v;
}

static inline ASTRO_REF_T
ASTRO_REF_FN(_SLICE_REF)(ASTRO_SLICE_T s, uint32_t i)
{
    return (ASTRO_REF_T){ &s.p[i] };
}

#undef ASTRO_REF_T
#undef ASTRO_SLICE_T
#undef ASTRO_REF_FN
