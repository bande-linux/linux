/* SPDX-License-Identifier: MIT */
/*
 * AVD instruction stream encoding, from the asahi avd driver (traces of the
 * macOS AppleAVD driver).
 */

#ifndef AVD_INST_H_
#define AVD_INST_H_

#include <linux/bitfield.h>
#include <linux/types.h>

#include "avd.h"

#define AVD_OP_EXEC			FIELD_PREP(GENMASK(31, 24), 0x2b)
#define AVD_OP_EXEC_FIFO_MASK(v)	FIELD_PREP(GENMASK(3, 0), v)
#define AVD_OP_EXEC_FIFO_IDX(v)		FIELD_PREP(GENMASK(7, 4), v)
#define AVD_OP_EXEC_FLAG_START(v)	FIELD_PREP(BIT(9), !!(v))
#define AVD_OP_EXEC_FLAG_END(v)		FIELD_PREP(BIT(10), !!(v))

#define AVD_OP_HDR			FIELD_PREP(GENMASK(31, 20), 0x2db)
#define AVD_OP_HDR_CONST		FIELD_PREP(GENMASK(10, 0), 0x2e0)
#define AVD_OP_HDR_FLAG_DECOMP(v)	FIELD_PREP(BIT(12), !!(v))
#define AVD_OP_HDR_FLAG_INTRA(v)	FIELD_PREP(BIT(13), !!(v))
#define AVD_OP_HDR_FLAG_PIPE_STATE(v)	FIELD_PREP(BIT(19), !!(v))

#define AVD_OP_WEIGHTS_HDR		FIELD_PREP(GENMASK(31, 20), 0x2dd)
#define AVD_OP_WEIGHTS_HDR_CHROMA(v)	FIELD_PREP(GENMASK(2, 0), v)
#define AVD_OP_WEIGHTS_HDR_LUMA(v)	FIELD_PREP(GENMASK(5, 3), v)
#define AVD_OP_WEIGHTS_HDR_FLAG0(v)	FIELD_PREP(BIT(6), !!(v))
#define AVD_OP_WEIGHTS_HDR_FLAG1(v)	FIELD_PREP(BIT(7), !!(v))

#define AVD_OP_WEIGHTS			FIELD_PREP(GENMASK(31, 20), 0x2de)
#define AVD_OP_WEIGHTS_WEIGHT(v)	FIELD_PREP(GENMASK(8, 0), v)
#define AVD_OP_WEIGHTS_INDEX(v)		FIELD_PREP(GENMASK(12, 9), v)
#define AVD_OP_WEIGHTS_LIST_IDX(v)	FIELD_PREP(BIT(13), v)
#define AVD_OP_WEIGHTS_IDENT(v)		FIELD_PREP(GENMASK(16, 14), v)

#define AVD_OP_OFFSETS			FIELD_PREP(GENMASK(31, 20), 0x2df)
#define AVD_OP_OFFSETS_OFFSET(v)	FIELD_PREP(GENMASK(15, 0), v)

#define AVD_OP_CODED_DATA		FIELD_PREP(GENMASK(31, 20), 0x2d8)
#define AVD_OP_CODED_DATA_ADDR(v)	FIELD_PREP(GENMASK(12, 0), v)
#define AVD_OP_CODED_DATA_BIT_OFF(v)	FIELD_PREP(GENMASK(18, 15), v)

#define AVD_OP_SL_LOC			FIELD_PREP(GENMASK(31, 24), 0x2c)
#define AVD_OP_SL_LOC_X(v)		FIELD_PREP(GENMASK(11, 0), v)
#define AVD_OP_SL_LOC_Y(v)		FIELD_PREP(GENMASK(23, 12), v)

#define AVD_OP_SL_DIM_START		FIELD_PREP(GENMASK(31, 24), 0x2a)
#define AVD_OP_SL_DIM_START_X(v)	FIELD_PREP(GENMASK(11, 0), v)
#define AVD_OP_SL_DIM_START_Y(v)	FIELD_PREP(GENMASK(23, 12), v)

