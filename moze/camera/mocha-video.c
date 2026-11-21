// SPDX-License-Identifier: GPL-2.0-only
/* Standard V4L2 preview nodes over the Mocha VI2 RAW10 receiver. */
#include <linux/kthread.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <media/v4l2-common.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched/task.h>
#include <linux/util_macros.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "mocha-vi2-capture.h"

static const struct v4l2_frmsize_discrete mocha_sizes[] = {
	{ 320, 180 }, { 640, 360 }, { 1280, 720 },
};
static const unsigned int mocha_rates[] = { 5, 10, 15, 30 };

struct mocha_video_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head entry;
};

struct mocha_video {
	struct video_device vdev;
	struct v4l2_device v4l2;
	struct vb2_queue queue;
	struct mutex lock;
	spinlock_t pending_lock;
	struct list_head pending;
	wait_queue_head_t wait;
	struct task_struct *worker;
	struct i2c_client *client;
	mocha_sensor_power powered;
	mocha_sensor_action configure;
	int (*stream)(void *, bool);
	unsigned int sequence;
	unsigned int width, height, fps;
	u64 next_frame;
	bool rear;
	u8 tone[1024];
};

static atomic_t active_camera = ATOMIC_INIT(0);

static int mocha_tone(const struct mocha_video *camera, const __le16 *raw,
		      unsigned int x, unsigned int y)
{
	return camera->tone[le16_to_cpu(raw[y * MOCHA_VI2_WIDTH + x]) & 1023];
}

static void mocha_pixel(const struct mocha_video *camera, const __le16 *raw,
			unsigned int x, unsigned int y, unsigned int step,
			int *r, int *g, int *b)
{
	int a, c;

	if (step > 1) {
		a = mocha_tone(camera, raw, x, y);
		c = mocha_tone(camera, raw, x + 1, y + 1);
		*g = (mocha_tone(camera, raw, x + 1, y) +
		      mocha_tone(camera, raw, x, y + 1)) / 2;
	} else {
		unsigned int left = x ? x - 1 : x + 1;
		unsigned int right = x + 1 < MOCHA_VI2_WIDTH ? x + 1 : x - 1;
		unsigned int up = y ? y - 1 : y + 1;
		unsigned int down = y + 1 < MOCHA_VI2_HEIGHT ? y + 1 : y - 1;
		int center = mocha_tone(camera, raw, x, y);
		int horizontal = mocha_tone(camera, raw, left, y) +
				 mocha_tone(camera, raw, right, y);
		int vertical = mocha_tone(camera, raw, x, up) +
			       mocha_tone(camera, raw, x, down);

		if ((x ^ y) & 1) {
			*g = center;
			a = (y & 1 ? vertical : horizontal) / 2;
			c = (y & 1 ? horizontal : vertical) / 2;
		} else {
			int diagonal = (mocha_tone(camera, raw, left, up) +
					mocha_tone(camera, raw, right, up) +
					mocha_tone(camera, raw, left, down) +
					mocha_tone(camera, raw, right, down)) / 4;

			*g = (horizontal + vertical) / 4;
			a = y & 1 ? diagonal : center;
			c = y & 1 ? center : diagonal;
		}
	}
	*r = camera->rear ? a : c;
	*b = camera->rear ? c : a;
}

static void mocha_preview(const struct mocha_video *camera, const __le16 *raw, u8 *out)
{
	unsigned int step = MOCHA_VI2_WIDTH / camera->width;

	for (unsigned int y = 0; y < MOCHA_VI2_HEIGHT; y += step) {
		for (unsigned int x = 0; x < MOCHA_VI2_WIDTH; x += 2 * step) {
			int u = 0, v = 0;

			for (unsigned int i = 0; i < 2; i++) {
				int r, g, b;

				mocha_pixel(camera, raw, x + step * i, y, step, &r, &g, &b);
				out[2 * i] = 16 + ((66 * r + 129 * g + 25 * b + 128) >> 8);
				u += -38 * r - 74 * g + 112 * b;
				v += 112 * r - 94 * g - 18 * b;
			}
			out[1] = 128 + ((u + 256) >> 9);
			out[3] = 128 + ((v + 256) >> 9);
			out += 4;
		}
	}
}

static struct mocha_video_buffer *mocha_pop(struct mocha_video *camera)
{
	struct mocha_video_buffer *buffer;
	unsigned long flags;

