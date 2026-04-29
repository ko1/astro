// luastro standard library — minimum useful surface.
//
// We implement the subset that real benchmarks and tests rely on:
//   - basic globals: print, tostring, tonumber, type, ipairs, pairs,
//                     error, pcall, xpcall, assert, select, unpack
//                     (compat alias for table.unpack), rawget, rawset,
//                     rawequal, rawlen, setmetatable, getmetatable
//   - math.*  : pi, huge, abs, floor, ceil, sqrt, sin, cos, tan,
//               asin, acos, atan, exp, log, max, min, random, fmod,
//               modf, pow, tointeger, type
//   - string.*: len, sub, upper, lower, rep, reverse, byte, char,
//               format (a useful subset of %d/%i/%f/%g/%s/%x/%c/%%),
//               find/match (literal substring only)
//   - table.* : insert, remove, concat, unpack, pack
//   - io.*    : write (stdout), read (line-only)
//   - os.*    : clock, time, date (literal), getenv, exit

#include <math.h>
#include <time.h>
#include <inttypes.h>
#include <stdarg.h>
#include "context.h"
#include "node.h"

#define ARG(i) (((i) < argc) ? args[i] : LUAV_NIL)

static struct LuaString *
arg_string(CTX *c, LuaValue v, int idx)
{
    if (LV_IS_STR(v)) return LV_AS_STR(v);
    LuaValue s = lua_tostring(c, v);
    return LV_AS_STR(s);
}

static int64_t
arg_int(CTX *c, LuaValue v, int idx)
{
    int64_t i;
    if (lua_to_int(v, &i)) return i;
    lua_raisef(c, "bad argument #%d (integer expected, got %s)", idx, lua_type_name(v));
    return 0;
}

static double
arg_num(CTX *c, LuaValue v, int idx)
{
    double f;
    if (lua_to_float(v, &f)) return f;
    lua_raisef(c, "bad argument #%d (number expected, got %s)", idx, lua_type_name(v));
    return 0.0;
}

static struct LuaTable *
arg_table(CTX *c, LuaValue v, int idx)
{
    if (!LV_IS_TBL(v))
        lua_raisef(c, "bad argument #%d (table expected, got %s)", idx, lua_type_name(v));
    return LV_AS_TBL(v);
}

// =====================================================================
// Basic globals
// =====================================================================

static RESULT
b_print(CTX *c, LuaValue *args, uint32_t argc)
{
    for (uint32_t i = 0; i < argc; i++) {
        if (i) fputc('\t', stdout);
        // Honour __tostring on tables.
        if (LV_IS_TBL(args[i]) && LV_AS_TBL(args[i])->metatable) {
            LuaValue mm = lua_table_get_str(LV_AS_TBL(args[i])->metatable, lua_str_intern("__tostring"));
            if (LV_IS_CALL(mm)) {
                RESULT r = lua_call(c, mm, &args[i], 1);
                lua_print_value(stdout, r);
                continue;
            }
        }
        lua_print_value(stdout, args[i]);
    }
    fputc('\n', stdout);
    c->ret_info.result_cnt = 0;
    return RESULT_OK(LUAV_NIL);
}

static RESULT
b_tostring(CTX *c, LuaValue *args, uint32_t argc)
{
    return RESULT_OK(lua_tostring(c, ARG(0)));
}

static RESULT
b_tonumber(CTX *c, LuaValue *args, uint32_t argc)
{
    LuaValue v = ARG(0);
    if (LV_IS_NUM(v)) return RESULT_OK(v);
    if (LV_IS_STR(v)) {
        int64_t i;
        if (lua_to_int(v, &i)) return RESULT_OK(LUAV_INT(i));
        double f;
        if (lua_to_float(v, &f)) return RESULT_OK(LUAV_FLOAT(f));
    }
    return RESULT_OK(LUAV_NIL);
    (void)c;
}

static RESULT
b_type(CTX *c, LuaValue *args, uint32_t argc)
{
    (void)c;
    return RESULT_OK(LUAV_STR(lua_str_intern(lua_type_name(ARG(0)))));
}

// ipairs iterator: returns next i+1 and t[i+1], stopping at first nil.
static RESULT
b_ipairs_iter(CTX *c, LuaValue *args, uint32_t argc)
{
    (void)c;
    LuaValue t = ARG(0);
    int64_t i; lua_to_int(ARG(1), &i);
    i++;
    if (!LV_IS_TBL(t)) {
        c->ret_info.result_cnt = 0;
        return RESULT_OK(LUAV_NIL);
    }
    LuaValue v = lua_table_geti(LV_AS_TBL(t), i);
    if (LV_IS_NIL(v)) {
        c->ret_info.result_cnt = 0;
        return RESULT_OK(LUAV_NIL);
    }
    c->ret_info.results[0] = LUAV_INT(i);
    c->ret_info.results[1] = v;
    c->ret_info.result_cnt = 2;
    return RESULT_OK(LUAV_INT(i));
}

static RESULT
b_ipairs(CTX *c, LuaValue *args, uint32_t argc)
{
    LuaValue t = ARG(0);
    LuaValue iter = LUAV_CFUNC(lua_cfunc_new("ipairs_iter", b_ipairs_iter));
    c->ret_info.results[0] = iter;
    c->ret_info.results[1] = t;
    c->ret_info.results[2] = LUAV_INT(0);
    c->ret_info.result_cnt = 3;
    return RESULT_OK(iter);
}

static RESULT
b_pairs_iter(CTX *c, LuaValue *args, uint32_t argc)
{
    (void)c;
    LuaValue t = ARG(0);
    LuaValue k = ARG(1);
    if (!LV_IS_TBL(t)) {
        c->ret_info.result_cnt = 0;
        return RESULT_OK(LUAV_NIL);
    }
    LuaValue v = LUAV_NIL;
    if (!lua_table_next(LV_AS_TBL(t), &k, &v)) {
        c->ret_info.result_cnt = 0;
        return RESULT_OK(LUAV_NIL);
    }
    c->ret_info.results[0] = k;
    c->ret_info.results[1] = v;
    c->ret_info.result_cnt = 2;
    return RESULT_OK(k);
}

static RESULT
b_pairs(CTX *c, LuaValue *args, uint32_t argc)
{
    LuaValue t = ARG(0);
    LuaValue iter = LUAV_CFUNC(lua_cfunc_new("pairs_iter", b_pairs_iter));
    c->ret_info.results[0] = iter;
    c->ret_info.results[1] = t;
    c->ret_info.results[2] = LUAV_NIL;
    c->ret_info.result_cnt = 3;
    return RESULT_OK(iter);
}

static RESULT
b_error(CTX *c, LuaValue *args, uint32_t argc)
{
    LuaValue v = ARG(0);
    lua_raise(c, v);
    return RESULT_OK(LUAV_NIL);
}

static RESULT
b_pcall(CTX *c, LuaValue *args, uint32_t argc)
{
    if (argc == 0) lua_raisef(c, "bad argument #1 to 'pcall'");
    LuaValue fn = args[0];
    return lua_pcall(c, fn, args + 1, argc - 1);
}

