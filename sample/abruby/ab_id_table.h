#ifndef AB_ID_TABLE_H
#define AB_ID_TABLE_H 1

#include <ruby.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

// Hybrid ID -> VALUE table.
//
// - Small tables (capa <= AB_ID_TABLE_SMALL_CAPA) are packed: entries are stored
//   sequentially in [0, cnt) and lookup is a linear scan of exactly `cnt` items.
//   This avoids the hash-multiply and empty-slot overhead for tiny tables
//   (typical user classes with a handful of methods or objects with a few ivars).
//
// - Large tables use open-addressing hashing. capa is a power of two.
//   Empty slots are marked by key == 0 (rb_intern never returns 0).
//   Linear probing with backward-shift deletion (no tombstones).
//
// The two layouts don't coexist: when a small table grows past the threshold,
// ab_id_table_grow switches to the hash layout in one step.

#define AB_ID_TABLE_SMALL_CAPA 4

struct ab_id_table_entry {
    ID key;
    VALUE val;
};

// Table with embedded inline storage. Small tables (<= SMALL_CAPA entries)
// live entirely inside the struct with zero separate allocations — crucial
// for abruby_object's ivars where binary-trees-style workloads allocate
// millions of objects and paying an extra ruby_xmalloc2 per object is the
// dominant cost.  When a table outgrows the inline storage,
// ab_id_table_grow allocates a heap buffer and `entries` is switched; the
// inline storage stays resident but unused.
struct ab_id_table {
    unsigned int cnt;
    unsigned int capa;
    struct ab_id_table_entry *entries;
    struct ab_id_table_entry inline_storage[AB_ID_TABLE_SMALL_CAPA];
};

// Static zero-initialization: capa=0, entries=NULL. On first insert,
// reserve_one switches `entries` to `inline_storage` with capa=SMALL_CAPA.
#define AB_ID_TABLE_INIT {0, 0, NULL, {{0, 0}, {0, 0}, {0, 0}, {0, 0}}}

static inline bool
ab_id_table_is_inline(const struct ab_id_table *t)
{
    return t->entries == &t->inline_storage[0];
}

static inline void
ab_id_table_init(struct ab_id_table *t)
{
    t->cnt = 0;
    t->capa = 0;
    t->entries = NULL;
    memset(t->inline_storage, 0, sizeof(t->inline_storage));
}

static inline void
ab_id_table_free(struct ab_id_table *t)
{
    if (t->entries && !ab_id_table_is_inline(t)) {
        ruby_xfree(t->entries);
    }
    t->entries = NULL;
    t->cnt = t->capa = 0;
}

static inline unsigned int
ab_id_table_hash_index(ID key, unsigned int mask)
{
    // Fibonacci hashing: spreads sparse Ruby IDs across the table.
    return (unsigned int)(((uintptr_t)key * 0x9e3779b97f4a7c15ULL) >> 32) & mask;
}

static inline bool
ab_id_table_lookup(const struct ab_id_table *t, ID key, VALUE *val)
{
    unsigned int capa = t->capa;
    if (capa == 0) return false;
    if (capa <= AB_ID_TABLE_SMALL_CAPA) {
        // Packed layout: only the first cnt slots are occupied.
        unsigned int cnt = t->cnt;
        for (unsigned int i = 0; i < cnt; i++) {
            if (t->entries[i].key == key) {
                if (val) *val = t->entries[i].val;
                return true;
            }
        }
        return false;
    }
    unsigned int mask = capa - 1;
    unsigned int i = ab_id_table_hash_index(key, mask);
    for (;;) {
        ID k = t->entries[i].key;
        if (k == 0) return false;
        if (k == key) {
            if (val) *val = t->entries[i].val;
            return true;
        }
        i = (i + 1) & mask;
    }
}

// Like ab_id_table_lookup but also returns the slot index on hit.
// On miss, *slot is set to UINT_MAX.
static inline bool
ab_id_table_lookup_slot(const struct ab_id_table *t, ID key, VALUE *val, unsigned int *slot)
{
    unsigned int capa = t->capa;
    if (capa == 0) { *slot = UINT_MAX; return false; }
    if (capa <= AB_ID_TABLE_SMALL_CAPA) {
        unsigned int cnt = t->cnt;
        for (unsigned int i = 0; i < cnt; i++) {
            if (t->entries[i].key == key) {
                if (val) *val = t->entries[i].val;
                *slot = i;
                return true;
            }
        }
        *slot = UINT_MAX;
        return false;
    }
    unsigned int mask = capa - 1;
    unsigned int i = ab_id_table_hash_index(key, mask);
    for (;;) {
        ID k = t->entries[i].key;
        if (k == 0) { *slot = UINT_MAX; return false; }
        if (k == key) {
            if (val) *val = t->entries[i].val;
            *slot = i;
            return true;
        }
        i = (i + 1) & mask;
    }
}

