/*
 * Copyright (c) 2015-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include "targcfg.h"
#include "qdf_lock.h"
#include "qdf_status.h"
#include "qdf_status.h"
#include <qdf_atomic.h>         /* qdf_atomic_read */
#include <targaddrs.h>
#include "hif_io32.h"
#include <hif.h>
#include <target_type.h>
#include "regtable.h"
#define ATH_MODULE_NAME hif
#include <a_debug.h>
#include "hif_main.h"
#include "hif_hw_version.h"
#if (defined(HIF_PCI) || defined(HIF_SNOC) || defined(HIF_AHB) || \
     defined(HIF_IPCI))
#include "ce_tasklet.h"
#include "ce_api.h"
#endif
#include "qdf_trace.h"
#include "qdf_status.h"
#include "hif_debug.h"
#include "mp_dev.h"
#if defined(QCA_WIFI_QCA8074) || defined(QCA_WIFI_QCA6018) || \
	defined(QCA_WIFI_QCA5018) || defined(QCA_WIFI_QCA9574) || \
	defined(QCA_WIFI_QCA5332)
#include "hal_api.h"
#endif
#include "hif_napi.h"
#include "hif_unit_test_suspend_i.h"
#include "qdf_module.h"
#ifdef HIF_CE_LOG_INFO
#include <qdf_notifier.h>
#include <qdf_hang_event_notifier.h>
#endif
#include <linux/cpumask.h>

#include <pld_common.h>
#include "ce_internal.h"
#include <qdf_tracepoint.h>
#include "qdf_ssr_driver_dump.h"

void hif_dump(struct hif_opaque_softc *hif_ctx, uint8_t cmd_id, bool start)
{
	hif_trigger_dump(hif_ctx, cmd_id, start);
}

/**
 * hif_get_target_id(): hif_get_target_id
 * @scn: scn
 *
 * Return the virtual memory base address to the caller
 *
 * @scn: hif_softc
 *
 * Return: A_target_id_t
 */
A_target_id_t hif_get_target_id(struct hif_softc *scn)
{
	return scn->mem;
}

/**
 * hif_get_targetdef(): hif_get_targetdef
 * @hif_ctx: hif context
 *
 * Return: void *
 */
void *hif_get_targetdef(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	return scn->targetdef;
}

#ifdef FORCE_WAKE
#ifndef QCA_WIFI_WCN6450
void hif_srng_init_phase(struct hif_opaque_softc *hif_ctx,
			 bool init_phase)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	if (ce_srng_based(scn))
		hal_set_init_phase(scn->hal_soc, init_phase);
}
#else
void hif_srng_init_phase(struct hif_opaque_softc *hif_ctx,
			 bool init_phase)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	hal_set_init_phase(scn->hal_soc, init_phase);
}
#endif
#endif /* FORCE_WAKE */

#ifdef HIF_IPCI
void hif_shutdown_notifier_cb(void *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	scn->recovery = true;
}
#endif

/**
 * hif_vote_link_down(): unvote for link up
 * @hif_ctx: hif context
 *
 * Call hif_vote_link_down to release a previous request made using
 * hif_vote_link_up. A hif_vote_link_down call should only be made
 * after a corresponding hif_vote_link_up, otherwise you could be
 * negating a vote from another source. When no votes are present
 * hif will not guarantee the linkstate after hif_bus_suspend.
 *
 * SYNCHRONIZE WITH hif_vote_link_up by only calling in MC thread
 * and initialization deinitialization sequencences.
 *
 * Return: n/a
 */
void hif_vote_link_down(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	QDF_BUG(scn);
	if (scn->linkstate_vote == 0)
		QDF_DEBUG_PANIC("linkstate_vote(%d) has already been 0",
				scn->linkstate_vote);

	scn->linkstate_vote--;
	hif_info("Down_linkstate_vote %d", scn->linkstate_vote);
	if (scn->linkstate_vote == 0)
		hif_bus_prevent_linkdown(scn, false);
}

/**
 * hif_vote_link_up(): vote to prevent bus from suspending
 * @hif_ctx: hif context
 *
 * Makes hif guarantee that fw can message the host normally
 * during suspend.
 *
 * SYNCHRONIZE WITH hif_vote_link_up by only calling in MC thread
 * and initialization deinitialization sequencences.
 *
 * Return: n/a
 */
void hif_vote_link_up(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	QDF_BUG(scn);
	scn->linkstate_vote++;
	hif_info("Up_linkstate_vote %d", scn->linkstate_vote);
	if (scn->linkstate_vote == 1)
		hif_bus_prevent_linkdown(scn, true);
}

/**
 * hif_can_suspend_link(): query if hif is permitted to suspend the link
 * @hif_ctx: hif context
 *
 * Hif will ensure that the link won't be suspended if the upperlayers
 * don't want it to.
 *
 * SYNCHRONIZATION: MC thread is stopped before bus suspend thus
 * we don't need extra locking to ensure votes dont change while
 * we are in the process of suspending or resuming.
 *
 * Return: false if hif will guarantee link up during suspend.
 */
bool hif_can_suspend_link(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	QDF_BUG(scn);
	return scn->linkstate_vote == 0;
}

/**
 * hif_hia_item_address(): hif_hia_item_address
 * @target_type: target_type
 * @item_offset: item_offset
 *
 * Return: n/a
 */
uint32_t hif_hia_item_address(uint32_t target_type, uint32_t item_offset)
{
	switch (target_type) {
	case TARGET_TYPE_AR6002:
		return AR6002_HOST_INTEREST_ADDRESS + item_offset;
	case TARGET_TYPE_AR6003:
		return AR6003_HOST_INTEREST_ADDRESS + item_offset;
	case TARGET_TYPE_AR6004:
		return AR6004_HOST_INTEREST_ADDRESS + item_offset;
	case TARGET_TYPE_AR6006:
		return AR6006_HOST_INTEREST_ADDRESS + item_offset;
	case TARGET_TYPE_AR9888:
		return AR9888_HOST_INTEREST_ADDRESS + item_offset;
	case TARGET_TYPE_AR6320:
	case TARGET_TYPE_AR6320V2:
		return AR6320_HOST_INTEREST_ADDRESS + item_offset;
	case TARGET_TYPE_ADRASTEA:
		/* ADRASTEA doesn't have a host interest address */
		ASSERT(0);
		return 0;
	case TARGET_TYPE_AR900B:
		return AR900B_HOST_INTEREST_ADDRESS + item_offset;
	case TARGET_TYPE_QCA9984:
		return QCA9984_HOST_INTEREST_ADDRESS + item_offset;
	case TARGET_TYPE_QCA9888:
		return QCA9888_HOST_INTEREST_ADDRESS + item_offset;

	default:
		ASSERT(0);
		return 0;
	}
}

/**
 * hif_max_num_receives_reached() - check max receive is reached
 * @scn: HIF Context
 * @count: unsigned int.
 *
 * Output check status as bool
 *
 * Return: bool
 */
bool hif_max_num_receives_reached(struct hif_softc *scn, unsigned int count)
{
	if (QDF_IS_EPPING_ENABLED(hif_get_conparam(scn)))
		return count > 120;
	else
		return count > MAX_NUM_OF_RECEIVES;
}

/**
 * init_buffer_count() - initial buffer count
 * @maxSize: qdf_size_t
 *
 * routine to modify the initial buffer count to be allocated on an os
 * platform basis. Platform owner will need to modify this as needed
 *
 * Return: qdf_size_t
 */
qdf_size_t init_buffer_count(qdf_size_t maxSize)
{
	return maxSize;
}

/**
 * hif_save_htc_htt_config_endpoint() - save htt_tx_endpoint
 * @hif_ctx: hif context
 * @htc_htt_tx_endpoint: htt_tx_endpoint
 *
 * Return: void
 */
void hif_save_htc_htt_config_endpoint(struct hif_opaque_softc *hif_ctx,
							int htc_htt_tx_endpoint)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	if (!scn) {
		hif_err("scn or scn->hif_sc is NULL!");
		return;
	}

	scn->htc_htt_tx_endpoint = htc_htt_tx_endpoint;
}
qdf_export_symbol(hif_save_htc_htt_config_endpoint);

static const struct qwlan_hw qwlan_hw_list[] = {
	{
		.id = AR6320_REV1_VERSION,
		.subid = 0,
		.name = "QCA6174_REV1",
	},
	{
		.id = AR6320_REV1_1_VERSION,
		.subid = 0x1,
		.name = "QCA6174_REV1_1",
	},
	{
		.id = AR6320_REV1_3_VERSION,
		.subid = 0x2,
		.name = "QCA6174_REV1_3",
	},
	{
		.id = AR6320_REV2_1_VERSION,
		.subid = 0x4,
		.name = "QCA6174_REV2_1",
	},
	{
		.id = AR6320_REV2_1_VERSION,
		.subid = 0x5,
		.name = "QCA6174_REV2_2",
	},
	{
		.id = AR6320_REV3_VERSION,
		.subid = 0x6,
		.name = "QCA6174_REV2.3",
	},
	{
		.id = AR6320_REV3_VERSION,
		.subid = 0x8,
		.name = "QCA6174_REV3",
	},
	{
		.id = AR6320_REV3_VERSION,
		.subid = 0x9,
		.name = "QCA6174_REV3_1",
	},
	{
		.id = AR6320_REV3_2_VERSION,
		.subid = 0xA,
		.name = "AR6320_REV3_2_VERSION",
	},
	{
		.id = QCA6390_V1,
		.subid = 0x0,
		.name = "QCA6390_V1",
	},
	{
		.id = QCA6490_V1,
		.subid = 0x0,
		.name = "QCA6490_V1",
	},
	{
		.id = WCN3990_v1,
		.subid = 0x0,
		.name = "WCN3990_V1",
	},
	{
		.id = WCN3990_v2,
		.subid = 0x0,
		.name = "WCN3990_V2",
	},
	{
		.id = WCN3990_v2_1,
		.subid = 0x0,
		.name = "WCN3990_V2.1",
	},
	{
		.id = WCN3998,
		.subid = 0x0,
		.name = "WCN3998",
	},
	{
		.id = QCA9379_REV1_VERSION,
		.subid = 0xC,
		.name = "QCA9379_REV1",
	},
	{
		.id = QCA9379_REV1_VERSION,
		.subid = 0xD,
		.name = "QCA9379_REV1_1",
	},
	{
		.id = MANGO_V1,
		.subid = 0xF,
		.name = "MANGO_V1",
	},
	{
		.id = PEACH_V1,
		.subid = 0,
		.name = "PEACH_V1",
	},

	{
		.id = KIWI_V1,
		.subid = 0,
		.name = "KIWI_V1",
	},
	{
		.id = KIWI_V2,
		.subid = 0,
		.name = "KIWI_V2",
	},
	{
		.id = WCN6750_V1,
		.subid = 0,
		.name = "WCN6750_V1",
	},
	{
		.id = WCN6750_V2,
		.subid = 0,
		.name = "WCN6750_V2",
	},
	{
		.id = WCN6450_V1,
		.subid = 0,
		.name = "WCN6450_V1",
	},
	{
		.id = QCA6490_v2_1,
		.subid = 0,
		.name = "QCA6490",
	},
	{
		.id = QCA6490_v2,
		.subid = 0,
		.name = "QCA6490",
	},
	{
		.id = WCN3990_TALOS,
		.subid = 0,
		.name = "WCN3990",
	},
	{
		.id = WCN3990_MOOREA,
		.subid = 0,
		.name = "WCN3990",
	},
	{
		.id = WCN3990_SAIPAN,
		.subid = 0,
		.name = "WCN3990",
	},
	{
		.id = WCN3990_RENNELL,
		.subid = 0,
		.name = "WCN3990",
	},
	{
		.id = WCN3990_BITRA,
		.subid = 0,
		.name = "WCN3990",
	},
	{
		.id = WCN3990_DIVAR,
		.subid = 0,
		.name = "WCN3990",
	},
	{
		.id = WCN3990_ATHERTON,
		.subid = 0,
		.name = "WCN3990",
	},
	{
		.id = WCN3990_STRAIT,
		.subid = 0,
		.name = "WCN3990",
	},
	{
		.id = WCN3990_NETRANI,
		.subid = 0,
		.name = "WCN3990",
	},
	{
		.id = WCN3990_CLARENCE,
		.subid = 0,
		.name = "WCN3990",
	}
};

/**
 * hif_get_hw_name(): get a human readable name for the hardware
 * @info: Target Info
 *
 * Return: human readable name for the underlying wifi hardware.
 */
static const char *hif_get_hw_name(struct hif_target_info *info)
{
	int i;

	hif_debug("target version = %d, target revision = %d",
		  info->target_version,
		  info->target_revision);

	if (info->hw_name)
		return info->hw_name;

	for (i = 0; i < ARRAY_SIZE(qwlan_hw_list); i++) {
		if (info->target_version == qwlan_hw_list[i].id &&
		    info->target_revision == qwlan_hw_list[i].subid) {
			return qwlan_hw_list[i].name;
		}
	}

	info->hw_name = qdf_mem_malloc(64);
	if (!info->hw_name)
		return "Unknown Device (nomem)";

	i = qdf_snprint(info->hw_name, 64, "HW_VERSION=%x.",
			info->target_version);
	if (i < 0)
		return "Unknown Device (snprintf failure)";
	else
		return info->hw_name;
}

/**
 * hif_get_hw_info(): hif_get_hw_info
 * @scn: scn
 * @version: version
 * @revision: revision
 * @target_name: target name
 *
 * Return: n/a
 */
void hif_get_hw_info(struct hif_opaque_softc *scn, u32 *version, u32 *revision,
			const char **target_name)
{
	struct hif_target_info *info = hif_get_target_info_handle(scn);
	struct hif_softc *sc = HIF_GET_SOFTC(scn);

	if (sc->bus_type == QDF_BUS_TYPE_USB)
		hif_usb_get_hw_info(sc);

	*version = info->target_version;
	*revision = info->target_revision;
	*target_name = hif_get_hw_name(info);
}

