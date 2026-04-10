#ifndef AB_ID_TABLE_H
#define AB_ID_TABLE_H 1

#include <ruby.h>
#include <stdint.h>
#include <string.h>

// Generic ID → VALUE table with dynamic growth.
// Used for methods, constants, instance variables, and global variables.

struct ab_id_table_entry {
    ID key;
    VALUE val;
};

struct ab_id_table {
    unsigned int cnt;
    unsigned int capa;
    struct ab_id_table_entry *entries;
};

#define AB_ID_TABLE_INIT {0, 0, NULL}

static inline void
ab_id_table_init(struct ab_id_table *t)
{
    t->cnt = 0;
    t->capa = 0;
    t->entries = NULL;
}

static inline void
ab_id_table_free(struct ab_id_table *t)
{
    if (t->entries) {
        ruby_xfree(t->entries);
        t->entries = NULL;
    }
    t->cnt = t->capa = 0;
}

// Lookup by key. Returns true if found (and sets *val), false if not found.
static inline bool
ab_id_table_lookup(const struct ab_id_table *t, ID key, VALUE *val)
{
    for (unsigned int i = 0; i < t->cnt; i++) {
        if (t->entries[i].key == key) {
            if (val) *val = t->entries[i].val;
            return true;
        }
    }
    return false;
}

// Insert or update. Returns true if new entry, false if updated existing.
static inline bool
ab_id_table_insert(struct ab_id_table *t, ID key, VALUE val)
{
    for (unsigned int i = 0; i < t->cnt; i++) {
        if (t->entries[i].key == key) {
            t->entries[i].val = val;
            return false;  // updated
        }
    }
    // New entry: grow if needed
    if (t->cnt >= t->capa) {
        unsigned int new_capa = t->capa == 0 ? 4 : t->capa * 2;
        t->entries = ruby_xrealloc2(t->entries, new_capa, sizeof(struct ab_id_table_entry));
        t->capa = new_capa;
    }
    t->entries[t->cnt].key = key;
    t->entries[t->cnt].val = val;
    t->cnt++;
    return true;  // new
}

// Delete by key. Returns true if found and deleted, false if not found.
static inline bool
ab_id_table_delete(struct ab_id_table *t, ID key)
{
    for (unsigned int i = 0; i < t->cnt; i++) {
        if (t->entries[i].key == key) {
            // Shift remaining entries
            t->cnt--;
            if (i < t->cnt) {
                memmove(&t->entries[i], &t->entries[i + 1],
                        sizeof(struct ab_id_table_entry) * (t->cnt - i));
            }
            return true;
        }
    }
    return false;
}

// Iterate over all entries. Callback receives key and val.
#define ab_id_table_foreach(t, key_var, val_var, body) \
    for (unsigned int _i = 0; _i < (t)->cnt; _i++) { \
        ID key_var __attribute__((unused)) = (t)->entries[_i].key; \
        VALUE val_var __attribute__((unused)) = (t)->entries[_i].val; \
        body \
    }

#endif
