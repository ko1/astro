// naruby exe driver — linked only when naruby is built via
// `--generate-executable`.  No prism, no parser, no Code Store dir.
//
// Dispatchers in the embedded AST are pre-bound to SD_<hash> by the
// framework — no runtime cs_load step needed.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "node.h"
#include "context.h"

struct naruby_option OPTION = { 0 };

size_t node_cnt;

extern CTX *global_c;
extern CTX *create_context(int frames, int funcs);
extern void define_builtin_functions(CTX *c);

extern NODE *astro_build_embedded_ast(void);

// JIT stubs — exe builds carry no live JIT.
void astro_jit_submit_query(NODE *n)   { (void)n; }
void astro_jit_submit_compile(NODE *n) { (void)n; }

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    CTX *c = create_context(10000, 2000);
    global_c = c;

    NODE *ast = astro_build_embedded_ast();
    RESULT r = EVAL(c, ast, c->env);
    (void)r;
    return 0;
}
