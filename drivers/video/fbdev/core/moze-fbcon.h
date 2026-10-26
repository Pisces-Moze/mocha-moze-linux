/* SPDX-License-Identifier: GPL-2.0-or-later
 * Boot-only background composition beneath the stock fbcon rasterizers.
 * Incoming glyph images, font choice, ANSI colours, cursor and scrolling
 * remain fbcon's. No printk reader, replacement logger or text formatting.
 */
#if IS_BUILTIN(CONFIG_DRM) && IS_BUILTIN(CONFIG_FB_SYS_FILLRECT) && \
    IS_BUILTIN(CONFIG_FB_SYS_COPYAREA) && IS_BUILTIN(CONFIG_FB_SYS_IMAGEBLIT)
#include <drm/drm_fb_helper.h>
#include <linux/of.h>
#include <linux/math64.h>
#include <linux/timekeeping.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include "../../../tty/serial/moze-boot-art.h"

static bool moze_native_requested, moze_native_used;
static int __init moze_native_option(char *value)
{
	moze_native_requested = !strcmp(value, "1");
	return 1;
}
__setup("mocha_boot_animation=", moze_native_option);

static struct {
	struct fb_info *info, text, coverage;
	const struct fb_ops *original;
	struct fb_ops wrapped, software;
	struct delayed_work work;
	unsigned long deadline;
	u32 rgb_mask, alpha;
	size_t length;
	bool live;
} moze_native;

static u32 moze_native_channel(unsigned int value, struct fb_bitfield field)
{
	return field.length ? ((value * ((1u << field.length) - 1) + 127) / 255)
		<< field.offset : 0;
}

static bool moze_native_rect(u32 x, u32 y, u32 w, u32 h)
{
	return x <= 1536 && y <= 2048 && w <= 1536 - x && h <= 2048 - y;
}

static u32 moze_native_background(struct fb_info *info, u32 x, u32 y, u32 ms)
{
	u32 rgb = moze_art_pixel(x, y, ms, 0);

	if (!rgb)
		return moze_native.alpha;
	if (info->var.bits_per_pixel == 32 && info->var.red.length == 8 &&
	    info->var.green.length == 8 && info->var.blue.length == 8 &&
	    info->var.red.offset == 16 && info->var.green.offset == 8 &&
	    info->var.blue.offset == 0)
		return rgb | moze_native.alpha;
	return moze_native_channel((rgb >> 16) & 255, info->var.red) |
		moze_native_channel((rgb >> 8) & 255, info->var.green) |
		moze_native_channel(rgb & 255, info->var.blue) | moze_native.alpha;
}

static void moze_native_compose(u32 x, u32 y, u32 w, u32 h)
{
	struct fb_info *info = moze_native.info;
	struct drm_fb_helper *helper = info->par;
	u32 px, py, left, right, value, bytes = info->var.bits_per_pixel / 8;
	u32 ms = div_u64(ktime_get_boottime_ns(), NSEC_PER_MSEC);
	char *src, *dst;

	if (!moze_native_rect(x, y, w, h))
		return;
	for (py = y; py < y + h; py++) {
		/* Away from the four artwork rectangles, the native text plane
		 * already is the final image. Copy those scanlines directly.
		 * Full console scrolls must not sample millions of black pixels.
		 */
		memcpy(info->screen_buffer + py * info->fix.line_length + x * bytes,
		       moze_native.text.screen_buffer + py * info->fix.line_length + x * bytes,
		       w * bytes);
		if (moze_native.alpha) {
			/* Preserve opaque-background semantics on alpha scanouts. */
			left = x; right = x + w;
		} else if (py >= MOZE_ART_Y && py < MOZE_ART_Y + MOZE_ART_H) {
			left = MOZE_ART_X; right = left + MOZE_ART_W;
		} else if (py >= MOZE_BRAND_Y && py < MOZE_BRAND_Y + MOZE_BRAND_H) {
			left = MOZE_BRAND_X; right = left + MOZE_BRAND_W;
		} else if (py >= MOZE_FOOTER_Y && py < MOZE_FOOTER_Y + MOZE_FOOTER_H) {
			left = MOZE_FOOTER_X; right = left + MOZE_FOOTER_W;
		} else if (py >= MOZE_RING_Y && py < MOZE_RING_Y + MOZE_RING_H) {
			left = MOZE_RING_X; right = left + MOZE_RING_W;
		} else {
			continue;
		}
		left = max(left, x);
		right = min(right, x + w);
		for (px = left; px < right; px++) {
			src = moze_native.text.screen_buffer +
				py * info->fix.line_length + px * bytes;
			dst = info->screen_buffer + py * info->fix.line_length + px * bytes;
			if (moze_native.coverage.screen_buffer[py * 1536 + px]) {
				memcpy(dst, src, bytes);
			} else {
				value = moze_native_background(info, px, py, ms);
				memcpy(dst, &value, bytes);
			}
		}
	}
	/* Direct DMA scanout is already updated by these writes. Only shadow
	 * framebuffers provide fb_dirty and need a deferred damage copy. */
	if (helper->funcs->fb_dirty)
		drm_fb_helper_damage_area(info, x, y, w, h);
}

