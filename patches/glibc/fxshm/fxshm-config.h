/* fexdroid: how glibc starts the shared-memory registry daemon when no
   fxshmd is listening.  scripts/build-glibc.sh rewrites both paths (sed) to
   the on-device rootfs before building; the socket path itself derives from
   _PATH_TMP (also rewritten there).  */
#ifndef _FXSHM_CONFIG_H
#define _FXSHM_CONFIG_H 1

#define FXSHM_LDSO   "/lib/ld-linux-aarch64.so.1"
#define FXSHM_DAEMON "/usr/libexec/fxshmd"

#endif /* fxshm-config.h */
