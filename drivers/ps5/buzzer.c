// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/ps5.h>

#define BUZZER_IOC_MAGIC	'B'

#define PS5_BUZZ_SILENT		0
#define PS5_BUZZ_SHORT		1
#define PS5_BUZZ_ERROR		2
#define PS5_BUZZ_LONG		3

struct ps5_buzzer_step {
	__u8  value;
	__u16 hold_ms;
} __attribute__((packed));

struct ps5_buzzer_pattern {
	__u32 nsteps;
	struct ps5_buzzer_step steps[];
};

#define PS5_BUZZER_BEEP		_IOW(BUZZER_IOC_MAGIC, 1, __u8)
#define PS5_BUZZER_PLAY		_IOW(BUZZER_IOC_MAGIC, 2, struct ps5_buzzer_pattern)
#define PS5_BUZZER_STOP		_IO(BUZZER_IOC_MAGIC, 3)

#define MAX_PATTERN_STEPS	1024

static struct input_dev *snd_idev;
static struct task_struct *play_thread;
static DEFINE_MUTEX(play_lock);

static int ps5_buzz_raw(u8 level)
{
	u8 buf[ICC_MSG_MAX_SIZE] = {};
	struct icc_msg *msg = (struct icc_msg *)buf;

	if (level > PS5_BUZZ_LONG)
		return -EINVAL;

	msg->service_id = ICC_SERVICE_ID_INDICATOR;
	msg->msg_type   = 0x00;
	msg->length     = 0x20;
	msg->data[0]    = level;

	return icc_query(buf, buf);
}

struct play_ctx {
	struct ps5_buzzer_step *steps;
	u32 nsteps;
};

static int play_fn(void *data)
{
	struct play_ctx *ctx = data;
	u32 i;

	for (i = 0; i < ctx->nsteps && !kthread_should_stop(); i++) {
		ps5_buzz_raw(ctx->steps[i].value);
		if (ctx->steps[i].hold_ms)
			msleep_interruptible(ctx->steps[i].hold_ms);
	}

	kfree(ctx->steps);
	kfree(ctx);

	mutex_lock(&play_lock);
	play_thread = NULL;
	mutex_unlock(&play_lock);
	return 0;
}

static int play_start(struct ps5_buzzer_step *steps, u32 nsteps)
{
	struct play_ctx *ctx;
	struct task_struct *t;

	mutex_lock(&play_lock);
	if (play_thread) {
		mutex_unlock(&play_lock);
		return -EBUSY;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		mutex_unlock(&play_lock);
		return -ENOMEM;
	}
	ctx->steps  = steps;
	ctx->nsteps = nsteps;

	t = kthread_run(play_fn, ctx, "ps5_buzz_play");
	if (IS_ERR(t)) {
		kfree(ctx);
		mutex_unlock(&play_lock);
		return PTR_ERR(t);
	}
	play_thread = t;
	mutex_unlock(&play_lock);
	return 0;
}

static int play_stop(void)
{
	mutex_lock(&play_lock);
	if (play_thread) {
		kthread_stop(play_thread);
		play_thread = NULL;
	}
	mutex_unlock(&play_lock);
	return 0;
}

static ssize_t buzz_write(struct file *f, const char __user *ubuf,
			  size_t n, loff_t *pos)
{
	char c;
	int ret;

	if (n < 1)
		return -EINVAL;
	if (get_user(c, ubuf))
		return -EFAULT;
	if (c < '0' || c > '3')
		return -EINVAL;

	ret = ps5_buzz_raw(c - '0');
	return ret ? ret : n;
}

