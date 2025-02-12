/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022 MediaTek Inc.
 */

#include <linux/arm-smccc.h>
#include <linux/soc/mediatek/mtk_sip_svc.h>


enum conn_smc_opid {
	/* gps_hw_ops */
	SMC_GPS_COMMON_ON_SET_FLAG_OPID = 0x0,
	SMC_GPS_WAKEUP_CONNINFRA_TOP_OFF_OPID = 0x1,
	SMC_GPS_COMMON_ON_PART1_OPID = 0x2,
	SMC_GPS_COMMON_ON_PART2_OPID = 0x3,
	SMC_GPS_COMMON_ON_PART3_OPID = 0x4,
	SMC_GPS_COMMON_ON_PART4_OPID = 0x5,
	/*adie_on_1: de-assert A-die reset & enable A-die clock hw mode*/
	SMC_GPS_COMMON_ON_CONTROL_ADIE_ON_1_OPID = 0x6,
	/*adie_on_2: no-use*/
	SMC_GPS_COMMON_ON_CONTROL_ADIE_ON_2_OPID = 0x7,
	SMC_GPS_COMMON_ON_FAIL_HANDLER_OPID = 0x8,
	SMC_GPS_COMMON_ON_INNER_FAIL_HANDLER_OPID = 0x9,
	SMC_GPS_COMMON_ON_PART5_OPID = 0xa,
	SMC_GPS_DL_HW_GPS_PWR_STAT_CTRL_OPID = 0xb,
	SMC_GPS_DL_SET_DSP_ON_AND_POLL_ACK_OPID = 0xc,
	SMC_GPS_DL_HW_USRT_CTRL_OPID = 0xd,
	SMC_GPS_DL_SET_CFG_DSP_MEM_AND_DSP_OFF_OPID = 0xe,
	SMC_GPS_COMMON_OFF_PART1_OPID = 0xf,
	SMC_GPS_COMMON_OFF_PART2_OPID = 0x10,
	SMC_GPS_SW_REQUEST_EMI_USAGE_OPID = 0x11,
	SMC_GPS_COMMON_OFF_PART3_OPID = 0x12,

	/* gps_hw_irq */
	SMC_GPS_DL_HW_GET_MCUB_INFO_OPID = 0x13,
	SMC_GPS_DL_HW_CLEAR_MCUB_D2A_FLAG_OPID = 0x14,
	SMC_GPS_DL_HW_POLL_USRT_DSP_RX_EMPTY_OPID = 0x15,
	SMC_GPS_DL_HW_SET_DMA_START_OPID = 0x16,
	SMC_GPS_DL_HW_GET_MCUB_A2D_FLAG_OPID = 0x17,
	SMC_GPS_DL_HW_MCUB_DSP_READ_REQUEST_OPID = 0x18,
	SMC_GPS_DL_HW_USRT_HAS_SET_NODATA_FLAG_OPID = 0x19,
	SMC_GPS_DL_HW_USRT_CLEAR_NODATA_IRQ_OPID = 0x1a,
	SMC_GPS_DL_HW_GET_DMA_LEFT_LEN_OPID = 0x1b,
	SMC_GPS_DL_HW_SET_DMA_STOP_OPID = 0x1c,
	SMC_GPS_DL_HW_USRT_IRQ_ENABLE_OPID = 0x1d,
	SMC_GPS_DL_HW_GET_DMA_INT_STATUS_OPID = 0x1e,
	SMC_GPS_MVCD_GET_DSP_BOOT_UP_INFO = 0x1f,
	SMC_GPS_MVCD_SEND_DSP_FRAGEMENT = 0x20,

#if GPS_DL_HAS_MCUDL
	/* gps_mcudl_hw_ops */
	SMC_GPS_MCUDL_HW_CCIF_IS_TCH_BUSY_OPID = 0x21,
	SMC_GPS_MCUDL_HW_CCIF_SET_TCH_BUSY_OPID = 0x22,
	SMC_GPS_MCUDL_HW_CCIF_SET_TCH_START_OPID = 0x23,
	SMC_GPS_MCUDL_HW_CCIF_GET_TCH_BUSY_BITMASK_OPID = 0x24,
	SMC_GPS_MCUDL_HW_CCIF_CLR_TCH_BUSY_BITMASK_OPID = 0x25,
	SMC_GPS_MCUDL_HW_CCIF_CLR_RCH_BUSY_BITMASK_OPID = 0x26,
	SMC_GPS_MCUDL_HW_CCIF_GET_TCH_START_BITMASK_OPID = 0x27,
	SMC_GPS_MCUDL_HW_CCIF_GET_RCH_BITMASK_OPID = 0x28,
	SMC_GPS_MCUDL_HW_CCIF_SET_RCH_ACK_OPID = 0x29,
	SMC_GPS_MCUDL_HW_CCIF_SET_IRQ_MASK_OPID = 0x2a,
	SMC_GPS_MCUDL_HW_CCIF_GET_IRQ_MASK_OPID = 0x2b,
	SMC_GPS_MCUDL_HW_CCIF_SET_DUMMY_OPID = 0x2c,
	SMC_GPS_MCUDL_HW_CCIF_GET_DUMMY_OPID = 0x2d,
	SMC_GPS_MCUDL_HW_CCIF_GET_SHADOW_OPID = 0x2e,

