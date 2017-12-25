/*
 * DSPG DBMDX I2C interface driver
 *
 * Copyright (C) 2014 DSP Group
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
/* #define DEBUG */
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_gpio.h>
#endif
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/clk.h>
#include <linux/firmware.h>

#include "dbmdx-interface.h"
#include "dbmdx-va-regmap.h"
#include "dbmdx-vqe-regmap.h"
#include "dbmdx-i2c.h"


#define DEFAULT_I2C_WRITE_CHUNK_SIZE	32
#define MAX_I2C_WRITE_CHUNK_SIZE	128
#define DEFAULT_I2C_READ_CHUNK_SIZE	8
#define MAX_I2C_READ_CHUNK_SIZE		128

ssize_t send_i2c_cmd_vqe(struct dbmdx_private *p,
	u32 command, u16 *response)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;
	u8 send[4];
	u8 recv[4];
	ssize_t ret = 0;
	int retries = 10;

	send[0] = (command >> 24) & 0xff;
	send[1] = (command >> 16) & 0xff;
	send[2] = (command >> 8) & 0xff;
	send[3] = command & 0xff;

	ret = i2c_master_send(i2c_p->client, send, 4);
	if (ret < 0) {
		dev_err(i2c_p->dev,
				"%s: cmd:0x%08X - i2c_master_send failed ret:%zd\n",
				__func__, command, ret);
		return ret;
	}

	usleep_range(DBMDX_USLEEP_I2C_VQE_CMD_AFTER_SEND,
		DBMDX_USLEEP_I2C_VQE_CMD_AFTER_SEND + 1000);

	if ((command == (DBMDX_VQE_SET_POWER_STATE_CMD |
			DBMDX_VQE_SET_POWER_STATE_HIBERNATE)) ||
		(command == DBMDX_VQE_SET_SWITCH_TO_BOOT_CMD))
		return 0;

	/* we need additional sleep till system is ready */
	if ((command == (DBMDX_VQE_SET_SYSTEM_CONFIG_CMD |
			DBMDX_VQE_SET_SYSTEM_CONFIG_PRIMARY_CFG)))
		msleep(DBMDX_MSLEEP_I2C_VQE_SYS_CFG_CMD);

	/* read response */
	do {
		ret = i2c_master_recv(i2c_p->client, recv, 4);
		if (ret < 0) {
#if 0
			dev_dbg(i2c_p->dev, "%s: read failed; retries:%d\n",
				__func__, retries);
#endif
			/* Wait before polling again */
			usleep_range(10000, 11000);

			continue;
		}
		/*
		 * Check that the first two bytes of the response match
		 * (the ack is in those bytes)
		 */
		if ((send[0] == recv[0]) && (send[1] == recv[1])) {
			if (response)
				*response = (recv[2] << 8) | recv[3];
			ret = 0;
			break;
		}

		dev_warn(i2c_p->dev,
			"%s: incorrect ack (got 0x%.2x%.2x)\n",
			__func__, recv[0], recv[1]);
		ret = -EINVAL;

		/* Wait before polling again */
		usleep_range(10000, 11000);
	} while (--retries);

	if (!retries)
		dev_err(i2c_p->dev,
			"%s: cmd:0x%8x - wrong ack, giving up\n",
			__func__, command);

	return ret;
}

ssize_t send_i2c_cmd_va(struct dbmdx_private *p, u32 command,
				  u16 *response)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;
	u8 send[3];
	u8 recv[2];
	int ret;

	send[0] = (command >> 16) & 0xff;
	send[1] = (command >>  8) & 0xff;
	send[2] = (command) & 0xff;

	dev_dbg(i2c_p->dev, "%s: Send 0x%02x\n", __func__, command);

	if (response) {
		ret = i2c_master_send(i2c_p->client, send, 1);
		if (ret < 0) {
			dev_err(i2c_p->dev,
				"%s: i2c_master_send failed ret = %d\n",
				__func__, ret);
			return ret;
		}

		usleep_range(DBMDX_USLEEP_I2C_VA_CMD_AFTER_SEND,
			DBMDX_USLEEP_I2C_VA_CMD_AFTER_SEND + 1000);
		ret = i2c_master_recv(i2c_p->client, recv, 2);
		if (ret < 0) {
			dev_err(i2c_p->dev, "%s: i2c_master_recv failed\n",
				__func__);
			return ret;
		}
		*response = (recv[0] << 8) | recv[1];

		dev_dbg(i2c_p->dev, "%s: Received 0x%02x\n", __func__,
			*response);

		ret = 0;
	} else {
		ret = i2c_master_send(i2c_p->client, send, 3);
		if (ret < 0) {
			dev_err(i2c_p->dev,
				"%s: i2c_master_send failed ret = %d\n",
				__func__, ret);
			return ret;
		}

		ret = 0;
	}

	usleep_range(DBMDX_USLEEP_I2C_VA_CMD_AFTER_SEND_2,
		DBMDX_USLEEP_I2C_VA_CMD_AFTER_SEND_2 + 1000);

	return ret;
}