static RESULT
b_xpcall(CTX *c, LuaValue *args, uint32_t argc)
{
    if (argc < 2) lua_raisef(c, "bad arguments to 'xpcall'");
    // We don't actually invoke the handler in v1 — pcall semantics are
    // sufficient for most code that uses xpcall as a wrapped pcall.
    LuaValue fn = args[0];
    return lua_pcall(c, fn, args + 2, argc - 2);
}

static RESULT
b_assert(CTX *c, LuaValue *args, uint32_t argc)
{
    if (argc == 0 || !LV_TRUTHY(args[0])) {
        LuaValue msg = argc > 1 ? args[1] : LUAV_STR(lua_str_intern("assertion failed!"));
        lua_raise(c, msg);
    }
    // Return all args.
    for (uint32_t i = 0; i < argc && i < LUASTRO_MAX_RETS; i++)
        c->ret_info.results[i] = args[i];
    c->ret_info.result_cnt = argc < LUASTRO_MAX_RETS ? argc : LUASTRO_MAX_RETS;
    return RESULT_OK(args[0]);
}

static RESULT
b_select(CTX *c, LuaValue *args, uint32_t argc)
{
    if (argc == 0) lua_raisef(c, "bad argument #1 to 'select'");
    LuaValue sel = args[0];
    if (LV_IS_STR(sel) && strcmp(lua_str_data(LV_AS_STR(sel)), "#") == 0) {
        return RESULT_OK(LUAV_INT((int64_t)argc - 1));
    }
    int64_t idx; if (!lua_to_int(sel, &idx)) lua_raisef(c, "bad argument #1 to 'select'");
    if (idx < 0) idx = (int64_t)argc - 1 + idx + 1;
    if (idx < 1) lua_raisef(c, "bad argument #1 to 'select' (index out of range)");
    int64_t n = idx;
    uint32_t base = (uint32_t)n;
    if (base >= argc) {
        c->ret_info.result_cnt = 0;
        return RESULT_OK(LUAV_NIL);
    }
    uint32_t cnt = argc - base;
    for (uint32_t i = 0; i < cnt && i < LUASTRO_MAX_RETS; i++)
        c->ret_info.results[i] = args[base + i];
    c->ret_info.result_cnt = cnt < LUASTRO_MAX_RETS ? cnt : LUASTRO_MAX_RETS;
    return RESULT_OK(args[base]);
}

static RESULT
b_setmetatable(CTX *c, LuaValue *args, uint32_t argc)
{
    LuaValue t = ARG(0); LuaValue mt = ARG(1);
    if (!LV_IS_TBL(t)) lua_raisef(c, "bad argument #1 to 'setmetatable'");
    if (LV_IS_NIL(mt)) {
        lua_table_set_metatable(LV_AS_TBL(t), NULL);
        LV_AS_TBL(t)->gc.weak_mode = 0;
    } else if (LV_IS_TBL(mt)) {
        lua_table_set_metatable(LV_AS_TBL(t), LV_AS_TBL(mt));
        // Pick up __mode for weak tables.
        LuaValue mode = lua_table_get_str(LV_AS_TBL(mt), lua_str_intern("__mode"));
        uint8_t wm = 0;
        if (LV_IS_STR(mode)) {
            const char *s = lua_str_data(LV_AS_STR(mode));
            for (; *s; s++) {
                if (*s == 'k') wm |= 1;
                if (*s == 'v') wm |= 2;
            }
        }
        LV_AS_TBL(t)->gc.weak_mode = wm;
    } else lua_raisef(c, "bad argument #2 to 'setmetatable'");
    return RESULT_OK(t);
}

static RESULT
b_collectgarbage(CTX *c, LuaValue *args, uint32_t argc)
{
    const char *cmd = "collect";
    if (argc >= 1 && LV_IS_STR(ARG(0))) cmd = lua_str_data(LV_AS_STR(ARG(0)));
    if (strcmp(cmd, "collect") == 0) {
        luastro_gc_collect(c);
        return RESULT_OK(LUAV_INT(0));
    }
    if (strcmp(cmd, "count") == 0) {
        return RESULT_OK(LUAV_FLOAT((double)luastro_gc_total() / 1024.0));
    }
    if (strcmp(cmd, "stop") == 0 || strcmp(cmd, "restart") == 0 ||
        strcmp(cmd, "step") == 0) {
        return RESULT_OK(LUAV_INT(0));
    }
    return RESULT_OK(LUAV_INT(0));
}

static RESULT
b_getmetatable(CTX *c, LuaValue *args, uint32_t argc)
{
    (void)c;
    LuaValue t = ARG(0);
    if (!LV_IS_TBL(t)) return RESULT_OK(LUAV_NIL);
    struct LuaTable *mt = lua_table_metatable(LV_AS_TBL(t));
    return RESULT_OK(mt ? LUAV_TABLE(mt) : LUAV_NIL);
}

static RESULT
b_rawget(CTX *c, LuaValue *args, uint32_t argc)
{
    (void)c;
    if (!LV_IS_TBL(ARG(0))) return RESULT_OK(LUAV_NIL);
    return RESULT_OK(lua_table_get(LV_AS_TBL(ARG(0)), ARG(1)));
}

static RESULT
b_rawset(CTX *c, LuaValue *args, uint32_t argc)
{
    if (!LV_IS_TBL(ARG(0))) lua_raisef(c, "bad argument #1 to 'rawset'");
    lua_table_set(LV_AS_TBL(ARG(0)), ARG(1), ARG(2));
    return RESULT_OK(ARG(0));
}

static RESULT
b_rawequal(CTX *c, LuaValue *args, uint32_t argc)
{
    (void)c;
    return RESULT_OK(LUAV_BOOL(lua_eq_raw(ARG(0), ARG(1))));
}
extern bool lua_eq_raw(LuaValue, LuaValue);

static RESULT
b_rawlen(CTX *c, LuaValue *args, uint32_t argc)
{
    (void)c;
    LuaValue v = ARG(0);
    if (LV_IS_STR(v)) return RESULT_OK(LUAV_INT((int64_t)lua_str_len(LV_AS_STR(v))));
    if (LV_IS_TBL(v))  return RESULT_OK(LUAV_INT(lua_table_len(LV_AS_TBL(v))));
    return RESULT_OK(LUAV_INT(0));
}

// =====================================================================
// math.*
// =====================================================================

