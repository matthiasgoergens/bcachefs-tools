/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_CUCKOO_H
#define _BCACHEFS_CUCKOO_H

/*
 * Cuckoo hash table
 *
 * Open addressed, with entries stored inline in the slot array and exactly two
 * candidate slots per key: a lookup touches at most two cache lines, with no
 * probe sequence to walk and no chain to chase.
 *
 * Insert takes either of the key's two slots if one is free. If both are full
 * it evicts one occupant and re-homes it in *its* other slot, repeating until
 * something lands somewhere free. That walk is bounded (CUCKOO_MAX_KICKS);
 * when it runs out the insert failed, and the caller grows the table with
 * cuckoo_resize() and retries.
 *
 * Deletion is just clearing the slot. A key only ever lives in one of two fixed
 * slots, so removing one can't break any other key's lookup: no tombstones, and
 * the table never needs compacting.
 *
 * Load factor: two slots per key and one entry per slot means the table stops
 * accepting new keys somewhere around 50% full. That's a property of 2-way
 * cuckoo, not of this implementation - cuckoo_too_full() is where to grow, and
 * past it inserts start doing long eviction walks before failing outright.
 *
 * Genericity is by ops struct, like rhashtable_params. Pass a static const
 * struct cuckoo_ops and the entry size, key offset and indirect calls all
 * constant fold away.
 *
 * Locking: none, the caller serializes. There is no support for lockless
 * readers and it can't be bolted on: eviction moves a live entry between slots
 * with a non-atomic swap, and resize replaces the whole slot array.
 */

#include <linux/bug.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

/*
 * Bound on the eviction walk. Exceeding it means the walk hit a cycle (or the
 * table is simply too full); either way the answer is a bigger table.
 *
 * cuckoo_insert() keeps a u32 per kick on the stack to unwind with, so this is
 * also what a failed insert costs to undo.
 */
#define CUCKOO_MAX_KICKS	32

/* Bound on entry_size, for the rehash scratch buffer in cuckoo_resize() */
#define CUCKOO_MAX_ENTRY_SIZE	128

struct cuckoo_table {
	u64			seed[2];
	/*
	 * mask is nr_slots - 1, and only meaningful when slots is non NULL: an
	 * empty table is all zeroes, and nr_slots == 0 has no valid mask.
	 */
	size_t			mask;
	size_t			nr;		/* live entries */
	void			*slots;
};

struct cuckoo_ops {
	size_t			entry_size;

	/*
	 * Keys are compared with memcmp over key_size bytes, so the key type
	 * must have no padding and no two representations of the same value -
	 * a struct with a hole in it will fail to match itself.
	 */
	size_t			key_offset;
	size_t			key_size;

	/*
	 * Both slot indices are cut out of one 64 bit hash, low half and high
	 * half - which caps the table at 2^32 slots.
	 */
	u64			(*hash)(const void *key, size_t key_size, const u64 seed[2]);

	/*
	 * Is this slot free? Must report true for an all zeroes slot: that's
	 * how a fresh table and cuckoo_remove() mark slots free.
	 */
	bool			(*entry_empty)(const void *entry);
};

/*
 * Xor the key down to one word, then two rounds of multiply-xorshift - the
 * mixing splitmix64 finalizes with. With a static const ops the key size is
 * constant, so this unrolls to a couple of loads, two multiplies and a few
 * shifts: no call, no context, nothing on the stack.
 *
 * Not cryptographic. What cuckoo needs is avalanche, so that a key's two slot
 * indices aren't correlated; it does not need to resist anyone choosing keys.
 * A caller whose keys are untrusted should supply something stronger through
 * ops->hash.
 *
 * Measured against jhash on 1026 realistic disk_accounting_pos keys (20 bytes,
 * mostly zeroes, small values in a few fixed byte positions - the low entropy
 * case that punishes a weak hash), packing into a fixed 1024 slot table over
 * 200 seeds: 53.6% mean achievable load vs jhash's 53.9%, i.e. the same, both
 * sitting on 2-way cuckoo's own ~50% threshold rather than on the hash. So the
 * hash isn't what limits the table, and this is the cheap end of "at least as
 * good as jhash".
 */
