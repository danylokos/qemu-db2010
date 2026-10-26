/* Bounded AAC-LC host state; no pointers or original DSP execution.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_ARM_DB2010_AAC_LC_H
#define HW_ARM_DB2010_AAC_LC_H
#include <stdbool.h>
#include <stdint.h>

typedef struct DB2010AacLc {
    bool primed;
    uint8_t window_shape[2];
    uint32_t rng[2];
    int32_t overlap[2][1024];
} DB2010AacLc;

/* Exact raw AAC-LC stereo16000Hz access units; one1024-frame result.
 * A fresh upstream handle reconstructs transient allocations per call.
 * Only successful calls commit overlap/window/PNS history. */
bool db2010_aac_lc_decode(DB2010AacLc *state, const uint8_t *raw,
                          unsigned bytes, int16_t output[2048]);
#endif
