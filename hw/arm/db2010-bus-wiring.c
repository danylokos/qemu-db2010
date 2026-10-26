/* Board compatibility adapter for component contracts. SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "db2010-internal.h"

static void add_owner(DB2010MachineState *s, const char *name,
                      const DB2010RegMap *map,
                      DB2010AccessFn access, void *opaque, unsigned priority)
{
    const DB2010RegisterSpec *claims = map->claims;
    const char *fault = db2010_bus_add_map(&s->bus, name, map, access, opaque, priority);
    if (fault) { error_report("db2010: %s: %s", name, fault); exit(EXIT_FAILURE); }
    /* Historically page contracts route by starting page, including their
     * guarded holes; intersecting claims use explicit priority otherwise. */
    if (claims[0].span >= 4096 && strcmp(name, "absent-bank")) {
        db2010_bus_owner(&s->bus, claims[0].address, 1)->start_within = true;
    }
}

static bool memory_view(void *opaque, uint32_t address, unsigned size, DB2010MemoryView *view)
{
    DB2010MachineState *s = opaque;
    DB2010Region *r = find_region(s, address, size);
    if (!r) { return false; }
    *view = (DB2010MemoryView){r->base, memory_region_get_ram_ptr(r->mr), r->known,
                            !s->research_guards && !r->readonly};
    return true;
}
static bool memory_overlay(void *opaque, uint32_t address, unsigned size)
{
    DB2010MachineState *s = opaque;
    return s->has_nor && db2010_nor_overlay_view(s->nor, address, size);
}
static bool memory_dma_write(void *opaque, uint32_t address, const uint8_t *data, unsigned size)
{
    DB2010MachineState *s = opaque;
    DB2010Region *r = find_region(s, address, size);
    if (!r || r->readonly || memory_overlay(s, address, size)) { return false; }
    if (data) {
        if (address_space_write(&address_space_memory, address, MEMTXATTRS_UNSPECIFIED,
                                data, size) != MEMTX_OK) { return false; }
        memset(r->known + address - r->base, 1, size);
    }
    return true;
}
static void model_fault(void *opaque, const char *reason, uint32_t address, unsigned size)
{
    db2010_async_stop(opaque, reason, address, size);
}
static void connect_components(DB2010MachineState *s)
{
    DB2010MemoryPort memory = {s, memory_view, memory_overlay, memory_dma_write};
    DB2010TimePort time = {&s->count, &s->timer->state.idle_cycles, &s->instruction_pc};
    DB2010FaultPort faults = {s, model_fault};
    s->display->pdi.config.repeat_frames = s->dma->config.display_stream;
    s->timer->config.unlimited_irqs = s->bounded_history;
    s->timer->config.i2c_route = s->i2c->config.complete_async_init;
    s->timer->config.display_route = s->dma->config.display_frame;
    s->timer->config.dsp_route = s->fa->has_dsp_peer;
    if (s->has_timer) {
        for (unsigned bank = 0; bank < ((s->i2c->config.complete_async_init || s->has_keypad) ? 3 : 2); bank++) {
            DB2010Register *mask = find_register(s, 0xfb020104 + bank * 0x100);
            if (mask) {
                s->timer->config.initial_masks_known[bank] = mask->initial_known;
                s->timer->config.initial_masks[bank] = mask->initial;
            }
        }
    }
    s->timer->config.keypad_route = s->has_keypad;
    s->timer->config.simif_route = s->has_simif;
    s->timer->config.rtc_route = s->has_rtc && s->rtc->config.minute_irq;
    s->keypad->count = &s->count;
    s->bank->timer = s->has_timer ? s->timer : NULL;
    s->display->dma = s->dma;
    s->display->has_dsp_peer = s->fa->has_dsp_peer;
    s->display->stream = find_register(s, 0xf7000304);
    s->display->memory = memory; s->display->time = time; s->display->faults = faults;
    s->fa->dma = s->dma;
    s->fa->mode = find_register(s, 0xfa040014);
    s->fa->memory = memory; s->fa->time = time; s->fa->faults = faults;
    s->dma_link = (DB2010DmaLink){s->dma, s->display, s->fa};
    s->timer_link = (DB2010TimerLink){s->timer, s->i2c, &s->dma->config.display_frame};
}

