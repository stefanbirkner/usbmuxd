/*
 * client.c
 *
 * Copyright (C) 2009 Hector Martin <hector@marcansoft.com>
 * Copyright (C) 2009 Nikias Bassen <nikias@gmx.li>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 or version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE 1

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <fcntl.h>

#include <plist/plist.h>

#include "collection.h"
#include "log.h"
#include "usb.h"
#include "utils.h"
#include "client.h"
#include "device.h"
#include "conf.h"

#define CMD_BUF_SIZE	0x10000
#define REPLY_BUF_SIZE	0x10000

enum client_state {
	CLIENT_COMMAND,		// waiting for command
	CLIENT_LISTEN,		// listening for devices
	CLIENT_CONNECTING1,	// issued connection request
	CLIENT_CONNECTING2,	// connection established, but waiting for response message to get sent
	CLIENT_CONNECTED,	// connected
	CLIENT_DEAD
};

struct mux_client {
	int fd;
	unsigned char *ob_buf;
	uint32_t ob_size;
	uint32_t ob_capacity;
	unsigned char *ib_buf;
	uint32_t ib_size;
	uint32_t ib_capacity;
	short events, devents;
	uint32_t connect_tag;
	int connect_device;
	enum client_state state;
	uint32_t proto_version;
	uint32_t number;
	plist_t info;
};

static struct collection client_list;
pthread_mutex_t client_list_mutex;
static uint32_t client_number = 0;

#ifdef SO_PEERCRED
static char* _get_process_name_by_pid(const int pid)
{
	char* name = (char*)calloc(1024, sizeof(char));
	if(name) {
		sprintf(name, "/proc/%d/cmdline", pid);
		FILE* f = fopen(name, "r");
		if(f) {
			size_t size;
			size = fread(name, sizeof(char), 1024, f);
			if(size > 0) {
				if('\n' == name[size-1])
					name[size-1]='\0';
			}
			fclose(f);
		}
	}
	return name;
}
#endif

/**
 * Receive raw data from the client socket.
 *
 * @param client Client to read from.
 * @param buffer Buffer to store incoming data.
 * @param len Max number of bytes to read.
 * @return Same as recv() system call. Number of bytes read; when < 0 errno will be set.
 */
int client_read(struct mux_client *client, void *buffer, uint32_t len)
{
	usbmuxd_log(LL_SPEW, "client_read fd %d buf %p len %d", client->fd, buffer, len);
	if(client->state != CLIENT_CONNECTED) {
		usbmuxd_log(LL_ERROR, "Attempted to read from client %d not in CONNECTED state", client->fd);
		return -1;
	}
	return recv(client->fd, buffer, len, 0);
}

/**
 * Send raw data to the client socket.
 *
 * @param client Client to send to.
 * @param buffer The data to send.
 * @param len Number of bytes to write.
 * @return Same as system call send(). Number of bytes written; when < 0 errno will be set.
 */
int client_write(struct mux_client *client, void *buffer, uint32_t len)
{
	int sret = -1;

	usbmuxd_log(LL_SPEW, "client_write fd %d buf %p len %d", client->fd, buffer, len);
	if(client->state != CLIENT_CONNECTED) {
		usbmuxd_log(LL_ERROR, "Attempted to write to client %d not in CONNECTED state", client->fd);
		return -1;
	}

	sret = send(client->fd, buffer, len, 0);
	if (sret < 0) {
		if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
			usbmuxd_log(LL_DEBUG, "client_write: fd %d not ready for writing", client->fd);
			sret = 0;
		} else {
			usbmuxd_log(LL_ERROR, "ERROR: client_write: sending to fd %d failed: %s", client->fd, strerror(errno));
		}
	}
	return sret;
}

/**
 * Set event mask to use for ppoll()ing the client socket.
 * Typically POLLOUT and/or POLLIN. Note that this overrides
 * the current mask, that is, it is not ORing the argument
 * into the current mask.
 *
 * @param client The client to set the event mask on.
 * @param events The event mask to sert.
 * @return 0 on success, -1 on error.
 */
int client_set_events(struct mux_client *client, short events)
{
	if((client->state != CLIENT_CONNECTED) && (client->state != CLIENT_CONNECTING2)) {
		usbmuxd_log(LL_ERROR, "client_set_events to client %d not in CONNECTED state", client->fd);
		return -1;
	}
	client->devents = events;
	if(client->state == CLIENT_CONNECTED)
		client->events = events;
	return 0;
}

