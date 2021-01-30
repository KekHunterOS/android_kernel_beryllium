/*
 *  Bluetooth supports for Qualcomm Atheros chips
 *
 *  Copyright (c) 2017 The Linux Foundation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
#include <linux/module.h>
#include <linux/firmware.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "btqca.h"

#define VERSION "0.1"

#define MAX_PATCH_FILE_SIZE (100*1024)
#define MAX_NVM_FILE_SIZE   (10*1024)

int qca_read_soc_version(struct hci_dev *hdev, u32 *soc_version)
{
	struct sk_buff *skb;
	struct edl_event_hdr *edl;
	struct rome_version *ver;
	char cmd;
	int err = 0;

	bt_dev_dbg(hdev, "QCA Version Request");

	cmd = EDL_PATCH_VER_REQ_CMD;
	skb = __hci_cmd_sync_ev(hdev, EDL_PATCH_CMD_OPCODE, EDL_PATCH_CMD_LEN,
				&cmd, HCI_VENDOR_PKT, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		err = PTR_ERR(skb);
		bt_dev_err(hdev, "Reading QCA version information failed (%d)",
			   err);
		return err;
	}

	if (skb->len != sizeof(*edl) + sizeof(*ver)) {
		bt_dev_err(hdev, "QCA Version size mismatch len %d", skb->len);
		err = -EILSEQ;
		goto out;
	}

	edl = (struct edl_event_hdr *)(skb->data);
	if (!edl) {
		bt_dev_err(hdev, "QCA TLV with no header");
		err = -EILSEQ;
		goto out;
	}

	if (edl->cresp != EDL_CMD_REQ_RES_EVT ||
	    edl->rtype != EDL_APP_VER_RES_EVT) {
		bt_dev_err(hdev, "QCA Wrong packet received %d %d", edl->cresp,
			   edl->rtype);
		err = -EIO;
		goto out;
	}

	ver = (struct rome_version *)(edl->data);

	BT_DBG("%s: Product:0x%08x", hdev->name, le32_to_cpu(ver->product_id));
	BT_DBG("%s: Patch  :0x%08x", hdev->name, le16_to_cpu(ver->patch_ver));
	BT_DBG("%s: ROM    :0x%08x", hdev->name, le16_to_cpu(ver->rome_ver));
	BT_DBG("%s: SOC    :0x%08x", hdev->name, le32_to_cpu(ver->soc_id));

	/* QCA chipset version can be decided by patch and SoC
	 * version, combination with upper 2 bytes from SoC
	 * and lower 2 bytes from patch will be used.
	 */
	*soc_version = (le32_to_cpu(ver->soc_id) << 16) |
		        (le16_to_cpu(ver->rome_ver) & 0x0000ffff);
	if (*soc_version == 0)
		err = -EILSEQ;

out:
	kfree_skb(skb);
	if (err)
		bt_dev_err(hdev, "QCA Failed to get version (%d)", err);

	return err;
}
EXPORT_SYMBOL_GPL(qca_read_soc_version);

static int qca_send_reset(struct hci_dev *hdev)
{
	struct sk_buff *skb;
	int err;

	bt_dev_dbg(hdev, "QCA HCI_RESET");

	skb = __hci_cmd_sync(hdev, HCI_OP_RESET, 0, NULL, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		err = PTR_ERR(skb);
		bt_dev_err(hdev, "QCA Reset failed (%d)", err);
		return err;
	}

	kfree_skb(skb);

	return 0;
}

