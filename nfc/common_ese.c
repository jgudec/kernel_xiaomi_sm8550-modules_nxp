/******************************************************************************
 *  Copyright (C) 2020 NXP
 *   *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 ******************************************************************************/

#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <linux/fs.h>
#include "common.h"
#include "common_ese.h"

static void cold_reset_gaurd_timer_callback(struct timer_list *t)
{
	cold_reset_t *cold_reset = from_timer(cold_reset, t, timer);
	pr_debug("%s: Enter\n", __func__);
	cold_reset->in_progress = false;
	return;
}

static long start_cold_reset_guard_timer(cold_reset_t *cold_reset)
{
	long ret = -EINVAL;
	if (timer_pending(&cold_reset->timer) == 1) {
		pr_debug("ese_cold_reset_guard_timer: delete pending timer \n");
		/* delete timer if already pending */
		del_timer(&cold_reset->timer);
	}
	cold_reset->in_progress = true;
	timer_setup(&cold_reset->timer, cold_reset_gaurd_timer_callback, 0);
	ret = mod_timer(&cold_reset->timer,
			jiffies + msecs_to_jiffies(ESE_CLD_RST_GUARD_TIME));
	return ret;
}

static int send_cold_reset_protection_cmd(nfc_dev_t *nfc_dev, bool requestType)
{
	int ret = 0;
	int length = 0;
	uint8_t *cmd = NULL;
	uint8_t cld_rst_cmd[] = { NCI_PROP_MSG_CMD, CLD_RST_OID,
				  CLD_RST_PAYLOAD_SIZE
				};
	uint8_t rst_prot_cmd[] = { NCI_PROP_MSG_CMD, RST_PROT_OID,
				   RST_PROT_PAYLOAD_SIZE, 0x00
				 };

	cold_reset_t *cold_reset = &nfc_dev->cold_reset;
	if (requestType) {
		length = sizeof(rst_prot_cmd);
		rst_prot_cmd[NCI_PAYLOAD_IDX] = (!cold_reset->reset_protection) ? 1 : 0;
		cmd = rst_prot_cmd;
	} else {
		length = sizeof(cld_rst_cmd);
		cmd = cld_rst_cmd;
	}

	ret = nfc_dev->nfc_write(nfc_dev, cmd, length, MAX_RETRY_COUNT);
	if (ret != length) {
		pr_err("%s : nfc_write returned %d\n", __func__, ret);
		return -EIO;
	}

	if (requestType) {
		pr_debug("%s: NxpNciX: %d > %02X%02X%02X%02X\n", __func__, ret, cmd[0], cmd[1],
			 cmd[2], cmd[3]);
	} else {
		pr_debug("%s: NxpNciX: %d > %02X%02X%02X\n", __func__, ret, cmd[0], cmd[1],
			 cmd[2]);
	}
	return ret;
}

void wakeup_on_prop_rsp(nfc_dev_t *nfc_dev, uint8_t *buf)
{
	cold_reset_t *cold_reset = &nfc_dev->cold_reset;
	cold_reset->status = -EIO;

	if ((NCI_HDR_LEN + buf[NCI_PAYLOAD_LEN_IDX]) != NCI_PROP_MSG_RSP_LEN) {
		pr_err("%s: - invalid response for cold_reset/protection \n", __func__);
	} else {
		cold_reset->status = buf[NCI_PAYLOAD_IDX];
	}
	pr_debug("%s NxpNciR : len = 4 > %02X%02X%02X%02X\n", __func__, buf[0], buf[1],
		buf[2], buf[3]);

	cold_reset->rsp_pending = false;
	wake_up_interruptible(&cold_reset->read_wq);
}

static int validate_cold_reset_protection_request(cold_reset_t *cold_reset,
		unsigned long arg)
{
	if (!cold_reset->reset_protection) {
		if (IS_RST_PROT_EN_REQ(arg) && IS_SRC_VALID_PROT(arg)) {
			pr_debug("%s:req - reset protection enable\n", __func__);
		} else if (IS_CLD_RST_REQ(arg) && IS_SRC_VALID(arg)) {
			pr_debug("%s:req - cold reset\n", __func__);
		} else if (IS_RST_PROT_DIS_REQ(arg) && IS_SRC_VALID_PROT(arg)) {
			pr_debug("%s:req - reset protection already disable\n", __func__);
			return -EINVAL;
		} else {
			pr_err("%s:Operation not permitted \n", __func__);
			return -EPERM;
		}
	} else {
		if (IS_RST_PROT_DIS_REQ(arg)
		    && IS_SRC(arg, cold_reset->rst_prot_src)) {
			pr_debug("%s:req - disable reset protection from same src\n", __func__);
		} else if (IS_CLD_RST_REQ(arg)
			   && IS_SRC(arg, cold_reset->rst_prot_src)) {
			pr_debug("%s:req - cold reset from same source\n", __func__);
		} else if (IS_RST_PROT_EN_REQ(arg)
			   && IS_SRC(arg, cold_reset->rst_prot_src)) {
			pr_debug("%s:request - enable reset protection from same source\n", __func__);
		} else {
			pr_err("%s: Operation not permitted \n", __func__);
			return -EPERM;
		}
	}
	return 0;
}

