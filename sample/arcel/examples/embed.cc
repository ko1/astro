// embed.cc — minimal C++ program embedding arcel.
//
// Demonstrates that arcel is fully C++-usable via the existing C
// header — no separate arcel.hpp / wrapper class is needed.  RAII via
// std::unique_ptr handles cleanup automatically.
//
// Same predicate as embed.c (the C version), just shows the C++ idiom:
//
//     bazel run //sample/arcel:embed_cc
//
// or with the Makefile:
//
//     g++ -std=c++17 -I.. embed.cc ../libarcel.a -ldl -lm -o embed_cc

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

extern "C" {
#include "arcel.h"
}

namespace {

// RAII wrappers — five lines that replace any need for a wrapper
// header.  Embedders can paste this snippet into their own code and
// move on.
struct ArcelEnvDeleter      { void operator()(arcel_env *e)        const noexcept { arcel_env_free(e); } };
struct ArcelProgramDeleter  { void operator()(arcel_program *p)    const noexcept { arcel_program_free(p); } };
struct ArcelActDeleter      { void operator()(arcel_activation *a) const noexcept { arcel_activation_free(a); } };

using EnvPtr     = std::unique_ptr<arcel_env,        ArcelEnvDeleter>;
using ProgramPtr = std::unique_ptr<arcel_program,    ArcelProgramDeleter>;
using ActPtr     = std::unique_ptr<arcel_activation, ArcelActDeleter>;

// One-call helper for the recurring "set u to a JSON snippet, eval,
// print result" pattern.  Realistic embedders would build the
// activation directly via arcel_activation_set_int / set_string etc.
// instead of going through JSON, but this keeps the example self-
// contained.
void example_eval(arcel_program *prg, arcel_activation *act,
                  const char *what, std::int64_t age,
                  const char *country, const char *role)
{
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  R"({"age":%lld,"country":"%s","role":"%s"})",
                  static_cast<long long>(age), country, role);
    arcel_activation_reset(act);
    arcel_activation_set_json(act, "u", buf, std::strlen(buf));

    arcel_value r = arcel_eval(prg, act);
    char out[64];
    arcel_format_json(r, out, sizeof out);
    std::printf("OK %-20s u={age:%lld country:%s role:%s} -> %s\n",
                what, static_cast<long long>(age), country, role, out);
}

}  // namespace

int
main()
{
    EnvPtr env(arcel_env_new());

    char err[256];
    ProgramPtr prg(arcel_compile(env.get(),
        R"(u.age >= 18 && u.country == "JP" && u.role in ["admin", "user"])",
        -1, err, sizeof err));
    if (!prg) {
        std::fprintf(stderr, "compile: %s\n", err);
        return 1;
    }

    ActPtr act(arcel_activation_new(env.get()));

    example_eval(prg.get(), act.get(), "matches",       25, "JP", "admin");
    example_eval(prg.get(), act.get(), "wrong country", 25, "US", "admin");
    example_eval(prg.get(), act.get(), "underage",      16, "JP", "user");
    example_eval(prg.get(), act.get(), "guest role",    30, "JP", "guest");
    return 0;
}
