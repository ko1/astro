#include "context.h"
#include "node.h"

struct calc_option OPTION;

// code repository

static struct code_repo {
    uint32_t size;
    uint32_t capa;

    struct code_entry {
        const char *name;
        NODE *body;
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
code_repo_fnid(node_hash_t h)
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

void
code_repo_add(const char *name, NODE *body, bool _)
{
    if (body == NULL || code_repo_fnid(HASH(body))) {
        // ignore
    }
    else {
        struct code_entry *ce = code_repo_new_entry();
        ce->name = name;
        ce->body = body;
    }
}

// generate specialized code

static void
generate_sc_entry(FILE *fp, NODE *n, const char *name)
{
    if (n->head.hash_value) {
        fprintf(fp, "    { // %s\n", name);
        fprintf(fp, "     .hash = 0x%lxLL,\n", n->head.hash_value);
        fprintf(fp, "     .dispatcher_name = \"%s\",\n", n->head.dispatcher_name);
        fprintf(fp, "     .dispatcher      = %s,\n", n->head.dispatcher_name);
        fprintf(fp, "    },\n");
    }
    else {
        fprintf(stderr, "hash value is 0 for %s (%p)\n", name, n);
    }
}

static void
generate_specialized_code(NODE *n)
{
    fprintf(stderr, "START generating specialized code\n");

    FILE *fp = fopen("node_specialized.c", "w");

    if (fp == NULL) {
        perror("can't open for write");
        exit(1);
    }

    sc_repo_clear();

    // specialize main
    SPECIALIZE(fp, n);

    // specialize functions
    for (uint32_t i=0; i<code_repo.size; i++) {
        NODE *body = code_repo.entries[i].body;
        SPECIALIZE(fp, body);
    }

    fprintf(fp, "struct specialized_code sc_entries[] = {\n");

    if (n) generate_sc_entry(fp, n, "main");

    for (uint32_t i=0; i<code_repo.size; i++) {
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
    CTX *c = malloc(sizeof(CTX));

    // 1 + 2 * 3
    NODE *ast = ALLOC_node_add(ALLOC_node_num(1),
                               ALLOC_node_mul(
                                   ALLOC_node_num(2),
                                   ALLOC_node_num(3)));

    printf("result: %ld\n", EVAL(c, ast));
    generate_specialized_code(ast);
    return 0;
}
