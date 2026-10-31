/* SPDX-License-Identifier: GPL-2.0 */
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

#ifndef AVD_H_
#define AVD_H_

#include <linux/firmware.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>

#define AVD_CAPABILITY_HEVC	BIT(0)
#define AVD_CAPABILITY_H264	BIT(1)
#define AVD_CAPABILITY_VP9	BIT(2)

/* The decoder takes pixel buffer addresses shifted right by 8 (asahi avd). */
#define AVD_ALIGN		256
#define AVD_MAX_INST		512

struct avd_ctx;
struct avd_dev;

/* Header mode in the instruction stream and VP slot order (asahi avd). */
enum avd_codec {
	AVD_CODEC_HEVC = 0,
	AVD_CODEC_H264 = 1,
	AVD_CODEC_VP9 = 2,
	AVD_CODEC_NUM,
};

struct avd_run {
	struct vb2_v4l2_buffer *src;
	struct vb2_v4l2_buffer *dst;

	dma_addr_t coded_in;
	dma_addr_t y_out;
	dma_addr_t uv_out;
	dma_addr_t comp_out;
	dma_addr_t mv_out;
};

struct avd_comp {
	u32 size;
	u32 offsets[4];
};

/* CAPTURE plane: NV12, then the compressed reference copy, then MVs. */
struct avd_layout {
	u32 comp_offset;
	struct avd_comp comp;
	u32 mv_offset;
	u32 size;
};

struct avd_decoded_buffer {
	/* Must be the first field in this struct. */
	struct v4l2_m2m_buffer base;

	struct avd_layout layout;
	bool error;

	union {
		struct {
			u16 width;
			u16 height;
		} vp9;
		struct {
			bool is_intra;
		} hevc;
	};
};

static inline struct avd_decoded_buffer *
vb2_to_avd_decoded_buf(struct vb2_buffer *buf)
{
	return container_of(buf, struct avd_decoded_buffer, base.vb.vb2_buf);
}

struct avd_coded_fmt_ops {
	u32 (*mv_size)(u32 width, u32 height);
	int (*start)(struct avd_ctx *ctx);
	void (*stop)(struct avd_ctx *ctx);
	int (*run)(struct avd_ctx *ctx);
	void (*submit)(struct avd_ctx *ctx);
	void (*done)(struct avd_ctx *ctx, enum vb2_buffer_state result);
	int (*try_ctrl)(struct v4l2_ctrl *ctrl);
};

struct avd_ctrls {
	const struct v4l2_ctrl_config *ctrls;
	unsigned int num_ctrls;
};

struct avd_coded_fmt_desc {
	u32 fourcc;
	struct v4l2_frmsize_stepwise frmsize;
	const struct avd_ctrls *ctrls;
	const struct avd_coded_fmt_ops *ops;
	u32 frame_ctrl;
	u32 subsystem_flags;
	unsigned int capability;
};

struct avd_buf {
	void *cpu;
	dma_addr_t addr;
	size_t size;
};

struct avd_variant {
	unsigned int vp_slots[AVD_CODEC_NUM];
	unsigned int fifo_slots;
	unsigned int capabilities;
	const char *fw_name;
	u32 vp_slot_offset;
	u32 submit_offset;
	u32 submit_queue_max_offset;
	u32 submit_queue_status_offset;
};

struct avd_dev {
	struct device *dev;
	struct v4l2_device v4l2_dev;
	struct media_device mdev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;

	const struct firmware *fw;

	void __iomem *code;
	void __iomem *mbox;
	void __iomem *ctrl;

	int irq;
	struct avd_buf insn_fifo;

	/* Serialises the ioctls and the queues of all contexts. */
	struct mutex vdev_lock;
	/* Serialises programming a job against restarting the decoder. */
	struct mutex hw_lock;

	/* The job in flight, owned by whoever clears job_ctx under job_lock. */
	spinlock_t job_lock;
	struct avd_ctx *job_ctx;
	unsigned long job_start;
	struct delayed_work watchdog;
	struct work_struct recover_work;
	struct avd_ctx *recover_ctx;

	const struct avd_variant *variant;
	unsigned int capabilities;
};

struct avd_segment {
	unsigned int num;
	u32 instructions[AVD_MAX_INST];
};

/* Segment 0 is the frame header; every further segment is a slice or tile. */
struct avd_job {
	unsigned int num;
	unsigned int alloc;
	bool overflow;
	struct avd_segment *segments;
};

struct avd_ctx {
	struct v4l2_fh fh;
	struct avd_dev *dev;

	struct v4l2_format coded_fmt;
	struct v4l2_format decoded_fmt;

	const struct avd_coded_fmt_desc *coded_fmt_desc;
	struct v4l2_ctrl_handler ctrl_hdl;

	void *priv;

	struct avd_layout layout;
	struct avd_job job;
	bool ref_error;
};

static inline struct avd_ctx *file_to_ctx(struct file *filp)
{
	return container_of(file_to_v4l2_fh(filp), struct avd_ctx, fh);
}

struct avd_decoded_buffer *avd_get_ref_buf(struct avd_ctx *ctx,
					   struct avd_run *run, u64 timestamp);
dma_addr_t avd_buf_addr(struct avd_decoded_buffer *buf);
int avd_request_validate(struct media_request *req);
void *avd_ctrl_ptr(struct avd_ctx *ctx, u32 id);

int avd_job_begin(struct avd_ctx *ctx, unsigned int segments);
int avd_job_next(struct avd_ctx *ctx);
void avd_job_free(struct avd_ctx *ctx);
int avd_submit_job(struct avd_ctx *ctx, enum avd_codec codec);
void avd_submit_pp(struct avd_ctx *ctx, unsigned int count);

int avd_buf_alloc(struct avd_dev *avd, struct avd_buf *buf, size_t size);
void avd_buf_free(struct avd_dev *avd, struct avd_buf *buf);

void avd_reset_coded_fmt(struct avd_ctx *ctx);
void avd_reset_decoded_fmt(struct avd_ctx *ctx);
int avd_init_ctrls(struct avd_ctx *ctx);

void avd_job_finish_no_pm(struct avd_ctx *ctx, enum vb2_buffer_state result);
void avd_job_finish(struct avd_ctx *ctx, enum vb2_buffer_state result);

int avd_run_preamble(struct avd_ctx *ctx, struct avd_run *run);

extern const struct avd_coded_fmt_ops avd_h264_fmt_ops;
extern const struct avd_coded_fmt_ops avd_hevc_fmt_ops;
extern const struct avd_coded_fmt_ops avd_vp9_fmt_ops;

extern const struct v4l2_ioctl_ops avd_ioctl_ops;
extern const struct vb2_ops avd_queue_ops;

int avd_boot(struct avd_dev *avd);
void avd_shutdown(struct avd_dev *avd);
void avd_configure_stream(struct avd_dev *avd, dma_addr_t addr, u32 vp_slot);

#endif /* AVD_H_ */
