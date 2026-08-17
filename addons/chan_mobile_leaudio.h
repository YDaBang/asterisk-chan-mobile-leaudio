/*
 * LC3 media helper for the chan_mobile LE Audio canary.
 *
 * This helper owns only media descriptors and codec state.  It deliberately
 * has no access to HFP call control, Asterisk channel disposition, or SMS.
 */

#ifndef CHAN_MOBILE_LEAUDIO_H
#define CHAN_MOBILE_LEAUDIO_H

#include <stddef.h>
#include <stdint.h>

#define CM_LE_HANDOFF_VERSION 3
#define CM_LE_HANDOFF_HEADER_SIZE 72
#define CM_LE_MAX_TRANSPORT_PATH 512
#define CM_LE_FRAME_DURATION_7P5_US 7500
#define CM_LE_FRAME_DURATION_10_US 10000
#define CM_LE_MAX_FRAME_OCTETS 80
#define CM_LE_EMPTY_SDU_GUARD_DISABLED 0U

#define CM_LE_HANDOFF_FLAG_OWNERSHIP (1U << 0)
#define CM_LE_HANDOFF_FLAG_LINKED (1U << 1)
#define CM_LE_HANDOFF_FLAG_CALL_TOKEN (1U << 2)
#define CM_LE_HANDOFF_FLAG_LIFECYCLE (1U << 3)

#define CM_LE_LIFECYCLE_VERSION 1
#define CM_LE_LIFECYCLE_PROGRESS 1
#define CM_LE_LIFECYCLE_NORMAL_END 2
#define CM_LE_LIFECYCLE_MESSAGE_SIZE 32

enum cm_le_direction {
	CM_LE_DIRECTION_SINK = 1,
	CM_LE_DIRECTION_SOURCE = 2,
};

enum cm_leaudio_io_stage {
	CM_LE_IO_NONE = 0,
	CM_LE_IO_RX_STATE,
	CM_LE_IO_RX_POLL,
	CM_LE_IO_RX_RECV,
	CM_LE_IO_RX_EMPTY_GUARD,
	CM_LE_IO_RX_DECODE,
	CM_LE_IO_TX_STATE,
	CM_LE_IO_TX_ENCODE,
	CM_LE_IO_TX_SEND,
};

struct cm_leaudio_io_stats {
	uint64_t read_calls;
	uint64_t read_retries;
	uint64_t poll_hups;
	uint64_t poll_errors;
	uint64_t empty_sdus;
	uint64_t consecutive_empty_sdus;
	uint64_t max_consecutive_empty_sdus;
	uint64_t guarded_consecutive_empty_sdus;
	uint64_t empty_sdu_guard_arms;
	uint64_t empty_sdu_guard_progress_resets;
	uint64_t empty_sdu_guard_trips;
	uint64_t stale_rx_session_rejections;
	uint64_t stale_tx_session_rejections;
	uint64_t lifecycle_progress_attempts;
	uint64_t lifecycle_progress_successes;
	uint64_t lifecycle_progress_errors;
	uint64_t lifecycle_normal_end_attempts;
	uint64_t lifecycle_normal_end_successes;
	uint64_t lifecycle_normal_end_errors;
	uint64_t short_sdus;
	uint64_t full_sdus;
	uint64_t truncated_sdus;
	uint64_t packet_status_errors;
	uint64_t receive_errors;
	uint64_t decode_calls;
	uint64_t decode_successes;
	uint64_t decode_plc_frames;
	uint64_t prehistory_silence_frames;
	uint64_t decode_errors;
	uint64_t write_calls;
	uint64_t encode_calls;
	uint64_t encode_successes;
	uint64_t encode_errors;
	uint64_t send_calls;
	uint64_t send_successes;
	uint64_t send_retries;
	uint64_t send_errors;
	enum cm_leaudio_io_stage first_error_stage;
	int first_error_result;
	int first_error_detail;
	int64_t first_error_sdu_length;
	enum cm_leaudio_io_stage last_rx_error_stage;
	int last_rx_error_result;
	int last_rx_error_detail;
	int64_t last_rx_sdu_length;
	enum cm_leaudio_io_stage last_tx_error_stage;
	int last_tx_error_result;
	int last_tx_error_detail;
};

struct cm_le_descriptor {
	enum cm_le_direction direction;
	uint64_t generation;
	uint64_t bundle_id;
	uint64_t call_control_token;
	uint32_t sample_rate;
	uint32_t frame_duration_us;
	uint32_t channel_allocation;
	uint32_t interval_us;
	uint32_t presentation_delay_us;
	uint16_t octets_per_frame;
	uint16_t read_mtu;
	uint16_t write_mtu;
	uint16_t latency_ms;
	uint8_t framing;
	uint8_t phy;
	uint8_t retransmissions;
	uint8_t target_latency;
	uint8_t cig;
	uint8_t cis;
	unsigned int call_control_correlated : 1;
	unsigned int lifecycle_owned : 1;
	char transport[CM_LE_MAX_TRANSPORT_PATH + 1];
};

