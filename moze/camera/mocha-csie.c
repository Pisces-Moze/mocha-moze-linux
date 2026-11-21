// SPDX-License-Identifier: GPL-2.0-only
/* Tegra124 CSI receiver for the Mocha front and rear sensors. */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/host1x.h>
#include <linux/iopoll.h>
#include <soc/tegra/pmc.h>

#include "mocha-vi2-capture.h"

/* Tegra124 routes CIL E to parser B and VI channel 1. */
#define CSIA_PARSER_BASE		0x838
#define CSIA_CIL_BASE		0x92c
#define CSIB_CIL_BASE		0x960
#define CSIE_PARSER_BASE		0x86c
#define CSIE_CIL_BASE		0xa08
#define CSI_PHY_COMMAND		0x908
#define CSI_PHY_ENABLE_AB	0x0101
#define CSI_PHY_DISABLE_AB	0x0202
#define CSI_INPUT_SINGLE_LANE	(0x3f << 16)
#define CSI_PHY_ENABLE_E	0x12020000
#define CSI_PHY_DISABLE_E	0x22020000
#define CSI_PARSER_COMMAND	0x010
#define CSI_PARSER_STATUS	0x01c
#define CSI_PARSER_MARKER	(0xf << 12)
#define CSI_PARSER_SINGLE	BIT(2)
#define VI_SYNCPT_CONTROL	0x004
#define VI_SYNCPT_ERROR		0x008
#define VI_CLOCK_GATING		0x0b8
#define VI_RESET		0x000
#define VI_SINGLE_SHOT		0x004
#define VI_IMAGE_DEF		0x00c

static void mocha_csi_set_phy(void __iomem *vi, bool rear, u32 command)
{
	u32 keep = rear ? GENMASK(31, 16) : GENMASK(15, 0);
	u32 phy = readl(vi + CSI_PHY_COMMAND);

	writel((phy & keep) | command, vi + CSI_PHY_COMMAND);
}

static void mocha_csi_configure(const struct mocha_vi2_job *job,
				void __iomem *pp, void __iomem *cil, u32 input)
{
	void __iomem *vi = job->vi;

	if (job->rear) {
		writel(BIT(16), cil);
		writel(0, vi + CSIB_CIL_BASE);
		writel(0, vi + CSIB_CIL_BASE + 0x00c);
		writel(U32_MAX, vi + CSIB_CIL_BASE + 0x010);
		writel(0x9, vi + CSIB_CIL_BASE + 0x008);
	} else {
		writel(BIT(16), vi + 0x994);
		writel(0, vi + 0x9c8);
		writel(0, vi + 0x9a0);
		writel(0, vi + 0x9d4);
		writel(0, cil);
	}
	writel(U32_MAX, pp + CSI_PARSER_STATUS);
	writel(U32_MAX, cil + 0x010);
	writel(0, cil + 0x00c);
	writel(0x9, cil + 0x008);

	writel(CSI_PARSER_MARKER | CSI_PARSER_SINGLE | 3, pp + CSI_PARSER_COMMAND);
	writel(0, pp + 0x018);
	writel(job->rear ? 0x280301f0 : 0x280301f1, pp + 0x004);
	writel(0x11, pp + 0x008);
	writel(0x14 << 16, pp + 0x00c);
	writel(0, pp + 0x014);
	/* The final OV5693 vendor mode writes 0x11 to lane register 0x3011. */
	writel(input, pp);
	mocha_csi_set_phy(vi, job->rear, job->rear ? CSI_PHY_ENABLE_AB : CSI_PHY_ENABLE_E);
}

static void mocha_csi_start(void __iomem *pp, bool continuous)
{
	writel(U32_MAX, pp + CSI_PARSER_STATUS);
	writel(CSI_PARSER_MARKER | (continuous ? 0 : CSI_PARSER_SINGLE) | 1,
	       pp + CSI_PARSER_COMMAND);
}

static int mocha_csi_stop(const struct mocha_vi2_job *job,
			  void __iomem *pp, void __iomem *port)
{
	void __iomem *vi = job->vi;
	u32 gating, shot;
	int ret;

	writel(CSI_PARSER_MARKER | 2, pp + CSI_PARSER_COMMAND);
	mocha_csi_set_phy(vi, job->rear, job->rear ? CSI_PHY_DISABLE_AB : CSI_PHY_DISABLE_E);

	/* Match the vendor VI reset sequence with clock gating disabled. */
	gating = readl(vi + VI_CLOCK_GATING);
	writel(0, vi + VI_CLOCK_GATING);
	writel(0, port + VI_IMAGE_DEF);
	writel(0xf, port + VI_RESET);
	readl(port + VI_RESET);
	usleep_range(100, 200);
	writel(0, port + VI_RESET);
	ret = readl_poll_timeout(port + VI_SINGLE_SHOT, shot, !shot, 10, 2000);
	writel(gating, vi + VI_CLOCK_GATING);
	return ret;
}