static RESULT m_abs  (CTX *c, LuaValue *args, uint32_t argc) { (void)c; (void)argc; LuaValue v = ARG(0); if (LV_IS_INT(v)) return RESULT_OK(LUAV_INT(LV_AS_INT(v) < 0 ? -LV_AS_INT(v) : LV_AS_INT(v))); double f = arg_num(c, v, 1); return RESULT_OK(LUAV_FLOAT(fabs(f))); }
static RESULT m_floor(CTX *c, LuaValue *args, uint32_t argc) { (void)argc; double f = arg_num(c, ARG(0), 1); return RESULT_OK(LUAV_INT((int64_t)floor(f))); }
static RESULT m_ceil (CTX *c, LuaValue *args, uint32_t argc) { (void)argc; double f = arg_num(c, ARG(0), 1); return RESULT_OK(LUAV_INT((int64_t)ceil(f)));  }
static RESULT m_sqrt (CTX *c, LuaValue *args, uint32_t argc) { (void)argc; return RESULT_OK(LUAV_FLOAT(sqrt(arg_num(c, ARG(0), 1)))); }
static RESULT m_sin  (CTX *c, LuaValue *args, uint32_t argc) { (void)argc; return RESULT_OK(LUAV_FLOAT(sin (arg_num(c, ARG(0), 1)))); }
static RESULT m_cos  (CTX *c, LuaValue *args, uint32_t argc) { (void)argc; return RESULT_OK(LUAV_FLOAT(cos (arg_num(c, ARG(0), 1)))); }
static RESULT m_tan  (CTX *c, LuaValue *args, uint32_t argc) { (void)argc; return RESULT_OK(LUAV_FLOAT(tan (arg_num(c, ARG(0), 1)))); }
static RESULT m_asin (CTX *c, LuaValue *args, uint32_t argc) { (void)argc; return RESULT_OK(LUAV_FLOAT(asin(arg_num(c, ARG(0), 1)))); }
static RESULT m_acos (CTX *c, LuaValue *args, uint32_t argc) { (void)argc; return RESULT_OK(LUAV_FLOAT(acos(arg_num(c, ARG(0), 1)))); }
static RESULT m_atan (CTX *c, LuaValue *args, uint32_t argc) {
    double y = arg_num(c, ARG(0), 1);
    double x = argc >= 2 ? arg_num(c, ARG(1), 2) : 1.0;
    return RESULT_OK(LUAV_FLOAT(atan2(y, x)));
}
static RESULT m_exp  (CTX *c, LuaValue *args, uint32_t argc) { (void)argc; return RESULT_OK(LUAV_FLOAT(exp(arg_num(c, ARG(0), 1)))); }
static RESULT m_log  (CTX *c, LuaValue *args, uint32_t argc) {
    double x = arg_num(c, ARG(0), 1);
    if (argc < 2) return RESULT_OK(LUAV_FLOAT(log(x)));
    double base = arg_num(c, ARG(1), 2);
    return RESULT_OK(LUAV_FLOAT(log(x) / log(base)));
}
static RESULT m_max(CTX *c, LuaValue *args, uint32_t argc) {
    if (argc == 0) lua_raisef(c, "bad argument #1 to 'max'");
    LuaValue best = args[0];
    for (uint32_t i = 1; i < argc; i++) if (lua_lt(c, best, args[i])) best = args[i];
    return RESULT_OK(best);
}
static RESULT m_min(CTX *c, LuaValue *args, uint32_t argc) {
    if (argc == 0) lua_raisef(c, "bad argument #1 to 'min'");
    LuaValue best = args[0];
    for (uint32_t i = 1; i < argc; i++) if (lua_lt(c, args[i], best)) best = args[i];
    return RESULT_OK(best);
}
static RESULT m_random(CTX *c, LuaValue *args, uint32_t argc) {
    (void)c; (void)args;
    if (argc == 0) {
        return RESULT_OK(LUAV_FLOAT((double)rand() / (double)RAND_MAX));
    }
    int64_t lo = 1, hi;
    if (argc == 1) { hi = arg_int(c, args[0], 1); }
    else        { lo = arg_int(c, args[0], 1); hi = arg_int(c, args[1], 2); }
    if (hi < lo) lua_raisef(c, "interval is empty");
    int64_t r = lo + ((int64_t)rand() % (hi - lo + 1));
    return RESULT_OK(LUAV_INT(r));
}
static RESULT m_fmod(CTX *c, LuaValue *args, uint32_t argc) {
    (void)argc;
    if (LV_IS_INT(ARG(0)) && LV_IS_INT(ARG(1))) {
        int64_t a = LV_AS_INT(ARG(0)), b = LV_AS_INT(ARG(1));
        if (b == 0) lua_raisef(c, "bad argument #2 to 'fmod' (zero)");
        return RESULT_OK(LUAV_INT(a % b));
    }
    double x = arg_num(c, ARG(0), 1); double y = arg_num(c, ARG(1), 2);
    return RESULT_OK(LUAV_FLOAT(fmod(x, y)));
}
static RESULT m_modf(CTX *c, LuaValue *args, uint32_t argc) {
    (void)argc; double x = arg_num(c, ARG(0), 1); double ip; double fp = modf(x, &ip);
    c->ret_info.results[0] = LUAV_FLOAT(ip);
    c->ret_info.results[1] = LUAV_FLOAT(fp);
    c->ret_info.result_cnt = 2;
    return RESULT_OK(LUAV_FLOAT(ip));
}
static RESULT m_pow (CTX *c, LuaValue *args, uint32_t argc) { (void)argc; return RESULT_OK(LUAV_FLOAT(pow(arg_num(c, ARG(0), 1), arg_num(c, ARG(1), 2)))); }
static RESULT m_tointeger(CTX *c, LuaValue *args, uint32_t argc) {
    (void)c; (void)argc;
    int64_t i; if (lua_to_int(ARG(0), &i)) return RESULT_OK(LUAV_INT(i));
    return RESULT_OK(LUAV_NIL);
}
static RESULT m_type(CTX *c, LuaValue *args, uint32_t argc) {
    (void)c; (void)argc;
    if (LV_IS_INT(ARG(0)))   return RESULT_OK(LUAV_STR(lua_str_intern("integer")));
    if (LV_IS_FLOAT(ARG(0))) return RESULT_OK(LUAV_STR(lua_str_intern("float")));
    return RESULT_OK(LUAV_NIL);
}

// =====================================================================
// string.*
// =====================================================================

static RESULT
s_len(CTX *c, LuaValue *args, uint32_t argc) {
    (void)c; (void)argc;
    if (!LV_IS_STR(ARG(0))) lua_raisef(c, "bad argument #1 to 'len'");
    return RESULT_OK(LUAV_INT((int64_t)lua_str_len(LV_AS_STR(ARG(0)))));
}

static int64_t
norm_idx(int64_t i, size_t len)
{
    if (i < 0) i = (int64_t)len + i + 1;
    if (i < 1) i = 1;
    if ((size_t)i > len + 1) i = (int64_t)len + 1;
    return i;
}

static RESULT
s_sub(CTX *c, LuaValue *args, uint32_t argc) {
    if (!LV_IS_STR(ARG(0))) lua_raisef(c, "bad argument #1 to 'sub'");
    const char *p = lua_str_data(LV_AS_STR(ARG(0)));
    size_t len = lua_str_len(LV_AS_STR(ARG(0)));
    int64_t i = arg_int(c, ARG(1), 2);
    int64_t j = argc >= 3 ? arg_int(c, ARG(2), 3) : (int64_t)len;
    i = norm_idx(i, len);
    j = j < 0 ? (int64_t)len + j + 1 : (j > (int64_t)len ? (int64_t)len : j);
    if (j < i) return RESULT_OK(LUAV_STR(lua_str_intern("")));
    return RESULT_OK(LUAV_STR(lua_str_intern_n(p + i - 1, (size_t)(j - i + 1))));
}

