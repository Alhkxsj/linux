// SPDX-License-Identifier: GPL-2.0 or MIT
/* Copyright 2019 Collabora ltd. */

#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/devfreq_cooling.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>

#include <drm/drm_managed.h>

#include "panthor_devfreq.h"
#include "panthor_device.h"

#if IS_ENABLED(CONFIG_MTK_GPUEB)
#include <linux/of.h>
#include <linux/soc/mediatek/mt6895_gpueb.h>

/*
 * MT6895: the GPU has two frequency domains.
 *
 *   core  ("gpu")    - MFGPLL behind CLK_TOP_MFG_SEL_0_CK, driven by CCF here
 *   stack ("stacks") - MFGSCPLL behind MFG_SEL_1, owned entirely by the gpueb
 *
 * The stack domain is not reachable through CCF: mfgsc_ao_mfgscpll sits at
 * enable_count 0 while the PLL is demonstrably running, and its VSTACK rail
 * (MT6368 BUCK2) is re-programmed by the EB on every commit, together with
 * AVS trimming and VSRAM sequencing. Driving either of those from the kernel
 * would add a second writer to a live rail, so the only sane control path is
 * to hand the EB a working-table index and let it apply clock plus voltage.
 *
 * Without this the stack domain stays frozen at whatever index the EB picked
 * at boot (s27 = 385MHz), which measures as a flat ~28% loss under load
 * (1264 fps vs 1624 fps, vkcube 1440x1440, 1500 frames, this unit).
 *
 * Both domains have to go to the EB, because the EB owns both bucks: VSTACK
 * (MT6368 BUCK2) for the stack domain and VGPU for the core domain, plus the
 * VSRAM sequencing and AVS trimming for each. Committing only the stack index
 * and letting CCF raise the core clock -- which is what this file used to do --
 * leaves the core PLL climbing to 950MHz on whatever VGPU the EB last applied,
 * because the "mali" supply the OPP core sees here is a fixed 800mV stub
 * (mt6895.dtsi). Frequency without voltage held under real shader load is what
 * took the SoC down; see HANDOFF §18.19 §36.
 *
 * An earlier note in this file claimed TARGET_GPU commits were "silently
 * ignored by this firmware (they ack 0 but cur_oppidx_gpu never moves)". The
 * index spaces are what differ: the GPU working table has 6 entries while the
 * stack table has 40, so a stack-space index sent to TARGET_GPU is out of range
 * and gets dropped. Sending a GPU-space index works, which is exactly what the
 * xaga 6.18 branch does.
 *
 * With both domains committed the EB is the sole owner of these clocks and
 * rails, so nothing here calls CCF or a regulator: not at probe, not on scale.
 * Measured on this unit after the change: idle parks at gpu 350MHz/575mV and
 * stack 219MHz/487.5mV, under load they rise together to 880MHz/787.5mV and
 * 852MHz/687.5mV. Before it, the core PLL reached 950MHz while VGPU stayed at
 * the idle 575mV.
 */
#define MT6895_EB_TARGET_GPU	1	/* gpufreq_ipi.h TARGET_GPU */
#define MT6895_EB_TARGET_STACK	2	/* gpufreq_ipi.h TARGET_STACK */

/*
 * CORE working table (opp_num_gpu = 6), highest-first like the stack table.
 */
static const unsigned int mt6895_eb_gpu_freq[] = {
	950000000, 880000000, 800000000, 610000000, 430000000, 350000000,
};

/*
 * STACK working table, read live from this unit's EB shared page
 * (opp_num_stack = 40). Sorted highest-first, index 0 is the fastest.
 */
static const unsigned int mt6895_eb_stack_freq[] = {
	852000000, 845000000, 837000000, 830000000, 822000000, 815000000,
	807000000, 800000000, 773000000, 747000000, 721000000, 695000000,
	668000000, 642000000, 616000000, 590000000, 572000000, 555000000,
	538000000, 521000000, 504000000, 487000000, 470000000, 453000000,
	436000000, 419000000, 402000000, 385000000, 368000000, 351000000,
	334000000, 317000000, 304000000, 292000000, 280000000, 268000000,
	255000000, 243000000, 231000000, 219000000,
};