int mocha_vi2_capture(const struct mocha_vi2_job *job, bool *dma_quiesced)
{
	void __iomem *vi = job->vi;
	void __iomem *pp = vi + (job->rear ? CSIA_PARSER_BASE : CSIE_PARSER_BASE);
	void __iomem *cil = vi + (job->rear ? CSIA_CIL_BASE : CSIE_CIL_BASE);
	void __iomem *port = vi + (job->rear ? MOCHA_VI2_REAR_BASE : MOCHA_VI2_FRONT_BASE);
	enum tegra_io_pad pad = job->rear ? TEGRA_IO_PAD_CSIA : TEGRA_IO_PAD_CSIE;
	u32 lanes = CSI_INPUT_SINGLE_LANE | (job->rear ? 3 : 0);
	int ret, stop, cal;
	u32 threshold;

	*dma_quiesced = true;
	ret = tegra_io_pad_power_enable(pad);
	if (ret) {
		pr_err("Mocha VI2: cannot power primary CSI pad: %d\n", ret);
		return ret;
	}
	if (job->rear) {
		ret = tegra_io_pad_power_enable(TEGRA_IO_PAD_CSIB);
		if (ret)
			goto disable_primary;
	}
	ret = tegra_mipi_enable(job->mipi);
	if (ret)
		goto disable_pad;
	ret = tegra_mipi_start_calibration(job->mipi);
	if (ret)
		goto disable_mipi;

	writel(U32_MAX, vi + VI_SYNCPT_ERROR);
	writel(BIT(8), vi + VI_SYNCPT_CONTROL);
	mocha_csi_configure(job, pp, cil, lanes);

	/* RW fields must latch before committing a DMA address. */
	{
		u32 input = readl(pp);
		u32 phy = readl(cil + 0x008);

		pr_info("Mocha VI2: pre-arm %s input=%08x CIL PHY=%08x\n",
			job->rear ? "CSIA" : "CSIE", input, phy);
		if (input != lanes || (phy & 0x7f) != 0x9) {
			ret = -EIO;
			tegra_mipi_finish_calibration(job->mipi);
			mocha_csi_stop(job, pp, port);
			goto disable_mipi;
		}
	}

	/* Internal color bars are valid immediately. Optical capture skips
	 * the startup frames so the sensor latches its exposure first.
	 */
	if (job->settle_sensor) {
		ret = job->stream(job->sensor, true);
		cal = tegra_mipi_finish_calibration(job->mipi);
		if (!ret && !cal) {
			msleep(150);
			*dma_quiesced = false;
			ret = mocha_vi2_arm(job, &threshold);
			if (!ret)
				mocha_csi_start(pp, !!job->frame);
		} else if (!ret) {
			ret = cal;
		}
	} else {
		mocha_csi_start(pp, !!job->frame);
		*dma_quiesced = false;
		ret = mocha_vi2_arm(job, &threshold);
		if (!ret)
			ret = job->stream(job->sensor, true);
		cal = tegra_mipi_finish_calibration(job->mipi);
	}
	pr_info("Mocha VI2: arm/stream=%d MIPI calibration=%d\n", ret, cal);
	/* Diagnose DMA independently of calibration, but require both to pass. */
	if (!ret) {
		ret = mocha_vi2_wait(job, threshold);
		pr_info("Mocha VI2: DMA wait=%d counter=%u threshold=%u\n",
			ret, host1x_syncpt_read(job->syncpt), threshold);
	}
	/* A streaming parser stays active between successive VI single shots. */
	while (!ret && !cal && job->frame) {
		dma_sync_single_for_cpu(job->dma_dev, job->dma, job->bytes, DMA_FROM_DEVICE);
		ret = job->frame(job->frame_data, job->pixels);
		dma_sync_single_for_device(job->dma_dev, job->dma, job->bytes, DMA_FROM_DEVICE);
		if (ret == -ECANCELED) {
			ret = 0;
			break;
		}
		if (!ret)
			ret = mocha_vi2_arm(job, &threshold);
		if (!ret)
			ret = mocha_vi2_wait(job, threshold);
	}
	if (ret || cal)
		pr_info("Mocha VI2: PP command=%08x PHY command=%08x VI format=%08x parser=%08x CIL=%08x VI error=%08x shot=%08x\n",
			readl(pp + CSI_PARSER_COMMAND),
			readl(vi + CSI_PHY_COMMAND),
			readl(port + VI_IMAGE_DEF),
			readl(pp + CSI_PARSER_STATUS),
			readl(cil + 0x010),
			readl(port + 0x084),
			readl(port + VI_SINGLE_SHOT));
	if (!ret && cal)
		ret = cal;

	job->stream(job->sensor, false);
	stop = mocha_csi_stop(job, pp, port);
	*dma_quiesced = !stop;
	if (!ret)
		ret = stop;

disable_mipi:
	tegra_mipi_disable(job->mipi);
disable_pad:
	if (job->rear) {
		cal = tegra_io_pad_power_disable(TEGRA_IO_PAD_CSIB);
		if (!ret)
			ret = cal;
	}
disable_primary:
	cal = tegra_io_pad_power_disable(pad);
	return ret ? ret : cal;
}
