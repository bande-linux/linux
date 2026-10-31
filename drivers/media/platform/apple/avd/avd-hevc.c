// SPDX-License-Identifier: GPL-2.0
/*
 * Apple AVD HEVC decoding
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

#include <media/videobuf2-dma-contig.h>

#include "avd.h"
#include "avd-inst.h"

#define NEW_TILE_ID		BIT(0)
#define NEW_SLICE		BIT(1)

#define HEVC_PCM_EN(v)		FIELD_PREP(BIT(12), !!(v))
#define HEVC_PCM_BD_LUMA(v)	FIELD_PREP(GENMASK(11, 8), v)
#define HEVC_PCM_BD_CHROMA(v)	FIELD_PREP(GENMASK(7, 4), v)
#define HEVC_PCM_CB_MIN_LUMA(v)	FIELD_PREP(GENMASK(3, 2), v)
#define HEVC_PCM_CB_LUMA(v)	FIELD_PREP(GENMASK(1, 0), v)

#define HEVC_UNK_ISM_EN(v)	FIELD_PREP(BIT(9), !!(v))
#define HEVC_UNK_FLAG		FIELD_PREP(BIT(3), 1)

#define HEVC_CTB_SIZE(v)	FIELD_PREP(GENMASK(8, 3), v)
#define HEVC_MERGE_LV(v)	FIELD_PREP(GENMASK(11, 9), v)
#define HEVC_FLAG_ENTROPY_EN(v)	FIELD_PREP(BIT(12), !!(v))
#define HEVC_FLAG_TILES_EN(v)	FIELD_PREP(BIT(13), !!(v))
#define HEVC_FLAG_TQB_EN(v)	FIELD_PREP(BIT(14), !!(v))
#define HEVC_CU_QP_DD(v)	FIELD_PREP(GENMASK(16, 15), v)
#define HEVC_FLAG_CU_QP_EN(v)	FIELD_PREP(BIT(17), !!(v))
#define HEVC_FLAG_TSKIP_EN(v)	FIELD_PREP(BIT(18), !!(v))
#define HEVC_FLAG_CI_PRED(v)	FIELD_PREP(BIT(19), !!(v))
#define HEVC_FLAG_SDH_EN(v)	FIELD_PREP(BIT(20), !!(v))
#define HEVC_FLAG_TMVP_EN(v)	FIELD_PREP(BIT(21), !!(v))

#define HEVC_SCL_DIMS		0x127ffff

#define HEVC_MAX_TILE_COLS	20
#define HEVC_MAX_TILE_ROWS	22

struct avd_hevc_tile_info {
	u16 col_width[HEVC_MAX_TILE_COLS];
	u16 row_height[HEVC_MAX_TILE_ROWS];
	u32 col_bd[HEVC_MAX_TILE_COLS + 1];
	u32 row_bd[HEVC_MAX_TILE_ROWS + 1];
	u32 *ctb_addr_rs_to_ts;
	u32 *tile_ids;
	u32 ctbs_w;
	u32 ctbs_h;
};

struct avd_hevc_run {
	struct avd_run base;
	const struct v4l2_ctrl_hevc_slice_params *sl;
	const struct v4l2_ctrl_hevc_decode_params *decode;
	const struct v4l2_ctrl_hevc_sps *sps;
	const struct v4l2_ctrl_hevc_pps *pps;
	const struct v4l2_ctrl_hevc_scaling_matrix *scaling_matrix;
	const u32 *entry_point_offsets;

	unsigned int num_entry_point_offsets;
	unsigned int num_slices;
	bool intra;

	struct avd_hevc_tile_info tile_info;
};

struct avd_hevc_ctx {
	struct avd_buf mv_above_info;
	struct avd_buf az_above;
	struct avd_buf ip_above;
	struct avd_buf lf_above;
	struct avd_buf lf_above_info;
	struct avd_buf lf_left;
	struct avd_buf lf_left_info;
	struct avd_buf sw_left;
	struct avd_buf pipe_state;

	unsigned int submit_num;
};

static u32 avd_hevc_mv_size(u32 w, u32 h)
{
	return DIV_ROUND_UP(w, 64) * DIV_ROUND_UP(h, 64) * 256;
}

static u32 ctb_size(const struct v4l2_ctrl_hevc_sps *sps)
{
	return 1 << (sps->log2_min_luma_coding_block_size_minus3 + 3 +
		     sps->log2_diff_max_min_luma_coding_block_size);
}

static void stream_refs(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_decode_params *decode = run->decode;
	struct avd_hevc_ctx *hevc_ctx = ctx->priv;
	struct avd_decoded_buffer *ref;
	dma_addr_t addr;

	push(ctx, 0);
	push_address(ctx, hevc_ctx->mv_above_info.addr);
	push_address(ctx, run->base.mv_out);
	push(ctx, 0);
	push(ctx, 0);
	push(ctx, 0);
	push(ctx, 0);

	for (int i = 0; i < decode->num_active_dpb_entries; i++) {
		const struct v4l2_hevc_dpb_entry *dpb = &decode->dpb[i];

		ref = avd_get_ref_buf(ctx, &run->base, dpb->timestamp);
		addr = avd_buf_addr(ref) +
		       ref->layout.comp_offset;

		push(ctx, AVD_REF_NUM(decode->num_active_dpb_entries - 1) |
			  AVD_REF_FLAG_CONST |
			  AVD_REF_FLAG_LONG(dpb->flags &
					    V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE) |
			  AVD_REF_DELTA_POC(run->sl[0].slice_pic_order_cnt -
					    dpb->pic_order_cnt_val));
		push_comp(ctx, addr, &ref->layout.comp);
	}
}

static u32 pack_column(const u8 *m, unsigned int stride, unsigned int col)
{
	return AVD_SCALING_I3(m[0 * stride + col]) |
	       AVD_SCALING_I2(m[1 * stride + col]) |
	       AVD_SCALING_I1(m[2 * stride + col]) |
	       AVD_SCALING_I0(m[3 * stride + col]);
}

/* Lists are sent transposed in groups of four rows (asahi avd). */
static void set_scaling_lists(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_scaling_matrix *s = run->scaling_matrix;

	push(ctx, HEVC_SCL_DIMS);

	for (int i = 0; i < 2; i++)
		push(ctx, AVD_SCALING_I2(s->scaling_list_dc_coef_16x16[i * 3 + 0]) |
			  AVD_SCALING_I1(s->scaling_list_dc_coef_16x16[i * 3 + 1]) |
			  AVD_SCALING_I0(s->scaling_list_dc_coef_16x16[i * 3 + 2]));

	for (int i = 0; i < 2; i++)
		push(ctx, AVD_SCALING_I2(s->scaling_list_dc_coef_32x32[i]));

	for (int i = 0; i < 6; i++)
		for (int j = 0; j < 4; j++)
			push(ctx, pack_column(s->scaling_list_4x4[i], 4, j));

	for (int i = 0; i < 6; i++)
		for (int j = 0; j < 2; j++)
			for (int k = 0; k < 8; k++)
				push(ctx, pack_column(&s->scaling_list_8x8[i][j * 32],
						      8, k));

	for (int i = 0; i < 6; i++)
		for (int j = 0; j < 2; j++)
			for (int k = 0; k < 8; k++)
				push(ctx, pack_column(&s->scaling_list_16x16[i][j * 32],
						      8, k));

	for (int i = 0; i < 2; i++)
		for (int j = 0; j < 2; j++)
			for (int k = 0; k < 8; k++)
				push(ctx, pack_column(&s->scaling_list_32x32[i][j * 32],
						      8, k));
}