#define MT6895_EB_STACK_IDX_LOWEST	(ARRAY_SIZE(mt6895_eb_stack_freq) - 1)
#define MT6895_EB_GPU_IDX_LOWEST	(ARRAY_SIZE(mt6895_eb_gpu_freq) - 1)

/* Set once a commit fails: the EB is the only owner of these clocks, so stop
 * scaling rather than handing the core clock to CCF, which has no way to move
 * VGPU (the "mali" supply here is a fixed stub) and would run the GPU fast at
 * the low-OPP voltage -- the exact failure this path exists to avoid.
 */
static bool mt6895_eb_frozen;
static int mt6895_eb_stack_idx_cur = -1;
static int mt6895_eb_gpu_idx_cur = -1;

static bool mt6895_eb_mode(void)
{
	return of_machine_is_compatible("mediatek,mt6895") &&
	       mt6895_gpueb_available();
}

/* Nearest working-table entry at or below freq. */
static int mt6895_eb_stack_idx(unsigned long freq)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mt6895_eb_stack_freq); i++) {
		if (freq >= mt6895_eb_stack_freq[i])
			return i;
	}

	return MT6895_EB_STACK_IDX_LOWEST;
}

static int mt6895_eb_gpu_idx(unsigned long freq)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mt6895_eb_gpu_freq); i++) {
		if (freq >= mt6895_eb_gpu_freq[i])
			return i;
	}

	return MT6895_EB_GPU_IDX_LOWEST;
}

static int mt6895_eb_commit(struct panthor_device *ptdev, int target, int idx)
{
	int ret = mt6895_gpueb_commit(target, idx);

	if (ret)
		dev_warn(ptdev->base.dev,
			 "GPUEB commit target=%d idx=%d failed (%d), freezing GPU scaling\n",
			 target, idx, ret);

	return ret;
}

/*
 * Mirror the downstream ordering across the two domains: the core side leads on
 * the way up and trails on the way down, so neither rail is ever below what the
 * running clocks need. Indexes are highest-first, so a smaller index is a higher
 * frequency.
 *
 * Nothing here touches CCF or a regulator: each commit makes the EB apply that
 * domain's PLL and its buck together, which is the only sequencing that keeps
 * voltage ahead of frequency.
 */
static int mt6895_eb_scale(struct panthor_device *ptdev, unsigned long freq)
{
	int sidx = mt6895_eb_stack_idx(freq);
	int gidx = mt6895_eb_gpu_idx(freq);
	bool up = mt6895_eb_gpu_idx_cur < 0 || gidx < mt6895_eb_gpu_idx_cur;
	int first = up ? MT6895_EB_TARGET_GPU : MT6895_EB_TARGET_STACK;
	int second = up ? MT6895_EB_TARGET_STACK : MT6895_EB_TARGET_GPU;
	int ret;

	if (mt6895_eb_frozen)
		return 0;

	if (sidx == mt6895_eb_stack_idx_cur && gidx == mt6895_eb_gpu_idx_cur)
		return 0;

	ret = mt6895_eb_commit(ptdev, first,
			       first == MT6895_EB_TARGET_GPU ? gidx : sidx);
	if (!ret)
		ret = mt6895_eb_commit(ptdev, second,
				       second == MT6895_EB_TARGET_GPU ? gidx : sidx);
	if (ret) {
		mt6895_eb_frozen = true;
		return ret;
	}

	mt6895_eb_stack_idx_cur = sidx;
	mt6895_eb_gpu_idx_cur = gidx;
	ptdev->current_frequency = freq;

	return 0;
}

