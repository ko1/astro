/* Hand-written emit-time helpers for `--build` AST embedding.  Included into
 * node.c BEFORE node_emit_ast.c; the generated EMIT_AST_* functions call these
 * (via koruby_gen.rb's Operand#build_emit_ast overrides) to print expressions
 * for operands the framework default cannot embed:
 *
 *   - symbol IDs (per-process; re-interned at exe startup via `_ectx`)
 *   - byte arrays with embedded NULs (length-aware C literals)
 *   - the parse-built side structures behind `void *` operands
 *
 * The printed expressions call korb_embed_* (korb_runtime.c) inside the
 * generated builder function `NODE *fn(CTX *_ectx)`.  Anything genuinely
 * unsupported prints an undeclared identifier so the exe build fails loudly
 * instead of silently mis-embedding. */

/* Emit runs in the bake process, where symbol names must be resolved from the
 * live VM.  Set once by the --build driver before emitting. */
static const struct korb_vm *koruby_emit_vm;

void
koruby_emit_set_vm(const struct korb_vm *vm)
{
    koruby_emit_vm = vm;
}

/* "..." with octal escapes; NUL-safe (a C literal may contain \000, and octal
 * escapes are capped at 3 digits so a following digit can't extend them).
 * Non-static: the --build driver (main.c) also uses it for metadata strings. */
void
koruby_emit_cstr_len(FILE *fp, const char *p, uint32_t len)
{
    fputc('"', fp);
    for (uint32_t i = 0; i < len; i++) {
        const unsigned char ch = (unsigned char)p[i];
        if (ch == '"' || ch == '\\') { fputc('\\', fp); fputc(ch, fp); }
        else if (ch >= 0x20 && ch < 0x7f) fputc(ch, fp);
        else fprintf(fp, "\\%03o", ch);
    }
    fputc('"', fp);
}

/* `"name", <len>` — the argument pair every re-interning helper takes. */
static void
koruby_emit_sym_args(FILE *fp, uint32_t id)
{
    const char *const name = korb_sym_name(koruby_emit_vm, id);
    if (name == NULL) {
        fprintf(stderr, "koruby_emit: symbol id %u has no name\n", id);
        fprintf(fp, "KORUBY_EMBED_UNKNOWN_SYMBOL");
        return;
    }
    const uint32_t len = (uint32_t)strlen(name);
    koruby_emit_cstr_len(fp, name, len);
    fprintf(fp, ", %uU", len);
}

/* @sym scalar operand: `korb_intern(_ectx->vm, "name", len)`; 0 stays 0. */
void
koruby_emit_intern(FILE *fp, uint32_t id)
{
    if (id == 0) { fprintf(fp, "0U"); return; }
    fprintf(fp, "korb_intern(_ectx->vm, ");
    koruby_emit_sym_args(fp, id);
    fprintf(fp, ")");
}

/* VALUE operand (node_lit): immediates are process-independent bit patterns;
 * symbols re-intern.  Heap VALUEs never occur in parse-built literals
 * (bignum/float/rational have their own nodes) — fail loudly if one does. */
void
koruby_emit_value(FILE *fp, VALUE v)
{
    if (SYMBOL_P(v)) {
        fprintf(fp, "ID2SYM(korb_intern(_ectx->vm, ");
        koruby_emit_sym_args(fp, SYM2ID(v));
        fprintf(fp, "))");
    }
    else if (v == KORB_NIL || FIXNUM_P(v) || FLONUM_P(v) || KORB_SPECIAL_P(v)) {
        fprintf(fp, "(VALUE)0x%llxLL", (unsigned long long)KORB_WORD(v));
    }
    else {
        fprintf(stderr, "koruby_emit: heap VALUE operand cannot be embedded\n");
        fprintf(fp, "KORUBY_EMBED_HEAP_VALUE");
    }
}

