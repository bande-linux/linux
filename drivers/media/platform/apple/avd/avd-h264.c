// SPDX-License-Identifier: GPL-2.0
/*
 * Apple AVD H.264 decoding
 *
 * Copyright The Asahi Linux Contributors
 * Copyright 2023 Eileen Yoon <eyn@gmx.com>
 *
 * Copyright (c) 2014 Rockchip Electronics Co., Ltd.
 *	Hertz Wong <hertz.wong@rock-chips.com>
 *	Herman Chen <herman.chen@rock-chips.com>
 *
 * Copyright (C) 2014 Google, Inc.
 *	Tomasz Figa <tfiga@chromium.org>
 */

#include <media/v4l2-h264.h>
#include <media/videobuf2-dma-contig.h>

#include "avd.h"
#include "avd-inst.h"

#define H264_SCL_DIMS	(0x1000000 | ((64 / 4) << 5) | (((16 / 4) << 5) - 1))

#define H264_FLAG_CI_PRED(v)			FIELD_PREP(BIT(19), !!(v))
#define H264_FLAG_ENTROPY_CODING_MODE(v)	FIELD_PREP(BIT(20), !!(v))
#define H264_FLAG_NOT_IDR(v)			FIELD_PREP(BIT(21), !!(v))

#define H264_TRANSFORM_8X8_MODE(v)		FIELD_PREP(BIT(7), !!(v))

#define H264_DEFAULT_WEIGHT_DENOM \
	(AVD_OP_WEIGHTS_HDR_LUMA(5) | AVD_OP_WEIGHTS_HDR_CHROMA(5))

struct avd_h264_run {
	struct avd_run base;

	const struct v4l2_ctrl_h264_decode_params *decode_params;
	const struct v4l2_ctrl_h264_sps *sps;
	const struct v4l2_ctrl_h264_pps *pps;
	const struct v4l2_ctrl_h264_slice_params *slice_params;
	const struct v4l2_ctrl_h264_scaling_matrix *scaling_matrix;
	const struct v4l2_ctrl_h264_pred_weights *pred_weights;

	s32 cur_poc;
	u8 num_valid;
};

struct avd_h264_ctx {
	struct avd_buf bitstream;
	size_t bitstream_used;

	struct avd_buf pipe_state;
	struct avd_buf above_info;
	struct avd_buf lf_above_info;
	struct avd_buf lf_above;
	struct avd_buf ip_above;
	struct avd_buf mv_above_info;
};

/* Default 8x8 scaling lists in the decoder's order (asahi avd). */
static const u32 default_8x8_intra[] = {
	0x060a0d10, 0x0a0b1012, 0x0d101217, 0x10121719, 0x1217191b, 0x17191b1d,
	0x191b1d1f, 0x1b1d1f21, 0x1217191b, 0x17191b1d, 0x191b1d1f, 0x1b1d1f21,
	0x1d1f2124, 0x1f212426, 0x21242628, 0x2426282a,
};

static const u32 default_8x8_inter[] = {
	0x090d0f11, 0x0d0d1113, 0x0f111315, 0x11131516, 0x13151618, 0x15161819,
	0x1618191b, 0x18191b1c, 0x13151618, 0x15161819, 0x1618191b, 0x18191b1c,
	0x191b1c1e, 0x1b1c1e20, 0x1c1e2021, 0x1e202123,
};

/* The decoder takes 8x8 lists as four 4x4 blocks, not in raster order. */
static const u8 scaling_8x8_map[16] = {
	0,  8,  16, 24,
	4,  12, 20, 28,
	32, 40, 48, 56,
	36, 44, 52, 60,
};

static u32 avd_h264_mv_size(u32 w, u32 h)
{
	return (DIV_ROUND_UP(w, 16) + 1) * (DIV_ROUND_UP(h, 16) + 1) * 64;
}

