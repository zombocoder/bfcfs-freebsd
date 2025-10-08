/*-
 *
 * Copyright (c) 2025 BFCFS Project
 *
 * FreeBSD kernel module for mounting BFC (Binary File Container) images
 */

#ifndef _FS_BFCFS_BFCFS_H_
#define _FS_BFCFS_BFCFS_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/proc.h>

/*
 * BFC Format Constants (from BFC spec)
 */
#define BFC_MAGIC		0x46434642	/* "BFCF" little-endian */
#define BFC_HEADER_SIZE		4096
#define BFC_FOOTER_SIZE		56
#define BFC_ALIGN		16

/* BFC object types */
#define BFC_TYPE_FILE		1
#define BFC_TYPE_DIR		2
#define BFC_TYPE_SYMLINK	3

/* Compression types */
#define BFC_COMP_NONE		0
#define BFC_COMP_ZSTD		1

/* Encryption types */
#define BFC_ENC_NONE		0
#define BFC_ENC_CHACHA20_POLY1305	1

/* Error codes */
#define BFC_OK			0
#define BFC_E_BADMAGIC		-1
#define BFC_E_IO		-2
#define BFC_E_CRC		-3
#define BFC_E_INVAL		-4
#define BFC_E_EXISTS		-5
#define BFC_E_NOTFOUND		-6
#define BFC_E_PERM		-7

/*
 * BFC on-disk structures
 */
struct bfc_header {
	char magic[8];			/* "BFCFv1\0\0" */
	uint32_t header_crc32;		/* CRC32 of remaining header */
	uint32_t block_size;		/* Alignment boundary */
	uint64_t features;		/* Feature flags */
	uint8_t  uuid[16];		/* RFC 4122 v4 UUID */
	uint8_t  enc_salt[32];		/* Salt for key derivation */
	uint8_t  reserved[4024];	/* Zero-filled */
} __packed;

struct bfc_footer {
	char magic_start[8];		/* "BFCFIDX\0" */
	uint64_t index_size;		/* Size comes FIRST */
	uint32_t index_crc32;		/* CRC32 */
	uint64_t index_offset;		/* Offset comes AFTER size */
	uint32_t container_crc;		/* Additional field */
	uint8_t  reserved[16];		/* 16 bytes reserved */
	char magic_end[8];		/* "BFCFEND\0" */
} __packed;

struct bfc_obj_header {
	uint8_t  type;			/* Object type */
	uint8_t  comp;			/* Compression type */
	uint8_t  enc;			/* Encryption type */
	uint8_t  reserved;		/* Reserved */
	uint16_t name_len;		/* Name length in bytes */
	uint16_t padding;		/* Padding for alignment */
	uint32_t mode;			/* POSIX mode bits */
	uint64_t mtime_ns;		/* Modification time (ns) */
	uint64_t orig_size;		/* Original size */
	uint64_t enc_size;		/* Encoded size */
	uint32_t crc32c;		/* CRC32C of original content */
} __packed;

struct bfc_entry {
	const char *path;		/* UTF-8 path */
	uint32_t mode;			/* POSIX mode bits */
	uint64_t mtime_ns;		/* Modification time (ns) */
	uint32_t comp;			/* Compression type */
	uint32_t enc;			/* Encryption type */
	uint64_t size;			/* Uncompressed size */
	uint32_t crc32c;		/* CRC32C checksum */
	uint64_t obj_offset;		/* Offset in container */
	uint64_t obj_size;		/* Object size on disk */
};

/*
 * In-memory BFC index entry (simplified for kernel use)
 */
struct bfcfs_index_entry {
	char *path;			/* Allocated path string */
	uint32_t mode;
	uint64_t mtime_ns;
	uint32_t comp;
	uint32_t enc;
	uint64_t size;
	uint32_t crc32c;
	uint64_t obj_offset;
	uint64_t obj_size;
	ino_t ino;			/* Synthesized inode number */
	LIST_ENTRY(bfcfs_index_entry) entries;
};

