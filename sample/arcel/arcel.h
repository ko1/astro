/* arcel — embedding API for the CEL evaluator.
 *
 * This header is the stable public surface for using arcel from C or
 * C++ programs (Envoy filter, gRPC interceptor, custom policy
 * evaluator, etc.).  The CLI in main.c is a thin wrapper around
 * exactly these calls.
 *
 *   #include "arcel.h"
 *
 *   arcel_env *env = arcel_env_new();
 *   char err[256];
 *   arcel_program *prg = arcel_compile(env, "u.age >= 18", -1, err, sizeof err);
 *   if (!prg) { ... err ... }
 *
 *   arcel_activation *act = arcel_activation_new();
 *   arcel_value u = arcel_value_map(act);
 *   arcel_value_map_set_int(u, "age", 25);
 *   arcel_activation_set(act, "u", u);
 *
 *   arcel_value r = arcel_eval(prg, act);
 *   if (arcel_type_of(r) == ARCEL_T_BOOL && arcel_get_bool(r)) ...
 *
 *   arcel_activation_free(act);
 *   arcel_program_free(prg);
 *   arcel_env_free(env);
 *
 * Threading
 *   arcel_env, arcel_program, arcel_activation are NOT thread-safe.
 *   Use one set per thread, or guard with a mutex.  Programs ARE
 *   safe to compile once and clone the activation per request.
 *
 * Lifetimes
 *   arcel_value handles returned by arcel_eval are valid until the
 *   next arcel_eval call on the same program (the underlying arena
 *   is reset between evals).  Copy any value you need to outlive the
 *   eval via arcel_format_json or by extracting scalars.
 *
 * Error handling
 *   Compile errors: arcel_compile returns NULL and writes to err_buf.
 *   Eval errors: arcel_eval returns a value with type ARCEL_T_ERR;
 *   call arcel_get_error to retrieve the message.  arcel never
 *   throws or longjmp's.
 */

#ifndef ARCEL_H
#define ARCEL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- value type tag (matches the internal arcel_tag) ------------- */

typedef enum {
    ARCEL_T_ERR    = 0,
    ARCEL_T_NULL,
    ARCEL_T_BOOL,
    ARCEL_T_INT,
    ARCEL_T_UINT,
    ARCEL_T_DOUBLE,
    ARCEL_T_STRING,
    ARCEL_T_BYTES,
    ARCEL_T_LIST,
    ARCEL_T_MAP,
} arcel_type;

/* arcel_value is a value handle.  Internally a 16-byte tagged union
 * passed by value (returned in 2 registers per SysV ABI), so callers
 * pass it around without indirection.  Treat the contents as opaque
 * and use the accessors below.
 *
 * The struct is exposed (rather than a heap-allocated handle) so that
 * the value passes through the C API at the same cost as it does
 * internally — there is no marshalling layer between embedder and
 * evaluator. */
/* 24 bytes — matches the internal VALUE layout (4-byte tag + 4 pad
 * + 16 payload union, where the largest union member is the
 * `{const char *, uint32_t}` string descriptor padded to its 8-byte
 * alignment).  A static_assert in arcel_lib.c keeps these in sync.
 *
 * Returned by value through the API; on x86_64 SysV ABI a 24-byte
 * struct passes through 3 registers (rax/rdx/rcx) instead of via
 * sret + memory, so the call cost is the same as the internal
 * dispatcher cost. */
typedef struct {
    uint64_t _opaque[3];
} arcel_value;

/* ---- opaque types ------------------------------------------------ */

typedef struct arcel_env_struct        arcel_env;
typedef struct arcel_program_struct    arcel_program;
typedef struct arcel_activation_struct arcel_activation;

/* ---- env --------------------------------------------------------- */

/* Create an env with default options (AOT specialization on).  The
 * env owns no per-eval state; programs and activations bind to it
 * for shared config (e.g. code-store path).  Free with arcel_env_free. */
arcel_env *arcel_env_new(void);
void       arcel_env_free(arcel_env *env);

/* Disable AOT specialization for this env.  Programs compiled
 * afterwards will run as plain interpreter (matches `--no-compile`
 * on the CLI).  Useful in test environments where the AOT
 * `astro_cs_build` make step is unwanted. */
