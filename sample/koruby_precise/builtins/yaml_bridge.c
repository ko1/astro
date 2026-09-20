/* libyaml への橋渡し。
 *
 * CRuby の psych は C 拡張が Psych::Parser / Psych::Emitter の 2 クラスだけを
 * 提供し、残り (nodes / visitors / tree_builder …) は全部 Ruby。koruby も同じ形に
 * する: ここは libyaml のイベント列を Ruby の配列にして渡す/受け取るだけで、
 * lib/psych/ 以下の Ruby 実装がそのまま載る。
 *
 *   __yaml_parse(src)      -> [[kind, ...], ...]  (パースエラーは配列でなく
 *                             [:error, message, line, column] を返す)
 *   __yaml_emit(events, opts) -> String
 *
 * イベントの並びは Psych::Handler のコールバックと 1:1:
 *   0 stream_start(enc)                 5 sequence_start(anchor, tag, implicit, style)
 *   1 document_start(ver, tags, impl)   6 sequence_end
 *   2 document_end(implicit)            7 mapping_start(anchor, tag, implicit, style)
 *   3 alias(anchor)                     8 mapping_end
 *   4 scalar(val, anchor, tag, plain, quoted, style)   9 stream_end
 * どのイベントも末尾に start_line, start_column, end_line, end_column を持つ。 */
#ifdef KORB_HAVE_YAML
#include <yaml.h>

/* 配列 1 本を作って `n` 個の値を積む。値は呼び出し側が VALUE で持っているが、
 * korb_ary_push_val が中で root してくれるので直列に積んでよい。 */
static RESULT korb_yaml_ev(CTX *c, VALUE *slots, VALUE_REF dst, const VALUE *vals, uint32_t n)
{
    slots[0] = UNWRAP(korb_ary_new(c, slots, n));
    { VALUE_REF r = VALUE_REF_AT(&slots[0]);
      for (uint32_t i = 0; i < n; i++) CHECK(korb_ary_push_val(c, slots + 1, r, vals[i])); }
    return korb_ary_push_val(c, slots + 1, dst, slots[0]);
}

/* libyaml の文字列は yaml_char_t* (UTF-8)。NULL は nil に。 */
static RESULT korb_yaml_str(CTX *c, VALUE *slots, const yaml_char_t *p, size_t len)
{
    if (p == NULL) return RESULT_OK(KORB_NIL);
    return korb_str_new(c, slots, (const char *)p, (uint32_t)len);
}

static RESULT
korb_bi_yaml_parse(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    const VALUE sv = VALUE_SLICE_GET(args, 0);
    if (UNLIKELY(!KORB_STRING_P(sv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, sv));
    const KorbString *const ss = VAL2STR(sv);
    const size_t slen = ss->len;
    unsigned char *const src = malloc(slen + 1);
    if (!src) { fprintf(stderr, "koruby_precise: oom (yaml source)\n"); abort(); }
    memcpy(src, korb_strbuf_data(ss->buf), slen);
    src[slen] = '\0';

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) { free(src); return korb_raise(c, slots, KORB_E_RUNTIME, 0, "yaml_parser_initialize failed"); }
    yaml_parser_set_input_string(&parser, src, slen);

    slots[0] = UNWRAP(korb_ary_new(c, slots, 16));
    VALUE_REF out = VALUE_REF_AT(&slots[0]);
    VALUE *const sp = slots + 1;                 /* 以降の一時領域 */
    for (;;) {
        yaml_event_t ev;
        if (!yaml_parser_parse(&parser, &ev)) {
            const char *const msg = parser.problem ? parser.problem : "unknown error";
            const size_t line = parser.problem_mark.line + 1, col = parser.problem_mark.column + 1;
            const char *const ctxs = parser.context;
            char buf[512];
            snprintf(buf, sizeof buf, "%s%s%s", ctxs ? ctxs : "", ctxs ? ": " : "", msg);
            yaml_parser_delete(&parser); free(src);
            VALUE e[4];
            e[0] = ID2SYM(korb_intern(c->vm, "error", 5));
            e[1] = UNWRAP(korb_str_new(c, sp, buf, (uint32_t)strlen(buf)));
            e[2] = LONG2FIX((korb_sword_t)line);
            e[3] = LONG2FIX((korb_sword_t)col);
            slots[0] = UNWRAP(korb_ary_new(c, slots, 1));          /* [[:error, msg, line, col]] */
            { VALUE_REF r = VALUE_REF_AT(&slots[0]);
              CHECK(korb_yaml_ev(c, slots + 1, r, e, 4)); }
            return RESULT_OK(slots[0]);
        }
        /* alloc を跨いで C ローカルに VALUE を持たない: イベント配列を先に作り、
         * 値は作った直後に push する (String の生成が前の String を動かす)。 */
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 8));
        VALUE_REF eva = VALUE_REF_AT(&slots[1]);
