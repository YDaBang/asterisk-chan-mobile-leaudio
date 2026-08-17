#include "chan_mobile_leaudio.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <lc3.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void put_le16(uint8_t *value, uint16_t number)
{
	value[0] = (uint8_t) number;
	value[1] = (uint8_t) (number >> 8);
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

static uint64_t get_le64(const uint8_t *value)
{
	return (uint64_t) value[0] |
		((uint64_t) value[1] << 8) |
		((uint64_t) value[2] << 16) |
		((uint64_t) value[3] << 24) |
		((uint64_t) value[4] << 32) |
		((uint64_t) value[5] << 40) |
		((uint64_t) value[6] << 48) |
		((uint64_t) value[7] << 56);
}

static size_t make_packet(uint8_t *packet, size_t capacity,
	enum cm_le_direction direction, uint64_t generation, uint64_t bundle_id,
	uint32_t rate, uint32_t frame_duration_us, uint16_t octets,
	const char *path)
{
	size_t path_length = strlen(path);
	size_t packet_length = CM_LE_HANDOFF_HEADER_SIZE + path_length;

	assert(capacity >= packet_length);
	memset(packet, 0, packet_length);
	memcpy(packet, "GGLE", 4);
	packet[4] = CM_LE_HANDOFF_VERSION;
	packet[5] = (uint8_t) direction;
	packet[6] = 1;
	packet[7] = CM_LE_HANDOFF_FLAG_OWNERSHIP | CM_LE_HANDOFF_FLAG_LINKED;
	if (direction == CM_LE_DIRECTION_SINK)
		packet[7] |= CM_LE_HANDOFF_FLAG_LIFECYCLE;
	put_le64(packet + 8, generation);
	put_le64(packet + 16, bundle_id);
	put_le64(packet + 24, 0);
	put_le32(packet + 32, rate);
	put_le32(packet + 36, frame_duration_us);
	put_le32(packet + 40, 0x00000004U);
	put_le32(packet + 44, frame_duration_us);
	put_le32(packet + 48, 40000);
	put_le16(packet + 52, octets);
	put_le16(packet + 54, octets);
	put_le16(packet + 56, octets);
	put_le16(packet + 58, 10);
	packet[60] = 0;
	packet[61] = 2;
	packet[62] = 2;
	packet[63] = 2;
	packet[64] = 1;
	packet[65] = 2;
	put_le16(packet + 66, (uint16_t) path_length);
	memcpy(packet + CM_LE_HANDOFF_HEADER_SIZE, path, path_length);
	return packet_length;
}

static void test_descriptor_and_rights(void)
{
	static const char path[] =
		"/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/fd1";
	uint8_t packet[CM_LE_HANDOFF_HEADER_SIZE + CM_LE_MAX_TRANSPORT_PATH];
	uint8_t control[CMSG_SPACE(sizeof(int) * 2)];
	struct cm_le_descriptor descriptor;
	struct iovec iov;
	struct msghdr message;
	struct cmsghdr *cmsg;
	int handoff[2];
	int media[2];
	int lifecycle[2];
	int rights[2];
	int received_fd = -1;
	int received_lifecycle_fd = -1;
	size_t length;

	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, handoff) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, media) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, lifecycle) == 0);
	length = make_packet(packet, sizeof(packet), CM_LE_DIRECTION_SINK,
		1, 7, 32000, CM_LE_FRAME_DURATION_10_US, 80, path);
	memset(&message, 0, sizeof(message));
	memset(control, 0, sizeof(control));
	iov.iov_base = packet;
	iov.iov_len = length;
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	cmsg = CMSG_FIRSTHDR(&message);
	assert(cmsg != NULL);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(rights));
	rights[0] = media[0];
	rights[1] = lifecycle[0];
	memcpy(CMSG_DATA(cmsg), rights, sizeof(rights));
	message.msg_controllen = CMSG_SPACE(sizeof(rights));
	assert(sendmsg(handoff[0], &message, 0) == (ssize_t) length);
	assert(cm_le_handoff_receive(handoff[1], &descriptor, &received_fd,
		&received_lifecycle_fd) == 0);
	assert(received_fd >= 0);
	assert(received_lifecycle_fd >= 0);
	assert(descriptor.direction == CM_LE_DIRECTION_SINK);
	assert(descriptor.generation == 1);
	assert(descriptor.bundle_id == 7);
	assert(descriptor.sample_rate == 32000);
	assert(descriptor.frame_duration_us == CM_LE_FRAME_DURATION_10_US);
	assert(descriptor.octets_per_frame == 80);
	assert(descriptor.channel_allocation == 0x00000004U);
	assert(descriptor.interval_us == CM_LE_FRAME_DURATION_10_US);
	assert(descriptor.phy == 2 && descriptor.cig == 1 && descriptor.cis == 2);
	assert(strcmp(descriptor.transport, path) == 0);
	assert(cm_le_descriptor_matches_address(&descriptor,
		"aa:bb:cc:dd:ee:ff"));
	assert(!cm_le_descriptor_matches_address(&descriptor,
		"00:11:22:33:44:55"));
	assert(!cm_le_descriptor_matches_address(&descriptor,
		"aa:bb:cc:dd:ee:fg"));
	assert(!cm_le_descriptor_matches_address(&descriptor,
		"aa-bb-cc-dd-ee-ff"));
	assert((fcntl(received_fd, F_GETFD) & FD_CLOEXEC) != 0);
	assert((fcntl(received_fd, F_GETFL) & O_NONBLOCK) != 0);
	assert((fcntl(received_lifecycle_fd, F_GETFD) & FD_CLOEXEC) != 0);
	assert((fcntl(received_lifecycle_fd, F_GETFL) & O_NONBLOCK) != 0);
	close(received_fd);
	close(received_lifecycle_fd);
	close(media[0]);
	close(media[1]);
	close(lifecycle[0]);
	close(lifecycle[1]);
	close(handoff[0]);
	close(handoff[1]);

	packet[6] = 0;
	assert(cm_le_descriptor_decode(packet, length, &descriptor) == -EPROTO);
	packet[6] = 1;
	packet[68] = 1;
	assert(cm_le_descriptor_decode(packet, length, &descriptor) == -EPROTO);
	packet[68] = 0;
	packet[7] |= 0x80;
	assert(cm_le_descriptor_decode(packet, length, &descriptor) == -EPROTO);
	packet[7] &= (uint8_t) ~0x80U;
	put_le64(packet + 24, 9);
	assert(cm_le_descriptor_decode(packet, length, &descriptor) == -EPROTO);
	put_le64(packet + 24, 0);
	packet[CM_LE_HANDOFF_HEADER_SIZE + 2] = '\0';
	assert(cm_le_descriptor_decode(packet, length, &descriptor) == -EPROTO);
}

