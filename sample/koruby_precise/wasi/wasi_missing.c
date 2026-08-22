/* koruby_precise — WASI に無い POSIX 関数。
 *
 * 方針は CRuby の wasm/missing.c に合わせる:
 *   ID 系 (getuid/geteuid/getgid/getegid/getppid) と umask → 0 を返す
 *   操作系 (chown/chmod/dup/mkfifo/...)              → errno = ENOTSUP, -1
 *
 * CRuby と同じく weak にしてあるが、**判定を weak に任せてはいない**。
 * この翻訳単位は wasm ビルドでしか混ぜない。全ビルドに入れると、Linux でも
 * 「未定義シンボルが無いので libc のメンバが引かれず、こちらの stub が勝つ」
 * ことになり得る (geteuid が常に 0 を返す Linux バイナリができる)。
 * weak なのは将来 wasi-libc がこれらを実装したときの保険。 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define KORB_WASI_MISSING __attribute__((weak))

KORB_WASI_MISSING uid_t geteuid(void)  { return 0; }
KORB_WASI_MISSING uid_t getuid(void)   { return 0; }
KORB_WASI_MISSING gid_t getegid(void)  { return 0; }
KORB_WASI_MISSING gid_t getgid(void)   { return 0; }
KORB_WASI_MISSING pid_t getppid(void)  { return 0; }
KORB_WASI_MISSING pid_t getpid(void)   { return 0; }
KORB_WASI_MISSING mode_t umask(mode_t mask) { (void)mask; return 0; }

KORB_WASI_MISSING int getgroups(int size, gid_t list[]) {
    (void)size; (void)list;
    return 0;                          /* 所属グループ無し (エラーではない) */
}
KORB_WASI_MISSING int chown(const char *path, uid_t owner, gid_t group) {
    (void)path; (void)owner; (void)group; errno = ENOTSUP; return -1;
}
KORB_WASI_MISSING int chmod(const char *path, mode_t mode) {
    (void)path; (void)mode; errno = ENOTSUP; return -1;
}
KORB_WASI_MISSING int mkfifo(const char *path, mode_t mode) {
    (void)path; (void)mode; errno = ENOTSUP; return -1;
}
KORB_WASI_MISSING int kill(pid_t pid, int sig) {
    (void)pid; (void)sig; errno = ENOTSUP; return -1;
}
KORB_WASI_MISSING pid_t waitpid(pid_t pid, int *status, int options) {
    (void)pid; (void)status; (void)options; errno = ECHILD; return -1;
}
KORB_WASI_MISSING pid_t wait(int *status) {
    (void)status; errno = ECHILD; return -1;
}
KORB_WASI_MISSING pid_t fork(void)      { errno = ENOTSUP; return -1; }
KORB_WASI_MISSING int   pipe(int fds[2]) { (void)fds; errno = ENOTSUP; return -1; }
KORB_WASI_MISSING int   pipe2(int fds[2], int flags) { (void)fds; (void)flags; errno = ENOTSUP; return -1; }
KORB_WASI_MISSING int   execvp(const char *f, char *const a[]) { (void)f; (void)a; errno = ENOTSUP; return -1; }
KORB_WASI_MISSING int   dup(int fd)             { (void)fd; errno = ENOTSUP; return -1; }
KORB_WASI_MISSING int   dup2(int o, int n)      { (void)o; (void)n; errno = ENOTSUP; return -1; }
KORB_WASI_MISSING int   setpgid(pid_t p, pid_t g) { (void)p; (void)g; errno = ENOTSUP; return -1; }
KORB_WASI_MISSING pid_t getpgrp(void)           { return 0; }
KORB_WASI_MISSING char *getlogin(void)          { errno = ENOTSUP; return NULL; }
KORB_WASI_MISSING char *crypt(const char *k, const char *s) { (void)k; (void)s; errno = ENOTSUP; return NULL; }

/* dlopen 一式: AOT の code store は wasm では使えない (実行時コンパイルが
 * できないので、そもそも呼ばれない経路)。 */
KORB_WASI_MISSING void *dlopen(const char *f, int flag) { (void)f; (void)flag; return NULL; }
KORB_WASI_MISSING void *dlsym(void *h, const char *s)   { (void)h; (void)s; return NULL; }
KORB_WASI_MISSING int   dlclose(void *h)                { (void)h; return 0; }
KORB_WASI_MISSING char *dlerror(void)                   { return (char *)"dlopen is not available on WASI"; }

KORB_WASI_MISSING void (*signal(int sig, void (*handler)(int)))(int) {
    (void)sig; (void)handler; return NULL;      /* シグナルは配送されない */
}
KORB_WASI_MISSING int gettid(void) { return 0; }

/* qsort_r: wasi-libc には無い。単一スレッドなので、比較関数の引数を
 * 静的に持って libc の qsort に流す。比較関数の中で更に qsort_r が
 * 呼ばれても (Array#sort_by のブロックが sort する等)、内側が退避・復元
 * するので外側は壊れない。 */
static int (*korb_qsr_cmp)(const void *, const void *, void *);
static void *korb_qsr_arg;
static int korb_qsr_tramp(const void *a, const void *b) { return korb_qsr_cmp(a, b, korb_qsr_arg); }
KORB_WASI_MISSING void qsort_r(void *base, size_t n, size_t size,
                               int (*cmp)(const void *, const void *, void *), void *arg) {
    int (*const saved_cmp)(const void *, const void *, void *) = korb_qsr_cmp;
    void *const saved_arg = korb_qsr_arg;
    korb_qsr_cmp = cmp; korb_qsr_arg = arg;
    qsort(base, n, size, korb_qsr_tramp);
    korb_qsr_cmp = saved_cmp; korb_qsr_arg = saved_arg;
}

KORB_WASI_MISSING int madvise(void *addr, size_t len, int advice) {
    (void)addr; (void)len; (void)advice; return 0;   /* 助言なので無視でよい */
}
KORB_WASI_MISSING int system(const char *cmd) { (void)cmd; errno = ENOTSUP; return -1; }
