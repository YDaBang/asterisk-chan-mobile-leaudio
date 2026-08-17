/*
 * LC3 media helper for the chan_mobile LE Audio canary.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "chan_mobile_leaudio.h"

#include <errno.h>
#include <fcntl.h>
#include <lc3.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef SOL_BLUETOOTH
#define SOL_BLUETOOTH 274
#endif
#ifndef BT_PKT_STATUS
#define BT_PKT_STATUS 16
#endif
#ifndef BT_SCM_PKT_STATUS
#define BT_SCM_PKT_STATUS 0x03
#endif
#ifndef MSG_CMSG_CLOEXEC
#define MSG_CMSG_CLOEXEC 0
#endif

static const uint8_t handoff_magic[4] = { 'G', 'G', 'L', 'E' };

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

static void put_le32(uint8_t *value, uint32_t number)
{
	value[0] = (uint8_t) number;
	value[1] = (uint8_t) (number >> 8);
	value[2] = (uint8_t) (number >> 16);
	value[3] = (uint8_t) (number >> 24);
}

static void put_le64(uint8_t *value, uint64_t number)
{
	put_le32(value, (uint32_t) number);
	put_le32(value + 4, (uint32_t) (number >> 32));
}

static uint16_t expected_octets(uint32_t sample_rate,
	uint32_t frame_duration_us)
{
	if (frame_duration_us == CM_LE_FRAME_DURATION_7P5_US) {
		switch (sample_rate) {
		case 16000:
			return 30;
		case 24000:
			return 45;
		case 32000:
			return 60;
		default:
			return 0;
		}
	}
	if (frame_duration_us == CM_LE_FRAME_DURATION_10_US) {
		switch (sample_rate) {
		case 16000:
			return 40;
		case 24000:
			return 60;
		case 32000:
			return 80;
		default:
			return 0;
		}
	}
	return 0;
}

static int one_bit_set(uint32_t value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

static void stream_init(struct cm_le_stream *stream)
{
	memset(stream, 0, sizeof(*stream));
	stream->fd = -1;
}

static void stream_close(struct cm_le_stream *stream)
{
	if (stream->fd >= 0)
		close(stream->fd);
	stream_init(stream);
}

static void lifecycle_close(struct cm_leaudio *state)
{
	if (state->lifecycle_fd >= 0)
		close(state->lifecycle_fd);
	state->lifecycle_fd = -1;
}

static int lifecycle_send(struct cm_leaudio *state, uint8_t type)
{
	static const uint8_t magic[4] = { 'G', 'G', 'L', 'C' };
	uint8_t message[CM_LE_LIFECYCLE_MESSAGE_SIZE];
	ssize_t written;

	if (!state || state->lifecycle_fd < 0 || !state->active)
		return -ENOTCONN;
	memset(message, 0, sizeof(message));
	memcpy(message, magic, sizeof(magic));
	message[4] = CM_LE_LIFECYCLE_VERSION;
	message[5] = type;
	put_le64(message + 8, state->sink.bundle_id);
	put_le64(message + 16, state->sink.generation);
	put_le64(message + 24, state->source.generation);
	if (type == CM_LE_LIFECYCLE_PROGRESS)
		state->io_stats.lifecycle_progress_attempts++;
	else
		state->io_stats.lifecycle_normal_end_attempts++;
	written = send(state->lifecycle_fd, message, sizeof(message),
		MSG_DONTWAIT | MSG_NOSIGNAL);
	if (written == (ssize_t) sizeof(message)) {
		if (type == CM_LE_LIFECYCLE_PROGRESS)
			state->io_stats.lifecycle_progress_successes++;
		else
			state->io_stats.lifecycle_normal_end_successes++;
		return 0;
	}
	if (type == CM_LE_LIFECYCLE_PROGRESS)
		state->io_stats.lifecycle_progress_errors++;
	else
		state->io_stats.lifecycle_normal_end_errors++;
	return written < 0 ? -errno : -EIO;
}

static void lifecycle_progress(struct cm_leaudio *state)
{
	(void) lifecycle_send(state, CM_LE_LIFECYCLE_PROGRESS);
}

static void media_progress(struct cm_leaudio *state)
{
	if (!state || !state->active)
		return;
	if (!state->media_progressed) {
		state->media_progressed = 1;
		state->io_stats.empty_sdu_guard_arms++;
	}
	if (state->guarded_empty_sdus != 0) {
		state->io_stats.empty_sdu_guard_progress_resets++;
		state->guarded_empty_sdus = 0;
		state->io_stats.guarded_consecutive_empty_sdus = 0;
	}
	state->empty_guard_tx_baseline = state->io_stats.send_successes;
	lifecycle_progress(state);
}

static void record_first_error(struct cm_leaudio_io_stats *stats,
	enum cm_leaudio_io_stage stage, int result, int detail,
	int64_t sdu_length)
{
	if (stats->first_error_stage != CM_LE_IO_NONE)
		return;
	stats->first_error_stage = stage;
	stats->first_error_result = result;
	stats->first_error_detail = detail;
	stats->first_error_sdu_length = sdu_length;
}

void cm_leaudio_init(struct cm_leaudio *state)
{
	if (!state)
		return;
	memset(state, 0, sizeof(*state));
	stream_init(&state->sink);
	stream_init(&state->source);
	state->lifecycle_fd = -1;
}

void cm_leaudio_end(struct cm_leaudio *state)
{
	if (!state)
		return;
	cm_leaudio_stop_rx(state);
	cm_leaudio_stop_tx(state);
	lifecycle_close(state);
	state->active = 0;
	state->active_rate = 0;
	state->active_frame_duration_us = 0;
	state->active_octets = 0;
	state->empty_guard_tx_baseline = 0;
	state->guarded_empty_sdus = 0;
	state->media_progressed = 0;
	state->rx_decoded = 0;
}

int cm_leaudio_normal_end(struct cm_leaudio *state)
{
	return lifecycle_send(state, CM_LE_LIFECYCLE_NORMAL_END);
}

int cm_leaudio_set_empty_sdu_guard_us(struct cm_leaudio *state,
	uint32_t guard_us)
{
	if (!state || state->active)
		return -EBUSY;
	state->empty_sdu_guard_us = guard_us;
	return 0;
}

int cm_leaudio_rx_active(const struct cm_leaudio *state)
{
	return state && state->active && state->rx_active && state->decoder &&
		state->sink.fd >= 0;
}

int cm_leaudio_tx_active(const struct cm_leaudio *state)
{
	return state && state->active && state->tx_active && state->encoder &&
		state->source.fd >= 0;
}

void cm_leaudio_stop_rx(struct cm_leaudio *state)
{
	if (!state)
		return;
	free(state->decoder_memory);
	state->decoder_memory = NULL;
	state->decoder = NULL;
	state->rx_active = 0;
	stream_close(&state->sink);
	if (!state->tx_active)
		state->active = 0;
}

void cm_leaudio_stop_tx(struct cm_leaudio *state)
{
	if (!state)
		return;
	free(state->encoder_memory);
	state->encoder_memory = NULL;
	state->encoder = NULL;
	state->tx_active = 0;
	stream_close(&state->source);
	if (!state->rx_active)
		state->active = 0;
}

void cm_leaudio_reset_staged(struct cm_leaudio *state)
{
	if (!state || state->active)
		return;
	stream_close(&state->sink);
	stream_close(&state->source);
	lifecycle_close(state);
}

int cm_le_descriptor_decode(const uint8_t *packet, size_t packet_length,
	struct cm_le_descriptor *descriptor)
{
	size_t path_length;
	uint16_t octets;
	uint16_t read_mtu;
	uint16_t write_mtu;
	uint8_t flags;
	enum cm_le_direction direction;

	if (!packet || !descriptor || packet_length < CM_LE_HANDOFF_HEADER_SIZE)
		return -EINVAL;
	flags = packet[7];
	if (memcmp(packet, handoff_magic, sizeof(handoff_magic)) != 0 ||
		packet[4] != CM_LE_HANDOFF_VERSION || packet[6] != 1 ||
			 (flags & ~(CM_LE_HANDOFF_FLAG_OWNERSHIP |
			 CM_LE_HANDOFF_FLAG_LINKED | CM_LE_HANDOFF_FLAG_CALL_TOKEN |
			 CM_LE_HANDOFF_FLAG_LIFECYCLE)) != 0 ||
		(flags & CM_LE_HANDOFF_FLAG_OWNERSHIP) == 0 ||
		(flags & CM_LE_HANDOFF_FLAG_LINKED) == 0 ||
		get_le32(packet + 68) != 0)
		return -EPROTO;

	direction = (enum cm_le_direction) packet[5];
	if (direction != CM_LE_DIRECTION_SINK &&
		direction != CM_LE_DIRECTION_SOURCE)
		return -EPROTO;

	path_length = get_le16(packet + 66);
	if (path_length == 0 || path_length > CM_LE_MAX_TRANSPORT_PATH ||
		packet_length != CM_LE_HANDOFF_HEADER_SIZE + path_length)
		return -EMSGSIZE;
	if (packet[CM_LE_HANDOFF_HEADER_SIZE] != '/' ||
		memchr(packet + CM_LE_HANDOFF_HEADER_SIZE, '\0', path_length))
		return -EPROTO;

	memset(descriptor, 0, sizeof(*descriptor));
	descriptor->direction = direction;
	descriptor->generation = get_le64(packet + 8);
	descriptor->bundle_id = get_le64(packet + 16);
	descriptor->call_control_token = get_le64(packet + 24);
	descriptor->sample_rate = get_le32(packet + 32);
	descriptor->frame_duration_us = get_le32(packet + 36);
	descriptor->channel_allocation = get_le32(packet + 40);
	descriptor->interval_us = get_le32(packet + 44);
	descriptor->presentation_delay_us = get_le32(packet + 48);
	descriptor->octets_per_frame = get_le16(packet + 52);
	descriptor->read_mtu = get_le16(packet + 54);
	descriptor->write_mtu = get_le16(packet + 56);
	descriptor->latency_ms = get_le16(packet + 58);
	descriptor->framing = packet[60];
	descriptor->phy = packet[61];
	descriptor->retransmissions = packet[62];
	descriptor->target_latency = packet[63];
	descriptor->cig = packet[64];
	descriptor->cis = packet[65];
	descriptor->call_control_correlated =
		(flags & CM_LE_HANDOFF_FLAG_CALL_TOKEN) != 0;
	descriptor->lifecycle_owned =
		(flags & CM_LE_HANDOFF_FLAG_LIFECYCLE) != 0;

	octets = expected_octets(descriptor->sample_rate,
		descriptor->frame_duration_us);
	read_mtu = descriptor->read_mtu;
	write_mtu = descriptor->write_mtu;
	if (descriptor->generation == 0 || descriptor->bundle_id == 0 ||
		octets == 0 || descriptor->octets_per_frame != octets)
		return -EPROTO;
	if (!one_bit_set(descriptor->channel_allocation) ||
		descriptor->interval_us != descriptor->frame_duration_us ||
		descriptor->presentation_delay_us == 0 ||
		descriptor->presentation_delay_us > 0x00ffffffU ||
		descriptor->latency_ms == 0 || descriptor->latency_ms > 100 ||
		descriptor->framing > 1 || (descriptor->phy & 0x02U) == 0 ||
		descriptor->retransmissions == 0 ||
		descriptor->retransmissions > 13 ||
		descriptor->target_latency < 1 ||
		descriptor->target_latency > 3 ||
		descriptor->cig > 0xef || descriptor->cis > 0xef)
		return -EPROTO;
	if (descriptor->call_control_correlated !=
		(descriptor->call_control_token != 0))
		return -EPROTO;
	if (descriptor->lifecycle_owned !=
		(descriptor->direction == CM_LE_DIRECTION_SINK))
		return -EPROTO;
	if ((read_mtu == 0 && write_mtu == 0) ||
		read_mtu > 4096 || write_mtu > 4096)
		return -EPROTO;
	if ((direction == CM_LE_DIRECTION_SINK && read_mtu < octets) ||
		(direction == CM_LE_DIRECTION_SOURCE && write_mtu < octets))
		return -EMSGSIZE;

	memcpy(descriptor->transport,
		packet + CM_LE_HANDOFF_HEADER_SIZE, path_length);
	descriptor->transport[path_length] = '\0';
	return 0;
}

int cm_le_handoff_connect(const char *path)
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

int cm_le_handoff_receive(int socket_fd, struct cm_le_descriptor *descriptor,
	int *media_fd, int *lifecycle_fd)
{
	uint8_t packet[CM_LE_HANDOFF_HEADER_SIZE + CM_LE_MAX_TRANSPORT_PATH];
	uint8_t control[CMSG_SPACE(sizeof(int) * 4)];
	struct iovec iov;
	struct msghdr message;
	struct cmsghdr *cmsg;
	int received_fds[4];
	size_t fd_count = 0;
	int malformed_rights = 0;
	ssize_t received;
	int result;
	size_t index;

	if (socket_fd < 0 || !descriptor || !media_fd || !lifecycle_fd)
		return -EINVAL;
	*media_fd = -1;
	*lifecycle_fd = -1;
	memset(&message, 0, sizeof(message));
	memset(control, 0, sizeof(control));
	iov.iov_base = packet;
	iov.iov_len = sizeof(packet);
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	received = recvmsg(socket_fd, &message,
		MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
	if (received < 0)
		return -errno;
	if (received == 0)
		return -ECONNRESET;
	for (cmsg = CMSG_FIRSTHDR(&message); cmsg;
		cmsg = CMSG_NXTHDR(&message, cmsg)) {
		size_t bytes;
		size_t count;

		if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
			cmsg->cmsg_len < CMSG_LEN(0))
			continue;
		bytes = cmsg->cmsg_len - CMSG_LEN(0);
		if (bytes % sizeof(int) != 0)
			malformed_rights = 1;
		count = bytes / sizeof(int);
		for (index = 0; index < count; ++index) {
			int descriptor_fd;

			memcpy(&descriptor_fd,
				(uint8_t *) CMSG_DATA(cmsg) + index * sizeof(int),
				sizeof(descriptor_fd));
			if (fd_count < sizeof(received_fds) / sizeof(received_fds[0]))
				received_fds[fd_count++] = descriptor_fd;
			else
				close(descriptor_fd);
		}
	}

	if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
		malformed_rights || (fd_count != 1 && fd_count != 2)) {
		for (index = 0; index < fd_count; ++index)
			close(received_fds[index]);
		return (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ?
			-EMSGSIZE : -EPROTO;
	}
	result = cm_le_descriptor_decode(packet, (size_t) received, descriptor);
	if (result < 0) {
		for (index = 0; index < fd_count; ++index)
			close(received_fds[index]);
		return result;
	}
	if (fd_count != (descriptor->lifecycle_owned ? 2U : 1U)) {
		for (index = 0; index < fd_count; ++index)
			close(received_fds[index]);
		return -EPROTO;
	}

	{
		int flags = fcntl(received_fds[0], F_GETFL, 0);
		int descriptor_flags = fcntl(received_fds[0], F_GETFD, 0);

		if (flags < 0 || descriptor_flags < 0 ||
			fcntl(received_fds[0], F_SETFL, flags | O_NONBLOCK) < 0 ||
			fcntl(received_fds[0], F_SETFD,
				descriptor_flags | FD_CLOEXEC) < 0) {
			int saved_errno = errno;
			close(received_fds[0]);
			return -saved_errno;
		}
	}
	if (descriptor->lifecycle_owned) {
		int flags = fcntl(received_fds[1], F_GETFL, 0);
		int descriptor_flags = fcntl(received_fds[1], F_GETFD, 0);

		if (flags < 0 || descriptor_flags < 0 ||
			fcntl(received_fds[1], F_SETFL, flags | O_NONBLOCK) < 0 ||
			fcntl(received_fds[1], F_SETFD,
				descriptor_flags | FD_CLOEXEC) < 0) {
			int saved_errno = errno;
			close(received_fds[0]);
			close(received_fds[1]);
			return -saved_errno;
		}
	}

	if (descriptor->direction == CM_LE_DIRECTION_SINK) {
		int enabled = 1;

		/* Kernels without packet-status reporting may reject this option.
		 * The descriptor remains usable; short SDUs still take the PLC path. */
		(void) setsockopt(received_fds[0], SOL_BLUETOOTH,
			BT_PKT_STATUS, &enabled, sizeof(enabled));
	}
	*media_fd = received_fds[0];
	if (descriptor->lifecycle_owned)
		*lifecycle_fd = received_fds[1];
	return 0;
}

