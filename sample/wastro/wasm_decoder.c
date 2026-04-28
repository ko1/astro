// wastro — binary `.wasm` decoder
//
// `#include`'d from main.c.  Section-by-section decoder for wasm 1.0
// + saturating-trunc, populating the same module-state arrays that
// the WAT parser uses.  Magic detection in `wastro_load_module`
// dispatches WAT vs. binary automatically.

// =====================================================================
// Binary .wasm decoder
// =====================================================================
//
// Decodes a wasm 1.0 binary module into the same in-memory state
// (WASTRO_TYPES, WASTRO_FUNCS, WASTRO_GLOBALS, WASTRO_TABLE,
// MOD_DATA_SEGS, ...) that the WAT parser populates.  Function bodies
// (Code section) are converted to AST via the same OpStack/StmtList
// machinery used by the stack-style WAT parser.

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} BinReader;

static void bin_check(BinReader *r, size_t need, const char *what) {
    if ((size_t)(r->end - r->p) < need) {
        wastro_die("binary: short read at %s", what);
    }
}
static uint8_t bin_u8(BinReader *r) {
    bin_check(r, 1, "u8");
    return *r->p++;
}
static uint32_t bin_u32(BinReader *r) {
    bin_check(r, 4, "u32");
    uint32_t v; memcpy(&v, r->p, 4); r->p += 4;
    return v;
}
static uint64_t bin_u64(BinReader *r) {
    bin_check(r, 8, "u64");
    uint64_t v; memcpy(&v, r->p, 8); r->p += 8;
    return v;
}
static uint64_t bin_leb_u(BinReader *r, int max_bits) {
    uint64_t v = 0; int shift = 0;
    while (1) {
        bin_check(r, 1, "leb_u");
        uint8_t b = *r->p++;
        v |= ((uint64_t)(b & 0x7F)) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
        if (shift >= max_bits + 7) {
            wastro_die("binary: LEB128 overflow");
        }
    }
    return v;
}
static int64_t bin_leb_s(BinReader *r, int max_bits) {
    int64_t v = 0; int shift = 0;
    uint8_t b;
    do {
        bin_check(r, 1, "leb_s");
        b = *r->p++;
        v |= ((int64_t)(b & 0x7F)) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) v |= -(int64_t)1 << shift;   // sign-extend
    (void)max_bits;
    return v;
}
static uint32_t bin_leb_u32(BinReader *r) { return (uint32_t)bin_leb_u(r, 32); }
static uint64_t bin_leb_u64(BinReader *r) { return bin_leb_u(r, 64); }
static int32_t  bin_leb_s32(BinReader *r) { return (int32_t)bin_leb_s(r, 32); }
static int64_t  bin_leb_s64(BinReader *r) { return bin_leb_s(r, 64); }

static wtype_t bin_valtype(uint8_t b) {
    switch (b) {
    case 0x7F: return WT_I32;
    case 0x7E: return WT_I64;
    case 0x7D: return WT_F32;
    case 0x7C: return WT_F64;
    }
    wastro_die("binary: unknown valtype 0x%02x", b);
}

// Bin-side function-body parser — opcode stream → AST.  Mirrors
// parse_bare_instr but reads from BinReader.  Calls itself
// recursively for block/loop/if bodies.
static TypedExpr parse_bin_code_seq(BinReader *r, LocalEnv *env, LabelEnv *labels, int allow_else, int *got_else);

static wtype_t bin_blocktype(BinReader *r) {
    bin_check(r, 1, "blocktype");
    uint8_t b = *r->p;
    if (b == 0x40) { r->p++; return WT_VOID; }
    if (b == 0x7F || b == 0x7E || b == 0x7D || b == 0x7C) {
        r->p++;
        return bin_valtype(b);
    }
    // s33 typeidx (multi-value) — read & discard.  Body type is
    // taken from the block's tail value at runtime.
    (void)bin_leb_s64(r);
    return WT_VOID;
}

static void parse_memarg(BinReader *r, uint32_t *out_offset) {
    (void)bin_leb_u32(r);   // align — informational
    *out_offset = bin_leb_u32(r);
}

#define BBIN(OPND_T, RES_T, ALLOC) do { \
        TypedExpr rr = op_pop(&S, OPND_T, "bin op right"); \
        TypedExpr ll = op_pop(&S, OPND_T, "bin op left"); \
        op_push(&S, ALLOC(ll.node, rr.node), RES_T); \
    } while (0)
#define BUN(OPND_T, RES_T, ALLOC) do { \
        TypedExpr ee = op_pop(&S, OPND_T, "un op"); \
        op_push(&S, ALLOC(ee.node), RES_T); \
    } while (0)
#define BLOAD(RES_T, ALLOC) do { \
        uint32_t off; parse_memarg(r, &off); \
        TypedExpr addr = op_pop(&S, WT_I32, "load addr"); \
        op_push(&S, ALLOC(off, addr.node), RES_T); \
    } while (0)
#define BSTORE(VAL_T, ALLOC) do { \
        uint32_t off; parse_memarg(r, &off); \
        TypedExpr value = op_pop(&S, VAL_T, "store value"); \
        TypedExpr addr  = op_pop(&S, WT_I32, "store addr"); \
        stmts_append(&L, ALLOC(off, addr.node, value.node)); \
    } while (0)

