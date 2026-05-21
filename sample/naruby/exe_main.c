// naruby exe driver — linked only when naruby is built via
// `--generate-executable`.  No prism, no parser, no Code Store dir.
//
// Steps at startup:
//   1. astro_cs_init (NULL store — no disk-backed dlopen)
//   2. astro_cs_static_init  → wires SD_<hash> → function pointers
//   3. create_context        → CTX + builtin registration
//   4. astro_build_embedded_ast() — rebuild the AST from C
//   5. astro_cs_load on the root  — bind dispatcher via static table
//   6. EVAL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "node.h"
#include "context.h"
#include "astro_code_store.h"

struct naruby_option OPTION = {
    // exe is always "plain enough": no parsing, no on-disk code store
    // touches.  Static SD table is wired separately below.
};

size_t node_cnt;

extern CTX *global_c;
extern CTX *create_context(int frames, int funcs);
extern void define_builtin_functions(CTX *c);

// From _embed.c (generated at exe-build time).
extern NODE *astro_build_embedded_ast(void);

// From _static_table.c (generated at exe-build time).
extern struct astro_cs_static_entry astro_cs_static_table[];
extern size_t astro_cs_static_table_size;

// JIT stubs (naruby's JIT path is build-time-only; exe has no JIT).
void astro_jit_submit_query(NODE *n) { (void)n; }
void astro_jit_submit_compile(NODE *n) { (void)n; }

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    astro_cs_init(NULL, NULL, 0);
    astro_cs_static_init(astro_cs_static_table, astro_cs_static_table_size);

    CTX *c = create_context(10000, 2000);
    global_c = c;

    NODE *ast = astro_build_embedded_ast();
    (void)astro_cs_load(ast, NULL);

    RESULT r = EVAL(c, ast, c->env);
    (void)r;
    return 0;
}
