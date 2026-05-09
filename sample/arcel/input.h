#ifndef ARCEL_INPUT_H
#define ARCEL_INPUT_H

#include "context.h"

/* Parse a JSON string into a VALUE living in `arena`.  On parse
 * error, returns AC_ERR with a stable error message in the arena.
 *
 * Mapping to CEL types:
 *   true / false → bool
 *   null         → null
 *   integers     → int  (always int — CEL has no implicit uint
 *                  promotion from JSON; pin specific bindings as uint
 *                  via -i if needed)
 *   numbers w/ . or exp → double
 *   strings      → string
 *   arrays       → list
 *   objects      → map (keys are strings)
 */
VALUE arcel_parse_json(arcel_arena *arena, const char *src, uint32_t len);

#endif
