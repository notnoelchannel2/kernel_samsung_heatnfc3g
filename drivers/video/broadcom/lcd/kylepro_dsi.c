/*******************************************************************************
* Copyright 2011 Broadcom Corporation.	All rights reserved.
*
* @file drivers/video/broadcom/lcd/kylepro_dsi.c
*
* Panel helper for the Samsung kylepro (GT-S7580): panel ID detection,
* lcd_type sysfs and polling ESD recovery. Ported from the kylepro 3.4
* nt35510_dsi.c to the 3.10 kona_fb interface (see heatnfc_dsi.c).
*
* kylepro ships with one of two command-mode DSI panels:
*   ILI9806  ID 55 BC D1 (D0 accepted)  first  kona-fb node  backlight_check=1
*   NT35510  ID 55 BC C0                second kona-fb node  backlight_check=2
*
* Unless you and Broadcom execute a separate written software license agreement
* governing use of this software, this software is licensed to you under the
* terms of the GNU General Public License version 2, available at
* http://www.gnu.org/copyleft/gpl.html (the "GPL").
*
* Notwithstanding the above, under no circumstances may you combine this
* software in any way with any other Broadcom software provided under a license
* other than the GPL, without Broadcom's express prior written consent.
*******************************************************************************/

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/lcd.h>
#include <linux/mutex.h>
#include <linux/fb.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <video/kona_fb.h>
#include <linux/broadcom/mobcom_types.h>
#include "dispdrv_common.h"

#define ESD_OPERATION
#define ESD_PERIOD_MS		3000		/* poll period */
#define ESD_BOOT_WAIT_MS	(1000 * 600)	/* first check 10 min after boot */

extern void panel_write(UInt8 *buff);
extern int panel_read(UInt8 reg, UInt8 *rxBuff, UInt8 buffLen);
extern void kona_fb_esd_hw_reset(void);
extern int kona_fb_obtain_lock(void);
extern int kona_fb_release_lock(void);

#ifdef CONFIG_LCD_CLASS_DEVICE
extern struct class *lcd_class;
struct device *lcd_dev;
#endif

#ifdef CONFIG_FB_BRCM_LCD_EXIST_CHECK
extern int lcd_exist;
#endif

struct kylepro_dsi_lcd {
	struct device		*dev;
	struct mutex		lock;
#ifdef ESD_OPERATION
	unsigned int		esd_enable;
	struct delayed_work	esd_work;
#endif
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend	earlysuspend;
#endif
};

static struct kylepro_dsi_lcd *lcd;
static u8 gPanelID[3];
int id1, id2, id3;

/* 1 = ILI9806, 2 = NT35510, <= 0 = unknown. Selects the brightness curve. */
int backlight_check;
EXPORT_SYMBOL(backlight_check);

