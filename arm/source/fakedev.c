#include <string.h>
#include "fakedev.h"
#include "hci_state.h"
#include "l2cap.h"
#include "syscalls.h"
#include "utils.h"
#include "wiimote.h"

/* Fake devices */
// 00:21:BD:2D:57:FF Name: Nintendo RVL-CNT-01

typedef enum {
	FAKEDEV_BASEBAND_STATE_INACTIVE,
	FAKEDEV_BASEBAND_STATE_REQUEST_CONNECTION,
	FAKEDEV_BASEBAND_STATE_COMPLETE
} fakedev_baseband_state_e;

typedef enum {
	FAKEDEV_ACL_STATE_INACTIVE,
	FAKEDEV_ACL_STATE_LINKING
} fakedev_acl_state_e;

typedef enum {
	FAKEDEV_L2CAP_CHANNEL_STATE_INACTIVE,
	FAKEDEV_L2CAP_CHANNEL_STATE_CONFIG_PEND,
	FAKEDEV_L2CAP_CHANNEL_STATE_COMPLETE
} fakedev_l2cap_channel_state_e;

typedef struct {
	bool valid;
	fakedev_l2cap_channel_state_e state;
	u16 psm;
	u16 local_cid;
	u16 remote_cid;
	u16 remote_mtu;
} l2cap_channel_info_t;

typedef struct {
	bdaddr_t bdaddr;
	u16 hci_con_handle;
	fakedev_baseband_state_e baseband_state;
	fakedev_acl_state_e acl_state;
	l2cap_channel_info_t psm_sdp_chn;
	l2cap_channel_info_t psm_hid_cntl_chn;
	l2cap_channel_info_t psm_hid_intr_chn;
} fakedev_t;

static fakedev_t fakedev = {
	.bdaddr = {.b = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66}},
	.baseband_state = FAKEDEV_BASEBAND_STATE_INACTIVE,
	.acl_state = FAKEDEV_ACL_STATE_INACTIVE,
};

/* Helper functions */

static inline bool fakedev_is_connected(void)
{
	return fakedev.baseband_state == FAKEDEV_BASEBAND_STATE_COMPLETE;
}

/* Channel bookkeeping */

static inline u16 generate_l2cap_channel_id(void)
{
	/* "Identifiers from 0x0001 to 0x003F are reserved" */
	static u16 starting_id = 0x40;
	return starting_id++;
}

static inline bool l2cap_channel_is_accepted(const l2cap_channel_info_t *info)
{
	return info->valid && (info->remote_cid != L2CAP_NULL_CID);
}

static inline bool l2cap_channel_is_is_remote_configured(const l2cap_channel_info_t *info)
{
	return info->valid && (info->remote_mtu != 0);
}

static inline bool l2cap_channel_is_complete(const l2cap_channel_info_t *info)
{
	return info->valid &&
	       l2cap_channel_is_accepted(info) &&
	       l2cap_channel_is_is_remote_configured(info) &&
	       (info->state == FAKEDEV_L2CAP_CHANNEL_STATE_COMPLETE);
}

static l2cap_channel_info_t *get_channel_info(fakedev_t *dev, u16 local_cid)
{
	if (dev->psm_sdp_chn.valid && (local_cid == dev->psm_sdp_chn.local_cid)) {
		return &dev->psm_sdp_chn;
	} else if (dev->psm_hid_cntl_chn.valid && (local_cid == dev->psm_hid_cntl_chn.local_cid)) {
		return &dev->psm_hid_cntl_chn;
	} else if (dev->psm_hid_intr_chn.valid && (local_cid == dev->psm_hid_intr_chn.local_cid)) {
		return &dev->psm_hid_intr_chn;
	}
	return NULL;
}

static void l2cap_channel_info_setup(l2cap_channel_info_t *info, u16 psm, u16 local_cid)
{
	info->psm = psm;
	info->state = FAKEDEV_L2CAP_CHANNEL_STATE_INACTIVE;
	info->local_cid = local_cid;
	info->remote_cid = L2CAP_NULL_CID;
	info->remote_mtu = 0;
	info->valid = true;
}

/* HID reports */

static int send_hid_data(u16 hci_con_handle, u16 dcid, u8 hid_type, const void *data, u32 size)
{
	u8 buf[WIIMOTE_MAX_PAYLOAD];
	assert(size <= (WIIMOTE_MAX_PAYLOAD - 1));
	buf[0] = hid_type;
	memcpy(&buf[1], data, size);
	return l2cap_send_msg(hci_con_handle, dcid, buf, size + 1);
}