static u32 pack4(const u8 *v)
{
	return AVD_SCALING_I3(v[0]) | AVD_SCALING_I2(v[1]) |
	       AVD_SCALING_I1(v[2]) | AVD_SCALING_I0(v[3]);
}

static void stream_refs(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	const struct v4l2_h264_dpb_entry *dpb = run->decode_params->dpb;
	struct avd_h264_ctx *h264_ctx = ctx->priv;
	struct avd_decoded_buffer *ref;
	dma_addr_t addr;

	push(ctx, 0);
	push_address(ctx, h264_ctx->mv_above_info.addr);
	push_address(ctx, run->base.mv_out);
	push(ctx, 0);
	push(ctx, 0);
	push(ctx, 0);
	push(ctx, 0);

	for (int i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		if (!(dpb[i].flags & V4L2_H264_DPB_ENTRY_FLAG_VALID))
			continue;

		ref = avd_get_ref_buf(ctx, &run->base, dpb[i].reference_ts);
		addr = avd_buf_addr(ref) +
		       ref->layout.comp_offset;

		push(ctx, AVD_REF_NUM(run->num_valid - 1) | AVD_REF_FLAG_CONST |
			  AVD_REF_FLAG_LONG(dpb[i].flags &
					    V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM) |
			  AVD_REF_DELTA_POC(run->cur_poc -
					    dpb[i].top_field_order_cnt));
		push_comp(ctx, addr, &ref->layout.comp);
	}
}

static void stream_scaling(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	const struct v4l2_ctrl_h264_scaling_matrix *scaling =
		run->scaling_matrix;
	u8 v[4];

	push(ctx, H264_SCL_DIMS);

	for (int i = 0; i < 6; i++)
		for (int j = 0; j < 16; j += 4)
			push(ctx, pack4(&scaling->scaling_list_4x4[i][j]));

	/* Only lists 0 and 1 are used with chroma_format_idc < 3 (7.3.2.2). */
	if (run->pps->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE) {
		for (int i = 0; i < 2; i++) {
			for (int j = 0; j < 16; j++) {
				for (int k = 0; k < 4; k++)
					v[k] = scaling->scaling_list_8x8[i]
						[scaling_8x8_map[j] + k];
				push(ctx, pack4(v));
			}
		}
	} else {
		for (int i = 0; i < ARRAY_SIZE(default_8x8_intra); i++)
			push(ctx, default_8x8_intra[i]);
		for (int i = 0; i < ARRAY_SIZE(default_8x8_inter); i++)
			push(ctx, default_8x8_inter[i]);
	}
}

