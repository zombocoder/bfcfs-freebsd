/*-
 *
 * Copyright (c) 2025 BFCFS Project
 *
 * Vnode operations for BFCFS - lookup, getattr, read, readdir, etc.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <sys/buf.h>
#include <sys/bio.h>
#include <vm/vm.h>
#include <vm/vnode_pager.h>

#include "bfcfs.h"

/* Forward declarations */
static vop_lookup_t	bfcfs_lookup;
static vop_getattr_t	bfcfs_getattr;
static vop_open_t	bfcfs_open;
static vop_close_t	bfcfs_close;
static vop_access_t	bfcfs_access;
static vop_read_t	bfcfs_read;
static vop_readdir_t	bfcfs_readdir;
static vop_reclaim_t	bfcfs_reclaim;
static vop_strategy_t	bfcfs_strategy;
static vop_print_t	bfcfs_print;
static vop_pathconf_t	bfcfs_pathconf;

/* Vnode operations vector */
struct vop_vector bfcfs_vnodeops = {
	.vop_default	= &default_vnodeops,
	.vop_lookup	= bfcfs_lookup,
	.vop_getattr	= bfcfs_getattr,
	.vop_open	= bfcfs_open,
	.vop_close	= bfcfs_close,
	.vop_access	= bfcfs_access,
	.vop_read	= bfcfs_read,
	.vop_readdir	= bfcfs_readdir,
	.vop_reclaim	= bfcfs_reclaim,
	.vop_strategy	= bfcfs_strategy,
	.vop_print	= bfcfs_print,
	.vop_pathconf	= bfcfs_pathconf,
};
VFS_VOP_VECTOR_REGISTER(bfcfs_vnodeops);

/*
 * Create or lookup a vnode for a BFC index entry
 * This is the central vnode allocation point
 * If entry is NULL, creates a synthetic root directory (inode 1)
 */
int
bfcfs_vnode_create(struct bfcfs_mount *bmp, struct bfcfs_index_entry *entry,
    struct vnode **vpp)
{
	struct vnode *vp;
	struct bfcfs_node *node;
	int error;

	/* Allocate a fresh vnode (not yet attached to mount) */
	error = getnewvnode("bfcfs", bmp->mp, &bfcfs_vnodeops, &vp);
	if (error) {
		*vpp = NULLVP;
		return (error);
	}

	/* Initialize filesystem-private node and vnode fields */
	node = malloc(sizeof(struct bfcfs_node), M_BFCFS, M_WAITOK | M_ZERO);
	node->bmp = bmp;
	node->entry = entry;  /* NULL for synthetic root */
	node->vnode = vp;

	vp->v_data = node;

	/* Set vnode type based on mode (or VDIR for synthetic root) */
	if (entry == NULL) {
		vp->v_type = VDIR;  /* Synthetic root is always a directory */
		vp->v_vflag |= VV_ROOT;  /* Mark as root vnode */
	} else if (S_ISDIR(entry->mode)) {
		vp->v_type = VDIR;
	} else if (S_ISREG(entry->mode)) {
		vp->v_type = VREG;
	} else if (S_ISLNK(entry->mode)) {
		vp->v_type = VLNK;
	} else {
		vp->v_type = VNON;
	}

	/* Lock the vnode BEFORE insmntque - this is critical */
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);

	/* Attach vnode to the mount — THIS sets vp->v_mount */
	error = insmntque(vp, bmp->mp);
	if (error) {
		/* Vnode wasn't attached; clean up and return */
		free(node, M_BFCFS);
		*vpp = NULLVP;
		return (error);
	}

	/* For regular files, set up the vnode object for VM */
	if (vp->v_type == VREG && entry != NULL) {
		vnode_create_vobject(vp, entry->size, curthread);
	}

	*vpp = vp;
	return (0);
}

/*
 * VOP_LOOKUP - Lookup a file/directory in a directory
 */
static int
bfcfs_lookup(struct vop_lookup_args *ap)
{
	struct vnode *dvp = ap->a_dvp;
	struct vnode **vpp = ap->a_vpp;
	struct componentname *cnp = ap->a_cnp;
	struct bfcfs_node *dnode = VTOBFCFS(dvp);
	struct bfcfs_mount *bmp = dnode->bmp;
	struct bfcfs_index_entry *entry;
	char path[MAXPATHLEN];
	int error, nameiop = cnp->cn_nameiop;

	*vpp = NULL;

	/* Only lookup in directories */
	if (dvp->v_type != VDIR)
		return (ENOTDIR);

	/* Handle "." */
	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		VREF(dvp);
		*vpp = dvp;
		return (0);
	}

	/* Build full path: parent_path + "/" + component */
	if (dnode->entry == NULL) {
		/* Synthetic root - paths are relative to root */
		snprintf(path, sizeof(path), "%.*s",
		    (int)cnp->cn_namelen, cnp->cn_nameptr);
	} else if (strcmp(dnode->entry->path, "/") == 0) {
		snprintf(path, sizeof(path), "/%.*s",
		    (int)cnp->cn_namelen, cnp->cn_nameptr);
	} else {
		snprintf(path, sizeof(path), "%s/%.*s",
		    dnode->entry->path, (int)cnp->cn_namelen, cnp->cn_nameptr);
	}

	BFCFS_DEBUG("lookup: searching for '%s'", path);

	/* Lookup in BFC index */
	entry = bfc_lookup_path(bmp, path);
	if (entry == NULL) {
		/* Not found - handle based on operation */
		if (nameiop == CREATE || nameiop == RENAME)
			return (EROFS);  /* Read-only FS */
		return (ENOENT);
	}

	/* Create vnode for this entry */
	error = bfcfs_vnode_create(bmp, entry, vpp);
	if (error)
		return (error);

	return (0);
}

