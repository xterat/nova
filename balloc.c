/*
 * NOVA persistent memory management
 *
 * Copyright 2015-2016 Regents of the University of California,
 * UCSD Non-Volatile Systems Lab, Andiry Xu <jix024@cs.ucsd.edu>
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/fs.h>
#include <linux/bitops.h>
#include "nova.h"

int nova_alloc_block_free_lists(struct super_block *sb)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct free_list *free_list;
	int i;

	sbi->free_lists = kzalloc(sbi->cpus * sizeof(struct free_list),
							GFP_KERNEL);

	if (!sbi->free_lists)
		return -ENOMEM;

	for (i = 0; i < sbi->cpus; i++) {
		free_list = nova_get_free_list(sb, i);
		free_list->block_free_tree = RB_ROOT;
		spin_lock_init(&free_list->s_lock);
		free_list->index = i;
	}

	return 0;
}

void nova_delete_free_lists(struct super_block *sb)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);

	/* Each tree is freed in save_blocknode_mappings */
	kfree(sbi->free_lists);
	sbi->free_lists = NULL;
}

static void nova_init_free_list(struct super_block *sb,
	struct free_list *free_list, int index)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	unsigned long per_list_blocks;

	per_list_blocks = sbi->num_blocks / sbi->cpus;

	free_list->block_start = per_list_blocks * index;
	free_list->block_end = free_list->block_start +
					per_list_blocks - 1;
	if (index == 0)
		free_list->block_start += sbi->reserved_blocks;

	nova_data_csum_init_free_list(sb, free_list);
	nova_data_parity_init_free_list(sb, free_list);
}

void nova_init_blockmap(struct super_block *sb, int recovery)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct rb_root *tree;
	struct nova_range_node *blknode;
	struct free_list *free_list;
	int i;
	int ret;

	/* Divide the block range among per-CPU free lists */
	sbi->per_list_blocks = sbi->num_blocks / sbi->cpus;
	for (i = 0; i < sbi->cpus; i++) {
		free_list = nova_get_free_list(sb, i);
		tree = &(free_list->block_free_tree);
		nova_init_free_list(sb, free_list, i);

		/* For recovery, update these fields later */
		if (recovery == 0) {
			free_list->num_free_blocks = free_list->block_end -
						free_list->block_start + 1;

			blknode = nova_alloc_blocknode(sb);
			if (blknode == NULL)
				NOVA_ASSERT(0);
			blknode->range_low = free_list->block_start;
			blknode->range_high = free_list->block_end;
			nova_update_range_node_checksum(blknode);
			ret = nova_insert_blocktree(sbi, tree, blknode);
			if (ret) {
				nova_err(sb, "%s failed\n", __func__);
				nova_free_blocknode(sb, blknode);
				return;
			}
			free_list->first_node = blknode;
			free_list->last_node = blknode;
			free_list->num_blocknode = 1;
		}

		nova_dbgv("%s: free list %d: block start %lu, end %lu, "
				"%lu free blocks\n", __func__, i,
				free_list->block_start,
				free_list->block_end,
				free_list->num_free_blocks);
	}
}

static inline int nova_rbtree_compare_rangenode(struct nova_range_node *curr,
	unsigned long range_low)
{
	if (range_low < curr->range_low)
		return -1;
	if (range_low > curr->range_high)
		return 1;

	return 0;
}

static int nova_find_range_node(struct nova_sb_info *sbi,
	struct rb_root *tree, unsigned long range_low,
	struct nova_range_node **ret_node)
{
	struct nova_range_node *curr = NULL;
	struct rb_node *temp;
	int compVal;
	int ret = 0;

	temp = tree->rb_node;

	while (temp) {
		curr = container_of(temp, struct nova_range_node, node);
		compVal = nova_rbtree_compare_rangenode(curr, range_low);

		if (compVal == -1) {
			temp = temp->rb_left;
		} else if (compVal == 1) {
			temp = temp->rb_right;
		} else {
			ret = 1;
			break;
		}
	}

	if (curr && !nova_range_node_checksum_ok(curr)) {
		nova_dbg("%s: curr failed\n", __func__);
		return 0;
	}

	*ret_node = curr;
	return ret;
}

