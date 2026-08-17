/*
 * Fixed-size GTBS call-control protocol used by chan_mobile.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "chan_mobile_lecall.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

static const uint8_t call_magic[4] = { 'G', 'G', 'C', 'C' };

static uint16_t get_le16(const uint8_t *value)
{
	return (uint16_t) value[0] | ((uint16_t) value[1] << 8);
}

static uint32_t get_le32(const uint8_t *value)
{
	return (uint32_t) value[0] |
		((uint32_t) value[1] << 8) |
		((uint32_t) value[2] << 16) |
		((uint32_t) value[3] << 24);
}

static uint64_t get_le64(const uint8_t *value)
{
	return (uint64_t) get_le32(value) |
		((uint64_t) get_le32(value + 4) << 32);
}

static void put_le16(uint8_t *value, uint16_t input)
{
	value[0] = (uint8_t) input;
	value[1] = (uint8_t) (input >> 8);
}

static void put_le32(uint8_t *value, uint32_t input)
{
	value[0] = (uint8_t) input;
	value[1] = (uint8_t) (input >> 8);
	value[2] = (uint8_t) (input >> 16);
	value[3] = (uint8_t) (input >> 24);
}

static void put_le64(uint8_t *value, uint64_t input)
{
	put_le32(value, (uint32_t) input);
	put_le32(value + 4, (uint32_t) (input >> 32));
}

static int valid_device(const char *device)
{
	size_t i;

	if (!device || strlen(device) != 17)
		return 0;
	for (i = 0; i < 17; ++i) {
		if ((i + 1) % 3 == 0) {
			if (device[i] != ':')
				return 0;
		} else if (!isxdigit((unsigned char) device[i])) {
			return 0;
		}
	}
	return 1;
}

static int valid_result(uint8_t result)
{
	return result <= 0x04 || result == 0x06;
}

static int valid_call_reference(uint8_t opcode, uint8_t index,
	uint64_t token)
{
	if (opcode == CM_LE_CALL_ORIGINATE)
		return (index == 0 && token == 0) ||
			(index != 0 && token != 0);
	return index != 0 && token != 0;
}

int cm_le_call_valid_uri(const uint8_t *uri, size_t length)
{
	size_t i;

	if (!uri || length < 5 || length > CM_LE_CALL_PAYLOAD_SIZE ||
		memcmp(uri, "tel:", 4) != 0)
		return 0;
	for (i = 4; i < length; ++i) {
		if ((uri[i] < '0' || uri[i] > '9') &&
			uri[i] != '+' && uri[i] != '*' && uri[i] != '#')
			return 0;
	}
	return 1;
}

static int valid_message(const struct cm_le_call_message *message)
{
	if (!message || message->type < CM_LE_CALL_STATE ||
		message->type > CM_LE_CALL_READY ||
		(message->flags & ~(CM_LE_CALL_FLAG_SNAPSHOT |
		 CM_LE_CALL_FLAG_LAST)) != 0 ||
		message->sequence == 0 ||
		message->payload_length > CM_LE_CALL_PAYLOAD_SIZE ||
		!valid_device(message->device))
		return 0;

	switch (message->type) {
	case CM_LE_CALL_STATE:
		if (!(message->flags & CM_LE_CALL_FLAG_SNAPSHOT) ||
			message->payload_length != 0)
			return 0;
		if (message->code == CM_LE_CALL_NO_STATE)
			return message->index == 0 && message->token == 0 &&
				message->value == 0;
		return message->index != 0 && message->token != 0 &&
			message->code <= CM_LE_CALL_STATE_LOCALLY_REMOTELY_HELD &&
			(message->value & ~(CM_LE_CALL_FLAG_OUTGOING |
			 CM_LE_CALL_FLAG_WITHHELD_SERVER |
			 CM_LE_CALL_FLAG_WITHHELD_NETWORK)) == 0;
	case CM_LE_CALL_COMMAND:
		if (message->flags != 0 || message->code > CM_LE_CALL_JOIN)
			return 0;
		if (message->code == CM_LE_CALL_ORIGINATE)
			return message->index == 0 && message->token == 0 &&
				cm_le_call_valid_uri(message->payload,
					message->payload_length);
		return message->index != 0 && message->token != 0 &&
			message->payload_length == 0;
	case CM_LE_CALL_ACK:
		return message->flags == 0 && message->code <= CM_LE_CALL_JOIN &&
			message->value <= CM_LE_CALL_ACK_WRITE_FAILED &&
			valid_call_reference(message->code, message->index,
				message->token) &&
			message->payload_length == 0;
	case CM_LE_CALL_RESULT:
		return message->flags == 0 && message->code <= CM_LE_CALL_JOIN &&
			valid_call_reference(message->code, message->index,
				message->token) &&
			valid_result(message->value) &&
			message->payload_length == 0;
	case CM_LE_CALL_READY:
		return message->flags == 0 && message->index == 0 &&
			message->token == 0 && message->code <= 1 &&
			message->value == 0 && message->payload_length == 0;
	default:
		return 0;
	}
}

int cm_le_call_encode(const struct cm_le_call_message *message,
	uint8_t packet[CM_LE_CALL_PACKET_SIZE])
{
	if (!packet || !valid_message(message))
		return -EINVAL;
	memset(packet, 0, CM_LE_CALL_PACKET_SIZE);
	memcpy(packet, call_magic, sizeof(call_magic));
	packet[4] = CM_LE_CALL_VERSION;
	packet[5] = message->type;
	packet[6] = message->flags;
	put_le64(packet + 8, message->sequence);
	put_le64(packet + 16, message->token);
	packet[24] = message->index;
	packet[25] = message->code;
	packet[26] = message->value;
	put_le16(packet + 28, message->payload_length);
	memcpy(packet + 32, message->device, 17);
	memcpy(packet + 50, message->payload, message->payload_length);
	return 0;
}

int cm_le_call_decode(const uint8_t *packet, size_t packet_length,
	struct cm_le_call_message *message)
{
	uint16_t payload_length;
	size_t i;

	if (!packet || !message || packet_length != CM_LE_CALL_PACKET_SIZE ||
		memcmp(packet, call_magic, sizeof(call_magic)) != 0 ||
		packet[4] != CM_LE_CALL_VERSION || packet[7] != 0 ||
		packet[27] != 0 || get_le16(packet + 30) != 0 ||
		packet[49] != 0)
		return -EPROTO;
	payload_length = get_le16(packet + 28);
	if (payload_length > CM_LE_CALL_PAYLOAD_SIZE)
		return -EMSGSIZE;
	for (i = 50 + payload_length; i < CM_LE_CALL_PACKET_SIZE; ++i) {
		if (packet[i] != 0)
			return -EPROTO;
	}
	memset(message, 0, sizeof(*message));
	message->type = packet[5];
	message->flags = packet[6];
	message->sequence = get_le64(packet + 8);
	message->token = get_le64(packet + 16);
	message->index = packet[24];
	message->code = packet[25];
	message->value = packet[26];
	message->payload_length = payload_length;
	memcpy(message->device, packet + 32, 17);
	message->device[17] = '\0';
	memcpy(message->payload, packet + 50, payload_length);
	if (!valid_message(message))
		return -EPROTO;
	return 0;
}

int cm_le_call_device_equal(const char *left, const char *right)
{
	size_t i;

	if (!valid_device(left) || !valid_device(right))
		return 0;
	for (i = 0; i < 17; ++i) {
		if (toupper((unsigned char) left[i]) !=
			toupper((unsigned char) right[i]))
			return 0;
	}
	return 1;
}

int cm_le_call_connect(const char *path)
{
	struct sockaddr_un address;
	size_t length;
	int socket_fd;
	int flags;

	if (!path || path[0] != '/')
		return -EINVAL;
	length = strlen(path);
	if (length == 0 || length >= sizeof(address.sun_path))
		return -ENAMETOOLONG;
	socket_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (socket_fd < 0)
		return -errno;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	memcpy(address.sun_path, path, length + 1);
	if (connect(socket_fd, (struct sockaddr *) &address, sizeof(address)) < 0) {
		int saved_errno = errno;
		close(socket_fd);
		return -saved_errno;
	}
	flags = fcntl(socket_fd, F_GETFL, 0);
	if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		int saved_errno = errno;
		close(socket_fd);
		return -saved_errno;
	}
	return socket_fd;
}

int cm_le_call_receive(int socket_fd, struct cm_le_call_message *message)
{
	uint8_t packet[CM_LE_CALL_PACKET_SIZE];
	struct iovec iov;
	struct msghdr header;
	ssize_t received;

	if (socket_fd < 0 || !message)
		return -EINVAL;
	memset(&header, 0, sizeof(header));
	iov.iov_base = packet;
	iov.iov_len = sizeof(packet);
	header.msg_iov = &iov;
	header.msg_iovlen = 1;
	received = recvmsg(socket_fd, &header,
		MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
	if (received < 0)
		return -errno;
	if (received == 0)
		return -ECONNRESET;
	if ((header.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
		received != CM_LE_CALL_PACKET_SIZE)
		return -EMSGSIZE;
	return cm_le_call_decode(packet, sizeof(packet), message);
}

int cm_le_call_send(int socket_fd,
	const struct cm_le_call_message *message)
{
	uint8_t packet[CM_LE_CALL_PACKET_SIZE];
	ssize_t written;
	int result;

	if (socket_fd < 0)
		return -EINVAL;
	result = cm_le_call_encode(message, packet);
	if (result < 0)
		return result;
	written = send(socket_fd, packet, sizeof(packet),
		MSG_DONTWAIT | MSG_NOSIGNAL);
	if (written < 0)
		return -errno;
	if (written != sizeof(packet))
		return -EIO;
	return 0;
}
