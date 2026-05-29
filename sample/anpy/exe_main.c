// AnPy exe driver — linked only into `--build` executables.
#include <setjmp.h>
#include "context.h"
#include "node.h"

struct anpy_option OPTION;
extern NODE *astro_build_embedded_ast(void);
jmp_buf *anpy_get_jmp(void);
void anpy_set_jmp_active(int v);

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    OPTION.quiet = true;
    INIT();
    CTX *c = anpy_make_context();
    anpy_install_globals(c);
    NODE *ast = astro_build_embedded_ast();
    if (setjmp(*anpy_get_jmp()) == 0) { anpy_set_jmp_active(1); EVAL(c, ast); }
    anpy_set_jmp_active(0);
    return 0;
}
