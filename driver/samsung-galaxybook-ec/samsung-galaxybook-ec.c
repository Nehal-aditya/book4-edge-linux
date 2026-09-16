// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Galaxy Book4 Edge (NP750XQA) embedded controller, ENE KB9058.
 *
 * Two I2C endpoints: 0x62 on the private controller at 0xB94000 and 0x64 on
 * 0xB80000 (shared with the HID keyboard). EC RAM is read through a mailbox
 * protocol; an opcode path on 0x62 carries fan-level commands.
 *
 * Read-only telemetry: battery and AC state, three temperature sensors, the
 * fan level as a cooling device. Fan writes sit behind the fan_control
 * parameter (default off) and are limited to one whitelisted opcode.
 *
 * Mailbox framing after the Saddytech Galaxy-Book4-Edge-linux battery driver
 * (GPL-2.0); register offsets from the DSDT.
 *
 * Copyright (C) 2026 Book4 Edge NP750XQA Linux port contributors
 */

#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/pm_wakeup.h>
#include <linux/power_supply.h>
#include <linux/minmax.h>
#include <linux/property.h>
#include <linux/string.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#define DRV_NAME			"samsung-galaxybook-ec"

/* Mailbox framing */
#define MBOX_WRITE_PREFIX		0x40
#define MBOX_READ_PREFIX		0x30
#define MBOX_READ_OK			0x50
#define MBOX_CMD_BUF0			0xF480	/* mailbox buffer byte 0 */
#define MBOX_CMD_EXEC			0xFF10	/* execute; data = operation */
#define MBOX_OP_READ_EC_SPACE		0x88

/* EC RAM (ACPI region space 0xA1) offsets */
#define ECR_FLAGS			0x80	/* bit0 B1EX, bit2 ACEX */
#define ECR_B1ST			0x84	/* ACPI _BST state word */
#define ECR_FANS			0x87	/* 4-bit fan level, meaning unverified */
#define ECR_B1RR			0xA0	/* remaining capacity (upper BE word) */
#define ECR_B1PV			0xA4	/* voltage (upper), signed rate (lower) */
#define ECR_B1AF			0xB0	/* design (lower), last full (upper) */
#define ECR_B1VL			0xB4	/* design voltage (lower) */
#define ECR_CTMP			0xC0
#define ECR_CET1			0xC2
#define ECR_CET2			0xC3
#define ECR_CYLC			0xD0	/* cycle count, BE16 */

#define FLAG_B1EX			BIT(0)
#define FLAG_ACEX			BIT(2)
#define B1ST_DISCHARGING		BIT(0)
#define B1ST_CHARGING			BIT(1)
#define B1ST_CRITICAL			BIT(2)

/* Opcode path on 0x62. The only opcode this driver may send. */
#define EC_OP_FANZONE			0x08
#define BOOK4_EC_FAN_MAX_STATE		5	/* ACPI _FST domain */

#define BOOK4_EC_MAX_BLOCK		8	/* bus-hog cap per mutex hold */
#define BOOK4_EC_MIN_POLL_S		2

static bool fan_control;
module_param(fan_control, bool, 0444);
MODULE_PARM_DESC(fan_control,
		 "Let the thermal framework set the fan level (opcode 0x08). Default off; enable only after the manual->auto recovery test passed");

static bool allow_poke;
module_param(allow_poke, bool, 0644);
MODULE_PARM_DESC(allow_poke,
		 "Enable the raw_poke experiment (writing EC space). Default off. The EC runs charging, the fan and the gauge, so nothing here is safe by default.");

static unsigned int mbox_delay_us = 5000;
module_param(mbox_delay_us, uint, 0644);
MODULE_PARM_DESC(mbox_delay_us, "Delay after each mailbox write frame in microseconds (default 5000, min 500)");

static unsigned int poll_interval = 10;
module_param(poll_interval, uint, 0644);
MODULE_PARM_DESC(poll_interval, "Telemetry poll interval in seconds (default 10, min 2)");

struct book4_ec_telemetry {
	bool valid;
	unsigned long stamp;	/* jiffies of the read */
	u8 flags;
	u8 bst;
	u8 fans;
	u8 b1rr[4];
	u8 b1pv[4];
	u8 ctmp, cet1, cet2;
	u16 cycles;
};

struct book4_ec {
	struct device *dev;
	struct i2c_client *client;	/* 0x62 on i2c5: DT node, IRQ, opcode path */
	struct i2c_client *client2;	/* 0x64 on i2c0, optional second face */
	struct i2c_adapter *adap2;
	struct i2c_client *mbox;	/* endpoint that answered the mailbox probe */

	/* Serialises every EC transaction on both endpoints. */
	struct mutex lock;

	struct delayed_work poll_work;

	/* Static battery data, read once at probe. */
	u16 design_mah;
	u16 full_mah;
	u16 design_mv;

	struct mutex cache_lock;	/* protects cache */
	struct book4_ec_telemetry cache;

	struct power_supply *bat_psy;
	struct power_supply *ac_psy;

	struct thermal_cooling_device *fan_cdev;
	u8 fan_state;		/* last state we wrote */
	bool fan_manual;	/* true once we took the fan off EC auto */
	bool fan_frozen;	/* set while restoring auto: set_cur_state -> -EBUSY */
};

