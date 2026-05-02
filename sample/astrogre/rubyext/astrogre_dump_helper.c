/*
 * Tiny helper that bridges to astrogre's DUMP() macro for the Cext's
 * Pattern#dump method.  Lives in its own translation unit so the
 * astrogre headers (which typedef their own VALUE) don't have to be
 * pulled into astrogre_ext.c (which uses Ruby's VALUE).
 */

#define _GNU_SOURCE 1

#include <stdio.h>
#include "node.h"
#include "context.h"
#include "parse.h"

/* `OPTION` is normally defined by main.c (the grep CLI driver).  When
 * building the Ruby extension we don't link main.c, so we define a
 * zero-initialized stub here so the engine sees the same symbol with
 * default values (`cs_verbose=false`, `no_compiled_code=false`, etc.). */
struct astrogre_option OPTION = {0};

void
astrogre_ext_dump_root(astrogre_pattern *p, FILE *fp)
{
    if (p && p->root) DUMP(fp, p->root, true);
}