static int perform_cold_reset_protection(nfc_dev_t *nfc_dev, unsigned long arg)
{
	int ret = 0;
	struct file filp;
	cold_reset_t *cold_reset = &nfc_dev->cold_reset;
	bool nfc_dev_opened = false;

	/*check if NFCC not in the FW download or hard reset state */
	ret = validate_nfc_state_nci(nfc_dev);
	if (ret < 0) {
		pr_err("%s: invalid cmd", __func__);
		return ret;
	}

	/* check if NFC is enabled */
	mutex_lock(&nfc_dev->dev_ref_mutex);
	nfc_dev_opened = (nfc_dev->dev_ref_count > 0) ? true : false;
	mutex_unlock(&nfc_dev->dev_ref_mutex);

	mutex_lock(&cold_reset->sync_mutex);
	/*check if NFCC not in the FW download or hard reset state */
	ret = validate_cold_reset_protection_request(cold_reset, arg);
	if (ret < 0) {
		pr_err("%s: invalid cmd", __func__);
		goto err;
	}

	/*check if cold reset already in progress */
	if (IS_CLD_RST_REQ(arg) && cold_reset->in_progress) {
		pr_err("%s: cold reset already in progress", __func__);
		ret = -EBUSY;
		goto err;
	}
	/* set default value for status as failure */
	cold_reset->status = -EIO;
	cold_reset->rsp_pending = true;

	/*enable interrupt before sending cmd, when devnode not opened by HAL */
	if (!nfc_dev_opened)
		nfc_dev->nfc_enable_intr(nfc_dev);

	ret = send_cold_reset_protection_cmd(nfc_dev, IS_RST_PROT_REQ(arg));
	if (ret < 0) {
		pr_err("failed to send cold reset/protection command\n");
		cold_reset->rsp_pending = false;
		goto err;
	}
	ret = 0;
	/*start the cold reset guard timer */
	if (IS_CLD_RST_REQ(arg)) {
		/*Guard timer not needed when OSU over NFC*/
		if(!(cold_reset->reset_protection && IS_SRC_NFC(arg))) {
			ret = start_cold_reset_guard_timer(cold_reset);
			if (ret) {
				pr_err("%s: Error in mod_timer\n", __func__);
				goto err;
			}
		}
	}

	do {
		/* Read is pending from the HAL service which will complete the response */
		if (nfc_dev_opened) {
			if (!wait_event_interruptible_timeout
			    (cold_reset->read_wq,
			     cold_reset->rsp_pending == false,
			     msecs_to_jiffies(NCI_CMD_RSP_TIMEOUT))) {
				pr_err("%s:cold reset/protection response timeout\n", __func__);
				ret = -EAGAIN;
			}
		} else {
			/* Read data as NFC thread is not active */
			filp.private_data = nfc_dev;
#if IS_ENABLED(CONFIG_NXP_NFC_I2C)
			if (nfc_dev->interface == PLATFORM_IF_I2C) {
				filp.f_flags &= ~O_NONBLOCK;
				ret = nfc_i2c_dev_read(&filp, NULL, 3, 0);
				usleep_range(3500, 4000);
			}
#endif //IS_ENABLED(CONFIG_NXP_NFC_I2C)
		}
	} while (ret == -ERESTARTSYS || ret == -EFAULT);

	if (ret == 0) {		/* success case */
		ret = cold_reset->status;
		if (IS_RST_PROT_REQ(arg)) {
			cold_reset->reset_protection = IS_RST_PROT_EN_REQ(arg);
			cold_reset->rst_prot_src =
				IS_RST_PROT_EN_REQ(arg) ? GET_SRC(arg) : SRC_NONE;
			/* wait for reboot guard timer */
		} else if (wait_event_interruptible_timeout
			   (cold_reset->read_wq, true,
			    msecs_to_jiffies(ESE_CLD_RST_REBOOT_GUARD_TIME)) ==
			   0) {
			pr_info("%s: reboot guard timer timeout", __func__);
		}
	}
err:
	mutex_unlock(&cold_reset->sync_mutex);
	return ret;
}

