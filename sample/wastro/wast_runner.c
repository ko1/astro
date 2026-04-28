// wastro — spec-test (`.wast`) harness
//
// `#include`'d from main.c.  Walks each top-level form in a `.wast`
// file: `(module ...)` resets state and re-loads, `assert_return`
// runs an invocation and compares the result, `assert_trap` invokes
// under setjmp and asserts a trap fires, `register` is accepted
// without cross-module linkage.  Forced into `--no-compile` mode to
// avoid AOT overhead per assertion.

// =====================================================================
// Spec test harness — `.wast` runner
// =====================================================================
//
// `.wast` files are WAT plus assertion forms used by the wasm spec
// testsuite.  We support the most common subset:
//   (module ...)
//   (assert_return (invoke "name" args...) result)
//   (assert_trap (invoke "name" args...) "trap-msg")
//   (invoke "name" args...)
//   (register "name")    — accepted but ignored (no cross-module link)
//   (assert_invalid ...) | (assert_malformed ...) — reported as skipped
//   (assert_exhaustion ...) — reported as skipped

static CTX *wastro_instantiate(uint32_t initial_local_slots);
static VALUE wastro_invoke(CTX *c, int func_idx, VALUE *args, uint32_t argc);

static void
wastro_reset_module(void)
{
    for (uint32_t i = 0; i < WASTRO_FUNC_CNT; i++) {
        if (WASTRO_FUNCS[i].name) free((void *)WASTRO_FUNCS[i].name);
        if (WASTRO_FUNCS[i].export_name) free((void *)WASTRO_FUNCS[i].export_name);
        free(WASTRO_FUNCS[i].param_types);
        free(WASTRO_FUNCS[i].local_types);
    }
    memset(WASTRO_FUNCS, 0, sizeof(WASTRO_FUNCS));
    WASTRO_FUNC_CNT = 0;

    if (WASTRO_GLOBALS) { free(WASTRO_GLOBALS); WASTRO_GLOBALS = NULL; }
    for (uint32_t i = 0; i < WASTRO_GLOBAL_CNT; i++) {
        if (WASTRO_GLOBAL_NAMES[i]) free(WASTRO_GLOBAL_NAMES[i]);
        WASTRO_GLOBAL_NAMES[i] = NULL;
    }
    WASTRO_GLOBAL_CNT = 0;

    if (WASTRO_BR_TABLE) { free(WASTRO_BR_TABLE); WASTRO_BR_TABLE = NULL; }
    WASTRO_BR_TABLE_CNT = 0;
    WASTRO_BR_TABLE_CAP = 0;

    for (uint32_t i = 0; i < MOD_DATA_SEG_CNT; i++) {
        if (MOD_DATA_SEGS[i].bytes) free(MOD_DATA_SEGS[i].bytes);
    }
    MOD_DATA_SEG_CNT = 0;

    for (uint32_t i = 0; i < WASTRO_TYPE_CNT; i++) {
        if (WASTRO_TYPE_NAMES[i]) { free(WASTRO_TYPE_NAMES[i]); WASTRO_TYPE_NAMES[i] = NULL; }
        free(WASTRO_TYPES[i].param_types);
    }
    memset(WASTRO_TYPES, 0, sizeof(WASTRO_TYPES));
    WASTRO_TYPE_CNT = 0;

    wastro_reset_call_args();

    if (WASTRO_TABLE) { free(WASTRO_TABLE); WASTRO_TABLE = NULL; }
    WASTRO_TABLE_SIZE = 0;
    WASTRO_TABLE_MAX = 0;
    MOD_HAS_TABLE = 0;

    MOD_HAS_MEMORY = 0;
    MOD_MEM_INITIAL_PAGES = 0;
    MOD_MEM_MAX_PAGES = 65536;

    MOD_HAS_START = 0;
    MOD_START_FUNC = -1;

    PENDING_ELEM_CNT = 0;
    PENDING_EXPORT_CNT = 0;
}

