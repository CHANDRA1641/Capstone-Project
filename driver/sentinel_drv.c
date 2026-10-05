// SPDX-License-Identifier: GPL-2.0-only
/*
 * sentinel_drv - simulated environmental-sensor character device.
 *
 * Exposes /dev/sentinel0. While the device is open an hrtimer fires every
 * period_ms and schedules a work item that appends a struct sentinel_sample
 * (random-walk temperature/humidity) to a FIFO. read(2) returns whole
 * samples, blocking until data exists. If user space is too slow the oldest
 * samples are overwritten and the next sample is flagged OVERRUN.
 *
 * Kernel concepts exercised: char device (cdev), class/device creation, sysfs
 * attributes, hrtimer, workqueue, spinlock, wait queue, poll, ioctl,
 * copy_to_user, module parameters.
 *
 * Requires Linux >= 5.10 (sysfs_emit). Tested-by: nobody yet, see README.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/random.h>
#include <linux/log2.h>

#include <sentinel/uapi.h>

#define DRV_NAME	"sentinel"
#define READ_BATCH	16u

static unsigned int period_ms = 100;
module_param_named(period_ms, period_ms, uint, 0644);
MODULE_PARM_DESC(period_ms, "Initial sampling period in ms (10..10000)");

static unsigned int fifo_depth = 256;
module_param(fifo_depth, uint, 0444);
MODULE_PARM_DESC(fifo_depth, "FIFO capacity in samples, power of two (default 256)");

struct sentinel_dev {
	dev_t			devt;
	struct cdev		cdev;
	struct class		*cls;
	struct device		*dev;

	atomic_t		busy;		/* exclusive open */
	struct mutex		cfg_lock;	/* serialises ioctl/sysfs config changes */

	struct hrtimer		timer;
	struct work_struct	work;
	atomic_t		running;	/* timer armed */
	atomic_t		period;		/* ms, read from timer context */

	spinlock_t		lock;		/* protects everything below */
	struct sentinel_sample	*fifo;
	u32			mask;		/* depth - 1 */
	u32			head;		/* next write index (free running) */
	u32			tail;		/* next read index (free running) */
	u32			seq;
	bool			overrun_pending;
	u64			produced;
	u64			dropped;

	/* random-walk state, only touched from the work item */
	s32			temp_mc;
	s32			hum_mpct;

	wait_queue_head_t	wq;
};

static struct sentinel_dev *sdev;

/* ------------------------------------------------------------------ */
/* Sample generation                                                   */
/* ------------------------------------------------------------------ */

static s32 walk(s32 v, s32 step, s32 lo, s32 hi)
{
	s32 r = (s32)(get_random_u32() % (u32)(2 * step + 1)) - step;

	v += r;
	return clamp(v, lo, hi);
}

/* Runs in process context (workqueue): may take its time, never in IRQ. */
static void sentinel_work_fn(struct work_struct *w)
{
	struct sentinel_dev *d = container_of(w, struct sentinel_dev, work);
	struct sentinel_sample s;
	unsigned long flags;

	d->temp_mc = walk(d->temp_mc, 150, -10000, 60000);
	d->hum_mpct = walk(d->hum_mpct, 400, 5000, 95000);

	s.ts_ns = ktime_get_ns();
	s.temp_mc = d->temp_mc;
	s.hum_mpct = d->hum_mpct;
	s.flags = 0;

	spin_lock_irqsave(&d->lock, flags);
	s.seq = d->seq++;
	if (d->head - d->tail > d->mask) {	/* full: drop the oldest */
		d->tail++;
		d->dropped++;
		d->overrun_pending = true;
	}
	if (d->overrun_pending) {
		s.flags |= SENTINEL_FLAG_OVERRUN;
		d->overrun_pending = false;
	}
	d->fifo[d->head & d->mask] = s;
	d->head++;
	d->produced++;
	spin_unlock_irqrestore(&d->lock, flags);

	wake_up_interruptible(&d->wq);
}

static enum hrtimer_restart sentinel_timer_fn(struct hrtimer *t)
{
	struct sentinel_dev *d = container_of(t, struct sentinel_dev, timer);

	if (!atomic_read(&d->running))
		return HRTIMER_NORESTART;

	schedule_work(&d->work);
	hrtimer_forward_now(t, ms_to_ktime(atomic_read(&d->period)));
	return HRTIMER_RESTART;
}

