// calc exe driver — linked only when calc is built via `--generate-executable`.
//
// The standalone exe has no REPL, no parser invocation, no Code Store
// machinery: just reconstruct the embedded AST (dispatchers already
// baked to SD_<hash> by the framework) and evaluate.

#include "context.h"
#include "node.h"

struct calc_option OPTION;

extern NODE *astro_build_embedded_ast(void);

int
main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    OPTION.quiet = true;
    CTX *c = malloc(sizeof(CTX));
    printf("%ld\n", EVAL(c, astro_build_embedded_ast()));
    return 0;
}