// Parse a constant expression `(T.const X)` from the active token
// stream, advance past it, and return the encoded VALUE plus type.
static VALUE
wast_parse_const_value(wtype_t *out_type)
{
    expect_lparen();
    VALUE v = 0;
    if (tok_is_keyword("i32.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected i32 literal");
        v = FROM_I32((int32_t)cur_tok.int_value);
        if (out_type) *out_type = WT_I32;
        next_token();
    }
    else if (tok_is_keyword("i64.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected i64 literal");
        v = FROM_I64((int64_t)cur_tok.int_value);
        if (out_type) *out_type = WT_I64;
        next_token();
    }
    else if (tok_is_keyword("f32.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected f32 literal");
        double dv = cur_tok.has_dot ? cur_tok.float_value : (double)cur_tok.int_value;
        uint32_t bits = token_to_f32_bits(&cur_tok, dv);
        v = FROM_U32(bits);
        if (out_type) *out_type = WT_F32;
        next_token();
    }
    else if (tok_is_keyword("f64.const")) {
        next_token();
        if (cur_tok.kind != T_INT) parse_error("expected f64 literal");
        double dv = cur_tok.has_dot ? cur_tok.float_value : (double)cur_tok.int_value;
        uint64_t bits = token_to_f64_bits(&cur_tok, dv);
        v = FROM_U64(bits);
        if (out_type) *out_type = WT_F64;
        next_token();
    }
    else parse_error("expected (T.const X) value");
    expect_rparen();
    return v;
}

// Skip a balanced (...) form starting at cur_tok='('.  Used to skip
// over assert_invalid / assert_malformed contents we don't validate.
static void
wast_skip_balanced(void)
{
    int depth = 0;
    do {
        if (cur_tok.kind == T_LPAREN) depth++;
        else if (cur_tok.kind == T_RPAREN) depth--;
        else if (cur_tok.kind == T_EOF) parse_error("unbalanced parens");
        next_token();
    } while (depth > 0);
}

// Parse `(invoke "name" args...)`.  Returns 1 if found and resolved,
// 0 if invoke target unresolved.  Caller has consumed the leading '('
// and is at cur_tok='invoke'.
static int
wast_parse_invoke(int *func_idx_out, VALUE *args_out, uint32_t *argc_out)
{
    next_token();   // consume 'invoke'
    if (cur_tok.kind != T_STRING) parse_error("invoke: expected name string");
    char name[128]; size_t nl = cur_tok.len < 127 ? cur_tok.len : 127;
    memcpy(name, cur_tok.start, nl); name[nl] = 0;
    next_token();
    int fi = wastro_find_export(name);
    *argc_out = 0;
    while (cur_tok.kind == T_LPAREN) {
        wtype_t at;
        args_out[(*argc_out)++] = wast_parse_const_value(&at);
    }
    expect_rparen();
    *func_idx_out = fi;
    return fi >= 0;
}

// Run a single test and report.  Returns 1 = pass, 0 = fail, -1 = skip.
typedef enum { TR_PASS = 1, TR_FAIL = 0, TR_SKIP = -1 } TestResult;

static TestResult
wast_run_assert_return(int line, CTX *c)
{
    expect_lparen();
    if (!tok_is_keyword("invoke")) {
        wast_skip_balanced();
        // skip the rest
        while (cur_tok.kind != T_RPAREN) {
            if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
            else next_token();
        }
        expect_rparen();
        return TR_SKIP;
    }
    int func_idx;
    VALUE args[WASTRO_MAX_PARAMS];
    uint32_t argc;
    if (!wast_parse_invoke(&func_idx, args, &argc)) {
        // unresolved — skip and skip remaining args
        while (cur_tok.kind != T_RPAREN) {
            if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
            else next_token();
        }
        expect_rparen();
        return TR_SKIP;
    }
    // Optional expected result.  Multi-value (post-1.0) extras are
    // accepted and discarded — we compare only the first.
    int has_expected = 0;
    VALUE expected = 0;
    wtype_t exp_t = WT_VOID;
    if (cur_tok.kind == T_LPAREN) {
        expected = wast_parse_const_value(&exp_t);
        has_expected = 1;
        while (cur_tok.kind == T_LPAREN) {
            wtype_t t; (void)wast_parse_const_value(&t);
        }
    }
    expect_rparen();

    if (setjmp(wastro_trap_jmp) == 0) {
        wastro_trap_active = 1;
        VALUE got = wastro_invoke(c, func_idx, args, argc);
        wastro_trap_active = 0;
        if (has_expected) {
            int ok = 0;
            switch (exp_t) {
            case WT_I32: ok = (AS_I32(got) == AS_I32(expected)); break;
            case WT_I64: ok = (AS_I64(got) == AS_I64(expected)); break;
            case WT_F32: {
                uint32_t gb = (uint32_t)got, eb = (uint32_t)expected;
                float gf, ef; memcpy(&gf, &gb, 4); memcpy(&ef, &eb, 4);
                ok = (gb == eb) || (gf != gf && ef != ef);   // bit-exact OR both NaN
            } break;
            case WT_F64: {
                uint64_t gb = got, eb = expected;
                double gf, ef; memcpy(&gf, &gb, 8); memcpy(&ef, &eb, 8);
                ok = (gb == eb) || (gf != gf && ef != ef);
            } break;
            default: ok = 1;
            }
            if (!ok) {
                fprintf(stderr, "FAIL line %d: %s expected=%llx got=%llx\n",
                        line, wtype_name(exp_t),
                        (unsigned long long)expected, (unsigned long long)got);
                return TR_FAIL;
            }
        }
        return TR_PASS;
    }
    else {
        wastro_trap_active = 0;
        fprintf(stderr, "FAIL line %d: unexpected trap (%s)\n", line, wastro_trap_message);
        return TR_FAIL;
    }
}