static RESULT
s_upper(CTX *c, LuaValue *args, uint32_t argc) {
    (void)c; (void)argc;
    if (!LV_IS_STR(ARG(0))) lua_raisef(c, "bad argument #1 to 'upper'");
    size_t len = lua_str_len(LV_AS_STR(ARG(0)));
    char *buf = (char *)malloc(len);
    for (size_t i = 0; i < len; i++) buf[i] = (char)toupper((unsigned char)lua_str_data(LV_AS_STR(ARG(0)))[i]);
    LuaValue r = LUAV_STR(lua_str_intern_n(buf, len)); free(buf); return RESULT_OK(r);
}

static RESULT
s_lower(CTX *c, LuaValue *args, uint32_t argc) {
    (void)c; (void)argc;
    if (!LV_IS_STR(ARG(0))) lua_raisef(c, "bad argument #1 to 'lower'");
    size_t len = lua_str_len(LV_AS_STR(ARG(0)));
    char *buf = (char *)malloc(len);
    for (size_t i = 0; i < len; i++) buf[i] = (char)tolower((unsigned char)lua_str_data(LV_AS_STR(ARG(0)))[i]);
    LuaValue r = LUAV_STR(lua_str_intern_n(buf, len)); free(buf); return RESULT_OK(r);
}

static RESULT
s_rep(CTX *c, LuaValue *args, uint32_t argc) {
    (void)argc;
    if (!LV_IS_STR(ARG(0))) lua_raisef(c, "bad argument #1 to 'rep'");
    int64_t k = arg_int(c, ARG(1), 2);
    if (k < 0) k = 0;
    size_t len = lua_str_len(LV_AS_STR(ARG(0)));
    char *buf = (char *)malloc(len * (size_t)k + 1);
    for (int64_t i = 0; i < k; i++) memcpy(buf + i * (int64_t)len, lua_str_data(LV_AS_STR(ARG(0))), len);
    LuaValue r = LUAV_STR(lua_str_intern_n(buf, len * (size_t)k));
    free(buf); return RESULT_OK(r);
}

static RESULT
s_byte(CTX *c, LuaValue *args, uint32_t argc) {
    if (!LV_IS_STR(ARG(0))) lua_raisef(c, "bad argument #1 to 'byte'");
    int64_t i = argc >= 2 ? arg_int(c, ARG(1), 2) : 1;
    size_t len = lua_str_len(LV_AS_STR(ARG(0)));
    if (i < 1 || (size_t)i > len) return RESULT_OK(LUAV_NIL);
    return RESULT_OK(LUAV_INT((int64_t)(unsigned char)lua_str_data(LV_AS_STR(ARG(0)))[i - 1]));
}

static RESULT
s_char(CTX *c, LuaValue *args, uint32_t argc) {
    (void)c;
    char buf[64];
    size_t L = argc < sizeof(buf) ? argc : sizeof(buf);
    for (size_t i = 0; i < L; i++) {
        int64_t v; lua_to_int(args[i], &v); buf[i] = (char)(unsigned char)v;
    }
    return RESULT_OK(LUAV_STR(lua_str_intern_n(buf, L)));
}

static RESULT
s_reverse(CTX *c, LuaValue *args, uint32_t argc) {
    (void)c; (void)argc;
    if (!LV_IS_STR(ARG(0))) lua_raisef(c, "bad argument #1 to 'reverse'");
    size_t len = lua_str_len(LV_AS_STR(ARG(0)));
    char *buf = (char *)malloc(len);
    for (size_t i = 0; i < len; i++) buf[i] = lua_str_data(LV_AS_STR(ARG(0)))[len - 1 - i];
    LuaValue r = LUAV_STR(lua_str_intern_n(buf, len)); free(buf); return RESULT_OK(r);
}

static RESULT
s_format(CTX *c, LuaValue *args, uint32_t argc) {
    if (!LV_IS_STR(ARG(0))) lua_raisef(c, "bad argument #1 to 'format'");
    const char *fmt = lua_str_data(LV_AS_STR(ARG(0)));
    char out[4096];
    size_t outpos = 0;
    uint32_t ai = 1;
    while (*fmt && outpos < sizeof(out) - 1) {
        if (*fmt != '%') { out[outpos++] = *fmt++; continue; }
        fmt++;
        char spec[32];
        size_t sp = 0;
        spec[sp++] = '%';
        while (*fmt && !strchr("dixXfgeEsqc%", *fmt) && sp < sizeof(spec) - 2) spec[sp++] = *fmt++;
        if (!*fmt) break;
        spec[sp++] = *fmt;
        spec[sp]   = '\0';
        char buf[256];
        switch (*fmt) {
        case '%': buf[0] = '%'; buf[1] = 0; break;
        case 'd': case 'i': case 'x': case 'X': case 'c': {
            uint32_t which = ai++;
            int64_t v = arg_int(c, ARG(which), (int)which);
            char rspec[40];
            size_t k = 0;
            for (size_t i = 0; spec[i] && spec[i] != *fmt; i++) rspec[k++] = spec[i];
            rspec[k++] = 'l'; rspec[k++] = 'l'; rspec[k++] = *fmt; rspec[k] = 0;
            snprintf(buf, sizeof(buf), rspec, (long long)v);
            break;
        }
        case 'f': case 'g': case 'e': case 'E': {
            uint32_t which = ai++;
            double v = arg_num(c, ARG(which), (int)which);
            snprintf(buf, sizeof(buf), spec, v);
            break;
        }
        case 's': {
            uint32_t which = ai++;
            struct LuaString *s = arg_string(c, ARG(which), (int)which);
            snprintf(buf, sizeof(buf), spec, lua_str_data(s));
            break;
        }
        case 'q': {
            uint32_t which = ai++;
            struct LuaString *s = arg_string(c, ARG(which), (int)which);
            snprintf(buf, sizeof(buf), "\"%s\"", lua_str_data(s));
            break;
        }
        default: buf[0] = *fmt; buf[1] = 0;
        }
        size_t l = strlen(buf);
        if (outpos + l >= sizeof(out)) l = sizeof(out) - 1 - outpos;
        memcpy(out + outpos, buf, l);
        outpos += l;
        fmt++;
    }
    out[outpos] = 0;
    return RESULT_OK(LUAV_STR(lua_str_intern_n(out, outpos)));
}

// --- Lua patterns ---  (implementation in lua_pattern.c)
#include "lua_pattern.c"

// --- coroutine ---
#include "lua_coroutine.c"

// Forward decl: uses set_lib_fn defined later in this file.
static void set_lib_fn(struct LuaTable *t, const char *name, lua_cfunc_ptr_t fn);

struct LuaCoroutine *luaco_create(CTX *c, LuaValue fn);
RESULT  luaco_resume(CTX *c, struct LuaCoroutine *co, LuaValue *args, uint32_t argc);
RESULT  luaco_yield(CTX *c, LuaValue *args, uint32_t argc);
const char *luaco_status_name(struct LuaCoroutine *co);
bool    luaco_is_yieldable(void);
struct LuaCoroutine *luaco_running(void);