#define AVD_SL_DIM_END_X(v)		FIELD_PREP(GENMASK(11, 0), v)
#define AVD_SL_DIM_END_Y(v)		FIELD_PREP(GENMASK(23, 12), v)
#define AVD_SL_DIM_END_COL(v)		FIELD_PREP(GENMASK(27, 24), v)
#define AVD_SL_DIM_END_ROW(v)		FIELD_PREP(GENMASK(31, 28), v)

#define AVD_OP_SL_REF			FIELD_PREP(GENMASK(31, 24), 0x2d)
#define AVD_OP_SL_REF_MAX_MERGE(v)	FIELD_PREP(GENMASK(3, 1), v)
#define AVD_OP_SL_REF_FLAG0(v)		FIELD_PREP(BIT(4), !!(v))
#define AVD_OP_SL_REF_FLAG_CABAC(v)	FIELD_PREP(BIT(5), !!(v))
#define AVD_OP_SL_REF_FLAG1(v)		FIELD_PREP(BIT(6), !!(v))
#define AVD_OP_SL_REF_NUM_L1(v)		FIELD_PREP(GENMASK(10, 7), v)
#define AVD_OP_SL_REF_NUM_L0(v)		FIELD_PREP(GENMASK(15, 11), v)
#define AVD_OP_SL_REF_FLAG2(v)		FIELD_PREP(BIT(15), !!(v))
#define AVD_OP_SL_REF_SLICE_P(v)	FIELD_PREP(BIT(16), !!(v))
#define AVD_OP_SL_REF_SLICE_I(v)	FIELD_PREP(BIT(17), !!(v))
#define AVD_OP_SL_REF_SLICE_B(v)	FIELD_PREP(BIT(18), !!(v))

#define AVD_OP_QP			FIELD_PREP(GENMASK(31, 20), 0x2d9)
#define AVD_OP_QP_CR_OFF(v)		FIELD_PREP(GENMASK(4, 0), v)
#define AVD_OP_QP_CB_OFF(v)		FIELD_PREP(GENMASK(9, 5), v)
#define AVD_OP_QP_VAL(v)		FIELD_PREP(GENMASK(17, 10), v)

#define AVD_OP_DBLK			FIELD_PREP(GENMASK(31, 20), 0x2da)
#define AVD_OP_DBLK_FLAG_SAO_CHROMA(v)	FIELD_PREP(BIT(6), !!(v))
#define AVD_OP_DBLK_FLAG_SAO_LUMA(v)	FIELD_PREP(BIT(7), !!(v))
#define AVD_OP_DBLK_OFF0(v)		FIELD_PREP(GENMASK(11, 8), v)
#define AVD_OP_DBLK_OFF1(v)		FIELD_PREP(GENMASK(16, 12), v)
#define AVD_OP_DBLK_FLAG_EN(v)		FIELD_PREP(BIT(16), !!(v))
#define AVD_OP_DBLK_FLAG_FULL_EN(v)	FIELD_PREP(BIT(17), !!(v))
#define AVD_OP_DBLK_FLAG_TILES_EN(v)	FIELD_PREP(BIT(18), !!(v))
#define AVD_OP_DBLK_FLAG_PCM_EN(v)	FIELD_PREP(BIT(19), !!(v))

#define AVD_OP_REF			FIELD_PREP(GENMASK(31, 20), 0x2dc)
#define AVD_OP_REF_DBP_IDX(v)		FIELD_PREP(GENMASK(3, 0), v)
#define AVD_OP_REF_LOOP_IDX(v)		FIELD_PREP(GENMASK(7, 4), v)
#define AVD_OP_REF_LIST_IDX(v)		FIELD_PREP(GENMASK(11, 8), v)

