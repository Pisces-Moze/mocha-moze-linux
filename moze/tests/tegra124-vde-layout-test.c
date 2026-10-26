// SPDX-License-Identifier: GPL-2.0-only
/* Independent per-byte scatter oracle versus the driver's 16-byte row copy. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../source-overlay/drivers/media/platform/nvidia/tegra-vde/tegra124-layout.h"

static size_t scatter_address(unsigned int x, unsigned int y, unsigned int pitch)
{
	/* Bit significance inside a 512-byte 64x8 GOB: x[3:0], y[0],
	 * x[4], y[2:1], x[5]. Outside it, GOBs progress by x, then y.
	 */
	size_t gob = ((size_t)(y >> 3) * (pitch >> 6) + (x >> 6)) << 9;
	return gob | (x & 15) | ((y & 1) << 4) | ((x & 16) << 1) |
	       ((y & 6) << 5) | ((x & 32) << 3);
}

static void check(unsigned int pitch, unsigned int height)
{
	size_t size = (size_t)pitch * height, x, y;
	u8 *linear = malloc(size), *raw = calloc(size + 32, 1);
	u8 *target = malloc(size + 32);
	assert(linear && raw && target);
	memset(target, 0xa5, size + 32);
	for (y = 0; y < height; y++)
		for (x = 0; x < pitch; x++) {
			linear[y * pitch + x] = (x * 13 + y * 37 + (x >> 6)) & 255;
			raw[scatter_address(x, y, pitch)] = linear[y * pitch + x];
		}
	assert(!tegra_vde_t124_linearize(target + 16, size, raw, size, pitch, height));
	assert(!memcmp(target + 16, linear, size));
	for (x = 0; x < 16; x++)
		assert(target[x] == 0xa5 && target[size + 16 + x] == 0xa5);
	assert(tegra_vde_t124_linearize(target, size - 1, raw, size, pitch, height) == -EINVAL);
	assert(tegra_vde_t124_linearize(target, size, raw, size - 1, pitch, height) == -EINVAL);
	free(linear); free(raw); free(target);
}

int main(void)
{
	unsigned int pitches[] = {64, 128, 192, 512, 768, 1024, 1536, 1920, 2048};
	unsigned int rows[] = {8, 16, 24, 544, 1088, 2032};
	size_t i, j;
	u8 byte = 0;
	assert(scatter_address(63, 7, 128) == 511);
	assert(scatter_address(64, 0, 128) == 512);
	assert(scatter_address(0, 8, 128) == 1024);
	assert(tegra_vde_t124_linearize(&byte, 1, &byte, 1, 0, 8) == -EINVAL);
	assert(tegra_vde_t124_linearize(&byte, 1, &byte, 1, 63, 8) == -EINVAL);
	assert(tegra_vde_t124_linearize(&byte, 1, &byte, 1, 64, 7) == -EINVAL);
	for (i = 0; i < sizeof(pitches) / sizeof(*pitches); i++)
		for (j = 0; j < sizeof(rows) / sizeof(*rows); j++)
			check(pitches[i], rows[j]);
	puts("T124_GOB_LAYOUT_54_GEOMETRIES_AND_BOUNDS_OK");
	return 0;
}
