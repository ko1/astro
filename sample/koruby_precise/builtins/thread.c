/* koruby_precise — thread.c: green threads (Ruby Thread), #included into
 * korb_runtime.c's TU after fiber.c (reuses its stack-size constants).
 *
 * Phase 1 (docs/io_design.md): native thread 1 本 + green thread M 本の協調
 * スケジューリング。切替点は blocking 操作 (M1: #join / Thread.pass) のみ。
 * 全 mutable 状態と GC roots は libc-stable な struct korb_thread に置き、
 * vm->thread_list 経由で AROH_VISIT_ROOTS が scan する (suspend 中の value
 * stack 含む)。KorbThread heap object は薄い可動 handle。
 *
 * 制約 (M1):
 *  - Fiber の中からの thread 切替: 走っている fiber の stack top を fiber 側
 *    (vslots_top) に、thread 側には root stack と running_fiber を退避する
 *    (korb_thread_ctx_save / ctx_load)。GC は fiber_list 経由で fstate 1/2 の
 *    非稼働 fiber を全部 scan する。
 *  - Thread#join(timeout) / #kill(実行中) / sleep 連携は M2/M3 (blop 層)。
 *  - dead thread の stack は未回収 (M2 で reap; rep 自体は意図的に不滅)。 */

/* ==== blop 層 — blocking operations (docs/io_design.md) ====================
 * 唯一の suspension point は korb_blop_wait。M2 実装範囲: TIMER (sleep /
 * join timeout) + pump (poll(2) ベース)。POLL/READ/WRITE の fd 系は M3。 */

enum korb_blop_kind  { KORB_BLOP_POLL, KORB_BLOP_READ, KORB_BLOP_WRITE, KORB_BLOP_TIMER, KORB_BLOP_CFUNC };
enum korb_blop_flags { KORB_BLOP_F_TIMEOUT = 1, KORB_BLOP_F_DONE = 2,
                       KORB_BLOP_F_DEADLOCKABLE = 4 /* Mutex#sleep forever: only another thread can end it */ };

static void korb_thread_runq_push(struct korb_vm *vm, struct korb_thread *t);   /* fwd (下の scheduler 節) */
static RESULT korb_bi_exit(CTX *c, VALUE *slots, VALUE_SLICE args);            /* fwd (korb_runtime.c) */
static RESULT korb_thread_tmo_arg(CTX *c, VALUE *slots, VALUE v, double *out);  /* fwd (下の IO 節) */

struct korb_blop {
    uint8_t  kind, flags;
    struct timespec deadline;         /* 絶対時刻 (CLOCK_MONOTONIC)。F_TIMEOUT 時有効 */
    ssize_t  result;                  /* 完了時: TIMER 0=満了 / -ETIMEDOUT / -ECANCELED */
    struct korb_thread *waiter;
    struct korb_blop *bl_prev, *bl_next;   /* vm->blop_pending 双方向リスト */
    union {                           /* op 固有 (M3 で使用) */
        struct { struct pollfd *fds; nfds_t nfds; }             poll;
        struct { int fd; void *buf; size_t len; int64_t off; }  rw;
        struct { void *(*fn)(void *); void *arg;
                 void  (*ubf)(void *); void *ubf_arg; }         cfunc;
    } u;
};

