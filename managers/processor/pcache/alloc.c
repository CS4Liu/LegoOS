/*
 * Copyright (c) 2016-2017 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <lego/mm.h>
#include <lego/wait.h>
#include <lego/slab.h>
#include <lego/log2.h>
#include <lego/hash.h>
#include <lego/kernel.h>
#include <lego/pgfault.h>
#include <lego/syscalls.h>
#include <lego/jiffies.h>
#include <processor/pcache.h>
#include <processor/processor.h>

/**
 * sysctl_pcache_alloc_timeout_sec
 *
 * The maximum time a pcache_alloc can take due to slowpath eviction.
 */
unsigned long sysctl_pcache_alloc_timeout_sec __read_mostly = 10;

#define WAIT_TABLE_BITS 8
#define WAIT_TABLE_SIZE (1 << WAIT_TABLE_BITS)
static wait_queue_head_t pcache_bit_wait_table[WAIT_TABLE_SIZE] __cacheline_aligned;

void __init pcache_init_waitqueue(void)
{
	int i;

	for (i = 0; i < WAIT_TABLE_SIZE; i++)
		init_waitqueue_head(pcache_bit_wait_table + i);
}

wait_queue_head_t *pcache_bit_waitqueue(void *word, int bit)
{
	const int shift = BITS_PER_LONG == 32 ? 5 : 6;
	unsigned long val = (unsigned long)word << shift | bit;

	return pcache_bit_wait_table + hash_long(val, WAIT_TABLE_BITS);
}

/*
 * In order to wait for pages to become available there must be
 * waitqueues associated with pages. By using a hash table of
 * waitqueues where the bucket discipline is to maintain all
 * waiters on the same queue and wake all when any of the pages
 * become available, and for the woken contexts to check to be
 * sure the appropriate page became available, this saves space
 * at a cost of "thundering herd" phenomena during rare hash
 * collisions.
 */
wait_queue_head_t *pcache_waitqueue(struct pcache_meta *pcm)
{
	return pcache_bit_waitqueue(pcm, 0);
}

/**
 * unlock_pcache - unlock a locked pcache line
 * @pcm: the pcache line
 *
 * The mb is necessary to enforce ordering between the clear_bit and the read
 * of the waitqueue (to avoid SMP races with xxx).
 */
void unlock_pcache(struct pcache_meta *pcm)
{
	BUG_ON(!PcacheLocked(pcm));

	clear_bit(PC_locked, (unsigned long *)&pcm->bits);
	smp_mb();
	__wake_up_bit(pcache_waitqueue(pcm), &pcm->bits, PC_locked);
}

/**
 * __lock_pcache
 * @pcm: the pcache line to lock
 *
 * Get a lock on the pcache line, assuming we need to sleep to get it.
 */
void __lock_pcache(struct pcache_meta *pcm)
{
	DEFINE_WAIT_BIT(wait, &pcm->bits, PC_locked);

	__wait_on_bit_lock(pcache_waitqueue(pcm), &wait, bit_wait,
			TASK_UNINTERRUPTIBLE);
}

static void bad_pcache(struct pcache_meta *pcm,
		       const char *reason, unsigned long bad_flags)
{
	pr_alert("BUG: Bad pcache state in process %s\n", current->comm);

	dump_pcache_meta(pcm, reason);

	bad_flags &= pcm->bits;
	if (bad_flags)
		pr_alert("bad because of flags: %#lx(%pGc)\n",\
			 bad_flags, &bad_flags);
	/* Leave bad fields for debug */
}

static void pcache_free_check_bad(struct pcache_meta *pcm)
{
	const char *bad_reason;
	unsigned long bad_flags;

	bad_reason = NULL;
	bad_flags = 0;

	/* This is more critical bug */
	if (unlikely(!PcacheAllocated(pcm) || !PcacheUsable(pcm))) {
		bad_reason = "double free";
		bad_pcache(pcm, bad_reason, bad_flags);
		return;
	}

	if (unlikely(atomic_read(&pcm->mapcount) != 0))
		bad_reason = "nonzero mapcount";
	if (unlikely(pcache_ref_count(pcm) != 0))
		bad_reason = "nonzero _refcount";
	if (unlikely(pcm->bits & PCACHE_FLAGS_CHECK_AT_FREE)) {
		bad_reason = "PCACHE_FLAGS_CHECK_AT_FREE flag(s) set";
		bad_flags = PCACHE_FLAGS_CHECK_AT_FREE;
	}
	bad_pcache(pcm, bad_reason, bad_flags);
}

