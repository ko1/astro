/* koruby_precise — process.c: fork/exec based Process.spawn, Kernel#system,
 * Kernel#` and Process.wait, plus the Process::Status object behind $?.
 * #included into korb_runtime.c's TU.
 *
 * A command is either a single String (run through /bin/sh when it contains
 * shell metacharacters, else split on whitespace and exec'd directly) or an
 * Array of argv strings, matching CRuby's rule.  Supported spawn options:
 * an env Hash before the command, and :in / :out / :err redirections to a
 * filename String, an Integer fd, an IO, [:child, fd] or :close.
 */
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

#define KORB_SPAWN_MAX_ARGV 256

/* ---- signal delivery -------------------------------------------------------
 * koruby delivers signals at interpreter check points rather than from a C
 * handler: a signal it wants to deliver is *blocked*, so it stays pending in
 * the kernel until korb_signal_deliver reaps it with sigtimedwait(2).  With no
 * async handler there is no handler-side global state and no reentrancy
 * hazard; the cost is that delivery waits for the next check point (a blocking
 * operation, Thread.pass, or the kill(2) that raised it).  Policy — ignore /
 * run a Proc / raise SignalException — lives in Signal.__deliver in the prelude.
 *
 * The signal mask survives execve, so every fork site clears it in the child
 * (korb_child_reset_signals) or the child would start with signals blocked. */
/* Nothing is blocked at startup.  A blocked signal waits for the next check
 * point and a pure-CPU loop has none, so blocking up front would make
 * `timeout`, Ctrl-C and `kill` unable to stop a runaway script — a worse bug
 * than the one this delivers.  A signal becomes koruby-delivered only when the
 * program asks for it: Signal.trap blocks it (see __signal_block), and
 * Process.kill blocks it around a send to our own pid (korb_kill_self).
 * Signals arriving from outside an untrapped process keep the OS default. */
static const int korb_sig_deliverable[] = {
    SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGUSR1, SIGUSR2, SIGALRM,
    SIGVTALRM, SIGXCPU, SIGXFSZ, SIGPROF, SIGWINCH, SIGCONT,
};

/* Everything koruby is willing to reap.  Reaping is by wait-set, so listing an
 * unblocked signal here is harmless — it can never be pending. */
static void korb_sigset_deliverable(sigset_t *const set) {
    sigemptyset(set);
    for (size_t i = 0; i < sizeof korb_sig_deliverable / sizeof korb_sig_deliverable[0]; i++)
        sigaddset(set, korb_sig_deliverable[i]);
}

static void korb_child_reset_signals(void) {
    sigset_t empty;
    sigemptyset(&empty);
    (void)pthread_sigmask(SIG_SETMASK, &empty, NULL);
}

/* Reap one pending deliverable signal without blocking; 0 = none pending. */
static int korb_signal_reap(void) {
    sigset_t set;
    korb_sigset_deliverable(&set);
    const struct timespec zero = { 0, 0 };
    siginfo_t info;
    const int s = sigtimedwait(&set, &info, &zero);
    return s > 0 ? s : 0;
}

static int korb_signo_of(CTX *c, VALUE v);   /* fwd (defined with the trap builtins below) */
RESULT korb_signal_deliver(CTX *c, VALUE *slots);   /* fwd (defined just below korb_kill_self) */

/* __signal_block(signo, flag) — add/remove one signal from the blocked (i.e.
 * koruby-delivered) set.  Signal.trap uses it to take over INT/QUIT. */
static RESULT korb_m_signal_block(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int sig = korb_signo_of(c, VALUE_SLICE_GET(a, 0));
    if (sig <= 0 || sig == SIGKILL || sig == SIGSTOP) return RESULT_OK(KORB_FALSE);
    const bool on = VALUE_SLICE_LEN(a) < 2 || KORB_TRUTHY(VALUE_SLICE_GET(a, 1));
    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, sig);
    (void)pthread_sigmask(on ? SIG_BLOCK : SIG_UNBLOCK, &one, NULL);
    (void)slots;
    return RESULT_OK(on ? KORB_TRUE : KORB_FALSE);
}

/* Process.kill to our own pid.  The signal is blocked across the send so it
 * lands as *pending* and is reaped here, instead of taking its default action
 * — that is what makes `Process.kill(:TERM, Process.pid)` raise SignalException
 * the way CRuby's handler-based delivery does.  The previous mask is restored,
 * so a signal the program trapped stays blocked and one it did not goes back to
 * its default disposition for anything arriving from outside. */