static void client_init2(struct mux_client *client, int fd)
{
	client->fd = fd;
	client->ob_buf = malloc(REPLY_BUF_SIZE);
	client->ob_size = 0;
	client->ob_capacity = REPLY_BUF_SIZE;
	client->ib_buf = malloc(CMD_BUF_SIZE);
	client->ib_size = 0;
	client->ib_capacity = CMD_BUF_SIZE;
	client->state = CLIENT_COMMAND;
	client->events = POLLIN;
	client->info = NULL;
}

static void client_log_event(struct mux_client *client, const char *event)
{
#ifdef SO_PEERCRED
	if (log_level >= LL_INFO) {
		struct ucred cr;
		socklen_t len = sizeof(struct ucred);
		getsockopt(client->fd, SOL_SOCKET, SO_PEERCRED, &cr, &len);

		if (getpid() == cr.pid) {
			usbmuxd_log(LL_INFO, "Client %d %s: %s[%d]", client->fd, event, PACKAGE_NAME, cr.pid);
		} else {
			char* process_name = _get_process_name_by_pid(cr.pid);
			usbmuxd_log(LL_INFO, "Client %d %s: %s[%d]", client->fd, event, process_name, cr.pid);
			free(process_name);
		}
	}
#else
	usbmuxd_log(LL_INFO, "Client %d %s", client->fd, event);
#endif
}

static void client_list_add(struct mux_client *client)
{
	pthread_mutex_lock(&client_list_mutex);
	client->number = client_number++;
	collection_add(&client_list, client);
	pthread_mutex_unlock(&client_list_mutex);
}

static int client_list_contains(struct mux_client *client)
{
	FOREACH(struct mux_client *lc, &client_list) {
		if (client == lc) {
			return 1;
		}
	} ENDFOREACH
	return 0;
}

/**
 * Wait for an inbound connection on the usbmuxd socket
 * and create a new mux_client instance for it, and store
 * the client in the client list.
 *
 * @param listenfd the socket fd to accept() on.
 * @return The connection fd for the client, or < 0 for error
 *   in which case errno will be set.
 */
int client_accept(int listenfd)
{
	struct sockaddr_un addr;
	int cfd;
	socklen_t len = sizeof(struct sockaddr_un);
	cfd = accept(listenfd, (struct sockaddr *)&addr, &len);
	if (cfd < 0) {
		usbmuxd_log(LL_ERROR, "accept() failed (%s)", strerror(errno));
		return cfd;
	}

	int flags = fcntl(cfd, F_GETFL, 0);
	if (flags < 0) {
		usbmuxd_log(LL_ERROR, "ERROR: Could not get socket flags!");
	} else {
		if (fcntl(cfd, F_SETFL, flags | O_NONBLOCK) < 0) {
			usbmuxd_log(LL_ERROR, "ERROR: Could not set socket to non-blocking mode");
		}
	}

	int bufsize = 0x20000;
	if (setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(int)) == -1) {
		usbmuxd_log(LL_WARNING, "Could not set send buffer for client socket");
	}
	if (setsockopt(cfd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(int)) == -1) {
		usbmuxd_log(LL_WARNING, "Could not set receive buffer for client socket");
	}

	int yes = 1;
	setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (void*)&yes, sizeof(int));

	struct mux_client *client;
	client = malloc(sizeof(struct mux_client));
	memset(client, 0, sizeof(struct mux_client));
	client_init2(client, cfd);

	client_list_add(client);

	client_log_event(client, "accepted");
	return client->fd;
}

void client_close(struct mux_client *client)
{
	pthread_mutex_lock(&client_list_mutex);
	if (!client_list_contains(client)) {
		// in case we get called again but client was already freed
		usbmuxd_log(LL_DEBUG, "%s: ignoring for non-existing client %p", __func__, client);
		pthread_mutex_unlock(&client_list_mutex);
		return;
	}
	client_log_event(client, "is going to be disconnected: %s[%d]");
	if(client->state == CLIENT_CONNECTING1 || client->state == CLIENT_CONNECTING2) {
		usbmuxd_log(LL_INFO, "Client died mid-connect, aborting device %d connection", client->connect_device);
		client->state = CLIENT_DEAD;
		device_abort_connect(client->connect_device, client);
	}
	close(client->fd);
	free(client->ob_buf);
	free(client->ib_buf);
	plist_free(client->info);

	collection_remove(&client_list, client);
	pthread_mutex_unlock(&client_list_mutex);
	free(client);
}

