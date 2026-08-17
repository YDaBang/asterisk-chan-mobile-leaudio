/*
 * Minimal HFP mSBC framing helper for the chan_mobile candidate.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef CHAN_MOBILE_MSBC_H
#define CHAN_MOBILE_MSBC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sbc/sbc.h>

#define CM_MSBC_PCM_SAMPLES 120
#define CM_MSBC_PCM_BYTES (CM_MSBC_PCM_SAMPLES * sizeof(int16_t))
#define CM_MSBC_PAYLOAD_BYTES 57
#define CM_MSBC_FRAME_BYTES 60
#define CM_MSBC_MAX_SCO_MTU 360
#define CM_MSBC_TX_BUFFER_BYTES \
	(CM_MSBC_MAX_SCO_MTU + CM_MSBC_FRAME_BYTES - 1)
#define CM_MSBC_MAX_MISSING_FRAMES 3
#define CM_MSBC_MAX_DECODE_SAMPLES \
	(CM_MSBC_PCM_SAMPLES * (CM_MSBC_MAX_MISSING_FRAMES + 1))
#define CM_MSBC_RX_BUFFER_BYTES (CM_MSBC_FRAME_BYTES * 12)

/*
 * HFP Appendix C packet-loss concealment dimensions at 16 kHz.
 * The work area after the 375-sample history holds one replacement frame,
 * 36 decoder-reconvergence samples and a 16-sample overlap.
 */
#define CM_MSBC_PLC_TEMPLATE_SAMPLES 64
#define CM_MSBC_PLC_SEARCH_SAMPLES 256
#define CM_MSBC_PLC_HISTORY_SAMPLES \
	(CM_MSBC_PLC_SEARCH_SAMPLES + CM_MSBC_PCM_SAMPLES - 1)
#define CM_MSBC_PLC_RECONVERGENCE_SAMPLES 36
#define CM_MSBC_PLC_OVERLAP_SAMPLES 16
#define CM_MSBC_PLC_BUFFER_SAMPLES \
	(CM_MSBC_PLC_HISTORY_SAMPLES + CM_MSBC_PCM_SAMPLES + \
	CM_MSBC_PLC_RECONVERGENCE_SAMPLES + CM_MSBC_PLC_OVERLAP_SAMPLES)
#define CM_MSBC_PLC_LOSS_WINDOW 5

/*
 * Aggregate receive counters only.  They deliberately contain no audio,
 * phone number, Bluetooth address, call identifier or wall-clock timestamp.
 * Updates saturate at UINT64_MAX.
 */
struct cm_msbc_rx_stats {
	uint64_t feed_bytes;
	uint64_t good_frames;
	uint64_t seq_gap_events;
	uint64_t seq_gap_frames;
	uint64_t decode_failures;
	uint64_t h2_resync_events;
	uint64_t h2_discarded_bytes;
	uint64_t concealed_frames;
	uint64_t plc_substitution_frames;
	uint64_t plc_zir_frames;
	uint64_t plc_decoder_reinits;
	uint64_t plc_zero_decode_failures;
	uint64_t enobufs;
	uint64_t enospc;
};

struct cm_msbc {
	sbc_t encoder;
	sbc_t decoder;
	bool initialized;
	bool rx_sequence_initialized;
	bool have_last_pcm;
	bool rx_resync_active;
	bool plc_substitution_active;
	uint8_t rx_sequence;
	uint8_t tx_sequence;
	uint8_t plc_loss_index;
	uint8_t plc_loss_count;
	unsigned int concealed_frames;
	unsigned int plc_best_lag;
	unsigned int plc_bad_frames;
	uint8_t rx_buffer[CM_MSBC_RX_BUFFER_BYTES];
	uint8_t zero_payload[CM_MSBC_PAYLOAD_BYTES];
	uint8_t plc_loss_history[CM_MSBC_PLC_LOSS_WINDOW];
	size_t rx_length;
	struct cm_msbc_rx_stats *rx_stats;
	int16_t last_pcm[CM_MSBC_PCM_SAMPLES];
	int16_t last_concealed_pcm[CM_MSBC_PCM_SAMPLES];
	int16_t plc_history[CM_MSBC_PLC_BUFFER_SAMPLES];
};

/*
 * Transparent SCO is a byte stream carried in fixed-size socket writes.  A
 * newly encoded 60-byte frame can arrive while as many as mtu - 1 bytes are
 * still buffered, so the buffer must hold max_mtu + frame_bytes - 1 bytes.
 */
struct cm_msbc_tx_packetizer {
	uint8_t buffer[CM_MSBC_TX_BUFFER_BYTES];
	size_t length;
};

int cm_msbc_init(struct cm_msbc *state);
int cm_msbc_reset(struct cm_msbc *state);
void cm_msbc_finish(struct cm_msbc *state);

void cm_msbc_set_rx_stats(struct cm_msbc *state,
	struct cm_msbc_rx_stats *stats);

int cm_msbc_encode(struct cm_msbc *state,
	const int16_t pcm[CM_MSBC_PCM_SAMPLES],
	uint8_t frame[CM_MSBC_FRAME_BYTES]);

void cm_msbc_tx_packetizer_reset(struct cm_msbc_tx_packetizer *state);

/*
 * Append one complete H2/mSBC frame and return every complete mtu-sized SCO
 * packet in output.  The returned byte count is a multiple of mtu and never
 * exceeds CM_MSBC_MAX_SCO_MTU; a final partial packet remains in state.
 */
int cm_msbc_tx_packetize(struct cm_msbc_tx_packetizer *state,
	const uint8_t frame[CM_MSBC_FRAME_BYTES], size_t mtu,
	uint8_t output[CM_MSBC_MAX_SCO_MTU], size_t *output_length);

/*
 * Append an arbitrary fragment of the transparent eSCO byte stream and
 * decode every complete H2/mSBC frame. The return value is a sample count.
 * Missing sequence numbers are concealed, up to three frames per gap.
 */
int cm_msbc_decode_feed(struct cm_msbc *state,
	const uint8_t *data, size_t data_length,
	int16_t *pcm, size_t pcm_capacity_samples);

#endif
