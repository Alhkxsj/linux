// SPDX-License-Identifier: GPL-2.0
/*
 * MT6895 GPUEB power handshake for mainline panthor.
 *
 * This is the minimal client needed by the Mali CSF driver: tell the GPUEB
 * firmware where the shared status/debug memory is, then ask it to release
 * the GPU for MCU boot.
 */

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/seq_file.h>
#include <linux/soc/mediatek/mt6895_gpueb.h>
#include <linux/soc/mediatek/mtk_tinysys_ipi.h>

#include "gpueb_ipi.h"
#include "gpueb_reserved_mem.h"
#include "gpufreq_ipi.h"

/*
 * Frequency commits are sent in POLLING mode, and that path takes pin_lock with
 * spin_lock_irqsave() and then busy-waits for the coprocessor's ack until the
 * caller's timeout expires.  IPI_TIMEOUT_MS is 10 s, which is the right bound
 * for the init and power-control commands but catastrophic here: when the EB
 * stops acking mid-storm (simple_ondemand can issue ~20 commits/s on a bursty
 * load) one CPU sits with interrupts disabled for the full ten seconds.  That
 * was captured live -- "IPI 1 POLLING kept IRQs off 10000010 us", GPUEB frozen,
 * panthor abandoning the stack domain, display wedged.
 *
 * A healthy commit acks in ~1.2 ms (ipi_poll_max_us), so 20 ms leaves an order
 * of magnitude of headroom while making the worst-case IRQ-off window
 * negligible.  A commit that misses this deadline simply fails: the GPU keeps
 * its current OPP and the next poll retries.
 */
#define GPUEB_COMMIT_TIMEOUT_MS		20U

static DEFINE_MUTEX(gpueb_power_lock);
static struct gpufreq_ipi_data gpueb_power_recv_msg;
static int gpueb_power_channel = -1;
static bool gpueb_power_registered;

/*
 * Debug view straight into the EB's shared status page: whatever the
 * firmware believes the current OPP indexes, frequencies, voltages and
 * limiters are. Field order mirrors downstream's struct
 * gpufreq_shared_status.
 */
static int gpueb_gpufreq_status_show(struct seq_file *s, void *unused)
{
	static const char * const names[] = {
		"cur_oppidx_gpu", "cur_oppidx_stack", "opp_num_gpu",
		"opp_num_stack", "cur_fgpu", "cur_fstack", "cur_vgpu",
		"cur_vstack", "cur_vsram_gpu", "cur_vsram_stack",
		"cur_power_gpu", "cur_power_stack", "max_power_gpu",
		"max_power_stack", "min_power_gpu", "min_power_stack",
		"cur_ceiling_gpu", "cur_floor_gpu", "cur_ceiling_stack",
		"cur_floor_stack", "cur_c_limiter_gpu", "cur_f_limiter_gpu",
		"cur_c_limiter_stack", "cur_f_limiter_stack", "power_control",
		"dvfs_state", "shader_present", "power_count", "aging_enable",
		"avs_enable", "sb_version", "ptp_version",
	};
	phys_addr_t va = gpueb_get_reserve_mem_virt_by_name("MEM_ID_GPUFREQ");
	u32 *p = (u32 *)(uintptr_t)va;
	unsigned int i;

	if (!va || IS_ERR_VALUE(va)) {
		seq_puts(s, "MEM_ID_GPUFREQ not available\n");
		return 0;
	}

	BUILD_BUG_ON(ARRAY_SIZE(names) != 32);
	for (i = 0; i < ARRAY_SIZE(names); i++)
		seq_printf(s, "%-20s %11d (0x%08x)\n", names[i],
			   (int)p[i], p[i]);

	seq_puts(s, "-- raw --\n");
	for (i = 0; i < 64; i++)
		seq_printf(s, "%08x%c", p[i], (i % 8 == 7) ? '\n' : ' ');

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gpueb_gpufreq_status);

/*
 * Manual OPP commit knob, for bring-up validation only:
 *   echo "<target> <oppidx>" > /sys/kernel/debug/gpueb_gpufreq_commit
 * target 1 = TARGET_GPU, 2 = TARGET_STACK; oppidx is an index into the EB's
 * working table (see opp_num_gpu / opp_num_stack in gpueb_gpufreq_status).
 * Out-of-range indexes are what pinned this SoC to its lowest OPP before, so
 * bound them here rather than trusting the caller.
 */
static ssize_t gpueb_gpufreq_commit_write(struct file *file,
					  const char __user *ubuf,
					  size_t count, loff_t *ppos)
{
	unsigned int target, oppidx;
	char buf[32];
	int ret;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	if (sscanf(buf, "%u %u", &target, &oppidx) != 2)
		return -EINVAL;
	if (target != TARGET_GPU && target != TARGET_STACK)
		return -EINVAL;

	ret = mt6895_gpueb_commit(target, oppidx);
	if (ret)
		return ret;

	return count;
}

static const struct file_operations gpueb_gpufreq_commit_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.write	= gpueb_gpufreq_commit_write,
};