void client_get_fds(struct fdlist *list)
{
	pthread_mutex_lock(&client_list_mutex);
	FOREACH(struct mux_client *client, &client_list) {
		fdlist_add_client_fd(list, client->fd, client->events);
	} ENDFOREACH
	pthread_mutex_unlock(&client_list_mutex);
}

static int output_buffer_add_message(struct mux_client *client, uint32_t tag, enum usbmuxd_msgtype msg, void *payload, int payload_length)
{
	struct usbmuxd_header hdr;
	hdr.version = client->proto_version;
	hdr.length = sizeof(hdr) + payload_length;
	hdr.message = msg;
	hdr.tag = tag;
	usbmuxd_log(LL_DEBUG, "Client %d output buffer got tag %d msg %d payload_length %d", client->fd, tag, msg, payload_length);

	uint32_t available = client->ob_capacity - client->ob_size;
	/* the output buffer _should_ be large enough, but just in case */
	if(available < hdr.length) {
		unsigned char* new_buf;
		uint32_t new_size = ((client->ob_capacity + hdr.length + 4096) / 4096) * 4096;
		usbmuxd_log(LL_DEBUG, "%s: Enlarging client %d output buffer %d -> %d", __func__, client->fd, client->ob_capacity, new_size);
		new_buf = realloc(client->ob_buf, new_size);
		if (!new_buf) {
			usbmuxd_log(LL_FATAL, "%s: Failed to realloc.", __func__);
			return -1;
		}
		client->ob_buf = new_buf;
		client->ob_capacity = new_size;
	}
	memcpy(client->ob_buf + client->ob_size, &hdr, sizeof(hdr));
	if(payload && payload_length)
		memcpy(client->ob_buf + client->ob_size + sizeof(hdr), payload, payload_length);
	client->ob_size += hdr.length;
	client->events |= POLLOUT;
	return hdr.length;
}

static int send_plist(struct mux_client *client, uint32_t tag, plist_t plist)
{
	int res = -1;
	char *xml = NULL;
	uint32_t xmlsize = 0;
	plist_to_xml(plist, &xml, &xmlsize);
	if (xml) {
		res = output_buffer_add_message(client, tag, MESSAGE_PLIST, xml, xmlsize);
		free(xml);
	} else {
		usbmuxd_log(LL_ERROR, "%s: Could not convert plist to xml", __func__);
	}
	return res;
}

static int send_result(struct mux_client *client, uint32_t tag, uint32_t result)
{
	int res = -1;
	if (client->proto_version == 1) {
		/* XML plist packet */
		plist_t dict = plist_new_dict();
		plist_dict_set_item(dict, "MessageType", plist_new_string("Result"));
		plist_dict_set_item(dict, "Number", plist_new_uint(result));
		res = send_plist(client, tag, dict);
		plist_free(dict);
	} else {
		/* binary packet */
		res = output_buffer_add_message(client, tag, MESSAGE_RESULT, &result, sizeof(uint32_t));
	}
	return res;
}

static int send_bad_command(struct mux_client *client, uint32_t tag)
{
	if(send_result(client, tag, RESULT_BADCOMMAND) < 0) {
		return -1;
	}
	return 0;
}

int client_notify_connect(struct mux_client *client, enum usbmuxd_result result)
{
	usbmuxd_log(LL_SPEW, "client_notify_connect fd %d result %d", client->fd, result);
	if(client->state == CLIENT_DEAD)
		return -1;
	if(client->state != CLIENT_CONNECTING1) {
		usbmuxd_log(LL_ERROR, "client_notify_connect when client %d is not in CONNECTING1 state", client->fd);
		return -1;
	}
	if(send_result(client, client->connect_tag, result) < 0)
		return -1;
	if(result == RESULT_OK) {
		client->state = CLIENT_CONNECTING2;
		client->events = POLLOUT; // wait for the result packet to go through
		// no longer need this
		free(client->ib_buf);
		client->ib_buf = NULL;
	} else {
		client->state = CLIENT_COMMAND;
	}
	return 0;
}

