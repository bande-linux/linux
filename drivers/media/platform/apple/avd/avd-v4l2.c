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

#include <linux/log2.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>

#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "avd.h"

#define AVD_MIN_SIZE		64
#define AVD_MAX_SIZE		4096

/* The decoder writes 64-pixel-wide, 16-line-aligned pictures (asahi avd). */
static const struct v4l2_frmsize_stepwise avd_capture_frmsize = {
	.min_width = AVD_MIN_SIZE,
	.max_width = AVD_MAX_SIZE,
	.step_width = 64,
	.min_height = AVD_MIN_SIZE,
	.max_height = AVD_MAX_SIZE,
	.step_height = 16,
};

static void avd_tile_meta(u32 w, u32 h, u32 bpp, u32 tile_dim, u32 meta_bytes,
			  u32 *tile, u32 *meta)
{
	u32 tiles_w = DIV_ROUND_UP(w, tile_dim);
	u32 tiles_h = DIV_ROUND_UP(h, tile_dim);

	*tile = ALIGN(tiles_w * tiles_h * tile_dim * tile_dim *
		      DIV_ROUND_UP(bpp, 8), 16);
	*meta = ALIGN(roundup_pow_of_two(tiles_w) * roundup_pow_of_two(tiles_h) *
		      meta_bytes, 16);
}

/* Compressed copy: 32x32 luma, 16x16 chroma tiles, 32/8 metadata bytes (asahi avd). */
static void avd_fill_layout(struct avd_ctx *ctx, struct avd_layout *layout,
			    const struct v4l2_pix_format_mplane *pix_mp)
{
	const struct avd_coded_fmt_ops *ops = ctx->coded_fmt_desc->ops;
	u32 y, y_meta, uv, uv_meta;

	avd_tile_meta(pix_mp->width, pix_mp->height, 8, 32, 32, &y, &y_meta);
	avd_tile_meta(pix_mp->width / 2, pix_mp->height / 2, 16, 16, 8, &uv,
		      &uv_meta);

	layout->comp.offsets[0] = y;
	layout->comp.offsets[1] = 0;
	layout->comp.offsets[2] = y + y_meta + uv;
	layout->comp.offsets[3] = y + y_meta;
	layout->comp.size = y + y_meta + uv + uv_meta;

	layout->comp_offset = ALIGN(pix_mp->plane_fmt[0].sizeimage, AVD_ALIGN);
	layout->mv_offset = ALIGN(layout->comp_offset + layout->comp.size,
				  AVD_ALIGN);
	layout->size = layout->mv_offset +
		       ops->mv_size(pix_mp->width, pix_mp->height);
}

static void avd_fill_decoded_pixfmt(struct avd_ctx *ctx,
				    struct v4l2_pix_format_mplane *pix_mp,
				    struct avd_layout *layout)
{
	const struct v4l2_pix_format_mplane *coded = &ctx->coded_fmt.fmt.pix_mp;

	pix_mp->width = coded->width;
	pix_mp->height = coded->height;
	v4l2_apply_frmsize_constraints(&pix_mp->width, &pix_mp->height,
				       &avd_capture_frmsize);
	v4l2_fill_pixfmt_mp(pix_mp, V4L2_PIX_FMT_NV12, pix_mp->width,
			    pix_mp->height);
	avd_fill_layout(ctx, layout, pix_mp);
	pix_mp->plane_fmt[0].sizeimage = layout->size;
	pix_mp->field = V4L2_FIELD_NONE;
	pix_mp->colorspace = coded->colorspace;
	pix_mp->xfer_func = coded->xfer_func;
	pix_mp->ycbcr_enc = coded->ycbcr_enc;
	pix_mp->quantization = coded->quantization;
}

void avd_reset_decoded_fmt(struct avd_ctx *ctx)
{
	struct v4l2_format *f = &ctx->decoded_fmt;

	memset(f, 0, sizeof(*f));
	f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	avd_fill_decoded_pixfmt(ctx, &f->fmt.pix_mp, &ctx->layout);
}

static const struct v4l2_ctrl_h264_sps avd_h264_default_sps = {
	.chroma_format_idc = 1,
	.flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY,
};