static inline bool pcache_expected_state(struct pcache_meta *pcm,
					 unsigned long check_flags)
{
	/* Flags MUST be set */
	if (unlikely(!PcacheAllocated(pcm) || !PcacheUsable(pcm)))
		return false;

	/* which implies p->rmap list is empty */
	if (unlikely(atomic_read(&pcm->mapcount) != 0))
		return false;

	if (unlikely(pcache_ref_count(pcm)))
		return false;

	/* Flags should not be set */
	if (unlikely(pcm->bits & check_flags))
		return false;

	return true;
}

static inline void pcache_free_check(struct pcache_meta *pcm)
{
	if (likely(pcache_expected_state(pcm, PCACHE_FLAGS_CHECK_AT_FREE)))
		return;
	pcache_free_check_bad(pcm);
}

/* Free a pcache line, return it back to free pool within a set */
void __put_pcache(struct pcache_meta *pcm)
{
	pcache_free_check(pcm);

	detach_from_lru(pcm);

	pcache_reset_flags(pcm);
}

static inline void prep_new_pcache_meta(struct pcache_meta *pcm)
{
	/*
	 * _refcount = 1  for the caller
	 * _mapcount = 0
	 */
	init_pcache_ref_count(pcm);
	pcache_mapcount_reset(pcm);

	INIT_LIST_HEAD(&pcm->rmap);
	init_pcache_lru(pcm);
}

/*
 * Fastpath: try to allocate a pcache line from @pset.
 * If succeed, the line is initialized upon return.
 */
static inline struct pcache_meta *
pcache_alloc_fastpath(struct pcache_set *pset)
{
	int way;
	struct pcache_meta *pcm;

	pcache_for_each_way_set(pcm, pset, way) {
		if (likely(!TestSetPcacheAllocated(pcm))) {
			prep_new_pcache_meta(pcm);
			add_to_lru_list(pcm, pset);

			/*
			 * Make the pcache line visible to other
			 * pcache subsystems:
			 */
			set_pcache_usable(pcm);
			return pcm;
		}
	}
	return NULL;
}

/*
 * Slowpath: find line to evict and initalize the eviction process,
 * if eviction succeed, try fastpath again. We do have a time limit
 * on this function to avoid livelock.
 */
static struct pcache_meta *
pcache_alloc_slowpath(struct pcache_set *pset, unsigned long address)
{
	struct pcache_meta *pcm;
	int ret;
	unsigned long alloc_start = jiffies;

retry:
	ret = pcache_evict_line(pset, address);
	if (ret)
		return NULL;

	/* Do we still have time? */
	if (time_after(jiffies,
		       alloc_start + sysctl_pcache_alloc_timeout_sec * HZ)) {
		WARN(1, "Abort pcache alloc (%ums) pid:%u, addr:%#lx",
			jiffies_to_msecs(jiffies - alloc_start), current->pid, address);
		return NULL;
	}

	pcm = pcache_alloc_fastpath(pset);
	if (!pcm)
		goto retry;
	return pcm;
}

/**
 * pcache_alloc
 * @address: user virtual address
 *
 * This function will try to allocate a cacheline from the set that @address
 * belongs to. On success, the returned @pcm has PcaheAllocated set, refcount 1,
 * and mapcount 0.
 */
struct pcache_meta *pcache_alloc(unsigned long address)
{
	struct pcache_set *pset;
	struct pcache_meta *pcm;

	pset = user_vaddr_to_pcache_set(address);

	/* Fastpath: try to allocate one directly */
	pcm = pcache_alloc_fastpath(pset);
	if (likely(pcm))
		return pcm;

	/* Slowpath: fallback and try to evict one */
	return pcache_alloc_slowpath(pset, address);
}