static void qca_tlv_check_data(struct rome_config *config,
				const struct firmware *fw)
{
	const u8 *data;
	u32 type_len;
	u16 tag_id, tag_len;
	int idx, length;
	struct tlv_type_hdr *tlv;
	struct tlv_type_patch *tlv_patch;
	struct tlv_type_nvm *tlv_nvm;

	tlv = (struct tlv_type_hdr *)fw->data;

	type_len = le32_to_cpu(tlv->type_len);
	length = (type_len >> 8) & 0x00ffffff;

	BT_DBG("TLV Type\t\t : 0x%x", type_len & 0x000000ff);
	BT_DBG("Length\t\t : %d bytes", length);

	switch (config->type) {
	case TLV_TYPE_PATCH:
		tlv_patch = (struct tlv_type_patch *)tlv->data;
		BT_DBG("Total Length\t\t : %d bytes",
		       le32_to_cpu(tlv_patch->total_size));
		BT_DBG("Patch Data Length\t : %d bytes",
		       le32_to_cpu(tlv_patch->data_length));
		BT_DBG("Signing Format Version : 0x%x",
		       tlv_patch->format_version);
		BT_DBG("Signature Algorithm\t : 0x%x",
		       tlv_patch->signature);
		BT_DBG("Reserved\t\t : 0x%x",
		       le16_to_cpu(tlv_patch->reserved1));
		BT_DBG("Product ID\t\t : 0x%04x",
		       le16_to_cpu(tlv_patch->product_id));
		BT_DBG("Rom Build Version\t : 0x%04x",
		       le16_to_cpu(tlv_patch->rom_build));
		BT_DBG("Patch Version\t\t : 0x%04x",
		       le16_to_cpu(tlv_patch->patch_version));
		BT_DBG("Reserved\t\t : 0x%x",
		       le16_to_cpu(tlv_patch->reserved2));
		BT_DBG("Patch Entry Address\t : 0x%x",
		       le32_to_cpu(tlv_patch->entry));
		break;

	case TLV_TYPE_NVM:
		idx = 0;
		data = tlv->data;
		while (idx < length) {
			tlv_nvm = (struct tlv_type_nvm *)(data + idx);

			tag_id = le16_to_cpu(tlv_nvm->tag_id);
			tag_len = le16_to_cpu(tlv_nvm->tag_len);

			/* Update NVM tags as needed */
			switch (tag_id) {
			case EDL_TAG_ID_HCI:
				/* HCI transport layer parameters
				 * enabling software inband sleep
				 * onto controller side.
				 */
				tlv_nvm->data[0] |= 0x80;

				/* UART Baud Rate */
				tlv_nvm->data[2] = config->user_baud_rate;

				break;

			case EDL_TAG_ID_DEEP_SLEEP:
				/* Sleep enable mask
				 * enabling deep sleep feature on controller.
				 */
				tlv_nvm->data[0] |= 0x01;

				break;
			}

			idx += (sizeof(u16) + sizeof(u16) + 8 + tag_len);
		}
		break;

	default:
		BT_ERR("Unknown TLV type %d", config->type);
		break;
	}
}

static int qca_tlv_send_segment(struct hci_dev *hdev, int idx, int seg_size,
				 const u8 *data)
{
	struct sk_buff *skb;
	struct edl_event_hdr *edl;
	struct tlv_seg_resp *tlv_resp;
	u8 cmd[MAX_SIZE_PER_TLV_SEGMENT + 2];
	int err = 0;

	BT_DBG("%s: Download segment #%d size %d", hdev->name, idx, seg_size);

	cmd[0] = EDL_PATCH_TLV_REQ_CMD;
	cmd[1] = seg_size;
	memcpy(cmd + 2, data, seg_size);

	skb = __hci_cmd_sync_ev(hdev, EDL_PATCH_CMD_OPCODE, seg_size + 2, cmd,
				HCI_VENDOR_PKT, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		err = PTR_ERR(skb);
		bt_dev_err(hdev, "QCA Failed to send TLV segment (%d)", err);
		return err;
	}

	if (skb->len != sizeof(*edl) + sizeof(*tlv_resp)) {
		bt_dev_err(hdev, "QCA TLV response size mismatch");
		err = -EILSEQ;
		goto out;
	}

	edl = (struct edl_event_hdr *)(skb->data);
	if (!edl) {
		bt_dev_err(hdev, "TLV with no header");
		err = -EILSEQ;
		goto out;
	}

	tlv_resp = (struct tlv_seg_resp *)(edl->data);

	if (edl->cresp != EDL_CMD_REQ_RES_EVT ||
	    edl->rtype != EDL_TVL_DNLD_RES_EVT || tlv_resp->result != 0x00) {
		bt_dev_err(hdev, "QCA TLV with error stat 0x%x rtype 0x%x (0x%x)",
			   edl->cresp, edl->rtype, tlv_resp->result);
		err = -EIO;
	}

out:
	kfree_skb(skb);

	return err;
}

static int rome_tlv_download_request(struct hci_dev *hdev,
				     const struct firmware *fw)
{
	const u8 *buffer, *data;
	int total_segment, remain_size;
	int ret, i;

	if (!fw || !fw->data)
		return -EINVAL;

	total_segment = fw->size / MAX_SIZE_PER_TLV_SEGMENT;
	remain_size = fw->size % MAX_SIZE_PER_TLV_SEGMENT;

	BT_DBG("%s: Total segment num %d remain size %d total size %zu",
	       hdev->name, total_segment, remain_size, fw->size);

	data = fw->data;
	for (i = 0; i < total_segment; i++) {
		buffer = data + i * MAX_SIZE_PER_TLV_SEGMENT;
		ret = qca_tlv_send_segment(hdev, i, MAX_SIZE_PER_TLV_SEGMENT,
					    buffer);
		if (ret < 0)
			return -EIO;
	}

	if (remain_size) {
		buffer = data + total_segment * MAX_SIZE_PER_TLV_SEGMENT;
		ret = qca_tlv_send_segment(hdev, total_segment, remain_size,
					    buffer);
		if (ret < 0)
			return -EIO;
	}

	return 0;
}