#define AVD_HDR_CODEC_MODE(v)		FIELD_PREP(GENMASK(28, 24), v)
#define AVD_HDR_WIDTH(v)		FIELD_PREP(GENMASK(15, 0), v)
#define AVD_HDR_HEIGHT(v)		FIELD_PREP(GENMASK(31, 16), v)

#define AVD_HDR_FEAT_H264		FIELD_PREP(GENMASK(3, 0), 10)
#define AVD_HDR_FEAT_PIPE_STATE_EN	FIELD_PREP(GENMASK(7, 4), 3)
#define AVD_HDR_FEAT_VP9		FIELD_PREP(BIT(17), 1)
#define AVD_HDR_FEAT_H26X		FIELD_PREP(BIT(20), 1)
#define AVD_HDR_FEAT_COMMON		FIELD_PREP(BIT(21), 1)

#define AVD_HDR_COMMON_FLAG0(v)		FIELD_PREP(BIT(0), !!(v))
#define AVD_HDR_COMMON_TR_INTRA(v)	FIELD_PREP(GENMASK(4, 1), v)
#define AVD_HDR_COMMON_TR_INTER(v)	FIELD_PREP(GENMASK(7, 4), v)
#define AVD_HDR_COMMON_LUMA_TBS(v)	FIELD_PREP(GENMASK(8, 7), v)
#define AVD_HDR_COMMON_MIN_LUMA_TBS(v)	FIELD_PREP(GENMASK(10, 9), v)
#define AVD_HDR_COMMON_LUMA_CBS(v)	FIELD_PREP(GENMASK(12, 11), v)
#define AVD_HDR_COMMON_MIN_LUMA_CBS(v)	FIELD_PREP(GENMASK(14, 13), v)
#define AVD_HDR_COMMON_BIT_DEPTH_L(v)	FIELD_PREP(GENMASK(18, 15), v)
#define AVD_HDR_COMMON_BIT_DEPTH_C(v)	FIELD_PREP(GENMASK(23, 19), v)
#define AVD_HDR_COMMON_CHROMA_FORMAT(v)	FIELD_PREP(GENMASK(26, 24), v)

#define AVD_HDR_H26X_QP_OFFSET_CR(v)	FIELD_PREP(GENMASK(4, 0), v)
#define AVD_HDR_H26X_QP_OFFSET_CB(v)	FIELD_PREP(GENMASK(9, 5), v)

#define AVD_SCALING_I0(v)		FIELD_PREP(GENMASK(7, 0), v)
#define AVD_SCALING_I1(v)		FIELD_PREP(GENMASK(15, 8), v)
#define AVD_SCALING_I2(v)		FIELD_PREP(GENMASK(23, 16), v)
#define AVD_SCALING_I3(v)		FIELD_PREP(GENMASK(31, 24), v)

#define AVD_REF_NUM(v)			FIELD_PREP(GENMASK(31, 28), v)
#define AVD_REF_FLAG_CONST		FIELD_PREP(BIT(24), 1)
#define AVD_REF_FLAG_LONG(v)		FIELD_PREP(BIT(17), !!(v))
#define AVD_REF_DELTA_POC(v)		FIELD_PREP(GENMASK(16, 0), v)

static inline void push(struct avd_ctx *ctx, u32 inst)
{
	struct avd_job *job = &ctx->job;
	struct avd_segment *seg = &job->segments[job->num];

	if (seg->num >= AVD_MAX_INST) {
		job->overflow = true;
		return;
	}
	seg->instructions[seg->num++] = inst;
}

static inline void push_address(struct avd_ctx *ctx, dma_addr_t addr)
{
	push(ctx, addr >> 8);
}

/* The compressed planes are addressed in units of 128 bytes. */
static inline void push_comp(struct avd_ctx *ctx, dma_addr_t addr,
			     const struct avd_comp *comp)
{
	for (int i = 0; i < ARRAY_SIZE(comp->offsets); i++)
		push(ctx, (addr + comp->offsets[i]) >> 7);
}

#endif /* AVD_INST_H_ */