static inline u16 be16_at(const u8 *b)
{
	return ((u16)b[0] << 8) | b[1];
}

/* ---------------------------------------------------------------------- */
/* Transport. Only these functions touch the I2C API.                      */
/* ---------------------------------------------------------------------- */

static int book4_ec_mbox_write_locked(struct book4_ec *ec, u16 cmd, u8 data)
{
	u8 buf[5] = { MBOX_WRITE_PREFIX, 0x00, cmd >> 8, cmd & 0xff, data };
	unsigned int delay = max_t(unsigned int, mbox_delay_us, 500);
	int ret;

	lockdep_assert_held(&ec->lock);

	dev_dbg(ec->dev, "EC mbox write %*ph -> %s\n", (int)sizeof(buf), buf,
		dev_name(&ec->mbox->dev));

	ret = i2c_master_send(ec->mbox, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;

	usleep_range(delay, delay + 1000);
	return 0;
}

static int book4_ec_mbox_read_locked(struct book4_ec *ec, u16 cmd, u8 *out)
{
	u8 wbuf[4] = { MBOX_READ_PREFIX, 0x00, cmd >> 8, cmd & 0xff };
	u8 rbuf[2] = { 0, 0 };
	struct i2c_msg msgs[2] = {
		{
			.addr = ec->mbox->addr,
			.flags = 0,
			.len = sizeof(wbuf),
			.buf = wbuf,
		}, {
			.addr = ec->mbox->addr,
			.flags = I2C_M_RD,
			.len = sizeof(rbuf),
			.buf = rbuf,
		},
	};
	int ret;

	lockdep_assert_held(&ec->lock);

	/*
	 * Write and read must be one transaction with a repeated START. A STOP
	 * in between makes the EC drop its mailbox state.
	 */
	ret = i2c_transfer(ec->mbox->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	if (rbuf[0] != MBOX_READ_OK) {
		dev_err_ratelimited(ec->dev,
				    "EC mbox read cmd 0x%04x: status 0x%02x, expected 0x50\n",
				    cmd, rbuf[0]);
		return -EIO;
	}

	*out = rbuf[1];
	return 0;
}

static int book4_ec_ram_read_once_locked(struct book4_ec *ec, u8 reg, u8 *out)
{
	int ret;

	lockdep_assert_held(&ec->lock);

	ret = book4_ec_mbox_write_locked(ec, MBOX_CMD_BUF0, reg);
	if (ret)
		return ret;
	ret = book4_ec_mbox_write_locked(ec, MBOX_CMD_EXEC, MBOX_OP_READ_EC_SPACE);
	if (ret)
		return ret;
	return book4_ec_mbox_read_locked(ec, MBOX_CMD_BUF0, out);
}

/* The EC occasionally answers a read with a non-0x50 status; one retry. */
static int book4_ec_ram_read_locked(struct book4_ec *ec, u8 reg, u8 *out)
{
	int ret = book4_ec_ram_read_once_locked(ec, reg, out);

	if (ret == -EIO)
		ret = book4_ec_ram_read_once_locked(ec, reg, out);
	return ret;
}

static int book4_ec_ram_read(struct book4_ec *ec, u8 reg, u8 *out)
{
	guard(mutex)(&ec->lock);

	if (!ec->mbox)
		return -ENODEV;
	return book4_ec_ram_read_locked(ec, reg, out);
}

static int book4_ec_ram_read_block(struct book4_ec *ec, u8 reg, u8 *buf, size_t len)
{
	size_t i;
	int ret;

	if (len == 0 || len > BOOK4_EC_MAX_BLOCK || reg + len > 0x100)
		return -EINVAL;

	guard(mutex)(&ec->lock);

	if (!ec->mbox)
		return -ENODEV;

	for (i = 0; i < len; i++) {
		ret = book4_ec_ram_read_locked(ec, reg + i, &buf[i]);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * Opcode path on the 0x62 endpoint. Whitelist: FANZONE only. Everything else
 * is refused here so no caller can widen the write surface by accident.
 */
static int book4_ec_opcode_write_locked(struct book4_ec *ec, u8 opcode, u8 arg)
{
	u8 buf[2] = { opcode, arg };
	int ret;

	lockdep_assert_held(&ec->lock);

	if (opcode != EC_OP_FANZONE)
		return -EINVAL;

	dev_dbg(ec->dev, "EC opcode write %*ph -> %s\n", (int)sizeof(buf), buf,
		dev_name(&ec->client->dev));

	ret = i2c_master_send(ec->client, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	if (ret != sizeof(buf))
		return -EIO;

	usleep_range(10000, 12000);
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Telemetry                                                               */
/* ---------------------------------------------------------------------- */

static int book4_ec_refresh(struct book4_ec *ec)
{
	struct book4_ec_telemetry t = { };
	u8 blk[8];
	int ret;

	ret = book4_ec_ram_read(ec, ECR_FLAGS, &t.flags);
	if (ret)
		return ret;
	ret = book4_ec_ram_read(ec, ECR_B1ST, &t.bst);
	if (ret)
		return ret;
	ret = book4_ec_ram_read(ec, ECR_FANS, &t.fans);
	if (ret)
		return ret;
	ret = book4_ec_ram_read_block(ec, ECR_B1RR, blk, 8);	/* B1RR + B1PV */
	if (ret)
		return ret;
	memcpy(t.b1rr, blk, 4);
	memcpy(t.b1pv, blk + 4, 4);
	ret = book4_ec_ram_read(ec, ECR_CTMP, &t.ctmp);
	if (ret)
		return ret;
	ret = book4_ec_ram_read(ec, ECR_CET1, &t.cet1);
	if (ret)
		return ret;
	ret = book4_ec_ram_read(ec, ECR_CET2, &t.cet2);
	if (ret)
		return ret;
	ret = book4_ec_ram_read_block(ec, ECR_CYLC, blk, 2);
	if (ret)
		return ret;
	t.cycles = be16_at(blk);
	t.valid = true;
	t.stamp = jiffies;

	guard(mutex)(&ec->cache_lock);
	ec->cache = t;
	return 0;
}

/* A failed refresh must not leave old values looking current. */
static void book4_ec_invalidate(struct book4_ec *ec)
{
	guard(mutex)(&ec->cache_lock);
	ec->cache.valid = false;
}

/*
 * Cached telemetry, or an invalid snapshot if the last good read is older than
 * three poll intervals. Readers report -ENODATA on an invalid snapshot.
 */
static struct book4_ec_telemetry book4_ec_snapshot(struct book4_ec *ec)
{
	struct book4_ec_telemetry t;
	unsigned int interval = max_t(unsigned int, poll_interval, BOOK4_EC_MIN_POLL_S);

	scoped_guard(mutex, &ec->cache_lock)
		t = ec->cache;

	if (t.valid && time_after(jiffies, t.stamp + 3 * interval * HZ))
		t.valid = false;
	return t;
}

static void book4_ec_poll_work(struct work_struct *work)
{
	struct book4_ec *ec = container_of(to_delayed_work(work), struct book4_ec, poll_work);
	struct book4_ec_telemetry before = book4_ec_snapshot(ec);
	unsigned int interval = max_t(unsigned int, poll_interval, BOOK4_EC_MIN_POLL_S);
	int ret;

	ret = book4_ec_refresh(ec);
	if (ret) {
		dev_warn_ratelimited(ec->dev, "telemetry refresh failed: %d\n", ret);
		book4_ec_invalidate(ec);
		power_supply_changed(ec->bat_psy);
		power_supply_changed(ec->ac_psy);
	} else {
		struct book4_ec_telemetry after = book4_ec_snapshot(ec);

		/* Notify on state changes and on battery-value changes (normal discharge). */
		if (!before.valid || before.flags != after.flags || before.bst != after.bst ||
		    memcmp(before.b1rr, after.b1rr, sizeof(after.b1rr)) ||
		    be16_at(before.b1pv + 2) != be16_at(after.b1pv + 2)) {
			power_supply_changed(ec->bat_psy);
			power_supply_changed(ec->ac_psy);
		}
	}

	schedule_delayed_work(&ec->poll_work, interval * HZ);
}

static irqreturn_t book4_ec_irq(int irq, void *data)
{
	struct book4_ec *ec = data;

	/* The I2C controller is not ready immediately after resume. */
	usleep_range(15000, 30000);

	if (book4_ec_refresh(ec)) {
		dev_dbg(ec->dev, "IRQ refresh failed\n");
		book4_ec_invalidate(ec);
	}

	power_supply_changed(ec->bat_psy);
	power_supply_changed(ec->ac_psy);

	return IRQ_HANDLED;	/* never IRQ_NONE, even on I2C error */
}

/* ---------------------------------------------------------------------- */
/* power_supply                                                            */
/* ---------------------------------------------------------------------- */

static enum power_supply_property book4_ec_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_SCOPE,
};

static enum power_supply_property book4_ec_ac_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static int book4_ec_bat_status(const struct book4_ec_telemetry *t, u16 remaining, u16 full)
{
	bool ac = t->flags & FLAG_ACEX;

	if (t->bst & B1ST_CHARGING)
		return POWER_SUPPLY_STATUS_CHARGING;
	if (t->bst & B1ST_DISCHARGING)
		return POWER_SUPPLY_STATUS_DISCHARGING;
	if (ac && full && remaining != 0xFFFF && remaining * 100 >= full * 97)
		return POWER_SUPPLY_STATUS_FULL;
	if (ac)
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
	return POWER_SUPPLY_STATUS_UNKNOWN;
}

static int book4_ec_bat_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct book4_ec *ec = power_supply_get_drvdata(psy);
	struct book4_ec_telemetry t = book4_ec_snapshot(ec);
	u16 remaining, voltage;
	s16 rate;

	switch (psp) {
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;	/* nameplate assumption */
		return 0;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = ec->design_mah * 1000;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		if (ec->full_mah == 0xFFFF || ec->full_mah == 0)
			return -ENODATA;
		val->intval = ec->full_mah * 1000;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		if (ec->design_mv == 0xFFFF || ec->design_mv == 0)
			return -ENODATA;
		val->intval = ec->design_mv * 1000;
		return 0;
	default:
		break;
	}

	if (!t.valid)
		return -ENODATA;

	remaining = be16_at(t.b1rr + 2);
	voltage = be16_at(t.b1pv + 2);
	rate = (s16)be16_at(t.b1pv);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = book4_ec_bat_status(&t, remaining, ec->full_mah);
		return 0;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = !!(t.flags & FLAG_B1EX);
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		if (voltage == 0xFFFF)
			return -ENODATA;
		val->intval = voltage * 1000;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		if (be16_at(t.b1pv) == 0xFFFF)
			return -ENODATA;
		/*
		 * The EC reports the rate as a magnitude; direction comes from the
		 * state byte. power_supply wants positive = charging.
		 */
		val->intval = (t.bst & B1ST_CHARGING ? 1 : -1) * abs((int)rate) * 1000;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		if (remaining == 0xFFFF)
			return -ENODATA;
		val->intval = remaining * 1000;
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (remaining == 0xFFFF || ec->full_mah == 0 || ec->full_mah == 0xFFFF)
			return -ENODATA;
		val->intval = clamp_t(int, (int)remaining * 100 / ec->full_mah, 0, 100);
		return 0;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		if (t.cycles == 0xFFFF)
			return -ENODATA;
		val->intval = t.cycles;
		return 0;
	default:
		return -EINVAL;
	}
}

static int book4_ec_ac_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct book4_ec *ec = power_supply_get_drvdata(psy);
	struct book4_ec_telemetry t = book4_ec_snapshot(ec);

	if (psp != POWER_SUPPLY_PROP_ONLINE)
		return -EINVAL;
	if (!t.valid)
		return -ENODATA;
	val->intval = !!(t.flags & FLAG_ACEX);
	return 0;
}

static const struct power_supply_desc book4_ec_bat_desc = {
	.name = "book4-ec-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = book4_ec_bat_props,
	.num_properties = ARRAY_SIZE(book4_ec_bat_props),
	.get_property = book4_ec_bat_get_property,
};

static const struct power_supply_desc book4_ec_ac_desc = {
	.name = "book4-ec-ac",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = book4_ec_ac_props,
	.num_properties = ARRAY_SIZE(book4_ec_ac_props),
	.get_property = book4_ec_ac_get_property,
};

/* ---------------------------------------------------------------------- */
/* hwmon: three EC temperature bytes, units assumed degrees Celsius        */
/* ---------------------------------------------------------------------- */

static const char * const book4_ec_temp_labels[] = {
	"ec_ctmp",
	"ec_thermistor1",
	"ec_thermistor2",
};

static umode_t book4_ec_hwmon_is_visible(const void *data, enum hwmon_sensor_types type,
					 u32 attr, int channel)
{
	if (type != hwmon_temp)
		return 0;
	if (attr == hwmon_temp_input || attr == hwmon_temp_label)
		return 0444;
	return 0;
}

static int book4_ec_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long *val)
{
	struct book4_ec *ec = dev_get_drvdata(dev);
	struct book4_ec_telemetry t = book4_ec_snapshot(ec);
	u8 raw;

	if (type != hwmon_temp || attr != hwmon_temp_input)
		return -EOPNOTSUPP;
	if (!t.valid)
		return -ENODATA;

	switch (channel) {
	case 0:
		raw = t.ctmp;
		break;
	case 1:
		raw = t.cet1;
		break;
	case 2:
		raw = t.cet2;
		break;
	default:
		return -EINVAL;
	}

	if (raw == 0xFF || raw == 0x00)
		return -ENODATA;
	*val = (long)raw * 1000;
	return 0;
}

static int book4_ec_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type,
				      u32 attr, int channel, const char **str)
{
	if (type != hwmon_temp || attr != hwmon_temp_label)
		return -EOPNOTSUPP;
	if (channel < 0 || channel >= ARRAY_SIZE(book4_ec_temp_labels))
		return -EINVAL;
	*str = book4_ec_temp_labels[channel];
	return 0;
}

