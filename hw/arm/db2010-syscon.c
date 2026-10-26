/* Nominal clock modes recovered from original MAIN 44A35F34/44A36168.
 * Exact latches/guards and checkpoint storage remain in the register bank.
 * Unknown selections disable these descriptive Clock outputs, not the CPU.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qobject/qdict.h"
#include "db2010-syscon.h"

bool dbsys_register(uint32_t address)
{
    return address == 0xf909000c || address == 0xf909001c || address == 0xf909f800;
}

static const DB2010Register *known_register(DB2010Syscon *s, uint32_t address)
{
    DB2010Register *r = s->bank ? dbreg_find(s->bank, address) : NULL;
    return r && r->width == 2 && dbreg_known(r) ? r : NULL;
}

void dbsys_update(DB2010Syscon *s)
{
    const DB2010Register *mux = known_register(s, 0xf909001c);
    const DB2010Register *setup = known_register(s, 0xf909000c);
    const DB2010Register *pll = known_register(s, 0xf909f800);
    s->cpu_hz = s->bus_hz = s->pll_hz = 0;
    s->pll_ready = false;
    if (setup && setup->value == 0xf800 && pll) {
        if ((pll->value & ~1u) == 0x410) { s->pll_hz = 52000000; }
        if ((pll->value & ~1u) == 0x810) { s->pll_hz = 104000000; }
        /* In normal boot read_mask bit0 is the existing immediate-lock mock.
         * Conditional masks do not establish a persistent ready state. */
        uint32_t mask = pll->conditional ? 0 : pll->read_mask;
        s->pll_ready = s->pll_hz && ((pll->value | mask) & 1);
    }
    if (mux) {
        unsigned selection = mux->value & 0xf00;
        if (selection == 0x800) {
            s->cpu_hz = s->bus_hz = 13000000;
        } else if (s->pll_ready &&
                   ((selection == 0x500 && s->pll_hz == 52000000) ||
                    (selection == 0x600 && s->pll_hz == 104000000))) {
            s->cpu_hz = s->pll_hz;
            s->bus_hz = s->cpu_hz / 2;
        }
    }
    clock_update_hz(s->cpu_clock, s->cpu_hz);
    clock_update_hz(s->bus_clock, s->bus_hz);
}

const char *dbsys_access(void *opaque, DB2010Access *a)
{
    DB2010Syscon *s = opaque;
    const char *fault = db2010_bank_access(s->bank, a);
    if (!fault && a->commit && a->write) { dbsys_update(s); }
    return fault;
}

void dbsys_reset(DB2010Syscon *s)
{
    /* QEMU's bus reset order need not match board construction order. Reset
     * the owned latches before deriving outputs, independently of bank reset. */
    for (unsigned i = 0; s->bank && i < s->bank->registers->len; i++) {
        DB2010Register *r = g_ptr_array_index(s->bank->registers, i);
        if (!dbsys_register(r->address)) { continue; }
        r->value = r->initial;
        r->known_mask = r->initial_known ? UINT32_MAX >> (8 * (4 - r->width)) : 0;
    }
    dbsys_update(s);
}

void dbsys_export(DB2010Syscon *s, QDict *snapshot)
{
    QDict *out = qdict_new();
    if (s->cpu_hz) { qdict_put_int(out, "cpu_hz", s->cpu_hz); }
    else { qdict_put_null(out, "cpu_hz"); }
    if (s->bus_hz) { qdict_put_int(out, "bus_hz", s->bus_hz); }
    else { qdict_put_null(out, "bus_hz"); }
    if (s->pll_hz) { qdict_put_int(out, "pll_hz", s->pll_hz); }
    else { qdict_put_null(out, "pll_hz"); }
    qdict_put_bool(out, "pll_ready", s->pll_ready);
    qdict_put_str(out, "basis", "firmware-derived-nominal");
    qdict_put_bool(out, "execution_cycle_timing", false);
    qdict_put_bool(out, "physical_lock_timing", false);
    qdict_put(snapshot, "syscon", out);
}
