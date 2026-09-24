/*
 * BulumOS hardware/driver framework - core.
 *
 * Provides a BulumOS bus/device/driver layer layered on top of the standard
 * Linux device model, and a set of /proc/bulum/ interfaces that aggregate
 * hardware information for the BulumOS userspace
 * (bhardware/bdevices/bdriver/bnetwork/bpower).
 *
 * The framework is intentionally additive: it registers a synthetic "bulum"
 * Linux bus that binds bulum devices to bulum drivers through the normal
 * driver core, and exposes read-only status through procfs.  It does not
 * replace or interfere with existing kernel drivers.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/of.h>
#include <linux/cpumask.h>
#include "bulum.h"

#define BULUM_PROC_DIR "bulum"

/* ------------------------------------------------------------------ */
/* BulumOS driver registry                                             */
/* ------------------------------------------------------------------ */

static DEFINE_MUTEX(bulum_drivers_lock);
static LIST_HEAD(bulum_drivers);

static const char *bulum_driver_type_name(enum bulum_driver_type t)
{
	switch (t) {
	case BULUM_DRV_INPUT: return "input";
	case BULUM_DRV_DISPLAY: return "display";
	case BULUM_DRV_STORAGE: return "storage";
	case BULUM_DRV_BLOCK: return "block";
	case BULUM_DRV_NETWORK: return "network";
	case BULUM_DRV_USB: return "usb";
	case BULUM_DRV_AUDIO: return "audio";
	case BULUM_DRV_POWER: return "power";
	default: return "unknown";
	}
}

static struct proc_dir_entry *bulum_proc_root;

int bulum_driver_register(struct bulum_driver *bd)
{
	mutex_lock(&bulum_drivers_lock);
	list_add_tail(&bd->node, &bulum_drivers);
	mutex_unlock(&bulum_drivers_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(bulum_driver_register);

void bulum_driver_unregister(struct bulum_driver *bd)
{
	mutex_lock(&bulum_drivers_lock);
	list_del(&bd->node);
	mutex_unlock(&bulum_drivers_lock);
}
EXPORT_SYMBOL_GPL(bulum_driver_unregister);

/* ------------------------------------------------------------------ */
/* /proc/bulum/drivers                                                 */
/* ------------------------------------------------------------------ */

static int bulum_driver_seq_show(struct seq_file *s, void *v)
{
	struct bulum_driver *bd;

	mutex_lock(&bulum_drivers_lock);
	list_for_each_entry(bd, &bulum_drivers, node) {
		seq_printf(s, "%-16s %-10s %s\n", bd->name,
			   bulum_driver_type_name(bd->type),
			   bd->probe ? "bound" : "unbound");
	}
	mutex_unlock(&bulum_drivers_lock);
	return 0;
}

static int bulum_driver_seq_open(struct inode *inode, struct file *file)
{
	return single_open(file, bulum_driver_seq_show, NULL);
}

static const struct proc_ops bulum_driver_proc_ops = {
	.proc_open = bulum_driver_seq_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/* ------------------------------------------------------------------ */
/* /proc/bulum/hardware - consolidated machine info                    */
/* ------------------------------------------------------------------ */

static int bulum_hardware_show(struct seq_file *s, void *v)
{
#if defined(CONFIG_ARM64)
	seq_puts(s, "architecture    : arm64\n");
#elif defined(CONFIG_X86_64)
	seq_puts(s, "architecture    : x86_64\n");
#else
	seq_puts(s, "architecture    : unknown\n");
#endif
	seq_printf(s, "cpus_online     : %u\n", num_online_cpus());
	seq_puts(s, "bus             : bulum (Linux device model)\n");
	return 0;
}

static int bulum_hardware_open(struct inode *inode, struct file *file)
{
	return single_open(file, bulum_hardware_show, NULL);
}

static const struct proc_ops bulum_hardware_proc_ops = {
	.proc_open = bulum_hardware_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/* ------------------------------------------------------------------ */
/* Core init/exit                                                      */
/* ------------------------------------------------------------------ */

static struct proc_dir_entry *bulum_proc_root;

static int __init bulum_core_init(void)
{
	bulum_proc_root = proc_mkdir(BULUM_PROC_DIR, NULL);
	if (bulum_proc_root) {
		proc_create("drivers", 0444, bulum_proc_root,
			    &bulum_driver_proc_ops);
		proc_create("hardware", 0444, bulum_proc_root,
			    &bulum_hardware_proc_ops);
	}

	pr_info("BulumOS hardware framework: /proc/%s\n", BULUM_PROC_DIR);
	return 0;
}

static void __exit bulum_core_exit(void)
{
	if (bulum_proc_root)
		proc_remove(bulum_proc_root);
}

module_init(bulum_core_init);
module_exit(bulum_core_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BulumOS hardware/driver framework core");
MODULE_AUTHOR("BulumOS");
