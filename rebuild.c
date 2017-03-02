/*
 * BRIEF DESCRIPTION
 *
 * Inode rebuild methods.
 *
 * Copyright 2015-2016 Regents of the University of California,
 * UCSD Non-Volatile Systems Lab, Andiry Xu <jix024@cs.ucsd.edu>
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include "nova.h"

static void nova_apply_setattr_entry(struct super_block *sb,
	struct nova_inode_rebuild *reb,	struct nova_inode_info_header *sih,
	struct nova_setattr_logentry *entry)
{
	unsigned int data_bits = blk_type_to_shift[sih->i_blk_type];
	unsigned long first_blocknr, last_blocknr;
	loff_t start, end;
	int freed = 0;

	if (entry->entry_type != SET_ATTR)
		BUG();

	reb->i_mode	= entry->mode;
	reb->i_uid	= entry->uid;
	reb->i_gid	= entry->gid;
	reb->i_atime	= entry->atime;

	if (S_ISREG(reb->i_mode)) {
		start = entry->size;
		end = reb->i_size;

		first_blocknr = (start + (1UL << data_bits) - 1) >> data_bits;

		if (end > 0)
			last_blocknr = (end - 1) >> data_bits;
		else
			last_blocknr = 0;

		freed = nova_delete_file_tree(sb, sih, first_blocknr,
					last_blocknr, false, false, false, 0);
	}
}

static void nova_apply_link_change_entry(struct super_block *sb,
	struct nova_inode_rebuild *reb,	struct nova_link_change_entry *entry)
{
	if (entry->entry_type != LINK_CHANGE)
		BUG();

	reb->i_links_count	= entry->links;
	reb->i_ctime		= entry->ctime;
	reb->i_flags		= entry->flags;
	reb->i_generation	= entry->generation;

	/* Do not flush now */
}

static void nova_update_inode_with_rebuild(struct super_block *sb,
	struct nova_inode_rebuild *reb, struct nova_inode *pi)
{
	pi->i_size = cpu_to_le64(reb->i_size);
	pi->i_flags = cpu_to_le32(reb->i_flags);
	pi->i_uid = cpu_to_le32(reb->i_uid);
	pi->i_gid = cpu_to_le32(reb->i_gid);
	pi->i_atime = cpu_to_le32(reb->i_atime);
	pi->i_ctime = cpu_to_le32(reb->i_ctime);
	pi->i_mtime = cpu_to_le32(reb->i_mtime);
	pi->i_generation = cpu_to_le32(reb->i_generation);
	pi->i_links_count = cpu_to_le16(reb->i_links_count);
	pi->i_mode = cpu_to_le16(reb->i_mode);
}

static int nova_init_inode_rebuild(struct super_block *sb,
	struct nova_inode_rebuild *reb, struct nova_inode *pi)
{
	struct nova_inode fake_pi;
	int rc;

	rc = memcpy_from_pmem(&fake_pi, pi, sizeof(struct nova_inode));
	if (rc)
		return rc;

	reb->i_size = le64_to_cpu(fake_pi.i_size);
	reb->i_flags = le32_to_cpu(fake_pi.i_flags);
	reb->i_uid = le32_to_cpu(fake_pi.i_uid);
	reb->i_gid = le32_to_cpu(fake_pi.i_gid);
	reb->i_atime = le32_to_cpu(fake_pi.i_atime);
	reb->i_ctime = le32_to_cpu(fake_pi.i_ctime);
	reb->i_mtime = le32_to_cpu(fake_pi.i_mtime);
	reb->i_generation = le32_to_cpu(fake_pi.i_generation);
	reb->i_links_count = le16_to_cpu(fake_pi.i_links_count);
	reb->i_mode = le16_to_cpu(fake_pi.i_mode);

	return rc;
}

static inline void nova_rebuild_file_time_and_size(struct super_block *sb,
	struct nova_inode_rebuild *reb, u32 mtime, u32 ctime, u64 size)
{
	reb->i_mtime = cpu_to_le32(mtime);
	reb->i_ctime = cpu_to_le32(ctime);
	reb->i_size = cpu_to_le64(size);
}

