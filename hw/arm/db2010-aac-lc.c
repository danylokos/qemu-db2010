/* FAAD2 host AAC-LC service, independent of firmware or media identity.
 * Code from FAAD2 is copyright (c) Nero AG, www.nero.com
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
#include <string.h>
#include "db2010-aac-lc.h"
#include "db2010-faad2/libfaad/common.h"
#include "db2010-faad2/libfaad/structs.h"
#include "db2010-faad2/libfaad/syntax.h"

_Static_assert(sizeof(real_t) == sizeof(int32_t), "fixed-point AAC state");

bool db2010_aac_lc_decode(DB2010AacLc *state, const uint8_t *raw,
                          unsigned bytes, int16_t output[2048])
{
    /* Native declared16000Hz stereo LC configuration; raw CPE input only.
     * Do not scan past data, guess another profile or conceal errors. */
    if (!bytes || (raw[0] >> 5) != ID_CPE) { return false; }
    NeAACDecStruct *d = NeAACDecOpen();
    if (!d) { return false; }
    uint8_t asc[] = {0x14, 0x10};
    unsigned long rate = 0;
    unsigned char channels = 0;
    bool valid = NeAACDecInit2(d, asc, sizeof(asc), &rate, &channels) == 0 &&
                 rate == 16000 && channels == 2;
    /* Keep every1024-frame access unit, including the priming unit. FAAD2's
     * file frontend normally suppresses that first output; this native service
     * owns source/sample timing rather than an MP4 gapless-trimming policy. */
    d->frame = 1;
    if (valid && state->primed) {
        d->__r1 = state->rng[0]; d->__r2 = state->rng[1];
        for (unsigned ch = 0; ch < 2; ch++) {
            d->window_shape_prev[ch] = state->window_shape[ch];
            d->fb_intermed[ch] = faad_malloc(sizeof(state->overlap[ch]));
            if (!d->fb_intermed[ch]) { valid = false; break; }
            memcpy(d->fb_intermed[ch], state->overlap[ch], sizeof(state->overlap[ch]));
        }
    }
    NeAACDecFrameInfo info = {0};
    const int16_t *pcm = valid ? NeAACDecDecode(d, &info, (unsigned char *)raw, bytes) : NULL;
    valid = pcm && !info.error && info.samples == 2048 && info.bytesconsumed == bytes &&
            info.channels == 2 && info.samplerate == 16000 && info.object_type == LC &&
            d->fr_ch_ele == 1 && d->first_syn_ele == ID_CPE &&
            d->element_output_channels[0] == 2 && !d->pce_set && !d->drc->present &&
            !d->downMatrix && !d->upMatrix;
    if (valid) {
        memcpy(output, pcm, 2048 * sizeof(*output));
        state->primed = true;
        state->rng[0] = d->__r1; state->rng[1] = d->__r2;
        for (unsigned ch = 0; ch < 2; ch++) {
            state->window_shape[ch] = d->window_shape_prev[ch];
            memcpy(state->overlap[ch], d->fb_intermed[ch], sizeof(state->overlap[ch]));
        }
    }
    NeAACDecClose(d);
    return valid;
}
