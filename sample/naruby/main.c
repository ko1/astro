
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "node.h"
#include "astro_jit.h"

NODE *PARSE(int argc, char *argv[]);

struct naruby_option OPTION = {
    // .static_lang = true,
};
static CTX *global_c;

size_t node_cnt;

void sc_repo_clear(void);

// builtin functions

static void
define_func(CTX *c, const char *name, const char *func_name, builtin_func_ptr func, uint32_t params_cnt)
{
    struct function_entry *fe = &c->func_set[c->func_set_cnt++];
    struct builtin_func *bf = malloc(sizeof(struct builtin_func));
    bf->name = name;
    bf->func = func;
    bf->func_name = func_name;
    bf->have_src = true;

    fe->name = name;
    fe->body = ALLOC_node_call_builtin(bf, func, params_cnt);
    fe->params_cnt = params_cnt;
    fe->locals_cnt = params_cnt;

    code_repo_add(name, fe->body, true);
}

#define DEFINE_FUNC(c, name, func_name, arity) define_func(c, name, #func_name, (builtin_func_ptr)func_name, arity)

static void
define_builtin_functions(CTX *c)
{
    DEFINE_FUNC(c, "p", narb_p, 1);
    DEFINE_FUNC(c, "zero", narb_zero, 0);
    DEFINE_FUNC(c, "bf_add", narb_add, 2);
}

// code repository

static struct code_repo {
    uint32_t size;
    uint32_t capa;

    struct code_entry {
        const char *name;
        NODE *body;
        uint32_t params_cnt;
        bool skip_specialize;
    } *entries;
} code_repo;

static struct code_entry *
code_repo_new_entry(void)
{
    if (code_repo.size < code_repo.capa) {
        return &code_repo.entries[code_repo.size++];
    }
    else {
        uint32_t capa = code_repo.capa * 2;
        if (capa == 0) {
            capa = 8;
        }
        code_repo.entries = realloc(code_repo.entries, sizeof(struct code_entry) * capa);

        if (code_repo.entries) {
            code_repo.capa = capa;
            return code_repo_new_entry();
        }
        else {
            fprintf(stderr, "no memory for capa:%u\n", capa);
            exit(1);
        }
    }
}

NODE *
code_repo_find(node_hash_t h)
{
    if (h != 0) {
        for (uint32_t i=0; i<code_repo.size; i++) {
            NODE *n = code_repo.entries[i].body;
            if (HASH(n) == h) {
                return n;
            }
        }
    }

    return NULL;
}

NODE *
code_repo_find_by_name(const char *name)
{
    for (uint32_t i=0; i<code_repo.size; i++) {
        if (strcmp(code_repo.entries[i].name, name) == 0) {
            return code_repo.entries[i].body;
        }
    }

    return NULL;
}

void
code_repo_add(const char *name, NODE *body, bool force_add)
{
    bool found = code_repo_find(HASH(body)) != NULL;

    if (body == NULL || (!force_add && found)) {
        // ignore
    }
    else {
        struct code_entry *ce = code_repo_new_entry();
        ce->name = name;
        ce->body = body;
        ce->skip_specialize = found;
    }
}

// context management

static CTX *
create_context(int frames, int funcs)
{
    CTX *c = (CTX *)malloc(sizeof(CTX));
    c->env = c->fp = (VALUE *)malloc(sizeof(VALUE) * 10 * frames);
    c->func_set = malloc(sizeof(struct function_entry) * funcs); // supports 100 functions
    c->func_set_cnt = 0;
    c->serial = 1;

#if DEBUG_EVAL
    c->frame_cnt = 0;
    c->rec_cnt = 0;
#endif

    define_builtin_functions(c);
    return c;
}

// generate specialized code

static void
generate_sc_entry(FILE *fp, NODE *n, const char *name)
{
    node_hash_t hash_value = HASH(n);

    if (hash_value) {
        fprintf(stderr, "generate_sc_entry - name:%s func:%s\n", name, n->head.dispatcher_name);
        fprintf(fp, "    { // %s\n", name);
        fprintf(fp, "     .hash = 0x%lxLL,\n", hash_value);
        fprintf(fp, "     .dispatcher_name = \"%s\",\n", n->head.dispatcher_name);
        fprintf(fp, "     .dispatcher      = %s,\n", n->head.dispatcher_name);
        fprintf(fp, "    },\n");
    }
    else {
        fprintf(stderr, "generate_sc_entry - hash value is 0 for %s (%p)\n", name, n);
    }
}

static void
generate_specialized_code(NODE *n)
{
    // fprintf(stderr, "START generating specialized code\n");

    FILE *fp = fopen("node_specialized.c", "w");

    if (fp == NULL) {
        perror("can't open for write");
        exit(1);
    }

    sc_repo_clear();

    // specialize main
    SPECIALIZE(fp, n);

    // specialize code_repo
    for (uint32_t i=0; i<code_repo.size; i++) {
        if (code_repo.entries[i].skip_specialize) {
            printf("skip %s\n", code_repo.entries[i].name);
            continue;
        }
        NODE *body = code_repo.entries[i].body;
        SPECIALIZE(fp, body);
    }

    fprintf(fp, "struct specialized_code sc_entries[] = {\n");

    if (n) generate_sc_entry(fp, n, "main");

    for (uint32_t i=0; i<code_repo.size; i++) {
        if (code_repo.entries[i].skip_specialize) continue;
        NODE *body = code_repo.entries[i].body;
        generate_sc_entry(fp, body, code_repo.entries[i].name);
    }

    fprintf(fp, "};\n\n");
    fprintf(fp, "#define NODE_SPECIALIZED_INCLUDED 1\n");

    fclose(fp);
}

int
main(int argc, char *argv[])
{
    INIT();

    CTX *c = create_context(10000, 2000);
    global_c = c;
    NODE *ast = PARSE(argc, argv);

    if (0 && !OPTION.quiet) {
        DUMP(stdout, ast, true);
        printf("\n");
    }

    if (!OPTION.compile_only) {
        OPTIMIZE(ast);
        // printf("main dispatcher:%s (h:%lx)\n", ast->head.dispatcher_name, HASH(ast));
        VALUE r = EVAL(c, ast);
        printf("Result: %ld, node_cnt:%lu\n", r, node_cnt);
    }

    if (!OPTION.no_generate_specialized_code) {
        generate_specialized_code(ast);
    }

    return 0;
}
