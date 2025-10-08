# FreeBSD kernel module Makefile for BFCFS
# Build with: make
# Load with: kldload ./bfcfs.ko

KMOD=	bfcfs
SRCS=	bfcfs_vfs.c bfcfs_vnops.c bfc_format.c bfc_io.c
SRCS+=	vnode_if.h opt_compat.h

# Compilation flags
CFLAGS+= -O2 -fno-strict-aliasing -DKLD_MODULE
CFLAGS+= -Wall -Wextra -Wno-unused-parameter

# Optional debug flag (make DEBUG=1)
.if defined(DEBUG)
CFLAGS+= -DDEBUG
.endif

.include <bsd.kmod.mk>