#define CUCKOO_HASH_MUL		0x9E3779B97F4A7C15ULL

/*
 * __always_inline, not inline: the loop makes gcc's inliner decline this even
 * after it has devirtualized ops->hash, so we got an out-of-line call per lookup
 * - 13% of a gc profile, with its own symbol in perf. Inlined, key_size is a
 * constant and the loop unrolls to a few loads and two multiplies.
 */
static __always_inline u64 cuckoo_hash_bytes(const void *key, size_t key_size, const u64 seed[2])
{
	const u8 *p = key;
	u64 h = seed[0];
	size_t i = 0;

	for (; i + sizeof(u64) <= key_size; i += sizeof(u64))
		h ^= get_unaligned((const u64 *) (p + i));

	for (; i < key_size; i++)
		h ^= (u64) p[i] << ((i & 7) * 8);

	h = (h ^ seed[1]) * CUCKOO_HASH_MUL;
	h ^= h >> 32;
	h *= CUCKOO_HASH_MUL;
	return h ^ (h >> 29);
}

static inline void *__cuckoo_slot(const struct cuckoo_table *t,
				  const struct cuckoo_ops *ops, size_t idx)
{
	return t->slots + idx * ops->entry_size;
}

static inline u64 __cuckoo_hash(const struct cuckoo_table *t,
				const struct cuckoo_ops *ops, const void *entry)
{
	return ops->hash(entry + ops->key_offset, ops->key_size, t->seed);
}

/*
 * The two slots for a key are i and i ^ delta, delta coming out of the high half
 * of the hash. Relating them by xor makes "the other slot" an involution: an
 * entry sitting in one of its slots gets to the other with a single xor, and
 * xoring again brings it back. That's what lets the eviction walk carry an entry
 * onwards without working out which slot it was just evicted from - and makes it
 * impossible to accidentally send it back there.
 *
 * delta is forced odd before masking, so the two slots are always distinct (a
 * key whose slots collided could never be displaced at all) and always within
 * the table.
 */
static inline size_t __cuckoo_delta(const struct cuckoo_table *t, u64 h)
{
	return ((h >> 32) | 1) & t->mask;
}

static inline void *cuckoo_lookup(const struct cuckoo_table *t,
				  const struct cuckoo_ops *ops, const void *key)
{
	if (unlikely(!t->slots))
		return NULL;

	u64 h = ops->hash(key, ops->key_size, t->seed);

	size_t i = h & t->mask;
	void *e0 = __cuckoo_slot(t, ops, i);
	void *e1 = __cuckoo_slot(t, ops, i ^ __cuckoo_delta(t, h));

	if (!ops->entry_empty(e0) &&
	    !memcmp(e0 + ops->key_offset, key, ops->key_size))
		return e0;

	if (!ops->entry_empty(e1) &&
	    !memcmp(e1 + ops->key_offset, key, ops->key_size))
		return e1;

	return NULL;
}

static inline void cuckoo_memswap(void *a, void *b, size_t size)
{
	u8 *p = a, *q = b;

	for (size_t i = 0; i < size; i++)
		swap(p[i], q[i]);
}

/*
 * Insert the entry in @entry, which the caller owns and which this uses as
 * working space - it must not point into the table itself.
 *
 * Does not check whether the key is already present: inserting a key twice puts
 * two copies in the table, and removing one leaves the other to reappear as a
 * stale entry. Callers that can race must cuckoo_lookup() first.
 *
 * Failure leaves the table exactly as it was and @entry still holding the entry
 * we were asked to insert: the eviction walk records the slot it displaced at
 * each step, and running that back restores every entry to where it started.
 * So the retry loop
 *
 *	while (!cuckoo_insert(t, ops, &e))
 *		try(cuckoo_resize(t, ops, ...));
 *
 * can bail out anywhere without losing anything - on the error path @e is the
 * caller's own entry, never some other key the walk was carrying.
 */
