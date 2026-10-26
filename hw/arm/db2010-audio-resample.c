/* Exact reduced source/48000 ratio, linear interpolation, nearest rounding with ties away
 * from zero. No frame-local phase reset, floating point or invented silence.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "db2010-audio-resample.h"

unsigned db2010_audio_resample(DB2010AudioResampler *r, const int16_t *input,
                               unsigned frames, int16_t *output, bool end,
                               unsigned source_rate)
{
    unsigned count = 0;
    assert(db2010_audio_rate_supported(source_rate));
    unsigned divisor = db2010_audio_rate_gcd(source_rate);
    int scale = 48000 / divisor;
    unsigned step = source_rate / divisor;
    assert(frames <= 1152);
    assert(!r->ended || (!frames && end));
    for (unsigned i = 0; i < frames; i++) {
        const int16_t *current = input + i * 2;
        if (!r->primed) {
            output[count * 2] = current[0]; output[count * 2 + 1] = current[1];
            count++; r->phase = step; r->primed = true;
        } else {
            while (r->phase <= scale) {
                for (unsigned ch = 0; ch < 2; ch++) {
                    int value = r->previous[ch] * (int)(scale - r->phase) +
                                current[ch] * (int)r->phase;
                    output[count * 2 + ch] = value >= 0 ? (value + scale / 2) / (int)scale :
                                                        -((-value + scale / 2) / (int)scale);
                }
                count++; r->phase += step;
            }
            r->phase -= scale;
        }
        r->previous[0] = current[0]; r->previous[1] = current[1];
        r->input_frames++;
    }
    if (end && !r->ended) {
        /* Hold the last source value only over its remaining sample interval.
         * Total output is ceil(input_frames*48000/source_rate); no extra padding frame. */
        while (r->primed && r->phase < scale) {
            output[count * 2] = r->previous[0]; output[count * 2 + 1] = r->previous[1];
            count++; r->phase += step;
        }
        r->ended = true;
    }
    r->output_frames += count;
    return count;
}
