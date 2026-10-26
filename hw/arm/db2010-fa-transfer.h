/* DSP-manager FA transfer port. Protocol follows original ARM callers; pair
 * addressing, initial supplied cells and latency2 are explicit hypotheses.
 * No DSP instruction execution, mailbox response or interrupt is synthesized.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_DB2010_FA_TRANSFER_H
#define HW_ARM_DB2010_FA_TRANSFER_H

#include "db2010-bus.h"
const DB2010RegMap *dbfa_map(void);
typedef struct QDict QDict;
typedef enum DB2010FaPhase {
    DBFA_IDLE = 0, DBFA_WRITE_PENDING = 1, DBFA_READ_PENDING = 2,
} DB2010FaPhase;
typedef struct DB2010FaTransfer {
    GHashTable *memory, *initial;
    uint32_t address, data;
    DB2010FaPhase command;
    bool address_known, data_known;
    uint64_t due, writes, reads, completions, polls;
} DB2010FaTransfer;

bool dbfa_contains(uint32_t address, unsigned size);

bool dbfa_word(DB2010FaTransfer *f, uint32_t address, uint16_t *value);

void dbfa_set_word(DB2010FaTransfer *f, uint32_t address, uint16_t value);

const char *dbfa_bus_access(void *opaque, DB2010Access *a);

void dbfa_reset(DB2010FaTransfer *f);

void dbfa_finish(DB2010FaTransfer *f, uint64_t now);

/* Pre-effect validation and native bus commit share this contract; validation
 * never changes memory, latches, counters or pending completion. */
const char *dbfa_access(DB2010FaTransfer *f, uint32_t address,
                               unsigned size, uint64_t now, bool write,
                               uint32_t *value, bool apply);

gint dbfa_key_compare(gconstpointer a, gconstpointer b, gpointer opaque);

QDict *dbfa_snapshot(DB2010FaTransfer *f);

#endif