static inline int send_hid_input_report(u16 hci_con_handle, u16 dcid, u8 report_id,
					const void *data, u32 size)
{
	u8 buf[WIIMOTE_MAX_PAYLOAD - 1];
	assert(size <= (WIIMOTE_MAX_PAYLOAD - 2));
	buf[0] = report_id;
	memcpy(&buf[1], data, size);
	return send_hid_data(hci_con_handle, dcid, (HID_TYPE_DATA << 4) | HID_PARAM_INPUT, buf, size + 1);
}

static int wiimote_send_ack(u8 rpt_id, u8 error_code)
{
	struct wiimote_input_report_ack_t ack ATTRIBUTE_ALIGN(32);
	ack.buttons = 0;
	ack.rpt_id = rpt_id;
	ack.error_code = error_code;
	return send_hid_input_report(fakedev.hci_con_handle,
				     fakedev.psm_hid_intr_chn.remote_cid,
				     INPUT_REPORT_ID_ACK, &ack, sizeof(ack));
}

/* Init state */

void fakedev_init(void)
{
	fakedev.baseband_state = FAKEDEV_BASEBAND_STATE_INACTIVE;
	fakedev.acl_state = FAKEDEV_L2CAP_CHANNEL_STATE_INACTIVE;
	fakedev.psm_sdp_chn.valid = false;
	fakedev.psm_hid_cntl_chn.valid = false;
	fakedev.psm_hid_intr_chn.valid = false;

	/* Activate */
	fakedev.baseband_state = FAKEDEV_BASEBAND_STATE_REQUEST_CONNECTION;
}

static void check_send_config_for_new_channel(l2cap_channel_info_t *info)
{
	int ret;

	if (l2cap_channel_is_accepted(info) &&
	    (info->state == FAKEDEV_L2CAP_CHANNEL_STATE_INACTIVE)) {
		ret = l2cap_send_config_req(fakedev.hci_con_handle, info->remote_cid,
					    WII_REQUEST_MTU, L2CAP_FLUSH_TIMO_DEFAULT);
		if (ret == IOS_OK) {
			info->state = FAKEDEV_L2CAP_CHANNEL_STATE_CONFIG_PEND;
		}
	}
}

void fakedev_tick_devices(void)
{
	int ret;
	bool req;

	if (fakedev.baseband_state == FAKEDEV_BASEBAND_STATE_REQUEST_CONNECTION) {
		req = hci_request_connection(&fakedev.bdaddr, WIIMOTE_HCI_CLASS_0,
					     WIIMOTE_HCI_CLASS_1, WIIMOTE_HCI_CLASS_2,
					     HCI_LINK_ACL);
		/* After a connection request is visible to the controller switch to inactive */
		if (req)
			fakedev.baseband_state = FAKEDEV_BASEBAND_STATE_INACTIVE;
	}

	if (!fakedev_is_connected())
		return;

	/*  Send configuration for any newly connected channels. */
	check_send_config_for_new_channel(&fakedev.psm_sdp_chn);
	check_send_config_for_new_channel(&fakedev.psm_hid_cntl_chn);
	check_send_config_for_new_channel(&fakedev.psm_hid_intr_chn);

	/* "If the connection originated from the device (Wiimote) it will create
	 * HID control and interrupt channels (in that order)." */
	if (fakedev.acl_state == FAKEDEV_ACL_STATE_LINKING) {
		if (!fakedev.psm_hid_cntl_chn.valid) {
			u16 local_cid = generate_l2cap_channel_id();
			ret = l2cap_send_connect_req(fakedev.hci_con_handle, L2CAP_PSM_HID_CNTL,
						     local_cid);
			assert(ret == IOS_OK);
			l2cap_channel_info_setup(&fakedev.psm_hid_cntl_chn, L2CAP_PSM_HID_CNTL, local_cid);
			DEBUG("Generated local CID for HID CNTL: 0x%x\n", local_cid);
		}

		if (!fakedev.psm_hid_intr_chn.valid) {
			u16 local_cid = generate_l2cap_channel_id();
			ret = l2cap_send_connect_req(fakedev.hci_con_handle, L2CAP_PSM_HID_INTR,
						     local_cid);
			assert(ret == IOS_OK);
			l2cap_channel_info_setup(&fakedev.psm_hid_intr_chn, L2CAP_PSM_HID_INTR, local_cid);
			DEBUG("Generated local CID for HID INTR: 0x%x\n", local_cid);
		}

		if (l2cap_channel_is_complete(&fakedev.psm_hid_cntl_chn) &&
		    l2cap_channel_is_complete(&fakedev.psm_hid_intr_chn))
			fakedev.acl_state = FAKEDEV_ACL_STATE_INACTIVE;
	}

	if (l2cap_channel_is_complete(&fakedev.psm_hid_intr_chn)) {
		DEBUG("Faking buttons...\n");
		static u16 buttons = 0;
		buttons ^= 0x80;
		send_hid_input_report(fakedev.hci_con_handle, fakedev.psm_hid_intr_chn.remote_cid,
			INPUT_REPORT_ID_REPORT_CORE, &buttons, sizeof(buttons));
	}
}