static int qca_download_firmware(struct hci_dev *hdev,
				  struct rome_config *config)
{
	const struct firmware *fw;
	u32 type_len, length;
	struct tlv_type_hdr *tlv;
	int ret;

	BT_INFO("%s: QCA Downloading file: %s", hdev->name, config->fwname);
	ret = request_firmware(&fw, config->fwname, &hdev->dev);

	if (ret || !fw || !fw->data || fw->size <= 0) {
		bt_dev_err(hdev, "QCA Failed to request file: %s (%d)",
			   config->fwname, ret);
		ret = ret ? ret : -EINVAL;
		return ret;
	}

	if (config->type != TLV_TYPE_NVM &&
		config->type != TLV_TYPE_PATCH) {
		ret = -EINVAL;
		BT_ERR("TLV_NVM dload: wrong config type selected");
		goto exit;
	}

	if (config->type == TLV_TYPE_PATCH &&
		(fw->size > MAX_PATCH_FILE_SIZE)) {
		ret = -EINVAL;
		BT_ERR("TLV_PATCH dload: wrong patch file sizes");
		goto exit;
	} else if (config->type == TLV_TYPE_NVM &&
		(fw->size > MAX_NVM_FILE_SIZE)) {
		ret = -EINVAL;
		BT_ERR("TLV_NVM dload: wrong NVM file sizes");
		goto exit;
	}

	if (fw->size < sizeof(struct tlv_type_hdr)) {
		ret = -EINVAL;
		BT_ERR("Firware size smaller to fit minimum value");
		goto exit;
	}

	tlv = (struct tlv_type_hdr *)fw->data;
	type_len = le32_to_cpu(tlv->type_len);
	length = (type_len >> 8) & 0x00ffffff;

	if (fw->size - 4 != length) {
		ret = -EINVAL;
		BT_ERR("Requested size not matching size in header");
		goto exit;
	}

	qca_tlv_check_data(config, fw);
	ret = rome_tlv_download_request(hdev, fw);

	if (ret) {
		BT_ERR("Failed to download FW: error = (%d)", ret);
	}

exit:
	release_firmware(fw);
	return ret;
}

int qca_set_bdaddr_rome(struct hci_dev *hdev, const bdaddr_t *bdaddr)
{
	struct sk_buff *skb;
	u8 cmd[9];
	int err;

	cmd[0] = EDL_NVM_ACCESS_SET_REQ_CMD;
	/* Set the TAG ID of 0x02 for NVM set and size of tag */
	cmd[1] = 0x02;
	cmd[2] = sizeof(bdaddr_t);
	memcpy(cmd + 3, bdaddr, sizeof(bdaddr_t));
	skb = __hci_cmd_sync_ev(hdev, EDL_NVM_ACCESS_OPCODE, sizeof(cmd), cmd,
				HCI_VENDOR_PKT, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		err = PTR_ERR(skb);
		bt_dev_err(hdev, "QCA Change address command failed (%d)", err);
		return err;
	}

	kfree_skb(skb);

	return 0;
}
EXPORT_SYMBOL_GPL(qca_set_bdaddr_rome);

int qca_uart_setup(struct hci_dev *hdev, uint8_t baudrate,
		   enum qca_btsoc_type soc_type, u32 soc_ver)
{
	struct rome_config config;
	int err;

	bt_dev_dbg(hdev, "QCA setup on UART");

	config.user_baud_rate = baudrate;

	bt_dev_info(hdev, "QCA controller version 0x%08x", soc_ver);

	/* Download rampatch file */
	config.type = TLV_TYPE_PATCH;
	snprintf(config.fwname, sizeof(config.fwname), "qca/rampatch_%08x.bin",
		 soc_ver);
	err = qca_download_firmware(hdev, &config);
	if (err < 0) {
		bt_dev_err(hdev, "QCA Failed to download patch (%d)", err);
		return err;
	}

	/* Give the controller some time to get ready to receive the NVM */
	msleep(10);

	/* Download NVM configuration */
	config.type = TLV_TYPE_NVM;
	snprintf(config.fwname, sizeof(config.fwname), "qca/nvm_%08x.bin",
		 soc_ver);
	err = qca_download_firmware(hdev, &config);
	if (err < 0) {
		bt_dev_err(hdev, "QCA Failed to download NVM (%d)", err);
		return err;
	}

	/* Perform HCI reset */
	err = qca_send_reset(hdev);
	if (err < 0) {
		bt_dev_err(hdev, "QCA Failed to run HCI_RESET (%d)", err);
		return err;
	}

	BT_INFO("%s: QCA setup on UART is completed", hdev->name);

	return 0;
}
EXPORT_SYMBOL_GPL(qca_uart_setup);

MODULE_AUTHOR("Ben Young Tae Kim <ytkim@qca.qualcomm.com>");
MODULE_DESCRIPTION("Bluetooth support for Qualcomm Atheros family ver " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL");