struct cm_leaudio_session_id {
	uint64_t bundle_id;
	uint64_t sink_generation;
	uint64_t source_generation;
};

struct cm_le_stream {
	int fd;
	uint64_t generation;
	uint64_t bundle_id;
	uint64_t call_control_token;
	uint32_t sample_rate;
	uint32_t frame_duration_us;
	uint32_t channel_allocation;
	uint32_t interval_us;
	uint32_t presentation_delay_us;
	uint16_t octets_per_frame;
	uint16_t read_mtu;
	uint16_t write_mtu;
	uint16_t latency_ms;
	uint8_t framing;
	uint8_t phy;
	uint8_t retransmissions;
	uint8_t target_latency;
	uint8_t cig;
	uint8_t cis;
	unsigned int call_control_correlated : 1;
};

struct cm_leaudio {
	struct cm_le_stream sink;
	struct cm_le_stream source;
	uint64_t highest_sink_generation;
	uint64_t highest_source_generation;
	uint64_t highest_bundle_id;
	uint32_t active_rate;
	uint32_t active_frame_duration_us;
	uint32_t empty_sdu_guard_us;
	uint16_t active_octets;
	uint64_t empty_guard_tx_baseline;
	uint64_t guarded_empty_sdus;
	int lifecycle_fd;
	unsigned int active : 1;
	unsigned int rx_active : 1;
	unsigned int tx_active : 1;
	unsigned int media_progressed : 1;
	/* media_progressed is also set by a successful send, so it cannot say
	 * whether the decoder has any history.  This one is RX only. */
	unsigned int rx_decoded : 1;
	void *encoder_memory;
	void *decoder_memory;
	void *encoder;
	void *decoder;
	struct cm_leaudio_io_stats io_stats;
};

void cm_leaudio_init(struct cm_leaudio *state);
void cm_leaudio_end(struct cm_leaudio *state);
void cm_leaudio_reset_staged(struct cm_leaudio *state);
int cm_leaudio_normal_end(struct cm_leaudio *state);

int cm_le_descriptor_decode(const uint8_t *packet, size_t packet_length,
	struct cm_le_descriptor *descriptor);
int cm_le_handoff_connect(const char *path);
int cm_le_handoff_receive(int socket_fd, struct cm_le_descriptor *descriptor,
	int *media_fd, int *lifecycle_fd);
int cm_le_descriptor_matches_address(const struct cm_le_descriptor *descriptor,
	const char *address);

int cm_leaudio_install(struct cm_leaudio *state,
	const struct cm_le_descriptor *descriptor, int media_fd);
int cm_leaudio_install_with_lifecycle(struct cm_leaudio *state,
	const struct cm_le_descriptor *descriptor, int media_fd,
	int lifecycle_fd);
int cm_leaudio_ready(const struct cm_leaudio *state);
int cm_leaudio_begin(struct cm_leaudio *state);
int cm_leaudio_read_fd(const struct cm_leaudio *state);
int cm_leaudio_rx_active(const struct cm_leaudio *state);
int cm_leaudio_tx_active(const struct cm_leaudio *state);
void cm_leaudio_stop_rx(struct cm_leaudio *state);
void cm_leaudio_stop_tx(struct cm_leaudio *state);
int cm_leaudio_set_empty_sdu_guard_us(struct cm_leaudio *state,
	uint32_t guard_us);
int cm_leaudio_sample_rate(const struct cm_leaudio *state);
int cm_leaudio_frame_duration_us(const struct cm_leaudio *state);
int cm_leaudio_frame_samples(const struct cm_leaudio *state);
int cm_leaudio_poll_is_terminal(short revents);
int cm_leaudio_get_session_id(const struct cm_leaudio *state,
	struct cm_leaudio_session_id *session);
int cm_leaudio_session_matches(const struct cm_leaudio *state,
	const struct cm_leaudio_session_id *session);
const struct cm_leaudio_io_stats *cm_leaudio_get_io_stats(
	const struct cm_leaudio *state);
const char *cm_leaudio_io_stage_name(enum cm_leaudio_io_stage stage);

/*
 * Returns one decoded/accepted PCM frame, zero for a nonblocking retry, or a
 * negative errno-style value.  A bad ISO SDU is decoded as LC3 PLC and is not
 * promoted to a call-control failure.
 */
int cm_leaudio_read(struct cm_leaudio *state, int16_t *pcm,
	size_t pcm_capacity, int *concealed);
int cm_leaudio_write(struct cm_leaudio *state, const int16_t *pcm,
	size_t pcm_samples);
int cm_leaudio_read_session(struct cm_leaudio *state,
	const struct cm_leaudio_session_id *session, int16_t *pcm,
	size_t pcm_capacity, int *concealed);
int cm_leaudio_write_session(struct cm_leaudio *state,
	const struct cm_leaudio_session_id *session, const int16_t *pcm,
	size_t pcm_samples);

#endif
