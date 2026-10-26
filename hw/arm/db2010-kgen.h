/* Explicit synthetic SHA-1 state, NOT recovered DB2010 KGEN ROM behavior.
 * Assumed layout: LE chaining words20 / LE bit count8 / pending buffer64.
 * No entropy source, chip secret, MMIO device or ROM timing is supplied.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_DB2010_KGEN_H
#define HW_ARM_DB2010_KGEN_H

#include "qemu/bswap.h"

uint32_t dbk_rol(uint32_t v, unsigned n);

void dbk_compress(uint8_t *ctx, const uint8_t *block);

void dbk_init(uint8_t *ctx);

bool dbk_update(uint8_t *ctx, const uint8_t *data, size_t size);

/* Explicit Final hypothesis: SHA-1 padding, big-endian digest, padded context
 * retained in the same LE92 layout. No claim about actual ROM side effects. */
bool dbk_final(uint8_t *ctx, uint8_t *digest);

#endif
