/* Original-firmware SIMIF byte transport with a declared dummy card.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_SIMIF_H
#define DB2010_SIMIF_H
#include "db2010-bus.h"
#include "db2010-links.h"
#include "db2010-sim-card.h"
typedef struct DB2010SimEvent {
    uint64_t ns;
    uint32_t pc;
    uint8_t offset, value;
    bool write;
} DB2010SimEvent;
typedef struct DB2010Simif {
    bool omit_diagnostics;
    uint32_t byte_ns;
    uint8_t regs[8];
    bool active, rx_ready, tx_ready, tx_event, setup_known;
    int64_t rx_due, tx_due;
    uint64_t activations, tx_bytes, rx_bytes, events;
    DB2010Signal rx_irq, tx_irq;
    DB2010SimCard card;
    uint64_t history_baseline; /* Host capture session, excluded from migration. */
    DB2010SimEvent history[256];
} DB2010Simif;
const DB2010RegMap *dbsim_map(void);
const char *dbsim_access(void *opaque, DB2010Access *a);
void dbsim_reset(DB2010Simif *s);
void dbsim_advance(DB2010Simif *s, uint64_t now);
int64_t dbsim_deadline(DB2010Simif *s);
typedef struct QDict QDict;
void dbsim_export(DB2010Simif *s, QDict *snapshot);
#endif