static plist_t create_device_attached_plist(struct device_info *dev)
{
	plist_t dict = plist_new_dict();
	plist_dict_set_item(dict, "MessageType", plist_new_string("Attached"));
	plist_dict_set_item(dict, "DeviceID", plist_new_uint(dev->id));
	plist_t props = plist_new_dict();
	plist_dict_set_item(props, "ConnectionSpeed", plist_new_uint(dev->speed));
	plist_dict_set_item(props, "ConnectionType", plist_new_string("USB"));
	plist_dict_set_item(props, "DeviceID", plist_new_uint(dev->id));
	plist_dict_set_item(props, "LocationID", plist_new_uint(dev->location));
	plist_dict_set_item(props, "ProductID", plist_new_uint(dev->pid));
	plist_dict_set_item(props, "SerialNumber", plist_new_string(dev->serial));
	plist_dict_set_item(dict, "Properties", props);
	return dict;
}

static int send_device_list(struct mux_client *client, uint32_t tag)
{
	int res = -1;
	plist_t dict = plist_new_dict();
	plist_t devices = plist_new_array();

	struct device_info *devs = NULL;
	struct device_info *dev;
	int i;

	int count = device_get_list(0, &devs);
	dev = devs;
	for (i = 0; devs && i < count; i++) {
		plist_t device = create_device_attached_plist(dev++);
		if (device) {
			plist_array_append_item(devices, device);
		}
	}
	if (devs)
		free(devs);

	plist_dict_set_item(dict, "DeviceList", devices);
	res = send_plist(client, tag, dict);
	plist_free(dict);
	return res;
}

static int send_listener_list(struct mux_client *client, uint32_t tag)
{
	int res = -1;

	plist_t dict = plist_new_dict();
	plist_t listeners = plist_new_array();

	pthread_mutex_lock(&client_list_mutex);
	FOREACH(struct mux_client *lc, &client_list) {
		if (lc->state == CLIENT_LISTEN) {
			plist_t n = NULL;
			plist_t l = plist_new_dict();
			plist_dict_set_item(l, "Blacklisted", plist_new_bool(0));
			n = NULL;
			if (lc->info) {
				n = plist_dict_get_item(lc->info, "BundleID");
			}
			if (n) {
				plist_dict_set_item(l, "BundleID", plist_copy(n));
			}
			plist_dict_set_item(l, "ConnType", plist_new_uint(0));

			n = NULL;
			char *progname = NULL;
			if (lc->info) {
				n = plist_dict_get_item(lc->info, "ProgName");
			}
			if (n) {
				plist_get_string_val(n, &progname);
			}
			if (!progname) {
				progname = strdup("unknown");
			}
			char *idstring = malloc(strlen(progname) + 12);
			sprintf(idstring, "%u-%s", client->number, progname);

			plist_dict_set_item(l, "ID String", plist_new_string(idstring));
			free(idstring);
			plist_dict_set_item(l, "ProgName", plist_new_string(progname));
			free(progname);

			n = NULL;
			uint64_t version = 0;
			if (lc->info) {
				n = plist_dict_get_item(lc->info, "kLibUSBMuxVersion");
			}
			if (n) {
				plist_get_uint_val(n, &version);
			}
			plist_dict_set_item(l, "kLibUSBMuxVersion", plist_new_uint(version));

			plist_array_append_item(listeners, l);
		}
	} ENDFOREACH
	pthread_mutex_unlock(&client_list_mutex);

	plist_dict_set_item(dict, "ListenerList", listeners);
	res = send_plist(client, tag, dict);
	plist_free(dict);

	return res;
}

static int send_system_buid(struct mux_client *client, uint32_t tag)
{
	int res = -1;
	char* buid = NULL;

	config_get_system_buid(&buid);

	plist_t dict = plist_new_dict();
	plist_dict_set_item(dict, "BUID", plist_new_string(buid));
	free(buid);
	res = send_plist(client, tag, dict);
	plist_free(dict);
	return res;
}

static int send_pair_record(struct mux_client *client, uint32_t tag, const char* record_id)
{
	int res = -1;
	char* record_data = NULL;
	uint64_t record_size = 0;

	if (!record_id) {
		return send_result(client, tag, EINVAL);
	}

	config_get_device_record(record_id, &record_data, &record_size);

	if (record_data) {
		plist_t dict = plist_new_dict();
		plist_dict_set_item(dict, "PairRecordData", plist_new_data(record_data, record_size));
		free(record_data);
		res = send_plist(client, tag, dict);
		plist_free(dict);
	} else {
		res = send_result(client, tag, ENOENT);
	}
	return res;
}

