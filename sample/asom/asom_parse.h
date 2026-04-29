#ifndef ASOM_PARSE_H
#define ASOM_PARSE_H 1

#include "context.h"
#include "node.h"

typedef struct ParsedMethod_struct {
    const char *selector;
    uint32_t num_params;
    uint32_t num_locals;
    NODE *body;
    bool is_primitive;
} ASOM_PARSED_METHOD;

typedef struct ParsedClass_struct {
    const char *name;
    const char *superclass_name;          // NULL = default to Object

    const char **fields;                  // instance-side fields
    uint32_t fields_cnt;
    const char **class_fields;            // class-side fields
    uint32_t class_fields_cnt;

    ASOM_PARSED_METHOD **methods;
    uint32_t methods_cnt, methods_cap;
    ASOM_PARSED_METHOD **class_methods;
    uint32_t class_methods_cnt, class_methods_cap;
} ASOM_PARSED_CLASS;

ASOM_PARSED_CLASS *asom_parse_class_str (CTX *c, const char *src,  const char *file);
ASOM_PARSED_CLASS *asom_parse_class_file(CTX *c, const char *path);

#endif // ASOM_PARSE_H