#define YPUSH(v)        CHECK(korb_ary_push_val(c, slots + 2, eva, (v)))
#define YPUSH_STR(p, n) do { const RESULT r_ = korb_yaml_str(c, slots + 2, (p), (n)); CHECK(r_); YPUSH(r_.value); } while (0)
#define YPUSH_CSTR(p)   YPUSH_STR((p), (p) ? strlen((const char *)(p)) : 0)
        switch (ev.type) {
          case YAML_STREAM_START_EVENT:
            YPUSH(LONG2FIX(0)); YPUSH(LONG2FIX((korb_sword_t)ev.data.stream_start.encoding));
            break;
          case YAML_DOCUMENT_START_EVENT:
            YPUSH(LONG2FIX(1));
            if (ev.data.document_start.version_directive) {
                slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
                VALUE_REF vr = VALUE_REF_AT(&slots[2]);
                CHECK(korb_ary_push_val(c, slots + 3, vr, LONG2FIX(ev.data.document_start.version_directive->major)));
                CHECK(korb_ary_push_val(c, slots + 3, vr, LONG2FIX(ev.data.document_start.version_directive->minor)));
                YPUSH(VALUE_REF_GET(vr));
            } else {
                YPUSH(KORB_NIL);
            }
            slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 0));      /* tag directives */
            { VALUE_REF tr = VALUE_REF_AT(&slots[2]);
              for (yaml_tag_directive_t *td = ev.data.document_start.tag_directives.start;
                   td && td < ev.data.document_start.tag_directives.end; td++) {
                  slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 2));
                  VALUE_REF pr = VALUE_REF_AT(&slots[3]);
                  { const RESULT h_ = korb_yaml_str(c, slots + 4, td->handle, strlen((const char *)td->handle));
                    CHECK(h_); CHECK(korb_ary_push_val(c, slots + 4, pr, h_.value)); }
                  { const RESULT p_ = korb_yaml_str(c, slots + 4, td->prefix, strlen((const char *)td->prefix));
                    CHECK(p_); CHECK(korb_ary_push_val(c, slots + 4, pr, p_.value)); }
                  CHECK(korb_ary_push_val(c, slots + 4, tr, VALUE_REF_GET(pr)));
              }
              YPUSH(VALUE_REF_GET(tr)); }
            YPUSH(ev.data.document_start.implicit ? KORB_TRUE : KORB_FALSE);
            break;
          case YAML_DOCUMENT_END_EVENT:
            YPUSH(LONG2FIX(2)); YPUSH(ev.data.document_end.implicit ? KORB_TRUE : KORB_FALSE);
            break;
          case YAML_ALIAS_EVENT:
            YPUSH(LONG2FIX(3)); YPUSH_CSTR(ev.data.alias.anchor);
            break;
          case YAML_SCALAR_EVENT:
            YPUSH(LONG2FIX(4));
            YPUSH_STR(ev.data.scalar.value, ev.data.scalar.length);
            YPUSH_CSTR(ev.data.scalar.anchor);
            YPUSH_CSTR(ev.data.scalar.tag);
            YPUSH(ev.data.scalar.plain_implicit ? KORB_TRUE : KORB_FALSE);
            YPUSH(ev.data.scalar.quoted_implicit ? KORB_TRUE : KORB_FALSE);
            YPUSH(LONG2FIX((korb_sword_t)ev.data.scalar.style));
            break;
          case YAML_SEQUENCE_START_EVENT:
            YPUSH(LONG2FIX(5));
            YPUSH_CSTR(ev.data.sequence_start.anchor);
            YPUSH_CSTR(ev.data.sequence_start.tag);
            YPUSH(ev.data.sequence_start.implicit ? KORB_TRUE : KORB_FALSE);
            YPUSH(LONG2FIX((korb_sword_t)ev.data.sequence_start.style));
            break;
          case YAML_SEQUENCE_END_EVENT: YPUSH(LONG2FIX(6)); break;
          case YAML_MAPPING_START_EVENT:
            YPUSH(LONG2FIX(7));
            YPUSH_CSTR(ev.data.mapping_start.anchor);
            YPUSH_CSTR(ev.data.mapping_start.tag);
            YPUSH(ev.data.mapping_start.implicit ? KORB_TRUE : KORB_FALSE);
            YPUSH(LONG2FIX((korb_sword_t)ev.data.mapping_start.style));
            break;
          case YAML_MAPPING_END_EVENT: YPUSH(LONG2FIX(8)); break;
          case YAML_STREAM_END_EVENT:  YPUSH(LONG2FIX(9)); break;
          default: YPUSH(LONG2FIX(-1)); break;
        }
        /* 末尾は start(index, line, column) / end(index, line, column) */
        YPUSH(LONG2FIX((korb_sword_t)ev.start_mark.index));
        YPUSH(LONG2FIX((korb_sword_t)ev.start_mark.line));
        YPUSH(LONG2FIX((korb_sword_t)ev.start_mark.column));
        YPUSH(LONG2FIX((korb_sword_t)ev.end_mark.index));
        YPUSH(LONG2FIX((korb_sword_t)ev.end_mark.line));
        YPUSH(LONG2FIX((korb_sword_t)ev.end_mark.column));