static uint16_t octets_for_setting(uint32_t rate, uint32_t frame_duration_us)
{
	if (frame_duration_us == CM_LE_FRAME_DURATION_7P5_US)
		return rate == 16000 ? 30 : rate == 24000 ? 45 : 60;
	return rate == 16000 ? 40 : rate == 24000 ? 60 : 80;
}

static void fill_sine(int16_t *pcm, int samples, int rate)
{
	int index;

	for (index = 0; index < samples; ++index) {
		double phase = 2.0 * 3.14159265358979323846 * 1000.0 * index / rate;
		pcm[index] = (int16_t) (sin(phase) * 12000.0);
	}
}

static void fill_descriptor(struct cm_le_descriptor *descriptor,
	enum cm_le_direction direction, uint64_t generation, uint64_t bundle_id,
	uint32_t rate, uint32_t frame_duration_us, uint16_t octets)
{
	memset(descriptor, 0, sizeof(*descriptor));
	descriptor->direction = direction;
	descriptor->generation = generation;
	descriptor->bundle_id = bundle_id;
	descriptor->sample_rate = rate;
	descriptor->frame_duration_us = frame_duration_us;
	descriptor->channel_allocation = 0x00000004U;
	descriptor->interval_us = frame_duration_us;
	descriptor->presentation_delay_us = 40000;
	descriptor->octets_per_frame = octets;
	descriptor->read_mtu = octets;
	descriptor->write_mtu = octets;
	descriptor->latency_ms = 10;
	descriptor->framing = 0;
	descriptor->phy = 2;
	descriptor->retransmissions = 2;
	descriptor->target_latency = 2;
	descriptor->cig = 1;
	descriptor->cis = 2;
}

static void test_codec_setting(uint32_t rate, uint32_t frame_duration_us)
{
	struct cm_leaudio state;
	struct cm_le_descriptor sink;
	struct cm_le_descriptor source;
	lc3_encoder_t peer_encoder;
	lc3_decoder_t peer_decoder;
	void *peer_encoder_memory;
	void *peer_decoder_memory;
	uint8_t encoded[CM_LE_MAX_FRAME_OCTETS];
	int16_t input[320];
	int16_t decoded[320];
	int16_t output[320];
	int rx[2];
	int tx[2];
	int samples;
	int concealed = 0;
	int rejected_fd;
	ssize_t received;
	long long energy = 0;
	int index;
	uint16_t octets = octets_for_setting(rate, frame_duration_us);

	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, rx) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, tx) == 0);
	cm_leaudio_init(&state);
	fill_descriptor(&sink, CM_LE_DIRECTION_SINK, 1, 10, rate,
		frame_duration_us, octets);
	source = sink;
	source.direction = CM_LE_DIRECTION_SOURCE;
	source.generation = 2;
	/* BlueZ may deliver the two ASE directions in either order. */
	if (frame_duration_us == CM_LE_FRAME_DURATION_7P5_US) {
		assert(cm_leaudio_install(&state, &sink, dup(rx[0])) == 0);
		assert(!cm_leaudio_ready(&state));
		assert(cm_leaudio_install(&state, &source, dup(tx[0])) == 0);
	} else {
		assert(cm_leaudio_install(&state, &source, dup(tx[0])) == 0);
		assert(!cm_leaudio_ready(&state));
		assert(cm_leaudio_install(&state, &sink, dup(rx[0])) == 0);
	}
	assert(cm_leaudio_ready(&state));
	assert(cm_leaudio_begin(&state) == 0);
	assert(cm_leaudio_sample_rate(&state) == (int) rate);
	assert(cm_leaudio_frame_duration_us(&state) == (int) frame_duration_us);
	samples = cm_leaudio_frame_samples(&state);
	assert(samples == (int) (((uint64_t) rate * frame_duration_us) /
		1000000U));

	peer_encoder_memory = malloc(lc3_encoder_size((int) frame_duration_us,
		(int) rate));
	peer_decoder_memory = malloc(lc3_decoder_size((int) frame_duration_us,
		(int) rate));
	assert(peer_encoder_memory != NULL && peer_decoder_memory != NULL);
	peer_encoder = lc3_setup_encoder((int) frame_duration_us,
		(int) rate, 0, peer_encoder_memory);
	peer_decoder = lc3_setup_decoder((int) frame_duration_us,
		(int) rate, 0, peer_decoder_memory);
	assert(peer_encoder != NULL && peer_decoder != NULL);

	fill_sine(input, samples, (int) rate);
	assert(lc3_encode(peer_encoder, LC3_PCM_FORMAT_S16, input, 1,
		octets, encoded) == 0);
	received = send(rx[1], encoded, octets, 0);
	if (received != octets) {
		fprintf(stderr,
			"ISO test send failed rate=%u frame-us=%u result=%zd errno=%d\n",
			rate, frame_duration_us, received, errno);
		abort();
	}
	assert(cm_leaudio_read(&state, decoded,
		sizeof(decoded) / sizeof(decoded[0]), &concealed) == samples);
	assert(!concealed);
	for (index = 0; index < samples; ++index)
		energy += llabs(decoded[index]);
	assert(energy > samples * 100);

	assert(cm_leaudio_write(&state, input, (size_t) samples) == samples);
	received = recv(tx[1], encoded, sizeof(encoded), 0);
	assert(received == octets);
	assert(lc3_decode(peer_decoder, encoded, octets,
		LC3_PCM_FORMAT_S16, output, 1) >= 0);
	energy = 0;
	for (index = 0; index < samples; ++index)
		energy += llabs(output[index]);
	assert(energy > samples * 100);

	/* A short ISO SDU is converted to LC3 packet-loss concealment. */
	assert(send(rx[1], encoded, 1, 0) == 1);
	concealed = 0;
	assert(cm_leaudio_read(&state, decoded,
		sizeof(decoded) / sizeof(decoded[0]), &concealed) == samples);
	assert(concealed);

	/* A live call cannot have either direction replaced underneath it. */
	rejected_fd = dup(rx[0]);
	assert(rejected_fd >= 0);
	sink.generation = 3;
	assert(cm_leaudio_install(&state, &sink, rejected_fd) == -EBUSY);
	close(rejected_fd);

	free(peer_encoder_memory);
	free(peer_decoder_memory);
	cm_leaudio_end(&state);
	assert(cm_leaudio_read_fd(&state) == -1);
	/* The anti-replay generation survives call cleanup. */
	rejected_fd = dup(rx[0]);
	assert(rejected_fd >= 0);
	sink.generation = 1;
	assert(cm_leaudio_install(&state, &sink, rejected_fd) == -ESTALE);
	close(rejected_fd);
	rejected_fd = dup(tx[0]);
	assert(rejected_fd >= 0);
	source.generation = 2;
	assert(cm_leaudio_install(&state, &source, rejected_fd) == -ESTALE);
	close(rejected_fd);
	close(rx[0]);
	close(rx[1]);
	close(tx[0]);
	close(tx[1]);
}