static void sentinel_timer_init(struct sentinel_dev *d)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	hrtimer_setup(&d->timer, sentinel_timer_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#else
	hrtimer_init(&d->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	d->timer.function = sentinel_timer_fn;
#endif
}

static void sentinel_start(struct sentinel_dev *d)
{
	atomic_set(&d->running, 1);
	hrtimer_start(&d->timer, ms_to_ktime(atomic_read(&d->period)), HRTIMER_MODE_REL);
}

static void sentinel_stop(struct sentinel_dev *d)
{
	atomic_set(&d->running, 0);
	hrtimer_cancel(&d->timer);
	cancel_work_sync(&d->work);
}

static void sentinel_reset_fifo(struct sentinel_dev *d)
{
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	d->head = 0;
	d->tail = 0;
	d->seq = 0;
	d->produced = 0;
	d->dropped = 0;
	d->overrun_pending = false;
	spin_unlock_irqrestore(&d->lock, flags);
}

static u32 sentinel_used(struct sentinel_dev *d)
{
	unsigned long flags;
	u32 used;

	spin_lock_irqsave(&d->lock, flags);
	used = d->head - d->tail;
	spin_unlock_irqrestore(&d->lock, flags);
	return used;
}

static int sentinel_set_period(struct sentinel_dev *d, u32 ms)
{
	if (ms < SENTINEL_PERIOD_MIN_MS || ms > SENTINEL_PERIOD_MAX_MS)
		return -EINVAL;
	atomic_set(&d->period, (int)ms);
	return 0;
}

static void sentinel_get_stats(struct sentinel_dev *d, struct sentinel_dev_stats *st)
{
	unsigned long flags;

	memset(st, 0, sizeof(*st));
	spin_lock_irqsave(&d->lock, flags);
	st->produced = d->produced;
	st->dropped = d->dropped;
	st->fifo_used = d->head - d->tail;
	spin_unlock_irqrestore(&d->lock, flags);
	st->period_ms = (u32)atomic_read(&d->period);
	st->fifo_depth = d->mask + 1;
}

/* ------------------------------------------------------------------ */
/* File operations                                                     */
/* ------------------------------------------------------------------ */

static int sentinel_open(struct inode *inode, struct file *filp)
{
	struct sentinel_dev *d = container_of(inode->i_cdev, struct sentinel_dev, cdev);

	if (atomic_cmpxchg(&d->busy, 0, 1) != 0)
		return -EBUSY;

	sentinel_reset_fifo(d);
	filp->private_data = d;
	sentinel_start(d);
	return stream_open(inode, filp);
}

static int sentinel_release(struct inode *inode, struct file *filp)
{
	struct sentinel_dev *d = filp->private_data;

	sentinel_stop(d);
	atomic_set(&d->busy, 0);
	return 0;
}

static ssize_t sentinel_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
	struct sentinel_dev *d = filp->private_data;
	struct sentinel_sample batch[READ_BATCH];
	unsigned long flags;
	size_t want, n, i;
	int ret;

	if (count < sizeof(struct sentinel_sample))
		return -EINVAL;
	want = min_t(size_t, count / sizeof(struct sentinel_sample), READ_BATCH);

	if (filp->f_flags & O_NONBLOCK) {
		if (!sentinel_used(d))
			return -EAGAIN;
	} else {
		ret = wait_event_interruptible(d->wq, sentinel_used(d) > 0);
		if (ret)
			return ret;	/* -ERESTARTSYS */
	}

	spin_lock_irqsave(&d->lock, flags);
	n = min_t(size_t, want, d->head - d->tail);
	for (i = 0; i < n; i++)
		batch[i] = d->fifo[(d->tail + i) & d->mask];
	d->tail += (u32)n;
	spin_unlock_irqrestore(&d->lock, flags);

	if (!n)
		return -EAGAIN;	/* lost a race with RESET; caller retries */

	/* copy_to_user may sleep, so it must stay outside the spinlock */
	if (copy_to_user(buf, batch, n * sizeof(batch[0])))
		return -EFAULT;
	return (ssize_t)(n * sizeof(batch[0]));
}

static __poll_t sentinel_poll(struct file *filp, poll_table *wait)
{
	struct sentinel_dev *d = filp->private_data;

	poll_wait(filp, &d->wq, wait);
	return sentinel_used(d) ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static long sentinel_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct sentinel_dev *d = filp->private_data;
	void __user *uarg = (void __user *)arg;
	long ret = 0;

	if (_IOC_TYPE(cmd) != SENTINEL_IOC_MAGIC)
		return -ENOTTY;

	mutex_lock(&d->cfg_lock);
	switch (cmd) {
	case SENTINEL_IOC_SET_PERIOD: {
		u32 ms;

		if (get_user(ms, (u32 __user *)uarg))
			ret = -EFAULT;
		else
			ret = sentinel_set_period(d, ms);
		break;
	}
	case SENTINEL_IOC_GET_STATS: {
		struct sentinel_dev_stats st;

		sentinel_get_stats(d, &st);
		if (copy_to_user(uarg, &st, sizeof(st)))
			ret = -EFAULT;
		break;
	}
	case SENTINEL_IOC_RESET:
		sentinel_reset_fifo(d);
		break;
	default:
		ret = -ENOTTY;
	}
	mutex_unlock(&d->cfg_lock);
	return ret;
}

static const struct file_operations sentinel_fops = {
	.owner		= THIS_MODULE,
	.open		= sentinel_open,
	.release	= sentinel_release,
	.read		= sentinel_read,
	.poll		= sentinel_poll,
	.unlocked_ioctl	= sentinel_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	.llseek		= no_llseek,
#endif
};

/* ------------------------------------------------------------------ */
/* sysfs: /sys/class/sentinel/sentinel0/{period_ms,stats}              */
/* ------------------------------------------------------------------ */

