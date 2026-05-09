// embed_protobuf.cc — evaluate CEL against a real libprotobuf
// message via the arcel_protobuf.h adapter.
//
// Build via Bazel (preferred — the proto + adapter are wired up
// in ../BUILD.bazel):
//
//     bazel run //sample/arcel:embed_protobuf
//
// What this demonstrates:
//   1. arcel itself stays protobuf-free (no libprotobuf in libarcel).
//   2. A 100-line C++ header (arcel_protobuf.h) bridges any
//      libprotobuf message to arcel via Reflection — works for every
//      .pb.h-generated type without per-type adapter code.
//   3. CEL expressions touch nested messages (`u.meta.created_at`)
//      with no special handling on either side.
//
// Same predicate as the other embed examples plus a nested-message
// access, so we can compare cost / shape across all three (JSON,
// native struct via desc, libprotobuf via desc).

#include <cstdio>
#include <memory>

extern "C" {
#include "arcel.h"
}
#include "examples/arcel_protobuf.h"
#include "examples/user_request.pb.h"

namespace {

void
example(arcel_program *prg, arcel_activation *act,
        const char *what, std::int64_t age,
        const char *country, const char *role,
        std::int64_t created_at,
        std::initializer_list<const char *> tags,
        std::initializer_list<std::pair<const char *, const char *>> labels)
{
    arcel::examples::UserRequest req;
    req.set_age(age);
    req.set_country(country);
    req.set_role(role);
    req.mutable_meta()->set_created_at(created_at);
    for (const char *t : tags) req.add_tags(t);
    for (auto &[k, v] : labels) (*req.mutable_labels())[k] = v;

    arcel_activation_reset(act);
    arcel_activation_set_object(act, "u", &req, &arcel::pbf::descriptor);

    arcel_value r = arcel_eval(prg, act);
    char buf[64];
    arcel_format_json(r, buf, sizeof buf);
    std::printf("OK %-25s tags=%zu labels=%zu -> %s\n",
                what, tags.size(), labels.size(), buf);
}

}  // namespace

int
main()
{
    arcel_env *env = arcel_env_new();

    char err[256];
    // The expression touches scalar fields on the top-level message
    // AND on a nested message — exercises both code paths in the
    // adapter.
    // The expression touches every shape the adapter supports:
    //   scalar fields   — u.age, u.country, u.role
    //   nested message  — u.meta.created_at
    //   repeated string — u.tags.all(t, t.startsWith("env:"))
    //   map<string,string> — u.labels["team"] == "platform"
    arcel_program *prg = arcel_compile(env,
        "u.age >= 18 "
        "&& u.country == \"JP\" "
        "&& u.role in [\"admin\", \"user\"] "
        "&& u.meta.created_at > 0 "
        "&& u.tags.all(t, t.startsWith(\"env:\")) "
        "&& u.labels[\"team\"] == \"platform\"",
        -1, err, sizeof err);
    if (!prg) {
        std::fprintf(stderr, "compile: %s\n", err);
        return 1;
    }

    arcel_activation *act = arcel_activation_new(env);

    using L = std::initializer_list<std::pair<const char *, const char *>>;
    L kPlatform = {{"team", "platform"}};
    L kInfra    = {{"team", "infra"}};

    example(prg, act, "matches",                  25, "JP", "admin", 1700000000, {"env:prod", "env:k8s"}, kPlatform);
    example(prg, act, "wrong team label",         25, "JP", "admin", 1700000000, {"env:prod"},            kInfra);
    example(prg, act, "missing label",            25, "JP", "admin", 1700000000, {"env:prod"},            {});
    example(prg, act, "tag not env:",             25, "JP", "admin", 1700000000, {"env:prod", "owner:x"}, kPlatform);
    example(prg, act, "wrong country",            25, "US", "admin", 1700000000, {"env:prod"},            kPlatform);
    example(prg, act, "underage",                 16, "JP", "user",  1700000000, {"env:prod"},            kPlatform);

    arcel_activation_free(act);
    arcel_program_free(prg);
    arcel_env_free(env);
    return 0;
}
