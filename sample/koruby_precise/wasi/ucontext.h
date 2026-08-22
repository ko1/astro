/* WASI には ucontext が無い。Fiber / Thread はこの構成では使えないので、
 * 型と関数だけ用意して呼ばれたら失敗させる (koruby は起動時には触らない)。 */
#ifndef KORB_WASI_UCONTEXT_H
#define KORB_WASI_UCONTEXT_H
#include <errno.h>
typedef struct { void *ss_sp; size_t ss_size; int ss_flags; } korb_wasi_stack_t;
typedef struct ucontext_t {
    struct ucontext_t *uc_link;
    korb_wasi_stack_t  uc_stack;
    void              *uc_opaque[16];
} ucontext_t;
static inline int  getcontext(ucontext_t *u)                       { (void)u; return 0; }
static inline int  setcontext(const ucontext_t *u)                 { (void)u; errno = ENOTSUP; return -1; }
static inline int  swapcontext(ucontext_t *a, const ucontext_t *b) { (void)a; (void)b; errno = ENOTSUP; return -1; }
static inline void makecontext(ucontext_t *u, void (*f)(void), int n, ...) { (void)u; (void)f; (void)n; }
#endif
