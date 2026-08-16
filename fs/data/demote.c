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
 * controls, the extent has >= data_replicas AUTHORITATIVE copies; the
 * flip promotes as many cached legs as it kills ptrs (the demote
 * selector kills at most one ptr per op, the update writes one leg
 * per killed ptr), so the authoritative count is monotone across the
 * transition and the cached surplus is never load-bearing for the
 * invariant.
 *
 * Revalidation prefers EXACT: the arm re-reads the published key and
 * records it; an untouched key commits against the recorded image. If
 * the key changed while the flip waited for debt coverage (copygc or
 * another mover rewrote the extent), the flip re-derives against the
 * CURRENT key instead of aborting: if the demote's cached legs survive
 * (possibly moved to new buckets), the kill mask is remapped and the
 * flip commits against the current key. A changed leg ptr is a NEW
 * write, covered only by an exchange that ran after it - the leg
 * devices' debt tickets are refreshed (bounded, see FLIP_REFRESH_MAX)
 * and the flip waits for the rewritten legs. Only a leg that is truly
 * gone drops the flip; an authoritative ptr already on a leg device
 * means the concurrent rewrite demoted the extent itself and nothing
 * re-runs. Aborting on every key change instead re-demotes the extent
 * and is the amplification loop measured in the stall rig: 10x
 * data_update, zero flip commits, the background device permanently
 * saturated.
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
/* Bound the per-flip debt-ticket refreshes: a leg that keeps being
 * rewritten every exchange epoch must not loop the flip forever; after
 * this many refreshes the flip drops and the demote re-runs. */
#define FLIP_REFRESH_MAX	4