static int send_device_add(struct mux_client *client, struct device_info *dev)
{
	int res = -1;
	if (client->proto_version == 1) {
		/* XML plist packet */
		plist_t dict = create_device_attached_plist(dev);
		res = send_plist(client, 0, dict);
		plist_free(dict);
	} else {
		/* binary packet */
		struct usbmuxd_device_record dmsg;
		memset(&dmsg, 0, sizeof(dmsg));
		dmsg.device_id = dev->id;
		strncpy(dmsg.serial_number, dev->serial, 256);
		dmsg.serial_number[255] = 0;
		dmsg.location = dev->location;
		dmsg.product_id = dev->pid;
		res = output_buffer_add_message(client, 0, MESSAGE_DEVICE_ADD, &dmsg, sizeof(dmsg));
	}
	return res;
}

static int send_message(struct mux_client *client, enum usbmuxd_msgtype msg, const char *type, uint32_t device_id)
{
	int res = -1;
	if (client->proto_version == 1) {
		/* XML plist packet */
		plist_t dict = plist_new_dict();
		plist_dict_set_item(dict, "MessageType", plist_new_string(type));
		plist_dict_set_item(dict, "DeviceID", plist_new_uint(device_id));
		res = send_plist(client, 0, dict);
		plist_free(dict);
	}
	else {
		/* binary packet */
		res = output_buffer_add_message(client, 0, msg, &device_id, sizeof(uint32_t));
	}
	return res;
}

static int send_device_remove(struct mux_client *client, uint32_t device_id)
{
	return send_message(
		client, MESSAGE_DEVICE_REMOVE, "Detached", device_id);
}

static int send_device_paired(struct mux_client *client, uint32_t device_id)
{
	return send_message(
		client, MESSAGE_DEVICE_PAIRED, "Paired", device_id);
}

static int start_listen(struct mux_client *client, struct usbmuxd_header *hdr)
{
	struct device_info *devs = NULL;
	struct device_info *dev;
	int count, i;

	if(send_result(client, hdr->tag, 0) < 0)
		return -1;

	usbmuxd_log(LL_DEBUG, "Client %d now LISTENING", client->fd);
	client->state = CLIENT_LISTEN;

	count = device_get_list(0, &devs);
	dev = devs;
	for(i=0; devs && i < count; i++) {
		if(send_device_add(client, dev++) < 0) {
			free(devs);
			return -1;
		}
	}
	if (devs)
		free(devs);

	return count;
}

static char* plist_dict_get_string_val(plist_t dict, const char* key)
{
	if (plist_get_node_type(dict) != PLIST_DICT)
		return NULL;
	plist_t item = plist_dict_get_item(dict, key);
	if (plist_get_node_type(item) != PLIST_STRING)
		return NULL;
	char *str = NULL;
	plist_get_string_val(item, &str);
	return str;
}

static void copy_plist_item(const char* key, plist_type type, plist_t src, plist_t dest)
{
	plist_t item = plist_dict_get_item(src, key);
	if (plist_get_node_type(item) == type) {
		plist_dict_set_item(dest, key, plist_copy(item));
	}
}

static void update_client_info(struct mux_client *client, plist_t dict)
{
	plist_t info = plist_new_dict();

	copy_plist_item("BundleID", PLIST_STRING, dict, info);
	copy_plist_item("ClientVersionString", PLIST_STRING, dict, info);
	copy_plist_item("ProgName", PLIST_STRING, dict, info);
	copy_plist_item("kLibUSBMuxVersion", PLIST_UINT, dict, info);
	plist_free(client->info);
	client->info = info;
}

static int start_connect(int device_id, uint16_t port, struct mux_client *client, uint32_t tag)
{
	int res;
	usbmuxd_log(LL_DEBUG, "Client %d requesting connection to device %d port %d", client->fd, device_id, ntohs(port));
	res = device_start_connect(device_id, ntohs(port), client);
	if(res < 0) {
		if (send_result(client, tag, -res) < 0)
			return -1;
	} else {
		client->connect_tag = tag;
		client->connect_device = device_id;
		client->state = CLIENT_CONNECTING1;
	}
	return 0;
}