static void hevc_set_flags(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_decode_params *decode = run->decode;
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	u32 log2_ctb_size = sps->log2_min_luma_coding_block_size_minus3 +
			    sps->log2_diff_max_min_luma_coding_block_size;

	push(ctx, HEVC_PCM_EN(sps->flags & V4L2_HEVC_SPS_FLAG_PCM_ENABLED) |
		  HEVC_PCM_BD_LUMA(sps->pcm_sample_bit_depth_luma_minus1) |
		  HEVC_PCM_BD_CHROMA(sps->pcm_sample_bit_depth_chroma_minus1) |
		  HEVC_PCM_CB_MIN_LUMA(sps->log2_min_pcm_luma_coding_block_size_minus3) |
		  HEVC_PCM_CB_LUMA(sps->log2_diff_max_min_pcm_luma_coding_block_size +
				   sps->log2_min_pcm_luma_coding_block_size_minus3));
	push(ctx, HEVC_UNK_FLAG |
		  HEVC_UNK_ISM_EN(sps->flags &
				  V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED));
	push(ctx, HEVC_CTB_SIZE(log2_ctb_size) |
		  HEVC_MERGE_LV(pps->log2_parallel_merge_level_minus2) |
		  HEVC_FLAG_ENTROPY_EN(pps->flags &
				       V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED) |
		  HEVC_FLAG_TILES_EN(pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED) |
		  HEVC_FLAG_TQB_EN(pps->flags &
				   V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED) |
		  HEVC_CU_QP_DD(log2_ctb_size - pps->diff_cu_qp_delta_depth) |
		  HEVC_FLAG_CU_QP_EN(pps->flags &
				     V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED) |
		  HEVC_FLAG_TSKIP_EN(pps->flags &
				     V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED) |
		  HEVC_FLAG_CI_PRED(pps->flags &
				    V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED) |
		  HEVC_FLAG_SDH_EN(pps->flags &
				   V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED) |
		  HEVC_FLAG_TMVP_EN(!(decode->flags &
				      V4L2_HEVC_DECODE_PARAM_FLAG_IDR_PIC) &&
				    (sps->flags &
				     V4L2_HEVC_SPS_FLAG_SPS_TEMPORAL_MVP_ENABLED)));
	push(ctx, AVD_HDR_H26X_QP_OFFSET_CB(pps->pps_cb_qp_offset) |
		  AVD_HDR_H26X_QP_OFFSET_CR(pps->pps_cr_qp_offset));

	for (int i = 0; i < 6; i++)
		push(ctx, 0);
}

