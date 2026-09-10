// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek LCD Backlight Driver
 */

#include <linux/backlight.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

extern int mtkfb_set_backlight_level(unsigned int level);

/*
 * Lowest level handed to the panel while the display is meant to be on.
 * The NT36672C takes DCS 0x51 == 0 literally and also gets 0x53 0x0C
 * (dimming/BL off) from jdi_setbacklight_cmdq(), i.e. a fully black screen
 * with no on-screen way back. Desktop brightness sliders happily send 0 for
 * "0%", so clamp an explicitly-requested zero up to the dimmest visible
 * step instead. Deliberate blanking (DPMS, suspend, fbcon blank) still
 * reaches level 0 through the props.power / props.state path below.
 */
#define MTK_LCD_BL_MIN_ON	1

static int mtk_lcd_bl_update_status(struct backlight_device *bd)
{
	int brightness = bd->props.brightness;

	if (bd->props.power != FB_BLANK_UNBLANK ||
	    bd->props.state & (BL_CORE_SUSPENDED | BL_CORE_FBBLANK))
		brightness = 0;
	else if (brightness < MTK_LCD_BL_MIN_ON)
		brightness = MTK_LCD_BL_MIN_ON;

	return mtkfb_set_backlight_level(brightness);
}

static const struct backlight_ops mtk_lcd_bl_ops = {
	.update_status = mtk_lcd_bl_update_status,
};

static int mtk_lcd_bl_probe(struct platform_device *pdev)
{
	struct backlight_properties props;
	struct backlight_device *bd;
	int ret;

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = 4095;
	props.brightness = 2047;
	props.power = FB_BLANK_UNBLANK;

	bd = devm_backlight_device_register(&pdev->dev, "lcd-backlight",
					    &pdev->dev, NULL,
					    &mtk_lcd_bl_ops, &props);
	if (IS_ERR(bd))
		return PTR_ERR(bd);

	platform_set_drvdata(pdev, bd);

	mutex_lock(&bd->ops_lock);
	if (bd->ops && bd->ops->update_status) {
		ret = bd->ops->update_status(bd);
		mutex_unlock(&bd->ops_lock);
		if (ret)
			dev_info(&pdev->dev, "initial backlight update deferred: %d\n", ret);
	} else {
		mutex_unlock(&bd->ops_lock);
		dev_info(&pdev->dev, "initial backlight update deferred: %d\n", -2);
	}

	dev_info(&pdev->dev, "qqcandy lcd-backlight registered (max=%u, brightness=%u)\n",
		 bd->props.max_brightness, bd->props.brightness);

	return 0;
}

static const struct of_device_id mtk_lcd_bl_of_match[] = {
	{ .compatible = "mediatek,lcd-backlight" },
	{ .compatible = "mediatek,disp-leds" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_lcd_bl_of_match);

static struct platform_driver mtk_lcd_bl_driver = {
	.probe = mtk_lcd_bl_probe,
	.driver = {
		.name = "mtk-lcd-backlight",
		.of_match_table = mtk_lcd_bl_of_match,
	},
};
module_platform_driver(mtk_lcd_bl_driver);

MODULE_AUTHOR("MediaTek Inc.");
MODULE_DESCRIPTION("MediaTek LCD Backlight Driver");
MODULE_LICENSE("GPL");
