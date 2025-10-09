/*-
 *
 * Copyright (c) 2025 BFCFS Project
 *
 * Low-level I/O routines for reading from .bfc backing file
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/gsb_crc32.h>

#include "bfcfs.h"

/*
 * Read from backing .bfc file at specified offset
 * This is the low-level primitive used by all other read operations
 */
int
bfc_pread(struct bfcfs_mount *bmp, void *buf, size_t len, off_t offset,
    struct thread *td)
{
	struct uio auio;
	struct iovec aiov;
	int error;

	if (bmp->devvp == NULL)
		return (EIO);

	/* Set up iovec and uio for kernel buffer */
	aiov.iov_base = buf;
	aiov.iov_len = len;

	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_offset = offset;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_resid = len;
	auio.uio_td = td;

	/* Lock vnode and perform read */
	vn_lock(bmp->devvp, LK_SHARED | LK_RETRY);
	error = VOP_READ(bmp->devvp, &auio, IO_DIRECT, td->td_ucred);
	VOP_UNLOCK(bmp->devvp);

	if (error) {
		BFCFS_DEBUG("io: read failed at offset %ld: error %d", offset, error);
		return (error);
	}

	/* Check if we read the full amount */
	if (auio.uio_resid != 0) {
		BFCFS_DEBUG("io: short read at offset %ld: got %zu of %zu bytes",
		    offset, len - auio.uio_resid, len);
		return (EIO);
	}

	/* Update statistics */
	BFCFS_LOCK(bmp);
	bmp->reads++;
	bmp->bytes_read += len;
	BFCFS_UNLOCK(bmp);

	return (0);
}

/*
 * CRC32C calculation using FreeBSD's hardware-accelerated implementation
 */
static uint32_t
bfc_crc32c(const void *buf, size_t len)
{
	/*
	 * calculate_crc32c() from FreeBSD already handles initialization and XOR
	 * Just pass BFC_CRC32C_INIT as initial CRC for a new calculation
	 */
	return calculate_crc32c(BFC_CRC32C_INIT, buf, len);
}

/*
 * Read and decompress/decrypt a BFC object
 * This handles the full BFC object reading pipeline:
 * 1. Read compressed/encrypted data from container
 * 2. Decrypt if needed
 * 3. Decompress if needed
 * 4. Verify CRC if requested
 */
int
bfc_read_object(struct bfcfs_mount *bmp, struct bfcfs_index_entry *entry,
    void *buf, size_t len, off_t offset, struct thread *td)
{
	struct bfc_obj_header obj_hdr;
	off_t content_offset;
	size_t hdr_name_size, padding;
	int error = 0;

	/* Sanity checks */
	if (offset < 0 || len == 0)
		return (EINVAL);
	if (offset >= (off_t)entry->size)
		return (0);  /* EOF */

	/* Clamp read to file size */
	if (offset + len > entry->size)
		len = entry->size - offset;

	BFCFS_DEBUG("read_object: path=%s offset=%ld len=%zu comp=%u enc=%u",
	    entry->path, offset, len, entry->comp, entry->enc);

	/*
	 * Read object header to calculate content offset
	 * The content starts after: header + name + padding
	 */
	error = bfc_pread(bmp, &obj_hdr, sizeof(obj_hdr), entry->obj_offset, td);
	if (error)
		return (error);

	/* Calculate content offset like Linux and BFC library do */
	hdr_name_size = sizeof(struct bfc_obj_header) + le16toh(obj_hdr.name_len);
	padding = BFC_ALIGN_UP(hdr_name_size, BFC_ALIGN) - hdr_name_size;
	content_offset = entry->obj_offset + hdr_name_size + padding;

	BFCFS_DEBUG("read_object: %s obj_off=%lu hdr=%zu name_len=%u hdr_name=%zu pad=%zu content_off=%ld",
	    entry->path, entry->obj_offset, sizeof(struct bfc_obj_header),
	    le16toh(obj_hdr.name_len), hdr_name_size, padding, content_offset);

	/*
	 * Simple case: uncompressed, unencrypted file
	 * Read directly from container at content_offset + offset
	 */
	if (entry->comp == BFC_COMP_NONE && entry->enc == BFC_ENC_NONE) {
		error = bfc_pread(bmp, buf, len, content_offset + offset, td);
		if (error)
			return (error);

		/* Verify CRC if requested (on full file reads only) */
		if (bmp->verify_crc && offset == 0 && len == entry->size) {
			uint32_t crc = bfc_crc32c(buf, len);
			if (crc != entry->crc32c) {
				BFCFS_ERR("CRC mismatch for %s: "
				    "expected 0x%x, got 0x%x",
				    entry->path, entry->crc32c, crc);
				return (EIO);
			}
		}

		return (0);
	}

	/*
	 * Complex case: compressed and/or encrypted
	 * For now, we only support reading the full object
	 * TODO: Add support for partial reads with decompression
	 */
	if (offset != 0 || len != entry->size) {
		BFCFS_ERR("partial reads not yet supported for "
		    "compressed/encrypted files");
		return (EOPNOTSUPP);
	}

	/* Handle compressed files */
	if (entry->comp == BFC_COMP_ZSTD) {
		BFCFS_ERR("ZSTD decompression not yet implemented");
		return (EOPNOTSUPP);
		/*
		 * TODO: Implement ZSTD decompression
		 * 1. Allocate compressed_buf of size entry->obj_size
		 * 2. Read compressed data: bfc_pread(bmp, compressed_buf, ...)
		 * 3. Decompress into buf using ZSTD_decompress()
		 * 4. Verify CRC if requested
		 */
	}

	/* Handle encrypted files */
	if (entry->enc == BFC_ENC_CHACHA20_POLY1305) {
		BFCFS_ERR("ChaCha20-Poly1305 decryption not yet implemented");
		return (EOPNOTSUPP);
		/*
		 * TODO: Implement decryption
		 * 1. Check if encryption key is available
		 * 2. Read encrypted data
		 * 3. Decrypt using libsodium or native crypto
		 * 4. Verify authentication tag
		 */
	}

	/* Unknown compression/encryption */
	BFCFS_ERR("unsupported compression (%u) or encryption (%u)",
	    entry->comp, entry->enc);
	return (EOPNOTSUPP);
}


/*
 * Utility: get backing file size
 */
int
bfc_get_file_size(struct bfcfs_mount *bmp, off_t *size, struct thread *td)
{
	struct vattr vattr;
	int error;

	vn_lock(bmp->devvp, LK_SHARED | LK_RETRY);
	error = VOP_GETATTR(bmp->devvp, &vattr, td->td_ucred);
	VOP_UNLOCK(bmp->devvp);

	if (error)
		return (error);

	*size = vattr.va_size;
	return (0);
}
