/*
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2011  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <poll.h>
#include <getopt.h>
#include <stdbool.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <glib.h>

#include "src/shared/mgmt.h"
#include "glib-helper.h"
#include "lib/mgmt.h"
#include "eir.h"

static GMainLoop *event_loop = NULL;

static bool monitor = false;
static bool discovery = false;
static bool resolve_names = true;

static int pending = 0;

static void controller_error(uint16_t index, uint16_t len,
				const void *param, void *user_data)
{
	const struct mgmt_ev_controller_error *ev = param;

	if (len < sizeof(*ev)) {
		fprintf(stderr,
			"Too short (%u bytes) controller error event\n", len);
		return;
	}

	if (monitor)
		printf("hci%u error 0x%02x\n", index, ev->error_code);
}

static void index_added(uint16_t index, uint16_t len,
				const void *param, void *user_data)
{
	if (monitor)
		printf("hci%u added\n", index);
}

static void index_removed(uint16_t index, uint16_t len,
				const void *param, void *user_data)
{
	if (monitor)
		printf("hci%u removed\n", index);
}

static const char *settings_str[] = {
				"powered",
				"connectable",
				"fast-connectable",
				"discoverable",
				"pairable",
				"link-security",
				"ssp",
				"br/edr",
				"hs",
				"le" ,
};

static void print_settings(uint32_t settings)
{
	unsigned i;

	for (i = 0; i < NELEM(settings_str); i++) {
		if ((settings & (1 << i)) != 0)
			printf("%s ", settings_str[i]);
	}
}

static void new_settings(uint16_t index, uint16_t len,
					const void *param, void *user_data)
{
	const uint32_t *ev = param;

	if (len < sizeof(*ev)) {
		fprintf(stderr, "Too short new_settings event (%u)\n", len);
		return;
	}

	if (monitor) {
		printf("hci%u new_settings: ", index);
		print_settings(bt_get_le32(ev));
		printf("\n");
	}
}

static void discovering(uint16_t index, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_ev_discovering *ev = param;

	if (len < sizeof(*ev)) {
		fprintf(stderr, "Too short (%u bytes) discovering event\n",
									len);
		return;
	}

	if (ev->discovering == 0 && discovery)
		g_main_loop_quit(event_loop);

	if (monitor)
		printf("hci%u type %u discovering %s\n", index,
				ev->type, ev->discovering ? "on" : "off");
}

static void new_link_key(uint16_t index, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_ev_new_link_key *ev = param;

	if (len != sizeof(*ev)) {
		fprintf(stderr, "Invalid new_link_key length (%u bytes)\n",
									len);
		return;
	}

	if (monitor) {
		char addr[18];
		ba2str(&ev->key.addr.bdaddr, addr);
		printf("hci%u new_link_key %s type 0x%02x pin_len %d "
				"store_hint %u\n", index, addr, ev->key.type,
				ev->key.pin_len, ev->store_hint);
	}
}

static const char *typestr(uint8_t type)
{
	const char *str[] = { "BR/EDR", "LE Public", "LE Random" };

	if (type <= BDADDR_LE_RANDOM)
		return str[type];

	return "(unknown)";
}

static void connected(uint16_t index, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_ev_device_connected *ev = param;
	uint16_t eir_len;

	if (len < sizeof(*ev)) {
		fprintf(stderr,
			"Invalid connected event length (%u bytes)\n", len);
		return;
	}

	eir_len = bt_get_le16(&ev->eir_len);
	if (len != sizeof(*ev) + eir_len) {
		fprintf(stderr, "Invalid connected event length "
			"(%u bytes, eir_len %u bytes)\n", len, eir_len);
		return;
	}

	if (monitor) {
		char addr[18];
		ba2str(&ev->addr.bdaddr, addr);
		printf("hci%u %s type %s connected eir_len %u\n", index, addr,
					typestr(ev->addr.type), eir_len);
	}
}

static void disconnected(uint16_t index, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_ev_device_disconnected *ev = param;

	if (len < sizeof(struct mgmt_addr_info)) {
		fprintf(stderr,
			"Invalid disconnected event length (%u bytes)\n", len);
		return;
	}

	if (monitor) {
		char addr[18];
		uint8_t reason;

		if (len < sizeof(*ev))
			reason = MGMT_DEV_DISCONN_UNKNOWN;
		else
			reason = ev->reason;

		ba2str(&ev->addr.bdaddr, addr);
		printf("hci%u %s type %s disconnected with reason %u\n",
				index, addr, typestr(ev->addr.type), reason);
	}
}

static void conn_failed(uint16_t index, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_ev_connect_failed *ev = param;

	if (len != sizeof(*ev)) {
		fprintf(stderr,
			"Invalid connect_failed event length (%u bytes)\n", len);
		return;
	}

	if (monitor) {
		char addr[18];
		ba2str(&ev->addr.bdaddr, addr);
		printf("hci%u %s type %s connect failed (status 0x%02x, %s)\n",
				index, addr, typestr(ev->addr.type), ev->status,
				mgmt_errstr(ev->status));
	}
}

static void auth_failed(uint16_t index, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_ev_auth_failed *ev = param;

	if (len != sizeof(*ev)) {
		fprintf(stderr,
			"Invalid auth_failed event length (%u bytes)\n", len);
		return;
	}

	if (monitor) {
		char addr[18];
		ba2str(&ev->addr.bdaddr, addr);
		printf("hci%u %s auth failed with status 0x%02x (%s)\n",
			index, addr, ev->status, mgmt_errstr(ev->status));
	}
}

static void local_name_changed(uint16_t index, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_ev_local_name_changed *ev = param;

	if (len != sizeof(*ev)) {
		fprintf(stderr,
			"Invalid local_name_changed length (%u bytes)\n", len);
		return;
	}

	if (monitor)
		printf("hci%u name changed: %s\n", index, ev->name);
}

static void confirm_name_rsp(uint8_t status, uint16_t len,
					const void *param, void *user_data)
{
	const struct mgmt_rp_confirm_name *rp = param;
	char addr[18];

	if (len == 0 && status != 0) {
		fprintf(stderr,
			"confirm_name failed with status 0x%02x (%s)\n",
					status, mgmt_errstr(status));
		return;
	}

	if (len != sizeof(*rp)) {
		fprintf(stderr, "confirm_name rsp length %u instead of %zu\n",
			len, sizeof(*rp));
		return;
	}

	ba2str(&rp->addr.bdaddr, addr);

	if (status != 0)
		fprintf(stderr, "confirm_name for %s failed: 0x%02x (%s)\n",
			addr, status, mgmt_errstr(status));
	else
		printf("confirm_name succeeded for %s\n", addr);
}

static void device_found(uint16_t index, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_ev_device_found *ev = param;
	struct mgmt *mgmt = user_data;
	uint32_t flags;
	uint16_t eir_len;
	struct eir_data eir;

	if (len < sizeof(*ev)) {
		fprintf(stderr,
			"Too short device_found length (%u bytes)\n", len);
		return;
	}

	flags = btohl(ev->flags);

	eir_len = bt_get_le16(&ev->eir_len);
	if (len != sizeof(*ev) + eir_len) {
		fprintf(stderr, "dev_found: expected %zu bytes, got %u bytes\n",
						sizeof(*ev) + eir_len, len);
		return;
	}

	memset(&eir, 0, sizeof(eir));
	eir_parse(&eir, ev->eir, eir_len);

	if (monitor || discovery) {
		char addr[18];
		ba2str(&ev->addr.bdaddr, addr);
		printf("hci%u dev_found: %s type %s rssi %d "
			"flags 0x%04x ", index, addr,
			typestr(ev->addr.type), ev->rssi, flags);

		if (eir.name)
			printf("name %s\n", eir.name);
		else
			printf("eir_len %u\n", eir_len);
	}

	eir_data_free(&eir);

	if (discovery && (flags & MGMT_DEV_FOUND_CONFIRM_NAME)) {
		struct mgmt_cp_confirm_name cp;

		memset(&cp, 0, sizeof(cp));
		memcpy(&cp.addr, &ev->addr, sizeof(cp.addr));
		if (resolve_names)
			cp.name_known = 0;
		else
			cp.name_known = 1;

		mgmt_reply(mgmt, MGMT_OP_CONFIRM_NAME, index, sizeof(cp), &cp,
						confirm_name_rsp, NULL, NULL);
	}
}

static void pin_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	if (status != 0) {
		fprintf(stderr,
			"PIN Code reply failed with status 0x%02x (%s)\n",
						status, mgmt_errstr(status));
		g_main_loop_quit(event_loop);
		return;
	}

	printf("PIN Reply successful\n");
}

static int mgmt_pin_reply(struct mgmt *mgmt, uint16_t index,
					const struct mgmt_addr_info *addr,
					const char *pin, size_t len)
{
	struct mgmt_cp_pin_code_reply cp;

	memset(&cp, 0, sizeof(cp));
	memcpy(&cp.addr, addr, sizeof(cp.addr));
	cp.pin_len = len;
	memcpy(cp.pin_code, pin, len);

	return mgmt_reply(mgmt, MGMT_OP_PIN_CODE_REPLY, index, sizeof(cp), &cp,
							pin_rsp, NULL, NULL);
}

static void pin_neg_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	if (status != 0) {
		fprintf(stderr,
			"PIN Neg reply failed with status 0x%02x (%s)\n",
						status, mgmt_errstr(status));
		g_main_loop_quit(event_loop);
		return;
	}

	printf("PIN Negative Reply successful\n");
}

static int mgmt_pin_neg_reply(struct mgmt *mgmt, uint16_t index,
					const struct mgmt_addr_info *addr)
{
	struct mgmt_cp_pin_code_neg_reply cp;

	memset(&cp, 0, sizeof(cp));
	memcpy(&cp.addr, addr, sizeof(cp.addr));

	return mgmt_reply(mgmt, MGMT_OP_PIN_CODE_NEG_REPLY, index,
				sizeof(cp), &cp, pin_neg_rsp, NULL, NULL);
}

static void request_pin(uint16_t index, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_ev_pin_code_request *ev = param;
	struct mgmt *mgmt = user_data;
	char pin[18];
	size_t pin_len;

	if (len != sizeof(*ev)) {
		fprintf(stderr,
			"Invalid pin_code request length (%u bytes)\n", len);
		return;
	}

	if (monitor) {
		char addr[18];
		ba2str(&ev->addr.bdaddr, addr);
		printf("hci%u %s request PIN\n", index, addr);
	}

	printf("PIN Request (press enter to reject) >> ");
	fflush(stdout);

	memset(pin, 0, sizeof(pin));

	if (fgets(pin, sizeof(pin), stdin) == NULL || pin[0] == '\n') {
		mgmt_pin_neg_reply(mgmt, index, &ev->addr);
		return;
	}

	pin_len = strlen(pin);
	if (pin[pin_len - 1] == '\n') {
		pin[pin_len - 1] = '\0';
		pin_len--;
	}

	mgmt_pin_reply(mgmt, index, &ev->addr, pin, pin_len);
}

static void confirm_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	if (status != 0) {
		fprintf(stderr,
			"User Confirm reply failed. status 0x%02x (%s)\n",
						status, mgmt_errstr(status));
		g_main_loop_quit(event_loop);
		return;
	}

	printf("User Confirm Reply successful\n");
}

static int mgmt_confirm_reply(struct mgmt *mgmt, uint16_t index,
							const bdaddr_t *bdaddr)
{
	struct mgmt_cp_user_confirm_reply cp;

	memset(&cp, 0, sizeof(cp));
	bacpy(&cp.addr.bdaddr, bdaddr);

	return mgmt_reply(mgmt, MGMT_OP_USER_CONFIRM_REPLY, index,
				sizeof(cp), &cp, confirm_rsp, NULL, NULL);
}

static void confirm_neg_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	if (status != 0) {
		fprintf(stderr,
			"Confirm Neg reply failed. status 0x%02x (%s)\n",
						status, mgmt_errstr(status));
		g_main_loop_quit(event_loop);
		return;
	}

	printf("User Confirm Negative Reply successful\n");
}

static int mgmt_confirm_neg_reply(struct mgmt *mgmt, uint16_t index,
							const bdaddr_t *bdaddr)
{
	struct mgmt_cp_user_confirm_reply cp;

	memset(&cp, 0, sizeof(cp));
	bacpy(&cp.addr.bdaddr, bdaddr);

	return mgmt_reply(mgmt, MGMT_OP_USER_CONFIRM_NEG_REPLY, index,
				sizeof(cp), &cp, confirm_neg_rsp, NULL, NULL);
}


static void user_confirm(uint16_t index, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_ev_user_confirm_request *ev = param;
	struct mgmt *mgmt = user_data;
	char rsp[5];
	size_t rsp_len;
	uint32_t val;
	char addr[18];

	if (len != sizeof(*ev)) {
		fprintf(stderr,
			"Invalid user_confirm request length (%u)\n", len);
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);
	val = bt_get_le32(&ev->value);

	if (monitor)
		printf("hci%u %s User Confirm %06u hint %u\n", index, addr,
							val, ev->confirm_hint);

	if (ev->confirm_hint)
		printf("Accept pairing with %s (yes/no) >> ", addr);
	else
		printf("Confirm value %06u for %s (yes/no) >> ", val, addr);

	fflush(stdout);

	memset(rsp, 0, sizeof(rsp));

	if (fgets(rsp, sizeof(rsp), stdin) == NULL || rsp[0] == '\n') {
		mgmt_confirm_neg_reply(mgmt, index, &ev->addr.bdaddr);
		return;
	}

	rsp_len = strlen(rsp);
	if (rsp[rsp_len - 1] == '\n') {
		rsp[rsp_len - 1] = '\0';
		rsp_len--;
	}

	if (rsp[0] == 'y' || rsp[0] == 'Y')
		mgmt_confirm_reply(mgmt, index, &ev->addr.bdaddr);
	else
		mgmt_confirm_neg_reply(mgmt, index, &ev->addr.bdaddr);
}

static void cmd_monitor(struct mgmt *mgmt, uint16_t index, int argc,
								char **argv)
{
	printf("Monitoring mgmt events...\n");
	monitor = true;
}

static void version_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_rp_read_version *rp = param;

	if (status != 0) {
		fprintf(stderr, "Reading mgmt version failed with status"
			" 0x%02x (%s)\n", status, mgmt_errstr(status));
		goto done;
	}

	if (len < sizeof(*rp)) {
		fprintf(stderr, "Too small version reply (%u bytes)\n", len);
		goto done;
	}

	printf("MGMT Version %u, revision %u\n", rp->version,
						bt_get_le16(&rp->revision));

done:
	g_main_loop_quit(event_loop);
}

static void cmd_version(struct mgmt *mgmt, uint16_t index, int argc,
								char **argv)
{
	if (mgmt_send(mgmt, MGMT_OP_READ_VERSION, MGMT_INDEX_NONE,
				0, NULL, version_rsp, NULL, NULL) == 0) {
		fprintf(stderr, "Unable to send read_version cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void commands_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_rp_read_commands *rp = param;
	uint16_t num_commands, num_events;
	const uint16_t *opcode;
	size_t expected_len;
	int i;

	if (status != 0) {
		fprintf(stderr, "Reading supported commands failed with status"
			" 0x%02x (%s)\n", status, mgmt_errstr(status));
		goto done;
	}

	if (len < sizeof(*rp)) {
		fprintf(stderr, "Too small commands reply (%u bytes)\n", len);
		goto done;
	}

	num_commands = bt_get_le16(&rp->num_commands);
	num_events = bt_get_le16(&rp->num_events);

	expected_len = sizeof(*rp) + num_commands * sizeof(uint16_t) +
						num_events * sizeof(uint16_t);

	if (len < expected_len) {
		fprintf(stderr, "Too small commands reply (%u != %zu)\n",
							len, expected_len);
		goto done;
	}

	opcode = rp->opcodes;

	printf("%u commands:\n", num_commands);
	for (i = 0; i < num_commands; i++) {
		uint16_t op = bt_get_le16(opcode++);
		printf("\t%s (0x%04x)\n", mgmt_opstr(op), op);
	}

	printf("%u events:\n", num_events);
	for (i = 0; i < num_events; i++) {
		uint16_t ev = bt_get_le16(opcode++);
		printf("\t%s (0x%04x)\n", mgmt_evstr(ev), ev);
	}

done:
	g_main_loop_quit(event_loop);
}

static void cmd_commands(struct mgmt *mgmt, uint16_t index, int argc,
								char **argv)
{
	if (mgmt_send(mgmt, MGMT_OP_READ_COMMANDS, MGMT_INDEX_NONE,
				0, NULL, commands_rsp, NULL, NULL) == 0) {
		fprintf(stderr, "Unable to send read_commands cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void info_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_rp_read_info *rp = param;
	int id = GPOINTER_TO_INT(user_data);
	char addr[18];

	pending--;

	if (status != 0) {
		fprintf(stderr,
			"Reading hci%u info failed with status 0x%02x (%s)\n",
					id, status, mgmt_errstr(status));
		goto done;
	}

	if (len < sizeof(*rp)) {
		fprintf(stderr, "Too small info reply (%u bytes)\n", len);
		goto done;
	}

	ba2str(&rp->bdaddr, addr);
	printf("hci%u:\taddr %s version %u manufacturer %u"
			" class 0x%02x%02x%02x\n",
			id, addr, rp->version, bt_get_le16(&rp->manufacturer),
			rp->dev_class[2], rp->dev_class[1], rp->dev_class[0]);

	printf("\tsupported settings: ");
	print_settings(bt_get_le32(&rp->supported_settings));

	printf("\n\tcurrent settings: ");
	print_settings(bt_get_le32(&rp->current_settings));

	printf("\n\tname %s\n", rp->name);
	printf("\tshort name %s\n", rp->short_name);

	if (pending > 0)
		return;

done:
	g_main_loop_quit(event_loop);
}

static void index_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_rp_read_index_list *rp = param;
	struct mgmt *mgmt = user_data;
	uint16_t count;
	unsigned int i;

	if (status != 0) {
		fprintf(stderr,
			"Reading index list failed with status 0x%02x (%s)\n",
						status, mgmt_errstr(status));
		goto done;
	}

	if (len < sizeof(*rp)) {
		fprintf(stderr, "Too small index list reply (%u bytes)\n",
									len);
		goto done;
	}

	count = bt_get_le16(&rp->num_controllers);

	if (len < sizeof(*rp) + count * sizeof(uint16_t)) {
		fprintf(stderr,
			"Index count (%u) doesn't match reply length (%u)\n",
								count, len);
		goto done;
	}

	if (monitor)
		printf("Index list with %u item%s\n",
						count, count > 1 ? "s" : "");

	if (count == 0)
		goto done;

	if (monitor && count > 0)
		printf("\t");

	for (i = 0; i < count; i++) {
		uint16_t index;
		void *data;

		index = bt_get_le16(&rp->index[i]);

		if (monitor)
			printf("hci%u ", index);

		data = GINT_TO_POINTER((int) index);

		if (mgmt_send(mgmt, MGMT_OP_READ_INFO, index, 0, NULL,
						info_rsp, data, NULL) == 0) {
			fprintf(stderr, "Unable to send read_info cmd\n");
			goto done;
		}

		pending++;
	}

	if (monitor && count > 0)
		printf("\n");

	return;

done:
	g_main_loop_quit(event_loop);
}

static void cmd_info(struct mgmt *mgmt, uint16_t index, int argc, char **argv)
{
	void *data;

	if (index == MGMT_INDEX_NONE) {
		if (mgmt_send(mgmt, MGMT_OP_READ_INDEX_LIST,
					MGMT_INDEX_NONE, 0, NULL,
					index_rsp, mgmt, NULL) == 0) {
			fprintf(stderr, "Unable to send index_list cmd\n");
			exit(EXIT_FAILURE);
		}

		return;
	}

	data = GINT_TO_POINTER((int) index);

	if (mgmt_send(mgmt, MGMT_OP_READ_INFO, index, 0, NULL, info_rsp,
							data, NULL) == 0) {
		fprintf(stderr, "Unable to send read_info cmd\n");
		exit(EXIT_FAILURE);
	}
}

/* Wrapper to get the index and opcode to the response callback */
struct command_data {
	uint16_t id;
	uint16_t op;
	void (*callback) (uint16_t id, uint16_t op, uint8_t status,
					uint16_t len, const void *param);
};

