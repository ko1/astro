// asom: class loader. Walks OPTION.classpath looking for <name>.som,
// parses it via asom_parse_class_file, and installs the resulting class
// (with method table) into the global namespace.
//
// Phase 1 keeps things simple: there is no metaclass-side wiring and the
// Smalltalk standard library is not auto-loaded. Each class is loaded the
// first time it is referenced; cycles are broken by caching the
// asom_class skeleton before parsing methods.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "context.h"
#include "node.h"
#include "asom_runtime.h"
#include "asom_parse.h"

extern void asom_install_primitives(CTX *c);

static struct asom_method *
make_ast_method(struct asom_class *holder, ASOM_PARSED_METHOD *pm)
{
    struct asom_method *m = calloc(1, sizeof(*m));
    m->selector = pm->selector;
    m->num_params = pm->num_params;
    m->num_locals = pm->num_locals;
    m->body = pm->body;
    m->primitive = NULL;
    m->holder = holder;
    return m;
}

static char *
find_in_classpath(const char *name, const char *classpath)
{
    if (!classpath || !*classpath) classpath = ".";
    char *cp = strdup(classpath);
    char path[1024];
    char *save = NULL;
    for (char *dir = strtok_r(cp, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        snprintf(path, sizeof(path), "%s/%s.som", dir, name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            free(cp);
            return strdup(path);
        }
    }
    free(cp);
    return NULL;
}

struct asom_class *asom_load_class_impl(CTX *c, const char *name);

// Forward decl: non-loading global lookup defined in asom_runtime.c.
extern VALUE asom_global_lookup_for_loader(const char *interned_name);

// Merge AST methods from a parsed class into a bootstrap class. We keep
// primitives that were already installed (typed in C) and overlay any
// non-primitive methods from the .som file. Used so that loading e.g.
// `Array.som` adds Smalltalk-level helpers (`sum`, `average`, `join:`,
// `copyFrom:to:`...) on top of the primitive at:/at:put:/length core.
static void
asom_merge_into(struct asom_class *cls, ASOM_PARSED_CLASS *pc)
{
    // Policy: install AST methods only when there's no existing C primitive
    // for that selector. C primitives are typically the canonical (and
    // faster) implementation; the stdlib's Smalltalk-side helpers fill in
    // the long tail (`sum`, `average`, `join:`, `whileFalse:`...).
    char label_buf[256];
    for (uint32_t i = 0; i < pc->methods_cnt; i++) {
        ASOM_PARSED_METHOD *pm = pc->methods[i];
        if (pm->is_primitive) continue;
        struct asom_method *existing = asom_class_lookup(cls, pm->selector);
        if (existing && existing->primitive) continue;
        struct asom_method *m = calloc(1, sizeof(*m));
        m->selector   = pm->selector;
        m->num_params = pm->num_params;
        m->num_locals = pm->num_locals;
        m->body       = pm->body;
        m->holder     = cls;
        asom_class_define_method(cls, m);
        if (m->body) {
            snprintf(label_buf, sizeof(label_buf), "%s>>%s", cls->name, m->selector);
            asom_register_entry(m->body, asom_intern_cstr(label_buf), cls->name);
        }
    }
    if (cls->metaclass) {
        for (uint32_t i = 0; i < pc->class_methods_cnt; i++) {
            ASOM_PARSED_METHOD *pm = pc->class_methods[i];
            if (pm->is_primitive) continue;
            struct asom_method *existing = asom_class_lookup(cls->metaclass, pm->selector);
            if (existing && existing->primitive) continue;
            struct asom_method *m = calloc(1, sizeof(*m));
            m->selector   = pm->selector;
            m->num_params = pm->num_params;
            m->num_locals = pm->num_locals;
            m->body       = pm->body;
            m->holder     = cls->metaclass;
            asom_class_define_method(cls->metaclass, m);
            if (m->body) {
                snprintf(label_buf, sizeof(label_buf), "%s class>>%s", cls->name, m->selector);
                asom_register_entry(m->body, asom_intern_cstr(label_buf), cls->name);
            }
        }
    }
}

// Merge the .som file matching `name` (if any on the classpath) into the
// already-bootstrapped class `cls`. Idempotent: tracks a per-process set
// of names already merged.
static const char *g_merged[256];
static uint32_t    g_merged_cnt;

static void
asom_try_merge(CTX *c, struct asom_class *cls, const char *name)
{
    name = asom_intern_cstr(name);
    for (uint32_t i = 0; i < g_merged_cnt; i++) {
        if (g_merged[i] == name) return;
    }
    if (g_merged_cnt < 256) g_merged[g_merged_cnt++] = name;
    char *path = find_in_classpath(name, OPTION.classpath);
    if (!path) return;
    ASOM_PARSED_CLASS *pc = asom_parse_class_file(c, path);
    free(path);
    if (pc) asom_merge_into(cls, pc);
}

void
asom_merge_stdlib(CTX *c)
{
    asom_try_merge(c, c->cls_object,  "Object");
    asom_try_merge(c, c->cls_integer, "Integer");
    asom_try_merge(c, c->cls_double,  "Double");
    asom_try_merge(c, c->cls_string,  "String");
    asom_try_merge(c, c->cls_symbol,  "Symbol");
    asom_try_merge(c, c->cls_array,   "Array");
    asom_try_merge(c, c->cls_block,   "Block");
    asom_try_merge(c, c->cls_true,    "True");
    asom_try_merge(c, c->cls_false,   "False");
    asom_try_merge(c, c->cls_nil,     "Nil");
    asom_try_merge(c, c->cls_class,   "Class");
    asom_try_merge(c, c->cls_system,  "System");
}

struct asom_class *
asom_load_class_impl(CTX *c, const char *name)
{
    name = asom_intern_cstr(name);
    VALUE existing = asom_global_lookup_for_loader(name);
    if (existing && existing != c->val_nil) {
        return (struct asom_class *)ASOM_VAL2OBJ(existing);
    }

    char *path = find_in_classpath(name, OPTION.classpath);
    if (!path) return NULL; // missing-file is signalled to caller as NULL

    ASOM_PARSED_CLASS *pc = asom_parse_class_file(c, path);
    free(path);
    if (!pc) return NULL;

    struct asom_class *super = NULL;
    if (pc->superclass_name) {
        if (strcmp(pc->superclass_name, "nil") == 0) super = NULL;
        else super = asom_load_class_impl(c, pc->superclass_name);
    } else {
        super = c->cls_object;
    }

    // Build a per-class metaclass so class-side methods (TowersDisk new:)
    // attach somewhere. Its superclass is the parent's metaclass (or
    // cls_class if there's no parent), giving us a proper class-side chain
    // back up to Class>>new and friends.
    struct asom_class *meta = calloc(1, sizeof(*meta));
    char meta_name_buf[256];
    snprintf(meta_name_buf, sizeof(meta_name_buf), "%s class", pc->name);
    meta->name = asom_intern_cstr(meta_name_buf);
    meta->superclass = (super && super->metaclass) ? super->metaclass : c->cls_class;
    meta->hdr.klass = c->cls_metaclass;
    meta->metaclass = c->cls_metaclass;

    struct asom_class *cls = calloc(1, sizeof(*cls));
    cls->hdr.klass = meta;
    cls->metaclass = meta;
    cls->name = pc->name;
    cls->superclass = super;
    cls->num_instance_fields = pc->fields_cnt;
    if (pc->fields_cnt) {
        cls->field_names = malloc(pc->fields_cnt * sizeof(*cls->field_names));
        memcpy(cls->field_names, pc->fields, pc->fields_cnt * sizeof(*cls->field_names));
    }
    if (pc->class_fields_cnt) {
        cls->num_class_side_fields = pc->class_fields_cnt;
        cls->class_side_fields = calloc(pc->class_fields_cnt, sizeof(VALUE));
        for (uint32_t i = 0; i < pc->class_fields_cnt; i++) cls->class_side_fields[i] = c->val_nil;
        cls->class_side_field_names = malloc(pc->class_fields_cnt * sizeof(*cls->class_side_field_names));
        memcpy(cls->class_side_field_names, pc->class_fields, pc->class_fields_cnt * sizeof(*cls->class_side_field_names));
    }
    asom_global_set(c, pc->name, ASOM_OBJ2VAL(cls));

    char label_buf[256];
    for (uint32_t i = 0; i < pc->methods_cnt; i++) {
        struct asom_method *m = make_ast_method(cls, pc->methods[i]);
        asom_class_define_method(cls, m);
        if (m->body) {
            snprintf(label_buf, sizeof(label_buf), "%s>>%s", pc->name, m->selector);
            asom_register_entry(m->body, asom_intern_cstr(label_buf), pc->name);
        }
    }
    for (uint32_t i = 0; i < pc->class_methods_cnt; i++) {
        struct asom_method *m = make_ast_method(meta, pc->class_methods[i]);
        asom_class_define_method(meta, m);
        if (m->body) {
            snprintf(label_buf, sizeof(label_buf), "%s class>>%s", pc->name, m->selector);
            asom_register_entry(m->body, asom_intern_cstr(label_buf), pc->name);
        }
    }
    return cls;
}