	spin_lock_irqsave(&camera->pending_lock, flags);
	buffer = list_first_entry_or_null(&camera->pending, struct mocha_video_buffer, entry);
	if (buffer)
		list_del(&buffer->entry);
	spin_unlock_irqrestore(&camera->pending_lock, flags);
	return buffer;
}

static int mocha_deliver(void *data, const void *pixels)
{
	struct mocha_video *camera = data;
	struct mocha_video_buffer *buffer;
	struct vb2_buffer *vb;
	u64 now = ktime_get_ns();
	u64 period = NSEC_PER_SEC / READ_ONCE(camera->fps);

	if (kthread_should_stop())
		return -ECANCELED;
	if (now + NSEC_PER_MSEC * 2 < camera->next_frame)
		return 0;
	camera->next_frame = max(now, camera->next_frame + period);
	if (camera->next_frame == now)
		camera->next_frame += period;
	wait_event(camera->wait, kthread_should_stop() || !list_empty(&camera->pending));
	if (kthread_should_stop())
		return -ECANCELED;
	buffer = mocha_pop(camera);
	if (!buffer)
		return -EIO;
	vb = &buffer->vb.vb2_buf;
	vb->timestamp = ktime_get_ns();
	mocha_preview(camera, pixels, vb2_plane_vaddr(vb, 0));
	buffer->vb.sequence = camera->sequence++;
	buffer->vb.field = V4L2_FIELD_NONE;
	vb2_set_plane_payload(vb, 0, camera->width * camera->height * 2);
	vb2_buffer_done(vb, VB2_BUF_STATE_DONE);
	return 0;
}

static int mocha_session(struct i2c_client *client, void *data)
{
	struct mocha_video *camera = data;
	int ret = camera->configure(client, NULL);

	if (ret)
		return ret;
	return mocha_vi2_run_sensor(&client->dev, camera->stream, client, true,
				    camera->rear, mocha_deliver, camera);
}

static int mocha_worker(void *data)
{
	struct mocha_video *camera = data;
	int ret = camera->powered(camera->client, mocha_session, camera);

	if (ret && !kthread_should_stop()) {
		dev_warn(&camera->client->dev, "camera preview stopped: %d\n", ret);
		vb2_queue_error(&camera->queue);
	}
	return ret;
}

static int mocha_queue_setup(struct vb2_queue *queue, unsigned int *count,
			     unsigned int *planes, unsigned int sizes[],
			     struct device *alloc_devs[])
{
	struct mocha_video *camera = vb2_get_drv_priv(queue);
	unsigned int bytes = camera->width * camera->height * 2;

	if (*planes)
		return *planes == 1 && sizes[0] >= bytes ? 0 : -EINVAL;
	*planes = 1;
	sizes[0] = bytes;
	return 0;
}

static void mocha_queue_buffer(struct vb2_buffer *vb)
{
	struct mocha_video *camera = vb2_get_drv_priv(vb->vb2_queue);
	struct mocha_video_buffer *buffer = container_of(to_vb2_v4l2_buffer(vb),
							  struct mocha_video_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&camera->pending_lock, flags);
	list_add_tail(&buffer->entry, &camera->pending);
	spin_unlock_irqrestore(&camera->pending_lock, flags);
	wake_up(&camera->wait);
}

static void mocha_flush(struct mocha_video *camera, enum vb2_buffer_state state)
{
	struct mocha_video_buffer *buffer;

	while ((buffer = mocha_pop(camera)))
		vb2_buffer_done(&buffer->vb.vb2_buf, state);
}

