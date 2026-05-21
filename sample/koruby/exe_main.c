/* koruby exe driver — linked only when koruby is built via
 * `--generate-executable`.  No prism, no parser, no Code Store dir.
 *
 * Steps:
 *   1. INIT() + korb_runtime_init()
 *   2. astro_cs_init (no disk store) + astro_cs_static_init (static SDs)
 *   3. Bootstrap eval (same bootstrap.rb as REPL koruby)
 *   4. Set up CTX
 *   5. Build embedded AST
 *   6. astro_cs_load on the root (binds dispatcher from static table)
 *   7. EVAL with REPL-equivalent error handling
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "context.h"
#include "object.h"
#include "node.h"
#include "../../runtime/astro_code_store.h"

struct koruby_option OPTION = {0};

/* Helpers from main.c (now non-static). */
extern CTX *koruby_setup_ctx(const char *current_file);
extern void koruby_eval_bootstrap(CTX *c);
extern int  koruby_run_ast(CTX *c, NODE *ast);

/* Generated. */
extern NODE *astro_build_embedded_ast(void);
extern struct astro_cs_static_entry astro_cs_static_table[];
extern size_t astro_cs_static_table_size;

int
main(int argc, char *argv[])
{
    INIT();
    korb_runtime_init();

    /* No disk-backed store; just bring framework state up + wire static
     * SDs from the linked-in table. */
    astro_cs_init(NULL, NULL, 0);
    astro_cs_static_init(astro_cs_static_table, astro_cs_static_table_size);

    /* ARGV / $0 / $PROGRAM_NAME — mirror the REPL koruby behaviour but
     * skip the per-script setup (the exe IS one specific script). */
    {
        VALUE argv_array = korb_ary_new();
        for (int i = 1; i < argc; i++) {
            korb_ary_push(argv_array, korb_str_new_cstr(argv[i]));
        }
        korb_const_set(korb_vm->object_class, korb_intern("ARGV"), argv_array);
        VALUE pn = korb_str_new_cstr(argv[0]);
        korb_gvar_set(korb_intern("$0"), pn);
        korb_gvar_set(korb_intern("$PROGRAM_NAME"), pn);
    }

    /* current_file = argv[0] so __FILE__ / require_relative resolve
     * relative to the exe's directory.  Resolve to absolute path so
     * downstream relative-path lookups work from any cwd. */
    static char abs_argv0[PATH_MAX];
    if (!realpath(argv[0], abs_argv0)) {
        snprintf(abs_argv0, sizeof(abs_argv0), "%s", argv[0]);
    }
    CTX *c = koruby_setup_ctx(abs_argv0);

    /* Bootstrap.  This adds methods to built-in classes (Enumerable
     * include targets, Comparable, Rational/Complex helpers, etc.) —
     * exactly the same as the REPL koruby's bootstrap. */
    koruby_eval_bootstrap(c);

    NODE *ast = astro_build_embedded_ast();
    (void)astro_cs_load(ast, NULL);   /* wire SD from static table */

    return koruby_run_ast(c, ast);
}