static RESULT korb_kill_self(CTX *c, VALUE *slots, int sig) {
    if (sig == 0 || sig == SIGKILL || sig == SIGSTOP) {          /* cannot be blocked/deferred */
        if (kill(getpid(), sig) != 0) return korb_raise_errno(c, slots, errno, "kill", "");
        return RESULT_OK(KORB_NIL);
    }
    sigset_t one, old;
    sigemptyset(&one);
    sigaddset(&one, sig);
    (void)pthread_sigmask(SIG_BLOCK, &one, &old);
    const int r = kill(getpid(), sig);
    const int e = errno;
    RESULT d = (r == 0) ? korb_signal_deliver(c, slots) : RESULT_OK(KORB_NIL);
    (void)pthread_sigmask(SIG_SETMASK, &old, NULL);               /* restore before propagating a raise */
    if (r != 0) return korb_raise_errno(c, slots, e, "kill", "");
    return d;
}

/* Interpreter check point: hand one pending signal to Signal.__deliver, which
 * runs the trap handler or raises.  RESULT_OK(nil) when nothing was pending. */
RESULT korb_signal_deliver(CTX *c, VALUE *slots) {
    const int sig = korb_signal_reap();
    if (LIKELY(sig == 0)) return RESULT_OK(KORB_NIL);
    const VALUE sigmod = korb_const_get(c->vm, korb_intern(c->vm, "Signal", 6));
    if (UNLIKELY(!KORB_CLASS_P(sigmod))) return RESULT_OK(KORB_NIL);   /* prelude not up yet */
    slots[0] = sigmod;
    slots[1] = LONG2FIX(sig);
    return korb_send(c, slots + 2, korb_intern(c->vm, "__deliver", 9), 0, 1);
}

/* One parsed redirection: the child's fd `from` becomes `to` (an fd) or is
 * opened from `path`. */
struct korb_redir {
    int from;
    int to;                 /* -1 = use path, -2 = close */
    char path[512];
    int  oflags;
};

struct korb_spawn_plan {
    char  chdir[512];
    bool  close_others;
    char *argv[KORB_SPAWN_MAX_ARGV];
    char  argbuf[8192];
    size_t argbuf_len;
    bool  use_shell;
    char *envp[KORB_SPAWN_MAX_ARGV];
    char  envbuf[8192];
    size_t envbuf_len;
    bool  have_env;
    struct korb_redir redir[8];
    uint32_t nredir;
};

/* Copy `s` into the plan's argument arena and record it as the next argv slot. */
static bool korb_spawn_push_arg(struct korb_spawn_plan *p, const char *s, uint32_t n) {
    uint32_t i = 0;
    while (p->argv[i] != NULL && i < KORB_SPAWN_MAX_ARGV - 1) i++;
    if (i >= KORB_SPAWN_MAX_ARGV - 1 || p->argbuf_len + n + 1 > sizeof p->argbuf) return false;
    char *dst = p->argbuf + p->argbuf_len;
    memcpy(dst, s, n); dst[n] = '\0';
    p->argbuf_len += n + 1;
    p->argv[i] = dst;
    p->argv[i + 1] = NULL;
    return true;
}

static bool korb_spawn_push_env(struct korb_spawn_plan *p, const char *k, uint32_t kn,
                                const char *v, uint32_t vn) {
    uint32_t i = 0;
    while (p->envp[i] != NULL && i < KORB_SPAWN_MAX_ARGV - 1) i++;
    if (i >= KORB_SPAWN_MAX_ARGV - 1 || p->envbuf_len + kn + vn + 2 > sizeof p->envbuf) return false;
    char *dst = p->envbuf + p->envbuf_len;
    memcpy(dst, k, kn); dst[kn] = '=';
    memcpy(dst + kn + 1, v, vn); dst[kn + 1 + vn] = '\0';
    p->envbuf_len += kn + vn + 2;
    p->envp[i] = dst;
    p->envp[i + 1] = NULL;
    return true;
}

/* CRuby runs a single-String command through the shell only when it contains a
 * metacharacter; otherwise it splits and execs directly. */
static bool korb_cmd_needs_shell(const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (strchr("*?{}[]<>()~&|\\$;'`\"\n", s[i]) != NULL) return true;
    return false;
}

/* Split a shell-free command string on whitespace into argv. */
static bool korb_spawn_split(struct korb_spawn_plan *p, const char *s, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i >= n) break;
        const uint32_t start = i;
        while (i < n && s[i] != ' ' && s[i] != '\t') i++;
        if (!korb_spawn_push_arg(p, s + start, i - start)) return false;
    }
    return p->argv[0] != NULL;
}

/* :in / :out / :err (or an Integer fd) → the child fd it names. */
static int korb_redir_key_fd(CTX *c, VALUE k) {
    if (FIXNUM_P(k)) return (int)FIX2LONG(k);
    if (SYMBOL_P(k)) {
        const uint32_t id = (uint32_t)SYM2ID(k);
        if (id == korb_intern(c->vm, "in", 2))  return 0;
        if (id == korb_intern(c->vm, "out", 3)) return 1;
        if (id == korb_intern(c->vm, "err", 3)) return 2;
    }
    return -1;
}

