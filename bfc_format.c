/*-
 *
 * Copyright (c) 2025 BFCFS Project
 *
 * BFC format parsing and index management
 * This file handles reading the BFC header, footer, and index structure
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/endian.h>
#include <sys/libkern.h>

#include "bfcfs.h"

/* Helper to convert little-endian to host byte order */
#define LE16(x) le16toh(x)
#define LE32(x) le32toh(x)
#define LE64(x) le64toh(x)

/*
 * Read and validate BFC header
 */
int
bfc_read_header(struct bfcfs_mount *bmp, struct thread *td)
{
	struct bfc_header *hdr = &bmp->header;
	int error;

	BFCFS_DEBUG("format: reading header");

	/* Read header from offset 0 */
	error = bfc_pread(bmp, hdr, sizeof(*hdr), 0, td);
	if (error) {
		BFCFS_ERR("failed to read header: %d", error);
		return (error);
	}

	/* Validate magic string */
	if (strncmp(hdr->magic, "BFCFv1", 6) != 0) {
		BFCFS_ERR("invalid magic: expected 'BFCFv1', got '%.8s'",
		    hdr->magic);
		return (EINVAL);
	}

	/* Convert fields to host byte order */
	hdr->header_crc32 = LE32(hdr->header_crc32);
	hdr->block_size = LE32(hdr->block_size);
	hdr->features = LE64(hdr->features);

	BFCFS_DEBUG("format: header valid, block_size=%u, features=0x%lx",
	    hdr->block_size, hdr->features);

	/* Sanity check block size (must be non-zero and reasonable) */
	if (hdr->block_size == 0 || hdr->block_size > BFC_MAX_BLOCK_SIZE) {
		BFCFS_ERR("invalid block size: %u (max %u)",
		    hdr->block_size, BFC_MAX_BLOCK_SIZE);
		return (EINVAL);
	}

	return (0);
}

/*
 * Read and parse BFC footer (last 56 bytes of file)
 */
static int
bfc_read_footer(struct bfcfs_mount *bmp, struct thread *td)
{
	struct bfc_footer *footer = &bmp->footer;
	off_t file_size, footer_offset;
	int error;

	/* Get backing file size */
	error = bfc_get_file_size(bmp, &file_size, td);
	if (error)
		return (error);

	if (file_size < (off_t)(BFC_HEADER_SIZE + BFC_FOOTER_SIZE)) {
		BFCFS_ERR("file too small: %ld bytes", file_size);
		return (EINVAL);
	}

	/* Read footer from end of file */
	footer_offset = file_size - BFC_FOOTER_SIZE;
	error = bfc_pread(bmp, footer, sizeof(*footer), footer_offset, td);
	if (error) {
		BFCFS_ERR("failed to read footer: %d", error);
		return (error);
	}

	/* Validate footer magic */
	if (strncmp(footer->magic_start, "BFCFIDX", 7) != 0) {
		BFCFS_ERR("invalid footer magic");
		return (EINVAL);
	}

	/* Convert to host byte order */
	footer->index_size = LE64(footer->index_size);
	footer->index_crc32 = LE32(footer->index_crc32);
	footer->index_offset = LE64(footer->index_offset);
	footer->container_crc = LE32(footer->container_crc);

	BFCFS_DEBUG("format: footer: index_offset=%lu, index_size=%lu",
	    footer->index_offset, footer->index_size);

	/* Sanity checks */
	if (footer->index_offset < BFC_HEADER_SIZE ||
	    footer->index_offset >= (uint64_t)file_size) {
		BFCFS_ERR("invalid index offset: %lu", footer->index_offset);
		return (EINVAL);
	}

	if (footer->index_size == 0 || footer->index_size > BFC_MAX_INDEX_SIZE) {
		BFCFS_ERR("invalid index size: %lu (max %u)",
		    footer->index_size, BFC_MAX_INDEX_SIZE);
		return (EINVAL);
	}

	return (0);
}

/*
 * Parse a single index entry from raw buffer
 * BFC index format:
 *   - path_len: uint32_t (4 bytes, little-endian)
 *   - path: char[path_len] (variable length, UTF-8)
 *   - obj_offset: uint64_t (8 bytes)
 *   - obj_size: uint64_t (8 bytes)
 *   - mode: uint32_t (4 bytes)
 *   - mtime_ns: uint64_t (8 bytes)
 *   - comp: uint32_t (4 bytes)
 *   - enc: uint32_t (4 bytes)
 *   - orig_size: uint64_t (8 bytes)
 *   - crc32c: uint32_t (4 bytes)
 * Total: 4 + path_len + 48 bytes
 */
static int
bfc_parse_index_entry(const uint8_t *buf, size_t buflen, size_t *offset,
    struct bfcfs_index_entry *entry, ino_t ino)
{
	const uint8_t *p = buf + *offset;
	size_t remaining = buflen - *offset;
	uint32_t path_len;