static void moze_native_fill(struct fb_info *info, const struct fb_fillrect *rect)
{
	struct fb_fillrect mask = *rect;
	u32 value;

	if (!moze_native_rect(rect->dx, rect->dy, rect->width, rect->height))
		return;
	sys_fillrect(&moze_native.text, rect);
	value = ((u32 *)info->pseudo_palette)[rect->color & 15];
	mask.color = (value & moze_native.rgb_mask) ? 1 : 0;
	sys_fillrect(&moze_native.coverage, &mask);
	moze_native_compose(rect->dx, rect->dy, rect->width, rect->height);
}

static void moze_native_copy(struct fb_info *info, const struct fb_copyarea *area)
{
	(void)info;
	if (!moze_native_rect(area->sx, area->sy, area->width, area->height) ||
	    !moze_native_rect(area->dx, area->dy, area->width, area->height))
		return;
	sys_copyarea(&moze_native.text, area);
	sys_copyarea(&moze_native.coverage, area);
	moze_native_compose(area->dx, area->dy, area->width, area->height);
}

static void moze_native_image(struct fb_info *info, const struct fb_image *image)
{
	struct fb_image mask = *image;
	struct fb_fillrect opaque = { .dx = image->dx, .dy = image->dy,
		.width = image->width, .height = image->height, .color = 1,
		.rop = ROP_COPY };
	u32 background;

	if (!moze_native_rect(image->dx, image->dy, image->width, image->height))
		return;
	/* These are the unmodified native rasterizers and unmodified font bits. */
	sys_imageblit(&moze_native.text, image);
	if (image->depth == 1) {
		background = ((u32 *)info->pseudo_palette)[image->bg_color & 15];
		mask.fg_color = 1;
		mask.bg_color = (background & moze_native.rgb_mask) ? 1 : 0;
		sys_imageblit(&moze_native.coverage, &mask);
	} else {
		sys_fillrect(&moze_native.coverage, &opaque);
	}
	moze_native_compose(image->dx, image->dy, image->width, image->height);
}

/* Called with console_lock. The static context outlives cancelled work. */
static void moze_native_detach(struct fb_info *info, bool restore_text)
{
	if (!moze_native.live || moze_native.info != info)
		return;
	moze_native.live = false;
	cancel_delayed_work(&moze_native.work);
	info->fbops = moze_native.original;
	if (restore_text) {
		struct drm_fb_helper *helper = info->par;

		memcpy(info->screen_buffer, moze_native.text.screen_buffer, moze_native.length);
		if (helper->funcs->fb_dirty)
			drm_fb_helper_damage_area(info, 0, 0, 1536, 2048);
	}
	vfree(moze_native.text.screen_buffer);
	vfree(moze_native.coverage.screen_buffer);
	moze_native.text.screen_buffer = NULL;
	moze_native.coverage.screen_buffer = NULL;
	moze_native.info = NULL;
}

