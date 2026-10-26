#include "qemu/osdep.h"
#include "db2010-internal.h"

void synthetic_hash_api(DB2010MachineState *s, CPUARMState *env, uint32_t pc,
                               unsigned size, bool sha)
{
    bool init = pc == (sha ? 0xffff0f48 : 0xffff12a2);
    bool final = pc == (sha ? 0xffff129e : 0xffff12c2);
    uint32_t caller = sha ? (init ? 0x44d9f8ac : final ? 0x44d9f972 : 0x44d9f912) :
                           (init ? 0x44d9f9ce : final ? 0x44d9fa92 : 0x44d9fa32);
    uint32_t address = env->regs[final ? 1 : 0];
    uint32_t source = env->regs[1], length = final ? 0 : env->regs[2];
    uint32_t destination = env->regs[0];
    DB2010Region *output = final ? find_region(s, destination, 20) : NULL;
    DB2010Region *r = find_region(s, address, 92);
    DB2010Region *code = find_region(s, caller, 2);
    uint8_t context[92], digest[20];
    if (!env->thumb || size != 2 || env->regs[14] != (caller + 2) + 1 ||
        env->regs[init ? 1 : final ? 2 : 3] != pc + 1 || !code ||
        lduw_le_p((uint8_t *)memory_region_get_ram_ptr(code->mr) + caller - code->base) !=
            (init ? 0x4788 : final ? 0x4790 : 0x4798)) {
        db2010_stop(s, sha ? "synthetic_sha_caller_unmodeled" : "synthetic_kgen_caller_unmodeled", pc, size);
    }
    if (!r || r->readonly || address < 0x4c000000 ||
        (uint64_t)address + 92 > 0x4c800000) {
        db2010_stop(s, sha ? "synthetic_sha_context_unmodeled" : "synthetic_kgen_context_unmodeled", address, 92);
    }
    if (final && (!output || output->readonly || destination < 0x4c000000 ||
                  (uint64_t)destination + 20 > 0x4c800000 ||
                  ((uint64_t)destination < (uint64_t)address + 92 &&
                   (uint64_t)address < (uint64_t)destination + 20))) {
        db2010_stop(s, sha ? "synthetic_sha_output_unmodeled" : "synthetic_kgen_output_unmodeled", destination, 20);
    }
    if (init) {
        dbk_init(context);
    } else {
        DB2010Region *input = length ? find_region(s, source, length) : NULL;
        for (unsigned i = 0; i < 92; i++) {
            if (s->research_guards && !r->known[address - r->base + i]) {
                db2010_stop(s, sha ? "synthetic_sha_unknown_context" : "synthetic_kgen_unknown_context", address + i, 92);
            }
        }
        if (length && (!input ||
            !((source >= 0x44020000 && (uint64_t)source + length <= 0x46000000) ||
              (source >= 0x4c000000 && (uint64_t)source + length <= 0x4c800000)))) {
            db2010_stop(s, sha ? "synthetic_sha_input_unmodeled" : "synthetic_kgen_input_unmodeled", source, length);
        }
        for (uint32_t i = 0; i < length; i++) {
            if ((s->research_guards || input->readonly) && !input->known[source - input->base + i]) {
                db2010_stop(s, sha ? "synthetic_sha_unknown_input" : "synthetic_kgen_unknown_input", source + i, length);
            }
            if (s->has_nor && db2010_nor_overlay_view(s->nor, source + i, 1)) {
                db2010_stop(s, sha ? "synthetic_sha_overlay_source_unmodeled" : "synthetic_kgen_overlay_source_unmodeled", source+i, length);
            }
            if (s->has_nor && source < 0x46000000 &&
                s->nor->part[(source + i - 0x44000000) / 0x200000].mode != DBNOR_ARRAY) {
                db2010_stop(s, sha ? "synthetic_sha_nor_not_array" : "synthetic_kgen_nor_not_array", source + i, length);
            }
        }
        memcpy(context, (uint8_t *)memory_region_get_ram_ptr(r->mr) + address - r->base, 92);
        if (final ? !dbk_final(context, digest) : !dbk_update(context, length ?
                (uint8_t *)memory_region_get_ram_ptr(input->mr) + source - input->base : NULL, length)) {
            db2010_stop(s, sha ? "synthetic_sha_context_format_unmodeled" : "synthetic_kgen_context_format_unmodeled", address, 92);
        }
    }
    memcpy((uint8_t *)memory_region_get_ram_ptr(r->mr) + address - r->base, context, 92);
    memset(r->known + address - r->base, 1, 92);
    if (final) {
        memcpy((uint8_t *)memory_region_get_ram_ptr(output->mr) + destination - output->base, digest, 20);
        memset(output->known + destination - output->base, 1, 20);
    }
    if (sha) {
        s->sha_last_context = address;
        if (init) { s->sha_inits++; }
        else if (final) { s->sha_finals++; }
        else { s->sha_updates++; s->sha_bytes += length; }
    } else {
        s->kgen_last_context = address;
        if (init) { s->kgen_inits++; }
        else if (final) { s->kgen_finals++; }
        else { s->kgen_updates++; s->kgen_bytes += length; }
    }
    /* Preserve flags and all registers except PC, returning to native LR.
     * ROM instruction/time cost is deliberately unspecified (zero modeled).
     * This synthetic call is not counted as an executed ROM instruction. */
    env->regs[15] = env->regs[14] & ~1u;
    cpu_loop_exit_noexc(CPU(s->cpu));
}

/* User-authorized synthetic completion for one allowlisted GDFS request.
 * This is NOT recovered ROM code or cryptographic verification. */
void synthetic_etx(DB2010MachineState *s, CPUARMState *env, uint32_t pc,
                          unsigned size)
{
    uint32_t address = env->regs[0];
    DB2010Region *code = find_region(s, 0x4490d1ae, 2);
    DB2010Region *request = find_region(s, address, 148);
    DB2010Register *status = find_register(s, 0xf9030004);
    if (!env->thumb || size != 2 || env->regs[14] != 0x4490d1b1 ||
        env->regs[4] != 0xffff0ddd || !code ||
        lduw_le_p((uint8_t *)memory_region_get_ram_ptr(code->mr) + 0x4490d1ae - code->base) != 0x47a0) {
        db2010_stop(s, "synthetic_etx_caller_unmodeled", pc, size);
    }
    if (!request || request->readonly || address < 0x4c000000 ||
        (uint64_t)address + 148 > 0x4c800000) {
        db2010_stop(s, "synthetic_etx_request_unmodeled", address, 148);
    }
    for (unsigned i = 0; i < sizeof(s->etx_request); i++) {
        if (s->research_guards && !request->known[address - request->base + i]) {
            db2010_stop(s, "synthetic_etx_unknown_request", address + i, 82);
        }
    }
    if (memcmp((uint8_t *)memory_region_get_ram_ptr(request->mr) + address - request->base,
               s->etx_request, sizeof(s->etx_request))) {
        db2010_stop(s, "synthetic_etx_payload_unmodeled", address, 82);
    }
    if (!status || status->width != 4 || !dbreg_known(status) || status->read_mask ||
        status->value > 1) {
        db2010_stop(s, "synthetic_etx_status_unmodeled", 0xf9030004, 4);
    }
    status->value = 1;
    s->etx_calls++; s->etx_last_request = address;
    /* Native BLX already established LR. Return0 is ignored by this caller;
     * only the declared peripheral status effect enables its success branch. */
    env->regs[0] = 0; env->regs[15] = env->regs[14] & ~1u;
    cpu_loop_exit_noexc(CPU(s->cpu));
}