/* Record one redirection target.  Returns false for a form koruby does not
 * model (the caller then leaves the fd alone). */
static bool korb_redir_value(CTX *c, struct korb_spawn_plan *p, int from, VALUE v) {
    if (p->nredir >= sizeof p->redir / sizeof p->redir[0]) return false;
    struct korb_redir *r = &p->redir[p->nredir];
    r->from = from; r->to = -1; r->path[0] = '\0'; r->oflags = 0;
    if (FIXNUM_P(v)) { r->to = (int)FIX2LONG(v); p->nredir++; return true; }
    if (SYMBOL_P(v)) {
        const uint32_t id = (uint32_t)SYM2ID(v);
        if (id == korb_intern(c->vm, "close", 5)) { r->to = -2; p->nredir++; return true; }
        if (id == korb_intern(c->vm, "in", 2))  { r->to = 0; p->nredir++; return true; }
        if (id == korb_intern(c->vm, "out", 3)) { r->to = 1; p->nredir++; return true; }
        if (id == korb_intern(c->vm, "err", 3)) { r->to = 2; p->nredir++; return true; }
        return false;
    }
    if (KORB_STRING_P(v)) {
        uint32_t n; const char *s = korb_str_cstr_len(v, &n);
        if (n >= sizeof r->path) return false;
        memcpy(r->path, s, n); r->path[n] = '\0';
        r->oflags = (from == 0) ? O_RDONLY : (O_WRONLY | O_CREAT | O_TRUNC);
        p->nredir++;
        return true;
    }
    if (KORB_ARRAY_P(v)) {                       /* [:child, fd] or [path, mode] */
        const KorbArray *ar = VAL2ARY(v);
        if (ar->len == 0) return false;
        const VALUE first = korb_items_data(ar->items)[0];
        if (SYMBOL_P(first) && (uint32_t)SYM2ID(first) == korb_intern(c->vm, "child", 5)) {
            if (ar->len < 2 || !FIXNUM_P(korb_items_data(ar->items)[1])) return false;
            r->to = (int)FIX2LONG(korb_items_data(ar->items)[1]);
            p->nredir++;
            return true;
        }
        if (KORB_STRING_P(first)) {
            uint32_t n; const char *s = korb_str_cstr_len(first, &n);
            if (n >= sizeof r->path) return false;
            memcpy(r->path, s, n); r->path[n] = '\0';
            r->oflags = (from == 0) ? O_RDONLY : (O_WRONLY | O_CREAT | O_TRUNC);
            if (ar->len >= 2 && KORB_STRING_P(korb_items_data(ar->items)[1])) {
                uint32_t ml; const char *m = korb_str_cstr_len(korb_items_data(ar->items)[1], &ml);
                if (ml > 0 && m[0] == 'a') r->oflags = O_WRONLY | O_CREAT | O_APPEND;
                else if (ml > 0 && m[0] == 'r') r->oflags = O_RDONLY;
            }
            p->nredir++;
            return true;
        }
        return false;
    }
    /* an IO → its descriptor */
    if (KORB_OBJECT_P(v)) {
        const KorbIORep *const rep = korb_io_rep(c, v);
        if (korb_io_open_p(rep)) { r->to = rep->fd; p->nredir++; return true; }
    }
    return false;
}

/* Build a spawn plan from the Ruby argument list.  Returns a raised RESULT on a
 * type error; `*ok` is false when the arguments are not a command at all. */
