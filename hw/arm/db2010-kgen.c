/* Compiled DB2010 component. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "db2010-kgen.h"

uint32_t dbk_rol(uint32_t v, unsigned n)
{
    return v << n | v >> (32 - n);
}

void dbk_compress(uint8_t *ctx, const uint8_t *block)
{
    uint32_t w[80], h[5], a, b, c, d, e;
    for (unsigned i = 0; i < 16; i++) {
        w[i] = ldl_be_p(block + 4 * i);
    }
    for (unsigned i = 16; i < 80; i++) {
        w[i] = dbk_rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }
    for (unsigned i = 0; i < 5; i++) {
        h[i] = ldl_le_p(ctx + 4 * i);
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (unsigned i = 0; i < 80; i++) {
        uint32_t f, k, next;
        if (i < 20) {
            f = (b & c) | (~b & d); k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d; k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d; k = 0xca62c1d6;
        }
        next = dbk_rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = dbk_rol(b, 30); b = a; a = next;
    }
    stl_le_p(ctx, h[0] + a); stl_le_p(ctx + 4, h[1] + b);
    stl_le_p(ctx + 8, h[2] + c); stl_le_p(ctx + 12, h[3] + d);
    stl_le_p(ctx + 16, h[4] + e);
}

void dbk_init(uint8_t *ctx)
{
    static const uint32_t iv[] = {
        0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0
    };
    memset(ctx, 0, 92);
    for (unsigned i = 0; i < 5; i++) {
        stl_le_p(ctx + 4 * i, iv[i]);
    }
}

bool dbk_update(uint8_t *ctx, const uint8_t *data, size_t size)
{
    uint64_t bits = ldq_le_p(ctx + 20);
    unsigned used = (bits / 8) % 64;
    if (bits % 8) {
        return false;
    }
    stq_le_p(ctx + 20, bits + (uint64_t)size * 8);
    while (size) {
        unsigned n = MIN(size, 64 - used);
        memcpy(ctx + 28 + used, data, n);
        data += n; size -= n; used += n;
        if (used == 64) {
            dbk_compress(ctx, ctx + 28);
            used = 0;
        }
    }
    memset(ctx + 28 + used, 0, 64 - used);
    return true;
}

bool dbk_final(uint8_t *ctx, uint8_t *digest)
{
    uint64_t bits = ldq_le_p(ctx + 20);
    unsigned used = (bits / 8) % 64;
    unsigned pad = used < 56 ? 56 - used : 120 - used;
    uint8_t padding[128] = { 0x80 };
    if (bits % 8) {
        return false;
    }
    stq_be_p(padding + pad, bits);
    if (!dbk_update(ctx, padding, pad + 8)) {
        return false;
    }
    for (unsigned i = 0; i < 5; i++) {
        stl_be_p(digest + 4 * i, ldl_le_p(ctx + 4 * i));
    }
    return true;
}