static TypedExpr
parse_bin_code_seq(BinReader *r, LocalEnv *env, LabelEnv *labels, int allow_else, int *got_else)
{
    OpStack S = {0};
    StmtList L = {0};
    if (got_else) *got_else = 0;
    while (1) {
        bin_check(r, 1, "code");
        uint8_t op = *r->p++;
        if (op == 0x0B) break;   // end
        if (op == 0x05) {
            if (!allow_else) wastro_die("binary: unexpected 0x05 (else)");
            if (got_else) *got_else = 1;
            break;
        }
        switch (op) {
        case 0x00: stmts_append(&L, ALLOC_node_unreachable()); break;
        case 0x01: stmts_append(&L, ALLOC_node_nop()); break;

        case 0x02: case 0x03: {  // block / loop
            int is_loop = (op == 0x03);
            wtype_t bt = bin_blocktype(r);
            if (labels->cnt >= 32) parse_error("too many nested labels");
            labels->names[labels->cnt] = NULL;
            labels->result_types[labels->cnt] = bt;
            labels->is_loop[labels->cnt] = is_loop;
            labels->cnt++;
            TypedExpr body = parse_bin_code_seq(r, env, labels, 0, NULL);
            labels->cnt--;
            NODE *node = is_loop ? ALLOC_node_loop(body.node) : ALLOC_node_block(body.node);
            if (bt != WT_VOID) op_push(&S, node, bt); else stmts_append(&L, node);
        } break;

        case 0x04: {  // if
            wtype_t bt = bin_blocktype(r);
            TypedExpr cond = op_pop(&S, WT_I32, "if cond");
            if (labels->cnt >= 32) parse_error("too many nested labels");
            labels->names[labels->cnt] = NULL;
            labels->result_types[labels->cnt] = bt;
            labels->is_loop[labels->cnt] = 0;
            labels->cnt++;
            int saw_else = 0;
            TypedExpr th = parse_bin_code_seq(r, env, labels, 1, &saw_else);
            TypedExpr el = {ALLOC_node_nop(), WT_VOID};
            if (saw_else) el = parse_bin_code_seq(r, env, labels, 0, NULL);
            labels->cnt--;
            NODE *node = ALLOC_node_if(cond.node, th.node, el.node);
            if (bt != WT_VOID) op_push(&S, node, bt); else stmts_append(&L, node);
        } break;

        case 0x0C: {  // br l
            uint32_t depth = bin_leb_u32(r);
            wtype_t want = labels->result_types[labels->cnt - 1 - depth];
            if (!labels->is_loop[labels->cnt - 1 - depth] && want != WT_VOID && S.cnt > 0) {
                TypedExpr v = op_pop(&S, want, "br value");
                stmts_append(&L, ALLOC_node_br_v(depth, v.node));
            } else {
                stmts_append(&L, ALLOC_node_br(depth));
            }
        } break;
        case 0x0D: {  // br_if l
            uint32_t depth = bin_leb_u32(r);
            TypedExpr cond = op_pop(&S, WT_I32, "br_if cond");
            wtype_t want = labels->result_types[labels->cnt - 1 - depth];
            if (!labels->is_loop[labels->cnt - 1 - depth] && want != WT_VOID && S.cnt > 0 && S.items[S.cnt - 1].type == want) {
                TypedExpr v = op_pop(&S, want, "br_if value");
                stmts_append(&L, ALLOC_node_br_if_v(depth, cond.node, v.node));
            } else {
                stmts_append(&L, ALLOC_node_br_if(depth, cond.node));
            }
        } break;
        case 0x0E: {  // br_table
            uint32_t cnt = bin_leb_u32(r);
            uint32_t depths[256];
            if (cnt > 256) parse_error("br_table too large");
            for (uint32_t i = 0; i < cnt; i++) depths[i] = bin_leb_u32(r);
            uint32_t default_depth = bin_leb_u32(r);
            if (WASTRO_BR_TABLE_CNT + cnt > WASTRO_BR_TABLE_CAP) {
                WASTRO_BR_TABLE_CAP = WASTRO_BR_TABLE_CAP ? WASTRO_BR_TABLE_CAP * 2 : 64;
                while (WASTRO_BR_TABLE_CAP < WASTRO_BR_TABLE_CNT + cnt) WASTRO_BR_TABLE_CAP *= 2;
                WASTRO_BR_TABLE = realloc(WASTRO_BR_TABLE, sizeof(uint32_t) * WASTRO_BR_TABLE_CAP);
            }
            uint32_t target_index = WASTRO_BR_TABLE_CNT;
            for (uint32_t i = 0; i < cnt; i++) WASTRO_BR_TABLE[target_index + i] = depths[i];
            WASTRO_BR_TABLE_CNT += cnt;
            TypedExpr idx = op_pop(&S, WT_I32, "br_table idx");
            wtype_t want = labels->result_types[labels->cnt - 1 - default_depth];
            if (!labels->is_loop[labels->cnt - 1 - default_depth] && want != WT_VOID && S.cnt > 0 && S.items[S.cnt - 1].type == want) {
                TypedExpr v = op_pop(&S, want, "br_table value");
                stmts_append(&L, ALLOC_node_br_table_v(target_index, cnt, default_depth, idx.node, v.node));
            } else {
                stmts_append(&L, ALLOC_node_br_table(target_index, cnt, default_depth, idx.node));
            }
        } break;
        case 0x0F: {  // return
            if (S.cnt > 0) {
                TypedExpr v = S.items[--S.cnt];
                stmts_append(&L, ALLOC_node_return_v(v.node));
            } else stmts_append(&L, ALLOC_node_return());
        } break;
        case 0x10: {  // call
            uint32_t fi = bin_leb_u32(r);
            struct wastro_function *callee = &WASTRO_FUNCS[fi];
            if (callee->param_cnt > WASTRO_MAX_PARAMS) parse_error("binary: call arity > 1024");
            NODE **args = NULL;
            uint32_t args_capa = 0;
            if (callee->param_cnt) node_args_grow(&args, &args_capa, callee->param_cnt);
            for (int i = (int)callee->param_cnt - 1; i >= 0; i--) {
                TypedExpr a = op_pop(&S, callee->param_types[i], "call arg");
                args[i] = a.node;
            }
            NODE *cn;
            if (callee->is_import) {
                switch (callee->param_cnt) {
                case 0: cn = ALLOC_node_host_call_0(fi); break;
                case 1: cn = ALLOC_node_host_call_1(fi, args[0]); break;
                case 2: cn = ALLOC_node_host_call_2(fi, args[0], args[1]); break;
                case 3: cn = ALLOC_node_host_call_3(fi, args[0], args[1], args[2]); break;
                default: {
                    uint32_t ai = wastro_register_call_args(args, callee->param_cnt);
                    cn = ALLOC_node_host_call_var(fi, ai, callee->param_cnt);
                    break;
                }
                }
            } else {
                uint32_t lc = callee->local_cnt;
                NODE *body = callee->body;  // may be NULL if forward ref
                switch (callee->param_cnt) {
                case 0: cn = ALLOC_node_call_0(fi, lc, body); break;
                case 1: cn = ALLOC_node_call_1(fi, lc, args[0], body); break;
                case 2: cn = ALLOC_node_call_2(fi, lc, args[0], args[1], body); break;
                case 3: cn = ALLOC_node_call_3(fi, lc, args[0], args[1], args[2], body); break;
                case 4: cn = ALLOC_node_call_4(fi, lc, args[0], args[1], args[2], args[3], body); break;
                default: {
                    uint32_t ai = wastro_register_call_args(args, callee->param_cnt);
                    cn = ALLOC_node_call_var(fi, lc, ai, callee->param_cnt, body);
                    break;
                }
                }
                register_call_body_fixup(cn, fi,
                                         callee->param_cnt <= 4 ? (uint8_t)callee->param_cnt
                                                                : PENDING_ARITY_VAR);
            }
            free(args);
            if (callee->result_type == WT_VOID) stmts_append(&L, cn);
            else op_push(&S, cn, callee->result_type);
        } break;
        case 0x11: {  // call_indirect
            uint32_t ti = bin_leb_u32(r);
            uint8_t table = bin_u8(r);
            (void)table;
            struct wastro_type_sig *sig = &WASTRO_TYPES[ti];
            if (sig->param_cnt > WASTRO_MAX_PARAMS) parse_error("binary: call_indirect arity > 1024");
            TypedExpr idx = op_pop(&S, WT_I32, "call_indirect idx");
            NODE **args = NULL;
            uint32_t args_capa = 0;
            if (sig->param_cnt) node_args_grow(&args, &args_capa, sig->param_cnt);
            for (int i = (int)sig->param_cnt - 1; i >= 0; i--) {
                TypedExpr a = op_pop(&S, sig->param_types[i], "ci arg");
                args[i] = a.node;
            }
            NODE *cn;
            switch (sig->param_cnt) {
            case 0: cn = ALLOC_node_call_indirect_0(ti, idx.node); break;
            case 1: cn = ALLOC_node_call_indirect_1(ti, idx.node, args[0]); break;
            case 2: cn = ALLOC_node_call_indirect_2(ti, idx.node, args[0], args[1]); break;
            case 3: cn = ALLOC_node_call_indirect_3(ti, idx.node, args[0], args[1], args[2]); break;
            case 4: cn = ALLOC_node_call_indirect_4(ti, idx.node, args[0], args[1], args[2], args[3]); break;
            default: {
                uint32_t ai = wastro_register_call_args(args, sig->param_cnt);
                cn = ALLOC_node_call_indirect_var(ti, ai, sig->param_cnt, idx.node);
                break;
            }
            }
            free(args);
            if (sig->result_type == WT_VOID) stmts_append(&L, cn);
            else op_push(&S, cn, sig->result_type);
        } break;

        case 0x1A: {  // drop
            TypedExpr e = op_pop(&S, WT_VOID, "drop");
            stmts_append(&L, ALLOC_node_drop(e.node));
        } break;
        case 0x1B: {  // select
            TypedExpr cond = op_pop(&S, WT_I32, "select cond");
            TypedExpr v2 = op_pop(&S, WT_VOID, "select v2");
            TypedExpr v1 = op_pop(&S, WT_VOID, "select v1");
            if (v1.type != v2.type) parse_error("select: type mismatch");
            op_push(&S, ALLOC_node_select(v1.node, v2.node, cond.node), v1.type);
        } break;

        case 0x20: {  // local.get
            uint32_t li = bin_leb_u32(r);
            op_push(&S, alloc_local_get(env->types[li], li), env->types[li]);
        } break;
        case 0x21: {  // local.set
            uint32_t li = bin_leb_u32(r);
            TypedExpr e = op_pop(&S, env->types[li], "local.set");
            stmts_append(&L, alloc_local_set(env->types[li], li, e.node));
        } break;
        case 0x22: {  // local.tee
            uint32_t li = bin_leb_u32(r);
            TypedExpr e = op_pop(&S, env->types[li], "local.tee");
            op_push(&S, alloc_local_tee(env->types[li], li, e.node), env->types[li]);
        } break;
        case 0x23: {  // global.get
            uint32_t gi = bin_leb_u32(r);
            op_push(&S, ALLOC_node_global_get(gi), WASTRO_GLOBAL_TYPES[gi]);
        } break;
        case 0x24: {  // global.set
            uint32_t gi = bin_leb_u32(r);
            TypedExpr e = op_pop(&S, WASTRO_GLOBAL_TYPES[gi], "global.set");
            stmts_append(&L, ALLOC_node_global_set(gi, e.node));
        } break;

        case 0x28: BLOAD(WT_I32, ALLOC_node_i32_load); break;
        case 0x29: BLOAD(WT_I64, ALLOC_node_i64_load); break;
        case 0x2A: BLOAD(WT_F32, ALLOC_node_f32_load); break;
        case 0x2B: BLOAD(WT_F64, ALLOC_node_f64_load); break;
        case 0x2C: BLOAD(WT_I32, ALLOC_node_i32_load8_s); break;
        case 0x2D: BLOAD(WT_I32, ALLOC_node_i32_load8_u); break;
        case 0x2E: BLOAD(WT_I32, ALLOC_node_i32_load16_s); break;
        case 0x2F: BLOAD(WT_I32, ALLOC_node_i32_load16_u); break;
        case 0x30: BLOAD(WT_I64, ALLOC_node_i64_load8_s); break;
        case 0x31: BLOAD(WT_I64, ALLOC_node_i64_load8_u); break;
        case 0x32: BLOAD(WT_I64, ALLOC_node_i64_load16_s); break;
        case 0x33: BLOAD(WT_I64, ALLOC_node_i64_load16_u); break;
        case 0x34: BLOAD(WT_I64, ALLOC_node_i64_load32_s); break;
        case 0x35: BLOAD(WT_I64, ALLOC_node_i64_load32_u); break;
        case 0x36: BSTORE(WT_I32, ALLOC_node_i32_store); break;
        case 0x37: BSTORE(WT_I64, ALLOC_node_i64_store); break;
        case 0x38: BSTORE(WT_F32, ALLOC_node_f32_store); break;
        case 0x39: BSTORE(WT_F64, ALLOC_node_f64_store); break;
        case 0x3A: BSTORE(WT_I32, ALLOC_node_i32_store8); break;
        case 0x3B: BSTORE(WT_I32, ALLOC_node_i32_store16); break;
        case 0x3C: BSTORE(WT_I64, ALLOC_node_i64_store8); break;
        case 0x3D: BSTORE(WT_I64, ALLOC_node_i64_store16); break;
        case 0x3E: BSTORE(WT_I64, ALLOC_node_i64_store32); break;
        case 0x3F: { (void)bin_u8(r); op_push(&S, ALLOC_node_memory_size(), WT_I32); } break;
        case 0x40: { (void)bin_u8(r); TypedExpr d = op_pop(&S, WT_I32, "memory.grow"); op_push(&S, ALLOC_node_memory_grow(d.node), WT_I32); } break;

        case 0x41: { int32_t v = bin_leb_s32(r); op_push(&S, ALLOC_node_i32_const(v), WT_I32); } break;
        case 0x42: { int64_t v = bin_leb_s64(r); op_push(&S, ALLOC_node_i64_const((uint64_t)v), WT_I64); } break;
        case 0x43: { uint32_t b = bin_u32(r); op_push(&S, ALLOC_node_f32_const(b), WT_F32); } break;
        case 0x44: { uint64_t b = bin_u64(r); double dv; memcpy(&dv,&b,8); op_push(&S, ALLOC_node_f64_const(dv), WT_F64); } break;

        case 0x45: BUN (WT_I32, WT_I32, ALLOC_node_i32_eqz); break;
        case 0x46: BBIN(WT_I32, WT_I32, ALLOC_node_i32_eq); break;
        case 0x47: BBIN(WT_I32, WT_I32, ALLOC_node_i32_ne); break;
        case 0x48: BBIN(WT_I32, WT_I32, ALLOC_node_i32_lt_s); break;
        case 0x49: BBIN(WT_I32, WT_I32, ALLOC_node_i32_lt_u); break;
        case 0x4A: BBIN(WT_I32, WT_I32, ALLOC_node_i32_gt_s); break;
        case 0x4B: BBIN(WT_I32, WT_I32, ALLOC_node_i32_gt_u); break;
        case 0x4C: BBIN(WT_I32, WT_I32, ALLOC_node_i32_le_s); break;
        case 0x4D: BBIN(WT_I32, WT_I32, ALLOC_node_i32_le_u); break;
        case 0x4E: BBIN(WT_I32, WT_I32, ALLOC_node_i32_ge_s); break;
        case 0x4F: BBIN(WT_I32, WT_I32, ALLOC_node_i32_ge_u); break;
        case 0x50: BUN (WT_I64, WT_I32, ALLOC_node_i64_eqz); break;
        case 0x51: BBIN(WT_I64, WT_I32, ALLOC_node_i64_eq); break;
        case 0x52: BBIN(WT_I64, WT_I32, ALLOC_node_i64_ne); break;
        case 0x53: BBIN(WT_I64, WT_I32, ALLOC_node_i64_lt_s); break;
        case 0x54: BBIN(WT_I64, WT_I32, ALLOC_node_i64_lt_u); break;
        case 0x55: BBIN(WT_I64, WT_I32, ALLOC_node_i64_gt_s); break;
        case 0x56: BBIN(WT_I64, WT_I32, ALLOC_node_i64_gt_u); break;
        case 0x57: BBIN(WT_I64, WT_I32, ALLOC_node_i64_le_s); break;
        case 0x58: BBIN(WT_I64, WT_I32, ALLOC_node_i64_le_u); break;
        case 0x59: BBIN(WT_I64, WT_I32, ALLOC_node_i64_ge_s); break;
        case 0x5A: BBIN(WT_I64, WT_I32, ALLOC_node_i64_ge_u); break;
        case 0x5B: BBIN(WT_F32, WT_I32, ALLOC_node_f32_eq); break;
        case 0x5C: BBIN(WT_F32, WT_I32, ALLOC_node_f32_ne); break;
        case 0x5D: BBIN(WT_F32, WT_I32, ALLOC_node_f32_lt); break;
        case 0x5E: BBIN(WT_F32, WT_I32, ALLOC_node_f32_gt); break;
        case 0x5F: BBIN(WT_F32, WT_I32, ALLOC_node_f32_le); break;
        case 0x60: BBIN(WT_F32, WT_I32, ALLOC_node_f32_ge); break;
        case 0x61: BBIN(WT_F64, WT_I32, ALLOC_node_f64_eq); break;
        case 0x62: BBIN(WT_F64, WT_I32, ALLOC_node_f64_ne); break;
        case 0x63: BBIN(WT_F64, WT_I32, ALLOC_node_f64_lt); break;
        case 0x64: BBIN(WT_F64, WT_I32, ALLOC_node_f64_gt); break;
        case 0x65: BBIN(WT_F64, WT_I32, ALLOC_node_f64_le); break;
        case 0x66: BBIN(WT_F64, WT_I32, ALLOC_node_f64_ge); break;

        case 0x67: BUN (WT_I32, WT_I32, ALLOC_node_i32_clz); break;
        case 0x68: BUN (WT_I32, WT_I32, ALLOC_node_i32_ctz); break;
        case 0x69: BUN (WT_I32, WT_I32, ALLOC_node_i32_popcnt); break;
        case 0x6A: BBIN(WT_I32, WT_I32, ALLOC_node_i32_add); break;
        case 0x6B: BBIN(WT_I32, WT_I32, ALLOC_node_i32_sub); break;
        case 0x6C: BBIN(WT_I32, WT_I32, ALLOC_node_i32_mul); break;
        case 0x6D: BBIN(WT_I32, WT_I32, ALLOC_node_i32_div_s); break;
        case 0x6E: BBIN(WT_I32, WT_I32, ALLOC_node_i32_div_u); break;
        case 0x6F: BBIN(WT_I32, WT_I32, ALLOC_node_i32_rem_s); break;
        case 0x70: BBIN(WT_I32, WT_I32, ALLOC_node_i32_rem_u); break;
        case 0x71: BBIN(WT_I32, WT_I32, ALLOC_node_i32_and); break;
        case 0x72: BBIN(WT_I32, WT_I32, ALLOC_node_i32_or); break;
        case 0x73: BBIN(WT_I32, WT_I32, ALLOC_node_i32_xor); break;
        case 0x74: BBIN(WT_I32, WT_I32, ALLOC_node_i32_shl); break;
        case 0x75: BBIN(WT_I32, WT_I32, ALLOC_node_i32_shr_s); break;
        case 0x76: BBIN(WT_I32, WT_I32, ALLOC_node_i32_shr_u); break;
        case 0x77: BBIN(WT_I32, WT_I32, ALLOC_node_i32_rotl); break;
        case 0x78: BBIN(WT_I32, WT_I32, ALLOC_node_i32_rotr); break;
        case 0x79: BUN (WT_I64, WT_I64, ALLOC_node_i64_clz); break;
        case 0x7A: BUN (WT_I64, WT_I64, ALLOC_node_i64_ctz); break;
        case 0x7B: BUN (WT_I64, WT_I64, ALLOC_node_i64_popcnt); break;
        case 0x7C: BBIN(WT_I64, WT_I64, ALLOC_node_i64_add); break;
        case 0x7D: BBIN(WT_I64, WT_I64, ALLOC_node_i64_sub); break;
        case 0x7E: BBIN(WT_I64, WT_I64, ALLOC_node_i64_mul); break;
        case 0x7F: BBIN(WT_I64, WT_I64, ALLOC_node_i64_div_s); break;
        case 0x80: BBIN(WT_I64, WT_I64, ALLOC_node_i64_div_u); break;
        case 0x81: BBIN(WT_I64, WT_I64, ALLOC_node_i64_rem_s); break;
        case 0x82: BBIN(WT_I64, WT_I64, ALLOC_node_i64_rem_u); break;
        case 0x83: BBIN(WT_I64, WT_I64, ALLOC_node_i64_and); break;
        case 0x84: BBIN(WT_I64, WT_I64, ALLOC_node_i64_or); break;
        case 0x85: BBIN(WT_I64, WT_I64, ALLOC_node_i64_xor); break;
        case 0x86: BBIN(WT_I64, WT_I64, ALLOC_node_i64_shl); break;
        case 0x87: BBIN(WT_I64, WT_I64, ALLOC_node_i64_shr_s); break;
        case 0x88: BBIN(WT_I64, WT_I64, ALLOC_node_i64_shr_u); break;
        case 0x89: BBIN(WT_I64, WT_I64, ALLOC_node_i64_rotl); break;
        case 0x8A: BBIN(WT_I64, WT_I64, ALLOC_node_i64_rotr); break;
        case 0x8B: BUN (WT_F32, WT_F32, ALLOC_node_f32_abs); break;
        case 0x8C: BUN (WT_F32, WT_F32, ALLOC_node_f32_neg); break;
        case 0x8D: BUN (WT_F32, WT_F32, ALLOC_node_f32_ceil); break;
        case 0x8E: BUN (WT_F32, WT_F32, ALLOC_node_f32_floor); break;
        case 0x8F: BUN (WT_F32, WT_F32, ALLOC_node_f32_trunc); break;
        case 0x90: BUN (WT_F32, WT_F32, ALLOC_node_f32_nearest); break;
        case 0x91: BUN (WT_F32, WT_F32, ALLOC_node_f32_sqrt); break;
        case 0x92: BBIN(WT_F32, WT_F32, ALLOC_node_f32_add); break;
        case 0x93: BBIN(WT_F32, WT_F32, ALLOC_node_f32_sub); break;
        case 0x94: BBIN(WT_F32, WT_F32, ALLOC_node_f32_mul); break;
        case 0x95: BBIN(WT_F32, WT_F32, ALLOC_node_f32_div); break;
        case 0x96: BBIN(WT_F32, WT_F32, ALLOC_node_f32_min); break;
        case 0x97: BBIN(WT_F32, WT_F32, ALLOC_node_f32_max); break;
        case 0x98: BBIN(WT_F32, WT_F32, ALLOC_node_f32_copysign); break;
        case 0x99: BUN (WT_F64, WT_F64, ALLOC_node_f64_abs); break;
        case 0x9A: BUN (WT_F64, WT_F64, ALLOC_node_f64_neg); break;
        case 0x9B: BUN (WT_F64, WT_F64, ALLOC_node_f64_ceil); break;
        case 0x9C: BUN (WT_F64, WT_F64, ALLOC_node_f64_floor); break;
        case 0x9D: BUN (WT_F64, WT_F64, ALLOC_node_f64_trunc); break;
        case 0x9E: BUN (WT_F64, WT_F64, ALLOC_node_f64_nearest); break;
        case 0x9F: BUN (WT_F64, WT_F64, ALLOC_node_f64_sqrt); break;
        case 0xA0: BBIN(WT_F64, WT_F64, ALLOC_node_f64_add); break;
        case 0xA1: BBIN(WT_F64, WT_F64, ALLOC_node_f64_sub); break;
        case 0xA2: BBIN(WT_F64, WT_F64, ALLOC_node_f64_mul); break;
        case 0xA3: BBIN(WT_F64, WT_F64, ALLOC_node_f64_div); break;
        case 0xA4: BBIN(WT_F64, WT_F64, ALLOC_node_f64_min); break;
        case 0xA5: BBIN(WT_F64, WT_F64, ALLOC_node_f64_max); break;
        case 0xA6: BBIN(WT_F64, WT_F64, ALLOC_node_f64_copysign); break;

        case 0xA7: BUN(WT_I64, WT_I32, ALLOC_node_i32_wrap_i64); break;
        case 0xA8: BUN(WT_F32, WT_I32, ALLOC_node_i32_trunc_f32_s); break;
        case 0xA9: BUN(WT_F32, WT_I32, ALLOC_node_i32_trunc_f32_u); break;
        case 0xAA: BUN(WT_F64, WT_I32, ALLOC_node_i32_trunc_f64_s); break;
        case 0xAB: BUN(WT_F64, WT_I32, ALLOC_node_i32_trunc_f64_u); break;
        case 0xAC: BUN(WT_I32, WT_I64, ALLOC_node_i64_extend_i32_s); break;
        case 0xAD: BUN(WT_I32, WT_I64, ALLOC_node_i64_extend_i32_u); break;
        case 0xAE: BUN(WT_F32, WT_I64, ALLOC_node_i64_trunc_f32_s); break;
        case 0xAF: BUN(WT_F32, WT_I64, ALLOC_node_i64_trunc_f32_u); break;
        case 0xB0: BUN(WT_F64, WT_I64, ALLOC_node_i64_trunc_f64_s); break;
        case 0xB1: BUN(WT_F64, WT_I64, ALLOC_node_i64_trunc_f64_u); break;
        case 0xB2: BUN(WT_I32, WT_F32, ALLOC_node_f32_convert_i32_s); break;
        case 0xB3: BUN(WT_I32, WT_F32, ALLOC_node_f32_convert_i32_u); break;
        case 0xB4: BUN(WT_I64, WT_F32, ALLOC_node_f32_convert_i64_s); break;
        case 0xB5: BUN(WT_I64, WT_F32, ALLOC_node_f32_convert_i64_u); break;
        case 0xB6: BUN(WT_F64, WT_F32, ALLOC_node_f32_demote_f64); break;
        case 0xB7: BUN(WT_I32, WT_F64, ALLOC_node_f64_convert_i32_s); break;
        case 0xB8: BUN(WT_I32, WT_F64, ALLOC_node_f64_convert_i32_u); break;
        case 0xB9: BUN(WT_I64, WT_F64, ALLOC_node_f64_convert_i64_s); break;
        case 0xBA: BUN(WT_I64, WT_F64, ALLOC_node_f64_convert_i64_u); break;
        case 0xBB: BUN(WT_F32, WT_F64, ALLOC_node_f64_promote_f32); break;
        case 0xBC: BUN(WT_F32, WT_I32, ALLOC_node_i32_reinterpret_f32); break;
        case 0xBD: BUN(WT_F64, WT_I64, ALLOC_node_i64_reinterpret_f64); break;
        case 0xBE: BUN(WT_I32, WT_F32, ALLOC_node_f32_reinterpret_i32); break;
        case 0xBF: BUN(WT_I64, WT_F64, ALLOC_node_f64_reinterpret_i64); break;
        case 0xC0: BUN(WT_I32, WT_I32, ALLOC_node_i32_extend8_s); break;
        case 0xC1: BUN(WT_I32, WT_I32, ALLOC_node_i32_extend16_s); break;
        case 0xC2: BUN(WT_I64, WT_I64, ALLOC_node_i64_extend8_s); break;
        case 0xC3: BUN(WT_I64, WT_I64, ALLOC_node_i64_extend16_s); break;
        case 0xC4: BUN(WT_I64, WT_I64, ALLOC_node_i64_extend32_s); break;

        case 0xFC: {
            uint32_t sub = bin_leb_u32(r);
            switch (sub) {
            case 0: BUN(WT_F32, WT_I32, ALLOC_node_i32_trunc_sat_f32_s); break;
            case 1: BUN(WT_F32, WT_I32, ALLOC_node_i32_trunc_sat_f32_u); break;
            case 2: BUN(WT_F64, WT_I32, ALLOC_node_i32_trunc_sat_f64_s); break;
            case 3: BUN(WT_F64, WT_I32, ALLOC_node_i32_trunc_sat_f64_u); break;
            case 4: BUN(WT_F32, WT_I64, ALLOC_node_i64_trunc_sat_f32_s); break;
            case 5: BUN(WT_F32, WT_I64, ALLOC_node_i64_trunc_sat_f32_u); break;
            case 6: BUN(WT_F64, WT_I64, ALLOC_node_i64_trunc_sat_f64_s); break;
            case 7: BUN(WT_F64, WT_I64, ALLOC_node_i64_trunc_sat_f64_u); break;
            default:
                wastro_die("binary: unsupported 0xFC subop %u", sub);
            }
        } break;

        default:
            wastro_die("binary: unknown opcode 0x%02x", op);
        }
    }
    NODE *final_val = NULL;
    wtype_t final_t = WT_VOID;
    if (S.cnt >= 1) {
        final_val = S.items[S.cnt - 1].node;
        final_t = S.items[S.cnt - 1].type;
        for (uint32_t i = 0; i + 1 < S.cnt; i++) stmts_append(&L, S.items[i].node);
    }
    NODE *body = build_body_node(&L, final_val);
    return (TypedExpr){body, final_t};
}

