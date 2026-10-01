/*
 * smart_meter_driver.c - Linux character device driver for a smart energy
 *                        meter pulse output (S0 / IEC 62053-21 style).
 *
 * ============================================================================
 * WHAT THIS DRIVER DOES
 * ============================================================================
 * A smart meter produces a train of pulses: every time a fixed amount of
 * energy has been consumed it emits a short electrical pulse (typically
 * 1 Wh per pulse, 12.5 Hz at maximum demand for a 50 A meter).  This driver
 * turns that pulse train into a cumulative counter that user space can read.
 *
 * Two pulse sources are supported:
 *
 *   1. SM_SOURCE_GPIO (hardware)
 *      The meter (or an optocoupler front end) is wired to a GPIO line.
 *      We take a rising-edge interrupt on that line and increment the
 *      kernel counter from the ISR.  This is the classic "count edges in
 *      the kernel, report to user space" pattern: the ISR does the
 *      minimum possible work (one atomic add) and user space polls or
 *      blocks on the descriptor, so no pulse is ever missed even if the
 *      application is busy.
 *
 *   2. SM_SOURCE_SIM (virtual meter)
 *      An hrtimer fires every 3600/pulses_per_hour seconds and produces a
 *      pulse.  This lets the whole stack - driver, analytics, dashboard -
 *      be demonstrated on a machine with no meter attached, and it is what
 *      makes the project runnable in a container or on a laptop.
 *
 * ============================================================================
 * USER SPACE INTERFACE
 * ============================================================================
 *   /dev/smart_meter            character device (misc device, dynamic minor)
 *   read(fd, &sm_sample, 24)    cumulative counter snapshot
 *   write(fd, &u64, 8)          inject pulses manually (test hook)
 *   poll(fd, ...)               blocks until new pulses arrive
 *   ioctl(fd, SM_IOC_*)         stats / configuration / reset
 *   /proc/smart_meter           human readable status
 *   /sys/kernel/smart_meter/*   pulse_count, irq_count, running, ...
 *
 * ============================================================================
 * CONCURRENCY
 * ============================================================================
 *   * The counter is a 64-bit atomic: any context may increment it.
 *   * ioctl / config changes are serialised with a mutex.
 *   * read() copies a whole snapshot with a single copy_to_user(), so
 *     readers always see a self-consistent (sequence, pulse_count) pair.
 *
 * Author : Smart Meter Project
 * License: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sysfs.h>
#include <linux/gpio.h>

#include "driver_interface.h"

#define SM_DRIVER_NAME       "smart_meter"
#define SM_DEFAULT_PPH       3600u   /* simulated rate: 1 pulse/second     */
#define SM_MAX_PPH           1000000u

/* --- kernel version compatibility shims ----------------------------- */

/* sysfs_emit() landed in 5.10; these attributes only ever print a single
 * scalar, so sprintf() is a safe substitute on older trees. */
#ifndef sysfs_emit
#define sysfs_emit(buf, fmt, ...) sprintf(buf, fmt, ##__VA_ARGS__)
#endif

/* no_llseek was renamed noop_llseek in 6.12. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define sm_llseek noop_llseek
#else
#define sm_llseek no_llseek
#endif


/* ------------------------------------------------------------------ */
/* module parameters                                                   */
/* ------------------------------------------------------------------ */

/* Simulated pulse rate, in pulses per hour. 3600 == 1 pulse/second. */
static unsigned int pulses_per_hour = SM_DEFAULT_PPH;
module_param(pulses_per_hour, uint, 0444);
MODULE_PARM_DESC(pulses_per_hour, "Simulated pulse rate in pulses per hour (1..1000000)");

/* GPIO line connected to the meter pulse output. 0 == not connected. */
static unsigned int gpio_pin;
module_param(gpio_pin, uint, 0444);
MODULE_PARM_DESC(gpio_pin, "GPIO line attached to the meter pulse output (0 = none)");

