/* Firmware-derived nominal CPU clock selection; not a CPU cycle model.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_SYSCON_H
#define DB2010_SYSCON_H
#include "hw/core/clock.h"
#include "db2010-components.h"

typedef struct DB2010Syscon {
    DB2010RegisterBank *bank;
    Clock *cpu_clock, *bus_clock;
    uint64_t cpu_hz, bus_hz, pll_hz;
    bool pll_ready;
} DB2010Syscon;

bool dbsys_register(uint32_t address);
void dbsys_update(DB2010Syscon *s);
void dbsys_reset(DB2010Syscon *s);
const char *dbsys_access(void *opaque, DB2010Access *a);
void dbsys_export(DB2010Syscon *s, QDict *snapshot);
#endif