static RESULT
co_create(CTX *c, LuaValue *args, uint32_t argc)
{
    (void)argc;
    LuaValue fn = ARG(0);
    if (!LV_IS_CALL(fn)) lua_raisef(c, "bad argument #1 to 'create'");
    struct LuaCoroutine *co = luaco_create(c, fn);
    if (!co) lua_raisef(c, "out of memory");
    return RESULT_OK(LUAV_THREAD(co));
}

static RESULT
co_resume(CTX *c, LuaValue *args, uint32_t argc)
{
    if (argc < 1 || !LV_IS_THREAD(ARG(0)))
        lua_raisef(c, "bad argument #1 to 'resume'");
    return luaco_resume(c, (struct LuaCoroutine *)LV_AS_PTR(ARG(0)), args + 1, argc - 1);
}

static RESULT
co_yield(CTX *c, LuaValue *args, uint32_t argc) { return luaco_yield(c, args, argc); }

static RESULT
co_status(CTX *c, LuaValue *args, uint32_t argc)
{
    (void)c; (void)argc;
    if (!LV_IS_THREAD(ARG(0))) return RESULT_OK(LUAV_STR(lua_str_intern("dead")));
    return RESULT_OK(LUAV_STR(lua_str_intern(luaco_status_name((struct LuaCoroutine *)LV_AS_PTR(ARG(0))))));
}

static RESULT
co_isyieldable(CTX *c, LuaValue *args, uint32_t argc)
{
    (void)c; (void)args; (void)argc;
    return RESULT_OK(LUAV_BOOL(luaco_is_yieldable()));
}

static RESULT
co_running(CTX *c, LuaValue *args, uint32_t argc)
{
    (void)args; (void)argc;
    struct LuaCoroutine *co = luaco_running();
    c->ret_info.results[0] = co ? LUAV_THREAD(co) : LUAV_NIL;
    c->ret_info.results[1] = LUAV_BOOL(co == NULL);
    c->ret_info.result_cnt = 2;
    return RESULT_OK(c->ret_info.results[0]);
}

// coroutine.wrap: returns a function that resumes the coroutine and
// re-raises errors instead of returning (false, msg).
static RESULT
co_wrap_iter(CTX *c, LuaValue *args, uint32_t argc)
{
    // The wrapper closure stores the coroutine in upvalue 0.  But we're
    // implementing this as a CFunction, which has no upvalues — we use
    // a closure-shaped LuaTable holding the coroutine instead.  The
    // first arg (passed through wrap()) is the table.
    if (LUASTRO_CUR_UPVALS == NULL) lua_raisef(c, "wrap: missing state");
    LuaValue stv = *LUASTRO_CUR_UPVALS[0];
    if (!LV_IS_THREAD(stv)) lua_raisef(c, "wrap: not a thread");
    RESULT r = luaco_resume(c, (struct LuaCoroutine *)LV_AS_PTR(stv), args, argc);
    // r.value is true on success, false on error.  On error we re-raise.
    if (LV_IS_BOOL(r) && LV_AS_BOOL(r) == 0) {
        lua_raise(c, c->ret_info.results[1]);
    }
    // Shift results down: drop the leading `true`.
    uint32_t n = c->ret_info.result_cnt;
    if (n > 1) {
        for (uint32_t i = 0; i + 1 < n; i++) c->ret_info.results[i] = c->ret_info.results[i + 1];
        c->ret_info.result_cnt = n - 1;
        return RESULT_OK(c->ret_info.results[0]);
    }
    c->ret_info.result_cnt = 0;
    return RESULT_OK(LUAV_NIL);
}

static RESULT
co_wrap(CTX *c, LuaValue *args, uint32_t argc)
{
    (void)argc;
    LuaValue fn = ARG(0);
    if (!LV_IS_CALL(fn)) lua_raisef(c, "bad argument #1 to 'wrap'");
    struct LuaCoroutine *co = luaco_create(c, fn);
    // We use a Lua closure with one upvalue cell holding the thread.
    // To avoid building a full closure, we just stash via a TCFUNC
    // whose name carries the pointer (hack) — but cleaner is to reuse
    // a LuaClosure that points at a tiny dummy body.
    //
    // Simpler: emit an error if wrap is called for now and tell users
    // to use coroutine.resume directly.  We can revisit if a benchmark
    // depends on wrap.
    (void)co;
    lua_raisef(c, "coroutine.wrap not implemented in v1 — use coroutine.resume");
    return RESULT_OK(LUAV_NIL);
}

static void
luastro_init_coroutine(CTX *c)
{
    struct LuaTable *co_tbl = lua_table_new(0, 8);
    set_lib_fn(co_tbl, "create",      co_create);
    set_lib_fn(co_tbl, "resume",      co_resume);
    set_lib_fn(co_tbl, "yield",       co_yield);
    set_lib_fn(co_tbl, "status",      co_status);
    set_lib_fn(co_tbl, "isyieldable", co_isyieldable);
    set_lib_fn(co_tbl, "running",     co_running);
    set_lib_fn(co_tbl, "wrap",        co_wrap);
    lua_table_set_str(c->globals, lua_str_intern("coroutine"), LUAV_TABLE(co_tbl));
}

static void
push_captures(CTX *c, struct LuaString *src, struct luapat_cap *caps, int n,
              size_t start, size_t end, int with_se)
{
    int idx = 0;
    if (with_se) {
        c->ret_info.results[idx++] = LUAV_INT((int64_t)start + 1);
        c->ret_info.results[idx++] = LUAV_INT((int64_t)end);
    }
    if (n == 0 && !with_se) {
        c->ret_info.results[idx++] = LUAV_STR(lua_str_intern_n(
            lua_str_data(src) + start, end - start));
    }
    for (int i = 0; i < n && idx < LUASTRO_MAX_RETS; i++) {
        if (caps[i].len < 0) {
            c->ret_info.results[idx++] = LUAV_INT(caps[i].start + 1);   // position capture
        } else {
            c->ret_info.results[idx++] = LUAV_STR(lua_str_intern_n(
                lua_str_data(src) + caps[i].start, caps[i].len));
        }
    }
    c->ret_info.result_cnt = idx;
}

