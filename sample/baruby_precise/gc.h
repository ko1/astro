#ifndef BARUBY_PRECISE_GC_H
#define BARUBY_PRECISE_GC_H 1

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Forward decls (defined in context.h)
struct CTX_struct;
typedef struct CTX_struct CTX;
typedef intptr_t VALUE;

// ----------------------------------------------------------------------------
// Precise semispace (Cheney) moving GC with stress mode.
//
// Two equal-size mmap'd regions.  Allocate from the active region; when full
// (or in stress mode, on every alloc) GC copies all reachable objects to the
// inactive region, swaps active, and (in stress mode) mprotect's the old
// active as PROT_NONE so any stale pointer dereference SIGSEGVs immediately.
//
// Stress mode (env var BARUBY_GC_STRESS=1):
//   - every baruby_gc_alloc triggers a full collection
//   - old from-space is mprotect'd PROT_NONE after move
//   This catches: VALUE held in C local across an alloc + use of the C
//   local after the alloc.  The C local is the OLD (from-space) address;
//   reading through it segfaults.  Compare with the root in sp[] which GC
//   has already updated to the new address.
//
// Memory layout per allocation:
//
//   [ GCHeader (16B) | payload (size bytes, 8-byte aligned) ]
//
// GCHeader.fwd is NULL while the object is live; during GC the from-space
// header's fwd is overwritten with the to-space payload pointer
// (= forwarding pointer for Cheney's algorithm).
// ----------------------------------------------------------------------------

typedef enum {
    KIND_FREE         = 0,
    KIND_OBJ_ARRAY    = 1,   // BaArray header (items field is a pointer)
    KIND_OBJ_STRING   = 2,   // BaString header (bytes field is a pointer)
    KIND_PAYLOAD_VAL  = 3,   // VALUE[] (BaArray.items target)
    KIND_PAYLOAD_BYTE = 4,   // char[]  (BaString.bytes target)
} BarubyGCKind;

typedef struct GCHeader {
    uint32_t kind;
    uint32_t size;   // payload bytes (not including this header)
    void    *fwd;    // NULL while live; forwarding-ptr-to-new-payload during/after move
} GCHeader;

typedef struct {
    size_t total_bytes;
    size_t heap_bytes;
    size_t gc_count;
} BarubyGCStats;

extern BarubyGCStats baruby_gc_stats;
extern int baruby_gc_stress;     // 1 = collect on every alloc + mprotect from-space

// Initialize: mmap two regions, register CTX.
void baruby_gc_init(CTX *c);

// Allocate `payload_size` bytes for an object of `kind` (zero-initialized).
// Use for KIND_OBJ_ARRAY / KIND_OBJ_STRING / KIND_PAYLOAD_VAL — anything
// the collector scans for pointers / VALUEs.  May trigger GC (always in
// stress mode).  Returns the payload (after the GCHeader).
void *baruby_gc_alloc(BarubyGCKind kind, size_t payload_size, VALUE *sp_top);

// Raw byte payload (KIND_PAYLOAD_BYTE) — GC never reads it as pointers,
// so the memset is skipped.  Caller MUST fill bytes[0..size-1] before the
// next alloc / GC opportunity.
void *baruby_gc_alloc_byte(size_t payload_size, VALUE *sp_top);

// Realloc a payload object.  Allocates a new block of new_size bytes,
// copies min(old_size, new_size) bytes from the old block, returns the
// new pointer.  CAUTION: the old block may move during the new alloc
// (= become stale).  In non-stress mode, the implementation follows the
// fwd pointer.  In stress mode, dereferencing the stale old pointer to
// read fwd will SIGSEGV — the bug must be fixed in the caller.
void *baruby_gc_realloc_payload(void *old, size_t new_size, VALUE *sp_top);

// Force a collection now.
void baruby_gc_collect(VALUE *sp_top);

size_t baruby_gc_total_bytes(void);
size_t baruby_gc_heap_bytes(void);
size_t baruby_gc_count(void);

#endif
