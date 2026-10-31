// SPDX-License-Identifier: GPL-2.0
/*
 * Apple Video Decoder driver
 *
 * Copyright The Asahi Linux Contributors
 *
 * Based on rkvdec driver by Collabora, Ltd.
 * Copyright (C) 2019 Collabora, Ltd.
 * Based on rkvdec driver by Google LLC. (Tomasz Figa <tfiga@chromium.org>)
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 */

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include <media/videobuf2-dma-contig.h>

#include "avd.h"
#include "avd-inst.h"
#include "avd-regs.h"

/* avd-fw places a 256-byte config at 0x400: magic "AVDc", then codec mask. */
#define AVD_FW_CONFIG_OFFSET	0x400
#define AVD_FW_CONFIG_MAGIC	0x63445641
#define AVD_FW_CONFIG_CODECS	0x4
#define AVD_FW_CONFIG_SIZE	0x100

/* Instruction FIFO the asahi avd driver programs for every VP stream. */
#define AVD_INSN_FIFO_SIZE	(12 * SZ_1M)

/* Watchdog of the asahi avd driver. */
#define AVD_TIMEOUT_MS		2000

int avd_buf_alloc(struct avd_dev *avd, struct avd_buf *buf, size_t size)
{
	if (buf->cpu && size <= buf->size)
		return 0;

	avd_buf_free(avd, buf);

	buf->cpu = dma_alloc_coherent(avd->dev, size, &buf->addr, GFP_KERNEL);
	if (!buf->cpu)
		return -ENOMEM;
	buf->size = size;

	return 0;
}

void avd_buf_free(struct avd_dev *avd, struct avd_buf *buf)
{
	if (buf->cpu)
		dma_free_coherent(avd->dev, buf->size, buf->cpu, buf->addr);
	memset(buf, 0, sizeof(*buf));
}

struct avd_decoded_buffer *avd_get_ref_buf(struct avd_ctx *ctx,
					   struct avd_run *run, u64 timestamp)
{
	struct vb2_queue *cap_q = &ctx->fh.m2m_ctx->cap_q_ctx.q;
	struct avd_decoded_buffer *ref;
	struct vb2_buffer *buf;

	buf = vb2_find_buffer(cap_q, timestamp);
	if (!buf) {
		ctx->ref_error = true;
		buf = &run->dst->vb2_buf;
	}

	ref = vb2_to_avd_decoded_buf(buf);
	if (ref->error)
		ctx->ref_error = true;

	return ref;
}

dma_addr_t avd_buf_addr(struct avd_decoded_buffer *buf)
{
	return vb2_dma_contig_plane_dma_addr(&buf->base.vb.vb2_buf, 0);
}

void avd_job_free(struct avd_ctx *ctx)
{
	kvfree(ctx->job.segments);
	memset(&ctx->job, 0, sizeof(ctx->job));
}

int avd_job_begin(struct avd_ctx *ctx, unsigned int segments)
{
	struct avd_job *job = &ctx->job;

	avd_job_free(ctx);
	ctx->ref_error = false;

	job->segments = kvcalloc(segments, sizeof(*job->segments), GFP_KERNEL);
	if (!job->segments)
		return -ENOMEM;
	job->alloc = segments;

	return 0;
}

int avd_job_next(struct avd_ctx *ctx)
{
	struct avd_job *job = &ctx->job;

	if (!job->segments)
		return -EINVAL;

	if (job->num + 1 >= job->alloc) {
		unsigned int alloc = job->alloc * 2;
		struct avd_segment *segments;

		segments = kvrealloc(job->segments,
				     array_size(alloc, sizeof(*segments)),
				     GFP_KERNEL);
		if (!segments)
			return -ENOMEM;
		job->segments = segments;
		job->alloc = alloc;
	}

	job->segments[++job->num].num = 0;

	return 0;
}

static struct avd_ctx *avd_claim_job(struct avd_dev *avd, bool timed_out)
{
	struct avd_ctx *ctx;

	spin_lock(&avd->job_lock);
	ctx = avd->job_ctx;
	if (ctx && timed_out &&
	    time_before(jiffies, avd->job_start + msecs_to_jiffies(AVD_TIMEOUT_MS)))
		ctx = NULL;
	if (ctx)
		avd->job_ctx = NULL;
	spin_unlock(&avd->job_lock);

	return ctx;
}

static void avd_queue_recovery(struct avd_dev *avd, struct avd_ctx *ctx)
{
	cancel_delayed_work(&avd->watchdog);
	avd->recover_ctx = ctx;
	queue_work(system_wq, &avd->recover_work);
}