/**
 * hif_get_dev_ba(): API to get device base address.
 * @hif_handle: hif handle
 *
 * Return: device base address
 */
void *hif_get_dev_ba(struct hif_opaque_softc *hif_handle)
{
	struct hif_softc *scn = (struct hif_softc *)hif_handle;

	return scn->mem;
}
qdf_export_symbol(hif_get_dev_ba);

/**
 * hif_get_dev_ba_ce(): API to get device ce base address.
 * @hif_handle: hif handle
 *
 * Return: dev mem base address for CE
 */
void *hif_get_dev_ba_ce(struct hif_opaque_softc *hif_handle)
{
	struct hif_softc *scn = (struct hif_softc *)hif_handle;

	return scn->mem_ce;
}

qdf_export_symbol(hif_get_dev_ba_ce);

/**
 * hif_get_dev_ba_pmm(): API to get device pmm base address.
 * @hif_handle: scn
 *
 * Return: dev mem base address for PMM
 */

void *hif_get_dev_ba_pmm(struct hif_opaque_softc *hif_handle)
{
	struct hif_softc *scn = (struct hif_softc *)hif_handle;

	return scn->mem_pmm_base;
}

qdf_export_symbol(hif_get_dev_ba_pmm);

uint32_t hif_get_soc_version(struct hif_opaque_softc *hif_handle)
{
	struct hif_softc *scn = (struct hif_softc *)hif_handle;

	return scn->target_info.soc_version;
}

qdf_export_symbol(hif_get_soc_version);

/**
 * hif_get_dev_ba_cmem(): API to get device ce base address.
 * @hif_handle: hif handle
 *
 * Return: dev mem base address for CMEM
 */
void *hif_get_dev_ba_cmem(struct hif_opaque_softc *hif_handle)
{
	struct hif_softc *scn = (struct hif_softc *)hif_handle;

	return scn->mem_cmem;
}

qdf_export_symbol(hif_get_dev_ba_cmem);

#ifdef FEATURE_RUNTIME_PM
void hif_runtime_prevent_linkdown(struct hif_softc *scn, bool is_get)
{
	if (is_get)
		qdf_runtime_pm_prevent_suspend(&scn->prevent_linkdown_lock);
	else
		qdf_runtime_pm_allow_suspend(&scn->prevent_linkdown_lock);
}

static inline
void hif_rtpm_lock_init(struct hif_softc *scn)
{
	qdf_runtime_lock_init(&scn->prevent_linkdown_lock);
}

static inline
void hif_rtpm_lock_deinit(struct hif_softc *scn)
{
	qdf_runtime_lock_deinit(&scn->prevent_linkdown_lock);
}
#else
static inline
void hif_rtpm_lock_init(struct hif_softc *scn)
{
}

static inline
void hif_rtpm_lock_deinit(struct hif_softc *scn)
{
}
#endif

#ifdef WLAN_CE_INTERRUPT_THRESHOLD_CONFIG
/**
 * hif_get_interrupt_threshold_cfg_from_psoc() - Retrieve ini cfg from psoc
 * @scn: hif context
 * @psoc: psoc objmgr handle
 *
 * Return: None
 */
static inline
void hif_get_interrupt_threshold_cfg_from_psoc(struct hif_softc *scn,
					       struct wlan_objmgr_psoc *psoc)
{
	if (psoc) {
		scn->ini_cfg.ce_status_ring_timer_threshold =
			cfg_get(psoc,
				CFG_CE_STATUS_RING_TIMER_THRESHOLD);
		scn->ini_cfg.ce_status_ring_batch_count_threshold =
			cfg_get(psoc,
				CFG_CE_STATUS_RING_BATCH_COUNT_THRESHOLD);
	}
}
#else
static inline
void hif_get_interrupt_threshold_cfg_from_psoc(struct hif_softc *scn,
					       struct wlan_objmgr_psoc *psoc)
{
}
#endif /* WLAN_CE_INTERRUPT_THRESHOLD_CONFIG */

/**
 * hif_get_cfg_from_psoc() - Retrieve ini cfg from psoc
 * @scn: hif context
 * @psoc: psoc objmgr handle
 *
 * Return: None
 */
static inline
void hif_get_cfg_from_psoc(struct hif_softc *scn,
			   struct wlan_objmgr_psoc *psoc)
{
	if (psoc) {
		scn->ini_cfg.disable_wake_irq =
			cfg_get(psoc, CFG_DISABLE_WAKE_IRQ);
		/**
		 * Wake IRQ can't share the same IRQ with the copy engines
		 * In one MSI mode, we don't know whether wake IRQ is triggered
		 * or not in wake IRQ handler. known issue CR 2055359
		 * If you want to support Wake IRQ. Please allocate at least
		 * 2 MSI vector. The first is for wake IRQ while the others
		 * share the second vector
		 */
		if (pld_is_one_msi(scn->qdf_dev->dev)) {
			hif_debug("Disable wake IRQ once it is one MSI mode");
			scn->ini_cfg.disable_wake_irq = true;
		}
		hif_get_interrupt_threshold_cfg_from_psoc(scn, psoc);
	}
}

#if defined(HIF_CE_LOG_INFO) || defined(HIF_BUS_LOG_INFO)
/**
 * hif_recovery_notifier_cb - Recovery notifier callback to log
 *  hang event data
 * @block: notifier block
 * @state: state
 * @data: notifier data
 *
 * Return: status
 */
static
int hif_recovery_notifier_cb(struct notifier_block *block, unsigned long state,
			     void *data)
{
	struct qdf_notifer_data *notif_data = data;
	qdf_notif_block *notif_block;
	struct hif_softc *hif_handle;
	bool bus_id_invalid;

	if (!data || !block)
		return -EINVAL;

	notif_block = qdf_container_of(block, qdf_notif_block, notif_block);

	hif_handle = notif_block->priv_data;
	if (!hif_handle)
		return -EINVAL;

	bus_id_invalid = hif_log_bus_info(hif_handle, notif_data->hang_data,
					  &notif_data->offset);
	if (bus_id_invalid)
		return NOTIFY_STOP_MASK;

	hif_log_ce_info(hif_handle, notif_data->hang_data,
			&notif_data->offset);

	return 0;
}

/**
 * hif_register_recovery_notifier - Register hif recovery notifier
 * @hif_handle: hif handle
 *
 * Return: status
 */
static
QDF_STATUS hif_register_recovery_notifier(struct hif_softc *hif_handle)
{
	qdf_notif_block *hif_notifier;

	if (!hif_handle)
		return QDF_STATUS_E_FAILURE;

	hif_notifier = &hif_handle->hif_recovery_notifier;

	hif_notifier->notif_block.notifier_call = hif_recovery_notifier_cb;
	hif_notifier->priv_data = hif_handle;
	return qdf_hang_event_register_notifier(hif_notifier);
}

/**
 * hif_unregister_recovery_notifier - Un-register hif recovery notifier
 * @hif_handle: hif handle
 *
 * Return: status
 */
static
QDF_STATUS hif_unregister_recovery_notifier(struct hif_softc *hif_handle)
{
	qdf_notif_block *hif_notifier = &hif_handle->hif_recovery_notifier;

	return qdf_hang_event_unregister_notifier(hif_notifier);
}
#else
static inline
QDF_STATUS hif_register_recovery_notifier(struct hif_softc *hif_handle)
{
	return QDF_STATUS_SUCCESS;
}

static inline
QDF_STATUS hif_unregister_recovery_notifier(struct hif_softc *hif_handle)
{
	return QDF_STATUS_SUCCESS;
}
#endif

#if defined(HIF_CPU_PERF_AFFINE_MASK) || \
	defined(FEATURE_ENABLE_CE_DP_IRQ_AFFINE)
/**
 * __hif_cpu_hotplug_notify() - CPU hotplug event handler
 * @context: HIF context
 * @cpu: CPU Id of the CPU generating the event
 * @cpu_up: true if the CPU is online
 *
 * Return: None
 */
static void __hif_cpu_hotplug_notify(void *context,
				     uint32_t cpu, bool cpu_up)
{
	struct hif_softc *scn = context;

	if (!scn)
		return;
	if (hif_is_driver_unloading(scn) || hif_is_recovery_in_progress(scn))
		return;

	if (cpu_up) {
		hif_config_irq_set_perf_affinity_hint(GET_HIF_OPAQUE_HDL(scn));
		hif_debug("Setting affinity for online CPU: %d", cpu);
	} else {
		hif_debug("Skip setting affinity for offline CPU: %d", cpu);
	}
}

/**
 * hif_cpu_hotplug_notify - cpu core up/down notification
 * handler
 * @context: HIF context
 * @cpu: CPU generating the event
 * @cpu_up: true if the CPU is online
 *
 * Return: None
 */
static void hif_cpu_hotplug_notify(void *context, uint32_t cpu, bool cpu_up)
{
	struct qdf_op_sync *op_sync;

	if (qdf_op_protect(&op_sync))
		return;

	__hif_cpu_hotplug_notify(context, cpu, cpu_up);

	qdf_op_unprotect(op_sync);
}

static void hif_cpu_online_cb(void *context, uint32_t cpu)
{
	hif_cpu_hotplug_notify(context, cpu, true);
}

static void hif_cpu_before_offline_cb(void *context, uint32_t cpu)
{
	hif_cpu_hotplug_notify(context, cpu, false);
}

static void hif_cpuhp_register(struct hif_softc *scn)
{
	if (!scn) {
		hif_info_high("cannot register hotplug notifiers");
		return;
	}
	qdf_cpuhp_register(&scn->cpuhp_event_handle,
			   scn,
			   hif_cpu_online_cb,
			   hif_cpu_before_offline_cb);
}

static void hif_cpuhp_unregister(struct hif_softc *scn)
{
	if (!scn) {
		hif_info_high("cannot unregister hotplug notifiers");
		return;
	}
	qdf_cpuhp_unregister(&scn->cpuhp_event_handle);
}

#else
static void hif_cpuhp_register(struct hif_softc *scn)
{
}

static void hif_cpuhp_unregister(struct hif_softc *scn)
{
}
#endif /* ifdef HIF_CPU_PERF_AFFINE_MASK */

#ifdef HIF_DETECTION_LATENCY_ENABLE
/*
 * Bitmask to control enablement of latency detection for the tasklets,
 * bit-X represents for tasklet of WLAN_CE_X.
 */
#ifndef DETECTION_LATENCY_TASKLET_MASK
#define DETECTION_LATENCY_TASKLET_MASK (BIT(2) | BIT(7))
#endif

static inline int
__hif_tasklet_latency(struct hif_softc *scn, bool from_timer, int idx)
{
	qdf_time_t sched_time =
		scn->latency_detect.tasklet_info[idx].sched_time;
	qdf_time_t exec_time =
		scn->latency_detect.tasklet_info[idx].exec_time;
	qdf_time_t curr_time = qdf_system_ticks();
	uint32_t threshold = scn->latency_detect.threshold;
	qdf_time_t expect_exec_time =
		sched_time + qdf_system_msecs_to_ticks(threshold);

	/* 2 kinds of check here.
	 * from_timer==true:  check if tasklet stall
	 * from_timer==false: check tasklet execute comes late
	 */
	if (from_timer ?
	    (qdf_system_time_after(sched_time, exec_time) &&
	     qdf_system_time_after(curr_time, expect_exec_time)) :
	    qdf_system_time_after(exec_time, expect_exec_time)) {
		hif_err("tasklet[%d] latency detected: from_timer %d, curr_time %lu, sched_time %lu, exec_time %lu, threshold %ums, timeout %ums, cpu_id %d, called: %ps",
			idx, from_timer, curr_time, sched_time,
			exec_time, threshold,
			scn->latency_detect.timeout,
			qdf_get_cpu(), (void *)_RET_IP_);
		qdf_trigger_self_recovery(NULL,
					  QDF_TASKLET_CREDIT_LATENCY_DETECT);
		return -ETIMEDOUT;
	}

	return 0;
}

/**
 * hif_tasklet_latency_detect_enabled() - check whether latency detect
 * is enabled for the tasklet which is specified by idx
 * @scn: HIF opaque context
 * @idx: CE id
 *
 * Return: true if latency detect is enabled for the specified tasklet,
 * false otherwise.
 */
static inline bool
hif_tasklet_latency_detect_enabled(struct hif_softc *scn, int idx)
{
	if (QDF_GLOBAL_MISSION_MODE != hif_get_conparam(scn))
		return false;

	if (!scn->latency_detect.enable_detection)
		return false;

	if (idx < 0 || idx >= HIF_TASKLET_IN_MONITOR ||
	    !qdf_test_bit(idx, scn->latency_detect.tasklet_bmap))
		return false;

	return true;
}

void hif_tasklet_latency_record_exec(struct hif_softc *scn, int idx)
{
	if (!hif_tasklet_latency_detect_enabled(scn, idx))
		return;

	/*
	 * hif_set_enable_detection(true) might come between
	 * hif_tasklet_latency_record_sched() and
	 * hif_tasklet_latency_record_exec() during wlan startup, then the
	 * sched_time is 0 but exec_time is not, and hit the timeout case in
	 * __hif_tasklet_latency().
	 * To avoid such issue, skip exec_time recording if sched_time has not
	 * been recorded.
	 */
	if (!scn->latency_detect.tasklet_info[idx].sched_time)
		return;

	scn->latency_detect.tasklet_info[idx].exec_time = qdf_system_ticks();
	__hif_tasklet_latency(scn, false, idx);
}

void hif_tasklet_latency_record_sched(struct hif_softc *scn, int idx)
{
	if (!hif_tasklet_latency_detect_enabled(scn, idx))
		return;

	scn->latency_detect.tasklet_info[idx].sched_cpuid = qdf_get_cpu();
	scn->latency_detect.tasklet_info[idx].sched_time = qdf_system_ticks();
}

