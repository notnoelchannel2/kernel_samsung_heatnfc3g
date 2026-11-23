/*
 * Samsung kylepro (GT-S7580) sensor devices on BSC3 (i2c bus 2).
 *
 * Ported from the kylepro 3.4 board file (board-hawaii_ss_kylepro.c):
 *   bma2x2   accelerometer   0x10, INT on GPIO92
 *   bmm050   magnetometer    0x12
 *   gp2ap002 proximity       0x44, INT on GPIO89, VDD tcxldo, LED vibldo
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/regulator/consumer.h>
#include <linux/bst_sensor_common.h>
#include <linux/gp2ap002_dev.h>

#define KYLEPRO_SENSOR_I2C_BUS	2

#define ACC_INT_GPIO_PIN	92
#define PROXI_INT_GPIO_PIN	89

#define BMA2X2_SLAVE_ADDR	0x10
#define BMM050_SLAVE_ADDR	0x12
#define GP2AP002_SLAVE_ADDR	0x44

static struct bosch_sensor_specific bss_bma2x2 = {
	.name = "bma2x2",
	.place = 0,
	.irq = ACC_INT_GPIO_PIN,
};

static struct bosch_sensor_specific bss_bmm050 = {
	.name = "bmm050",
	.place = 0,
};

static void gp2ap002_power_onoff(bool onoff)
{
	struct regulator *reg;
	int ret;

	if (!onoff)
		return;

	reg = regulator_get(NULL, "tcxldo_uc");
	if (IS_ERR(reg)) {
		pr_err("[GP2A] cannot get tcxldo_uc (SENSOR_3.3V)\n");
		return;
	}
	ret = regulator_set_voltage(reg, 3300000, 3300000);
	if (ret)
		pr_err("[GP2A] tcxldo_uc set_voltage: %d\n", ret);
	ret = regulator_enable(reg);
	if (ret)
		pr_err("[GP2A] tcxldo_uc enable: %d\n", ret);
	regulator_put(reg);
	mdelay(5);
}

static void gp2ap002_led_onoff(bool onoff)
{
	struct regulator *reg;
	int ret;

	reg = regulator_get(NULL, "vibldo_uc");
	if (IS_ERR(reg)) {
		pr_err("[GP2A] cannot get vibldo_uc (SENSOR_LED_3.3V)\n");
		return;
	}
	if (onoff) {
		ret = regulator_set_voltage(reg, 3000000, 3000000);
		if (ret)
			pr_err("[GP2A] vibldo_uc set_voltage: %d\n", ret);
		ret = regulator_enable(reg);
		if (ret)
			pr_err("[GP2A] vibldo_uc enable: %d\n", ret);
		mdelay(5);
	} else {
		ret = regulator_disable(reg);
		if (ret)
			pr_err("[GP2A] vibldo_uc disable: %d\n", ret);
	}
	regulator_put(reg);
}

static struct gp2ap002_platform_data gp2ap002_pdata = {
	.power_on = gp2ap002_power_onoff,
	.led_on = gp2ap002_led_onoff,
	.irq_gpio = PROXI_INT_GPIO_PIN,
	/* .irq is filled at init: gpio_to_irq() is not a constant here */
};

static struct i2c_board_info __initdata kylepro_sensor_i2c_info[] = {
	{
		I2C_BOARD_INFO("bma2x2", BMA2X2_SLAVE_ADDR),
		.platform_data = &bss_bma2x2,
	},
	{
		I2C_BOARD_INFO("bmm050", BMM050_SLAVE_ADDR),
		.platform_data = &bss_bmm050,
	},
	{
		I2C_BOARD_INFO("gp2ap002", GP2AP002_SLAVE_ADDR),
		.platform_data = &gp2ap002_pdata,
	},
};

void __init hawaii_sensor_init(void)
{
	gp2ap002_pdata.irq = gpio_to_irq(PROXI_INT_GPIO_PIN);
	i2c_register_board_info(KYLEPRO_SENSOR_I2C_BUS, kylepro_sensor_i2c_info,
				ARRAY_SIZE(kylepro_sensor_i2c_info));
}
