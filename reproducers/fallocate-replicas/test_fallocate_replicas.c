// SPDX-License-Identifier: GPL-2.0
/*
 * Pure-userspace test for Kent's "disk reservation at swapon — that should
 * just be fallocate" (koverstreet/bcachefs-tools#787).
 *
 * Drives real libbcachefs: formats nothing itself (see run-test.sh), opens an
 * existing image, creates a file, fallocates a range through the same
 * bch2_extent_fallocate() the VFS fallocate path uses, and then evaluates —
 * against the keys that fallocate actually put in the extents btree — the
 * predicate that bch2_check_range_allocated() applies on the O_DIRECT write
 * path.
 *
 * bch2_check_range_allocated() itself is in fs/vfs/direct.c, which the
 * userspace build excludes wholesale via -DNO_BCACHEFS_FS, so it cannot be
 * called here. Its per-key test (fs/vfs/direct.c:265-268) is transcribed
 * verbatim below and marked; the inputs to it are genuine keys from a genuine
 * fallocate, so the only thing being stubbed is the loop around the test.
 *
 * Expected: FAIL before the bch2_bkey_replicas() fix, PASS after.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bcachefs.h"
#include "alloc/foreground.h"
#include "btree/iter.h"
#include "btree/update.h"
#include "data/extents.h"
#include "data/io_misc.h"
#include "fs/inode.h"
#include "fs/namei.h"
#include "init/fs.h"
#include "opts.h"
#include "snapshots/subvolume.h"
#include "util/darray.h"

#include <linux/shrinker.h>

#define TEST_SECTORS	2048		/* 1 MiB */

/*
 * libbcachefs.a is normally linked by cargo alongside the Rust half of the
 * tools. This test links the C library on its own, so the handful of Rust
 * symbols the C code refers to have to be stubbed. None of them is on any path
 * this test exercises.
 */
void bch2_start_http_lazy(void);
void bch2_start_http_lazy(void) {}

static unsigned nr_fail;