inline int nova_search_inodetree(struct nova_sb_info *sbi,
	unsigned long ino, struct nova_range_node **ret_node)
{
	struct rb_root *tree;
	unsigned long internal_ino;
	int cpu;

	cpu = ino % sbi->cpus;
	tree = &sbi->inode_maps[cpu].inode_inuse_tree;
	internal_ino = ino / sbi->cpus;
	return nova_find_range_node(sbi, tree, internal_ino, ret_node);
}

static int nova_insert_range_node(struct rb_root *tree,
	struct nova_range_node *new_node)
{
	struct nova_range_node *curr;
	struct rb_node **temp, *parent;
	int compVal;

	temp = &(tree->rb_node);
	parent = NULL;

	while (*temp) {
		curr = container_of(*temp, struct nova_range_node, node);
		compVal = nova_rbtree_compare_rangenode(curr,
					new_node->range_low);
		parent = *temp;

		if (compVal == -1) {
			temp = &((*temp)->rb_left);
		} else if (compVal == 1) {
			temp = &((*temp)->rb_right);
		} else {
			nova_dbg("%s: entry %lu - %lu already exists: "
				"%lu - %lu\n", __func__,
				new_node->range_low,
				new_node->range_high,
				curr->range_low,
				curr->range_high);
			return -EINVAL;
		}
	}

	rb_link_node(&new_node->node, parent, temp);
	rb_insert_color(&new_node->node, tree);

	return 0;
}

inline int nova_insert_blocktree(struct nova_sb_info *sbi,
	struct rb_root *tree, struct nova_range_node *new_node)
{
	int ret;

	ret = nova_insert_range_node(tree, new_node);
	if (ret)
		nova_dbg("ERROR: %s failed %d\n", __func__, ret);

	return ret;
}

inline int nova_insert_inodetree(struct nova_sb_info *sbi,
	struct nova_range_node *new_node, int cpu)
{
	struct rb_root *tree;
	int ret;

	tree = &sbi->inode_maps[cpu].inode_inuse_tree;
	ret = nova_insert_range_node(tree, new_node);
	if (ret)
		nova_dbg("ERROR: %s failed %d\n", __func__, ret);

	return ret;
}

/* Used for both block free tree and inode inuse tree */
int nova_find_free_slot(struct nova_sb_info *sbi,
	struct rb_root *tree, unsigned long range_low,
	unsigned long range_high, struct nova_range_node **prev,
	struct nova_range_node **next)
{
	struct nova_range_node *ret_node = NULL;
	struct rb_node *temp;
	int check_prev = 0, check_next = 0;
	int ret;

	ret = nova_find_range_node(sbi, tree, range_low, &ret_node);
	if (ret) {
		nova_dbg("%s ERROR: %lu - %lu already in free list\n",
			__func__, range_low, range_high);
		return -EINVAL;
	}

	if (!ret_node) {
		*prev = *next = NULL;
	} else if (ret_node->range_high < range_low) {
		*prev = ret_node;
		temp = rb_next(&ret_node->node);
		if (temp) {
			*next = container_of(temp, struct nova_range_node, node);
			check_next = 1;
		} else {
			*next = NULL;
		}
	} else if (ret_node->range_low > range_high) {
		*next = ret_node;
		temp = rb_prev(&ret_node->node);
		if (temp) {
			*prev = container_of(temp, struct nova_range_node, node);
			check_prev = 1;
		} else {
			*prev = NULL;
		}
	} else {
		nova_dbg("%s ERROR: %lu - %lu overlaps with existing node "
			"%lu - %lu\n", __func__, range_low,
			range_high, ret_node->range_low,
			ret_node->range_high);
		return -EINVAL;
	}

	if (check_prev && !nova_range_node_checksum_ok(*prev)) {
		nova_dbg("%s: prev failed\n", __func__);
		return -EIO;
	}

	if (check_next && !nova_range_node_checksum_ok(*next)) {
		nova_dbg("%s: next failed\n", __func__);
		return -EIO;
	}

	return 0;
}