void db2010_register_owners(DB2010MachineState *s)
{
    connect_components(s);
    db2010_bus_init(&s->bus);
    if (s->has_f500_port) {
        const DB2010RegMap *map = dbfp_map(&dbfp_primary);
        add_owner(s, "f500-primary", map, dbfp_bus_access, s->f500_port, 4);
    }
    if (s->has_f500_aux_port) {
        const DB2010RegMap *map = dbfp_map(&dbfp_auxiliary);
        add_owner(s, "f500-auxiliary", map, dbfp_bus_access, s->f500_aux_port, 5);
    }
    if (s->has_f500_control) {
        DB2010Register *r = find_register(s, dbfc_map()->claims[0].address);
        if (!r) { error_report("db2010: F500 control latch missing"); exit(EXIT_FAILURE); }
        s->f500_control_link = (DB2010F500ControlLink){s->f500_control, &r->value, &r->known_mask};
        const DB2010RegMap *map = dbfc_map();
        add_owner(s, "f500-control", map, dbfc_bus_access, &s->f500_control_link, 16);
    }
    if (s->has_fa_transfer) {
        const DB2010RegMap *map = dbfa_map();
        add_owner(s, "fa-transfer", map, dbfa_bus_access, s->fa_transfer, 6);
    }
    if (s->fa->has_dsp_peer) {
        const DB2010RegMap *map = dbdsp_map();
        add_owner(s, "dsp-peer", map, db2010_peer_access, s->fa, 7);
        if (s->fa->dsp_peer->config.modexp) {
            add_owner(s, "dsp-peer-rx", dbdsp_rx_map(), db2010_peer_rx_access, s->fa, 7);
        }
    }
    if (s->fa->has_fa_mailbox) {
        const DB2010RegMap *map = dbfm_map();
        add_owner(s, "fa-mailbox", map,
                  db2010_mailbox_access, s->fa, 8);
    }
    if (s->has_dma) {
        const DB2010RegMap *map = dbdma_map();
        add_owner(s, "dma", map, db2010_dma_access, &s->dma_link, 11);
    }
    if (s->display->has_pdi) {
        const DB2010RegMap *map = dbpdi_map();
        add_owner(s, "pdi", map, db2010_display_access, s->display, 8);
    }
    if (s->has_i2c) {
        const DB2010RegMap *map = dbi_map();
        add_owner(s, "i2c", map, dbi_bus_access, s->i2c, 9);
    }
    if (s->has_simif) {
        add_owner(s, "simif", dbsim_map(), dbsim_access, s->simif, 10);
    }
    if (s->has_keypad) {
        add_owner(s, "keypad", dbkey_map(), dbkey_access, s->keypad, 10);
    }
    if (s->has_nand) {
        const DB2010RegMap *map = dbn_map();
        add_owner(s, "nand", map, dbn_bus_access, s->nand, 10);
    }
    if (s->has_timer) {
        const DB2010RegMap *map = dbtimer_map();
        add_owner(s, "timer-irq", map, db2010_timer_access, &s->timer_link, 0);
    }
    if (s->has_controller) {
        const DB2010RegMap *map = db2010_controller_map();
        add_owner(s, "memory-controller", map, db2010_controller_access, s->memory_controller, 20);
    }
    if (s->has_region_controls) {
        const DB2010RegMap *map = db2010_region_map();
        add_owner(s, "region-controls", map, db2010_region_access, s->region_controller, 19);
    }
    if (s->has_absent_bank) {
        const DB2010RegMap *map = db2010_absent_map();
        add_owner(s, "absent-bank", map, db2010_absent_access, s->absent, 2);
    }
    if (s->has_nor) {
        const DB2010RegMap *map = db2010_nor_map();
        add_owner(s, "nor", map, db2010_nor_bus_access, s->nor, 3);
        db2010_bus_owner(&s->bus, map->claims[0].address, 1)->prepare_native = db2010_nor_prepare;
    }
    if (s->has_uart) {
        const DB2010RegMap *map = db2010_uart_map();
        add_owner(s, "uart", map, db2010_uart_access, s->uart_port, 1);
        if (s->uart_port->bluetooth_enabled) {
            add_owner(s, "bluetooth-uart", db2010_bluetooth_uart_map(), db2010_uart_access, s->uart_port, 1);
        }
        if (s->uart_port->data_enabled) {
            add_owner(s, "data-uart", db2010_data_uart_map(), db2010_uart_access, s->uart_port, 1);
        }
    }
    if (s->has_rtc) {
        const DB2010RegMap *map = dbrtc_map();
        add_owner(s, "rtc", map, db2010_rtc_access, s->rtc, 12);
        if (s->rtc->config.minute_irq) {
            add_owner(s, "rtc-minute", dbrtc_irq_map(), db2010_rtc_access, s->rtc, 12);
        }
    }
    if (s->has_rtc_retained) {
        const DB2010RegMap *map = dbrtr_map();
        add_owner(s, "rtc-retained", map, db2010_retained_access, s->rtc_retained, 14);
    }
    if (s->has_startup_status) {
        const DB2010RegMap *map = db2010_startup_map();
        add_owner(s, "startup-status", map, db2010_startup_access, s->startup, 13);
    }
    if (s->has_power_hold) {
        const DB2010RegMap *map = db2010_power_map();
        add_owner(s, "power-hold", map, db2010_power_access, s->power, 15);
    }
    for (unsigned i = 0; i < s->bank->registers->len; i++) {
        DB2010Register *r = g_ptr_array_index(s->bank->registers, i);
        if (s->has_f500_control && r->address == dbfc_map()->claims[0].address) { continue; }
        const DB2010RegMap *map = dbreg_map(r);
        if (dbsys_register(r->address)) {
            add_owner(s, r->name, map, dbsys_access, s->syscon, 100);
        } else {
            add_owner(s, r->name, map, db2010_bank_access, s->bank, 100);
        }
    }

}

