/* Bounded F5000800 command/latch hypothesis, not a measured reset controller.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef DB2010_F500_CONTROL_H
#define DB2010_F500_CONTROL_H

#include "db2010-bus.h"
const DB2010RegMap *dbfc_map(void);
typedef enum DB2010F500ControlPhase {
    DBFC_READY = 0, DBFC_PENDING = 1,
} DB2010F500ControlPhase;
typedef struct DB2010F500ControlRuntime {
    DB2010F500ControlPhase pending;
    uint64_t due, commands, completions, busy_reads, ready_reads, config_writes;
} DB2010F500ControlRuntime;
typedef struct DB2010F500Control {
    struct { unsigned latency; } config;
    DB2010F500ControlRuntime state;
} DB2010F500Control;
typedef struct DB2010F500ControlLink {
    DB2010F500Control *control;
    uint32_t *latch;
    uint32_t *known_mask;
} DB2010F500ControlLink;
const char *dbfc_bus_access(void *opaque, DB2010Access *a);

void dbfc_reset(DB2010F500Control *s);

void dbfc_advance(DB2010F500Control *s, uint32_t *latch, uint64_t now);

const char *dbfc_access(DB2010F500Control *s, uint32_t *latch,
                              bool known, uint64_t now, unsigned size,
                              bool write, uint32_t *value);

typedef struct QDict QDict;
typedef struct QList QList;
void dbfc_export(DB2010F500Control *model, QDict *snapshot);
#endif
