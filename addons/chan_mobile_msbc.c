/*
 * Minimal HFP mSBC framing helper for the chan_mobile candidate.
 *
 * H2 framing and sequence handling follow the interoperable implementation
 * in BlueALSA (MIT), specifically shared/h2.h and codec-msbc.c. This file is
 * an independent, small adaptation for Asterisk's synchronous channel API.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "chan_mobile_msbc.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#if defined(__linux__)
#include <endian.h>
#else
#error "The chan_mobile mSBC candidate currently requires Linux endian helpers"
#endif

#define CM_H2_SYNCWORD 0x0801U
#define CM_H2_SYNCWORD_MASK 0x0fffU
#define CM_MSBC_SYNCWORD 0xadU
#define CM_MSBC_PLC_SCALE_ONE 32768U
#define CM_MSBC_PLC_SCALE_MIN 24576U
#define CM_MSBC_PLC_SCALE_MAX 39322U
#define CM_MSBC_PLC_PAUSE_THRESHOLD 2
#define CM_MSBC_PLC_MIN_TEMPLATE_RMS 16U
#define CM_MSBC_PLC_MIN_CORRELATION_SQUARED_DENOMINATOR 4U

_Static_assert(CM_MSBC_TX_BUFFER_BYTES >=
	CM_MSBC_MAX_SCO_MTU + CM_MSBC_FRAME_BYTES - 1,
	"mSBC TX buffer must hold one frame behind the largest accepted MTU");
_Static_assert(CM_MSBC_PLC_BUFFER_SAMPLES ==
	CM_MSBC_PLC_HISTORY_SAMPLES + CM_MSBC_PCM_SAMPLES +
	CM_MSBC_PLC_RECONVERGENCE_SAMPLES + CM_MSBC_PLC_OVERLAP_SAMPLES,
	"mSBC PLC buffer dimensions are inconsistent");

/*
 * Q15 values of 0.5 * (1 + cos(pi * (i + 1) / 17)), generated from the
 * raised-cosine definition rather than copied from the HFP sample code.
 * Complementary entries sum to exactly 32768.
 */
static const uint16_t cm_plc_raised_cosine[CM_MSBC_PLC_OVERLAP_SAMPLES] = {
	32489, 31662, 30314, 28492, 26258, 23687, 20868, 17896,
	14872, 11900, 9081, 6510, 4276, 2454, 1106, 279,
};

static uint16_t cm_h2_pack(uint8_t sequence)
{
	static const uint8_t sequence_bits[4][2] = {
		{ 0, 0 }, { 3, 0 }, { 0, 3 }, { 3, 3 },
	};
	uint16_t value;

	sequence &= 3;
	value = CM_H2_SYNCWORD |
		((uint16_t) sequence_bits[sequence][0] << 12) |
		((uint16_t) sequence_bits[sequence][1] << 14);
	return htole16(value);
}

static bool cm_h2_unpack(const uint8_t *data, uint8_t *sequence)
{
	uint16_t value;
	uint8_t sn0;
	uint8_t sn1;

	memcpy(&value, data, sizeof(value));
	value = le16toh(value);
	if ((value & CM_H2_SYNCWORD_MASK) != CM_H2_SYNCWORD)
		return false;

	sn0 = (value >> 12) & 3;
	sn1 = (value >> 14) & 3;
	if ((sn0 >> 1) != (sn0 & 1) || (sn1 >> 1) != (sn1 & 1))
		return false;

	*sequence = (sn1 & 2) | (sn0 & 1);
	return true;
}

static size_t cm_h2_find(const uint8_t *data, size_t length)
{
	size_t offset;
	uint8_t sequence;

	for (offset = 0; offset + 2 <= length; ++offset) {
		if (cm_h2_unpack(data + offset, &sequence))
			return offset;
	}
	/* Preserve a trailing byte that may be the first half of a split H2. */
	return length == 0 ? 0 : length - 1;
}

static void cm_sat_add(uint64_t *value, uint64_t add)
{
	if (UINT64_MAX - *value < add)
		*value = UINT64_MAX;
	else
		*value += add;
}

