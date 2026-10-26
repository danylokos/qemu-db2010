#include "qemu/osdep.h"
#include "db2010-internal.h"
#include "exec/icount.h"

void db2010_reset(void *opaque)
{
    DB2010MachineState *s = opaque;
    CPUARMState *env = &s->cpu->env;
    unsigned i;
    g_clear_pointer(&s->snapshot_json, g_free);
    s->snapshot_size = s->snapshot_offset = 0;
    cpu_reset(CPU(s->cpu));
    s->instruction_pc = 0;
    s->kgen_inits = s->kgen_updates = s->kgen_finals = s->kgen_bytes = 0;
    s->kgen_last_context = 0; s->wfi_waits = 0;
    s->sha_inits = s->sha_updates = s->sha_finals = s->sha_bytes = 0;
    s->sha_last_context = 0;
    s->etx_calls = 0; s->etx_last_request = 0;
    if (s->has_timer) { qobject_unref(s->irq_events); s->irq_events = qlist_new(); }
    for (i = 0; i < s->regions->len; i++) {
        DB2010Region *r = g_ptr_array_index(s->regions, i);
        uint8_t *data = memory_region_get_ram_ptr(r->mr);
        if (r->initial) {
            memcpy(data, r->initial, r->size);
            memcpy(r->known, r->initial_known, r->size);
        } else {
            memset(data, 0, r->size);
            memset(r->known, 0, r->size);
        }
    }
    for (i = 0; i < 2; i++) {
        if (s->tcm[i].region) {
            memory_region_set_enabled(s->tcm[i].region->mr, false);
            s->tcm[i].region->active = false;
        }
        s->tcm[i].base = 0;
        s->tcm[i].enabled = false;
    }
    memcpy(env->regs, s->initial_registers, sizeof(env->regs));
    env->regs[15] = s->entry;
    cpsr_write(env, s->cpsr, UINT32_MAX, CPSRWriteRaw);
    env->cp15.sctlr_el[1] = s->sctlr;
    arm_rebuild_hflags(env);
    s->count = 0;
    s->native_count_base = s->research_guards ? 0 : icount_get_raw();
    qobject_unref(s->stop);
    s->stop = NULL;
}
