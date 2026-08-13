// SPDX-License-Identifier: GPL-2.0
/*
 * Cached-leg demote flips (stage 2 of dependency-scoped preflush).
 *
 * A demote to a non-foreground target whose replica was written as a
 * cached copy (see the demote_cached_leg gate in
 * bch2_data_update_init()) is armed here after publication. The flip
 * waits for each cached replica's durability-debt ticket to be covered
 * (bch2_journal_debt_ticket_covered()) and then commits ONE atomic key
 * update: the cached ptrs become authoritative and the deferred
 * ptrs_kill ptrs become cached. Invariant: at every instant the design
 * controls, the extent has >= data_replicas authoritative-durable
 * copies; the transient states carry a cached surplus.
 *
 * Revalidation is EXACT: the arm re-reads the published key and records
 * it; the flip commits only if the current key is byte-identical
 * (bkey_eq) to the recorded one. Anything that touched the extent in
 * between aborts the flip conservatively: the cached copy is then GC'd
 * by the existing reconcile cached-ptr cleanup
 * (bch2_bkey_drop_extra_durability()) and the demote re-runs.
 *
 * The queue is RAM-only and bounded (DEMOTE_FLIPS_MAX); when full, new
 * demotes fall back to the fused path (fail-closed). A crash strands
 * cached copies, which the same reconcile cleanup drops. The work
 * demands a flushing commit (JOURNAL_need_flush_write) while flips
 * remain uncovered, since an idle fs only writes noflush entries that
 * never exchange the debt.
 */

#include "bcachefs.h"
#include "alloc/buckets.h"

#include "btree/iter.h"

#include "data/demote.h"
#include "data/extents.h"
#include "data/update.h"

#define DEMOTE_FLIPS_MAX	1024

struct demote_flip {
	struct list_head	list;
	enum btree_id		btree_id;
	struct bbpos		pos;
	struct bkey_buf		k;		/* the published key, for exact revalidation */
	struct bch_devs_mask	devs;		/* cached-leg devices to un-cache */
	u64			ticket[BCH_SB_MEMBERS_MAX];
	unsigned		flip_ptrs_kill;
	bool			updated;	/* the try queued an update */
};

static void demote_flip_free(struct demote_flip *f)
{
	bch2_bkey_buf_exit(&f->k);
	kfree(f);
}

bool bch2_demote_flip_room(struct bch_fs *c)
{
	return READ_ONCE(c->demote_flips_pending) < DEMOTE_FLIPS_MAX;
}

void bch2_demote_flip_arm(struct data_update *u)
{
	struct bch_fs *c = u->op.c;
	struct demote_flip *f;

	if (!u->flip_ptrs_kill || !u->op.written)
		return;

	if (IS_ENABLED(CONFIG_BCACHEFS_DEBUG)) {
		CLASS(bch_log_msg_ratelimited, msg)(c);
		prt_printf(&msg.m, "demote flip: armed %llu:%llu kill 0x%x\n",
			   u->k.k->k.p.inode, u->k.k->k.p.offset, u->flip_ptrs_kill);
	}

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return;

	f->btree_id	= u->btree_id;
	f->pos		= u->pos;
	bch2_bkey_buf_init(&f->k);
	f->flip_ptrs_kill = u->flip_ptrs_kill;

	/*
	 * Re-read the published key and record it verbatim. The leg
	 * devices are the ones this op actually wrote (recorded at endio
	 * in op->written_devs) whose ptrs in the published key are
	 * cached; the in-flight update table excludes concurrent
	 * data_updates on this extent, so those are exactly our leg.
	 * No leg present means a race already changed the key - skip.
	 */
	bool have_leg = false;
	{
		CLASS(btree_trans, trans)(c);
		CLASS(btree_iter, iter)(trans, u->btree_id,
					bkey_start_pos(&u->k.k->k),
					BTREE_ITER_slots);
		struct bkey_s_c k = bch2_btree_iter_peek_slot(&iter);

		if (k.k) {
			bch2_bkey_buf_reassemble(&f->k, k);

			struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(k);
			const struct bch_extent_ptr *ptr;

			bkey_for_each_ptr(ptrs, ptr)
				if (ptr->cached &&
				    test_bit(ptr->dev, u->op.written_devs.d)) {
					__set_bit(ptr->dev, f->devs.d);
					have_leg = true;
				}
		}
	}
	if (!have_leg) {
		demote_flip_free(f);
		return;
	}

	{
		unsigned dev;
		for_each_set_bit(dev, f->devs.d, BCH_SB_MEMBERS_MAX)
			f->ticket[dev] = u->op.debt_tickets[dev];
	}

	spin_lock(&c->demote_flips_lock);
	if (c->demote_flips_pending >= DEMOTE_FLIPS_MAX) {
		/* fail-closed: drop the flip, the cached copy is GC'd and
		 * the demote re-runs fused later */
		spin_unlock(&c->demote_flips_lock);
		demote_flip_free(f);
		return;
	}
	c->demote_flips_pending++;
	list_add_tail(&f->list, &c->demote_flips);
	spin_unlock(&c->demote_flips_lock);

	mod_delayed_work(system_unbound_wq, &c->demote_flip_work, 0);
}

