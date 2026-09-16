// SPDX-License-Identifier: GPL-2.0
/*
 * i2c-wake: enable the Type-C path's I2C controllers at runtime.
 *
 * For a DTB that leaves i2c@a84000, i2c@a9c000 and i2c@890000 disabled (the
 * community DTBs before this port's DTS). Applies overlay.dts at load and
 * removes it at unload; talks to no device. Not needed with dts/ from this
 * repo, which enables the buses itself.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>

#include "overlay_blob.h"

static int ovcs_id;

static int __init i2c_wake_init(void)
{
	int ret = of_overlay_fdt_apply(overlay_dtbo, overlay_dtbo_len, &ovcs_id, NULL);

	if (ret)
		pr_err("i2c-wake: overlay apply failed: %d\n", ret);
	else
		pr_info("i2c-wake: overlay applied (id %d)\n", ovcs_id);
	return ret;
}

static void __exit i2c_wake_exit(void)
{
	of_overlay_remove(&ovcs_id);
	pr_info("i2c-wake: overlay removed\n");
}

module_init(i2c_wake_init);
module_exit(i2c_wake_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Enable the Type-C path I2C controllers via a runtime DT overlay");