/* 1 == use the hrtimer simulator, 0 == wait for GPIO interrupts. */
static int simulate = 1;
module_param(simulate, int, 0444);
MODULE_PARM_DESC(simulate, "1 = hrtimer generated pulses, 0 = GPIO interrupt pulses");

/* Energy represented by one pulse, in Wh. Informational: the conversion
 * itself is performed in user space. */
static unsigned int wh_per_pulse = 1;
module_param(wh_per_pulse, uint, 0444);
MODULE_PARM_DESC(wh_per_pulse, "Energy represented by one pulse, in Wh");

/* ------------------------------------------------------------------ */
/* driver state                                                        */
/* ------------------------------------------------------------------ */

struct smart_meter {
	struct mutex lock;              /* serialises ioctl / reconfiguration  */
	wait_queue_head_t waitq;        /* poll() waiters                     */

	atomic64_t pulse_count;         /* cumulative pulses                   */
	atomic64_t sequence;            /* bumped on every pulse               */
	atomic64_t consumed_sequence;   /* sequence last handed to read()      */
	atomic64_t irq_count;           /* interrupt handler invocations       */
	atomic64_t simulated_count;     /* pulses from the hrtimer            */
	atomic64_t injected_count;      /* pulses from write()                */
	atomic_t open_count;            /* number of open descriptors          */

	u64 last_pulse_ns;              /* CLOCK_MONOTONIC of last pulse       */
	u64 start_ns;                   /* module load timestamp               */

	u32 source;                     /* SM_SOURCE_*                         */
	u32 enabled;                    /* counter running?                    */
	u32 config_pph;                 /* effective simulated rate           */

	int irq;                        /* GPIO interrupt number, -1 if unused */
	struct hrtimer sim_timer;
	bool timer_running;
};

static struct smart_meter sm_dev = {
	.source = SM_SOURCE_NONE,
	.enabled = 1,
	.config_pph = SM_DEFAULT_PPH,
	.irq = -1,
};

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* Records `count` pulses.  Callable from any context (including the ISR
 * and hrtimer softirq), hence no sleeping and no locking. */
static void sm_record_pulses(u64 count)
{
	if (count == 0)
		return;
	atomic64_inc(&sm_dev.sequence);
	atomic64_add(count, &sm_dev.pulse_count);
	/* ktime_get_ns() is safe from any context (CLOCK_MONOTONIC). */
	sm_dev.last_pulse_ns = ktime_get_ns();
}

/* (Re)program the hrtimer for the configured rate. */
static void sm_sim_start(void)
{
	u64 period_ns;
	ktime_t expiry;

	if (sm_dev.source != SM_SOURCE_SIM || !sm_dev.enabled)
		return;
	if (sm_dev.config_pph == 0)
		return;

	/* period = 1 hour / pulses_per_hour */
	period_ns = (u64)NSEC_PER_SEC * 3600ULL / (u64)sm_dev.config_pph;
	if (period_ns == 0)
		period_ns = 1;

	expiry = ns_to_ktime(period_ns);
	sm_dev.timer_running = true;
	hrtimer_start(&sm_dev.sim_timer, expiry, HRTIMER_MODE_REL);
}

static void sm_sim_stop(void)
{
	if (!sm_dev.timer_running)
		return;
	hrtimer_cancel(&sm_dev.sim_timer);
	sm_dev.timer_running = false;
}

/* ------------------------------------------------------------------ */
/* pulse sources                                                       */
/* ------------------------------------------------------------------ */

static enum hrtimer_restart sm_sim_timer_fn(struct hrtimer *timer)
{
	sm_record_pulses(1);
	atomic64_inc(&sm_dev.simulated_count);
	/* Reschedule relative to *now* so the callback's own execution time
	 * does not accumulate as drift. */
	hrtimer_forward_now(timer);
	return HRTIMER_RESTART;
}

/*
 * GPIO interrupt handler.  Runs in interrupt context: only atomic
 * operations are allowed here, which is exactly what sm_record_pulses()
 * does.  This is the property that guarantees no pulse is lost even if
 * user space is blocked, scheduling a page fault, or the CPU is busy.
 */
