#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "node.h"
#include "context.h"

NODE *PARSE_FILE(CTX *c, const char *path);
extern uint32_t JSTRO_TOP_NLOCALS;
void jstro_install_stdlib(CTX *c);

struct jstro_option OPTION = {0};
extern unsigned long jstro_shape_find_count;
extern unsigned long jstro_object_set_count;
extern unsigned long jstro_call_ic_miss;
static bool g_dump_ic = false;

int
main(int argc, char *argv[])
{
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) OPTION.quiet = true;
        else if (strcmp(argv[i], "-v") == 0) OPTION.verbose = true;
        else if (strcmp(argv[i], "--show-result") == 0) OPTION.show_result = true;
        else if (strcmp(argv[i], "--no-compile") == 0) OPTION.no_compiled_code = true;
        else if (strcmp(argv[i], "--dump") == 0) OPTION.dump_ast = true;
        else if (strcmp(argv[i], "--dump-ic") == 0) g_dump_ic = true;
        else if (argv[i][0] != '-') path = argv[i];
    }
    if (!path) { fprintf(stderr, "usage: %s [options] file.js\n", argv[0]); return 1; }

    INIT();

    CTX *c = js_create_context();
    js_init_globals(c);
    jstro_install_stdlib(c);

    NODE *body = PARSE_FILE(c, path);
    if (!body) { fprintf(stderr, "parse failed\n"); return 1; }

    if (OPTION.dump_ast) {
        DUMP(stderr, body, true);
        fputc('\n', stderr);
    }

    JsValue *frame = (JsValue *)calloc(JSTRO_TOP_NLOCALS + 16, sizeof(JsValue));

    JsValue r = EVAL(c, body, frame);
    if (JSTRO_BR == JS_BR_THROW) {
        fprintf(stderr, "Uncaught: ");
        js_print_value(c, stderr, JSTRO_BR_VAL);
        fputc('\n', stderr);
        return 1;
    }
    if (OPTION.show_result) {
        js_print_value(c, stdout, r);
        fputc('\n', stdout);
    }
    if (g_dump_ic) {
        fprintf(stderr, "[IC] js_shape_find_slot: %lu\n", jstro_shape_find_count);
        fprintf(stderr, "[IC] js_object_set:      %lu\n", jstro_object_set_count);
        fprintf(stderr, "[IC] call_ic_miss:       %lu\n", jstro_call_ic_miss);
    }
    return 0;
}