int cm_le_descriptor_matches_address(const struct cm_le_descriptor *descriptor,
	const char *address)
{
	char needle[32];
	size_t output = 0;
	size_t index;

	if (!descriptor || !address || strlen(address) != 17)
		return 0;
	memcpy(needle, "/dev_", 5);
	output = 5;
	for (index = 0; address[index] != '\0'; ++index) {
		char value = address[index];

		if (index % 3 == 2) {
			if (value != ':')
				return 0;
			value = '_';
		} else if (!((value >= '0' && value <= '9') ||
			(value >= 'a' && value <= 'f') ||
			(value >= 'A' && value <= 'F'))) {
			return 0;
		}
		if (value >= 'a' && value <= 'f')
			value = (char) (value - ('a' - 'A'));
		needle[output++] = value;
	}
	needle[output++] = '/';
	needle[output] = '\0';
	return strstr(descriptor->transport, needle) != NULL;
}

static int descriptor_matches_stream(const struct cm_le_descriptor *descriptor,
	const struct cm_le_stream *stream)
{
	return descriptor->bundle_id == stream->bundle_id &&
		descriptor->sample_rate == stream->sample_rate &&
		descriptor->frame_duration_us == stream->frame_duration_us &&
		descriptor->octets_per_frame == stream->octets_per_frame &&
		descriptor->interval_us == stream->interval_us &&
		descriptor->framing == stream->framing &&
		descriptor->phy == stream->phy && descriptor->cig == stream->cig &&
		descriptor->cis == stream->cis &&
		descriptor->call_control_correlated ==
			stream->call_control_correlated &&
		descriptor->call_control_token == stream->call_control_token;
}