static const struct hwmon_ops book4_ec_hwmon_ops = {
	.is_visible = book4_ec_hwmon_is_visible,
	.read = book4_ec_hwmon_read,
	.read_string = book4_ec_hwmon_read_string,
};

static const struct hwmon_channel_info * const book4_ec_hwmon_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_chip_info book4_ec_hwmon_chip_info = {
	.ops = &book4_ec_hwmon_ops,
	.info = book4_ec_hwmon_info,
};

/* ---------------------------------------------------------------------- */
/* Fan cooling device                                                      */
/* ---------------------------------------------------------------------- */

/*
 * Hand the fan back to the EC. Sets fan_frozen first so a racing
 * set_cur_state() is refused, then sends FANZONE 0. On success clears the
 * manual bookkeeping. Callers decide what a failure means (suspend aborts).
 */
static int book4_ec_fan_restore_auto(struct book4_ec *ec)
{
	int ret;

	guard(mutex)(&ec->lock);

	ec->fan_frozen = true;
	if (!ec->fan_manual)
		return 0;

	ret = book4_ec_opcode_write_locked(ec, EC_OP_FANZONE, 0);
	if (ret) {
		dev_err(ec->dev, "failed to return fan to EC control: %d\n", ret);
		return ret;
	}
	ec->fan_manual = false;
	ec->fan_state = 0;
	return 0;
}