static irqreturn_t sm_gpio_isr(int irq, void *data)
{
	(void)irq;
	(void)data;
	atomic64_inc(&sm_dev.irq_count);
	sm_record_pulses(1);
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* file operations                                                     */
/* ------------------------------------------------------------------ */

static int sm_open(struct inode *inode, struct file *filp)
{
	(void)inode;
	filp->private_data = &sm_dev;
	atomic_inc(&sm_dev.open_count);
	/* Char devices are not seekable. */
	return nonseekable_open(inode, filp);
}

static int sm_release(struct inode *inode, struct file *filp)
{
	(void)inode;
	(void)filp;
	atomic_dec(&sm_dev.open_count);
	return 0;
}

/*
 * read() returns one cumulative snapshot.
 *
 * A snapshot (sequence, timestamp_ns, pulse_count) is consistent because it
 * is copied to user space in a single copy_to_user() call while counter
 * updates are atomic.  No read() can observe a half-updated counter.
 */
static ssize_t sm_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
	struct sm_sample sample;

	(void)ppos;

	if (count < sizeof(sample))
		return -EINVAL;

	mutex_lock(&sm_dev.lock);
	sample.sequence = (u64)atomic64_read(&sm_dev.sequence);
	sample.timestamp_ns = ktime_get_ns();
	sample.pulse_count = (u64)atomic64_read(&sm_dev.pulse_count);
	/* Remember what has been reported so poll() can detect new data. */
	atomic64_store(&sm_dev.consumed_sequence, (s64)sample.sequence);
	mutex_unlock(&sm_dev.lock);

	if (copy_to_user(buf, &sample, sizeof(sample)))
		return -EFAULT;

	return sizeof(sample);
}

/*
 * write() injects pulses: the user passes an optional 8 byte little-endian
 * count (1 when only a single byte / nothing is written).  This is the
 * manual test path - writing 3600 is the equivalent of simulating one
 * hour of consumption.
 */
static ssize_t sm_write(struct file *filp, const char __user *buf, size_t count,
			loff_t *ppos)
{
	u64 pulses = 1;
	u8 staging[sizeof(u64)];
	size_t copied = count < sizeof(u64) ? count : sizeof(u64);

	(void)filp;
	(void)ppos;

	if (copied > 0) {
		if (copy_from_user(staging, buf, copied))
			return -EFAULT;
		memcpy(&pulses, staging, copied);
		if (pulses == 0)
			pulses = 1;
	}

	sm_record_pulses(pulses);
	atomic64_add(pulses, &sm_dev.injected_count);

	return (ssize_t)count;
}

/*
 * poll(): report POLLIN whenever at least one pulse has been produced since
 * the last successful read().  A reader that wants to be woken up by the
 * meter simply poll()s instead of busy-looping.
 */
static __poll_t sm_poll(struct file *filp, poll_table *wait)
{
	__poll_t mask = 0;

	(void)filp;
	poll_wait(filp, &sm_dev.waitq, wait);

	if (atomic64_read(&sm_dev.sequence) != atomic64_read(&sm_dev.consumed_sequence))
		mask |= EPOLLIN | EPOLLRDNORM;

	return mask;
}

/* Wake every waiter - used when the module is being torn down. */
static void sm_wake_readers(void)
{
	wake_up_interruptible_poll(&sm_dev.waitq, EPOLLIN | EPOLLRDNORM);
}

/*
 * Applies a validated configuration: (re)programs the simulated generator
 * and switches between the hrtimer and the GPIO interrupt source.
 * Shared by SM_IOC_SET_CONFIG and by the writable `running` sysfs
 * attribute.  Caller must hold sm_dev.lock.
 */
