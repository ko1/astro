#ifndef PARSE_H
#define PARSE_H 1

#include "node.h"

// A parsed program is a flat list of top-level statements.  Keeping them
// flat (rather than one big seq node) lets the driver evaluate each under
// its own runtime-error guard, the way bc recovers per top-level line.
typedef struct {
    NODE **stmts;
    int count;
} Program;

// Parse `src` into top-level statements, registering any `define`d
// functions into `c`.  On a syntax error, prints a message and returns a
// program with count < 0.
Program parse_program(CTX *c, const char *src);

// Build a single seq-chain root from a program (used for AOT compile /
// executable embedding).
NODE *program_to_root(const Program *p);

#endif // PARSE_H