#define CM_STAT_ADD(state, member, amount) \
	do { \
		if ((state)->rx_stats != NULL) \
			cm_sat_add(&(state)->rx_stats->member, (uint64_t) (amount)); \
	} while (0)

static void cm_record_discard(struct cm_msbc *state, size_t bytes)
{
	if (bytes == 0)
		return;
	if (!state->rx_resync_active) {
		state->rx_resync_active = true;
		CM_STAT_ADD(state, h2_resync_events, 1);
	}
	CM_STAT_ADD(state, h2_discarded_bytes, bytes);
}

static int16_t cm_clip_sample(int64_t value)
{
	if (value > INT16_MAX)
		return INT16_MAX;
	if (value < INT16_MIN)
		return INT16_MIN;
	return (int16_t) value;
}

static int16_t cm_scale_q15(int16_t sample, uint32_t scale)
{
	int64_t value = (int64_t) sample * scale;

	value += value >= 0 ? CM_MSBC_PLC_SCALE_ONE / 2 :
		-(int64_t) CM_MSBC_PLC_SCALE_ONE / 2;
	return cm_clip_sample(value / CM_MSBC_PLC_SCALE_ONE);
}

static int16_t cm_mix_q15(int16_t descending, uint32_t descending_scale,
	int16_t ascending, uint32_t ascending_scale, uint16_t descending_weight)
{
	uint32_t ascending_weight = CM_MSBC_PLC_SCALE_ONE - descending_weight;
	int64_t value =
		(int64_t) descending * descending_scale * descending_weight +
		(int64_t) ascending * ascending_scale * ascending_weight;
	const int64_t denominator =
		(int64_t) CM_MSBC_PLC_SCALE_ONE * CM_MSBC_PLC_SCALE_ONE;

	value += value >= 0 ? denominator / 2 : -denominator / 2;
	return cm_clip_sample(value / denominator);
}

static uint32_t cm_abs16(int16_t sample)
{
	return sample < 0 ? (uint32_t) -(int32_t) sample : (uint32_t) sample;
}

static uint32_t cm_plc_amplitude_scale(const int16_t *reference,
	const int16_t *replacement)
{
	uint32_t reference_sum = 0;
	uint32_t replacement_sum = 0;
	uint64_t scale;
	size_t i;

	for (i = 0; i < CM_MSBC_PCM_SAMPLES; ++i) {
		reference_sum += cm_abs16(reference[i]);
		replacement_sum += cm_abs16(replacement[i]);
	}
	if (replacement_sum == 0)
		return CM_MSBC_PLC_SCALE_MAX;

	scale = ((uint64_t) reference_sum * CM_MSBC_PLC_SCALE_ONE +
		replacement_sum / 2) / replacement_sum;
	if (scale < CM_MSBC_PLC_SCALE_MIN)
		return CM_MSBC_PLC_SCALE_MIN;
	if (scale > CM_MSBC_PLC_SCALE_MAX)
		return CM_MSBC_PLC_SCALE_MAX;
	return (uint32_t) scale;
}