static int nova_free_blocks(struct super_block *sb, unsigned long blocknr,
	int num, unsigned short btype, int log_page)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct rb_root *tree;
	unsigned long block_low;
	unsigned long block_high;
	unsigned long num_blocks = 0;
	struct nova_range_node *prev = NULL;
	struct nova_range_node *next = NULL;
	struct nova_range_node *curr_node;
	struct free_list *free_list;
	int cpuid;
	int new_node_used = 0;
	int ret;
	timing_t free_time;

	if (num <= 0) {
		nova_dbg("%s ERROR: free %d\n", __func__, num);
		return -EINVAL;
	}

	NOVA_START_TIMING(free_blocks_t, free_time);
	cpuid = blocknr / sbi->per_list_blocks;

	/* Pre-allocate blocknode */
	curr_node = nova_alloc_blocknode(sb);
	if (curr_node == NULL) {
		/* returning without freeing the block*/
		NOVA_END_TIMING(free_blocks_t, free_time);
		return -ENOMEM;
	}

	free_list = nova_get_free_list(sb, cpuid);
	spin_lock(&free_list->s_lock);

	tree = &(free_list->block_free_tree);

	num_blocks = nova_get_numblocks(btype) * num;
	block_low = blocknr;
	block_high = blocknr + num_blocks - 1;

	nova_dbgv("Free: %lu - %lu\n", block_low, block_high);

	ret = nova_find_free_slot(sbi, tree, block_low,
					block_high, &prev, &next);

	if (ret) {
		nova_dbg("%s: find free slot fail: %d\n", __func__, ret);
		goto out;
	}

	if (prev && next && (block_low == prev->range_high + 1) &&
			(block_high + 1 == next->range_low)) {
		/* fits the hole */
		rb_erase(&next->node, tree);
		free_list->num_blocknode--;
		prev->range_high = next->range_high;
		nova_update_range_node_checksum(prev);
		if (free_list->last_node == next)
			free_list->last_node = prev;
		nova_free_blocknode(sb, next);
		goto block_found;
	}
	if (prev && (block_low == prev->range_high + 1)) {
		/* Aligns left */
		prev->range_high += num_blocks;
		nova_update_range_node_checksum(prev);
		goto block_found;
	}
	if (next && (block_high + 1 == next->range_low)) {
		/* Aligns right */
		next->range_low -= num_blocks;
		nova_update_range_node_checksum(next);
		goto block_found;
	}

	/* Aligns somewhere in the middle */
	curr_node->range_low = block_low;
	curr_node->range_high = block_high;
	nova_update_range_node_checksum(curr_node);
	new_node_used = 1;
	ret = nova_insert_blocktree(sbi, tree, curr_node);
	if (ret) {
		new_node_used = 0;
		goto out;
	}
	if (!prev)
		free_list->first_node = curr_node;
	if (!next)
		free_list->last_node = curr_node;

	free_list->num_blocknode++;

block_found:
	free_list->num_free_blocks += num_blocks;

	if (log_page) {
		free_list->free_log_count++;
		free_list->freed_log_pages += num_blocks;
	} else {
		free_list->free_data_count++;
		free_list->freed_data_pages += num_blocks;
	}

out:
	spin_unlock(&free_list->s_lock);
	if (new_node_used == 0)
		nova_free_blocknode(sb, curr_node);

	NOVA_END_TIMING(free_blocks_t, free_time);
	return ret;
}

int nova_free_data_blocks(struct super_block *sb,
	struct nova_inode_info_header *sih, unsigned long blocknr, int num)
{
	int ret;
	timing_t free_time;

	nova_dbgv("Inode %lu: free %d data block from %lu to %lu\n",
			sih->ino, num, blocknr, blocknr + num - 1);
	if (blocknr == 0) {
		nova_dbg("%s: ERROR: %lu, %d\n", __func__, blocknr, num);
		return -EINVAL;
	}
	NOVA_START_TIMING(free_data_t, free_time);
	ret = nova_free_blocks(sb, blocknr, num, sih->i_blk_type, 0);
	if (ret) {
		nova_err(sb, "Inode %lu: free %d data block from %lu to %lu "
				"failed!\n", sih->ino, num, blocknr,
				blocknr + num - 1);
		nova_print_nova_log(sb, sih);
	}
	NOVA_END_TIMING(free_data_t, free_time);

	return ret;
}

int nova_free_log_blocks(struct super_block *sb,
	struct nova_inode_info_header *sih, unsigned long blocknr, int num)
{
	int ret;
	timing_t free_time;

