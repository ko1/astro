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
 *  - Fiber の中では thread 切替不可 (ThreadError) — fiber/main-slots の GC
 *    scan 機構と直交させるため。切替点では常に vm->running_fiber == NULL。
 *  - Thread#join(timeout) / #kill(実行中) / sleep 連携は M2/M3 (blop 層)。
 *  - dead thread の stack は未回収 (M2 で reap; rep 自体は意図的に不滅)。 */

/* ==== blop 層 — blocking operations (docs/io_design.md) ====================
 * 唯一の suspension point は korb_blop_wait。M2 実装範囲: TIMER (sleep /
 * join timeout) + pump (poll(2) ベース)。POLL/READ/WRITE の fd 系は M3。 */

enum korb_blop_kind  { KORB_BLOP_POLL, KORB_BLOP_READ, KORB_BLOP_WRITE, KORB_BLOP_TIMER, KORB_BLOP_CFUNC };
enum korb_blop_flags { KORB_BLOP_F_TIMEOUT = 1, KORB_BLOP_F_DONE = 2 };

static void korb_thread_runq_push(struct korb_vm *vm, struct korb_thread *t);   /* fwd (下の scheduler 節) */

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

/* engine への登録 (M2: pending リストに繋ぐだけ; fd 系の kernel 登録は M3) */
static void
korb_blop_prep(struct korb_vm *vm, struct korb_blop *b)
{
    b->flags &= (uint8_t)~KORB_BLOP_F_DONE;
    b->bl_prev = NULL; b->bl_next = vm->blop_pending;
    if (vm->blop_pending) vm->blop_pending->bl_prev = b;
    vm->blop_pending = b;
    vm->blop_npending++;
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

/* pump: 完了回収。block=1 なら次の deadline まで (無ければ無期限) native に眠る —
 * scheduler idle が呼ぶ、native thread が眠る唯一の場所。戻り値: post した数。 */
static int
korb_blop_pump(struct korb_vm *vm, int block)
{
    struct timespec now; korb_blop_now(&now);
    int posted = 0;
    for (struct korb_blop *b = vm->blop_pending, *nx; b; b = nx) {   /* 期限切れを post */
        nx = b->bl_next;
        if (korb_blop_deadline_passed(b, &now)) {
            korb_blop_post(vm, b, b->kind == KORB_BLOP_TIMER ? 0 : -ETIMEDOUT);
            posted++;
        }
    }
    if (posted || !block) return posted;
    /* 眠る長さ = 最短 deadline (無ければ無期限)。M3 でここが poll(fds…) になる */
    int ms = -1;
    for (const struct korb_blop *b = vm->blop_pending; b; b = b->bl_next) {
        if (!(b->flags & KORB_BLOP_F_TIMEOUT)) continue;
        long d = (long)(b->deadline.tv_sec - now.tv_sec) * 1000
               + (b->deadline.tv_nsec - now.tv_nsec) / 1000000L;
        if (d < 0) d = 0;
        if (ms < 0 || d < ms) ms = (int)d;
    }
    poll(NULL, 0, ms);
    korb_blop_now(&now);
    for (struct korb_blop *b = vm->blop_pending, *nx; b; b = nx) {
        nx = b->bl_next;
        if (korb_blop_deadline_passed(b, &now)) {
            korb_blop_post(vm, b, b->kind == KORB_BLOP_TIMER ? 0 : -ETIMEDOUT);
            posted++;
        }
    }
    return posted;
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
}

static struct korb_thread *
korb_thread_runq_pop(struct korb_vm *vm)
{
    for (;;) {
        struct korb_thread *t = vm->runq_head;
        if (t == NULL) return NULL;
        vm->runq_head = t->rq_next;
        if (vm->runq_head == NULL) vm->runq_tail = NULL;
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
    m->thval = KORB_NIL; m->args = KORB_NIL; m->blk = KORB_NIL; m->captured_self = KORB_NIL;
    m->result = KORB_NIL; m->exc = KORB_NIL; m->tls = KORB_NIL;
    m->name = KORB_NIL; m->pending_ints = KORB_NIL;
    /* main の stack base: fiber 内から boot されたら c->slots は fiber base なので
     * 保留し、最初の切替 (fiber 外が保証される) で埋める */
    if (vm->running_fiber == NULL) { m->vslots = c->slots; m->vslots_limit = c->slots_limit; }
    m->uctx = calloc(1, sizeof(ucontext_t));
    if (!m->uctx) abort();
    m->state = KORB_TH_RUNNING; m->started = 1;
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
static void
korb_thread_ctx_load(CTX *c, struct korb_thread *t)
{
    if (!t->started) {                        /* 初走: 新品の stack */
        c->slots = t->vslots; c->slots_top = t->vslots;
        c->slots_limit = t->vslots_limit; c->slots_high_water = t->vslots;
        c->cstack_limit = (const char *)t->cstack + KORB_FIBER_CSTACK_MARGIN;
    } else {                                  /* 再開: suspend 時の tuple */
        c->slots = t->saved_base; c->slots_top = t->saved_top;
        c->slots_limit = t->vslots_limit; c->slots_high_water = t->saved_hw;
        c->cstack_limit = t->saved_cstack_limit;
    }
}

static void
korb_thread_deadlock_fatal(void)
{
    fprintf(stderr, "koruby_precise: deadlock detected (no runnable thread; all waiting)\n");
    exit(2);
}

/* 現 thread から離れる。呼び出し側が cur->state を設定済みであること
 * (Thread.pass: READY + runq_push 済 / #join: PENDED + joiners 登録済)。
 * `slots` = 生きた cursor (suspend 中の GC scan top)。
 * 他に走れる thread がいなければ: READY の cur はそのまま続行、
 * PENDED なら deadlock (M2 で blop pump に置き換わる)。 */
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
            if (vm->blop_npending == 0) korb_thread_deadlock_fatal();
            korb_blop_pump(vm, 1);
            next = korb_thread_runq_pop(vm);
            if (next) break;
        }
        if (next == cur) { cur->state = KORB_TH_RUNNING; return RESULT_OK(KORB_NIL); }   /* 自分の blop が完了 */
    }
    if (cur->vslots == NULL) {                /* boot が fiber 内だった main (遅延分) */
        cur->vslots = c->slots; cur->vslots_limit = c->slots_limit;
    }
    cur->saved_base = c->slots; cur->saved_top = slots;
    cur->saved_hw = c->slots_high_water; cur->saved_cstack_limit = c->cstack_limit;
    vm->cur_thread = next;
    next->state = KORB_TH_RUNNING;
    korb_thread_ctx_load(c, next);
    swapcontext((ucontext_t *)cur->uctx, (ucontext_t *)next->uctx);
    /* === 再開: 切替元が我々の saved_* を c に載せてから swap してきている === */
    return RESULT_OK(KORB_NIL);
}