static void test_empty_sdu_guard_requires_media_progress(void)
{
	struct cm_leaudio state;
	struct cm_le_descriptor sink;
	struct cm_le_descriptor source;
	const struct cm_leaudio_io_stats *stats;
	uint8_t encoded[CM_LE_MAX_FRAME_OCTETS];
	int16_t pcm[320];
	int16_t decoded[320];
	int rx[2];
	int tx[2];
	int samples;
	int concealed = 0;
	unsigned int startup_frames;
	unsigned int index;

	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, rx) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, tx) == 0);
	cm_leaudio_init(&state);
	fill_descriptor(&sink, CM_LE_DIRECTION_SINK, 1, 20, 32000,
		CM_LE_FRAME_DURATION_10_US, 80);
	source = sink;
	source.direction = CM_LE_DIRECTION_SOURCE;
	source.generation = 2;
	assert(cm_leaudio_install(&state, &sink, dup(rx[0])) == 0);
	assert(cm_leaudio_install(&state, &source, dup(tx[0])) == 0);
	/* The production timeout is intentionally not guessed in this harness.
	 * Three frames make the armed/no-progress state machine observable. */
	assert(cm_leaudio_set_empty_sdu_guard_us(&state, 30000) == 0);
	assert(cm_leaudio_begin(&state) == 0);
	assert(cm_leaudio_rx_active(&state));
	assert(cm_leaudio_tx_active(&state));
	samples = cm_leaudio_frame_samples(&state);
	assert(samples == 320);

	/* More than the old one-second limit of startup empties must not arm or
	 * trip the guard before either direction demonstrates real media. */
	startup_frames = 120;
	for (index = 0; index < startup_frames; ++index) {
		int sample;

		concealed = 0;
		/* Poison the buffer so that silence has to be written, not merely
		 * left over from a previous iteration. */
		memset(decoded, 0x5a, sizeof(decoded));
		assert(send(rx[1], encoded, 0, 0) == 0);
		assert(cm_leaudio_read(&state, decoded,
			sizeof(decoded) / sizeof(decoded[0]), &concealed) == samples);
		assert(concealed);
		/* Concealment has no decoded frame to extrapolate from before media
		 * starts, so the opening window must be true silence. */
		for (sample = 0; sample < samples; ++sample)
			assert(decoded[sample] == 0);
	}
	stats = cm_leaudio_get_io_stats(&state);
	assert(stats != NULL);
	assert(stats->read_calls == startup_frames);
	assert(stats->empty_sdus == startup_frames);
	assert(stats->prehistory_silence_frames == startup_frames);
	assert(stats->decode_calls == 0);
	assert(stats->decode_successes == 0);
	assert(stats->decode_plc_frames == 0);
	assert(stats->receive_errors == 0);
	assert(stats->decode_errors == 0);
	assert(stats->empty_sdu_guard_arms == 0);
	assert(stats->empty_sdu_guard_trips == 0);
	assert(stats->first_error_stage == CM_LE_IO_NONE);
	puts("PASS: startup empty SDUs emit silence without arming the guard");

	/* A successful TX arms the guard.  Progress on TX then resets an active
	 * empty window, so only three empties with no progress may trip it. */
	fill_sine(pcm, samples, 32000);
	assert(cm_leaudio_write(&state, pcm, (size_t) samples) == samples);
	assert(recv(tx[1], encoded, sizeof(encoded), 0) == 80);
	for (index = 0; index < 2; ++index) {
		concealed = 0;
		assert(send(rx[1], encoded, 0, 0) == 0);
		assert(cm_leaudio_read(&state, decoded,
			sizeof(decoded) / sizeof(decoded[0]), &concealed) == samples);
		assert(concealed);
	}
	assert(cm_leaudio_write(&state, pcm, (size_t) samples) == samples);
	assert(recv(tx[1], encoded, sizeof(encoded), 0) == 80);
	for (index = 0; index < 2; ++index) {
		concealed = 0;
		assert(send(rx[1], encoded, 0, 0) == 0);
		assert(cm_leaudio_read(&state, decoded,
			sizeof(decoded) / sizeof(decoded[0]), &concealed) == samples);
		assert(concealed);
	}
	assert(send(rx[1], encoded, 0, 0) == 0);
	assert(cm_leaudio_read(&state, decoded,
		sizeof(decoded) / sizeof(decoded[0]), &concealed) == -ETIMEDOUT);
	stats = cm_leaudio_get_io_stats(&state);
	assert(stats->empty_sdu_guard_arms == 1);
	assert(stats->empty_sdu_guard_progress_resets == 1);
	assert(stats->guarded_consecutive_empty_sdus == 3);
	assert(stats->empty_sdu_guard_trips == 1);
	assert(stats->first_error_stage == CM_LE_IO_RX_EMPTY_GUARD);
	assert(stats->first_error_result == -ETIMEDOUT);
	assert(stats->first_error_sdu_length == 0);
	puts("PASS: armed empty guard requires both RX empties and no TX progress");

	/* A terminal media result tears down the complete call-level pair. */
	cm_leaudio_end(&state);
	assert(!cm_leaudio_rx_active(&state));
	assert(!cm_leaudio_tx_active(&state));
	assert(cm_leaudio_read_fd(&state) == -1);
	close(rx[0]);
	close(rx[1]);
	close(tx[0]);
	close(tx[1]);
}