static void
korb_blop_now(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

#define KORB_TIME_QUANTUM_NS 100000000ull   /* CRuby's TIME_QUANTUM_MSEC (100) */
/* coarse (a few ms) is plenty for a 100ms slice and costs no syscall */
static uint64_t
korb_mono_coarse_ns(void)
{
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_COARSE
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* deadline = now + sec (秒は double) */
static void
korb_blop_deadline_in(struct korb_blop *b, double sec)
{
    struct timespec now; korb_blop_now(&now);
    double whole = 0.0, frac = 0.0;
    frac = sec - (double)(long)sec; whole = (double)(long)sec;
    b->deadline.tv_sec  = now.tv_sec + (time_t)whole;
    b->deadline.tv_nsec = now.tv_nsec + (long)(frac * 1e9);
    if (b->deadline.tv_nsec >= 1000000000L) { b->deadline.tv_sec++; b->deadline.tv_nsec -= 1000000000L; }
    b->flags |= KORB_BLOP_F_TIMEOUT;
}

static bool
korb_blop_deadline_passed(const struct korb_blop *b, const struct timespec *now)
{
    if (!(b->flags & KORB_BLOP_F_TIMEOUT)) return false;
    return now->tv_sec > b->deadline.tv_sec ||
           (now->tv_sec == b->deadline.tv_sec && now->tv_nsec >= b->deadline.tv_nsec);
}

static inline void korb_sched_refresh(struct korb_vm *vm) {
    vm->sched_busy = vm->runq_head != NULL || vm->blop_npending != 0;
}

/* engine への登録 (M2: pending リストに繋ぐだけ; fd 系の kernel 登録は M3) */
static void
korb_blop_prep(struct korb_vm *vm, struct korb_blop *b)
{
    b->flags &= (uint8_t)~KORB_BLOP_F_DONE;
    b->bl_prev = NULL; b->bl_next = vm->blop_pending;
    if (vm->blop_pending) vm->blop_pending->bl_prev = b;
    vm->blop_pending = b;
    vm->blop_npending++;
    korb_sched_refresh(vm);
}

/* 完了: 登録を外し result を書き、waiter を runnable に (二重 wake は state 検査で冪等) */
static void
korb_blop_post(struct korb_vm *vm, struct korb_blop *b, ssize_t result)
{
    if (b->flags & KORB_BLOP_F_DONE) return;
    if (b->bl_prev) b->bl_prev->bl_next = b->bl_next; else vm->blop_pending = b->bl_next;
    if (b->bl_next) b->bl_next->bl_prev = b->bl_prev;
    b->bl_prev = b->bl_next = NULL;
    vm->blop_npending--;
    korb_sched_refresh(vm);
    b->result = result;
    b->flags |= KORB_BLOP_F_DONE;
    struct korb_thread *const w = b->waiter;
    if (w && w->state == KORB_TH_PENDED) { w->state = KORB_TH_READY; korb_thread_runq_push(vm, w); }
}

static void
korb_blop_cancel(struct korb_vm *vm, struct korb_blop *b, int neg_errno)
{
    korb_blop_post(vm, b, neg_errno);
}

/* 期限切れ deadline を post。戻り値: post した数 */
static int
korb_blop_expire(struct korb_vm *vm)
{
    struct timespec now; korb_blop_now(&now);
    int posted = 0;
    for (struct korb_blop *b = vm->blop_pending, *nx; b; b = nx) {
        nx = b->bl_next;
        if (korb_blop_deadline_passed(b, &now)) {
            korb_blop_post(vm, b, b->kind == KORB_BLOP_TIMER ? 0 : -ETIMEDOUT);
            posted++;
        }
    }
    return posted;
}

/* fd を閉じる側から: その fd で park している POLL blop を POLLNVAL で叩き起こす
 * (waiter は syscall をやり直して EBADF を見る)。閉じるだけでは poll(2) が
 * POLLNVAL を返すことに頼ることになり、番号が別の fd に再利用された瞬間に
 * 永久に起きなくなる。 */
static void korb_blop_wake_fd(struct korb_vm *vm, int fd)
{
    if (fd < 0) return;
    for (struct korb_blop *b = vm->blop_pending, *nx; b; b = nx) {
        nx = b->bl_next;
        if (b->kind != KORB_BLOP_POLL) continue;
        ssize_t hit = 0;
        for (nfds_t i = 0; i < b->u.poll.nfds; i++)
            if (b->u.poll.fds[i].fd == fd) { b->u.poll.fds[i].revents = POLLNVAL; hit++; }
        if (hit) korb_blop_post(vm, b, hit);
    }
}

/* pending signal で main thread を runnable にする。ここでは Ruby は動かせないので、
 * 起こすだけ — 配送は起きた側の check point (korb_blop_wait → check_ints)。
 * CRuby は trap handler を main thread でしか走らせない (他の thread は配送しない)。
 * thread 未起動なら待っているのは main だけ。戻り値: 起こした数。 */
static int
korb_blop_signal_wake(struct korb_vm *vm)
{
    struct korb_thread *t = vm->main_thread;
    if (t == NULL)
        for (const struct korb_blop *b = vm->blop_pending; b && t == NULL; b = b->bl_next)
            if (b->waiter && b->waiter->state == KORB_TH_PENDED) t = b->waiter;
    if (t == NULL || t->state != KORB_TH_PENDED) return 0;
    if (t->blop) korb_blop_post(vm, t->blop, -EINTR);   /* wait 側が配送後に待ち直す */
    else { t->state = KORB_TH_READY; korb_thread_runq_push(vm, t); }
    return 1;
}

/* pump: 完了回収。block=1 なら fd readiness / 次の deadline まで native に眠る —
 * scheduler idle が呼ぶ、native thread が眠る唯一の場所。戻り値: post した数。
 * POLL blop の pollfd 群を 1 本の poll(2) に束ね、revents を書き戻して post。 */
static int
korb_blop_pump(struct korb_vm *vm, int block)
{
    int posted = korb_blop_expire(vm);
    if (UNLIKELY(vm->sig_caught != 0)) posted += korb_blop_signal_wake(vm);   /* reaped before a context switch */
    /* 眠る側だけ signalfd を見る: blocked signal は poll(2) から見えないので、
     * これが無いと trap 済みプロセスの sleep が kill(2) を寝過ごす。 */
    sigset_t sig_old;
    const bool sig_armed = block && korb_signal_sleep_begin(vm, &sig_old);
    const int sigfd = block ? korb_signal_wait_fd(vm) : -1;
    /* fd 収集 (POLL blops) */
    nfds_t total = (sigfd >= 0) ? 1 : 0;
    for (const struct korb_blop *b = vm->blop_pending; b; b = b->bl_next)
        if (b->kind == KORB_BLOP_POLL) total += b->u.poll.nfds;
    struct pollfd sbuf[64];
    struct pollfd *pf = (total <= 64) ? sbuf : malloc(sizeof(*pf) * total);
    if (!pf) abort();
    nfds_t k = 0;
    if (sigfd >= 0) { pf[0].fd = sigfd; pf[0].events = POLLIN; pf[0].revents = 0; k = 1; }
    const nfds_t fd0 = k;
    for (const struct korb_blop *b = vm->blop_pending; b; b = b->bl_next)
        if (b->kind == KORB_BLOP_POLL)
            for (nfds_t i = 0; i < b->u.poll.nfds; i++) {
                pf[k] = b->u.poll.fds[i]; pf[k].revents = 0; k++;
            }
    /* 眠る長さ: 既に post 済み or 非 block なら 0、それ以外は最短 deadline (無ければ無期限) */
    int ms = 0;
    if (!posted && block) {
        ms = -1;
        struct timespec now; korb_blop_now(&now);
        for (const struct korb_blop *b = vm->blop_pending; b; b = b->bl_next) {
            if (!(b->flags & KORB_BLOP_F_TIMEOUT)) continue;
            long d = (long)(b->deadline.tv_sec - now.tv_sec) * 1000
                   + (b->deadline.tv_nsec - now.tv_nsec) / 1000000L;
            if (d < 0) d = 0;
            if (ms < 0 || d < ms) ms = (int)d;
        }
    }
    if (total == 0 && ms == 0) {
        if (sig_armed) korb_signal_sleep_end(vm, &sig_old);
        if (pf != sbuf) free(pf);
        return posted;
    }
    const int rc = poll(pf, total, ms);
    if (sig_armed) korb_signal_sleep_end(vm, &sig_old);
    /* signal の起床は revents 書き戻しの後: blop を list から外すと pf[] の
       index 対応が崩れる */
    const bool sig_ready = (rc > 0 && sigfd >= 0 && pf[0].revents != 0);
    if (rc > 0) {                                   /* revents を各 blop に書き戻し */
        k = fd0;
        for (struct korb_blop *b = vm->blop_pending, *nx; b; b = nx) {
            nx = b->bl_next;
            if (b->kind != KORB_BLOP_POLL) continue;
            int ready = 0;
            for (nfds_t i = 0; i < b->u.poll.nfds; i++) {
                b->u.poll.fds[i].revents = pf[k + i].revents;
                if (pf[k + i].revents) ready++;
            }
            k += b->u.poll.nfds;
            if (ready) { korb_blop_post(vm, b, ready); posted++; }
        }
    }
    if (pf != sbuf) free(pf);
    posted += korb_blop_expire(vm);
    if (sig_ready) posted += korb_blop_signal_wake(vm);
    return posted;
}

/* Collect finished timers / ready fds without sleeping, at a yield point.  With
 * others runnable the run queue never drains, so without this two spinning
 * threads starve every sleeper; that case is rate-limited to once a millisecond. */
static void
korb_blop_pump_at_yield(struct korb_vm *vm)
{
    if (vm->blop_npending == 0) return;
    if (vm->runq_head != NULL) {
        struct timespec now; korb_blop_now(&now);
        const int64_t ns = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
        if (ns < vm->blop_next_poll_ns) return;
        vm->blop_next_poll_ns = ns + 1000000;
    }
    (void)korb_blop_pump(vm, 0);
}

/* POLL blop 1 発: fds (stable メモリ) の readiness を待つ。timeout_sec < 0 =
 * 無期限。*out_ready = ready fd 数 (0 = timeout)。per-fd 結果は fds[i].revents。 */
static RESULT korb_blop_wait(CTX *c, VALUE *slots, struct korb_blop *b);   /* fwd */
static RESULT
korb_blop_poll_wait(CTX *c, VALUE *slots, struct pollfd *fds, nfds_t nfds,
                    double timeout_sec, ssize_t *out_ready)
{
    if (timeout_sec == 0.0) {                       /* poll(2) with a zero timeout: one probe, no parking */
        int n;
        do { n = poll(fds, nfds, 0); } while (n < 0 && errno == EINTR);
        *out_ready = (n > 0) ? n : 0;
        return RESULT_OK(KORB_NIL);
    }
    struct korb_blop b; memset(&b, 0, sizeof b);
    b.kind = KORB_BLOP_POLL;
    b.u.poll.fds = fds; b.u.poll.nfds = nfds;
    if (timeout_sec >= 0) korb_blop_deadline_in(&b, timeout_sec);
    CHECK(korb_blop_wait(c, slots, &b));
    *out_ready = (b.result > 0) ? b.result : 0;     /* -ETIMEDOUT / -ECANCELED → 0 */
    return RESULT_OK(KORB_NIL);
}

/* ---- ThreadError (const-only class) -------------------------------------- */
static RESULT
korb_raise_thread_error(CTX *c, VALUE *slots, const char *msg)
{
    const VALUE cls = korb_const_get(c->vm, korb_intern(c->vm, "ThreadError", 11));
    slots[0] = KORB_CLASS_P(cls) ? cls : KORB_NIL;
    RESULT r = korb_raise(c, slots + 1, KORB_E_RUNTIME, 0, "%s", msg);
    if (KORB_CLASS_P(slots[0]) && KORB_EXC_P(r.value))
        ARO_STORE(c, VAL2EXC(r.value), (VALUE *)(uintptr_t)&VAL2EXC(r.value)->exc_class, slots[0]);
    return r;
}

/* ---- run queue (READY FIFO) ---------------------------------------------- */
static void
korb_thread_runq_push(struct korb_vm *vm, struct korb_thread *t)
{
    t->rq_next = NULL;
    if (vm->runq_tail) vm->runq_tail->rq_next = t; else vm->runq_head = t;
    vm->runq_tail = t;
    vm->sched_busy = 1;
}

/* t の #join 待ちを全員起こす。既に runnable な待ち手は queue に入れ直さない:
 * 二重 push は runq を切り詰めて、後ろの thread を丸ごと落とす。 */
static void
korb_thread_joiners_wake(struct korb_vm *vm, struct korb_thread *t)
{
    for (struct korb_thread *j = t->joiners, *nx; j; j = nx) {
        nx = j->join_next;
        j->join_next = NULL;
        if (j->state != KORB_TH_PENDED) continue;      /* 割り込み等で既に起きている */
        j->state = KORB_TH_READY;
        korb_thread_runq_push(vm, j);
    }
    t->joiners = NULL;
}

static struct korb_thread *
korb_thread_runq_pop(struct korb_vm *vm)
{
    for (;;) {
        struct korb_thread *t = vm->runq_head;
        if (t == NULL) return NULL;
        vm->runq_head = t->rq_next;
        if (vm->runq_head == NULL) vm->runq_tail = NULL;
        korb_sched_refresh(vm);
        t->rq_next = NULL;
        if (t->state != KORB_TH_DEAD) return t;   /* killed-before-start: skip */
    }
}

/* ---- boot: main thread rep (lazy, first Thread API use) ------------------ */
static struct korb_thread *
korb_thread_boot(CTX *c)
{
    struct korb_vm *const vm = c->vm;
    if (vm->cur_thread) return vm->cur_thread;
    struct korb_thread *m = calloc(1, sizeof *m);
    if (!m) { fprintf(stderr, "koruby_precise: oom (thread rep)\n"); abort(); }
    vm->slice_start_ns = korb_mono_coarse_ns();   /* main's slice starts with the first thread */
    m->thval = KORB_NIL; m->args = KORB_NIL; m->blk = KORB_NIL; m->captured_self = KORB_NIL;
    m->result = KORB_NIL; m->exc = KORB_NIL; m->tls = KORB_NIL; m->tvars = KORB_NIL;
    m->tl_gv[0] = m->tl_gv[1] = m->tl_gv[2] = KORB_NIL;
    m->name = KORB_NIL; m->pending_ints = KORB_NIL; m->tgroup = KORB_NIL;
    /* main の stack base: fiber 内から boot されたら c->slots は fiber base なので
     * 保留し、最初の切替 (fiber 外が保証される) で埋める */
    if (vm->running_fiber == NULL) { m->vslots = c->slots; m->vslots_limit = c->slots_limit; }
    m->uctx = calloc(1, sizeof(ucontext_t));
    if (!m->uctx) abort();
    m->int_masks = KORB_NIL;
    m->root_fiber = vm->root_fiber;
    m->state = KORB_TH_RUNNING; m->started = 1; m->roe = 1;
    m->next = vm->thread_list; vm->thread_list = m;
    vm->cur_thread = vm->main_thread = m;
    return m;
}

/* handle (KorbThread) を遅延生成して返す */
static RESULT
korb_thread_handle(CTX *c, VALUE *slots, struct korb_thread *t)
{
    if (t->thval == KORB_NIL) {
        KorbThread *h = korb_alloc(c, slots, sizeof(KorbThread), KORB_OBJ_THREAD);
        h->rep = t;
        t->thval = (VALUE)h;
    }
    return RESULT_OK(t->thval);
}

/* ---- context switch ------------------------------------------------------ */
/* c の stack tuple を t のものに載せ替える (t: これから走る thread) */
/* $_, $? and $~ are per thread but live in the global table: park the running
 * thread's pair at a switch-out and put the next thread's back.  Written in
 * place, not through korb_const_define (a switch must not flush const caches). */
static void
korb_tl_gvars_swap(CTX *c, struct korb_thread *const t, const bool save)
{
    struct korb_vm *const vm = c->vm;
    if (UNLIKELY(vm->tl_gv_sym[0] == 0)) {
        vm->tl_gv_sym[0] = korb_intern(vm, "$_", 2);
        vm->tl_gv_sym[1] = korb_intern(vm, "$?", 2);
        vm->tl_gv_sym[2] = korb_intern(vm, "$~", 2);
    }
    for (int k = 0; k < 3; k++) {
        const uint32_t idx = korb_const_index(vm, vm->tl_gv_sym[k]);
        if (save) t->tl_gv[k] = idx != UINT32_MAX ? vm->const_vals[idx] : KORB_NIL;
        else if (idx != UINT32_MAX) vm->const_vals[idx] = t->tl_gv[k];
        else if (t->tl_gv[k] != KORB_NIL) korb_const_define(c, vm->tl_gv_sym[k], t->tl_gv[k]);
    }
}

static void
korb_thread_ctx_load(CTX *c, struct korb_thread *t)
{
    struct korb_vm *const vm = c->vm;
    KorbFiberRep *const f = t->saved_fiber;
    vm->slice_start_ns = korb_mono_coarse_ns();
    korb_tl_gvars_swap(c, t, false);
    if (!t->started) {                        /* 初走: 新品の stack */
        c->slots = t->vslots; c->slots_top = t->vslots;
        c->slots_limit = t->vslots_limit; c->slots_high_water = t->vslots;
        c->cstack_limit = (const char *)t->cstack + KORB_FIBER_CSTACK_MARGIN;
        vm->running_fiber = NULL; vm->main_slots = vm->main_slots_top = NULL;
    } else if (f != NULL) {                   /* fiber の中で suspend: fiber の stack tuple + root stack を戻す */
        c->slots = f->vslots; c->slots_top = f->vslots_top;
        c->slots_limit = f->vslots_limit; c->slots_high_water = f->vslots_hw;
        c->cstack_limit = (const char *)f->cstack + KORB_FIBER_CSTACK_MARGIN;
        vm->running_fiber = f;
        vm->main_slots = t->saved_base; vm->main_slots_top = t->saved_top;
    } else {                                  /* 再開: suspend 時の tuple */
        c->slots = t->saved_base; c->slots_top = t->saved_top;
        c->slots_limit = t->vslots_limit; c->slots_high_water = t->saved_hw;
        c->cstack_limit = t->saved_cstack_limit;
        vm->running_fiber = NULL; vm->main_slots = vm->main_slots_top = NULL;
    }
    korb_re_sync_floor(c);                    /* astrogre の C-stack floor も切替 */
    vm->root_fiber = t->root_fiber;           /* Fiber.current / storage は thread 毎 */
    /* backtrace: この thread の stack を指すので一緒に載せ替える (未開始なら空) */
    c->cfunc_link = t->started ? t->saved_cfunc_link : 0;
    c->carry_base = t->started ? t->saved_carry_base : NULL;
    c->carry_line_bits = t->started ? t->saved_carry_line_bits : 0;
    korb_scope_stacks_swap(c, &t->ss);        /* its own $! and $~ / cref / refinement save stacks */
}

/* 現 thread の stack tuple を退避する (yield_cpu の直前)。fiber の中なら走っている
 * fiber の top は fiber 側に置き (GC は fiber_list で scan)、thread 側は fiber
 * switch_in が park した root stack の範囲を持つ。 */
static void
korb_thread_ctx_save(CTX *c, struct korb_thread *cur, VALUE *slots)
{
    struct korb_vm *const vm = c->vm;
    KorbFiberRep *const f = vm->running_fiber;
    korb_tl_gvars_swap(c, cur, true);
    cur->saved_fiber = f;
    cur->root_fiber = vm->root_fiber;
    if (f != NULL) {
        f->vslots_top = slots; f->vslots_hw = c->slots_high_water;
        cur->saved_base = vm->main_slots; cur->saved_top = vm->main_slots_top;
        cur->saved_hw = NULL; cur->saved_cstack_limit = NULL;
    } else {
        if (cur->vslots == NULL) {            /* boot が fiber 内だった main (遅延分) */
            cur->vslots = c->slots; cur->vslots_limit = c->slots_limit;
        }
        cur->saved_base = c->slots; cur->saved_top = slots;
        cur->saved_hw = c->slots_high_water; cur->saved_cstack_limit = c->cstack_limit;
    }
    korb_scope_stacks_swap(c, &cur->ss);
    cur->saved_cfunc_link = c->cfunc_link;
    cur->saved_carry_base = c->carry_base;
    cur->saved_carry_line_bits = c->carry_line_bits;
}

static void
korb_thread_deadlock_fatal(void)
{
    fprintf(stderr, "No live threads left. Deadlock? (fatal)\n");   /* CRuby's fatal */
    exit(1);
}
static RESULT korb_thread_interrupt(CTX *c, VALUE *slots, struct korb_thread *t, VALUE exc);
/* CRuby raises `fatal` in the main thread, so its ensure / at_exit run.  An
 * interrupt main has not taken (masked) means nothing can change any more. */
static void
korb_thread_deadlock(CTX *c, VALUE *slots)
{
    struct korb_thread *const main = c->vm->main_thread;
    if (main == NULL || main->state == KORB_TH_DEAD
        || (main->pending_ints != KORB_NIL && VAL2ARY(main->pending_ints)->len > 0)) korb_thread_deadlock_fatal();
    const VALUE cls = korb_const_get(c->vm, korb_intern(c->vm, "fatal", 5));
    slots[0] = KORB_CLASS_P(cls) ? cls : KORB_NIL;
    char dump[400];   /* CRuby's message carries a thread dump; this is its outline */
    uint32_t n = 0;
    for (const struct korb_thread *t = c->vm->thread_list; t; t = t->next) n += t->state != KORB_TH_DEAD;
    int len = snprintf(dump, sizeof dump, "%u threads, %u sleeps current:%p main thread:%p\n",
                       n, n, (const void *)c->vm->cur_thread, (const void *)main);
    for (const struct korb_thread *t = c->vm->thread_list; t && len > 0 && (size_t)len < sizeof dump; t = t->next)
        if (t->state != KORB_TH_DEAD)
            len += snprintf(dump + len, sizeof dump - (size_t)len, "* #<Thread:%p sleep_forever>\n", (const void *)t);
    const RESULT r = korb_raise(c, slots + 1, KORB_E_RUNTIME, 0, "No live threads left. Deadlock?\n%s", dump);
    if (!KORB_CLASS_P(slots[0]) || !KORB_EXC_P(r.value)) korb_thread_deadlock_fatal();
    ARO_STORE(c, VAL2EXC(r.value), (VALUE *)(uintptr_t)&VAL2EXC(r.value)->exc_class, slots[0]);
    if (korb_thread_interrupt(c, slots, main, r.value).state != KORB_NORMAL) korb_thread_deadlock_fatal();
}
/* Nothing left that could wake anyone: no pending blop, or only untimed
 * Mutex#sleep ones (Queue#pop, ConditionVariable#wait).  A bare Kernel#sleep is
 * not one of them — CRuby lets `sleep` wait forever. */
static bool
korb_blop_deadlocked(struct korb_vm *const vm)
{
    korb_signal_park_pending(vm);            /* a trapped signal still pending in the kernel counts too */
    if (vm->sig_caught != 0) return false;   /* a signal is on its way to the main thread */
    for (const struct korb_blop *b = vm->blop_pending; b; b = b->bl_next)
        if (!(b->flags & KORB_BLOP_F_DEADLOCKABLE)) return false;
    return true;
}

/* 現 thread から離れる。呼び出し側が cur->state を設定済みであること
 * (Thread.pass: READY + runq_push 済 / #join: PENDED + joiners 登録済)。
 * `slots` = 生きた cursor (suspend 中の GC scan top)。
 * 他に走れる thread がいなければ: READY の cur はそのまま続行、
 * PENDED なら deadlock (M2 で blop pump に置き換わる)。 */
static void korb_thread_reap_pending(struct korb_vm *vm);   /* fwd */
static RESULT
korb_thread_yield_cpu(CTX *c, VALUE *slots)
{
    struct korb_vm *const vm = c->vm;
    struct korb_thread *const cur = vm->cur_thread;
    struct korb_thread *next = korb_thread_runq_pop(vm);
    if (next == NULL) {
        if (cur->state == KORB_TH_READY) { cur->state = KORB_TH_RUNNING; return RESULT_OK(KORB_NIL); }
        /* PENDED で他に runnable 無し: blop の完了を pump で待つ (native thread が
         * 眠る唯一の場所)。待てる blop も無ければ本物の deadlock。 */
        for (;;) {
            if (korb_blop_deadlocked(vm)) {       /* wakes main with `fatal` */
                korb_thread_deadlock(c, slots);
                if ((next = korb_thread_runq_pop(vm)) != NULL) break;
                continue;
            }
            korb_blop_pump(vm, 1);
            next = korb_thread_runq_pop(vm);
            if (next) break;
        }
        if (next == cur) { cur->state = KORB_TH_RUNNING; return RESULT_OK(KORB_NIL); }   /* 自分の blop が完了 */
    }
    korb_thread_ctx_save(c, cur, slots);
    vm->cur_thread = next;
    next->state = KORB_TH_RUNNING;
    korb_thread_ctx_load(c, next);
    const uint32_t mask_gen = vm->sigmask_gen;
    cur->mask_gen = mask_gen;
    if (next->mask_gen != mask_gen) korb_signal_park_pending(vm);   /* next's saved mask predates a trap */
    swapcontext((ucontext_t *)cur->uctx, (ucontext_t *)next->uctx);
    korb_sigmask_resume(vm, mask_gen);
    korb_thread_reap_pending(vm);
    /* === 再開: 切替元が我々の saved_* を c に載せてから swap してきている === */
    return RESULT_OK(KORB_NIL);
}

/* A dead thread's value stack, native stack and context.  Never the stack we
 * run on: a thread that ran is reaped by the next one (korb_thread_reap_pending). */
static void
korb_thread_free_stacks(struct korb_vm *const vm, struct korb_thread *const t, VALUE *const hw)
{
    if (t->cstack == NULL) return;                      /* main: the process stack */
    const uint32_t n = vm->thread_stack_pool_n;
    if (n < sizeof vm->thread_stack_pool / sizeof vm->thread_stack_pool[0]) {   /* keep it for the next Thread.new */
        vm->thread_stack_pool[n].vslots = t->vslots; vm->thread_stack_pool[n].hw = hw;
        vm->thread_stack_pool[n].cstack = t->cstack; vm->thread_stack_pool[n].uctx = t->uctx;
        vm->thread_stack_pool_n = n + 1;
    } else {
        munmap(t->vslots - 4, KORB_FIBER_VSLOTS_BYTES);
        free(t->cstack);
        free(t->uctx);
    }
    t->vslots = NULL; t->vslots_limit = NULL; t->cstack = NULL; t->uctx = NULL;
}
static void
korb_thread_reap_pending(struct korb_vm *const vm)
{
    struct korb_thread *const t = vm->reap_thread;
    if (t == NULL) return;
    vm->reap_thread = NULL;
    korb_thread_free_stacks(vm, t, t->saved_hw);
}

/* dead thread からの離脱 (状態保存なし)。戻らない。 */
static void
korb_thread_exit_switch(CTX *c)
{
    struct korb_vm *const vm = c->vm;
    struct korb_thread *next = korb_thread_runq_pop(vm);
    while (next == NULL) {                         /* 残りは全員 blop 待ち → pump */
        if (korb_blop_deadlocked(vm)) { korb_thread_deadlock(c, c->slots); next = korb_thread_runq_pop(vm); continue; }   /* wakes main */
        korb_blop_pump(vm, 1);
        next = korb_thread_runq_pop(vm);
    }
    vm->cur_thread = next;
    next->state = KORB_TH_RUNNING;
    korb_thread_ctx_load(c, next);
    if (next->mask_gen != vm->sigmask_gen) korb_signal_park_pending(vm);
    setcontext((ucontext_t *)next->uctx);     /* never returns */
}

/* ==== 割り込み (Thread#raise / #kill) ======================================
 * pending_ints (Array) に例外 VALUE (kill は KORB_FALSE マーカ) を積み、対象が
 * PENDED なら blop cancel / 直接 unpark で蹴り起こす。配送は check_ints
 * (blop_wait 戻り・join/pass/stop の wake 直後) — RUBY_VM_CHECK_INTS 相当。
 * Phase 1 は発行側も green thread なので対象が RUNNING のことはない。 */

/* Thread#kill の unwind payload: 例外でない即値なので rescue に掛からず $! も
 * 変えない (CRuby の TAG_FATAL 相当; ensure だけ走る)。Fiber#kill の KORB_UNDEF
 * とは別物 — fiber を貫いて thread 全体を畳む。 */
#define KORB_THREAD_KILL LONG2FIX(0x4b494c4c)

/* 自 thread を kill: main は exit(0) (CRuby)、他は aborting にして sentinel を投げる */
static RESULT
korb_thread_kill_raise(CTX *c, VALUE *slots)
{
    struct korb_thread *const cur = c->vm->cur_thread;
    if (cur == c->vm->main_thread) return korb_bi_exit(c, slots, VALUE_SLICE_MAKE(slots, 0));
    cur->aborting = 1;
    return RESULT_RAISE_(KORB_THREAD_KILL);
}

/* 割り込みを積んで、対象が PENDED なら起こす。exc = 例外 VALUE / KORB_FALSE (kill)。
 * exc は caller が slots に root 済みであること。 */
static RESULT
korb_thread_interrupt(CTX *c, VALUE *slots, struct korb_thread *t, VALUE exc)
{
    struct korb_vm *const vm = c->vm;
    slots[0] = exc;
    if (t->pending_ints == KORB_NIL)
        t->pending_ints = UNWRAP(korb_ary_new(c, slots + 1, 2));
    CHECK(korb_ary_push_val(c, slots + 1, VALUE_REF_AT(&t->pending_ints), slots[0]));
    if (t->state == KORB_TH_PENDED) {
        if (t->blop) korb_blop_cancel(vm, t->blop, -ECANCELED);   /* post が unpark する */
        else { t->state = KORB_TH_READY; korb_thread_runq_push(vm, t); }   /* stop / join 待ち */
    }
    return RESULT_OK(KORB_NIL);
}

/* handle_interrupt のマスク: 0 immediate / 1 on_blocking / 2 never。int_masks は
 * [klass, sym, ..., n] のフレーム列 (内側が末尾)。内側のフレームから見て最初に
 * 一致した klass が効く。kill マーカは CRuby と同じく Integer として照合する。 */
static int
korb_thread_int_mask_of(CTX *c, const struct korb_thread *t, VALUE exc)
{
    if (t->int_masks == KORB_NIL) return 0;
    const KorbArray *const ma = VAL2ARY(t->int_masks);
    const VALUE *const d = korb_items_data(ma->items);
    const VALUE intcls = korb_const_get(c->vm, korb_intern(c->vm, "Integer", 7));
    const uint32_t s_imm = korb_intern(c->vm, "immediate", 9), s_blk = korb_intern(c->vm, "on_blocking", 11);
    uint32_t i = ma->len;
    while (i > 0) {
        const uint32_t n = (uint32_t)FIX2LONG(d[i - 1]);
        const uint32_t base = i - 1 - 2 * n;
        for (uint32_t k = 0; k < n; k++) {
            const VALUE klass = d[base + 2 * k];
            const bool hit = exc == KORB_FALSE ? (KORB_CLASS_P(intcls) && KORB_CLASS_P(klass) && korb_class_le(intcls, klass))
                                               : korb_exc_matches(c, exc, klass);
            if (!hit) continue;
            const uint32_t sym = (uint32_t)SYM2ID(d[base + 2 * k + 1]);
            return sym == s_imm ? 0 : sym == s_blk ? 1 : 2;
        }
        i = base;
    }
    return 0;
}

/* 配送点: マスクが許す最初の pending を取り出して RAISE で返す (無ければ NORMAL)。
 * blocking = この check 点が blocking 操作か (:on_blocking はそこでだけ配送)。 */
static RESULT
korb_thread_check_ints_at(CTX *c, VALUE *slots, bool blocking)
{
    /* Pending OS signals are delivered here too: they are blocked process-wide
     * (process.c) and only become visible when reaped at a check point. */
    CHECK(korb_signal_deliver(c, slots));
    struct korb_thread *const cur = c->vm->cur_thread;
    if (cur == NULL || cur->pending_ints == KORB_NIL) return RESULT_OK(KORB_NIL);
    if (cur->defer_ints) return RESULT_OK(KORB_NIL);   /* __defer_ints 区間 (旧経路) */
    KorbArray *const pa = VAL2ARY(cur->pending_ints);
    for (uint32_t i = 0; i < pa->len; i++) {
        const VALUE exc = korb_items_data(pa->items)[i];
        const int m = korb_thread_int_mask_of(c, cur, exc);
        if (m == 2 || (m == 1 && !blocking)) continue;
        memmove(&korb_items_data(pa->items)[i], &korb_items_data(pa->items)[i + 1],   /* 要素移動のみ; 新 edge なし */
                (size_t)(pa->len - i - 1) * sizeof(VALUE));
        pa->len--;
        if (exc == KORB_FALSE) return korb_thread_kill_raise(c, slots);
        /* Thread#raise の配送: 対象 thread 側でも #exception を呼ぶ (CRuby は
         * rb_make_exception 経由なので、caller / target の両方で呼ばれる)。 */
        slots[0] = exc;
        const RESULT er = korb_send(c, slots + 1, korb_intern(c->vm, "exception", 9), 0, 0);
        if (UNLIKELY(er.state != KORB_NORMAL)) return er;
        return RESULT_RAISE_(KORB_EXC_P(er.value) ? er.value : slots[0]);
    }
    return RESULT_OK(KORB_NIL);
}
static RESULT korb_thread_check_ints(CTX *c, VALUE *slots) { return korb_thread_check_ints_at(c, slots, true); }
static RESULT korb_thread_check_ints_nb(CTX *c, VALUE *slots) { return korb_thread_check_ints_at(c, slots, false); }

/* Thread#__int_mask_push(hash) / #__int_mask_pop — handle_interrupt のマスク
 * フレーム (prelude が検証済みの {Class => sym} Hash)。 */
static RESULT
korb_m_thread_int_mask_push(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    slots[0] = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_HASH_P(slots[0])))
        return korb_raise(c, slots + 1, KORB_E_ARGUMENT, 0, "unknown mask signature");
    if (t->int_masks == KORB_NIL) t->int_masks = UNWRAP(korb_ary_new(c, slots + 1, 8));
    const uint32_t n = VAL2HASH(slots[0])->len;
    for (uint32_t i = 0; i < 2 * n; i++)                 /* Hash は slots[0] で rooted: 毎回読み直す */
        CHECK(korb_ary_push_val(c, slots + 1, VALUE_REF_AT(&t->int_masks), korb_items_data(VAL2HASH(slots[0])->items)[i]));
    CHECK(korb_ary_push_val(c, slots + 1, VALUE_REF_AT(&t->int_masks), LONG2FIX(n)));
    return RESULT_OK(KORB_NIL);
}