/*
 * Park both domains at their lowest index at probe time. The EB comes out of
 * POWER_CONTROL at s27 (385MHz) while CCF has the core at its own lowest OPP
 * (219MHz), so the two disagree until the first governor poll. Pinning both low
 * lets simple_ondemand scale up from a consistent, minimum-power start.
 */
static void mt6895_eb_park_lowest(struct panthor_device *ptdev)
{
	if (!mt6895_eb_mode())
		return;

	if (mt6895_eb_commit(ptdev, MT6895_EB_TARGET_STACK,
			     MT6895_EB_STACK_IDX_LOWEST) ||
	    mt6895_eb_commit(ptdev, MT6895_EB_TARGET_GPU,
			     MT6895_EB_GPU_IDX_LOWEST)) {
		mt6895_eb_frozen = true;
		return;
	}

	mt6895_eb_stack_idx_cur = MT6895_EB_STACK_IDX_LOWEST;
	mt6895_eb_gpu_idx_cur = MT6895_EB_GPU_IDX_LOWEST;
}
#else
static inline bool mt6895_eb_mode(void) { return false; }
static inline int mt6895_eb_scale(struct panthor_device *ptdev,
				  unsigned long freq) { return -ENODEV; }
static inline void mt6895_eb_park_lowest(struct panthor_device *ptdev) { }
#endif

/**
 * struct panthor_devfreq - Device frequency management
 */
struct panthor_devfreq {
	/** @devfreq: devfreq device. */
	struct devfreq *devfreq;

	/** @gov_data: Governor data. */
	struct devfreq_simple_ondemand_data gov_data;

	/** @busy_time: Busy time. */
	ktime_t busy_time;

	/** @idle_time: Idle time. */
	ktime_t idle_time;

	/** @time_last_update: Last update time. */
	ktime_t time_last_update;

	/** @last_busy_state: True if the GPU was busy last time we updated the state. */
	bool last_busy_state;

	/**
	 * @lock: Lock used to protect busy_time, idle_time, time_last_update and
	 * last_busy_state.
	 *
	 * These fields can be accessed concurrently by panthor_devfreq_get_dev_status()
	 * and panthor_devfreq_record_{busy,idle}().
	 */
	spinlock_t lock;
};

static void panthor_devfreq_update_utilization(struct panthor_devfreq *pdevfreq)
{
	ktime_t now, last;

	now = ktime_get();
	last = pdevfreq->time_last_update;

	if (pdevfreq->last_busy_state)
		pdevfreq->busy_time += ktime_sub(now, last);
	else
		pdevfreq->idle_time += ktime_sub(now, last);

	pdevfreq->time_last_update = now;
}

static int panthor_devfreq_target(struct device *dev, unsigned long *freq,
				  u32 flags)
{
	struct panthor_device *ptdev = dev_get_drvdata(dev);
	struct dev_pm_opp *opp;
	int err;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);

	if (mt6895_eb_mode())
		return mt6895_eb_scale(ptdev, *freq);

	err = dev_pm_opp_set_rate(dev, *freq);
	if (!err)
		ptdev->current_frequency = *freq;

	return err;
}

static void panthor_devfreq_reset(struct panthor_devfreq *pdevfreq)
{
	pdevfreq->busy_time = 0;
	pdevfreq->idle_time = 0;
	pdevfreq->time_last_update = ktime_get();
}

static int panthor_devfreq_get_dev_status(struct device *dev,
					  struct devfreq_dev_status *status)
{
	struct panthor_device *ptdev = dev_get_drvdata(dev);
	struct panthor_devfreq *pdevfreq = ptdev->devfreq;
	unsigned long irqflags;

	/*
	 * CCF's cached rate is stale in EB mode -- the EB owns both PLLs and
	 * never tells the clock framework -- so report what was last committed.
	 */
	if (mt6895_eb_mode())
		status->current_frequency = ptdev->current_frequency;
	else
		status->current_frequency = clk_get_rate(ptdev->clks.core);

	spin_lock_irqsave(&pdevfreq->lock, irqflags);

