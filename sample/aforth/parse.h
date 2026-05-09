#ifndef PARSE_H
#define PARSE_H 1

#include "node.h"

/* Tokenize + parse a Forth source file into a single NODE tree.
 * Side effects: populates `aforth_word_table[]` for every : word,
 * advances `aforth_vars_used_top` for VARIABLE / CREATE / ALLOT. */
NODE *aforth_parse_file(const char *path);

/* Parse-time count of var slots needed by the program.  Read by
 * aforth_ctx_new() to size CTX::vars_used.  Defined in parse.c. */
extern uint32_t aforth_vars_used_top;

#endif // PARSE_H