static RESULT
korb_m_thread_int_mask_pop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots; (void)a;
    struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    if (t->int_masks == KORB_NIL) return RESULT_OK(KORB_NIL);
    KorbArray *const ma = VAL2ARY(t->int_masks);
    if (ma->len == 0) return RESULT_OK(KORB_NIL);
    const uint32_t n = (uint32_t)FIX2LONG(korb_items_data(ma->items)[ma->len - 1]);
    ma->len -= 2 * n + 1;
    return RESULT_OK(KORB_NIL);
}

/* Thread.__check_ints — 非 blocking の配送点 (handle_interrupt の入口 / 出口) */
static RESULT
korb_m_thread_s_check_ints(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)self; (void)a;
    korb_thread_boot(c);
    return korb_thread_check_ints_nb(c, slots);
}

/* ---- korb_blop_wait: 唯一の suspension point ------------------------------ */
/* b を engine に登録し、完了 (F_DONE) まで現 thread を park する。戻ったら
 * b->result 確定。green thread を眠らせるだけで native thread はブロックしない
 * (runnable 皆無時の pump 内を除く)。may-GC (park 中に他 thread が alloc)。 */
static RESULT
korb_blop_wait(CTX *c, VALUE *slots, struct korb_blop *b)
{
    struct korb_vm *const vm = c->vm;
    korb_thread_boot(c);
    b->waiter = vm->cur_thread;
    for (;;) {
        korb_blop_prep(vm, b);
        while (!(b->flags & KORB_BLOP_F_DONE)) {
            struct korb_thread *const cur = vm->cur_thread;
            cur->state = KORB_TH_PENDED; cur->blop = b;
            RESULT r = korb_thread_yield_cpu(c, slots);
            cur->blop = NULL;
            if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        }
        const RESULT r = korb_thread_check_ints(c, slots);   /* 割り込み配送点 (Thread#raise/#kill/signal) */
        /* -EINTR は signal で起こされただけ。handler が戻ったのなら CRuby と
           同じく操作を続行する (sleep も read も signal では打ち切らない)。 */
        if (LIKELY(b->result != -EINTR) || UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
}

/* Kernel#sleep([sec]) — TIMER blop。他の green thread はその間走れる。
 * 戻り値は眠った秒数 (CRuby 同様、丸めた Integer)。引数なし = 無期限。 */
static RESULT
korb_bi_sleep(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    double sec = -1.0;                              /* forever */
    if (VALUE_SLICE_LEN(args) >= 1 && VALUE_SLICE_GET(args, 0) != KORB_NIL)
        CHECK(korb_thread_tmo_arg(c, slots, VALUE_SLICE_GET(args, 0), &sec));   /* validates negative / NaN */
    /* A Fiber scheduler takes over sleeping for a NON-blocking fiber (CRuby):
     * `scheduler.kernel_sleep(*args)`, with the duration passed through as
     * written.  The root fiber and a `blocking: true` one keep the timer. */
    if (UNLIKELY(c->vm->running_fiber != NULL && !c->vm->running_fiber->blocking)) {
        const VALUE fibc = korb_const_get(c->vm, korb_intern(c->vm, "Fiber", 5));
        const VALUE sched = KORB_CLASS_P(fibc)
            ? korb_class_ivar_get(fibc, ID2SYM(korb_intern(c->vm, "@__scheduler", 12))) : KORB_NIL;
        if (sched != KORB_NIL) {
            const uint32_t n = VALUE_SLICE_LEN(args);
            slots[0] = sched;
            for (uint32_t i = 0; i < n; i++) slots[1 + i] = VALUE_SLICE_GET(args, i);
            return korb_send(c, slots + 1 + n, korb_intern(c->vm, "kernel_sleep", 12), 0, n);
        }
    }
    struct korb_blop b; memset(&b, 0, sizeof b);
    b.kind = KORB_BLOP_TIMER;
    if (sec >= 0) korb_blop_deadline_in(&b, sec);
    struct timespec t0; korb_blop_now(&t0);
    CHECK(korb_blop_wait(c, slots, &b));
    struct timespec t1; korb_blop_now(&t1);
    long slept = (long)(t1.tv_sec - t0.tv_sec);
    const long nd = t1.tv_nsec - t0.tv_nsec;
    if (nd > 500000000L) slept++; else if (nd < -500000000L) slept--;
    return RESULT_OK(LONG2FIX(slept < 0 ? 0 : slept));
}

/* ---- thread body (runs on the thread's own stacks) ----------------------- */
static RESULT korb_m_thread_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a);   /* fwd (defined below) */
static VALUE  korb_out_target(CTX *c, const char *name, uint32_t len, bool *is_default);   /* fwd (korb_runtime.c) */
static RESULT korb_out_emit(CTX *c, VALUE *slots, VALUE out, uint32_t stdidx, const char *data, size_t len);   /* fwd */
static void   korb_report_uncaught_fp(CTX *c, FILE *fp, VALUE exc, bool synth_main, const char *dm);   /* fwd (korb_runtime.c) */
/* report_on_exception: CRuby prefixes the thread's own inspect line and sends the
 * whole thing through $stderr (a test harness replaces it), so render to memory
 * first.  The exception is read back from t->exc — building the inspect String
 * allocates, so a C local would go stale. */
static void
korb_thread_report(CTX *c, VALUE *base, struct korb_thread *t)
{
    base[0] = t->thval;
    const RESULT ts = korb_m_thread_to_s(c, base + 1, VALUE_REF_AT(&base[0]), VALUE_SLICE_MAKE(NULL, 0));
    char *rbuf = NULL; size_t rlen = 0;
    FILE *const ms = open_memstream(&rbuf, &rlen);
    if (ms == NULL) return;
    if (ts.state == KORB_NORMAL && KORB_STRING_P(ts.value))
        fprintf(ms, "%.*s terminated with exception (report_on_exception is true):\n",
                (int)VAL2STR(ts.value)->len, korb_strbuf_data(VAL2STR(ts.value)->buf));
    korb_report_uncaught_fp(c, ms, t->exc, false, NULL);   /* no '<main>' closer: a thread's bottom is its block */
    fclose(ms);
    bool def;
    const VALUE out = korb_out_target(c, "$stderr", 7, &def);
    if (def) { if (rlen) (void)fwrite(rbuf, 1, rlen, stderr); }
    else     { (void)korb_out_emit(c, base, out, 2, rbuf ? rbuf : "", rlen); }
    free(rbuf);
}

static void
korb_thread_trampoline(unsigned hi, unsigned lo)
{
    CTX *const c = (CTX *)(uintptr_t)(((uint64_t)hi << 32) | (uint64_t)lo);
    struct korb_thread *const t = c->vm->cur_thread;
    t->started = 1;
    korb_sigmask_resume(c->vm, c->vm->sigmask_gen - 1);   /* the context was built with an older mask */
    korb_thread_reap_pending(c->vm);
    /* body Proc を自分の (scan される) value stack で proc.call(*args) する。
     * recv = base[0], args = base[1..n] → korb_send(base+1+n, :call, n)。 */
    VALUE *const base = c->slots;
    base[0] = t->blk;
    uint32_t n = 0;
    if (t->args != KORB_NIL) {
        const KorbArray *aa = VAL2ARY(t->args);
        n = aa->len;
        for (uint32_t i = 0; i < n; i++) base[1 + i] = korb_items_data(aa->items)[i];   /* no alloc in this loop */
        t->args = KORB_NIL;
    }
    RESULT r = korb_send(c, base + 1 + n, korb_intern(c->vm, "call", 4), 0, n);
    if (r.state == KORB_RAISE) {
        if (r.value == KORB_THREAD_KILL) {
            t->result = KORB_NIL;             /* #kill: 正常終了扱い (join は self を返す) */
        } else {
            t->raised = 1; t->exc = r.value;
            /* SystemExit ends the whole program, so it is delivered to the main
             * thread even without abort_on_exception (CRuby). */
            const bool sysexit = korb_system_exit_status(c, r.value) >= 0;
            if (t->roe && !sysexit) korb_thread_report(c, base, t);   /* report BEFORE the hand-off (CRuby) */
            if (t->aoe || c->vm->thread_aoe_global || sysexit) {   /* abort_on_exception: main へ転送 */
                if (c->vm->main_thread != t)
                    (void)korb_thread_interrupt(c, c->slots, c->vm->main_thread, t->exc);
            }
        }
    } else {
        t->result = r.value;                  /* NORMAL (break/return unwinds folded in M1) */
    }
    t->state = KORB_TH_DEAD;
    korb_thread_joiners_wake(c->vm, t);
    t->saved_hw = c->slots_high_water;        /* how much of the value stack a reuse must clear */
    c->vm->reap_thread = t;                   /* this stack is still in use: the next thread frees it */
    korb_thread_exit_switch(c);               /* never returns */
}

/* ---- Thread.new ----------------------------------------------------------- */
/* 未初期化 thread (rep + handle のみ; stacks なし・未 queue)。blk==NIL が未初期化の印。
 * Thread subclass の .new が user #initialize を走らせる前段。 */
static RESULT
korb_thread_alloc_handle(CTX *c, VALUE *slots)
{
    korb_thread_boot(c);
    struct korb_thread *t = calloc(1, sizeof *t);
    if (!t) { fprintf(stderr, "koruby_precise: oom (thread rep)\n"); abort(); }
    t->thval = KORB_NIL; t->captured_self = KORB_NIL; t->args = KORB_NIL; t->blk = KORB_NIL;
    t->result = KORB_NIL; t->exc = KORB_NIL; t->tls = KORB_NIL; t->tvars = KORB_NIL;
    t->tl_gv[0] = t->tl_gv[1] = t->tl_gv[2] = KORB_NIL;   /* a new thread's block starts with no $~ (CRuby) */
    t->name = KORB_NIL; t->pending_ints = KORB_NIL; t->int_masks = KORB_NIL;
    t->tgroup = c->vm->cur_thread ? c->vm->cur_thread->tgroup : KORB_NIL;   /* 親 group を継承 (CRuby) */
    t->roe = !c->vm->thread_roe_off;         /* a new thread takes the class-level default (CRuby) */
    t->state = KORB_TH_DEAD;                 /* 初期化まで scheduler から不可視 (queue にも入れない) */
    t->next = c->vm->thread_list; c->vm->thread_list = t;
    CHECK(korb_thread_handle(c, slots, t));
    return RESULT_OK(t->thval);
}

/* Thread#initialize の実体: body/args を束ね、stacks を作って READY に。
 * Thread class obj 上の cfn としても登録され、subclass の super がここに届く。 */
static RESULT
korb_thread_init_body(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                      NODE *block, VALUE *def_env, VALUE *captured_self)
{
    struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    if (UNLIKELY(t->blk != KORB_NIL || t->started))
        return korb_raise_thread_error(c, slots, "already initialized thread");
    if (UNLIKELY(block == NULL))
        return korb_raise_thread_error(c, slots, "must be called with a block");
    const uint32_t n = VALUE_SLICE_LEN(a);
    slots[0] = UNWRAP(korb_ary_new(c, slots, n ? n : 1));   /* args (rooted) */
    { VALUE_REF ar = VALUE_REF_AT(&slots[0]);
      for (uint32_t i = 0; i < n; i++) CHECK(korb_ary_push_val(c, slots + 1, ar, VALUE_SLICE_GET(a, i))); }
    /* body を Proc に close する: 作成フレームが thread 実行前に死ぬのが普通
     * (n.times { Thread.new {…} }) なので、生の def_env は保持できない。 */
    /* &blk / &method(:m) / &sym: どちらも既に Proc が captured_self にある */
    if (def_env == KORB_BLK_FWD || block == KORB_BLK_CPROC) slots[1] = KORB_CSELF_VAL(captured_self);
    else slots[1] = UNWRAP(korb_make_proc(c, slots + 1, block, def_env, KORB_CSELF_VAL(captured_self), 0));
    t->args = slots[0];
    t->blk = slots[1];
    korb_scope_stacks_init(c, &t->ss);
    /* the new thread's root fiber starts with a COPY of the creator fiber's
     * storage (CRuby: Fiber.current.storage is inherited across Thread.new) */
    { const KorbFiberRep *const cf = c->vm->running_fiber ? c->vm->running_fiber : c->vm->root_fiber;
      if (cf != NULL && cf->storage != KORB_NIL && VAL2HASH(cf->storage)->len > 0) {
          slots[2] = cf->storage;
          const RESULT dr = korb_send(c, slots + 3, korb_intern(c->vm, "dup", 3), 0, 0);
          if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
          slots[2] = dr.value;
          t->root_fiber = korb_fiber_root_rep_alloc(c, slots + 3);
          t->root_fiber->storage = slots[2];
      } }
    void *vs;
    if (c->vm->thread_stack_pool_n > 0) {     /* a dead thread's: clear what it used, as a fresh map would be */
        const uint32_t n = --c->vm->thread_stack_pool_n;
        VALUE *const pv = c->vm->thread_stack_pool[n].vslots;
        VALUE *const hw = c->vm->thread_stack_pool[n].hw;
        vs = pv - 4;
        memset(vs, 0, (size_t)((hw > pv ? hw : pv) - (VALUE *)vs) * sizeof(VALUE));
        t->cstack = c->vm->thread_stack_pool[n].cstack;
        t->uctx = c->vm->thread_stack_pool[n].uctx;
        memset(t->uctx, 0, sizeof(ucontext_t));
    } else {
        vs = mmap(NULL, KORB_FIBER_VSLOTS_BYTES, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (vs == MAP_FAILED) { perror("koruby_precise: mmap thread vslots"); abort(); }
        t->cstack = malloc(KORB_FIBER_CSTACK_BYTES);
        if (!t->cstack) abort();
        t->uctx = calloc(1, sizeof(ucontext_t));
        if (!t->uctx) abort();
    }
    t->vslots = (VALUE *)vs + 4;              /* bottom-header slack (base[-1]=self, [-2]=EP, [-3]=link, [-4]=identity) */
    t->vslots[-1] = 0; t->vslots[-2] = 0; t->vslots[-3] = 0; t->vslots[-4] = 0;
    t->vslots_limit = (VALUE *)vs + KORB_FIBER_VSLOTS_BYTES / sizeof(VALUE) - KORB_FIBER_VSLOTS_MARGIN;
    getcontext((ucontext_t *)t->uctx);
    ((ucontext_t *)t->uctx)->uc_stack.ss_sp = t->cstack;
    ((ucontext_t *)t->uctx)->uc_stack.ss_size = KORB_FIBER_CSTACK_BYTES;
    ((ucontext_t *)t->uctx)->uc_link = NULL;
    makecontext((ucontext_t *)t->uctx, (void (*)(void))korb_thread_trampoline, 2,
                (unsigned)((uint64_t)(uintptr_t)c >> 32), (unsigned)((uintptr_t)c & 0xFFFFFFFFu));
    t->state = KORB_TH_READY;
    korb_thread_runq_push(c->vm, t);
    return RESULT_OK(VALUE_REF_GET(self));
}

/* Thread.new / Thread.start (exact Thread): alloc + init 一体 (initialize は経由しない) */
static RESULT
korb_thread_s_new(CTX *c, VALUE *slots, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self)
{
    slots[0] = UNWRAP(korb_thread_alloc_handle(c, slots + 1));
    CHECK(korb_thread_init_body(c, slots + 1, VALUE_REF_AT(&slots[0]), a, block, def_env, captured_self));
    return RESULT_OK(slots[0]);
}

/* Thread.start/fork — subclass でも #initialize を呼ばない (CRuby 意味論) */
static RESULT
korb_m_thread_s_start(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                      NODE *block, VALUE *def_env, VALUE *cself)
{
    /* CRuby: .start / .fork は Proc を作るところで落ちるので ArgumentError
     * (block 無しの .new が出す ThreadError とは別) */
    if (UNLIKELY(block == NULL))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "tried to create Proc object without a block");
    slots[0] = VALUE_REF_GET(self);                       /* class (root) */
    slots[1] = UNWRAP(korb_thread_s_new(c, slots + 2, a, block, def_env, cself));
    if (KORB_CLASS_P(slots[0]) &&
        VAL2CLASS(slots[0])->name_sym != c->vm->name_thread)
        korb_klass_override_set(c, slots[1], slots[0]);   /* subclass tag */
    return RESULT_OK(slots[1]);
}

/* ---- instance methods ------------------------------------------------------ */
static void
korb_thread_joiners_remove(struct korb_thread *target, struct korb_thread *w)
{
    struct korb_thread **pp = &target->joiners;
    while (*pp && *pp != w) pp = &(*pp)->join_next;
    if (*pp) { *pp = w->join_next; w->join_next = NULL; }
}

static RESULT
korb_m_thread_join(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    struct korb_vm *const vm = c->vm;
    double tmo = -1.0;
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        const VALUE v = VALUE_SLICE_GET(a, 0);
        if (FIXNUM_P(v)) tmo = (double)FIX2LONG(v);
        else if (KORB_FLOAT_P(v)) tmo = korb_float_val(v);
        else return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into time interval", korb_coerce_name(c, v));
    }
    korb_thread_boot(c);
    if (UNLIKELY(!t->started && t->blk == KORB_NIL))
        return korb_raise_thread_error(c, slots, "uninitialized thread");
    if (UNLIKELY(t == vm->cur_thread))
        return korb_raise_thread_error(c, slots, "Target thread must not be current thread");
    while (t->state != KORB_TH_DEAD) {
        struct korb_thread *const cur = vm->cur_thread;
        struct korb_blop tb;                        /* join(timeout): TIMER blop を併走 */
        if (tmo >= 0) {
            memset(&tb, 0, sizeof tb);
            tb.kind = KORB_BLOP_TIMER; tb.waiter = cur;
            korb_blop_deadline_in(&tb, tmo);
            korb_blop_prep(vm, &tb);
        }
        cur->state = KORB_TH_PENDED;
        cur->join_next = t->joiners; t->joiners = cur;
        RESULT r = korb_thread_yield_cpu(c, slots);
        korb_thread_joiners_remove(t, cur);   /* 毎周入れ直す: 残すと多重登録になる */
        if (tmo >= 0) {
            if (!(tb.flags & KORB_BLOP_F_DONE))
                korb_blop_cancel(vm, &tb, -ECANCELED);      /* 死亡側が先: timer を回収 */
            else if (t->state != KORB_TH_DEAD) {            /* timeout 側が先 */
                korb_thread_joiners_remove(t, vm->cur_thread);
                RESULT ci = korb_thread_check_ints(c, slots);
                if (UNLIKELY(ci.state != KORB_NORMAL)) return ci;
                return RESULT_OK(KORB_NIL);                 /* CRuby: join(tmo) 失敗 = nil */
            }
        }
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        RESULT ci = korb_thread_check_ints(c, slots);       /* 割り込みで起こされた? */
        if (UNLIKELY(ci.state != KORB_NORMAL)) {
            korb_thread_joiners_remove(t, vm->cur_thread);  /* 登録を残さない */
            return ci;
        }
    }
    if (t->raised) return RESULT_RAISE_(t->exc);   /* CRuby: join は死因の例外を再 raise */
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT
korb_m_thread_value(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)a;
    RESULT r = korb_m_thread_join(c, slots, self, VALUE_SLICE_MAKE(slots, 0));
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(VAL2THREAD(VALUE_REF_GET(self))->rep->result);
}