static inline void hif_credit_latency(struct hif_softc *scn, bool from_timer)
{
	qdf_time_t credit_request_time =
		scn->latency_detect.credit_request_time;
	qdf_time_t credit_report_time = scn->latency_detect.credit_report_time;
	qdf_time_t curr_jiffies = qdf_system_ticks();
	uint32_t threshold = scn->latency_detect.threshold;
	int cpu_id = qdf_get_cpu();

	/* 2 kinds of check here.
	 * from_timer==true:  check if credit report stall
	 * from_timer==false: check credit report comes late
	 */

	if ((from_timer ?
	     qdf_system_time_after(credit_request_time, credit_report_time) :
	     qdf_system_time_after(credit_report_time, credit_request_time)) &&
	    qdf_system_time_after(curr_jiffies,
				  credit_request_time +
				  qdf_system_msecs_to_ticks(threshold))) {
		hif_err("credit report latency: from timer %d, curr_jiffies %lu, credit_request_time %lu, credit_report_time %lu, threshold %ums, timeout %ums, cpu_id %d, called: %ps",
			from_timer, curr_jiffies, credit_request_time,
			credit_report_time, threshold,
			scn->latency_detect.timeout,
			cpu_id, (void *)_RET_IP_);
		goto latency;
	}
	return;

latency:
	qdf_trigger_self_recovery(NULL, QDF_TASKLET_CREDIT_LATENCY_DETECT);
}

static inline void hif_tasklet_latency(struct hif_softc *scn, bool from_timer)
{
	int i, ret;

	for (i = 0; i < HIF_TASKLET_IN_MONITOR; i++) {
		if (!qdf_test_bit(i, scn->latency_detect.tasklet_bmap))
			continue;

		ret = __hif_tasklet_latency(scn, from_timer, i);
		if (ret)
			return;
	}
}

/**
 * hif_check_detection_latency(): to check if latency for tasklet/credit
 *
 * @scn: hif context
 * @from_timer: if called from timer handler
 * @bitmap_type: indicate if check tasklet or credit
 *
 * Return: none
 */
void hif_check_detection_latency(struct hif_softc *scn,
				 bool from_timer,
				 uint32_t bitmap_type)
{
	if (QDF_GLOBAL_MISSION_MODE != hif_get_conparam(scn))
		return;

	if (!scn->latency_detect.enable_detection)
		return;

	if (bitmap_type & BIT(HIF_DETECT_TASKLET))
		hif_tasklet_latency(scn, from_timer);

	if (bitmap_type & BIT(HIF_DETECT_CREDIT))
		hif_credit_latency(scn, from_timer);
}

static void hif_latency_detect_timeout_handler(void *arg)
{
	struct hif_softc *scn = (struct hif_softc *)arg;
	int next_cpu, i;
	qdf_cpu_mask cpu_mask = {0};
	struct hif_latency_detect *detect = &scn->latency_detect;

	hif_check_detection_latency(scn, true,
				    BIT(HIF_DETECT_TASKLET) |
				    BIT(HIF_DETECT_CREDIT));

	/* it need to make sure timer start on a different cpu,
	 * so it can detect the tasklet schedule stall, but there
	 * is still chance that, after timer has been started, then
	 * irq/tasklet happens on the same cpu, then tasklet will
	 * execute before softirq timer, if this tasklet stall, the
	 * timer can't detect it, we can accept this as a limitation,
	 * if tasklet stall, anyway other place will detect it, just
	 * a little later.
	 */
	qdf_cpumask_copy(&cpu_mask, (const qdf_cpu_mask *)cpu_active_mask);
	for (i = 0; i < HIF_TASKLET_IN_MONITOR; i++) {
		if (!qdf_test_bit(i, detect->tasklet_bmap))
			continue;

		qdf_cpumask_clear_cpu(detect->tasklet_info[i].sched_cpuid,
				      &cpu_mask);
	}

	next_cpu = cpumask_first(&cpu_mask);
	if (qdf_unlikely(next_cpu >= nr_cpu_ids)) {
		hif_debug("start timer on local");
		/* it doesn't found a available cpu, start on local cpu*/
		qdf_timer_mod(&detect->timer, detect->timeout);
	} else {
		qdf_timer_start_on(&detect->timer, detect->timeout, next_cpu);
	}
}

static void hif_latency_detect_timer_init(struct hif_softc *scn)
{
	scn->latency_detect.timeout =
		DETECTION_TIMER_TIMEOUT;
	scn->latency_detect.threshold =
		DETECTION_LATENCY_THRESHOLD;

	hif_info("timer timeout %u, latency threshold %u",
		 scn->latency_detect.timeout,
		 scn->latency_detect.threshold);

	scn->latency_detect.is_timer_started = false;

	qdf_timer_init(NULL,
		       &scn->latency_detect.timer,
		       &hif_latency_detect_timeout_handler,
		       scn,
		       QDF_TIMER_TYPE_SW_SPIN);
}

static void hif_latency_detect_timer_deinit(struct hif_softc *scn)
{
	hif_info("deinit timer");
	qdf_timer_free(&scn->latency_detect.timer);
}

static void hif_latency_detect_init(struct hif_softc *scn)
{
	uint32_t tasklet_mask;
	int i;

	if (QDF_GLOBAL_MISSION_MODE != hif_get_conparam(scn))
		return;

	tasklet_mask = DETECTION_LATENCY_TASKLET_MASK;
	hif_info("tasklet mask is 0x%x", tasklet_mask);
	for (i = 0; i < HIF_TASKLET_IN_MONITOR; i++) {
		if (BIT(i) & tasklet_mask)
			qdf_set_bit(i, scn->latency_detect.tasklet_bmap);
	}

	hif_latency_detect_timer_init(scn);
}

static void hif_latency_detect_deinit(struct hif_softc *scn)
{
	int i;

	if (QDF_GLOBAL_MISSION_MODE != hif_get_conparam(scn))
		return;

	hif_latency_detect_timer_deinit(scn);
	for (i = 0; i < HIF_TASKLET_IN_MONITOR; i++)
		qdf_clear_bit(i, scn->latency_detect.tasklet_bmap);
}

void hif_latency_detect_timer_start(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	if (QDF_GLOBAL_MISSION_MODE != hif_get_conparam(scn))
		return;

	hif_debug_rl("start timer");
	if (scn->latency_detect.is_timer_started) {
		hif_info("timer has been started");
		return;
	}

	qdf_timer_start(&scn->latency_detect.timer,
			scn->latency_detect.timeout);
	scn->latency_detect.is_timer_started = true;
}

void hif_latency_detect_timer_stop(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	if (QDF_GLOBAL_MISSION_MODE != hif_get_conparam(scn))
		return;

	hif_debug_rl("stop timer");

	qdf_timer_sync_cancel(&scn->latency_detect.timer);
	scn->latency_detect.is_timer_started = false;
}

void hif_latency_detect_credit_record_time(
	enum hif_credit_exchange_type type,
	struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	if (!scn) {
		hif_err("Could not do runtime put, scn is null");
		return;
	}

	if (QDF_GLOBAL_MISSION_MODE != hif_get_conparam(scn))
		return;

	if (HIF_REQUEST_CREDIT == type)
		scn->latency_detect.credit_request_time = qdf_system_ticks();
	else if (HIF_PROCESS_CREDIT_REPORT == type)
		scn->latency_detect.credit_report_time = qdf_system_ticks();

	hif_check_detection_latency(scn, false, BIT(HIF_DETECT_CREDIT));
}

void hif_set_enable_detection(struct hif_opaque_softc *hif_ctx, bool value)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	if (!scn) {
		hif_err("Could not do runtime put, scn is null");
		return;
	}

	if (QDF_GLOBAL_MISSION_MODE != hif_get_conparam(scn))
		return;

	scn->latency_detect.enable_detection = value;
}
#else
static inline void hif_latency_detect_init(struct hif_softc *scn)
{}

static inline void hif_latency_detect_deinit(struct hif_softc *scn)
{}
#endif

#ifdef WLAN_FEATURE_AFFINITY_MGR
#define AFFINITY_THRESHOLD 5000000
static inline void
hif_affinity_mgr_init(struct hif_softc *scn, struct wlan_objmgr_psoc *psoc)
{
	unsigned int cpus;
	qdf_cpu_mask allowed_mask = {0};

	scn->affinity_mgr_supported =
		(cfg_get(psoc, CFG_IRQ_AFFINE_AUDIO_USE_CASE) &&
		qdf_walt_get_cpus_taken_supported());

	hif_info("Affinity Manager supported: %d", scn->affinity_mgr_supported);

	if (!scn->affinity_mgr_supported)
		return;

	scn->time_threshold = AFFINITY_THRESHOLD;
	qdf_for_each_possible_cpu(cpus)
		if (qdf_topology_physical_package_id(cpus) ==
			CPU_CLUSTER_TYPE_LITTLE)
			qdf_cpumask_set_cpu(cpus, &allowed_mask);
	qdf_cpumask_copy(&scn->allowed_mask, &allowed_mask);
}
#else
static inline void
hif_affinity_mgr_init(struct hif_softc *scn, struct wlan_objmgr_psoc *psoc)
{
}
#endif

#ifdef FEATURE_DIRECT_LINK
/**
 * hif_init_direct_link_rcv_pipe_num(): Initialize the direct link receive
 *  pipe number
 * @scn: hif context
 *
 * Return: None
 */
static inline
void hif_init_direct_link_rcv_pipe_num(struct hif_softc *scn)
{
	scn->dl_recv_pipe_num = INVALID_PIPE_NO;
}
#else
static inline
void hif_init_direct_link_rcv_pipe_num(struct hif_softc *scn)
{
}
#endif

struct hif_opaque_softc *hif_open(qdf_device_t qdf_ctx,
				  uint32_t mode,
				  enum qdf_bus_type bus_type,
				  struct hif_driver_state_callbacks *cbk,
				  struct wlan_objmgr_psoc *psoc)
{
	struct hif_softc *scn;
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	int bus_context_size = hif_bus_get_context_size(bus_type);

	if (bus_context_size == 0) {
		hif_err("context size 0 not allowed");
		return NULL;
	}

	scn = (struct hif_softc *)qdf_mem_malloc(bus_context_size);
	if (!scn)
		return GET_HIF_OPAQUE_HDL(scn);

	scn->qdf_dev = qdf_ctx;
	scn->hif_con_param = mode;
	qdf_atomic_init(&scn->active_tasklet_cnt);
	qdf_atomic_init(&scn->active_oom_work_cnt);

	qdf_atomic_init(&scn->active_grp_tasklet_cnt);
	qdf_atomic_init(&scn->link_suspended);
	qdf_atomic_init(&scn->tasklet_from_intr);
	hif_system_pm_set_state_on(GET_HIF_OPAQUE_HDL(scn));
	qdf_mem_copy(&scn->callbacks, cbk,
		     sizeof(struct hif_driver_state_callbacks));
	scn->bus_type  = bus_type;

	hif_allow_ep_vote_access(GET_HIF_OPAQUE_HDL(scn));
	hif_get_cfg_from_psoc(scn, psoc);

	hif_set_event_hist_mask(GET_HIF_OPAQUE_HDL(scn));
	status = hif_bus_open(scn, bus_type);
	if (status != QDF_STATUS_SUCCESS) {
		hif_err("hif_bus_open error = %d, bus_type = %d",
			status, bus_type);
		qdf_mem_free(scn);
		scn = NULL;
		goto out;
	}

	hif_rtpm_lock_init(scn);

	hif_cpuhp_register(scn);
	hif_latency_detect_init(scn);
	hif_affinity_mgr_init(scn, psoc);
	hif_init_direct_link_rcv_pipe_num(scn);
	hif_ce_desc_history_log_register(scn);
	hif_desc_history_log_register();
	qdf_ssr_driver_dump_register_region("hif", scn, sizeof(*scn));

out:
	return GET_HIF_OPAQUE_HDL(scn);
}

#ifdef ADRASTEA_RRI_ON_DDR
/**
 * hif_uninit_rri_on_ddr(): free consistent memory allocated for rri
 * @scn: hif context
 *
 * Return: none
 */
void hif_uninit_rri_on_ddr(struct hif_softc *scn)
{
	if (scn->vaddr_rri_on_ddr)
		qdf_mem_free_consistent(scn->qdf_dev, scn->qdf_dev->dev,
					RRI_ON_DDR_MEM_SIZE,
					scn->vaddr_rri_on_ddr,
					scn->paddr_rri_on_ddr, 0);
	scn->vaddr_rri_on_ddr = NULL;
}
#endif

/**
 * hif_close(): hif_close
 * @hif_ctx: hif_ctx
 *
 * Return: n/a
 */
void hif_close(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	if (!scn) {
		hif_err("hif_opaque_softc is NULL");
		return;
	}

	qdf_ssr_driver_dump_unregister_region("hif");
	hif_desc_history_log_unregister();
	hif_ce_desc_history_log_unregister();
	hif_latency_detect_deinit(scn);

	if (scn->athdiag_procfs_inited) {
		athdiag_procfs_remove();
		scn->athdiag_procfs_inited = false;
	}

	if (scn->target_info.hw_name) {
		char *hw_name = scn->target_info.hw_name;

		scn->target_info.hw_name = "ErrUnloading";
		qdf_mem_free(hw_name);
	}

	hif_uninit_rri_on_ddr(scn);
	hif_cleanup_static_buf_to_target(scn);
	hif_cpuhp_unregister(scn);
	hif_rtpm_lock_deinit(scn);

	hif_bus_close(scn);

	qdf_mem_free(scn);
}

/**
 * hif_get_num_active_grp_tasklets() - get the number of active
 *		datapath group tasklets pending to be completed.
 * @scn: HIF context
 *
 * Returns: the number of datapath group tasklets which are active
 */
static inline int hif_get_num_active_grp_tasklets(struct hif_softc *scn)
{
	return qdf_atomic_read(&scn->active_grp_tasklet_cnt);
}