int avd_submit_job(struct avd_ctx *ctx, enum avd_codec codec)
{
	struct avd_dev *avd = ctx->dev;
	struct avd_job job = ctx->job;
	void __iomem *queue_max, *queue_cur, *reg;
	unsigned int vp = 0, i, idx;
	bool stalled = false;
	u32 max, cur;

	/* The job may finish and ctx go away once the hardware has it. */
	memset(&ctx->job, 0, sizeof(ctx->job));
	if (!job.segments || !job.num || job.overflow) {
		kvfree(job.segments);
		return -EINVAL;
	}

	for (i = 0; i < codec; i++)
		vp += avd->variant->vp_slots[i];

	reg = avd->ctrl + avd->variant->vp_slot_offset + vp * 4;
	queue_max = avd->ctrl + avd->variant->submit_queue_max_offset + vp * 4;
	queue_cur = avd->ctrl + avd->variant->submit_queue_status_offset + vp * 4;

	mutex_lock(&avd->hw_lock);

	spin_lock(&avd->job_lock);
	avd->job_ctx = ctx;
	avd->job_start = jiffies;
	spin_unlock(&avd->job_lock);
	schedule_delayed_work(&avd->watchdog, msecs_to_jiffies(AVD_TIMEOUT_MS));

	avd_configure_stream(avd, avd->insn_fifo.addr, vp);

	writel(AVD_OP_EXEC | AVD_OP_EXEC_FLAG_START(1) | AVD_OP_EXEC_FIFO_IDX(0),
	       reg);
	for (idx = 0; idx <= job.num; idx++) {
		struct avd_segment *seg = &job.segments[idx];

		for (i = 0; i < seg->num; i++)
			writel(seg->instructions[i], reg);
		if (!idx)
			continue;

		max = readl_relaxed(queue_max);
		if (readl_poll_timeout(queue_cur, cur, cur < max, 10, 100000)) {
			dev_err(avd->dev, "VP %u queue stuck at %u/%u\n", vp, cur,
				max);
			stalled = true;
			break;
		}
		writel(AVD_OP_EXEC | AVD_OP_EXEC_FLAG_END(idx == job.num), reg);
	}

	mutex_unlock(&avd->hw_lock);

	kvfree(job.segments);

	if (stalled) {
		ctx = avd_claim_job(avd, false);
		if (ctx)
			avd_queue_recovery(avd, ctx);
	}

	return 0;
}

void avd_submit_pp(struct avd_ctx *ctx, unsigned int count)
{
	struct avd_dev *avd = ctx->dev;
	void __iomem *reg = avd->ctrl + avd->variant->submit_offset;
	u32 op = AVD_OP_EXEC | AVD_OP_EXEC_FIFO_IDX(0) |
		 AVD_OP_EXEC_FIFO_MASK(avd->variant->fifo_slots);

	writel(op | AVD_OP_EXEC_FLAG_START(1), reg);
	for (unsigned int i = 1; i < count; i++)
		writel(op, reg);
}

/*
 * ps_avd_sys also holds the AVD DART (t8112.dtsi), so it is not reset; this
 * restarts the coprocessor, and the engines only if the domain powers down.
 */
static void avd_recover(struct avd_dev *avd, struct avd_ctx *ctx)
{
	bool suspended;
	int ret, boot = 0;

	mutex_lock(&avd->hw_lock);
	suspended = !pm_runtime_put_sync_suspend(avd->dev);
	ret = pm_runtime_resume_and_get(avd->dev);
	if (!ret && !suspended) {
		disable_irq(avd->irq);
		avd_shutdown(avd);
		boot = avd_boot(avd);
		enable_irq(avd->irq);
	}
	mutex_unlock(&avd->hw_lock);

	if (ret < 0 || boot)
		dev_err(avd->dev, "decoder restart failed: %d\n", ret ?: boot);
	if (ret < 0)
		avd_job_finish_no_pm(ctx, VB2_BUF_STATE_ERROR);
	else
		avd_job_finish(ctx, VB2_BUF_STATE_ERROR);
}

static void avd_watchdog_func(struct work_struct *work)
{
	struct avd_dev *avd = container_of(to_delayed_work(work),
					   struct avd_dev, watchdog);
	struct avd_ctx *ctx = avd_claim_job(avd, true);

	if (ctx)
		avd_recover(avd, ctx);
}

static void avd_recover_func(struct work_struct *work)
{
	struct avd_dev *avd = container_of(work, struct avd_dev, recover_work);

	avd_recover(avd, avd->recover_ctx);
}