/* @children operand: prints BOTH ALLOC arguments (`<argv>, <cnt>`). */
void
koruby_emit_children(FILE *fp, NODE **argv, uint32_t cnt)
{
    if (cnt == 0) { fprintf(fp, "NULL, 0U"); return; }
    fprintf(fp, "korb_embed_nodes(%uU", cnt);
    for (uint32_t i = 0; i < cnt; i++) {
        fprintf(fp, ", ");
        astro_emit_ast_c_child(fp, argv[i]);
    }
    fprintf(fp, "), %uU", cnt);
}

/* Symbol-ID array behind a `const char *` operand (kw_syms / mids / name_syms). */
void
koruby_emit_syms(FILE *fp, const char *ptr, uint32_t cnt)
{
    if (ptr == NULL || cnt == 0) { fprintf(fp, "NULL"); return; }
    const uint32_t *const ids = (const uint32_t *)(const void *)ptr;
    fprintf(fp, "(const char *)korb_embed_syms(_ectx, %uU", cnt);
    for (uint32_t i = 0; i < cnt; i++) {
        fprintf(fp, ", ");
        koruby_emit_sym_args(fp, ids[i]);
    }
    fprintf(fp, ")");
}

/* opt_defaults: NODE *[cnt] (default-value expression per optional param). */
void
koruby_emit_opt_defaults(FILE *fp, void *p, uint32_t cnt)
{
    if (p == NULL) { fprintf(fp, "NULL"); return; }
    NODE **const defs = (NODE **)p;
    fprintf(fp, "(void *)korb_embed_nodes(%uU", cnt);
    for (uint32_t i = 0; i < cnt; i++) {
        fprintf(fp, ", ");
        astro_emit_ast_c_child(fp, defs[i]);
    }
    fprintf(fp, ")");
}

/* kw_info: {count, kwrest_slot, entries[mid, slot, deflt]} */
void
koruby_emit_kw_info(FILE *fp, void *p)
{
    if (p == NULL) { fprintf(fp, "NULL"); return; }
    const struct korb_kw_info *const kw = (const struct korb_kw_info *)p;
    fprintf(fp, "korb_embed_kw_info(_ectx, %uU, %d", kw->count, kw->kwrest_slot);
    for (uint32_t i = 0; i < kw->count; i++) {
        fprintf(fp, ", ");
        koruby_emit_sym_args(fp, kw->entries[i].mid);
        fprintf(fp, ", %uU, ", kw->entries[i].slot);
        astro_emit_ast_c_child(fp, kw->entries[i].deflt);
    }
    fprintf(fp, ")");
}

/* param_info: {n, e[kind, name]} — #parameters metadata (cold). */
void
koruby_emit_param_info(FILE *fp, void *p)
{
    if (p == NULL) { fprintf(fp, "NULL"); return; }
    const struct korb_param_info *const pi = (const struct korb_param_info *)p;
    fprintf(fp, "korb_embed_param_info(_ectx, %uU", pi->n);
    for (uint32_t i = 0; i < pi->n; i++) {
        fprintf(fp, ", %uU, ", (unsigned)pi->e[i].kind);
        if (pi->e[i].name == 0) fprintf(fp, "NULL, 0U");
        else koruby_emit_sym_args(fp, pi->e[i].name);
    }
    fprintf(fp, ")");
}

/* Small scalar arrays behind void* operands. */
void
koruby_emit_u8s(FILE *fp, const void *p, uint32_t cnt)
{
    if (p == NULL) { fprintf(fp, "NULL"); return; }
    const uint8_t *const a = (const uint8_t *)p;
    fprintf(fp, "korb_embed_u8(%uU", cnt);
    for (uint32_t i = 0; i < cnt; i++) fprintf(fp, ", %u", a[i]);
    fprintf(fp, ")");
}

void
koruby_emit_u16s(FILE *fp, const void *p, uint32_t cnt)
{
    if (p == NULL) { fprintf(fp, "NULL"); return; }
    const uint16_t *const a = (const uint16_t *)p;
    fprintf(fp, "korb_embed_u16(%uU", cnt);
    for (uint32_t i = 0; i < cnt; i++) fprintf(fp, ", %u", a[i]);
    fprintf(fp, ")");
}