static RESULT korb_spawn_plan_build(CTX *c, VALUE *slots, VALUE_SLICE a, struct korb_spawn_plan *p) {
    memset(p, 0, sizeof *p);
    uint32_t lo = 0, hi = VALUE_SLICE_LEN(a);
    if (hi > 0 && KORB_HASH_P(VALUE_SLICE_GET(a, 0))) {          /* leading env Hash */
        const VALUE ev = VALUE_SLICE_GET(a, 0);
        const KorbHash *h = VAL2HASH(ev);
        for (uint32_t i = 0; i < h->len; i++) {
            const VALUE k = korb_items_data(h->items)[2 * i];
            const VALUE v = korb_items_data(h->items)[2 * i + 1];
            if (!KORB_STRING_P(k)) continue;
            uint32_t kn; const char *ks = korb_str_cstr_len(k, &kn);
            if (KORB_STRING_P(v)) {
                uint32_t vn; const char *vs = korb_str_cstr_len(v, &vn);
                if (!korb_spawn_push_env(p, ks, kn, vs, vn)) break;
            }
        }
        p->have_env = true;
        lo = 1;
    }
    if (hi > lo && KORB_HASH_P(VALUE_SLICE_GET(a, hi - 1))) {    /* trailing options Hash */
        const VALUE ov = VALUE_SLICE_GET(a, hi - 1);
        const KorbHash *h = VAL2HASH(ov);
        const uint32_t chdir_id = korb_intern(c->vm, "chdir", 5);
        const uint32_t close_others_id = korb_intern(c->vm, "close_others", 12);
        for (uint32_t i = 0; i < h->len; i++) {
            const VALUE k = korb_items_data(h->items)[2 * i];
            const VALUE v = korb_items_data(h->items)[2 * i + 1];
            if (SYMBOL_P(k) && (uint32_t)SYM2ID(k) == close_others_id) {
                p->close_others = KORB_TRUTHY(v);
                continue;
            }
            if (SYMBOL_P(k) && (uint32_t)SYM2ID(k) == chdir_id && KORB_STRING_P(v)) {
                uint32_t dn; const char *ds = korb_str_cstr_len(v, &dn);
                if (dn < sizeof p->chdir) { memcpy(p->chdir, ds, dn); p->chdir[dn] = '\0'; }
                continue;
            }
            const int from = korb_redir_key_fd(c, k);
            if (from < 0) continue;
            (void)korb_redir_value(c, p, from, v);
        }
        hi--;
    }
    if (UNLIKELY(hi <= lo)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");

    if (hi - lo == 1 && KORB_STRING_P(VALUE_SLICE_GET(a, lo))) {
        uint32_t n; const char *s = korb_str_cstr_len(VALUE_SLICE_GET(a, lo), &n);
        if (korb_cmd_needs_shell(s, n)) {
            p->use_shell = true;
            if (!korb_spawn_push_arg(p, "/bin/sh", 7) || !korb_spawn_push_arg(p, "-c", 2) ||
                !korb_spawn_push_arg(p, s, n))
                return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "command too long");
        } else if (!korb_spawn_split(p, s, n)) {
            return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no command given");
        }
        return RESULT_OK(KORB_NIL);
    }
    for (uint32_t i = lo; i < hi; i++) {
        const VALUE v = VALUE_SLICE_GET(a, i);
        if (KORB_ARRAY_P(v) && VAL2ARY(v)->len == 2) {          /* [cmdname, argv0] */
            const VALUE e0 = korb_items_data(VAL2ARY(v)->items)[0];
            if (UNLIKELY(!KORB_STRING_P(e0)))
                return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(e0));
            uint32_t n; const char *s = korb_str_cstr_len(e0, &n);
            if (!korb_spawn_push_arg(p, s, n)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "command too long");
            continue;
        }
        if (UNLIKELY(!KORB_STRING_P(v)))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(v));
        uint32_t n; const char *s = korb_str_cstr_len(v, &n);
        if (!korb_spawn_push_arg(p, s, n)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "command too long");
    }
    if (UNLIKELY(p->argv[0] == NULL)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "no command given");
    return RESULT_OK(KORB_NIL);
}

/* fork + exec.  Returns the child pid, or -1 with errno set. */
static pid_t korb_spawn_run(const struct korb_spawn_plan *p) {
    fflush(NULL);
    const pid_t pid = fork();
    if (pid != 0) return pid;                       /* parent (or error) */
    korb_child_reset_signals();                     /* the mask survives execve — the child must not start with signals blocked */
    if (p->chdir[0] != '\0' && chdir(p->chdir) != 0) _exit(127);
    for (uint32_t i = 0; i < p->nredir; i++) {
        const struct korb_redir *r = &p->redir[i];
        if (r->to == -2) { close(r->from); continue; }
        int fd = r->to;
        if (fd == -1) {
            fd = open(r->path, r->oflags, 0666);
            if (fd < 0) _exit(127);
        }
        if (fd != r->from) dup2(fd, r->from);
        if (r->to == -1) close(fd);
        /* An explicitly redirected descriptor must survive the exec even though
           koruby opens everything close-on-exec. */
        (void)fcntl(r->from, F_SETFD, 0);
        /* Hand the child a blocking descriptor.  O_NONBLOCK is a property of the
           open file description, so a koruby pipe (which parks rather than
           blocks) would otherwise surface as EAGAIN inside a program that has
           no idea what to do with it. */
        { const int fl = fcntl(r->from, F_GETFL);
          if (fl >= 0 && (fl & O_NONBLOCK)) (void)fcntl(r->from, F_SETFL, fl & ~O_NONBLOCK); }
    }
    if (p->close_others) {              /* close every descriptor above stderr */
        for (int fd = 3; fd < 1024; fd++) {
            bool kept = false;
            for (uint32_t i = 0; i < p->nredir; i++) if (p->redir[i].from == fd) { kept = true; break; }
            if (!kept) close(fd);
        }
    }
    for (uint32_t i = 0; p->envp[i] != NULL; i++) putenv(p->envp[i]);
    execvp(p->argv[0], p->argv);
    _exit(127);
}

/* ---- Process::Status ------------------------------------------------------ */