#if (defined(QCA_WIFI_QCA8074) || defined(QCA_WIFI_QCA6018) || \
	defined(QCA_WIFI_QCA6290) || defined(QCA_WIFI_QCA6390) || \
	defined(QCA_WIFI_QCN9000) || defined(QCA_WIFI_QCA6490) || \
	defined(QCA_WIFI_QCA6750) || defined(QCA_WIFI_QCA5018) || \
	defined(QCA_WIFI_KIWI) || defined(QCA_WIFI_QCN9224) || \
	defined(QCA_WIFI_QCN6432) || \
	defined(QCA_WIFI_QCA9574)) || defined(QCA_WIFI_QCA5332)
/**
 * hif_get_num_pending_work() - get the number of entries in
 *		the workqueue pending to be completed.
 * @scn: HIF context
 *
 * Returns: the number of tasklets which are active
 */
static inline int hif_get_num_pending_work(struct hif_softc *scn)
{
	return hal_get_reg_write_pending_work(scn->hal_soc);
}
#elif defined(FEATURE_HIF_DELAYED_REG_WRITE)
static inline int hif_get_num_pending_work(struct hif_softc *scn)
{
	return qdf_atomic_read(&scn->active_work_cnt);
}
#else

static inline int hif_get_num_pending_work(struct hif_softc *scn)
{
	return 0;
}
#endif

QDF_STATUS hif_try_complete_tasks(struct hif_softc *scn)
{
	uint32_t task_drain_wait_cnt = 0;
	int tasklet = 0, grp_tasklet = 0, work = 0, oom_work = 0;

	while ((tasklet = hif_get_num_active_tasklets(scn)) ||
	       (grp_tasklet = hif_get_num_active_grp_tasklets(scn)) ||
	       (work = hif_get_num_pending_work(scn)) ||
		(oom_work = hif_get_num_active_oom_work(scn))) {
		if (++task_drain_wait_cnt > HIF_TASK_DRAIN_WAIT_CNT) {
			hif_err("pending tasklets %d grp tasklets %d work %d oom work %d",
				tasklet, grp_tasklet, work, oom_work);
			/*
			 * There is chance of OOM thread getting scheduled
			 * continuously or execution get delayed during low
			 * memory state. So avoid panic and prevent suspend
			 * if OOM thread is unable to complete pending
			 * work.
			 */
			if (oom_work)
				hif_err("OOM thread is still pending %d tasklets %d grp tasklets %d work %d",
					oom_work, tasklet, grp_tasklet, work);
			else
				QDF_DEBUG_PANIC("Complete tasks takes more than %u ms: tasklets %d grp tasklets %d work %d oom_work %d",
						HIF_TASK_DRAIN_WAIT_CNT * 10,
						tasklet, grp_tasklet, work,
						oom_work);
			return QDF_STATUS_E_FAULT;
		}
		hif_info("waiting for tasklets %d grp tasklets %d work %d oom_work %d",
			 tasklet, grp_tasklet, work, oom_work);
		msleep(10);
	}

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS hif_try_complete_dp_tasks(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);
	uint32_t task_drain_wait_cnt = 0;
	int grp_tasklet = 0, work = 0;

	while ((grp_tasklet = hif_get_num_active_grp_tasklets(scn)) ||
	       (work = hif_get_num_pending_work(scn))) {
		if (++task_drain_wait_cnt > HIF_TASK_DRAIN_WAIT_CNT) {
			hif_err("pending grp tasklets %d work %d",
				grp_tasklet, work);
			QDF_DEBUG_PANIC("Complete tasks takes more than %u ms: grp tasklets %d work %d",
					HIF_TASK_DRAIN_WAIT_CNT * 10,
					grp_tasklet, work);
			return QDF_STATUS_E_FAULT;
		}
		hif_info("waiting for grp tasklets %d work %d",
			 grp_tasklet, work);
		msleep(10);
	}

	return QDF_STATUS_SUCCESS;
}

#ifdef HIF_HAL_REG_ACCESS_SUPPORT
void hif_reg_window_write(struct hif_softc *scn, uint32_t offset,
			  uint32_t value)
{
	hal_write32_mb(scn->hal_soc, offset, value);
}

uint32_t hif_reg_window_read(struct hif_softc *scn, uint32_t offset)
{
	return hal_read32_mb(scn->hal_soc, offset);
}
#endif

#if defined(HIF_IPCI) && defined(FEATURE_HAL_DELAYED_REG_WRITE)
QDF_STATUS hif_try_prevent_ep_vote_access(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);
	uint32_t work_drain_wait_cnt = 0;
	uint32_t wait_cnt = 0;
	int work = 0;

	qdf_atomic_set(&scn->dp_ep_vote_access,
		       HIF_EP_VOTE_ACCESS_DISABLE);
	qdf_atomic_set(&scn->ep_vote_access,
		       HIF_EP_VOTE_ACCESS_DISABLE);

	while ((work = hif_get_num_pending_work(scn))) {
		if (++work_drain_wait_cnt > HIF_WORK_DRAIN_WAIT_CNT) {
			qdf_atomic_set(&scn->dp_ep_vote_access,
				       HIF_EP_VOTE_ACCESS_ENABLE);
			qdf_atomic_set(&scn->ep_vote_access,
				       HIF_EP_VOTE_ACCESS_ENABLE);
			hif_err("timeout wait for pending work %d ", work);
			return QDF_STATUS_E_FAULT;
		}
		qdf_sleep(10);
	}

	if (pld_is_pci_ep_awake(scn->qdf_dev->dev) == -ENOTSUPP)
	return QDF_STATUS_SUCCESS;

	while (pld_is_pci_ep_awake(scn->qdf_dev->dev)) {
		if (++wait_cnt > HIF_EP_WAKE_RESET_WAIT_CNT) {
			hif_err("Release EP vote is not proceed by Fw");
			return QDF_STATUS_E_FAULT;
		}
		qdf_sleep(5);
	}

	return QDF_STATUS_SUCCESS;
}

void hif_set_ep_intermediate_vote_access(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);
	uint8_t vote_access;

	vote_access = qdf_atomic_read(&scn->ep_vote_access);

	if (vote_access != HIF_EP_VOTE_ACCESS_DISABLE)
		hif_info("EP vote changed from:%u to intermediate state",
			 vote_access);

	if (QDF_IS_STATUS_ERROR(hif_try_prevent_ep_vote_access(hif_ctx)))
		QDF_BUG(0);

	qdf_atomic_set(&scn->ep_vote_access,
		       HIF_EP_VOTE_INTERMEDIATE_ACCESS);
}

void hif_allow_ep_vote_access(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	qdf_atomic_set(&scn->dp_ep_vote_access,
		       HIF_EP_VOTE_ACCESS_ENABLE);
	qdf_atomic_set(&scn->ep_vote_access,
		       HIF_EP_VOTE_ACCESS_ENABLE);
}

void hif_set_ep_vote_access(struct hif_opaque_softc *hif_ctx,
			    uint8_t type, uint8_t access)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	if (type == HIF_EP_VOTE_DP_ACCESS)
		qdf_atomic_set(&scn->dp_ep_vote_access, access);
	else
		qdf_atomic_set(&scn->ep_vote_access, access);
}

uint8_t hif_get_ep_vote_access(struct hif_opaque_softc *hif_ctx,
			       uint8_t type)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	if (type == HIF_EP_VOTE_DP_ACCESS)
		return qdf_atomic_read(&scn->dp_ep_vote_access);
	else
		return qdf_atomic_read(&scn->ep_vote_access);
}
#endif

#ifdef FEATURE_HIF_DELAYED_REG_WRITE
#ifdef MEMORY_DEBUG
#define HIF_REG_WRITE_QUEUE_LEN 128
#else
#define HIF_REG_WRITE_QUEUE_LEN 32
#endif

/**
 * hif_print_reg_write_stats() - Print hif delayed reg write stats
 * @hif_ctx: hif opaque handle
 *
 * Return: None
 */
void hif_print_reg_write_stats(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);
	struct CE_state *ce_state;
	uint32_t *hist;
	int i;

	hist = scn->wstats.sched_delay;
	hif_debug("wstats: enq %u deq %u coal %u direct %u q_depth %u max_q %u sched-delay hist %u %u %u %u",
		  qdf_atomic_read(&scn->wstats.enqueues),
		  scn->wstats.dequeues,
		  qdf_atomic_read(&scn->wstats.coalesces),
		  qdf_atomic_read(&scn->wstats.direct),
		  qdf_atomic_read(&scn->wstats.q_depth),
		  scn->wstats.max_q_depth,
		  hist[HIF_REG_WRITE_SCHED_DELAY_SUB_100us],
		  hist[HIF_REG_WRITE_SCHED_DELAY_SUB_1000us],
		  hist[HIF_REG_WRITE_SCHED_DELAY_SUB_5000us],
		  hist[HIF_REG_WRITE_SCHED_DELAY_GT_5000us]);

	for (i = 0; i < scn->ce_count; i++) {
		ce_state = scn->ce_id_to_state[i];
		if (!ce_state)
			continue;

		hif_debug("ce%d: enq %u deq %u coal %u direct %u",
			  i, ce_state->wstats.enqueues,
			  ce_state->wstats.dequeues,
			  ce_state->wstats.coalesces,
			  ce_state->wstats.direct);
	}
}

/**
 * hif_is_reg_write_tput_level_high() - throughput level for delayed reg writes
 * @scn: hif_softc pointer
 *
 * Return: true if throughput is high, else false.
 */
static inline bool hif_is_reg_write_tput_level_high(struct hif_softc *scn)
{
	int bw_level = hif_get_bandwidth_level(GET_HIF_OPAQUE_HDL(scn));

	return (bw_level >= PLD_BUS_WIDTH_MEDIUM) ? true : false;
}

/**
 * hif_reg_write_fill_sched_delay_hist() - fill reg write delay histogram
 * @scn: hif_softc pointer
 * @delay_us: delay in us
 *
 * Return: None
 */
static inline void hif_reg_write_fill_sched_delay_hist(struct hif_softc *scn,
						       uint64_t delay_us)
{
	uint32_t *hist;

	hist = scn->wstats.sched_delay;

	if (delay_us < 100)
		hist[HIF_REG_WRITE_SCHED_DELAY_SUB_100us]++;
	else if (delay_us < 1000)
		hist[HIF_REG_WRITE_SCHED_DELAY_SUB_1000us]++;
	else if (delay_us < 5000)
		hist[HIF_REG_WRITE_SCHED_DELAY_SUB_5000us]++;
	else
		hist[HIF_REG_WRITE_SCHED_DELAY_GT_5000us]++;
}

/**
 * hif_process_reg_write_q_elem() - process a register write queue element
 * @scn: hif_softc pointer
 * @q_elem: pointer to hal register write queue element
 *
 * Return: The value which was written to the address
 */
static int32_t
hif_process_reg_write_q_elem(struct hif_softc *scn,
			     struct hif_reg_write_q_elem *q_elem)
{
	struct CE_state *ce_state = q_elem->ce_state;
	uint32_t write_val = -1;

	qdf_spin_lock_bh(&ce_state->ce_index_lock);

	ce_state->reg_write_in_progress = false;
	ce_state->wstats.dequeues++;

	if (ce_state->src_ring) {
		q_elem->dequeue_val = ce_state->src_ring->write_index;
		hal_write32_mb(scn->hal_soc, ce_state->ce_wrt_idx_offset,
			       ce_state->src_ring->write_index);
		write_val = ce_state->src_ring->write_index;
	} else if (ce_state->dest_ring) {
		q_elem->dequeue_val = ce_state->dest_ring->write_index;
		hal_write32_mb(scn->hal_soc, ce_state->ce_wrt_idx_offset,
			       ce_state->dest_ring->write_index);
		write_val = ce_state->dest_ring->write_index;
	} else {
		hif_debug("invalid reg write received");
		qdf_assert(0);
	}

	q_elem->valid = 0;
	ce_state->last_dequeue_time = q_elem->dequeue_time;

	qdf_spin_unlock_bh(&ce_state->ce_index_lock);

	return write_val;
}

/**
 * hif_reg_write_work() - Worker to process delayed writes
 * @arg: hif_softc pointer
 *
 * Return: None
 */
static void hif_reg_write_work(void *arg)
{
	struct hif_softc *scn = arg;
	struct hif_reg_write_q_elem *q_elem;
	uint32_t offset;
	uint64_t delta_us;
	int32_t q_depth, write_val;
	uint32_t num_processed = 0;
	int32_t ring_id;

	q_elem = &scn->reg_write_queue[scn->read_idx];
	q_elem->work_scheduled_time = qdf_get_log_timestamp();
	q_elem->cpu_id = qdf_get_cpu();

	/* Make sure q_elem consistent in the memory for multi-cores */
	qdf_rmb();
	if (!q_elem->valid)
		return;

	q_depth = qdf_atomic_read(&scn->wstats.q_depth);
	if (q_depth > scn->wstats.max_q_depth)
		scn->wstats.max_q_depth =  q_depth;

	if (hif_prevent_link_low_power_states(GET_HIF_OPAQUE_HDL(scn))) {
		scn->wstats.prevent_l1_fails++;
		return;
	}

	while (true) {
		qdf_rmb();
		if (!q_elem->valid)
			break;

		qdf_rmb();
		q_elem->dequeue_time = qdf_get_log_timestamp();
		ring_id = q_elem->ce_state->id;
		offset = q_elem->offset;
		delta_us = qdf_log_timestamp_to_usecs(q_elem->dequeue_time -
						      q_elem->enqueue_time);
		hif_reg_write_fill_sched_delay_hist(scn, delta_us);

		scn->wstats.dequeues++;
		qdf_atomic_dec(&scn->wstats.q_depth);

		write_val = hif_process_reg_write_q_elem(scn, q_elem);
		hif_debug("read_idx %u ce_id %d offset 0x%x dequeue_val %d",
			  scn->read_idx, ring_id, offset, write_val);

		qdf_trace_dp_del_reg_write(ring_id, q_elem->enqueue_val,
					   q_elem->dequeue_val,
					   q_elem->enqueue_time,
					   q_elem->dequeue_time);
		num_processed++;
		scn->read_idx = (scn->read_idx + 1) &
					(HIF_REG_WRITE_QUEUE_LEN - 1);
		q_elem = &scn->reg_write_queue[scn->read_idx];
	}

	hif_allow_link_low_power_states(GET_HIF_OPAQUE_HDL(scn));

	/*
	 * Decrement active_work_cnt by the number of elements dequeued after
	 * hif_allow_link_low_power_states.
	 * This makes sure that hif_try_complete_tasks will wait till we make
	 * the bus access in hif_allow_link_low_power_states. This will avoid
	 * race condition between delayed register worker and bus suspend
	 * (system suspend or runtime suspend).
	 *
	 * The following decrement should be done at the end!
	 */
	qdf_atomic_sub(num_processed, &scn->active_work_cnt);
}