void       arcel_env_set_no_compile(arcel_env *env, bool no_compile);

/* ---- compile ----------------------------------------------------- */

/* Parse + plan + AOT-specialize `src` (length `src_len`, or -1 for
 * NUL-terminated).  Returns a program handle, or NULL on parse /
 * plan error — in which case `err_buf` is filled with a diagnostic.
 *
 * Programs are not thread-safe to evaluate, but they are safe to
 * compile once and use across threads if each thread has its own
 * activation. */
arcel_program *arcel_compile(arcel_env *env,
                             const char *src, ptrdiff_t src_len,
                             char *err_buf, size_t err_buf_cap);

void           arcel_program_free(arcel_program *prg);

/* ---- activation (input bindings) -------------------------------- */

/* Create an empty activation.  Bindings are added via arcel_activation_set*
 * helpers; the same activation can be re-used across many evals on
 * the same program (e.g. one activation per request). */
arcel_activation *arcel_activation_new(arcel_env *env);
void              arcel_activation_free(arcel_activation *act);

/* Reset the activation to empty so it can be reused for a fresh
 * request without per-request malloc. */
void              arcel_activation_reset(arcel_activation *act);

/* Scalar setters — name is borrowed (caller keeps the storage alive
 * for the lifetime of the activation; usually a string literal).
 * String setter takes (ptr, len) so binary content is supported. */
void arcel_activation_set_null  (arcel_activation *act, const char *name);
void arcel_activation_set_bool  (arcel_activation *act, const char *name, bool v);
void arcel_activation_set_int   (arcel_activation *act, const char *name, int64_t v);
void arcel_activation_set_uint  (arcel_activation *act, const char *name, uint64_t v);
void arcel_activation_set_double(arcel_activation *act, const char *name, double v);
void arcel_activation_set_string(arcel_activation *act, const char *name, const char *s, size_t len);
void arcel_activation_set_bytes (arcel_activation *act, const char *name, const char *s, size_t len);

/* Container setters: pass a JSON snippet for now (cheap to use, slow
 * for large objects).  A native `arcel_value`-tree builder API will
 * follow once we have callers that demand it. */
void arcel_activation_set_json  (arcel_activation *act, const char *name, const char *json, size_t len);

/* Bulk: parse a JSON object and use each top-level key as a binding
 * (same semantics as the CLI's `-i` flag). */
int  arcel_activation_load_json (arcel_activation *act, const char *json, size_t len,
                                 char *err_buf, size_t err_buf_cap);

/* ---- eval ------------------------------------------------------- */

/* Evaluate `prg` against `act`, return the result value.  The
 * returned value's payload (string contents, list/map storage) lives
 * in the program's per-eval arena and is valid only until the next
 * arcel_eval call on this program. */
arcel_value arcel_eval(arcel_program *prg, arcel_activation *act);

/* ---- value inspection ------------------------------------------ */

arcel_type   arcel_type_of   (arcel_value v);

/* Scalar accessors.  Behaviour is undefined if the type doesn't
 * match (cheap — no runtime check); use arcel_type_of first if you
 * don't already know the type. */
bool         arcel_get_bool  (arcel_value v);
int64_t      arcel_get_int   (arcel_value v);
uint64_t     arcel_get_uint  (arcel_value v);
double       arcel_get_double(arcel_value v);
const char  *arcel_get_string(arcel_value v, size_t *out_len);  /* also for bytes */
const char  *arcel_get_error (arcel_value v);

/* Container introspection. */
uint32_t     arcel_list_len  (arcel_value v);
arcel_value  arcel_list_at   (arcel_value v, uint32_t i);
uint32_t     arcel_map_len   (arcel_value v);
arcel_value  arcel_map_key_at(arcel_value v, uint32_t i);
arcel_value  arcel_map_val_at(arcel_value v, uint32_t i);

/* Render a value as JSON (matches the CLI's eval output).  Writes up
 * to buf_cap bytes including a NUL terminator; returns the number of
 * bytes that *would* have been written if there were room (snprintf
 * convention). */
size_t       arcel_format_json(arcel_value v, char *buf, size_t buf_cap);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ARCEL_H */
