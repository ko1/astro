// calc exe driver — linked only when calc is built via `--generate-executable`.
//
// The standalone exe has no REPL, no parser invocation, no Code Store
// directory.  It just:
//   1. initialises framework state (no disk-backed dlopen),
//   2. wires the static SD table (compile-time-resolved dispatchers),
//   3. reconstructs the embedded AST,
//   4. binds dispatchers via astro_cs_load (static table hit),
//   5. evaluates and prints.

#include "context.h"
#include "node.h"
#include "astro_code_store.h"

struct calc_option OPTION;

// From _embed.c (generated at exe-build time).
extern NODE *astro_build_embedded_ast(void);

// From _static_table.c (generated at exe-build time).
extern struct astro_cs_static_entry astro_cs_static_table[];
extern size_t astro_cs_static_table_size;

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    // Don't print "hit/miss" for every node during AST reconstruction;
    // the static table is the source of truth.
    OPTION.quiet = true;

    // No disk-backed code store; just bring the framework up.
    astro_cs_init(NULL, NULL, 0);
    astro_cs_static_init(astro_cs_static_table, astro_cs_static_table_size);

    NODE *ast = astro_build_embedded_ast();
    (void)astro_cs_load(ast, NULL);  // wires SD from static table if present

    CTX *c = malloc(sizeof(CTX));
    VALUE r = EVAL(c, ast);
    printf("%ld\n", r);
    return 0;
}