/*
 * VOP_GETATTR - Get vnode attributes (like stat(2))
 */
static int
bfcfs_getattr(struct vop_getattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	struct bfcfs_node *node = VTOBFCFS(vp);
	struct bfcfs_index_entry *entry = node->entry;
	struct timespec ts;

	vattr_null(vap);
	vap->va_type = vp->v_type;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	vap->va_nlink = 1;
	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_gen = 0;
	vap->va_flags = 0;
	vap->va_rdev = NODEV;
	vap->va_filerev = 0;
	vap->va_blocksize = node->bmp->header.block_size;

	/* Handle synthetic root (NULL entry) */
	if (entry == NULL) {
		vap->va_mode = S_IFDIR | BFC_ROOT_MODE;
		vap->va_fileid = 1;
		vap->va_size = 0;
		vap->va_bytes = 0;
		/* Use current time for synthetic root */
		vfs_timestamp(&ts);
		vap->va_atime = ts;
		vap->va_mtime = ts;
		vap->va_ctime = ts;
		vap->va_birthtime = ts;
	} else {
		vap->va_mode = entry->mode & ALLPERMS;
		vap->va_fileid = entry->ino;
		vap->va_size = entry->size;
		vap->va_bytes = (entry->size + (BFC_BLOCK_ROUND - 1)) & ~(BFC_BLOCK_ROUND - 1);
		vap->va_atime.tv_sec = entry->mtime_ns / NSEC_PER_SEC;
		vap->va_atime.tv_nsec = entry->mtime_ns % NSEC_PER_SEC;
		vap->va_mtime = vap->va_atime;
		vap->va_ctime = vap->va_atime;
		vap->va_birthtime = vap->va_atime;
	}

	return (0);
}

/*
 * VOP_OPEN - Open a file
 */
static int
bfcfs_open(struct vop_open_args *ap)
{
	/* Only allow read access */
	if (ap->a_mode & (FWRITE | O_APPEND | O_TRUNC))
		return (EROFS);

	BFCFS_DEBUG("open: vnode type=%d", ap->a_vp->v_type);
	return (0);
}

/*
 * VOP_CLOSE - Close a file
 */
static int
bfcfs_close(struct vop_close_args *ap)
{
	return (0);
}

/*
 * VOP_ACCESS - Check access permissions
 */
static int
bfcfs_access(struct vop_access_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct bfcfs_node *node = VTOBFCFS(vp);
	accmode_t accmode = ap->a_accmode;
	mode_t mode;

	/* Deny write access */
	if (accmode & VWRITE)
		return (EROFS);

	/* Get mode: synthetic root or from entry */
	mode = (node->entry == NULL) ? BFC_ROOT_MODE : (node->entry->mode & ALLPERMS);

	/* Simple permission check based on mode bits */
	return (vaccess(vp->v_type, mode, 0, 0, accmode, ap->a_cred));
}

/*
 * VOP_READ - Read file data
 */
static int
bfcfs_read(struct vop_read_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct bfcfs_node *node = VTOBFCFS(vp);
	struct bfcfs_index_entry *entry = node->entry;
	int error = 0;
	size_t len, resid;
	off_t offset;
	void *buf;

	if (vp->v_type != VREG)
		return (EISDIR);

	offset = uio->uio_offset;
	resid = uio->uio_resid;

	/* Check bounds */
	if (offset < 0)
		return (EINVAL);
	if (offset >= (off_t)entry->size || resid == 0)
		return (0);

	/* Clamp to file size */
	if (offset + resid > entry->size)
		resid = entry->size - offset;

	BFCFS_DEBUG("read: offset=%ld len=%zu", offset, resid);

	/* Allocate temporary buffer */
	buf = malloc(resid, M_BFCFS, M_WAITOK);

	/* Read from BFC object (handles decompression/decryption) */
	error = bfc_read_object(node->bmp, entry, buf, resid, offset, curthread);
	if (error) {
		free(buf, M_BFCFS);
		return (error);
	}

	/* Copy to userspace */
	len = resid;
	error = uiomove(buf, len, uio);
	free(buf, M_BFCFS);

	return (error);
}

/*
 * VOP_READDIR - Read directory entries
 */