static const struct v4l2_ctrl_config avd_h264_ctrl_descs[] = {
	{
		.id = V4L2_CID_STATELESS_H264_DECODE_PARAMS,
	},
	{
		.id = V4L2_CID_STATELESS_H264_SPS,
		.p_def.p_const = &avd_h264_default_sps,
	},
	{
		.id = V4L2_CID_STATELESS_H264_PPS,
	},
	{
		.id = V4L2_CID_STATELESS_H264_SCALING_MATRIX,
	},
	{
		.id = V4L2_CID_STATELESS_H264_PRED_WEIGHTS,
	},
	{
		.id = V4L2_CID_STATELESS_H264_SLICE_PARAMS,
	},
	{
		.id = V4L2_CID_STATELESS_H264_DECODE_MODE,
		.min = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED,
		.max = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED,
		.def = V4L2_STATELESS_H264_DECODE_MODE_SLICE_BASED,
	},
	{
		.id = V4L2_CID_STATELESS_H264_START_CODE,
		.min = V4L2_STATELESS_H264_START_CODE_NONE,
		.max = V4L2_STATELESS_H264_START_CODE_NONE,
		.def = V4L2_STATELESS_H264_START_CODE_NONE,
	},
	{
		.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE,
		.min = V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE,
		.max = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
		.menu_skip_mask = BIT(V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED),
		.def = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN,
	},
	{
		.id = V4L2_CID_MPEG_VIDEO_H264_LEVEL,
		.min = V4L2_MPEG_VIDEO_H264_LEVEL_1_0,
		.max = V4L2_MPEG_VIDEO_H264_LEVEL_5_1,
	},
};

static const struct avd_ctrls avd_h264_ctrls = {
	.ctrls = avd_h264_ctrl_descs,
	.num_ctrls = ARRAY_SIZE(avd_h264_ctrl_descs),
};

/* Level 5.1 area (36864 MBs, 8912896 samples) at 4096 wide; not a height limit. */
static const struct avd_coded_fmt_desc avd_coded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE,
		.frmsize = {
			.min_width = AVD_MIN_SIZE,
			.max_width = AVD_MAX_SIZE,
			.step_width = 1,
			.min_height = AVD_MIN_SIZE,
			.max_height = 2304,
			.step_height = 1,
		},
		.ctrls = &avd_h264_ctrls,
		.ops = &avd_h264_fmt_ops,
		.frame_ctrl = V4L2_CID_STATELESS_H264_SLICE_PARAMS,
		.subsystem_flags = VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF,
		.capability = AVD_CAPABILITY_H264,
	},
};

static bool avd_is_capable(struct avd_ctx *ctx, unsigned int capability)
{
	return (ctx->dev->capabilities & capability) == capability;
}

static int avd_try_ctrl(struct v4l2_ctrl *ctrl)
{
	struct avd_ctx *ctx = container_of(ctrl->handler, struct avd_ctx,
					   ctrl_hdl);
	int ret;

	for (unsigned int i = 0; i < ARRAY_SIZE(avd_coded_fmts); i++) {
		if (!avd_is_capable(ctx, avd_coded_fmts[i].capability))
			continue;
		ret = avd_coded_fmts[i].ops->try_ctrl(ctrl);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct v4l2_ctrl_ops avd_ctrl_ops = {
	.try_ctrl = avd_try_ctrl,
};

static const struct avd_coded_fmt_desc *
avd_enum_coded_fmt_desc(struct avd_ctx *ctx, int index)
{
	int fmt_idx = -1;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(avd_coded_fmts); i++) {
		if (!avd_is_capable(ctx, avd_coded_fmts[i].capability))
			continue;
		fmt_idx++;
		if (index == fmt_idx)
			return &avd_coded_fmts[i];
	}

	return NULL;
}

static const struct avd_coded_fmt_desc *
avd_find_coded_fmt_desc(struct avd_ctx *ctx, u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(avd_coded_fmts); i++) {
		if (avd_is_capable(ctx, avd_coded_fmts[i].capability) &&
		    avd_coded_fmts[i].fourcc == fourcc)
			return &avd_coded_fmts[i];
	}

	return NULL;
}

static u32 avd_coded_sizeimage(u32 width, u32 height)
{
	return max_t(u32, SZ_2M, width * height * 3 / 4);
}

void avd_reset_coded_fmt(struct avd_ctx *ctx)
{
	struct v4l2_format *f = &ctx->coded_fmt;

	ctx->coded_fmt_desc = avd_enum_coded_fmt_desc(ctx, 0);

	memset(f, 0, sizeof(*f));
	f->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	f->fmt.pix_mp.pixelformat = ctx->coded_fmt_desc->fourcc;
	f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	f->fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
	f->fmt.pix_mp.width = ctx->coded_fmt_desc->frmsize.min_width;
	f->fmt.pix_mp.height = ctx->coded_fmt_desc->frmsize.min_height;
	f->fmt.pix_mp.num_planes = 1;
	f->fmt.pix_mp.plane_fmt[0].sizeimage =
		avd_coded_sizeimage(f->fmt.pix_mp.width, f->fmt.pix_mp.height);
}