static int mocha_start_streaming(struct vb2_queue *queue, unsigned int count)
{
	struct mocha_video *camera = vb2_get_drv_priv(queue);
	int ret;

	if (atomic_cmpxchg(&active_camera, 0, 1)) {
		ret = -EBUSY;
		goto fail;
	}
	camera->sequence = 0;
	camera->next_frame = 0;
	camera->worker = kthread_create(mocha_worker, camera, "mocha-camera");
	if (!IS_ERR(camera->worker)) {
		get_task_struct(camera->worker);
		wake_up_process(camera->worker);
		return 0;
	}

	ret = PTR_ERR(camera->worker);
	camera->worker = NULL;
	atomic_set(&active_camera, 0);
fail:
	mocha_flush(camera, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void mocha_stop_streaming(struct vb2_queue *queue)
{
	struct mocha_video *camera = vb2_get_drv_priv(queue);

	kthread_stop(camera->worker);
	put_task_struct(camera->worker);
	camera->worker = NULL;
	mocha_flush(camera, VB2_BUF_STATE_ERROR);
	atomic_set(&active_camera, 0);
}

static const struct vb2_ops mocha_vb2_ops = {
	.queue_setup = mocha_queue_setup,
	.buf_queue = mocha_queue_buffer,
	.start_streaming = mocha_start_streaming,
	.stop_streaming = mocha_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static int mocha_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	struct mocha_video *camera = video_drvdata(file);

	strscpy(cap->driver, "mocha-camera", sizeof(cap->driver));
	strscpy(cap->card, camera->vdev.name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:mocha-camera-%s",
		 camera->rear ? "rear" : "front");
	return 0;
}

static int mocha_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *fmt)
{
	if (fmt->index)
		return -EINVAL;
	fmt->pixelformat = V4L2_PIX_FMT_YUYV;
	return 0;
}

static void mocha_format(struct v4l2_pix_format *pix, unsigned int width,
			 unsigned int height)
{
	*pix = (struct v4l2_pix_format) {
		.width = width, .height = height,
		.pixelformat = V4L2_PIX_FMT_YUYV, .field = V4L2_FIELD_NONE,
		.bytesperline = width * 2, .sizeimage = width * height * 2,
		.colorspace = V4L2_COLORSPACE_SMPTE170M,
	};
}

static int mocha_g_fmt(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct mocha_video *camera = video_drvdata(file);

	mocha_format(&fmt->fmt.pix, camera->width, camera->height);
	return 0;
}

static int mocha_try_fmt(struct file *file, void *fh, struct v4l2_format *fmt)
{
	const struct v4l2_frmsize_discrete *size =
		v4l2_find_nearest_size(mocha_sizes, ARRAY_SIZE(mocha_sizes), width, height,
				       fmt->fmt.pix.width, fmt->fmt.pix.height);

	mocha_format(&fmt->fmt.pix, size->width, size->height);
	return 0;
}

static int mocha_s_fmt(struct file *file, void *fh, struct v4l2_format *fmt)
{
	struct mocha_video *camera = video_drvdata(file);

	if (vb2_is_busy(&camera->queue))
		return -EBUSY;
	mocha_try_fmt(file, fh, fmt);
	camera->width = fmt->fmt.pix.width;
	camera->height = fmt->fmt.pix.height;
	return 0;
}

static int mocha_framesizes(struct file *file, void *fh, struct v4l2_frmsizeenum *size)
{
	if (size->index >= ARRAY_SIZE(mocha_sizes) || size->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;
	size->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	size->discrete = mocha_sizes[size->index];
	return 0;
}

static int mocha_intervals(struct file *file, void *fh, struct v4l2_frmivalenum *ival)
{
	unsigned int i;

	if (ival->index >= ARRAY_SIZE(mocha_rates) || ival->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(mocha_sizes); i++)
		if (ival->width == mocha_sizes[i].width && ival->height == mocha_sizes[i].height)
			break;
	if (i == ARRAY_SIZE(mocha_sizes))
		return -EINVAL;
	ival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	ival->discrete = (struct v4l2_fract) { 1, mocha_rates[ival->index] };
	return 0;
}

static int mocha_g_parm(struct file *file, void *fh, struct v4l2_streamparm *parm)
{
	struct mocha_video *camera = video_drvdata(file);

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	parm->parm.capture = (struct v4l2_captureparm) {
		.capability = V4L2_CAP_TIMEPERFRAME,
		.timeperframe = { 1, READ_ONCE(camera->fps) },
	};
	return 0;
}

static int mocha_s_parm(struct file *file, void *fh, struct v4l2_streamparm *parm)
{
	struct mocha_video *camera = video_drvdata(file);
	struct v4l2_fract requested = parm->parm.capture.timeperframe;
	unsigned int fps = 30;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (requested.numerator && requested.denominator) {
		unsigned int target = DIV_ROUND_CLOSEST_ULL((u64)requested.denominator,
							    requested.numerator);

		fps = mocha_rates[find_closest(target, mocha_rates, ARRAY_SIZE(mocha_rates))];
	}
	WRITE_ONCE(camera->fps, fps);
	return mocha_g_parm(file, fh, parm);
}

static int mocha_enum_input(struct file *file, void *fh, struct v4l2_input *input)
{
	struct mocha_video *camera = video_drvdata(file);

	if (input->index)
		return -EINVAL;
	strscpy(input->name, camera->vdev.name, sizeof(input->name));
	input->type = V4L2_INPUT_TYPE_CAMERA;
	return 0;
}

static int mocha_g_input(struct file *file, void *fh, unsigned int *input)
{
	*input = 0;
	return 0;
}

static int mocha_s_input(struct file *file, void *fh, unsigned int input)
{
	return input ? -EINVAL : 0;
}

static const struct v4l2_ioctl_ops mocha_ioctls = {
	.vidioc_querycap = mocha_querycap,
	.vidioc_enum_input = mocha_enum_input,
	.vidioc_g_input = mocha_g_input,
	.vidioc_s_input = mocha_s_input,
	.vidioc_enum_fmt_vid_cap = mocha_enum_fmt,
	.vidioc_g_fmt_vid_cap = mocha_g_fmt,
	.vidioc_s_fmt_vid_cap = mocha_s_fmt,
	.vidioc_try_fmt_vid_cap = mocha_try_fmt,
	.vidioc_enum_framesizes = mocha_framesizes,
	.vidioc_enum_frameintervals = mocha_intervals,
	.vidioc_g_parm = mocha_g_parm,
	.vidioc_s_parm = mocha_s_parm,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations mocha_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.poll = vb2_fop_poll,
	.read = vb2_fop_read,
	.mmap = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

static void mocha_video_release(struct video_device *vdev)
{
	struct mocha_video *camera = video_get_drvdata(vdev);

	v4l2_device_unregister(&camera->v4l2);
	kfree(camera);
}

struct mocha_video *mocha_video_register(struct i2c_client *client, bool rear,
		mocha_sensor_power powered, mocha_sensor_action configure,
		int (*stream)(void *, bool))
{
	struct mocha_video *camera;
	int black = rear ? 64 : 16;
	int ret;

	camera = kzalloc(sizeof(*camera), GFP_KERNEL);
	if (!camera)
		return ERR_PTR(-ENOMEM);
	camera->client = client;
	camera->width = 640;
	camera->height = 360;
	camera->fps = 15;
	camera->rear = rear;
	camera->powered = powered;
	camera->configure = configure;
	camera->stream = stream;
	for (int i = black; i < ARRAY_SIZE(camera->tone); i++)
		camera->tone[i] = int_sqrt((i - black) * (255 * 255) / (1023 - black));

	mutex_init(&camera->lock);
	spin_lock_init(&camera->pending_lock);
	INIT_LIST_HEAD(&camera->pending);
	init_waitqueue_head(&camera->wait);
	camera->queue = (struct vb2_queue) {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.io_modes = VB2_MMAP | VB2_READ,
		.drv_priv = camera,
		.buf_struct_size = sizeof(struct mocha_video_buffer),
		.ops = &mocha_vb2_ops,
		.mem_ops = &vb2_vmalloc_memops,
		.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC,
		.min_queued_buffers = 1,
		.lock = &camera->lock,
		.dev = &client->dev,
	};
	ret = vb2_queue_init(&camera->queue);
	if (ret)
		goto free;
	ret = v4l2_device_register(&client->dev, &camera->v4l2);
	if (ret)
		goto free;
	camera->vdev = (struct video_device) {
		.v4l2_dev = &camera->v4l2,
		.fops = &mocha_fops,
		.ioctl_ops = &mocha_ioctls,
		.release = mocha_video_release,
		.lock = &camera->lock,
		.queue = &camera->queue,
		.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE,
	};
	strscpy(camera->vdev.name, rear ? "Mocha Rear Camera" : "Mocha Front Camera",
		sizeof(camera->vdev.name));
	video_set_drvdata(&camera->vdev, camera);
	ret = video_register_device(&camera->vdev, VFL_TYPE_VIDEO, -1);
	if (!ret)
		return camera;
	v4l2_device_unregister(&camera->v4l2);
free:
	kfree(camera);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(mocha_video_register);

void mocha_video_unregister(struct mocha_video *camera)
{
	vb2_video_unregister_device(&camera->vdev);
}
EXPORT_SYMBOL_GPL(mocha_video_unregister);