static void test_two_consecutive_calls_release_and_reacquire(void)
{
	struct cm_leaudio state;
	struct cm_le_descriptor sink;
	struct cm_le_descriptor source;
	lc3_encoder_t peer_encoder;
	void *peer_encoder_memory;
	uint8_t encoded[CM_LE_MAX_FRAME_OCTETS];
	int16_t input[320];
	int16_t decoded[320];
	int first_rx[2];
	int first_tx[2];
	int second_rx[2];
	int second_tx[2];
	int first_sink_fd;
	int first_source_fd;
	int concealed = 0;
	int samples;

	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, first_rx) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, first_tx) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, second_rx) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, second_tx) == 0);
	cm_leaudio_init(&state);
	fill_descriptor(&sink, CM_LE_DIRECTION_SINK, 1, 40, 32000,
		CM_LE_FRAME_DURATION_10_US, 80);
	source = sink;
	source.direction = CM_LE_DIRECTION_SOURCE;
	source.generation = 2;
	assert(cm_leaudio_install(&state, &sink, dup(first_rx[0])) == 0);
	assert(cm_leaudio_install(&state, &source, dup(first_tx[0])) == 0);
	assert(cm_leaudio_begin(&state) == 0);
	first_sink_fd = state.sink.fd;
	first_source_fd = state.source.fd;
	assert(first_sink_fd >= 0 && first_source_fd >= 0);

	/* The first call ends at the transport boundary. */
	close(first_rx[1]);
	assert(cm_leaudio_read(&state, decoded,
		sizeof(decoded) / sizeof(decoded[0]), &concealed) == -ECONNRESET);
	cm_leaudio_end(&state);
	errno = 0;
	assert(fcntl(first_sink_fd, F_GETFD) == -1 && errno == EBADF);
	errno = 0;
	assert(fcntl(first_source_fd, F_GETFD) == -1 && errno == EBADF);
	assert(!cm_leaudio_rx_active(&state) && !cm_leaudio_tx_active(&state));

	/* A higher-generation second call must acquire fresh descriptors and move
	 * real LC3 in both directions. */
	fill_descriptor(&sink, CM_LE_DIRECTION_SINK, 3, 41, 32000,
		CM_LE_FRAME_DURATION_10_US, 80);
	source = sink;
	source.direction = CM_LE_DIRECTION_SOURCE;
	source.generation = 4;
	assert(cm_leaudio_install(&state, &sink, dup(second_rx[0])) == 0);
	assert(cm_leaudio_install(&state, &source, dup(second_tx[0])) == 0);
	assert(cm_leaudio_begin(&state) == 0);
	samples = cm_leaudio_frame_samples(&state);
	assert(samples == 320);
	peer_encoder_memory = malloc(lc3_encoder_size(
		CM_LE_FRAME_DURATION_10_US, 32000));
	assert(peer_encoder_memory != NULL);
	peer_encoder = lc3_setup_encoder(CM_LE_FRAME_DURATION_10_US, 32000, 0,
		peer_encoder_memory);
	assert(peer_encoder != NULL);
	fill_sine(input, samples, 32000);
	assert(lc3_encode(peer_encoder, LC3_PCM_FORMAT_S16, input, 1, 80,
		encoded) == 0);
	assert(send(second_rx[1], encoded, 80, 0) == 80);
	concealed = 0;
	assert(cm_leaudio_read(&state, decoded,
		sizeof(decoded) / sizeof(decoded[0]), &concealed) == samples);
	assert(!concealed);
	assert(cm_leaudio_write(&state, input, (size_t) samples) == samples);
	assert(recv(second_tx[1], encoded, sizeof(encoded), 0) == 80);
	free(peer_encoder_memory);
	cm_leaudio_end(&state);
	puts("PASS: first call releases both FDs and second call reacquires media");

	close(first_rx[0]);
	close(first_tx[0]);
	close(first_tx[1]);
	close(second_rx[0]);
	close(second_rx[1]);
	close(second_tx[0]);
	close(second_tx[1]);
}