static void book4_ec_fan_unfreeze(struct book4_ec *ec)
{
	guard(mutex)(&ec->lock);
	ec->fan_frozen = false;
}

/* devm teardown hook: runs on probe failure and after remove(). Idempotent. */
static void book4_ec_fan_auto_action(void *data)
{
	struct book4_ec *ec = data;

	cancel_delayed_work_sync(&ec->poll_work);
	book4_ec_fan_restore_auto(ec);
}

static int book4_ec_fan_get_max_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	*state = BOOK4_EC_FAN_MAX_STATE;
	return 0;
}

static int book4_ec_fan_get_cur_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	struct book4_ec *ec = cdev->devdata;

	*state = ec->fan_state;	/* cached; state 0 means EC automatic */
	return 0;
}

static int book4_ec_fan_set_cur_state(struct thermal_cooling_device *cdev, unsigned long state)
{
	struct book4_ec *ec = cdev->devdata;
	int ret;

	if (state > BOOK4_EC_FAN_MAX_STATE)
		return -EINVAL;
	if (!fan_control)
		return -EOPNOTSUPP;

	guard(mutex)(&ec->lock);

	if (ec->fan_frozen)
		return -EBUSY;

	/* State 0 means "EC automatic control", not "fan stopped". */
	ret = book4_ec_opcode_write_locked(ec, EC_OP_FANZONE, (u8)state);
	if (ret)
		return ret;

	ec->fan_state = state;
	ec->fan_manual = state != 0;
	return 0;
}