/* Build a Process::Status (the prelude owns the class) and publish it as $?. */
static RESULT korb_status_make(CTX *c, VALUE *slots, pid_t pid, int raw) {
    slots[0] = korb_const_get(c->vm, korb_intern(c->vm, "Process", 7));
    if (slots[0] == KORB_NIL) return RESULT_OK(KORB_NIL);
    slots[1] = LONG2FIX(pid);
    slots[2] = LONG2FIX(raw);
    const RESULT r = korb_send(c, slots + 3, korb_intern(c->vm, "__mkstatus", 10), 0, 2);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    slots[0] = r.value;
    korb_const_define(c, korb_intern(c->vm, "$?", 2), slots[0]);
    return RESULT_OK(slots[0]);
}

/* ---- Ruby entry points ---------------------------------------------------- */

static RESULT korb_m_process_spawn(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    struct korb_spawn_plan plan;
    CHECK(korb_spawn_plan_build(c, slots, a, &plan));
    korb_io_flush_std(c->vm);   /* the child inherits our write buffer: drain it first */
    const pid_t pid = korb_spawn_run(&plan);
    if (pid < 0) return korb_raise_errno(c, slots, errno, "spawn", plan.argv[0]);
    return RESULT_OK(LONG2FIX(pid));
}

/* Process.wait(pid = -1) → pid ($? gets the status).  wait2 returns [pid, status]. */
static RESULT korb_process_wait_common(CTX *c, VALUE *slots, VALUE_SLICE a, bool pair) {
    pid_t want = -1;
    int flags = 0;
    if (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) want = (pid_t)FIX2LONG(VALUE_SLICE_GET(a, 0));
    if (VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1))) flags = (int)FIX2LONG(VALUE_SLICE_GET(a, 1));
    int raw = 0;
    const pid_t got = waitpid(want, &raw, flags);
    if (got < 0) return korb_raise_errno(c, slots, errno, "waitpid", "");
    if (got == 0) return RESULT_OK(KORB_NIL);            /* WNOHANG, still running */
    slots[0] = UNWRAP(korb_status_make(c, slots, got, raw));
    if (!pair) return RESULT_OK(LONG2FIX(got));
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 2));
    VALUE_REF ar = VALUE_REF_AT(&slots[1]);
    CHECK(korb_ary_push_val(c, slots + 2, ar, LONG2FIX(got)));
    CHECK(korb_ary_push_val(c, slots + 2, ar, slots[0]));
    return RESULT_OK(VALUE_REF_GET(ar));
}
static RESULT korb_m_process_wait(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; return korb_process_wait_common(c, slots, a, false);
}
static RESULT korb_m_process_wait2(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; return korb_process_wait_common(c, slots, a, true);
}

/* Kernel#system → true (exit 0), false (non-zero), nil (could not run). */
static RESULT korb_m_kernel_system(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    struct korb_spawn_plan plan;
    CHECK(korb_spawn_plan_build(c, slots, a, &plan));
    korb_io_flush_std(c->vm);   /* the child inherits our write buffer: drain it first */
    const pid_t pid = korb_spawn_run(&plan);
    if (pid < 0) return RESULT_OK(KORB_NIL);
    int raw = 0;
    if (waitpid(pid, &raw, 0) < 0) return RESULT_OK(KORB_NIL);
    CHECK(korb_status_make(c, slots, pid, raw));
    if (WIFEXITED(raw) && WEXITSTATUS(raw) == 127) return RESULT_OK(KORB_NIL);   /* exec failed */
    return RESULT_OK((WIFEXITED(raw) && WEXITSTATUS(raw) == 0) ? KORB_TRUE : KORB_FALSE);
}

/* Kernel#` — run the command through the shell and return its stdout. */
static RESULT korb_m_kernel_backtick(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE cv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(cv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion of %s into String", korb_type_name(cv));
    char cmd[8192];
    uint32_t n; const char *s = korb_str_cstr_len(cv, &n);
    if (n >= sizeof cmd) n = sizeof cmd - 1;
    memcpy(cmd, s, n); cmd[n] = '\0';
    int fds[2];
    if (pipe(fds) != 0) return korb_raise_errno(c, slots, errno, "pipe", "");
    fflush(NULL);
    korb_io_flush_std(c->vm);   /* the child inherits our write buffer: drain it first */
    const pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return korb_raise_errno(c, slots, errno, "fork", ""); }
    if (pid == 0) {
        korb_child_reset_signals();
        close(fds[0]);
        if (fds[1] != 1) { dup2(fds[1], 1); close(fds[1]); }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    /* Accumulate into a malloc'd buffer: no Ruby allocation until the child is
       done, so nothing can move underneath us. */
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fds[0]); return korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of memory"); }
    for (;;) {
        if (len + 4096 > cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) break;
            buf = nb; cap *= 2;
        }
        const ssize_t r = read(fds[0], buf + len, cap - len);
        if (r <= 0) break;
        len += (size_t)r;
    }
    close(fds[0]);
    int raw = 0;
    waitpid(pid, &raw, 0);
    CHECK(korb_status_make(c, slots, pid, raw));
    const RESULT sr = korb_str_new(c, slots, buf, (uint32_t)len);
    free(buf);
    return sr;
}

