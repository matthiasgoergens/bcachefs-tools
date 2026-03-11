// SPDX-License-Identifier: GPL-2.0

/*
 * Background deduplication for bcachefs.
 *
 * Scans extents via the reconcile subsystem, looks up their existing
 * checksums in BTREE_ID_dedup, and converts duplicates to reflinks.
 *
 * The dedup index (BTREE_ID_dedup) maps checksums to the position of
 * the first extent seen with that checksum.  When a second extent with
 * the same checksum is found, both extents are read, byte-compared,
 * and if they match, converted to reflinks sharing the same data.
 *
 * Byte-verification is mandatory: CRC checksums are not cryptographic,
 * and a malicious user could craft files with matching CRCs to steal
 * or corrupt other users' data through dedup.
 */

#include "bcachefs.h"
#include "btree/iter.h"
#include "btree/bkey_methods.h"
#include "util/printbuf.h"
#include "btree/update.h"
#include "btree/bkey_buf.h"
#include "data/checksum.h"
#include "data/dedup.h"
#include "data/dedup_format.h"
#include "data/extents.h"
#include "data/move.h"
#include "data/read.h"
#include "data/reconcile/trigger.h"
#include "data/reflink.h"
#include "sb/counters_format.h"

#include <linux/bio.h>

/*
 * Extract the first CRC entry from an extent key.
 * Returns true if a usable checksum was found.
 */
static bool extent_get_crc(struct bch_fs *c, struct bkey_s_c k,
			   struct bch_extent_crc_unpacked *crc_ret)
{
	struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(k);
	const union bch_extent_entry *entry;
	struct extent_ptr_decoded p;

	bkey_for_each_ptr_decode(k.k, ptrs, p, entry) {
		if (p.crc.csum_type != BCH_CSUM_none) {
			*crc_ret = p.crc;
			return true;
		}
	}
	return false;
}

/*
 * Encode an extent's checksum as a bpos for BTREE_ID_dedup lookup.
 *
 * bpos.inode  = (csum_type << 56) | csum.lo
 * bpos.offset = csum.hi
 */
static struct bpos dedup_pos_from_crc(struct bch_extent_crc_unpacked crc)
{
	return POS(((u64)crc.csum_type << 56) | crc.csum.lo,
		   crc.csum.hi);
}

/* Completion callback for synchronous dedup reads */
static void dedup_read_endio(struct bio *bio)
{
	complete(bio->bi_private);
}

/*
 * Read an extent's data into a bio synchronously.
 *
 * Allocates pages for the bio, issues the read via __bch2_read_extent,
 * and waits for it to complete.  Caller must free the bio with bio_put.
 *
 * Returns 0 on success, -errno on failure.
 */
static int dedup_read_extent(struct btree_trans *trans,
			     struct bkey_s_c k,
			     struct bio **bio_ret)
{
	struct bch_fs *c = trans->c;
	unsigned sectors = k.k->size;
	unsigned bytes = sectors << 9;

	struct bio *bio = bio_alloc_bioset(NULL, DIV_ROUND_UP(bytes, PAGE_SIZE),
					   REQ_OP_READ, GFP_KERNEL, &c->bio_read);
	if (!bio)
		return -ENOMEM;

	unsigned remaining = bytes;
	while (remaining) {
		struct page *page = alloc_page(GFP_KERNEL);
		if (!page) {
			bio_free_pages(bio);
			bio_put(bio);
			return -ENOMEM;
		}
		unsigned len = min_t(unsigned, remaining, PAGE_SIZE);
		__bio_add_page(bio, page, len, 0);
		remaining -= len;
	}

	DECLARE_COMPLETION_ONSTACK(done);
	bio->bi_private = &done;

	struct bch_read_bio *rbio = rbio_init(bio, c,
					      (struct bch_inode_opts) { 0 },
					      dedup_read_endio);
	rbio->data_pos = bkey_start_pos(k.k);

	__bch2_read_extent(trans, rbio, bio->bi_iter,
			   bkey_start_pos(k.k),
			   BTREE_ID_extents, k, 0, NULL,
			   BCH_READ_last_fragment, -1);

	wait_for_completion(&done);

	if (rbio->ret) {
		bio_free_pages(bio);
		bio_put(bio);
		return rbio->ret;
	}

	*bio_ret = bio;
	return 0;
}

/*
 * Compare data in two bios byte-by-byte.
 * Returns true if content is identical.
 */