ssize_t read_i2c_data(struct dbmdx_private *p, void *buf, size_t len)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;
	size_t count = i2c_p->pdata->read_chunk_size;
	ssize_t i;
	int ret;

	/* We are going to read everything in on chunk */
	if (len < count) {
		ret = i2c_master_recv(i2c_p->client, buf, len);

		if (ret < 0) {
			dev_err(i2c_p->dev, "%s:  i2c_master_recv failed\n",
				__func__);
			i = -EIO;
			goto out;
		}

		return len;
	} else {

		u8 *d = (u8 *)buf;
		/* if stuck for more than 10s, something is wrong */
		unsigned long timeout = jiffies + msecs_to_jiffies(10000);

		for (i = 0; i < len; i += count) {
			if ((i + count) > len)
				count = len - i;

			ret =  i2c_master_recv(i2c_p->client,
				i2c_p->pdata->read_buf, count);
			if (ret < 0) {
				dev_err(i2c_p->dev, "%s: i2c_master_recv failed\n",
					__func__);
				i = -EIO;
				goto out;
			}
			memcpy(d + i, i2c_p->pdata->read_buf, count);

			if (!time_before(jiffies, timeout)) {
				dev_err(i2c_p->dev,
					"%s: read data timed out after %zd bytes\n",
					__func__, i);
				i = -ETIMEDOUT;
				goto out;
			}
		}
		return len;
	}
out:
	return i;
}

ssize_t write_i2c_data(struct dbmdx_private *p, const void *buf,
			      size_t len)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;
	ssize_t ret = 0;
	const u8 *cmds = (const u8 *)buf;
	size_t to_copy = len;
	size_t max_size = (size_t)(i2c_p->pdata->write_chunk_size);

	while (to_copy > 0) {
		ret = i2c_master_send(i2c_p->client, cmds,
				min_t(size_t, to_copy, max_size));
		if (ret < 0) {
			dev_err(i2c_p->dev,
				"%s: i2c_master_send failed ret=%zd\n",
				__func__, ret);
			break;
		}
		to_copy -= ret;
		cmds += ret;
	}

	return len - to_copy;
}

int send_i2c_cmd_boot(struct dbmdx_i2c_private *i2c_p, u32 command)
{
	u8 send[3];
	int ret = 0;

	dev_info(i2c_p->dev, "%s: send_i2c_cmd_boot = %x\n", __func__, command);

	send[0] = (command >> 16) & 0xff;
	send[1] = (command >>  8) & 0xff;

	ret = i2c_master_send(i2c_p->client, send, 2);
	if (ret < 0) {
		dev_err(i2c_p->dev, "%s: i2c_master_send failed ret = %d\n",
			__func__, ret);
		return ret;
	}

	/* A host command received will blocked until the current audio frame
	   processing is finished, which can take up to 10 ms */
	usleep_range(DBMDX_USLEEP_I2C_AFTER_BOOT_CMD,
		DBMDX_USLEEP_I2C_AFTER_BOOT_CMD + 1000);

	return 0;
}

static int i2c_can_boot(struct dbmdx_private *p)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;

	dev_dbg(i2c_p->dev, "%s\n", __func__);
	return 0;
}

static int i2c_prepare_boot(struct dbmdx_private *p)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;

	dev_dbg(i2c_p->dev, "%s\n", __func__);
	return 0;
}