static void test_stale_session_cannot_touch_reused_fd_numbers(void)
{
	enum { reused_sink_fd = 200, reused_source_fd = 201 };
	struct cm_leaudio state;
	struct cm_le_descriptor sink;
	struct cm_le_descriptor source;
	struct cm_leaudio_session_id first_session;
	struct cm_leaudio_session_id second_session;
	const struct cm_leaudio_io_stats *stats;
	lc3_encoder_t peer_encoder;
	void *peer_encoder_memory;
	uint8_t encoded[CM_LE_MAX_FRAME_OCTETS];
	int16_t pcm[320];
	int16_t decoded[320];
	struct pollfd wait_fd;
	int first_rx[2];
	int first_tx[2];
	int second_rx[2];
	int second_tx[2];
	int concealed = 0;
	int samples;

	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, first_rx) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, first_tx) == 0);
	assert(dup2(first_rx[0], reused_sink_fd) == reused_sink_fd);
	assert(dup2(first_tx[0], reused_source_fd) == reused_source_fd);
	cm_leaudio_init(&state);
	fill_descriptor(&sink, CM_LE_DIRECTION_SINK, 1, 50, 32000,
		CM_LE_FRAME_DURATION_10_US, 80);
	source = sink;
	source.direction = CM_LE_DIRECTION_SOURCE;
	source.generation = 2;
	assert(cm_leaudio_install(&state, &sink, reused_sink_fd) == 0);
	assert(cm_leaudio_install(&state, &source, reused_source_fd) == 0);
	assert(cm_leaudio_begin(&state) == 0);
	assert(cm_leaudio_get_session_id(&state, &first_session) == 0);
	cm_leaudio_end(&state);

	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, second_rx) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, second_tx) == 0);
	assert(second_rx[0] != reused_sink_fd && second_rx[1] != reused_sink_fd &&
		second_tx[0] != reused_sink_fd && second_tx[1] != reused_sink_fd);
	assert(second_rx[0] != reused_source_fd && second_rx[1] != reused_source_fd &&
		second_tx[0] != reused_source_fd && second_tx[1] != reused_source_fd);
	assert(dup2(second_rx[0], reused_sink_fd) == reused_sink_fd);
	assert(dup2(second_tx[0], reused_source_fd) == reused_source_fd);
	fill_descriptor(&sink, CM_LE_DIRECTION_SINK, 3, 51, 32000,
		CM_LE_FRAME_DURATION_10_US, 80);
	source = sink;
	source.direction = CM_LE_DIRECTION_SOURCE;
	source.generation = 4;
	assert(cm_leaudio_install(&state, &sink, reused_sink_fd) == 0);
	assert(cm_leaudio_install(&state, &source, reused_source_fd) == 0);
	assert(cm_leaudio_begin(&state) == 0);
	assert(cm_leaudio_get_session_id(&state, &second_session) == 0);
	assert(!cm_leaudio_session_matches(&state, &first_session));
	assert(cm_leaudio_session_matches(&state, &second_session));

	samples = cm_leaudio_frame_samples(&state);
	assert(samples == 320);
	peer_encoder_memory = malloc(lc3_encoder_size(
		CM_LE_FRAME_DURATION_10_US, 32000));
	assert(peer_encoder_memory != NULL);
	peer_encoder = lc3_setup_encoder(CM_LE_FRAME_DURATION_10_US, 32000, 0,
		peer_encoder_memory);
	assert(peer_encoder != NULL);
	fill_sine(pcm, samples, 32000);
	assert(lc3_encode(peer_encoder, LC3_PCM_FORMAT_S16, pcm, 1, 80,
		encoded) == 0);
	assert(send(second_rx[1], encoded, 80, 0) == 80);

	assert(cm_leaudio_read_session(&state, &first_session, decoded,
		sizeof(decoded) / sizeof(decoded[0]), &concealed) == -ESTALE);
	assert(cm_leaudio_write_session(&state, &first_session, pcm,
		(size_t) samples) == -ESTALE);
	memset(&wait_fd, 0, sizeof(wait_fd));
	wait_fd.fd = second_tx[1];
	wait_fd.events = POLLIN;
	assert(poll(&wait_fd, 1, 0) == 0);

	assert(cm_leaudio_read_session(&state, &second_session, decoded,
		sizeof(decoded) / sizeof(decoded[0]), &concealed) == samples);
	assert(!concealed);
	assert(cm_leaudio_write_session(&state, &second_session, pcm,
		(size_t) samples) == samples);
	assert(recv(second_tx[1], encoded, sizeof(encoded), 0) == 80);
	stats = cm_leaudio_get_io_stats(&state);
	assert(stats->stale_rx_session_rejections == 1);
	assert(stats->stale_tx_session_rejections == 1);
	puts("PASS: stale call I/O cannot touch reused call descriptor numbers");

	free(peer_encoder_memory);
	cm_leaudio_end(&state);
	close(first_rx[0]);
	close(first_rx[1]);
	close(first_tx[0]);
	close(first_tx[1]);
	close(second_rx[0]);
	close(second_rx[1]);
	close(second_tx[0]);
	close(second_tx[1]);
}

