/*
 * Copyright (c) 2018 Motorola Mobility, LLC.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/power_supply.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include "mmi_charger_core.h"

extern struct mmi_charger_ops mtk_pump_charger_ops;
extern struct mmi_charger_ops mtk_pmic_charger_ops;
extern struct mmi_charger_device *mmi_charger_device_register(const char *name,
		const char *psy_name,struct device *parent, void *devdata,
		const struct mmi_charger_ops *ops, struct charger_device *chgdev);
extern void mmi_charger_device_unregister(struct mmi_charger_device* charger_dev);
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
extern int wt6670f_set_voltage(u16 voltage);
extern int wt6670f_set_volt_count(int count);
//extern int wt6670f_get_charger_type(void);
int qc3p_select_pdo(struct mmi_charger_manager *chip, int target_uv, int target_ua, bool sw_reduce_volt);
int get_caculated_real_ibus_vbus(struct mmi_charger_manager *chip, int *ibus_curr, int *calculated_vbus);
#endif

static const struct mmi_chrg_dev_ops dev_ops[] = {
	{
		.dev_name = "pmic-sw",
		.ops = &mtk_pmic_charger_ops,
	},
	{
		.dev_name = "cp-master",
		.ops = &mtk_pump_charger_ops,
	},
	{
		.dev_name = "cp-slave",
		.ops = &mtk_pump_charger_ops,
	},
};

struct mmi_cp_policy_dev {
	bool pmic_sw;
	bool cp_master;
	bool cp_slave;
	bool cp_clave_later;
	struct mmi_charger_device *chrg_dev[CHRG_NUM];
};

typedef enum  {
	PM_STATE_DISCONNECT,
	PM_STATE_ENTRY,
	PM_STATE_SW_ENTRY,
	PM_STATE_SW_LOOP,
	PM_STATE_CHRG_PUMP_ENTRY,
	PM_STATE_SINGLE_CP_ENTRY,
	PM_STATE_DULE_CP_ENTRY,
	PM_STATE_PPS_TUNNING_CURR,
	PM_STATE_PPS_TUNNING_VOLT,
	PM_STATE_CP_CC_LOOP,
	PM_STATE_CP_CV_LOOP,
	PM_STATE_CP_QUIT,
	PM_STATE_RECOVERY_SW,
	PM_STATE_STOP_CHARGE,
	PM_STATE_COOLING_LOOP,
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
	PM_STATE_POWER_LIMIT_LOOP,
#endif
} pm_sm_state_t;

const unsigned char *pm_state_str[] = {
	"PM_STATE_DISCONNECT",
	"PM_STATE_ENTRY",
	"PM_STATE_SW_ENTRY",
	"PM_STATE_SW_LOOP",
	"PM_STATE_CHRG_PUMP_ENTRY",
	"PM_STATE_SINGLE_CP_ENTRY",
	"PM_STATE_DULE_CP_ENTRY",
	"PM_STATE_PPS_TUNNING_CURR",
	"PM_STATE_PPS_TUNNING_VOLT",
	"PM_STATE_CP_CC_LOOP",
	"PM_STATE_CP_CV_LOOP",
	"PM_STATE_CP_QUIT",
	"PM_STATE_RECOVERY_SW",
	"PM_STATE_STOP_CHARGE",
	"PM_STATE_COOLING_LOOP",
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
	"PM_STATE_POWER_LIMIT_LOOP",
#endif
};

static pm_sm_state_t	sm_state = PM_STATE_DISCONNECT;
static pm_sm_state_t    prev_sm_state = PM_STATE_DISCONNECT;
static int chrg_cc_power_tunning_cnt = 0;
static int chrg_cv_taper_tunning_cnt = 0;
static int chrg_cv_delta_volt = 0;
static int quit_slave_chrg_cnt = 0;
static int batt_curr_roof = 0;
static int pd_constant_power_cnt = 0;
static struct mmi_cp_policy_dev g_chrg_list = {0};

static void mmi_chrg_sm_move_state(struct mmi_charger_manager *chip, pm_sm_state_t state)
{
	mmi_chrg_dbg(chip, PR_INTERRUPT, "pm_state change:%s -> %s\n",
		pm_state_str[sm_state], pm_state_str[state]);
	prev_sm_state = sm_state;
	sm_state = state;
	pd_constant_power_cnt = 0;
	batt_curr_roof = 0;
}

static void chrg_dev_init(struct mmi_charger_manager *chip, struct mmi_cp_policy_dev *chrg_list)
{
	int chrg_num, i;
	chrg_num = chip->mmi_chrg_dev_num;
	mmi_chrg_err(chip,"runing in chrg dev init!\n");

	for (i = 0; i < chrg_num; i++) {

		switch (i) {
		case PMIC_SW:
			if (is_charger_exist(dev_ops[PMIC_SW].dev_name)) {
				chrg_list->pmic_sw = true;
				chrg_list->chrg_dev[PMIC_SW] = chip->chrg_list[PMIC_SW];
				}
			break;
		case CP_MASTER:
			if (is_charger_exist(dev_ops[CP_MASTER].dev_name)) {
				chrg_list->cp_master = true;
				chrg_list->chrg_dev[CP_MASTER] = chip->chrg_list[CP_MASTER];
				}
			break;
		case CP_SLAVE:
			if (is_charger_exist(dev_ops[CP_SLAVE].dev_name)) {
				chrg_list->cp_slave = true;
				chrg_list->cp_clave_later = false;
				chrg_list->chrg_dev[CP_SLAVE] = chip->chrg_list[CP_SLAVE];
				}
			break;
		default:
			mmi_chrg_err(chip,"No mmi_chrg_dev found %d !\n",i);
			break;
		}
	}
	return;
}

static void mmi_chrg_dev_init_protect(struct mmi_charger_manager *chip)
{
	struct mmi_cp_policy_dev *chrg_list = &g_chrg_list;
	mmi_chrg_err(chip,"runing in chrg dev init protect!\n");

	if (chrg_list->cp_master) {
		mmi_init_chip(chrg_list->chrg_dev[CP_MASTER]);
	}

	if (chrg_list->cp_slave) {
		mmi_init_chip(chrg_list->chrg_dev[CP_SLAVE]);
	}

	return;
}

static void clear_chrg_dev_error_cnt(struct mmi_charger_manager *chip, struct mmi_cp_policy_dev *chrg_list)
{
	int chrg_num, i;
	struct mmi_charger_device *chrg_dev;
	chrg_num = chip->mmi_chrg_dev_num;

	for (i = 0; i < chrg_num; i++) {
		if (is_charger_exist(dev_ops[i].dev_name)) {
			chrg_dev = chrg_list->chrg_dev[i];
			chrg_dev->charger_error.chrg_err_type = 0;
			chrg_dev->charger_error.bus_ucp_err_cnt = 0;
			chrg_dev->charger_error.bus_ocp_err_cnt = 0;
		}
	}
	return;
}

#define PPS_INIT_VOLT_COMP	500000
#define PPS_VOLT_COMP_DELTA	300000
static void chrg_policy_error_recovery(struct mmi_charger_manager *chip,
										struct mmi_cp_policy_dev *chrg_list)
{
	int chrg_num = 0, i = 0, chrg_error_type= 0;
	struct mmi_charger_device *chrg_dev;
	chrg_num = chip->mmi_chrg_dev_num;

	for (i = 0; i < chrg_num; i++) {

		switch (i) {
		case PMIC_SW:
			break;
		case CP_MASTER:
		if (is_charger_exist(dev_ops[CP_MASTER].dev_name)) {
				chrg_dev = chrg_list->chrg_dev[CP_MASTER];
				chrg_error_type = chrg_dev->charger_error.chrg_err_type;
			if (chrg_error_type & (1 << MMI_BUS_UCP_ALARM_BIT) ||
				chrg_error_type & (1 << MMI_BUS_UCP_FAULT_BIT)) {
				mmi_chrg_info(chip,"CP master bus ucp error %d, cnt %d,"
						"pps volt comp %dmV\n",
						chrg_error_type,
						chrg_dev->charger_error.bus_ucp_err_cnt,
						chip->pps_volt_comp);

				if (chrg_dev->charger_error.bus_ucp_err_cnt > 3) {
					if (chrg_list->cp_slave) {
						chrg_list->cp_slave = false;
						chrg_dev->charger_error.bus_ucp_err_cnt = 0;
						mmi_chrg_sm_move_state(chip,
							PM_STATE_CHRG_PUMP_ENTRY);
					} else {
						chip->recovery_pmic_chrg = true;
						mmi_chrg_sm_move_state(chip,
							PM_STATE_SW_ENTRY);
						chrg_dev->charger_error.bus_ucp_err_cnt = 0;
					}
				} else if (chrg_dev->charger_error.bus_ucp_err_cnt > 6) {
					chip->recovery_pmic_chrg = true;
					mmi_chrg_sm_move_state(chip,
							PM_STATE_SW_ENTRY);
					chrg_dev->charger_error.bus_ucp_err_cnt = 0;
				}

				chrg_dev->charger_error.bus_ucp_err_cnt++;
				chip->pps_volt_comp += PPS_VOLT_COMP_DELTA;
				mmi_chrg_info(chip,"Restart charging, "
						"increase pps volt comp %dmV\n",
						chip->pps_volt_comp);
				mmi_chrg_sm_move_state(chip,
							PM_STATE_CHRG_PUMP_ENTRY);
				mmi_clear_charger_error(chrg_dev);
			}else if (chrg_error_type & (1 << MMI_BUS_OCP_ALARM_BIT) ||
				chrg_error_type & (1 << MMI_BUS_OCP_FAULT_BIT) ||
				chrg_error_type & (1 << MMI_CONV_OCP_FAULT_BIT)) {
				mmi_chrg_info(chip,"CP master ocp error %d, cnt %d,"
					"pps volt comp %dmV\n",
					chrg_error_type,
					chrg_dev->charger_error.bus_ocp_err_cnt,
					chip->pps_volt_comp);
				if (chrg_dev->charger_error.bus_ocp_err_cnt > 3) {
					chip->recovery_pmic_chrg = true;
					mmi_chrg_sm_move_state(chip,
							PM_STATE_SW_ENTRY);
					chrg_dev->charger_error.bus_ocp_err_cnt = 0;
				}
				chrg_dev->charger_error.bus_ocp_err_cnt++;
				chip->pps_volt_comp -= PPS_VOLT_COMP_DELTA;
				if (chip->pps_volt_comp < 0)
					chip->pps_volt_comp = 0;
				mmi_chrg_info(chip,"Restart charging, "
						"decrease pps volt comp %dmV\n",
						chip->pps_volt_comp);
				mmi_chrg_sm_move_state(chip,
							PM_STATE_CHRG_PUMP_ENTRY);
				mmi_clear_charger_error(chrg_dev);
			}
		}
			break;
		case CP_SLAVE:
			break;
		default:
			mmi_chrg_err(chip,"No mmi_chrg_dev found %d !\n",i);
			break;
		}
	}
	return;
}

static void chrg_policy_error_clear(struct mmi_charger_manager *chip,
					struct mmi_cp_policy_dev *chrg_list)
{
	int chrg_num = 0, i = 0;
	struct mmi_charger_device *chrg_dev;
	chrg_num = chip->mmi_chrg_dev_num;

	for (i = 0; i < chrg_num; i++) {
		switch (i) {
			case PMIC_SW:
				break;

			case CP_MASTER:
				if (is_charger_exist(dev_ops[CP_MASTER].dev_name)) {
					chrg_dev = chrg_list->chrg_dev[CP_MASTER];
					mmi_clear_charger_error(chrg_dev);
				}
				break;

			case CP_SLAVE:
				break;

			default:
				mmi_chrg_err(chip,"mmi_chrg_dev not found %d !\n",i);
				break;
		}
	}

	return;
}

void mmi_chrg_enable_all_cp(struct mmi_charger_manager *chip, int val)
{
	struct mmi_cp_policy_dev *chrg_list = &g_chrg_list;
	bool enable = !!val;

	mmi_chrg_dbg(chip, PR_MOTO,"enable all cp = %d\n", enable);
	if (enable) {
		if (chip->cp_disable == false)
			return;

		cancel_delayed_work_sync(&chip->mmi_chrg_sm_work);
		mmi_chrg_sm_move_state(chip, PM_STATE_CHRG_PUMP_ENTRY);
		chip->cp_disable = false;
		schedule_delayed_work(&chip->mmi_chrg_sm_work,
				msecs_to_jiffies(0));

	} else {
		if(chrg_list->cp_master
			&& !chrg_list->chrg_dev[CP_MASTER]->charger_enabled)
			return;

		cancel_delayed_work_sync(&chip->mmi_chrg_sm_work);

		chip->cp_disable = true;
		if (chrg_list->cp_slave
			&& chrg_list->chrg_dev[CP_SLAVE]->charger_enabled) {
			mmi_enable_charging(chrg_list->chrg_dev[CP_SLAVE], false);
		}

		if (chrg_list->cp_master
			&& chrg_list->chrg_dev[CP_MASTER]->charger_enabled) {
			mmi_enable_charging(chrg_list->chrg_dev[CP_MASTER], false);
		}

		mmi_chrg_sm_move_state(chip, PM_STATE_RECOVERY_SW);
		schedule_delayed_work(&chip->mmi_chrg_sm_work,
				msecs_to_jiffies(0));
	}
}

#define HEARTBEAT_SHORT_DELAY_MS 1000
#define HEARTBEAT_lOOP_WAIT_MS 3000
#define HEARTBEAT_PPS_TUNNING_MS 100
#define HEARTBEAT_NEXT_STATE_MS 100
#define HEARTBEAT_CANCEL -1
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
#define CC_CURR_DEBOUNCE 40000
#else
#define CC_CURR_DEBOUNCE 100000
#endif
#define CV_TAPPER_COUNT 3
#define CC_POWER_COUNT 3
#define CV_DELTA_VOLT 100000
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
#define THERMAL_TUNNING_CURR 40000
#else
#define THERMAL_TUNNING_CURR 100000
#endif
#define COOLING_DELTA_POWER 100000
#define COOLING_MAX_CNT 5
#define PPS_SELECT_PDO_RETRY_COUNT 3
#define DISABLE_CHRG_LIMIT_CURR_UA  	4000000
#define CP_CHRG_SOC_LIMIT 90
#define PD_CONT_PWR_CNT 5

#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
#define AICL_DEFALLT_THREHOLD 4500
#define QC3P_V_STEP  20000
#define CURR_TO_VOLT_STEP 5
#define DEFAULT_HVDCP_POWER_MAX 15000
#endif

void mmi_chrg_policy_clear(struct mmi_charger_manager *chip) {
	struct mmi_cp_policy_dev *chrg_list = &g_chrg_list;
	chrg_dev_init(chip, &g_chrg_list);
	clear_chrg_dev_error_cnt(chip, &g_chrg_list);;
	clear_chg_manager(chip);
	if (chrg_list->cp_slave)
		mmi_enable_charging(chrg_list->chrg_dev[CP_SLAVE], false);
	if (chrg_list->cp_master)
		mmi_enable_charging(chrg_list->chrg_dev[CP_MASTER], false);
	mmi_set_charing_current_limit(chrg_list->chrg_dev[PMIC_SW],false,
							DISABLE_CHRG_LIMIT_CURR_UA);
	chrg_list->chrg_dev[PMIC_SW]->charger_limited = false;
	sm_state = PM_STATE_DISCONNECT;
	prev_sm_state = PM_STATE_DISCONNECT;
	chip->pps_volt_comp = PPS_INIT_VOLT_COMP;
	quit_slave_chrg_cnt = 0;
	chrg_cc_power_tunning_cnt = 0;
	chrg_cv_taper_tunning_cnt = 0;
	chrg_cv_delta_volt = 0;
	pd_constant_power_cnt = 0;
	batt_curr_roof = 0;
	return;
}

#define REV_BST_THRESH_UV 4400000
#define REV_BST_DROP 150000
#define CP_IBUS_THRESH_MA 20
#define IBAT_THRESH_UA 10000
static bool is_cable_plugout(struct mmi_charger_manager *chip)
{
	struct mmi_cp_policy_dev *chrg_list = &g_chrg_list;
//	union power_supply_propval prop = {0,};
	int vbus_volt, ibatt_curr, ibus_curr, vbatt_volt;
//	bool pd_active = 0;

	if (chip->extrn_sense) {
		ibatt_curr = chrg_list->chrg_dev[CP_MASTER]->charger_data.ibatt_curr;
		ibatt_curr *= 1000;
		vbatt_volt = chrg_list->chrg_dev[CP_MASTER]->charger_data.vbatt_volt;
		vbatt_volt *= 1000;
	} else {
		ibatt_curr= chrg_list->chrg_dev[PMIC_SW]->charger_data.ibatt_curr;
		vbatt_volt = chrg_list->chrg_dev[PMIC_SW]->charger_data.vbatt_volt;
	}
	vbus_volt= chrg_list->chrg_dev[PMIC_SW]->charger_data.vbus_volt;
	ibus_curr = chrg_list->chrg_dev[CP_MASTER]->charger_data.ibus_curr;
	mmi_chrg_info(chip, "ibat:%d, vbat:%d, vbus:%d, ibus:%d\n",
		ibatt_curr, vbatt_volt, vbus_volt, ibus_curr);

	if (ibus_curr > CP_IBUS_THRESH_MA || ibatt_curr > IBAT_THRESH_UA)
		return false;

	if  (vbatt_volt >= vbus_volt
		&& vbus_volt < REV_BST_THRESH_UV) {
			mmi_chrg_info(chip, "cable plug out: cp reverse\n");
			return true;
	}

	return false;
}
#ifndef CONFIG_MOTO_CHG_WT6670F_SUPPORT
static bool mmi_chrg_check_capability(struct mmi_charger_manager *chip)
{
	int old_pps_volt = 0;
	int old_pps_curr = 0;

	old_pps_volt = chip->pd_volt_max;
	old_pps_curr = chip->pd_curr_max;
	mmi_chrg_info(chip,"old pd_volt_max = %d, old pd_curr_max = %d\n",
                      old_pps_volt, old_pps_curr);
	usbpd_get_pps_status_curr_volt(chip);
	mmi_chrg_info(chip,"new pd_volt_max = %d, new pd_curr_max = %d\n",
                              chip->pd_volt_max, chip->pd_curr_max);
	if (old_pps_volt != chip->pd_volt_max || old_pps_curr != chip->pd_curr_max) {
		return false;
	}
	return true;
}
#endif
static void mmi_chrg_sm_work_func(struct work_struct *work)
{
	struct mmi_charger_manager *chip = container_of(work,
				struct mmi_charger_manager, mmi_chrg_sm_work.work);
	int i = 0, rc = 0;
	int ibatt_curr = 0, vbatt_volt = 0, batt_temp = 0, vbus_pres = 0;
	int batt_soc = 0;
	int heartbeat_dely_ms = 0;
	int cooling_curr = 0;
	int cooling_volt = 0;
	bool zone_change = false;
	bool ignore_hysteresis_degc = false;
	struct mmi_chrg_step_info *chrg_step;
	union power_supply_propval prop = {0,};
	struct mmi_cp_policy_dev *chrg_list = &g_chrg_list;
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
	int ibus_curr = 0, calculated_vbus = 0,ibus_cov_to_volt = 0, vbus_cov_to_volt = 0;
//	int vbus_volt = 0, volt_change = 0;
	int volt_change = 0;
	bool thermal_reset = false;
	bool skip_qc3p_pdo = false;
	bool sw_reduce_volt = false;
#endif

	mmi_chrg_dbg(chip, PR_MOTO, "\n\n\n");

	mmi_chrg_dbg(chip, PR_MOTO, "schedule SM work, sm state %s \n",
					pm_state_str[sm_state]);

	mmi_chrg_dbg(chip, PR_MOTO, "pmic-sw is exist: %d  "
								"cp-master is exist: %d  "
								"cp-slave is exist: %d\n",
								chrg_list->pmic_sw,
								chrg_list->cp_master,
								chrg_list->cp_slave);
	if (!chrg_list->pmic_sw) {
		mmi_chrg_err(chip,"PMIC-SW isn't exist, force quite mmi chrg sm work !\n");
		return;
	}

	rc = power_supply_get_property(chip->batt_psy,
				POWER_SUPPLY_PROP_CURRENT_NOW, &prop);
	if (!rc)
		ibatt_curr = prop.intval;

	mmi_chrg_info(chip, "Get battery current %d\n", ibatt_curr);

	rc = power_supply_get_property(chip->batt_psy,
				POWER_SUPPLY_PROP_TEMP, &prop);
	if (!rc)
		batt_temp = prop.intval / 10;

	rc = power_supply_get_property(chip->batt_psy,
				POWER_SUPPLY_PROP_CAPACITY, &prop);
	if (!rc)
		batt_soc = prop.intval;


	if (ibatt_curr < 0)
		ibatt_curr *= -1;


	mmi_update_all_charger_status(chip);
	mmi_update_all_charger_error(chip);

	if (chip->extrn_sense) {
		ibatt_curr = chrg_list->chrg_dev[CP_MASTER]->charger_data.ibatt_curr;
		ibatt_curr *= 1000;
		if (ibatt_curr < 0)
			ibatt_curr *= -1;
	        mmi_chrg_info(chip, "Get extrn_sense battery current %d\n", ibatt_curr);
	}
	vbatt_volt = chrg_list->chrg_dev[CP_MASTER]->charger_data.vbatt_volt;

	vbus_pres = chrg_list->chrg_dev[PMIC_SW]->charger_data.vbus_pres;
	if (!vbus_pres) {
		for (i = 0; i < 3; i++) {
			mmi_update_all_charger_status(chip);
			vbus_pres = chrg_list->chrg_dev[PMIC_SW]->charger_data.vbus_pres;
			mmi_chrg_info(chip, "Retry check charger status, vbus %d\n", vbus_pres);
			if (vbus_pres) {
				mmi_chrg_info(chip, "Get vbus present , continue to charging\n");
				break;
			}
			msleep(100);
		}
	}

	mmi_chrg_info(chip, "battery current %d\n", ibatt_curr);
	mmi_chrg_info(chip, "battery voltage %d\n", vbatt_volt);
	mmi_chrg_info(chip, "battery temp %d\n", batt_temp);
	mmi_chrg_info(chip, "battery capacity %d\n", batt_soc);

	if (vbus_pres && is_cable_plugout(chip)) {
		mmi_chrg_info(chip, "is_cable_plugout true\n");
		vbus_pres = 0;
	}
	if (vbus_pres &&
		(sm_state == PM_STATE_PPS_TUNNING_CURR
		|| sm_state == PM_STATE_PPS_TUNNING_VOLT
		|| sm_state == PM_STATE_CP_CC_LOOP
		|| sm_state == PM_STATE_CP_CV_LOOP)) {
		mmi_dump_charger_error(chip, chrg_list->chrg_dev[CP_MASTER]);
		chrg_policy_error_recovery(chip, chrg_list);
	}

	if (sm_state == PM_STATE_CP_CV_LOOP && false == chip->disable_ignore_hysteresis)
		ignore_hysteresis_degc = true;
	zone_change = mmi_find_temp_zone(chip, batt_temp, ignore_hysteresis_degc);
	chrg_step = &chip->chrg_step;

	if (chip->pres_temp_zone == ZONE_COLD
		|| chip->pres_temp_zone == ZONE_HOT
		|| !chrg_list->chrg_dev[PMIC_SW]->charger_enabled
		|| vbatt_volt > chip->batt_ovp_lmt) {

		mmi_chrg_info(chip, "Force stop charging, "
						"pres_temp_zone %d, "
						"pmic charger enabled %d, "
						"vbatt_volt %dmv, "
						"batt ovp limit %dmv\n",
						chip->pres_temp_zone,
						chrg_list->chrg_dev[PMIC_SW]->charger_enabled,
						vbatt_volt, chip->batt_ovp_lmt);
		mmi_chrg_sm_move_state(chip, PM_STATE_STOP_CHARGE);
	}

	if (!vbus_pres) {
		mmi_chrg_sm_move_state(chip, PM_STATE_DISCONNECT);
	} else if (sm_state == PM_STATE_ENTRY
			|| sm_state == PM_STATE_STOP_CHARGE) {
		mmi_find_chrg_step(chip, chip->pres_temp_zone, vbatt_volt);
	} else if (sm_state == PM_STATE_DISCONNECT) {
		mmi_find_chrg_step(chip, chip->pres_temp_zone, vbatt_volt);
		mmi_chrg_sm_move_state(chip, PM_STATE_ENTRY);
	} else if (zone_change &&
			chip->pres_temp_zone != ZONE_COLD &&
			chip->pres_temp_zone != ZONE_HOT) {

			if (batt_temp >= chrg_step->temp_c) {
				mmi_chrg_info(chip, "battery temp %d, temp thre %d "
						"Enter into COOLING LOOP !\n",
						batt_temp, chrg_step->temp_c);
				chip->batt_therm_cooling = true;
				chip->pd_batt_therm_volt = chip->pd_request_volt_prev;
				chip->pd_batt_therm_curr = chip->pd_request_curr_prev;
			} else if (!chip->batt_therm_cooling &&
					!ignore_hysteresis_degc) {
				mmi_chrg_info(chip, "battery temp %d, temp thre %d "
						"Restart select chrg step and temp zone !\n",
						batt_temp, chrg_step->temp_c);
				mmi_find_chrg_step(chip, chip->pres_temp_zone, vbatt_volt);
				mmi_chrg_sm_move_state(chip, PM_STATE_ENTRY);
			}
	}

	mmi_chrg_dbg(chip, PR_MOTO, "temp zone %d, is_changed %d, "
					"chrg_step %d, "
					"step cc curr %d, step cv volt %d, "
					"step cv tapper curr %d\n",
					chip->pres_temp_zone, zone_change,
					chrg_step->pres_chrg_step,
					chrg_step->chrg_step_cc_curr,
					chrg_step->chrg_step_cv_volt,
					chrg_step->chrg_step_cv_tapper_curr);
	switch (sm_state) {
	case PM_STATE_DISCONNECT:
		mmi_chrg_info(chip,"vbus disconnect !, jump to PM_STATE_DISCONNECT,"
					"recovery PMIC-SW limitation, and close CP charg\n");
		if (chrg_list->cp_slave
			&& chrg_list->chrg_dev[CP_SLAVE]->charger_enabled) {
			mmi_enable_charging(chrg_list->chrg_dev[CP_SLAVE], false);
		}

		if (chrg_list->cp_master
			&& chrg_list->chrg_dev[CP_MASTER]->charger_enabled) {
			mmi_enable_charging(chrg_list->chrg_dev[CP_MASTER], false);
		}

		if (chrg_list->chrg_dev[PMIC_SW]->charger_limited) {
			mmi_set_charing_current_limit(chrg_list->chrg_dev[PMIC_SW],false,
							DISABLE_CHRG_LIMIT_CURR_UA);
			chrg_list->chrg_dev[PMIC_SW]->charger_limited = false;
		}

		heartbeat_dely_ms = HEARTBEAT_CANCEL;
		break;
	case PM_STATE_ENTRY:
		if (chrg_list->cp_slave
			&& chrg_list->chrg_dev[CP_SLAVE]->charger_enabled) {
			mmi_chrg_info(chip,"Disable slave charger pump! \n");
			mmi_enable_charging(chrg_list->chrg_dev[CP_SLAVE], false);
		}

		if (chrg_list->cp_master
			&& chrg_list->chrg_dev[CP_MASTER]->charger_enabled) {
			mmi_enable_charging(chrg_list->chrg_dev[CP_MASTER], false);
		}

		if (chip->pd_pps_support
			&& chrg_list->cp_master
			&& vbatt_volt > chip->pl_chrg_vbatt_min
			&& chrg_step->pres_chrg_step != chip->chrg_step_nums - 1
			&& chrg_step->chrg_step_cc_curr >=
				chrg_list->chrg_dev[CP_MASTER]->charging_curr_min
			&& batt_soc < CP_CHRG_SOC_LIMIT) {

			mmi_chrg_dbg(chip, PR_MOTO, "Enter into CHRG PUMP, "
							"vbatt %d uV, "
							"pps support %d, "
							"chrg step %d, "
							"chrg step cc curr %d uA, "
							"CP master charging curr min %d uA\n",
							vbatt_volt,
							chip->pd_pps_support,
							chrg_step->pres_chrg_step,
							chrg_step->chrg_step_cc_curr,
							chrg_list->chrg_dev[CP_MASTER]->charging_curr_min);
			mmi_chrg_sm_move_state(chip, PM_STATE_CHRG_PUMP_ENTRY);
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
			skip_qc3p_pdo = true;
#endif

		} else {
			mmi_chrg_dbg(chip, PR_MOTO, "Enter into PMIC switch charging, "
							"the reason is : vbatt %d uV, "
							"pl chrg vbatt min %d uV, "
							"pps support %d, "
							"chrg step %d\n",
							vbatt_volt, chip->pl_chrg_vbatt_min,
							chip->pd_pps_support,
							chrg_step->pres_chrg_step);
			mmi_chrg_sm_move_state(chip, PM_STATE_SW_ENTRY);

		}
		chip->batt_therm_cooling = false;
		chip->batt_therm_cooling_cnt= 0;
		chip->pd_sys_therm_volt=  chip->pd_request_volt_prev;
		chip->pd_sys_therm_curr = chip->pd_request_curr_prev;
		chip->pd_batt_therm_volt = chip->pd_request_volt_prev;
		chip->pd_batt_therm_curr = chip->pd_request_curr_prev;
		chip->sys_therm_cooling = false;
		chip->sys_therm_force_pmic_chrg = false;
		chip->recovery_pmic_chrg = false;
		heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
		break;
	case PM_STATE_SW_ENTRY:
		if (chrg_list->cp_slave
			&& chrg_list->chrg_dev[CP_SLAVE]->charger_enabled) {
			mmi_enable_charging(chrg_list->chrg_dev[CP_SLAVE], false);
		}

		if (chrg_list->cp_master
			&& chrg_list->chrg_dev[CP_MASTER]->charger_enabled) {
			mmi_enable_charging(chrg_list->chrg_dev[CP_MASTER], false);
		}

		if (chrg_list->chrg_dev[PMIC_SW]->charger_limited) {
			mmi_chrg_info(chip, "Recovery PMIC-SW ichg lmt \n");
			mmi_set_charing_current_limit(chrg_list->chrg_dev[PMIC_SW],false,
								DISABLE_CHRG_LIMIT_CURR_UA);
			chrg_list->chrg_dev[PMIC_SW]->charger_limited = false;
		}

		if (!chip->dont_rerun_aicl) {
			mmi_chrg_info(chip, "Do an rerun usb AICL for PMIC-SW\n");
			mmi_enable_charging(chrg_list->chrg_dev[PMIC_SW], false);
			msleep(100);
			mmi_enable_charging(chrg_list->chrg_dev[PMIC_SW], true);
		}
		chip->pd_request_volt = SWITCH_CHARGER_PPS_VOLT;
		chip->pd_request_curr = TYPEC_HIGH_CURRENT_UA;
#ifndef CONFIG_MOTO_CHG_WT6670F_SUPPORT
		usbpd_pps_enable_charging(chip, false,
						chip->pd_request_volt / 1000,
						chip->pd_request_curr / 1000);
#endif

		mmi_chrg_info(chip,"Select pd request curr %d, volt %d\n",
						chip->pd_request_curr,
						chip->pd_request_volt);

//#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
//		if(!chrg_list->chrg_dev[PMIC_SW]->charger_limited){
//			mmi_chrg_sm_move_state(chip, PM_STATE_STOP_CHARGE);
//		} else {
//#endif
		        mmi_chrg_sm_move_state(chip, PM_STATE_SW_LOOP);
//#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
//		}
//#endif
		heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
		break;
	case PM_STATE_SW_LOOP:
		if (chip->pd_pps_support
			&& chip->cp_disable == false
			&& vbatt_volt > chip->pl_chrg_vbatt_min
			&& chrg_step->pres_chrg_step != chip->chrg_step_nums - 1
			&& chrg_step->chrg_step_cc_curr >=
				chrg_list->chrg_dev[CP_MASTER]->charging_curr_min
			&& !chip->recovery_pmic_chrg
			&& !chip->sys_therm_force_pmic_chrg
			&& batt_soc < CP_CHRG_SOC_LIMIT) {
			mmi_chrg_info(chip, "Enter CP, the reason is : "
							"pd pps support %d, "
							"vbatt %duV, chrg step %d\n",
							chip->pd_pps_support,
							vbatt_volt, chrg_step->pres_chrg_step);
			mmi_chrg_sm_move_state(chip, PM_STATE_CHRG_PUMP_ENTRY);
			heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
			skip_qc3p_pdo = true;
#endif
		} else {
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
			if(vbatt_volt > 4530000){
				sw_reduce_volt = true;
			}
			mmi_chrg_dbg(chip, PR_MOTO, "Continue to SW charging, "
						"vbatt %d uV, ibatt %d uA, sw_reduce_volt: %d\n",
						vbatt_volt, ibatt_curr, sw_reduce_volt);
#else
                        mmi_chrg_dbg(chip, PR_MOTO, "Continue to SW charging, "
                                                "vbatt %d uV, ibatt %d uA\n",
                                                vbatt_volt, ibatt_curr);
#endif
			heartbeat_dely_ms = HEARTBEAT_lOOP_WAIT_MS;
		}
		break;
	case PM_STATE_CHRG_PUMP_ENTRY:
		mmi_chrg_info(chip,"CP master exist %d, CP slave exist %d !\n",
							chrg_list->cp_master,
							chrg_list->cp_slave);

		mmi_chrg_dev_init_protect(chip);

		if (chrg_list->cp_slave) {
			mmi_chrg_info(chip,"CP slave is exist !\n");
			mmi_chrg_info(chip,"chrg step cc curr %d uA, "
							"CP slave charging curr min %d uA\n",
							chrg_step->chrg_step_cc_curr,
							chrg_list->chrg_dev[CP_SLAVE]->charging_curr_min);
			if (chrg_step->chrg_step_cc_curr >=
					chrg_list->chrg_dev[CP_SLAVE]->charging_curr_min) {
				mmi_chrg_sm_move_state(chip,
							PM_STATE_DULE_CP_ENTRY);
			} else
				mmi_chrg_sm_move_state(chip,
							PM_STATE_SINGLE_CP_ENTRY);
		} else {
			mmi_chrg_info(chip,"CP slave isn't exist !\n");
			mmi_chrg_sm_move_state(chip, PM_STATE_SINGLE_CP_ENTRY);
		}

		mmi_chrg_info(chip,"Set PMIC SW FCC limits!\n");
		if (!chrg_list->chrg_dev[PMIC_SW]->charger_limited
			&& chrg_list->chrg_dev[PMIC_SW]->charging_curr_limited > 0) {
			mmi_set_charing_current_limit(chrg_list->chrg_dev[PMIC_SW],true,
							chrg_list->chrg_dev[PMIC_SW]->charging_curr_limited);
			chrg_list->chrg_dev[PMIC_SW]->charger_limited = true;
			mmi_chrg_info(chip,"Set PMIC-SW ichg lmt ,%d uA\n",
							chrg_list->chrg_dev[PMIC_SW]->charging_curr_limited);
		}

		mmi_chrg_info(chip, "Check all effective pdo info again\n");
#ifndef CONFIG_MOTO_CHG_WT6670F_SUPPORT
		chip->pd_pps_support = usbpd_get_pps_status(chip);
#endif

		/*Initial setup pps request power by the battery voltage*/
		chip->pd_request_volt = (2 * vbatt_volt) % 20000;
		chip->pd_request_volt = 2 * vbatt_volt - chip->pd_request_volt
							+ chip->pps_volt_comp;
		chip->pd_request_curr =
					min(chip->pd_curr_max, chip->typec_middle_current);
		mmi_chrg_info(chip,"pps init , volt %dmV, curr %d uA, volt comp %d uv\n",
			chip->pd_request_volt, chip->pd_request_curr, chip->pps_volt_comp);
		chrg_policy_error_clear(chip, chrg_list);
		heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
		break;
	case PM_STATE_SINGLE_CP_ENTRY:
		if (chrg_list->cp_slave
			&& chrg_list->chrg_dev[CP_SLAVE]->charger_enabled) {
			mmi_chrg_info(chip,"Disable Slave Charger Pump !\n");
			mmi_enable_charging(chrg_list->chrg_dev[CP_SLAVE], false);
		}

		if (chrg_list->cp_master) {
			mmi_enable_charging(chrg_list->chrg_dev[CP_MASTER], true);
			mmi_chrg_info(chip,"Enable Master Charger Pump !\n");
			msleep(120);
			mmi_chrg_sm_move_state(chip, PM_STATE_PPS_TUNNING_CURR);
			heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
		}
		break;
	case PM_STATE_DULE_CP_ENTRY:
		if (chrg_list->cp_master) {
			mmi_enable_charging(chrg_list->chrg_dev[CP_MASTER], true);
			mmi_chrg_info(chip,"Enable Master Charger Pump !\n");
		}

		if (chrg_list->cp_slave) {
			chrg_list->cp_clave_later = true;
			mmi_chrg_info(chip,"For ibus UCP, "
			"delay start Slave Charger Pump in TUNNING_VOLT stage !\n");
		}

		if (chrg_list->chrg_dev[CP_MASTER]->charger_enabled) {
			mmi_chrg_sm_move_state(chip, PM_STATE_PPS_TUNNING_CURR);
			heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
		}
		break;
	case PM_STATE_PPS_TUNNING_CURR:
		heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
		if (chrg_list->cp_master
			&& (!chrg_list->chrg_dev[CP_MASTER]->charger_enabled
#ifndef CONFIG_MOTO_CHG_WT6670F_SUPPORT
			|| !(chrg_list->chrg_dev[CP_MASTER]->charger_error.chrg_err_type & (1<< MMI_CP_SWITCH_BIT))
#endif
			)) {
			mmi_chrg_info(chip,"CP MASTER was disabled, Enter into "
								"SW directly\n");
			chip->pps_volt_comp = PPS_INIT_VOLT_COMP;
			mmi_chrg_sm_move_state(chip, PM_STATE_SW_ENTRY);
		}else if (vbatt_volt > chrg_step->chrg_step_cv_volt) {
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
			if ((chip->pd_request_curr - chip->pps_curr_steps
				> chip->typec_middle_current) ||
			    (ibatt_curr - chip->pps_curr_steps > chrg_step->chrg_step_cc_curr)){
#else
                        if (chip->pd_request_curr - chip->pps_curr_steps
                                > chip->typec_middle_current){
#endif
				chip->pd_request_curr -= chip->pps_curr_steps;
			}
			mmi_chrg_sm_move_state(chip,
						PM_STATE_CP_CC_LOOP);
			mmi_chrg_info(chip,"During the curr going up process, "
						"the chrg step was changed,"
						"stop increase pps curr and Enter into "
						"CC stage as soon!\n");
		} else if (chip->pps_result < 0) {
			if (mmi_get_pps_result_history(chip) != NO_ERROR) {
#ifndef CONFIG_MOTO_CHG_WT6670F_SUPPORT
				mmi_chrg_info(chip,"PM_STATE_PPS_TUNNING_CURR: capability detect begin, sourcecap_dec_enable = %d\n",
					chip->sourcecap_dec_enable);
				if (chip->sourcecap_dec_enable) {
					if (false == mmi_chrg_check_capability(chip)) {
						mmi_chrg_info(chip,"PM_STATE_PPS_TUNNING_CURR:pd_volt_max or pd_curr_max changed\n");
						goto schedule;
					}
				}
#endif

				mmi_chrg_sm_move_state(chip,
						PM_STATE_CP_CC_LOOP);
				mmi_chrg_info(chip,"Too many pdo request failed,"
							" Enter into CC stage directly!\n");
				mmi_clear_pps_result_history(chip);
			}
			chip->pd_request_curr = chip->pd_request_curr_prev;
			goto schedule;
		} else if (chip->pd_request_curr + chip->pps_curr_steps
				<= chip->pd_curr_max
				&& vbatt_volt < chrg_step->chrg_step_cv_volt
				&& ibatt_curr < ((chip->system_thermal_level != THERMAL_NOT_LIMIT)?
				min(chrg_step->chrg_step_cc_curr, chip->system_thermal_level):
				chrg_step->chrg_step_cc_curr)){
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
                                mmi_chrg_dbg(chip, PR_MOTO, "pd_request_curr: %dua, pd_curr_max: %dua, chrg_step_cc_curr: %d, pps_curr_steps: %d, system_thermal_level: %d\n", chip->pd_request_curr, chip->pd_curr_max, chrg_step->chrg_step_cc_curr, chip->pps_curr_steps, chip->system_thermal_level);

//				if(chip->pd_request_curr + 2 * chip->pps_curr_steps <= chrg_step->chrg_step_cc_curr){
//				volt_change = chip->pps_curr_steps * 2;
//				} else {
				      volt_change = chip->pps_curr_steps;
//				}

				chip->pd_request_curr += min(volt_change, chrg_step->chrg_step_cc_curr - ibatt_curr);
				mmi_chrg_dbg(chip, PR_MOTO, "Increase pps curr %d, volt_chage: %d \n", chip->pd_request_curr, volt_change);
				volt_change = 0;

#else
				chip->pd_request_curr += chip->pps_curr_steps;
				mmi_chrg_dbg(chip, PR_MOTO, "Increase pps curr %d\n",
								chip->pd_request_curr);
#endif
				heartbeat_dely_ms = HEARTBEAT_PPS_TUNNING_MS;
		} else {
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
			if(ibatt_curr > chrg_step->chrg_step_cc_curr){
                                volt_change = ((ibatt_curr - chrg_step->chrg_step_cc_curr) % 20000 == 0) ?
					(ibatt_curr - chrg_step->chrg_step_cc_curr) :
					(ibatt_curr - chrg_step->chrg_step_cc_curr + 20000);

                                chip->pd_request_curr -= volt_change;

                                mmi_chrg_dbg(chip, PR_MOTO, "Reduce pps curr %d, volt_chage: %d \n", chip->pd_request_curr, volt_change);
                                volt_change = 0;
		     	}
#endif

			mmi_chrg_info(chip,"Enter into tunning pps volt\n");
			mmi_chrg_sm_move_state(chip, PM_STATE_PPS_TUNNING_VOLT);
		}
		break;
	case PM_STATE_PPS_TUNNING_VOLT:
		heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
		if (chrg_list->cp_slave
			&& chrg_list->cp_clave_later
			&& !chrg_list->chrg_dev[CP_SLAVE]->charger_enabled) {
			mmi_enable_charging(chrg_list->chrg_dev[CP_SLAVE], true);
			mmi_chrg_info(chip,"Enable Slave Charger Pump !\n");
		}

		if (chrg_list->cp_master
			&& (!chrg_list->chrg_dev[CP_MASTER]->charger_enabled
#ifndef CONFIG_MOTO_CHG_WT6670F_SUPPORT
			|| !(chrg_list->chrg_dev[CP_MASTER]->charger_error.chrg_err_type & (1<< MMI_CP_SWITCH_BIT))
#endif
			)) {
			mmi_chrg_info(chip,"CP MASTER was disabled, "
							"Enter into SW directly\n");
			chip->pps_volt_comp = PPS_INIT_VOLT_COMP;
			mmi_chrg_sm_move_state(chip, PM_STATE_SW_ENTRY);
		} else if (vbatt_volt > chrg_step->chrg_step_cv_volt) {
			chip->pd_request_volt -= chip->pps_volt_steps;
			mmi_chrg_sm_move_state(chip,
						PM_STATE_CP_CC_LOOP);
			mmi_chrg_info(chip,"Duing the volt going up process, "
						"the chrg step was changed,"
						"stop increase pps volt and"
						" Enter into CC stage as soon!\n");
		} else if (chip->pps_result < 0) {
			if (mmi_get_pps_result_history(chip) != NO_ERROR) {
#ifndef CONFIG_MOTO_CHG_WT6670F_SUPPORT
				mmi_chrg_info(chip,"PM_STATE_PPS_TUNNING_VOLT: capability detect begin, sourcecap_dec_enable = %d\n",
					chip->sourcecap_dec_enable);
				if (chip->sourcecap_dec_enable) {
					if (false == mmi_chrg_check_capability(chip)) {
						mmi_chrg_info(chip,"PM_STATE_PPS_TUNNING_VOLT:pd_volt_max or pd_curr_max changed\n");
						goto schedule;
					}
				}
#endif
				mmi_chrg_sm_move_state(chip,
						PM_STATE_CP_CC_LOOP);
				mmi_clear_pps_result_history(chip);
				mmi_chrg_info(chip,"Too many pdo request failed,"
							" Enter into CC stage directly!\n");
			}
			chip->pd_request_volt = chip->pd_request_volt_prev;
			goto schedule;
		} else if (chip->pd_request_volt + chip->pps_volt_steps
				<= chip->pd_volt_max
				&& vbatt_volt < chrg_step->chrg_step_cv_volt
				&& ibatt_curr < ((chrg_step->pres_chrg_step == STEP_FIRST) ?
				chrg_step->chrg_step_cc_curr + chip->step_first_curr_comp:
				chrg_step->chrg_step_cc_curr)) {
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
				mmi_chrg_dbg(chip, PR_MOTO, "pd_request_volt: %duv, pd_volt_max: %duv, chrg_step_cv_volt: %d, pps_volt_steps: %d \n", chip->pd_request_volt, chip->pd_volt_max, chrg_step->chrg_step_cv_volt, chip->pps_volt_steps);
/*
				if((chip->pd_volt_max - chip->pd_request_volt) >= (30 * chip->pps_volt_steps)){
					chip->pd_request_volt += (chip->pd_volt_max - 30 * chip->pps_volt_steps - chip->pd_request_volt)/chip->pps_volt_steps * chip->pps_volt_steps;
				} else {
					chip->pd_request_volt += chip->pps_volt_steps;
				}
*/
				if(chip->pd_request_volt + 3 * chip->pps_volt_steps <= chip->pd_volt_max){
				        volt_change = 3 * chip->pps_volt_steps;
				} else {
					volt_change = 1 * chip->pps_volt_steps;
				}

				ibus_curr = (chrg_step->pres_chrg_step == STEP_FIRST) ?
					chrg_step->chrg_step_cc_curr + chip->step_first_curr_comp:
					chrg_step->chrg_step_cc_curr;

				chip->pd_request_volt += min(volt_change, ibus_curr - ibatt_curr);

				mmi_chrg_dbg(chip, PR_MOTO, "Increase pps volt %d, volt_change: %d, ibus_curr: %d \n", chip->pd_request_volt, volt_change, ibus_curr);
#else
				chip->pd_request_volt += chip->pps_volt_steps;
				mmi_chrg_dbg(chip, PR_MOTO, "Increase pps volt %d \n", chip->pd_request_volt);
#endif
				heartbeat_dely_ms = HEARTBEAT_PPS_TUNNING_MS;
		} else {
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
                        if(ibatt_curr > chrg_step->chrg_step_cc_curr){
                                volt_change = ((ibatt_curr - chrg_step->chrg_step_cc_curr) % 20000 == 0) ?
                                        (ibatt_curr - chrg_step->chrg_step_cc_curr) :
                                        (ibatt_curr - chrg_step->chrg_step_cc_curr + 20000);

				volt_change = min(volt_change, chip->pps_curr_steps);
                                chip->pd_request_curr -= volt_change;

                                mmi_chrg_dbg(chip, PR_MOTO, "Reduce pps curr %d, volt_chage: %d \n", chip->pd_request_curr, volt_change);
                                volt_change = 0;
                        }
#endif

			mmi_chrg_info(chip,"Enter into CC loop stage !\n");
			mmi_chrg_sm_move_state(chip, PM_STATE_CP_CC_LOOP);
		}

		if (ibatt_curr > batt_curr_roof) {
			batt_curr_roof = ibatt_curr;
		}
		if (ibatt_curr < batt_curr_roof)
			pd_constant_power_cnt++;
		else
			pd_constant_power_cnt = 0;

		if (pd_constant_power_cnt > PD_CONT_PWR_CNT) {
			mmi_chrg_info(chip,"PD adapter was ready in constant power state, "
							"Enter into CC loop stage !\n");

			mmi_chrg_sm_move_state(chip, PM_STATE_CP_CC_LOOP);
		}
		mmi_chrg_info(chip, "pd_constant_power_cnt %d, "
							"batt_curr_roof %d\n",
							pd_constant_power_cnt,
							batt_curr_roof);
		break;
	case PM_STATE_CP_CC_LOOP:
		heartbeat_dely_ms = HEARTBEAT_lOOP_WAIT_MS;
		mmi_chrg_dbg(chip, PR_MOTO,
								"Chrg CC loop, chrg_step %d, "
								"vbatt %dmV, ibatt %d uA, "
								"CC target curr %d uA, "
								"next CV target volt %dmV\n ",
								chrg_step->pres_chrg_step,
								vbatt_volt, ibatt_curr,
								chrg_step->chrg_step_cc_curr,
								chrg_step->chrg_step_cv_volt);
		if (chrg_list->cp_master
			&& (!chrg_list->chrg_dev[CP_MASTER]->charger_enabled
#ifndef CONFIG_MOTO_CHG_WT6670F_SUPPORT
			|| !(chrg_list->chrg_dev[CP_MASTER]->charger_error.chrg_err_type & (1<< MMI_CP_SWITCH_BIT))
#endif
			)) {
			mmi_chrg_info(chip,"CP MASTER was disabled, Enter into SW directly\n");
			chip->pps_volt_comp = PPS_INIT_VOLT_COMP;
			mmi_chrg_sm_move_state(chip, PM_STATE_SW_ENTRY);
			heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
			goto schedule;
		}

#ifndef CONFIG_MOTO_CHG_WT6670F_SUPPORT
		mmi_chrg_info(chip,"PM_STATE_CP_CC_LOOP: capability detect begin, sourcecap_dec_enable = %d\n",
			chip->sourcecap_dec_enable);
		if (chip->sourcecap_dec_enable) {
			if (false == mmi_chrg_check_capability(chip)) {
				mmi_chrg_info(chip,"PM_STATE_CP_CC_LOOP:pd_volt_max or pd_curr_max changed\n");
				goto schedule;
			}
		}
#endif

		if (chip->pps_result < 0) {
			mmi_chrg_err(chip, "Last select pdo failed\n");
			chip->pps_result = mmi_get_pps_result_history(chip);
			switch (chip->pps_result) {
			case BLANCE_POWER:
				chip->pd_pps_balance = true;
				mmi_chrg_err(chip, "Enable pps power balance\n");
				mmi_clear_pps_result_history(chip);
				break;
			case RESET_POWER:
				mmi_chrg_sm_move_state(chip, PM_STATE_ENTRY);
				mmi_chrg_err(chip, "Hard reset charger policy to recovery power,"
								"Since too many pdo failed\n");
				mmi_clear_pps_result_history(chip);
				break;
			default:
				break;
			}

			chip->pd_request_volt = chip->pd_request_volt_prev;
			if (ibatt_curr < chrg_step->chrg_step_cc_curr
				&& chip->pd_request_volt < chip->pd_volt_max)
				chrg_cc_power_tunning_cnt =
							CC_POWER_COUNT;
			goto schedule;
		}

#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
                if (((!chip->sys_therm_cooling) || (chip->system_thermal_level >= chrg_step->chrg_step_cc_curr))
#else
		if (!chip->sys_therm_cooling
#endif
			&& !chip->batt_therm_cooling
			&& pd_constant_power_cnt <= PD_CONT_PWR_CNT) {
			if (ibatt_curr < chrg_step->chrg_step_cc_curr
				&& chip->pd_request_volt < chip->pd_volt_max
				&& chrg_cc_power_tunning_cnt >=
					CC_POWER_COUNT) {
				if (chip->pd_pps_balance
					&& (chip->pd_request_curr - chip->pps_curr_steps)
						> chip->typec_middle_current) {
					chip->pd_request_curr -=
						chip->pps_curr_steps;
					chip->pd_request_volt +=
						mmi_calculate_delta_volt(chip->pd_request_volt_prev,
						chip->pd_request_curr_prev,
						chip->pps_curr_steps);
					mmi_chrg_dbg(chip, PR_MOTO,
									"Request curr decreass %d uA, "
									"Request volt increase %d uV \n",
									chip->pd_request_curr,
									chip->pd_request_volt);
				} else {
					if (chip->pd_request_curr + chip->pps_curr_steps
						< chip->pd_curr_max) {
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
						chip->pd_request_curr +=
							chip->pps_curr_steps;
//							chip->pps_curr_steps - 40000;
#else
						chip->pd_request_curr +=
							chip->pps_curr_steps;
#endif
						mmi_chrg_dbg(chip, PR_MOTO,
									"Request curr increase %d uA\n ",
									chip->pd_request_curr);
					} else if (chip->pd_request_volt + chip->pps_volt_steps
							< chip->pd_volt_max) {
						chip->pd_request_volt +=
							chip->pps_volt_steps;
						mmi_chrg_dbg(chip, PR_MOTO,
									"Request volt increase %duV\n ",
									chip->pd_request_volt);
					}
				}
				chrg_cc_power_tunning_cnt = 0;
				mmi_chrg_dbg(chip, PR_MOTO,
									"For keeping CC chrg POWER, "
									"Have to adjust pd input volt or curr"
									"Request volt %d uV,  "
									"Request curr %d uA\n",
									chip->pd_request_volt,
									chip->pd_request_curr);
			}else if (ibatt_curr < chrg_step->chrg_step_cc_curr
				&& chip->pd_request_volt < chip->pd_volt_max) {
				chrg_cc_power_tunning_cnt++;
				mmi_chrg_dbg(chip, PR_MOTO,
									"Chrg CC tunning cnt %d\n",
									chrg_cc_power_tunning_cnt);
			} else if (ibatt_curr > chrg_step->chrg_step_cc_curr
								+ CC_CURR_DEBOUNCE) {
				chip->pd_request_volt -= chip->pps_volt_steps;
				mmi_chrg_dbg(chip, PR_MOTO,
					"In the CC step , the ibatt is greater than CC curr, "
					"Request volt decreass %duV to remain CC step\n ",
					chip->pd_request_volt);
			} else
				chrg_cc_power_tunning_cnt = 0;

			if(ibatt_curr < chrg_step->chrg_step_cc_curr)
				heartbeat_dely_ms = HEARTBEAT_SHORT_DELAY_MS;
		}

#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
		if (vbatt_volt >= chrg_step->chrg_step_cv_volt) {
#else
		if (vbatt_volt > chrg_step->chrg_step_cv_volt) {
#endif
			if (chrg_cv_taper_tunning_cnt >
				CV_TAPPER_COUNT) {
				mmi_chrg_sm_move_state(chip, PM_STATE_CP_CV_LOOP);
				chrg_cv_taper_tunning_cnt = 0;
				chrg_cv_delta_volt = CV_DELTA_VOLT;
				heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
			} else {
				chrg_cv_taper_tunning_cnt++;
				mmi_chrg_dbg(chip, PR_MOTO,
								"Chrg CV taper cnt %d, "
								"chrg step cv volt %dmV, "
								"vbatt %dmV\n",
								chrg_cv_taper_tunning_cnt,
								chrg_step->chrg_step_cv_volt,
								vbatt_volt);
			}
		} else
			chrg_cv_taper_tunning_cnt = 0;

		break;
	case PM_STATE_CP_CV_LOOP:
		heartbeat_dely_ms = HEARTBEAT_SHORT_DELAY_MS;
		mmi_chrg_dbg(chip, PR_MOTO,
								"Chrg CV loop, chrg_step %d, "
								"vbatt %dmV, ibatt %d uA, "
								"CV target volt %d uV, "
								"CV taper curr %d uA\n ",
								chrg_step->pres_chrg_step,
								vbatt_volt, ibatt_curr,
								chrg_step->chrg_step_cv_volt,
								chrg_step->chrg_step_cv_tapper_curr);
		if (chrg_list->cp_master
			&& (!chrg_list->chrg_dev[CP_MASTER]->charger_enabled
#ifndef CONFIG_MOTO_CHG_WT6670F_SUPPORT
			|| !(chrg_list->chrg_dev[CP_MASTER]->charger_error.chrg_err_type &  (1<< MMI_CP_SWITCH_BIT))
#endif
			)) {
			mmi_chrg_info(chip,"CP MASTER was disabled, Enter into SW directly\n");
			chip->pps_volt_comp = PPS_INIT_VOLT_COMP;
			mmi_chrg_sm_move_state(chip, PM_STATE_SW_ENTRY);
			heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
			goto schedule;
		}

#ifndef CONFIG_MOTO_CHG_WT6670F_SUPPORT
		mmi_chrg_info(chip,"PM_STATE_CP_CV_LOOP: capability detect begin,sourcecap_dec_enable = %d\n",
			chip->sourcecap_dec_enable);
		if (chip->sourcecap_dec_enable) {
			if (false == mmi_chrg_check_capability(chip)) {
				mmi_chrg_info(chip,"PM_STATE_CP_CV_LOOP:pd_volt_max or pd_curr_max changed\n");
				goto schedule;
			}
		}
#endif

		if (chip->pps_result < 0) {
			mmi_chrg_err(chip, "Last select pdo failed\n");
			chip->pps_result = mmi_get_pps_result_history(chip);
			switch (chip->pps_result) {
			case BLANCE_POWER:
				chip->pd_request_curr -=
					chip->pps_curr_steps;
				mmi_chrg_err(chip, "Reduce pps curr,for pps power balance\n");
				mmi_clear_pps_result_history(chip);
				break;
			case RESET_POWER:
				mmi_chrg_sm_move_state(chip, PM_STATE_ENTRY);
				mmi_chrg_err(chip, "Hard reset charger policy to recovery power,"
								"Since too many pdo failed\n");
				mmi_clear_pps_result_history(chip);
				break;
			default:
				break;
			}
			chip->pd_request_volt = chip->pd_request_volt_prev;
			goto schedule;
		}

		if (vbatt_volt >= chrg_step->chrg_step_cv_volt
			&& ((!chrg_step->last_step &&
				ibatt_curr < chrg_step->chrg_step_cv_tapper_curr)
				|| ibatt_curr < chrg_list->chrg_dev[CP_MASTER]->charging_curr_min)) {
			if (chrg_cv_taper_tunning_cnt >= CV_TAPPER_COUNT) {
				if (ibatt_curr <
					chrg_list->chrg_dev[CP_MASTER]->charging_curr_min) {
					mmi_chrg_info(chip, "Ready quite CP chrg stage, "
								"and Enter into PMIC switch chrg stage, "
								"chrg step %d, ibatt %d uA\n",
								chrg_step->pres_chrg_step, ibatt_curr);
					mmi_find_chrg_step(chip,
							chip->pres_temp_zone, vbatt_volt);
					mmi_chrg_sm_move_state(chip, PM_STATE_CP_QUIT);
					heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
				} else {
					if (mmi_find_chrg_step(chip,
							chip->pres_temp_zone, vbatt_volt)) {
						heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
						mmi_chrg_info(chip,"Jump to next chrg step\n");
						mmi_chrg_sm_move_state(chip,
									PM_STATE_CP_CC_LOOP);
					} else {
						mmi_chrg_info(chip,"Can't find next chrg step\n");
						mmi_chrg_sm_move_state(chip, PM_STATE_CP_QUIT);
					}
				}
				chrg_cv_taper_tunning_cnt = 0;
			} else {
				chrg_cv_taper_tunning_cnt++;
				mmi_chrg_dbg(chip, PR_MOTO, "chrg cv taper cnt ++, %d\n",
								chrg_cv_taper_tunning_cnt);
			}

		}else if (!chip->sys_therm_cooling
			&& !chip->batt_therm_cooling)  {

			chrg_cv_taper_tunning_cnt = 0;
			if (vbatt_volt > chrg_step->chrg_step_cv_volt + 10000) {
				if (chrg_cv_delta_volt > 20000)
					chip->pd_request_volt -= chrg_cv_delta_volt;
				else
					chip->pd_request_volt -= 20000;
				mmi_chrg_info(chip,
					"For keeping CV stage, decrease volt %dmV, "
					"cv delta volt %dmV\n",
					chip->pd_request_volt, chrg_cv_delta_volt);
			} else if (vbatt_volt < chrg_step->chrg_step_cv_volt - 10000) {
				chrg_cv_delta_volt -= 20000;
				chip->pd_request_volt += 20000;
				mmi_chrg_info(chip,
					"For keeping CV stage, increase volt %dmV, "
					"cv delta volt %dmV\n",
					chip->pd_request_volt, chrg_cv_delta_volt);
			} else {
				mmi_chrg_dbg(chip, PR_MOTO, "CV loop work well, "
						"keep pd power, volt %dmV, curr %d uA\n",
						chip->pd_request_volt, chip->pd_request_curr);
			}

		}else {
		/*In this case, sys_therm_cooling or batt_therm_cooling is ture*/

			if (vbatt_volt > chrg_step->chrg_step_cv_volt + 10000) {
				if (chrg_cv_delta_volt > 20000)
					chip->pd_request_volt -= chrg_cv_delta_volt;
				else
					chip->pd_request_volt -= 20000;
				mmi_chrg_info(chip,
					"For keeping CV stage, decrease volt %dmV, "
					"cv delta volt %dmV\n",
					chip->pd_request_volt, chrg_cv_delta_volt);
			} else {
				mmi_chrg_dbg(chip, PR_MOTO, "CV loop work well, "
							"keep pd power, volt %d uV, curr %d uA\n",
							chip->pd_request_volt, chip->pd_request_curr);
			}
		}

		if (chrg_list->cp_slave) {
			if (quit_slave_chrg_cnt > 3
				&& chrg_list->chrg_dev[CP_SLAVE]->charger_enabled) {
				mmi_enable_charging(chrg_list->chrg_dev[CP_SLAVE], false);
				mmi_chrg_info(chip,"Quit slave chrg, the reason is :ibatt %duA, "
							"CP slave charging curr min %d uA\n",
							ibatt_curr,
							chrg_list->chrg_dev[CP_SLAVE]->charging_curr_min);
				msleep(100);
				mmi_enable_charging(chrg_list->chrg_dev[CP_MASTER], true);
				mmi_chrg_info(chip,"Restart CP master again\n");
			}

			if (ibatt_curr <
					chrg_list->chrg_dev[CP_SLAVE]->charging_curr_min)
				quit_slave_chrg_cnt++;
			else
				quit_slave_chrg_cnt = 0;
		}
		break;
	case PM_STATE_CP_QUIT:
		if (chrg_list->cp_slave
			&& chrg_list->chrg_dev[CP_SLAVE]->charger_enabled) {
			mmi_enable_charging(chrg_list->chrg_dev[CP_SLAVE], false);
		}

		if (chrg_list->cp_master
			&& chrg_list->chrg_dev[CP_MASTER]->charger_enabled) {
			mmi_enable_charging(chrg_list->chrg_dev[CP_MASTER], false);
		}

		chip->batt_therm_cooling = false;
		chip->batt_therm_cooling_cnt = 0;
		mmi_chrg_sm_move_state(chip, PM_STATE_RECOVERY_SW);
		heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
		break;
	case PM_STATE_RECOVERY_SW:
		heartbeat_dely_ms = HEARTBEAT_SHORT_DELAY_MS;
		if (chrg_list->chrg_dev[PMIC_SW]->charger_limited) {
			mmi_chrg_info(chip,"Recovery PMIC-SW ichg lmt\n");
			mmi_set_charing_current_limit(chrg_list->chrg_dev[PMIC_SW],false,
								DISABLE_CHRG_LIMIT_CURR_UA);
			chrg_list->chrg_dev[PMIC_SW]->charger_limited = false;

			mmi_chrg_info(chip,"Do an rerun usb AICL for PMIC-SW\n");
			mmi_enable_charging(chrg_list->chrg_dev[PMIC_SW], false);
			msleep(100);
			mmi_enable_charging(chrg_list->chrg_dev[PMIC_SW], true);
			chip->recovery_pmic_chrg = true;
			chrg_cv_taper_tunning_cnt = 0;
		}

		chip->pd_request_curr = TYPEC_HIGH_CURRENT_UA;
		mmi_chrg_info(chip,"ibatt : %d uA, step cc curr : %d uA\n",
						ibatt_curr, chrg_step->chrg_step_cc_curr);
		if (ibatt_curr > chrg_step->chrg_step_cc_curr) {
			chip->pd_request_volt -= CV_DELTA_VOLT;

			mmi_chrg_dbg(chip, PR_MOTO, "Reduce pps volt %dmV, curr %d uA\n ",
						chip->pd_request_volt, chip->pd_request_curr);
			heartbeat_dely_ms = HEARTBEAT_PPS_TUNNING_MS;
			chrg_cv_taper_tunning_cnt = 0;
		} else {
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
			chip->pd_request_volt = 5000000;
#endif
			chrg_cv_taper_tunning_cnt++;

		}

		if (chrg_cv_taper_tunning_cnt > CV_TAPPER_COUNT){
//#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
//			mmi_chrg_sm_move_state(chip, PM_STATE_STOP_CHARGE);
//#else
			mmi_chrg_sm_move_state(chip, PM_STATE_SW_LOOP);
//#endif
			heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
		}
		break;
	case PM_STATE_STOP_CHARGE:
		heartbeat_dely_ms = HEARTBEAT_lOOP_WAIT_MS;
		if (chrg_list->cp_slave
			&& chrg_list->chrg_dev[CP_SLAVE]->charger_enabled) {
			mmi_enable_charging(chrg_list->chrg_dev[CP_SLAVE], false);
		}

		if (chrg_list->cp_master
			&& chrg_list->chrg_dev[CP_MASTER]->charger_enabled) {
			mmi_enable_charging(chrg_list->chrg_dev[CP_MASTER], false);
		}

		if (chrg_list->chrg_dev[PMIC_SW]->charger_limited) {
			mmi_chrg_info(chip,"Recovery PMIC-SW ichg lmt \n");
			mmi_set_charing_current_limit(chrg_list->chrg_dev[PMIC_SW],false,
								DISABLE_CHRG_LIMIT_CURR_UA);
			chrg_list->chrg_dev[PMIC_SW]->charger_limited = false;
		}

		if (chip->pres_temp_zone != ZONE_COLD
		&& chip->pres_temp_zone != ZONE_HOT
		&& chrg_list->chrg_dev[PMIC_SW]->charger_enabled
		&& chrg_step->chrg_step_cc_curr > 0) {
			mmi_chrg_sm_move_state(chip, PM_STATE_ENTRY);
			heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
		}
		chip->pd_request_volt = SWITCH_CHARGER_PPS_VOLT;
		chip->pd_request_curr = TYPEC_HIGH_CURRENT_UA;
		break;
	case PM_STATE_COOLING_LOOP:
		mmi_chrg_info(chip,"In cooling loop, batt temp %d, cmp temp %d\n",
						batt_temp, chrg_step->temp_c);

		if (batt_temp > chrg_step->temp_c + COOLING_HYSTERISIS_DEGC) {

			mmi_chrg_info(chip,"Batt temp %d, Cooling loop failed, "
							"force enter PM_STATE_ENTRY "
							"restart this charger process !\n",
						batt_temp);
			mmi_find_chrg_step(chip, chip->pres_temp_zone,
										vbatt_volt);
			mmi_chrg_sm_move_state(chip,
						PM_STATE_ENTRY);

		} else if (batt_temp > chrg_step->temp_c) {
				cooling_curr =
					min(chip->pd_curr_max, chip->typec_middle_current);
				if (chip->pd_request_curr > cooling_curr) {
					chip->pd_request_curr -= COOLING_DELTA_POWER;
				} else {
					mmi_chrg_info(chip, "pd request curr %d uA, "
						"battery temp %d, "
						"cooling failed, Restart PM_STATE_ENTRY !\n",
						chip->pd_request_curr, batt_temp);
					mmi_find_chrg_step(chip, chip->pres_temp_zone,
										vbatt_volt);
					mmi_chrg_sm_move_state(chip,
										PM_STATE_ENTRY);
				}

		} else if (batt_temp < chrg_step->temp_c - COOLING_DELTA_POWER) {

			mmi_chrg_info(chip,"Batt temp %d, successful exit from COOLing loop, "
							"and Enter int TUNNING_CURR again !\n",
						batt_temp);
			mmi_chrg_sm_move_state(chip,
						PM_STATE_PPS_TUNNING_CURR);
		}

		if (vbatt_volt > chrg_step->chrg_step_cv_volt) {
			if (chrg_cv_taper_tunning_cnt >
				CV_TAPPER_COUNT) {

				mmi_chrg_info(chip, "vbatt_volt %dmV, "
					"battery temp %d, "
					"chrg step cv volt %dmV"
					"cooling failed, Restart PM_STATE_ENTRY !\n",
					vbatt_volt, batt_temp, chrg_step->chrg_step_cv_volt);
				mmi_find_chrg_step(chip, chip->pres_temp_zone,
										vbatt_volt);
				mmi_chrg_sm_move_state(chip, PM_STATE_ENTRY);
				chrg_cv_taper_tunning_cnt = 0;
				heartbeat_dely_ms = HEARTBEAT_NEXT_STATE_MS;
			} else {
				chrg_cv_taper_tunning_cnt++;
				mmi_chrg_dbg(chip, PR_MOTO,
								"Chrg CV taper cnt %d, "
								"chrg step cv volt %dmV, "
								"vbatt %dmV\n",
								chrg_cv_taper_tunning_cnt,
								chrg_step->chrg_step_cv_volt,
								vbatt_volt);
			}
		} else
			chrg_cv_taper_tunning_cnt = 0;
		heartbeat_dely_ms = HEARTBEAT_SHORT_DELAY_MS;
		break;
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
        case PM_STATE_POWER_LIMIT_LOOP:
                mmi_chrg_info(chip, "ibus_curr over, enter limitation status:%s\n",pm_state_str[sm_state]);
                get_caculated_real_ibus_vbus(chip,&ibus_curr,&calculated_vbus);

                if( ibus_curr <= (chip->pd_curr_max ) && calculated_vbus < (chip->pd_volt_max ) ) {
                        mmi_chrg_info(chip, "ibus_curr and vbus back to normal, resume prev status:%s\n",pm_state_str[prev_sm_state]);
                        mmi_chrg_sm_move_state(chip, prev_sm_state);
                        heartbeat_dely_ms = HEARTBEAT_PPS_TUNNING_MS;
                        goto schedule;
                }

                ibus_cov_to_volt  = (((ibus_curr - chip->pd_curr_max + chip->pps_curr_steps)/100000) * CURR_TO_VOLT_STEP);
                if(ibus_cov_to_volt <0)
                        ibus_cov_to_volt = 0;
                vbus_cov_to_volt= (((calculated_vbus -chip->pd_volt_max + chip->pps_volt_steps*2)/QC3P_V_STEP)  );
                if(vbus_cov_to_volt <0)
                        vbus_cov_to_volt = 0;

                mmi_chrg_info(chip, "ibus_curr abnormal, ibus decrease:%d,vbus decrease:%d\n", ibus_cov_to_volt, vbus_cov_to_volt );

                chip->pd_request_volt  -= max(ibus_cov_to_volt, vbus_cov_to_volt ) * chip->pps_volt_steps;

                mmi_chrg_info(chip, "ibus_curr abnormal, decrease pps volt:%d\n", chip->pd_request_volt );

                heartbeat_dely_ms = HEARTBEAT_PPS_TUNNING_MS;
                break;
#endif
	default:
		break;
        }

schedule:

	chip->pd_target_volt = min(chip->pd_request_volt, chip->pd_volt_max);
	chip->pd_target_curr = min(chip->pd_request_curr, chip->pd_curr_max);

	if (chip->system_thermal_level == THERMAL_NOT_LIMIT) {
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
		if(chip->sys_therm_force_pmic_chrg){
		        mmi_chrg_info(chip, "sys_therm_force_pmic_chrg is reset!\n");
			thermal_reset = true;
		}
#endif
		chip->sys_therm_cooling= false;
		chip->sys_therm_force_pmic_chrg = false;
	} else if ((chip->system_thermal_level <= chip->thermal_min_level)
	&& !chip->sys_therm_force_pmic_chrg) {
		chip->sys_therm_cooling = true;
		chip->sys_therm_force_pmic_chrg = true;
		mmi_chrg_sm_move_state(chip, PM_STATE_SW_ENTRY);
		mmi_chrg_info(chip, "Thermal is the highest, level %d, "
						"Force enter into single pmic charging !\n",
						chip->system_thermal_level);

	} else if (chip->system_thermal_level > chip->thermal_min_level &&
		(sm_state == PM_STATE_CP_CC_LOOP ||
		sm_state == PM_STATE_CP_CV_LOOP ||
		sm_state == PM_STATE_PPS_TUNNING_VOLT||
		sm_state == PM_STATE_PPS_TUNNING_CURR
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
		|| sm_state == PM_STATE_POWER_LIMIT_LOOP
#endif
		)) {

		mmi_chrg_dbg(chip, PR_MOTO, "Thermal level is %d\n",
								chip->system_thermal_level);
		if (!chip->sys_therm_cooling) {
			chip->sys_therm_cooling = true;
			chip->pd_sys_therm_volt = chip->pd_request_volt_prev;
			chip->pd_sys_therm_curr = chip->pd_request_curr_prev;
		}

		if (ibatt_curr > chip->system_thermal_level + CC_CURR_DEBOUNCE) {
			if (chip->pd_sys_therm_curr - THERMAL_TUNNING_CURR >=
				chip->typec_middle_current) {
				chip->pd_sys_therm_curr -= THERMAL_TUNNING_CURR;
				mmi_chrg_dbg(chip, PR_MOTO, "For thermal, decrease pps curr %d\n",
								chip->pd_sys_therm_curr);

			} else {
				mmi_chrg_dbg(chip, PR_MOTO,
								"pd_sys_therm_curr %d uA was less than %d uA, "
								"Give up thermal mitigation!",
								chip->pd_sys_therm_curr - THERMAL_TUNNING_CURR,
								chip->typec_middle_current);
			}
		} else if (ibatt_curr < chip->system_thermal_level - CC_CURR_DEBOUNCE) {
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
			if ( (ibatt_curr + THERMAL_TUNNING_CURR <= chrg_step->chrg_step_cc_curr)
				&&(vbatt_volt <= chrg_step->chrg_step_cv_volt - 10000) ) {
				chip->pd_sys_therm_curr += THERMAL_TUNNING_CURR;
				chip->pd_target_curr = chip->pd_sys_therm_curr;
#else
                        if (chip->pd_sys_therm_curr + THERMAL_TUNNING_CURR <=
                                chip->pd_curr_max) {
                                chip->pd_sys_therm_curr += THERMAL_TUNNING_CURR;
#endif
				mmi_chrg_dbg(chip, PR_MOTO, "For thermal, increase pps curr %d\n",
								chip->pd_sys_therm_curr);
			}
		}

		heartbeat_dely_ms = HEARTBEAT_SHORT_DELAY_MS;
	} else if (chip->system_thermal_level != THERMAL_NOT_LIMIT &&
		chip->system_thermal_level > chip-> thermal_min_level &&
		sm_state == PM_STATE_SW_LOOP &&
		chip->sys_therm_force_pmic_chrg) {
			mmi_chrg_dbg(chip, PR_MOTO, "Try to recovery charger pump!\n");
			mmi_chrg_sm_move_state(chip, PM_STATE_ENTRY);
	}

	if (chip->sys_therm_cooling) {
		chip->pd_target_volt = min(chip->pd_target_volt, chip->pd_sys_therm_volt);
		chip->pd_target_curr = min(chip->pd_target_curr, chip->pd_sys_therm_curr);
	}

	if (chip->batt_therm_cooling && !chip->sys_therm_force_pmic_chrg) {
		if (batt_temp > chrg_step->temp_c + COOLING_HYSTERISIS_DEGC) {

			mmi_chrg_info(chip,"Batt temp %d, Cooling loop failed, "
							"force enter PM_STATE_ENTRY "
							"restart this charger process !\n",
						batt_temp);
			chip->batt_therm_cooling = false;
			chip->batt_therm_cooling_cnt= 0;
			mmi_find_chrg_step(chip, chip->pres_temp_zone,
										vbatt_volt);
			mmi_chrg_sm_move_state(chip,
						PM_STATE_ENTRY);

		} else if (batt_temp > chrg_step->temp_c) {
				cooling_curr =
					min(chip->pd_curr_max, chip->typec_middle_current);
				cooling_volt = (2 * vbatt_volt) % 20000;
				cooling_volt = 2 * vbatt_volt - cooling_volt
						+ chip->pps_volt_comp;
				if (ibatt_curr > TYPEC_HIGH_CURRENT_UA
					&& chip->pd_batt_therm_curr > cooling_curr) {

					if (chip->pd_batt_therm_curr - COOLING_DELTA_POWER >=
						chip->typec_middle_current)
						chip->pd_batt_therm_curr -= COOLING_DELTA_POWER;
					mmi_chrg_info(chip, "Do chrg power cooling"
						"pd_batt_therm_curr %d uA, "
						"battery temp %d\n",
						chip->pd_batt_therm_curr, batt_temp);
				} else if (ibatt_curr > TYPEC_HIGH_CURRENT_UA
						&& chip->pd_batt_therm_volt > cooling_volt) {
					chip->pd_batt_therm_volt -= COOLING_DELTA_POWER;
					mmi_chrg_info(chip, "Do chrg power cooling"
						"pd request volt %d uA, "
						"battery temp %d\n",
						chip->pd_batt_therm_volt, batt_temp);

					} else {
						if (chip->batt_therm_cooling_cnt > COOLING_MAX_CNT) {

						mmi_chrg_info(chip, "Do chrg power cooling failed"
							"pd_batt_therm_currr %d uA, "
							"pd_batt_therm_volt %d uV"
							"battery temp %d, "
							"Restart PM_STATE_ENTRY !\n",
							chip->pd_batt_therm_curr,
							chip->pd_batt_therm_volt,
							batt_temp);
						mmi_find_chrg_step(chip, chip->pres_temp_zone,
											vbatt_volt);
						chip->batt_therm_cooling = false;
						chip->batt_therm_cooling_cnt = 0;
						mmi_chrg_sm_move_state(chip,
											PM_STATE_ENTRY);

						} else {
						mmi_chrg_info(chip, "It's already the lowest cooling chrg power"
							"waiting for a while, cooling cnt %d, "
							"battery temp %d\n",
							batt_temp, chip->batt_therm_cooling);
							chip->batt_therm_cooling_cnt++;
						}
				}

		} else if (batt_temp < chrg_step->temp_c - COOLING_HYSTERISIS_DEGC) {

			mmi_chrg_info(chip,"Batt temp %d, "
							"Exit successfully from COOLing loop!\n",
							batt_temp);

			if (sm_state == PM_STATE_CP_CC_LOOP) {
				mmi_chrg_info(chip,"Jump into CURR tunning"
								"for chrg power poerformance!\n");
					mmi_chrg_sm_move_state(chip,
								PM_STATE_PPS_TUNNING_CURR);
			}
			chip->batt_therm_cooling = false;
			chip->batt_therm_cooling_cnt = 0;
		}
		heartbeat_dely_ms = HEARTBEAT_lOOP_WAIT_MS;
	}

#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
	if (chip->batt_therm_cooling || thermal_reset) {
		if(thermal_reset){
			thermal_reset = false;
		}
#else
	if (chip->batt_therm_cooling) {
#endif
		chip->pd_target_volt = min(chip->pd_target_volt, chip->pd_batt_therm_volt);
		chip->pd_target_curr = min(chip->pd_target_curr, chip->pd_batt_therm_curr);
	}

	mmi_chrg_dbg(chip, PR_MOTO, "chrg sm work,%s, "
								"battery soc %d, "
								"battery temp %d, "
								"battery current %d, "
								"battery voltage %d, "
								"sys therm level %d, "
								"sys therm cooling %d, "
								"batt therm cooling %d, "
								"sys therm force pmic chrg %d, "
								"recovery pmic chrg %d\n",
								pm_state_str[sm_state],
								batt_soc, batt_temp,
								ibatt_curr, vbatt_volt,
								chip->system_thermal_level,
								chip->sys_therm_cooling,
								chip->batt_therm_cooling,
								chip->sys_therm_force_pmic_chrg,
								chip->recovery_pmic_chrg);

	mmi_chrg_dbg(chip, PR_MOTO, 	"pd request volt %d uV, "
								"pd request curr %d uA, "
								"pd target volt %d uV, "
								"pd target curr %d uA, "
								"sys therm volt %d uV, "
								"sys therm curr %d uA, "
								"batt therm volt %d uV, "
								"batt therm curr %d uA\n",
								chip->pd_request_volt,
								chip->pd_request_curr,
								chip->pd_target_volt,
								chip->pd_target_curr,
								chip->pd_sys_therm_volt,
								chip->pd_sys_therm_curr,
								chip->pd_batt_therm_volt,
								chip->pd_batt_therm_curr);

	if (chip->pd_target_volt < SWITCH_CHARGER_PPS_VOLT
		|| chip->pd_target_curr < chip->typec_middle_current) {

		if (sm_state == PM_STATE_PPS_TUNNING_CURR
			|| sm_state == PM_STATE_PPS_TUNNING_VOLT
			|| sm_state == PM_STATE_CP_CC_LOOP
			|| sm_state == PM_STATE_CP_CV_LOOP) {

			mmi_chrg_err(chip, "%s, wrong pd voltage or current , "
						"request pd voltage %d, "
						"request pd current %d\n",
						pm_state_str[sm_state],
						chip->pd_target_volt,
						chip->pd_target_curr);
			mmi_chrg_sm_move_state(chip, PM_STATE_ENTRY);
		}
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
		if(!sw_reduce_volt)
#endif
			goto skip_pd_select;
	}

#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
/*
	mmi_chrg_err(chip, "PMIC_SW charger_limited: %d, charger_enabled: %d\n", chrg_list->chrg_dev[PMIC_SW]->charger_limited, chrg_list->chrg_dev[PMIC_SW]->charger_enabled);
	if(!chrg_list->chrg_dev[PMIC_SW]->charger_limited &&
	    chrg_list->chrg_dev[PMIC_SW]->charger_enabled &&
	    !chip->recovery_pmic_chrg){
*/
	if(skip_qc3p_pdo){
		goto skip_pd_select;
	}
	chip->pps_result = qc3p_select_pdo(chip, chip->pd_target_volt, chip->pd_target_curr, sw_reduce_volt);
/*
	vbus_volt = chrg_list->chrg_dev[CP_MASTER]->charger_data.vbus_volt;
	vbus_volt *= 1000;
	volt_change = (chip->pd_target_volt > vbus_volt)?
		(chip->pd_target_volt - vbus_volt) :
		(vbus_volt - chip->pd_target_volt);

	volt_change = volt_change/1000;

	if(volt_change > 200){
		heartbeat_dely_ms = volt_change/200 * 100;
	}
	mmi_chrg_err(chip, "checking vbus_volt: %d, target_volt: %d, volt_chage: %d, delay_ms: %dms\n", vbus_volt, chip->pd_target_volt, volt_change, heartbeat_dely_ms);
*/
#else
	chip->pps_result = usbpd_select_pdo(chip,
								chip->pd_target_volt / 1000,
								chip->pd_target_curr / 1000);
#endif
	mmi_set_pps_result_history(chip, chip->pps_result);
	if (!chip->pps_result) {
		chip->pd_request_volt_prev = chip->pd_target_volt;
		chip->pd_request_curr_prev = chip->pd_target_curr;
	}

skip_pd_select:

	if (heartbeat_dely_ms > 0) {
		mmi_chrg_err(chip, "schedule work timer %dms\n", heartbeat_dely_ms);
		schedule_delayed_work(&chip->mmi_chrg_sm_work,
				msecs_to_jiffies(heartbeat_dely_ms));
	} else {
		mmi_chrg_policy_clear(chip);
		chip->sm_work_running = false;
		chip->pd_pps_support = false;
		mmi_chrg_err(chip, "exit sm work\n");
	}

	return;
}

#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
int get_caculated_real_ibus_vbus(struct mmi_charger_manager *chip, int *ibus_curr,int *calculated_vbus) {
        int rc;
        struct mmi_cp_policy_dev *chrg_list = &g_chrg_list;

        int  vbus_volt = 0,ibus_curr_temp = 0,ibus_usb = 0,ibus_pump = 0,ibus_pump_slave = 0;
        int calculated_vbus_temp = 0;

	if (chrg_list->cp_master) {
		mmi_get_input_voltage_settled(chrg_list->chrg_dev[CP_MASTER],&vbus_volt);
	}

	if (chrg_list->cp_master) {
		mmi_get_input_current(chrg_list->chrg_dev[CP_MASTER],&ibus_pump);
	}

        if(ibus_pump < 0)
                ibus_pump = 0;

	if (chrg_list->pmic_sw) {
		rc = mmi_get_input_current(chrg_list->chrg_dev[PMIC_SW],&ibus_usb);
		if (rc) {
			ibus_usb = 200; //Force return 200mA input charging current of main charger.
		}
	}

        if(ibus_usb < 0)
                ibus_usb = 0;

        if (chrg_list->cp_slave && chrg_list->chrg_dev[CP_SLAVE]->charger_enabled) {
		mmi_get_input_current(chrg_list->chrg_dev[CP_SLAVE],&ibus_pump_slave);
                if(ibus_pump_slave < 0)
                        ibus_pump_slave = 0;
        }

        //calculate the real ibus vbus
        ibus_curr_temp = ibus_usb + ibus_pump+ibus_pump_slave;
        calculated_vbus_temp = vbus_volt+ (ibus_curr_temp *200)/1000; // Define vbus resistance 200 mili oum

        *ibus_curr = ibus_curr_temp;
        *calculated_vbus = calculated_vbus_temp;
        mmi_chrg_err(chip, "calculated ibus:%d,vbus:%d\n",*ibus_curr,*calculated_vbus);

        return 0;
}

#define QC3P_PULSE_COUNT_MAX    ((11000 - 5000) / 20 )
#define USB_VBUS_IRQ_THRESHOLD		3800000
#define QC3P_STEP_VOLT	20000 // 20mV
int qc3p_select_pdo(struct mmi_charger_manager *chip,int target_uv, int target_ua, bool sw_reduce_volt){
        int rc,vbus_val;
        struct mmi_cp_policy_dev *chrg_list = &g_chrg_list;
        union power_supply_propval prop = {0,};
        int ibatt_curr = 0, vbatt_volt = 0, vbus_volt = 0,ibus_curr = 0,ibus_usb = 0,ibus_pump = 0,ibus_pump_slave = 0;
//        int i = 0;
        int calculated_vbus = 0;
        int req_volt_inc_step = 0,req_volt_dec_step = 0;
        int req_curr_inc_step = 0,req_curr_dec_step = 0;
        int real_inc_step = 0,real_dec_step =0;
//        union power_supply_propval val = {0, };
//        int pulse_cnt_curr;
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
	int target_vbus_volt = 0;
	static int retry_cnt = 0;
//	struct mmi_chrg_step_info *chrg_step;
	int count = 0;
	int vbus_volt_new = 0;
#endif

#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
        rc = power_supply_get_property(chip->batt_psy,
                                POWER_SUPPLY_PROP_VOLTAGE_NOW, &prop);
        if (!rc)
                vbatt_volt = prop.intval;
#else
	vbatt_volt = chrg_list->chrg_dev[CP_MASTER]->charger_data.vbatt_volt;
#endif

        rc = power_supply_get_property(chip->batt_psy,
                                POWER_SUPPLY_PROP_CURRENT_NOW, &prop);

        if (!rc)
                ibatt_curr = prop.intval;

	if (chrg_list->cp_master) {
		mmi_get_input_voltage_settled(chrg_list->chrg_dev[CP_MASTER],&vbus_volt);
	}

	if (chrg_list->cp_master) {
		mmi_get_input_current(chrg_list->chrg_dev[CP_MASTER],&ibus_pump);
	}

        if(ibus_pump < 0)
                ibus_pump = 0;

        if (chrg_list->cp_slave && chrg_list->chrg_dev[CP_SLAVE]->charger_enabled) {
		mmi_get_input_current(chrg_list->chrg_dev[CP_SLAVE],&ibus_pump_slave);
        }

	if (chrg_list->pmic_sw) {
		rc = mmi_get_input_current(chrg_list->chrg_dev[PMIC_SW],&ibus_usb);
		if (rc) {
			ibus_usb = 200000; //Force return 200mA input charging current of main charger.
		}
	}

        if(ibus_usb < 0)
                ibus_usb = 0;

        mmi_chrg_err(chip, "ibus_usb:%d uA, ibus_pump:%d uA, ibus_pump_slave:%d uA\n", ibus_usb , ibus_pump, ibus_pump_slave);

        //calculate the real ibus vbus
        ibus_curr = ibus_usb + ibus_pump;
        if (chrg_list->cp_slave && chrg_list->chrg_dev[CP_SLAVE]->charger_enabled) {
                ibus_curr += ibus_pump_slave;
        }

	calculated_vbus = vbus_volt+ (ibus_curr *200)/1000; // Define vbus resistance 200 mili oum

	mmi_chrg_err(chip, "qc3p_select_pdo current vbat:%d,ibatt:%d,vbus:%d,calculated_vbus:%d,ibus:%d,target_uv:%d,prev_uv:%d,target_ua:%d,prev_ua:%d\n",
			vbatt_volt,ibatt_curr,vbus_volt,calculated_vbus,ibus_curr,chip->pd_target_volt,chip->pd_request_volt_prev,chip->pd_target_curr,chip->pd_request_curr_prev);

        if(((ibus_curr > chip->pd_curr_max && chrg_list->chrg_dev[CP_MASTER]->charger_enabled) || calculated_vbus > chip->pd_volt_max) && sm_state != PM_STATE_POWER_LIMIT_LOOP && sm_state != PM_STATE_SW_LOOP ){
                if(chip->pd_request_volt_prev != 0 && chip->pd_request_curr_prev != 0) {
                        chip->pd_request_volt = chip->pd_request_volt_prev;
                        chip->pd_target_volt = chip->pd_request_volt;
                        chip->pd_request_curr = chip->pd_request_curr_prev;
                        chip->pd_target_curr = chip->pd_request_curr;
                }
                mmi_chrg_sm_move_state(chip,PM_STATE_POWER_LIMIT_LOOP);
                return 0;
        }

	if(sw_reduce_volt && vbatt_volt > 4530000){
		count = -1;
	} else{

        if(chip->pd_request_volt_prev == 0)
                chip->pd_request_volt_prev = 5000000;

        //req current step
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
        if((chip->pd_target_curr > chip->pd_request_curr_prev) &&
	   (chip->pd_request_curr_prev > 0)) {  //200mA -> 100mv
                req_curr_inc_step =  (chip->pd_target_curr - chip->pd_request_curr_prev)/2;

        }else if(chip->pd_target_curr < chip->pd_request_curr_prev) {
                req_curr_dec_step = (chip->pd_request_curr_prev - chip->pd_target_curr)/2;

                //check the steps
                if(ibus_curr <= chip->pd_target_curr) {
                        mmi_chrg_err(chip,"ibus <= target curr,not decrease curr");
                        req_curr_dec_step = 0;
                }
        }
#else
        if(chip->pd_target_curr > chip->pd_request_curr_prev) {  //100mA -> 100mv
                req_curr_inc_step =  (chip->pd_target_curr -chip->pd_request_curr_prev)/100000 * CURR_TO_VOLT_STEP;
                if(req_curr_inc_step < 0 )
                        req_curr_inc_step = 0;

        }else if(chip->pd_target_curr < chip->pd_request_curr_prev) {
                req_curr_dec_step =  (chip->pd_request_curr_prev - chip->pd_target_curr)/100000 * CURR_TO_VOLT_STEP;
                if(req_curr_dec_step < 0)
                        req_curr_dec_step = 0;

                //check the steps
                if(ibus_curr <= chip->pd_target_curr ) {
                        mmi_chrg_err(chip,"ibus <= target curr,not decrease curr");
                                req_volt_inc_step = 0;
                }
        }
#endif

        //req voltage step
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
//        if((chip->pd_target_volt >= (vbus_volt * 1000)) &&
        if((chip->pd_target_volt >= chip->pd_request_volt_prev) &&
	  (chip->pd_target_volt > 5000000)) {
//                req_volt_inc_step =  chip->pd_target_volt - vbus_volt * 1000;
                req_volt_inc_step =  chip->pd_target_volt - chip->pd_request_volt_prev;

        mmi_chrg_err(chip, "thermal flag sys_therm_cooling:%d, batt_therm_cooling:%d\n", chip->sys_therm_cooling, chip->batt_therm_cooling);
//	if(chip->sys_therm_cooling || chip->batt_therm_cooling)
//		req_volt_inc_step = 0;
#else
        if(chip->pd_target_volt >= chip->pd_request_volt_prev) {
                req_volt_inc_step =  (chip->pd_target_volt -chip->pd_request_volt_prev)/QC3P_V_STEP;
#endif
                if(req_volt_inc_step<0)
                        req_volt_inc_step = 0;

        }else {
#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
                req_volt_dec_step =  chip->pd_request_volt_prev - chip->pd_target_volt;
//                req_volt_dec_step = vbus_volt * 1000 - chip->pd_target_volt;
#else
                req_volt_dec_step =  (chip->pd_request_volt_prev - chip->pd_target_volt)/QC3P_V_STEP;
#endif
                if(req_volt_dec_step < 0)
                        req_volt_dec_step = 0;

        }

	//check the steps
	if(ibus_curr > chip->pd_curr_max || calculated_vbus > chip->pd_volt_max) {
		mmi_chrg_err(chip,"ibus > max curr or vbus, skip inc volt");
		req_volt_inc_step = 0;
		req_volt_inc_step = 0;
        }

        mmi_chrg_err(chip, "prev_calculate current inc:%d,dec:%d,voltage inc:%d,dec:%d\n",req_curr_inc_step,req_curr_dec_step,req_volt_inc_step,req_volt_dec_step);

        real_inc_step = max(req_volt_inc_step,req_curr_inc_step);
        real_dec_step = max(req_volt_dec_step,req_curr_dec_step);

        mmi_chrg_err(chip, "after calculated, voltage inc:%d,dec:%d\n",real_inc_step,real_dec_step);

#ifdef CONFIG_MOTO_CHG_WT6670F_SUPPORT
	if(real_dec_step > 0){
		target_vbus_volt = vbus_volt - real_dec_step;
		count -= real_dec_step/QC3P_V_STEP;
		if(real_dec_step % QC3P_V_STEP > 0){
			count--;
		}

		mmi_chrg_err(chip, "vbus descend to volt: %d, vbus now = %d, count = %d \n", target_vbus_volt, vbus_volt, count);
	} else if(real_inc_step > 0
                && (calculated_vbus <= chip->pd_volt_max)
                && ibus_curr <= chip->pd_curr_max){  //increase
		target_vbus_volt = vbus_volt + real_inc_step;
		target_vbus_volt = min(target_vbus_volt, chip->pd_volt_max);
		count = (target_vbus_volt - vbus_volt)/QC3P_V_STEP;
		if((target_vbus_volt - vbus_volt) % QC3P_V_STEP > 0){
			count++;
		}

		mmi_chrg_err(chip, "vbus increase to volt: %d, vbus now = %d, count = %d \n", target_vbus_volt, vbus_volt, count);
	}

	if(count == 0){
		if(vbus_volt > 0){
		      chip->pd_target_volt = vbus_volt;
		      chip->pd_request_volt = chip->pd_target_volt;
		      if(chip->sys_therm_cooling){
			      chip->pd_sys_therm_volt = chip->pd_target_volt;
		      }
		      if(chip->batt_therm_cooling){
			      chip->pd_batt_therm_volt = chip->pd_target_volt;
		      }
		}
		if(ibus_pump >= chip->typec_middle_current){
		      chip->pd_target_curr = ibus_pump;
		}
		chip->pd_request_curr = chip->pd_target_curr;
                if(chip->sys_therm_cooling){
                      chip->pd_sys_therm_curr = chip->pd_target_curr;
                }
                if(chip->batt_therm_cooling){
                      chip->pd_batt_therm_curr = chip->pd_target_curr;
                }
	        mmi_chrg_err(chip, "after adjust vbus volt, pd_request_volt = %d, target_vbus = %d, pd_request_curr = %d, target_cur = %d \n",
				chip->pd_request_volt, chip->pd_target_volt, chip->pd_request_curr, chip->pd_target_curr);
	        mmi_chrg_err(chip, "after adjust vbus volt, sys_therm_cooling = %d, pd_sys_therm_volt = %d, pd_sys_therm_curr = %d, batt_therm_cooling = %d, pd_batt_therm_volt = %d, pd_batt_therm_curr = %d \n",
				chip->sys_therm_cooling, chip->pd_sys_therm_volt, chip->pd_sys_therm_curr, chip->batt_therm_cooling, chip->pd_batt_therm_volt, chip->pd_batt_therm_curr);
		return 0;
	}
	}

	for(retry_cnt = 0; retry_cnt < 3; retry_cnt++){
		rc = wt6670f_set_volt_count(count);
//        rc = wt6670f_set_voltage(target_vbus_volt);
                if(rc != 0){
                        mmi_chrg_err(chip, "wt6670f_set_voltage failed, switch to main charger!\n");
                        mmi_chrg_sm_move_state(chip,PM_STATE_STOP_CHARGE);
			break;
                } else{
			// Need to check if vbus voltage change as expected.
#ifdef CONFIG_MOTO_CHARGER_PUMP_MEASURE_AVG_VOL
			mdelay((5 * abs(count)) + 100);
#else
			udelay(10000*abs(count));
#endif
			if (chrg_list->cp_master) {
		                rc = mmi_get_input_voltage_settled(chrg_list->chrg_dev[CP_MASTER],&vbus_val);
		        }

			if (!rc){
				vbus_volt_new = vbus_val;

				mmi_chrg_err(chip,"vbus_volt_new: %d, vbus_volt: %d", vbus_volt_new, vbus_volt);
				if(vbus_volt_new > USB_VBUS_IRQ_THRESHOLD &&
				   ((count > 0 && (vbus_volt_new > vbus_volt)) ||
				    (count < 0 && (vbus_volt_new < vbus_volt)))){
					mmi_chrg_err(chip,"vbus modify successfully!");
					break;
				} else if(vbus_volt_new > USB_VBUS_IRQ_THRESHOLD){
					mmi_chrg_err(chip,"vbus modify failed, retry!");
					count = (count > 0) ?
					       	(count + (vbus_volt - vbus_volt_new)/QC3P_V_STEP) :
					       	(count-(vbus_volt_new - vbus_volt)/QC3P_V_STEP);
					vbus_volt = vbus_volt_new;
				} else{
					break;
				}
			} else {
				mmi_chrg_err(chip,"vbus read failed, exit!");
				break;
			}
	        }
	}

	if(sw_reduce_volt){
		mmi_chrg_err(chip,"reduce volt for SW LOOP, exit!");
		return 0;
	}

	if(vbus_volt_new > USB_VBUS_IRQ_THRESHOLD){
		chip->pd_target_volt = vbus_volt_new;
		chip->pd_request_volt = chip->pd_target_volt;
		if(chip->sys_therm_cooling){
			chip->pd_sys_therm_volt = chip->pd_target_volt;
		}
		if(chip->batt_therm_cooling){
			chip->pd_batt_therm_volt = chip->pd_target_volt;
		}

	       if (chrg_list->cp_master) {
			mmi_get_input_current(chrg_list->chrg_dev[CP_MASTER],&ibus_pump);
	       }
	       if(ibus_pump >= chip->typec_middle_current){
		       chip->pd_target_curr = ibus_pump;
	       }
	       chip->pd_request_curr = chip->pd_target_curr;
               if(chip->sys_therm_cooling){
                       chip->pd_sys_therm_curr = chip->pd_target_curr;
               }
               if(chip->batt_therm_cooling){
                       chip->pd_batt_therm_curr = chip->pd_target_curr;
               }
	}
	mmi_chrg_err(chip, "after adjust vbus volt, pd_request_volt = %d, target_vbus = %d, pd_request_curr = %d, target_cur = %d \n",
		chip->pd_request_volt, chip->pd_target_volt, chip->pd_request_curr, chip->pd_target_curr);
	mmi_chrg_err(chip, "after adjust vbus volt, sys_therm_cooling = %d, pd_sys_therm_volt = %d, pd_sys_therm_curr = %d, batt_therm_cooling = %d, pd_batt_therm_volt = %d, pd_batt_therm_curr = %d \n",
		chip->sys_therm_cooling, chip->pd_sys_therm_volt, chip->pd_sys_therm_curr, chip->batt_therm_cooling, chip->pd_batt_therm_volt, chip->pd_batt_therm_curr);
#else
        //get current dp dm count
        rc = power_supply_get_property(chip->qcom_psy,
                        POWER_SUPPLY_PROP_DP_DM, &val);
        if (rc < 0) {
                mmi_chrg_err(chip, "Couldn't read dpdm pulse count rc=%d\n", rc);
                return -EINVAL;
        } else {
                mmi_chrg_err(chip, "DP DM pulse count = %d\n", val.intval);
                pulse_cnt_curr = val.intval;
        }

        //limit not beyond max pulse
        if(real_inc_step + pulse_cnt_curr > QC3P_PULSE_COUNT_MAX) {
                real_inc_step = QC3P_PULSE_COUNT_MAX - pulse_cnt_curr;
                mmi_chrg_err(chip, "volt increase step bigger than max, real_increse_step:%d\n",real_inc_step);
        }

        if(real_dec_step > pulse_cnt_curr) {
                real_dec_step = pulse_cnt_curr;
                mmi_chrg_err(chip, "vol decrese step bigger than current pulse, set to curr_pulse:%d\n",real_dec_step);
        }

        if(real_inc_step > 0
//              && (chip->pd_target_volt >= calculated_vbus)
                && (calculated_vbus <= chip->pd_volt_max)
                && ibus_curr <= chip->pd_curr_max){  //increase

                prop.intval = POWER_SUPPLY_DP_DM_DP_PULSE;
                mmi_chrg_err(chip, "increase voltage pulse:%d\n", real_inc_step);
                for(i=0;i<real_inc_step;i++) {
                        rc = power_supply_set_property(chip->qcom_psy,
                                        POWER_SUPPLY_PROP_DP_DM, &prop);
                        if (rc < 0) {
                                mmi_chrg_err(chip, "Couldn't set dp pulse\n");
                        }
                        udelay(5000);
                }
                if(chip->pd_request_volt_prev != 0) {
                        chip->pd_request_volt = chip->pd_request_volt_prev + real_inc_step*QC3P_V_STEP;
                        chip->pd_target_volt = chip->pd_request_volt;
                }
        } else {
                prop.intval = POWER_SUPPLY_DP_DM_DM_PULSE;
                mmi_chrg_err(chip, "decrease volt pulse:%d\n", real_dec_step);
                for(i=0;i<real_dec_step;i++) {
                        rc = power_supply_set_property(chip->qcom_psy,
                                        POWER_SUPPLY_PROP_DP_DM, &prop);
                        if (rc < 0) {
                                mmi_chrg_err(chip, "Couldn't set dm pulse\n");
                        }
                        udelay(5000);
                }
                if(chip->pd_request_volt_prev != 0) {
                        chip->pd_request_volt = chip->pd_request_volt_prev - real_dec_step*QC3P_V_STEP;
                        chip->pd_target_volt = chip->pd_request_volt;
                }
        }
#endif
        return 0;
}
#endif

struct mtk_chgdev_desc {
	enum mmi_chrg_dev type;
	const char *name;
	bool must_exist;
};

static struct mtk_chgdev_desc mtk_chgdev_desc_tbl[CHRG_NUM] = {
	{
		.type = PMIC_SW,
		.name = "primary_chg",
		.must_exist = true,
	},
	{
		.type = CP_MASTER,
		.name = "primary_dvchg",
		.must_exist = true,
	},
	{
		.type = CP_SLAVE,
		.name = "secondary_dvchg",
		.must_exist = false,
	},
};

int mmi_chrg_policy_init(struct mmi_charger_manager *chip,
					struct mmi_chrg_dts_info *chrg_dts,
					int chrg_cnt) {
	int i = 0, ops_cnt = 0, chrg_idx = 0;
	struct mmi_charger_device *chrg_dev;
	struct charger_device *mtk_chgdev;

	if (!chip ||!chrg_dts) {
		mmi_chrg_err(chip, "invalid input param!\n");
		return -EINVAL;
	}

	ops_cnt = ARRAY_SIZE(dev_ops);
	if (chrg_cnt > ops_cnt ||
		chrg_cnt > chip->mmi_chrg_dev_num) {
		mmi_chrg_err(chip, "invalid input param!, chrg_cnt %d , ops_cnt %d, "
						"mmi_chrg_dev_num %d\n",
			chrg_cnt, ops_cnt, chip->mmi_chrg_dev_num);
		return -EINVAL;
	}

	mmi_chrg_err(chip, "chrg_cnt %d\n", chrg_cnt);
	for (i = 0; i < chrg_cnt; i++) {
		if (!strcmp(chrg_dts[i].chrg_name, dev_ops[i].dev_name)) {
			mtk_chgdev = get_charger_by_name(mtk_chgdev_desc_tbl[i].name);
			chrg_dev = mmi_charger_device_register(chrg_dts[i].chrg_name,
				chrg_dts[i].psy_name, chip->dev, chip,
				dev_ops[i].ops, mtk_chgdev);
			if (IS_ERR_OR_NULL(chrg_dev)
				|| !is_charger_exist(chrg_dts[i].chrg_name)) {
				mmi_chrg_err(chip,
					"register mmi charger %s failed\n",
					chrg_dts[i].chrg_name);
				return -EINVAL;
			} else {
				mmi_chrg_info(chip,
				"register mmi charger %s successfully, i %d, chrg_idx %d\n",
				chrg_dts[i].chrg_name, i, chrg_idx);
				chrg_dev->charging_curr_limited =
					chrg_dts[i].charging_curr_limited;
				mmi_chrg_err(chip, "charging_curr_limited %d\n",
					chrg_dts[i].charging_curr_limited);
				chrg_dev->charging_curr_min =
					chrg_dts[i].charging_curr_min;
				mmi_chrg_err(chip, "charging_curr_min %d, chrg_id %d\n",
								chrg_dts[i].charging_curr_min, chrg_idx);
				chip->chrg_list[chrg_idx] = chrg_dev;
				mmi_chrg_err(chip, "--- over ----\n");
				chrg_idx++;
			}
		}
	}

	mmi_chrg_info(chip,"chrg_cnt %d, ops_cnt %d, mmi_chrg_dev_num %d, "
				"chrg_idx %d\n",
				chrg_cnt, ops_cnt, chip->mmi_chrg_dev_num, chrg_idx);

	if (chrg_idx != chip->mmi_chrg_dev_num
		&& chrg_idx > 0) {

		mmi_chrg_err(chip, "chrg_id %d != charger num %d\n",
						chrg_idx, chip->mmi_chrg_dev_num);
		for (i = 0; i < chrg_idx; i++)
			mmi_charger_device_unregister(chip->chrg_list[i]);
		return -EINVAL;
	}

	chrg_dev_init(chip, &g_chrg_list);
	chip->pps_volt_comp = PPS_INIT_VOLT_COMP;
	INIT_DELAYED_WORK(&chip->mmi_chrg_sm_work, mmi_chrg_sm_work_func);
	return 0;
}