void bch2_demote_flip_wake(struct bch_fs *c)
{
	if (READ_ONCE(c->demote_flips_pending))
		mod_delayed_work(system_unbound_wq, &c->demote_flip_work, 0);
}

bool bch2_demote_flip_pending(struct bch_fs *c, struct bbpos pos)
{
	struct demote_flip *f;
	bool ret = false;

	spin_lock(&c->demote_flips_lock);
	list_for_each_entry(f, &c->demote_flips, list)
		if (f->pos.btree == pos.btree &&
		    f->pos.pos.inode == pos.pos.inode &&
		    f->pos.pos.offset == pos.pos.offset &&
		    f->pos.pos.snapshot == pos.pos.snapshot) {
			ret = true;
			break;
		}
	spin_unlock(&c->demote_flips_lock);

	return ret;
}

static bool demote_flip_covered(struct bch_fs *c, struct demote_flip *f)
{
	unsigned dev;

	for_each_set_bit(dev, f->devs.d, BCH_SB_MEMBERS_MAX)
		if (!bch2_journal_debt_ticket_covered(c, dev, f->ticket[dev]))
			return false;
	return true;
}

/*
 * The flip transaction. Returns true when the flip is finished either
 * way: committed, raced (aborted - the cached copy is GC'd and the
 * demote re-runs), or failed.
 */
static bool demote_flip_try(struct bch_fs *c, struct demote_flip *f)
{
	CLASS(btree_trans, trans)(c);
	CLASS(disk_reservation, res)(c);
	int ret;

	ret = for_each_btree_key_commit(trans, iter, f->btree_id,
			bkey_start_pos(&f->k.k->k),
			BTREE_ITER_slots|BTREE_ITER_intent,
			k, &res.r, NULL, 0, ({
		if (bkey_le(f->k.k->k.p, bkey_start_pos(k.k)))
			break;

		/* exact revalidation: the published key must be untouched
		 * (both are unpacked bkeys of the same format; compare the
		 * full byte image) */
		if (k.k->u64s != f->k.k->k.u64s ||
		    memcmp(k.k, &f->k.k->k, bkey_bytes(k.k)))
			continue;

		f->updated = true;

		struct bkey_i *new = errptr_try(bch2_bkey_make_mut_noupdate(trans, k));
		struct bkey_ptrs ptrs = bch2_bkey_ptrs(bkey_i_to_s(new));
		struct bch_extent_ptr *ptr;
		unsigned ptr_bit = 1;

		bkey_for_each_ptr(ptrs, ptr) {
			if (ptr->cached && test_bit(ptr->dev, f->devs.d))
				ptr->cached = false;
			if (ptr_bit & f->flip_ptrs_kill)
				ptr->cached = true;
			ptr_bit <<= 1;
		}

		bch2_trans_update(trans, &iter, new, 0);
	}));

	if (ret && IS_ENABLED(CONFIG_BCACHEFS_DEBUG)) {
		CLASS(bch_log_msg_ratelimited, msg)(c);
		prt_printf(&msg.m, "demote flip: try failed %llu:%llu ret %s\n",
			   f->k.k->k.p.inode, f->k.k->k.p.offset, bch2_err_str(ret));
	}

	if (bch2_err_matches(ret, BCH_ERR_transaction_restart))
		return false;

	if (!ret && IS_ENABLED(CONFIG_BCACHEFS_DEBUG)) {
		CLASS(bch_log_msg_ratelimited, msg)(c);
		prt_printf(&msg.m, "demote flip: %s %llu:%llu kill 0x%x\n",
			   f->updated ? "committed" : "dropped (key not found)",
			   f->k.k->k.p.inode, f->k.k->k.p.offset, f->flip_ptrs_kill);
	}

	return true;
}

