/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_DEDUP_H
#define _BCACHEFS_DEDUP_H

struct bch_fs;
struct btree_iter;
struct moving_context;
struct bkey_s_c;
struct bch_inode_opts;

int bch2_dedup_extent(struct moving_context *,
		      struct bch_inode_opts *,
		      struct btree_iter *,
		      struct bkey_s_c);

void bch2_dedup_val_to_text(struct printbuf *, struct bch_fs *, struct bkey_s_c);

#define bch2_bkey_ops_dedup ((struct bkey_ops) {	\
	.val_to_text	= bch2_dedup_val_to_text,	\
})

#endif /* _BCACHEFS_DEDUP_H */
