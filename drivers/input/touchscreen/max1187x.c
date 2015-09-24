/* drivers/input/touchscreen/max1187x.c
 *
 * Copyright (c)2013 Maxim Integrated Products, Inc.
 * Copyright (C) 2012-2014 Sony Mobile Communications AB.
 *
 * Driver Version: 3.1.8
 * Release Date: May 10, 2013
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <asm/byteorder.h>
#include <linux/crc16.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/max1187x.h>
#include <linux/input/max1187x_config.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <asm-generic/cputime.h>

#define NWORDS(a)    (sizeof(a) / sizeof(u16))
#define BYTE_SIZE(a) ((a) * sizeof(u16))
#define BYTEH(a)     ((a) >> 8)
#define BYTEL(a)     ((a) & 0xFF)
#define HI_NIBBLE(a) (((a) & 0xF0) >> 4)
#define LO_NIBBLE(a) ((a) & 0x0F)
#define DEBUG_FLAG(DEV, NAME, ...) ({		\
	bool debug_flag = false; \
	dev_dbg(DEV, NAME "%s", ##__VA_ARGS__, (debug_flag = true, "")); \
	debug_flag; \
})
#define INFO_BUFFER(DEV, BUF, LEN, FMT, FLN) ({			\
	int i, written;							\
	char debug_string[MXM_DEBUG_STRING_LEN_MAX];			\
	for (i = 0, written = 0; i < (LEN); i++) {			\
		written += snprintf(debug_string + written, FLN, FMT, BUF[i]); \
		if (written + FLN >= MXM_DEBUG_STRING_LEN_MAX) {	\
			dev_info(DEV, "%s", debug_string);		\
			written = 0;					\
		}							\
	}								\
	if (written > 0)						\
		dev_info(DEV, "%s", debug_string);			\
})
#define DEBUG_BUFFER(DEV, NAME, BUF, LEN, FMT, FLN) ({	\
	if (DEBUG_FLAG(DEV, NAME " (%d)", LEN))		\
		INFO_BUFFER(DEV, BUF, LEN, FMT, FLN);	\
})

#define MXM_TOUCH_WAKEUP_FEATURE

#define MXM_CMD_LEN_PACKET_MAX      9
#define MXM_CMD_LEN_MAX             (15 * MXM_CMD_LEN_PACKET_MAX)
#define MXM_CMD_LEN_PACKET_MIN      2
#define MXM_CMD_ID_AND_SIZE_LEN     2
#define MXM_CMD_ADD_AND_HDR_LEN     2
#define MXM_RPT_LEN_PACKET_MAX      245
#define MXM_RPT_LEN_MAX             1000
#define MXM_RPT_FIRST_PACKET        1
#define MXM_RPT_PKT_HDR_LEN         1
#define MXM_RPT_HDR_AND_ID_LEN      2
#define MXM_RPT_MAX_WORDS_PER_PKT   0xF4
#define MXM_ONE_PACKET_RPT          0x11
#define MXM_TOUCH_REPORT_MODE_EXT   0x02
#define MXM_REPORT_READERS_MAX      5
#define MXM_BYTES_LEN_IN_WORDS      2
#define MXM_BYTES_LEN_WR_BYTES      6
#define MXM_BYTES_LEN_WR_WORDS      8
#define MXM_DEBUG_STRING_LEN_MAX    60
#define MXM_PWR_DATA_WAKEUP_GEST    0x0102
#define MXM_TOUCH_COUNT_MAX         10
#define MXM_PRESSURE_Z_MIN_TO_SQRT  2
#define MXM_PRESSURE_SQRT_MAX       181
#define MXM_LCD_X_MIN               480
#define MXM_LCD_Y_MIN               240
#define MXM_LCD_SIZE_MAX            0x7FFF
#define MXM_NUM_SENSOR_MAX          40

/* Timings */
#define MXM_WAIT_MIN_US             1000
#define MXM_WAIT_MAX_US             2000
#define MXM_PWR_SET_WAIT_MS         100
#define MXM_PWR_RESET_WAIT_MS       30

/* Regulator */
#define MXM_LPM_UA_LOAD       2000
#define MXM_HPM_UA_LOAD       15000
#define MXM_VREG_MAX_UV       3000000

/* Bootloader */
#define MXM_BL_STATUS_ADDR_H     0x00
#define MXM_BL_STATUS_ADDR_L     0xFF
#define MXM_BL_DATA_ADDR_H       0x00
#define MXM_BL_DATA_ADDR_L       0xFE
#define MXM_BL_STATUS_READY_H    0xAB
#define MXM_BL_STATUS_READY_L    0xCC
#define MXM_BL_DATA_READY_H      0x00
#define MXM_BL_DATA_READY_L      0x3E
#define MXM_BL_RXTX_COMPLETE_H   0x54
#define MXM_BL_RXTX_COMPLETE_L   0x32
#define MXM_BL_ENTER_SEQ_L       0x7F00
#define MXM_BL_ENTER_SEQ_H1      0x0047
#define MXM_BL_ENTER_SEQ_H2      0x00C7
#define MXM_BL_ENTER_SEQ_H3      0x0007
#define MXM_BL_ENTER_RETRY       3
#define MXM_BL_ENTER_CONF_RETRY  5
#define MXM_BL_SET_BYTE_MODE_H   0x00
#define MXM_BL_SET_BYTE_MODE_L   0x0A
#define MXM_BL_ERASE_CONF_RETRY  10
#define MXM_BL_ERASE_DELAY_MS    60
#define MXM_BL_ERASE_FLASH_L     0x02
#define MXM_BL_ERASE_FLASH_H     0x00
#define MXM_BL_RD_STATUS_RETRY   3
#define MXM_BL_WR_FAST_FLASH_L   0xF0
#define MXM_BL_WR_FAST_FLASH_H   0x00
#define MXM_BL_WR_START_ADDR     0x00
#define MXM_BL_WR_DBUF0_ADDR     0x00
#define MXM_BL_WR_DBUF1_ADDR     0x40
#define MXM_BL_WR_BLK_SIZE       128
#define MXM_BL_WR_TX_SZ          130
#define MXM_BL_WR_MIN_US         10000
#define MXM_BL_WR_MAX_US         11000
#define MXM_BL_WR_STATUS_RETRY   100
#define MXM_BL_WR_DELAY_MS       200
#define MXM_BL_WR_CONF_RETRY     5
#define MXM_BL_GET_CRC_L         0x30
#define MXM_BL_GET_CRC_H         0x02
#define MXM_BL_CRC_GET_RETRY     5
#define MXM_BL_EXIT_SEQ_L        0x7F00
#define MXM_BL_EXIT_SEQ_H1       0x0040
#define MXM_BL_EXIT_SEQ_H2       0x00C0
#define MXM_BL_EXIT_SEQ_H3       0x0000
#define MXM_BL_EXIT_RETRY        3

/* Firmware Update */
#define MXM_FW_RETRIES_MAX    5
#define MXM_FW_UPDATE_DEFAULT 0 /* flashing with dflt_cfg if fw is corrupted */
#define MXM_FW_UPDATE_FORCE   1 /* force flashing with dflt_cfg */

static const char * const fw_update_mode[] = {
	[MXM_FW_UPDATE_DEFAULT] = "default",
	[MXM_FW_UPDATE_FORCE] = "force",
};

enum maxim_coordinate_settings {
	MXM_REVERSE_X   = 0x0001,
	MXM_REVERSE_Y   = 0x0002,
	MXM_SWAP_XY     = 0x0004,
};

enum maxim_start_address {
	MXM_CMD_START_ADDR  = 0x0000,
	MXM_RPT_START_ADDR  = 0x000A,
};

enum maxim_command_size {
	MXM_ZERO_SIZE_CMD  = 0x0000,
	MXM_ONE_SIZE_CMD   = 0x0001,
};

enum maxim_command_id {
	MXM_CMD_ID_GET_CFG_INF         = 0x0002,
	MXM_CMD_ID_SET_TOUCH_RPT_MODE  = 0x0018,
	MXM_CMD_ID_SET_POWER_MODE      = 0x0020,
	MXM_CMD_ID_GET_FW_VERSION      = 0x0040,
	MXM_CMD_ID_RESET_SYSTEM        = 0x00E9,
};

enum maxim_report_id {
	MXM_RPT_ID_CFG_INF           = 0x0102,
	MXM_RPT_ID_POWER_MODE        = 0x0121,
	MXM_RPT_ID_FW_VERSION        = 0x0140,
	MXM_RPT_ID_SYS_STATUS        = 0x01A0,
	MXM_RPT_ID_TOUCH_RAW_IMAGE   = 0x0800,
	MXM_RPT_ID_EXT_TOUCH_INFO    = 0x0802,
};

enum maxim_power_mode {
#ifdef MXM_TOUCH_WAKEUP_FEATURE
	MXM_PWR_SLEEP_MODE  = 0x0001,
#else
	MXM_PWR_SLEEP_MODE  = 0x0000,
#endif
	MXM_ACTIVE_MODE     = 0x0002,
	MXM_WAKEUP_MODE     = 0x0006,
};

struct max1187x_packet_header {
	u16 total_num;
	u16 curr_num;
	u16 curr_size;
};

struct max1187x_touch_report_header {
	u16 header;
	u16 report_id;
	u16 report_size;
	u16 touch_count:4;
	u16 reserved0:12;
	u16 button0:1;
	u16 button1:1;
	u16 button2:1;
	u16 button3:1;
	u16 reserved1:12;
	u16 framecounter;
};

struct max1187x_touch_report_extended {
	u16 finger_id:4;
	u16 reserved0:4;
	u16 tool_type:4;
	u16 reserved1:4;
	u16 x:12;
	u16 reserved2:4;
	u16 y:12;
	u16 reserved3:4;
	u16 z;
	s16 xspeed;
	s16 yspeed;
	s8 xpixel;
	s8 ypixel;
	u16 area;
	u16 xmin;
	u16 xmax;
	u16 ymin;
	u16 ymax;
};

struct report_reader {
	u16 report_id;
	u16 reports_passed;
	struct semaphore sem;
	int status;
};

struct data {
	struct max1187x_pdata *pdata;
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct input_dev *input_pen;
	char phys[32];
#ifdef MXM_TOUCH_WAKEUP_FEATURE
	struct input_dev *input_dev_key;
	char phys_key[32];
#endif
	bool is_suspended;
	struct regulator *vreg_touch_vdd;
	char *vdd_supply_name;
	wait_queue_head_t waitqueue_all;

	u16 chip_id;
	u16 config_id;

	struct mutex fw_mutex;
	struct mutex i2c_mutex;
	struct mutex report_mutex;
	struct semaphore report_sem;
	struct report_reader report_readers[MXM_REPORT_READERS_MAX];
	u8 report_readers_outstanding;

	u16 cmd_buf[MXM_CMD_LEN_MAX];
	u16 cmd_len;

	struct semaphore sema_rbcmd;
	wait_queue_head_t waitqueue_rbcmd;
	u8 rbcmd_waiting;
	u8 rbcmd_received;
	u16 rbcmd_report_id;
	u16 rbcmd_rx_report[MXM_RPT_LEN_MAX];
	u16 rbcmd_rx_report_len;

	u16 rx_report[MXM_RPT_LEN_MAX]; /* with header */
	u16 rx_report_len;
	u16 rx_packet[MXM_RPT_LEN_PACKET_MAX + 1]; /* with header */
	u32 irq_count;
	u16 framecounter;
	u16 list_finger_ids;
	u16 list_tool_ids;
	u16 curr_finger_ids;
	u16 curr_tool_ids;
	u8 used_tools;
	u8 fw_update_mode;
	u8 sysfs_created;
	bool is_raw_mode;
	int screen_status;

	u16 button0:1;
	u16 button1:1;
	u16 button2:1;
	u16 button3:1;
};

#ifdef MXM_TOUCH_WAKEUP_FEATURE
#define DT2W_DEFAULT_MODE	1
#define DT2W_COUNT		2
#define DT2W_DEFAULT_TIME	700
int touch_no = 0;
int dt2w_active = DT2W_DEFAULT_MODE;
int dt2w_time = DT2W_DEFAULT_TIME;
cputime64_t dt2w_tap_time = 0;
#endif

static int vreg_configure(struct data *ts, bool enable);

static void validate_fw(struct data *ts);
static int bootloader_enter(struct data *ts);
static int bootloader_exit(struct data *ts);
static int bootloader_get_crc(struct data *ts, u16 *crc16,
		u16 addr, u16 len, u16 delay);
static int bootloader_set_byte_mode(struct data *ts);
static int bootloader_erase_flash(struct data *ts);
static int bootloader_write_flash(struct data *ts, const u8 *image, u16 length);