static void moze_native_tick(struct work_struct *work)
{
	struct fb_info *info;
	struct vc_data *vc;
	(void)work;
	if (!console_trylock()) {
		schedule_delayed_work(&moze_native.work, msecs_to_jiffies(MOZE_RING_FRAME_MS));
		return;
	}
	if (!moze_native.live)
		goto out;
	info = moze_native.info;
	vc = vc_cons[fg_console].d;
	if (!vc || vc->vc_mode != KD_TEXT || time_after(jiffies, moze_native.deadline) ||
	    info->state != FBINFO_STATE_RUNNING || info->var.xoffset || info->var.yoffset) {
		moze_native_detach(info, vc && vc->vc_mode == KD_TEXT);
		goto out;
	}
	/* Refresh only the small loading ring; the original avatar is static.
	 * The coverage plane keeps default console glyphs above every update. */
	moze_native_compose(MOZE_RING_X, MOZE_RING_Y, MOZE_RING_W, MOZE_RING_H);
	schedule_delayed_work(&moze_native.work, msecs_to_jiffies(MOZE_RING_FRAME_MS));
out:
	console_unlock();
}

static void moze_native_attach(struct fb_info *info)
{
	u32 bits = info->var.bits_per_pixel;
	size_t bytes = (size_t)info->fix.line_length * 2048;

	if (!moze_native_requested || moze_native_used ||
	    (!of_machine_is_compatible("nvidia,mocha") &&
	     !of_machine_is_compatible("xiaomi,mocha")) ||
	    strcmp(info->fix.id, "tegradrmfb") || !(info->flags & FBINFO_VIRTFB) ||
	    !info->screen_buffer || !info->pseudo_palette ||
	    info->var.xres != 1536 || info->var.yres != 2048 ||
	    info->var.xres_virtual != 1536 || info->var.yres_virtual != 2048 ||
	    info->var.xoffset || info->var.yoffset || (bits != 16 && bits != 32) ||
	    info->var.red.length > 8 || info->var.green.length > 8 || info->var.blue.length > 8 ||
	    info->var.red.offset + info->var.red.length > bits ||
	    info->var.green.offset + info->var.green.length > bits ||
	    info->var.blue.offset + info->var.blue.length > bits ||
	    info->var.transp.length > 8 || info->var.transp.offset + info->var.transp.length > bits ||
	    info->fix.line_length < 1536 * (bits / 8) || bytes > 32 * 1024 * 1024 ||
	    (info->screen_size && info->screen_size < bytes) || info->fix.smem_len < bytes)
		return;
	moze_native.text = *info;
	moze_native.coverage = *info;
	moze_native.text.screen_buffer = vzalloc(bytes);
	moze_native.coverage.screen_buffer = vzalloc(1536 * 2048);
	if (!moze_native.text.screen_buffer || !moze_native.coverage.screen_buffer) {
		vfree(moze_native.text.screen_buffer);
		vfree(moze_native.coverage.screen_buffer);
		return;
	}
	moze_native.original = info->fbops;
	moze_native.wrapped = *info->fbops;
	moze_native.wrapped.fb_fillrect = moze_native_fill;
	moze_native.wrapped.fb_copyarea = moze_native_copy;
	moze_native.wrapped.fb_imageblit = moze_native_image;
	moze_native.text.fbops = &moze_native.software;
	moze_native.coverage.fbops = &moze_native.software;
	moze_native.coverage.var.bits_per_pixel = 8;
	moze_native.coverage.fix.visual = FB_VISUAL_PSEUDOCOLOR;
	moze_native.coverage.fix.line_length = 1536;
	moze_native.coverage.pseudo_palette = NULL;
	moze_native.info = info;
	moze_native.rgb_mask = moze_native_channel(255, info->var.red) |
		moze_native_channel(255, info->var.green) | moze_native_channel(255, info->var.blue);
	moze_native.alpha = moze_native_channel(255, info->var.transp);
	moze_native.length = bytes;
	moze_native.live = true;
	moze_native_used = true;
	info->fbops = &moze_native.wrapped;
	moze_native.deadline = jiffies + msecs_to_jiffies(45000);
	INIT_DELAYED_WORK(&moze_native.work, moze_native_tick);
	moze_native_compose(0, 0, 1536, 2048);
	pr_info("Moze boot background attached to %s (%ux%u, %u bpp)\n",
		info->fix.id, info->var.xres, info->var.yres, bits);
	schedule_delayed_work(&moze_native.work, msecs_to_jiffies(MOZE_RING_FRAME_MS));
}
#else
static inline void moze_native_attach(struct fb_info *info) { (void)info; }
static inline void moze_native_detach(struct fb_info *info, bool restore_text)
{ (void)info; (void)restore_text; }
#endif
