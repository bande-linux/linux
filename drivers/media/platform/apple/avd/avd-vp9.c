// SPDX-License-Identifier: GPL-2.0
/*
 * Apple AVD VP9 decoding
 *
 * Copyright The Asahi Linux Contributors
 * Copyright 2023 Eileen Yoon <eyn@gmx.com>
 *
 * Copyright (C) 2019 Collabora, Ltd.
 *	Boris Brezillon <boris.brezillon@collabora.com>
 * Copyright (C) 2021 Collabora, Ltd.
 *	Andrzej Pietrasiewicz <andrzej.p@collabora.com>
 *
 * Copyright (C) 2016 Rockchip Electronics Co., Ltd.
 *	Alpha Lin <Alpha.Lin@rock-chips.com>
 */

#include <linux/unaligned.h>

#include <media/v4l2-vp9.h>
#include <media/videobuf2-dma-contig.h>

#include "avd.h"
#include "avd-inst.h"

#define VP9_Q_IDX(v)			FIELD_PREP(GENMASK(31, 15), v)
#define VP9_Q_DC_Y(v)			FIELD_PREP(GENMASK(14, 10), v)
#define VP9_Q_DC_UV(v)			FIELD_PREP(GENMASK(9, 5), v)
#define VP9_Q_AC_UV(v)			FIELD_PREP(GENMASK(4, 0), v)

#define VP9_LF_SHARPNESS(v)		FIELD_PREP(GENMASK(31, 28), v)
#define VP9_LF_REF0(v)			FIELD_PREP(GENMASK(27, 21), v)
#define VP9_LF_REF1(v)			FIELD_PREP(GENMASK(20, 14), v)
#define VP9_LF_REF2(v)			FIELD_PREP(GENMASK(13, 7), v)
#define VP9_LF_REF3(v)			FIELD_PREP(GENMASK(6, 0), v)

#define VP9_LF_LV(v)			FIELD_PREP(GENMASK(31, 14), v)
#define VP9_LF_MODE0(v)			FIELD_PREP(GENMASK(13, 7), v)
#define VP9_LF_MODE1(v)			FIELD_PREP(GENMASK(6, 0), v)

#define VP9_FEAT_LVL_ALT_Q_EN(v)	FIELD_PREP(BIT(21), !!(v))
#define VP9_FEAT_LVL_ALT_Q(v)		FIELD_PREP(GENMASK(20, 12), v)
#define VP9_FEAT_LVL_ALT_L_EN(v)	FIELD_PREP(BIT(11), !!(v))
#define VP9_FEAT_LVL_ALT_L(v)		FIELD_PREP(GENMASK(10, 4), v)
#define VP9_FEAT_LVL_REF_FRAME_EN(v)	FIELD_PREP(BIT(3), !!(v))
#define VP9_FEAT_LVL_REF_FRAME(v)	FIELD_PREP(GENMASK(2, 1), v)
#define VP9_FEAT_LVL_SKIP_EN(v)		FIELD_PREP(BIT(0), !!(v))

#define VP9_REF_SEL_LAST(v)		FIELD_PREP(GENMASK(2, 0), v)
#define VP9_REF_BIAS_LAST(v)		FIELD_PREP(BIT(3), !!(v))
#define VP9_REF_SEL_GOLDEN(v)		FIELD_PREP(GENMASK(6, 4), v)
#define VP9_REF_BIAS_GOLDEN(v)		FIELD_PREP(BIT(7), !!(v))
#define VP9_REF_SEL_ALT(v)		FIELD_PREP(GENMASK(10, 8), v)
#define VP9_REF_BIAS_ALT(v)		FIELD_PREP(BIT(11), !!(v))
#define VP9_REFERENCE_MODE(v)		FIELD_PREP(GENMASK(13, 12), v)
#define VP9_PARALLEL_DEC(v)		FIELD_PREP(BIT(14), !!(v))
#define VP9_REFRESH_CTX(v)		FIELD_PREP(BIT(15), !!(v))
#define VP9_INTERP_FILTER(v)		FIELD_PREP(GENMASK(18, 16), v)
#define VP9_HIGH_PREC_MV(v)		FIELD_PREP(BIT(19), !!(v))
#define VP9_ERR_RES(v)			FIELD_PREP(BIT(20), !!(v))
#define VP9_HAS_REF(v)			FIELD_PREP(BIT(21), !!(v))
#define VP9_SEG_ABS(v)			FIELD_PREP(BIT(22), !!(v))
#define VP9_SEG_UDATA_TEMP(v)		FIELD_PREP(BIT(23), !!(v))
#define VP9_SEG_UPDATE_MAP(v)		FIELD_PREP(BIT(24), !!(v))
#define VP9_SEG_ENABLED(v)		FIELD_PREP(BIT(25), !!(v))
#define VP9_SEG_RESET(v)		FIELD_PREP(BIT(26), !!(v))

#define VP9_MAX_TILE_COLS		16
#define VP9_REF_SCALE_SHIFT		14
#define VP9_LAST_FRAME			1
#define VP9_GOLDEN_FRAME		2
#define VP9_ALTREF_FRAME		3

