#ifndef ASCHEME_PARSE_H
#define ASCHEME_PARSE_H 1

#include "context.h"

/* `scm_read` is already declared in context.h for use as the
 * `(read)` primitive.  This header adds the bulk-reader entry that
 * the driver uses to load whole files into a forms list. */

/* Read every form in `src[0..len)` into a Scheme list.  Stops at EOF. */
VALUE scm_read_all_string(CTX *c, const char *src, size_t len);

#endif /* ASCHEME_PARSE_H */