static irqreturn_t avd_irq_handler(int irq, void *data)
{
	struct avd_dev *avd = data;
	struct avd_ctx *ctx;
	u32 status;

	status = readl(avd->mbox + AVD_REG_MBOX1_RETRIEVE);
	writel(AVD_MBOX1_NOT_EMPTY, avd->mbox + AVD_REG_MBOX_IRQ_CLR);

	if ((status & AVD_MSG_UNHANDLED) ||
	    !(status & (AVD_MSG_PP_DONE | AVD_MSG_VP_DONE))) {
		dev_dbg(avd->dev, "firmware status %#x\n", status);
		ctx = avd_claim_job(avd, false);
		if (ctx)
			avd_queue_recovery(avd, ctx);
	} else if (status & AVD_MSG_PP_DONE) {
		ctx = avd_claim_job(avd, false);
		if (ctx) {
			cancel_delayed_work(&avd->watchdog);
			avd_job_finish(ctx, VB2_BUF_STATE_DONE);
		}
	} else {
		spin_lock(&avd->job_lock);
		ctx = avd->job_ctx;
		if (ctx)
			ctx->coded_fmt_desc->ops->submit(ctx);
		spin_unlock(&avd->job_lock);
	}

	return IRQ_HANDLED;
}

static void avd_device_run(void *priv)
{
	struct avd_ctx *ctx = priv;
	struct avd_dev *avd = ctx->dev;
	int ret;

	ret = pm_runtime_resume_and_get(avd->dev);
	if (ret < 0) {
		avd_job_finish_no_pm(ctx, VB2_BUF_STATE_ERROR);
		return;
	}

	ret = ctx->coded_fmt_desc->ops->run(ctx);
	if (ret)
		avd_job_finish(ctx, VB2_BUF_STATE_ERROR);
}