static int avd_enum_framesizes(struct file *file, void *priv,
			       struct v4l2_frmsizeenum *fsize)
{
	struct avd_ctx *ctx = file_to_ctx(file);
	const struct avd_coded_fmt_desc *desc;

	if (fsize->index != 0)
		return -EINVAL;

	desc = avd_find_coded_fmt_desc(ctx, fsize->pixel_format);
	if (!desc)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_CONTINUOUS;
	fsize->stepwise = desc->frmsize;

	return 0;
}

static int avd_querycap(struct file *file, void *priv,
			struct v4l2_capability *cap)
{
	strscpy(cap->driver, KBUILD_MODNAME, sizeof(cap->driver));
	strscpy(cap->card, "Apple Video Decoder", sizeof(cap->card));

	return 0;
}

static int avd_try_capture_fmt(struct file *file, void *priv,
			       struct v4l2_format *f)
{
	struct avd_ctx *ctx = file_to_ctx(file);
	struct avd_layout layout;

	avd_fill_decoded_pixfmt(ctx, &f->fmt.pix_mp, &layout);

	return 0;
}

static void avd_try_output_pixfmt(struct avd_ctx *ctx,
				  struct v4l2_pix_format_mplane *pix_mp)
{
	const struct avd_coded_fmt_desc *desc;
	u32 min_size;

	desc = avd_find_coded_fmt_desc(ctx, pix_mp->pixelformat);
	if (!desc) {
		desc = avd_enum_coded_fmt_desc(ctx, 0);
		pix_mp->pixelformat = desc->fourcc;
	}

	v4l2_apply_frmsize_constraints(&pix_mp->width, &pix_mp->height,
				       &desc->frmsize);

	pix_mp->field = V4L2_FIELD_NONE;
	pix_mp->num_planes = 1;
	pix_mp->plane_fmt[0].bytesperline = 0;
	min_size = avd_coded_sizeimage(pix_mp->width, pix_mp->height);
	pix_mp->plane_fmt[0].sizeimage = clamp_t(u32,
						 pix_mp->plane_fmt[0].sizeimage,
						 min_size, SZ_32M);
}

static int avd_try_output_fmt(struct file *file, void *priv,
			      struct v4l2_format *f)
{
	avd_try_output_pixfmt(file_to_ctx(file), &f->fmt.pix_mp);
	return 0;
}

static int avd_s_capture_fmt(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	struct avd_ctx *ctx = file_to_ctx(file);
	struct avd_layout layout;
	struct vb2_queue *vq;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (vb2_is_busy(vq))
		return -EBUSY;

	avd_fill_decoded_pixfmt(ctx, &f->fmt.pix_mp, &layout);
	ctx->decoded_fmt = *f;
	ctx->layout = layout;

	return 0;
}