static bool cm_plc_pattern_match(const int16_t *history,
	unsigned int *best_match)
{
	const int16_t *reference =
		history + CM_MSBC_PLC_HISTORY_SAMPLES -
		CM_MSBC_PLC_TEMPLATE_SAMPLES;
	uint64_t reference_energy = 0;
	uint64_t best_energy = 1;
	int64_t best_correlation = 0;
	bool found = false;
	size_t candidate;
	size_t i;

	for (i = 0; i < CM_MSBC_PLC_TEMPLATE_SAMPLES; ++i)
		reference_energy +=
			(uint64_t) (int64_t) reference[i] * reference[i];
	if (reference_energy <
		(uint64_t) CM_MSBC_PLC_TEMPLATE_SAMPLES *
			CM_MSBC_PLC_MIN_TEMPLATE_RMS *
			CM_MSBC_PLC_MIN_TEMPLATE_RMS)
		return false;

	for (candidate = 0; candidate < CM_MSBC_PLC_SEARCH_SAMPLES;
		++candidate) {
		int64_t correlation = 0;
		uint64_t energy = 0;

		for (i = 0; i < CM_MSBC_PLC_TEMPLATE_SAMPLES; ++i) {
			correlation += (int64_t) reference[i] *
				history[candidate + i];
			energy += (uint64_t) (int64_t) history[candidate + i] *
				history[candidate + i];
		}
		/*
		 * The template energy is common to every candidate.  Compare
		 * correlation^2 / candidate_energy exactly, without sqrt() or a
		 * new libm dependency.  Like the HFP reference, only positive
		 * correlations are useful.
		 */
		if (correlation <= 0 || energy == 0)
			continue;
		if (!found ||
			(unsigned __int128) correlation * correlation * best_energy >
			(unsigned __int128) best_correlation * best_correlation *
				energy) {
			found = true;
			best_correlation = correlation;
			best_energy = energy;
			*best_match = (unsigned int) candidate;
		}
	}
	if (!found)
		return false;

	/*
	 * Unvoiced noise can have a weak accidental positive match. Repeating
	 * that match sounds more mechanical than a bounded ZIR fallback. Require
	 * a normalized correlation of at least 0.5, compared without sqrt().
	 */
	return (unsigned __int128) best_correlation * best_correlation *
			CM_MSBC_PLC_MIN_CORRELATION_SQUARED_DENOMINATOR >=
		(unsigned __int128) reference_energy * best_energy;
}

static void cm_plc_update_loss_window(struct cm_msbc *state, uint8_t lost)
{
	uint8_t previous = state->plc_loss_history[state->plc_loss_index];

	state->plc_loss_count -= previous;
	state->plc_loss_history[state->plc_loss_index] = lost;
	state->plc_loss_count += lost;
	state->plc_loss_index =
		(state->plc_loss_index + 1) % CM_MSBC_PLC_LOSS_WINDOW;
}

static bool cm_plc_decode_zero(struct cm_msbc *state,
	int16_t zir[CM_MSBC_PCM_SAMPLES])
{
	size_t decoded = 0;
	ssize_t consumed;

	consumed = sbc_decode(&state->decoder, state->zero_payload,
		CM_MSBC_PAYLOAD_BYTES, zir, CM_MSBC_PCM_BYTES, &decoded);
	if (consumed == CM_MSBC_PAYLOAD_BYTES && decoded == CM_MSBC_PCM_BYTES)
		return true;

	/*
	 * A generated, validated zero payload should always decode.  If the
	 * decoder was nevertheless left unusable by damaged input, restore only
	 * decoder state and retry.  This exceptional path intentionally gives up
	 * the old filter response instead of disturbing the independent encoder.
	 */
	if (sbc_reinit_msbc(&state->decoder, 0) >= 0) {
		CM_STAT_ADD(state, plc_decoder_reinits, 1);
		state->decoder.endian = SBC_LE;
		decoded = 0;
		consumed = sbc_decode(&state->decoder, state->zero_payload,
			CM_MSBC_PAYLOAD_BYTES, zir, CM_MSBC_PCM_BYTES, &decoded);
		if (consumed == CM_MSBC_PAYLOAD_BYTES &&
			decoded == CM_MSBC_PCM_BYTES)
			return true;
	}
	CM_STAT_ADD(state, plc_zero_decode_failures, 1);
	memset(zir, 0, CM_MSBC_PCM_BYTES);
	return false;
}