/* Probability and count tables in the decoder's layout (asahi avd). */
struct avd_vp9_probs {
	struct {
		u8 tree_probs[7];
		u8 pred_probs[3];
	} seg;
	u8 tx8[2][1];
	u8 tx16[2][2];
	u8 tx32[2][3];
	/* [4][2][2][6][(k == 0) ? 3 : 6][3] */
	u8 coef[1584];
	u8 skip[3];
	u8 inter_mode[7][3];
	u8 interp_filter[4][2];
	u8 is_inter[4];
	u8 comp_mode[5];
	u8 single_ref[5][2];
	u8 comp_ref[5];
	u8 y_mode[4][9];
	u8 uv_mode[10][9];
	u8 partition[16][3];
	u8 joint[3];
	struct {
		u8 sign;
		u8 classes[10];
		u8 class0_bit;
		u8 bits[10];
	} mv_comp[2];
	struct {
		u8 class0_fr[2][3];
		u8 fr[3];
	} mv_fr[2];
	struct {
		u8 class0_hp;
		u8 hp;
	} mv_hp[2];
};

static_assert(sizeof(struct avd_vp9_probs) == 1905);

struct avd_vp9_frame_symbol_counts {
	u32 padding;
	u32 tx8p[2][2];
	u32 tx16p[2][3];
	u32 tx32p[2][4];
	/* [4][2][2][6][(k == 0) ? 3 : 6] */
	u32 eob_0[528];
	/* { u32 coef[3]; u32 eob_1; } [4][2][2][6][(k == 0) ? 3 : 6] */
	u32 ref_cnt[2112];
	u32 skip[3][2];
	u32 mv_mode[7][4];
	u32 filter[4][3];
	u32 intra_inter[4][2];
	u32 comp[5][2];
	u32 single_ref[5][2][2];
	u32 comp_ref[5][2];
	u32 y_mode[4][10];
	u32 uv_mode[10][10];
	u32 partition[16][4];
	u32 mv_joint[4];
	struct {
		u32 sign[2];
		u32 classes[11];
		u32 class0[2];
		u32 bits[10][2];
	} mv_comp[2];
	struct {
		u32 class0_fr[2][4];
		u32 fr[4];
	} mv_fr[2];
	struct {
		u32 class0_hp[2];
		u32 hp[2];
	} mv_hp[2];
};

static_assert(sizeof(struct avd_vp9_frame_symbol_counts) == 12252);

struct avd_vp9_frame_info {
	bool valid;
	u8 frame_context_idx;
	u8 reference_mode;
	u8 tx_mode;
	u8 interpolation_filter;
	u32 flags;
	u16 width;
	u16 height;
	struct v4l2_vp9_segmentation seg;
};

struct avd_vp9_run {
	struct avd_run base;

	const struct v4l2_ctrl_vp9_frame *frame;
	const struct v4l2_ctrl_vp9_compressed_hdr *prob_updates;
};

struct avd_vp9_ctx {
	struct v4l2_vp9_frame_symbol_counts cnts;
	struct v4l2_vp9_frame_context probability_tables;
	struct v4l2_vp9_frame_context frame_context[4];

	struct avd_buf az_left;
	struct avd_buf above_info;
	struct avd_buf ip_above;
	struct avd_buf lf_above;
	struct avd_buf lf_left_info;
	struct avd_buf lf_left;
	struct avd_buf color;
	struct avd_buf seg;
	struct avd_buf pipe_state;
	struct avd_buf counts;
	struct avd_buf probs;

	struct avd_vp9_frame_info cur;
	struct avd_vp9_frame_info last;
	unsigned int submit_num;
};

static u32 avd_vp9_mv_size(u32 w, u32 h)
{
	return 0;
}

static void set_refs(struct avd_ctx *ctx, struct avd_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame *frame = run->frame;
	struct avd_decoded_buffer *dst, *ref[3];
	u32 xscale, yscale;
	dma_addr_t addr;

	dst = vb2_to_avd_decoded_buf(&run->base.dst->vb2_buf);
	ref[0] = avd_get_ref_buf(ctx, &run->base, frame->last_frame_ts);
	ref[1] = avd_get_ref_buf(ctx, &run->base, frame->golden_frame_ts);
	ref[2] = avd_get_ref_buf(ctx, &run->base, frame->alt_frame_ts);

	push(ctx, 0);
	push(ctx, 0);
	push(ctx, 0);

	for (int i = 0; i < ARRAY_SIZE(ref); i++) {
		if (!ref[i]->vp9.width || ref[i]->error)
			ref[i] = dst;

		addr = avd_buf_addr(ref[i]) +
		       ref[i]->layout.comp_offset;

		push(ctx, AVD_REF_FLAG_CONST);
		push(ctx, AVD_HDR_HEIGHT(ref[i]->vp9.height - 1) |
			  AVD_HDR_WIDTH(ref[i]->vp9.width - 1));

		xscale = (ref[i]->vp9.width << VP9_REF_SCALE_SHIFT) /
			 dst->vp9.width;
		yscale = (ref[i]->vp9.height << VP9_REF_SCALE_SHIFT) /
			 dst->vp9.height;
		push(ctx, AVD_HDR_HEIGHT(xscale) | AVD_HDR_WIDTH(yscale));

		push_comp(ctx, addr, &ref[i]->layout.comp);
	}
}