static int nova_rebuild_inode_start(struct super_block *sb,
	struct nova_inode *pi, struct nova_inode_info_header *sih,
	struct nova_inode_rebuild *reb, u64 pi_addr)
{
	int ret;

	ret = nova_get_head_tail(sb, pi, sih);
	if (ret)
		return ret;

	ret = nova_init_inode_rebuild(sb, reb, pi);
	if (ret)
		return ret;

	sih->pi_addr = pi_addr;

	nova_dbg_verbose("Log head 0x%llx, tail 0x%llx\n",
				sih->log_head, sih->log_tail);
	sih->log_pages = 1;

	return ret;
}

static int nova_rebuild_inode_finish(struct super_block *sb,
	struct nova_inode *pi, struct nova_inode_info_header *sih,
	struct nova_inode_rebuild *reb, u64 curr_p)
{
	struct nova_inode *alter_pi;
	u64 next;

	sih->i_size = le64_to_cpu(reb->i_size);
	sih->i_mode = le64_to_cpu(reb->i_mode);

	nova_memunlock_inode(sb, pi);
	nova_update_inode_with_rebuild(sb, reb, pi);
	nova_update_inode_checksum(pi);
	if (replica_metadata) {
		alter_pi = (struct nova_inode *)nova_get_block(sb,
							sih->alter_pi_addr);
		memcpy_to_pmem_nocache(alter_pi, pi, sizeof(struct nova_inode));
	}
	nova_memlock_inode(sb, pi);

	/* Keep traversing until log ends */
	curr_p &= PAGE_MASK;
	while ((next = next_log_page(sb, curr_p)) > 0) {
		sih->log_pages++;
		curr_p = next;
	}

	if (replica_metadata)
		sih->log_pages *= 2;

	return 0;
}

static int nova_reset_csum_parity_page(struct super_block *sb,
	struct nova_inode_info_header *sih, struct nova_file_write_entry *entry,
	unsigned long pgoff)
{
	if (data_csum)
		nova_update_block_csum(sb, sih, entry, pgoff);

	if (data_parity)
		nova_update_pgoff_parity(sb, sih, entry, pgoff);

	return 0;
}

static int nova_reset_csum_parity_range(struct super_block *sb,
	struct nova_inode_info_header *sih, struct nova_file_write_entry *entry,
	unsigned long start_pgoff, unsigned long end_pgoff)
{
	struct nova_file_write_entry *curr;
	unsigned long pgoff;

	for (pgoff = start_pgoff; pgoff < end_pgoff; pgoff++) {
		if (entry) {
			curr = nova_get_write_entry(sb, sih, pgoff);
			if (curr != entry)
				continue;
		}

		/* FIXME: For mmap, check dirty? */
		nova_reset_csum_parity_page(sb, sih, entry, pgoff);
	}

	return 0;
}

/* Reset data csum for updating entries */
static int nova_reset_data_csum_parity(struct super_block *sb,
	struct nova_inode_info_header *sih, struct nova_file_write_entry *entry)
{
	struct nova_file_write_entry fake_entry;
	size_t size = sizeof(struct nova_file_write_entry);
	unsigned long end_pgoff;
	int ret = 0;

	if (data_csum == 0 && data_parity == 0)
		goto out;

	ret = memcpy_from_pmem(&fake_entry, entry, size);
	if (ret < 0)
		return ret;

	if (fake_entry.invalid_pages == fake_entry.num_pages) {
		/* Dead entry */
		goto out;
	}

	end_pgoff = fake_entry.pgoff + fake_entry.num_pages;
	nova_reset_csum_parity_range(sb, sih, entry, fake_entry.pgoff,
			end_pgoff);

out:
	if (ret == 0)
		nova_set_write_entry_updating(sb, entry, 0);

	return ret;
}

/* Reset data csum for mmap entries */
static int nova_reset_mmap_csum_parity(struct super_block *sb,
	struct nova_inode_info_header *sih, struct nova_mmap_entry *entry)
{
	struct nova_mmap_entry fake_entry;
	size_t size = sizeof(struct nova_mmap_entry);
	unsigned long end_pgoff;
	int ret = 0;