static RESULT
s_find(CTX *c, LuaValue *args, uint32_t argc)
{
    if (!LV_IS_STR(ARG(0)) || !LV_IS_STR(ARG(1)))
        lua_raisef(c, "bad arguments to 'find'");
    struct LuaString *src = LV_AS_STR(ARG(0)), *pat = LV_AS_STR(ARG(1));
    int64_t init = argc >= 3 ? arg_int(c, ARG(2), 3) : 1;
    bool plain = argc >= 4 && LV_TRUTHY(ARG(3));
    if (init < 0) init = (int64_t)src->len + init + 1;
    if (init < 1) init = 1;
    if ((size_t)(init - 1) > src->len) {
        c->ret_info.result_cnt = 0;
        return RESULT_OK(LUAV_NIL);
    }
    if (plain) {
        const char *s = lua_str_data(src) + (init - 1);
        const char *p = strstr(s, lua_str_data(pat));
        if (!p) { c->ret_info.result_cnt = 0; return RESULT_OK(LUAV_NIL); }
        int64_t start = p - lua_str_data(src) + 1;
        int64_t end   = start + (int64_t)pat->len - 1;
        c->ret_info.results[0] = LUAV_INT(start);
        c->ret_info.results[1] = LUAV_INT(end);
        c->ret_info.result_cnt = 2;
        return RESULT_OK(LUAV_INT(start));
    }
    size_t mstart, mend;
    struct luapat_cap caps[LUAPAT_MAXCAP];
    int ncaps;
    if (!luapat_match(lua_str_data(src), src->len, lua_str_data(pat), pat->len,
                      (size_t)(init - 1), &mstart, &mend, caps, &ncaps)) {
        c->ret_info.result_cnt = 0;
        return RESULT_OK(LUAV_NIL);
    }
    push_captures(c, src, caps, ncaps, mstart, mend, 1);
    return RESULT_OK(c->ret_info.results[0]);
}

static RESULT
s_match(CTX *c, LuaValue *args, uint32_t argc)
{
    if (!LV_IS_STR(ARG(0)) || !LV_IS_STR(ARG(1)))
        lua_raisef(c, "bad arguments to 'match'");
    struct LuaString *src = LV_AS_STR(ARG(0)), *pat = LV_AS_STR(ARG(1));
    int64_t init = argc >= 3 ? arg_int(c, ARG(2), 3) : 1;
    if (init < 0) init = (int64_t)src->len + init + 1;
    if (init < 1) init = 1;
    size_t mstart, mend;
    struct luapat_cap caps[LUAPAT_MAXCAP];
    int ncaps;
    if (!luapat_match(lua_str_data(src), src->len, lua_str_data(pat), pat->len,
                      (size_t)(init - 1), &mstart, &mend, caps, &ncaps)) {
        c->ret_info.result_cnt = 0;
        return RESULT_OK(LUAV_NIL);
    }
    push_captures(c, src, caps, ncaps, mstart, mend, 0);
    return RESULT_OK(c->ret_info.results[0]);
}

// gmatch returns an iterator stateful via a closure-with-table — we
// fake it by allocating a table with {src, pat, pos} and a shared
// iterator C-fn that reads/writes the table.
static struct LuaTable *gmatch_state(CTX *c, struct LuaString *s, struct LuaString *p) {
    struct LuaTable *t = lua_table_new(0, 4);
    lua_table_set_str(t, lua_str_intern("s"), LUAV_STR(s));
    lua_table_set_str(t, lua_str_intern("p"), LUAV_STR(p));
    lua_table_set_str(t, lua_str_intern("i"), LUAV_INT(0));
    return t;
}

static RESULT
s_gmatch_iter(CTX *c, LuaValue *args, uint32_t argc)
{
    (void)argc;
    LuaValue st = ARG(0);   // the state table
    if (!LV_IS_TBL(st)) { c->ret_info.result_cnt = 0; return RESULT_OK(LUAV_NIL); }
    LuaValue sv = lua_table_get_str(LV_AS_TBL(st), lua_str_intern("s"));
    LuaValue pv = lua_table_get_str(LV_AS_TBL(st), lua_str_intern("p"));
    LuaValue iv = lua_table_get_str(LV_AS_TBL(st), lua_str_intern("i"));
    if (!LV_IS_STR(sv) || !LV_IS_STR(pv)) { c->ret_info.result_cnt = 0; return RESULT_OK(LUAV_NIL); }
    int64_t i; lua_to_int(iv, &i);
    if ((size_t)i > LV_AS_STR(sv)->len) { c->ret_info.result_cnt = 0; return RESULT_OK(LUAV_NIL); }
    size_t mstart, mend;
    struct luapat_cap caps[LUAPAT_MAXCAP]; int ncaps;
    if (!luapat_match(lua_str_data(LV_AS_STR(sv)), LV_AS_STR(sv)->len,
                      lua_str_data(LV_AS_STR(pv)), LV_AS_STR(pv)->len,
                      (size_t)i, &mstart, &mend, caps, &ncaps)) {
        c->ret_info.result_cnt = 0;
        return RESULT_OK(LUAV_NIL);
    }
    int64_t advance = (int64_t)mend > i ? (int64_t)mend : i + 1;
    lua_table_set_str(LV_AS_TBL(st), lua_str_intern("i"), LUAV_INT(advance));
    push_captures(c, LV_AS_STR(sv), caps, ncaps, mstart, mend, 0);
    return RESULT_OK(c->ret_info.results[0]);
}

static RESULT
s_gmatch(CTX *c, LuaValue *args, uint32_t argc)
{
    (void)argc;
    if (!LV_IS_STR(ARG(0)) || !LV_IS_STR(ARG(1)))
        lua_raisef(c, "bad arguments to 'gmatch'");
    LuaValue iter = LUAV_CFUNC(lua_cfunc_new("gmatch_iter", s_gmatch_iter));
    LuaValue st   = LUAV_TABLE(gmatch_state(c, LV_AS_STR(ARG(0)), LV_AS_STR(ARG(1))));
    c->ret_info.results[0] = iter;
    c->ret_info.results[1] = st;
    c->ret_info.results[2] = LUAV_NIL;
    c->ret_info.result_cnt = 3;
    return RESULT_OK(iter);
}

