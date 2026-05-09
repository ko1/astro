// embed_bench.cc — in-process micro-benchmark across arcel's binding
// paths.  Single binary, single process: avoids `Open3`-style fork-per-
// case overhead and lets you compare the marginal cost of each input
// shape without subprocess noise.
//
// What it benches (all in one process, all on the SAME predicate):
//
//   1. arcel + JSON binding         (set_json from a stable JSON snippet)
//   2. arcel + native struct        (arcel_object_desc against a C struct)
//   3. arcel + libprotobuf message  (arcel_protobuf.h adapter)
//   4. arcel via cel-cpp shim       (compat/celcpp_compat.hpp Activation)
//
// Each row is run twice: once with arcel in plain interpreter mode
// (`--no-compile`-equivalent: arcel_env_set_no_compile(env, true)) and
// once with the AOT specializer engaged.  The AOT bake happens lazily
// inside arcel_compile, so we time outside of the parse/compile path
// (warm-up runs both; reported number is steady-state ns/op).
//
// Build via Bazel:
//
//     bazel build //sample/arcel:embed_bench
//     bazel-bin/sample/arcel/embed_bench               # default 1 s/case
//     bazel-bin/sample/arcel/embed_bench --secs 3      # longer for stable
//
// Why not also link cel-cpp here for a true 4-engine comparison: cel-cpp
// drags absl + re2 + a chunk of libprotobuf-lite as Bazel deps, and the
// MODULE.bazel for ASTro intentionally stays light.  benchmark/run.rb
// already does the cross-engine comparison via subprocess against
// celcpp_bench / celgo_ref.  This binary's job is the in-process
// arcel-side numbers — the ones you optimize against.

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "arcel.h"
}
#include "compat/celcpp_compat.hpp"
#include "examples/arcel_protobuf.h"
#include "examples/user_request.pb.h"