static void stream_hdr(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	const struct v4l2_ctrl_h264_decode_params *decode = run->decode_params;
	const struct v4l2_ctrl_h264_sps *sps = run->sps;
	const struct v4l2_ctrl_h264_pps *pps = run->pps;
	struct avd_h264_ctx *h264_ctx = ctx->priv;
	bool idr = decode->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC;
	u32 width = (sps->pic_width_in_mbs_minus1 + 1) * 16;
	u32 height = (sps->pic_height_in_map_units_minus1 + 1) * 16;
	u32 bytesperline;

	push(ctx, AVD_OP_HDR | AVD_OP_HDR_FLAG_DECOMP(1) |
		  AVD_OP_HDR_FLAG_INTRA(idr) | AVD_OP_HDR_CONST |
		  AVD_OP_HDR_FLAG_PIPE_STATE(1));
	push(ctx, AVD_HDR_CODEC_MODE(AVD_CODEC_H264));
	push(ctx, AVD_HDR_HEIGHT(height - 1) | AVD_HDR_WIDTH(width - 1));
	push(ctx, 0);
	push(ctx, AVD_HDR_HEIGHT((height - 1) >> 3) |
		  AVD_HDR_WIDTH((width - 1) >> 3));
	push(ctx, AVD_HDR_COMMON_CHROMA_FORMAT(sps->chroma_format_idc) |
		  AVD_HDR_COMMON_BIT_DEPTH_L(sps->bit_depth_luma_minus8) |
		  AVD_HDR_COMMON_BIT_DEPTH_C(sps->bit_depth_chroma_minus8) |
		  AVD_HDR_COMMON_MIN_LUMA_CBS(1) | AVD_HDR_COMMON_LUMA_CBS(1) |
		  H264_TRANSFORM_8X8_MODE(pps->flags &
					  V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE) |
		  AVD_HDR_COMMON_FLAG0(sps->flags &
				       V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE));
	push(ctx, H264_FLAG_ENTROPY_CODING_MODE(pps->flags &
						V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) |
		  H264_FLAG_NOT_IDR(!idr) |
		  H264_FLAG_CI_PRED(pps->flags &
				    V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED));
	push(ctx, AVD_HDR_H26X_QP_OFFSET_CB(pps->chroma_qp_index_offset) |
		  AVD_HDR_H26X_QP_OFFSET_CR(pps->second_chroma_qp_index_offset));
	push(ctx, AVD_HDR_FEAT_H26X | AVD_HDR_FEAT_COMMON | AVD_HDR_FEAT_H264 |
		  AVD_HDR_FEAT_PIPE_STATE_EN);
	push(ctx, 0);
	push(ctx, 0);
	push_address(ctx, h264_ctx->above_info.addr);
	push(ctx, 0);
	push(ctx, 0);
	push_address(ctx, h264_ctx->pipe_state.addr);
	push_address(ctx, h264_ctx->ip_above.addr);
	push_address(ctx, h264_ctx->lf_above.addr);
	push_address(ctx, h264_ctx->lf_above_info.addr);
	push(ctx, 0);

	push_comp(ctx, run->base.comp_out, &ctx->layout.comp);

	bytesperline = ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline >> 4;
	push_address(ctx, run->base.y_out);
	push(ctx, bytesperline);
	push_address(ctx, run->base.uv_out);
	push(ctx, bytesperline);

	push(ctx, 0);
	push(ctx, ((height - 1) << 16) | (width - 1));

	if (!idr && run->num_valid)
		stream_refs(ctx, run);

	if (pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT)
		stream_scaling(ctx, run);
	else
		push(ctx, 0);
}

static void stream_weight(struct avd_ctx *ctx, unsigned int list,
			  unsigned int idx, unsigned int ident, s16 weight,
			  s16 offset)
{
	push(ctx, AVD_OP_WEIGHTS | AVD_OP_WEIGHTS_IDENT(ident) |
		  AVD_OP_WEIGHTS_LIST_IDX(list) | AVD_OP_WEIGHTS_INDEX(idx) |
		  AVD_OP_WEIGHTS_WEIGHT(weight));
	push(ctx, AVD_OP_OFFSETS | AVD_OP_OFFSETS_OFFSET(offset));
}