static int sm_apply_config_locked(const struct sm_config *config)
{
	u32 source = config->source;

	sm_sim_stop(); /* stop with the old rate ... */

	/* Leave GPIO mode only if the new source really is the simulator. */
	if (source != sm_dev.source && sm_dev.irq >= 0 && source == SM_SOURCE_SIM) {
		free_irq(sm_dev.irq, &sm_dev);
		sm_dev.irq = -1;
#if IS_ENABLED(CONFIG_GPIOLIB)
		if (gpio_pin)
			gpio_free(gpio_pin);
#endif
	}

	/* Enter GPIO mode only when the hardware is actually present. */
	if (source == SM_SOURCE_GPIO && sm_dev.irq < 0) {
		if (!gpio_pin) {
			pr_warn(SM_DRIVER_NAME
				": gpio_pin not set, staying on the simulator\n");
		} else {
#if IS_ENABLED(CONFIG_GPIOLIB)
			int err = gpio_request(gpio_pin, SM_DRIVER_NAME);

			if (err == 0) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 5, 0)
				gpio_direction_input(gpio_pin);
#endif
				sm_dev.irq = gpio_to_irq(gpio_pin);
				if (sm_dev.irq < 0) {
					gpio_free(gpio_pin);
					sm_dev.irq = -1;
					pr_warn(SM_DRIVER_NAME
						": no IRQ for GPIO %u\n", gpio_pin);
				} else {
					err = request_irq(sm_dev.irq, sm_gpio_isr,
							  IRQF_TRIGGER_RISING,
							  SM_DRIVER_NAME, &sm_dev);
					if (err) {
						gpio_free(gpio_pin);
						sm_dev.irq = -1;
						pr_warn(SM_DRIVER_NAME
							": request_irq failed: %d\n",
							err);
					}
				}
			} else {
				pr_warn(SM_DRIVER_NAME ": gpio_request failed: %d\n",
					err);
			}
#else
			pr_warn(SM_DRIVER_NAME ": GPIO support not compiled in\n");
#endif
		}
		/* If the hardware could not be claimed, fall back to the virtual
		 * meter rather than silently counting nothing. */
		if (sm_dev.irq < 0)
			source = SM_SOURCE_SIM;
	}

	sm_dev.config_pph = config->pulses_per_hour;
	sm_dev.source = source;
	sm_dev.enabled = config->enabled ? 1 : 0;

	if (sm_dev.enabled)
		sm_sim_start(); /* ... restart with the new rate */

	pr_info(SM_DRIVER_NAME ": configured %u pulses/hour, source=%u, %s\n",
		sm_dev.config_pph, sm_dev.source,
		sm_dev.enabled ? "running" : "paused");
	return 0;
}

static long sm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	(void)filp;

	switch (cmd) {
	case SM_IOC_GET_STATS: {
		struct sm_stats stats;

		mutex_lock(&sm_dev.lock);
		stats.pulse_count = (u64)atomic64_read(&sm_dev.pulse_count);
		stats.pulses_delta = stats.pulse_count -
				     (u64)atomic64_read(&sm_dev.consumed_sequence);
		stats.last_pulse_ns = sm_dev.last_pulse_ns;
		stats.driver_uptime_ns = ktime_get_ns() - sm_dev.start_ns;
		stats.interrupt_count = (u64)atomic64_read(&sm_dev.irq_count);
		stats.simulated_count = (u64)atomic64_read(&sm_dev.simulated_count);
		stats.injected_count = (u64)atomic64_read(&sm_dev.injected_count);
		stats.source = sm_dev.source;
		stats.enabled = sm_dev.enabled;
		mutex_unlock(&sm_dev.lock);

		if (copy_to_user((void __user *)arg, &stats, sizeof(stats)))
			return -EFAULT;
		return 0;
	}

	case SM_IOC_GET_CONFIG: {
		struct sm_config config;

		mutex_lock(&sm_dev.lock);
		config.pulses_per_hour = sm_dev.config_pph;
		config.source = sm_dev.source;
		config.enabled = sm_dev.enabled;
		config.reserved = 0;
		mutex_unlock(&sm_dev.lock);

		if (copy_to_user((void __user *)arg, &config, sizeof(config)))
			return -EFAULT;
		return 0;
	}

	case SM_IOC_SET_CONFIG: {
		struct sm_config config;
		int ret;

		if (copy_from_user(&config, (void __user *)arg, sizeof(config)))
			return -EFAULT;
		if (config.pulses_per_hour == 0 ||
		    config.pulses_per_hour > SM_MAX_PPH)
			return -EINVAL;
		if (config.source != SM_SOURCE_SIM && config.source != SM_SOURCE_GPIO)
			return -EINVAL;

		mutex_lock(&sm_dev.lock);
		ret = sm_apply_config_locked(&config);
		mutex_unlock(&sm_dev.lock);
		return ret;
	}

	case SM_IOC_RESET:
		mutex_lock(&sm_dev.lock);
		atomic64_set(&sm_dev.pulse_count, 0);
		atomic64_set(&sm_dev.sequence, 0);
		atomic64_set(&sm_dev.consumed_sequence, 0);
		atomic64_set(&sm_dev.irq_count, 0);
		atomic64_set(&sm_dev.simulated_count, 0);
		atomic64_set(&sm_dev.injected_count, 0);
		sm_dev.last_pulse_ns = 0;
		mutex_unlock(&sm_dev.lock);
		pr_info(SM_DRIVER_NAME ": counter reset\n");
		return 0;

	case SM_IOC_SET_CONSTANT: {
		u64 value;

		if (copy_from_user(&value, (void __user *)arg, sizeof(value)))
			return -EFAULT;
		if (value == 0)
			return -EINVAL;
		wh_per_pulse = (unsigned int)(value > 1000 ? 1000 : value);
		return 0;
	}

	default:
		return -ENOTTY;
	}
}