static u32 make_flags1(struct avd_ctx *ctx, struct avd_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame *frame = run->frame;
	struct avd_vp9_ctx *vp9_ctx = ctx->priv;
	const struct avd_vp9_frame_info *last = &vp9_ctx->last;
	struct avd_decoded_buffer *lastb, *golden, *alt;
	u32 flags;

	lastb = avd_get_ref_buf(ctx, &run->base, frame->last_frame_ts);
	golden = avd_get_ref_buf(ctx, &run->base, frame->golden_frame_ts);
	alt = avd_get_ref_buf(ctx, &run->base, frame->alt_frame_ts);

	flags = VP9_REF_SEL_LAST(VP9_LAST_FRAME);
	flags |= VP9_REF_BIAS_LAST(frame->ref_frame_sign_bias &
				   V4L2_VP9_SIGN_BIAS_LAST);
	flags |= VP9_REF_SEL_GOLDEN(golden == lastb ? VP9_LAST_FRAME :
						      VP9_GOLDEN_FRAME);
	flags |= VP9_REF_BIAS_GOLDEN(frame->ref_frame_sign_bias &
				     V4L2_VP9_SIGN_BIAS_GOLDEN);
	flags |= VP9_REF_SEL_ALT(alt != golden ? VP9_ALTREF_FRAME :
				 alt != lastb ? VP9_GOLDEN_FRAME :
						VP9_LAST_FRAME);
	flags |= VP9_REF_BIAS_ALT(frame->ref_frame_sign_bias &
				  V4L2_VP9_SIGN_BIAS_ALT);
	flags |= VP9_REFERENCE_MODE(frame->reference_mode);
	flags |= VP9_PARALLEL_DEC(frame->flags &
				  V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE);
	flags |= VP9_REFRESH_CTX(frame->flags &
				 V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX);
	flags |= VP9_INTERP_FILTER(frame->interpolation_filter);
	flags |= VP9_HIGH_PREC_MV(frame->flags &
				  V4L2_VP9_FRAME_FLAG_ALLOW_HIGH_PREC_MV);
	flags |= VP9_ERR_RES(frame->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT);
	flags |= VP9_SEG_ABS(frame->seg.flags &
			     V4L2_VP9_SEGMENTATION_FLAG_ABS_OR_DELTA_UPDATE);
	flags |= VP9_SEG_UDATA_TEMP((frame->seg.flags &
				     V4L2_VP9_SEGMENTATION_FLAG_UPDATE_DATA) &&
				    (frame->seg.flags &
				     V4L2_VP9_SEGMENTATION_FLAG_TEMPORAL_UPDATE));
	flags |= VP9_SEG_UPDATE_MAP(frame->seg.flags &
				    V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP);
	flags |= VP9_SEG_ENABLED(frame->seg.flags &
				 V4L2_VP9_SEGMENTATION_FLAG_ENABLED);

	/* Motion vectors of the previous frame are usable (6.1.2 UsePrevFrameMvs). */
	flags |= VP9_HAS_REF(!(frame->flags & V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT) &&
			     !(frame->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) &&
			     last->valid &&
			     last->width == frame->frame_width_minus_1 + 1 &&
			     last->height == frame->frame_height_minus_1 + 1 &&
			     (last->flags & V4L2_VP9_FRAME_FLAG_SHOW_FRAME) &&
			     !(last->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME));

	flags |= VP9_SEG_RESET((frame->seg.flags &
				V4L2_VP9_SEGMENTATION_FLAG_ENABLED) &&
			       ((frame->flags & V4L2_VP9_FRAME_FLAG_KEY_FRAME) ||
				(last->valid &&
				 !(last->seg.flags &
				   V4L2_VP9_SEGMENTATION_FLAG_ENABLED) &&
				 (last->flags & (V4L2_VP9_FRAME_FLAG_KEY_FRAME |
						 V4L2_VP9_FRAME_FLAG_INTRA_ONLY)))));

	return flags;
}

static u32 seg_features(const struct v4l2_vp9_segmentation *seg,
			unsigned int segid)
{
	u32 val;

	val = VP9_FEAT_LVL_ALT_Q_EN(v4l2_vp9_seg_feat_enabled(seg->feature_enabled,
							       V4L2_VP9_SEG_LVL_ALT_Q,
							       segid));
	val |= VP9_FEAT_LVL_ALT_Q(seg->feature_data[segid][V4L2_VP9_SEG_LVL_ALT_Q]);
	val |= VP9_FEAT_LVL_ALT_L_EN(v4l2_vp9_seg_feat_enabled(seg->feature_enabled,
								V4L2_VP9_SEG_LVL_ALT_L,
								segid));
	val |= VP9_FEAT_LVL_ALT_L(seg->feature_data[segid][V4L2_VP9_SEG_LVL_ALT_L]);
	val |= VP9_FEAT_LVL_REF_FRAME_EN(v4l2_vp9_seg_feat_enabled(seg->feature_enabled,
								    V4L2_VP9_SEG_LVL_REF_FRAME,
								    segid));
	val |= VP9_FEAT_LVL_REF_FRAME(seg->feature_data[segid][V4L2_VP9_SEG_LVL_REF_FRAME]);
	val |= VP9_FEAT_LVL_SKIP_EN(v4l2_vp9_seg_feat_enabled(seg->feature_enabled,
							       V4L2_VP9_SEG_LVL_SKIP,
							       segid));

	return val;
}