static const struct thermal_cooling_device_ops book4_ec_fan_ops = {
	.get_max_state = book4_ec_fan_get_max_state,
	.get_cur_state = book4_ec_fan_get_cur_state,
	.set_cur_state = book4_ec_fan_set_cur_state,
};

/* ---------------------------------------------------------------------- */
/* Debug attributes: raw EC RAM bytes, read live. Read-only.               */
/* ---------------------------------------------------------------------- */

static ssize_t book4_ec_raw_show(struct device *dev, u8 reg, char *buf)
{
	struct book4_ec *ec = dev_get_drvdata(dev);
	u8 v;
	int ret;

	ret = book4_ec_ram_read(ec, reg, &v);
	if (ret)
		return ret;
	return sysfs_emit(buf, "0x%02x\n", v);
}

static ssize_t raw_flags_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return book4_ec_raw_show(dev, ECR_FLAGS, buf);
}

static ssize_t raw_battery_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return book4_ec_raw_show(dev, ECR_B1ST, buf);
}

static ssize_t raw_fan_level_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return book4_ec_raw_show(dev, ECR_FANS, buf);
}

/*
 * The whole 256-byte EC RAM as 32 rows of 8. One mailbox round trip per
 * byte: a debug attribute, not something to poll.
 */
static ssize_t raw_ram_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct book4_ec *ec = dev_get_drvdata(dev);
	u8 row[BOOK4_EC_MAX_BLOCK];
	unsigned int reg, i;
	size_t len = 0;
	int ret;

	for (reg = 0; reg < 0x100; reg += BOOK4_EC_MAX_BLOCK) {
		/* Breathe between rows; a back-to-back dump has upset the EC. */
		if (reg)
			usleep_range(2000, 3000);
		ret = book4_ec_ram_read_block(ec, reg, row, BOOK4_EC_MAX_BLOCK);
		if (ret)
			return ret;
		len += sysfs_emit_at(buf, len, "%02x:", reg);
		for (i = 0; i < BOOK4_EC_MAX_BLOCK; i++)
			len += sysfs_emit_at(buf, len, " %02x", row[i]);
		len += sysfs_emit_at(buf, len, "\n");
	}
	return len;
}

/*
 * Experimental mailbox write: takes "op reg val" and runs the obvious write
 * shape. Gated behind allow_poke: a wrong opcode goes to the controller that
 * runs charging, the fan and the battery gauge.
 */
static ssize_t raw_poke_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct book4_ec *ec = dev_get_drvdata(dev);
	unsigned int op, reg, val;
	int ret;

	if (!allow_poke)
		return -EPERM;
	if (sscanf(buf, "%x %x %x", &op, &reg, &val) != 3)
		return -EINVAL;
	if (op > 0xff || reg > 0xff || val > 0xff)
		return -EINVAL;

	guard(mutex)(&ec->lock);
	if (!ec->mbox)
		return -ENODEV;

	ret = book4_ec_mbox_write_locked(ec, MBOX_CMD_BUF0, reg);
	if (ret)
		return ret;
	ret = book4_ec_mbox_write_locked(ec, MBOX_CMD_BUF0 + 1, val);
	if (ret)
		return ret;
	ret = book4_ec_mbox_write_locked(ec, MBOX_CMD_EXEC, op);
	if (ret)
		return ret;

	dev_info(ec->dev, "poke: op %02x reg %02x val %02x sent\n", op, reg, val);
	return count;
}
static DEVICE_ATTR_WO(raw_poke);