static void cm_conceal_frame(struct cm_msbc *state, int16_t *pcm)
{
	int16_t zir[CM_MSBC_PCM_SAMPLES];
	int16_t *frame_head =
		state->plc_history + CM_MSBC_PLC_HISTORY_SAMPLES;
	const size_t continuation_samples =
		CM_MSBC_PLC_RECONVERGENCE_SAMPLES +
		CM_MSBC_PLC_OVERLAP_SAMPLES;
	bool use_substitution =
		state->plc_loss_count < CM_MSBC_PLC_PAUSE_THRESHOLD;
	unsigned int best_match = 0;
	uint32_t scale = CM_MSBC_PLC_SCALE_ONE;
	size_t i;

	(void) cm_plc_decode_zero(state, zir);

	if (state->plc_bad_frames != 0)
		use_substitution =
			use_substitution && state->plc_substitution_active;
	if (use_substitution && state->plc_bad_frames == 0) {
		use_substitution =
			cm_plc_pattern_match(state->plc_history, &best_match);
		state->plc_substitution_active = use_substitution;
		if (use_substitution) {
			state->plc_best_lag =
				best_match + CM_MSBC_PLC_TEMPLATE_SAMPLES;
			/*
			 * Extend the matched waveform through the whole replacement
			 * and recovery workspace before amplitude matching.  A late
			 * best_lag can cross the end of received history; sequential
			 * copying then repeats already generated samples cyclically
			 * instead of measuring stale or zero work-area data.
			 */
			for (i = 0;
				i < CM_MSBC_PCM_SAMPLES + continuation_samples; ++i) {
				frame_head[i] = state->plc_history[
					state->plc_best_lag + i];
			}
			scale = cm_plc_amplitude_scale(
				state->plc_history + CM_MSBC_PLC_HISTORY_SAMPLES -
					CM_MSBC_PCM_SAMPLES,
				frame_head);

			for (i = 0; i < CM_MSBC_PLC_OVERLAP_SAMPLES; ++i) {
				frame_head[i] = cm_mix_q15(zir[i],
					CM_MSBC_PLC_SCALE_ONE,
					frame_head[i], scale,
					cm_plc_raised_cosine[i]);
			}
			for (; i < CM_MSBC_PCM_SAMPLES; ++i) {
				frame_head[i] = cm_scale_q15(frame_head[i], scale);
			}
			for (i = 0; i < CM_MSBC_PLC_OVERLAP_SAMPLES; ++i) {
				int16_t sample =
					frame_head[CM_MSBC_PCM_SAMPLES + i];
				frame_head[CM_MSBC_PCM_SAMPLES + i] =
					cm_mix_q15(sample, scale, sample,
						CM_MSBC_PLC_SCALE_ONE,
						cm_plc_raised_cosine[i]);
			}
		}
	} else if (use_substitution) {
		for (i = 0; i < CM_MSBC_PCM_SAMPLES + continuation_samples; ++i)
			frame_head[i] =
				state->plc_history[state->plc_best_lag + i];
	}

	if (!use_substitution) {
		int16_t old_side[CM_MSBC_PLC_OVERLAP_SAMPLES];

		if (state->plc_bad_frames != 0 &&
			state->plc_substitution_active) {
			/*
			 * A prior substitution already prepared the next 52 samples
			 * in this work area. Preserve its first millisecond before
			 * replacing the area with ZIR.
			 */
			memcpy(old_side, frame_head, sizeof(old_side));
		} else {
			/* No future waveform exists yet; hold the actual boundary. */
			for (i = 0; i < CM_MSBC_PLC_OVERLAP_SAMPLES; ++i)
				old_side[i] = state->last_pcm[
					CM_MSBC_PCM_SAMPLES - 1];
		}
		memcpy(frame_head, zir, CM_MSBC_PCM_BYTES);
		/*
		 * The high-loss gate intentionally stops repeating synthetic
		 * history, but a direct jump from that history to ZIR can click.
		 * Crossfade one millisecond from the preceding output tail.
		 */
		if (state->have_last_pcm) {
			for (i = 0; i < CM_MSBC_PLC_OVERLAP_SAMPLES; ++i) {
				frame_head[i] = cm_mix_q15(old_side[i],
					CM_MSBC_PLC_SCALE_ONE, zir[i],
					CM_MSBC_PLC_SCALE_ONE,
					cm_plc_raised_cosine[i]);
			}
		}
		memset(frame_head + CM_MSBC_PCM_SAMPLES, 0,
			continuation_samples * sizeof(int16_t));
		state->plc_substitution_active = false;
	}
	CM_STAT_ADD(state, plc_substitution_frames, use_substitution ? 1 : 0);
	CM_STAT_ADD(state, plc_zir_frames, use_substitution ? 0 : 1);
	/*
	 * Even a zero-input fallback advances the decoder through an erasure.
	 * Preserve the 36+16 recovery mask so the next real decoded frame does
	 * not expose the mSBC synthesis-filter reconvergence transient.
	 */
	if (state->plc_bad_frames != UINT_MAX)
		state->plc_bad_frames++;

	memcpy(pcm, frame_head, CM_MSBC_PCM_BYTES);
	memcpy(state->last_concealed_pcm, pcm, CM_MSBC_PCM_BYTES);
	memcpy(state->last_pcm, pcm, CM_MSBC_PCM_BYTES);
	state->have_last_pcm = true;
	if (state->concealed_frames != UINT_MAX)
		state->concealed_frames++;
	CM_STAT_ADD(state, concealed_frames, 1);

	memmove(state->plc_history,
		state->plc_history + CM_MSBC_PCM_SAMPLES,
		(CM_MSBC_PLC_HISTORY_SAMPLES + continuation_samples) *
			sizeof(int16_t));
	cm_plc_update_loss_window(state, 1);
}