#undef BBIN
#undef BUN
#undef BLOAD
#undef BSTORE

// Decode a const expr (used in global init / data offset / elem offset).
// Only `*.const X end` and `global.get X end` forms are accepted.
static VALUE
parse_const_expr(BinReader *r, wtype_t *out_type)
{
    uint8_t op = bin_u8(r);
    VALUE v = 0;
    if (op == 0x41) {
        v = FROM_I32(bin_leb_s32(r));
        if (out_type) *out_type = WT_I32;
    } else if (op == 0x42) {
        v = FROM_I64(bin_leb_s64(r));
        if (out_type) *out_type = WT_I64;
    } else if (op == 0x43) {
        v = FROM_U32(bin_u32(r));
        if (out_type) *out_type = WT_F32;
    } else if (op == 0x44) {
        v = FROM_U64(bin_u64(r));
        if (out_type) *out_type = WT_F64;
    } else if (op == 0x23) {
        uint32_t gi = bin_leb_u32(r);
        v = WASTRO_GLOBALS ? WASTRO_GLOBALS[gi] : 0;
        if (out_type) *out_type = WASTRO_GLOBAL_TYPES[gi];
    } else {
        wastro_die("binary: unsupported const-expr opcode 0x%02x", op);
    }
    if (bin_u8(r) != 0x0B) {
        wastro_die("binary: const-expr missing end");
    }
    return v;
}