static bool bio_data_equal(struct bio *a, struct bio *b)
{
	struct bvec_iter iter_a = a->bi_iter;
	struct bvec_iter iter_b = b->bi_iter;

	while (iter_a.bi_size) {
		unsigned len = min3(iter_a.bi_size, iter_b.bi_size,
				    (unsigned)PAGE_SIZE);

		struct bio_vec bv_a = bio_iter_iovec(a, iter_a);
		struct bio_vec bv_b = bio_iter_iovec(b, iter_b);

		len = min3(len, bv_a.bv_len, bv_b.bv_len);

		void *pa = bvec_kmap_local(&bv_a);
		void *pb = bvec_kmap_local(&bv_b);
		int cmp = memcmp(pa, pb, len);
		kunmap_local(pb);
		kunmap_local(pa);

		if (cmp)
			return false;

		bio_advance_iter_single(a, &iter_a, len);
		bio_advance_iter_single(b, &iter_b, len);
	}

	return true;
}

static void bio_free_and_put(struct bio *bio)
{
	bio_free_pages(bio);
	bio_put(bio);
}

/*
 * Clear dedup_pending on the extent key so it won't be re-processed.
 * Called after successful indexing in the "not found" path.
 */
static int dedup_clear_pending(struct btree_trans *trans,
			       struct btree_iter *extent_iter,
			       struct bkey_s_c k)
{
	struct bch_fs *c = trans->c;
	struct bkey_i *n = errptr_try(
		bch2_trans_kmalloc(trans, bkey_bytes(k.k)));
	bkey_reassemble(n, k);

	struct bch_extent_reconcile *r =
		(struct bch_extent_reconcile *) bch2_bkey_reconcile_opts(c, bkey_i_to_s_c(n));
	if (r)
		r->dedup_pending = 0;

	return bch2_trans_update(trans, extent_iter, n,
				BTREE_UPDATE_internal_snapshot_node);
}

/*
 * Insert a new dedup index entry recording this extent as the first
 * seen with this checksum.
 */
static int dedup_index_insert(struct btree_trans *trans,
			      struct btree_iter *dedup_iter,
			      struct bkey_s_c k,
			      unsigned size_sectors)
{
	struct bkey_i_dedup *new = bch2_trans_kmalloc(trans, sizeof(*new));
	if (IS_ERR(new))
		return PTR_ERR(new);

	bkey_dedup_init(&new->k_i);
	new->k.p = dedup_iter->pos;

	struct bch_dedup *d = &new->v;
	memset(d, 0, sizeof(*d));
	d->src_inode	= cpu_to_le64(k.k->p.inode);
	d->src_offset	= cpu_to_le64(k.k->p.offset);
	d->src_snapshot	= cpu_to_le32(k.k->p.snapshot);
	d->size_sectors	= cpu_to_le32(size_sectors);

	set_bkey_val_bytes(&new->k, sizeof(*d));

	return bch2_trans_update(trans, dedup_iter, &new->k_i, 0);
}

/*
 * Update a dedup index entry after conversion: store the reflink_v
 * index so future duplicates go straight to the reflink_v.
 */
static int dedup_index_update_reflink(struct btree_trans *trans,
				      struct btree_iter *dedup_iter,
				      struct bkey_s_c dedup_k,
				      u64 reflink_idx)
{
	struct bkey_i *new = bch2_bkey_make_mut_noupdate(trans, dedup_k);
	if (IS_ERR(new))
		return PTR_ERR(new);

	struct bkey_i_dedup *d_key = bkey_i_to_dedup(new);
	struct bch_dedup *d = &d_key->v;
	d->reflink_idx	= cpu_to_le64(reflink_idx);
	d->flags	= cpu_to_le32(BCH_DEDUP_HAS_REFLINK);

	return bch2_trans_update(trans, dedup_iter, new, 0);
}

/*
 * Verify that the source extent still exists and still has the same
 * checksum.  Returns the source key (with iterator held) on success,
 * or bkey_s_c_null if stale.
 */
static struct bkey_s_c dedup_lookup_source(struct btree_trans *trans,
					   struct btree_iter *src_iter,
					   const struct bch_dedup *d,
					   struct bch_extent_crc_unpacked *src_crc)
{
	struct bpos src_pos = SPOS(le64_to_cpu(d->src_inode),
				   le64_to_cpu(d->src_offset),
				   le32_to_cpu(d->src_snapshot));

	bch2_trans_iter_init(trans, src_iter, BTREE_ID_extents, src_pos,
			     BTREE_ITER_intent);

	struct bkey_s_c src_k = bch2_btree_iter_peek_slot(src_iter);
	if (bkey_err(src_k))
		return src_k;