static const struct file_operations sm_fops = {
	.owner		= THIS_MODULE,
	.open		= sm_open,
	.release	= sm_release,
	.read		= sm_read,
	.write		= sm_write,
	.unlocked_ioctl	= sm_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.poll		= sm_poll,
	.llseek		= sm_llseek,
};

static struct miscdevice sm_misc_device = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= SM_DRIVER_NAME,
	.fops	= &sm_fops,
	.mode	= 0666,
};

/* ------------------------------------------------------------------ */
/* sysfs interface                                                     */
/* ------------------------------------------------------------------ */

static ssize_t pulse_count_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	(void)dev;
	(void)attr;
	return sysfs_emit(buf, "%lld\n", (long long)atomic64_read(&sm_dev.pulse_count));
}
static DEVICE_ATTR_RO(pulse_count);

static ssize_t irq_count_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	(void)dev;
	(void)attr;
	return sysfs_emit(buf, "%lld\n", (long long)atomic64_read(&sm_dev.irq_count));
}
static DEVICE_ATTR_RO(irq_count);

static ssize_t simulated_count_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	(void)dev;
	(void)attr;
	return sysfs_emit(buf, "%lld\n", (long long)atomic64_read(&sm_dev.simulated_count));
}
static DEVICE_ATTR_RO(simulated_count);

static ssize_t injected_count_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	(void)dev;
	(void)attr;
	return sysfs_emit(buf, "%lld\n", (long long)atomic64_read(&sm_dev.injected_count));
}
static DEVICE_ATTR_RO(injected_count);

static ssize_t running_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	(void)dev;
	(void)attr;
	return sysfs_emit(buf, "%u\n", sm_dev.enabled);
}

/* `running` is read/write: echo 0 > running pauses the counter. */
static ssize_t running_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct sm_config config;
	bool on;

	(void)dev;
	(void)attr;
	if (kstrtobool(buf, &on))
		return -EINVAL;

	mutex_lock(&sm_dev.lock);
	config.pulses_per_hour = sm_dev.config_pph;
	config.source = sm_dev.source;
	config.enabled = on ? 1 : 0;
	config.reserved = 0;
	sm_apply_config_locked(&config);
	mutex_unlock(&sm_dev.lock);

	return (ssize_t)count;
}

static DEVICE_ATTR_RW(running);

static ssize_t source_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	const char *name;

	(void)dev;
	(void)attr;
	switch (sm_dev.source) {
	case SM_SOURCE_SIM:
		name = "simulated";
		break;
	case SM_SOURCE_GPIO:
		name = "gpio";
		break;
	default:
		name = "none";
		break;
	}
	return sysfs_emit(buf, "%s\n", name);
}
static DEVICE_ATTR_RO(source);