static int client_process_message(struct mux_client *client, struct usbmuxd_header *hdr, char *message, plist_t dict)
{
	int res;

	if (!strcmp(message, "Listen")) {
		return start_listen(client, hdr);
	} else if (!strcmp(message, "Connect")) {
		uint64_t val;
		uint16_t portnum = 0;
		uint32_t device_id = 0;
		plist_t node = NULL;
		// get device id
		node = plist_dict_get_item(dict, "DeviceID");
		if (!node) {
			usbmuxd_log(LL_ERROR, "Received connect request without device_id!");
			if (send_result(client, hdr->tag, RESULT_BADDEV) < 0)
				return -1;
			return 0;
		}
		val = 0;
		plist_get_uint_val(node, &val);
		device_id = (uint32_t)val;

		// get port number
		node = plist_dict_get_item(dict, "PortNumber");
		if (!node) {
			usbmuxd_log(LL_ERROR, "Received connect request without port number!");
			return send_bad_command(client, hdr->tag);
		}
		val = 0;
		plist_get_uint_val(node, &val);
		portnum = (uint16_t)val;
		return start_connect(device_id, portnum, client, hdr->tag);
	} else if (!strcmp(message, "ListDevices")) {
		if (send_device_list(client, hdr->tag) < 0)
			return -1;
		return 0;
	} else if (!strcmp(message, "ListListeners")) {
		if (send_listener_list(client, hdr->tag) < 0)
			return -1;
		return 0;
	} else if (!strcmp(message, "ReadBUID")) {
		if (send_system_buid(client, hdr->tag) < 0)
			return -1;
		return 0;
	} else if (!strcmp(message, "ReadPairRecord")) {
		char* record_id = plist_dict_get_string_val(dict, "PairRecordID");

		res = send_pair_record(client, hdr->tag, record_id);
		if (record_id)
			free(record_id);
		if (res < 0)
			return -1;
		return 0;
	} else if (!strcmp(message, "SavePairRecord")) {
		uint32_t rval = RESULT_OK;
		char* record_id = plist_dict_get_string_val(dict, "PairRecordID");
		char* record_data = NULL;
		uint64_t record_size = 0;
		plist_t rdata = plist_dict_get_item(dict, "PairRecordData");
		if (plist_get_node_type(rdata) == PLIST_DATA) {
			plist_get_data_val(rdata, &record_data, &record_size);
		}

		if (record_id && record_data) {
			res = config_set_device_record(record_id, record_data, record_size);
			if (res < 0) {
				rval = -res;
			} else {
				plist_t p_dev_id = plist_dict_get_item(dict, "DeviceID");
				uint32_t dev_id = 0;
				if (plist_get_node_type(p_dev_id) == PLIST_UINT) {
					uint64_t u_dev_id = 0;
					plist_get_uint_val(p_dev_id, &u_dev_id);
					dev_id = (uint32_t)u_dev_id;
				}
				if (dev_id > 0) {
					struct device_info *devs = NULL;
					struct device_info *dev;
					int i;
					int count = device_get_list(1, &devs);
					int found = 0;
					dev = devs;
					for (i = 0; devs && i < count; i++, dev++) {
						if ((uint32_t)dev->id == dev_id && (strcmp(dev->serial, record_id) == 0)) {
							found++;
							break;
						}
					}
					if (!found) {
						usbmuxd_log(LL_ERROR, "ERROR: SavePairRecord: DeviceID %d (%s) is not connected\n", dev_id, record_id);
					} else {
						client_device_paired(dev_id);
					}
					free(devs);
				}
			}
			free(record_id);
		} else {
			rval = EINVAL;
		}
		free(record_data);
		if (send_result(client, hdr->tag, rval) < 0)
			return -1;
		return 0;
	} else if (!strcmp(message, "DeletePairRecord")) {
		uint32_t rval = RESULT_OK;
		char* record_id = plist_dict_get_string_val(dict, "PairRecordID");
		if (record_id) {
			res = config_remove_device_record(record_id);
			if (res < 0) {
				rval = -res;
			}
			free(record_id);
		} else {
			rval = EINVAL;
		}
		if (send_result(client, hdr->tag, rval) < 0)
			return -1;
		return 0;
	} else {
		usbmuxd_log(LL_ERROR, "Unexpected command '%s' received!", message);
		return send_bad_command(client, hdr->tag);
	}
}