static int i2c_finish_boot(struct dbmdx_private *p)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;

	/* XXX */
	msleep(DBMDX_MSLEEP_I2C_FINISH_BOOT);

	/* change to normal operation I2C address */
	i2c_p->client->addr =  (unsigned short)(i2c_p->pdata->operation_addr);

	i2c_set_clientdata(i2c_p->client, &p->chip);

	dev_dbg(i2c_p->dev, "%s\n", __func__);
	return 0;
}

static int i2c_dump_state(struct dbmdx_private *p, char *buf)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;
	int off = 0;

	dev_dbg(i2c_p->dev, "%s\n", __func__);

	off += sprintf(buf + off, "\t===I2C Interface  Dump====\n");
	off += sprintf(buf + off, "\tI2C Write Chunk Size:\t\t%d\n",
				i2c_p->pdata->write_chunk_size);
	off += sprintf(buf + off, "\tI2C Read Chunk Size:\t\t%d\n",
				i2c_p->pdata->read_chunk_size);
	return off;
}

static int i2c_set_va_firmware_ready(struct dbmdx_private *p)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;

	dev_dbg(i2c_p->dev, "%s\n", __func__);
	return 0;
}


static int i2c_set_vqe_firmware_ready(struct dbmdx_private *p)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;

	dev_dbg(i2c_p->dev, "%s\n", __func__);
	return 0;
}

static void i2c_transport_enable(struct dbmdx_private *p, bool enable)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;

	dev_dbg(i2c_p->dev, "%s (%s)\n", __func__, enable ? "ON" : "OFF");

	if (enable) {
		p->wakeup_set(p);
		msleep(DBMDX_MSLEEP_I2C_WAKEUP);
	} else
		p->wakeup_release(p);
}


static int i2c_prepare_buffering(struct dbmdx_private *p)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;

	dev_dbg(i2c_p->dev, "%s\n", __func__);
	return 0;
}

int i2c_read_audio_data(struct dbmdx_private *p,
		void *buf,
		size_t samples,
		bool to_read_metadata,
		size_t *available_samples,
		size_t *data_offset)
{
	size_t bytes_to_read;
	int ret;
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;

	dev_dbg(i2c_p->dev, "%s\n", __func__);



	ret = send_i2c_cmd_va(p, DBMDX_VA_READ_AUDIO_BUFFER | samples, NULL);

	if (ret) {
		dev_err(p->dev, "%s: failed to request %zu audio samples\n",
			__func__, samples);
		ret = -1;
		goto out;
	}

	*available_samples = 0;

	if (to_read_metadata)
		*data_offset = 8;
	else
		*data_offset = 0;

	bytes_to_read = samples * 8 * p->bytes_per_sample + *data_offset;

	ret = read_i2c_data(p, buf, bytes_to_read);

	if (ret != bytes_to_read) {
		dev_err(p->dev,
			"%s: read audio failed, %zu bytes to read, res(%d)\n",
			__func__,
			bytes_to_read,
			ret);
		ret = -1;
		goto out;
	}

	/* Word #4 contains current number of available samples */
	if (to_read_metadata)
		*available_samples = (size_t)(((u16 *)buf)[3]);
	else
		*available_samples = samples;

	ret = samples;
out:
	return ret;
}

static int i2c_finish_buffering(struct dbmdx_private *p)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;

	dev_dbg(i2c_p->dev, "%s\n", __func__);

	return 0;
}

static int i2c_prepare_amodel_loading(struct dbmdx_private *p)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;

	dev_dbg(i2c_p->dev, "%s\n", __func__);
	return 0;
}

static int i2c_load_amodel(struct dbmdx_private *p,  const void *data,
			   size_t size, size_t gram_size, size_t net_size,
			   const void *checksum, size_t chksum_len,
			   enum dbmdx_load_amodel_mode load_amodel_mode)
{
	int retry = RETRY_COUNT;
	int ret;
	ssize_t send_bytes;
	size_t cur_pos;
	size_t cur_size;
	size_t model_size;
	int model_size_fw;
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;
	u8 rx_checksum[6];

	dev_dbg(i2c_p->dev, "%s\n", __func__);

	model_size = gram_size + net_size + DBMDX_AMODEL_HEADER_SIZE*2;
	model_size_fw = (int)(model_size / 16) + 1;