/*
 * Experimental mailbox read: one command byte with one offset, reply reported
 * as is. 0x88/0x89 are EC RAM read/write; the other ACPI region spaces (e.g.
 * BMOP 0x9E, the charge limit) have their own, still unknown, command bytes.
 */
static u8 last_cmd_value;
static int last_cmd_ret;

static ssize_t raw_cmd_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct book4_ec *ec = dev_get_drvdata(dev);
	unsigned int op, reg;
	int ret;
	u8 v = 0;

	if (!allow_poke)
		return -EPERM;
	if (sscanf(buf, "%x %x", &op, &reg) != 2 || op > 0xff || reg > 0xff)
		return -EINVAL;

	guard(mutex)(&ec->lock);
	if (!ec->mbox)
		return -ENODEV;

	ret = book4_ec_mbox_write_locked(ec, MBOX_CMD_BUF0, reg);
	if (!ret)
		ret = book4_ec_mbox_write_locked(ec, MBOX_CMD_EXEC, op);
	if (!ret)
		ret = book4_ec_mbox_read_locked(ec, MBOX_CMD_BUF0, &v);

	last_cmd_ret = ret;
	last_cmd_value = v;
	dev_info(ec->dev, "cmd: op %02x reg %02x -> ret %d value 0x%02x\n", op, reg, ret, v);
	return count;
}

static ssize_t raw_cmd_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "ret %d value 0x%02x\n", last_cmd_ret, last_cmd_value);
}
static DEVICE_ATTR_RW(raw_cmd);

/*
 * Extended-command path (EC2.sys SecEne9058KbcSendExCmd): payload bytes to
 * MBOX_CMD_BUF0 + i, then 0x86 to the exec register. payload[0] is the
 * sub-command (0x06 battery trip point, 0x0D cable detect). Takes
 * "op byte0 byte1 ..." in hex, up to 15 payload bytes.
 */
#define MBOX_OP_SEND_EXCMD	0x86
#define BOOK4_EC_MAX_PAYLOAD	15

static ssize_t raw_excmd_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct book4_ec *ec = dev_get_drvdata(dev);
	u8 payload[BOOK4_EC_MAX_PAYLOAD], reply[BOOK4_EC_MAX_PAYLOAD] = {};
	unsigned int op, v, len = 0, tries;
	const char *p = buf;
	int ret, n;

	if (!allow_poke)
		return -EPERM;
	if (sscanf(p, "%x%n", &op, &n) != 1 || op > 0xff)
		return -EINVAL;
	p += n;
	while (len < BOOK4_EC_MAX_PAYLOAD && sscanf(p, "%x%n", &v, &n) == 1) {
		if (v > 0xff)
			return -EINVAL;
		payload[len++] = v;
		p += n;
	}
	if (!len)
		return -EINVAL;

	guard(mutex)(&ec->lock);
	if (!ec->mbox)
		return -ENODEV;

	for (unsigned int i = 0; i < len; i++) {
		ret = book4_ec_mbox_write_locked(ec, MBOX_CMD_BUF0 + i, payload[i]);
		if (ret)
			return ret;
	}
	ret = book4_ec_mbox_write_locked(ec, MBOX_CMD_EXEC, op);
	if (ret)
		return ret;

	/*
	 * The exec register reads non-zero while the command runs. Poll it
	 * (1 ms, up to 30 times) before reading the response.
	 */
	for (tries = 0; tries < 30; tries++) {
		u8 busy;

		ret = book4_ec_mbox_read_locked(ec, MBOX_CMD_EXEC, &busy);
		if (ret)
			return ret;
		if (!busy)
			break;
		usleep_range(1000, 1500);
	}

	for (unsigned int i = 0; i < len; i++)
		if (book4_ec_mbox_read_locked(ec, MBOX_CMD_BUF0 + i, &reply[i]))
			break;

	dev_info(ec->dev, "excmd: op %02x sent %*ph -> after %u polls reply %*ph\n",
		 op, len, payload, tries, len, reply);
	return count;
}
static DEVICE_ATTR_WO(raw_excmd);

static DEVICE_ATTR_RO(raw_flags);
static DEVICE_ATTR_RO(raw_battery_state);
static DEVICE_ATTR_RO(raw_fan_level);
static DEVICE_ATTR_RO(raw_ram);

static struct attribute *book4_ec_attrs[] = {
	&dev_attr_raw_flags.attr,
	&dev_attr_raw_battery_state.attr,
	&dev_attr_raw_fan_level.attr,
	&dev_attr_raw_ram.attr,
	&dev_attr_raw_poke.attr,
	&dev_attr_raw_cmd.attr,
	&dev_attr_raw_excmd.attr,
	NULL
};
ATTRIBUTE_GROUPS(book4_ec);

/* ---------------------------------------------------------------------- */
/* Probe helpers                                                           */
/* ---------------------------------------------------------------------- */

static void book4_ec_release_client2(void *data)
{
	struct book4_ec *ec = data;

	if (ec->client2)
		i2c_unregister_device(ec->client2);
	if (ec->adap2)
		i2c_put_adapter(ec->adap2);
}