static int streams_compatible(const struct cm_le_stream *sink,
	const struct cm_le_stream *source)
{
	return sink->fd >= 0 && source->fd >= 0 &&
		sink->bundle_id == source->bundle_id &&
		sink->sample_rate == source->sample_rate &&
		sink->frame_duration_us == source->frame_duration_us &&
		sink->octets_per_frame == source->octets_per_frame &&
		sink->interval_us == source->interval_us &&
		sink->framing == source->framing && sink->phy == source->phy &&
		sink->cig == source->cig && sink->cis == source->cis &&
		sink->call_control_correlated == source->call_control_correlated &&
		sink->call_control_token == source->call_control_token;
}

int cm_leaudio_install_with_lifecycle(struct cm_leaudio *state,
	const struct cm_le_descriptor *descriptor, int media_fd,
	int lifecycle_fd)
{
	struct cm_le_stream *stream;
	struct cm_le_stream *opposite;
	uint64_t *highest_generation;

	if (!state || !descriptor || media_fd < 0)
		return -EINVAL;
	if (descriptor->lifecycle_owned) {
		if (descriptor->direction != CM_LE_DIRECTION_SINK || lifecycle_fd < 0)
			return -EINVAL;
	} else if (lifecycle_fd >= 0) {
		return -EINVAL;
	}
	if (state->active)
		return -EBUSY;
	if (descriptor->direction == CM_LE_DIRECTION_SINK) {
		stream = &state->sink;
		opposite = &state->source;
		highest_generation = &state->highest_sink_generation;
	} else if (descriptor->direction == CM_LE_DIRECTION_SOURCE) {
		stream = &state->source;
		opposite = &state->sink;
		highest_generation = &state->highest_source_generation;
	} else {
		return -EINVAL;
	}
	if (descriptor->generation <= *highest_generation)
		return -ESTALE;
	if (descriptor->bundle_id <= state->highest_bundle_id)
		return -ESTALE;
	if (expected_octets(descriptor->sample_rate,
		descriptor->frame_duration_us) !=
		descriptor->octets_per_frame ||
		descriptor->interval_us != descriptor->frame_duration_us ||
		!one_bit_set(descriptor->channel_allocation))
		return -EINVAL;
	if (opposite->fd >= 0 && opposite->bundle_id != descriptor->bundle_id)
		cm_leaudio_reset_staged(state);
	else if (opposite->fd >= 0 &&
		!descriptor_matches_stream(descriptor, opposite)) {
		cm_leaudio_reset_staged(state);
		return -EPROTO;
	}

	stream_close(stream);
	if (descriptor->direction == CM_LE_DIRECTION_SINK) {
		lifecycle_close(state);
		if (descriptor->lifecycle_owned)
			state->lifecycle_fd = lifecycle_fd;
	}
	stream->fd = media_fd;
	stream->generation = descriptor->generation;
	stream->bundle_id = descriptor->bundle_id;
	stream->call_control_token = descriptor->call_control_token;
	stream->sample_rate = descriptor->sample_rate;
	stream->frame_duration_us = descriptor->frame_duration_us;
	stream->channel_allocation = descriptor->channel_allocation;
	stream->interval_us = descriptor->interval_us;
	stream->presentation_delay_us = descriptor->presentation_delay_us;
	stream->octets_per_frame = descriptor->octets_per_frame;
	stream->read_mtu = descriptor->read_mtu;
	stream->write_mtu = descriptor->write_mtu;
	stream->latency_ms = descriptor->latency_ms;
	stream->framing = descriptor->framing;
	stream->phy = descriptor->phy;
	stream->retransmissions = descriptor->retransmissions;
	stream->target_latency = descriptor->target_latency;
	stream->cig = descriptor->cig;
	stream->cis = descriptor->cis;
	stream->call_control_correlated =
		descriptor->call_control_correlated;
	*highest_generation = descriptor->generation;
	if (streams_compatible(&state->sink, &state->source))
		state->highest_bundle_id = descriptor->bundle_id;
	return 0;
}

