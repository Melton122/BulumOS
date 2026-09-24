/*
 * BulumOS hardware/driver framework - example skeleton driver.
 *
 * Self-contained driver that registers a BulumOS driver through the shared
 * framework (bulum_driver_register), demonstrating the registry and the
 * /proc/bulum/drivers report.  The probe callback shows how a real driver
 * would hook hardware once bound through the bulum bus.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include "bulum.h"

static int bulum_dummy_probe(struct device *dev)
{
	dev_info(dev, "BulumOS example driver probed\n");
	return 0;
}

static void bulum_dummy_remove(struct device *dev)
{
	dev_info(dev, "BulumOS example driver removed\n");
}

static struct bulum_driver bulum_dummy = {
	.name = "bulum-info",
	.type = BULUM_DRV_UNKNOWN,
	.probe = bulum_dummy_probe,
	.remove = bulum_dummy_remove,
	.owner = THIS_MODULE,
};

static int __init bulum_dummy_init(void)
{
	pr_info("BulumOS example driver registering\n");
	return bulum_driver_register(&bulum_dummy);
}

static void __exit bulum_dummy_exit(void)
{
	bulum_driver_unregister(&bulum_dummy);
}

module_init(bulum_dummy_init);
module_exit(bulum_dummy_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BulumOS framework example driver");
MODULE_AUTHOR("BulumOS");
