/* WASI にはデバイス番号の概念が無い。File::Stat#dev_major などが 0 を返す。 */
#ifndef KORB_WASI_SYSMACROS_H
#define KORB_WASI_SYSMACROS_H
#define major(x)     ((int)(((x) >> 8) & 0xfff))
#define minor(x)     ((int)((x) & 0xff))
#define makedev(a,b) ((dev_t)(((a) << 8) | (b)))
#endif