/* Functions called by the HCI state manager */

bool fakedev_handle_hci_cmd_accept_con(const bdaddr_t *bdaddr, u8 role)
{
	int ret;

	/* Check if this bdaddr belongs to a fake device */
	if (memcmp(bdaddr, &fakedev.bdaddr, sizeof(bdaddr_t) != 0))
		return false;

	/* Connection accepted to our fake device */
	DEBUG("Connection accepted for our fake device!\n");

	/* The Accept_Connection_Request command will cause the Command Status
	   event to be sent from the Host Controller when the Host Controller
	   begins setting up the connection */

	ret = enqueue_hci_event_command_status(HCI_CMD_ACCEPT_CON);
	assert(ret == IOS_OK);

	fakedev.baseband_state = FAKEDEV_BASEBAND_STATE_COMPLETE;
	fakedev.hci_con_handle = hci_con_handle_virt_alloc();
	DEBUG("Our fake device got HCI con_handle: 0x%x\n", fakedev.hci_con_handle);

	/* We can start the ACL (L2CAP) linking now */
	fakedev.acl_state = FAKEDEV_ACL_STATE_LINKING;

	if (role == HCI_ROLE_MASTER) {
		ret = enqueue_hci_event_role_change(bdaddr, HCI_ROLE_MASTER);
		assert(ret == IOS_OK);
	}

	/* In addition, when the Link Manager determines the connection is established,
	 * the Host Controllers on both Bluetooth devices that form the connection
	 * will send a Connection Complete event to each Host */
	ret = enqueue_hci_event_con_compl(bdaddr, fakedev.hci_con_handle, 0);
	assert(ret == IOS_OK);

	DEBUG("Connection complete sent, starting ACL linking!\n");

	return true;
}

bool fakedev_handle_hci_cmd_from_host(u16 hci_con_handle, const hci_cmd_hdr_t *hdr)
{
	/* Check if this HCI connection handle belongs to a fake device */
	if (!fakedev_is_connected() || (fakedev.hci_con_handle != hci_con_handle))
		return false;

	/* TODO */
	DEBUG("FAKEDEV H > C HCI CMD: 0x%x\n", hci_con_handle);

	return true;
}

static void handle_l2cap_config_req(u8 ident, u16 dcid, u16 flags, const u8 *options, u16 options_size)
{
	u8 tmp[256];
	u32 opt_size;
	l2cap_channel_info_t *info;
	l2cap_cfg_opt_t *opt;
	l2cap_cfg_opt_val_t *val;
	l2cap_cfg_rsp_cp *rsp = (l2cap_cfg_rsp_cp *)tmp;
	u32 offset = 0;
	u32 resp_len = 0;
	/* If the option is not provided, configure the default. */
	u16 remote_mtu = L2CAP_MTU_DEFAULT;

	assert(flags == 0x00);
	assert(options_size <= sizeof(tmp));

	info = get_channel_info(&fakedev, dcid);
	assert(info);

	/* Response to the config request */
	rsp->scid = htole16(info->remote_cid);
	rsp->flags = htole16(0x00);
	rsp->result = htole16(L2CAP_SUCCESS);
	resp_len += sizeof(l2cap_cfg_rsp_cp);

	/* Read configuration options. */
	while (offset < options_size) {
		opt = (l2cap_cfg_opt_t *)&options[offset];
		offset += sizeof(l2cap_cfg_opt_t);
		val = (l2cap_cfg_opt_val_t *)&options[offset];

		switch (opt->type) {
		case L2CAP_OPT_MTU:
			assert(opt->length == L2CAP_OPT_MTU_SIZE);
			remote_mtu = le16toh(val->mtu);
			DEBUG("      MTU configured to: 0x%x\n", remote_mtu);
			break;
		/* We don't care what the flush timeout is. Our packets are not dropped. */
		case L2CAP_OPT_FLUSH_TIMO:
			assert(opt->length == L2CAP_OPT_FLUSH_TIMO_SIZE);
			DEBUG("      Flush timeout configured to 0x%x\n", val->flush_timo);
			break;
		default:
			DEBUG("      Unknown Option: 0x%02x", opt->type);
			break;
		}

		offset += opt->length;
		opt_size = sizeof(l2cap_cfg_opt_t) + opt->length;
		memcpy(&tmp[resp_len], options, opt_size);
		resp_len += opt_size;
	}

	/* Send Respone */
	l2cap_send_config_rsp(fakedev.hci_con_handle, dcid, ident, tmp, resp_len);

	/* Set the MTU */
	info->remote_mtu = remote_mtu;
}

