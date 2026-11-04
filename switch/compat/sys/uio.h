/* MPH-nx compat: newlib on Horizon has no <sys/uio.h>. libslirp only needs
   struct iovec, which libnx already provides in <sys/_iovec.h>. */
#ifndef MPHNX_SYS_UIO_H
#define MPHNX_SYS_UIO_H
#include <sys/types.h>
#include <sys/_iovec.h>
#endif
