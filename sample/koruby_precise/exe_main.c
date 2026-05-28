/* koruby exe driver — linked only when koruby is built via
 * `--generate-executable`.  Dispatchers in the embedded AST are
 * pre-bound to SD_<hash> by the framework, so no runtime cs_load
 * step is needed.
 *
 * Steps:
 *   1. INIT() + korb_runtime_init() — basic VM bring-up
 *   2. Set ARGV / $0 / $PROGRAM_NAME
 *   3. koruby_setup_ctx + bootstrap eval
 *   4. astro_build_embedded_ast() rebuilds the program AST with
 *      dispatchers already pointing at SD_<hash>
 *   5. koruby_run_ast with CRuby-style exception handling
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "context.h"
#include "object.h"
#include "node.h"

struct koruby_option OPTION = { 0 };

extern CTX *koruby_setup_ctx(const char *current_file);
extern void koruby_eval_bootstrap(CTX *c);
extern int  koruby_run_ast(CTX *c, NODE *ast);

extern NODE *astro_build_embedded_ast(void);

int
main(int argc, char *argv[])
{
    INIT();
    korb_runtime_init();

    /* ARGV / $0 / $PROGRAM_NAME. */
    {
        VALUE argv_array = korb_ary_new();
        for (int i = 1; i < argc; i++) {
            korb_ary_push(argv_array, korb_str_new_cstr(c, c->sp, argv[i]));
        }
        korb_const_set(korb_vm->object_class, korb_intern("ARGV"), argv_array);
        VALUE pn = korb_str_new_cstr(c, c->sp, argv[0]);
        korb_gvar_set(korb_intern("$0"), pn);
        korb_gvar_set(korb_intern("$PROGRAM_NAME"), pn);
    }

    /* Resolve argv[0] for __FILE__ / require_relative. */
    static char abs_argv0[PATH_MAX];
    if (!realpath(argv[0], abs_argv0)) {
        snprintf(abs_argv0, sizeof(abs_argv0), "%s", argv[0]);
    }
    CTX *c = koruby_setup_ctx(abs_argv0);

    koruby_eval_bootstrap(c);

    NODE *ast = astro_build_embedded_ast();
    return koruby_run_ast(c, ast);
}
