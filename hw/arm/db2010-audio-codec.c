/* Deterministic Layer III/AAC-LC host service, independent of file identity.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "db2010-audio-codec.h"

/* Reuse the pinned decoder's header tables. Free format needs lookahead and
 * resynchronization, neither part of this framed firmware service. */
static bool layer3_header(const uint8_t *b)
{
    return hdr_valid(b) && HDR_GET_LAYER(b) == 1 &&
           !HDR_IS_FREE_FORMAT(b) && ((b[1] >> 3) & 3) != 1;
}

unsigned db2010_audio_frame_bytes(DB2010AudioRuntime *a)
{
    if (a->source_codec == 1) {
        if (a->input_bytes < 8) { return 8; }
        unsigned bytes = (a->input[4] << 5) | (a->input[5] >> 3);
        return bytes < 9 || bytes > sizeof(a->input) ? 8 : ((bytes + 1) & ~1u);
    }
    if (a->input_bytes < 4 || !layer3_header(a->input)) {
        return 4;
    }
    return hdr_frame_bytes(a->input, 0) + hdr_padding(a->input);
}

static const char *aac_decode(DB2010AudioRuntime *a)
{
    if (a->source_rate != 16000 || a->source_channels != 2) {
        return "dsp_peer_audio_source_format_unmodeled";
    }
    if (a->eof && !a->input_bytes) {
        a->pcm_frames = db2010_audio_resample(&a->resampler, NULL, 0, a->pcm, true, 16000);
        return NULL;
    }
    if (a->input_bytes < 8) { return NULL; }
    const uint8_t *b = a->input;
    unsigned bytes = (b[4] << 5) | (b[5] >> 3);
    /* Original4488E864 length/sequence packing, not standard ADTS.
     * One zero alignment byte belongs to this individual native access unit. */
    if (memcmp(b, "\xff\xf1\x60\x80", 4) || bytes < 9 || bytes > sizeof(a->input) ||
        (b[5] & 7) != 7 || b[7]) {
        return "dsp_peer_aac_source_header_unmodeled";
    }
    /* Byte6 counts generated source units. Original4488DCB8..DCE0 may
     * discard stale units before submitting them during synchronized video
     * rebuffering, so it is not a count of delivered/decoded frames. */
    unsigned padded = (bytes + 1) & ~1u;
    if (a->input_bytes < padded) { return NULL; }
    if ((bytes & 1) && b[bytes]) { return "dsp_peer_aac_alignment_unmodeled"; }
    int16_t pcm[2048];
    if (!db2010_aac_lc_decode(&a->aac, b + 8, bytes - 8, pcm)) {
        return "dsp_peer_aac_decode_unmodeled";
    }
    a->pcm_frames = db2010_audio_resample(&a->resampler, pcm, 1024, a->pcm, false, 16000);
    a->frames_decoded++;
    a->input_bytes -= padded;
    memmove(a->input, a->input + padded, a->input_bytes);
    return NULL;
}

const char *db2010_audio_decode(DB2010AudioRuntime *a)
{
    if (a->source_codec == 1) { return aac_decode(a); }
    if (a->source_codec) { return "dsp_peer_audio_source_format_unmodeled"; }
    unsigned rate = a->source_rate ? a->source_rate : 48000;
    unsigned channels = a->source_channels ? a->source_channels : 2;
    bool converted = rate != 48000;
    /* Native4488DCE6 rounds source bytes up to halfwords after4488DE0C
     * zeroes staging. Accept one zero transport byte only at a frame boundary
     * after actual decoding, for every source configuration. */
    if (a->eof && a->frames_decoded && a->input_bytes == 1 && !a->input[0]) {
        a->input_bytes = 0;
    }
    if (converted && a->eof && !a->input_bytes) {
        a->pcm_frames = db2010_audio_resample(&a->resampler, NULL, 0,
                                             a->pcm, true, rate);
        return NULL;
    }
    if (a->input_bytes < 4) {
        return NULL;
    }
    const uint8_t *b = a->input;
    /* Bitrate, padding, protection and stereo coding mode may vary per frame.
     * Sample rate/channel count must agree with the firmware's setup. Never
     * scan past invalid data, skip metadata or synthesize a truncated frame. */
    if (!layer3_header(b) || hdr_sample_rate_hz(b) != rate ||
        (HDR_IS_MONO(b) ? 1 : 2) != channels) {
        return "dsp_peer_audio_source_format_unmodeled";
    }
    unsigned bytes = db2010_audio_frame_bytes(a);
    if (a->input_bytes < bytes) {
        return NULL;
    }
    mp3dec_frame_info_t info = {0};
    mp3dec_t decoder = a->decoder;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME] = {0};
    int frames = mp3dec_decode_frame(&decoder, b, bytes, pcm, &info);
    if (frames != hdr_frame_samples(b) || info.frame_bytes != bytes ||
        info.frame_offset || info.hz != rate || info.channels != channels ||
        info.layer != 3 || info.bitrate_kbps != hdr_bitrate_kbps(b)) {
        return "dsp_peer_audio_decode_unmodeled";
    }
    a->decoder = decoder;
    if (channels == 1) {
        for (int i = frames - 1; i >= 0; i--) {
            pcm[i * 2 + 1] = pcm[i * 2] = pcm[i];
        }
    }
    if (converted) {
        a->pcm_frames = db2010_audio_resample(&a->resampler, pcm, frames,
                                             a->pcm, false, rate);
    } else {
        memcpy(a->pcm, pcm, frames * 2 * sizeof(*pcm));
        a->pcm_frames = frames;
    }
    assert(a->pcm_frames <= DB2010_AUDIO_MAX_PCM_FRAMES);
    a->frames_decoded++;
    a->input_bytes -= bytes;
    memmove(a->input, a->input + bytes, a->input_bytes);
    return NULL;
}