static void set_header(struct avd_ctx *ctx, struct avd_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame *frame = run->frame;
	struct avd_vp9_ctx *vp9_ctx = ctx->priv;
	u8 tx_mode = run->prob_updates->tx_mode;
	bool intra_only = frame->flags & (V4L2_VP9_FRAME_FLAG_KEY_FRAME |
					  V4L2_VP9_FRAME_FLAG_INTRA_ONLY);
	bool lf_delta = frame->lf.flags & V4L2_VP9_LOOP_FILTER_FLAG_DELTA_ENABLED;
	u32 size = AVD_HDR_HEIGHT(frame->frame_height_minus_1) |
		   AVD_HDR_WIDTH(frame->frame_width_minus_1);
	u32 bytesperline;

	push(ctx, AVD_OP_HDR | AVD_OP_HDR_FLAG_DECOMP(1) |
		  AVD_OP_HDR_FLAG_INTRA(intra_only) | AVD_OP_HDR_CONST |
		  AVD_OP_HDR_FLAG_PIPE_STATE(1));
	push(ctx, AVD_HDR_CODEC_MODE(AVD_CODEC_VP9));
	push(ctx, size);
	push(ctx, 0);
	push(ctx, size);
	push(ctx, AVD_HDR_COMMON_CHROMA_FORMAT(1) |
		  AVD_HDR_COMMON_BIT_DEPTH_C(frame->profile) |
		  AVD_HDR_COMMON_BIT_DEPTH_L(frame->bit_depth - 8) |
		  AVD_HDR_COMMON_LUMA_CBS(3) |
		  AVD_HDR_COMMON_LUMA_TBS(min_t(u8, tx_mode, 3)) |
		  AVD_HDR_COMMON_FLAG0(tx_mode & V4L2_VP9_TX_MODE_SELECT));
	push(ctx, make_flags1(ctx, run));

	for (int i = 0; i < 8; i++)
		push(ctx, seg_features(&vp9_ctx->cur.seg, i));

	push(ctx, AVD_HDR_FEAT_VP9);
	push(ctx, 0);
	push(ctx, 0);

	push_address(ctx, vp9_ctx->counts.addr);
	push_address(ctx, vp9_ctx->probs.addr);
	push_address(ctx, vp9_ctx->above_info.addr);
	push_address(ctx, vp9_ctx->seg.addr);
	push_address(ctx, vp9_ctx->seg.addr);
	push_address(ctx, vp9_ctx->color.addr);
	push_address(ctx, vp9_ctx->color.addr);

	push(ctx, VP9_Q_IDX(frame->quant.base_q_idx) |
		  VP9_Q_DC_Y(frame->quant.delta_q_y_dc) |
		  VP9_Q_DC_UV(frame->quant.delta_q_uv_dc) |
		  VP9_Q_AC_UV(frame->quant.delta_q_uv_ac));
	push(ctx, VP9_LF_SHARPNESS(frame->lf.sharpness) |
		  (lf_delta ? VP9_LF_REF0(frame->lf.ref_deltas[0]) |
			      VP9_LF_REF1(frame->lf.ref_deltas[1]) |
			      VP9_LF_REF2(frame->lf.ref_deltas[2]) |
			      VP9_LF_REF3(frame->lf.ref_deltas[3]) : 0));
	push(ctx, VP9_LF_LV(frame->lf.level) |
		  (lf_delta ? VP9_LF_MODE0(frame->lf.mode_deltas[0]) |
			      VP9_LF_MODE1(frame->lf.mode_deltas[1]) : 0));

	push(ctx, 0);
	push(ctx, 0);

	push_address(ctx, vp9_ctx->pipe_state.addr);
	push_address(ctx, vp9_ctx->ip_above.addr);
	push_address(ctx, vp9_ctx->lf_above.addr);
	push_address(ctx, 0);
	push_address(ctx, vp9_ctx->lf_left.addr);
	push_address(ctx, vp9_ctx->lf_left_info.addr);
	push_address(ctx, vp9_ctx->az_left.addr);
	push(ctx, 0);

	push_comp(ctx, run->base.comp_out, &ctx->layout.comp);
	push_address(ctx, 0);

	bytesperline = ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline >> 4;
	push_address(ctx, run->base.y_out);
	push(ctx, bytesperline);
	push_address(ctx, run->base.uv_out);
	push(ctx, bytesperline);
	push(ctx, 0);
	push(ctx, size);

	if (!intra_only)
		set_refs(ctx, run);
}

