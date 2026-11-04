/* MPH-nx compat: libnx's <net/if.h> lacks if_nametoindex. libslirp calls it
   only for IPv6 scope ids; the Switch build runs with networking off. */
#ifndef MPHNX_NET_IF_H
#define MPHNX_NET_IF_H
#include_next <net/if.h>
#ifdef __cplusplus
extern "C" {
#endif
static inline unsigned int if_nametoindex(const char *ifname) { (void)ifname; return 0; }
#ifdef __cplusplus
}
#endif
#endif
