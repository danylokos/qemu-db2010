/* Scoped host Layer III/AAC service, not original DSP execution. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ARM_DB2010_AUDIO_CODEC_H
#define HW_ARM_DB2010_AUDIO_CODEC_H
#include "db2010-minimp3/minimp3.h"
#include "db2010-audio-samples.h"
#include "db2010-audio-resample.h"
#include "db2010-aac-lc.h"
/* Worst legal Layer III frame:576 samples at8kHz ->3456 at48kHz.
 * Input holds a maximal1441-byte frame plus one full794-word ring payload. */
#define DB2010_AUDIO_MAX_PCM_FRAMES 3456
#define DB2010_AUDIO_INPUT_BYTES 4096
typedef struct DB2010AudioRuntime {
    mp3dec_t decoder;
    DB2010AudioSamples samples;
    uint8_t input[DB2010_AUDIO_INPUT_BYTES];
    unsigned input_bytes;
    int16_t pcm[DB2010_AUDIO_MAX_PCM_FRAMES * 2];
    unsigned source_rate;
    unsigned source_channels;
    bool coefficient_ready;
    DB2010AudioResampler resampler;
    unsigned pcm_frames;
    bool active, eof, completed, credit_notify;
    bool interrupted;
    bool paused;
    unsigned source_codec; /* 0: inherited Layer III;1: recovered AAC-LC service. */
    DB2010AacLc aac;
    bool underrun, underrun_notified;
    uint64_t underruns_offered, underruns_acked;
    unsigned credit, rx_kind;
    uint64_t due, start_ns, frames_decoded, frames_played, pcm_hash, nonzero_samples;
    uint64_t credits_offered, credits_acked, words_credited, completions_acked;
} DB2010AudioRuntime;
static inline uint64_t db2010_audio_duration(unsigned frames)
{
    return ((uint64_t)frames * 1000000000 + 47999) / 48000;
}
unsigned db2010_audio_frame_bytes(DB2010AudioRuntime *a);
const char *db2010_audio_decode(DB2010AudioRuntime *a);
#endif