namespace {

// The same predicate every row evaluates.  Touches every feature the
// adapters need to bridge: scalar field, nested message, repeated
// string + macro, map<string,string>.  Mirrors the K8s-admission
// pattern most production CEL is shaped like.
constexpr const char *kExpr =
    "u.age >= 18 "
    "&& u.country == \"JP\" "
    "&& u.role in [\"admin\", \"user\"] "
    "&& u.meta.created_at > 0 "
    "&& u.tags.all(t, t.startsWith(\"env:\")) "
    "&& u.labels[\"team\"] == \"platform\"";

// ---- native struct adapter ------------------------------------------

struct native_user {
    std::int64_t age;
    const char  *country;
    const char  *role;
    std::int64_t created_at;
    const char **tags;            // NUL-terminated array of strings
    int          tags_len;
    const char **label_keys;      // parallel arrays for the label map
    const char **label_vals;
    int          labels_len;
};

// Subdescriptor for the nested `meta` message.  AC_OBJECT field can
// recursively yield another AC_OBJECT, so we model `u.meta` as a
// distinct descriptor that only knows about `created_at`.
extern "C" int native_meta_field(const arcel_object_desc *, const void *obj_,
                                 const char *name, std::size_t name_len,
                                 arcel_arena_handle *, arcel_value *out)
{
    const auto *u = static_cast<const native_user *>(obj_);
    if (name_len == 10 && std::memcmp(name, "created_at", 10) == 0) {
        *out = arcel_value_int(u->created_at);
        return 0;
    }
    return -1;
}
static const arcel_object_desc native_meta_desc = {
    .field     = native_meta_field,
    .has       = nullptr,
    .format_json = nullptr,
    .type_name = "UserMeta",
};

extern "C" int native_user_field(const arcel_object_desc *, const void *obj_,
                                 const char *name, std::size_t name_len,
                                 arcel_arena_handle *arena, arcel_value *out)
{
    const auto *u = static_cast<const native_user *>(obj_);
    auto IS = [&](const char *s, std::size_t n) {
        return name_len == n && std::memcmp(name, s, n) == 0;
    };
    if (IS("age", 3))     { *out = arcel_value_int(u->age);                          return 0; }
    if (IS("country", 7)) { *out = arcel_value_string(u->country, std::strlen(u->country)); return 0; }
    if (IS("role", 4))    { *out = arcel_value_string(u->role,    std::strlen(u->role));    return 0; }
    if (IS("meta", 4))    { *out = arcel_value_object(u, &native_meta_desc);         return 0; }
    if (IS("tags", 4)) {
        // Build a list in the eval arena.  Items pass through
        // arcel_value_string (interior pointers borrow the C strings,
        // which outlive the eval).
        arcel_value items[16];
        const int n = u->tags_len < 16 ? u->tags_len : 16;
        for (int i = 0; i < n; ++i)
            items[i] = arcel_value_string(u->tags[i], std::strlen(u->tags[i]));
        *out = arcel_value_list_new(arena, (std::uint32_t)n, items);
        return 0;
    }
    if (IS("labels", 6)) {
        arcel_value kv[32];
        const int n = u->labels_len < 16 ? u->labels_len : 16;
        for (int i = 0; i < n; ++i) {
            kv[2*i]     = arcel_value_string(u->label_keys[i], std::strlen(u->label_keys[i]));
            kv[2*i + 1] = arcel_value_string(u->label_vals[i], std::strlen(u->label_vals[i]));
        }
        *out = arcel_value_map_new(arena, (std::uint32_t)n, kv);
        return 0;
    }
    return -1;
}
static const arcel_object_desc native_user_desc = {
    .field       = native_user_field,
    .has         = nullptr,
    .format_json = nullptr,
    .type_name   = "UserRequest",
};

// ---- timer ----------------------------------------------------------

struct bench_result {
    double ns_per_op;
    std::int64_t iters;
    bool result;     // last evaluated boolean (so the optimizer can't
                     // dead-code-eliminate the eval call)
};

// Run `body` until `min_secs` of wall time has accumulated.  Body
// returns the predicate's bool value; we sample the FIRST result for
// the cross-row sanity check (XOR'ing all of them gives a parity that
// depends on iteration count, which is misleading), and we still
// observe each call via a sink so the optimizer can't dead-code it.
template <typename F>
bench_result
time_loop(double min_secs, F &&body)
{
    using clk = std::chrono::steady_clock;
    bool first = body();
    // Warm-up: run for ~50 ms to bring caches + branch predictors hot.
    auto warm_end = clk::now() + std::chrono::milliseconds(50);
    bool sink = first;
    while (clk::now() < warm_end) sink ^= body();

    // Measured run: keep doubling iters until elapsed >= min_secs.
    std::int64_t iters = 1024;
    while (true) {
        auto t0 = clk::now();
        for (std::int64_t i = 0; i < iters; ++i) sink ^= body();
        auto t1 = clk::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();
        if (secs >= min_secs) {
            asm volatile("" : : "r"(sink));    // observe to retain side effect
            return { secs * 1e9 / (double)iters, iters, first };
        }
        iters *= 2;
        if (iters > (1LL << 30)) {
            asm volatile("" : : "r"(sink));
            return { secs * 1e9 / (double)iters, iters, first };
        }
    }
}

// ---- per-row drivers ------------------------------------------------

bench_result
run_arcel_json(arcel_program *prg, arcel_activation *act,
               const char *json, double secs)
{
    return time_loop(secs, [&]() {
        arcel_activation_reset(act);
        arcel_activation_set_json(act, "u", json, std::strlen(json));
        arcel_value r = arcel_eval(prg, act);
        return arcel_type_of(r) == ARCEL_T_BOOL && arcel_get_bool(r);
    });
}

bench_result
run_arcel_native(arcel_program *prg, arcel_activation *act,
                 const native_user &u, double secs)
{
    return time_loop(secs, [&]() {
        arcel_activation_reset(act);
        arcel_activation_set_object(act, "u", &u, &native_user_desc);
        arcel_value r = arcel_eval(prg, act);
        return arcel_type_of(r) == ARCEL_T_BOOL && arcel_get_bool(r);
    });
}

bench_result
run_arcel_proto(arcel_program *prg, arcel_activation *act,
                const arcel::examples::UserRequest &req, double secs)
{
    return time_loop(secs, [&]() {
        arcel_activation_reset(act);
        arcel_activation_set_object(act, "u", &req, &arcel::pbf::descriptor);
        arcel_value r = arcel_eval(prg, act);
        return arcel_type_of(r) == ARCEL_T_BOOL && arcel_get_bool(r);
    });
}

bench_result
run_celcpp_shim(double secs, const native_user &u)
{
    namespace cel = arcel::celcpp;
    auto parsed = cel::Parse(kExpr);
    cel::InterpreterOptions iopts;
    auto builder = cel::CreateCelExpressionBuilder(iopts);
    cel::RegisterBuiltinFunctions(builder->GetRegistry(), iopts);
    auto cel_expr = builder->CreateExpression(parsed.value());

    cel::Activation act;
    return time_loop(secs, [&]() {
        // The shim's Activation::InsertObject mirrors arcel_activation_set_object.
        // Bind once per iter to amortize the same cost the arcel direct
        // path pays.
        act.InsertObject("u", &u, &native_user_desc);
        auto r = cel_expr.value()->Evaluate(act);
        return r.ok() && r.value().IsBool() && r.value().BoolOrDie();
    });
}

}  // namespace

