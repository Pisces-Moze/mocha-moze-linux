/* SPDX-License-Identifier: GPL-2.0-or-later
 * Background-only hooks for the existing Mocha early console. Its original
 * font, white foreground, positions, control characters and row wrap stay
 * in mocha_fb_write/newline. No new logger or reformatted message stream.
 */
#include <linux/sched/clock.h>
#include <linux/math64.h>
#include "moze-boot-art.h"

static unsigned char moze_early_ink[128][192];
static unsigned int moze_early_phase, moze_early_last;
static bool moze_early_enabled;

static u16 moze_early_background(unsigned int px, unsigned int py)
{
	return moze_rgb565(moze_art_pixel(px, py, moze_early_phase, 0));
}

static void moze_early_note(unsigned int col, unsigned int row, unsigned char ch)
{
	if (row < 128 && col < 192)
		moze_early_ink[row][col] = ch;
}

static void moze_early_clear(volatile u16 *fb, unsigned int row)
{
	unsigned int x, y;

	if (row >= 128)
		return;
	memset(moze_early_ink[row], 0, sizeof(moze_early_ink[row]));
	for (y = row * 16; y < (row + 1) * 16; y++)
		for (x = 0; x < 1536; x++)
			fb[y * 1536 + x] = moze_early_background(x, y);
}

static void moze_early_tick(volatile u16 *fb)
{
	unsigned int x, y, px, py, ms = (unsigned int)div_u64(sched_clock(), 1000000U);
	const unsigned char *font = font_vga_8x16.data;
	unsigned char ch;

	if (ms - moze_early_last < MOZE_RING_FRAME_MS)
		return;
	moze_early_phase = ms;
	for (y = MOZE_RING_Y; y < MOZE_RING_Y + MOZE_RING_H; y++)
		for (x = MOZE_RING_X; x < MOZE_RING_X + MOZE_RING_W; x++) {
			px = x;
			py = y;
			ch = moze_early_ink[py / 16][px / 8];
			if (!(ch && (font[ch * 16 + py % 16] &
				      (0x80 >> (px % 8)))))
				fb[py * 1536 + px] = moze_early_background(px, py);
		}
	moze_early_last = ms;
}

static void moze_early_init(volatile u16 *fb)
{
	unsigned int x, y;

	for (x = 0; x < 1536 * 2048; x++)
		fb[x] = 0;
	for (y = MOZE_ART_Y; y < MOZE_ART_Y + MOZE_ART_H; y++)
		for (x = MOZE_ART_X; x < MOZE_ART_X + MOZE_ART_W; x++)
			fb[y * 1536 + x] = moze_early_background(x, y);
	for (y = MOZE_BRAND_Y; y < MOZE_BRAND_Y + MOZE_BRAND_H; y++)
		for (x = MOZE_BRAND_X; x < MOZE_BRAND_X + MOZE_BRAND_W; x++)
			fb[y * 1536 + x] = moze_early_background(x, y);
	for (y = MOZE_FOOTER_Y; y < MOZE_FOOTER_Y + MOZE_FOOTER_H; y++)
		for (x = MOZE_FOOTER_X; x < MOZE_FOOTER_X + MOZE_FOOTER_W; x++)
			fb[y * 1536 + x] = moze_early_background(x, y);
	moze_early_enabled = true;
}
