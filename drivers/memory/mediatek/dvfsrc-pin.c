// SPDX-License-Identifier: GPL-2.0
/*
 * Manual DDR-OPP pinning for the MT6895 DVFSRC.
 *
 * The DVFSRC aggregates DRAM bandwidth requests from many clients and
 * picks an operating point.  Nothing in this port votes yet, so DRAM
 * stays at whatever level LK left behind.  Until the interconnect /
 * bandwidth-vote stack is ported, expose the raw SW_REQ dram field so an
 * operating point can be pinned by hand.
 *
 * Register layout matches the downstream mt6983 dvfsrc data (MT6895
 * shares that generation):
 *
 *   DVFSRC_SW_REQ (0x18): bits [15:12] dram level, [6:4] vcore level
 *   DVFSRC_LEVEL  (0x5f0): currently applied level (low 6 bits)
 *
 * The block must be initialized through the EL3 VCOREFS service before it
 * services requests; probe issues MTK_SIP_DVFSRC_INIT and reports whether
 * the running firmware supports it.
 *
 * Level 0 is pinned at probe, matching the xaga port: without a voter the DRAM
 * sits at whatever LK handed off (3200 Mbps measured here) while the stock OS
 * scales to 6400 Mbps under load, and that shows up as UI smoothness. Pinning
 * was held back during the random hard-reset hunt so boot behaviour stayed
 * bit-identical; that root cause is fixed (HANDOFF §18.19 §36), so the pin is
 * on by default now. boot_level=0xff restores the old hands-off behaviour.
 *
 * Note the pin transiently visits the low band during the unlock sequence
 * below, i.e. DRAM briefly drops to 800 Mbps at probe.
 */
#include <linux/arm-smccc.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>

#include <soc/mediatek/dramc.h>

#define SW_REQ			0x18
#define LEVEL			0x5f0

#define SW_REQ_DRAM_SHIFT	12
#define SW_REQ_DRAM_MASK	(0xf << SW_REQ_DRAM_SHIFT)

#define MTK_SIP_DVFSRC_INIT	0x00

#define BOOT_LEVEL_NONE		0xff

static unsigned int boot_level;
module_param(boot_level, uint, 0644);
MODULE_PARM_DESC(boot_level,
		 "DRAM level to pin at probe (0..15, 0 = fastest); "
		 "0xff leaves the LK hand-off level untouched");

static bool allow_low_band;
module_param(allow_low_band, bool, 0644);
MODULE_PARM_DESC(allow_low_band,
		 "Permit pinning low-band levels (9..15) -- parks DRAM at "
		 "800 Mbps and will wedge an active GPU");

struct mtk_dvfsrc_pin {
	void __iomem *base;
	bool fw_ready;
	bool unlocked;
	unsigned int dram_type;
};

static u32 xpin_read(struct mtk_dvfsrc_pin *d, u32 off)
{
	return readl(d->base + off);
}

/*
 * Write only the dram-level nibble of SW_REQ; the vcore field and the
 * other requesters' bits stay untouched.
 */
static void xpin_set_level(struct mtk_dvfsrc_pin *d, u32 lvl)
{
	u32 val = xpin_read(d, SW_REQ) & ~SW_REQ_DRAM_MASK;

	writel(val | ((lvl & 0xf) << SW_REQ_DRAM_SHIFT), d->base + SW_REQ);
}

/*
 * Bring-up quirk observed on this DVFSRC generation: the firmware ignores
 * a direct high-band request made from the boot state.  Visiting the low
 * band once makes subsequent high-band requests latch -- empirically
 * 0 -> 8 -> 9 -> N ends up pinned at N.  DDR shuffles are slow, so settle
 * between steps.  Only needed once per boot.
 *
 * This transiently parks DRAM in the low band, which is why it is not run
 * unless a pin is actually requested.
 */
static void xpin_unlock_once(struct mtk_dvfsrc_pin *d)
{
	static const u32 seq[] = { 0, 8, 9 };
	unsigned int i;

	if (d->unlocked)
		return;

	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		xpin_set_level(d, seq[i]);
		msleep(100);
	}
	d->unlocked = true;
}