static void access_owner(DB2010MachineState *s, DB2010Owner *owner,
                         uint32_t address, unsigned size, bool write,
                         uint32_t *value, bool commit)
{
    DB2010Access a = {.address = address, .size = size, .value = *value,
        .fault_address = address, .fault_size = size, .pc = s->instruction_pc, .now = db2010_virtual_ns(),
        .idle = s->has_timer ? s->timer->state.idle_cycles : 0,
        .write = write, .commit = commit};
    const char *fault = owner ? owner->access(owner->opaque, &a) : db2010_bank_access(s->bank, &a);
    if (fault) {
        if (!commit) { db2010_stop(s, fault, a.fault_address, a.fault_size); }
        /* The CPU guard validates the architectural request. QEMU may split
         * an authorized unaligned halfword into byte callbacks (RTC retained
         * fields are indexed by byte). Preserve the old callbacks' rejected
         * fragment behavior: zero read/no write effect. Direct guest byte
         * requests still fail in preview. This is compatibility, not a new
         * byte-wide peripheral contract. */
        *value = 0;
        return;
    }
    *value = a.value;
}

bool db2010_dispatch(DB2010MachineState *s, uint32_t address, unsigned size,
                      bool write, uint32_t *value, bool commit)
{
    DB2010Owner *owner = db2010_bus_owner(&s->bus, address, size);
    if (!owner && !find_mmio_page(s, address)) { return false; }
    access_owner(s, owner, address, size, write, value, commit);
    return true;
}

bool db2010_validate_bus(DB2010MachineState *s, uint32_t address, unsigned size,
                          unsigned operation, uint32_t value)
{
    DB2010Owner *owner = db2010_bus_owner(&s->bus, address, size);
    if (!owner && !find_mmio_page(s, address)) { return false; }
    if (operation != 2) {
        access_owner(s, owner, address, size, operation == 1, &value, false);
        if (!operation && owner && owner->prepare_native) {
            DB2010Access a = {.address = address, .size = size};
            owner->prepare_native(owner->opaque, &a);
        }
    }
    return true;
}