	while (retry--) {

		if (load_amodel_mode == LOAD_AMODEL_PRIMARY) {
			ret = send_i2c_cmd_va(
					p,
					DBMDX_VA_PRIMARY_AMODEL_SIZE |
					model_size_fw,
					NULL);

			if (ret < 0) {
				dev_err(p->dev,
					"%s: failed to set prim. amodel size\n",
					__func__);
				continue;
			}
		} else if (load_amodel_mode == LOAD_AMODEL_2NDARY) {
			ret = send_i2c_cmd_va(
					p,
					DBMDX_VA_SECONDARY_AMODEL_SIZE |
					model_size_fw,
					NULL);

			if (ret < 0) {
				dev_err(p->dev,
					"%s: failed to set prim. amodel size\n",
					__func__);
				continue;
			}
		}

		ret = send_i2c_cmd_va(
				p,
				DBMDX_VA_LOAD_NEW_ACUSTIC_MODEL |
				load_amodel_mode,
				NULL);

		if (ret < 0) {
			dev_err(p->dev,
				"%s: failed to set fw to receive new amodel\n",
				__func__);
			continue;
		}

		dev_info(p->dev,
			"%s: ---------> acoustic model download start\n",
			__func__);

		cur_size = DBMDX_AMODEL_HEADER_SIZE;
		cur_pos = 0;
		/* Send Gram Header */
		send_bytes = write_i2c_data(p, data, cur_size);

		if (send_bytes != cur_size) {
			dev_err(p->dev,
				"%s: sending of acoustic model data failed\n",
				__func__);
			continue;
		}

		/* wait for FW to process the header */
		usleep_range(DBMDX_USLEEP_AMODEL_HEADER,
			DBMDX_USLEEP_AMODEL_HEADER + 1000);

		cur_pos += DBMDX_AMODEL_HEADER_SIZE;
		cur_size = gram_size;

		/* Send Gram Data */
		send_bytes = write_i2c_data(p, data + cur_pos, cur_size);

		if (send_bytes != cur_size) {
			dev_err(p->dev,
				"%s: sending of acoustic model data failed\n",
				__func__);
			continue;
		}

		cur_pos += gram_size;
		cur_size = DBMDX_AMODEL_HEADER_SIZE;

		/* Send Net Header */
		send_bytes = write_i2c_data(p, data + cur_pos, cur_size);
		if (send_bytes != cur_size) {
			dev_err(p->dev,
				"%s: sending of acoustic model data failed\n",
				__func__);
			continue;
		}

		/* wait for FW to process the header */
		usleep_range(DBMDX_USLEEP_AMODEL_HEADER,
			DBMDX_USLEEP_AMODEL_HEADER + 1000);

		cur_pos += DBMDX_AMODEL_HEADER_SIZE;
		cur_size = net_size;

		/* Send Net Data */
		send_bytes = write_i2c_data(p, data + cur_pos, cur_size);
		if (send_bytes != cur_size) {
			dev_err(p->dev,
				"%s: sending of acoustic model data failed\n",
				__func__);
			continue;
		}

		/* verify checksum */
		if (checksum) {
			ret = send_i2c_cmd_boot(i2c_p, DBMDX_READ_CHECKSUM);
			if (ret < 0) {
				dev_err(i2c_p->dev,
					"%s: could not read checksum\n",
					__func__);
				continue;
			}

			ret = i2c_master_recv(i2c_p->client, rx_checksum, 6);
			if (ret < 0) {
				dev_err(i2c_p->dev,
					"%s: could not read checksum data\n",
					__func__);
				continue;
			}

			ret = p->verify_checksum(p, checksum, &rx_checksum[2],
						 4);
			if (ret) {
				dev_err(p->dev, "%s: checksum mismatch\n",
					__func__);
				continue;
			}
		}
		break;
	}

	/* no retries left, failed to load acoustic */
	if (retry < 0) {
		dev_err(p->dev, "%s: failed to load acoustic model\n",
			__func__);
		return -1;
	}

	/* send boot command */
	ret = send_i2c_cmd_boot(i2c_p, DBMDX_FIRMWARE_BOOT);
	if (ret < 0) {
		dev_err(p->dev, "%s: booting the firmware failed\n", __func__);
		return -1;
	}