static void propagate_report(struct data *ts, int status, u16 *report);
static int get_report(struct data *ts, u16 report_id, ulong timeout);
static void release_report(struct data *ts);
static int cmd_send(struct data *ts, u16 *buf, u16 len);
static int rbcmd_send_receive(struct data *ts, u16 *buf,
		u16 len, u16 report_id, u16 timeout);
static u16 max1187x_sqrt(u32 num);
static int reset_power(struct data *ts);
static void set_resume_mode(struct data *ts);
static void set_suspend_mode(struct data *ts);

/* I2C communication */
static int i2c_rx_bytes(struct data *ts, u8 *buf, u16 len)
{
	int ret;
	struct device *dev = &ts->client->dev;

	do {
		ret = i2c_master_recv(ts->client, (char *) buf, (int) len);
	} while (ret == -EAGAIN);
	if (ret < 0) {
		dev_err(dev, "I2C RX fail (%d)", ret);
		return ret;
	}

	len = ret;

	DEBUG_BUFFER(dev, "i2c_debug:rx",
		     buf, len, "0x%02x ", MXM_BYTES_LEN_WR_BYTES);

	return len;
}

static int i2c_rx_words(struct data *ts, u16 *buf, u16 len)
{
	int i, ret;
	struct device *dev = &ts->client->dev;

	do {
		ret = i2c_master_recv(ts->client,
			(char *) buf, (int) (len * MXM_BYTES_LEN_IN_WORDS));
	} while (ret == -EAGAIN);
	if (ret < 0) {
		dev_err(dev, "I2C RX fail (%d)", ret);
		return ret;
	}

	if (ret % 2) {
		dev_err(dev, "I2C words RX fail: odd number bytes (%d)", ret);
		return -EIO;
	}

	len = ret/2;

	for (i = 0; i < len; i++)
		buf[i] = cpu_to_le16(buf[i]);

	DEBUG_BUFFER(dev, "i2c_debug:rx",
		     buf, len, "0x%04x ", MXM_BYTES_LEN_WR_WORDS);

	return len;
}

static int i2c_tx_bytes(struct data *ts, u8 *buf, u16 len)
{
	int ret;
	struct device *dev = &ts->client->dev;

	do {
		ret = i2c_master_send(ts->client, (char *) buf, (int) len);
	} while (ret == -EAGAIN);
	if (ret < 0) {
		dev_err(dev, "I2C TX fail (%d)", ret);
		return ret;
	}

	len = ret;

	DEBUG_BUFFER(dev, "i2c_debug:tx",
		     buf, len, "0x%02x ", MXM_BYTES_LEN_WR_BYTES);

	return len;
}

static int i2c_tx_words(struct data *ts, u16 *buf, u16 len)
{
	int i, ret;
	struct device *dev = &ts->client->dev;

	for (i = 0; i < len; i++)
		buf[i] = cpu_to_le16(buf[i]);

	do {
		ret = i2c_master_send(ts->client,
			(char *) buf, (int) (len * MXM_BYTES_LEN_IN_WORDS));
	} while (ret == -EAGAIN);
	
	if (ret < 0) {
		dev_err(dev, "I2C TX fail (%d)", ret);
		return ret;
	}
	if (ret % 2) {
		dev_err(dev, "I2C words TX fail: odd number bytes (%d)", ret);
		return -EIO;
	}

	len = ret/2;

	DEBUG_BUFFER(dev, "i2c_debug:tx",
		     buf, len, "0x%04x ", MXM_BYTES_LEN_WR_WORDS);

	return len;
}

/* Read report */
static int read_mtp_report(struct data *ts, u16 *buf)
{
	int words = 1, words_tx, words_rx;
	int ret = 0, remainder = 0, offset = 0;
	u16 address = MXM_RPT_START_ADDR;
	struct device *dev = &ts->client->dev;

	/* read header, get size, read entire report */
	words_tx = i2c_tx_words(ts, &address, 1);
	if (words_tx != 1) {
		dev_err(dev, "Report RX fail: failed to set address");
		return -EIO;
	}

	if (!ts->is_raw_mode) {
		words_rx = i2c_rx_words(ts, buf, MXM_RPT_HDR_AND_ID_LEN);
		if (words_rx != MXM_RPT_HDR_AND_ID_LEN ||
			BYTEL(buf[0]) > MXM_RPT_LEN_PACKET_MAX) {
			ret = -EIO;
			dev_err(dev, "Report RX fail: received (%d) " \
					"expected (%d) words, " \
					"header (%04X)",
					words_rx, words, buf[0]);
			return ret;
		}

		if ((LO_NIBBLE(BYTEH(buf[0])) == MXM_RPT_FIRST_PACKET)
			&& buf[1] == MXM_RPT_ID_TOUCH_RAW_IMAGE)
			ts->is_raw_mode = true;

		words = BYTEL(buf[0]) + MXM_RPT_PKT_HDR_LEN;

		words_tx = i2c_tx_words(ts, &address, 1);
		if (words_tx != 1) {
			dev_err(dev, "Report RX fail:" \
				"failed to set address");
			return -EIO;
		}

		words_rx = i2c_rx_words(ts, &buf[offset], words);
		if (words_rx != words) {
			dev_err(dev, "Report RX fail 0x%X: " \
				"received (%d) expected (%d) words",
				address, words_rx, remainder);
			return -EIO;
		}
	} else {
		words_rx = i2c_rx_words(ts, buf, (u16) ts->pdata->i2c_words);
		if (words_rx != (u16) ts->pdata->i2c_words ||
			BYTEL(buf[0]) > MXM_RPT_LEN_PACKET_MAX) {
			ret = -EIO;
			dev_err(dev, "Report RX fail: received (%d) " \
				"expected (%d) words, header (%04X)",
				words_rx, words, buf[0]);
			return ret;
		}

		if ((LO_NIBBLE(BYTEH(buf[0])) == MXM_RPT_FIRST_PACKET)
			&& buf[1] != MXM_RPT_ID_TOUCH_RAW_IMAGE)
			ts->is_raw_mode = false;

		words = BYTEL(buf[0]) + MXM_RPT_PKT_HDR_LEN;
		remainder = words;

		if (remainder - (u16) ts->pdata->i2c_words > 0) {
			remainder -= (u16) ts->pdata->i2c_words;
			offset += (u16) ts->pdata->i2c_words;
			address += (u16) ts->pdata->i2c_words;
		}

		words_tx = i2c_tx_words(ts, &address, 1);
		if (words_tx != 1) {
			dev_err(dev, "Report RX fail: failed to set " \
				"address 0x%X", address);
			return -EIO;
		}

		words_rx = i2c_rx_words(ts, &buf[offset], remainder);
		if (words_rx != remainder) {
			dev_err(dev, "Report RX fail 0x%X: " \
				"received (%d) expected (%d) words",
					address, words_rx, remainder);
			return -EIO;
		}
	}

	return ret;
}

/* Send command */
static int send_mtp_command(struct data *ts, u16 *buf, u16 len)
{
	u16 tx_buf[MXM_CMD_LEN_PACKET_MAX + MXM_CMD_ADD_AND_HDR_LEN];
	u16 packets, last_packet, words_tx;
	struct max1187x_packet_header pkt;
	int i, ret = 0;
	u16 cmd_data_size = buf[1];

	/* check basics */
	if (len < MXM_CMD_LEN_PACKET_MIN || len > MXM_CMD_LEN_MAX ||
			(cmd_data_size + MXM_CMD_ID_AND_SIZE_LEN) != len) {
		dev_err(&ts->client->dev, "Command length is not valid");
		ret = -EINVAL;
		goto err_send_mtp_command;
	}

	/* packetize and send */
	packets = len / MXM_CMD_LEN_PACKET_MAX;
	if (len % MXM_CMD_LEN_PACKET_MAX)
		packets++;
	last_packet = packets - 1;
	pkt.total_num = packets << 12;
	tx_buf[0] = MXM_CMD_START_ADDR;

	for (i = 0; i < packets; i++) {
		pkt.curr_num = (i + 1) << 8;
		pkt.curr_size = (i == (last_packet)) ?
					len : MXM_CMD_LEN_PACKET_MAX;
		tx_buf[1] = pkt.total_num | pkt.curr_num | pkt.curr_size;
		memcpy(&tx_buf[2], &buf[i * MXM_CMD_LEN_PACKET_MAX],
						BYTE_SIZE(pkt.curr_size));
		words_tx = i2c_tx_words(ts, tx_buf,
				pkt.curr_size + MXM_CMD_ADD_AND_HDR_LEN);
		if (words_tx != (pkt.curr_size + MXM_CMD_ADD_AND_HDR_LEN)) {
			dev_err(&ts->client->dev, "Command TX fail: " \
			"transmitted (%d) expected (%d) words, packet (%d)",
			words_tx, pkt.curr_size + MXM_CMD_ADD_AND_HDR_LEN, i);
			ret = -EIO;
			goto err_send_mtp_command;
		}
		len -= MXM_CMD_LEN_PACKET_MAX;
	}

err_send_mtp_command:
	return ret;
}

/* Integer math operations */
u16 max1187x_sqrt(u32 num)
{
	u16 mask = 0x8000;
	u16 guess = 0;
	u32 prod = 0;

	if (num < MXM_PRESSURE_Z_MIN_TO_SQRT)
		return num;

	while (mask) {
		guess = guess ^ mask;
		prod = guess * guess;
		if (num < prod)
			guess = guess ^ mask;
		mask = mask>>1;
	}
	if (guess != 0xFFFF) {
		prod = guess * guess;
		if ((num - prod) > (prod + 2 * guess + 1 - num))
			guess++;
	}

	return guess;
}

static void report_buttons(struct data *ts,
			   struct max1187x_touch_report_header *header)
{
	struct max1187x_pdata *pdata = ts->pdata;

	if (!ts->input_dev->users)
		return;

	if (header->button0 != ts->button0) {
		input_report_key(ts->input_dev, pdata->button_code0,
				header->button0);
		input_sync(ts->input_dev);
		ts->button0 = header->button0;
	}
	if (header->button1 != ts->button1) {
		input_report_key(ts->input_dev, pdata->button_code1,
				header->button1);
		input_sync(ts->input_dev);
		ts->button1 = header->button1;
	}
	if (header->button2 != ts->button2) {
		input_report_key(ts->input_dev, pdata->button_code2,
				header->button2);
		input_sync(ts->input_dev);
		ts->button2 = header->button2;
	}
	if (header->button3 != ts->button3) {
		input_report_key(ts->input_dev, pdata->button_code3,
				header->button3);
		input_sync(ts->input_dev);
		ts->button3 = header->button3;
	}
}

static void report_down(struct data *ts,
			struct max1187x_touch_report_extended *e)
{
	struct max1187x_pdata *pdata = ts->pdata;
	struct device *dev = &ts->client->dev;
	struct input_dev *idev;
	u32 xcell = pdata->lcd_x / pdata->num_sensor_x;
	u32 ycell = pdata->lcd_y / pdata->num_sensor_y;
	u16 x = e->x;
	u16 y = e->y;
	u16 z = e->z;
	u16 tool_type = e->tool_type;
	u16 id = e->finger_id;
	u16 idbit = 1 << id;
	s8 xpixel = e->xpixel;
	s8 ypixel = e->ypixel;
	u32 touch_major, touch_minor;
	s16 xsize, ysize, orientation;
	bool valid;

	if (pdata->coordinate_settings & MXM_SWAP_XY) {
		swap(x, y);
		swap(xpixel, ypixel);
	}
	if (pdata->coordinate_settings & MXM_REVERSE_X) {
		x = pdata->panel_margin_xl + pdata->lcd_x
			+ pdata->panel_margin_xh - 1 - x;
		xpixel = -xpixel;
	}
	if (pdata->coordinate_settings & MXM_REVERSE_Y) {
		y = pdata->panel_margin_yl + pdata->lcd_y
			+ pdata->panel_margin_yh - 1 - y;
		ypixel = -ypixel;
	}
	if (tool_type == 1) {
		idev = ts->input_pen;
		tool_type = MT_TOOL_PEN;
		ts->curr_tool_ids |= idbit;
		ts->used_tools = (1 << MT_TOOL_PEN);
	} else {
		idev = ts->input_dev;
		tool_type = MT_TOOL_FINGER;
		ts->curr_tool_ids &= ~idbit;
		ts->used_tools = (1 << MT_TOOL_FINGER);
	}
	valid = idev->users > 0;
	ts->curr_finger_ids |= idbit;
	z = (MXM_PRESSURE_SQRT_MAX >> 2) + max1187x_sqrt(z);
	if (z > MXM_PRESSURE_SQRT_MAX)
		z = MXM_PRESSURE_SQRT_MAX;
	xsize = xpixel * (s16)xcell;
	ysize = ypixel * (s16)ycell;
	if (xsize < 0)
		xsize = -xsize;
	if (ysize < 0)
		ysize = -ysize;
	orientation = (xsize > ysize) ? 0 : 90;
	touch_major = (xsize > ysize) ? xsize : ysize;
	touch_minor = (xsize > ysize) ? ysize : xsize;