	if (data_csum == 0 && data_parity == 0)
		goto out;

	ret = memcpy_from_pmem(&fake_entry, entry, size);
	if (ret < 0)
		return ret;

	if (fake_entry.invalid == 1) {
		/* Dead entry */
		goto out;
	}

	end_pgoff = fake_entry.pgoff + fake_entry.num_pages;
	nova_reset_csum_parity_range(sb, sih, NULL, fake_entry.pgoff,
			end_pgoff);

out:
	if (ret == 0)
		ret = nova_invalidate_logentry(sb, entry, MMAP_WRITE, 0);

	return ret;
}

int nova_reset_vma_csum_parity(struct super_block *sb,
	struct vm_area_struct *vma)
{
	struct address_space *mapping = vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	unsigned long num_pages;

	if (data_csum == 0 && data_parity == 0)
		return 0;

	num_pages = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;

	nova_reset_csum_parity_range(sb, sih, NULL, vma->vm_pgoff,
			vma->vm_pgoff + num_pages);

	return 0;
}


static void nova_rebuild_handle_write_entry(struct super_block *sb,
	struct nova_inode_info_header *sih, struct nova_inode_rebuild *reb,
	struct nova_file_write_entry *entry, u64 *curr_trans_id)
{
	if (entry->num_pages != entry->invalid_pages) {
		/*
		 * The overlaped blocks are already freed.
		 * Don't double free them, just re-assign the pointers.
		 */
		nova_assign_write_entry(sb, sih, entry, false);
	}

	if (entry->trans_id >= *curr_trans_id) {
		nova_rebuild_file_time_and_size(sb, reb,
					entry->mtime, entry->mtime,
					entry->size);
		*curr_trans_id = entry->trans_id;
	}

	if (entry->updating) {
		nova_reset_data_csum_parity(sb, sih, entry);
	}

	/* Update sih->i_size for setattr apply operations */
	sih->i_size = le64_to_cpu(reb->i_size);
}

int nova_rebuild_file_inode_tree(struct super_block *sb,
	struct nova_inode *pi, u64 pi_addr,
	struct nova_inode_info_header *sih)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct nova_file_write_entry *entry = NULL;
	struct nova_setattr_logentry *attr_entry = NULL;
	struct nova_link_change_entry *link_change_entry = NULL;
	struct nova_mmap_entry *mmap_entry = NULL;
	struct nova_inode_rebuild rebuild, *reb;
	unsigned int data_bits = blk_type_to_shift[sih->i_blk_type];
	u64 ino = pi->nova_ino;
	u64 curr_trans_id = 0;
	timing_t rebuild_time;
	void *addr;
	u64 curr_p;
	u8 type;
	int ret;

	NOVA_START_TIMING(rebuild_file_t, rebuild_time);
	nova_dbg_verbose("Rebuild file inode %llu tree\n", ino);

	reb = &rebuild;
	ret = nova_rebuild_inode_start(sb, pi, sih, reb, pi_addr);
	if (ret)
		goto out;

	curr_p = sih->log_head;
	if (curr_p == 0 && sih->log_tail == 0)
		goto out;

	while (curr_p != sih->log_tail) {
		if (goto_next_page(sb, curr_p)) {
			sih->log_pages++;
			curr_p = next_log_page(sb, curr_p);
		}

		if (curr_p == 0) {
			nova_err(sb, "File inode %llu log is NULL!\n", ino);
			BUG();
		}

		addr = (void *)nova_get_block(sb, curr_p);
		if (!nova_verify_entry_csum(sb, addr)) {
			nova_err(sb, "%s: entry checksum fail "
					"inode %llu entry addr 0x%llx\n",
					__func__, ino, (u64)addr);
			break;
		}

		type = nova_get_entry_type(addr);

		if (sbi->mount_snapshot) {
			if (nova_encounter_mount_snapshot(sb, addr, type))
				break;
		}

		switch (type) {
			case SET_ATTR:
				attr_entry =
					(struct nova_setattr_logentry *)addr;
				nova_apply_setattr_entry(sb, reb, sih,
								attr_entry);
				sih->last_setattr = curr_p;
				if (attr_entry->trans_id >= curr_trans_id) {
					nova_rebuild_file_time_and_size(sb, reb,
							attr_entry->mtime,
							attr_entry->ctime,
							attr_entry->size);
					curr_trans_id = attr_entry->trans_id;
				}

				/* Update sih->i_size for setattr operation */
				sih->i_size = le64_to_cpu(reb->i_size);
				curr_p += sizeof(struct nova_setattr_logentry);
				break;
			case LINK_CHANGE:
				link_change_entry =
					(struct nova_link_change_entry *)addr;
				nova_apply_link_change_entry(sb, reb,
							link_change_entry);
				sih->last_link_change = curr_p;
				curr_p += sizeof(struct nova_link_change_entry);
				break;
			case FILE_WRITE:
				entry = (struct nova_file_write_entry *)addr;
				nova_rebuild_handle_write_entry(sb, sih, reb,
						entry, &curr_trans_id);
				curr_p += sizeof(struct nova_file_write_entry);
				break;
			case MMAP_WRITE:
				mmap_entry = (struct nova_mmap_entry *)addr;
				nova_reset_mmap_csum_parity(sb, sih,
						mmap_entry);
				curr_p += sizeof(struct nova_mmap_entry);
				break;
			default:
				nova_err(sb, "unknown type %d, 0x%llx\n",
							type, curr_p);
				NOVA_ASSERT(0);
				curr_p += sizeof(struct nova_file_write_entry);
				break;
		}

	}

	ret = nova_rebuild_inode_finish(sb, pi, sih, reb, curr_p);
	sih->i_blocks = sih->log_pages + (sih->i_size >> data_bits);