int cm_leaudio_install(struct cm_leaudio *state,
	const struct cm_le_descriptor *descriptor, int media_fd)
{
	return cm_leaudio_install_with_lifecycle(state, descriptor, media_fd, -1);
}

int cm_leaudio_ready(const struct cm_leaudio *state)
{
	if (!state || state->active || state->sink.fd < 0 || state->source.fd < 0)
		return 0;
	return streams_compatible(&state->sink, &state->source);
}

int cm_leaudio_begin(struct cm_leaudio *state)
{
	unsigned encoder_size;
	unsigned decoder_size;
	lc3_encoder_t encoder;
	lc3_decoder_t decoder;

	if (!cm_leaudio_ready(state))
		return -EAGAIN;
	encoder_size = lc3_encoder_size((int) state->source.frame_duration_us,
		(int) state->source.sample_rate);
	decoder_size = lc3_decoder_size((int) state->sink.frame_duration_us,
		(int) state->sink.sample_rate);
	if (encoder_size == 0 || decoder_size == 0)
		return -EINVAL;
	state->encoder_memory = malloc(encoder_size);
	state->decoder_memory = malloc(decoder_size);
	if (!state->encoder_memory || !state->decoder_memory) {
		free(state->encoder_memory);
		free(state->decoder_memory);
		state->encoder_memory = NULL;
		state->decoder_memory = NULL;
		return -ENOMEM;
	}
	encoder = lc3_setup_encoder((int) state->source.frame_duration_us,
		(int) state->source.sample_rate, 0, state->encoder_memory);
	decoder = lc3_setup_decoder((int) state->sink.frame_duration_us,
		(int) state->sink.sample_rate, 0, state->decoder_memory);
	if (!encoder || !decoder) {
		free(state->encoder_memory);
		free(state->decoder_memory);
		state->encoder_memory = NULL;
		state->decoder_memory = NULL;
		return -EINVAL;
	}
	state->encoder = encoder;
	state->decoder = decoder;
	state->active_rate = state->sink.sample_rate;
	state->active_frame_duration_us = state->sink.frame_duration_us;
	state->active_octets = state->sink.octets_per_frame;
	memset(&state->io_stats, 0, sizeof(state->io_stats));
	state->empty_guard_tx_baseline = 0;
	state->guarded_empty_sdus = 0;
	state->media_progressed = 0;
	state->rx_decoded = 0;
	state->active = 1;
	state->rx_active = 1;
	state->tx_active = 1;
	return 0;
}