/*
 * Per-mount BFC filesystem context
 */
struct bfcfs_mount {
	struct mount *mp;		/* Back pointer to mount */
	struct vnode *devvp;		/* Backing .bfc file vnode */
	struct vnode *rootvp;		/* Cached root vnode */
	struct mtx bfc_mtx;		/* Protects mount state */

	/* BFC container metadata */
	struct bfc_header header;
	struct bfc_footer footer;
	uint32_t num_entries;

	/* In-memory index (sorted by path) */
	struct bfcfs_index_entry *index;	/* Array of entries */
	LIST_HEAD(, bfcfs_index_entry) index_list;

	/* Mount options */
	uint8_t encryption_key[32];
	int has_encryption_key;
	int verify_crc;
	int no_readahead;

	/* Statistics */
	uint64_t reads;
	uint64_t bytes_read;
};

/*
 * Per-vnode BFC node data (v_data points here)
 */
struct bfcfs_node {
	struct bfcfs_mount *bmp;	/* Back pointer to mount */
	struct bfcfs_index_entry *entry; /* Index entry for this node */
	struct vnode *vnode;		/* Back pointer to vnode */
};

/*
 * Mount argument structure (from userland mount(8))
 */
struct bfcfs_args {
	char *source;			/* Path to .bfc file */
	char *encryption_key;		/* Optional encryption key */
	int verify;			/* Verify CRC on reads */
	int no_readahead;		/* Disable readahead */
};

/* Malloc type declaration */
MALLOC_DECLARE(M_BFCFS);

/*
 * Function prototypes
 */

/* bfcfs_vfs.c - VFS operations */
extern struct vfsops bfcfs_vfsops;

/* bfcfs_vnops.c - Vnode operations */
extern struct vop_vector bfcfs_vnodeops;
int bfcfs_vnode_create(struct bfcfs_mount *bmp, struct bfcfs_index_entry *entry,
    struct vnode **vpp);

/* bfc_format.c - BFC format parsing */
int bfc_read_header(struct bfcfs_mount *bmp, struct thread *td);
int bfc_read_index(struct bfcfs_mount *bmp, struct thread *td);
struct bfcfs_index_entry *bfc_lookup_path(struct bfcfs_mount *bmp, const char *path);
int bfc_readdir(struct bfcfs_mount *bmp, const char *dir_path,
    struct bfcfs_index_entry ***entries, int *count);
void bfc_free_index(struct bfcfs_mount *bmp);

/* bfc_io.c - BFC I/O helpers */
int bfc_pread(struct bfcfs_mount *bmp, void *buf, size_t len, off_t offset,
    struct thread *td);
int bfc_read_object(struct bfcfs_mount *bmp, struct bfcfs_index_entry *entry,
    void *buf, size_t len, off_t offset, struct thread *td);
int bfc_get_file_size(struct bfcfs_mount *bmp, off_t *size, struct thread *td);

/*
 * Utility macros
 */
#define BFCFS_LOCK(bmp)		mtx_lock(&(bmp)->bfc_mtx)
#define BFCFS_UNLOCK(bmp)	mtx_unlock(&(bmp)->bfc_mtx)
#define BFCFS_LOCK_ASSERT(bmp)	mtx_assert(&(bmp)->bfc_mtx, MA_OWNED)

#define VFSTOBFCFS(mp)		((struct bfcfs_mount *)((mp)->mnt_data))
#define VTOBFCFS(vp)		((struct bfcfs_node *)((vp)->v_data))

/* Debug printf */
#ifdef DEBUG
#define BFCFS_DEBUG(fmt, ...)	printf("bfcfs: " fmt "\n", ##__VA_ARGS__)
#else
#define BFCFS_DEBUG(fmt, ...)	do { } while (0)
#endif

#endif /* !_FS_BFCFS_BFCFS_H_ */
