/*
 * Fixed-size GTBS call-control protocol used by chan_mobile.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CHAN_MOBILE_LECALL_H
#define CHAN_MOBILE_LECALL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Version 2 carries the caller identity on incoming state messages.  There is
 * no negotiation: a version 1 peer rejects the header outright, so both halves
 * have to be replaced together and a mismatch fails loudly rather than losing
 * identities in silence.
 */
#define CM_LE_CALL_VERSION 2
#define CM_LE_CALL_PACKET_SIZE 128
#define CM_LE_CALL_PAYLOAD_SIZE 78
#define CM_LE_CALL_SOCKET_MAX 107

/*
 * Caller identity inside a state payload:
 *
 *   [0]         uri length   0..CM_LE_CALL_URI_MAX
 *   [1 .. n]    uri          scheme stripped by the peer
 *   [1+n]       name length  0..CM_LE_CALL_NAME_MAX
 *   [2+n .. ]   name         UTF-8, already cut on a character boundary
 *
 * 1 + 32 + 1 + 44 is exactly CM_LE_CALL_PAYLOAD_SIZE.  An absent number means
 * it was withheld; an absent name means the caller is not in the phone's
 * contacts, which is the common case and not an error.
 */
#define CM_LE_CALL_URI_MAX 32
#define CM_LE_CALL_NAME_MAX 44

#define CM_LE_CALL_FLAG_SNAPSHOT (1U << 0)
#define CM_LE_CALL_FLAG_LAST (1U << 1)

#define CM_LE_CALL_NO_STATE 0xff

enum cm_le_call_type {
	CM_LE_CALL_STATE = 1,
	CM_LE_CALL_COMMAND = 2,
	CM_LE_CALL_ACK = 3,
	CM_LE_CALL_RESULT = 4,
	CM_LE_CALL_READY = 5,
};

enum cm_le_call_opcode {
	CM_LE_CALL_ACCEPT = 0x00,
	CM_LE_CALL_TERMINATE = 0x01,
	CM_LE_CALL_LOCAL_HOLD = 0x02,
	CM_LE_CALL_LOCAL_RETRIEVE = 0x03,
	CM_LE_CALL_ORIGINATE = 0x04,
	CM_LE_CALL_JOIN = 0x05,
};

enum cm_le_call_ack {
	CM_LE_CALL_ACK_ACCEPTED = 0,
	CM_LE_CALL_ACK_STALE = 1,
	CM_LE_CALL_ACK_UNAVAILABLE = 2,
	CM_LE_CALL_ACK_INVALID = 3,
	CM_LE_CALL_ACK_WRITE_FAILED = 4,
};

enum cm_le_call_state {
	CM_LE_CALL_STATE_INCOMING = 0x00,
	CM_LE_CALL_STATE_DIALING = 0x01,
	CM_LE_CALL_STATE_ALERTING = 0x02,
	CM_LE_CALL_STATE_ACTIVE = 0x03,
	CM_LE_CALL_STATE_LOCALLY_HELD = 0x04,
	CM_LE_CALL_STATE_REMOTELY_HELD = 0x05,
	CM_LE_CALL_STATE_LOCALLY_REMOTELY_HELD = 0x06,
};

#define CM_LE_CALL_FLAG_OUTGOING (1U << 0)
#define CM_LE_CALL_FLAG_WITHHELD_SERVER (1U << 1)
#define CM_LE_CALL_FLAG_WITHHELD_NETWORK (1U << 2)

struct cm_le_call_message {
	uint8_t type;
	uint8_t flags;
	uint64_t sequence;
	uint64_t token;
	char device[18];
	uint8_t index;
	uint8_t code;
	uint8_t value;
	uint16_t payload_length;
	uint8_t payload[CM_LE_CALL_PAYLOAD_SIZE];
};

int cm_le_call_encode(const struct cm_le_call_message *message,
	uint8_t packet[CM_LE_CALL_PACKET_SIZE]);
int cm_le_call_decode(const uint8_t *packet, size_t packet_length,
	struct cm_le_call_message *message);
int cm_le_call_valid_uri(const uint8_t *uri, size_t length);

struct cm_le_call_identity {
	char uri[CM_LE_CALL_URI_MAX + 1];
	char name[CM_LE_CALL_NAME_MAX + 1];
};

/*
 * Read a caller identity out of a state payload.  An empty payload yields an
 * empty identity and success: a call with no caller ID is ordinary.
 */
int cm_le_call_parse_identity(const uint8_t *payload, size_t length,
	struct cm_le_call_identity *identity);
int cm_le_call_device_equal(const char *left, const char *right);

int cm_le_call_connect(const char *path);
int cm_le_call_receive(int socket_fd, struct cm_le_call_message *message);
int cm_le_call_send(int socket_fd,
	const struct cm_le_call_message *message);

#endif