int
main(int argc, char **argv)
{
    double secs = 1.0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--secs") == 0 && i + 1 < argc) {
            secs = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: %s [--secs N]\n", argv[0]);
            return 0;
        }
    }

    // ---- shared input data --------------------------------------------
    const char *json =
        "{\"age\":25,\"country\":\"JP\",\"role\":\"admin\","
        "\"meta\":{\"created_at\":1700000000},"
        "\"tags\":[\"env:prod\",\"env:k8s\"],"
        "\"labels\":{\"team\":\"platform\"}}";

    const char *tags[]   = { "env:prod", "env:k8s" };
    const char *lkeys[]  = { "team" };
    const char *lvals[]  = { "platform" };
    native_user nu = {
        .age = 25, .country = "JP", .role = "admin",
        .created_at = 1700000000,
        .tags = tags, .tags_len = 2,
        .label_keys = lkeys, .label_vals = lvals, .labels_len = 1,
    };

    arcel::examples::UserRequest req;
    req.set_age(25);
    req.set_country("JP");
    req.set_role("admin");
    req.mutable_meta()->set_created_at(1700000000);
    req.add_tags("env:prod"); req.add_tags("env:k8s");
    (*req.mutable_labels())["team"] = "platform";

    // ---- bench rows ----------------------------------------------------
    // For each binding path we run one program in interp mode and one in
    // AOT mode.  Both use the SAME arcel_env, but compile produces
    // separate programs.
    // OPTIMIZE() in the dispatcher only consults OPTION at AST ALLOC
    // time (during parse), not per-eval, so the interp-vs-AOT split is
    // baked into the prg_i / prg_a tree at compile_pair() time.  No
    // per-iteration OPTION flipping needed in the eval loop.
    auto run_pair = [&](const char *label, auto &&body_interp, auto &&body_aot) {
        std::printf("%-26s  ", label);
        std::fflush(stdout);
        bench_result a = body_interp();
        bench_result b = body_aot();
        const double speedup = (b.ns_per_op > 0) ? a.ns_per_op / b.ns_per_op : 0.0;
        std::printf("%9.1f ns/op   %9.1f ns/op   %4.2fx   (interp/AOT both = %s)\n",
                    a.ns_per_op, b.ns_per_op, speedup,
                    (a.result == b.result) ? "true" : "MISMATCH");
    };

    std::printf("# in-process arcel binding-path benchmark\n");
    std::printf("# expression: %s\n", kExpr);
    std::printf("# wall budget per measurement: %.2f s (warm-up: 50 ms)\n", secs);
    std::printf("# %-24s  %14s   %14s   %s\n",
                "binding", "interp", "AOT", "speedup");

    // Compile two programs per binding-cell: prg_i with global
    // OPTION.no_compiled_code=true (so per-ALLOC OPTIMIZE skips body
    // patching) and prg_a with =false (so OPTIMIZE swaps in the baked
    // SD body during parse).  Each set_no_compile call mutates the
    // global; doing it BEFORE arcel_compile is what gives a clean
    // interp/AOT split.
    auto compile_pair = [&](arcel_env **out_env_i, arcel_program **out_prg_i,
                            arcel_env **out_env_a, arcel_program **out_prg_a) {
        char err[256];
        *out_env_i = arcel_env_new();
        arcel_env_set_no_compile(*out_env_i, true);
        *out_prg_i = arcel_compile(*out_env_i, kExpr, -1, err, sizeof err);
        *out_env_a = arcel_env_new();
        arcel_env_set_no_compile(*out_env_a, false);
        *out_prg_a = arcel_compile(*out_env_a, kExpr, -1, err, sizeof err);
    };
    auto teardown_pair = [&](arcel_env *env_i, arcel_program *prg_i,
                             arcel_activation *act_i,
                             arcel_env *env_a, arcel_program *prg_a,
                             arcel_activation *act_a) {
        arcel_activation_free(act_i); arcel_activation_free(act_a);
        arcel_program_free(prg_i);    arcel_program_free(prg_a);
        arcel_env_free(env_i);        arcel_env_free(env_a);
    };

    {
        arcel_env *env_i, *env_a;
        arcel_program *prg_i, *prg_a;
        compile_pair(&env_i, &prg_i, &env_a, &prg_a);
        arcel_activation *act_i = arcel_activation_new(env_i);
        arcel_activation *act_a = arcel_activation_new(env_a);
        run_pair("1. arcel + JSON",
            [&] { return run_arcel_json(prg_i, act_i, json, secs); },
            [&] { return run_arcel_json(prg_a, act_a, json, secs); });
        teardown_pair(env_i, prg_i, act_i, env_a, prg_a, act_a);
    }
    {
        arcel_env *env_i, *env_a;
        arcel_program *prg_i, *prg_a;
        compile_pair(&env_i, &prg_i, &env_a, &prg_a);
        arcel_activation *act_i = arcel_activation_new(env_i);
        arcel_activation *act_a = arcel_activation_new(env_a);
        run_pair("2. arcel + C struct",
            [&] { return run_arcel_native(prg_i, act_i, nu, secs); },
            [&] { return run_arcel_native(prg_a, act_a, nu, secs); });
        teardown_pair(env_i, prg_i, act_i, env_a, prg_a, act_a);
    }
    {
        arcel_env *env_i, *env_a;
        arcel_program *prg_i, *prg_a;
        compile_pair(&env_i, &prg_i, &env_a, &prg_a);
        arcel_activation *act_i = arcel_activation_new(env_i);
        arcel_activation *act_a = arcel_activation_new(env_a);
        run_pair("3. arcel + libprotobuf",
            [&] { return run_arcel_proto(prg_i, act_i, req, secs); },
            [&] { return run_arcel_proto(prg_a, act_a, req, secs); });
        teardown_pair(env_i, prg_i, act_i, env_a, prg_a, act_a);
    }
    {
        // 4. cel-cpp shim binding (Activation::InsertObject in shim
        // bridges to arcel_activation_set_object under the hood).
        std::printf("%-26s  ", "4. cel-cpp shim + struct");
        std::fflush(stdout);
        // celcpp shim builds its own program internally; ensure
        // OPTION=false so the shim's compile picks up baked SDs.
        arcel_env *tmp = arcel_env_new();
        arcel_env_set_no_compile(tmp, false);
        bench_result r = run_celcpp_shim(secs, nu);
        arcel_env_free(tmp);
        std::printf("%9s          %9.1f ns/op   %4s   (shim eval, AOT in shim env)\n",
                    "n/a", r.ns_per_op, "—");
    }
    return 0;
}
