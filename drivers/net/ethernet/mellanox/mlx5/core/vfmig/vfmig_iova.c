// SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB
/* Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. */

#include <linux/err.h>
#include <linux/iommu.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "vfmig_iova.h"

/*
 * A per-VF unmanaged paging domain the PF driver fully owns, attached in
 * place of the VF's default DMA domain. @vf_pdev is pinned for the
 * domain's lifetime; @vf_id derives the IOVA window and labels log lines;
 * [@base, @end) is the per-VF IOVA window carved for a later allocator.
 */
struct vfmig_iova_domain {
	struct iommu_domain	*iommu_dom;
	struct pci_dev		*vf_pdev;	/* held via pci_dev_get() */
	u32			 vf_id;
	u64			 base;
	u64			 end;
};

int vfmig_iova_domain_create(struct pci_dev *vf_pdev, u32 vf_id,
			     struct vfmig_iova_domain **out)
{
	struct vfmig_iova_domain *dom;
	struct iommu_domain *idom;
	int err;

	if (!vf_pdev || !out)
		return -EINVAL;
	if ((u64)vf_id >= U32_MAX / 2)		/* defensive overflow guard */
		return -EINVAL;

	dom = kzalloc(sizeof(*dom), GFP_KERNEL);
	if (!dom)
		return -ENOMEM;

	dom->vf_id = vf_id;
	dom->base  = VFMIG_IOVA_BASE + (u64)vf_id * VFMIG_IOVA_PER_VF;
	dom->end   = dom->base + VFMIG_IOVA_PER_VF;
	if (dom->base < VFMIG_IOVA_BASE) {	/* wrapped */
		err = -ERANGE;
		goto err_free;
	}

	idom = iommu_paging_domain_alloc(&vf_pdev->dev);
	if (IS_ERR(idom)) {
		err = PTR_ERR(idom);
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: paging_domain_alloc failed: %d\n", err);
		goto err_free;
	}

	err = iommu_attach_device(idom, &vf_pdev->dev);
	if (err) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: attach failed: %d\n", err);
		goto err_free_idom;
	}

	/*
	 * The per-VF window must fit the IOMMU geometry aperture, whose
	 * upper bound the iommu driver derives from the hardware address
	 * width (e.g. 39-bit on some Intel VT-d). iommu_map() would
	 * -ERANGE outside it, so validate here and fail SET_TRACKED with a
	 * printed reason rather than a later opaque cmd-ring DMA failure.
	 */
	if (dom->base < idom->geometry.aperture_start ||
	    dom->end - 1 > idom->geometry.aperture_end) {
		dev_warn(&vf_pdev->dev,
			 "vfmig_iova: vf %u window [0x%llx, 0x%llx) does not fit IOMMU aperture [0x%llx, 0x%llx]\n",
			 vf_id, dom->base, dom->end,
			 idom->geometry.aperture_start,
			 idom->geometry.aperture_end);
		err = -EOPNOTSUPP;
		goto err_detach;
	}

	dom->iommu_dom = idom;
	dom->vf_pdev   = pci_dev_get(vf_pdev);

	dev_info(&vf_pdev->dev,
		 "vfmig_iova: vf %u domain attached, IOVA window [0x%llx, 0x%llx) within IOMMU aperture [0x%llx, 0x%llx]\n",
		 vf_id, dom->base, dom->end,
		 idom->geometry.aperture_start, idom->geometry.aperture_end);

	*out = dom;
	return 0;

err_detach:
	iommu_detach_device(idom, &vf_pdev->dev);
err_free_idom:
	iommu_domain_free(idom);
err_free:
	kfree(dom);
	return err;
}

void vfmig_iova_domain_destroy(struct vfmig_iova_domain *dom)
{
	if (!dom)
		return;

	iommu_detach_device(dom->iommu_dom, &dom->vf_pdev->dev);
	iommu_domain_free(dom->iommu_dom);
	pci_dev_put(dom->vf_pdev);
	kfree(dom);
}