/* Process.kill(sig, *pids) → the number of signalled processes. */
static RESULT korb_m_process_kill(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    if (UNLIKELY(VALUE_SLICE_LEN(a) < 1)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    int sig = 0;
    const VALUE sv = VALUE_SLICE_GET(a, 0);
    if (FIXNUM_P(sv)) sig = (int)FIX2LONG(sv);
    else if (KORB_STRING_P(sv) || SYMBOL_P(sv)) {
        char nm[32];
        if (SYMBOL_P(sv)) {
            const char *p = korb_sym_name(c->vm, SYM2ID(sv));
            snprintf(nm, sizeof nm, "%s", p ? p : "");
        } else {
            uint32_t n; const char *p = korb_str_cstr_len(sv, &n);
            if (n >= sizeof nm) n = sizeof nm - 1;
            memcpy(nm, p, n); nm[n] = '\0';
        }
        const char *base = strncmp(nm, "SIG", 3) == 0 ? nm + 3 : nm;
        if      (!strcmp(base, "TERM")) sig = SIGTERM;
        else if (!strcmp(base, "KILL")) sig = SIGKILL;
        else if (!strcmp(base, "INT"))  sig = SIGINT;
        else if (!strcmp(base, "HUP"))  sig = SIGHUP;
        else if (!strcmp(base, "USR1")) sig = SIGUSR1;
        else if (!strcmp(base, "USR2")) sig = SIGUSR2;
        else if (!strcmp(base, "STOP")) sig = SIGSTOP;
        else if (!strcmp(base, "CONT")) sig = SIGCONT;
        else if (!strcmp(base, "EXIT") || !strcmp(base, "0")) sig = 0;
        else return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "unsupported signal '%s'", nm);
    } else return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "bad signal type");
    uint32_t n = 0;
    for (uint32_t i = 1; i < VALUE_SLICE_LEN(a); i++) {
        const VALUE pv = VALUE_SLICE_GET(a, i);
        if (!FIXNUM_P(pv)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
        const pid_t target = (pid_t)FIX2LONG(pv);
        if (target == getpid()) CHECK(korb_kill_self(c, slots, sig));   /* deliver to ourselves, don't die */
        else if (kill(target, sig) != 0) return korb_raise_errno(c, slots, errno, "kill", "");
        n++;
    }
    return RESULT_OK(LONG2FIX(n));
}

/* IO.popen(cmd, mode = "r") [ { |io| ... } ] — pipe + fork + exec, so the IO
 * carries the child's #pid and #close reaps it into $?.  `cmd` is one positional
 * (String or argv Array); an env Hash may precede it and an options Hash follow. */
static RESULT korb_m_io_s_popen(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a,
                                struct Node *block, VALUE *def_env, VALUE *captured_self) {
    uint32_t lo = 0, hi = VALUE_SLICE_LEN(a);
    if (hi > 0 && KORB_HASH_P(VALUE_SLICE_GET(a, 0))) lo = 1;
    if (hi > lo && KORB_HASH_P(VALUE_SLICE_GET(a, hi - 1))) hi--;
    if (UNLIKELY(hi <= lo)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "wrong number of arguments");
    char mode[16] = "r";
    if (hi - lo >= 2 && !korb_io_mode_arg(VALUE_SLICE_GET(a, lo + 1), mode, sizeof mode))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid access mode");
    /* Re-slice to (env?, cmd…, opts?) so the plan builder sees no mode word.  For
     * IO.popen an Array command IS the argv list (unlike spawn's [cmd, argv0]),
     * so expand it. */
    VALUE av[64]; uint32_t an = 0;
    if (lo == 1) av[an++] = VALUE_SLICE_GET(a, 0);
    const VALUE cmdv = VALUE_SLICE_GET(a, lo);
    if (KORB_ARRAY_P(cmdv)) {
        const uint32_t cn = VAL2ARY(cmdv)->len;
        for (uint32_t i = 0; i < cn && an < 62; i++) av[an++] = korb_items_data(VAL2ARY(cmdv)->items)[i];
    } else {
        av[an++] = cmdv;
    }
    if (hi < VALUE_SLICE_LEN(a)) av[an++] = VALUE_SLICE_GET(a, VALUE_SLICE_LEN(a) - 1);
    struct korb_spawn_plan plan;
    CHECK(korb_spawn_plan_build(c, slots, VALUE_SLICE_MAKE(av, an), &plan));

    const bool reading = (mode[0] == 'r');
    int fds[2];
    if (pipe(fds) != 0) return korb_raise_errno(c, slots, errno, "pipe", "");
    /* The child gets the raw end it needs; ours stays close-on-exec. */
    (void)fcntl(fds[reading ? 0 : 1], F_SETFD, FD_CLOEXEC);
    struct korb_redir *r = &plan.redir[plan.nredir++];
    r->from = reading ? 1 : 0;
    r->to = fds[reading ? 1 : 0];
    r->path[0] = '\0'; r->oflags = 0;
    korb_io_flush_std(c->vm);   /* the child inherits our write buffer: drain it first */
    const pid_t pid = korb_spawn_run(&plan);
    if (pid < 0) { close(fds[0]); close(fds[1]); return korb_raise_errno(c, slots, errno, "fork", ""); }
    close(fds[reading ? 1 : 0]);
    slots[0] = UNWRAP(korb_io_make(c, slots, VALUE_REF_GET(self), fds[reading ? 0 : 1], reading ? 1 : 2));
    korb_io_set_nonblock(korb_io_rep(c, slots[0]));   /* our end only; the child got the other */
    VALUE_REF io = VALUE_REF_AT(&slots[0]);
    CHECK(korb_ivar_set(c, slots + 1, io, ID2SYM(korb_intern(c->vm, "@__io_pid", 9)), LONG2FIX(pid)));
    if (strchr(mode, 'b'))
        CHECK(korb_ivar_set(c, slots + 1, io, ID2SYM(korb_io_bin_mid(c)), KORB_TRUE));
    if (block == NULL) return RESULT_OK(VALUE_REF_GET(io));
    slots[1] = VALUE_REF_GET(io);
    RESULT br = korb_block_yield(c, slots + 2, block, def_env, &slots[1], 1, captured_self);
    slots[1] = br.value;                        /* root across close's GC */
    slots[2] = VALUE_REF_GET(io);
    RESULT cr = korb_send(c, slots + 3, korb_intern(c->vm, "close", 5), 0, 0);
    if (br.state != KORB_NORMAL) { br.value = slots[1]; return br; }
    if (cr.state != KORB_NORMAL) return cr;
    return RESULT_OK(slots[1]);
}