static int
bfcfs_readdir(struct vop_readdir_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct bfcfs_node *node = VTOBFCFS(vp);
	struct bfcfs_mount *bmp = node->bmp;
	struct bfcfs_index_entry **entries = NULL;
	struct dirent dent;
	int error = 0, count = 0, i;
	const char *dir_path;

	if (vp->v_type != VDIR)
		return (ENOTDIR);

	/* Get directory path: "/" for synthetic root */
	dir_path = (node->entry == NULL) ? "/" : node->entry->path;

	/* Get directory entries from BFC */
	error = bfc_readdir(bmp, dir_path, &entries, &count);
	if (error)
		return (error);

	BFCFS_DEBUG("readdir VFS: got %d entries, uio_offset=%ld", count, uio->uio_offset);

	/*
	 * uio_offset tracks which entry index to start from.
	 * Each entry is one unit of offset (not bytes).
	 */
	for (i = uio->uio_offset; i < count; i++) {

		/* Fill in dirent structure */
		bzero(&dent, sizeof(dent));
		dent.d_fileno = entries[i]->ino;
		if (S_ISDIR(entries[i]->mode))
			dent.d_type = DT_DIR;
		else if (S_ISREG(entries[i]->mode))
			dent.d_type = DT_REG;
		else if (S_ISLNK(entries[i]->mode))
			dent.d_type = DT_LNK;
		else
			dent.d_type = DT_UNKNOWN;

		/* Extract just the filename (last component) */
		const char *name = strrchr(entries[i]->path, '/');
		name = (name != NULL) ? name + 1 : entries[i]->path;
		strlcpy(dent.d_name, name, sizeof(dent.d_name));
		dent.d_namlen = strlen(dent.d_name);
		dent.d_reclen = GENERIC_DIRSIZ(&dent);

		BFCFS_DEBUG("readdir VFS: entry[%d] name='%s', ino=%lu, type=%d, reclen=%d",
		    i, dent.d_name, dent.d_fileno, dent.d_type, dent.d_reclen);

		/* Copy to userspace */
		error = uiomove(&dent, dent.d_reclen, uio);
		if (error) {
			BFCFS_DEBUG("readdir VFS: uiomove failed: %d", error);
			break;
		}
	}

	if (entries)
		free(entries, M_BFCFS);

	if (ap->a_eofflag && error == 0)
		*ap->a_eofflag = (i >= count);

	return (error);
}

/*
 * VOP_RECLAIM - Reclaim vnode (called when vnode is being freed)
 */
static int
bfcfs_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct bfcfs_node *node = VTOBFCFS(vp);

	/*
	 * We don't use vfs_hash_insert(), so don't call vfs_hash_remove()
	 * Just clean up our private data
	 */
	if (node) {
		free(node, M_BFCFS);
		vp->v_data = NULL;
	}

	return (0);
}

/*
 * VOP_STRATEGY - Handle buffer I/O (for VM paging)
 */
static int
bfcfs_strategy(struct vop_strategy_args *ap)
{
	struct buf *bp = ap->a_bp;
	struct vnode *vp = ap->a_vp;
	struct bfcfs_node *node = VTOBFCFS(vp);
	struct bfcfs_index_entry *entry = node->entry;
	int error;

	/* Read-only filesystem - should not get write requests */
	if (bp->b_iocmd == BIO_WRITE) {
		bp->b_error = EROFS;
		bp->b_ioflags |= BIO_ERROR;
		bufdone(bp);
		return (EROFS);
	}

	/* Read data from BFC object */
	error = bfc_read_object(node->bmp, entry, bp->b_data,
	    bp->b_bcount, bp->b_iooffset, curthread);

	if (error) {
		bp->b_error = error;
		bp->b_ioflags |= BIO_ERROR;
	} else {
		bp->b_resid = 0;
	}

	bufdone(bp);
	return (error);
}

/*
 * VOP_PRINT - Print vnode info (for debugging)
 */
static int
bfcfs_print(struct vop_print_args *ap)
{
#ifdef DEBUG
	struct vnode *vp = ap->a_vp;
	struct bfcfs_node *node = VTOBFCFS(vp);
	BFCFS_DEBUG("vnode type=%d path=%s",
	    vp->v_type, node->entry ? node->entry->path : "<synthetic root>");
#else
	(void)ap;
#endif

	return (0);
}

/*
 * VOP_PATHCONF - Get filesystem configuration
 */
static int
bfcfs_pathconf(struct vop_pathconf_args *ap)
{
	int error = 0;

	switch (ap->a_name) {
	case 1:	/* _PC_LINK_MAX */
		*ap->a_retval = 1;
		break;
	case 4:	/* _PC_NAME_MAX */
		*ap->a_retval = BFC_NAME_MAX;
		break;
	case 5:	/* _PC_PATH_MAX */
		*ap->a_retval = BFC_PATH_MAX;
		break;
	case 7:	/* _PC_NO_TRUNC */
		*ap->a_retval = 1;
		break;
	case 18:	/* _PC_FILESIZEBITS */
		*ap->a_retval = BFC_FILESIZEBITS;
		break;
	default:
		error = EINVAL;
		break;
	}

	return (error);
}
