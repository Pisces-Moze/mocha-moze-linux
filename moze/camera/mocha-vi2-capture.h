// SPDX-License-Identifier: GPL-2.0-only
#ifndef MOCHA_VI2_CAPTURE_H
#define MOCHA_VI2_CAPTURE_H

#include <linux/dma-mapping.h>
#include <linux/io.h>

struct host1x_syncpt;
struct device;
struct i2c_client;
struct mocha_video;
struct mocha_vi2_job;

#define MOCHA_VI2_REAR_BASE 0x100
#define MOCHA_VI2_FRONT_BASE 0x200
#define MOCHA_VI2_WIDTH 1280
#define MOCHA_VI2_HEIGHT 720
#define MOCHA_VI2_STRIDE (MOCHA_VI2_WIDTH * 2)
#define MOCHA_VI2_FRAME_BYTES (MOCHA_VI2_STRIDE * MOCHA_VI2_HEIGHT)

/*
 * Caller owns the powered VI, calibrated CSI link and a
 * fresh client-managed host1x syncpoint, and serializes access. DMA must be
 * mapped to the VI device. On timeout do not recycle the buffer or syncpoint
 * before resetting/draining the capture engine.
 */
int mocha_vi2_arm(const struct mocha_vi2_job *job, u32 *threshold);
int mocha_vi2_wait(const struct mocha_vi2_job *job, u32 threshold);

struct tegra_mipi_device;

/* The platform must own powered VI exclusively before starting this job. */
typedef int (*mocha_frame_fn)(void *data, const void *pixels);

struct mocha_vi2_job {
	void __iomem *vi;
	struct host1x_syncpt *syncpt;
	struct tegra_mipi_device *mipi;
	dma_addr_t dma;
	size_t bytes;
	bool settle_sensor;
	bool rear;
	int (*stream)(void *sensor, bool enable);
	void *sensor;
	mocha_frame_fn frame;
	void *frame_data;
	void *pixels;
	struct device *dma_dev;
};

/* If dma_quiesced is false, retain the DMA mapping and syncpoint until
 * the platform has reset the entire VI. A failed job is not reusable.
 */
int mocha_vi2_capture(const struct mocha_vi2_job *job, bool *dma_quiesced);

int mocha_vi2_run_sensor(struct device *sensor, int (*stream)(void *, bool),
			 void *data, bool settle_sensor, bool rear,
			 mocha_frame_fn frame, void *frame_data);


typedef int (*mocha_sensor_action)(struct i2c_client *client, void *data);
typedef int (*mocha_sensor_power)(struct i2c_client *client,
				 mocha_sensor_action action, void *data);
struct mocha_video *mocha_video_register(struct i2c_client *client, bool rear,
		mocha_sensor_power powered, mocha_sensor_action configure,
		int (*stream)(void *, bool));
void mocha_video_unregister(struct mocha_video *video);

#endif