static void test_lifecycle_progress_is_media_driven(void)
{
	struct cm_leaudio state;
	struct cm_le_descriptor sink;
	struct cm_le_descriptor source;
	struct cm_leaudio_session_id session;
	lc3_encoder_t peer_encoder;
	void *peer_encoder_memory;
	uint8_t encoded[CM_LE_MAX_FRAME_OCTETS];
	uint8_t message[CM_LE_LIFECYCLE_MESSAGE_SIZE];
	int16_t pcm[320];
	int16_t decoded[320];
	struct pollfd wait_fd;
	int rx[2];
	int tx[2];
	int lifecycle[2];
	int concealed = 0;
	int samples;

	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, rx) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, tx) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, lifecycle) == 0);
	cm_leaudio_init(&state);
	fill_descriptor(&sink, CM_LE_DIRECTION_SINK, 11, 60, 32000,
		CM_LE_FRAME_DURATION_10_US, 80);
	sink.lifecycle_owned = 1;
	source = sink;
	source.direction = CM_LE_DIRECTION_SOURCE;
	source.generation = 12;
	source.lifecycle_owned = 0;
	assert(cm_leaudio_install_with_lifecycle(&state, &sink, dup(rx[0]),
		dup(lifecycle[0])) == 0);
	close(lifecycle[0]);
	lifecycle[0] = -1;
	assert(cm_leaudio_install_with_lifecycle(&state, &source, dup(tx[0]),
		-1) == 0);
	assert(cm_leaudio_begin(&state) == 0);
	assert(cm_leaudio_get_session_id(&state, &session) == 0);
	samples = cm_leaudio_frame_samples(&state);
	peer_encoder_memory = malloc(lc3_encoder_size(
		CM_LE_FRAME_DURATION_10_US, 32000));
	assert(peer_encoder_memory != NULL);
	peer_encoder = lc3_setup_encoder(CM_LE_FRAME_DURATION_10_US, 32000, 0,
		peer_encoder_memory);
	assert(peer_encoder != NULL);
	fill_sine(pcm, samples, 32000);

	/* A non-empty but unusable SDU is still PLC, not media progress. */
	assert(send(rx[1], encoded, 1, 0) == 1);
	assert(cm_leaudio_read_session(&state, &session, decoded,
		sizeof(decoded) / sizeof(decoded[0]), &concealed) == samples);
	assert(concealed);
	memset(&wait_fd, 0, sizeof(wait_fd));
	wait_fd.fd = lifecycle[1];
	wait_fd.events = POLLIN;
	assert(poll(&wait_fd, 1, 0) == 0);
	assert(cm_leaudio_get_io_stats(&state)->empty_sdu_guard_arms == 0);

	assert(lc3_encode(peer_encoder, LC3_PCM_FORMAT_S16, pcm, 1, 80,
		encoded) == 0);
	assert(send(rx[1], encoded, 80, 0) == 80);
	assert(cm_leaudio_read_session(&state, &session, decoded,
		sizeof(decoded) / sizeof(decoded[0]), &concealed) == samples);
	assert(recv(lifecycle[1], message, sizeof(message), 0) ==
		(ssize_t) sizeof(message));
	assert(memcmp(message, "GGLC", 4) == 0);
	assert(message[4] == CM_LE_LIFECYCLE_VERSION &&
		message[5] == CM_LE_LIFECYCLE_PROGRESS);
	assert(get_le64(message + 8) == session.bundle_id);
	assert(get_le64(message + 16) == session.sink_generation);
	assert(get_le64(message + 24) == session.source_generation);
	assert(cm_leaudio_get_io_stats(&state)->empty_sdu_guard_arms == 1);

	assert(send(rx[1], encoded, 0, 0) == 0);
	assert(cm_leaudio_read_session(&state, &session, decoded,
		sizeof(decoded) / sizeof(decoded[0]), &concealed) == samples);
	memset(&wait_fd, 0, sizeof(wait_fd));
	wait_fd.fd = lifecycle[1];
	wait_fd.events = POLLIN;
	assert(poll(&wait_fd, 1, 0) == 0);

	assert(cm_leaudio_write_session(&state, &session, pcm,
		(size_t) samples) == samples);
	assert(recv(tx[1], encoded, sizeof(encoded), 0) == 80);
	assert(recv(lifecycle[1], message, sizeof(message), 0) ==
		(ssize_t) sizeof(message));
	assert(get_le64(message + 8) == session.bundle_id);
	assert(cm_leaudio_get_io_stats(&state)->lifecycle_progress_successes == 2);
	cm_leaudio_end(&state);
	wait_fd.events = POLLIN | POLLHUP | POLLERR;
	assert(poll(&wait_fd, 1, 100) == 1);
	assert(wait_fd.revents & POLLHUP);
	puts("PASS: lifecycle progress comes only from real RX or successful TX");

	free(peer_encoder_memory);
	close(rx[0]);
	close(rx[1]);
	close(tx[0]);
	close(tx[1]);
	if (lifecycle[0] >= 0)
		close(lifecycle[0]);
	close(lifecycle[1]);
}