static RESULT
korb_m_thread_alive(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots; (void)a;
    return RESULT_OK(VAL2THREAD(VALUE_REF_GET(self))->rep->state != KORB_TH_DEAD ? KORB_TRUE : KORB_FALSE);
}

static RESULT
korb_m_thread_status(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)a;
    const struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    if (t->state == KORB_TH_DEAD) return RESULT_OK(t->raised ? KORB_NIL : KORB_FALSE);
    if (t->state == KORB_TH_PENDED) return korb_str_new(c, slots, "sleep", 5);
    if (t->aborting) return korb_str_new(c, slots, "aborting", 8);   /* #kill 済みで ensure 走行中 */
    return korb_str_new(c, slots, "run", 3);
}

static RESULT
korb_m_thread_name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots; (void)a;
    return RESULT_OK(VAL2THREAD(VALUE_REF_GET(self))->rep->name);
}

static RESULT
korb_m_thread_name_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    VALUE v = VALUE_SLICE_GET(a, 0);
    if (v != KORB_NIL) {
        if (!KORB_STRING_P(v)) {
            const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
            if (KORB_OBJECT_P(v) && korb_responds_to_coerce_p(c, slots, &v, to_str)) {
                slots[0] = v;
                RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
                if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
                if (KORB_STRING_P(sr.value)) v = sr.value;
            }
            if (UNLIKELY(!KORB_STRING_P(v)))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_coerce_name(c, VALUE_SLICE_GET(a, 0)));
        }
        const KorbString *const ns = VAL2STR(v);
        if (UNLIKELY(memchr(korb_strbuf_data(ns->buf), 0, ns->len) != NULL))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "string contains null byte");
    }
    VAL2THREAD(VALUE_REF_GET(self))->rep->name = v;   /* rep は libc: barrier 不要 (root visit が forward) */
    return RESULT_OK(v);
}