static int kylepro_id_to_panel(void)
{
	if (id1 != 0x55 || id2 != 0xBC)
		return -1;
	if (id3 == 0xD1 || id3 == 0xD0)
		return LCD_PANEL_ID_ONE;	/* ILI9806 */
	if (id3 == 0xC0)
		return LCD_PANEL_ID_TWO;	/* NT35510 */
	return -1;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * Called from kona_fb's "lcd_panel=" handler. Accepts either the panel name
 * ("ILI9806"/"NT35510", which is what the kylepro bootloader is believed to
 * pass) or a 6-digit hex panel ID ("55bcc0"). Returns the kona-fb node index
 * (1 or 2), or 0 to let kona_fb detect the panel at probe time.
 */
int sBoot_panel_read_id(char *boot_id)
{
	int i, v[6];

	if (!boot_id || boot_id[0] == (char)0xFF)
		return 0;

	if (!strncmp(boot_id, "ILI9806", 7)) {
		backlight_check = LCD_PANEL_ID_ONE;
		return LCD_PANEL_ID_ONE;
	}
	if (!strncmp(boot_id, "NT35510", 7)) {
		backlight_check = LCD_PANEL_ID_TWO;
		return LCD_PANEL_ID_TWO;
	}

	for (i = 0; i < 6; i++) {
		v[i] = hexval(boot_id[i]);
		if (v[i] < 0)
			return 0;
	}
	id1 = v[0] * 16 + v[1];
	id2 = v[2] * 16 + v[3];
	id3 = v[4] * 16 + v[5];
	pr_info("[LCD] from bootloader id1=%x id2=%x id3=%x\n", id1, id2, id3);

	i = kylepro_id_to_panel();
	if (i > 0) {
		backlight_check = i;
		return i;
	}
	return 0;
}
EXPORT_SYMBOL(sBoot_panel_read_id);

/*
 * Read the panel ID over DSI (LP mode, as the 3.4 kernel did) and return
 * LCD_PANEL_ID_ONE/TWO if it matches the panel named @panel_name, else -1.
 */
int panel_read_id(char *panel_name)
{
	int ret, want;

	want = (panel_name && !strcmp(panel_name, "NT35510")) ?
		LCD_PANEL_ID_TWO : LCD_PANEL_ID_ONE;

	kona_fb_obtain_lock();
	ret = panel_read(0xDA, gPanelID, 1);
	if (ret < 0)
		goto out;
	id1 = gPanelID[0];
	ret = panel_read(0xDB, gPanelID, 1);
	if (ret < 0)
		goto out;
	id2 = gPanelID[0];
	ret = panel_read(0xDC, gPanelID, 1);
	if (ret < 0)
		goto out;
	id3 = gPanelID[0];

	pr_info("[LCD] %s: id1=%x id2=%x id3=%x\n", panel_name, id1, id2, id3);
	ret = kylepro_id_to_panel();
	if (ret != want)
		ret = -1;
out:
	kona_fb_release_lock();
	if (ret > 0)
		backlight_check = ret;
	pr_info("[LCD] %s(%s) = %d\n", __func__, panel_name, ret);
	return ret;
}
EXPORT_SYMBOL(panel_read_id);

#ifdef CONFIG_LCD_CLASS_DEVICE
static ssize_t show_lcd_info(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	if (id3 == 0xD1 || id3 == 0xD0)
		return sprintf(buf, "INH_55BC%02X\n", id3);
	if (id3 == 0xC0)
		return sprintf(buf, "INH_55BCC0\n");
	return sprintf(buf, "unknown\n");
}
static DEVICE_ATTR(lcd_type, 0444, show_lcd_info, NULL);
#endif

#ifdef ESD_OPERATION
static int ili9806_esd_reset;

int kylepro_esd_reset_get(void)
{
	return ili9806_esd_reset;
}
EXPORT_SYMBOL(kylepro_esd_reset_get);

static u8 mauc0_cmd[] = { 6, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00, 0x00 };
static u8 mauc1_cmd[] = { 6, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01, 0x00 };
static u8 ili_bch_cmd[] = { 21, 0xBC, 0x01, 0x12, 0x61, 0xff, 0x10, 0x10,
	0x0b, 0x13, 0x32, 0x73, 0xff, 0xff, 0x0e, 0x0e, 0x00, 0x03, 0x66,
	0x63, 0x01, 0x00, 0x00 };

static bool ili_ref_valid;
static u8 ili_ref_09h[4];
static u8 ili_ref_bch[20];

/* Returns 0 if the panel looks healthy, -1 if it needs a reset. */
static int kylepro_panel_check(void)
{
	int ret = 0;

	kona_fb_obtain_lock();
	if (backlight_check == LCD_PANEL_ID_TWO) {
		/* NT35510: IC vendor code on page 1, must differ on page 0 */
		u8 p1 = 0, p0 = 0;

		panel_write(mauc1_cmd);
		panel_read(0xC5, &p1, 1);
		if (p1 != 0x55) {
			pr_err("[LCD] ESD: 0xC5 page1 = 0x%02x\n", p1);
			ret = -1;
			goto out;
		}
		panel_write(mauc0_cmd);
		panel_read(0xC5, &p0, 1);
		if (p0 == p1) {
			pr_err("[LCD] ESD: 0xC5 page0 = 0x%02x\n", p0);
			ret = -1;
		}
	} else if (backlight_check == LCD_PANEL_ID_ONE) {
		/* ILI9806: status (09h) and BCh must stay as first read */
		u8 r09[5], rbc[21];

		if (panel_read(0x09, r09, 5) < 0 ||
		    panel_read(0xBC, rbc, 21) < 0) {
			ret = -1;
			goto out;
		}
		if (!ili_ref_valid) {
			memcpy(ili_ref_09h, &r09[1], sizeof(ili_ref_09h));
			memcpy(ili_ref_bch, &rbc[1], sizeof(ili_ref_bch));
			ili_ref_valid = true;
		}
		if (memcmp(ili_ref_09h, &r09[1], sizeof(ili_ref_09h))) {
			pr_err("[LCD] ESD: 09h changed\n");
			ret = -1;
		} else if (memcmp(ili_ref_bch, &rbc[1], sizeof(ili_ref_bch))) {
			pr_err("[LCD] ESD: BCh changed\n");
			panel_write(ili_bch_cmd);
			msleep(10);
			ret = -1;
		}
	}
out:
	kona_fb_release_lock();
	return ret;
}

static void esd_work_func(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	int tries = 0;

	mutex_lock(&lcd->lock);
	if (lcd->esd_enable) {
		while (kylepro_panel_check() && tries++ < 5) {
			ili9806_esd_reset = 1;
			kona_fb_esd_hw_reset();
			ili9806_esd_reset = 0;
			msleep(100);
		}
		schedule_delayed_work(dwork, msecs_to_jiffies(ESD_PERIOD_MS));
	}
	mutex_unlock(&lcd->lock);
}
#else
int kylepro_esd_reset_get(void) { return 0; }
EXPORT_SYMBOL(kylepro_esd_reset_get);
#endif /* ESD_OPERATION */

#ifdef CONFIG_HAS_EARLYSUSPEND
static void kylepro_dsi_early_suspend(struct early_suspend *h)
{
	struct kylepro_dsi_lcd *l =
		container_of(h, struct kylepro_dsi_lcd, earlysuspend);
#ifdef ESD_OPERATION
	l->esd_enable = 0;
	cancel_delayed_work_sync(&l->esd_work);
#endif
}

static void kylepro_dsi_late_resume(struct early_suspend *h)
{
	struct kylepro_dsi_lcd *l =
		container_of(h, struct kylepro_dsi_lcd, earlysuspend);
#ifdef ESD_OPERATION
#ifdef CONFIG_FB_BRCM_LCD_EXIST_CHECK
	if (!lcd_exist)
		return;
#endif
	l->esd_enable = 1;
	schedule_delayed_work(&l->esd_work, msecs_to_jiffies(ESD_PERIOD_MS));
#endif
}
#endif

static int kylepro_panel_probe(struct platform_device *pdev)
{
	lcd = kzalloc(sizeof(*lcd), GFP_KERNEL);
	if (!lcd)
		return -ENOMEM;

	lcd->dev = &pdev->dev;
	platform_set_drvdata(pdev, lcd);
	mutex_init(&lcd->lock);

#ifdef CONFIG_LCD_CLASS_DEVICE
	lcd_dev = device_create(lcd_class, NULL, 0, NULL, "panel");
	if (IS_ERR(lcd_dev))
		pr_err("[LCD] failed to create lcd class device\n");
	else if (device_create_file(lcd_dev, &dev_attr_lcd_type))
		pr_err("[LCD] failed to create lcd_type\n");
#endif

#ifdef ESD_OPERATION
	INIT_DELAYED_WORK(&lcd->esd_work, esd_work_func);
#ifdef CONFIG_FB_BRCM_LCD_EXIST_CHECK
	if (lcd_exist)
#endif
	{
		lcd->esd_enable = 1;
		schedule_delayed_work(&lcd->esd_work,
				      msecs_to_jiffies(ESD_BOOT_WAIT_MS));
	}
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
	lcd->earlysuspend.level = EARLY_SUSPEND_LEVEL_DISABLE_FB - 1;
	lcd->earlysuspend.suspend = kylepro_dsi_early_suspend;
	lcd->earlysuspend.resume = kylepro_dsi_late_resume;
	register_early_suspend(&lcd->earlysuspend);
#endif
	pr_info("[LCD] kylepro panel helper: backlight_check=%d\n",
		backlight_check);
	return 0;
}

static int kylepro_panel_remove(struct platform_device *pdev)
{
	struct kylepro_dsi_lcd *l = platform_get_drvdata(pdev);

#ifdef ESD_OPERATION
	cancel_delayed_work_sync(&l->esd_work);
#endif
#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&l->earlysuspend);
#endif
	kfree(l);
	return 0;
}

static struct platform_driver kylepro_panel_driver = {
	.driver = {
		.name = "kylepro-panel",
		.owner = THIS_MODULE,
	},
	.probe = kylepro_panel_probe,
	.remove = kylepro_panel_remove,
};

static int __init kylepro_panel_init(void)
{
	return platform_driver_register(&kylepro_panel_driver);
}

static void __exit kylepro_panel_exit(void)
{
	platform_driver_unregister(&kylepro_panel_driver);
}

late_initcall_sync(kylepro_panel_init);
module_exit(kylepro_panel_exit);

MODULE_DESCRIPTION("kylepro panel control driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:kylepro-panel");
