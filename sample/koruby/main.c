#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <limits.h>

#include "context.h"
#include "object.h"
#include "node.h"

struct koruby_option OPTION = {0};

NODE *koruby_parse(const char *src, size_t len, const char *filename);

extern void sc_repo_clear(void);

static char *read_all(FILE *fp, size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *buf = korb_xmalloc_atomic(cap);
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (len + 1 >= cap) { cap *= 2; buf = korb_xrealloc(buf, cap); }
        buf[len++] = (char)c;
    }
    buf[len] = 0;
    *out_len = len;
    return buf;
}

static void usage(void) {
    fprintf(stderr,
        "usage: koruby [options] [file]\n"
        "  -e <code>      eval code\n"
        "  --dump         dump AST\n"
        "  -c             compile only (no run, generate node_specialized.c)\n"
        "  -q             quiet\n"
        "  -v             verbose\n");
    exit(1);
}

static void
generate_specialized_code(NODE *ast)
{
    FILE *fp = fopen("node_specialized.c", "w");
    if (!fp) { perror("node_specialized.c"); return; }
    sc_repo_clear();

    /* main */
    SPECIALIZE(fp, ast);

    /* code repo entries (methods) */
    extern NODE *code_repo_find(node_hash_t);
    /* cheating: walk our internal repo via DUMP-friendly iteration not available;
       Instead, iterate entries directly through accessor below. */
    extern void koruby_specialize_repo(FILE *fp);
    koruby_specialize_repo(fp);

    fprintf(fp, "struct specialized_code sc_entries[] = {\n");
    /* main entry */
    if (ast && HASH(ast)) {
        fprintf(fp, "    { .hash = 0x%lxLL, .dispatcher_name = \"%s\", .dispatcher = %s },\n",
                (unsigned long)HASH(ast), ast->head.dispatcher_name, ast->head.dispatcher_name);
    }
    extern void koruby_emit_sc_entries(FILE *fp);
    koruby_emit_sc_entries(fp);
    fprintf(fp, "};\n#define NODE_SPECIALIZED_INCLUDED 1\n");
    fclose(fp);
}

int main(int argc, char *argv[])
{
    INIT();
    korb_runtime_init();

    const char *e_code = NULL;
    const char *file = NULL;
    int script_arg_start = argc;  /* args beyond the script are passed to ARGV */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-e") == 0 && i+1 < argc) { e_code = argv[++i]; }
        else if (strcmp(a, "--dump") == 0) { OPTION.dump_ast = true; }
        else if (strcmp(a, "-c") == 0) { OPTION.compile_only = true; }
        else if (strcmp(a, "-q") == 0) { OPTION.quiet = true; }
        else if (strcmp(a, "-v") == 0) { OPTION.verbose = true; }
        else if (a[0] == '-' && a[1] == '-' && file) { script_arg_start = i; break; }
        else if (a[0] == '-') {
            fprintf(stderr, "unknown option: %s\n", a);
            usage();
        }
        else if (!file) {
            file = a;
            script_arg_start = i + 1;
        }
        else {
            /* extra positional argument */
            script_arg_start = i;
            break;
        }
    }
    /* Build ARGV from args after the script path */
    VALUE argv_array = korb_ary_new();
    for (int i = script_arg_start; i < argc; i++) {
        korb_ary_push(argv_array, korb_str_new_cstr(argv[i]));
    }
    korb_const_set(korb_vm->object_class, korb_intern("ARGV"), argv_array);

    char *src = NULL;
    size_t len = 0;
    const char *filename = "(eval)";
    if (e_code) {
        src = (char *)e_code;
        len = strlen(e_code);
    }
    else if (file) {
        FILE *fp = fopen(file, "rb");
        if (!fp) { perror(file); exit(1); }
        src = read_all(fp, &len);
        fclose(fp);
        filename = file;
    }
    else usage();

    NODE *ast = koruby_parse(src, len, filename);
    if (OPTION.dump_ast) {
        DUMP(stdout, ast, true);
        printf("\n");
    }

    /* Set up a CTX. Stack is registered with GC by virtue of being a GC alloc. */
    CTX *c = korb_xcalloc(1, sizeof(CTX));
    /* The value stack is heap allocated so GC scans it.  Use 64K slots. */
    size_t stack_size = 4 * 1024 * 1024;
    c->stack_base = korb_xmalloc(stack_size * sizeof(VALUE));
    for (size_t i = 0; i < stack_size; i++) c->stack_base[i] = Qnil;
    c->stack_end  = c->stack_base + stack_size;
    c->fp = c->stack_base;
    c->sp = c->fp;
    c->self = korb_vm->main_obj;
    c->current_class = korb_vm->object_class;
    static struct korb_cref top_cref;
    top_cref.klass = korb_vm->object_class;
    top_cref.prev = NULL;
    c->cref = &top_cref;
    /* For -e mode, current_file = ./script.rb so require_relative resolves
     * against cwd. */
    static char ecwd[PATH_MAX] = {0};
    if (!file) {
        if (!getcwd(ecwd, sizeof(ecwd))) strcpy(ecwd, ".");
        strcat(ecwd, "/-e");
        c->current_file = ecwd;
    } else {
        c->current_file = file;
    }
    c->state = KORB_NORMAL;
    c->method_serial = korb_vm->method_serial;

    OPTIMIZE(ast);

    if (!OPTION.compile_only) {
        VALUE r = EVAL(c, ast);
        (void)r;
        if (c->state == KORB_RAISE) {
            VALUE s = korb_inspect(c->state_value);
            fprintf(stderr, "unhandled exception: %s\n", korb_str_cstr(s));
            return 1;
        }
    }

    if (OPTION.compile_only) {
        generate_specialized_code(ast);
    }
    return 0;
}

/* hooks for specialized-code generation */
struct code_repo {
    uint32_t size, capa;
    struct code_entry { const char *name; struct Node *body; } *entries;
};
extern struct code_repo code_repo;

void koruby_specialize_repo(FILE *fp) {
    extern void SPECIALIZE(FILE *, NODE *);
    for (uint32_t i = 0; i < code_repo.size; i++) {
        SPECIALIZE(fp, code_repo.entries[i].body);
    }
}

void koruby_emit_sc_entries(FILE *fp) {
    for (uint32_t i = 0; i < code_repo.size; i++) {
        NODE *b = code_repo.entries[i].body;
        if (HASH(b)) {
            fprintf(fp, "    { .hash = 0x%lxLL, .dispatcher_name = \"%s\", .dispatcher = %s },\n",
                    (unsigned long)HASH(b), b->head.dispatcher_name, b->head.dispatcher_name);
        }
    }
}
