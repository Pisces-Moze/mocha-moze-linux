// SPDX-License-Identifier: GPL-2.0-only
/* Mocha VI2 RAM resource check and explicit one-shot capture. */
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/debugfs.h>
#include <linux/vmalloc.h>
#include <linux/host1x.h>
#include <linux/crc32.h>
#include <linux/iommu.h>
#include <linux/slab.h>
#include <linux/regulator/consumer.h>
#include "mocha-vi2-capture.h"
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

static bool capture;
module_param(capture, bool, 0400);

static bool export_frame;
module_param(export_frame, bool, 0400);
MODULE_PARM_DESC(export_frame, "Expose the first captured frame to root through debugfs");

static struct dentry *frame_file;
static struct debugfs_blob_wrapper frame_blob;

static int capture_frame(struct platform_device *pdev, void __iomem *regs,
			 struct tegra_mipi_device *mipi,
			 int (*stream)(void *, bool), void *sensor,
			 bool settle_sensor, bool rear,
			 mocha_frame_fn frame, void *frame_data)
{
	struct device *dev = &pdev->dev;
	struct device *parent = dev->parent;
	struct host1x *host = parent ? dev_get_drvdata(parent) : NULL;
	struct mocha_vi2_job job = { .vi = regs, .mipi = mipi,
		.bytes = MOCHA_VI2_FRAME_BYTES, .settle_sensor = settle_sensor,
		.stream = stream, .sensor = sensor, .rear = rear,
		.frame = frame, .frame_data = frame_data, .dma_dev = dev };
	bool quiesced;
	void *pixels;
	int ret;

	if (export_frame && frame_file)
		return -EBUSY;
	if (!host || !device_iommu_mapped(dev))
		return -ENODEV;
	job.syncpt = host1x_syncpt_alloc(host, HOST1X_SYNCPT_CLIENT_MANAGED,
					dev_name(dev));
	if (!job.syncpt)
		return -ENOMEM;
	if (frame)
		pixels = dma_alloc_noncoherent(dev, job.bytes, &job.dma,
					       DMA_FROM_DEVICE, GFP_KERNEL);
	else
		pixels = dma_alloc_coherent(dev, job.bytes, &job.dma, GFP_KERNEL);
	if (!pixels) {
		ret = -ENOMEM;
		goto put_syncpt;
	}
	job.pixels = pixels;
	if (frame) {
		dma_sync_single_for_device(dev, job.dma, job.bytes, DMA_FROM_DEVICE);
	} else {
		memset(pixels, 0xa5, job.bytes);
		dma_wmb();
	}
	ret = mocha_vi2_capture(&job, &quiesced);
	if (!quiesced) {
		dev_crit(dev, "VI2 did not quiesce; retaining DMA buffer and syncpoint until reboot\n");
		return -EIO;
	}
	if (!ret && !frame) {
		dma_rmb();
		for (unsigned int row = 0; row < MOCHA_VI2_HEIGHT; row++) {
			if (!memchr_inv(pixels + row * MOCHA_VI2_STRIDE, 0xa5,
					MOCHA_VI2_STRIDE)) {
				dev_err(dev, "VI2 row %u was not written\n", row);
				ret = -ENODATA;
				break;
			}
		}
		if (!ret && !memchr_inv(pixels, *(u8 *)pixels, job.bytes))
			ret = -EIO;
		if (!ret && export_frame) {
			frame_blob.data = kvmalloc(job.bytes, GFP_KERNEL);
			if (!frame_blob.data) {
				ret = -ENOMEM;
			} else {
				frame_blob.size = job.bytes;
				memcpy(frame_blob.data, pixels, job.bytes);
				frame_file = debugfs_create_blob("mocha-camera-raw", 0400,
								 NULL, &frame_blob);
				if (IS_ERR(frame_file)) {
					ret = PTR_ERR(frame_file);
					frame_file = NULL;
					kvfree(frame_blob.data);
					frame_blob.data = NULL;
				}
			}
		}
		if (!ret)
			dev_info(dev, "VI2 test frame DMA complete: %zu bytes CRC32=%08x\n",
				 job.bytes, crc32_le(0, pixels, job.bytes));
	}
	if (frame)
		dma_free_noncoherent(dev, job.bytes, pixels, job.dma, DMA_FROM_DEVICE);
	else
		dma_free_coherent(dev, job.bytes, pixels, job.dma);
put_syncpt:
	host1x_syncpt_put(job.syncpt);
	return ret;
}