static void cm_accept_good_frame(struct cm_msbc *state, int16_t *pcm)
{
	int16_t *continuation =
		state->plc_history + CM_MSBC_PLC_HISTORY_SAMPLES;
	size_t i;

	if (state->plc_bad_frames != 0) {
		memcpy(pcm, continuation,
			CM_MSBC_PLC_RECONVERGENCE_SAMPLES * sizeof(int16_t));
		for (i = 0; i < CM_MSBC_PLC_OVERLAP_SAMPLES; ++i) {
			size_t offset = CM_MSBC_PLC_RECONVERGENCE_SAMPLES + i;
			pcm[offset] = cm_mix_q15(continuation[offset],
				CM_MSBC_PLC_SCALE_ONE, pcm[offset],
				CM_MSBC_PLC_SCALE_ONE,
				cm_plc_raised_cosine[i]);
		}
		state->plc_bad_frames = 0;
		state->plc_substitution_active = false;
	}

	memmove(state->plc_history,
		state->plc_history + CM_MSBC_PCM_SAMPLES,
		(CM_MSBC_PLC_HISTORY_SAMPLES - CM_MSBC_PCM_SAMPLES) *
			sizeof(int16_t));
	memcpy(state->plc_history + CM_MSBC_PLC_HISTORY_SAMPLES -
			CM_MSBC_PCM_SAMPLES,
		pcm, CM_MSBC_PCM_BYTES);
	cm_plc_update_loss_window(state, 0);

	memcpy(state->last_pcm, pcm, CM_MSBC_PCM_BYTES);
	state->have_last_pcm = true;
	state->concealed_frames = 0;
}

static int cm_make_zero_payload(uint8_t payload[CM_MSBC_PAYLOAD_BYTES])
{
	sbc_t encoder;
	int16_t zero_pcm[CM_MSBC_PCM_SAMPLES] = { 0 };
	ssize_t encoded = 0;
	ssize_t consumed;
	int result;

	memset(&encoder, 0, sizeof(encoder));
	result = sbc_init_msbc(&encoder, 0);
	if (result < 0)
		return result;
	encoder.endian = SBC_LE;
	consumed = sbc_encode(&encoder, zero_pcm, sizeof(zero_pcm), payload,
		CM_MSBC_PAYLOAD_BYTES, &encoded);
	sbc_finish(&encoder);

	if (consumed < 0)
		return (int) consumed;
	if (consumed != CM_MSBC_PCM_BYTES ||
		encoded != CM_MSBC_PAYLOAD_BYTES ||
		payload[0] != CM_MSBC_SYNCWORD)
		return -EIO;
	return 0;
}