// gsub: src, pat, repl [, max].  repl is a string with %0..%9 backrefs
// or a function or a table — we support string and function.
static RESULT
s_gsub(CTX *c, LuaValue *args, uint32_t argc)
{
    if (!LV_IS_STR(ARG(0)) || !LV_IS_STR(ARG(1)))
        lua_raisef(c, "bad arguments to 'gsub'");
    struct LuaString *src = LV_AS_STR(ARG(0)), *pat = LV_AS_STR(ARG(1));
    LuaValue repl = ARG(2);
    int64_t max_subs = argc >= 4 ? arg_int(c, ARG(3), 4) : -1;
    const char *s = lua_str_data(src);
    size_t pos = 0, srclen = src->len;
    char *out = (char *)malloc(srclen * 2 + 16);
    size_t outcap = srclen * 2 + 16, outlen = 0;
    int64_t nsub = 0;

    while (pos <= srclen && (max_subs < 0 || nsub < max_subs)) {
        size_t mstart, mend;
        struct luapat_cap caps[LUAPAT_MAXCAP]; int ncaps;
        if (!luapat_match(s, srclen, lua_str_data(pat), pat->len,
                          pos, &mstart, &mend, caps, &ncaps)) break;
        if (outlen + (mstart - pos) >= outcap) {
            outcap = outcap * 2 + (mstart - pos);
            out = (char *)realloc(out, outcap);
        }
        memcpy(out + outlen, s + pos, mstart - pos);
        outlen += mstart - pos;

        // Build replacement
        char buf[1024]; size_t blen = 0;
        if (LV_IS_STR(repl)) {
            const char *r = lua_str_data(LV_AS_STR(repl));
            for (size_t i = 0; i < LV_AS_STR(repl)->len; i++) {
                if (r[i] == '%' && i + 1 < LV_AS_STR(repl)->len) {
                    char d = r[i + 1];
                    if (d == '%') { buf[blen++] = '%'; }
                    else if (d == '0') {
                        size_t L = mend - mstart;
                        memcpy(buf + blen, s + mstart, L); blen += L;
                    } else if (d >= '1' && d <= '9' && (d - '0') <= ncaps) {
                        struct luapat_cap *cap = &caps[d - '1'];
                        if (cap->len >= 0) { memcpy(buf + blen, s + cap->start, cap->len); blen += cap->len; }
                    }
                    i++;
                } else buf[blen++] = r[i];
            }
        } else if (LV_IS_CALL(repl)) {
            // Call repl with captures (or whole match if no captures).
            LuaValue argv_r[LUAPAT_MAXCAP];
            int n_arg = ncaps;
            if (ncaps == 0) {
                argv_r[0] = LUAV_STR(lua_str_intern_n(s + mstart, mend - mstart));
                n_arg = 1;
            } else {
                for (int i = 0; i < ncaps; i++) {
                    if (caps[i].len < 0) argv_r[i] = LUAV_INT(caps[i].start + 1);
                    else argv_r[i] = LUAV_STR(lua_str_intern_n(s + caps[i].start, caps[i].len));
                }
            }
            RESULT rr = lua_call(c, repl, argv_r, n_arg);
            if (LV_IS_STR(rr)) {
                memcpy(buf + blen, lua_str_data(LV_AS_STR(rr)), LV_AS_STR(rr)->len);
                blen += LV_AS_STR(rr)->len;
            } else if (LV_IS_NUM(rr)) {
                LuaValue ss = lua_tostring(c, rr);
                memcpy(buf + blen, lua_str_data(LV_AS_STR(ss)), LV_AS_STR(ss)->len);
                blen += LV_AS_STR(ss)->len;
            } else {
                // false / nil → keep original
                size_t L = mend - mstart;
                memcpy(buf + blen, s + mstart, L); blen += L;
            }
        } else {
            size_t L = mend - mstart;
            memcpy(buf + blen, s + mstart, L); blen += L;
        }
        if (outlen + blen >= outcap) { outcap = outcap * 2 + blen; out = (char *)realloc(out, outcap); }
        memcpy(out + outlen, buf, blen); outlen += blen;
        nsub++;
        if (mend == pos) {
            if (pos < srclen) out[outlen++] = s[pos];
            pos++;
        } else {
            pos = mend;
        }
    }
    if (outlen + (srclen - pos) >= outcap) {
        outcap = outlen + (srclen - pos) + 1;
        out = (char *)realloc(out, outcap);
    }
    memcpy(out + outlen, s + pos, srclen - pos); outlen += srclen - pos;

    LuaValue r = LUAV_STR(lua_str_intern_n(out, outlen));
    free(out);
    c->ret_info.results[0] = r;
    c->ret_info.results[1] = LUAV_INT(nsub);
    c->ret_info.result_cnt = 2;
    return RESULT_OK(r);
}

// =====================================================================
// table.*
// =====================================================================

static RESULT
t_insert(CTX *c, LuaValue *args, uint32_t argc) {
    if (!LV_IS_TBL(ARG(0))) lua_raisef(c, "bad argument #1 to 'insert'");
    struct LuaTable *t = LV_AS_TBL(ARG(0));
    if (argc == 2) { lua_table_seti(t, lua_table_len(t) + 1, ARG(1)); return RESULT_OK(LUAV_NIL); }
    int64_t pos = arg_int(c, ARG(1), 2);
    int64_t len = lua_table_len(t);
    for (int64_t i = len; i >= pos; i--) lua_table_seti(t, i + 1, lua_table_geti(t, i));
    lua_table_seti(t, pos, ARG(2));
    return RESULT_OK(LUAV_NIL);
}

static RESULT
t_remove(CTX *c, LuaValue *args, uint32_t argc) {
    if (!LV_IS_TBL(ARG(0))) lua_raisef(c, "bad argument #1 to 'remove'");
    struct LuaTable *t = LV_AS_TBL(ARG(0));
    int64_t len = lua_table_len(t);
    int64_t pos = argc >= 2 ? arg_int(c, ARG(1), 2) : len;
    if (len == 0) return RESULT_OK(LUAV_NIL);
    LuaValue removed = lua_table_geti(t, pos);
    for (int64_t i = pos; i < len; i++) lua_table_seti(t, i, lua_table_geti(t, i + 1));
    lua_table_seti(t, len, LUAV_NIL);
    return RESULT_OK(removed);
}

static RESULT
t_concat(CTX *c, LuaValue *args, uint32_t argc) {
    if (!LV_IS_TBL(ARG(0))) lua_raisef(c, "bad argument #1 to 'concat'");
    struct LuaTable *t = LV_AS_TBL(ARG(0));
    const char *sep = argc >= 2 ? lua_str_data(arg_string(c, ARG(1), 2)) : "";
    int64_t i = argc >= 3 ? arg_int(c, ARG(2), 3) : 1;
    int64_t j = argc >= 4 ? arg_int(c, ARG(3), 4) : lua_table_len(t);
    size_t cap = 64, len = 0;
    char *buf = (char *)malloc(cap);
    for (int64_t k = i; k <= j; k++) {
        LuaValue v = lua_table_geti(t, k);
        LuaValue s = lua_tostring(c, v);
        size_t sl = lua_str_len(LV_AS_STR(s));
        size_t add = sl + (k > i ? strlen(sep) : 0);
        if (len + add + 1 > cap) { while (len + add + 1 > cap) cap *= 2; buf = (char *)realloc(buf, cap); }
        if (k > i) { memcpy(buf + len, sep, strlen(sep)); len += strlen(sep); }
        memcpy(buf + len, lua_str_data(LV_AS_STR(s)), sl); len += sl;
    }
    LuaValue r = LUAV_STR(lua_str_intern_n(buf, len));
    free(buf);
    return RESULT_OK(r);
}

static RESULT
t_unpack(CTX *c, LuaValue *args, uint32_t argc) {
    if (!LV_IS_TBL(ARG(0))) lua_raisef(c, "bad argument #1 to 'unpack'");
    struct LuaTable *t = LV_AS_TBL(ARG(0));
    int64_t i = argc >= 2 ? arg_int(c, ARG(1), 2) : 1;
    int64_t j = argc >= 3 ? arg_int(c, ARG(2), 3) : lua_table_len(t);
    uint32_t cnt = j >= i ? (uint32_t)(j - i + 1) : 0;
    if (cnt > LUASTRO_MAX_RETS) cnt = LUASTRO_MAX_RETS;
    for (uint32_t k = 0; k < cnt; k++) c->ret_info.results[k] = lua_table_geti(t, i + (int64_t)k);
    c->ret_info.result_cnt = cnt;
    return RESULT_OK(cnt ? c->ret_info.results[0] : LUAV_NIL);
}

static RESULT
t_pack(CTX *c, LuaValue *args, uint32_t argc) {
    (void)c;
    struct LuaTable *t = lua_table_new(argc, 1);
    for (uint32_t i = 0; i < argc; i++) lua_table_seti(t, (int64_t)i + 1, args[i]);
    lua_table_set_str(t, lua_str_intern("n"), LUAV_INT(argc));
    return RESULT_OK(LUAV_TABLE(t));
}

