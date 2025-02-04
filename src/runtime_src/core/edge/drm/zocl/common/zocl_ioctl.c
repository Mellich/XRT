/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * A GEM style (optionally CMA backed) device manager for ZynQ based
 * OpenCL accelerators.
 *
 * Copyright (C) 2016-2022 Xilinx, Inc. All rights reserved.
 *
 * Authors:
 *    Sonal Santan <sonal.santan@xilinx.com>
 *    Jan Stephan  <j.stephan@hzdr.de>
 *
 * This file is dual-licensed; you may select either the GNU General Public
 * License version 2 or Apache License, Version 2.0.
 */

#include <drm/drm_file.h>
#include "zocl_drv.h"
#include "zocl_xclbin.h"
#include "zocl_error.h"
#include "zocl_hwctx.h"

/*
 * read_axlf and ctx should be protected by slot_xclbin_lock exclusively.
 */
int
zocl_read_axlf_ioctl(struct drm_device *ddev, void *data, struct drm_file *filp)
{
	struct drm_zocl_axlf *axlf_obj = data;
	struct drm_zocl_dev *zdev = ZOCL_GET_ZDEV(ddev);
	struct kds_client *client = filp->driver_priv;
	int slot_id = -1;

	return zocl_xclbin_read_axlf(zdev, axlf_obj, client, &slot_id);
}

/*
 * IOCTL to create hw context on a slot on device for a xclbin.
 */
int zocl_create_hw_ctx_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct drm_zocl_dev *zdev = ZOCL_GET_ZDEV(dev);
	struct drm_zocl_create_hw_ctx *drm_hw_ctx = data;
	struct kds_client *client = filp->driver_priv;
	struct drm_zocl_axlf axlf_obj = {};
	int slot_id = -1;
	int ret = 0;

	if (copy_from_user(&axlf_obj, drm_hw_ctx->axlf_ptr, sizeof(struct drm_zocl_axlf))) {
		DRM_WARN("copy_from_user failed for axlf_ptr");
		return -EFAULT;
	}

	ret = zocl_xclbin_read_axlf(zdev, &axlf_obj, client, &slot_id);
	if (ret) {
		DRM_WARN("xclbin download FAILED.");
		return ret;
	}

	return zocl_create_hw_ctx(zdev, drm_hw_ctx, filp, slot_id);
}

/*
 * IOCTL to destroy hw context on a slot on device
 */
int zocl_destroy_hw_ctx_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct drm_zocl_dev *zdev = ZOCL_GET_ZDEV(dev);
	struct drm_zocl_destroy_hw_ctx *drm_hw_ctx = (struct drm_zocl_destroy_hw_ctx *)data;

	return zocl_destroy_hw_ctx(zdev, drm_hw_ctx, filp);
}

/*
 * IOCTL to open a cu context under the given hw context
 */
int zocl_open_cu_ctx_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct drm_zocl_dev *zdev = ZOCL_GET_ZDEV(dev);
	struct drm_zocl_open_cu_ctx *drm_cu_ctx = (struct drm_zocl_open_cu_ctx *)data;

	return zocl_open_cu_ctx(zdev, drm_cu_ctx, filp);
}

/*
 * IOCTL to close a opened cu context under the given hw context
 */
int zocl_close_cu_ctx_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct drm_zocl_dev *zdev = ZOCL_GET_ZDEV(dev);
	struct drm_zocl_close_cu_ctx *drm_cu_ctx = (struct drm_zocl_close_cu_ctx *)data;

	return zocl_close_cu_ctx(zdev, drm_cu_ctx, filp);
}