static inline bool cuckoo_insert(struct cuckoo_table *t,
				 const struct cuckoo_ops *ops, void *entry)
{
	if (unlikely(!t->slots))
		return false;

	/* Slot displaced at each step, for unwinding: see below */
	u32 walk[CUCKOO_MAX_KICKS];
	unsigned kicks = 0;

	u64 h = __cuckoo_hash(t, ops, entry);
	size_t slot = h & t->mask;
	size_t other = slot ^ __cuckoo_delta(t, h);

	/* Take whichever of the key's own two slots is free, if either is */
	if (!ops->entry_empty(__cuckoo_slot(t, ops, slot)) &&
	    ops->entry_empty(__cuckoo_slot(t, ops, other)))
		slot = other;

	for (; kicks < CUCKOO_MAX_KICKS; kicks++) {
		void *e = __cuckoo_slot(t, ops, slot);

		if (ops->entry_empty(e)) {
			memcpy(e, entry, ops->entry_size);
			t->nr++;
			return true;
		}

		/*
		 * Occupied: evict into @entry and carry the evicted one on to
		 * its other slot. It was living in @slot, so xoring by its delta
		 * is that other slot - and can't be @slot again.
		 */
		cuckoo_memswap(entry, e, ops->entry_size);
		walk[kicks] = slot;

		slot ^= __cuckoo_delta(t, __cuckoo_hash(t, ops, entry));
	}

	/*
	 * Out of kicks. Undo the walk, last step first: each swap puts the entry
	 * we're carrying back in the slot it was displaced from and picks up the
	 * one that displaced it, so the last swap hands us back our own entry.
	 *
	 * A swap is its own inverse, and these are the forward swaps in reverse
	 * order, so they cancel from the inside out - exact even when the walk
	 * cycled and @walk names the same slot more than once.
	 */
	while (kicks--)
		cuckoo_memswap(entry, __cuckoo_slot(t, ops, walk[kicks]), ops->entry_size);

	return false;
}

static inline void cuckoo_remove(struct cuckoo_table *t,
				 const struct cuckoo_ops *ops, void *entry)
{
	BUG_ON(ops->entry_empty(entry));

	memset(entry, 0, ops->entry_size);
	--t->nr;
}

/*
 * Next live entry at or after @e, for iterating the whole table; @e starts at
 * t->slots. Callers wrap this in a typed macro.
 */
static inline void *cuckoo_next_live(const struct cuckoo_table *t,
				     const struct cuckoo_ops *ops, void *e)
{
	if (unlikely(!t->slots))
		return NULL;

	for (; e < t->slots + (t->mask + 1) * ops->entry_size; e += ops->entry_size)
		if (!ops->entry_empty(e))
			return e;

	return NULL;
}

/*
 * Rehash into a table of @nr_slots (a power of two) with a fresh seed.
 *
 * Entries are copied, not moved, so a rehash that doesn't fit leaves the old
 * table untouched and loses nothing - the caller just tries a bigger size.
 */
static inline bool cuckoo_resize(struct cuckoo_table *t, const struct cuckoo_ops *ops,
				 size_t nr_slots, gfp_t gfp)
{
	BUG_ON(!is_power_of_2(nr_slots));
	BUG_ON(ops->entry_size > CUCKOO_MAX_ENTRY_SIZE);

	struct cuckoo_table n = {
		.seed	= { get_random_u64(), get_random_u64() },
		.mask	= nr_slots - 1,
		.slots	= kvmalloc_array(nr_slots, ops->entry_size, gfp|__GFP_ZERO),
	};

	if (!n.slots)
		return false;

	for (void *src = cuckoo_next_live(t, ops, t->slots);
	     src;
	     src = cuckoo_next_live(t, ops, src + ops->entry_size)) {
		u8 scratch[CUCKOO_MAX_ENTRY_SIZE] __aligned(8);

		memcpy(scratch, src, ops->entry_size);

		if (!cuckoo_insert(&n, ops, scratch)) {
			kvfree(n.slots);
			return false;
		}
	}

	kvfree(t->slots);
	*t = n;
	return true;
}

/* Where to grow: see the load factor note at the top */
static inline bool cuckoo_too_full(const struct cuckoo_table *t)
{
	return !t->slots || t->nr * 2 >= t->mask + 1;
}

static inline void cuckoo_exit(struct cuckoo_table *t)
{
	kvfree(t->slots);
	memset(t, 0, sizeof(*t));
}

#endif /* _BCACHEFS_CUCKOO_H */