	nova_dbgv("Inode %lu: free %d log block from %lu to %lu\n",
			sih->ino, num, blocknr, blocknr + num - 1);
	if (blocknr == 0) {
		nova_dbg("%s: ERROR: %lu, %d\n", __func__, blocknr, num);
		return -EINVAL;
	}
	NOVA_START_TIMING(free_log_t, free_time);
	ret = nova_free_blocks(sb, blocknr, num, sih->i_blk_type, 1);
	if (ret) {
		nova_err(sb, "Inode %lu: free %d log block from %lu to %lu "
				"failed!\n", sih->ino, num, blocknr,
				blocknr + num - 1);
		nova_print_nova_log(sb, sih);
	}
	NOVA_END_TIMING(free_log_t, free_time);

	return ret;
}

static long nova_alloc_blocks_in_free_list(struct super_block *sb,
	struct free_list *free_list, unsigned short btype,
	unsigned long num_blocks, unsigned long *new_blocknr,
	int from_tail)
{
	struct rb_root *tree;
	struct nova_range_node *curr, *next = NULL, *prev = NULL;
	struct rb_node *temp, *next_node, *prev_node;
	unsigned long curr_blocks;
	bool found = 0;
	unsigned long step = 0;

	if (!free_list->first_node || free_list->num_free_blocks == 0)
		return -ENOSPC;

	tree = &(free_list->block_free_tree);
	if (from_tail == 0)
		temp = &(free_list->first_node->node);
	else
		temp = &(free_list->last_node->node);

	while (temp) {
		step++;
		curr = container_of(temp, struct nova_range_node, node);

		if (!nova_range_node_checksum_ok(curr)) {
			nova_err(sb, "%s curr failed\n", __func__);
			goto next;
		}

		curr_blocks = curr->range_high - curr->range_low + 1;

		if (num_blocks >= curr_blocks) {
			/* Superpage allocation must succeed */
			if (btype > 0 && num_blocks > curr_blocks)
				goto next;

			/* Otherwise, allocate the whole blocknode */
			if (curr == free_list->first_node) {
				next_node = rb_next(temp);
				if (next_node)
					next = container_of(next_node,
						struct nova_range_node, node);
				free_list->first_node = next;
			}

			if (curr == free_list->last_node) {
				prev_node = rb_prev(temp);
				if (prev_node)
					prev = container_of(prev_node,
						struct nova_range_node, node);
				free_list->last_node = prev;
			}

			rb_erase(&curr->node, tree);
			free_list->num_blocknode--;
			num_blocks = curr_blocks;
			*new_blocknr = curr->range_low;
			nova_free_blocknode(sb, curr);
			found = 1;
			break;
		}

		/* Allocate partial blocknode */
		if (from_tail == 0) {
			*new_blocknr = curr->range_low;
			curr->range_low += num_blocks;
		} else {
			*new_blocknr = curr->range_high + 1 - num_blocks;
			curr->range_high -= num_blocks;
		}

		nova_update_range_node_checksum(curr);
		found = 1;
		break;
next:
		if (from_tail == 0)
			temp = rb_next(temp);
		else
			temp = rb_prev(temp);
	}

	if (free_list->num_free_blocks < num_blocks) {
		nova_dbg("%s: free list %d has %lu free blocks, "
				"but allocated %lu blocks?\n",
				__func__, free_list->index,
				free_list->num_free_blocks, num_blocks);
		return -ENOSPC;
	}

	if (found == 1)
		free_list->num_free_blocks -= num_blocks;
	else
		return -ENOSPC;

	NOVA_STATS_ADD(alloc_steps, step);

	return num_blocks;
}

/* Find out the free list with most free blocks */
static int nova_get_candidate_free_list(struct super_block *sb)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct free_list *free_list;
	int cpuid = 0;
	int num_free_blocks = 0;
	int i;

	for (i = 0; i < sbi->cpus; i++) {
		free_list = nova_get_free_list(sb, i);
		if (free_list->num_free_blocks > num_free_blocks) {
			cpuid = i;
			num_free_blocks = free_list->num_free_blocks;
		}
	}

	return cpuid;
}

/* Return how many blocks allocated */
static int nova_new_blocks(struct super_block *sb, unsigned long *blocknr,
	unsigned int num, unsigned short btype, int zero,
	enum alloc_type atype, int cpuid, int from_tail)
{
	struct free_list *free_list;
	void *bp;
	unsigned long num_blocks = 0;
	unsigned long new_blocknr = 0;
	long ret_blocks = 0;
	struct rb_node *temp;
	struct nova_range_node *first;
	int retried = 0;
	timing_t alloc_time;