static void
load_module_binary(const uint8_t *buf, size_t sz)
{
    BinReader R = { buf, buf + sz };
    if (sz < 8 || memcmp(buf, "\0asm", 4) != 0) {
        wastro_die("binary: bad magic");
    }
    R.p += 4;
    uint32_t version = bin_u32(&R);
    if (version != 1) {
        wastro_die("binary: unsupported version %u", version);
    }

    // Track imported function count for the Function section indexing.
    uint32_t imported_funcs = 0;

    // First-pass: section dispatch.  Sections are unique except Custom.
    while (R.p < R.end) {
        uint8_t sid = bin_u8(&R);
        uint32_t ssize = bin_leb_u32(&R);
        const uint8_t *send = R.p + ssize;
        bin_check(&R, ssize, "section payload");
        BinReader S2 = { R.p, send };

        switch (sid) {
        case 0:   // Custom section — skip
            break;
        case 1: { // Type
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                if (bin_u8(&S2) != 0x60) parse_error("binary: bad type form");
                struct wastro_type_sig sig = {0};
                uint32_t pc = bin_leb_u32(&S2);
                if (pc > WASTRO_MAX_PARAMS) parse_error("binary: too many params (>1024)");
                sig.param_cnt = pc;
                sig.param_types = wtype_alloc(pc);
                for (uint32_t k = 0; k < pc; k++)
                    sig.param_types[k] = bin_valtype(bin_u8(&S2));
                uint32_t rc = bin_leb_u32(&S2);
                if (rc > 1) parse_error("binary: multi-result not supported");
                sig.result_type = rc ? bin_valtype(bin_u8(&S2)) : WT_VOID;
                if (WASTRO_TYPE_CNT >= WASTRO_MAX_TYPES) parse_error("binary: too many types");
                WASTRO_TYPES[WASTRO_TYPE_CNT++] = sig;
            }
        } break;
        case 2: { // Import
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t ml = bin_leb_u32(&S2);
                char mod[64]; if (ml >= 64) parse_error("binary: import mod too long");
                bin_check(&S2, ml, "import mod"); memcpy(mod, S2.p, ml); mod[ml] = 0; S2.p += ml;
                uint32_t fl = bin_leb_u32(&S2);
                char fld[64]; if (fl >= 64) parse_error("binary: import fld too long");
                bin_check(&S2, fl, "import fld"); memcpy(fld, S2.p, fl); fld[fl] = 0; S2.p += fl;
                uint8_t kind = bin_u8(&S2);
                if (kind == 0x00) {
                    uint32_t ti = bin_leb_u32(&S2);
                    int fi = WASTRO_FUNC_CNT;
                    if (fi >= WASTRO_MAX_FUNCS) parse_error("binary: too many funcs");
                    WASTRO_FUNCS[fi].is_import = 1;
                    WASTRO_FUNCS[fi].name = NULL;
                    WASTRO_FUNCS[fi].local_cnt = 0;
                    const struct host_entry *he = find_host(mod, fld);
                    struct wastro_type_sig *sig = &WASTRO_TYPES[ti];
                    if (he) {
                        WASTRO_FUNCS[fi].host_fn = he->fn;
                        func_set_params(&WASTRO_FUNCS[fi], he->param_cnt);
                        for (uint32_t k = 0; k < he->param_cnt; k++)
                            WASTRO_FUNCS[fi].param_types[k] = he->param_types[k];
                        WASTRO_FUNCS[fi].result_type = he->result_type;
                    } else {
                        WASTRO_FUNCS[fi].host_fn = host_unbound_trap;
                        func_set_params(&WASTRO_FUNCS[fi], sig->param_cnt);
                        for (uint32_t k = 0; k < sig->param_cnt; k++)
                            WASTRO_FUNCS[fi].param_types[k] = sig->param_types[k];
                        WASTRO_FUNCS[fi].result_type = sig->result_type;
                    }
                    WASTRO_FUNC_CNT++;
                    imported_funcs++;
                } else if (kind == 0x01) {
                    (void)bin_u8(&S2);   // reftype (funcref = 0x70)
                    uint8_t flags = bin_u8(&S2);
                    uint32_t init = bin_leb_u32(&S2);
                    uint32_t mx = (flags & 1) ? bin_leb_u32(&S2) : 0xFFFFFFFFu;
                    if (MOD_HAS_TABLE) parse_error("binary: multiple tables");
                    MOD_HAS_TABLE = 1;
                    WASTRO_TABLE_SIZE = init; WASTRO_TABLE_MAX = mx;
                    WASTRO_TABLE = malloc(sizeof(int32_t) * (init ? init : 1));
                    for (uint32_t k = 0; k < init; k++) WASTRO_TABLE[k] = -1;
                } else if (kind == 0x02) {
                    uint8_t flags = bin_u8(&S2);
                    MOD_MEM_INITIAL_PAGES = bin_leb_u32(&S2);
                    MOD_MEM_MAX_PAGES = (flags & 1) ? bin_leb_u32(&S2) : 0xFFFFFFFFu;
                    MOD_HAS_MEMORY = 1;
                } else if (kind == 0x03) {
                    uint8_t vt = bin_u8(&S2);
                    uint8_t mut = bin_u8(&S2);
                    if (WASTRO_GLOBAL_CNT >= WASTRO_MAX_GLOBALS) parse_error("binary: too many globals");
                    if (!WASTRO_GLOBALS) WASTRO_GLOBALS = calloc(WASTRO_MAX_GLOBALS, sizeof(VALUE));
                    WASTRO_GLOBALS[WASTRO_GLOBAL_CNT] = 0;
                    WASTRO_GLOBAL_TYPES[WASTRO_GLOBAL_CNT] = bin_valtype(vt);
                    WASTRO_GLOBAL_MUT[WASTRO_GLOBAL_CNT] = mut;
                    WASTRO_GLOBAL_NAMES[WASTRO_GLOBAL_CNT] = NULL;
                    WASTRO_GLOBAL_CNT++;
                } else parse_error("binary: bad import kind");
            }
        } break;
        case 3: { // Function — typeidx for each defined func
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t ti = bin_leb_u32(&S2);
                int fi = WASTRO_FUNC_CNT;
                if (fi >= WASTRO_MAX_FUNCS) parse_error("binary: too many funcs");
                struct wastro_type_sig *sig = &WASTRO_TYPES[ti];
                WASTRO_FUNCS[fi].name = NULL;
                WASTRO_FUNCS[fi].is_import = 0;
                func_set_params(&WASTRO_FUNCS[fi], sig->param_cnt);
                for (uint32_t k = 0; k < sig->param_cnt; k++)
                    WASTRO_FUNCS[fi].param_types[k] = sig->param_types[k];
                WASTRO_FUNCS[fi].result_type = sig->result_type;
                func_set_locals(&WASTRO_FUNCS[fi], sig->param_cnt);   // body sets total
                for (uint32_t k = 0; k < sig->param_cnt; k++)
                    WASTRO_FUNCS[fi].local_types[k] = sig->param_types[k];
                WASTRO_FUNC_CNT++;
            }
        } break;
        case 4: { // Table
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                (void)bin_u8(&S2);   // reftype
                uint8_t flags = bin_u8(&S2);
                uint32_t init = bin_leb_u32(&S2);
                uint32_t mx = (flags & 1) ? bin_leb_u32(&S2) : 0xFFFFFFFFu;
                if (MOD_HAS_TABLE) parse_error("binary: multiple tables");
                MOD_HAS_TABLE = 1;
                WASTRO_TABLE_SIZE = init; WASTRO_TABLE_MAX = mx;
                WASTRO_TABLE = malloc(sizeof(int32_t) * (init ? init : 1));
                for (uint32_t k = 0; k < init; k++) WASTRO_TABLE[k] = -1;
            }
        } break;
        case 5: { // Memory
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint8_t flags = bin_u8(&S2);
                MOD_MEM_INITIAL_PAGES = bin_leb_u32(&S2);
                MOD_MEM_MAX_PAGES = (flags & 1) ? bin_leb_u32(&S2) : 0xFFFFFFFFu;
                MOD_HAS_MEMORY = 1;
            }
        } break;
        case 6: { // Global
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint8_t vt = bin_u8(&S2);
                uint8_t mut = bin_u8(&S2);
                wtype_t wtv = bin_valtype(vt);
                wtype_t got_t;
                VALUE init_val = parse_const_expr(&S2, &got_t);
                if (WASTRO_GLOBAL_CNT >= WASTRO_MAX_GLOBALS) parse_error("binary: too many globals");
                if (!WASTRO_GLOBALS) WASTRO_GLOBALS = calloc(WASTRO_MAX_GLOBALS, sizeof(VALUE));
                WASTRO_GLOBALS[WASTRO_GLOBAL_CNT] = init_val;
                WASTRO_GLOBAL_TYPES[WASTRO_GLOBAL_CNT] = wtv;
                WASTRO_GLOBAL_MUT[WASTRO_GLOBAL_CNT] = mut;
                WASTRO_GLOBAL_NAMES[WASTRO_GLOBAL_CNT] = NULL;
                WASTRO_GLOBAL_CNT++;
            }
        } break;
        case 7: { // Export
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t nl = bin_leb_u32(&S2);
                bin_check(&S2, nl, "export name");
                char *name = malloc(nl + 1); memcpy(name, S2.p, nl); name[nl] = 0; S2.p += nl;
                uint8_t kind = bin_u8(&S2);
                uint32_t idx = bin_leb_u32(&S2);
                if (kind == 0) {
                    WASTRO_FUNCS[idx].exported = 1;
                    WASTRO_FUNCS[idx].export_name = name;
                } else {
                    free(name);   // mem/global/table exports — ignored
                }
            }
        } break;
        case 8: { // Start
            uint32_t fi = bin_leb_u32(&S2);
            MOD_HAS_START = 1;
            MOD_START_FUNC = (int)fi;
        } break;
        case 9: { // Element
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t flags = bin_leb_u32(&S2);
                if (flags != 0) {
                    wastro_die("binary: elem flag %u not supported", flags);
                }
                wtype_t off_t;
                VALUE off_v = parse_const_expr(&S2, &off_t);
                uint32_t off = AS_U32(off_v);
                uint32_t ec = bin_leb_u32(&S2);
                for (uint32_t k = 0; k < ec; k++) {
                    uint32_t fi = bin_leb_u32(&S2);
                    if (!MOD_HAS_TABLE) parse_error("binary: elem without table");
                    if (off + k >= WASTRO_TABLE_SIZE) parse_error("binary: elem overflows table");
                    WASTRO_TABLE[off + k] = (int32_t)fi;
                }
            }
        } break;
        case 10: { // Code
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t body_size = bin_leb_u32(&S2);
                const uint8_t *body_end = S2.p + body_size;
                int fi = (int)imported_funcs + (int)i;
                struct wastro_function *fn = &WASTRO_FUNCS[fi];
                LocalEnv env = {0};
                for (uint32_t k = 0; k < fn->param_cnt; k++) {
                    lenv_push(&env, NULL, fn->param_types[k]);
                }
                // locals: vec of (count, valtype)
                BinReader BR = { S2.p, body_end };
                uint32_t lg = bin_leb_u32(&BR);
                for (uint32_t g = 0; g < lg; g++) {
                    uint32_t cnt = bin_leb_u32(&BR);
                    wtype_t lt = bin_valtype(bin_u8(&BR));
                    for (uint32_t k = 0; k < cnt; k++) {
                        lenv_push(&env, NULL, lt);
                    }
                }
                func_set_locals(fn, env.cnt);
                for (uint32_t k = 0; k < env.cnt; k++) fn->local_types[k] = env.types[k];
                LabelEnv labels = {0};
                int save_idx = CUR_FUNC_IDX;
                CUR_FUNC_IDX = fi;
                TypedExpr body = parse_bin_code_seq(&BR, &env, &labels, 0, NULL);
                CUR_FUNC_IDX = save_idx;
                lenv_free(&env);
                if (BR.p != body_end) {
                    wastro_die("binary: code body length mismatch");
                }
                fn->body = body.node;
                S2.p = body_end;
            }
        } break;
        case 11: { // Data
            uint32_t n = bin_leb_u32(&S2);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t flags = bin_leb_u32(&S2);
                uint32_t off = 0;
                if (flags == 0) {
                    wtype_t ot;
                    VALUE off_v = parse_const_expr(&S2, &ot);
                    off = AS_U32(off_v);
                } else if (flags == 2) {
                    (void)bin_leb_u32(&S2);   // memidx (always 0)
                    wtype_t ot;
                    VALUE off_v = parse_const_expr(&S2, &ot);
                    off = AS_U32(off_v);
                } else {
                    wastro_die("binary: data flag %u not supported", flags);
                }
                uint32_t dl = bin_leb_u32(&S2);
                bin_check(&S2, dl, "data bytes");
                if (MOD_DATA_SEG_CNT >= WASTRO_MAX_DATA_SEGS) parse_error("binary: too many data");
                MOD_DATA_SEGS[MOD_DATA_SEG_CNT].offset = off;
                MOD_DATA_SEGS[MOD_DATA_SEG_CNT].length = dl;
                uint8_t *bytes = malloc(dl);
                memcpy(bytes, S2.p, dl);
                MOD_DATA_SEGS[MOD_DATA_SEG_CNT].bytes = bytes;
                MOD_DATA_SEG_CNT++;
                S2.p += dl;
            }
        } break;
        case 12: { // DataCount (post-1.0; ignore)
            (void)bin_leb_u32(&S2);
        } break;
        default:
            fprintf(stderr, "wastro: binary: unknown section id %u (skipping)\n", sid);
            break;
        }
        R.p = send;
    }
}