static void test_normal_end_is_queued_before_immediate_close(void)
{
	struct cm_leaudio state;
	struct cm_le_descriptor sink;
	struct cm_le_descriptor source;
	const struct cm_leaudio_io_stats *stats;
	uint8_t message[CM_LE_LIFECYCLE_MESSAGE_SIZE];
	struct pollfd wait_fd;
	int sink_media[2];
	int source_media[2];
	int lifecycle[2];

	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sink_media) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, source_media) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, lifecycle) == 0);
	cm_leaudio_init(&state);
	fill_descriptor(&sink, CM_LE_DIRECTION_SINK, 31, 160, 32000,
		CM_LE_FRAME_DURATION_10_US, 80);
	sink.lifecycle_owned = 1;
	source = sink;
	source.direction = CM_LE_DIRECTION_SOURCE;
	source.generation = 32;
	source.lifecycle_owned = 0;
	assert(cm_leaudio_install_with_lifecycle(&state, &sink,
		dup(sink_media[0]), dup(lifecycle[0])) == 0);
	close(lifecycle[0]);
	lifecycle[0] = -1;
	assert(cm_leaudio_install_with_lifecycle(&state, &source,
		dup(source_media[0]), -1) == 0);
	assert(cm_leaudio_begin(&state) == 0);
	assert(cm_leaudio_normal_end(&state) == 0);
	cm_leaudio_end(&state);

	memset(&wait_fd, 0, sizeof(wait_fd));
	wait_fd.fd = lifecycle[1];
	wait_fd.events = POLLIN | POLLHUP | POLLERR;
	assert(poll(&wait_fd, 1, 100) == 1);
	assert(wait_fd.revents & POLLIN);
	assert(wait_fd.revents & POLLHUP);
	assert(recv(lifecycle[1], message, sizeof(message), 0) ==
		(ssize_t) sizeof(message));
	assert(memcmp(message, "GGLC", 4) == 0);
	assert(message[4] == CM_LE_LIFECYCLE_VERSION);
	assert(message[5] == CM_LE_LIFECYCLE_NORMAL_END);
	assert(get_le64(message + 8) == 160);
	assert(get_le64(message + 16) == 31);
	assert(get_le64(message + 24) == 32);
	assert(recv(lifecycle[1], message, sizeof(message), 0) == 0);
	stats = cm_leaudio_get_io_stats(&state);
	assert(stats->lifecycle_normal_end_attempts == 1);
	assert(stats->lifecycle_normal_end_successes == 1);
	assert(stats->lifecycle_normal_end_errors == 0);
	puts("PASS: normal-end remains readable when Asterisk immediately closes");

	close(sink_media[0]);
	close(sink_media[1]);
	close(source_media[0]);
	close(source_media[1]);
	if (lifecycle[0] >= 0)
		close(lifecycle[0]);
	close(lifecycle[1]);
}

/* A real call sends before the remote fills its first SDU, so TX progress
 * always lands first.  The v18 silence window originally keyed off
 * media_progressed, which a successful send also sets, so on live calls it
 * never fired once: prehistory_silence stayed 0 while 56 empty SDUs went
 * through concealment.  The decoder's history is an RX property, so this
 * pins the window to RX only. */
static void test_tx_progress_does_not_end_the_silence_window(void)
{
	struct cm_leaudio state;
	struct cm_le_descriptor sink;
	struct cm_le_descriptor source;
	const struct cm_leaudio_io_stats *stats;
	uint8_t encoded[CM_LE_MAX_FRAME_OCTETS];
	int16_t pcm[320];
	int16_t decoded[320];
	int rx[2];
	int tx[2];
	int samples;
	int concealed;
	int sample;

	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, rx) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, tx) == 0);
	cm_leaudio_init(&state);
	fill_descriptor(&sink, CM_LE_DIRECTION_SINK, 1, 20, 32000,
		CM_LE_FRAME_DURATION_10_US, 80);
	source = sink;
	source.direction = CM_LE_DIRECTION_SOURCE;
	source.generation = 2;
	assert(cm_leaudio_install(&state, &sink, dup(rx[0])) == 0);
	assert(cm_leaudio_install(&state, &source, dup(tx[0])) == 0);
	assert(cm_leaudio_begin(&state) == 0);
	samples = cm_leaudio_frame_samples(&state);
	assert(samples == 320);

	/* TX progress first, exactly as a live call does. */
	fill_sine(pcm, samples, 32000);
	assert(cm_leaudio_write(&state, pcm, (size_t) samples) == samples);
	assert(recv(tx[1], encoded, sizeof(encoded), 0) == 80);

	concealed = 0;
	memset(decoded, 0x5a, sizeof(decoded));
	assert(send(rx[1], encoded, 0, 0) == 0);
	assert(cm_leaudio_read(&state, decoded,
		sizeof(decoded) / sizeof(decoded[0]), &concealed) == samples);
	assert(concealed);
	for (sample = 0; sample < samples; ++sample)
		assert(decoded[sample] == 0);
	stats = cm_leaudio_get_io_stats(&state);
	assert(stats != NULL);
	assert(stats->prehistory_silence_frames == 1);
	assert(stats->decode_plc_frames == 0);

	cm_leaudio_end(&state);
	close(rx[0]);
	close(rx[1]);
	close(tx[0]);
	close(tx[1]);
	puts("PASS: TX progress alone does not end the RX silence window");
}

static void test_terminal_poll_classification(void)
{
	assert(!cm_leaudio_poll_is_terminal(0));
	assert(!cm_leaudio_poll_is_terminal(POLLIN));
	assert(cm_leaudio_poll_is_terminal(POLLHUP));
	assert(cm_leaudio_poll_is_terminal(POLLERR));
	assert(cm_leaudio_poll_is_terminal(POLLNVAL));
	assert(cm_leaudio_poll_is_terminal(POLLIN | POLLERR));
	puts("PASS: POLLHUP, POLLERR, and POLLNVAL are terminal; POLLIN is not");
}