static void set_header(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;
	struct avd_hevc_ctx *hevc_ctx = ctx->priv;
	u32 width = sps->pic_width_in_luma_samples;
	u32 height = sps->pic_height_in_luma_samples;
	bool is_intra = run->intra;
	u32 bytesperline;

	push(ctx, AVD_OP_HDR | AVD_OP_HDR_FLAG_DECOMP(1) |
		  AVD_OP_HDR_FLAG_INTRA(is_intra) | AVD_OP_HDR_CONST |
		  AVD_OP_HDR_FLAG_PIPE_STATE(1));
	push(ctx, AVD_HDR_CODEC_MODE(AVD_CODEC_HEVC));
	push(ctx, AVD_HDR_HEIGHT(height - 1) | AVD_HDR_WIDTH(width - 1));
	push(ctx, 0);
	push(ctx, AVD_HDR_HEIGHT((height - 1) >> 3) |
		  AVD_HDR_WIDTH((width - 1) >> 3));
	push(ctx, AVD_HDR_COMMON_CHROMA_FORMAT(sps->chroma_format_idc) |
		  AVD_HDR_COMMON_BIT_DEPTH_C(sps->bit_depth_chroma_minus8) |
		  AVD_HDR_COMMON_BIT_DEPTH_L(sps->bit_depth_luma_minus8) |
		  AVD_HDR_COMMON_MIN_LUMA_CBS(sps->log2_min_luma_coding_block_size_minus3) |
		  AVD_HDR_COMMON_LUMA_CBS(sps->log2_diff_max_min_luma_coding_block_size +
					  sps->log2_min_luma_coding_block_size_minus3) |
		  AVD_HDR_COMMON_MIN_LUMA_TBS(sps->log2_min_luma_transform_block_size_minus2) |
		  AVD_HDR_COMMON_LUMA_TBS(sps->log2_diff_max_min_luma_transform_block_size +
					  sps->log2_min_luma_transform_block_size_minus2) |
		  AVD_HDR_COMMON_TR_INTER(sps->max_transform_hierarchy_depth_inter) |
		  AVD_HDR_COMMON_TR_INTRA(sps->max_transform_hierarchy_depth_intra) |
		  AVD_HDR_COMMON_FLAG0(sps->flags & V4L2_HEVC_SPS_FLAG_AMP_ENABLED));

	hevc_set_flags(ctx, run);

	push(ctx, AVD_HDR_FEAT_H26X | AVD_HDR_FEAT_COMMON |
		  AVD_HDR_FEAT_PIPE_STATE_EN);
	push(ctx, 0);
	push(ctx, 0);
	push(ctx, 0);
	push(ctx, 0);

	push_address(ctx, hevc_ctx->pipe_state.addr);
	push_address(ctx, hevc_ctx->ip_above.addr);
	push_address(ctx, hevc_ctx->lf_above.addr);
	push_address(ctx, hevc_ctx->lf_above_info.addr);
	push_address(ctx, hevc_ctx->lf_left.addr);
	push_address(ctx, hevc_ctx->lf_left_info.addr);
	push_address(ctx, hevc_ctx->az_above.addr);
	push_address(ctx, hevc_ctx->sw_left.addr);
	push(ctx, 0);

	push_comp(ctx, run->base.comp_out, &ctx->layout.comp);
	push_address(ctx, 0);

	bytesperline = ctx->decoded_fmt.fmt.pix_mp.plane_fmt[0].bytesperline >> 4;
	push_address(ctx, run->base.y_out);
	push(ctx, bytesperline);
	push_address(ctx, run->base.uv_out);
	push(ctx, bytesperline);
	push(ctx, 0);
	push(ctx, AVD_HDR_HEIGHT(height - 1) | AVD_HDR_WIDTH(width - 1));

	if (!is_intra)
		stream_refs(ctx, run);

	if (sps->flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED)
		set_scaling_lists(ctx, run);
	else
		push(ctx, 0);
}

static void stream_weights(struct avd_ctx *ctx, struct avd_hevc_run *run,
			   const struct v4l2_ctrl_hevc_slice_params *sl)
{
	const struct v4l2_hevc_pred_weight_table *pred = &sl->pred_weight_table;
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	bool weighted = ((pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED) &&
			 sl->slice_type == V4L2_HEVC_SLICE_TYPE_P) ||
			((pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED) &&
			 sl->slice_type == V4L2_HEVC_SLICE_TYPE_B);
	u8 chroma_denom_log2;
	int luma_denom, chroma_denom;

	if (!weighted) {
		push(ctx, AVD_OP_WEIGHTS_HDR);
		return;
	}

	chroma_denom_log2 = pred->luma_log2_weight_denom +
			    pred->delta_chroma_log2_weight_denom;

	push(ctx, AVD_OP_WEIGHTS_HDR | AVD_OP_WEIGHTS_HDR_FLAG0(1) |
		  AVD_OP_WEIGHTS_HDR_LUMA(pred->luma_log2_weight_denom) |
		  AVD_OP_WEIGHTS_HDR_CHROMA(chroma_denom_log2));

	luma_denom = 1 << pred->luma_log2_weight_denom;
	chroma_denom = 1 << chroma_denom_log2;

	/* 7.4.7.3; default weights must not be sent (asahi avd). */
	for (int list = 0; list < 2; list++) {
		const s8 *dlw = list ? pred->delta_luma_weight_l1 :
				       pred->delta_luma_weight_l0;
		const s8 *lo = list ? pred->luma_offset_l1 : pred->luma_offset_l0;
		const s8 (*dcw)[2] = list ? pred->delta_chroma_weight_l1 :
					    pred->delta_chroma_weight_l0;
		const s8 (*co)[2] = list ? pred->chroma_offset_l1 :
					   pred->chroma_offset_l0;
		unsigned int num;

		if (list == 1 && sl->slice_type != V4L2_HEVC_SLICE_TYPE_B)
			break;

		num = (list ? sl->num_ref_idx_l1_active_minus1 :
			      sl->num_ref_idx_l0_active_minus1) + 1;

		for (unsigned int i = 0; i < num; i++) {
			if (dlw[i] || lo[i]) {
				push(ctx, AVD_OP_WEIGHTS | AVD_OP_WEIGHTS_IDENT(1) |
					  AVD_OP_WEIGHTS_LIST_IDX(list) |
					  AVD_OP_WEIGHTS_INDEX(i) |
					  AVD_OP_WEIGHTS_WEIGHT(dlw[i] + luma_denom));
				push(ctx, AVD_OP_OFFSETS | AVD_OP_OFFSETS_OFFSET(lo[i]));
			}

			if (dcw[i][0] || co[i][0] || dcw[i][1] || co[i][1]) {
				for (int c = 0; c < 2; c++) {
					push(ctx, AVD_OP_WEIGHTS |
						  AVD_OP_WEIGHTS_IDENT(2 + c) |
						  AVD_OP_WEIGHTS_LIST_IDX(list) |
						  AVD_OP_WEIGHTS_INDEX(i) |
						  AVD_OP_WEIGHTS_WEIGHT(dcw[i][c] +
									chroma_denom));
					push(ctx, AVD_OP_OFFSETS |
						  AVD_OP_OFFSETS_OFFSET(co[i][c]));
				}
			}
		}
	}
}