// Load a module from a memory buffer.  Detects binary vs text by
// magic.  Caller retains ownership of `buf` until done with the
// module — text-mode parsing keeps pointers into it.
NODE *
wastro_load_module_buf(const char *buf, size_t sz)
{
    if (sz >= 4 && (uint8_t)buf[0] == 0 && buf[1] == 'a' && buf[2] == 's' && buf[3] == 'm') {
        load_module_binary((const uint8_t *)buf, sz);
        wastro_fixup_call_bodies();
        return NULL;
    }
    MODULE_TEXT_START = buf;
    const char *func_offsets[WASTRO_MAX_FUNCS];
    int n = 0;
    scan_module(buf, sz, func_offsets, &n);

    // Resolve deferred (export "name" (func $f)) — the func may be
    // declared later in the source, so we resolve post-scan.
    for (uint32_t s = 0; s < PENDING_EXPORT_CNT; s++) {
        struct export_pending *ep = &PENDING_EXPORTS[s];
        int fi = resolve_func(&ep->ref);
        WASTRO_FUNCS[fi].exported = 1;
        WASTRO_FUNCS[fi].export_name = ep->name;
    }
    PENDING_EXPORT_CNT = 0;

    if (MOD_HAS_START) {
        MOD_START_FUNC = resolve_func(&MOD_START_TOK);
    }

    for (uint32_t s = 0; s < PENDING_ELEM_CNT; s++) {
        struct elem_pending *ep = &PENDING_ELEMS[s];
        if (!MOD_HAS_TABLE) { fprintf(stderr, "wastro: (elem ...) without (table ...)\n"); exit(1); }
        for (uint32_t i = 0; i < ep->cnt; i++) {
            int fi = resolve_func(&ep->refs[i]);
            if (ep->offset + i >= WASTRO_TABLE_SIZE) {
                fprintf(stderr, "wastro: (elem) overflows table\n"); exit(1);
            }
            WASTRO_TABLE[ep->offset + i] = fi;
        }
        free(ep->refs);
    }
    PENDING_ELEM_CNT = 0;

    for (int i = 0; i < n; i++) {
        if (!func_offsets[i]) continue;
        src_pos = func_offsets[i];
        src_end = buf + sz;
        next_token();
        parse_func_pass2(i);
    }
    wastro_fixup_call_bodies();
    return NULL;
}