static void cmd_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	struct command_data *data = user_data;

	data->callback(data->op, data->id, status, len, param);
}

static unsigned int send_cmd(struct mgmt *mgmt, uint16_t op, uint16_t id,
				uint16_t len, const void *param,
				void (*cb)(uint16_t id, uint16_t op,
						uint8_t status, uint16_t len,
						const void *param))
{
	struct command_data *data;
	unsigned int send_id;

	data = g_new0(struct command_data, 1);
	data->id = id;
	data->op = op;
	data->callback = cb;

	send_id = mgmt_send(mgmt, op, id, len, param, cmd_rsp, data, g_free);
	if (send_id == 0)
		g_free(data);

	return id;
}

static void setting_rsp(uint16_t op, uint16_t id, uint8_t status, uint16_t len,
							const void *param)
{
	const uint32_t *rp = param;

	if (status != 0) {
		fprintf(stderr,
			"%s for hci%u failed with status 0x%02x (%s)\n",
			mgmt_opstr(op), id, status, mgmt_errstr(status));
		goto done;
	}

	if (len < sizeof(*rp)) {
		fprintf(stderr, "Too small %s response (%u bytes)\n",
							mgmt_opstr(op), len);
		goto done;
	}

	printf("hci%u %s complete, settings: ", id, mgmt_opstr(op));
	print_settings(bt_get_le32(rp));
	printf("\n");

done:
	g_main_loop_quit(event_loop);
}

