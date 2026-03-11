/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_DEDUP_FORMAT_H
#define _BCACHEFS_DEDUP_FORMAT_H

/*
 * On-disk dedup index entry: maps a checksum to the source extent.
 *
 * bpos encoding:
 *   bpos.inode    = (csum_type << 56) | csum.lo
 *   bpos.offset   = csum.hi
 *   bpos.snapshot = 0 (not snapshot-aware)
 *
 * Different checksum types occupy different key ranges and never collide.
 *
 * The value stores the position of the source extent so the "found"
 * path can look it up, byte-verify, and convert both to reflinks.
 * Once converted, reflink_idx is set and BCH_DEDUP_HAS_REFLINK is
 * flagged; subsequent matches go straight to the reflink_v.
 */

enum bch_dedup_flags {
	BCH_DEDUP_HAS_REFLINK	= 1 << 0,	/* reflink_idx is valid */
};

struct bch_dedup {
	struct bch_val		v;
	__le64			src_inode;	/* bpos.inode of source extent */
	__le64			src_offset;	/* bpos.offset of source extent */
	__le32			src_snapshot;	/* bpos.snapshot */
	__le32			size_sectors;	/* extent size for cheap mismatch */
	__le64			reflink_idx;	/* valid when BCH_DEDUP_HAS_REFLINK */
	__le32			flags;
	__le32			pad;
};

#endif /* _BCACHEFS_DEDUP_FORMAT_H */
