/* wasi-libc has no <termios.h>: WASI gives no terminal control.  Just enough
 * for builtins/io.c's io/console to compile; tcgetattr / tcsetattr fail with
 * ENOTTY (wasi_missing.c), so the console methods raise as on a non-tty. */
#ifndef KORB_WASI_TERMIOS_H
#define KORB_WASI_TERMIOS_H

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
#define NCCS 32
struct termios { tcflag_t c_iflag, c_oflag, c_cflag, c_lflag; cc_t c_line; cc_t c_cc[NCCS]; };

/* Linux values, so a saved state reads the same as on the host */
#define VTIME 5
#define VMIN  6
#define IGNBRK  0000001
#define BRKINT  0000002
#define PARMRK  0000010
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IXON    0002000
#define IXANY   0004000
#define IXOFF   0010000
#define IMAXBEL 0020000
#define OPOST   0000001
#define CSIZE   0000060
#define CS8     0000060
#define PARENB  0000400
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define IEXTEN  0100000
#define TCSANOW 0

struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#ifndef TIOCGWINSZ
#  define TIOCGWINSZ 0x5413
#  define TIOCSWINSZ 0x5414
#endif

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int act, const struct termios *t);

#endif