static void test_peer_close_is_not_an_empty_sdu(void)
{
	struct cm_leaudio state;
	struct cm_le_descriptor sink;
	struct cm_le_descriptor source;
	const struct cm_leaudio_io_stats *stats;
	int16_t decoded[320];
	int rx[2];
	int tx[2];
	int concealed = 0;

	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, rx) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, tx) == 0);
	cm_leaudio_init(&state);
	fill_descriptor(&sink, CM_LE_DIRECTION_SINK, 1, 30, 32000,
		CM_LE_FRAME_DURATION_10_US, 80);
	source = sink;
	source.direction = CM_LE_DIRECTION_SOURCE;
	source.generation = 2;
	assert(cm_leaudio_install(&state, &sink, dup(rx[0])) == 0);
	assert(cm_leaudio_install(&state, &source, dup(tx[0])) == 0);
	assert(cm_leaudio_begin(&state) == 0);
	close(rx[1]);
	assert(cm_leaudio_read(&state, decoded,
		sizeof(decoded) / sizeof(decoded[0]), &concealed) == -ECONNRESET);
	stats = cm_leaudio_get_io_stats(&state);
	assert(stats != NULL);
	assert(stats->poll_hups == 1);
	assert(stats->receive_errors == 1);
	assert(stats->empty_sdus == 0);
	assert(stats->decode_calls == 0);
	assert(stats->first_error_stage == CM_LE_IO_RX_POLL);
	assert(stats->first_error_result == -ECONNRESET);
	assert(cm_leaudio_tx_active(&state));
	puts("PASS: peer close is POLLHUP termination, not an empty SDU");
	cm_leaudio_stop_rx(&state);
	assert(cm_leaudio_tx_active(&state));
	cm_leaudio_end(&state);
	close(rx[0]);
	close(tx[0]);
	close(tx[1]);
}

static void test_pair_contract_and_staged_reset(void)
{
	struct cm_leaudio state;
	struct cm_le_descriptor sink;
	struct cm_le_descriptor source;
	int sink_media[2];
	int source_media[2];
	int passed_fd;

	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sink_media) == 0);
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, source_media) == 0);
	cm_leaudio_init(&state);
	fill_descriptor(&sink, CM_LE_DIRECTION_SINK, 1, 1, 32000,
		CM_LE_FRAME_DURATION_10_US, 80);
	fill_descriptor(&source, CM_LE_DIRECTION_SOURCE, 2, 1, 32000,
		CM_LE_FRAME_DURATION_10_US, 80);
	assert(cm_leaudio_install(&state, &sink, dup(sink_media[0])) == 0);
	source.cis = 3;
	passed_fd = dup(source_media[0]);
	assert(passed_fd >= 0);
	assert(cm_leaudio_install(&state, &source, passed_fd) == -EPROTO);
	assert(fcntl(passed_fd, F_GETFD) >= 0);
	close(passed_fd);
	assert(state.sink.fd == -1 && state.source.fd == -1);
	assert(!cm_leaudio_ready(&state));

	fill_descriptor(&source, CM_LE_DIRECTION_SOURCE, 3, 2, 32000,
		CM_LE_FRAME_DURATION_10_US, 80);
	assert(cm_leaudio_install(&state, &source, dup(source_media[0])) == 0);
	cm_leaudio_reset_staged(&state);
	assert(state.sink.fd == -1 && state.source.fd == -1);
	passed_fd = dup(source_media[0]);
	assert(passed_fd >= 0);
	assert(cm_leaudio_install(&state, &source, passed_fd) == -ESTALE);
	close(passed_fd);

	fill_descriptor(&sink, CM_LE_DIRECTION_SINK, 4, 3, 32000,
		CM_LE_FRAME_DURATION_10_US, 80);
	fill_descriptor(&source, CM_LE_DIRECTION_SOURCE, 5, 3, 32000,
		CM_LE_FRAME_DURATION_10_US, 80);
	assert(cm_leaudio_install(&state, &source, dup(source_media[0])) == 0);
	assert(cm_leaudio_install(&state, &sink, dup(sink_media[0])) == 0);
	assert(cm_leaudio_ready(&state));
	assert(cm_leaudio_begin(&state) == 0);
	passed_fd = cm_leaudio_read_fd(&state);
	assert(passed_fd >= 0);
	cm_leaudio_reset_staged(&state);
	assert(cm_leaudio_read_fd(&state) == passed_fd);
	cm_leaudio_end(&state);

	close(sink_media[0]);
	close(sink_media[1]);
	close(source_media[0]);
	close(source_media[1]);
}

static void test_bounded_descriptor_fuzz(void)
{
	uint8_t data[CM_LE_HANDOFF_HEADER_SIZE + CM_LE_MAX_TRANSPORT_PATH];
	struct cm_le_descriptor descriptor;
	uint32_t random_state = 0x6c633301U;
	unsigned iteration;

	for (iteration = 0; iteration < 100000; ++iteration) {
		size_t length;
		size_t index;

		random_state ^= random_state << 13;
		random_state ^= random_state >> 17;
		random_state ^= random_state << 5;
		length = random_state % (sizeof(data) + 1);
		for (index = 0; index < length; ++index) {
			random_state ^= random_state << 13;
			random_state ^= random_state >> 17;
			random_state ^= random_state << 5;
			data[index] = (uint8_t) random_state;
		}
		(void) cm_le_descriptor_decode(data, length, &descriptor);
	}
}

int main(void)
{
	test_descriptor_and_rights();
	test_codec_setting(16000, CM_LE_FRAME_DURATION_7P5_US);
	test_codec_setting(16000, CM_LE_FRAME_DURATION_10_US);
	test_codec_setting(24000, CM_LE_FRAME_DURATION_7P5_US);
	test_codec_setting(24000, CM_LE_FRAME_DURATION_10_US);
	test_codec_setting(32000, CM_LE_FRAME_DURATION_7P5_US);
	test_codec_setting(32000, CM_LE_FRAME_DURATION_10_US);
	test_empty_sdu_guard_requires_media_progress();
	test_tx_progress_does_not_end_the_silence_window();
	test_terminal_poll_classification();
	test_peer_close_is_not_an_empty_sdu();
	test_two_consecutive_calls_release_and_reacquire();
	test_stale_session_cannot_touch_reused_fd_numbers();
	test_lifecycle_progress_is_media_driven();
	test_normal_end_is_queued_before_immediate_close();
	test_pair_contract_and_staged_reset();
	test_bounded_descriptor_fuzz();
	puts("PASS: chan_mobile LE Audio helper");
	return 0;
}