NODE *
wastro_load_module(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { perror("fread"); exit(1); }
    buf[sz] = '\0';
    fclose(f);
    return wastro_load_module_buf(buf, (size_t)sz);
}

#if 0
// Old in-place body of wastro_load_module — superseded by
// wastro_load_module_buf which does the same work on memory.
NODE *
wastro_load_module_OLD(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { perror("fread"); exit(1); }
    buf[sz] = '\0';
    fclose(f);

    // Detect binary by magic: 0x00 'a' 's' 'm'.
    if (sz >= 4 && (uint8_t)buf[0] == 0 && buf[1] == 'a' && buf[2] == 's' && buf[3] == 'm') {
        load_module_binary((const uint8_t *)buf, (size_t)sz);
        return NULL;
    }

    MODULE_TEXT_START = buf;

    const char *func_offsets[WASTRO_MAX_FUNCS];
    int n = 0;
    scan_module(buf, (size_t)sz, func_offsets, &n);

    // Resolve deferred (export "name" (func $f)) — the func may be
    // declared later in the source, so we resolve post-scan.
    for (uint32_t s = 0; s < PENDING_EXPORT_CNT; s++) {
        struct export_pending *ep = &PENDING_EXPORTS[s];
        int fi = resolve_func(&ep->ref);
        WASTRO_FUNCS[fi].exported = 1;
        WASTRO_FUNCS[fi].export_name = ep->name;   // ownership transferred
    }
    PENDING_EXPORT_CNT = 0;

    if (MOD_HAS_START) {
        MOD_START_FUNC = resolve_func(&MOD_START_TOK);
    }

    // Resolve deferred elem segments — function names from elem can
    // refer to funcs defined later in the source, so we resolve here
    // (after all func names are registered).
    for (uint32_t s = 0; s < PENDING_ELEM_CNT; s++) {
        struct elem_pending *ep = &PENDING_ELEMS[s];
        if (!MOD_HAS_TABLE) {
            fprintf(stderr, "wastro: (elem ...) without (table ...)\n"); exit(1);
        }
        for (uint32_t i = 0; i < ep->cnt; i++) {
            int fi = resolve_func(&ep->refs[i]);
            if (ep->offset + i >= WASTRO_TABLE_SIZE) {
                fprintf(stderr,
                    "wastro: (elem) overflows table (size %u, writing index %u)\n",
                    WASTRO_TABLE_SIZE, ep->offset + i);
                exit(1);
            }
            WASTRO_TABLE[ep->offset + i] = fi;
        }
        free(ep->refs);
    }
    PENDING_ELEM_CNT = 0;

    // Pass 2: parse each func body in order.  Imports are skipped
    // (their func_offsets entry is NULL).
    for (int i = 0; i < n; i++) {
        if (!func_offsets[i]) continue;   // import slot
        src_pos = func_offsets[i];
        src_end = buf + sz;
        next_token();
        parse_func_pass2(i);
    }
    return NULL; // module-level AST not needed; functions are addressable via WASTRO_FUNCS.
}
#endif