static int client_plist_command(struct mux_client *client, struct usbmuxd_header *hdr)
{
	char *payload;
	uint32_t payload_size;
	int res;

	client->proto_version = 1;
	payload = (char*)(hdr) + sizeof(struct usbmuxd_header);
	payload_size = hdr->length - sizeof(struct usbmuxd_header);
	plist_t dict = NULL;
	plist_from_xml(payload, payload_size, &dict);
	if (!dict) {
		usbmuxd_log(LL_ERROR, "Could not parse plist from payload!");
		return -1;
	} else {
		char *message = NULL;
		plist_t node = plist_dict_get_item(dict, "MessageType");
		if (plist_get_node_type(node) != PLIST_STRING) {
			usbmuxd_log(LL_ERROR, "Could not read valid MessageType node from plist!");
			plist_free(dict);
			return -1;
		}
		plist_get_string_val(node, &message);
		if (!message) {
			usbmuxd_log(LL_ERROR, "Could not extract MessageType from plist!");
			plist_free(dict);
			return -1;
		}
		update_client_info(client, dict);
		res = client_process_message(client, hdr, message, dict);
		free(message);
		free(dict);
		return res;
	}
	// should not be reached?!
	return -1;
}

static int handle_command(struct mux_client *client)
{
	struct usbmuxd_header *hdr = (void*)client->ib_buf;
	usbmuxd_log(LL_DEBUG, "Client %d command len %d ver %d msg %d tag %d", client->fd, hdr->length, hdr->version, hdr->message, hdr->tag);

	if(client->state != CLIENT_COMMAND) {
		usbmuxd_log(LL_ERROR, "Client %d command received in the wrong state, got %d but want %d", client->fd, client->state, CLIENT_COMMAND);
		if(send_bad_command(client, hdr->tag) < 0)
			return -1;
		client_close(client);
		return -1;
	}

	if((hdr->version != 0) && (hdr->version != 1)) {
		usbmuxd_log(LL_INFO, "Client %d version mismatch: expected 0 or 1, got %d", client->fd, hdr->version);
		send_result(client, hdr->tag, RESULT_BADVERSION);
		return 0;
	}

	struct usbmuxd_connect_request *ch;

	switch(hdr->message) {
		case MESSAGE_PLIST:
			return client_plist_command(client, hdr);
		case MESSAGE_LISTEN:
			return start_listen(client, hdr);
		case MESSAGE_CONNECT:
			ch = (void*)hdr;
			return start_connect(ch->device_id, ch->port, client, hdr->tag);
		default:
			usbmuxd_log(LL_ERROR, "Client %d invalid command %d", client->fd, hdr->message);
			return send_bad_command(client, hdr->tag);
	}
	return -1;
}

static void output_buffer_process(struct mux_client *client)
{
	int res;
	if(!client->ob_size) {
		usbmuxd_log(LL_WARNING, "Client %d OUT process but nothing to send?", client->fd);
		client->events &= ~POLLOUT;
		return;
	}
	res = send(client->fd, client->ob_buf, client->ob_size, 0);
	if(res <= 0) {
		usbmuxd_log(LL_ERROR, "Sending to client fd %d failed: %d %s", client->fd, res, strerror(errno));
		client_close(client);
		return;
	}
	if((uint32_t)res == client->ob_size) {
		client->ob_size = 0;
		client->events &= ~POLLOUT;
		if(client->state == CLIENT_CONNECTING2) {
			usbmuxd_log(LL_DEBUG, "Client %d switching to CONNECTED state", client->fd);
			client->state = CLIENT_CONNECTED;
			client->events = client->devents;
			// no longer need this
			free(client->ob_buf);
			client->ob_buf = NULL;
		}
	} else {
		client->ob_size -= res;
		memmove(client->ob_buf, client->ob_buf + res, client->ob_size);
	}
}

static int complete(struct mux_client *client, size_t expected_size)
{
	int res = recv(
		client->fd,
		client->ib_buf + client->ib_size,
		expected_size - client->ib_size,
		0);

	if(res < 0) {
		usbmuxd_log(LL_ERROR, "Receive from client fd %d failed: %s", client->fd, strerror(errno));
		client_close(client);
	} else if(res == 0) {
		usbmuxd_log(LL_INFO, "Client %d connection closed", client->fd);
		client_close(client);
	} else
		client->ib_size += res;
	return res;
}

static int message_incomplete(struct mux_client *client)
{
	struct usbmuxd_header *header = (void*)client->ib_buf;
	return client->ib_size < header->length;
}

static int complete_message(struct mux_client *client)
{
	struct usbmuxd_header *header = (void*)client->ib_buf;
	uint32_t message_length = header->length;
	return complete(client, message_length);
}

static int client_try_complete_message(struct mux_client *client, int did_read)
{
	int res;
	if(message_incomplete(client)) {
		if(did_read)
			return -1; //maybe we would block, so defer to next loop
		res = complete_message(client);
		if(res <= 0)
			return -1;
		else if(message_incomplete(client))
			return -1;
	}
	return 0;
}

