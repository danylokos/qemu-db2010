/* Original F500 word/ready/strobe interface; physical peer is unresolved.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DB2010_F500_PORT_H
#define DB2010_F500_PORT_H

typedef struct QDict QDict;
#include "db2010-bus.h"

typedef struct DB2010F500Command {
    uint16_t tag, mask, required;
} DB2010F500Command;

typedef struct DB2010F500Variant {
    const char *profile;
    DB2010RegisterSpec registers[4]; /* Immutable map initializer rows: setup, TX, strobe, status. */
    unsigned setup_value, tag_shift, tag_mask, snapshot_tags;
    const DB2010F500Command *commands;
    unsigned command_count;
} DB2010F500Variant;

extern const DB2010F500Variant dbfp_primary, dbfp_auxiliary;
const DB2010RegMap *dbfp_map(const DB2010F500Variant *variant);

typedef struct DB2010F500PortConfig {
    const DB2010F500Variant *variant;
    unsigned latency;
} DB2010F500PortConfig;

typedef enum DB2010F500Pending { DBFP_READY = 0, DBFP_PENDING = 1 } DB2010F500Pending;
typedef enum DB2010F500Strobe { DBFP_STROBE_LOW = 0, DBFP_STROBE_HIGH = 1 } DB2010F500Strobe;

typedef struct DB2010F500PortRuntime {
    bool setup_known, status_known, strobe_known;
    DB2010F500Pending pending;
    DB2010F500Strobe strobe_high;
    uint8_t setup_value;
    uint16_t word, last_by_tag[16];
    bool tag_known[16];
    uint64_t due, commands, completions, reads, busy_reads, pulses;
} DB2010F500PortRuntime;

typedef struct DB2010F500Port {
    DB2010F500PortConfig config;
    DB2010F500PortRuntime state;
} DB2010F500Port;

const char *dbfp_bus_access(void *opaque, DB2010Access *a);

void dbfp_reset(DB2010F500Port *p);

bool dbfp_contains(uint32_t address, unsigned size, const DB2010F500Variant *variant);

void dbfp_finish(DB2010F500Port *p, uint64_t now);

const char *dbfp_access(DB2010F500Port *p, uint32_t address,
                               unsigned size, uint64_t now, bool write,
                               uint32_t *value, bool apply);

QDict *dbfp_snapshot(DB2010F500Port *p);

#endif