#undef YPUSH_CSTR
#undef YPUSH_STR
#undef YPUSH
        const bool done = ev.type == YAML_STREAM_END_EVENT;
        yaml_event_delete(&ev);
        CHECK(korb_ary_push_val(c, slots + 2, out, VALUE_REF_GET(eva)));
        if (done) break;
    }
    yaml_parser_delete(&parser);
    free(src);
    return RESULT_OK(VALUE_REF_GET(out));
}

/* --- emitter --------------------------------------------------------------- */

struct korb_yaml_out { char *buf; size_t len, capa; };

static int korb_yaml_write(void *data, unsigned char *buffer, size_t size)
{
    struct korb_yaml_out *const o = data;
    if (o->len + size + 1 > o->capa) {
        size_t nc = o->capa ? o->capa * 2 : 1024;
        while (nc < o->len + size + 1) nc *= 2;
        o->buf = realloc(o->buf, nc);
        if (!o->buf) { fprintf(stderr, "koruby_precise: oom (yaml out)\n"); abort(); }
        o->capa = nc;
    }
    memcpy(o->buf + o->len, buffer, size);
    o->len += size;
    return 1;
}

/* 1 イベント分の文字列引数を libc 側へ写す (この先で alloc は起きないが、
 * libyaml が保持するので寿命を合わせる)。 */
static char *korb_yaml_dup(VALUE v)
{
    if (!KORB_STRING_P(v)) return NULL;
    const KorbString *const s = VAL2STR(v);
    char *const p = malloc(s->len + 1);
    if (!p) { fprintf(stderr, "koruby_precise: oom (yaml arg)\n"); abort(); }
    memcpy(p, korb_strbuf_data(s->buf), s->len); p[s->len] = '\0';
    return p;
}