	if (valid) {
		input_mt_slot(idev, id);
		input_mt_report_slot_state(idev, tool_type, true);
		input_report_abs(idev, ABS_MT_POSITION_X, x);
		input_report_abs(idev, ABS_MT_POSITION_Y, y);
		if (pdata->pressure_enabled)
			input_report_abs(idev, ABS_MT_PRESSURE, z);
		if (pdata->orientation_enabled)
			input_report_abs(idev, ABS_MT_ORIENTATION, orientation);
		if (pdata->size_enabled) {
			input_report_abs(idev, ABS_MT_TOUCH_MAJOR, touch_major);
			input_report_abs(idev, ABS_MT_TOUCH_MINOR, touch_minor);
		}
	}
	dev_dbg(dev, "event: %s%s%s %u: [XY %4d %4d ][PMmO %4d %4d %4d %3d ]",
		!(ts->list_finger_ids & (1 << id)) ? "DOWN" : "MOVE",
		valid ? " " : "#",
		tool_type == 0 ? "Finger" :
		tool_type == 1 ? "Stylus" : "*Unknown*",
		id, x, y, z, touch_major, touch_minor, orientation);
}

static void report_up(struct data *ts, int id)
{
	struct device *dev = &ts->client->dev;
	struct input_dev *idev;
	u16 tool_type;
	u16 idbit = 1 << id;
	bool valid;

	if (!(ts->list_finger_ids & idbit))
		return;

	if (ts->list_tool_ids & idbit) {
		idev = ts->input_pen;
		tool_type = MT_TOOL_PEN;
		ts->used_tools = (1 << MT_TOOL_PEN);
	} else {
		idev = ts->input_dev;
		tool_type = MT_TOOL_FINGER;
		ts->used_tools = (1 << MT_TOOL_FINGER);
	}
	valid = idev->users > 0;
	if (valid) {
		input_mt_slot(idev, id);
		input_mt_report_slot_state(idev, tool_type, false);
	}
	dev_dbg(dev, "event: UP%s%s %u\n",
		valid ? " " : "#",
		tool_type == 0 ? "Finger" :
		tool_type == 1 ? "Stylus" : "*Unknown*",
		id);
	ts->list_finger_ids &= ~idbit;
}

static void report_sync(struct data *ts)
{
	if (ts->input_dev->users && (ts->used_tools & (1 << MT_TOOL_FINGER)))
		input_sync(ts->input_dev);
	if (ts->input_pen->users && (ts->used_tools & (1 << MT_TOOL_PEN)))
		input_sync(ts->input_pen);
}

static void invalidate_all_fingers(struct data *ts)
{
	struct device *dev = &ts->client->dev;
	u32 i;

	dev_dbg(dev, "event: UP all\n");
	ts->used_tools = 0;
	if (ts->input_dev->users) {
		for (i = 0; i < MXM_TOUCH_COUNT_MAX; i++) {
			input_mt_slot(ts->input_dev, i);
			input_mt_report_slot_state(ts->input_dev,
						   MT_TOOL_FINGER, false);
		}
		input_sync(ts->input_dev);
	}
	if (ts->input_pen->users) {
		for (i = 0; i < MXM_TOUCH_COUNT_MAX; i++) {
			input_mt_slot(ts->input_pen, i);
			input_mt_report_slot_state(ts->input_pen,
						   MT_TOOL_PEN, false);
		}
		input_sync(ts->input_pen);
	}
	ts->list_finger_ids = 0;
	ts->list_tool_ids = 0;
	ts->used_tools = 0;
}

#ifdef MXM_TOUCH_WAKEUP_FEATURE
static void report_wakeup_gesture(struct data *ts,
				  struct max1187x_touch_report_header *header)
{
	struct device *dev = &ts->client->dev;
	
	dev_dbg(dev, "event: Received dt2w wakeup report\n");
	if (ts->input_dev_key->users) {
		input_report_key(ts->input_dev_key, KEY_POWER, 1);
		input_sync(ts->input_dev_key);
		input_report_key(ts->input_dev_key, KEY_POWER, 0);
		input_sync(ts->input_dev_key);
		
	}
}
#endif

static void process_report(struct data *ts, u16 *buf)
{
	u32 i;
	struct device *dev = &ts->client->dev;
	struct max1187x_touch_report_header *header;
	struct max1187x_touch_report_extended *reporte;

	header = (struct max1187x_touch_report_header *) buf;

	if (BYTEH(header->header) != MXM_ONE_PACKET_RPT)
		goto end;

#ifdef MXM_TOUCH_WAKEUP_FEATURE
	if (dt2w_active == 1  && ts->is_suspended && header->touch_count >= DT2W_COUNT) {
//	  input_mt_slot(ts->input_dev, i);
	  report_wakeup_gesture(ts, header);
	  
// 		if(header->touch_count >= DT2W_COUNT) {
// 			report_wakeup_gesture(ts, header);
// 			goto end;
// 		}
// 		if (header->touch_count == 1 && dt2w_tap_time > 0 ) {
// 			if ((ktime_to_ms(ktime_get()) - dt2w_tap_time) < dt2w_time) {
// 				touch_no = 0;
// 				report_wakeup_gesture(ts, header);
// 				goto end;
// 			}
// 			dt2w_tap_time = ktime_to_ms(ktime_get());
// 			goto end;
// 		}
// 		if (header->touch_count == 1 && dt2w_tap_time == 0) {	
// 			dt2w_tap_time = ktime_to_ms(ktime_get());
// 			goto end;
// 		}
// 	
 		goto end;
 	}
#endif
	if (header->report_id != MXM_RPT_ID_EXT_TOUCH_INFO)
		goto end;

	if (ts->framecounter == header->framecounter) {
		dev_err(dev, "Same framecounter (%u) encountered  " \
			"at irq (%u)!\n", ts->framecounter, ts->irq_count);
		goto end;
	}
	ts->framecounter = header->framecounter;

	report_buttons(ts, header);

	if (header->touch_count > MXM_TOUCH_COUNT_MAX) {
		dev_err(dev, "Touch count (%u) out of bounds [0,10]!",
				header->touch_count);
		goto end;
	} else if (!header->touch_count) {
		invalidate_all_fingers(ts);
		goto end;
	}

	ts->curr_finger_ids = 0;
	ts->used_tools = 0;
	reporte = (struct max1187x_touch_report_extended *)
		((u8 *)buf + sizeof(*header));
	for (i = 0; i < header->touch_count; i++, reporte++)
		report_down(ts, reporte);
	for (i = 0; i < MXM_TOUCH_COUNT_MAX; i++) {
		if (!(ts->curr_finger_ids & (1 << i)))
			report_up(ts, i);
	}
	report_sync(ts);
	ts->list_finger_ids = ts->curr_finger_ids;
	ts->list_tool_ids = ts->curr_tool_ids;
end:
	return;
}

static irqreturn_t irq_handler_soft(int irq, void *context)
{
	struct data *ts = (struct data *) context;
	int ret;

	dev_dbg(&ts->client->dev, "%s: Enter\n", __func__);

	mutex_lock(&ts->i2c_mutex);

	ret = read_mtp_report(ts, ts->rx_packet);
	if (!ret) {
		process_report(ts, ts->rx_packet);
		propagate_report(ts, 0, ts->rx_packet);
	} else {
		reset_power(ts);
	}

	mutex_unlock(&ts->i2c_mutex);
	dev_dbg(&ts->client->dev, "%s: Exit\n", __func__);
	return IRQ_HANDLED;
}

static irqreturn_t irq_handler_hard(int irq, void *context)
{
	struct data *ts = (struct data *) context;

	dev_dbg(&ts->client->dev, "%s: Enter\n", __func__);

	if (gpio_get_value(ts->pdata->gpio_tirq))
		goto irq_handler_hard_complete;

	ts->irq_count++;

	dev_dbg(&ts->client->dev, "%s: Exit\n", __func__);
	return IRQ_WAKE_THREAD;

irq_handler_hard_complete:
	dev_dbg(&ts->client->dev, "%s: Exit\n", __func__);
	return IRQ_HANDLED;
}

static ssize_t i2c_reset_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);
	int ret;

	ret = bootloader_exit(ts);
	if (ret) {
		dev_err(dev, "Failed to do i2c reset.");
		goto exit;
	}
	dev_info(dev, "i2c reset occured\n");
exit:
	return count;
}

static int reset_power(struct data *ts)
{
	int ret;

	invalidate_all_fingers(ts);

	ret = vreg_configure(ts, false);
	if (ret)
		goto exit;
	usleep_range(MXM_WAIT_MIN_US, MXM_WAIT_MAX_US);
	if (ts->pdata->gpio_pwr_en) {
		ret = gpio_direction_output(ts->pdata->gpio_pwr_en, 1);
		if (ret)
			goto exit;
	}
	msleep(MXM_PWR_RESET_WAIT_MS);
	if (ts->pdata->gpio_pwr_en) {
		ret = gpio_direction_output(ts->pdata->gpio_pwr_en, 0);
		if (ret)
			goto exit;
	}
	usleep_range(MXM_WAIT_MIN_US, MXM_WAIT_MAX_US);
	ret = vreg_configure(ts, true);
	if (ret)
		goto exit;

	msleep(MXM_PWR_RESET_WAIT_MS);

	dev_dbg(&ts->client->dev, "power on reset\n");
exit:
	if (ret)
		dev_err(&ts->client->dev, "Failed to power on reset\n");
	return ret;
}

static ssize_t power_on_reset_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&ts->i2c_mutex);
	ret = reset_power(ts);
	mutex_unlock(&ts->i2c_mutex);
	if (ret)
		goto exit;

	dev_info(dev, "hw reset occured\n");
exit:
	return count;
}

static ssize_t sreset_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);
	u16 cmd_buf[] = {MXM_CMD_ID_RESET_SYSTEM, MXM_ZERO_SIZE_CMD};
	int ret;

	ret = rbcmd_send_receive(ts, cmd_buf, 2, MXM_RPT_ID_SYS_STATUS, 3 * HZ);
	if (ret)
		dev_err(dev, "Failed to do soft reset.");
	return count;
}

static ssize_t fw_update_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);
	int ret;

	mutex_lock(&ts->fw_mutex);
	if (sysfs_streq(buf, fw_update_mode[MXM_FW_UPDATE_DEFAULT])) {
		ts->fw_update_mode = MXM_FW_UPDATE_DEFAULT;
	} else if (sysfs_streq(buf, fw_update_mode[MXM_FW_UPDATE_FORCE])) {
		ts->fw_update_mode = MXM_FW_UPDATE_FORCE;
	} else {
		dev_err(dev, "Invalid argument: %s\n", buf);
		ret = -EINVAL;
		goto end;

	}
	dev_info(dev, "firmware update (%s)\n",
			fw_update_mode[ts->fw_update_mode]);
	validate_fw(ts);
	ret = count;

end:
	ts->fw_update_mode = MXM_FW_UPDATE_DEFAULT;
	mutex_unlock(&ts->fw_mutex);
	return ret;
}

static ssize_t irq_count_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);

	return snprintf(buf, PAGE_SIZE, "%u\n", ts->irq_count);
}

static ssize_t irq_count_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);

	ts->irq_count = 0;
	return count;
}

static ssize_t dflt_cfg_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);

	return snprintf(buf, PAGE_SIZE, "%u 0x%x 0x%x\n",
			ts->pdata->defaults_allow,
			ts->pdata->default_config_id,
			ts->pdata->default_chip_id);
}

static ssize_t dflt_cfg_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);

	(void) sscanf(buf, "%u 0x%x 0x%x", &ts->pdata->defaults_allow,
		&ts->pdata->default_config_id, &ts->pdata->default_chip_id);
	return count;
}

static ssize_t panel_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);

	return snprintf(buf, PAGE_SIZE, "%u %u %u %u %u %u\n",
			ts->pdata->panel_margin_xl, ts->pdata->panel_margin_xh,
			ts->pdata->panel_margin_yl, ts->pdata->panel_margin_yh,
			ts->pdata->lcd_x, ts->pdata->lcd_y);
}

static ssize_t panel_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);

	(void) sscanf(buf, "%u %u %u %u %u %u", &ts->pdata->panel_margin_xl,
		&ts->pdata->panel_margin_xh, &ts->pdata->panel_margin_yl,
		&ts->pdata->panel_margin_yh, &ts->pdata->lcd_x,
		&ts->pdata->lcd_y);
	return count;
}