// Insert into hash layout. Caller ensures capa > AB_ID_TABLE_SMALL_CAPA and enough capacity.
static inline bool
ab_id_table_hash_insert_nogrow(struct ab_id_table *t, ID key, VALUE val)
{
    unsigned int mask = t->capa - 1;
    unsigned int i = ab_id_table_hash_index(key, mask);
    for (;;) {
        ID k = t->entries[i].key;
        if (k == 0) {
            t->entries[i].key = key;
            t->entries[i].val = val;
            t->cnt++;
            return true;
        }
        if (k == key) {
            t->entries[i].val = val;
            return false;
        }
        i = (i + 1) & mask;
    }
}

// Grow to new_capa. Handles both the small-packed -> hash and hash -> hash transitions.
// If new_capa fits in inline_storage and we're not already using it, prefer inline.
static inline void
ab_id_table_grow(struct ab_id_table *t, unsigned int new_capa)
{
    struct ab_id_table_entry *old_entries = t->entries;
    unsigned int old_capa = t->capa;
    unsigned int old_cnt = t->cnt;
    bool old_was_inline = old_entries && ab_id_table_is_inline(t);

    struct ab_id_table_entry *new_entries;
    bool new_is_inline;
    if (new_capa <= AB_ID_TABLE_SMALL_CAPA && !old_was_inline) {
        // Fresh inline transition: use the embedded storage, no malloc.
        new_entries = t->inline_storage;
        memset(new_entries, 0, AB_ID_TABLE_SMALL_CAPA * sizeof(struct ab_id_table_entry));
        new_is_inline = true;
    } else {
        new_entries = (struct ab_id_table_entry *)ruby_xcalloc(new_capa, sizeof(struct ab_id_table_entry));
        new_is_inline = false;
    }
    t->entries = new_entries;
    t->capa = new_capa;
    t->cnt = 0;
    if (new_capa <= AB_ID_TABLE_SMALL_CAPA) {
        // Small -> small: copy packed entries in order.
        memcpy(t->entries, old_entries, old_cnt * sizeof(struct ab_id_table_entry));
        t->cnt = old_cnt;
    } else if (old_capa <= AB_ID_TABLE_SMALL_CAPA) {
        // Small (packed) -> large (hash): entries live in [0, old_cnt).
        for (unsigned int j = 0; j < old_cnt; j++) {
            ab_id_table_hash_insert_nogrow(t, old_entries[j].key, old_entries[j].val);
        }
    } else {
        // Large -> large: scan all old slots.
        for (unsigned int j = 0; j < old_capa; j++) {
            if (old_entries[j].key != 0) {
                ab_id_table_hash_insert_nogrow(t, old_entries[j].key, old_entries[j].val);
            }
        }
    }
    // Only free if the old buffer was a heap allocation.
    if (old_entries && !old_was_inline && !new_is_inline) {
        ruby_xfree(old_entries);
    } else if (old_entries && !old_was_inline && new_is_inline) {
        // Unlikely shrinking path into inline.
        ruby_xfree(old_entries);
    }
    (void)new_is_inline;
}

// Grow the table if the upcoming insert would exceed capacity / load factor.
// Must be called before touching entries.
static inline void
ab_id_table_reserve_one(struct ab_id_table *t)
{
    if (t->capa == 0) {
        ab_id_table_grow(t, AB_ID_TABLE_SMALL_CAPA);
        return;
    }
    if (t->capa <= AB_ID_TABLE_SMALL_CAPA) {
        // Packed: grow when fully occupied.
        if (t->cnt >= t->capa) {
            ab_id_table_grow(t, t->capa * 2);
        }
    } else {
        // Hash: maintain load factor <= 0.75 after insert.
        if ((t->cnt + 1) * 4 > t->capa * 3) {
            ab_id_table_grow(t, t->capa * 2);
        }
    }
}

// Insert or update. Returns true if new entry, false if updated existing.
static inline bool
ab_id_table_insert(struct ab_id_table *t, ID key, VALUE val)
{
    // Update-in-place: check existing key first (avoids grow-then-update).
    if (t->capa > 0) {
        if (t->capa <= AB_ID_TABLE_SMALL_CAPA) {
            unsigned int cnt = t->cnt;
            for (unsigned int i = 0; i < cnt; i++) {
                if (t->entries[i].key == key) {
                    t->entries[i].val = val;
                    return false;
                }
            }
        } else {
            unsigned int mask = t->capa - 1;
            unsigned int i = ab_id_table_hash_index(key, mask);
            for (;;) {
                ID k = t->entries[i].key;
                if (k == 0) break;
                if (k == key) {
                    t->entries[i].val = val;
                    return false;
                }
                i = (i + 1) & mask;
            }
        }
    }
    ab_id_table_reserve_one(t);
    if (t->capa <= AB_ID_TABLE_SMALL_CAPA) {
        t->entries[t->cnt].key = key;
        t->entries[t->cnt].val = val;
        t->cnt++;
        return true;
    }
    return ab_id_table_hash_insert_nogrow(t, key, val);
}

