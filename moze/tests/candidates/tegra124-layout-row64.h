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
	size_t size;
	unsigned int x, y;

	if (!dst || !src || !pitch || !height || pitch % 64 || height % 8)
		return -EINVAL;
	if ((size_t)height > (size_t)-1 / pitch)
		return -EOVERFLOW;
	size = (size_t)pitch * height;
	if (size > dst_size || size > src_size)
		return -EINVAL;

	/* Keep destination traversal row-major. Four constant 16-byte lanes
	 * share one GOB base; this reduces arithmetic without strided row writes.
	 */
	for (y = 0; y < height; y++) {
		const u8 *line = src + (size_t)(y / 8) * pitch * 8
			+ ((y & 6) << 5) + ((y & 1) << 4);
		u8 *out = dst + (size_t)y * pitch;

		for (x = pitch; ; x -= 64) {
			memcpy(out, line, 16);
			memcpy(out + 16, line + 32, 16);
			memcpy(out + 32, line + 256, 16);
			memcpy(out + 48, line + 288, 16);
			/* Do not form a source pointer beyond its final GOB. */
			if (x == 64)
				break;
			out += 64;
			line += 512;
		}
	}
	return 0;
}
#endif