static void stream_slice_dqtblk(struct avd_ctx *ctx, struct avd_hevc_run *run,
				const struct v4l2_ctrl_hevc_slice_params *sl)
{
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;

	push(ctx, AVD_OP_QP |
		  AVD_OP_QP_VAL(pps->init_qp_minus26 + 26 + sl->slice_qp_delta) |
		  AVD_OP_QP_CB_OFF(pps->pps_cb_qp_offset + sl->slice_cb_qp_offset) |
		  AVD_OP_QP_CR_OFF(pps->pps_cr_qp_offset + sl->slice_cr_qp_offset));

	push(ctx, AVD_OP_DBLK |
		  AVD_OP_DBLK_FLAG_SAO_CHROMA(sl->flags &
					      V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA) |
		  AVD_OP_DBLK_FLAG_SAO_LUMA(sl->flags &
					    V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA) |
		  AVD_OP_DBLK_OFF0(sl->slice_tc_offset_div2) |
		  AVD_OP_DBLK_OFF1(sl->slice_beta_offset_div2) |
		  AVD_OP_DBLK_FLAG_EN(!(sl->flags &
					V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED) &&
				      ((sps->flags &
					V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED) ||
				       (sl->flags &
					V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA) ||
				       (pps->flags &
					(V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_OVERRIDE_ENABLED |
					 V4L2_HEVC_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT)))) |
		  AVD_OP_DBLK_FLAG_FULL_EN(sl->flags &
					   V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED) |
		  AVD_OP_DBLK_FLAG_TILES_EN(!(pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED) ||
					    (pps->flags &
					     V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED)) |
		  AVD_OP_DBLK_FLAG_PCM_EN((sps->flags & V4L2_HEVC_SPS_FLAG_PCM_ENABLED) &&
					  !(sps->flags &
					    V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED)));

	if (sl->slice_type == V4L2_HEVC_SLICE_TYPE_B ||
	    sl->slice_type == V4L2_HEVC_SLICE_TYPE_P) {
		for (int i = 0; i <= sl->num_ref_idx_l0_active_minus1; i++)
			push(ctx, AVD_OP_REF | AVD_OP_REF_LIST_IDX(0) |
				  AVD_OP_REF_LOOP_IDX(i) |
				  AVD_OP_REF_DBP_IDX(sl->ref_idx_l0[i]));
		if (sl->slice_type == V4L2_HEVC_SLICE_TYPE_B)
			for (int i = 0; i <= sl->num_ref_idx_l1_active_minus1; i++)
				push(ctx, AVD_OP_REF | AVD_OP_REF_LIST_IDX(1) |
					  AVD_OP_REF_LOOP_IDX(i) |
					  AVD_OP_REF_DBP_IDX(sl->ref_idx_l1[i]));

		stream_weights(ctx, run, sl);
	}
}

static void stream_slice_mv(struct avd_ctx *ctx, struct avd_hevc_run *run,
			    const struct v4l2_ctrl_hevc_slice_params *sl,
			    bool is_first)
{
	const struct v4l2_ctrl_hevc_decode_params *decode = run->decode;
	bool tmvp = sl->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED;
	struct avd_decoded_buffer *ref;
	const u8 *ref_list;
	bool ref_valid;

	if (sl->slice_type == V4L2_HEVC_SLICE_TYPE_I) {
		push(ctx, AVD_OP_SL_REF | AVD_OP_SL_REF_SLICE_I(1));
		return;
	}

	if (sl->slice_type == V4L2_HEVC_SLICE_TYPE_P ||
	    (sl->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0))
		ref_list = sl->ref_idx_l0;
	else
		ref_list = sl->ref_idx_l1;

	ref = avd_get_ref_buf(ctx, &run->base,
			      decode->dpb[ref_list[sl->collocated_ref_idx]].timestamp);

	ref_valid = !(sl->flags &
		      V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT) &&
		    tmvp && is_first && !ref->hevc.is_intra;

	push(ctx, AVD_OP_SL_REF |
		  AVD_OP_SL_REF_MAX_MERGE(5 - sl->five_minus_max_num_merge_cand) |
		  AVD_OP_SL_REF_FLAG_CABAC(sl->flags &
					   V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT) |
		  AVD_OP_SL_REF_FLAG0(tmvp &&
				      !(sl->flags &
					V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0)) |
		  AVD_OP_SL_REF_FLAG1(!(sl->flags &
					V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO)) |
		  AVD_OP_SL_REF_FLAG2(tmvp ||
				      (sl->flags &
				       V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT)) |
		  AVD_OP_SL_REF_NUM_L0(sl->num_ref_idx_l0_active_minus1) |
		  AVD_OP_SL_REF_NUM_L1(sl->num_ref_idx_l1_active_minus1) |
		  AVD_OP_SL_REF_SLICE_P(sl->slice_type == V4L2_HEVC_SLICE_TYPE_P) |
		  AVD_OP_SL_REF_SLICE_B(ref_valid));

	if (ref_valid)
		push_address(ctx, avd_buf_addr(ref) +
				  ref->layout.mv_offset);
}

