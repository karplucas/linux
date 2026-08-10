/*
 * Copyright (c) 2013-2015, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>
#include <rdma/ib_umem.h>

#include "mlx5_ib.h"

struct mlx5_ib_user_db_page {
	struct list_head	list;
	struct ib_umem	       *umem;
	unsigned long		user_virt;
	int			refcnt;
	struct mm_struct	*mm;
};

int mlx5_ib_db_map_user(struct mlx5_ib_ucontext *context, unsigned long virt,
			struct mlx5_db *db)
{
	struct mlx5_ib_dev *dev = to_mdev(context->ibucontext.device);
	struct mlx5_ib_user_db_page *page;
	int err = 0;

	mutex_lock(&context->db_page_mutex);

	list_for_each_entry(page, &context->db_page_list, list)
		if ((current->mm == page->mm) &&
		    (page->user_virt == (virt & PAGE_MASK)))
			goto found;

	page = kmalloc(sizeof(*page), GFP_KERNEL);
	if (!page) {
		err = -ENOMEM;
		goto out;
	}

	page->user_virt = (virt & PAGE_MASK);
	page->refcnt    = 0;
	page->umem = ib_umem_get(context->ibucontext.device, virt & PAGE_MASK,
				 PAGE_SIZE, 0);
	if (IS_ERR(page->umem)) {
		err = PTR_ERR(page->umem);
		kfree(page);
		goto out;
	}
	mmgrab(current->mm);
	page->mm = current->mm;

	list_add(&page->list, &context->db_page_list);

	/*
	 * Source-side vfmig retag (mirrors create_real_mr). Only the miss
	 * branch retags: the ib_umem_get above is the sole path that hands a
	 * fresh PAGE_SIZE umem to vfmig_dma_ops.map_sg, planting the
	 * KIND_NONE registry entry; the 'found' branch just refcounts an
	 * already-retagged page. The key's fw_id is the page-aligned user VA
	 * (mlx5_ib_db_map_user's own dedup key), not a FW id -- doorbell
	 * pages have no FW identity. Non-fatal on error: the CQ/QP/SRQ
	 * create that triggered this mapping continues; the doorbell page is
	 * usable, just not CRIU-restorable.
	 */
	if (dev->mdev->cmd.vfmig_iova_dom) {
		struct sg_table *sgt = &page->umem->sgt_append.sgt;
		dma_addr_t iova_base = sg_dma_address(sgt->sgl) & PAGE_MASK;
		int retag_err;

		retag_err = mlx5_vfmig_retag_user_dbr(dev->mdev,
						      page->user_virt,
						      iova_base, PAGE_SIZE);
		if (retag_err)
			mlx5_ib_warn(dev,
				     "vfmig: source-side retag for DBR page failed: user_virt=0x%lx iova_base=0x%llx err=%d -- DBR page usable but not CRIU-restorable\n",
				     page->user_virt, (u64)iova_base, retag_err);
	}

found:
	db->dma = sg_dma_address(page->umem->sgt_append.sgt.sgl) +
		  (virt & ~PAGE_MASK);
	db->u.user_page = page;
	++page->refcnt;

out:
	mutex_unlock(&context->db_page_mutex);

	return err;
}

void mlx5_ib_db_unmap_user(struct mlx5_ib_ucontext *context, struct mlx5_db *db)
{
	mutex_lock(&context->db_page_mutex);

	if (!--db->u.user_page->refcnt) {
		list_del(&db->u.user_page->list);
		mmdrop(db->u.user_page->mm);
		ib_umem_release(db->u.user_page->umem);
		kfree(db->u.user_page);
	}

	mutex_unlock(&context->db_page_mutex);
}
