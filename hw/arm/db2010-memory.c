#include "qemu/osdep.h"
#include "db2010-internal.h"

DB2010Region *find_region(DB2010MachineState *s, uint32_t addr,
                                unsigned size)
{
    unsigned i;
    for (i = 0; i < s->regions->len; i++) {
        DB2010Region *r = g_ptr_array_index(s->regions, i);
        if (r->active && addr >= r->base &&
            (uint64_t)addr + size <= (uint64_t)r->base + r->size) {
            return r;
        }
    }
    return NULL;
}

bool executable(DB2010MachineState *s, uint32_t addr, unsigned size)
{
    DB2010Region *r = find_region(s, addr, size);
    unsigned i;
    if (r && r->itcm) {
        return true;
    }
    for (i = 0; i < s->execution->len; i++) {
        DB2010Interval *range = &g_array_index(s->execution, DB2010Interval, i);
        if (addr >= range->start && (uint64_t)addr + size <= range->end) {
            return true;
        }
    }
    return false;
}

const char *generate_section_table(DB2010MachineState *s, uint32_t *table,
                                          unsigned *fault_slot)
{
    bool occupied[4096] = { false };
    unsigned slot, section;
    memset(table, 0, 16384);
    for (slot = 0; slot < 12; slot++) {
        uint32_t range = s->region_controller->region_words[slot];
        uint32_t attr = s->region_controller->region_words[slot + 12];
        unsigned start = range & 0xfff, end = (range >> 16) & 0xfff;
        *fault_slot = slot;
        if (!s->region_controller->region_known[slot]) {
            return "section_table_unknown_range";
        }
        if (!(range & 0x80000000)) {
            continue;
        }
        if (!s->region_controller->region_known[slot + 12]) {
            return "section_table_unknown_attribute";
        }
        if (range & 0x7000f000 || attr & ~0xdff || (attr & 3) != 2 || !(attr & 0x10)) {
            return "section_table_encoding_unmodeled";
        }
        if (end < start) {
            return "section_table_reversed_range";
        }
        for (section = start; section <= end; section++) {
            if (occupied[section]) {
                return "section_table_overlap_unmodeled";
            }
            occupied[section] = true;
            table[section] = cpu_to_le32((section << 20) | attr);
        }
    }
    return NULL;
}

void commit_section_table(DB2010MachineState *s)
{
    uint32_t table[4096];
    unsigned slot;
    const char *fault = generate_section_table(s, table, &slot);
    if (fault) {
        db2010_stop(s, fault, 0xfe004000 + 4 * slot, 4);
    }
    memcpy(memory_region_get_ram_ptr(s->section_table->mr), table, sizeof(table));
    db2010_region_table_committed(s->region_controller);
}

DB2010Region *add_region(DB2010MachineState *s, const char *name,
                                uint32_t base, uint32_t size, bool readonly)
{
    DB2010Region *r;
    unsigned i;
    if (!size || base % 4096 || size % 4096 ||
        size > 0x2000000 || (uint64_t)base + size > (1ULL << 32)) {
        error_report("db2010: unsupported prepared region bounds");
        exit(EXIT_FAILURE);
    }
    for (i = 0; i < s->regions->len; i++) {
        DB2010Region *other = g_ptr_array_index(s->regions, i);
        if (base < (uint64_t)other->base + other->size &&
            other->base < (uint64_t)base + size) {
            error_report("db2010: overlapping prepared regions");
            exit(EXIT_FAILURE);
        }
    }
    r = g_new0(DB2010Region, 1);
    r->mr = &r->storage;
    r->base = base;
    r->size = size;
    r->readonly = readonly;
    r->active = true;
    r->known = g_malloc0(size);
    if (s->has_nor && base == 0x44000000) {
        if (size != 0x2000000 || !readonly) {
            error_report("db2010: NOR requires a 32 MiB read-only prepared array");
            exit(EXIT_FAILURE);
        }
        r->mr = db2010_device_nor_region(s->devices[DBDEV_NOR]);
        db2010_nor_init(s->nor, r->mr, r->known, name, OBJECT(s->devices[DBDEV_NOR]));
    } else {
        memory_region_init_ram(r->mr, NULL, name, size, &error_fatal);
    }
    if (!(s->has_nor && base == 0x44000000)) {
        memory_region_add_subregion(get_system_memory(), base, r->mr);
    }
    g_ptr_array_add(s->regions, r);
    return r;
}