	/* Source must still be a direct data extent */
	if (!bkey_extent_is_direct_data(src_k.k))
		return bkey_s_c_null;

	/* Verify it still has a matching checksum */
	if (!extent_get_crc(trans->c, src_k, src_crc))
		return bkey_s_c_null;

	return src_k;
}

/*
 * Process a single extent for deduplication.
 *
 * Called by the reconcile worker for extents with dedup_pending set.
 *
 * Algorithm:
 *   1. Extract checksum from extent key
 *   2. Look up (csum_type, csum) in BTREE_ID_dedup
 *   3. If not found: insert index entry
 *   4. If found:
 *      a. Quick-reject if sizes differ
 *      b. Look up source, verify still exists with matching checksum
 *      c. Read both extents, byte-compare (security requirement)
 *      d. Make source indirect, convert current to reflink_p
 *      e. Update dedup index with reflink_v index
 */

int bch2_dedup_extent(struct moving_context *ctxt,
		      struct bch_inode_opts *opts,
		      struct btree_iter *extent_iter,
		      struct bkey_s_c k)
{
	struct btree_trans *trans = ctxt->trans;
	struct bch_fs *c = trans->c;
	u32 restart_count_orig = trans->restart_count;

	/* We currently only deduplicate direct extents in the extents tree */
	if (extent_iter->btree_id != BTREE_ID_extents || k.k->type != KEY_TYPE_extent)
		return 0;

	struct bch_extent_crc_unpacked crc;
	if (!extent_get_crc(c, k, &crc)) {
		pr_err("dedup: extent_get_crc false!\n");
		return 0;
	}

	/* Skip compressed extents — their checksums cover compressed data */
	if (crc_is_compressed(crc)) {
		pr_err("dedup: crc_is_compressed true!\n");
		return 0;
	}

	struct bpos dedup_pos = dedup_pos_from_crc(crc);
	pr_err("dedup: ino=%llu off=%llu sz=%u csum=%llx:%llx\n",
	       k.k->p.inode, k.k->p.offset,
	       k.k->size,
	       crc.csum.hi, crc.csum.lo);

	/* Look up this checksum in the dedup index */
	CLASS(btree_iter, dedup_iter)(trans, BTREE_ID_dedup, dedup_pos,
				     BTREE_ITER_intent);
	struct bkey_s_c dedup_k = bch2_btree_iter_peek_slot(&dedup_iter);
	if (bkey_err(dedup_k)) {
		pr_err("dedup: peek_slot error=%d\n", bkey_err(dedup_k));
		return bkey_err(dedup_k);
	}
	pr_err("dedup: peek_slot ok type=%d\n", dedup_k.k->type);

	if (dedup_k.k->type != KEY_TYPE_dedup) {
		/*
		 * No existing entry — this is the first extent we've
		 * seen with this checksum.  Record it in the index.
		 */
		int ret;
		
		ret = dedup_index_insert(trans, &dedup_iter, k, k.k->size);
		pr_err("dedup: index_insert ret=%d\n", ret);
		if (ret)
			return ret;
		
		ret = dedup_clear_pending(trans, extent_iter, k);
		pr_err("dedup: clear_pending ret=%d\n", ret);
		if (ret)
			return ret;

		ret = bch2_trans_commit(trans, NULL, NULL,
					BCH_TRANS_COMMIT_no_enospc);
		pr_err("dedup: trans_commit ret=%d\n", ret);
		if (!ret)
			this_cpu_inc(c->counters.now[BCH_COUNTER_dedup_extent_indexed]);
		return ret;
	}

	/*
	 * Found existing entry — a previous extent with the same
	 * checksum is indexed.
	 */
	const struct bch_dedup *d =
		(const struct bch_dedup *) dedup_k.v;

	/* Quick reject: size mismatch means different content */
	if (le32_to_cpu(d->size_sectors) != k.k->size) {
		pr_err("dedup: size mismatch d=%u k=%u\n", le32_to_cpu(d->size_sectors), k.k->size);
		return 0;
	}

	/* Don't dedup an extent with itself */
	if (le64_to_cpu(d->src_inode)  == k.k->p.inode &&
	    le64_to_cpu(d->src_offset) == k.k->p.offset) {
		pr_err("dedup: self-dedup skip inode=%llu offset=%llu\n",
		       k.k->p.inode, k.k->p.offset);
		return 0;
	}
	pr_err("dedup: found match, proceeding to verify\n");

	/*
	 * Look up the source extent to verify it still exists and
	 * still has the right checksum.
	 */
	CLASS(btree_iter_uninit, src_iter)(trans);
	struct bch_extent_crc_unpacked src_crc;
	struct bkey_s_c src_k = dedup_lookup_source(trans, &src_iter, d, &src_crc);

