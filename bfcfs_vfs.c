/*-
 *
 * Copyright (c) 2025 BFCFS Project
 *
 * VFS operations for BFCFS - mount, unmount, root, statfs
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/namei.h>
#include <sys/fcntl.h>
#include <sys/priv.h>
#include <sys/stat.h>
#include <sys/conf.h>
#include <sys/sysctl.h>

#include "bfcfs.h"

/* Malloc type for BFCFS allocations */
MALLOC_DEFINE(M_BFCFS, "bfcfs", "BFCFS filesystem structures");

/* Forward declarations */
static vfs_mount_t	bfcfs_mount;
static vfs_unmount_t	bfcfs_unmount;
static vfs_root_t	bfcfs_root;
static vfs_statfs_t	bfcfs_statfs;
static vfs_init_t	bfcfs_init;
static vfs_uninit_t	bfcfs_uninit;

/* VFS operations vector */
struct vfsops bfcfs_vfsops = {
	.vfs_init	= bfcfs_init,
	.vfs_uninit	= bfcfs_uninit,
	.vfs_mount	= bfcfs_mount,
	.vfs_unmount	= bfcfs_unmount,
	.vfs_root	= bfcfs_root,
	.vfs_statfs	= bfcfs_statfs,
};

VFS_SET(bfcfs_vfsops, bfcfs, VFCF_READONLY);

MODULE_VERSION(bfcfs, 1);

/*
 * VFS initialization - called once when module loads
 */
int
bfcfs_init(struct vfsconf *vfsp)
{
	BFCFS_DEBUG("init: BFCFS initializing");
	return (0);
}

/*
 * VFS cleanup - called when module unloads
 */
int
bfcfs_uninit(struct vfsconf *vfsp)
{
	BFCFS_DEBUG("uninit: BFCFS cleaning up");
	return (0);
}

/*
 * Mount a BFC container
 * Called by mount(2) with: mount -t bfcfs -o source=/path/to/file.bfc /mnt
 */
static int
bfcfs_mount(struct mount *mp)
{
	struct bfcfs_mount *bmp;
	struct nameidata nd;
	struct vnode *devvp;
	struct thread *td = curthread;
	char *source_path;
	void *opt_ptr;
	int opt_len;
	int error, flags;

	/* Only allow read-only mounts */
	if ((mp->mnt_flag & MNT_RDONLY) == 0)
		return (EROFS);

	/* Update mounts not supported */
	if (mp->mnt_flag & MNT_UPDATE)
		return (EOPNOTSUPP);

	/* Parse mount options: try "source" option first, then "from" (device arg) */
	error = vfs_getopt(mp->mnt_optnew, "source", &opt_ptr, &opt_len);
	if (error || opt_ptr == NULL) {
		/* Try to get the device path from "from" option */
		error = vfs_getopt(mp->mnt_optnew, "from", &opt_ptr, &opt_len);
		if (error || opt_ptr == NULL) {
			BFCFS_ERR("mount requires device path or -o source=/path/to/file.bfc");
			return (EINVAL);
		}
	}

	/* Copy source path (null-terminated) */
	source_path = malloc(opt_len + 1, M_BFCFS, M_WAITOK);
	bcopy(opt_ptr, source_path, opt_len);
	source_path[opt_len] = '\0';

	BFCFS_DEBUG("mount: opening container: %s", source_path);

	/* Open the backing .bfc file */
	NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, source_path);
	flags = FREAD;
	error = vn_open(&nd, &flags, 0, NULL);
	if (error) {
		free(source_path, M_BFCFS);
		BFCFS_ERR("failed to open %s: error %d", source_path, error);
		return (error);
	}
	NDFREE_PNBUF(&nd);

	devvp = nd.ni_vp;
	VOP_UNLOCK(devvp);

	/* Verify it's a regular file */
	if (devvp->v_type != VREG) {
		vn_close(devvp, FREAD, td->td_ucred, td);
		free(source_path, M_BFCFS);
		return (EINVAL);
	}

	/* Allocate mount structure */
	bmp = malloc(sizeof(struct bfcfs_mount), M_BFCFS, M_WAITOK | M_ZERO);
	bmp->mp = mp;
	bmp->devvp = devvp;
	BFCFS_DEBUG("mount: bmp=%p, bmp->mp=%p, mp=%p", bmp, bmp->mp, mp);
	mtx_init(&bmp->bfc_mtx, "bfcfs mount lock", NULL, MTX_DEF);
	LIST_INIT(&bmp->index_list);

	/* Parse optional mount options */
	bmp->verify_crc = 1;  /* Default: verify CRC enabled */
	bmp->no_readahead = 0;  /* Default: readahead enabled */
	if (vfs_getopt(mp->mnt_optnew, "noverify", NULL, NULL) == 0)
		bmp->verify_crc = 0;
	if (vfs_getopt(mp->mnt_optnew, "noreadahead", NULL, NULL) == 0)
		bmp->no_readahead = 1;

	/* Read BFC header and validate */
	error = bfc_read_header(bmp, td);
	if (error) {
		BFCFS_ERR("invalid BFC header: error %d", error);
		goto fail;
	}

	/* Read and parse the index */
	error = bfc_read_index(bmp, td);
	if (error) {
		BFCFS_ERR("failed to read index: error %d", error);
		goto fail;
	}

	BFCFS_DEBUG("mount: loaded %u entries from %s", bmp->num_entries, source_path);

	/* Set up mount point */
	mp->mnt_data = bmp;
	mp->mnt_flag |= MNT_LOCAL | MNT_RDONLY;
	mp->mnt_kern_flag |= MNTK_LOOKUP_SHARED | MNTK_EXTENDED_SHARED;

	vfs_getnewfsid(mp);
	vfs_mountedfrom(mp, source_path);
	free(source_path, M_BFCFS);

	return (0);

