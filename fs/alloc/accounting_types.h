/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_DISK_ACCOUNTING_TYPES_H
#define _BCACHEFS_DISK_ACCOUNTING_TYPES_H

#include "util/cuckoo.h"

struct accounting_mem_entry {
	struct bpos				pos;
	unsigned				nr_counters;
	/*
	 * Normal and gc counters. v[0] doubles as the hash table's
	 * slot-in-use marker (accounting_cuckoo_ops.entry_empty): a live entry
	 * always has it allocated. pos can't serve - POS_MIN byte swabs to an
	 * all zero disk_accounting_pos, which is a real key (nr_inodes).
	 */
	u64 __percpu				*v[2];
};

struct bch_accounting_mem {
	struct cuckoo_table			t;
	bool					gc_running;
};

#endif /* _BCACHEFS_DISK_ACCOUNTING_TYPES_H */