static RESULT korb_m_process_getpgid(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const pid_t pid = (VALUE_SLICE_LEN(a) >= 1 && FIXNUM_P(VALUE_SLICE_GET(a, 0))) ? (pid_t)FIX2LONG(VALUE_SLICE_GET(a, 0)) : 0;
    const pid_t g = getpgid(pid);
    if (g < 0) return korb_raise_errno(c, slots, errno, "getpgid", "");
    return RESULT_OK(LONG2FIX(g));
}
static RESULT korb_m_process_setpgid(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    if (setpgid((pid_t)FIX2LONG(VALUE_SLICE_GET(a, 0)), (pid_t)FIX2LONG(VALUE_SLICE_GET(a, 1))) < 0)
        return korb_raise_errno(c, slots, errno, "setpgid", "");
    return RESULT_OK(LONG2FIX(0));
}

/* Signal name ("INT" / "SIGINT" / :INT / 2) → signal number, or -1. */
static int korb_signo_of(CTX *c, VALUE v) {
    if (FIXNUM_P(v)) return (int)FIX2LONG(v);
    char nm[32] = "";
    if (SYMBOL_P(v)) {
        const char *p = korb_sym_name(c->vm, SYM2ID(v));
        snprintf(nm, sizeof nm, "%s", p ? p : "");
    } else if (KORB_STRING_P(v)) {
        uint32_t n; const char *p = korb_str_cstr_len(v, &n);
        if (n >= sizeof nm) n = sizeof nm - 1;
        memcpy(nm, p, n); nm[n] = '\0';
    } else return -1;
    const char *b = strncmp(nm, "SIG", 3) == 0 ? nm + 3 : nm;
    static const struct { const char *n; int s; } tab[] = {
        {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"ILL", SIGILL},
        {"TRAP", SIGTRAP}, {"ABRT", SIGABRT}, {"FPE", SIGFPE}, {"KILL", SIGKILL},
        {"BUS", SIGBUS}, {"SEGV", SIGSEGV}, {"SYS", SIGSYS}, {"PIPE", SIGPIPE},
        {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"URG", SIGURG}, {"STOP", SIGSTOP},
        {"TSTP", SIGTSTP}, {"CONT", SIGCONT}, {"CHLD", SIGCHLD}, {"TTIN", SIGTTIN},
        {"TTOU", SIGTTOU}, {"XCPU", SIGXCPU}, {"XFSZ", SIGXFSZ}, {"VTALRM", SIGVTALRM},
        {"PROF", SIGPROF}, {"WINCH", SIGWINCH}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2},
        {"EXIT", 0},
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (!strcmp(b, tab[i].n)) return tab[i].s;
    return -1;
}

/* __signal_trap(sig, command) → the previous command String.
 * "IGNORE"/"SIG_IGN" and "DEFAULT"/"SIG_DFL" are installed for real; a Proc or
 * "" is accepted and the signal ignored, because koruby has no safe point to run
 * Ruby from a signal handler yet (see docs/todo.md).  Without this a spec that
 * signals its own process just kills the interpreter. */