static void stream_weights(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	const struct v4l2_ctrl_h264_pred_weights *weights = run->pred_weights;
	const struct v4l2_ctrl_h264_slice_params *sl = run->slice_params;
	bool pred_weight_req = V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED(run->pps, sl);
	bool default_weights = run->pps->weighted_bipred_idc == 2 &&
			       !pred_weight_req;
	int luma_denom, chroma_denom;

	if (default_weights)
		push(ctx, AVD_OP_WEIGHTS_HDR | AVD_OP_WEIGHTS_HDR_FLAG1(1) |
			  H264_DEFAULT_WEIGHT_DENOM);
	else
		push(ctx, AVD_OP_WEIGHTS_HDR |
			  AVD_OP_WEIGHTS_HDR_FLAG0(pred_weight_req) |
			  AVD_OP_WEIGHTS_HDR_LUMA(weights->luma_log2_weight_denom) |
			  AVD_OP_WEIGHTS_HDR_CHROMA(weights->chroma_log2_weight_denom));

	if (!pred_weight_req)
		return;

	luma_denom = 1 << weights->luma_log2_weight_denom;
	chroma_denom = 1 << weights->chroma_log2_weight_denom;

	for (int list = 0; list < 2; list++) {
		const struct v4l2_h264_weight_factors *f =
			&weights->weight_factors[list];
		unsigned int num;

		if (list == 1 && sl->slice_type != V4L2_H264_SLICE_TYPE_B)
			break;

		num = (list ? sl->num_ref_idx_l1_active_minus1 :
			      sl->num_ref_idx_l0_active_minus1) + 1;

		/* Default weights must not be sent: they cause artifacts (asahi avd). */
		for (unsigned int i = 0; i < num; i++) {
			if (f->luma_weight[i] != luma_denom || f->luma_offset[i])
				stream_weight(ctx, list, i, 1, f->luma_weight[i],
					      f->luma_offset[i]);

			if (f->chroma_weight[i][0] != chroma_denom ||
			    f->chroma_offset[i][0] ||
			    f->chroma_weight[i][1] != chroma_denom ||
			    f->chroma_offset[i][1]) {
				stream_weight(ctx, list, i, 2,
					      f->chroma_weight[i][0],
					      f->chroma_offset[i][0]);
				stream_weight(ctx, list, i, 3,
					      f->chroma_weight[i][1],
					      f->chroma_offset[i][1]);
			}
		}
	}
}

/* header_bit_size excludes emulation prevention bytes; skip them in the NAL. */
static int slice_data_offset(const u8 *data, u32 len, u32 header_bytes,
			     u32 *offset)
{
	u32 off = 2, bytes_read = 2;

	while (bytes_read < header_bytes) {
		if (off >= len)
			return -EINVAL;
		if (data[off - 2] || data[off - 1] || data[off] != 0x03)
			bytes_read++;
		off++;
	}

	if (off >= len)
		return -EINVAL;

	*offset = off;
	return 0;
}

static int stream_slice(struct avd_ctx *ctx, struct avd_h264_run *run,
			struct avd_buf *slice, u32 len)
{
	const struct v4l2_ctrl_h264_decode_params *decode = run->decode_params;
	const struct v4l2_ctrl_h264_pps *pps = run->pps;
	const struct v4l2_ctrl_h264_sps *sps = run->sps;
	const struct v4l2_ctrl_h264_slice_params *sl = run->slice_params;
	bool cavlc = !(pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE);
	u32 mbs_w = sps->pic_width_in_mbs_minus1 + 1;
	dma_addr_t addr;
	u32 off;
	int ret;

	ret = slice_data_offset(slice->cpu, len,
				(sl->header_bit_size + (cavlc ? 0 : 7)) / 8,
				&off);
	if (ret)
		return ret;

	addr = slice->addr + off;
	push(ctx, AVD_OP_CODED_DATA |
		  AVD_OP_CODED_DATA_BIT_OFF(cavlc ? sl->header_bit_size % 8 : 0) |
		  AVD_OP_CODED_DATA_ADDR(upper_32_bits(addr)));
	push(ctx, lower_32_bits(addr));
	push(ctx, len - off);

	push(ctx, AVD_OP_SL_LOC | AVD_OP_SL_LOC_Y(sl->first_mb_in_slice / mbs_w) |
		  AVD_OP_SL_LOC_X(sl->first_mb_in_slice % mbs_w));
	push(ctx, AVD_OP_QP | AVD_OP_QP_VAL(26 + pps->pic_init_qp_minus26 +
					    sl->slice_qp_delta));
	push(ctx, AVD_OP_DBLK |
		  AVD_OP_DBLK_FLAG_FULL_EN(sl->disable_deblocking_filter_idc == 0) |
		  AVD_OP_DBLK_FLAG_EN(sl->disable_deblocking_filter_idc != 1) |
		  AVD_OP_DBLK_OFF1(sl->slice_beta_offset_div2) |
		  AVD_OP_DBLK_OFF0(sl->slice_alpha_c0_offset_div2));