void
koruby_emit_i32s(FILE *fp, const void *p, uint32_t cnt)
{
    if (p == NULL) { fprintf(fp, "NULL"); return; }
    const int32_t *const a = (const int32_t *)p;
    fprintf(fp, "korb_embed_i32(%uU", cnt);
    for (uint32_t i = 0; i < cnt; i++) fprintf(fp, ", %d", a[i]);
    fprintf(fp, ")");
}

/* node_massign_het descs: {kind, data}[cnt]; data is a slot for kind 0 but a
 * symbol ID for kinds 1 (@ivar) / 2 (CONST) — re-intern those. */
void
koruby_emit_het_descs(FILE *fp, const void *p, uint32_t cnt)
{
    if (p == NULL) { fprintf(fp, "NULL"); return; }
    const struct { int32_t kind; int32_t data; } *const d = p;
    fprintf(fp, "korb_embed_het_descs(_ectx, %uU", cnt);
    for (uint32_t i = 0; i < cnt; i++) {
        fprintf(fp, ", %d, ", d[i].kind);
        if (d[i].kind == 0) fprintf(fp, "%d, NULL, 0U", d[i].data);
        else { fprintf(fp, "0, "); koruby_emit_sym_args(fp, (uint32_t)d[i].data); }
    }
    fprintf(fp, ")");
}

/* node_attr descs: {mid, ivar, is_writer}[cnt] — both IDs re-intern. */
void
koruby_emit_attr_descs(FILE *fp, const void *p, uint32_t cnt)
{
    if (p == NULL) { fprintf(fp, "NULL"); return; }
    const struct korb_attr_desc *const d = (const struct korb_attr_desc *)p;
    fprintf(fp, "korb_embed_attr_descs(_ectx, %uU", cnt);
    for (uint32_t i = 0; i < cnt; i++) {
        fprintf(fp, ", ");
        koruby_emit_sym_args(fp, d[i].mid);
        fprintf(fp, ", ");
        koruby_emit_sym_args(fp, d[i].ivar);
        fprintf(fp, ", %u", (unsigned)d[i].is_writer);
    }
    fprintf(fp, ")");
}

/* Pattern-matching descriptor: recursive struct korb_pat.  Sub-patterns and
 * the value NODE nest as expressions; hash keys are symbol VALUEs. */
void
koruby_emit_pat(FILE *fp, const void *p)
{
    if (p == NULL) { fprintf(fp, "NULL"); return; }
    const struct korb_pat *const pat = (const struct korb_pat *)p;
    fprintf(fp, "korb_embed_pat(_ectx, %u, %d, ", (unsigned)pat->kind, pat->bind_off);
    astro_emit_ast_c_child(fp, pat->value_node);
    fprintf(fp, ", %uU, %uU", pat->n, pat->npost);
    /* elems count by kind (korb_pat_match): 2/3/5/8 → n, 4 → 1, 6 → n+npost. */
    uint32_t ecnt = 0;
    if (pat->elems != NULL) {
        switch (pat->kind) {
          case 2: case 3: case 5: case 8: ecnt = pat->n; break;
          case 4: ecnt = 1; break;
          case 6: ecnt = pat->n + pat->npost; break;
          default: ecnt = 0; break;
        }
    }
    fprintf(fp, ", %uU", ecnt);
    for (uint32_t i = 0; i < ecnt; i++) {
        fprintf(fp, ", ");
        koruby_emit_pat(fp, pat->elems[i]);
    }
    const uint32_t kcnt = (pat->kind == 3 && pat->keys != NULL) ? pat->n : 0;
    fprintf(fp, ", %uU", kcnt);
    for (uint32_t i = 0; i < kcnt; i++) {
        fprintf(fp, ", ");
        koruby_emit_value(fp, pat->keys[i]);
    }
    fprintf(fp, ")");
}
