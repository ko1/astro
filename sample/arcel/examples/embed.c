/* embed.c — minimal C program embedding arcel via arcel.h.
 *
 * Builds against libarcel.a (or libarcel.so):
 *
 *     gcc -I.. embed.c ../libarcel.a -ldl -lm -o embed
 *
 * Demonstrates the production embedding pattern:
 *   - compile a CEL expression once
 *   - reuse the same activation across many requests
 *   - per-request: reset bindings, set inputs, evaluate, inspect
 *
 * This is the same path Envoy / gRPC interceptors / custom policy
 * evaluators would use.  No CLI / no JSON envelope / no subprocess.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arcel.h"

static void
example_eval(arcel_program *prg, arcel_activation *act,
             const char *what, int64_t age, const char *country, const char *role)
{
    arcel_activation_reset(act);
    /* Three top-level bindings; arcel_compile parsed `u.age`, `u.country`,
     * `u.role` etc. against these. */
    arcel_activation_set_json(act, "u",
        (char[]){0}, 0);   /* placeholder we'll overwrite below */

    /* Build u as a JSON snippet for simplicity.  A real embedder
     * would build a value tree directly via the (TODO) value
     * builders to avoid the JSON hop entirely. */
    char buf[256];
    snprintf(buf, sizeof buf, "{\"age\":%lld,\"country\":\"%s\",\"role\":\"%s\"}",
             (long long)age, country, role);
    arcel_activation_reset(act);
    arcel_activation_set_json(act, "u", buf, strlen(buf));

    arcel_value r = arcel_eval(prg, act);
    char out[64];
    arcel_format_json(r, out, sizeof out);
    printf("OK %-20s u={age:%lld country:%s role:%s} -> %s\n",
           what, (long long)age, country, role, out);
}

int
main(void)
{
    arcel_env *env = arcel_env_new();

    /* The same predicate we benchmark in benchmark/run.rb's
     * `predicate_user` case.  Compiled once. */
    char err[256];
    arcel_program *prg = arcel_compile(env,
        "u.age >= 18 && u.country == \"JP\" && u.role in [\"admin\", \"user\"]",
        -1, err, sizeof err);
    if (!prg) {
        fprintf(stderr, "compile: %s\n", err);
        return 1;
    }

    /* Reusable activation — same instance across all requests. */
    arcel_activation *act = arcel_activation_new(env);

    example_eval(prg, act, "matches",     25, "JP", "admin");
    example_eval(prg, act, "wrong country",25, "US", "admin");
    example_eval(prg, act, "underage",    16, "JP", "user");
    example_eval(prg, act, "guest role",  30, "JP", "guest");

    arcel_activation_free(act);
    arcel_program_free(prg);
    arcel_env_free(env);
    return 0;
}