int cm_leaudio_read_fd(const struct cm_leaudio *state)
{
	return cm_leaudio_rx_active(state) ? state->sink.fd : -1;
}

const struct cm_leaudio_io_stats *cm_leaudio_get_io_stats(
	const struct cm_leaudio *state)
{
	return state ? &state->io_stats : NULL;
}

const char *cm_leaudio_io_stage_name(enum cm_leaudio_io_stage stage)
{
	switch (stage) {
	case CM_LE_IO_NONE:
		return "none";
	case CM_LE_IO_RX_STATE:
		return "rx-state";
	case CM_LE_IO_RX_POLL:
		return "rx-poll";
	case CM_LE_IO_RX_RECV:
		return "rx-recv";
	case CM_LE_IO_RX_EMPTY_GUARD:
		return "rx-empty-guard";
	case CM_LE_IO_RX_DECODE:
		return "rx-decode";
	case CM_LE_IO_TX_STATE:
		return "tx-state";
	case CM_LE_IO_TX_ENCODE:
		return "tx-encode";
	case CM_LE_IO_TX_SEND:
		return "tx-send";
	default:
		return "unknown";
	}
}

int cm_leaudio_sample_rate(const struct cm_leaudio *state)
{
	return state && state->active ? (int) state->active_rate : 0;
}

