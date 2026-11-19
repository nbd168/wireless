// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Copyright (C) 2023 MediaTek Inc. */

#include "mt7925.h"
#include "mcu.h"

static int
mt7925_mcu_send_message(struct mt76_dev *mdev, struct sk_buff *skb,
			int cmd, int *seq)
{
	struct mt792x_dev *dev = container_of(mdev, struct mt792x_dev, mt76);
	enum mt76_mcuq_id txq = MT_MCUQ_WM;
	int ret;

	ret = mt7925_mcu_fill_message(mdev, skb, cmd, seq);
	if (ret)
		return ret;

	mdev->mcu.timeout = 3 * HZ;

	if (cmd == MCU_CMD(FW_SCATTER))
		txq = MT_MCUQ_FWDL;

	return mt76_tx_queue_skb_raw(dev, mdev->q_mcu[txq], skb, 0);
}

/* The ROM bootloader may answer a command without raising the interrupt.
 * Until the firmware runs, look for pending events from a timer as well.
 */
#define MT7925E_FWDL_POLL_MS	10

struct mt7925e_fwdl_poll {
	struct timer_list timer;
	struct mt792x_dev *dev;
	u32 pending;
};

static void mt7925e_fwdl_poll_fn(struct timer_list *t)
{
	struct mt7925e_fwdl_poll *poll = timer_container_of(poll, t, timer);
	struct mt792x_dev *dev = poll->dev;
	u32 pending;

	/* An enabled interrupt that stays pending for a whole period was lost. */
	pending = mt76_rr(dev, MT_WFDMA0_HOST_INT_STA) &
		  mt76_rr(dev, dev->irq_map->host_irq_enable);
	if (pending & poll->pending) {
		dev_warn_once(dev->mt76.dev,
			      "pending interrupt was not delivered, polling for MCU events\n");
		mt792x_irq_handler(0, dev);
		pending = 0;
	}
	poll->pending = pending;

	mod_timer(&poll->timer,
		  jiffies + msecs_to_jiffies(MT7925E_FWDL_POLL_MS));
}

int mt7925e_mcu_init(struct mt792x_dev *dev)
{
	static const struct mt76_mcu_ops mt7925_mcu_ops = {
		.headroom = sizeof(struct mt76_connac2_mcu_txd),
		.mcu_skb_send_msg = mt7925_mcu_send_message,
		.mcu_parse_response = mt7925_mcu_parse_response,
	};
	struct mt7925e_fwdl_poll poll;
	int err;

	dev->mt76.mcu_ops = &mt7925_mcu_ops;

	err = mt792xe_mcu_fw_pmctrl(dev);
	if (err)
		return err;

	err = __mt792xe_mcu_drv_pmctrl(dev);
	if (err)
		return err;

	mt76_rmw_field(dev, dev->pcie_reg->pm, MT_PCIE_MAC_PM_L0S_DIS, 1);

	poll.dev = dev;
	poll.pending = 0;
	timer_setup(&poll.timer, mt7925e_fwdl_poll_fn, 0);
	mod_timer(&poll.timer,
		  jiffies + msecs_to_jiffies(MT7925E_FWDL_POLL_MS));

	err = mt7925_run_firmware(dev);

	timer_shutdown_sync(&poll.timer);
	if (!err)
		mt7925e_axidma_cal_cache_init(dev);

	mt76_queue_tx_cleanup(dev, dev->mt76.q_mcu[MT_MCUQ_FWDL], false);

	return err;
}