static int set_tiles(struct avd_ctx *ctx, struct avd_vp9_run *run)
{
	const struct v4l2_ctrl_vp9_frame *frame = run->frame;
	struct avd_vp9_ctx *vp9_ctx = ctx->priv;
	const u8 *data = vb2_plane_vaddr(&run->base.src->vb2_buf, 0);
	u32 payload = vb2_get_plane_payload(&run->base.src->vb2_buf, 0);
	u32 rows = 1 << frame->tile_rows_log2;
	u32 cols = 1 << frame->tile_cols_log2;
	/* 7.4.3: MiCols and MiRows rounded to 64x64 superblocks. */
	u32 sb_cols = (((frame->frame_width_minus_1 + 8) >> 3) + 7) >> 3;
	u32 sb_rows = (((frame->frame_height_minus_1 + 8) >> 3) + 7) >> 3;
	u32 offset, size, tile_size;
	dma_addr_t addr;
	int ret;

	if (!data)
		return -EINVAL;

	offset = frame->uncompressed_header_size + frame->compressed_header_size;
	if (offset >= payload || cols > sb_cols || rows > sb_rows)
		return -EINVAL;
	size = payload - offset;

	vp9_ctx->submit_num = 0;
	for (u32 row = 0; row < rows; row++) {
		for (u32 col = 0; col < cols; col++) {
			if (row == rows - 1 && col == cols - 1) {
				tile_size = size;
			} else {
				if (size < 4)
					return -EINVAL;
				tile_size = get_unaligned_be32(&data[offset]);
				offset += 4;
				size -= 4;
				if (!tile_size || tile_size > size)
					return -EINVAL;
			}

			ret = avd_job_next(ctx);
			if (ret)
				return ret;

			addr = run->base.coded_in + offset;
			push(ctx, AVD_OP_CODED_DATA |
				  AVD_OP_CODED_DATA_ADDR(upper_32_bits(addr)));
			push(ctx, lower_32_bits(addr));
			push(ctx, tile_size);
			push(ctx, AVD_OP_SL_DIM_START |
				  AVD_OP_SL_DIM_START_Y(row * sb_rows / rows) |
				  AVD_OP_SL_DIM_START_X(col * sb_cols / cols));
			push(ctx, AVD_SL_DIM_END_COL(col) |
				  AVD_SL_DIM_END_Y((row + 1) * sb_rows / rows - 1) |
				  AVD_SL_DIM_END_X((col + 1) * sb_cols / cols - 1));

			offset += tile_size;
			size -= tile_size;
			vp9_ctx->submit_num++;
		}
	}

	return 0;
}

static void copy_vp9_frame_mv(struct avd_vp9_probs *avd_probs,
			      const struct v4l2_vp9_frame_context *probs)
{
	memcpy(avd_probs->joint, probs->mv.joint, sizeof(avd_probs->joint));
	for (int i = 0; i < 2; i++) {
		avd_probs->mv_comp[i].sign = probs->mv.sign[i];
		memcpy(avd_probs->mv_comp[i].classes, probs->mv.classes[i],
		       sizeof(avd_probs->mv_comp[i].classes));
		avd_probs->mv_comp[i].class0_bit = probs->mv.class0_bit[i];
		memcpy(avd_probs->mv_comp[i].bits, probs->mv.bits[i],
		       sizeof(avd_probs->mv_comp[i].bits));
		memcpy(avd_probs->mv_fr[i].class0_fr, probs->mv.class0_fr[i],
		       sizeof(avd_probs->mv_fr[i].class0_fr));
		memcpy(avd_probs->mv_fr[i].fr, probs->mv.fr[i],
		       sizeof(avd_probs->mv_fr[i].fr));
		avd_probs->mv_hp[i].class0_hp = probs->mv.class0_hp[i];
		avd_probs->mv_hp[i].hp = probs->mv.hp[i];
	}
}

static void init_probs(struct avd_ctx *ctx, const struct avd_vp9_run *run)
{
	struct avd_vp9_ctx *vp9_ctx = ctx->priv;
	struct avd_vp9_probs *avd_probs = vp9_ctx->probs.cpu;
	const struct v4l2_vp9_frame_context *probs = &vp9_ctx->probability_tables;
	const struct v4l2_vp9_segmentation *seg = &run->frame->seg;
	bool intra_only = run->frame->flags & (V4L2_VP9_FRAME_FLAG_KEY_FRAME |
					       V4L2_VP9_FRAME_FLAG_INTRA_ONLY);
	int count = 0;

	memset(avd_probs, 0, sizeof(*avd_probs));

	memcpy(avd_probs->tx8, probs->tx8, sizeof(avd_probs->tx8));
	memcpy(avd_probs->tx16, probs->tx16, sizeof(avd_probs->tx16));
	memcpy(avd_probs->tx32, probs->tx32, sizeof(avd_probs->tx32));
	memcpy(avd_probs->skip, probs->skip, sizeof(avd_probs->skip));
	memcpy(avd_probs->inter_mode, probs->inter_mode,
	       sizeof(avd_probs->inter_mode));
	memcpy(avd_probs->interp_filter, probs->interp_filter,
	       sizeof(avd_probs->interp_filter));
	memcpy(avd_probs->is_inter, probs->is_inter, sizeof(avd_probs->is_inter));
	memcpy(avd_probs->comp_mode, probs->comp_mode,
	       sizeof(avd_probs->comp_mode));
	memcpy(avd_probs->single_ref, probs->single_ref,
	       sizeof(avd_probs->single_ref));
	memcpy(avd_probs->comp_ref, probs->comp_ref, sizeof(avd_probs->comp_ref));
	memcpy(avd_probs->y_mode, probs->y_mode, sizeof(avd_probs->y_mode));
	memcpy(avd_probs->partition,
	       intra_only ? v4l2_vp9_kf_partition_probs : probs->partition,
	       sizeof(avd_probs->partition));
	memcpy(avd_probs->uv_mode,
	       intra_only ? v4l2_vp9_kf_uv_mode_prob : probs->uv_mode,
	       sizeof(avd_probs->uv_mode));

	copy_vp9_frame_mv(avd_probs, probs);

	for (int t = 0; t < 4; t++)
		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 2; j++)
				for (int k = 0; k < 6; k++)
					for (int l = 0; l < (k ? 6 : 3); l++)
						for (int n = 0; n < 3; n++)
							avd_probs->coef[count++] =
								probs->coef[t][i][j][k][l][n];

	memcpy(avd_probs->seg.pred_probs, seg->pred_probs,
	       sizeof(avd_probs->seg.pred_probs));
	memcpy(avd_probs->seg.tree_probs, seg->tree_probs,
	       sizeof(avd_probs->seg.tree_probs));
}