int cm_msbc_init(struct cm_msbc *state)
{
	int result;

	if (state == NULL)
		return -EINVAL;
	memset(state, 0, sizeof(*state));

	result = sbc_init_msbc(&state->encoder, 0);
	if (result < 0)
		return result;
	result = sbc_init_msbc(&state->decoder, 0);
	if (result < 0) {
		sbc_finish(&state->encoder);
		return result;
	}

	state->encoder.endian = SBC_LE;
	state->decoder.endian = SBC_LE;
	result = cm_make_zero_payload(state->zero_payload);
	if (result < 0) {
		sbc_finish(&state->decoder);
		sbc_finish(&state->encoder);
		memset(state, 0, sizeof(*state));
		return result;
	}
	state->initialized = true;
	return 0;
}

int cm_msbc_reset(struct cm_msbc *state)
{
	struct cm_msbc_rx_stats *stats;
	int result;

	if (state == NULL)
		return -EINVAL;
	stats = state->rx_stats;
	if (state->initialized) {
		sbc_finish(&state->encoder);
		sbc_finish(&state->decoder);
	}
	result = cm_msbc_init(state);
	state->rx_stats = stats;
	return result;
}

void cm_msbc_finish(struct cm_msbc *state)
{
	if (state == NULL)
		return;
	if (state->initialized) {
		sbc_finish(&state->encoder);
		sbc_finish(&state->decoder);
	}
	memset(state, 0, sizeof(*state));
}

void cm_msbc_set_rx_stats(struct cm_msbc *state,
	struct cm_msbc_rx_stats *stats)
{
	if (state != NULL)
		state->rx_stats = stats;
}

int cm_msbc_encode(struct cm_msbc *state,
	const int16_t pcm[CM_MSBC_PCM_SAMPLES],
	uint8_t frame[CM_MSBC_FRAME_BYTES])
{
	ssize_t written;
	ssize_t encoded = 0;

	if (state == NULL || !state->initialized || pcm == NULL || frame == NULL)
		return -EINVAL;

	written = sbc_encode(&state->encoder, pcm, CM_MSBC_PCM_BYTES,
		frame + 2, CM_MSBC_PAYLOAD_BYTES, &encoded);
	if (written < 0)
		return (int) written;
	if (written != CM_MSBC_PCM_BYTES || encoded != CM_MSBC_PAYLOAD_BYTES)
		return -EIO;

	state->tx_sequence = (state->tx_sequence + 1) & 3;
	{
		uint16_t header = cm_h2_pack(state->tx_sequence);
		memcpy(frame, &header, sizeof(header));
	}
	frame[CM_MSBC_FRAME_BYTES - 1] = 0;
	return CM_MSBC_FRAME_BYTES;
}

void cm_msbc_tx_packetizer_reset(struct cm_msbc_tx_packetizer *state)
{
	if (state != NULL)
		memset(state, 0, sizeof(*state));
}

int cm_msbc_tx_packetize(struct cm_msbc_tx_packetizer *state,
	const uint8_t frame[CM_MSBC_FRAME_BYTES], size_t mtu,
	uint8_t output[CM_MSBC_MAX_SCO_MTU], size_t *output_length)
{
	size_t emitted;

	if (state == NULL || frame == NULL || output == NULL ||
		output_length == NULL)
		return -EINVAL;
	*output_length = 0;
	if (mtu == 0 || mtu > CM_MSBC_MAX_SCO_MTU)
		return -EINVAL;
	if (state->length >= mtu ||
		state->length + CM_MSBC_FRAME_BYTES > sizeof(state->buffer))
		return -ENOBUFS;

	memcpy(state->buffer + state->length, frame, CM_MSBC_FRAME_BYTES);
	state->length += CM_MSBC_FRAME_BYTES;
	emitted = state->length / mtu * mtu;
	if (emitted > CM_MSBC_MAX_SCO_MTU)
		return -ENOBUFS;
	if (emitted != 0) {
		memcpy(output, state->buffer, emitted);
		memmove(state->buffer, state->buffer + emitted,
			state->length - emitted);
		state->length -= emitted;
	}
	*output_length = emitted;
	return 0;
}