out:
//	nova_print_inode_log_page(sb, inode);
	NOVA_END_TIMING(rebuild_file_t, rebuild_time);
	return ret;
}

/******************* Directory rebuild *********************/

static inline void nova_rebuild_dir_time_and_size(struct super_block *sb,
	struct nova_inode_rebuild *reb, struct nova_dentry *entry)
{
	if (!entry || !reb)
		return;

	reb->i_ctime = entry->mtime;
	reb->i_mtime = entry->mtime;
	reb->i_links_count = entry->links_count;
	reb->i_size = entry->size;
}

static void nova_reassign_last_dentry(struct super_block *sb,
	struct nova_inode_info_header *sih, u64 curr_p)
{
	struct nova_dentry *dentry, *old_dentry;
	if (sih->last_dentry == 0) {
		sih->last_dentry = curr_p;
	} else {
		old_dentry = (struct nova_dentry *)nova_get_block(sb,
							sih->last_dentry);
		dentry = (struct nova_dentry *)nova_get_block(sb, curr_p);
		if (dentry->trans_id >= old_dentry->trans_id)
			sih->last_dentry = curr_p;
	}
}

static inline int nova_replay_add_dentry(struct super_block *sb,
	struct nova_inode_info_header *sih, struct nova_dentry *entry)
{
	if (!entry->name_len)
		return -EINVAL;

	nova_dbg_verbose("%s: add %s\n", __func__, entry->name);
	return nova_insert_dir_radix_tree(sb, sih,
			entry->name, entry->name_len, entry);
}

static inline int nova_replay_remove_dentry(struct super_block *sb,
	struct nova_inode_info_header *sih,
	struct nova_dentry *entry)
{
	nova_dbg_verbose("%s: remove %s\n", __func__, entry->name);
	nova_remove_dir_radix_tree(sb, sih, entry->name,
					entry->name_len, 1, NULL);
	return 0;
}

static int nova_rebuild_handle_dentry(struct super_block *sb,
	struct nova_inode_info_header *sih, struct nova_inode_rebuild *reb,
	struct nova_dentry *entry, u64 curr_p, u64 *curr_trans_id)
{
	int ret = 0;

	nova_dbgv("curr_p: 0x%llx, type %d, ino %llu, "
			"name %s, namelen %u, csum 0x%x, rec len %u\n", curr_p,
			entry->entry_type, le64_to_cpu(entry->ino),
			entry->name, entry->name_len, entry->csum,
			le16_to_cpu(entry->de_len));

