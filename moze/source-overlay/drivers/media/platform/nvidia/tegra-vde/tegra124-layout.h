/* SPDX-License-Identifier: GPL-2.0-only */
/* Tegra124 16Bx2 block-linear, one GOB high, to a linear plane.
 * Layout follows NVIDIA's DRM modifier and Chromium minigbm's Tegra layout.
 * This compatibility copy is explicit; it is not a zero-copy output path.
 */
#ifndef TEGRA_VDE_TEGRA124_LAYOUT_H
#define TEGRA_VDE_TEGRA124_LAYOUT_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t u8;
#endif

static inline int tegra_vde_t124_linearize(void *destination, size_t dst_size,
					const void *source, size_t src_size,
					unsigned int pitch, unsigned int height)
{
	u8 *dst = destination;
	const u8 *src = source;
	size_t size, gob_row;
	unsigned int x, y;

	if (!dst || !src || !pitch || !height || pitch % 64 || height % 8)
		return -EINVAL;
	if ((size_t)height > (size_t)-1 / pitch)
		return -EOVERFLOW;
	size = (size_t)pitch * height;
	if (size > dst_size || size > src_size)
		return -EINVAL;

	for (y = 0; y < height; y++) {
		gob_row = (size_t)(y / 8) * pitch * 8;
		for (x = 0; x < pitch; x += 16) {
			size_t offset = gob_row + (size_t)(x / 64) * 512;

			offset += ((x % 64) / 32) * 256;
			offset += ((y % 8) / 2) * 64;
			offset += ((x % 32) / 16) * 32 + (y % 2) * 16;
			memcpy(dst + (size_t)y * pitch + x, src + offset, 16);
		}
	}
	return 0;
}
#endif
