/* wasi-libc が宣言しない POSIX 関数の宣言。実体は wasi/wasi_missing.c。
 * -include で全 TU の先頭に入れる (ヘッダを書き換えずに済ませるため)。 */
#ifndef KORB_WASI_DECLS_H
#define KORB_WASI_DECLS_H
#include <sys/types.h>
#include <stdio.h>
uid_t  geteuid(void);
uid_t  getuid(void);
gid_t  getegid(void);
gid_t  getgid(void);
pid_t  getppid(void);
pid_t  getpid(void);
pid_t  getpgrp(void);
mode_t umask(mode_t mask);
int    getgroups(int size, gid_t list[]);
int    chown(const char *path, uid_t owner, gid_t group);
int    chmod(const char *path, mode_t mode);
int    mkfifo(const char *path, mode_t mode);
int    kill(pid_t pid, int sig);
pid_t  fork(void);
int    pipe(int fds[2]);
int    pipe2(int fds[2], int flags);
int    execvp(const char *file, char *const argv[]);
int    dup(int fd);
int    dup2(int oldfd, int newfd);
int    setpgid(pid_t pid, pid_t pgid);
char  *getlogin(void);
char  *crypt(const char *key, const char *salt);
void  *dlopen(const char *file, int flag);
void  *dlsym(void *handle, const char *sym);
int    dlclose(void *handle);
char  *dlerror(void);

/* signal: wasi-emulated-signal は SIGPIPE を持たない。koruby は
 * "書き込み先が消えても落ちない" ためだけに使うので、無視で足りる。 */
#ifndef SIGPIPE
#  define SIGPIPE 13
#endif
#ifndef SIG_IGN
#  define SIG_IGN ((void (*)(int))1)
#endif
void (*signal(int sig, void (*handler)(int)))(int);
int  gettid(void);
int  madvise(void *addr, size_t len, int advice);
void qsort_r(void *base, size_t n, size_t size,
             int (*cmp)(const void *, const void *, void *), void *arg);

#endif