	if (sl->slice_type == V4L2_H264_SLICE_TYPE_P ||
	    sl->slice_type == V4L2_H264_SLICE_TYPE_B) {
		for (u32 i = 0; i <= sl->num_ref_idx_l0_active_minus1; i++)
			push(ctx, AVD_OP_REF | AVD_OP_REF_LIST_IDX(0) |
				  AVD_OP_REF_LOOP_IDX(i) |
				  AVD_OP_REF_DBP_IDX(sl->ref_pic_list0[i].index));

		if (sl->slice_type == V4L2_H264_SLICE_TYPE_B)
			for (u32 i = 0; i <= sl->num_ref_idx_l1_active_minus1; i++)
				push(ctx, AVD_OP_REF | AVD_OP_REF_LIST_IDX(1) |
					  AVD_OP_REF_LOOP_IDX(i) |
					  AVD_OP_REF_DBP_IDX(sl->ref_pic_list1[i].index));

		stream_weights(ctx, run);
	}

	if (sl->first_mb_in_slice == 0) {
		push(ctx, AVD_OP_SL_DIM_START);
		push(ctx, AVD_SL_DIM_END_Y(sps->pic_height_in_map_units_minus1) |
			  AVD_SL_DIM_END_X(sps->pic_width_in_mbs_minus1));
	}

	push(ctx, AVD_OP_SL_REF |
		  AVD_OP_SL_REF_FLAG_CABAC(sl->cabac_init_idc == 1) |
		  AVD_OP_SL_REF_FLAG1(sl->cabac_init_idc == 2) |
		  AVD_OP_SL_REF_FLAG2(!(sl->flags &
					V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED)) |
		  AVD_OP_SL_REF_NUM_L0(sl->num_ref_idx_l0_active_minus1) |
		  AVD_OP_SL_REF_NUM_L1(sl->num_ref_idx_l1_active_minus1) |
		  AVD_OP_SL_REF_SLICE_P(sl->slice_type == V4L2_H264_SLICE_TYPE_P) |
		  AVD_OP_SL_REF_SLICE_I(sl->slice_type == V4L2_H264_SLICE_TYPE_I) |
		  AVD_OP_SL_REF_SLICE_B(sl->slice_type == V4L2_H264_SLICE_TYPE_B));

	if (sl->slice_type == V4L2_H264_SLICE_TYPE_B) {
		const struct v4l2_h264_dpb_entry *col =
			&decode->dpb[sl->ref_pic_list1[0].index];
		struct avd_decoded_buffer *ref;

		ref = avd_get_ref_buf(ctx, &run->base, col->reference_ts);
		push_address(ctx, avd_buf_addr(ref) + ref->layout.mv_offset);
	}

	return 0;
}

static int avd_h264_alloc_bufs(struct avd_ctx *ctx)
{
	struct avd_dev *avd = ctx->dev;
	struct avd_h264_ctx *h264_ctx = ctx->priv;
	u32 mb = DIV_ROUND_UP(ctx->decoded_fmt.fmt.pix_mp.width, 16);
	int ret;

	/* Sizes for 8-bit pictures, from the asahi avd driver. */
	ret = avd_buf_alloc(avd, &h264_ctx->pipe_state, 0x200);
	if (!ret)
		ret = avd_buf_alloc(avd, &h264_ctx->above_info, mb * 20);
	if (!ret)
		ret = avd_buf_alloc(avd, &h264_ctx->ip_above, 8 * 4 * mb);
	if (!ret)
		ret = avd_buf_alloc(avd, &h264_ctx->lf_above, 8 * 4 * 4 * mb);
	if (!ret)
		ret = avd_buf_alloc(avd, &h264_ctx->lf_above_info, 32 * mb);
	if (!ret)
		ret = avd_buf_alloc(avd, &h264_ctx->mv_above_info, 32 * mb);
	if (!ret)
		ret = avd_buf_alloc(avd, &h264_ctx->bitstream,
				    ctx->coded_fmt.fmt.pix_mp.plane_fmt[0].sizeimage);

	return ret;
}