	/* wait some time */
	usleep_range(DBMDX_USLEEP_I2C_AFTER_LOAD_AMODEL,
		DBMDX_USLEEP_I2C_AFTER_LOAD_AMODEL + 1000);

	return 0;
}

static int i2c_finish_amodel_loading(struct dbmdx_private *p)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;

	dev_dbg(i2c_p->dev, "%s\n", __func__);

	return 0;
}

static u32 i2c_get_read_chunk_size(struct dbmdx_private *p)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;

	dev_dbg(i2c_p->dev, "%s I2C read chunk is %u\n",
		__func__, i2c_p->pdata->read_chunk_size);

	return i2c_p->pdata->read_chunk_size;
}

static u32 i2c_get_write_chunk_size(struct dbmdx_private *p)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;

	dev_dbg(i2c_p->dev, "%s I2C write chunk is %u\n",
		__func__, i2c_p->pdata->write_chunk_size);

	return i2c_p->pdata->write_chunk_size;
}

static int i2c_set_read_chunk_size(struct dbmdx_private *p, u32 size)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;

	if (size > MAX_I2C_READ_CHUNK_SIZE) {
		dev_err(i2c_p->dev,
			"%s Error setting I2C read chunk. Max chunk size: %u\n",
		__func__, MAX_I2C_READ_CHUNK_SIZE);
		return -1;
	} else if ((size % 2) != 0) {
		dev_err(i2c_p->dev,
			"%s Error setting I2C read chunk. Uneven size\n",
		__func__);
		return -2;
	} else if (size == 0)
		i2c_p->pdata->read_chunk_size = DEFAULT_I2C_READ_CHUNK_SIZE;
	else
		i2c_p->pdata->read_chunk_size = size;

	dev_dbg(i2c_p->dev, "%s I2C read chunk was set to %u\n",
		__func__, i2c_p->pdata->read_chunk_size);

	return 0;
}

static int i2c_set_write_chunk_size(struct dbmdx_private *p, u32 size)
{
	struct dbmdx_i2c_private *i2c_p =
				(struct dbmdx_i2c_private *)p->chip->pdata;

	if (size > MAX_I2C_WRITE_CHUNK_SIZE) {
		dev_err(i2c_p->dev,
			"%s Error setting I2C write chunk. Max chunk size: %u\n",
		__func__, MAX_I2C_WRITE_CHUNK_SIZE);
		return -1;
	} else if ((size % 2) != 0) {
		dev_err(i2c_p->dev,
			"%s Error setting I2C write chunk. Uneven size\n",
		__func__);
		return -2;
	} else if (size == 0)
		i2c_p->pdata->write_chunk_size = DEFAULT_I2C_WRITE_CHUNK_SIZE;
	else
		i2c_p->pdata->write_chunk_size = size;

	dev_dbg(i2c_p->dev, "%s I2C write chunk was set to %u\n",
		__func__, i2c_p->pdata->write_chunk_size);

	return 0;
}
int i2c_common_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
#ifdef CONFIG_OF
	struct  device_node *np;
#endif
	int ret;
	struct dbmdx_i2c_private *p;
	struct dbmdx_i2c_data *pdata;

	dev_dbg(&client->dev, "%s\n", __func__);

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (p == NULL)
		return -ENOMEM;

	p->client = client;
	p->dev = &client->dev;

	p->chip.pdata = p;
#ifdef CONFIG_OF
	np = p->dev->of_node;
	if (!np) {
		dev_err(p->dev, "%s: no devicetree entry\n", __func__);
		ret = -EINVAL;
		goto out_err_kfree;
	}

	pdata = kzalloc(sizeof(struct dbmdx_i2c_data), GFP_KERNEL);
	if (!pdata) {
		ret = -ENOMEM;
		goto out_err_kfree;
	}
#else
	pdata = dev_get_platdata(&client->dev);
	if (pdata == NULL) {
		dev_err(p->dev, "%s: dbmdx, no platform data found\n",
			__func__);
		return -ENODEV;
	}
#endif

	/* remember boot address */
	pdata->boot_addr = (u32)(p->client->addr);

#ifdef CONFIG_OF
	ret = of_property_read_u32(np, "operational-addr",
		&(pdata->operation_addr));
	if (ret != 0) {
		/*
		 * operational address not set, assume it is the same as the
		 * boot address
		 */
		pdata->operation_addr = pdata->boot_addr;
		dev_info(p->dev, "%s: setting operational addr to boot address\n",
			__func__);
	}