static inline void
__hif_flush_delayed_reg_write_work(struct hif_softc *scn)
{
	qdf_flush_work(&scn->reg_write_work);
	qdf_disable_work(&scn->reg_write_work);
}

/**
 * hif_flush_delayed_reg_write_work() - flush pending reg write work
 * @scn: hif_softc pointer
 *
 * Return: None
 */
void hif_flush_delayed_reg_write_work(struct hif_softc *scn)
{
	__hif_flush_delayed_reg_write_work(scn);
}

/**
 * hif_delayed_reg_write_deinit() - De-Initialize delayed reg write processing
 * @scn: hif_softc pointer
 *
 * De-initialize main data structures to process register writes in a delayed
 * workqueue.
 *
 * Return: None
 */
static void hif_delayed_reg_write_deinit(struct hif_softc *scn)
{
	__hif_flush_delayed_reg_write_work(scn);
	qdf_flush_workqueue(0, scn->reg_write_wq);
	qdf_destroy_workqueue(0, scn->reg_write_wq);
	qdf_mem_free(scn->reg_write_queue);
}

/**
 * hif_delayed_reg_write_init() - Initialization function for delayed reg writes
 * @scn: hif_softc pointer
 *
 * Initialize main data structures to process register writes in a delayed
 * workqueue.
 */

static QDF_STATUS hif_delayed_reg_write_init(struct hif_softc *scn)
{
	qdf_atomic_init(&scn->active_work_cnt);
	scn->reg_write_wq =
		qdf_alloc_high_prior_ordered_workqueue("hif_register_write_wq");
	qdf_create_work(0, &scn->reg_write_work, hif_reg_write_work, scn);
	scn->reg_write_queue = qdf_mem_malloc(HIF_REG_WRITE_QUEUE_LEN *
					      sizeof(*scn->reg_write_queue));
	if (!scn->reg_write_queue) {
		hif_err("unable to allocate memory for delayed reg write");
		QDF_BUG(0);
		return QDF_STATUS_E_NOMEM;
	}

	/* Initial value of indices */
	scn->read_idx = 0;
	qdf_atomic_set(&scn->write_idx, -1);

	return QDF_STATUS_SUCCESS;
}

static void hif_reg_write_enqueue(struct hif_softc *scn,
				  struct CE_state *ce_state,
				  uint32_t value)
{
	struct hif_reg_write_q_elem *q_elem;
	uint32_t write_idx;

	if (ce_state->reg_write_in_progress) {
		hif_debug("Already in progress ce_id %d offset 0x%x value %u",
			  ce_state->id, ce_state->ce_wrt_idx_offset, value);
		qdf_atomic_inc(&scn->wstats.coalesces);
		ce_state->wstats.coalesces++;
		return;
	}

	write_idx = qdf_atomic_inc_return(&scn->write_idx);
	write_idx = write_idx & (HIF_REG_WRITE_QUEUE_LEN - 1);

	q_elem = &scn->reg_write_queue[write_idx];
	if (q_elem->valid) {
		hif_err("queue full");
		QDF_BUG(0);
		return;
	}

	qdf_atomic_inc(&scn->wstats.enqueues);
	ce_state->wstats.enqueues++;

	qdf_atomic_inc(&scn->wstats.q_depth);

	q_elem->ce_state = ce_state;
	q_elem->offset = ce_state->ce_wrt_idx_offset;
	q_elem->enqueue_val = value;
	q_elem->enqueue_time = qdf_get_log_timestamp();

	/*
	 * Before the valid flag is set to true, all the other
	 * fields in the q_elem needs to be updated in memory.
	 * Else there is a chance that the dequeuing worker thread
	 * might read stale entries and process incorrect srng.
	 */
	qdf_wmb();
	q_elem->valid = true;

	/*
	 * After all other fields in the q_elem has been updated
	 * in memory successfully, the valid flag needs to be updated
	 * in memory in time too.
	 * Else there is a chance that the dequeuing worker thread
	 * might read stale valid flag and the work will be bypassed
	 * for this round. And if there is no other work scheduled
	 * later, this hal register writing won't be updated any more.
	 */
	qdf_wmb();

	ce_state->reg_write_in_progress  = true;
	qdf_atomic_inc(&scn->active_work_cnt);

	hif_debug("write_idx %u ce_id %d offset 0x%x value %u",
		  write_idx, ce_state->id, ce_state->ce_wrt_idx_offset, value);

	qdf_queue_work(scn->qdf_dev, scn->reg_write_wq,
		       &scn->reg_write_work);
}

void hif_delayed_reg_write(struct hif_softc *scn, uint32_t ctrl_addr,
			   uint32_t val)
{
	struct CE_state *ce_state;
	int ce_id = COPY_ENGINE_ID(ctrl_addr);

	ce_state = scn->ce_id_to_state[ce_id];

	if (!ce_state->htt_tx_data && !ce_state->htt_rx_data) {
		hif_reg_write_enqueue(scn, ce_state, val);
		return;
	}

	if (hif_is_reg_write_tput_level_high(scn) ||
	    (PLD_MHI_STATE_L0 == pld_get_mhi_state(scn->qdf_dev->dev))) {
		hal_write32_mb(scn->hal_soc, ce_state->ce_wrt_idx_offset, val);
		qdf_atomic_inc(&scn->wstats.direct);
		ce_state->wstats.direct++;
	} else {
		hif_reg_write_enqueue(scn, ce_state, val);
	}
}
#else
static inline QDF_STATUS hif_delayed_reg_write_init(struct hif_softc *scn)
{
	return QDF_STATUS_SUCCESS;
}

static inline void  hif_delayed_reg_write_deinit(struct hif_softc *scn)
{
}
#endif

#if defined(QCA_WIFI_WCN6450)
static QDF_STATUS hif_hal_attach(struct hif_softc *scn)
{
	scn->hal_soc = hal_attach(hif_softc_to_hif_opaque_softc(scn),
				  scn->qdf_dev);
	if (!scn->hal_soc)
		return QDF_STATUS_E_FAILURE;

	return QDF_STATUS_SUCCESS;
}

static QDF_STATUS hif_hal_detach(struct hif_softc *scn)
{
	hal_detach(scn->hal_soc);
	scn->hal_soc = NULL;

	return QDF_STATUS_SUCCESS;
}
#elif (defined(QCA_WIFI_QCA8074) || defined(QCA_WIFI_QCA6018) || \
	defined(QCA_WIFI_QCA6290) || defined(QCA_WIFI_QCA6390) || \
	defined(QCA_WIFI_QCN9000) || defined(QCA_WIFI_QCA6490) || \
	defined(QCA_WIFI_QCA6750) || defined(QCA_WIFI_QCA5018) || \
	defined(QCA_WIFI_KIWI) || defined(QCA_WIFI_QCN9224) || \
	defined(QCA_WIFI_QCA9574)) || defined(QCA_WIFI_QCA5332)
static QDF_STATUS hif_hal_attach(struct hif_softc *scn)
{
	if (ce_srng_based(scn)) {
		scn->hal_soc = hal_attach(
					hif_softc_to_hif_opaque_softc(scn),
					scn->qdf_dev);
		if (!scn->hal_soc)
			return QDF_STATUS_E_FAILURE;
	}

	return QDF_STATUS_SUCCESS;
}

static QDF_STATUS hif_hal_detach(struct hif_softc *scn)
{
	if (ce_srng_based(scn)) {
		hal_detach(scn->hal_soc);
		scn->hal_soc = NULL;
	}

	return QDF_STATUS_SUCCESS;
}
#else
static QDF_STATUS hif_hal_attach(struct hif_softc *scn)
{
	return QDF_STATUS_SUCCESS;
}

static QDF_STATUS hif_hal_detach(struct hif_softc *scn)
{
	return QDF_STATUS_SUCCESS;
}
#endif

int hif_init_dma_mask(struct device *dev, enum qdf_bus_type bus_type)
{
	int ret;

	switch (bus_type) {
	case QDF_BUS_TYPE_IPCI:
		ret = qdf_set_dma_coherent_mask(dev,
						DMA_COHERENT_MASK_DEFAULT);
		if (ret) {
			hif_err("Failed to set dma mask error = %d", ret);
			return ret;
		}

		break;
	default:
		/* Follow the existing sequence for other targets */
		break;
	}

	return 0;
}

/**
 * hif_enable(): hif_enable
 * @hif_ctx: hif_ctx
 * @dev: dev
 * @bdev: bus dev
 * @bid: bus ID
 * @bus_type: bus type
 * @type: enable type
 *
 * Return: QDF_STATUS
 */
QDF_STATUS hif_enable(struct hif_opaque_softc *hif_ctx, struct device *dev,
					  void *bdev,
					  const struct hif_bus_id *bid,
					  enum qdf_bus_type bus_type,
					  enum hif_enable_type type)
{
	QDF_STATUS status;
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	if (!scn) {
		hif_err("hif_ctx = NULL");
		return QDF_STATUS_E_NULL_VALUE;
	}

	status = hif_enable_bus(scn, dev, bdev, bid, type);
	if (status != QDF_STATUS_SUCCESS) {
		hif_err("hif_enable_bus error = %d", status);
		return status;
	}

	status = hif_hal_attach(scn);
	if (status != QDF_STATUS_SUCCESS) {
		hif_err("hal attach failed");
		goto disable_bus;
	}

	if (hif_delayed_reg_write_init(scn) != QDF_STATUS_SUCCESS) {
		hif_err("unable to initialize delayed reg write");
		goto hal_detach;
	}

	if (hif_bus_configure(scn)) {
		hif_err("Target probe failed");
		status = QDF_STATUS_E_FAILURE;
		goto free_delayed_reg_mem;
	}

	hif_ut_suspend_init(scn);
	hif_register_recovery_notifier(scn);
	hif_latency_detect_timer_start(hif_ctx);

	/*
	 * Flag to avoid potential unallocated memory access from MSI
	 * interrupt handler which could get scheduled as soon as MSI
	 * is enabled, i.e to take care of the race due to the order
	 * in where MSI is enabled before the memory, that will be
	 * in interrupt handlers, is allocated.
	 */

	scn->hif_init_done = true;

	hif_debug("OK");

	return QDF_STATUS_SUCCESS;

free_delayed_reg_mem:
	hif_delayed_reg_write_deinit(scn);
hal_detach:
	hif_hal_detach(scn);
disable_bus:
	hif_disable_bus(scn);
	return status;
}

void hif_disable(struct hif_opaque_softc *hif_ctx, enum hif_disable_type type)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	if (!scn)
		return;

	hif_delayed_reg_write_deinit(scn);
	hif_set_enable_detection(hif_ctx, false);
	hif_latency_detect_timer_stop(hif_ctx);

	hif_unregister_recovery_notifier(scn);

	hif_nointrs(scn);
	if (scn->hif_init_done == false)
		hif_shutdown_device(hif_ctx);
	else
		hif_stop(hif_ctx);

	hif_hal_detach(scn);

	hif_disable_bus(scn);

	hif_wlan_disable(scn);

	scn->notice_send = false;

	hif_debug("X");
}

#ifdef CE_TASKLET_DEBUG_ENABLE
void hif_enable_ce_latency_stats(struct hif_opaque_softc *hif_ctx, uint8_t val)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	if (!scn)
		return;

	scn->ce_latency_stats = val;
}
#endif

void hif_display_stats(struct hif_opaque_softc *hif_ctx)
{
	hif_display_bus_stats(hif_ctx);
}

qdf_export_symbol(hif_display_stats);

void hif_clear_stats(struct hif_opaque_softc *hif_ctx)
{
	hif_clear_bus_stats(hif_ctx);
}

/**
 * hif_crash_shutdown_dump_bus_register() - dump bus registers
 * @hif_ctx: hif_ctx
 *
 * Return: n/a
 */
#if defined(TARGET_RAMDUMP_AFTER_KERNEL_PANIC) && defined(WLAN_FEATURE_BMI)

static void hif_crash_shutdown_dump_bus_register(void *hif_ctx)
{
	struct hif_opaque_softc *scn = hif_ctx;

	if (hif_check_soc_status(scn))
		return;

	if (hif_dump_registers(scn))
		hif_err("Failed to dump bus registers!");
}

/**
 * hif_crash_shutdown(): hif_crash_shutdown
 *
 * This function is called by the platform driver to dump CE registers
 *
 * @hif_ctx: hif_ctx
 *
 * Return: n/a
 */