	panthor_devfreq_update_utilization(pdevfreq);

	status->total_time = ktime_to_ns(ktime_add(pdevfreq->busy_time,
						   pdevfreq->idle_time));

	status->busy_time = ktime_to_ns(pdevfreq->busy_time);

	panthor_devfreq_reset(pdevfreq);

	spin_unlock_irqrestore(&pdevfreq->lock, irqflags);

	drm_dbg(&ptdev->base, "busy %lu total %lu %lu %% freq %lu MHz\n",
		status->busy_time, status->total_time,
		status->busy_time / (status->total_time / 100),
		status->current_frequency / 1000 / 1000);

	return 0;
}

static struct devfreq_dev_profile panthor_devfreq_profile = {
	.timer = DEVFREQ_TIMER_DELAYED,
	.polling_ms = 50, /* ~3 frames */
	.target = panthor_devfreq_target,
	.get_dev_status = panthor_devfreq_get_dev_status,
};

int panthor_devfreq_init(struct panthor_device *ptdev)
{
	/* There's actually 2 regulators (mali and sram), but the OPP core only
	 * supports one.
	 *
	 * We assume the sram regulator is coupled with the mali one and let
	 * the coupling logic deal with voltage updates.
	 */
	static const char * const reg_names[] = { "mali", NULL };
	struct thermal_cooling_device *cooling;
	struct device *dev = ptdev->base.dev;
	struct panthor_devfreq *pdevfreq;
	struct dev_pm_opp *opp;
	unsigned long cur_freq;
	unsigned long freq = ULONG_MAX;
	int ret;

	pdevfreq = drmm_kzalloc(&ptdev->base, sizeof(*ptdev->devfreq), GFP_KERNEL);
	if (!pdevfreq)
		return -ENOMEM;

	ptdev->devfreq = pdevfreq;

	ret = devm_pm_opp_set_regulators(dev, reg_names);
	if (ret) {
		if (ret != -EPROBE_DEFER)
			DRM_DEV_ERROR(dev, "Couldn't set OPP regulators\n");

		return ret;
	}

	ret = devm_pm_opp_of_add_table(dev);
	if (ret)
		return ret;

	spin_lock_init(&pdevfreq->lock);

	panthor_devfreq_reset(pdevfreq);

	cur_freq = clk_get_rate(ptdev->clks.core);

	/* Regulator coupling only takes care of synchronizing/balancing voltage
	 * updates, but the coupled regulator needs to be enabled manually.
	 *
	 * We use devm_regulator_get_enable_optional() and keep the sram supply
	 * enabled until the device is removed, just like we do for the mali
	 * supply, which is enabled when dev_pm_opp_set_opp(dev, opp) is called,
	 * and disabled when the opp_table is torn down, using the devm action.
	 *
	 * If we really care about disabling regulators on suspend, we should:
	 * - use devm_regulator_get_optional() here
	 * - call dev_pm_opp_set_opp(dev, NULL) before leaving this function
	 *   (this disables the regulator passed to the OPP layer)
	 * - call dev_pm_opp_set_opp(dev, NULL) and
	 *   regulator_disable(ptdev->regulators.sram) in
	 *   panthor_devfreq_suspend()
	 * - call dev_pm_opp_set_opp(dev, default_opp) and
	 *   regulator_enable(ptdev->regulators.sram) in
	 *   panthor_devfreq_resume()
	 *
	 * But without knowing if it's beneficial or not (in term of power
	 * consumption), or how much it slows down the suspend/resume steps,
	 * let's just keep regulators enabled for the device lifetime.
	 */
	ret = devm_regulator_get_enable_optional(dev, "sram");
	if (ret && ret != -ENODEV) {
		if (ret != -EPROBE_DEFER)
			DRM_DEV_ERROR(dev, "Couldn't retrieve/enable sram supply\n");
		return ret;
	}

	opp = devfreq_recommended_opp(dev, &cur_freq, 0);
	if (IS_ERR(opp))
		return PTR_ERR(opp);

	panthor_devfreq_profile.initial_freq = cur_freq;
	ptdev->current_frequency = cur_freq;

	/* MT6895: park both EB domains at their lowest index. */
	mt6895_eb_park_lowest(ptdev);

	/*
	 * Set the recommend OPP this will enable and configure the regulator
	 * if any and will avoid a switch off by regulator_late_cleanup()
	 *
	 * Skipped in EB mode: the EB owns both PLLs and both bucks, and the
	 * "mali" supply here is a fixed always-on stub, so this would only add
	 * a second writer to the core clock. See the comment at the top.
	 */
	if (!mt6895_eb_mode()) {
		ret = dev_pm_opp_set_opp(dev, opp);
		if (ret) {
			dev_pm_opp_put(opp);
			DRM_DEV_ERROR(dev, "Couldn't set recommended OPP\n");
			return ret;
		}
	}
	dev_pm_opp_put(opp);

	/* Find the fastest defined rate  */
	opp = dev_pm_opp_find_freq_floor(dev, &freq);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	ptdev->fast_rate = freq;

	dev_pm_opp_put(opp);

	/*
	 * Setup default thresholds for the simple_ondemand governor.
	 * The values are chosen based on experiments.
	 */
	pdevfreq->gov_data.upthreshold = 45;
	pdevfreq->gov_data.downdifferential = 5;

	pdevfreq->devfreq = devm_devfreq_add_device(dev, &panthor_devfreq_profile,
						    DEVFREQ_GOV_SIMPLE_ONDEMAND,
						    &pdevfreq->gov_data);
	if (IS_ERR(pdevfreq->devfreq)) {
		DRM_DEV_ERROR(dev, "Couldn't initialize GPU devfreq\n");
		ret = PTR_ERR(pdevfreq->devfreq);
		pdevfreq->devfreq = NULL;
		return ret;
	}

	cooling = devfreq_cooling_em_register(pdevfreq->devfreq, NULL);
	if (IS_ERR(cooling))
		DRM_DEV_INFO(dev, "Failed to register cooling device\n");

	return 0;
}

