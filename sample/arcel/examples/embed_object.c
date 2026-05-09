/* embed_object.c — embedding arcel against a native C struct via the
 * `arcel_object_desc` adapter pattern.
 *
 * The same shape works for protobuf-c messages, libprotobuf C++
 * messages (via a thin C wrapper), capnproto, MessagePack, etc.
 * arcel itself stays protobuf-agnostic — the embedder owns the data
 * format and provides a 20-line dispatch table that arcel calls when
 * CEL code accesses the object's fields.
 *
 *     gcc -I.. embed_object.c ../libarcel.a -ldl -lm -o embed_object
 *
 * The example evaluates the K8s-admission-shape predicate
 *
 *     u.age >= 18 && u.country == "JP" && u.role in ["admin", "user"]
 *
 * against a `struct user_request` that lives entirely in caller-owned
 * memory.  Compare to embed.c, which round-trips through JSON.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arcel.h"

/* The embedder's native data structure. */
struct user_request {
    int64_t     age;
    const char *country;
    const char *role;
};

/* The dispatch table arcel will call into.  Three fields are exposed:
 * `age` (int), `country` (string), `role` (string).  Every other name
 * returns -1 (= missing field, surfaces as `no such key` at eval). */
static int
user_request_field(const arcel_object_desc *desc, const void *obj_,
                   const char *name, size_t name_len,
                   arcel_arena_handle *arena, arcel_value *out)
{
    (void)desc;
    (void)arena;   /* this adapter only returns scalars; lists / owned
                    * strings would use `arena` via arcel_value_list_new
                    * or arcel_value_string_copy */
    const struct user_request *const u = (const struct user_request *)obj_;
    if (name_len == 3 && memcmp(name, "age", 3) == 0) {
        *out = arcel_value_int(u->age);
        return 0;
    }
    if (name_len == 7 && memcmp(name, "country", 7) == 0) {
        *out = arcel_value_string(u->country, strlen(u->country));
        return 0;
    }
    if (name_len == 4 && memcmp(name, "role", 4) == 0) {
        *out = arcel_value_string(u->role, strlen(u->role));
        return 0;
    }
    return -1;  /* missing */
}

static const arcel_object_desc user_request_desc = {
    .field      = user_request_field,
    .type_name  = "UserRequest",
    /* .has and .format_json default to NULL — arcel falls back to
     * "field returned 0 = present" and `<object:UserRequest>` for
     * format_json output. */
};

static void
example(arcel_program *prg, arcel_activation *act,
        const char *what, int64_t age, const char *country, const char *role)
{
    struct user_request req = { age, country, role };
    arcel_activation_reset(act);
    arcel_activation_set_object(act, "u", &req, &user_request_desc);

    arcel_value r = arcel_eval(prg, act);
    char buf[64];
    arcel_format_json(r, buf, sizeof buf);
    printf("OK %-15s age=%lld country=%s role=%s -> %s\n",
           what, (long long)age, country, role, buf);
}

int
main(void)
{
    arcel_env *env = arcel_env_new();

    char err[256];
    arcel_program *prg = arcel_compile(env,
        "u.age >= 18 && u.country == \"JP\" && u.role in [\"admin\", \"user\"]",
        -1, err, sizeof err);
    if (!prg) { fprintf(stderr, "compile: %s\n", err); return 1; }

    arcel_activation *act = arcel_activation_new(env);

    example(prg, act, "matches",       25, "JP", "admin");
    example(prg, act, "wrong country", 25, "US", "admin");
    example(prg, act, "underage",      16, "JP", "user");
    example(prg, act, "guest role",    30, "JP", "guest");

    arcel_activation_free(act);
    arcel_program_free(prg);
    arcel_env_free(env);
    return 0;
}