struct demote_flip {
	struct list_head	list;
	enum btree_id		btree_id;
	struct bbpos		pos;
	struct bkey_buf		k;		/* the published key, for exact revalidation */
	struct bch_devs_mask	devs;		/* cached-leg devices to un-cache */
	u64			ticket[BCH_SB_MEMBERS_MAX];
	unsigned		flip_ptrs_kill;
	unsigned		refreshes;
	enum {
		FLIP_UNFINISHED,
		FLIP_UPDATED,		/* the try queued an update */
		FLIP_REDUNDANT,		/* a concurrent rewrite demoted it */
		FLIP_DROPPED,		/* the cached legs are gone */
	}			state;
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
		prt_printf(&msg.m, "demote flip: armed %llu:%llu:%u kill 0x%x\n",
			   u->k.k->k.p.inode, u->k.k->k.p.offset,
			   u->k.k->k.p.snapshot, u->flip_ptrs_kill);
	}

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return;

	f->btree_id	= u->btree_id;
	f->pos		= u->pos;
	bch2_bkey_buf_init(&f->k);

	/*
	 * The mask was computed against the key the op started from; the
	 * index update may have rewritten the published key in between
	 * (extra-durability drops, cached ptr cleanup), shifting ptr
	 * positions. Remap below once the published key is re-read.
	 */
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
	struct bkey_s_c peeked = {};
	{
		CLASS(btree_trans, trans)(c);
		CLASS(btree_iter, iter)(trans, u->btree_id,
					bkey_start_pos(&u->k.k->k),
					BTREE_ITER_slots);
		struct bkey_s_c k = bch2_btree_iter_peek_slot(&iter);

		peeked = k;
		if (k.k) {
			bch2_bkey_buf_reassemble(&f->k, k);
			f->flip_ptrs_kill =
				ptr_mask_remap(c, bkey_i_to_s_c(u->k.k),
					       u->flip_ptrs_kill, k);

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
		if (IS_ENABLED(CONFIG_BCACHEFS_DEBUG)) {
			CLASS(bch_log_msg_ratelimited, msg)(c);
			prt_printf(&msg.m, "demote flip: no leg at %u:%llu:%llu:%u written_devs 0x%lx\n",
				   u->btree_id, u->k.k->k.p.inode, u->k.k->k.p.offset,
				   u->k.k->k.p.snapshot, u->op.written_devs.d[0]);
		}
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

	if (IS_ENABLED(CONFIG_BCACHEFS_DEBUG)) {
		CLASS(bch_log_msg_ratelimited, msg)(c);
		prt_printf(&msg.m, "demote flip: queued %u:%llu:%llu:%u (%u pending)\n",
			   f->pos.btree, f->pos.pos.inode, f->pos.pos.offset,
			   f->pos.pos.snapshot, c->demote_flips_pending);
	}

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

	if (ret && IS_ENABLED(CONFIG_BCACHEFS_DEBUG)) {
	}

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
 * way: committed, redundant (a concurrent rewrite already demoted the
 * extent - nothing re-runs), raced (aborted - the cached copy is GC'd
 * and the demote re-runs), or failed. Returns false when the flip needs
 * another wait pass: transaction restarts, and ticket refreshes (the
 * leg ptr changed, the flip must wait for the rewritten leg's coverage
 * before committing - committing inside this transaction would make an
 * uncovered ptr authoritative).
 */
static bool demote_flip_try(struct bch_fs *c, struct demote_flip *f)
{
	CLASS(btree_trans, trans)(c);
	CLASS(disk_reservation, res)(c);
	int ret;
	bool refreshed = false;

	ret = for_each_btree_key_commit(trans, iter, f->btree_id,
			bkey_start_pos(&f->k.k->k),
			BTREE_ITER_slots|BTREE_ITER_intent,
			k, &res.r, NULL, 0, ({
		int _ret = 0;
		unsigned flip_ptrs_kill = f->flip_ptrs_kill;

		/* walk the published key's range; break only past it - the
		 * key AT the published pos is the match candidate (le broke
		 * on equality and every flip dropped, measured) */
		if (bkey_lt(f->k.k->k.p, bkey_start_pos(k.k)))
			break;

		/* exact revalidation: the published key must be untouched
		 * (both are unpacked bkeys of the same format; compare the
		 * full byte image) */
		bool match = k.k->u64s == f->k.k->k.u64s &&
			!memcmp(k.k, &f->k.k->k, bkey_bytes(k.k));

		if (!match) {
			/*
			 * The key changed while the flip waited for debt
			 * coverage (copygc/mover rewrite). Re-derive against
			 * the current key: if the demote's cached legs
			 * survive, remap the kill mask and commit against the
			 * current key. Dropping here instead is the measured
			 * amplification loop (stall rig: 10x data_update,
			 * zero commits, the background device permanently
			 * saturated).
			 */
			struct bkey_ptrs_c cptrs = bch2_bkey_ptrs_c(k);
			const struct bch_extent_ptr *cptr;
			bool leg = false, leg_changed = false;

			bkey_for_each_ptr(cptrs, cptr) {
				if (!cptr->cached ||
				    !test_bit(cptr->dev, f->devs.d))
					continue;
				leg = true;

				/* is this exact leg ptr in the recorded key? */
				struct bkey_ptrs_c rptrs =
					bch2_bkey_ptrs_c(bkey_i_to_s_c(f->k.k));
				const struct bch_extent_ptr *rptr;
				bool found = false;

				bkey_for_each_ptr(rptrs, rptr)
					if (rptr->cached &&
					    rptr->dev == cptr->dev &&
					    rptr->offset == cptr->offset &&
					    rptr->generation == cptr->generation) {
						found = true;
						break;
					}
				if (!found)
					leg_changed = true;
			}

			if (!leg) {
				/*
				 * No cached leg left on the leg devices. An
				 * authoritative ptr there means whoever
				 * rewrote the key demoted the extent itself;
				 * otherwise the leg is truly gone and
				 * reconcile re-demotes the ptrs.
				 */
				bool done = false;

				bkey_for_each_ptr(cptrs, cptr)
					if (!cptr->cached &&
					    test_bit(cptr->dev, f->devs.d)) {
						done = true;
						break;
					}
				f->state = done ? FLIP_REDUNDANT : FLIP_DROPPED;
				break;
			}

			flip_ptrs_kill = ptr_mask_remap(c,
					bkey_i_to_s_c(f->k.k),
					f->flip_ptrs_kill, k);

			/*
			 * A changed leg ptr is a NEW write: its durability
			 * is covered only by an exchange that ran after it.
			 * Refresh the leg devices' tickets and wait for the
			 * rewritten legs' coverage (never commit inside this
			 * transaction). Bounded: a leg that keeps being
			 * rewritten every exchange epoch must not loop the
			 * flip forever.
			 */
			if (leg_changed) {
				unsigned dev;

				if (f->refreshes >= FLIP_REFRESH_MAX) {
					f->state = FLIP_DROPPED;
					break;
				}
				f->refreshes++;
				for_each_set_bit(dev, f->devs.d,
						 BCH_SB_MEMBERS_MAX)
					f->ticket[dev] = bch2_journal_debt_add(c, dev);
				refreshed = true;
				break;
			}
		}

		f->state = FLIP_UPDATED;

		/*
		 * Staged-flip invariant, in durability units: the flip may
		 * cache a killed ptr only against leg durability that
		 * actually covers it. Compute the leg and kill durability
		 * from the devices the op really wrote to / the ptrs it
		 * really kills, and shrink the kill mask to a coverable
		 * subset (first-fit - under-killing is safe); the
		 * uncovered ptrs stay authoritative and reconcile
		 * re-demotes them in later ops.
		 */
		unsigned kill_mask = flip_ptrs_kill;
		{
			unsigned legs_durability = 0, kill_durability = 0;
			unsigned dev;
			for_each_set_bit(dev, f->devs.d, BCH_SB_MEMBERS_MAX)
				legs_durability += bch2_dev_durability(c, dev);

			const union bch_extent_entry *entry;
			struct extent_ptr_decoded p;
			unsigned ptr_bit = 1;
			bkey_for_each_ptr_decode(k.k, bch2_bkey_ptrs_c(k), p, entry) {
				if (ptr_bit & kill_mask)
					kill_durability += bch2_dev_durability(c, p.ptr.dev);
				ptr_bit <<= 1;
			}

			if (legs_durability < kill_durability) {
				unsigned covered = 0, subset = 0;
				ptr_bit = 1;
				bkey_for_each_ptr_decode(k.k, bch2_bkey_ptrs_c(k), p, entry) {
					unsigned d = bch2_dev_durability(c, p.ptr.dev);
					if ((ptr_bit & kill_mask) &&
					    covered + d <= legs_durability) {
						covered += d;
						subset |= ptr_bit;
					}
					ptr_bit <<= 1;
				}
				kill_mask = subset;
			}
		}

		if (kill_mask) {
			struct bkey_i *new = errptr_try(bch2_bkey_make_mut_noupdate(trans, k));
			struct bkey_ptrs ptrs = bch2_bkey_ptrs(bkey_i_to_s(new));
			struct bch_extent_ptr *ptr;
			unsigned ptr_bit = 1;

			bkey_for_each_ptr(ptrs, ptr) {
				if (ptr->cached && test_bit(ptr->dev, f->devs.d))
					ptr->cached = false;
				if (ptr_bit & kill_mask)
					ptr->cached = true;
				ptr_bit <<= 1;
			}

			if (IS_ENABLED(CONFIG_BCACHEFS_DEBUG)) {
				/*
				 * Staged-flip invariant: the extent's total
				 * durability must never drop across a flip -
				 * the flip promotes exactly the leg durability
				 * it kills against, so the extent never goes
				 * below data_replicas.
				 */
				struct bkey_durability pre = {}, post = {};
				int pre_ret = bch2_bkey_durability(trans, k, &pre);
				int post_ret = bch2_bkey_durability(trans, bkey_i_to_s_c(new), &post);

				if (!pre_ret && !post_ret && post.total < pre.total)
					bch_err_ratelimited(c,
						"demote flip: durability DROPPED %u -> %u (kill 0x%x) %llu:%llu",
						pre.total, post.total, kill_mask,
						f->k.k->k.p.inode, f->k.k->k.p.offset);
			}

			_ret = bch2_trans_update(trans, &iter, new, 0);
		} else {
			/*
			 * Nothing coverable: drop the flip - the cached legs
			 * are GC'd and reconcile re-demotes the ptrs.
			 */
			f->state = FLIP_DROPPED;
		}
		_ret;
	}));

	if (ret && IS_ENABLED(CONFIG_BCACHEFS_DEBUG)) {
		CLASS(bch_log_msg_ratelimited, msg)(c);
		prt_printf(&msg.m, "demote flip: try failed %llu:%llu ret %s\n",
			   f->k.k->k.p.inode, f->k.k->k.p.offset, bch2_err_str(ret));
	}

	if (bch2_err_matches(ret, BCH_ERR_transaction_restart))
		return false;

	if (refreshed)
		return false;

	if (!ret && IS_ENABLED(CONFIG_BCACHEFS_DEBUG)) {
		CLASS(bch_log_msg_ratelimited, msg)(c);
		prt_printf(&msg.m, "demote flip: %s %llu:%llu kill 0x%x\n",
			   f->state == FLIP_UPDATED ? "committed" :
			   f->state == FLIP_REDUNDANT ? "already demoted" :
			   "dropped",
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