	SMC_GPS_MCUDL_HW_WAKEUP_GPS_OPID = 0x2f,
	SMC_GPS_MCUDL_CHECK_CONN_INFRA_VER_IS_OK_OPID = 0x30,
	SMC_GPS_MCUDL_POLL_CONN_INFRA_CMBDT_RESTORE_IS_OK_OPID = 0x31,
	SMC_GPS_MCUDL_HW_GPS_SLEEP_PROT_CTRL_CONN2GPS_RX_OPID = 0x32,
	SMC_GPS_MCUDL_HW_GPS_SLEEP_PROT_CTRL_CONN2GPS_TX_OPID = 0x33,
	SMC_GPS_MCUDL_HW_GPS_SLEEP_PROT_CTRL_GPS2CONN_RX_OPID = 0x34,
	SMC_GPS_MCUDL_HW_GPS_SLEEP_PROT_CTRL_GPS2CONN_TX_OPID = 0x35,
	SMC_GPS_MCUDL_HW_GPS_SLEEP_PROT_CTRL_GPS2CONN_AXI_RX_OPID = 0x36,
	SMC_GPS_MCUDL_HW_GPS_SLEEP_PROT_CTRL_GPS2CONN_AXI_TX_OPID = 0x37,
	SMC_GPS_MCUDL_HW_MCU_DO_ON_WITH_RST_HELD_1_OPID = 0x38,
	SMC_GPS_MCUDL_HW_MCU_DO_ON_WITH_RST_HELD_2_OPID = 0x39,
	SMC_GPS_MCUDL_HW_MCU_DO_ON_WITH_RST_HELD_3_OPID = 0x3a,
	SMC_GPS_MCUDL_HW_MCU_DO_ON_WITH_RST_HELD_FAIL_HANDLER_1_OPID = 0x3b,
	SMC_GPS_MCUDL_HW_MCU_DO_ON_WITH_RST_HELD_FAIL_HANDLER_2_OPID = 0x3c,
	SMC_GPS_MCUDL_HW_MCU_ENABLE_CLOCK_OPID = 0x3d,
	SMC_GPS_MCUDL_HW_MCU_WAIT_CLOCK_READY_OPID = 0x3e,
	SMC_GPS_MCUDL_HW_MCU_SET_PLL_OPID = 0x3f,
	SMC_GPS_MCUDL_HW_MCU_RELEASE_RST_OPID = 0x40,
	SMC_GPS_MCUDL_HW_MCU_WAIT_IDLE_LOOP_OPID = 0x41,
	SMC_GPS_MCUDL_HW_MCU_DO_OFF_1_OPID = 0x42,
	SMC_GPS_MCUDL_HW_MCU_SHOW_STATUS_OPID = 0x43,
	SMC_GPS_MCUDL_HW_MCU_SET_OR_CLR_FW_OWN_OPID = 0x44,
	SMC_GPS_DL_HW_SET_MCU_EMI_REMAPPING_TMP_OPID = 0x45,
	SMC_GPS_DL_HW_GET_MCU_EMI_REMAPPING_TMP_OPID = 0x46,
	SMC_GPS_DL_HW_SET_GPS_DYNC_REMAPPING_TMP_OPID = 0x47,
#endif
	SMC_GPS_DL_HW_DEP_SET_HOST_CSR_GPS_DBG_SEL_OPID = 0x48,
	SMC_GPS_DL_HW_DEP_SET_HOST_CSR2BGF_DBG_SEL_OPID = 0x49,
	SMC_GPS_SET_ADIE_CHIPID_TO_ATF_OPID = 0x4a,
	SMC_GPS_DL_GIVE_SEMA_OPID = 0x4b,
	SMC_GPS_DL_COMMON_ENTER_DPSTOP_DSLEEP = 0x4c,
	SMC_GPS_DL_COMMON_LEAVE_DPSTOP_DSLEEP = 0x4d,
	SMC_GPS_DL_COMMON_CLEAR_WAKEUP_SOURCE = 0x4e,
	SMC_GPS_DL_COMMON_ENABLE_ADIE = 0x4f,

#ifdef GPS_DL_ENABLE_MET
	SMC_GPS_DL_HW_DEP_SET_EMI_WRITE_RANGE = 0x60,
	SMC_GPS_DL_HW_DEP_SET_RINGBUFFER_MODE = 0x61,
	SMC_GPS_DL_HW_DEP_SET_SAMPLING_RATE = 0x62,
	SMC_GPS_DL_HW_DEP_SET_MASK_SIGNAL = 0x63,
	SMC_GPS_DL_HW_DEP_SET_EDGE_DETECTION = 0x64,
	SMC_GPS_DL_HW_DEP_SET_EVENT_SIGNAL = 0x65,
	SMC_GPS_DL_HW_DEP_SET_EVENT_SELECT = 0x66,
	SMC_GPS_DL_HW_DEP_ENABLE_MET = 0x67,
	SMC_GPS_DL_HW_DEP_DISABLE_MET = 0x68,
	SMC_GPS_DL_HW_DEP_GET_MET_READ_PTR_ADDR = 0x69,
	SMC_GPS_DL_HW_DEP_GET_MET_WRITE_PTR_ADDR = 0x6a,
	SMC_GPS_DL_HW_DEP_SET_TIMER_SOURCE = 0x6b,
#endif
};

enum gps_dl_hw_mvcd_dsp_type {
	GPS_DL_MVCD_DSP_L1 = 0,
	GPS_DL_MVCD_DSP_L5,
	GPS_DL_MVCD_DSP_L1_CW,
	GPS_DL_MVCD_DSP_L5_CW,
	GPS_DL_MVCD_DSP_RESERVED1,
	GPS_DL_MVCD_DSP_RESERVED2,
};

void gps_dl_hw_may_set_link_power_flag(enum gps_dl_link_id_enum link_id, bool power_ctrl);
bool gps_dl_hw_gps_force_wakeup_conninfra_top_off(bool enable);