#endif
	dev_info(p->dev, "%s: setting operational addr to 0x%2.2x\n",
			__func__, pdata->operation_addr);

#ifdef CONFIG_OF
	ret = of_property_read_u32(np, "read-chunk-size",
		&pdata->read_chunk_size);
	if (ret != 0) {
		/*
		 * read-chunk-size not set, set it to default
		 */
		pdata->read_chunk_size = DEFAULT_I2C_READ_CHUNK_SIZE;
		dev_info(p->dev,
			"%s: Setting i2c read chunk to default val: %u bytes\n",
			__func__, pdata->read_chunk_size);
	}
#endif
	if (pdata->read_chunk_size > MAX_I2C_READ_CHUNK_SIZE)
		pdata->read_chunk_size = MAX_I2C_READ_CHUNK_SIZE;
	if (pdata->read_chunk_size == 0)
		pdata->read_chunk_size = DEFAULT_I2C_READ_CHUNK_SIZE;

	dev_info(p->dev, "%s: Setting i2c read chunk to %u bytes\n",
			__func__, pdata->read_chunk_size);

#ifdef CONFIG_OF
	ret = of_property_read_u32(np, "write-chunk-size",
		&pdata->write_chunk_size);
	if (ret != 0) {
		/*
		 * write-chunk-size not set, set it to default
		 */
		pdata->write_chunk_size = DEFAULT_I2C_WRITE_CHUNK_SIZE;
		dev_info(p->dev,
			"%s: Setting i2c write chunk to default val: %u bytes\n",
			__func__, pdata->write_chunk_size);
	}
#endif
	if (pdata->write_chunk_size > MAX_I2C_WRITE_CHUNK_SIZE)
		pdata->write_chunk_size = MAX_I2C_WRITE_CHUNK_SIZE;
	if (pdata->write_chunk_size == 0)
		pdata->write_chunk_size = DEFAULT_I2C_WRITE_CHUNK_SIZE;

	dev_info(p->dev, "%s: Setting i2c write chunk to %u bytes\n",
			__func__, pdata->write_chunk_size);

	p->pdata = pdata;

	/* fill in chip interface functions */
	p->chip.can_boot = i2c_can_boot;
	p->chip.prepare_boot = i2c_prepare_boot;
	p->chip.finish_boot = i2c_finish_boot;
	p->chip.dump = i2c_dump_state;
	p->chip.set_va_firmware_ready = i2c_set_va_firmware_ready;
	p->chip.set_vqe_firmware_ready = i2c_set_vqe_firmware_ready;
	p->chip.transport_enable = i2c_transport_enable;
	p->chip.read = read_i2c_data;
	p->chip.write = write_i2c_data;
	p->chip.send_cmd_va = send_i2c_cmd_va;
	p->chip.send_cmd_vqe = send_i2c_cmd_vqe;
	p->chip.prepare_buffering = i2c_prepare_buffering;
	p->chip.read_audio_data = i2c_read_audio_data;
	p->chip.finish_buffering = i2c_finish_buffering;
	p->chip.prepare_amodel_loading = i2c_prepare_amodel_loading;
	p->chip.load_amodel = i2c_load_amodel;
	p->chip.finish_amodel_loading = i2c_finish_amodel_loading;
	p->chip.get_write_chunk_size = i2c_get_write_chunk_size;
	p->chip.get_read_chunk_size = i2c_get_read_chunk_size;
	p->chip.set_write_chunk_size = i2c_set_write_chunk_size;
	p->chip.set_read_chunk_size = i2c_set_read_chunk_size;

	i2c_set_clientdata(client, &p->chip);

	dev_info(&client->dev, "%s: successfully probed\n", __func__);
	ret = 0;
	goto out;
#ifdef CONFIG_OF
out_err_kfree:
#endif
	kfree(p);
out:
	return ret;
}

int i2c_common_remove(struct i2c_client *client)
{
	struct chip_interface *ci = i2c_get_clientdata(client);
	struct dbmdx_i2c_private *p = (struct dbmdx_i2c_private *)ci->pdata;

	kfree(p);

	i2c_set_clientdata(client, NULL);

	return 0;
}