static int mocha_vi2_run(struct platform_device *pdev,
			 int (*stream)(void *, bool), void *sensor,
			 bool settle_sensor, struct device_node *rear,
			 mocha_frame_fn frame, void *frame_data)
{
	struct device *dev = &pdev->dev;
	struct clk_bulk_data clocks[] = {
		{ .id = "vi" }, { .id = "csi" }, { .id = "cile" },
		{ .id = "cilab" }, { .id = "cilcd" },
	};
	struct tegra_mipi_device *mipi;
	struct regulator *avdd = NULL;
	void __iomem *regs;
	unsigned long old_rate = 0;
	int ret, num_clocks = stream ? ARRAY_SIZE(clocks) : 3;

	if (!of_machine_is_compatible("nvidia,mocha"))
		return -ENODEV;
	if (!dev->pm_domain)
		return dev_err_probe(dev, -EPROBE_DEFER, "VI power domain missing\n");

	regs = platform_get_drvdata(pdev);
	if (!regs)
		return -ENODEV;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "cannot set VI DMA mask\n");
	/* The original VI master enables CILAB, and its slave enables
	 * CILCD and CILE. Keep the smaller resource-only probe unchanged.
	 */

	ret = clk_bulk_get(dev, num_clocks, clocks);
	if (ret)
		return dev_err_probe(dev, ret, "cannot get VI/CSI clocks\n");
	mipi = tegra_mipi_request(dev, rear ?: dev->of_node);
	if (IS_ERR_OR_NULL(mipi)) {
		ret = mipi ? PTR_ERR(mipi) : -ENODEV;
		dev_err_probe(dev, ret, "cannot get CSI calibration provider\n");
		goto put_clocks;
	}

	if (stream) {
		avdd = regulator_get(dev, "avdd-dsi-csi");
		if (IS_ERR(avdd)) {
			ret = dev_err_probe(dev, PTR_ERR(avdd), "cannot get CSI analog supply\n");
			goto free_mipi;
		}
		ret = regulator_enable(avdd);
		if (ret)
			goto put_supply;
		dev_info(dev, "CSI analog supply: %d uV\n",
			 regulator_get_voltage(avdd));
	}

	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		dev_err_probe(dev, ret, "cannot resume VENC domain\n");
		goto disable_pm;
	}
	if (rear || frame) {
		old_rate = clk_get_rate(clocks[0].clk);
		ret = clk_set_rate(clocks[0].clk, 204000000);
		if (ret)
			goto suspend;
	}
	ret = clk_bulk_prepare_enable(num_clocks, clocks);
	if (ret) {
		dev_err_probe(dev, ret, "cannot enable VI/CSI clocks\n");
	} else {
		dev_info(dev, "VI2 resources online: VI=%lu CSI=%lu CILE=%lu Hz, DMA mask 32-bit; capture not armed\n",
			 clk_get_rate(clocks[0].clk), clk_get_rate(clocks[1].clk),
			 clk_get_rate(clocks[2].clk));
		if (stream) {
			dev_info(dev, "VI2 shared receiver clocks: CILAB=%lu CILCD=%lu Hz\n",
				 clk_get_rate(clocks[3].clk), clk_get_rate(clocks[4].clk));
			ret = capture_frame(pdev, regs, mipi, stream, sensor, settle_sensor, !!rear,
					    frame, frame_data);
			if (frame && (ret == -EAGAIN || ret == -ETIMEDOUT))
				ret = capture_frame(pdev, regs, mipi, stream, sensor,
						    settle_sensor, !!rear, frame, frame_data);
		}
		clk_bulk_disable_unprepare(num_clocks, clocks);
	}
suspend:
	if (old_rate)
		clk_set_rate(clocks[0].clk, old_rate);
	{
		int suspend = pm_runtime_put_sync_suspend(dev);

		if (suspend < 0) {
			dev_err(dev, "cannot suspend VENC domain: %d\n", suspend);
			if (!ret)
				ret = suspend;
		}
	}
	if (!ret)
		dev_info(dev, "VI2 resource check complete\n");

disable_pm:
	pm_runtime_disable(dev);
	if (avdd)
		regulator_disable(avdd);
put_supply:
	if (avdd)
		regulator_put(avdd);
free_mipi:
	tegra_mipi_free(mipi);
put_clocks:
	clk_bulk_put(num_clocks, clocks);
	return ret;
}

static int mocha_vi2_probe(struct platform_device *pdev)
{
	void __iomem *regs = devm_platform_ioremap_resource(pdev, 0);
	int ret;

	if (!of_machine_is_compatible("nvidia,mocha"))
		return -ENODEV;
	if (IS_ERR(regs))
		return PTR_ERR(regs);
	platform_set_drvdata(pdev, regs);
	ret = mocha_vi2_run(pdev, NULL, NULL, false, NULL, NULL, NULL);
	if (ret)
		platform_set_drvdata(pdev, NULL);
	return ret;
}

int mocha_vi2_run_sensor(struct device *sensor, int (*stream)(void *, bool),
			 void *data, bool settle_sensor, bool rear,
			 mocha_frame_fn frame, void *frame_data)
{
	struct device_node *node;
	struct platform_device *pdev;
	int ret = -ENODEV;

	if (!capture && !frame)
		return -EPERM;
	node = of_parse_phandle(sensor->of_node, "moze,vi2", 0);
	if (!node)
		return -ENODEV;
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return -EPROBE_DEFER;
	device_lock(&pdev->dev);
	if (pdev->dev.driver &&
	    !strcmp(pdev->dev.driver->name, "mocha-vi2-probe"))
		ret = mocha_vi2_run(pdev, stream, data, settle_sensor,
				   rear ? sensor->of_node : NULL, frame, frame_data);
	device_unlock(&pdev->dev);
	put_device(&pdev->dev);
	return ret;
}
EXPORT_SYMBOL_GPL(mocha_vi2_run_sensor);

static const struct of_device_id mocha_vi2_match[] = {
	{ .compatible = "moze,mocha-vi2-resource-probe" },
	{ }
};

static struct platform_driver mocha_vi2_driver = {
	.probe = mocha_vi2_probe,
	.driver = {
		.name = "mocha-vi2-probe",
		.of_match_table = mocha_vi2_match,
	},
};
static int __init mocha_vi2_init(void)
{
	return platform_driver_register(&mocha_vi2_driver);
}

static void __exit mocha_vi2_exit(void)
{
	platform_driver_unregister(&mocha_vi2_driver);
	debugfs_remove(frame_file);
	kvfree(frame_blob.data);
}

module_init(mocha_vi2_init);
module_exit(mocha_vi2_exit);

MODULE_DESCRIPTION("Mocha VI2 RAM resource probe");
MODULE_LICENSE("GPL");