static ssize_t fw_ver_show(struct device *dev, struct device_attribute *attr,
	char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);

	int ret, count = 0;
	u16 cmd_buf[2];

	/* Read firmware version */
	cmd_buf[0] = MXM_CMD_ID_GET_FW_VERSION;
	cmd_buf[1] = MXM_ZERO_SIZE_CMD;

	ret = rbcmd_send_receive(ts, cmd_buf, 2, MXM_RPT_ID_FW_VERSION, HZ/4);

	if (ret)
		goto err_fw_ver_show;

	ts->chip_id = BYTEH(ts->rbcmd_rx_report[4]);
	count += snprintf(buf, PAGE_SIZE, "fw_ver (%u.%u.%u) " \
					"chip_id (0x%02X)\n",
					BYTEH(ts->rbcmd_rx_report[3]),
					BYTEL(ts->rbcmd_rx_report[3]),
					ts->rbcmd_rx_report[5],
					ts->chip_id);

	/* Read touch configuration */
	cmd_buf[0] = MXM_CMD_ID_GET_CFG_INF;
	cmd_buf[1] = MXM_ZERO_SIZE_CMD;

	ret = rbcmd_send_receive(ts, cmd_buf, 2, MXM_RPT_ID_CFG_INF, HZ/4);

	if (ret) {
		dev_err(dev, "Failed to receive chip config\n");
		goto err_fw_ver_show;
	}

	ts->config_id = ts->rbcmd_rx_report[3];

	count += snprintf(buf + count, PAGE_SIZE, "config_id (0x%04X) ",
					ts->config_id);
	count += snprintf(buf + count, PAGE_SIZE,
			"customer_info[1:0] (0x%04X, 0x%04X)\n",
					ts->rbcmd_rx_report[43],
					ts->rbcmd_rx_report[42]);
err_fw_ver_show:
	return count;
}

static ssize_t chip_id_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);

	return snprintf(buf, PAGE_SIZE, "0x%02X\n", ts->chip_id);
}

static ssize_t config_id_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);

	return snprintf(buf, PAGE_SIZE, "0x%04X\n", ts->config_id);
}

static ssize_t driver_ver_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "3.1.8: May 10, 2013\n");
}

static ssize_t command_store(struct device *dev, struct device_attribute *attr,
	const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);
	u16 buffer[MXM_CMD_LEN_MAX];
	char scan_buf[5];
	int i, ret;

	count--; /* ignore carriage return */
	if (count % 4) {
		dev_err(dev, "words not properly defined");
		return -EINVAL;
	}
	scan_buf[4] = '\0';
	for (i = 0; i < count; i += 4) {
		memcpy(scan_buf, &buf[i], 4);
		if (sscanf(scan_buf, "%4hx", &buffer[i / 4]) != 1) {
			dev_err(dev, "bad word (%s)", scan_buf);
			return -EINVAL;
		}
	}
	ret = cmd_send(ts, buffer, count / 4);
	if (ret)
		dev_err(dev, "MTP command failed");
	return ++count;
}

static ssize_t screen_status_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);

	dev_info(&ts->client->dev, "%s: screen_status = %d\n", __func__,
				ts->screen_status);
	return snprintf(buf, PAGE_SIZE, "%d\n", ts->screen_status);
}

static ssize_t screen_status_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);

	if (sscanf(buf, "%d", &ts->screen_status) != 1) {
		dev_err(dev, "bad value (%s)", buf);
		return -EINVAL;
	}
	dev_dbg(&ts->client->dev, "%s: screen_status = %d\n", __func__,
				ts->screen_status);

	if (ts->screen_status) {
		if (ts->is_suspended)
			set_resume_mode(ts);
	} else {
		if (!ts->is_suspended)
			set_suspend_mode(ts);
	}

	return count;
}

static ssize_t report_read(struct file *file, struct kobject *kobj,
	struct bin_attribute *attr, char *buf, loff_t off, size_t count)
{
	struct i2c_client *client = kobj_to_i2c_client(kobj);
	struct data *ts = i2c_get_clientdata(client);
	int printed, i, offset = 0, payload;
	int full_packet;
	int num_term_char;

	if (get_report(ts, 0xFFFF, 0xFFFFFFFF))
		return 0;

	payload = ts->rx_report_len;
	full_packet = payload;
	num_term_char = 2; /* number of term char */
	if (count < (4 * full_packet + num_term_char))
		return -EIO;
	if (count > (4 * full_packet + num_term_char))
		count = 4 * full_packet + num_term_char;

	for (i = 1; i <= payload; i++) {
		printed = snprintf(&buf[offset], PAGE_SIZE, "%04X\n",
			ts->rx_report[i]);
		if (printed <= 0)
			return -EIO;
		offset += printed - 1;
	}
	snprintf(&buf[offset], PAGE_SIZE, ",\n");
	release_report(ts);

	return count;
}

static struct device_attribute dev_attrs[] = {
	__ATTR(i2c_reset, S_IWUSR, NULL, i2c_reset_store),
	__ATTR(por, S_IWUSR, NULL, power_on_reset_store),
	__ATTR(sreset, S_IWUSR, NULL, sreset_store),
	__ATTR(fw_update, S_IWUSR, NULL, fw_update_store),
	__ATTR(irq_count, S_IRUGO | S_IWUSR, irq_count_show,
		irq_count_store),
	__ATTR(dflt_cfg, S_IRUGO | S_IWUSR, dflt_cfg_show, dflt_cfg_store),
	__ATTR(panel, S_IRUGO | S_IWUSR, panel_show, panel_store),
	__ATTR(fw_ver, S_IRUGO, fw_ver_show, NULL),
	__ATTR(chip_id, S_IRUGO, chip_id_show, NULL),
	__ATTR(config_id, S_IRUGO, config_id_show, NULL),
	__ATTR(driver_ver, S_IRUGO, driver_ver_show, NULL),
	__ATTR(command, S_IWUSR, NULL, command_store),
	__ATTR(screen_status, S_IRUGO | S_IWUSR, screen_status_show,
						screen_status_store)
};

static struct bin_attribute dev_attr_report = {
		.attr = {.name = "report", .mode = S_IRUGO},
		.read = report_read };

static int create_sysfs_entries(struct data *ts)
{
	int i, ret = 0;

	for (i = 0; i < ARRAY_SIZE(dev_attrs); i++) {
		ret = device_create_file(&ts->client->dev, &dev_attrs[i]);
		if (ret) {
			for (; i >= 0; --i) {
				device_remove_file(&ts->client->dev,
							&dev_attrs[i]);
				ts->sysfs_created--;
			}
			break;
		}
		ts->sysfs_created++;
	}
	return ret;
}

static void remove_sysfs_entries(struct data *ts)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dev_attrs); i++)
		if (ts->sysfs_created && ts->sysfs_created--)
			device_remove_file(&ts->client->dev, &dev_attrs[i]);
}

/* Send command to chip */
static int cmd_send(struct data *ts, u16 *buf, u16 len)
{
	int ret;

	memcpy(ts->cmd_buf, buf, len * sizeof(buf[0]));
	ts->cmd_len = len;

	mutex_lock(&ts->i2c_mutex);
	ret = send_mtp_command(ts, ts->cmd_buf, ts->cmd_len);
	mutex_unlock(&ts->i2c_mutex);

	if (ret)
		dev_err(&ts->client->dev, "Failed to send command (ret=%d)\n",
									ret);

	return ret;
}

/*
 * Send command to chip and expect a report with
 * id == report_id within timeout time.
 * timeout is measured in jiffies. 1s = HZ jiffies
 */
static int rbcmd_send_receive(struct data *ts, u16 *buf,
		u16 len, u16 report_id, u16 timeout)
{
	int ret;

	ret = down_interruptible(&ts->sema_rbcmd);
	if (ret)
		goto err_rbcmd_send_receive_sema_rbcmd;

	ts->rbcmd_report_id = report_id;
	ts->rbcmd_received = 0;
	ts->rbcmd_waiting = 1;

	ret = cmd_send(ts, buf, len);
	if (ret)
		goto err_rbcmd_send_receive_cmd_send;

	ret = wait_event_interruptible_timeout(ts->waitqueue_rbcmd,
			ts->rbcmd_received != 0, timeout);
	if (ret < 0 || !ts->rbcmd_received)
		goto err_rbcmd_send_receive_timeout;

	ts->rbcmd_waiting = 0;
	up(&ts->sema_rbcmd);

	return 0;

err_rbcmd_send_receive_timeout:
err_rbcmd_send_receive_cmd_send:
	ts->rbcmd_waiting = 0;
	up(&ts->sema_rbcmd);
err_rbcmd_send_receive_sema_rbcmd:
	return -ERESTARTSYS;
}

static int read_chip_data(struct data *ts)
{
	int ret;
	u16 loopcounter;
	u16 cmd_buf[2];

	/* Read firmware version */
	cmd_buf[0] = MXM_CMD_ID_GET_FW_VERSION;
	cmd_buf[1] = MXM_ZERO_SIZE_CMD;

	loopcounter = 0;
	ret = -1;
	while (loopcounter < MXM_FW_RETRIES_MAX && ret) {
		ret = rbcmd_send_receive(ts, cmd_buf, 2,
				MXM_RPT_ID_FW_VERSION, HZ/4);
		loopcounter++;
	}

	if (ret) {
		dev_err(&ts->client->dev, "Failed to receive fw version\n");
		goto err_read_chip_data;
	}

	ts->chip_id = BYTEH(ts->rbcmd_rx_report[4]);
	dev_info(&ts->client->dev, "(INIT): fw_ver (%u.%u.%u) " \
					"chip_id (0x%02X)\n",
					BYTEH(ts->rbcmd_rx_report[3]),
					BYTEL(ts->rbcmd_rx_report[3]),
					ts->rbcmd_rx_report[5],
					ts->chip_id);

	/* Read touch configuration */
	cmd_buf[0] = MXM_CMD_ID_GET_CFG_INF;
	cmd_buf[1] = MXM_ZERO_SIZE_CMD;

	loopcounter = 0;
	ret = -1;
	while (loopcounter < MXM_FW_RETRIES_MAX && ret) {
		ret = rbcmd_send_receive(ts, cmd_buf, 2,
				MXM_RPT_ID_CFG_INF, HZ/4);
		loopcounter++;
	}

	if (ret) {
		dev_err(&ts->client->dev, "Failed to receive chip config\n");
		goto err_read_chip_data;
	}

	ts->config_id = ts->rbcmd_rx_report[3];

	dev_info(&ts->client->dev, "(INIT): config_id (0x%04X)\n",
					ts->config_id);

	return 0;

err_read_chip_data:
	return ret;
}

static int device_fw_load(struct data *ts, const struct firmware *fw,
	u16 fw_index)
{
	struct device *dev = &ts->client->dev;
	u16 filesize, file_codesize, loopcounter;
	u16 file_crc16_1, file_crc16_2, local_crc16;
	int chip_crc16_1 = -1, chip_crc16_2 = -1, ret;

	filesize = ts->pdata->fw_mapping[fw_index].filesize;
	file_codesize = ts->pdata->fw_mapping[fw_index].file_codesize;

	if (fw->size != filesize) {
		dev_err(dev, "filesize (%d) is not equal to expected size (%d)",
				fw->size, filesize);
		return -EIO;
	}

	file_crc16_1 = crc16(0, fw->data, file_codesize);

	loopcounter = 0;
	do {
		ret = bootloader_enter(ts);
		if (!ret)
			ret = bootloader_get_crc(ts, &local_crc16,
				0, file_codesize, MXM_BL_WR_DELAY_MS);
		if (!ret)
			chip_crc16_1 = local_crc16;
		ret = bootloader_exit(ts);
		loopcounter++;
	} while (loopcounter < MXM_FW_RETRIES_MAX && chip_crc16_1 == -1);

	dev_info(dev, "(INIT): file_crc16_1 = 0x%04x, chip_crc16_1 = 0x%04x\n",
			file_crc16_1, chip_crc16_1);

	if (ts->fw_update_mode == MXM_FW_UPDATE_FORCE ||
	    file_crc16_1 != chip_crc16_1) {
		loopcounter = 0;
		file_crc16_2 = crc16(0, fw->data, filesize);

		while (loopcounter < MXM_FW_RETRIES_MAX && file_crc16_2
				!= chip_crc16_2) {
			dev_info(dev, "(INIT): Reprogramming chip." \
					"Attempt %d", loopcounter+1);
			ret = bootloader_enter(ts);
			if (!ret)
				ret = bootloader_erase_flash(ts);
			if (!ret)
				ret = bootloader_set_byte_mode(ts);
			if (!ret)
				ret = bootloader_write_flash(ts, fw->data,
					filesize);
			if (!ret)
				ret = bootloader_get_crc(ts, &local_crc16,
					0, filesize, MXM_BL_WR_DELAY_MS);
			if (!ret)
				chip_crc16_2 = local_crc16;
			dev_info(dev, "(INIT): file_crc16_2 = 0x%04x, " \
					"chip_crc16_2 = 0x%04x\n",
					file_crc16_2, chip_crc16_2);
			ret = bootloader_exit(ts);
			loopcounter++;
		}

		if (file_crc16_2 != chip_crc16_2)
			return -EAGAIN;
	}

	loopcounter = 0;
	do {
		ret = bootloader_exit(ts);
		loopcounter++;
	} while (loopcounter < MXM_FW_RETRIES_MAX && ret);

	if (ret)
		return -EIO;

	return 0;
}

