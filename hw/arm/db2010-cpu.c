/* Required DB2010 CPU integration, independent of optional research checks.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "db2010-internal.h"
#include "exec/icount.h"

void db2010_runtime_observe(DB2010MachineState *s)
{
    if (!s->research_guards) {
        s->count = icount_get_raw() - s->native_count_base;
        s->instruction_pc = s->cpu->env.regs[15];
    }
}

bool db2010_cpu_instruction_needed(void *opaque, uint32_t pc,
                                  uint32_t insn, bool thumb)
{
    DB2010MachineState *s = opaque;
    return (s->synthetic_etx && pc == 0xffff0ddc) ||
        (s->synthetic_kgen && (pc == 0xffff12a2 || pc == 0xffff12be ||
                               (s->kgen_final && pc == 0xffff12c2))) ||
        (s->synthetic_sha && (pc == 0xffff0f48 || pc == 0xffff112c || pc == 0xffff129e)) ||
        (s->has_nor && s->nor->probes && (pc == 0x4c002c32 || pc == 0x4c002c3a)) ||
        (!thumb && (insn & 0x0f000f10) == 0x0e000f10);
}

void db2010_cpu_instruction(void *opaque, CPUARMState *env,
                            unsigned size, uint32_t insn)
{
    DB2010MachineState *s = opaque;
    uint32_t pc = env->regs[15];
    db2010_runtime_observe(s);
    db2010_cpu_service(s, env, pc, size);
    db2010_cpu_control(s, env, pc, size, insn);
    if (s->has_nor && s->nor->probes &&
        (pc == 0x4c002c32 || pc == 0x4c002c3a)) {
        uint32_t address = env->regs[pc == 0x4c002c32 ? 2 : 5];
        if (address >= DBNOR_BASE && (uint64_t)address < (uint64_t)DBNOR_BASE + DBNOR_SIZE) {
            DB2010Access a = {.address = address, .size = 1};
            db2010_nor_prepare(s->nor, &a);
        }
    }
}

void db2010_cpu_io(void *opaque, CPUARMState *env, uint32_t address,
                    unsigned size, bool write, uint64_t value, uintptr_t retaddr)
{
    DB2010MachineState *s = opaque;
    env->research_retaddr = retaddr;
    if (!retaddr) { CPU(s->cpu)->neg.can_do_io = true; }
    db2010_runtime_observe(s);
    if (!db2010_validate_bus(s, address, size, write, value)) {
        db2010_stop(s, write ? "write_unmapped" : "read_unmapped", address, size);
    }
    env->research_retaddr = 0;
}

static bool condition_passed(unsigned cond, uint32_t psr)
{
    bool n = psr >> 31, z = psr & (1u << 30), c = psr & (1u << 29);
    bool v = psr & (1u << 28);
    bool conditions[] = {z, !z, c, !c, n, !n, v, !v, c && !z,
                         !c || z, n == v, n != v, !z && n == v,
                         z || n != v, true, false};
    return conditions[cond];
}

void select_timer_irq(DB2010MachineState *s, CPUARMState *env, bool candidate)
{
    const char *fault = dbtimer_select(s->timer);
    if (fault) {
        db2010_stop(s, fault, env->regs[15], 4);
    }
    if (s->omit_diagnostics) {
        dbtimer_update_irq(s->timer);
        return;
    }
    QDict *event = qdict_new();
    qdict_put_int(event, "instruction_observations", s->count);
    qdict_put_int(event, "pc", env->regs[15]);
    qdict_put_int(event, "cpsr", cpsr_read(env));
    qdict_put_int(event, "irq_number", s->timer->state.irqs);
    qdict_put_bool(event, "rejected_candidate", candidate);
    qdict_put_int(event, "candidate_observation", candidate ? s->count + s->timer->state.irqs : 0);
    qdict_put_int(event, "virtual_ns", db2010_virtual_ns());
    qdict_put_int(event, "vector", env->cp15.sctlr_el[1] & 0x2000 ? 0xffff0018 : 0x18);
    if (s->i2c->config.complete_async_init) {
        qdict_put_str(event, "source", s->timer->state.rtc_selected ? "rtc" : s->timer->state.dsp_selected ? "dsp" : s->timer->state.pdi_selected ? "pdi" : s->timer->state.dma_selected ? "dma" : s->timer->state.i2c_selected ? "i2c" : "timer1");
    }
    qlist_append(s->irq_events, event);
    if (s->bounded_history && qlist_size(s->irq_events) > 512) {
        qobject_unref(qlist_pop(s->irq_events));
    }
    dbtimer_update_irq(s->timer);
}

void db2010_cpu_exception(void *opaque, CPUARMState *env,
                          int exception, uint32_t vector)
{
    DB2010MachineState *s = opaque;
    db2010_runtime_observe(s);
    if (exception == EXCP_IRQ && s->has_timer) {
        select_timer_irq(s, env, false);
    }
}

void db2010_cpu_wfi(void *opaque, CPUARMState *env, unsigned size)
{
    DB2010MachineState *s = opaque;
    s->wfi_waits++;
}

void db2010_cpu_service(DB2010MachineState *s, CPUARMState *env,
                        uint32_t pc, unsigned size)
{
    if (s->synthetic_etx && pc == 0xffff0ddc) { synthetic_etx(s, env, pc, size); }
    if (s->synthetic_kgen && (pc == 0xffff12a2 || pc == 0xffff12be ||
                              (s->kgen_final && pc == 0xffff12c2))) {
        synthetic_hash_api(s, env, pc, size, false);
    }
    if (s->synthetic_sha && (pc == 0xffff0f48 || pc == 0xffff112c || pc == 0xffff129e)) {
        synthetic_hash_api(s, env, pc, size, true);
    }
}

void db2010_cpu_control(DB2010MachineState *s, CPUARMState *env,
                        uint32_t pc, unsigned size, uint32_t insn)
{
    {
        bool executing = !env->thumb && condition_passed(insn >> 28, cpsr_read(env));
        bool control = executing && (insn & 0x0fff0fff) == 0x0e010f10;
        bool tlb = executing && (insn & 0x0fff0fff) == 0x0e080f17;
        unsigned rd = (insn >> 12) & 15;
        bool enabling = control && (rd == 15 || (env->regs[rd] & 1));
        bool mmu = env->cp15.sctlr_el[1] & 1;
        if (control && (rd == 15 || (env->regs[rd] & 0x80))) {
            db2010_stop(s, "cp15_big_endian_mapping_unmodeled", pc, size);
        }
        /* Native ARM926 applies FCSE before checking SCTLR.M. A nonzero PID
         * would redirect even MMU-off fetches beyond our identity contract. */
        if (executing && (insn & 0x0fff0fff) == 0x0e0d0f10 &&
            (rd == 15 || env->regs[rd] != 0)) {
            db2010_stop(s, "section_table_cpu_mapping_unmodeled", pc, size);
        }
        if (enabling || mmu) {
            if (!s->region_controller->has_section_table) {
                db2010_stop(s, "region_translation_table_unmodeled", pc, size);
            }
            if ((control && rd == 15) || env->cp15.ttbr0_ns != 0xfe000000 ||
                env->cp15.fcseidr_ns || env->cp15.fcseidr_s) {
                db2010_stop(s, "section_table_cpu_mapping_unmodeled", pc, size);
            }
            /* Prevent a changed native mapping from taking effect before the
             * next guard could run (the next fetch might already fault). */
            if (mmu && executing &&
                (((insn & 0x0fff0fff) == 0x0e020f10 &&
                  (rd == 15 || env->regs[rd] != 0xfe000000)) ||
                 ((insn & 0x0fff0fff) == 0x0e0d0f10 &&
                  (rd == 15 || env->regs[rd] != 0)))) {
                db2010_stop(s, "section_table_cpu_mapping_unmodeled", pc, size);
            }
            if (s->region_controller->table_dirty && ((!mmu && enabling) || (mmu && tlb))) {
                commit_section_table(s);
            }
        }
    }
}

