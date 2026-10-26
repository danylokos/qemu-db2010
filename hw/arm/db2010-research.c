#include "qemu/osdep.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "db2010-internal.h"

void guard_memory(void *opaque, CPUARMState *env, uint32_t addr,
                          unsigned size, unsigned operation, uint32_t value)
{
    DB2010MachineState *s = opaque;
    DB2010Region *r = find_region(s, addr, size);
    unsigned i;
    if (s->stop) {
        db2010_stop(s, "already_stopped", addr, size);
    }
    if (db2010_validate_bus(s, addr, size, operation, value)) { return; }
    if (r && r == s->section_table && !operation) {
        if (s->region_controller->table_dirty) {
            db2010_stop(s, "section_table_uncommitted_read_unmodeled", addr, size);
        }
        if (size != 4 || addr % 4) {
            db2010_stop(s, "section_table_read_width_unmodeled", addr, size);
        }
    }
    if (!r) {
        db2010_stop(s, operation ? "write_unmapped" : "read_unmapped", addr, size);
    }
    /* Native ARM LDRD/STRD reach this hook as a single eight-byte access.
     * Validate the complete RAM range here; the CPU owns alignment/faults,
     * and only the post-success hook marks bytes established.
     * Device accesses have already gone through their own bus guards.
     */
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        db2010_stop(s, "memory_access_width_unmodeled", addr, size);
    }
    if (operation == 0) {
        for (i = 0; i < size; i++) {
            if (!r->known[addr - r->base + i]) {
                db2010_stop(s, "read_unknown_ram", addr + i, size);
            }
        }

    } else if (operation == 1) {
        if (r->readonly) {
            db2010_stop(s, "write_readonly_memory", addr, size);
        }
    } else {
        /* This callback runs only after the native store succeeds. */
        memset(r->known + addr - r->base, 1, size);
    }
}

void guard_exception(void *opaque, CPUARMState *env, int exception, uint32_t vector)
{
    DB2010MachineState *s = opaque;
    DB2010Region *r = find_region(s, vector, 4);
    if (!r || !executable(s, vector, 4)) {
        db2010_stop(s, "exception_vector_unmodeled", vector, 4);
    }
    for (unsigned i = 0; i < 4; i++) {
        unsigned off = vector - r->base + i;
        if (r->known[off] != 1) {
            db2010_stop(s, "exception_vector_unknown", vector + i, 4);
        }
    }
    if ((env->cp15.sctlr_el[1] & 1) && !r->itcm) {
        if (!s->region_controller->has_section_table || env->cp15.ttbr0_ns != 0xfe000000 ||
            env->cp15.fcseidr_ns || env->cp15.fcseidr_s) {
            db2010_stop(s, "exception_vector_translation_unmodeled", vector, 4);
        }
        uint32_t *words = memory_region_get_ram_ptr(s->section_table->mr);
        uint32_t descriptor = le32_to_cpu(words[vector >> 20]);
        unsigned domain = (env->cp15.dacr_ns >> (2 * ((descriptor >> 5) & 15))) & 3;
        bool inaccessible = domain == 0 || domain == 2 ||
            (domain == 1 && !((descriptor >> 10) & 3) && !(env->cp15.sctlr_el[1] & 0x300));
        if ((descriptor & 3) != 2 || inaccessible) {
            db2010_stop(s, "exception_vector_translation_unmodeled", vector, 4);
        }
    }
}

void timer_wfi(void *opaque, CPUARMState *env, unsigned size)
{
    DB2010MachineState *s = opaque;
    if (!dbirq_any_routed(s->timer) && dbtimer_deadline(s->timer) < 0 &&
        !(s->has_rtc && dbrtc_deadline(s->rtc) >= 0) &&
        !(s->uart_port->data_enabled && s->uart_port->data_mode_known &&
          (s->uart_port->data_mode & 7) != 7 && (s->uart_port->data_mask & 0x61)) &&
        !s->has_keypad && !s->i2c->state.pending &&
        !(s->display->pdi.state.dma_armed && !s->display->display_completed) &&
        !(s->fa->has_dsp_peer && dbdsp_deadline(s->fa->dsp_peer) >= 0)) {
        db2010_stop(s, "wfi_without_modeled_wake", env->regs[15] - size, size);
    }
    /* The native ARM helper performs the halt and honors the physical pin even
     * with CPSR.I set. Legitimate external-input waits may have no timer. */

}

/* Synthetic ROM API substitution explicitly requested for bring-up. Original
 * BLX executes normally; no ROM bytes are supplied or executed. Only the selected
 * 8040 entries and their original MAIN return sites are accepted.
 * Latest state lives in guest RAM; there is no growing transaction history. */
void guard_instruction(void *opaque, CPUARMState *env, uint32_t pc,
                               unsigned size, uint32_t insn)
{
    DB2010MachineState *s = opaque;
    DB2010Region *r = find_region(s, pc, size);
    s->instruction_pc = pc;
    unsigned i;
    if (s->stop) {
        db2010_stop(s, "already_stopped", pc, size);
    }
    if (s->display->display_first_burst_done && !s->dma->config.display_frame) {
        db2010_stop(s, "display_dma_next_burst_unmodeled", 0xf7000308, 16);
    }
    if (s->instruction_limited && s->count >= s->limit) {
        db2010_stop(s, "instruction_limit", pc, size);
    }
    db2010_cpu_service(s, env, pc, size);
    if (!r || !executable(s, pc, size)) {
        db2010_stop(s, "fetch_outside_executable_bytes", pc, size);
    }
    if (s->has_nor && pc >= 0x44000000 && pc < 0x46000000) {
        for (i = 0; i < size; i++) {
            if (s->nor->part[(pc + i - 0x44000000) / 0x200000].mode != DBNOR_ARRAY) {
                db2010_stop(s, "nor_fetch_not_array", pc, size);
            }
        }
    }
    if (r == s->tcm[0].region) {
        db2010_stop(s, "dtcm_instruction_backing_unmodeled", pc, size);
    }
    for (i = 0; i < size; i++) {
        unsigned off = pc - r->base + i;
        if (!r->known[off]) {
            db2010_stop(s, "fetch_unknown_bytes", pc + i, size);
        }
        if (r->known[off] == 2) {
            db2010_stop(s, "fetch_assumed_ram_bytes", pc + i, size);
        }
    }
    {
        uint32_t raw = env->thumb && size == 4 ? (insn << 16) | (insn >> 16) : insn;
        uint8_t *bytes = memory_region_get_ram_ptr(r->mr);
        for (i = 0; i < size; i++) {
            if (bytes[pc - r->base + i] != ((raw >> (8 * i)) & 255)) {
                db2010_stop(s, "fetch_backing_mismatch", pc + i, size);
            }
        }
        for (unsigned p = 0; p < s->execution_proofs->len; p++) {
            DB2010ExecutionProof *proof = g_ptr_array_index(s->execution_proofs, p);
            if ((uint64_t)pc < (uint64_t)proof->base + proof->size &&
                (uint64_t)pc + size > proof->base) {
                if (pc < proof->base || (uint64_t)pc + size > (uint64_t)proof->base + proof->size) {
                    db2010_stop(s, "fetch_execution_proof_boundary", pc, size);
                }
                if (memcmp(bytes + pc - r->base, proof->bytes + pc - proof->base, size)) {
                    db2010_stop(s, "fetch_original_copy_mismatch", pc, size);
                }
            }
        }
    }
    db2010_cpu_control(s, env, pc, size, insn);
    s->count++;
}