static void avd_h264_free_bufs(struct avd_ctx *ctx)
{
	struct avd_h264_ctx *h264_ctx = ctx->priv;
	struct avd_dev *avd = ctx->dev;

	avd_buf_free(avd, &h264_ctx->pipe_state);
	avd_buf_free(avd, &h264_ctx->above_info);
	avd_buf_free(avd, &h264_ctx->lf_above_info);
	avd_buf_free(avd, &h264_ctx->lf_above);
	avd_buf_free(avd, &h264_ctx->ip_above);
	avd_buf_free(avd, &h264_ctx->mv_above_info);

	avd_buf_free(avd, &h264_ctx->bitstream);
}

static int avd_h264_validate_sps(const struct v4l2_ctrl_h264_sps *sps)
{
	if (sps->chroma_format_idc != 1 || sps->bit_depth_luma_minus8 ||
	    sps->bit_depth_chroma_minus8 ||
	    !(sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY))
		return -EINVAL;

	return 0;
}

static int avd_h264_start(struct avd_ctx *ctx)
{
	struct avd_h264_ctx *h264_ctx;
	int ret;

	h264_ctx = kzalloc_obj(*h264_ctx);
	if (!h264_ctx)
		return -ENOMEM;
	ctx->priv = h264_ctx;

	ret = avd_h264_alloc_bufs(ctx);
	if (ret) {
		avd_h264_free_bufs(ctx);
		kfree(h264_ctx);
	}

	return ret;
}

static void avd_h264_stop(struct avd_ctx *ctx)
{
	avd_h264_free_bufs(ctx);
	kfree(ctx->priv);
}

static void avd_h264_get_ctrls(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	run->decode_params = avd_ctrl_ptr(ctx, V4L2_CID_STATELESS_H264_DECODE_PARAMS);
	run->sps = avd_ctrl_ptr(ctx, V4L2_CID_STATELESS_H264_SPS);
	run->pps = avd_ctrl_ptr(ctx, V4L2_CID_STATELESS_H264_PPS);
	run->slice_params = avd_ctrl_ptr(ctx, V4L2_CID_STATELESS_H264_SLICE_PARAMS);
	run->scaling_matrix = avd_ctrl_ptr(ctx, V4L2_CID_STATELESS_H264_SCALING_MATRIX);
	run->pred_weights = avd_ctrl_ptr(ctx, V4L2_CID_STATELESS_H264_PRED_WEIGHTS);
}

static int avd_h264_validate_run(struct avd_ctx *ctx, struct avd_h264_run *run)
{
	const struct v4l2_ctrl_h264_slice_params *sl = run->slice_params;
	const struct v4l2_ctrl_h264_sps *sps = run->sps;
	const struct v4l2_pix_format_mplane *pix_mp = &ctx->decoded_fmt.fmt.pix_mp;
	u32 mbs = (sps->pic_width_in_mbs_minus1 + 1) *
		  (sps->pic_height_in_map_units_minus1 + 1);
	u32 width = (sps->pic_width_in_mbs_minus1 + 1) * 16;
	u32 height = (sps->pic_height_in_map_units_minus1 + 1) * 16;

	if (avd_h264_validate_sps(sps) || run->pps->num_slice_groups_minus1)
		return -EINVAL;
	if (width < 64 || height < 64 || width > pix_mp->width ||
	    height > pix_mp->height)
		return -EINVAL;
	if (sl->first_mb_in_slice >= mbs ||
	    sl->num_ref_idx_l0_active_minus1 > 15 ||
	    sl->num_ref_idx_l1_active_minus1 > 15)
		return -EINVAL;

	if (sl->slice_type == V4L2_H264_SLICE_TYPE_P ||
	    sl->slice_type == V4L2_H264_SLICE_TYPE_B) {
		for (u32 i = 0; i <= sl->num_ref_idx_l0_active_minus1; i++)
			if (sl->ref_pic_list0[i].index >= V4L2_H264_NUM_DPB_ENTRIES)
				return -EINVAL;
	}
	if (sl->slice_type == V4L2_H264_SLICE_TYPE_B) {
		for (u32 i = 0; i <= sl->num_ref_idx_l1_active_minus1; i++)
			if (sl->ref_pic_list1[i].index >= V4L2_H264_NUM_DPB_ENTRIES)
				return -EINVAL;
	}

	return 0;
}

