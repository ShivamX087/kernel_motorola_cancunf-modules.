/******************************************************************************
 *
 * This file is provided under a dual license.  When you use or
 * distribute this software, you may choose to be licensed under
 * version 2 of the GNU General Public License ("GPLv2 License")
 * or BSD License.
 *
 * GPLv2 License
 *
 * Copyright(C) 2016 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 *
 * BSD LICENSE
 *
 * Copyright(C) 2016 MediaTek Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************/

/*! \file   "bss.h"
 *    \brief  In this file we define the function prototype used in BSS/IBSS.
 *
 *    The file contains the function declarations and defines
 *						for used in BSS/IBSS.
 */


#ifndef _BSS_H
#define _BSS_H

/*******************************************************************************
 *                         C O M P I L E R   F L A G S
 *******************************************************************************
 */

/*******************************************************************************
 *                    E X T E R N A L   R E F E R E N C E S
 *******************************************************************************
 */
#include "wlan_def.h"
extern const uint8_t *apucNetworkType[NETWORK_TYPE_NUM];

/*******************************************************************************
 *                              C O N S T A N T S
 *******************************************************************************
 */
/* Define how many concurrent operation networks. */
#define BSS_P2P_NUM             KAL_P2P_NUM

#if (BSS_P2P_NUM > MAX_BSSID_NUM)
#error Exceed HW capability (KAL_BSS_NUM or KAL_P2P_NUM)!!
#endif

/* NOTE(Kevin): change define for george */
/* #define MAX_LEN_TIM_PARTIAL_BMP     (((MAX_ASSOC_ID + 1) + 7) / 8) */
/* Required bits = (MAX_ASSOC_ID + 1) */
#define MAX_LEN_TIM_PARTIAL_BMP                     ((CFG_STA_REC_NUM + 7) / 8)
/* reserve length greater than maximum size of STA_REC */
/* obsoleted: Assume we only use AID:1~15 */

/* CTRL FLAGS for Probe Response */
#define BSS_PROBE_RESP_USE_P2P_DEV_ADDR             BIT(0)
#define BSS_PROBE_RESP_INCLUDE_P2P_IE               BIT(1)

#define MAX_BSS_INDEX           HW_BSSID_NUM
#define P2P_DEV_BSS_INDEX       MAX_BSS_INDEX

#define IS_BSS_ALIVE(_prAdapter, _prBssInfo) \
	(_prBssInfo->fgIsInUse && \
	_prBssInfo->fgIsNetActive && \
	(_prBssInfo->eConnectionState == MEDIA_STATE_CONNECTED || \
	(_prBssInfo->eCurrentOPMode == OP_MODE_ACCESS_POINT && \
	IS_NET_PWR_STATE_ACTIVE(_prAdapter, \
	_prBssInfo->ucBssIndex))))

#define IS_BSS_NOT_ALIVE(_prAdapter, _prBssInfo) \
	(!IS_BSS_ALIVE(_prAdapter, _prBssInfo))

/*******************************************************************************
 *                             D A T A   T Y P E S
 *******************************************************************************
 */

/*******************************************************************************
 *                            P U B L I C   D A T A
 *******************************************************************************
 */

/*******************************************************************************
 *                           P R I V A T E   D A T A
 *******************************************************************************
 */

/*******************************************************************************
 *                                 M A C R O S
 *******************************************************************************
 */
#define IS_BSS_INDEX_VALID(_ucBssIndex)     ((_ucBssIndex) <= P2P_DEV_BSS_INDEX)

#define GET_BSS_INFO_BY_INDEX(_prAdapter, _ucBssIndex) \
	(IS_BSS_INDEX_VALID(_ucBssIndex) ? \
		(_prAdapter)->aprBssInfo[(_ucBssIndex)] : NULL)

/*******************************************************************************
 *                   F U N C T I O N   D E C L A R A T I O N S
 *******************************************************************************
 */
/*----------------------------------------------------------------------------*/
/* Routines for all Operation Modes                                           */
/*----------------------------------------------------------------------------*/
uint32_t bssInfoConnType(struct ADAPTER *ad, struct BSS_INFO *bssinfo);

struct STA_RECORD *
bssCreateStaRecFromBssDesc(struct ADAPTER *prAdapter,
			   enum ENUM_STA_TYPE eStaType, uint8_t uBssIndex,
			   struct BSS_DESC *prBssDesc);

void bssComposeNullFrame(struct ADAPTER *prAdapter,
			 uint8_t *pucBuffer, struct STA_RECORD *prStaRec);

void
bssComposeQoSNullFrame(struct ADAPTER *prAdapter,
		       uint8_t *pucBuffer, struct STA_RECORD *prStaRec,
		       uint8_t ucUP, u_int8_t fgSetEOSP);

uint32_t
bssSendNullFrame(struct ADAPTER *prAdapter,
		 struct STA_RECORD *prStaRec,
		 PFN_TX_DONE_HANDLER pfTxDoneHandler);

uint32_t
bssSendQoSNullFrame(struct ADAPTER *prAdapter,
		    struct STA_RECORD *prStaRec, uint8_t ucUP,
		    PFN_TX_DONE_HANDLER pfTxDoneHandler);

void bssDumpBssInfo(struct ADAPTER *prAdapter,
		    uint8_t ucBssIndex);

