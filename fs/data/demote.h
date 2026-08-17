/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_DATA_DEMOTE_H
#define _BCACHEFS_DATA_DEMOTE_H

/*
 * Cached-leg demote flips (stage 2). A demote whose new replica was
 * written as a cached copy (see the gate in bch2_data_update_init())
 * queues a flip here after the cached ptr is published. Once every
 * cached replica's durability-debt ticket is covered
 * (bch2_journal_debt_ticket_covered()), the flip runs one atomic key
 * update: the cached ptrs become authoritative and the deferred
 * ptrs_kill ptrs become cached — at no point does the extent drop
 * below data_replicas authoritative copies.
 *
 * The queue is RAM-only and bounded: a crash strands the cached copies,
 * which the existing reconcile cached-ptr cleanup drops, and the demote
 * simply re-runs.
 */

struct bch_fs;
struct data_update;

void bch2_demote_flip_arm(struct data_update *);
bool bch2_demote_flip_room(struct bch_fs *);
void bch2_demote_flip_wake(struct bch_fs *);
int bch2_demote_flip_init(struct bch_fs *);
void bch2_demote_flip_exit(struct bch_fs *);
bool bch2_demote_flip_pending(struct bch_fs *, struct bbpos);

#endif /* _BCACHEFS_DATA_DEMOTE_H */