static int avd_h264_run(struct avd_ctx *ctx)
{
	struct avd_h264_ctx *h264_ctx = ctx->priv;
	struct v4l2_h264_reflist_builder builder;
	struct avd_h264_run run;
	struct avd_buf slice;
	const void *data;
	size_t pos;
	u32 len;
	int ret;

	ret = avd_run_preamble(ctx, &run.base);
	if (ret)
		return ret;
	avd_h264_get_ctrls(ctx, &run);

	ret = avd_h264_validate_run(ctx, &run);
	if (ret)
		return ret;

	len = vb2_get_plane_payload(&run.base.src->vb2_buf, 0);
	data = vb2_plane_vaddr(&run.base.src->vb2_buf, 0);
	if (!data || !len)
		return -EINVAL;

	if (ctx->fh.m2m_ctx->new_frame) {
		h264_ctx->bitstream_used = 0;

		v4l2_h264_init_reflist_builder(&builder, run.decode_params,
					       run.sps, run.decode_params->dpb);
		run.num_valid = builder.num_valid;
		run.cur_poc = builder.cur_pic_order_count;

		ret = avd_job_begin(ctx, 2 + run.sps->pic_height_in_map_units_minus1);
		if (ret)
			return ret;
		stream_hdr(ctx, &run);
	} else if (!ctx->job.segments) {
		return -EINVAL;
	}

	pos = ALIGN(h264_ctx->bitstream_used, AVD_ALIGN);
	if (pos + len > h264_ctx->bitstream.size)
		return -ENOSPC;
	slice.cpu = h264_ctx->bitstream.cpu + pos;
	slice.addr = h264_ctx->bitstream.addr + pos;
	memcpy(slice.cpu, data, len);
	h264_ctx->bitstream_used = pos + len;

	ret = avd_job_next(ctx);
	if (ret)
		return ret;
	ret = stream_slice(ctx, &run, &slice, len);
	if (ret)
		return ret;

	if (run.base.src->flags & V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF) {
		avd_job_finish(ctx, VB2_BUF_STATE_DONE);
		return 0;
	}

	return avd_submit_job(ctx, AVD_CODEC_H264);
}

static void avd_h264_done(struct avd_ctx *ctx, enum vb2_buffer_state result)
{
	if (result == VB2_BUF_STATE_ERROR)
		avd_job_free(ctx);
}

static int avd_h264_try_ctrl(struct v4l2_ctrl *ctrl)
{
	if (ctrl->id == V4L2_CID_STATELESS_H264_SPS)
		return avd_h264_validate_sps(ctrl->p_new.p_h264_sps);
	if (ctrl->id == V4L2_CID_STATELESS_H264_PPS &&
	    ctrl->p_new.p_h264_pps->num_slice_groups_minus1)
		return -EINVAL;

	return 0;
}

static void avd_h264_submit(struct avd_ctx *ctx)
{
	avd_submit_pp(ctx, 1);
}

const struct avd_coded_fmt_ops avd_h264_fmt_ops = {
	.mv_size = avd_h264_mv_size,
	.start = avd_h264_start,
	.stop = avd_h264_stop,
	.run = avd_h264_run,
	.submit = avd_h264_submit,
	.done = avd_h264_done,
	.try_ctrl = avd_h264_try_ctrl,
};
