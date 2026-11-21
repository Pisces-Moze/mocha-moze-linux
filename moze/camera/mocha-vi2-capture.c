// SPDX-License-Identifier: GPL-2.0-only
/* Tegra124 VI2 one-shot helpers; no platform registration or auto-probe. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/host1x.h>
#include <linux/jiffies.h>

#include "mocha-vi2-capture.h"

#define VI_INCR_SYNCPT			0x000
#define VI_INCR_SYNCPT_ERROR		0x008
#define CSI_SINGLE_SHOT			0x004
#define CSI_IMAGE_DEF			0x00c
#define CSI_IMAGE_SIZE			0x018
#define CSI_IMAGE_SIZE_WC		0x01c
#define CSI_IMAGE_DT			0x020
#define CSI_SURFACE0_MSB		0x024
#define CSI_SURFACE0_LSB		0x028
#define CSI_SURFACE0_STRIDE		0x054
#define CSI_ERROR_STATUS		0x084

#define CSI_RAW10			0x2b
#define VI_R16_I			32
#define VI_RAW_TO_MEMORY		(BIT(24) | (VI_R16_I << 16) | BIT(0))
#define VI_MWA_ACK_DONE			6
#define VI_MWB_ACK_DONE			7

int mocha_vi2_arm(const struct mocha_vi2_job *job, u32 *threshold)
{
	void __iomem *vi = job->vi;
	void __iomem *port = vi + (job->rear ? MOCHA_VI2_REAR_BASE : MOCHA_VI2_FRONT_BASE);
	u32 done = job->rear ? VI_MWA_ACK_DONE : VI_MWB_ACK_DONE;
	dma_addr_t dma = job->dma;
	u32 id = host1x_syncpt_id(job->syncpt);

	/* The vendor backend uses a 32-bit DMA address and a 256-byte stride. */
	if (job->bytes < MOCHA_VI2_FRAME_BYTES || upper_32_bits(dma) ||
	    lower_32_bits(dma) > U32_MAX - (MOCHA_VI2_FRAME_BYTES - 1) ||
	    !IS_ALIGNED(dma, 256) || id > 0xff)
		return -EINVAL;

	if (readl(port + CSI_SINGLE_SHOT))
		return -EBUSY;

	writel(U32_MAX, port + CSI_ERROR_STATUS);
	writel(CSI_RAW10, port + CSI_IMAGE_DT);
	writel(MOCHA_VI2_WIDTH * 10 / 8, port + CSI_IMAGE_SIZE_WC);
	writel(MOCHA_VI2_HEIGHT << 16 | MOCHA_VI2_WIDTH, port + CSI_IMAGE_SIZE);
	writel(0, port + CSI_SURFACE0_MSB);
	writel(lower_32_bits(dma), port + CSI_SURFACE0_LSB);
	writel(MOCHA_VI2_STRIDE, port + CSI_SURFACE0_STRIDE);
	writel(VI_RAW_TO_MEMORY, port + CSI_IMAGE_DEF);

	*threshold = host1x_syncpt_incr_max(job->syncpt, 1);
	writel(done << 8 | id, vi + VI_INCR_SYNCPT);
	writel(1, port + CSI_SINGLE_SHOT);
	return 0;
}

int mocha_vi2_wait(const struct mocha_vi2_job *job, u32 threshold)
{
	void __iomem *vi = job->vi;
	void __iomem *port = vi + (job->rear ? MOCHA_VI2_REAR_BASE : MOCHA_VI2_FRONT_BASE);
	u32 done = job->rear ? VI_MWA_ACK_DONE : VI_MWB_ACK_DONE;
	int ret;

	ret = host1x_syncpt_wait(job->syncpt, threshold, msecs_to_jiffies(200), NULL);
	if (ret)
		return ret;
	if (readl(port + CSI_ERROR_STATUS) ||
	    (readl(vi + VI_INCR_SYNCPT_ERROR) & BIT(done)))
		return -EIO;

	return 0;
}