/* #[] / #[]= / #key? — TLS (CRuby: fiber-local だが Phase 1 は thread-local 一枚)。
 * key は Symbol / String / #to_str。他は TypeError。 */
static RESULT
korb_thread_tls_key(CTX *c, VALUE *slots, VALUE k, VALUE *out)
{
    if (SYMBOL_P(k)) { *out = k; return RESULT_OK(KORB_NIL); }
    if (!KORB_STRING_P(k)) {
        const uint32_t to_str = korb_intern(c->vm, "to_str", 6);
        if (KORB_OBJECT_P(k) && korb_responds_to_coerce_p(c, slots, &k, to_str)) {
            slots[0] = k;
            RESULT sr = korb_send_impl(c, slots + 1, to_str, 0, 0, NULL, NULL, NULL);
            if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
            if (KORB_STRING_P(sr.value)) k = sr.value;
        }
        if (UNLIKELY(!KORB_STRING_P(k))) {            /* CRuby は inspect 形式で報告 */
            char kb[64]; kb[0] = '?'; kb[1] = 0;
            slots[0] = k;
            RESULT ir = korb_send(c, slots + 1, korb_intern(c->vm, "inspect", 7), 0, 0);
            if (ir.state == KORB_NORMAL && KORB_STRING_P(ir.value)) {
                const uint32_t il = VAL2STR(ir.value)->len < 60 ? VAL2STR(ir.value)->len : 60;
                memcpy(kb, korb_strbuf_data(VAL2STR(ir.value)->buf), il); kb[il] = 0;   /* raise (may-GC) 前に copy */
            }
            return korb_raise(c, slots, KORB_E_TYPE, 0, "%s is not a symbol nor a string", kb);
        }
    }
    *out = ID2SYM(korb_intern(c->vm, korb_strbuf_data(VAL2STR(k)->buf), VAL2STR(k)->len));
    return RESULT_OK(KORB_NIL);
}

/* 汎用 hash-slot ops (tls / tvars 共用; hp は rep 内の Hash root フィールド) */
static RESULT
korb_thread_hget(CTX *c, VALUE *slots, VALUE *hp, VALUE key)
{
    VALUE k; CHECK(korb_thread_tls_key(c, slots, key, &k));
    if (*hp == KORB_NIL) return RESULT_OK(KORB_NIL);
    const int32_t i = korb_hash_find(VAL2HASH(*hp), k);
    return RESULT_OK(i >= 0 ? korb_items_data(VAL2HASH(*hp)->items)[2 * i + 1] : KORB_NIL);
}

/* Drop `k` in place (keys are Symbols here, so no #eql? dispatch / no GC). */
static void
korb_thread_hdel(CTX *c, VALUE *hp, VALUE k)
{
    if (*hp == KORB_NIL) return;
    KorbHash *const h = VAL2HASH(*hp);
    const int32_t idx = korb_hash_find(h, k);
    if (idx < 0) return;
    KorbArrayItems *const it = h->items;
    for (uint32_t i = (uint32_t)idx; i + 1 < h->len; i++) {          /* shift to keep order */
        ARO_STORE(c, it, &korb_items_data(it)[2 * i],     korb_items_data(it)[2 * (i + 1)]);
        ARO_STORE(c, it, &korb_items_data(it)[2 * i + 1], korb_items_data(it)[2 * (i + 1) + 1]);
    }
    h->len--;
    ARO_STORE(c, it, &korb_items_data(it)[2 * h->len], KORB_NIL);
    ARO_STORE(c, it, &korb_items_data(it)[2 * h->len + 1], KORB_NIL);
    KORB_HASH_DROP_INDEX(h);
}

/* CRuby stores nil by REMOVING the key, so #key? / #thread_variable? go false. */
static RESULT
korb_thread_hset(CTX *c, VALUE *slots, VALUE *hp, VALUE key, VALUE val)
{
    slots[0] = val;
    VALUE k; CHECK(korb_thread_tls_key(c, slots + 1, key, &k));
    slots[1] = k;
    if (slots[0] == KORB_NIL) { korb_thread_hdel(c, hp, slots[1]); return RESULT_OK(KORB_NIL); }
    if (*hp == KORB_NIL) *hp = UNWRAP(korb_hash_new(c, slots + 2, 4));
    CHECK(korb_hash_set(c, slots + 2, VALUE_REF_AT(hp), VALUE_REF_AT(&slots[1]), slots[0]));
    return RESULT_OK(slots[0]);
}

/* Thread#[]= / #thread_variable_set on a frozen Thread (CRuby message). */
static RESULT
korb_thread_check_frozen(CTX *c, VALUE *slots, VALUE th)
{
    if (UNLIKELY(((const AroObjectHeader *)(uintptr_t)th)->flags & KORB_FL_FROZEN))
        return korb_raise(c, slots, KORB_E_FROZEN, 0, "can't modify frozen thread locals");
    return RESULT_OK(KORB_NIL);
}

static RESULT
korb_thread_hkey_p(CTX *c, VALUE *slots, VALUE *hp, VALUE key)
{
    VALUE k; CHECK(korb_thread_tls_key(c, slots, key, &k));
    if (*hp == KORB_NIL) return RESULT_OK(KORB_FALSE);
    return RESULT_OK(korb_hash_find(VAL2HASH(*hp), k) >= 0 ? KORB_TRUE : KORB_FALSE);
}

/* Thread#[] は CRuby では FIBER-local: 走っている fiber がある間はその fiber の
 * 表を使う (root fiber の分は thread rep の tls)。receiver が他 thread のときは
 * その thread の root 表 — 他 thread の fiber を覗く手段は無い。表の置き場は
 * どちらも libc-stable な rep 内なので、ポインタとして持ち回してよい。 */
static VALUE *korb_thread_tls_slot(CTX *c, VALUE thv)
{
    struct korb_thread *const t = VAL2THREAD(thv)->rep;
    if (t == c->vm->cur_thread && c->vm->running_fiber != NULL)
        return &c->vm->running_fiber->tls;
    return &t->tls;
}

static RESULT
korb_m_thread_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{ return korb_thread_hget(c, slots, korb_thread_tls_slot(c, VALUE_REF_GET(self)), VALUE_SLICE_GET(a, 0)); }

static RESULT
korb_m_thread_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{ CHECK(korb_thread_check_frozen(c, slots, VALUE_REF_GET(self)));
  return korb_thread_hset(c, slots, korb_thread_tls_slot(c, VALUE_REF_GET(self)), VALUE_SLICE_GET(a, 0), VALUE_SLICE_GET(a, 1)); }

static RESULT
korb_m_thread_key_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{ return korb_thread_hkey_p(c, slots, korb_thread_tls_slot(c, VALUE_REF_GET(self)), VALUE_SLICE_GET(a, 0)); }

