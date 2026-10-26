/* Native clock phase; no host timeline. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef DB2010_TIME_H
#define DB2010_TIME_H
typedef struct Clock Clock;
typedef struct DB2010ClockPhase {
    uint64_t ns, ticks, fraction;
} DB2010ClockPhase;
uint64_t db2010_virtual_ns(void);
uint64_t db2010_clock_sync(DB2010ClockPhase *p, Clock *clk, uint64_t now);
int64_t db2010_clock_deadline(const DB2010ClockPhase *p, Clock *clk, uint64_t ticks);
#endif