/* dead thread からの離脱 (状態保存なし)。戻らない。 */
static void
korb_thread_exit_switch(CTX *c)
{
    struct korb_vm *const vm = c->vm;
    struct korb_thread *next = korb_thread_runq_pop(vm);
    while (next == NULL) {                         /* 残りは全員 blop 待ち → pump */
        if (vm->blop_npending == 0) korb_thread_deadlock_fatal();
        korb_blop_pump(vm, 1);
        next = korb_thread_runq_pop(vm);
    }
    vm->cur_thread = next;
    next->state = KORB_TH_RUNNING;
    korb_thread_ctx_load(c, next);
    setcontext((ucontext_t *)next->uctx);     /* never returns */
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
    if (UNLIKELY(vm->running_fiber != NULL))
        return korb_raise_thread_error(c, slots, "can't switch threads from inside a Fiber");
    b->waiter = vm->cur_thread;
    korb_blop_prep(vm, b);
    while (!(b->flags & KORB_BLOP_F_DONE)) {
        struct korb_thread *const cur = vm->cur_thread;
        cur->state = KORB_TH_PENDED; cur->blop = b;
        RESULT r = korb_thread_yield_cpu(c, slots);
        cur->blop = NULL;
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    }
    return RESULT_OK(KORB_NIL);
}

/* Kernel#sleep([sec]) — TIMER blop。他の green thread はその間走れる。
 * 戻り値は眠った秒数 (CRuby 同様、丸めた Integer)。引数なし = 無期限。 */