void panthor_devfreq_resume(struct panthor_device *ptdev)
{
	struct panthor_devfreq *pdevfreq = ptdev->devfreq;

	if (!pdevfreq || !pdevfreq->devfreq)
		return;

	panthor_devfreq_reset(pdevfreq);

	drm_WARN_ON(&ptdev->base, devfreq_resume_device(pdevfreq->devfreq));
}

void panthor_devfreq_suspend(struct panthor_device *ptdev)
{
	struct panthor_devfreq *pdevfreq = ptdev->devfreq;

	if (!pdevfreq || !pdevfreq->devfreq)
		return;

	drm_WARN_ON(&ptdev->base, devfreq_suspend_device(pdevfreq->devfreq));
}

void panthor_devfreq_record_busy(struct panthor_device *ptdev)
{
	struct panthor_devfreq *pdevfreq = ptdev->devfreq;
	unsigned long irqflags;

	if (!pdevfreq || !pdevfreq->devfreq)
		return;

	spin_lock_irqsave(&pdevfreq->lock, irqflags);

	panthor_devfreq_update_utilization(pdevfreq);
	pdevfreq->last_busy_state = true;

	spin_unlock_irqrestore(&pdevfreq->lock, irqflags);
}

void panthor_devfreq_record_idle(struct panthor_device *ptdev)
{
	struct panthor_devfreq *pdevfreq = ptdev->devfreq;
	unsigned long irqflags;

	if (!pdevfreq || !pdevfreq->devfreq)
		return;

	spin_lock_irqsave(&pdevfreq->lock, irqflags);

	panthor_devfreq_update_utilization(pdevfreq);
	pdevfreq->last_busy_state = false;

	spin_unlock_irqrestore(&pdevfreq->lock, irqflags);
}