static int avd_s_output_fmt(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	struct avd_ctx *ctx = file_to_ctx(file);
	struct v4l2_m2m_ctx *m2m_ctx = ctx->fh.m2m_ctx;
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct vb2_queue *peer_vq, *vq;
	bool busy;

	vq = v4l2_m2m_get_vq(m2m_ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	busy = vb2_is_busy(vq);
	if (vb2_is_streaming(vq) ||
	    (busy && pix_mp->pixelformat != ctx->coded_fmt.fmt.pix_mp.pixelformat))
		return -EBUSY;

	peer_vq = v4l2_m2m_get_vq(m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (vb2_is_busy(peer_vq))
		return -EBUSY;

	avd_try_output_pixfmt(ctx, pix_mp);
	if (busy)
		pix_mp->plane_fmt[0].sizeimage =
			ctx->coded_fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

	ctx->coded_fmt_desc = avd_find_coded_fmt_desc(ctx, pix_mp->pixelformat);
	ctx->coded_fmt = *f;
	avd_reset_decoded_fmt(ctx);

	vq->subsystem_flags &= ~VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF;
	vq->subsystem_flags |= ctx->coded_fmt_desc->subsystem_flags;

	return 0;
}

static int avd_g_output_fmt(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	*f = file_to_ctx(file)->coded_fmt;
	return 0;
}

static int avd_g_capture_fmt(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	*f = file_to_ctx(file)->decoded_fmt;
	return 0;
}

static int avd_enum_output_fmt(struct file *file, void *priv,
			       struct v4l2_fmtdesc *f)
{
	const struct avd_coded_fmt_desc *desc;

	desc = avd_enum_coded_fmt_desc(file_to_ctx(file), f->index);
	if (!desc)
		return -EINVAL;

	f->pixelformat = desc->fourcc;
	return 0;
}

static int avd_enum_capture_fmt(struct file *file, void *priv,
				struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_NV12;
	return 0;
}

const struct v4l2_ioctl_ops avd_ioctl_ops = {
	.vidioc_querycap = avd_querycap,
	.vidioc_enum_framesizes = avd_enum_framesizes,

	.vidioc_try_fmt_vid_cap_mplane = avd_try_capture_fmt,
	.vidioc_try_fmt_vid_out_mplane = avd_try_output_fmt,
	.vidioc_s_fmt_vid_out_mplane = avd_s_output_fmt,
	.vidioc_s_fmt_vid_cap_mplane = avd_s_capture_fmt,
	.vidioc_g_fmt_vid_out_mplane = avd_g_output_fmt,
	.vidioc_g_fmt_vid_cap_mplane = avd_g_capture_fmt,
	.vidioc_enum_fmt_vid_out = avd_enum_output_fmt,
	.vidioc_enum_fmt_vid_cap = avd_enum_capture_fmt,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_remove_bufs = v4l2_m2m_ioctl_remove_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_decoder_cmd = v4l2_m2m_ioctl_stateless_decoder_cmd,
	.vidioc_try_decoder_cmd = v4l2_m2m_ioctl_stateless_try_decoder_cmd,
};

static int avd_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
			   unsigned int *num_planes, unsigned int sizes[],
			   struct device *alloc_devs[])
{
	struct avd_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_format *f;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		f = &ctx->coded_fmt;
	else
		f = &ctx->decoded_fmt;

	if (*num_planes) {
		if (*num_planes != 1 ||
		    sizes[0] < f->fmt.pix_mp.plane_fmt[0].sizeimage)
			return -EINVAL;
		return 0;
	}

	*num_planes = 1;
	sizes[0] = f->fmt.pix_mp.plane_fmt[0].sizeimage;

	return 0;
}

static int avd_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct avd_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_format *f;
	u32 sizeimage;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		f = &ctx->coded_fmt;
	else
		f = &ctx->decoded_fmt;

	sizeimage = f->fmt.pix_mp.plane_fmt[0].sizeimage;
	if (vb2_plane_size(vb, 0) < sizeimage)
		return -EINVAL;

	if (V4L2_TYPE_IS_CAPTURE(vq->type))
		vb2_set_plane_payload(vb, 0, sizeimage);

	return 0;
}

static void avd_buf_queue(struct vb2_buffer *vb)
{
	struct avd_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static int avd_buf_out_validate(struct vb2_buffer *vb)
{
	to_vb2_v4l2_buffer(vb)->field = V4L2_FIELD_NONE;
	return 0;
}

static void avd_buf_request_complete(struct vb2_buffer *vb)
{
	struct avd_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->ctrl_hdl);
}

static int avd_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct avd_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *vbuf;
	int ret;

	if (V4L2_TYPE_IS_CAPTURE(q->type))
		return 0;

	ret = ctx->coded_fmt_desc->ops->start(ctx);
	if (!ret)
		return 0;

	while ((vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx)))
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_QUEUED);

	return ret;
}

