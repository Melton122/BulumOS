/*
 * BulumOS hardware/driver framework - shared definitions.
 */
#ifndef _BULUM_H
#define _BULUM_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/device.h>

enum bulum_driver_type {
	BULUM_DRV_UNKNOWN = 0,
	BULUM_DRV_INPUT,
	BULUM_DRV_DISPLAY,
	BULUM_DRV_STORAGE,
	BULUM_DRV_BLOCK,
	BULUM_DRV_NETWORK,
	BULUM_DRV_USB,
	BULUM_DRV_AUDIO,
	BULUM_DRV_POWER,
};

struct bulum_driver {
	const char *name;
	enum bulum_driver_type type;
	int (*probe)(struct device *dev);
	void (*remove)(struct device *dev);
	struct list_head node;
	struct module *owner;
};

int bulum_driver_register(struct bulum_driver *bd);
void bulum_driver_unregister(struct bulum_driver *bd);

#endif
