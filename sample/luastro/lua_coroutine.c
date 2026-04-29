// luastro coroutines — ucontext-based implementation.
//
// Each coroutine has its own (CTX, ucontext, status) plus a tiny C
// stack we mmap.  Resume swaps to the coroutine's context; yield swaps
// back.  Values cross the boundary through a per-coroutine slot pair.
//
// Public API matches Lua's coroutine library:
//   coroutine.create(fn)      → thread
//   coroutine.resume(co, ...) → ok, ...vals
//   coroutine.yield(...)      → ...vals from next resume
//   coroutine.status(co)      → "suspended" / "running" / "dead" / "normal"
//   coroutine.wrap(fn)        → caller-style fn that resumes underneath
//   coroutine.isyieldable()   → bool
//   coroutine.running()       → thread, isMain

#include <ucontext.h>
#include <sys/mman.h>
#include "context.h"

#define LUACO_STACK_SIZE  (256 * 1024)
#define LUACO_MAX_TRANSFER 16

typedef enum {
    LUACO_SUSPENDED, LUACO_RUNNING, LUACO_NORMAL, LUACO_DEAD,
} luaco_status_t;

struct LuaCoroutine {
    ucontext_t ctx;
    void      *stack;       // mmap'd C stack for this co
    luaco_status_t status;
    LuaValue   fn;          // the body fn
    LuaValue   transfer[LUACO_MAX_TRANSFER];   // yield/resume payload
    uint32_t   transfer_cnt;
    LuaValue   error_val;   // set if body raised
    bool       errored;

    struct LuaCoroutine *prev;   // resumer chain (for status=normal)
};

// Single global "current coroutine" pointer; main thread = NULL.
static struct LuaCoroutine *LUACO_CURRENT = NULL;
static ucontext_t LUACO_MAIN_CTX;

// Trampoline: ucontext makecontext takes int args only.  Pass the co
// pointer through two ints.
static struct LuaCoroutine *G_pending_co = NULL;
static CTX                  *G_pending_c = NULL;

static void
luaco_entry(void)
{
    struct LuaCoroutine *co = G_pending_co;
    CTX *c = G_pending_c;
    LuaValue *args = co->transfer;
    uint32_t  argc = co->transfer_cnt;
    LuaValue fn = co->fn;
    // Clear before lua_call so reused transfer slots can carry results.
    co->transfer_cnt = 0;
    RESULT r;
    struct lua_pcall_frame frame = {0};
    frame.prev = c->pcall_top;
    c->pcall_top = &frame;
    if (setjmp(frame.jb) == 0) {
        r = lua_call(c, fn, args, argc);
        c->pcall_top = frame.prev;
        // Stash final results in transfer.
        uint32_t nret = c->ret_info.result_cnt;
        if (nret == 0) {
            co->transfer[0] = r;
            co->transfer_cnt = 1;
        } else {
            uint32_t lim = nret < LUACO_MAX_TRANSFER ? nret : LUACO_MAX_TRANSFER;
            for (uint32_t i = 0; i < lim; i++) co->transfer[i] = c->ret_info.results[i];
            co->transfer_cnt = lim;
        }
        co->errored = false;
    } else {
        c->pcall_top = frame.prev;
        co->errored = true;
        co->error_val = c->last_error;
        co->transfer_cnt = 0;
    }
    co->status = LUACO_DEAD;
    setcontext(&LUACO_MAIN_CTX);   // never returns
}