int cm_leaudio_frame_duration_us(const struct cm_leaudio *state)
{
	return state && state->active ?
		(int) state->active_frame_duration_us : 0;
}

int cm_leaudio_frame_samples(const struct cm_leaudio *state)
{
	if (!state || !state->active)
		return 0;
	return lc3_frame_samples((int) state->active_frame_duration_us,
		(int) state->active_rate);
}

int cm_leaudio_poll_is_terminal(short revents)
{
	return (revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
}

int cm_leaudio_get_session_id(const struct cm_leaudio *state,
	struct cm_leaudio_session_id *session)
{
	if (!state || !session)
		return -EINVAL;
	if (!state->active || state->sink.fd < 0 || state->source.fd < 0)
		return -EAGAIN;
	session->bundle_id = state->sink.bundle_id;
	session->sink_generation = state->sink.generation;
	session->source_generation = state->source.generation;
	return 0;
}

int cm_leaudio_session_matches(const struct cm_leaudio *state,
	const struct cm_leaudio_session_id *session)
{
	return state && session && state->active &&
		state->sink.fd >= 0 && state->source.fd >= 0 &&
		state->sink.bundle_id == session->bundle_id &&
		state->source.bundle_id == session->bundle_id &&
		state->sink.generation == session->sink_generation &&
		state->source.generation == session->source_generation;
}

int cm_leaudio_read(struct cm_leaudio *state, int16_t *pcm,
	size_t pcm_capacity, int *concealed)
{
	uint8_t encoded[CM_LE_MAX_FRAME_OCTETS];
	uint8_t control[CMSG_SPACE(sizeof(uint8_t))];
	struct iovec iov;
	struct msghdr message;
	struct cmsghdr *cmsg;
	struct pollfd poll_descriptor;
	uint8_t packet_status = 0;
	int samples;
	ssize_t received;
	int decode_result;
	int use_plc = 0;
	int result;

	if (!state)
		return -EINVAL;
	state->io_stats.read_calls++;
	state->io_stats.last_rx_error_stage = CM_LE_IO_NONE;
	state->io_stats.last_rx_error_result = 0;
	state->io_stats.last_rx_error_detail = 0;
	state->io_stats.last_rx_sdu_length = -1;
	if (!cm_leaudio_rx_active(state) || !pcm) {
		result = -EINVAL;
		state->io_stats.last_rx_error_stage = CM_LE_IO_RX_STATE;
		state->io_stats.last_rx_error_result = result;
		record_first_error(&state->io_stats, CM_LE_IO_RX_STATE, result, 0,
			-1);
		return result;
	}
	samples = cm_leaudio_frame_samples(state);
	if (samples <= 0 || pcm_capacity < (size_t) samples) {
		result = -ENOSPC;
		state->io_stats.last_rx_error_stage = CM_LE_IO_RX_STATE;
		state->io_stats.last_rx_error_result = result;
		record_first_error(&state->io_stats, CM_LE_IO_RX_STATE, result, 0,
			-1);
		return result;
	}
	if (concealed)
		*concealed = 0;

	memset(&poll_descriptor, 0, sizeof(poll_descriptor));
	poll_descriptor.fd = state->sink.fd;
	poll_descriptor.events = POLLIN;
	result = poll(&poll_descriptor, 1, 0);
	if (result < 0) {
		int saved_errno = errno;

		if (saved_errno == EINTR) {
			state->io_stats.read_retries++;
			return 0;
		}
		result = -saved_errno;
		state->io_stats.poll_errors++;
		state->io_stats.receive_errors++;
		state->io_stats.last_rx_error_stage = CM_LE_IO_RX_POLL;
		state->io_stats.last_rx_error_result = result;
		state->io_stats.last_rx_error_detail = saved_errno;
		record_first_error(&state->io_stats, CM_LE_IO_RX_POLL, result,
			saved_errno, -1);
		return result;
	}
	if (result > 0 && cm_leaudio_poll_is_terminal(poll_descriptor.revents)) {
		int socket_error = 0;
		socklen_t socket_error_length = sizeof(socket_error);

		if (poll_descriptor.revents & POLLHUP)
			state->io_stats.poll_hups++;
		if (poll_descriptor.revents & (POLLERR | POLLNVAL))
			state->io_stats.poll_errors++;
		if (getsockopt(state->sink.fd, SOL_SOCKET, SO_ERROR, &socket_error,
			&socket_error_length) < 0)
			socket_error = errno;
		if (socket_error == 0)
			socket_error = ECONNRESET;
		result = -socket_error;
		state->io_stats.receive_errors++;
		state->io_stats.last_rx_error_stage = CM_LE_IO_RX_POLL;
		state->io_stats.last_rx_error_result = result;
		state->io_stats.last_rx_error_detail = poll_descriptor.revents;
		record_first_error(&state->io_stats, CM_LE_IO_RX_POLL, result,
			poll_descriptor.revents, -1);
		return result;
	}

	memset(&message, 0, sizeof(message));
	iov.iov_base = encoded;
	iov.iov_len = state->active_octets;
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	received = recvmsg(state->sink.fd, &message, MSG_DONTWAIT);
	if (received < 0) {
		int saved_errno = errno;

		if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK ||
			saved_errno == EINTR) {
			state->io_stats.read_retries++;
			return 0;
		}
		result = -saved_errno;
		state->io_stats.receive_errors++;
		state->io_stats.last_rx_error_stage = CM_LE_IO_RX_RECV;
		state->io_stats.last_rx_error_result = result;
		state->io_stats.last_rx_error_detail = saved_errno;
		record_first_error(&state->io_stats, CM_LE_IO_RX_RECV, result,
			saved_errno, -1);
		return result;
	}
	state->io_stats.last_rx_sdu_length = received;
	if (received == 0) {
		uint64_t guard_frames = 0;

		state->io_stats.empty_sdus++;
		state->io_stats.consecutive_empty_sdus++;
		if (state->io_stats.consecutive_empty_sdus >
			state->io_stats.max_consecutive_empty_sdus)
			state->io_stats.max_consecutive_empty_sdus =
				state->io_stats.consecutive_empty_sdus;
		/* Empty ISO SDUs before the first real RX or successful TX are startup,
		 * not proof of a dead stream.  Only a stream that has demonstrated media
		 * progress may arm the bounded no-progress guard. */
		if (state->media_progressed && state->empty_sdu_guard_us != 0) {
			if (state->guarded_empty_sdus == 0) {
				state->empty_guard_tx_baseline =
					state->io_stats.send_successes;
				state->guarded_empty_sdus = 1;
			} else if (state->io_stats.send_successes !=
				state->empty_guard_tx_baseline) {
				state->io_stats.empty_sdu_guard_progress_resets++;
				state->empty_guard_tx_baseline =
					state->io_stats.send_successes;
				state->guarded_empty_sdus = 1;
			} else {
				state->guarded_empty_sdus++;
			}
			state->io_stats.guarded_consecutive_empty_sdus =
				state->guarded_empty_sdus;
			guard_frames = (state->empty_sdu_guard_us +
				state->active_frame_duration_us - 1) /
				state->active_frame_duration_us;
		}
		if (guard_frames != 0 &&
			state->guarded_empty_sdus >= guard_frames) {
			result = -ETIMEDOUT;
			state->io_stats.empty_sdu_guard_trips++;
			state->io_stats.last_rx_error_stage =
				CM_LE_IO_RX_EMPTY_GUARD;
			state->io_stats.last_rx_error_result = result;
			record_first_error(&state->io_stats,
				CM_LE_IO_RX_EMPTY_GUARD, result, 0, 0);
			return result;
		}
		use_plc = 1;
	} else if (received < state->active_octets) {
		state->io_stats.consecutive_empty_sdus = 0;
		state->guarded_empty_sdus = 0;
		state->io_stats.guarded_consecutive_empty_sdus = 0;
		state->io_stats.short_sdus++;
		use_plc = 1;
	} else {
		state->io_stats.consecutive_empty_sdus = 0;
		state->guarded_empty_sdus = 0;
		state->io_stats.guarded_consecutive_empty_sdus = 0;
		state->io_stats.full_sdus++;
	}
	if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
		state->io_stats.truncated_sdus++;
		use_plc = 1;
	}
	for (cmsg = CMSG_FIRSTHDR(&message); cmsg;
		cmsg = CMSG_NXTHDR(&message, cmsg)) {
		if (cmsg->cmsg_level == SOL_BLUETOOTH &&
			cmsg->cmsg_type == BT_SCM_PKT_STATUS &&
			cmsg->cmsg_len >= CMSG_LEN(sizeof(packet_status))) {
			memcpy(&packet_status, CMSG_DATA(cmsg), sizeof(packet_status));
			break;
		}
	}
	if (packet_status != 0) {
		state->io_stats.packet_status_errors++;
		use_plc = 1;
	}
	/* Concealment extrapolates from the previously decoded frame, but until
	 * the first real frame arrives the decoder has no such history. The
	 * remote fills the opening of every call with empty SDUs while its own
	 * audio path settles, so running PLC over that window asks the decoder
	 * to invent roughly 400 ms of audio from nothing. Emit true silence
	 * until media actually starts.
	 */
	if (use_plc && !state->rx_decoded) {
		memset(pcm, 0, (size_t) samples * sizeof(*pcm));
		state->io_stats.prehistory_silence_frames++;
		if (concealed)
			*concealed = 1;
		return samples;
	}
	state->io_stats.decode_calls++;
	decode_result = lc3_decode((lc3_decoder_t) state->decoder,
		use_plc ? NULL : encoded, state->active_octets,
		LC3_PCM_FORMAT_S16, pcm, 1);
	if (decode_result < 0) {
		result = -EBADMSG;
		state->io_stats.decode_errors++;
		state->io_stats.last_rx_error_stage = CM_LE_IO_RX_DECODE;
		state->io_stats.last_rx_error_result = result;
		state->io_stats.last_rx_error_detail = decode_result;
		record_first_error(&state->io_stats, CM_LE_IO_RX_DECODE, result,
			decode_result, received);
		return result;
	}
	state->io_stats.decode_successes++;
	if (use_plc || decode_result == 1)
		state->io_stats.decode_plc_frames++;
	if (concealed)
		*concealed = use_plc || decode_result == 1;
	if (!use_plc && decode_result == 0) {
		state->rx_decoded = 1;
		media_progress(state);
	}
	return samples;
}