/* thread_variable_* — CRuby では fiber-local (#[]) と別空間 */
static RESULT
korb_m_thread_tvar_get(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{ return korb_thread_hget(c, slots, &VAL2THREAD(VALUE_REF_GET(self))->rep->tvars, VALUE_SLICE_GET(a, 0)); }

static RESULT
korb_m_thread_tvar_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{ CHECK(korb_thread_check_frozen(c, slots, VALUE_REF_GET(self)));
  return korb_thread_hset(c, slots, &VAL2THREAD(VALUE_REF_GET(self))->rep->tvars, VALUE_SLICE_GET(a, 0), VALUE_SLICE_GET(a, 1)); }

static RESULT
korb_m_thread_tvar_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{ return korb_thread_hkey_p(c, slots, &VAL2THREAD(VALUE_REF_GET(self))->rep->tvars, VALUE_SLICE_GET(a, 0)); }

/* #kill / #exit / #terminate — 未起動は即取り消し、自分なら即 unwind、
 * 実行済みの他 thread へは pending interrupt (KORB_FALSE マーカ) で配送。 */
static RESULT
korb_m_thread_kill(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)a;
    struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    korb_thread_boot(c);
    if (t->state == KORB_TH_DEAD) return RESULT_OK(VALUE_REF_GET(self));
    if (!t->started && t->state == KORB_TH_READY) {    /* 未起動: 走らせず葬る */
        t->state = KORB_TH_DEAD;                       /* runq からは pop 時に skip */
        korb_thread_joiners_wake(c->vm, t);
        korb_thread_free_stacks(c->vm, t, t->vslots);  /* never ran on them */
        return RESULT_OK(VALUE_REF_GET(self));
    }
    if (t == c->vm->cur_thread) return korb_thread_kill_raise(c, slots);   /* 自殺: 即 unwind */
    CHECK(korb_thread_interrupt(c, slots, t, KORB_FALSE));
    return RESULT_OK(VALUE_REF_GET(self));
}

/* #raise(exc_class_or_instance_or_msg[, msg]) — 対象 thread に例外を配送。 */
static RESULT
korb_m_thread_raise(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    korb_thread_boot(c);
    /* 例外オブジェクトの組み立ては Kernel#raise と共通の builder (cause: kwarg
     * 込み)。bare `Thread#raise` は $! 再送出でなく RuntimeError("") (CRuby)。 */
    if (VALUE_SLICE_LEN(a) == 0) {
        RESULT r = korb_raise(c, slots + 1, KORB_E_RUNTIME, 0, "%s", "");   /* CRuby: message は "" */
        slots[0] = r.value;
    } else {
        RESULT r = korb_exc_build_with_cause(c, slots, a);
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        slots[0] = r.value;
    }
    if (t->state == KORB_TH_DEAD) return RESULT_OK(KORB_NIL);   /* CRuby: dead へは無視 */
    if (t == c->vm->cur_thread) return RESULT_RAISE_(slots[0]); /* 自分: 即 raise */
    CHECK(korb_thread_interrupt(c, slots + 1, t, slots[0]));
    return RESULT_OK(KORB_NIL);
}

/* #wakeup / #run — PENDED を起こす (sleep は中断され早期 return、stop は解除)。 */
static RESULT
korb_m_thread_wakeup(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)a;
    struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    if (t->state == KORB_TH_DEAD)
        return korb_raise_thread_error(c, slots, "killed thread");
    if (t->state == KORB_TH_PENDED) {
        if (t->blop) korb_blop_cancel(c->vm, t->blop, -ECANCELED);   /* sleep 等を中断 */
        else { t->state = KORB_TH_READY; korb_thread_runq_push(c->vm, t); }
    }
    return RESULT_OK(VALUE_REF_GET(self));
}

/* Thread.stop — #wakeup / 割り込みまで park。 */
static RESULT
korb_m_thread_s_stop(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)self; (void)a;
    struct korb_vm *const vm = c->vm;
    korb_thread_boot(c);
    if (vm->runq_head == NULL && vm->blop_npending == 0)
        return korb_raise_thread_error(c, slots, "stopping only thread");
    struct korb_thread *const cur = vm->cur_thread;
    cur->state = KORB_TH_PENDED;                       /* blop なしの素の park */
    CHECK(korb_thread_yield_cpu(c, slots));
    return korb_thread_check_ints(c, slots);
}

/* ---- class methods --------------------------------------------------------- */
static RESULT
korb_m_thread_s_current(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)self; (void)a;
    return korb_thread_handle(c, slots, korb_thread_boot(c));
}

static RESULT
korb_m_thread_s_main(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)self; (void)a;
    korb_thread_boot(c);
    return korb_thread_handle(c, slots, c->vm->main_thread);
}

/* Preemption point for a pure-Ruby loop.  Green threads are cooperative, so a
 * `while true; end` starves every other thread: Thread#raise / #kill never get
 * delivered and a sleeping thread's timer never fires.  The loop nodes call
 * this every so often; it is a no-op unless somebody is actually waiting. */
bool korb_loop_wants_yield(const CTX *c) {
    const struct korb_vm *const vm = c->vm;
    if (vm->cur_thread == NULL) return false;
    if (vm->runq_head != NULL || vm->blop_npending != 0) return true;
    const VALUE pi = vm->cur_thread->pending_ints;
    return pi != KORB_NIL && VAL2ARY(pi)->len != 0;
}
RESULT korb_loop_yield(CTX *c, VALUE *slots) {
    struct korb_vm *const vm = c->vm;
    if (vm->cur_thread == NULL) return RESULT_OK(KORB_NIL);
    /* somebody waits on a timer or an fd: a spinning loop is the one place that
     * never reaches the blocking pump, so collect what is ready here */
    korb_blop_pump_at_yield(vm);
    /* CRuby's timeslice: a thread keeps the CPU for its quantum, so a new thread
     * does not run until its creator blocks or 100ms pass (code races on it) */
    if (vm->runq_head != NULL && korb_mono_coarse_ns() - vm->slice_start_ns >= KORB_TIME_QUANTUM_NS) {
        struct korb_thread *const cur = vm->cur_thread;
        cur->state = KORB_TH_READY;
        korb_thread_runq_push(vm, cur);
        CHECK(korb_thread_yield_cpu(c, slots));
    }
    return korb_thread_check_ints_nb(c, slots);
}

static RESULT
korb_m_thread_s_pass(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)self; (void)a;
    struct korb_vm *const vm = c->vm;
    if (vm->cur_thread == NULL) return RESULT_OK(KORB_NIL);
    /* `Thread.pass while cond` を回す thread が何本いても、blop 待ちの thread を
       飢えさせないよう engine を回す (CRuby の thread は native なので進む)。 */
    korb_blop_pump_at_yield(vm);
    if (vm->runq_head == NULL) return korb_thread_check_ints_nb(c, slots);
    struct korb_thread *const cur = vm->cur_thread;
    cur->state = KORB_TH_READY;
    korb_thread_runq_push(vm, cur);
    CHECK(korb_thread_yield_cpu(c, slots));
    return korb_thread_check_ints_nb(c, slots);
}

static RESULT
korb_m_thread_s_list(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    korb_thread_boot(c);
    /* 協調 scheduler の観測点: CRuby では任意の呼び出しで preempt され得るので、
     * list の度に一回 yield しても意味論上不可視。`while t.alive?; Thread.list; end`
     * 型の busy-poll (preemption 前提の spec 頻出) がこれで前進する。 */
    CHECK(korb_m_thread_s_pass(c, slots, self, a));
    (void)self; (void)a;
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (struct korb_thread *t = c->vm->thread_list; t; t = t->next) {
        if (t->state == KORB_TH_DEAD) continue;
        slots[1] = UNWRAP(korb_thread_handle(c, slots + 1, t));
        CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

/* ---- Thread 小物 ----------------------------------------------------------- */
static RESULT
korb_m_thread_stop_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots; (void)a;
    const struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    const uint8_t st = t->state;
    return RESULT_OK((st == KORB_TH_PENDED || st == KORB_TH_DEAD || t->blocked_in != NULL)
                     ? KORB_TRUE : KORB_FALSE);
}

/* #fetch(key[, default]) { |key| … } — TLS 版 Hash#fetch (KeyError あり) */
static RESULT
korb_m_thread_fetch(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                    NODE *block, VALUE *def_env, VALUE *cself)
{
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1 || VALUE_SLICE_LEN(a) > 2))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given %u, expected 1..2)",
                          (unsigned)VALUE_SLICE_LEN(a));
    if (UNLIKELY(block != NULL && VALUE_SLICE_LEN(a) >= 2))
        korb_warn(c, slots, "block supersedes default value argument");
    VALUE k; CHECK(korb_thread_tls_key(c, slots, VALUE_SLICE_GET(a, 0), &k));
    const VALUE *const hp = korb_thread_tls_slot(c, VALUE_REF_GET(self));
    if (*hp != KORB_NIL) {
        const int32_t i = korb_hash_find(VAL2HASH(*hp), k);
        if (i >= 0) return RESULT_OK(korb_items_data(VAL2HASH(*hp)->items)[2 * i + 1]);
    }
    if (block != NULL) {
        slots[0] = k;
        return korb_block_yield(c, slots + 1, block, def_env, &slots[0], 1, cself);
    }
    if (VALUE_SLICE_LEN(a) >= 2) return RESULT_OK(VALUE_SLICE_GET(a, 1));
    return korb_raise(c, slots, KORB_E_KEY, 0, "key not found");
}

static RESULT
korb_thread_hkeys(CTX *c, VALUE *slots, const VALUE *hp)
{
    const uint32_t n = (*hp == KORB_NIL) ? 0 : VAL2HASH(*hp)->len;
    slots[0] = UNWRAP(korb_ary_new(c, slots, n ? n : 1));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (uint32_t i = 0; i < n; i++) {
        slots[1] = korb_items_data(VAL2HASH(*hp)->items)[2 * i];   /* hp は libc rep 内: 再読可 */
        CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}

static RESULT
korb_m_thread_keys(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{ (void)a; return korb_thread_hkeys(c, slots, korb_thread_tls_slot(c, VALUE_REF_GET(self))); }

static RESULT
korb_m_thread_tvars(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{ (void)a; return korb_thread_hkeys(c, slots, &VAL2THREAD(VALUE_REF_GET(self))->rep->tvars); }

static RESULT
korb_m_thread_priority(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots; (void)a;
    return RESULT_OK(LONG2FIX(VAL2THREAD(VALUE_REF_GET(self))->rep->priority));
}

static RESULT
korb_m_thread_priority_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    const VALUE v = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(v)))
        return korb_raise_no_int(c, slots, v);
    korb_sword_t p = FIX2LONG(v);                      /* CRuby は -3..3 に clamp して保持 */
    if (p > 3) p = 3; else if (p < -3) p = -3;
    VAL2THREAD(VALUE_REF_GET(self))->rep->priority = (int)p;   /* 保持のみ (協調 scheduler) */
    return RESULT_OK(v);                               /* 代入式の値は clamp 前 */
}

static RESULT
korb_m_thread_aoe(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots; (void)a;
    return RESULT_OK(VAL2THREAD(VALUE_REF_GET(self))->rep->aoe ? KORB_TRUE : KORB_FALSE);
}

static RESULT
korb_m_thread_aoe_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots;
    VAL2THREAD(VALUE_REF_GET(self))->rep->aoe = KORB_TRUTHY(VALUE_SLICE_GET(a, 0)) ? 1 : 0;
    return RESULT_OK(VALUE_SLICE_GET(a, 0));
}

/* class-level (singleton 専用登録) */
static RESULT
korb_m_thread_s_aoe(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)slots; (void)self; (void)a;
    return RESULT_OK(c->vm->thread_aoe_global ? KORB_TRUE : KORB_FALSE);
}

static RESULT
korb_m_thread_s_aoe_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)slots; (void)self;
    c->vm->thread_aoe_global = KORB_TRUTHY(VALUE_SLICE_GET(a, 0)) ? 1 : 0;
    return RESULT_OK(VALUE_SLICE_GET(a, 0));
}

static RESULT
korb_m_thread_s_roe(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)slots; (void)self; (void)a;
    return RESULT_OK(c->vm->thread_roe_off ? KORB_FALSE : KORB_TRUE);
}

static RESULT
korb_m_thread_s_roe_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)slots; (void)self;
    c->vm->thread_roe_off = KORB_TRUTHY(VALUE_SLICE_GET(a, 0)) ? 0 : 1;
    return RESULT_OK(VALUE_SLICE_GET(a, 0));
}

static RESULT
korb_m_thread_roe(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots; (void)a;
    return RESULT_OK(VAL2THREAD(VALUE_REF_GET(self))->rep->roe ? KORB_TRUE : KORB_FALSE);
}

static RESULT
korb_m_thread_roe_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots;
    VAL2THREAD(VALUE_REF_GET(self))->rep->roe = KORB_TRUTHY(VALUE_SLICE_GET(a, 0)) ? 1 : 0;
    return RESULT_OK(VALUE_SLICE_GET(a, 0));
}

/* #backtrace / #backtrace_locations — stub: alive → 空配列 / dead → nil
 * (frame 情報の本物は将来; 型だけ CRuby に合わせる) */