static void set_slice(struct avd_ctx *ctx, struct avd_hevc_run *run,
		      u32 offset, u32 size, u32 flags)
{
	dma_addr_t addr = run->base.coded_in + offset;

	push(ctx, AVD_OP_CODED_DATA | flags |
		  AVD_OP_CODED_DATA_ADDR(upper_32_bits(addr)));
	push(ctx, lower_32_bits(addr));
	push(ctx, size);
}

static u32 submit_slice_segment(struct avd_ctx *ctx, struct avd_hevc_run *run,
				const struct v4l2_ctrl_hevc_slice_params *sl,
				int row, int col, bool first_slice, bool hflip,
				bool vflip, u32 coded_flags, u32 last_tile_block)
{
	const struct avd_hevc_tile_info *ti = &run->tile_info;
	u32 tile_block = 0, tile_boundary = 0, block;

	if (coded_flags & NEW_SLICE) {
		tile_block = AVD_OP_SL_LOC_Y(sl->slice_segment_addr / ti->ctbs_w) |
			     AVD_OP_SL_LOC_X(sl->slice_segment_addr % ti->ctbs_w);

		if (!(sl->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT))
			last_tile_block = tile_block;

		push(ctx, AVD_OP_SL_LOC | last_tile_block);
		stream_slice_dqtblk(ctx, run, sl);
	} else {
		tile_boundary = AVD_OP_SL_LOC_Y(ti->row_bd[row]) |
				AVD_OP_SL_LOC_X(ti->col_bd[col]);
	}

	block = coded_flags & NEW_SLICE ? tile_block : tile_boundary;

	if (coded_flags & NEW_TILE_ID) {
		push(ctx, AVD_OP_SL_DIM_START | block);

		if (run->pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED)
			push(ctx, AVD_SL_DIM_END_ROW((hflip ? 4 : 0) | (vflip ? 8 : 0)) |
				  AVD_SL_DIM_END_COL(col) |
				  AVD_SL_DIM_END_Y(ti->row_bd[row + 1] - 1) |
				  AVD_SL_DIM_END_X(ti->col_bd[col + 1] - 1));
		else
			push(ctx, AVD_SL_DIM_END_Y(ti->ctbs_h - 1) |
				  AVD_SL_DIM_END_X(ti->ctbs_w - 1));
	}

	if (coded_flags & NEW_SLICE)
		stream_slice_mv(ctx, run, sl, first_slice);

	push(ctx, AVD_SL_DIM_END_COL(1) | block);

	return last_tile_block;
}

static int avd_hevc_tile_id(const struct avd_hevc_tile_info *ti, u32 ctb_addr)
{
	return ti->tile_ids[ti->ctb_addr_rs_to_ts[ctb_addr]];
}

struct sl_ctx {
	u32 ctx_col;
	u32 ctx_row;
	s32 q1_col;
	s32 q1_row;
};

static int stream_slices(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	const struct avd_hevc_tile_info *ti = &run->tile_info;
	struct avd_hevc_ctx *hevc_ctx = ctx->priv;
	bool tiles = run->pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED;
	u32 num_cols = run->pps->num_tile_columns_minus1 + 1;
	u32 payload = vb2_get_plane_payload(&run->base.src->vb2_buf, 0);
	unsigned int entry_point_idx = 0, pos = 0;
	u32 offset = 0, last_tile_block = 0;
	struct sl_ctx last = { .q1_col = -1, .q1_row = -1 };
	int row = 0, col = 0;
	int ret;

	for (unsigned int s = 0; s < run->num_slices; s++) {
		const struct v4l2_ctrl_hevc_slice_params *sl = &run->sl[s];
		u32 slice_bytes = sl->bit_size / 8;
		u32 seg_offset = sl->data_byte_offset;
		unsigned int to = tiles ? sl->num_entry_point_offsets + 1 : 1;
		int tile_id, last_tile_id;

		if (seg_offset >= slice_bytes || offset + slice_bytes > payload ||
		    offset + slice_bytes < offset)
			return -EINVAL;
		if (tiles && sl->num_entry_point_offsets >
			     run->num_entry_point_offsets - entry_point_idx)
			return -EINVAL;

		tile_id = avd_hevc_tile_id(ti, sl->slice_segment_addr);
		last_tile_id = s ? avd_hevc_tile_id(ti, run->sl[s - 1].slice_segment_addr) : -1;

		for (unsigned int i = 0; i < to; i++) {
			bool hflip = false, vflip = false;
			u32 flags = 0, coded, size;

			if (i < to - 1) {
				size = run->entry_point_offsets[entry_point_idx++];
				if (!size || size >= slice_bytes - seg_offset)
					return -EINVAL;
			} else {
				size = slice_bytes - seg_offset;
			}

			if (!s || tile_id != last_tile_id || i)
				flags |= NEW_TILE_ID;
			if (!i)
				flags |= NEW_SLICE;

			/* Scan direction of tiles after a wrap (asahi avd, TILES_B_Cisco_1). */
			if (flags & NEW_TILE_ID) {
				if (pos >= (run->pps->num_tile_rows_minus1 + 1) * num_cols)
					return -EINVAL;
				row = pos / num_cols;
				col = pos % num_cols;

				if ((col >= last.ctx_col && row > last.ctx_row) ||
				    (col <= last.q1_col && row > last.q1_row))
					vflip = true;

				if (!(flags & NEW_SLICE)) {
					if (row && row == last.ctx_row + 1) {
						hflip = true;
						if (!vflip) {
							last.q1_row = row;
							last.q1_col = col;
						}
					}
				} else {
					last.ctx_row = row;
					last.ctx_col = col;
					last.q1_row = -1;
					last.q1_col = -1;
				}
			}

			ret = avd_job_next(ctx);
			if (ret)
				return ret;

			coded = flags;
			if (sl->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT)
				coded &= ~NEW_SLICE;
			set_slice(ctx, run, offset + seg_offset, size, coded << 13);

			last_tile_block = submit_slice_segment(ctx, run, sl, row, col,
							       !s, hflip, vflip, flags,
							       last_tile_block);

			if (flags & NEW_TILE_ID)
				pos++;

			seg_offset += size;
		}
		offset += slice_bytes;
	}

	hevc_ctx->submit_num = pos;
	return 0;
}