uint64_t tcm_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    DB2010MachineState *s = env->board_hooks->opaque;
    DB2010Tcm *tcm = &s->tcm[ri->opc2];
    return tcm->base | 7 << 2 | tcm->enabled;
}

void tcm_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    DB2010MachineState *s = env->board_hooks->opaque;
    DB2010Tcm *tcm = &s->tcm[ri->opc2];
    uint32_t base = value & 0xfffff000;
    bool enabled = value & 1;
    if (value & 0xfc2 || base % 0x10000) {
        db2010_stop(s, "cp15_tcm_encoding_unmodeled", base, 4);
    }
    if (tcm->enabled && (!enabled || base != tcm->base)) {
        db2010_stop(s, "cp15_tcm_mapping_change_unmodeled", base, 4);
    }
    if (enabled && !tcm->enabled) {
        unsigned i;
        if (!tcm->bank && s->cold_zero_reset_record && base != 0x42000000) {
            db2010_stop(s, "dtcm_reset_record_mapping_unmodeled", base, 4);
        }
        if (env->cp15.sctlr_el[1] & 1) {
            db2010_stop(s, "cp15_tcm_mmu_mapping_unmodeled", base, 4);
        }
        for (i = 0; i < s->regions->len; i++) {
            DB2010Region *r = g_ptr_array_index(s->regions, i);
            if (r == tcm->region) {
                continue;
            }
            if (base < (uint64_t)r->base + r->size && r->base < (uint64_t)base + 0x10000) {
                db2010_stop(s, "cp15_tcm_overlay_unmodeled", base, 0x10000);
            }
        }
        if (tcm->region && tcm->region->base != base) {
            db2010_stop(s, "cp15_tcm_mapping_change_unmodeled", base, 4);
        }
        {
            BQL_LOCK_GUARD();
            if (!tcm->region) {
                tcm->region = add_region(s, tcm->bank ? "db2010.itcm" : "db2010.dtcm",
                                        base, 0x10000, false);
                tcm->region->itcm = tcm->bank;
            } else {
                memory_region_set_enabled(tcm->region->mr, true);
                tcm->region->active = true;
            }
            if (!tcm->bank && s->cold_zero_reset_record) {
                /* Direct-MAIN cold input, not measured SRAM reset contents. */
                memset((uint8_t *)memory_region_get_ram_ptr(tcm->region->mr) + 0x604, 0, 28);
                memset(tcm->region->known + 0x604, 1, 28);
            }
        }
    }
    tcm->base = base;
    tcm->enabled = enabled;
}

uint64_t untranslated_read(void *opaque, hwaddr address, unsigned size)
{
    DB2010MachineState *s = opaque;
    /* Translation must decode a placeholder before emitting an allowlisted
     * synthetic service hook. The hook exits before executing this halfword.
     * Data requests still fail in the architectural MMIO bridge before here. */
    bool service = size == 2 && address >= 0xffff0000 &&
        db2010_cpu_instruction_needed(s, address, 0, true);
    if (!s->research_guards && !service) {
        db2010_stop(s, "read_unmapped", address, size);
    }
    return 0;
}

void untranslated_write(void *opaque, hwaddr address, uint64_t value,
                                unsigned size)
{
    db2010_stop(opaque, "write_unmapped", address, size);
}

const MemoryRegionOps untranslated_ops = {
    .read = untranslated_read, .write = untranslated_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};
