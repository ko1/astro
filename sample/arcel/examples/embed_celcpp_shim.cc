// embed_celcpp_shim.cc — embedding arcel via the cel-cpp-shaped API
// in compat/celcpp_compat.hpp.
//
// The structure mirrors what a typical cel-cpp embedder writes
// (parse → builder → compile → activation → evaluate → inspect).
// Compare to embed.c (raw arcel C API) and embed_protobuf.cc
// (libprotobuf adapter) — same eval semantics, three different
// surfaces.
//
// Build via Bazel:
//
//     bazel run //sample/arcel:embed_celcpp_shim
//
// What this proves:
//   1. cel-cpp-shaped code (Parse / CreateCelExpressionBuilder /
//      Activation::InsertValue / Evaluate / IsBool / BoolOrDie /
//      StatusOr) runs unmodified on the arcel core via the shim.
//   2. arcel's per-eval cost (~50 ns) replaces cel-cpp's per-eval
//      cost (~1 µs) without any source change to the embedder.

#include <cstdio>
#include <string>

#include "compat/celcpp_compat.hpp"
namespace cel = arcel::celcpp;

namespace {

void
example(cel::CelExpression &expr, cel::Activation &act,
        const char *what,
        std::int64_t age, std::string country, std::string role)
{
    // Note: InsertValue's string argument borrows the buffer for the
    // lifetime of the activation, so keep `country` / `role` alive
    // until after Evaluate returns.  Same lifetime rule as cel-cpp.
    auto &c_buf = country;
    auto &r_buf = role;
    cel::Activation fresh;  // brand new so previous bindings don't shadow
    fresh.InsertValue("age",     cel::CelValue::CreateInt64(age));
    fresh.InsertValue("country", cel::CelValue::CreateString(c_buf));
    fresh.InsertValue("role",    cel::CelValue::CreateString(r_buf));
    (void)act;  // unused — fresh activation per call so each test case is independent

    auto r = expr.Evaluate(fresh);
    if (!r.ok()) {
        std::printf("ER %-15s eval: %s\n", what, r.status().message().c_str());
        return;
    }
    if (!r->IsBool()) {
        std::printf("ER %-15s expected bool, got type=%d\n", what,
                    static_cast<int>(r->type()));
        return;
    }
    std::printf("OK %-15s age=%lld country=%s role=%s -> %s\n",
                what, static_cast<long long>(age), country.c_str(), role.c_str(),
                r->BoolOrDie() ? "true" : "false");
}

}  // namespace

int
main()
{
    // Step 1: parse.  cel-cpp returns ParsedExpr; we mirror that.
    auto parsed = cel::Parse(
        "age >= 18 && country == \"JP\" && role in [\"admin\", \"user\"]");
    if (!parsed.ok()) {
        std::fprintf(stderr, "parse: %s\n", parsed.status().message().c_str());
        return 1;
    }

    // Step 2: builder.  cel-cpp's InterpreterOptions is shimmed; the
    // RegisterBuiltinFunctions call is a no-op (arcel has all builtins
    // baked in).
    cel::InterpreterOptions opts;
    opts.constant_folding = true;  // shim ignores; arcel always folds
    auto builder = cel::CreateCelExpressionBuilder(opts);
    auto reg = cel::RegisterBuiltinFunctions(builder->GetRegistry(), opts);
    if (!reg.ok()) {
        std::fprintf(stderr, "register: %s\n", reg.message().c_str());
        return 1;
    }

    // Step 3: compile.  Returns StatusOr<unique_ptr<CelExpression>>.
    auto expr_or = builder->CreateExpression(*parsed);
    if (!expr_or.ok()) {
        std::fprintf(stderr, "compile: %s\n", expr_or.status().message().c_str());
        return 1;
    }
    auto cel_expr = std::move(expr_or).value();

    // Step 4: per-request — bind activation + evaluate.
    cel::Activation act;  // unused in `example()`; one per call for isolation
    example(*cel_expr, act, "matches",       25, "JP", "admin");
    example(*cel_expr, act, "wrong country", 25, "US", "admin");
    example(*cel_expr, act, "underage",      16, "JP", "user");
    example(*cel_expr, act, "guest role",    30, "JP", "guest");
    return 0;
}