	if (bkey_err(src_k))
		return bkey_err(src_k);

	if (!src_k.k) {
		/*
		 * Source is gone or changed — stale entry.
		 * Update the index to point to the current extent instead.
		 */
		try(dedup_index_insert(trans, &dedup_iter, k, k.k->size));
		try(dedup_clear_pending(trans, extent_iter, k));
		this_cpu_inc(c->counters.now[BCH_COUNTER_dedup_stale_entry]);
		return bch2_trans_commit(trans, NULL, NULL,
					BCH_TRANS_COMMIT_no_enospc);
	}

	/* Verify checksums actually match (type + value) */
	if (src_crc.csum_type != crc.csum_type ||
	    src_crc.csum.lo != crc.csum.lo ||
	    src_crc.csum.hi != crc.csum.hi)
		return 0;

	/*
	 * Save our current keys so we can verify they haven't changed
	 * after we drop locks for I/O.
	 */
	struct bkey_buf src_saved __cleanup(bch2_bkey_buf_exit);
	bch2_bkey_buf_init(&src_saved);
	bch2_bkey_buf_reassemble(&src_saved, src_k);

	/* 
	 * The extent key 'k' we were passed might have an old bversion 
	 * if it was just modified by bch2_update_reconcile_opts.
	 * Re-peek to get the latest version before saving.
	 */
	k = bkey_try(bch2_btree_iter_peek_slot(extent_iter));
	if (!k.k)
		return 0;

	struct bkey_buf dst_saved __cleanup(bch2_bkey_buf_exit);
	bch2_bkey_buf_init(&dst_saved);
	bch2_bkey_buf_reassemble(&dst_saved, k);

	/* Drop locks for blocking I/O */
	bch2_trans_unlock_long(trans);

	/*
	 * Read both extents and byte-compare to be certain.
	 * This is a security requirement: CRC is not cryptographic,
	 * so a malicious user could craft files with matching CRCs
	 * to steal or corrupt another user's data through dedup.
	 */
	struct bio *bio_src = NULL, *bio_dst = NULL;
	int ret;

	pr_err("dedup: reading src extent\n");
	ret = dedup_read_extent(trans, bkey_i_to_s_c(src_saved.k), &bio_src);
	if (ret) {
		pr_err("dedup: reading src extent failed ret=%d\n", ret);
		return ret;
	}

	pr_err("dedup: reading dst extent\n");
	ret = dedup_read_extent(trans, bkey_i_to_s_c(dst_saved.k), &bio_dst);
	if (ret) {
		pr_err("dedup: reading dst extent failed ret=%d\n", ret);
		bio_free_and_put(bio_src);
		return ret;
	}

	pr_err("dedup: byte-comparing extents\n");
	bool match = bio_data_equal(bio_src, bio_dst);

	bio_free_and_put(bio_src);
	bio_free_and_put(bio_dst);

	if (!match) {
		pr_err("dedup: byte-verify mismatch! CRC collision!\n");
		/* Checksum collision — skip silently */
		this_cpu_inc(c->counters.now[BCH_COUNTER_dedup_byte_verify_mismatch]);
		return 0;
	}

