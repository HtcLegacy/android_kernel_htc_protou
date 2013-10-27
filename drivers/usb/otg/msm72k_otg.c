/* Copyright (c) 2009-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include<linux/fastchg.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/pm_runtime.h>

#include <linux/device.h>
#include <linux/pm_qos.h>

#include <mach/msm_hsusb_hw.h>
#include <mach/msm72k_otg.h>
#include <mach/msm_hsusb.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <mach/clk.h>
#include <mach/msm_xo.h>
#include <linux/usb/htc_info.h>
#include <linux/usb/hcd.h>

#include <mach/cable_detect.h>

#define MSM_USB_BASE	(dev->regs)
#define USB_LINK_RESET_TIMEOUT	(msecs_to_jiffies(10))
#define DRIVER_NAME	"msm_otg"
static void otg_reset(struct otg_transceiver *xceiv, int phy_reset);
void msm_otg_set_id_state(int id);
void msm_otg_set_vbus_state(int online);
static int htc_otg_vbus, force_host_mode;
int USB_disabled;
struct msm_otg *the_msm_otg;

enum {
    NOT_ON_AUTOBOT,
    DOCK_ON_AUTOBOT,
    HTC_MODE_RUNNING
};

int htc_get_accessory_state(void)
{
	return 0;
}


static DEFINE_MUTEX(notify_sem);
static DEFINE_MUTEX(smwork_sem);
static void send_usb_connect_notify(struct work_struct *w)
{
	static struct t_usb_status_notifier *notifier;
	struct msm_otg *motg = container_of(w, struct msm_otg,  notifier_work);
	if (!motg)
		return;

	motg->connect_type_ready = 1;
	USBH_INFO("send connect type %d\n", motg->connect_type);
	mutex_lock(&notify_sem);
#ifdef CONFIG_CABLE_DETECT_ACCESSORY
        if (cable_get_accessory_type() == DOCK_STATE_DMB) {
                motg->connect_type = CONNECT_TYPE_CLEAR;
                USBH_INFO("current accessory is DMB, send  %d\n", motg->connect_type);
        }
#endif
	list_for_each_entry(notifier, &g_lh_usb_notifier_list, notifier_link) {
		if (notifier->func != NULL) {
			
			
			if (motg->connect_type == CONNECT_TYPE_UNKNOWN)
				notifier->func(CONNECT_TYPE_USB);
			else
				notifier->func(motg->connect_type);
		}
	}
	mutex_unlock(&notify_sem);
}

int usb_register_notifier(struct t_usb_status_notifier *notifier)
{
	if (!notifier || !notifier->name || !notifier->func)
		return -EINVAL;

	mutex_lock(&notify_sem);
	list_add(&notifier->notifier_link,
			&g_lh_usb_notifier_list);
	mutex_unlock(&notify_sem);
	return 0;
}

int usb_is_connect_type_ready(void)
{
	if (!the_msm_otg)
		return 0;
	return the_msm_otg->connect_type_ready;
}
EXPORT_SYMBOL(usb_is_connect_type_ready);

int usb_get_connect_type(void)
{
	if (!the_msm_otg)
		return 0;
#ifdef CONFIG_MACH_VERDI_LTE
	if (the_msm_otg->connect_type == CONNECT_TYPE_USB_9V_AC)
		return CONNECT_TYPE_9V_AC;
#endif
	return the_msm_otg->connect_type;
}
EXPORT_SYMBOL(usb_get_connect_type);

void msm_hsusb_vbus_notif_register(void (*vbus_notif)(int))
{
	struct msm_otg *motg = the_msm_otg;
	if (motg) {
		motg->vbus_notification_cb = vbus_notif;
		USBH_INFO("%s: success\n", __func__);
	}
}

static int is_host(void)
{
	struct msm_otg *dev = the_msm_otg;

	if (dev->pmic_id_notif_supp)
		return dev->pmic_id_status ? 0 : 1;
#ifdef CONFIG_USB_OTG_HOST
	else if (dev->pdata->otg_mode == OTG_ID)
		return (OTGSC_ID & readl(USB_OTGSC)) ? 0 : 1;
#endif
	else
		return (dev->otg.state >= OTG_STATE_A_IDLE);
}

static int is_b_sess_vld(void)
{
	struct msm_otg *dev = the_msm_otg;
#if 0
	if (dev->pdata->otg_mode == OTG_ID)
		return (OTGSC_BSV & readl(USB_OTGSC)) ? 1 : 0;
	else
		return (dev->otg.state == OTG_STATE_B_PERIPHERAL);
#endif
	USBH_INFO("%s: htc_otg_vbus:%d, otgsc_bsv:%d\n", __func__,
		htc_otg_vbus, (OTGSC_BSV & readl(USB_OTGSC)) ? 1 : 0);
	return htc_otg_vbus;
}

static unsigned ulpi_read(struct msm_otg *dev, unsigned reg)
{
	unsigned ret, timeout = 100000;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);

	
	writel(ULPI_RUN | ULPI_READ | ULPI_ADDR(reg),
	       USB_ULPI_VIEWPORT);

	
	while ((readl(USB_ULPI_VIEWPORT) & ULPI_RUN) && (--timeout))
		cpu_relax();

	if (timeout == 0) {
		USBH_WARNING("ulpi_read: timeout %08x\n",
				 readl(USB_ULPI_VIEWPORT));
		spin_unlock_irqrestore(&dev->lock, flags);
		return 0xffffffff;
	}
	ret = ULPI_DATA_READ(readl(USB_ULPI_VIEWPORT));

	spin_unlock_irqrestore(&dev->lock, flags);

	return ret;
}

static int ulpi_write(struct msm_otg *dev, unsigned val, unsigned reg)
{
	unsigned timeout = 10000;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);

	
	writel(ULPI_RUN | ULPI_WRITE |
	       ULPI_ADDR(reg) | ULPI_DATA(val),
	       USB_ULPI_VIEWPORT);

	
	while ((readl(USB_ULPI_VIEWPORT) & ULPI_RUN) && (--timeout))
		;

	if (timeout == 0) {
		USBH_WARNING("ulpi_write: timeout\n");
		spin_unlock_irqrestore(&dev->lock, flags);
		return -1;
	}

	spin_unlock_irqrestore(&dev->lock, flags);

	return 0;
}

static int usb_ulpi_write(struct otg_transceiver *xceiv, u32 val, u32 reg)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);

	return ulpi_write(dev, val, reg);
}

static int usb_ulpi_read(struct otg_transceiver *xceiv, u32 reg)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);

	return ulpi_read(dev, reg);
}

#ifdef CONFIG_USB_EHCI_MSM_72K
static void enable_idgnd(struct msm_otg *dev)
{
	unsigned temp;

	
	if (dev->pdata->otg_mode == OTG_USER_CONTROL)
		return;

	ulpi_write(dev, (1<<4), 0x0E);
	ulpi_write(dev, (1<<4), 0x11);
	ulpi_write(dev, (1<<0), 0x0B);
	temp = OTGSC_IDIE | OTGSC_IDPU;
	writel_relaxed(readl_relaxed(USB_OTGSC) | temp, USB_OTGSC);
}

static void disable_idgnd(struct msm_otg *dev)
{
	unsigned temp;

	
	if (dev->pdata->otg_mode == OTG_USER_CONTROL)
		return;
	temp = OTGSC_IDIE | OTGSC_IDPU;
	writel_relaxed(readl_relaxed(USB_OTGSC) & ~temp, USB_OTGSC);
	ulpi_write(dev, (1<<4), 0x0F);
	ulpi_write(dev, (1<<4), 0x12);
	ulpi_write(dev, (1<<0), 0x0C);
}
#else
static void enable_idgnd(struct msm_otg *dev)
{
}
static void disable_idgnd(struct msm_otg *dev)
{
}
#endif

static void enable_idabc(struct msm_otg *dev)
{
#ifdef CONFIG_USB_MSM_ACA
	ulpi_write(dev, (1<<5), 0x0E);
	ulpi_write(dev, (1<<5), 0x11);
#endif
}
static void disable_idabc(struct msm_otg *dev)
{
#ifdef CONFIG_USB_MSM_ACA
	ulpi_write(dev, (1<<5), 0x0F);
	ulpi_write(dev, (1<<5), 0x12);
#endif
}

static void enable_sess_valid(struct msm_otg *dev)
{
	
	if (dev->pdata->otg_mode == OTG_USER_CONTROL)
		return;

	ulpi_write(dev, (1<<2), 0x0E);
	ulpi_write(dev, (1<<2), 0x11);
	writel(readl(USB_OTGSC) | OTGSC_BSVIE, USB_OTGSC);
}

static void disable_sess_valid(struct msm_otg *dev)
{
	
	if (dev->pdata->otg_mode == OTG_USER_CONTROL)
		return;

	ulpi_write(dev, (1<<2), 0x0F);
	ulpi_write(dev, (1<<2), 0x12);
	writel(readl(USB_OTGSC) & ~OTGSC_BSVIE, USB_OTGSC);
}
#ifdef CONFIG_USB_MSM_ACA
static void set_aca_id_inputs(struct msm_otg *dev)
{
	u8		phy_ints;

	phy_ints = ulpi_read(dev, 0x13);
	if (phy_ints == -ETIMEDOUT)
		return;

	pr_debug("phy_ints = %x\n", phy_ints);
	clear_bit(ID_A, &dev->inputs);
	clear_bit(ID_B, &dev->inputs);
	clear_bit(ID_C, &dev->inputs);
	if (phy_id_state_a(phy_ints)) {
		pr_debug("ID_A set\n");
		set_bit(ID_A, &dev->inputs);
		set_bit(A_BUS_REQ, &dev->inputs);
	} else if (phy_id_state_b(phy_ints)) {
		pr_debug("ID_B set\n");
		set_bit(ID_B, &dev->inputs);
	} else if (phy_id_state_c(phy_ints)) {
		pr_debug("ID_C set\n");
		set_bit(ID_C, &dev->inputs);
	}
	if (is_b_sess_vld())
		set_bit(B_SESS_VLD, &dev->inputs);
	else
		clear_bit(B_SESS_VLD, &dev->inputs);
}
#define get_aca_bmaxpower(dev)		(dev->b_max_power)
#define set_aca_bmaxpower(dev, power)	(dev->b_max_power = power)
#else
static void set_aca_id_inputs(struct msm_otg *dev)
{
}
#define get_aca_bmaxpower(dev)		0
#define set_aca_bmaxpower(dev, power)
#endif
static inline void set_pre_emphasis_level(struct msm_otg *dev)
{
	unsigned res = 0;

	if (!dev->pdata || dev->pdata->pemp_level == PRE_EMPHASIS_DEFAULT)
		return;

	res = ulpi_read(dev, ULPI_CONFIG_REG3);
	res &= ~(ULPI_PRE_EMPHASIS_MASK);
	if (dev->pdata->pemp_level != PRE_EMPHASIS_DISABLE)
		res |= dev->pdata->pemp_level;
	ulpi_write(dev, res, ULPI_CONFIG_REG3);
}

static inline void set_hsdrv_slope(struct msm_otg *dev)
{
	unsigned res = 0;

	if (!dev->pdata || dev->pdata->hsdrvslope == HS_DRV_SLOPE_DEFAULT)
		return;

	res = ulpi_read(dev, ULPI_CONFIG_REG3);
	res &= ~(ULPI_HSDRVSLOPE_MASK);
	res |= (dev->pdata->hsdrvslope & ULPI_HSDRVSLOPE_MASK);
	ulpi_write(dev, res, ULPI_CONFIG_REG3);
}

static inline void set_cdr_auto_reset(struct msm_otg *dev)
{
	unsigned res = 0;

	if (!dev->pdata || dev->pdata->cdr_autoreset == CDR_AUTO_RESET_DEFAULT)
		return;

	res = ulpi_read(dev, ULPI_DIGOUT_CTRL);
	if (dev->pdata->cdr_autoreset == CDR_AUTO_RESET_ENABLE)
		res &=  ~ULPI_CDR_AUTORESET;
	else
		res |=  ULPI_CDR_AUTORESET;
	ulpi_write(dev, res, ULPI_DIGOUT_CTRL);
}

static inline void set_se1_gating(struct msm_otg *dev)
{
	unsigned res = 0;

	if (!dev->pdata || dev->pdata->se1_gating == SE1_GATING_DEFAULT)
		return;

	res = ulpi_read(dev, ULPI_DIGOUT_CTRL);
	if (dev->pdata->se1_gating == SE1_GATING_ENABLE)
		res &=  ~ULPI_SE1_GATE;
	else
		res |=  ULPI_SE1_GATE;
	ulpi_write(dev, res, ULPI_DIGOUT_CTRL);
}
static inline void set_driver_amplitude(struct msm_otg *dev)
{
	unsigned res = 0;

	if (!dev->pdata || dev->pdata->drv_ampl == HS_DRV_AMPLITUDE_DEFAULT)
		return;

	res = ulpi_read(dev, ULPI_CONFIG_REG2);
	res &= ~ULPI_DRV_AMPL_MASK;
	if (dev->pdata->drv_ampl != HS_DRV_AMPLITUDE_ZERO_PERCENT)
		res |= dev->pdata->drv_ampl;
	ulpi_write(dev, res, ULPI_CONFIG_REG2);
}

static const char *state_string(enum usb_otg_state state)
{
	switch (state) {
	case OTG_STATE_A_IDLE:		return "a_idle";
	case OTG_STATE_A_WAIT_VRISE:	return "a_wait_vrise";
	case OTG_STATE_A_WAIT_BCON:	return "a_wait_bcon";
	case OTG_STATE_A_HOST:		return "a_host";
	case OTG_STATE_A_SUSPEND:	return "a_suspend";
	case OTG_STATE_A_PERIPHERAL:	return "a_peripheral";
	case OTG_STATE_A_WAIT_VFALL:	return "a_wait_vfall";
	case OTG_STATE_A_VBUS_ERR:	return "a_vbus_err";
	case OTG_STATE_B_IDLE:		return "b_idle";
	case OTG_STATE_B_SRP_INIT:	return "b_srp_init";
	case OTG_STATE_B_PERIPHERAL:	return "b_peripheral";
	case OTG_STATE_B_WAIT_ACON:	return "b_wait_acon";
	case OTG_STATE_B_HOST:		return "b_host";
	default:			return "UNDEFINED";
	}
}

static const char *timer_string(int bit)
{
	switch (bit) {
	case A_WAIT_VRISE:		return "a_wait_vrise";
	case A_WAIT_VFALL:		return "a_wait_vfall";
	case B_SRP_FAIL:		return "b_srp_fail";
	case A_WAIT_BCON:		return "a_wait_bcon";
	case A_AIDL_BDIS:		return "a_aidl_bdis";
	case A_BIDL_ADIS:		return "a_bidl_adis";
	case B_ASE0_BRST:		return "b_ase0_brst";
	default:			return "UNDEFINED";
	}
}

static const char *chg_to_string(enum chg_type chg_type)
{
	switch (chg_type) {
	case USB_CHG_TYPE__SDP:		return "CHG_TYPE__SDP";
	case USB_CHG_TYPE__CARKIT:	return "CHG_TYPE__CARKIT";
	case USB_CHG_TYPE__WALLCHARGER:	return "CHG_TYPE__WALLCHARGER";
	default:			return "CHG_TYPE__INVALID";
	}
}

static const char *connect_to_string(enum usb_connect_type connect_type)
{
	switch (connect_type) {
	case CONNECT_TYPE_CLEAR:	return "CONNECT_CLEAR";
	case CONNECT_TYPE_UNKNOWN:	return "CONNECT_UNKNOWN";
	case CONNECT_TYPE_NONE:		return "CONNECT_NONE";
	case CONNECT_TYPE_USB:  	return "CONNECT_USB";
	case CONNECT_TYPE_AC:  		return "CONNECT_AC";
	case CONNECT_TYPE_9V_AC: 	return "CONNECT_9V_AC";
	case CONNECT_TYPE_WIRELESS:	return "CONNECT_WIRELESS";
	case CONNECT_TYPE_INTERNAL:	return "CONNECT_INTERNAL";
	case CONNECT_TYPE_UNSUPPORTED:	return "CONNECT_UNSUPPORTED";
	default:			return "CONNECT_INVALID";
	}
}

static void otg_pm_qos_update_latency(struct msm_otg *dev, int vote)
{
	struct msm_otg_platform_data *pdata = dev->pdata;
	u32 swfi_latency = 0;

	if (pdata)
		swfi_latency = pdata->swfi_latency + 1;

	if (vote)
		pm_qos_update_request(&pdata->pm_qos_req_dma,
				swfi_latency);
	else
		pm_qos_update_request(&pdata->pm_qos_req_dma,
				PM_QOS_DEFAULT_VALUE);
}

static enum hrtimer_restart msm_otg_timer_func(struct hrtimer *_timer)
{
	struct msm_otg *dev = container_of(_timer, struct msm_otg, timer);

	if (dev->active_tmout == A_WAIT_VRISE)
		set_bit(A_VBUS_VLD, &dev->inputs);
	else
		set_bit(dev->active_tmout, &dev->tmouts);

	pr_debug("expired %s timer\n", timer_string(dev->active_tmout));
	queue_work(dev->wq, &dev->sm_work);
	return HRTIMER_NORESTART;
}

static void msm_otg_del_timer(struct msm_otg *dev)
{
	int bit = dev->active_tmout;

	pr_debug("deleting %s timer. remaining %lld msec \n", timer_string(bit),
			div_s64(ktime_to_us(hrtimer_get_remaining(&dev->timer)),
					1000));
	hrtimer_cancel(&dev->timer);
	clear_bit(bit, &dev->tmouts);
}

static void msm_otg_start_timer(struct msm_otg *dev, int time, int bit)
{
	clear_bit(bit, &dev->tmouts);
	dev->active_tmout = bit;
	pr_debug("starting %s timer\n", timer_string(bit));
	hrtimer_start(&dev->timer,
			ktime_set(time / 1000, (time % 1000) * 1000000),
			HRTIMER_MODE_REL);
}

static void msm_otg_init_timer(struct msm_otg *dev)
{
	hrtimer_init(&dev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	dev->timer.function = msm_otg_timer_func;
}

static const char *event_string(enum usb_otg_event event)
{
	switch (event) {
	case OTG_EVENT_DEV_CONN_TMOUT:
		return "DEV_CONN_TMOUT";
	case OTG_EVENT_NO_RESP_FOR_HNP_ENABLE:
		return "NO_RESP_FOR_HNP_ENABLE";
	case OTG_EVENT_HUB_NOT_SUPPORTED:
		return "HUB_NOT_SUPPORTED";
	case OTG_EVENT_DEV_NOT_SUPPORTED:
		return "DEV_NOT_SUPPORTED,";
	case OTG_EVENT_HNP_FAILED:
		return "HNP_FAILED";
	case OTG_EVENT_NO_RESP_FOR_SRP:
		return "NO_RESP_FOR_SRP";
	default:
		return "UNDEFINED";
	}
}

static int msm_otg_send_event(struct otg_transceiver *xceiv,
				enum usb_otg_event event)
{
	char module_name[16];
	char udev_event[128];
	char *envp[] = { module_name, udev_event, NULL };
	int ret;

	pr_debug("sending %s event\n", event_string(event));

	snprintf(module_name, 16, "MODULE=%s", DRIVER_NAME);
	snprintf(udev_event, 128, "EVENT=%s", event_string(event));
	ret = kobject_uevent_env(&xceiv->dev->kobj, KOBJ_CHANGE, envp);
	if (ret < 0)
		pr_info("uevent sending failed with ret = %d\n", ret);
	return ret;
}

static int msm_otg_start_hnp(struct otg_transceiver *xceiv)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);
	enum usb_otg_state state;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	state = dev->otg.state;
	spin_unlock_irqrestore(&dev->lock, flags);

	if (state != OTG_STATE_A_HOST) {
		USBH_ERR("HNP can not be initiated in %s state\n",
				state_string(state));
		return -EINVAL;
	}

	pr_debug("A-Host: HNP initiated\n");
	clear_bit(A_BUS_REQ, &dev->inputs);
	wake_lock(&dev->wlock);
	queue_work(dev->wq, &dev->sm_work);
	return 0;
}

static int msm_otg_start_srp(struct otg_transceiver *xceiv)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);
	u32	val;
	int ret = 0;
	enum usb_otg_state state;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	state = dev->otg.state;
	spin_unlock_irqrestore(&dev->lock, flags);

	if (state != OTG_STATE_B_IDLE) {
		USBH_ERR("SRP can not be initiated in %s state\n",
				state_string(state));
		ret = -EINVAL;
		goto out;
	}

	if ((jiffies - dev->b_last_se0_sess) < msecs_to_jiffies(TB_SRP_INIT)) {
		pr_debug("initial conditions of SRP are not met. Try again"
				"after some time\n");
		ret = -EAGAIN;
		goto out;
	}

	val = readl(USB_OTGSC);
	writel((val & ~OTGSC_INTR_STS_MASK) | OTGSC_HADP, USB_OTGSC);

	
out:
	return ret;
}

static int msm_otg_set_power(struct otg_transceiver *xceiv, unsigned mA)
{
	static enum chg_type 	curr_chg = USB_CHG_TYPE__INVALID;
	struct msm_otg		*dev = container_of(xceiv, struct msm_otg, otg);
	struct msm_otg_platform_data *pdata = dev->pdata;
	enum chg_type 		new_chg = atomic_read(&dev->chg_type);
	unsigned 		charge = mA;

	
	if (new_chg != curr_chg && pdata->chg_connected) {
		curr_chg = new_chg;
		pdata->chg_connected(new_chg);
	}

	
	if (test_bit(ID_C, &dev->inputs) ||
				test_bit(ID_B, &dev->inputs))
		charge = USB_IDCHG_MAX;

	if (dev->curr_power == charge)
		return 0;

#if 0
	pr_debug("Charging with %dmA current\n", charge);
#endif
	if (pdata->chg_vbus_draw && new_chg != USB_CHG_TYPE__INVALID &&
		(charge || new_chg != USB_CHG_TYPE__WALLCHARGER))
			pdata->chg_vbus_draw(charge);

	dev->curr_power = charge;

	if (new_chg == USB_CHG_TYPE__WALLCHARGER) {
		wake_lock(&dev->wlock);
		queue_work(dev->wq, &dev->sm_work);
	}

	return 0;
}

static void ac_detect_expired(unsigned long _data)
{
	u32 delay = 0;
	struct msm_otg *dev = the_msm_otg;

	USBH_INFO("%s: count = %d, connect_type = %d\n", __func__,
			dev->ac_detect_count, dev->connect_type);

	if (dev->connect_type == CONNECT_TYPE_USB || dev->ac_detect_count >= 3)
		return;

	
	if ((readl(USB_PORTSC) & PORTSC_LS) != PORTSC_LS) {
#ifdef CONFIG_CABLE_DETECT_ACCESSORY
		if (cable_get_accessory_type() == DOCK_STATE_CAR) {
			USBH_INFO("car mode charger\n");
			atomic_set(&dev->chg_type, USB_CHG_TYPE__WALLCHARGER);
			dev->connect_type = CONNECT_TYPE_AC;
			dev->ac_detect_count = 0;

			queue_work(dev->wq, &dev->notifier_work);
			queue_work(dev->wq, &dev->sm_work);
			return;
		}
#endif
		dev->ac_detect_count++;
		if (dev->ac_detect_count == 1)
			delay = 5 * HZ;
		else if (dev->ac_detect_count == 2)
			delay = 10 * HZ;

		mod_timer(&dev->ac_detect_timer, jiffies + delay);
	} else {
		USBH_INFO("AC charger\n");
		atomic_set(&dev->chg_type, USB_CHG_TYPE__WALLCHARGER);
		dev->connect_type = CONNECT_TYPE_AC;
		dev->ac_detect_count = 0;

		queue_work(dev->wq, &dev->notifier_work);
		queue_work(dev->wq, &dev->sm_work);
	}
}

static void msm_otg_notify_charger_attached(int connect_type)
{
	struct msm_otg *motg = the_msm_otg;

	USBH_INFO("chg_type: %s %s => %s\n",
		chg_to_string(atomic_read(&motg->chg_type)),
		connect_to_string(motg->connect_type),
		connect_to_string(connect_type));

	switch (connect_type) {
	case CONNECT_TYPE_UNKNOWN:
		if (atomic_read(&motg->chg_type) != USB_CHG_TYPE__SDP)
			atomic_set(&motg->chg_type, USB_CHG_TYPE__SDP);

		if (motg->connect_type != CONNECT_TYPE_USB) {
			motg->connect_type = CONNECT_TYPE_UNKNOWN;
			motg->ac_detect_count = 0;
			mod_timer(&motg->ac_detect_timer, jiffies + (3 * HZ));
		} else {
			
			return;
		}
		break;
	case CONNECT_TYPE_USB:
if(!force_fast_charge){
		printk("USB connected");
		if (atomic_read(&motg->chg_type) != USB_CHG_TYPE__SDP)
			atomic_set(&motg->chg_type, USB_CHG_TYPE__SDP);
		motg->ac_detect_count = 0;
		del_timer(&motg->ac_detect_timer);

		if (motg->connect_type != CONNECT_TYPE_USB) {
			motg->connect_type = CONNECT_TYPE_USB;
		} else {
			
			return;
		}}
else if (force_fast_charge)
{
	printk("AC connected");
		if (atomic_read(&motg->chg_type) != USB_CHG_TYPE__WALLCHARGER)
			atomic_set(&motg->chg_type, USB_CHG_TYPE__WALLCHARGER);
		motg->connect_type = CONNECT_TYPE_AC;
}
		break;
	case CONNECT_TYPE_AC:
		printk("AC connected");
		if (atomic_read(&motg->chg_type) != USB_CHG_TYPE__WALLCHARGER)
			atomic_set(&motg->chg_type, USB_CHG_TYPE__WALLCHARGER);
		motg->connect_type = CONNECT_TYPE_AC;
		break;
	}
	queue_work(motg->wq, &motg->notifier_work);
}


static int msm_otg_set_clk(struct otg_transceiver *xceiv, int on)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);

	if (!dev || (dev != the_msm_otg))
		return -ENODEV;

	if (on)
		
		clk_prepare_enable(dev->alt_core_clk);
	else
		clk_disable_unprepare(dev->alt_core_clk);

	return 0;
}
static void msm_otg_start_peripheral(struct otg_transceiver *xceiv, int on)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);
	struct msm_otg_platform_data *pdata = dev->pdata;

	if (!xceiv->gadget)
		return;

	if (on) {
		if (pdata->setup_gpio)
			pdata->setup_gpio(USB_SWITCH_PERIPHERAL);
		otg_pm_qos_update_latency(dev, 1);

		if (dev->pdata->pclk_required_during_lpm)
			clk_prepare_enable(dev->iface_clk);

		usb_gadget_vbus_connect(xceiv->gadget);
	} else {
		atomic_set(&dev->chg_type, USB_CHG_TYPE__INVALID);
		usb_gadget_vbus_disconnect(xceiv->gadget);

		if (dev->pdata->pclk_required_during_lpm)
			clk_disable_unprepare(dev->iface_clk);

		otg_pm_qos_update_latency(dev, 0);
		if (pdata->setup_gpio)
			pdata->setup_gpio(USB_SWITCH_DISABLE);
	}
}

static void msm_otg_start_host(struct otg_transceiver *xceiv, int on)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);
	struct msm_otg_platform_data *pdata = dev->pdata;

	if (!xceiv->host)
		return;
	if (on)
		USBH_DEBUG("host on\n");
	else
		USBH_DEBUG("host off\n");

	if (dev->start_host) {
		
		if (on && pdata->setup_gpio)
			pdata->setup_gpio(USB_SWITCH_HOST);

		if (dev->pdata->pclk_required_during_lpm) {
			if (on)
				clk_prepare_enable(dev->iface_clk);
			else
				clk_disable_unprepare(dev->iface_clk);
		}

		dev->start_host(xceiv->host, on);

		if (!on && pdata->setup_gpio)
			pdata->setup_gpio(USB_SWITCH_DISABLE);
	}
}

static int msm_otg_suspend(struct msm_otg *dev)
{
	unsigned long timeout;
	bool host_bus_suspend;
	unsigned ret;
	enum chg_type chg_type = atomic_read(&dev->chg_type);
	unsigned long flags;
	u32 cmd_val;

	disable_irq(dev->irq);
	if (atomic_read(&dev->in_lpm))
		goto out;
#ifdef CONFIG_USB_MSM_ACA
#ifndef CONFIG_USB_MSM_STANDARD_ACA
	if ((test_bit(ID, &dev->inputs) && test_bit(B_SESS_VLD, &dev->inputs) &&
			chg_type != USB_CHG_TYPE__WALLCHARGER) ||
			test_bit(ID_A, &dev->inputs))
		goto out;
#endif
	
	disable_idabc(dev);
#endif
	ulpi_read(dev, 0x14);

	host_bus_suspend = dev->otg.host && is_host();

	if (dev->pdata->pmic_id_notif_init && !host_bus_suspend &&
		!test_bit(ID_A, &dev->inputs)) {
		disable_idgnd(dev);
		ret = dev->pdata->pmic_id_notif_init(
			&msm_otg_set_id_state, 1);
		if (!ret) {
			dev->pmic_id_notif_supp = 1;
			if (dev->pdata->pmic_id_irq)
				dev->id_irq = dev->pdata->pmic_id_irq;
		} else if (ret == -ENOTSUPP) {
			pr_debug("%s:USB ID is not routed to pmic",
			__func__);
			enable_idgnd(dev);
		} else {
			pr_err("%s: pmic_id_ notif_init failed err:%d",
				__func__, ret);
		}
	}

	if ((dev->otg.gadget && chg_type == USB_CHG_TYPE__WALLCHARGER) ||
		host_bus_suspend ||
		(dev->otg.host && !dev->pmic_id_notif_supp) ||
		(dev->otg.gadget && !dev->pmic_vbus_notif_supp)) {
		ulpi_write(dev, 0x01, 0x30);
	}

	ulpi_write(dev, 0x08, 0x09);

	timeout = jiffies + msecs_to_jiffies(500);
	disable_phy_clk();
	while (!is_phy_clk_disabled()) {
		if (time_after(jiffies, timeout)) {
			USBH_ERR("%s: Unable to suspend phy\n", __func__);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_UNDEFINED;
			spin_unlock_irqrestore(&dev->lock, flags);
			queue_work(dev->wq, &dev->sm_work);
			goto out;
		}
		msleep(1);
		
		if (((readl(USB_OTGSC) & OTGSC_INTR_MASK) >> 8) &
				readl(USB_OTGSC)) {
			enable_idabc(dev);
			goto out;
		}
	}

	if (dev->otg.state == OTG_STATE_A_WAIT_BCON) {
		cmd_val = readl_relaxed(USB_USBCMD);
		if (host_bus_suspend)
			cmd_val |= ASYNC_INTR_CTRL | ULPI_STP_CTRL;
		else
			cmd_val |= ULPI_STP_CTRL;
		writel_relaxed(cmd_val, USB_USBCMD);
	} else {
		
		
		writel(readl(USB_USBCMD) | ULPI_STP_CTRL, USB_USBCMD);
	}
	
	mb();

	if (dev->iface_clk)
		clk_disable_unprepare(dev->iface_clk);

	clk_disable_unprepare(dev->core_clk);
	
	ret = msm_xo_mode_vote(dev->xo_handle, MSM_XO_MODE_OFF);
	if (ret)
		USBH_ERR("%s failed to devote for"
			"TCXO D1 buffer%d\n", __func__, ret);

	if (device_may_wakeup(dev->otg.dev)) {
		enable_irq_wake(dev->irq);
		if (dev->vbus_on_irq)
			enable_irq_wake(dev->vbus_on_irq);
		if (dev->id_irq)
			enable_irq_wake(dev->id_irq);
	}

	atomic_set(&dev->in_lpm, 1);

	 if (!host_bus_suspend && dev->pmic_vbus_notif_supp &&
		!test_bit(ID_A, &dev->inputs)) {
		pr_debug("phy can power collapse: (%d)\n",
			can_phy_power_collapse(dev));
		if (can_phy_power_collapse(dev) && dev->pdata->ldo_enable) {
			pr_debug("disabling the regulators\n");
			dev->pdata->ldo_enable(0);
		}
	}

	if (dev->pdata->config_vddcx)
		dev->pdata->config_vddcx(0);
	pr_info("%s: usb in low power mode\n", __func__);

out:
	enable_irq(dev->irq);

	return 0;
}

static int msm_otg_resume(struct msm_otg *dev)
{
	unsigned temp;
	unsigned ret;

	if (!atomic_read(&dev->in_lpm))
		return 0;

	USBH_INFO("lpm exit\n");
	
	if (dev->pdata->config_vddcx) {
		ret = dev->pdata->config_vddcx(1);
		if (ret) {
			USBH_ERR("%s: unable to enable vddcx digital core:%d\n",
				__func__, ret);
		}
	}
	if (dev->pdata->ldo_set_voltage)
		dev->pdata->ldo_set_voltage(3400);

	
	ret = msm_xo_mode_vote(dev->xo_handle, MSM_XO_MODE_ON);
	if (ret)
		USBH_ERR("%s failed to vote for"
			"TCXO D1 buffer%d\n", __func__, ret);

	clk_prepare_enable(dev->core_clk);

	if (dev->iface_clk)
		clk_prepare_enable(dev->iface_clk);

	temp = readl(USB_USBCMD);
	if (dev->otg.state != OTG_STATE_A_WAIT_BCON) {
		temp &= ~ASYNC_INTR_CTRL;
	}
	temp &= ~ULPI_STP_CTRL;
	writel(temp, USB_USBCMD);

	if (device_may_wakeup(dev->otg.dev)) {
		disable_irq_wake(dev->irq);
		if (dev->vbus_on_irq)
			disable_irq_wake(dev->vbus_on_irq);
		if (dev->id_irq)
			disable_irq_wake(dev->id_irq);
	}

	atomic_set(&dev->in_lpm, 0);

	pr_info("%s: usb exited from low power mode\n", __func__);

	return 0;
}

static void msm_otg_get_resume(struct msm_otg *dev)
{
#ifdef CONFIG_PM_RUNTIME
	pm_runtime_get_noresume(dev->otg.dev);
	pm_runtime_resume(dev->otg.dev);
#else
	msm_otg_resume(dev);
#endif
}

static void msm_otg_put_suspend(struct msm_otg *dev)
{
#ifdef CONFIG_PM_RUNTIME
	pm_runtime_put_sync(dev->otg.dev);
	if (!atomic_read(&dev->in_lpm))
		pm_runtime_get_sync(dev->otg.dev);
#else
	msm_otg_suspend(dev);
#endif
}

static void msm_otg_resume_w(struct work_struct *w)
{
	struct msm_otg	*dev = container_of(w, struct msm_otg, otg_resume_work);
	unsigned long timeout;

	if (can_phy_power_collapse(dev) && dev->pdata->ldo_enable)
		dev->pdata->ldo_enable(1);

	if (pm_runtime_enabled(dev->otg.dev)) {
		msm_otg_get_resume(dev);
	} else {
		pm_runtime_get_noresume(dev->otg.dev);
		msm_otg_resume(dev);
		pm_runtime_set_active(dev->otg.dev);
	}

	if (!is_phy_clk_disabled())
		goto phy_resumed;

	timeout = jiffies + usecs_to_jiffies(100);
	enable_phy_clk();
	while (is_phy_clk_disabled() || !is_phy_active()) {
		if (time_after(jiffies, timeout)) {
			USBH_ERR("%s: Unable to wakeup phy. is_phy_active: %x\n",
				 __func__, !!is_phy_active());
			
			otg_reset(&dev->otg, 1);
			break;
		}
		udelay(10);
	}

phy_resumed:
	if (readl_relaxed(USB_OTGSC) & OTGSC_BSVIS) {
		set_bit(B_SESS_VLD, &dev->inputs);
		queue_work(dev->wq, &dev->sm_work);
	}
#if 0
	if (dev->pmic_id_notif_supp) {
		dev->pdata->pmic_id_notif_init(&msm_otg_set_id_state, 0);
		dev->pmic_id_notif_supp = 0;
		enable_idgnd(dev);
	}
#endif

	
	enable_idabc(dev);

	set_aca_id_inputs(dev);

	if (is_host() || test_bit(ID_A, &dev->inputs)) {
		writel(readl(USB_PORTSC) | PORTSC_FPR, USB_PORTSC);
		msm_otg_start_host(&dev->otg, REQUEST_RESUME);
	}

	enable_irq(dev->irq);
}

static int msm_otg_set_suspend(struct otg_transceiver *xceiv, int suspend)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);
	enum usb_otg_state state;
	unsigned long flags;

	if (!dev || (dev != the_msm_otg))
		return -ENODEV;

	spin_lock_irqsave(&dev->lock, flags);
	state = dev->otg.state;
	spin_unlock_irqrestore(&dev->lock, flags);

	pr_debug("suspend request in state: %s\n",
			state_string(state));

	if (suspend) {
		switch (state) {
#ifndef CONFIG_MSM_OTG_ENABLE_A_WAIT_BCON_TIMEOUT
		case OTG_STATE_A_WAIT_BCON:
			if (test_bit(ID_A, &dev->inputs))
				msm_otg_set_power(xceiv, USB_IDCHG_MIN - 100);
			msm_otg_put_suspend(dev);
			break;
#endif
		case OTG_STATE_A_HOST:
			clear_bit(A_BUS_REQ, &dev->inputs);
			wake_lock(&dev->wlock);
			queue_work(dev->wq, &dev->sm_work);
			break;
		case OTG_STATE_B_PERIPHERAL:
			if (xceiv->gadget->b_hnp_enable) {
				set_bit(A_BUS_SUSPEND, &dev->inputs);
				set_bit(B_BUS_REQ, &dev->inputs);
				wake_lock(&dev->wlock);
				queue_work(dev->wq, &dev->sm_work);
			}
			break;
		case OTG_STATE_A_PERIPHERAL:
			msm_otg_start_timer(dev, TA_BIDL_ADIS,
					A_BIDL_ADIS);
			break;
		default:
			break;
		}
	} else {
		unsigned long timeout;

		switch (state) {
		case OTG_STATE_A_PERIPHERAL:
			msm_otg_del_timer(dev);
			break;
		case OTG_STATE_A_SUSPEND:
			
			set_bit(A_BUS_REQ, &dev->inputs);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_HOST;
			spin_unlock_irqrestore(&dev->lock, flags);
			if (test_bit(ID_A, &dev->inputs) &&
				(get_aca_bmaxpower(dev) < USB_IDCHG_MIN))
				msm_otg_set_power(xceiv,
					USB_IDCHG_MIN - get_aca_bmaxpower(dev));
			break;
		default:
			break;
		}

		if (suspend == atomic_read(&dev->in_lpm))
			return 0;

		disable_irq(dev->irq);
		if (dev->pmic_vbus_notif_supp)
			if (can_phy_power_collapse(dev) &&
					dev->pdata->ldo_enable)
				dev->pdata->ldo_enable(1);

		msm_otg_get_resume(dev);

		if (!is_phy_clk_disabled())
			goto out;

		timeout = jiffies + usecs_to_jiffies(100);
		enable_phy_clk();
		while (is_phy_clk_disabled() || !is_phy_active()) {
			if (time_after(jiffies, timeout)) {
				USBH_ERR("%s: Unable to wakeup phy. "
					"is_phy_active: %x\n",
					__func__, !!is_phy_active());
				
				otg_reset(&dev->otg, 1);
				break;
			}
			udelay(10);
		}
#if 0
		if (dev->pmic_id_notif_supp) {
			dev->pdata->pmic_id_notif_init(
				&msm_otg_set_id_state, 0);
			dev->pmic_id_notif_supp = 0;
			enable_idgnd(dev);
		}
#endif
out:
		enable_idabc(dev);
		enable_irq(dev->irq);

	}

	return 0;
}

static int msm_otg_set_peripheral(struct otg_transceiver *xceiv,
			struct usb_gadget *gadget)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);

	if (!dev || (dev != the_msm_otg))
		return -ENODEV;

	if (!gadget) {
		msm_otg_start_peripheral(xceiv, 0);
		dev->otg.gadget = 0;
		disable_sess_valid(dev);
		if (!dev->otg.host)
			disable_idabc(dev);
		return 0;
	}
	dev->otg.gadget = gadget;
	pr_info("peripheral driver registered w/ tranceiver\n");

	wake_lock(&dev->wlock);
	queue_work(dev->wq, &dev->sm_work);
	return 0;
}

#ifdef CONFIG_USB_EHCI_MSM_72K
static int usbdev_notify(struct notifier_block *self,
			unsigned long action, void *device)
{
	enum usb_otg_state state;
	struct msm_otg *dev = container_of(self, struct msm_otg, usbdev_nb);
	struct usb_device *udev = device;
	int work = 1;
	unsigned long flags;

	if (!udev->parent || udev->parent->parent)
		goto out;

	spin_lock_irqsave(&dev->lock, flags);
	state = dev->otg.state;
	spin_unlock_irqrestore(&dev->lock, flags);

	switch (state) {
	case OTG_STATE_A_WAIT_BCON:
		if (action == USB_DEVICE_ADD) {
			pr_debug("B_CONN set\n");
			set_bit(B_CONN, &dev->inputs);
			if (udev->actconfig) {
				set_aca_bmaxpower(dev,
					udev->actconfig->desc.bMaxPower * 2);
				goto do_work;
			}
			if (udev->portnum == udev->bus->otg_port)
				set_aca_bmaxpower(dev, USB_IB_UNCFG);
			else
				set_aca_bmaxpower(dev, 100);
		}
		break;
	case OTG_STATE_A_HOST:
		if (action == USB_DEVICE_REMOVE) {
			pr_debug("B_CONN clear\n");
			clear_bit(B_CONN, &dev->inputs);
			set_aca_bmaxpower(dev, 0);
		}
		break;
	default:
		work = 0;
		break;
	}
do_work:
	if (work) {
		wake_lock(&dev->wlock);
		queue_work(dev->wq, &dev->sm_work);
	}
out:
	return NOTIFY_OK;
}

static void msm_hsusb_vbus_power(struct msm_otg *motg, bool on)
{
#if 0
	int ret;
	static bool vbus_is_on;

	if (vbus_is_on == on)
		return;

	if (motg->pdata->vbus_power) {
		ret = motg->pdata->vbus_power(on);
		if (!ret)
			vbus_is_on = on;
		return;
	}

	if (!vbus_otg) {
		pr_err("vbus_otg is NULL.");
		return;
	}

	if (on) {
		msm_otg_notify_host_mode(motg, on);
		ret = regulator_enable(vbus_otg);
		if (ret) {
			pr_err("unable to enable vbus_otg\n");
			return;
		}
		vbus_is_on = true;
	} else {
		ret = regulator_disable(vbus_otg);
		if (ret) {
			pr_err("unable to disable vbus_otg\n");
			return;
		}
		msm_otg_notify_host_mode(motg, on);
		vbus_is_on = false;
	}
#else
	static bool vbus_is_on;

	if (vbus_is_on == on)
		return;
	if (on) {
		motg->connect_type = CONNECT_TYPE_INTERNAL;
		queue_work(motg->wq, &motg->notifier_work);
		vbus_is_on = true;
	} else {
		motg->connect_type = CONNECT_TYPE_CLEAR;
		queue_work(motg->wq, &motg->notifier_work);
		vbus_is_on = false;
	}
#endif
}

static int msm_otg_set_host(struct otg_transceiver *xceiv, struct usb_bus *host)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);

	if (!dev || (dev != the_msm_otg))
		return -ENODEV;

	if (!dev->start_host)
		return -ENODEV;

	if (!host) {
		msm_otg_start_host(xceiv, REQUEST_STOP);
		usb_unregister_notify(&dev->usbdev_nb);
		dev->otg.host = 0;
		dev->start_host = 0;
		disable_idgnd(dev);
		if (!dev->otg.gadget)
			disable_idabc(dev);
		return 0;
	}
#ifdef CONFIG_USB_OTG
	host->otg_port = 1;
#endif
	dev->usbdev_nb.notifier_call = usbdev_notify;
	usb_register_notify(&dev->usbdev_nb);
	dev->otg.host = host;
	pr_info("host driver registered w/ tranceiver\n");

#ifndef CONFIG_USB_MSM_72K
	wake_lock(&dev->wlock);
	queue_work(dev->wq, &dev->sm_work);
#endif
	return 0;
}

#endif

void msm_otg_set_id_state(int id)
{
	struct msm_otg *dev = the_msm_otg;
	unsigned long flags;

	USBH_INFO("%s: %d\n", __func__, id);

	if (!dev) {
		USBH_INFO("OTG does not probe yet\n");
		return;
	}

	dev->pmic_id_status = id;
	if (id) {
		set_bit(ID, &dev->inputs);
		USBH_INFO("Set ID\n");
	} else {
		clear_bit(ID, &dev->inputs);
		USBH_INFO("Clear ID\n");
	}
	spin_lock_irqsave(&dev->lock, flags);
	if (dev->otg.state != OTG_STATE_UNDEFINED) {
		wake_lock(&dev->wlock);
		queue_work(dev->wq, &dev->sm_work);
	}
	spin_unlock_irqrestore(&dev->lock, flags);
}
#if (defined(CONFIG_USB_OTG) && defined(CONFIG_USB_OTG_HOST))
static void usb_host_cable_detect(bool cable_in)
{
	if (cable_in)
		msm_otg_set_id_state(0);
	else
		msm_otg_set_id_state(1);
}
#endif

void msm_otg_set_disable_usb(int disable_usb)
{
	struct msm_otg *dev = the_msm_otg;

	USB_disabled = disable_usb;

	queue_work(dev->wq, &dev->sm_work);
}

int msm_otg_get_vbus_state(void)
{
	return htc_otg_vbus;
}

void msm_otg_set_vbus_state(int online)
{
	struct msm_otg *dev = the_msm_otg;

	USBH_INFO("%s: %d\n", __func__, online);

	htc_otg_vbus = online;
	if (!dev) {
		USBH_INFO("OTG does not probe yet\n");
		return;
	}
	
	if (force_host_mode)
		return;
	if (online)
		set_bit(B_SESS_VLD, &dev->inputs);
	else
		clear_bit(B_SESS_VLD, &dev->inputs);

	
	if (dev->pdata->usb_uart_switch)
		dev->pdata->usb_uart_switch(!online);

	wake_lock(&dev->wlock);
	queue_work(dev->wq, &dev->sm_work);
}

static irqreturn_t msm_otg_irq(int irq, void *data)
{
	struct msm_otg *dev = data;
	u32 otgsc, sts, pc;
	irqreturn_t ret = IRQ_HANDLED;
	int work = 0;
	enum usb_otg_state state;
	unsigned long flags;

	if (atomic_read(&dev->in_lpm)) {
		disable_irq_nosync(dev->irq);
		wake_lock(&dev->wlock);
		queue_work(dev->wq, &dev->otg_resume_work);
		goto out;
	}

	
	if (dev->pdata->otg_mode == OTG_USER_CONTROL)
		return IRQ_NONE;


	otgsc = readl(USB_OTGSC);
	sts = readl(USB_USBSTS);

	if (!((otgsc & OTGSC_INTR_STS_MASK) || (sts & STS_PCI))) {
		ret = IRQ_NONE;
		goto out;
	}
	writel_relaxed(otgsc, USB_OTGSC);

	spin_lock_irqsave(&dev->lock, flags);
	state = dev->otg.state;
	spin_unlock_irqrestore(&dev->lock, flags);


	if ((otgsc & OTGSC_IDIE) && (otgsc & OTGSC_IDIS)) {
		if (otgsc & OTGSC_ID) {
			USBH_INFO("Id set\n");
			set_bit(ID, &dev->inputs);
		} else {
			USBH_INFO("Id clear\n");
			set_bit(A_BUS_REQ, &dev->inputs);
			clear_bit(ID, &dev->inputs);
		}
		work = 1;
	} else if (otgsc & OTGSC_BSVIS) {
		if (dev->vbus_notification_cb) {
			if ((state >= OTG_STATE_A_IDLE) &&
				!test_bit(ID_A, &dev->inputs))
				goto out;
			if (otgsc & OTGSC_BSV) {
				pr_debug("BSV set\n");
				dev->vbus_notification_cb(1);
				
			} else {
				pr_debug("BSV clear\n");
				dev->vbus_notification_cb(0);
				
			}
		}
	} else if (otgsc & OTGSC_DPIS) {
		pr_debug("DPIS detected\n");
		set_bit(A_SRP_DET, &dev->inputs);
		set_bit(A_BUS_REQ, &dev->inputs);
		work = 1;
	} else if (sts & STS_PCI) {
		pc = readl(USB_PORTSC);
		pr_debug("portsc = %x\n", pc);
		ret = IRQ_NONE;
		work = 1;
		switch (state) {
		case OTG_STATE_A_SUSPEND:
			if (dev->otg.host->b_hnp_enable && (pc & PORTSC_CSC) &&
					!(pc & PORTSC_CCS)) {
				pr_debug("B_CONN clear\n");
				clear_bit(B_CONN, &dev->inputs);
			}
			break;
		case OTG_STATE_B_WAIT_ACON:
			if ((pc & PORTSC_CSC) && (pc & PORTSC_CCS)) {
				pr_debug("A_CONN set\n");
				set_bit(A_CONN, &dev->inputs);
				
				msm_otg_del_timer(dev);
			}
			break;
		case OTG_STATE_B_HOST:
			if ((pc & PORTSC_CSC) && !(pc & PORTSC_CCS)) {
				pr_debug("A_CONN clear\n");
				clear_bit(A_CONN, &dev->inputs);
			}
			break;
		case OTG_STATE_B_IDLE:
		case OTG_STATE_A_IDLE:
		case OTG_STATE_A_WAIT_VRISE:
		case OTG_STATE_A_WAIT_VFALL:
		case OTG_STATE_A_WAIT_BCON:
			
			USBH_WARNING("%s: nobody handles IRQ\n", __func__);
			writel(sts, USB_USBSTS);
			work = 0;
			break;
		default:
			work = 0;
			break;
		}
	}
	if (work) {
#ifdef CONFIG_USB_MSM_ACA
		
		if ((otgsc & OTGSC_IDIS) || (otgsc & OTGSC_BSVIS))
			set_aca_id_inputs(dev);
#endif
		wake_lock(&dev->wlock);
		queue_work(dev->wq, &dev->sm_work);
	}
out:
	return ret;
}

#define ULPI_VERIFY_MAX_LOOP_COUNT  5
#define PHY_CALIB_RETRY_COUNT 10
static void phy_clk_reset(struct msm_otg *dev)
{
	unsigned rc;
	enum clk_reset_action assert = CLK_RESET_ASSERT;

	if (dev->pdata->phy_reset_sig_inverted)
		assert = CLK_RESET_DEASSERT;

	rc = clk_reset(dev->phy_reset_clk, assert);
	if (rc) {
		USBH_ERR("%s: phy clk assert failed\n", __func__);
		return;
	}

	msleep(1);

	rc = clk_reset(dev->phy_reset_clk, !assert);
	if (rc) {
		USBH_ERR("%s: phy clk deassert failed\n", __func__);
		return;
	}

	msleep(1);
}

static unsigned ulpi_read_with_reset(struct msm_otg *dev, unsigned reg)
{
	int temp;
	unsigned res;

	for (temp = 0; temp < ULPI_VERIFY_MAX_LOOP_COUNT; temp++) {
		res = ulpi_read(dev, reg);
		if (res != 0xffffffff)
			return res;

		phy_clk_reset(dev);
	}

	USBH_ERR("%s: ulpi read failed for %d times\n",
			__func__, ULPI_VERIFY_MAX_LOOP_COUNT);

	return -1;
}

static int ulpi_write_with_reset(struct msm_otg *dev,
unsigned val, unsigned reg)
{
	int temp, res;

	for (temp = 0; temp < ULPI_VERIFY_MAX_LOOP_COUNT; temp++) {
		res = ulpi_write(dev, val, reg);
		if (!res)
			return 0;
		phy_clk_reset(dev);
	}
	USBH_ERR("%s: ulpi write failed for %d times\n",
		__func__, ULPI_VERIFY_MAX_LOOP_COUNT);

	return -1;
}

static inline int turn_off_phy_pll(struct msm_otg *dev)
{
	unsigned res;

	res = ulpi_read_with_reset(dev, ULPI_CONFIG_REG1);
	if (res == 0xffffffff)
		return -ETIMEDOUT;

	res = ulpi_write_with_reset(dev,
		res & ~(ULPI_ONCLOCK), ULPI_CONFIG_REG1);
	if (res)
		return -ETIMEDOUT;

	res = ulpi_write_with_reset(dev,
		ULPI_CLOCK_SUSPENDM, ULPI_IFC_CTRL_CLR);
	if (res)
		return -ETIMEDOUT;

	
	res = ulpi_write_with_reset(dev,
		ULPI_SUSPENDM, ULPI_FUNC_CTRL_CLR);
	if (res)
		return -ETIMEDOUT;

	return res;
}

static inline int check_phy_caliberation(struct msm_otg *dev)
{
	unsigned res;

	res = ulpi_read_with_reset(dev, ULPI_DEBUG);

	if (res == 0xffffffff)
		return -ETIMEDOUT;

	if (!(res & ULPI_CALIB_STS) && ULPI_CALIB_VAL(res))
		return 0;

	return -1;
}

static int msm_otg_phy_caliberate(struct msm_otg *dev)
{
	int i = 0;
	unsigned long res;

	do {
		res = turn_off_phy_pll(dev);
		if (res)
			return -ETIMEDOUT;

		
		phy_clk_reset(dev);

		res = check_phy_caliberation(dev);
		if (!res)
			return res;
		i++;

	} while (i < PHY_CALIB_RETRY_COUNT);

	return res;
}

static int msm_otg_phy_reset(struct msm_otg *dev)
{
	unsigned rc;
	unsigned temp;
	unsigned long timeout;

	rc = clk_reset(dev->alt_core_clk, CLK_RESET_ASSERT);
	if (rc) {
		USBH_ERR("%s: usb hs clk assert failed\n", __func__);
		return -1;
	}

	phy_clk_reset(dev);

	rc = clk_reset(dev->alt_core_clk, CLK_RESET_DEASSERT);
	if (rc) {
		USBH_ERR("%s: usb hs clk deassert failed\n", __func__);
		return -1;
	}
	writel_relaxed(USBCMD_RESET, USB_USBCMD);
	timeout = jiffies + USB_LINK_RESET_TIMEOUT;
	do {
		if (time_after(jiffies, timeout)) {
			pr_err("msm_otg: usb link reset timeout\n");
			break;
		}
		usleep_range(1000, 1200);
	} while (readl_relaxed(USB_USBCMD) & USBCMD_RESET);

	
	temp = (readl(USB_PORTSC) & ~PORTSC_PTS);
	writel(temp | PORTSC_PTS_ULPI, USB_PORTSC);

	if (atomic_read(&dev->chg_type) !=
				USB_CHG_TYPE__WALLCHARGER) {
		rc = msm_otg_phy_caliberate(dev);
		if (rc)
			return rc;
	}

	writel(USBCMD_RESET, USB_USBCMD);
	timeout = jiffies + USB_LINK_RESET_TIMEOUT;
	do {
		if (time_after(jiffies, timeout)) {
			USBH_ERR("msm_otg: usb link reset timeout\n");
			break;
		}
		msleep(1);
	} while (readl(USB_USBCMD) & USBCMD_RESET);

	if (readl(USB_USBCMD) & USBCMD_RESET) {
		USBH_ERR("%s: usb core reset failed\n", __func__);
		return -1;
	}

	return 0;
}

static void otg_reset(struct otg_transceiver *xceiv, int phy_reset)
{
	struct msm_otg *dev = container_of(xceiv, struct msm_otg, otg);
	unsigned long timeout;
	u32 mode, work = 0;

	USBH_INFO("%s\n", __func__);
	clk_prepare_enable(dev->alt_core_clk);

	if (!phy_reset)
		goto reset_link;

	if (dev->pdata->phy_reset)
		dev->pdata->phy_reset(dev->regs);
	else
		msm_otg_phy_reset(dev);

	
	if (readl(USB_OTGSC) & OTGSC_IDPU)
		writel(readl(USB_OTGSC) & ~OTGSC_IDPU, USB_OTGSC);
	
	ulpi_write(dev, 0xFF, 0x0F);
	ulpi_write(dev, 0xFF, 0x12);
	msleep(100);

reset_link:
	writel(USBCMD_RESET, USB_USBCMD);
	timeout = jiffies + USB_LINK_RESET_TIMEOUT;
	do {
		if (time_after(jiffies, timeout)) {
			USBH_ERR("msm_otg: usb link reset timeout\n");
			break;
		}
		msleep(1);
	} while (readl(USB_USBCMD) & USBCMD_RESET);

	
	writel(0x80000000, USB_PORTSC);

	set_pre_emphasis_level(dev);
	set_hsdrv_slope(dev);
	set_cdr_auto_reset(dev);
	set_driver_amplitude(dev);
	set_se1_gating(dev);

	writel(0x0, USB_AHB_BURST);
	writel(0x00, USB_AHB_MODE);
	if (dev->pdata->bam_disable) {
		writel_relaxed((readl_relaxed(USB_GEN_CONFIG) |
					USB_BAM_DISABLE), USB_GEN_CONFIG);
		pr_debug("%s(): USB_GEN_CONFIG = %x\n",
				__func__, readl_relaxed(USB_GEN_CONFIG));
	}
	
	mb();

	clk_disable_unprepare(dev->alt_core_clk);

	if ((xceiv->gadget && xceiv->gadget->is_a_peripheral) ||
			test_bit(ID, &dev->inputs))
		mode = USBMODE_SDIS | USBMODE_DEVICE;
	else
		mode = USBMODE_SDIS | USBMODE_HOST;
	USBH_INFO("%s: mode:0x%08x\n", __func__, mode);
	writel(mode, USB_USBMODE);

#if 0
	writel_relaxed((readl_relaxed(USB_OTGSC) | OTGSC_IDPU), USB_OTGSC);
#endif
	if (dev->otg.gadget && dev->vbus_notification_cb) {
		enable_sess_valid(dev);
		if (is_b_sess_vld() && !test_bit(B_SESS_VLD, &dev->inputs)) {
			pr_debug("%s: handle missing BSV event\n", __func__);
			set_bit(B_SESS_VLD, &dev->inputs);
			work = 1;
		} else if (!is_b_sess_vld() && test_bit(B_SESS_VLD,
				&dev->inputs)) {
			pr_debug("%s: handle missing !BSV event\n", __func__);
			clear_bit(B_SESS_VLD, &dev->inputs);
			work = 1;
		}
	}

#ifdef CONFIG_USB_EHCI_MSM_72K
	if (dev->otg.host && !dev->pmic_id_notif_supp) {
		enable_idgnd(dev);
		
		if (is_host() && test_bit(ID, &dev->inputs)) {
			pr_debug("%s: handle missing ID_GND event\n", __func__);
			clear_bit(ID, &dev->inputs);
			work = 1;
		} else if (!is_host() && !test_bit(ID, &dev->inputs)) {
			pr_debug("%s: handle missing !ID_GND event\n",
						__func__);
			set_bit(ID, &dev->inputs);
			work = 1;
		}
	} else {
		disable_idgnd(dev);
	}
#endif

	enable_idabc(dev);

	if (readl(USB_OTGSC) & OTGSC_IDPU)
		writel(readl(USB_OTGSC) & ~OTGSC_IDPU, USB_OTGSC);

	if (work) {
		wake_lock(&dev->wlock);
		queue_work(dev->wq, &dev->sm_work);
	}
}

static void msm_otg_sm_work(struct work_struct *w)
{
	struct msm_otg	*dev = container_of(w, struct msm_otg, sm_work);
	enum chg_type	chg_type = atomic_read(&dev->chg_type);
	int ret;
	int work = 0;
	enum usb_otg_state state;
	unsigned long flags;

	if (atomic_read(&dev->in_lpm))
		msm_otg_set_suspend(&dev->otg, 0);

	spin_lock_irqsave(&dev->lock, flags);
	state = dev->otg.state;
	spin_unlock_irqrestore(&dev->lock, flags);

	USBH_INFO("%s: state:%s bit:0x%08x\n", __func__, state_string(state),
			(unsigned) dev->inputs);
	mutex_lock(&smwork_sem);
	switch (state) {
	case OTG_STATE_UNDEFINED:

		if (atomic_read(&dev->chg_type) ==
				USB_CHG_TYPE__WALLCHARGER) {
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_B_PERIPHERAL;
			spin_unlock_irqrestore(&dev->lock, flags);
			work = 1;
			break;
		}

		
		otg_reset(&dev->otg, 1);

#ifdef CONFIG_USB_MSM_ACA
		set_aca_id_inputs(dev);
#endif
		if (dev->pdata->otg_mode == OTG_USER_CONTROL) {
			if ((dev->pdata->usb_mode == USB_PERIPHERAL_MODE) ||
					!dev->otg.host) {
				set_bit(ID, &dev->inputs);
				set_bit(B_SESS_VLD, &dev->inputs);
			}
		} else {
			if (!dev->otg.host || !is_host())
				set_bit(ID, &dev->inputs);

			if (dev->otg.gadget && is_b_sess_vld()) {
				set_bit(B_SESS_VLD, &dev->inputs);
				if (dev->pdata->usb_uart_switch)
					dev->pdata->usb_uart_switch(0);
			}
		}
		spin_lock_irqsave(&dev->lock, flags);
		if ((test_bit(ID, &dev->inputs)) &&
				!test_bit(ID_A, &dev->inputs)) {
			dev->otg.state = OTG_STATE_B_IDLE;
		} else {
			set_bit(A_BUS_REQ, &dev->inputs);
			dev->otg.state = OTG_STATE_A_IDLE;
		}
		spin_unlock_irqrestore(&dev->lock, flags);

		work = 1;
		break;
	case OTG_STATE_B_IDLE:
		dev->otg.default_a = 0;
		if (dev->connect_type == CONNECT_TYPE_USB && USB_disabled) {
			dev->connect_type = CONNECT_TYPE_UNKNOWN;
			msm_otg_set_power(&dev->otg, 0);
			pr_debug("Fake Sleep:entering into lpm\n");
			msm_otg_put_suspend(dev);

			if (dev->pdata->ldo_set_voltage)
				dev->pdata->ldo_set_voltage(3075);
		} else if (!test_bit(ID, &dev->inputs) ||
				test_bit(ID_A, &dev->inputs)) {
			pr_debug("!id || id_A\n");
			clear_bit(B_BUS_REQ, &dev->inputs);
			otg_reset(&dev->otg, 0);
			set_bit(A_BUS_REQ, &dev->inputs);

			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_IDLE;
			spin_unlock_irqrestore(&dev->lock, flags);
			msm_otg_set_power(&dev->otg, 0);
			work = 1;
		} else if (test_bit(B_SESS_VLD, &dev->inputs) &&
				!test_bit(ID_B, &dev->inputs)) {
			pr_debug("b_sess_vld\n");
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_B_PERIPHERAL;
			spin_unlock_irqrestore(&dev->lock, flags);
			msm_otg_set_power(&dev->otg, 0);
			msm_otg_start_peripheral(&dev->otg, 1);
		} else if (test_bit(B_BUS_REQ, &dev->inputs)) {
			pr_debug("b_sess_end && b_bus_req\n");
			ret = msm_otg_start_srp(&dev->otg);
			if (ret < 0) {
				
				clear_bit(B_BUS_REQ, &dev->inputs);
				work = 1;
				break;
			}
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_B_SRP_INIT;
			spin_unlock_irqrestore(&dev->lock, flags);
			msm_otg_start_timer(dev, TB_SRP_FAIL, B_SRP_FAIL);
			break;
		} else if (test_bit(ID_B, &dev->inputs)) {
			atomic_set(&dev->chg_type, USB_CHG_TYPE__SDP);
			msm_otg_set_power(&dev->otg, USB_IDCHG_MAX);
		} else {
			msm_otg_set_power(&dev->otg, 0);
			pr_debug("entering into lpm\n");
			msm_otg_put_suspend(dev);

			if (dev->pdata->ldo_set_voltage)
				dev->pdata->ldo_set_voltage(3075);
		}
		break;
	case OTG_STATE_B_SRP_INIT:
		if (!test_bit(ID, &dev->inputs) ||
				test_bit(ID_A, &dev->inputs) ||
				test_bit(ID_C, &dev->inputs) ||
				(test_bit(B_SESS_VLD, &dev->inputs) &&
				!test_bit(ID_B, &dev->inputs))) {
			pr_debug("!id || id_a/c || b_sess_vld+!id_b\n");
			msm_otg_del_timer(dev);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_B_IDLE;
			spin_unlock_irqrestore(&dev->lock, flags);
			work = 1;
		} else if (test_bit(B_SRP_FAIL, &dev->tmouts)) {
			pr_debug("b_srp_fail\n");
			
			msm_otg_send_event(&dev->otg,
				OTG_EVENT_NO_RESP_FOR_SRP);
			clear_bit(B_BUS_REQ, &dev->inputs);
			clear_bit(B_SRP_FAIL, &dev->tmouts);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_B_IDLE;
			spin_unlock_irqrestore(&dev->lock, flags);
			dev->b_last_se0_sess = jiffies;
			work = 1;
		}
		break;
	case OTG_STATE_B_PERIPHERAL:
		if (!test_bit(ID, &dev->inputs) ||
				test_bit(ID_A, &dev->inputs) ||
				test_bit(ID_B, &dev->inputs) ||
				!test_bit(B_SESS_VLD, &dev->inputs)) {
			pr_debug("!id  || id_a/b || !b_sess_vld\n");
			clear_bit(B_BUS_REQ, &dev->inputs);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_B_IDLE;
			spin_unlock_irqrestore(&dev->lock, flags);
			msm_otg_start_peripheral(&dev->otg, 0);
			dev->b_last_se0_sess = jiffies;

			if (dev->connect_type != CONNECT_TYPE_NONE) {
				dev->connect_type = CONNECT_TYPE_NONE;
				queue_work(dev->wq, &dev->notifier_work);
			}

			if (check_htc_mode_status() != NOT_ON_AUTOBOT) {
				htc_mode_enable(0);
				android_switch_default();
			}
			dev->ac_detect_count = 0;
			del_timer(&dev->ac_detect_timer);
			
			otg_reset(&dev->otg, 1);
			work = 1;
		} else if (test_bit(B_BUS_REQ, &dev->inputs) &&
				dev->otg.gadget->b_hnp_enable &&
				test_bit(A_BUS_SUSPEND, &dev->inputs)) {
			pr_debug("b_bus_req && b_hnp_en && a_bus_suspend\n");
			msm_otg_start_timer(dev, TB_ASE0_BRST, B_ASE0_BRST);
			msm_otg_start_peripheral(&dev->otg, 0);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_B_WAIT_ACON;
			spin_unlock_irqrestore(&dev->lock, flags);
			dev->otg.host->is_b_host = 1;
			msm_otg_start_host(&dev->otg, REQUEST_START);

		} else if (test_bit(ID_C, &dev->inputs)) {
			atomic_set(&dev->chg_type, USB_CHG_TYPE__SDP);
			msm_otg_set_power(&dev->otg, USB_IDCHG_MAX);
		} else if (chg_type == USB_CHG_TYPE__WALLCHARGER) {
#ifdef CONFIG_USB_MSM_ACA
			del_timer_sync(&dev->id_timer);
#endif
			
			otg_reset(&dev->otg, 1);
			pr_debug("entering into lpm with wall-charger\n");
			msm_otg_put_suspend(dev);
			
			otg_pm_qos_update_latency(dev, 0);
		} else if (dev->connect_type == CONNECT_TYPE_USB && USB_disabled) {
			pr_debug("Fake sleep: Disable USB function\n");
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_B_IDLE;
			spin_unlock_irqrestore(&dev->lock, flags);
			msm_otg_start_peripheral(&dev->otg, 0);

			dev->ac_detect_count = 0;
			del_timer(&dev->ac_detect_timer);
			
			otg_reset(&dev->otg, 1);
			work = 1;
		}
		pr_debug("do nothing !!\n");
		break;
	case OTG_STATE_B_WAIT_ACON:
		if (!test_bit(ID, &dev->inputs) ||
				test_bit(ID_A, &dev->inputs) ||
				test_bit(ID_B, &dev->inputs) ||
				!test_bit(B_SESS_VLD, &dev->inputs)) {
			pr_debug("!id || id_a/b || !b_sess_vld\n");
			msm_otg_del_timer(dev);
			msm_otg_start_host(&dev->otg, REQUEST_STOP);
			dev->otg.host->is_b_host = 0;

			clear_bit(B_BUS_REQ, &dev->inputs);
			clear_bit(A_BUS_SUSPEND, &dev->inputs);
			dev->b_last_se0_sess = jiffies;
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_B_IDLE;
			spin_unlock_irqrestore(&dev->lock, flags);

			
			otg_reset(&dev->otg, 1);
			work = 1;
		} else if (test_bit(A_CONN, &dev->inputs)) {
			pr_debug("a_conn\n");
			clear_bit(A_BUS_SUSPEND, &dev->inputs);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_B_HOST;
			spin_unlock_irqrestore(&dev->lock, flags);
			if (test_bit(ID_C, &dev->inputs)) {
				atomic_set(&dev->chg_type, USB_CHG_TYPE__SDP);
				msm_otg_set_power(&dev->otg, USB_IDCHG_MAX);
			}
		} else if (test_bit(B_ASE0_BRST, &dev->tmouts)) {
			pr_debug("b_ase0_brst_tmout\n");
			msm_otg_send_event(&dev->otg,
				OTG_EVENT_HNP_FAILED);
			msm_otg_start_host(&dev->otg, REQUEST_STOP);
			dev->otg.host->is_b_host = 0;
			clear_bit(B_ASE0_BRST, &dev->tmouts);
			clear_bit(A_BUS_SUSPEND, &dev->inputs);
			clear_bit(B_BUS_REQ, &dev->inputs);

			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_B_PERIPHERAL;
			spin_unlock_irqrestore(&dev->lock, flags);
			msm_otg_start_peripheral(&dev->otg, 1);
		} else if (test_bit(ID_C, &dev->inputs)) {
			atomic_set(&dev->chg_type, USB_CHG_TYPE__SDP);
			msm_otg_set_power(&dev->otg, USB_IDCHG_MAX);
		}
		break;
	case OTG_STATE_B_HOST:
		if (!test_bit(B_BUS_REQ, &dev->inputs) ||
				!test_bit(A_CONN, &dev->inputs)) {
			pr_debug("!b_bus_req || !a_conn\n");
			clear_bit(A_CONN, &dev->inputs);
			clear_bit(B_BUS_REQ, &dev->inputs);

			msm_otg_start_host(&dev->otg, REQUEST_STOP);
			dev->otg.host->is_b_host = 0;

			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_B_IDLE;
			spin_unlock_irqrestore(&dev->lock, flags);
			
			otg_reset(&dev->otg, 1);
			work = 1;
		} else if (test_bit(ID_C, &dev->inputs)) {
			atomic_set(&dev->chg_type, USB_CHG_TYPE__SDP);
			msm_otg_set_power(&dev->otg, USB_IDCHG_MAX);
		}
		break;
	case OTG_STATE_A_IDLE:
		dev->otg.default_a = 1;
		if (test_bit(ID, &dev->inputs) &&
				!test_bit(ID_A, &dev->inputs)) {
			pr_debug("id && !id_a\n");
			dev->otg.default_a = 0;
			otg_reset(&dev->otg, 0);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_B_IDLE;
			spin_unlock_irqrestore(&dev->lock, flags);
			msm_otg_set_power(&dev->otg, 0);
			work = 1;
		} else if (!test_bit(A_BUS_DROP, &dev->inputs) &&
				(test_bit(A_SRP_DET, &dev->inputs) ||
				 test_bit(A_BUS_REQ, &dev->inputs))) {
			pr_debug("!a_bus_drop && (a_srp_det || a_bus_req)\n");

			clear_bit(A_SRP_DET, &dev->inputs);
			
			writel((readl(USB_OTGSC) & ~OTGSC_INTR_STS_MASK) &
					~OTGSC_DPIE, USB_OTGSC);

			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_WAIT_VRISE;
			spin_unlock_irqrestore(&dev->lock, flags);
			
			if (test_bit(ID_A, &dev->inputs))
				msm_otg_set_power(&dev->otg, 0);
			else
				msm_hsusb_vbus_power(dev, 1);
			msm_otg_start_timer(dev, TA_WAIT_VRISE, A_WAIT_VRISE);
			
		} else {
			pr_debug("No session requested\n");

			writel((readl(USB_OTGSC) & ~OTGSC_INTR_STS_MASK) |
					OTGSC_DPIE, USB_OTGSC);
			msm_otg_put_suspend(dev);

		}
		break;
	case OTG_STATE_A_WAIT_VRISE:
		if ((test_bit(ID, &dev->inputs) &&
				!test_bit(ID_A, &dev->inputs)) ||
				test_bit(A_BUS_DROP, &dev->inputs) ||
				test_bit(A_WAIT_VRISE, &dev->tmouts)) {
			pr_debug("id || a_bus_drop || a_wait_vrise_tmout\n");
			clear_bit(A_BUS_REQ, &dev->inputs);
			msm_otg_del_timer(dev);
			msm_hsusb_vbus_power(dev, 0);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_WAIT_VFALL;
			spin_unlock_irqrestore(&dev->lock, flags);
			msm_otg_start_timer(dev, TA_WAIT_VFALL, A_WAIT_VFALL);
		} else if (test_bit(A_VBUS_VLD, &dev->inputs)) {
			pr_debug("a_vbus_vld\n");
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_WAIT_BCON;
			spin_unlock_irqrestore(&dev->lock, flags);
			if (TA_WAIT_BCON > 0)
				msm_otg_start_timer(dev, TA_WAIT_BCON,
					A_WAIT_BCON);
			
			msm_otg_start_host(&dev->otg, REQUEST_START);
		}
		break;
	case OTG_STATE_A_WAIT_BCON:
		if ((test_bit(ID, &dev->inputs) &&
				!test_bit(ID_A, &dev->inputs)) ||
				test_bit(A_BUS_DROP, &dev->inputs) ||
				test_bit(A_WAIT_BCON, &dev->tmouts)) {
			pr_debug("id_f/b/c || a_bus_drop ||"
					"a_wait_bcon_tmout\n");
			if (test_bit(A_WAIT_BCON, &dev->tmouts))
				msm_otg_send_event(&dev->otg,
					OTG_EVENT_DEV_CONN_TMOUT);
			msm_otg_del_timer(dev);
			clear_bit(A_BUS_REQ, &dev->inputs);
			msm_otg_start_host(&dev->otg, REQUEST_STOP);
			
			otg_reset(&dev->otg, 1);
			if (test_bit(ID_A, &dev->inputs))
				msm_otg_set_power(&dev->otg, USB_IDCHG_MAX);
			else
				msm_hsusb_vbus_power(dev, 0);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_WAIT_VFALL;
			spin_unlock_irqrestore(&dev->lock, flags);
			msm_otg_start_timer(dev, TA_WAIT_VFALL, A_WAIT_VFALL);
		} else if (test_bit(B_CONN, &dev->inputs)) {
			pr_debug("b_conn\n");
			msm_otg_del_timer(dev);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_HOST;
			spin_unlock_irqrestore(&dev->lock, flags);
			if (test_bit(ID_A, &dev->inputs)) {
				atomic_set(&dev->chg_type, USB_CHG_TYPE__SDP);
				msm_otg_set_power(&dev->otg,
					USB_IDCHG_MIN - get_aca_bmaxpower(dev));
			}
		} else if (!test_bit(A_VBUS_VLD, &dev->inputs)) {
			pr_debug("!a_vbus_vld\n");
			msm_otg_del_timer(dev);
			msm_otg_start_host(&dev->otg, REQUEST_STOP);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_VBUS_ERR;
			spin_unlock_irqrestore(&dev->lock, flags);
			
			otg_reset(&dev->otg, 1);
		} else if (test_bit(ID_A, &dev->inputs)) {
			msm_hsusb_vbus_power(dev, 0);
		} else if (!test_bit(ID, &dev->inputs)) {
			msm_otg_set_power(&dev->otg, 0);
			msm_hsusb_vbus_power(dev, 1);
		}
		break;
	case OTG_STATE_A_HOST:
		if ((test_bit(ID, &dev->inputs) &&
				!test_bit(ID_A, &dev->inputs)) ||
				test_bit(A_BUS_DROP, &dev->inputs)) {
			pr_debug("id_f/b/c || a_bus_drop\n");
			clear_bit(B_CONN, &dev->inputs);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_WAIT_VFALL;
			spin_unlock_irqrestore(&dev->lock, flags);
			msm_otg_start_host(&dev->otg, REQUEST_STOP);
			
			otg_reset(&dev->otg, 1);
			if (!test_bit(ID_A, &dev->inputs))
				msm_hsusb_vbus_power(dev, 0);
			msm_otg_start_timer(dev, TA_WAIT_VFALL, A_WAIT_VFALL);
			msm_otg_set_power(&dev->otg, 0);
		} else if (!test_bit(A_VBUS_VLD, &dev->inputs)) {
			pr_debug("!a_vbus_vld\n");
			clear_bit(B_CONN, &dev->inputs);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_VBUS_ERR;
			spin_unlock_irqrestore(&dev->lock, flags);
			msm_otg_start_host(&dev->otg, REQUEST_STOP);
			
			otg_reset(&dev->otg, 1);
			
		} else if (!test_bit(A_BUS_REQ, &dev->inputs)) {
			pr_debug("!a_bus_req\n");
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_SUSPEND;
			spin_unlock_irqrestore(&dev->lock, flags);
			if (dev->otg.host->b_hnp_enable) {
				msm_otg_start_timer(dev, TA_AIDL_BDIS,
						A_AIDL_BDIS);
			} else {
				
				msm_otg_put_suspend(dev);
			}
			if (test_bit(ID_A, &dev->inputs))
				msm_otg_set_power(&dev->otg,
						USB_IDCHG_MIN - USB_IB_UNCFG);
		} else if (!test_bit(B_CONN, &dev->inputs)) {
			pr_debug("!b_conn\n");
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_WAIT_BCON;
			spin_unlock_irqrestore(&dev->lock, flags);
			if (TA_WAIT_BCON > 0)
				msm_otg_start_timer(dev, TA_WAIT_BCON,
					A_WAIT_BCON);
		} else if (test_bit(ID_A, &dev->inputs)) {
			atomic_set(&dev->chg_type, USB_CHG_TYPE__SDP);
			msm_hsusb_vbus_power(dev, 0);
			msm_otg_set_power(&dev->otg,
					USB_IDCHG_MIN - get_aca_bmaxpower(dev));
		} else if (!test_bit(ID, &dev->inputs)) {
			atomic_set(&dev->chg_type, USB_CHG_TYPE__INVALID);
			msm_otg_set_power(&dev->otg, 0);
			msm_hsusb_vbus_power(dev, 1);
		}
		break;
	case OTG_STATE_A_SUSPEND:
		if ((test_bit(ID, &dev->inputs) &&
				!test_bit(ID_A, &dev->inputs)) ||
				test_bit(A_BUS_DROP, &dev->inputs) ||
				test_bit(A_AIDL_BDIS, &dev->tmouts)) {
			pr_debug("id_f/b/c || a_bus_drop ||"
					"a_aidl_bdis_tmout\n");
			if (test_bit(A_AIDL_BDIS, &dev->tmouts))
				msm_otg_send_event(&dev->otg,
					OTG_EVENT_HNP_FAILED);
			msm_otg_del_timer(dev);
			clear_bit(B_CONN, &dev->inputs);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_WAIT_VFALL;
			spin_unlock_irqrestore(&dev->lock, flags);
			msm_otg_start_host(&dev->otg, REQUEST_STOP);
			msm_hsusb_vbus_power(dev, 0);
			
			otg_reset(&dev->otg, 1);
			if (!test_bit(ID_A, &dev->inputs))
				msm_hsusb_vbus_power(dev, 0);
			msm_otg_start_timer(dev, TA_WAIT_VFALL, A_WAIT_VFALL);
			msm_otg_set_power(&dev->otg, 0);
		} else if (!test_bit(A_VBUS_VLD, &dev->inputs)) {
			pr_debug("!a_vbus_vld\n");
			msm_otg_del_timer(dev);
			clear_bit(B_CONN, &dev->inputs);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_VBUS_ERR;
			spin_unlock_irqrestore(&dev->lock, flags);
			msm_otg_start_host(&dev->otg, REQUEST_STOP);
			
			otg_reset(&dev->otg, 1);
		} else if (!test_bit(B_CONN, &dev->inputs) &&
				dev->otg.host->b_hnp_enable) {
			pr_debug("!b_conn && b_hnp_enable");
			
			msm_otg_del_timer(dev);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_PERIPHERAL;
			spin_unlock_irqrestore(&dev->lock, flags);

			msm_otg_start_host(&dev->otg, REQUEST_HNP_SUSPEND);

			dev->otg.gadget->is_a_peripheral = 1;
			msm_otg_start_peripheral(&dev->otg, 1);
			
			if (test_bit(ID_A, &dev->inputs)) {
				atomic_set(&dev->chg_type, USB_CHG_TYPE__SDP);
				msm_otg_set_power(&dev->otg,
					 USB_IDCHG_MIN - USB_IB_UNCFG);
			}
		} else if (!test_bit(B_CONN, &dev->inputs) &&
				!dev->otg.host->b_hnp_enable) {
			pr_debug("!b_conn && !b_hnp_enable");
			set_bit(A_BUS_REQ, &dev->inputs);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_WAIT_BCON;
			spin_unlock_irqrestore(&dev->lock, flags);
			if (TA_WAIT_BCON > 0)
				msm_otg_start_timer(dev, TA_WAIT_BCON,
					A_WAIT_BCON);
			msm_otg_set_power(&dev->otg, 0);
		} else if (test_bit(ID_A, &dev->inputs)) {
			msm_hsusb_vbus_power(dev, 0);
			atomic_set(&dev->chg_type, USB_CHG_TYPE__SDP);
			msm_otg_set_power(&dev->otg,
					 USB_IDCHG_MIN - USB_IB_UNCFG);
		} else if (!test_bit(ID, &dev->inputs)) {
			msm_otg_set_power(&dev->otg, 0);
			msm_hsusb_vbus_power(dev, 1);
		}
		break;
	case OTG_STATE_A_PERIPHERAL:
		if ((test_bit(ID, &dev->inputs) &&
				!test_bit(ID_A, &dev->inputs)) ||
				test_bit(A_BUS_DROP, &dev->inputs)) {
			pr_debug("id _f/b/c || a_bus_drop\n");
			
			msm_otg_del_timer(dev);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_WAIT_VFALL;
			spin_unlock_irqrestore(&dev->lock, flags);
			msm_otg_start_peripheral(&dev->otg, 0);
			dev->otg.gadget->is_a_peripheral = 0;
			
			msm_otg_start_host(&dev->otg, REQUEST_STOP);

			
			otg_reset(&dev->otg, 1);
			if (!test_bit(ID_A, &dev->inputs))
				msm_hsusb_vbus_power(dev, 0);
			msm_otg_start_timer(dev, TA_WAIT_VFALL, A_WAIT_VFALL);
			msm_otg_set_power(&dev->otg, 0);
		} else if (!test_bit(A_VBUS_VLD, &dev->inputs)) {
			pr_debug("!a_vbus_vld\n");
			
			msm_otg_del_timer(dev);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_VBUS_ERR;
			spin_unlock_irqrestore(&dev->lock, flags);
			msm_otg_start_peripheral(&dev->otg, 0);
			dev->otg.gadget->is_a_peripheral = 0;
			
			msm_otg_start_host(&dev->otg, REQUEST_STOP);
		} else if (test_bit(A_BIDL_ADIS, &dev->tmouts)) {
			pr_debug("a_bidl_adis_tmout\n");
			msm_otg_start_peripheral(&dev->otg, 0);
			dev->otg.gadget->is_a_peripheral = 0;

			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_WAIT_BCON;
			spin_unlock_irqrestore(&dev->lock, flags);
			set_bit(A_BUS_REQ, &dev->inputs);
			msm_otg_start_host(&dev->otg, REQUEST_HNP_RESUME);
			if (TA_WAIT_BCON > 0)
				msm_otg_start_timer(dev, TA_WAIT_BCON,
					A_WAIT_BCON);
			msm_otg_set_power(&dev->otg, 0);
		} else if (test_bit(ID_A, &dev->inputs)) {
			msm_hsusb_vbus_power(dev, 0);
			atomic_set(&dev->chg_type, USB_CHG_TYPE__SDP);
			msm_otg_set_power(&dev->otg,
					 USB_IDCHG_MIN - USB_IB_UNCFG);
		} else if (!test_bit(ID, &dev->inputs)) {
			msm_otg_set_power(&dev->otg, 0);
			msm_hsusb_vbus_power(dev, 1);
		}
		break;
	case OTG_STATE_A_WAIT_VFALL:
		if (test_bit(A_WAIT_VFALL, &dev->tmouts)) {
			clear_bit(A_VBUS_VLD, &dev->inputs);
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_IDLE;
			spin_unlock_irqrestore(&dev->lock, flags);
			work = 1;
		}
		break;
	case OTG_STATE_A_VBUS_ERR:
		if ((test_bit(ID, &dev->inputs) &&
				!test_bit(ID_A, &dev->inputs)) ||
				test_bit(A_BUS_DROP, &dev->inputs) ||
				test_bit(A_CLR_ERR, &dev->inputs)) {
			spin_lock_irqsave(&dev->lock, flags);
			dev->otg.state = OTG_STATE_A_WAIT_VFALL;
			spin_unlock_irqrestore(&dev->lock, flags);
			if (!test_bit(ID_A, &dev->inputs))
				msm_hsusb_vbus_power(dev, 0);
			msm_otg_start_timer(dev, TA_WAIT_VFALL, A_WAIT_VFALL);
			msm_otg_set_power(&dev->otg, 0);
		}
		break;
	default:
		pr_err("invalid OTG state\n");
	}
	mutex_unlock(&smwork_sem);

	if (work)
		queue_work(dev->wq, &dev->sm_work);

#ifdef CONFIG_USB_MSM_ACA
	
	if ((test_bit(ID, &dev->inputs) &&
			test_bit(B_SESS_VLD, &dev->inputs) &&
			chg_type != USB_CHG_TYPE__WALLCHARGER) ||
			test_bit(ID_A, &dev->inputs)) {
		mod_timer(&dev->id_timer, jiffies +
				 msecs_to_jiffies(OTG_ID_POLL_MS));
		return;
	}
	del_timer(&dev->id_timer);
#endif
	if (!work_pending(&dev->sm_work) && !hrtimer_active(&dev->timer) &&
			!work_pending(&dev->otg_resume_work))
		wake_unlock(&dev->wlock);
}

#ifdef CONFIG_USB_MSM_ACA
static void msm_otg_id_func(unsigned long _dev)
{
	struct msm_otg	*dev = (struct msm_otg *) _dev;
	u8		phy_ints;

#ifdef CONFIG_USB_MSM_STANDARD_ACA
	if (test_bit(ID_A, &dev->inputs))
		goto out;
#endif

	if (atomic_read(&dev->in_lpm))
		msm_otg_set_suspend(&dev->otg, 0);

	phy_ints = ulpi_read(dev, 0x13);

	if (phy_ints == -ETIMEDOUT)
		return;


	
	if (phy_id_state_gnd(phy_ints))
		goto out;

	if ((test_bit(ID_A, &dev->inputs) == phy_id_state_a(phy_ints)) &&
	    (test_bit(ID_B, &dev->inputs) == phy_id_state_b(phy_ints)) &&
	    (test_bit(ID_C, &dev->inputs) == phy_id_state_c(phy_ints))) {
		mod_timer(&dev->id_timer,
				jiffies + msecs_to_jiffies(OTG_ID_POLL_MS));
		goto out;
	} else {
		set_aca_id_inputs(dev);
	}
	wake_lock(&dev->wlock);
	queue_work(dev->wq, &dev->sm_work);
out:
	
	if (!is_b_sess_vld()) {
		clear_bit(B_SESS_VLD, &dev->inputs);
		wake_lock(&dev->wlock);
		queue_work(dev->wq, &dev->sm_work);
	}
	return;
}
#endif
#ifdef CONFIG_USB_OTG
static ssize_t
set_pwr_down(struct device *_dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct msm_otg *dev = the_msm_otg;
	int value;
	enum usb_otg_state state;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	state = dev->otg.state;
	spin_unlock_irqrestore(&dev->lock, flags);

	
	if (state <= OTG_STATE_A_IDLE)
		return -EINVAL;

	sscanf(buf, "%d", &value);

	if (test_bit(A_BUS_DROP, &dev->inputs) != !!value) {
		change_bit(A_BUS_DROP, &dev->inputs);
		wake_lock(&dev->wlock);
		queue_work(dev->wq, &dev->sm_work);
	}

	return count;
}
static DEVICE_ATTR(pwr_down, S_IRUGO | S_IWUSR, NULL, set_pwr_down);

static ssize_t
set_srp_req(struct device *_dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct msm_otg *dev = the_msm_otg;
	enum usb_otg_state state;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	state = dev->otg.state;
	spin_unlock_irqrestore(&dev->lock, flags);

	if (state != OTG_STATE_B_IDLE)
		return -EINVAL;

	set_bit(B_BUS_REQ, &dev->inputs);
	wake_lock(&dev->wlock);
	queue_work(dev->wq, &dev->sm_work);

	return count;
}
static DEVICE_ATTR(srp_req, S_IRUGO | S_IWUSR, NULL, set_srp_req);

static ssize_t
set_clr_err(struct device *_dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct msm_otg *dev = the_msm_otg;
	enum usb_otg_state state;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	state = dev->otg.state;
	spin_unlock_irqrestore(&dev->lock, flags);

	if (state == OTG_STATE_A_VBUS_ERR) {
		set_bit(A_CLR_ERR, &dev->inputs);
		wake_lock(&dev->wlock);
		queue_work(dev->wq, &dev->sm_work);
	}

	return count;
}
static DEVICE_ATTR(clr_err, S_IRUGO | S_IWUSR, NULL, set_clr_err);

static struct attribute *msm_otg_attrs[] = {
	&dev_attr_pwr_down.attr,
	&dev_attr_srp_req.attr,
	&dev_attr_clr_err.attr,
	NULL,
};

static struct attribute_group msm_otg_attr_grp = {
	.attrs = msm_otg_attrs,
};
#endif

#ifdef CONFIG_DEBUG_FS
static int otg_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}
static ssize_t otg_mode_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct msm_otg *dev = file->private_data;
	int ret = count;
	int work = 0;
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	dev->pdata->otg_mode = OTG_USER_CONTROL;
	if (!memcmp(buf, "none", 4)) {
		clear_bit(B_SESS_VLD, &dev->inputs);
		set_bit(ID, &dev->inputs);
		work = 1;
	} else if (!memcmp(buf, "peripheral", 10)) {
		set_bit(B_SESS_VLD, &dev->inputs);
		set_bit(ID, &dev->inputs);
		work = 1;
	} else if (!memcmp(buf, "host", 4)) {
		clear_bit(B_SESS_VLD, &dev->inputs);
		clear_bit(ID, &dev->inputs);
		set_bit(A_BUS_REQ, &dev->inputs);
		work = 1;
	} else {
		pr_info("%s: unknown mode specified\n", __func__);
		ret = -EINVAL;
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	if (work) {
		wake_lock(&dev->wlock);
		queue_work(dev->wq, &dev->sm_work);
	}

	return ret;
}
const struct file_operations otgfs_fops = {
	.open	= otg_open,
	.write	= otg_mode_write,
};

#define OTG_INFO_SIZE 512
static ssize_t otg_info_read(struct file *file, char __user *ubuf,
				size_t count, loff_t *ppos)
{
	char *buf;
	int temp = 0;
	int ret;
	struct msm_otg *dev = file->private_data;

	buf = kzalloc(sizeof(char) * OTG_INFO_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	temp += scnprintf(buf + temp, OTG_INFO_SIZE - temp,
			"OTG State:             %s\n"
			"OTG Mode:              %d\n"
			"OTG Inputs:            0x%lx\n"
			"Charger Type:          %d\n"
			"PMIC VBUS Support:     %u\n"
			"PMIC ID Support:       %u\n"
			"USB In SPS:            %d\n"
			"pre_emphasis_level:    0x%x\n"
			"cdr_auto_reset:        0x%x\n"
			"hs_drv_amplitude:      0x%x\n"
			"se1_gate_state:        0x%x\n"
			"swfi_latency:          0x%x\n"
			"PHY Powercollapse:     0x%x\n",
			state_string(dev->otg.state),
			dev->pdata->otg_mode,
			dev->inputs,
			atomic_read(&dev->chg_type),
			dev->pmic_vbus_notif_supp,
			dev->pmic_id_notif_supp,
			dev->pdata->usb_in_sps,
			dev->pdata->pemp_level,
			dev->pdata->cdr_autoreset,
			dev->pdata->drv_ampl,
			dev->pdata->se1_gating,
			dev->pdata->swfi_latency,
			dev->pdata->phy_can_powercollapse);

	ret = simple_read_from_buffer(ubuf, count, ppos, buf, temp);

	kfree(buf);

	return ret;
}

const struct file_operations otgfs_info_fops = {
	.open	= otg_open,
	.read	= otg_info_read,
};

struct dentry *otg_debug_root;
struct dentry *otg_debug_mode;
struct dentry *otg_debug_info;
#endif

static int otg_debugfs_init(struct msm_otg *dev)
{
#ifdef CONFIG_DEBUG_FS
	otg_debug_root = debugfs_create_dir("otg", NULL);
	if (!otg_debug_root)
		return -ENOENT;

	otg_debug_mode = debugfs_create_file("mode", 0222,
						otg_debug_root, dev,
						&otgfs_fops);
	if (!otg_debug_mode)
		goto free_root;

	otg_debug_info = debugfs_create_file("info", 0444,
						otg_debug_root, dev,
						&otgfs_info_fops);
	if (!otg_debug_info)
		goto free_mode;

	return 0;

free_mode:
	debugfs_remove(otg_debug_mode);
	otg_debug_mode = NULL;

free_root:
	debugfs_remove(otg_debug_root);
	otg_debug_root = NULL;
	return -ENOENT;
#endif
	return 0;
}

static void otg_debugfs_cleanup(void)
{
#ifdef CONFIG_DEBUG_FS
	debugfs_remove(otg_debug_info);
	debugfs_remove(otg_debug_mode);
	debugfs_remove(otg_debug_root);
#endif
}

struct otg_io_access_ops msm_otg_io_ops = {
	.read = usb_ulpi_read,
	.write = usb_ulpi_write,
};

#if (defined(CONFIG_USB_OTG) && defined(CONFIG_USB_OTG_HOST))
static struct t_usb_host_status_notifier usb_host_status_notifier = {
	.name = "usb_host",
	.func = usb_host_cable_detect,
};
#endif

static int __init msm_otg_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct resource *res;
	struct msm_otg *dev;

	dev = kzalloc(sizeof(struct msm_otg), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	the_msm_otg = dev;
	dev->otg.dev = &pdev->dev;
	dev->pdata = pdev->dev.platform_data;

	if (!dev->pdata) {
		ret = -ENODEV;
		goto free_dev;
	}

#ifdef CONFIG_USB_EHCI_MSM_72K
	if (!dev->pdata->vbus_power) {
		ret = -ENODEV;
		goto free_dev;
	} else {
		msm_hsusb_vbus_power(dev, 0);
	}

#endif


	if (dev->pdata->rpc_connect) {
		ret = dev->pdata->rpc_connect(1);
		pr_debug("%s: rpc_connect(%d)\n", __func__, ret);
		if (ret) {
			pr_err("%s: rpc connect failed\n", __func__);
			ret = -ENODEV;
			goto free_dev;
		}
	}

	dev->alt_core_clk = clk_get(&pdev->dev, "alt_core_clk");
	if (IS_ERR(dev->alt_core_clk)) {
		pr_err("%s: failed to get alt_core_clk\n", __func__);
		ret = PTR_ERR(dev->alt_core_clk);
		goto rpc_fail;
	}
	clk_set_rate(dev->alt_core_clk, 60000000);

	
	pm_qos_add_request(&dev->pdata->pm_qos_req_dma, PM_QOS_CPU_DMA_LATENCY,
			   PM_QOS_DEFAULT_VALUE);

	dev->core_clk = clk_get(&pdev->dev, "core_clk");
	if (IS_ERR(dev->core_clk)) {
		pr_err("%s: failed to get core_clk\n", __func__);
		ret = PTR_ERR(dev->core_clk);
		goto put_alt_core_clk;
	}
	clk_set_rate(dev->core_clk, INT_MAX);

	clk_prepare_enable(dev->core_clk);

	if (!dev->pdata->pclk_is_hw_gated) {
		dev->iface_clk = clk_get(&pdev->dev, "iface_clk");
		if (IS_ERR(dev->iface_clk)) {
			pr_err("%s: failed to get abh_clk\n", __func__);
			ret = PTR_ERR(dev->iface_clk);
			goto put_core_clk;
		}
		clk_prepare_enable(dev->iface_clk);
	}

	if (!dev->pdata->phy_reset) {
		dev->phy_reset_clk = clk_get(&pdev->dev, "phy_clk");
		if (IS_ERR(dev->phy_reset_clk)) {
			pr_err("%s: failed to get phy_clk\n", __func__);
			ret = PTR_ERR(dev->phy_reset_clk);
			goto put_iface_clk;
		}
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		pr_err("%s: failed to get platform resource mem\n", __func__);
		ret = -ENODEV;
		goto put_phy_clk;
	}

	dev->regs = ioremap(res->start, resource_size(res));
	if (!dev->regs) {
		pr_err("%s: ioremap failed\n", __func__);
		ret = -ENOMEM;
		goto put_phy_clk;
	}
	dev->irq = platform_get_irq(pdev, 0);
	if (!dev->irq) {
		pr_err("%s: platform_get_irq failed\n", __func__);
		ret = -ENODEV;
		goto free_regs;
	}
	dev->xo_handle = msm_xo_get(MSM_XO_TCXO_D1, "usb");
	if (IS_ERR(dev->xo_handle)) {
		pr_err(" %s not able to get the handle"
			"to vote for TCXO D1 buffer\n", __func__);
		ret = PTR_ERR(dev->xo_handle);
		goto free_regs;
	}

	ret = msm_xo_mode_vote(dev->xo_handle, MSM_XO_MODE_ON);
	if (ret) {
		pr_err("%s failed to vote for TCXO"
			"D1 buffer%d\n", __func__, ret);
		goto free_xo_handle;
	}


	msm_otg_init_timer(dev);
	INIT_WORK(&dev->sm_work, msm_otg_sm_work);
	INIT_WORK(&dev->otg_resume_work, msm_otg_resume_w);
	INIT_WORK(&dev->notifier_work, send_usb_connect_notify);
	spin_lock_init(&dev->lock);
	wake_lock_init(&dev->wlock, WAKE_LOCK_SUSPEND, "msm_otg");

	dev->ac_detect_count = 0;
	dev->ac_detect_timer.function = ac_detect_expired;
	init_timer(&dev->ac_detect_timer);

	if (board_mfg_mode() == 5) 
		USB_disabled = 1;

	dev->wq = alloc_workqueue("k_otg", WQ_NON_REENTRANT, 0);
	if (!dev->wq) {
		ret = -ENOMEM;
		goto free_wlock;
	}

	if (dev->pdata->init_gpio) {
		ret = dev->pdata->init_gpio(1);
		if (ret) {
			pr_err("%s: gpio init failed with err:%d\n",
					__func__, ret);
			goto free_wq;
		}
	}
	if (dev->pdata->pmic_vbus_notif_init) {
		ret = dev->pdata->pmic_vbus_notif_init
			(&msm_otg_set_vbus_state, 1);
		if (!ret) {
			dev->pmic_vbus_notif_supp = 1;
		} else if (ret != -ENOTSUPP) {
			pr_err("%s: pmic_vbus_notif_init() failed, err:%d\n",
					__func__, ret);
			goto free_gpio;
		}
	}

	if (dev->pdata->phy_id_setup_init) {
		ret = dev->pdata->phy_id_setup_init(1);
		if (ret) {
			pr_err("%s: phy_id_setup_init failed err:%d",
					__func__, ret);
			goto free_pmic_vbus_notif;
		}
	}

	if (dev->pdata->pmic_id_notif_init) {
		ret = dev->pdata->pmic_id_notif_init(&msm_otg_set_id_state, 1);
		if (!ret) {
			dev->pmic_id_notif_supp = 1;
			dev->pmic_id_status = 1;
		} else if (ret != -ENOTSUPP) {
			USBH_ERR("%s: pmic_id_ notif_init failed err:%d",
					__func__, ret);
			goto free_pmic_vbus_notif;
		}
	}
#if defined(CONFIG_USB_OTG_HOST)
	else {
		dev->pmic_id_notif_supp = 1;
		dev->pmic_id_status = 1;
		force_host_mode = 0;
	}
#endif

	if (dev->pdata->pmic_vbus_irq)
		dev->vbus_on_irq = dev->pdata->pmic_vbus_irq;

	
	if (dev->pdata->init_vddcx) {
		ret = dev->pdata->init_vddcx(1);
		if (ret) {
			pr_err("%s: unable to enable vddcx digital core:%d\n",
				__func__, ret);
			goto free_phy_id_setup;
		}
	}

	if (dev->pdata->ldo_init) {
		ret = dev->pdata->ldo_init(1);
		if (ret) {
			pr_err("%s: ldo_init failed with err:%d\n",
					__func__, ret);
			goto free_config_vddcx;
		}
	}

	if (dev->pdata->ldo_enable) {
		ret = dev->pdata->ldo_enable(1);
		if (ret) {
			pr_err("%s: ldo_enable failed with err:%d\n",
					__func__, ret);
			goto free_ldo_init;
		}
	}


	
	writel((readl(USB_OTGSC) & ~OTGSC_INTR_MASK), USB_OTGSC);
	writel(readl(USB_USBSTS), USB_USBSTS);
	writel(0, USB_USBINTR);
	
	mb();

	ret = request_irq(dev->irq, msm_otg_irq, IRQF_SHARED,
					"msm_otg", dev);
	if (ret) {
		pr_err("%s: request irq failed\n", __func__);
		goto free_ldo_enable;
	}

	dev->otg.set_peripheral = msm_otg_set_peripheral;
#ifdef CONFIG_USB_EHCI_MSM_72K
	dev->otg.set_host = msm_otg_set_host;
#endif
	dev->otg.set_suspend = msm_otg_set_suspend;
	dev->otg.start_hnp = msm_otg_start_hnp;
	dev->otg.send_event = msm_otg_send_event;
	dev->otg.set_power = msm_otg_set_power;
	dev->otg.notify_charger = msm_otg_notify_charger_attached;
	dev->set_clk = msm_otg_set_clk;
	dev->reset = otg_reset;
	dev->otg.io_ops = &msm_otg_io_ops;
	if (otg_set_transceiver(&dev->otg)) {
		WARN_ON(1);
		goto free_otg_irq;
	}
#ifdef CONFIG_USB_MSM_ACA
	init_timer(&dev->id_timer);
	dev->id_timer.function = msm_otg_id_func;
	dev->id_timer.data = (unsigned long) dev;
#endif

	atomic_set(&dev->chg_type, USB_CHG_TYPE__INVALID);
#if 0
FIXME 3.4 porting
	if (dev->pdata->chg_init && dev->pdata->chg_init(1))
		pr_err("%s: chg_init failed\n", __func__);
#endif

	device_init_wakeup(&pdev->dev, 1);

	ret = pm_runtime_set_active(&pdev->dev);
	if (ret < 0)
		pr_err("%s: pm_runtime: Fail to set active\n", __func__);
	ret = 0;
#if (defined(CONFIG_USB_OTG) && defined(CONFIG_USB_OTG_HOST))
	usb_host_detect_register_notifier(&usb_host_status_notifier);
#endif

	pm_runtime_enable(&pdev->dev);
	pm_runtime_get(&pdev->dev);


	ret = otg_debugfs_init(dev);
	if (ret) {
		pr_err("%s: otg_debugfs_init failed\n", __func__);
		goto chg_deinit;
	}

#ifdef CONFIG_USB_OTG
	ret = sysfs_create_group(&pdev->dev.kobj, &msm_otg_attr_grp);
	if (ret < 0) {
		pr_err("%s: Failed to create the sysfs entry\n", __func__);
		otg_debugfs_cleanup();
		goto chg_deinit;
	}
#endif

	return 0;

chg_deinit:
	if (dev->pdata->chg_init)
		dev->pdata->chg_init(0);
free_otg_irq:
	free_irq(dev->irq, dev);
free_ldo_enable:
	if (dev->pdata->ldo_enable)
		dev->pdata->ldo_enable(0);
	if (dev->pdata->setup_gpio)
		dev->pdata->setup_gpio(USB_SWITCH_DISABLE);
free_ldo_init:
	if (dev->pdata->ldo_init)
		dev->pdata->ldo_init(0);
free_config_vddcx:
	if (dev->pdata->init_vddcx)
		dev->pdata->init_vddcx(0);
free_phy_id_setup:
	if (dev->pdata->phy_id_setup_init)
		dev->pdata->phy_id_setup_init(0);
free_pmic_vbus_notif:
	if (dev->pdata->pmic_vbus_notif_init && dev->pmic_vbus_notif_supp)
		dev->pdata->pmic_vbus_notif_init(&msm_otg_set_vbus_state, 0);
free_gpio:
	if (dev->pdata->init_gpio)
		dev->pdata->init_gpio(0);
free_wq:
	destroy_workqueue(dev->wq);
free_wlock:
	wake_lock_destroy(&dev->wlock);
free_xo_handle:
	msm_xo_put(dev->xo_handle);
free_regs:
	iounmap(dev->regs);
put_phy_clk:
	if (dev->phy_reset_clk)
		clk_put(dev->phy_reset_clk);
put_iface_clk:
	if (dev->iface_clk) {
		clk_disable_unprepare(dev->iface_clk);
		clk_put(dev->iface_clk);
	}
put_core_clk:
	clk_disable_unprepare(dev->core_clk);
	clk_put(dev->core_clk);
put_alt_core_clk:
	clk_put(dev->alt_core_clk);
rpc_fail:
	if (dev->pdata->rpc_connect)
		dev->pdata->rpc_connect(0);
free_dev:
	kfree(dev);
	return ret;
}

static int __exit msm_otg_remove(struct platform_device *pdev)
{
	struct msm_otg *dev = the_msm_otg;

	otg_debugfs_cleanup();
#ifdef CONFIG_USB_OTG
	sysfs_remove_group(&pdev->dev.kobj, &msm_otg_attr_grp);
#endif
	destroy_workqueue(dev->wq);
	wake_lock_destroy(&dev->wlock);

	if (dev->pdata->setup_gpio)
		dev->pdata->setup_gpio(USB_SWITCH_DISABLE);

	if (dev->pdata->init_vddcx)
		dev->pdata->init_vddcx(0);
	if (dev->pdata->ldo_enable)
		dev->pdata->ldo_enable(0);

	if (dev->pdata->ldo_init)
		dev->pdata->ldo_init(0);

	if (dev->pmic_vbus_notif_supp)
		dev->pdata->pmic_vbus_notif_init(&msm_otg_set_vbus_state, 0);

	if (dev->pdata->phy_id_setup_init)
		dev->pdata->phy_id_setup_init(0);

	if (dev->pmic_id_notif_supp)
		dev->pdata->pmic_id_notif_init(&msm_otg_set_id_state, 0);

#ifdef CONFIG_USB_MSM_ACA
	del_timer_sync(&dev->id_timer);
#endif
	if (dev->pdata->chg_init)
		dev->pdata->chg_init(0);
	free_irq(dev->irq, pdev);
	iounmap(dev->regs);
	clk_disable_unprepare(dev->core_clk);
	clk_put(dev->core_clk);
	if (dev->iface_clk) {
		clk_disable_unprepare(dev->iface_clk);
		clk_put(dev->iface_clk);
	}
	if (dev->alt_core_clk)
		clk_put(dev->alt_core_clk);
	if (dev->phy_reset_clk)
		clk_put(dev->phy_reset_clk);
	if (dev->pdata->rpc_connect)
		dev->pdata->rpc_connect(0);
	msm_xo_put(dev->xo_handle);
	pm_qos_remove_request(&dev->pdata->pm_qos_req_dma);

	pm_runtime_put(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	kfree(dev);
	return 0;
}

static int msm_otg_runtime_suspend(struct device *dev)
{
	struct msm_otg *otg = the_msm_otg;

	dev_dbg(dev, "pm_runtime: suspending...\n");
	msm_otg_suspend(otg);
	return  0;
}

static int msm_otg_runtime_resume(struct device *dev)
{
	struct msm_otg *otg = the_msm_otg;

	dev_dbg(dev, "pm_runtime: resuming...\n");
	msm_otg_resume(otg);
	return  0;
}

static int msm_otg_runtime_idle(struct device *dev)
{
	dev_dbg(dev, "pm_runtime: idling...\n");
	return  0;
}

static struct dev_pm_ops msm_otg_dev_pm_ops = {
	.runtime_suspend = msm_otg_runtime_suspend,
	.runtime_resume = msm_otg_runtime_resume,
	.runtime_idle = msm_otg_runtime_idle,
};

static struct platform_driver msm_otg_driver = {
	.remove = __exit_p(msm_otg_remove),
	.driver = {
		.name = DRIVER_NAME,
		.owner = THIS_MODULE,
		.pm = &msm_otg_dev_pm_ops,
	},
};

static int __init msm_otg_init(void)
{
	return platform_driver_probe(&msm_otg_driver, msm_otg_probe);
}

static void __exit msm_otg_exit(void)
{
	platform_driver_unregister(&msm_otg_driver);
}

module_init(msm_otg_init);
module_exit(msm_otg_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MSM usb transceiver driver");
MODULE_VERSION("1.00");
