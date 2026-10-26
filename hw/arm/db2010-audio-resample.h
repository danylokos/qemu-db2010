/* Bounded integer Layer III sample rates ->48kHz presentation policy, not a handset DSP model.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_AUDIO_RESAMPLE_H
#define DB2010_AUDIO_RESAMPLE_H
typedef struct DB2010AudioResampler {
    int16_t previous[2];
    unsigned phase;
    bool primed, ended;
    uint64_t input_frames, output_frames;
} DB2010AudioResampler;
/* Reduced ratio keeps existing44.1/22.05kHz checkpoint phase units intact. */
static inline unsigned db2010_audio_rate_gcd(unsigned rate)
{
    unsigned divisor = 48000;
    while (rate) {
        unsigned next = divisor % rate;
        divisor = rate;
        rate = next;
    }
    return divisor;
}

static inline bool db2010_audio_rate_supported(unsigned rate)
{
    return rate == 8000 || rate == 11025 || rate == 12000 ||
           rate == 16000 || rate == 22050 || rate == 24000 ||
           rate == 32000 || rate == 44100 || rate == 48000;
}

unsigned db2010_audio_resample(DB2010AudioResampler *r, const int16_t *input,
                               unsigned frames, int16_t *output, bool end,
                               unsigned source_rate);
#endif