static void validate_fw(struct data *ts)
{
	struct device *dev = &ts->client->dev;
	const struct firmware *fw;
	u16 config_id, chip_id;
	int i, ret;
	u16 cmd_buf[3];

	if (ts->fw_update_mode == MXM_FW_UPDATE_FORCE) {
		ts->chip_id = 0;
		ts->config_id = 0;
		goto set_id;
	}

	ret = read_chip_data(ts);
	if (ret && !ts->pdata->defaults_allow) {
		dev_err(dev, "Firmware is not responsive " \
				"and default update is disabled\n");
		return;
	}

set_id:
	if (ts->chip_id)
		chip_id = ts->chip_id;
	else
		chip_id = ts->pdata->default_chip_id;

	if (ts->config_id)
		config_id = ts->config_id;
	else
		config_id = ts->pdata->default_config_id;

	for (i = 0; i < ts->pdata->num_fw_mappings; i++) {
		if (ts->pdata->fw_mapping[i].config_id == config_id &&
			ts->pdata->fw_mapping[i].chip_id == chip_id)
			break;
	}

	if (i == ts->pdata->num_fw_mappings) {
		dev_err(dev, "FW not found for configID(0x%04X) " \
				"and chipID(0x%04X)", config_id, chip_id);
		return;
	}

	dev_info(dev, "(INIT): Firmware file (%s)",
		ts->pdata->fw_mapping[i].filename);

	ret = request_firmware(&fw, ts->pdata->fw_mapping[i].filename,
					&ts->client->dev);

	if (ret || fw == NULL) {
		dev_err(dev, "firmware request failed (ret = %d, fwptr = %p)",
			ret, fw);
		return;
	}

	mutex_lock(&ts->i2c_mutex);
	disable_irq(ts->client->irq);
	if (device_fw_load(ts, fw, i)) {
		release_firmware(fw);
		dev_err(dev, "firmware download failed");
		enable_irq(ts->client->irq);
		mutex_unlock(&ts->i2c_mutex);
		return;
	}

	release_firmware(fw);
	dev_info(dev, "(INIT): firmware okay\n");
	enable_irq(ts->client->irq);
	mutex_unlock(&ts->i2c_mutex);
	ret = read_chip_data(ts);

	cmd_buf[0] = MXM_CMD_ID_SET_TOUCH_RPT_MODE;
	cmd_buf[1] = MXM_ONE_SIZE_CMD;
	cmd_buf[2] = MXM_TOUCH_REPORT_MODE_EXT;
	ret = cmd_send(ts, cmd_buf, 3);
	if (ret) {
		dev_err(dev, "Failed to set up touch report mode");
		return;
	}
}

static int regulator_handler(struct regulator *regulator,
				struct device *dev,
				const char *func_str,
				const char *reg_str,
				int sw)
{
	int rc, enabled;

	if (IS_ERR_OR_NULL(regulator)) {
		rc = regulator ? PTR_ERR(regulator) : -EINVAL;
		dev_err(dev, "%s: regulator '%s' invalid",
			func_str ? func_str : "?",
			reg_str ? reg_str : "?");
		goto exit;
	}

	if (sw)
		rc = regulator_enable(regulator);
	else
		rc = regulator_disable(regulator);
	if (rc) {
		enabled = regulator_is_enabled(regulator);
		dev_warn(dev,
			"%s: regulator '%s' status is %d",
			func_str ? func_str : "?",
			reg_str ? reg_str : "?",
			enabled);
		if ((!enabled && !sw) || (enabled > 0 && sw))
			rc = 0;
	}
exit:
	return rc;
}

static int vreg_suspend(struct data *ts, bool enable)
{
	struct device *dev = &ts->client->dev;
	int rc = 0;

	if (IS_ERR(ts->vreg_touch_vdd)) {
		dev_err(dev, "vreg_touch_vdd is not initialized\n");
		rc = -ENODEV;
		goto exit;
	}

	if (enable)
		rc = regulator_set_optimum_mode(ts->vreg_touch_vdd,
							MXM_LPM_UA_LOAD);
	else
		rc = regulator_set_optimum_mode(ts->vreg_touch_vdd,
							MXM_HPM_UA_LOAD);

	if (rc < 0) {
		dev_err(dev, "%s: vdd: set mode (%s) failed, rc=%d\n",
				__func__, (enable ? "LPM" : "HPM"), rc);
		goto exit;
	} else {
		dev_dbg(dev, "%s: vdd: set mode (%s) ok, new mode=%d\n",
			__func__, (enable ? "LPM" : "HPM"), rc);
		rc = 0;
	}
exit:
	return rc;
}

static int vreg_configure(struct data *ts, bool enable)
{
	struct device *dev = &ts->client->dev;
	int rc = 0;

	if (enable) {
		ts->vreg_touch_vdd = regulator_get(dev,
						ts->pdata->vdd_supply_name);
		if (IS_ERR(ts->vreg_touch_vdd)) {
			dev_err(dev, "%s: get vdd failed\n", __func__);
			rc = -ENODEV;
			goto err_ret;
		}
		rc = regulator_set_voltage(ts->vreg_touch_vdd,
					MXM_VREG_MAX_UV, MXM_VREG_MAX_UV);
		if (rc) {
			dev_err(dev, "%s: set voltage vdd failed, rc=%d\n",
								__func__, rc);
			goto err_put_vdd;
		}
		rc = regulator_handler(ts->vreg_touch_vdd, dev,
				__func__, ts->pdata->vdd_supply_name, 1);
		if (rc)
			goto err_put_vdd;
		rc = vreg_suspend(ts, false);
		if (rc) {
			dev_err(dev, "%s: set vdd mode failed, rc=%d\n",
							__func__, rc);
			goto err_disable_vdd;
		}
		dev_dbg(dev, "%s: set touch_vdd ON\n", __func__);
	} else {
		if (!IS_ERR(ts->vreg_touch_vdd)) {
			rc = regulator_set_voltage(ts->vreg_touch_vdd,
					0, MXM_VREG_MAX_UV);
			if (rc)
				dev_err(dev, "%s: set voltage vdd failed, " \
						"rc=%d\n", __func__, rc);
			regulator_handler(ts->vreg_touch_vdd,
				dev, __func__, ts->pdata->vdd_supply_name, 0);
			regulator_put(ts->vreg_touch_vdd);
			dev_dbg(dev, "%s: set touch_vdd OFF\n", __func__);
		}
	}
	return rc;
err_disable_vdd:
	regulator_handler(ts->vreg_touch_vdd, dev, __func__,
					ts->pdata->vdd_supply_name, 0);
err_put_vdd:
	regulator_put(ts->vreg_touch_vdd);
err_ret:
	return rc;
}

/* #ifdef CONFIG_OF */
static struct max1187x_pdata *max1187x_get_platdata_dt(struct device *dev)
{
	struct max1187x_pdata *pdata = NULL;
	struct device_node *devnode = dev->of_node;
	u32 i;
	u32 datalist[MXM_NUM_FW_MAPPINGS_MAX];