/* 6.5.1 CTB raster and tile scanning conversion process, (6-3) to (6-10). */
static int avd_hevc_compute_tiles(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	struct avd_hevc_tile_info *ti = &run->tile_info;
	u32 cols = pps->num_tile_columns_minus1 + 1;
	u32 rows = pps->num_tile_rows_minus1 + 1;
	u32 ctb = ctb_size(run->sps);
	u32 size, sum, i, j;

	ti->ctbs_w = DIV_ROUND_UP(run->sps->pic_width_in_luma_samples, ctb);
	ti->ctbs_h = DIV_ROUND_UP(run->sps->pic_height_in_luma_samples, ctb);
	size = ti->ctbs_w * ti->ctbs_h;

	if (cols > ti->ctbs_w || rows > ti->ctbs_h)
		return -EINVAL;

	if (!(pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED) ||
	    (pps->flags & V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING)) {
		for (i = 0; i < cols; i++)
			ti->col_width[i] = (i + 1) * ti->ctbs_w / cols -
					   i * ti->ctbs_w / cols;
		for (i = 0; i < rows; i++)
			ti->row_height[i] = (i + 1) * ti->ctbs_h / rows -
					    i * ti->ctbs_h / rows;
	} else {
		for (i = 0, sum = 0; i < cols - 1; i++) {
			ti->col_width[i] = pps->column_width_minus1[i] + 1;
			sum += ti->col_width[i];
		}
		if (sum >= ti->ctbs_w)
			return -EINVAL;
		ti->col_width[i] = ti->ctbs_w - sum;

		for (i = 0, sum = 0; i < rows - 1; i++) {
			ti->row_height[i] = pps->row_height_minus1[i] + 1;
			sum += ti->row_height[i];
		}
		if (sum >= ti->ctbs_h)
			return -EINVAL;
		ti->row_height[i] = ti->ctbs_h - sum;
	}

	ti->col_bd[0] = 0;
	for (i = 0; i < cols; i++)
		ti->col_bd[i + 1] = ti->col_bd[i] + ti->col_width[i];
	ti->row_bd[0] = 0;
	for (i = 0; i < rows; i++)
		ti->row_bd[i + 1] = ti->row_bd[i] + ti->row_height[i];

	ti->ctb_addr_rs_to_ts = kcalloc(size, sizeof(u32), GFP_KERNEL);
	ti->tile_ids = kcalloc(size, sizeof(u32), GFP_KERNEL);
	if (!ti->ctb_addr_rs_to_ts || !ti->tile_ids)
		return -ENOMEM;

	for (u32 rs = 0; rs < size; rs++) {
		u32 tb_x = rs % ti->ctbs_w, tb_y = rs / ti->ctbs_w;
		u32 tile_x = 0, tile_y = 0, ts = 0;

		for (i = 0; i < cols; i++)
			if (tb_x >= ti->col_bd[i])
				tile_x = i;
		for (j = 0; j < rows; j++)
			if (tb_y >= ti->row_bd[j])
				tile_y = j;
		for (i = 0; i < tile_x; i++)
			ts += ti->row_height[tile_y] * ti->col_width[i];
		for (j = 0; j < tile_y; j++)
			ts += ti->ctbs_w * ti->row_height[j];
		ts += (tb_y - ti->row_bd[tile_y]) * ti->col_width[tile_x] +
		      tb_x - ti->col_bd[tile_x];
		ti->ctb_addr_rs_to_ts[rs] = ts;
	}

	for (j = 0, i = 0; j < rows; j++)
		for (u32 c = 0; c < cols; c++, i++)
			for (u32 y = ti->row_bd[j]; y < ti->row_bd[j + 1]; y++)
				for (u32 x = ti->col_bd[c]; x < ti->col_bd[c + 1]; x++)
					ti->tile_ids[ti->ctb_addr_rs_to_ts[y * ti->ctbs_w + x]] = i;

	return 0;
}

