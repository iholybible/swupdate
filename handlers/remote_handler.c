/*
 * (C) Copyright 2016
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <sys/types.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <zmq.h>

#include <swupdate.h>
#include <handler.h>
#include <util.h>

#define MSG_FRAMES	2
#define FRAME_CMD	0
#define FRAME_BODY	1

#define REMOTE_IPC_TIMEOUT	2000

static int timeout = REMOTE_IPC_TIMEOUT;

struct RHmsg {
    zmq_msg_t frame[MSG_FRAMES];
};

struct remote_command {
	char *cmd;
};

void remote_handler(void);

static void RHset_command(struct RHmsg *self, const char *key)
{
    zmq_msg_t *msg = &self->frame[FRAME_CMD];
    zmq_msg_init_size (msg, strlen(key));
    memcpy (zmq_msg_data (msg), key, strlen(key));
}

static void RHset_payload(struct RHmsg *self, const void *body, size_t size)
{
    zmq_msg_t *msg = &self->frame[FRAME_BODY];
    zmq_msg_init_size(msg, size);
    memcpy (zmq_msg_data(msg), body, size);
}

static int RHmsg_send_cmd(struct RHmsg *self, void *request)
{
	int i;
	int ret;

	for (i = 0; i < MSG_FRAMES; i++) {
		ret = zmq_msg_send (&self->frame[i], request,
			(i < MSG_FRAMES - 1)? ZMQ_SNDMORE: 0);
		if (ret < 0 )
			return errno;
	}

	return 0;
}

static int RHmsg_get_ack(struct RHmsg *self, void *request)
{
	int rc;
	unsigned long size;
	zmq_pollitem_t zpoll;
	char *string;
	int newtimeout;
	int len;

	zpoll.socket = request;
	zpoll.events = ZMQ_POLLIN;

	/*
	 * Wait for an answer, raise
	 * an error if no message is received
	 */
	rc = zmq_poll(&zpoll, 1, timeout);
	if (rc <= 0)
		return -EFAULT;

	zmq_msg_init (&self->frame[0]);
	if (zmq_msg_recv(&self->frame[0], request, 0) == -1) {
		zmq_msg_close(&self->frame[0]);
		return -EFAULT;
	}

	size = zmq_msg_size(&self->frame[0]);
	string = malloc (size + 1);
	memcpy (string, zmq_msg_data (&self->frame[0]), size);
	string[size] = '\0';
	zmq_msg_close(&self->frame[0]);

	/*
	 * Check if the remote send a new timeout
	 */
	len = size;

	if (strchr(string, ':'))
		len = (strchr(string, ':') - string - 1);
	if (strncmp(string, "ACK", len) != 0) {
		ERROR("Remote Handler returns error, exiting");
		return -EFAULT;
	}

	/*
	 * Check if the remote ask to wait longer
	 * we get ack, check the rest of the received
	 * string
	 */
	if ((size > 4) && (string[3] == ':')) {
		newtimeout = strtoul(&string[4], NULL, 10);
		if (newtimeout > 0)
			timeout = newtimeout;
	}

	return 0;
}

static int forward_data(void *request, const void *buf, unsigned int len)
{
	struct RHmsg RHmessage;
	int ret;

	if (!request)
		return -EFAULT;

	RHset_command(&RHmessage, "DATA");
	RHset_payload(&RHmessage, buf, len);
	ret = RHmsg_send_cmd(&RHmessage, request);
	if (ret)
		return ret;

	ret = RHmsg_get_ack(&RHmessage, request);

	return ret;
}

static int install_remote_image(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	void *context = zmq_ctx_new();
	void *request = zmq_socket (context, ZMQ_REQ);
	char *connect_string;
	int len;
	int ret = 0;
	struct RHmsg RHmessage;
	char bufcmd[80];

	len = strlen(img->type_data) + strlen(TMPDIR) + strlen("ipc://") + 4;

	/*
	 * Allocate maximum string
	 */
	connect_string = malloc(len);
	if (!connect_string) {
		ERROR("Not enough memory");
		return -ENOMEM;
	}
	snprintf(connect_string, len, "ipc://%s%s", TMPDIR,
			img->type_data);

	ret = zmq_connect(request, connect_string);
	if (ret < 0) {
		ERROR("Connection with %s cannot be established",
				connect_string);
		ret = -ENODEV;
		goto cleanup;
	}

	/* Initialize default timeout */
	timeout = REMOTE_IPC_TIMEOUT;

	/* Send initialization string */
	snprintf(bufcmd, sizeof(bufcmd), "INIT:%lld", img->size);
	RHset_command(&RHmessage, bufcmd);
	RHset_payload(&RHmessage, NULL, 0);
	RHmsg_send_cmd(&RHmessage, request);
	if (RHmsg_get_ack(&RHmessage, request))
		return -ENODEV;

	ret = copyimage(request, img, forward_data);

cleanup:
	zmq_close(request);
	zmq_ctx_destroy(context);

	return ret;
}

__attribute__((constructor))
void remote_handler(void)
{
	register_handler("remote", install_remote_image, NULL);
}