static void cmd_setting(struct mgmt *mgmt, uint16_t index, uint16_t op,
							int argc, char **argv)
{
	uint8_t val;

	if (argc < 2) {
		printf("Specify \"on\" or \"off\"\n");
		exit(EXIT_FAILURE);
	}

	if (strcasecmp(argv[1], "on") == 0 || strcasecmp(argv[1], "yes") == 0)
		val = 1;
	else if (strcasecmp(argv[1], "off") == 0)
		val = 0;
	else
		val = atoi(argv[1]);

	if (index == MGMT_INDEX_NONE)
		index = 0;

	if (send_cmd(mgmt, op, index, sizeof(val), &val, setting_rsp) == 0) {
		fprintf(stderr, "Unable to send %s cmd\n", mgmt_opstr(op));
		exit(EXIT_FAILURE);
	}
}

static void cmd_power(struct mgmt *mgmt, uint16_t index, int argc, char **argv)
{
	cmd_setting(mgmt, index, MGMT_OP_SET_POWERED, argc, argv);
}

static void cmd_discov(struct mgmt *mgmt, uint16_t index, int argc,
								char **argv)
{
	struct mgmt_cp_set_discoverable cp;

	if (argc < 2) {
		printf("Usage: btmgmt %s <yes/no> [timeout]\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	memset(&cp, 0, sizeof(cp));

	if (strcasecmp(argv[1], "on") == 0 || strcasecmp(argv[1], "yes") == 0)
		cp.val = 1;
	else if (strcasecmp(argv[1], "off") == 0)
		cp.val = 0;
	else
		cp.val = atoi(argv[1]);

	if (argc > 2)
		cp.timeout = htobs(atoi(argv[2]));

	if (index == MGMT_INDEX_NONE)
		index = 0;

	if (send_cmd(mgmt, MGMT_OP_SET_DISCOVERABLE, index, sizeof(cp), &cp,
							setting_rsp) == 0) {
		fprintf(stderr, "Unable to send set_discoverable cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void cmd_connectable(struct mgmt *mgmt, uint16_t index, int argc,
								char **argv)
{
	cmd_setting(mgmt, index, MGMT_OP_SET_CONNECTABLE, argc, argv);
}

static void cmd_pairable(struct mgmt *mgmt, uint16_t index, int argc,
								char **argv)
{
	cmd_setting(mgmt, index, MGMT_OP_SET_PAIRABLE, argc, argv);
}

static void cmd_linksec(struct mgmt *mgmt, uint16_t index, int argc,
								char **argv)
{
	cmd_setting(mgmt, index, MGMT_OP_SET_LINK_SECURITY, argc, argv);
}

static void cmd_ssp(struct mgmt *mgmt, uint16_t index, int argc, char **argv)
{
	cmd_setting(mgmt, index, MGMT_OP_SET_SSP, argc, argv);
}

static void cmd_hs(struct mgmt *mgmt, uint16_t index, int argc, char **argv)
{
	cmd_setting(mgmt, index, MGMT_OP_SET_HS, argc, argv);
}

static void cmd_le(struct mgmt *mgmt, uint16_t index, int argc, char **argv)
{
	cmd_setting(mgmt, index, MGMT_OP_SET_LE, argc, argv);
}

static void class_rsp(uint16_t op, uint16_t id, uint8_t status, uint16_t len,
							const void *param)
{
	const struct mgmt_ev_class_of_dev_changed *rp = param;

	if (len == 0 && status != 0) {
		fprintf(stderr, "%s failed, status 0x%02x (%s)\n",
				mgmt_opstr(op), status, mgmt_errstr(status));
		goto done;
	}

	if (len != sizeof(*rp)) {
		fprintf(stderr, "Unexpected %s len %u\n", mgmt_opstr(op), len);
		goto done;
	}

	printf("%s succeeded. Class 0x%02x%02x%02x\n", mgmt_opstr(op),
		rp->class_of_dev[2], rp->class_of_dev[1], rp->class_of_dev[0]);

done:
	g_main_loop_quit(event_loop);
}

static void cmd_class(struct mgmt *mgmt, uint16_t index, int argc, char **argv)
{
	uint8_t class[2];

	if (argc < 3) {
		printf("Usage: btmgmt %s <major> <minor>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	class[0] = atoi(argv[1]);
	class[1] = atoi(argv[2]);

	if (index == MGMT_INDEX_NONE)
		index = 0;

	if (send_cmd(mgmt, MGMT_OP_SET_DEV_CLASS, index, sizeof(class), class,
							class_rsp) == 0) {
		fprintf(stderr, "Unable to send set_dev_class cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void disconnect_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_rp_disconnect *rp = param;
	char addr[18];

	if (len == 0 && status != 0) {
		fprintf(stderr, "Disconnect failed with status 0x%02x (%s)\n",
						status, mgmt_errstr(status));
		goto done;
	}

	if (len != sizeof(*rp)) {
		fprintf(stderr, "Invalid disconnect response length (%u)\n",
									len);
		goto done;
	}

	ba2str(&rp->addr.bdaddr, addr);

	if (status == 0)
		printf("%s disconnected\n", addr);
	else
		fprintf(stderr,
			"Disconnecting %s failed with status 0x%02x (%s)\n",
				addr, status, mgmt_errstr(status));

done:
	g_main_loop_quit(event_loop);
}

static void cmd_disconnect(struct mgmt *mgmt, uint16_t index, int argc,
								char **argv)
{
	struct mgmt_cp_disconnect cp;

	if (argc < 2) {
		printf("Usage: btmgmt %s <address>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	str2ba(argv[1], &cp.addr.bdaddr);

	if (index == MGMT_INDEX_NONE)
		index = 0;

	if (mgmt_send(mgmt, MGMT_OP_DISCONNECT, index, sizeof(cp), &cp,
					disconnect_rsp, NULL, NULL) == 0) {
		fprintf(stderr, "Unable to send disconnect cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void con_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_rp_get_connections *rp = param;
	uint16_t count, i;

	if (len < sizeof(*rp)) {
		fprintf(stderr, "Too small (%u bytes) get_connections rsp\n",
									len);
		goto done;
	}

	count = bt_get_le16(&rp->conn_count);
	if (len != sizeof(*rp) + count * sizeof(struct mgmt_addr_info)) {
		fprintf(stderr, "Invalid get_connections length "
					" (count=%u, len=%u)\n", count, len);
		goto done;
	}

	for (i = 0; i < count; i++) {
		char addr[18];

		ba2str(&rp->addr[i].bdaddr, addr);

		printf("%s type %s\n", addr, typestr(rp->addr[i].type));
	}

done:
	g_main_loop_quit(event_loop);
}

static void cmd_con(struct mgmt *mgmt, uint16_t index, int argc, char **argv)
{
	if (index == MGMT_INDEX_NONE)
		index = 0;

	if (mgmt_send(mgmt, MGMT_OP_GET_CONNECTIONS, index, 0, NULL,
						con_rsp, NULL, NULL) == 0) {
		fprintf(stderr, "Unable to send get_connections cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void find_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	if (status != 0) {
		fprintf(stderr,
			"Unable to start discovery. status 0x%02x (%s)\n",
						status, mgmt_errstr(status));
		g_main_loop_quit(event_loop);
		return;
	}

	printf("Discovery started\n");
	discovery = true;
}

static void find_usage(void)
{
	printf("Usage: btmgmt find [-l|-b]>\n");
}

static struct option find_options[] = {
	{ "help",	0, 0, 'h' },
	{ "le-only",	1, 0, 'l' },
	{ "bredr-only",	1, 0, 'b' },
	{ 0, 0, 0, 0 }
};

static void cmd_find(struct mgmt *mgmt, uint16_t index, int argc, char **argv)
{
	struct mgmt_cp_start_discovery cp;
	uint8_t type;
	int opt;

	if (index == MGMT_INDEX_NONE)
		index = 0;

	type = 0;
	hci_set_bit(BDADDR_BREDR, &type);
	hci_set_bit(BDADDR_LE_PUBLIC, &type);
	hci_set_bit(BDADDR_LE_RANDOM, &type);

	while ((opt = getopt_long(argc, argv, "+lbh", find_options,
								NULL)) != -1) {
		switch (opt) {
		case 'l':
			hci_clear_bit(BDADDR_BREDR, &type);
			hci_set_bit(BDADDR_LE_PUBLIC, &type);
			hci_set_bit(BDADDR_LE_RANDOM, &type);
			break;
		case 'b':
			hci_set_bit(BDADDR_BREDR, &type);
			hci_clear_bit(BDADDR_LE_PUBLIC, &type);
			hci_clear_bit(BDADDR_LE_RANDOM, &type);
			break;
		case 'h':
		default:
			find_usage();
			exit(EXIT_SUCCESS);
		}
	}

	argc -= optind;
	argv += optind;
	optind = 0;

	memset(&cp, 0, sizeof(cp));
	cp.type = type;

	if (mgmt_send(mgmt, MGMT_OP_START_DISCOVERY, index, sizeof(cp), &cp,
						find_rsp, NULL, NULL) == 0) {
		fprintf(stderr, "Unable to send start_discovery cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void name_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	if (status != 0)
		fprintf(stderr, "Unable to set local name "
						"with status 0x%02x (%s)\n",
						status, mgmt_errstr(status));

	g_main_loop_quit(event_loop);
}

static void cmd_name(struct mgmt *mgmt, uint16_t index, int argc, char **argv)
{
	struct mgmt_cp_set_local_name cp;

	if (argc < 2) {
		printf("Usage: btmgmt %s <name> [shortname]\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	if (index == MGMT_INDEX_NONE)
		index = 0;

	memset(&cp, 0, sizeof(cp));
	strncpy((char *) cp.name, argv[1], HCI_MAX_NAME_LENGTH);
	if (argc > 2)
		strncpy((char *) cp.short_name, argv[2],
					MGMT_MAX_SHORT_NAME_LENGTH);

	if (mgmt_send(mgmt, MGMT_OP_SET_LOCAL_NAME, index, sizeof(cp), &cp,
						name_rsp, NULL, NULL) == 0) {
		fprintf(stderr, "Unable to send set_name cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void pair_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_rp_pair_device *rp = param;
	char addr[18];

	if (len == 0 && status != 0) {
		fprintf(stderr, "Pairing failed with status 0x%02x (%s)\n",
						status, mgmt_errstr(status));
		goto done;
	}

	if (len != sizeof(*rp)) {
		fprintf(stderr, "Unexpected pair_rsp len %u\n", len);
		goto done;
	}

	ba2str(&rp->addr.bdaddr, addr);

	if (status != 0) {
		fprintf(stderr,
			"Pairing with %s (%s) failed. status 0x%02x (%s)\n",
			addr, typestr(rp->addr.type), status,
			mgmt_errstr(status));
		goto done;
	}

	printf("Paired with %s\n", addr);

done:
	g_main_loop_quit(event_loop);
}

static void pair_usage(void)
{
	printf("Usage: btmgmt pair [-c cap] [-t type] <remote address>\n");
}

static struct option pair_options[] = {
	{ "help",	0, 0, 'h' },
	{ "capability",	1, 0, 'c' },
	{ "type",	1, 0, 't' },
	{ 0, 0, 0, 0 }
};

static void cmd_pair(struct mgmt *mgmt, uint16_t index, int argc, char **argv)
{
	struct mgmt_cp_pair_device cp;
	uint8_t cap = 0x01;
	uint8_t type = BDADDR_BREDR;
	int opt;

	while ((opt = getopt_long(argc, argv, "+c:t:h", pair_options,
								NULL)) != -1) {
		switch (opt) {
		case 'c':
			cap = strtol(optarg, NULL, 0);
			break;
		case 't':
			type = strtol(optarg, NULL, 0);
			break;
		case 'h':
		default:
			pair_usage();
			exit(EXIT_SUCCESS);
		}
	}

	argc -= optind;
	argv += optind;
	optind = 0;

	if (argc < 1) {
		pair_usage();
		exit(EXIT_FAILURE);
	}

	if (index == MGMT_INDEX_NONE)
		index = 0;

	memset(&cp, 0, sizeof(cp));
	str2ba(argv[0], &cp.addr.bdaddr);
	cp.addr.type = type;
	cp.io_cap = cap;

	if (mgmt_send(mgmt, MGMT_OP_PAIR_DEVICE, index, sizeof(cp), &cp,
						pair_rsp, NULL, NULL) == 0) {
		fprintf(stderr, "Unable to send pair_device cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void cancel_pair_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_addr_info *rp = param;
	char addr[18];

	if (len == 0 && status != 0) {
		fprintf(stderr, "Cancel Pairing failed with 0x%02x (%s)\n",
						status, mgmt_errstr(status));
		goto done;
	}

	if (len != sizeof(*rp)) {
		fprintf(stderr, "Unexpected cancel_pair_rsp len %u\n", len);
		goto done;
	}

	ba2str(&rp->bdaddr, addr);

	if (status != 0) {
		fprintf(stderr,
			"Cancel Pairing with %s (%s) failed. 0x%02x (%s)\n",
			addr, typestr(rp->type), status,
			mgmt_errstr(status));
		goto done;
	}

	printf("Pairing Cancelled with %s\n", addr);

done:
	g_main_loop_quit(event_loop);
}

static void cancel_pair_usage(void)
{
	printf("Usage: btmgmt cancelpair [-t type] <remote address>\n");
}

static struct option cancel_pair_options[] = {
	{ "help",	0, 0, 'h' },
	{ "type",	1, 0, 't' },
	{ 0, 0, 0, 0 }
};

static void cmd_cancel_pair(struct mgmt *mgmt, uint16_t index, int argc,
								char **argv)
{
	struct mgmt_addr_info cp;
	uint8_t type = BDADDR_BREDR;
	int opt;

	while ((opt = getopt_long(argc, argv, "+t:h", cancel_pair_options,
								NULL)) != -1) {
		switch (opt) {
		case 't':
			type = strtol(optarg, NULL, 0);
			break;
		case 'h':
		default:
			cancel_pair_usage();
			exit(EXIT_SUCCESS);
		}
	}

	argc -= optind;
	argv += optind;
	optind = 0;

	if (argc < 1) {
		cancel_pair_usage();
		exit(EXIT_FAILURE);
	}

	if (index == MGMT_INDEX_NONE)
		index = 0;

	memset(&cp, 0, sizeof(cp));
	str2ba(argv[0], &cp.bdaddr);
	cp.type = type;

	if (mgmt_send(mgmt, MGMT_OP_CANCEL_PAIR_DEVICE, index, sizeof(cp), &cp,
					cancel_pair_rsp, NULL, NULL) == 0) {
		fprintf(stderr, "Unable to send cancel_pair_device cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void unpair_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_rp_unpair_device *rp = param;
	char addr[18];

	if (len == 0 && status != 0) {
		fprintf(stderr, "Unpair device failed. status 0x%02x (%s)\n",
						status, mgmt_errstr(status));
		goto done;
	}

	if (len != sizeof(*rp)) {
		fprintf(stderr, "Unexpected unpair_device_rsp len %u\n", len);
		goto done;
	}

	ba2str(&rp->addr.bdaddr, addr);

	if (status != 0) {
		fprintf(stderr,
			"Unpairing %s failed. status 0x%02x (%s)\n",
				addr, status, mgmt_errstr(status));
		goto done;
	}

	printf("%s unpaired\n", addr);

done:
	g_main_loop_quit(event_loop);
}

static void cmd_unpair(struct mgmt *mgmt, uint16_t index, int argc,
								char **argv)
{
	struct mgmt_cp_unpair_device cp;

	if (argc < 2) {
		printf("Usage: btmgmt %s <remote address>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	if (index == MGMT_INDEX_NONE)
		index = 0;

	memset(&cp, 0, sizeof(cp));
	str2ba(argv[1], &cp.addr.bdaddr);
	cp.disconnect = 1;

	if (mgmt_send(mgmt, MGMT_OP_UNPAIR_DEVICE, index, sizeof(cp), &cp,
						unpair_rsp, NULL, NULL) == 0) {
		fprintf(stderr, "Unable to send unpair_device cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void keys_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	if (status != 0)
		fprintf(stderr, "Load keys failed with status 0x%02x (%s)\n",
						status, mgmt_errstr(status));
	else
		printf("Keys successfully loaded\n");

	g_main_loop_quit(event_loop);
}

static void cmd_keys(struct mgmt *mgmt, uint16_t index, int argc, char **argv)
{
	struct mgmt_cp_load_link_keys cp;

	if (index == MGMT_INDEX_NONE)
		index = 0;

	memset(&cp, 0, sizeof(cp));

	if (mgmt_send(mgmt, MGMT_OP_LOAD_LINK_KEYS, index, sizeof(cp), &cp,
						keys_rsp, NULL, NULL) == 0) {
		fprintf(stderr, "Unable to send load_keys cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void block_rsp(uint16_t op, uint16_t id, uint8_t status, uint16_t len,
							const void *param)
{
	const struct mgmt_addr_info *rp = param;
	char addr[18];

	if (len == 0 && status != 0) {
		fprintf(stderr, "%s failed, status 0x%02x (%s)\n",
				mgmt_opstr(op), status, mgmt_errstr(status));
		goto done;
	}

	if (len != sizeof(*rp)) {
		fprintf(stderr, "Unexpected %s len %u\n", mgmt_opstr(op), len);
		goto done;
	}

	ba2str(&rp->bdaddr, addr);

	if (status != 0) {
		fprintf(stderr, "%s %s (%s) failed. status 0x%02x (%s)\n",
				mgmt_opstr(op), addr, typestr(rp->type),
				status, mgmt_errstr(status));
		goto done;
	}

	printf("%s %s succeeded\n", mgmt_opstr(op), addr);

done:
	g_main_loop_quit(event_loop);
}

static void block_usage(void)
{
	printf("Usage: btmgmt block [-t type] <remote address>\n");
}

static struct option block_options[] = {
	{ "help",	0, 0, 'h' },
	{ "type",	1, 0, 't' },
	{ 0, 0, 0, 0 }
};

static void cmd_block(struct mgmt *mgmt, uint16_t index, int argc, char **argv)
{
	struct mgmt_cp_block_device cp;
	uint8_t type = BDADDR_BREDR;
	int opt;

	while ((opt = getopt_long(argc, argv, "+t:h", block_options,
							NULL)) != -1) {
		switch (opt) {
		case 't':
			type = strtol(optarg, NULL, 0);
			break;
		case 'h':
		default:
			block_usage();
			exit(EXIT_SUCCESS);
		}
	}

	argc -= optind;
	argv += optind;
	optind = 0;

	if (argc < 1) {
		block_usage();
		exit(EXIT_FAILURE);
	}

	if (index == MGMT_INDEX_NONE)
		index = 0;

	memset(&cp, 0, sizeof(cp));
	str2ba(argv[0], &cp.addr.bdaddr);
	cp.addr.type = type;

	if (send_cmd(mgmt, MGMT_OP_BLOCK_DEVICE, index, sizeof(cp), &cp,
							block_rsp) == 0) {
		fprintf(stderr, "Unable to send block_device cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void unblock_usage(void)
{
	printf("Usage: btmgmt unblock [-t type] <remote address>\n");
}

static void cmd_unblock(struct mgmt *mgmt, uint16_t index, int argc,
								char **argv)
{
	struct mgmt_cp_unblock_device cp;
	uint8_t type = BDADDR_BREDR;
	int opt;

	while ((opt = getopt_long(argc, argv, "+t:h", block_options,
							NULL)) != -1) {
		switch (opt) {
		case 't':
			type = strtol(optarg, NULL, 0);
			break;
		case 'h':
		default:
			unblock_usage();
			exit(EXIT_SUCCESS);
		}
	}

	argc -= optind;
	argv += optind;
	optind = 0;

	if (argc < 1) {
		unblock_usage();
		exit(EXIT_FAILURE);
	}

	if (index == MGMT_INDEX_NONE)
		index = 0;

	memset(&cp, 0, sizeof(cp));
	str2ba(argv[0], &cp.addr.bdaddr);
	cp.addr.type = type;

	if (send_cmd(mgmt, MGMT_OP_UNBLOCK_DEVICE, index, sizeof(cp), &cp,
							block_rsp) == 0) {
		fprintf(stderr, "Unable to send unblock_device cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void uuid_to_uuid128(uuid_t *uuid128, const uuid_t *uuid)
{
	if (uuid->type == SDP_UUID16)
		sdp_uuid16_to_uuid128(uuid128, uuid);
	else if (uuid->type == SDP_UUID32)
		sdp_uuid32_to_uuid128(uuid128, uuid);
	else
		memcpy(uuid128, uuid, sizeof(*uuid));
}

static void cmd_add_uuid(struct mgmt *mgmt, uint16_t index, int argc,
							char **argv)
{
	struct mgmt_cp_add_uuid cp;
	uint128_t uint128;
	uuid_t uuid, uuid128;

	if (argc < 3) {
		printf("UUID and service hint needed\n");
		exit(EXIT_FAILURE);
	}

	if (index == MGMT_INDEX_NONE)
		index = 0;

	if (bt_string2uuid(&uuid, argv[1]) < 0) {
		printf("Invalid UUID: %s\n", argv[1]);
		exit(EXIT_FAILURE);
	}

	memset(&cp, 0, sizeof(cp));

	uuid_to_uuid128(&uuid128, &uuid);
	ntoh128((uint128_t *) uuid128.value.uuid128.data, &uint128);
	htob128(&uint128, (uint128_t *) cp.uuid);

	cp.svc_hint = atoi(argv[2]);

	if (send_cmd(mgmt, MGMT_OP_ADD_UUID, index, sizeof(cp), &cp,
							class_rsp) == 0) {
		fprintf(stderr, "Unable to send add_uuid cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void cmd_remove_uuid(struct mgmt *mgmt, uint16_t index, int argc,
								char **argv)
{
	struct mgmt_cp_remove_uuid cp;
	uint128_t uint128;
	uuid_t uuid, uuid128;

	if (argc < 2) {
		printf("UUID needed\n");
		exit(EXIT_FAILURE);
	}

	if (index == MGMT_INDEX_NONE)
		index = 0;

	if (bt_string2uuid(&uuid, argv[1]) < 0) {
		printf("Invalid UUID: %s\n", argv[1]);
		exit(EXIT_FAILURE);
	}

	memset(&cp, 0, sizeof(cp));

	uuid_to_uuid128(&uuid128, &uuid);
	ntoh128((uint128_t *) uuid128.value.uuid128.data, &uint128);
	htob128(&uint128, (uint128_t *) cp.uuid);

	if (send_cmd(mgmt, MGMT_OP_REMOVE_UUID, index, sizeof(cp), &cp,
							class_rsp) == 0) {
		fprintf(stderr, "Unable to send remove_uuid cmd\n");
		exit(EXIT_FAILURE);
	}
}

static void cmd_clr_uuids(struct mgmt *mgmt, uint16_t index, int argc,
								char **argv)
{
	char *uuid_any = "00000000-0000-0000-0000-000000000000";
	char *rm_argv[] = { "rm-uuid", uuid_any, NULL };

	cmd_remove_uuid(mgmt, index, 2, rm_argv);
}

static void did_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	if (status != 0)
		fprintf(stderr, "Set Device ID failed "
						"with status 0x%02x (%s)\n",
						status, mgmt_errstr(status));
	else
		printf("Device ID successfully set\n");

	g_main_loop_quit(event_loop);
}

static void did_usage(void)
{
	printf("Usage: btmgmt did <source>:<vendor>:<product>:<version>\n");
	printf("       possible source values: bluetooth, usb\n");
}

static void cmd_did(struct mgmt *mgmt, uint16_t index, int argc, char **argv)
{
	struct mgmt_cp_set_device_id cp;
	uint16_t vendor, product, version , source;
	int result;

	if (argc < 2) {
		did_usage();
		exit(EXIT_FAILURE);
	}

	result = sscanf(argv[1], "bluetooth:%4hx:%4hx:%4hx", &vendor, &product,
								&version);
	if (result == 3) {
		source = 0x0001;
		goto done;
	}

	result = sscanf(argv[1], "usb:%4hx:%4hx:%4hx", &vendor, &product,
								&version);
	if (result == 3) {
		source = 0x0002;
		goto done;
	}

	did_usage();
	exit(EXIT_FAILURE);

done:
	if (index == MGMT_INDEX_NONE)
		index = 0;

	cp.source = htobs(source);
	cp.vendor = htobs(vendor);
	cp.product = htobs(product);
	cp.version = htobs(version);

	if (mgmt_send(mgmt, MGMT_OP_SET_DEVICE_ID, index, sizeof(cp), &cp,
						did_rsp, NULL, NULL) == 0) {
		fprintf(stderr, "Unable to send set_dev_class cmd\n");
		exit(EXIT_FAILURE);
	}
}

static struct {
	char *cmd;
	void (*func)(struct mgmt *mgmt, uint16_t index, int argc, char **argv);
	char *doc;
} command[] = {
	{ "monitor",	cmd_monitor,	"Monitor events"		},
	{ "version",	cmd_version,	"Get the MGMT Version"		},
	{ "commands",	cmd_commands,	"List supported commands"	},
	{ "info",	cmd_info,	"Show controller info"		},
	{ "power",	cmd_power,	"Toggle powered state"		},
	{ "discov",	cmd_discov,	"Toggle discoverable state"	},
	{ "connectable",cmd_connectable,"Toggle connectable state"	},
	{ "pairable",	cmd_pairable,	"Toggle pairable state"		},
	{ "linksec",	cmd_linksec,	"Toggle link level security"	},
	{ "ssp",	cmd_ssp,	"Toggle SSP mode"		},
	{ "hs",		cmd_hs,		"Toggle HS Support"		},
	{ "le",		cmd_le,		"Toggle LE Support"		},
	{ "class",	cmd_class,	"Set device major/minor class"	},
	{ "disconnect", cmd_disconnect, "Disconnect device"		},
	{ "con",	cmd_con,	"List connections"		},
	{ "find",	cmd_find,	"Discover nearby devices"	},
	{ "name",	cmd_name,	"Set local name"		},
	{ "pair",	cmd_pair,	"Pair with a remote device"	},
	{ "cancelpair",	cmd_cancel_pair,"Cancel pairing"		},
	{ "unpair",	cmd_unpair,	"Unpair device"			},
	{ "keys",	cmd_keys,	"Load Keys"			},
	{ "block",	cmd_block,	"Block Device"			},
	{ "unblock",	cmd_unblock,	"Unblock Device"		},
	{ "add-uuid",	cmd_add_uuid,	"Add UUID"			},
	{ "rm-uuid",	cmd_remove_uuid, "Remove UUID"			},
	{ "clr-uuids",	cmd_clr_uuids,	"Clear UUIDs",			},
	{ "did",	cmd_did,	"Set Device ID",		},
	{ NULL, NULL, 0 }
};

static void usage(void)
{
	int i;

	printf("btmgmt ver %s\n", VERSION);
	printf("Usage:\n"
		"\tbtmgmt [options] <command> [command parameters]\n");

	printf("Options:\n"
		"\t--index <id>\tSpecify adapter index\n"
		"\t--verbose\tEnable extra logging\n"
		"\t--help\tDisplay help\n");

	printf("Commands:\n");
	for (i = 0; command[i].cmd; i++)
		printf("\t%-15s\t%s\n", command[i].cmd, command[i].doc);

	printf("\n"
		"For more information on the usage of each command use:\n"
		"\tbtmgmt <command> --help\n" );
}

static struct option main_options[] = {
	{ "index",	1, 0, 'i' },
	{ "verbose",	0, 0, 'v' },
	{ "help",	0, 0, 'h' },
	{ 0, 0, 0, 0 }
};

int main(int argc, char *argv[])
{
	int opt, i;
	uint16_t index = MGMT_INDEX_NONE;
	struct mgmt *mgmt;

	while ((opt = getopt_long(argc, argv, "+hvi:",
						main_options, NULL)) != -1) {
		switch (opt) {
		case 'i':
			if (strlen(optarg) > 3 &&
					strncasecmp(optarg, "hci", 3) == 0)
				index = atoi(&optarg[4]);
			else
				index = atoi(optarg);
			break;
		case 'v':
			monitor = true;
			break;
		case 'h':
		default:
			usage();
			return 0;
		}
	}

	argc -= optind;
	argv += optind;
	optind = 0;

	if (argc < 1) {
		usage();
		return 0;
	}

	event_loop = g_main_loop_new(NULL, FALSE);

	mgmt = mgmt_new_default();
	if (!mgmt) {
		fprintf(stderr, "Unable to open mgmt_socket\n");
		g_main_loop_unref(event_loop);
		return -1;
	}

	for (i = 0; command[i].cmd; i++) {
		if (strcmp(command[i].cmd, argv[0]) != 0)
			continue;

		command[i].func(mgmt, index, argc, argv);
		break;
	}

	if (command[i].cmd == NULL) {
		fprintf(stderr, "Unknown command: %s\n", argv[0]);
		mgmt_unref(mgmt);
		g_main_loop_unref(event_loop);
		return -1;
	}

	mgmt_register(mgmt, MGMT_EV_CONTROLLER_ERROR, index, controller_error,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_INDEX_ADDED, index, index_added,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_INDEX_REMOVED, index, index_removed,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_NEW_SETTINGS, index, new_settings,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_DISCOVERING, index, discovering,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_NEW_LINK_KEY, index, new_link_key,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_DEVICE_CONNECTED, index, connected,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_DEVICE_DISCONNECTED, index, disconnected,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_CONNECT_FAILED, index, conn_failed,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_AUTH_FAILED, index, auth_failed,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_LOCAL_NAME_CHANGED, index,
					local_name_changed, NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_DEVICE_FOUND, index, device_found,
								mgmt, NULL);
	mgmt_register(mgmt, MGMT_EV_PIN_CODE_REQUEST, index, request_pin,
								mgmt, NULL);
	mgmt_register(mgmt, MGMT_EV_USER_CONFIRM_REQUEST, index, user_confirm,
								mgmt, NULL);

	g_main_loop_run(event_loop);

	mgmt_cancel_all(mgmt);
	mgmt_unregister_all(mgmt);
	mgmt_unref(mgmt);

	g_main_loop_unref(event_loop);

	return 0;
}
