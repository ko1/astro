// abc exe driver — linked only into executables produced by `--build`.
//
// The embedded AST already has its dispatchers baked to the specialized
// SD_<hash> functions, so there is no parser / Code Store machinery here:
// just rebuild the context and evaluate the program.
#include <setjmp.h>
#include "context.h"
#include "node.h"

struct abc_option OPTION;

extern NODE *astro_build_embedded_ast(void);
jmp_buf *bc_get_jmp(void);
void bc_set_jmp_active(int v);

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    OPTION.quiet = true;
    INIT();
    CTX *c = bc_make_context();
    NODE *ast = astro_build_embedded_ast();
    if (setjmp(*bc_get_jmp()) == 0) {
        bc_set_jmp_active(1);
        EVAL(c, ast);
    }
    bc_set_jmp_active(0);
    return 0;
}