void db2010_async_stop(DB2010MachineState *s, const char *reason,
                                  uint32_t address, unsigned size)
{
    db2010_runtime_observe(s);
    if (!s->stop) {
        s->stop = qdict_new();
        qdict_put_str(s->stop, "reason", reason);
        qdict_put_int(s->stop, "address", address);
        qdict_put_int(s->stop, "size", size);
        qdict_put_int(s->stop, "pc", s->cpu->env.regs[15]);
        qdict_put_int(s->stop, "observations", s->count);
        qdict_put_int(s->stop, "virtual_ns", db2010_virtual_ns());
        if (s->has_nand && !strcmp(reason, "nand_read_unknown_spare")) {
            qdict_put_int(s->stop, "page", s->nand->page);
            int column = s->nand->column - (s->nand->main_selected ? 512 : 0);
            qdict_put_int(s->stop, "column", column);
            qdict_put_int(s->stop, "absolute_column", 512 + column);
        }
        if (s->has_nand && !strcmp(reason, "nand_read_unknown_main")) {
            qdict_put_int(s->stop, "page", s->nand->page);
            qdict_put_int(s->stop, "column", s->nand->column);
            qdict_put_int(s->stop, "absolute_column", s->nand->column);
        }
    }
    {
        BQL_LOCK_GUARD();
        for (unsigned id = 0; id < DBDEV_COUNT; id++) {
            db2010_device_cancel_timer(s->devices[id]);
        }
        vm_stop(RUN_STATE_DEBUG);
    }
}
G_NORETURN void db2010_stop(DB2010MachineState *s, const char *reason,
                           uint32_t address, unsigned size)
{
    CPUState *cs = CPU(s->cpu);
    /* Return unexecuted TB budget before taking the virtual timestamp. */
    if (s->cpu->env.research_retaddr) {
        cpu_restore_state(cs, s->cpu->env.research_retaddr);
        s->cpu->env.research_retaddr = 0;
    }
    cs->neg.can_do_io = true;
    db2010_async_stop(s, reason, address, size);
    cs->exception_index = EXCP_INTERRUPT;
    cpu_loop_exit(cs);
}

/* Conditional model inherited from the reference harness, not measured silicon.
 * Build privately first: no partial table becomes visible on a rejected map. */