static int avd_queue_init(void *priv, struct vb2_queue *src_vq,
			  struct vb2_queue *dst_vq)
{
	struct avd_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->ops = &avd_queue_ops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->vdev_lock;
	src_vq->dev = ctx->dev->dev;
	src_vq->supports_requests = true;
	src_vq->requires_requests = true;
	src_vq->subsystem_flags = ctx->coded_fmt_desc->subsystem_flags;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->ops = &avd_queue_ops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->dma_attrs = DMA_ATTR_NO_KERNEL_MAPPING;
	dst_vq->buf_struct_size = sizeof(struct avd_decoded_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->vdev_lock;
	dst_vq->dev = ctx->dev->dev;

	return vb2_queue_init(dst_vq);
}

static int avd_open(struct file *filp)
{
	struct avd_dev *avd = video_drvdata(filp);
	struct avd_ctx *ctx;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = avd;
	avd_reset_coded_fmt(ctx);
	avd_reset_decoded_fmt(ctx);

	v4l2_fh_init(&ctx->fh, video_devdata(filp));

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(avd->m2m_dev, ctx, avd_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_exit_fh;
	}

	ret = avd_init_ctrls(ctx);
	if (ret)
		goto err_release_m2m;

	v4l2_fh_add(&ctx->fh, filp);

	return 0;

err_release_m2m:
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
err_exit_fh:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int avd_release(struct file *filp)
{
	struct avd_ctx *ctx = file_to_ctx(filp);

	v4l2_fh_del(&ctx->fh, filp);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	v4l2_fh_exit(&ctx->fh);
	avd_job_free(ctx);
	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations avd_fops = {
	.owner = THIS_MODULE,
	.open = avd_open,
	.release = avd_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static const struct v4l2_m2m_ops avd_m2m_ops = {
	.device_run = avd_device_run,
};

static const struct media_device_ops avd_media_ops = {
	.req_validate = avd_request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

static void avd_v4l2_release(struct v4l2_device *v4l2_dev)
{
	struct avd_dev *avd = container_of(v4l2_dev, struct avd_dev, v4l2_dev);

	if (avd->m2m_dev)
		v4l2_m2m_release(avd->m2m_dev);
	media_device_cleanup(&avd->mdev);
	mutex_destroy(&avd->hw_lock);
	mutex_destroy(&avd->vdev_lock);
	kfree(avd);
}

static void avd_v4l2_put(void *data)
{
	struct avd_dev *avd = data;

	v4l2_device_unregister(&avd->v4l2_dev);
	v4l2_device_put(&avd->v4l2_dev);
}

static void avd_video_unregister(void *data)
{
	struct avd_dev *avd = data;

	media_device_unregister(&avd->mdev);
	v4l2_m2m_unregister_media_controller(avd->m2m_dev);
	video_unregister_device(&avd->vdev);
}

static int avd_video_register(struct avd_dev *avd)
{
	int ret;

	avd->vdev.lock = &avd->vdev_lock;
	avd->vdev.v4l2_dev = &avd->v4l2_dev;
	avd->vdev.fops = &avd_fops;
	avd->vdev.release = video_device_release_empty;
	avd->vdev.vfl_dir = VFL_DIR_M2M;
	avd->vdev.device_caps = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_M2M_MPLANE;
	avd->vdev.ioctl_ops = &avd_ioctl_ops;
	video_set_drvdata(&avd->vdev, avd);
	strscpy(avd->vdev.name, "apple-avd", sizeof(avd->vdev.name));

	ret = video_register_device(&avd->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		return ret;

	ret = v4l2_m2m_register_media_controller(avd->m2m_dev, &avd->vdev,
						 MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret)
		goto err_unregister_vdev;

	ret = media_device_register(&avd->mdev);
	if (ret)
		goto err_unregister_mc;

	return devm_add_action_or_reset(avd->dev, avd_video_unregister, avd);

err_unregister_mc:
	v4l2_m2m_unregister_media_controller(avd->m2m_dev);
err_unregister_vdev:
	video_unregister_device(&avd->vdev);
	return ret;
}

/* VP slots and register offsets from the asahi avd driver. */
static const struct avd_variant avd_t8112_variant = {
	.vp_slots = {
		[AVD_CODEC_HEVC] = 4,
		[AVD_CODEC_H264] = 4,
		[AVD_CODEC_VP9] = 1,
	},
	.fifo_slots = 15,
	.capabilities = AVD_CAPABILITY_HEVC | AVD_CAPABILITY_H264 |
			AVD_CAPABILITY_VP9,
	.fw_name = "apple/avd-fw-v3-t1.bin",
	.vp_slot_offset = 0xc,
	.submit_offset = 0x30,
	.submit_queue_max_offset = 0x34,
	.submit_queue_status_offset = 0x5c,
};

static const struct of_device_id avd_of_match[] = {
	{ .compatible = "apple,t8112-avd", .data = &avd_t8112_variant },
	{ }
};
MODULE_DEVICE_TABLE(of, avd_of_match);

static void avd_release_firmware(void *data)
{
	release_firmware(data);
}

static int avd_load_firmware(struct avd_dev *avd, resource_size_t code_size)
{
	const u8 *config;
	u32 magic, codecs;
	int ret;

	ret = request_firmware(&avd->fw, avd->variant->fw_name, avd->dev);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(avd->dev, avd_release_firmware,
				       (void *)avd->fw);
	if (ret)
		return ret;

	if (avd->fw->size < AVD_FW_CONFIG_OFFSET + AVD_FW_CONFIG_SIZE ||
	    avd->fw->size > code_size || !IS_ALIGNED(avd->fw->size, 4))
		return dev_err_probe(avd->dev, -EINVAL,
				     "invalid firmware size %zu\n",
				     avd->fw->size);

	config = avd->fw->data + AVD_FW_CONFIG_OFFSET;
	magic = get_unaligned_le32(config);
	if (magic != AVD_FW_CONFIG_MAGIC)
		return dev_err_probe(avd->dev, -EINVAL,
				     "invalid firmware config magic %#x\n",
				     magic);

	codecs = get_unaligned_le32(config + AVD_FW_CONFIG_CODECS);
	avd->capabilities = codecs & avd->variant->capabilities;
	if (!avd->capabilities)
		return dev_err_probe(avd->dev, -ENODEV,
				     "firmware supports no codec (%#x)\n",
				     codecs);

	return 0;
}

static void avd_free_insn_fifo(void *data)
{
	struct avd_dev *avd = data;

	avd_buf_free(avd, &avd->insn_fifo);
}

static int avd_runtime_suspend(struct device *dev)
{
	struct avd_dev *avd = dev_get_drvdata(dev);

	avd_shutdown(avd);
	disable_irq(avd->irq);

	return 0;
}

static int avd_runtime_resume(struct device *dev)
{
	struct avd_dev *avd = dev_get_drvdata(dev);
	int ret;

	ret = avd_boot(avd);
	if (ret) {
		avd_shutdown(avd);
		return ret;
	}
	enable_irq(avd->irq);

	return 0;
}

static void avd_pm_disable(void *data)
{
	struct device *dev = data;

	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev)) {
		avd_runtime_suspend(dev);
		pm_runtime_set_suspended(dev);
	}
}

static void avd_quiesce(void *data)
{
	struct avd_dev *avd = data;

	v4l2_m2m_suspend(avd->m2m_dev);
	cancel_delayed_work_sync(&avd->watchdog);
	cancel_work_sync(&avd->recover_work);
}

static int avd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *code;
	struct avd_dev *avd;
	int ret;

	avd = kzalloc_obj(*avd);
	if (!avd)
		return -ENOMEM;

	avd->dev = dev;
	avd->variant = device_get_match_data(dev);
	mutex_init(&avd->vdev_lock);
	mutex_init(&avd->hw_lock);
	spin_lock_init(&avd->job_lock);
	INIT_DELAYED_WORK(&avd->watchdog, avd_watchdog_func);
	INIT_WORK(&avd->recover_work, avd_recover_func);
	media_device_init(&avd->mdev);
	avd->v4l2_dev.release = avd_v4l2_release;

	ret = v4l2_device_register(dev, &avd->v4l2_dev);
	if (ret) {
		avd_v4l2_release(&avd->v4l2_dev);
		return ret;
	}
	ret = devm_add_action_or_reset(dev, avd_v4l2_put, avd);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, avd);

	code = platform_get_resource_byname(pdev, IORESOURCE_MEM, "code");
	avd->code = devm_ioremap_resource(dev, code);
	if (IS_ERR(avd->code))
		return PTR_ERR(avd->code);
	avd->mbox = devm_platform_ioremap_resource_byname(pdev, "mbox");
	if (IS_ERR(avd->mbox))
		return PTR_ERR(avd->mbox);
	avd->ctrl = devm_platform_ioremap_resource_byname(pdev, "ctrl");
	if (IS_ERR(avd->ctrl))
		return PTR_ERR(avd->ctrl);

	ret = avd_load_firmware(avd, resource_size(code));
	if (ret)
		return ret;

	/* The 38-bit limit the asahi avd driver uses for t8112. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(38));
	if (ret)
		return ret;

	ret = avd_buf_alloc(avd, &avd->insn_fifo, AVD_INSN_FIFO_SIZE);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, avd_free_insn_fifo, avd);
	if (ret)
		return ret;

	avd->m2m_dev = v4l2_m2m_init(&avd_m2m_ops);
	if (IS_ERR(avd->m2m_dev)) {
		ret = PTR_ERR(avd->m2m_dev);
		avd->m2m_dev = NULL;
		return ret;
	}
	avd->mdev.dev = dev;
	avd->mdev.ops = &avd_media_ops;
	strscpy(avd->mdev.model, "apple-avd", sizeof(avd->mdev.model));
	avd->v4l2_dev.mdev = &avd->mdev;

	avd->irq = platform_get_irq(pdev, 1);
	if (avd->irq < 0)
		return avd->irq;
	ret = devm_request_threaded_irq(dev, avd->irq, NULL, avd_irq_handler,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), avd);
	if (ret)
		return ret;

	pm_runtime_set_autosuspend_delay(dev, 100);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	ret = devm_add_action_or_reset(dev, avd_pm_disable, dev);
	if (ret)
		return ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return dev_err_probe(dev, ret, "firmware did not boot\n");
	pm_runtime_put_autosuspend(dev);

	ret = devm_add_action_or_reset(dev, avd_quiesce, avd);
	if (ret)
		return ret;

	return avd_video_register(avd);
}

static int avd_suspend(struct device *dev)
{
	struct avd_dev *avd = dev_get_drvdata(dev);

	v4l2_m2m_suspend(avd->m2m_dev);
	return pm_runtime_force_suspend(dev);
}

static int avd_resume(struct device *dev)
{
	struct avd_dev *avd = dev_get_drvdata(dev);
	int ret;

	ret = pm_runtime_force_resume(dev);
	v4l2_m2m_resume(avd->m2m_dev);

	return ret;
}

static const struct dev_pm_ops avd_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(avd_suspend, avd_resume)
	RUNTIME_PM_OPS(avd_runtime_suspend, avd_runtime_resume, NULL)
};

static struct platform_driver avd_driver = {
	.probe = avd_probe,
	.driver = {
		.name = "apple-avd",
		.of_match_table = avd_of_match,
		.pm = pm_ptr(&avd_pm_ops),
	},
};
module_platform_driver(avd_driver);

MODULE_DESCRIPTION("Apple Video Decoder driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("apple/avd-fw-v3-t1.bin");