static int header_incomplete(struct mux_client *client)
{
	return client->ib_size < sizeof(struct usbmuxd_header);
}

static int complete_header(struct mux_client *client)
{
	size_t header_length = sizeof(struct usbmuxd_header);
	return complete(client, header_length);
}

static int client_try_complete_header(struct mux_client *client)
{
	int res;
	if(header_incomplete(client)) {
		res = complete_header(client);
		if(res <= 0)
			return -1;
		else if(header_incomplete(client))
			return -1;
		else
			return 1;
	} else {
		return 0;
	}
}

static int close_client_with_invalid_header(struct mux_client *client)
{
	struct usbmuxd_header *hdr = (void*)client->ib_buf;
	if(hdr->length > client->ib_capacity) {
		usbmuxd_log(LL_INFO, "Client %d message is too long (%d bytes)", client->fd, hdr->length);
		client_close(client);
		return -1;
	}
	if(hdr->length < sizeof(struct usbmuxd_header)) {
		usbmuxd_log(LL_ERROR, "Client %d message is too short (%d bytes)", client->fd, hdr->length);
		client_close(client);
		return -1;
	}
	return 0;
}

static void input_buffer_process(struct mux_client *client)
{
	int res;
	int did_read;
	did_read = client_try_complete_header(client);
	if (did_read < 0)
		return;
	res = close_client_with_invalid_header(client);
	if (res < 0)
		return;
	res = client_try_complete_message(client, did_read);
	if (res < 0)
		return;
	handle_command(client);
	client->ib_size = 0;
}

static struct mux_client* find_by_fd(int fd)
{
	struct mux_client *client = NULL;

	pthread_mutex_lock(&client_list_mutex);
	FOREACH(struct mux_client *lc, &client_list) {
		if(lc->fd == fd) {
			client = lc;
			break;
		}
	} ENDFOREACH
	pthread_mutex_unlock(&client_list_mutex);

	return client;
}

void client_process(int fd, short events)
{
	struct mux_client *client = find_by_fd(fd);

	if(!client) {
		usbmuxd_log(LL_INFO, "client_process: fd %d not found in client list", fd);
		return;
	}

	if(client->state == CLIENT_CONNECTED) {
		usbmuxd_log(LL_SPEW, "client_process in CONNECTED state");
		device_client_process(client->connect_device, client, events);
	} else {
		if(events & POLLIN) {
			input_buffer_process(client);
		} else if(events & POLLOUT) { //not both in case client died as part of process_recv
			output_buffer_process(client);
		}
	}

}

void client_device_add(struct device_info *dev)
{
	pthread_mutex_lock(&client_list_mutex);
	usbmuxd_log(LL_DEBUG, "client_device_add: id %d, location 0x%x, serial %s", dev->id, dev->location, dev->serial);
	device_set_visible(dev->id);
	FOREACH(struct mux_client *client, &client_list) {
		if(client->state == CLIENT_LISTEN)
			send_device_add(client, dev);
	} ENDFOREACH
	pthread_mutex_unlock(&client_list_mutex);
}

void client_device_remove(int device_id)
{
	pthread_mutex_lock(&client_list_mutex);
	uint32_t id = device_id;
	usbmuxd_log(LL_DEBUG, "client_device_remove: id %d", device_id);
	FOREACH(struct mux_client *client, &client_list) {
		if(client->state == CLIENT_LISTEN)
			send_device_remove(client, id);
	} ENDFOREACH
	pthread_mutex_unlock(&client_list_mutex);
}

void client_device_paired(int device_id)
{
	pthread_mutex_lock(&client_list_mutex);
	uint32_t id = device_id;
	usbmuxd_log(LL_DEBUG, "client_device_paired: id %d", device_id);
	FOREACH(struct mux_client *client, &client_list) {
		if (client->state == CLIENT_LISTEN)
			send_device_paired(client, id);
	} ENDFOREACH
	pthread_mutex_unlock(&client_list_mutex);
}

void client_init(void)
{
	usbmuxd_log(LL_DEBUG, "client_init");
	collection_init(&client_list);
	pthread_mutex_init(&client_list_mutex, NULL);
}

void client_shutdown(void)
{
	usbmuxd_log(LL_DEBUG, "client_shutdown");
	FOREACH(struct mux_client *client, &client_list) {
		client_close(client);
	} ENDFOREACH
	pthread_mutex_destroy(&client_list_mutex);
	collection_free(&client_list);
}