struct LuaCoroutine *
luaco_create(CTX *c, LuaValue fn)
{
    (void)c;
    struct LuaCoroutine *co = (struct LuaCoroutine *)calloc(1, sizeof(*co));
    co->stack = mmap(NULL, LUACO_STACK_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (co->stack == MAP_FAILED) { free(co); return NULL; }
    getcontext(&co->ctx);
    co->ctx.uc_stack.ss_sp   = co->stack;
    co->ctx.uc_stack.ss_size = LUACO_STACK_SIZE;
    co->ctx.uc_link          = NULL;
    co->status = LUACO_SUSPENDED;
    co->fn     = fn;
    return co;
}

// Resume: switch into the coroutine.  Returns when it yields or dies.
//   args are passed as the values yield/start sees.
//   On return, values are in c->ret_info.results.  result_cnt + 1 if errored
//   (we don't follow Lua's exact (false, msg) shape; we set it ourselves).
RESULT
luaco_resume(CTX *c, struct LuaCoroutine *co, LuaValue *args, uint32_t argc)
{
    if (co->status == LUACO_DEAD) {
        c->ret_info.results[0] = LUAV_BOOL(false);
        c->ret_info.results[1] = LUAV_STR(lua_str_intern("cannot resume dead coroutine"));
        c->ret_info.result_cnt = 2;
        return RESULT_OK(LUAV_BOOL(false));
    }
    if (co->status == LUACO_RUNNING) {
        c->ret_info.results[0] = LUAV_BOOL(false);
        c->ret_info.results[1] = LUAV_STR(lua_str_intern("cannot resume non-suspended coroutine"));
        c->ret_info.result_cnt = 2;
        return RESULT_OK(LUAV_BOOL(false));
    }
    // Stage args into transfer.
    uint32_t lim = argc < LUACO_MAX_TRANSFER ? argc : LUACO_MAX_TRANSFER;
    for (uint32_t i = 0; i < lim; i++) co->transfer[i] = args[i];
    co->transfer_cnt = lim;

    // Set up trampoline on first resume.
    bool first = (co->transfer != NULL && co->status == LUACO_SUSPENDED && co->ctx.uc_stack.ss_sp != NULL);
    static bool initialized_ctx = false;
    if (!first || !initialized_ctx) {
        // Initialize trampoline only once.
    }

    struct LuaCoroutine *prev = LUACO_CURRENT;
    if (prev) prev->status = LUACO_NORMAL;
    co->prev   = prev;
    co->status = LUACO_RUNNING;
    LUACO_CURRENT = co;

    G_pending_co = co;
    G_pending_c  = c;
    if (co->status == LUACO_RUNNING && co->ctx.uc_stack.ss_size && !LV_IS_NIL(co->fn)) {
        // First-time setup if uc_link not yet wired.
        static int firstrun_marker = 0;
        // Detect "fresh" by checking if we've ever swapped to this co.
        // We set up makecontext only on the first resume.
        // Simpler: use a flag on the co struct.
    }

    // We track first-resume via co->prev being NULL initially? No,
    // co->prev was assigned above.  Use a flag instead.
    // Add a started flag on the coroutine.
    // For simplicity, we keep a static last-resume target:
    extern void luaco_first_resume_setup(struct LuaCoroutine *co);
    luaco_first_resume_setup(co);

    swapcontext(&LUACO_MAIN_CTX, &co->ctx);

    LUACO_CURRENT = prev;
    if (prev) prev->status = LUACO_RUNNING;

    // Coroutine yielded or died.  Read transfer.
    if (co->errored) {
        co->errored = false;
        c->ret_info.results[0] = LUAV_BOOL(false);
        c->ret_info.results[1] = co->error_val;
        c->ret_info.result_cnt = 2;
        return RESULT_OK(LUAV_BOOL(false));
    }
    c->ret_info.results[0] = LUAV_BOOL(true);
    uint32_t n = co->transfer_cnt;
    if (n > LUASTRO_MAX_RETS - 1) n = LUASTRO_MAX_RETS - 1;
    for (uint32_t i = 0; i < n; i++) c->ret_info.results[i + 1] = co->transfer[i];
    c->ret_info.result_cnt = n + 1;
    return RESULT_OK(LUAV_BOOL(true));
}

// Yield: from within the coroutine, hand control back to its resumer.
RESULT
luaco_yield(CTX *c, LuaValue *args, uint32_t argc)
{
    if (!LUACO_CURRENT) lua_raisef(c, "attempt to yield from outside a coroutine");
    struct LuaCoroutine *co = LUACO_CURRENT;
    uint32_t lim = argc < LUACO_MAX_TRANSFER ? argc : LUACO_MAX_TRANSFER;
    for (uint32_t i = 0; i < lim; i++) co->transfer[i] = args[i];
    co->transfer_cnt = lim;
    co->status = LUACO_SUSPENDED;
    swapcontext(&co->ctx, &LUACO_MAIN_CTX);
    co->status = LUACO_RUNNING;
    // We're back: read transfer (the next resume's args).
    uint32_t n = co->transfer_cnt;
    if (n > LUASTRO_MAX_RETS) n = LUASTRO_MAX_RETS;
    for (uint32_t i = 0; i < n; i++) c->ret_info.results[i] = co->transfer[i];
    c->ret_info.result_cnt = n;
    return RESULT_OK(n ? co->transfer[0] : LUAV_NIL);
}

// First-resume setup helper: tracks whether makecontext has been wired
// for this coroutine.
void
luaco_first_resume_setup(struct LuaCoroutine *co)
{
    // We use uc_link as a sentinel: on first resume we wire the
    // trampoline; afterwards uc_link stays NULL (already wired).
    if (co->ctx.uc_link == (void *)1) return;     // already initialized
    makecontext(&co->ctx, luaco_entry, 0);
    co->ctx.uc_link = (void *)1;                  // marker; not used otherwise
}

// status name
const char *
luaco_status_name(struct LuaCoroutine *co)
{
    switch (co->status) {
    case LUACO_SUSPENDED: return "suspended";
    case LUACO_RUNNING:   return "running";
    case LUACO_NORMAL:    return "normal";
    case LUACO_DEAD:      return "dead";
    }
    return "?";
}

bool luaco_is_yieldable(void) { return LUACO_CURRENT != NULL; }
struct LuaCoroutine *luaco_running(void) { return LUACO_CURRENT; }
