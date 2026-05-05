#include <ctype.h>
#include "context.h"
#include "node.h"
#include "astro_code_store.h"

#ifdef USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

struct calc_option OPTION;

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

static NODE *
parse(const char *input)
{
    pos = input;
    return parse_expr();
}

static char *
read_line(const char *prompt)
{
#ifdef USE_READLINE
    char *line = readline(prompt);
    if (line && *line) add_history(line);
    return line;
#else
    printf("%s", prompt);
    fflush(stdout);
    static char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
#endif
}

static void
usage(const char *progname)
{
    fprintf(stderr,
        "Usage: %s [options] [-e EXPR]\n"
        "\n"
        "  -e EXPR        evaluate EXPR once and exit (no REPL)\n"
        "      --disasm   print x86 disassembly of the specialized code\n"
        "      --no-compile  skip code-store specialization (pure interpreter)\n"
        "  -q, --quiet    suppress hit/miss progress messages\n"
        "  -h, --help     show this help\n"
        "\n"
        "With no -e, %s starts an interactive REPL.\n",
        progname, progname);
}

static VALUE
evaluate(CTX *const c, const char *const input)
{
    NODE *const ast = parse(input);
    if (!OPTION.no_compiled_code) {
        if (!ast->head.flags.is_specialized) {
            astro_cs_compile(ast, NULL);
            astro_cs_build(NULL);
            astro_cs_reload();
            astro_cs_load(ast, NULL);
        }
        if (OPTION.disasm) astro_cs_disasm(ast);
    }
    return EVAL(c, ast);
}

int
main(int argc, char *argv[])
{
    const char *eval_expr = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            OPTION.quiet = true;
        }
        else if (strcmp(argv[i], "--no-compile") == 0) {
            OPTION.no_compiled_code = true;
        }
        else if (strcmp(argv[i], "--disasm") == 0) {
            OPTION.disasm = true;
        }
        else if (strcmp(argv[i], "-e") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "calc: -e requires an argument\n");
                return 1;
            }
            eval_expr = argv[i];
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        else {
            fprintf(stderr, "calc: unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    INIT();
    CTX *const c = malloc(sizeof(CTX));

    if (eval_expr) {
        printf("%ld\n", evaluate(c, eval_expr));
        return 0;
    }

    char *line;
    while ((line = read_line("calc> ")) != NULL) {
        if (line[0] != '\0') printf("=> %ld\n", evaluate(c, line));
#ifdef USE_READLINE
        free(line);
#endif
    }

    printf("\n");
    return 0;
}