/*
 * Block comment for context switch.
 * The read_axlf_ioctl can happen without calling open context, we need to use mutex
 * lock to exclude access between read_axlf_ioctl and zocl_ctx_ioctl. At one
 * time, only one operation can be accessed.
 *
 * When swaping xclbin, first call read_axlf_ioctl to download new xclbin, the
 * following conditions have to be true:
 *   -  When we lock the slot_xclbin_lock, no more zocl_ctx/read_axlf
 *   -  If still have live context, we cannot swap xclbin
 *   -  If no live contexts, but still live cmds from previous closed context,
 *      we cannot swap xclbin.
 * If all the above conditions is cleared, we start changing to new xclbin.
 */
int
zocl_ctx_ioctl(struct drm_device *ddev, void *data, struct drm_file *filp)
{
	struct drm_zocl_dev *zdev = ZOCL_GET_ZDEV(ddev);

	/* Do not acquire slot_xclbin_lock like sched_xclbin_ctx().
	 * New KDS would lock bitstream when open the fist context.
	 * The lock bitstream would exclude read_axlf_ioctl().
	 */
	return zocl_context_ioctl(zdev, data, filp);
}

/* IOCTL to get CU index in aperture list
 * used for recognizing BO and CU in mmap
 */
int
zocl_info_cu_ioctl(struct drm_device *ddev, void *data, struct drm_file *filp)
{
	struct drm_zocl_info_cu *args = data;
	struct drm_zocl_dev *zdev = ddev->dev_private;
	struct addr_aperture *apts = zdev->cu_subdev.apertures;
	int apt_idx = args->apt_idx;
	int cu_idx = args->cu_idx;
	phys_addr_t addr = args->paddr;

	if (cu_idx != -1) {
		apt_idx = get_apt_index_by_cu_idx(zdev, cu_idx);
		if (apt_idx != -EINVAL) {
			addr = apts[apt_idx].addr;
			goto out;
		}
	}

	apt_idx = get_apt_index_by_addr(zdev, args->paddr);
	if (apt_idx != -EINVAL)
		cu_idx = apts[apt_idx].cu_idx;

out:
	args->paddr = addr;
	args->apt_idx = apt_idx;
	args->cu_idx = cu_idx;
        //update cu size based on the apt_index
        args->cu_size = apts[apt_idx].size;
	return 0;
}

int
zocl_execbuf_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct drm_zocl_dev *zdev = dev->dev_private;

	return zocl_command_ioctl(zdev, data, filp);
}

int
zocl_hw_ctx_execbuf_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct drm_zocl_dev *zdev = ZOCL_GET_ZDEV(dev);
	struct drm_zocl_hw_ctx_execbuf *drm_hw_ctx_execbuf = (struct drm_zocl_hw_ctx_execbuf *)data;

	return zocl_hw_ctx_execbuf(zdev, drm_hw_ctx_execbuf, filp);
}

int
zocl_error_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct drm_zocl_dev *zdev = dev->dev_private;
	int ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	ret = zocl_inject_error(zdev, data, filp);
	return ret;
}

int
zocl_aie_fd_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct drm_zocl_aie *args = data;
	struct drm_zocl_dev *zdev = ZOCL_GET_ZDEV(dev);
	int ret = 0;

	ret = zocl_aie_request_part_fd(zdev, args);
	return ret;
}

int
zocl_aie_reset_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct drm_zocl_dev *zdev = ZOCL_GET_ZDEV(dev);
	int ret;

	ret = zocl_aie_reset(zdev);
	return ret;
}

int
zocl_aie_freqscale_ioctl(struct drm_device *dev, void *data, struct drm_file *filp)
{
	struct drm_zocl_dev *zdev = ZOCL_GET_ZDEV(dev);

	return zocl_aie_freqscale(zdev, data);
}

int
zocl_set_cu_read_only_range_ioctl(struct drm_device *dev, void *data,
				  struct drm_file *filp)
{
	struct drm_zocl_dev *zdev = ZOCL_GET_ZDEV(dev);
	struct drm_zocl_set_cu_range *info = data;
	int ret = 0;

	ret = zocl_kds_set_cu_read_range(zdev, info->cu_index, info->start, info->size);
	return ret;
}