/*
 * Second EC face, declared by "samsung,ec-bus" (adapter phandle) and
 * "samsung,ec-bus-reg" (address, default 0x64). Absent property: single
 * endpoint. Present but adapter not registered yet: defer. Present and
 * adapter up but device creation fails: hard error.
 */
static int book4_ec_setup_client2(struct book4_ec *ec)
{
	struct fwnode_handle *fw;
	struct i2c_adapter *adap;
	struct i2c_client *cl;
	u32 reg = 0x64;
	int ret;

	fw = fwnode_find_reference(dev_fwnode(ec->dev), "samsung,ec-bus", 0);
	if (IS_ERR(fw)) {
		dev_info(ec->dev, "no samsung,ec-bus reference (%ld): single-endpoint mode\n",
			 PTR_ERR(fw));
		return 0;
	}

	adap = i2c_get_adapter_by_fwnode(fw);
	fwnode_handle_put(fw);
	if (!adap)
		return -EPROBE_DEFER;

	device_property_read_u32(ec->dev, "samsung,ec-bus-reg", &reg);
	if (reg > 0x7f) {
		i2c_put_adapter(adap);
		return dev_err_probe(ec->dev, -EINVAL,
				     "samsung,ec-bus-reg 0x%x out of range\n", reg);
	}

	cl = i2c_new_dummy_device(adap, reg);
	if (IS_ERR(cl)) {
		i2c_put_adapter(adap);
		return dev_err_probe(ec->dev, PTR_ERR(cl), "cannot create second EC endpoint\n");
	}

	ec->adap2 = adap;
	ec->client2 = cl;
	ret = devm_add_action_or_reset(ec->dev, book4_ec_release_client2, ec);
	if (ret)
		return ret;

	dev_info(ec->dev, "second EC endpoint 0x%02x on %s\n", reg, dev_name(&adap->dev));
	return 0;
}

/*
 * Identity gate. Which endpoint carries the mailbox is not determined by any
 * static source, so try 0x62 first (private bus), then 0x64. Whichever
 * answers a READ_EC_SPACE of the flags byte with status 0x50 is the mailbox.
 */
static int book4_ec_detect_mbox(struct book4_ec *ec, u8 *flags)
{
	struct i2c_client *cands[] = { ec->client, ec->client2 };
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cands); i++) {
		int ret;

		if (!cands[i])
			continue;

		scoped_guard(mutex, &ec->lock) {
			ec->mbox = cands[i];
			ret = book4_ec_ram_read_locked(ec, ECR_FLAGS, flags);
			if (ret)
				ec->mbox = NULL;
		}

		if (!ret) {
			dev_info(ec->dev, "EC mailbox answers at 0x%02x on %s (flags 0x%02x)\n",
				 cands[i]->addr, dev_name(&cands[i]->adapter->dev), *flags);
			return 0;
		}
		dev_info(ec->dev, "no mailbox at 0x%02x on %s: %d\n",
			 cands[i]->addr, dev_name(&cands[i]->adapter->dev), ret);
	}
	return -ENODEV;
}