static void avd_stop_streaming(struct vb2_queue *q)
{
	struct avd_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *vbuf;

	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		ctx->coded_fmt_desc->ops->stop(ctx);
		avd_job_free(ctx);
	}

	while (true) {
		if (V4L2_TYPE_IS_OUTPUT(q->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;

		v4l2_ctrl_request_complete(vbuf->vb2_buf.req_obj.req,
					   &ctx->ctrl_hdl);
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

const struct vb2_ops avd_queue_ops = {
	.queue_setup = avd_queue_setup,
	.buf_prepare = avd_buf_prepare,
	.buf_queue = avd_buf_queue,
	.buf_out_validate = avd_buf_out_validate,
	.buf_request_complete = avd_buf_request_complete,
	.start_streaming = avd_start_streaming,
	.stop_streaming = avd_stop_streaming,
};

void avd_job_finish_no_pm(struct avd_ctx *ctx, enum vb2_buffer_state result)
{
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	bool held = src && (src->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF);

	if (!held && ctx->ref_error)
		result = VB2_BUF_STATE_ERROR;
	if (dst)
		vb2_to_avd_decoded_buf(&dst->vb2_buf)->error =
			result != VB2_BUF_STATE_DONE;

	ctx->coded_fmt_desc->ops->done(ctx, result);

	if (src)
		v4l2_ctrl_request_complete(src->vb2_buf.req_obj.req,
					   &ctx->ctrl_hdl);

	v4l2_m2m_buf_done_and_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx,
					 result);
}

void avd_job_finish(struct avd_ctx *ctx, enum vb2_buffer_state result)
{
	pm_runtime_put_autosuspend(ctx->dev->dev);
	avd_job_finish_no_pm(ctx, result);
}

int avd_run_preamble(struct avd_ctx *ctx, struct avd_run *run)
{
	struct avd_decoded_buffer *dst;
	struct media_request *src_req;
	dma_addr_t base;
	int ret;

	memset(run, 0, sizeof(*run));

	run->src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	run->dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	src_req = run->src->vb2_buf.req_obj.req;
	if (src_req) {
		ret = v4l2_ctrl_request_setup(src_req, &ctx->ctrl_hdl);
		if (ret)
			return ret;
	}

	run->coded_in = vb2_dma_contig_plane_dma_addr(&run->src->vb2_buf, 0);
	base = vb2_dma_contig_plane_dma_addr(&run->dst->vb2_buf, 0);
	run->y_out = base;
	run->uv_out = base + ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline *
			     ctx->decoded_fmt.fmt.pix_mp.height;
	run->comp_out = base + ctx->layout.comp_offset;
	run->mv_out = base + ctx->layout.mv_offset;

	dst = vb2_to_avd_decoded_buf(&run->dst->vb2_buf);
	dst->layout = ctx->layout;

	v4l2_m2m_buf_copy_metadata(run->src, run->dst);

	return 0;
}

int avd_init_ctrls(struct avd_ctx *ctx)
{
	unsigned int i, j, nctrls = 0;
	int ret;

	for (i = 0; i < ARRAY_SIZE(avd_coded_fmts); i++)
		if (avd_is_capable(ctx, avd_coded_fmts[i].capability))
			nctrls += avd_coded_fmts[i].ctrls->num_ctrls;

	v4l2_ctrl_handler_init(&ctx->ctrl_hdl, nctrls);

	for (i = 0; i < ARRAY_SIZE(avd_coded_fmts); i++) {
		const struct avd_ctrls *ctrls = avd_coded_fmts[i].ctrls;

		if (!avd_is_capable(ctx, avd_coded_fmts[i].capability))
			continue;

		for (j = 0; j < ctrls->num_ctrls; j++) {
			struct v4l2_ctrl_config cfg = ctrls->ctrls[j];

			cfg.ops = &avd_ctrl_ops;
			v4l2_ctrl_new_custom(&ctx->ctrl_hdl, &cfg, ctx);
		}
	}

	ret = ctx->ctrl_hdl.error;
	if (ret)
		goto err_free_handler;

	ret = v4l2_ctrl_handler_setup(&ctx->ctrl_hdl);
	if (ret)
		goto err_free_handler;

	ctx->fh.ctrl_handler = &ctx->ctrl_hdl;
	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	return ret;
}

int avd_request_validate(struct media_request *req)
{
	struct media_request_object *obj;
	struct v4l2_ctrl_handler *hdl;
	struct avd_ctx *ctx = NULL;
	struct v4l2_ctrl *ctrl;
	unsigned int count;

	count = vb2_request_buffer_cnt(req);
	if (!count)
		return -ENOENT;
	if (count > 1)
		return -EINVAL;

	list_for_each_entry(obj, &req->objects, list) {
		if (vb2_request_object_is_buffer(obj)) {
			struct vb2_buffer *vb = container_of(obj, struct vb2_buffer,
							     req_obj);

			ctx = vb2_get_drv_priv(vb->vb2_queue);
			break;
		}
	}
	if (!ctx)
		return -ENOENT;

	hdl = v4l2_ctrl_request_hdl_find(req, &ctx->ctrl_hdl);
	if (!hdl)
		return -ENOENT;
	ctrl = v4l2_ctrl_request_hdl_ctrl_find(hdl, ctx->coded_fmt_desc->frame_ctrl);
	v4l2_ctrl_request_hdl_put(hdl);
	if (!ctrl)
		return -ENOENT;

	return vb2_request_validate(req);
}

void *avd_ctrl_ptr(struct avd_ctx *ctx, u32 id)
{
	return v4l2_ctrl_find(&ctx->ctrl_hdl, id)->p_cur.p;
}