static int validate_format(const struct v4l2_ctrl_vp9_frame *frame)
{
	u32 subsampling = V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING |
			  V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING;

	if (frame->profile || frame->bit_depth != 8 ||
	    (frame->flags & subsampling) != subsampling ||
	    frame->tile_rows_log2 > 2)
		return -EINVAL;

	return 0;
}

static int validate_frame(struct avd_ctx *ctx, struct avd_vp9_run *run)
{
	const struct v4l2_pix_format_mplane *pix_mp = &ctx->decoded_fmt.fmt.pix_mp;
	const struct v4l2_ctrl_vp9_frame *frame = run->frame;
	bool intra_only = frame->flags & (V4L2_VP9_FRAME_FLAG_KEY_FRAME |
					  V4L2_VP9_FRAME_FLAG_INTRA_ONLY);
	u32 w = frame->frame_width_minus_1 + 1;
	u32 h = frame->frame_height_minus_1 + 1;
	u64 refs[] = { frame->last_frame_ts, frame->golden_frame_ts,
		       frame->alt_frame_ts };

	if (validate_format(frame))
		return -EINVAL;
	if ((1 << frame->tile_cols_log2) > VP9_MAX_TILE_COLS)
		return -EINVAL;

	/* Reference scaling limits of 6.4.1 (2x down, 16x up). */
	for (int i = 0; i < ARRAY_SIZE(refs) && !intra_only; i++) {
		struct avd_decoded_buffer *ref;

		ref = avd_get_ref_buf(ctx, &run->base, refs[i]);
		if (!ref->vp9.width || ref->error || &ref->base.vb == run->base.dst)
			continue;
		if (2 * w < ref->vp9.width || 2 * h < ref->vp9.height ||
		    w > 16 * ref->vp9.width || h > 16 * ref->vp9.height)
			return -EINVAL;
	}

	if (w < 64 || h < 64 || w > pix_mp->width || h > pix_mp->height)
		return -EINVAL;

	return 0;
}

static int avd_vp9_alloc_bufs(struct avd_ctx *ctx)
{
	struct avd_dev *avd = ctx->dev;
	struct avd_vp9_ctx *vp9_ctx = ctx->priv;
	u32 w = ctx->decoded_fmt.fmt.pix_mp.width;
	u32 h = ctx->decoded_fmt.fmt.pix_mp.height;
	int ret;

	/* Sizes for 8-bit pictures, from the asahi avd driver. */
	ret = avd_buf_alloc(avd, &vp9_ctx->pipe_state, 0x200);
	if (!ret)
		ret = avd_buf_alloc(avd, &vp9_ctx->probs,
				    sizeof(struct avd_vp9_probs));
	if (!ret)
		ret = avd_buf_alloc(avd, &vp9_ctx->counts,
				    sizeof(struct avd_vp9_frame_symbol_counts));
	if (!ret)
		ret = avd_buf_alloc(avd, &vp9_ctx->color,
				    DIV_ROUND_UP(w, 16) * DIV_ROUND_UP(h, 64) * 144);
	if (!ret)
		ret = avd_buf_alloc(avd, &vp9_ctx->ip_above,
				    DIV_ROUND_UP(w, 16) * 4 * 8 +
				    (VP9_MAX_TILE_COLS - 1) * 128);
	if (!ret)
		ret = avd_buf_alloc(avd, &vp9_ctx->lf_above,
				    DIV_ROUND_UP(w, 8) * 16 * 8);
	if (!ret)
		ret = avd_buf_alloc(avd, &vp9_ctx->seg, DIV_ROUND_UP(w, 8) * 24);
	if (!ret)
		ret = avd_buf_alloc(avd, &vp9_ctx->lf_left,
				    DIV_ROUND_UP(h, 8) * 16 * 8);
	if (!ret)
		ret = avd_buf_alloc(avd, &vp9_ctx->lf_left_info,
				    DIV_ROUND_UP(h, 64) * 16);
	if (!ret)
		ret = avd_buf_alloc(avd, &vp9_ctx->az_left,
				    8 * 36 * DIV_ROUND_UP(h, 8) +
				    8 * 18 * DIV_ROUND_UP(h, 16));
	if (!ret)
		ret = avd_buf_alloc(avd, &vp9_ctx->above_info,
				    DIV_ROUND_UP(w, 64) * 288 +
				    (VP9_MAX_TILE_COLS - 1) * 128);

	return ret;
}

static void avd_vp9_free_bufs(struct avd_ctx *ctx)
{
	struct avd_vp9_ctx *vp9_ctx = ctx->priv;
	struct avd_dev *avd = ctx->dev;

	avd_buf_free(avd, &vp9_ctx->pipe_state);
	avd_buf_free(avd, &vp9_ctx->probs);
	avd_buf_free(avd, &vp9_ctx->counts);
	avd_buf_free(avd, &vp9_ctx->seg);
	avd_buf_free(avd, &vp9_ctx->color);
	avd_buf_free(avd, &vp9_ctx->ip_above);
	avd_buf_free(avd, &vp9_ctx->lf_above);
	avd_buf_free(avd, &vp9_ctx->above_info);
	avd_buf_free(avd, &vp9_ctx->az_left);
	avd_buf_free(avd, &vp9_ctx->lf_left_info);
	avd_buf_free(avd, &vp9_ctx->lf_left);
}