static RESULT
korb_m_thread_backtrace(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)a;
    const struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    if (t->state == KORB_TH_DEAD) return RESULT_OK(KORB_NIL);
    /* A suspended thread always parked inside a C builtin (__check_ints or a
     * blocking primitive), so its saved cfunc link names the innermost frame and
     * the chain from there is walkable — in ITS stack, hence the explicit bounds. */
    if (t->saved_cfunc_link != 0 && t->saved_base != NULL) {
        const uint32_t d = korb_frames_snapshot_at(c, t->saved_base, t->vslots_limit,
                                                   korb_flink_ptr(t->saved_cfunc_link), 0, NULL);
        if (d > 0) return korb_caller_ary(c, slots, d, 0, d);
    }
    if (t->blocked_in == NULL) return korb_ary_new(c, slots, 1);
    /* Blocked in a C-level wait with no frame chain to walk: one synthetic frame
     * naming the method, so introspection like `bt.any? { |f| f.include?("require") }`
     * (rubyspec's concurrent-require fixture) can see where the thread sits. */
    char frame[128];
    snprintf(frame, sizeof frame, "<internal>:in '%s'", t->blocked_in);
    slots[0] = UNWRAP(korb_ary_new(c, slots + 1, 1));
    VALUE_REF arr = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_str_new(c, slots + 1, frame, (uint32_t)strlen(frame)));
    CHECK(korb_ary_push_val(c, slots + 2, arr, slots[1]));
    return RESULT_OK(VALUE_REF_GET(arr));
}

static RESULT
korb_m_thread_pending_interrupt_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots; (void)a;
    const VALUE recv = VALUE_REF_GET(self);
    const struct korb_thread *t;
    if (KORB_THREAD_P(recv)) t = VAL2THREAD(recv)->rep;
    else t = c->vm->cur_thread;                            /* Thread.pending_interrupt? (class 経由) */
    if (t == NULL || t->pending_ints == KORB_NIL) return RESULT_OK(KORB_FALSE);
    const KorbArray *const pa = VAL2ARY(t->pending_ints);
    if (VALUE_SLICE_LEN(a) >= 1) {                         /* pending_interrupt?(klass): その class のものだけ */
        const VALUE klass = VALUE_SLICE_GET(a, 0);
        if (UNLIKELY(!KORB_CLASS_P(klass)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "class or module required for rescue clause");
        for (uint32_t i = 0; i < pa->len; i++)
            if (korb_exc_matches(c, korb_items_data(pa->items)[i], klass)) return RESULT_OK(KORB_TRUE);
        return RESULT_OK(KORB_FALSE);
    }
    return RESULT_OK(pa->len > 0 ? KORB_TRUE : KORB_FALSE);
}

static RESULT
korb_m_thread_native_thread_id(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots; (void)a;
    const struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    /* M:N: native thread に載っているのは「今走っている」green thread だけ。
     * CRuby も M:N scheduler では載っていない thread に nil を返す。 */
    if (t->state == KORB_TH_DEAD || t != c->vm->cur_thread) return RESULT_OK(KORB_NIL);
    return RESULT_OK(LONG2FIX((long)gettid()));
}

/* ThreadGroup 連携 (本体は prelude の Ruby class; 所属だけ rep が持つ) */
static RESULT
korb_m_thread_group_raw(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots; (void)a;
    return RESULT_OK(VAL2THREAD(VALUE_REF_GET(self))->rep->tgroup);
}

static RESULT
korb_m_thread_group_set(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots;
    VAL2THREAD(VALUE_REF_GET(self))->rep->tgroup = VALUE_SLICE_GET(a, 0);   /* rep は libc: visit が forward */
    return RESULT_OK(VALUE_SLICE_GET(a, 0));
}

/* handle_interrupt(:never) の配送延期区間 (prelude が begin/end を呼ぶ) */
static RESULT
korb_m_thread_defer_begin(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)slots; (void)self; (void)a;
    korb_thread_boot(c)->defer_ints++;
    return RESULT_OK(KORB_NIL);
}

static RESULT
korb_m_thread_defer_end(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)self; (void)a;
    struct korb_thread *const cur = korb_thread_boot(c);
    if (cur->defer_ints) cur->defer_ints--;
    if (cur->defer_ints == 0) return korb_thread_check_ints(c, slots);   /* 区間終了で即配送 (CRuby) */
    return RESULT_OK(KORB_NIL);
}

/* #to_s / #inspect — "#<Thread:0x… name? status>" */
static RESULT
korb_m_thread_to_s(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)a;
    struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    const char *st = t->state == KORB_TH_DEAD ? "dead"
                   : t->state == KORB_TH_PENDED ? "sleep" : t->aborting ? "aborting" : "run";
    char buf[640]; char nb[64]; nb[0] = 0;
    int name_enc = -1;                                /* 非 ASCII の名前ならその encoding が伝播 (CRuby) */
    if (t->name != KORB_NIL && KORB_STRING_P(t->name)) {
        const uint32_t nl = VAL2STR(t->name)->len < 48 ? VAL2STR(t->name)->len : 48;
        memcpy(nb, korb_strbuf_data(VAL2STR(t->name)->buf), nl);
        nb[nl] = 0;
        for (uint32_t i = 0; i < nl; i++)
            if ((unsigned char)nb[i] >= 0x80) { name_enc = (int)KORB_STR_ENC(t->name); break; }
    }
    /* CRuby names the source location of the block the thread runs, when it has
     * one: "#<Thread:0x… file:line run>". */
    char loc[512]; loc[0] = 0;   /* file paths can be long */
    if (KORB_PROC_P(t->blk)) {
        const NODE *const body = VAL2PROC(t->blk)->iseq;
        uint32_t fsym, line;
        if (body != NULL && body != KORB_BLK_CPROC && korb_get_srcloc(c->vm, body, &fsym, &line))
            snprintf(loc, sizeof loc, " %s:%u", korb_sym_name(c->vm, fsym), line);
    }
    const int len = snprintf(buf, sizeof buf, "#<Thread:%p%s%s%s %s>",
                             (void *)t, nb[0] ? "@" : "", nb, loc, st);
    RESULT r = korb_str_new(c, slots, buf, (uint32_t)len);
    if (LIKELY(r.state == KORB_NORMAL))
        KORB_STR_ENC_SET(r.value, name_enc >= 0 ? (uint32_t)name_enc : KORB_ENC_BINARY);   /* CRuby: 既定は ASCII-8BIT */
    return r;
}

/* ==== Mutex / ConditionVariable — 純 green-thread プリミティブ ==============
 * OS lock 不要 (native 1 本 + 協調なので critical section は切替点まで自明)。
 * 待ちは blop なしの素の park。payload (KorbMutex/KorbCondVar) は可動なので
 * park 跨ぎでは必ず rooted VALUE_REF から再導出する。待ち行列リンクは
 * korb_thread.join_next 流用 (同時に 1 つしか待てない)。 */

static void
korb_waitq_push(struct korb_thread **head, struct korb_thread **tail, struct korb_thread *t)
{
    t->join_next = NULL;
    if (*tail) (*tail)->join_next = t; else *head = t;
    *tail = t;
}

static void
korb_waitq_remove(struct korb_thread **head, struct korb_thread **tail, struct korb_thread *t)
{
    struct korb_thread **pp = head, *prev = NULL;
    while (*pp && *pp != t) { prev = *pp; pp = &(*pp)->join_next; }
    if (*pp) {
        *pp = t->join_next;
        if (*tail == t) *tail = prev;
        t->join_next = NULL;
    }
}

/* 先頭から「まだ PENDED の」waiter を 1 人起こす (interrupt 済みの stale entry は捨てる)。
 * blop で眠っている waiter (CondVar#wait は Mutex#sleep 経由なのでこちら) は blop を
 * cancel しないと wait ループに戻ってしまうので、post して起こす。 */
static void
korb_waitq_wake_one(struct korb_vm *vm, struct korb_thread **head, struct korb_thread **tail)
{
    while (*head) {
        struct korb_thread *t = *head;
        *head = t->join_next;
        if (*head == NULL) *tail = NULL;
        t->join_next = NULL;
        if (t->state == KORB_TH_PENDED) {
            if (t->blop) korb_blop_cancel(vm, t->blop, -ECANCELED);   /* post が unpark する */
            else { t->state = KORB_TH_READY; korb_thread_runq_push(vm, t); }
            return;
        }
    }
}

/* 所有スレッドが死んだ mutex は「未 lock」扱い。CRuby は thread 終了時に
 * その thread が握っていた Mutex を全部解放する。rep は libc 側で回収されない
 * ので、死んだ owner ポインタを読むのは安全。 */
static struct korb_thread *korb_mutex_owner(const KorbMutex *m) {
    return (m->owner != NULL && m->owner->state == KORB_TH_DEAD) ? NULL : m->owner;
}

/* Mutex#lock の芯 (Mutex#sleep の再 lock からも使う)。self は rooted ref。
 * interruptible=false は CRuby の mutex_lock_uninterruptible 相当: Thread#kill /
 * #raise が来ていても先に lock を取り、割り込みは次の check 点まで持ち越す
 * (Mutex#sleep / CondVar#wait は「起きたら必ず lock を持っている」が仕様)。 */
static RESULT
korb_mutex_lock_core(CTX *c, VALUE *slots, VALUE_REF self, bool interruptible)
{
    struct korb_vm *const vm = c->vm;
    korb_thread_boot(c);
    if (UNLIKELY(interruptible && vm->cur_thread->in_trap))
        return korb_raise_thread_error(c, slots, "can't be called from trap context");
    for (;;) {
        KorbMutex *m = VAL2MUTEX(VALUE_REF_GET(self));      /* park 跨ぎ毎に再導出 */
        struct korb_thread *const cur = vm->cur_thread;
        if (korb_mutex_owner(m) == NULL) { m->owner = cur; m->owner_fiber = vm->running_fiber; return RESULT_OK(VALUE_REF_GET(self)); }
        if (UNLIKELY(m->owner == cur))
            return korb_raise_thread_error(c, slots, m->owner_fiber == vm->running_fiber
                                           ? "deadlock; recursive locking"
                                           : "deadlock; lock already owned by another fiber belonging to the same thread");
        korb_waitq_push(&m->wq_head, &m->wq_tail, cur);     /* ここから park まで alloc なし */
        cur->state = KORB_TH_PENDED;
        RESULT r = korb_thread_yield_cpu(c, slots);
        m = VAL2MUTEX(VALUE_REF_GET(self));                 /* GC で動いたかもしれない */
        korb_waitq_remove(&m->wq_head, &m->wq_tail, vm->cur_thread);   /* interrupt 起床なら残っている */
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
        if (interruptible) {
            RESULT ci = korb_thread_check_ints(c, slots);
            if (UNLIKELY(ci.state != KORB_NORMAL)) return ci;
        }
        /* unlock に起こされた: ループ先頭で再取得を試みる (他が先取りしたら再 wait) */
    }
}

static RESULT
korb_mutex_unlock_core(CTX *c, VALUE *slots, VALUE_REF self)
{
    struct korb_vm *const vm = c->vm;
    korb_thread_boot(c);
    KorbMutex *const m = VAL2MUTEX(VALUE_REF_GET(self));
    if (UNLIKELY(korb_mutex_owner(m) == NULL))
        return korb_raise_thread_error(c, slots, "Attempt to unlock a mutex which is not locked");
    if (UNLIKELY(m->owner != vm->cur_thread || m->owner_fiber != vm->running_fiber))
        return korb_raise_thread_error(c, slots, "Attempt to unlock a mutex which is locked by another thread/fiber");
    m->owner = NULL; m->owner_fiber = NULL;
    korb_waitq_wake_one(vm, &m->wq_head, &m->wq_tail);
    return RESULT_OK(VALUE_REF_GET(self));
}

/* __trap_ctx(bool) — Signal.__deliver brackets a trap handler with it */
static RESULT
korb_m_trap_ctx(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)slots; (void)self;
    korb_thread_boot(c);
    struct korb_thread *const cur = c->vm->cur_thread;
    if (KORB_TRUTHY(VALUE_SLICE_GET(a, 0))) cur->in_trap++;
    else if (cur->in_trap > 0) cur->in_trap--;
    return RESULT_OK(KORB_NIL);
}

static RESULT
korb_m_mutex_lock(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{ (void)a; return korb_mutex_lock_core(c, slots, self, true); }

static RESULT
korb_m_mutex_unlock(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{ (void)a; return korb_mutex_unlock_core(c, slots, self); }

static RESULT
korb_m_mutex_try_lock(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)slots; (void)a;
    korb_thread_boot(c);
    KorbMutex *const m = VAL2MUTEX(VALUE_REF_GET(self));
    if (korb_mutex_owner(m) != NULL) return RESULT_OK(KORB_FALSE);
    m->owner = c->vm->cur_thread; m->owner_fiber = c->vm->running_fiber;
    return RESULT_OK(KORB_TRUE);
}

static RESULT
korb_m_mutex_locked_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots; (void)a;
    return RESULT_OK(korb_mutex_owner(VAL2MUTEX(VALUE_REF_GET(self))) != NULL ? KORB_TRUE : KORB_FALSE);
}

static RESULT
korb_m_mutex_owned_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)slots; (void)a;
    korb_thread_boot(c);
    const KorbMutex *const m = VAL2MUTEX(VALUE_REF_GET(self));
    return RESULT_OK(korb_mutex_owner(m) == c->vm->cur_thread && m->owner_fiber == c->vm->running_fiber ? KORB_TRUE : KORB_FALSE);
}

/* Mutex#synchronize { … } — lock; yield; ensure unlock (block の例外でも解放) */
static RESULT
korb_m_mutex_synchronize(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                         NODE *block, VALUE *def_env, VALUE *cself)
{
    (void)a;
    if (UNLIKELY(block == NULL))
        return korb_raise_thread_error(c, slots, "must be called with a block");
    CHECK(korb_mutex_lock_core(c, slots, self, true));
    RESULT r = korb_block_yield(c, slots, block, def_env, NULL, 0, cself);
    RESULT u = korb_mutex_unlock_core(c, slots, self);
    if (r.state != KORB_NORMAL) return r;                 /* block の unwind が優先 */
    if (UNLIKELY(u.state != KORB_NORMAL)) return u;
    return r;
}