	if (!devnode)
		return NULL;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		dev_err(dev, "Failed to allocate memory for pdata\n");
		return NULL;
	}

	/* Parse touch_vdd_supply_name */
	if (of_property_read_string(devnode, "touch_vdd-supply_name",
				(const char **)&pdata->vdd_supply_name)) {
		dev_err(dev, "Failed to get property: touch_vdd-supply_name\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse gpio_tirq */
	if (of_property_read_u32(devnode, "gpio_tirq", &pdata->gpio_tirq)) {
		dev_err(dev, "Failed to get property: gpio_tirq\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse gpio_pwr_en */
	if (of_property_read_u32(devnode, "gpio_pwr_en", &pdata->gpio_pwr_en))
		dev_err(dev, "Failed to get property: gpio_pwr_en\n");

	/* Parse num_fw_mappings */
	if (of_property_read_u32(devnode, "num_fw_mappings",
		&pdata->num_fw_mappings)) {
		dev_err(dev, "Failed to get property: num_fw_mappings\n");
		goto err_max1187x_get_platdata_dt;
	}

	if (pdata->num_fw_mappings > MXM_NUM_FW_MAPPINGS_MAX)
		pdata->num_fw_mappings = MXM_NUM_FW_MAPPINGS_MAX;

	/* Parse config_id */
	if (of_property_read_u32_array(devnode, "config_id", datalist,
			pdata->num_fw_mappings)) {
		dev_err(dev, "Failed to get property: config_id\n");
		goto err_max1187x_get_platdata_dt;
	}

	for (i = 0; i < pdata->num_fw_mappings; i++)
		pdata->fw_mapping[i].config_id = datalist[i];

	/* Parse chip_id */
	if (of_property_read_u32_array(devnode, "chip_id", datalist,
			pdata->num_fw_mappings)) {
		dev_err(dev, "Failed to get property: chip_id\n");
		goto err_max1187x_get_platdata_dt;
	}

	for (i = 0; i < pdata->num_fw_mappings; i++)
		pdata->fw_mapping[i].chip_id = datalist[i];

	/* Parse filename */
	for (i = 0; i < pdata->num_fw_mappings; i++) {
		if (of_property_read_string_index(devnode, "filename", i,
			(const char **) &pdata->fw_mapping[i].filename)) {
				dev_err(dev, "Failed to get property: "\
					"filename[%d]\n", i);
				goto err_max1187x_get_platdata_dt;
			}
	}

	/* Parse filesize */
	if (of_property_read_u32_array(devnode, "filesize", datalist,
		pdata->num_fw_mappings)) {
		dev_err(dev, "Failed to get property: filesize\n");
		goto err_max1187x_get_platdata_dt;
	}

	for (i = 0; i < pdata->num_fw_mappings; i++)
		pdata->fw_mapping[i].filesize = datalist[i];

	/* Parse file_codesize */
	if (of_property_read_u32_array(devnode, "file_codesize", datalist,
		pdata->num_fw_mappings)) {
		dev_err(dev, "Failed to get property: file_codesize\n");
		goto err_max1187x_get_platdata_dt;
	}

	for (i = 0; i < pdata->num_fw_mappings; i++)
		pdata->fw_mapping[i].file_codesize = datalist[i];

	/* Parse defaults_allow */
	if (of_property_read_u32(devnode, "defaults_allow",
		&pdata->defaults_allow)) {
		dev_err(dev, "Failed to get property: defaults_allow\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse default_config_id */
	if (of_property_read_u32(devnode, "default_config_id",
		&pdata->default_config_id)) {
		dev_err(dev, "Failed to get property: default_config_id\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse default_chip_id */
	if (of_property_read_u32(devnode, "default_chip_id",
		&pdata->default_chip_id)) {
		dev_err(dev, "Failed to get property: default_chip_id\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse i2c_words */
	if (of_property_read_u32(devnode, "i2c_words", &pdata->i2c_words)) {
		dev_err(dev, "Failed to get property: i2c_words\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse coordinate_settings */
	if (of_property_read_u32(devnode, "coordinate_settings",
		&pdata->coordinate_settings)) {
		dev_err(dev, "Failed to get property: coordinate_settings\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse panel_margin_xl */
	if (of_property_read_u32(devnode, "panel_margin_xl",
		&pdata->panel_margin_xl)) {
		dev_err(dev, "Failed to get property: panel_margin_xl\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse lcd_x */
	if (of_property_read_u32(devnode, "lcd_x", &pdata->lcd_x)) {
		dev_err(dev, "Failed to get property: lcd_x\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse panel_margin_xh */
	if (of_property_read_u32(devnode, "panel_margin_xh",
		&pdata->panel_margin_xh)) {
		dev_err(dev, "Failed to get property: panel_margin_xh\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse panel_margin_yl */
	if (of_property_read_u32(devnode, "panel_margin_yl",
		&pdata->panel_margin_yl)) {
		dev_err(dev, "Failed to get property: panel_margin_yl\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse lcd_y */
	if (of_property_read_u32(devnode, "lcd_y", &pdata->lcd_y)) {
		dev_err(dev, "Failed to get property: lcd_y\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse panel_margin_yh */
	if (of_property_read_u32(devnode, "panel_margin_yh",
		&pdata->panel_margin_yh)) {
		dev_err(dev, "Failed to get property: panel_margin_yh\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse row_count */
	if (of_property_read_u32(devnode, "num_sensor_x",
		&pdata->num_sensor_x)) {
		dev_err(dev, "Failed to get property: num_sensor_x\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse num_sensor_y */
	if (of_property_read_u32(devnode, "num_sensor_y",
		&pdata->num_sensor_y)) {
		dev_err(dev, "Failed to get property: num_sensor_y\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse button_code0 */
	if (of_property_read_u32(devnode, "button_code0",
		&pdata->button_code0)) {
		dev_err(dev, "Failed to get property: button_code0\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse button_code1 */
	if (of_property_read_u32(devnode, "button_code1",
		&pdata->button_code1)) {
		dev_err(dev, "Failed to get property: button_code1\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse button_code2 */
	if (of_property_read_u32(devnode, "button_code2",
		&pdata->button_code2)) {
		dev_err(dev, "Failed to get property: button_code2\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse touch_pressure_enabled */
	if (of_property_read_u32(devnode, "touch_pressure_enabled",
		&pdata->pressure_enabled)) {
		dev_err(dev, "Failed to get property: touch_pressure_enabled\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse touch_size_enabled */
	if (of_property_read_u32(devnode, "touch_size_enabled",
		&pdata->size_enabled)) {
		dev_err(dev, "Failed to get property: touch_size_enabled\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse touch_orientation_enabled */
	if (of_property_read_u32(devnode, "touch_orientation_enabled",
		&pdata->orientation_enabled)) {
		dev_err(dev, "Failed to get property: touch_orientation_enabled\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse button_code3 */
	if (of_property_read_u32(devnode, "button_code3",
		&pdata->button_code3)) {
		dev_err(dev, "Failed to get property: button_code3\n");
		goto err_max1187x_get_platdata_dt;
	}

	/* Parse enable_resume_por */
	if (of_property_read_u32(devnode, "enable_resume_por",
		&pdata->enable_resume_por)) {
		dev_err(dev, "Failed to get property: enable_resume_por\n");
		goto err_max1187x_get_platdata_dt;
	}

	return pdata;

err_max1187x_get_platdata_dt:
	devm_kfree(dev, pdata);
	return NULL;
}

static int validate_pdata(struct device *dev, struct max1187x_pdata *pdata)
{
	if (!pdata) {
		dev_err(dev, "Platform data not found!\n");
		goto err_validate_pdata;
	}

	if (!pdata->gpio_tirq) {
		dev_err(dev, "gpio_tirq (%u) not defined!\n", pdata->gpio_tirq);
		goto err_validate_pdata;
	}

	if (!pdata->gpio_pwr_en)
		dev_err(dev, "gpio_pwr_en (%u) not defined!\n",
							pdata->gpio_pwr_en);

	if (pdata->lcd_x < MXM_LCD_X_MIN || pdata->lcd_x > MXM_LCD_SIZE_MAX) {
		dev_err(dev, "lcd_x (%u) out of range!\n", pdata->lcd_x);
		goto err_validate_pdata;
	}

	if (pdata->lcd_y < MXM_LCD_Y_MIN || pdata->lcd_y > MXM_LCD_SIZE_MAX) {
		dev_err(dev, "lcd_y (%u) out of range!\n", pdata->lcd_y);
		goto err_validate_pdata;
	}

	if (!pdata->num_sensor_x || pdata->num_sensor_x > MXM_NUM_SENSOR_MAX) {
		dev_err(dev, "num_sensor_x (%u) out of range!\n",
				pdata->num_sensor_x);
		goto err_validate_pdata;
	}

	if (!pdata->num_sensor_y || pdata->num_sensor_y > MXM_NUM_SENSOR_MAX) {
		dev_err(dev, "num_sensor_y (%u) out of range!\n",
				pdata->num_sensor_y);
		goto err_validate_pdata;
	}

	return 0;

err_validate_pdata:
	return -ENXIO;
}

static int max1187x_chip_init(struct data *ts, bool enable)
{
	int  ret;

	if (enable) {
		ret = gpio_request(ts->pdata->gpio_tirq, "max1187x_tirq");
		if (ret) {
			dev_err(&ts->client->dev,
				"GPIO request failed for max1187x_tirq (%d)\n",
				ts->pdata->gpio_tirq);
			return -EIO;
		}
		ret = gpio_direction_input(ts->pdata->gpio_tirq);
		if (ret) {
			dev_err(&ts->client->dev,
				"GPIO set input direction failed for " \
				"max1187x_tirq (%d)\n", ts->pdata->gpio_tirq);
			gpio_free(ts->pdata->gpio_tirq);
			return -EIO;
		}
	} else {
		gpio_free(ts->pdata->gpio_tirq);
	}

	return 0;
}

static void max1187x_gpio_pwr_en_init(struct data *ts, bool enable)
{
	int  ret;

	if (enable) {
		ret = gpio_request(ts->pdata->gpio_pwr_en,
						"max1187x_gpio_pwr_en");
		if (ret) {
			dev_err(&ts->client->dev,
				"GPIO request failed for gpio_pwr_en (%d)\n",
				ts->pdata->gpio_pwr_en);
			return;
		}
		ret = gpio_direction_output(ts->pdata->gpio_pwr_en, 0);
		if (ret) {
			dev_err(&ts->client->dev,
				"GPIO set output direction failed for " \
				"max1187x_gpio_pwr_en (%d)\n",
				ts->pdata->gpio_pwr_en);
			gpio_free(ts->pdata->gpio_pwr_en);
		}
	} else {
		gpio_free(ts->pdata->gpio_pwr_en);
	}
}

static int probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct data *ts;
	struct max1187x_pdata *pdata;
	struct kobject *parent;
	int ret;

	dev_info(dev, "(INIT): Start");

	/* if I2C functionality is not present we are done */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "I2C core driver does not support  " \
						"I2C functionality");
		ret = -ENXIO;
		goto err_device_init;
	}
	dev_info(dev, "(INIT): I2C functionality OK");

	/* allocate control block; nothing more to do if we can't */
	ts = kzalloc(sizeof(*ts), GFP_KERNEL);
	if (!ts) {
		dev_err(dev, "Failed to allocate control block memory");
		ret = -ENOMEM;
		goto err_device_init;
	}

	/* Get platform data */
	pdata = dev_get_platdata(dev);
	/* If pdata is missing, try to get pdata from device tree (dts) */
	if (!pdata)
		pdata = max1187x_get_platdata_dt(dev);

	/* Validate if pdata values are okay */
	ret = validate_pdata(dev, pdata);
	if (ret < 0)
		goto err_device_init_pdata;
	dev_info(dev, "(INIT): Platform data OK");

	ts->pdata = pdata;
	ts->client = client;
	i2c_set_clientdata(client, ts);
	mutex_init(&ts->fw_mutex);
	mutex_init(&ts->i2c_mutex);
	mutex_init(&ts->report_mutex);
	sema_init(&ts->report_sem, 1);
	sema_init(&ts->sema_rbcmd, 1);
	ts->button0 = 0;
	ts->button1 = 0;
	ts->button2 = 0;
	ts->button3 = 0;

	init_waitqueue_head(&ts->waitqueue_all);
	init_waitqueue_head(&ts->waitqueue_rbcmd);

	dev_info(dev, "(INIT): Memory allocation OK");

	ret = vreg_configure(ts, true);
	if (ret < 0) {
			dev_err(dev, "Failed to configure VREG");
			goto err_device_init_vreg;
	}
	dev_info(&ts->client->dev, "(INIT): VREG OK");
	msleep(MXM_PWR_SET_WAIT_MS);

	/* Initialize GPIO pins */
	if (max1187x_chip_init(ts, true) < 0) {
		ret = -EIO;
		goto err_device_init_gpio;
	}
	dev_info(dev, "(INIT): chip init OK");

	if (pdata->gpio_pwr_en)
		max1187x_gpio_pwr_en_init(ts, true);

	/* allocate and register touch device */
	ts->input_dev = input_allocate_device();
	if (!ts->input_dev) {
		dev_err(dev, "Failed to allocate touch input device");
		ret = -ENOMEM;
		goto err_device_init_inputdev;
	}
	snprintf(ts->phys, sizeof(ts->phys), "%s/input0",
			dev_name(dev));
	ts->input_dev->name = MAX1187X_TOUCH;
	ts->input_dev->phys = ts->phys;
	ts->input_dev->id.bustype = BUS_I2C;
	__set_bit(EV_SYN, ts->input_dev->evbit);
	__set_bit(EV_ABS, ts->input_dev->evbit);
	__set_bit(EV_KEY, ts->input_dev->evbit);

	input_mt_init_slots(ts->input_dev, MXM_TOUCH_COUNT_MAX);
	ts->list_finger_ids = 0;
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X,
			ts->pdata->panel_margin_xl,
			ts->pdata->panel_margin_xl + ts->pdata->lcd_x, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y,
			ts->pdata->panel_margin_yl,
			ts->pdata->panel_margin_yl + ts->pdata->lcd_y, 0, 0);
	if (ts->pdata->pressure_enabled)
		input_set_abs_params(ts->input_dev, ABS_MT_PRESSURE,
				0, MXM_PRESSURE_SQRT_MAX, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TOOL_TYPE,
			MT_TOOL_FINGER, MT_TOOL_FINGER, 0, 0);
	if (ts->pdata->size_enabled) {
		input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR,
				0, ts->pdata->lcd_x + ts->pdata->lcd_y, 0, 0);
		input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MINOR,
				0, ts->pdata->lcd_x + ts->pdata->lcd_y, 0, 0);
	}
	if (ts->pdata->orientation_enabled)
		input_set_abs_params(ts->input_dev, ABS_MT_ORIENTATION,
				-90, 90, 0, 0);
	if (ts->pdata->button_code0 != KEY_RESERVED)
		set_bit(pdata->button_code0, ts->input_dev->keybit);
	if (ts->pdata->button_code1 != KEY_RESERVED)
		set_bit(pdata->button_code1, ts->input_dev->keybit);
	if (ts->pdata->button_code2 != KEY_RESERVED)
		set_bit(pdata->button_code2, ts->input_dev->keybit);
	if (ts->pdata->button_code3 != KEY_RESERVED)
		set_bit(pdata->button_code3, ts->input_dev->keybit);

	ret = input_register_device(ts->input_dev);
	if (ret) {
		dev_err(dev, "Failed to register touch input device");
		ret = -EPERM;
		input_free_device(ts->input_dev);
		goto err_device_init_inputdev;
	}
	dev_info(dev, "(INIT): Input touch device OK");

	/* allocate and register touch device */
	ts->input_pen = input_allocate_device();
	if (!ts->input_pen) {
		dev_err(dev, "Failed to allocate touch input pen device");
		ret = -ENOMEM;
		goto err_device_init_inputpendev;
	}
	snprintf(ts->phys, sizeof(ts->phys), "%s/input0",
			dev_name(dev));
	ts->input_pen->name = MAX1187X_PEN;
	ts->input_pen->phys = ts->phys;
	ts->input_dev->id.bustype = BUS_I2C;
	__set_bit(EV_SYN, ts->input_pen->evbit);
	__set_bit(EV_ABS, ts->input_pen->evbit);

	input_mt_init_slots(ts->input_pen, MXM_TOUCH_COUNT_MAX);
	ts->list_finger_ids = 0;
	input_set_abs_params(ts->input_pen, ABS_MT_POSITION_X,
			ts->pdata->panel_margin_xl,
			ts->pdata->panel_margin_xl + ts->pdata->lcd_x, 0, 0);
	input_set_abs_params(ts->input_pen, ABS_MT_POSITION_Y,
			ts->pdata->panel_margin_yl,
			ts->pdata->panel_margin_yl + ts->pdata->lcd_y, 0, 0);
	if (ts->pdata->pressure_enabled)
		input_set_abs_params(ts->input_pen, ABS_MT_PRESSURE,
				0, 0xFF, 0, 0);
	input_set_abs_params(ts->input_pen, ABS_MT_TOOL_TYPE,
			0, MT_TOOL_MAX, 0, 0);
	if (ts->pdata->size_enabled) {
		input_set_abs_params(ts->input_pen, ABS_MT_TOUCH_MAJOR,
				0, ts->pdata->lcd_x + ts->pdata->lcd_y, 0, 0);
		input_set_abs_params(ts->input_pen, ABS_MT_TOUCH_MINOR,
				0, ts->pdata->lcd_x + ts->pdata->lcd_y, 0, 0);
	}
	if (ts->pdata->orientation_enabled)
		input_set_abs_params(ts->input_pen, ABS_MT_ORIENTATION,
				-90, 90, 0, 0);
	ret = input_register_device(ts->input_pen);
	if (ret) {
		dev_err(dev, "Failed to register touch pen input device");
		ret = -EPERM;
		input_free_device(ts->input_pen);
		goto err_device_init_inputpendev;
	}
	dev_info(dev, "(INIT): Input touch pen device OK");

#ifdef MXM_TOUCH_WAKEUP_FEATURE
	ts->input_dev_key = input_allocate_device();
	if (!ts->input_dev_key) {
		dev_err(dev, "Failed to allocate touch input key device");
		ret = -ENOMEM;
		goto err_device_init_inputdevkey;
	}
	snprintf(ts->phys_key, sizeof(ts->phys_key), "%s/input1",
		dev_name(&client->dev));
	ts->input_dev_key->name = MAX1187X_KEY;
	ts->input_dev_key->phys = ts->phys_key;
	ts->input_dev_key->id.bustype = BUS_I2C;
	__set_bit(EV_KEY, ts->input_dev_key->evbit);
	set_bit(KEY_POWER, ts->input_dev_key->keybit);
	ret = input_register_device(ts->input_dev_key);
	if (ret) {
		dev_err(dev, "Failed to register touch input key device");
		ret = -EPERM;
		input_free_device(ts->input_dev_key);
		goto err_device_init_inputdevkey;
	}
	dev_info(dev, "(INIT): Input key device OK");
#endif

	/* Setup IRQ and handler */
	ret = request_threaded_irq(client->irq,
			irq_handler_hard, irq_handler_soft,
			IRQF_TRIGGER_FALLING, client->name, ts);
	if (ret) {
		dev_err(dev, "Failed to setup IRQ handler");
		ret = -EIO;
		goto err_device_init_irq;
	}
	dev_info(&ts->client->dev, "(INIT): IRQ handler OK");

	/* collect controller ID and configuration ID data from firmware   */
	ret = read_chip_data(ts);
	if (ret)
		dev_warn(dev, "No firmware response (%d)", ret);

	ts->is_suspended = false;
	dev_info(dev, "(INIT): suspend/resume registration OK");

	/* set up debug interface */
	ret = create_sysfs_entries(ts);
	if (ret) {
		dev_err(dev, "failed to create sysfs file");
		goto err_device_init_irq;
	}

	if (device_create_bin_file(&client->dev, &dev_attr_report) < 0) {
		dev_err(dev, "failed to create sysfs file [report]");
		goto err_device_init_sysfs_remove_group;
	}
	ts->sysfs_created++;

	/* create symlink */
	parent = ts->input_dev->dev.kobj.parent;
	ret = sysfs_create_link(parent, &client->dev.kobj, MAX1187X_NAME);
	if (ret)
		dev_err(dev, "sysfs_create_link error\n");

#ifdef MXM_TOUCH_WAKEUP_FEATURE
	dev_info(dev, "Touch Wakeup Feature enabled\n");
	device_init_wakeup(&client->dev, 1);
	device_wakeup_disable(&client->dev);
#endif

	dev_info(dev, "(INIT): Done\n");
	return 0;

err_device_init_sysfs_remove_group:
	remove_sysfs_entries(ts);
err_device_init_irq:
#ifdef MXM_TOUCH_WAKEUP_FEATURE
	input_unregister_device(ts->input_dev_key);
err_device_init_inputdevkey:
#endif
	input_unregister_device(ts->input_pen);
err_device_init_inputpendev:
	input_unregister_device(ts->input_dev);
err_device_init_inputdev:
	max1187x_chip_init(ts, false);
err_device_init_gpio:
	vreg_configure(ts, false);
err_device_init_vreg:
err_device_init_pdata:
	kfree(ts);
err_device_init:
	return ret;
}

static int remove(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct data *ts = i2c_get_clientdata(client);
	struct max1187x_pdata *pdata;

	if (ts == NULL)
		return 0;

	pdata = ts->pdata;

	propagate_report(ts, -1, NULL);

#ifdef MXM_TOUCH_WAKEUP_FEATURE
	device_init_wakeup(&client->dev, 0);
#endif

	remove_sysfs_entries(ts);

	if (ts->sysfs_created && ts->sysfs_created--)
		device_remove_bin_file(&client->dev, &dev_attr_report);

	if (client->irq)
			free_irq(client->irq, ts);

#ifdef MXM_TOUCH_WAKEUP_FEATURE
	input_unregister_device(ts->input_dev_key);
#endif
	sysfs_remove_link(ts->input_dev->dev.kobj.parent,
						MAX1187X_NAME);
	input_unregister_device(ts->input_dev);
	input_unregister_device(ts->input_pen);

	(void) max1187x_chip_init(ts, false);
	if (pdata->gpio_pwr_en)
		max1187x_gpio_pwr_en_init(ts, false);
	vreg_configure(ts, false);
	kfree(ts);

	dev_info(dev, "(INIT): Deinitialized\n");

	return 0;
}

/* Commands */
static void process_rbcmd(struct data *ts)
{
	if (!ts->rbcmd_waiting)
		return;
	if (ts->rbcmd_report_id != ts->rx_report[1])
		return;

	ts->rbcmd_received = 1;
	memcpy(ts->rbcmd_rx_report, ts->rx_report, (ts->rx_report_len + 1)<<1);
	ts->rbcmd_rx_report_len = ts->rx_report_len;
	wake_up_interruptible(&ts->waitqueue_rbcmd);
}

static int combine_multipacketreport(struct data *ts, u16 *report)
{
	u16 packet_header = report[0];
	u8 packet_seq_num = BYTEH(packet_header);
	u8 packet_size = BYTEL(packet_header);
	u16 total_packets, this_packet_num, offset;
	static u16 packet_seq_combined;

	if (packet_seq_num == MXM_ONE_PACKET_RPT) {
		memcpy(ts->rx_report, report, (packet_size + 1) << 1);
		ts->rx_report_len = packet_size;
		packet_seq_combined = 1;
		return 0;
	}

	total_packets = HI_NIBBLE(packet_seq_num);
	this_packet_num = LO_NIBBLE(packet_seq_num);

	if (this_packet_num == 1) {
		if (report[1] == MXM_RPT_ID_TOUCH_RAW_IMAGE) {
			ts->rx_report_len = report[2] + 2;
			packet_seq_combined = 1;
			memcpy(ts->rx_report, report, (packet_size + 1) << 1);
			return -EAGAIN;
		} else {
			return -EIO;
		}
	} else if (this_packet_num == packet_seq_combined + 1) {
		packet_seq_combined++;
		offset = (this_packet_num - 1) * MXM_RPT_MAX_WORDS_PER_PKT + 1;
		memcpy(ts->rx_report + offset, report + 1, packet_size << 1);
		if (total_packets == this_packet_num)
			return 0;
		else
			return -EIO;
	}
	return -EIO;
}

static void propagate_report(struct data *ts, int status, u16 *report)
{
	int i, ret;

	down(&ts->report_sem);
	mutex_lock(&ts->report_mutex);

	if (report) {
		ret = combine_multipacketreport(ts, report);
		if (ret) {
			up(&ts->report_sem);
			mutex_unlock(&ts->report_mutex);
			return;
		}
	}
	process_rbcmd(ts);

	for (i = 0; i < MXM_REPORT_READERS_MAX; i++) {
		if (!status) {
			if (ts->report_readers[i].report_id == 0xFFFF
				|| (ts->rx_report[1]
				&& ts->report_readers[i].report_id
				== ts->rx_report[1])) {
				up(&ts->report_readers[i].sem);
				ts->report_readers[i].reports_passed++;
				ts->report_readers_outstanding++;
			}
		} else {
			if (ts->report_readers[i].report_id) {
				ts->report_readers[i].status = status;
				up(&ts->report_readers[i].sem);
			}
		}
	}
	if (!ts->report_readers_outstanding)
		up(&ts->report_sem);
	mutex_unlock(&ts->report_mutex);
}

static int get_report(struct data *ts, u16 report_id, ulong timeout)
{
	int i, ret, status;

	mutex_lock(&ts->report_mutex);
	for (i = 0; i < MXM_REPORT_READERS_MAX; i++)
		if (!ts->report_readers[i].report_id)
			break;
	if (i == MXM_REPORT_READERS_MAX) {
		mutex_unlock(&ts->report_mutex);
		dev_err(&ts->client->dev, "maximum readers reached");
		return -EBUSY;
	}
	ts->report_readers[i].report_id = report_id;
	sema_init(&ts->report_readers[i].sem, 1);
	down(&ts->report_readers[i].sem);
	ts->report_readers[i].status = 0;
	ts->report_readers[i].reports_passed = 0;
	mutex_unlock(&ts->report_mutex);

	if (timeout == 0xFFFFFFFF)
		ret = down_interruptible(&ts->report_readers[i].sem);
	else
		ret = down_timeout(&ts->report_readers[i].sem,
			(timeout * HZ) / 1000);

	mutex_lock(&ts->report_mutex);
	if (ret && ts->report_readers[i].reports_passed > 0)
		if (--ts->report_readers_outstanding == 0)
			up(&ts->report_sem);
	status = ts->report_readers[i].status;
	ts->report_readers[i].report_id = 0;
	mutex_unlock(&ts->report_mutex);

	return (!status) ? ret : status;
}

static void release_report(struct data *ts)
{
	mutex_lock(&ts->report_mutex);
	if (--ts->report_readers_outstanding == 0)
		up(&ts->report_sem);
	mutex_unlock(&ts->report_mutex);
}

static void set_suspend_mode(struct data *ts)
{
	u16 cmd_buf[] = {MXM_CMD_ID_SET_POWER_MODE,
			 MXM_ONE_SIZE_CMD,
			 MXM_PWR_SLEEP_MODE};
	int ret;

	dev_info(&ts->client->dev, "%s\n", __func__);

	disable_irq(ts->client->irq);
	ts->is_suspended = true;

#ifdef MXM_TOUCH_WAKEUP_FEATURE
	if (&ts->is_suspended)
		cmd_buf[2] = MXM_WAKEUP_MODE;
#endif
	ret = cmd_send(ts, cmd_buf, 3);
	if (ret)
		dev_err(&ts->client->dev, "Failed to set sleep mode");

	if (cmd_buf[2] != MXM_WAKEUP_MODE) {
		usleep_range(MXM_WAIT_MIN_US, MXM_WAIT_MAX_US);
		vreg_suspend(ts, true);
	}

#ifdef MXM_TOUCH_WAKEUP_FEATURE
	if (&ts->is_suspended)
		enable_irq(ts->client->irq);
#endif

	dev_dbg(&ts->client->dev, "%s: Exit\n", __func__);
	return;
}

static void set_resume_mode(struct data *ts)
{
	u16 cmd_buf[] = {MXM_CMD_ID_SET_POWER_MODE,
			 MXM_ONE_SIZE_CMD,
			 MXM_ACTIVE_MODE};
	int ret;

	dev_info(&ts->client->dev, "%s\n", __func__);

	vreg_suspend(ts, false);
	usleep_range(MXM_WAIT_MIN_US, MXM_WAIT_MAX_US);

#ifdef MXM_TOUCH_WAKEUP_FEATURE
	if (&ts->is_suspended)
		disable_irq(ts->client->irq);
#endif

	if (ts->pdata->enable_resume_por)
		reset_power(ts);

	ret = cmd_send(ts, cmd_buf, 3);
	if (ret)
		dev_err(&ts->client->dev, "Failed to set active mode");

	cmd_buf[0] = MXM_CMD_ID_SET_TOUCH_RPT_MODE;
	cmd_buf[1] = MXM_ONE_SIZE_CMD;
	cmd_buf[2] = MXM_TOUCH_REPORT_MODE_EXT;
	ret = cmd_send(ts, cmd_buf, 3);
	if (ret)
		dev_err(&ts->client->dev, "Failed to set up touch report mode");

	ts->is_suspended = false;

	enable_irq(ts->client->irq);

	dev_dbg(&ts->client->dev, "%s: Exit\n", __func__);

	return;
}

static int suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);

	dev_dbg(&ts->client->dev, "%s: Enter\n", __func__);

#ifdef MXM_TOUCH_WAKEUP_FEATURE
	if (&ts->is_suspended)
		enable_irq_wake(client->irq);
#endif

	dev_dbg(&ts->client->dev, "%s: Exit\n", __func__);

	return 0;
}

static int resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct data *ts = i2c_get_clientdata(client);

	dev_dbg(&ts->client->dev, "%s: Enter\n", __func__);

#ifdef MXM_TOUCH_WAKEUP_FEATURE
	if (&ts->is_suspended)
		disable_irq_wake(client->irq);
#endif

	dev_dbg(&ts->client->dev, "%s: Exit\n", __func__);

	return 0;
}

static const struct dev_pm_ops max1187x_pm_ops = {
	.resume = resume,
	.suspend = suspend,
};

static int bootloader_read_status_reg(struct data *ts, const u8 byteL,
	const u8 byteH)
{
	u8 buffer[] = { MXM_BL_STATUS_ADDR_L, MXM_BL_STATUS_ADDR_H }, i;

	for (i = 0; i < MXM_BL_RD_STATUS_RETRY; i++) {
		if (i2c_tx_bytes(ts, buffer, 2) != 2) {
			dev_err(&ts->client->dev, "TX fail");
			return -EIO;
		}
		if (i2c_rx_bytes(ts, buffer, 2) != 2) {
			dev_err(&ts->client->dev, "RX fail");
			return -EIO;
		}
		if (buffer[0] == byteL && buffer[1] == byteH)
			break;
	}
	if (i == MXM_BL_RD_STATUS_RETRY) {
		dev_err(&ts->client->dev, "Unexpected status => %02X%02X vs" \
			"%02X%02X", buffer[0], buffer[1], byteL, byteH);
		return -EIO;
	}

	return 0;
}

static int bootloader_write_status_reg(struct data *ts, const u8 byteL,
	const u8 byteH)
{
	u8 buffer[] = { MXM_BL_STATUS_ADDR_L, MXM_BL_STATUS_ADDR_H,
							byteL, byteH };

	if (i2c_tx_bytes(ts, buffer, 4) != 4) {
		dev_err(&ts->client->dev, "TX fail");
		return -EIO;
	}
	return 0;
}

static int bootloader_rxtx_complete(struct data *ts)
{
	return bootloader_write_status_reg(ts, MXM_BL_RXTX_COMPLETE_L,
				MXM_BL_RXTX_COMPLETE_H);
}

static int bootloader_read_data_reg(struct data *ts, u8 *byteL, u8 *byteH)
{
	u8 buffer[] = { MXM_BL_DATA_ADDR_L, MXM_BL_DATA_ADDR_H, 0x00, 0x00 };

	if (i2c_tx_bytes(ts, buffer, 2) != 2) {
		dev_err(&ts->client->dev, "TX fail");
		return -EIO;
	}
	if (i2c_rx_bytes(ts, buffer, 4) != 4) {
		dev_err(&ts->client->dev, "RX fail");
		return -EIO;
	}
	if (buffer[2] != MXM_BL_STATUS_READY_L &&
		buffer[3] != MXM_BL_STATUS_READY_H) {
		dev_err(&ts->client->dev, "Status is not ready");
		return -EIO;
	}

	*byteL = buffer[0];
	*byteH = buffer[1];
	return bootloader_rxtx_complete(ts);
}

static int bootloader_write_data_reg(struct data *ts, const u8 byteL,
	const u8 byteH)
{
	u8 buffer[6] = { MXM_BL_DATA_ADDR_L, MXM_BL_DATA_ADDR_H, byteL, byteH,
			MXM_BL_RXTX_COMPLETE_L, MXM_BL_RXTX_COMPLETE_H };

	if (bootloader_read_status_reg(ts, MXM_BL_STATUS_READY_L,
		MXM_BL_STATUS_READY_H) < 0) {
		dev_err(&ts->client->dev, "read status register fail");
		return -EIO;
	}
	if (i2c_tx_bytes(ts, buffer, 6) != 6) {
		dev_err(&ts->client->dev, "TX fail");
		return -EIO;
	}
	return 0;
}

static int bootloader_rxtx(struct data *ts, u8 *byteL, u8 *byteH,
	const int tx)
{
	if (tx > 0) {
		if (bootloader_write_data_reg(ts, *byteL, *byteH) < 0) {
			dev_err(&ts->client->dev, "write data register fail");
			return -EIO;
		}
		return 0;
	}

	if (bootloader_read_data_reg(ts, byteL, byteH) < 0) {
		dev_err(&ts->client->dev, "read data register fail");
		return -EIO;
	}
	return 0;
}

static int bootloader_get_cmd_conf(struct data *ts, int retries)
{
	u8 byteL, byteH;

	do {
		if (bootloader_read_data_reg(ts, &byteL, &byteH) >= 0) {
			if (byteH == MXM_BL_DATA_READY_H &&
				byteL == MXM_BL_DATA_READY_L)
				return 0;
		}
		retries--;
	} while (retries > 0);

	return -EIO;
}

static int bootloader_write_buffer(struct data *ts, u8 *buffer, int size)
{
	u8 byteH = MXM_BL_WR_START_ADDR;
	int k;

	for (k = 0; k < size; k++) {
		if (bootloader_rxtx(ts, &buffer[k], &byteH, 1) < 0) {
			dev_err(&ts->client->dev, "bootloader RX-TX fail");
			return -EIO;
		}
	}
	return 0;
}

static int bootloader_enter(struct data *ts)
{
	int i;
	u16 enter[3][2] = { { MXM_BL_ENTER_SEQ_L, MXM_BL_ENTER_SEQ_H1 },
			    { MXM_BL_ENTER_SEQ_L, MXM_BL_ENTER_SEQ_H2 },
			    { MXM_BL_ENTER_SEQ_L, MXM_BL_ENTER_SEQ_H3 }
			  };

	for (i = 0; i < MXM_BL_ENTER_RETRY; i++) {
		if (i2c_tx_words(ts, enter[i], 2) != 2) {
			dev_err(&ts->client->dev, "Failed to enter bootloader");
			return -EIO;
		}
	}

	if (bootloader_get_cmd_conf(ts, MXM_BL_ENTER_CONF_RETRY) < 0) {
		dev_err(&ts->client->dev, "Failed to enter bootloader mode");
		return -EIO;
	}
	return 0;
}

static int bootloader_exit(struct data *ts)
{
	int i;
	u16 exit[3][2] = { { MXM_BL_EXIT_SEQ_L, MXM_BL_EXIT_SEQ_H1 },
			   { MXM_BL_EXIT_SEQ_L, MXM_BL_EXIT_SEQ_H2 },
			   { MXM_BL_EXIT_SEQ_L, MXM_BL_EXIT_SEQ_H3 }
			 };

	for (i = 0; i < MXM_BL_EXIT_RETRY; i++) {
		if (i2c_tx_words(ts, exit[i], 2) != 2) {
			dev_err(&ts->client->dev, "Failed to exit bootloader");
			return -EIO;
		}
	}

	return 0;
}

static int bootloader_get_crc(struct data *ts, u16 *crc16,
		u16 addr, u16 len, u16 delay)
{
	u8 crc_command[] = { MXM_BL_GET_CRC_L, MXM_BL_GET_CRC_H,
			     BYTEL(addr), BYTEH(addr), BYTEL(len), BYTEH(len)
			   };
	u8 byteL = 0, byteH = 0;
	u16 rx_crc16 = 0;

	if (bootloader_write_buffer(ts, crc_command, 6) < 0) {
		dev_err(&ts->client->dev, "write buffer fail");
		return -EIO;
	}
	msleep(delay);

	/* reads low 8bits (crcL) */
	if (bootloader_rxtx(ts, &byteL, &byteH, 0) < 0) {
		dev_err(&ts->client->dev,
			"Failed to read low byte of crc response!");
		return -EIO;
	}
	rx_crc16 = (u16) byteL;

	/* reads high 8bits (crcH) */
	if (bootloader_rxtx(ts, &byteL, &byteH, 0) < 0) {
		dev_err(&ts->client->dev,
			"Failed to read high byte of crc response!");
		return -EIO;
	}
	rx_crc16 = (u16)(byteL << 8) | rx_crc16;

	if (bootloader_get_cmd_conf(ts, MXM_BL_CRC_GET_RETRY) < 0) {
		dev_err(&ts->client->dev, "CRC get failed!");
		return -EIO;
	}
	*crc16 = rx_crc16;

	return 0;
}

static int bootloader_set_byte_mode(struct data *ts)
{
	u8 buffer[2] = {MXM_BL_SET_BYTE_MODE_L, MXM_BL_SET_BYTE_MODE_H};

	if (bootloader_write_buffer(ts, buffer, 2) < 0) {
		dev_err(&ts->client->dev, "write buffer fail");
		return -EIO;
	}
	if (bootloader_get_cmd_conf(ts, 10) < 0) {
		dev_err(&ts->client->dev, "command confirm fail");
		return -EIO;
	}
	return 0;
}

static int bootloader_erase_flash(struct data *ts)
{
	u8 byteL = MXM_BL_ERASE_FLASH_L, byteH = MXM_BL_ERASE_FLASH_H;
	int i, verify = 0;

	if (bootloader_rxtx(ts, &byteL, &byteH, 1) < 0) {
		dev_err(&ts->client->dev, "bootloader RX-TX fail");
		return -EIO;
	}

	for (i = 0; i < MXM_BL_ERASE_CONF_RETRY; i++) {
		msleep(MXM_BL_ERASE_DELAY_MS);

		if (bootloader_get_cmd_conf(ts, 0) < 0)
			continue;

		verify = 1;
		break;
	}

	if (verify != 1) {
		dev_err(&ts->client->dev, "Flash Erase failed");
		return -EIO;
	}

	return 0;
}

static int bootloader_write_flash(struct data *ts, const u8 *image, u16 length)
{
	struct device *dev = &ts->client->dev;
	u8 buffer[MXM_BL_WR_TX_SZ];
	u8 length_L = length & 0xFF;
	u8 length_H = (length >> 8) & 0xFF;
	u8 command[] = { MXM_BL_WR_FAST_FLASH_L, MXM_BL_WR_FAST_FLASH_H,
			length_H, length_L, MXM_BL_WR_START_ADDR };
	u16 blocks_of_128bytes;
	int i, j;

	if (bootloader_write_buffer(ts, command, 5) < 0) {
		dev_err(dev, "write buffer fail");
		return -EIO;
	}

	blocks_of_128bytes = length >> 7;

	for (i = 0; i < blocks_of_128bytes; i++) {
		for (j = 0; j < MXM_BL_WR_STATUS_RETRY; j++) {
			usleep_range(MXM_WAIT_MIN_US, MXM_WAIT_MAX_US);
			if (!bootloader_read_status_reg(ts,
				MXM_BL_STATUS_READY_L, MXM_BL_STATUS_READY_H))
				break;
		}
		if (j == MXM_BL_WR_STATUS_RETRY) {
			dev_err(dev, "Failed to read Status register!");
			return -EIO;
		}

		buffer[0] = (!(i % 2)) ? MXM_BL_WR_DBUF0_ADDR :
					 MXM_BL_WR_DBUF1_ADDR;
		buffer[1] = 0x00;
		memcpy(buffer + 2, image + i * MXM_BL_WR_BLK_SIZE,
						MXM_BL_WR_BLK_SIZE);

		if (i2c_tx_bytes(ts, buffer, MXM_BL_WR_TX_SZ) !=
						MXM_BL_WR_TX_SZ) {
			dev_err(dev, "Failed to write data (%d)", i);
			return -EIO;
		}
		if (bootloader_rxtx_complete(ts) < 0) {
			dev_err(dev, "Transfer failure (%d)", i);
			return -EIO;
		}
	}

	usleep_range(MXM_BL_WR_MIN_US, MXM_BL_WR_MAX_US);
	if (bootloader_get_cmd_conf(ts, MXM_BL_WR_CONF_RETRY) < 0) {
		dev_err(dev, "Flash programming failed");
		return -EIO;
	}
	return 0;
}

/****************************************
 *
 * Standard Driver Structures/Functions
 *
 ****************************************/
static const struct i2c_device_id id[] = { { MAX1187X_NAME, 0 }, { } };

MODULE_DEVICE_TABLE(i2c, id);

static struct of_device_id max1187x_dt_match[] = {
	{ .compatible = "maxim,max1187x_tsc" },	{ } };

static struct i2c_driver driver = {
		.probe = probe,
		.remove = remove,
		.id_table = id,
		.driver = {
			.name = MAX1187X_NAME,
			.owner	= THIS_MODULE,
			.of_match_table = max1187x_dt_match,
			.pm = &max1187x_pm_ops,
		},
};

static int __devinit max1187x_init(void)
{
	return i2c_add_driver(&driver);
}

static void __exit max1187x_exit(void)
{
	i2c_del_driver(&driver);
}

module_init(max1187x_init);
module_exit(max1187x_exit);

MODULE_AUTHOR("Maxim Integrated Products, Inc.");
MODULE_DESCRIPTION("MAX1187X Touchscreen Driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("3.1.8");