fail:
	mtx_destroy(&bmp->bfc_mtx);
	vn_close(devvp, FREAD, td->td_ucred, td);
	free(bmp, M_BFCFS);
	free(source_path, M_BFCFS);
	return (error);
}

/*
 * Unmount a BFC filesystem
 */
static int
bfcfs_unmount(struct mount *mp, int mntflags)
{
	struct bfcfs_mount *bmp = VFSTOBFCFS(mp);
	struct thread *td = curthread;
	int error, flags = 0;

	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;

	BFCFS_DEBUG("unmount: flushing vnodes");

	/* Release cached root vnode */
	if (bmp->rootvp) {
		vrele(bmp->rootvp);
		bmp->rootvp = NULL;
	}

	/* Flush all vnodes for this mount */
	error = vflush(mp, 0, flags, td);
	if (error)
		return (error);

	/* Close backing file */
	if (bmp->devvp) {
		vn_close(bmp->devvp, FREAD, td->td_ucred, td);
		bmp->devvp = NULL;
	}

	/* Free index */
	bfc_free_index(bmp);

	/* Clean up mount structure */
	mtx_destroy(&bmp->bfc_mtx);
	free(bmp, M_BFCFS);
	mp->mnt_data = NULL;

	BFCFS_DEBUG("unmount: complete");
	return (0);
}

/*
 * Get root vnode for this mount
 */
static int
bfcfs_root(struct mount *mp, int flags, struct vnode **vpp)
{
	struct bfcfs_mount *bmp = VFSTOBFCFS(mp);
	struct bfcfs_index_entry *root_entry;
	struct vnode *vp;
	int error;

	BFCFS_DEBUG("root: mp=%p, bmp=%p, rootvp=%p", mp, bmp, bmp ? bmp->rootvp : NULL);

	/* Check if we already have a cached root vnode */
	BFCFS_LOCK(bmp);
	vp = bmp->rootvp;
	if (vp != NULL) {
		/* Root vnode exists, get a reference and lock it */
		vref(vp);
		BFCFS_UNLOCK(bmp);
		vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
		*vpp = vp;
		BFCFS_DEBUG("root: returning cached root vnode");
		return (0);
	}
	BFCFS_UNLOCK(bmp);

	BFCFS_DEBUG("root: creating new root vnode");

	/* Look up root directory "/" in index */
	root_entry = bfc_lookup_path(bmp, "/");
	if (root_entry == NULL) {
		/*
		 * No explicit root directory entry - create synthetic root.
		 * This is the normal case for BFC containers that store only
		 * files without an explicit "/" entry.
		 */
		BFCFS_DEBUG("root: creating synthetic root directory");
		error = bfcfs_vnode_create(bmp, NULL, vpp);
		if (error) {
			BFCFS_ERR("failed to create root vnode: %d", error);
			return (error);
		}
	} else {
		/* Create/get vnode for explicit root entry */
		error = bfcfs_vnode_create(bmp, root_entry, vpp);
		if (error) {
			BFCFS_ERR("failed to create root vnode: %d", error);
			return (error);
		}
	}

	/* Cache the root vnode */
	BFCFS_LOCK(bmp);
	if (bmp->rootvp == NULL) {
		bmp->rootvp = *vpp;
		vref(*vpp);  /* Add reference for the cache */
	}
	BFCFS_UNLOCK(bmp);

	return (0);
}

/*
 * Get filesystem statistics
 */
static int
bfcfs_statfs(struct mount *mp, struct statfs *sbp)
{
	struct bfcfs_mount *bmp = VFSTOBFCFS(mp);

	sbp->f_bsize = bmp->header.block_size;
	sbp->f_iosize = BFC_DEFAULT_IO_SIZE;
	sbp->f_blocks = 0;      /* Unknown (container is append-only) */
	sbp->f_bfree = 0;       /* Read-only */
	sbp->f_bavail = 0;      /* Read-only */
	sbp->f_files = bmp->num_entries;
	sbp->f_ffree = 0;       /* Read-only */

	return (0);
}