static int avd_hevc_alloc_scratch(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	struct avd_hevc_tile_info *ti = &run->tile_info;
	struct avd_hevc_ctx *hevc_ctx = ctx->priv;
	struct avd_dev *avd = ctx->dev;
	u32 cols = pps->num_tile_columns_minus1 + 1;
	u32 rows = pps->num_tile_rows_minus1 + 1;
	u32 w = run->sps->pic_width_in_luma_samples;
	u32 ctb = ctb_size(run->sps);
	u32 max_col = 0, max_row = 0;
	int ret;

	for (u32 i = 0; i < cols; i++)
		max_col = max_t(u32, max_col, ti->col_width[i]);
	for (u32 i = 0; i < rows; i++)
		max_row = max_t(u32, max_row, ti->row_height[i]);

	/* Sizes for 8-bit pictures, from the asahi avd driver. */
	ret = avd_buf_alloc(avd, &hevc_ctx->ip_above, max_col * ctb / 4 * 8);
	if (!ret)
		ret = avd_buf_alloc(avd, &hevc_ctx->mv_above_info,
				    max_col * ctb / 16 * 20);
	if (!ret)
		ret = avd_buf_alloc(avd, &hevc_ctx->lf_above,
				    DIV_ROUND_UP(w, 16) * 8 * 16 + (cols - 1) * 256);
	if (!ret)
		ret = avd_buf_alloc(avd, &hevc_ctx->lf_above_info,
				    DIV_ROUND_UP(w + 7, 16) * 36 + cols * 128);
	if (ret)
		return ret;

	if (pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED) {
		ret = avd_buf_alloc(avd, &hevc_ctx->lf_left,
				    max_row * ctb / 4 * 36 + 144);
		if (!ret)
			ret = avd_buf_alloc(avd, &hevc_ctx->lf_left_info,
					    max_row * ctb / 4 * 9);
		if (!ret)
			ret = avd_buf_alloc(avd, &hevc_ctx->az_above,
					    DIV_ROUND_UP(w, 64) * 144 +
					    (cols - 1) * 128);
		if (!ret)
			ret = avd_buf_alloc(avd, &hevc_ctx->sw_left,
					    max_row * ctb / 4 * 216);
	} else {
		ret = avd_buf_alloc(avd, &hevc_ctx->az_above,
				    DIV_ROUND_UP(w + 7, 16) * 4 * 8);
	}

	return ret;
}

static int avd_hevc_validate_sps(const struct v4l2_ctrl_hevc_sps *sps)
{
	if (sps->chroma_format_idc > 1 || sps->bit_depth_luma_minus8 ||
	    sps->bit_depth_chroma_minus8)
		return -EINVAL;
	if (sps->log2_min_luma_coding_block_size_minus3 > 3 ||
	    sps->log2_diff_max_min_luma_coding_block_size > 3 ||
	    sps->log2_min_luma_coding_block_size_minus3 +
	    sps->log2_diff_max_min_luma_coding_block_size > 3)
		return -EINVAL;
	if (sps->log2_min_luma_transform_block_size_minus2 +
	    sps->log2_diff_max_min_luma_transform_block_size > 3 ||
	    sps->max_transform_hierarchy_depth_inter > 4 ||
	    sps->max_transform_hierarchy_depth_intra > 4)
		return -EINVAL;
	if ((sps->flags & V4L2_HEVC_SPS_FLAG_PCM_ENABLED) &&
	    (sps->pcm_sample_bit_depth_luma_minus1 > 7 ||
	     sps->pcm_sample_bit_depth_chroma_minus1 > 7 ||
	     sps->log2_min_pcm_luma_coding_block_size_minus3 +
	     sps->log2_diff_max_min_pcm_luma_coding_block_size > 2))
		return -EINVAL;

	return 0;
}

static int avd_hevc_validate_pps(const struct v4l2_ctrl_hevc_pps *pps)
{
	if (pps->num_tile_columns_minus1 >= HEVC_MAX_TILE_COLS ||
	    pps->num_tile_rows_minus1 >= HEVC_MAX_TILE_ROWS ||
	    pps->log2_parallel_merge_level_minus2 > 4 ||
	    pps->diff_cu_qp_delta_depth > 3)
		return -EINVAL;

	return 0;
}