static RESULT korb_m_signal_trap(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int sig = korb_signo_of(c, VALUE_SLICE_GET(a, 0));
    if (sig < 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "unsupported signal");
    if (sig == 0 || sig == SIGKILL || sig == SIGSTOP)   /* not trappable */
        return korb_str_new(c, slots, "DEFAULT", 7);
    const VALUE cmd = VALUE_SLICE_LEN(a) >= 2 ? VALUE_SLICE_GET(a, 1) : KORB_NIL;
    bool dflt = false;
    if (KORB_STRING_P(cmd)) {
        uint32_t n; const char *p = korb_str_cstr_len(cmd, &n);
        dflt = (n == 7 && !memcmp(p, "DEFAULT", 7)) || (n == 7 && !memcmp(p, "SIG_DFL", 7));
    }
    void (*prev)(int) = signal(sig, dflt ? SIG_DFL : SIG_IGN);
    const char *pname = prev == SIG_DFL ? "DEFAULT" : prev == SIG_IGN ? "IGNORE" : "DEFAULT";
    return korb_str_new(c, slots, pname, (uint32_t)strlen(pname));
}

static RESULT korb_m_signal_signame(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    if (!FIXNUM_P(VALUE_SLICE_GET(a, 0))) return RESULT_OK(KORB_NIL);
    const int want = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    static const struct { const char *n; int s; } tab[] = {
        {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"ILL", SIGILL},
        {"TRAP", SIGTRAP}, {"ABRT", SIGABRT}, {"FPE", SIGFPE}, {"KILL", SIGKILL},
        {"BUS", SIGBUS}, {"SEGV", SIGSEGV}, {"SYS", SIGSYS}, {"PIPE", SIGPIPE},
        {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"URG", SIGURG}, {"STOP", SIGSTOP},
        {"TSTP", SIGTSTP}, {"CONT", SIGCONT}, {"CHLD", SIGCHLD}, {"TTIN", SIGTTIN},
        {"TTOU", SIGTTOU}, {"XCPU", SIGXCPU}, {"XFSZ", SIGXFSZ}, {"VTALRM", SIGVTALRM},
        {"PROF", SIGPROF}, {"WINCH", SIGWINCH}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2},
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (tab[i].s == want) return korb_str_new(c, slots, tab[i].n, (uint32_t)strlen(tab[i].n));
    return RESULT_OK(KORB_NIL);
}

static RESULT korb_m_signal_signo(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)slots; (void)self;
    const int s = korb_signo_of(c, VALUE_SLICE_GET(a, 0));
    return RESULT_OK(s < 0 ? KORB_NIL : LONG2FIX(s));
}

void korb_init_process(CTX *c, VALUE *slots) {
    (void)slots;
    /* The Process module itself comes from the prelude, which loads after this,
     * so the primitives go on Object and prelude/system.rb wires Process.spawn /
     * .wait / .kill to them. */
    const VALUE obj = korb_builtin_class_obj(c->vm, KORB_C_OBJECT);
    korb_class_def_cfn(c, obj, "system",     korb_m_kernel_system,   -1);
    korb_class_def_cfn(c, obj, "spawn",      korb_m_process_spawn,   -1);
    korb_class_def_cfn(c, obj, "`",          korb_m_kernel_backtick,  1);
    korb_class_def_cfn(c, obj, "__spawn",    korb_m_process_spawn,   -1);
    korb_class_def_cfn(c, obj, "__waitpid",  korb_m_process_wait,    -1);
    korb_class_def_cfn(c, obj, "__waitpid2", korb_m_process_wait2,   -1);
    korb_class_def_cfn(c, obj, "__kill",     korb_m_process_kill,    -1);
    korb_class_def_cfn(c, obj, "__getpgid",  korb_m_process_getpgid, -1);
    korb_class_def_cfn(c, obj, "__setpgid",  korb_m_process_setpgid,  2);
    korb_class_def_cfn(c, obj, "__signal_trap",    korb_m_signal_trap,    -1);
    korb_class_def_cfn(c, obj, "__signal_block",   korb_m_signal_block,   -1);
    korb_class_def_cfn(c, obj, "__signal_signame", korb_m_signal_signame,  1);
    korb_class_def_cfn(c, obj, "__signal_signo",   korb_m_signal_signo,    1);
    const VALUE io_cls = korb_const_get(c->vm, korb_intern(c->vm, "IO", 2));
    if (KORB_CLASS_P(io_cls)) {
        VALUE sl[4]; sl[0] = io_cls;
        const VALUE io_sing = korb_obj_singleton(c, sl + 1, io_cls).value;
        korb_class_def_cfn_blk(c, io_sing, "popen", korb_m_io_s_popen, -1);
    }
}