	/* Need at least 4 bytes for path_len */
	if (remaining < 4)
		return (EINVAL);

	/* Read path length (32-bit) */
	bcopy(p, &path_len, 4);
	path_len = LE32(path_len);
	p += 4;
	remaining -= 4;

	/* Sanity check path length */
	if (path_len == 0 || path_len > BFC_MAX_PATH_LEN || path_len > remaining)
		return (EINVAL);

	/* Allocate and copy path */
	entry->path = malloc(path_len + 1, M_BFCFS, M_WAITOK);
	bcopy(p, entry->path, path_len);
	entry->path[path_len] = '\0';
	p += path_len;
	remaining -= path_len;

	/* Need at least metadata bytes for entry fields */
	if (remaining < BFC_INDEX_ENTRY_METADATA_SIZE)
		return (EINVAL);

	/* Parse metadata fields (all little-endian) */
	bcopy(p, &entry->obj_offset, 8);
	entry->obj_offset = LE64(entry->obj_offset);
	p += 8;

	bcopy(p, &entry->obj_size, 8);
	entry->obj_size = LE64(entry->obj_size);
	p += 8;

	bcopy(p, &entry->mode, 4);
	entry->mode = LE32(entry->mode);
	p += 4;

	bcopy(p, &entry->mtime_ns, 8);
	entry->mtime_ns = LE64(entry->mtime_ns);
	p += 8;

	bcopy(p, &entry->comp, 4);
	entry->comp = LE32(entry->comp);
	p += 4;

	bcopy(p, &entry->enc, 4);
	entry->enc = LE32(entry->enc);
	p += 4;

	bcopy(p, &entry->size, 8);	/* orig_size */
	entry->size = LE64(entry->size);
	p += 8;

	bcopy(p, &entry->crc32c, 4);
	entry->crc32c = LE32(entry->crc32c);
	p += 4;

	entry->ino = ino;

	/* Update offset */
	*offset = p - buf;
	return (0);
}

/*
 * Read and parse the full BFC index
 */
int
bfc_read_index(struct bfcfs_mount *bmp, struct thread *td)
{
	uint8_t *index_buf = NULL;
	size_t offset = 0;
	int error, i;

	/* Read footer first to get index location */
	error = bfc_read_footer(bmp, td);
	if (error)
		return (error);

	BFCFS_DEBUG("format: reading index at offset %lu, size %lu",
	    bmp->footer.index_offset, bmp->footer.index_size);

	/* Allocate buffer for index */
	index_buf = malloc(bmp->footer.index_size, M_BFCFS, M_WAITOK);

	/* Read index data */
	error = bfc_pread(bmp, index_buf, bmp->footer.index_size,
	    bmp->footer.index_offset, td);
	if (error) {
		BFCFS_ERR("failed to read index: %d", error);
		free(index_buf, M_BFCFS);
		return (error);
	}

	/* Parse index header (8 bytes: version + count) */
	if (bmp->footer.index_size < 8) {
		BFCFS_ERR("index too small for header");
		free(index_buf, M_BFCFS);
		return (EINVAL);
	}

	uint32_t version, count;
	bcopy(index_buf, &version, 4);
	version = LE32(version);
	bcopy(index_buf + 4, &count, 4);
	count = LE32(count);

	if (version != 1) {
		BFCFS_ERR("unsupported index version: %u", version);
		free(index_buf, M_BFCFS);
		return (EINVAL);
	}

	bmp->num_entries = count;
	if (bmp->num_entries == 0 || bmp->num_entries > BFC_MAX_ENTRIES) {
		BFCFS_ERR("invalid entry count: %u (max %u)",
		    bmp->num_entries, BFC_MAX_ENTRIES);
		free(index_buf, M_BFCFS);
		return (EINVAL);
	}

	BFCFS_DEBUG("format: index version=%u, count=%u", version, bmp->num_entries);

	/* Allocate index entry array */
	bmp->index = malloc(bmp->num_entries * sizeof(struct bfcfs_index_entry),
	    M_BFCFS, M_WAITOK | M_ZERO);

	/* Parse all entries (starting after 8-byte header) */
	offset = 8;
	for (i = 0; i < (int)bmp->num_entries; i++) {
		error = bfc_parse_index_entry(index_buf, bmp->footer.index_size,
		    &offset, &bmp->index[i], i + 1);  /* inode numbers start at 1 */
		if (error) {
			BFCFS_ERR("failed to parse index entry %d: %d", i, error);
			goto fail;
		}

		/* Add to list */
		LIST_INSERT_HEAD(&bmp->index_list, &bmp->index[i], entries);

		BFCFS_DEBUG("format: entry[%d]: path=%s, size=%lu, mode=0%o, crc32c=0x%x",
		    i, bmp->index[i].path, bmp->index[i].size, bmp->index[i].mode,
		    bmp->index[i].crc32c);
	}

	free(index_buf, M_BFCFS);
	BFCFS_DEBUG("format: loaded %d index entries", bmp->num_entries);
	return (0);

fail:
	/* Clean up on error */
	for (i = 0; i < (int)bmp->num_entries; i++) {
		if (bmp->index[i].path)
			free(bmp->index[i].path, M_BFCFS);
	}
	free(bmp->index, M_BFCFS);
	free(index_buf, M_BFCFS);
	bmp->index = NULL;
	bmp->num_entries = 0;
	return (error);
}