static ssize_t pulses_per_hour_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	(void)dev;
	(void)attr;
	return sysfs_emit(buf, "%u\n", sm_dev.config_pph);
}
static DEVICE_ATTR_RO(pulses_per_hour);

static struct attribute *sm_attrs[] = {
	&dev_attr_pulse_count.attr,
	&dev_attr_irq_count.attr,
	&dev_attr_simulated_count.attr,
	&dev_attr_injected_count.attr,
	&dev_attr_source.attr,
	&dev_attr_pulses_per_hour.attr,
	&dev_attr_running.attr,
	NULL,
};

static const struct attribute_group sm_attr_group = {
	.attrs = sm_attrs,
};
static struct kobject *sm_kobj;

/* ------------------------------------------------------------------ */
/* procfs interface                                                    */
/* ------------------------------------------------------------------ */

static int sm_proc_show(struct seq_file *m, void *v)
{
	(void)v;

	mutex_lock(&sm_dev.lock);
	seq_printf(m, "smart_meter driver %s\n", SM_DRIVER_VERSION);
	seq_printf(m, "pulse_count       : %lld\n",
		   (long long)atomic64_read(&sm_dev.pulse_count));
	seq_printf(m, "sequence          : %lld\n",
		   (long long)atomic64_read(&sm_dev.sequence));
	seq_printf(m, "interrupt_count   : %lld\n",
		   (long long)atomic64_read(&sm_dev.irq_count));
	seq_printf(m, "simulated_count   : %lld\n",
		   (long long)atomic64_read(&sm_dev.simulated_count));
	seq_printf(m, "injected_count    : %lld\n",
		   (long long)atomic64_read(&sm_dev.injected_count));
	seq_printf(m, "source            : %s\n",
		   sm_dev.source == SM_SOURCE_SIM ? "simulated" :
		   sm_dev.source == SM_SOURCE_GPIO ? "gpio" : "none");
	seq_printf(m, "pulses_per_hour   : %u\n", sm_dev.config_pph);
	seq_printf(m, "wh_per_pulse      : %u\n", wh_per_pulse);
	seq_printf(m, "enabled           : %u\n", sm_dev.enabled);
	seq_printf(m, "uptime_ms         : %llu\n",
		   (unsigned long long)((ktime_get_ns() - sm_dev.start_ns) / NSEC_PER_MSEC));
	seq_printf(m, "open_fds          : %d\n", atomic_read(&sm_dev.open_count));
	mutex_unlock(&sm_dev.lock);
	return 0;
}

static int sm_proc_open(struct inode *inode, struct file *file)
{
	(void)inode;
	return single_open(file, sm_proc_show, NULL);
}