void hif_crash_shutdown(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	if (!hif_ctx)
		return;

	if (scn->bus_type == QDF_BUS_TYPE_SNOC) {
		hif_warn("RAM dump disabled for bustype %d", scn->bus_type);
		return;
	}

	if (TARGET_STATUS_RESET == scn->target_status) {
		hif_warn("Target is already asserted, ignore!");
		return;
	}

	if (hif_is_load_or_unload_in_progress(scn)) {
		hif_err("Load/unload is in progress, ignore!");
		return;
	}

	hif_crash_shutdown_dump_bus_register(hif_ctx);
	hif_set_target_status(hif_ctx, TARGET_STATUS_RESET);

	if (ol_copy_ramdump(hif_ctx))
		goto out;

	hif_info("RAM dump collecting completed!");

out:
	return;
}
#else
void hif_crash_shutdown(struct hif_opaque_softc *hif_ctx)
{
	hif_debug("Collecting target RAM dump disabled");
}
#endif /* TARGET_RAMDUMP_AFTER_KERNEL_PANIC */

#ifdef QCA_WIFI_3_0
/**
 * hif_check_fw_reg(): hif_check_fw_reg
 * @scn: scn
 *
 * Return: int
 */
int hif_check_fw_reg(struct hif_opaque_softc *scn)
{
	return 0;
}
#endif

/**
 * hif_read_phy_mem_base(): hif_read_phy_mem_base
 * @scn: scn
 * @phy_mem_base: physical mem base
 *
 * Return: n/a
 */
void hif_read_phy_mem_base(struct hif_softc *scn, qdf_dma_addr_t *phy_mem_base)
{
	*phy_mem_base = scn->mem_pa;
}
qdf_export_symbol(hif_read_phy_mem_base);

/**
 * hif_get_device_type(): hif_get_device_type
 * @device_id: device_id
 * @revision_id: revision_id
 * @hif_type: returned hif_type
 * @target_type: returned target_type
 *
 * Return: int
 */
int hif_get_device_type(uint32_t device_id,
			uint32_t revision_id,
			uint32_t *hif_type, uint32_t *target_type)
{
	int ret = 0;

	switch (device_id) {
	case ADRASTEA_DEVICE_ID_P2_E12:

		*hif_type = HIF_TYPE_ADRASTEA;
		*target_type = TARGET_TYPE_ADRASTEA;
		break;

	case AR9888_DEVICE_ID:
		*hif_type = HIF_TYPE_AR9888;
		*target_type = TARGET_TYPE_AR9888;
		break;

	case AR6320_DEVICE_ID:
		switch (revision_id) {
		case AR6320_FW_1_1:
		case AR6320_FW_1_3:
			*hif_type = HIF_TYPE_AR6320;
			*target_type = TARGET_TYPE_AR6320;
			break;

		case AR6320_FW_2_0:
		case AR6320_FW_3_0:
		case AR6320_FW_3_2:
			*hif_type = HIF_TYPE_AR6320V2;
			*target_type = TARGET_TYPE_AR6320V2;
			break;

		default:
			hif_err("dev_id = 0x%x, rev_id = 0x%x",
				device_id, revision_id);
			ret = -ENODEV;
			goto end;
		}
		break;

	case AR9887_DEVICE_ID:
		*hif_type = HIF_TYPE_AR9888;
		*target_type = TARGET_TYPE_AR9888;
		hif_info(" *********** AR9887 **************");
		break;

	case QCA9984_DEVICE_ID:
		*hif_type = HIF_TYPE_QCA9984;
		*target_type = TARGET_TYPE_QCA9984;
		hif_info(" *********** QCA9984 *************");
		break;

	case QCA9888_DEVICE_ID:
		*hif_type = HIF_TYPE_QCA9888;
		*target_type = TARGET_TYPE_QCA9888;
		hif_info(" *********** QCA9888 *************");
		break;

	case AR900B_DEVICE_ID:
		*hif_type = HIF_TYPE_AR900B;
		*target_type = TARGET_TYPE_AR900B;
		hif_info(" *********** AR900B *************");
		break;

	case QCA8074_DEVICE_ID:
		*hif_type = HIF_TYPE_QCA8074;
		*target_type = TARGET_TYPE_QCA8074;
		hif_info(" *********** QCA8074  *************");
		break;

	case QCA6290_EMULATION_DEVICE_ID:
	case QCA6290_DEVICE_ID:
		*hif_type = HIF_TYPE_QCA6290;
		*target_type = TARGET_TYPE_QCA6290;
		hif_info(" *********** QCA6290EMU *************");
		break;

	case QCN9000_DEVICE_ID:
		*hif_type = HIF_TYPE_QCN9000;
		*target_type = TARGET_TYPE_QCN9000;
		hif_info(" *********** QCN9000 *************");
		break;

	case QCN9224_DEVICE_ID:
		*hif_type = HIF_TYPE_QCN9224;
		*target_type = TARGET_TYPE_QCN9224;
		hif_info(" *********** QCN9224 *************");
		break;

	case QCN6122_DEVICE_ID:
		*hif_type = HIF_TYPE_QCN6122;
		*target_type = TARGET_TYPE_QCN6122;
		hif_info(" *********** QCN6122 *************");
		break;

	case QCN9160_DEVICE_ID:
		*hif_type = HIF_TYPE_QCN9160;
		*target_type = TARGET_TYPE_QCN9160;
		hif_info(" *********** QCN9160 *************");
		break;

	case QCN6432_DEVICE_ID:
		*hif_type = HIF_TYPE_QCN6432;
		*target_type = TARGET_TYPE_QCN6432;
		hif_info(" *********** QCN6432 *************");
		break;

	case QCN7605_DEVICE_ID:
	case QCN7605_COMPOSITE:
	case QCN7605_STANDALONE:
	case QCN7605_STANDALONE_V2:
	case QCN7605_COMPOSITE_V2:
		*hif_type = HIF_TYPE_QCN7605;
		*target_type = TARGET_TYPE_QCN7605;
		hif_info(" *********** QCN7605 *************");
		break;

	case QCA6390_DEVICE_ID:
	case QCA6390_EMULATION_DEVICE_ID:
		*hif_type = HIF_TYPE_QCA6390;
		*target_type = TARGET_TYPE_QCA6390;
		hif_info(" *********** QCA6390 *************");
		break;

	case QCA6490_DEVICE_ID:
	case QCA6490_EMULATION_DEVICE_ID:
		*hif_type = HIF_TYPE_QCA6490;
		*target_type = TARGET_TYPE_QCA6490;
		hif_info(" *********** QCA6490 *************");
		break;

	case QCA6750_DEVICE_ID:
	case QCA6750_EMULATION_DEVICE_ID:
		*hif_type = HIF_TYPE_QCA6750;
		*target_type = TARGET_TYPE_QCA6750;
		hif_info(" *********** QCA6750 *************");
		break;

	case KIWI_DEVICE_ID:
		*hif_type = HIF_TYPE_KIWI;
		*target_type = TARGET_TYPE_KIWI;
		hif_info(" *********** KIWI *************");
		break;

	case MANGO_DEVICE_ID:
		*hif_type = HIF_TYPE_MANGO;
		*target_type = TARGET_TYPE_MANGO;
		hif_info(" *********** MANGO *************");
		break;

	case PEACH_DEVICE_ID:
		*hif_type = HIF_TYPE_PEACH;
		*target_type = TARGET_TYPE_PEACH;
		hif_info(" *********** PEACH *************");
		break;

	case QCA8074V2_DEVICE_ID:
		*hif_type = HIF_TYPE_QCA8074V2;
		*target_type = TARGET_TYPE_QCA8074V2;
		hif_info(" *********** QCA8074V2 *************");
		break;

	case QCA6018_DEVICE_ID:
	case RUMIM2M_DEVICE_ID_NODE0:
	case RUMIM2M_DEVICE_ID_NODE1:
	case RUMIM2M_DEVICE_ID_NODE2:
	case RUMIM2M_DEVICE_ID_NODE3:
	case RUMIM2M_DEVICE_ID_NODE4:
	case RUMIM2M_DEVICE_ID_NODE5:
		*hif_type = HIF_TYPE_QCA6018;
		*target_type = TARGET_TYPE_QCA6018;
		hif_info(" *********** QCA6018 *************");
		break;

	case QCA5018_DEVICE_ID:
		*hif_type = HIF_TYPE_QCA5018;
		*target_type = TARGET_TYPE_QCA5018;
		hif_info(" *********** qca5018 *************");
		break;

	case QCA5332_DEVICE_ID:
		*hif_type = HIF_TYPE_QCA5332;
		*target_type = TARGET_TYPE_QCA5332;
		hif_info(" *********** QCA5332 *************");
		break;

	case QCA9574_DEVICE_ID:
		*hif_type = HIF_TYPE_QCA9574;
		*target_type = TARGET_TYPE_QCA9574;
		hif_info(" *********** QCA9574 *************");
		break;

	case WCN6450_DEVICE_ID:
		*hif_type = HIF_TYPE_WCN6450;
		*target_type = TARGET_TYPE_WCN6450;
		hif_info(" *********** WCN6450 *************");
		break;

	default:
		hif_err("Unsupported device ID = 0x%x!", device_id);
		ret = -ENODEV;
		break;
	}

	if (*target_type == TARGET_TYPE_UNKNOWN) {
		hif_err("Unsupported target_type!");
		ret = -ENODEV;
	}
end:
	return ret;
}

/**
 * hif_get_bus_type() - return the bus type
 * @hif_hdl: HIF Context
 *
 * Return: enum qdf_bus_type
 */
enum qdf_bus_type hif_get_bus_type(struct hif_opaque_softc *hif_hdl)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_hdl);

	return scn->bus_type;
}

/*
 * Target info and ini parameters are global to the driver
 * Hence these structures are exposed to all the modules in
 * the driver and they don't need to maintains multiple copies
 * of the same info, instead get the handle from hif and
 * modify them in hif
 */

/**
 * hif_get_ini_handle() - API to get hif_config_param handle
 * @hif_ctx: HIF Context
 *
 * Return: pointer to hif_config_info
 */
struct hif_config_info *hif_get_ini_handle(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *sc = HIF_GET_SOFTC(hif_ctx);

	return &sc->hif_config;
}

/**
 * hif_get_target_info_handle() - API to get hif_target_info handle
 * @hif_ctx: HIF context
 *
 * Return: Pointer to hif_target_info
 */
struct hif_target_info *hif_get_target_info_handle(
					struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *sc = HIF_GET_SOFTC(hif_ctx);

	return &sc->target_info;

}
qdf_export_symbol(hif_get_target_info_handle);

#ifdef RECEIVE_OFFLOAD
void hif_offld_flush_cb_register(struct hif_opaque_softc *scn,
				 void (offld_flush_handler)(void *))
{
	if (hif_napi_enabled(scn, -1))
		hif_napi_rx_offld_flush_cb_register(scn, offld_flush_handler);
	else
		hif_err("NAPI not enabled");
}
qdf_export_symbol(hif_offld_flush_cb_register);

void hif_offld_flush_cb_deregister(struct hif_opaque_softc *scn)
{
	if (hif_napi_enabled(scn, -1))
		hif_napi_rx_offld_flush_cb_deregister(scn);
	else
		hif_err("NAPI not enabled");
}
qdf_export_symbol(hif_offld_flush_cb_deregister);

int hif_get_rx_ctx_id(int ctx_id, struct hif_opaque_softc *hif_hdl)
{
	if (hif_napi_enabled(hif_hdl, -1))
		return NAPI_PIPE2ID(ctx_id);
	else
		return ctx_id;
}
#else /* RECEIVE_OFFLOAD */
int hif_get_rx_ctx_id(int ctx_id, struct hif_opaque_softc *hif_hdl)
{
	return 0;
}
qdf_export_symbol(hif_get_rx_ctx_id);
#endif /* RECEIVE_OFFLOAD */

#if defined(FEATURE_LRO)

/**
 * hif_get_lro_info - Returns LRO instance for instance ID
 * @ctx_id: LRO instance ID
 * @hif_hdl: HIF Context
 *
 * Return: Pointer to LRO instance.
 */
void *hif_get_lro_info(int ctx_id, struct hif_opaque_softc *hif_hdl)
{
	void *data;

	if (hif_napi_enabled(hif_hdl, -1))
		data = hif_napi_get_lro_info(hif_hdl, ctx_id);
	else
		data = hif_ce_get_lro_ctx(hif_hdl, ctx_id);

	return data;
}
#endif

/**
 * hif_get_target_status - API to get target status
 * @hif_ctx: HIF Context
 *
 * Return: enum hif_target_status
 */
enum hif_target_status hif_get_target_status(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	return scn->target_status;
}
qdf_export_symbol(hif_get_target_status);

/**
 * hif_set_target_status() - API to set target status
 * @hif_ctx: HIF Context
 * @status: Target Status
 *
 * Return: void
 */
void hif_set_target_status(struct hif_opaque_softc *hif_ctx, enum
			   hif_target_status status)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	scn->target_status = status;
}

/**
 * hif_init_ini_config() - API to initialize HIF configuration parameters
 * @hif_ctx: HIF Context
 * @cfg: HIF Configuration
 *
 * Return: void
 */
void hif_init_ini_config(struct hif_opaque_softc *hif_ctx,
			 struct hif_config_info *cfg)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	qdf_mem_copy(&scn->hif_config, cfg, sizeof(struct hif_config_info));
}

/**
 * hif_get_conparam() - API to get driver mode in HIF
 * @scn: HIF Context
 *
 * Return: driver mode of operation
 */
uint32_t hif_get_conparam(struct hif_softc *scn)
{
	if (!scn)
		return 0;

	return scn->hif_con_param;
}

/**
 * hif_get_callbacks_handle() - API to get callbacks Handle
 * @scn: HIF Context
 *
 * Return: pointer to HIF Callbacks
 */
struct hif_driver_state_callbacks *hif_get_callbacks_handle(
							struct hif_softc *scn)
{
	return &scn->callbacks;
}

/**
 * hif_is_driver_unloading() - API to query upper layers if driver is unloading
 * @scn: HIF Context
 *
 * Return: True/False
 */
bool hif_is_driver_unloading(struct hif_softc *scn)
{
	struct hif_driver_state_callbacks *cbk = hif_get_callbacks_handle(scn);

	if (cbk && cbk->is_driver_unloading)
		return cbk->is_driver_unloading(cbk->context);

	return false;
}