static int book4_ec_read_static(struct book4_ec *ec)
{
	u8 blk[8];
	int ret;

	ret = book4_ec_ram_read_block(ec, ECR_B1AF, blk, 8);	/* B1AF + B1VL */
	if (ret)
		return ret;

	ec->design_mah = be16_at(blk);		/* B1AF lower word */
	ec->full_mah = be16_at(blk + 2);	/* B1AF upper word */
	ec->design_mv = be16_at(blk + 4);	/* B1VL lower word */

	/* Sanity gate: refuse to export nonsense to userspace. */
	if (ec->design_mah < 1000 || ec->design_mah > 20000)
		return dev_err_probe(ec->dev, -ENODEV,
				     "implausible design capacity %u mAh (raw %*ph); wrong endpoint or framing\n",
				     ec->design_mah, 8, blk);

	dev_info(ec->dev, "battery design %u mAh, last full %u mAh, design %u mV\n",
		 ec->design_mah, ec->full_mah, ec->design_mv);
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Driver glue                                                             */
/* ---------------------------------------------------------------------- */

static int book4_ec_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct power_supply_config psy_cfg = { };
	struct book4_ec *ec;
	struct device *hwmon;
	u8 flags;
	int ret;

	ec = devm_kzalloc(dev, sizeof(*ec), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;

	ec->dev = dev;
	ec->client = client;
	i2c_set_clientdata(client, ec);

	ret = devm_mutex_init(dev, &ec->lock);
	if (ret)
		return ret;
	ret = devm_mutex_init(dev, &ec->cache_lock);
	if (ret)
		return ret;
	INIT_DELAYED_WORK(&ec->poll_work, book4_ec_poll_work);

	ret = book4_ec_setup_client2(ec);
	if (ret)
		return ret;

	ret = book4_ec_detect_mbox(ec, &flags);
	if (ret)
		return dev_err_probe(dev, ret, "EC mailbox not found on any endpoint\n");

	ret = book4_ec_read_static(ec);
	if (ret)
		return ret;

	ret = book4_ec_refresh(ec);
	if (ret)
		return dev_err_probe(dev, ret, "initial telemetry read failed\n");

	hwmon = devm_hwmon_device_register_with_info(dev, "book4_ec", ec,
						     &book4_ec_hwmon_chip_info, NULL);
	if (IS_ERR(hwmon))
		return dev_err_probe(dev, PTR_ERR(hwmon), "hwmon registration failed\n");

	psy_cfg.drv_data = ec;
	psy_cfg.fwnode = dev_fwnode(dev);

	ec->bat_psy = devm_power_supply_register(dev, &book4_ec_bat_desc, &psy_cfg);
	if (IS_ERR(ec->bat_psy))
		return dev_err_probe(dev, PTR_ERR(ec->bat_psy), "battery registration failed\n");

	ec->ac_psy = devm_power_supply_register(dev, &book4_ec_ac_desc, &psy_cfg);
	if (IS_ERR(ec->ac_psy))
		return dev_err_probe(dev, PTR_ERR(ec->ac_psy), "AC registration failed\n");

	/*
	 * From here on a governor may write the fan (fan_control=1). Make sure
	 * every exit path, including a failed probe below and driver removal,
	 * hands the fan back to the EC. Registered before the cooling device so
	 * it runs after the device is gone.
	 */
	ret = devm_add_action_or_reset(dev, book4_ec_fan_auto_action, ec);
	if (ret)
		return ret;

	/* Registered even with fan_control off so DT cooling maps can be tested. */
	ec->fan_cdev = devm_thermal_of_cooling_device_register(dev, 0, "book4-ec-fan", ec,
							       &book4_ec_fan_ops);
	if (IS_ERR(ec->fan_cdev)) {
		dev_warn(dev, "fan cooling device not registered: %ld\n", PTR_ERR(ec->fan_cdev));
		ec->fan_cdev = NULL;
	}

	if (client->irq > 0) {
		ret = devm_request_threaded_irq(dev, client->irq, NULL, book4_ec_irq,
						IRQF_ONESHOT, dev_name(dev), ec);
		if (ret)
			return dev_err_probe(dev, ret, "cannot request EC IRQ\n");
		/* Wake-capable in firmware, but we cannot filter wake events yet. */
		device_wakeup_disable(dev);
	} else {
		dev_warn(dev, "no IRQ; relying on polling\n");
	}

	schedule_delayed_work(&ec->poll_work,
			      max_t(unsigned int, poll_interval, BOOK4_EC_MIN_POLL_S) * HZ);

	dev_info(dev, "ready (fan control %s)\n", fan_control ? "enabled" : "disabled");
	return 0;
}

static void book4_ec_stop(struct book4_ec *ec)
{
	cancel_delayed_work_sync(&ec->poll_work);
	/* Never leave the fan under manual control of a driver that is gone. */
	book4_ec_fan_restore_auto(ec);
}

static void book4_ec_remove(struct i2c_client *client)
{
	book4_ec_stop(i2c_get_clientdata(client));
}

static void book4_ec_shutdown(struct i2c_client *client)
{
	book4_ec_stop(i2c_get_clientdata(client));
}

static int book4_ec_suspend(struct device *dev)
{
	struct book4_ec *ec = dev_get_drvdata(dev);
	int ret;

	cancel_delayed_work_sync(&ec->poll_work);

	/*
	 * The EC keeps running and owns thermal control while the AP sleeps.
	 * A manual level must not be latched across suspend. If the restore
	 * fails, refuse to suspend rather than sleep with an unknown fan state.
	 */
	ret = book4_ec_fan_restore_auto(ec);
	if (ret) {
		book4_ec_fan_unfreeze(ec);
		schedule_delayed_work(&ec->poll_work, HZ);
		return ret;
	}
	return 0;
}

static int book4_ec_resume(struct device *dev)
{
	struct book4_ec *ec = dev_get_drvdata(dev);

	book4_ec_fan_unfreeze(ec);

	scoped_guard(mutex, &ec->cache_lock)
		ec->cache.valid = false;

	/* Do not replay a manual level; the governor re-drives it from real temperatures. */
	schedule_delayed_work(&ec->poll_work, HZ);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(book4_ec_pm_ops, book4_ec_suspend, book4_ec_resume);

static const struct of_device_id book4_ec_of_match[] = {
	{ .compatible = "samsung,galaxy-book4-edge-ec" },
	{ }
};
MODULE_DEVICE_TABLE(of, book4_ec_of_match);

static const struct i2c_device_id book4_ec_i2c_id[] = {
	{ "galaxy-book4-edge-ec" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, book4_ec_i2c_id);

static struct i2c_driver book4_ec_driver = {
	.driver = {
		.name = DRV_NAME,
		.of_match_table = book4_ec_of_match,
		.pm = pm_sleep_ptr(&book4_ec_pm_ops),
		.dev_groups = book4_ec_groups,
	},
	.probe = book4_ec_probe,
	.remove = book4_ec_remove,
	.shutdown = book4_ec_shutdown,
	.id_table = book4_ec_i2c_id,
};
module_i2c_driver(book4_ec_driver);

MODULE_DESCRIPTION("Samsung Galaxy Book4 Edge (Snapdragon X Plus) embedded controller driver");
MODULE_LICENSE("GPL");