static long buzz_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case PS5_BUZZER_BEEP: {
		u8 level;
		if (get_user(level, (u8 __user *)arg))
			return -EFAULT;
		return ps5_buzz_raw(level);
	}
	case PS5_BUZZER_PLAY: {
		struct ps5_buzzer_pattern hdr;
		struct ps5_buzzer_step *steps;
		size_t bytes;
		int ret;

		if (copy_from_user(&hdr, (void __user *)arg, sizeof(hdr)))
			return -EFAULT;
		if (!hdr.nsteps || hdr.nsteps > MAX_PATTERN_STEPS)
			return -EINVAL;

		bytes = hdr.nsteps * sizeof(struct ps5_buzzer_step);
		steps = kmalloc(bytes, GFP_KERNEL);
		if (!steps)
			return -ENOMEM;
		if (copy_from_user(steps,
				   (void __user *)(arg + sizeof(hdr)), bytes)) {
			kfree(steps);
			return -EFAULT;
		}
		ret = play_start(steps, hdr.nsteps);
		if (ret)
			kfree(steps);
		return ret;
	}
	case PS5_BUZZER_STOP:
		return play_stop();
	}
	return -ENOTTY;
}

static const struct file_operations buzz_fops = {
	.owner          = THIS_MODULE,
	.write          = buzz_write,
	.unlocked_ioctl = buzz_ioctl,
	.llseek         = noop_llseek,
};

static struct miscdevice buzz_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "ps5_buzzer",
	.fops  = &buzz_fops,
	.mode  = 0660,
};

static atomic_t bell_state = ATOMIC_INIT(PS5_BUZZ_SILENT);

static void bell_work_fn(struct work_struct *w)
{
	ps5_buzz_raw(atomic_read(&bell_state));
}
static DECLARE_WORK(bell_work, bell_work_fn);

static int snd_event(struct input_dev *dev, unsigned int type,
		     unsigned int code, int value)
{
	if (type == EV_SND && code == SND_BELL) {
		atomic_set(&bell_state, value ? PS5_BUZZ_SHORT : PS5_BUZZ_SILENT);
		schedule_work(&bell_work);
	}
	return 0;
}

static ssize_t beep_store(struct device *d, struct device_attribute *a,
			  const char *buf, size_t n)
{
	u8 level;
	int ret;

	if (kstrtou8(buf, 0, &level))
		return -EINVAL;
	ret = ps5_buzz_raw(level);
	return ret ? ret : n;
}
static DEVICE_ATTR_WO(beep);

static ssize_t stop_store(struct device *d, struct device_attribute *a,
			  const char *buf, size_t n)
{
	play_stop();
	return n;
}
static DEVICE_ATTR_WO(stop);

static struct attribute *buzz_attrs[] = {
	&dev_attr_beep.attr,
	&dev_attr_stop.attr,
	NULL,
};

static const struct attribute_group buzz_attr_group = {
	.attrs = buzz_attrs,
};

static int __init ps5_buzzer_init(void)
{
	int ret;

	ret = misc_register(&buzz_misc);
	if (ret)
		return ret;

	ret = sysfs_create_group(&buzz_misc.this_device->kobj, &buzz_attr_group);
	if (ret)
		goto err_misc;

	snd_idev = input_allocate_device();
	if (!snd_idev) {
		ret = -ENOMEM;
		goto err_sysfs;
	}
	snd_idev->name  = "PS5 Chassis Buzzer";
	snd_idev->phys  = "ps5_buzzer/input0";
	snd_idev->id.bustype = BUS_VIRTUAL;
	snd_idev->event = snd_event;
	input_set_capability(snd_idev, EV_SND, SND_BELL);

	ret = input_register_device(snd_idev);
	if (ret)
		goto err_input;

	return 0;

err_input:
	input_free_device(snd_idev);
	snd_idev = NULL;
err_sysfs:
	sysfs_remove_group(&buzz_misc.this_device->kobj, &buzz_attr_group);
err_misc:
	misc_deregister(&buzz_misc);
	return ret;
}

static void __exit ps5_buzzer_exit(void)
{
	play_stop();
	if (snd_idev)
		input_unregister_device(snd_idev);
	cancel_work_sync(&bell_work);
	sysfs_remove_group(&buzz_misc.this_device->kobj, &buzz_attr_group);
	misc_deregister(&buzz_misc);
}

module_init(ps5_buzzer_init);
module_exit(ps5_buzzer_exit);

MODULE_AUTHOR("Armandas Kvietkus");
MODULE_DESCRIPTION("PlayStation 5 chassis piezo buzzer");
MODULE_LICENSE("GPL");