static int xpin_pin(struct mtk_dvfsrc_pin *d, u32 lvl)
{
	unsigned int before, after;

	if (!d->fw_ready)
		return -EOPNOTSUPP;
	if (lvl > 0xf)
		return -EINVAL;
	/*
	 * The low band (9..15) parks DRAM at 800 Mbps; with an active GPU
	 * that wedges jobs hard.  Require an explicit opt-in.
	 */
	if (lvl >= 9 && !allow_low_band)
		return -EINVAL;

	before = mtk_dramc_get_data_rate();
	xpin_unlock_once(d);
	xpin_set_level(d, lvl);
	msleep(100);
	after = mtk_dramc_get_data_rate();

	pr_info("mt6895-dvfsrc-pin: level %u: %u -> %u Mbps\n",
		lvl, before, after);

	return 0;
}

static ssize_t dram_level_raw_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct mtk_dvfsrc_pin *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n",
			  (xpin_read(d, SW_REQ) & SW_REQ_DRAM_MASK) >>
			  SW_REQ_DRAM_SHIFT);
}

static ssize_t dram_level_raw_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct mtk_dvfsrc_pin *d = dev_get_drvdata(dev);
	u32 val;
	int ret;

	ret = kstrtou32(buf, 0, &val);
	if (ret)
		return ret;

	ret = xpin_pin(d, val);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(dram_level_raw);

static ssize_t level_applied_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct mtk_dvfsrc_pin *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%08x\n", xpin_read(d, LEVEL));
}
static DEVICE_ATTR_RO(level_applied);

static ssize_t sw_req_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct mtk_dvfsrc_pin *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%08x\n", xpin_read(d, SW_REQ));
}
static DEVICE_ATTR_RO(sw_req);

static ssize_t fw_ready_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct mtk_dvfsrc_pin *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", d->fw_ready);
}
static DEVICE_ATTR_RO(fw_ready);

static struct attribute *mtk_dvfsrc_pin_attrs[] = {
	&dev_attr_dram_level_raw.attr,
	&dev_attr_level_applied.attr,
	&dev_attr_sw_req.attr,
	&dev_attr_fw_ready.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mtk_dvfsrc_pin);

static int mtk_dvfsrc_pin_probe(struct platform_device *pdev)
{
	struct arm_smccc_res res;
	struct mtk_dvfsrc_pin *d;

	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(d->base))
		return PTR_ERR(d->base);

	dev_set_drvdata(&pdev->dev, d);

	/*
	 * Ask EL3 to bring up the DVFSRC firmware.  a0 == 0 means success;
	 * a1 then carries the detected DRAM type.
	 */
	arm_smccc_smc(MTK_SIP_DVFSRC_VCOREFS_CONTROL, MTK_SIP_DVFSRC_INIT,
		      0, 0, 0, 0, 0, 0, &res);
	if (res.a0 == 0) {
		d->fw_ready = true;
		d->dram_type = res.a1;
	} else {
		dev_warn(&pdev->dev,
			 "VCOREFS init not supported by EL3 (a0=%ld); DRAM stays at boot OPP\n",
			 res.a0);
	}

	dev_info(&pdev->dev,
		 "SW_REQ=0x%08x LEVEL=0x%08x fw_ready=%d dram_type=%u rate=%u Mbps\n",
		 xpin_read(d, SW_REQ), xpin_read(d, LEVEL),
		 d->fw_ready, d->dram_type, mtk_dramc_get_data_rate());

	if (boot_level != BOOT_LEVEL_NONE) {
		int ret = xpin_pin(d, boot_level);

		if (ret)
			dev_warn(&pdev->dev, "boot pin to %u failed: %d\n",
				 boot_level, ret);
	}

	return 0;
}

static const struct of_device_id mtk_dvfsrc_pin_of_match[] = {
	{ .compatible = "mediatek,mt6895-dvfsrc-pin" },
	{ }
};
MODULE_DEVICE_TABLE(of, mtk_dvfsrc_pin_of_match);

static struct platform_driver mtk_dvfsrc_pin_drv = {
	.probe = mtk_dvfsrc_pin_probe,
	.driver = {
		.name = "mt6895-dvfsrc-pin",
		.of_match_table = mtk_dvfsrc_pin_of_match,
		.dev_groups = mtk_dvfsrc_pin_groups,
	},
};
module_platform_driver(mtk_dvfsrc_pin_drv);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6895 DVFSRC manual DDR-OPP pinning");
