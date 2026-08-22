/* WASI にプロセスは無い。waitpid は必ず失敗する (wasi_compat.c に実体)。 */
#ifndef KORB_WASI_WAIT_H
#define KORB_WASI_WAIT_H
#include <sys/types.h>
#define WNOHANG   1
#define WUNTRACED 2
#define WIFEXITED(s)     (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)   (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)   (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)      ((s) & 0x7f)
#define WIFSTOPPED(s)    (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)      WEXITSTATUS(s)
#define WCOREDUMP(s)     0
pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait(int *status);
#endif