int cm_msbc_decode_feed(struct cm_msbc *state,
	const uint8_t *data, size_t data_length,
	int16_t *pcm, size_t pcm_capacity_samples)
{
	size_t produced = 0;

	if (state == NULL || !state->initialized || pcm == NULL)
		return -EINVAL;
	if (data_length != 0 && data == NULL)
		return -EINVAL;
	if (data_length > sizeof(state->rx_buffer) - state->rx_length) {
		CM_STAT_ADD(state, enobufs, 1);
		return -ENOBUFS;
	}
	CM_STAT_ADD(state, feed_bytes, data_length);

	if (data_length != 0) {
		memcpy(state->rx_buffer + state->rx_length, data, data_length);
		state->rx_length += data_length;
	}

	while (state->rx_length >= 2) {
		size_t offset = cm_h2_find(state->rx_buffer, state->rx_length);
		uint8_t sequence;
		unsigned int missing = 0;
		unsigned int missing_to_conceal;
		size_t needed_samples;
		ssize_t consumed;
		size_t decoded = 0;

		if (offset != 0) {
			cm_record_discard(state, offset);
			memmove(state->rx_buffer, state->rx_buffer + offset,
				state->rx_length - offset);
			state->rx_length -= offset;
		}
		if (state->rx_length < CM_MSBC_FRAME_BYTES)
			break;
		if (!cm_h2_unpack(state->rx_buffer, &sequence)) {
			cm_record_discard(state, 1);
			memmove(state->rx_buffer, state->rx_buffer + 1,
				--state->rx_length);
			continue;
		}

		if (state->rx_sequence_initialized) {
			uint8_t expected = (state->rx_sequence + 1) & 3;
			missing = (sequence + 4 - expected) & 3;
		}

		needed_samples = ((size_t) missing + 1) *
			CM_MSBC_PCM_SAMPLES;
		/*
		 * Capacity failure is transactional for the pending frame: do
		 * not update sequence state, PLC history or the decoder until
		 * the caller supplies enough room.  Samples already completed
		 * in this call are returned normally.
		 */
		if (produced > pcm_capacity_samples ||
			needed_samples > pcm_capacity_samples - produced) {
			CM_STAT_ADD(state, enospc, 1);
			return produced != 0 ? (int) produced : -ENOSPC;
		}

		state->rx_sequence_initialized = true;
		state->rx_sequence = sequence;
		if (missing != 0) {
			CM_STAT_ADD(state, seq_gap_events, 1);
			CM_STAT_ADD(state, seq_gap_frames, missing);
		}
		missing_to_conceal = missing;
		while (missing_to_conceal-- != 0) {
			cm_conceal_frame(state, pcm + produced);
			produced += CM_MSBC_PCM_SAMPLES;
		}

		consumed = sbc_decode(&state->decoder, state->rx_buffer + 2,
			CM_MSBC_PAYLOAD_BYTES, pcm + produced, CM_MSBC_PCM_BYTES,
			&decoded);
		if (consumed != CM_MSBC_PAYLOAD_BYTES ||
			decoded != CM_MSBC_PCM_BYTES) {
			CM_STAT_ADD(state, decode_failures, 1);
			cm_conceal_frame(state, pcm + produced);
			produced += CM_MSBC_PCM_SAMPLES;
			/* Advance one byte and let H2 synchronization recover. */
			cm_record_discard(state, 1);
			memmove(state->rx_buffer, state->rx_buffer + 1,
				--state->rx_length);
			continue;
		}

		cm_accept_good_frame(state, pcm + produced);
		CM_STAT_ADD(state, good_frames, 1);
		produced += CM_MSBC_PCM_SAMPLES;
		memmove(state->rx_buffer,
			state->rx_buffer + CM_MSBC_FRAME_BYTES,
			state->rx_length - CM_MSBC_FRAME_BYTES);
		state->rx_length -= CM_MSBC_FRAME_BYTES;
		state->rx_resync_active = false;
	}

	return (int) produced;
}