static int avd_vp9_run(struct avd_ctx *ctx)
{
	struct avd_vp9_ctx *vp9_ctx = ctx->priv;
	struct avd_decoded_buffer *dst;
	struct avd_vp9_run run;
	unsigned int fctx_idx;
	int ret;

	ret = avd_run_preamble(ctx, &run.base);
	if (ret)
		return ret;
	run.prob_updates = avd_ctrl_ptr(ctx, V4L2_CID_STATELESS_VP9_COMPRESSED_HDR);
	run.frame = avd_ctrl_ptr(ctx, V4L2_CID_STATELESS_VP9_FRAME);

	dst = vb2_to_avd_decoded_buf(&run.base.dst->vb2_buf);
	dst->vp9.width = 0;
	dst->vp9.height = 0;

	ret = validate_frame(ctx, &run);
	if (ret)
		return ret;

	fctx_idx = v4l2_vp9_reset_frame_ctx(run.frame, vp9_ctx->frame_context);
	vp9_ctx->cur.frame_context_idx = fctx_idx;
	vp9_ctx->cur.tx_mode = run.prob_updates->tx_mode;
	vp9_ctx->probability_tables = vp9_ctx->frame_context[fctx_idx];
	v4l2_vp9_fw_update_probs(&vp9_ctx->probability_tables,
				 run.prob_updates, run.frame);

	ret = avd_job_begin(ctx, 1 + (1 << run.frame->tile_rows_log2) *
				     (1 << run.frame->tile_cols_log2));
	if (ret)
		return ret;

	init_probs(ctx, &run);

	dst->vp9.width = run.frame->frame_width_minus_1 + 1;
	dst->vp9.height = run.frame->frame_height_minus_1 + 1;

	vp9_ctx->cur.valid = true;
	vp9_ctx->cur.reference_mode = run.frame->reference_mode;
	vp9_ctx->cur.interpolation_filter = run.frame->interpolation_filter;
	vp9_ctx->cur.flags = run.frame->flags;
	vp9_ctx->cur.width = run.frame->frame_width_minus_1 + 1;
	vp9_ctx->cur.height = run.frame->frame_height_minus_1 + 1;
	vp9_ctx->cur.seg = run.frame->seg;

	set_header(ctx, &run);
	ret = set_tiles(ctx, &run);
	if (ret)
		return ret;

	return avd_submit_job(ctx, AVD_CODEC_VP9);
}

#define copy_tx_and_skip(p1, p2)					\
	do {								\
		memcpy((p1)->tx8, (p2)->tx8, sizeof((p1)->tx8));	\
		memcpy((p1)->tx16, (p2)->tx16, sizeof((p1)->tx16));	\
		memcpy((p1)->tx32, (p2)->tx32, sizeof((p1)->tx32));	\
		memcpy((p1)->skip, (p2)->skip, sizeof((p1)->skip));	\
	} while (0)

static void avd_vp9_adapt_noncoef(struct avd_vp9_ctx *vp9_ctx,
				  struct v4l2_vp9_frame_symbol_counts *counts)
{
	const struct avd_vp9_frame_symbol_counts *cnts = vp9_ctx->counts.cpu;
	u32 tx16p[2][4] = {};
	u32 sign[2][2];
	u32 classes[2][11];
	u32 class0[2][2];
	u32 bits[2][10][2];
	u32 class0_fp[2][2][4];
	u32 fp[2][4];
	u32 class0_hp[2][2];
	u32 hp[2][2];

	/* The decoder writes tx16p[2][3] and per-component MV counts. */
	for (int i = 0; i < 2; i++) {
		memcpy(tx16p[i], cnts->tx16p[i], sizeof(cnts->tx16p[i]));
		memcpy(sign[i], cnts->mv_comp[i].sign, sizeof(sign[i]));
		memcpy(classes[i], cnts->mv_comp[i].classes, sizeof(classes[i]));
		memcpy(class0[i], cnts->mv_comp[i].class0, sizeof(class0[i]));
		memcpy(bits[i], cnts->mv_comp[i].bits, sizeof(bits[i]));
		memcpy(class0_fp[i], cnts->mv_fr[i].class0_fr,
		       sizeof(class0_fp[i]));
		memcpy(fp[i], cnts->mv_fr[i].fr, sizeof(fp[i]));
		memcpy(class0_hp[i], cnts->mv_hp[i].class0_hp,
		       sizeof(class0_hp[i]));
		memcpy(hp[i], cnts->mv_hp[i].hp, sizeof(hp[i]));
	}

	counts->tx16p = &tx16p;
	counts->sign = &sign;
	counts->classes = &classes;
	counts->class0 = &class0;
	counts->bits = &bits;
	counts->class0_fp = &class0_fp;
	counts->fp = &fp;
	counts->class0_hp = &class0_hp;
	counts->hp = &hp;

	v4l2_vp9_adapt_noncoef_probs(&vp9_ctx->probability_tables, counts,
				     vp9_ctx->cur.reference_mode,
				     vp9_ctx->cur.interpolation_filter,
				     vp9_ctx->cur.tx_mode, vp9_ctx->cur.flags);
}

