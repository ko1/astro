// Minimal driver for `ancaml --build OUT ...` executables.  The framework has
// baked the dispatcher pointers into the embedded AST, so no astro_cs_*
// calls are needed here — just rebuild the runtime context and evaluate.
#include "context.h"
#include "node.h"

struct ancaml_option OPTION;
extern NODE *astro_build_embedded_ast(void);

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    ac_register_externals();
    CTX *c = ac_make_context();
    if (setjmp(c->err_jmp) == 0) {
        c->err_active = 1;
        EVAL(c, astro_build_embedded_ast());
    }
    c->err_active = 0;
    fflush(stdout);
    return 0;
}