// Insert or update, returning the final slot index via *slot.
static inline bool
ab_id_table_insert_slot(struct ab_id_table *t, ID key, VALUE val, unsigned int *slot)
{
    if (t->capa > 0) {
        if (t->capa <= AB_ID_TABLE_SMALL_CAPA) {
            unsigned int cnt = t->cnt;
            for (unsigned int i = 0; i < cnt; i++) {
                if (t->entries[i].key == key) {
                    t->entries[i].val = val;
                    *slot = i;
                    return false;
                }
            }
        } else {
            unsigned int mask = t->capa - 1;
            unsigned int i = ab_id_table_hash_index(key, mask);
            for (;;) {
                ID k = t->entries[i].key;
                if (k == 0) break;
                if (k == key) {
                    t->entries[i].val = val;
                    *slot = i;
                    return false;
                }
                i = (i + 1) & mask;
            }
        }
    }
    ab_id_table_reserve_one(t);
    if (t->capa <= AB_ID_TABLE_SMALL_CAPA) {
        unsigned int i = t->cnt;
        t->entries[i].key = key;
        t->entries[i].val = val;
        t->cnt++;
        *slot = i;
        return true;
    }
    unsigned int mask = t->capa - 1;
    unsigned int i = ab_id_table_hash_index(key, mask);
    for (;;) {
        if (t->entries[i].key == 0) {
            t->entries[i].key = key;
            t->entries[i].val = val;
            t->cnt++;
            *slot = i;
            return true;
        }
        i = (i + 1) & mask;
    }
}

// Delete by key. Returns true if found and deleted, false if not found.
static inline bool
ab_id_table_delete(struct ab_id_table *t, ID key)
{
    if (t->capa == 0) return false;
    if (t->capa <= AB_ID_TABLE_SMALL_CAPA) {
        unsigned int cnt = t->cnt;
        for (unsigned int i = 0; i < cnt; i++) {
            if (t->entries[i].key == key) {
                // Pack: move last entry into this slot.
                t->entries[i] = t->entries[cnt - 1];
                t->entries[cnt - 1].key = 0;
                t->entries[cnt - 1].val = 0;
                t->cnt--;
                return true;
            }
        }
        return false;
    }
    unsigned int mask = t->capa - 1;
    unsigned int i = ab_id_table_hash_index(key, mask);
    for (;;) {
        ID k = t->entries[i].key;
        if (k == 0) return false;
        if (k == key) break;
        i = (i + 1) & mask;
    }
    // Backward-shift: move back any subsequent entries whose natural slot is <= i.
    unsigned int j = i;
    for (;;) {
        unsigned int next = (j + 1) & mask;
        ID nk = t->entries[next].key;
        if (nk == 0) break;
        unsigned int natural = ab_id_table_hash_index(nk, mask);
        if (((next - natural) & mask) < ((j - natural) & mask)) break;
        t->entries[j] = t->entries[next];
        j = next;
    }
    t->entries[j].key = 0;
    t->entries[j].val = 0;
    t->cnt--;
    return true;
}

// Shallow clone: copy entries (values are not deep-copied).
static inline void
ab_id_table_clone(struct ab_id_table *dst, const struct ab_id_table *src)
{
    if (src->capa == 0) {
        dst->entries = NULL;
        dst->capa = 0;
        dst->cnt = 0;
        return;
    }
    if (src->capa <= AB_ID_TABLE_SMALL_CAPA) {
        // Fits in the destination's inline storage — no allocation.
        memcpy(dst->inline_storage, src->entries, src->capa * sizeof(struct ab_id_table_entry));
        dst->entries = dst->inline_storage;
        dst->capa = src->capa;
        dst->cnt = src->cnt;
        return;
    }
    // Larger than inline: heap-allocate.  Publish `entries` before cnt/capa
    // so a GC triggered by ruby_xmalloc2 never sees a half-built table.
    struct ab_id_table_entry *new_entries =
        (struct ab_id_table_entry *)ruby_xmalloc2(src->capa, sizeof(struct ab_id_table_entry));
    memcpy(new_entries, src->entries, src->capa * sizeof(struct ab_id_table_entry));
    dst->entries = new_entries;
    dst->capa = src->capa;
    dst->cnt = src->cnt;
}

// Iterate over all live entries. Small tables are packed in [0, cnt);
// large tables must skip empty (key == 0) slots.
#define ab_id_table_foreach(t, key_var, val_var, body) \
    do { \
        unsigned int _capa = (t)->capa; \
        if (_capa <= AB_ID_TABLE_SMALL_CAPA) { \
            unsigned int _cnt = (t)->cnt; \
            for (unsigned int _i = 0; _i < _cnt; _i++) { \
                ID key_var __attribute__((unused)) = (t)->entries[_i].key; \
                VALUE val_var __attribute__((unused)) = (t)->entries[_i].val; \
                body \
            } \
        } else { \
            for (unsigned int _i = 0; _i < _capa; _i++) { \
                if ((t)->entries[_i].key == 0) continue; \
                ID key_var __attribute__((unused)) = (t)->entries[_i].key; \
                VALUE val_var __attribute__((unused)) = (t)->entries[_i].val; \
                body \
            } \
        } \
    } while (0)

#endif
