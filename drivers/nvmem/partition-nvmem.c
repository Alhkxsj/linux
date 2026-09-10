// SPDX-License-Identifier: GPL-2.0
/*
 * nvmem provider backed by a raw storage partition.
 *
 * Some vendor platforms keep per-unit data -- MAC addresses, calibration -- in a
 * dedicated partition rather than in fuses or in a bootloader-patched DT. On
 * MT6895 phones the factory addresses live in `proinfo` at fixed offsets
 * (Bluetooth at 0x68, WiFi at 0x6e), mirrored in the ext4 `nvdata` partition
 * that the downstream nvram_daemon reads from userspace.
 *
 * Describing that in DT keeps the values out of the kernel and out of the board
 * file: the DT says *where* the data is, this driver reads it at runtime, and
 * consumers pick it up through the ordinary nvmem cell bindings, e.g.
 * of_get_mac_address() for a netdev.
 *
 *	proinfo_nvmem: nvmem {
 *		compatible = "mediatek,partition-nvmem";
 *		partition-label = "proinfo";
 *		#address-cells = <1>;
 *		#size-cells = <1>;
 *
 *		wifi_mac: mac@6e { reg = <0x6e 6>; };
 *		bt_mac:   mac@68 { reg = <0x68 6>; };
 *	};
 *
 * The partition is opened lazily on the first cell read, not at probe: partition
 * device nodes and their by-label symlinks only appear once storage has been
 * scanned, which is well after this driver binds. Consumers that need a value
 * during their own probe therefore have to tolerate -EPROBE_DEFER or read the
 * cell later.
 */
#include <linux/blkdev.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

/* Enough for the descriptor blocks these partitions keep at the front. */
#define PART_NVMEM_DEFAULT_SIZE		4096

struct part_nvmem {
	struct device *dev;
	const char *label;
	u32 size;
	struct mutex lock;
	void *cache;		/* whole window, filled on first successful read */
};

static const char * const part_nvmem_dirs[] = {
	"/dev/disk/by-partlabel/",
	"/dev/block/by-name/",
};

static int part_nvmem_fill_cache(struct part_nvmem *p)
{
	unsigned int i;
	int ret = -ENODEV;

	if (p->cache)
		return 0;

	for (i = 0; i < ARRAY_SIZE(part_nvmem_dirs); i++) {
		char *path = kasprintf(GFP_KERNEL, "%s%s", part_nvmem_dirs[i],
				       p->label);
		struct file *f;
		void *buf;
		loff_t pos = 0;

		if (!path)
			return -ENOMEM;

		f = bdev_file_open_by_path(path, BLK_OPEN_READ, p, NULL);
		if (IS_ERR(f)) {
			dev_dbg(p->dev, "%s: %pe\n", path, f);
			kfree(path);
			continue;
		}

		buf = kzalloc(p->size, GFP_KERNEL);
		if (!buf) {
			fput(f);
			kfree(path);
			return -ENOMEM;
		}

		ret = kernel_read(f, buf, p->size, &pos);
		fput(f);
		if (ret == p->size) {
			dev_info(p->dev, "read %u bytes from %s\n", p->size,
				 path);
			p->cache = buf;
			kfree(path);
			return 0;
		}

		dev_warn(p->dev, "short read from %s: %d\n", path, ret);
		kfree(buf);
		kfree(path);
		ret = ret < 0 ? ret : -EIO;
	}

	return ret;
}

static int part_nvmem_read(void *priv, unsigned int offset, void *val,
			   size_t bytes)
{
	struct part_nvmem *p = priv;
	int ret;

	if (offset + bytes > p->size)
		return -EINVAL;

	guard(mutex)(&p->lock);

	ret = part_nvmem_fill_cache(p);
	if (ret)
		return ret;

	memcpy(val, p->cache + offset, bytes);

	return 0;
}

static int part_nvmem_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nvmem_config cfg = { };
	struct part_nvmem *p;
	int ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	ret = of_property_read_string(dev->of_node, "partition-label",
				      &p->label);
	if (ret)
		return dev_err_probe(dev, ret, "missing partition-label\n");

	p->dev = dev;
	p->size = PART_NVMEM_DEFAULT_SIZE;
	of_property_read_u32(dev->of_node, "size", &p->size);
	ret = devm_mutex_init(dev, &p->lock);
	if (ret)
		return ret;

	cfg.dev = dev;
	cfg.name = p->label;
	cfg.priv = p;
	cfg.size = p->size;
	cfg.word_size = 1;
	cfg.stride = 1;
	cfg.read_only = true;
	cfg.reg_read = part_nvmem_read;
	cfg.add_legacy_fixed_of_cells = true;

	return PTR_ERR_OR_ZERO(devm_nvmem_register(dev, &cfg));
}

static const struct of_device_id part_nvmem_of_match[] = {
	{ .compatible = "mediatek,partition-nvmem" },
	{ }
};
MODULE_DEVICE_TABLE(of, part_nvmem_of_match);

static struct platform_driver part_nvmem_driver = {
	.probe = part_nvmem_probe,
	.driver = {
		.name = "partition-nvmem",
		.of_match_table = part_nvmem_of_match,
	},
};
module_platform_driver(part_nvmem_driver);

MODULE_DESCRIPTION("nvmem provider backed by a raw storage partition");
MODULE_LICENSE("GPL");