int cm_leaudio_write(struct cm_leaudio *state, const int16_t *pcm,
	size_t pcm_samples)
{
	uint8_t encoded[CM_LE_MAX_FRAME_OCTETS];
	int samples;
	ssize_t written;
	int encode_result;
	int result;

	if (!state)
		return -EINVAL;
	state->io_stats.write_calls++;
	state->io_stats.last_tx_error_stage = CM_LE_IO_NONE;
	state->io_stats.last_tx_error_result = 0;
	state->io_stats.last_tx_error_detail = 0;
	if (!cm_leaudio_tx_active(state) || !pcm) {
		result = -EINVAL;
		state->io_stats.last_tx_error_stage = CM_LE_IO_TX_STATE;
		state->io_stats.last_tx_error_result = result;
		record_first_error(&state->io_stats, CM_LE_IO_TX_STATE, result, 0,
			-1);
		return result;
	}
	samples = cm_leaudio_frame_samples(state);
	if (samples <= 0 || pcm_samples != (size_t) samples) {
		result = -EMSGSIZE;
		state->io_stats.last_tx_error_stage = CM_LE_IO_TX_STATE;
		state->io_stats.last_tx_error_result = result;
		record_first_error(&state->io_stats, CM_LE_IO_TX_STATE, result, 0,
			-1);
		return result;
	}
	state->io_stats.encode_calls++;
	encode_result = lc3_encode((lc3_encoder_t) state->encoder,
		LC3_PCM_FORMAT_S16, pcm, 1, state->active_octets, encoded);
	if (encode_result < 0) {
		result = -EBADMSG;
		state->io_stats.encode_errors++;
		state->io_stats.last_tx_error_stage = CM_LE_IO_TX_ENCODE;
		state->io_stats.last_tx_error_result = result;
		state->io_stats.last_tx_error_detail = encode_result;
		record_first_error(&state->io_stats, CM_LE_IO_TX_ENCODE, result,
			encode_result, state->active_octets);
		return result;
	}
	state->io_stats.encode_successes++;
	state->io_stats.send_calls++;
	written = send(state->source.fd, encoded, state->active_octets,
		MSG_DONTWAIT | MSG_NOSIGNAL);
	if (written < 0) {
		int saved_errno = errno;

		result = -saved_errno;
		if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK ||
			saved_errno == EINTR)
			state->io_stats.send_retries++;
		else
			state->io_stats.send_errors++;
		state->io_stats.last_tx_error_stage = CM_LE_IO_TX_SEND;
		state->io_stats.last_tx_error_result = result;
		state->io_stats.last_tx_error_detail = saved_errno;
		record_first_error(&state->io_stats, CM_LE_IO_TX_SEND, result,
			saved_errno, state->active_octets);
		return result;
	}
	if (written != state->active_octets) {
		result = -EIO;
		state->io_stats.send_errors++;
		state->io_stats.last_tx_error_stage = CM_LE_IO_TX_SEND;
		state->io_stats.last_tx_error_result = result;
		state->io_stats.last_tx_error_detail = (int) written;
		record_first_error(&state->io_stats, CM_LE_IO_TX_SEND, result,
			(int) written, state->active_octets);
		return result;
	}
	state->io_stats.send_successes++;
	media_progress(state);
	return samples;
}