static RESULT
korb_bi_yaml_emit(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    const VALUE evs = VALUE_SLICE_GET(args, 0);
    if (UNLIKELY(!KORB_ARRAY_P(evs)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Array");
    const VALUE opts = VALUE_SLICE_LEN(args) > 1 ? VALUE_SLICE_GET(args, 1) : KORB_NIL;
    korb_sword_t indent = 2, width = 80;
    bool canonical = false;
    if (KORB_ARRAY_P(opts) && VAL2ARY(opts)->len >= 3) {
        const VALUE *const o = korb_items_data(VAL2ARY(opts)->items);
        if (FIXNUM_P(o[0])) indent = FIX2LONG(o[0]);
        if (FIXNUM_P(o[1])) width = FIX2LONG(o[1]);
        canonical = o[2] == KORB_TRUE;
    }

    yaml_emitter_t em;
    if (!yaml_emitter_initialize(&em)) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "yaml_emitter_initialize failed");
    struct korb_yaml_out out = { NULL, 0, 0 };
    yaml_emitter_set_output(&em, korb_yaml_write, &out);
    yaml_emitter_set_indent(&em, (int)indent);
    yaml_emitter_set_width(&em, (int)width);
    yaml_emitter_set_canonical(&em, canonical ? 1 : 0);
    yaml_emitter_set_unicode(&em, 1);

    const char *err = NULL;
    const KorbArray *const ea = VAL2ARY(evs);
    for (uint32_t i = 0; i < ea->len && err == NULL; i++) {
        const VALUE evv = korb_items_data(ea->items)[i];
        if (!KORB_ARRAY_P(evv) || VAL2ARY(evv)->len == 0) continue;
        const VALUE *const f = korb_items_data(VAL2ARY(evv)->items);
        const korb_sword_t kind = FIXNUM_P(f[0]) ? FIX2LONG(f[0]) : -1;
        yaml_event_t ev;
        int ok = 0;
        char *a = NULL, *t = NULL, *val = NULL;
        yaml_tag_directive_t td_saved[64];
        uint32_t td_free = 0;
        switch (kind) {
          case 0: ok = yaml_stream_start_event_initialize(&ev, YAML_UTF8_ENCODING); break;
          case 1: {
            yaml_version_directive_t vd, *vdp = NULL;
            if (KORB_ARRAY_P(f[1]) && VAL2ARY(f[1])->len >= 2) {
                const VALUE *const vv = korb_items_data(VAL2ARY(f[1])->items);
                vd.major = (int)(FIXNUM_P(vv[0]) ? FIX2LONG(vv[0]) : 1);
                vd.minor = (int)(FIXNUM_P(vv[1]) ? FIX2LONG(vv[1]) : 1);
                vdp = &vd;
            }
            yaml_tag_directive_t tds[64];
            uint32_t ntd = 0;
            if (KORB_ARRAY_P(f[2])) {
                const KorbArray *const ta = VAL2ARY(f[2]);
                for (uint32_t k = 0; k < ta->len && ntd < 64; k++) {
                    const VALUE pv = korb_items_data(ta->items)[k];
                    if (!KORB_ARRAY_P(pv) || VAL2ARY(pv)->len < 2) continue;
                    const VALUE *const pf = korb_items_data(VAL2ARY(pv)->items);
                    tds[ntd].handle = (yaml_char_t *)korb_yaml_dup(pf[0]);
                    tds[ntd].prefix = (yaml_char_t *)korb_yaml_dup(pf[1]);
                    if (tds[ntd].handle && tds[ntd].prefix) ntd++;
                    else { free(tds[ntd].handle); free(tds[ntd].prefix); }
                }
            }
            ok = yaml_document_start_event_initialize(&ev, vdp, ntd ? tds : NULL, ntd ? tds + ntd : NULL,
                                                      f[3] != KORB_FALSE);
            if (!ok) for (uint32_t k = 0; k < ntd; k++) { free(tds[k].handle); free(tds[k].prefix); }
            else { td_free = ntd; for (uint32_t k = 0; k < ntd; k++) td_saved[k] = tds[k]; }
            break;
          }
          case 2: ok = yaml_document_end_event_initialize(&ev, f[1] != KORB_FALSE); break;
          case 3: a = korb_yaml_dup(f[1]); ok = yaml_alias_event_initialize(&ev, (yaml_char_t *)a); break;
          case 4: {
            val = korb_yaml_dup(f[1]); a = korb_yaml_dup(f[2]); t = korb_yaml_dup(f[3]);
            const size_t vlen = KORB_STRING_P(f[1]) ? VAL2STR(f[1])->len : 0;
            ok = yaml_scalar_event_initialize(&ev, (yaml_char_t *)a, (yaml_char_t *)t, (yaml_char_t *)(val ? val : ""),
                                              (int)vlen, f[4] != KORB_FALSE, f[5] != KORB_FALSE,
                                              (yaml_scalar_style_t)(FIXNUM_P(f[6]) ? FIX2LONG(f[6]) : YAML_ANY_SCALAR_STYLE));
            break;
          }
          case 5:
            a = korb_yaml_dup(f[1]); t = korb_yaml_dup(f[2]);
            ok = yaml_sequence_start_event_initialize(&ev, (yaml_char_t *)a, (yaml_char_t *)t, f[3] != KORB_FALSE,
                                                      (yaml_sequence_style_t)(FIXNUM_P(f[4]) ? FIX2LONG(f[4]) : YAML_ANY_SEQUENCE_STYLE));
            break;
          case 6: ok = yaml_sequence_end_event_initialize(&ev); break;
          case 7:
            a = korb_yaml_dup(f[1]); t = korb_yaml_dup(f[2]);
            ok = yaml_mapping_start_event_initialize(&ev, (yaml_char_t *)a, (yaml_char_t *)t, f[3] != KORB_FALSE,
                                                     (yaml_mapping_style_t)(FIXNUM_P(f[4]) ? FIX2LONG(f[4]) : YAML_ANY_MAPPING_STYLE));
            break;
          case 8: ok = yaml_mapping_end_event_initialize(&ev); break;
          case 9: ok = yaml_stream_end_event_initialize(&ev); break;
          default: ok = 0; break;
        }
        if (!ok) err = "failed to initialize a YAML event";
        else if (!yaml_emitter_emit(&em, &ev)) err = em.problem ? em.problem : "emitter error";
        free(a); free(t); free(val);
        for (uint32_t k = 0; k < td_free; k++) { free(td_saved[k].handle); free(td_saved[k].prefix); }
    }
    yaml_emitter_delete(&em);
    if (err != NULL) { free(out.buf); return korb_raise(c, slots, KORB_E_RUNTIME, 0, "%s", err); }
    const RESULT r = korb_str_new(c, slots, out.buf ? out.buf : "", (uint32_t)out.len);
    free(out.buf);
    return r;
}
#endif  /* KORB_HAVE_YAML */