static int gpueb_power_init_locked(void)
{
	struct gpufreq_ipi_data msg = {};
	phys_addr_t shared_mem_pa;
	phys_addr_t shared_mem_size;
	int ret;

	if (gpueb_power_registered)
		return 0;

	if (!gpueb_ipidev.ipi_inited)
		return -EPROBE_DEFER;

	gpueb_power_channel = gpueb_get_send_PIN_ID_by_name("IPI_ID_GPUFREQ");
	if (gpueb_power_channel < 0)
		return -ENODEV;

	ret = mtk_ipi_register(&gpueb_ipidev, gpueb_power_channel, NULL, NULL,
			       &gpueb_power_recv_msg);
	if (ret != IPI_ACTION_DONE && ret != IPI_DUPLEX)
		return -EIO;

	shared_mem_pa = gpueb_get_reserve_mem_phys_by_name("MEM_ID_GPUFREQ");
	shared_mem_size = gpueb_get_reserve_mem_size_by_name("MEM_ID_GPUFREQ");
	if (!shared_mem_pa || !shared_mem_size)
		return -ENOMEM;

	msg.cmd_id = CMD_INIT_SHARED_MEM;
	msg.u.addr.status_base = shared_mem_pa;
	msg.u.addr.status_size = GPUFREQ_STATUS_MEM_SZ;
	msg.u.addr.debug_base = shared_mem_pa + GPUFREQ_STATUS_MEM_SZ;
	msg.u.addr.debug_size = shared_mem_size - GPUFREQ_STATUS_MEM_SZ;

	ret = mtk_ipi_send_compl(&gpueb_ipidev, gpueb_power_channel,
				 IPI_SEND_POLLING, &msg,
				 GPUFREQ_IPI_DATA_LEN, IPI_TIMEOUT_MS);
	if (ret != IPI_ACTION_DONE)
		return -EIO;

	gpueb_power_registered = true;
	debugfs_create_file("gpueb_gpufreq_status", 0444, NULL, NULL,
			    &gpueb_gpufreq_status_fops);
	debugfs_create_file("gpueb_gpufreq_commit", 0200, NULL, NULL,
			    &gpueb_gpufreq_commit_fops);
	pr_info("XAGA-GPUEB: CMD_INIT_SHARED_MEM done\n");
	return 0;
}



static int gpueb_power_commit_locked(int target, int oppidx)
{
	struct gpufreq_ipi_data msg = {};
	static unsigned int commit_count;
	int ret;

	msg.cmd_id = CMD_COMMIT;
	msg.target = target;
	msg.u.oppidx = oppidx;

	ret = mtk_ipi_send_compl(&gpueb_ipidev, gpueb_power_channel,
				 IPI_SEND_POLLING, &msg,
				 GPUFREQ_IPI_DATA_LEN, GPUEB_COMMIT_TIMEOUT_MS);
	if (ret != IPI_ACTION_DONE)
		return -EIO;

	if (commit_count++ < 64)
		pr_info("XAGA-GPUEB: COMMIT target=%u idx=%d transport=%d ack=%d\n",
			target, oppidx, ret, gpueb_power_recv_msg.u.return_value);

	if (gpueb_power_recv_msg.u.return_value < 0)
		return gpueb_power_recv_msg.u.return_value;

	return 0;
}


bool mt6895_gpueb_available(void)
{
	struct device_node *np;
	bool available;

	np = of_find_compatible_node(NULL, NULL, "mediatek,gpueb");
	available = np != NULL;
	of_node_put(np);

	return available;
}
EXPORT_SYMBOL_GPL(mt6895_gpueb_available);

int mt6895_gpueb_power_control(unsigned int power_on)
{
	struct gpufreq_ipi_data msg = {};
	int ret;

	mutex_lock(&gpueb_power_lock);

	ret = gpueb_power_init_locked();
	if (ret)
		goto out;

	msg.cmd_id = CMD_POWER_CONTROL;
	msg.u.power_state = power_on ? POWER_ON : POWER_OFF;

	ret = mtk_ipi_send_compl(&gpueb_ipidev, gpueb_power_channel,
				 IPI_SEND_POLLING, &msg,
				 GPUFREQ_IPI_DATA_LEN, IPI_TIMEOUT_MS);
	if (ret != IPI_ACTION_DONE) {
		ret = -EIO;
		goto out;
	}

	if (gpueb_power_recv_msg.u.return_value < 0)
		ret = gpueb_power_recv_msg.u.return_value;
	else
		ret = 0;

	/* Downstream gpufreq_v2 sends ONLY CMD_POWER_CONTROL here when the EB
	 * owns DVFS; the firmware restores its own boot OPP internally. Never
	 * commit OPP indexes from this path -- they are working-table indexes
	 * and belong to the DVFS client via mt6895_gpueb_commit().
	 */
	pr_info("XAGA-GPUEB: power_control(%u) ret=%d\n", power_on, ret);

out:
	mutex_unlock(&gpueb_power_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6895_gpueb_power_control);

int mt6895_gpueb_power_on(void)
{
	return mt6895_gpueb_power_control(true);
}
EXPORT_SYMBOL_GPL(mt6895_gpueb_power_on);

int mt6895_gpueb_commit(unsigned int target, unsigned int oppidx)
{
	int ret;

	mutex_lock(&gpueb_power_lock);

	ret = gpueb_power_init_locked();
	if (!ret)
		ret = gpueb_power_commit_locked(target, oppidx);

	mutex_unlock(&gpueb_power_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6895_gpueb_commit);