static int avd_hevc_validate(struct avd_ctx *ctx, struct avd_hevc_run *run)
{
	const struct v4l2_pix_format_mplane *pix_mp = &ctx->decoded_fmt.fmt.pix_mp;
	const struct v4l2_ctrl_hevc_decode_params *decode = run->decode;
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;
	u32 ctbs;

	if (!run->num_slices)
		return -EINVAL;
	if (avd_hevc_validate_sps(sps) || sps->chroma_format_idc != 1 ||
	    avd_hevc_validate_pps(run->pps))
		return -EINVAL;
	if (sps->pic_width_in_luma_samples > pix_mp->width ||
	    sps->pic_height_in_luma_samples > pix_mp->height ||
	    sps->pic_width_in_luma_samples < 64 ||
	    sps->pic_height_in_luma_samples < 64)
		return -EINVAL;
	if (ctb_size(sps) < 16 ||
	    run->pps->diff_cu_qp_delta_depth >
	    sps->log2_diff_max_min_luma_coding_block_size)
		return -EINVAL;

	ctbs = DIV_ROUND_UP(sps->pic_width_in_luma_samples, ctb_size(sps)) *
	       DIV_ROUND_UP(sps->pic_height_in_luma_samples, ctb_size(sps));

	for (unsigned int s = 0; s < run->num_slices; s++) {
		const struct v4l2_ctrl_hevc_slice_params *sl = &run->sl[s];
		u8 num_l0 = sl->num_ref_idx_l0_active_minus1 + 1;
		u8 num_l1 = sl->num_ref_idx_l1_active_minus1 + 1;

		if (sl->slice_segment_addr >= ctbs ||
		    sl->slice_type > V4L2_HEVC_SLICE_TYPE_I ||
		    sl->five_minus_max_num_merge_cand > 4)
			return -EINVAL;
		if (sl->slice_type == V4L2_HEVC_SLICE_TYPE_I)
			continue;
		if (sl->pred_weight_table.luma_log2_weight_denom > 7 ||
		    sl->pred_weight_table.luma_log2_weight_denom +
		    sl->pred_weight_table.delta_chroma_log2_weight_denom < 0 ||
		    sl->pred_weight_table.luma_log2_weight_denom +
		    sl->pred_weight_table.delta_chroma_log2_weight_denom > 7)
			return -EINVAL;
		if (num_l0 > V4L2_HEVC_DPB_ENTRIES_NUM_MAX ||
		    num_l1 > V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
			return -EINVAL;
		for (int i = 0; i < num_l0; i++)
			if (sl->ref_idx_l0[i] >= decode->num_active_dpb_entries)
				return -EINVAL;
		if (sl->slice_type == V4L2_HEVC_SLICE_TYPE_B)
			for (int i = 0; i < num_l1; i++)
				if (sl->ref_idx_l1[i] >= decode->num_active_dpb_entries)
					return -EINVAL;
		if (sl->collocated_ref_idx >=
		    ((sl->slice_type == V4L2_HEVC_SLICE_TYPE_P ||
		      (sl->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0)) ?
		     num_l0 : num_l1))
			return -EINVAL;
	}

	return 0;
}

static int avd_hevc_start(struct avd_ctx *ctx)
{
	struct avd_hevc_ctx *hevc_ctx;
	int ret;

	hevc_ctx = kzalloc_obj(*hevc_ctx);
	if (!hevc_ctx)
		return -ENOMEM;
	ctx->priv = hevc_ctx;

	ret = avd_buf_alloc(ctx->dev, &hevc_ctx->pipe_state, 0x200);
	if (ret)
		kfree(hevc_ctx);

	return ret;
}

static void avd_hevc_stop(struct avd_ctx *ctx)
{
	struct avd_hevc_ctx *hevc_ctx = ctx->priv;
	struct avd_dev *avd = ctx->dev;

	avd_buf_free(avd, &hevc_ctx->pipe_state);
	avd_buf_free(avd, &hevc_ctx->mv_above_info);
	avd_buf_free(avd, &hevc_ctx->az_above);
	avd_buf_free(avd, &hevc_ctx->ip_above);
	avd_buf_free(avd, &hevc_ctx->lf_above);
	avd_buf_free(avd, &hevc_ctx->lf_above_info);
	avd_buf_free(avd, &hevc_ctx->lf_left);
	avd_buf_free(avd, &hevc_ctx->lf_left_info);
	avd_buf_free(avd, &hevc_ctx->sw_left);
	kfree(hevc_ctx);
}

static int avd_hevc_run(struct avd_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->ctrl_hdl;
	struct avd_decoded_buffer *dst;
	struct avd_hevc_run run = {};
	struct v4l2_ctrl *ctrl;
	int ret;

	ret = avd_run_preamble(ctx, &run.base);
	if (ret)
		return ret;

	run.decode = avd_ctrl_ptr(ctx, V4L2_CID_STATELESS_HEVC_DECODE_PARAMS);
	ctrl = v4l2_ctrl_find(hdl, V4L2_CID_STATELESS_HEVC_SLICE_PARAMS);
	run.sl = ctrl->p_cur.p;
	run.num_slices = ctrl->elems;
	run.sps = avd_ctrl_ptr(ctx, V4L2_CID_STATELESS_HEVC_SPS);
	run.pps = avd_ctrl_ptr(ctx, V4L2_CID_STATELESS_HEVC_PPS);
	run.scaling_matrix = avd_ctrl_ptr(ctx, V4L2_CID_STATELESS_HEVC_SCALING_MATRIX);
	ctrl = v4l2_ctrl_find(hdl, V4L2_CID_STATELESS_HEVC_ENTRY_POINT_OFFSETS);
	run.entry_point_offsets = ctrl->p_cur.p;
	run.num_entry_point_offsets = ctrl->elems;

	ret = avd_hevc_validate(ctx, &run);
	if (ret)
		return ret;

	dst = vb2_to_avd_decoded_buf(&run.base.dst->vb2_buf);
	run.intra = true;
	for (unsigned int s = 0; s < run.num_slices; s++)
		if (run.sl[s].slice_type != V4L2_HEVC_SLICE_TYPE_I)
			run.intra = false;
	dst->hevc.is_intra = run.intra;

	ret = avd_hevc_compute_tiles(ctx, &run);
	if (!ret)
		ret = avd_hevc_alloc_scratch(ctx, &run);
	if (!ret)
		ret = avd_job_begin(ctx, run.num_slices +
					 run.num_entry_point_offsets + 1);
	if (!ret) {
		set_header(ctx, &run);
		ret = stream_slices(ctx, &run);
	}
	if (!ret)
		ret = avd_submit_job(ctx, AVD_CODEC_HEVC);

	kfree(run.tile_info.ctb_addr_rs_to_ts);
	kfree(run.tile_info.tile_ids);
	return ret;
}

static void avd_hevc_done(struct avd_ctx *ctx, enum vb2_buffer_state result)
{
	if (result == VB2_BUF_STATE_ERROR)
		avd_job_free(ctx);
}

static int avd_hevc_try_ctrl(struct v4l2_ctrl *ctrl)
{
	if (ctrl->id == V4L2_CID_STATELESS_HEVC_SPS)
		return avd_hevc_validate_sps(ctrl->p_new.p_hevc_sps);
	if (ctrl->id == V4L2_CID_STATELESS_HEVC_PPS)
		return avd_hevc_validate_pps(ctrl->p_new.p_hevc_pps);

	return 0;
}

static void avd_hevc_submit(struct avd_ctx *ctx)
{
	struct avd_hevc_ctx *hevc_ctx = ctx->priv;

	avd_submit_pp(ctx, hevc_ctx->submit_num);
}

const struct avd_coded_fmt_ops avd_hevc_fmt_ops = {
	.mv_size = avd_hevc_mv_size,
	.start = avd_hevc_start,
	.stop = avd_hevc_stop,
	.run = avd_hevc_run,
	.submit = avd_hevc_submit,
	.done = avd_hevc_done,
	.try_ctrl = avd_hevc_try_ctrl,
};