/*
 * Look up a path in the index (binary search on sorted index)
 * TODO: The index should be sorted; for now we do linear search
 */
struct bfcfs_index_entry *
bfc_lookup_path(struct bfcfs_mount *bmp, const char *path)
{
	int i;

	BFCFS_LOCK(bmp);
	for (i = 0; i < (int)bmp->num_entries; i++) {
		if (strcmp(bmp->index[i].path, path) == 0) {
			BFCFS_UNLOCK(bmp);
			return &bmp->index[i];
		}
	}
	BFCFS_UNLOCK(bmp);

	return (NULL);
}

/*
 * List all entries in a directory
 * Returns an array of pointers to matching entries
 */
int
bfc_readdir(struct bfcfs_mount *bmp, const char *dir_path,
    struct bfcfs_index_entry ***entries_out, int *count_out)
{
	struct bfcfs_index_entry **entries;
	int i, count = 0;
	size_t dir_len;
	const char *dir_prefix;
	int is_root;

	/* Normalize directory path */
	is_root = (strcmp(dir_path, "/") == 0);
	dir_prefix = is_root ? "/" : dir_path;
	dir_len = strlen(dir_prefix);

	/* Count matching entries first */
	BFCFS_LOCK(bmp);

	for (i = 0; i < (int)bmp->num_entries; i++) {
		const char *path = bmp->index[i].path;
		const char *after_prefix;
		const char *slash;

		/* Skip the directory itself */
		if (strcmp(path, dir_path) == 0)
			continue;

		/* Check if this path is under dir_path */
		if (is_root) {
			/*
			 * Root: show all top-level entries
			 * - Entries with no slash (e.g., "index.html", "assets")
			 * - Skip files inside directories (e.g., "assets/file.css")
			 */
			slash = strchr(path, '/');
			BFCFS_DEBUG("readdir: checking path='%s', slash=%p", path, slash);
			if (slash != NULL) {
				/* Has a slash - this is inside a directory, skip it */
				BFCFS_DEBUG("readdir: skipping (has slash)");
				continue;
			}
			/* Top-level entry with no slash - include it */
			BFCFS_DEBUG("readdir: including");
		} else {
			/* Non-root: match direct children only */
			if (strncmp(path, dir_prefix, dir_len) != 0)
				continue;  /* Not under this directory */

			after_prefix = path + dir_len;
			if (*after_prefix != '/')
				continue;  /* Not a child */

			/* Skip the '/' and check if there are more '/' (subdirectory) */
			after_prefix++;
			if (strchr(after_prefix, '/') != NULL)
				continue;  /* This is in a subdirectory */
		}

		count++;
	}

	if (count == 0) {
		BFCFS_UNLOCK(bmp);
		*entries_out = NULL;
		*count_out = 0;
		return (0);
	}

	/* Allocate array for results */
	entries = malloc(count * sizeof(struct bfcfs_index_entry *),
	    M_BFCFS, M_WAITOK);

	/* Fill in matching entries */
	count = 0;
	for (i = 0; i < (int)bmp->num_entries; i++) {
		const char *path = bmp->index[i].path;
		const char *after_prefix;
		const char *slash;

		if (strcmp(path, dir_path) == 0)
			continue;

		if (is_root) {
			/* Same logic as counting loop */
			slash = strchr(path, '/');
			if (slash != NULL)
				continue;  /* Skip subdirectory entries */
		} else {
			if (strncmp(path, dir_prefix, dir_len) != 0)
				continue;
			after_prefix = path + dir_len;
			if (*after_prefix != '/')
				continue;
			after_prefix++;
			if (strchr(after_prefix, '/') != NULL)
				continue;
		}

		entries[count++] = &bmp->index[i];
	}

	BFCFS_UNLOCK(bmp);

	*entries_out = entries;
	*count_out = count;
	return (0);
}

/*
 * Free all index entries
 */
void
bfc_free_index(struct bfcfs_mount *bmp)
{
	int i;

	if (bmp->index == NULL)
		return;

	BFCFS_LOCK(bmp);
	for (i = 0; i < (int)bmp->num_entries; i++) {
		if (bmp->index[i].path)
			free(bmp->index[i].path, M_BFCFS);
	}
	free(bmp->index, M_BFCFS);
	bmp->index = NULL;
	bmp->num_entries = 0;
	BFCFS_UNLOCK(bmp);
}
