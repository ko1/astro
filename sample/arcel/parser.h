#ifndef ARCEL_PARSER_H
#define ARCEL_PARSER_H

#include "context.h"
#include "node.h"

/* Parse a CEL source expression into an AST.
 *
 * `src` is `len` bytes long; pass through embedded NULs (CEL source
 * can carry binary, e.g. `b'\x00'` in bytes literals).  Use
 * arcel_parse() for NUL-terminated input.
 *
 * On success: returns the root NODE, allocated through ALLOC_node_*
 * (heap, never freed — `arcel` is short-lived).
 *
 * On failure: returns NULL and writes a human-readable diagnostic
 * (with `<input>:line:col` prefix) into `*out_err` (a stable string
 * valid until the next parse call). */
NODE *arcel_parse_n(const char *src, uint32_t len, const char **out_err);
NODE *arcel_parse  (const char *src,                const char **out_err);

/* Variadic-children side array.  Used by node_list, node_map, and
 * (later) node_call.  Grows append-only; per-parse children are
 * pushed contiguously so a node only needs (idx, cnt) to find them.
 *
 * Exposed for the ASTroGen-generated allocator helpers and the eval
 * functions in node.def. */
extern NODE   **arcel_node_arr;
extern uint32_t arcel_node_arr_cap;
extern uint32_t arcel_node_arr_top;

uint32_t arcel_node_arr_push_n(NODE *const *nodes, uint32_t n);

/* Rewind the side array — invalidates every (idx, cnt) pair handed
 * out before this call.  Caller MUST be done with all ASTs that
 * depend on those entries (i.e. node_list / node_map / node_call
 * children won't be evaluated again).
 *
 * In repl mode we call this after each eval so the array doesn't
 * accumulate one entry per literal element per envelope.  In bench
 * mode we never call it — the AST is parsed once and reused across
 * the whole iter loop. */
void arcel_node_arr_reset(void);

/* Side table of pre-evaluated, constant list literals.  When the
 * parser sees `[a, b, c]` and every element is a pure literal node
 * (int / uint / double / bool / null / string / bytes), it builds
 * the arcel_list once at parse time and parks it here, then emits
 * `node_const_list(idx)` instead of `node_list(arr_idx, cnt)` —
 * skipping the per-eval arena alloc + per-element re-evaluation that
 * `role in ["admin", "user"]` style policies pay every request.
 *
 * Same lifetime semantics as arcel_node_arr: bench keeps it forever
 * (parse 1×), repl resets via arcel_const_list_reset() each loop. */
extern struct arcel_list **arcel_const_list_arr;
extern uint32_t            arcel_const_list_cap;
extern uint32_t            arcel_const_list_top;

uint32_t arcel_const_list_push(struct arcel_list *l);
void     arcel_const_list_reset(void);

#endif