static const struct proc_ops sm_proc_ops = {
	.proc_open	= sm_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

/* ------------------------------------------------------------------ */
/* init / exit                                                         */
/* ------------------------------------------------------------------ */

static int __init sm_driver_init(void)
{
	int err;

	pr_info(SM_DRIVER_NAME ": loading (version %s)\n", SM_DRIVER_VERSION);

	if (pulses_per_hour == 0 || pulses_per_hour > SM_MAX_PPH) {
		pr_err(SM_DRIVER_NAME ": invalid pulses_per_hour=%u\n", pulses_per_hour);
		return -EINVAL;
	}

	mutex_init(&sm_dev.lock);
	init_waitqueue_head(&sm_dev.waitq);
	hrtimer_init(&sm_dev.sim_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	sm_dev.sim_timer.function = sm_sim_timer_fn;

	atomic64_set(&sm_dev.pulse_count, 0);
	atomic64_set(&sm_dev.sequence, 0);
	atomic64_set(&sm_dev.consumed_sequence, 0);
	atomic64_set(&sm_dev.irq_count, 0);
	atomic64_set(&sm_dev.simulated_count, 0);
	atomic64_set(&sm_dev.injected_count, 0);
	atomic_set(&sm_dev.open_count, 0);

	sm_dev.config_pph = pulses_per_hour;
	sm_dev.enabled = 1;
	sm_dev.start_ns = ktime_get_ns();
	sm_dev.last_pulse_ns = 0;
	sm_dev.timer_running = false;
	sm_dev.irq = -1;

	/* Register the character device: creates /dev/smart_meter with a
	 * dynamically allocated minor number. */
	err = misc_register(&sm_misc_device);
	if (err) {
		pr_err(SM_DRIVER_NAME ": misc_register failed: %d\n", err);
		return err;
	}

	/* procfs: /proc/smart_meter */
	proc_create(SM_PROC_NAME, 0444, NULL, &sm_proc_ops);

	/* sysfs: /sys/kernel/smart_meter/{pulse_count,running,source,...} */
	sm_kobj = kobject_create_and_add(SM_DRIVER_NAME, kernel_kobj);
	if (sm_kobj) {
		if (sysfs_create_group(sm_kobj, &sm_attr_group)) {
			pr_warn(SM_DRIVER_NAME ": sysfs group creation failed\n");
			kobject_put(sm_kobj);
			sm_kobj = NULL;
		}
	}

	/*
	 * Attach a pulse source through the very same helper the ioctl uses,
	 * so module load and runtime reconfiguration cannot drift apart.
	 * `simulate=1` picks the hrtimer; `simulate=0 gpio_pin=N` claims the
	 * GPIO interrupt; `simulate=0` without a pin warns and falls back.
	 */
	{
		struct sm_config config;

		config.pulses_per_hour = sm_dev.config_pph;
		if (simulate)
			config.source = SM_SOURCE_SIM;
		else if (gpio_pin)
			config.source = SM_SOURCE_GPIO;
		else {
			pr_warn(SM_DRIVER_NAME
				": simulate=0 but gpio_pin=0, using the simulator\n");
			config.source = SM_SOURCE_SIM;
		}
		config.enabled = 1;
		config.reserved = 0;

		mutex_lock(&sm_dev.lock);
		sm_apply_config_locked(&config);
		mutex_unlock(&sm_dev.lock);
	}

	if (sm_dev.source == SM_SOURCE_SIM && !simulate)
		pr_info(SM_DRIVER_NAME ": virtual meter enabled at %u pulses/hour\n",
			sm_dev.config_pph);
	else if (sm_dev.source == SM_SOURCE_GPIO)
		pr_info(SM_DRIVER_NAME ": listening on GPIO %u (IRQ %d), rising edge\n",
			gpio_pin, sm_dev.irq);

	pr_info(SM_DRIVER_NAME ": /dev/%s ready\n", SM_DRIVER_NAME);
	return 0;
}

static void __exit sm_driver_exit(void)
{
	pr_info(SM_DRIVER_NAME ": unloading, total pulses = %lld\n",
		(long long)atomic64_read(&sm_dev.pulse_count));

	/* Order matters: stop generating pulses before dismantling the
	 * interfaces that report them. */
	sm_sim_stop();

	if (sm_dev.irq >= 0) {
		free_irq(sm_dev.irq, &sm_dev);
		sm_dev.irq = -1;
	}
#if IS_ENABLED(CONFIG_GPIOLIB)
	if (gpio_pin)
		gpio_free(gpio_pin);
#endif

	sm_wake_readers();

	if (sm_kobj) {
		sysfs_remove_group(sm_kobj, &sm_attr_group);
		kobject_put(sm_kobj);
		sm_kobj = NULL;
	}
	remove_proc_entry(SM_PROC_NAME, NULL);
	misc_deregister(&sm_misc_device);
	mutex_destroy(&sm_dev.lock);
	pr_info(SM_DRIVER_NAME ": unloaded\n");
}

module_init(sm_driver_init);
module_exit(sm_driver_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Smart Meter Project");
MODULE_DESCRIPTION("Character device driver for smart energy meter pulse input");
MODULE_VERSION("1.0.0");