static void handle_l2cap_signal_channel(u8 code, u8 ident, const void *payload, u16 size)
{
	l2cap_channel_info_t *info;

	DEBUG("  signal channel: code: 0x%x, ident: 0x%x\n", code, ident);

	switch (code) {
	case L2CAP_CONNECT_REQ: {
		const l2cap_con_req_cp *req = payload;
		u16 psm = le16toh(req->psm);
		u16 scid = le16toh(req->scid);
		DEBUG("  L2CAP_CONNECT_REQ: psm: 0x%x, scid: 0x%x\n", psm, scid);
		break;
	}
	case L2CAP_CONNECT_RSP: {
		const l2cap_con_rsp_cp *rsp = payload;
		u16 dcid = le16toh(rsp->dcid);
		u16 scid = le16toh(rsp->scid);
		u16 result = le16toh(rsp->result);
		u16 status = le16toh(rsp->status);
		DEBUG("  L2CAP_CONNECT_RSP: dcid: 0x%x, scid: 0x%x, result: 0x%x, status: 0x%x\n",
			dcid, scid, result, status);

		assert(result == L2CAP_SUCCESS);
		assert(status == L2CAP_NO_INFO);
		info = get_channel_info(&fakedev, scid);
		assert(info);

		/* Save endpoint's Destination CID  */
		info->remote_cid = dcid;
		break;
	}
	case L2CAP_CONFIG_REQ: {
		const l2cap_cfg_req_cp *rsp = payload;
		u16 dcid = le16toh(rsp->dcid);
		u16 flags = le16toh(rsp->flags);
		const void *options = (const void *)((u8 *)rsp + sizeof(l2cap_cfg_req_cp));

		DEBUG("  L2CAP_CONFIG_REQ: dcid: 0x%x, flags: 0x%x\n", dcid, flags);
		handle_l2cap_config_req(ident, dcid, flags, options, size - sizeof(l2cap_cfg_req_cp));
		break;
	}
	case L2CAP_CONFIG_RSP: {
		const l2cap_cfg_rsp_cp *rsp = payload;
		u16 scid = le16toh(rsp->scid);
		u16 flags = le16toh(rsp->flags);
		u16 result = le16toh(rsp->result);
		DEBUG("  L2CAP_CONFIG_RSP: scid: 0x%x, flags: 0x%x, result: 0x%x\n",
			scid, flags, result);

		assert(result == L2CAP_SUCCESS);
		info = get_channel_info(&fakedev, scid);
		assert(info);

		/* Mark channel as complete!  */
		info->state = FAKEDEV_L2CAP_CHANNEL_STATE_COMPLETE;
		break;
	}
	}
}

static void handle_l2cap_signal_channel_request(const void *data, u16 length)
{
	const l2cap_cmd_hdr_t *cmd_hdr;
	const void *cmd_payload;
	u16 cmd_len;

	while (length >= sizeof(l2cap_cmd_hdr_t)) {
		cmd_hdr = (const void *)data;
		cmd_len = le16toh(cmd_hdr->length);
		cmd_payload = (const void *)((u8 *)data + sizeof(*cmd_hdr));

		handle_l2cap_signal_channel(cmd_hdr->code, cmd_hdr->ident,
					    cmd_payload, cmd_len);

		data += sizeof(l2cap_cmd_hdr_t) + cmd_len;
		length -= sizeof(l2cap_cmd_hdr_t) + cmd_len;
	}
}