static void demote_flip_work_fn(struct work_struct *work)
{
	struct bch_fs *c = container_of(to_delayed_work(work), struct bch_fs,
					demote_flip_work);
	struct demote_flip *f, *n;
	bool again = false;

	spin_lock(&c->demote_flips_lock);
	list_for_each_entry_safe(f, n, &c->demote_flips, list) {
		if (!demote_flip_covered(c, f)) {
			again = true;
			continue;
		}

		/*
		 * Try with the flip still on the list: the reconcile path
		 * uses list membership as its exclusion, so removing
		 * before the flip's transaction would open a window for
		 * the mover to rewrite the key under it. A restart leaves
		 * it in place for the next pass.
		 */
		spin_unlock(&c->demote_flips_lock);

		if (!demote_flip_try(c, f)) {
			again = true;
		} else {
			spin_lock(&c->demote_flips_lock);
			list_del(&f->list);
			c->demote_flips_pending--;
			spin_unlock(&c->demote_flips_lock);
			demote_flip_free(f);
		}

		spin_lock(&c->demote_flips_lock);
	}
	spin_unlock(&c->demote_flips_lock);

	/*
	 * Flips still pending: an idle fs only writes noflush entries (the
	 * auto-commit timer), which never exchange the debt - force a
	 * flushing commit so the coverage generations actually advance.
	 * Same demand fsync waiters make.
	 */
	if (again) {
		struct journal *j = &c->journal;

		if (!test_bit(JOURNAL_need_flush_write, &j->flags)) {
			set_bit(JOURNAL_need_flush_write, &j->flags);
			mod_delayed_work(j->wq, &j->write_work,
					 msecs_to_jiffies(1));
		}

		mod_delayed_work(system_unbound_wq, &c->demote_flip_work,
				 msecs_to_jiffies(1000));
	}
}

int bch2_demote_flip_init(struct bch_fs *c)
{
	spin_lock_init(&c->demote_flips_lock);
	INIT_LIST_HEAD(&c->demote_flips);
	c->demote_flips_pending = 0;
	INIT_DELAYED_WORK(&c->demote_flip_work, demote_flip_work_fn);
	return 0;
}

void bch2_demote_flip_exit(struct bch_fs *c)
{
	struct demote_flip *f, *n;

	/* partial-fs-init safety: the exit path may run without the
	 * subsystem ever having been initialised */
	if (!c->demote_flip_work.work.func)
		return;

	cancel_delayed_work_sync(&c->demote_flip_work);

	spin_lock(&c->demote_flips_lock);
	list_for_each_entry_safe(f, n, &c->demote_flips, list) {
		list_del(&f->list);
		spin_unlock(&c->demote_flips_lock);
		demote_flip_free(f);
		spin_lock(&c->demote_flips_lock);
	}
	spin_unlock(&c->demote_flips_lock);
}
