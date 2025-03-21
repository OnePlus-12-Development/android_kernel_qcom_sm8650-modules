/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021,2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef BTFM_SLIM_H
#define BTFM_SLIM_H
#include <linux/slimbus.h>

#define BTFMSLIM_DBG(fmt, arg...)  pr_debug("%s: " fmt "\n", __func__, ## arg)
#define BTFMSLIM_INFO(fmt, arg...) pr_info("%s: " fmt "\n", __func__, ## arg)
#define BTFMSLIM_ERR(fmt, arg...)  pr_err("%s: " fmt "\n", __func__, ## arg)

/* Vendor specific defines
 * This should redefines in slimbus slave specific header
 */
#define SLIM_SLAVE_COMPATIBLE_STR	"btfmslim_slave"
#define SLIM_SLAVE_REG_OFFSET		0x0000
#define SLIM_SLAVE_RXPORT		NULL
#define SLIM_SLAVE_TXPORT		NULL
#define SLIM_SLAVE_INIT			NULL
#define SLIM_SLAVE_PORT_EN		NULL

/* Misc defines */
#define SLIM_SLAVE_RW_MAX_TRIES		3
#define SLIM_SLAVE_PRESENT_TIMEOUT	100

#define PGD	1
#define IFD	0

#if IS_ENABLED(CONFIG_BTFM_SLIM)
#define BTFMSLIM_DEV_NAME "btfmslim_slave"
#else
#define BTFMSLIM_DEV_NAME "btfmslim"
#endif

/* Codec driver defines */
enum {
	BTFM_FM_SLIM_TX = 0,
	BTFM_BT_SCO_SLIM_TX,
	BTFM_BT_SCO_A2DP_SLIM_RX,
	BTFM_BT_SPLIT_A2DP_SLIM_RX,
	BTFM_BT_SPLIT_A2DP_SLIM_TX,
	BTFM_SLIM_NUM_CODEC_DAIS
};

struct btfm_slim_codec_dai_data {
	struct slim_stream_config sconfig;
	struct slim_stream_runtime *sruntime;
};

struct btfmslim_ch {
	int id;
	char *name;
	uint16_t port;		/* slimbus port number */
	uint8_t ch;		/* slimbus channel number */
	struct btfm_slim_codec_dai_data dai;
};

/* Slimbus Port defines - This should be redefined in specific device file */
#define BTFM_SLIM_PGD_PORT_LAST				0xFF

struct btfmslim {
	struct device *dev;
	struct slim_device *slim_pgd; //Physical address
	struct slim_device slim_ifd; //Interface address
	struct mutex io_lock;
	struct mutex xfer_lock;
	uint8_t enabled;
	uint32_t num_rx_port;
	uint32_t num_tx_port;
	uint32_t sample_rate;
	int dai_id;
	uint32_t bps;
	uint16_t direction;
	struct btfmslim_ch *rx_chs;
	struct btfmslim_ch *tx_chs;
	int (*vendor_init)(struct btfmslim *btfmslim);
	int (*vendor_port_en)(struct btfmslim *btfmslim, uint8_t port_num,
		uint8_t rxport, uint8_t enable);
#if IS_ENABLED(CONFIG_SLIM_BTFM_CODEC)
	int device_id;
#endif
};

extern int btfm_feedback_ch_setting;

/**
 * btfm_slim_hw_init: Initialize slimbus slave device
 * Returns:
 * 0: Success
 * else: Fail
 */
int btfm_slim_hw_init(struct btfmslim *btfmslim);

/**
 * btfm_slim_hw_deinit: Deinitialize slimbus slave device
 * Returns:
 * 0: Success
 * else: Fail
 */
int btfm_slim_hw_deinit(struct btfmslim *btfmslim);

/**
 * btfm_slim_write: write value to pgd or ifd device
 * @btfmslim: slimbus slave device data pointer.
 * @reg: slimbus slave register address
 * @reg_val: value to write at register address
 * @pgd: selection for device: either PGD or IFD
 * Returns:
   No of bytes written
   -1
 */
int btfm_slim_write(struct btfmslim *btfmslim,
	uint16_t reg, uint8_t reg_val, uint8_t pgd);



/**
 * btfm_slim_read: read value from pgd or ifd device
 * @btfmslim: slimbus slave device data pointer.
 * @reg: slimbus slave register address
 * @dest: data pointer to read
 * @pgd: selection for device: either PGD or IFD
 * Returns:
   No of bytes read
   -1
 */
int btfm_slim_read(struct btfmslim *btfmslim,
	uint32_t reg, uint8_t pgd);


/**
 * btfm_slim_enable_ch: enable channel for slimbus slave port
 * @btfmslim: slimbus slave device data pointer.
 * @ch: slimbus slave channel pointer
 * @rxport: rxport or txport
 * Returns:
 * -EINVAL
 * -ETIMEDOUT
 * -ENOMEM
 */
int btfm_slim_enable_ch(struct btfmslim *btfmslim,
	struct btfmslim_ch *ch, uint8_t rxport, uint32_t rates,
	uint8_t nchan);

/**
 * btfm_slim_disable_ch: disable channel for slimbus slave port
 * @btfmslim: slimbus slave device data pointer.
 * @ch: slimbus slave channel pointer
 * @rxport: rxport or txport
 * @nChan: number of chaneels.
 * Returns:
 * -EINVAL
 * -ETIMEDOUT
 * -ENOMEM
 */
int btfm_slim_disable_ch(struct btfmslim *btfmslim,
	struct btfmslim_ch *ch, uint8_t rxport, uint8_t nchan);

/**
 * btfm_slim_register_codec: Register codec driver in slimbus device node
 * @btfmslim: slimbus slave device data pointer.
 * Returns:
 * -ENOMEM
 * 0
 */
int btfm_slim_register_codec(struct btfmslim *btfmslim);

/**
 * btfm_slim_unregister_codec: Unregister codec driver in slimbus device node
 * @dev: device node
 * Returns:
 * VOID
 */
void btfm_slim_unregister_codec(struct device *dev);
#endif /* BTFM_SLIM_H */