#define check(cond, fmt, ...)						\
do {									\
	if (!(cond)) {							\
		printf("FAIL: " fmt "\n", ##__VA_ARGS__);		\
		nr_fail++;						\
	} else {							\
		printf("ok:   " fmt "\n", ##__VA_ARGS__);		\
	}								\
} while (0)

/*
 * Verbatim from bch2_check_range_allocated(), fs/vfs/direct.c:265-268 — the
 * per-key half of the test it applies to every key in the range. Returns true
 * if the key means "this range is already paid for; the write may proceed
 * without a fresh disk reservation".
 */
static bool check_range_allocated_key(struct bch_fs *c, struct bkey_s_c k,
				      u32 snapshot, unsigned nr_replicas,
				      bool compressed)
{
	return !(k.k->p.snapshot != snapshot ||
		 nr_replicas > bch2_bkey_replicas(c, k) ||
		 (!compressed && bch2_bkey_sectors_compressed(c, k)));
}

static int make_file(struct bch_fs *c, struct bch_inode_unpacked *new_inode)
{
	struct bch_inode_unpacked root_inode;
	struct bch_subvolume new_subvol;
	struct qstr name = QSTR("fallocated");

	int ret = bch2_trans_do(c,
		bch2_inode_find_by_inum_trans(trans, BCACHEFS_ROOT_SUBVOL_INUM,
					      &root_inode));
	if (ret) {
		fprintf(stderr, "reading root inode: %s\n", bch2_err_str(ret));
		return ret;
	}

	bch2_inode_init_early(c, new_inode);

	ret = bch2_trans_commit_do(c, NULL, NULL, 0,
		bch2_create_trans(trans, BCACHEFS_ROOT_SUBVOL_INUM,
				  &root_inode, new_inode, &new_subvol, &name,
				  0, 0, S_IFREG|0644, 0,
				  NULL, NULL, (subvol_inum) { 0 }, 0));
	if (ret)
		fprintf(stderr, "creating file: %s\n", bch2_err_str(ret));
	return ret;
}

static int do_fallocate(struct bch_fs *c, subvol_inum inum,
			struct bch_inode_opts *opts, u64 sectors)
{
	CLASS(btree_trans, trans)(c);
	u32 snapshot;
	int ret = 0;

	ret = lockrestart_do(trans,
		bch2_subvolume_get_snapshot(trans, inum.subvol, &snapshot));
	if (ret)
		return ret;

	CLASS(btree_iter, iter)(trans, BTREE_ID_extents,
				SPOS(inum.inum, 0, snapshot),
				BTREE_ITER_slots|BTREE_ITER_intent);

	while (iter.pos.offset < sectors) {
		s64 i_sectors_delta = 0;

		bch2_trans_begin(trans);

		ret = bch2_extent_fallocate(trans, inum, &iter,
					    sectors - iter.pos.offset,
					    *opts, &i_sectors_delta,
					    writepoint_hashed(0));
		if (bch2_err_matches(ret, BCH_ERR_transaction_restart)) {
			ret = 0;
			continue;
		}
		if (ret) {
			fprintf(stderr, "fallocate at %llu: %s\n",
				iter.pos.offset, bch2_err_str(ret));
			return ret;
		}
	}

	return 0;
}

/* Walk what fallocate actually wrote, and apply the direct.c predicate. */
static int verify(struct bch_fs *c, subvol_inum inum,
		  struct bch_inode_opts *io_opts)
{
	CLASS(btree_trans, trans)(c);
	u32 snapshot;
	unsigned nr_keys = 0, nr_reservation = 0, nr_seen_allocated = 0;
	u64 covered = 0;

	int ret = lockrestart_do(trans,
		bch2_subvolume_get_snapshot(trans, inum.subvol, &snapshot));
	if (ret)
		return ret;

	CLASS(btree_iter, iter)(trans, BTREE_ID_extents,
				SPOS(inum.inum, 0, snapshot),
				BTREE_ITER_slots);

	while (iter.pos.offset < TEST_SECTORS) {
		struct bkey_s_c k = bch2_btree_iter_peek_slot(&iter);

		ret = bkey_err(k);
		if (bch2_err_matches(ret, BCH_ERR_transaction_restart)) {
			bch2_trans_begin(trans);
			continue;
		}
		if (ret)
			return ret;
		if (bkey_ge(bkey_start_pos(k.k), POS(inum.inum, TEST_SECTORS)))
			break;

		nr_keys++;
		if (k.k->type == KEY_TYPE_reservation) {
			nr_reservation++;
			covered += k.k->p.offset - bkey_start_offset(k.k);
		}

		if (check_range_allocated_key(c, k, snapshot,
					      io_opts->data_replicas, false)) {
			nr_seen_allocated++;
		} else {
			printf("      %s key at %llu: bch2_bkey_replicas() = %u, want >= %u\n",
			       bch2_bkey_types[k.k->type], bkey_start_offset(k.k),
			       bch2_bkey_replicas(c, k), io_opts->data_replicas);
		}

		bch2_btree_iter_advance(&iter);
	}

	printf("\n%u keys over the fallocated range, %u of them reservations, "
	       "%llu sectors covered\n", nr_keys, nr_reservation, covered);

	check(nr_keys > 0, "fallocate left keys in the extents btree");
	check(nr_reservation == nr_keys,
	      "every key over the range is KEY_TYPE_reservation (%u/%u)",
	      nr_reservation, nr_keys);
	check(covered == TEST_SECTORS,
	      "reservations cover the whole range (%llu/%u sectors)",
	      covered, TEST_SECTORS);
	check(nr_seen_allocated == nr_keys,
	      "bch2_check_range_allocated() predicate holds for every key (%u/%u)",
	      nr_seen_allocated, nr_keys);

	return 0;
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <image>\n", argv[0]);
		return 2;
	}

	/*
	 * What src/bcachefs.rs:330 does before touching libbcachefs. Skipping it
	 * is not merely untidy: _totalram_pages stays 0, so the journal's
	 * mem_limit (fs/journal/reclaim.c:166) is 0, journal_space_total.total is
	 * 0, the watermark sits permanently at "reclaim", and the first
	 * normal-priority transaction wedges on journal_full. (Recovery and fsck
	 * survive it because they commit at a higher watermark.)
	 */
	linux_shrinkers_init();

	darray_const_str devs = {};
	darray_push(&devs, argv[1]);

	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, read_only, false);
	opt_set(opts, errors, BCH_ON_ERROR_continue);

	struct bch_fs *c = bch2_fs_open(&devs, &opts);
	if (IS_ERR(c)) {
		fprintf(stderr, "opening %s: %s\n", argv[1],
			bch2_err_str(PTR_ERR(c)));
		return 1;
	}

	struct bch_inode_unpacked inode;
	if (make_file(c, &inode))
		goto err;

	subvol_inum inum = {
		.subvol	= BCACHEFS_ROOT_SUBVOL,
		.inum	= inode.bi_inum,
	};

	struct bch_inode_opts io_opts;
	bch2_inode_opts_get_inode(c, &inode, &io_opts);

	printf("inum %llu, data_replicas %u\n", inum.inum, io_opts.data_replicas);

	if (do_fallocate(c, inum, &io_opts, TEST_SECTORS))
		goto err;

	if (verify(c, inum, &io_opts))
		goto err;

	bch2_fs_stop(c);
	darray_exit(&devs);

	printf("\n%s\n", nr_fail ? "FAILED" : "PASSED");
	return nr_fail ? 1 : 0;
err:
	bch2_fs_stop(c);
	darray_exit(&devs);
	fprintf(stderr, "setup failed\n");
	return 1;
}