	num_blocks = num * nova_get_numblocks(btype);
	if (num_blocks == 0)
		return -EINVAL;

	NOVA_START_TIMING(new_blocks_t, alloc_time);
	if (cpuid == ANY_CPU)
		cpuid = smp_processor_id();

retry:
	free_list = nova_get_free_list(sb, cpuid);
	spin_lock(&free_list->s_lock);

	if (free_list->num_free_blocks < num_blocks || !free_list->first_node) {
		nova_dbgv("%s: cpu %d, free_blocks %lu, required %lu, "
			"blocknode %lu\n", __func__, cpuid,
			free_list->num_free_blocks, num_blocks,
			free_list->num_blocknode);
		if (free_list->num_free_blocks >= num_blocks) {
			nova_dbg("free list %d: first node is NULL "
				"but still has %lu free blocks\n",
				free_list->index,
				free_list->num_free_blocks);
			temp = rb_first(&free_list->block_free_tree);
			first = container_of(temp, struct nova_range_node, node);
			free_list->first_node = first;
		} else {
			if (retried >= 2)
				/* Allocate anyway */
				goto alloc;

			spin_unlock(&free_list->s_lock);
			cpuid = nova_get_candidate_free_list(sb);
			retried++;
			goto retry;
		}
	}
alloc:
	ret_blocks = nova_alloc_blocks_in_free_list(sb, free_list, btype,
					num_blocks, &new_blocknr, from_tail);

	if (ret_blocks > 0) {
		if (atype == LOG) {
			free_list->alloc_log_count++;
			free_list->alloc_log_pages += ret_blocks;
		} else if (atype == DATA) {
			free_list->alloc_data_count++;
			free_list->alloc_data_pages += ret_blocks;
		}
	}

	spin_unlock(&free_list->s_lock);
	NOVA_END_TIMING(new_blocks_t, alloc_time);

	if (ret_blocks <= 0 || new_blocknr == 0)
		return -ENOSPC;

	if (zero) {
		bp = nova_get_block(sb, nova_get_block_off(sb,
						new_blocknr, btype));
		nova_memunlock_block(sb, bp);
		memset_nt(bp, 0, PAGE_SIZE * ret_blocks);
		nova_memlock_block(sb, bp);
	}
	*blocknr = new_blocknr;

	nova_dbg_verbose("Alloc %lu NVMM blocks 0x%lx\n", ret_blocks, *blocknr);
	return ret_blocks / nova_get_numblocks(btype);
}

inline int nova_new_data_blocks(struct super_block *sb,
	struct nova_inode_info_header *sih, unsigned long *blocknr,
	unsigned long start_blk, unsigned int num,
	int zero, int cpu, int from_tail)
{
	int allocated;
	timing_t alloc_time;
	NOVA_START_TIMING(new_data_blocks_t, alloc_time);
	allocated = nova_new_blocks(sb, blocknr, num,
				sih->i_blk_type, zero, DATA, cpu, from_tail);
	NOVA_END_TIMING(new_data_blocks_t, alloc_time);
	nova_dbgv("Inode %lu, start blk %lu, "
			"alloc %d data blocks from %lu to %lu\n",
			sih->ino, start_blk, allocated, *blocknr,
			*blocknr + allocated - 1);
	return allocated;
}

inline int nova_new_log_blocks(struct super_block *sb,
	struct nova_inode_info_header *sih,
	unsigned long *blocknr, unsigned int num,
	int zero, int cpu, int from_tail)
{
	int allocated;
	timing_t alloc_time;
	NOVA_START_TIMING(new_log_blocks_t, alloc_time);
	allocated = nova_new_blocks(sb, blocknr, num,
				sih->i_blk_type, zero, LOG, cpu, from_tail);
	NOVA_END_TIMING(new_log_blocks_t, alloc_time);
	nova_dbgv("Inode %lu, alloc %d log blocks from %lu to %lu\n",
			sih->ino, allocated, *blocknr,
			*blocknr + allocated - 1);
	return allocated;
}

unsigned long nova_count_free_blocks(struct super_block *sb)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct free_list *free_list;
	unsigned long num_free_blocks = 0;
	int i;

	for (i = 0; i < sbi->cpus; i++) {
		free_list = nova_get_free_list(sb, i);
		num_free_blocks += free_list->num_free_blocks;
	}

	return num_free_blocks;
}