	nova_reassign_last_dentry(sb, sih, curr_p);

	if (entry->invalid == 0) {
		if (entry->ino > 0)
			ret = nova_replay_add_dentry(sb, sih, entry);
		else
			ret = nova_replay_remove_dentry(sb, sih, entry);
	}

	if (ret) {
		nova_err(sb, "%s ERROR %d\n", __func__, ret);
		return ret;
	}

	if (entry->trans_id >= *curr_trans_id) {
		nova_rebuild_dir_time_and_size(sb, reb, entry);
		*curr_trans_id = entry->trans_id;
	}

	return ret;
}

int nova_rebuild_dir_inode_tree(struct super_block *sb,
	struct nova_inode *pi, u64 pi_addr,
	struct nova_inode_info_header *sih)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct nova_dentry *entry = NULL;
	struct nova_setattr_logentry *attr_entry = NULL;
	struct nova_link_change_entry *lc_entry = NULL;
	struct nova_inode_rebuild rebuild, *reb;
	u64 ino = pi->nova_ino;
	unsigned short de_len;
	timing_t rebuild_time;
	void *addr;
	u64 curr_p;
	u64 curr_trans_id = 0;
	u8 type;
	int ret;

	NOVA_START_TIMING(rebuild_dir_t, rebuild_time);
	nova_dbgv("Rebuild dir %llu tree\n", ino);

	reb = &rebuild;
	ret = nova_rebuild_inode_start(sb, pi, sih, reb, pi_addr);
	if (ret)
		goto out;

	curr_p = sih->log_head;
	if (curr_p == 0) {
		nova_err(sb, "Dir %llu log is NULL!\n", ino);
		BUG();
		goto out;
	}

	while (curr_p != sih->log_tail) {
		if (goto_next_page(sb, curr_p)) {
			sih->log_pages++;
			curr_p = next_log_page(sb, curr_p);
		}

		if (curr_p == 0) {
			nova_err(sb, "Dir %llu log is NULL!\n", ino);
			BUG();
		}

		addr = (void *)nova_get_block(sb, curr_p);
		if (!nova_verify_entry_csum(sb, addr)) {
			nova_err(sb, "%s: entry checksum fail "
				"inode %llu entry addr 0x%llx\n",
				__func__, ino, (u64)addr);
			break;
		}

		type = nova_get_entry_type(addr);

		if (sbi->mount_snapshot) {
			if (nova_encounter_mount_snapshot(sb, addr, type))
				break;
		}

		switch (type) {
			case SET_ATTR:
				attr_entry =
					(struct nova_setattr_logentry *)addr;
				nova_apply_setattr_entry(sb, reb, sih,
								attr_entry);
				sih->last_setattr = curr_p;
				curr_p += sizeof(struct nova_setattr_logentry);
				break;
			case LINK_CHANGE:
				lc_entry =
					(struct nova_link_change_entry *)addr;
				if (lc_entry->trans_id >= curr_trans_id) {
					nova_apply_link_change_entry(sb, reb,
								lc_entry);
					curr_trans_id = lc_entry->trans_id;
				}
				sih->last_link_change = curr_p;
				curr_p += sizeof(struct nova_link_change_entry);
				break;
			case DIR_LOG:
				entry = (struct nova_dentry *)addr;
				ret = nova_rebuild_handle_dentry(sb, sih, reb,
						entry, curr_p, &curr_trans_id);
				if (ret)
					goto out;
				de_len = le16_to_cpu(entry->de_len);
				curr_p += de_len;
				break;
			default:
				nova_dbg("%s: unknown type %d, 0x%llx\n",
							__func__, type, curr_p);
				NOVA_ASSERT(0);
				break;
		}
	}

	ret = nova_rebuild_inode_finish(sb, pi, sih, reb, curr_p);
	sih->i_blocks = sih->log_pages;

out:
//	nova_print_dir_tree(sb, sih, ino);
	NOVA_END_TIMING(rebuild_dir_t, rebuild_time);
	return ret;
}