static RESULT
korb_bi_sleep(CTX *c, VALUE *slots, VALUE_SLICE args)
{
    double sec = -1.0;                              /* forever */
    if (VALUE_SLICE_LEN(args) >= 1 && VALUE_SLICE_GET(args, 0) != KORB_NIL) {
        const VALUE v = VALUE_SLICE_GET(args, 0);
        if (FIXNUM_P(v)) sec = (double)FIX2LONG(v);
        else if (KORB_FLOAT_P(v)) sec = korb_float_val(v);
        else return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into time interval", korb_type_name(v));
        if (sec < 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "time interval must not be negative");
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
static void
korb_thread_trampoline(unsigned hi, unsigned lo)
{
    CTX *const c = (CTX *)(((uintptr_t)hi << 32) | (uintptr_t)lo);
    struct korb_thread *const t = c->vm->cur_thread;
    t->started = 1;
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
        t->raised = 1; t->exc = r.value;
        korb_report_uncaught(c, r.value);     /* report_on_exception (CRuby default: true) */
    } else {
        t->result = r.value;                  /* NORMAL (break/return unwinds folded in M1) */
    }
    t->state = KORB_TH_DEAD;
    for (struct korb_thread *j = t->joiners; j; ) {   /* #join 待ちを全員起こす */
        struct korb_thread *nx = j->join_next;
        j->join_next = NULL; j->state = KORB_TH_READY;
        korb_thread_runq_push(c->vm, j);
        j = nx;
    }
    t->joiners = NULL;
    /* TODO(M2): vslots/cstack の reap (この stack の上では解放できない) */
    korb_thread_exit_switch(c);               /* never returns */
}

/* ---- Thread.new ----------------------------------------------------------- */
static RESULT
korb_thread_s_new(CTX *c, VALUE *slots, VALUE_SLICE a, NODE *block, VALUE *def_env, VALUE *captured_self)
{
    if (UNLIKELY(block == NULL))
        return korb_raise_thread_error(c, slots, "must be called with a block");
    if (UNLIKELY(block == KORB_BLK_CPROC))
        return korb_raise(c, slots, KORB_E_NOTIMPL, 0, "Thread.new with a forwarded C proc");
    korb_thread_boot(c);
    const uint32_t n = VALUE_SLICE_LEN(a);
    slots[0] = UNWRAP(korb_ary_new(c, slots, n ? n : 1));   /* args (rooted) */
    { VALUE_REF ar = VALUE_REF_AT(&slots[0]);
      for (uint32_t i = 0; i < n; i++) CHECK(korb_ary_push_val(c, slots + 1, ar, VALUE_SLICE_GET(a, i))); }
    /* body を Proc に close する: 作成フレームが thread 実行前に死ぬのが普通
     * (n.times { Thread.new {…} }) なので、生の def_env は保持できない。 */
    if (def_env == KORB_BLK_FWD) slots[1] = *captured_self;   /* &blk: 既に Proc */
    else slots[1] = UNWRAP(korb_make_proc(c, slots + 1, block, def_env, KORB_CSELF_VAL(captured_self), 0));
    struct korb_thread *t = calloc(1, sizeof *t);
    if (!t) { fprintf(stderr, "koruby_precise: oom (thread rep)\n"); abort(); }
    t->thval = KORB_NIL; t->captured_self = KORB_NIL;
    t->result = KORB_NIL; t->exc = KORB_NIL; t->tls = KORB_NIL;
    t->name = KORB_NIL; t->pending_ints = KORB_NIL;
    t->args = slots[0];
    t->blk = slots[1];
    void *vs = mmap(NULL, KORB_FIBER_VSLOTS_BYTES, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (vs == MAP_FAILED) { perror("koruby_precise: mmap thread vslots"); abort(); }
    t->vslots = (VALUE *)vs + 2;              /* bottom-header slack (base[-1]=self, base[-2]=EP) */
    t->vslots[-1] = 0; t->vslots[-2] = 0;
    t->vslots_limit = (VALUE *)vs + KORB_FIBER_VSLOTS_BYTES / sizeof(VALUE) - KORB_FIBER_VSLOTS_MARGIN;
    t->cstack = malloc(KORB_FIBER_CSTACK_BYTES);
    if (!t->cstack) abort();
    t->uctx = calloc(1, sizeof(ucontext_t));
    if (!t->uctx) abort();
    getcontext((ucontext_t *)t->uctx);
    ((ucontext_t *)t->uctx)->uc_stack.ss_sp = t->cstack;
    ((ucontext_t *)t->uctx)->uc_stack.ss_size = KORB_FIBER_CSTACK_BYTES;
    ((ucontext_t *)t->uctx)->uc_link = NULL;
    makecontext((ucontext_t *)t->uctx, (void (*)(void))korb_thread_trampoline, 2,
                (unsigned)((uintptr_t)c >> 32), (unsigned)((uintptr_t)c & 0xFFFFFFFFu));
    t->state = KORB_TH_READY;
    t->next = c->vm->thread_list; c->vm->thread_list = t;   /* GC scan 対象に (roots 設定後) */
    korb_thread_runq_push(c->vm, t);
    CHECK(korb_thread_handle(c, slots + 1, t));
    return RESULT_OK(t->thval);
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
        else return korb_raise(c, slots, KORB_E_TYPE, 0, "can't convert %s into time interval", korb_type_name(v));
    }
    korb_thread_boot(c);
    if (UNLIKELY(t == vm->cur_thread))
        return korb_raise_thread_error(c, slots, "Target thread must not be current thread");
    while (t->state != KORB_TH_DEAD) {
        if (UNLIKELY(vm->running_fiber != NULL))
            return korb_raise_thread_error(c, slots, "can't switch threads from inside a Fiber");
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
        if (tmo >= 0) {
            if (!(tb.flags & KORB_BLOP_F_DONE))
                korb_blop_cancel(vm, &tb, -ECANCELED);      /* 死亡側が先: timer を回収 */
            else if (t->state != KORB_TH_DEAD) {            /* timeout 側が先 */
                korb_thread_joiners_remove(t, vm->cur_thread);
                return RESULT_OK(KORB_NIL);                 /* CRuby: join(tmo) 失敗 = nil */
            }
        }
        if (UNLIKELY(r.state != KORB_NORMAL)) return r;
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
    (void)c; (void)slots;
    VAL2THREAD(VALUE_REF_GET(self))->rep->name = VALUE_SLICE_GET(a, 0);   /* rep は libc: barrier 不要 (root visit が forward) */
    return RESULT_OK(VALUE_SLICE_GET(a, 0));
}

/* #[] / #[]= / #key? — TLS (CRuby: fiber-local だが Phase 1 は thread-local 一枚) */
static VALUE
korb_thread_tls_key(CTX *c, VALUE k)
{
    if (KORB_STRING_P(k))
        return ID2SYM(korb_intern(c->vm, korb_strbuf_data(VAL2STR(k)->buf), VAL2STR(k)->len));
    return k;
}

static RESULT
korb_m_thread_aref(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    (void)slots;
    if (t->tls == KORB_NIL) return RESULT_OK(KORB_NIL);
    const VALUE k = korb_thread_tls_key(c, VALUE_SLICE_GET(a, 0));
    const int32_t i = korb_hash_find(VAL2HASH(t->tls), k);
    return RESULT_OK(i >= 0 ? korb_items_data(VAL2HASH(t->tls)->items)[2 * i + 1] : KORB_NIL);
}

static RESULT
korb_m_thread_aset(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    if (t->tls == KORB_NIL) t->tls = UNWRAP(korb_hash_new(c, slots, 4));
    slots[0] = korb_thread_tls_key(c, VALUE_SLICE_GET(a, 0));
    slots[1] = VALUE_SLICE_GET(a, 1);
    CHECK(korb_hash_set(c, slots + 2, VALUE_REF_AT(&t->tls), VALUE_REF_AT(&slots[0]), slots[1]));
    return RESULT_OK(slots[1]);
}

static RESULT
korb_m_thread_key_p(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    (void)slots;
    if (t->tls == KORB_NIL) return RESULT_OK(KORB_FALSE);
    const VALUE k = korb_thread_tls_key(c, VALUE_SLICE_GET(a, 0));
    return RESULT_OK(korb_hash_find(VAL2HASH(t->tls), k) >= 0 ? KORB_TRUE : KORB_FALSE);
}

/* #kill / #exit — M1: 未起動 thread の取り消しのみ (実行中の kill は M3 の
 * pending-interrupt 経由; ここでは CRuby 同様 self を返すが何もしない) */
static RESULT
korb_m_thread_kill(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)slots; (void)a;
    struct korb_thread *const t = VAL2THREAD(VALUE_REF_GET(self))->rep;
    if (!t->started && t->state == KORB_TH_READY) {
        t->state = KORB_TH_DEAD;                       /* runq からは pop 時に skip */
        for (struct korb_thread *j = t->joiners; j; ) {
            struct korb_thread *nx = j->join_next;
            j->join_next = NULL; j->state = KORB_TH_READY;
            korb_thread_runq_push(c->vm, j);
            j = nx;
        }
        t->joiners = NULL;
    }
    return RESULT_OK(VALUE_REF_GET(self));
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

static RESULT
korb_m_thread_s_pass(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)self; (void)a;
    struct korb_vm *const vm = c->vm;
    if (vm->cur_thread == NULL || vm->runq_head == NULL) return RESULT_OK(KORB_NIL);
    if (UNLIKELY(vm->running_fiber != NULL))
        return korb_raise_thread_error(c, slots, "can't switch threads from inside a Fiber");
    struct korb_thread *const cur = vm->cur_thread;
    cur->state = KORB_TH_READY;
    korb_thread_runq_push(vm, cur);
    CHECK(korb_thread_yield_cpu(c, slots));
    return RESULT_OK(KORB_NIL);
}

static RESULT
korb_m_thread_s_list(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a)
{
    (void)self; (void)a;
    korb_thread_boot(c);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));
    VALUE_REF dst = VALUE_REF_AT(&slots[0]);
    for (struct korb_thread *t = c->vm->thread_list; t; t = t->next) {
        if (t->state == KORB_TH_DEAD) continue;
        slots[1] = UNWRAP(korb_thread_handle(c, slots + 1, t));
        CHECK(korb_ary_push_val(c, slots + 2, dst, slots[1]));
    }
    return RESULT_OK(VALUE_REF_GET(dst));
}