static ssize_t period_ms_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&sdev->period));
}

static ssize_t period_ms_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t len)
{
	u32 ms;
	int ret;

	ret = kstrtou32(buf, 10, &ms);
	if (ret)
		return ret;

	mutex_lock(&sdev->cfg_lock);
	ret = sentinel_set_period(sdev, ms);
	mutex_unlock(&sdev->cfg_lock);
	return ret ? ret : (ssize_t)len;
}
static DEVICE_ATTR_RW(period_ms);

static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sentinel_dev_stats st;

	sentinel_get_stats(sdev, &st);
	return sysfs_emit(buf, "produced=%llu dropped=%llu fifo_used=%u fifo_depth=%u period_ms=%u\n",
			  st.produced, st.dropped, st.fifo_used, st.fifo_depth, st.period_ms);
}
static DEVICE_ATTR_RO(stats);

static struct attribute *sentinel_attrs[] = {
	&dev_attr_period_ms.attr,
	&dev_attr_stats.attr,
	NULL,
};
ATTRIBUTE_GROUPS(sentinel);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
static char *sentinel_devnode(const struct device *dev, umode_t *mode)
#else
static char *sentinel_devnode(struct device *dev, umode_t *mode)
#endif
{
	if (mode)
		*mode = 0660;
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Module init / exit                                                  */
/* ------------------------------------------------------------------ */

static int __init sentinel_init(void)
{
	struct sentinel_dev *d;
	int ret;

	if (!is_power_of_2(fifo_depth) || fifo_depth < 2 || fifo_depth > 65536) {
		pr_err("fifo_depth must be a power of two in 2..65536\n");
		return -EINVAL;
	}
	if (period_ms < SENTINEL_PERIOD_MIN_MS || period_ms > SENTINEL_PERIOD_MAX_MS) {
		pr_err("period_ms must be in %u..%u\n", SENTINEL_PERIOD_MIN_MS,
		       SENTINEL_PERIOD_MAX_MS);
		return -EINVAL;
	}

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->fifo = kvcalloc(fifo_depth, sizeof(*d->fifo), GFP_KERNEL);
	if (!d->fifo) {
		ret = -ENOMEM;
		goto err_free;
	}
	d->mask = fifo_depth - 1;
	d->temp_mc = 22000;
	d->hum_mpct = 45000;

	spin_lock_init(&d->lock);
	mutex_init(&d->cfg_lock);
	init_waitqueue_head(&d->wq);
	INIT_WORK(&d->work, sentinel_work_fn);
	sentinel_timer_init(d);
	atomic_set(&d->busy, 0);
	atomic_set(&d->running, 0);
	atomic_set(&d->period, (int)period_ms);
	sdev = d;

	ret = alloc_chrdev_region(&d->devt, 0, 1, DRV_NAME);
	if (ret) {
		pr_err("alloc_chrdev_region failed: %d\n", ret);
		goto err_fifo;
	}

	cdev_init(&d->cdev, &sentinel_fops);
	d->cdev.owner = THIS_MODULE;
	ret = cdev_add(&d->cdev, d->devt, 1);
	if (ret) {
		pr_err("cdev_add failed: %d\n", ret);
		goto err_region;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	d->cls = class_create(DRV_NAME);
#else
	d->cls = class_create(THIS_MODULE, DRV_NAME);
#endif
	if (IS_ERR(d->cls)) {
		ret = PTR_ERR(d->cls);
		pr_err("class_create failed: %d\n", ret);
		goto err_cdev;
	}
	d->cls->devnode = sentinel_devnode;

	d->dev = device_create_with_groups(d->cls, NULL, d->devt, d, sentinel_groups,
					   DRV_NAME "%d", 0);
	if (IS_ERR(d->dev)) {
		ret = PTR_ERR(d->dev);
		pr_err("device_create failed: %d\n", ret);
		goto err_class;
	}

	pr_info("loaded: /dev/%s0 (major %d, period %u ms, fifo %u samples)\n", DRV_NAME,
		MAJOR(d->devt), period_ms, fifo_depth);
	return 0;

err_class:
	class_destroy(d->cls);
err_cdev:
	cdev_del(&d->cdev);
err_region:
	unregister_chrdev_region(d->devt, 1);
err_fifo:
	kvfree(d->fifo);
err_free:
	sdev = NULL;
	kfree(d);
	return ret;
}

static void __exit sentinel_exit(void)
{
	struct sentinel_dev *d = sdev;

	device_destroy(d->cls, d->devt);
	class_destroy(d->cls);
	cdev_del(&d->cdev);
	unregister_chrdev_region(d->devt, 1);
	/* the timer is stopped in release(); open files pin the module so it is idle here */
	kvfree(d->fifo);
	sdev = NULL;
	kfree(d);
	pr_info("unloaded\n");
}

module_init(sentinel_init);
module_exit(sentinel_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sentinel capstone");
MODULE_DESCRIPTION("Simulated environmental sensor character device");
MODULE_VERSION("1.0.0");