/**
 * hif_is_load_or_unload_in_progress() - API to query upper layers if
 * load/unload in progress
 * @scn: HIF Context
 *
 * Return: True/False
 */
bool hif_is_load_or_unload_in_progress(struct hif_softc *scn)
{
	struct hif_driver_state_callbacks *cbk = hif_get_callbacks_handle(scn);

	if (cbk && cbk->is_load_unload_in_progress)
		return cbk->is_load_unload_in_progress(cbk->context);

	return false;
}

/**
 * hif_is_recovery_in_progress() - API to query upper layers if recovery in
 * progress
 * @scn: HIF Context
 *
 * Return: True/False
 */
bool hif_is_recovery_in_progress(struct hif_softc *scn)
{
	struct hif_driver_state_callbacks *cbk = hif_get_callbacks_handle(scn);

	if (cbk && cbk->is_recovery_in_progress)
		return cbk->is_recovery_in_progress(cbk->context);

	return false;
}

#if defined(HIF_PCI) || defined(HIF_SNOC) || defined(HIF_AHB) || \
    defined(HIF_IPCI)

/**
 * hif_update_pipe_callback() - API to register pipe specific callbacks
 * @osc: Opaque softc
 * @pipeid: pipe id
 * @callbacks: callbacks to register
 *
 * Return: void
 */

void hif_update_pipe_callback(struct hif_opaque_softc *osc,
					u_int8_t pipeid,
					struct hif_msg_callbacks *callbacks)
{
	struct hif_softc *scn = HIF_GET_SOFTC(osc);
	struct HIF_CE_state *hif_state = HIF_GET_CE_STATE(scn);
	struct HIF_CE_pipe_info *pipe_info;

	QDF_BUG(pipeid < CE_COUNT_MAX);

	hif_debug("pipeid: %d", pipeid);

	pipe_info = &hif_state->pipe_info[pipeid];

	qdf_mem_copy(&pipe_info->pipe_callbacks,
			callbacks, sizeof(pipe_info->pipe_callbacks));
}
qdf_export_symbol(hif_update_pipe_callback);

/**
 * hif_is_target_ready() - API to query if target is in ready state
 * progress
 * @scn: HIF Context
 *
 * Return: True/False
 */
bool hif_is_target_ready(struct hif_softc *scn)
{
	struct hif_driver_state_callbacks *cbk = hif_get_callbacks_handle(scn);

	if (cbk && cbk->is_target_ready)
		return cbk->is_target_ready(cbk->context);
	/*
	 * if callback is not registered then there is no way to determine
	 * if target is ready. In-such case return true to indicate that
	 * target is ready.
	 */
	return true;
}
qdf_export_symbol(hif_is_target_ready);

int hif_get_bandwidth_level(struct hif_opaque_softc *hif_handle)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_handle);
	struct hif_driver_state_callbacks *cbk = hif_get_callbacks_handle(scn);

	if (cbk && cbk->get_bandwidth_level)
		return cbk->get_bandwidth_level(cbk->context);

	return 0;
}

qdf_export_symbol(hif_get_bandwidth_level);

#ifdef DP_MEM_PRE_ALLOC
void *hif_mem_alloc_consistent_unaligned(struct hif_softc *scn,
					 qdf_size_t size,
					 qdf_dma_addr_t *paddr,
					 uint32_t ring_type,
					 uint8_t *is_mem_prealloc)
{
	void *vaddr = NULL;
	struct hif_driver_state_callbacks *cbk =
				hif_get_callbacks_handle(scn);

	*is_mem_prealloc = false;
	if (cbk && cbk->prealloc_get_consistent_mem_unaligned) {
		vaddr = cbk->prealloc_get_consistent_mem_unaligned(size,
								   paddr,
								   ring_type);
		if (vaddr) {
			*is_mem_prealloc = true;
			goto end;
		}
	}

	vaddr = qdf_mem_alloc_consistent(scn->qdf_dev,
					 scn->qdf_dev->dev,
					 size,
					 paddr);
end:
	dp_info("%s va_unaligned %pK pa_unaligned %pK size %d ring_type %d",
		*is_mem_prealloc ? "pre-alloc" : "dynamic-alloc", vaddr,
		(void *)*paddr, (int)size, ring_type);

	return vaddr;
}

void hif_mem_free_consistent_unaligned(struct hif_softc *scn,
				       qdf_size_t size,
				       void *vaddr,
				       qdf_dma_addr_t paddr,
				       qdf_dma_context_t memctx,
				       uint8_t is_mem_prealloc)
{
	struct hif_driver_state_callbacks *cbk =
				hif_get_callbacks_handle(scn);

	if (is_mem_prealloc) {
		if (cbk && cbk->prealloc_put_consistent_mem_unaligned) {
			cbk->prealloc_put_consistent_mem_unaligned(vaddr);
		} else {
			dp_warn("dp_prealloc_put_consistent_unligned NULL");
			QDF_BUG(0);
		}
	} else {
		qdf_mem_free_consistent(scn->qdf_dev, scn->qdf_dev->dev,
					size, vaddr, paddr, memctx);
	}
}

void hif_prealloc_get_multi_pages(struct hif_softc *scn, uint32_t desc_type,
				  qdf_size_t elem_size, uint16_t elem_num,
				  struct qdf_mem_multi_page_t *pages,
				  bool cacheable)
{
	struct hif_driver_state_callbacks *cbk =
			hif_get_callbacks_handle(scn);

	if (cbk && cbk->prealloc_get_multi_pages)
		cbk->prealloc_get_multi_pages(desc_type, elem_size, elem_num,
					      pages, cacheable);

	if (!pages->num_pages)
		qdf_mem_multi_pages_alloc(scn->qdf_dev, pages,
					  elem_size, elem_num, 0, cacheable);
}

void hif_prealloc_put_multi_pages(struct hif_softc *scn, uint32_t desc_type,
				  struct qdf_mem_multi_page_t *pages,
				  bool cacheable)
{
	struct hif_driver_state_callbacks *cbk =
			hif_get_callbacks_handle(scn);

	if (cbk && cbk->prealloc_put_multi_pages &&
	    pages->is_mem_prealloc)
		cbk->prealloc_put_multi_pages(desc_type, pages);

	if (!pages->is_mem_prealloc)
		qdf_mem_multi_pages_free(scn->qdf_dev, pages, 0,
					 cacheable);
}
#endif

/**
 * hif_batch_send() - API to access hif specific function
 * ce_batch_send.
 * @osc: HIF Context
 * @msdu: list of msdus to be sent
 * @transfer_id: transfer id
 * @len: downloaded length
 * @sendhead:
 *
 * Return: list of msds not sent
 */
qdf_nbuf_t hif_batch_send(struct hif_opaque_softc *osc, qdf_nbuf_t msdu,
		uint32_t transfer_id, u_int32_t len, uint32_t sendhead)
{
	void *ce_tx_hdl = hif_get_ce_handle(osc, CE_HTT_TX_CE);

	if (!ce_tx_hdl)
		return NULL;

	return ce_batch_send((struct CE_handle *)ce_tx_hdl, msdu, transfer_id,
			len, sendhead);
}
qdf_export_symbol(hif_batch_send);

/**
 * hif_update_tx_ring() - API to access hif specific function
 * ce_update_tx_ring.
 * @osc: HIF Context
 * @num_htt_cmpls: number of htt compl received.
 *
 * Return: void
 */
void hif_update_tx_ring(struct hif_opaque_softc *osc, u_int32_t num_htt_cmpls)
{
	void *ce_tx_hdl = hif_get_ce_handle(osc, CE_HTT_TX_CE);

	ce_update_tx_ring(ce_tx_hdl, num_htt_cmpls);
}
qdf_export_symbol(hif_update_tx_ring);


/**
 * hif_send_single() - API to access hif specific function
 * ce_send_single.
 * @osc: HIF Context
 * @msdu : msdu to be sent
 * @transfer_id: transfer id
 * @len : downloaded length
 *
 * Return: msdu sent status
 */
QDF_STATUS hif_send_single(struct hif_opaque_softc *osc, qdf_nbuf_t msdu,
			   uint32_t transfer_id, u_int32_t len)
{
	void *ce_tx_hdl = hif_get_ce_handle(osc, CE_HTT_TX_CE);

	if (!ce_tx_hdl)
		return QDF_STATUS_E_NULL_VALUE;

	return ce_send_single((struct CE_handle *)ce_tx_hdl, msdu, transfer_id,
			len);
}
qdf_export_symbol(hif_send_single);
#endif

/**
 * hif_reg_write() - API to access hif specific function
 * hif_write32_mb.
 * @hif_ctx : HIF Context
 * @offset : offset on which value has to be written
 * @value : value to be written
 *
 * Return: None
 */
void hif_reg_write(struct hif_opaque_softc *hif_ctx, uint32_t offset,
		uint32_t value)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	hif_write32_mb(scn, scn->mem + offset, value);

}
qdf_export_symbol(hif_reg_write);

/**
 * hif_reg_read() - API to access hif specific function
 * hif_read32_mb.
 * @hif_ctx : HIF Context
 * @offset : offset from which value has to be read
 *
 * Return: Read value
 */
uint32_t hif_reg_read(struct hif_opaque_softc *hif_ctx, uint32_t offset)
{

	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	return hif_read32_mb(scn, scn->mem + offset);
}
qdf_export_symbol(hif_reg_read);

/**
 * hif_ramdump_handler(): generic ramdump handler
 * @scn: struct hif_opaque_softc
 *
 * Return: None
 */
void hif_ramdump_handler(struct hif_opaque_softc *scn)
{
	if (hif_get_bus_type(scn) == QDF_BUS_TYPE_USB)
		hif_usb_ramdump_handler(scn);
}

hif_pm_wake_irq_type hif_pm_get_wake_irq_type(struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	return scn->wake_irq_type;
}

irqreturn_t hif_wake_interrupt_handler(int irq, void *context)
{
	struct hif_softc *scn = context;

	hif_info("wake interrupt received on irq %d", irq);

	hif_rtpm_set_monitor_wake_intr(0);
	hif_rtpm_request_resume();

	if (scn->initial_wakeup_cb)
		scn->initial_wakeup_cb(scn->initial_wakeup_priv);

	if (hif_is_ut_suspended(scn))
		hif_ut_fw_resume(scn);

	qdf_pm_system_wakeup();

	return IRQ_HANDLED;
}

void hif_set_initial_wakeup_cb(struct hif_opaque_softc *hif_ctx,
			       void (*callback)(void *),
			       void *priv)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	scn->initial_wakeup_cb = callback;
	scn->initial_wakeup_priv = priv;
}

void hif_set_ce_service_max_yield_time(struct hif_opaque_softc *hif,
				       uint32_t ce_service_max_yield_time)
{
	struct hif_softc *hif_ctx = HIF_GET_SOFTC(hif);

	hif_ctx->ce_service_max_yield_time =
		ce_service_max_yield_time * 1000;
}

unsigned long long
hif_get_ce_service_max_yield_time(struct hif_opaque_softc *hif)
{
	struct hif_softc *hif_ctx = HIF_GET_SOFTC(hif);

	return hif_ctx->ce_service_max_yield_time;
}

void hif_set_ce_service_max_rx_ind_flush(struct hif_opaque_softc *hif,
				       uint8_t ce_service_max_rx_ind_flush)
{
	struct hif_softc *hif_ctx = HIF_GET_SOFTC(hif);

	if (ce_service_max_rx_ind_flush == 0 ||
	    ce_service_max_rx_ind_flush > MSG_FLUSH_NUM)
		hif_ctx->ce_service_max_rx_ind_flush = MSG_FLUSH_NUM;
	else
		hif_ctx->ce_service_max_rx_ind_flush =
						ce_service_max_rx_ind_flush;
}

#ifdef SYSTEM_PM_CHECK
void __hif_system_pm_set_state(struct hif_opaque_softc *hif,
			       enum hif_system_pm_state state)
{
	struct hif_softc *hif_ctx = HIF_GET_SOFTC(hif);

	qdf_atomic_set(&hif_ctx->sys_pm_state, state);
}

int32_t hif_system_pm_get_state(struct hif_opaque_softc *hif)
{
	struct hif_softc *hif_ctx = HIF_GET_SOFTC(hif);

	return qdf_atomic_read(&hif_ctx->sys_pm_state);
}

int hif_system_pm_state_check(struct hif_opaque_softc *hif)
{
	struct hif_softc *hif_ctx = HIF_GET_SOFTC(hif);
	int32_t sys_pm_state;

	if (!hif_ctx) {
		hif_err("hif context is null");
		return -EFAULT;
	}

	sys_pm_state = qdf_atomic_read(&hif_ctx->sys_pm_state);
	if (sys_pm_state == HIF_SYSTEM_PM_STATE_BUS_SUSPENDING ||
	    sys_pm_state == HIF_SYSTEM_PM_STATE_BUS_SUSPENDED) {
		hif_info("Triggering system wakeup");
		qdf_pm_system_wakeup();
		return -EAGAIN;
	}

	return 0;
}
#endif
#ifdef WLAN_FEATURE_AFFINITY_MGR
/*
 * hif_audio_cpu_affinity_allowed() - Check if audio cpu affinity allowed
 *
 * @scn: hif handle
 * @cfg: hif affinity manager configuration for IRQ
 * @audio_taken_cpu: Current CPUs which are taken by audio.
 * @current_time: Current system time.
 *
 * This API checks for 2 conditions
 *  1) Last audio taken mask and current taken mask are different
 *  2) Last time when IRQ was affined away due to audio taken CPUs is
 *     more than time threshold (5 Seconds in current case).
 * If both condition satisfies then only return true.
 *
 * Return: bool: true if it is allowed to affine away audio taken cpus.
 */