/*
 * Power management of the eSE
 * eSE and NFCC both are powered using VEN gpio,
 * VEN HIGH - eSE and NFCC both are powered on
 * VEN LOW - eSE and NFCC both are power down
 */
int nfc_ese_pwr(nfc_dev_t *nfc_dev, unsigned long arg)
{
	int ret = 0;
	if (arg == ESE_POWER_ON) {
		/**
		 * Let's store the NFC VEN pin state
		 * will check stored value in case of eSE power off request,
		 * to find out if NFC MW also sent request to set VEN HIGH
		 * VEN state will remain HIGH if NFC is enabled otherwise
		 * it will be set as LOW
		 */
		nfc_dev->nfc_ven_enabled = gpio_get_value(nfc_dev->gpio.ven);
		if (!nfc_dev->nfc_ven_enabled) {
			pr_debug("eSE HAL service setting ven HIGH\n");
			gpio_set_ven(nfc_dev, 1);
		} else {
			pr_debug("ven already HIGH\n");
		}
	} else if (arg == ESE_POWER_OFF) {
		if (!nfc_dev->nfc_ven_enabled) {
			pr_debug("NFC not enabled, disabling ven\n");
			gpio_set_ven(nfc_dev, 0);
		} else {
			pr_debug("keep ven high as NFC is enabled\n");
		}
	} else if (arg == ESE_POWER_STATE) {
		// eSE get power state
		ret = gpio_get_value(nfc_dev->gpio.ven);
	} else if (IS_CLD_RST_REQ(arg) || IS_RST_PROT_REQ(arg)) {
		ret = perform_cold_reset_protection(nfc_dev, arg);
	} else {
		pr_err("%s bad arg %lu\n", __func__, arg);
		ret = -ENOIOCTLCMD;
	}
	return ret;
}

EXPORT_SYMBOL(nfc_ese_pwr);

#define ESE_LEGACY_INTERFACE
#ifdef ESE_LEGACY_INTERFACE
static nfc_dev_t *nfc_dev_legacy = NULL;

/******************************************************************************
 * perform_ese_cold_reset() - It shall be called by others driver(not nfc/ese)
 * to perform cold reset only
 * @arg: request of cold reset from other drivers should be ESE_CLD_RST_OTHER
 *
 * Returns:- 0 in case of sucess and negative values in case of failure
 *****************************************************************************/
int perform_ese_cold_reset(unsigned long arg)
{
	int ret = 0;
	if (nfc_dev_legacy) {
		if (IS_CLD_RST_REQ(arg) && IS_SRC_OTHER(arg)) {
			ret = nfc_ese_pwr(nfc_dev_legacy, arg);
		} else {
			pr_err("%s :  Operation not permitted \n", __func__);
			return -EPERM;
		}
	}
	pr_debug("%s:%d exit, status:%lu", __func__, arg, ret);
	return ret;
}

EXPORT_SYMBOL(perform_ese_cold_reset);
#endif //ESE_LEGACY_INTERFACE

void common_ese_on_hard_reset(nfc_dev_t *nfc_dev)
{
	cold_reset_t *cold_reset = &nfc_dev->cold_reset;
	cold_reset->rsp_pending = false;
	cold_reset->in_progress = false;
	if (timer_pending(&cold_reset->timer) == 1) {
		del_timer(&cold_reset->timer);
	}
}

void common_ese_init(nfc_dev_t *nfc_dev)
{
	cold_reset_t *cold_reset = &nfc_dev->cold_reset;
	cold_reset->reset_protection = false;
	cold_reset->rst_prot_src = SRC_NONE;
	init_waitqueue_head(&cold_reset->read_wq);
	mutex_init(&cold_reset->sync_mutex);
	common_ese_on_hard_reset(nfc_dev);
#ifdef ESE_LEGACY_INTERFACE
	nfc_dev_legacy = nfc_dev;
#endif //ESE_LEGACY_INTERFACE
}

void common_ese_exit(nfc_dev_t *nfc_dev)
{
	mutex_destroy(&nfc_dev->cold_reset.sync_mutex);
#ifdef ESE_LEGACY_INTERFACE
	nfc_dev_legacy = NULL;
#endif //ESE_LEGACY_INTERFACE
}
