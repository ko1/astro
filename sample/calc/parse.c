#include <ctype.h>
#include "context.h"
#include "node.h"
#include "parse.h"

// --- Simple recursive descent parser for: expr = term (('+') term)*
//     term = factor (('*') factor)*
//     factor = NUMBER | '(' expr ')'

static const char *pos;

static void skip_spaces(void) { while (isspace(*pos)) pos++; }

static NODE *parse_expr(void);

static NODE *
parse_factor(void)
{
    skip_spaces();
    if (*pos == '(') {
        pos++;
        NODE *n = parse_expr();
        skip_spaces();
        if (*pos == ')') pos++;
        return n;
    }
    // number (possibly negative)
    int sign = 1;
    if (*pos == '-') { sign = -1; pos++; }
    int32_t num = 0;
    while (isdigit(*pos)) {
        num = num * 10 + (*pos - '0');
        pos++;
    }
    return ALLOC_node_num(sign * num);
}

static NODE *
parse_term(void)
{
    NODE *left = parse_factor();
    for (;;) {
        skip_spaces();
        if (*pos == '*') { pos++; left = ALLOC_node_mul(left, parse_factor()); }
        else if (*pos == '/') { pos++; left = ALLOC_node_div(left, parse_factor()); }
        else if (*pos == '%') { pos++; left = ALLOC_node_mod(left, parse_factor()); }
        else break;
    }
    return left;
}

static NODE *
parse_expr(void)
{
    NODE *left = parse_term();
    for (;;) {
        skip_spaces();
        if (*pos == '+') { pos++; left = ALLOC_node_add(left, parse_term()); }
        else if (*pos == '-') { pos++; left = ALLOC_node_sub(left, parse_term()); }
        else break;
    }
    return left;
}

NODE *
parse(const char *input)
{
    pos = input;
    return parse_expr();
}