	/*
	 * We use commit_do to safely perform the btree updates.
	 * Inside the commit block, we verify the extents haven't changed.
	 */
	int dedup_ret = commit_do(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc, ({
		int _ret = 0;


		struct bkey_s_c cur_src = bch2_btree_iter_peek_slot(&src_iter);
		if (bkey_err(cur_src)) {
			pr_info("bch2_dedup_extent() dedup: cur_src bkey_err %d\n", bkey_err(cur_src));
			_ret = bkey_err(cur_src);
			goto out;
		}

		struct bkey_s_c cur_dst = bch2_btree_iter_peek_slot(extent_iter);
		if (bkey_err(cur_dst)) {
			pr_info("bch2_dedup_extent() dedup: cur_dst bkey_err %d\n", bkey_err(cur_dst));
			_ret = bkey_err(cur_dst);
			goto out;
		}

		if (!cur_src.k || !cur_dst.k) {
			pr_info("bch2_dedup_extent() dedup: verify iterators NULL\n");
			goto out;
		}

		if (!bpos_eq(cur_src.k->p, src_saved.k->k.p) ||
		    cur_src.k->size != src_saved.k->k.size ||
		    cur_src.k->bversion.lo != src_saved.k->k.bversion.lo ||
		    cur_src.k->bversion.hi != src_saved.k->k.bversion.hi) {
			pr_info("bch2_dedup_extent() dedup: source changed pos %llu-%llu, size %llu vs %llu\n",
				(u64)cur_src.k->p.offset, (u64)src_saved.k->k.p.offset,
				(u64)cur_src.k->size, (u64)src_saved.k->k.size);
			goto out;
		}

		if (!bpos_eq(cur_dst.k->p, dst_saved.k->k.p) ||
		    cur_dst.k->size != dst_saved.k->k.size ||
		    cur_dst.k->bversion.lo != dst_saved.k->k.bversion.lo ||
		    cur_dst.k->bversion.hi != dst_saved.k->k.bversion.hi) {
			pr_info("bch2_dedup_extent() dedup: dest changed pos %llu-%llu, size %llu vs %llu\n",
				(u64)cur_dst.k->p.offset, (u64)dst_saved.k->k.p.offset,
				(u64)cur_dst.k->size, (u64)dst_saved.k->k.size);
			goto out;
		}

		if (!bkey_extent_is_direct_data(cur_src.k) ||
		    !bkey_extent_is_direct_data(cur_dst.k)) {
			pr_info("bch2_dedup_extent() dedup: not direct data anymore\n");
			goto out;
		}

		/* Step 1: Make the source extent indirect (creates reflink_v) */
		struct bkey_buf src_indirect_buf __cleanup(bch2_bkey_buf_exit);
		bch2_bkey_buf_init(&src_indirect_buf);
		bch2_bkey_buf_reassemble(&src_indirect_buf, cur_src);

		_ret = bch2_make_extent_indirect(trans, &src_iter, src_indirect_buf.k, false);
		if (_ret) {
			pr_info("bch2_dedup_extent() dedup: make_extent_indirect ret=%d\n", _ret);
			goto out;
		}
		BUG_ON(src_indirect_buf.k->k.type != KEY_TYPE_reflink_p);

		/* Step 2: Create a reflink_p for the destination extent */
		struct bkey_i_reflink_p *src_p = bkey_i_to_reflink_p(src_indirect_buf.k);
		u64 reflink_idx = REFLINK_P_IDX(&src_p->v);

		struct bkey_buf dst_indirect_buf __cleanup(bch2_bkey_buf_exit);
		bch2_bkey_buf_init(&dst_indirect_buf);

		bkey_init(&dst_indirect_buf.k->k);
		dst_indirect_buf.k->k.type = KEY_TYPE_reflink_p;
		set_bkey_val_bytes(&dst_indirect_buf.k->k, sizeof(struct bch_reflink_p));

		struct bkey_i_reflink_p *dst_p = bkey_i_to_reflink_p(dst_indirect_buf.k);
		memset(&dst_p->v, 0, sizeof(dst_p->v));

		dst_indirect_buf.k->k.p = cur_dst.k->p;
		dst_indirect_buf.k->k.size = cur_dst.k->size;

		SET_REFLINK_P_IDX(&dst_p->v, reflink_idx);

		_ret = bch2_trans_update(trans, extent_iter, &dst_p->k_i,
				      BTREE_UPDATE_internal_snapshot_node);
		if (_ret) {
			pr_info("bch2_dedup_extent() dedup: trans_update ret=%d\n", _ret);
			goto out;
		}

		/* Step 3: Update dedup index to record the reflink_v */
		_ret = dedup_index_update_reflink(trans, &dedup_iter, dedup_k, reflink_idx);
		if (_ret) goto out;

out:
		_ret;
	}));

	/* suppress trans_was_restarted() check in callers */
	trans->restart_count = restart_count_orig;

	if (dedup_ret == 0) {
		this_cpu_inc(c->counters.now[BCH_COUNTER_dedup_extent_deduped]);
		this_cpu_add(c->counters.now[BCH_COUNTER_dedup_sectors_saved],
			     k.k->size);
	}

	return dedup_ret;
}

void bch2_dedup_val_to_text(struct printbuf *out, struct bch_fs *c,
			    struct bkey_s_c k)
{
	struct bkey_s_c_dedup d = bkey_s_c_to_dedup(k);

	prt_printf(out, "src=%llu:%llu:%u size=%u",
		   le64_to_cpu(d.v->src_inode),
		   le64_to_cpu(d.v->src_offset),
		   le32_to_cpu(d.v->src_snapshot),
		   le32_to_cpu(d.v->size_sectors));

	if (le32_to_cpu(d.v->flags) & BCH_DEDUP_HAS_REFLINK)
		prt_printf(out, " reflink_idx=%llu", le64_to_cpu(d.v->reflink_idx));
}