// =====================================================================
// io / os
// =====================================================================

static RESULT
io_write(CTX *c, LuaValue *args, uint32_t argc) {
    for (uint32_t i = 0; i < argc; i++) {
        LuaValue s = lua_tostring(c, args[i]);
        fwrite(lua_str_data(LV_AS_STR(s)), 1, lua_str_len(LV_AS_STR(s)), stdout);
    }
    c->ret_info.result_cnt = 0;
    return RESULT_OK(LUAV_NIL);
}

static RESULT
os_clock(CTX *c, LuaValue *args, uint32_t argc) {
    (void)c; (void)args; (void)argc;
    return RESULT_OK(LUAV_FLOAT((double)clock() / (double)CLOCKS_PER_SEC));
}

static RESULT
os_time(CTX *c, LuaValue *args, uint32_t argc) {
    (void)c; (void)args; (void)argc;
    return RESULT_OK(LUAV_INT((int64_t)time(NULL)));
}

static RESULT
os_exit(CTX *c, LuaValue *args, uint32_t argc) {
    (void)c; (void)argc;
    int64_t code = 0;
    if (argc >= 1) lua_to_int(args[0], &code);
    exit((int)code);
    return RESULT_OK(LUAV_NIL);
}

static RESULT
os_getenv(CTX *c, LuaValue *args, uint32_t argc) {
    (void)c; (void)argc;
    if (!LV_IS_STR(ARG(0))) return RESULT_OK(LUAV_NIL);
    const char *v = getenv(lua_str_data(LV_AS_STR(ARG(0))));
    return RESULT_OK(v ? LUAV_STR(lua_str_intern(v)) : LUAV_NIL);
}

// =====================================================================
// Globals registration
// =====================================================================

static void
set_g(CTX *c, const char *name, lua_cfunc_ptr_t fn)
{
    lua_table_set_str(c->globals, lua_str_intern(name),
                      LUAV_CFUNC(lua_cfunc_new(name, fn)));
}

static void
set_lib_fn(struct LuaTable *t, const char *name, lua_cfunc_ptr_t fn)
{
    lua_table_set_str(t, lua_str_intern(name),
                      LUAV_CFUNC(lua_cfunc_new(name, fn)));
}

void
luastro_init_globals(CTX *c)
{
    set_g(c, "print",         b_print);
    set_g(c, "tostring",      b_tostring);
    set_g(c, "tonumber",      b_tonumber);
    set_g(c, "type",          b_type);
    set_g(c, "ipairs",        b_ipairs);
    set_g(c, "pairs",         b_pairs);
    set_g(c, "error",         b_error);
    set_g(c, "pcall",         b_pcall);
    set_g(c, "xpcall",        b_xpcall);
    set_g(c, "assert",        b_assert);
    set_g(c, "select",        b_select);
    set_g(c, "setmetatable",  b_setmetatable);
    set_g(c, "getmetatable",  b_getmetatable);
    set_g(c, "rawget",        b_rawget);
    set_g(c, "rawset",        b_rawset);
    set_g(c, "rawequal",      b_rawequal);
    set_g(c, "rawlen",        b_rawlen);
    set_g(c, "unpack",        t_unpack);
    set_g(c, "collectgarbage", b_collectgarbage);

    struct LuaTable *math = lua_table_new(0, 24);
    set_lib_fn(math, "abs",       m_abs);
    set_lib_fn(math, "floor",     m_floor);
    set_lib_fn(math, "ceil",      m_ceil);
    set_lib_fn(math, "sqrt",      m_sqrt);
    set_lib_fn(math, "sin",       m_sin);
    set_lib_fn(math, "cos",       m_cos);
    set_lib_fn(math, "tan",       m_tan);
    set_lib_fn(math, "asin",      m_asin);
    set_lib_fn(math, "acos",      m_acos);
    set_lib_fn(math, "atan",      m_atan);
    set_lib_fn(math, "exp",       m_exp);
    set_lib_fn(math, "log",       m_log);
    set_lib_fn(math, "max",       m_max);
    set_lib_fn(math, "min",       m_min);
    set_lib_fn(math, "random",    m_random);
    set_lib_fn(math, "fmod",      m_fmod);
    set_lib_fn(math, "modf",      m_modf);
    set_lib_fn(math, "pow",       m_pow);
    set_lib_fn(math, "tointeger", m_tointeger);
    set_lib_fn(math, "type",      m_type);
    lua_table_set_str(math, lua_str_intern("pi"),       LUAV_FLOAT(3.141592653589793));
    lua_table_set_str(math, lua_str_intern("huge"),     LUAV_FLOAT(HUGE_VAL));
    lua_table_set_str(math, lua_str_intern("maxinteger"), LUAV_INT(INT64_MAX));
    lua_table_set_str(math, lua_str_intern("mininteger"), LUAV_INT(INT64_MIN));
    lua_table_set_str(c->globals, lua_str_intern("math"), LUAV_TABLE(math));

    struct LuaTable *str = lua_table_new(0, 16);
    set_lib_fn(str, "len",     s_len);
    set_lib_fn(str, "sub",     s_sub);
    set_lib_fn(str, "upper",   s_upper);
    set_lib_fn(str, "lower",   s_lower);
    set_lib_fn(str, "rep",     s_rep);
    set_lib_fn(str, "reverse", s_reverse);
    set_lib_fn(str, "byte",    s_byte);
    set_lib_fn(str, "char",    s_char);
    set_lib_fn(str, "format",  s_format);
    set_lib_fn(str, "find",    s_find);
    set_lib_fn(str, "match",   s_match);
    set_lib_fn(str, "gmatch",  s_gmatch);
    set_lib_fn(str, "gsub",    s_gsub);
    lua_table_set_str(c->globals, lua_str_intern("string"), LUAV_TABLE(str));

    struct LuaTable *tab = lua_table_new(0, 8);
    set_lib_fn(tab, "insert",  t_insert);
    set_lib_fn(tab, "remove",  t_remove);
    set_lib_fn(tab, "concat",  t_concat);
    set_lib_fn(tab, "unpack",  t_unpack);
    set_lib_fn(tab, "pack",    t_pack);
    lua_table_set_str(c->globals, lua_str_intern("table"), LUAV_TABLE(tab));

    struct LuaTable *io_t = lua_table_new(0, 4);
    set_lib_fn(io_t, "write",  io_write);
    lua_table_set_str(c->globals, lua_str_intern("io"), LUAV_TABLE(io_t));

    struct LuaTable *os_t = lua_table_new(0, 8);
    set_lib_fn(os_t, "clock",  os_clock);
    set_lib_fn(os_t, "time",   os_time);
    set_lib_fn(os_t, "exit",   os_exit);
    set_lib_fn(os_t, "getenv", os_getenv);
    lua_table_set_str(c->globals, lua_str_intern("os"), LUAV_TABLE(os_t));

    luastro_init_coroutine(c);
}