/* 6.1.2 refresh_probs(), as in rkvdec. */
static void avd_vp9_done(struct avd_ctx *ctx, enum vb2_buffer_state result)
{
	struct avd_vp9_ctx *vp9_ctx = ctx->priv;
	struct v4l2_vp9_frame_context *probs = &vp9_ctx->probability_tables;
	unsigned int fctx_idx = vp9_ctx->cur.frame_context_idx;
	bool frame_is_intra;
	struct {
		u8 tx8[2][1];
		u8 tx16[2][2];
		u8 tx32[2][3];
		u8 skip[3];
	} tx_skip;

	if (result == VB2_BUF_STATE_ERROR)
		goto out_update_last;

	if (!(vp9_ctx->cur.flags & V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX))
		goto out_update_last;

	if (!(vp9_ctx->cur.flags & V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE)) {
		frame_is_intra = vp9_ctx->cur.flags &
				 (V4L2_VP9_FRAME_FLAG_KEY_FRAME |
				  V4L2_VP9_FRAME_FLAG_INTRA_ONLY);

		if (frame_is_intra)
			copy_tx_and_skip(&tx_skip, probs);

		*probs = vp9_ctx->frame_context[fctx_idx];

		if (frame_is_intra)
			copy_tx_and_skip(probs, &tx_skip);

		v4l2_vp9_adapt_coef_probs(probs, &vp9_ctx->cnts,
					  !vp9_ctx->last.valid ||
					  (vp9_ctx->last.flags &
					   V4L2_VP9_FRAME_FLAG_KEY_FRAME),
					  frame_is_intra);
		if (!frame_is_intra)
			avd_vp9_adapt_noncoef(vp9_ctx, &vp9_ctx->cnts);
	}

	vp9_ctx->frame_context[fctx_idx] = *probs;

out_update_last:
	vp9_ctx->last = vp9_ctx->cur;
}

static noinline_for_stack void avd_vp9_init_count_tbl(struct avd_ctx *ctx)
{
	struct avd_vp9_ctx *vp9_ctx = ctx->priv;
	struct avd_vp9_frame_symbol_counts *cnts = vp9_ctx->counts.cpu;
	int coeff_cnts = 0, eob_cnts = 0;

	vp9_ctx->cnts.intra_inter = &cnts->intra_inter;
	vp9_ctx->cnts.y_mode = &cnts->y_mode;
	vp9_ctx->cnts.uv_mode = &cnts->uv_mode;
	vp9_ctx->cnts.comp = &cnts->comp;
	vp9_ctx->cnts.comp_ref = &cnts->comp_ref;
	vp9_ctx->cnts.single_ref = &cnts->single_ref;
	vp9_ctx->cnts.filter = &cnts->filter;
	vp9_ctx->cnts.mv_mode = &cnts->mv_mode;
	vp9_ctx->cnts.mv_joint = &cnts->mv_joint;
	vp9_ctx->cnts.tx8p = &cnts->tx8p;
	vp9_ctx->cnts.tx32p = &cnts->tx32p;
	vp9_ctx->cnts.partition = &cnts->partition;
	vp9_ctx->cnts.skip = &cnts->skip;

	for (int t = 0; t < 4; t++)
		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 2; j++)
				for (int k = 0; k < 6; k++)
					for (int l = 0; l < (k ? 6 : 3); l++) {
						vp9_ctx->cnts.coeff[t][i][j][k][l] =
							(u32 (*)[3]) &cnts->ref_cnt[coeff_cnts];
						coeff_cnts += 3;
						vp9_ctx->cnts.eob[t][i][j][k][l][0] =
							&cnts->eob_0[eob_cnts++];
						vp9_ctx->cnts.eob[t][i][j][k][l][1] =
							&cnts->ref_cnt[coeff_cnts++];
					}
}

static int avd_vp9_start(struct avd_ctx *ctx)
{
	struct avd_vp9_ctx *vp9_ctx;
	int ret;

	vp9_ctx = kzalloc_obj(*vp9_ctx);
	if (!vp9_ctx)
		return -ENOMEM;
	ctx->priv = vp9_ctx;

	ret = avd_vp9_alloc_bufs(ctx);
	if (ret) {
		avd_vp9_free_bufs(ctx);
		kfree(vp9_ctx);
		return ret;
	}

	avd_vp9_init_count_tbl(ctx);

	return 0;
}

static void avd_vp9_stop(struct avd_ctx *ctx)
{
	avd_vp9_free_bufs(ctx);
	kfree(ctx->priv);
}

static int avd_vp9_try_ctrl(struct v4l2_ctrl *ctrl)
{
	const struct v4l2_ctrl_vp9_frame *frame = ctrl->p_new.p_vp9_frame;

	if (ctrl->id != V4L2_CID_STATELESS_VP9_FRAME)
		return 0;

	return validate_format(frame);
}

static void avd_vp9_submit(struct avd_ctx *ctx)
{
	struct avd_vp9_ctx *vp9_ctx = ctx->priv;

	avd_submit_pp(ctx, vp9_ctx->submit_num);
}

const struct avd_coded_fmt_ops avd_vp9_fmt_ops = {
	.mv_size = avd_vp9_mv_size,
	.start = avd_vp9_start,
	.stop = avd_vp9_stop,
	.run = avd_vp9_run,
	.submit = avd_vp9_submit,
	.done = avd_vp9_done,
	.try_ctrl = avd_vp9_try_ctrl,
};