static void handle_hid_intr_data_output(const u8 *data, u16 size)
{
	DEBUG("handle_hid_intr_data_output: size: 0x%x, 0x%x\n", size, *(u32 *)(data-1));

	if (size == 0)
		return;

	switch (data[0]) {
	case OUTPUT_REPORT_ID_LED: {
		struct wiimote_output_report_led_t *led = (void *)&data[1];
		DEBUG("  LED: 0x%x\n", led->leds);
		if (led->ack)
			wiimote_send_ack(OUTPUT_REPORT_ID_LED, ERROR_CODE_SUCCESS);
		break;
	}
	case OUTPUT_REPORT_ID_STATUS: {
		struct wiimote_input_report_status_t status;
		memset(&status, 0, sizeof(status));
		status.buttons = 0;
		send_hid_input_report(fakedev.hci_con_handle,
				     fakedev.psm_hid_intr_chn.remote_cid,
				     INPUT_REPORT_ID_STATUS, &status, sizeof(status));
		break;
	}
	case OUTPUT_REPORT_ID_REPORT_MODE: {
		struct wiimote_output_report_mode_t *mode = (void *)&data[1];
		DEBUG("  Report mode: 0x%02x, cont: %d, rumble: %d, ack: %d\n",
			mode->mode, mode->continuous, mode->rumble, mode->ack);
		if (mode->ack)
			wiimote_send_ack(OUTPUT_REPORT_ID_REPORT_MODE, ERROR_CODE_SUCCESS);
		break;
	}
	case OUTPUT_REPORT_ID_READ_DATA: {
		struct wiimote_output_report_read_data_t *read = (void *)&data[1];
		struct wiimote_input_report_read_data_t reply;
		memset(&reply, 0, sizeof(reply));
		reply.buttons = 0;
		reply.size_minus_one = read->size - 1;
		reply.error = ERROR_CODE_SUCCESS;
		reply.address = read->address;
		send_hid_input_report(fakedev.hci_con_handle,
				     fakedev.psm_hid_intr_chn.remote_cid,
				     INPUT_REPORT_ID_READ_DATA_REPLY, &reply, sizeof(reply));
		break;
	}
	default:
		DEBUG("Unhandled output report: 0x%x\n", data[0]);
		break;
	}
}

bool fakedev_handle_acl_data_out_request_from_host(u16 hci_con_handle, const hci_acldata_hdr_t *acl)
{
	const l2cap_hdr_t *header;
	u16 dcid, length;
	const u8 *payload;

	/* Check if this HCI connection handle belongs to a fake device */
	if (!fakedev_is_connected() || (fakedev.hci_con_handle != hci_con_handle))
		return false;

	/* L2CAP header */
	header  = (const void *)((u8 *)acl + sizeof(hci_acldata_hdr_t));
	length  = le16toh(header->length);
	dcid    = le16toh(header->dcid);
	payload = (u8 *)header + sizeof(l2cap_hdr_t);

	DEBUG("FD ACL OUT: con_handle: 0x%x, dcid: 0x%x, len: 0x%x\n", hci_con_handle, dcid, length);

	if (dcid == L2CAP_SIGNAL_CID) {
		handle_l2cap_signal_channel_request(payload, length);
	} else {
		l2cap_channel_info_t *info = get_channel_info(&fakedev, dcid);
		if (info) {
			switch (info->psm) {
			case L2CAP_PSM_SDP:
				/* TODO */
				DEBUG("  PSM HID SDP\n");
				break;
			case L2CAP_PSM_HID_CNTL:
				/* TODO */
				DEBUG("  PSM HID CNTL\n");
				break;
			case L2CAP_PSM_HID_INTR:
				if (payload[0] == ((HID_TYPE_DATA << 4) | HID_PARAM_OUTPUT))
					handle_hid_intr_data_output(&payload[1], length - 1);
				break;
			}
		} else {
			DEBUG("Received L2CAP packet to unknown channel: 0x%x\n", dcid);
		}
	}

	return true;
}

void fakedev_handle_hci_event_request_from_host(u32 length)
{

}

void fakedev_handle_acl_data_in_request_from_host(u32 length)
{

}