void bssDetermineApBssInfoPhyTypeSet(struct ADAPTER
				     *prAdapter, u_int8_t fgIsPureAp,
				     struct BSS_INFO *prBssInfo);
int8_t bssGetRxNss(struct ADAPTER *prAdapter,
	struct BSS_DESC *prBssDesc);
#if CFG_SUPPORT_IOT_AP_BLACKLIST
uint32_t bssGetIotApAction(struct ADAPTER *prAdapter,
	struct BSS_DESC *prBssDesc);
#endif
/*----------------------------------------------------------------------------*/
/* Routines for both IBSS(AdHoc) and BSS(AP)                                  */
/*----------------------------------------------------------------------------*/
void bssGenerateExtSuppRate_IE(struct ADAPTER *prAdapter,
			       struct MSDU_INFO *prMsduInfo);

void
bssBuildBeaconProbeRespFrameCommonIEs(struct MSDU_INFO
				      *prMsduInfo,
				      struct BSS_INFO *prBssInfo,
				      uint8_t *pucDestAddr);

void
bssComposeBeaconProbeRespFrameHeaderAndFF(
	uint8_t *pucBuffer,
	uint8_t *pucDestAddr,
	uint8_t *pucOwnMACAddress,
	uint8_t *pucBSSID, uint16_t u2BeaconInterval,
	uint16_t u2CapInfo);

uint32_t
bssSendBeaconProbeResponse(struct ADAPTER *prAdapter,
				uint8_t uBssIndex, uint8_t *pucDestAddr,
				uint32_t u4ControlFlags);

uint32_t bssProcessProbeRequest(struct ADAPTER *prAdapter,
				struct SW_RFB *prSwRfb);

void bssInitializeClientList(struct ADAPTER *prAdapter,
				struct BSS_INFO *prBssInfo);

void bssAddClient(struct ADAPTER *prAdapter,
				struct BSS_INFO *prBssInfo,
				struct STA_RECORD *prStaRec);

u_int8_t bssRemoveClient(struct ADAPTER *prAdapter,
				struct BSS_INFO *prBssInfo,
				struct STA_RECORD *prStaRec);

struct STA_RECORD *bssRemoveClientByMac(struct ADAPTER *prAdapter,
				struct BSS_INFO *prBssInfo,
				uint8_t *pucMac);

struct STA_RECORD *bssGetClientByMac(struct ADAPTER *prAdapter,
				struct BSS_INFO *prBssInfo,
				uint8_t *pucMac);

struct STA_RECORD *bssRemoveHeadClient(struct ADAPTER *prAdapter,
				struct BSS_INFO *prBssInfo);

uint32_t bssGetClientCount(struct ADAPTER *prAdapter,
				struct BSS_INFO *prBssInfo);

void bssDumpClientList(struct ADAPTER *prAdapter,
				struct BSS_INFO *prBssInfo);

void bssCheckClientList(struct ADAPTER *prAdapter,
				struct BSS_INFO *prBssInfo);

/*----------------------------------------------------------------------------*/
/* Routines for IBSS(AdHoc) only                                              */
/*----------------------------------------------------------------------------*/
void
ibssProcessMatchedBeacon(struct ADAPTER *prAdapter,
			 struct BSS_INFO *prBssInfo,
			 struct BSS_DESC *prBssDesc, uint8_t ucRCPI);

uint32_t ibssCheckCapabilityForAdHocMode(
		struct ADAPTER *prAdapter,
		struct BSS_DESC *prBssDesc,
		uint8_t uBssIndex);

void ibssInitForAdHoc(struct ADAPTER *prAdapter,
		      struct BSS_INFO *prBssInfo);

uint32_t bssUpdateBeaconContent(struct ADAPTER
				*prAdapter, uint8_t uBssIndex);

uint32_t bssUpdateBeaconContentEx(struct ADAPTER
				*prAdapter, uint8_t uBssIndex,
				enum ENUM_IE_UPD_METHOD eMethod);

/*----------------------------------------------------------------------------*/
/* Routines for BSS(AP) only                                                  */
/*----------------------------------------------------------------------------*/
void bssInitForAP(struct ADAPTER *prAdapter,
		  struct BSS_INFO *prBssInfo, u_int8_t fgIsRateUpdate);

void bssUpdateDTIMCount(struct ADAPTER *prAdapter,
			uint8_t uBssIndex);

void bssSetTIMBitmap(struct ADAPTER *prAdapter,
		     struct BSS_INFO *prBssInfo, uint16_t u2AssocId);

/*link function to p2p module for txBcnIETable*/

/* WMM-2.2.2 WMM ACI to AC coding */
enum ENUM_ACI {
	ACI_BE = 0,
	ACI_BK = 1,
	ACI_VI = 2,
	ACI_VO = 3,
	ACI_NUM
};

enum ENUM_AC_PRIORITY {
	AC_BK_PRIORITY = 0,
	AC_BE_PRIORITY,
	AC_VI_PRIORITY,
	AC_VO_PRIORITY
};

#if (CFG_SUPPORT_HE_ER == 1)
struct EVENT_ER_TX_MODE {
	uint8_t ucBssInfoIdx;
	uint8_t ucErMode;
};

void bssProcessErTxModeEvent(struct ADAPTER *prAdapter,
	struct WIFI_EVENT *prEvent);
#endif

#endif /* _BSS_H */