/* Mutex#sleep([sec]) — unlock して sleep、起きたら再 lock (CondVar の素) */
static RESULT
korb_m_mutex_sleep(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    double sec = -1.0;                                 /* -1 = 無期限 (引数なし / nil) */
    if (VALUE_SLICE_LEN(a) >= 1 && VALUE_SLICE_GET(a, 0) != KORB_NIL) {
        CHECK(korb_thread_tmo_arg(c, slots, VALUE_SLICE_GET(a, 0), &sec));
        if (UNLIKELY(sec < 0))
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "time interval must not be negative");
    }
    CHECK(korb_mutex_unlock_core(c, slots, self));
    struct korb_blop b; memset(&b, 0, sizeof b);
    b.kind = KORB_BLOP_TIMER;
    if (sec >= 0) korb_blop_deadline_in(&b, sec);
    else b.flags |= KORB_BLOP_F_DEADLOCKABLE;
    struct timespec t0; korb_blop_now(&t0);
    RESULT sr = korb_blop_wait(c, slots, &b);
    RESULT lr = korb_mutex_lock_core(c, slots, self, false);     /* raise でも必ず再 lock を試す */
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    if (UNLIKELY(lr.state != KORB_NORMAL)) return lr;
    struct timespec t1; korb_blop_now(&t1);
    long slept = (long)(t1.tv_sec - t0.tv_sec);
    const long nd = t1.tv_nsec - t0.tv_nsec;
    if (nd > 500000000L) slept++; else if (nd < -500000000L) slept--;
    return RESULT_OK(LONG2FIX(slept < 0 ? 0 : slept));
}

/* ConditionVariable#wait(obj[, timeout]) — cv 待機列に入って obj.sleep(timeout) を
 * 呼ぶ (CRuby も #sleep への funcall なので Mutex 以外でもよい)。起床時の再 lock は
 * Mutex#sleep 側の責務。待機列からの自己除去は ensure 相当で必ず行う。 */
static RESULT
korb_m_condvar_wait(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    struct korb_vm *const vm = c->vm;
    korb_thread_boot(c);
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..2)");
    struct korb_thread *const cur = vm->cur_thread;
    { KorbCondVar *cv = VAL2CONDVAR(VALUE_REF_GET(self));
      korb_waitq_push(&cv->wq_head, &cv->wq_tail, cur); }
    slots[0] = VALUE_SLICE_GET(a, 0);                     /* receiver of #sleep */
    slots[1] = VALUE_SLICE_LEN(a) >= 2 ? VALUE_SLICE_GET(a, 1) : KORB_NIL;   /* CRuby は常に 1 引数 */
    const RESULT r = korb_send(c, slots + 2, korb_intern(vm, "sleep", 5), 0, 1);
    { KorbCondVar *cv = VAL2CONDVAR(VALUE_REF_GET(self));  /* 再導出 + 自己除去 (冪等) */
      korb_waitq_remove(&cv->wq_head, &cv->wq_tail, vm->cur_thread); }
    return r;                                             /* CRuby は #sleep の戻り値をそのまま返す */
}

static RESULT
korb_m_condvar_signal(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)slots; (void)a;
    korb_thread_boot(c);
    KorbCondVar *const cv = VAL2CONDVAR(VALUE_REF_GET(self));
    korb_waitq_wake_one(c->vm, &cv->wq_head, &cv->wq_tail);
    return RESULT_OK(VALUE_REF_GET(self));
}

static RESULT
korb_m_condvar_broadcast(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)slots; (void)a;
    korb_thread_boot(c);
    KorbCondVar *const cv = VAL2CONDVAR(VALUE_REF_GET(self));
    while (cv->wq_head) korb_waitq_wake_one(c->vm, &cv->wq_head, &cv->wq_tail);
    return RESULT_OK(VALUE_REF_GET(self));
}

/* ConditionVariable の waiter 数 (Queue#num_waiting 用) */
static RESULT
korb_m_condvar_num_waiting(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)c; (void)slots; (void)a;
    long n = 0;
    for (const struct korb_thread *t = VAL2CONDVAR(VALUE_REF_GET(self))->wq_head; t; t = t->join_next) n++;
    return RESULT_OK(LONG2FIX(n));
}

/* .new (korb_send_impl の name 特例から) */
static RESULT
korb_mutex_s_new(CTX *c, VALUE *slots)
{
    KorbMutex *m = korb_alloc(c, slots, sizeof(KorbMutex), KORB_OBJ_MUTEX);
    m->owner = NULL; m->wq_head = m->wq_tail = NULL;
    return RESULT_OK((VALUE)m);
}

static RESULT
korb_condvar_s_new(CTX *c, VALUE *slots)
{
    KorbCondVar *cv = korb_alloc(c, slots, sizeof(KorbCondVar), KORB_OBJ_CONDVAR);
    cv->wq_head = cv->wq_tail = NULL;
    return RESULT_OK((VALUE)cv);
}

/* ==== IO 連携 (M3): wait_readable / wait_writable / IO.select ============= */

static RESULT
korb_thread_tmo_arg(CTX *c, VALUE *slots, VALUE v, double *out)
{
    if (v == KORB_NIL) { *out = -1.0; return RESULT_OK(KORB_NIL); }   /* -1 = wait forever */
    if (FIXNUM_P(v)) *out = (double)FIX2LONG(v);
    else if (KORB_FLOAT_P(v)) *out = korb_float_val(v);
    else if (!korb_num_to_d(v, out)) {                                /* Rational / Bignum */
        /* CRuby accepts anything with #divmod: seconds = q + r (rb_time_interval) */
        const char *const cls = korb_coerce_name(c, v);
        bool got = false;
        if (KORB_OBJECT_P(v) && korb_responds_to(c, v, korb_intern(c->vm, "divmod", 6))) {
            slots[0] = v; slots[1] = LONG2FIX(1);
            const RESULT dr = korb_send_impl(c, slots + 2, korb_intern(c->vm, "divmod", 6), 0, 1, NULL, NULL, NULL);
            if (UNLIKELY(dr.state != KORB_NORMAL)) return dr;
            if (KORB_ARRAY_P(dr.value) && VAL2ARY(dr.value)->len == 2) {
                double q = 0, r = 0;
                if (korb_num_to_d(korb_items_data(VAL2ARY(dr.value)->items)[0], &q) &&
                    korb_num_to_d(korb_items_data(VAL2ARY(dr.value)->items)[1], &r)) { *out = q + r; got = true; }
            }
        }
        if (!got) return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into time interval", cls);
    }
    /* an unvalidated negative would land on the -1 "forever" sentinel and hang */
    if (isnan(*out)) return korb_raise(c, slots, KORB_E_RANGE, 0, "NaN out of Time range");
    if (*out < 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "time interval must not be negative");
    if (isinf(*out)) *out = -1.0;                                     /* +Infinity: no deadline */
    return RESULT_OK(KORB_NIL);
}

/* IO#wait_readable([tmo]) / #wait_writable([tmo]) — POLL blop 1 fd。
 * ready → self、timeout → nil (CRuby)。 */
static RESULT
korb_m_io_wait_ev(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a, short ev)
{
    const int wfd = korb_io_fd(c, VALUE_REF_GET(self));
    if (UNLIKELY(wfd < 0)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    double tmo = -1.0;
    if (VALUE_SLICE_LEN(a) >= 1) CHECK(korb_thread_tmo_arg(c, slots, VALUE_SLICE_GET(a, 0), &tmo));
    struct pollfd p; p.fd = wfd; p.events = ev; p.revents = 0;
    ssize_t ready = 0;
    CHECK(korb_blop_poll_wait(c, slots, &p, 1, tmo, &ready));
    return RESULT_OK(ready ? VALUE_REF_GET(self) : KORB_NIL);
}

static RESULT
korb_m_io_wait_readable(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{ return korb_m_io_wait_ev(c, slots, self, a, POLLIN); }

/* IO#__io_poll(events_int, timeout_or_nil) → ready revents mask (0 = timeout)。
 * prelude の IO#wait (io/wait) が使う汎用形。 */
static RESULT
korb_m_io_poll_raw(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    const int wfd = korb_io_fd(c, VALUE_REF_GET(self));
    if (UNLIKELY(wfd < 0)) return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1 || !FIXNUM_P(VALUE_SLICE_GET(a, 0))))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "events must be an Integer");
    double tmo = -1.0;
    if (VALUE_SLICE_LEN(a) >= 2) CHECK(korb_thread_tmo_arg(c, slots, VALUE_SLICE_GET(a, 1), &tmo));
    struct pollfd p;
    p.fd = wfd;
    p.events = (short)FIX2LONG(VALUE_SLICE_GET(a, 0));
    p.revents = 0;
    ssize_t ready = 0;
    CHECK(korb_blop_poll_wait(c, slots, &p, 1, tmo, &ready));
    return RESULT_OK(LONG2FIX(ready ? p.revents : 0));
}

static RESULT
korb_m_io_wait_writable(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{ return korb_m_io_wait_ev(c, slots, self, a, POLLOUT); }

/* IO.select(reads[, writes[, excepts[, timeout]]]) — pollfd 配列を 1 個の
 * POLL blop に (poll(2) 意味論そのまま: その時点で ready な全部が返る)。 */
static RESULT
korb_m_io_s_select(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)self;
    const uint32_t alen = VALUE_SLICE_LEN(a);
    if (UNLIKELY(alen == 0))   /* CRuby: 1..4 arguments; without any it is not "wait forever" */
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments (given 0, expected 1..4)");
    double tmo = -1.0;
    if (alen >= 4) CHECK(korb_thread_tmo_arg(c, slots, VALUE_SLICE_GET(a, 3), &tmo));
    uint32_t cnt[3] = { 0, 0, 0 };
    for (uint32_t s = 0; s < 3; s++) {
        const VALUE av = (alen > s) ? VALUE_SLICE_GET(a, s) : KORB_NIL;
        if (av == KORB_NIL) continue;
        if (UNLIKELY(!KORB_ARRAY_P(av)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "wrong argument type %s (expected Array)", korb_type_name(av));
        cnt[s] = VAL2ARY(av)->len;
    }
    const nfds_t total = cnt[0] + cnt[1] + cnt[2];
    if (total == 0) {                              /* fd なし: ただの (無期限) sleep */
        struct korb_blop b; memset(&b, 0, sizeof b);
        b.kind = KORB_BLOP_TIMER;
        if (tmo >= 0) korb_blop_deadline_in(&b, tmo);
        CHECK(korb_blop_wait(c, slots, &b));
        return RESULT_OK(KORB_NIL);
    }
    struct pollfd pbuf[64];
    struct pollfd *pf = (total <= 64) ? pbuf : malloc(sizeof(*pf) * total);
    if (!pf) abort();
    static const short evs[3] = { POLLIN, POLLOUT, POLLPRI };
    nfds_t k = 0;
    for (uint32_t s = 0; s < 3; s++) {             /* fill (no alloc in this loop) */
        if (cnt[s] == 0) continue;
        const KorbArray *arr = VAL2ARY(VALUE_SLICE_GET(a, s));
        for (uint32_t i = 0; i < cnt[s]; i++) {
            const int efd = korb_io_fd(c, korb_items_data(arr->items)[i]);
            if (UNLIKELY(efd < 0)) {
                if (pf != pbuf) free(pf);
                return korb_raise(c, slots, KORB_E_IOERROR, 0, "closed stream");
            }
            pf[k].fd = efd; pf[k].events = evs[s]; pf[k].revents = 0; k++;
        }
    }
    ssize_t ready = 0;
    { RESULT r = korb_blop_poll_wait(c, slots, pf, total, tmo, &ready);
      if (UNLIKELY(r.state != KORB_NORMAL)) { if (pf != pbuf) free(pf); return r; } }
    if (ready == 0) { if (pf != pbuf) free(pf); return RESULT_OK(KORB_NIL); }   /* timeout */
    /* [[r], [w], [e]] — alloc を跨ぐので IO は毎回 arg 配列 (rooted) から再読出 */
    static const short want[3] = { POLLIN | POLLHUP | POLLERR, POLLOUT | POLLERR, POLLPRI };
    slots[0] = UNWRAP(korb_ary_new(c, slots, 3));
    VALUE_REF outer = VALUE_REF_AT(&slots[0]);
    nfds_t base = 0;
    for (uint32_t s = 0; s < 3; s++) {
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, cnt[s] ? cnt[s] : 1));
        VALUE_REF sub = VALUE_REF_AT(&slots[1]);
        for (uint32_t i = 0; i < cnt[s]; i++) {
            if (!(pf[base + i].revents & want[s])) continue;
            slots[2] = korb_items_data(VAL2ARY(VALUE_SLICE_GET(a, s))->items)[i];   /* 再読出 */
            CHECK(korb_ary_push_val(c, slots + 3, sub, slots[2]));
        }
        base += cnt[s];
        CHECK(korb_ary_push_val(c, slots + 2, outer, VALUE_REF_GET(sub)));
    }
    if (pf != pbuf) free(pf);
    return RESULT_OK(VALUE_REF_GET(outer));
}