static inline bool
hif_audio_cpu_affinity_allowed(struct hif_softc *scn,
			       struct hif_cpu_affinity *cfg,
			       qdf_cpu_mask audio_taken_cpu,
			       uint64_t current_time)
{
	if (!qdf_cpumask_equal(&audio_taken_cpu, &cfg->walt_taken_mask) &&
	    (qdf_log_timestamp_to_usecs(current_time -
			 cfg->last_affined_away)
		< scn->time_threshold))
		return false;
	return true;
}

/*
 * hif_affinity_mgr_check_update_mask() - Check if cpu mask need to be updated
 *
 * @scn: hif handle
 * @cfg: hif affinity manager configuration for IRQ
 * @audio_taken_cpu: Current CPUs which are taken by audio.
 * @cpu_mask: CPU mask which need to be updated.
 * @current_time: Current system time.
 *
 * This API checks if Pro audio use case is running and if cpu_mask need
 * to be updated
 *
 * Return: QDF_STATUS
 */
static inline QDF_STATUS
hif_affinity_mgr_check_update_mask(struct hif_softc *scn,
				   struct hif_cpu_affinity *cfg,
				   qdf_cpu_mask audio_taken_cpu,
				   qdf_cpu_mask *cpu_mask,
				   uint64_t current_time)
{
	qdf_cpu_mask allowed_mask;

	/*
	 * Case 1: audio_taken_mask is empty
	 *   Check if passed cpu_mask and wlan_requested_mask is same or not.
	 *      If both mask are different copy wlan_requested_mask(IRQ affinity
	 *      mask requested by WLAN) to cpu_mask.
	 *
	 * Case 2: audio_taken_mask is not empty
	 *   1. Only allow update if last time when IRQ was affined away due to
	 *      audio taken CPUs is more than 5 seconds or update is requested
	 *      by WLAN
	 *   2. Only allow silver cores to be affined away.
	 *   3. Check if any allowed CPUs for audio use case is set in cpu_mask.
	 *       i. If any CPU mask is set, mask out that CPU from the cpu_mask
	 *       ii. If after masking out audio taken cpu(Silver cores) cpu_mask
	 *           is empty, set mask to all cpu except cpus taken by audio.
	 * Example:
	 *| Audio mask | mask allowed | cpu_mask | WLAN req mask | new cpu_mask|
	 *|  0x00      |       0x00   |   0x0C   |       0x0C    |      0x0C   |
	 *|  0x00      |       0x00   |   0x03   |       0x03    |      0x03   |
	 *|  0x00      |       0x00   |   0xFC   |       0x03    |      0x03   |
	 *|  0x00      |       0x00   |   0x03   |       0x0C    |      0x0C   |
	 *|  0x0F      |       0x03   |   0x0C   |       0x0C    |      0x0C   |
	 *|  0x0F      |       0x03   |   0x03   |       0x03    |      0xFC   |
	 *|  0x03      |       0x03   |   0x0C   |       0x0C    |      0x0C   |
	 *|  0x03      |       0x03   |   0x03   |       0x03    |      0xFC   |
	 *|  0x03      |       0x03   |   0xFC   |       0x03    |      0xFC   |
	 *|  0xF0      |       0x00   |   0x0C   |       0x0C    |      0x0C   |
	 *|  0xF0      |       0x00   |   0x03   |       0x03    |      0x03   |
	 */

	/* Check if audio taken mask is empty*/
	if (qdf_likely(qdf_cpumask_empty(&audio_taken_cpu))) {
		/* If CPU mask requested by WLAN for the IRQ and
		 * cpu_mask passed CPU mask set for IRQ is different
		 * Copy requested mask into cpu_mask and return
		 */
		if (qdf_unlikely(!qdf_cpumask_equal(cpu_mask,
						    &cfg->wlan_requested_mask))) {
			qdf_cpumask_copy(cpu_mask, &cfg->wlan_requested_mask);
			return QDF_STATUS_SUCCESS;
		}
		return QDF_STATUS_E_ALREADY;
	}

	if (!(hif_audio_cpu_affinity_allowed(scn, cfg, audio_taken_cpu,
					     current_time) ||
	      cfg->update_requested))
		return QDF_STATUS_E_AGAIN;

	/* Only allow Silver cores to be affine away */
	qdf_cpumask_and(&allowed_mask, &scn->allowed_mask, &audio_taken_cpu);
	if (qdf_cpumask_intersects(cpu_mask, &allowed_mask)) {
		/* If any of taken CPU(Silver cores) mask is set in cpu_mask,
		 *  mask out the audio taken CPUs from the cpu_mask.
		 */
		qdf_cpumask_andnot(cpu_mask, &cfg->wlan_requested_mask,
				   &allowed_mask);
		/* If cpu_mask is empty set it to all CPUs
		 * except taken by audio(Silver cores)
		 */
		if (qdf_unlikely(qdf_cpumask_empty(cpu_mask)))
			qdf_cpumask_complement(cpu_mask, &allowed_mask);
		return QDF_STATUS_SUCCESS;
	}

	return QDF_STATUS_E_ALREADY;
}

static inline QDF_STATUS
hif_check_and_affine_irq(struct hif_softc *scn, struct hif_cpu_affinity *cfg,
			 qdf_cpu_mask audio_taken_cpu, qdf_cpu_mask cpu_mask,
			 uint64_t current_time)
{
	QDF_STATUS status;

	status = hif_affinity_mgr_check_update_mask(scn, cfg,
						    audio_taken_cpu,
						    &cpu_mask,
						    current_time);
	/* Set IRQ affinity if CPU mask was updated */
	if (QDF_IS_STATUS_SUCCESS(status)) {
		status = hif_irq_set_affinity_hint(cfg->irq,
						   &cpu_mask);
		if (QDF_IS_STATUS_SUCCESS(status)) {
			/* Store audio taken CPU mask */
			qdf_cpumask_copy(&cfg->walt_taken_mask,
					 &audio_taken_cpu);
			/* Store CPU mask which was set for IRQ*/
			qdf_cpumask_copy(&cfg->current_irq_mask,
					 &cpu_mask);
			/* Set time when IRQ affinity was updated */
			cfg->last_updated = current_time;
			if (hif_audio_cpu_affinity_allowed(scn, cfg,
							   audio_taken_cpu,
							   current_time))
				/* If CPU mask was updated due to CPU
				 * taken by audio, update
				 * last_affined_away time
				 */
				cfg->last_affined_away = current_time;
		}
	}

	return status;
}

void hif_affinity_mgr_affine_irq(struct hif_softc *scn)
{
	bool audio_affinity_allowed = false;
	int i, j, ce_id;
	uint64_t current_time;
	char cpu_str[10];
	QDF_STATUS status;
	qdf_cpu_mask cpu_mask, audio_taken_cpu;
	struct HIF_CE_state *hif_state;
	struct hif_exec_context *hif_ext_group;
	struct CE_attr *host_ce_conf;
	struct HIF_CE_state *ce_sc;
	struct hif_cpu_affinity *cfg;

	if (!scn->affinity_mgr_supported)
		return;

	current_time = hif_get_log_timestamp();
	/* Get CPU mask for audio taken CPUs */
	audio_taken_cpu = qdf_walt_get_cpus_taken();

	ce_sc = HIF_GET_CE_STATE(scn);
	host_ce_conf = ce_sc->host_ce_config;
	for (ce_id = 0; ce_id < scn->ce_count; ce_id++) {
		if (host_ce_conf[ce_id].flags & CE_ATTR_DISABLE_INTR)
			continue;
		cfg = &scn->ce_irq_cpu_mask[ce_id];
		qdf_cpumask_copy(&cpu_mask, &cfg->current_irq_mask);
		status =
			hif_check_and_affine_irq(scn, cfg, audio_taken_cpu,
						 cpu_mask, current_time);
		if (QDF_IS_STATUS_SUCCESS(status))
			audio_affinity_allowed = true;
	}

	hif_state = HIF_GET_CE_STATE(scn);
	for (i = 0; i < hif_state->hif_num_extgroup; i++) {
		hif_ext_group = hif_state->hif_ext_group[i];
		for (j = 0; j < hif_ext_group->numirq; j++) {
			cfg = &scn->irq_cpu_mask[hif_ext_group->grp_id][j];
			qdf_cpumask_copy(&cpu_mask, &cfg->current_irq_mask);
			status =
				hif_check_and_affine_irq(scn, cfg, audio_taken_cpu,
							 cpu_mask, current_time);
			if (QDF_IS_STATUS_SUCCESS(status)) {
				qdf_atomic_set(&hif_ext_group->force_napi_complete, -1);
				audio_affinity_allowed = true;
			}
		}
	}
	if (audio_affinity_allowed) {
		qdf_thread_cpumap_print_to_pagebuf(false, cpu_str,
						   &audio_taken_cpu);
		hif_info("Audio taken CPU mask: %s", cpu_str);
	}
}

static inline QDF_STATUS
hif_affinity_mgr_set_irq_affinity(struct hif_softc *scn, uint32_t irq,
				  struct hif_cpu_affinity *cfg,
				  qdf_cpu_mask *cpu_mask)
{
	uint64_t current_time;
	char cpu_str[10];
	QDF_STATUS status, mask_updated;
	qdf_cpu_mask audio_taken_cpu = qdf_walt_get_cpus_taken();

	current_time = hif_get_log_timestamp();
	qdf_cpumask_copy(&cfg->wlan_requested_mask, cpu_mask);
	cfg->update_requested = true;
	mask_updated = hif_affinity_mgr_check_update_mask(scn, cfg,
							  audio_taken_cpu,
							  cpu_mask,
							  current_time);
	status = hif_irq_set_affinity_hint(irq, cpu_mask);
	if (QDF_IS_STATUS_SUCCESS(status)) {
		qdf_cpumask_copy(&cfg->walt_taken_mask, &audio_taken_cpu);
		qdf_cpumask_copy(&cfg->current_irq_mask, cpu_mask);
		if (QDF_IS_STATUS_SUCCESS(mask_updated)) {
			cfg->last_updated = current_time;
			if (hif_audio_cpu_affinity_allowed(scn, cfg,
							   audio_taken_cpu,
							   current_time)) {
				cfg->last_affined_away = current_time;
				qdf_thread_cpumap_print_to_pagebuf(false,
								   cpu_str,
								   &audio_taken_cpu);
				hif_info_rl("Audio taken CPU mask: %s",
					    cpu_str);
			}
		}
	}
	cfg->update_requested = false;
	return status;
}

QDF_STATUS
hif_affinity_mgr_set_qrg_irq_affinity(struct hif_softc *scn, uint32_t irq,
				      uint32_t grp_id, uint32_t irq_index,
				      qdf_cpu_mask *cpu_mask)
{
	struct hif_cpu_affinity *cfg;

	if (!scn->affinity_mgr_supported)
		return hif_irq_set_affinity_hint(irq, cpu_mask);

	cfg = &scn->irq_cpu_mask[grp_id][irq_index];
	return hif_affinity_mgr_set_irq_affinity(scn, irq, cfg, cpu_mask);
}

QDF_STATUS
hif_affinity_mgr_set_ce_irq_affinity(struct hif_softc *scn, uint32_t irq,
				     uint32_t ce_id, qdf_cpu_mask *cpu_mask)
{
	struct hif_cpu_affinity *cfg;

	if (!scn->affinity_mgr_supported)
		return hif_irq_set_affinity_hint(irq, cpu_mask);

	cfg = &scn->ce_irq_cpu_mask[ce_id];
	return hif_affinity_mgr_set_irq_affinity(scn, irq, cfg, cpu_mask);
}

void
hif_affinity_mgr_init_ce_irq(struct hif_softc *scn, int id, int irq)
{
	unsigned int cpus;
	qdf_cpu_mask cpu_mask = {0};
	struct hif_cpu_affinity *cfg = NULL;

	if (!scn->affinity_mgr_supported)
		return;

	/* Set CPU Mask to Silver core */
	qdf_for_each_possible_cpu(cpus)
		if (qdf_topology_physical_package_id(cpus) ==
		    CPU_CLUSTER_TYPE_LITTLE)
			qdf_cpumask_set_cpu(cpus, &cpu_mask);

	cfg = &scn->ce_irq_cpu_mask[id];
	qdf_cpumask_copy(&cfg->current_irq_mask, &cpu_mask);
	qdf_cpumask_copy(&cfg->wlan_requested_mask, &cpu_mask);
	cfg->irq = irq;
	cfg->last_updated = 0;
	cfg->last_affined_away = 0;
	cfg->update_requested = false;
}

void
hif_affinity_mgr_init_grp_irq(struct hif_softc *scn, int grp_id,
			      int irq_num, int irq)
{
	unsigned int cpus;
	qdf_cpu_mask cpu_mask = {0};
	struct hif_cpu_affinity *cfg = NULL;

	if (!scn->affinity_mgr_supported)
		return;

	/* Set CPU Mask to Silver core */
	qdf_for_each_possible_cpu(cpus)
		if (qdf_topology_physical_package_id(cpus) ==
		    CPU_CLUSTER_TYPE_LITTLE)
			qdf_cpumask_set_cpu(cpus, &cpu_mask);

	cfg = &scn->irq_cpu_mask[grp_id][irq_num];
	qdf_cpumask_copy(&cfg->current_irq_mask, &cpu_mask);
	qdf_cpumask_copy(&cfg->wlan_requested_mask, &cpu_mask);
	cfg->irq = irq;
	cfg->last_updated = 0;
	cfg->last_affined_away = 0;
	cfg->update_requested = false;
}
#endif

#if defined(HIF_CPU_PERF_AFFINE_MASK) || \
	defined(FEATURE_ENABLE_CE_DP_IRQ_AFFINE)
void hif_config_irq_set_perf_affinity_hint(
	struct hif_opaque_softc *hif_ctx)
{
	struct hif_softc *scn = HIF_GET_SOFTC(hif_ctx);

	hif_config_irq_affinity(scn);
}

qdf_export_symbol(hif_config_irq_set_perf_affinity_hint);
#endif