int cm_leaudio_read_session(struct cm_leaudio *state,
	const struct cm_leaudio_session_id *session, int16_t *pcm,
	size_t pcm_capacity, int *concealed)
{
	if (!state || !session)
		return -EINVAL;
	if (!cm_leaudio_session_matches(state, session)) {
		state->io_stats.read_calls++;
		state->io_stats.stale_rx_session_rejections++;
		state->io_stats.last_rx_error_stage = CM_LE_IO_RX_STATE;
		state->io_stats.last_rx_error_result = -ESTALE;
		state->io_stats.last_rx_error_detail = 0;
		state->io_stats.last_rx_sdu_length = -1;
		return -ESTALE;
	}
	return cm_leaudio_read(state, pcm, pcm_capacity, concealed);
}

int cm_leaudio_write_session(struct cm_leaudio *state,
	const struct cm_leaudio_session_id *session, const int16_t *pcm,
	size_t pcm_samples)
{
	if (!state || !session)
		return -EINVAL;
	if (!cm_leaudio_session_matches(state, session)) {
		state->io_stats.write_calls++;
		state->io_stats.stale_tx_session_rejections++;
		state->io_stats.last_tx_error_stage = CM_LE_IO_TX_STATE;
		state->io_stats.last_tx_error_result = -ESTALE;
		state->io_stats.last_tx_error_detail = 0;
		return -ESTALE;
	}
	return cm_leaudio_write(state, pcm, pcm_samples);
}
