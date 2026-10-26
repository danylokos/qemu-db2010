/* Integral-Hz phase accumulators. Fraction is a billionth of a tick and
 * survives input clock changes/gating. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"
#include "db2010-time.h"
uint64_t db2010_virtual_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}
uint64_t db2010_clock_sync(DB2010ClockPhase *p, Clock *clk,
                                        uint64_t now)
{
    assert(now >= p->ns);
    __uint128_t amount = (__uint128_t)(now - p->ns) * clock_get_hz(clk) + p->fraction;
    uint64_t ticks = amount / NANOSECONDS_PER_SECOND;
    p->fraction = amount % NANOSECONDS_PER_SECOND;
    p->ticks += ticks;
    p->ns = now;
    return ticks;
}
int64_t db2010_clock_deadline(const DB2010ClockPhase *p,
                                            Clock *clk, uint64_t ticks)
{
    uint64_t hz = clock_get_hz(clk);
    if (!hz) { return -1; }
    __uint128_t ns = (__uint128_t)ticks * NANOSECONDS_PER_SECOND - p->fraction;
    ns = p->ns + (ns + hz - 1) / hz;
    return ns <= INT64_MAX ? (int64_t)ns : -1;
}