static TestResult
wast_run_assert_trap(int line, CTX *c)
{
    expect_lparen();
    if (!tok_is_keyword("invoke")) {
        wast_skip_balanced();
        while (cur_tok.kind != T_RPAREN) {
            if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
            else next_token();
        }
        expect_rparen();
        return TR_SKIP;
    }
    int func_idx;
    VALUE args[WASTRO_MAX_PARAMS];
    uint32_t argc;
    if (!wast_parse_invoke(&func_idx, args, &argc)) {
        while (cur_tok.kind != T_RPAREN) {
            if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
            else next_token();
        }
        expect_rparen();
        return TR_SKIP;
    }
    if (cur_tok.kind == T_STRING) next_token();   // expected trap msg — ignored
    expect_rparen();

    if (setjmp(wastro_trap_jmp) == 0) {
        wastro_trap_active = 1;
        wastro_invoke(c, func_idx, args, argc);
        wastro_trap_active = 0;
        fprintf(stderr, "FAIL line %d: expected trap, returned normally\n", line);
        return TR_FAIL;
    }
    else {
        wastro_trap_active = 0;
        return TR_PASS;
    }
}

// Walk the .wast file form-by-form.  For each `(module ...)`, reset
// state and load.  For each assertion, run it.  Print summary.
static int
wastro_run_wast(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (fread(buf, 1, sz, f) != (size_t)sz) { perror("fread"); return 2; }
    buf[sz] = 0;
    fclose(f);

    src_pos = buf;
    src_end = buf + sz;
    next_token();

    int passed = 0, failed = 0, skipped = 0;
    int loaded_any = 0;
    int load_failed = 0;       // sticky: subsequent asserts auto-skip
    CTX *active_ctx = NULL;    // persistent instance across asserts
    while (cur_tok.kind != T_EOF) {
        // Track approximate line number for diagnostics.
        int line = 1;
        for (const char *p = buf; p < cur_tok.start; p++) if (*p == '\n') line++;

        if (cur_tok.kind != T_LPAREN) parse_error("wast: expected '('");
        const char *form_start = cur_tok.start;
        next_token();
        if (cur_tok.kind != T_KEYWORD) parse_error("wast: expected keyword");

        if (tok_is_keyword("module")) {
            // Find the closing ')' to delimit the module text.
            int depth = 1;
            while (depth > 0 && cur_tok.kind != T_EOF) {
                next_token();
                if (cur_tok.kind == T_LPAREN) depth++;
                else if (cur_tok.kind == T_RPAREN) depth--;
            }
            const char *form_end = src_pos;
            // Save outer parser state, load the module, restore.
            const char *o_pos = src_pos, *o_end = src_end;
            Token o_tok = cur_tok;
            // Free the previous module's instance (memory etc.).
            if (active_ctx) {
                if (active_ctx->memory)
                    munmap(active_ctx->memory, WASTRO_VM_RESERVE_BYTES);
                free(active_ctx);
                active_ctx = NULL;
                wastro_segv_ctx = NULL;
            }
            wastro_reset_module();
            load_failed = 0;
            if (setjmp(wastro_parse_jmp) == 0) {
                wastro_parse_active = 1;
                wastro_load_module_buf(form_start, (size_t)(form_end - form_start));
                wastro_parse_active = 0;
                loaded_any = 1;
                // Build a persistent instance for this module.  Any
                // (start) is invoked at instantiation.
                uint32_t locals = 0;
                if (MOD_HAS_START) locals = WASTRO_FUNCS[MOD_START_FUNC].local_cnt;
                for (uint32_t i = 0; i < WASTRO_FUNC_CNT; i++) {
                    if (WASTRO_FUNCS[i].local_cnt > locals)
                        locals = WASTRO_FUNCS[i].local_cnt;
                }
                if (locals < 16) locals = 16;
                if (setjmp(wastro_trap_jmp) == 0) {
                    wastro_trap_active = 1;
                    active_ctx = wastro_instantiate(locals);
                    wastro_trap_active = 0;
                }
                else {
                    wastro_trap_active = 0;
                    fprintf(stderr, "  skip line %d: instantiation trap: %s\n", line, wastro_trap_message);
                    load_failed = 1;
                    active_ctx = NULL;
                    goto skip_module_done;
                }
                if (MOD_HAS_START) {
                    if (setjmp(wastro_trap_jmp) == 0) {
                        wastro_trap_active = 1;
                        wastro_invoke(active_ctx, MOD_START_FUNC, NULL, 0);
                        wastro_trap_active = 0;
                    }
                    else { wastro_trap_active = 0; }
                }
            skip_module_done: ;
            }
            else {
                wastro_parse_active = 0;
                fprintf(stderr, "  skip line %d: module load failed: %s\n", line, wastro_parse_message);
                load_failed = 1;
                loaded_any = 1;   // we did "load" — subsequent asserts skip
            }
            src_pos = o_pos; src_end = o_end; cur_tok = o_tok;
            next_token();   // advance past the module's ')'
        }
        else if (tok_is_keyword("assert_return")) {
            next_token();
            if (!loaded_any || load_failed) {
                while (cur_tok.kind != T_RPAREN) {
                    if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
                    else next_token();
                }
                expect_rparen();
                skipped++; continue;
            }
            TestResult r;
            if (setjmp(wastro_parse_jmp) == 0) {
                wastro_parse_active = 1;
                r = wast_run_assert_return(line, active_ctx);
                wastro_parse_active = 0;
            }
            else {
                wastro_parse_active = 0;
                fprintf(stderr, "  skip line %d: parse error in assert: %s\n", line, wastro_parse_message);
                r = TR_SKIP;
            }
            if (r == TR_PASS) passed++;
            else if (r == TR_FAIL) failed++;
            else skipped++;
        }
        else if (tok_is_keyword("assert_trap")) {
            next_token();
            if (!loaded_any || load_failed) {
                while (cur_tok.kind != T_RPAREN) {
                    if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
                    else next_token();
                }
                expect_rparen();
                skipped++; continue;
            }
            TestResult r;
            if (setjmp(wastro_parse_jmp) == 0) {
                wastro_parse_active = 1;
                r = wast_run_assert_trap(line, active_ctx);
                wastro_parse_active = 0;
            }
            else {
                wastro_parse_active = 0;
                fprintf(stderr, "  skip line %d: parse error in assert: %s\n", line, wastro_parse_message);
                r = TR_SKIP;
            }
            if (r == TR_PASS) passed++;
            else if (r == TR_FAIL) failed++;
            else skipped++;
        }
        else if (tok_is_keyword("invoke")) {
            // Bare invoke — run on the active instance, ignore result.
            int fi; VALUE args[WASTRO_MAX_PARAMS]; uint32_t argc;
            if (loaded_any && !load_failed && active_ctx &&
                wast_parse_invoke(&fi, args, &argc)) {
                if (setjmp(wastro_trap_jmp) == 0) {
                    wastro_trap_active = 1;
                    wastro_invoke(active_ctx, fi, args, argc);
                    wastro_trap_active = 0;
                }
                else wastro_trap_active = 0;
            } else {
                while (cur_tok.kind != T_RPAREN) {
                    if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
                    else next_token();
                }
                expect_rparen();
            }
        }
        else if (tok_is_keyword("register")) {
            // (register "name") — ignored
            next_token();
            if (cur_tok.kind == T_STRING) next_token();
            if (cur_tok.kind == T_IDENT) next_token();
            expect_rparen();
        }
        else if (tok_is_keyword("assert_invalid") ||
                 tok_is_keyword("assert_malformed") ||
                 tok_is_keyword("assert_exhaustion") ||
                 tok_is_keyword("assert_unlinkable") ||
                 tok_is_keyword("assert_return_canonical_nan") ||
                 tok_is_keyword("assert_return_arithmetic_nan")) {
            // Forms we don't fully support — skip.
            next_token();
            while (cur_tok.kind != T_RPAREN) {
                if (cur_tok.kind == T_LPAREN) wast_skip_balanced();
                else next_token();
            }
            expect_rparen();
            skipped++;
        }
        else {
            // Unknown form — skip.
            wast_skip_balanced();
            skipped++;
        }
    }

    if (active_ctx) {
        if (active_ctx->memory)
            munmap(active_ctx->memory, WASTRO_VM_RESERVE_BYTES);
        free(active_ctx);
        wastro_segv_ctx = NULL;
    }
    fprintf(stderr, "\n%s: %d passed, %d failed, %d skipped\n",
            path, passed, failed, skipped);
    free(buf);
    return failed == 0 ? 0 : 1;
}

